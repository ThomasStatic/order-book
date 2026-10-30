base: C:\Users\PC\AppData\Local\Temp\claude\C--dev-order-book\34415323-83ba-4976-9159-a30bcd7c3916\scratchpad\base-v1.exe
exp: C:\Users\PC\AppData\Local\Temp\claude\C--dev-order-book\34415323-83ba-4976-9159-a30bcd7c3916\scratchpad\exp-final.exe
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 0.981 | 0.963 - 0.992 | 6% | 16 |
| small-book | replay | 0.913 | 0.909 - 0.925 | 0% | 16 |
| deep-book | core | 0.985 | 0.952 - 1.050 | 44% | 16 |
| deep-book | replay | 0.924 | 0.914 - 0.941 | 6% | 16 |
| matching-heavy | core | 0.992 | 0.986 - 0.998 | 25% | 16 |
| matching-heavy | replay | 0.931 | 0.925 - 0.934 | 0% | 16 |
| cancellation-heavy | core | 0.947 | 0.873 - 1.029 | 31% | 16 |
| cancellation-heavy | replay | 0.945 | 0.919 - 0.968 | 19% | 16 |
| mostly-resting | core | 0.978 | 0.874 - 1.032 | 31% | 16 |
| mostly-resting | replay | 0.910 | 0.864 - 0.936 | 6% | 16 |
