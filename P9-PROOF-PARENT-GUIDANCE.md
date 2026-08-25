# Proof-parent-guided confirmation

This experimental mode uses the parent graph of an already known Prover9
proof to avoid trying every active clause as a paramodulation partner during
a proof-confirmation run.  It is opt-in and intentionally incomplete.  Every
permitted inference and every resulting clause still goes through Prover9's
ordinary inference, demodulation, redundancy, hint-matching, and proof code.

## Build and extract a guide

Build this branch normally:

```sh
make -j4
```

Convert a plain or gzip-compressed output containing a `PROOF` section:

```sh
python3 utilities/extract_proof_parent_guide.py \
  /path/to/J04_part1.out.gz \
  -o /path/to/J04.proof-parent-guide.in
```

Validate and count a proof without writing the guide:

```sh
python3 utilities/extract_proof_parent_guide.py \
  --summary-only /path/to/J04_part1.out.gz
```

For the supplied J04 proof the expected summary starts with:

```text
nodes=153267 para=100124 hyper=17899 back_rewrite=35017
```

The extractor checks that every dependency exists and precedes its child.  It
renumbers the source proof IDs densely and emits repeated integer attributes,
which avoids creating one interned string symbol per proof edge.

## Run a control and a candidate

Add this to the ordinary problem/options file for the measurement-only run:

```prover9
assign(search_loop,otter).
assign(inference_frontier,clauses).
assign(proof_parent_guidance,shadow).
```

Keep the existing hint-index, demodulation, compact-index, passive-store, and
ancestor-store options.  The proof-parent block does not replace them.  Run
the problem and generated guide as two input files:

```sh
/usr/bin/time -v bin/prover9 -f \
  /path/to/Josef_04.shadow.in \
  /path/to/J04.proof-parent-guide.in \
  > /path/to/Josef_04.shadow.out \
  2> /path/to/Josef_04.shadow.err
```

`shadow` performs the ordinary search and only classifies parent pairs.  It is
therefore the first bounded coverage test.  Its `Proof-parent guidance`
statistics report mapped/unmapped givens and accepted/rejected pairs.

For the restricted candidate, change only:

```prover9
assign(proof_parent_guidance,authoritative).
```

In this mode:

- paramodulation directly enumerates active representatives of recorded
  co-parent proof bodies and does not scan the full Usable list;
- hyperresolution uses its normal literal index, then rejects clause
  candidates that were not co-parents of the given clause in any recorded
  hyperresolution step;
- binary and UR resolution remain unrestricted in this first version; and
- demodulation and all forward/backward simplification remain unrestricted
  and ordinary.

An exact structural body match after deterministic variable renumbering maps
a runtime clause to one or more proof nodes.  Runtime clause numbers are never
compared with source-run clause numbers.  Repeated proof bodies share one
body-index record; multiple live runtime copies are allowed and deduplicated
by runtime ID when partners are collected.

## Important limitations

`authoritative` prints an incompleteness warning.  If a selected clause has no
exact guide-body match, it gets no paramodulation partners.  A missing edge can
therefore lose the known proof, but it cannot validate a false proof: retained
conclusions are still built and checked by the normal calculus.

The initial implementation requires `search_loop=otter` and
`inference_frontier=clauses`, cannot be combined with
`hash_targeted_inference`, and rejects checkpoint resume.  Hyperresolution is
pair-filtered rather than replayed as an exact multi-parent recipe.  Equality
sides and paramodulation positions are not yet restricted, so an admitted
parent pair can still try more directions and positions than the recorded
proof used.

Use short limits for the first J04 shadow and authoritative runs.  On the
development machine, parsing the complete 41 MiB J04 guide and building its
153,267-node graph took about 22 seconds wall time and peaked at about 262 MiB
RSS in a one-given loader test.  Graph construction itself reported 0.69 CPU
seconds.  These are startup measurements, not a prediction of full J04 search
time or proof success.

The natural next stages, if mapped-given coverage is high, are exact
paramodulation position/direction filtering and scheduling complete recorded
recipes as soon as all parents are live.
