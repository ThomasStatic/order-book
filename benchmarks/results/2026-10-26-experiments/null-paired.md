base: <scratch>/base\order_book_benchmarks.exe
exp: <scratch>/base2\order_book_benchmarks.exe
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 0.994 | 0.958 - 1.015 | 44% | 16 |
| small-book | replay | 0.983 | 0.959 - 1.024 | 44% | 16 |
| deep-book | core | 0.977 | 0.969 - 1.002 | 25% | 16 |
| deep-book | replay | 0.995 | 0.986 - 1.005 | 38% | 16 |
| matching-heavy | core | 0.990 | 0.976 - 0.997 | 12% | 16 |
| matching-heavy | replay | 0.996 | 0.986 - 1.001 | 38% | 16 |
| cancellation-heavy | core | 0.984 | 0.963 - 1.011 | 44% | 16 |
| cancellation-heavy | replay | 0.979 | 0.967 - 1.005 | 31% | 16 |
| mostly-resting | core | 0.941 | 0.891 - 1.023 | 31% | 16 |
| mostly-resting | replay | 0.984 | 0.963 - 1.013 | 38% | 16 |
