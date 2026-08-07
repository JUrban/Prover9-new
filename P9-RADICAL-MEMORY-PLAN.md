# Prover9 radical-memory architecture plan

Date: 2026-08-07 (Europe/Berlin)

This plan deliberately targets an 80--90% reduction in the resident memory
of large AIM searches.  It is not a list of another five-percent worth of
structure packing.  The measurements below show that reaching the target
requires changing how Prover9 represents indexes, terms, and the passive
search frontier together.

## What the new measurement says

The pre-project revision is `b36df4c06d5794dd98e42335f191dd41d7abd2ab`.
It and the Phase 6 candidate were built with GCC 13.3.0, `-O2 -Wall`, and run
on exactly the same `Ka_to_aK1.in` prefix:

- `max_given=2000`, `max_seconds=180`, external `timeout 210`;
- no proof printing, `stats=all`;
- input SHA-256
  `8820c56718929e0f850e51317abac1beab8c8febdb0518e73f3a247921885bdd`;
- both executions stopped at given/generated/kept =
  2,001/6,980,123/2,198, with usable/SOS/disabled = 2,000/196/29.

| Build/mode | Logical live allocation | Peak RSS | User / wall |
| --- | ---: | ---: | ---: |
| Pre-project base | 11,230,432 B | 14,720 KiB | 71.47 / 71.65 s |
| Phase 6 candidate, default-compatible | 10,717,376 B | 14,720 KiB | 69.49 / 69.95 s |
| Phase 6 candidate, mmap ancestor archive | 10,709,688 B | 14,720 KiB | 72.93 / 73.34 s |

The current representation therefore saves 513,056 logical live bytes
(4.57%) in the default-compatible comparison, but saves **zero measured peak
RSS** on this substantial run.  The ancestor archive cannot help materially
because only 29 clauses are disabled.  This is the expected failure mode for
small optimizations: the live search state is dominated by active indexes and
terms, not cold proof ancestors.

The pre-project `stats=all` table allocates approximately:

| Live structure | KiB | Share of 11,230,432 B |
| --- | ---: | ---: |
| FPA trie nodes | 2,817.7 | 25.7% |
| FPA chunk headers | 1,280.7 | 11.7% |
| FPA list headers | 552.1 | 5.0% |
| FPA chunk pointer arrays | 1,581.1 | 14.4% |
| **FPA subtotal** | **6,231.6** | **56.8%** |
| Terms plus argument arrays | 2,565.9 | 23.4% |
| Discrimination nodes | 659.8 | 6.0% |
| Topforms | 510.9 | 4.7% |
| Clause-list positions | 220.3 | 2.0% |

The compact Phase 5 term header lowers the current term subtotal to about
2,118.5 KiB, but the FPA subtotal is unchanged.  The FPA estimate also omits
its separately `malloc`-allocated child hash tables, so 56.8% is a lower
bound.  The primary target is consequently the complete indexing
representation, followed by the persistent term representation.

Peak RSS cannot fall by 80% on this particular 14,720-KiB process: an x2
smoke process already occupies about 3,328 KiB, while an 80% target would be
2,944 KiB.  The meaningful acceptance target is 80--90% on large retained
states where the fixed executable/libc cost is small, together with a
separate target for workload-incremental bytes on medium cases.

## What the archived AIM statistics say

There are 169 `.out` files containing a final `Megabytes=` statistic.  This
is the old monotonic palloc quantity, not independently measured process RSS,
but it is a useful view of the scale and retained-state growth:

- median 62.34 MB, 90th percentile 95.48 MB, maximum 921.92 MB;
- 106 of 169 are at least 50 MB and 11 are at least 100 MB;
- ordinary large cases combine tens of thousands of usable/SOS clauses with
  tens of thousands of hints.  For example `aK2_eq_aK3_a.out` reports
  109.33 MB, 23,067 usable, 2,236 SOS, and 33,113 hints;
- the 921.92-MB outlier `MBOL/aa1sq4_to_nil3.out` reports 376,065 kept,
  227,634 SOS, 168,327 demodulators, and 146,320 disabled clauses after only
  1,861 given clauses.  It is the clearest evidence that retaining every
  passive conclusion as a full clause/index participant is the architectural
  problem.

Compressing disabled bodies addresses the 146,320 cold clauses in the
outlier, but it does not address the 227,634 passive clauses or the 168,327
live simplifiers.  Likewise, shrinking a `Topform` by a word cannot turn a
roughly quadratic frontier into a small resident state.

## Lessons taken from Waldmeister

Hillenbrand's [*Citius altius fortius: Lessons learned from the Theorem Prover
Waldmeister*](../Citius_altius_fortius_Lessons_learned_from_the_The.pdf)
supplies three directly applicable ideas:

1. Its perfect discrimination trees normalize variables, store fixed child
   sets in arrays rather than linked lists, and collapse every unary tail.
   Compactness improves both memory and cache behavior.
2. It strictly separates active facts from passive facts.  More importantly,
   it represents a whole set of critical pairs generated from one newly
   activated equation by a constant-size descriptor plus a fixed cache of
   promising individual pairs.  This changed passive-space growth from
   quadratic to linear.  Completeness requires retaining access to historical
   normal-form functions.
3. Theory-aware redundancy can prevent facts from entering the expensive
   inference state.  A redundant equation can still be retained as a cheap
   simplifier while being excluded from critical-pair generation.  The paper
   also uses successor sets for conjectures so some searches finish before a
   large saturation is built.

Prover9 cannot copy these details mechanically: it supports general clauses,
several inference rules, hint-adjusted weights, and an Otter-style passive set
that participates in simplification indexes.  The ideas nevertheless point
to the necessary system boundary: immutable compact active indexes, an
unindexed/collective passive frontier, and versioned simplification state.

## Proposed architecture

### 1. Replace pointer-rich FPA with packed immutable index segments

Build a new index around stable 32-bit occurrence IDs rather than `Term *`.
Each segment is a variable-normalized perfect discrimination trie with:

- packed symbol/child-range arrays instead of 48-byte nodes and sibling
  pointers;
- path compression for unary subtrees;
- direct child arrays for dense symbol ranges and compact sorted pairs for
  sparse ranges;
- postings stored as sorted 32-bit IDs, frame-of-reference/delta encoded in
  small blocks instead of 40-byte chunk headers plus 64-bit pointer arrays;
- a small mutable delta segment and tombstone bitmap, periodically merged
  into immutable segments.  Immutability is what makes Waldmeister-style
  fixed arrays practical during a dynamic Prover9 search.

The retrieval API must preserve FPA-ID order exactly in the compatibility
mode.  On the measured prefix, a credible first gate is reducing the
6.23-MiB FPA subtotal to 0.8--1.3 MiB.  Even the favorable end saves only
about half of total live bytes, so this is necessary but not sufficient.

### 2. Publish kept terms into an immutable hash-consed term bank

Do not attempt to share the current mutable `struct term`.  Split it into:

- an immutable canonical node keyed by `(symbol, child IDs)` with 32-bit
  handles; and
- clause/occurrence sidecars for mutable container links, atom flags,
  maximal/selected state, and any indexing ID.

Variables are clause-normalized before publication.  Generated candidates
live first in a short-lived bump arena; rejected candidates disappear by
resetting the arena.  Only kept clauses are hash-consed into the persistent
bank.  This avoids performing global hash-table work for the hundreds of
millions of transient term allocations observed in the measured run.

Reference counts are a poor fit for shared DAGs under mass deletion.  Use
epoch/bank ownership and periodic reachability collection from active,
passive, hint, and proof roots.  Clause serialization and proof output resolve
handles through the bank.  The current objections to term interning—mutable
flags, `container`, and FPA IDs—are handled by the sidecar split rather than
ignored.

Before committing to the migration, add a read-only census reporting term
occurrences, unique canonical nodes, fanout, and sharing separately for
usable, SOS, demodulators, hints, and transient kept candidates.  The gate is
at least a 4:1 retained occurrence/unique-node ratio on the large AIM suite;
otherwise the bank still wins on 32-bit child IDs but not enough to justify
the full migration by itself.

### 3. Stop materializing and indexing the whole passive frontier

This is the step that makes an 80--90% result plausible on the 921.92-MB
class of searches.

Introduce a new, explicitly different `discount_frontier` search mode:

- only active facts participate in inference and simplification indexes;
- passive clauses are compact metadata plus term-bank root handles and proof
  parent IDs, stored in append-only compressed/mmap pages;
- selection queues hold small records, not `Topform *` graphs;
- cold pages are evictable and materialized into a bounded cache only for
  selection, printing, or proof reconstruction.

Then implement the stronger Waldmeister-style form: store resumable inference
batch descriptors (parent IDs, rule/position cursors, simplifier epoch, and a
lower bound on selection weight) instead of every generated conclusion.  A
fixed promising-candidate cache contains individually simplified clauses.
The scheduler expands batches until it can prove that the next cached
candidate precedes every unexpanded batch, or uses a fair interleaving when a
tight weight bound is unavailable.

Historical demodulator/index epochs must remain accessible so deferred
conclusions are normalized under the proof procedure specified for this mode.
That requirement is not optional: the Waldmeister paper calls out the same
completeness issue.  For Prover9, the formal work is to define whether a
deferred conclusion is normalized at generation epoch or selection epoch and
prove fairness/completeness for that choice.

This mode will not preserve the old given-clause sequence, because the old
SOS is itself indexed and participates in simplification.  It should be judged
by proof validity, calculus completeness, and bounded-suite proof coverage,
not by byte-identical search traces.  The old mode remains available until the
new procedure passes those gates.

### 4. Put hints and remaining indexes on the same shared substrate

Hints account for 1,268,568 body bytes even in the measured medium prefix and
reach 33,113 entries in a 109.33-MB archived case.  Store their bodies as term
bank roots, their mutable match/degradation fields in dense arrays, and their
matching index as another packed immutable segment.  Expired hints then drop
an ID/range rather than a graph of terms and index nodes.

Convert discrimination indexes to the same packed/path-compressed segment
format.  Dense clause IDs and literal/position ordinals replace container
pointers.  Once all indexes use IDs, clauses no longer need to keep address
stable and cold-store eviction becomes straightforward.

### 5. Reduce the state, not just its encoding

After the exact representation work is stable, add optional complete
redundancy modules for domains where they apply:

- canonical AC/C representatives or ground-joinability checks;
- retain an expensive-to-prove redundant clause as a demodulator but mark it
  `no_inferences`, following Waldmeister's distinction;
- successor-set goal processing for ground/equational conjectures;
- domain-specific scheduling selected from detected algebraic laws.

These can multiply the representation gains by preventing index/frontier
entries.  They must be reported separately because they change explored
search and may help or hurt a particular proof even when complete.

## Delivery order and hard gates

1. **Census and accounting.** Add exact byte counters for FPA child hashes,
   posting terms/capacity, unique canonical subterms, passive bodies, queue
   metadata, and resident mmap pages.  Reconstruct or replace the large
   921.92-MB input with a checked-in bounded stress case.
2. **Packed FPA prototype.** Run old/new indexes side by side, compare every
   answer stream and deletion result, then make the packed index selectable.
   Gate: at least 75% FPA-byte reduction and no more than 10% throughput loss
   on the AIM suite.
3. **Term bank and transient arena.** Differentially serialize every kept
   clause and validate all proof outputs.  Gate: at least 70% reduction in
   retained term/argument bytes on large AIM, with no soundness change.
4. **ID-based hints/discrimination and cold passive store.** Gate: identical
   candidate sets and proofs in compatibility mode; bounded materialization
   cache; no raw pointers into evictable storage.
5. **Collective frontier mode.** Specify epoch semantics and fairness first,
   then implement batch descriptors and the promising cache.  Gate: proof
   verification on the full regression corpus, checkpoint/resume equivalence
   within the new mode, and no unexplained loss of previously solved AIM
   problems under matched resource limits.
6. **Radical acceptance test.** On at least three searches whose old peak RSS
   exceeds 100 MB, require 80% lower externally measured peak RSS; the stretch
   gate is 90%.  Also require allocator live/reserved and index/term/frontier
   component totals to explain at least 95% of the RSS delta.  Medium cases
   use workload-incremental RSS because fixed process overhead makes an 80%
   total reduction mathematically impossible.

The packed index plus term bank can plausibly approach a 70--80% logical-live
reduction on the measured medium state.  The full 80--90% target on large AIM
states depends on the collective passive frontier: without removing hundreds
of thousands of individually materialized/indexed passive facts, the target
is not credible.
