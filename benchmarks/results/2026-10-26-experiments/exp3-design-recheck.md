base: <scratch>/bin-exp2\order_book_benchmarks.exe
exp: <scratch>/bin-exp3\order_book_benchmarks.exe
size standard, 24 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| deep-book | core | 1.018 | 0.997 - 1.025 | 62% | 24 |
| cancellation-heavy | core | 1.004 | 0.992 - 1.029 | 54% | 24 |
