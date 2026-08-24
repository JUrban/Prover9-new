# Compiled Hint Matcher Implementation Report

Date: 2026-08-24  
Branch: `compiled-hint-matcher`  
Production base: `faster-packed-fallback` (`1b03bcf`)

## Current decision

Phase 0 passes its stop/go gate.  The proposed compiled unit matcher has a
large exact-work opportunity on three structurally different training
problems, and retained unit terms exhibit enough cross-hint sharing to justify
a compact shared term table.

Production behavior has not changed.  The new measurements require the
explicit diagnostic option:

```text
set(hint_compiled_census).
```

With that option clear—the default—`packed_fast` continues to use its original
compressed matcher hot loop.

## What Phase 0 measures

The diagnostic compressed matcher returns exactly the same match answer as
the production matcher and additionally reports:

- fixed-symbol mismatch;
- repeated generated-variable mismatch;
- whole stored subterms consumed by generated variables;
- compact nodes and bytes skipped;
- compact nodes compared for repeated bindings; and
- sampled CPU for the final exact-candidate loop.

It deliberately continues after a repeated-variable mismatch, allowing a
later independent fixed-symbol mismatch to be counted too.  This reveals the
potential of testing rare fixed symbols before binding broad variables.

The retained-bank census runs after redundancy decisions and reports unit,
nonunit, `_AnyConst`, polarity, equation, term-length, and variable
populations.  A temporary 64-bit fingerprint set estimates distinct subterms
across retained unit hints.  The set is freed before search, and its peak
bytes are reported.

## Bounded measurements

All runs used one process, a 2-GiB virtual-address cap, bounded given/time
limits, and the same current binary.  These are opportunity measurements, not
candidate/control speed comparisons: enabling the census intentionally does
extra work.

For a supported unit candidate, define `reject union` as:

```text
rigid_rejects + repeated_rejects - combined
```

In every run below:

```text
reject union + direct matches = profiled unit candidates
```

Thus the two conditions completely explain direct matching on these inputs.

| Problem | Boundary | Profiled units | Reject union | Rejected | Direct matches | Exact-loop CPU | Whole packed match CPU |
|---|---:|---:|---:|---:|---:|---:|---:|
| CHAT | 100 givens | 77,453 | 74,943 | 96.76% | 2,510 | 0.094 s | 0.133 s |
| Osborn | 100 givens | 279,144 | 274,476 | 98.33% | 4,668 | 2.900 s | 3.411 s |
| Josef 01 | 300 givens | 1,047,519 | 1,043,019 | 99.57% | 4,500 | 0.998 s | 1.509 s |
| Josef 02 | 300 givens | 326,812 | 293,962 | 89.95% | 32,850 | 0.430 s | 1.290 s |

The whole-search opportunity differs by problem:

- CHAT search used about 9.05 user seconds after startup; its entire ordinary
  match operation was only about 0.13 seconds.  A perfect new matcher cannot
  materially accelerate this prefix.
- Osborn search used about 5.76 user seconds after startup; the exact candidate
  loop alone was about 2.90 seconds.  This is a major whole-search target.
- Josef 01 search used about 4.07 user seconds after startup; its exact loop
  was about 1.00 second.  This is a meaningful target.
- Josef 02 search used about 12.39 user seconds after startup; its exact loop
  was about 0.43 seconds.  Candidate selection and the rest of the prover
  dominate this prefix, although unit filtering remains highly selective.

The implementation must therefore avoid hurting CHAT/Josef 02 while
accelerating Osborn/Josef 01.  A global always-on structure is not acceptable
merely because candidate rejection percentages look impressive.

## Retained-bank shape

| Problem | Retained hints | Units | Unit term nodes | 32-bit token bytes | Distinct subterm fingerprints | Sharing ratio | Temporary fingerprint peak |
|---|---:|---:|---:|---:|---:|---:|---:|
| CHAT | 62,805 | 62,765 | 1,626,386 | 6,505,544 | 171,082 | 9.506× | 2 MiB |
| Osborn | 219,171 | 206,292 | 4,954,480 | 19,817,920 | 594,455 | 8.334× | 8 MiB |
| Josef 01 | 96,678 | 96,225 | 2,530,398 | 10,121,592 | 283,176 | 8.936× | 4 MiB |
| Josef 02 | 136,486 | 136,435 | 2,902,091 | 11,608,364 | 351,049 | 8.267× | 4 MiB |

At a conceptual 16 bytes per canonical subterm, the measured distinct
populations would occupy approximately 2.6, 9.1, 4.3, and 5.4 MiB before hash
table slack and per-hint roots.  This is not yet a measured implementation
size, but it supports building Phase 1 under the 128-MiB gate.

Repeated generated variables dominate rejection on CHAT, Osborn, and Josef
01.  Josef 02's flipped-equality path is the complementary case: fixed-symbol
tests reject 20,790 of its 24,623 profiled candidates.  The implementation
therefore needs both subterm identity and rarity-ordered fixed-symbol tests;
optimizing only one would overfit.

## Phase 1 implementation target

The next implementation unit is an immutable, hint-specific unit-term table:

1. Intern normalized retained subterms and give each a stable 32-bit handle.
2. Store one root handle and sign per ordinary unit hint.
3. Keep compact child handles and symbol/arity in flat arrays.
4. Maintain stable hint IDs and a small mutable delta for rewritten hints.
5. Run in construction/shadow mode first; `packed_fast` remains authoritative.
6. Measure real allocated bytes, build CPU, and lookup work before adding
   multiple path postings.

The first lookup will add one measured rare fixed-path posting per root group,
then validate its complete decreasing-ID stream against `packed_fast`.  The
full compiled `BIND/SAME` query plan follows only after that shadow gate.

## Correctness evidence

The following pass on the current branch:

```text
test.src/compressed_unit_match_test
test.src/hint_compiled_census_test.sh
test.src/hint_preview_test
test.src/hint_index_trace_test.sh
```

The integration test exercises retained unit/nonunit classification,
positive and negative units, equations and disequations, stored repeated
variables, a generated repeated-variable rejection, sampled/cumulative
statistics, and interval output.
