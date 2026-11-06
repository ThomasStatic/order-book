# Running order_book_engine

What an operator needs to start the engine, stop it, and read how it ended. The engine logs every command, and a start recovers every book from the log an earlier run left, crashed or stopped.

## Starting

```text
$ order_book_engine --instruments list.csv --shards 2 --listen 127.0.0.1:9000 --log-dir logs
listening 127.0.0.1:9000
```

`--instruments`, `--shards`, `--listen`, and `--log-dir` are required, and `--listen` has no default, so a gateway with no authentication is never exposed by accident. Port 0 lets the system pick, and the `listening` line says which it did. `-h` or `--help` anywhere on the line prints the usage, with every flag and its value's form, on stdout and exits 0. A usage error prints one line saying what was wrong, then the usage, on stderr, and exits 2.

- **stdout** is for a machine: one flushed `listening ADDR:PORT` once every shard is ready, then the stop report after a clean stop.
- **stderr** is for a person, one line each, prefixed `order_book_engine:`, with no timestamps, which journald or a terminal can add. The first line is the run's record: every flag with its value, defaults included, so it would give the same run.

### The command log

`--log-dir DIR` is where the log goes: each shard writes every command it runs to its own file in that directory, `log-<shard, 4 digits>-<first position, 20 digits>.oblog`, through a log writer thread of its own. A command still matches at once, but its answer goes out only once its command is in the log, so a crash can lose only commands nobody was told about. `--durability` says what "in the log" means:

- **`machine`** (the default): written through to the drive (`FILE_FLAG_WRITE_THROUGH` on Windows, `O_DSYNC` on Linux), so an answered command survives the engine crashing and the machine crashing too, as far as the drive keeps its word.
- **`process`**: handed to the system, and flushed when the file closes, so an answered command survives the engine crashing but not the machine.

Each shard's log is a run of files of `--log-file-size` (256 MiB by default, with `K`, `M`, or `G` if you like), each holding its header and as many whole commands as fit after it, 40 bytes each, so 6.7M at the default; a size that can't hold one command refuses the start. A file is named for its first position, so the files of a shard sort in order and each starts where the last ended. A housekeeping thread makes each shard's next file ahead, under its name plus `.tmp`, then renames it, so the log writer never waits for a file unless the drive is slow. The writer flushes a file it fills before it writes to the next, so only the last file can lose anything at `process` level. If the next file can't be made, the drive full for instance, the engine aborts at once, naming it, while the current file still has room, since the log could never go on. Files are never reused: every start opens new ones.

`--log-ring RECORDS`, 65,536 by default and a power of two, is how many commands per shard may wait for their log writer, and so the most that can be unsafe at once. When it fills, the shard waits, its rings fill, and connections are parked: a slow drive reaches clients as backpressure, and nothing is refused.

The directory must exist. It holds `engine.lock`, an empty file under an OS file lock while an engine runs there, which the system lets go of when the process ends, however it ends, so a second engine on a directory in use is refused with exit 1 (`start refused: <lock file> is locked: another engine or command is using <dir>`), and a crash never leaves it held. It is never deleted. A start that is refused or stopped before it is ready removes the log files and empty checkpoints it made. A log that can't be written or flushed once the engine runs ends it with an abort, naming the shard and file, since no write can be trusted after one fails.

A clean stop answers everything queued, then each log writer writes the rest, cuts its last file to its last command, flushes, and closes it, and the unused next files are deleted, so, with `--keep-log`, `order_book_cli replay DIR --book` replays the engine's books from the log directory; without it, the files retention deleted are gone, and replay refuses a log that doesn't start at 0. A last file that still has zeros at its end means the engine did not stop cleanly. Each shard's line in the stop report then starts with `log-end=`, where its log ends.

### Recovery

Every start with a log already in the directory recovers it before anything listens, though the port is bound first, so a busy port fails at once rather than after a long recovery. Each book restores from its newest checkpoint, or its older one if the newest doesn't restore, and each shard replays only the log after its oldest book's checkpoint, so a start replays about a file per shard, plus any closed files the workers hadn't reached.

1. The engine takes the lock, then reads every log file's header. A symbol in one that isn't on the instrument list refuses the start with exit 1. A shard count other than the log's, or an instrument on another shard than the log's, is a placement change, which needs the last run to have stopped cleanly and is otherwise refused with exit 4 (see "Changing placement" below). A new instrument is none of these. A file a crash left being made, `.tmp`, held nothing anyone was told of, and is deleted.
2. Each shard, on its own thread, restores each book from its newest checkpoint, or its older one, deletes trailing log files with no commands in them, such as the next file made ahead, then replays the log from its oldest book's checkpoint into its books, passing each command a book's checkpoint already holds, checks every book, and warms each to the larger of what the list declares and 1.5 times what it holds, each of orders, stops, and levels apart; the stop report's `book` lines give what each was warmed to. If the last file with commands ends in a torn tail, the zeros an unstopped log leaves, or a command cut short as it was written, the tail is cut off and flushed. So is a hole that valid commands follow, if it lies within 1 MiB of the end of what that file holds: that is a write a machine crash tore, some of its sectors reaching the drive and others not, and none of the commands from the hole on were ever answered at `machine`. A file the cut leaves with no command is deleted. Each shard then starts a new file where its log ended.
3. Each shard says what it did, in shard order, before `listening` prints: `order_book_engine: shard 0: recovered books=3 replayed=120 tail-cut-bytes=40 from=96 fell-back=0`, where `replayed` is where the log ends, `from` where the replay began, `fell-back` how many books restored from their older checkpoint, and `tail-cut-bytes` counts the bytes a write had reached that recovery dropped, a torn command's, and not the zeros past them, so 0 means nothing was torn.

A start also refuses a checkpoint naming a symbol that isn't on the list, with exit 1, and stops with exit 5 on a lost checkpoint, an instrument a log file names that no checkpoint holds (`recovery stopped: shard 0 symbol MSFT: a log file's header names it, and no checkpoint holds it: a lost checkpoint`), on a book none of whose checkpoints restores, and on a checkpoint standing past the log's end, which means the disk lost data it had confirmed. An instrument added to the list, named in no log file and with no checkpoint, is new and starts empty. The recovery line ends `from=` and `fell-back=`: where the replay began, and how many books restored from their older checkpoint.

Anything else recovery finds in the log, it can't explain, so it stops with exit 5 and changes nothing more, naming where: `order_book_engine: recovery stopped: shard 0 position 4 file log-0000-00000000000000000008.oblog: a gap: ...`. That covers a damaged header, a damaged command, a torn tail or a hole in a file before the last, which its log writer flushed whole, a hole further back than 1 MiB from the end, which no crash explains, a missing file at or after the oldest book's checkpoint (one before it is no longer needed), and a book that replay left broken. The message's cause is the reader's finding, such as `CORRUPT_RECORD: a record is damaged, and valid records follow it, with 2097160 bytes of records from it to the end of what was written, more than one write's 1048576`. The log is left as recovery found it; whether to cut it there is the operator's choice, below. A stop during recovery is enough on its own: the start ends with exit 6, and nothing is harmed.

### When recovery stops: verify, then maybe cut-log

With the engine stopped (each command takes the directories' locks, and is refused with exit 1 while anything else holds them):

1. **Find the record.** `order_book_cli verify --log-dir DIR [--checkpoint-dir DIR]` runs a start's checks and replay without changing anything, and stops where a start would, with the same message on stderr, exit 1, after the lines of the shards it got through. A store a start would change placement on first is refused, since following it would mean writing. A damaged command names its shard, position, and file. A *poison command* is one that breaks a book the same way on every replay: in a build with checks on (Debug), the start's abort names the last command that ran; in Release, a start and `verify` can name only the range the book broke in, `shard 0 positions 24 to 30 symbol AAPL: replay left the book broken: ...; verify --validate per-command finds the command`. `verify --validate per-command` checks every book after each command it runs, and exits 3 naming the exact one: `shard 0 position 27 file <name> symbol AAPL command <row>: the book fails its check after it: <why>`. Keep its output for the report.
2. **Decide.** There is no skipping one record: later commands ran on a book that included it, and clients were told their results. Cutting removes the record *and everything after it* from the log, so those commands' answers no longer hold. Ask whether the clients that sent them can resend.
3. **Cut.** `order_book_cli cut-log --log-dir DIR --shard S --position P`, with the shard and position `verify` named, moves that record and every one after it to `cut-<shard>-<position>.oblog` beside the log, keeping every command's bytes, a hole and the records after it included: a log of its own under the header of the file the record was in, readable with `order_book_cli replay` up to any damaged record, which it holds as found. Each record's 4-byte checksum is sealed anew in the cut file's own chain, as far as the old chain held, since a checksum chains from the file's header; the originals follow from the log, and a damaged record and those after it keep theirs. It prints `moved records=N shard=S to <file>`. It refuses, exit 1, if a checkpoint of that shard stands past the position, since that checkpoint holds the commands being cut; after a crash before any roll there is none, but a roll's checkpoint past a poison command can't exist, since the roll's proof would have aborted on it. It also refuses if the checkpoint directory holds none of the shard's checkpoints, which means `--checkpoint-dir` was left off. If it is cut short, run it again: it finishes the cut when the log holds what the cut file does, and once the cut is done it says so again.
4. **Check, then start.** `verify` again should print `ok`, its shard line ending at the cut. Then start the engine as before: the shard's log goes on from the cut position, and the `cut-` file is never read by a start or by `replay <log-dir>`.

`order_book_cli checkpoint roll --log-dir DIR [--checkpoint-dir DIR]` rolls checkpoints forward over the closed log files offline, as the workers would, so a start after a long crash has less to replay. It runs a start's checks of the store first, prints `rolled files=N books=M`, and exits 1 if they refuse it, if a checkpoint couldn't be published, or if a log file or checkpoint couldn't be read, as it reports on stderr; `verify` then says what a start would make of the store. It exits 3, rather than aborting, if a rolled book fails its proof, which is a bug to report with the log.

### A real power cut cannot be tested here

What survives a crash is tested only against a model of the drive: an engine crash keeps everything written, and a machine crash keeps what was flushed or written through and, of anything else, whole pages, none, or the first sectors of a page, chosen at random. The kill-and-restart test kills a real engine, which is an engine crash; nothing here pulls the plug on a machine. Whether a drive keeps what it said it had flushed, how it tears a write, and whether a file system keeps a rename it said was flushed are the drive's and the file system's promises, and the only check of them is a real power cut on the hardware the engine runs on. `machine` durability is exactly as good as those promises: a drive with a volatile write cache that ignores flushes can lose answered commands, which recovery would then report as a gap or a stop, or not see at all.

### Worst cases

Every answer waits for its command to reach the log, so whatever slows the log's drive slows every answer. These are the cases that slow it most. The figures are from v2.1.0's recorded set, on one machine with one NVMe drive ([`LATENCY.md`](LATENCY.md), [`BENCHMARKS.md`](BENCHMARKS.md#the-v210-engine-set-2026-11-06)), and say what that machine did, not what another will.

- **Only a separate drive protects the p99 from an image flush.** A roll worker writes and flushes each book's image in the checkpoint directory, which defaults to the log's, whenever a log file closes. Publishing one checkpoint, image and flush, took a mean of 1.25 ms for a 1,000-order book and 35.0 ms for a 1,000,000-order one. On the log's drive, that flush makes the log's own writes wait, and the slowest answers wait with them. Put checkpoints on another drive (`--checkpoint-dir`) to keep the two apart. The reference machine has one drive, so no second drive has been measured.
- **A roll worker needs a core.** Rolling replays every command of a closed file, so a worker is about as busy as the matching thread it follows. In the benchmark, a worker sharing the log writer's logical processor, which was the matching thread's sibling, nearly halved that shard's throughput ([#108](https://github.com/ThomasStatic/order-book/issues/108)). The engine doesn't pin its roll workers. Leave them a core the matching and gateway threads don't use.
- **Anything else writing heavily to the log's drive** does the same: another program, a copy, a backup, an indexing or antivirus scan. During such a burst the log's writes slow enough that the log ring can fill.
- **A drive that stalls stalls every answer on its shards** for as long as it stalls. The log ring fills, the shard waits, its rings fill, and connections are parked. Nothing is refused and nothing times out, so a client sees only answers arriving late. `--log-ring` bounds how many commands that is per shard, not how long it lasts.
- **At `machine`, each write waits for the drive.** A log writer has no batching timer: each write takes every command queued since the last, up to 1 MiB, so under load commands share a write, and an idle engine pays a whole write-through for one command. At `process` a write waits only for the system, and a machine crash can lose answered commands.
  - **Closed loop.** Over loopback through the engine, one command at a time and one connection, the recorded round trip had a p50 of 12.34 µs and a p99 of 19.30 µs at `process`. At `machine` they were 92.95 µs and 160.69 µs.
  - **Under load.** At `process`, the round trip's p50 stayed between 12.93 µs and 22.05 µs at every rung up to 90% of the capacity it measured, the highest at the lowest rate, 7,792 a second, whose 128 µs gap outlasts the 100 µs spin window. At `machine` it stayed between 101.35 µs and 106.08 µs up to half.
  - **Past half capacity at `machine`.** p99 rose to 73.1 ms at 75% and 121.0 ms at 90%: the drive's write-through stalls on files extended as the engine makes them ([`LATENCY.md`](LATENCY.md#10ks-durable-wait)).
  - **The workload matters.** On the cheapest workload, `quote-refresh`, the `queue` benchmark kind ran at 4.27M commands/s at `process` on extended files. That was one shard, one producer, and 8 instruments, each thread on a physical core of its own, the log writer on the matching thread's sibling, at spin window `100us`. At `machine` its three sessions read 1.25M, 0.63M, and 4.58M: the drive's stalls, not the engine, set that rate, and unevenly.

### Gateway threads

`--gateway-threads M`, 1 by default and never worked out from the machine, sets how many threads serve connections. Gateway thread 0 owns the listener and hands each new connection to the next thread in turn, itself included, and a connection stays on its thread until it closes. `--max-connections` is per thread: past it, the thread a connection was handed to accepts it only to send `TOO_MANY_CONNECTIONS` and close, even if another thread has room. Since connections are handed out in turn and closed ones aren't, a thread can end up with more of the long-lived ones than another.

### Pinning and priority

Off by default.

- `--pin-matching CORES` pins each shard's matching thread to a core, given as a comma list with exactly one core per shard, in shard order. `--pin-gateway CORES` does the same for the gateway threads, with exactly one core per gateway thread. A core may repeat. Cores are the system's logical processor numbers. On Windows only the first 64 can be named.
- `--pin-log-writers CORES` pins each shard's log writer, with exactly one core per shard.
- `--thread-priority high` raises the matching and gateway threads, and the log writers: `THREAD_PRIORITY_HIGHEST` on Windows, a nice value of -10 on Linux, never a real-time policy, since a thread spinning under one can starve everything else on its core. Windows lets any user do this. Linux needs root, `CAP_SYS_NICE`, or an `RLIMIT_NICE` of at least 30 (`ulimit -e`, or `LimitNICE=` under systemd).

Each thread pins and raises itself before the engine is ready. If the system refuses either, the start is refused with exit 1 and the system's reason, naming the thread and core for a pin (`cannot pin matching thread 1 to core 5: ...`, `log writer 1`, or `gateway thread 1` with several) or the setting for a priority (`--thread-priority high was denied: ...`), so a run's record never claims a setting that didn't apply.

## Stopping

A **first stop** starts the clean stop: the listener closes, along with any connection not yet taken on by its gateway thread, every command already queued is answered, once it is in the log if there is one, commands arriving after it are refused with `ENGINE_STOPPING`, each log writer finishes its file, and each gateway thread flushes what it owes for at most `--stop-grace` (5 s by default) before closing what's left. With a log, each matching thread then checks its books in full and writes their images, and the roll-forward workers roll each log to its end, the last short file included, and compare every book with its live one byte for byte, publishing a checkpoint at the log's end for each that matches. Then the stop report prints and the engine exits 0, or 7 if some book was not published. A clean stop therefore replays each shard's last file, and any closed file the workers had not reached, once more before it exits.

A **second stop** exits at once with 8 and prints `order_book_engine: forced stop`. What was still queued gets no answer, and no stop report prints. Use it when a client that never reads holds the clean stop for the whole grace.

A stop before the engine is ready ends the start: one is enough, it exits 6, and nothing was harmed.

What counts as a stop:

- **Linux:** SIGINT, SIGTERM, and SIGHUP, so Ctrl-C, `kill`, and `systemctl stop` each count. SIGPIPE is ignored.
- **Windows:** Ctrl-C, Ctrl-Break, closing the console window, logging off, and shutting down.
- **Both, when asked:** with `--stop-on-stdin-close`, closing the engine's stdin counts too. It is the one stop that works the same on both platforms, so it suits a parent process holding a pipe. It is opt-in, so it never fires under a service manager that closes stdin.

### Closing the console window can cut a clean stop short

On Windows, closing the console window, logging off, or shutting down starts the clean stop, but the system ends the process when its own timeout passes: 5 s by default for each, and the setting can differ on a given machine. A stop that needs longer, such as one waiting out a 5 s stop grace for a slow client, is then cut short, the same as a forced stop. Stop the engine with Ctrl-C or Ctrl-Break instead, which have no timeout, and leave the window open until it exits.

## How it ended

| Exit | Meaning | What to do |
|---|---|---|
| 0 | Clean stop | Nothing |
| 1 | Start refused: `order_book_engine: start refused: <cause>` | Fix the cause and start again |
| 2 | Usage error, with the usage | Fix the command line |
| 3 | Never returned; on Windows, an abort | Investigate |
| 4 | Start refused: `order_book_engine: start refused: placement changed: <what moved> and the last run did not stop cleanly; restart on the old placement and stop cleanly first` | Start on the old placement, stop it cleanly, then change it |
| 5 | Recovery stopped: `order_book_engine: recovery stopped: shard <n> position <p> file <name>: <cause>` | Run `order_book_cli verify`, then maybe `cut-log`: "When recovery stops" above; the log is left as recovery found it |
| 6 | Stopped before ready: `order_book_engine: stopped before ready` | Start again |
| 7 | Clean stop finished, but a book was not published: `order_book_engine: clean stop incomplete: <symbol> <why>; the next start recovers from the log` | Investigate; the next start recovers that book from the log |
| 8 | Forced stop: `order_book_engine: forced stop` | Start again: recovery does the rest |

### Checkpoints

A checkpoint is one book's image at a position in its shard's log, `ckpt-<symbol>-<position, 20 digits>.obckpt` in the checkpoint directory, the log's unless `--checkpoint-dir` says ([`checkpoint-format.md`](checkpoint-format.md)). Matching never pauses for one: as each log file closes, a roll-forward worker restores the books that have commands in it from their last checkpoints, replays the file on them, proves each result restores to the same bytes, and publishes every book of the file's shard at the file's end, a book with no commands in it getting a new wrapper around the same image. `--roll-workers N`, 1 by default and at most `--shards`, sets how many workers there are, each rolling every N-th shard. Each book keeps its two newest checkpoints. A new instrument, one no checkpoint and no log file names, gets an empty-book checkpoint where its shard's log ends, before the next file opens. `order_book_cli checkpoint verify <file>` checks one, and `checkpoint describe <file>` prints its headers.

A checkpoint that can't be written, the drive full for instance, prints `order_book_engine: shard 0: checkpoint <name> not published: <why>; <symbol> stays at its last checkpoint`, and trading goes on; that book is rolled again when the next file closes. A pass that runs out of memory prints that it was lost and is made again then too. A log file or checkpoint that can't be read prints `order_book_engine: shard 0: roll forward stopped: <why>`, uncounted, and the next file's close tries again; a checkpoint that no longer decodes, met by a worker or by the next start, is removed, which is printed, and its book rolls on from its older one. The housekeeping thread deletes a closed log file once every book's older kept checkpoint stands at or past its end, so a start that falls back to an older checkpoint still finds the log back to it, and a shard normally holds about three files. `--keep-log` keeps every file. `--checkpoint-dir DIR` puts the checkpoints, with a lock of their own, in another directory, which must exist (`start refused: cannot use checkpoint directory <dir>: no such directory`) and needs `--log-dir`; it defaults to the log directory, and a separate drive keeps an image's flush off the log's writes.

### Changing placement

A shard count or an instrument's pin can change between runs, but only after a clean stop that exited 0: every book's checkpoint must stand at its shard's log end, and every log must end as a clean stop leaves it, with no torn tail. The start then moves each book whose shard changed, or all of them for a new shard count, rewrapping its checkpoint with its new shard and that shard's log end, and printing `order_book_engine: placement changed: MSFT from shard 1 of 2 at 3 to shard 0 of 2 at 2` for each. A moved book's older checkpoint is dropped, since its position means nothing in its new log, and so is that of every book on a shard some book left, whose log after it holds the moved book's records; under a new shard count that is every book. Until its next file closes, such a book has one checkpoint. After a crash, or a clean stop that exited 7, the start is refused with exit 4: start on the old placement, stop cleanly, and change it then. Adding instruments needs none of this.

A change cut short, by a crash or a stop during the start that makes it, is finished by starting on the new placement again. A start on the old one is refused once any book was rewrapped: `start refused: placement changed: <checkpoint> was written by shard 1 of 3, and this start places AAPL on shard 0 of 2, though the logs' headers name this placement: a placement change was cut short, so start on the placement that checkpoint names to finish it`.

Fewer shards leave the logs of the shards that went. No shard writes them, recovery and the workers don't read them, and retention never deletes them, but every start still reads their headers, so a symbol they name can't leave the instrument list while they stand. Going back up to that many shards makes such a log a shard's own again, its books placed where it ended. Delete them by hand once the books stand elsewhere, if the count won't go back up. `order_book_cli replay <log-dir>` refuses a log in which a symbol appears under two shards, since their positions can't be compared; replay each shard's files on their own.

### Exit 7: a clean stop that didn't publish a book

A clean stop compares each live book with what the log rebuilds of it, byte for byte, which is the check that the engine is deterministic in a Release build. Each book not published prints one line, and the log stays the truth: the next start recovers that book from its last checkpoint and the log after it.

- `clean stop incomplete: AAPL differs at byte 812, saved ckpt-AAPL-00000000000000000120.live; ...`: the live book drifted from its log, a bug. The live image is saved beside the checkpoints, in the checkpoint format, so `order_book_cli checkpoint describe` reads it; no start reads it. Keep it, and the log, for the report.
- `clean stop incomplete: AAPL's image buffer could not be allocated; ...`: memory ran out at the stop, so nothing was compared.
- `clean stop incomplete: AAPL's checkpoint at 120 could not be published; ...`: it matched, and writing it failed, as printed above it.
- `clean stop incomplete: AAPL could not be rolled forward to the log's end, 120; ...`: an earlier checkpoint failed, or a pass was lost, as printed above it.

A book whose live state breaks its own invariants at the stop, such as an index entry the book's orders don't match, aborts instead, naming the shard and symbol, since no image holds that state for a comparison to see.

### The stop report

`key=value` lines on stdout after a clean stop, one that exits 7 included, so a harness can parse them:

```text
stopped
book AAPL shard=0 orders=4096 stops=1024 levels=256 growths=0
book MSFT shard=1 orders=4096 stops=1024 levels=256 growths=2
shard 0: log-end=12 result-growths=0 checkpoint-writes-failed=0 passes-lost=0 files-behind=0
shard 1: log-end=40 result-growths=0 checkpoint-writes-failed=0 passes-lost=0 files-behind=0
total: books=2 capacity-growths=2 checkpoint-writes-failed=0 passes-lost=0
```

One `book` line per book, sorted by symbol, with the capacity it was warmed to and how many times it grew since ready, then a line per shard, with where its log ends, how many of its checkpoints failed to publish, how many passes were lost to memory, and how many log files some book still needs rolled, then the total last. A clean stop rolls what is left, so `files-behind` is 0 unless a book went unpublished. A book that grew needs a larger capacity in the list or the defaults.

### An abort

The engine aborts rather than go on with a book it can no longer trust: an exception escaping a command, or a failed check. The failing path prints what it knows first, then the abort handler prints one line. In a build with checks on, it names where the shard stood:

```text
order_book_engine: aborted: shard 2 symbol AAPL position 12345
```

A Release build records nothing for it, so the line is only `order_book_engine: aborted`. The process then dies as an abort does: by SIGABRT on Linux, and with status 3 on Windows, where the crash dialog is turned off.
