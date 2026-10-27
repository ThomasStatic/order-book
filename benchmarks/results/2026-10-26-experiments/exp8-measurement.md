base: <scratch>/bin-exp6b\order_book_benchmarks.exe
exp: <scratch>/bin-exp8m\order_book_benchmarks.exe
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 1.251 | 1.244 - 1.258 | 100% | 16 |
| deep-book | core | 1.261 | 1.191 - 1.360 | 100% | 16 |
| matching-heavy | core | 1.075 | 1.062 - 1.090 | 100% | 16 |
| cancellation-heavy | core | 1.635 | 1.572 - 1.676 | 100% | 16 |
| mostly-resting | core | 1.349 | 1.301 - 1.412 | 100% | 16 |
