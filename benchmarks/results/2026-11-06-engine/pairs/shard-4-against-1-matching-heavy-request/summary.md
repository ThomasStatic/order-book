base: C:\dev\order-book\build\Release\order_book_benchmarks.exe
exp: C:\dev\order-book\build\Release\order_book_benchmarks.exe
exp args: --shards 4 --instruments 32 --pin-cores 2,4,6,8 --layout one-core-each
size extended, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| matching-heavy-request | shard | 3.092 | 3.079 - 3.130 | 100% | 16 |
