#pragma once

#include <cstddef>
#include <cstdint>

namespace order_book::runtime {

    // What a released response promises about its command (ADR 0013).
    enum class Durability : std::uint8_t {
        MACHINE,  // written through to the drive: FILE_FLAG_WRITE_THROUGH, or O_DSYNC
        PROCESS,  // handed to the system, and flushed when the file closes
    };

    // A shard's log ring: the most commands that can wait for its log writer, and so the most
    // that can be unsafe at once.
    inline constexpr std::size_t defaultLogRingRecords = 65536;

}  // namespace order_book::runtime
