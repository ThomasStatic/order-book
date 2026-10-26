base: <scratch>/bin-exp4b\order_book_benchmarks.exe
exp: <scratch>/bin-exp5\order_book_benchmarks.exe
size extended, 8 ABBA pairs per workload, 3 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| mostly-resting | core | 0.995 | 0.980 - 1.015 | 38% | 8 |
| cancellation-heavy | core | 1.067 | 1.043 - 1.098 | 100% | 8 |
