base: <scratch>/bin-exp9a\order_book_benchmarks.exe
exp: <scratch>/bin-exp9b2\order_book_benchmarks.exe
size standard, 12 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | replay | 1.520 | 1.504 - 1.525 | 100% | 12 |
| deep-book | replay | 1.309 | 1.293 - 1.328 | 100% | 12 |
| matching-heavy | replay | 1.417 | 1.407 - 1.429 | 100% | 12 |
| cancellation-heavy | replay | 1.407 | 1.387 - 1.429 | 100% | 12 |
| mostly-resting | replay | 1.410 | 1.398 - 1.431 | 100% | 12 |
