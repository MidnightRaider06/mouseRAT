#include "../mouseRAT-client/CommandExecutor.h"

#include <chrono>
#include <iostream>
#include <stdexcept>

using Clock = std::chrono::steady_clock;

void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

std::string binary() {
    std::string bytes(262144, '\0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>(index % 256);
    }
    return bytes;
}

void write(std::string_view bytes) {
    while (!bytes.empty()) {
        DWORD count = 0;
        check(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes.data(), static_cast<DWORD>(bytes.size()),
            &count, nullptr) && count != 0, "Fixture write failed");
        bytes.remove_prefix(count);
    }
}

std::string finish(CommandExecutor& execution, std::uint32_t result) {
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    std::string bytes;
    while (Clock::now() < deadline) {
        auto update = execution.poll();
        bytes += update.output;
        check(bytes.size() <= 1024 * 1024, "Unexpected test output volume");
        if (update.result) {
            if (*update.result != result) {
                std::cerr << "Expected " << result << ", received " << *update.result << '\n';
            }
            check(*update.result == result, "Wrong command exit status");
            return bytes;
        }
        Sleep(1);
    }
    throw std::runtime_error("Command did not complete");
}

std::string run(CommandExecutor& execution, std::string_view command, std::uint32_t result = 0) {
    execution.start(command);
    return finish(execution, result);
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc > 1) {
            if (std::wstring_view(argv[1]) == L"--report-command-result") {
                check(argc == 4, "Invalid result reporter arguments");
                std::string pipe, value;
                for (const auto byte : std::wstring_view(argv[2])) {
                    check(byte < 128, "Non-ASCII result pipe");
                    pipe += static_cast<char>(byte);
                }
                for (const auto byte : std::wstring_view(argv[3])) {
                    value += byte < 128 ? static_cast<char>(byte) : '?';
                }
                return CommandExecutor::reportResult(pipe, value);
            }
            if (std::wstring_view(argv[1]) == L"--held") {
                check(argc == 3, "Missing held fixture event");
                const auto event = OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
                check(event != nullptr, "Cannot open held fixture event");
                write("held\r\n");
                const auto ready = WaitForSingleObject(event, 10000);
                CloseHandle(event);
                check(ready == WAIT_OBJECT_0, "Held fixture was not released");
                write(std::string("late\0\xff", 6));
                return 0;
            }
            const auto bytes = binary();
            const auto repeats = std::wstring_view(argv[1]) == L"--flood" ? 80 : 1;
            for (int index = 0; index < repeats; ++index) {
                write(bytes);
            }
            return 37;
        }

        wchar_t path[32768]{};
        check(GetModuleFileNameW(nullptr, path, 32768) != 0, "Cannot find execution fixture");
        check(SetEnvironmentVariableW(L"MRAT_EXEC_FIXTURE", path) != FALSE, "Cannot set fixture path");
        wchar_t temporary[MAX_PATH + 1]{}, previous[MAX_PATH + 1]{};
        check(GetTempPathW(MAX_PATH + 1, temporary) != 0, "Cannot find execution test directory");
        const auto previousLength = GetEnvironmentVariableW(L"TMP", previous, MAX_PATH + 1);
        check(previousLength <= MAX_PATH, "Test TMP value is too long");
        const auto parent = std::wstring(temporary) + L"MRAT_\u03bb ! & %MRAT_EXEC%_"
            + std::to_wstring(GetCurrentProcessId());
        check(CreateDirectoryW(parent.c_str(), nullptr) != FALSE, "Cannot create Unicode execution directory");
        struct RestoreTemporary {
            std::wstring path, previous;
            ~RestoreTemporary() {
                SetEnvironmentVariableW(L"TMP", previous.empty() ? nullptr : previous.c_str());
                RemoveDirectoryW(path.c_str());
            }
        } restore{ parent, std::wstring(previous, previousLength) };
        check(SetEnvironmentVariableW(L"TMP", parent.c_str()) != FALSE, "Cannot select execution test directory");
        CommandExecutor execution;
        check(run(execution, "rem no output").empty(), "Empty command leaked shell prompts");
        for (const auto& value : { "7", "2147483647", "-1", "-2147483648" }) {
            const auto result = static_cast<std::uint32_t>(std::stoll(value));
            check(run(execution, std::string("exit /b ") + value, result).empty(), "Status command leaked output");
        }
        const std::string unicode = "\xce\xbb\xe4\xb8\xad\xf0\x9f\x98\x80";
        (void)run(execution, "set \"MRAT_KEEP=" + unicode + "\"\r\ncd /d \"%SystemRoot%\"");
        const auto state = run(execution, "echo %MRAT_KEEP%\r\n"
            "if /i \"%CD%\"==\"%SystemRoot%\" (echo cwd_ok) else (exit /b 9)");
        check(state.find(unicode) != std::string::npos && state.find("cwd_ok") != std::string::npos,
            "Persistent shell state was lost");
        (void)run(execution, "set \"COMSPEC=missing-shell.exe\"\r\nexit /b 23", 23);
        check(run(execution, "\"%MRAT_EXEC_FIXTURE%\" --binary", 37) == binary(), "Opaque output changed");
        check(run(execution, "echo ### END OF OUTPUT ###") == "### END OF OUTPUT ###\r\n",
            "Literal output was treated as completion metadata");
        std::cout << "PASS: empty/nonzero commands, full exit-code range, Unicode state and opaque output\n";

        const auto eventText = "Local\\MRAT_Command_Held_" + std::to_string(GetCurrentProcessId());
        const auto eventName = std::wstring(eventText.begin(), eventText.end());
        std::unique_ptr<void, decltype(&CloseHandle)> event(
            CreateEventW(nullptr, TRUE, FALSE, eventName.c_str()), CloseHandle);
        check(event != nullptr, "Cannot create held fixture event");
        execution.start("start \"\" /b \"%MRAT_EXEC_FIXTURE%\" --held \"" + eventText + "\"\r\nexit /b 7");
        std::string held;
        const auto waiting = Clock::now() + std::chrono::seconds(5);
        while (held.empty() && Clock::now() < waiting) {
            const auto update = execution.poll();
            check(!update.result, "Command completed before its background writer closed");
            held += update.output;
            Sleep(1);
        }
        check(held == "held\r\n", "Background fixture did not start");
        bool busy = false;
        try {
            execution.start("echo overlap");
        } catch (const std::logic_error&) {
            busy = true;
        }
        check(busy, "An overlapping command was accepted");
        for (int index = 0; index < 50; ++index) {
            check(!execution.poll().result, "Held output did not delay completion");
            Sleep(1);
        }
        check(SetEvent(event.get()) != FALSE, "Cannot release background writer");
        check(finish(execution, 7) == std::string("late\0\xff", 6), "Late output was lost");
        check(run(execution, "echo fresh") == "fresh\r\n", "Old output reached a later request");
        std::cout << "PASS: background EOF boundary, overlap rejection and no stale output\n";

        {
            CommandExecutor invalid;
            invalid.start("set \"ERRORLEVEL=123\"");
            bool rejected = false;
            try {
                (void)finish(invalid, 0);
            } catch (const std::runtime_error& error) {
                rejected = std::string(error.what()) == "Command exit status could not be established";
            }
            check(rejected, "A shadowed exit status was accepted");
            bool failed = false;
            try {
                invalid.start("echo retry");
            } catch (const std::logic_error&) {
                failed = true;
            }
            check(failed, "A failed runner accepted another command");
        }
        {
            CommandExecutor limited;
            limited.start("\"%MRAT_EXEC_FIXTURE%\" --flood");
            bool rejected = false;
            const auto deadline = Clock::now() + std::chrono::seconds(10);
            try {
                while (Clock::now() < deadline) {
                    check(!limited.poll().result, "Oversized output completed successfully");
                }
            } catch (const std::length_error&) {
                rejected = true;
            }
            check(rejected, "Oversized output was not rejected");
        }
        {
            CommandExecutor exiting;
            check(run(exiting, "exit 19", 19).empty(), "Shell exit leaked output");
        }
        const auto stopping = Clock::now();
        {
            CommandExecutor stopped;
            stopped.start("\"%MRAT_EXEC_FIXTURE%\" --flood");
        }
        check(Clock::now() - stopping < std::chrono::seconds(5), "Command cleanup hung");
        std::cout << "PASS: unknown status rejection, output limit, shell exit and pending-I/O cleanup\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
