base: <scratch>/bin-exp4b\order_book_benchmarks.exe
exp: <scratch>/bin-exp5\order_book_benchmarks.exe
size standard, 24 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| cancellation-heavy | core | 1.049 | 1.010 - 1.090 | 79% | 24 |
| deep-book | core | 0.996 | 0.965 - 1.053 | 50% | 24 |
