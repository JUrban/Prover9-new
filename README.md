# Prover9 and Mace4 -- LADR-2026

**Website:** [prover9.org](https://prover9.org)
**Manual:** [prover9.org/manual-2026](https://prover9.org/manual-2026/)
**Build Instructions:** [Building from Source](https://prover9.org/manual-2026/install.html)

Jeffrey Machado and Larry Lesyna present a backwards compatible, comprehensive modernization of William McCune's Prover9 and Mace4 within the LADR framework.  Prover9 is an automated theorem prover for first-order and equational logic, and Mace4 searches for finite models and counterexamples.

This work preserves the original paramodulation calculus and intended inference rules of Prover9. The focus has been on stability, scalability, infrastructure alignment with contemporary ATP standards, and rigorous empirical validation.

---

## Engineering Modernization

The modernization effort addresses long-standing implementation constraints to better serve modern hardware:

- **Iterative Conversion:** Systematic conversion of deep recursive components to iterative implementations, eliminating stack overflows on large derivations.
- **Integer & Indexing Audit:** Full audit and correction of fixed-size integer handling and internal indexing structures.
- **Expanded Scaling:** Removal of legacy constraints; input capacity has been extended to greater than 100,000 formulas.
- **Fault Tolerance:** Correction of historical clause-handling defects to ensure robustness.
- **State Management:** Deterministic, architecture-neutral checkpoint and restore capability.

**Drop-in Compatibility:** LADR-2026 produces identical search behavior to McCune's original LADR-2009-11A.

---

## Performance & Benchmarks

- **Reliability:** Across 13,000+ TPTP problems, the system executes without segmentation faults.
- **Throughput:** Preprocessing performance improved an order of magnitude on large inputs. Inference throughput increased by approximately 10-20% under fixed benchmark configurations.
- **Validation:** LADR-2026 solved multiple TPTP problems rated 1.00 difficulty. These proofs were verified via GDV and IVY.

---

## Native TPTP Integration

LADR-2026 provides direct support for native TPTP input and TSTP proof output. No translation layer is required; proof objects conform to standard formats used in the modern ATP ecosystem.

---

## Premise Selection and Strategy Portfolio

The system integrates:

- **SInE-based premise selection** for handling large-theory problems.
- **Strategy Portfolio:** A Machine Learning (ML) selection of complementary search configurations.
- **Multi-Threading and Time-Slicing** A hyper-visor like manager for Multi-Tasking and Time-Slicing ML strategies.

While the underlying calculus remains unchanged, these implementation-level orchestrations significantly expand the system's "solve" profile.

---

## Determinism and Reproducibility

In single-strategy mode, LADR-2026 is fully deterministic: identical inputs and configurations produce identical proofs.

In multi-tasking mode, multiple search configurations execute in parallel. In this mode, proof selection may be influenced by process scheduling; repeated runs may produce different (but independently verifiable) proofs. This does not alter calculus semantics or soundness. All reported benchmark results specify the execution mode used.

## Disabled-clause memory option

Long searches can retain many clauses solely as proof ancestors. Add
`set(compress_disabled).` to compact the literal/term bodies of those cold
clauses after they leave every active index. IDs, justifications, attributes,
flags, and proof/checkpoint behavior are preserved; bodies are materialized
temporarily for printing, proof expansion, or checkpoint serialization. The
option is exact (it does not make search incomplete) and is currently default
off.

For longer ancestor-heavy searches, `assign(ancestor_store, memory).` replaces
each cold, official-ID clause by an immutable versioned record and retains only
a tagged offset in the disabled store and ID table.  Use
`assign(ancestor_store, mmap).` for the same exact representation backed by a
private temporary mmap file; `off` is the default.  Both archive modes retain
stable IDs, all proof flags and attributes, compact justifications and parent
IDs, and materialize only the requested final proof DAG.  Record bounds,
version, reserved fields, and header/payload checksums are validated before a
record is used.  A bad record aborts proof/checkpoint use instead of emitting a
possibly unsound proof.

In compact-OTTER dense-passive mode, a clause that leaves the selector with
unchanged archived metadata now retains its existing ancestor record directly.
This automatically avoids a second decode, compression, checksum, and append;
metadata changes use the original exact fallback.  No additional P9 option is
needed.  `Disabled_compression` reports `direct_retentions`,
`retention_fallbacks`, and `payload_bytes_avoided`, while `Ancestor_store`
reports the complete record and I/O reduction.

The mmap file is unlinked immediately and exists only for the running process.
Crash/restart remains the job of the architecture-neutral format-3 checkpoint:
checkpoint output materializes records to the established textual format, and
restart reconstructs the selected archive mode.  Existing checkpoints remain
readable, and non-clause formula placeholders stay in the existing
`formulas.txt` path.  See `P9-ANCESTOR-STORE.md` for the format and ownership
contract.

Normal statistics report compression attempts, successes, skips,
materializations and recompressions, along with active/full/compact logical
body bytes and live/peak FPA node/list counts.  The Phase 5 allocator reports
logical live/peak bytes, current/peak reservation, reusable/unallocated and
metadata fragmentation, direct/permanent storage, reclaimed slabs, allocation
traffic, and current/peak RSS separately.  Small pointer-count allocations use
reclaimable segregated slabs; wholly free excess slabs are returned to the
operating system and `memory_release_unused()` can purge the one warm slab kept
per active size class.  See `P9-ALLOCATOR.md` for the ownership and measurement
contract.

Run `make memory-tests` for the focused allocator churn/reclamation, compact
term layout, body, FPA, bookkeeping, ancestor-format, corruption, mmap, and
proof-DAG lifecycle regressions.  Run
`make -C test.src ancestor-store-scale` for the bounded 20,000-record accounting
test.
`benchmarks/run-memory-benchmarks.sh smoke` runs the bounded proof and
disabled-heavy comparisons across `off`, body-only compression, `memory`, and
`mmap`; `... all` also runs the two capped AIM cases when the supplied AIM
corpus is available.

## Long-run scalability reports

Periodic output is more useful than a final average when a search slows down
after millions of clauses.  Run
`test.src/compact_long_run_report.py RUN.out.gz` to summarize interval
given-clause throughput, generated work, clock shares, compact-index work,
PSS/swap, and file I/O directly from an existing plain or gzip output.  Use
`--format tsv` or `--format json` for every sample, `--tail 0` for a complete
Markdown table, and `--summary-only` for several runs.  No theorem search is
started by the reporter.  Put the reference first and add
`--compare-to-first` to audit exact final counters, CPU ratio, peak-RSS saving,
periodic evidence, and post-warm-up back-lookup CPU normalized by query count
plus exact successful answers.  Seven complete intervals are required; the
gate compares robust early/tail windows and tail stability.  Matrix runs
discover their adjacent GNU `time` sidecars automatically and include this
audit when more than one case is selected.  Set `CHAT_REFERENCE_OUTPUT` to
reuse an archived baseline without rerunning its prover; comparison thresholds
have corresponding
`CHAT_COMPARE_*` variables.  `CHAT_CGROUP_ACCOUNTING=1` measures whole-job
cgroup-v2 peak memory, including charged file cache, and refuses with exit 77
when the current scope is not delegated.  File-selector candidates must also
exercise reads and successfully advise at least 99% of consumed bytes out of
cache.  Interval output also reports selector minimum calls and run-head checks
per given, so file-queue CPU growth is visible.  It also requires the
month-scale file representation to report a 64-bit record reference in the
unchanged 24-byte entry.  See
`P9-LONG-RUN-SCALABILITY-RESULTS.md` for exact mature-run commands,
measurement caveats, and acceptance criteria.

---

## Availability & Positioning

LADR-2026 is an implementation-level modernization of a historically significant engine. We welcome independent benchmarking and scrutiny.

- **License:** Open source under GPLv2, consistent with the original LADR license.
- **Artifacts:** Source code and build instructions are included in each release.
