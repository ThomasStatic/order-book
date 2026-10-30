base: C:\dev\order-book\build\Release\order_book_benchmarks.exe
exp: C:\dev\order-book\build\Release\order_book_benchmarks.exe
exp workload: matching-heavy-owned
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| matching-heavy-request | core | 0.980 | 0.904 - 1.067 | 44% | 16 |
