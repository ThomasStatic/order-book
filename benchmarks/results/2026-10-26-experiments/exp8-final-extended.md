base: <scratch>/bin-exp6b\order_book_benchmarks.exe
exp: <scratch>/bin-exp8\order_book_benchmarks.exe
size extended, 8 ABBA pairs per workload, 3 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| deep-book | core | 1.386 | 1.319 - 1.460 | 100% | 8 |
| cancellation-heavy | core | 1.605 | 1.552 - 1.628 | 100% | 8 |
| mostly-resting | core | 1.317 | 1.255 - 1.416 | 100% | 8 |
