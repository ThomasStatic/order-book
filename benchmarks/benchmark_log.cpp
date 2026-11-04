#include "benchmark_log.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace order_book::benchmark {

namespace {

// Distinguishes one run's directory from the next in this process; the clock tells processes apart.
std::atomic<std::uint64_t> runsMade{0};

}  // namespace

std::filesystem::path freshLogDirectory(const std::filesystem::path& parent) {
    const auto stamp = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::filesystem::path candidate =
            parent / ("order_book_benchmark_log_" + std::to_string(stamp) + "_" + std::to_string(runsMade.fetch_add(1)));
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error)) {
            return candidate;
        }
        if (error) {
            throw std::runtime_error("cannot make a log directory in " + parent.string() + ": " + error.message());
        }
    }
    throw std::runtime_error("cannot make a fresh log directory in " + parent.string());
}

const char* durabilityName(RunDurability durability) {
    switch (durability) {
        case RunDurability::NONE: return "none";
        case RunDurability::PROCESS: return "process";
        case RunDurability::MACHINE: return "machine";
    }
    return "none";
}

RunDurability parseDurability(std::string_view name) {
    if (name == "none") {
        return RunDurability::NONE;
    }
    if (name == "process") {
        return RunDurability::PROCESS;
    }
    if (name == "machine") {
        return RunDurability::MACHINE;
    }
    throw std::invalid_argument("unknown durability '" + std::string(name) + "' (known: none, process, machine)");
}

std::filesystem::path defaultLogParent() {
    return std::filesystem::temp_directory_path();
}

RunLog::RunLog(const std::filesystem::path& parent, RunDurability durability, const runtime::InstrumentDirectory& directory,
               LogRing& ring, std::uint64_t firstPosition, std::function<void()> setUpWriter)
    : log(ring) {
    if (durability == RunDurability::NONE) {
        throw std::invalid_argument("a run with no log has no log writer");
    }
    where = freshLogDirectory(parent);
    try {
        storage = std::make_unique<runtime::FileLogStorage>(where);
        housekeeping = std::make_unique<runtime::Housekeeping>(
            *storage, directory, durability == RunDurability::MACHINE ? runtime::Durability::MACHINE : runtime::Durability::PROCESS,
            runtime::defaultLogFileBytes);
        std::future<void> started = ready.get_future();
        writer = std::thread([this, firstPosition, setUp = std::move(setUpWriter)] {
            std::optional<runtime::LogWriter> made;
            try {
                if (setUp) {
                    setUp();
                }
                made.emplace(*housekeeping, 0, log.ring, log.durable, firstPosition);
            } catch (...) {
                ready.set_exception(std::current_exception());
                return;
            }
            ready.set_value();
            made->run(log.writerBell, runtime::defaultSpinWindow);
            logEnd = made->nextPosition();
        });
        try {
            started.get();
        } catch (...) {
            writer.join();
            throw;
        }
        // The next file too, here rather than on the housekeeping thread while the run is timed.
        housekeeping->pass();
        housekeeper = std::thread([this] { housekeeping->run(); });
    } catch (...) {
        // Nothing has been sent to the matching thread yet, so it isn't writing the log ring, and
        // this thread may stop the writer itself.
        if (writer.joinable()) {
            log.sendStop();
            writer.join();
        }
        if (housekeeping) {
            housekeeping->stop();
            housekeeping->removeUnused();
        }
        housekeeping.reset();
        storage.reset();
        std::error_code ignored;
        std::filesystem::remove_all(where, ignored);
        throw;
    }
}

std::uint64_t RunLog::finish() {
    if (!finished) {
        writer.join();
        housekeeping->stop();
        housekeeper.join();
        // Only once both threads are joined, so a failure here leaves the destructor to try again.
        finished = true;
        housekeeping->removeUnused();
    }
    return logEnd;
}

RunLog::~RunLog() {
    try {
        finish();
    } catch (const std::exception&) {  // a destructor can't report it, and the files go anyway
    }
    housekeeping.reset();
    storage.reset();
    std::error_code ignored;
    std::filesystem::remove_all(where, ignored);
}

}  // namespace order_book::benchmark
