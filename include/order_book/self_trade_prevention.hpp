#pragma once

namespace order_book {
    // What happens when an incoming order would trade with a resting order of the same nonzero
    // owner (FIX SelfMatchPreventionInstruction(2964), with this book's own values, not FIX's
    // wire values). The incoming order's mode decides; a resting order's is not kept.
    enum class SelfTradePrevention {
        CANCEL_RESTING = 0,   // remove the resting order whole; the incoming order keeps matching
        CANCEL_INCOMING = 1,  // leave the resting order; the incoming order stops, and its remainder expires
        CANCEL_BOTH = 2       // remove the resting order whole; the incoming remainder expires
    };

    // False for any value outside the enumerators, e.g. one made by a bad cast.
    constexpr bool isValidSelfTradePrevention(SelfTradePrevention mode) {
        return mode == SelfTradePrevention::CANCEL_RESTING || mode == SelfTradePrevention::CANCEL_INCOMING ||
               mode == SelfTradePrevention::CANCEL_BOTH;
    }
}  // namespace order_book
