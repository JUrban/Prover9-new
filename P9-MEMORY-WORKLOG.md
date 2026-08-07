# Prover9 AIM memory worklog (Phases 0--6)

Date: 2026-08-07 (Europe/Berlin)
Base revision: `b36df4c06d5794dd98e42335f191dd41d7abd2ab`
Phase 4 starting revision: `9172cd401095a10aebf5b90f835715baeca0aaca`
Compiler: GCC 13.3.0; GNU Make 4.3

The worktree was clean at the start. `make all` is the authoritative
sequential release build and `make all DEBUG=1` is the supported debug build.
A trial `make -j2 all` exposed an existing Mace4 copy/link race, so all recorded
builds use the documented sequential command.

## Phase 0 audit

### Disabled-clause ownership

`disable_clause()` removes a clause from demodulation, literal,
back-demodulation, clash, usable/SOS/limbo, and selector structures before
placing it in `Glob.disabled`. Other retention paths are input equality
orientation, inference outside the main loop, initial-SOS copying, predicate
elimination, and checkpoint restore. The first three now use the same checked
retention helper. Predicate elimination and restore compact their results only
after their body consumers finish.

The consumers were classified as follows.

| Class | Consumers | Decision |
| --- | --- | --- |
| Never see disabled clauses | inference, simplification, demodulation, subsumption, literal/back-demod/clash indexes, selectors, active hint index | Compression is allowed only after removal from all these lists/indexes. A checked active-list guard fails hard on a lifecycle violation. |
| Metadata only | clause-ID lookup, `get_clause_ancestors`, justification-parent walks, proof length/tree weight, used flags, negative-descendant traversal, disabled membership/sorting, compression statistics | Leave compact. `negative_clause_possibly_compressed()` uses cached polarity. |
| Need a body temporarily | native/TSTP/XML/IVY/debug clause printing, ordinary and expanded proofs, matched/new hints output, checkpoint clause text and atom flags | Materialize once for the enclosing scope and recompress afterward. Generic file/debug print entry points also provide a one-clause safety scope. |
| Transfer/retain ownership | `collect_prover_results()` and returned proof DAGs | Materialize intentionally and leave full because the result owner serializes or frees the proof later. |
| Restore-time body users | CAC property seeding, atom-flag restoration, FPA-ID lookup construction, index rebuilding, optional hit-list loading | Restored disabled clauses remain full through all restore operations, then compact before search resumes. |
| Destruction | `zap_topform()` / disabled-list teardown | Free either the full literal tree or the compact allocation, never both. |

Checkpoint writing scopes materialization across both clause text and the
subsequent per-literal atom-flag record. Justifications, IDs, attributes,
weights, `Topform` flags, and list/ID-table identity remain resident throughout.
The on-disk checkpoint remains format 3 and stores ordinary clause text; the
new compact representation is process-internal.

### Compressor audit and design

The legacy compressor encoded variables and symbol numbers in signed bytes,
declined large symbol tables, had a fixed 1,000-entry traversal stack, and
treated double compression as fatal. It had no size field for bounds checking.

The cold-body format now has a magic byte and version. Version 2 uses bounded
unsigned varints for full-range variable/symbol IDs and stores every rigid
term's private flags. Version 1 decoding remains supported for the earlier
flag-less version. Decoding validates magic, version, length, varint width,
symbol existence/arity, variable range, term flags, tree completeness, and
trailing bytes before changing the clause. Atom ownership is transferred from
the decoded clause term so nested flags are not lost. Failed decoding leaves
the compact body intact. Growable temporary arrays also remove the old
1,000-literal clause-wrapper boundary. Empty/formula bodies are safe counted skips;
compression, materialization, recompression, and deletion have explicit state
handling.

The option is `compress_disabled`, default off. It is exact and does not alter
completeness. The payload is not an external checkpoint format.

### FPA audit

`path_delete()` emptied a terminal `Fpa_list` and called
`fpa_trie_possible_delete()`. The old loop required every node in the upward
walk to have a non-null empty `terms` pointer. After freeing the terminal node,
the immediate intermediate node normally had `terms == NULL`, so pruning
stopped and the chain remained. The terminal list header was also abandoned
when its trie node was freed.

The corrected lifecycle frees an empty list header, clears `terms`, and prunes
while a node is non-root, termless, and childless. Each unlink updates the
parent list, child count, and hash table before freeing the node. Root ownership
is unchanged. FPA checkpoint restore also destroys the initialization root
before installing the restored trie.

The focused test covers depths 0 and 10, hash threshold 2 and effectively
linear lookup, structurally duplicate terms, shared prefixes, forward/reverse/
mixed deletion orders, repeated destruction, reinsertion with stable FPA IDs,
and a 96-child clustered hash deletion/rebuild stress. Live node/list counts
must return exactly to their pre-index baselines.

## Instrumentation

Normal statistics now separate:

- active, hint, full-disabled, compressed-disabled, and estimated original
  disabled body bytes;
- full/compact disabled clause counts and attempted/successful/skipped/
  materialized/recompressed operations;
- palloc cumulative bytes and whole allocator slabs reserved from `malloc()`;
- live and high-water FPA trie nodes and `Fpa_list` headers.
- compact disabled-store allocation versus the replaced `Clist` estimate;
- clause-ID entries, 64-ID pages, hash slots, allocated bytes, and the
  replaced fixed-table/`Plist` estimate.

Body-byte figures are logical allocation-size estimates for literal cells and
non-variable term cells. Compact bytes are payload bytes and exclude general
malloc metadata. Allocator reserve is slab capacity, while `/usr/bin/time -v`
peak RSS includes the complete process. These quantities are deliberately not
presented as interchangeable.

## Phase decisions and risks

- The option stays default off. Current short cases show exact on/off counters
  and no stable throughput regression, but long-run evidence is not yet broad
  enough to change the default.
- FPA pruning preserves the indexed set and FPA-ID ordering. Removing and later
  recreating empty paths changes allocation/layout and can perturb search
  ordering. On `LCC_to_aK1`, the unmodified binary found a proof at given 494,
  whereas the corrected index reaches the 500-given cap without that proof.
  This is a search-order observation, not an inference-rule or soundness
  change; capped non-proof outcomes are not used as correctness evidence.
- The palloc allocator keeps 20 MB slabs. Reclaimed term/literal cells become
  reusable, but short runs need not show an RSS drop. The intended long-run
  effect is avoiding later slab growth.
- Checkpoint verification with compressed ancestors passes all 16 hashes and
  resumes. The supplied repository has two pre-existing checkpoint issues that
  reproduce with `compress_disabled` off: `aK1_nil3_a` resume reports two more
  kept/SOS clauses at the same generated/given cap, and a sampled
  `LCC_to_aK1` resume later fails `avl_delete, item not found`. These are not
  caused by cold compression and remain follow-up correctness work.
- Direct `sb_*_write_clause` helpers are low-level and still expose their old
  compact-clause marker if called directly. All repository consumers that can
  see disabled clauses go through scoped proof materialization or the safe
  `fwrite_clause*` entry point.

## Phase 3 compact bookkeeping

The replaced clause-ID structure was a static 50,000-pointer bucket array plus
one 16-byte `Plist` cell per official clause. Each bucket was ID-sorted, but a
dense million-record run averaged 56.4 bookkeeping bytes per record when its
40-byte disabled `Clist_pos` was included. The `Topform` was 112 bytes and had
unused trailing padding.

The new ID table hashes sparse 64-ID pages. A page contains its page number,
live count, and 64 direct `Topform` slots; the open-addressed page table grows
at a bounded load factor and uses tombstones only until a same-size compaction
or final teardown. Lookup is one page hash plus one direct slot. Removing the
last ID frees its page, and deleting the last page frees the page table. A
restored isolated high ID therefore allocates one 528-byte page and a bounded
hash entry rather than storage proportional to the maximum ID.

Disabled clauses now use an append-order `Topform *` vector. Membership is an
O(1) byte flag placed in the existing `Topform` padding, so `sizeof(struct
topform)` remains 112. Appending preserves retention/checkpoint order. The
negative-descendant path applies the same stable merge sort by clause ID as the
old `Clist`, then merges usable, SOS, and compact-store iterators without
materializing compressed clauses. Generic Clist teardown was taught that the
disabled flag is also live ownership.

Checkpoint format 3 is unchanged. Specialized store writers preserve the old
ID-zero omission, clause text, atom flags, justification order, and scoped
materialize/recompress behavior. Restore loads through the existing temporary
Clist and transfers clauses to the compact store. Verification now additionally
hashes the checkpointed disabled IDs and count; the fresh aK checkpoint passed
18/18 checks. Formula collection from the paged ID table is ID ordered so goal
formula restoration remains deterministic.

`bookkeeping_lifecycle_test` covers 200,000 records by default and accepts a
larger count for bounded scale runs. It checks dense and sparse ID lookup,
stable next-ID assignment, formula collection, sort/order, O(1) membership,
exact counts/bytes, and complete page/store teardown. At one million records,
the two structures use 16,901,304 logical allocated bytes (16.901/record),
versus 56,400,032 bytes (56.400/record) for the replaced structures. This
excludes the unchanged 112-byte `Topform` and allocator metadata.

## Phase 3 decisions and remaining risks

- Compact bookkeeping is always on and exact; it adds no incomplete-search
  mode and changes no option default. `compress_disabled` remains exact and
  default off.
- The paged table preserves pointer lookup and IDs, but debug printing now
  follows page-hash order rather than legacy bucket order. Formula checkpoint
  output is explicitly ID ordered. Neither order participates in inference.
- Vector capacity is power-of-two high-water storage. Its steady cost tends to
  8 bytes per record at powers of two and can approach 16 just before growth;
  reported allocation uses actual capacity rather than an idealized count.
- Stable sorting temporarily allocates one pointer per disabled clause, as the
  old Clist sort did. It is only used by denial-descendant processing.
- Format 3 still omits disabled clauses with ID zero, matching the old writer.
  These pre-elimination clauses cannot be proof parents. The new disabled
  checkpoint hashes deliberately cover the serialized, official-ID subset.
- The two pre-existing checkpoint divergences above remain out of Phase 3.

## Phase 3 handoff (completed below)

1. Before Phase 4 implementation, specify the compact proof-record ownership,
   versioning, bounds checks, and proof-checker reconstruction contract.
2. Add a long, deterministic ancestor-heavy AIM soak to quantify when the
   current vector and paged-table high-water reaches another allocator/RSS
   boundary; keep strict time and work caps for routine CI.
3. Independently diagnose the aK kept/SOS resume divergence and LCC AVL restore
   failure before relying on checkpoint equivalence for a Phase 4 record log.

These were the Phase 3 handoff items.  The Phase 4 design, implementation, and
bounded evidence follow.

## Phase 4 versioned ancestor store

The detailed ownership, binary layout, validation, and checkpoint contract is
in `P9-ANCESTOR-STORE.md`.  The implementation adds the exact string option
`ancestor_store = off | memory | mmap`, default `off`.  Archive modes use the
Phase 2 body encoder, a new versioned term encoder for formulas/attributes, and
a versioned compact justification encoder covering every current LADR
justification variant.  Each record also carries an immediately readable array
of parent IDs.

Once an official-ID disabled clause has left all active indexes, the store
appends its record, atomically replaces both owning references by low-bit
tagged offsets, and frees the `Topform`, literals, terms, attributes, and
justification graph.  ID-zero pre-elimination clauses remain Phase 2 compressed
so assigning a synthetic ID cannot perturb search order.  Formula placeholders
remain resident because the established format-3 `formulas.txt` namespace owns
their restart representation.  The `Topform` additions reuse padding;
`sizeof(struct topform)` remains 112 on the measurement host.

Parent-only operations no longer assume that every ID maps to a pointer.
Proof DAG size/tree weight, ancestor subsumption cost, negative-parent walks,
CAC triggers, and delayed denial disabling use stable IDs or record metadata.
Final native/TSTP proof output, proof expansion, generic formula-parent output,
checkpoint serialization, and returned prover results materialize scoped
clones.  Archived hint matchers store the hint ID and relink to the separate
hint namespace for output.  Every scoped clone path was audited for release,
including proof-limit and derivation-only exits.

The 96-byte header and every multibyte integer are explicitly little-endian.
Readers reject bad offsets, truncated headers, magic/version/header-size
mismatches, nonzero reserved fields, header CRC mismatch, arithmetic overflow,
section/parent bounds mismatch, payload CRC mismatch, and invalid nested term
or justification encoding.  Metadata operations required by proof or
checkpoint correctness fail fatally on damage; materialization APIs return a
visible failure and never a partial clause.  The corruption counter is printed
with normal statistics.

`memory` grows a heap byte vector geometrically.  `mmap` grows an unlinked
temporary file with `ftruncate`/`MAP_SHARED` and calls `msync` before checkpoint
serialization.  The mmap file is an ephemeral backing, not a competing restart
format.  Checkpoint format 3 is unchanged and architecture-neutral: records
are validated/materialized into the same text, flags, and justification files,
then reconstructed in the selected backing after restore.  Old checkpoints
without the option migrate to the default `off` path.

### Phase 4 verification

- Sequential release build, project `test1`, and `make memory-tests` pass.
  The new focused test runs both memory and mmap backings; round-trips clauses,
  private term flags, weights, attributes, parent arrays, and proof flags;
  covers paramodulation, demodulation, IVY, and INSTANCE records; builds a final
  proof DAG; rejects corrupt version, size, and payload records; syncs; and
  returns every store and ID-page allocation to baseline.
- The LADR library plus the ancestor-store and existing memory lifecycle tests
  pass ASan+UBSan with leak detection and halt-on-error enabled.
- x2 has identical given/generated/kept counts (12/118/23), proof length 16,
  and proof content under `off`, Phase 2 compression, `memory`, and `mmap`.
  Both `prooftrans parents_only` and `directproof` accept the archive proof;
  TSTP output contains a complete `CNFRefutation` block and theorem status.
- Capped LCC and aK prefixes have identical work, active sets, and FPA counters
  in all four modes.  LCC is 501/258,858/562/76 at its given cap; aK is
  201/84,789/340/58.
- A fresh mmap-backed aK format-3 checkpoint at given 29 serialized 5,235
  clauses, including eight archived official-ID ancestors.  Resume passed all
  18 hashes and reconstructed sos=65, usable=37, disabled=8.  It later reached
  the repository's pre-existing `avl_insert, item already there` restore fault.
  The same family of AVL/selector restart defects is documented above for
  option-off/Phase 2 runs, so Phase 4 claims checkpoint representation and
  verification compatibility, not a repaired general restart algorithm.

### Phase 4 decisions and remaining risks

- The feature stays default off.  Exact bounded work and proof results are
  strong enough to expose the option, but not to change defaults without a
  longer ancestor-heavy soak and broader proof corpus.
- A 96-byte fixed header and exact proof metadata can outweigh a tiny clause.
  The scale result is favorable for the measured representative clause, while
  body-only Phase 2 compression remains useful when record metadata dominates.
- The mmap backing can reduce heap/RSS pressure only when the kernel can evict
  cold pages; these short tests fit in the same RSS plateaus.  The unlinked file
  intentionally cannot recover an uncheckpointed crash.
- Compact parent IDs inherit the existing justification layer's signed-int
  parent range even though the record and clause ID fields are 64-bit.  This is
  a pre-existing representational limit, not widened in Phase 4.
- The outstanding deterministic-restart AVL/selector failure remains the first
  correctness follow-up.  Phase 5 was not started.

## Phase 5 reclaimable allocator and exact representation

The Phase 5 design and accounting contract are in `P9-ALLOCATOR.md`.  The old
global per-size free lists were replaced by 1 MiB, power-of-two-aligned,
segregated slabs for pointer classes 1--127.  Each slab owns its free list and
tracks exact live/free/unallocated slots; this prevents a reclaimed mapping
from leaving dangling free-list links elsewhere.  Larger objects are directly
allocated, and permanent `tp_alloc` ownership is explicit.

Native builds use anonymous mappings so a completely free excess slab is
returned with `munmap` without moving any live pointer.  A single empty slab
per class stays warm because a measured release-on-every-empty prototype made
short object churn syscall-bound.  `memory_release_unused()` purges warm slabs,
and `max_megs` does so automatically before failing.  Emscripten retains a
safe malloc/free fallback and class-local slab lookup.

Normal statistics now distinguish logical current/peak bytes, mapped/direct
current/peak reservation, reusable/unallocated/metadata fragmentation,
direct/permanent storage, slab reclamation, allocation traffic, current RSS,
and peak RSS.  The legacy monotonic `megs_malloced()` remains usable by Mace4
and progress code as peak reservation; current reservation is no longer
inferred from it.

Making free storage genuinely inaccessible exposed one old ownership bug:
`zap_flatterm()` reread its first node after freeing it.  Capturing the list
boundary before the first free preserves the exact traversal and allows the
slab to disappear safely.  Release, debug, and sanitizer proof/lifecycle tests
cover the corrected path.

The representation study was deliberately separate.  The measured aK prefix
allocated 2,005,677 term nodes, making the redundant stored pointer to the
already-contiguous argument array a clear target.  Deriving the array from the
header shrinks 64-bit terms from 32 to 24 bytes without changing mutability,
identity, ordering, proof text, archive encoding, or checkpoint format.  The
paired run saved 16.0 MB of allocation traffic, 426 KB live logical memory,
one 1 MiB slab, and about 536 KiB peak RSS.

Hash-consing was rejected because terms are mutable and carry container and
FPA/auxiliary state.  Packed literals were rejected because the measured live
saving is small and a 16-byte result requires pointer tagging across hundreds
of direct accesses.  Attributes were too rare, and justification/parajust
live counts too small to justify tagging or another arena after Phase 4 already
compacted cold proof metadata.  No speculative layout was bundled into the
term patch.

### Phase 5 verification and compatibility

- The 300,000-object allocator test spans 74 slabs, touches 76.8 MB, reclaims
  mappings while a later pointer remains valid, observes RSS fall by over 70
  MiB, checks exact fragmentation components, purges the warm mapping, and
  adds four mixed-class shuffled churn rounds.
- Sequential release tests 1--6, `make memory-tests`, the supported debug
  build/test1/memory tests, and ASan+UBSan focused tests pass.  Leak-enabled
  allocator churn is clean; initialized tests have only known global registry
  retention and pass invalid-access/UB checks with leak reporting disabled.
- x2 work and its 16-step proof are unchanged in all four ancestor/body modes.
  Normalized proofs are byte-identical; `prooftrans`, `directproof`, and TSTP
  archive proofs pass.
- LCC and aK fixed-work counters and FPA live/peak counts are identical across
  modes and to the Phase 5 baseline.  Final single-run allocator overhead is
  about 2.6% on LCC and 7.1% on aK; the term compaction offsets memory traffic.
- A fresh mmap format-3 checkpoint saved 5,329 clauses at given 84, resumed,
  restored archive/proof state, and passed 18/18 hashes.  Its later kept-count
  divergence is the previously documented selector/restart defect.  No
  checkpoint version or compatibility rule changed.

Phase 5 changes no inference, soundness, completeness, default option, proof
contract, or archive default.  Slabs are process-global and non-thread-safe as
before.  Reservations are virtual mappings rather than RSS; warm sparse slabs
therefore remain visible as fragmentation and are reported separately.  Phase
6 was not started.

## Next three actions after Phase 5

1. Diagnose and repair the format-3 AVL/selector duplicate on aK and the
   related option-off resume divergence, then repeat full-run checkpoint
   equality with all ancestor modes.
2. Profile the Phase 6 unification-context clear path and prototype a sparse or
   generation-stamped context behind focused equivalence/throughput tests; do
   not infer a win from allocation statistics alone.
3. Add a bounded long-churn allocator workload with phase-boundary
   `memory_release_unused()` calls to measure warm-slab virtual fragmentation,
   RSS return, and mapping rate on Linux, macOS, and Emscripten.

## Phase 6 outcomes

The three Phase 5 follow-ups are resolved.

1. Commit `77660fc` repairs selector duplication, restores the nonunit feature
   index after fast FPA loading, and preserves `unfold` symbol state.  Fresh
   and resumed aK and LCC runs now have identical final work counters; the
   post-checkpoint aK kept trace is byte-identical and both resumes pass all
   18 verification hashes.
2. An opt-in `CONTEXT_PROFILE` build observes every substitution bind/unbind
   path without changing release layout.  Both sampled AIM prefixes allocate
   only five contexts concurrently (8,080 release bytes total), 95.7--96.5%
   of lifetimes are empty, non-empty lifetimes touch about 2--3 slots, maxima
   are six/seven, and dirty frees are zero.  Generation stamping is rejected:
   it enlarges a tiny pool that the existing private freelist already reuses
   without clearing.  Sparse contexts cannot affect total RAM materially.
3. The bounded churn check now makes platform capability explicit.  Linux
   asserts RSS rise and contraction using `/proc`; macOS obtains current RSS
   through Mach; Emscripten requires logical slab reclamation but does not
   pretend its linear heap can return pages to the host.  The local Linux
   150,000-object run passed and contracted from 40,304 to 1,620 KiB after
   purge.  macOS/Emscripten toolchains are unavailable on this host and are
   not reported as executed.

The requested pre-project/current measurement used the exact same 2,000-given
`Ka_to_aK1` prefix for 69--73 seconds.  Both builds performed
2,001/6,980,123/2,198 given/generated/kept and both peaked at 14,720 KiB RSS.
Logical live bytes fell only 4.57%, from 11,230,432 to 10,717,376.  Detailed
pre-project accounting assigns at least 56.8% of live allocation to FPA and
23.4% to terms/argument arrays.  The archived AIM statistics and the local
Waldmeister paper were then used to produce `P9-RADICAL-MEMORY-PLAN.md`; its
core is packed immutable index segments, an immutable shared term bank with
mutable sidecars, and a Waldmeister-style collective passive frontier.
