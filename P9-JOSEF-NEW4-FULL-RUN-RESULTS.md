# Josef 01--03 `new4` proof-endpoint results

## Outcome

All three `new4` runs prove their theorems with the intended clocks-off,
portable-LTO compact configuration.  Their echoed inputs verify the compact
options, while portable-LTO provenance still depends on the separately
recorded build/executable hash because Prover9 output does not encode its link
mode.  They use file-backed dense passive and
selector storage, a 1,048,576-entry selector buffer, `packed_fast` hints, the
2-MiB staged hint cache, and adaptive unit indexing with feature depth 2.  The
complete selected-given trajectories and endpoints match their respective
authorities.  There is no evidence that any speed result comes from search
divergence.

The separately supplied GNU `time -v` records are the authority for
current-process CPU, wall time and peak RSS:

| run | user CPU | system CPU | total CPU | elapsed | max RSS | major faults | file inputs / outputs |
|---|---:|---:|---:|---:|---:|---:|---:|
| Josef 01 | 13,492.74 s | 47.90 s | **13,540.64 s** | 3:45:55 | 10,062,044 KiB (9.596 GiB) | 17,300 | 7,505,888 / 23,445,544 |
| Josef 02 | 5,669.69 s | 173.31 s | **5,843.00 s** | 1:37:48 | 5,131,160 KiB (4.893 GiB) | 3,099 | 4,100,464 / 17,358,888 |
| Josef 03 | 20,358.64 s | 471.63 s | **20,830.27 s** | 5:49:47 | 9,660,824 KiB (9.213 GiB) | 98,556 | 40,245,600 / 50,122,832 |

All three processes received 99% CPU and exited normally with status zero.
The sum of their separate peak-RSS values is 23.703 GiB, but that is only an
upper bound because the peaks need not have occurred simultaneously.  The
runs started within nine seconds of one another and overlapped, so CPU and I/O
numbers describe a realistic concurrent load rather than an isolated serial
benchmark.

GNU `time`'s Linux `Swaps` field is `ru_nswap`, which is commonly left zero by
the kernel and is not a resident-swap measurement.  Prover9's periodic
`/proc` sampler is the relevant check.  It reports zero swap throughout Josef
01 and 02, but Josef 03 reaches 20,896 KiB sampled swapped residency and ends
with 10,468 KiB.  That is small relative to its 9.2-GiB peak, but Josef 03 must
not be described as a zero-swap run.  Its 98,556 major faults and much larger
file-input count are additional reasons not to interpret small timing
differences as isolated CPU effects.

## Search and proof identity

| problem | authority files | endpoint `(given, generated, kept, proofs)` | matched hints | selected-given SHA-256 |
|---|---|---:|---:|---|
| Josef 01 | old P9 / `new3` / `new4` | `(30827, 1602769536, 36195388, 1)` | 48,968 | `c13273c0ccc8a7e3e57306e6a7922952c2c41699575fa259968f45544dc4faa8` |
| Josef 02 | `new3` / `new4` | `(13006, 129776312, 9226457, 1)` | 1,625 | `f5e0dfa2cbbaf6865656e2db16fd53e7827d4a0f1ff011da166b68abb5d3417a` |
| Josef 03 | old P9 / `new3` / `new4` | `(15634, 460797485, 17344322, 1)` | 19,170 | `da6b6cf2476bc75bee05dc5c48104104a77bcf10cb91671dca65cf9d2b5dfd79` |

Each digest covers every printed `given #...` line, not just periodic
checkpoints.  For Josef 03, the normalized 57,787-clause proof also has the
same SHA-256 in old P9, `new3` and `new4`:
`b64e8c8f2b31c4d53299d436ee22a0eebfbe08171c8fce4f93cfbcdf03c5615a`.
Normalization removes only the timing line and the reported proof level.  The
old version reports level 273 and current P9 level 174; every proof clause and
justification is otherwise identical.

The raw `new4` output hashes are:

```text
Josef_01.out.new4  94c8a69149dbcfb3ebcc98eefe2e2f30271191516ac9a306a645f0a8bd584e99
Josef_02.out.new4  ae460aa0f10d359280ce613c9a080167d51dae93ab22b5c0248c023415b63a24
Josef_03.out.new4  fdf4c402a7126fd2e72c63e5adc642ce80e4debd325ce41e30bc26e4b44d3cc2
```

## Performance comparisons

| problem | reference | reference total CPU | `new4` authority CPU | reduction | throughput ratio | scope |
|---|---|---:|---:|---:|---:|---|
| Josef 01 | old P9 on `ar-2` | 19,352.46 s | 13,540.64 s | **30.03%** | **1.429x** | valid proof-to-proof old/new comparison |
| Josef 02 | compact `new3` on `ar-2` | 8,841.30 s | 5,843.00 s | **33.91%** | **1.513x** | no full old-P9 proof is available |
| Josef 03 | compact `new3` on `ar-2` | 29,565.02 s | 20,830.27 s | **29.54%** | **1.419x** | conservative external-current versus internal-reference comparison |

The older Josef 02/03 references expose final Prover9 user/system CPU rather
than GNU `time -v`, so the table deliberately uses the more conservative
external total for `new4`.  On a strictly internal-to-internal basis Josef 03
falls from 29,565.02 to 20,715.55 seconds, a 29.93% reduction (1.427x).

Josef 03's preserved old-P9 output reports 137,568.81 total CPU seconds, making
the raw observed old/current ratio 6.60x.  That is **not** an admissible
old-versus-new speedup claim: old P9 ran on `air-05`, while `new4` ran on
`ar-2`.  The old output remains an excellent semantic authority because its
selected clauses, endpoint, hints and normalized proof match.  A controlled
Josef 03 performance claim requires running preserved old P9 on `ar-2` under
the same resource conditions, preferably with the two competitors run alone
or as a serial reversed pair.

## RAM and adaptive-index interpretation

Josef 01's externally observed 9,826.2-MiB peak RSS compared with old P9's
45,258.48-MiB internal report implies an approximate 78.29% peak reduction;
using final sampled PSS gives 80.21%.  Because the metrics differ, the robust
claim is a roughly 78--80% radical reduction.  The external peak also shows
that the earlier final-PSS-only statement understated Josef 01's maximum by
about 872 MiB, most likely during terminal proof reconstruction/reporting.

Josef 03 `new3` and `new4` peak RSS values are 9,575,356 and 9,660,824 KiB.
The CPU-first adaptive sidecar therefore costs only 83.5 MiB (0.89%) at the
full proof.  Against old P9's 31,222.05-MiB internal report, current peak RSS
suggests an approximate 69.78% reduction, again with a cross-metric caveat.

Adaptive depth 2 is doing real work on Josef 03.  Relative to `new3`'s code
tree, terminal code-tree nodes fall from 3,516,872,738 to 88,994,049 (-97.5%).
Of 34,558,726 adaptive conflict queries, 7,404,015 choose a position route and
7,363,742 of those immediately find an empty position posting.  The mature
sidecar occupies 78.8 MiB within a 1,288.8-MiB unit index.  This is a strong
generalization result for the adaptive policy: it accelerates a third large
problem while adding less than 1% whole-process peak RSS relative to `new3`.

The roughly 30--34% `new3` reductions are whole-configuration results.  They
combine clocks-off execution, portable LTO and multiple accepted source/data
structure changes; they must not be attributed entirely to adaptive unit
indexing or any single optimization.
