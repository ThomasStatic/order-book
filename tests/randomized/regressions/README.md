# Saved regression streams

Every `*.txt` file here is a saved command stream. The `randomized_regressions` CTest test replays each one exactly against a fresh book, with the full checks after every command, and fails on the first file that fails.

When `randomized_tests` finds a failure, it saves two streams to its `--out-dir` and prints their paths: the executed prefix (`<profile>-seed<seed>-prefix.txt`) and a minimized version of it (`...-minimized.txt`). Once the bug is fixed, copy the minimized stream here, under a name that says what it caught, so it stays a permanent regression test.

A saved stream doesn't depend on the generator: it replays the same commands even after the generator or a profile changes. A seed reproduces its stream only under the generator version it was found with. So this directory, not the seed list in `randomized_main.cpp`, is the durable record.

Format: one command per line (`SUBMIT id=17 side=BUY price=10002 qty=5 intent=NEW_ID`, `CANCEL id=291 intent=CANCEL_ACTIVE`). A field prints only when it differs from its default: `type=N` for a type other than `LIMIT`, `status=FILLED`, and `overload=ORDER` for a submission through `submitOrder(Order)` rather than `submitOrder(OrderRequest)` (`status=FILLED` needs it). Header lines are `key value` (`format`, `generator`, `profile`, `seed`, `config`, `note`), and `#` starts a comment. `format 1` is required; the other headers are informational. Replay one with `randomized_tests --replay <file>`.
