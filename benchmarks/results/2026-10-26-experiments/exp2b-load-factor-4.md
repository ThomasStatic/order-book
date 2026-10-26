base: <scratch>/bin-exp1b\order_book_benchmarks.exe
exp: <scratch>/bin-lf4.0\order_book_benchmarks.exe
size standard, 12 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| deep-book | core | 0.917 | 0.886 - 0.967 | 8% | 12 |
| cancellation-heavy | core | 0.911 | 0.881 - 0.922 | 0% | 12 |
| small-book | core | 0.925 | 0.918 - 0.929 | 0% | 12 |
