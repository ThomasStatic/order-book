# A response waits for the log, but matching does not

A command matches as soon as its matching thread takes it, and only its response waits until the command is durable. The log's order is fixed at the take, and a response is the only effect anyone can see, since a connection hears only its own commands' events (ADR 0012). So a crash can lose only commands nobody was told about, and matching speed never depends on the disk. Each shard has one log writer thread, fed by a bounded ring from the matching thread. It publishes a durable position that gateway threads compare with a marker the matching thread puts on the response ring before each take, so the response ring itself is what holds a response back. A response promises one of two durability levels, set at start: `machine`, written through to the drive, or `process`, handed to the OS. `machine` is the default because replication is out of scope, so nothing else covers a machine crash.

Write-through added about 70 µs to a command at 100k to 3M commands a second on a zero-filled file, against about 500 µs for a write followed by a flush, and the extended-only files this design uses cost about 30 µs more per write. A log writer took 2 to 6% off a shard's throughput, against 25 to 46% for writing on the matching thread. Those numbers come from a throwaway Release (MSVC) program outside the repo: real books, 1M commands, medians of 5, on one consumer NVMe drive, in one session, with no Linux run. They are not a recorded baseline. Nobody pulled the plug, so they show what the drive reported, not what it kept.

## Considered options

- **Matching only after the command is durable**, as LMAX and Aeron do. Simpler to state, but every take would wait for the disk, for no gain a client could see.
- **Writing on the matching thread.** Needs no thread or ring, but keeps only 54 to 75% of a shard's throughput, and puts I/O on the thread that must never block.
- **A flush after each write.** Portable and needs no preallocated file, but costs about seven times as long per command here.
- **Holding responses on the matching thread, or passing them through the log writer.** Either needs a buffer or a hop for every response, and the second would slow `process` too. The response ring already exists, and a gateway thread is spinning anyway while commands are in flight.
- **A batching timer.** The writer takes whatever has queued, which gave about 4 records a write at 100k/s and 155 at 3M/s, so a timer would only add delay.

## Consequences

- A book can hold commands a crash will lose, but no response for them has gone out, and recovery simply never replays them.
- A slow disk reaches clients as backpressure: the log ring or the response ring fills, the matching thread waits between takes or in place, and connections park. The log ring's 65,536 records are the most that can be unsafe at once.
- At `process` level a machine crash can lose answered commands, and can leave a hole in the tail that recovery will not cut without being told. That level never promised otherwise.
- A failed log write or sync aborts the process. A retry cannot be trusted, since the OS may have dropped those pages and report success later, and no response has gone out for those commands.
- A command that aborts the process may or may not have reached the log first, so a poison command is possible but not certain after a crash. Recovery must handle it, and an operator gets past it by cutting the log, never by skipping one record, since later commands ran on a book that included it.
- One write is capped at 1 MiB, one at a time, so a hole with valid records after it, within 1 MiB of the end of the last file, is a torn write. Past that it is damage, and start-up stops.
- Log files are extended ahead of time, so a clean stop must cut the zero tail, or every log would read as `TORN_TAIL`. A zero tail then honestly means the engine did not stop cleanly.
