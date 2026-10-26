base: <scratch>/bin-exp4b\order_book_benchmarks.exe
exp: <scratch>/bin-exp5\order_book_benchmarks.exe
size standard, 24 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| mostly-resting | core | 0.971 | 0.945 - 1.014 | 33% | 24 |
