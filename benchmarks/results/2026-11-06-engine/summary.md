## A/A floors

| Kind | Workload | Median ratio | 95% CI | Floor |
|---|---|---:|---|---:|
| engine | deep-book | 1.006 | 0.998 - 1.011 | ±1.1% |
| engine | quote-refresh | 1.004 | 0.991 - 1.014 | ±1.4% |
| gateway | deep-book | 1.000 | 0.999 - 1.002 | ±0.2% |
| gateway | quote-refresh | 1.001 | 0.997 - 1.004 | ±0.4% |
| queue | deep-book | 1.000 | 0.996 - 1.005 | ±0.5% |
| queue | quote-refresh | 1.003 | 0.995 - 1.014 | ±1.4% |
| shard | deep-book | 1.004 | 0.991 - 1.019 | ±1.9% |
| shard | quote-refresh | 1.010 | 1.001 - 1.014 | ±1.4% |

- engine's floor: ±1.4%
- gateway's floor: ±0.4%
- queue's floor: ±1.4%
- shard's floor: ±1.9%

## The gate: core against tag v2.0.0

| Size | Workload | Kind | Median ratio now/v2.0.0 | 95% CI | Verdict |
|---|---|---|---:|---|---|
| standard | small-book | core | 0.982 | 0.966 - 0.990 | within 3% |
| standard | small-book | replay | 0.955 | 0.950 - 0.960 | reported, not gated |
| standard | deep-book | core | 1.088 | 1.050 - 1.134 | faster |
| standard | deep-book | replay | 0.997 | 0.994 - 1.017 | reported, not gated |
| standard | matching-heavy | core | 1.052 | 1.043 - 1.064 | faster |
| standard | matching-heavy | replay | 0.993 | 0.981 - 0.997 | reported, not gated |
| standard | cancellation-heavy | core | 1.069 | 0.933 - 1.195 | faster |
| standard | cancellation-heavy | replay | 0.978 | 0.970 - 1.004 | reported, not gated |
| standard | mostly-resting | core | 1.277 | 1.216 - 1.330 | faster |
| standard | mostly-resting | replay | 0.997 | 0.990 - 1.011 | reported, not gated |
| standard | quote-refresh | core | 0.963 | 0.952 - 0.967 | MORE THAN 3% SLOWER: explain and accept, or fix |
| standard | quote-refresh | replay | 0.941 | 0.933 - 0.944 | reported, not gated |
| standard | pending-stops | core | 1.132 | 1.122 - 1.140 | faster |
| standard | pending-stops | replay | 1.024 | 1.013 - 1.031 | reported, not gated |
| standard | self-trade | core | 1.087 | 1.055 - 1.097 | faster |
| standard | self-trade | replay | 0.993 | 0.984 - 1.001 | reported, not gated |
| standard | immediate | core | 1.082 | 1.074 - 1.087 | faster |
| standard | immediate | replay | 0.996 | 0.989 - 1.000 | reported, not gated |
| standard | matching-heavy-request | core | 1.076 | 1.065 - 1.081 | faster |
| standard | matching-heavy-request | replay | 0.989 | 0.978 - 0.992 | reported, not gated |
| standard | matching-heavy-owned | core | 1.067 | 1.061 - 1.070 | faster |
| standard | matching-heavy-owned | replay | 0.984 | 0.973 - 0.991 | reported, not gated |
| extended | small-book | core | 0.981 | 0.975 - 0.994 | within 3% |
| extended | small-book | replay | 0.951 | 0.941 - 0.954 | reported, not gated |
| extended | deep-book | core | 1.045 | 1.033 - 1.121 | faster |
| extended | deep-book | replay | 0.997 | 0.982 - 1.004 | reported, not gated |
| extended | matching-heavy | core | 1.047 | 1.041 - 1.050 | faster |
| extended | matching-heavy | replay | 0.999 | 0.995 - 1.006 | reported, not gated |
| extended | cancellation-heavy | core | 1.032 | 0.987 - 1.043 | faster |
| extended | cancellation-heavy | replay | 0.979 | 0.974 - 0.993 | reported, not gated |
| extended | mostly-resting | core | 1.093 | 1.089 - 1.100 | faster |
| extended | mostly-resting | replay | 1.018 | 1.015 - 1.024 | reported, not gated |
| extended | quote-refresh | core | 0.964 | 0.961 - 0.973 | MORE THAN 3% SLOWER: explain and accept, or fix |
| extended | quote-refresh | replay | 0.933 | 0.930 - 0.942 | reported, not gated |
| extended | pending-stops | core | 1.144 | 1.134 - 1.160 | faster |
| extended | pending-stops | replay | 1.031 | 1.023 - 1.036 | reported, not gated |
| extended | self-trade | core | 1.090 | 1.085 - 1.098 | faster |
| extended | self-trade | replay | 0.986 | 0.981 - 0.989 | reported, not gated |
| extended | immediate | core | 1.096 | 1.090 - 1.101 | faster |
| extended | immediate | replay | 1.004 | 1.000 - 1.008 | reported, not gated |
| extended | matching-heavy-request | core | 1.068 | 1.062 - 1.072 | faster |
| extended | matching-heavy-request | replay | 0.988 | 0.981 - 0.993 | reported, not gated |
| extended | matching-heavy-owned | core | 1.066 | 1.054 - 1.070 | faster |
| extended | matching-heavy-owned | replay | 0.987 | 0.981 - 0.992 | reported, not gated |

2 core row(s) outside the gate.

## Paired comparisons

| Pair | Workload | Kind | Median ratio exp/base | 95% CI | Exp faster in | Re-check |
|---|---|---|---:|---|---:|---|
| core-shard-deep-book | deep-book | core | 0.785 | 0.769 - 0.808 | 0% |  |
| core-shard-immediate | immediate | core | 0.919 | 0.916 - 0.923 | 0% |  |
| core-shard-matching-heavy-request | matching-heavy-request | core | 0.914 | 0.905 - 0.920 | 0% |  |
| core-shard-pending-stops | pending-stops | core | 0.961 | 0.955 - 0.969 | 0% |  |
| core-shard-quote-refresh | quote-refresh | core | 0.831 | 0.829 - 0.834 | 0% |  |
| gateway-engine-deep-book-24 | deep-book | gateway | 0.993 | 0.988 - 0.997 | 17% |  |
| gateway-engine-deep-book | deep-book | gateway | 0.998 | 0.989 - 1.002 | 38% | near the floor |
| gateway-engine-immediate-24 | immediate | gateway | 1.005 | 0.993 - 1.026 | 54% |  |
| gateway-engine-immediate | immediate | gateway | 0.996 | 0.983 - 1.012 | 38% | near the floor |
| gateway-engine-matching-heavy-request | matching-heavy-request | gateway | 1.006 | 0.988 - 1.024 | 69% |  |
| gateway-engine-pending-stops | pending-stops | gateway | 1.019 | 1.005 - 1.028 | 81% |  |
| gateway-engine-quote-refresh-24 | quote-refresh | gateway | 1.013 | 1.002 - 1.017 | 75% |  |
| gateway-engine-quote-refresh | quote-refresh | gateway | 1.013 | 1.002 - 1.021 | 75% | near the floor |
| none-process-deep-book | deep-book | queue | 0.766 | 0.759 - 0.776 | 0% |  |
| none-process-immediate | immediate | queue | 0.762 | 0.750 - 0.767 | 0% |  |
| none-process-matching-heavy-request | matching-heavy-request | queue | 0.749 | 0.736 - 0.754 | 0% |  |
| none-process-pending-stops | pending-stops | queue | 0.744 | 0.737 - 0.752 | 0% |  |
| none-process-quote-refresh | quote-refresh | queue | 0.764 | 0.742 - 0.772 | 0% |  |
| process-machine-deep-book | deep-book | queue | 0.970 | 0.965 - 0.984 | 6% |  |
| process-machine-immediate | immediate | queue | 1.081 | 1.065 - 1.087 | 100% |  |
| process-machine-matching-heavy-request | matching-heavy-request | queue | 1.060 | 1.053 - 1.073 | 100% |  |
| process-machine-pending-stops-24 | pending-stops | queue | 1.014 | 1.001 - 1.034 | 71% |  |
| process-machine-pending-stops | pending-stops | queue | 1.009 | 1.003 - 1.018 | 88% | near the floor |
| process-machine-quote-refresh | quote-refresh | queue | 1.060 | 0.953 - 1.086 | 62% |  |
| queue-gateway-deep-book | deep-book | queue | 1.075 | 1.071 - 1.081 | 100% |  |
| queue-gateway-immediate | immediate | queue | 1.126 | 1.117 - 1.134 | 100% |  |
| queue-gateway-matching-heavy-request | matching-heavy-request | queue | 1.096 | 1.087 - 1.105 | 100% |  |
| queue-gateway-pending-stops | pending-stops | queue | 1.205 | 1.192 - 1.216 | 100% |  |
| queue-gateway-quote-refresh | quote-refresh | queue | 0.732 | 0.730 - 0.744 | 0% |  |
| shard-2-against-1-deep-book | deep-book | shard | 1.865 | 1.851 - 1.885 | 100% |  |
| shard-2-against-1-immediate | immediate | shard | 1.905 | 1.895 - 1.919 | 100% |  |
| shard-2-against-1-matching-heavy-request | matching-heavy-request | shard | 1.878 | 1.863 - 1.890 | 100% |  |
| shard-2-against-1-pending-stops | pending-stops | shard | 1.729 | 1.712 - 1.745 | 100% |  |
| shard-2-against-1-quote-refresh | quote-refresh | shard | 1.950 | 1.939 - 1.960 | 100% |  |
| shard-3-against-1-deep-book | deep-book | shard | 2.741 | 2.718 - 2.765 | 100% |  |
| shard-3-against-1-immediate | immediate | shard | 2.625 | 2.610 - 2.653 | 100% |  |
| shard-3-against-1-matching-heavy-request | matching-heavy-request | shard | 2.542 | 2.523 - 2.555 | 100% |  |
| shard-3-against-1-pending-stops | pending-stops | shard | 2.211 | 2.192 - 2.223 | 100% |  |
| shard-3-against-1-quote-refresh | quote-refresh | shard | 2.871 | 2.853 - 2.891 | 100% |  |
| shard-4-against-1-deep-book | deep-book | shard | 3.524 | 3.481 - 3.563 | 100% |  |
| shard-4-against-1-immediate | immediate | shard | 3.171 | 3.145 - 3.187 | 100% |  |
| shard-4-against-1-matching-heavy-request | matching-heavy-request | shard | 3.092 | 3.079 - 3.130 | 100% |  |
| shard-4-against-1-pending-stops | pending-stops | shard | 2.679 | 2.651 - 2.696 | 100% |  |
| shard-4-against-1-quote-refresh | quote-refresh | shard | 3.766 | 3.751 - 3.790 | 100% |  |
| shard-6-against-1-deep-book | deep-book | shard | 4.939 | 4.886 - 4.970 | 100% |  |
| shard-6-against-1-immediate | immediate | shard | 4.081 | 4.026 - 4.105 | 100% |  |
| shard-6-against-1-matching-heavy-request | matching-heavy-request | shard | 4.101 | 4.048 - 4.144 | 100% |  |
| shard-6-against-1-pending-stops | pending-stops | shard | 3.460 | 3.449 - 3.486 | 100% |  |
| shard-6-against-1-quote-refresh | quote-refresh | shard | 5.257 | 5.215 - 5.277 | 100% |  |
| shard-queue-deep-book | deep-book | shard | 0.962 | 0.954 - 0.971 | 0% |  |
| shard-queue-immediate | immediate | shard | 0.838 | 0.834 - 0.841 | 0% |  |
| shard-queue-matching-heavy-request | matching-heavy-request | shard | 0.850 | 0.841 - 0.862 | 0% |  |
| shard-queue-pending-stops | pending-stops | shard | 0.907 | 0.886 - 0.926 | 0% |  |
| shard-queue-quote-refresh | quote-refresh | shard | 0.926 | 0.906 - 0.938 | 0% |  |
| worker-long-immediate | immediate | queue | 0.549 | 0.548 - 0.552 | 0% |  |
| worker-long-pending-stops | pending-stops | queue | 0.568 | 0.566 - 0.570 | 0% |  |
| worker-long-quote-refresh | quote-refresh | queue | 0.531 | 0.530 - 0.532 | 0% |  |

Rerun at 24 pairs: python benchmarks/record_baseline.py ... --only pairs --recheck gateway-engine-deep-book --recheck gateway-engine-immediate --recheck gateway-engine-quote-refresh --recheck process-machine-pending-stops

## Zero-heap before and after: command service time, now against v2.0.0's library (ns)

| Row | p50 before | p50 now | Ratio | p99 before | p99 now | Ratio |
|---|---:|---:|---:|---:|---:|---:|
| command, service time, cancellation-heavy, closed-loop | 100 | 90 | 0.900 | 206 | 200 | 0.976 |
| command, service time, deep-book, closed-loop | 175 | 151 | 0.863 | 556 | 522 | 0.940 |
| command, service time, immediate, closed-loop | 151 | 151 | 1.000 | 660 | 611 | 0.926 |
| command, service time, matching-heavy, closed-loop | 140 | 130 | 0.929 | 777 | 730 | 0.940 |
| command, service time, matching-heavy-owned, closed-loop | 140 | 140 | 1.000 | 783 | 751 | 0.959 |
| command, service time, matching-heavy-request, closed-loop | 140 | 140 | 1.000 | 783 | 746 | 0.953 |
| command, service time, mostly-resting, closed-loop | 196 | 180 | 0.918 | 1058 | 466 | 0.441 |
| command, service time, pending-stops, closed-loop | 180 | 161 | 0.894 | 2272 | 2072 | 0.912 |
| command, service time, quote-refresh, closed-loop | 100 | 100 | 1.000 | 392 | 381 | 0.972 |
| command, service time, self-trade, closed-loop | 151 | 140 | 0.927 | 714 | 673 | 0.943 |
| command, service time, small-book, closed-loop | 90 | 90 | 1.000 | 291 | 266 | 0.912 |

## Medians across sessions

| Cell | Kind | N | M | Instruments | Spread | Placement | Layout | Durability | Sessions | Median ops/s | Session medians | Lowest thread CPU share |
|---|---|---:|---:|---:|---|---|---|---|---:|---:|---|---:|
| books-queue-1-matching-heavy-request | queue | 1 | 1 | 1 | uniform | even | one-core-each | none | 3 | 4,049,673 | 4,017,775, 4,049,673, 4,198,791 | 0.996 |
| books-queue-1-quote-refresh | queue | 1 | 1 | 1 | uniform | even | one-core-each | none | 3 | 6,255,900 | 6,324,983, 6,255,900, 6,066,253 | 0.997 |
| books-queue-512-matching-heavy-request | queue | 1 | 1 | 512 | uniform | even | one-core-each | none | 3 | 1,939,945 | 1,991,307, 1,939,945, 1,859,638 | 0.996 |
| books-queue-512-quote-refresh | queue | 1 | 1 | 512 | uniform | even | one-core-each | none | 3 | 2,134,016 | 2,134,016, 2,130,337, 2,137,699 | 0.996 |
| books-queue-64-matching-heavy-request | queue | 1 | 1 | 64 | uniform | even | one-core-each | none | 3 | 2,491,429 | 2,491,429, 2,518,328, 2,461,380 | 0.995 |
| books-queue-64-quote-refresh | queue | 1 | 1 | 64 | uniform | even | one-core-each | none | 3 | 3,540,352 | 3,596,557, 3,540,352, 3,499,711 | 0.997 |
| books-queue-8-matching-heavy-request | queue | 1 | 1 | 8 | uniform | even | one-core-each | none | 3 | 3,833,087 | 3,833,087, 3,774,112, 3,875,431 | 0.996 |
| books-queue-8-quote-refresh | queue | 1 | 1 | 8 | uniform | even | one-core-each | none | 3 | 5,584,050 | 5,584,050, 5,603,704, 5,562,868 | 0.997 |
| books-shard-1-matching-heavy-request | shard | 1 | 0 | 1 | uniform | even | one-core-each | none | 3 | 5,720,022 | 5,675,288, 5,720,022, 5,744,815 | 0.999 |
| books-shard-1-quote-refresh | shard | 1 | 0 | 1 | uniform | even | one-core-each | none | 3 | 8,925,073 | 8,938,603, 8,746,603, 8,925,073 | 1.000 |
| books-shard-512-matching-heavy-request | shard | 1 | 0 | 512 | uniform | even | one-core-each | none | 3 | 1,937,846 | 1,937,846, 1,946,311, 1,875,130 | 0.999 |
| books-shard-512-quote-refresh | shard | 1 | 0 | 512 | uniform | even | one-core-each | none | 3 | 2,020,424 | 2,043,141, 2,020,424, 2,004,756 | 0.999 |
| books-shard-64-matching-heavy-request | shard | 1 | 0 | 64 | uniform | even | one-core-each | none | 3 | 2,643,793 | 2,648,012, 2,643,793, 2,620,195 | 0.999 |
| books-shard-64-quote-refresh | shard | 1 | 0 | 64 | uniform | even | one-core-each | none | 3 | 3,602,372 | 3,612,586, 3,537,046, 3,602,372 | 0.999 |
| books-shard-8-matching-heavy-request | shard | 1 | 0 | 8 | uniform | even | one-core-each | none | 3 | 4,474,145 | 4,503,014, 4,453,298, 4,474,145 | 0.994 |
| books-shard-8-quote-refresh | shard | 1 | 0 | 8 | uniform | even | one-core-each | none | 3 | 6,198,368 | 6,167,513, 6,198,368, 6,220,333 | 0.999 |
| both-extended-cancellation-heavy | core | 1 | 0 | 1 | uniform | even | unpinned | none | 1 | 11,956,617 | 11,956,617 | - |
| both-extended-cancellation-heavy | replay | 1 | 0 | 1 | uniform | even | unpinned | none | 1 | 3,425,933 | 3,425,933 | - |
| both-extended-deep-book | core | 1 | 0 | 1 | uniform | even | unpinned | none | 1 | 6,331,310 | 6,331,310 | - |
| both-extended-deep-book | replay | 1 | 0 | 1 | uniform | even | unpinned | none | 1 | 2,482,238 | 2,482,238 | - |
| both-extended-immediate | core | 1 | 0 | 1 | uniform | even | unpinned | none | 1 | 6,047,174 | 6,047,174 | - |
| both-extended-immediate | replay | 1 | 0 | 1 | uniform | even | unpinned | none | 1 | 1,956,796 | 1,956,796 | - |
| both-extended-matching-heavy | core | 1 | 0 | 1 | uniform | even | unpinned | none | 1 | 6,330,809 | 6,330,809 | - |
| both-extended-matching-heavy | replay | 1 | 0 | 1 | uniform | even | unpinned | none | 1 | 2,579,190 | 2,579,190 | - |
| both-extended-matching-heavy-owned | core | 1 | 0 | 1 | uniform | even | unpinned | none | 1 | 6,142,774 | 6,142,774 | - |
| both-extended-matching-heavy-owned | replay | 1 | 0 | 1 | uniform | even | unpinned | none | 1 | 1,936,879 | 1,936,879 | - |
| both-extended-matching-heavy-request | core | 1 | 0 | 1 | uniform | even | unpinned | none | 1 | 6,207,197 | 6,207,197 | - |
| both-extended-matching-heavy-request | replay | 1 | 0 | 1 | uniform | even | unpinned | none | 1 | 2,044,017 | 2,044,017 | - |
| both-extended-mostly-resting | core | 1 | 0 | 1 | uniform | even | unpinned | none | 1 | 5,081,048 | 5,081,048 | - |
| both-extended-mostly-resting | replay | 1 | 0 | 1 | uniform | even | unpinned | none | 1 | 2,399,554 | 2,399,554 | - |
| both-extended-pending-stops | core | 1 | 0 | 1 | uniform | even | unpinned | none | 1 | 4,271,115 | 4,271,115 | - |
| both-extended-pending-stops | replay | 1 | 0 | 1 | uniform | even | unpinned | none | 1 | 1,697,555 | 1,697,555 | - |
| both-extended-quote-refresh | core | 1 | 0 | 1 | uniform | even | unpinned | none | 1 | 10,561,067 | 10,561,067 | - |
| both-extended-quote-refresh | replay | 1 | 0 | 1 | uniform | even | unpinned | none | 1 | 2,453,904 | 2,453,904 | - |
| both-extended-self-trade | core | 1 | 0 | 1 | uniform | even | unpinned | none | 1 | 6,225,894 | 6,225,894 | - |
| both-extended-self-trade | replay | 1 | 0 | 1 | uniform | even | unpinned | none | 1 | 1,889,993 | 1,889,993 | - |
| both-extended-small-book | core | 1 | 0 | 1 | uniform | even | unpinned | none | 1 | 11,413,063 | 11,413,063 | - |
| both-extended-small-book | replay | 1 | 0 | 1 | uniform | even | unpinned | none | 1 | 3,318,272 | 3,318,272 | - |
| both-standard-cancellation-heavy | core | 1 | 0 | 1 | uniform | even | unpinned | none | 3 | 12,591,287 | 11,987,533, 12,591,287, 12,618,297 | - |
| both-standard-cancellation-heavy | replay | 1 | 0 | 1 | uniform | even | unpinned | none | 3 | 2,934,496 | 2,941,497, 2,914,025, 2,934,496 | - |
| both-standard-deep-book | core | 1 | 0 | 1 | uniform | even | unpinned | none | 3 | 6,699,045 | 4,694,748, 6,772,040, 6,699,045 | - |
| both-standard-deep-book | replay | 1 | 0 | 1 | uniform | even | unpinned | none | 3 | 2,226,383 | 2,244,775, 2,166,777, 2,226,383 | - |
| both-standard-immediate | core | 1 | 0 | 1 | uniform | even | unpinned | none | 3 | 6,231,656 | 6,231,656, 6,266,881, 5,808,955 | - |
| both-standard-immediate | replay | 1 | 0 | 1 | uniform | even | unpinned | none | 3 | 1,909,024 | 1,905,698, 1,909,024, 1,914,810 | - |
| both-standard-matching-heavy | core | 1 | 0 | 1 | uniform | even | unpinned | none | 3 | 6,672,449 | 6,650,926, 6,679,848, 6,672,449 | - |
| both-standard-matching-heavy | replay | 1 | 0 | 1 | uniform | even | unpinned | none | 3 | 2,506,649 | 2,532,223, 2,506,649, 2,489,265 | - |
| both-standard-matching-heavy-owned | core | 1 | 0 | 1 | uniform | even | unpinned | none | 3 | 6,354,734 | 5,952,523, 6,477,565, 6,354,734 | - |
| both-standard-matching-heavy-owned | replay | 1 | 0 | 1 | uniform | even | unpinned | none | 3 | 1,891,511 | 1,891,511, 1,803,017, 1,912,862 | - |
| both-standard-matching-heavy-request | core | 1 | 0 | 1 | uniform | even | unpinned | none | 3 | 6,502,796 | 6,502,796, 6,447,329, 6,502,923 | - |
| both-standard-matching-heavy-request | replay | 1 | 0 | 1 | uniform | even | unpinned | none | 3 | 1,990,085 | 1,994,865, 1,982,153, 1,990,085 | - |
| both-standard-mostly-resting | core | 1 | 0 | 1 | uniform | even | unpinned | none | 3 | 9,384,913 | 9,384,913, 8,753,195, 9,479,392 | - |
| both-standard-mostly-resting | replay | 1 | 0 | 1 | uniform | even | unpinned | none | 3 | 2,697,992 | 2,697,992, 2,748,227, 2,590,190 | - |
| both-standard-pending-stops | core | 1 | 0 | 1 | uniform | even | unpinned | none | 3 | 4,352,614 | 4,325,840, 4,352,614, 4,405,694 | - |
| both-standard-pending-stops | replay | 1 | 0 | 1 | uniform | even | unpinned | none | 3 | 1,628,773 | 1,628,773, 1,649,261, 1,600,179 | - |
| both-standard-quote-refresh | core | 1 | 0 | 1 | uniform | even | unpinned | none | 3 | 10,490,978 | 10,490,978, 10,606,816, 10,367,314 | - |
| both-standard-quote-refresh | replay | 1 | 0 | 1 | uniform | even | unpinned | none | 3 | 2,421,753 | 2,425,913, 2,421,753, 2,384,023 | - |
| both-standard-self-trade | core | 1 | 0 | 1 | uniform | even | unpinned | none | 3 | 6,272,857 | 6,181,463, 6,332,922, 6,272,857 | - |
| both-standard-self-trade | replay | 1 | 0 | 1 | uniform | even | unpinned | none | 3 | 1,829,157 | 1,829,070, 1,829,157, 1,830,251 | - |
| both-standard-small-book | core | 1 | 0 | 1 | uniform | even | unpinned | none | 3 | 11,528,175 | 11,384,076, 11,626,961, 11,528,175 | - |
| both-standard-small-book | replay | 1 | 0 | 1 | uniform | even | unpinned | none | 3 | 3,273,355 | 3,254,244, 3,291,477, 3,273,355 | - |
| connections-1 | gateway | 1 | 1 | 1 | uniform | even | one-core-each | none | 3 | 3,584,204 | 3,584,204, 3,598,334, 3,576,320 | 0.996 |
| connections-16 | gateway | 1 | 1 | 16 | uniform | even | one-core-each | none | 3 | 4,380,574 | 4,341,335, 4,384,849, 4,380,574 | 0.883 |
| connections-4 | gateway | 1 | 1 | 4 | uniform | even | one-core-each | none | 3 | 3,992,207 | 3,979,335, 3,992,207, 4,010,226 | 0.996 |
| connections-64 | gateway | 1 | 1 | 64 | uniform | even | one-core-each | none | 3 | 3,767,216 | 3,837,084, 3,767,216, 3,761,296 | 0.988 |
| durable-gateway-1-1-machine-extended-deep-book | gateway | 1 | 1 | 8 | uniform | even | one-core-each | machine | 3 | 2,057,454 | 2,044,967, 2,057,454, 2,081,667 | 0.213 |
| durable-gateway-1-1-machine-extended-quote-refresh | gateway | 1 | 1 | 8 | uniform | even | one-core-each | machine | 3 | 4,161,948 | 4,285,427, 4,161,948, 3,640,731 | 0.189 |
| durable-gateway-1-1-machine-zero-filled-deep-book | gateway | 1 | 1 | 8 | uniform | even | one-core-each | machine | 3 | 2,128,972 | 2,048,499, 2,128,972, 2,133,423 | 0.682 |
| durable-gateway-1-1-machine-zero-filled-quote-refresh | gateway | 1 | 1 | 8 | uniform | even | one-core-each | machine | 3 | 4,310,664 | 4,210,806, 4,310,664, 4,324,749 | 0.213 |
| durable-gateway-1-1-process-extended-deep-book | gateway | 1 | 1 | 8 | uniform | even | one-core-each | process | 3 | 2,207,221 | 2,206,819, 2,212,092, 2,207,221 | 0.990 |
| durable-gateway-1-1-process-extended-quote-refresh | gateway | 1 | 1 | 8 | uniform | even | one-core-each | process | 3 | 4,237,114 | 4,299,844, 4,215,738, 4,237,114 | 0.978 |
| durable-gateway-1-1-process-zero-filled-deep-book | gateway | 1 | 1 | 8 | uniform | even | one-core-each | process | 3 | 2,216,911 | 2,182,072, 2,230,395, 2,216,911 | 0.987 |
| durable-gateway-1-1-process-zero-filled-quote-refresh | gateway | 1 | 1 | 8 | uniform | even | one-core-each | process | 3 | 4,282,681 | 4,322,891, 4,275,808, 4,282,681 | 0.973 |
| durable-gateway-2-1-machine-extended-deep-book | gateway | 2 | 1 | 16 | uniform | even | one-core-each | machine | 3 | 3,870,204 | 3,815,353, 3,870,204, 3,922,206 | 0.351 |
| durable-gateway-2-1-machine-extended-quote-refresh | gateway | 2 | 1 | 16 | uniform | even | one-core-each | machine | 3 | 4,471,410 | 4,473,386, 2,496,307, 4,471,410 | 0.191 |
| durable-gateway-2-1-machine-zero-filled-deep-book | gateway | 2 | 1 | 16 | uniform | even | one-core-each | machine | 3 | 4,037,972 | 3,617,444, 4,037,972, 4,048,512 | 0.374 |
| durable-gateway-2-1-machine-zero-filled-quote-refresh | gateway | 2 | 1 | 16 | uniform | even | one-core-each | machine | 3 | 4,440,551 | 4,332,627, 4,440,551, 4,476,870 | 0.409 |
| durable-gateway-2-1-process-extended-deep-book | gateway | 2 | 1 | 16 | uniform | even | one-core-each | process | 3 | 4,229,331 | 4,218,616, 4,253,803, 4,229,331 | 0.963 |
| durable-gateway-2-1-process-extended-quote-refresh | gateway | 2 | 1 | 16 | uniform | even | one-core-each | process | 3 | 4,426,450 | 4,428,087, 4,426,450, 4,365,626 | 0.800 |
| durable-gateway-2-1-process-zero-filled-deep-book | gateway | 2 | 1 | 16 | uniform | even | one-core-each | process | 3 | 4,295,401 | 4,267,830, 4,295,401, 4,298,303 | 0.944 |
| durable-gateway-2-1-process-zero-filled-quote-refresh | gateway | 2 | 1 | 16 | uniform | even | one-core-each | process | 3 | 4,431,382 | 4,426,105, 4,431,382, 4,437,084 | 0.811 |
| durable-queue-1-1-machine-extended-deep-book | queue | 1 | 1 | 8 | uniform | even | one-core-each | machine | 3 | 2,037,768 | 2,046,772, 2,024,413, 2,037,768 | 0.272 |
| durable-queue-1-1-machine-extended-quote-refresh | queue | 1 | 1 | 8 | uniform | even | one-core-each | machine | 3 | 1,253,767 | 1,253,767, 627,739, 4,584,246 | 0.030 |
| durable-queue-1-1-machine-zero-filled-deep-book | queue | 1 | 1 | 8 | uniform | even | one-core-each | machine | 3 | 2,086,157 | 2,084,528, 2,086,157, 2,096,858 | 0.673 |
| durable-queue-1-1-machine-zero-filled-quote-refresh | queue | 1 | 1 | 8 | uniform | even | one-core-each | machine | 3 | 4,712,220 | 4,712,220, 4,729,136, 4,610,945 | 0.181 |
| durable-queue-1-1-process-extended-deep-book | queue | 1 | 1 | 8 | uniform | even | one-core-each | process | 3 | 2,062,234 | 2,075,110, 2,060,712, 2,062,234 | 0.982 |
| durable-queue-1-1-process-extended-quote-refresh | queue | 1 | 1 | 8 | uniform | even | one-core-each | process | 3 | 4,268,488 | 4,303,194, 4,268,488, 4,251,110 | 0.970 |
| durable-queue-1-1-process-zero-filled-deep-book | queue | 1 | 1 | 8 | uniform | even | one-core-each | process | 3 | 2,108,286 | 2,120,160, 2,108,286, 2,061,188 | 0.987 |
| durable-queue-1-1-process-zero-filled-quote-refresh | queue | 1 | 1 | 8 | uniform | even | one-core-each | process | 3 | 4,274,419 | 4,274,419, 4,273,661, 4,284,769 | 0.984 |
| durable-queue-2-2-machine-extended-deep-book | queue | 2 | 2 | 16 | uniform | even | one-core-each | machine | 3 | 3,723,328 | 2,823,123, 3,723,328, 3,764,892 | 0.214 |
| durable-queue-2-2-machine-extended-quote-refresh | queue | 2 | 2 | 16 | uniform | even | one-core-each | machine | 3 | 8,475,079 | 8,475,079, 8,664,505, 8,272,482 | 0.050 |
| durable-queue-2-2-machine-zero-filled-deep-book | queue | 2 | 2 | 16 | uniform | even | one-core-each | machine | 3 | 3,685,009 | 2,842,503, 3,685,009, 3,948,440 | 0.249 |
| durable-queue-2-2-machine-zero-filled-quote-refresh | queue | 2 | 2 | 16 | uniform | even | one-core-each | machine | 3 | 8,757,702 | 3,234,763, 8,757,702, 8,863,779 | 0.242 |
| durable-queue-2-2-process-extended-deep-book | queue | 2 | 2 | 16 | uniform | even | one-core-each | process | 3 | 3,944,985 | 3,969,782, 3,944,985, 3,867,622 | 0.891 |
| durable-queue-2-2-process-extended-quote-refresh | queue | 2 | 2 | 16 | uniform | even | one-core-each | process | 3 | 8,228,353 | 8,114,677, 8,228,353, 8,314,352 | 0.890 |
| durable-queue-2-2-process-zero-filled-deep-book | queue | 2 | 2 | 16 | uniform | even | one-core-each | process | 3 | 3,927,687 | 3,924,934, 3,927,687, 3,956,138 | 0.904 |
| durable-queue-2-2-process-zero-filled-quote-refresh | queue | 2 | 2 | 16 | uniform | even | one-core-each | process | 3 | 8,206,748 | 8,187,071, 8,206,748, 8,219,764 | 0.963 |
| engine-1-1-deep-book | engine | 1 | 1 | 8 | uniform | even | unpinned | process | 3 | 2,651,551 | 2,651,551, 2,651,790, 2,642,768 | - |
| engine-1-1-immediate | engine | 1 | 1 | 8 | uniform | even | unpinned | process | 3 | 3,771,921 | 3,771,921, 3,852,711, 3,760,228 | - |
| engine-1-1-matching-heavy-request | engine | 1 | 1 | 8 | uniform | even | unpinned | process | 3 | 3,851,571 | 3,775,326, 3,851,571, 3,860,826 | - |
| engine-1-1-pending-stops | engine | 1 | 1 | 8 | uniform | even | unpinned | process | 3 | 3,001,344 | 3,035,110, 3,001,344, 2,957,644 | - |
| engine-1-1-quote-refresh | engine | 1 | 1 | 8 | uniform | even | unpinned | process | 3 | 4,128,168 | 4,214,793, 4,128,168, 4,115,584 | - |
| engine-2-1-deep-book | engine | 2 | 1 | 16 | uniform | even | unpinned | process | 3 | 4,230,061 | 4,230,061, 4,206,534, 4,264,279 | - |
| engine-2-1-immediate | engine | 2 | 1 | 16 | uniform | even | unpinned | process | 3 | 4,021,111 | 3,984,454, 4,021,111, 4,022,004 | - |
| engine-2-1-matching-heavy-request | engine | 2 | 1 | 16 | uniform | even | unpinned | process | 3 | 4,130,996 | 4,144,823, 4,130,996, 4,018,657 | - |
| engine-2-1-pending-stops | engine | 2 | 1 | 16 | uniform | even | unpinned | process | 3 | 3,713,309 | 3,820,763, 3,713,309, 3,560,628 | - |
| engine-2-1-quote-refresh | engine | 2 | 1 | 16 | uniform | even | unpinned | process | 3 | 4,308,362 | 4,308,362, 4,350,451, 4,294,783 | - |
| frame-budget-64 | gateway | 1 | 1 | 16 | uniform | even | one-core-each | none | 3 | 4,422,673 | 4,422,673, 4,435,101, 4,418,269 | 0.901 |
| frame-budget-unlimited | gateway | 1 | 1 | 16 | uniform | even | one-core-each | none | 3 | 5,084,841 | 5,123,171, 5,049,722, 5,084,841 | 0.983 |
| gateway-1-1-separate-deep-book | gateway | 1 | 1 | 8 | uniform | even | one-core-each | none | 3 | 2,898,293 | 2,892,804, 2,898,293, 2,899,864 | 0.990 |
| gateway-1-1-separate-immediate | gateway | 1 | 1 | 8 | uniform | even | one-core-each | none | 3 | 4,107,852 | 4,107,852, 4,108,644, 4,044,390 | 0.993 |
| gateway-1-1-separate-matching-heavy-request | gateway | 1 | 1 | 8 | uniform | even | one-core-each | none | 3 | 4,077,638 | 4,077,638, 4,141,361, 4,060,126 | 0.993 |
| gateway-1-1-separate-pending-stops | gateway | 1 | 1 | 8 | uniform | even | one-core-each | none | 3 | 3,357,228 | 3,330,498, 3,357,228, 3,358,169 | 0.990 |
| gateway-1-1-separate-quote-refresh | gateway | 1 | 1 | 8 | uniform | even | one-core-each | none | 3 | 4,171,779 | 4,171,779, 4,183,504, 4,124,790 | 0.983 |
| gateway-2-1-separate-deep-book | gateway | 2 | 1 | 16 | uniform | even | one-core-each | none | 3 | 4,439,589 | 4,455,112, 4,426,794, 4,439,589 | 0.880 |
| gateway-2-1-separate-immediate | gateway | 2 | 1 | 16 | uniform | even | one-core-each | none | 3 | 4,139,912 | 4,108,881, 4,139,912, 4,162,385 | 0.794 |
| gateway-2-1-separate-matching-heavy-request | gateway | 2 | 1 | 16 | uniform | even | one-core-each | none | 3 | 4,232,747 | 4,263,673, 4,207,534, 4,232,747 | 0.799 |
| gateway-2-1-separate-pending-stops | gateway | 2 | 1 | 16 | uniform | even | one-core-each | none | 3 | 3,974,833 | 3,974,833, 3,978,974, 3,973,134 | 0.858 |
| gateway-2-1-separate-quote-refresh | gateway | 2 | 1 | 16 | uniform | even | one-core-each | none | 3 | 4,349,921 | 4,389,368, 4,349,921, 4,343,569 | 0.771 |
| gateway-2-1-unpinned-deep-book | gateway | 2 | 1 | 16 | uniform | even | unpinned | none | 3 | 4,433,600 | 4,433,600, 4,395,247, 4,438,408 | 0.886 |
| gateway-2-1-unpinned-immediate | gateway | 2 | 1 | 16 | uniform | even | unpinned | none | 3 | 4,116,507 | 4,135,361, 4,116,507, 4,080,848 | 0.791 |
| gateway-2-1-unpinned-matching-heavy-request | gateway | 2 | 1 | 16 | uniform | even | unpinned | none | 3 | 4,128,843 | 4,128,843, 4,139,376, 4,095,038 | 0.768 |
| gateway-2-1-unpinned-pending-stops | gateway | 2 | 1 | 16 | uniform | even | unpinned | none | 3 | 3,940,959 | 3,974,273, 3,919,231, 3,940,959 | 0.859 |
| gateway-2-1-unpinned-quote-refresh | gateway | 2 | 1 | 16 | uniform | even | unpinned | none | 3 | 4,307,448 | 4,313,373, 4,294,601, 4,307,448 | 0.754 |
| gateway-2-2-separate-deep-book | gateway | 2 | 2 | 16 | uniform | even | one-core-each | none | 3 | 5,331,434 | 5,390,565, 5,331,434, 5,309,865 | 0.930 |
| gateway-2-2-separate-immediate | gateway | 2 | 2 | 16 | uniform | even | one-core-each | none | 3 | 7,820,476 | 7,820,476, 7,850,874, 7,790,295 | 0.887 |
| gateway-2-2-separate-matching-heavy-request | gateway | 2 | 2 | 16 | uniform | even | one-core-each | none | 3 | 7,806,846 | 7,905,895, 7,774,747, 7,806,846 | 0.927 |
| gateway-2-2-separate-pending-stops | gateway | 2 | 2 | 16 | uniform | even | one-core-each | none | 3 | 5,087,202 | 5,087,202, 5,012,310, 5,183,449 | 0.901 |
| gateway-2-2-separate-quote-refresh | gateway | 2 | 2 | 16 | uniform | even | one-core-each | none | 3 | 8,331,952 | 8,340,875, 8,331,952, 8,295,086 | 0.847 |
| gateway-4-1-separate-deep-book | gateway | 4 | 1 | 32 | uniform | even | one-core-each | none | 3 | 4,703,270 | 4,715,738, 4,703,270, 4,700,486 | 0.620 |
| gateway-4-1-separate-immediate | gateway | 4 | 1 | 32 | uniform | even | one-core-each | none | 3 | 4,187,231 | 4,202,837, 4,176,946, 4,187,231 | 0.568 |
| gateway-4-1-separate-matching-heavy-request | gateway | 4 | 1 | 32 | uniform | even | one-core-each | none | 3 | 4,318,122 | 4,318,122, 4,331,387, 4,293,309 | 0.572 |
| gateway-4-1-separate-pending-stops | gateway | 4 | 1 | 32 | uniform | even | one-core-each | none | 3 | 4,009,367 | 4,022,535, 3,967,630, 4,009,367 | 0.610 |
| gateway-4-1-separate-quote-refresh | gateway | 4 | 1 | 32 | uniform | even | one-core-each | none | 3 | 4,620,950 | 4,620,950, 4,633,418, 4,579,336 | 0.511 |
| gateway-4-2-separate-deep-book | gateway | 4 | 2 | 32 | uniform | even | siblings | none | 3 | 7,933,009 | 7,933,009, 7,781,167, 7,963,070 | 0.805 |
| gateway-4-2-separate-immediate | gateway | 4 | 2 | 32 | uniform | even | siblings | none | 3 | 7,111,353 | 7,674,133, 7,111,353, 6,954,654 | 0.676 |
| gateway-4-2-separate-matching-heavy-request | gateway | 4 | 2 | 32 | uniform | even | siblings | none | 3 | 7,817,684 | 7,743,835, 7,863,142, 7,817,684 | 0.720 |
| gateway-4-2-separate-pending-stops | gateway | 4 | 2 | 32 | uniform | even | siblings | none | 3 | 6,255,417 | 6,255,417, 6,138,388, 6,266,970 | 0.800 |
| gateway-4-2-separate-quote-refresh | gateway | 4 | 2 | 32 | uniform | even | siblings | none | 3 | 8,422,457 | 8,534,721, 8,204,070, 8,422,457 | 0.577 |
| long-1-worker-off-immediate | queue | 1 | 1 | 8 | uniform | even | one-core-each | process | 3 | 2,674,619 | 2,669,207, 2,674,619, 2,675,512 | 0.955 |
| long-1-worker-off-pending-stops | queue | 1 | 1 | 8 | uniform | even | one-core-each | process | 3 | 1,999,006 | 1,992,990, 1,999,006, 2,007,618 | 0.964 |
| long-1-worker-off-quote-refresh | queue | 1 | 1 | 8 | uniform | even | one-core-each | process | 3 | 4,042,503 | 4,037,276, 4,094,521, 4,042,503 | 0.949 |
| long-1-worker-on-immediate | queue | 1 | 1 | 8 | uniform | even | one-core-each | process | 3 | 1,455,324 | 1,459,619, 1,455,324, 1,453,073 | 0.479 |
| long-1-worker-on-pending-stops | queue | 1 | 1 | 8 | uniform | even | one-core-each | process | 3 | 1,148,668 | 1,128,194, 1,148,668, 1,154,269 | 0.492 |
| long-1-worker-on-quote-refresh | queue | 1 | 1 | 8 | uniform | even | one-core-each | process | 3 | 2,140,957 | 2,145,017, 2,140,957, 2,137,028 | 0.448 |
| long-2-worker-off-immediate | queue | 2 | 2 | 16 | uniform | even | one-core-each | process | 3 | 4,839,726 | 4,839,726, 4,840,312, 4,666,996 | 0.871 |
| long-2-worker-off-pending-stops | queue | 2 | 2 | 16 | uniform | even | one-core-each | process | 3 | 3,362,366 | 3,382,443, 3,362,366, 3,340,269 | 0.908 |
| long-2-worker-off-quote-refresh | queue | 2 | 2 | 16 | uniform | even | one-core-each | process | 3 | 6,879,860 | 6,879,860, 7,090,781, 6,700,468 | 0.798 |
| long-2-worker-on-immediate | queue | 2 | 2 | 16 | uniform | even | one-core-each | process | 3 | 2,663,720 | 2,651,721, 2,674,497, 2,663,720 | 0.453 |
| long-2-worker-on-pending-stops | queue | 2 | 2 | 16 | uniform | even | one-core-each | process | 3 | 1,987,479 | 1,985,557, 1,987,479, 1,989,589 | 0.481 |
| long-2-worker-on-quote-refresh | queue | 2 | 2 | 16 | uniform | even | one-core-each | process | 3 | 3,969,257 | 3,969,257, 3,973,012, 3,957,510 | 0.417 |
| mixed-gateway-2-1 | gateway | 2 | 1 | 16 | uniform | even | one-core-each | none | 3 | 4,323,897 | 4,323,897, 4,329,560, 4,321,978 | 0.812 |
| mixed-queue-2-2 | queue | 2 | 2 | 16 | uniform | even | one-core-each | none | 3 | 6,168,289 | 6,243,797, 6,168,289, 6,161,099 | 0.968 |
| mixed-shard-2-1 | shard | 2 | 0 | 16 | uniform | even | one-core-each | none | 3 | 6,810,964 | 6,784,267, 6,810,964, 6,849,442 | 0.995 |
| queue-1-1-separate-deep-book | queue | 1 | 1 | 8 | uniform | even | one-core-each | none | 3 | 2,702,371 | 2,708,873, 2,702,371, 2,699,071 | 0.996 |
| queue-1-1-separate-immediate | queue | 1 | 1 | 8 | uniform | even | one-core-each | none | 3 | 3,683,264 | 3,683,264, 3,596,889, 3,723,850 | 0.996 |
| queue-1-1-separate-matching-heavy-request | queue | 1 | 1 | 8 | uniform | even | one-core-each | none | 3 | 3,787,431 | 3,711,001, 3,787,431, 3,840,628 | 0.993 |
| queue-1-1-separate-pending-stops | queue | 1 | 1 | 8 | uniform | even | one-core-each | none | 3 | 2,787,377 | 2,773,913, 2,819,107, 2,787,377 | 0.997 |
| queue-1-1-separate-quote-refresh | queue | 1 | 1 | 8 | uniform | even | one-core-each | none | 3 | 5,534,717 | 5,456,719, 5,534,717, 5,636,353 | 0.994 |
| queue-2-1-separate-deep-book | queue | 2 | 1 | 16 | uniform | even | one-core-each | none | 3 | 5,066,394 | 5,145,133, 5,066,394, 5,009,524 | 0.961 |
| queue-2-1-separate-immediate | queue | 2 | 1 | 16 | uniform | even | one-core-each | none | 3 | 6,987,935 | 6,754,051, 7,000,438, 6,987,935 | 0.945 |
| queue-2-1-separate-matching-heavy-request | queue | 2 | 1 | 16 | uniform | even | one-core-each | none | 3 | 7,181,924 | 7,181,924, 7,039,002, 7,243,666 | 0.958 |
| queue-2-1-separate-pending-stops | queue | 2 | 1 | 16 | uniform | even | one-core-each | none | 3 | 4,702,347 | 4,702,347, 4,730,281, 4,697,169 | 0.933 |
| queue-2-1-separate-quote-refresh | queue | 2 | 1 | 16 | uniform | even | one-core-each | none | 3 | 11,197,531 | 11,216,157, 11,197,531, 11,026,580 | 0.963 |
| queue-2-2-separate-deep-book | queue | 2 | 2 | 16 | uniform | even | one-core-each | none | 3 | 5,041,556 | 5,020,833, 5,063,309, 5,041,556 | 0.960 |
| queue-2-2-separate-immediate | queue | 2 | 2 | 16 | uniform | even | one-core-each | none | 3 | 6,847,064 | 6,839,442, 6,847,064, 6,864,075 | 0.983 |
| queue-2-2-separate-matching-heavy-request | queue | 2 | 2 | 16 | uniform | even | one-core-each | none | 3 | 7,032,421 | 6,895,370, 7,079,769, 7,032,421 | 0.958 |
| queue-2-2-separate-pending-stops | queue | 2 | 2 | 16 | uniform | even | one-core-each | none | 3 | 4,586,243 | 4,586,243, 4,531,067, 4,629,854 | 0.942 |
| queue-2-2-separate-quote-refresh | queue | 2 | 2 | 16 | uniform | even | one-core-each | none | 3 | 10,760,382 | 10,760,382, 10,716,437, 10,947,788 | 0.963 |
| queue-2-2-siblings-deep-book | queue | 2 | 2 | 16 | uniform | even | producer-on-sibling | none | 3 | 4,517,662 | 4,522,348, 4,517,662, 4,480,980 | 0.966 |
| queue-2-2-siblings-immediate | queue | 2 | 2 | 16 | uniform | even | producer-on-sibling | none | 3 | 6,110,062 | 6,071,181, 6,116,066, 6,110,062 | 0.973 |
| queue-2-2-siblings-matching-heavy-request | queue | 2 | 2 | 16 | uniform | even | producer-on-sibling | none | 3 | 6,142,361 | 6,036,770, 6,142,361, 6,260,200 | 0.965 |
| queue-2-2-siblings-pending-stops | queue | 2 | 2 | 16 | uniform | even | producer-on-sibling | none | 3 | 4,067,962 | 4,070,331, 4,067,962, 4,047,294 | 0.944 |
| queue-2-2-siblings-quote-refresh | queue | 2 | 2 | 16 | uniform | even | producer-on-sibling | none | 3 | 9,983,133 | 10,020,246, 9,983,133, 9,966,746 | 0.979 |
| queue-2-2-unpinned-deep-book | queue | 2 | 2 | 16 | uniform | even | unpinned | none | 3 | 4,940,173 | 4,940,173, 4,912,336, 4,968,497 | 0.962 |
| queue-2-2-unpinned-immediate | queue | 2 | 2 | 16 | uniform | even | unpinned | none | 3 | 6,699,710 | 6,699,710, 6,611,596, 6,736,121 | 0.915 |
| queue-2-2-unpinned-matching-heavy-request | queue | 2 | 2 | 16 | uniform | even | unpinned | none | 3 | 6,873,568 | 6,925,035, 6,699,315, 6,873,568 | 0.949 |
| queue-2-2-unpinned-pending-stops | queue | 2 | 2 | 16 | uniform | even | unpinned | none | 3 | 4,432,879 | 4,417,777, 4,436,834, 4,432,879 | 0.954 |
| queue-2-2-unpinned-quote-refresh | queue | 2 | 2 | 16 | uniform | even | unpinned | none | 3 | 10,542,879 | 10,748,851, 10,439,269, 10,542,879 | 0.945 |
| queue-3-1-separate-deep-book | queue | 3 | 1 | 24 | uniform | even | one-core-each | none | 3 | 7,357,235 | 7,357,812, 7,084,812, 7,357,235 | 0.953 |
| queue-3-1-separate-immediate | queue | 3 | 1 | 24 | uniform | even | one-core-each | none | 3 | 9,517,748 | 9,473,665, 9,517,748, 9,645,642 | 0.910 |
| queue-3-1-separate-matching-heavy-request | queue | 3 | 1 | 24 | uniform | even | one-core-each | none | 3 | 9,848,235 | 9,741,424, 9,848,235, 9,880,196 | 0.924 |
| queue-3-1-separate-pending-stops | queue | 3 | 1 | 24 | uniform | even | one-core-each | none | 3 | 6,109,613 | 6,159,442, 6,109,613, 6,102,332 | 0.908 |
| queue-3-1-separate-quote-refresh | queue | 3 | 1 | 24 | uniform | even | one-core-each | none | 3 | 17,113,061 | 16,978,380, 17,144,836, 17,113,061 | 0.918 |
| queue-3-3-separate-deep-book | queue | 3 | 3 | 24 | uniform | even | one-core-each | none | 3 | 7,265,757 | 7,096,445, 7,332,971, 7,265,757 | 0.936 |
| queue-3-3-separate-immediate | queue | 3 | 3 | 24 | uniform | even | one-core-each | none | 3 | 9,197,522 | 9,313,178, 9,197,522, 9,151,986 | 0.914 |
| queue-3-3-separate-matching-heavy-request | queue | 3 | 3 | 24 | uniform | even | one-core-each | none | 3 | 9,274,858 | 9,274,858, 9,266,258, 9,368,351 | 0.879 |
| queue-3-3-separate-pending-stops | queue | 3 | 3 | 24 | uniform | even | one-core-each | none | 3 | 5,890,891 | 5,868,316, 5,925,636, 5,890,891 | 0.896 |
| queue-3-3-separate-quote-refresh | queue | 3 | 3 | 24 | uniform | even | one-core-each | none | 3 | 15,803,697 | 15,122,950, 15,817,054, 15,803,697 | 0.865 |
| queue-3-3-siblings-deep-book | queue | 3 | 3 | 24 | uniform | even | producer-on-sibling | none | 3 | 6,616,071 | 6,616,071, 6,538,573, 6,645,741 | 0.940 |
| queue-3-3-siblings-immediate | queue | 3 | 3 | 24 | uniform | even | producer-on-sibling | none | 3 | 8,318,127 | 8,391,491, 8,210,268, 8,318,127 | 0.945 |
| queue-3-3-siblings-matching-heavy-request | queue | 3 | 3 | 24 | uniform | even | producer-on-sibling | none | 3 | 8,225,964 | 8,225,964, 8,196,262, 8,258,352 | 0.947 |
| queue-3-3-siblings-pending-stops | queue | 3 | 3 | 24 | uniform | even | producer-on-sibling | none | 3 | 5,365,574 | 5,482,295, 5,244,748, 5,365,574 | 0.885 |
| queue-3-3-siblings-quote-refresh | queue | 3 | 3 | 24 | uniform | even | producer-on-sibling | none | 3 | 14,484,259 | 14,484,259, 14,613,410, 14,354,520 | 0.986 |
| queue-4-4-siblings-deep-book | queue | 4 | 4 | 32 | uniform | even | producer-on-sibling | none | 3 | 8,560,215 | 8,533,788, 8,560,215, 8,629,853 | 0.922 |
| queue-4-4-siblings-immediate | queue | 4 | 4 | 32 | uniform | even | producer-on-sibling | none | 3 | 10,333,242 | 10,386,508, 10,333,242, 10,327,615 | 0.942 |
| queue-4-4-siblings-matching-heavy-request | queue | 4 | 4 | 32 | uniform | even | producer-on-sibling | none | 3 | 10,485,695 | 10,517,868, 10,463,442, 10,485,695 | 0.928 |
| queue-4-4-siblings-pending-stops | queue | 4 | 4 | 32 | uniform | even | producer-on-sibling | none | 3 | 6,724,097 | 6,744,688, 6,724,097, 6,703,574 | 0.909 |
| queue-4-4-siblings-quote-refresh | queue | 4 | 4 | 32 | uniform | even | producer-on-sibling | none | 3 | 18,643,638 | 18,643,638, 18,467,715, 18,886,847 | 0.945 |
| queue-6-6-siblings-deep-book | queue | 6 | 6 | 48 | uniform | even | producer-on-sibling | none | 3 | 12,010,423 | 11,951,756, 12,015,406, 12,010,423 | 0.922 |
| queue-6-6-siblings-immediate | queue | 6 | 6 | 48 | uniform | even | producer-on-sibling | none | 3 | 13,731,633 | 13,705,054, 13,856,701, 13,731,633 | 0.876 |
| queue-6-6-siblings-matching-heavy-request | queue | 6 | 6 | 48 | uniform | even | producer-on-sibling | none | 3 | 14,338,936 | 14,201,597, 14,338,936, 14,468,140 | 0.499 |
| queue-6-6-siblings-pending-stops | queue | 6 | 6 | 48 | uniform | even | producer-on-sibling | none | 3 | 8,817,743 | 8,817,743, 8,788,681, 8,858,334 | 0.562 |
| queue-6-6-siblings-quote-refresh | queue | 6 | 6 | 48 | uniform | even | producer-on-sibling | none | 3 | 25,048,678 | 24,993,304, 25,048,678, 25,253,896 | 0.568 |
| shard-1-1-one-core-deep-book | shard | 1 | 0 | 8 | uniform | even | one-core-each | none | 3 | 2,799,054 | 2,815,027, 2,799,054, 2,773,305 | 0.999 |
| shard-1-1-one-core-immediate | shard | 1 | 0 | 8 | uniform | even | one-core-each | none | 3 | 4,308,267 | 4,274,008, 4,308,267, 4,324,921 | 1.000 |
| shard-1-1-one-core-matching-heavy-request | shard | 1 | 0 | 8 | uniform | even | one-core-each | none | 3 | 4,488,425 | 4,488,425, 4,483,489, 4,539,815 | 0.999 |
| shard-1-1-one-core-pending-stops | shard | 1 | 0 | 8 | uniform | even | one-core-each | none | 3 | 3,080,682 | 3,089,371, 3,080,682, 3,045,342 | 0.999 |
| shard-1-1-one-core-quote-refresh | shard | 1 | 0 | 8 | uniform | even | one-core-each | none | 3 | 6,228,558 | 6,196,082, 6,228,558, 6,241,262 | 1.000 |
| shard-2-1-one-core-deep-book | shard | 2 | 0 | 16 | uniform | even | one-core-each | none | 3 | 5,255,983 | 5,300,109, 5,118,975, 5,255,983 | 0.996 |
| shard-2-1-one-core-immediate | shard | 2 | 0 | 16 | uniform | even | one-core-each | none | 3 | 8,221,325 | 8,235,424, 8,221,325, 8,182,787 | 0.995 |
| shard-2-1-one-core-matching-heavy-request | shard | 2 | 0 | 16 | uniform | even | one-core-each | none | 3 | 8,376,262 | 8,460,137, 8,376,262, 8,321,863 | 0.996 |
| shard-2-1-one-core-pending-stops | shard | 2 | 0 | 16 | uniform | even | one-core-each | none | 3 | 5,293,739 | 5,293,739, 5,324,482, 5,184,971 | 0.995 |
| shard-2-1-one-core-quote-refresh | shard | 2 | 0 | 16 | uniform | even | one-core-each | none | 3 | 12,232,835 | 12,232,835, 12,066,329, 12,257,538 | 0.994 |
| shard-2-1-unpinned-deep-book | shard | 2 | 0 | 16 | uniform | even | unpinned | none | 3 | 5,189,515 | 5,341,967, 5,189,515, 5,141,876 | 1.000 |
| shard-2-1-unpinned-immediate | shard | 2 | 0 | 16 | uniform | even | unpinned | none | 3 | 8,177,464 | 7,998,032, 8,177,464, 8,215,739 | 0.997 |
| shard-2-1-unpinned-matching-heavy-request | shard | 2 | 0 | 16 | uniform | even | unpinned | none | 3 | 8,502,548 | 8,502,548, 8,489,444, 8,507,246 | 0.998 |
| shard-2-1-unpinned-pending-stops | shard | 2 | 0 | 16 | uniform | even | unpinned | none | 3 | 5,162,192 | 5,385,599, 5,135,703, 5,162,192 | 0.996 |
| shard-2-1-unpinned-quote-refresh | shard | 2 | 0 | 16 | uniform | even | unpinned | none | 3 | 12,027,659 | 12,027,659, 12,027,087, 12,135,267 | 0.998 |
| shard-3-1-one-core-deep-book | shard | 3 | 0 | 24 | uniform | even | one-core-each | none | 3 | 7,696,922 | 7,537,064, 7,727,673, 7,696,922 | 0.996 |
| shard-3-1-one-core-immediate | shard | 3 | 0 | 24 | uniform | even | one-core-each | none | 3 | 11,220,780 | 11,220,780, 11,123,891, 11,473,096 | 0.994 |
| shard-3-1-one-core-matching-heavy-request | shard | 3 | 0 | 24 | uniform | even | one-core-each | none | 3 | 11,175,907 | 11,107,228, 11,175,907, 11,582,810 | 0.994 |
| shard-3-1-one-core-pending-stops | shard | 3 | 0 | 24 | uniform | even | one-core-each | none | 3 | 6,701,293 | 6,701,293, 6,863,336, 6,571,125 | 0.994 |
| shard-3-1-one-core-quote-refresh | shard | 3 | 0 | 24 | uniform | even | one-core-each | none | 3 | 17,808,614 | 17,959,125, 17,808,614, 17,804,133 | 1.000 |
| shard-4-1-one-core-deep-book | shard | 4 | 0 | 32 | uniform | even | one-core-each | none | 3 | 9,808,379 | 9,859,125, 9,808,379, 9,705,416 | 0.995 |
| shard-4-1-one-core-immediate | shard | 4 | 0 | 32 | uniform | even | one-core-each | none | 3 | 13,493,647 | 13,745,960, 13,493,647, 12,907,998 | 0.993 |
| shard-4-1-one-core-matching-heavy-request | shard | 4 | 0 | 32 | uniform | even | one-core-each | none | 3 | 13,740,931 | 13,740,931, 13,356,681, 13,776,035 | 0.994 |
| shard-4-1-one-core-pending-stops | shard | 4 | 0 | 32 | uniform | even | one-core-each | none | 3 | 8,163,510 | 8,237,446, 8,025,122, 8,163,510 | 0.996 |
| shard-4-1-one-core-quote-refresh | shard | 4 | 0 | 32 | uniform | even | one-core-each | none | 3 | 23,068,437 | 22,684,649, 23,466,769, 23,068,437 | 0.995 |
| shard-6-1-one-core-deep-book | shard | 6 | 0 | 48 | uniform | even | one-core-each | none | 3 | 13,470,668 | 13,084,150, 13,725,476, 13,470,668 | 0.993 |
| shard-6-1-one-core-immediate | shard | 6 | 0 | 48 | uniform | even | one-core-each | none | 3 | 17,111,558 | 17,455,124, 16,184,102, 17,111,558 | 0.991 |
| shard-6-1-one-core-matching-heavy-request | shard | 6 | 0 | 48 | uniform | even | one-core-each | none | 3 | 18,181,278 | 17,993,751, 18,275,856, 18,181,278 | 0.991 |
| shard-6-1-one-core-pending-stops | shard | 6 | 0 | 48 | uniform | even | one-core-each | none | 3 | 10,542,023 | 10,601,656, 10,542,023, 10,357,870 | 0.991 |
| shard-6-1-one-core-quote-refresh | shard | 6 | 0 | 48 | uniform | even | one-core-each | none | 3 | 33,174,538 | 33,255,958, 32,519,532, 33,174,538 | 0.993 |
| skewed-gateway-4-2-hash | gateway | 4 | 2 | 32 | skewed | hash | siblings | none | 3 | 5,804,262 | 5,814,270, 5,769,353, 5,804,262 | 0.448 |
| skewed-gateway-4-2-hot-alone | gateway | 4 | 2 | 32 | skewed | hot-alone | siblings | none | 3 | 5,847,424 | 5,888,285, 5,847,424, 5,834,179 | 0.495 |
| skewed-queue-3-3-hash | queue | 3 | 3 | 32 | skewed | hash | one-core-each | none | 3 | 10,401,404 | 10,402,446, 10,267,643, 10,401,404 | 0.252 |
| skewed-queue-3-3-hot-alone | queue | 3 | 3 | 32 | skewed | hot-alone | one-core-each | none | 3 | 12,304,255 | 12,304,255, 11,412,494, 12,346,395 | 0.447 |
| skewed-shard-4-1-hash | shard | 4 | 0 | 32 | skewed | hash | one-core-each | none | 3 | 18,532,375 | 18,467,579, 18,563,113, 18,532,375 | 0.995 |
| skewed-shard-4-1-hot-alone | shard | 4 | 0 | 32 | skewed | hot-alone | one-core-each | none | 3 | 19,692,182 | 19,906,677, 16,929,089, 19,692,182 | 0.999 |
| window-1 | gateway | 1 | 1 | 1 | uniform | even | one-core-each | none | 3 | 101,756 | 101,756, 101,307, 102,024 | 0.998 |
| window-16 | gateway | 1 | 1 | 1 | uniform | even | one-core-each | none | 3 | 1,155,842 | 1,155,842, 1,152,452, 1,167,735 | 0.997 |
| window-256 | gateway | 1 | 1 | 1 | uniform | even | one-core-each | none | 3 | 3,591,420 | 3,591,420, 3,636,305, 3,508,356 | 0.996 |
| window-4096 | gateway | 1 | 1 | 1 | uniform | even | one-core-each | none | 3 | 3,911,488 | 3,836,548, 3,961,471, 3,911,488 | 0.994 |

## Latency rows (ns)

| Row | Title | Quotable | p50 | p99 |
|---|---|---|---:|---:|
| command-cancellation-heavy | command, service time, cancellation-heavy, closed-loop | yes | 90 | 201 |
| command-deep-book | command, service time, deep-book, closed-loop | yes | 151 | 511 |
| command-immediate | command, service time, immediate, closed-loop | yes | 151 | 603 |
| command-ladder-matching-heavy-request | command, response time, matching-heavy-request, fixed at 10% | yes | 179 | 865 |
| command-ladder-matching-heavy-request | driver lateness, matching-heavy-request, fixed at 10% | yes | 11 unresolved | 29 unresolved |
| command-ladder-matching-heavy-request | command, response time, matching-heavy-request, fixed at 25% | yes | 179 | 1038 |
| command-ladder-matching-heavy-request | driver lateness, matching-heavy-request, fixed at 25% | yes | 11 unresolved | 474 |
| command-ladder-matching-heavy-request | command, response time, matching-heavy-request, fixed at 50% | yes | 186 | 5743 |
| command-ladder-matching-heavy-request | driver lateness, matching-heavy-request, fixed at 50% | yes | 14 unresolved | 5481 |
| command-ladder-matching-heavy-request | command, response time, matching-heavy-request, fixed at 75% | no (not sustained) | - | - |
| command-ladder-matching-heavy-request | driver lateness, matching-heavy-request, fixed at 75% | no (not sustained) | - | - |
| command-ladder-matching-heavy-request | command, response time, matching-heavy-request, fixed at 90% | no (not sustained) | - | - |
| command-ladder-matching-heavy-request | driver lateness, matching-heavy-request, fixed at 90% | no (not sustained) | - | - |
| command-ladder-matching-heavy-request | command, response time, matching-heavy-request, poisson at 50% | yes | 426 | 10043 |
| command-ladder-matching-heavy-request | driver lateness, matching-heavy-request, poisson at 50% | yes | 221 | 9780 |
| command-ladder-pending-stops | command, response time, pending-stops, fixed at 10% | yes | 200 | 5087 |
| command-ladder-pending-stops | driver lateness, pending-stops, fixed at 10% | yes | 11 unresolved | 2970 |
| command-ladder-pending-stops | command, response time, pending-stops, fixed at 25% | yes | 201 | 9583 |
| command-ladder-pending-stops | driver lateness, pending-stops, fixed at 25% | yes | 12 unresolved | 8796 |
| command-ladder-pending-stops | command, response time, pending-stops, fixed at 50% | yes | 223 | 18117 |
| command-ladder-pending-stops | driver lateness, pending-stops, fixed at 50% | yes | 16 unresolved | 17723 |
| command-ladder-pending-stops | command, response time, pending-stops, fixed at 75% | no (not sustained) | - | - |
| command-ladder-pending-stops | driver lateness, pending-stops, fixed at 75% | no (not sustained) | - | - |
| command-ladder-pending-stops | command, response time, pending-stops, fixed at 90% | no (not sustained) | - | - |
| command-ladder-pending-stops | driver lateness, pending-stops, fixed at 90% | no (not sustained) | - | - |
| command-ladder-pending-stops | command, response time, pending-stops, poisson at 50% | yes | 461 | 18773 |
| command-ladder-pending-stops | driver lateness, pending-stops, poisson at 50% | yes | 217 | 18379 |
| command-matching-heavy-owned | command, service time, matching-heavy-owned, closed-loop | yes | 140 | 751 |
| command-matching-heavy-request | command, service time, matching-heavy-request, closed-loop | yes | 140 | 751 |
| command-matching-heavy | command, service time, matching-heavy, closed-loop | yes | 130 | 722 |
| command-mostly-resting | command, service time, mostly-resting, closed-loop | yes | 180 | 472 |
| command-pending-stops | command, service time, pending-stops, closed-loop | yes | 161 | 2002 |
| command-quote-refresh | command, service time, quote-refresh, closed-loop | yes | 100 | 381 |
| command-self-trade | command, service time, self-trade, closed-loop | yes | 140 | 673 |
| command-small-book | command, service time, small-book, closed-loop | yes | 90 | 260 |
| durable-wait-machine-ladder-rolling | durable-wait, response time, matching-heavy-request, fixed at 10% | yes | 86646 | 145986 |
| durable-wait-machine-ladder-rolling | driver lateness, matching-heavy-request, fixed at 10% | yes | 10 unresolved  noise | 27 unresolved  noise |
| durable-wait-machine-ladder-rolling | durable-wait, response time, matching-heavy-request, fixed at 25% | yes | 86121 | 160689 |
| durable-wait-machine-ladder-rolling | driver lateness, matching-heavy-request, fixed at 25% | yes | 10 unresolved  noise | 29 unresolved  noise |
| durable-wait-machine-ladder-rolling | durable-wait, response time, matching-heavy-request, fixed at 50% | yes | 81920 | 621754 |
| durable-wait-machine-ladder-rolling | driver lateness, matching-heavy-request, fixed at 50% | yes | 11 unresolved | 445310 |
| durable-wait-machine-ladder-rolling | durable-wait, response time, matching-heavy-request, fixed at 75% | yes | 81920 | 125829522 |
| durable-wait-machine-ladder-rolling | driver lateness, matching-heavy-request, fixed at 75% | yes | 11 unresolved | 125829522 |
| durable-wait-machine-ladder-rolling | durable-wait, response time, matching-heavy-request, fixed at 90% | yes | 82445 | 167772697 |
| durable-wait-machine-ladder-rolling | driver lateness, matching-heavy-request, fixed at 90% | yes | 14 unresolved | 167772697 |
| durable-wait-machine-ladder-rolling | durable-wait, response time, matching-heavy-request, poisson at 50% | yes | 95048 | 756187 |
| durable-wait-machine-ladder-rolling | driver lateness, matching-heavy-request, poisson at 50% | yes | 28 unresolved | 613351 |
| durable-wait-machine-ladder | durable-wait, response time, matching-heavy-request, fixed at 10% | yes | 86646 | 147036 |
| durable-wait-machine-ladder | driver lateness, matching-heavy-request, fixed at 10% | yes | 10 unresolved  noise | 27 unresolved  noise |
| durable-wait-machine-ladder | durable-wait, response time, matching-heavy-request, fixed at 25% | yes | 86121 | 144935 |
| durable-wait-machine-ladder | driver lateness, matching-heavy-request, fixed at 25% | yes | 10 unresolved  noise | 28 unresolved  noise |
| durable-wait-machine-ladder | durable-wait, response time, matching-heavy-request, fixed at 50% | yes | 81920 | 181694 |
| durable-wait-machine-ladder | driver lateness, matching-heavy-request, fixed at 50% | yes | 11 unresolved | 12078 |
| durable-wait-machine-ladder | durable-wait, response time, matching-heavy-request, fixed at 75% | yes | 81920 | 84961644 |
| durable-wait-machine-ladder | driver lateness, matching-heavy-request, fixed at 75% | yes | 11 unresolved | 84961644 |
| durable-wait-machine-ladder | durable-wait, response time, matching-heavy-request, fixed at 90% | yes | 82970 | 201111741 |
| durable-wait-machine-ladder | driver lateness, matching-heavy-request, fixed at 90% | yes | 16 unresolved | 201111741 |
| durable-wait-machine-ladder | durable-wait, response time, matching-heavy-request, poisson at 50% | yes | 96098 | 462113 |
| durable-wait-machine-ladder | driver lateness, matching-heavy-request, poisson at 50% | yes | 1231 | 357087 |
| durable-wait-process-ladder-rolling | durable-wait, response time, matching-heavy-request, fixed at 10% | yes | 2593 | 1562782 |
| durable-wait-process-ladder-rolling | driver lateness, matching-heavy-request, fixed at 10% | yes | 11 unresolved | 1529173 |
| durable-wait-process-ladder-rolling | durable-wait, response time, matching-heavy-request, fixed at 25% | yes | 2609 | 3562470 |
| durable-wait-process-ladder-rolling | driver lateness, matching-heavy-request, fixed at 25% | yes | 11 unresolved | 3545666 |
| durable-wait-process-ladder-rolling | durable-wait, response time, matching-heavy-request, fixed at 50% | yes | 2625 | 4436284 |
| durable-wait-process-ladder-rolling | driver lateness, matching-heavy-request, fixed at 50% | yes | 13 unresolved | 4436284 |
| durable-wait-process-ladder-rolling | durable-wait, response time, matching-heavy-request, fixed at 75% | yes | 2707 | 5478138 |
| durable-wait-process-ladder-rolling | driver lateness, matching-heavy-request, fixed at 75% | yes | 17 unresolved | 5478138 |
| durable-wait-process-ladder-rolling | durable-wait, response time, matching-heavy-request, fixed at 90% | yes | 1176287 | 7998754 |
| durable-wait-process-ladder-rolling | driver lateness, matching-heavy-request, fixed at 90% | yes | 1167885 | 7998754 |
| durable-wait-process-ladder-rolling | durable-wait, response time, matching-heavy-request, poisson at 50% | yes | 2806 | 4285047 |
| durable-wait-process-ladder-rolling | driver lateness, matching-heavy-request, poisson at 50% | yes | 28 unresolved | 4268243 |
| durable-wait-process-ladder | durable-wait, response time, matching-heavy-request, fixed at 10% | yes | 2593 | 5481 noise |
| durable-wait-process-ladder | driver lateness, matching-heavy-request, fixed at 10% | yes | 10 unresolved | 28 unresolved  noise |
| durable-wait-process-ladder | durable-wait, response time, matching-heavy-request, fixed at 25% | yes | 2593 | 138634 |
| durable-wait-process-ladder | driver lateness, matching-heavy-request, fixed at 25% | yes | 11 unresolved | 127606 |
| durable-wait-process-ladder | durable-wait, response time, matching-heavy-request, fixed at 50% | yes | 2609 | 869613 |
| durable-wait-process-ladder | driver lateness, matching-heavy-request, fixed at 50% | yes | 11 unresolved | 865412 |
| durable-wait-process-ladder | durable-wait, response time, matching-heavy-request, fixed at 75% | yes | 2642 | 2604638 |
| durable-wait-process-ladder | driver lateness, matching-heavy-request, fixed at 75% | yes | 12 unresolved | 2587834 |
| durable-wait-process-ladder | durable-wait, response time, matching-heavy-request, fixed at 90% | yes | 2658 | 1579587 |
| durable-wait-process-ladder | driver lateness, matching-heavy-request, fixed at 90% | yes | 16 unresolved | 1579587 |
| durable-wait-process-ladder | durable-wait, response time, matching-heavy-request, poisson at 50% | yes | 2789 | 869613 |
| durable-wait-process-ladder | driver lateness, matching-heavy-request, poisson at 50% | yes | 29 unresolved | 865412 |
| hand-off-idle-wake | hand-off, response time, matching-heavy-request, fixed at 50/s | yes | 16935 noise | - |
| hand-off-idle-wake | driver lateness, matching-heavy-request, fixed at 50/s | yes | 10 unresolved  noise | - |
| hand-off-ladder | hand-off, response time, matching-heavy-request, fixed at 10% | yes | 445 | 1460 noise |
| hand-off-ladder | driver lateness, matching-heavy-request, fixed at 10% | yes | 11 unresolved | 28 unresolved  noise |
| hand-off-ladder | hand-off, response time, matching-heavy-request, fixed at 25% | yes | 439 | 1485 |
| hand-off-ladder | driver lateness, matching-heavy-request, fixed at 25% | yes | 11 unresolved | 29 unresolved |
| hand-off-ladder | hand-off, response time, matching-heavy-request, fixed at 50% | yes | 441 | 3315 |
| hand-off-ladder | driver lateness, matching-heavy-request, fixed at 50% | yes | 11 unresolved | 2445 |
| hand-off-ladder | hand-off, response time, matching-heavy-request, fixed at 75% | yes | 445 | 22712 |
| hand-off-ladder | driver lateness, matching-heavy-request, fixed at 75% | yes | 14 unresolved | 22055 |
| hand-off-ladder | hand-off, response time, matching-heavy-request, fixed at 90% | yes | 615 | 743582 |
| hand-off-ladder | driver lateness, matching-heavy-request, fixed at 90% | yes | 126 | 743582 |
| hand-off-ladder | hand-off, response time, matching-heavy-request, poisson at 50% | yes | 611 | 4792 |
| hand-off-ladder | driver lateness, matching-heavy-request, poisson at 50% | yes | 93 | 4020 |
| hand-off | hand-off, service time, matching-heavy-request, closed-loop | yes | 381 | 1181 |
| round-trip-idle-wake | round-trip, response time, matching-heavy-request, fixed at 50/s | yes | 194297 | - |
| round-trip-idle-wake | round-trip to the first frame, response time, matching-heavy-request, fixed at 50/s | yes | 194297 | - |
| round-trip-idle-wake | driver lateness, matching-heavy-request, fixed at 50/s | yes | 10 unresolved  noise | - |
| round-trip-machine-63-idle-ladder | round-trip, response time, matching-heavy-request, fixed at 10% | yes | 124980 | 198498 |
| round-trip-machine-63-idle-ladder | round-trip to the first frame, response time, matching-heavy-request, fixed at 10% | yes | 124980 | 198498 |
| round-trip-machine-63-idle-ladder | driver lateness, matching-heavy-request, fixed at 10% | yes | 11 unresolved  noise | 28 unresolved  noise |
| round-trip-machine-63-idle-ladder | round-trip, response time, matching-heavy-request, fixed at 25% | yes | 121304 | 323479 |
| round-trip-machine-63-idle-ladder | round-trip to the first frame, response time, matching-heavy-request, fixed at 25% | yes | 121304 | 323479 |
| round-trip-machine-63-idle-ladder | driver lateness, matching-heavy-request, fixed at 25% | yes | 11 unresolved  noise | 29 unresolved  noise |
| round-trip-machine-63-idle-ladder | round-trip, response time, matching-heavy-request, fixed at 50% | yes | 109752 | 312976 |
| round-trip-machine-63-idle-ladder | round-trip to the first frame, response time, matching-heavy-request, fixed at 50% | yes | 109752 | 312976 |
| round-trip-machine-63-idle-ladder | driver lateness, matching-heavy-request, fixed at 50% | yes | 11 unresolved  noise | 29 unresolved  noise |
| round-trip-machine-63-idle-ladder | round-trip, response time, matching-heavy-request, fixed at 75% | yes | 113427 | 1209895 |
| round-trip-machine-63-idle-ladder | round-trip to the first frame, response time, matching-heavy-request, fixed at 75% | yes | 113427 | 1209895 |
| round-trip-machine-63-idle-ladder | driver lateness, matching-heavy-request, fixed at 75% | yes | 11 unresolved | 29 unresolved  noise |
| round-trip-machine-63-idle-ladder | round-trip, response time, matching-heavy-request, fixed at 90% | yes | 113953 | 23660181 |
| round-trip-machine-63-idle-ladder | round-trip to the first frame, response time, matching-heavy-request, fixed at 90% | yes | 113953 | 23660181 |
| round-trip-machine-63-idle-ladder | driver lateness, matching-heavy-request, fixed at 90% | yes | 11 unresolved | 29 unresolved  noise |
| round-trip-machine-63-idle-ladder | round-trip, response time, matching-heavy-request, poisson at 50% | yes | 124455 | 277267 |
| round-trip-machine-63-idle-ladder | round-trip to the first frame, response time, matching-heavy-request, poisson at 50% | yes | 124455 | 277267 |
| round-trip-machine-63-idle-ladder | driver lateness, matching-heavy-request, poisson at 50% | yes | 11 unresolved  noise | 2314 noise |
| round-trip-machine-63-idle | round-trip, service time, matching-heavy-request, closed-loop | yes | 115528 | 184845 |
| round-trip-machine-ladder | round-trip, response time, matching-heavy-request, fixed at 10% | yes | 106076 | 192197 |
| round-trip-machine-ladder | round-trip to the first frame, response time, matching-heavy-request, fixed at 10% | yes | 106076 | 192197 |
| round-trip-machine-ladder | driver lateness, matching-heavy-request, fixed at 10% | yes | 11 unresolved  noise | 28 unresolved  noise |
| round-trip-machine-ladder | round-trip, response time, matching-heavy-request, fixed at 25% | yes | 103975 | 187996 |
| round-trip-machine-ladder | round-trip to the first frame, response time, matching-heavy-request, fixed at 25% | yes | 103975 | 187996 |
| round-trip-machine-ladder | driver lateness, matching-heavy-request, fixed at 25% | yes | 11 unresolved  noise | 29 unresolved  noise |
| round-trip-machine-ladder | round-trip, response time, matching-heavy-request, fixed at 50% | yes | 101350 | 183795 |
| round-trip-machine-ladder | round-trip to the first frame, response time, matching-heavy-request, fixed at 50% | yes | 101350 | 183795 |
| round-trip-machine-ladder | driver lateness, matching-heavy-request, fixed at 50% | yes | 11 unresolved | 29 unresolved  noise |
| round-trip-machine-ladder | round-trip, response time, matching-heavy-request, fixed at 75% | yes | 97149 | 73131556 |
| round-trip-machine-ladder | round-trip to the first frame, response time, matching-heavy-request, fixed at 75% | yes | 97149 | 73131556 |
| round-trip-machine-ladder | driver lateness, matching-heavy-request, fixed at 75% | yes | 11 unresolved | 29 unresolved  noise |
| round-trip-machine-ladder | round-trip, response time, matching-heavy-request, fixed at 90% | yes | 97149 | 120989708 |
| round-trip-machine-ladder | round-trip to the first frame, response time, matching-heavy-request, fixed at 90% | yes | 97149 | 120989708 |
| round-trip-machine-ladder | driver lateness, matching-heavy-request, fixed at 90% | yes | 11 unresolved | 29 unresolved  noise |
| round-trip-machine-ladder | round-trip, response time, matching-heavy-request, poisson at 50% | yes | 107651 | 289871 |
| round-trip-machine-ladder | round-trip to the first frame, response time, matching-heavy-request, poisson at 50% | yes | 107651 | 289871 |
| round-trip-machine-ladder | driver lateness, matching-heavy-request, poisson at 50% | yes | 11 unresolved | 2707 noise |
| round-trip-machine | round-trip, service time, matching-heavy-request, closed-loop | yes | 92948 | 160690 |
| round-trip-process-ladder | round-trip, response time, matching-heavy-request, fixed at 10% | yes | 22055 | 53826 |
| round-trip-process-ladder | round-trip to the first frame, response time, matching-heavy-request, fixed at 10% | yes | 22055 | 53826 |
| round-trip-process-ladder | driver lateness, matching-heavy-request, fixed at 10% | yes | 11 unresolved | 29 unresolved  noise |
| round-trip-process-ladder | round-trip, response time, matching-heavy-request, fixed at 25% | yes | 13062 | 36234 |
| round-trip-process-ladder | round-trip to the first frame, response time, matching-heavy-request, fixed at 25% | yes | 13062 | 36234 |
| round-trip-process-ladder | driver lateness, matching-heavy-request, fixed at 25% | yes | 11 unresolved | 29 unresolved  noise |
| round-trip-process-ladder | round-trip, response time, matching-heavy-request, fixed at 50% | yes | 12997 | 48312 |
| round-trip-process-ladder | round-trip to the first frame, response time, matching-heavy-request, fixed at 50% | yes | 12997 | 48312 |
| round-trip-process-ladder | driver lateness, matching-heavy-request, fixed at 50% | yes | 11 unresolved | 29 unresolved  noise |
| round-trip-process-ladder | round-trip, response time, matching-heavy-request, fixed at 75% | yes | 12931 | 33608 |
| round-trip-process-ladder | round-trip to the first frame, response time, matching-heavy-request, fixed at 75% | yes | 12931 | 33608 |
| round-trip-process-ladder | driver lateness, matching-heavy-request, fixed at 75% | yes | 11 unresolved | 29 unresolved  noise |
| round-trip-process-ladder | round-trip, response time, matching-heavy-request, fixed at 90% | yes | 13128 | 52250 |
| round-trip-process-ladder | round-trip to the first frame, response time, matching-heavy-request, fixed at 90% | yes | 13128 | 52250 |
| round-trip-process-ladder | driver lateness, matching-heavy-request, fixed at 90% | yes | 11 unresolved | 29 unresolved  noise |
| round-trip-process-ladder | round-trip, response time, matching-heavy-request, poisson at 50% | yes | 14113 | 37546 |
| round-trip-process-ladder | round-trip to the first frame, response time, matching-heavy-request, poisson at 50% | yes | 14113 | 37546 |
| round-trip-process-ladder | driver lateness, matching-heavy-request, poisson at 50% | yes | 13 unresolved | 5317 noise |
| round-trip-process | round-trip, service time, matching-heavy-request, closed-loop | yes | 12340 | 19298 |

## Noise floors around the threaded sessions

- s1-noise-after: noise floor before: 10.0 s; gaps/s over 1 us 526.1, over 10 us 8.6, over 100 us 0.2; max 257350 ns; lost 0.12%
- s1-noise-after: noise floor after: 10.0 s; gaps/s over 1 us 527.0, over 10 us 8.8, over 100 us 0.0; max 48240 ns; lost 0.12%
- s1-noise-before: noise floor before: 10.0 s; gaps/s over 1 us 533.2, over 10 us 7.7, over 100 us 0.4; max 192650 ns; lost 0.12%
- s1-noise-before: noise floor after: 10.0 s; gaps/s over 1 us 549.2, over 10 us 8.3, over 100 us 0.2; max 201130 ns; lost 0.12%
- s2-noise-after: noise floor before: 10.0 s; gaps/s over 1 us 536.5, over 10 us 4.6, over 100 us 0.2; max 226910 ns; lost 0.09%
- s2-noise-after: noise floor after: 10.0 s; gaps/s over 1 us 528.1, over 10 us 5.8, over 100 us 0.2; max 218740 ns; lost 0.09%
- s2-noise-before: noise floor before: 10.0 s; gaps/s over 1 us 546.8, over 10 us 9.7, over 100 us 0.3; max 210610 ns; lost 0.12%
- s2-noise-before: noise floor after: 10.0 s; gaps/s over 1 us 556.2, over 10 us 9.1, over 100 us 0.1; max 196970 ns; lost 0.12%
- s3-noise-after: noise floor before: 10.0 s; gaps/s over 1 us 540.3, over 10 us 9.3, over 100 us 0.2; max 219550 ns; lost 0.12%
- s3-noise-after: noise floor after: 10.0 s; gaps/s over 1 us 547.0, over 10 us 8.8, over 100 us 0.0; max 55240 ns; lost 0.12%
- s3-noise-before: noise floor before: 10.0 s; gaps/s over 1 us 563.2, over 10 us 6.6, over 100 us 0.1; max 286830 ns; lost 0.10%
- s3-noise-before: noise floor after: 10.0 s; gaps/s over 1 us 534.3, over 10 us 4.9, over 100 us 0.0; max 56120 ns; lost 0.09%

## Steps that failed: 0

