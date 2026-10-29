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
An order that waits until it triggers, then becomes a market order.
_Avoid_: stop-loss, stop-market

**Stop-limit order**:
An order that waits until it triggers, then becomes a limit order at its limit price. Its stop price and limit price may stand in any relation.

**Pending stop**:
A stop or stop-limit order the book holds until it triggers or is cancelled. It is not a resting order: it sits at no price level and has no place in time priority until it triggers. Its ID is live, as a resting order's is, so no other order may use it meanwhile.
_Avoid_: waiting stop, untriggered stop, resting stop

**Cascade**:
The stops a single command triggers and runs after the submitting order has finished, including those triggered by each other's trades. The command reports the cascade's events together with its own.

**Stop price**:
The price at which a stop or stop-limit order triggers (FIX StopPx). Distinct from the limit price.
_Avoid_: trigger price, stop limit

**Last trade price**:
The price of the most recent trade on this book. It is what stop and stop-limit orders trigger on. A prevented self-trade is not a trade and does not set it.
_Avoid_: last price, market price, reference price

**Trigger**:
Said of a stop or stop-limit order when a trade on this book is at or through its stop price: at or above it for a buy, at or below it for a sell.
_Avoid_: activate, elect, fire

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
Said of an accepted order's unfilled remainder that the book drops instead of resting, because the order may not wait, because resting it would mean taking liquidity it may not take, because self-trade prevention stopped it, or because it is a stop or stop-limit order that arrived already past its trigger. Expiring is not cancelling: no cancel was requested, and the remainder never rested since the order was accepted or last replaced.
_Avoid_: cancel, kill, eliminate

**Post-only**:
An instruction that the order may only add liquidity, never take it (FIX ExecInst "participate, don't initiate"). A post-only order that would take liquidity on arrival expires whole; it is never repriced.
_Avoid_: maker-only, book-or-cancel, add-liquidity-only

**Owner**:
Whoever an order belongs to, for self-trade prevention only: two orders with the same owner may not trade with each other. An order may have no owner, and then it can trade with anyone. The book gives the owner no other meaning.
_Avoid_: account, trader, firm, participant

**Self-trade prevention (STP)**:
Stopping an incoming order from trading with a resting order that has the same owner. The incoming order's instruction says which order gives way: the resting order is cancelled whole and the incoming order goes on matching, the incoming order's remainder expires, or both. A prevented match is not a trade.
_Avoid_: self-match prevention, SMP, wash-trade prevention

**Time priority**:
An order's place in the queue at its price level: among resting orders at one price, the one that has held its place longest trades first.
_Avoid_: queue priority, FIFO position

**Replace**:
Changing a resting order's price, quantity, or instructions in one command, under the same order ID (FIX cancel/replace). The quantity given is the new total, including what has already traded. The order keeps its time priority only when its price is unchanged and its quantity is not increased; otherwise it goes to the back of the queue at its new price, and may trade on the way, as if it had just arrived. Its side never changes.
_Avoid_: amend, modify, cancel-replace (except when citing FIX)

**Malformed**:
Said of an order or order request whose fields, alone or in combination, could never be accepted, whatever the book holds.
_Avoid_: invalid (too broad: a well-formed order can still be rejected)

## Books and records

**Instrument**:
One tradable thing, with its own book. Orders never cross instruments, and the book gives an instrument no other meaning: no tick size, lot size, or decimal price.
_Avoid_: product, security, market, symbol (a symbol names an instrument)

**Snapshot**:
A summary of a book's depth at one moment: its best levels on each side, with their aggregate quantities. It describes a book; it cannot rebuild one.
_Avoid_: checkpoint, dump

**Checkpoint**:
A complete image of a book's state at one moment, from which an identical book can be rebuilt.
_Avoid_: snapshot, dump, state image

**Command log**:
The sequence of commands a book or engine was given, recorded so that replaying it reproduces the same books and events. It records commands only, never their results.
_Avoid_: journal, event log, audit log

**Replay**:
Feeding a recorded sequence of commands to fresh books, which produces the same books and events as the original run.
_Avoid_: recovery (recovery is a replay that starts from a checkpoint)
