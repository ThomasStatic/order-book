# Checkpoint format

A checkpoint is one book's image at that book's own log position ([ADR 0009](adr/0009-a-checkpoint-is-one-books-image-at-its-own-log-position.md)). This document specifies the checkpoint file and the book image it wraps byte for byte, so either can be read without this code. The two are layers with their own versions: the matching library owns the image and judges it alone (`OrderBook::restore`), and the runtime owns the file around it (`encodeCheckpoint`, `decodeCheckpoint` in `runtime/checkpoint.hpp`).

## Checkpoint file

One file holds one instrument's book. Everything is little-endian.

### Header, 64 bytes

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic, `OBCKPT\r\n` (`4F 42 43 4B 50 54 0D 0A`), so a text-mode copy fails at once |
| 8 | 4 | format version (1) |
| 12 | 4 | minimum reader version (1) |
| 16 | 4 | header bytes (64) |
| 20 | 2 | writing engine's major version |
| 22 | 2 | minor version |
| 24 | 2 | patch version |
| 26 | 2 | reserved, 0 |
| 28 | 4 | shard count, never 0 |
| 32 | 4 | shard index, below the shard count |
| 36 | 4 | reserved, 0 |
| 40 | 8 | log position |
| 48 | 8 | symbol: 1 to 8 of `A-Z`, `0-9`, `.`, `-`, right-padded with spaces, as in the [command log](command-log-format.md) |
| 56 | 8 | image bytes |

Then, at header bytes:

1. the book image, image bytes long;
2. zero or more sections, each a `u32` type, a `u32` reserved 0, a `u64` length, and that many bytes (format 1 writes none);
3. an XXH64 (seed 0, `u64`) of every byte before it, the [command log](command-log-format.md#checksum)'s hash.

So a file `encodeCheckpoint` writes, format 1 with no section, is exactly 64 + image bytes + 8 long. A reader still accepts sections in any file, format 1 included, and skips them.

- **Position.** Every command for this symbol before the position is in the image, and none at or after it, counted as the shard's log counts them ([position](command-log-format.md#position)).
- **Shard.** A position means something only in its own shard's log, so the file records the placement it was written under. Comparing it with the current placement shows a change, so no placement table is needed, and a file stands alone: after a clean stop with a new placement, any shard can restore it.
- **Engine version.** Recorded, not enforced.

### Extension

As in the image, with the file's own version pair. The header can gain trailing fields, which its stated size lets an older reader skip: the image starts at the stated header bytes, whatever lies between. A field an older reader must not ignore raises the minimum reader version, so that reader refuses the file rather than rebuild the wrong book. A section of a type the reader doesn't know is skipped by its length; format 1 knows none, so it skips every section. `decodeCheckpoint` reads any format version whose minimum reader version is 1, and `encodeCheckpoint` writes format 1 at 64 header bytes with no section.

### Decoding

`decodeCheckpoint` checks the wrapper and returns the header and a view of the image. It checks the image's length but not its bytes, which `restore` judges. Each refusal has a code (an open set: codes are only appended) and the offset of the first byte of the field at fault. The checks run in this order, so a file with several faults reports the first:

1. The magic.
2. The length: fewer than 72 bytes, a header and a checksum.
3. The checksum, before any field is trusted, so a truncation and a bad length field fail the same check.
4. The versions, then the stated header size, then the lengths: the header, the image, and each section in turn, which together must end exactly at the checksum.
5. The header's fields by offset: reserved (26), shard count (28), shard index (32), reserved (36), symbol (48); then each section's reserved word, in file order.

| Code | Offset | Refused |
|---|---|---|
| `BAD_MAGIC` | 0 | first 8 bytes other than the magic, or fewer than 8 bytes |
| `TOO_SHORT` | the file's length | fewer than 72 bytes |
| `CHECKSUM_MISMATCH` | the checksum, at length − 8 | a checksum other than the XXH64 of the bytes before it |
| `UNSUPPORTED_VERSION` | 8 or 12 | a format version of 0 (8), or a minimum reader version other than 1 (12) |
| `SIZE_BELOW_KNOWN` | 16 | header bytes below 64 |
| `LENGTH_MISMATCH` | 16, 56, or a section's | header bytes past the checksum (16); an image past it (56); after the image, fewer than 16 bytes before the checksum (where they start), or a section whose length runs past it (its length field) |
| `NONZERO_RESERVED` | 26, 36, or a section's reserved word | reserved bytes that are not 0 |
| `INVALID_SHARD` | 28 or 32 | a shard count of 0 (28), or a shard index not below it (32) |
| `INVALID_SYMBOL` | 48 | symbol bytes that are not a symbol, all zeros among them |

### Verifying and describing

`verifyCheckpoint` decodes, then runs `restore` on the image: a file verifies if it would restore. A refusal from `restore` is reported with its own code and message, and its offset moved into the file by the image's start, the stated header bytes. `order_book_cli checkpoint verify <file>` prints `ok`, or exits 1 with `<path>: <offset>: <code>: <message>` on stderr.

`describeCheckpoint` reads both headers as stored and checks nothing past the magic, so a damaged file still describes; it refuses only `BAD_MAGIC`, and `TOO_SHORT` for fewer than 64 bytes. `order_book_cli checkpoint describe <file>` prints one `key: value` line per field: `file-bytes`, then the file header's fields (`format-version`, `minimum-reader-version`, `header-bytes`, `engine-version` as `major.minor.patch`, `reserved-at-26`, `shard-count`, `shard-index`, `reserved-at-36`, `position`, `symbol`, `image-bytes`; a symbol that is not one prints as `not a symbol:` and its 8 bytes in hex), then the image header's, read at the stated header bytes with the library's `readImageHeader` (`image-version`, `image-minimum-reader-version`, `image-header-bytes`, `level-record-bytes`, `order-record-bytes`, `stop-record-bytes`, `next-sequence-number`, `next-trade-id`, `last-trade-price`, `bid-levels`, `ask-levels`, `orders`, `stops`, `order-index-size`, `stop-index-size`, `image-reserved`), or `image-header: past the end of the file` if the file holds no 64 bytes there, and last `checksum: not checked`. Neither command takes a lock, since a published checkpoint never changes.

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

`tests/checkpoint/files/` holds checkpoint files the same script writes around those images, with the command log generator's hand-written XXH64: `resting`, `empty`, and `maximal`, which `encodeCheckpoint` must reproduce; `extended`, a format 2 file with a 72-byte header and two unknown sections, which this reader must still decode; and `bad_checksum`, `bad_image`, and `short` for the CLI's refusals.
