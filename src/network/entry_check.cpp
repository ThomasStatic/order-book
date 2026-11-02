#include "order_book/network/entry_check.hpp"

#include <optional>
#include <span>

namespace order_book::network {
    namespace {
        // Past every enumerator, so the book's own check names it.
        template <typename Enumeration>
        constexpr Enumeration unknownValue() noexcept {
            return static_cast<Enumeration>(99);
        }

        Side sideOf(std::uint8_t byte) noexcept {
            return byte == 1 ? Side::BUY : byte == 2 ? Side::SELL : unknownValue<Side>();
        }

        OrderType orderTypeOf(std::uint8_t byte) noexcept {
            switch (byte) {
                case 1: return OrderType::LIMIT;
                case 2: return OrderType::MARKET;
                case 3: return OrderType::STOP;
                case 4: return OrderType::STOP_LIMIT;
                default: return unknownValue<OrderType>();
            }
        }

        TimeInForce timeInForceOf(std::uint8_t byte) noexcept {
            switch (byte) {
                case 1: return TimeInForce::GTC;
                case 2: return TimeInForce::IOC;
                case 3: return TimeInForce::FOK;
                default: return unknownValue<TimeInForce>();
            }
        }

        // Absent means the default, with or without an owner; an unknown byte is refused either way.
        SelfTradePrevention modeOf(std::uint8_t byte) noexcept {
            switch (byte) {
                case 0:
                case 1: return SelfTradePrevention::CANCEL_RESTING;
                case 2: return SelfTradePrevention::CANCEL_INCOMING;
                case 3: return SelfTradePrevention::CANCEL_BOTH;
                default: return unknownValue<SelfTradePrevention>();
            }
        }

        // An instruction with no owner has nothing to protect, so it clears to the default, which
        // a log writes as absent.
        SelfTradePrevention clearedWithoutOwner(SelfTradePrevention mode, OwnerId owner) noexcept {
            return owner == 0 && isValidSelfTradePrevention(mode) ? SelfTradePrevention::CANCEL_RESTING : mode;
        }

        bool isPostOnly(const replay::RawCommand& raw) noexcept {
            return (raw.flags & replay::rawFlagPostOnly) != 0;
        }

        replay::AddCommand addCommandOf(const replay::RawCommand& raw) noexcept {
            return replay::AddCommand{raw.orderId,
                                      sideOf(raw.side),
                                      raw.price,
                                      raw.quantity,
                                      orderTypeOf(raw.orderType),
                                      timeInForceOf(raw.timeInForce),
                                      raw.stopPrice,
                                      isPostOnly(raw),
                                      raw.owner,
                                      clearedWithoutOwner(modeOf(raw.selfTradePrevention), raw.owner)};
        }

        replay::ReplaceCommand replaceCommandOf(const replay::RawCommand& raw) noexcept {
            return replay::ReplaceCommand{raw.orderId,
                                          raw.price,
                                          raw.quantity,
                                          isPostOnly(raw),
                                          raw.owner,
                                          clearedWithoutOwner(modeOf(raw.selfTradePrevention), raw.owner)};
        }

        // A well-formed command's last check.
        EntryDecision admit(const Symbol& symbol, const runtime::BookLocation& location, const replay::Command& command,
                            bool stopping) {
            if (stopping) {
                return EntryRefused{RefusedCode::ENGINE_STOPPING};
            }
            return EntryAccepted{symbol, location, command};
        }
    }  // namespace

    EntryDecision checkEntry(const CommandFrame& frame, const runtime::InstrumentDirectory& directory, bool stopping) {
        const replay::RawCommand& raw = frame.command;
        const std::optional<Symbol> symbol = Symbol::fromBytes(std::span<const char, Symbol::size>(raw.symbol));
        const std::optional<runtime::BookLocation> location = symbol ? directory.find(*symbol) : std::nullopt;
        if (!location) {
            return EntryRefused{RefusedCode::UNKNOWN_INSTRUMENT};
        }

        if (raw.command == replay::rawCommandCancel) {
            if (raw.orderId == 0) {
                return EntryRejected{CancelRejected{raw.orderId, RejectionReason::ORDER_NOT_FOUND}};
            }
            return admit(*symbol, *location, replay::CancelCommand{raw.orderId}, stopping);
        }
        if (raw.command == replay::rawCommandReplace) {
            const replay::ReplaceCommand command = replaceCommandOf(raw);
            if (const std::optional<RejectionReason> reason = malformedReason(replay::toRequest(command))) {
                return EntryRejected{ReplaceRejected{raw.orderId, *reason}};
            }
            return admit(*symbol, *location, command, stopping);
        }
        const replay::AddCommand command = addCommandOf(raw);
        if (const std::optional<RejectionReason> reason = malformedReason(replay::toRequest(command))) {
            return EntryRejected{OrderRejected{raw.orderId, *reason}};
        }
        return admit(*symbol, *location, command, stopping);
    }

}  // namespace order_book::network
