base: <scratch>/base\order_book_benchmarks.exe
exp: <scratch>/bin-exp8\order_book_benchmarks.exe
size extended, 6 ABBA pairs per workload, 3 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 1.784 | 1.723 - 1.814 | 100% | 6 |
| small-book | replay | 2.792 | 2.770 - 2.821 | 100% | 6 |
| deep-book | core | 1.948 | 1.864 - 1.972 | 100% | 6 |
| deep-book | replay | 2.494 | 2.463 - 2.516 | 100% | 6 |
| matching-heavy | core | 1.430 | 1.425 - 1.443 | 100% | 6 |
| matching-heavy | replay | 2.376 | 2.368 - 2.385 | 100% | 6 |
| cancellation-heavy | core | 2.409 | 2.263 - 2.546 | 100% | 6 |
| cancellation-heavy | replay | 2.872 | 2.843 - 2.976 | 100% | 6 |
| mostly-resting | core | 1.963 | 1.857 - 1.995 | 100% | 6 |
| mostly-resting | replay | 2.598 | 2.581 - 2.698 | 100% | 6 |
