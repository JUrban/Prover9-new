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
| 1,000 | archive, shared clause term pool | 142.07 s | 156.66 s | 94,652 KiB | 19.17 MB |
| 1,000 | archive, delta back-posting stream | 126.12 s | 140.04 s | 93,524 KiB | 19.17 MB |
| 1,000 | archive, delta rewrite-occurrence stream | 125.53 s | 138.90 s | 92,164 KiB | 19.17 MB |

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

The first structural compression slices are now implemented.  Radix edges
preserve the former child and posting order, and the exact 300-given trace is
unchanged:

| Index | Before | After | Reduction |
|---|---:|---:|---:|
| unit | 3,573,496 B | 1,148,664 B | 67.9% |
| nonunit | 562,376 B | 189,664 B | 66.3% |
| rewrite | 1,775,880 B | 1,317,128 B | 25.8% |
| back demod | 1,741,040 B | 1,282,296 B | 26.4% |
| all four indexes | 7,652,792 B | 3,937,752 B | 48.5% |
| all four plus shared clause term pool | 7,652,792 B | 3,020,360 B | 60.5% |
| plus delta back-posting stream | 7,652,792 B | 2,760,368 B | 63.9% |
| plus delta rewrite-occurrence stream | 7,652,792 B | 2,663,112 B | 65.2% |

The rewrite node pool itself falls from 655,360 to 196,608 bytes (70.0%).
The optimized rewrite traversal uses one binding trail per query; allocating
a `MAX_VARS` trail in every recursive frame was measured and rejected because
it raised the 300-given user time to 23.44--24.40 seconds.  The shared-trail
version takes 18.51--20.08 seconds versus a controlled 19.46-second
token-trie run, so the accepted representation has no measured CPU penalty.
The grouped back-demodulation representation turns 53,028 raw symbol
occurrences into 25,069 `(symbol, clause ID)` groups and a 53,028-byte
delta-varint offset stream.  A same-host control took 18.52 user seconds and
the grouped run took 18.47 seconds.

Rewrite, unit, and back-demodulation terms now use one clause-coalescing term
pool.  A clause is serialized once, and every later request from another
index returns a stable slice in that serialization.  At 300 givens, 26,855
term requests require only 5,737 physical clause serializations; the pool
reuses 222,332 tokens and occupies 655,464 bytes.  The exact CHAT/Osborn
boundary remains `Given=301`, `Generated=120,793`, and `Kept=5,737`.

The 1,000-given gate exposes the remaining work.  Before these structural
changes, the four indexes occupied 44,545,464 bytes.  Their compact metadata
plus the shared pool now occupy 20,248,136 bytes, a **54.5%** reduction rather
than the required 70%.  User CPU is 142.07 seconds versus 140.45 seconds for
the pre-structural archive run (+1.2%), while peak RSS falls from 113,264 KiB
to 94,652 KiB (-16.4%).  Search state is exact at `Given=1001`,
`Generated=1,268,285`, `Kept=33,909`, `Usable=949`, `Sos=26,052`,
`Demods=21,741`, and `Disabled=6,937`.

The pool currently removes duplication among indexes for the same clause; it
does not yet hash-cons equal subterms across different clauses.  Its
1,000-given logical content is 728,510 32-bit tokens, but power-of-two backing
holds 1,048,576 tokens and its proof-ID directory uses another 1,048,576
bytes.  This capacity slack, per-index stable-record tables, and repeated
posting metadata are now larger targets than private token copies.

Exact opt-in instrumentation (`set(compact_term_sharing_stats)`) now bounds
the cross-clause opportunity instead of assuming it.  At 300 givens, 115,486
subterm occurrences contain 16,185 unique terms; at 1,000 givens, 728,510
occurrences contain 91,686 unique terms (87.4% duplicate occurrences).  A
variable-record canonical DAG whose term ID is its word offset would need
1,304,344 logical bytes for symbol words, child IDs, and clause atom roots at
1,000 givens, versus the current 4,194,304-byte token capacity.  This
2,889,960-byte difference is an upper bound, not a promised saving: a live
production hash-cons table, fingerprints, and arena slack must be included.
The diagnostic table itself is reported separately and is never enabled in
ordinary CPU/RSS measurements.

The other immediate large target is the redex posting directory.  Its 152,909
logical `(symbol, clause)` groups occupy 3,672,064 bytes at 1,000 givens
because each group is a 12-byte linked record in a doubled array.  A
per-symbol block stream can delta-varint the monotonically increasing record
and occurrence positions while retaining insertion traversal.  This avoids
coupling the first production reduction to the much broader DAG matcher
rewrite.

That stream is now implemented and accepted.  At 300 givens its logical
75,654-byte posting stream occupies 131,072 bytes of blocks; complete
back-demod storage falls from 758,016 to 497,952 bytes, and all 132,267 CHAT
candidate/hint/kept/given trace lines remain byte-identical.  At 1,000 givens
the 152,909 posting groups encode into 461,366 logical bytes and 524,288
allocated block bytes.  Complete back-demod storage falls from 6,033,664 to
3,414,304 bytes (-43.4%), making the four indexes plus shared pool
17,628,848 bytes: **60.4% below** the pre-structural 44,545,464 bytes.  The
exact gate takes 126.12 user seconds and 93,524 KiB peak RSS, improvements
over the preceding shared-pool run's 142.07 seconds and 94,652 KiB.

Rewrite overlap occurrences now use the same per-symbol block principle, with
one monotone varint rule delta per entry.  At 300 givens the 16,174 logical
bytes occupy 32,768 block bytes, the complete rewrite bank falls from 792,840
to 695,584 bytes, and the full 132,267-line CHAT trace remains identical.  At
1,000 givens the stream is 108,440 logical and 131,072 allocated bytes; the
rewrite bank falls from 4,856,072 to 3,939,616 bytes (-18.9%).  Combined
structural storage is now 16,712,392 bytes, **62.5% below** the pre-structural
baseline.  The gate remains exact at 125.53 user seconds and 92,164 KiB peak
RSS.

### Next radical index reduction

The next implementation slice is structural, not another cache-size tweak:

1. **First layer completed:** rewrite rules, unit matching, and passive redex
   occurrences share one clause serialization and retain 32-bit offsets
   instead of private flattened token copies.  Cross-clause hash-consing of
   equal subterms remains to be implemented and measured; its hash/directory
   overhead is included in the reported total.
2. **Completed for the existing token arenas:** replace token-per-node unit
   and rewrite discrimination paths with radix edges.  Unary paths become one
   edge; child order and terminal posting order remain the audited legacy
   order.  Sharing those labels through item 1 remains outstanding.
3. **Completed:** radix-compress the fixed-length nonunit feature trie.  Its
   node pool fell by 96.1% at 300 givens without adding exact probes.
4. **Completed:** group back-demod occurrences by `(symbol, clause ID)` and
   delta-pack matching subterm offsets.  The existing structural exact filter
   remains, while the 12-byte posting per raw symbol occurrence and the
   redundant per-argument root directory are gone.
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
