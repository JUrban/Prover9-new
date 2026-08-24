# Compiled Hint Matcher Implementation Report

Date: 2026-08-24  
Branch: `compiled-hint-matcher`  
Production base: `faster-packed-fallback` (`1b03bcf`)

## Current decision

Keep using this for production and long comparison runs:

```text
assign(hint_index,packed_fast).
```

Use `packed_compiled` only for bounded experiments.  It now removes a large
amount of repeated-variable candidate work, but it has not yet passed the
whole-CPU promotion gate.  The current mode is safe: its new structures can
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
2. chooses the cheapest repeated pair, measured by the two child-route
   lengths;
3. walks the common route prefix only once;
4. compares the resulting canonical subterm handles; and
5. sends every survivor to the compressed exact matcher.

Here a “child route” is simply a sequence such as “second argument, then first
argument.”  No special ATP concept is hidden behind the term.

The direct pretest is skipped when the packed candidate population is below
`hint_compiled_min_candidates`, which defaults to 128.  This protects small
queries where another pass over the IDs is unlikely to pay.

### Adaptive collective cache

Long searches often generate the same repeated-position condition many
times.  The matcher learns an exact unordered pair of child routes.  It first
checks candidates directly and counts that work.  Once one route pair has:

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
sparse ID lists.  Metadata is separately capped at 4,096 learned route pairs
and 262,144 stored route words (1 MiB of route storage).  If a bitset or a
later matching hint cannot fit, or either metadata ceiling is reached, that
route pair falls back to direct exact-handle checking.  A partial set is never
used.

Lifecycle behavior is conservative:

- a removed or rewritten hint may leave a stale positive bit, but the active
  check and compressed matcher reject it;
- a new or reinserted hint is checked against every already-built route pair,
  preventing a false negative; and
- preview queries read existing cache state but neither train it nor change
  accounting.

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
of 0 disables collective bitsets but keeps the direct repeated-variable
pretest.  `set(hint_compiled_census)` is diagnostic and adds work; leave it
clear in CPU comparisons.

The shutdown statistics contain:

- `Compiled_hint_term_table`: canonical nodes, children, roots, real bytes,
  lifecycle counts, and handle-matcher work;
- `Compiled_hint_same_filter`: admitted queries, candidates before/after,
  direct handle comparisons, and admission skips; and
- `Compiled_hint_same_cache`: learned route pairs, hottest pair, builds,
  scans, hits, rejections, denials, and exact allocated/budgeted bytes.

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

On admitted repeated-variable queries:

| Problem/boundary | Queries | Candidates before | Candidates after | Rejected |
|---|---:|---:|---:|---:|
| CHAT/100 | 113 | 66,696 | 3,735 | 62,961 |
| Osborn/100 | 414 | 355,671 | 103,762 | 251,909 |

The reported compressed exact attempts fell from 77,227 to 14,468 on the
CHAT prefix and from 253,561 to 93,973 on Osborn.  These counters are the
strong result: the prefilter is removing the intended work.

The 100-given Osborn adaptive-cache run observed 27 distinct route pairs and
351,914 direct candidate comparisons in total, but built none.  That is
expected: repayment is decided per exact route pair, not from the misleading
sum over unrelated pairs.  Longer runs will report `maximum_queries` and
`maximum_candidate_work`, showing whether one pair approaches construction.

### CPU and RAM

Short-prefix wall/CPU measurements remain noisy and are not a promotion case:

- one adjacent CHAT/100 pair favored `packed_compiled` (15.43 versus 17.24
  user seconds), while another gated measurement regressed;
- the pinned Osborn/100 control used 22.24 user seconds, whereas current
  compiled variants used about 25.3--25.9 seconds;
- on that Osborn prefix, roughly 2.6--3.3 seconds of the regression was table
  construction/startup, and search CPU was also slightly worse.

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
stale removals, forced cache construction, and zero-budget fallback.

Bounded experiments were run one memory-relevant process at a time with a
2-GiB address-space cap.  The machine had old pages in swap but no active
swap-in/swap-out during the checks.

## Next implementation work

The next step should not be another threshold sweep.  It should complete the
collective program promised by the plan:

1. **Remove avoidable construction cost.** Equivalent input hints are already
   rejected before canonical insertion, and raw-count pre-sizing failed.
   Either bulk-build from an exact retained population or construct canonical
   roots on demand, so short runs do not pay for the full bank.
2. **Learn selective fixed-symbol tests.** Within one sign/root population,
   observe useful fixed child routes and build only those whose measured
   candidate work repays a bounded bitset.  Do not restore the eager all-path
   sidecar.
3. **Compile a short query program.** Combine a few independent fixed-symbol
   bitsets with one or more repeated-subterm conditions, ordered by measured
   selectivity.  Share programs for identical query shapes.
4. **Filter set-at-a-time.** Intersect machine-word blocks first, then run the
   exact compressed matcher only on surviving stable IDs.  This is the
   Waldmeister-like part: one compact program acts on a population instead of
   walking one pointer-rich term tree per candidate.
5. **Freeze before long gates.** After bounded CHAT/Osborn/Josef tests pass,
   freeze thresholds and use ar-2 for Josef 04 at 1,000 and 3,000 givens,
   without hint dumping.  Record startup CPU, search CPU, RSS, cache builds,
   and interval cost slopes.

Promotion still requires at least a 2× hint-matching CPU improvement on two
different problems, no training regression above 5%, no held-out regression
above 10%, exact search/proof agreement, and no upward per-query slope as the
run matures.

The radical 80--90% total-RAM goal remains primarily a passive-clause-storage
problem.  This matcher is designed to preserve that gain: canonical sharing
replaces duplicate term structure, learned indexes are bounded, and the
compressed hint bank remains authoritative until a replacement has earned
promotion.
