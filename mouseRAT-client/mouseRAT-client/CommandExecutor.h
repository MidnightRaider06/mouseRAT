#pragma once

#include "CommandFiles.h"
#include "PersistentShell.h"

#include <cstdint>

class CommandExecutor {
public:
    struct Update {
        std::string output;
        std::optional<std::uint32_t> result;
    };

    CommandExecutor();
    ~CommandExecutor();
    CommandExecutor(const CommandExecutor&) = delete;
    CommandExecutor& operator=(const CommandExecutor&) = delete;

    void start(std::string_view command);
    // Call again only after consuming the previous output
    [[nodiscard]] Update poll();
    // Internal child-process entry point, no network connection or shell creation
    static int reportResult(std::string_view pipe, std::string_view value);

private:
    class Pipe;
    Update pollActive();

    CommandFiles files_;
    PersistentShell shell_;
    std::unique_ptr<Pipe> output_, status_;
    std::string statusBytes_;
    std::optional<std::uint32_t> result_;
    std::uint64_t sequence_ = 0;
    std::size_t outputBytes_ = 0;
    bool active_ = false, failed_ = false;
};
