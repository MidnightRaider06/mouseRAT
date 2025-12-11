#include "CommandExecutor.h"
#include "Protocol.h"

#include <array>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace {

[[noreturn]] void fail(const char* operation, DWORD error = GetLastError()) {
    throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}

}

class CommandExecutor::Pipe {
public:
    explicit Pipe(std::string_view name, DWORD maximum) : maximum_(maximum) {
        const auto wide = std::wstring(name.begin(), name.end());
        const auto pipe = CreateNamedPipeW(wide.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED
            | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 0, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            fail("CreateNamedPipeW command");
        }
        pipe_.reset(pipe);
        event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event_) {
            fail("CreateEventW command");
        }
        operation_.hEvent = event_.get();
        if (ConnectNamedPipe(pipe_.get(), &operation_)) {
            connected_ = true;
        } else {
            const auto error = GetLastError();
            if (error == ERROR_IO_PENDING) {
                pending_ = true;
            } else if (error == ERROR_PIPE_CONNECTED) {
                connected_ = true;
            } else {
                fail("ConnectNamedPipe command", error);
            }
        }
    }

    ~Pipe() {
        if (pending_) {
            CancelIoEx(pipe_.get(), &operation_);
            DWORD ignored = 0;
            GetOverlappedResult(pipe_.get(), &operation_, &ignored, TRUE);
        }
    }

    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;

    std::optional<std::string> read() {
        if (closed_) {
            return std::nullopt;
        }
        DWORD count = 0;
        if (pending_) {
            if (!GetOverlappedResult(pipe_.get(), &operation_, &count, FALSE)) {
                const auto error = GetLastError();
                if (error == ERROR_IO_INCOMPLETE) {
                    return std::string();
                }
                pending_ = false;
                if (error == ERROR_BROKEN_PIPE) {
                    closed_ = true;
                    return std::nullopt;
                }
                fail("GetOverlappedResult command", error);
            }
            pending_ = false;
            if (connected_) {
                return std::string(bytes_.data(), count);
            }
            connected_ = true;
        }

        ResetEvent(event_.get());
        if (ReadFile(pipe_.get(), bytes_.data(), maximum_, &count, &operation_)) {
            return std::string(bytes_.data(), count);
        }
        const auto error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            pending_ = true;
            return std::string();
        }
        if (error == ERROR_BROKEN_PIPE) {
            closed_ = true;
            return std::nullopt;
        }
        fail("ReadFile command", error);
    }

private:
    using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;
    Handle pipe_{ nullptr, CloseHandle }, event_{ nullptr, CloseHandle };
    OVERLAPPED operation_{};
    std::array<char, 65535> bytes_{};
    DWORD maximum_;
    bool pending_ = false, connected_ = false, closed_ = false;
};

CommandExecutor::CommandExecutor() : shell_(files_.environment()) {
}

CommandExecutor::~CommandExecutor() = default;

int CommandExecutor::reportResult(std::string_view pipe, std::string_view text) {
    constexpr std::string_view prefix = "\\\\.\\pipe\\mouseRAT-";
    if (pipe.size() <= prefix.size() || pipe.size() > 256 || pipe.substr(0, prefix.size()) != prefix
        || pipe.substr(prefix.size()).find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string_view::npos) {
        return 2;
    }

    std::array<char, 5> record{};
    SetLastError(ERROR_SUCCESS);
    const auto shadowLength = GetEnvironmentVariableW(L"ERRORLEVEL", nullptr, 0);
    const auto shadowError = GetLastError();
    std::int64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (shadowLength == 0 && shadowError == ERROR_ENVVAR_NOT_FOUND && !text.empty()
        && parsed.ec == std::errc() && parsed.ptr == text.data() + text.size()
        && value >= (std::numeric_limits<std::int32_t>::min)()
        && value <= (std::numeric_limits<std::uint32_t>::max)()) {
        record[0] = 1;
        for (std::size_t index = 0; index < 4; ++index) {
            record[index + 1] = static_cast<char>(static_cast<std::uint32_t>(value) >> (24 - 8 * index));
        }
    }

    const auto wide = std::wstring(pipe.begin(), pipe.end());
    const auto handle = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return 2;
    }
    std::unique_ptr<void, decltype(&CloseHandle)> output(handle, CloseHandle);
    std::size_t offset = 0;
    while (offset < record.size()) {
        DWORD count = 0;
        if (!WriteFile(output.get(), record.data() + offset, static_cast<DWORD>(record.size() - offset),
            &count, nullptr) || count == 0) {
            return 2;
        }
        offset += count;
    }
    return 0;
}

void CommandExecutor::start(std::string_view command) {
    if (active_ || failed_) {
        throw std::logic_error("Command runner is busy or failed");
    }
    if (!protocol::isValidCommandText(command)) {
        throw std::invalid_argument("Invalid command text");
    }
    if (sequence_ == (std::numeric_limits<std::uint64_t>::max)()) {
        throw std::overflow_error("Command channel names exhausted");
    }

    try {
        const auto& directory = files_.directory();
        std::string base = "\\\\.\\pipe\\";
        for (const auto byte : directory.substr(directory.find_last_of(L'\\') + 1)) {
            base += static_cast<char>(byte);
        }
        base += "-" + std::to_string(++sequence_);
        const auto outputName = base + "-output", statusName = base + "-status";
        output_ = std::make_unique<Pipe>(outputName, 65535);
        status_ = std::make_unique<Pipe>(statusName, 16);
        const auto invocation = files_.stage(command, outputName, statusName);
        shell_.writeInput(invocation);
        active_ = true;
    } catch (...) {
        failed_ = true;
        throw;
    }
}

CommandExecutor::Update CommandExecutor::poll() {
    if (failed_) {
        throw std::logic_error("Command runner belongs to a failed session");
    }
    if (!active_) {
        return {};
    }
    try {
        return pollActive();
    } catch (...) {
        failed_ = true;
        throw;
    }
}

CommandExecutor::Update CommandExecutor::pollActive() {
    (void)shell_.inputPending();
    (void)shell_.readOutput();
    const auto output = output_->read();
    if (output && !output->empty()) {
        if (output->size() > 16 * 1024 * 1024 - outputBytes_) {
            throw std::length_error("Command output exceeds 16 MiB");
        }
        outputBytes_ += output->size();
        return { *output, std::nullopt };
    }

    if (!result_) {
        const auto status = status_->read();
        if (status) {
            statusBytes_ += *status;
            if (statusBytes_.size() > 5) {
                throw std::runtime_error("Invalid command result channel");
            }
        } else {
            if (statusBytes_.size() != 5 || statusBytes_[0] != 1) {
                throw std::runtime_error("Command exit status could not be established");
            }
            std::uint32_t value = 0;
            for (std::size_t index = 1; index < 5; ++index) {
                value = (value << 8) | static_cast<unsigned char>(statusBytes_[index]);
            }
            result_ = value;
        }
    }

    const auto exited = WaitForSingleObject(shell_.nativeHandle(), 0);
    if (exited == WAIT_FAILED) {
        fail("WaitForSingleObject command");
    }
    if (exited == WAIT_OBJECT_0 && !result_) {
        DWORD value = 0;
        if (!GetExitCodeProcess(shell_.nativeHandle(), &value)) {
            fail("GetExitCodeProcess command");
        }
        result_ = value;
    }

    if (!output && result_) {
        const auto value = *result_;
        files_.release();
        output_.reset();
        status_.reset();
        result_.reset();
        statusBytes_.clear();
        outputBytes_ = 0;
        active_ = false;
        failed_ = exited == WAIT_OBJECT_0;
        return { {}, value };
    }
    return {};
}
