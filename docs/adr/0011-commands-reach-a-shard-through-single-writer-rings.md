# Commands reach a shard through single-writer rings

Each gateway thread has its own bounded single-producer single-consumer ring to each shard, and each shard has its own back to each gateway thread, so every ring has one writer and one reader and M gateway threads on N shards make M×N pairs. It is lock-free from the start, which overrides the system-capabilities map's standing note that a lock-free structure is never the first design: that note exists so nobody optimizes before measuring, and here the measurement came first. Behind a mutex ring a matching thread kept 33 to 59% of its direct throughput, behind this one 71 to 94%, at about 160 to 270 ns a command against 40 to 60 ns, and a command arriving 20 µs after the last took 3,170 ns round trip against 190 ns.

Those numbers come from a throwaway Release (MSVC) program outside the repo: real books on four behavior workloads, 1M commands, medians of 5, pinned threads, one session on one machine, and no Linux. They are not a recorded baseline. A gateway thread with a client that sends one command and waits costs about 7.6 µs a command, or 130k commands a second, against a shard's 3 to 10 million, so several gateway threads are expected, and the shape has to hold up under them.

## Considered options

- **A mutex ring first, then a lock-free one as an experiment.** The ticket's starting assumption. The ring is only `tryPush` and `drain`, so nothing would have been lost by waiting, but the experiment's result was already in hand, and the mutex ring would have been built to be thrown away.
- **One multi-producer ring per shard.** Fewer rings, but an atomic claim on every push: 40.6, 20.8, and 13.7 M/s at 1, 2, and 4 producers with a trivial consumer, while separate rings kept scaling. All of those exceed what a shard takes, so saturation did not decide it; having no producer contention at any gateway thread count did.
- **One return queue per connection.** Would need a ring per connection and put connection lifetime on the matching thread.
- **Moving the result across, or encoding it on the matching thread.** A `SubmissionResult` is 336 bytes and not trivially copyable, and past its inline capacity its heap blocks would be freed by another thread. Encoding would put the wire format on the hot path and need a variable-length ring. One fixed record per event needs neither, and a response larger than the ring streams through it.

## Consequences

- The take order is whatever the matching thread's pass across its rings produces, and it differs between runs. The log records it, so replaying a run's own log is how a threaded run with several connections on one instrument is proved correct: it cannot be compared with one fixed order.
- A journaler can sit only after the matching thread's merge, not between a gateway thread and the matching thread, since until the merge no single order exists.
- A gateway thread must never block on anything, because a matching thread waits in place on a full response queue. A shard can be held up by an overloaded gateway thread, never by a client.
- A connection parked on one shard stops its commands for every shard, since its TCP stream is ordered.
- Memory-ordering correctness is ours to carry, with ThreadSanitizer in CI as the check.
