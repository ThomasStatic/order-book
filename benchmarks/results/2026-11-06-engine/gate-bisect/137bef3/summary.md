base: C:\dev\order-book\build\worktrees\v2.0.0\build\Release\order_book_benchmarks.exe
exp: C:\dev\order-book\build\worktrees\137bef3\build\Release\order_book_benchmarks.exe
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| quote-refresh | core | 0.983 | 0.967 - 0.988 | 19% | 16 |
| small-book | core | 1.007 | 0.993 - 1.020 | 62% | 16 |
