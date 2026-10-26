base: <scratch>/base\order_book_benchmarks.exe
exp: build\Release\order_book_benchmarks.exe
sessions: 3, repetitions: 30

| workload | kind | size | base medians (Mops/s) | exp medians (Mops/s) | ratio exp/base per session | median ratio | base vs 2026-10-25 baseline |
|---|---|---|---|---|---|---:|---:|
| cancellation-heavy | core | standard | 3.49 / 3.88 / 3.97 | 4.12 / 4.00 / 3.89 | 1.181 / 1.031 / 0.980 | 1.031 | 1.286 |
| cancellation-heavy | replay | standard | 0.99 / 1.12 / 1.10 | 1.11 / 1.15 / 1.06 | 1.119 / 1.030 / 0.962 | 1.030 | 1.043 |
| deep-book | core | standard | 2.60 / 2.49 / 2.48 | 2.54 / 2.69 / 1.79 | 0.976 / 1.079 / 0.720 | 0.976 | 1.113 |
| deep-book | replay | standard | 0.96 / 0.93 / 0.95 | 0.94 / 0.98 / 0.87 | 0.981 / 1.054 / 0.917 | 0.981 | 1.042 |
| matching-heavy | core | standard | 4.18 / 4.13 / 4.19 | 4.33 / 4.29 / 4.30 | 1.035 / 1.040 / 1.026 | 1.035 | 1.089 |
| matching-heavy | replay | standard | 1.14 / 1.13 / 1.13 | 1.16 / 1.16 / 1.13 | 1.021 / 1.026 / 1.004 | 1.021 | 1.009 |
| mostly-resting | core | standard | 4.20 / 4.34 / 4.15 | 4.41 / 4.14 / 3.33 | 1.049 / 0.954 / 0.801 | 0.954 | 1.269 |
| mostly-resting | replay | standard | 1.13 / 1.19 / 1.16 | 1.19 / 1.05 / 0.94 | 1.047 / 0.878 / 0.806 | 0.878 | 1.024 |
| small-book | core | standard | 6.47 / 6.58 / 6.55 | 6.57 / 6.52 / 6.67 | 1.014 / 0.990 / 1.019 | 1.014 | 1.039 |
| small-book | replay | standard | 1.36 / 1.36 / 1.36 | 1.36 / 1.35 / 1.36 | 1.001 / 0.992 / 1.000 | 1.000 | 1.029 |
| cancellation-heavy | core | extended | 3.76 / 4.12 / 3.90 | 4.08 / 3.96 / 3.97 | 1.085 / 0.961 / 1.018 | 1.018 | n/a |
| deep-book | core | extended | 2.35 / 2.51 / 2.50 | 2.47 / 2.50 / 2.49 | 1.051 / 0.998 / 0.997 | 0.998 | n/a |
| mostly-resting | core | extended | 2.37 / 2.31 / 2.40 | 2.42 / 2.42 / 2.46 | 1.018 / 1.047 / 1.027 | 1.027 | n/a |
