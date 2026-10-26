base: <scratch>/bin-exp5final\order_book_benchmarks.exe
exp: <scratch>/bin-exp7\order_book_benchmarks.exe
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 1.003 | 0.993 - 1.006 | 56% | 16 |
| small-book | replay | 1.004 | 1.002 - 1.009 | 88% | 16 |
| deep-book | core | 1.022 | 0.957 - 1.041 | 62% | 16 |
| deep-book | replay | 0.950 | 0.940 - 0.952 | 0% | 16 |
| matching-heavy | core | 0.994 | 0.985 - 1.021 | 44% | 16 |
| matching-heavy | replay | 0.998 | 0.994 - 1.006 | 38% | 16 |
| cancellation-heavy | core | 1.015 | 0.970 - 1.031 | 62% | 16 |
| cancellation-heavy | replay | 0.989 | 0.981 - 0.999 | 25% | 16 |
| mostly-resting | core | 1.003 | 0.990 - 1.034 | 56% | 16 |
| mostly-resting | replay | 0.953 | 0.945 - 0.971 | 6% | 16 |
