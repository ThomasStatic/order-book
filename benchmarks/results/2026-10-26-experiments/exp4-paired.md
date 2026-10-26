base: <scratch>/bin-exp2\order_book_benchmarks.exe
exp: <scratch>/bin-exp4\order_book_benchmarks.exe
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 1.183 | 1.177 - 1.206 | 100% | 16 |
| small-book | replay | 1.040 | 1.033 - 1.047 | 100% | 16 |
| deep-book | core | 1.161 | 1.140 - 1.183 | 94% | 16 |
| deep-book | replay | 1.050 | 1.043 - 1.057 | 100% | 16 |
| matching-heavy | core | 1.191 | 1.185 - 1.204 | 100% | 16 |
| matching-heavy | replay | 1.046 | 1.042 - 1.056 | 100% | 16 |
| cancellation-heavy | core | 1.190 | 1.158 - 1.230 | 100% | 16 |
| cancellation-heavy | replay | 1.035 | 1.023 - 1.040 | 100% | 16 |
| mostly-resting | core | 1.086 | 1.079 - 1.122 | 94% | 16 |
| mostly-resting | replay | 1.032 | 1.023 - 1.036 | 94% | 16 |
