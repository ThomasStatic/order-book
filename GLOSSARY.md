# Order book

A limit order book and matching engine: it accepts orders, matches them by price-time priority, and reports what happened as events.

## Orders

**Order request**:
What a caller submits to the book: the order's identity, side, size, price, and the instructions that say how it may trade.
_Avoid_: new order, submission (a submission is the act of sending one)

**Resting order**:
An order the book holds at a price level, waiting to be matched. Only limit orders rest.
_Avoid_: open order, working order, live order

**Order type**:
Whether an order has a limit price and whether it waits for a trigger: limit, market, stop, or stop-limit (FIX OrdType).
_Avoid_: kind

**Limit order**:
An order that trades only at its limit price or better.

**Market order**:
An order with no limit price, which takes the best prices available. It never rests: whatever it cannot trade at once expires.

**Stop order**:
An order that waits until the market trades at its stop price, then becomes a market order.
_Avoid_: stop-loss, stop-market

**Stop-limit order**:
An order that waits until the market trades at its stop price, then becomes a limit order at its limit price.

**Stop price**:
The price at which a stop or stop-limit order triggers (FIX StopPx). Distinct from the limit price.
_Avoid_: trigger price, stop limit

**Time in force**:
How long an order may remain working: good-till-cancelled, immediate-or-cancel, or fill-or-kill (FIX TimeInForce).
_Avoid_: validity, duration

**Good-till-cancelled (GTC)**:
An order whose unfilled remainder rests until it is filled or cancelled. Every resting order is GTC.

**Immediate-or-cancel (IOC)**:
An order that trades what it can at once; the rest expires.
_Avoid_: fill-and-kill, FAK

**Fill-or-kill (FOK)**:
An order that trades its whole quantity at once or not at all, and then its whole quantity expires.
_Avoid_: all-or-none

**Expire**:
Said of an accepted order's unfilled remainder that the book drops instead of resting, because the order may not wait, or because resting it would mean taking liquidity it may not take. Expiring is not cancelling: nobody asked for it, and the remainder never rested.
_Avoid_: cancel, kill, eliminate

**Post-only**:
An instruction that the order may only add liquidity, never take it (FIX ExecInst "participate, don't initiate"). A post-only order that would take liquidity on arrival expires whole; it is never repriced.
_Avoid_: maker-only, book-or-cancel, add-liquidity-only

**Owner**:
Whoever an order belongs to, for self-trade prevention only: two orders with the same owner may not trade with each other. An order may have no owner, and then it can trade with anyone. The book gives the owner no other meaning.
_Avoid_: account, trader, firm, participant

**Malformed**:
Said of an order or order request whose fields, alone or in combination, could never be accepted, whatever the book holds.
_Avoid_: invalid (too broad: a well-formed order can still be rejected)
