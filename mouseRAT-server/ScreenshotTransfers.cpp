#include "ScreenshotTransfers.h"

#include <limits>
#include <stdexcept>

ScreenshotTransfers::Transfer* ScreenshotTransfers::find(std::uint64_t id) noexcept {
    for (auto& transfer : transfers_) {
        if (transfer.state != State::Free && transfer.id == id) {
            return &transfer;
        }
    }

    return nullptr;
}

void ScreenshotTransfers::retryCancellations() {
    for (auto& transfer : transfers_) {
        if (transfer.state == State::CancelPending) {
            if (!worker_.submit({DiskOperation::Cancel, transfer.id, {}})) {
                return;
            }

            transfer.state = State::CancelQueued;
        }
    }
}

ScreenshotBegin ScreenshotTransfers::begin(std::uint64_t clientId, std::uint32_t requestId,
                                           std::string_view metadata) {
    if (clientId == 0 || requestId == 0 || metadata.size() != 12) {
        return {ScreenshotAdmission::InvalidState};
    }

    Transfer* available = nullptr;

    for (auto& transfer : transfers_) {
        if (transfer.state != State::Free && transfer.clientId == clientId) {
            return {ScreenshotAdmission::InvalidState};
        }

        if (transfer.state == State::Free) {
            available = &transfer;
        }
    }

    if (!available) {
        return {ScreenshotAdmission::Full};
    }

    if (lastId_ == std::numeric_limits<std::uint64_t>::max()) {
        return {ScreenshotAdmission::IdExhausted};
    }

    retryCancellations();
    const auto id = lastId_ + 1;

    if (!worker_.submit({DiskOperation::Begin, id, metadata})) {
        return {ScreenshotAdmission::Full};
    }

    std::uint32_t format = 0;

    for (unsigned char byte : metadata.substr(8)) {
        format = (format << 8) | byte;
    }

    *available = {
        id, clientId, requestId, State::Streaming, false, static_cast<protocol::ImageFormat>(format)};
    lastId_ = id;
    return {ScreenshotAdmission::Accepted, id};
}

ScreenshotAdmission ScreenshotTransfers::write(std::uint64_t id, std::string_view data) {
    auto* transfer = find(id);

    if (!transfer || transfer->state != State::Streaming || data.empty() ||
        data.size() > DiskWorker::maxJobBytes) {
        return ScreenshotAdmission::InvalidState;
    }

    retryCancellations();
    return worker_.submit({DiskOperation::Write, id, data}) ? ScreenshotAdmission::Accepted
                                                            : ScreenshotAdmission::Full;
}

ScreenshotAdmission ScreenshotTransfers::commit(std::uint64_t id) {
    auto* transfer = find(id);

    if (!transfer || transfer->state != State::Streaming) {
        return ScreenshotAdmission::InvalidState;
    }

    retryCancellations();

    if (!worker_.submit({DiskOperation::Commit, id, {}})) {
        return ScreenshotAdmission::Full;
    }

    transfer->state = State::Committing;
    return ScreenshotAdmission::Accepted;
}

void ScreenshotTransfers::cancelClient(std::uint64_t clientId) {
    for (auto& transfer : transfers_) {
        if (transfer.clientId == clientId && transfer.state == State::Streaming) {
            transfer.state = State::CancelPending;
        }
    }

    retryCancellations();
}

std::optional<ScreenshotCompletion> ScreenshotTransfers::takeCompletion() {
    const auto completed = worker_.takeCompletion();

    if (!completed) {
        return std::nullopt;
    }

    auto* transfer = find(completed->transferId);

    if (!transfer) {
        throw std::logic_error("Untracked screenshot completion");
    }

    const bool firstFailure = completed->error && !transfer->failed;

    if (completed->error) {
        transfer->failed = true;

        if (completed->operation == DiskOperation::Cancel) {
            transfer->state = State::CleanupFailed;
        } else if (transfer->state != State::CancelQueued) {
            transfer->state = State::CancelPending;
        }
    }

    const bool released =
        !completed->error && (completed->operation == DiskOperation::Cancel ||
                              (completed->operation == DiskOperation::Commit && !transfer->failed));
    const ScreenshotCompletion result{*completed,       transfer->clientId, transfer->requestId,
                                      transfer->format, firstFailure,       released};
    if (released) {
        *transfer = {};
    }

    retryCancellations();
    return result;
}
