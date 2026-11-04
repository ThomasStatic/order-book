#include "durable_wait_target.hpp"

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
                                     const std::filesystem::path& logParent, std::function<void()> setUpWriter)
    : records_(benchmark::workloadRecords(workload, benchmark::instrumentSymbol(0))), directory_(oneInstrument(), 1) {
    log_ = std::make_unique<benchmark::RunLog>(logParent, durability, directory_, ring_, 0, std::move(setUpWriter));
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
    const std::vector<replay::ReplayRecord> logged = readLog(log_->directory());
    log_.reset();
    if (end != sent_ || logged.size() != sent_) {
        throw std::runtime_error("durable wait: " + std::to_string(sent_) + " records sent, but the log ended at " + std::to_string(end) +
                                 " and its files hold " + std::to_string(logged.size()));
    }
    for (std::size_t i = 0; i < logged.size(); ++i) {
        if (logged[i].symbol != records_[i].symbol || logged[i].command != records_[i].command) {
            throw std::runtime_error("durable wait: the log's record " + std::to_string(i) + " isn't the one sent");
        }
    }
}

}  // namespace order_book::latency
