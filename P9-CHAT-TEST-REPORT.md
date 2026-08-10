# `chat_test.in` compatibility and radical-memory evaluation

## Scope and reproducibility

This report evaluates old Prover9 compatibility, the packed hint index,
DISCOUNT clause-frontier policies, the balanced collective frontier, and the
new eager-interreduced demodulation path on the same proof-producing input.
The objective is not merely to lower resident memory: a useful mode must keep
the hint semantics, remain scheduler-live, and either reproduce the old proof
or provide an accurately characterized alternative trajectory.

The input is `/project/bob/chat_test.in`:

```text
size:   7,638,308 bytes
SHA256: 9781ee07691bc62e01f67534208620ca0be3f026f55248a227e9161f1ed17e6c
hints:  88,494 total records
```

`LADR-2026-6A` commit `b36df4c` is the pre-project old-P9 baseline.  Its
benchmark binary has SHA256
`bcdf6bafbf608fde463fd43ef541891813f5c49a2d5153711c54925e98d76bcc`.
The two-posting packed predecessor used below has SHA256
`a7b93205fb3185344adc55b4b1cf68e0e23e37f509798962ca8ff0bbe18585f9`.
The structural-fingerprint build at commit `11f7226` has SHA256
`47fcaf7254ce6cced64688ca01436742e85e391dfe48bce3fe01b9d6c0fa6cf1`.
The mmap-statistics fix at commit `edd4ff0` has SHA256
`cc53da43c1affc80706575b9def34be967a25ff1fd00a8cd81277ce71b210717`.
The current build at commit `3a0dd66`, including the dense-record packing
follow-up, has SHA256
`b19e48d224f6d5c3deb9f1a0182168deb051aa4d0cf0f561c197a9ae7689bd15`.

Runs use `/usr/bin/time -v`, an internal Prover9 limit, an external `timeout`,
and one explicitly pinned CPU per process.  The machine has 23 GiB RAM; three
cases were run concurrently on distinct physical CPUs.  CPU seconds are more
portable than wall time, but even CPU measurements contain normal contention
and frequency noise.  Search counters are the semantic comparison authority.

The reproducible harness is `test.src/chat_test_matrix.sh`.  For example:

```sh
CHAT_CASES='old_otter new_otter_fpa' CHAT_CPU=0 \
  ./test.src/chat_test_matrix.sh /project/bob/chat_test.in \
  /project/chat-test-results/full-baseline 10000 900 2048 960

CHAT_CASES='new_otter_packed' CHAT_CPU=1 \
  ./test.src/chat_test_matrix.sh /project/bob/chat_test.in \
  /project/chat-test-results/full-packed 10000 900 2048 960
```

Harness status `0` means normal proof termination, `4` means `max_seconds`,
and `5` means `max_given`.  A periodic report can interrupt an inference
batch; only terminal equal-given states support exact trajectory comparison.

### Phase-5 compact-OTTER matrix cases

The harness now also defines three packed-fast OTTER cases:

- `new_otter_packed_fast` keeps full passive clauses and the ordinary OTTER
  indexes;
- `new_otter_compact_full` keeps full passive clauses but uses all four
  authoritative compact indexes, isolating index cost from body archiving;
- `new_otter_compact_packed_fast` uses the dense ancestor-backed passive store
  and all four authoritative compact indexes.

It strips inherited `compact_otter_*` controls from the common input, so the
generated case file is the sole authority.  Independent cases can be run in
parallel on distinct CPUs, for example:

```sh
CHAT_CASES=new_otter_packed_fast CHAT_CPU=0 \
  ./test.src/chat_test_matrix.sh /project/bob/chat_test.in results/full \
  300 120 512 180 &
CHAT_CASES=new_otter_compact_packed_fast CHAT_CPU=1 \
  ./test.src/chat_test_matrix.sh /project/bob/chat_test.in results/compact \
  300 120 512 180 &
wait
```

The Phase-5 shared-term-pool build produced this equal-boundary debug result
with the two processes running concurrently:

| Mode | Given | Generated | Kept | User CPU | Peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: |
| packed-fast, full clauses/indexes | 301 | 120,793 | 5,737 | 18.71 s | 90,640 KiB |
| packed-fast, dense + compact indexes | 301 | 120,793 | 5,737 | 21.75 s | 90,636 KiB |

Usable, SOS, demodulator, disabled, hint, and active-hint counts also agree.
The compact run's component statistics match the independent Osborn-prefix
measurement.  At only 300 givens, both processes remain on the roughly 89 MiB
packed-hint and allocator floor, so this is primarily a correctness/CPU gate;
the 1,000-given Osborn run is the useful RSS discriminator.  A deliberately
attempted OTTER+dense case without the compact indexes failed at startup with
the expected authoritative-index guard, and was not added as a matrix mode.

For longer exactness checks where `hint_trace` would emit one candidate line
for every generated clause, `set(search_event_trace)` emits only compact
`KEPT_TRACE` and `GIVEN_TRACE` identity/fingerprint records.  It is disabled
by default and does not affect selection.  At 100 givens the ordinary and
full-body compact-index CHAT cases produce 1,336 byte-identical event lines.

The same `chat_test.in` workload is also the performance/debug boundary for
the compact indexes.  Running `new_otter_compact_full` and the zero-cache
dense archive in parallel to 1,500 givens after the path-filter, unit-root,
and tight-arena fixes gives 254.36 and 256.53 user seconds, respectively.
Both have the same terminal search state; their peak RSS values are 143,480
and 117,940 KiB.  The parallel execution is used only to reduce experiment
turnaround: each process is pinned to a different physical CPU, and all
reported CPU times and states are per process.  This replay confirms that
dense clause materialization is not the remaining speed problem.

The successor back index is checked with the same workflow.  Sparse
root/path-signature buckets pass the focused test and compact-vs-legacy audit
in parallel with a 300-given full-hint replay; all 126,530 CHAT oracle lines
remain byte-identical.  Parallel full-body and zero-cache archive runs to
1,000 givens both end at `Generated=1,268,285`, `Kept=33,909`,
`Sos=26,052`, and `Demods=21,741`.  The archive case uses 103.31 user seconds
and 91,048 KiB peak RSS.  This makes `chat_test.in`, rather than a synthetic
microbenchmark alone, the acceptance oracle for both the signature filter and
its memory/CPU tradeoff.

At 1,500 givens the paired full-body/archive replay remains exact and takes
194.09/202.51 user seconds with 142,996/115,752 KiB peak RSS.  The archive
back-demodulation clock is 16.03 seconds, down from 63.66 before sparse path
buckets.  The remaining comparison against ordinary OTTER is therefore a
compact-rewrite problem, not evidence that dense passive materialization has
undone the back-index gain.

Ordered radix-sibling selection is accepted by the same oracle.  It passes
all compact component tests and the compact-vs-legacy audit, and the 300-given
full-hint trace remains byte-identical.  At 1,000 givens the paired full-body
and archive cases finish in 75.54/78.33 user seconds with the exact terminal
state; their compact `demod` clocks are 19.48/19.32 seconds, down from
35.74/35.48.  Peak RSS is unchanged.

## Exact old and compatibility proof baseline

| Mode | Result | Given | Generated | Kept | User CPU | Wall | Peak RSS |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Old P9 OTTER/FPA | proof | 2,945 | 8,248,034 | 272,789 | 765.69 s | 14:16 | 550,400 KiB |
| Current OTTER/FPA | proof | 2,945 | 8,248,034 | 272,789 | 778.16 s | 14:28 | 445,576 KiB |

The old proof has length 7,051 and uses 3,231 new hints.  `prooftrans
parents_only` reconstructs both proofs.  Apart from the timing comment, the
old and current proof sections are identical.  `directproof` is not applicable
to this proof because that utility requires all paramodulations to be unit;
the failure is a documented utility-domain limitation, not a bad proof.

Current compatibility mode saves 104,824 KiB, or **19.0%**, at the exact same
proof boundary.  CPU is essentially flat on this machine: current P9 is 1.6%
slower in the complete run, within the mixture of allocator wins and changed
low-level costs.  This is a useful compatibility improvement but not the
radical 80--90% memory target.

## Full-hint guarded policy runs before the fingerprint optimization

These runs use the same full input but stop on time, so they characterize
throughput and memory only at the reported prefix.  They must not be read as
proof-boundary RAM values.

| Mode | Limit | Last reported Given | Matched hints | User CPU | Peak RSS |
| --- | --- | ---: | ---: | ---: | ---: |
| Packed OTTER | 900 s | 1,475 | 467 | 864.16 s | 170,852 KiB |
| DISCOUNT clauses, selected demodulation | 900 s | 1,427 | 2,369 | 870.24 s | 98,632 KiB |
| DISCOUNT clauses, eager-interreduced | 900 s | 1,379 | 428 | 869.81 s | 99,592 KiB |
| Collective balanced, selected | 600 s | 667 | 1,515 | 587.01 s | 90,656 KiB |
| Collective balanced, eager-interreduced | 600 s | 665 | 269 | 587.54 s | 90,528 KiB |

Important conclusions:

- Packed hint storage is already a major memory improvement, but ordinary
  matching dominates CPU.
- Dense clause-frontier storage reaches roughly 99 MiB at these prefixes,
  about 82% below the old proof RSS even though the search boundary differs.
- Selected demodulation follows the strongest hint trajectory on this input.
  Eager demodulation contracts more aggressively but removes or changes many
  clauses that would match the supplied historical hint chain.
- Balanced collective scheduling is bounded and memory-efficient, but it
  advances this proof substantially more slowly than the clause frontier.
- Eager-interreduced DISCOUNT crossed the Osborn rewrite-debt threshold twice.
  Both drains exited, ordinary inference continued, and 52 bounded drain
  yields were observed.  The prior permanent-drain liveness bug is fixed.

At the 840-second eager report, dense passive storage held 57,636 compressed
records in 4.66 MB of logical bodies and justifications; its arena backing was
9.08 MB.  The compact rewrite bank and packed hints account for much of the
remaining roughly 99 MB RSS.  This confirms that millions of ordinary full
passive clause objects are no longer the memory growth law.

## Packed matcher diagnosis and correction

At the 1,475-given packed prefix, ordinary matching reported:

```text
queries=825,031
posting_candidates=4,643,264,315
unique_candidates=870,886
materialized=839,323
match clock=531.197 seconds
```

The prior fix bounded matching to the two rarest exact posting lists, but the
second list was still broad.  Most CPU scanned IDs that could be rejected by
the remaining exact shallow query features.

Commit `11f7226` stores a conservative 64-bit structural fingerprint for each
hint.  Ordinary matching now scans only the rarest exact posting, then tests
all query features against the fingerprint in O(1).  Collisions admit extra
candidates only.  Profile/path masks and authoritative exact subsumption
remain unchanged, and back-demodulation continues to intersect every safe
correlated feature.

### Equal-300-given performance

| Mode | Two postings | Fingerprint | CPU change | Fingerprint RSS |
| --- | ---: | ---: | ---: | ---: |
| Packed OTTER | 48.02 s | 42.17 s | -12.2% | 90,504 KiB |
| DISCOUNT selected | 55.24 s | 49.55 s | -10.3% | 90,636 KiB |
| DISCOUNT eager-interreduced | 52.83 s | 44.23 s | -16.3% | 90,632 KiB |
| Collective balanced selected | 126.92 s | 114.76 s | -9.6% | 90,652 KiB |
| Collective balanced eager | 105.83 s | 100.20 s | -5.3% | 90,652 KiB |

For packed OTTER, ordinary posting visits fall from 220.9 million to 82.6
million and its match clock falls from 22.65 to 17.32 seconds.  The fingerprint
rejects 75.7 million seed IDs.  The extra fingerprint capacity is 1 MiB in
this run and does not measurably increase peak RSS.

Every available exact terminal comparison agrees before and after the change:
generated and kept clauses, hint results, compact-rewrite outcomes, collective
frontier state, preview calls/results, discovery promotions, and final
selection statistics.  The full packed/DISCOUNT regression suite, AnyConst
cases, hint checkpoint/restore, dense passive tests, and focused proof tests
also pass.

### FPA as a DISCOUNT middle ground

Packed hints minimize the resident hint bank, but they are not required by
the dense passive store.  A fresh equal-300-given comparison at commit
`edd4ff0` measured the following selected-demodulation configurations:

| Hint index | Given | Generated | Kept | User CPU | Peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: |
| FPA | 301 | 122,541 | 15,039 | 27.72 s | 151,056 KiB |
| Packed | 301 | 122,541 | 15,039 | 50.03 s | 90,636 KiB |

Thus FPA is 44.6% faster at this prefix, while packed saves 60,420 KiB
(40.0% of FPA's resident set).  A second pair with `hint_trace` enabled
produced 15,039 byte-identical committed-candidate records with the same
SHA256, `505085c14b0576018a363cdad16beafddbf04aa9a03b6d2c0cf8f1cd2deda60c`.
Packed marks one additional hint redundant during initial duplicate
detection (25,689 rather than 25,688), but this does not alter any committed
candidate through the tested prefix.  FPA+dense is therefore a useful
throughput/RAM middle ground when the extra roughly 60 MiB is acceptable.

### Follow-up matcher prototypes not promoted

The remaining packed CPU cost was investigated rather than hidden.  Three
prototypes were measured and deliberately left out of the branch:

- Literal-local composite postings cut ordinary posting visits at 300 givens
  from 82.6 million to 8.19 million, but materialized candidates fell only
  from 135,410 to 130,218.  Index construction and lookup raised total CPU
  from 42.17 to 45.80 seconds.
- A bounds-checked matcher over serialized unit bodies cut materializations
  to 13,815.  Three simultaneous predecessor/new pairs had medians of 44.12
  and 43.84 seconds, only a 0.6% change, with identical RSS.  All 5,737
  committed `HINT_TRACE` records were byte-identical, but the codec complexity
  did not earn its keep.
- Increasing exact match-feature depth from two to three raised the same
  bounded run to 50.58 seconds.  More broad/stale memberships and a more
  saturated 64-bit fingerprint outweighed the extra structural information.

These results narrow the next CPU project: another shallow posting tweak or
bounded decoded cache is unlikely to close a 2.72-times gap.  A worthwhile
design needs either a compact discrimination structure that answers actual
one-way matching constraints, or an exact matcher that operates directly on
the serialized representation substantially faster than materialize/match/
recompress.  It must be judged against current FPA, not merely against the
former packed implementation.

## Final post-fingerprint long runs

Three runs with a 2,400-second internal wall-clock guard use commit `11f7226`
and the exact same full input.  The external times below are from
`/usr/bin/time`; status 4 is the normal Prover9 `max_seconds` exit.

| Mode | Result | Given | Generated | Kept | User CPU | Wall | Peak RSS |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Packed OTTER | proof | 2,945 | 8,248,034 | 272,789 | 2,119.65 s | 37:11 | 444,080 KiB |
| DISCOUNT clauses, selected | time limit | 2,529 | 6,748,855 | 451,706 | 2,310.14 s | 40:03 | 153,088 KiB |
| DISCOUNT clauses, eager-interreduced | time limit | 2,467 | 6,351,482 | 234,066 | 2,325.17 s | 40:03 | 204,960 KiB |

Packed OTTER terminates at exactly the old/current-FPA boundary.  Its proof
has the same 7,051 clauses in the same order; only the timing comment differs,
and `prooftrans parents_only` succeeds.  It saves 19.3% from old P9 but just
0.3% from current FPA.  Its 2,119.65 user seconds are 2.72 times current
FPA's 778.16 seconds.  Packed storage is therefore semantically compatible,
but it is not a competitive compatibility default on this input yet.

Selected DISCOUNT is the best measured radical mode: its peak is 72.2% below
old P9's proof RSS while retaining 448,838 dense passives at its last report.
It does not prove within the guard.  Eager interreduction retains only 179,527
dense passives, but its repeated rule-repair work and transient materialized
state produce a higher 204,960-KiB peak.  It enters and exits rewrite drain
eight times, yields 926 times, and ends live with ordinary inference still
advancing; this is contraction overhead, not the former permanent-drain bug.

The last displayed `Hint match stats: ... matched=` value is a snapshot over
hints that remain active, not a cumulative count of historical matches.  It
is 452 for the packed proof (whose proof itself records 3,231 new hints),
3,666 for selected, and 680 for eager.  This is why crossing an old run's
displayed `matched` number neither implies the same proof path nor guarantees
a proof.

### A report-driven mmap residency bug

The last pre-fix selected report shows 56,511,278 bytes of serialized passive
records in a 68,952,417-byte mmap arena.  Before commit `edd4ff0`, every
periodic statistics report visited every active dense passive and verified
the CRC over its entire archived body merely to recompute three payload
totals.  This faulted the nominally cold mapping back into the process and
made reporting frequency determine RSS.  The effect scales with all active
passive bodies and is therefore material for week-long searches.

Commit `edd4ff0` records body, justification, and logical-body sizes in the
dense selector metadata and maintains checked active totals on insertion,
deactivation, reactivation, reset, and compaction.  Reports now obtain the
same values in O(1) without reading the mmap body arena.  The cost is 16 bytes
per allocated selector record: at the 300-given differential point, selector
storage grows from 1,045,128 to 1,343,736 bytes, while all search counters,
final hint state, and reported payload bytes are exact.  The full regression
suite passes.

An intentionally aggressive `report=1` check emitted 51 full statistics
blocks through the same 300-given boundary.  It preserved Generated=122,541,
Kept=15,039 and the final hint state, and peaked at 90,500 KiB versus 90,636
KiB for `report=30`.  Statistics frequency no longer controls passive mmap
residency.  Formatting and writing the extra reports did raise user CPU from
50.03 to 55.21 seconds, so this is a memory regression check rather than a
recommended reporting interval.

Commit `3a0dd66` then packs the four-valued dense-passive `semantics` field
into two unused flag bits.  This reduces the selector record from 72 to 64
bytes and halves the reporting fix's metadata surcharge from 16 to 8 bytes
per allocated record.  At 300 givens, selector storage falls from 1,343,736
to 1,194,432 bytes.  Generated/kept counters and final hint state remain
exact; a regression cycles all four semantics values through direct
deactivation/reactivation, selection, and compaction, and the full
DISCOUNT test suite passes.

### Controlled post-fix long runs

Three simultaneous runs of the `edd4ff0` build used the same 2,400-second
internal wall guard.  The selected pair differs only in reporting interval;
`report=0` deliberately exercises no statistics traversal at all.  Because
the signal-based time limit exits without a terminal statistics block, the
table uses the last periodic state where one exists and external timing/RSS
for every process.

| Demodulation | Report | Last reported Given | Dense passives | User CPU | Wall | Peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Selected | 60 s | 2,268 | 384,755 | 2,312.71 s | 40:03 | 114,012 KiB |
| Selected | off | not reported | not reported | 2,325.95 s | 40:03 | 113,856 KiB |
| Eager-interreduced | 60 s | 2,311 | 155,549 | 2,328.97 s | 40:03 | 211,412 KiB |

All three exited normally at the time guard without a proof.  At the last
selected report, Generated=5,611,955 and Kept=387,359; eager reported
Generated=5,777,032 and Kept=206,901.

The selected controls differ by only 156 KiB (0.14%), with the reporting run
slightly lower.  Periodic statistics therefore no longer drive mmap
residency.  The measured 114,012-KiB peak is 25.5% below the otherwise
equivalent pre-fix selected run and **79.3% below old P9's 550,400-KiB proof
peak**.  This is a guarded-prefix comparison rather than a proof-boundary
claim, but the new run had already retained 384,755 passives at its last
report.  After an arena remap, live `/proc` samples showed a 67,340-KiB
logical passive mapping with only 4--5 MiB resident; newly appended dirty
pages between geometric remaps explain the remaining file-backed peak.

These long runs predate the 64-byte record follow-up.  At 384,755 active
records, `3a0dd66` stores about 3.0 MiB less live selector data than the
measured 72-byte build, so the latest build is expected to land very close to
an 80% old-P9 reduction at the same state.  This is a calculated estimate,
not a substituted measurement.

The mmap fix does not help eager mode's dominant allocation.  Eager retains
far fewer dense passives, yet its 82,504 compact rewrite rules, 62.4-MB rule
bank, repair arrays, and archived disabled clauses raise peak RSS to 211,412
KiB.  It remains scheduler-live, with six completed rewrite drains and 862
bounded yields, but it is not the radical-memory default for this hint
trajectory.

## Packed-fast follow-up

Branch `packed-fast` adds `assign(hint_index,packed_fast)`.  Its final common
path combines density-adaptive exact feature sets, a dependency-scoped query
cache, allocation-free matching against compressed unit hints, and compact
`AnyConst` references.  The last change removes two accidental scans over the
entire allocated hint-ID capacity from every ordinary query.

The exact selected-DISCOUNT boundaries are:

| Index/run | Given | User CPU | Wall | Peak RSS | Result |
| --- | ---: | ---: | ---: | ---: | --- |
| FPA prefix | 301 | 27.72 s | 30 s | 151,056 KiB | bound |
| packed prefix | 301 | 50.02 s | 54 s | 90,636 KiB | bound |
| packed-fast prefix | 301 | 10.23 s | 14 s | 90,496 KiB | bound |
| packed historical report | 1,001 | 420.12 s | 436 s | 90,516 KiB | mid-given |
| packed-fast | 1,001 | 49.78 s | 68 s | 90,492 KiB | bound |
| packed-fast guarded full | last report 4,111 | 672.59 s | 15:03 | 198,112 KiB | time limit |

The 300-given packed-fast run reproduces all 15,039 FPA/packed `HINT_TRACE`
records byte for byte (SHA256
`505085c14b0576018a363cdad16beafddbf04aa9a03b6d2c0cf8f1cd2deda60c`).
At Given #1000, packed-fast and the historical packed run select the same
clause 103726 with the same parents.  Packed-fast ordinary plus flipped hint
time is 7.406 seconds versus 353.586 seconds in the historical report.

The guarded run does not prove `chat_test`: selected DISCOUNT has a different
trajectory from OTTER.  At Given 2,916 it used 330.26 user seconds but already
held 569,005 passives; by the guard it held 1,046,419.  File-backed passive
pages became actively resident, raising peak RSS to 198,112 KiB.  This is
64.0% below old P9's 550,400-KiB proof peak, but it is not the 80--90% stretch
goal.  The remaining work is trajectory-preserving compact inference and
colder passive-selector access, not hint matching.

## Current operational recommendation

For compatibility-sensitive `chat_test` work, use current FPA OTTER.  It has
the same search/proof, essentially the same proof-boundary RSS as packed, and
is 2.72 times faster here:

```text
assign(search_loop,otter).
assign(passive_store,full).
assign(hint_index,fpa).
assign(inference_frontier,clauses).
assign(ancestor_store,off).
set(back_demod_hints).
```

Use packed OTTER only when a larger hint bank demonstrably dominates FPA RAM;
run a bounded prefix first because `chat_test` shows its remaining CPU cost.

FPA can also be combined with dense DISCOUNT if packed matching is the
bottleneck and roughly 60 MiB of additional RAM is affordable on this hint
bank:

```text
assign(search_loop,discount).
assign(passive_store,dense).
assign(discount_demodulation,selected).
assign(hint_index,fpa).
assign(inference_frontier,clauses).
assign(ancestor_store,mmap).
assign(sos_limit,-1).
set(back_demod_hints).
```

For the best radical-memory/hint throughput measured so far on `chat_test`, use
clause-frontier DISCOUNT with selected demodulation:

```text
assign(search_loop,discount).
assign(passive_store,dense).
assign(discount_demodulation,selected).
assign(hint_index,packed_fast).
assign(inference_frontier,clauses).
assign(ancestor_store,mmap).
assign(sos_limit,-1).
set(back_demod_hints).
```

Use eager-interreduced demodulation when contraction is more important than
replaying a historical hint chain:

```text
assign(discount_demodulation,eager_interreduced).
assign(rewrite_refresh_high_water,4096).
assign(rewrite_refresh_low_water,3072).
assign(rewrite_refresh_drain_burst,64).
assign(rewrite_refresh_hot_ratio,7).
assign(rewrite_refresh_raw_budget,64).
assign(rewrite_refresh_inference_ratio,8).
```

For long mmap-backed runs, place `TMPDIR` on a filesystem with sufficient
space.  Measure anonymous and file-backed RSS through `/proc/<pid>/smaps` or a
cgroup; Prover9's historical `Megabytes` counter does not include every mmap.
