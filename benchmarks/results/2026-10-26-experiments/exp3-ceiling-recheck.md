base: <scratch>/bin-exp2\order_book_benchmarks.exe
exp: <scratch>/bin-exp3-ceiling\order_book_benchmarks.exe
size standard, 24 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| deep-book | core | 1.028 | 1.013 - 1.061 | 75% | 24 |
| cancellation-heavy | core | 1.029 | 1.002 - 1.107 | 71% | 24 |
