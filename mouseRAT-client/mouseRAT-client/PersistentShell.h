#pragma once

#include <Windows.h>

#include <array>
#include <memory>
#include <string>
#include <system_error>

class PersistentShell {
public:
    PersistentShell() {
        job_.reset(CreateJobObjectW(nullptr, nullptr));
        if (!job_) {
            fail("CreateJobObjectW");
        }

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            fail("SetInformationJobObject");
        }

        std::array<wchar_t, MAX_PATH> directory{};
        const auto length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
        if (length == 0 || length >= directory.size()) {
            fail("GetSystemDirectoryW", length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER);
        }

        const auto application = std::wstring(directory.data(), length) + L"\\cmd.exe";
        wchar_t command[] = L"cmd.exe /D /Q";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(application.c_str(), command, nullptr, nullptr, FALSE,
            CREATE_SUSPENDED | CREATE_NEW_CONSOLE, nullptr, nullptr, &startup, &child)) {
            fail("CreateProcessW");
        }

        process_.reset(child.hProcess);
        Handle thread(child.hThread, CloseHandle);
        if (!AssignProcessToJobObject(job_.get(), process_.get())) {
            fail("AssignProcessToJobObject");
        }

        if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
            fail("ResumeThread");
        }
    }

    // Borrows the process handle
    [[nodiscard]] HANDLE nativeHandle() const noexcept {
        return process_.get();
    }

private:
    [[noreturn]] static void fail(const char* operation, DWORD error = GetLastError()) {
        throw std::system_error(static_cast<int>(error), std::system_category(), operation);
    }

    static void closeProcess(HANDLE process) noexcept {
        TerminateProcess(process, ERROR_CANCELLED);
        CloseHandle(process);
    }

    using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;
    Handle job_{ nullptr, CloseHandle };
    std::unique_ptr<void, decltype(&closeProcess)> process_{ nullptr, closeProcess };
};
