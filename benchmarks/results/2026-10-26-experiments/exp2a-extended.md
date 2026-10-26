base: <scratch>/bin-exp1b\order_book_benchmarks.exe
exp: <scratch>/bin-exp2\order_book_benchmarks.exe
size extended, 8 ABBA pairs per workload, 3 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| mostly-resting | core | 1.195 | 1.160 - 1.222 | 100% | 8 |
| deep-book | core | 1.018 | 1.009 - 1.030 | 88% | 8 |
