# Lazy generalized-hash inference gate

Date: 2026-08-25

Branch: `lazy-hash-gate`

Status: Stage 1 of `P9-HASH-GUIDED-INFERENCE-PLAN.md` is implemented.  The
more ambitious target-directed inference generator is not implemented.

## What this changes

Ordinary Prover9 paramodulation first finds two compatible terms and a
unifier, then normally allocates a complete conclusion: copied terms,
literals, a clause object, attributes, and a proof justification.  Only after
that construction does the generalized-hint hash learn that almost every
Josef_04 conclusion is a miss.

This branch inserts a callback at the point after successful unification but
before conclusion construction.  For supported positive unit equalities, it
walks the two parent terms through the live substitutions and calculates the
same generalized-hash key without allocating the conclusion.  The callback
can then:

- materialize the conclusion normally;
- skip it before construction; or
- cancel the current enumeration cleanly.

Both ordinary and resumable/bounded paramodulation iterators have this
callback contract.  The authoritative gate is currently restricted to the
ordinary OTTER clause frontier.

The virtual walker handles rewrites at the root or below it, either side of
the source equality, either side of the resulting equality, constants,
variables, shared bindings, and the two independent variable namespaces of
the parents.  It checks both final equality orientations.  A separate walker
hashes the newly materialized raw equality and audits the virtual result
before simplification can change it.

## Modes

`off`

: The callback is absent.  This is the existing generalized-hash search and
  is the lowest-overhead control.

`shadow`

: Computes the virtual answer but always constructs and processes the real
  clause.  It compares the virtual key with an independently traversed raw
  materialized key and reports what later simplification does.  Use this
  first on every new problem family.

`safe`

: Skips only virtual misses that can be proved to be deleted by the current
  configuration.  A supported miss is skipped only when the clause is above
  `max_weight`, the applicable keep rules require a hint, default symbol-node
  weighting is in use, and no enabled simplification or diagnostic behavior
  can invalidate the decision.  Otherwise it falls back to ordinary
  materialization.

  In particular, the current certificate rejects early skipping when
  demodulation rules are available, or when evaluation rewriting, unit
  deletion, CAC redundancy, generated-clause actions, generated-clause
  printing, hint tracing, or safe-unit-conflict handling makes the result
  uncertain.  This is intentionally conservative.  It means `safe` can
  preserve the search yet save little on a demodulation-heavy Josef_04 run.

`hit_only`

: Materializes supported hash hits and mandatory protected cases, but skips
  most supported raw misses.  This is an intentionally incomplete experiment
  for measuring the performance ceiling.  It can omit low-weight clauses
  that the ordinary search would keep, so it can change the given stream,
  lose a proof, or find a different proof.  Prover9 prints a warning when it
  is selected.

Unsupported conclusions, including nonunit shapes and inference rules other
than the implemented unit-paramodulation path, continue through the existing
materialized path in all modes.

## Required and recommended options

Keep the compact OTTER options appropriate to the problem and use this block
for a first validation run:

```prover9
assign(search_loop,otter).
assign(inference_frontier,clauses).

assign(hint_index,generalized_hash).
assign(hint_hash_complete_nodes,8).
assign(hint_hash_partial_per_hint,32).
assign(hint_hash_max_entries,100000000).
clear(back_demod_hints).

assign(hash_inference_gate,shadow).
assign(hash_inference_sample_rate,65536).
assign(hash_inference_validate_rate,0).
```

The generalized hash still requires a static hint bank:

- `clear(back_demod_hints)`;
- leave `hint_match_once` clear;
- leave `hint_expiry` disabled.

Every non-off gate currently requires all of the following:

- `assign(hint_index,generalized_hash)`;
- `assign(search_loop,otter)`;
- `assign(inference_frontier,clauses)`.

Invalid combinations stop with a fatal configuration error instead of
quietly changing behavior.

`hash_inference_sample_rate` controls how often stage time and allocator
deltas are sampled.  `65536` is the low-overhead default.  A value of `1` is
useful only for small tests.

`hash_inference_validate_rate` materializes every Nth virtual miss instead of
skipping it.  In `safe` mode the sampled conclusion must be deleted, remain a
hash miss, and agree with the raw materialized key or the run stops.  Use
`65536` for an audited bounded experiment and `0` to disable validation after
the mode is trusted.

## Suggested Josef_04 sequence

Do not start with an unlimited run.  On the machine that has the complete
input and hint bank:

1. Run `shadow` to 100 or 1,000 givens.  Require `raw_mismatches=0`.  Compare
   its given stream and ordinary summary with gate `off`.
2. Inspect the postprocessing mismatch fields.  These can be nonzero when
   demodulation changes a correctly predicted raw conclusion; they are not a
   virtual-walker error.
3. Run `safe` with `hash_inference_validate_rate=65536`.  Check whether
   `certified_skips` is substantial or whether `safety_fallbacks` dominates.
4. Only as an explicitly incomplete speed/coverage experiment, run
   `hit_only` with the same validation rate at 1,000 and then 3,000 givens.
   Compare CPU, generated conclusions, hash hits, proof progress, and the
   given stream, but do not describe it as equivalent to the control.

For each run use both Prover9 limits and an external limit suited to the
machine, for example `max_given`, `max_seconds`, and `max_megs` plus
`timeout`.  The implementation work did not launch a new full Josef_04 run.

## Reading the statistics

`Hash_inference_gate:` reports:

- `supported` and `unsupported_fallbacks`: results the virtual unit-equality
  walker could and could not represent;
- `virtual_hits`, `virtual_misses`, and `probes`: pre-allocation hash work;
- `materialized_hits`, `materialized_misses`, and `safety_fallbacks`: work
  that still entered ordinary construction;
- `certified_skips`: equivalent deletions avoided by `safe`;
- `hit_only_skips`: deliberately incomplete omissions;
- `raw_mismatches`: disagreement between the allocation-free virtual key and
  the independently traversed materialized raw key; this must be zero;
- `postprocess_false_misses`, `postprocess_false_hits`, and
  `postprocess_changed_hint_ids`: differences after ordinary simplification
  and orientation; these diagnose the boundary at which `safe` must fall
  back;
- `estimated_allocations_avoided` and `estimated_bytes_avoided`: estimates
  derived from sampled materialized constructions, not direct CPU-time
  measurements.

`Hash_inference_stages:` separates sampled time and allocation deltas for the
virtual precheck, real conclusion construction/justification, and the normal
consumer (`cl_process`).  Estimates are meaningful only when
`materialized_samples` is nonzero.  With no materialized sample they remain
zero rather than claiming that construction costs nothing.

The existing allocator totals remain useful traffic counters.  They do not
say how much CPU the allocator itself consumed; the stage samples are what
separate construction from processing.

## Verification completed on this branch

The focused gate matrix checks all four modes.  For its deterministic bounded
case, `off`, `shadow`, and `safe` emit the same given stream and the same
Given/Generated/Kept summary.  Shadow and safe report zero raw-key mismatch.
Safe materializes hits and certifiably skips misses; hit-only emits its
incompleteness warning.

A separate safe-mode equality problem proves a theorem while exercising both
a materialized hash hit and certified skips.  Its proof is accepted by both
`prooftrans parents_only` and `directproof`.

The virtual-key unit tests cover root and deep positions, both equality
sides, both source sides, variable and constant replacements, reflexive
results, shared variables, cross-parent binding merges, and independent
unbound variable namespaces.  Iterator tests cover materialize, skip, cancel,
and skip behavior across bounded yield/resume boundaries.

On `chat_test.in`, which contains 70,185 supplied hint formulas before the
normal hint-bank processing, a bounded 31-given shadow check preserved the
control trajectory (`Generated=1391`, `Kept=192`).  It observed 1,062
successful paramodulation candidates: 811 supported and 251 fallback shapes.
All 811 raw validations agreed.  Later simplification produced one raw-miss
to final-hit transition and 17 changed hint IDs, demonstrating why raw-key
correctness and postprocessing effects must be reported separately.

An earlier same-host off/shadow timing pair for this bounded input was 3.63
user seconds in each mode, with approximately 117.4 MB peak RSS.  This tiny
run establishes low visible shadow overhead, not a long-run speedup.

No post-gate Josef_04 1,000- or 3,000-given performance result exists yet.
The old `Josef_04.hash-all.out` figures motivate the work but cannot be used
as a measured saving: at its mature report the old implementation had
constructed about 4.09 billion conclusions and recorded about 282.5 billion
allocator calls, whereas the new gate has not been run on that workload.

## Building and testing

```sh
make -j4 all
make -C test.src hint-postings-test
```

The second command includes `hash_inference_gate_test.sh`.  The focused tests
can also be run directly:

```sh
./test.src/hash_inference_gate_test.sh
./test.src/hint_generalization_hash_test
./test.src/paramod_iterator_test
```

## Source map

- `ladr/paramod.[ch]`: pre-materialization callback, iterator decisions, and
  sampled construction/consumer accounting;
- `ladr/hint_generalization_hash.[ch]`: allocation-free virtual and raw
  materialized generalized-hash lookup;
- `ladr/hints.[ch]`: access to the active generalized-hint hash;
- `provers.src/search.c`: policies, safety certificate, generated-event
  accounting, validation, and statistics;
- `provers.src/search-structures.h`: public options;
- `ladr/clause_eval.[ch]` and `provers.src/white_black.[ch]`: conservative
  proof that configured keep rules require a hint;
- `test.src/hash_inference_gate*`, `hint_generalization_hash_test.c`, and
  `paramod_iterator_test.c`: behavior, proof, key, and iterator coverage.

## What remains for the ambitious stage

The lazy gate still enumerates every eligible parent/position combination
and performs unification.  It avoids construction of rejected supported
results, but cannot remove the dominant enumeration/unification work if that
is where Josef_04 spends its CPU.

The next stage therefore remains target-directed generation: index structural
requirements from generalized-hash entries, use them to select plausible
parent clauses and rewrite positions, generate only sound ordinary
inferences suggested by those targets, and pass every emitted conclusion
through the existing authoritative processor.  It needs a shadow-recall
phase against ordinary enumeration and, for a complete search, a separate
fair fallback lane.  That work is deliberately absent from this branch.
