#include "ScreenshotStore.h"
#include "RequestTracker.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <new>
#include <stdexcept>
#include <stdio.h>

namespace {
std::uint64_t readInteger(std::string_view bytes) noexcept {
    std::uint64_t value = 0;

    for (unsigned char byte : bytes) {
        value = (value << 8) | byte;
    }

    return value;
}

std::string_view signature(protocol::ImageFormat format) noexcept {
    return format == protocol::ImageFormat::Png ? std::string_view("\x89PNG\r\n\x1a\n", 8) : "BM";
}
}

ScreenshotStore::Transfer::~Transfer() {
    if (temporaryExists) {
        unlink(temporary.c_str());
    }
}

std::filesystem::path ScreenshotStore::destination(std::uint64_t id, protocol::ImageFormat format) const {
    if (format != protocol::ImageFormat::Png && format != protocol::ImageFormat::Bmp) {
        throw std::invalid_argument("Unknown image format");
    }

    return directory_ /
           ("capture-" + std::to_string(id) + (format == protocol::ImageFormat::Png ? ".png" : ".bmp"));
}

std::error_code ScreenshotStore::removeTemporary(Transfer& transfer) noexcept {
    transfer.file = UniqueFd{};

    if (transfer.temporaryExists) {
        if (unlink(transfer.temporary.c_str()) != 0 && errno != ENOENT) {
            return {errno, std::generic_category()};
        }

        transfer.temporaryExists = false;
    }

    return {};
}

std::error_code ScreenshotStore::fail(std::uint64_t id, std::error_code error) noexcept {
    const auto found = transfers_.find(id);

    if (found == transfers_.end()) {
        return error;
    }

    auto& transfer = found->second;

    if (!transfer.failure) {
        transfer.failure = error;
    }

    removeTemporary(transfer);
    return transfer.failure;
}

std::error_code ScreenshotStore::begin(const DiskJob& job) {
    if (job.transferId == 0 || job.transferId <= lastBegin_) {
        return fail(job.transferId, std::make_error_code(std::errc::invalid_argument));
    }

    lastBegin_ = job.transferId;

    if (transfers_.size() == maxTransfers) {
        return std::make_error_code(std::errc::no_buffer_space);
    }

    auto& transfer = transfers_.try_emplace(job.transferId).first->second;

    if (job.data.size() != 12) {
        return fail(job.transferId, std::make_error_code(std::errc::invalid_argument));
    }

    transfer.expected = readInteger(job.data.substr(0, 8));
    const auto format = readInteger(job.data.substr(8, 4));

    if ((format != 1 && format != 2) || transfer.expected == 0 ||
        transfer.expected > protocol::RequestTracker::maxImageBytes) {
        return fail(job.transferId, std::make_error_code(std::errc::invalid_argument));
    }

    transfer.format = static_cast<protocol::ImageFormat>(format);

    if (transfer.expected < signature(transfer.format).size()) {
        return fail(job.transferId, std::make_error_code(std::errc::invalid_argument));
    }

    std::error_code error;
    std::filesystem::create_directories(directory_, error);

    if (error) {
        return fail(job.transferId, error);
    }

    transfer.finalPath = destination(job.transferId, transfer.format).string();
    transfer.temporary =
        (directory_ / ("capture-" + std::to_string(job.transferId) + "-XXXXXX.part")).string();
    transfer.file = UniqueFd(mkostemps(transfer.temporary.data(), 5, O_CLOEXEC));

    if (transfer.file.get() < 0) {
        return fail(job.transferId, {errno, std::generic_category()});
    }

    transfer.temporaryExists = true;
    return {};
}

std::error_code ScreenshotStore::operator()(const DiskJob& job) {
    try {
        if (job.operation == DiskOperation::Begin) {
            return begin(job);
        }

        const auto found = transfers_.find(job.transferId);

        if (job.operation == DiskOperation::Cancel) {
            if (!job.data.empty()) {
                return fail(job.transferId, std::make_error_code(std::errc::invalid_argument));
            }

            if (found == transfers_.end()) {
                return {};
            }

            if (const auto error = removeTemporary(found->second)) {
                if (!found->second.failure) {
                    found->second.failure = error;
                }

                return error;
            }

            transfers_.erase(found);
            return {};
        }

        if (found == transfers_.end()) {
            return std::make_error_code(std::errc::no_such_file_or_directory);
        }

        auto& transfer = found->second;

        if (transfer.failure) {
            return transfer.failure;
        }

        if (job.operation == DiskOperation::Write) {
            if (job.data.empty() || job.data.size() > DiskWorker::maxJobBytes ||
                job.data.size() > transfer.expected - transfer.received) {
                return fail(job.transferId, std::make_error_code(std::errc::invalid_argument));
            }

            const auto count = std::min(job.data.size(), transfer.prefix.size() - transfer.prefixSize);
            std::copy_n(job.data.data(), count, transfer.prefix.data() + transfer.prefixSize);
            transfer.prefixSize += count;
            const auto expected = signature(transfer.format);

            if (transfer.prefixSize >= expected.size() &&
                std::string_view(transfer.prefix.data(), expected.size()) != expected) {
                return fail(job.transferId, std::make_error_code(std::errc::illegal_byte_sequence));
            }

            auto remaining = job.data;

            while (!remaining.empty()) {
                const auto written = write(transfer.file.get(), remaining.data(), remaining.size());

                if (written < 0 && errno == EINTR) {
                    continue;
                }

                if (written <= 0) {
                    return fail(job.transferId, written == 0
                                                    ? std::make_error_code(std::errc::io_error)
                                                    : std::error_code(errno, std::generic_category()));
                }

                remaining.remove_prefix(static_cast<std::size_t>(written));
            }

            transfer.received += job.data.size();
            return {};
        }

        if (job.operation != DiskOperation::Commit || !job.data.empty() ||
            transfer.received != transfer.expected) {
            return fail(job.transferId, std::make_error_code(std::errc::invalid_argument));
        }

        int synced;
        do {
            synced = fsync(transfer.file.get());
        } while (synced < 0 && errno == EINTR);

        if (synced != 0) {
            return fail(job.transferId, {errno, std::generic_category()});
        }

        if (renameat2(AT_FDCWD, transfer.temporary.c_str(), AT_FDCWD, transfer.finalPath.c_str(),
                      RENAME_NOREPLACE) != 0) {
            return fail(job.transferId, {errno, std::generic_category()});
        }

        transfer.temporaryExists = false;
        transfers_.erase(found);
        return {};
    } catch (const std::system_error& failure) {
        return fail(job.transferId, failure.code());
    } catch (const std::bad_alloc&) {
        return fail(job.transferId, std::make_error_code(std::errc::not_enough_memory));
    } catch (...) {
        return fail(job.transferId, std::make_error_code(std::errc::io_error));
    }
}
