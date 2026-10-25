# Design Notes

This directory will contain architecture decisions, matching rules, and invariants.

- [`replay-format.md`](replay-format.md): the CSV command format for replaying a session through the book: columns, field syntax, sequence ordering, which layer rejects what, and the fail-fast error policy. Sample files are in `tests/replay/`.
