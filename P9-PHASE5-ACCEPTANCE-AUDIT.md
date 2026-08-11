# Phase 5 completion audit

Date: 2026-08-11 (Europe/Berlin)

## Audited product boundary

This audit checks the requirements and acceptance gates in
`P9-PHASE5-COMPACT-FRONTIER-PLAN.md` against the current code, not merely an
earlier successful binary.

- code commit: `2483a7a49e4e21c54b1692c142b55435eeefe44b`;
- current `bin/prover9` SHA-256:
  `78b5ef4b2e53fc5ae2ab81cc46e6455213128e8ac083d6fb11259ad7b1339a6b`;
- old-P9 comparison binary SHA-256:
  `bcdf6bafbf608fde463fd43ef541891813f5c49a2d5153711c54925e98d76bcc`;
- `/project/bob/chat_test.in` SHA-256:
  `9781ee07691bc62e01f67534208620ca0be3f026f55248a227e9161f1ed17e6c`;
- current full-proof artifacts:
  `/project/phase5-results/phase5-completion-audit/final-proof-current`.

The accepted controls are the documented eager compact-OTTER configuration,
`ancestor_store=file`, zero passive cache, 10% compact-index stale threshold,
2-MiB shared-term reclaim threshold, and process-start
`P9_COMPACT_HEAP=1`.

## Requirement-by-requirement result

| Requirement | Authoritative evidence | Result |
|---|---|---|
| Preserve eager OTTER processing while making cold passive bodies file-backed | Full run reports `search_loop=otter`, `passive_store=dense`, `backing=ancestor-file`, and all four compact indexes authoritative | Pass |
| Compact demodulator bank agrees with ordinary processing | `compact_otter_audit_test.sh`; zero validation failures in current proof | Pass |
| Unit, back-demodulation, and nonunit indexes are exact | Focused component tests, compact differential audit, 300-event oracle, and zero authoritative failures | Pass |
| Stable-ID archived passives preserve selection and eager simplification | Current 300 trace and current 1,000-given full-body/file-backed A/B state are exact | Pass |
| Checkpoint/resume preserves continuation and terminal state | Current file-backed 300-given checkpoint audit described below | Pass |
| Final proof is valid and reaches the accepted boundary | Current `prooftrans parents_only` result, proof hash, and terminal statistics | Pass |
| 1,000-given CPU is no more than 1.25 times the full-body reference | 81.35 / 76.90 = 1.058 | Pass |
| 1,000-given peak RSS is at most 125 MiB | 90,404 KiB | Pass |
| Full proof user CPU is at most 957 seconds | 754.78 seconds | Pass |
| Full proof wall time is at most 18 minutes | 14:19.48 | Pass |
| Full proof peak RSS is at most 125 MiB (128,000 KiB) | 127,420 KiB; 580-KiB margin | Pass |
| Explain at least 95% of resident compact-frontier memory | 111,970,683 accounted bytes / 115,516,416 PSS bytes = 96.93% | Pass |
| Keep logical file size separate from RAM | 84,404,096-byte ancestor file reported separately from PSS/RSS | Pass |

The optional 115-MiB peak stretch target is not met.  The 300-given run also
uses 19.71 user seconds rather than the optional 12.79-second DISCOUNT-based
stretch target, but remains faster than the 28--35-second FPA-OTTER
compatibility boundary.  Neither optional stretch condition is treated as a
hard acceptance gate.

## Exact prefix evidence

The final 16,384-entry cache build emits 132,567 events at 300 givens.  Its
event stream is byte-identical to the accepted full-body `packed_fast`
reference:

```text
SHA-256 bc38f369ef02271d9b8a00db0cd01a6ba8f890e9608e0abd947de68f763322aa
Given=301 Generated=120793 Kept=5737
Usable=285 Sos=4298 Demods=3282 Disabled=1183
```

A fresh current-binary 1,000-given A/B comparison ran both cases concurrently
on separate physical CPUs:

| Representation | User | Wall | Peak RSS | Terminal state |
|---|---:|---:|---:|---|
| Compact indexes, full bodies | 76.90 s | 1:32.58 | 90,404 KiB | reference |
| File archive, zero cache | 81.35 s | 1:37.27 | 90,404 KiB | byte-identical counters |

Both end at `Given=1001`, `Generated=1268285`, `Kept=33909`, `Usable=949`,
`Sos=26052`, `Demods=21741`, `Disabled=6937`, and the same hint state.  All
four compact index statistics and exact-test totals agree.

## Checkpoint completion evidence

The audit initially found that checkpoint format 3 intentionally omitted
dead pre-elimination disabled clauses with ID 0 but also lost their reported
count.  Commit `2483a7a` retains only the omitted count as optional metadata;
it does not serialize the dead clause bodies.

The current full-hint file-backed audit checkpoints at Given 100 and resumes
to Given 301.  The concatenated pre-checkpoint/resumed event stream and the
uninterrupted stream are byte-identical with the same `bc38f369...322aa`
hash.  All terminal populations agree, including `Disabled=1183`, and the
resume reports `Verification: 22 passed, 0 failed`.  The focused checkpoint
regression independently exercises boundaries 0 and 2 and compares the final
proof, continuation trace, and complete two-line terminal search state.

## Current full-proof evidence

The current run terminates normally with:

```text
Given=2945 Generated=8248032 Kept=272787 proofs=1
Usable=1800 Sos=131001 Demods=109987 Limbo=300 Disabled=139715
Hints=88494 Active_Hints=4122
```

`prooftrans parents_only` emits 7,051 proof clauses with 3,231 new hints.  The
normalized 7,059-line proof is byte-identical to the accepted compact
full-body reference:

```text
SHA-256 9d7c9a12894c1c11ede6aeae08d1cec658ccee66a47fb9663859347a5413fd07
```

The archived FPA run has the same Given boundary, SOS/demodulator populations,
proof length, and successful endpoint, but generated and kept two additional
`other` clauses.  Its later raw IDs and some independent proof-line ordering
therefore differ.  Exact representation-change claims are made against the
full-body `packed_fast` OTTER control, which isolates storage/index changes
from that pre-existing hint-index difference.

External `/usr/bin/time -v` reports 754.78 user seconds, 104.03 system
seconds, 14:19.48 wall, exit status 0, and 127,420-KiB maximum RSS.  Against
old P9's 550,400 KiB and 765.69 user seconds, this is a 76.85% whole-process
RAM reduction (4.32 times smaller) and a 1.42% user-CPU improvement.

## Resident-memory accounting

At the frozen terminal report, the four compact indexes, shared term pool,
dense selector, packed hint nodes/references/tables and bodies, ancestor and
clause-ID handles, P9 allocator reservation, and nonanonymous process pages
sum to 111,970,683 bytes (106.78 MiB).  Terminal PSS is 112,809 KiB
(110.17 MiB), giving 96.93% coverage.  The sampled transient peak is 123,084
KiB RSS; the more authoritative external high-water mark is 127,420 KiB.

The ancestor file has 84,404,096 logical bytes and a 4-KiB I/O buffer.  Its
logical length is disk backing and is not included as process-resident RAM;
reclaimable kernel page cache is outside this PSS accounting.

## Regression evidence

The current tree passes:

- `make all -j4`;
- `make test1`;
- `make compact-frontier-tests`;
- `make discount-tests`;
- memory lifecycle, allocator churn, bookkeeping, ancestor-store, iterator,
  hint postings/preview, compressed-unit matching, rewrite-only store, and all
  compact term/ID/rewrite/unit/back/nonunit focused tests.

This evidence satisfies every hard Phase-5 gate.  The remaining 115-MiB and
literal 80% whole-process targets are explicitly stretch goals for this
large-fixed-hint exact-replay problem, not unfinished correctness or product
requirements.
