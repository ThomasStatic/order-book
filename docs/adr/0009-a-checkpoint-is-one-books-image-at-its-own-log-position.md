# A checkpoint is one book's image at that book's own log position

Each book checkpoints alone. A checkpoint is one file holding one instrument's book image, its symbol, and the log position that image stands at, and books on one shard may stand at different positions. Nothing orders one instrument's commands against another's, inside a shard as much as across shards, so a shard-wide cut would buy nothing that recovery can use, and it would make the matching thread pause for all its books at once. The image itself is bytes owned by the matching library: the matching thread writes the same bytes the file will hold, and the check that proves a checkpoint restores rebuilds a book from exactly those bytes, so every checkpoint exercises the decoder recovery depends on.

## Considered options

- **One checkpoint per shard, at one log position.** One file per shard and the simplest recovery, but the pause is the sum of the shard's books: 69 ms for eight books of 100k orders, against eight pauses of 8.6 ms, and the matching thread's buffer must hold every book at once rather than the largest. The cost per order is chasing list nodes, so a smaller record does not shrink it.
- **A visitor and a builder, with the runtime owning every byte.** Keeps the library free of any format, but grows its interface and splits restore's validation across two layers, for no saving in the copy.
- **A `BookImage` struct of vectors.** Plain to read, but it needs pre-sized capacity on the matching thread and a second representation to encode, and the check would then prove the struct rather than the bytes written.

## Consequences

- Recovery replays a shard's log from its oldest book's position and skips each command older than its own book's position: one compare per replayed command, and nothing per command in normal running.
- A log can be trimmed only up to the oldest book's position, so a quiet book must still be checkpointed now and then or it holds the whole log.
- An engine has one checkpoint file per instrument, not per shard, and a clean stop writes them all at one position.
- A checkpoint records its shard and shard count, because a position means something only in that shard's log. It is self-contained, so after a clean stop with a new placement any shard can restore it.
- The library gains `imageSize`, `writeImage`, and `restore` over spans of bytes. It still knows nothing of files, symbols, or positions, which the runtime's wrapper adds.
- The same state gives the same image bytes on every platform, so an image holds no clock or random value, and restore refuses one that is not in canonical order. That makes `writeImage(restore(x)) == x` the round-trip test, covering fields `BookState` does not hold.
