# Second-Generation Compiled Hint Matcher Plan

Date: 2026-08-24  
Production base: `faster-packed-fallback` (`1b03bcf`)  
Experimental evidence: `hint-code-tree` through `e63d9bb`

## Decision

Long production runs should go back to:

```text
assign(hint_index,packed_fast).
```

The `packed_code_tree*` modes remain on the experimental branch for tests and
measurement.  We should not tune their thresholds further.  The held-out
Josef 01/300 run preserved the exact search but made total user CPU 11.4%
worse and search-phase CPU about 24.5% worse.  It completed only 2 of 174
bounded tree probes.

The next project should build a compiled, set-at-a-time instance matcher that
can skip entire subterms and choose its tests by selectivity.  This is a
different architecture, not a faster version of the present traversal.

## A terminology correction

The local Waldmeister paper describes a **perfect discrimination tree**: term
symbols are stored in a trie, variables are normalized, children are fixed
arrays, and one-leaf tails are collapsed.  It says that code trees and context
trees are newer alternatives; it does not itself specify their algorithms.

The current Prover9 prototype already implements the important structural
parts of the paper's discrimination tree: normalized variables, shared
prefixes, collapsed one-way paths, compact offsets, and direct selection of a
rigid child.  Its measured fan-out is small, so replacing the remaining
sibling lists with full child arrays would consume more memory without fixing
the main cost.

Here “Waldmeister-like” should therefore mean the paper's broader engineering
lesson: perform one query against the retained set as a whole, use compact
immutable arrays, and minimize memory traffic.  The proposed compiled matcher
goes beyond the exact data structure described in that paper.

## Why the first tree failed

Suppose a generated clause contains `f(x,x)`.  To find hints that it subsumes,
the matcher must find stored terms `f(t,t)` for any term `t`.

The current tree reads stored hints from left to right.  At the first `x`, it
must explore many possible stored subterms.  It can reject a different second
subterm later, but by then it has already entered many nodes.  CHAT/100 reduced
79,437 packed candidates to 2,534 final candidates, yet the unbounded tree
entered 2.2 million nodes.  Faster child lookup does not remove that work.

The new representation must make two operations cheap:

- **skip-subterm:** when a generated variable meets a stored subterm, jump
  directly to the first symbol after that subterm;
- **same-subterm:** when that generated variable occurs again, compare the two
  stored subterms cheaply and then confirm exactly if needed.

It must also be free to test a rare rigid symbol before a broad variable even
when the rare symbol occurs later in the term.

## Proposed architecture

### 1. Keep `packed_fast` as the semantic and performance control

Do not modify the existing mode.  Every experimental lookup initially runs
the new candidate generator in shadow and still uses the packed answer.  The
existing compressed direct matcher remains the final authority even after
the new candidate generator becomes authoritative.

This preserves hint identity, decreasing stable-ID order, equivalence versus
proper-subsumee preference, match-once behavior, equality flipping,
`_AnyConst`, rewriting, deletion, and checkpoint behavior.

### 2. Store retained hint atoms in a compact immutable term table

Build this table only after Prover9 has discarded redundant input hints.  Do
not size it from Josef 04's 1.81 million input formulas when only about 846,000
remain active.

For each flattened symbol position store compact arrays containing:

- the symbol or normalized stored-variable number;
- the position immediately after the complete subterm;
- a 64-bit structural fingerprint;
- optionally a canonical 32-bit subterm ID when measured sharing justifies
  interning; and
- the owning stable hint ID at term boundaries.

The end position makes `skip-subterm` constant time.  A subterm ID makes most
`same-subterm` tests one integer comparison.  With fingerprints, equality is
confirmed by an exact compact-token comparison before a candidate is
accepted, so collisions cannot change semantics.

Use immutable geometrically sized arrays plus a small mutable delta for
back-demodulated/reinserted hints.  Stable-ID tombstones and measured stale
rebuilds preserve lifecycle behavior without pointer-rich per-hint nodes.

### 3. Compile each generated unit into a short matching program

Compilation is per generated clause and should use a reusable scratch buffer;
it creates no persistent objects.  Its operations are intentionally simple:

- `SIGN_ROOT`: select the sign and root-symbol population;
- `RIGID(path,symbol)`: require a symbol at a rigid path;
- `BIND(variable,path)`: remember the target subterm at the variable's first
  occurrence;
- `SAME(variable,path)`: require the same target subterm at a later occurrence;
- `LENGTH/ARITY`: apply exact cheap shape bounds where valid;
- `EMIT`: return a stable hint ID.

The compiler orders independent `RIGID` operations by measured rarity rather
than source-term order.  It delays broad `BIND` operations until rigid tests
have reduced the population.  Dependencies still constrain the order: a path
below a generated variable is not rigid and cannot be tested as though it
were.

This is best viewed as a tiny query plan.  The terms “path” and “rigid” mean,
respectively, a child-position route such as “second argument, then first
argument,” and a function/constant symbol fixed by the generated clause.

### 4. Execute the program over shared candidate blocks

Retained hints are grouped first by sign and root symbol.  Within a group,
compact sorted postings map a useful `(path,symbol)` test to stable hint IDs.
The executor starts with the rarest valid posting and intersects subsequent
postings in decreasing stable-ID order.

For the survivors it executes `BIND` and `SAME` using subterm end positions
and IDs/fingerprints.  Only survivors reach the existing exact matcher.  No
candidate clause is decompressed merely to discover that a fixed symbol or a
repeated variable is wrong.

Postings must be selective: create them only when a census predicts that the
bytes will repay enough exact tests or tree nodes.  Use sorted 32-bit IDs in
small blocks initially; evaluate delta coding only after measuring decode
cost.  A full `(every path,every symbol)` index is specifically out of scope.

### 5. Share compiled blocks, not only prefixes

Hints with the same useful tests and variable-equality pattern share one
instruction block.  Blocks use 32-bit offsets into flat arrays, not pointers.
Common tails may be shared as a directed acyclic graph after the first exact
implementation is measured.

Direct child arrays are allowed only for measured hot, wide dispatch points.
The present tree's maximum CHAT fan-out was 12 and most parents had about two
applicable children, so full alphabet-sized arrays are not the default.

### 6. Preserve a small explicit fallback

The first version handles ordinary positive and negative unit hints.  Keep
nonunit and `_AnyConst` hints on their current packed paths, then merge stable
IDs in decreasing order before final matching.  A generated nonunit clause
uses the nonunit path only.

Do not broaden scope to nonunit code trees until the unit matcher passes a
large held-out CPU gate.

## Implementation phases and stop/go gates

### Phase 0: measure what an ideal matcher could save

Add read-only interval counters to `packed_fast`:

- candidates after each existing filter;
- final exact-match attempts and their CPU;
- candidates removable by the rarest valid rigid path;
- candidates removable by repeated-variable subterm equality;
- distinct and repeated retained subterms;
- token-length and subterm-length distributions; and
- all values per 100 or 1,000 givens, not only at shutdown.

Gate: on at least two training problems, the proposed exact pretests must
remove enough work to permit either a 2x hint-matching speedup or a material
whole-search speedup.  If exact matching is already a small fraction of CPU,
stop before allocating a new index.

### Phase 1: compact term table and one rare-path posting

Implement subterm end positions and fingerprints, then one selected rigid-path
posting per root group.  Run in shadow and compare the complete ordered ID
stream with `packed_fast`.

Gate: zero semantic differences; incremental index memory below 128 MiB on
the retained Josef 04 bank, or below 15% of packed hint storage on smaller
problems; at least 4x fewer exact candidate checks on a training problem.

### Phase 2: compiled multi-test query plans

Add rarity-ordered posting intersections and repeated-variable `BIND/SAME`
instructions.  Keep allocations out of the query loop and report instructions
executed, IDs intersected, subterm comparisons, exact confirmations, and CPU.

Gate: identical traces and proof objects; at least 2x hint-matching CPU
improvement on two structurally different training problems; no more than 5%
whole-run CPU regression on any training case.

### Phase 3: lifecycle and authoritative mode

Add delta segments, tombstones, back-demodulation reinsertion, stale rebuild,
checkpoint reconstruction, `_AnyConst` merge tests, and equality-flip tests.
Then introduce an explicit option such as:

```text
assign(hint_index,packed_compiled).
```

Gate: exact matching-hint checksum and given/generated/kept counters through
bounded CHAT, Osborn, Josef 01, and Josef 02 comparisons.

### Phase 4: frozen held-out and long-run gates

Freeze code and thresholds before opening held-out outputs.  Run one process
at a time locally and use ar-2 for Josef 04 and multi-thousand-given tests.

Required evidence:

- Josef 01 and 02 at 300, then 1,000 givens;
- Josef 04 at 1,000, then 3,000 givens without hint dumping;
- CHAT and Osborn at a boundary large enough to expose interval slopes;
- exact proof/search agreement where the mode promises it;
- startup CPU, search CPU, maximum RSS, file-cache effects, and swap;
- instructions, candidates, and microseconds per query by interval.

Promotion gate: geometric-mean whole-run CPU improves materially, no held-out
case regresses more than 10%, hint matching improves at least 2x where it is a
major cost, and per-query costs do not trend upward with the retained bank.

## Resource discipline

- Keep all new options off by default.
- Use one memory-relevant local process at a time, a 2-GiB address-space cap
  for bounded prefixes, and short given/time limits first.
- Stop if swap activity appears.
- Reuse downloaded statistics; do not rerun week-long searches locally.
- Separate startup time from search time, because building an impressive
  index that never repays its construction cost is not a win.
- Commit construction, correctness, layout, traversal, and policy changes
  separately with detailed messages.

## Expected outcome

This plan has a plausible route to a large CPU improvement because it removes
the operation that defeated the first tree: walking many symbols and branches
under a generated variable.  It does not promise an 80-90% whole-prover CPU
reduction in advance.  Phase 0 is designed to calculate the maximum available
gain before implementation, and the project stops if the measured ceiling is
too low.

For RAM, the new index is deliberately bounded and built over retained hints.
The radical total-RAM reduction still comes primarily from file-backed passive
storage and compact clause/ancestor representations; the matcher must preserve
those gains rather than add another large resident copy of the hint bank.
