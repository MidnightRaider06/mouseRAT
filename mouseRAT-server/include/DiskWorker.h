#pragma once

#include "UniqueFd.h"

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>
#include <thread>

enum class DiskOperation { Begin, Write, Commit, Cancel };

struct DiskJob {
    DiskOperation operation;
    std::uint64_t transferId;
    std::string_view data;
};

struct DiskCompletion {
    DiskOperation operation;
    std::uint64_t transferId;
    std::size_t bytes;
    std::error_code error;
};

class DiskWorker {
public:
    static constexpr std::size_t maxJobs = 128;
    static constexpr std::size_t maxJobBytes = 65536;
    using Handler = std::function<std::error_code(const DiskJob&)>;

    explicit DiskWorker(Handler handler);
    ~DiskWorker();
    DiskWorker(const DiskWorker&) = delete;
    DiskWorker& operator=(const DiskWorker&) = delete;

    [[nodiscard]] bool submit(const DiskJob& job);
    [[nodiscard]] std::optional<DiskCompletion> takeCompletion();
    [[nodiscard]] int descriptor() const noexcept {
        return completionFd_.get();
    }

    void requestStop();
    void stop();

private:
    struct Slot {
        DiskOperation operation{};
        std::uint64_t transferId{};
        std::size_t size{};
        std::error_code error{};
        std::array<char, maxJobBytes> data{};
    };

    void run();
    void updateNotification(bool signal) noexcept;

    Handler handler_;
    std::unique_ptr<Slot[]> slots_;
    UniqueFd completionFd_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::size_t head_ = 0;
    std::size_t nextWork_ = 0;
    std::size_t occupied_ = 0;
    std::size_t queued_ = 0;
    std::size_t completed_ = 0;
    bool stopping_ = false;
    std::thread worker_;
};
