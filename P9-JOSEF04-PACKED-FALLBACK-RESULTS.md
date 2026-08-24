# Josef 04 packed-fallback matcher experiment

## Purpose

This branch tests CPU improvements for the ordinary `packed_fast` fallback
matcher when `hint_conjunction_kb` is zero.  It does not enable or allocate the
large packed-conjunction sidecar.  The intended use is therefore the existing
low-RAM Josef configuration, unchanged apart from the matcher implementation.

The implementation consists of three separately reviewable commits:

1. Preserve deterministic feature-key collection order for the result-cache
   key, instead of sorting the same key array twice per uncached query.
2. Seed dense posting intersections with the rarest posting that was already
   found during query planning, rather than always beginning at key zero.
3. Reject candidates with impossible literal counts or feature masks before
   inserting their IDs into the materialized result vector.  The existing
   final authoritative checks remain in place.

Back-demodulation ordering and matching semantics are unchanged.  The early
filter is only supplied by the ordinary clause-matching path; back-demodulation
continues to use its established order and final validation.

## Correctness tests

The following tests pass on the candidate:

```text
make -C test.src hint-postings-test
./test.src/compact_generalization_smoke_test.sh
```

`hint_preview_test` also includes new literal-count and deep-feature near misses
to exercise the early rejection path.

## Paired Josef 04 result

Both binaries were built with `make prover9-lto` and run sequentially on the
same machine with the exact input echoed by
`bob/Josef_04.matcher-control.out`.  The single generated warning line inside
that echo was removed because it is output, not source input.  Both runs used
the input's `max_given=1000`, `max_seconds=900`, and `max_megs=4000` limits.

| Measurement | Control `be72aa6` | Candidate `74363b1` | Change |
|---|---:|---:|---:|
| User CPU | 547.32 s | 515.42 s | -31.90 s (-5.83%) |
| System CPU | 12.17 s | 10.53 s | -1.64 s (-13.48%) |
| Wall time | 561.54 s | 526.18 s | -35.36 s (-6.30%) |
| Maximum RSS | 2,041,088 KiB | 2,041,216 KiB | +128 KiB (+0.006%) |
| Given counter | 1001 | 1001 | identical |
| Generated | 2,435,141 | 2,435,141 | identical |
| Kept | 5,815 | 5,815 | identical |
| Matched hints | 5,700 | 5,700 | identical |

The 1,000 printed `given #...` lines are byte-identical.  Their SHA-256 in both
runs is:

```text
34d5383f3eb1594c3601a439587527b13ae2caf0a1576a8904c598c65d3d9426
```

Exit status 5 is expected here: it is Prover9's normal `max_given` exit, not a
crash.

The first uploaded ar-2 control took 262.53 user seconds, but it was not used as
the denominator above because it ran on a different CPU from that candidate.
The local pair established the initial 5.83% result.

## Paired ar-2 result without input echo

The control and candidate were subsequently rebuilt as pinned binaries and run
sequentially on ar-2.  Both inputs began with `clear(echo_input)` and
`clear(print_initial_clauses)`.  This suppresses output only; all search options
and the 1,000-given endpoint remained unchanged.

| Measurement | Control `be72aa6` | Candidate `74363b1` | Change |
|---|---:|---:|---:|
| User CPU | 260.35 s | 240.62 s | -19.73 s (-7.58%) |
| System CPU | 1.81 s | 1.91 s | +0.10 s (+5.52%) |
| Total CPU | 262.16 s | 242.53 s | -19.63 s (-7.49%) |
| Wall time | 262.18 s | 242.56 s | -19.62 s (-7.48%) |
| Maximum RSS | 2,041,088 KiB | 2,040,192 KiB | -896 KiB (-0.044%) |
| Given counter | 1001 | 1001 | identical |
| Generated | 2,435,141 | 2,435,141 | identical |
| Kept | 5,815 | 5,815 | identical |
| Matched hints | 5,700 | 5,700 | identical |

The 1,000 printed given clauses are byte-identical and have the same SHA-256
shown above.  Both runs ended normally at `max_given`, with no swapping.

Suppressing the echo reduced the control output from 130,849,417 bytes to
127,434 bytes, a factor of 1,026.8.  However, it reduced control user CPU by
only 2.18 seconds (0.83%) and wall time by 3.78 seconds (1.42%).  Therefore the
large delay before the given loop is not primarily output dumping: it is hint
parsing, compression, indexing, and initial equivalence processing.

## What the new counters show

At the common endpoint, the candidate performed 2,832,866,041 early profile
checks.  Literal counts rejected 589,635 IDs and feature masks rejected
2,713,711,947 IDs before result-vector insertion.  Despite that very large
rejection count, whole-run user CPU improved by 5.83% locally and 7.58% on
ar-2.  Most of the remaining matcher cost is paid
before that point while reading and combining posting bitplanes, and the
authoritative matching work and generated search are unchanged.

This is a safe, measurable improvement, but the 1,000-given data alone do not
establish a radical speedup.  The fixed hint initialization cost is paid once,
whereas the optimized fallback queries accumulate with generated clauses.  A
paired 3,000-given ar-2 run is therefore the next scaling measurement.  If its
relative gain does not grow, the next optimization should avoid bitplane
reads/intersections themselves (or change their representation), rather than
only making result materialization cheaper.

## How to run the candidate on ar-2

Do not change `Josef_04.matcher-test.in`; it is the fixed control input.  Once
the `faster-packed-fallback` branch is available in the ar-2 checkout, build the
candidate and run the same file:

```sh
git switch faster-packed-fallback
make prover9-lto
cd bob
/usr/bin/time -v -o Josef_04.matcher-candidate.time \
  ../bin/prover9 \
  < Josef_04.matcher-test.in \
  > Josef_04.matcher-candidate.out \
  2> Josef_04.matcher-candidate.err
```

Run only this candidate now; the ar-2 control has already completed.  A normal
bounded result ends with `exit (max_given)` and `/usr/bin/time` status 5.

The two local reference binaries were retained as untracked build artifacts:

```text
bin/prover9-packed-fallback-control
bin/prover9-packed-fallback-candidate
```

Their SHA-256 values are, respectively:

```text
8401eed8a2060ae4ccebf7e75fe45e913c5ad226a5f3aaae2622b353ecddac4b
6929e1d2b594573362a21f306c57bf21713c7ef91faca8bd4cece38fbc92716e
```
