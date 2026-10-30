base: C:\Users\PC\AppData\Local\Temp\claude\C--dev-order-book\34415323-83ba-4976-9159-a30bcd7c3916\scratchpad\base-p1.exe
exp: C:\Users\PC\AppData\Local\Temp\claude\C--dev-order-book\34415323-83ba-4976-9159-a30bcd7c3916\scratchpad\exp-p2.exe
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 1.002 | 0.991 - 1.037 | 62% | 16 |
| deep-book | core | 0.990 | 0.977 - 1.008 | 44% | 16 |
| matching-heavy | core | 0.999 | 0.990 - 1.015 | 50% | 16 |
| cancellation-heavy | core | 0.976 | 0.955 - 1.000 | 25% | 16 |
| mostly-resting | core | 0.976 | 0.946 - 0.997 | 25% | 16 |
| matching-heavy-request | core | 1.001 | 0.994 - 1.016 | 62% | 16 |
| matching-heavy-owned | core | 1.055 | 1.044 - 1.061 | 100% | 16 |
