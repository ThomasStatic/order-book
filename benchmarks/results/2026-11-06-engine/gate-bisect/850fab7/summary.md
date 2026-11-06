base: C:\dev\order-book\build\worktrees\v2.0.0\build\Release\order_book_benchmarks.exe
exp: C:\dev\order-book\build\worktrees\850fab7\build\Release\order_book_benchmarks.exe
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| quote-refresh | core | 1.024 | 1.008 - 1.027 | 75% | 16 |
| small-book | core | 0.991 | 0.986 - 0.994 | 12% | 16 |
