# The command log is fixed-width chained records of well-formed commands

A command log is a header followed by 40-byte records, each holding one well-formed command and its symbol, with a position that is simply the count of records before it. Each record's checksum is seeded with the previous record's, so a dropped, duplicated, reordered, or stale record fails its check, though ADR 0008 left records no sequence field. A log holds exactly what a v3 replay CSV can hold: a malformed command is refused before it enters the engine, on the same path as an unknown instrument, and the reader applies the CSV's own checks. So a log and a CSV are interchangeable inputs, and a decoded `ADD` is always a well-formed request, as ADR 0005 requires of a parsed row.

## Considered options

- **Records sized by command.** 20% smaller on a 60/25/15 add, cancel, and replace mix (32.1 bytes against 40), but a position would need a scan or an index to become a file offset, and a reader could not step past a bad record, since its length cannot be trusted. Stepping past one is how recovery tells a torn tail from damage in the middle of a file.
- **A checksum per block.** Saves 3 to 6 ns a command, but risks up to 102 records on a torn tail, and every flush of a partial block must pad or rewrite it.
- **CRC-32C.** Guarantees detection of short bursts, but the failures here are torn writes and zero-filled tails, where it guarantees nothing either. Portable it took 23 ns a record against XXH64's 13; hardware took 6 ns but needs a second implementation and CPU detection. XXH64 is the checkpoint's checksum already (ADR 0009), so the engine keeps one.
- **All 64 bits of XXH64.** Makes every record 48 bytes, 20% more disk, to improve a 1-in-4-billion miss that the field checks already back up.
- **Logging malformed commands too.** Replay would reproduce their rejections, but the log would hold records no CSV can express, so conversion would be lossy or ADR 0005 would have to be undone.
- **Skipping unknown trailing bytes, as the checkpoint file does.** Every command field changes replay, so an old reader that skipped one would replay a different command without noticing. A field could never be safely ignored, and each addition would have to refuse old readers even on files that never use it.

## Consequences

- A reader may skip bytes past what it knows only if they are all zero, and a new field must use zero to mean today's behavior. This is ADR 0009's extension rule, stated sizes and a minimum reader version, with that one stricter check. So a log that does not use a new feature stays readable, and one that does stops at the first record that does, never misreplaying it.
- A replay of a log cannot reproduce the rejection of a malformed command, as a replay of a CSV already cannot. Such a command changes no book and consumes no number, so nothing else differs.
- The library gains public `malformedReason` checks for requests and replaces, so the entry can refuse what the book would, without a second copy of the rules.
- A log cut exactly at a record boundary looks like a clean end, so recovery must separately check that no checkpoint's position lies past it.
- A record carries no timestamp, session, or client ID. A later need for one uses the extension rule, and the CSV gains a matching column so conversion stays lossless.
