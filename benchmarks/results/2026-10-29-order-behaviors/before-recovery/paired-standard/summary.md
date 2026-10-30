base: C:\Users\PC\AppData\Local\Temp\claude\C--dev-order-book\34415323-83ba-4976-9159-a30bcd7c3916\scratchpad\base-v1.exe
exp: C:\Users\PC\AppData\Local\Temp\claude\C--dev-order-book\34415323-83ba-4976-9159-a30bcd7c3916\scratchpad\exp-head.exe
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 0.897 | 0.890 - 0.902 | 0% | 16 |
| small-book | replay | 0.847 | 0.840 - 0.856 | 0% | 16 |
| deep-book | core | 0.943 | 0.908 - 0.964 | 12% | 16 |
| deep-book | replay | 0.870 | 0.844 - 0.881 | 0% | 16 |
| matching-heavy | core | 0.898 | 0.894 - 0.914 | 0% | 16 |
| matching-heavy | replay | 0.863 | 0.859 - 0.868 | 0% | 16 |
| cancellation-heavy | core | 0.913 | 0.811 - 1.018 | 25% | 16 |
| cancellation-heavy | replay | 0.857 | 0.844 - 0.881 | 6% | 16 |
| mostly-resting | core | 0.989 | 0.913 - 1.041 | 50% | 16 |
| mostly-resting | replay | 0.874 | 0.861 - 0.888 | 0% | 16 |
