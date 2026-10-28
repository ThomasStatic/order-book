# Unfilled remainders expire, reported as OrderExpired

A market order's remainder never rests, and neither does an IOC order's. We accept such an order whenever it is well formed, trade what the book offers, and then expire whatever is left, including the whole quantity when the opposite side is empty. A new `OrderExpired` event reports the remainder, after the `OrderAccepted` and the `TradeExecuted`s. A submission is rejected only when it is malformed or its ID conflicts with a resting order, never because liquidity is missing. So one rule covers no fill, a partial fill, and a full fill, and a reader can tell a refused request from an accepted order that found nothing to trade against.

## Considered options

- **Reject a market order when the opposite side is empty** (CME Globex). This makes rejection depend on liquidity, and a market order facing one lot would behave differently from one facing none.
- **Report the remainder as `OrderCancelled`** (Eurex, Nasdaq). Here `OrderCancelled` means a resting order was removed by `cancelOrder`. An expired remainder never rested, and nobody cancelled it.
- **Rest the remainder, or convert it to a limit order** (Eurex, CME, FIX `OrdType=K`). These need a reference or protection price, and market-order price protection is out of scope.

## Consequences

- An accepted result's remaining quantity is the unfilled quantity, so traded plus remaining still equals the accepted quantity. For a `submitOrder(OrderRequest)` result, a remaining quantity above zero no longer means the order rests. Only a trailing `OrderExpired` says that it did not. Results from `submitOrder(Order)` are unchanged.
- `OrderExpired` is a new `BookEvent` alternative, so code that visits every alternative must handle it.
- An order that expires still consumes a sequence number, as any accepted submission does.
- A fill-or-kill order that cannot fill completely is handled the same way. It is accepted, makes no trades, and its whole quantity expires. It is not rejected, so FOK has no liquidity-dependent rejection either.
