# Command log format

A command log is a file of commands: a header, then fixed 40-byte records, each one well-formed command and its symbol, with each record's checksum seeded from the one before. It holds exactly what a v3 [replay CSV](replay-format.md) can hold, so the two are interchangeable inputs. This document specifies a log byte for byte, so it can be read without this code. The reasons are in [ADR 0010](adr/0010-the-command-log-is-fixed-width-chained-records-of-well-formed-commands.md).

Everything is little-endian. Byte values are defined here, not taken from any C++ enumerator, and 0 always means absent, as an empty CSV field does.

## File header

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic, `OBCLOG\r\n`, so a text-mode copy fails at once |
| 8 | 4 | format version (1) |
| 12 | 4 | minimum reader version (1) |
| 16 | 4 | header bytes (64), the fixed part |
| 20 | 4 | record bytes (40) |
| 24 | 6 | writing engine's version: major, minor, patch, `u16` each; recorded, not enforced; 0.0.0 when no engine wrote it, as in a converted file |
| 30 | 2 | reserved, 0 |
| 32 | 8 | first position |
| 40 | 4 | shard count, 0 for no placement |
| 44 | 4 | shard index |
| 48 | 4 | symbol count, n |
| 52 | 12 | reserved, 0 |
| header bytes | 8 × n | this file's symbols, sorted |
| then | 8 | XXH64, seed 0, of everything before it |
| then | record bytes each | records |

There is no clock and no random ID, so the same commands give the same bytes.

**Placement.** The shard count, shard index, and symbols are the outcome of placement, not its recipe, so they name what moved. Replay and conversion ignore them, so the records still know nothing of shards.

## Record

| Offset | Size | Field | Values |
|---|---|---|---|
| 0 | 4 | checksum | see below |
| 4 | 1 | command | 1 `ADD`, 2 `CANCEL`, 3 `REPLACE`; 0 is never a record |
| 5 | 1 | side | 1 `BUY`, 2 `SELL`; 0 absent |
| 6 | 1 | order type | 1 `LIMIT`, 2 `MARKET`, 3 `STOP`, 4 `STOP_LIMIT`; 0 absent |
| 7 | 1 | time in force | 1 `GTC`, 2 `IOC`, 3 `FOK`; 0 absent |
| 8 | 8 | symbol | 1 to 8 of `A-Z`, `0-9`, `.`, `-`, right-padded with spaces |
| 16 | 4 | order ID | |
| 20 | 4 | price | 0 absent |
| 24 | 4 | quantity | 0 absent |
| 28 | 4 | stop price | 0 absent |
| 32 | 4 | owner | 0 no owner |
| 36 | 1 | flags | bit 0 post-only; every other bit must be zero |
| 37 | 1 | self-trade prevention | 1 `CANCEL_RESTING`, 2 `CANCEL_INCOMING`, 3 `CANCEL_BOTH`; 0 absent |
| 38 | 2 | reserved | must be zero |

Which fields a command must fill or leave absent is exactly v2's column table in [`replay-format.md`](replay-format.md), plus the required `symbol`, and a record is checked in a v3 row's order. So a decoded `ADD` is always a well-formed request.

A log holds well-formed commands only, with no timestamp or client ID. A rejection that depends on a book's state (a duplicate ID, an order not found, a total too low) is logged, so replay reproduces it. A malformed command is refused before it reaches the log. It consumes no number and changes no book, so nothing is lost.

## Checksum

A record's checksum is the low 32 bits of XXH64 over its bytes from offset 4 to its end, seeded with the previous record's stored checksum. The first record's seed is the low 32 bits of the header's checksum, which ties a file's records to its header.

So a dropped, duplicated, reordered, or stale record fails, which stands in for a sequence field. A reader starting mid-file needs only the preceding record's first 4 bytes, and a flipped bit fails at most two records, so later ones still verify.

## Position

A log position is the number of records in the log before this one, so the first record is position 0. Each file's header carries its first position, so a log may span files, a missing file shows as a gap, and a position's file offset is arithmetic.

A reader gives each record the values it has in the file's CSV twin: `sequence` is position + 1, and the line is the record's 0-based index in this file + 2. So a log and its twin print the same bytes, and a diagnostic's line finds the same command in the converted file.

## Extension

A file states its sizes and a minimum reader version, and a reader may skip bytes past what it knows only if they are all zero, so a new field must use zero to mean today's behavior. Every command field changes replay, so an old reader that skipped one would replay a different command without noticing.

| A later release adds | An old reader |
|---|---|
| A field, unused in this file | reads it normally |
| A field, used by a record | stops at that record with `UNKNOWN_FIELD`, keeping what came before |
| A new order type or command value | stops at that record, as on any unknown value |
| A change that moves or redefines a field | raises the minimum reader version, so the header refuses the file |

A new reader keeps every older decoder. Raising a format's minimum reader version is a breaking change, so it is a major release.

## Errors

The reader follows the CSV's fail-fast rule: at the first bad record it returns nothing further, and looks ahead only to classify the failure.

| Code | Meaning |
|---|---|
| `TORN_TAIL` | the file ends inside a record, or nothing valid follows the bad record; this covers zero-filled and stale tails, and an all-zero record, since command 0 is never valid |
| `CORRUPT_RECORD` | a later record passes both its checksum and the field checks, so the file continues past the damage |
| `CORRUPT_HEADER` | the header fails its checksum, or the file ends inside it; nothing runs |
| `UNSUPPORTED_VERSION` | the minimum reader version is above this reader's; nothing runs |
| `UNKNOWN_FIELD` | a reserved or skipped byte is not zero |
| `INVALID_SYMBOL` | not a valid symbol |

Field problems reuse the CSV's codes and v3's column names, such as `INVALID_SIDE` at `side`. A log cut exactly at a record boundary looks like a clean end. Replay runs the good prefix, prints the diagnostic, and exits 1 on either tail code.

Diagnostics read as the CSV's do, `<path>:<line>: <column>: <message>`, where the line is the one the record has in its CSV twin.

## Lossless

Lossless means the same commands and symbols in the same order, per instrument across files. A CSV's sequence gaps, leading zeros, and line endings are not carried. Binary to CSV to binary is byte-identical for a log with no placement at position 0, and otherwise identical in its commands, since a different header changes the chain's checksums. Canonical CSV to binary to CSV is byte-identical.

## Reading one

`order_book_cli replay` tells a log from a CSV by its first 8 bytes, so no format is ever named. In code, `CommandLogReader` in `command_log.hpp` is the counterpart of `ReplayReader`, and `encodeLogHeader` and `encodeLogRecord` are a pure encoder. There is no writer class: whoever buffers and syncs calls the encoder into its own buffer.

`tests/replay/log/` holds golden files, and `make_golden.py` there writes them from this document alone, with its own XXH64.
