# Checked proof-recipe replay

Date: 2026-08-26

Branch: `proof-recipe-replay`

## Result

Prover9 now has a deterministic known-proof verification mode:

```prover9
assign(proof_parent_guidance,recipe_replay).
```

Unlike the earlier parent-pair filter, this mode does not run an altered
OTTER search and hope that the altered demodulator chronology recreates the
proof.  It anchors proof roots in the actual input, reconstructs every other
node from already verified parents, executes the recorded simplifications in
their exact order, and checks the computed clause against the recorded body.
The guide can select an operation, but it cannot inject its expected result.

The complete 153,267-node J04 proof was replayed successfully.  Prover9
printed a normal 153,267-step proof and exited with `THEOREM PROVED`.

This is an intentionally incomplete proof-confirmation facility.  It is a
radical replacement for the billions-of-generated-clauses workload when a
known proof is available; it is not a complete search strategy for a new
problem with no proof recipe.

## What is checked

The extractor stores a versioned, URL-safe-base64 recipe on each auxiliary
guide clause.  The compact recipe contains dense parent IDs and all
rule-specific coordinates.  Replay checks:

- exact assumption and clausal-goal bodies against the real problem input;
- that a denial is the real denial generated from its anchored goal;
- paramodulation parent roles, equality side, literal, and term position;
- binary and hyperresolution nucleus/satellite literals, including flipped
  equality satellites;
- copy and back-rewrite parents;
- every rewrite occurrence and direction, in recorded order;
- every recorded equality flip, in recorded order; and
- the canonically variable-renumbered result with `clause_ident()`.

A malformed record, unavailable parent, inapplicable operation, wrong
position, wrong rewrite, or wrong resulting body stops replay with the dense
node number and failing stage.  No mismatching node is installed.

## Building the guide

The extractor accepts plain or gzipped Prover9 output:

```sh
python3 utilities/extract_proof_parent_guide.py \
  J04_part1.out.gz \
  -o J04.recipe-guide.in
```

Validation without writing the guide is available with:

```sh
python3 utilities/extract_proof_parent_guide.py \
  --summary-only J04_part1.out.gz
```

For J04 the summary is:

```text
nodes=153267 para=100124 hyper=17899 back_rewrite=35017
resolve=1 copy=47 assumption=177 goal=1 deny=1 other=0
rewrite_refs=596592
```

## Running prefixes and the full proof

Use this as a replay prelude:

```prover9
assign(proof_parent_guidance,recipe_replay).

% Use 1000 or 10000 first; -1 means the complete recipe.
assign(proof_recipe_max_nodes,1000).

% Logical verification only: do not construct a hint index.
clear(proof_recipe_hint_audit).
clear(proof_recipe_require_hint).

assign(cores,0).
assign(max_megs,4000).
assign(max_seconds,300).
assign(stats,all).
clear(print_initial_clauses).
clear(echo_input).
```

Place the prelude before the original problem, and the generated guide after
the problem.  Reading the prelude first lets logical-only replay discard each
large hints list as it is read.  If the old problem overrides resource limits,
read the prelude again at the end:

```sh
/usr/bin/time -v -o J04.recipe-1k.time \
  ../bin/prover9 -f \
  recipe-options.in \
  original-J04.in \
  J04.recipe-guide.in \
  recipe-options.in \
  > J04.recipe-1k.out \
  2> J04.recipe-1k.err
```

Prover9 uses one `-f` followed by all input filenames; do not repeat `-f`
before every filename.

For the next gate, change only:

```prover9
assign(proof_recipe_max_nodes,10000).
```

For the full replay:

```prover9
assign(proof_recipe_max_nodes,-1).
```

A completed prefix deliberately does not claim a theorem.  It prints, for
example, `Proof recipe prefix completed: 10000 of 153267 nodes verified`, and
uses Prover9's `sos_empty`/exit-status-2 path.  The complete recipe must end in
a checked empty clause and exits normally with a proof.

Ordinary `search_loop`, passive-store, compact-index, given-selection, and
inference-frontier settings do not drive recipe replay.  No SOS selection,
passive clause store, forward demodulation pass, or inference index is built.
The ordinary problem commands may remain in the source file, but they are not
the replay scheduler.

## Optional current-hint audit

To ask how the verified clauses interact with the hints supplied *now*:

```prover9
set(proof_recipe_hint_audit).
clear(proof_recipe_require_hint).
```

This builds the configured hint matcher and reports queries, matches, and
keep/delete-rule outcomes.  It is separate from logical verification.

For an explicitly strict coverage experiment:

```prover9
set(proof_recipe_hint_audit).
set(proof_recipe_require_hint).
```

Strict audit rejects the first nonroot verified node that matches no current
hint.  It is not needed to establish the proof.

The audit cannot reconstruct historical hint state exactly.  The source run
back-demodulated hints using nonproof demodulators that are absent from the
proof closure.  The statistics therefore say "current input hints", not
"the exact hint state of the source run".

## J04 measurements

The measurements below used a 4 GiB process virtual-memory cap and a
five-minute Prover9 limit.  Logical-only replay used the 229 logical input
clauses and omitted the source hint lists.  The generated guide was 52 MiB.

| Run | Verified nodes | Replay CPU | Whole user CPU | Wall | Peak RSS |
|---|---:|---:|---:|---:|---:|
| J04 prefix | 1,000 | 0.019 s | 52.12 s | 56.35 s | 309,964 KiB |
| J04 prefix | 10,000 | 0.156 s | 31.74 s | 34.59 s | 309,740 KiB |
| J04 complete | 153,267 | 4.701 s | 40.09 s | 43.59 s | 430,704 KiB |

The slower first prefix echoed the 52 MiB guide because its `clear(echo_input)`
was read too late.  Reading the prelude first removed that I/O and is the
recommended invocation.

The complete replay reported:

```text
copy=47, back_rewrite=35017, paramod=100124, hyper=17899, resolve=1
rewrites=596592, flips=62052
source_givens=40067
digest=1477c0c745e3fcb1
```

The source J04 search recorded 5,452,965,848 generated clauses,
149,661.79 seconds of user CPU, 150,792 seconds wall time, and
`Megabytes=13339.59`.  Whole-process replay was about 3,733 times smaller in
user CPU and 3,459 times smaller in wall time.  Its measured peak RSS was
about 421 MiB.  The source `Megabytes` counter and `/usr/bin/time` peak RSS are
not identical metrics, so the memory ratio is only indicative; comparing
13.34 GB with 0.43 GB suggests roughly a 97% reduction.  The structurally
important fact is that replay has no millions-of-passives population at all.

The guide itself reported 66,697,456 bytes of clause bodies, 5,616,068 recipe
bytes, and 31,769,117 bytes for its compact directories/arena accounting.

The emitted 21 MiB proof was also parsed successfully by `prooftrans
parents_only`: exit status 0, 27.76 seconds user CPU, 30.47 seconds wall, and
409,344 KiB peak RSS.

## Terminal-proof scalability fixes

The first full replay exposed two old quadratic operations after the empty
clause had already been verified:

1. proof ancestor collection did linear membership and sorted-list insertion
   for every ancestor; and
2. terminal snapshot construction repeatedly appended to a singly linked
   list.

Ancestor collection now uses an open-addressed ID set, an array, and one final
sort.  Snapshot construction prepends and reverses once.  Proof-level
calculation uses an ID-to-level table over the already closed proof DAG.  These
changes are general terminal-proof improvements, not J04-specific inference
hacks.

## Tests

The focused test target is:

```sh
TMPDIR=/path/with/free-space make -C test.src proof-parent-guide-test
```

It covers recipe encoding/decoding, exact paramodulation, multi-parent
hyperresolution, rewriting, flips, wrong bodies, wrong positions, actual root
anchoring, a normal theorem result, bounded-prefix termination, current-hint
auditing, strict missing-hint rejection, and compatibility of the older
off/shadow/authoritative parent-guidance modes.
