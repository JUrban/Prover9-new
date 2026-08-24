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

### Demand-learned fixed symbols and mixed programs

For an admitted query, the compiler also records at most 32 fixed symbols
below the depth already covered exactly by `packed_fast`.  It tests at most
eight evenly spaced candidates per position, chooses one condition predicted
to reject at least 25%, and performs only that one full direct pass.  Thus the
policy learns `RIGID(child route,symbol)` conditions from actual search work;
it does not rebuild the rejected all-path sidecar.

Previously learned SAME and RIGID conditions are ordered by measured
selectivity and compiled into a program of at most eight dense stable-ID
sets.  A multi-condition program intersects 64 IDs at a time in reusable
scratch, then preserves the original candidate-vector order for the exact
compressed matcher.  If the scratch cannot fit, it falls back to bounded
per-candidate bit membership tests.  Scratch and persistent dense sets share
one hard cache budget.

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

The dense-bit payload and word-intersection scratch have a hard aggregate
budget, controlled by `hint_compiled_cache_kb` and defaulting to 32768 KiB.
It stores no duplicate sparse ID lists.  Metadata is separately capped at
4,096 learned conditions
and 262,144 stored route words (1 MiB of route storage).  If a bitset or a
later matching hint cannot fit, or either metadata ceiling is reached, that
condition falls back to direct exact-handle checking.  A partial set is never
used.

Lifecycle behavior is conservative:

- a removed or rewritten hint may leave a stale positive bit, but the active
  check and compressed matcher reject it;
- a new or reinserted hint is checked against every already-built condition,
  preventing a false negative; and
- preview queries read existing cache state but neither train it nor change
  accounting.

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
of 0 disables collective bitsets but keeps the direct repeated-variable
pretest.  `set(hint_compiled_census)` is diagnostic and adds work; leave it
clear in CPU comparisons.

The shutdown statistics contain:

- `Compiled_hint_term_table`: canonical nodes, children, roots, real bytes,
  lifecycle counts, and handle-matcher work;
- `Compiled_hint_same_filter`: admitted queries, candidates before/after,
  direct handle comparisons, and admission skips; and
- `Compiled_hint_rigid_filter`: sampled fixed positions, admitted scans, and
  exact fixed-symbol rejections;
- `Compiled_hint_program`: dense instructions combined, word operations,
  fallback membership tests, and jointly budgeted scratch; and
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

On the latest corrected bounded runs:

| Problem/boundary | Condition | Queries | Candidates before | Candidates after | Rejected |
|---|---|---:|---:|---:|---:|
| CHAT/100 | SAME | 128 | 72,319 | 4,586 | 67,733 |
| CHAT/100 | RIGID | 0 | 0 | 0 | 0 |
| Osborn/100 | SAME | 397 | 344,018 | 94,077 | 249,941 |
| Osborn/100 | RIGID | 63 | 9,632 | 6,192 | 3,440 |
| Josef 01/300 | SAME | 970 | 966,789 | 95,899 | 870,890 |
| Josef 01/300 | RIGID | 56 | 24,701 | 5,152 | 19,549 |
| Josef 01/1,000 | SAME | 3,093 | 3,215,969 | 213,670 | 3,002,299 |
| Josef 01/1,000 | RIGID | 119 | 56,188 | 6,348 | 49,840 |
| Josef 02/300 | SAME | 626 | 299,127 | 85,279 | 213,848 |
| Josef 02/300 | RIGID | 18 | 2,955 | 943 | 2,012 |
| Josef 02/1,000 | SAME | 2,872 | 1,110,410 | 321,525 | 788,885 |
| Josef 02/1,000 | RIGID | 40 | 9,592 | 3,133 | 6,459 |

Earlier SAME-only measurements reduced compressed exact attempts from 77,227
to 14,468 on the CHAT prefix and from 253,561 to 93,973 on Osborn.  These
counters are the strong result: the prefilter is removing the intended work.

The 100-given Osborn adaptive-cache run observed 37 condition keys and
349,898 direct candidate comparisons in total, but built none.  That is
expected: repayment is decided per exact condition, not from the misleading
sum over unrelated conditions.  Longer runs report `maximum_queries` and
`maximum_candidate_work`, showing whether one condition approaches construction.

At Josef 02/1,000, two SAME conditions finally earned dense sets.  They were
used by 300 queries and rejected 119,333 candidates.  No RIGID condition
earned a set, and every executed program still had width one.  The collective
executor therefore remained functionally a one-condition lookup on this
prefix; it did not yet exercise a word-wise intersection.

Josef 01/1,000 showed the same qualitative limitation at greater scale.  Three
SAME conditions earned dense sets and served 730 queries, rejecting 1,493,035
candidates through the cache.  No RIGID condition matured and every program
again had width one.  The direct structural pass nevertheless rejected more
than three million candidates before exact matching.

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

The adjacent Josef 01/1,000 pair also had identical search counters:
Given=1,001, Generated=1,628,048, and Kept=320,239.  Here the compiled mode
reduced ordinary-match direct attempts from 3,632,023 to 656,968 (81.9%) and
the sampled ordinary-match time from 9.881 to 6.732 seconds (1.47x).  Whole
user CPU improved only from 50.95 to 50.13 seconds, while combined user and
system CPU was effectively unchanged (70.79 versus 70.82 seconds).  Peak RSS
rose from 600,912 to 620,216 KiB.  This is a real matcher-level improvement,
but it misses both the 2x matcher target and the material whole-run target.

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
invalid SAME route precedes a later valid candidate.

Bounded experiments were run one memory-relevant process at a time with a
2-GiB address-space cap.  The machine had old pages in swap but no active
swap-in/swap-out during the checks.

## Next implementation work

The set-at-a-time foundation is now implemented.  The next work is gated
rather than open-ended threshold tuning:

1. **Measure program maturity.** Run eager `packed_compiled` at 1,000 givens
   on Josef 01/02 and a larger CHAT/Osborn boundary.  Record how often two or
   more conditions actually coexist; a correct multi-program that is never
   reused is not a speedup.
2. **Instrument interval cost.** Report root construction, direct tests,
   dense builds, program widths, and exact-match CPU per interval so a long
   run cannot hide a rising per-query slope.
3. **Keep lazy experimental.** Do not promote it unless retained-bank
   coverage can be predicted cheaply; current evidence is positive on
   Osborn and negative on Josef 01.
4. **Freeze before long gates.** After the 1,000-given gates pass,
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
