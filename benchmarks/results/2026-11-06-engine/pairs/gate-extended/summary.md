base: C:\dev\order-book\build\worktrees\v2.0.0\build\Release\order_book_benchmarks.exe
exp: C:\dev\order-book\build\Release\order_book_benchmarks.exe
size extended, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 0.981 | 0.975 - 0.994 | 19% | 16 |
| small-book | replay | 0.951 | 0.941 - 0.954 | 0% | 16 |
| deep-book | core | 1.045 | 1.033 - 1.121 | 81% | 16 |
| deep-book | replay | 0.997 | 0.982 - 1.004 | 31% | 16 |
| matching-heavy | core | 1.047 | 1.041 - 1.050 | 100% | 16 |
| matching-heavy | replay | 0.999 | 0.995 - 1.006 | 38% | 16 |
| cancellation-heavy | core | 1.032 | 0.987 - 1.043 | 62% | 16 |
| cancellation-heavy | replay | 0.979 | 0.974 - 0.993 | 25% | 16 |
| mostly-resting | core | 1.093 | 1.089 - 1.100 | 100% | 16 |
| mostly-resting | replay | 1.018 | 1.015 - 1.024 | 100% | 16 |
| quote-refresh | core | 0.964 | 0.961 - 0.973 | 0% | 16 |
| quote-refresh | replay | 0.933 | 0.930 - 0.942 | 0% | 16 |
| pending-stops | core | 1.144 | 1.134 - 1.160 | 100% | 16 |
| pending-stops | replay | 1.031 | 1.023 - 1.036 | 94% | 16 |
| self-trade | core | 1.090 | 1.085 - 1.098 | 100% | 16 |
| self-trade | replay | 0.986 | 0.981 - 0.989 | 6% | 16 |
| immediate | core | 1.096 | 1.090 - 1.101 | 100% | 16 |
| immediate | replay | 1.004 | 1.000 - 1.008 | 69% | 16 |
| matching-heavy-request | core | 1.068 | 1.062 - 1.072 | 100% | 16 |
| matching-heavy-request | replay | 0.988 | 0.981 - 0.993 | 12% | 16 |
| matching-heavy-owned | core | 1.066 | 1.054 - 1.070 | 100% | 16 |
| matching-heavy-owned | replay | 0.987 | 0.981 - 0.992 | 0% | 16 |
