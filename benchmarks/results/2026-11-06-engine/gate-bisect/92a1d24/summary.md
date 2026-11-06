base: C:\dev\order-book\build\worktrees\v2.0.0\build\Release\order_book_benchmarks.exe
exp: C:\dev\order-book\build\worktrees\92a1d24\build\Release\order_book_benchmarks.exe
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| quote-refresh | core | 0.986 | 0.962 - 1.004 | 31% | 16 |
| small-book | core | 0.985 | 0.972 - 0.993 | 12% | 16 |
