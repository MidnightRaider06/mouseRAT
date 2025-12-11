#include "CommandFiles.h"

#include <Windows.h>
#include <objbase.h>

#include "Protocol.h"

#include <algorithm>
#include <array>
#include <cwchar>
#include <memory>
#include <stdexcept>
#include <system_error>

namespace {

[[noreturn]] void fail(const char* operation, DWORD error = GetLastError()) {
    throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}

std::wstring_view name(const std::wstring& entry) {
    return std::wstring_view(entry).substr(0, entry.find(L'=', 1));
}

void writeFile(const std::wstring& path, std::string_view bytes, bool& owned) {
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        fail("CreateFileW command");
    }
    owned = true;
    std::unique_ptr<void, decltype(&CloseHandle)> file(handle, CloseHandle);
    try {
        while (!bytes.empty()) {
            DWORD written = 0;
            if (!WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) {
                fail("WriteFile command");
            }
            if (written == 0) {
                throw std::runtime_error("Command file made no write progress");
            }
            bytes.remove_prefix(written);
        }
    } catch (...) {
        file.reset();
        if (DeleteFileW(path.c_str())) {
            owned = false;
        }
        throw;
    }
}

}

CommandFiles::CommandFiles() {
    GUID id{};
    const auto result = CoCreateGuid(&id);
    if (FAILED(result)) {
        fail("CoCreateGuid", static_cast<DWORD>(result));
    }

    std::string token;
    constexpr char hex[] = "0123456789abcdef";
    const auto* bytes = reinterpret_cast<const unsigned char*>(&id);
    for (std::size_t index = 0; index < sizeof(id); ++index) {
        const auto byte = bytes[index];
        token += hex[byte >> 4];
        token += hex[byte & 15];
    }

    std::array<wchar_t, MAX_PATH + 1> temporary{};
    const auto length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
    if (length == 0 || length >= temporary.size()) {
        fail("GetTempPathW", length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER);
    }

    directory_ = std::wstring(temporary.data(), length) + L"mouseRAT-" + std::wstring(token.begin(), token.end());
    path_ = directory_ + L"\\request.cmd";
    const auto variable = "MRAT_COMMANDS_" + token;
    invocation_ = "@chcp 65001>nul && \"%" + variable + "%\\request.cmd\"\r\n";
    reportInvocation_ = "@\"%" + variable + "_EXE%\" --report-command-result ";
    std::vector<std::wstring> entries;
    std::unique_ptr<wchar_t, decltype(&FreeEnvironmentStringsW)>
        inherited(GetEnvironmentStringsW(), FreeEnvironmentStringsW);
    if (!inherited) {
        fail("GetEnvironmentStringsW");
    }

    for (auto* entry = inherited.get(); *entry; entry += std::wcslen(entry) + 1) {
        entries.emplace_back(entry);
    }

    entries.emplace_back(std::wstring(variable.begin(), variable.end()) + L"=" + directory_);
    std::vector<wchar_t> executable(32768);
    const auto executableLength = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (executableLength == 0 || executableLength >= executable.size()) {
        fail("GetModuleFileNameW", executableLength == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER);
    }
    entries.emplace_back(std::wstring(variable.begin(), variable.end()) + L"_EXE="
        + std::wstring(executable.data(), executableLength));
    std::sort(entries.begin(), entries.end(), [](const auto& first, const auto& second) {
        const auto a = name(first), b = name(second);
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
            b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    });
    for (const auto& entry : entries) {
        environment_.insert(environment_.end(), entry.begin(), entry.end());
        environment_.push_back(L'\0');
    }
    environment_.push_back(L'\0');

    if (!CreateDirectoryW(directory_.c_str(), nullptr)) {
        fail("CreateDirectoryW");
    }
}

CommandFiles::~CommandFiles() {
    if (staged_) {
        DeleteFileW(path_.c_str());
    }
    RemoveDirectoryW(directory_.c_str());
}

std::string CommandFiles::stage(std::string_view command,
    std::string_view outputPipe, std::string_view statusPipe) {
    if (!protocol::isValidCommandText(command)) {
        throw std::invalid_argument("Command must contain 1..4096 valid UTF-8 bytes without NUL");
    }

    if (staged_) {
        throw std::logic_error("A command file is still in use");
    }

    const auto validPipe = [](std::string_view pipe) {
        constexpr std::string_view prefix = "\\\\.\\pipe\\";
        return pipe.size() > prefix.size() && pipe.size() <= 256 && pipe.substr(0, prefix.size()) == prefix
            && pipe.substr(prefix.size()).find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string_view::npos;
    };
    if ((!outputPipe.empty() || !statusPipe.empty())
        && (!validPipe(outputPipe) || !validPipe(statusPipe) || outputPipe == statusPipe)) {
        throw std::invalid_argument("Invalid command channel names");
    }

    std::string bytes;
    if (!outputPipe.empty()) {
        const auto label = "mrat_command_" + std::string(outputPipe.substr(9));
        bytes = "@call :" + label + " >\"" + std::string(outputPipe) + "\" 2>&1\r\n"
            "@exit /b\r\n:" + label + "\r\n";
    }
    bytes += std::string(command) + "\r\n";
    writeFile(path_, bytes, staged_);

    return statusPipe.empty() ? invocation_ : invocation_ + reportInvocation_
        + "\"" + std::string(statusPipe) + "\" \"%ERRORLEVEL%\"\r\n";
}

void CommandFiles::release() {
    if (staged_) {
        if (!DeleteFileW(path_.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
            fail("DeleteFileW command");
        }

        staged_ = false;
    }
}
