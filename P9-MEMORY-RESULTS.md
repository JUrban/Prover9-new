# Prover9 AIM memory results (Phases 0--4)

Date: 2026-08-07 (Europe/Berlin)
Base revision: `b36df4c06d5794dd98e42335f191dd41d7abd2ab`
Phase 4 starting revision: `9172cd401095a10aebf5b90f835715baeca0aaca`
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

## Phase 3 compact-bookkeeping results

### Structure baseline and million-record scale run

Before replacement, `sizeof(struct topform)` was 112,
`sizeof(struct clist_pos)` was 40, and `sizeof(struct plist)` was 16 on this
64-bit host. The fixed ID bucket array occupied another 400,000 bytes. A
bounded synthetic run retained one million allocated `Topform` records in both
the ID table and disabled list:

| Representation | Disabled metadata | ID metadata | Combined logical allocation | Bytes/record | Peak RSS | Wall |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Legacy fixed hash + Clist | 40,000,032 B | 16,400,000 B | 56,400,032 B | 56.400 | 166,400 KB | 1.11 s |
| Phase 3 vector + paged IDs | 8,388,632 B | 8,512,672 B | 16,901,304 B | 16.901 | 127,488 KB | 0.42 s |

The Phase 3 command was:

```sh
CAP_SECONDS=30 timeout 60s benchmarks/run-memory-benchmarks.sh bookkeeping
```

It completed all one million insertions, sampled exact pointer lookup, tested
an isolated ID above `2^50`, verified stable next-ID assignment and ordered
formula collection, and tore the table down to zero live pages/bytes. The
logical bookkeeping reduction is 39,498,728 bytes (70.0%) for this workload;
process peak RSS is 38,912 KB lower (23.4%). Both RSS values include the
unchanged one million 112-byte `Topform` records and complete process overhead.
Logical bytes exclude malloc headers. The wall observations are not treated as
a general throughput claim because the small drivers are not a full prover
workload.

### Integrated capped workloads

The Phase 3 release build produced these bookkeeping counters. “Legacy” is the
measured allocation the replaced structures would require for the same live
counts, not an RSS estimate.

| Workload/cap | Store current / legacy | ID current / legacy | Exact work | Peak RSS / wall |
| --- | ---: | ---: | --- | --- |
| x2 proof | 160 / 592 B | 656 / 400,400 B | 12 / 118 / 23 / 14; proof | 3,328 KB / 0.01 s |
| disabled-heavy, `max_given=1` | 16,416 / 80,072 B | 8,704 / 416,016 B | 1 / 1,001 / 1,001 / 2,001 | 5,248 KB / 0.05 s off; 5,120 KB / 0.06 s on |
| LCC, `max_given=500` | 1,056 / 3,072 B | 5,408 / 409,280 B | 501 / 258,858 / 562 / 76 | 6,016 KB / 1.91 s off; 1.90 s on |
| aK, `max_given=200` | 544 / 2,352 B | 3,296 / 405,776 B | 201 / 84,789 / 340 / 58 | 15,488 KB / 1.39 s off; 1.28 s on |

The slash-separated work fields are given/generated/kept/disabled. Option-off
and option-on work counters, FPA counters, and x2 proof were identical in each
same-build pair. Phase 3 removes 56,088 cumulative palloc bytes in the
disabled-heavy run by eliminating 2,001 Clist cells and 1,001 ID Plist cells;
the replacement vector/pages use tracked malloc instead. Peak RSS remains on
the same short-run plateau because the palloc allocator retains its 20 MB slab.

Commands, all externally capped at 30 seconds per prover and internally at 20
seconds or less:

```sh
CAP_SECONDS=30 timeout 120s benchmarks/run-memory-benchmarks.sh smoke
CAP_SECONDS=30 timeout 150s benchmarks/run-memory-benchmarks.sh aim
```

### Phase 3 correctness coverage

- Sequential release `make all` and project tests 1--6 passed.
- `make memory-tests` passes both the Phase 1/2 lifecycle test and the new
  200,000-record bookkeeping lifecycle test.
- The bookkeeping test and existing memory lifecycle test passed with the full
  LADR library instrumented by ASan+UBSan. Leak reporting was disabled because
  process-global LADR initialization is intentionally retained; invalid access,
  double free, and undefined behavior remained halt-on-error.
- x2 with `compress_disabled` off/on had identical search counters. Both
  outputs passed `prooftrans expand` and `directproof`; their normalized
  34-line proof sections were byte-identical.
- A fresh `aK1_nil3_a` format-3 checkpoint used `compress_disabled`,
  `checkpoint_exit`, `checkpoint_verify`, `max_given=200`, `max_seconds=20`,
  and SIGUSR2 only after the handler was registered. Save exited 107. Resume
  passed 18/18 hashes (including serialized disabled IDs/count), exited at the
  deterministic cap with status 5, and reported given/generated/kept =
  201/84,789/342. The +2 kept/SOS difference from a fresh run is the same
  pre-existing resume issue recorded above.

The checkpoint contained 57 in-memory disabled clauses, of which eight had
official IDs and were serialized. This is the existing format-3 behavior;
Phase 3 neither drops additional proof ancestors nor assigns IDs to previously
ID-zero pre-elimination clauses.  The Phase 4 results below supersede that
historical handoff statement.

## Phase 4 append-only ancestor-store results

### Synthetic scale and retained allocation

The bounded 100,000-record driver used an official-ID input clause with one
binary atom, one unary function, an attribute-free input justification, and
the same disabled-store/ID-table ownership as Prover9:

| Quantity | Phase 4 value |
| --- | ---: |
| Records / used record bytes | 100,000 / 11,100,000 B |
| Backing capacity | 16,777,216 B |
| Handle vector / paged ID table | 1,048,656 / 858,032 B |
| Resident handle+ID logical bytes | 1,906,688 B (19.067/record) |
| Used record+resident logical bytes | 13,006,688 B (130.067/record) |
| Estimated replaced live graph+bookkeeping | 26,400,000 B (264.000/record) |
| Peak RSS / wall | 14,464 KB / 0.63 s |

The used-record comparison reduces logical retained allocation by 13,393,312
bytes (50.7%).  The always-resident handle/ID portion is 92.8% below the
estimated replaced graph and bookkeeping.  `backing capacity` is reported
separately: geometric high-water is real allocated/address-space capacity, not
used record bytes, and RSS also includes the complete process and mapped-page
residency.

Reproduce with:

```sh
make -C test.src ancestor_store_scale_test
/usr/bin/time -v test.src/ancestor_store_scale_test 100000
```

### Integrated exact workloads

All modes below used the same release binary.  `compressed` means the Phase 2
`compress_disabled` option without archive records.  The slash-separated work
columns are given/generated/kept/disabled.

| Workload/cap | Mode | Exact work | Records / used bytes / backing | Peak RSS / wall |
| --- | --- | --- | ---: | ---: |
| x2 proof | off | 12/118/23/14; length 16 | 0 / 0 / 0 B | 3,328 KB / 0.01 s |
| x2 proof | compressed | identical proof/work | 0 / 0 / 0 B | 3,328 KB / 0.01 s |
| x2 proof | memory | identical proof/work | 9 / 1,276 / 4,096 B | 3,328 KB / 0.01 s |
| x2 proof | mmap | identical proof/work | 9 / 1,276 / 4,096 B | 3,456 KB / 0.01 s |
| disabled-heavy | off | 1/1,001/1,001/2,001 | 0 / 0 / 0 B | 5,248 KB / 0.05 s |
| disabled-heavy | compressed | identical work | 0 / 0 / 0 B | 5,248 KB / 0.04 s |
| disabled-heavy | memory | identical work | 1,000 / 119,000 / 131,072 B | 5,248 KB / 0.05 s |
| disabled-heavy | mmap | identical work | 1,000 / 119,000 / 131,072 B | 5,120 KB / 0.06 s |
| LCC, given 500 | off | 501/258,858/562/76 | 0 / 0 / 0 B | 6,016 KB / 1.93 s |
| LCC, given 500 | compressed | identical work | 0 / 0 / 0 B | 6,016 KB / 1.88 s |
| LCC, given 500 | memory | identical work | 54 / 8,367 / 16,384 B | 6,016 KB / 1.87 s |
| LCC, given 500 | mmap | identical work | 54 / 8,367 / 16,384 B | 6,016 KB / 1.84 s |
| aK, given 200 | off | 201/84,789/340/58 | 0 / 0 / 0 B | 15,488 KB / 1.36 s |
| aK, given 200 | compressed | identical work | 0 / 0 / 0 B | 15,360 KB / 1.49 s |
| aK, given 200 | memory | identical work | 9 / 1,152 / 4,096 B | 15,488 KB / 1.39 s |
| aK, given 200 | mmap | identical work | 9 / 1,152 / 4,096 B | 15,360 KB / 1.43 s |

The disabled-heavy workload has 1,001 official-ID clauses and 1,000 archived
records; the other 1,001 disabled objects are pre-ID elimination clauses kept
in the exact Phase 2 representation.  Its full bodies are 432,064 B, while the
compressed body payload is 34,006 B and the archive records are 119,000 used
bytes.  Thus Phase 4 pays for proof metadata that Phase 2 leaves in live
objects, but deletes the much larger `Topform`/literal/term/justification graph.

No archive-mode capped workload is more than 5.2% slower than `off` in these
single short observations, and the direction varies by workload.  This is
within short-run noise and below the 10% regression threshold; it is not a
speedup claim.  RSS is flat because each workload fits the same allocator/page
plateau and AIM hint/active storage dominates its few archived records.

Reproduce with per-prover internal and external caps:

```sh
CAP_SECONDS=30 timeout 120s benchmarks/run-memory-benchmarks.sh smoke
CAP_SECONDS=30 timeout 150s benchmarks/run-memory-benchmarks.sh aim
```

### Proof, corruption, sanitizer, and checkpoint evidence

- x2's archive proof is accepted by `prooftrans parents_only` and
  `directproof`; native proof content and all work counters match `off` and
  `compressed`.  Archive TSTP output reports Theorem and contains a complete
  `SZS output start CNFRefutation` / end block.
- `ancestor_store_test` passes in memory and mmap modes.  It explicitly rejects
  a changed version, a corrupt total bound, and payload CRC damage, then
  verifies the restored bytes remain usable and that validation counters
  increase.  It exercises proof-DAG on-demand materialization and all compact
  justification families used by the engine.
- The full LADR library and focused ancestor/memory tests pass ASan+UBSan with
  `detect_leaks=1`, `halt_on_error=1`, and no invalid access, undefined
  behavior, or lifecycle leak.
- A fresh mmap aK checkpoint at given 29 had eight archived ancestors (1,011
  used bytes, 4,096-byte backing) and materialized them into the unchanged
  format-3 checkpoint.  Resume passed 18/18 hashes and reconstructed sos=65,
  usable=37, demods=0, disabled=8.  It subsequently hit the known
  `avl_insert, item already there` checkpoint restore defect.  This defect is
  already documented for non-archive restores, so it limits the general
  deterministic-restart claim but does not indicate archive corruption or a
  mismatched checkpoint hash.

No uncapped AIM search was run, no capped non-proof result is treated as a
logical failure, and Phase 5 was not started.
