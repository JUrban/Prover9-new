# Proof-parent-guided confirmation plan

Date: 2026-08-25

Branch: `proof-parent-guidance`

Status: implementation plan for an opt-in, sound but deliberately incomplete
proof-confirmation mode driven by an already known Prover9 proof.

## Objective

Use the derivation recorded in `J04_part1.out.gz` to avoid combining every
selected clause with the complete eligible active set during a hint-based
proof-confirmation run.  The mode must never trust source-run clause numbers
as runtime identities and must never manufacture an unchecked conclusion.
It may omit unrecorded inferences: that incompleteness is the point of the
experiment and must be announced in the output.

This is different from the rejected generalized-hash target-first prototype.
That prototype searched backward through roughly 9.8 million broad target
patterns.  The present input is a finite, closed proof DAG whose parent edges
are already known.

## Evidence and scale

The supplied proof has:

- 5,452,965,848 generated clauses in the original run;
- 153,267 clauses in the final proof DAG;
- 100,124 primary paramodulation steps;
- 17,899 primary hyperresolution steps;
- 35,017 back-rewrite steps;
- 66,169 givens in the run, of which 40,067 occur in the proof;
- 142,201 clauses with explicit forward rewriting;
- 596,592 listed rewrite-parent references, mean 4.20 and maximum 56; and
- only 6,941 proof clauses carrying an older stable `p9loop ... ID<...>`
  label.

Consequently, labels and source clause numbers are insufficient as runtime
identities.  Exact clause bodies modulo deterministic variable numbering are
the first implementation boundary.

## User-facing input

The extractor accepts an ordinary or gzip-compressed Prover9 output and emits
an include file containing:

```prover9
formulas(proof_parent_guide).
  <proof clause body>
    # label("proof_parent_node=<source proof ID>")
    # label("proof_parent_para=<parent ID>,<parent ID>").
  ...
end_of_list.
```

Hyperresolution, binary resolution, back-rewrite, and copy nodes use distinct
`proof_parent_*` labels.  The complete parent list is retained even when the
first implementation only consumes a subset of the rule kinds.  Existing
labels on proof clauses are preserved.

The search option is:

```prover9
assign(proof_parent_guidance,shadow).        % measure only
assign(proof_parent_guidance,authoritative). % omit unrecorded pairs
```

The default is `off`.  The mode initially requires the ordinary OTTER clause
frontier and rejects checkpoint resume.  It does not require a particular
hint index and can therefore be combined with `generalized_hash` and its safe
lazy gate.

## Stable identity and guide representation

1. Clausify each auxiliary guide formula without adding it to the theory.
2. Deterministically renumber variables.
3. Hash literal signs and the complete term tree, including symbol numbers,
   arities, and variable numbers.
4. Confirm every hash candidate with `clause_ident`; a 64-bit collision can
   only cost an exact comparison, never create an allowed edge.
5. Retain every proof node separately.  Repeated formula bodies can therefore
   map one runtime clause to several proof nodes.
6. Convert source proof IDs to dense node indexes and store symmetric sparse
   co-parent adjacency for each supported primary inference.

Runtime active clauses are registered by exact guide-node membership.  A
node can have multiple active runtime representatives.  Deactivation marks
the representative stale before its body can be archived or freed.  Partner
enumeration deduplicates runtime IDs and restores increasing runtime-ID order
so permitted ordinary inferences retain Prover9's deterministic order.

## Modes

### `off`

No guide is built and existing behavior is unchanged.

### `shadow`

Run ordinary inference enumeration.  For every paramodulation partner and
every hyperresolution clause candidate, determine whether the pair occurs in
at least one recorded primary proof step.  Record mapped/unmapped clauses,
allowed/disallowed pairs, and the number of proof nodes with live runtime
representatives.  Do not filter.

### `authoritative`

For paramodulation, collect active runtime representatives only from proof
nodes adjacent to a node matching the selected clause.  Do not scan the full
Usable list.  For hyperresolution, use the existing `Clash_clause_test` hook
to reject clause candidates that never co-occur with the selected clause in
a recorded hyper step.

Binary resolution, UR resolution, factoring, forward demodulation, and
backward simplification remain ordinary in the first version.  This preserves
proof-sensitive denial handling and all recorded demodulation behavior while
the high-volume paramodulation path is measured.

Every conclusion still goes through the unchanged inference implementation,
`cl_process_simplify`, hint matching, keep/delete rules, subsumption, proof
justification construction, and proof validation.  Restriction can lose a
proof but cannot make an invalid proof sound.

## Safety and failure behavior

- `authoritative` prints a prominent incompleteness warning.
- An empty guide in a non-off mode is a configuration error.
- Malformed, duplicate, missing, or forward-referencing proof-node metadata
  is rejected by the extractor or loader with the source node number.
- Runtime source IDs are never compared with proof source IDs.
- Hash matches are always confirmed structurally.
- An unmapped selected clause has no guided paramodulation partners in
  authoritative mode; this is counted explicitly rather than silently using
  the ordinary full scan.
- Guide clauses do not enter Usable, SOS, Demods, Hints, the symbol ordering,
  or the logical theory.
- Checkpoint/resume is rejected until guide state is serialized.

## Statistics

Print one stable `Proof_parent_guidance:` line containing at least:

- mode, guide nodes, unique bodies, and primary edges by rule;
- runtime activation/deactivation counts;
- mapped and unmapped activations/givens;
- paramodulation ordinary candidates, allowed candidates, rejected
  candidates, and directly enumerated partners;
- hyperresolution clause-test calls, allowed candidates, and rejected
  candidates;
- duplicate partner suppressions and maximum collected partner count; and
- resident guide/index bytes.

Shadow and authoritative runs on the same deterministic bounded input must
agree on how every examined pair is classified.

## Implementation stages and gates

### Stage 1: extractor and loader

- Add a streaming extractor supporting `.out` and `.out.gz`.
- Preserve clause bodies and primary rule metadata.
- Add parser tests for paramodulation, hyperresolution, back-rewrite, copy,
  resolve, flip, and rewrite suffixes.
- Load a small hand-written guide and prove collision-confirmed exact mapping.

Gate: malformed input fails clearly; the J04 extraction reports exactly
153,267 nodes, 100,124 para steps, 17,899 hyper steps, and 35,017 back-rewrite
steps.

### Stage 2: shadow pair classification

- Register/unregister active runtime clauses.
- Classify every ordinary paramodulation pair.
- Connect the existing hyperresolution clause-test hook.
- Report stable statistics without changing Generated/Kept/given streams.

Gate: control and shadow proof output and normalized given stream are
identical on focused paramodulation and hyperresolution tests.

### Stage 3: authoritative parent filtering

- Replace the complete Usable scan with sorted guide-neighbor enumeration for
  paramodulation.
- Authoritatively filter hyper clause candidates.
- Preserve unrestricted proof-sensitive rules outside the supported set.

Gate: focused guided examples prove with fewer examined parent pairs, their
proofs pass `prooftrans parents_only` and `directproof`, and an intentionally
missing edge loses the proof without crashing or generating an invalid one.

### Stage 4: J04 bounded validation

- Build the guide from `J04_part1.out.gz`.
- Measure loader time/RSS and body/edge index bytes without starting a long
  proof search.
- Run a small given/time-bounded shadow comparison first.
- Run authoritative mode only after shadow demonstrates useful coverage.

Gate: guide construction remains comfortably below the RAM saved by avoiding
ordinary inference generation; no unrestricted long run is started on the
current machine.

## Deferred extensions

The following require separate evidence and are not hidden in the first
authoritative mode:

- restricting equality sides and exact paramodulation positions;
- scheduling a proof recipe immediately when all parents become live;
- exact multi-parent hyperrecipe state rather than pairwise co-parent
  filtering;
- mapping a more-general runtime clause to a proof node by subsumption;
- explicitly scheduling the ordered rewrite lists; and
- checkpoint serialization of guide and live-node state.

Position filtering and direct recipe scheduling are the likely next CPU
steps if sparse parent enumeration reproduces useful proof prefixes.
