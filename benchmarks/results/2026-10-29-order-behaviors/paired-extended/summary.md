base: C:\Users\PC\AppData\Local\Temp\claude\C--dev-order-book\34415323-83ba-4976-9159-a30bcd7c3916\scratchpad\base-v1.exe
exp: C:\Users\PC\AppData\Local\Temp\claude\C--dev-order-book\34415323-83ba-4976-9159-a30bcd7c3916\scratchpad\exp-final.exe
size extended, 8 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 0.980 | 0.978 - 0.985 | 12% | 8 |
| small-book | replay | 0.923 | 0.916 - 0.929 | 0% | 8 |
| deep-book | core | 0.953 | 0.901 - 0.989 | 12% | 8 |
| deep-book | replay | 0.917 | 0.908 - 0.932 | 0% | 8 |
| matching-heavy | core | 0.991 | 0.991 - 0.993 | 12% | 8 |
| matching-heavy | replay | 0.930 | 0.922 - 0.935 | 0% | 8 |
| cancellation-heavy | core | 0.977 | 0.933 - 1.086 | 50% | 8 |
| cancellation-heavy | replay | 0.951 | 0.895 - 1.025 | 38% | 8 |
| mostly-resting | core | 1.002 | 0.991 - 1.057 | 50% | 8 |
| mostly-resting | replay | 0.930 | 0.895 - 0.954 | 12% | 8 |
