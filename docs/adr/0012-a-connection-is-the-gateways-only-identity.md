# A connection is the gateway's only identity

A TCP connection is all the identity the gateway has: there is no session, no login, and nothing that outlives a reconnect. Clients supply owners and order IDs unchanged, and are trusted. Each connection hears only its own commands' events, so a resting order's owner gets no fill report when another connection's order trades with it. Closing a connection does nothing to its resting orders, and nothing is resent. So the command log, the book, and the matching thread never learn that connections exist, and a CSV streamed through the gateway reaches the book exactly as written, which is what lets an end-to-end run be compared byte for byte with in-process replay.

## Considered options

- **A session that survives reconnects**, as SoupBinTCP and iLink 3 have. Would need a numbered response stream kept for replay, and a log or a store to hold it, to recover something a client here has no use for: the one client is a command that streams a file.
- **Mapping a connection to an owner.** Would make self-trade prevention work without the client's help, but a reconnect would change the owner, and a CSV's owners would be overwritten, so a gateway run could no longer match a replay.
- **Scoping order IDs by connection**, or having the engine assign them. Stops one client colliding with or cancelling another's orders, but needs a translation table in the gateway and changes the IDs the book and the log see. With no authentication, it would protect nothing from a client that meant harm.
- **Reporting fills to the resting side**, as every order-entry venue surveyed does. Needs a table from each live order to its connection, which a restart loses and which grows with the book. It stays open as a fresh effort.
- **Cancel on disconnect.** Needs the gateway to track each connection's live orders and issue cancels, and turns a network failure into trading.
- **Idempotent resends**, as OUCH's UserRefNum gives. Needs a client ID in the log and a memory of IDs seen in the engine, for a recovery no client here performs.

## Consequences

- A connection cannot learn that its resting order traded, or that its stop triggered, unless its own command caused it, and it has no query to ask. A cascade is reported whole to the connection whose command caused it.
- Two connections using one live ID get `DUPLICATE_ORDER_ID`, and any connection may cancel or replace any order.
- After a lost connection a client cannot tell whether its last commands ran, as after a crash. Since IDs are reusable after a fill, resending an `ADD` blindly can trade twice. Only a refused command is safe to retry, because it certainly did not run. `docs/wire-protocol.md` must say so.
- Log records stay free of any client or connection field, as ADR 0010 left them. If idempotent resends ever return, they arrive by that ADR's extension rule.
