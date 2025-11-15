#pragma once

#include "ScreenshotStore.h"

#include <array>
#include <optional>

enum class ScreenshotAdmission { Accepted, Full, InvalidState, IdExhausted };

struct ScreenshotBegin {
    ScreenshotAdmission status;
    std::uint64_t transferId = 0;
};

struct ScreenshotCompletion {
    DiskCompletion disk;
    std::uint64_t clientId;
    std::uint32_t requestId;
    bool firstFailure;
    bool released;
};

class ScreenshotTransfers {
public:
    static constexpr std::size_t maxTransfers = ScreenshotStore::maxTransfers;

    explicit ScreenshotTransfers(DiskWorker& worker) : worker_(worker) {}
    ScreenshotTransfers(const ScreenshotTransfers&) = delete;
    ScreenshotTransfers& operator=(const ScreenshotTransfers&) = delete;

    [[nodiscard]] ScreenshotBegin begin(std::uint64_t clientId, std::uint32_t requestId,
                                        std::string_view metadata);
    [[nodiscard]] ScreenshotAdmission write(std::uint64_t id, std::string_view data);
    [[nodiscard]] ScreenshotAdmission commit(std::uint64_t id);
    void cancelClient(std::uint64_t clientId);
    [[nodiscard]] std::optional<ScreenshotCompletion> takeCompletion();

private:
    enum class State { Free, Streaming, Committing, CancelPending, CancelQueued, CleanupFailed };
    struct Transfer {
        std::uint64_t id = 0;
        std::uint64_t clientId = 0;
        std::uint32_t requestId = 0;
        State state = State::Free;
        bool failed = false;
    };

    Transfer* find(std::uint64_t id) noexcept;
    void retryCancellations();

    DiskWorker& worker_;
    std::array<Transfer, maxTransfers> transfers_{};
    std::uint64_t lastId_ = 0;
};
