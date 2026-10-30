# Replay sample files

Sample input for the CSV replay format defined in [`docs/replay-format.md`](../../docs/replay-format.md). `replay_parser_tests` reads every file here and checks the parsed records of each valid file and the line, column, and rule (`ParseErrorCode`) of each malformed one; it fails on a file it has no expectation for. `replay_engine_tests` replays every file and checks the events, statistics, and final book of each valid one and, for each malformed one, the error line and how many commands ran before it; it fails on a valid file it has no expectation for. The `replay_cli_*` tests run `order_book_cli replay --events --book` on every valid file (`<name>.stdout`) and on `price_zero.csv`, `market_gtc.csv`, `empty_file.csv`, and a missing file, and `--trades` on `stops.csv` (cascade trades tagged with the line that ran the cascade); they also cover each output selection (statistics only, `--trades`, `--book`), option order and `--`, help, the demo, and every usage error. Each compares the exit status, stdout, and stderr byte for byte with `expected/` (`*.stdout`, `*.stderr`, also `-text`), written by hand from the events below. A failing case with no command run prints the empty book's statistics (`empty_statistics.stdout`), or with `--events --book` its summary (`header_only.stdout`).

`.gitattributes` marks every `*.csv` here `-text`, so Git keeps the bytes exactly as committed, whatever `core.autocrlf` says. Several files depend on that: `crlf_line_endings.csv`, `no_final_newline.csv`, `header_with_bom.csv`, and `trailing_blank_line.csv`. Don't re-save these files in an editor that normalizes line endings or encoding.

## `valid/`

Every file here should replay to the end with exit status 0. The events are the `formatEvent` lines each command produces on a fresh book. They were checked against the engine when the files were written.

| File | Shows |
|---|---|
| `basic.csv` | the example from the spec: rest, rest, cross, cancel |
| `price_time_priority.csv` | FIFO within a level, a multi-level sweep, a resting remainder, execution at the resting price |
| `sweep_and_rest.csv` | a sweep stopped by its limit with the next ask out of reach; the remainder rests at its limit, not its last fill price, with the book left two-sided |
| `book_rejections.csv` | well-formed rows the book refuses, with the replay continuing; ID reuse after a cancel |
| `sequence_gaps.csv` | non-contiguous sequence numbers; a cancel moves the next order to the front of the queue |
| `limits.csv` | maximal ID, price, and quantity (4294967295) and maximal sequence (18446744073709551615) |
| `leading_zeros.csv` | leading zeros in every numeric column, ignored; sequence order compares values (`0010` follows `002`) |
| `header_only.csv` | header and no commands: replays nothing |
| `crlf_line_endings.csv` | CRLF record terminators |
| `no_final_newline.csv` | last record without a line ending |
| `v2_header_only.csv` | v2: the v2 header and no commands: replays nothing |
| `market_orders.csv` | v2: a market sweep with an expiring remainder, a market order facing an empty side, a market FOK that cannot fill (the book untouched) and one that does |
| `ioc_fok.csv` | v2: an IOC that trades what it can and expires the rest, a FOK that fills, a FOK that cannot fill (accepted, then expired, the book untouched) |
| `post_only.csv` | v2: post-only orders: one rests, ones at and through the touch on either side expire whole |
| `self_trade_prevention.csv` | v2: an owner-less order trading freely with an owned one; CANCEL_RESTING sweeping past its own orders into a deeper level; CANCEL_INCOMING; CANCEL_BOTH |
| `stops.csv` | v2: stops pending, cancelled while pending, past their trigger on arrival, a duplicate ID against a pending stop, a two-stop cascade tagged with the line that ran it, a triggered stop-limit resting, and an ID reused after its stop triggered |
| `replace.csv` | v2: replaces: a decrease keeping priority, an increase losing it, three refused (total too low, not found, a pending stop), a post-only replace that would take, a price change that clears the owner by omitting it, and a crossing replace whose trade triggers a stop |

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

`sweep_and_rest.csv`

```text
line 2  OrderAccepted id=8001 side=SELL price=10125 qty=50
line 3  OrderAccepted id=8002 side=SELL price=10125 qty=30
line 4  OrderAccepted id=8003 side=SELL price=10126 qty=100
line 5  OrderAccepted id=8004 side=SELL price=10128 qty=200
line 6  OrderAccepted id=8005 side=BUY price=10120 qty=100
line 7  OrderAccepted id=8006 side=BUY price=10127 qty=200
        TradeExecuted incoming=8006 resting=8001 price=10125 qty=50
        TradeExecuted incoming=8006 resting=8002 price=10125 qty=30
        TradeExecuted incoming=8006 resting=8003 price=10126 qty=100
final   bids: 10127 x 20 (8006), 10120 x 100 (8005)   asks: 10128 x 200 (8004)
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

`leading_zeros.csv`

```text
line 2  OrderAccepted id=7001 side=BUY price=100 qty=10
line 3  OrderAccepted id=7002 side=SELL price=100 qty=4
        TradeExecuted incoming=7002 resting=7001 price=100 qty=4
line 4  OrderCancelled id=7001 qty=6
final   empty
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

`v2_header_only.csv`: the v2 header and no commands; the book stays empty.

`market_orders.csv`

```text
line 2  OrderAccepted id=1 side=SELL price=100 qty=5
line 3  OrderAccepted id=2 side=SELL price=101 qty=5
line 4  OrderAccepted id=3 side=BUY price=MARKET qty=12
        TradeExecuted incoming=3 resting=1 price=100 qty=5
        TradeExecuted incoming=3 resting=2 price=101 qty=5
        OrderExpired id=3 qty=2
line 5  OrderAccepted id=4 side=SELL price=MARKET qty=3
        OrderExpired id=4 qty=3
line 6  OrderAccepted id=5 side=SELL price=102 qty=4
line 7  OrderAccepted id=6 side=BUY price=MARKET qty=5
        OrderExpired id=6 qty=5
line 8  OrderAccepted id=7 side=BUY price=MARKET qty=4
        TradeExecuted incoming=7 resting=5 price=102 qty=4
final   empty   last trade: 102
```

`ioc_fok.csv`

```text
line 2  OrderAccepted id=1 side=SELL price=100 qty=4
line 3  OrderAccepted id=2 side=SELL price=101 qty=6
line 4  OrderAccepted id=3 side=BUY price=100 qty=10
        TradeExecuted incoming=3 resting=1 price=100 qty=4
        OrderExpired id=3 qty=6
line 5  OrderAccepted id=4 side=BUY price=101 qty=6
        TradeExecuted incoming=4 resting=2 price=101 qty=6
line 6  OrderAccepted id=5 side=SELL price=99 qty=3
line 7  OrderAccepted id=6 side=BUY price=99 qty=5
        OrderExpired id=6 qty=5
line 8  OrderCancelled id=5 qty=3
final   empty   last trade: 101
```

`post_only.csv`

```text
line 2  OrderAccepted id=1 side=SELL price=101 qty=5
line 3  OrderAccepted id=2 side=BUY price=100 qty=4
line 4  OrderAccepted id=3 side=BUY price=101 qty=3
        OrderExpired id=3 qty=3
line 5  OrderAccepted id=4 side=BUY price=102 qty=2
        OrderExpired id=4 qty=2
line 6  OrderAccepted id=5 side=SELL price=100 qty=1
        OrderExpired id=5 qty=1
line 7  OrderAccepted id=6 side=SELL price=102 qty=2
final   bids: 100 x 4   asks: 101 x 5, 102 x 2   last trade: none
```

`self_trade_prevention.csv`

```text
line 2  OrderAccepted id=1 side=SELL price=100 qty=3
line 3  OrderAccepted id=2 side=SELL price=100 qty=4
line 4  OrderAccepted id=3 side=SELL price=101 qty=5
line 5  OrderAccepted id=4 side=SELL price=102 qty=6
line 6  OrderAccepted id=10 side=BUY price=100 qty=1
        TradeExecuted incoming=10 resting=1 price=100 qty=1
line 7  OrderAccepted id=11 side=BUY price=101 qty=20
        SelfTradePrevented incoming=11 resting=1 price=100 qty=2
        TradeExecuted incoming=11 resting=2 price=100 qty=4
        SelfTradePrevented incoming=11 resting=3 price=101 qty=5
line 8  OrderAccepted id=12 side=SELL price=101 qty=3
        SelfTradePrevented incoming=12 resting=11 price=101 qty=0
        OrderExpired id=12 qty=3
line 9  OrderAccepted id=13 side=SELL price=101 qty=2
        SelfTradePrevented incoming=13 resting=11 price=101 qty=16
        OrderExpired id=13 qty=2
final   bids: none   asks: 102 x 6   last trade: 100
```

`stops.csv`

```text
line 2  OrderAccepted id=1 side=SELL price=100 qty=1
line 3  OrderAccepted id=2 side=BUY price=100 qty=1
        TradeExecuted incoming=2 resting=1 price=100 qty=1
line 4  OrderAccepted id=20 side=BUY price=MARKET qty=2
line 5  OrderAccepted id=21 side=BUY price=103 qty=2
line 6  OrderAccepted id=22 side=SELL price=MARKET qty=1
line 7  OrderCancelled id=22 qty=1
line 8  OrderAccepted id=23 side=BUY price=MARKET qty=1
        OrderExpired id=23 qty=1
line 9  OrderRejected id=20 reason=DUPLICATE_ORDER_ID
line 10 OrderAccepted id=3 side=SELL price=102 qty=1
line 11 OrderAccepted id=4 side=SELL price=103 qty=1
line 12 OrderAccepted id=5 side=BUY price=102 qty=1
        TradeExecuted incoming=5 resting=3 price=102 qty=1
        StopTriggered id=20 stop_price=102 trade_price=102
        TradeExecuted incoming=20 resting=4 price=103 qty=1
        OrderExpired id=20 qty=1
        StopTriggered id=21 stop_price=103 trade_price=103
line 13 OrderAccepted id=20 side=SELL price=103 qty=2
        TradeExecuted incoming=20 resting=21 price=103 qty=2
final   empty   last trade: 103
```

`replace.csv`

```text
line 2  OrderAccepted id=1 side=SELL price=101 qty=10
line 3  OrderAccepted id=2 side=SELL price=101 qty=5
line 4  OrderAccepted id=3 side=BUY price=100 qty=4
line 5  OrderAccepted id=4 side=BUY price=101 qty=3
        TradeExecuted incoming=4 resting=1 price=101 qty=3
line 6  OrderReplaced id=1 side=SELL price=101 qty=8 open=5 priority=KEPT
line 7  OrderReplaced id=2 side=SELL price=101 qty=6 open=6 priority=LOST
line 8  OrderAccepted id=30 side=SELL price=MARKET qty=2
line 9  ReplaceRejected id=1 reason=REPLACE_QUANTITY_TOO_LOW
line 10 ReplaceRejected id=99 reason=ORDER_NOT_FOUND
line 11 ReplaceRejected id=30 reason=ORDER_NOT_REPLACEABLE
line 12 OrderReplaced id=2 side=SELL price=100 qty=6 open=6 priority=LOST
        OrderExpired id=2 qty=6
line 13 OrderReplaced id=3 side=BUY price=99 qty=4 open=4 priority=LOST
line 14 OrderReplaced id=1 side=SELL price=99 qty=8 open=5 priority=LOST
        TradeExecuted incoming=1 resting=3 price=99 qty=4
        StopTriggered id=30 stop_price=100 trade_price=99
        OrderExpired id=30 qty=2
final   bids: none   asks: 99 x 1   last trade: 99
```

## `malformed/`

Every file has exactly one error. The replay must stop there with exit status 1 and report the line and column below. It must not dispatch the bad record or anything after it, and it must not roll back what already ran.

Most files share one layout (in v2's columns for the v2 files, each of which uses the v2 header). Lines 2 and 3 are valid and get dispatched (`OrderAccepted` for 1001 BUY 100 x 50, then for 1002 SELL 105 x 30, with no trade). Line 4 is the bad record. Line 5, `4,CANCEL,1001,,,`, must never run, so after the stop both 1001 and 1002 are still resting. "Dispatched" below counts the commands that ran before the stop.

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
| `quantity_zero_padded.csv` | 4 | `quantity` | 2 | `000`: zero, however it is written |
| `quantity_negative.csv` | 4 | `quantity` | 2 | `-20` |
| `quantity_out_of_range.csv` | 4 | `quantity` | 2 | 4294967296 exceeds `Quantity` |
| `cancel_with_side.csv` | 4 | `side` | 2 | `CANCEL` with `BUY` |
| `cancel_with_price.csv` | 4 | `price` | 2 | `CANCEL` with `0` (any value counts as filled in) |
| `cancel_with_quantity.csv` | 4 | `quantity` | 2 | `CANCEL` with `50` |
| `cancel_with_space.csv` | 4 | `side` | 2 | `CANCEL` with ` `: a space is not empty |

### Version 2

Each breaks one v2 rule (`v1_replace.csv` uses the v1 header). The last column is the `ParseErrorCode`.

| File | Line | Column | Dispatched | Code |
|---|---|---|---|---|
| `v2_header_wrong_case.csv` | 1 | — | 0 | `INVALID_HEADER` |
| `v2_six_fields.csv` | 4 | — | 2 | `WRONG_COLUMN_COUNT` |
| `v1_replace.csv` | 4 | `command` | 2 | `UNKNOWN_COMMAND` |
| `order_type_unknown.csv` | 4 | `order_type` | 2 | `INVALID_ORDER_TYPE` |
| `order_type_lowercase.csv` | 4 | `order_type` | 2 | `INVALID_ORDER_TYPE` |
| `add_missing_order_type.csv` | 4 | `order_type` | 2 | `MISSING_REQUIRED_FIELD` |
| `time_in_force_unknown.csv` | 4 | `time_in_force` | 2 | `INVALID_TIME_IN_FORCE` |
| `add_missing_time_in_force.csv` | 4 | `time_in_force` | 2 | `MISSING_REQUIRED_FIELD` |
| `market_gtc.csv` | 4 | `time_in_force` | 2 | `INVALID_COMBINATION` |
| `stop_gtc.csv` | 4 | `time_in_force` | 2 | `INVALID_COMBINATION` |
| `market_with_price.csv` | 4 | `price` | 2 | `UNEXPECTED_FIELD` |
| `stop_limit_missing_price.csv` | 4 | `price` | 2 | `MISSING_REQUIRED_FIELD` |
| `stop_missing_stop_price.csv` | 4 | `stop_price` | 2 | `MISSING_REQUIRED_FIELD` |
| `limit_with_stop_price.csv` | 4 | `stop_price` | 2 | `UNEXPECTED_FIELD` |
| `stop_price_zero.csv` | 4 | `stop_price` | 2 | `INVALID_VALUE` |
| `post_only_n.csv` | 4 | `post_only` | 2 | `INVALID_POST_ONLY` |
| `post_only_ioc.csv` | 4 | `post_only` | 2 | `INVALID_COMBINATION` |
| `owner_zero.csv` | 4 | `owner` | 2 | `INVALID_VALUE` |
| `owner_without_mode.csv` | 4 | `self_trade_prevention` | 2 | `MISSING_REQUIRED_FIELD` |
| `mode_without_owner.csv` | 4 | `self_trade_prevention` | 2 | `UNEXPECTED_FIELD` |
| `mode_unknown.csv` | 4 | `self_trade_prevention` | 2 | `INVALID_SELF_TRADE_PREVENTION` |
| `v2_cancel_with_order_type.csv` | 4 | `order_type` | 2 | `UNEXPECTED_FIELD` |
| `replace_with_side.csv` | 4 | `side` | 2 | `UNEXPECTED_FIELD` |
| `replace_with_order_type.csv` | 4 | `order_type` | 2 | `UNEXPECTED_FIELD` |
| `replace_missing_price.csv` | 4 | `price` | 2 | `MISSING_REQUIRED_FIELD` |
| `replace_missing_quantity.csv` | 4 | `quantity` | 2 | `MISSING_REQUIRED_FIELD` |
| `replace_with_stop_price.csv` | 4 | `stop_price` | 2 | `UNEXPECTED_FIELD` |
