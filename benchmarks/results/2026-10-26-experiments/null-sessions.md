base: <scratch>/base\order_book_benchmarks.exe
exp: <scratch>/base2\order_book_benchmarks.exe
sessions: 3, repetitions: 30

| workload | kind | size | base medians (Mops/s) | exp medians (Mops/s) | ratio exp/base per session | median ratio | base vs 2026-10-25 baseline |
|---|---|---|---|---|---|---:|---:|
| cancellation-heavy | core | standard | 2.57 / 3.12 / 4.13 | 4.02 / 3.18 / 3.96 | 1.566 / 1.022 / 0.958 | 1.022 | 1.033 |
| cancellation-heavy | replay | standard | 1.01 / 1.00 / 1.13 | 1.09 / 1.06 / 1.12 | 1.082 / 1.059 / 0.993 | 1.059 | 0.954 |
| deep-book | core | standard | 2.72 / 2.27 / 1.99 | 2.36 / 2.50 / 2.66 | 0.870 / 1.099 / 1.337 | 1.099 | 1.015 |
| deep-book | replay | standard | 0.91 / 0.90 / 0.81 | 0.87 / 0.95 / 0.97 | 0.953 / 1.062 / 1.201 | 1.062 | 0.989 |
| matching-heavy | core | standard | 3.55 / 3.79 / 3.86 | 4.03 / 4.14 / 4.21 | 1.136 / 1.092 / 1.089 | 1.092 | 0.987 |
| matching-heavy | replay | standard | 1.00 / 1.03 / 1.09 | 1.07 / 1.12 / 1.14 | 1.073 / 1.081 / 1.040 | 1.073 | 0.923 |
| mostly-resting | core | standard | 3.38 / 3.47 / 4.15 | 3.95 / 3.83 / 4.28 | 1.168 / 1.103 / 1.031 | 1.103 | 1.049 |
| mostly-resting | replay | standard | 1.00 / 1.11 / 1.17 | 1.13 / 1.13 / 1.18 | 1.127 / 1.017 / 1.007 | 1.017 | 0.977 |
| small-book | core | standard | 6.62 / 6.38 / 5.82 | 6.44 / 6.44 / 6.56 | 0.974 / 1.010 / 1.127 | 1.010 | 1.012 |
| small-book | replay | standard | 1.36 / 1.33 / 1.33 | 1.34 / 1.34 / 1.35 | 0.984 / 1.006 / 1.009 | 1.006 | 1.008 |
| cancellation-heavy | core | extended | 3.44 / 2.38 / 3.51 | 3.45 / 4.19 / 2.16 | 1.004 / 1.758 / 0.615 | 1.004 | n/a |
| deep-book | core | extended | 2.34 / 2.43 / 2.57 | 2.21 / 2.46 / 2.04 | 0.943 / 1.011 / 0.795 | 0.943 | n/a |
| mostly-resting | core | extended | 2.29 / 2.16 / 2.36 | 2.22 / 2.38 / 1.65 | 0.970 / 1.103 / 0.700 | 0.970 | n/a |
