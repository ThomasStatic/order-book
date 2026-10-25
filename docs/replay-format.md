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

- `splitRecord(text, lineNumber)` splits one line, its line ending already removed, into a `CsvRow` (the fields and the physical line number). It never fails.
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

A successfully parsed command satisfies the `Order` constructor's preconditions: nonzero ID, price, and quantity, and a valid side. So the replay engine never constructs an `Order` that throws. No replay engine exists yet.

## Versioning and scope

The format has no version field. The exact header acts as its version marker. If a later version adds or renames a column, its header changes, and a reader that doesn't know it fails at line 1 instead of misreading rows.

These are out of scope for this version: quoting, comments, order types other than `LIMIT`, modify/replace commands, timestamps, and lifecycle events as input.

This format is separate from the randomized-test stream format (`SUBMIT id=17 ...`, see `tests/randomized/regressions/README.md`). That format is internal test tooling, and it records generator intent alongside each command.

## Sample files

`tests/replay/valid/` holds well-formed files. `tests/replay/malformed/` holds files with exactly one error each. `tests/replay/README.md` lists every file with its expected outcome: the events for each valid file, and the line, column, and rule for each malformed one. Git stores these files byte for byte (`tests/replay/.gitattributes`), so line endings, the BOM, and the missing final newline survive checkout on every platform.
