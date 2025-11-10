#include "DiskWorker.h"

#include <algorithm>
#include <cerrno>
#include <exception>
#include <stdexcept>
#include <sys/eventfd.h>
#include <utility>

DiskWorker::DiskWorker(Handler handler)
    : handler_(std::move(handler)), slots_(std::make_unique<Slot[]>(maxJobs)),
      completionFd_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE)) {
    if (!handler_) throw std::invalid_argument("Disk worker requires a handler");
    if (completionFd_.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "eventfd");
    }
    worker_ = std::thread(&DiskWorker::run, this);
}

DiskWorker::~DiskWorker() { stop(); }

bool DiskWorker::submit(const DiskJob& job) {
    if (job.data.size() > maxJobBytes) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || occupied_ == maxJobs) return false;

    auto& slot = slots_[(head_ + occupied_) % maxJobs];
    slot.operation = job.operation;
    slot.transferId = job.transferId;
    slot.size = job.data.size();
    if (!job.data.empty()) std::copy_n(job.data.data(), job.data.size(), slot.data.data());
    ++occupied_;
    ++queued_;
    ready_.notify_one();
    return true;
}

std::optional<DiskCompletion> DiskWorker::takeCompletion() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (completed_ == 0) return std::nullopt;

    updateNotification(false);
    const auto& slot = slots_[head_];
    const DiskCompletion result{slot.operation, slot.transferId, slot.size, slot.error};
    head_ = (head_ + 1) % maxJobs;
    --completed_;
    --occupied_;
    return result;
}

void DiskWorker::requestStop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_one();
}

void DiskWorker::stop() {
    requestStop();
    if (worker_.joinable()) worker_.join();
}

void DiskWorker::updateNotification(bool signal) noexcept {
    eventfd_t value = 1;
    int result;
    do {
        result = signal ? eventfd_write(completionFd_.get(), value)
                        : eventfd_read(completionFd_.get(), &value);
    } while (result < 0 && errno == EINTR);

    if (result < 0) std::terminate();
}

void DiskWorker::run() {
    for (;;) {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return stopping_ || queued_ != 0; });
        if (queued_ == 0) return;

        auto& slot = slots_[nextWork_];
        nextWork_ = (nextWork_ + 1) % maxJobs;
        --queued_;
        const bool canceled = stopping_;
        lock.unlock();

        std::error_code error;
        if (canceled) {
            error = std::make_error_code(std::errc::operation_canceled);
        } else {
            try {
                error = handler_({slot.operation, slot.transferId,
                                  std::string_view(slot.data.data(), slot.size)});
            } catch (const std::system_error& failure) {
                error = failure.code();
            } catch (...) {
                error = std::make_error_code(std::errc::io_error);
            }
        }

        lock.lock();
        slot.error = error;
        ++completed_;
        updateNotification(true);
    }
}
