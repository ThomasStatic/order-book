base: C:\Users\PC\AppData\Local\Temp\claude\C--dev-order-book\34415323-83ba-4976-9159-a30bcd7c3916\scratchpad\base-v1.exe
exp: C:\Users\PC\AppData\Local\Temp\claude\C--dev-order-book\34415323-83ba-4976-9159-a30bcd7c3916\scratchpad\expA.exe
size standard, 8 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 0.970 | 0.957 - 0.988 | 12% | 8 |
| small-book | replay | 0.856 | 0.853 - 0.867 | 0% | 8 |
| matching-heavy | core | 0.984 | 0.961 - 1.017 | 50% | 8 |
| matching-heavy | replay | 0.891 | 0.870 - 0.904 | 12% | 8 |
