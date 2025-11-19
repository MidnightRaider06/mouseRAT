#include "ScreenshotTransfers.h"

#include <cassert>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iterator>
#include <poll.h>

constexpr auto accepted = ScreenshotAdmission::Accepted;
constexpr auto full = ScreenshotAdmission::Full;
constexpr auto invalid = ScreenshotAdmission::InvalidState;

const std::string bmp("\x42\x4d\x3a\x00\x00\x00\x00\x00\x00\x00\x36\x00\x00\x00\x28\x00"
                      "\x00\x00\x01\x00\x00\x00\x01\x00\x00\x00\x01\x00\x18\x00\x00\x00"
                      "\x00\x00\x04\x00\x00\x00\x13\x0b\x00\x00\x13\x0b\x00\x00\x00\x00"
                      "\x00\x00\x00\x00\x00\x00\x00\x00\xff\x00",
                      58);

std::string metadata() {
    std::string bytes(12, '\0');
    bytes[7] = static_cast<char>(bmp.size());
    bytes[11] = 2;
    return bytes;
}

ScreenshotCompletion completion(ScreenshotTransfers& transfers, DiskWorker& worker) {
    pollfd event{worker.descriptor(), POLLIN, 0};
    assert(poll(&event, 1, 5000) == 1 && (event.revents & POLLIN));
    const auto result = transfers.takeCompletion();
    assert(result);
    return *result;
}

void lifecycle(const std::filesystem::path& directory) {
    ScreenshotStore store(directory);
    DiskWorker worker(std::ref(store));
    ScreenshotTransfers transfers(worker);
    assert(transfers.begin(0, 1, metadata()).status == invalid);
    assert(transfers.begin(10, 1, "bad").status == invalid);

    const auto first = transfers.begin(10, 900, metadata());
    const auto second = transfers.begin(20, 1, metadata());
    assert(first.status == accepted && first.transferId == 1);
    assert(second.status == accepted && second.transferId == 2);
    assert(transfers.begin(10, 901, metadata()).status == invalid);
    assert(transfers.write(999, bmp) == invalid);
    assert(transfers.write(first.transferId, {}) == invalid);
    assert(transfers.write(first.transferId, bmp) == accepted);
    assert(transfers.commit(first.transferId) == accepted);
    assert(transfers.write(first.transferId, bmp) == invalid);
    assert(transfers.commit(first.transferId) == invalid);
    transfers.cancelClient(10);
    transfers.cancelClient(20);

    for (int i = 0; i < 3; ++i) {
        assert(!completion(transfers, worker).released);
    }

    const auto saved = completion(transfers, worker);
    assert(saved.clientId == 10 && saved.requestId == 900 && saved.released && !saved.disk.error);
    assert(saved.disk.operation == DiskOperation::Commit);
    const auto canceled = completion(transfers, worker);
    assert(canceled.clientId == 20 && canceled.released && !canceled.disk.error);
    assert(canceled.disk.operation == DiskOperation::Cancel);

    std::ifstream input(store.destination(1, protocol::ImageFormat::Bmp), std::ios::binary);
    assert(std::string(std::istreambuf_iterator<char>(input), {}) == bmp);
    assert(!std::filesystem::exists(store.destination(2, protocol::ImageFormat::Bmp)));
    assert(!transfers.takeCompletion());

    const auto bad = transfers.begin(30, 6, metadata());
    assert(transfers.write(bad.transferId, std::string(bmp.size(), 'x')) == accepted);
    assert(transfers.commit(bad.transferId) == accepted);
    assert(!completion(transfers, worker).disk.error);
    const auto failedWrite = completion(transfers, worker);
    assert(failedWrite.firstFailure && failedWrite.disk.error && !failedWrite.released);
    const auto failedCommit = completion(transfers, worker);
    assert(!failedCommit.firstFailure && failedCommit.disk.error && !failedCommit.released);
    assert(completion(transfers, worker).released);
    assert(!std::filesystem::exists(store.destination(bad.transferId, protocol::ImageFormat::Bmp)));

    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        assert(entry.path().extension() != ".part");
    }
}

void saturatedQueue() {
    std::promise<void> started, release;
    const auto gate = release.get_future().share();
    DiskWorker worker([&](const DiskJob& job) -> std::error_code {
        if (job.operation == DiskOperation::Begin && job.transferId == 1) {
            started.set_value();
            gate.wait();
        }

        return {};
    });
    ScreenshotTransfers transfers(worker);
    assert(transfers.begin(1, 500, metadata()).transferId == 1);
    assert(started.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    for (std::size_t i = 1; i < DiskWorker::maxJobs; ++i) {
        assert(transfers.write(1, "x") == accepted);
    }

    assert(transfers.begin(2, 1, metadata()).status == full);
    assert(transfers.commit(1) == full);
    assert(transfers.write(1, "x") == full);
    transfers.cancelClient(1);
    transfers.cancelClient(1);
    assert(transfers.write(1, "x") == invalid);
    assert(transfers.begin(1, 501, metadata()).status == invalid);
    release.set_value();

    for (std::size_t i = 0; i < DiskWorker::maxJobs; ++i) {
        assert(!completion(transfers, worker).released);
    }

    assert(completion(transfers, worker).released);
    const auto next = transfers.begin(1, 501, metadata());
    assert(next.status == accepted && next.transferId == 2);
    assert(transfers.commit(next.transferId) == accepted);
    assert(!completion(transfers, worker).released);
    assert(completion(transfers, worker).released);
    assert(!transfers.takeCompletion());
}

void boundedRetirement(bool failCleanup) {
    DiskWorker worker([=](const DiskJob& job) {
        return failCleanup && job.operation == DiskOperation::Cancel
                   ? std::make_error_code(std::errc::permission_denied)
                   : std::error_code{};
    });
    ScreenshotTransfers transfers(worker);

    for (std::uint64_t i = 1; i <= ScreenshotTransfers::maxTransfers; ++i) {
        assert(transfers.begin(i, 1, metadata()).status == accepted);
        transfers.cancelClient(i);
    }

    assert(transfers.begin(1000, 1, metadata()).status == full);
    assert(!completion(transfers, worker).released);
    assert(transfers.begin(1000, 1, metadata()).status == full);

    for (std::size_t i = 1; i < 2 * ScreenshotTransfers::maxTransfers; ++i) {
        const auto result = completion(transfers, worker);
        const bool cancel = result.disk.operation == DiskOperation::Cancel;
        assert(result.released == (cancel && !failCleanup));
        assert(result.firstFailure == (cancel && failCleanup));
    }

    transfers.cancelClient(1);
    assert(!transfers.takeCompletion());
    const auto next = transfers.begin(1000, 1, metadata());
    assert(next.status == (failCleanup ? full : accepted));

    if (!failCleanup) {
        assert(next.transferId == ScreenshotTransfers::maxTransfers + 1);
        transfers.cancelClient(1000);
        assert(!completion(transfers, worker).released);
        assert(completion(transfers, worker).released);
    }
}

int main() {
    char path[] = "/tmp/mouserat-transfers-XXXXXX";
    assert(mkdtemp(path));
    lifecycle(path);
    std::filesystem::remove_all(path);
    saturatedQueue();
    boundedRetirement(false);
    boundedRetirement(true);
}
