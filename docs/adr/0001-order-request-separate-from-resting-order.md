# Order requests are a separate type from resting orders

The new order behaviors (market, IOC, FOK, post-only, stop and stop-limit) need attributes that matter only when an order is submitted: a time in force, a stop price, a post-only flag, and an order type other than limit. We put them on a new aggregate, `OrderRequest`, taken by a new `submitOrder(OrderRequest)` overload, and left `Order`, the resting record, exactly as in v1.0.0. `Order` is the element of every price level's queue, so each field added to it would make every resting node bigger. And only limit orders ever rest, so most of those fields would be dead weight on a resting order. Keeping them off `Order` keeps the hot path's memory footprint and v1's behavior unchanged.

## Considered options

- **Every attribute on `Order`.** One type and one place for field checks, but every resting node grows by fields that cannot matter once the order rests. Rejected for performance.
- **Attributes on `Order` publicly, with a slimmer internal record.** `findActiveOrder` returns `Order` copies, so the book would have to rebuild the dropped fields or store them anyway.

## Consequences

- An `OrderRequest` does not wrap an `Order`. `Order`'s constructor refuses price 0, which a market or stop request requires. The book builds an `Order` only when something rests.
- `Order` keeps accepting only `LIMIT`. `isSupportedOrderType` splits into "is an enumerator" (for requests) and "can rest" (for `Order`).
- There are two places for field checks. The per-field checks (ID, price, quantity, side) are written once and shared. The combination checks belong to the request.
- An attribute that the book must still see on a resting order (for example a self-trade-prevention owner) has to go on `Order` at a node-size cost, or into a side structure keyed by order ID.
