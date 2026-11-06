base: C:\dev\order-book\build\worktrees\v2.0.0\build\Release\order_book_benchmarks.exe
exp: C:\dev\order-book\build\worktrees\6b37df4\build\Release\order_book_benchmarks.exe
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| quote-refresh | core | 0.975 | 0.963 - 0.989 | 6% | 16 |
| small-book | core | 1.005 | 0.996 - 1.012 | 56% | 16 |
