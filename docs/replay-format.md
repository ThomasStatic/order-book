# Replay file format

A replay file is a CSV list of **commands**: what outside participants ask the book to do, in the order they asked. It does not hold lifecycle events. Those are what the book did in response, and the replay gets them from each command's result.

Three layers handle a replay file, and the order book knows about none of this:

1. **Parser.** It reads the external representation. It checks the file structure, the record framing, the field syntax, and the sequence ordering, and turns each data record into a typed command. It knows nothing about the book's state.
2. **Replay engine.** It hands each parsed command to the matching `OrderBook` call in file order, collects results and events, and reports totals. It implements no trading rules.
3. **Order book.** It enforces trading rules. A well-formed command can still be refused here, for example an `ADD` that reuses an active order ID. That refusal is an ordinary result, not a parse error.

The rule for which layer rejects what: if deciding needs the book's state, the book decides. Everything this document lists as invalid can be decided from the file alone.

## Quick example

```csv
sequence,command,order_id,side,price,quantity
1,ADD,1001,BUY,100,50
2,ADD,1002,SELL,105,30
3,ADD,1003,BUY,105,20
4,CANCEL,1001,,,
```

Replaying it on an empty book gives these events (printed by `formatEvent`):

| Line | Sequence | Events |
|---|---|---|
| 2 | 1 | `OrderAccepted id=1001 side=BUY price=100 qty=50` |
| 3 | 2 | `OrderAccepted id=1002 side=SELL price=105 qty=30` |
| 4 | 3 | `OrderAccepted id=1003 side=BUY price=105 qty=20`, `TradeExecuted incoming=1003 resting=1002 price=105 qty=20` |
| 5 | 4 | `OrderCancelled id=1001 qty=50` |

At the end, 1002 rests alone with 10 at 105.

## File structure

- **Encoding.** ASCII. A byte-order mark is not allowed. It would be part of the header, so the header would not match.
- **Records.** Each line is one record. A record ends with LF or with CRLF, and a file may mix the two. The last record may leave out its line ending. A CR that is not directly followed by LF belongs to the field it appears in, which makes that field invalid.
- **Header.** The first record must be exactly this, case-sensitive, with no extra whitespace, columns, or trailing comma:

  ```text
  sequence,command,order_id,side,price,quantity
  ```

  A file with no header, an empty file included, is invalid. A file with only the header is valid and replays no commands.
- **Data records.** Every record after the header is a command. Each one has exactly six fields, split on every comma.
- **No quoting.** Fields are never quoted. A `"` anywhere in a record is an error. No valid field needs quoting, since none can contain a comma, quote, or line break.
- **No blank lines or comments.** An empty record is an error wherever it appears, including an extra empty line at the end of the file (`...\n\n`). No comment syntax exists.

Line numbers in diagnostics are physical and 1-based. The header is line 1, and blank lines count. A line number is a different thing from a sequence number. In the example above, sequence 1 is on line 2.

## Columns

| Column | `ADD` | `CANCEL` | Domain |
|---|---|---|---|
| `sequence` | required | required | positive integer up to 2^64 − 1 (`std::uint64_t`) |
| `command` | `ADD` | `CANCEL` | exactly one of these, upper case |
| `order_id` | required | required | positive `OrderId`, up to 4294967295 |
| `side` | required | must be empty | `BUY` or `SELL`, upper case |
| `price` | required | must be empty | positive `Price` in integer ticks ($0.01 each), up to 4294967295 |
| `quantity` | required | must be empty | positive `Quantity`, up to 4294967295 |

### Field syntax

- **Numbers.** Unsigned decimal digits only, `[0-9]+`, read without regard to locale. Leading zeros are allowed and ignored: `000105` is 105, in every numeric column, `sequence` included. The value must be no larger than the column's maximum, however many digits it is written with. These are all invalid: signs (`+105`, `-105`), decimal points (`100.50`), exponents (`1e3`), any other character (`12abc`, `0x10`), and surrounding whitespace (` 105`). Prices are integer ticks, the same `Price` the engine uses, so a replay file never contains a decimal price.
- **Zero.** Zero is well-formed syntax but an invalid value for every numeric column, however it is written (`0`, `000`). `types.hpp` reserves zero for `Price`, `Quantity`, and `OrderId`. So a zero is a parse error, not a book rejection, and that covers `CANCEL,0` too.
- **Keywords.** `ADD`, `CANCEL`, `BUY`, and `SELL` match exactly. Case variants and padded values are invalid.
- **Empty and absent.** A field is empty only when it holds no characters. An empty field is never read as zero or as a default value. `3,ADD,1002,SELL,,20` is invalid because the price is missing. In `4,CANCEL,1002,,,` the empty fields are correct, because those columns don't apply to a cancel. `5,CANCEL,1002,,0,` is invalid because it fills in a price, and the value doesn't matter. A single space also counts as filled in.

### Sequence numbers

`sequence` must be strictly greater than the value on the previous data record. Gaps are allowed (`10, 20, 35, 36`), so rows can be removed from a file without renumbering it. The first data record can start at any positive value. A repeated or decreasing value is an error.

Commands run in **physical file order**. Strict ordering means file order and sequence order always agree. A replay never sorts by sequence. When the two disagree, the file is wrong, and the replay doesn't try to fix it. This matters because the book uses price-time priority: running the same commands in a different order can produce different trades.

File sequence numbers are separate from the book's `SequenceNum`. They are never passed to the engine. The book still numbers each order it accepts from its own counter.

## Command semantics

| Command | Engine call | Notes |
|---|---|---|
| `ADD` | `OrderBook::submitOrder(Order(order_id, price, quantity, <placeholder>, side))` | A `LIMIT` order. It matches by price-time priority, and any remainder rests at its limit price. The book assigns the sequence number. |
| `CANCEL` | `OrderBook::cancelOrder(order_id)` | Cancels whatever quantity the order still has. |

`ADD` maps to `submitOrder`, **not** `addOrder`. The names are similar, but `addOrder` never matches. It throws on a crossing order, and on any rejection, so a perfectly ordinary file row such as line 4 of the example would stop the replay. `submitOrder` matches and reports rejections as values. The replay never calls `addOrder`.

Valid rows that the book refuses produce rejection events, and the replay carries on:

- `ADD` with an order ID that is already resting gives `OrderRejected ... reason=DUPLICATE_ORDER_ID`.
- `CANCEL` of an ID that is not resting (unknown, already cancelled, fully filled, or an incoming order that never rested) gives `CancelRejected ... reason=ORDER_NOT_FOUND`.
- An ID that was filled or cancelled can be used again in a later `ADD`.

## Errors

### What each kind of error does

| Error | Example | Layer | Kind | Effect |
|---|---|---|---|---|
| File can't be opened | missing path | replay | fatal | nothing runs |
| Missing or wrong header | empty file, reordered or duplicate columns, BOM | parser (file) | fatal | nothing runs |
| Record framing | quote character, wrong field count, blank line | parser (record) | fatal | stop at that line |
| Sequence out of order | repeated or decreasing `sequence` | parser (field, across records) | fatal | stop at that line |
| Invalid field | unknown command or side, bad number, missing or extra field | parser (field) | recoverable in principle, fatal under the current policy | stop at that line |
| Book rejection | duplicate active ID, cancel of an ID that isn't resting | order book | recoverable | record the event and continue |

Fatal errors either make later records unreliable to read or break the ordering contract, so the replay can never continue past them. An invalid field affects only its own command, so a later skip-and-continue policy could allow it. That would be a new option, though, not the default.

### Fail-fast policy

The current policy fails fast on every parse error. At the first one, the replay:

1. stops reading the file;
2. reports the file path, the physical line number, the column when one field is to blame, and a diagnostic;
3. does not dispatch the bad command or anything after it;
4. exits with status 1;
5. does not roll back commands that already ran. Their events stand, and so does the book state they produced.

A file that replays to the end exits with status 0, even when the book rejected some commands. Rejections are outcomes, not input errors. An exception from the engine (an allocation failure, or an internal inconsistency in a build without automatic invariant checks) also stops the replay with a nonzero status. That is reported as an engine failure, not as an input error.

### Checking order and diagnostics

A record is checked in this order, and only the first problem is reported:

1. empty record (no column);
2. a `"` anywhere in the record (no column);
3. field count other than six (no column);
4. `sequence`: syntax and range, then strict increase;
5. `command`;
6. `order_id`;
7. `side`, `price`, then `quantity`: each must be present and valid for `ADD`, and each must be empty for `CANCEL`.

Header errors are reported at line 1 with no column. Diagnostics take this form:

```text
<path>:<line>: <column>: <message>
<path>:<line>: <message>                  (record- and file-level errors)
```

For example:

```text
orders.csv:4: price: ADD requires a price
orders.csv:4: price: CANCEL must leave price empty
orders.csv:4: price: '100.50' is not a whole number of ticks
orders.csv:4: sequence: 2 does not follow 2 (sequence numbers must strictly increase)
orders.csv:4: expected 6 fields, found 5
orders.csv:1: header must be 'sequence,command,order_id,side,price,quantity'
```

The message wording is not part of the format. Tests should check the line, the column, and which rule was broken.

## Parser

The parser is `include/order_book/replay_parser.hpp` (namespace `order_book::replay`), built as its own library, `order_book_replay`, so the `order_book` library stays free of CSV. It needs no `OrderBook` and does no file I/O of its own:

- `splitRecord(text, lineNumber)` splits one line, its line ending already removed, into a `CsvRow` (the fields and the physical line number). It never fails. `splitRecordInto(text, lineNumber, row)` does the same into an existing row, reusing its storage; `ReplayReader` splits every record into one row this way, so reading a record allocates nothing once the row has grown.
- `parseRow(row, previousSequence)` checks one data record in the order above and returns a `ReplayRecord` or the first `ParseError`. It is a pure function: the same row and previous sequence always give the same result.
- `checkHeader(text)` checks line 1.
- `ReplayReader` reads a `std::istream` line by line: the header, then one record per `next()`, tracking line numbers, line endings, and the sequence ordering between records. After the first error, or the end of input, it returns that again without reading further. Open files in binary mode, so CRs reach it exactly as written.
- `formatParseError(error, path)` prints a diagnostic in the form above.

```c++
struct AddCommand { OrderId orderId; Side side; Price price; Quantity quantity; };
struct CancelCommand { OrderId orderId; };
using Command = std::variant<AddCommand, CancelCommand>;

struct ReplayRecord {
    std::size_t lineNumber;   // physical line in the file, header = 1
    std::uint64_t sequence;
    Command command;
};

struct ParseError {
    std::size_t lineNumber;
    ParseErrorCode code;      // which rule was broken
    std::string column;       // header name of the column at fault; empty for record- and file-level errors
    std::string value;        // that field's text, or the whole record for record- and file-level errors
    std::string message;
};
```

`ParseErrorCode` names the rule:

| Code | Level | Rule |
|---|---|---|
| `MISSING_HEADER` | file | empty input, or a first line that is itself a valid data record |
| `INVALID_HEADER` | file | any other first line that is not exactly the header |
| `EMPTY_RECORD` | record | a blank line |
| `QUOTE_CHARACTER` | record | a `"` anywhere in the record |
| `WRONG_COLUMN_COUNT` | record | not six fields |
| `SEQUENCE_NOT_INCREASING` | field | `sequence` not greater than the previous record's |
| `UNKNOWN_COMMAND` | field | `command` neither `ADD` nor `CANCEL` |
| `MISSING_REQUIRED_FIELD` | field | empty where a value is required |
| `UNEXPECTED_FIELD` | field | filled in where `CANCEL` needs it empty |
| `INVALID_SIDE` | field | `side` neither `BUY` nor `SELL` |
| `INVALID_NUMBER` | field | anything but decimal digits |
| `NUMBER_OUT_OF_RANGE` | field | above the column's maximum |
| `INVALID_VALUE` | field | zero |

A successfully parsed command satisfies the `Order` constructor's preconditions: nonzero ID, price, and quantity, and a valid side. So the replay engine never constructs an `Order` that throws.

## Replay engine

The engine is `include/order_book/replay_engine.hpp`, in the same library and namespace as the parser. It knows no CSV and implements no trading rules.

- `readReplayFile(path)` opens the file in binary mode and reads it through a `ReplayReader`; `readReplayInput(stream)` does the same for a stream. Either returns a `ReplayInput`: every record up to the first error, and that error, if any, as a `ReplayError` holding the parser's `ParseError`. A file that can't be opened, or a stream that fails mid-read, is an error with no line. Since the format fails fast, the records before the error are exactly the ones a replay runs.
- `ReplayEngine::run(records, book, options)` runs each record against `book` in the order given, as the command table above says, and returns a `ReplayOutcome`: a `ReplayResult` or a `ReplayError`. A result holds every event the book returned, unchanged and in order, each with its record's line (`ReplayEvent`); the `ReplayStatistics` (commands processed; `ADD` and `CANCEL` commands, each split into accepted and rejected; trades executed; volume traded); and the final book as a `BookSnapshot` of every level, best price first. A replay of a file starts from an empty `OrderBook`, so the same file always gives the same events, statistics, and final book.
- `run` stops at the first exception from the book (`INTERNAL_EXECUTION_FAILURE`, naming the command's line) or failed invariant check (`INVARIANT_VIOLATION`). `ReplayOptions::invariantValidation` sets how often it calls `checkInvariants`: `NONE`, `FINAL` (the default: once, after the last command, which names no line), or `PER_COMMAND` (after every command, naming the first line after which the book is broken, at O(N + L) per command). These are separate from the book's own automatic checks, which abort rather than report.
- `replay_summary.hpp` prints a replay: `formatReplayEvent` (`line 4: TradeExecuted ...`), `formatReplayStatistics`, `formatFinalBook`, `formatReplaySummary` (the statistics, then the final book), and `formatReplayError`. The statistics are one `name: value` line per statistic, in a fixed order (commands processed, then the `ADD` counts, then the `CANCEL` counts, then trades and volume), then `resting orders`. The final book is `bids:` and `asks:`, each followed by one `  price=<ticks> qty=<total> orders=<count>` line per level, best price first (bids descending, asks ascending), or `  none`. It holds no timestamps, durations, addresses, or unordered iteration, and numbers are formatted without a locale.

### Command line

```text
order_book_cli replay <file.csv> [--trades] [--events] [--book] [--validate none|final|per-command]
order_book_cli demo
order_book_cli --help
```

It replays the file on an empty book and prints, on stdout with LF line endings on every platform: each trade (`--trades`) or each event (`--events`, which includes the trades), as `formatReplayEvent` writes them, then a blank line if any were printed; then the statistics (`formatReplayStatistics`), always; then the final book (`formatFinalBook`) with `--book`. `--events --book` is the full summary, `formatReplaySummary`. Options go before or after the file, and `--` makes the next argument the file. `-h` or `--help` anywhere before `--` prints the help on stdout and exits 0; there are no prompts. A parse error stops the replay at that line: what ran before it is printed as usual, and the diagnostic follows on stderr. Exit statuses:

| Status | Meaning |
|---|---|
| 0 | replayed to the end, even if the book rejected some commands; or help printed |
| 1 | input error: the file can't be opened or read, or is malformed |
| 2 | usage error: one line on stderr naming the problem (a missing or unknown command, no file or two, an unknown option, a missing or unknown `--validate` mode), then the usage |
| 3 | engine failure: the book threw, or failed an invariant check. Only the diagnostic is printed. When the final check fails, the same records are replayed on a fresh book with per-command checks, and a second diagnostic names the first line after which the book is broken. |

`tests/replay/expected/` holds the exact output for each valid sample (with `--events --book`), each output selection, a few failing files, help, the demo, and each usage error; the `replay_cli_*` tests compare it byte for byte.

## Version 2

Version 2 ([ADR 0005](adr/0005-replay-format-v2-is-a-second-exact-header.md)) records the order behaviors: market, IOC, FOK, post-only, self-trade prevention, stops, and replaces. It is a second exact header, v1's six columns followed by six more:

```text
sequence,command,order_id,side,price,quantity,order_type,time_in_force,stop_price,post_only,owner,self_trade_prevention
```

The reader picks the layout from line 1. A v1 file parses and replays exactly as before and means v1's defaults (`LIMIT`, `GTC`, no stop price, not post-only, no owner, `CANCEL_RESTING`); it knows only `ADD` and `CANCEL`, so `REPLACE` in a v1 file is `UNKNOWN_COMMAND`. Every v2 record has exactly twelve fields. `MISSING_HEADER` means a first line that is a valid data record in either layout. Converting a v1 file is mechanical: append `LIMIT,GTC,,,,` to each `ADD` and `,,,,,,` to each `CANCEL`.

### Columns (v2)

Empty means absent, and zero is still refused in every numeric column.

| Column | `ADD` | `REPLACE` | `CANCEL` |
|---|---|---|---|
| `side` | required | empty | empty |
| `price` | required on `LIMIT`/`STOP_LIMIT`, empty on `MARKET`/`STOP` | required | empty |
| `quantity` | required | required: the new total | empty |
| `order_type` | required: `LIMIT`, `MARKET`, `STOP`, `STOP_LIMIT` | empty | empty |
| `time_in_force` | required: `GTC`, `IOC`, `FOK` | empty | empty |
| `stop_price` | required on `STOP`/`STOP_LIMIT`, empty otherwise | empty | empty |
| `post_only` | empty (no) or `Y` | empty or `Y` | empty |
| `owner` | empty (no owner) or a positive `OwnerId` | the same; empty clears the stored owner | empty |
| `self_trade_prevention` | required exactly when `owner` is set: `CANCEL_RESTING`, `CANCEL_INCOMING`, `CANCEL_BOTH` | the same | empty |

Keywords are exact upper case, one spelling each: `N`, case variants, padding, and FIX wire codes are invalid. `0` for an owner or a stop price is `INVALID_VALUE`, so a missing owner is written empty. Two combinations are parse errors, `INVALID_COMBINATION`: `MARKET` or `STOP` with `GTC` (reported at `time_in_force`), and `Y` on an `ADD` that is not a GTC `LIMIT` (at `post_only`). With the presence rules above, that covers every request check a row alone decides, so **a parsed `ADD` is always a well-formed request**, and a replay never produces an `OrderRejected` for a malformed request.

### Checking order (v2)

1. empty record, `"`, field count (twelve);
2. `sequence`, `command` (`ADD`, `CANCEL`, or `REPLACE`), `order_id`;
3. `order_type`, then `time_in_force` (its keyword, then the GTC combination);
4. `side`, `price`, `quantity`, `stop_price`;
5. `post_only` (its keyword, then the combination);
6. `owner`, then `self_trade_prevention`.

Within a column: present or empty as the command and type require, then syntax, then range and zero, then the combination. The type comes before `side` because a price's rule depends on it. A v1 record goes through exactly v1's order.

`ParseErrorCode` appends `INVALID_ORDER_TYPE`, `INVALID_TIME_IN_FORCE`, `INVALID_POST_ONLY`, `INVALID_SELF_TRADE_PREVENTION`, and `INVALID_COMBINATION`, and it is an open set: later versions only append. `UNEXPECTED_FIELD` also covers a field filled in where the command, the order type, or a missing owner needs it empty; `MISSING_REQUIRED_FIELD` covers a missing stop price, or a missing mode with an owner; `UNKNOWN_COMMAND` covers `REPLACE` in a v1 file.

### Commands (v2)

| Command | Engine call | Notes |
|---|---|---|
| `ADD` | `OrderBook::submitOrder(OrderRequest{...})` | Always the request, in either layout; for v1's fields that is exactly `submitOrder(Order)`. |
| `REPLACE` | `OrderBook::replaceOrder(ReplaceRequest{...})` | A `ReplaceRejected` (not found, a total too low, `ORDER_NOT_REPLACEABLE` for a pending stop) is a book refusal, and the replay continues. |
| `CANCEL` | `OrderBook::cancelOrder(order_id)` | May name a pending stop: one `OrderCancelled` with its whole quantity. |

A command's events include its cascade (`StopTriggered`, then that stop's trades, preventions, and expiry), all tagged with the line of the command that ran it.

### API (v2)

- `enum class ReplayFormat { V1, V2 }`; `expectedHeaderV2`, `columnCountV2`, and the six new column indices and `columnNamesV2`.
- `checkHeader` accepts either header, and `headerFormat(text)` returns the `ReplayFormat` or a `ParseError`.
- `parseRow(row, ReplayFormat, previousSequence)`; the two-argument `parseRow` still means v1. `ReplayReader::format()` is the layout its header named.
- `AddCommand` gains `orderType`, `timeInForce`, `stopPrice`, `postOnly`, `owner`, and `selfTradePrevention` after `quantity`, with v1's defaults, so existing initializers still compile. `ReplaceCommand { orderId, price, quantity, postOnly, owner, selfTradePrevention }` is appended to `Command`, which is an open set like `BookEvent`.
- `ReplayStatistics` gains `replaceCommands`, `replacesAccepted`, `replacesRejected`, `selfTradesPrevented`, `ordersExpired`, and `stopsTriggered` (appended, so v1 initializers keep their meaning); commands processed = adds + cancels + replaces. The engine counts them from events, so it still implements no trading rules, and an `ADD` accepted then expired counts as accepted.
- `ReplayResult` gains `pendingStops` and `lastTradePrice`, the book's at the end.

### Output (v2)

The statistics always print every line, in this order: commands processed; the `ADD`, `CANCEL`, and then `REPLACE` counts; trades executed and volume traded; self-trades prevented, orders expired, and stops triggered; resting orders; pending stops. `--book` adds `last trade price: <ticks>` (or `none`) after the asks (`formatFinalBook(result)`; the snapshot-only `formatFinalBook(book)` prints the two sides alone). Pending stops are not listed: the book offers no query for them beyond `pendingStopCount` and `findPendingStop`. `--trades` prints every `TradeExecuted`, the cascade's included, and never a `SelfTradePrevented`; `--events` prints everything.

## Versioning and scope

The format has no version field. The exact header acts as its version marker: v1 and v2 are two exact headers. A later layout would be a third, and a reader that doesn't know it fails at line 1 instead of misreading rows.

These are out of scope: quoting, comments, timestamps, lifecycle events as input, and a listing of pending stops in the output.

This format is separate from the randomized-test stream format (`SUBMIT id=17 ...`, see `tests/randomized/regressions/README.md`). That format is internal test tooling, and it records generator intent alongside each command.

## Sample files

`tests/replay/valid/` holds well-formed files. `tests/replay/malformed/` holds files with exactly one error each. `tests/replay/README.md` lists every file with its expected outcome: the events for each valid file, and the line, column, and rule for each malformed one. Git stores these files byte for byte (`tests/replay/.gitattributes`), so line endings, the BOM, and the missing final newline survive checkout on every platform.
