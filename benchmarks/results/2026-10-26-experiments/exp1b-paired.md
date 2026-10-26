base: <scratch>/base\order_book_benchmarks.exe
exp: <scratch>/bin-exp1b\order_book_benchmarks.exe
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 1.019 | 1.016 - 1.048 | 88% | 16 |
| small-book | replay | 1.012 | 1.008 - 1.020 | 88% | 16 |
| deep-book | core | 1.007 | 0.989 - 1.012 | 56% | 16 |
| deep-book | replay | 1.009 | 0.993 - 1.016 | 62% | 16 |
| matching-heavy | core | 1.009 | 0.984 - 1.030 | 69% | 16 |
| matching-heavy | replay | 1.003 | 0.996 - 1.016 | 62% | 16 |
| cancellation-heavy | core | 1.031 | 0.989 - 1.062 | 69% | 16 |
| cancellation-heavy | replay | 1.024 | 1.004 - 1.067 | 75% | 16 |
| mostly-resting | core | 1.030 | 0.973 - 1.068 | 62% | 16 |
| mostly-resting | replay | 1.005 | 0.995 - 1.027 | 56% | 16 |
