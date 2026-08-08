# Stronger collective scheduler completion audit

Date: 2026-08-08 (Europe/Berlin)

Branch: `better-collective-scheduler`

Scope: implementation and bounded-host phases 0--6 of
`P9-COLLECTIVE-SCHEDULER-PLAN.md`.  Phase 7 is the explicitly deferred
suitable-host acceptance campaign; it is not safe or useful to simulate a
week-scale 1,000/4,000-given comparison on this host.

## Invariant audit

| Contract | Implementation evidence | Verification evidence | Result |
| --- | --- | --- | --- |
| Every committed candidate follows the normal Prover9 path | Pool commit calls unchanged `cl_process`; preview cannot delete | Balanced equality/hyper proofs and `prooftrans`; legacy suites | Pass |
| Preview is advisory and read-only | Owned normalized scratch, dedicated packed workspace, restored counters, no CAC discovery | `hint_preview_test` across FPA, packed legacy, and packed stable-ID modes | Pass |
| Preview and authoritative hint keys agree at stable epochs | Same exact matcher/weight policy; tautologies excluded because authoritative deletion precedes matching | Balanced integration plus final bounded Osborn/AIM runs: zero false positives and changed IDs | Pass |
| Paramodulation turns have bounded native continuation | Stable literal/side/argument/subterm-path state | Eager sequence equals budgets 1--64 in both directions; zero replay in integrations | Pass |
| Hyperresolution turns have bounded native continuation | Stable outer/nucleus/mate/nested-choice coordinates | Positive/negative nucleus/satellite eager sequences equal budgets 1--64; zero replay | Pass |
| Rule and descriptor fairness | Split rule lanes, weighted service, mandatory oldest turn | Both rule families advance/complete in synthetic and all bounded Osborn/AIM runs | Pass |
| Descriptor memory has hard admission bound | Reserved high-water admission plus low-water/lag drain hysteresis | High-water 8 synthetic peak 8; tuned Osborn high-water 256 peak 255 | Pass |
| Candidate bodies remain globally bounded | Shared pool/cache, target window, forced commit interval, oldest pool commit | Synthetic peak at configured 4; tuned Osborn pool peak 126 below global 4,096 cap | Pass |
| Lookahead is bounded and cannot lose ordinary work | Separate monotone cursor, raw/distance/cap gates, low-rate general turns | Hot/general/forced-fair counters; bounded distances; ordinary rule descriptors complete | Pass |
| A promoted ordinal is committed once | Ordinal plus raw structural fingerprint; fair callback verifies/removes/deletes duplicate | Promotions equal confirmations in drained test; duplicate skips/catch-ups exact; zero residual records | Pass |
| Promotion metadata remains bounded | Per-descriptor cap and distance; dynamic capacity included in descriptor bytes | Checkpoint validation plus live record/byte reports; 250-given run used about 25 KiB | Pass |
| Checkpoint/resume is deterministic and fail-closed | `P9COLLF`, `P9CPOOL3`, policy/version checks; prior `P9COLLE/P9CPOOL2` compatibility | Live promoted body + consumed ordinal checkpoint resumes to identical terminal aggregate state | Pass |
| Legacy policy is unchanged by opt-in work | `collective_scheduler=legacy` remains default; old paths/formats retained | Collective frontier, DISCOUNT, hint-index, hint checkpoint, and standard proof suites | Pass |

## Phase gate audit

1. Phase 0 froze external artifact hashes and added interval/raw/rule/hint/RAM
   attribution without changing legacy traces.
2. Phase 1 added split rule descriptors, deterministic lane fairness, and
   hard high/low-water admission.
3. Phases 2--3 replaced paramodulation and hyperresolution prefix replay with
   native checkpointable iterators.
4. Phase 4 added one bounded cross-descriptor candidate window and exact
   side-effect-free hint/selector preview.
5. Phase 5 added bounded hot/general dual-cursor discovery, authoritative
   promotion, fingerprinted fair skipping, and live-state checkpointing.
6. Phase 6 corrected the production tautology-preview ordering, tuned the
   balanced descriptor profile to 256/192/256, and passed bounded 10%-hint,
   full-hint, and two independent AIM-prefix checks.

## Final bounded measurements

| Run | Limit outcome | Generated / kept | Hint result | Scheduler result | Peak RSS |
| --- | --- | ---: | ---: | --- | ---: |
| Osborn 10%, 250 | max given | 60,703 / 16,449 | 266 current matches; 3,314 confirmed promotions | 525 complete, peak 255, zero replay | 46,436 KiB |
| Osborn 10%, 500 request | max seconds at 30 | 60,704 at 20-second report | +252 then +7 interval matches; `Hha` active | given 255, peak 255, zero replay | 46,468 KiB |
| Osborn full, packed, 100 | max given | 825 / 550 | 141 current; 67/67 confirmed | 82 complete, peak 254, zero replay | 296,192 KiB |
| AIM Osborn+Kcom, packed, 100 | max given | 14,273 / 271 | 181 current; 228/228 confirmed | 108 complete, peak 254 | 9,984 KiB |
| AIM generalized-Bol, packed, 100 | max given | 1,972 / 107 | 102 current; 84/84 confirmed | 49 complete, peak 251 | 15,496 KiB |

The full-hint peak is the fixed packed-hint preprocessing high-water, not a
growing scheduler frontier.  Terminal allocator live/reserved memory in that
run was 52.1/97.5 MB.  These results support the architecture and bounded
profile but do not replace the phase-7 old/new long-run RSS and solved-coverage
comparison.

## Verification commands

```sh
make all -j2
make test1
make -C test.src iterator-tests hint-postings-test
test.src/collective_balanced_test.sh
test.src/collective_frontier_test.sh
test.src/discount_loop_test.sh
test.src/hint_index_trace_test.sh
test.src/hint_checkpoint_test.sh
```

All commands passed.  The LaTeX report source was updated, but a PDF was not
rendered because this environment has no `pdflatex`, `latexmk`, or `tectonic`.

## Remaining acceptance work

On a suitable host, run phase 7 exactly as written: paired one-hour full
Osborn jobs, checkpoint/resume the stronger policy, then the user-controlled
1,000/4,000-given and week-scale AIM comparisons.  Require sound proof
validation, bounded state, credible hint/solved trajectory, and at least 80%
lower old-to-new external peak RSS before promoting the policy beyond opt-in.
