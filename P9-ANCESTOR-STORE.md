# Prover9 Phase 4 ancestor-store contract

Date: 2026-08-19 (Europe/Berlin)

This document specifies the exact, internal store used by
`assign(ancestor_store, memory).`, `assign(ancestor_store, mmap).`, and
`assign(ancestor_store, file).`.  The default is `off`.  The feature changes
representation and ownership only; it does not remove proof ancestors, relax
inference, or create an incomplete search mode.

## Ownership and references

A clause becomes archivable only after it has left every active list, selector,
demodulator, and inference/subsumption index.  It must have a nonzero official
clause ID.  Pre-ID elimination scratch clauses continue to use the Phase 2
body compressor, and formula placeholders remain resident for the established
`formulas.txt` checkpoint path.

Archiving is transactional from the caller's perspective:

1. Encode and append the complete immutable record.
2. Replace the paged clause-ID slot by the record offset.
3. Replace the disabled-store pointer by the same offset.
4. Delete the original `Topform` and its graph.

Failure before the ID replacement leaves the resident clause owned by the
disabled store.  A successful replacement leaves no stale `Topform` pointer.
Both tables use the low bit as an archive tag and store `offset << 1 | 1`;
ordinary aligned pointers retain a zero low bit.  There is one archive-enabled
disabled store per LADR process, matching Prover9's ownership model.

Metadata-only consumers read stable IDs, parent-ID arrays, polarity, and hint
IDs directly from records.  A body is decoded only for proof/checkpoint output
or another explicit body consumer.  Materialized clauses carry a private
ownership flag, are never installed into search indexes, and are released at
the end of the enclosing proof/output scope.  Final proof traversal follows
parent IDs and materializes exactly the reachable DAG.

Compact-OTTER dense passives are a temporary exception to the ordinary
store-handle ownership sequence: the dense directory owns the current record
offset while the clause remains selectable, so no disabled-store handle is
allocated yet.  If the clause is later disabled and its directory metadata
still matches the immutable record, `clause_store_retain_detached` transfers
that same offset into the disabled-store handle array exactly once.  The ID
slot continues to name the original record; the body is neither decoded nor
rewritten.  If `used`, activation/rewrite epoch, delayed-demodulator state, or
rewrite-rule debt changed, the directory marks the record dirty and the
caller uses the original materialize/rearchive transaction.  Handle append
order remains disable order, preserving proof/checkpoint traversal order.

## Version 2 record

Every integer field is fixed little-endian.  A record has a 96-byte header,
followed by five length-delimited sections.

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | magic `P9AR` |
| 4 | 2 | record version (`2`) |
| 6 | 2 | header size (`96`) |
| 8 | 8 | total record bytes |
| 16 | 8 | stable clause ID |
| 24 | 8 | matching-hint ID, or zero |
| 32 | 8 | last matched given ID |
| 40 | 8 | exact IEEE-754 weight bits |
| 48 | 4 | formula/normal/used/initial/negative/subsumer/given/goal flags |
| 52 | 4 | signed semantics bits |
| 56 | 4 | signed proof-tree cache bits |
| 60 | 4 | estimated original body bytes |
| 64 | 4 | compressed-body bytes |
| 68 | 4 | formula-term bytes |
| 72 | 4 | attribute-term bytes |
| 76 | 4 | compact-justification bytes |
| 80 | 4 | parent count |
| 84 | 4 | CRC-32 of the complete payload, XOR activation epoch |
| 88 | 4 | CRC-32 of header bytes 0--87 |
| 92 | 4 | DISCOUNT simplifier/activation epoch |

The payload order is the versioned Phase 2 exact clause body, a versioned
formula term, a versioned attribute term, the versioned compact justification,
then `parent_count` 64-bit parent IDs.  Absent sections have length zero.  The
term codec preserves full-range variable/symbol IDs and every term private
flag.  The justification codec covers every current LADR justification shape,
including resolution and paramodulation positions, rewrite triples, nested IVY
records, and INSTANCE substitution terms.  The separate parent array permits
proof size, tree weight, subsumption cost, and negative-parent walks without
decoding a justification or clause body.

Version 2 preserves the activation epoch needed by delayed collective
inference after an archived historical clause is materialized for a
checkpoint.  The epoch is folded into the payload checksum, so changing it is
detected even though the fixed 96-byte header and its original CRC boundary
remain unchanged.  The reader also accepts version 1 records, whose reserved
word must be zero and whose restored epoch is zero.

The record is process-internal: term sections use the current process's symbol
numbers.  It is deliberately not a new external checkpoint format.

## Validation and failure behavior

Before exposing a record view, the reader checks the store offset, minimum
header extent, magic, supported version, exact header size, the version-1
reserved zero,
header checksum, overflow-safe section sum and parent multiplication, exact
total size, store bounds, and payload checksum.  Section decoders then check
their own versions, lengths, varints, symbol/arity validity, term shape, and
justification tags/counts.  Decode failure destroys the partial clone and
increments `validation_failures`.

Optional metadata probes return failure where their API permits it.  Required
proof, checkpoint, sorting, ancestry, and teardown paths convert a missing or
invalid record into a fatal prover error.  They never skip a damaged parent or
print a partial derivation.  Thus corruption can make a run fail visibly, but
cannot silently turn a prover crash into an accepted proof.

## Backing modes and checkpoints

- `off`: preserve the Phase 3 disabled store; `compress_disabled` remains an
  independent, default-off body-only option.
- `memory`: grow the append-only bytes geometrically from 4 KiB with
  `realloc`.
- `mmap`: grow a private temporary file geometrically from 4 KiB with
  `ftruncate` and `MAP_SHARED`; sync it before checkpoint output.  The name is
  unlinked immediately, and close/crash removes the backing.  Complete cold
  pages are synchronized and advised `MADV_DONTNEED` in 8-MiB steps, but later
  random reads can fault them resident again.
- `file`: append complete records to the same kind of unlinked temporary file
  with `pwrite`, and retrieve them with `pread` through one grow-on-demand
  record buffer.  Logical file bytes and kernel page-cache bytes are not a
  process mapping and are therefore excluded from process RSS.  The resident
  store cost is the stable-offset vector plus that bounded buffer.  This is
  the recommended radical-RAM mode for long compact-OTTER searches.

Neither temporary-file mode is itself a restart file.  Format-3 checkpoint
writing materializes each archived clause, emits the unchanged textual clause,
atom flags, and justification records in disabled-store order, and
validates/syncs the backing first.  Resume reads old or new format-3
checkpoints through the existing loader, verifies all hashes, rebuilds active
indexes, and archives cold clauses again according to the restored
`ancestor_store` option.  Old checkpoints that do not name the option use the
default `off` mode.

## Instrumentation and tests

The normal statistics line reports record count and used bytes, backing
capacity, disabled-handle allocation, materialization count, validation
failures, mmap eviction counters, bounded I/O-buffer bytes, and file read/write
operations and bytes.  The count of current (nonsuperseded) disabled records is
maintained incrementally, so periodic statistics do not scan the file.  Body
statistics continue to report compressed and estimated full bytes so Phase 2
and Phase 4 are comparable.

Compact-OTTER statistics additionally report clean direct retentions, dirty
fallbacks, and compressed body/justification payload bytes whose duplicate
append was avoided.  The normal `Ancestor_store` record/read/write counters
include the complete header and payload effect and remain the authority for
physical/logical comparison.  Direct retention is automatic when its ownership
preconditions hold; there is no user-facing tuning option.

`ancestor_store_test` exercises memory, mmap, and file round trips, all scalar
and term flags, attributes, compact parents and justifications (including IVY
and INSTANCE), final-DAG reconstruction, synchronization, unknown versions,
malformed bounds, payload corruption, counters, and complete ID/store teardown.
It also covers detached-to-retained transfer, mismatched-ID rejection, exact
current/detached accounting, and retained-ID teardown.
`ancestor_store_scale_test` measures bounded record/handle/ID accounting in all
three modes.
Integrated x2 proof checks, capped AIM runs, and format-3 checkpoint
verification cover the search-level consumers.
