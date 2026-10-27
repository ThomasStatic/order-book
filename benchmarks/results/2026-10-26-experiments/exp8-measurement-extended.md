base: <scratch>/bin-exp6b\order_book_benchmarks.exe
exp: <scratch>/bin-exp8m\order_book_benchmarks.exe
size extended, 8 ABBA pairs per workload, 3 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| deep-book | core | 1.393 | 1.364 - 1.417 | 100% | 8 |
| cancellation-heavy | core | 1.589 | 1.562 - 1.648 | 100% | 8 |
| mostly-resting | core | 1.270 | 1.266 - 1.308 | 100% | 8 |
