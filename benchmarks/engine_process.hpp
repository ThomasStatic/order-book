#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "benchmark_log.hpp"
#include "order_book/order_book.hpp"
#include "workload.hpp"

namespace order_book::process_testing {
class ChildProcess;
}

// The shipped order_book_engine as a child process, for the engine benchmark kind and the
// round-trip latency interval, so both measure what ships and share no heap with the client.
// It writes an instrument list naming one book per workload, B0, B1, and so on, each at its
// workload's expected capacity, starts the engine on port 0 with --stop-on-stdin-close, logging
// to a fresh directory it deletes afterwards, and reads the port from its listening line.
namespace order_book::benchmark {

// order_book_engine beside the running program, whose argv[0] this takes.
std::filesystem::path engineBesideProgram(std::string_view programPath);

// What the instrument list declares for a workload's book: its expected capacity, with each part
// at least 1, since the list takes no 0.
BookCapacity declaredCapacity(const Workload& workload);

// What a clean stop's report says about one book.
struct EngineBookReport {
    std::string symbol;
    std::uint32_t shard = 0;
    BookCapacity capacity;
    std::uint64_t growths = 0;  // since ready
};

// The book lines of a stop report, in the report's order. Throws std::runtime_error if it has no
// "stopped" line or a book line it can't read.
std::vector<EngineBookReport> parseStopReport(std::string_view report);

// Settings for the child, beyond what the workloads decide.
struct EngineOptions {
    std::size_t shards = 1;  // book i goes on shard i modulo this, pinned in the instrument list
    // Passed to the engine as they are, for pinning and priority: --pin-matching, --pin-gateway,
    // and --thread-priority.
    std::vector<std::string> extraArguments;
    // The engine's --durability, which can't be NONE: the engine always logs.
    RunDurability durability = RunDurability::PROCESS;
    std::filesystem::path logParent = defaultLogParent();  // where its log directory is made
};

class EngineProcess {
public:
    // Writes the instrument list, starts the engine, and waits for its listening line. Throws
    // std::runtime_error, with what the engine wrote to stderr, if it doesn't start. The
    // workloads are read here only.
    EngineProcess(const std::filesystem::path& engine, const std::vector<Workload>& workloads, const EngineOptions& options = {});
    // Kills the engine if stop has not run, and removes the instrument list and the log.
    ~EngineProcess();
    EngineProcess(const EngineProcess&) = delete;
    EngineProcess& operator=(const EngineProcess&) = delete;

    std::uint16_t port() const noexcept { return listeningPort; }

    // Closes the engine's stdin, which starts its clean stop, and waits for it to exit. Throws
    // std::runtime_error if it doesn't exit 0 in time, or if a book's capacity is not what its
    // workload expected or it grew: the run's capacity was wrong.
    void stop();

private:
    std::filesystem::path instrumentList;
    std::filesystem::path logDirectory;
    std::unique_ptr<process_testing::ChildProcess> child;
    std::vector<BookCapacity> expected;
    std::uint16_t listeningPort = 0;
    bool stopped = false;
};

}  // namespace order_book::benchmark
