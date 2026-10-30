base: C:\Users\PC\AppData\Local\Temp\claude\C--dev-order-book\34415323-83ba-4976-9159-a30bcd7c3916\scratchpad\base-v1.exe
exp: C:\Users\PC\AppData\Local\Temp\claude\C--dev-order-book\34415323-83ba-4976-9159-a30bcd7c3916\scratchpad\expB.exe
size standard, 8 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 0.985 | 0.969 - 0.991 | 12% | 8 |
| small-book | replay | 0.909 | 0.891 - 0.929 | 0% | 8 |
| deep-book | core | 1.007 | 0.959 - 1.057 | 50% | 8 |
| deep-book | replay | 0.913 | 0.892 - 0.929 | 0% | 8 |
| matching-heavy | core | 0.969 | 0.956 - 0.991 | 12% | 8 |
| matching-heavy | replay | 0.907 | 0.884 - 0.932 | 0% | 8 |
| cancellation-heavy | core | 0.949 | 0.929 - 1.043 | 25% | 8 |
| cancellation-heavy | replay | 0.941 | 0.850 - 1.028 | 25% | 8 |
| mostly-resting | core | 0.995 | 0.943 - 1.028 | 50% | 8 |
| mostly-resting | replay | 0.934 | 0.910 - 0.951 | 12% | 8 |
