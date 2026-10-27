base: <scratch>/bin-exp9b2\order_book_benchmarks.exe
exp: <scratch>/bin-exp6\order_book_benchmarks.exe
size extended, 8 ABBA pairs per workload, 3 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| mostly-resting | core | 1.082 | 1.065 - 1.116 | 100% | 8 |
| deep-book | core | 1.226 | 1.183 - 1.257 | 100% | 8 |
