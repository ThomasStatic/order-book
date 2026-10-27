base: <scratch>/bin-exp6b\order_book_benchmarks.exe
exp: <scratch>/bin-exp8\order_book_benchmarks.exe
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 1.223 | 1.213 - 1.238 | 100% | 16 |
| small-book | replay | 1.079 | 1.070 - 1.086 | 100% | 16 |
| deep-book | core | 1.260 | 1.200 - 1.372 | 100% | 16 |
| deep-book | replay | 1.169 | 1.155 - 1.180 | 100% | 16 |
| matching-heavy | core | 1.049 | 1.039 - 1.057 | 100% | 16 |
| matching-heavy | replay | 1.030 | 1.021 - 1.035 | 100% | 16 |
| cancellation-heavy | core | 1.621 | 1.498 - 1.675 | 100% | 16 |
| cancellation-heavy | replay | 1.131 | 1.119 - 1.149 | 100% | 16 |
| mostly-resting | core | 1.262 | 1.199 - 1.363 | 100% | 16 |
| mostly-resting | replay | 1.091 | 1.077 - 1.103 | 94% | 16 |
