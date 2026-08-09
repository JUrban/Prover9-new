# Maximum demodulation in collective DISCOUNT

Date: 2026-08-09 (Europe/Berlin)  
Branch: `new-demod`

## Outcome

The new production policy is:

```text
assign(discount_demodulation,eager_interreduced).
```

It separates rewrite activation from inference activation.  Every retained
positive unit equation accepted by Prover9's existing
`demodulator_type()` test becomes a rewrite rule immediately, even while its
clause remains cold and passive.  It does **not** become a paramodulation,
resolution, subsumption, or collective-history partner until ordinary given
selection activates it.

This restores the essential OTTER/Waldmeister behavior—new equations simplify
future clauses immediately—without putting millions of full passive
`Topform` graphs into the legacy demodulation index.

The implementation is proof producing, hint aware, checkpointable, bounded in
its repair scheduling, and exact against the legacy demodulator on the focused
differential corpus.  It is ready for the user's long external comparison,
but the bounded measurements do **not** yet prove an 80–90% whole-process RAM
saving for every workload.  The compact rewrite rules are now an explicit
linear RAM term and must be included in capacity planning.

## Recommended full-run configuration

Add these settings to the ordinary problem input.  Later occurrences of the
same option override earlier ones, so place this block after generic defaults
if possible.

```text
assign(search_loop,discount).
assign(passive_store,dense).
assign(discount_demodulation,eager_interreduced).
assign(hint_index,packed).
assign(inference_frontier,collective).
assign(collective_scheduler,balanced_hint).
set(collective_hint_discovery).
assign(ancestor_store,mmap).
assign(sos_limit,-1).

set(back_demod).
set(back_demod_hints).

assign(rewrite_refresh_high_water,4096).
assign(rewrite_refresh_low_water,3072).
assign(rewrite_refresh_drain_burst,64).
assign(rewrite_refresh_hot_ratio,7).
assign(rewrite_refresh_raw_budget,64).
assign(rewrite_refresh_inference_ratio,8).

clear(collective_hint_probes).
clear(collective_promising_candidates).
clear(collective_promising_scheduler).
clear(compress_disabled).

assign(stats,all).
assign(report,3600).
```

Keep the already-tuned collective descriptor settings if they are present:

```text
assign(collective_descriptor_high_water,256).
assign(collective_descriptor_low_water,192).
assign(collective_oldest_lag_limit,256).
```

For the most aggressive cold hint/selector discovery, change only:

```text
assign(rewrite_refresh_inference_ratio,1).
```

That spends one background repair turn per inference turn.  It does not make
forward demodulation stronger—generated and selected clauses always use the
complete current rule bank—but it revisits mmap passives sooner and can expose
new hint matches earlier.  The 250-given prefix showed that this setting can
be expensive.  Values 4 and 8 are the useful stronger/balanced starting
points.

The two controls are:

```text
assign(discount_demodulation,selected).       % old active-only DISCOUNT
assign(discount_demodulation,eager_legacy).   % semantic oracle, not scalable
```

`eager_legacy` owns full clause clones and uses the legacy discrimination
index.  Use it only for bounded comparisons.

## How it works

### Immediate forward contraction

When an eligible passive equation is kept, the compact bank stores encoded
left/right token streams, its legacy direction/type, a stable proof ID, and
exact discrimination-trie postings.  Future inferred clauses, collective
candidate previews, authoritative candidate commits, selected passives, and
hints use this bank immediately.

The cold passive body remains in the dense mmap arena.  A compressed proof
shell owns the stable rule ID and justification while the clause is cold.
Every rewrite justification records the actual rule ID, so `prooftrans` and
`directproof` can reconstruct rules that were used before being selected.

### Interreduction without the quadratic sweep

New rules can compose or collapse old rules.  The first implementation made
every old rule stale at every rewrite epoch; the bounded Osborn run exposed
this as quadratic.  The final implementation adds a pointer-free reverse
occurrence index over rewrite-source symbols and then performs an exact
token-level pattern match, including repeated-variable constraints.

Only old cold rules whose source side can actually contain a redex receive a
`rewrite_rule_dirty` bit.  Repeated discoveries coalesce into that bit and an
O(1) debt counter.  Exact repair suspends the old rule, normalizes it against
the other current rules, and then:

- restores it unchanged;
- retires it and admits its composed replacement through ordinary
  `cl_process`; or
- retires it after collapse/subsumption.

Old proof records remain available after retirement.  Tombstone-heavy compact
banks rebuild physically and report reclaimed bytes.

### Cold passive repair and scheduling

There are independent circular cursors for dirty rules, hinted passives, and
general passives.  Outside debt drain, the default performs one background
repair turn per eight inference turns.  Within repair work, the default
`rewrite_refresh_hot_ratio=7` gives short bursts to rule/hinted work and then
forces a general turn, so a finite ordinary stale set is eventually visited.

If exact dirty-rule debt reaches `rewrite_refresh_high_water`, the scheduler
raises dirty rules to urgent priority.  Urgent work is limited to
`rewrite_refresh_drain_burst` consecutive turns and then yields one ordinary
search turn even if debt has not fallen.  The nonempty high/low band prevents
chattering, while the burst bound prevents a self-replenishing composition
queue from starving givens and collective descriptors.  Selection itself
always normalizes a stale clause, independently of background lag.

A primary inferred demodulator starts one exact backward-composition wave.
A composed replacement is installed immediately, back-demodulates hints, and
advances the rewrite epoch, but it does not recursively start another urgent
rule-only wave.  Further rule normalization proceeds through the bounded fair
background/general lane.  This preserves current-bank forward rewriting and
hint behavior without requiring transitive composition closure before search
can continue.

A changed cold passive is copied with a rewrite justification and returned to
the authoritative clause pipeline.  Hint matching, selector weights,
subsumption, deletion rules, and possible demodulator admission are therefore
recomputed from the new body.

### Collective snapshots and current rewrite epochs

Collective descriptors retain the historical activation snapshot that defines
their parent set.  Deferred raw conclusions are normalized by the **latest**
proved rewrite system when previewed and committed.  Candidate previews carry
hint and simplifier epochs and are refreshed lazily before priority use.

Using a later proved equality to contract a consequence is sound, and every
step has a proof rule ID.  Historical parent bodies are retained, so old
normal forms do not have to be reconstructed.  No epoch-filtered historical
rewrite bank is therefore implemented.  A formal completeness theorem for
the combined ordered calculus is outside this implementation; the present
claim is operational Prover9 soundness plus the tested fairness properties.

## What to monitor

The important new report lines are:

```text
Discount_demodulation: ... admitted=..., retired=..., selected=..., bytes=...
Compact_rewrite: ... physical=..., compactions=..., occurrences=..., ...
Rewrite_refresh: epoch=..., inference_ratio=..., stale=..., lag_max=..., ...
Rewrite_interreduce: ... overlap_visits=..., dirty_marks=...,
  cascade_suppressed=..., debt=..., drain_burst=..., drain_yields=..., ...
```

Interpretation:

- `current` under `Discount_demodulation` is the number of cold proof shells;
- `Compact_rewrite current` includes cold and selected/active live rules;
- `dirty_marks` counts exact, coalesced rule candidates;
- `cascade_suppressed` counts demodulators admitted while repairing a rule;
  they enter the live bank and rewrite hints but do not recursively seed an
  urgent rule-only wave;
- `debt` is the current dirty cold-rule count, not all stale passives;
- `stale` is all passives not normalized at the latest rewrite epoch;
- `lag_max` can be large with ratio 8; selection remains exact;
- `drain_yields` proves the ordinary scheduler was allowed to run while debt
  remained urgent; a long run with a fixed `Given` count and increasing drain
  turns is a liveness failure;
- repeated nonzero `drain_entries` with little useful composition suggests
  lowering rule generation or retuning the watermarks, but can no longer
  block all inference;
- `physical` far above `current` should be temporary because compact rebuilds
  reclaim tombstones.

For mmap accounting, report anonymous and file-backed memory separately.  The
passive file is created with `mkstemp()` under `$TMPDIR` (normally `/tmp`) and
unlinked immediately.  While running, find it through `/proc/<pid>/fd` and
`/proc/<pid>/smaps`.  Its logical size, allocated disk blocks, and resident
file-backed pages are different quantities.

For a long run, put temporary storage on a filesystem with enough space:

```sh
TMPDIR=/local/p9-tmp /usr/bin/time -v /path/to/prover9 < osborn.in \
  > osborn-new-demod.out 2> osborn-new-demod.err
```

Do not rely on Prover9's `max_megs` alone as an external resident-memory cap;
also sample `/proc/<pid>/smaps_rollup` or use an appropriate cgroup limit.

## Osborn drain incident and correction

The six-hour full-hint run
`bob/rr_osbe.out2-unl-rad-coll-bal1-demod1.gz` exposed a failure that the
bounded prefixes did not reach.  Once exact rule debt reached the default
high-water mark, every repaired rule admitted a replacement that dirtied one
other rule.  Debt remained at 4,095 and the exclusive drain never exited.

From the two-hour report through the six-hour report:

- `Given` remained exactly 974;
- rewrite inference turns remained exactly 3,184,094;
- matched hints remained exactly 610;
- new demodulators rose from 200,829 to 683,729;
- ancestor-store payload rose from 2.36 GB to 14.52 GB;
- RSS rose from 2.65 GB to 14.53 GB.

The rising `Generated` and `Kept` counters were replacement clauses, not
search progress.  Packed hint back-demodulation amplified the cost: it kept
querying for every replacement after its successful rewrite count had stopped
changing.

Commits `ce12a97` and `2abf74c` correct the two independent causes:

1. urgent drain yields after a configurable bounded burst; and
2. a composed replacement cannot recursively seed another urgent rule wave.

The regression suite now includes a live-debt proof that requires a drain
yield and a two-stage composition case that would previously propagate the
replacement cascade.  A 500-given Osborn stress run used deliberately low
32/16 watermarks and an eight-turn burst.  At the 120-second report it had
advanced to 365 givens and 359,221 inference turns, all six drain entries had
exited, debt was 8, ancestor data was 0.60 MB, and RSS was 47.7 MB.  The
180-second capped process peaked at 54.3 MB RSS.  This is bounded-prefix
evidence, not a forecast that the full week-long search will remain at that
absolute size.

Do not use a binary ending at commit `e3e9e6a` for a full
`eager_interreduced` run.  Use `2abf74c` or later and leave the new burst at
its default initially:

```text
assign(rewrite_refresh_high_water,4096).
assign(rewrite_refresh_low_water,3072).
assign(rewrite_refresh_drain_burst,64).
assign(rewrite_refresh_inference_ratio,8).
```

## Bounded acceptance results

These are development-prefix measurements, not full Osborn proof results.
The 250-given input used a deterministic 1-in-10 subsample of the already
10%-filtered hints (3,102 hints), a 30-second limit, a 256 MB Prover9 limit,
and identical packed-hint/back-demodulation settings.

| Policy/version | Given | Generated | Kept | Rule candidates | Rule turns | Cold materializations | User CPU | Max RSS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Eager legacy oracle | 251 | 54,214 | 6,440 | 3,096 | n/a | 406 | 8.64 s | 11,552 KiB |
| Compact, pre-fix every-rule epochs | 251 | 55,823 | 7,118 | 3,688 | 129,282 | 139,820 | 13.69 s | 14,644 KiB |
| Compact, exact overlap + ratio 8 | 251 | 54,515 | 6,875 | 3,495 | 11,864 | 17,550 | 9.14 s | 14,156 KiB |

The final row was the acceptance run used to choose the default ratio.  It
reduced background materializations by 87.4% and rule turns by 90.8% relative
to the naive compact scheduler.  CPU overhead versus the eager legacy oracle
fell from about 58% to about 6%.  The searches are not clause-for-clause
identical because interreduction and repair timing intentionally change the
search trajectory.

On the 100-given input with all 31,014 already-10%-filtered hints, eager legacy
used 3.87 user seconds and 36,180 KiB RSS; the compact mode before the scheduler
optimization used 3.82 seconds and 36,184 KiB.  At this scale the packed hint
index dominates and fixed compact-bank pools hide any rewrite-memory saving.

All focused eager/compact proof tests, proof translation, direct proof,
composition, collapse, hint rewriting, ordinary-passive-to-hint promotion,
unchanged-rule restoration, checkpoint verification/resume, dense cursor
compaction, DISCOUNT, dense-passive, collective-frontier, balanced scheduler,
and hint checkpoint/trace regressions pass.

## RAM estimate and limitations

The earlier radical passive-store estimate must be amended for maximum
demodulation.  The asymptotic model is now:

```text
old P9 RAM ~= hints + all resident SOS/disabled bodies + full demod indexes
new RAM    ~= packed hints + active/history/cache
             + dense metadata + O(all live compact rewrite rules)
```

The 250-given compact report allocated roughly 1.5 MB for compact-bank pools
and about 0.5 MB for cold proof shells at approximately 2,500 live rules.  Pool
rounding makes small-prefix division noisy, but **0.7–0.8 KiB per live compact
rule** is the prudent current planning range.  It includes the exact rewrite
trie, term tokens, reverse occurrences, rule/hash sidecars, and compressed
cold proof shells; it does not include fixed hints or the file-backed passive
arena.

Applying that range only as a scenario:

- 577,347 live rules (the archived 4,000-given Osborn scale) imply roughly
  395–451 MiB of compact rewrite state;
- 3,759,850 live rules (the old Osborn proof boundary) imply roughly
  2.5–2.9 GiB; and
- 6,199,295 live rules (the reported AAPERM boundary) imply roughly
  4.1–4.7 GiB.

Interreduction can reduce the live count—the bounded prefix retired about a
quarter of admitted rules—but that fraction must not be extrapolated as a
guarantee.  Compared with old reports of 2.63 GiB at Osborn 4,000, 18.81 GiB
at the Osborn proof, and 125.83 GiB at the AAPERM boundary, a radical saving is
still plausible on large passive-dominated jobs.  A defensible forecast is
currently **about 70–90%**, workload dependent, rather than a blanket 90%
promise.  The bounded prefix itself showed no RSS saving: compact RSS was
about 22% above the oracle because fixed pools and different retained state
dominated at only 2,500 rules.

The remaining route to a materially lower rewrite coefficient is immutable
term/subterm hash-consing (especially repeated RHS structure), denser immutable
postings/segments, and smaller proof-shell ownership.  Those optimizations need
a full-scale sharing census; they were not guessed into this branch because
the current exactness and proof ownership provide a clean measurement base.

The full old/new RAM claim is therefore an acceptance item for the user's
large host, not a completed benchmark claim.  Record old/new binary hashes,
input hashes, exact options, `/usr/bin/time -v`, anonymous RSS, file-backed
RSS, mmap logical bytes, mmap allocated blocks, rule counts, and the four new
diagnostic lines at matched given/time boundaries.

## Implementation history

The branch is deliberately split into inspectable commits:

- `1997515` — candidate and cold-passive attribution;
- `c29ea53` — separately owned rewrite-only proof store;
- `509506f` — eager legacy semantic oracle;
- `7eec72b` — exact compact rewrite discrimination bank;
- `4f6440a` — compact eager integration, hints, proofs, and checkpoints;
- `21a30a5` — fair rewrite-epoch cold repair;
- `f10294f` — composition, collapse, and compact physical reclamation;
- `93043bb` — exact debt, hysteretic drain, and stable checkpoint cursors;
- `3a7bd9b` — exact reverse occurrences and throttled general repair.

The design rationale and acceptance gates remain in
`P9-COLLECTIVE-DEMODULATION-PLAN.md`.
