base: C:\dev\order-book\build\Release\order_book_benchmarks.exe
exp: C:\dev\order-book\build\Release\order_book_benchmarks.exe
exp workload: matching-heavy-request
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| matching-heavy | core | 0.974 | 0.969 - 0.991 | 19% | 16 |
