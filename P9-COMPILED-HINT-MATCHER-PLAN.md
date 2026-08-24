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

> Implementation update (2026-08-24): Phases 0 and 1, Phase 2, and the first
> candidate-generation part of Phase 2b are complete.  `packed_compiled`
> learns exact repeated-subterm
> and selective deep fixed-symbol conditions, promotes only conditions whose
> measured work can repay a dense stable-ID set, executes a bounded mixture
> of repeated-subterm and selective fixed-symbol instructions, and combines learned sets only for candidate
> blocks actually visited.  The explicit `packed_compiled_blocks` experiment
> now applies direct SAME/RIGID checks before a unique ID is appended, builds up to
> eight co-occurring hot masks in one retained-bank pass, and intersects
> learned masks inside `packed_fast`'s dense 64-ID posting loop before scalar
> IDs are enumerated.  Sparse and conjunction collectors precheck learned
> masks before profile work and candidate insertion.  Query programs are
> compiled during the existing full feature-mask traversal rather than by a
> second term walk.  A fixed 24 KiB normalized-shape policy cache reuses a
> deep-symbol instruction only after its full direct pass rejects at least
> 25%, revalidates it against the current exact plan, and evicts it if it
> stops paying.  Threshold-zero diagnostics use an exact compact
> SAME/RIGID cache identity.  At the normal threshold, narrow
> unfiltered results retain the small `packed_fast` key and broad filtered
> results are not cached under an incomplete identity.  The established
> compressed matcher remains the final authority.  The eager all-path
> experiment was not worthwhile and
> is isolated in `packed_compiled_paths`.  A demand-built canonical-bank mode,
> `packed_compiled_lazy`, saves construction on Osborn but loses when a search
> eventually touches most hints, so it too remains diagnostic.  Structural
> pruning is large, but bounded whole-run CPU is not yet a promotable win. See
> [P9-COMPILED-HINT-MATCHER-REPORT.md](P9-COMPILED-HINT-MATCHER-REPORT.md).

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

Build the permanent form only after Prover9 has discarded redundant input
hints.  Do not size it from Josef 04's 1.81 million input formulas when only
about 846,000 remain active.

The implemented table interns subterms into 12-byte canonical nodes with
children held as flat 32-bit handles.  Each ordinary unit hint has one root
handle and sign.  During input it uses a mutable hash table; finalization
freezes that base and releases the construction hash.  Later rewritten hints
go into a small mutable delta.  This provides:

- normalized stored variables and symbol/arity in canonical nodes;
- constant-time skipping through a whole subterm by following one handle;
- one 32-bit equality comparison for shared subterms;
- stable hint-ID roots and signs; and
- base/delta lifecycle support for back-demodulation and reinsertion.

Handles are exact, not hashes, so equality has no collision case.  Stable-ID
tombstones preserve safe deletion.  Equivalent input hints are rejected
before they enter this table.  The startup cost of incrementally hashing every
subterm of the large retained bank remains relevant.  `packed_compiled_lazy`
now constructs canonical roots directly from the retained compressed byte
streams as queries demand them, without materializing temporary terms.  This
reduces construction on Osborn, but a Josef 01 prefix needed every retained
unit and made the lazy mode slower.  It is therefore an explicit experiment,
not a universal replacement.  Pre-sizing from the raw input-hint count was
also measured and rejected: it overestimated Osborn's hash by one power of
two and was slower despite eliminating nine rebuilds.

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

Status: **complete; gate passed.**

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

Status: **term table complete; broad deep-path posting rejected.**

Implement subterm end positions and fingerprints, then one selected rigid-path
posting per root group.  Run in shadow and compare the complete ordered ID
stream with `packed_fast`.

Gate: zero semantic differences; incremental index memory below 128 MiB on
the retained Josef 04 bank, or below 15% of packed hint storage on smaller
problems; at least 4x fewer exact candidate checks on a training problem.

The canonical table passes unit and lifecycle tests and exact trace
comparisons.  It achieves the intended sharing and small memory footprint.
The depth-3--6 posting sidecar removed only about 13 additional candidates on
the measured Osborn prefix after the repeated-variable test, while adding
roughly 10 MiB and startup work.  It therefore fails the CPU/value part of the
gate and remains available only as `packed_compiled_paths` for diagnosis.

### Phase 2: compiled multi-test query plans

Status: **structural implementation complete; performance gate pending.**

Add rarity-ordered posting intersections and repeated-variable `BIND/SAME`
instructions.  Keep allocations out of the query loop and report instructions
executed, IDs intersected, subterm comparisons, exact confirmations, and CPU.

Implemented so far:

- discover all repeated generated variables without allocating in the hot
  loop;
- retain all valid repeated-variable conditions, order them by route cost,
  and execute up to eight in one short-circuiting direct pass;
- compare canonical subterm handles while sharing their common route prefix;
- skip the pretest below `hint_compiled_min_candidates` (default 128);
- sample at most 32 deep fixed-symbol conditions per admitted query, perform a
  full direct pass only when the best sample predicts at least 25% rejection,
  and avoid duplicating the exact depth-2 filtering already in `packed_fast`;
- learn exact repeated-subterm and fixed-symbol conditions during search and,
  after enough direct work, scan the live hint bank once to build a dense
  stable-ID membership set;
- compile up to eight learned conditions into one query program and combine
  their dense sets only for 64-ID blocks actually visited by the ordered
  candidate stream;
- use no query-sized bitmap scratch and hard-bound persistent dense sets to
  32 MiB by default, while separately capping learned metadata at 4,096
  conditions and 262,144 route words;
- keep additions complete and removals conservative across rewriting; and
- retain the compressed matcher as final authority for every survivor.

The opportunity census answered the main uncertainty.  At Josef 01/300, 937
of 970 admitted queries exposed several structural conditions and 665 mixed
SAME with RIGID conditions; Josef 02/300 reported 623 of 626 and 275,
respectively.  Both had a maximum latent width of nine.  The old width-one
behavior was therefore caused by training only the first condition, not by a
shortage of useful query structure.

The direct multi-SAME program converts that structure into 84.5% fewer exact
attempts on Josef 01/1,000.  However, sampled ordinary-match time improves
only from 9.175 to 6.507 seconds and whole CPU improves about 1.2%.  Candidate
generation still processes the same 147.5 million packed posting references
before the compiled filter runs.  Phase 2 passes semantics and demonstrates
the mechanism, but it does not pass the promotion gate.

Gate: identical traces and proof objects; at least 2x hint-matching CPU
improvement on two structurally different training problems; no more than 5%
whole-run CPU regression on any training case.

### Phase 2b: generate candidates with the compiled program

Status: **first implementation complete; bounded/long-run gate pending.**

Do not further tune the post-filter.  Integrate the useful conditions into
candidate generation so rejected stable IDs are not appended to the packed
candidate vector and, after a condition becomes hot, need not even be
enumerated from a dense posting word:

- time packed feature lookup, candidate marking/emission, structural checks,
  and compressed confirmation separately and by interval;
- normalize a query-shape key containing sign, shallow requirements, child
  routes, and repeated-variable relationships;
- combine existing shallow packed masks and selected SAME/RIGID masks for one
  visited 64-ID block before emitting IDs, preserving decreasing stable-ID
  order;
- build up to eight co-occurring hot conditions in one retained-bank scan,
  rather than scanning the bank independently for each condition;
- keep the aggregate persistent-mask budget at 32 MiB and preserve the
  base/delta/tombstone lifecycle; and
- run the new generator in shadow against the complete packed candidate and
  matching-hint traces before it may become authoritative.

The new explicit mode is:

```text
assign(hint_index,packed_compiled_blocks).
```

It now implements both SAME and selective deep RIGID conditions.  A query
plan is prepared during the established full packed-mask traversal.  Once the
configured candidate threshold is reached, the already-seen prefix is
filtered once and later unique IDs are tested before vector insertion.  If
exact condition masks have matured, the dense posting collector intersects
them one 64-ID word at a time; rejected bits are never converted to IDs.
Sparse posting vectors and conjunction profiles still map their stored
positions to stable IDs, but reject learned-mask misses before profile checks,
canonical-root resolution, or packed insertion.  Several conditions that
mature on the same query are allocated within the existing 32 MiB budget and
filled in one bank pass.  Unknown canonical roots, nonunit hints, and
`_AnyConst` hints receive conservative positive bits, so the optimization
cannot hide a possible match.  Later additions are added to every applicable
built mask, while stale positives from removal or rewriting remain harmless.

The packed result cache cannot reuse a structurally filtered candidate list
merely because two queries have the same shallow rigid features: `f(x,x)` and
`f(x,y)` are the minimal counterexample.  Threshold-zero diagnostic runs
therefore append an exact compact serialization of SAME path pairs to the
cache identity, while keeping the real posting-key count separate for
lifecycle validation.  At the normal nonzero threshold, narrow results that
never activate the compiled program retain the existing compact key.  Broad
results changed by the program are not stored in that result cache.  This
avoids both an incorrect alias and a large query-sized key in the common
long-run path.

Exact tests cover this cache-alias case; a generated 600-hint bank in which
two SAME conditions mature together; a separate RIGID-only 600-hint bank; a
three-cohort sparse posting case; and a retained conjunction profile.  Dense,
sparse, and conjunction candidate paths all remain trace-identical to
`packed_fast` while reporting pre-insertion rejections.  The repeated
RIGID-only query must also report a normalized-program cache hit, a profitable
store, and avoided selectivity-sample tests.

Also compare eager canonical construction with one sequential post-input
build directly from the exact retained compressed bank.  This is different
from the rejected raw-count pre-sizing experiment and from random on-demand
construction.  It must win startup CPU without assuming a Josef-specific
sharing ratio.

The first 300-given phase split is now available.  On Josef 01, `packed_fast`
spent an estimated 0.488 seconds generating candidates and 0.944 seconds
confirming them.  The compiled mode spent 0.373 seconds generating, 0.377
seconds in the separate structural pass, and 0.451 seconds confirming.  This
supports fusing the structural conditions into candidate generation: it can
remove a separate pass and preserve the confirmation saving.  It also limits
expectations—hint handling is too small a fraction of this short whole run for
any matcher alone to deliver a radical prover-wide speedup.

Gate: zero ordered-candidate differences in shadow; a material reduction in
candidate-generation CPU as well as confirmation CPU; at least 2x total hint
matching improvement on two problems; and a material whole-run CPU win at a
1,000-given boundary before any Josef 04 run.

The first 300-given adjacent comparisons against the old separate
`packed_compiled` postfilter are mixed but safe.  Josef 01 was effectively
flat (32.25 versus 32.15 total CPU seconds, about 472--473 MiB RSS).  Josef 02
improved from 17.94 to 16.60 total CPU seconds, and sampled ordinary hint
matching fell from 1.587 to 0.967 seconds.  Both pairs had identical final
Given/Generated/Kept counters.  No learned mask matured in either short
default-threshold run, so these results measure early candidate insertion,
not yet the long-run dense-block payoff.  They are enough to continue to a
frozen 1,000-given gate, not enough to promote the mode.

The first 600-given Josef 02 gate found and corrected a scaling error before
promotion.  The initial code activated a compiled program from the size of a
broad seed posting rather than from candidates actually emitted after the
other packed conditions.  It filled the 4,096-entry metadata ceiling and
recorded 110,113 denials.  The corrected code uses the real emitted-candidate
threshold: the same boundary (Given=601, Generated=524,799, Kept=26,671) used
105 metadata entries with zero denials.  One learned mask was applied to
2,294 visited 64-ID words and rejected 15,002 IDs before enumeration.

On a noisy, current-state candidate/control pair, blocks used 50.34 seconds
of total CPU and 216,464 KiB peak RSS, versus 52.89 seconds and 216,248 KiB
for the separate postfilter.  The corresponding Prover9 hint clocks were
4.33 and 4.54 seconds.  This roughly 5% whole-run improvement is encouraging,
but it is one machine-order-sensitive pair, not the frozen 1,000-given gate.

The next Josef 02/300 pair includes direct selective RIGID instructions and
single-pass plan compilation.  Candidate and control again stopped at
Given=301, Generated=127,774, Kept=7,823.  Blocks used 16.98 seconds total
CPU and 189,444 KiB peak RSS, versus 18.42 seconds and 189,892 KiB for the
separate postfilter.  It rejected 19,570 candidates with deep fixed-symbol
checks across 127 broad queries.  No learned mask matured, so this is evidence
for the direct RIGID path and removed query walks, not yet the long-run mask
payoff.

### Phase 3: lifecycle and authoritative mode

Status: **partly complete, but authority intentionally deferred.**

Add delta segments, tombstones, back-demodulation reinsertion, stale rebuild,
checkpoint reconstruction, `_AnyConst` merge tests, and equality-flip tests.
Then introduce an explicit option such as:

```text
assign(hint_index,packed_compiled).
```

The canonical base/delta table, stable-ID tombstones, late additions,
back-demodulated reinsertion, preview isolation, `_AnyConst` fallback, and
equality-flip trace coverage exist.  Mixed repeated-subterm/fixed-symbol
programs and the demand-built bank have matching trace coverage as well.
Despite the option name, the compiled
structure is currently a conservative prefilter.  It is not allowed to choose
the returned hint; the compressed matcher still does that.  This is deliberate
until the Phase-2 CPU gate is passed.

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
