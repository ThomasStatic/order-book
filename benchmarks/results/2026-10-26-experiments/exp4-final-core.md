base: <scratch>/bin-exp2\order_book_benchmarks.exe
exp: <scratch>/bin-exp4b\order_book_benchmarks.exe
size standard, 12 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 1.193 | 1.177 - 1.206 | 100% | 12 |
| deep-book | core | 1.157 | 1.133 - 1.248 | 100% | 12 |
| matching-heavy | core | 1.177 | 1.164 - 1.189 | 100% | 12 |
| cancellation-heavy | core | 1.147 | 1.122 - 1.218 | 100% | 12 |
| mostly-resting | core | 1.129 | 1.083 - 1.165 | 100% | 12 |
