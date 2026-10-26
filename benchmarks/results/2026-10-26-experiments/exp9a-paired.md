base: <scratch>/bin-exp5final\order_book_benchmarks.exe
exp: <scratch>/bin-exp9a\order_book_benchmarks.exe
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 0.998 | 0.993 - 1.004 | 38% | 16 |
| small-book | replay | 1.504 | 1.499 - 1.509 | 100% | 16 |
| deep-book | core | 0.996 | 0.956 - 1.022 | 38% | 16 |
| deep-book | replay | 1.351 | 1.347 - 1.361 | 100% | 16 |
| matching-heavy | core | 1.006 | 0.996 - 1.020 | 69% | 16 |
| matching-heavy | replay | 1.410 | 1.401 - 1.418 | 100% | 16 |
| cancellation-heavy | core | 0.981 | 0.948 - 1.005 | 25% | 16 |
| cancellation-heavy | replay | 1.404 | 1.388 - 1.430 | 100% | 16 |
| mostly-resting | core | 1.022 | 1.013 - 1.040 | 81% | 16 |
| mostly-resting | replay | 1.416 | 1.403 - 1.435 | 100% | 16 |
