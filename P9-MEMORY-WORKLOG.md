# Prover9 AIM memory worklog (Phases 0--2)

Date: 2026-08-07 (Europe/Berlin)
Base revision: `b36df4c06d5794dd98e42335f191dd41d7abd2ab`
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

## Exact next actions for Phase 3 (not implemented)

1. Measure bytes per disabled clause for the `Clist` node, clause-ID hash entry,
   and surviving `Topform`, then design a paged ID-indexed cold record with
   sparse-ID handling.
2. Prototype O(1) ID lookup and compact iteration without changing ID stability,
   justification ownership, proof ordering, or format-3 checkpoint behavior;
   add proof/checkpoint equivalence tests before replacing either collection.
3. Re-run the bounded driver plus a larger synthetic high-water workload and
   compare peak RSS, allocator slab growth, lookup time, and bytes per retained
   ancestor. Separately fix the documented checkpoint selector divergence.

No Phase 3 representation work was started.
