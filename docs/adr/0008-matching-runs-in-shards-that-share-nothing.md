# Matching runs in shards that share nothing

The engine runs a fixed number of shards, set explicitly at start. Each shard is one matching thread that alone touches the whole books placed on it, and shards share no state, so throughput scales with instruments while every book stays single-threaded and needs no locks. Determinism holds per instrument: an instrument's command sequence determines its book and events, and nothing orders one instrument's commands against another's. So each shard has its own command log and checkpoints alone, at its own log position, with no engine-wide cut.

## Considered options

- **One matching thread for all instruments.** Gives a total order and one log, but caps the engine at what one thread can do, 4 to 11 million commands a second on the v2.0.0 baseline.
- **One thread per instrument.** The same code as a pool with as many shards as instruments, but hundreds of instruments would mean hundreds of threads on a dozen cores.
- **One log for the engine, with per-instrument sequences.** Keeps a whole session in one file, but every shard funnels through one writer, the only point they would all contend on.
- **One log per instrument.** Moves an instrument between shards freely, but needs a file and a sync for every instrument, and one sync cannot cover several.
- **An engine-wide checkpoint cut**, by a marker through every shard or by stopping them all. Gives one instant for the whole engine, which nothing can use since nothing is ordered across instruments, and stopping them all makes every shard pause for as long as the slowest.

## Consequences

- A client cannot rely on order across instruments. Responses for two instruments on different shards may arrive in either order, and after a machine crash a later command on one shard may survive an earlier one on another.
- Sharding scales instruments, not a book: one hot instrument is still capped at one thread.
- Placement changes only across a clean stop, because an instrument's recent history is in its old shard's log. Moving a hot instrument means a restart.
- The shard count is never taken from the machine, and the placement hash is a function we specify rather than `std::hash`, so a log written on one machine reopens on another.
- No single file holds a whole session. Converting several logs to one CSV keeps each instrument's order, and that is what lossless means here.
- `std::abort` is process-wide, so a broken book on one shard stops every shard.
- ADR 0009 narrows the checkpoint further: each book on a shard checkpoints alone, at its own log position, so there is no shard-wide cut either.
