#include "DiskWorker.h"

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <future>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <string>

DiskCompletion completion(DiskWorker& worker) {
    pollfd event{worker.descriptor(), POLLIN, 0};
    assert(poll(&event, 1, 5000) == 1 && (event.revents & POLLIN));
    const auto result = worker.takeCompletion();
    assert(result);
    return *result;
}

void ownershipAndCapacity() {
    char path[] = "/tmp/mouserat-worker-XXXXXX";
    UniqueFd file(mkstemp(path));
    assert(file.get() >= 0);
    assert(unlink(path) == 0);
    const auto owner = std::this_thread::get_id();
    std::promise<void> started, release, executed;
    const auto gate = release.get_future().share();
    auto entered = started.get_future();
    auto allExecuted = executed.get_future();

    DiskWorker worker([&](const DiskJob& job) -> std::error_code {
        assert(std::this_thread::get_id() != owner);

        if (job.transferId == 0) {
            started.set_value();
            gate.wait();
        }

        if (job.transferId == 17) {
            return std::make_error_code(std::errc::no_space_on_device);
        }

        if (job.transferId == 18) {
            throw std::runtime_error("test handler failure");
        }

        if (job.transferId == 19) {
            throw std::system_error(std::make_error_code(std::errc::permission_denied));
        }

        auto remaining = job.data;

        while (!remaining.empty()) {
            const auto count = write(file.get(), remaining.data(), remaining.size());

            if (count < 0 && errno == EINTR) {
                continue;
            }

            if (count < 0) {
                return {errno, std::generic_category()};
            }

            assert(count > 0);
            remaining.remove_prefix(static_cast<std::size_t>(count));
        }

        if (job.transferId == DiskWorker::maxJobs - 1) {
            executed.set_value();
        }

        return {};
    });

    assert(worker.submit({DiskOperation::Begin, 0, {}}));
    assert(entered.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    std::string expected;
    auto submit = [&](std::size_t id) {
        std::string data(id == 1 ? DiskWorker::maxJobBytes : 1, static_cast<char>('a' + id % 26));

        if (id == 1) {
            data.front() = '\0';
            data.back() = '\xff';
        }

        assert(worker.submit({DiskOperation::Write, id, data}));

        if (id < 17 || id > 19) {
            expected += data;
        }

        data.assign(data.size(), '!');
    };

    for (std::size_t id = 1; id < DiskWorker::maxJobs; ++id) {
        submit(id);
    }

    assert(!worker.submit({DiskOperation::Write, 999, {}}));
    assert(!worker.submit({DiskOperation::Write, 999, std::string(DiskWorker::maxJobBytes + 1, 'x')}));
    release.set_value();
    assert(allExecuted.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    assert(!worker.submit({DiskOperation::Write, 999, {}}));

    for (std::size_t id = 0; id < 2 * DiskWorker::maxJobs; ++id) {
        const auto result = completion(worker);
        assert(result.transferId == id);
        assert(result.operation == (id == 0 ? DiskOperation::Begin : DiskOperation::Write));
        const auto expectedError = id == 17   ? std::errc::no_space_on_device
                                   : id == 18 ? std::errc::io_error
                                              : std::errc::permission_denied;
        if (id >= 17 && id <= 19) {
            assert(result.error == expectedError);
        } else {
            assert(!result.error);
        }

        assert(result.bytes == (id == 0 ? 0 : id == 1 ? DiskWorker::maxJobBytes : 1));

        if (id < DiskWorker::maxJobs) {
            submit(id + DiskWorker::maxJobs);
        }
    }

    assert(!worker.takeCompletion());
    pollfd event{worker.descriptor(), POLLIN, 0};
    assert(poll(&event, 1, 0) == 0);

    std::string actual(expected.size(), '\0');
    assert(pread(file.get(), actual.data(), actual.size(), 0) == static_cast<ssize_t>(actual.size()));
    assert(actual == expected);
}

void shutdown() {
    std::promise<void> started, release;
    auto entered = started.get_future();
    const auto gate = release.get_future().share();
    DiskWorker worker([&](const DiskJob& job) -> std::error_code {
        assert(job.transferId == 1);
        started.set_value();
        gate.wait();
        return {};
    });
    assert(worker.submit({DiskOperation::Write, 1, "active"}));
    assert(entered.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    for (std::size_t id = 2; id <= DiskWorker::maxJobs; ++id) {
        assert(worker.submit({DiskOperation::Cancel, id, {}}));
    }

    worker.requestStop();
    assert(!worker.submit({DiskOperation::Write, 3, "rejected"}));
    release.set_value();
    worker.stop();

    const auto active = completion(worker);
    assert(active.transferId == 1 && !active.error);

    for (std::size_t id = 2; id <= DiskWorker::maxJobs; ++id) {
        const auto canceled = completion(worker);
        assert(canceled.transferId == id && canceled.operation == DiskOperation::Cancel &&
               canceled.error == std::errc::operation_canceled);
    }

    assert(!worker.takeCompletion());
    worker.stop();
}

int main() {
    ownershipAndCapacity();
    shutdown();

    int descriptor;
    {
        DiskWorker idle([](const DiskJob&) {
            return std::error_code{};
        });
        descriptor = idle.descriptor();
        assert(!idle.takeCompletion());
    }

    assert(fcntl(descriptor, F_GETFD) == -1 && errno == EBADF);
}
