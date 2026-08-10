# Phase 5: compact OTTER-compatible frontier

Date: 2026-08-10 (Europe/Berlin)

## Objective

Recover the historical Osborn proof trajectory and proof while retaining the
radical-memory representation developed on `packed-fast`.  This phase is not
another DISCOUNT scheduler experiment.  It preserves OTTER's eager treatment
of every retained clause while replacing pointer-rich passive bodies and
indexes with stable-ID compact structures.

The new mode is isolated behind the proposed configuration:

```text
assign(search_loop,otter).
assign(passive_store,dense).
assign(hint_index,packed_fast).
assign(inference_frontier,clauses).
assign(ancestor_store,mmap).
assign(sos_limit,-1).
set(back_demod_hints).
set(compact_otter_demodulation).
set(compact_otter_unit_index).
set(compact_otter_back_demod_index).
set(compact_otter_nonunit_index).
assign(compact_passive_cache,4).
```

`search_loop=otter` without all four authoritative compact flags remains the
unchanged compatibility reference.  OTTER `passive_store=dense` fails at
startup unless every pointer-free index is authoritative; it never silently
falls back to DISCOUNT timing or a resident passive index.

## Implementation and measured status (2026-08-10)

Stages 1--4 are implemented on `phase5-compact-frontier`.  Compact rewrite,
unit, back-demodulation, and nonunit indexes first passed differential audit
at 10, 100, and 300 givens and are now authoritative.  A retained OTTER
clause completes the ordinary eager backward transaction before its body is
archived.  The dense selector and every index retain stable IDs and compact
metadata only.  Exact probes decode an immutable body on demand; selection
activates that same archived proof record.

The ancestor archive is the sole owner of a cold passive body and its proof
data.  This avoids duplicating bodies between a passive arena and proof
archive.  A 4-way hot materialization cache is bounded by
`compact_passive_cache` MiB (default 4, zero disables it).  It is an
optimization only: entries are pinned during exact probes and invalidated
before selection or backward retirement.

The bounded gates use the same clauses, hints, and selector rules in both
columns:

| Boundary | Representation | User | Wall | Peak RSS | Live P9 allocation |
|---|---:|---:|---:|---:|---:|
| 300 | compact indexes, full bodies | 19.55 s | 22.53 s | 90,612 KiB | 20.76 MB |
| 300 | archive, cache disabled | 23.22 s | 26.92 s | 90,484 KiB | 16.43 MB |
| 300 | archive, 4 MiB cache | 19.32--20.34 s | 22.04--23.01 s | 90,636 KiB | 16.89 MB |
| 1,000 | compact indexes, full bodies | 118.56 s | 133.05 s | 123,236 KiB | 45.73 MB |
| 1,000 | archive, 4 MiB cache | 140.45 s | 156.25 s | 113,264 KiB | 19.17 MB |

All compared runs have identical given, generated, kept, usable, SOS,
demodulator, disabled, hint, and active-hint counts.  At 1,000 givens both
runs end at `Generated=1,268,285`, `Kept=33,909`, and `Sos=26,052`.  The
archive run is 1.185 times the compact/full-body CPU, within the 1.25 gate,
and is below the 125 MiB prefix RSS gate.  Its cache served 60,667 hits from
8,893 misses, peaked at about 2.03 MiB charged, and the archive reported zero
validation failures.

The 300-given component accounting explains why body eviction alone is not
the final radical reduction:

| Structure | Total | Dominant component |
|---|---:|---:|
| compact unit index | 3.57 MB | 2.62 MB trie nodes (73%) |
| compact back-demod index | 1.74 MB | 0.79 MB postings + 0.52 MB tokens |
| compact nonunit index | 0.56 MB | 0.52 MB trie nodes (93%) |

At 1,000 givens the rewrite, unit, and back-demod structures total about
43.4 MB, while the complete shared clause archive is only 8 MB.  These
indexes grow with the passive population, so extrapolating the current
representation to the historical 131,001-clause proof boundary would miss
the final 125 MiB target even though passive bodies are cold.

The first structural compression slice is now implemented.  Radix edges
preserve the former child and posting order, and the exact 300-given trace is
unchanged:

| Index | Before | After | Reduction |
|---|---:|---:|---:|
| unit | 3,573,496 B | 1,148,664 B | 67.9% |
| nonunit | 562,376 B | 189,664 B | 66.3% |
| rewrite | 1,775,880 B | 1,317,128 B | 25.8% |
| all four indexes, including unchanged back demod | 7,652,792 B | 4,396,496 B | 42.6% |

The rewrite node pool itself falls from 655,360 to 196,608 bytes (70.0%).
The optimized rewrite traversal uses one binding trail per query; allocating
a `MAX_VARS` trail in every recursive frame was measured and rejected because
it raised the 300-given user time to 23.44--24.40 seconds.  The shared-trail
version takes 18.51--20.08 seconds versus a controlled 19.46-second
token-trie run, so the accepted representation has no measured CPU penalty.
The 1,000-given compression gate remains pending until the back-demodulation
and shared-term slices below are present.

### Next radical index reduction

The next implementation slice is structural, not another cache-size tweak:

1. Add one hash-consed compact term pool shared by rewrite rules, unit
   matching, and passive redex occurrences.  Each normalized subterm is
   stored once; the three indexes retain 32-bit term IDs instead of private
   flattened token copies.
2. **Completed for the existing token arenas:** replace token-per-node unit
   and rewrite discrimination paths with radix edges.  Unary paths become one
   edge; child order and terminal posting order remain the audited legacy
   order.  Sharing those labels through item 1 remains outstanding.
3. **Completed:** radix-compress the fixed-length nonunit feature trie.  Its
   node pool fell by 96.1% at 300 givens without adding exact probes.
4. Group back-demod occurrences by `(symbol, clause ID)` and delta-pack the
   matching subterm IDs/offsets.  Keep the existing structural exact filter,
   but eliminate the current 12-byte posting per raw symbol occurrence.
5. Share a packed stable-ID directory across the indexes and compact inactive
   records at deterministic thresholds.  Rebuilds preserve result ordering
   and are checked against the 10/100/300 event oracle.
6. Reclaim immutable archive pages behind the bounded cache (or add a true
   file-I/O clause-store backend).  The old 13 GB deleted mmap retaining
   roughly 10 GB RSS demonstrates that logical archive bytes cannot be
   assumed nonresident.

The gate for this slice is at least a 70% reduction of combined rewrite,
unit, nonunit, and redex-index bytes at 1,000 givens, with the exact 300 trace
unchanged and 1,000-given CPU no worse than the current archive run.  That is
large enough to matter at the proof boundary; 5--10% container tuning is not
accepted as completion.

## Why selected DISCOUNT did not solve Osborn

At the old proof boundary OTTER has processed 2,945 givens, retains 131,001
SOS clauses, and proves the problem in 764.94 user seconds with 536.99 MiB of
Prover9-accounted memory.  The guarded packed-fast DISCOUNT run reaches Given
2,916 in 330.26 user seconds, but already retains 569,005 passives and has not
found the proof.  By Given 4,111 it has 1,046,419 passives and uses 198,112
KiB peak RSS.

Packed-fast has removed hint lookup as the cause: at the 1,000-given exact
DISCOUNT boundary it reduces ordinary/flipped hint time from 353.586 to 7.406
seconds without changing the selected clause.  The remaining divergence is
semantic timing.  OTTER eagerly factors, installs demodulators, performs
backward subsumption/demodulation/unit deletion, and exposes every retained
SOS clause to forward simplification before it is selected.  DISCOUNT delays
those operations until selection.  A hint count therefore cannot stand in
for the old proof trajectory.

## Compatibility contract

For each newly retained clause, compact OTTER performs the same transaction
and in the same order as the reference loop:

1. simplify, orient, merge, and apply unit/CAC simplification;
2. perform exact forward subsumption and unit conflict;
3. weigh and perform exact hint matching/degradation;
4. assign the same clause ID and append it to limbo;
5. factor and apply the other limbo generators;
6. perform backward subsumption;
7. if eligible, install it immediately as a demodulator and back-demodulate
   both active and passive clauses;
8. perform backward unit deletion/CAC processing;
9. insert the unchanged survivor into the given selector.

Only after this transaction may an SOS body become cold.  Selection restores
the exact body and metadata without a refresh/requeue step.  Therefore given
selection and inference ordering remain OTTER ordering rather than DISCOUNT
ordering.

Stable clause ID is the public identity.  No evictable term, literal,
`Topform`, or mmap address may be retained by a selector or compact index.
All candidate lists are ordered to reproduce the reference index's answer
order before an operation with visible effects is performed.

## Representation

### Active and transient clauses

Usable, limbo, the current given, and bounded query scratch retain ordinary
materialized clauses and the existing inference indexes.  The active set is
small on Osborn (1,800 clauses at the measured proof boundary), so replacing
it is neither necessary nor worth compatibility risk.

### Cold passive records

Each SOS survivor has an append-only body record and a compact mutable
sidecar.  The body record owns literals, attributes, and justification.  The
sidecar owns only selection/query metadata: stable ID, body position, exact
weight key, matching-hint ID, selector membership, liveness, and compact
feature roots.  Fields needed only for reports are read from the body header
instead of duplicated in every resident sidecar.

The body arena gets a true file-I/O backend.  The current writable `MAP_SHARED`
arena makes every appended page resident in the process and explains much of
the 198-MiB guarded peak.  The file backend uses bounded write/read buffers and
`pwrite`/`pread`; kernel page cache is not charged as process RSS.  The mmap
ancestor store remains separate and proof-complete.

### Compact exact indexes

The implementation is split so each structure can be differentially tested:

- a packed unit index retrieves forward subsumers, backward subsumees, unit
  conflicts, and unit-deletion candidates using structural postings followed
  by allocation-free exact compressed matching;
- a compact nonunit feature index stores stable IDs and materializes only the
  conservative candidate set for the existing exact subsumption test;
- the existing compact rewrite trie stores demodulator sides in reference
  insertion order and returns the same first applicable rule as the ordinary
  demodulation index;
- a compact redex occurrence index maps conservative symbol/path features to
  passive IDs for back demodulation, followed by the ordinary exact rewrite
  eligibility check on a materialized candidate;
- selector heaps contain compact record ordinals, never clause pointers.

Tombstones preserve stable ordinals.  Rebuilds/compactions may change physical
positions only after all ID-based indexes and selector entries have been
relocated atomically.  A bounded materialization cache is an optimization,
not an owner; eviction cannot change semantics.

## Implementation stages

### Stage 0: frozen trajectory oracle and accounting

Add a compact search-event trace covering retained IDs/body hashes, selector
keys, given IDs, demodulator admissions, backward rewrites, subsumption
deactivations, and hint identity.  Freeze reference traces at 10, 100, and
300 givens.  Report resident bytes separately for sidecars, selector heaps,
unit/nonunit/redex indexes, rewrite trie, body buffers, and file logical and
physical bytes.

### Stage 1: truly cold file backend

Add a file-backed cold passive arena and validate byte-identical
archive/materialize/clone behavior against memory and mmap backends.  First
use it with selected DISCOUNT to isolate RSS impact; this stage is not claimed
to fix the proof trajectory.

### Stage 2: exact compact demodulator bank

Run the ordinary OTTER transaction with the compact rewrite trie while full
passive bodies still exist.  Differentially compare every rewrite rule ID,
rewritten body, and back-demodulation wave.  Fix ordering or representation
until the 300-given reference trace is identical before cold eviction is
enabled.

### Stage 3: stable-ID compact subsumption and redex indexes

Introduce the unit index first, because Osborn is dominated by unit
equations, then the nonunit and redex indexes.  During this stage reference
and compact indexes run together in audit mode and their ordered exact result
IDs must agree.  Only after agreement may the passive pointer index be
removed.

### Stage 4: archive OTTER SOS bodies

Enable `compact_otter`: complete eager processing, publish the survivor to
all compact indexes and selectors, archive the body, and release its term
tree.  Selection and every backward mutation deactivate the stable ID before
materialization so no stale candidate can be observed.  Add checkpoint
serialization only after uninterrupted traces agree.

### Stage 5: bounded and proof runs

Run 10 and 100 givens first, then the exact 300-given gate.  Continue to
1,000 only after exactness and RAM gates pass.  The final measurement is a
proof run with the same clauses, hints, selection rules, and limits as the
old OTTER reference.

## Acceptance gates

### Correctness and trajectory

- Existing proof, hint, iterator, selector, rewrite, ancestor, and checkpoint
  tests remain green in their default modes.
- Reference and compact search-event traces are byte-identical at 10, 100,
  and 300 givens.
- Given IDs, generated/kept counts, SOS/usable/demodulator/disabled counts,
  hint IDs and degradation, and all backward rewrite/subsumption events agree.
- Checkpoint/resume produces the same continuation trace as uninterrupted
  compact OTTER.
- The final proof is accepted by `prooftrans`; use `directproof` where its
  supported inference subset permits.

### CPU and memory

- At 300 givens, total user CPU is at most 1.25 times packed-fast selected
  DISCOUNT's 10.23 seconds as a stretch target and never slower than current
  FPA OTTER's approximately 28--35 seconds.
- At 1,000 givens, total CPU is no more than 1.25 times the reference OTTER
  prefix and peak RSS is at most 125 MiB.
- The final run must produce the old proof with user CPU at most 957 seconds
  and wall time at most 18 minutes on the measured machine.
- Final peak RSS is at most 125 MiB, with 115 MiB as the stretch target.
- Accounting must explain at least 95% of resident compact-frontier bytes;
  logical file size is reported separately and is never called RAM.

A failed trace gate means the compact operation is not enabled.  A faster
prefix with a different trajectory, or a low RSS run without the proof, does
not complete Phase 5.

## Commit discipline

Commit the plan, file backend, each compact index, compatibility integration,
checkpoint support, and measured report separately.  Commit messages include
ownership invariants, ordering decisions, test evidence, and measured
tradeoffs so later reviewers can audit the implementation without relying on
this conversation.
