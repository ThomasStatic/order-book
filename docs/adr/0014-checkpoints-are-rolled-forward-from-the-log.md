# Checkpoints are rolled forward from the log

A checkpoint is made from the last checkpoint and the log, never from a live book, except at a clean stop. The log is a series of fixed-size files. When one closes, a worker away from the matching thread scans it once, restores each book that has commands in it, replays them, proves each result restores, and publishes every book in the file at the file's end position. A book with no commands in the file gets a new wrapper at that position around the same image, so a quiet book never holds back the log. This narrows ADRs 0008 and 0009, which had the matching thread copy each image: the copy took 8.6 ms at 100k orders and 80 ms at 860k, every queued command waited through it, and a checkpoint's cost is the one place the matching thread would stop for milliseconds. Those ADRs' file format, one file per book at its own position, `restore`, and the rule that a checkpoint is published only once proved still stand.

The worker replays at 84 to 93% of a book's direct speed and takes 8 to 11% off the matching thread's throughput while it runs, through shared cache and memory. Those numbers come from a throwaway Release (MSVC) program outside the repo, in one session on one machine, with no Linux run. They are not a recorded baseline.

## Considered options

- **Copying from the live book, then checking the copy off-thread** (ADR 0009). Checks the live book at every checkpoint, but pauses matching for as long as the copy takes, needs a buffer sized to the largest book on the matching thread, and needs a rule for skipping a book when that buffer is too small.
- **A standing shadow book per book**, following the log on another thread. Needs no restore per pass, but doubles the books' memory all the time to save about 0.4 s per closed file at 860k orders. A worker uses that memory only during a pass.
- **A pass per book.** Moves one book at a time, but each pass scans the shard's whole file, which at a thousand instruments is a thousand scans.

## Consequences

- A matching thread never pauses for a checkpoint, and the only image it ever writes is at a clean stop. `writeImage` otherwise runs on the worker, on the worker's own book.
- In Release, nothing reads a live book until a clean stop. A deterministic bug still shows, because the worker runs the same commands through the same code and checks the result, but random damage to live memory goes unnoticed, and cannot reach a checkpoint either. In exchange, the log and the live book are compared at every clean stop, byte for byte, which proves determinism at no running cost. A mismatch publishes nothing, and the log stays the truth.
- Cadence is one setting, the log file size, and forcing a checkpoint online is not offered. A worker reads only closed files, which have been flushed, so no checkpoint can stand past what the disk holds, even at `process` level.
- Under sustained saturation a worker can fall behind, closed files pile up, and recovery gets longer. A counter reports it, and a clean stop must roll the backlog before it can finish.
- A background image flush on the log's drive slows the log's writes: p99 rose from about 110 µs to 5 to 7 ms while a flush ran nonstop, and a full ring cut throughput to 2 to 3 M/s. So the logs and checkpoints have separate directory settings, and only a separate drive protects that p99.
- Every instrument has a checkpoint from its first start, an empty one if need be, so "never checkpointed" is not a case. A listed instrument with no checkpoint that a log header names is a lost file, and start-up stops.
- Two checkpoints are kept per book, and the log reaches back to the older one, so a file that fails at start falls back to it. That is three log files per shard in normal running.
