# The symbol is an instrument's only identity in any format

Every command, record, and file names an instrument by its symbol: eight bytes, 1 to 8 characters of `A-Z`, `0-9`, `.`, or `-`, right-padded with spaces. No format carries an engine-assigned integer. Inside a process a dense index may route a command straight to its book, but it is never written to a file or sent over a socket. So a record means the same thing whatever the instrument list looks like when it is replayed: nothing depends on an assignment staying stable, and no log, checkpoint, or client has a mapping to carry and reconcile.

## Considered options

- **An engine-assigned integer** (CME's SecurityID, ITCH's stock locate). Four bytes smaller per record, but replay is right only while the assignment holds, and a reordered list would silently send logged commands to the wrong book.
- **Both, with a mapping.** Compact, but the log and checkpoint must carry the mapping, recovery must reconcile it, and clients need a directory.
- **A 16-byte or variable-length symbol.** Fits every venue's names, but doubles what each record pays, or gives up fixed record layouts. Eight bytes compare as one 64-bit word.

## Consequences

- Each record carries 8 bytes rather than 4, and an instrument cannot be renamed.
- The lookup happens once, when a command enters the engine, so the matching path keeps array-index routing.
- A command for a symbol not on the instrument list is refused at entry and never logged. If it were logged, adding that symbol to the list later would make replay accept a command the original run refused.
- The instrument list is fixed at start, but adding instruments at runtime stays open: records already describe themselves, so it would take one appended log command and no format break.
