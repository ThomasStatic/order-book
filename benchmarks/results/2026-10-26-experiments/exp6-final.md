base: <scratch>/bin-exp9b2\order_book_benchmarks.exe
exp: <scratch>/bin-exp6b\order_book_benchmarks.exe
size standard, 12 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 1.162 | 1.144 - 1.175 | 100% | 12 |
| small-book | replay | 1.074 | 1.069 - 1.083 | 100% | 12 |
| deep-book | core | 1.291 | 1.220 - 1.318 | 100% | 12 |
| deep-book | replay | 1.104 | 1.092 - 1.122 | 100% | 12 |
| matching-heavy | core | 1.112 | 1.094 - 1.134 | 100% | 12 |
| matching-heavy | replay | 1.069 | 1.060 - 1.078 | 100% | 12 |
| cancellation-heavy | core | 1.211 | 1.109 - 1.222 | 100% | 12 |
| cancellation-heavy | replay | 1.105 | 1.088 - 1.152 | 100% | 12 |
| mostly-resting | core | 1.128 | 1.098 - 1.162 | 92% | 12 |
| mostly-resting | replay | 1.077 | 1.053 - 1.106 | 100% | 12 |
