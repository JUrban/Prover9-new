# Terminal proof lifecycle: structural repair design and implementation

## Implementation status

The design below is implemented by six reviewable commits:

- `9c3d48a` changes the inference-result callback contract and makes every
  eager/bounded producer unwind on cancellation;
- `d81e56b` defers terminal proof work to search safe points and transfers a
  deep, closed proof snapshot to output/results;
- `4f2d287` adds direct, cached, checkpointed, closure, and producer-restart
  regressions;
- `9878358` preserves the formula/clause `Topform` variant throughout copied
  and expanded result DAGs; and
- `1d443bd` exercises expanded results, destruction, hint derivation, and a
  subsequent autosketch child search; and
- `da4abdc` makes resident/archive sign queries respect the formula/clause
  `Topform` variant and adds mixed-ancestor terminal regressions.

Release and ASan+UBSan builds pass the focused matrix and the broader compact,
DISCOUNT, collective, checkpoint, generalization, scaling, memory, LADR, and
TPTP suites.  The first exact mature Josef 02 replay matched all 13,006 givens
but found a formula-as-literals bug in the new metadata-only denial walker;
`da4abdc` repairs that boundary.  A post-`da4abdc` exact replay remains the
last acceptance gate and is intentionally not replaced by the smaller tests.

## Problem statement

The compact OTTER search can discover a terminal empty clause while it is
inside an inference callback and, more specifically, while an archive-backed
unit candidate is materialized and pinned.  The pre-fix callback immediately
froze statistics, destroyed compact passive and inference indexes, expanded
the proof, and `longjmp`ed out of the search.

That ordering violates three ownership rules:

1. an inference producer still owns retrieval positions, substitutions,
   traversal state, and temporary terms until its callback returns;
2. the compact unit-conflict caller still owns a pin on the materialized
   passive candidate until `release_compact_index_clause()` runs; and
3. the ancestor archive still owns proof parents until a complete,
   independent proof snapshot has been constructed.

The supplied Josef 02 runs reproduce this at one deterministic search
boundary: all three trajectories contain the same 13,006 givens and then
receive `SIGSEGV`.  A focused cold-passive conflict reproducer makes the
ownership violation directly observable: a nonzero passive cache aborts when
terminal teardown tries to evict the pinned unit; a zero cache avoids that
check but prints an open proof whose empty clause cites an omitted archived
parent.  The latter is the production Josef configuration.

This is not a hint-count or Josef-symbol problem.  It is an invalid nested
callback/owner transition.

The ownership repair's first mature replay exposed one additional invariant.
Proof ancestry is heterogeneous: original non-clausal formula premises and
derived clauses share a `Topform` union.  Metadata-only denial discovery must
treat formulas as known non-clauses, never as literal lists.  Violating that
rule faults before safe-point snapshotting and is independent of producer
cancellation.

## Required invariants

### Consumer cancellation is explicit

Every inference-result callback returns a Boolean:

- `TRUE`: the consumer accepted the result and enumeration may continue;
- `FALSE`: the consumer accepted or disposed of the current result and asks
  the producer to stop.

Binary resolution, hyperresolution, UR resolution, factoring,
paramodulation, unit conflict, and their bounded iterators must propagate a
false result.  Before returning, each producer must cancel every live index
retrieval, undo substitutions/trails, release contexts, destroy temporary
flips and position lists, and leave resumable iterators in an explicitly
complete/cancelled state.

### Proof discovery only requests termination

The proof callback may assign and register the empty clause, apply denial
reuse, mark parents, and increment the proof count.  At the terminal proof it
sets `Terminal_proof_pending` and returns `FALSE`.  It must not destroy an
index, materialize an ancestor DAG, print a proof, or `longjmp` while nested
inside an inference producer.

If a newly inferred clause participates in a unit conflict before it enters
a search container, that clause is transferred to a small terminal-transient
owner list.  Its official ID and ancestry remain valid until proof snapshot
construction; it is deleted afterward.

### Destruction occurs only at a search safe point

The main search/preprocessing loop handles the pending terminal after all
producers and direct callers have unwound.  The safe point verifies that no
inference query is active and that the compact passive cache has no pins.
It then:

1. freezes final statistics while all reporting structures are live;
2. reconstructs every completed proof from the live ID/archive namespace;
3. validates proof closure (every justification parent occurs in the DAG);
4. deep-copies each DAG into a self-contained result snapshot;
5. releases temporary archive materializations and terminal transients;
6. destroys compact search indexes and packed hint indexes;
7. prints/runs proof actions from the closed snapshot; and
8. exits the search only from that safe point.

### Ancestor metadata respects the body variant

Every sign query checks the `Topform` kind before inspecting its body.  A
formula ID is known but never denotes a negative clause.  Persistent ancestor
records apply the same rule, with the formula bit taking precedence over a
negative bit even in older records.  This lets metadata-only proof walks
traverse mixed formula/clause ancestry without materializing bodies or
crossing the union arm.

`collect_prover_results()` transfers those snapshots.  It must never query a
destroyed ancestor archive.

## Proof snapshot requirements

A snapshot owns a fresh Topform, body, justification, and attributes for
every proof step.  It preserves IDs, term flags, formula bodies, weights,
normal-variable state, input/given/goal metadata, semantics, and stable hint
annotations needed by proof output.  It has no search-container, official-ID,
archive-materialization, selector, or compact-index ownership.

Proof DAGs are intentionally heterogeneous.  Non-clausal formula premises
must be copied through the Formula arm of the `Topform` union, while inference
clauses use the Literals arm.  Expanded-proof replay applies only to clause
nodes.  Consumers with a clause-only contract, such as the autosketch hint
bank, filter formula nodes at their boundary instead of weakening the proof
result representation.

Closure validation uses the sorted proof IDs and binary lookup rather than a
bitmap sized by the maximum clause ID; this keeps validation proportional to
proof size even after tens of millions of generated clauses.

## Regression matrix

The repair is not accepted on a single Josef replay.  Tests must cover:

- a terminal conflict against a cold archive-backed passive with cache zero;
- the same conflict with a nonzero cache, proving the pin is released;
- an explicit empty result from paramodulation/resolution;
- cancel-after-first-result behavior for bounded and unbounded inference
  producers, including cleanup under ASan/UBSan;
- proof closure and presence of both unit-conflict parents;
- native, expanded, and `prooftrans parents_only` output;
- expanded results containing formula premises, followed by destruction and
  another child search;
- resident and archived atomic-formula sign queries through memory, mmap, and
  file stores, plus a terminal proof with a compound formula premise;
- preprocessing and main-loop terminal proofs;
- checkpoint/resume followed by the same terminal conflict; and
- the existing compact OTTER, collective, eager-demodulation, hint,
  checkpoint, and component suites.

The mature Josef 02 replay remains the end-to-end acceptance test.  Josef 01
is a second terminal-boundary check, and Josef 03 protects the independent
match-once retirement repair.

## Non-goals

The repair will not special-case given 13,006, a Josef input fingerprint, a
particular hint bank, cache size, or index strategy.  Disabling proof output,
turning off compact unit conflict, retaining one lucky archive parent, or
delaying teardown by an arbitrary number of callbacks would only hide the
ownership error and is not an acceptable solution.
