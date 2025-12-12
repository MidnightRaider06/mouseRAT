#include "../mouseRAT-client/PersistentShell.h"
#include "../mouseRAT-client/CommandFiles.h"

#include <chrono>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

using Clock = std::chrono::steady_clock;
using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;

struct TestScript {
    std::wstring path;
    ~TestScript() {
        DeleteFileW(path.c_str());
    }
};

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string utf8(const std::wstring& text) {
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    check(count > 0, "UTF-8 conversion failed");
    std::string bytes(count, '\0');
    check(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), bytes.data(), count, nullptr, nullptr) == count, "UTF-8 conversion failed");
    return bytes;
}

std::string collect(PersistentShell& shell, std::string_view expected, std::string_view also = {}) {
    const auto deadline = Clock::now() + std::chrono::seconds(8);
    std::string observed;
    while (observed.find(expected) == std::string::npos
        || (!also.empty() && observed.find(also) == std::string::npos)) {
        if (Clock::now() >= deadline) {
            std::cerr << "Expected: " << expected << "\nObserved: " << observed.substr(0, 512) << '\n';
        }
        check(Clock::now() < deadline, "Timed out waiting for test output");
        const auto bytes = shell.readOutput(257);
        check(bytes.has_value(), "Output closed unexpectedly");
        observed += *bytes;
        check(observed.size() < 1024 * 1024, "Unexpected test output volume");
        if (bytes->empty()) {
            Sleep(1);
        }
    }
    return observed;
}

void deliver(PersistentShell& shell, const std::string& bytes) {
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (shell.inputPending()) {
        check(Clock::now() < deadline, "Previous input did not reach the pipe");
        Sleep(1);
    }
    shell.writeInput(bytes);
}

std::string binary() {
    std::string bytes(262144, '\0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>(index % 256);
    }
    return bytes;
}

int fixture(int argc, wchar_t** argv) {
    check(argc >= 3, "Missing fixture event");
    Handle started(OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[2]), CloseHandle);
    check(started != nullptr && SetEvent(started.get()), "Cannot signal fixture startup");
    if (std::wstring_view(argv[1]) == L"--wait") {
        Sleep(INFINITE);
        return 0;
    }

    check(argc == 5, "Missing binary fixture arguments");
    Handle finished(OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[3]), CloseHandle);
    const auto unrelated = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(std::stoull(argv[4])));
    SetEvent(unrelated);
    const auto output = binary() + "__MRAT_BINARY_END__";
    std::size_t offset = 0;
    while (offset < output.size()) {
        DWORD written = 0;
        check(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output.data() + offset,
            static_cast<DWORD>(output.size() - offset), &written, nullptr) && written != 0,
            "Binary fixture write failed");
        offset += written;
    }
    check(finished != nullptr && SetEvent(finished.get()), "Cannot signal fixture completion");
    return 0;
}

void stagedCommands() {
    wchar_t temporary[MAX_PATH + 1]{}, previous[MAX_PATH + 1]{};
    check(GetTempPathW(MAX_PATH + 1, temporary) != 0, "Cannot find staging test directory");
    const auto previousLength = GetEnvironmentVariableW(L"TMP", previous, MAX_PATH + 1);
    check(previousLength <= MAX_PATH, "Test TMP value is too long");
    const auto parent = std::wstring(temporary) + L"MRAT_\u03bb\u4e2d ! & %MRAT_LITERAL%_"
        + std::to_wstring(GetCurrentProcessId());
    check(CreateDirectoryW(parent.c_str(), nullptr) != FALSE, "Cannot create Unicode staging fixture");
    struct RestoreTemporary {
        std::wstring path, previous;
        ~RestoreTemporary() {
            SetEnvironmentVariableW(L"TMP", previous.empty() ? nullptr : previous.c_str());
            RemoveDirectoryW(path.c_str());
        }
    } restore{ parent, std::wstring(previous, previousLength) };
    check(SetEnvironmentVariableW(L"TMP", parent.c_str()) != FALSE, "Cannot select staging test directory");
    std::wstring directory;
    {
        CommandFiles files;
        directory = files.directory();
        for (const auto& invalid : { std::string(), std::string("x\0y", 3),
            std::string("\xc0\xaf", 2), std::string(4097, 'x') }) {
            bool rejected = false;
            try {
                (void)files.stage(invalid);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            check(rejected, "Invalid command was staged");
        }

        const std::string value = "\xce\xbb\xe4\xb8\xad\xf0\x9f\x98\x80";
        const auto command = "set \"MRAT_STAGE_VALUE=" + value
            + "\"\r\ncd /d \"%TMP%\"\r\nfor %%x in (1 2) do @echo __STAGE_LOOP_%%x\r\n";
        const auto invocation = files.stage(command);
        for (const auto byte : invocation) {
            check(static_cast<unsigned char>(byte) < 128, "Invocation contains non-ASCII bytes");
        }
        const auto first = invocation.find('%'), last = invocation.find('%', first + 1);
        const auto variable = invocation.substr(first + 1, last - first - 1);
        check(GetEnvironmentVariableW(std::wstring(variable.begin(), variable.end()).c_str(), nullptr, 0) == 0,
            "Parent environment was changed");
        std::ifstream input(std::filesystem::path(directory) / L"request.cmd", std::ios::binary);
        const std::string contents{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
        check(contents == command + "\r\n", "Staged UTF-8 bytes changed");
        input.close();
        bool busy = false;
        try {
            (void)files.stage("echo unexpected");
        } catch (const std::logic_error&) {
            busy = true;
        }
        check(busy, "An active staged command was replaced");
        {
            PersistentShell shell(files.environment());
            deliver(shell, invocation);
            (void)collect(shell, "__STAGE_LOOP_2");
            deliver(shell, "echo __STAGE_STATE__%MRAT_STAGE_VALUE% & cd\r\n");
            const auto observed = collect(shell, "__STAGE_STATE__" + value, utf8(parent));
            check(observed.find("__STAGE_STATE__" + value) != std::string::npos,
                "Staged command lost persistent environment state");
            files.release();
            check(GetFileAttributesW((directory + L"\\request.cmd").c_str()) == INVALID_FILE_ATTRIBUTES,
                "Released command file was retained");
            deliver(shell, "set \"TMP=changed\" & chcp 437>nul\r\n");
            deliver(shell, files.stage("echo __STAGE_SECOND__" + value));
            (void)collect(shell, "__STAGE_SECOND__" + value);
        }
        files.release();
        (void)files.stage(std::string(4096, 'x'));
    }
    check(GetFileAttributesW(directory.c_str()) == INVALID_FILE_ATTRIBUTES, "Command directory was retained");
    std::cout << "PASS: validated UTF-8 staging, batch syntax, Unicode paths, persistent state and cleanup\n";
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc > 1) {
            return fixture(argc, argv);
        }

        stagedCommands();

        wchar_t path[MAX_PATH]{}, temporary[MAX_PATH]{};
        check(GetModuleFileNameW(nullptr, path, MAX_PATH) != 0, "Cannot find test executable");
        check(GetEnvironmentVariableW(L"TEMP", temporary, MAX_PATH) != 0, "Cannot find temporary directory");
        check(SetEnvironmentVariableW(L"MRAT_IO_FIXTURE", path) != FALSE, "Cannot prepare fixture path");
        std::wstring directory(temporary);
        while (directory.size() > 3 && directory.back() == L'\\') {
            directory.pop_back();
        }

        const auto eventName = L"Local\\MRAT_Shell_IO_" + std::to_wstring(GetCurrentProcessId());
        Handle started(CreateEventW(nullptr, TRUE, FALSE, (eventName + L"_start").c_str()), CloseHandle);
        Handle finished(CreateEventW(nullptr, TRUE, FALSE, (eventName + L"_finish").c_str()), CloseHandle);
        SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
        Handle unrelated(CreateEventW(&security, TRUE, FALSE, nullptr), CloseHandle);
        check(started && finished && unrelated, "Cannot create test events");
        const auto scriptName = L"MRAT_Shell_IO_" + std::to_wstring(GetCurrentProcessId()) + L".cmd";
        const auto scriptPath = directory + L"\\" + scriptName;
        const auto scriptHandle = CreateFileW(scriptPath.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        check(scriptHandle != INVALID_HANDLE_VALUE, "Cannot create Unicode input fixture");
        TestScript script{ scriptPath };
        Handle file(scriptHandle, CloseHandle);
        const std::string scriptBytes = "@echo __MRAT_UNICODE__\xce\xbb\xe4\xb8\xad\xf0\x9f\x98\x80\r\n";
        DWORD written = 0;
        check(WriteFile(file.get(), scriptBytes.data(), static_cast<DWORD>(scriptBytes.size()),
            &written, nullptr) && written == scriptBytes.size(), "Cannot write Unicode input fixture");
        file.reset();
        {
            PersistentShell initial;
            const auto process = initial.nativeHandle();
            PersistentShell shell(std::move(initial));
            check(!initial.nativeHandle() && shell.nativeHandle() == process, "Move lost shell ownership");
            deliver(shell, "set \"MRAT_IO_STATE=retained\" & cd /d \"%TEMP%\" & echo __MRAT_FIRST__\r\n");
            (void)collect(shell, "__MRAT_FIRST__");
            deliver(shell, "echo __MRAT_STATE__%MRAT_IO_STATE% & cd\r\n");
            const auto state = collect(shell, "__MRAT_STATE__retained", utf8(directory));
            check(state.find("__MRAT_STATE__retained") != std::string::npos, "Environment state was lost");
            deliver(shell, utf8(L"\"%TEMP%\\" + scriptName + L"\"\r\n"));
            (void)collect(shell, "__MRAT_UNICODE__\xce\xbb\xe4\xb8\xad\xf0\x9f\x98\x80");
            std::cout << "PASS: persistent directory, environment and UTF-8 batch input\n";

            const auto command = L"\"%MRAT_IO_FIXTURE%\" --binary "
                + eventName + L"_start " + eventName + L"_finish "
                + std::to_wstring(reinterpret_cast<UINT_PTR>(unrelated.get())) + L"\r\n";
            deliver(shell, utf8(command));
            check(WaitForSingleObject(started.get(), 5000) == WAIT_OBJECT_0, "Binary fixture did not start");
            Sleep(100);
            check(WaitForSingleObject(finished.get(), 0) == WAIT_TIMEOUT, "Output producer was not backpressured");
            const auto observed = collect(shell, "__MRAT_BINARY_END__");
            check(observed.find(binary()) != std::string::npos, "Binary bytes changed or were dropped");
            check(WaitForSingleObject(finished.get(), 5000) == WAIT_OBJECT_0, "Binary fixture did not finish");
            check(WaitForSingleObject(unrelated.get(), 0) == WAIT_TIMEOUT, "An unrelated handle was inherited");
            std::cout << "PASS: opaque binary output, bounded backpressure and handle isolation\n";

            deliver(shell, "exit 7\r\n");
            check(WaitForSingleObject(process, 5000) == WAIT_OBJECT_0, "Shell did not exit");
            DWORD result = 0;
            check(GetExitCodeProcess(process, &result) && result == 7, "Wrong shell process exit code");
            const auto deadline = Clock::now() + std::chrono::seconds(5);
            while (shell.readOutput().has_value()) {
                check(Clock::now() < deadline, "Output pipe did not report closure");
                Sleep(1);
            }
            std::cout << "PASS: process exit and output EOF remain distinct\n";
        }

        ResetEvent(started.get());
        auto blocked = std::make_unique<PersistentShell>();
        deliver(*blocked, utf8(L"\"%MRAT_IO_FIXTURE%\" --wait " + eventName + L"_start\r\n"));
        check(WaitForSingleObject(started.get(), 5000) == WAIT_OBJECT_0, "Wait fixture did not start");
        for (unsigned attempt = 0; attempt < 32 && !blocked->inputPending(); ++attempt) {
            blocked->writeInput(std::string(PersistentShell::maxInputBytes, 'x'));
            Sleep(10);
        }
        check(blocked->inputPending(), "Input pipe did not apply backpressure");
        bool rejected = false;
        try {
            blocked->writeInput("dir\r\n");
        } catch (const std::logic_error&) {
            rejected = true;
        }
        check(rejected, "A second pending write was accepted");
        const auto stopping = Clock::now();
        blocked.reset();
        check(Clock::now() - stopping < std::chrono::seconds(5), "Shutdown hung on pending I/O");
        std::cout << "PASS: bounded pending input and shutdown during blocked I/O\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
