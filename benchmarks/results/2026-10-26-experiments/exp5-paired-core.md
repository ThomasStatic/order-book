base: <scratch>/bin-exp4b\order_book_benchmarks.exe
exp: <scratch>/bin-exp5\order_book_benchmarks.exe
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 1.022 | 1.009 - 1.039 | 100% | 16 |
| deep-book | core | 1.026 | 0.987 - 1.048 | 62% | 16 |
| matching-heavy | core | 0.995 | 0.989 - 1.007 | 38% | 16 |
| cancellation-heavy | core | 1.057 | 1.007 - 1.135 | 75% | 16 |
| mostly-resting | core | 0.982 | 0.927 - 1.012 | 31% | 16 |
