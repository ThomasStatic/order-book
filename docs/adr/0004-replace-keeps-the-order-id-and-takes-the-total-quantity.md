# A replace keeps the order ID and takes the total quantity

`replaceOrder(ReplaceRequest)` names a resting order by its `OrderId`, and the order keeps that ID: there is no new ID for the replacement. Its `quantity` is the new total, including what has already traded, so the new open quantity is that total minus the executed quantity, and a total at or below the executed quantity is rejected (`REPLACE_QUANTITY_TOO_LOW`) rather than cancelling the order. FIX chains a new ClOrdID on every replace (OrigClOrdID names the last one), as CME and Nasdaq do, because over an asynchronous link a client must tell responses to the old and new requests apart. This book is a synchronous library call whose result answers exactly one request, so chaining would only add a second ID to every replace event and a duplicate-ID check, and split one order's history across IDs. The total-quantity rule, on the other hand, is what FIX, Nasdaq, Eurex, and CME (with in-flight mitigation) agree on, and `Order` already tracks the initial quantity it needs.

## Considered options

- **A new ID per replace** (FIX, CME, Nasdaq). Matches FIX's chaining, at the cost above; we use FIX vocabulary with our own documented semantics, and this is one of the places they differ.
- **Quantity as the new open (remaining) quantity.** Simpler for a synchronous caller, but it diverges from every source, and a caller translating FIX would have to convert.
- **Delete the order when the total is at or below the executed quantity** (Eurex). A replace would then double as a cancel; FIX says to use a cancel for that, and `cancelOrder` exists.

## Consequences

- Events and results of a replace carry one order ID, the same one the order was accepted under. A consumer that keys orders by ID needs no chain.
- The ID is never freed by a successful replace, except when the replacement fills completely or expires, as for any order leaving the book.
- After a replace the order's initial quantity is the new total, so a partially filled order stays `PARTIAL`, and executed plus open quantity still equals the initial quantity.
- A replace keeps time priority only when the price is unchanged and the total does not increase. Otherwise it loses it, takes a new sequence number, and matches if it crosses, as if it had just arrived.
