base: <scratch>/bin-exp2\order_book_benchmarks.exe
exp: <scratch>/bin-exp3-ceiling\order_book_benchmarks.exe
size standard, 12 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 1.018 | 1.013 - 1.034 | 92% | 12 |
| deep-book | core | 1.066 | 1.004 - 1.110 | 75% | 12 |
| matching-heavy | core | 1.037 | 1.015 - 1.049 | 100% | 12 |
| cancellation-heavy | core | 1.049 | 1.034 - 1.067 | 100% | 12 |
| mostly-resting | core | 1.020 | 0.993 - 1.062 | 67% | 12 |
