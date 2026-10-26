base: <scratch>/base\order_book_benchmarks.exe
exp: <scratch>/bin-exp1\order_book_benchmarks.exe
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 1.017 | 1.013 - 1.027 | 94% | 16 |
| small-book | replay | 1.010 | 1.002 - 1.020 | 75% | 16 |
| deep-book | core | 1.034 | 1.013 - 1.040 | 81% | 16 |
| deep-book | replay | 1.000 | 0.989 - 1.008 | 50% | 16 |
| matching-heavy | core | 1.016 | 1.003 - 1.046 | 75% | 16 |
| matching-heavy | replay | 1.006 | 0.995 - 1.016 | 69% | 16 |
| cancellation-heavy | core | 1.019 | 0.982 - 1.056 | 62% | 16 |
| cancellation-heavy | replay | 0.999 | 0.987 - 1.029 | 50% | 16 |
| mostly-resting | core | 1.044 | 1.000 - 1.071 | 69% | 16 |
| mostly-resting | replay | 1.012 | 0.995 - 1.027 | 62% | 16 |
