#pragma once

namespace order_book {
    enum class FillStatus {
        NONE,
        PARTIAL,
        FILLED
    };
}  // namespace order_book