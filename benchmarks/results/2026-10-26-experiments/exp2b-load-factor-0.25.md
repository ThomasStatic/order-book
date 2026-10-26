base: <scratch>/bin-exp1b\order_book_benchmarks.exe
exp: <scratch>/bin-lf0.25\order_book_benchmarks.exe
size standard, 12 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| deep-book | core | 0.977 | 0.950 - 1.012 | 33% | 12 |
| cancellation-heavy | core | 0.914 | 0.891 - 0.935 | 0% | 12 |
| small-book | core | 1.012 | 1.008 - 1.016 | 83% | 12 |
