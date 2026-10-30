base: C:\dev\order-book\build\Release\order_book_benchmarks.exe
exp: C:\dev\order-book\build\Release\order_book_benchmarks.exe
exp workload: matching-heavy-owned
size standard, 32 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| matching-heavy-request | core | 0.999 | 0.993 - 1.011 | 47% | 32 |
