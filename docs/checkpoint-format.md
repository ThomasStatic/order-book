# Checkpoint format

A checkpoint is one book's image at that book's own log position ([ADR 0009](adr/0009-a-checkpoint-is-one-books-image-at-its-own-log-position.md)). This document specifies the book image byte for byte, so it can be read without this code. The file that wraps an image, with its symbol, position, and checksum, comes with roll-forward and is not specified yet.

## Book image

The bytes `OrderBook::writeImage` writes and `OrderBook::restore` reads: everything needed to rebuild a book exactly, and nothing derived from its live structures but two index sizes. It holds no indexes, level handles, pool, or capacity, no triggered stops (there are none between commands), and no clock or random value, so the same book gives the same bytes on every platform. An image carries no checksum: the wrapper checks one, then restores.

Everything is little-endian. Byte values are defined here, not taken from any C++ enumerator.

### Header, 64 bytes

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | image version (1) |
| 4 | 4 | minimum reader version (1) |
| 8 | 4 | header bytes (64) |
| 12 | 4 | level record bytes (16) |
| 16 | 4 | order record bytes (24) |
| 20 | 4 | stop record bytes (32) |
| 24 | 4 | next sequence number, never 0 |
| 28 | 4 | next trade ID, never 0 |
| 32 | 4 | last trade price, 0 for none |
| 36 | 4 | bid levels |
| 40 | 4 | ask levels |
| 44 | 4 | resting orders written |
| 48 | 4 | pending stops written |
| 52 | 4 | order index size |
| 56 | 4 | pending-stop index size |
| 60 | 4 | reserved, 0 |

Then, at header bytes:

1. each bid level, best (highest) price first, followed at once by its orders in queue order;
2. each ask level, best (lowest) price first, the same way;
3. the buy stops, by ascending stop price, then the sell stops, by descending stop price, each stop price's stops in queue order.

So an image is exactly header bytes + level bytes × levels + order bytes × orders + stop bytes × stops long. A level's side is its section's, and an order's price and side are its level's. Stops have no level records: a run of stops with one side and stop price is one stop level.

The two index sizes are what the book's indexes held, written beside the record counts so a missing or stale index entry that changes a count is caught when the image is restored. In a sound book each equals its count.

### Level record, 16 bytes

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | price |
| 4 | 4 | order count, the order records that follow |
| 8 | 8 | aggregate: the sum of those orders' remaining quantities |

### Order record, 24 bytes

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | order ID |
| 4 | 4 | sequence number |
| 8 | 4 | initial quantity |
| 12 | 4 | remaining quantity |
| 16 | 4 | owner, 0 for none |
| 20 | 1 | fill status: 1 `NONE`, 2 `PARTIAL`, 3 `FILLED` |
| 21 | 3 | reserved, 0 |

Every resting order is a `LIMIT` order, so the type is not stored. A resting order's self-trade prevention mode is not book state, since the incoming order's mode decides, so it is not stored either. The fill status is stored rather than derived, because `Order::setFillStatus` lets it disagree with the quantities. `FILLED` has a value so a broken book's image says what it held, and restore refuses it.

### Stop record, 32 bytes

| Offset | Size | Field | Values |
|---|---|---|---|
| 0 | 4 | order ID | |
| 4 | 4 | sequence number | |
| 8 | 4 | stop price | |
| 12 | 4 | limit price | 0 for a `STOP`, which has none |
| 16 | 4 | quantity | |
| 20 | 4 | owner | 0 for none |
| 24 | 1 | side | 1 `BUY`, 2 `SELL` |
| 25 | 1 | order type | 1 `LIMIT`, 2 `MARKET`, 3 `STOP`, 4 `STOP_LIMIT` |
| 26 | 1 | time in force | 1 `GTC`, 2 `IOC`, 3 `FOK` |
| 27 | 1 | self-trade prevention | 1 `CANCEL_RESTING`, 2 `CANCEL_INCOMING`, 3 `CANCEL_BOTH`, written whatever the owner |
| 28 | 1 | post-only | 0 no, 1 yes |
| 29 | 3 | reserved, 0 | |

A stop record is the request the stop was accepted with, every field as submitted, so `findPendingStop` gives the same request after a restore. The values are the [command log](command-log-format.md)'s, except that self-trade prevention is never 0 here, since a pending stop keeps its mode even with no owner.

### Canonical order

Records are never sorted by sequence number, because numbers wrap: a record's place is its queue position. Levels are strictly best first, stops are in trigger order, and nothing else varies, so `writeImage(restore(x)) == x` byte for byte for every version 1 image restore accepts with this version's sizes.

### Extension

A header or record can gain trailing fields, which its stated size lets an older reader skip: restore reads each record at its stated stride and ignores what follows the fields it knows. A field an older reader must not ignore raises the minimum reader version, so that reader refuses the image rather than rebuild the wrong book. Restore reads any image version whose minimum reader version is 1. Only version 1 images at this version's sizes round-trip byte for byte, since `writeImage` writes version 1 at these sizes.

### Restore

`OrderBook::restore` runs no command, makes no event, and never matches, so a crossing image is refused rather than traded. It treats its input as untrusted, even behind a checksum that passed. It returns a `RestoreError` as a value for a bad image, never aborting, and lets `std::bad_alloc` propagate. The restored book's indexes are sized for the image, and it reports no capacity growths.

Each refusal has a code (an open set: codes are only appended) and the offset of the first byte of the field at fault. The checks run in this order, so an image with several faults reports the first:

1. The header: its size, versions, stated sizes, and length, then its own fields.
2. Every record, in the order the image holds them, each field in the order of its table.
3. Building the book: duplicate IDs, which need the indexes.
4. `checkInvariants`, in every build, whatever `ORDER_BOOK_INVARIANT_CHECKS` says. It covers crossing, FIFO order by sequence number, malformed orders and stops (a zero price, quantity, or sequence number among them), stop levels whose stops are not their side's or price's, and stops past their trigger.

| Code | Offset | Refused |
|---|---|---|
| `TOO_SHORT` | the image's length | fewer bytes than 64, or than the stated header bytes |
| `UNSUPPORTED_VERSION` | 0 or 4 | an image version of 0, a minimum reader version other than 1, or an image version below its minimum reader version (offset 0 for the image version, 4 otherwise) |
| `SIZE_BELOW_KNOWN` | 8, 12, 16, or 20 | a stated header or record size below this version's |
| `LENGTH_MISMATCH` | the image's length | a length other than the counts give, trailing bytes included |
| `ZERO_FIELD` | the field | a next sequence number or next trade ID of 0 (24, 28), or an order or stop ID of 0, which no index can hold |
| `NONZERO_RESERVED` | the reserved bytes | reserved bytes that are not 0, in the header (60) or a record |
| `INDEX_SIZE_MISMATCH` | 52 or 56 | an index size other than its record count |
| `EMPTY_LEVEL` | the level's order count | a level with no orders |
| `ORDER_COUNT_MISMATCH` | the level's order count, or the byte after the last level's orders | levels whose order counts reach past the orders written, or fall short of them |
| `LEVELS_OUT_OF_ORDER` | the level's price | a bid level not below the one before it, or an ask level not above |
| `AGGREGATE_MISMATCH` | the level's aggregate | an aggregate other than its orders' remaining sum |
| `INVALID_ENUMERATOR` | the byte | a fill status other than `NONE` or `PARTIAL`, or a side, order type, time in force, self-trade prevention, or post-only byte outside its values |
| `REMAINING_ABOVE_INITIAL` | the remaining quantity | an order with more remaining than its initial quantity, which no invariant looks at |
| `STOPS_OUT_OF_ORDER` | the stop's side or stop price | a buy stop after a sell stop, a buy stop below the one before it, or a sell stop above |
| `DUPLICATE_ID` | the second ID | an ID resting twice, pending twice, or both resting and pending |
| `INVARIANT_VIOLATION` | 0 | the rebuilt book fails `checkInvariants`; the error carries its `InvariantResult` |

A record's fields are checked in this order: a level's order count, then its price, then each of its orders, then its aggregate, once its orders are read; an order's ID, its fill status, its reserved bytes, then its quantities; a stop's ID, its side, its stop price, its enumerator bytes, then its reserved bytes.

### Golden samples

`tests/checkpoint/images/` holds images written by `tests/checkpoint/make_golden.py` from hand-listed records, with no line of the library's code, and committed byte-exact (`-text`). `checkpoint_tests` builds each sample's book through the public API and requires `writeImage` to give those bytes, so CI on Linux and Windows proves the format is the same on both.
