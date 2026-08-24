# Compiled Hint Matcher Implementation Report

Date: 2026-08-24  
Branch: `compiled-hint-matcher`  
Production base: `faster-packed-fallback` (`1b03bcf`)

## Current decision

Keep using this for production and long comparison runs:

```text
assign(hint_index,packed_fast).
```

Use `packed_compiled` or `packed_compiled_blocks` only for bounded experiments.
They now remove a large
amount of repeated-variable and deep fixed-symbol candidate work, but it has
not yet passed the whole-CPU promotion gate.  The current mode is safe: its new structures can
only reject candidates that fail a necessary condition, and the established
compressed matcher still chooses and returns the hint.

This distinction matters.  We have implemented the useful foundation for a
Waldmeister-like, set-at-a-time matcher; we have not yet demonstrated that its
construction and query costs beat `packed_fast` on long runs.

## What is implemented

### Opportunity census

The opt-in `hint_compiled_census` instrumentation measures which exact
candidate failures could have been avoided by checking fixed symbols or
repeated generated variables first.  It also measures retained subterm
sharing and samples exact matcher CPU.  Phase 0 found a real target:
fixed-symbol and repeated-variable conditions reject 89.95--99.57% of
profiled unit candidates on the bounded CHAT, Osborn, Josef 01, and Josef 02
inputs.

### Canonical retained-term table

Ordinary unit hints are represented by shared canonical subterms:

- each node is 12 bytes;
- child links are flat 32-bit handles rather than pointers;
- normalized stored variables make alpha-equivalent subterms share;
- each stable hint ID has one root handle and sign;
- the finalized input bank is immutable and releases its construction hash;
- rewritten or newly indexed hints use a mutable delta; and
- removals use stable-ID tombstones.

Two equal subterms therefore normally compare as one 32-bit handle.  There is
no fingerprint collision case: handles denote exact canonical nodes.

The table also has an allocation-free exact unit matcher.  It was first run in
shadow against the compressed matcher.  Although those comparisons pass, the
current production candidate path deliberately keeps the compressed matcher
as final authority.

### Repeated-variable query plan

For a generated term such as `f(x,x)`, any matching retained term must have
the same subterm in the two corresponding child positions.  The compiled mode
now:

1. finds repeated generated variables in reusable scratch storage;
2. orders their equality requirements by child-route cost;
3. executes up to eight requirements in one short-circuiting candidate pass;
4. walks each common route prefix only once and compares the resulting
   canonical subterm handles; and
5. sends every survivor to the compressed exact matcher.

Here a “child route” is simply a sequence such as “second argument, then first
argument.”  No special ATP concept is hidden behind the term.

The direct pretest is skipped when the packed candidate population is below
`hint_compiled_min_candidates`, which defaults to 128.  This protects small
queries where another pass over the IDs is unlikely to pay.

### Demand-learned fixed symbols and mixed programs

For an admitted query, the compiler also records at most 32 fixed symbols
below the depth already covered exactly by `packed_fast`.  It tests at most
eight evenly spaced candidates per position, chooses one condition predicted
to reject at least 25%, and performs only that one full direct pass.  Thus the
policy learns `RIGID(child route,symbol)` conditions from actual search work;
it does not rebuild the rejected all-path sidecar.

Previously learned SAME and RIGID conditions are ordered by measured
selectivity and compiled into a program of at most eight dense stable-ID
sets.  A multi-condition program combines the sets once for each 64-ID block
actually visited by the ordered candidate stream.  It never sweeps a bitmap
proportional to the whole retained bank and needs no query-sized intersection
scratch.  Candidate order is preserved for the exact compressed matcher.

### Adaptive collective cache

Long searches often generate the same structural condition many times.  The
matcher learns exact SAME route pairs and RIGID route/symbol keys.  It first
checks candidates directly and counts that work.  Once one condition has:

- been observed at least twice; and
- accumulated at least `active hints × hint_compiled_cache_build_factor`
  candidate comparisons,

it scans the live canonical hint bank once and builds a dense stable-ID
membership bitset.  The default build factor is 2, so construction is delayed
until the measured direct work is large enough to repay two bank scans.
Later queries filter candidate IDs by bit lookup instead of repeatedly walking
the retained terms.

The dense-bit payload has a hard aggregate budget, controlled by
`hint_compiled_cache_kb` and defaulting to 32768 KiB.  It stores no duplicate
sparse ID lists.  Metadata is separately capped at 4,096 learned conditions
and 262,144 stored route words (1 MiB of route storage).  If a bitset or a
later matching hint cannot fit, or either metadata ceiling is reached, that
condition falls back to direct exact-handle checking.  A partial set is never
used.

### Normalized query-program policy cache

Blocks mode also has a fixed 1,024-entry policy cache.  Each entry is 24
bytes, for a total payload of 24 KiB.  Two 64-bit shape hashes are accumulated
during the existing feature-mask traversal over symbol/arity tokens and
first-occurrence-normalized variables; operation and literal sign are
included.

The cache does not store matching hints or reuse a previous candidate answer.
After a selected deep fixed-symbol instruction rejects at least 25% in its
full direct pass, the cache remembers that instruction for the normalized
query shape.  On reuse, the stored condition is compared exactly with the
current query's path and symbol before it can run.  Consequently, even a
shape-hash collision cannot create a false negative: the suggested condition
must independently be a necessary condition of the current query.  A reused
choice whose full rejection rate falls below 25% is evicted.

Lifecycle behavior is conservative:

- a removed or rewritten hint may leave a stale positive bit, but the active
  check and compressed matcher reject it;
- a new or reinserted hint is checked against every already-built condition,
  preventing a false negative; and
- preview queries read existing cache state but neither train it nor change
  accounting.

### Candidate-emission and dense-block experiment

`packed_compiled_blocks` moves repeated-variable and selected deep
fixed-symbol checks into candidate generation.  In plain language, the
separate mode no longer always “builds a
large list and then throws most of it away”:

1. it prepares repeated-variable child routes and bounded deep-symbol routes
   during the existing full packed-mask traversal;
2. after the configured threshold, it filters the small prefix already seen;
3. it checks subsequent unique IDs before appending them to the candidate
   vector; and
4. once a condition has earned a dense mask, it ANDs up to eight such masks
   with each visited 64-ID packed posting word before extracting scalar IDs.

Sparse posting vectors cannot skip ID enumeration because IDs are their
storage format.  They now apply a learned mask after ordinary feature
membership but before profile checks, canonical-root resolution, and packed
insertion.  Conjunction profiles similarly map a surviving profile position
to its stable ID and precheck the learned mask before literal-count work.

Co-occurring conditions that become profitable together are built together.
Their masks remain independent, but one retained-bank traversal resolves each
canonical hint root and fills all ready masks.  The existing 32 MiB aggregate
budget is unchanged.

Dense masks conservatively include hints that the canonical unit table cannot
represent, including nonunits and `_AnyConst`.  Later additions are installed
in every built mask; removals and rewrites may leave positive stale bits but
can never create a false negative.  Every surviving ID still reaches the
compressed matcher, which remains the final authority.

The established packed result cache also needed a correctness change in this
mode.  Queries such as `f(x,x)` and `f(x,y)` can have identical shallow packed
features but different repeated-variable requirements.  In threshold-zero
diagnostic runs, blocks mode adds an exact compact encoding of the SAME
child-route pairs to the cache identity.  It stores full equality data, not
only a collision-prone hash, and validates dependencies using only actual
posting keys.  At the normal nonzero threshold, narrow results that never
activate structural filtering retain `packed_fast`'s small key.  A broad
result changed by structural filtering is not stored in this cache, because
the small key does not identify it exactly and copying a large structural key
on every broad query would recreate the scaling cost the experiment is meant
to remove.

### Demand-built canonical bank experiment

`packed_compiled_lazy` leaves the canonical table empty while input hints are
loaded.  An admitted broad query resolves a retained target directly from the
existing compressed preorder byte stream, without allocating a temporary
term or recompressing the clause.  Missing roots are conservative fallbacks.
If a learned bitset earns a full-bank scan, the completed base is sealed and
its construction hash/scratch is released; later rewrites use the delta.

This is an explicit experiment, not a new default.  It helps when the search
touches only part of the bank, but it is slower when most hints are eventually
decoded.  The measurements below demonstrate both cases.

### Rejected post-input sequential build

One further experiment moved eager canonical construction to the existing
boundary after all initial hints have been checked for equivalence.  It then
scanned active stable IDs and streamed retained compact units into the shared
term table before processing the initial SOS.  Compiled filtering remained
dormant until that build completed, so the ordinary packed matcher alone
decided initial hint redundancy.

The mechanism was exact and needed no materialization on Josef 02: it scanned
136,486 active hints, skipped 51 nonunits, and streamed 136,435 units.  It did
not, however, remove real work.  The eager path already adds a hint only after
the equivalence check accepts it.  Both versions consequently ended with the
same canonical node, child, occurrence, hash, and byte counts.

On the first adjacent startup-only pair, eager construction used 11.01 total
CPU seconds and the post-input version used 11.14; peak RSS was 182,860 and
182,700 KiB respectively.  A second pair was substantially disturbed by host
noise and supplied no evidence of a deferred win.  At 300 givens the deferred
version reached the exact same Given=301, Generated=127,774, Kept=7,823
boundary, but used 18.05 total CPU seconds versus 16.98 in the preceding
blocks measurement.  That pair is also too timing-noisy to claim a regression,
but plainly does not establish a benefit.

The implementation was committed for inspection and then reverted.  This is
an important negative result: a useful deterministic bulk builder must change
how hash-consing is performed (for example, a bounded sampled reservation or
sort-and-merge construction), not just when the same insertions occur.  Such
a builder needs its own startup CPU and peak-memory gate and must not assume a
fixed Josef-specific sharing ratio.

### Rejected deep-path sidecar

`packed_compiled_paths` additionally builds eager postings for fixed symbols
at depths 3--6.  On the bounded Osborn bank this cost roughly 10 MiB and more
startup time.  Once the repeated-variable condition had run, it rejected only
about 13 additional candidates.  That is not a useful trade and the mode is
retained only for diagnosis.  It should not be used for long runs.

## Modes and options

The useful modes are:

```text
% Production/control:
assign(hint_index,packed_fast).

% Build the canonical table and cross-check its exact matcher in shadow:
assign(hint_index,packed_compiled_shadow).

% Experimental repeated-variable prefilter, compressed final authority:
assign(hint_index,packed_compiled).

% Experimental SAME checks during ID emission and learned 64-ID block masks:
assign(hint_index,packed_compiled_blocks).

% Experimental demand-built form of the same prefilter:
assign(hint_index,packed_compiled_lazy).

% Negative deep-path experiment; normally do not use:
assign(hint_index,packed_compiled_paths).
```

For a bounded candidate run, replace only the old `hint_index` assignment and
leave the successful OTTER/compact/passive-storage configuration unchanged:

```text
assign(hint_index,packed_compiled).
assign(hint_compiled_min_candidates,128).
assign(hint_compiled_cache_build_factor,2).
assign(hint_compiled_cache_kb,32768).

set(hint_match_stats).
assign(stats,all).
```

The three numeric assignments show the defaults and may be omitted.  A build
factor of 0 is useful only for a tiny forced-construction test.  A cache budget
of 0 disables collective bitsets but keeps direct SAME/RIGID pretests.
`set(hint_compiled_census)` is diagnostic and adds work; leave it
clear in CPU comparisons.

To exercise the candidate-emission implementation, use the same surrounding
configuration and replace only that one line:

```text
assign(hint_index,packed_compiled_blocks).
```

Do not combine it with `packed_compiled_lazy`: block masks require a complete
canonical table.  The numeric defaults above remain applicable.  A build
factor of zero forces construction for tests and should not be used for a
real long search.

The shutdown statistics contain:

- `Compiled_hint_term_table`: canonical nodes, children, roots, real bytes,
  lifecycle counts, and handle-matcher work;
- `Compiled_hint_same_filter`: admitted queries, candidates before/after,
  direct handle comparisons, and admission skips; and
- `Compiled_hint_rigid_filter`: sampled fixed positions, admitted scans, and
  exact fixed-symbol rejections;
- `Compiled_hint_plan`: structural conditions available before learning,
  including multi-condition and mixed-condition query counts;
- `Compiled_hint_program`: learned dense instructions combined and candidate
  blocks visited (the scratch counters remain zero); and
- `Compiled_hint_same_cache`: learned route pairs, hottest pair, builds,
  scans, batch widths, hits, rejections, denials, and exact
  allocated/budgeted bytes; and
- `Compiled_hint_blocks`: activations, prefix work, early scalar rejections,
  dense posting words intersected, bits rejected before ID enumeration, and
  sparse/conjunction mask prechecks; and
- `Compiled_hint_query_program_cache`: bounded shape-policy lookups, hits,
  condition-validation misses, stores, replacements, evictions, and avoided
  fixed-position sample tests.

## Measurements so far

### Phase-0 opportunity

| Problem | Boundary | Profiled units | Rejected by fixed/repeated conditions | Exact-loop CPU | Whole packed-match CPU |
|---|---:|---:|---:|---:|---:|
| CHAT | 100 givens | 77,453 | 96.76% | 0.094 s | 0.133 s |
| Osborn | 100 givens | 279,144 | 98.33% | 2.900 s | 3.411 s |
| Josef 01 | 300 givens | 1,047,519 | 99.57% | 0.998 s | 1.509 s |
| Josef 02 | 300 givens | 326,812 | 89.95% | 0.430 s | 1.290 s |

This says the opportunity is large on Osborn and meaningful on Josef 01.  It
also says a perfect hint matcher cannot materially accelerate the measured
CHAT prefix, because only 0.13 seconds was in ordinary hint matching there.

### Sharing in the retained bank

| Problem | Retained hints | Unit term nodes before sharing | Distinct subterms | Sharing ratio |
|---|---:|---:|---:|---:|
| CHAT | 62,805 | 1,626,386 | 171,082 | 9.506× |
| Osborn | 219,171 | 4,954,480 | 594,455 | 8.334× |
| Josef 01 | 96,678 | 2,530,398 | 283,176 | 8.936× |
| Josef 02 | 136,486 | 2,902,091 | 351,049 | 8.267× |

This justified canonical sharing.  It did not justify a second large eager
index over every path.

### Current structural pruning

On the latest corrected bounded runs:

| Problem/boundary | Condition | Queries | Candidates before | Candidates after | Rejected |
|---|---|---:|---:|---:|---:|
| CHAT/100 | SAME | 128 | 72,319 | 4,586 | 67,733 |
| CHAT/100 | RIGID | 0 | 0 | 0 | 0 |
| Osborn/100 | SAME | 397 | 344,018 | 94,077 | 249,941 |
| Osborn/100 | RIGID | 63 | 9,632 | 6,192 | 3,440 |
| Josef 01/300 | SAME | 970 | 966,789 | 22,018 | 944,771 |
| Josef 01/300 | RIGID | 7 | 1,842 | 84 | 1,758 |
| Josef 01/1,000 | SAME | 3,093 | 3,215,969 | 81,664 | 3,134,305 |
| Josef 01/1,000 | RIGID | 44 | 18,260 | 804 | 17,456 |
| Josef 02/300 | SAME | 626 | 299,127 | 7,116 | 292,011 |
| Josef 02/300 | RIGID | 1 | 196 | 111 | 85 |

Earlier SAME-only measurements reduced compressed exact attempts from 77,227
to 14,468 on the CHAT prefix and from 253,561 to 93,973 on Osborn.  These
counters are the strong result: the prefilter is removing the intended work.

The 100-given Osborn adaptive-cache run observed 37 condition keys and
349,898 direct candidate comparisons in total, but built none.  That is
expected: repayment is decided per exact condition, not from the misleading
sum over unrelated conditions.  Longer runs report `maximum_queries` and
`maximum_candidate_work`, showing whether one condition approaches construction.

The new plan census showed that the old width-one behavior was a learning
policy failure, not a property of the clauses.  On Josef 01/300, 937 of 970
admitted queries exposed at least two structural conditions, 665 exposed both
SAME and RIGID conditions, and the maximum latent width was nine.  Josef
02/300 reported 623 of 626 multi-condition queries, 275 mixed queries, and the
same maximum width.

The bounded direct SAME program now executes those co-occurring requirements
without waiting for dense construction.  Dense reuse remains narrow: at
Josef 01/1,000 three SAME conditions earned sets and served 730 queries, while
no RIGID condition matured and every dense program still had width one.  The
direct program nevertheless applied 6,744 SAME instructions across 3,093
queries and rejected 3,134,305 candidates.

### CPU and RAM

Short-prefix wall/CPU measurements remain noisy and are not a promotion case.
The latest adjacent or same-build observations are:

- corrected eager Osborn/100 used 26.01 user seconds; lazy used 24.59 with
  identical match candidates and positives;
- eager Josef 01/300 used 14.19 user seconds; direct-stream lazy used 17.56,
  because all 96,225 retained unit hints were eventually needed;
- eager Josef 02/300 used 13.87 user seconds; and
- CHAT/100 used 17.76 user seconds, but hint matching itself remained too
  small a CPU fraction to establish a promotion.

The first frozen 1,000-given pair was Josef 02 on the same binary, filesystem,
and limits.  Both modes reached Given=1,001, Generated=1,310,234, and
Kept=65,416.  `packed_compiled` reduced ordinary-match direct attempts from
1,515,546 to 860,556 (43.2%) but used 71.54 user seconds versus 71.51 for
`packed_fast`.  Peak RSS was 248,612 KiB versus 235,128 KiB.  This passes the
semantic and bounded-resource gates but supplies no CPU promotion case.

On the latest adjacent Josef 01/300 pair, the direct multi-SAME program cut
ordinary-match attempts from 1,085,836 to 157,712 (85.5%) and sampled
ordinary-match time from 3.046 seconds to 0.731--0.892 seconds across two
candidate runs.  The complete compiled runs took 17.55--17.78 seconds wall
time versus 16.36 for `packed_fast`: canonical-table startup still cost more
than the short search saved.

The exact-build Josef 01/1,000 pair had identical search counters:
Given=1,001, Generated=1,628,048, and Kept=320,239.  The compiled mode reduced
ordinary-match direct attempts from 3,632,023 to 562,319 (84.5%) and sampled
ordinary-match time from 9.175 to 6.507 seconds.  Combined user and system CPU
improved only from 67.99 to 67.16 seconds (1.2%); wall time changed from 68.16
to 67.37 seconds.  Peak RSS rose from 600,796 to 620,524 KiB.  This proves that
startup is repaid by 1,000 givens, but it is still far below the intended
whole-run improvement.

The first candidate-emission pairs compared `packed_compiled_blocks` with the
separate `packed_compiled` postfilter at 300 givens:

| Problem | Mode | Total CPU | Wall | Peak RSS | Sampled ordinary match |
|---|---|---:|---:|---:|---:|
| Josef 01 | separate postfilter | 32.25 s | 32.63 s | 472,460 KiB | 0.308 s |
| Josef 01 | candidate emission | 32.15 s | 32.30 s | 473,188 KiB | 0.332 s |
| Josef 02 | separate postfilter | 17.94 s | 18.01 s | 189,572 KiB | 1.587 s |
| Josef 02 | candidate emission | 16.60 s | 16.68 s | 189,764 KiB | 0.967 s |

All four runs stopped at the same final search boundary as their pair.
Josef 01 ended at Given=301, Generated=120,793, Kept=5,737; Josef 02 ended at
Given=301, Generated=127,774, Kept=7,823.  No default-threshold condition mask
matured in these short runs.  Thus Josef 02's roughly 7.5% total-CPU gain is
promising evidence for early insertion filtering, while Josef 01 is an honest
neutral result; neither measures the new long-run dense-block path or passes
the 1,000-given promotion gate.

The first 600-given Josef 02 run exposed a real scaling bug.  The initial
dense path used the cardinality of its broad seed posting as a proxy for the
configured 128-candidate activation threshold.  That posting was much larger
than the stream remaining after the other packed tests, so the run filled all
4,096 compiled metadata entries and denied 110,113 later entries.  Activation
now has one authority: unique candidates actually presented for emission.

After that correction, candidate and control both stopped at Given=601,
Generated=524,799, and Kept=26,671.  The candidate used 105 metadata entries
with zero metadata denials.  One learned mask was applied to 2,294 visited
64-ID words and rejected 15,002 IDs before they became scalar candidate IDs.
The current-state runs were:

| Mode | Total CPU | Wall | Peak RSS | Prover9 hint clock |
|---|---:|---:|---:|---:|
| separate postfilter | 52.89 s | 53.19 s | 216,248 KiB | 4.54 s |
| candidate emission | 50.34 s | 50.54 s | 216,464 KiB | 4.33 s |

This approximately 5% whole-run and hint-clock improvement is useful bounded
evidence.  The host was timing-noisy and the pair was candidate-then-control,
so it is not a promotion result.  More importantly, the 600-given gate caught
and removed a policy that would have deteriorated as queries accumulated.

The follow-up Josef 02/300 pair adds selective RIGID instructions to the
before-emission executor and compiles the query plan during the existing
feature-mask traversal.  Both modes ended at Given=301, Generated=127,774,
Kept=7,823:

| Mode | Total CPU | Wall | Peak RSS |
|---|---:|---:|---:|
| separate postfilter | 18.42 s | 18.42 s | 189,892 KiB |
| SAME/RIGID candidate emission | 16.98 s | 17.12 s | 189,444 KiB |

The candidate rejected 19,570 clauses with selected fixed-symbol checks
across 127 broad queries.  No default-threshold mask matured, so the roughly
8% pairwise CPU difference measures direct early RIGID checks plus removal of
redundant term traversals.  It remains a short, timing-noisy sanity pair.

Josef 02/300 confirmed that extra necessary conditions are not automatically
valuable: direct attempts fell from 139,308 to 86,377, while sampled ordinary
match time remained 0.990 versus 0.988 seconds and whole CPU was unchanged.
The extra handle checks merely replaced already-cheap compressed checks.

The new phase timer explains where the Josef 01/300 matcher win goes.  On one
adjacent pair, `packed_fast` used an estimated 0.488 seconds to generate
candidates and 0.944 seconds to confirm them; its nominal 0.100-second filter
interval contains only timing-boundary overhead.  `packed_compiled` used
0.373 seconds for candidate generation, 0.377 for its separate structural
pass, and 0.451 for confirmation.  The program roughly halves confirmation
but pays for a second traversal of the candidate vector.  Phase 2b is intended
to fold those structural operations into the existing block loop, not add a
third index traversal.

On Osborn, lazy constructed 103,171 of 248,809 cumulative canonical
additions and used 15.2 MiB for the term table versus eager's 20.0 MiB.  Both
processes were about 301 MiB RSS because other structures dominate.  On
Josef 01 the lazy scan built the complete 6.5 MiB table and raised total time;
this rejects lazy construction as a universal policy.

A follow-up pre-sizing experiment reduced the immutable-base hash from ten
geometric builds to one, but the raw hint count selected 2,097,152 slots for
594,455 final base nodes.  It used 29.18 user seconds versus 23.91 for an
adjacent ordinary-growth run; both ended at Given=101, Generated=10,412, and
Kept=1,401.  Their detailed hint-query counts differed, so this is not a clean
paired CPU proof, but it supplies no reason to retain the larger allocation.
The pre-sizing code was removed; only the rebuild counter remains.

RSS did not show a concerning increase on the pinned Osborn pair (about
300.8--300.9 MiB in both runs); a CHAT pair showed roughly 7 MiB more.  Those
small-prefix RSS observations are compatible with the compact table but do
not replace a Josef 04 retained-bank measurement.  The adaptive cache's dense
payload is hard-bounded at 32 MiB by default, with small fixed ceilings on
entries, buckets, and copied child routes.

The honest conclusion is: **semantic and structural gates pass; the CPU gate
does not yet pass.**

## Correctness and resource evidence

The current checkpoint passes:

```text
test.src/hint_compiled_census_test.sh
test.src/hint_index_trace_test.sh
test.src/hint_term_table_test
test.src/hint_preview_test
```

Coverage includes exact trace agreement with the established index modes,
positive/negative units, equations/disequations, `_AnyConst`, equality
flipping, preview isolation, match-once and expiry lifecycle, late additions,
stale removals, forced cache construction, mixed SAME/RIGID word programs,
zero-budget fallback, lazy byte-stream construction, and the case where an
invalid SAME route precedes a later valid candidate.  It now also includes
the `f(x,x)`/`f(x,y)` result-cache alias case, batched construction of two
co-occurring SAME masks, conservative fallback membership, and pre-ID dense
block intersection on a 600-hint generated bank.  A second 600-hint fixture
has no repeated query variables and proves RIGID-only dense filtering.  A
three-cohort fixture forces the sparse vector collector, and a compact
retained profile forces the conjunction collector; both require nonzero
learned-mask rejections and exact `packed_fast` trace agreement.  The
RIGID-only fixture also requires a profitable normalized-program store, a
later hit, and avoided sample work.

Bounded experiments were run one memory-relevant process at a time with a
2-GiB address-space cap.  The machine had old pages in swap but no active
swap-in/swap-out during the checks.

## Next implementation work

The new bottleneck is architectural and visible in the Josef 01/1,000
counters.  `packed_fast` first examined 147.5 million posting references and
constructed its ordered candidate stream.  Only then did the compiled program
discard 84.5% of exact attempts.  Making the post-filter still stricter cannot
avoid the work that has already happened.

The next Waldmeister-like stage should therefore be:

1. **Split candidate-generation time from confirmation time.** Add sampled
   interval timers around packed feature lookup, candidate marking/emission,
   compiled structural tests, and compressed confirmation.  This establishes
   the remaining ceiling without relying on aggregate `hints` time.
2. **Measure the implemented conditions-before-emission paths at scale.**
   Blocks mode now handles direct SAME and selected RIGID checks, learned
   masks in dense posting words, and mask prechecks in sparse/conjunction
   collectors.  The next 1,000-given gate must show whether these paths are
   reused often enough to repay training and mask construction.
3. **Measure and extend normalized query-program reuse.** The first bounded
   cache now normalizes operation, sign, symbols/arities, and variable
   occurrence pattern during the existing traversal, then reuses only a
   fixed-symbol instruction whose full work rejected at least 25%.  The
   1,000-given gate should decide whether hits justify caching a wider ordered
   SAME/RIGID instruction vector.  The compressed matcher remains final
   authority.
4. **Measure and generalize batched construction.** Blocks mode now scans the
   retained bank once for up to eight co-occurring ready SAME or selected
   RIGID instructions.  Keep this only if long runs report useful batch
   widths and amortization.
5. **Change, rather than reschedule, startup construction.** The sequential
   post-input build was exact but CPU/RSS-neutral and has been reverted.  Any
   successor must reduce the cost of hash-consing itself under an explicit
   temporary-memory cap.  The rejected raw-input pre-sizing policy must not
   return, and lazy construction remains diagnostic because Josef 01
   eventually touches every unit.
6. **Gate in shadow before authority.** Require exact ordered candidate and
   hint traces on CHAT, Osborn, Josef 01, and Josef 02; then run adjacent
   300/1,000-given CPU pairs.  Only a version that materially improves total
   CPU proceeds to Josef 04 on ar-2.

In plain language, the blocks mode now applies the compiled code while making
the list and, for learned dense conditions, before turning packed words into
individual IDs.  The next question is whether long runs reuse those learned
blocks enough to repay their construction.

Promotion still requires at least a 2× hint-matching CPU improvement on two
different problems, no training regression above 5%, no held-out regression
above 10%, exact search/proof agreement, and no upward per-query slope as the
run matures.

The radical 80--90% total-RAM goal remains primarily a passive-clause-storage
problem.  This matcher is designed to preserve that gain: canonical sharing
replaces duplicate term structure, learned indexes are bounded, and the
compressed hint bank remains authoritative until a replacement has earned
promotion.
