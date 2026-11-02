# Wire protocol

How a client talks to `order_book_engine` over TCP, byte for byte, version 1. Nothing else is needed to write a client. Everything is little-endian. Values come from the tables here, never from a C++ enumerator.

**Only a refused command is safe to retry.** A `Refused` frame means the command never entered a command queue. After anything else, including a closed connection, the command may have run, and there are no idempotent resends: a blind resend of an `ADD` can trade twice, since an ID is reusable after a fill.

## Connection

There is no session: nothing outlives a connection. A client's first frame is `Hello`, once, and the engine answers `Welcome`. A client may send commands straight after `Hello` without waiting. `TCP_NODELAY` is always on at both ends, and there are no heartbeats.

- **Who hears.** Only the sender. A resting order's owner gets no report when another connection's order trades with it, and a triggered stop's cascade goes whole to the connection whose command caused it. Closing a connection does nothing to its resting orders.
- **Owners and IDs.** `owner` passes through unchanged and order IDs are not scoped to a connection, so a CSV reaches the book as written. Two connections using one live ID get `DUPLICATE_ORDER_ID`, and any connection may cancel or replace any order. Clients are trusted.
- **Ordering.** Responses for one connection and one instrument arrive in command order, except a command the entry check refuses itself, with a `Refused` frame or, for a malformed command, the book's own event, which can overtake earlier commands' responses. Nothing is ordered across instruments, so a client matches responses by correlation.
- **Half-close.** A client that half-closes is answered in full, then closed, with no error frame, so a client can send a file, half-close, and read to end of stream knowing it has every answer. Read and write at once: a client that writes everything before reading anything can hang, because a connection with too many unsent responses stops being read.

## Header

Every frame, in both directions, starts with these 12 bytes.

| Offset | Size | Field |
|---|---:|---|
| 0 | 2 | length of the whole frame, header included |
| 2 | 1 | frame type |
| 3 | 1 | flags: bit 0 marks the last frame of a response, and every other bit must be zero |
| 4 | 8 | correlation, picked by the client, echoed on every response, never read or checked for uniqueness |

A client's own frames have flags 0. `Welcome`, `Refused`, and `ProtocolError` always set bit 0.

## Frames

| Frame | Type | Body after the header | Size |
|---|---:|---|---:|
| `Hello` | 1 | see below | 24 |
| `Welcome` | 2 | see below | 24 |
| `Command` | 3 | bytes 4 to 39 of a log record, unchanged | 48 |
| `Refused` | 4 | symbol, order ID, code (2 bytes), 2 zero bytes | 28 |
| `ProtocolError` | 5 | code (2 bytes), engine's version (2 bytes) | 16 |
| Events | 16 onward | symbol, then the fields below | 28 to 40 |

Types 6 to 15 are free for new frames that are not events. A client skips a frame type it does not know, and trailing bytes on one it does, by the length. The engine accepts only the exact length for a type in the connection's version.

**`Hello` and `Welcome`.** The 8 bytes of magic `OBWIRE\r\n`, then the version (2 bytes), then 2 zero bytes. `Welcome` carries the version the engine accepted. The version starts at 1 and follows the log's extension rule: a new field's zero means today's behavior, and a new engine keeps every older decoder. If the log's record gains a field, the wire version rises with it.

**`Command`.** [`command-log-format.md`](command-log-format.md) holds the field table and its value tables. The record's checksum is dropped, because it chains from the previous record, which a client cannot know. Fields a command does not use must be zero: a `CANCEL` uses only its symbol and order ID, and a `REPLACE` leaves its side, order type, time in force, and stop price unused.

**Events.** One frame per event, in the order the book made them, in `BookEvent` order of type. The last has bit 0 set.

| Event | Type | Fields after the symbol | Size |
|---|---:|---|---:|
| `OrderAccepted` | 16 | order ID, price, quantity, side (1 byte), 3 zero bytes | 36 |
| `OrderRejected` | 17 | order ID, reason (2 bytes), 2 zero bytes | 28 |
| `TradeExecuted` | 18 | incoming order ID, resting order ID, price, quantity | 36 |
| `OrderCancelled` | 19 | order ID, quantity | 28 |
| `CancelRejected` | 20 | order ID, reason (2 bytes), 2 zero bytes | 28 |
| `OrderExpired` | 21 | order ID, quantity | 28 |
| `SelfTradePrevented` | 22 | incoming order ID, resting order ID, price, cancelled quantity | 36 |
| `StopTriggered` | 23 | order ID, stop price, trade price | 32 |
| `OrderReplaced` | 24 | order ID, price, quantity, open quantity, side (1 byte), priority kept (1 byte), 2 zero bytes | 40 |
| `ReplaceRejected` | 25 | order ID, reason (2 bytes), 2 zero bytes | 28 |

IDs, prices, and quantities are 4 bytes each. A side is 1 for buy and 2 for sell, as in the log. Price 0 on `OrderAccepted` means an order with no limit price.

## Entry check

Once a `Command`'s shape passes, the gateway checks it, stopping at the first failure.

1. **Unknown or malformed symbol:** `Refused`, `UNKNOWN_INSTRUMENT`.
2. **Malformed:** the book's own event and reason, flagged last. An unknown side, order type, time in force, or self-trade byte gets the book's own reason, as if the field held a value past every known one. A `CANCEL` with ID 0 gets `CancelRejected` with `ORDER_NOT_FOUND`.
3. **Engine stopping:** `Refused`, `ENGINE_STOPPING`, meaning retry. It comes last, so that code only ever means this command would have been queued.

No owner clears the self-trade mode to absent, and an owner with an absent mode takes `CANCEL_RESTING`. Any other unknown mode is `INVALID_SELF_TRADE_PREVENTION`, whatever the owner.

## Codes

All three tables are append-only: a value is never renumbered or reused.

**`Refused`.**

| Code | Name | Meaning |
|---:|---|---|
| 1 | `UNKNOWN_INSTRUMENT` | not on the instrument list, or not a symbol at all |
| 2 | `ENGINE_STOPPING` | the engine is stopping; this command would otherwise have been queued |

**Reasons,** in `OrderRejected`, `CancelRejected`, and `ReplaceRejected`.

| Code | Name |
|---:|---|
| 1 | `DUPLICATE_ORDER_ID` |
| 2 | `ORDER_NOT_FOUND` |
| 3 | `INVALID_ORDER_ID` |
| 4 | `INVALID_PRICE` |
| 5 | `INVALID_QUANTITY` |
| 6 | `INVALID_SIDE` |
| 7 | `UNSUPPORTED_ORDER_TYPE` |
| 8 | `ORDER_NOT_ACTIVE` |
| 9 | `WOULD_CROSS_BOOK` |
| 10 | `PRICE_NOT_ALLOWED` |
| 11 | `INVALID_TIME_IN_FORCE` |
| 12 | `TIME_IN_FORCE_NOT_ALLOWED` |
| 13 | `INVALID_STOP_PRICE` |
| 14 | `STOP_PRICE_NOT_ALLOWED` |
| 15 | `POST_ONLY_NOT_ALLOWED` |
| 16 | `INVALID_SELF_TRADE_PREVENTION` |
| 17 | `REPLACE_QUANTITY_TOO_LOW` |
| 18 | `ORDER_NOT_REPLACEABLE` |

**`ProtocolError`.**

| Code | Name | Meaning |
|---:|---|---|
| 1 | `BAD_LENGTH` | under 12, or not the exact length for its type |
| 2 | `BAD_MAGIC` | a `Hello` without the magic |
| 3 | `UNKNOWN_FRAME_TYPE` | a type the engine does not take |
| 4 | `HELLO_REQUIRED` | a first frame that is not `Hello` |
| 5 | `DUPLICATE_HELLO` | a second `Hello` |
| 6 | `UNSUPPORTED_VERSION` | a version this engine cannot speak |
| 7 | `UNKNOWN_COMMAND` | a `Command` whose command byte is not 1, 2, or 3 |
| 8 | `UNEXPECTED_FIELD` | a non-zero unused or reserved byte |
| 9 | `TOO_MANY_CONNECTIONS` | past the limit on one gateway thread |

An unknown frame type closes the connection rather than being refused, because after `Hello` it can only be a bug. On a protocol error the gateway stops reading, answers everything in flight on that connection, sends one `ProtocolError` with the offending frame's correlation, or 0 for a length under 12, which has none, and closes.
