# Bucketed merge sweep report

Dataset: `large`

The best configuration is selected by the lowest median `algorithm_ms`, not by `merge_ms` alone.

| Threads | Best buckets | Algorithm ms | CV | Merge ms | Merge share | Algorithm improvement vs baseline |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 8 | 64 | 8647.393 | 1.64% | 553.829 | 6.40% | 34.58% |
| 16 | 64 | 5843.781 | 8.09% | 493.066 | 8.44% | 48.99% |
| 24 | 64 | 5054.269 | 7.57% | 395.725 | 7.83% | 56.05% |
| 32 | 64 | 5272.652 | 6.14% | 437.210 | 8.29% | 58.95% |
| 48 | 64 | 5940.022 | 11.91% | 562.379 | 9.47% | 54.50% |

## Correctness

All bucket configurations passed cross-experiment checksum, graph, and recommendation-quality consistency checks.
