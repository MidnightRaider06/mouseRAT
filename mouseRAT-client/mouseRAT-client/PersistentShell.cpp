#include "PersistentShell.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <vector>

struct PersistentShell::OutputState {
    std::mutex mutex;
    std::condition_variable ready;
    std::array<char, maxOutputBytes> bytes{};
    std::size_t begin = 0, end = 0;
    bool stopping = false, closed = false;
    DWORD error = ERROR_SUCCESS;
};

PersistentShell::PersistentShell() : outputState_(std::make_shared<OutputState>()) {
    job_.reset(CreateJobObjectW(nullptr, nullptr));
    if (!job_) {
        fail("CreateJobObjectW");
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        fail("SetInformationJobObject");
    }

    Handle childInput(nullptr, CloseHandle), childOutput(nullptr, CloseHandle);
    createPipe(input_, childInput, true);
    createPipe(output_, childOutput, false);
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    if (attributeSize == 0) {
        fail("InitializeProcThreadAttributeList");
    }

    std::vector<unsigned char> storage(attributeSize);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &attributeSize)) {
        fail("InitializeProcThreadAttributeList");
    }

    std::unique_ptr<_PROC_THREAD_ATTRIBUTE_LIST, decltype(&DeleteProcThreadAttributeList)>
        attributes(list, DeleteProcThreadAttributeList);
    HANDLE inherited[]{ childInput.get(), childOutput.get() };
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited, sizeof(inherited), nullptr, nullptr)) {
        fail("UpdateProcThreadAttribute");
    }

    std::array<wchar_t, MAX_PATH> directory{};
    const auto length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    if (length == 0 || length >= directory.size()) {
        fail("GetSystemDirectoryW", length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER);
    }

    const auto application = std::wstring(directory.data(), length) + L"\\cmd.exe";
    wchar_t command[] = L"cmd.exe /D /Q /K \"chcp 65001>nul\"";
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = childInput.get();
    startup.StartupInfo.hStdOutput = childOutput.get();
    startup.StartupInfo.hStdError = childOutput.get();
    startup.lpAttributeList = list;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(application.c_str(), command, nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_NEW_CONSOLE | EXTENDED_STARTUPINFO_PRESENT,
        nullptr, nullptr, &startup.StartupInfo, &child)) {
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

    outputTask_ = std::async(std::launch::async, [state = outputState_, pipe = output_.get()] {
        while (true) {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->ready.wait(lock, [&] { return state->stopping || state->begin == state->end; });
            if (state->stopping) {
                return;
            }

            lock.unlock();
            DWORD count = 0;
            const auto read = ReadFile(pipe, state->bytes.data(),
                static_cast<DWORD>(state->bytes.size()), &count, nullptr);
            const auto error = read ? ERROR_SUCCESS : GetLastError();
            lock.lock();
            if (!read) {
                state->closed = error == ERROR_BROKEN_PIPE;
                state->error = state->closed ? ERROR_SUCCESS : error;
                return;
            }

            state->begin = 0;
            state->end = count;
        }
    });
}

PersistentShell::~PersistentShell() {
    if (outputState_) {
        {
            std::lock_guard<std::mutex> lock(outputState_->mutex);
            outputState_->stopping = true;
        }
        outputState_->ready.notify_one();
    }

    job_.reset();
    process_.reset();
    if (inputTask_.valid()) {
        inputTask_.wait();
    }

    if (outputTask_.valid()) {
        outputTask_.wait();
    }
}

void PersistentShell::writeInput(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > maxInputBytes) {
        throw std::invalid_argument("Shell input must contain 1..8192 bytes");
    }

    if (inputPending()) {
        throw std::logic_error("Shell input is still pending");
    }

    inputTask_ = std::async(std::launch::async, [pipe = input_.get(), data = std::string(bytes)] {
        std::size_t offset = 0;
        while (offset < data.size()) {
            DWORD written = 0;
            if (!WriteFile(pipe, data.data() + offset, static_cast<DWORD>(data.size() - offset),
                &written, nullptr)) {
                fail("Shell input WriteFile");
            }

            if (written == 0) {
                throw std::runtime_error("Shell input made no write progress");
            }

            offset += written;
        }
    });
}

bool PersistentShell::inputPending() {
    if (failed_) {
        throw std::logic_error("Shell I/O belongs to a failed session");
    }

    if (!inputTask_.valid()) {
        return false;
    }

    if (inputTask_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return true;
    }

    try {
        inputTask_.get();
    } catch (...) {
        failed_ = true;
        throw;
    }

    return false;
}

std::optional<std::string> PersistentShell::readOutput(std::size_t maximum) {
    if (failed_) {
        throw std::logic_error("Shell I/O belongs to a failed session");
    }

    if (maximum == 0 || maximum > maxOutputBytes) {
        throw std::invalid_argument("Shell output read must allow 1..65535 bytes");
    }

    if (outputTask_.valid()
        && outputTask_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            outputTask_.get();
        } catch (...) {
            failed_ = true;
            throw;
        }
    }

    std::unique_lock<std::mutex> lock(outputState_->mutex);
    if (outputState_->begin == outputState_->end) {
        if (outputState_->error != ERROR_SUCCESS) {
            failed_ = true;
            fail("Shell output ReadFile", outputState_->error);
        }

        return outputState_->closed ? std::nullopt : std::optional<std::string>("");
    }

    const auto count = (std::min)(maximum, outputState_->end - outputState_->begin);
    std::string bytes(outputState_->bytes.data() + outputState_->begin, count);
    outputState_->begin += count;
    const auto drained = outputState_->begin == outputState_->end;
    lock.unlock();
    if (drained) {
        outputState_->ready.notify_one();
    }

    return bytes;
}

void PersistentShell::createPipe(Handle& parent, Handle& child, bool parentWrites) {
    SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &security, 4096)) {
        fail("CreatePipe");
    }

    parent.reset(parentWrites ? write : read);
    child.reset(parentWrites ? read : write);
    if (!SetHandleInformation(parent.get(), HANDLE_FLAG_INHERIT, 0)) {
        fail("SetHandleInformation");
    }
}

void PersistentShell::fail(const char* operation, DWORD error) {
    throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}

void PersistentShell::closeProcess(HANDLE process) noexcept {
    TerminateProcess(process, ERROR_CANCELLED);
    CloseHandle(process);
}
