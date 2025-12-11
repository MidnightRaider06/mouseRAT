#pragma once

#include <string>
#include <string_view>
#include <vector>

class CommandFiles {
public:
    CommandFiles();
    ~CommandFiles();
    CommandFiles(const CommandFiles&) = delete;
    CommandFiles& operator=(const CommandFiles&) = delete;

    // Borrows a UTF-16 environment block for shell creation
    [[nodiscard]] const wchar_t* environment() const noexcept {
        return environment_.data();
    }

    [[nodiscard]] const std::wstring& directory() const noexcept {
        return directory_;
    }

    // Returns ASCII shell input that invokes the staged UTF-8 batch file
    [[nodiscard]] std::string stage(std::string_view command,
        std::string_view outputPipe = {}, std::string_view statusPipe = {});
    // Call only after the shell has finished using the file
    void release();

private:
    std::wstring directory_, path_;
    std::vector<wchar_t> environment_;
    std::string invocation_, reportInvocation_;
    bool staged_ = false;
};
