# Prover9 AIM memory results (Phases 0--2)

Date: 2026-08-07 (Europe/Berlin)
Base revision: `b36df4c06d5794dd98e42335f191dd41d7abd2ab`
Release flags: `-O2 -Wall`; debug flags: `-g -O0` (prover sources also
define `DEBUG`)
Host compiler: GCC 13.3.0

Input SHA-256:

- `prover9.examples/x2.in`: `05398944db1a997c7958bfafcdd1c70437f9b2471f6e92aa6c112d67be6b7748`
- `LCC_to_aK1.in`: `18f3fca4309802693b0b3e77468cbbe3b7e96296c251f4e67cb8755e126a303c`
- `aK1_nil3_a.in`: `cdfb80bb64e6acf44b224b93d4df9083817de8ddcdbbcf6eeffeba8fd52141e0`

Every prover invocation below had an external `timeout` of at most 30 seconds
and an internal `max_seconds` of at most 20 seconds. AIM runs also had a fixed
`max_given`. No uncapped AIM run was made.

## Unmodified baseline

The baseline was recorded before editing with sequential `make all`, followed
by `timeout 30s make test1`. The release build took 17.68 s (75,580 KB build
peak RSS); `test1` produced the 16-step proof in about 0.05 s with 3,328 KB
peak RSS. The supported `make all DEBUG=1` build and debug `test1` also passed;
debug palloc immediately reserves a 20 MB slab.

| Input/cap | Outcome and deterministic work | User + system / wall | Peak RSS | Internal memory |
| --- | --- | --- | --- | --- |
| `x2.in`, 30 s | proof; given 12, generated 118, kept 23, proof length 16 | below timer resolution / about 0.01 s | 3,328 KB | later instrumentation shows 3,136 logical disabled-body bytes |
| `LCC_to_aK1.in`, `max_given=500`, 30 s | proof at given 494; generated 247,538, kept 556, disabled 80 | 1.90 + 0.08 / 1.99 s | 5,888 KB | reported palloc 2.08 MB |
| `aK1_nil3_a.in`, `max_given=200`, 30 s | cap at given 201; generated 84,789, kept 340, disabled 58 | 1.30 + 0.17 / 1.48 s | 15,360 KB | reported palloc 11.45 MB |

The LCC proof/no-proof difference after FPA pruning is discussed in the
worklog. It is an ordering effect, so the aK case and same-build option-off/on
comparisons are the cleaner fixed-work throughput evidence.

## Final combined build: option off versus on

Command:

```sh
CAP_SECONDS=30 timeout 120s benchmarks/run-memory-benchmarks.sh all
```

The driver runs `make memory-tests`, verifies the x2 proof through
`prooftrans parents_only`, generates 1,000 nested ground clauses for the
disabled-heavy case, and appends deterministic caps to the supplied AIM
inputs.

| Workload | Option | Given / generated / kept / disabled | Disabled full -> compact payload | palloc cumulative | Peak RSS | Wall |
| --- | --- | --- | --- | --- | --- | --- |
| x2 proof | off | 12 / 118 / 23 / 14 | 3,136 -> 0 B | 57,792 B | 3,328 KB | 0.01 s |
| x2 proof | on | identical | 0 -> 202 B (estimated full 3,136 B) | 55,808 B | 3,328 KB | 0.01 s |
| disabled-heavy | off | 1 / 1,001 / 1,001 / 2,001 | 432,064 -> 0 B | 1,836,248 B | 5,248 KB | 0.05 s |
| disabled-heavy | on | identical | 0 -> 34,006 B (estimated full 432,064 B) | 1,620,296 B | 5,120 KB | 0.05 s |
| LCC, cap 500 | off | 501 / 258,858 / 562 / 76 | 25,920 -> 0 B | 2,209,184 B | 5,760--6,016 KB | median 2.16 s |
| LCC, cap 500 | on | identical | 0 -> 2,033 B (estimated full 25,920 B) | 2,183,744 B | 6,016 KB | median 2.11 s |
| aK, cap 200 | off | 201 / 84,789 / 340 / 58 | 16,872 -> 0 B | 11,484,776 B | usually 15,488 KB | randomized median 1.64 s |
| aK, cap 200 | on | identical | 0 -> 1,391 B (estimated full 16,872 B) | 11,470,240 B | 15,360 KB | randomized median 1.58 s |

The disabled-heavy logical retained body falls from 432,064 B to 34,006 B,
a 92.1% payload reduction, and cumulative palloc falls by 215,952 B. Its
single short RSS observation falls only 128 KB because freed palloc cells stay
inside the already-reserved 20 MB slab. LCC and aK compact payload reductions
are 92.2% and 91.8%, respectively, but disabled bodies are a small share of
these short runs (aK has 2.52 MB of hints), so RSS is essentially flat.

LCC timings are medians of three runs: off 2.16 s, on 2.11 s. The aK table uses
seven additional alternating-order pairs to reduce warm-up noise: median wall
off 1.64 s, on 1.58 s; median user CPU off 1.59 s, on 1.51 s. These short
measurements show no serious throughput regression, but they are not a claim
of a general speedup.

FPA counters for the final capped runs were also identical option off/on:
LCC live/peak nodes 11,493 and lists 4,623; aK nodes 50,591 and lists 20,106.
The disabled-heavy churn peaked at 5,025 nodes and 5,001 lists and ended at 16
nodes / 2 lists belonging to the remaining active index contents.

## Correctness and lifecycle validation

Commands (all individually capped where they execute a prover):

```sh
make all
for n in 1 2 3 4 5 6; do timeout 30s make "test$n"; done
make all DEBUG=1
timeout 30s make test1
timeout 30s make memory-tests

make -C ladr clean
timeout 30s make -C ladr lib DEBUG=1 \
  XFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'
make -C test.src clean
timeout 30s make -C test.src memory_lifecycle_test \
  XFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
timeout 30s test.src/memory_lifecycle_test
```

Results:

- Release tests 1--6 passed (native proof, Mace4, utilities, two TPTP statuses,
  SInE proof, and Mace4 TPTP statuses).
- Supported debug build, debug x2 proof, and focused memory test passed.
- Focused ASan+UBSan test passed with leak detection and halt-on-error enabled.
- The focused test performs repeated versioned round trips over variables,
  constants, nested functions, equality, negative literals, attributes,
  nested/atom flags, and a symbol number above 127. It tests double-compress,
  scoped recompression, malformed version/truncation rejection, and printing a
  compact clause. A 300-argument clause, a 300-deep clause, and a 1,100-literal
  clause exercise growable encoding, sizing, decoding, and clause-wrapper
  arrays beyond their legacy fixed boundaries.
- FPA coverage includes 144 depth/hash/order combinations plus 96-child hash
  clustering and reinsertion. It finishes with zero live nodes/list headers;
  observed peaks were 481 trie nodes and 96 list headers.

For proof equivalence, x2 was run with the option off and on, and each output
was processed by both `prooftrans expand` and `directproof` under 30-second
caps. After removing PID/date/timing banner noise, both transformed proof
outputs were byte-identical. In-prover `set(print_expanded_proof)` also
completed and its output was accepted by `prooftrans parents_only`.

For checkpoint coverage, `aK1_nil3_a` was started with
`set(compress_disabled)`, `set(checkpoint_exit)`,
`set(checkpoint_verify)`, `max_given=200`, and `max_seconds=20`; SIGUSR2 was
sent only after the process advertised a SIGUSR2 handler. The checkpoint held
57 compact disabled clauses. Resume reported `Verification: 16 passed, 0
failed` and reached the deterministic cap without a compression error. It had
the same pre-existing final kept/SOS discrepancy as an option-off checkpoint;
the separate LCC AVL restore failure is recorded in the worklog.

## Interpretation

The logical-body reduction is the primary successful Phase 1 result. Short
peak RSS is a conservative indicator here: palloc never releases an early
20 MB slab, and the tested AIM prefixes retain far more hint/active data than
disabled bodies. On a long saturation run, reusable term/literal cells delay
or prevent allocation of later slabs; that high-water effect is what these
phases target. No result here is an uncapped proof search, and no short capped
failure is treated as a theorem-proving failure.
