base: <scratch>/bin-exp9b2\order_book_benchmarks.exe
exp: <scratch>/bin-exp6\order_book_benchmarks.exe
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 1.185 | 1.164 - 1.197 | 100% | 16 |
| small-book | replay | 1.083 | 1.069 - 1.095 | 100% | 16 |
| deep-book | core | 1.200 | 1.182 - 1.246 | 100% | 16 |
| deep-book | replay | 1.102 | 1.083 - 1.129 | 100% | 16 |
| matching-heavy | core | 1.132 | 1.128 - 1.145 | 100% | 16 |
| matching-heavy | replay | 1.076 | 1.069 - 1.085 | 100% | 16 |
| cancellation-heavy | core | 1.242 | 1.215 - 1.311 | 100% | 16 |
| cancellation-heavy | replay | 1.129 | 1.110 - 1.147 | 100% | 16 |
| mostly-resting | core | 1.166 | 1.149 - 1.227 | 100% | 16 |
| mostly-resting | replay | 1.087 | 1.078 - 1.106 | 94% | 16 |
