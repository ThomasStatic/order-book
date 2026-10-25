# Replay sample files

Sample input for the CSV replay format defined in [`docs/replay-format.md`](../../docs/replay-format.md). No parser reads these yet. They are the agreed examples that the parser and replay-engine tests will check against.

`.gitattributes` marks every `*.csv` here `-text`, so Git keeps the bytes exactly as committed, whatever `core.autocrlf` says. Several files depend on that: `crlf_line_endings.csv`, `no_final_newline.csv`, `header_with_bom.csv`, and `trailing_blank_line.csv`. Don't re-save these files in an editor that normalizes line endings or encoding.

## `valid/`

Every file here should replay to the end with exit status 0. The events are the `formatEvent` lines each command produces on a fresh book. They were checked against the engine when the files were written.

| File | Shows |
|---|---|
| `basic.csv` | the example from the spec: rest, rest, cross, cancel |
| `price_time_priority.csv` | FIFO within a level, a multi-level sweep, a resting remainder, execution at the resting price |
| `book_rejections.csv` | well-formed rows the book refuses, with the replay continuing; ID reuse after a cancel |
| `sequence_gaps.csv` | non-contiguous sequence numbers; a cancel moves the next order to the front of the queue |
| `limits.csv` | maximal ID, price, and quantity (4294967295) and maximal sequence (18446744073709551615) |
| `header_only.csv` | header and no commands: replays nothing |
| `crlf_line_endings.csv` | CRLF record terminators |
| `no_final_newline.csv` | last record without a line ending |

### Expected events

`basic.csv`

```text
line 2  OrderAccepted id=1001 side=BUY price=100 qty=50
line 3  OrderAccepted id=1002 side=SELL price=105 qty=30
line 4  OrderAccepted id=1003 side=BUY price=105 qty=20
        TradeExecuted incoming=1003 resting=1002 price=105 qty=20
line 5  OrderCancelled id=1001 qty=50
final   bids: none   asks: 105 x 10 (1002)
```

`price_time_priority.csv`

```text
line 2  OrderAccepted id=2001 side=SELL price=101 qty=10
line 3  OrderAccepted id=2002 side=SELL price=101 qty=15
line 4  OrderAccepted id=2003 side=SELL price=102 qty=20
line 5  OrderAccepted id=2004 side=BUY price=99 qty=5
line 6  OrderAccepted id=2005 side=BUY price=102 qty=50
        TradeExecuted incoming=2005 resting=2001 price=101 qty=10
        TradeExecuted incoming=2005 resting=2002 price=101 qty=15
        TradeExecuted incoming=2005 resting=2003 price=102 qty=20
line 7  OrderAccepted id=2006 side=SELL price=99 qty=8
        TradeExecuted incoming=2006 resting=2005 price=102 qty=5
        TradeExecuted incoming=2006 resting=2004 price=99 qty=3
final   bids: 99 x 2 (2004)   asks: none
```

`book_rejections.csv`

```text
line 2  OrderAccepted id=3001 side=BUY price=100 qty=10
line 3  OrderRejected id=3001 reason=DUPLICATE_ORDER_ID
line 4  CancelRejected id=9999 reason=ORDER_NOT_FOUND
line 5  OrderCancelled id=3001 qty=10
line 6  CancelRejected id=3001 reason=ORDER_NOT_FOUND
line 7  OrderAccepted id=3001 side=SELL price=110 qty=5
final   bids: none   asks: 110 x 5 (3001)
```

`sequence_gaps.csv`

```text
line 2  OrderAccepted id=4001 side=SELL price=200 qty=5
line 3  OrderAccepted id=4002 side=SELL price=200 qty=5
line 4  OrderCancelled id=4001 qty=5
line 5  OrderAccepted id=4003 side=BUY price=200 qty=5
        TradeExecuted incoming=4003 resting=4002 price=200 qty=5
final   empty
```

`limits.csv`

```text
line 2  OrderAccepted id=4294967295 side=SELL price=4294967295 qty=4294967295
line 3  OrderAccepted id=1 side=BUY price=1 qty=1
line 4  OrderCancelled id=4294967295 qty=4294967295
final   bids: 1 x 1 (1)   asks: none
```

`header_only.csv`: no events; the book stays empty.

`crlf_line_endings.csv`

```text
line 2  OrderAccepted id=5001 side=BUY price=100 qty=10
line 3  OrderAccepted id=5002 side=SELL price=100 qty=4
        TradeExecuted incoming=5002 resting=5001 price=100 qty=4
final   bids: 100 x 6 (5001)   asks: none
```

`no_final_newline.csv`

```text
line 2  OrderAccepted id=6001 side=SELL price=300 qty=7
line 3  OrderCancelled id=6001 qty=7
final   empty
```

## `malformed/`

Every file has exactly one error. The replay must stop there with exit status 1 and report the line and column below. It must not dispatch the bad record or anything after it, and it must not roll back what already ran.

Most files share one layout. Lines 2 and 3 are valid and get dispatched (`OrderAccepted` for 1001 BUY 100 x 50, then for 1002 SELL 105 x 30, with no trade). Line 4 is the bad record. Line 5, `4,CANCEL,1001,,,`, must never run, so after the stop both 1001 and 1002 are still resting. "Dispatched" below counts the commands that ran before the stop.

"—" means a record- or file-level error that names no column.

### File level (header)

| File | Line | Column | Dispatched | Broken rule |
|---|---|---|---|---|
| `empty_file.csv` | 1 | — | 0 | header missing (zero-byte file) |
| `missing_header.csv` | 1 | — | 0 | header missing (the first line is a data row) |
| `header_wrong_order.csv` | 1 | — | 0 | header mismatch: `price` and `side` swapped |
| `header_missing_column.csv` | 1 | — | 0 | header mismatch: no `quantity` |
| `header_duplicate_column.csv` | 1 | — | 0 | header mismatch: `price` twice |
| `header_extra_column.csv` | 1 | — | 0 | header mismatch: extra `order_type` |
| `header_wrong_case.csv` | 1 | — | 0 | header mismatch: capitalized names |
| `header_with_bom.csv` | 1 | — | 0 | header mismatch: UTF-8 byte-order mark |

### Record framing

| File | Line | Column | Dispatched | Broken rule |
|---|---|---|---|---|
| `quoted_field.csv` | 4 | — | 2 | quote character (`"ADD"`) |
| `too_few_fields.csv` | 4 | — | 2 | 5 fields |
| `too_many_fields.csv` | 4 | — | 2 | 7 fields (trailing comma) |
| `blank_line.csv` | 4 | — | 2 | empty record |
| `trailing_blank_line.csv` | 5 | — | 3 | empty record at end of file (`...\n\n`) |

### Sequence

| File | Line | Column | Dispatched | Broken rule |
|---|---|---|---|---|
| `sequence_repeated.csv` | 4 | `sequence` | 2 | 2 after 2: not strictly increasing |
| `sequence_decreasing.csv` | 4 | `sequence` | 2 | 4 after 6: not strictly increasing |
| `sequence_zero.csv` | 2 | `sequence` | 0 | zero |
| `sequence_not_a_number.csv` | 4 | `sequence` | 2 | `three` is not a number |
| `sequence_out_of_range.csv` | 4 | `sequence` | 2 | 18446744073709551616 exceeds 2^64 − 1 |

### Fields

| File | Line | Column | Dispatched | Broken rule |
|---|---|---|---|---|
| `command_unknown.csv` | 4 | `command` | 2 | `MODIFY` |
| `command_lowercase.csv` | 4 | `command` | 2 | `add` (case-sensitive) |
| `command_empty.csv` | 4 | `command` | 2 | empty |
| `order_id_zero.csv` | 4 | `order_id` | 2 | zero on `ADD` |
| `order_id_leading_zero.csv` | 4 | `order_id` | 2 | `01003` is not canonical |
| `add_missing_order_id.csv` | 4 | `order_id` | 2 | empty on `ADD` |
| `cancel_missing_order_id.csv` | 4 | `order_id` | 2 | empty on `CANCEL` |
| `cancel_order_id_zero.csv` | 4 | `order_id` | 2 | zero on `CANCEL` (a parse error, not `ORDER_NOT_FOUND`) |
| `side_unknown.csv` | 4 | `side` | 2 | `SHORT` |
| `side_lowercase.csv` | 4 | `side` | 2 | `buy` (case-sensitive) |
| `add_missing_side.csv` | 4 | `side` | 2 | empty on `ADD` |
| `add_missing_price.csv` | 4 | `price` | 2 | empty on `ADD`, never read as zero |
| `price_zero.csv` | 4 | `price` | 2 | zero |
| `price_not_a_number.csv` | 4 | `price` | 2 | `abc` |
| `price_decimal.csv` | 4 | `price` | 2 | `100.50`: prices are whole ticks |
| `price_negative.csv` | 4 | `price` | 2 | `-105` |
| `price_leading_plus.csv` | 4 | `price` | 2 | `+105` |
| `price_whitespace.csv` | 4 | `price` | 2 | ` 105` (no trimming) |
| `price_out_of_range.csv` | 4 | `price` | 2 | 4294967296 exceeds `Price` |
| `add_missing_quantity.csv` | 4 | `quantity` | 2 | empty on `ADD` |
| `quantity_zero.csv` | 4 | `quantity` | 2 | zero |
| `quantity_negative.csv` | 4 | `quantity` | 2 | `-20` |
| `quantity_out_of_range.csv` | 4 | `quantity` | 2 | 4294967296 exceeds `Quantity` |
| `cancel_with_side.csv` | 4 | `side` | 2 | `CANCEL` with `BUY` |
| `cancel_with_price.csv` | 4 | `price` | 2 | `CANCEL` with `0` (any value counts as filled in) |
| `cancel_with_quantity.csv` | 4 | `quantity` | 2 | `CANCEL` with `50` |
| `cancel_with_space.csv` | 4 | `side` | 2 | `CANCEL` with ` `: a space is not empty |
