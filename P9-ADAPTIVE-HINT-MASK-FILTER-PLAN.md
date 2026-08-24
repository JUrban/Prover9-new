# Adaptive hint-mask filtering plan

Date: 2026-08-24 (Europe/Berlin)

Branch: `adaptive-hint-mask-filter`

Status: implementation and local 1,000-given validation complete; ar-2
promotion measurement pending

## Objective

Recover more of the CPU cost of `assign(hint_index,packed_fast)` without
changing hint matching, clause selection, demodulation, or the given-clause
trajectory.  The new work must remain small compared with the packed hint
bank, must adapt to broad and narrow queries, and must not assume the Josef 04
term distribution is universal.

The validated parent branch already makes the existing fallback matcher 7.58%
faster at 1,000 givens and 8.64% faster at 3,000 givens on ar-2.  It does so by
removing two key sorts, starting posting intersections with the least common
indexed property, and rejecting obviously unsuitable hint IDs before copying
them to the final candidate vector.  Both exact given-clause traces are
unchanged.

This branch addresses the work that still occurs before that rejection.

## Vocabulary

- **Active hint**: a non-redundant hint that can still match a generated
  clause.  Josef 04 reads 1,813,589 hints, rejects 967,643 as redundant, and
  retains 845,946 active hint matchers.
- **Term checklist**: the existing 64-bit summary of symbols and their
  positions in a literal.  A missing required bit proves that a hint cannot
  match, but passing the checklist does not prove a match.
- **Property bitmap**: a compact yes/no table with one bit per active-hint ID.
  A table answers, for example, which active positive hints contain checklist
  property 17.  Positive and negative literals have separate tables.
- **Rough candidate word**: one group of 64 hint IDs that survives the current
  shallow structural lookup.  It may contain zero, one, or many actual IDs.

The implementation and statistics use `mask` and `bitmap` in C because those
are standard representation names.  User documentation should prefer the
terms above and define the representation when it matters.

## Corrected population and RAM model

The matching index covers 845,946 active hints, not all 1,813,589 input hints.
Redundant hints are retained in compressed bookkeeping form but do not consume
ordinary active matcher IDs.  The current active-ID arrays round their capacity
to 1,048,576 entries.

Two sets of 64 property bitmaps at that capacity require 16 MiB.  Supporting
counters add negligible space.  This is bounded basic-property storage, not
the old conjunction sidecar that stored large numbers of property subsets.

The feature bitmaps already exist for the legacy packed matcher.  This branch
will reuse that representation for `packed_fast`; it will not introduce a
second pointer-rich index.

## Current generated-clause work

`packed_fast` already computes the 64-bit term checklist before retrieving
hints.  It then computes approximately six exact shallow properties and
intersects their hint postings.  At the Josef 04 1,000-given endpoint:

- ordinary match queries: 2,409,522;
- flipped-equality queries: 343,721;
- dense fallback queries: about 2.60 million;
- rough IDs surviving the shallow intersections: about 2.87 billion; and
- IDs rejected by the existing checklist afterward: about 2.75 billion.

Therefore no additional term traversal is required to use the checklist
during lookup.

The audit also found redundant current work.  An ordinary match computes the
first literal's complete checklist twice and computes complete checklists for
the remaining literals even though it needs only their positive/negative
counts.  Initial equivalence lookup computes a separate first-literal mask it
does not use.  Phase A removes those traversals before adding new storage.

## Why the bulk filter must be adaptive

It is not automatically profitable to combine every required property bitmap.
The existing shallow lookup already reads many bitmap words.  If a rough word
contains only one candidate, loading ten more property words is worse than one
ordinary per-hint checklist comparison.

The candidate chooses locally between two safe paths:

```text
few rough candidates in this 64-ID group
    -> keep the current individual checklist comparisons

many rough candidates in this 64-ID group
    -> combine a bounded number of the least-common required properties
       -> individually validate the survivors as before
```

The bulk step only removes IDs that the existing checklist would remove.  The
complete checklist comparison and the authoritative subsumption matcher remain
in place, so the new tables are an optimization rather than a new definition
of matching.

## Implementation phases

### Phase A: remove duplicate term traversals

1. Split ordinary/flipped matching from initial equivalence-profile creation.
2. For ordinary matching, compute the first literal checklist once and count
   all literals without computing unused aggregate masks.
3. For equivalence lookup, compute the positive/negative aggregate masks once
   and do not compute an unused first-literal mask.
4. Report the number of checklist builds performed and avoided, without adding
   per-term-node instrumentation.

Gate: focused hint tests, generalization smoke tests, and exact generated/given
traces remain unchanged.

### Phase B: build basic property tables for active hints

1. Allocate the existing 128-row packed bitmap representation for
   `packed_fast`: 64 properties for positive literals and 64 for negative
   literals.
2. Insert a hint ID into every property table present in its stored checklist.
3. On hint rewriting, add every property of the new body before publishing the
   re-indexed hint.  Old bits may remain as conservative false positives, just
   like existing stale postings; a missing new bit is forbidden.
4. Deactivated/redundant IDs remain harmless because the existing active check
   remains authoritative.
5. Include the exact bitmap bytes in packed-index memory statistics.

Gate: targeted add/rewrite/deactivate tests prove that no active candidate can
be lost and reported memory agrees with the allocated capacity.

### Phase C: bounded adaptive filtering

For each ordinary dense fallback query:

1. Rank the required checklist properties by their stored population counts.
   Counts may conservatively include stale rewritten/deactivated properties;
   that can only affect ranking, not matching correctness.
2. Retain only a small configured maximum of the least-common properties.
3. After the current shallow lookup produces one 64-ID rough word, count its
   surviving IDs.
4. If the word is sparse, use the established per-ID check.
5. If the word is broad, combine the selected property bitmaps and then
   enumerate only surviving IDs.
6. Apply the complete existing literal-count/checklist validation to every
   survivor and preserve decreasing hint-ID order.

Initial defaults will bound the extra work to at most four property tables and
require at least eight rough IDs in a word before bulk filtering.  These are
cost bounds, not Josef-specific term constants.  Both values will be exposed
for diagnostic A/B runs; setting the maximum property count to zero disables
the bitmap allocation and bulk path.

Telemetry will report:

- property-table bytes and active-ID capacity;
- words eligible for bulk filtering and words actually filtered;
- property-table reads;
- rough IDs entering and surviving the bulk step;
- queries for which no useful property was available; and
- remaining individual checklist checks and rejects.

Gate: the bulk path must remove substantially more ID visits than the property
words it reads.  If it does not, it must remain experimental/off rather than be
promoted based only on one elapsed-time result.

### Phase D: correctness and performance validation

Required local checks:

1. `make -C test.src hint-postings-test`.
2. `./test.src/compact_generalization_smoke_test.sh`.
3. Existing compressed-hint, AnyConst, flipped-equality, back-demodulation,
   checkpoint, and hint-preview tests reached by those runners.
4. Sequential Josef 04 runs capped at no more than 1,000 givens, after checking
   available RAM and live prover processes.
5. Byte-identical printed given-clause trace and identical generated, kept,
   matched-hint, and proof counters.

The local machine must run only one Josef process at a time.  No run may exceed
the input's existing memory/time limits merely to obtain a result.

Required ar-2 checks left to the user:

1. The completed parent-branch 3,000-given pair establishes an 8.64% whole-run
   and 8.84% incremental 1,000-to-3,000 saving for the already validated
   early-rejection change.
2. Run the pinned parent and new candidate at 3,000 givens without input echo;
   a same-binary `hint_mask_filter_bits=0` run may be added to isolate the bulk
   filter from the checklist-walk cleanup.
3. Compare both whole-run CPU and the incremental 1,000-to-3,000 CPU segment.
4. Longer proof-confirmation runs are allowed only after exact 3,000-given
   trajectory equality and a favorable incremental result.

## Acceptance criteria

The branch is acceptable only if all of the following hold:

1. Every focused and generalization test passes.
2. The Josef 04 prefix has an identical given trace and logical endpoint.
3. Property storage is bounded by active-ID capacity and reported explicitly.
4. The adaptive path records no candidate-order or exact-answer divergence.
5. Explicit allocation accounting shows exactly the documented 16 MiB and no
   hidden conjunction-sized allocation; OS peak RSS may hide that increment
   inside the existing startup high-water plateau.
6. CPU does not regress on narrow-query generalization cases.
7. The ar-2 3,000-given incremental CPU result improves sufficiently to justify
   the added resident memory.

The local 1,000-given measurement is a development gate, not evidence for
day/week/month behavior.  The user-run ar-2 prefixes and subsequent long runs
remain the promotion authority.

## Implementation outcome

Phases A through D are complete locally.  The focused and generalization test
suites pass, including broad-bank activation, disabled allocation,
deactivation, and rewritten-hint reindexing.  Sequential enabled/disabled
Josef 04 runs have an identical 1,000-given trace and endpoint.  Enabling the
filter improves matched local user CPU by 10.56% while reporting exactly 16
MiB of property tables and no swap activity.

The filter used 167,593,174 property-table reads to reject 1,391,378,351 rough
IDs before individual inspection, a ratio of about 8.30 avoided inspections
per table read.  Detailed commands, counters, hashes, and measurements are in
`P9-ADAPTIVE-HINT-MASK-FILTER-RESULTS.md`.  The remaining project gate is the
user-run 3,000-given ar-2 comparison; longer proof runs remain intentionally
deferred until that result is known.

## Follow-on work intentionally out of scope

- Reusing a preprocessed/memory-mapped hint bank to remove repeated startup.
- Replacing the matcher with a discrimination/code tree.
- Reintroducing the all-subset packed-conjunction sidecar.
- Changing hint redundancy, matching, degradation, or selection semantics.
- Tuning demodulation, inference generation, or passive-clause selection.

Those are separable projects.  This branch is successful only if it makes the
existing packed fallback lookup cheaper without changing the proof search.
