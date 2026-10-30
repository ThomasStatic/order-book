base: C:\Users\PC\AppData\Local\Temp\claude\C--dev-order-book\34415323-83ba-4976-9159-a30bcd7c3916\scratchpad\base-v1.exe
exp: C:\Users\PC\AppData\Local\Temp\claude\C--dev-order-book\34415323-83ba-4976-9159-a30bcd7c3916\scratchpad\exp-head.exe
size extended, 8 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 0.893 | 0.882 - 0.898 | 0% | 8 |
| small-book | replay | 0.853 | 0.842 - 0.861 | 0% | 8 |
| deep-book | core | 0.940 | 0.852 - 0.973 | 12% | 8 |
| deep-book | replay | 0.857 | 0.827 - 0.901 | 0% | 8 |
| matching-heavy | core | 0.899 | 0.897 - 0.905 | 0% | 8 |
| matching-heavy | replay | 0.633 | 0.627 - 0.652 | 0% | 8 |
| cancellation-heavy | core | 0.890 | 0.854 - 1.005 | 25% | 8 |
| cancellation-heavy | replay | 0.840 | 0.830 - 0.856 | 0% | 8 |
| mostly-resting | core | 0.956 | 0.943 - 0.964 | 0% | 8 |
| mostly-resting | replay | 0.928 | 0.902 - 1.082 | 38% | 8 |
