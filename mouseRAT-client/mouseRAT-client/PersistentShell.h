#pragma once

#include <Windows.h>

#include <cstddef>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

class PersistentShell {
public:
    static constexpr std::size_t maxInputBytes = 8192;
    static constexpr std::size_t maxOutputBytes = 65535;

    PersistentShell();
    ~PersistentShell();
    PersistentShell(PersistentShell&&) noexcept = default;
    PersistentShell& operator=(PersistentShell&&) = delete;
    PersistentShell(const PersistentShell&) = delete;
    PersistentShell& operator=(const PersistentShell&) = delete;

    // Prepared pipe bytes, completion only confirms delivery
    void writeInput(std::string_view bytes);
    [[nodiscard]] bool inputPending();
    // Empty means no bytes ready, nullopt means all output writers have closed
    [[nodiscard]] std::optional<std::string> readOutput(std::size_t maximum = maxOutputBytes);

    // Borrows the process handle
    [[nodiscard]] HANDLE nativeHandle() const noexcept {
        return process_.get();
    }

private:
    struct OutputState;
    using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;

    [[noreturn]] static void fail(const char* operation, DWORD error = GetLastError());
    static void closeProcess(HANDLE process) noexcept;
    static void createPipe(Handle& parent, Handle& child, bool parentWrites);

    Handle job_{ nullptr, CloseHandle };
    std::unique_ptr<void, decltype(&closeProcess)> process_{ nullptr, closeProcess };
    Handle input_{ nullptr, CloseHandle };
    Handle output_{ nullptr, CloseHandle };
    std::shared_ptr<OutputState> outputState_;
    std::future<void> inputTask_, outputTask_;
    bool failed_ = false;
};
