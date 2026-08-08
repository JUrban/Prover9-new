# Radical RAM Reduction in Prover9

## A DISCOUNT loop, compact state, and Waldmeister-style collective inference

**Engineering report, updated 8 August 2026**

## Executive summary

Long AIM searches did not run out of memory because rejected generated clauses
were accidentally retained.  The dominant problem was that millions of
*kept but not yet selected* clauses accumulated in SOS and were represented as
pointer-rich clause graphs, demodulators, and index entries.  For example:

| Archived run | Given | SOS | Disabled | Hints | Reported memory |
| --- | ---: | ---: | ---: | ---: | ---: |
| Osborn, 4,000-given prefix | 4,000 | 588,252 | 577,347 | 310,153 | 2,626.98 MiB |
| Osborn proof | 11,242 | 4,594,786 | 6,519,452 | 310,153 | 18,810.85 MiB |
| AAPERM long run | 12,560 | 8,504,637 | 4,593,127 | 220,117 | 125,829.01 MiB |

Deleting or compressing disabled clauses helped, but could not change this
growth law.  At the 4,000-given Osborn boundary, deleting almost all disabled
clauses reduced memory from 2,627 to 2,037 MiB, a useful 22% reduction, while
the unchanged 588,252-clause SOS still dominated the process.

The implemented solution changes the architecture:

1. A DISCOUNT loop indexes and infers only with selected active clauses.
2. Passive clauses use dense selector records and serialized/mmap-backed
   bodies instead of live `Topform` graphs in active indexes.
3. Hints use a packed exact-matching representation rather than an FPA forest
   of complete clause trees.
4. Waldmeister-style collective descriptors represent whole inference sets
   without first materializing every conclusion.
5. A bounded candidate cache controls how many collective conclusions may be
   resident at once.
6. Optional promising scans expose the lowest raw-weight conclusions first;
   an optional min-heap compares known next keys across descriptors, with
   mandatory FIFO turns for fairness.
7. Historical active states, proof parents, cursors, checksums, and scheduler
   state are compact and checkpointable.

For the AIM configuration using collective paramodulation and hyperresolution,
the default 4,096-entry cache changes the dominant passive term from millions
of resident clauses to a fixed configured bound after initial preprocessing.
Relative to the archived passive counts, this is a structural resident-count
reduction of approximately 99.3% at 4,000 givens, 99.91% at the full Osborn
proof boundary, and 99.95% at the AAPERM boundary.  These are count/capacity
reductions, not measured whole-process RSS reductions.

The prudent whole-process forecast is **80–95% less RAM**, with **90% as a
reasonable planning midpoint**.  Passive-dominated runs may do better, but
deployment should initially be sized against the 80% case until long runs on a
large-memory host close the acceptance gate.

## 1. What was wrong with the old architecture

The original loop behaved like an OTTER loop.  A kept clause could be used for
simplification, subsumption, demodulation, and indexing before it was selected
as a given clause.  Consequently, “passive” did not mean cold or cheap:

- every kept SOS clause retained a normal clause/literal/term graph;
- eligible passive equations became demodulators;
- forward/backward operations indexed or revisited passive clauses;
- active indexes grew with kept/generated search state rather than with the
  much smaller selected set;
- later-disabled proof ancestors required additional retained state;
- 310,153 hints contributed another large fixed forest and matching index.

[`../Plans-RAM-24.txt`](../Plans-RAM-24.txt) correctly warned that the generated count was not itself a
memory count: immediate tautologies and forward-subsumed clauses are deleted.
It also showed that disabled clauses mattered at large scale.  Both points are
true, but neither identifies the main asymptotic problem.  The crucial
statistics are the millions of SOS clauses and passive demodulators versus only
thousands of given/active clauses.

The Waldmeister paper describes the relevant remedy: keep active and passive
facts strictly separate and condense a whole simultaneously generated set of
critical pairs into constant-size state plus a fixed promising-pair buffer.
The Prover9 implementation generalizes that idea from unit equational critical
pairs to Prover9 paramodulation and positive/negative hyperresolution.

## 2. Implemented architecture

### 2.1 DISCOUNT ownership boundary

The new mode is selected with `assign(search_loop,discount)`.  In this mode:

- only selected clauses occur in active inference and simplification indexes;
- a new candidate is simplified, weighed, hint-matched, filtered, and placed
  in the passive selector without becoming active;
- a selected passive is refreshed against the current simplifier epoch;
- if refresh changes it, a complete copy/rewrite justification is produced
  and the clause is requeued or deleted;
- a clause becomes a demodulator and inference parent only after activation.

The old loop remains the default `search_loop=otter` compatibility mode.

### 2.2 Dense cold passive store

`assign(passive_store,dense)` removes passive ownership from live `Topform`,
`Clist_pos`, AVL selector nodes, active indexes, and the live clause-ID table.
The selector retains a dense record containing the stable ID, exact selector
metadata, matcher hint ID, weight, simplifier epoch, delayed-demodulator bit,
and selector membership.  The clause body and justification are serialized in
an append-only memory or mmap arena and materialized only when required.

The arena is compacted when dead historical records would otherwise grow
without bound.  Selector heaps contain 32-bit record indexes and use lazy
deletion.  Selection materializes and registers one clause before the ordinary
refresh/activation path.

### 2.3 Compact proof ancestors and bookkeeping

Disabled proof parents cannot simply be freed: proofs and deferred inference
may still reference their stable IDs.  `ancestor_store=memory` and
`ancestor_store=mmap` serialize proof-relevant clauses in an append-only,
checksummed archive and materialize them on demand.  The clause-ID table is
paged and compact, and the allocator can return completely empty slabs.

The archive format preserves activation/simplifier epochs needed by delayed
collective inference.  Corrupt versions, bounds, payloads, or checksums fail
closed.

### 2.4 Packed exact hints

`assign(hint_index,packed)` removes live hint term trees and their ordinary FPA
index.  The promoted implementation stores compressed hint bodies, dense
mutable side tables, and compact postings containing stable 32-bit hint IDs,
never pointers into evictable term trees.  Exact symbol/path features and
root-plus-descendant occurrence keys select candidates for equivalence,
ordinary/flipped matching, and hint back-demodulation.  Compact hash buckets
handle equivalence; safe literal-count ranges preserve theta-equivalent
clauses with repeated literals.

Filters are conservative.  The original exact subsumption/equivalence or
`rewritable_clause_type` test makes every final decision.  Rewritten hints
leave counted stale references, and a bounded threshold rebuilds the derived
postings while materializing at most one compressed hint at a time.

Candidate hint IDs are ordered so the existing “first equivalent / last
subsumed” result remains stable.  Differential tests cover matcher ID,
raw/adjusted weight, labels, degradation, hint epoch, and proof result.
`assign(hint_index,hybrid)` is a compatibility alias for the improved mode;
`assign(hint_index,packed_legacy)` selects the former broad packed algorithm
only for diagnostics.

### 2.5 Collective inference frontier

`assign(inference_frontier,collective)` replaces eager generation of most
paramodulation and hyperresolution conclusions with one descriptor per
activated given.  A descriptor records:

- given and historical partner ranges;
- inference-rule bits;
- activation and deactivation epochs;
- a partner cursor;
- a conclusion cursor or promising-order threshold;
- a structural replay/sequence checksum;
- the next known raw candidate key;
- checkpointed scheduler state.

An append-only activation history and persistent historical FPA index recreate
the active set seen when the descriptor was created.  Active bodies are shared
directly.  Only bodies that are later deactivated transfer to history
ownership, so historical state grows with selected givens, not generated or
passive clauses.

### 2.6 Bounded candidate cache and chunks

`collective_candidate_cache` defaults to 4,096.  It bounds materialized
conclusions emitted by the collective paramodulation/hyper frontier.  When the
selector reaches the limit, descriptor expansion pauses and ordinary given
selection drains the best already-materialized clauses.  The budget includes
the current limbo list, so one collective turn cannot overfill the cache.

`collective_candidate_chunk` defaults to 64 and bounds raw conclusions sent
through `cl_process` in one descriptor turn.  A partial inference unit rotates
fairly and resumes later.  Generator-prefix mode verifies a rolling prefix
checksum.  Promising mode verifies a checksum of the complete immutable raw
sequence.

Nothing is silently discarded: every conclusion in a finite descriptor is
eventually exposed by the fair path.  Every exposed conclusion uses the
unchanged simplification, exact hint matching, rule filtering, weighting,
keep/delete, proof-parent, and given-selection code.

Initial SOS clauses and separately enabled eager binary/UR inference are not
currently governed by the collective cache.  The tested AIM/Osborn
configuration clears automatic inference and uses paramodulation plus
hyperresolution, so the dominant generated frontier is covered.

### 2.7 Optional promising ordering

`set(collective_promising_candidates)` scans one immutable inference set and
retains only the lowest `collective_candidate_chunk` keys in a temporary fixed
buffer.  Keys are `(raw clause weight, generator ordinal)`.  Clauses outside
the buffer are deleted before clause IDs or hint state can observe them.  A
partial descriptor stores the committed threshold and exact next raw key.

`set(collective_promising_scheduler)` additionally maintains a min-heap over
descriptors with known next keys.  It requires promising candidates.  By
default, at most seven priority turns occur before one mandatory FIFO turn:

```text
assign(collective_promising_fair_interval,8).
```

The FIFO turn discovers descriptors whose key is not known and prevents
starvation.  Raw weight is a predictor, not a conservative lower bound on the
post-simplification or hint-adjusted selector key.  Both promising features
therefore remain default-off.

### 2.8 Checkpoint/resume

The current collective checkpoint magic is `P9COLLA` (the single-byte
version-10 marker).  It stores activation history, queue order, partial
inference cursors/keys/checksums, hint-probe credit, and the global
priority/fairness cycle.  The priority heap is rebuilt canonically from saved
keys.  Readers remain compatible with `P9COLL5` through `P9COLL9`.

## 3. Semantic and correctness guarantees

The new mode does **not** promise the old OTTER given-clause sequence.  That
sequence depended on eager operations over passive clauses, which are exactly
what the new design removes.  The intended guarantees are:

- sound emitted proofs;
- a fair collective calculus for finite inference sets;
- complete proof-parent reconstruction;
- historical inference against the correct activation epoch;
- deterministic resume within a fixed mode/configuration;
- exact hint behavior whenever a candidate is materialized.

For a materialized normalized clause, exact hint behavior includes
subsumption/equivalence, flipped equality matching, matcher ID, adjusted
weight, copied labels, degradation, `hint_match_once`, matcher limits,
`breadth_first_hints`, and `hint_age` state.

An unseen conclusion cannot yet influence hint-prioritized selection before
its descriptor is explored.  A starvation-safe `collective_hint_probes` flag
exists, but experiments showed that it can perturb the frontier substantially,
so it is also default-off.

## 4. How to build and use it

### 4.1 Build and smoke tests

```sh
cd /project/Prover9
make all -j2
make test1
./test.src/discount_loop_test.sh
./test.src/collective_frontier_test.sh
./test.src/hint_index_trace_test.sh
./test.src/hint_checkpoint_test.sh
```

`make memory-tests` runs the focused storage/allocator lifecycle tests.  For a
release or long-run deployment, run the broader repository test targets as
well.

### 4.2 Recommended conservative AIM configuration

Place the following after any automatic settings, so later assignments win:

```text
clear(auto_inference).
set(paramodulation).
set(hyper_resolution).

assign(search_loop,discount).
assign(passive_store,dense).
assign(hint_index,packed).
assign(inference_frontier,collective).
assign(ancestor_store,mmap).

assign(sos_limit,-1).
assign(collective_given_ratio,4).
assign(collective_candidate_chunk,64).
assign(collective_candidate_cache,4096).

clear(collective_hint_probes).
clear(collective_promising_candidates).
clear(collective_promising_scheduler).

set(clocks).
assign(stats,all).
```

This is the conservative measured configuration: stable-ID packed hints, dense
passives, collective paramodulation/hyperresolution, a 4,096-candidate cache,
and FIFO descriptor scheduling.  `passive_store=dense` currently requires
`sos_limit=-1`; collective mode requires DISCOUNT and a memory or mmap ancestor
store.

For an isolated hint-index comparison that preserves the current radical
search trajectory, keep every other option identical and vary only:

```text
assign(hint_index,compact).       % depth-2 FPA reference
assign(hint_index,packed).        % improved stable-ID implementation
assign(hint_index,packed_legacy). % former broad packed diagnostic
```

`hybrid` is an alias for improved `packed`.  Do not compare `packed` with a
run that also changed `back_demod_hints`, given selection, or inference-frontier
options; those change semantics or the search trajectory independently.

The `mmap` stores use immediately unlinked temporary backing files: they lower
resident pressure by letting the operating system page cold records, but they
still need adequate local temporary-filesystem capacity.  They are not restart
files; use Prover9 checkpoints for restart.

`collective_given_ratio=4` means one descriptor-expansion turn is due after
four given activations.  The default is 1.  Raising the value delays collective
work and can reduce short-prefix residency, but also changes the search.  It
does not drop work: if SOS empties, the finite descriptor queue is drained.

### 4.3 Tighter-memory experimental configuration

To study a much smaller materialized frontier:

```text
assign(collective_candidate_cache,64).
set(collective_promising_candidates).
clear(collective_promising_scheduler).
```

On the bounded Osborn prefix, per-set promising ordering gave the smallest SOS
of the cache-64 variants.  Global priority can be enabled separately:

```text
set(collective_promising_scheduler).
assign(collective_promising_fair_interval,8).
```

Do not assume 64 is universally optimal.  A very small cache changes which
deferred candidates become active early and increases replay work.  Start with
4,096 for compatibility/coverage testing, then compare 1,024, 256, and 64 on a
representative solved suite.

### 4.4 Statistics to watch

With `assign(stats,all)`, the important lines are:

- `Search_loop`: active versus passive indexed counts;
- `Passive_refresh`: stale selection/requeue/subsumption work;
- `Collective_frontier`: created, completed, pending descriptors;
- `Collective_work`: physical turns versus completed pairs/sets;
- `Collective_chunks`: emitted, replayed, deferred, and raw peak;
- `Collective_candidate_cache`: configured limit, observed peak, stalls;
- `Collective_promising` and `Collective_promising_scheduler`;
- `Collective_memory` and `Collective_history_index`;
- `Clause_body_bytes`, `Dense_passive`, `Hint_store`, and
  `Packed_hint_index`, including per-operation `Packed_hint_operation` and
  `Better_packed_postings` lines;
- allocator live/reserved/fragmentation and external RSS.

The cache peak includes initial/preprocessing SOS occupancy.  If the initial
set is larger than the configured cache, selection first drains it; this does
not mean a collective turn overfilled the cache.

### 4.5 Checkpoints and proof verification

Enable integrity hashes with:

```text
set(checkpoint_verify).
assign(checkpoint_minutes,30).
```

For a deterministic one-shot checkpoint after a completed given count:

```text
assign(checkpoint_given,4000).
set(checkpoint_exit).
```

`checkpoint_given` disables itself before writing the checkpoint, so resume
cannot retrigger the same boundary.

A checkpoint can also be requested after search initialization:

```sh
kill -USR2 PROVER9_PID
```

Resume with:

```sh
bin/prover9 -r prover9_PID_ckpt_GIVEN
```

Verify successful proof output independently:

```sh
bin/prooftrans expand < run.out > expanded-proof.out
bin/directproof < run.out > direct-proof.out
```

## 5. Measured results

Development began with at most hundreds of givens, 30 seconds, 256 MiB, and a
deterministic 10% (31,014) hint sample.  After those gates passed, the complete
310,153-hint input was run only to 100 givens with 90 CPU seconds and 512 MiB
as hard limits.  No week-long or 1,000/4,000-given improved run was launched
on the current host.

### 5.1 Component results

| Change | Measured result |
| --- | --- |
| Compressed passive bodies, 500 givens | 38.84 MB to 3.57 MB body storage (90.8%); total RSS about 145 to 114.7 MiB (21%) |
| Improved packed hints, 31,014 hints/100 givens | 3.98 s user CPU and 35,600 KiB RSS versus compact's 6.13 s/about 43.0 MiB; exact trace |
| Shared collective history | 88,976 estimated cloned-body bytes to 17,304 retained bytes (80.6%) |
| Sparse deactivation/history structures | 34,048 to 19,584 structural bytes (42.5%) |
| Original persistent collective scaffold | descriptor/history/retained state 132,944 to 46,808 bytes (64.8%) at that revision |
| Synthetic ancestor archive, 100,000 records | 26.4 MB replaced graph/bookkeeping estimate to 13.01 MB used+resident logical bytes (50.7%); always-resident portion 92.8% lower |

### 5.2 Frontier results at 250 givens

| Configuration | Generated | Kept | SOS | Peak RSS | Wall/CPU observation |
| --- | ---: | ---: | ---: | ---: | ---: |
| Eager hyper, collective paramodulation | 34,709 | — | 4,846 | about 32.3 MiB | bounded attribution run |
| Correct collective history, ratio 4, cache 4,096 | 830 | 587 | 2 | 32,972 KiB | 17.13 s wall / 16.73 s user |
| Cache 64, FIFO raw order | 852 | 615 | 38 | 32,976 KiB | 21.24 s wall / 20.76 s user |
| Cache 64, per-set promising | 873 | 588 | 8 | 33,108 KiB | 18.50 s wall / 17.77 s user |
| Cache 64, per-set + global priority | 873 | 595 | 15 | 33,112 KiB | 16.61 s wall / 16.25 s user |

Against the eager-hyper attribution run, the corrected collective boundary
generated 97.6% fewer clauses and retained 99.96% fewer SOS clauses.  The
total peak RSS did not fall in this short comparison because all variants
peaked during the same packed-hint preprocessing/allocator floor, before the
frontier could dominate.

The cache-64 comparison shows why optional policies are not defaulted merely
from one prefix.  Per-set promising ordering improved SOS quality substantially
(38 to 8), while global priority had only one actual priority opportunity and
finished at 15.  All three stay tiny relative to the eager 4,846-clause
frontier, but broader proof coverage is needed.

### 5.3 Checkpoint and proof evidence

- Focused DISCOUNT equality proofs pass `prooftrans` and `directproof`.
- Collective equality and hyperresolution proofs pass `prooftrans`.
- Forced one-candidate chunks exercise replay, deferral, and cache stalls.
- Historical-hyper tests require a parent disabled after descriptor creation.
- `P9COLL9` captured 43 partial promising cursors and resumed to exactly the
  uninterrupted 9,324-generated/151-kept boundary with 18/18 hashes.
- `P9COLLA` captured a nonzero fairness-cycle position and six heap entries;
  resume exactly matched 13,611 generated, 11,854 priority turns, 1,835 fair
  turns, and a heap peak of 68, again with 18/18 hashes.
- The current reader reproduced older `P9COLL5`–`P9COLL9` boundaries.
- Stable-ID packed hints checkpointed at givens 0, 2, and 6 reproduce the
  uninterrupted post-checkpoint `HINT_TRACE` byte-for-byte, including a
  boundary after hint rewrite/reindex activity.

### 5.4 Better packed hint-index results

The archived 1,000-given full-hint results established the target:

| Mode | Given | CPU seconds | Peak RSS |
| --- | ---: | ---: | ---: |
| Old P9 legacy | 1,001 | 113.44 | 507,108 KiB |
| Radical compact depth-2 FPA | 1,001 | 68.33 | 371,904 KiB |
| Former packed algorithm | 1,001 | 441.39 | 295,680 KiB |

The improved stable-ID mode was bounded to 100 givens on this host.  Two
full-hint repetitions used 71.90 and 81.03 user seconds and peaked at 296,064
and 295,808 KiB respectively.  Both produced `Generated=499`, `Kept=436`; the
recorded first-100 given trace exactly matches compact, with SHA-256
`620c7df29deac1f5cff1f5855570ca27c9a3a5594984f4bd742da18923e57ea2`.
The CPU range reflects a busy host and is not a 1,000-given result.

At the final bounded state, packed operations materialized 912,856 hint bodies
in total: 221,157 for equivalence, 141,588 for ordinary matching, 1,255 for
flipped matching, and 548,856 for back-demodulation.  Back-demodulation found
51,182 exact rewrites.  The posting rebuild kept stale storage bounded and
finished with 86,239 stale structural references and 3,087 stale equivalence
references versus more than 6.6 million live references.

The measured full-hint peak is essentially unchanged from the former packed
algorithm, 20.4% below compact FPA, and 41.6% below old P9.  This is the
fixed-hint-bank saving; it must not be mislabeled as the expected 80--95%
whole-process saving from combining packed hints with dense passives and the
collective inference frontier in week-long passive-dominated runs.

## 6. How much RAM is likely to be saved

### 6.1 Structural passive-count reduction

For collective paramodulation/hyperresolution and a cache limit of 4,096, the
resident materialized collective frontier no longer grows with all generated
conclusions.  Ignoring initial SOS and separately eager rules, the archived
count comparison is:

| Archived state | Old resident SOS | Collective cache | Resident-count reduction |
| --- | ---: | ---: | ---: |
| Osborn, 4,000 givens | 588,252 | 4,096 | 99.30% |
| Osborn proof boundary | 4,594,786 | 4,096 | 99.91% |
| AAPERM long boundary | 8,504,637 | 4,096 | 99.95% |

This does not mean total RSS falls by exactly those percentages.  The process
still has fixed hints, active clauses, historical active bodies, descriptors,
proof archives, allocator reservation, and executable/library memory.

The important asymptotic change is:

```text
old: RAM ~= fixed hints + O(all kept passive bodies and indexes)
new: RAM ~= packed hints + O(selected active/history) + O(cache limit)
```

At the archived Osborn proof there were 11,242 givens but 4.59 million SOS
clauses.  At AAPERM there were 12,560 givens but 8.50 million SOS clauses.
Replacing the latter scale by the former scale plus a fixed cache is why a
radical whole-process reduction is plausible.

### 6.2 Whole-process forecast

The following table is a scenario table, not a benchmark claim:

| Old reported memory | 80% saving | 90% saving | 95% saving | 98% saving |
| ---: | ---: | ---: | ---: | ---: |
| Osborn 4,000: 2,626.98 MiB | 525 MiB | 263 MiB | 131 MiB | 53 MiB |
| Osborn proof: 18,810.85 MiB | 3,762 MiB | 1,881 MiB | 941 MiB | 376 MiB |
| AAPERM: 125,829.01 MiB | 25,166 MiB | 12,583 MiB | 6,291 MiB | 2,517 MiB |

The engineering forecast is:

- **Capacity-plan minimum:** 80% total-RAM reduction.
- **Likely planning midpoint:** about 90%.
- **Plausible on strongly passive-dominated searches:** 95% or more.
- **Not yet justified as a promise:** 98–99% total RSS, despite the
  99%+ resident-passive count reduction.

The uncertainty is dominated by changed search trajectories, full-size packed
hint CPU beyond 100 givens, active/history term complexity, proof-archive growth,
eager rules outside the collective frontier, and allocator high-water effects.

### 6.3 How to establish the real number

On a suitable high-memory host, run old and new binaries/configurations with:

1. identical input and hint files, recorded SHA-256 hashes;
2. identical external wall/RAM limits;
3. at least three AIM/Osborn-class workloads with old peak RSS above 100 MB;
4. periodic external RSS sampling plus final `/usr/bin/time -v`;
5. `assign(stats,all)` accounting;
6. proof validation for every success;
7. checkpoint/resume validation on at least one long run.

Report both equal-resource solved coverage and equal-given boundary data.  Do
not compare only generated counts or only allocator “Megabytes”.  The final
acceptance gate should require at least 80% lower external peak RSS and enough
component accounting to explain 95% of the delta.

## 7. Current limitations and next work

1. Raw promising weight is not a lower bound after simplification or hint
   adjustment.  A safe hint-aware predictor remains research work.
2. Unmaterialized conclusions cannot exactly subsume/match hints before their
   descriptor is visited.  Every materialized conclusion is matched exactly.
3. Binary and UR resolution are still eager and can exceed the collective
   cache if enabled heavily.
4. Initial SOS may exceed a small cache and is drained rather than rejected.
5. Retained historical bodies are shared/ordinary term trees; a packed
   immutable retained-history representation could reduce the active tail.
6. The new mode is fair and proof-tested but does not reproduce OTTER search
   order.  Solved-problem coverage must decide default strategies.
7. Full 310,153-hint startup and 100-given gates passed; the 1,000-given,
   user-run 4,000-given, and week-long acceptance gates still belong on the
   suitable host.

## 8. Main commits and files

The implementation was split into reviewable commits, including:

| Commit | Main change |
| --- | --- |
| `8526000` | Compact DISCOUNT and initial Waldmeister path |
| `083c35d` | Bounded dense passive history and compaction |
| `f36c1c9` | Exact historical indexes for delayed hyper batches |
| `986bd6e` | Persistent versioned collective index |
| `3d05320` | Starvation-safe hint probes |
| `965feb2` | Shared active bodies in collective history |
| `b3cc296` | Sparse deactivation epochs |
| `9eeb048` | Resumable bounded hyper chunks |
| `2d59dad` | Bounded candidate residency and paramodulation chunks |
| `01b51c2` | Per-set promising candidate buffer |
| `c5f92f0` | Global raw-weight priority with FIFO fairness |
| `171e6be` | Bounded tight-cache Osborn scheduler measurements |
| `94a4a4d` | Compact stable-ID posting primitive |
| `852d926` | Selective stable-ID hint back-demodulation |
| `7b46feb` | Selective equivalence and ordinary hint matching |
| `24413df` | Occurrence-correlated rewrite features |
| `a6b36f6` | Deterministic packed-hint checkpoint regression |
| `7611da4` | Promote the improved index to `hint_index=packed` |

Important implementation and documentation files are:

- [`provers.src/search.c`](provers.src/search.c)
- [`provers.src/search-structures.h`](provers.src/search-structures.h)
- [`provers.src/cold_passive_store.c`](provers.src/cold_passive_store.c)
- [`provers.src/cold_passive_store.h`](provers.src/cold_passive_store.h)
- [`test.src/collective_frontier_test.sh`](test.src/collective_frontier_test.sh)
- [`test.src/discount_loop_test.sh`](test.src/discount_loop_test.sh)
- [`test.src/hint_index_trace_test.sh`](test.src/hint_index_trace_test.sh)
- [`test.src/hint_checkpoint_test.sh`](test.src/hint_checkpoint_test.sh)
- [`ladr/hint_postings.c`](ladr/hint_postings.c)
- [`ladr/hints.c`](ladr/hints.c)
- [`P9-BETTER-PACKED-PLAN.md`](P9-BETTER-PACKED-PLAN.md)
- [`P9-DISCOUNT-WALDMEISTER-PLAN.md`](P9-DISCOUNT-WALDMEISTER-PLAN.md)
- [`P9-MEMORY-RESULTS.md`](P9-MEMORY-RESULTS.md)
- [`Checkpoint-Format-Spec.txt`](Checkpoint-Format-Spec.txt)

## 9. Conclusion

The work does more than shrink clauses by a few percent.  It removes the
architectural requirement that millions of passive conclusions be resident,
fully materialized, and indexed.  The measured short prefixes demonstrate
correct ownership boundaries, exact materialized hint behavior, bounded
frontiers, deterministic restart, and dramatic SOS-count reductions.  The
remaining uncertainty is the whole-search trajectory and fixed/active tail,
not whether the dominant passive store still grows without a bound.

For operational planning today, assume **80–95% less total RAM**, use **90%**
as the midpoint, retain the old OTTER mode for compatibility comparisons, and
perform the final long-run acceptance campaign before changing production
defaults.

## References

1. T. Hillenbrand, “Citius altius fortius: Lessons learned from the Theorem
   Prover Waldmeister,” 2003/2004, DOI 10.1016/S1571-0661(04)80649-2.
2. [`../Plans-RAM-24.txt`](../Plans-RAM-24.txt), archived project discussion and long-run statistics.
3. [`P9-MEMORY-RESULTS.md`](P9-MEMORY-RESULTS.md), bounded baseline and component measurements.
4. [`P9-DISCOUNT-WALDMEISTER-PLAN.md`](P9-DISCOUNT-WALDMEISTER-PLAN.md), design contract, implementation log,
   and acceptance gates.
