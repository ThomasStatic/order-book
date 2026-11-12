# The browser's book view comes from shadow books

The page's ladder shows each book whole: its levels, whose orders rest there, its pending stops, and its last trade price. The wire protocol has no frame that asks for a book, and the public `OrderBook` API can't list owners or stops. So the engine worker keeps a shadow book per instrument: every log record already passes through it on the way to the page store (ADR 0017), and it runs each one again on a book of its own. The engine is deterministic, so the shadow equals the shard's book. And since an answer is released only after its log write is posted, the shadow already holds every command the page has seen answered. Owners and stops come from decoding the shadow's book image (`writeImage`, whose byte layout ADR 0009 fixes) inside `order_book_wasm`, so no public header changes. At each start the shadows are rebuilt from the store, as `verify` reads it.

## Considered options

- **A book-query frame in the wire protocol.** The view would be the shard's own book, but the protocol would change for a demo, and the matching thread would have to stop and copy its book on request, which the native engine never needs.
- **Replaying the whole log on every view.** No state to keep, but each click on the ladder would cost the whole session again, and the view would have to read checkpoints as recovery does.
- **Tracking the book in TypeScript from the events.** A second matching engine, which nothing in this repository would test.

## Consequences

- Every book is held twice in the tab, which counts against its memory along with the store's two copies.
- The page asks for a book and gets a snapshot back: nothing is pushed.
- The shadows also give each instrument's shard and each shard's record count, for the crash strip, and the highest order ID seen, from which the engine worker assigns the panels' IDs.
- A shadow that differs from its shard's book is a determinism bug, so tests can compare the two.
