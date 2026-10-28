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
- A post-only order that would take liquidity on arrival is handled the same way. It is accepted, makes no trades, and its whole quantity expires. It is not rejected and not repriced.
- Self-trade prevention is a third reason a remainder expires. Under `CANCEL_INCOMING` or `CANCEL_BOTH`, the incoming order stops at its first self-match and its remainder expires, even on a GTC limit order, since resting it would cross its own order. A resting order that self-trade prevention removes is reported by a new `SelfTradePrevented` event, not `OrderCancelled`, which stays the report of `cancelOrder`, and not `OrderExpired`, since that order did rest.
- A stop or stop-limit order that arrives already past its trigger is handled the same way: a buy stop whose stop price is at or below the last trade price, or a sell stop whose stop price is at or above it. It is accepted, makes no trades, and its whole quantity expires. It is not rejected, as CME, Eurex, and Binance do, since that would make rejection depend on the book's trading history. It is not triggered at once either, since a misplaced stop would then trade at market straight away. Before the book's first trade there is no last trade price, so no stop arrives past its trigger.
- A triggered stop or stop-limit order is handled the same way once it has become its market or limit order. Its unfilled remainder expires under the time in force it carried while pending, and a stop-limit with fill-or-kill that cannot fill at trigger time is wholly expired, not rejected and not left pending. A `STOP` request must carry IOC or FOK, as a `MARKET` request must, since the market order it becomes cannot rest.
