# Exact proof-recipe replay plan

Date: 2026-08-26

Branch: `proof-recipe-replay`

Status: implemented and validated; see `P9-PROOF-RECIPE-REPLAY-REPORT.md`

## Objective

Replace the unsuccessful proof-parent pair filter with a deterministic,
checked replay of the complete derivation recorded in a known Prover9 proof.
The guide may select which inference to attempt, but it must never supply an
unchecked conclusion.  Every non-input node must be reconstructed by LADR's
ordinary inference primitives, its recorded secondary simplifications must be
replayed, and the computed body must be structurally identical to the guide
body before that node can become a parent.

This mode is a known-proof verifier and hint-coverage experiment, not a
complete theorem search.  It deliberately avoids the ordinary OTTER
given-clause schedule because changing the inference population changes the
demodulator chronology and therefore the normalized clause bodies.

## Evidence from J04

`J04_part1.out.gz` contains:

- 153,267 proof nodes;
- 100,124 paramodulation steps;
- 17,899 hyperresolution steps, with at most three parents;
- 35,017 back-rewrite steps;
- one final binary-resolution step;
- 596,592 ordered rewrite references, at most 56 on one node;
- 1,668 right-to-left rewrites;
- 62,052 equality flips; and
- paramodulation-into positions of depth at most seven.

The failed pair-filter run loaded the correct guide but exhausted SOS after
27,311 givens.  Of those, 7,246 did not match a guide body.  Restricting the
inference population changed rewriting and clause selection, so a permitted
parent pair frequently failed to create the exact recorded child.  A single
missing child then removed all descendants from the known proof.

## Trust boundary

The implementation must enforce all of the following:

1. An assumption or goal node is accepted only if its body and role match the
   actual problem input.
2. A denial is reconstructed from the corresponding supplied goal.
3. Every other node is computed from already verified parents.
4. Paramodulation uses the recorded from/into roles, equality side, literal,
   and term position.
5. Hyperresolution uses the recorded nucleus literals and satellite literals,
   including flipped equality satellites.
6. Rewrites are applied in the recorded order, at the recorded occurrence,
   and in the recorded direction.
7. Equality flips and any other secondary operations are replayed in order.
8. The result is deterministically variable-renumbered and checked with
   `clause_ident()` against the expected body.
9. A mismatching node is never installed and replay stops with a precise
   diagnostic.
10. The final empty clause is returned through Prover9's normal proof-result
    path and the emitted proof is independently checkable.

## Recipe representation

The extractor will continue to emit native
`formulas(proof_parent_guide)` input.  Each guide body receives one additional
`proof_parent_recipe` string attribute containing a versioned compact binary
record encoded as URL-safe base64.  Strings avoid interning proof metadata as
function symbols and are substantially smaller than millions of repeated
integer attributes.

The binary record uses unsigned LEB128 integers and zig-zag coding for signed
literal designators.  It contains:

- a format version;
- a primary-rule opcode;
- dense parent IDs;
- rule-specific position/literal data;
- an ordered sequence of secondary opcodes; and
- source-run annotations needed for diagnostics, such as whether the node was
  selected as a given.

The loader validates the version, exact record length, parent ranges,
topological ordering, rule arity, position lengths, rewrite direction, and
root role before deleting the transient input attribute.  Decoded records are
retained in one compact byte arena with per-node offsets.

## Strict replay core

The existing `expand_proof()` implementation demonstrates the necessary LADR
operations, but its shortcut for uncomplicated steps copies the recorded
clause instead of recomputing the inference.  The replay implementation must
therefore factor out strict single-step helpers rather than calling that
shortcut.

The primary operations are based on:

- `para_pos()` for exact paramodulation;
- `resolve2()` for each recorded hyper/binary resolution component;
- `copy_inference()` for checked copies;
- the normal denial transformation for goals; and
- input-role matching for roots.

Secondary operations use:

- `particular_demod()` for each recorded rewrite;
- `flip_eq()` for equality flips; and
- explicit support for any additional secondary opcode admitted by the
  extractor.

The proof is already topologically ordered, so the first implementation uses
a simple node cursor rather than an active-clause scheduler.  Verified node
objects are stored by dense node ID.  Duplicate bodies remain distinct proof
nodes because their parents and justifications can differ.

## Interaction with ordinary search and hints

Recipe replay must not pass a reconstructed node through ordinary forward
demodulation a second time.  That would reintroduce the rewrite chronology
that caused the parent-filter experiment to fail.  It also does not need SOS,
Usable scans, passive selection, or inference indexes to decide the next
recipe.

After a node has been verified, an optional hint-audit phase may run the
ordinary configured hint matcher and record:

- whether the clause matched;
- the resulting hint weight;
- whether the keep/delete rules would retain it; and
- whether it was a given in the source run.

Historical hint state cannot be reconstructed exactly from the final proof:
the source run back-demodulated hints with nonproof demodulators that are not
in the proof closure.  Output must distinguish current-input hint auditing
from historical source behavior.  A strict audit option may reject a required
node that does not satisfy current hint rules, but this is separate from
logical proof verification.

## User-facing modes

Keep the existing values for compatibility:

```prover9
assign(proof_parent_guidance,off).
assign(proof_parent_guidance,shadow).
assign(proof_parent_guidance,authoritative).
```

Add:

```prover9
assign(proof_parent_guidance,recipe_replay).
```

Initial safety limits:

```prover9
assign(proof_recipe_max_nodes,-1).  % all nodes; use 1000/10000 for prefixes
clear(proof_recipe_hint_audit).     % logical replay only
clear(proof_recipe_require_hint).   % do not gate proof on current hints
```

Recipe mode rejects checkpoint resume, multicore portfolio search, and
options that attempt to combine it with hash-targeted inference.  Most
passive-store and compact search-index options are irrelevant and should be
reported as ignored rather than silently initialized at full cost.

## Diagnostics and statistics

Stable terminal statistics must include:

- nodes loaded, roots anchored, and nodes verified by primary rule;
- primary operations and secondary rewrite/flip operations;
- body comparisons and mismatches;
- current node, parent IDs, and failing substep on divergence;
- recipe arena, guide-body, verified-node, and peak resident bytes;
- nodes and rewrites per second;
- hint-audit queries, matches, keep/delete outcomes, and strict failures; and
- a deterministic digest of the verified node stream.

Prefix termination is a successful bounded measurement, not a theorem proof.
It must say explicitly that the recipe prefix completed without reaching the
terminal empty clause.

## Implementation and commit stages

1. Save this plan and create the branch.
2. Extend extractor parsing, dense-ID rewriting, compact recipe encoding, and
   Python tests.
3. Decode and validate recipes in `proof_parent_guide` with focused C tests.
4. Add strict replay helpers and small end-to-end paramodulation,
   hyperresolution, rewrite, flip, denial, and corruption tests.
5. Anchor roots and integrate topological replay with the Prover9 result path.
6. Add hint auditing and stable statistics.
7. Validate 1,000 and 10,000 J04 nodes with conservative time/RAM limits.
8. Attempt the complete 153,267-node replay only after all prefix gates pass.
9. Update usage documentation and record measured CPU/RSS.

Each stage is committed separately with a detailed message.  No long ordinary
J04 search is run on the development machine.

## Acceptance gates

The implementation is complete only when:

- malformed or tampered recipes fail closed;
- root clauses cannot be injected by the guide;
- all focused recipe forms compute, rather than copy, their expected result;
- deliberate wrong positions and wrong expected bodies are detected;
- existing off/shadow/authoritative behavior and tests remain intact;
- bounded J04 prefixes have zero replay divergence;
- the complete J04 recipe reaches a verified empty clause, subject to
  available local resources; and
- the emitted proof passes the existing proof transformation/checking tools.

The target is work proportional to the roughly 750,000 recorded inference and
rewrite operations rather than the 5.45 billion clauses generated by the
source search.  CPU and RSS claims remain measurements, not promises, until
the prefix and full gates are run.

## Completion evidence

The 1,000- and 10,000-node J04 prefix gates completed with zero divergence.
The complete 153,267-node recipe then verified its final empty clause and
printed a normal Prover9 proof.  It executed 100,124 paramodulations, 17,899
hyperresolution steps, 35,017 back-rewrite steps, one binary resolution,
596,592 ordered rewrites, and 62,052 flips.  The complete process used 40.09
seconds of user CPU, 43.59 seconds wall time, and 430,704 KiB peak RSS.  The
verified-clause digest was `1477c0c745e3fcb1`.

The emitted proof was accepted by `prooftrans parents_only` with exit status
0.  Detailed usage, caveats, measurements, and the terminal-proof scalability
fixes discovered during the full gate are recorded in the report.
