# Triggered stops run after the submitting order, in one result

A trade can trigger pending stops, and a triggered stop's own trades can trigger more. We run that cascade inside the command that made the first trade, after the submitting order has finished matching and its remainder has rested or expired. Each trade moves the stops it triggers into a triggered queue, ordered as if the price had moved one tick at a time (nearest stop price first, then acceptance order); the queue is then drained one stop at a time, each run to completion, with what it triggers appended. A triggered stop keeps its order ID but takes a fresh sequence number at trigger time, since a pending stop has no place in time priority. The command's one `SubmissionResult` reports everything in execution order: the submitting order's events, then for each triggered stop a `StopTriggered` event followed by that stop's trades, preventions, and expiry. `getGeneratedTrades()` holds every trade the command made.

## Considered options

- **Run a stop the instant a trade triggers it.** This interleaves two orders' matches, so a sweep would be interrupted partway and resumed against a changed book. Rejected: one incoming order matches at a time.
- **Rest the submitting order's remainder after the cascade.** A triggered stop-limit could rest first and the remainder then cross it. Rejected: the remainder rests first, and a triggered stop may trade against it.
- **Keep the sequence number from acceptance.** A stop that waited an hour would then rest ahead of every limit order accepted meanwhile. Rejected.
- **A separate result per triggered stop, or a result that holds only the submitted order's trades.** `getEvents()` would no longer be the full record of what a command did, and the trades a command made would be split across objects. Rejected.
- **A bound on cascade depth.** A stop leaves the pending set when it triggers and never re-enters, so a cascade already terminates; a cap would have to decide what happens to the stops it cut off. Rejected.

## Consequences

- `SubmissionResult::accepted` no longer requires every trade to be for the submitted order: a trade may be for an order whose `StopTriggered` precedes it. `remainingQuantity` and `FillStatus` describe the submitted order at the end of its own match, before the cascade, which may fill its rested remainder. `submitOrder(Order)` can carry a cascade too, since v1 trades trigger stops; a program that submits no stops never sees one.
- `StopTriggered` is a new `BookEvent` alternative, so code that visits every alternative must handle it. There is no second `OrderAccepted` and no resting event for a triggered stop.
- A triggered stop runs with its own owner and self-trade-prevention mode, and under its own time in force's atomicity contract: a market or IOC stop keeps v1's (executions made stand, the result is lost with the exception), a FOK stop is strongly atomic for its own match, walked against the book as it stands at trigger time.
- One v1 guarantee narrows. "A remainder never rests behind a throw" becomes: the submitted order's own lifecycle is complete (rested or expired) before anything in the cascade can throw. Stops triggered but not yet run when an exception escapes are discarded, as the unrested remainder is in v1; they have left the pending set, so no pending stop is ever past the last trade price, even after a throw. Moving a stop between the pending set and the triggered queue allocates nothing; only running one can.
- A cascade runs at most once per pending stop, so a command's worst case is every pending stop. Post-only is refused on every stop type.
