base: <scratch>/bin-exp9b2\order_book_benchmarks.exe
exp: <scratch>/bin-exp6m\order_book_benchmarks.exe
size standard, 16 ABBA pairs per workload, 5 measured runs per invocation, seed 42

| workload | kind | median ratio exp/base | 95% CI | exp faster in | pairs |
|---|---|---:|---|---:|---:|
| small-book | core | 1.192 | 1.182 - 1.206 | 100% | 16 |
| deep-book | core | 1.240 | 1.162 - 1.271 | 100% | 16 |
| matching-heavy | core | 1.129 | 1.118 - 1.142 | 100% | 16 |
| cancellation-heavy | core | 1.273 | 1.231 - 1.303 | 100% | 16 |
| mostly-resting | core | 1.038 | 0.955 - 1.064 | 62% | 16 |
