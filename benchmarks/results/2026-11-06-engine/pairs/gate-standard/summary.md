base: C:\dev\order-book\build\worktrees\v2.0.0\build\Release\order_book_benchmarks.exe
exp: C:\dev\order-book\build\Release\order_book_benchmarks.exe
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 0.982 | 0.966 - 0.990 | 12% | 16 |
| small-book | replay | 0.955 | 0.950 - 0.960 | 6% | 16 |
| deep-book | core | 1.088 | 1.050 - 1.134 | 94% | 16 |
| deep-book | replay | 0.997 | 0.994 - 1.017 | 44% | 16 |
| matching-heavy | core | 1.052 | 1.043 - 1.064 | 94% | 16 |
| matching-heavy | replay | 0.993 | 0.981 - 0.997 | 25% | 16 |
| cancellation-heavy | core | 1.069 | 0.933 - 1.195 | 69% | 16 |
| cancellation-heavy | replay | 0.978 | 0.970 - 1.004 | 38% | 16 |
| mostly-resting | core | 1.277 | 1.216 - 1.330 | 94% | 16 |
| mostly-resting | replay | 0.997 | 0.990 - 1.011 | 44% | 16 |
| quote-refresh | core | 0.963 | 0.952 - 0.967 | 0% | 16 |
| quote-refresh | replay | 0.941 | 0.933 - 0.944 | 0% | 16 |
| pending-stops | core | 1.132 | 1.122 - 1.140 | 100% | 16 |
| pending-stops | replay | 1.024 | 1.013 - 1.031 | 81% | 16 |
| self-trade | core | 1.087 | 1.055 - 1.097 | 94% | 16 |
| self-trade | replay | 0.993 | 0.984 - 1.001 | 31% | 16 |
| immediate | core | 1.082 | 1.074 - 1.087 | 100% | 16 |
| immediate | replay | 0.996 | 0.989 - 1.000 | 25% | 16 |
| matching-heavy-request | core | 1.076 | 1.065 - 1.081 | 100% | 16 |
| matching-heavy-request | replay | 0.989 | 0.978 - 0.992 | 19% | 16 |
| matching-heavy-owned | core | 1.067 | 1.061 - 1.070 | 100% | 16 |
| matching-heavy-owned | replay | 0.984 | 0.973 - 0.991 | 6% | 16 |
