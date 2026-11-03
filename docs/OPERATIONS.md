# Running order_book_engine

What an operator needs to start the engine, stop it, and read how it ended. The engine has no persistence yet, so a restart loses every book: nothing below about a stop protects a book until the command log lands.

## Starting

```text
$ order_book_engine --instruments list.csv --shards 2 --listen 127.0.0.1:9000
listening 127.0.0.1:9000
```

`--instruments`, `--shards`, and `--listen` are required, and `--listen` has no default, so a gateway with no authentication is never exposed by accident. Port 0 lets the system pick, and the `listening` line says which it did. `-h` or `--help` anywhere on the line prints the usage, with every flag and its value's form, on stdout and exits 0. A usage error prints one line saying what was wrong, then the usage, on stderr, and exits 2.

- **stdout** is for a machine: one flushed `listening ADDR:PORT` once every shard is ready, then the stop report after a clean stop.
- **stderr** is for a person, one line each, prefixed `order_book_engine:`, with no timestamps, which journald or a terminal can add. The first line is the run's record: every flag with its value, defaults included, so it would give the same run.

### Gateway threads

`--gateway-threads M`, 1 by default and never worked out from the machine, sets how many threads serve connections. Gateway thread 0 owns the listener and hands each new connection to the next thread in turn, itself included, and a connection stays on its thread until it closes. `--max-connections` is per thread: past it, the thread a connection was handed to accepts it only to send `TOO_MANY_CONNECTIONS` and close, even if another thread has room. Since connections are handed out in turn and closed ones aren't, a thread can end up with more of the long-lived ones than another.

### Pinning and priority

Off by default.

- `--pin-matching CORES` pins each shard's matching thread to a core, given as a comma list with exactly one core per shard, in shard order. `--pin-gateway CORES` does the same for the gateway threads, with exactly one core per gateway thread. A core may repeat. Cores are the system's logical processor numbers. On Windows only the first 64 can be named.
- `--thread-priority high` raises the matching and gateway threads: `THREAD_PRIORITY_HIGHEST` on Windows, a nice value of -10 on Linux, never a real-time policy, since a thread spinning under one can starve everything else on its core. Windows lets any user do this. Linux needs root, `CAP_SYS_NICE`, or an `RLIMIT_NICE` of at least 30 (`ulimit -e`, or `LimitNICE=` under systemd).

Each thread pins and raises itself before the engine is ready. If the system refuses either, the start is refused with exit 1 and the system's reason, naming the thread and core for a pin (`cannot pin matching thread 1 to core 5: ...`, or `gateway thread 1` with several) or the setting for a priority (`--thread-priority high was denied: ...`), so a run's record never claims a setting that didn't apply.

## Stopping

A **first stop** starts the clean stop: the listener closes, along with any connection not yet taken on by its gateway thread, every command already queued is answered, commands arriving after it are refused with `ENGINE_STOPPING`, and each gateway thread flushes what it owes for at most `--stop-grace` (5 s by default) before closing what's left. Then the stop report prints and the engine exits 0.

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
| 6 | Stopped before ready: `order_book_engine: stopped before ready` | Start again |
| 8 | Forced stop: `order_book_engine: forced stop` | Start again |

Codes 4, 5, and 7 come with persistence: a changed placement, a recovery that stopped, and a clean stop that couldn't publish a book.

### The stop report

`key=value` lines on stdout after a clean stop, so a harness can parse them:

```text
stopped
book AAPL shard=0 orders=4096 stops=1024 levels=256 growths=0
book MSFT shard=1 orders=4096 stops=1024 levels=256 growths=2
shard 0: result-growths=0
shard 1: result-growths=0
total: books=2 capacity-growths=2
```

One `book` line per book, sorted by symbol, with the capacity it was warmed to and how many times it grew since ready, then a line per shard, then the total last. A book that grew needs a larger capacity in the list or the defaults.

### An abort

The engine aborts rather than go on with a book it can no longer trust: an exception escaping a command, or a failed check. The failing path prints what it knows first, then the abort handler prints one line. In a build with checks on, it names where the shard stood:

```text
order_book_engine: aborted: shard 2 symbol AAPL position 12345
```

A Release build records nothing for it, so the line is only `order_book_engine: aborted`. The process then dies as an abort does: by SIGABRT on Linux, and with status 3 on Windows, where the crash dialog is turned off.
