base: <scratch>/bin-exp2\order_book_benchmarks.exe
exp: <scratch>/bin-exp3\order_book_benchmarks.exe
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 1.012 | 1.002 - 1.017 | 75% | 16 |
| small-book | replay | 1.016 | 1.004 - 1.021 | 88% | 16 |
| deep-book | core | 0.972 | 0.950 - 1.006 | 38% | 16 |
| deep-book | replay | 1.007 | 0.988 - 1.015 | 56% | 16 |
| matching-heavy | core | 1.005 | 0.992 - 1.016 | 56% | 16 |
| matching-heavy | replay | 1.009 | 0.999 - 1.014 | 69% | 16 |
| cancellation-heavy | core | 0.997 | 0.951 - 1.013 | 44% | 16 |
| cancellation-heavy | replay | 1.019 | 1.001 - 1.036 | 75% | 16 |
| mostly-resting | core | 1.034 | 1.018 - 1.074 | 81% | 16 |
| mostly-resting | replay | 1.010 | 1.005 - 1.028 | 88% | 16 |
