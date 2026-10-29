# Replay format v2 is a second exact header, and a parsed command is always well formed

The replay CSV gains the new behaviors through a second exact header, v1's six columns followed by `order_type,time_in_force,stop_price,post_only,owner,self_trade_prevention`, and the reader picks the layout from line 1. A v1 file parses exactly as before and means v1 defaults, and every record of a v2 file has exactly twelve fields. The exact header was already the format's version marker, so this keeps the rule that a reader which doesn't know a layout fails at line 1 instead of misreading rows. Every request check that can be decided from the row alone (a price on `MARKET` or `STOP`, a missing or misplaced stop price, `MARKET` or `STOP` with GTC, post-only on anything but a GTC `LIMIT`) is a parse error that stops the replay, not a book rejection. That extends v1's rule that a parsed command satisfies the book's preconditions: v1 already refuses a zero price in the parser although the book has `INVALID_PRICE`. So a parsed `ADD` is always a well-formed `OrderRequest`.

## Considered options

- **Optional trailing columns** (6 to 12 fields per record). They break the fixed field count, and a missing comma would silently shift a field into the wrong column.
- **One `key=value` extras column.** That puts a second syntax inside a field, with its own quoting and ordering questions.
- **Leaving malformed combinations to the book**, so a replay could produce `OrderRejected` with `POST_ONLY_NOT_ALLOWED` and the like. That would break v1's guarantee and split "decidable from the file" between two layers.

## Consequences

- A replay file cannot exercise the book's new malformed reasons. `submission_tests` and the randomized harness cover them, and a test pins that the book accepts every parsed `ADD` as well formed.
- Empty means absent, and zero is still refused in every column, so a missing owner is written empty, never `0`, although `OwnerId` 0 is legal in the library.
- A later layout is a third exact header. Converting a v1 file to v2 is mechanical: append `LIMIT,GTC,,,,` to each `ADD` and `,,,,,,` to each `CANCEL`.
