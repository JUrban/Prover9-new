# Target-directed generalized-hash inference prototype

Date: 2026-08-25

Branch: `target-directed-inference`

Status: implemented experimental prototype; **failed the Josef_04 CPU
stop/go gate and is not recommended for a long run**

## Outcome

The branch implements the reviewed second-stage experiment rather than only
the lazy conclusion-allocation gate.  It can reconstruct every unique target
stored in the generalized-hint hash, builds a bounded structural sidecar,
derives possible unit-paramodulation parent pairs by reverse narrowing, and
checks every emitted conclusion with the existing virtual hash gate and
ordinary Prover9 clause processor.

The implementation found an important negative result.  Josef_04's roughly
9.8 million generalized targets are so broad that target-first reverse
narrowing performs far more work than enumerating actual active-parent
inferences.  Compact storage fixes the first RAM failure, but it cannot fix
the algorithmic fan-out.  The prototype is useful for bounded measurements
and as reusable infrastructure; it is not the next long-run configuration.

The practical result from the new 3,000-given files remains the Stage-1
`safe` lazy gate.  It preserves the tested search and is effectively as fast
as the incomplete `hit_only` ceiling on this prefix.

## What was implemented

The generalized hash now retains an eight-byte reconstruction recipe for
each occupied target.  Exact, completely generalized short-hint, and bounded
one-hole long-hint entries can be reconstructed without retaining pointers to
temporary parsed hint terms.

The immutable target sidecar contains:

- one posting per distinct rigid root occurring in a target;
- one conservative 64-bit structural signature per target, covering rigid
  parent/child features;
- compact recipe IDs rather than clauses or term pointers;
- a bounded reusable query object and complete byte/candidate statistics.

Signature collisions can only admit extra candidates.  They cannot reject a
compatible target, because the same feature sets the same bits at build and
query time.

For a newly selected positive unit equality, the reverse planner examines
eligible equality directions, retrieves targets compatible with the result
side, reverses each possible target position, and queries active units for
the required parent.  Partner arrays are deduplicated and sorted by proof ID.
Shadow mode compares those selected pairs with ordinary supported hash-hit
paramodulants.

The symmetric shadow directory uses the existing serialized compact unit
code tree rather than allocated `Term` trees.  Parent deletion removes its
requirements; stale code records and the private serialized-term pool are
rebuilt together; synthetic IDs are reused only after stale directory entries
are gone.  This prevents cumulative ID/token growth.

The authoritative `targeted_only_unit_paramod` experiment is deliberately
more incomplete but RAM-bounded:

- it handles the direction in which the newly selected equality is the
  rewriting (`from`) parent;
- it omits the symmetric historical direction instead of retaining millions
  of future requirements;
- it omits variable replacement-side directions and unhinted unit results;
- it keeps nonunit and negative partners on ordinary Prover9 paths;
- it drives selected positive-unit pairs directly rather than scanning the
  complete positive-unit Usable list;
- every emitted result is still an ordinary sound Prover9 inference with an
  ordinary justification and normal clause processing.

`fair_unit_paramod` remains reserved and now stops with a configuration error.
There is no disguised full-enumeration implementation behind that name.

## The new 3,000-given result

The supplied files are a clean comparison:

| Measurement | `lazy-safe-3k` | `lazy-hit-only-3k` |
|---|---:|---:|
| User CPU | 124.46 s | 124.36 s |
| Search start | 51.05 s | 51.02 s |
| Approx. search CPU | 73.41 s | 73.34 s |
| Peak RSS | 2,041,088 KiB | 2,040,192 KiB |
| Generated | 20,522,821 | 20,522,821 |
| Kept | 10,452 | 10,452 |
| Supported virtual results | 10,201,601 | 10,201,601 |
| Virtual hits | 23,804 | 23,804 |
| Conservative miss fallbacks | 2,047 | 0 |

The normalized 3,000-given streams are identical.  `safe` certifies and skips
10,175,596 misses (96.48% of all successful paramodulation candidates and
99.98% of supported virtual misses); `hit_only` skips only 2,046 more.  The
0.10-second total CPU difference is noise-sized.  Therefore the
safe certificate is not causing the remaining slowdown, and `safe` is the
right Stage-1 mode.

Against the earlier `Josef_04.hash-3k` result (145.21 user seconds), `safe`
reduces total CPU by about 14.3%.  Subtracting the reported search start gives
about a 21.9% search-phase reduction.  Peak RSS is unchanged because the
roughly 1.8-million-hint startup bank dominates it; lazy gating removes
cumulative temporary allocation traffic, not that resident bank.

## Representation measurements

On a mixed CHAT sample retaining 1,000 formulas from each of two hint blocks,
20 shadow givens produced 56,016 live reverse requirements.  The final compact
layout used 16,563,832 bytes for those requirements and preserved all 19
supported ordinary hash hits.  The rejected first layout needed roughly
36.8 MB for pointer metadata alone and roughly 73 MB including allocated term
trees.  The compact code tree therefore solved the representation failure.

The same CHAT check reports 152 of 270 positive-unit pair-directions avoided.
Its total run is only around one CPU second, so it is a correctness and
ownership check rather than evidence for a mature speedup.

## Josef scale test and stop decision

An older echoed Josef_04 input made it possible to select every 180th supplied
hint deterministically, retaining 10,045 hints.  This is about 1/180 of the
full active hint population and is small enough for the current machine.

That sample already produced:

| Measurement | Value |
|---|---:|
| Generalized targets | 132,569 |
| Target sidecar bytes | 3,556,388 |
| Recipe bytes | 1,062,888 |
| Target queries in 20 givens | 34 |
| Targets surviving structural filtering | 530,845 |
| Target positions inspected | 15,111,630 |
| Compatible reverse positions | 931,661 |

With symmetric shadow retention, those 931,661 requirements occupied
280,074,716 bytes after only 20 givens and planning consumed 15.320 CPU
seconds.  This is unambiguously unscalable.

With symmetric retention disabled, the RAM-bounded target-only run kept zero
requirements and peaked at only 29,228 KiB RSS.  It still spent 6.546 CPU
seconds in target planning for 20 givens and found no target hit.  Scaling
the target population toward the full 9,813,875 entries makes the comparison
worse, not better.

The full target sidecar itself is projected at roughly 340 MiB with the
64-bit signatures, within the 512 MiB prototype gate.  Before signature
compression the measured posting density projected roughly 557 MiB, already
outside that gate.  But sidecar fit is no longer the deciding problem:
target-query CPU and reverse-join breadth fail by a much larger margin.

Per the reviewed plan, this fails the Stage-2 stop/go criteria.  Do not raise
`hash_target_index_kb` and launch a long run; more RAM cannot repair the broad
join.

## Options for bounded inspection only

Keep the existing Josef compact-OTTER and generalized-hash block.  For a
small shadow audit add:

```prover9
assign(hash_inference_gate,shadow).
assign(hash_targeted_inference,shadow_unit_paramod).
assign(hash_target_index_kb,524288).

assign(max_given,20).
assign(max_seconds,300).
assign(max_megs,4096).
```

Use a small deterministic/random hint subset.  Shadow mode retains symmetric
requirements and is intentionally unsuitable for the full Josef bank.

For the RAM-bounded but incomplete one-sided experiment use:

```prover9
assign(hash_inference_gate,safe).
assign(hash_targeted_inference,targeted_only_unit_paramod).
assign(hash_target_index_kb,524288).

assign(max_given,20).
assign(max_seconds,300).
assign(max_megs,4096).
```

The common required settings remain:

```prover9
assign(search_loop,otter).
assign(inference_frontier,clauses).
assign(hint_index,generalized_hash).
clear(back_demod_hints).
clear(hint_match_once).
assign(hint_expiry,-1).
```

These blocks replace only the corresponding hash-gate/targeted-mode and
limit assignments.  They do not replace the passive-store, compact-index,
selection, or problem-specific options.

## Reading the new statistics

`Hash_target_index:` reports target/recipe counts, rigid-root records,
structural-feature observations, resident and construction bytes, target
queries, structural survivors, and filter rejects.

`Hash_target_inference:` reports target recipes and positions inspected,
compatible reverse positions, active-parent query answers, unique/duplicate
partners, potential/planned/avoided unit directions, shadow hit recall,
requirement code-tree/token bytes, and planning CPU.  In target-only mode,
require `requirements=0`, `symmetric_requirements=disabled`, and inspect
`omitted_symmetric` rather than assuming both OTTER directions were covered.

Any `missed_hash_hits` in a supported shadow audit is a correctness failure.
Zero misses does not establish speed: candidate, position, requirement, and
planning-time fields must also pass their gates.

## Verification and source map

The focused tests prove both shadow and target-only examples, validate their
proofs with `prooftrans`, validate the Horn-equality proof with `directproof`,
and check that target-only does less generation.  A separate proof requires a
nonunit paramodulant and verifies that the unsupported ordinary path remains
live.  Compact-index tests cover deletion, private-pool reclamation, and safe
reuse of a reclaimed synthetic ID.

Run:

```sh
make -j4 all
./test.src/hash_target_inference_test.sh
./test.src/hash_inference_gate_test.sh
make -C test.src compact-unit-index-test
```

Main implementation files:

- `ladr/hint_generalization_hash.[ch]`: reconstructable target recipes;
- `provers.src/hash_target_index.[ch]`: immutable root/signature sidecar;
- `provers.src/hash_target_inference.[ch]`: reverse planning, compact
  symmetric requirements, partner streams, lifecycle, and statistics;
- `provers.src/search.c`: option validation, planner rebuild, authoritative
  target-only dispatch, ordinary fallback boundaries, and final reporting;
- `provers.src/compact_unit_index.[ch]`: independent code-tree strategy and
  private serialized-pool reclamation;
- `test.src/hash_target_inference_test.sh` and related proof inputs.

## Recommended next direction

Keep the Stage-1 `safe` lazy gate.  A future attempt should not tune root
postings, signature widths, FPA depth, or memory budgets around this reverse
join.  It would need a genuinely different context-with-a-hole/code-tree join
that constrains the target by the unchanged part of the actual into parent
before enumerating target positions, or an independently useful indexed
paramodulation generator.  That is a new reviewed project, not a parameter
change to this prototype.
