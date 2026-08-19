# Josef 02 compact-P9 CPU investigation

Status: source changes implemented and bounded-validated on branch
`josef02-cpu` through `acb1c63`, with the portable PGO workflow fixed through
`0b281d5`.  The current compact prover is already 3.9--5.4 times faster than
the preserved old-P9 binary on exact 300/600-given Josef 02 prefixes.  A full
old-P9 Josef 02 proof output was not supplied, so this report does not claim a
measured proof-to-proof old/new CPU ratio.  The existing compact proof is the
full-run baseline; the next full run is an external acceptance test, not
something attempted on the low-RAM development machine.

## Reproducible inputs and binaries

| artifact | SHA-256 |
|---|---|
| reconstructed uninterrupted Josef 02 input | `8961d86efd2163010117cbba0fd9c6085d3d71751f2d6f2675406f84c57634ba` |
| completed compact output, `/project/bob/Josef_02.out.new3` | `eaf22c2bda54fbe5eb0d487aca91f4ad27dabe1cb656111f97b74fc9930a1d29` |
| preserved old-P9 binary | `bcdf6bafbf608fde463fd43ef541891813f5c49a2d5153711c54925e98d76bcc` |
| accepted parent binary at `10b6abd` | `d13973d3311ba8e31590f139e48f6560ec24af845fc60acb4b2e342dfd001ddc` |
| accepted rewrite parent at `3846b92` | `25fd1849ea8ac898dd45af9ce96397f61465cfdfb7ab8baacda2b1803010bc73` |
| release binary at `06deec9` | `e756487a8ab02a5884b1ac370ca18ceb4de61dda1ea5ae60e929b761ba6a3306` |
| host-native binary at `06deec9` | `7c9cbd5471f92f659a6004a1a9d43f1e24dea8a7702d5ce9e8500fc8c29c10ac` |
| balanced GCC-PGO binary at `06deec9` | `6b1bced78f2ba5c4c36e95e3e0cfa035c7ff7c8c33c2b9d8e69d6fd5302764f0` |
| release binary at `fb7b873` | `7a911af3df69506d395a5c8c034ba472211f0953cfe65d44abfa5d4589a43e6f` |
| release binary at `c40a81d` | `94d16148e6bf3a13170712c54fb09cbb6f1d30cdc846f148667489db722ff52c` |
| release binary at `acb1c63` | `86821508f732be3bc9b8e35cc9e3cc1272eb86817b0938d1852887e15d04f0e4` |

The reconstructed input is
`josef02-debug.heNOIJ/Josef_02-uninterrupted.in`.  It retains every formula,
hint, ordering declaration and inference option from the supplied material;
only the experiment-level compact configuration and bounded limits were made
explicit.

## What the completed run says

The full compact baseline proved Josef 02 at:

| measurement | value |
|---|---:|
| Given / generated / kept | 13,006 / 129,776,312 / 9,226,457 |
| user / system / total CPU | 7,972.54 / 868.76 / 8,841.30 s |
| wall clock | 8,873 s |
| final PSS | 5,001,957 KiB |
| rewrite attempts / rewrites | 3,677,581,851 / 513,972,002 |
| rewrite target nodes | 25,306,441,784 |
| back-demod queries / sampled lookup | 7,440,943 / 957.668 s |
| back mask word checks | 27,753,331,661 |
| compact back-index bytes | 2,722,576,238 |
| allocator calls / object traffic | 16.7 billion / 415.77 GB |

The main clocks were preprocessing 3,739.43 s, demodulation 2,238.59 s,
disable 2,246.41 s, back demodulation 1,022.42 s, indexing 542.61 s, inference
406.87 s, hints 309.07 s and subsumption 284.26 s.  This is not primarily a
clause-selection problem: forward rewriting, clause retirement and the mature
back-demodulation index dominate.

The supplied archive contains no complete old-P9 Josef 02 output.  In
particular, the other Josef 02 files are compact crash/replay artifacts and
must not be labelled an old-P9 baseline.

## Direct old-P9 comparison on bounded prefixes

`test.src/chat_test_matrix.sh` ran one Prover9 process at a time with the same
input and exact endpoint.  The matrix strips compact-only commands before
feeding the preserved old binary.

| max given | old-P9 user | compact user | speedup | old RSS | compact RSS |
|---:|---:|---:|---:|---:|---:|
| 300 | 90.88 s | 16.85 s | 5.39x | 271,744 KiB | 179,744 KiB |
| 600 | 137.36 s | 35.08 s mean | 3.92x | 305,408 KiB | 206,482 KiB mean |

Both comparisons preserve the exact generated/kept state; the 600 endpoint is
`(601, 524799, 26671, 0)`.  The 600 old-P9 number was rerun on the current host
after `3846b92`; the compact number is the mean of two reversed candidate
runs.  The current compact binary is therefore 3.92 times faster and uses
about 32% less RSS at that exact endpoint.  These results establish current
prefix competitiveness, not the missing full old-P9 proof time.

## Accepted changes

### Correct deep-cache initialization (`e2bcb67`)

`compact_rewrite_deep_cache_kb` used to be applied after construction of the
compact rewrite bank.  Inputs echoed a nonzero value while the live bank
silently kept a zero-byte budget.  The option is now applied before bank
construction and the audit checks the requested byte count.  Josef 02 still
recommends zero: this commit makes experiments truthful but does not enable
the cache.

### O(1) compact population reads (`7965f66`, `6c472ca`)

Term-pool lifecycle polling needed only active/physical populations but called
complete back-demodulation statistics, which scan all position features.  A
600-given profile had already made 10,367 such calls; the complete run has
262,803 position features.  Narrow accessors now read the maintained counters
in constant time.

The same treatment was applied to compact rewrite availability and
unit/rewrite physical counts.  At 600 givens this removes roughly 524,846
complete rewrite-stat snapshots and 10,365 unit/rewrite lifecycle snapshots.
The full proof generated 129.8 million clauses and disabled 4.1 million, so
this is a long-run scaling fix even though two reversed 1,000-given timing
pairs were neutral (87.44 versus 87.49 mean user seconds for the first scalar
change).

### Skip mask censuses that cannot affect routing (`b143cc3`)

An eager exact-position query formerly traversed the complete mask directory
before using the position index.  Therefore the full run's 3.48 million
position choices still contributed to all 7.27 million mask queries and 27.75
billion word checks.

The exact `(root, required-mask)` bucket is one member of the compatible-mask
superset.  Its posting count is an O(1) lower bound.  If a complete position
posting is already no larger than that bucket, the full census cannot change
the existing decision and is skipped; otherwise the original census and
comparison run.  Route choice and answer order are unchanged by construction.

| Josef 02 gate | control | candidate | directory effect |
|---|---:|---:|---|
| 1,000 given user | 84.16 s | 88.92 s | 6,772 censuses and 1.14M word checks avoided; short-prefix regression |
| 1,500 given user | 205.79 s | 203.64 s | 24,316 censuses, 8.56M word checks and 2.44M bucket visits avoided |

At 1,500 givens both runs end at
`(1501, 3039735, 176392, 0)` with identical back-demod answer/output
fingerprints.  Candidate and control use about 300 MiB RSS.  The sampled
lookup estimate is 6.229 versus 5.960 s, so the result is a first total-CPU
crossover plus a deterministic work reduction, not proof of a uniform win at
every prefix.

### Snapshot shared-term addressing once per clause (`10b6abd`)

Every rigid-subterm rewrite attempt refreshed the shared compact term pool's
token and logical-address bases through two out-of-line accessors.  The pool
can move while clauses and rules are serialized, but compact normalization
does not mutate it while one clause is being rewritten.  The safe
invalidation boundary is therefore the clause, not every subterm.

The completed baseline made 3,677,581,851 rewrite attempts but processed only
130,519,375 subject atoms.  Moving the two refreshes to clause entry therefore
eliminates at least 7.09 billion redundant accessor calls on that trajectory;
there are no new allocations or indexes.

| gate | parent user | candidate user | CPU change | parent/candidate RSS |
|---|---:|---:|---:|---:|
| Josef 02, 600 given, two reversed pairs | 40.03 s mean | 39.63 s mean | -1.0% | 206.3 / 206.9 MiB mean |
| Josef 02, 1,000 given, adjacent | 91.21 s | 86.42 s | -5.25% | 230,576 / 230,848 KiB |
| CHAT, 600 given, adjacent | 55.27 s | 52.06 s | -5.81% | 467,748 / 467,464 KiB |
| Josef 01, 1,000 given, reverse adjacent | 60.77 s | 60.56 s | -0.35% | 620,808 / 617,848 KiB |

The Josef 02 comparisons preserve `(601, 524799, 26671, 0)` and
`(1001, 1310234, 65416, 0)`, respectively, with identical rewrite counters
and back-demod input, output and answer fingerprints.  CHAT likewise preserves
`(601, 497430, 16974, 0)` and all three fingerprints.  Josef 01 deliberately
clears back demodulation and records zero compact rewrite attempts, so its
neutral reverse pair is the expected independent no-regression result.  An
earlier Josef 01 candidate observation of 68.10 s did not repeat after the
adjacent 60.77 s control and is retained as host-load noise, not discarded
from the interpretation.

### Carry recursive rewrite state through one query context (`3846b92`)

The radix retrieval function formerly passed eleven arguments at every
recursive edge.  On x86-64, five invariant arguments were rebuilt on the
stack for each call.  A clause-local query context now owns the same target,
end pointer, bindings, binding trail, ordering flag and result; recursion
passes only the context pointer, node and current subject position.  Binding
creation, undo order, radix traversal and first-success order are unchanged.

This is a representation-only hot-path change: it adds no persistent memory,
cache or tuning option.  In the release object, `retrieve_rec` shrinks from
3,517 to 3,219 bytes (8.5%) and `find_rewrite` from 807 to 677 bytes (16.1%).

| gate | parent user | candidate user | CPU change | parent/candidate RSS |
|---|---:|---:|---:|---:|
| Josef 02, 600 given, two reversed pairs | 39.81 s mean | 35.08 s mean | -11.87% | 206,446 / 206,482 KiB mean |
| Josef 02, 1,000 given, adjacent | 79.55 s | 74.58 s | -6.25% | 230,848 / 230,908 KiB |
| CHAT, 600 given, two reversed pairs | 48.59 s mean | 48.87 s mean | +0.58% | 467,422 / 467,502 KiB mean |

Both Josef endpoints and CHAT preserve generated/kept counts, rewrite
attempts, and the back-demod input, output and answer fingerprints.  CHAT is
properly classified as neutral: its 45--52 s host spread is much larger than
the 0.28 s mean difference.  The accepted result is therefore a strong Josef
02 improvement without a demonstrated cross-workload regression, not a claim
that every rewrite population benefits equally.

### Batch packed-hint intersection counters (`92009ad`)

An isolated `gprof` build of the accepted rewrite code reached the exact
600-given endpoint in 66.96 profiled user seconds.  Its leading self-time was
recursive rewrite retrieval at 13.4%, followed by packed dense hint
intersection at 7.0% and slab allocation at 4.4%.  Dense hint intersection
had already visited 7.84 million summary words and 64.13 million data words.

The dense and sparse loops formerly updated global diagnostic counters for
every visited word, posting candidate, feature test, rejection and result.
Those counters are observational: candidate admission and order do not read
them.  They are now accumulated locally and published once per query.  At the
exact 1,000-given Josef endpoint this batches 20,009,856 summary-word,
161,660,929 data-word, 38,765,984 result and the associated posting-candidate
increments.  The compiled `fast_dense_collect_candidates` body is 9.5%
smaller and no persistent memory is added.

| gate | parent user | candidate user | CPU change | result |
|---|---:|---:|---:|---|
| Josef 02, 600 given, two reversed pairs | 44.30 s mean | 42.43 s mean | -4.21% | exact |
| Josef 02, 1,000 given, adjacent | 91.95 s | 91.82 s | -0.14% | exact/neutral |
| CHAT, 600 given, adjacent | 52.46 s | 52.29 s | -0.32% | exact/neutral |

All final packed-dense counters, packed-hint operation counters, generated and
kept counts, and back-demod input/output/answer fingerprints match.  Periodic
reports can occur at different given counts when host speed differs, so only
the final cumulative reports are equality authorities.

A second isolated profile of the accepted `92009ad` binary, stored in
`josef02-gprof-hint-600/`, reproduced the exact 600-given endpoint and every
final packed-dense counter.  Host load made its absolute profiled user time
84.46 s, so percentages rather than elapsed time are the useful comparison:
`retrieve_rec` accounted for 14.93% of samples, the batched dense intersection
6.13%, and `slab_get` 5.14% across 165,390,617 calls.  The earlier profile had
dense intersection at 7.00%; this confirms the intended local work reduction
without pretending that two separate `gprof` runs are a controlled timing
pair.

### Reverse monotone candidate vectors without sorting (`06deec9`)

Packed hint matching must inspect candidates in decreasing stable-ID order to
preserve legacy `back_subsume` tie-breaking.  Dense bit intersections and
append-only posting vectors normally produce unique IDs in increasing order,
but every query previously sent that already ordered vector through libc
`qsort`.  The candidate builder now records whether admitted IDs remain
nondecreasing.  A monotone vector is reversed in linear time; any mixed-order
vector uses the unchanged comparison-sort fallback.  Candidate membership and
the final decreasing order are therefore identical in both paths.

| gate | parent user | candidate user | CPU change | parent/candidate RSS |
|---|---:|---:|---:|---:|
| Josef 02, 600 given, two reversed pairs | 42.71 s mean | 40.88 s mean | -4.28% | 206,452 / 206,724 KiB mean |
| Josef 02, 1,000 given, adjacent | 96.59 s | 91.50 s | -5.27% | 235,136 / 235,284 KiB |
| CHAT, 600 given, adjacent | 56.44 s | 54.20 s | -3.97% | 467,588 / 466,924 KiB |

Josef 02 preserves `(601, 524799, 26671, 0)` and
`(1001, 1310234, 65416, 0)`; CHAT preserves
`(601, 497430, 16974, 0)`.  All final packed-dense counters match and no run
swapped.  The focused regression covers both the monotone reversal and a
materialize/re-index sequence whose conservative postings require the
mixed-order fallback.

### Post-change profile and optimized production build

An isolated `gprof` build of the accepted `06deec9` source reproduced the
exact 600-given endpoint and final packed counters.  It used 70.99 profiled
user seconds.  The most important self-time samples were recursive compact
rewrite retrieval (13.32%), packed dense intersection (6.82%), slab lookup
(4.36%), variable setup (2.49%), back-demodulation of hints (2.29%),
instance-tree candidate collection (2.26%), packed clause candidate
collection (2.23%) and varint term decoding (2.07%).  At this endpoint the
hint back-demodulation path had already materialized 1,433,225 candidates
after 3,426,140 unique-candidate visits and rejected 1,992,915 by fingerprint.
This profile explains why isolated micro-optimizations can move total CPU in
either direction: the remaining time is spread across retrieval, dense
filtering, allocation, decoding and hint maintenance rather than one dominant
loop.

The supported optimized build was then tested without changing search code or
input options.  The controlled candidate used `-O3 -flto`; the ordinary
release control used `-O2`.  Timings below are total CPU (user plus system),
which matters because the file-backed stores make system CPU material.

| gate | release total CPU | `-O3 -flto` total CPU | change | release/candidate RSS |
|---|---:|---:|---:|---:|
| Josef 02, 600 given, two reversed pairs | 47.28 s mean | 44.74 s mean | -5.37% | 206,454 / 206,862 KiB |
| Josef 02, 1,000 given, adjacent | 110.66 s | 97.30 s | -12.08% | 235,136 / 235,536 KiB |
| CHAT, 600 given, reversed order | 62.82 s | 61.33 s | -2.37% | 467,592 / 467,988 KiB |

All three endpoints and all normalized final search, rewrite, packed-hint,
allocator and fingerprint counters match.  No run swapped.  A separate build
using the repository's supported `NATIVE=1` mode adds `-march=native`; it was
neutral against otherwise identical `-O3 -flto` at Josef 02/600 (49.47 versus
49.36 total CPU in one adjacent pair).  Therefore the demonstrated benefit is
attributed to `-O3` plus link-time optimization, not to CPU-specific
instructions.  `NATIVE=1` remains the recommended interface because its build
mode sentinel prevents accidental mixing with release objects.

### Balanced profile-guided production build (`0b281d5`)

A GCC 13 PGO generator was trained sequentially on three bounded, exact
prefixes: Josef 02/600 (back demodulation and rewritten hints), CHAT/600 (a
different hint population whose conjunction index is admitted), and Josef
01/1,000 (compact unit indexing with back demodulation cleared).  The training
runs reached `(601,524799,26671,0)`, `(601,497430,16974,0)` and
`(1001,1628048,320239,0)`, respectively.  Their peaks were 208,688, 469,712
and 602,840 KiB and none swapped.  This profile is intentionally broader than
a Josef 02-only profile, while remaining small enough to regenerate on the
target host.

| PGO artifact | SHA-256 |
|---|---|
| Josef 02/600 training input | `2d9f1fff2134d619131d5568dfc95c5931ddfd7b134ca578f559ed6754ece2ab` |
| CHAT/600 training input | `161a40bdad2efef4970aa31eda6e0c8739358ecacc2b5fd59af7580184cc0c18` |
| Josef 01/1,000 training input | `ce01c17554300ca5747919dcb5220dae8586257a4d18b333b49d972be5c480a6` |
| CHAT/800 extended gate input | `94b8e43edfbd513605491ab77d0b29d4f5494009d77682b611345ac7ba14368d` |

GCC found profiles for all Prover9 search, compact-index, hint, rewrite and
allocator objects.  Its three missing-profile warnings were confined to
unexecuted pair-index, random-term and TSTP-reader utilities.  The PGO-use
binary preserved every normalized final search and index counter:

| gate | native total CPU | balanced PGO total CPU | change | native/PGO RSS |
|---|---:|---:|---:|---:|
| Josef 02, 600 given, control then PGO | 44.25 s | 38.88 s | -12.14% | 206,940 / 206,536 KiB |
| Josef 02, 1,000 given, PGO then control | 105.52 s | 86.48 s | -18.04% | 235,508 / 235,296 KiB |
| CHAT, 800 given, two reversed pairs | 70.91 s mean | 68.75 s mean | -3.05% | 469,614 / 469,154 KiB mean |

The CHAT pair must be classified as neutral: its PGO observations were 65.43
and 72.06 seconds, a spread larger than the 2.16-second mean advantage, and
the reverse pair mildly favored native.  It nevertheless supplies an
extended-endpoint exactness and no-regression gate.  The defensible result is
therefore a repeatable 12--18% bounded Josef 02 gain from a multi-workload
profile, with no demonstrated CPU or RAM penalty on CHAT.  It is not evidence
that every problem gains 12--18%, nor that a profile trained only through
given 1,000 predicts the mature given-13,006 phase exactly.

### Mature exact mask-directory result cache (`fb7b873`)

The completed proof exposes a later scaling problem which a 600-given profile
cannot represent.  Its mask directory performs 7,271,896 queries, examines
7,352,224,411 64-bucket blocks and applies 27,753,331,661 bit-plane word
checks: about 1,011 blocks and 3,816 word checks per query.  The sampled
back-index lookup estimate is 957.668 seconds and the enclosing back-demod
clock is 1,022.42 seconds.  Re-enumerating the same compatible mask buckets is
therefore material at proof scale even though it is cheap in early prefixes.

The accepted implementation caches the exact ordered compatible-bucket vector
for `(root symbol, required mask)`.  It preserves semantics in four ways:

- entries are admitted only after four observations and only after that root
  reaches 64 directory blocks (4,096 distinct mask buckets);
- existing bucket populations are read afresh on every hit, because posting
  lists continue to grow;
- directories are append-only between rebuilds, so a hit scans just the suffix
  appended since its saved cursor and appends compatible buckets in the
  original order; and
- compaction discards cache values and rebuilds them from the authoritative
  directory while preserving only cumulative diagnostics.

The metadata table has 2,048 two-way entries, stale entries lose replacement
priority after 16,384 eligible queries, and result vectors have a hard 16 MiB
budget.  Allocation is lazy.  Maximum cache storage is about 16.1 MiB plus one
byte per symbol-root; prefixes below the maturity gate allocate no cache at
all.  Cache helpers are deliberately out of line: allowing GCC to inline them
grew the hot `prepare_mask_directory` routine from 792 to 2,389 bytes and
caused a real unrelated CHAT slowdown.  The accepted form is 911 bytes and
restores the dormant-path result.

Validation is deliberately qualified:

| gate | cache behavior | deterministic effect | total CPU / RSS |
|---|---|---|---:|
| final source, Josef 02/600 | 12,687 bypasses, 0 cache bytes | exact control directory work and fingerprints | 45.30 s / 206,608 KiB |
| accepted-layout prototype, CHAT/600 | 7,343 bypasses, 0 cache bytes | exact control directory work and fingerprints | 61.67 s / 467,116 KiB |
| accepted active path, Josef 02/1,000 | 5,160 eligible, 415 hits, 164,288 bytes | removes 35,612 block scans and 139,485 word checks | 119.26 s / 235,880 KiB |

The adjacent final 1,000-given control took 115.82 seconds, so this table does
**not** establish a bounded CPU speedup; sampled lookup itself was essentially
equal (2.393 versus 2.362 seconds) and whole-run timings on this host vary by
several percent.  Earlier gated observations also moved in the opposite
direction.  The defensible claim is narrower: the cache is exact, bounded,
dormant on short/different workloads, and removes measured directory work once
a root matures.  Only the external full proof can establish its CPU payoff on
the 7.35-billion-block workload.  The planning ranges below are therefore not
lowered.  Any PGO profile made before `fb7b873` must be discarded and retrained
because this changes compact-index control flow.

### Constant-time packed hint-owner restoration (`34c790a`, `c40a81d`)

File-backed passive and ancestor records retain a matching hint by stable
hint ID.  Materializing such a record previously restored the pointer by
walking `Glob.hints` from its first entry.  Josef 02 owns 148,330 hints, so
this made a logically constant metadata restoration linear in the complete
hint population.  Packed hint matching already maintains the stable
`Packed_hint_by_id` owner vector, including owners which have expired from
active indexes because archived clauses can still reference them.

The restoration helper now reads that existing vector first and retains the
old clist walk only as the nonpacked/missing-owner fallback.  It allocates no
new table and does not reactivate expired hints.  The final
`Hint_id_lookup:` statistic reports queries, packed hits, fallback list steps
and `packed_id_sum`.  For the initial Josef 02 ordering the last value is a
conservative lower bound on eliminated clist visits because active IDs were
assigned in input-list order.  The helper is explicitly out of line so LTO or
PGO cannot duplicate the rare path into its eleven archive/materialization
call sites; this annotation leaves the ordinary release binary unchanged.

| gate | restoration work | total CPU / RSS | interpretation |
|---|---|---:|---|
| Josef 02/600 | 1,486 packed hits, zero fallback steps | 46.34 s / 206,572 KiB | inside 45.30--46.84 s bracketing controls |
| Josef 02/1,000 | 4,173 packed hits, zero fallback steps; `packed_id_sum=245545806` | 94.87 s / 235,856 KiB | 1.37% below adjacent 96.19 s / 235,952 KiB control |
| CHAT/600 | 1,856 packed hits, zero fallback steps; `packed_id_sum=38073727` | 60.21 s / 467,792 KiB reverse observation | 1.07% above adjacent 59.57 s control; no CHAT speedup claimed |

All gates preserve their exact generated/kept endpoints and compact
input/output/answer fingerprints, and none swapped.  An earlier CHAT
candidate before the same control took 62.94 seconds, illustrating the
host's several-second spread rather than supplying positive timing evidence.
The accepted claim is asymptotic and Josef-specific in magnitude, not a
universal short-prefix gain: at 1,000 Josef givens the removed pointer chasing
is already more than six times larger than on CHAT/600, and the existing
owner table makes the change RAM-neutral.  Any PGO profile made before
`c40a81d` must again be discarded and retrained because the search call graph
has changed.

### Retain clean passive archive records in place (`acb1c63`)

Compact OTTER writes each accepted passive clause to the ancestor store while
the dense selector owns its scheduling metadata.  The old disable path later
released the selected materialization, decoded the same record again, removed
the clause from ID-based compact indexes, and appended an equivalent second
archive record.  This was small at the start of Josef 02 but pathological at
maturity: the completed baseline spent 2,246.41 seconds in `disable`.

Clean passive records now transfer their original stable offset directly into
the retained disabled-store handle array.  Compact literal, rewrite and
back-demodulation records are removed by stable ID, so no clause body is
needed.  A dense-directory dirty bit records every mutable field that can
diverge from the immutable header (`used`, activation/rewrite epochs, delayed
demodulator state and rewrite-rule debt).  A dirty record takes the complete
old materialize/rearchive path.  This is an ownership optimization, not a
search heuristic, and requires no new input option.

A temporary environment gate was used only to compare both paths in the exact
same diagnostic binary
(`8663ccec39dfd9edc481334920bd5435251363e0fd1c58eb4c04281c4fdef46c`).
That gate is absent from the committed production source.  Raw outputs and
GNU-time sidecars are retained in
`josef02-direct-retain-samebin-{on,off}-pair{1,2}-600/`,
`josef02-direct-retain-samebin-{on,off}-1000/`, and
`josef02-direct-retain-final-300/`.

| gate | path-off total / `disable` | retained-path total / `disable` | eliminated work |
|---|---:|---:|---:|
| Josef 02/300, adjacent production binaries | 22.09 / 0.02 s | 20.33 / 0.01 s | 972 records, 1,944 read calls |
| Josef 02/600, two same-binary orientations | 47.44 / 0.245 s mean | 44.68 / 0.100 s mean | 10,257 records, 20,514 read calls |
| Josef 02/1,000, same binary | 103.02 / 1.26 s | 98.15 / 0.86 s | 23,612 records, 47,224 read calls |

Every run preserves its exact generated/kept endpoint and back-query semantic
fingerprint.  The 1,000-given candidate ends at
`(1001, 1310234, 65416, 0)`, reports 23,612 direct retentions and zero dirty
fallbacks, and reduces ancestor records from 89,279 to 65,667.  It also
removes exactly 23,612 compression attempts/materializations, reduces logical
archive bytes from 18,630,914 to 13,636,235, and reduces record reads from
150,684 to 103,460.  Prefix RSS is essentially unchanged because `file` mode
keeps archive bytes outside process RSS; `memory` and `mmap` modes can also
avoid the duplicate backing bytes.

The full baseline itself supplies an unusually strong scale check.  It has
13,337,405 archive records but only 9,222,382 detached records, a difference
of 4,115,023--within 43 records of its 4,115,066 disabled clauses.  Those are
the mature duplicate append operations this change targets.  If the external
trajectory again has zero dirty fallbacks, it should avoid about 4.1 million
record writes/materializations and 8.2 million read calls.  Scaling the
measured 600-given record width gives roughly 0.8 GiB less logical ancestor
file growth and read traffic.  The CPU benefit cannot be asserted until the
full rerun, but unlike a short-prefix matcher tweak this work and the old
`disable` clock are directly present in the completed proof.

The detached-record lifecycle test now covers transfer accounting, mismatched
ID rejection and teardown.  Archive, selector, compact rewrite/unit/back/nonunit,
long-run, hint, compact-OTTER audit/checkpoint, dense-passive and generalization
tests all pass.  PGO profiles made before `acb1c63` must be discarded because
the compact disable call graph changed.

## Rejected options and experiments

- Raising `hint_conjunction_kb` to 384 MiB is rejected.  Rewritten hints grew
  the actual table beyond that cap, disabled it after only 23 queries, and a
  300-given run rose from 16.85 s / 179,744 KiB to 29.77 s / 648,152 KiB.
  Keep the default 320 MiB cap; Josef 02 correctly rejects the all-or-nothing
  table by a few kilobytes.
- A low-threshold deep rewrite-child cache reduced sibling-check counters but
  made the 1,000-given run 10.7% slower.  Keep
  `compact_rewrite_deep_cache_kb=0`.
- Direct clause serialization removed a measured traversal but was neutral in
  reversed 600-given pairs (45.27 control versus 45.45 candidate mean).  Most
  Josef 02 clauses are positive units, so the expected OR/NOT wrapper saving
  was absent.  The experiment was reverted.
- `mask32` is not a CPU replacement for `adaptive32`.  At the exact 1,000
  endpoint it needs 90.65 versus 77.96 user seconds and examines 4.883M versus
  2.281M back-index work units.  It saves about 22 MiB at this prefix, but the
  CPU cost is already 16%.
- Enabling the rigid-edge side index is not justified by the existing CHAT
  and Josef profiles.  Leave `compact_back_edge_filter` clear.
- A bounded generation-aware cache of compatible mask buckets reduced the
  sampled 1,000-given back lookup from 2.542 to 2.069 s, but increased total
  user CPU from 90.47 to 99.26 s and RSS from 228,860 to 239,624 KiB.  Its
  8 MiB directory churn merely moved work into refresh/preprocessing.  The
  experiment was fully reverted; do not trade whole-run CPU for an isolated
  lookup counter.
- Splitting out a zero-deep-cache recursive matcher shrank the common
  `retrieve_rec` body from 3,219 to 1,739 bytes.  Two noisy reversed 600-given
  pairs averaged 39.05 s candidate versus 40.70 s control, but the decisive
  adjacent 1,000-given pair regressed from 79.61 to 83.97 s (+5.5%) with no
  memory benefit.  The extra dispatch and duplicated inlining outweighed the
  smaller recursive body at the longer prefix.  The specialization was fully
  reverted and the accepted release binary hash restored exactly.
- Dispatching a terminal radix child directly to leaf processing avoided a
  redundant recursive end test in principle, but duplicated enough leaf code
  into the inlined child wrapper to worsen two reversed 600-given pairs from
  39.29 to 41.36 s mean (+5.3%).  It was fully reverted.
- Starting dense hint intersection from the posting with the smallest sparse
  reference count was also rejected.  Sparse count did not predict dense
  summary selectivity and the reorder disturbed the cache-friendly feature
  order: two reversed 600-given pairs regressed from 36.62 to 41.33 s mean
  (+12.9%).  Candidate sets and counters were exact, and the code was fully
  reverted.
- Replacing KBO's transient variable-multiset lists with a stack table removed
  the main source of the allocator profile: 51.60 million `multiset_add`
  allocations by given 600.  The first implementation averaged 44.13 s versus
  45.02 s control across reversed 600-given pairs, only a noisy 2.0% apparent
  gain whose individual pairs disagreed.  A direct-indexed refinement then
  took 44.98 s after a 41.69 s adjacent control (+7.9%).  Linear lookup or
  clearing a 100-entry table merely replaced cheap recycled-slab work with
  other hot-loop work.  Both forms were fully reverted; the release binary
  hash returned exactly to the accepted value.
- Turning deterministic rigid radix descent into a loop reduced the compiled
  `retrieve_rec` body from 3,219 to 2,716 bytes, but the changed live-state and
  undo behavior cost total CPU.  It took 42.34 versus 41.59 user seconds at
  600 givens (+1.8%) and 97.62 versus 94.41 at 1,000 (+3.4%).  Both endpoints,
  all hint counters and RSS were unchanged.  The experiment was fully
  reverted; recursive rigid descent remains faster on the tested compiler and
  host.
- Reusing a known logical clause-body size across an unchanged
  materialize/recompress cycle avoided one size traversal, but regressed the
  decisive reversed 1,000-given comparison from 83.20 to 87.71 mean total CPU
  (+5.42%).  Exact trajectories, hint counters and RSS matched.  The apparent
  600-given improvement was short-prefix noise, and the experiment was fully
  reverted.
- Splitting the active-candidate admission case out of
  `packed_add_candidate` made its isolated function smaller, but did not
  shrink the inlined dense collector and averaged 44.34 versus 43.05 total
  CPU at 600 givens (+3.0%) across reversed ordering.  It also increased run
  variance.  The split was fully reverted.
- Copying every exact-demodulated hint solely to detect whether rewriting
  changed it looked attractive at 600 givens: an explicit change result
  removed 50,733 temporary clauses, 603,104 terms and about 704,000 allocator
  calls, and the noisy pair mean improved 10.9%.  At 1,000 givens the reversed
  mean was neutral (+0.36%).  More decisively, the completed proof performs
  only 179,868 exact-positive hint back-demodulations against 16.7 billion
  allocator calls, so this cannot change mature total CPU materially.  The
  prototype was rejected as a short-prefix optimization.
- Applying the new mask-result cache to every directory was also rejected.
  At 1,000 givens two reversed pairs averaged 107.82 versus 104.90 total CPU
  (+2.8%), despite reducing block scans.  Expanding its metadata table from
  2,048 to 16,384 entries reduced collisions but made Josef 02/600 slower and
  raised metadata to about 1.1 MiB.  The accepted 64-block maturity gate and
  compact table are consequences of these failures, not Josef-specific keys.
- Table-driven archive CRC implementations improved the isolated one-million
  record store benchmark by roughly 14--40%, but worsened Josef 02/600.  The
  clean 1-KiB byte table took 47.99 seconds and the compact nibble table 48.52
  seconds after a 43.68-second adjacent control; a slicing prototype was still
  worse.  Extra table/cache and code-layout pressure outweighed checksum work
  in the complete search, so the original small bitwise CRC remains.
- Exporting compressed clauses from `String_buf` in one linear operation
  improved the one-million-record archive benchmark from 6.25 to 4.64 seconds
  (-25.8%), but two reversed Josef 02/600 pairs averaged 44.94 versus 42.82
  seconds (+5.0%).  The microbenchmark win did not generalize and the change
  was reverted.
- Lazily initializing the ordinary matcher's `_AnyConst` table was noisy at
  600 givens and decisively regressed the exact 1,000-given endpoint from
  100.00 to 107.25 total CPU seconds (+7.25%).  Packed matching is not the only
  matcher consumer, so the eager initialization remains.

## Cross-workload gates

- CHAT at 600 givens reaches the known exact
  `(601, 497430, 16974, 0)` endpoint and 3,775 back-demod candidates.  The
  conjunction table is admitted for this different hint population, so its
  467,464 KiB peak RSS is expected and is not Josef 02 back-index growth.
  The clause-level address snapshot was 5.81% faster than its adjacent parent;
  the subsequent query-context refactor is neutral in two reversed pairs.
  Packed-hint counter batching is also neutral at 52.29 versus 52.46 s.
  The later monotone candidate-order fast path improves another 3.97%, from
  56.44 to 54.20 s.  Constant-time packed hint-owner restoration is timing
  neutral-to-slightly-negative at this short prefix (60.21 versus 59.57 s in
  the reverse adjacent observation), while eliminating at least 38.1 million
  list visits.  All preserve the rewrite and back-demod fingerprints.
- Josef 01 at 1,000 givens exactly reproduces
  `(1001, 1628048, 320239, 0)` and every compact unit-index counter.  Current
  host observations span 59.85--70.30 s; the controlled reverse-adjacent gate
  was 60.56 s versus 60.77 s for the parent.  That input clears back
  demodulation and records zero compact rewrite attempts.  It also records
  zero packed-dense queries, so it is an independent trajectory gate rather
  than timing evidence for the counter batching.  No bounded validation
  process swapped.
- `compact_back_demod_test`, `compact_long_run_test`,
  `compact_rewrite_test`, `compact_unit_index_test` and
  `compact_otter_audit_test` pass.  The hint-postings, hint-preview and
  compressed-unit-match tests also pass.  The back-demod test now explicitly
  covers cache admission, reuse, a compatible bucket appended after admission,
  exact decreasing proof-ID order and forced compaction.  Hint-preview also
  covers packed active owners, inactive retained owners, ordinary nonpacked
  mode and post-teardown lookup.

## Recommended full-run options

For the CPU comparison, build the current branch in its supported host-native
mode.  Start from a clean mode switch; do not append ad-hoc `XFLAGS` to an
existing object tree:

```sh
make realclean
make all NATIVE=1
sha256sum bin/prover9
```

This produces a host-specific binary and is unsuitable for copying to a
machine with a different instruction set.  Use plain `make all` for the
portable release control.  The build sentinel automatically recompiles when
switching between these modes.

For the strongest CPU candidate, regenerate a balanced PGO profile on the
same host, compiler and checkout that will run the proof.  Do not copy this
report's binary or profile files to another CPU.  First create bounded copies
of the three training inputs with explicit 600/600/1,000 `max_given` limits,
reasonable `max_seconds`, and a hard `max_megs`; then run:

```sh
make pgo-clean
make realclean
make all NATIVE=1 PGO=gen

taskset -c 1 bin/prover9 < Josef_02.train600.in > Josef_02.train600.out 2>&1
taskset -c 1 bin/prover9 < chat_test.train600.in > chat_test.train600.out 2>&1
taskset -c 1 bin/prover9 < Josef_01.train1000.in > Josef_01.train1000.out 2>&1

# LLVM merges .profraw files; on GCC this now reports a successful no-op.
make pgo-merge
make all NATIVE=1 PGO=use
sha256sum bin/prover9
```

Verify that each training output stopped at its intended `max_given` and did
not swap before accepting the PGO-use build.  Treat a coverage/hash mismatch
in a trained core search or compact-index object as a failed build and
retrain from an empty `pgo_data`; missing profiles in genuinely unused
utilities are harmless.  Retain a plain `NATIVE=1` binary as the unprofiled
control.

Use the following input block unchanged for the external Josef 02 rerun:

```prolog
assign(search_loop,otter).
assign(passive_store,dense).
assign(passive_directory,file).
assign(passive_selector_store,file).
assign(passive_selector_buffer,65536).
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
assign(compact_unit_strategy,code_tree).
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

assign(compact_rewrite_deep_cache_kb,0).
assign(compact_passive_cache,0).
assign(compact_index_stale_pct,25).
assign(compact_term_reclaim_kb,8192).
```

Retain the original inference, ordering, weight and hint commands after this
block.  For an unlimited run remove/replace only bounded `max_given`,
`max_seconds` and `max_megs` commands.  Do not raise the hint-conjunction
budget or enable the deep rewrite cache for the first authority run.  The
mature mask-result cache is automatic under `adaptive32`; there is no new P9
input command to add.  Its `mask_cache_*` fields will appear in the final
`Compact_back_demod:` report.  Constant-time packed hint-owner restoration
is likewise automatic under packed hint modes; its coverage appears in the
final `Hint_id_lookup:` line and requires no option.
Clean passive-record retention is also automatic for this compact-OTTER plus
ancestor-store configuration.  Its coverage appears as `direct_retentions`,
`retention_fallbacks`, and `payload_bytes_avoided` in
`Disabled_compression:`.  Do not add the experiment-only environment variable
used for the same-binary A/B measurement; it is not part of the product.

## Expected full result and acceptance gate

The completed compact baseline is 8,841 total CPU seconds.  The new scalar
accessors remove costs whose old complexity grows with both lifecycle events
and index width; the mask lower-bound optimization has only just crossed over
at 1,500 givens but targets a directory sixteen times wider at the proof.
Inherited slab recycling and direct symbol/term hot-path work also postdate the
baseline output.

A cautious planning range for the next same-machine **release** total is
**6,200--7,500 CPU seconds** (about 15--30% below the compact baseline), with
roughly 5.0--5.2 GiB process PSS.  The bounded `-O3`/LTO result supports a
separate, wider **5,900--7,200 CPU-second** planning range for the recommended
`NATIVE=1` authority run.  Balanced PGO supports a still provisional
**5,400--6,900 CPU-second** planning range.  These are deliberately ranges,
not measured full claims; neither the 2.37--12.08% compiler benefit nor the
12.14--18.04% bounded Josef PGO benefit can be assumed constant through the
mature 13,006-given phase.  A simple per-attempt extrapolation of only the
clause-snapshot delta
from the reversed 600-given mean and the adjacent 1,000-given pair spans about
120--530 user seconds at the proof's 3.678 billion attempts.  That range is
useful for planning but too load-sensitive to add mechanically to the other
unmeasured long-run changes.  The later query-context refactor removes another
6.25--11.87% of bounded Josef 02 user CPU, but the different mature rule and
subject mix makes that percentage equally unsafe to apply directly to the
full baseline.  The back-index optimization likewise cannot be extrapolated
linearly from the 1,500-given prefix.  The monotone candidate-order fast path
removes 4.28--5.27% on bounded Josef 02 and 3.97% on CHAT, but mature hint
rewrites may create more mixed-order fallback vectors, so those percentages
are not applied mechanically to the proof baseline.  The mature mask-result
cache can plausibly remove a large fraction of the proof's 7.35 billion
directory-block examinations if long-run key reuse is high, but the bounded
active-path timing did not prove a CPU gain.  It is consequently assigned no
advance credit in these ranges.  Packed owner lookup removes a proven
245.5-million-node lower bound by given 1,000 and improves that adjacent pair
by 1.37%, but its full restoration count is not present in the old output, so
it is also assigned no separate numerical credit.  The revised range gives
the direct-retention change only a fraction of its bounded 4.7--5.8% total-CPU
effect and of the mature 2,246-second disable clock: the exact 4.1-million
duplicate-record target is known, but dirty fallbacks and mature filesystem
behavior are not.  No full old-P9 Josef 02 time exists.

Accept the external run only if it:

1. proves at the exact 13,006-given trajectory, or explains any trajectory
   difference with normalized given-clause and answer fingerprints;
2. reports `Generated=129776312`, `Kept=9226457`, one proof, and the expected
   back-demod answer fingerprint when replay is exact;
3. shows no swap and records GNU-time RSS, final PSS, filesystem I/O and the
   file-backed directory sizes separately;
4. improves total CPU on 8,841.30 s without an unexplained increase over the
   approximately 5 GiB compact PSS baseline; and
5. retains the full `Compact_rewrite`, `Compact_back_demod`, route, query
   profile, hint and allocator statistics for the next scaling audit; and
6. reports nonzero `mask_cache_hits` after maturity and compares
   `mask_directory_blocks_examined` and `mask_directory_word_checks` with the
   exact baseline values 7,352,224,411 and 27,753,331,661.  Reject the cache as
   a CPU optimization if total CPU does not improve, even when those work
   counters fall; and
7. reports `Hint_id_lookup` with packed hits equal to queries and zero linear
   fallback steps.  Retain `packed_id_sum` so the eliminated-work scale can be
   compared with the 245,545,806 lower bound at given 1,000; and
8. reports `direct_retentions` and `retention_fallbacks`.  Compare the former
   with the structural 4,115,023-record target and explain any substantial
   fallback count.  When the trajectory is exact and fallbacks remain near
   zero, ancestor records should be close to 9.22 million rather than the old
   13.34 million.

Until that run exists, the precise conclusion is: current compact P9 is
decisively faster than old P9 on exact Josef 02 prefixes, and the identified
long-run accidental work is removed, but full proof-to-proof old/new CPU
competitiveness remains unmeasured.
