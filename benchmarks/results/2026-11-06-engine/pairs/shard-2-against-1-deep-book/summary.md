base: C:\dev\order-book\build\Release\order_book_benchmarks.exe
exp: C:\dev\order-book\build\Release\order_book_benchmarks.exe
exp args: --shards 2 --instruments 16 --pin-cores 2,4 --layout one-core-each
size extended, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| deep-book | shard | 1.865 | 1.851 - 1.885 | 100% | 16 |
