# Maximum demodulation for collective DISCOUNT

Date: 2026-08-09 (Europe/Berlin)

Implementation branch: `new-demod` (to be cut from the plan commit on
`better-collective-scheduler`)

Implementation status: Phases 0–4 are implemented on `new-demod`.  Phase 5's
stronger permanent `no_inferences` classifications remain deliberately off
pending a calculus-level redundancy audit.  Phase 6 has completed the bounded
100/250-given tests; full Osborn/AIM acceptance belongs on the user's larger
host.  See `P9-NEW-DEMOD-REPORT.md` for usage, measurements, and the revised
RAM forecast.

## Objective

Recover the demodulation strength needed by AIM/Osborn equational searches
without abandoning the bounded collective frontier or restoring the old
pointer-rich passive population.

The final mode must make every eligible retained positive unit equality
available for rewriting immediately.  It must keep rewrite activation
independent of inference activation: a passive equation may simplify future
and existing state without generating paramodulation or hyperresolution work
until the given-clause selector activates it.  Rewrite rules proved redundant
for generating inferences may remain useful permanent simplifiers with a
`no_inferences` classification.

The existing OTTER and current active-only DISCOUNT behavior remain available
as controls.  The new policy is opt-in until it passes proof, fairness,
checkpoint, coverage, CPU, and memory gates.

## Evidence and diagnosis

The old Osborn proof completed after 11,242 givens and reported 9,469,368 new
demodulators, 6,518,075 back-demodulations, and 3,759,850 live demodulators.
Its rewrite system was a dominant part of the successful search trajectory.

The eager clauses-frontier DISCOUNT control reached 65,944,358 passive clauses
at its last complete report.  Of those, 59,284,270 (about 90%) were eligible
delayed demodulators, while only 7,764 demodulators were active.  It matched
12,674 distinct hints and still had no proof.  The result shows both sides of
the problem:

1. delaying nearly all equations until given selection weakens contraction
   drastically; and
2. inserting all delayed equations as full `Topform` objects into the legacy
   demodulation indexes would recreate the RAM failure.

The balanced collective scheduler restores bounded, fair inference work and
hint discovery, but it intentionally does not restore OTTER's passive rewrite
relation.  A large distinct-hint count is not a substitute for the exact
rewrite-normalized proof chain.

The Waldmeister paper supplies the relevant design principles:

- keep active and passive facts strictly separated;
- completely interreduce the active rewrite system;
- normalize passives at generation and selection rather than rewriting the
  whole cold set synchronously;
- retain equations that are redundant for critical-pair generation when they
  are valuable simplifiers; and
- preserve access to successive normal-form functions when collective
  critical-pair representations require them for completeness.

## Architectural decision: two independent activation axes

Clause state is no longer represented by a single active/passive bit.

| State | Rewrite-active | Inference-active | Primary representation |
| --- | ---: | ---: | --- |
| Cold passive | no | no | dense selector record plus mmap body |
| Simplifier-only | yes | no | compact rewrite-bank entry plus stable proof ID |
| Selected active | yes when eligible | yes | active clause and collective descriptors |
| Disabled ancestor | no | no | mmap ancestor archive |

Every authoritative kept positive unit equality for which
`demodulator_type()` returns a valid direction is admitted to the rewrite
system immediately under the maximum policy.  It remains in passive given
selection unless a sound `no_inferences` criterion excludes it permanently.
Selecting an already rewrite-active rule changes only its inference-active
state; the rule is not inserted twice and the rewrite epoch does not advance.

This state split must not be implemented by deleting the current guard in
`cl_process_new_demod()`.  Dense passive archival destroys the materialized
`Topform`, whereas the legacy indexes retain pointers.  Rewrite-only rules
need separately owned storage and ID-based indexes.

## Semantic contract

### Soundness and proof ownership

- A rewrite-only equation has passed the unchanged authoritative
  `cl_process` path and has a stable clause ID and complete justification.
- Every rewrite step records the actual rule ID.  Proof reconstruction can
  materialize that rule from the passive store, rewrite bank, or ancestor
  archive even if it was never selected as a given.
- Rewriting never makes a clause an inference partner.  Only given activation
  creates collective rule descriptors and active clash/subsumption entries.
- Disabling, collapsing, or replacing a rewrite rule preserves its archived
  proof record.  Earlier uses of the old rule remain valid.

### Rewrite behavior

- All subsequently materialized candidates are normalized by the complete
  current rewrite bank.
- Oriented and safe lex-dependent demodulators retain existing Prover9 term
  ordering, step-limit, and loop-prevention behavior.
- Active clauses, rewrite rules, and hints are contracted promptly after rule
  admission.
- Cold passive repair is bounded and asynchronous, but every selected passive
  is normalized against the current system before activation.
- A fair background repair path eventually revisits every stale passive, so
  a clause that rewrites into a hint matcher cannot remain invisible forever.

### Deferred inference and epochs

Separate at least these versions:

1. activation/history epoch, used to define the parent set of a collective
   descriptor;
2. rewrite epoch, changed only when the effective rewrite system changes; and
3. hint epoch, changed when exact matching state changes.

The implementation must explicitly settle whether a deferred conclusion is
normalized by the current rewrite system or by the system at descriptor
creation.  The initial policy uses the latest sound rewrite system, matching
current collective behavior.  Before a completeness claim, audit that choice
against ordered completion.  If historical normalization is required, compact
rules carry born/dead epochs and the matcher supports an epoch-filtered query.

## Target rewrite bank

The production bank owns no pointer into a passive archive.  It uses:

- stable dense rule IDs and stable proof-clause IDs;
- variable-normalized left-hand sides;
- packed/path-compressed discrimination-trie segments;
- sorted, delta-encoded 32-bit postings;
- a small mutable delta plus immutable merged segments and tombstones;
- compact rule sidecars containing direction/type, born/dead epochs, proof ID,
  state flags, and usage counters; and
- shared/hash-consed immutable RHS/subterm nodes where the measured sharing
  census justifies them.

Query order must be deterministic.  A differential oracle must establish the
same normal form and ordered demodulator-ID justification sequence as the
legacy rewrite implementation on a fixed rule/query stream.

## Interreduction and rule admission

For every proposed rewrite-only equation:

1. Normalize it using the current rewrite bank.
2. Delete it if it becomes trivial or is rejected by existing authoritative
   checks.
3. Orient it with the existing reduction ordering.
4. Compose its right-hand side to normal form.
5. Detect identical and shadowed left-hand sides and retain a deterministic
   best proof representative.
6. Identify existing rules whose left side becomes reducible by the new rule.
7. Retire those rules at a new rewrite epoch and return their reduced
   equations to the ordinary passive pipeline with complete collapse/back
   rewrite justifications.
8. Update compact segments/delta state without leaving stale live pointers.

This implements the contracting half of the Waldmeister loop.  It prevents
"maximum demodulation" from meaning "retain every mutually redundant equation
forever".

## Contraction surfaces

### Future candidates

Forward normalization always uses all current rewrite-active rules.  Candidate
pool previews carry a rewrite epoch and are refreshed lazily before priority
commit if that epoch is stale.  The authoritative `cl_process` result remains
the final decision.

### Active clauses and collective history

Back-demodulate the small active state immediately.  A rewritten active fact
is deactivated at the current activation epoch and its replacement follows the
ordinary pipeline.  Existing descriptors retain access to historical parents
that were active at their snapshot, and their conclusions are normalized by
the selected epoch policy.

### Rewrite bank

Composition and collapse keep the effective rewrite bank interreduced.  Old
versions remain proof ancestors and, if needed, historical normal-form
entries, but are not live query answers.

### Hints

Keep `back_demod_hints` enabled.  Rewriting hints updates the packed hint bank,
stable matcher IDs, redundancy state, and hint epoch.  Stale collective
previews are refreshed before commit.

### Cold passives

Do not synchronously back-demodulate the entire mmap store per new rule.  Add
a bounded rewrite-refresh scheduler with:

- a hot cursor for Hha/Hw/LH and top selector candidates;
- a candidate cursor filtered by compact rewriteability features/postings;
- an oldest-stale/general cursor for fairness;
- a bounded materialization cache;
- record generations to coalesce duplicate work; and
- high/low watermarks for rewrite debt.

A first implementation may batch newly admitted rules and perform sequential
mmap sweeps.  It must measure bytes read and selectivity before adding a
resident passive occurrence index.  If postings are needed, they contain
record IDs and compact occurrence ordinals, never clause pointers.

A rewritten passive is authoritatively reprocessed with a copy/back-rewrite
justification, exact hint matching, selector-key replacement, and ordinary
deletion/subsumption.  This permits an initially ordinary clause to enter Hha
after rewriting.

## Scheduler integration

Rewrite work becomes a bounded debt lane beside collective inference debt.
When a new rule is admitted, the scheduler performs in order:

1. install and interreduce the rule;
2. contract active clauses and hints;
3. invalidate/refresh candidate-pool priority state;
4. spend a bounded quota on hot passive repair; and
5. resume descriptor and given scheduling.

Bursts of rules enter drain mode before the rewrite-work high-water mark can
be exceeded and leave it only below a lower watermark.  Mandatory ordinary
passive repair prevents an endless stream of hinted rules from starving stale
nonhint passives.  Rewrite debt and inference debt are reported independently.

## Options and controls

Names are provisional until the option audit, but the intended controls are:

```text
assign(discount_demodulation,selected).          % current behavior
assign(discount_demodulation,eager_legacy).      % bounded oracle only
assign(discount_demodulation,eager_interreduced).% production target

assign(rewrite_refresh_high_water,4096).
assign(rewrite_refresh_low_water,3072).
assign(rewrite_refresh_hot_ratio,7).
assign(rewrite_refresh_raw_budget,64).
assign(rewrite_refresh_inference_ratio,8).
```

The compatibility defaults remain unchanged.  `eager_legacy` is explicitly
incompatible with claims of bounded rewrite memory and is used only to test
the semantic hypothesis.

## Instrumentation

Periodic reports must include:

- rewrite-only rules admitted/current/peak/retired;
- oriented and lex-dependent rule counts;
- trivial, duplicate, composed, collapsed, and shadowed admissions;
- rewrite-bank term, posting, sidecar, delta, and segment bytes;
- forward demodulation attempts/rewrites by generated/pool/selected source;
- active, rule-bank, hint, hot-passive, and general-passive back rewrites;
- rewrite epoch changes and stale candidate/passive counts;
- rewrite-refresh queue current/peak, coalescing, materializations, bytes read,
  hot/general turns, and lag;
- rules selected later for inference and permanent `no_inferences` rules;
- proof-landmark state transitions; and
- anonymous versus file-backed RSS and passive backing-file blocks.

The old Osborn proof is converted into a landmark corpus of normalized clause
fingerprints.  Instrumentation records whether each landmark was seen raw,
normalized, kept, matched, selected, expanded by rule, and used in a proof.
This is a stronger signal than total distinct hints.

## Delivery phases and gates

### Phase 0: freeze controls and add attribution

- Preserve binary/input hashes and exact options for OTTER, eager clauses
  DISCOUNT, current balanced collective, and the running full Osborn job.
- Add the rewrite and landmark counters without changing search behavior.
- Build focused equations exercising orientation, lex-dependent rewriting,
  rule replacement, hint rewriting, and passive selection refresh.

Gate: instrumentation-on/off traces and proof results are identical; current
balanced bounded RSS and scheduler bounds do not regress.

### Phase 1: eager legacy semantic oracle

- Add the opt-in eager policy with separately owned full rewrite-only clones.
- Admit every eligible kept passive equation immediately.
- Contract future candidates, active state, and hints; keep cold passives at
  selection-time refresh initially.
- Implement rewrite-only to inference-active transition and proof retention.

Gate: proof/checkpoint tests pass; no rewrite-only clause enters an inference
or active subsumption index; bounded 100/250/500-given Osborn runs establish
whether eager demodulation restores old proof landmarks.  This phase is not a
memory result.

### Phase 2: compact exact rewrite bank

- Add the ID-based bank and packed/delta query representation.
- Run legacy and compact rewriting side by side in differential tests.
- Implement rule interreduction, composition, collapse, proof reconstruction,
  and deterministic query order.

Gate: identical normal forms and rewrite justification IDs on the oracle
corpus; at least 75% lower rewrite-index/term bytes; no passive pointers; all
proofs validate.

### Phase 3: cold-passive rewrite repair

- Add rewrite epochs to dense records and selector metadata.
- Implement hot and general bounded refresh with duplicate coalescing.
- Begin with batched sequential scans, then add compact filters/postings only
  if measurement shows a useful selectivity/memory tradeoff.
- Reinsert rewritten passives through exact hint/selector processing.

Gate: a synthetic ordinary passive that becomes a hint matcher is promoted
without direct selection; every finite stale set is eventually refreshed;
queue/cache bounds hold; checkpoint/resume is deterministic.

### Phase 4: scheduler and historical-normal-form integration

- Add rewrite-debt high/low-water drain behavior.
- Separate activation, rewrite, and hint epochs.
- Version the collective checkpoint with rule-bank and refresh state.
- Complete the latest-versus-historical normalization audit and implement
  epoch-filtered rule queries if required.

Gate: no inference lane, hot refresh, or general refresh starves; bounded
iterator traces remain equal to eager raw inference; mid-rule/mid-refresh
checkpoint resumes to the same terminal state.

### Phase 5: safe simplifier-only redundancy

- Mark exact duplicates, normalized equivalents, and proved shadowed rules as
  permanent `no_inferences` simplifiers.
- Add stronger theory-aware redundancy only behind separate options and
  acceptance results.

Gate: the generating calculus loses only inferences covered by an audited
redundancy theorem; solved coverage does not regress under matched resources.

### Phase 6: acceptance

Run bounded prefixes first, followed on a suitable host by paired one-hour,
1,000/4,000-given, full Osborn, and representative week-scale AIM jobs.

Require:

- every emitted proof validates with `prooftrans` and `directproof`;
- old Osborn proof landmarks occur substantially earlier than under current
  balanced mode, and full Osborn proves or has an explained remaining gap;
- rewrite-bank differential results remain exact;
- no unbounded rewrite cache, refresh queue, epoch history, or hidden passive
  index exists;
- external peak anonymous RAM is at least 80% below old P9, with file-backed
  mmap RSS and disk blocks reported separately;
- rewrite-bank bytes are at least 75% below the eager legacy oracle; and
- CPU cost is attributed to useful rewrites, interreduction, hint repair,
  passive repair, and inference rather than aggregate elapsed time alone.

## Initial implementation sequence

1. Add policy/state options and behavior-neutral counters.
2. Extract the old Osborn proof-landmark corpus and tracing support.
3. Implement the separately owned eager legacy oracle.
4. Validate bounded semantic impact before investing in packed storage.
5. Introduce the compact exact rewrite bank and interreduction.
6. Add bounded cold-passive refresh and scheduler backpressure.
7. Version checkpoints and run the full acceptance matrix.

The first go/no-go result is the eager oracle.  If maximum passive
demodulation does not recover old proof landmarks on bounded Osborn prefixes,
the project stops before the compact-bank migration and the evidence is used
to redirect work toward inference-chain scheduling instead.
