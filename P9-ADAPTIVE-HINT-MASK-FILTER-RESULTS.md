# Adaptive packed-hint checklist filter

Date: 2026-08-24 (Europe/Berlin)

Branch: `adaptive-hint-mask-filter`

Status: implemented and locally validated through 1,000 given clauses; the
3,000-given and longer promotion runs remain for ar-2.

## What changed

`packed_fast` already computes a 64-bit checklist for the first literal of
each generated clause.  Each set bit describes a symbol-and-position property
that any matching hint must also have.  Previously, the broad fallback lookup
first produced many rough hint IDs and then fetched the stored checklist for
each ID separately.

The new path adds 128 compact property tables: 64 for positive/equational
literals and 64 for negative/inequality literals.  Every table has one bit per
active-hint ID.  For a rough group containing at least eight IDs, the matcher
consults at most four of the least-common required property tables before it
enumerates IDs.  Narrow groups retain the old individual path.

The tables are only a rejection shortcut.  Every surviving ID still passes
the complete existing checklist comparison, active-hint check, and
authoritative subsumption matcher.  Candidate order, hint semantics, clause
selection, demodulation, and inference generation are unchanged.

The implementation also removes unused checklist construction:

- ordinary and flipped queries build the first-literal checklist once rather
  than twice;
- their remaining literals are counted without constructing unused complete
  checklists; and
- initial equivalence queries no longer build an unused separate first-literal
  checklist.

At the Josef 04 1,000-given endpoint, this removes 4,605,039 complete term
checklist walks.

## Options

The recommended candidate settings are:

```text
assign(hint_mask_filter_bits,4).
assign(hint_mask_filter_min_candidates,8).
```

These are also the defaults.  `hint_mask_filter_bits` bounds the number of
property tables consulted for one broad 64-ID group.  Setting it to zero
disables both the new tables and the bulk filter, which provides an exact A/B
control in the same binary.  `hint_mask_filter_min_candidates` is the minimum
number of rough IDs in a group before the bulk path is considered.

For Josef 04 these settings supplement, rather than replace, the established
configuration.  In particular, retain:

```text
assign(hint_index,packed_fast).
assign(hint_conjunction_kb,0).
```

The second line keeps the much larger packed-conjunction index disabled.  The
new 16 MiB basic-property tables are not that conjunction index.

## Correct population and RAM cost

Josef 04 supplies 1,813,589 input hints, but 967,643 are redundant.  Only the
845,946 active matchers receive IDs in this lookup.  Their arrays round to a
capacity of 1,048,576 IDs.

At that capacity, 128 one-bit-per-ID tables occupy exactly 16,777,216 bytes
(16 MiB).  The reported packed-index storage increases from 185,370,200 to
202,147,416 bytes, exactly the expected 16 MiB.  There is no pointer per hint
per property and no table for every property conjunction.

The Linux maximum-RSS high-water value stayed near 2,041 MiB in all local
runs; the 16 MiB addition was hidden inside the process's existing startup
high-water plateau.  The explicit `bitmap_bytes` counter is therefore the
reliable incremental allocation measurement.

## Correctness validation

The final focused and generalization suites pass:

```text
make -C test.src hint-postings-test
./test.src/compact_generalization_smoke_test.sh
```

The focused test covers a broad hint bank, exact match preservation, filter
activation, zero-allocation disablement, deactivation with conservative stale
bits, and reindexing a rewritten hint under its stable ID.

The filter-enabled, filter-disabled, and validated parent Josef prefixes have
identical given-clause traces and logical endpoints.  The 1,000 printed given
clauses have SHA-256:

```text
34d5383f3eb1594c3601a439587527b13ae2caf0a1576a8904c598c65d3d9426
```

Every 1,000-given run ends with:

```text
Given=1001. Generated=2435141. Kept=5815. proofs=0.
active hints=845946, matched=5700
```

Exit status 5 is the normal `max_given` result, not a crash.

## Matched local CPU measurements

The decisive comparison uses the same new LTO binary, the same input, and
sequential runs on this machine.  Only `hint_mask_filter_bits` changes.

| 1,000-given measurement | Disabled (`0`) | Enabled (`4`, minimum `8`) | Change |
|---|---:|---:|---:|
| User CPU | 612.93 s | 548.18 s | -64.75 s (-10.56%) |
| System CPU | 8.94 s | 7.61 s | -1.33 s (-14.88%) |
| Wall time | 622.14 s | 556.43 s | -65.71 s (-10.56%) |
| Maximum RSS | 2,041,216 KiB | 2,041,088 KiB | -128 KiB (noise-level) |
| Swaps | 0 | 0 | identical |

An earlier matched 300-given pair gave the same direction:

| 300-given measurement | Disabled (`0`) | Enabled (`4`, minimum `8`) | Change |
|---|---:|---:|---:|
| User CPU | 96.36 s | 86.81 s | -9.55 s (-9.91%) |
| Wall time | 106.69 s | 95.16 s | -11.53 s (-10.81%) |
| Maximum RSS | 2,041,344 KiB | 2,041,216 KiB | -128 KiB (noise-level) |

Absolute local CPU times varied between sessions, so the report does not
compare the 548.18-second candidate with an older unpaired local run.  The
same-session enabled/disabled pair is the valid measurement.

## Work removed at 1,000 givens

The enabled run reports:

```text
eligible rough groups = 91,116,892
property-table reads  = 167,593,174
rough IDs entering    = 1,466,803,717
IDs surviving         =    75,425,366
IDs rejected early    = 1,391,378,351
```

The bulk step rejects 94.86% of the IDs presented to it.  It avoids about 8.30
individual stored-checklist inspections per additional compact-table read.
The disabled run performs 2,868,155,392 individual profile checks; the enabled
run performs 1,476,777,041, and the difference exactly equals the IDs rejected
by the new step.

## Parent 3,000-given result

The newly uploaded `faster-packed-fallback` parent results are separate from
this adaptive-filter measurement.  They show that the already validated
parent optimization improves user CPU from 1,679.85 to 1,534.66 seconds
(8.64%) at 3,000 givens.  The incremental 1,000-to-3,000 segment improves by
8.84%.  All 3,000 given clauses and final counters are identical.  Full
details are in `P9-JOSEF04-PACKED-FALLBACK-RESULTS.md`.

This establishes a sound parent baseline; it does not yet establish the new
adaptive filter's 3,000-given scaling.

## ar-2 promotion run

Build and preserve the candidate binary on the new branch:

```sh
git switch adaptive-hint-mask-filter
make prover9-lto
cp bin/prover9 bin/prover9-adaptive-hint-mask-filter
sha256sum bin/prover9-adaptive-hint-mask-filter
```

In a copy of the no-dump 3,000-given input, retain all existing Josef options
and add the two recommended lines:

```text
assign(hint_mask_filter_bits,4).
assign(hint_mask_filter_min_candidates,8).
```

Then run only one prover at a time:

```sh
/usr/bin/time -v -o Josef_04.matcher-adaptive-3k.time \
  ../bin/prover9-adaptive-hint-mask-filter \
  < Josef_04.matcher-adaptive-3k.in \
  > Josef_04.matcher-adaptive-3k.out \
  2> Josef_04.matcher-adaptive-3k.err
```

The primary comparison is against the completed parent candidate
`Josef_04.matcher-nodump-candidate-3k.*`.  If a same-binary attribution control
is desired, make a second input with only this change:

```text
assign(hint_mask_filter_bits,0).
```

That control disables allocation and filtering while preserving the duplicate
checklist-walk cleanup.  Do not place either new option in an input consumed by
an older binary that does not recognize it.

Promotion requires an identical 3,000-clause trace and endpoint, no swapping,
and a favorable whole-run and incremental CPU result.  Day/week proof runs
should remain deferred until that ar-2 gate passes.

## Retained local binary

The development binary is retained as the untracked artifact:

```text
bin/prover9-adaptive-hint-mask-filter
SHA-256 3947a9ecebf801f233814f4a71cc3889954bb50af2086e6212d58c25d8b961ed
```

It contains the production code at commit `0a179c0`; later commits on the
branch only strengthen tests and documentation.
