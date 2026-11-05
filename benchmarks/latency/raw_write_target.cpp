#include "raw_write_target.hpp"

#include <algorithm>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>


namespace order_book::latency {

namespace {

constexpr const char* rawFileName = "raw-write.bin";
constexpr const char* aheadFileName = "raw-write-ahead.bin";

}  // namespace

void RawWriteTarget::fillRecord(std::array<std::byte, replay::logRecordBytes>& record, std::size_t index) {
    const auto value = static_cast<std::uint64_t>(index);
    for (std::size_t i = 0; i < record.size(); ++i) {
        record[i] = static_cast<std::byte>((value >> (8 * (i % 8))) & 0xFF);
    }
}

RawWriteTarget::RawWriteTarget(const benchmark::Workload& workload, benchmark::RunDurability durability,
                               const std::filesystem::path& logParent, benchmark::LogFileFill fill, std::uint64_t fileBytes) {
    if (durability == benchmark::RunDurability::NONE) {
        throw std::invalid_argument("a raw write loop writes a file, so it needs process or machine durability");
    }
    directory_ = benchmark::freshLogDirectory(logParent);
    try {
        storage_ = benchmark::runLogStorage(directory_, fill);
        file_ = storage_->create(rawFileName, durability == benchmark::RunDurability::MACHINE ? runtime::Durability::MACHINE
                                                                                              : runtime::Durability::PROCESS);
        const std::uint64_t needed = static_cast<std::uint64_t>(workload.timedCount()) * replay::logRecordBytes;
        file_->extend(std::max(needed, fileBytes));
        // A second file made the same way and left, as housekeeping makes a log's next file
        // before a run, so the drive has taken as much just before timing as under the durable
        // wait: half a GiB of zeros, zero-filled, leaves a drive busy into the run.
        const std::unique_ptr<runtime::LogFile> ahead =
            storage_->create(aheadFileName, durability == benchmark::RunDurability::MACHINE ? runtime::Durability::MACHINE
                                                                                           : runtime::Durability::PROCESS);
        ahead->extend(fileBytes);
        ahead->close();
    } catch (...) {
        file_.reset();
        storage_.reset();
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
        throw;
    }
}

RawWriteTarget::~RawWriteTarget() {
    try {
        if (file_) {
            file_->close();
        }
    } catch (const std::exception&) {  // a destructor can't report it, and the file goes anyway
    }
    file_.reset();
    storage_.reset();
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
}

void RawWriteTarget::checkOutcome() {
    std::vector<std::byte> bytes(written_ * record_.size());
    const std::size_t read = file_->read(0, bytes);
    file_->close();
    file_.reset();
    std::array<std::byte, replay::logRecordBytes> expected{};
    for (std::size_t i = 0; i < written_; ++i) {
        fillRecord(expected, i);
        if (read < (i + 1) * expected.size() ||
            !std::equal(expected.begin(), expected.end(), bytes.begin() + static_cast<std::ptrdiff_t>(i * expected.size()))) {
            throw std::runtime_error("raw write: the file's record " + std::to_string(i) + " isn't the one written");
        }
    }
}

}  // namespace order_book::latency
