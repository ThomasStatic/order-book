base: <scratch>/bin-exp1b\order_book_benchmarks.exe
exp: <scratch>/bin-exp2\order_book_benchmarks.exe
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 0.977 | 0.962 - 0.996 | 12% | 16 |
| small-book | replay | 0.994 | 0.987 - 0.999 | 19% | 16 |
| deep-book | core | 0.974 | 0.933 - 0.991 | 19% | 16 |
| deep-book | replay | 0.995 | 0.990 - 1.007 | 44% | 16 |
| matching-heavy | core | 1.015 | 0.998 - 1.029 | 62% | 16 |
| matching-heavy | replay | 1.001 | 0.995 - 1.015 | 50% | 16 |
| cancellation-heavy | core | 0.991 | 0.927 - 1.028 | 50% | 16 |
| cancellation-heavy | replay | 0.990 | 0.965 - 1.007 | 44% | 16 |
| mostly-resting | core | 1.028 | 0.994 - 1.085 | 69% | 16 |
| mostly-resting | replay | 0.992 | 0.986 - 0.999 | 25% | 16 |
