# Radical RAM Reduction in Prover9

## A DISCOUNT loop, compact state, and Waldmeister-style collective inference

**Engineering report, updated 11 August 2026**

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
4. A balanced Waldmeister-style scheduler splits positive hyperresolution,
   negative hyperresolution, and both paramodulation directions into
   independent, fairly serviced descriptors with admission backpressure.
5. Native pointer-free paramodulation and hyperresolution continuations bound
   every raw-enumeration turn without regenerating prefixes.
6. One global bounded pool ranks exact, read-only normalized hint previews,
   while every committed clause still follows the authoritative Prover9 path.
7. A bounded dual cursor can discover and promote a hint match ahead of fair
   enumeration, then verifies and skips that exact raw ordinal once the fair
   cursor catches it.
8. Historical active states, proof parents, both cursors, bounded promotion
   records, and scheduler/pool state are compact and checkpointable.

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

For the trajectory-sensitive Osborn proof, Phase 5 now provides a second
product mode: eager compact OTTER.  It reaches the same 2,945-given boundary
with the same 7,051-clause proof length and exactly replays the current
full-body packed-fast control.  It takes 754.78 user seconds and peaks at
127,420 KiB versus old P9's 550,400 KiB.
That is a measured **76.85% whole-process reduction (4.32x smaller)** while
being 1.42% faster in user CPU.  This trajectory-sensitive problem has a
large fixed 88,494-hint floor, so it does not quite reach the report's 80%
planning case; the much larger passive-dominated AIM searches remain the
workloads where the 80–95% forecast is expected to apply.

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

### 2.5 Balanced collective inference

`assign(inference_frontier,collective)` still selects the compact frontier.
The stronger policy is separately opt-in:

```text
assign(collective_scheduler,balanced_hint).
```

Each selected given creates independent descriptors for positive and negative
hyperresolution and for paramodulation from and into historical partners.
Weighted rule lanes plus a mandatory oldest turn prevent one large inference
rule from hiding another.  Descriptor admission stops at a hard high-water
mark and drains toward the low-water mark before selecting more givens.  The
bounded measurements selected defaults of 256/192 descriptors and an oldest
activation lag of 256; these replace the intentionally loose provisional
4096/3072 development values.

An append-only activation history recreates the active set visible at each
descriptor epoch.  Active bodies are shared; only later-deactivated bodies
transfer to history ownership.  Descriptor/history growth therefore follows
selected active work rather than the passive/generated clause count.

### 2.6 Native continuations and bounded candidate windows

Balanced paramodulation and hyperresolution use stable pointer-free native
continuations over partner, literal, equality side, subterm path, nested clash,
and mate-phase coordinates.  `collective_raw_work_budget` (default 64) is a
hard raw-visit bound per fair iterator turn.  No conclusion prefix is replayed.

Yielded clauses enter one global pool.  `collective_candidate_window`
(default 64) bounds the active ranked window;
`collective_candidate_commit_interval` forces regular authoritative commits,
and `collective_candidate_fair_interval` commits the oldest resident entry at
fixed intervals.  `collective_candidate_cache` remains the global hard count
bound across transient pool/limbo bodies.

Every pool member is normalized, hint-matched, filtered, weighed, and retained
or deleted by the unchanged `cl_process` path.  The preview changes ordering
only.  Initial SOS and separately enabled eager binary/UR inference are not
covered by the collective pool.

### 2.7 Exact preview and dual-cursor discovery

The pool preview normalizes an owned scratch clause, queries the configured
exact hint index read-only, and predicts the actual high/low selector key.  It
does not allocate a clause ID, mutate hints, copy labels, degrade a hint, or
change authoritative hint/index statistics.  Tautologies are deliberately not
preview-matched because `cl_process` deletes them before authoritative hint
matching.  Hint or simplifier epoch changes trigger lazy refresh before a
candidate can affect priority.

With `set(collective_hint_discovery)` (the balanced-policy default), every
descriptor also has an independent lookahead continuation.  Hot descendants
get additional turns, a low-rate general turn explores the oldest ordinary
descriptor, and every discovery turn is raw-work bounded.  Lookahead stops at
a configured conclusion distance or promotion count until fair work catches
up.  A previewed match may be committed early through `cl_process`; its raw
ordinal and structural fingerprint are retained in a small bounded set.  The
fair iterator later reproduces and verifies that conclusion exactly, removes
the record, and deletes the duplicate body.  A looked-ahead conclusion is thus
enumerated at most twice, and is authoritatively committed exactly once.

### 2.8 Legacy promising policies

The older `collective_promising_candidates`,
`collective_promising_scheduler`, and `collective_hint_probes` options remain
available for controlled comparisons but remain off in the balanced profile.
They use replay/raw-weight predictors and should not be combined casually with
the native preview scheduler.

### 2.9 Checkpoint/resume

Balanced discovery checkpoints use `P9COLLF`; live pool files use
`P9CPOOL3`.  They serialize queue/lane/drain state, both native cursors,
bounded consumed ordinal/fingerprint records, preview metadata, and candidate
bodies.  A regression checkpoints while both a promoted body and its
ahead-consumed record are live, resumes, and reproduces the uninterrupted
terminal accounting.  The reader still accepts legacy `P9COLL5`--`P9COLLA`
and the preceding balanced `P9COLLB`--`P9COLLE` formats under their matching
policy; frontier/pool version mismatches fail closed.

## 3. Semantic and correctness guarantees

The new mode does **not** promise the old OTTER given-clause sequence.  That
sequence depended on eager operations over passive clauses, which are exactly
what the new design removes.  The intended guarantees are:

- sound emitted proofs;
- a fair collective calculus for finite inference sets;
- complete proof-parent reconstruction;
- historical inference against the correct activation epoch;
- deterministic resume within a fixed mode/configuration;
- exact hint behavior for every preview and authoritative commitment;
- bounded raw work, descriptor count, candidate bodies, discovery distance,
  and promotion metadata.

For a materialized normalized clause, exact hint behavior includes
subsumption/equivalence, flipped equality matching, matcher ID, adjusted
weight, copied labels, degradation, `hint_match_once`, matcher limits,
`breadth_first_hints`, and `hint_age` state.

The discovery cursor may inspect a bounded prefix ahead of fair enumeration.
A nonmatching scratch conclusion is simply discarded and later revisited by
the fair cursor; it is never treated as redundant.  A promoted match is
rematched authoritatively and fingerprint-checked at fair catch-up.  Mandatory
ordinary discovery, oldest descriptor service, and oldest pool commitment
preserve finite-work fairness even under a continuing stream of hinted work.

## 4. How to build and use it

### 4.1 Build and smoke tests

```sh
cd /project/Prover9
make all -j2
make test1
make -C test.src iterator-tests hint-postings-test
./test.src/collective_balanced_test.sh
./test.src/discount_loop_test.sh
./test.src/collective_frontier_test.sh
./test.src/hint_index_trace_test.sh
./test.src/hint_checkpoint_test.sh
```

`make memory-tests` runs the focused storage/allocator lifecycle tests.  For a
release or long-run deployment, run the broader repository test targets as
well.

### 4.2 Recommended balanced-hint AIM configuration

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
assign(collective_scheduler,balanced_hint).

assign(sos_limit,-1).
assign(collective_candidate_cache,4096).
assign(collective_candidate_window,64).
assign(collective_raw_work_budget,64).

assign(collective_descriptor_high_water,256).
assign(collective_descriptor_low_water,192).
assign(collective_oldest_lag_limit,256).

set(collective_hint_discovery).
assign(collective_discovery_raw_budget,32).
assign(collective_discovery_distance,256).
assign(collective_discovery_promotion_cap,32).
assign(collective_discovery_general_interval,8).
assign(collective_discovery_turn_interval,2).

clear(collective_hint_probes).
clear(collective_promising_candidates).
clear(collective_promising_scheduler).

set(clocks).
assign(stats,all).
```

This is the bounded stronger configuration: stable-ID packed hints, dense
passives, split native paramodulation/hyperresolution iterators, a globally
bounded preview pool, descriptor backpressure, and exact dual-cursor hint
discovery.  The 256/192/256 descriptor profile is now the balanced-policy
default and is written explicitly above so long-run command files remain
self-documenting.  `passive_store=dense` requires `sos_limit=-1`; collective
mode requires DISCOUNT and a memory or mmap ancestor store.

For the old trajectory/control policy, omit `collective_scheduler` (its
default is `legacy`), use `collective_given_ratio=4`, and clear discovery and
the three older aids.  Do not resume a legacy checkpoint under the balanced
policy or vice versa.

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

`collective_given_ratio` is primarily a legacy-policy control.  Balanced mode
uses admission high/low watermarks: lowering them spends more time completing
inference debt before selecting new givens, while raising them admits more
pending rule work.  The Osborn gate showed that the provisional value 4096
allowed the 250-given prefix to finish with 715 of 750 descriptors pending;
256/192 completed 525 descriptors and materially restored the hint stream.

### 4.3 Compact-OTTER proof/replay mode and heap policy

For an old-OTTER-trajectory comparison with the Phase-5 compact indexes, use
the file-backed ancestor archive and start Prover9 with the compact glibc heap
policy:

```sh
P9_COMPACT_HEAP=1 bin/prover9 < osborn-compact.in > osborn-compact.out
```

The corresponding input controls are:

```text
assign(search_loop,otter).
assign(passive_store,dense).
assign(hint_index,packed_fast).
assign(inference_frontier,clauses).
assign(ancestor_store,file).
assign(compact_passive_cache,0).
assign(compact_index_stale_pct,10).
assign(compact_term_reclaim_kb,2048).
set(compact_otter_demodulation).
set(compact_otter_unit_index).
set(compact_otter_back_demod_index).
set(compact_otter_nonunit_index).
assign(sos_limit,-1).
```

`P9_COMPACT_HEAP=1` is a process-start environment control, not an input-file
assignment.  On glibc it requests a 64-KiB mmap threshold and a zero trim
threshold before problem allocation, keeping medium and large transient
arrays independently returnable.  On an unsupported libc it is a safe no-op.
With `assign(stats,all)`, verify the line `Libc_heap_bytes: ...
compact_policy=enabled`; a disabled value means the platform did not honor
the policy.  The equivalent low-level glibc spelling is
`GLIBC_TUNABLES=glibc.malloc.mmap_threshold=65536:glibc.malloc.trim_threshold=0`,
but the P9 switch is the product-facing invocation for scripts.

The accepted full product validation reaches the historical boundary at
2,945 givens (`Generated=8,248,032`, `Kept=272,787`, `Sos=131,001`).
`prooftrans parents_only` accepts the 7,051-clause proof; the normalized
section is byte-identical to the current full-body `packed_fast` reference
and has SHA-256
`9d7c9a12894c1c11ede6aeae08d1cec658ccee66a47fb9663859347a5413fd07`.
The archived FPA run generated and kept two additional `other` clauses, so
its later raw IDs and some independent proof-line order differ; its given,
inference-rule, SOS, demodulator, and proof-length boundaries agree.
It takes 754.78 user seconds and 14:19.48 wall time.  External peak RSS is
127,420 KiB, passing the 128,000-KiB hard gate by 580 KiB; frozen terminal PSS
is 112,809 KiB.  The current measured binary SHA-256 is
`78b5ef4b2e53fc5ae2ab81cc46e6455213128e8ac083d6fb11259ad7b1339a6b`.

The final transient controls matter for long runs: retained shared terms are
rebased through unlinked files with a bounded radix sorter, materialized
back-demod rebuilds stream IDs in 4,096-ID batches, and the packed-fast cache
uses 16,384 exact 136-byte entries.  Component accounting explains 96.93% of
terminal PSS.  The 84,404,096-byte ancestor file is disk backing, not resident
memory.  Because the peak-gate margin is narrow and libc behavior can vary,
remeasure `/usr/bin/time -v` RSS on a deployment host rather than relying on
Prover9's historical `Megabytes` line.

The final checkpoint audit preserves exact reporting as well as continuation
semantics.  Dead pre-elimination disabled clauses with ID 0 remain omitted
from checkpoint bodies, but their count is saved as metadata.  A full-hint
checkpoint at Given 100 resumes to the same 300-given event hash and every
terminal population, including `Disabled=1,183`; all 22 integrity checks pass.

### 4.4 Tighter-memory and legacy-order experiments

To make the balanced hard count agree with its default 64-entry ranked
window:

```text
assign(collective_candidate_cache,64).
assign(collective_candidate_window,64).
```

The following are older replay/raw-weight comparison policies, not additions
to the recommended balanced profile:

```text
set(collective_promising_candidates).
clear(collective_promising_scheduler).
```

On the bounded Osborn prefix, per-set promising ordering gave the smallest SOS
of the cache-64 variants.  Global priority can be enabled separately:

```text
set(collective_promising_scheduler).
assign(collective_promising_fair_interval,8).
```

Do not assume 64 is universally optimal.  A small pool changes which deferred
candidates become active first.  Native balanced iterators do not replay raw
prefixes, but legacy promising modes can.  Compare 4,096, 1,024, 256, and 64
on a representative solved suite before changing an established deployment.

### 4.5 Statistics to watch

With `assign(stats,all)`, the important lines are:

- `Search_loop`: active versus passive indexed counts;
- `Passive_refresh`: stale selection/requeue/subsumption work;
- `Collective_frontier`: created, completed, pending descriptors;
- `Collective_work`: physical turns versus completed pairs/sets;
- `Collective_balanced`: per-rule turns, oldest turns, and withheld givens;
- `Collective_chunks`: emitted, replayed, deferred, and raw peak;
- `Collective_iterators`: raw bounds and native continuation storage;
- `Collective_candidate_cache`: configured limit, observed peak, stalls;
- `Collective_candidate_pool` and `Collective_preview`;
- `Collective_discovery`: hot/general/fair turns, confirmations, skips,
  distance, caps, and live consumed-record bytes;
- `Collective_memory` and `Collective_history_index`;
- `Clause_body_bytes`, `Dense_passive`, `Hint_store`, and
  `Packed_hint_index`, including per-operation `Packed_hint_operation` and
  `Better_packed_postings` lines;
- allocator live/reserved/fragmentation and external RSS.

The cache peak includes initial/preprocessing SOS occupancy.  If the initial
set is larger than the configured cache, selection first drains it; this does
not mean a collective turn overfilled the cache.

### 4.6 Checkpoints and proof verification

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

### 4.7 Bounded comparison harness

The control harness arguments are input, output directory, maximum givens,
maximum CPU seconds, and maximum MiB.  `OSBORN_CASES` prevents accidental
reruns of policies not under test:

```sh
OSBORN_CASES=balanced_hint_packed \
  test.src/osborn_collective_controls.sh \
  /path/to/osborn.in results/osborn-balanced 500 3600 4096
```

Available cases are `otter_fpa`, `discount_clauses`,
`collective_conservative`, `collective_aids`, `balanced_hint` (compact/FPA
hints), and `balanced_hint_packed` (recommended).  The harness writes the
exact effective input, binary/input hashes, limits, exit status, stdout,
stderr, and `/usr/bin/time -v` output.  Optional balanced overrides are
recorded verbatim:

```sh
OSBORN_CASES=balanced_hint_packed \
OSBORN_BALANCED_OPTIONS='assign(collective_candidate_cache,256).
assign(collective_candidate_window,64).' \
  test.src/osborn_collective_controls.sh \
  /path/to/input.in results/tuned 250 120 2048
```

For a long production run, keep the limits in the generated input even when
an external job scheduler also enforces them.  Use periodic checkpoints and
retain the `.out`, `.err`, `.time`, `hashes.txt`, and `limits.txt` files for
the old/new comparison.

## 5. Measured results

Development began with at most hundreds of givens, 30 seconds, 256 MiB, and a
deterministic 10% (31,014) hint sample.  After those gates passed, the complete
310,153-hint input was run only to 100 givens with 120 CPU seconds and 512 MiB
as hard limits.  No week-long or 1,000/4,000-given improved run was launched
on the current host.  All stronger-scheduler results below use zero native
prefix replay.

### 5.1 Component results

| Change | Measured result |
| --- | --- |
| Compressed passive bodies, 500 givens | 38.84 MB to 3.57 MB body storage (90.8%); total RSS about 145 to 114.7 MiB (21%) |
| Improved packed hints, 31,014 hints/100 givens | 3.98 s user CPU and 35,600 KiB RSS versus compact's 6.13 s/about 43.0 MiB; exact trace |
| Shared collective history | 88,976 estimated cloned-body bytes to 17,304 retained bytes (80.6%) |
| Sparse deactivation/history structures | 34,048 to 19,584 structural bytes (42.5%) |
| Original persistent collective scaffold | descriptor/history/retained state 132,944 to 46,808 bytes (64.8%) at that revision |
| Synthetic ancestor archive, 100,000 records | 26.4 MB replaced graph/bookkeeping estimate to 13.01 MB used+resident logical bytes (50.7%); always-resident portion 92.8% lower |

### 5.2 Stronger scheduler results

The decisive tuning variable was inference debt, not a larger lookahead raw
budget.  With the provisional 4096/3072 descriptor watermarks, a 250-given
10%-hint run ended with 715 of 750 descriptors pending and found no real
lookahead promotion after tautologies were excluded from preview matching.
The bounded 256/192 profile instead forced rule work to catch up:

| Input/profile | Given | Generated | Kept | Currently matched hints | Completed / peak descriptors | Confirmed promotions | User CPU | Peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Osborn 10%, loose 4096/3072 | 251 | 866 | 588 | 38 | 35 / 715 | 0 | 7.60 s | 43,984 KiB |
| Osborn 10%, bounded 256/192 | 251 | 60,703 | 16,449 | 266 | 525 / 255 | 3,314 | 20.15 s | 46,436 KiB |
| Osborn full, packed, bounded | 101 | 825 | 550 | 141 | 82 / 254 | 67 | 79.09 s | 296,192 KiB |
| AIM Osborn+Kcom, packed | 101 | 14,273 | 271 | 181 | 108 / 254 | 228 | 2.70 s | 9,984 KiB |
| AIM generalized-Bol, packed | 101 | 1,972 | 107 | 102 | 49 / 251 | 84 | 1.61 s | 15,496 KiB |

The 10%-hint bounded run completed work in both rule families, performed
1,060,082 fair raw visits plus 372,295 discovery visits, kept the candidate
pool below 126 transient bodies, and reported zero replay and zero preview
false positives.  Its descriptor and consumed-record storage was about
124 KiB and 25 KiB respectively.  The older conservative 250-given artifact
reported 48 matched hints and only two completed descriptors; this is the
specific flat-hint/debt failure the new profile addresses.

A 30-second 500-given attempt stopped on its time limit as intended.  Reports
at 10 and 20 CPU seconds reached givens 176 and 255: the first interval added
252 distinct current hint matches and the next added seven, while `Hha`
selection continued.  The hint slope slowed but did not stop; no larger run
was attempted locally.

The full-hint input was deterministically recovered from the archived echoed
input (SHA-256
`d18adfc55494c56895ed5c15adbef8c09c3e3ca1619d7f49231759b70494eb60`).
Its 296 MiB peak is the same packed-hint preprocessing high-water already seen
in the earlier 100-given runs.  At termination allocator live/reserved memory
had fallen to 52.1/97.5 MB; the bounded scheduler itself did not recreate the
old passive-RAM peak.

### 5.3 Earlier frontier results at 250 givens

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

### 5.4 Checkpoint and proof evidence

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
- Native paramodulation and hyperresolution differential tests reproduce the
  eager raw sequence for budgets 1--64 and resume every continuation suffix.
- `P9COLLF` checkpoint/resume with a live promoted pool entry and live
  ahead-consumed ordinal reproduces terminal scheduler, preview, promotion,
  and fair-skip accounting with all integrity checks passing.

### 5.5 Better packed hint-index results

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

1. Discovery is deliberately bounded.  A hint deeper than the configured
   distance/raw schedule can still wait for fair enumeration; increasing
   lookahead is a CPU/search-policy tradeoff, not a RAM fix.
2. Preview is exact for its current epochs but advisory.  Later hint or
   simplifier changes can change a key, so stale entries are refreshed and
   every promotion is rematched authoritatively.
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
| `cc87d70` | Attribute inference debt and interval hint progress |
| `502e6c0` | Split rule lanes, fairness, and bounded admission |
| `b99df36` | Native resumable paramodulation iterators |
| `da8d1de` | Native resumable hyperresolution iterators |
| `7935082` | Bounded exact-preview candidate windows |
| `f1eb990` | Bounded dual-cursor hint discovery and checkpointing |

Important implementation and documentation files are:

- [`provers.src/search.c`](provers.src/search.c)
- [`provers.src/search-structures.h`](provers.src/search-structures.h)
- [`provers.src/cold_passive_store.c`](provers.src/cold_passive_store.c)
- [`provers.src/cold_passive_store.h`](provers.src/cold_passive_store.h)
- [`test.src/collective_frontier_test.sh`](test.src/collective_frontier_test.sh)
- [`test.src/collective_balanced_test.sh`](test.src/collective_balanced_test.sh)
- [`test.src/osborn_collective_controls.sh`](test.src/osborn_collective_controls.sh)
- [`test.src/discount_loop_test.sh`](test.src/discount_loop_test.sh)
- [`test.src/hint_index_trace_test.sh`](test.src/hint_index_trace_test.sh)
- [`test.src/hint_checkpoint_test.sh`](test.src/hint_checkpoint_test.sh)
- [`test.src/compact_otter_audit_test.sh`](test.src/compact_otter_audit_test.sh)
- [`test.src/compact_otter_checkpoint_test.sh`](test.src/compact_otter_checkpoint_test.sh)
- [`ladr/hint_postings.c`](ladr/hint_postings.c)
- [`ladr/hints.c`](ladr/hints.c)
- [`provers.src/compact_rewrite.c`](provers.src/compact_rewrite.c)
- [`provers.src/compact_unit_index.c`](provers.src/compact_unit_index.c)
- [`provers.src/compact_back_demod.c`](provers.src/compact_back_demod.c)
- [`provers.src/compact_feature_index.c`](provers.src/compact_feature_index.c)
- [`P9-BETTER-PACKED-PLAN.md`](P9-BETTER-PACKED-PLAN.md)
- [`P9-COLLECTIVE-SCHEDULER-PLAN.md`](P9-COLLECTIVE-SCHEDULER-PLAN.md)
- [`P9-COLLECTIVE-SCHEDULER-BASELINES.md`](P9-COLLECTIVE-SCHEDULER-BASELINES.md)
- [`P9-DISCOUNT-WALDMEISTER-PLAN.md`](P9-DISCOUNT-WALDMEISTER-PLAN.md)
- [`P9-MEMORY-RESULTS.md`](P9-MEMORY-RESULTS.md)
- [`P9-PHASE5-COMPACT-FRONTIER-PLAN.md`](P9-PHASE5-COMPACT-FRONTIER-PLAN.md)
- [`P9-PHASE5-ACCEPTANCE-AUDIT.md`](P9-PHASE5-ACCEPTANCE-AUDIT.md)
- [`Checkpoint-Format-Spec.txt`](Checkpoint-Format-Spec.txt)

## 9. Conclusion

The work does more than shrink clauses by a few percent.  It removes the
architectural requirement that millions of passive conclusions be resident,
fully materialized, and indexed.  The current exact-replay product closes the
whole-proof trajectory, restart, CPU, RSS, and accounting gates: it proves
Osborn at the accepted 2,945-given boundary with 76.85% less whole-process RAM
and slightly less user CPU than old P9.

For compatibility-sensitive equational work, eager compact OTTER is now the
measured default candidate; retain full/FPA OTTER as the raw-output control.
For much larger passive-dominated AIM searches, the DISCOUNT/collective mode
still offers the stronger asymptotic bound, and **80–95% less total RAM**
remains the planning range that those long production runs must validate.

## References

1. T. Hillenbrand, “Citius altius fortius: Lessons learned from the Theorem
   Prover Waldmeister,” 2003/2004, DOI 10.1016/S1571-0661(04)80649-2.
2. [`../Plans-RAM-24.txt`](../Plans-RAM-24.txt), archived project discussion and long-run statistics.
3. [`P9-MEMORY-RESULTS.md`](P9-MEMORY-RESULTS.md), bounded baseline and component measurements.
4. [`P9-DISCOUNT-WALDMEISTER-PLAN.md`](P9-DISCOUNT-WALDMEISTER-PLAN.md), design contract, implementation log,
   and acceptance gates.
5. [`P9-PHASE5-ACCEPTANCE-AUDIT.md`](P9-PHASE5-ACCEPTANCE-AUDIT.md), current-binary gate-by-gate evidence.
