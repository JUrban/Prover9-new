# Josef 01 compact-OTTER CPU and RAM report

## Status

Branch `josef01-cpu-next` preserves the Josef 01 search trajectory on every
bounded replay performed here and contains several general, non-Josef-specific
CPU improvements.  It has **not** been run to the Josef 01 proof endpoint on
this 23-GiB development host.  The proof-endpoint CPU result therefore remains
a user-run acceptance gate, not a completed claim.

The completed compact baseline is still an important result: it proves the
same theorem after the same 30,827 given clauses while reducing measured
resident memory by about 80%.  Its CPU cost, however, was 1.90 times old P9.
The work on this branch attacks measured causes of that cost without changing
clause selection, hint answers, or inference order.

### 2026-08-20 measurement correction

The 1.90-times full-run CPU comparison below is real as a record of the two
supplied processes, but it is not a fair measure of the compact search engine.
`Josef_01.out.new3` enabled exact detailed phase clocks with `set(clocks)` and
the default `clock_sample_rate=1`; `Josef_01.out.old` did not enable clocks.
The compact run consequently made kernel CPU-time queries around billions of
search-phase intervals that the old-P9 run never made.

This was isolated on `vmi3142790` with one process at a time, a 2-GiB hard
limit, no swap activity by any measured process, and the exact 1,001-given
endpoint `(Generated=1628048, Kept=320239, proofs=0)`.  All four runs have the
same normalized given-clause SHA-256,
`c23110e9918bbd442865e24d36de6dbc11e2406ceadde0dd36fd55e24bb7be75`.

| Exact 1,001-given control | User CPU | System CPU | Total CPU | Peak RSS | old / control |
|---|---:|---:|---:|---:|---:|
| preserved old P9, clocks off | 111.38 s | 5.44 s | 116.82 s | 697,984 KiB | 1.00 |
| installed pre-polling PGO, exact clocks | 45.90 s | 21.69 s | 67.59 s | 599,492 KiB | 1.73x faster |
| installed pre-polling PGO, clocks sampled 1/16 | 41.16 s | 6.36 s | 47.52 s | 604,256 KiB | 2.46x faster |
| installed pre-polling PGO, clocks off | 37.14 s | 3.71 s | 40.85 s | 609,000 KiB | 2.86x faster |

Thus exact clocks added 26.74 CPU seconds, or 65.5% relative to the clocks-off
process and 39.6% of the exact-clock total, at this prefix.  Sampling retained
approximate phase attribution but still cost 6.67 seconds relative to clocks
off.  `/usr/bin/time -v` remains the authority for total user/system CPU when
internal clocks are disabled.

The installed PGO binary used for this table is SHA-256
`dad5d12683bc8106f4bd43cff4cb7be5d66e200ab4ee7e145579639e209b98f2`;
the preserved old binary is
`bcdf6bafbf608fde463fd43ef541891813f5c49a2d5153711c54925e98d76bcc`.
The clocks-off current output is SHA-256
`164bcc74213885c7e67853dd479c65fa6192ffa83793b2daa65247f63a1248b3` and
the new old-P9 control output is
`2238f376324c7fd5010391063e7686646b914f7067fc7f0492eec89a6113f0a5`.
They are bounded evidence, not substitutes for a proof-endpoint run.

For a throughput comparison, use `clear(clocks)`.  For occasional approximate
phase reports, use `set(clocks)` with `assign(clock_sample_rate,16)`.  Exact
clocks are a diagnostic mode and must be enabled on both competitors if their
process CPU is compared.

### Amortized periodic CPU reports (`josef01-cpu-next`)

Turning detailed clocks off exposed a second diagnostic tax.  With
`assign(report,60)`, the clocks-off profile called `possible_report()` and
`user_time()` once for each of 1,628,048 generated clauses.  The latter is a
`getrusage()` call.  A control with periodic reports disabled reduced the
current PGO prefix from 40.85 to 37.16 total CPU seconds while preserving the
same endpoint and given digest.

`possible_report()` now polls CPU time once per 256 report opportunities.
Given-count reporting remains exact, final statistics are unchanged, and a
CPU-time report can be delayed by at most 255 clause-report opportunities.
On the 1,001-given gate this reduces about 1.63 million CPU-time reads to at
most 6,360; at the completed 1.60-billion-generation endpoint it reduces the
same polling path to about 6.26 million reads.  The change adds no persistent
search memory and cannot affect clause admission, inference or selection.
Every statistics section exposes the cumulative check as
`Periodic_report_poll`; this records generated clauses, actual CPU-time reads
and the fixed interval for an external mature-run audit.

Matched portable `-O2` binaries were run as a serial reversed pair with
clocks off, `report=60`, a 2-GiB limit and no swapping:

| Binary | Mean user CPU | Mean system CPU | Mean total CPU | Mean RSS |
|---|---:|---:|---:|---:|
| parent | 46.80 s | 4.57 s | 51.37 s | 610,108 KiB |
| amortized polling | 45.80 s | 2.48 s | 48.28 s | 609,484 KiB |
| change | -2.1% | -45.8% | **-6.0%** | -624 KiB |

The individual totals were parent/candidate 54.44/48.90 seconds followed by
48.29/47.65 seconds in reversed order.  Every run ended at
`(1001,1628048,320239,0)` with the normalized given digest above.  A separate
100-given `report=1` production smoke emitted six statistics sections,
confirming that short periodic reports remain live after amortization.
An independent `report_given=10` smoke reported at givens 10, 20 and 30
exactly, followed by the final given 31 statistics.

### Balanced PGO retraining audit: not accepted as the general binary

Because `search.c` changed, the installed binary's old profile is not valid
profile-guided optimization data for the current source.  A fresh isolated
PGO audit therefore trained the current branch serially on three workloads:
Josef 01 to 1,001 given clauses, Josef 02 to 601, and CHAT to 601.  All runs
were bounded to 2 GiB, had zero swap, and reproduced their exact endpoint and
normalized selected-given digest.  Training produced 103 profile files
totalling 298,988 bytes.  The instrumented generator and resulting PGO-use
binaries have SHA-256 values:

```text
generator  c1f4c193d096b27426dc323cc2cf4a143cfcdbdac5a3093005b78b42c6597dab
PGO-use   73971bff5c445c558ad7cc3e6fd42b4512f15c96e93fb852768b2dafa467768d
```

The retrained PGO-use binary was promising on its target workload.  In a
serial reversed Josef 01 pair against the installed PGO binary, mean total
CPU fell from 48.12 to 41.17 seconds (-14.5%), mean system CPU from 4.64 to
1.90 seconds (-59.0%), and RSS remained approximately 610 MiB.  Both binaries
ended at `(1001,1628048,320239,0)` with the same given digest.  An independent
Josef 02/600 run also reproduced `(601,524799,26671,0)` in 27.07 total CPU
seconds and 206,712 KiB RSS.

The cross-workload CHAT gate did not confirm a general improvement.  At the
exact `(601,497430,16974,0)` endpoint with a common given digest, the installed
binary averaged 50.81 total CPU seconds and the retrained candidate 52.63
seconds (+3.6%).  The two pair orientations contradicted each other: the
candidate lost the first comparison and won the reversed comparison.  This
is too noisy to claim a regression, but it is also insufficient evidence to
replace a general production executable with a Josef-trained PGO build.

Accordingly, the retrained binary is **not installed** and is not an
authority for the full run.  `bin/prover9` deliberately remains the prior
PGO executable with SHA-256 `dad5d126...`.  The accepted result on this branch
is the portable source-level polling change; final testing must build that
source afresh.  Any future PGO release needs repeated mature gates on Josef
01, Josef 02, CHAT and Osborn, not merely a larger Josef-weighted training
set.

The isolated current-source PGO build also passed
`compact_otter_audit_test.sh` and `compact_generalization_smoke_test.sh` under
hard memory/time limits.  The first checks compact/legacy proof equivalence
and all authoritative/audit index strategies; the second covers legacy and
file-backed compact variants on two distinct problems.  These tests establish
bounded semantic compatibility, not long-run CPU performance.

### Packed-hint cache: mature correction and selective admission

The production default allocates a 2-MiB exact result cache in `packed_fast`.
It is semantically transparent, but every eligible query hashes its complete
shallow feature profile, validates an index-generation dependency and may
store a replacement.  Disabling it looked attractive on short Josef 01
prefixes: with the current stable-key source, a clocks-off reversed pair at
1,001 givens averaged 56.09 seconds cache-on versus 53.33 cache-off, a 5.2%
prefix saving.  All runs ended at `(1001,1628048,320239,0)` with the same
exact match counts and selected-given trajectory.

That result does **not** justify disabling the cache for a day-long run.  The
completed output's cumulative 8.15% hit rate hides improving late reuse.  Its
last three reporting intervals hit 12.6%, 11.9% and 13.5%; each hit in those
intervals avoided approximately 369, 345 and 426 conjunction candidates.
The final cache had avoided 26.82 billion candidates.  Cache-off is therefore
a short-prefix optimization whose mature effect is unknown, not the authority
recommendation.

Commit `6a5d18c` removes avoidable cache overhead without weakening identity.
When the complete conjunction sidecar is resident, deterministic feature
collection order is already an exact key, so the query no longer sorts it.
The memory-budget fallback retains canonical sorting and now restores that
order before storing after sparse posting selection permutes its scratch
vector.  Exact cache hits are regression-tested through both paths.  Reversed
bounded means for the sort removal were:

| Gate | sorted exact key | stable exact key | change |
|---|---:|---:|---:|
| Josef 01 / 1,001 | 51.17 s | 50.48 s | -1.3% |
| CHAT / 601 | 45.21 s | 44.84 s | -0.8% |
| Josef 02 / 601 | 35.76 s | 35.59 s | -0.5% |

Every gate preserved its exact generated/kept endpoint and used zero process
swap.  The gains are small and frequency-sensitive, but they have the right
sign on three different workloads and remove work proportional to cache
queries rather than to a Josef-specific symbol pattern.

The new default-off control

```prolog
assign(hint_cache_min_candidates,128).
```

admits a miss result only if obtaining it scanned at least 128 conjunction or
posting candidates.  Lookups remain enabled, rejected results still pass
through authoritative subsumption, and the compatibility default is zero.
At Josef 01/1,001, 128 reduced stores from 1,804,420 to 203,283 (-88.7%) while
retaining 19.45 million of 20.46 million avoided candidates (95.1%) and about
12 MiB less touched RSS.  Its reversed CPU mean was 50.02 versus 50.43 seconds
for threshold zero (-0.8%), although the individual orientations disagreed.

The cross-workload gate prevents treating 128 as a universal default.  It was
3.1% faster on Josef 02/601 but 1.7% slower on CHAT/601; all endpoints were
exact and all process swap counts were zero.  Keep the general default at
zero.  Threshold 128 is recommended only for the Josef 01 full-run candidate
because it preserves almost all measured expensive reuse while avoiding most
cheap stores, and because the mature output shows that valuable late reuse is
real.  Only that full run can validate the choice.

`CHAT_HINT_CACHE_KB` and `CHAT_HINT_CACHE_MIN_CANDIDATES` let the matrix driver
emit either policy for `new_otter_compact_file_production`.  Both are unset by
default, retaining the general 2-MiB/zero-threshold policy.

### Mature file-selector buffer gate

The completed compact run used a 65,536-entry selector buffer.  At the proof
endpoint, its file selector had 36.11 million run entries and had performed
551 flushes and 546 binary-run merges.  It wrote 8.38 GB and reread 7.52 GB.
This is real merge amplification, not inference work, and motivates the
1,048,576-entry setting in the authority configuration below.

The scale probe was first repaired to use the directory's authoritative
runtime record width.  It had retained a historical 64-byte assertion after
the then-current metadata increased the record to 72 bytes.  The probe accepts
a buffer size and reports directory/selector entry widths, flushes and merges;
after the separately measured directory compaction below, it again asserts
the intentional 64-byte record.  Existing selector order, compaction and
checkpoint tests remain green.

An optimized serial reversed pair inserted four million age-selected records
with one file selector, selected the exact first ID and used zero swap:

| Buffer | Mean user | Mean system | Mean total | Peak RSS | Flush / merge |
|---:|---:|---:|---:|---:|---:|
| 65,536 | 0.96 s | 1.26 s | 2.22 s | 89,636 KiB | 61 / 56 |
| 1,048,576 | 0.71 s | 0.75 s | 1.46 s | 124,086 KiB | 3 / 1 |
| change | -26.0% | -40.6% | **-34.3%** | +34,450 KiB | bounded |

The larger buffer deterministically reduced bytes written from 498,597,888
to 125,829,120 (-74.8%) and bytes read from 403,636,224 to 50,724,864
(-87.4%).  The test is a selector-only scale proxy, so its 34% process result
must not be applied to the complete prover.  It does establish that the
larger buffer trades tens of MiB for substantially less mature selector CPU
and I/O.  Buffers grow on demand per selector; in Josef 01 only `TheRest`
grows to millions of entries, while the other active selector populations at
the proof endpoint were below 20,000.

### Compact file-selector entries

Commit `b3cde19` removes the clause ID duplicated in every file-selector
entry.  Dense directory records are appended in strictly increasing clause-ID
order; directory compaction preserves that order and rebuilds every selector
run.  The record reference is therefore the exact same final age tie-breaker
for age, weight and hint-age selectors.  The file entry now contains only its
eight-byte order key and eight-byte record reference, reducing its enforced
size from 24 to 16 bytes.  This is a representation change, not a scheduling
change.

An optimized 16-million-record reversed scale pair, after both executables
were warmed, preserved the exact first selected ID, selector counts, run
topology and zero-swap result:

| Entry | Mean user | Mean system | Mean total | Peak RSS | Read / written |
|---:|---:|---:|---:|---:|---:|
| 24 bytes | 3.92 s | 4.15 s | 8.07 s | 170,096 KiB | 856,424,448 / 1,233,125,376 B |
| 16 bytes | 3.91 s | 4.01 s | 7.91 s | 157,854 KiB | 570,949,632 / 822,083,584 B |
| change | -0.4% | -3.4% | **-1.9%** | -12,242 KiB | **-33.3% / -33.3%** |

The retained selector runs shrink from 377,487,360 to 251,658,240 bytes and
the 1-Mi-entry input buffer from 25,165,824 to 16,777,216 bytes.  The exact
Josef 01/1,501 reversed whole-process gate had not flushed a selector run and
was correspondingly CPU-neutral/noisy: candidate/control mean user CPU was
73.99/73.38 seconds and mean total CPU was 78.28/77.60 seconds (+0.8%/+0.9%),
while peak RSS fell by 15.8 MiB.  One placement won and one lost.  The change
is retained for its deterministic one-third reduction of the mature merge
data path, not as a short-prefix prover speed claim.

CHAT/601 and Josef 02/601 preserved their exact endpoints
`(497430,16974,0)` and `(524799,26671,0)`, respectively, including final hint
and compact-index counters.  Differential heap/file ordering, reactivation,
compaction, checkpoint and compact-generalization tests pass.  All measured
processes used zero swap.  At the completed Josef 01 baseline's 36.11 million
run entries, the representation alone would reduce the live selector run
footprint by about 276 MiB; the authority run's larger buffer will determine
the actual merge-byte and system-CPU savings.

### Compact dense-passive directory records

Commit `df7bfdb` reduces every dense passive directory record from 72 to 64
bytes.  Search and checkpoint initialization assign hint IDs with an `int`
counter, and packed hint tables already address them as unsigned IDs.  The
directory nevertheless retained an eight-byte copy.  A checked `uint32_t`
copy, moved after the naturally aligned 64-bit fields, removes four redundant
ID bytes and four bytes of tail padding.  The external directory view and
checkpoint format remain 64-bit, and insertion rejects an impossible wider
ID instead of truncating it.  A compile-time size assertion and a differential
`UINT32_MAX` hint-age test guard both boundaries.

An optimized serial reversed scale pair inserted 16 million records with the
same 16-byte selector entries and selected the exact first ID:

| Directory entry | Mean user | Mean system | Mean total | Logical / reserved |
|---:|---:|---:|---:|---:|
| 72 bytes | 3.61 s | 3.63 s | 7.23 s | 1,152,000,000 / 1,323,886,464 B |
| 64 bytes | 3.54 s | 3.36 s | 6.89 s | 1,024,000,000 / 1,176,787,968 B |
| change | -1.9% | -7.4% | **-4.7%** | **-11.1% / -11.1%** |

Selector run topology, selector bytes and all counts were identical; every
process used zero swap.  Peak RSS was effectively unchanged because old mmap
pages are explicitly evicted and the scale probe retains only a bounded hot
window.  The deterministic logical/reserved sizes, rather than one
instantaneous PSS sample, are the relevant mature-memory result.

The exact Josef 01/1,501 reversed whole-process gate improved in both
placements.  Candidate/control mean user CPU was 69.62/71.10 seconds (-2.1%)
and mean total CPU was 73.38/74.90 seconds (-2.0%); peak RSS fell from 686,978
to 683,580 KiB.  All runs ended at
`(Given=1501, Generated=3603947, Kept=521972, proofs=0)`.  CHAT/601 and Josef
02/601 likewise preserved `(497430,16974,0)` and `(524799,26671,0)`, including
final hint and compact-index counters.  Differential ordering, maximum-width
hint IDs, reactivation, compaction, checkpoint and compact-generalization
tests pass.

At the completed baseline's 36,164,363 directory records, the new width would
reduce logical directory size by 289,314,904 bytes (275.9 MiB).  Applying the
same width to that run's recorded capacity reduces reserved directory size by
330,971,616 bytes (315.6 MiB).  The full authority run is still required to
measure its page-cache residency and whole-process CPU contribution.

### Exact-default clause weighting

Commit `f4f4614` specializes only the exact default weighting configuration:
no ordinary weight rules, all symbol weights equal to one, all punctuation
and penalty weights equal to zero, and no installed resonators.  In that case
the clause weight is exactly its number of term nodes.  A depth-first frame
stack now counts those nodes directly instead of allocating one substitution
context per literal and entering the general rule/resonator/penalty evaluator.
Any changed parameter, ordinary rule or resonator keeps the original path.
The specialization has no option and does not alter the search policy.

The strict clocks-off Josef 01/1,501 reversed pair preserved
`(Given=1501, Generated=3603947, Kept=521972, proofs=0)` in every process:

| Portable binary | Mean user | Mean system | Mean total | Mean RSS |
|---|---:|---:|---:|---:|
| 64-byte-directory control | 74.895 s | 4.220 s | 79.115 s | 683,556 KiB |
| exact-default weighting | 74.275 s | 4.385 s | 78.660 s | 683,518 KiB |
| change | **-0.8%** | +3.9% | **-0.6%** | -38 KiB |

The candidate won user CPU in both placements.  It won first-placement total
CPU by 0.98 seconds and lost the reversed total by 0.07 seconds because of a
0.65-second system-CPU movement, so the whole-process effect is deliberately
reported as modest.  The exact-clock 1,501-given operation gate gives the
stronger mechanism check: measured `clock weigh` fell from 4.56 to 3.59
seconds (-21.3%) at the identical endpoint.  Exact clocks made those complete
process totals diagnostic rather than throughput results, as explained at the
start of this report.

CHAT/601 and Josef 02/601 independently preserved
`(497430,16974,0)` and `(524799,26671,0)`.  Custom multi-literal rules,
nondefault parameters and the shipped resonator example also preserved their
control endpoints.  `default_weight_test` permanently covers default
multi-literal/equality counts and all three fallback classes; the compact
generalization smoke passes.  Every measured process used zero swap.  This
change adds only a fixed traversal stack during a weight call and no
persistent per-clause or per-index RAM.

### Owner-free exact hint-cache key ring

Commit `1b15b40` removes the per-key owner array from the `packed_fast`
result cache.  The previous implementation associated every exact profile
key with its owning direct-mapped result slot.  Every admitted result walked
all of its keys, read and rewrote those owners, and invalidated any overlapping
result as the key arena wrapped.  This was exact, but its store-side work grew
with both cache misses and profile width.

The replacement keeps the complete profile keys in a circular arena and tags
each result with the arena generation in which its contiguous segment was
written.  A current-generation segment is live.  A segment from the immediately
previous generation is live exactly while its starting offset is at or beyond
the current write cursor; all older segments are expired.  Only after that
check succeeds does lookup compare the complete key sequence with `memcmp`.
The selected posting dependency is retained as an index into that same exact
sequence and still receives its generation check.  Thus neither a hash
collision nor stale key bytes can produce a hit.  A 32-bit generation rollover
performs a complete cache invalidation; no search-lifetime approximation is
introduced.

The fixed 2-MiB budget is now balanced at six exact keys per 80-byte direct
slot.  It contains 16,384 result slots and 98,304 keys, totalling exactly
2,097,152 bytes, versus 8,192 result slots plus 120,149 keys and owners in
2,097,148 bytes before the change.  Profiles longer than six keys remain fully
supported; they merely advance the ring faster.  Bounded Josef profiles
averaged 6.27 keys and reached nine.  Peak RSS was indistinguishable in the
paired runs, so this is a CPU/working-set rebalance within the existing budget,
not a new RAM allocation.

The completed `Josef_01.out.new3` cache telemetry explains why the change is
aimed at mature scaling rather than a single short input.  It recorded about
2.092 billion queries, 1.922 billion misses, 1.921 billion stores, mean/max
profiles of 6.48/9 keys, 103,751 arena wraps and 202,791,675 overlap
invalidations.  The old owner loop therefore processed approximately 12.45
billion key positions; each iteration read and then wrote owner metadata.
The new store path removes that entire loop.  It adds one constant-time arena
liveness check to a lookup and retains the exact profile comparison.

Strict clocks-off Josef 01/1,501 serial reversed pairs gave:

| Admission policy | Owner-array control | Exact key ring | Total change |
|---|---:|---:|---:|
| general default, `min_candidates=0` | 77.365 s | 76.585 s | **-1.01%** |
| staged Josef policy, `min_candidates=128` | 77.130 s | 76.735 s | **-0.51%** |

The zero-threshold candidate won both placements: 78.45 versus 78.67 seconds,
then 74.72 versus 76.06 seconds after reversing core/order assignment.  Mean
user CPU fell from 72.990 to 72.240 seconds (-1.03%).  Every process ended at
`(Given=1501, Generated=3603947, Kept=521972, proofs=0)` and used zero swap.
At that deterministic endpoint, exact-ring hits rose from 244,582 to 253,770
and avoided posting candidates from 33,879,884 to 34,998,648.  The ring wrapped
259 times and rejected 1,632 expired arena entries.  With threshold 128 it
raised hits from 46,131 to 47,268 and avoided posting candidates from
32,479,125 to 33,354,593.  One threshold-128 placement won and the other tied,
so the whole-process claim there is deliberately limited to non-regression
plus a deterministic reduction in underlying work.

CHAT/601 and Josef 02/601 independently preserved their exact endpoints
`(601,497430,16974,0)` and `(601,524799,26671,0)`.  The focused hint tests now
fill a deliberately tiny cache, force circular wraps, verify reusable
current-generation hits, reject overwritten generations and preserve the
authoritative hint identity on fallback.  `hint-postings-test` and
`compact_generalization_smoke_test.sh` pass.  The measured portable binary is
SHA-256 `0a55dcb64887ee964e70744b3a822c975cbec9d7031ac6d174aba1200941dbca`.
This bounded evidence supports retaining the general mechanism; only the full
authority run can quantify its mature CPU effect.

### Stack-owned eager paramodulation paths

Commit `b1d8128` removes a second long-run allocator loop from inference
construction.  Eager `para_into()` traverses every eligible subterm while
maintaining a position such as `(literal, argument, child, ...)`.  The old
iterative traversal allocated one 16-byte `Ilist` node for every visited
complex-term depth, scanned the linked position to append it, rescanned for
its predecessor after visiting the children, and then freed it.  This happened
even when unification at that position failed and produced no conclusion.

The traversal now keeps its dynamic position suffix in a depth-indexed array
of `struct ilist` nodes on the existing bounded traversal stack.  Linking and
unlinking are constant time.  `paramodulate()` reads the same live list, and
`para_just()` copies it before the conclusion callback can retain anything;
therefore no stack address enters a clause or justification.  Consumer
cancellation detaches the dynamic suffix before returning.  Inference
eligibility, traversal order, substitutions, conclusion order and
justification coordinates are unchanged.  The dynamic stack reservation is
the same size as the previous frame array because the smaller frame and
separate 16-byte coordinate array replace the old per-frame position pointer
and padding.

Commit `9fecf54` applies the same lifetime fact to the two fixed
literal/argument coordinates in each of the `from` and `into` position lists.
Those four nodes also exist only within `para_into_lit()` and are copied into
every retained justification, so the current source keeps the complete live
position on the stack.  This adds four small local nodes to that call frame,
not to a clause, index, cache or passive record.

The exact Josef 01/1,501 endpoint made the mechanism directly measurable:

| Portable binary | Mean user | Mean system | Mean total | Mean RSS | `Ilist` gets |
|---|---:|---:|---:|---:|---:|
| exact-ring parent | 73.045 s | 4.305 s | 77.350 s | 683,532 KiB | 74,720,701 |
| stack path | 70.760 s | 4.230 s | 74.990 s | 683,562 KiB | 43,203,989 |
| change | **-3.13%** | -1.74% | **-3.05%** | +30 KiB | **-42.2%** |

The candidate won both placements: 74.42 versus 76.64 seconds, then 75.56
versus 78.06 seconds after reversing core/order assignment.  It removed
31,516,712 allocator calls and exactly 504,267,392 bytes of cumulative object
traffic.  Live, reserved, reclaimed and slab-reuse states were identical,
because these short-lived nodes already returned to the bounded allocator
pool.  Every run ended at
`(Given=1501, Generated=3603947, Kept=521972, proofs=0)`, including identical
`Generated_by_rule` counts and all normalized final operational statistics;
RSS was effectively unchanged and process swap was zero.

The fixed-prefix extension was then measured incrementally against
`b1d8128`:

| Gate | Stack-suffix parent | Complete stack position | Change | Additional `Ilist` reduction |
|---|---:|---:|---:|---:|
| Josef 01 / 1,501 | 77.775 s | 76.980 s | **-1.02%** | 39.8% |
| CHAT / 601 | 40.040 s | 40.130 s | +0.22% | 46.6% |
| Josef 02 / 601 | 32.825 s | 32.805 s | -0.06% | 43.9% |

Josef won both placements and removed another 17,182,296 allocations plus
274,916,736 cumulative object bytes.  CHAT and Josef 02 each had one win and
one loss and are deliberately classified as neutral, not cross-workload CPU
gains.  All endpoints, rule-generation counts and normalized final operation
counters remained exact, with zero swap.  Relative to the exact-ring parent,
the two commits together reduce Josef's `Ilist` gets from 74,720,701 to
26,021,693 (-65.2%) and cumulative traffic by 779,184,128 bytes at 1,501
givens.

This is not a Josef-shaped fast path.  Reversed cross-workload gates gave:

| Gate | Parent mean total | Stack-path mean total | Change | `Ilist` reduction |
|---|---:|---:|---:|---:|
| CHAT / 601 | 39.120 s | 38.635 s | **-1.24%** | 40.8% |
| Josef 02 / 601 | 33.365 s | 32.860 s | **-1.51%** | 43.7% |

The dynamic-suffix candidate won all four placements and preserved exact
endpoints and rule-generation counts.  ASan/UBSan eager-versus-bounded
iterator tests cover unit and multi-literal conclusions, ordered instance
checks, every forced continuation boundary and consumer cancellation.  The
hint regression, `iterator-tests` and
`compact_generalization_smoke_test.sh` pass.

The completed Josef output contains 32 periodic `Ilist` reports.  Reconstructing
their 32-bit wraparound deltas gives 36,174,451,138 allocations over the run.
The complete stack-position source removes 13.76 nodes per generated
paramodulant at the bounded gate; applying that ratio to 1.600 billion mature
paramodulants suggests an order of 22.02 billion eliminated calls and 328 GiB
less cumulative object traffic.  That is a scaling indication, not a CPU
forecast: term depths and failed unifications change over the search.  The
external authority run must measure
the mature result.  The measured portable binary is SHA-256
`4a32437f5ff84e98946ae1309b130d9d7b9b574dbd949ca76521d275be03ff92`.

### Bitset-owned forward-generalization undo state

Commit `b3d19f3` removes another recursive hot-path array without adding an
index or cache.  Each compact-unit forward-generalization edge may bind a
stored-pattern variable before recursing.  The old implementation recorded
the variable numbers in an `unsigned new_bindings[MAX_VARS]` array and replayed
that array to clear the bindings after the child returned.  With
`MAX_VARS=100`, optimized `generalization_rec()` reserved 488 bytes per
recursive frame even though undo only needs the exact set of newly bound
slots.

The current implementation records that set in two 64-bit words and clears
set bits after the recursive call.  Binding order is immaterial because undo
only writes independent slots to null; matching, repeated-variable checks,
child order and returned proof IDs are unchanged.  A compile-time bound
rejects any future `MAX_VARS > 128` configuration instead of silently losing
a slot.  The optimized recursive frame is now 72 bytes, an 85.2% reduction.
This is transient stack traffic only: there is no per-node, per-clause or
per-index allocation.

Strict clocks-off reversed gates against the previous accepted portable
binary gave:

| Josef 01 gate | Parent mean total | Bitset mean total | Change | Mean RSS change |
|---|---:|---:|---:|---:|
| 301 givens | 14.755 s | 14.480 s | **-1.86%** | +36 KiB |
| 1,001 givens | 44.200 s | 44.235 s | +0.08% | +140 KiB |
| 1,501 givens | 75.705 s | 74.485 s | **-1.61%** | +142 KiB |

At 1,501 givens the candidate won both placements: 74.14 versus 76.24
seconds, then 74.83 versus 75.17 after reversing core and launch order.  Mean
user CPU fell from 71.770 to 70.600 seconds (-1.63%) and mean system CPU from
3.935 to 3.885 seconds (-1.27%).  All four outputs had the same digest for
every selected-given line and exact final
`(Given=1501, Generated=3603947, Kept=521972, proofs=0)` rule, index, hint,
passive, ancestor and allocator counters.  Every process used zero swap.

Cross-workload checks also remained exact.  CHAT/301 used 25.23 total CPU
seconds versus 25.54 for its adjacent parent and 464,680 versus 464,640 KiB
RSS.  Josef 02/601 with the code-tree unit strategy used 35.93 versus 36.00
total CPU seconds and 468,816 versus 468,760 KiB RSS.  These single placements
are classified as non-regressions, not independent speedup claims.  The
focused variable-64 regression forces a repeated-variable edge to fail after
an upper-word binding and verifies that its next sibling succeeds;
`compact_unit_index_test`, the long-run compaction/rebase test, the packed
generalization smoke and ASan/UBSan all pass.  No Prover9 option changes are
required.  The measured portable binary is SHA-256
`639513c2cf019436460cd41556136e82ce20cef147960a126c3cfbdd8efa389a`.

### Iterative compact-unit generalization traversal

Commit `7cb381b` removes the recursion that remained after the binding-bitset
change.  The predecessor's `generalization_rec()` was still the largest
scalable self-time entry in the fresh profile: 2.96 sampled seconds, or 7.94%
of profiled self CPU, at the exact 1,001-given Josef 01 endpoint.  Its 72-byte
optimized call frame was much smaller than the original 488-byte frame, but
every visited radix edge still paid for a C call, return and compiler-managed
frame.

The replacement is an explicit depth-first traversal with reusable 32-byte
frames.  A frame contains the radix node, flattened-query position, next
sibling, wanted rigid code and the exact two-word set of bindings introduced
by its incoming edge.  The parent advances to its next sibling before a
descent; popping restores only that edge's bindings.  Consequently variable
siblings, equal rigid siblings, terminal postings, excluded IDs and
repeated-variable failures are visited in exactly the same order as before.
The first live non-excluded proof ID therefore remains authoritative.

The stack grows from the flattened query length, so the implementation does
not replace recursion with a fixed-depth limit.  Its capacity is included in
the unit index's scratch and total byte statistics and is released both by
normal destruction and predecessor teardown during compaction.  The initial
64 frames cost 2 KiB.  Josef 01 needed 128 frames of capacity, or 4 KiB, by
the 1,001/1,501-given gates.  The complete index object, including its new
pointer and capacity fields, was only 4,112 bytes larger at 1,501 givens; this
does not scale with clauses, nodes or postings.

Strict clocks-off serial reversed gates against the accepted binding-bitset
binary gave:

| Josef 01 gate | Recursive mean total | Iterative mean total | Change | Mean RSS change |
|---|---:|---:|---:|---:|
| 301 givens | 14.760 s | 13.240 s | **-10.30%** | +1,706 KiB |
| 1,001 givens | 42.365 s | 42.155 s | **-0.50%** | -2,698 KiB |
| 1,501 givens | 77.145 s | 76.290 s | **-1.11%** | -7,128 KiB |

The 301 result is dominated by placement/system noise and is not used as a
mature projection.  At 1,501 givens, mean user CPU fell from 74.260 to 73.265
seconds (-1.34%); mean system CPU moved from 2.885 to 3.025 seconds.  All four
processes ended at
`(Given=1501, Generated=3603947, Kept=521972, proofs=0)`, shared selected-given
SHA-256 `41cae549c3acae5751600ee9fe9a7ec0f109c14882b0030679f8034e258e6310`,
and had exact rule, hint, passive, ancestor, allocator and compact-index work
counters.  Every process used zero swap.

The cross-workload gates did not expose a repeatable cost.  CHAT/301 reversed
means improved from 25.750 to 25.360 total CPU seconds (-1.51%) at exact
`(301,120793,5737,0)` endpoints and identical compact-unit profiles.  The
first serial Josef 02/601 pair was unfavorable (29.390 versus 28.830 seconds),
but its placements contradicted each other.  A concurrent, core-balanced
tie-break favored the candidate (31.110 versus 31.755 seconds).  Across all
four measurements per binary the candidate and control were effectively tied
at 30.250 and 30.293 seconds (-0.14%).  Every Josef 02 process ended at
`(601,524799,26671,0)`, had the same generalization profile and used zero
swap.  This is classified as a non-regression, not as a Josef 02 speedup.

The focused unit regression now constructs a depth-80 radix path, forcing the
stack beyond its initial 64-frame allocation.  Existing tests retain sign,
exclusion, posting order, repeated-variable, variable-64, retirement,
compaction and high logical-base/rebase coverage.  The focused unit test,
ASan/UBSan, the 10,000-record compact long-run stress test and the compact
generalization matrix all pass.  No Prover9 option changes are required.  The
measured portable binary is SHA-256
`55e42911d55d2f27174bf88259e9ec39d2cb603ba48fcaf55516dbbfb555a831`.

A new isolated `-pg` build at the documented head reached the exact
1,001-given endpoint and selected-given digest in 67.19 user and 2.60 system
seconds, 602,520 KiB peak RSS and zero swap.  Profiler overhead again makes
those process totals non-release measurements.  The iterative public
generalization routine remained the largest scalable self entry, but fell
from the predecessor's 2.96 sampled seconds (7.94%) to 2.58 seconds (6.90%).
The next entries were one-time packed-hint posting construction and scalable
unit-conflict retrieval: `posting_dense_add()` used 2.01 seconds and
`collect_code_tree_candidates()` 1.96 seconds.  The raw output, GNU-time
record and profile are `/tmp/josef01-iterative-gprof-1000.{out,time,txt}`;
their SHA-256 values are respectively `2b5c0280...`, `1bdc06f8...` and
`689bbf1d...`.

## Authoritative completed runs

The source files are:

- `/project/bob/Josef_01.out.old`, SHA-256
  `770ea6fe4bee3adfe0b82a2f5fd9938cb981b1ff32f8c04e3bdcd5fbc0b15955`;
- `/project/bob/Josef_01.out.new3`, SHA-256
  `3448aab96ec6fb4bd1571edf0a3e043f4f3194aa285e09be8adfbe08da61e974`.

| Measurement | old P9 | completed compact baseline | compact / old |
|---|---:|---:|---:|
| Given | 30,827 | 30,827 | exact |
| Generated | 1,602,769,536 | 1,602,769,536 | exact |
| Kept | 36,195,388 | 36,195,388 | exact |
| Hints matched | 48,968 | 48,968 | exact |
| User CPU | 19,064.46 s | 28,163.94 s | 1.477 |
| System CPU | 288.00 s | 8,532.45 s | 29.63 |
| Total CPU | 19,352.46 s | 36,696.39 s | 1.896 |
| Wall clock | 19,356 s | 36,725 s | 1.897 |
| Old internal memory / compact PSS | 45,258.48 MiB | 9,083,561 KiB | about 19.6% |

The last row compares the best available old-P9 internal peak-style memory
number with the compact process PSS, so it is not a same-instrument metric.
It nevertheless agrees with the observed practical result: the compact
process fits in roughly 9 GiB rather than roughly 45 GiB.  The next comparison
should record GNU-time RSS and `/proc/$pid/smaps_rollup` PSS for both binaries
on the same host.

The proof text and endpoint tuple are identical.  The compact baseline is not
a divergent search that happens to find the theorem later; it is the same
OTTER trajectory implemented with different storage and indexes.

## What consumed the compact baseline CPU

The completed output reports these top-level clocks:

| Clock | Seconds |
|---|---:|
| preprocess | 20,814.10 |
| infer | 4,736.38 |
| conflict | 4,337.29 |
| hints | 3,945.86 |
| weigh | 1,903.30 |
| subsume | 967.48 |
| index | 103.13 |

Some search clocks are nested, so this table must not be summed.  It locates
hot paths; total process CPU remains the authoritative total.

Three scaling failures stand out.

1. The compact allocator handled 148.9 billion allocation calls and 4.43 TB
   of cumulative object traffic.  It returned 11,942,479 empty 256-KiB slabs,
   causing 3.13 TB of cumulative `munmap` traffic.  This is the strongest
   explanation for the extra 8,244 seconds of system CPU over old P9.
2. The unit index issued 55.3 million forward-generalization and 71.2 million
   conflict queries.  It spent 745.868 sampled seconds in generalization and
   4,310.748 sampled seconds in unification, visiting 68.17 billion conflict
   tree nodes.  The mature tree averaged about 958 visited nodes per conflict
   query even though 95.8% of the queries returned no candidate.
3. Packed hint matching issued 1.60 billion ordinary and 490 million flipped
   queries.  The sampled timers report 3,331.879 and 922.614 seconds.  The
   2-MiB result cache hit only 8.15%; the conjunction profiles rejected
   235.74 billion posting candidates, so this is already a filtered hot path,
   not a missing elementary index.

An updated `-pg` profile of the accepted source at the exact 1,001-given
endpoint separates current steady-state costs from older code.  The profiled
process used 75.92 user and 2.61 system seconds, 619,328 KiB peak RSS and zero
swap; profiler overhead means those totals are not release benchmarks.
`generalization_rec()` remained the largest search-time self entry at 3.12
sampled seconds (7.69%), followed by `slab_get()` at 2.09 seconds (5.15%).
The larger one-time entries were construction of the 153,681-hint packed
index, not work which repeats with every given clause.  Temporary exact
counters at 301 givens then found 2,093,293 sibling reads behind only 923,880
previously reported generalization work units.  The route-cache experiment
below tested whether those unreported reads were profitably avoidable.

A fresh profile after the accepted binding-bitset change at `b3d19f3`
reproduced the exact 1,001-given endpoint in 68.00 user and 2.80 system
seconds, 611,964 KiB peak RSS and zero swap.  Profiler overhead again makes
those process totals unsuitable as release comparisons.  The ranking remains
stable: `generalization_rec()` is still the largest scalable self entry at
2.96 sampled seconds (7.94%), followed by `slab_get()` at 1.69 seconds
(4.53%).  Construction of the fixed 153,681-hint index occupies several of
the other leading entries, whereas paramodulation construction and clause
processing are the principal work that scales toward the proof endpoint.

The RAM itself is dominated by necessary long-lived compact state rather than
ordinary live clauses:

| Component | Approximate bytes at proof |
|---|---:|
| compact term pool | 3.84 GB |
| compact unit index | 3.79 GB |
| compact nonunit index | 0.16 GB |
| live allocator objects | 0.066 GB |
| process PSS | 9,083,561 KiB (8.66 GiB) |

The dense passive directory, selector runs, and ancestor store are
file-backed.  Their logical/physical files are much larger than their process
PSS and must be accounted separately when comparing RAM with disk/page-cache
use.

## Changes carried into `josef01-cpu-next`

### Instrumentation and adaptive escape

Commit `dc93531` splits unit-tree fanout into query-variable, pending-subtree,
rigid-child, and rigid-sibling sources.  It changes no answers.  This makes a
mature run capable of distinguishing unavoidable variable expansion from a
bad rigid lookup.

Commit `8ced379` added the original opt-in
`compact_unit_strategy=adaptive`.  Its complete position postings are
delta-compressed in 256-byte chunks: the 1,000-given Josef feature store fell
from 58,965,408 to 9,151,600 bytes, an 84.5% reduction.  That original,
unlimited-depth route cut tree visits but was slower at 1,000 and 2,000 givens
because it admitted more exact candidates.  It remains rejected for the full
run.  The later direct refinement and depth-2 admission policy are materially
different and form the CPU-first candidate described below.

### Packed-term access

Commit `b81b9cc` resolves the packed token and logical bases once per public
unit-index query.  It removes repeated public resolver/bounds-check calls from
each radix edge while keeping the exact matcher authoritative.  On an exact
1,000-given replay, sampled generalization time fell from 4.695 to 4.123
seconds and unification from 3.520 to 3.089 seconds, about 12% in both local
operations.  The trajectory and memory were unchanged.

### Bounded slab recycling

Commit `67a6c29` keeps at most 256 detached 256-KiB mappings in a cross-size
class pool.  The hard ceiling is 64 MiB.  Memory-pressure checks and
`memory_release_unused()` purge the pool, so this does not restore old P9's
unbounded append-only behavior.

At 2,000 Josef givens, the old reclamation policy had unmapped 1,861 slabs.
The bounded recycler had unmapped only the initial 141, reused 1,469 mappings,
and held 251 cached mappings.  It did **not** improve total CPU at that short
prefix; initialization and I/O still dominate there.  Its purpose is the
day-scale regime represented by the baseline's 11.94 million unmaps.  The
full run must show whether avoided kernel calls convert into the expected
system-CPU reduction.

### Constant-time symbol metadata

Commit `0af0650` maintains a growable direct `symnum -> Symbol *` table while
retaining the historical hashes for name lookup and iteration.  The pointers
refer to authoritative mutable records, so precedence, type, KB weight, and
other updates remain visible.

Two serial pinned 1,000-given controls averaged 82.14 CPU seconds; two
candidates averaged 78.90, a 3.95% reduction.  All 1,000 selected clauses and
the endpoint `Generated=1,628,048 / Kept=320,239` were identical.  Mean peak
RSS differed by about 1 KiB.  The new 4,096-symbol regression exercises table
growth and mutable metadata semantics.

### Ordered forward-generalization pruning

Commit `9b06885` uses the radix tree's existing sibling ordering during
forward generalization.  A rigid target can match only stored-variable edges
and the one equal rigid edge; incompatible rigid siblings are skipped or end
the scan.  The exact matcher still enforces repeated-variable consistency.

At 1,000 givens, work fell from 41,802,863 to 10,594,120 (-74.7%) and the
sampled generalization timer from a paired mean of 4.424 to 2.741 seconds
(-38.1%).  Whole-process CPU fell from 81.63 to 78.83 seconds (-3.44%).  At
2,000 givens, generalization work fell from 112,056,339 to 28,653,322 (-74.4%)
and its sampled timer from 12.552 to 8.468 seconds (-32.5%).  The selected
clauses and endpoint tuples remained exact.

### Direct two-position unit-conflict refinement

Commit `45ecaac` repairs the principal weakness of the experimental
`compact_unit_strategy=adaptive` route.  The original route selected the
rarest rigid query position, decoded the union of its exact-symbol and
ancestor-variable postings, and sent every surviving record to the exact
unifier.  At the mature completed endpoint, unit conflict retrieval had
visited 68.168 billion code-tree nodes for 71.180 million queries, about 958
nodes/query, while 95.8% of queries returned no candidate.  A route which can
avoid that traversal without inflating exact work is therefore a plausible
long-run CPU escape.

The accepted refinement chooses a second rigid query position, but it does
**not** read a second posting list.  It decodes only the rarest first-position
union.  For each record in that union it walks the compact prefix term to the
second position and requires either the same rigid symbol or a stored variable
at that position or one of its ancestors.  This is a necessary condition for
unification, so it can reject records but cannot reject a unifier.  The full
unifier remains authoritative, and final proof IDs retain their historical
descending order.  A zero-count first-position union is now an immediate
negative answer, avoiding empty bucket and query-stamp work.

The statistics line exposes the mechanism directly:

```text
position_refinement_queries=...
position_refinement_checks=...
position_refinement_rejects=...
position_tertiary_queries=...
position_tertiary_checks=...
position_tertiary_rejects=...
```

At the exact Josef 01 601-given endpoint, the refined route performed 4,183
two-position queries and rejected 15,505 of 23,960 first-union records before
unification (64.7%).  Conflict exact tests fell from 36,232 in the prior
single-position adaptive control to 20,727 (-42.8%).  Unlike the rejected
two-posting prototype, posting scans stayed at roughly the single-position
level: 23,960 rather than 71,706.

Pinned, clocks-off, 2-GiB bounded gates preserved the exact search endpoints
and used zero process swap:

| Josef 01 gate | Refined adaptive | `code_tree` | Observation |
|---|---:|---:|---|
| 601 given, CPU | 22.47 and 24.81 s | 24.13 s | adaptive mean 23.64 s; effectively parity (-2.0%) |
| 601 given, peak RSS | 541,496 and 542,832 KiB | 534,704 KiB | about 7--8 MiB extra |
| 1,001 given, CPU | 48.68 s | 49.40 s | effectively parity (-1.5%) |
| 1,001 given, peak RSS | 619,444 KiB | 612,400 KiB | about 7 MiB extra |
| 1,001 given, code-tree nodes | 25,697,573 | 35,045,361 | -26.7% |

The timings are too close and too few to claim a bounded speedup.  The work
counters are the reason to retain the experiment: its avoided code-tree work
grows with the mature index, while its direct checks are bounded by the
rarest posting union.  A structurally different CHAT/601 smoke also preserved
`(Generated=497430, Kept=16974)` and used 41.11 CPU seconds, versus 43.25 in a
single adjacent control; only one query there exercised two-position
refinement, while 31,437 zero-count choices exercised the fast-negative path.

The focused regression constructs two first-position near misses and checks
that the direct second feature rejects exactly one without losing the exact
answer.  `compact_unit_index_test`, `compact_long_run_test`, an adaptive `x2`
audit, and `compact_generalization_smoke_test.sh` all pass.  These are bounded
semantic and scalability gates, not a proof-endpoint CPU result.

This unlimited-depth form does not replace `code_tree` as an authority
configuration.  Adaptive must maintain compressed position features in
addition to the code tree.  The 2,000-given unlimited store used 21,988,160
feature bytes for 695,604 active units, about 31.6 bytes/unit.  A linear
extrapolation to the completed 35,592,170-unit population is approximately
1.1 GB, moving the measured compact PSS toward roughly 9.7 GiB and reducing
the estimated saving versus old P9 to about 77--78%.  The depth-bounded design
below is the accepted selective alternative; do not run adaptive accidentally
with its compatibility depth zero.

### Depth-bounded adaptive unit features

Commit `ff9ee62` supplies the selective admission step without making the
index approximate.  The new option

```prolog
assign(compact_unit_feature_depth,2).
```

causes `position` and `adaptive` to store and query only rigid/variable
position features at term depths 1 and 2.  Zero is the compatibility default
and retains the previous unlimited-depth store.  A shallow feature union is
still a complete necessary-condition filter because every resident record is
indexed at every admitted position; queries that do not profit from that
filter retain the complete code-tree route and final exact unification.  The
configured coverage is captured by each index and survives compaction, rather
than being reread from mutable process-global state.

The first bounded gates exposed a separate general adaptive-policy defect.
An empty posting union was always used immediately, even when the code tree
could reject a CHAT-like query in one or two nodes.  Adaptive routing now
measures that tree route once and uses an empty union only when the measured
tree traversal exceeds eight nodes.  Cheap routes continue through the tree
and are remeasured as it grows; expensive Josef wildcard routes still switch
to the complete empty-position answer.  The focused regression includes a
fanout query whose first call measures the costly tree and whose second call
uses the learned empty union with the same negative answer.

All clocks-off, pinned gates below used 2-GiB limits, stopped at the stated
given count, preserved the exact generated/kept endpoint and reported zero
process swap:

| Gate | Strategy | Total CPU | Unit feature capacity | Conflict retrieval work |
|---|---|---:|---:|---:|
| Josef 01 / 1,001 | `code_tree` | 55.79 s | 0 | 35.05M tree nodes + 0.20M exact tests |
| Josef 01 / 1,001 | adaptive, depth 1 | 58.44 s | 0.76 MB | 2.03M tree + 2.95M postings + 0.62M exact |
| Josef 01 / 1,001 | adaptive, depth 2 | 56.47 s | 2.33 MB | 2.89M tree + 1.55M postings + 0.51M exact |
| CHAT / 601 | `code_tree` | 48.19 s | 0 | 41,710 tree nodes |
| CHAT / 601 | adaptive, depth 2 | 48.47 s | 0.13 MB | 32,485 tree nodes + 15 postings |
| Josef 02 / 601 | `code_tree` | 37.43 s | 0 | 70,122 tree nodes |
| Josef 02 / 601 | adaptive, depth 2 | 37.55 s | 0.20 MB | identical 70,122 tree nodes; no position route selected |
| Josef 01 / 1,501 | `code_tree` | 84.40 s paired mean | 0 | 73.93M tree nodes + 0.30M exact tests |
| Josef 01 / 1,501 | adaptive, depth 2 | 82.19 s paired mean | 3.64 MB | 4.72M tree + 3.05M postings + 0.98M exact |

At 1,001 givens the CPU differences are noise-scale.  The stricter 1,501 gate
is the first observed crossover: adaptive won both core orientations, 79.31
versus 82.18 seconds and 85.07 versus 86.62, for a 2.6% paired-mean saving.
Both strategies ended at `(Given=1501, Generated=3603947, Kept=521972)` with
the same hint/search counters and zero process swap.  Mean RSS rose from
675,562 to 693,762 KiB (+18.2 MiB).

More important than the small elapsed win is the diverging work slope.  At
1,001 givens, depth 2 replaces roughly 35 million code-tree nodes with about
5 million combined tree/posting/exact items.  By 1,501, code-tree work has
grown to 73.93 million nodes, while adaptive performs 4.72 million tree nodes,
3.05 million posting checks and 0.98 million exact tests.  At the completed
endpoint the old code tree reached 68.17 **billion** nodes, so this is the only
currently measured mechanism with enough scaling leverage to remove a
material part of its 4,311 sampled conflict seconds.

At 601 givens depth 2 reduced feature capacity by 79.7% versus unlimited
adaptive (767,744 versus 3,777,032 bytes).  The 1,501 store uses 3,638,272
feature-capacity bytes for 490,940 physical units, or 7.41 bytes/unit.  A naive
linear extrapolation to the completed 35,592,170-unit population is about
264 MB (0.25 GiB), versus the earlier 1.1-GB unlimited-depth projection.
Allocator capacity, posting popularity and touched PSS can change at mature
scale, so only a full measurement is authoritative.

Depth-2 adaptive is therefore now the **CPU-first full authority candidate**,
not merely a post-run experiment.  `code_tree` remains the strict memory
control and should be retained if the approximately quarter-GiB projection is
unacceptable.  Accept adaptive only if the exact proof endpoint, hint
trajectory and proof are unchanged and the interval CPU/given slope improves
without compromising the radical RAM saving.  Depth 1 saves more RAM but
scans many more common postings and is not the recommended long-run policy.

### Third direct unit-conflict condition

Commit `107665b` extends direct refinement by one more rigid query position.
It still decodes only the rarest first-position union.  After a record passes
the existing second-position condition, the code checks a third distinct
position directly in the same compact prefix term before invoking the exact
unifier.  A stored variable at that position or any ancestor passes, so this
is another complete necessary-condition filter rather than an approximation.
No third posting union is decoded, proof-ID order is unchanged, and the only
new persistent capacity observed at the 1,501-given gate was a 256-byte path
scratch buffer.

In a clocks-off, 2-GiB Josef 01/1,501 reversed pair, the new condition won
both core orientations:

| Refinement | Pair totals | Mean CPU | Conflict exact tests | Peak RSS mean |
|---|---:|---:|---:|---:|
| two positions | 82.33 / 81.11 s | 81.72 s | 958,845 | 689,964 KiB |
| three positions | 81.83 / 80.31 s | 81.07 s | 452,851 | 689,694 KiB |

Thus the whole-prefix gain is modest (-0.8%), but exact-unifier calls fall by
505,994 (-52.8%).  The third condition ran for 398,526 queries, checked
680,261 survivors and rejected 505,994 of them (74.4%).  Every run ended at
`(Given=1501, Generated=3603947, Kept=521972)` with identical hint, routing,
posting and selected-search counters and zero swap.  A separate traced
100-given candidate/control gate produced the same 100 selected-given lines,
with SHA-256
`2eb5d7ef1674e54e8c25e6031e230072d971ec26fcc7aec291271de69ed362d3`.

The cross-workload gates found no general regression.  CHAT/601 preserved
`(Generated=497430, Kept=16974)` and averaged 41.07 seconds candidate versus
41.45 control across reversed cores; only 11 queries selected a third feature
and no posting survivor reached that check.  Josef 02/601 preserved
`(Generated=524799, Kept=26671)` and never selected the position route at all.
Its candidate/control timing difference is therefore optimized-binary layout
or host-frequency noise, not an algorithmic claim.  The focused test covers a
two-position near miss rejected only by the third condition, and the current
binary passes `compact_unit_index_test`, `hint_preview_test`, and the full
bounded compact-generalization smoke.

### Posting-wide packed-hint rejection

The completed Josef output identifies a separate mature hot path.  Its packed
conjunction index received 1,921,904,699 cache-miss queries and reported
235,862,987,833 posting candidates; profile masks and literal counts then
rejected 235,740,740,444 of them.  These candidate counts are abstract posting
references--the implementation scans them in 64-reference bit-plane
blocks--but the scale and rejection ratio show that reading profile sidecars
is substantial work even though almost no hint reaches exact subsumption.

Commit `3f2f566` stores three conservative summaries for every profile
posting: the OR of all 64-bit feature masks and the independent maximum
positive and negative literal counts.  If a query requires a feature absent
from the OR, or a literal count above either maximum, the entire posting is
impossible.  The per-block planes and counts are then not touched.  This is a
necessary-condition shortcut: overflow hints are still processed, surviving
IDs retain their original order, and authoritative exact matching is
unchanged.  The new cumulative counters are:

```text
summary_reject_queries=...
summary_reject_candidates=...
```

The initial representation added eight bytes to every hash-table slot and
therefore raised Josef's conjunction plan by 4 MiB.  That implementation was
not retained.  Commit `4c0d0b2` overlays profile summaries with the dense
bitset pointer and word-count fields used only by ordinary postings.  Posting
kind is checked in dense construction, destruction and statistics.  The final
representation restores Josef's exact control footprints:
`table_bytes=46137424`, `estimated_bytes=314579336` and
`peak_bytes=314579336`.  It therefore neither consumes extra index RAM nor
causes a near-budget conjunction index to be declined earlier.

At the exact Josef 01/1,501 endpoint, the shortcut rejected 2,221,209 of
4,270,806 posting views (52.0%) before their sidecars, covering 37,287,356 of
342,117,152 posting references (10.9%).  A clocks-off, 2-GiB concurrent
reversed-core pair produced:

| Binary | Pair user CPU | Mean user | Mean total CPU | Mean RSS |
|---|---:|---:|---:|---:|
| previous source | 80.42 / 79.82 s | 80.12 s | 84.67 s | 703,196 KiB |
| posting summary | 78.49 / 77.90 s | **78.20 s** | **82.80 s** | 703,464 KiB |
| change | -2.4% / -2.4% | **-2.4%** | **-2.2%** | +268 KiB |

All four runs ended at
`(Given=1501, Generated=3603947, Kept=521972, proofs=0)` with identical hint,
cache, unit-index, selector and search-work counters; every process reported
zero swap.  The 268-KiB RSS difference is measurement noise rather than an
allocated sidecar: both the planner and runtime table bytes are exact.

The generalization gates also preserved every endpoint and work counter.
CHAT/601 rejected 53,832 posting views covering 440,413 references and used
41.72 versus 42.67 total CPU seconds in one pinned orientation.  Josef 02/601
declined the conjunction index in both binaries at exactly 107,115 plan scans
and `estimated_bytes=335548256`; its inactive path used 34.01 versus 34.31
seconds.  These single-orientation held-out timings are compatibility evidence,
not standalone speedup claims.

Applying Josef's bounded 10.9% candidate ratio to the completed count would
suggest roughly 25.7 billion reference-equivalents whose sidecars might be
avoided.  That is a workload projection, not a mature measurement: posting
sizes and query profiles can shift substantially.  The full authority run
must use the new counters to report interval and final selectivity directly.

`hint_postings_test`, `hint_preview_test`, `compact_unit_index_test`, and the
full compact-generalization smoke pass with the final overlaid layout.

### Sparse hint planes and reused generalization edge heads

Commits `f13e552` and `3eb7c2b` remove two high-frequency pieces of redundant
work without changing an index, route, or answer.

`hint_postings_add_profile()` previously tested all 64 feature-mask bits for
every profile posting, even though clause feature masks are sparse.  The
Josef 01/1,000 gprof run attributed about 5.5% of sampled self CPU to roughly
8.1 million calls of this function.  It now iterates only the set bits with
`ctz` and `mask &= mask - 1`; the same bit planes and flags are populated in
the same posting order.

Forward generalization already reads the first packed radix-edge token to
choose a stored-variable edge or the equal rigid edge.  The exact edge matcher
then resolved and compared that same rigid token again.  It now receives the
classified head: equal rigid heads consume the known-equal query/token pair
and matching resumes at token one, while variable heads retain the existing
binding and repeated-variable logic.  This adds no state or RAM.

Pinned, clocks-off, 2-GiB gates preserved exact endpoints and every reported
search/index work counter.  In serial reversed Josef 01 pairs, the combined
candidate averaged 23.97 versus 25.79 CPU seconds at 600 givens (-7.1%) and
49.86 versus 51.22 at 1,000 (-2.6%).  Pair orientations disagreed at the
1,000-given gate, so the latter is bounded evidence, not a robust standalone
speedup claim.  An adjacent CHAT/600 pair favored the candidate 44.07 versus
45.05 seconds (-2.2%).

One combined Josef 02 orientation produced a 39.26-second candidate outlier;
the other was 34.01 versus its 33.80-second control.  The changes were then
isolated in separate binaries: hint-only and generalization-only each used
33.14 CPU seconds at the exact Josef 02/600 endpoint, versus recent controls
of 32.94 and 33.80 seconds.  Together with identical work counters, this
supports treating the 39-second sample as host/code-layout noise rather than
added logical work; the report does not use the outlier-skewed combined mean
as evidence of a gain.

`hint_postings_test`, `compact_unit_index_test`, `compact_long_run_test`, the
adaptive `x2` audit, and `compact_generalization_smoke_test.sh` pass on the
combined source.  Peak RSS was unchanged within measurement noise and every
measured process reported zero swap.

### Reproducible production CHAT smoke

Commit `de55328` adds the `new_otter_compact_file_production` case to
`test.src/chat_test_matrix.sh`.  Existing historical case names keep their
old policies.  On `/project/bob/chat_test.in` (SHA-256 `9781ee...`), the new
case stopped intentionally at 300 givens with
`Generated=120,793 / Kept=5,737`, exactly matching the archived endpoint.  It
used 33.51 user seconds and 462,852 KiB peak RSS.  This is a semantic smoke,
not a mature CPU result; `adaptive32` has visible short-prefix construction
cost.

## Rejected or non-default experiments

These results are important because they prevent tuning a large run with
plausible-looking options that were already negative.

- A direct terminal-posting fast path for iterative generalization was
  implemented and removed.  When a radix edge consumed the complete query,
  it scanned the edge's postings immediately instead of pushing and revisiting
  a terminal 32-byte DFS frame.  This preserved every answer and work counter,
  and all four 301-given outputs shared selected-given SHA-256 `5b709a38...`
  at `(Given=301, Generated=141040, Kept=38770, proofs=0)`.  Nevertheless it
  lost both clocks-off placements: candidate totals were 13.08 and 14.33
  seconds versus 12.73 and 13.34 for the controls.  Candidate/control means
  were 13.705/13.035 seconds, a 5.1% regression; RSS was effectively equal
  and every process used zero swap.  The larger branch/layout cost inside the
  now-inlined hot traversal outweighed the removed frame operations.  Longer
  gates were intentionally skipped, and source plus portable executable were
  restored byte-for-byte to SHA-256 `55e42911...`.
- Converting unit-conflict code-tree recursion to an explicit DFS was also
  implemented and removed.  The prototype shared a 32-byte union with the
  accepted generalization stack, retained pending-subtree, query-variable and
  ordered rigid-sibling routes exactly, and added no per-node or per-clause
  allocation.  A depth-80 broad-variable regression forced the new stack past
  its initial 64 frames.  The first implementation mistakenly called the
  grow/check helper at every expandable node: it looked 4.7% faster at 301
  givens, then lost both 1,001-given placements and regressed from 41.125 to
  42.555 mean total CPU seconds (+3.5%).  Hoisting that helper behind the
  actual capacity boundary removed the accidental call tax.  The corrected
  form remained inconclusive at 301 and lost at both mature gates: 41.330
  versus 40.875 seconds at 1,001 (+1.1%), then 73.030 versus 72.170 at 1,501
  (+1.2%).  At 1,501, mean user CPU was 70.275 versus 69.570 seconds and mean
  RSS was effectively equal at 672,294 versus 672,352 KiB.  All four mature
  outputs shared selected-given SHA-256 `41cae549...`, endpoint
  `(1501,3603947,521972,0)`, every tree-node/sibling/posting/exact-test count,
  and zero swap.  Explicit state traffic was costlier than compiler-managed
  recursion even as the tree grew.  Source, focused tests and executable were
  restored byte-for-byte to accepted SHA-256 `55e42911...`.
- An internal-edge pending-subtree summary was likewise rejected.  While a
  resident query variable covers a serialized stored subterm, the code tree
  updates an open-slot count for every packed token.  The prototype cached
  each internal radix edge's signed final slot delta and greatest prefix
  deficit in the two posting words that are structurally unused by internal
  nodes.  It could then skip the complete token walk whenever the incoming
  slot count proved the covered subterm could not end in that edge.  Split
  invalidation and terminal-posting guards made the overlay exact, and it
  added zero node bytes.  Lazy summary construction did not amortize: the
  301-given mean was already 13.880 versus 13.460 seconds (+3.1%, placements
  disagreed), and at 1,001 givens the candidate lost both placements, 44.045
  versus 42.285 seconds (+4.2%).  Mean RSS unexpectedly rose from 599,026 to
  608,230 KiB despite unchanged allocated index bytes, demonstrating another
  optimized-layout/page-touch effect.  Every 1,001-given output shared digest
  `d2c195ff...`, endpoint `(1001,1628048,320239,0)`, exact unifier profiles
  and zero swap.  The summary, guards and split handling were removed and the
  accepted executable restored byte-for-byte to `55e42911...`.
- The pre-`45ecaac` single-position, unlimited-depth
  `compact_unit_strategy=adaptive` reduced tree work but was about 8.5% slower
  at 2,000 givens and produced more exact tests.  Do not infer from the later
  depth-2 result that this old form is now acceptable: the CPU-first authority
  candidate requires both direct refinement and
  `compact_unit_feature_depth=2`.
- A first attempt at two-position filtering marked the first posting union
  and decoded a complete second union to intersect it.  At 1,000 givens it
  reduced conflict exact tests from 293,410 in the compressed adaptive
  baseline to 230,055, but raised position postings from 111,551 to 424,572.
  It took 56.33 CPU seconds versus 49.40 for `code_tree` (+14.0%).  This
  implementation was replaced completely by direct compact-term checking;
  no second posting union is decoded in commit `45ecaac`.
- Bypassing route learning for a complete one-posting union was rejected.
  At 600 givens, serial reversed pairs were effectively neutral: the refined
  control averaged 26.40 CPU seconds and cold admission 26.49 (+0.4%).  At
  the more informative 1,000-given gate, cold admission took 56.83 CPU
  seconds versus 52.05 for the adjacent control (+9.2%).  It reduced
  code-tree nodes from 25,697,573 to 22,759,380 (-11.4%), but raised position
  postings from 111,867 to 171,424 (+53.2%) and exact tests slightly from
  230,055 to 230,494.  Even one hash/posting decode plus a compact path walk
  costs more than dozens of contiguous radix nodes on this path; cold-score
  thresholds 1 and 4 were removed completely.
- Raising `CUI_ADAPTIVE_POSITION_FACTOR` from 12 to 24 was also rejected in
  a 600-given serial reversed pair.  Factor 24 reduced direct checks from
  23,960 to 8,045, but raised tree nodes from 3,074,870 to 3,364,559.  Its
  paired mean was 26.54 CPU seconds versus 23.79 for factor 12 (+11.6%), and
  it lost in both orientations.  The current learned factor 12 is therefore
  retained; larger factors merely exchange cheap contiguous tree work for
  too little avoided position work.
- Extending the resident feature sidecar from depth 2 to depth 3 was rejected
  at the exact 1,501-given endpoint.  It reduced position postings from
  2,988,159 to 1,438,042 (-51.9%) and exact tests from 958,845 to 750,933
  (-21.7%), but the adaptive policy selected the tree more often and tree
  nodes rose from 4,675,184 to 9,299,799.  In a reversed pair, depth 3
  averaged 92.76 CPU seconds versus 91.78 for depth 2 (+1.1%), lost one
  orientation, and raised mean RSS from 689,928 to 700,232 KiB.  All endpoints
  and search counters were exact and swap was zero.  Better-looking candidate
  counts do not justify the extra feature coverage or its altered route mix;
  keep `compact_unit_feature_depth=2`.
- A proposed cost gate for the third direct condition was also removed.  It
  skipped 67,098 third-feature selections while losing only 1,310 rejections,
  which looked favorable as a work count, but its reversed mean was 84.05 CPU
  seconds versus 83.43 for the unconditional third check (+0.7%), and it lost
  both core orientations.  The extra branch/counter and resulting optimized
  layout outweighed the avoided short query traversals.  The production code
  therefore has no hidden small-union threshold.
- Selecting the second and third rigid refinements in one top-two traversal
  was neutral and was reverted.  It reproduced every candidate, rejection,
  exact-test and search counter, but its reversed Josef 01/1,501 mean was
  90.60 CPU seconds versus 90.47 for the two simple minimum scans (+0.1%);
  the core orientations disagreed.  The nominally smaller traversal count did
  not repay a larger branch-heavy recursive selector, especially because the
  simple scans stop early after zero-score features.  The current source and
  portable binary were restored byte-for-byte to the committed two-pass form.
- A 64-MiB packed-hint result cache raised its hit rate only from 7.42% to
  9.03%, used about 63 MiB more RSS, and increased the 1,000-given CPU result
  from the current paired mean of 78.83 to 82.11 seconds.  Do not add
  `assign(hint_cache_kb,65536)`.
- Per-64-reference conjunction block summaries were implemented and removed.
  They stored a feature-mask OR and literal-count maxima for each bit-plane
  block, after the accepted posting-wide test.  At Josef 01/1,501 they rejected
  1,772,012 of 5,235,594 residual blocks (33.8%), covering 94,264,901
  reference-equivalents; the sidecar planned 5,850,752 bytes.  An early pair
  looked favorable, demonstrating why work counters alone are not an
  acceptance test.  After making summaries optional so a near-budget base
  conjunction index could never be displaced, and hoisting that policy branch
  outside the hot block loop, the final strict pair lost both orientations:
  user CPU was 88.23 versus 87.46 seconds and 79.71 versus 78.06 seconds.
  Candidate/control means were 83.97/82.76 user seconds (+1.5%) and
  89.59/88.33 total seconds (+1.4%); all endpoints and pre-existing counters
  were exact and swap was zero.  Commits `e373304` and `1a46d09` record the
  prototype and budget-safe design; `45b01f4` removes them completely and
  restores the accepted portable binary byte-for-byte.  Do not infer CPU from
  the impressive block-rejection total or add a similar sidecar without new
  mature evidence.
- Sharing the packed-fast query's key mixing across cache lookup, conjunction
  lookup and cache store was also rejected.  It computes exactly the same two
  hashes and preserves all cache slots, posting keys and candidates; the
  completed run's roughly 1.92 billion conjunction misses made the duplicate
  arithmetic look like an unusually strong memory-neutral target.  A first
  implementation regressed even at 301 givens because it introduced hot-path
  stack arguments and a pointer result.  A second returned both hashes in
  registers and kept the conjunction call within the six x86-64 argument
  registers.  Nevertheless, the exact Josef 01/1,501 reversed gate lost both
  placements: user CPU was 82.74 versus 80.36 seconds and 77.81 versus 76.68
  seconds.  Mean user/total CPU regressed 2.2%/2.3%, and optimized-layout page
  touching raised measured RSS by about 27 MiB despite unchanged allocated
  index bytes.  All cache, conjunction, hint and search counters were exact;
  process swap was zero.  The prototype was removed without a source commit,
  and the accepted binary was restored byte-for-byte.  Recomputing a compact
  hash can be cheaper than extending live ranges and perturbing this already
  large hot function.
- A direct unit-to-unit paramodulant constructor was implemented and removed.
  It passed the target literal already known by the eager and bounded
  traversals into construction, bypassed two one-element sibling-list walks,
  and built the sole result literal without `append_literal()`.  This was an
  exact specialization of the generic path, and a focused regression checked
  both the expected `P(a)` result and agreement with the independent
  `para_pos2()` implementation.  The completed run's 1.600 billion
  paramodulants made the call site look important, but strict whole-binary
  gates were negative.  At 301 givens, candidate totals were 15.60 and 15.81
  seconds versus 15.31 and 15.85 for the controls: one win, one loss, and a
  +0.8% mean regression.  At the exact 1,501-given gate the candidate lost
  both placements.  Candidate/control mean user CPU was 72.785/72.330 seconds
  (+0.63%); system CPU was 4.020/4.025 seconds; and total CPU was
  76.805/76.355 seconds (+0.59%).  Every process ended at
  `(Given=1501, Generated=3603947, Kept=521972, proofs=0)`, with identical
  rule-generation counts, normalized final operation counters and allocator
  state, effectively identical RSS, and zero swap.  The prototype and its
  temporary test were reverted completely, and the accepted portable binary
  was restored byte-for-byte to SHA-256
  `4a32437f5ff84e98946ae1309b130d9d7b9b574dbd949ca76521d275be03ff92`.
  Removing a source-level list walk can still worsen optimized hot-code layout;
  do not reintroduce this specialization without new mature evidence.
- Guarding attribute inheritance calls against null parent lists was also
  measured and rejected.  Temporary counters at the exact 301-given endpoint
  observed 284,891 `inheritable_att_instances()` calls: 267,473 (93.9%) had a
  null input, while the other 17,418 visited nodes were noninheritable labels
  and copied nothing.  The release prototype therefore called the existing
  routine only for non-null parent lists.  A focused paramodulation regression
  verified substitution into an inheritable term attribute and exclusion of a
  noninheritable string attribute.  Despite the high measured reach, the
  clocks-off reversed gate lost both placements: 15.19 versus 14.96 seconds,
  then 14.96 versus 14.38.  Candidate/control mean user CPU was
  12.545/12.125 seconds (+3.46%), system CPU was 2.530/2.545 seconds, and total
  CPU was 15.075/14.670 seconds (+2.76%).  All runs ended at
  `(Given=301, Generated=141040, Kept=38770, proofs=0)` with identical
  rule-generation counts, allocator traffic and normalized operation
  statistics; mean RSS differed by only 24 KiB and process swap was zero.
  The 1,501-given gate was intentionally skipped after this two-placement
  early failure.  Instrumentation, guards and the temporary test were removed,
  restoring the accepted binary to SHA-256
  `4a32437f5ff84e98946ae1309b130d9d7b9b574dbd949ca76521d275be03ff92`.
- A bounded rigid-sibling route cache for forward unit generalization was
  implemented and removed.  The 1.5-MiB direct-mapped design stored the first
  sibling at or above a requested rigid symbol.  A separate bounded parent
  generation table made every hit exact across concurrent radix insertions
  and splits; table collisions caused misses, and a focused regression covered
  cached negative reuse, insertion of the formerly missing symbol, positive
  reuse and splitting of a cached path.  At 301 givens it obtained 317,561
  validated hits and reduced sibling scans from 2,093,293 to 1,329,978
  (-36.5%), but the reversed total was neutral: 15.285 seconds candidate
  versus 15.310 control (-0.16%), with one win and one loss.  At 1,001 givens
  it obtained 3,325,759 hits but lost both placements: 46.74 versus 45.05
  seconds, then 48.62 versus 46.84.  Candidate/control means were
  44.310/42.595 user seconds (+4.03%), 3.370/3.350 system seconds, and
  47.680/45.945 total seconds (+3.78%).  Mean peak RSS rose from 601,374 to
  614,970 KiB, much more than the logical cache alone.  Every run preserved
  `(Given=1001, Generated=1628048, Kept=320239, proofs=0)`, rule-generation
  counts and allocator traffic, and used zero swap.  Hashing and exact
  mutation validation cost more than the pointer walk they avoided.  The
  implementation, telemetry and temporary tests were removed completely;
  the accepted binary is again SHA-256
  `4a32437f5ff84e98946ae1309b130d9d7b9b574dbd949ca76521d275be03ff92`.
- Removing the slab allocator's redundant `free_count` writes was also
  rejected.  The count is exactly `next_unused - live_count`, so the prototype
  derived reusable-space reports from those existing fields and removed one
  metadata write from every free-list allocation and every free.  Allocator
  lifecycle, 300,000-object churn and bookkeeping tests passed, and every
  reported allocator byte/counter remained exact.  The noisy 301-given gate
  favored the candidate in both placements (18.585 versus 19.140 mean total
  seconds), but the 1,001-given gate did not confirm it.  Candidate/control
  means were 45.630/45.605 user seconds, 3.985/3.820 system seconds, and
  49.615/49.425 total seconds (+0.38%); one placement won and the reverse
  lost.  More importantly, mean peak RSS reproducibly rose from 601,250 to
  618,488 KiB despite identical logical allocator state.  All processes ended
  at `(Given=1001, Generated=1628048, Kept=320239, proofs=0)` with identical
  rule-generation counts and allocator traffic, and zero swap.  The two hot
  writes were therefore not removed: their whole-binary layout effect was
  neutral for CPU and adverse for observed residency.  Source and binary were
  restored byte-for-byte to the accepted SHA-256
  `4a32437f5ff84e98946ae1309b130d9d7b9b574dbd949ca76521d275be03ff92`.
- Caching one immutable 32-bit first symbol beside every compact-unit radix
  node was implemented and removed.  It replaced the packed-term-pool load in
  both insertion sibling comparisons and query sibling scans; radix splits
  updated the old child explicitly, compaction rebuilt the sidecar, and the
  rebase path needed no update because symbol codes are immutable.  The
  focused unit-index and long-run compaction/rebase tests passed, as did an
  AddressSanitizer/UndefinedBehaviorSanitizer run.  Nevertheless, the exact
  301-given reversed gate lost both placements: candidate/control totals were
  15.24/15.13 seconds and 15.02/14.68 seconds.  Candidate/control means were
  12.245/12.095 user seconds, 2.885/2.810 system seconds, and
  15.130/14.905 total seconds (+1.51%).  Mean peak RSS rose from 512,020 to
  512,800 KiB.  Every run preserved
  `(Given=301, Generated=141040, Kept=38770, proofs=0)`, all pre-existing
  compact-unit counters, and zero swap.  Exact accounting reported 255,132
  sidecar bytes at this prefix.  The completed-run node capacity projects the
  same design to 257,610,840 bytes (about 246 MiB), consuming valuable margin
  around the 80% RAM-reduction target while slowing the short gate.  The
  1,001/1,501 gates were intentionally skipped after the two-placement loss;
  source and executable were restored byte-for-byte to accepted SHA-256
  `4a32437f5ff84e98946ae1309b130d9d7b9b574dbd949ca76521d275be03ff92`.
- Reusing the compact-unit node's posting-tail space for an inline first radix
  code was also implemented and removed.  To retain ordered O(1) insertion
  without increasing the 24-byte node, the prototype represented each exact
  terminal's postings as a circular list with its tail in the node.  Only
  three posting iterators required circular termination; a focused regression
  verified insertion order, exclusion, retirement and forced compaction, and
  the full compact-unit, long-run, ASan and UBSan tests passed.  This avoided
  both the packed-term first-code load and the rejected separate sidecar's
  projected 246 MiB allocation, but it still lost the strict 301-given gate.
  Candidate/control totals were 16.10/15.67 seconds and 15.33/15.37 seconds
  after reversing core and launch order.  Candidate/control means were
  12.780/12.675 user seconds, 2.935/2.845 system seconds, and 15.715/15.520
  total seconds (+1.26%), with one win and one loss.  Mean peak RSS rose from
  512,048 to 512,936 KiB despite identical allocated index bytes.  Every run
  preserved `(Given=301, Generated=141040, Kept=38770, proofs=0)`, all
  compact-unit counters and zero swap.  The longer gates were intentionally
  skipped; source and portable executable were restored byte-for-byte to
  accepted SHA-256
  `639513c2cf019436460cd41556136e82ce20cef147960a126c3cfbdd8efa389a`.
- A narrower internal-node first-code overlay was likewise rejected after it
  exposed a short-to-mature crossover.  Prefix term encodings are prefix-free,
  so a radix node with children cannot also be an exact-term terminal.  The
  prototype overlaid the otherwise unused internal-node posting-tail word
  with the first radix code while leaving every terminal posting list and all
  three posting iterators unchanged.  Temporary exact counters at 301 givens
  showed that 1,985,475 of 2,481,516 first-code reads (80.0%) targeted such
  internal nodes.  The node remained exactly 24 bytes and all allocated index
  byte totals were unchanged.  Focused unit-index, long-run compaction/rebase,
  ASan and UBSan tests passed.  Reversed means initially improved from 15.135
  to 14.915 total seconds at 301 givens (-1.45%) and from 42.900 to 42.595 at
  1,001 givens (-0.71%), winning all four placements.  The exact 1,501-given
  gate reversed that result decisively: candidate/control totals were
  71.21/70.61 and 81.88/80.26 seconds.  Candidate/control means were
  72.795/71.730 user seconds, 3.750/3.705 system seconds, and 76.545/75.435
  total seconds (+1.47%); the candidate lost both placements.  Mean RSS was
  effectively identical at 673,860 versus 673,852 KiB and process swap was
  zero.  All four outputs shared the selected-given digest `41cae549...` and
  exact final rule, index, hint, passive, ancestor and allocator counters at
  `(Given=1501, Generated=3603947, Kept=521972, proofs=0)`.  This is direct
  evidence that a representation can win every short gate yet lose as the
  index matures; source and executable were restored byte-for-byte to accepted
  SHA-256
  `639513c2cf019436460cd41556136e82ce20cef147960a126c3cfbdd8efa389a`.
- A negative Bloom summary was slower and was removed completely.
- A query-scoped binding trail preserved all answers but raised the sampled
  generalization timer from roughly 2.72--2.76 to 2.920 seconds.  It was
  reverted completely.
- Moving the 256-clause CPU-report cadence from `possible_report()` into its
  caller removed the residual per-generated function call and separated
  exact given-count reports.  In a clocks-off, cache-off reversed Josef
  01/1000 pair, mean system CPU fell from 2.25 to 2.08 seconds, but mean total
  CPU rose from 49.13 to 49.50 seconds (+0.8%) and the pair orientations
  disagreed.  Every given digest remained exact; the dispatch split was
  nevertheless reverted completely because the release compiler already
  makes the residual call negligible.
- A suspected long-run statistics scan was also audited and rejected as a
  target before merging extra bookkeeping.  `update_memory_stats()` scans
  `clause_store_length(Glob.disabled)`, but compact OTTER's dense passives are
  detached archive records and are not members of that retained-handle
  array.  At the completed endpoint the ancestor store reports 36,195,194
  cumulative records and 36,195,190 detached records: only four records ever
  used ordinary handles.  Its 1,053,512 handle bytes are the fixed 1-MiB
  write buffer, the 4-KiB I/O buffer, the 328-byte store, and a 64-pointer
  allocation.  Thus each report scans four entries, not 36 million.  A
  synthetic million-handle scan confirmed that such a layout would be
  measurable, but it does not model Josef's detached directory.  The proposed
  resident side list was reverted completely; it would have added lifecycle
  state without improving the production path.
- Requiring a packed-hint profile to appear three times before admitting its
  exact result was rejected at the first 101-given Josef gate.  A bounded
  two-row frequency sketch reduced cache stores from 10,398 to 390, but it
  also lost useful first-repeat results: hits fell from 2,620 to 885 and
  posting candidates avoided from 345,334 to 153,819.  Total CPU increased
  from 13.40 to 14.13 seconds (+5.4%) with the exact same
  `(Given=101, Generated=13675, Kept=7309)` endpoint, approximately equal RSS,
  and zero swap.  The sketch and admission policy were reverted completely.
  This rejects history/frequency admission: it cannot know whether the first
  result was expensive and discarded useful first-repeat entries.  The later
  `hint_cache_min_candidates` control is materially different: it uses exact
  work already measured on the miss, retains 95.1% of avoided candidates at
  the Josef 01/1,001 gate, needs no frequency table, and remains default-off.
- Reusing the first literal's already computed structural mask for the
  packed-fast clause profile was also rejected.  It removes one recursive
  `packed_term_feature_mask` traversal per ordinary query without changing
  any candidate or index state, but release-code layout outweighed that saved
  work.  At the exact 601-given Josef 01 endpoint, a pinned clocks-off reversed
  pair averaged 25.75 CPU seconds candidate versus 24.85 control (+3.6%).  An
  adjacent 1,001-given check took 50.94 seconds candidate versus 49.66 control
  (+2.6%).  All runs preserved `(Generated=560216, Kept=109842)` or
  `(Generated=1628048, Kept=320239)`, every packed-hint counter, approximately
  526/611 MiB peak RSS, and zero process swap.  The source was reverted
  completely; eliminating a source-level recursive call is not sufficient
  evidence when the whole optimized binary is slower at both bounded gates.
- Replacing KBO's temporary linked variable multisets with stack counters was
  rejected even though it substantially reduced allocator traffic.  A first
  direct 100-counter form fell back for the legal high variable numbers that
  occur after standardization: at Josef 01/1,001 it removed only 3.17 million
  of 180.03 million allocation calls and averaged 58.33 CPU seconds versus
  55.93 control (+4.3%).  A sparse `(variable,count)` stack table handled
  high numbers without allocation and removed 21.40 million calls plus 342.4
  MB of cumulative object traffic at the same endpoint.  It looked excellent
  in the shorter adaptive 601-given gate and improved the adaptive 1,001-given
  reversed mean from 54.77 to 52.64 seconds (-3.9%).  That result did not
  generalize to the then-primary `code_tree` authority configuration: its
  reversed 1,001-given mean was 49.31 seconds candidate versus 48.45 control
  (+1.8%).
  Josef 02/601 also moved from 36.18 to 36.91 seconds (+2.0%) despite 7.70
  million fewer allocation calls, while CHAT/601 was neutral at 45.47 versus
  45.30 seconds with 2.45 million fewer calls.  All endpoints, search/index
  counters and allocator live/reserved state were exact; RSS was unchanged
  within layout noise and every process used zero swap.  A focused test also
  covered multiplicities, high variable IDs, and the more-than-`MAX_VARS`
  fallback.  Both implementations and the temporary test were nevertheless
  removed: fewer allocations are useful telemetry, not a CPU win, and the
  production configuration plus a second Josef workload both regressed.
- Avoiding the default clause weighter's unused substitution `Context` was
  also implemented and removed.  With no ordinary weight rules the context
  cannot be read; with rules, failed matches restore their entry trail and
  successful matches are undone, so one clean context can safely serve a
  whole clause.  A custom multi-literal `list(weights)` regression preserved
  its endpoint, and the exact Josef 01/301 reversed pair looked strong at
  14.23 versus 15.44 mean total seconds.  The established 1,501-given gate did
  not confirm it: candidate/control mean user CPU was 70.70/70.32 seconds and
  mean total CPU was 74.99/74.09 seconds (+0.5%/+1.2%); the candidate won the
  first placement but lost the reverse.  Every search endpoint was exact and
  process swap was zero.  The prototype was removed without a source commit,
  and the accepted `b3cde19` binary was restored byte-for-byte.  Do not infer
  mature prover CPU from the removed context count or the short 301-given
  prefix.
- Reusing exact-unifier binding state was rejected after a strict whole-engine
  gate.  The completed Josef 01 run made 50,318,558 conflict exact tests, and
  the compiler-generated prologue clears roughly 5.5 KiB of local binding
  state for each test.  A prototype retained that state per compact unit index
  and reset only two 100-byte bound maps between calls.  It improved a focused
  repeated-variable failure microbenchmark, but the exact Josef 01/1,001
  adaptive-depth-2 reversed pair was neutral to the millisecond: both control
  and candidate averaged 53.425 CPU seconds.  Every run ended at
  `(Given=1001, Generated=1628048, Kept=320239)`, all search/index counters
  matched and process swap was zero.  Even the operation-level result projects
  to only roughly 1--7 seconds over the completed run's 50.3 million tests,
  which is immaterial against about 36,700 CPU seconds.  The prototype and its
  roughly 5-KiB-per-index persistent state were removed completely.
- A native `-O3 -march=native -flto` build used 77.60 CPU seconds in one
  pinned 1,000-given run versus 81.69 for the immediately following portable
  `-O2` control.  Earlier portable pairs averaged 78.83, so host-frequency
  variation is material.  `NATIVE=1` is an optional final-build experiment,
  not part of the algorithmic claim.

## Likely RAM and CPU effect

### RAM

The accepted CPU changes do not materially undo the compact memory result.
The direct symbol table costs one pointer per rounded symbol slot; term-base
caching and sibling pruning add no long-lived search structure.  The slab
pool has a strict 64-MiB ceiling, and changing the selector buffer from 65,536
to 1,048,576 entries adds at most roughly 24 MiB for each selector that
actually grows to the cap.  Only Josef's `TheRest` selector is large enough;
the four-million-record scale gate observed a 34-MiB PSS increase including
the associated mapping/page-cache effects.  Third-position unit refinement
adds only a growable query-path scratch array; it occupied 256 bytes at the
1,501-given gate and adds no per-clause or per-feature storage.
Posting-wide packed-hint summaries are overlaid with mutually exclusive dense
posting fields: measured conjunction table, plan and peak bytes are identical
to the previous source, so this optimization adds no index allocation.
Exact-default clause weighting adds no persistent state beyond one Boolean;
its traversal stack exists only during `clause_weight()`.
The owner-free hint-cache ring remains exactly within the configured 2-MiB
budget; it trades owner metadata and part of the old key arena for twice as
many result slots, with no measurable whole-process RSS change.
Stack-owned paramodulation positions add no persistent search state.  Their
dynamic traversal-stack reservation is unchanged because a smaller frame
array makes room for the coordinate array; the complete current source adds
only four 16-byte fixed coordinates to the transient `para_into_lit()` frame.
The compact-unit generalization binding bitset likewise adds no persistent
state and reduces each optimized recursive frame from 488 to 72 bytes.

The adaptive unit strategy is the exception: it maintains compressed position
features as well as the code tree.  Unlimited depth still projects to about
1.1 GB extra, but the 1,501-given depth-2 policy projects to about 0.25 GiB
from the bounded population.  The strict memory estimate below excludes both.
For the CPU-first depth-2 authority candidate, measure rather than extrapolate
mature PSS; the preliminary projection moves 8.76 GiB to about 9.01 GiB and
retains approximately a 79.6% saving.  Unlimited adaptive would fall to
roughly 77--78% and is not recommended.

Starting from 9,083,561 KiB (8.66 GiB) PSS, charging the complete 64-MiB slab
allowance and the observed 34-MiB selector increment gives roughly 8.76 GiB.
The expected saving against the approximately 45,258-MiB old result therefore
remains about 80.2%, subject to same-host measurement.  File-backed logical
bytes and page cache must be reported separately.

### CPU

CPU estimates are less certain and the gains are not additive:

- disabling exact detailed clocks removes a measured 39.6% of total CPU at
  the exact 1,001-given gate; this corrects a configuration mismatch in the
  completed full-run comparison rather than changing proof search;
- disabling the 2-MiB packed-hint result cache saves about 5% at the short
  Josef 01 prefix but is no longer recommended for the mature run: late
  intervals reached 12--14% hits and avoided hundreds of candidates per hit;
  stable exact keys remove 0.5--1.3% in three bounded workload means, while
  the optional 128-candidate admission threshold removes 88.7% of Josef
  stores and retains 95.1% of its avoided posting work; the owner-free exact
  key ring additionally removes a projected 12.45 billion mature owner-loop
  iterations and improves the Josef 01/1,501 reversed mean by 1.01% at the
  general zero threshold and 0.51% at threshold 128;
- the direct symbol table is a measured 3.95% whole-prefix gain;
- term-base caching saves about 12% only inside unit lookups;
- sibling pruning saves about 38% only inside forward generalization;
- depth-2 direct refinement reduces Josef unit-conflict tree traversal from
  73.93 million to 4.72 million nodes at 1,501 givens (-93.6%), in exchange
  for 3.05 million posting checks and 0.98 million exact tests; it wins the
  reversed-pair total by 2.6% while adding 18.2 MiB RSS, but its full-run gain
  and mature feature-store size still must be measured rather than
  extrapolated;
- the third direct position condition then cuts conflict exact-unifier calls
  from 958,845 to 452,851 (-52.8%) at the same 1,501-given state and wins its
  strict reversed whole-process pair by 0.8%; this is a scaling improvement,
  not a claim that the bounded elapsed change alone is large;
- posting-wide packed-hint summaries skip 52.0% of conjunction posting views
  and 10.9% of their reference-equivalents at Josef 01/1,501 with no added
  index bytes; their strict reversed pair improves mean user/total CPU by
  2.4%/2.2%, while only the full run can establish mature selectivity;
- sparse hint-plane population and first-edge reuse remove measured hot-path
  instructions with no new storage; their combined Josef 01/1,000 paired mean
  improved 2.6%, but frequency noise prevents a tighter full-run estimate;
- the 1-Mi-entry selector buffer cuts scale-probe selector CPU by 34% and
  read/write amplification by 87%/75%; its whole-prover contribution is
  unknown until the authority run;
- compact selector entries then cut the remaining selector buffer, run and
  merge bytes by exactly one third; the 16-million-entry paired proxy improves
  total CPU by 1.9%, while its no-flush Josef prefix is CPU-neutral/noisy;
- 64-byte dense passive directory records reduce that directory's logical and
  reserved size by 11.1%; their scale pair improves total CPU by 4.7% and the
  exact Josef 01/1,501 whole-process pair by 2.0%;
- exact-default clause weighting reduces its directly measured phase by 21.3%
  at Josef 01/1,501; its strict clocks-off reversed whole-process mean improves
  by a much smaller 0.8% user and 0.6% total CPU, with exact independent CHAT
  and Josef 02 trajectories;
- stack-owned eager paramodulation positions remove 42.2% of all `Ilist`
  allocations at Josef 01/1,501 and improve its strict reversed whole-process
  mean by 3.05%; reversed CHAT and Josef 02 means improve by 1.24% and 1.51%,
  while the mature completed telemetry suggests billions of transient path
  allocations are in scope; stack-owning the four fixed coordinates then
  removes another 39.8% of the remaining Josef `Ilist` calls and improves its
  incremental mean by 1.02%, while incremental CHAT and Josef 02 are neutral;
- bitset-owned forward-generalization undo state reduces its optimized
  recursive frame by 85.2% with no persistent allocation; its exact
  Josef 01/1,501 reversed mean improves total CPU by 1.61%, while bounded CHAT
  and Josef 02 remain exact and neutral-to-positive;
- the slab recycler changes almost no bounded CPU but removes nearly all
  post-warm-up slab unmaps by 2,000 givens;
- native compilation may add 0--5% depending on the host.

The earlier 28,000--34,000-second estimate was made before the clock mismatch
was discovered and is superseded.  It charged exact-clock work to the compact
storage/index design.  A precise replacement cannot be obtained by scaling a
1,001-given result across machines: the timer cost is approximately linear in
phase intervals, while unit-tree, hint and selector work grow differently.
The installed clocks-off engine is 2.86 times faster than old P9 at the
bounded exact state, and current source additionally contains the amortized
report polling, slab recycler and mature index changes that were absent from
`new3`.  It is therefore plausible that a clocks-off, freshly built
current-source full run reaches the 24,191-second 1.25-times-old gate, but
**CPU parity has not yet been demonstrated**.  Do not publish a tighter
full-run estimate until that external acceptance run completes.

## Build and run the decisive Josef 01 comparison

### Portable baseline build

From this repository and branch:

```sh
git switch josef01-cpu-next
make -C ladr lib
make -C provers.src prover9
cp -p provers.src/prover9 bin/prover9
sha256sum bin/prover9
```

Do not reuse objects from a differently instrumented, sanitized, profiled, or
`NATIVE=1` build.  If in doubt, use a fresh worktree or clean the build before
compiling.  In particular, the repository's currently installed `bin/prover9`
is intentionally the older PGO executable and does not contain commits
`107665b`, `3f2f566`, `4c0d0b2`, `b3cde19`, `df7bfdb`, `f4f4614`,
`1b15b40`, `b1d8128`, `9fecf54`, or `b3d19f3`; run the build and copy steps
above before the authority run.  It also lacks the iterative generalization
traversal in `7cb381b`.  The fresh portable source binary measured at
`7cb381b` has SHA-256
`55e42911d55d2f27174bf88259e9ec39d2cb603ba48fcaf55516dbbfb555a831`.

### Prover9 options

Place this block before the problem's ordinary search commands.  The Josef 01
input later executes `clear(back_demod).`; retain that command because it is
part of the authoritative old trajectory.  The compact back-demodulation
options are harmless telemetry/configuration while back demodulation is off,
and keeping the common production block makes cross-problem runs comparable.

```prolog
assign(search_loop,otter).
assign(passive_store,dense).
assign(passive_directory,file).
assign(passive_selector_store,file).
assign(passive_selector_buffer,1048576).
assign(hint_index,packed_fast).
assign(inference_frontier,clauses).
assign(ancestor_store,file).
assign(sos_limit,-1).

set(process_initial_sos).
set(back_demod).
set(back_demod_hints).
clear(unit_deletion).
clear(ancestor_subsume).
clear(eval_rewrite).
clear(compress_disabled).

set(compact_otter_demodulation).
set(compact_otter_unit_index).
set(compact_otter_back_demod_index).
set(compact_otter_nonunit_index).

assign(compact_unit_strategy,adaptive).
assign(compact_unit_feature_depth,2).  % Do not omit: zero means unlimited.
set(compact_nonunit_path_filter).

assign(compact_back_demod_strategy,adaptive32).
set(compact_back_sparse_positions).
assign(compact_back_position_budget_kb,0).
assign(compact_back_position_budget_pct,50).
assign(compact_back_position_build_factor,32).
assign(compact_back_eager_position_depth,4).
assign(compact_back_tree_budget_kb,65536).
assign(compact_back_tree_budget_pct,200).
clear(compact_back_edge_filter).

assign(compact_passive_cache,0).
assign(hint_cache_kb,2048).
assign(hint_cache_min_candidates,128).  % Josef 01 staged admission policy.
assign(compact_index_stale_pct,25).
assign(compact_term_reclaim_kb,8192).
assign(compact_rewrite_deep_cache_kb,0).

% Maximum-throughput authority run.  /usr/bin/time -v supplies total CPU.
clear(clocks).
set(hint_match_stats).
assign(stats,all).
assign(report,900).
```

Do not add the 64-MiB hint cache and do not omit the depth-2 limit.  The
adaptive configuration is now the CPU-first authority candidate because its
paired 1,501-given crossover and mature work projection are stronger than the
remaining code-tree case.  A bounded 4,000/10,000-given preflight on the large
host is still advisable before committing a week to it.

The strict memory control changes exactly one line:

```prolog
assign(compact_unit_strategy,code_tree).
```

Keep every other option identical so that code-tree remains a usable bounded
control.  For the adaptive authority run, continue only if the
given/generated/kept/hint trajectory is exact, the interval CPU/given slope
remains no worse than the code-tree preflight, there is no swap, and the ratio
of `position_refinement_rejects` to `position_refinement_checks` remains
substantial.  Also monitor the tertiary reject/check ratio separately; a
collapsing late ratio would turn the third query traversal into pure overhead.
Record feature bytes and PSS explicitly: the CPU-first run is a
measured CPU/RAM tradeoff with a preliminary 79.6% RAM-saving projection, not
a claim that the feature sidecar is free.

If approximate internal phase attribution is needed, replace `clear(clocks)`
with:

```prolog
set(clocks).
assign(clock_sample_rate,16).
```

Do not use exact rate 1 for a throughput comparison unless old P9 is also run
with exact clocks.

### Invocation and temporary files

Use a fast filesystem with ample free space for `TMPDIR`; `/local` is a good
choice on the previously described machine.  For example:

```sh
mkdir -p /local/mptp/prover9-tmp
TMPDIR=/local/mptp/prover9-tmp \
  /usr/bin/time -v ./bin/prover9 \
  < /path/to/Josef_01.compact.in \
  > /path/to/Josef_01.compact.latest.out \
  2> /path/to/Josef_01.compact.latest.time
```

The matrix driver can generate the same candidate with `CHAT_CLOCKS=0`,
`CHAT_COMPACT_UNIT_STRATEGY=adaptive`,
`CHAT_COMPACT_UNIT_FEATURE_DEPTH=2`, `CHAT_HINT_CACHE_KB=2048` and
`CHAT_HINT_CACHE_MIN_CANDIDATES=128`; set them alongside
`CHAT_CASES=new_otter_compact_file_production`.  The production matrix case
retains `code_tree` and feature depth zero when the new unit controls are
unset, preserving historical invocations.  `CHAT_CLOCKS=1` remains the
compatibility default for historical diagnostic matrices.  Unset cache
variables retain the general 2-MiB cache and zero admission threshold.

The file-backed passive/selector/ancestor stores are normally created as
unlinked temporary files.  `/proc/$pid/fd/*` can show them with a `(deleted)`
suffix while the process is alive; the kernel removes them automatically when
the process exits or is killed.  Logical file size, allocated blocks, process
PSS, and filesystem free space are different quantities.

For an optional native comparison, build in a separate clean worktree with
`NATIVE=1`.  Mixing portable and native objects is invalid.  Run it only after
the portable authority result so compilation variance cannot obscure whether
the algorithmic changes worked.

## Acceptance checklist

The new run is accepted only if all of the following hold:

1. `THEOREM PROVED` is present and the endpoint remains
   `Given=30827 / Generated=1602769536 / Kept=36195388`.
2. The hint report remains `matched=48968`, with the same proof and selector
   endpoint.  A mismatch means search divergence and invalidates a pure CPU
   comparison.
3. GNU time reports user, system, elapsed, maximum RSS, major faults, file
   input/output, and swaps.  Swapping invalidates the CPU comparison.
4. Capture `/proc/$pid/smaps_rollup` or cgroup `memory.peak` during the run so
   anonymous PSS and file-backed page cache are not confused.
5. Save the binary SHA-256, commit ID, complete echoed input, and periodic
   `stats=all` reports.
6. Inspect `Memory report` for cached/reused/evicted slabs.  If system CPU is
   still large despite high mapping reuse, the remaining cause is not slab
   churn.
7. Inspect `Compact_unit_fanout` and the adaptive unit query profiles.  Record
   `position_refinement_queries/checks/rejects`,
   `position_tertiary_queries/checks/rejects`, position postings, conflict
   exact tests, code-tree nodes and feature bytes at every report.  Continue
   the adaptive authority run only while its interval CPU/given slope stays
   competitive and refinement rejects a substantial fraction of checks.  A
   collapsing reject rate or rapidly growing posting/exact work means the
   feature sidecar is no longer buying selectivity; stop and use the otherwise
   identical `code_tree` configuration as the strict-memory fallback.
8. Inspect `Dense_passive_selector` flushes, merges and read/write bytes.  The
   entry width must be 16 bytes, and the 1-Mi-entry policy should be far below
   the baseline's 551/546 merge counts; otherwise the wrong binary is running
   or another selector is unexpectedly reaching the cap.
   `Dense_passive` must independently report `directory_entry_bytes=64`.
9. Inspect `Packed_fast_cache` for `min_candidates=128`, admission skips,
   stores and posting candidates avoided.  Compare interval deltas rather than
   only the cumulative hit rate; mature reuse was much more valuable than the
   first 1,001 givens suggested.  The new binary must report 16,384 entries,
   98,304 exact keys and `overlap_invalidations=0` for a 2-MiB cache.  Track
   `arena_wraps` and `arena_expired_misses`; expiration is expected and causes
   an authoritative miss/fallback, not a stale hit.
10. Inspect `Packed_fast_conjunction` for `summary_reject_queries` and
    `summary_reject_candidates`.  Compare interval ratios against conjunction
    queries and posting candidates; a falling ratio is harmless semantically
    but limits the full-run CPU gain projected from the bounded prefix.
11. Preserve every `Generated_by_rule` count.  Capture the periodic `ilist`
    allocation lines as well: their 32-bit totals wrap, so reconstruct each
    interval with modulo-2^32 deltas before comparing cumulative allocation
    work.  The stack-path change should affect allocation traffic, never the
    paramodulation result count or selected-given trajectory.

## Reproducing the bounded CHAT smoke

The exact production case is now one command:

```sh
CHAT_CASES=new_otter_compact_file_production \
CHAT_CPU=0 CHAT_REPORT_SECONDS=30 CHAT_CLOCKS=0 \
./test.src/chat_test_matrix.sh /project/bob/chat_test.in \
  chat-production-300 300 240 2048 300
```

It writes the generated input, raw output, GNU-time report, hashes, status,
summary TSV, and long-run parser output under `chat-production-300/`.

The focused default-weight/fallback regression is:

```sh
make -C test.src default_weight_test
./test.src/default_weight_test
```

The focused hint-cache wrap and packed-index regressions are:

```sh
make -C test.src hint-postings-test
P9_MATRIX_PROVER=./bin/prover9 \
  ./test.src/compact_generalization_smoke_test.sh
```

The exact eager/bounded inference-order and cancellation regression is:

```sh
make -C test.src iterator-tests
```

## If the full run is still slow

The next change should follow the mature telemetry, in this order:

1. If system CPU remains the outlier, separate allocator mapping reuse from
   passive/selector/ancestor file I/O and page-cache eviction.  Increase no
   cache until this attribution is known.
2. If unit retrieval still dominates under depth-2 adaptive, separate its
   position postings, secondary/tertiary refinement checks and rejects, exact
   tests and residual code-tree nodes.  If posting or exact work grows faster
   than the avoided tree traversal, make feature admission more selective; if
   the feature store itself compromises the RAM target, consider a file-backed
   sidecar.
   Do not restore the rejected two-posting intersection or unlimited depth by
   accident.  If adaptive's interval CPU/given slope loses to the bounded
   `code_tree` control, fall back instead of carrying an unproductive sidecar.
3. If packed hints remain near 4,250 seconds, first separate cache hit work,
   admission skips, posting-wide summary rejects and residual conjunction
   candidates from authoritative exact tests.  Stable keys and work-based
   admission address cache overhead, posting summaries avoid proven-impossible
   sidecar reads, and a larger cache has already failed.  Optimize residual
   profile intersections only from their mature interval counters, not from a
   Josef-shaped special case.
4. If selector read amplification dominates, tune run merging from measured
   run counts and bytes, not from the short-prefix 65,536/1,048,576 buffer
   difference.

These are general long-run mechanisms.  None should depend on Josef symbol
names, a singleton term shape, or a fixed hint count.
