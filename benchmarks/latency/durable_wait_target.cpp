#include "durable_wait_target.hpp"

#include "order_book/runtime/log_files.hpp"

#include <algorithm>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace order_book::latency {

namespace {

std::vector<runtime::Instrument> oneInstrument() {
    return {runtime::Instrument{benchmark::instrumentSymbol(0), 0, std::nullopt, std::nullopt, std::nullopt}};
}

// Every record of the files in directory, in the order their names give, which is their first
// positions'.
std::vector<replay::ReplayRecord> readLog(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> files;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".oblog") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    std::vector<replay::ReplayRecord> records;
    for (const std::filesystem::path& path : files) {
        std::ifstream input(path, std::ios::binary);
        replay::CommandLogReader reader(input);
        for (;;) {
            const replay::ReadResult next = reader.next();
            if (const auto* record = std::get_if<replay::ReplayRecord>(&next)) {
                records.push_back(*record);
            } else if (const auto* error = std::get_if<replay::ParseError>(&next)) {
                throw std::runtime_error("durable wait: " + path.string() + " doesn't read back: " + error->message);
            } else {
                break;
            }
        }
    }
    return records;
}

}  // namespace

DurableWaitTarget::DurableWaitTarget(const benchmark::Workload& workload, benchmark::RunDurability durability,
                                     const std::filesystem::path& logParent, std::function<void()> setUpWriter,
                                     benchmark::LogFileFill fill, bool rollWorker, std::optional<std::uint64_t> fileRecords)
    : records_(benchmark::workloadRecords(workload, benchmark::instrumentSymbol(0))), directory_(oneInstrument(), 1) {
    std::optional<benchmark::RunLogWorker> worker;
    rollWorker_ = rollWorker;
    if (rollWorker) {
        // No preload goes in the log, so the book starts empty: the timed records' cancels of
        // preloaded orders refuse, and submits rest rather than match, so a pass costs less than
        // one over the engine's own books would, which the run record says.
        OrderBook empty;
        std::vector<std::byte> image(empty.imageSize());
        image.resize(empty.writeImage(image));
        worker = benchmark::RunLogWorker{{{benchmark::instrumentSymbol(0), std::move(image)}}};
    }
    // Housekeeping and the worker go where the writer does only with a worker, so a row without
    // one places its threads as it always has.
    std::function<void()> setUpHelpers = rollWorker ? setUpWriter : std::function<void()>{};
    const std::uint64_t fileBytes =
        fileRecords ? runtime::leastLogFileBytes(directory_) - replay::logRecordBytes + *fileRecords * replay::logRecordBytes
                    : runtime::defaultLogFileBytes;
    log_ = std::make_unique<benchmark::RunLog>(logParent, durability, directory_, ring_, 0, std::move(setUpWriter), fill, 0, std::move(worker),
                                               fileBytes, std::move(setUpHelpers));
}

DurableWaitTarget::~DurableWaitTarget() {
    if (log_) {
        // This thread is the ring's one producer, so it sends the writer its STOP.
        ring_.sendStop();
    }
}

void DurableWaitTarget::checkOutcome() {
    ring_.sendStop();
    const std::uint64_t end = log_->finish();
    passes_ = log_->rollPasses();
    const std::vector<replay::ReplayRecord> logged = readLog(log_->directory());
    log_.reset();
    // A worker's rolls let retention delete the files no checkpoint needs, so then the files left
    // hold the last of the records sent; without one, every record.
    const bool everyRecord = !rollWorker_;
    if (end != sent_ || logged.size() > sent_ || (everyRecord && logged.size() != sent_) || logged.empty()) {
        throw std::runtime_error("durable wait: " + std::to_string(sent_) + " records sent, but the log ended at " + std::to_string(end) +
                                 " and its files hold " + std::to_string(logged.size()));
    }
    const std::size_t first = sent_ - logged.size();
    for (std::size_t i = 0; i < logged.size(); ++i) {
        if (logged[i].symbol != records_[first + i].symbol || logged[i].command != records_[first + i].command) {
            throw std::runtime_error("durable wait: the log's record " + std::to_string(first + i) + " isn't the one sent");
        }
    }
}

}  // namespace order_book::latency
