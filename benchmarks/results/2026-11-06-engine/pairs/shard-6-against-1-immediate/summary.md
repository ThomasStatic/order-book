base: C:\dev\order-book\build\Release\order_book_benchmarks.exe
exp: C:\dev\order-book\build\Release\order_book_benchmarks.exe
exp args: --shards 6 --instruments 48 --pin-cores 2,4,6,8,10,0 --layout one-core-each
size extended, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| immediate | shard | 4.081 | 4.026 - 4.105 | 100% | 16 |
