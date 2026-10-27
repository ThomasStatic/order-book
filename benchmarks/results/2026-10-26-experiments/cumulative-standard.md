base: <scratch>/base\order_book_benchmarks.exe
exp: <scratch>/bin-exp8\order_book_benchmarks.exe
size standard, 12 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 1.762 | 1.738 - 1.776 | 100% | 12 |
| small-book | replay | 2.742 | 2.727 - 2.769 | 100% | 12 |
| deep-book | core | 1.852 | 1.824 - 1.873 | 100% | 12 |
| deep-book | replay | 2.456 | 2.401 - 2.478 | 100% | 12 |
| matching-heavy | core | 1.437 | 1.428 - 1.446 | 100% | 12 |
| matching-heavy | replay | 2.339 | 2.311 - 2.346 | 100% | 12 |
| cancellation-heavy | core | 2.232 | 2.101 - 2.343 | 100% | 12 |
| cancellation-heavy | replay | 2.604 | 2.592 - 2.640 | 100% | 12 |
| mostly-resting | core | 1.827 | 1.745 - 2.013 | 100% | 12 |
| mostly-resting | replay | 2.367 | 2.329 - 2.401 | 100% | 12 |
