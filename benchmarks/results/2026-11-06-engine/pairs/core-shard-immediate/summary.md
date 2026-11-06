base: C:\dev\order-book\build\Release\order_book_benchmarks.exe
exp: C:\dev\order-book\build\Release\order_book_benchmarks.exe
exp kind: shard
exp args: --priority high --pin-cores 2 --layout one-core-each
size extended, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| immediate | core | 0.919 | 0.916 - 0.923 | 0% | 16 |
