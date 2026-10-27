base: <scratch>/bin-exp9a\order_book_benchmarks.exe
exp: <scratch>/bin-exp9b\order_book_benchmarks.exe
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 1.009 | 1.007 - 1.012 | 81% | 16 |
| small-book | replay | 1.512 | 1.508 - 1.521 | 100% | 16 |
| deep-book | core | 0.998 | 0.941 - 1.051 | 50% | 16 |
| deep-book | replay | 1.324 | 1.316 - 1.344 | 100% | 16 |
| matching-heavy | core | 1.004 | 0.997 - 1.015 | 62% | 16 |
| matching-heavy | replay | 1.429 | 1.422 - 1.435 | 100% | 16 |
| cancellation-heavy | core | 1.012 | 0.983 - 1.031 | 69% | 16 |
| cancellation-heavy | replay | 1.398 | 1.365 - 1.416 | 100% | 16 |
| mostly-resting | core | 0.990 | 0.977 - 1.002 | 31% | 16 |
| mostly-resting | replay | 1.417 | 1.396 - 1.425 | 100% | 16 |
