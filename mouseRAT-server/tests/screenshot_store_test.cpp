#include "ScreenshotStore.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iterator>
#include <poll.h>

namespace fs = std::filesystem;
std::atomic<int> writeBudget{-1};
std::atomic<bool> interruptWrite{true};
std::atomic<bool> failSync{false};

extern "C" ssize_t __real_write(int, const void*, std::size_t);
extern "C" int __real_fsync(int);

extern "C" ssize_t __wrap_write(int fd, const void* data, std::size_t size) {
    if (interruptWrite.exchange(false)) { errno = EINTR; return -1; }
    const auto budget = writeBudget.load();
    if (budget == 0) { errno = ENOSPC; return -1; }
    if (budget == -2) return 0;
    auto count = std::min(size, std::size_t{7});
    if (budget > 0) count = std::min(count, static_cast<std::size_t>(budget));
    const auto result = __real_write(fd, data, count);
    if (result > 0 && budget > 0) writeBudget.fetch_sub(static_cast<int>(result));
    return result;
}

extern "C" int __wrap_fsync(int fd) {
    if (failSync) { errno = ENOSPC; return -1; }
    return __real_fsync(fd);
}

const std::string png(
    "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52"
    "\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00\x1f\x15\xc4"
    "\x89\x00\x00\x00\x0d\x49\x44\x41\x54\x78\x9c\x63\xf8\xcf\xc0\xf0"
    "\x1f\x00\x05\x00\x01\xff\x89\x99\x3d\x1d\x00\x00\x00\x00\x49\x45"
    "\x4e\x44\xae\x42\x60\x82", 70);
const std::string bmp(
    "\x42\x4d\x3a\x00\x00\x00\x00\x00\x00\x00\x36\x00\x00\x00\x28\x00"
    "\x00\x00\x01\x00\x00\x00\x01\x00\x00\x00\x01\x00\x18\x00\x00\x00"
    "\x00\x00\x04\x00\x00\x00\x13\x0b\x00\x00\x13\x0b\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\xff\x00", 58);

std::string metadata(std::uint64_t total, std::uint32_t format = 1) {
    std::string data(12, '\0');
    for (std::size_t i = 0; i < 8; ++i) data[i] = static_cast<char>(total >> (56 - 8 * i));
    for (std::size_t i = 0; i < 4; ++i) data[8 + i] = static_cast<char>(format >> (24 - 8 * i));
    return data;
}

DiskCompletion completion(DiskWorker& worker) {
    pollfd event{worker.descriptor(), POLLIN, 0};
    assert(poll(&event, 1, 5000) == 1);
    const auto result = worker.takeCompletion();
    assert(result);
    return *result;
}

std::error_code run(DiskWorker& worker, DiskOperation operation, std::uint64_t id, std::string_view data = {}) {
    assert(worker.submit({operation, id, data}));
    const auto result = completion(worker);
    assert(result.transferId == id && result.operation == operation);
    return result.error;
}

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    assert(input);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void noPartialFiles(const fs::path& directory) {
    for (const auto& entry : fs::directory_iterator(directory)) assert(entry.path().extension() != ".part");
}

void lifecycle(const fs::path& directory) {
    ScreenshotStore store(directory);
    DiskWorker worker(std::ref(store));
    assert(!fs::exists(directory));
    assert(run(worker, DiskOperation::Begin, 0, metadata(png.size())));
    assert(!run(worker, DiskOperation::Begin, 1, metadata(png.size())));
    assert(!run(worker, DiskOperation::Write, 1, std::string_view(png).substr(0, 1)));
    assert(!run(worker, DiskOperation::Write, 1, std::string_view(png).substr(1, 2)));
    assert(!run(worker, DiskOperation::Begin, 2, metadata(bmp.size(), 2)));
    assert(!run(worker, DiskOperation::Write, 2, bmp));
    assert(!run(worker, DiskOperation::Commit, 2));
    assert(read(store.destination(2, protocol::ImageFormat::Bmp)) == bmp);
    assert(!run(worker, DiskOperation::Write, 1, std::string_view(png).substr(3)));
    const auto saved = store.destination(1, protocol::ImageFormat::Png);
    assert(!fs::exists(saved));
    assert(!run(worker, DiskOperation::Commit, 1));
    assert(read(saved) == png);

    assert(!run(worker, DiskOperation::Cancel, 2));
    assert(run(worker, DiskOperation::Begin, 2, metadata(bmp.size(), 2)));

    assert(!run(worker, DiskOperation::Begin, 3, metadata(png.size())));
    assert(!run(worker, DiskOperation::Write, 3, std::string_view(png).substr(0, 7)));
    assert(run(worker, DiskOperation::Commit, 3));
    assert(!run(worker, DiskOperation::Cancel, 3));
    assert(!run(worker, DiskOperation::Begin, 4, metadata(png.size())));
    assert(run(worker, DiskOperation::Write, 4, png + "x"));
    assert(!run(worker, DiskOperation::Cancel, 4));
    assert(!run(worker, DiskOperation::Begin, 5, metadata(png.size())));
    assert(!run(worker, DiskOperation::Write, 5, png));
    assert(!run(worker, DiskOperation::Cancel, 5));
    assert(!fs::exists(store.destination(5, protocol::ImageFormat::Png)));

    const auto existing = store.destination(6, protocol::ImageFormat::Png);
    std::ofstream(existing) << "original";
    assert(!run(worker, DiskOperation::Begin, 6, metadata(png.size())));
    assert(!run(worker, DiskOperation::Write, 6, png));
    assert(run(worker, DiskOperation::Commit, 6) == std::errc::file_exists);
    assert(read(existing) == "original");
    assert(!run(worker, DiskOperation::Cancel, 6));

    std::uint64_t id = 7;
    for (const auto& invalid : {metadata(0), metadata(7), metadata(33554433), metadata(70, 257), std::string("bad")}) {
        assert(run(worker, DiskOperation::Begin, id, invalid));
        assert(run(worker, DiskOperation::Commit, id));
        assert(!run(worker, DiskOperation::Cancel, id++));
    }
    assert(run(worker, DiskOperation::Write, 999, "unknown"));
    noPartialFiles(directory);
}

void badDirectory(const fs::path& path) {
    std::ofstream(path) << "original";
    ScreenshotStore store(path);
    DiskWorker worker(std::ref(store));
    const auto error = run(worker, DiskOperation::Begin, 1, metadata(png.size()));
    assert(error && run(worker, DiskOperation::Commit, 1) == error);
    assert(!run(worker, DiskOperation::Cancel, 1));
    assert(read(path) == "original");
}

void failures(const fs::path& directory) {
    ScreenshotStore store(directory);
    std::promise<void> committed;
    auto attempted = committed.get_future();
    DiskWorker worker([&](const DiskJob& job) {
        const auto result = store(job);
        if (job.operation == DiskOperation::Commit && job.transferId == 1) committed.set_value();
        return result;
    });
    assert(!run(worker, DiskOperation::Begin, 1, metadata(png.size())));
    writeBudget = 3;
    assert(worker.submit({DiskOperation::Write, 1, png}));
    assert(worker.submit({DiskOperation::Write, 1, "more"}));
    assert(worker.submit({DiskOperation::Commit, 1, {}}));
    assert(attempted.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    assert(!fs::exists(store.destination(1, protocol::ImageFormat::Png)));
    noPartialFiles(directory);
    for (int i = 0; i < 3; ++i) assert(completion(worker).error == std::errc::no_space_on_device);
    writeBudget = -1;
    assert(!run(worker, DiskOperation::Cancel, 1));

    assert(!run(worker, DiskOperation::Begin, 2, metadata(png.size())));
    assert(run(worker, DiskOperation::Write, 2, "notapng!") == std::errc::illegal_byte_sequence);
    assert(!run(worker, DiskOperation::Cancel, 2));
    assert(!run(worker, DiskOperation::Begin, 3, metadata(png.size())));
    writeBudget = -2;
    assert(run(worker, DiskOperation::Write, 3, png) == std::errc::io_error);
    writeBudget = -1;
    assert(!run(worker, DiskOperation::Cancel, 3));

    assert(!run(worker, DiskOperation::Begin, 4, metadata(png.size())));
    assert(!run(worker, DiskOperation::Write, 4, png));
    failSync = true;
    assert(run(worker, DiskOperation::Commit, 4) == std::errc::no_space_on_device);
    failSync = false;
    assert(!run(worker, DiskOperation::Cancel, 4));
    noPartialFiles(directory);
}

void boundedCleanup(const fs::path& directory) {
    {
        ScreenshotStore store(directory);
        DiskWorker worker(std::ref(store));
        for (std::uint64_t id = 1; id <= ScreenshotStore::maxTransfers; ++id) {
            assert(!run(worker, DiskOperation::Begin, id, metadata(png.size())));
        }
        assert(run(worker, DiskOperation::Begin, 65, metadata(png.size())) == std::errc::no_buffer_space);
        assert(!run(worker, DiskOperation::Cancel, 1));
        assert(!run(worker, DiskOperation::Begin, 66, metadata(png.size())));
    }
    assert(fs::is_empty(directory));
}

int main() {
    char path[] = "/tmp/mouserat-store-XXXXXX";
    assert(mkdtemp(path));
    const fs::path root(path);
    lifecycle(root / "lifecycle");
    failures(root / "failures");
    boundedCleanup(root / "bounded");
    badDirectory(root / "not-a-directory");
    fs::remove_all(root);
}
