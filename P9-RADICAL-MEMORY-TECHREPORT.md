# Reducing Memory in Hint-Guided Prover9 Saturation

## From passive-clause explosion to a compact OTTER-compatible given-clause loop

**Technical report, 11 August 2026**

**Implementation branch:** `phase5-compact-frontier`

**Accepted code:** `2483a7a49e4e21c54b1692c142b55435eeefe44b` plus audit
commit `ac3f0fe`

**Status:** all hard Phase-5 correctness, proof, CPU, memory, checkpoint, and
accounting gates passed on the current `chat_test`/Osborn acceptance problem

This Markdown file is the reviewable primary report.  The standalone LaTeX
companion is [`P9-RADICAL-MEMORY-TECHREPORT.tex`](P9-RADICAL-MEMORY-TECHREPORT.tex).
It has no external bibliography file and can be typeset on a TeX installation
with either `latexmk -pdf P9-RADICAL-MEMORY-TECHREPORT.tex` or two runs of
`pdflatex P9-RADICAL-MEMORY-TECHREPORT.tex`.

## Abstract

Long hint-guided Prover9 searches in algebra can retain millions of clauses in
the set of support (SOS), millions of demodulators, millions of disabled proof
ancestors, and hundreds of thousands of hints.  Historical runs reported
18.8 GiB for an Osborn proof and 125.8 GiB for an AAPERM prefix.  The initial
hypothesis that disabled clauses alone caused this growth was only partly
correct.  Clauses rejected immediately as tautologies or by forward
subsumption were not retained, while deleting almost all disabled clauses saved
only 22% at the 4,000-given Osborn boundary.  The dominant architectural
problem was that retained but not yet selected SOS clauses remained represented
as ordinary pointer-rich clause/term graphs and participated in eager
simplification indexes.

This report describes two responses.  The first is a DISCOUNT-style loop with a
strict active/passive separation, dense passive records, packed hints, and a
bounded collective representation of delayed paramodulation and
hyperresolution work inspired by Waldmeister.  It gives the strongest
asymptotic memory bound, but it changes when simplification and generating
inferences occur.  On the supplied Osborn hint chains, matching more hints did
not reproduce the historical proof: demodulation and inference timing are part
of the effective search strategy.

The accepted response is therefore an eager, compact OTTER-compatible loop.
It preserves the old treatment of every retained clause for demodulation, unit
conflict, subsumption, backward demodulation, hint matching, and given-clause
selection, but replaces resident clause graphs and pointer-based passive
indexes by an immutable file archive, dense selector records, stable clause
numbers, shared serialized terms, and four compact authoritative indexes.
Ordinary clauses are reconstructed only for exact logical tests or activation.
Every compact index is a filter: it may admit extra candidates, but the
ordinary Prover9 operation makes the final decision.

On the accepted full proof, old P9 used 550,400 KiB peak RSS and 765.69 seconds
of user CPU.  Compact OTTER proved at the same 2,945-given boundary, with the
same 7,051 proof clauses and 3,231 new hints, in 754.78 user seconds and
127,420 KiB peak RSS.  This is a measured 76.85% whole-process memory reduction
(4.32 times smaller) with 1.42% less user CPU.  A frozen terminal accounting
explains 96.93% of proportional resident memory.  A newly supplied 11,000-given
comparison records an earlier compact-index implementation at 355.86 MiB versus
3,354.28 MiB in Prover9's internal counter, but also 2.963 times the CPU.  That
experiment's terminal process diagnostics report 1,492,656 KiB versus
3,603,800 KiB peak RSS, a smaller but still substantial 58.58% reduction.  It
diagnosed candidate-scan amplification; it is not evidence for the performance
of the final implementation, and neither process RSS nor internal accounting
includes unmapped filesystem page cache in total job memory.

The main conclusion is methodological as well as quantitative.  A radical RAM
reduction could not be obtained by freeing one list or by compressing clause
bodies in isolation.  It required preserving the prover's contraction and
guidance semantics while changing ownership across the whole saturation state:
passive bodies, proof ancestors, hint bodies, selection records, rewrite
indexes, unit indexes, subsumption indexes, and temporary compaction data.

## 1. Reader's guide and terminology

The intended reader knows saturation theorem proving in the terminology used
for OTTER, Prover9, E, Vampire, the *Handbook of Automated Reasoning*, or
Harrison's *Handbook of Practical Logic and Automated Reasoning*.  The report
uses the following standard view.

- The **passive set**, called `sos` by Prover9, contains retained clauses
  waiting to be selected.
- The **active set**, called `usable` by Prover9, contains selected clauses
  available as parents of generating inferences.
- One **given clause** is selected from passive, moved to active, and combined
  with suitable active clauses by paramodulation, hyperresolution, or the other
  enabled inference rules.
- **Expansion** means generating new clauses.  **Contraction** means
  simplification and redundancy elimination: demodulation (rewriting), unit
  deletion, forward and backward subsumption, and backward demodulation.
- A **demodulator** is an oriented or conditionally oriented equation used as
  a rewrite rule.  Backward demodulation uses a newly admitted demodulator to
  rewrite already retained clauses.
- A **hint** (historically also related to a resonator) is a clause used for
  search guidance, not as an inference premise.  A derived clause that
  subsumes a hint, is subsumed by one, or satisfies the configured matching
  relation can receive favorable weight or selection treatment.

Several implementation terms recur below.

- A **stable clause number** is the ordinary Prover9 clause ID used in a data
  structure instead of a C pointer.  It survives movement between in-memory
  and file representations.
- A **dense record** is a fixed-size array element holding selection and status
  fields rather than a separately allocated graph of objects.
- **Materialization** means reconstructing an ordinary Prover9 clause/literal/
  term graph from its immutable serialized representation for an exact test.
  The reconstructed object is temporary unless the clause is selected.
- A **posting list** is a list of stable clause numbers that share an index
  feature, such as a root symbol or a shallow term path.
- A **conservative filter** never omits a possible answer.  It may return false
  positives, which the established matcher, unifier, subsumption test, or
  rewrite test then rejects.
- The **shared term pool** serializes each retained clause once for the compact
  indexes.  It shares one clause serialization among several indexes; it is
  not yet global hash-consing of equal subterms from different clauses.

The adjective “exact” in this report refers to the answer of the complete
operation, not necessarily to the candidate set returned by an index.  For
example, an index may conservatively return three clauses, after which the
ordinary subsumption test decides that one truly subsumes the query.  Exactness
means that this final answer agrees with the old implementation and no possible
answer was lost.

## 2. The workload and the original memory problem

The motivating AIM searches are far outside the scale suggested by their given
counts.  A few thousand selected clauses can generate and retain millions of
SOS clauses, and many of those clauses can become simplifiers before they are
ever selected.  Three archived states illustrate the scale.

| Archived state | Given | Generated | Kept | SOS | Demodulators | Disabled | Hints | Prover9 `Megabytes` |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Osborn, 4,000-given prefix | 4,000 | — | — | 588,252 | 489,224 | 577,347 | 310,153 | 2,626.98 MiB |
| Osborn proof | 11,242 | 212,152,996 | 11,131,760 | 4,594,786 | 3,759,850 | 6,519,452 | 310,153 | 18,810.85 MiB |
| AAPERM long prefix | 12,560 | 280,095,326 | 13,109,703 | 8,504,637 | 6,199,295 | 4,593,127 | 220,117 | 125,829.01 MiB |

The AAPERM time profile also shows that the cost was not primarily generating
inferences.  Of 225,533 user seconds, `infer` accounted for about 902 seconds,
while preprocessing, subsumption, indexing, disabling, and backward
demodulation dominated.  This is typical of a contraction-heavy algebraic
search: the number of retained objects and the work done to maintain them are
more informative than the raw number of inference-rule invocations.

### 2.1 What `Plans-RAM-24.txt` corrected

The discussion archived in [`../Plans-RAM-24.txt`](../Plans-RAM-24.txt)
prevents two tempting but incorrect readings of the statistics.

First, `Generated` is not a live-clause count.  In one 1,000-given Osborn run,
about 1.4 million printed tautologies arose while processing generated or
backward-rewritten clauses.  Such immediate tautologies and clauses rejected by
forward subsumption are normally destroyed.  They consume CPU and temporary
allocation traffic, but they do not explain terminal RAM by their count.

Second, disabled clauses do matter, especially near a long proof, but they are
not the entire problem.  A direct experiment that removed almost all disabled
clauses measured the following internal-memory changes:

| Boundary | Ordinary | Delete disabled | Saving |
| --- | ---: | ---: | ---: |
| 1,000 givens with hints | 674 MiB | 644 MiB | 4.5% |
| 2,000 givens with hints | 1,101 MiB | 963 MiB | 12.5% |
| 4,000 givens with hints | 2,627 MiB | 2,037 MiB | 22% |
| 8,000 givens | 9,192 MiB | 6,470 MiB | 30% |

At 4,000 givens, the disabled population fell from 577,347 to 383 while the
588,252-clause SOS and its eager indexes remained.  A 22% saving is useful, but
it cannot meet an 80--90% target.  The experiment also exposed why simply
calling `delete_clause` was unsound operationally: justifications and the final
proof refer to parent clause numbers.  Freeing a disabled object without a
replacement proof-history representation leaves dangling references.

The correct conclusion is therefore:

```text
Rejected generated clauses       mostly transient
Disabled retained clauses        important proof-history cost
Live SOS clauses and indexes      dominant growth law
Hints                             large fixed and mutable cost
Demodulator/index maintenance     dominant CPU cost on hard algebra
```

### 2.2 Why `Megabytes` is not RSS

Historical Prover9 `Megabytes` is an allocator-oriented internal statistic.
It does not necessarily include all anonymous mappings, mapped files, libc
arenas, shared pages, or file-backed resident pages.  Conversely, the logical
size of an archive file is disk usage, not resident RAM.  The report therefore
uses four separate quantities where available:

1. Prover9's logical/component byte counters;
2. RSS sampled from `/proc/<pid>/status` or `smaps`;
3. PSS from `smaps`, which apportions shared pages;
4. the maximum RSS reported by `/usr/bin/time -v`.

This distinction mattered in a long DISCOUNT run whose deleted mmap file had a
logical size of 13,419,485,217 bytes and 9.7 GiB of allocated disk blocks, but
10,223,128 KiB of its mapping was actually resident.  An mmap is not a promise
that data are cold: scanning it, including from a statistics routine, faults
pages into RAM.  The accepted compact-OTTER configuration consequently uses
explicit file reads rather than an mmap for the ancestor/passive archive.

Explicit file I/O can still populate the kernel page cache.  Those pages are
reclaimable and are not included in process RSS/PSS, but they can be charged to
a cgroup and do consume machine RAM until reclaimed.  Therefore 127,420 KiB is
an exact **process peak-RSS** result, not a measurement of every kernel page
temporarily used on behalf of the job.  A claim about total machine or
container memory must add cgroup `memory.current`/`memory.peak` and the `file`
fields of `memory.stat`.

## 3. Semantic constraints: why this was not just a compression task

A clause waiting in Prover9's SOS is passive for *generating* inferences, but
under the traditional OTTER loop it is not passive for contraction.  A newly
retained equation can become a demodulator immediately.  It can rewrite old
SOS and usable clauses, participate in backward demodulation of hints, and
change which later clauses survive forward subsumption.  Unit clauses can
participate in unit conflict and unit simplification.  Nonunit clauses must be
visible to subsumption.  Hint matching is performed while each candidate is
processed, before it enters the SOS, and the matched-hint result affects its
weight and selector membership.

These observations impose four contracts.

### 3.1 Search contract

For a trajectory-preserving mode, retaining a clause must trigger the same
eager contraction transaction as ordinary OTTER.  Selection cycles, clause
weights, hint degradation, `match_once`, matcher limits, and action rules must
see the same results in the same order.  Compact storage is allowed to change
addresses and representation; it is not allowed to silently turn an OTTER loop
into a DISCOUNT loop.

### 3.2 Hint contract

Hints are not inference premises, so they need not be inserted into active
paramodulation or resolution indexes.  Nevertheless, all configured hint
operations must remain available:

- equivalence and duplicate handling during hint initialization;
- ordinary and flipped matching during candidate processing;
- subsumption-based hint recognition;
- hint degradation and last-match state;
- rewriting and reindexing when `back_demod_hints` is set;
- checkpoint and resume of mutable hint state.

Thus a cold passive clause can still be checked against every hint: the check
occurs while the newly derived clause is an ordinary materialized candidate.
It is archived only after simplification, exact hint matching, weighting, and
retention.  No hint behavior is recovered by scanning the passive archive
later.

### 3.3 Proof contract

Every proof parent must remain recoverable by stable clause number after the
ordinary clause body has been freed.  A proof archive must retain the clause,
attributes, flags, matching-hint information, and justification.  Corruption
must cause a checked failure rather than a malformed proof.  Final proof
extraction can reconstruct the proof DAG on demand.

### 3.4 Index contract

Removing the full passive clause graph also removes the pointers stored in
ordinary term and feature indexes.  Each required operation therefore needs
either a compact authoritative replacement or an ordinary live representation.
The compact-OTTER mode is deliberately refused at startup unless all four
passive-sensitive replacements are enabled.  There is no partial mode that
silently misses passive clauses.

## 4. Waldmeister's lesson and the first architectural path

Hillenbrand's account of Waldmeister starts from a standard active/passive
completion loop.  It emphasizes three implementation lessons directly relevant
here.

1. Compact discrimination structures can improve both space and cache behavior;
   variable-size nodes need not be pointer lists, and unary paths can be
   collapsed.
2. A strict active/passive separation avoids normalizing the entire passive set
   on every iteration, as an OTTER loop would do.
3. Sets of critical pairs generated from one newly active equation can be held
   collectively as a constant-size descriptor plus the minimum weight of the
   unselected pairs, with only a fixed number of promising pairs represented
   individually.  Waldmeister changed passive space from quadratic to linear in
   its abstract search time.

The result reported for Waldmeister is striking: more than 500 million passive
equations and over 70,000 active equations could be handled in about 200 MB.
That result is for unit equational completion, not for Prover9's full mixture of
paramodulation and hyperresolution, and it cannot be imported as a benchmark.
It nevertheless supplied the right architectural question: can the prover
represent *delayed inference work* instead of materializing every conclusion?

### 4.1 DISCOUNT ownership boundary

The first answer added `assign(search_loop,discount)`.  In this mode only
selected active clauses are in inference and simplification indexes.  A new
candidate is simplified, hint-matched, weighed, and stored passively, but does
not become an inference parent or demodulator until selection.  On selection it
is refreshed against the current simplifier epoch; a changed clause is requeued
or discarded with an explicit justification.

This is a standard DISCOUNT-style distinction, not a new calculus.  With fair
selection it is a natural saturation architecture.  It does, however, order
contraction differently from Prover9's historical OTTER implementation.

### 4.2 Dense passive storage

`assign(passive_store,dense)` replaces an ordinary passive `Topform` graph,
list positions, and selector nodes by a dense record containing a stable clause
number, selector keys, weight, semantics, matched-hint ID, and epoch/status
bits.  The complete clause and justification are serialized.  Selector heaps
hold 32-bit record positions and tolerate lazily removed entries.  Dead archive
records are compacted in batches.

The important ownership property is that the passive selector does not own a
second copy of the clause body.  The proof/archive representation is the body
owner, and a selected clause is reconstructed from it.

### 4.3 Packed hints

The old FPA hint index retained pointer-rich term forests for hundreds of
thousands of hints.  `hint_index=packed` and its successor
`hint_index=packed_fast` retain immutable compressed hint bodies, compact
feature postings, and small mutable side arrays for active/degraded status.
Candidate retrieval is by conservative structural features; the established
hint matcher and subsumption code decide the result.

The initial packed implementation saved memory but was too slow.  At one
1,000-given full-hint boundary it used 295,680 KiB rather than old P9's
507,108 KiB, but took 441.39 rather than 113.44 CPU seconds.  Instrumentation
showed billions of broad posting candidates.  The `packed_fast` work added
density-adaptive intersections, exact feature profiles, bounded dependency-
scoped caching, direct matching of compressed unit hints, and compact handling
of `_AnyConst`.  At a 300-given selected-DISCOUNT boundary it took 10.23 user
seconds, compared with 27.72 for FPA and 50.02 for the former packed path,
while preserving the complete hint trace.

### 4.4 Collective inference scheduling

The collective frontier delays the expansion work associated with an activated
given clause.  Instead of materializing every paramodulant or hyperresolvent at
once, it records bounded descriptors for:

- paramodulation from the given clause;
- paramodulation into the given clause;
- positive hyperresolution;
- negative hyperresolution.

Each descriptor stores stable parent IDs, a snapshot boundary for the active
set, a rule-specific continuation, and fairness/checkpoint state.  A balanced
scheduler gives each enabled rule a nonzero service share, limits outstanding
descriptors with high/low water marks, and bounds the number of raw conclusions
admitted in one turn.  Paramodulation and hyperresolution continuations resume
natively rather than regenerate and discard an ever longer prefix.

Additional experiments ranked a bounded set of candidate conclusions by a
read-only hint preview.  Preview never assigns a clause ID, mutates a hint,
degrades a hint, or changes authoritative processing.  A dual-cursor discovery
lane can promote a promising raw conclusion early and later prove, by ordinal
and structural checksum, which fair conclusion must be skipped.  FIFO service
still supplies the fairness argument.

This machinery is checkpointed: active history, deactivation epochs, pending
descriptors, continuation ordinals, prefix checksums, candidate-pool state, and
fairness position are restored and verified.

### 4.5 Why the first path did not prove Osborn

The DISCOUNT/collective path achieved small resident passive frontiers and, in
some long runs, matched more distinct hints than the historical proof.  It did
not establish the same theorem within the tested time.  That is not a
contradiction.

A hint match is local evidence that a useful clause *shape* has appeared.  It
does not say that all of the proof clause's ancestors were generated, that the
same demodulators were available when they were needed, or that the clause
survived in the same normal form.  The old OTTER loop allows an unselected SOS
equation to become a demodulator immediately.  Selected DISCOUNT delays that
rule until activation.  Eager-interreduced DISCOUNT rewrites more aggressively,
but can thereby change or remove clauses expected by a historical hint chain.
Collective expansion changes the interleaving of paramodulation and
hyperresolution conclusions again.

Experiments with selected, eager-legacy, and eager-interreduced DISCOUNT found
that demodulation timing dominates this equational workload.  The stronger
eager mode also accumulated rewrite-repair debt: old passive clauses had to be
revisited after the rewrite relation changed.  Bounded high/low water marks and
forced inference turns fixed a liveness bug in which the prover could otherwise
remain in a permanent rewrite drain, but they did not recover the old proof
trajectory.

The lesson is the same one stated in the Waldmeister paper's conclusion: a
complete refinement can change practical search behavior in an unforeseen way.
For the Osborn chain, maximum memory reduction and historical search replay are
different product requirements.

## 5. The accepted architecture: eager compact OTTER

Phase 5 keeps `search_loop=otter` and `inference_frontier=clauses`.  Every
retained clause completes Prover9's ordinary eager processing transaction.
The change is representational: after that transaction, a cold passive clause
is archived and all long-lived indexes refer to compact records by stable
clause number.

The operational flow is:

```text
infer an ordinary candidate clause
    -> demodulate and simplify literals
    -> safe unit-conflict test
    -> exact hint matching and weighting
    -> limits / semantics / keep-delete rules
    -> forward subsumption
    -> assign stable clause number
    -> test/admit new demodulator
    -> backward subsumption and backward demodulation
    -> update all four compact indexes
    -> serialize body + justification to the archive
    -> retain dense selection record and stable IDs only

select a given clause
    -> locate archive record by stable number
    -> reconstruct ordinary clause
    -> remove its compact passive memberships
    -> activate it in the ordinary inference set
    -> make ordinary inferences at the historical point
```

### 5.1 One immutable file archive

`ancestor_store=file` uses an append-only, checksummed record format and
explicit `pread`/`pwrite`.  A record contains the compressed clause body,
attributes, flags, matched-hint state, and justification/parent numbers.
Dense passives and disabled ancestors share this proof/archive ownership; the
passive selector does not keep another body arena.

The logical file length is not process-resident RAM.  In the accepted proof it
was 84,404,096 bytes, while the reusable I/O buffer was 4 KiB.  The process
performed about 3.8 million reads totaling 374 MB and about 412,000 writes
totaling 84.4 MB.  Explicit I/O prevents an innocent address-space scan from
making the whole archive part of process RSS, as happened with mmap.  The
kernel may nevertheless cache file pages; cgroup accounting is required when
the desired quantity is total job memory rather than process RSS.

Each record has version, bounds, reserved-field, and checksum checks.  A bad
record aborts materialization rather than being used in a proof or exact test.

### 5.2 Dense given-clause selection

The dense SOS record holds the information required by Prover9's selection
rules without retaining literals and terms.  It preserves the exact low/high
selection cycles, breadth-first level, semantics, weight, hint ID, age, and
membership status.  In the final proof, 131,001 passive records used 9,532,864
bytes and their selector heap used 611,368 bytes: about 77 bytes per active
record for the reported dense-record component, rather than one general clause
graph plus multiple list/index nodes.

A decoded-passive cache exists, but the accepted configuration sets
`compact_passive_cache=0`.  A controlled 1,500-given comparison with 0, 4, and
32 MiB budgets showed that avoiding about 98,000 decodes did not improve CPU,
while nonzero settings increased RSS.  Repeated exact work was dominated by
index behavior, not archive decoding.

### 5.3 Compact rewrite bank

The compact demodulator bank stores serialized left- and right-hand sides,
rule kind, activity, and stable proof ID.  A radix-compressed discrimination
structure retrieves candidate rewrite rules.  Common unary paths are collapsed
into token slices.  Reverse occurrence streams identify clauses that may be
rewritable by a new rule for backward demodulation.

Matching uses the same orientation and rewrite semantics as ordinary Prover9.
The compact tree retrieves candidates; exact matching, ordering, and rewriting
determine the result.  At the final boundary the bank held 109,987 current
rules in 11,205,308 bytes.

### 5.4 Compact unit index

The unit index supports the operations for which ordinary unit discrimination
trees previously held clause pointers: generalization, instance/unifier
retrieval, forward/back unit subsumption, and unit conflict.  It stores
radix-compressed token paths, compact postings, polarity, a symbol mask, and a
stable proof ID.  Candidate clauses are decoded only for the authoritative
logical test.  It occupied 13,166,960 bytes at the final boundary.

### 5.5 Compact backward-demodulation index

Backward demodulation needs occurrences of possible redexes, not just clause
root symbols.  The new index groups occurrences by root symbol, clause number,
and exact shallow path signature.  Monotone record and occurrence positions are
delta encoded into small blocks.  A query opens only path buckets compatible
with its fixed symbols; the ordinary matcher checks every survivor.

This index also exposed the main CPU hazard of compact representations: a
small record does not help if a weak key returns billions of candidates.
Shallow path filters, grouped occurrences, delta streams, bounded rebuilds, and
stable-ID snapshots were added to control that amplification.  The final bank
used 12,109,376 bytes and reported 174,774 exact candidate tests.

### 5.6 Compact nonunit subsumption index

Nonunit forward and backward subsumption use a stable-ID feature trie.  Clause
features and a label sequence conservatively retrieve candidates; ordinary
subsumption remains authoritative.  The final structure was comparatively
small, 1,508,576 bytes, but is required for semantic completeness of the dense
passive mode.

### 5.7 Shared serialized terms

The four compact indexes formerly copied encodings of the same clause terms.
They now share one clause-coalescing term pool.  The first index request
serializes the clause; later requests receive stable offsets into that same
token sequence.  The pool uses 32-bit tokens, a sparse stable-ID directory,
12.5% growth, and coordinated compaction when stale slices justify the work.

Compaction is delicate because every index stores offsets into the pool.  The
implementation computes an exact old-to-new rebase, rebuilds dependent indexes,
and commits only a consistent new generation.  Large retained-ID orderings and
rebase maps are streamed and file-sorted so a compaction does not temporarily
double multi-megabyte arrays.  On Linux the token arena uses `mremap` where
possible to move page tables rather than copy all bytes.

The final pool held 3,220,436 logical tokens in 16,219,320 bytes and had
completed five compactions, reclaiming 14,793,444 bytes.  It recorded
38,557,344 token reuses across indexes.

This is intra-clause sharing across indexes, not a fully shared term DAG.
Instrumentation at 1,000 givens found 728,510 subterm occurrences but 91,686
unique terms, suggesting substantial cross-clause sharing potential.  A
variable-record canonical DAG was estimated at 1,304,344 logical bytes versus
4,194,304 bytes of then-allocated token capacity.  That difference is an upper
bound: a production hash table, fingerprints, reference management, and slack
would consume part of it.

### 5.8 Packed-fast hints

The accepted hint index combines compact immutable bodies with stable-ID
postings and an exact bounded query cache.  Profiles with at most eight exact
keys can use the 16,384-entry cache; wider profiles follow the unchanged
authoritative intersection path.  A cache entry is 136 bytes, for a
2,228,224-byte table.  At the proof boundary the hit rate was 35.08%, and the
cache avoided 136,373,547 posting candidates.

The hint bank remains mutable in the logically relevant sense.  Rewritten
hints are reindexed, stale references are skipped or rebuilt, degradation and
match-once state are updated only by committed exact matches, and checkpoint
state includes those changes.  The packed hint nodes, references, tables, and
bodies together used about 15.2 MiB at the final 88,494-hint boundary.

### 5.9 Reclamation and high-water control

Compact storage alone did not guarantee low peak RSS.  Several rounds of work
were required to bound temporary or stale state:

- stale unit, rewrite, and backward-demodulation records trigger deterministic
  rebuilds at `compact_index_stale_pct`;
- term slices are reclaimed once the estimated stale payload crosses
  `compact_term_reclaim_kb`;
- back-index rebuilds stream archived IDs in 4,096-ID batches rather than
  materialize an unbounded snapshot;
- retained-ID and rebase sorting spill to a temporary file;
- predecessor arrays are released before successor indexes are allocated;
- packed records are reused during rebuilds;
- sparse direct maps replace general clause hashes where the key is a clause
  number;
- dense passives, packed hints, and compact search indexes are released before
  final proof-DAG expansion;
- `P9_COMPACT_HEAP=1` configures the process heap policy before any Prover9
  allocation.

These changes are why a terminal component sum is not enough: peak memory can
occur during rebuild or proof expansion unless predecessor and successor
lifetimes are explicitly ordered.

## 6. Correctness and search-equivalence evidence

The implementation was developed as a sequence of differential oracles, not by
waiting for one long proof.

### 6.1 Component tests

Focused tests cover archive format/corruption, allocator lifecycle, dense
selector compaction, term-pool rebasing, stable-ID maps, compact rewrite,
compact unit retrieval, backward-demodulation retrieval, nonunit feature
retrieval, hint postings and previews, compressed-unit matching, and native
paramodulation/hyperresolution continuations.

The main aggregate commands are:

```sh
make all -j4
make test1
make compact-frontier-tests
make discount-tests
```

### 6.2 Exact prefix oracle

At 300 givens, the final compact-file build emits 132,567 candidate/hint/kept/
given events.  The stream is byte-identical to the accepted full-body
`packed_fast` OTTER reference:

```text
SHA-256 bc38f369ef02271d9b8a00db0cd01a6ba8f890e9608e0abd947de68f763322aa
Given=301 Generated=120793 Kept=5737
Usable=285 SOS=4298 Demodulators=3282 Disabled=1183
```

A fresh 1,000-given A/B comparison also has identical terminal state:

| Representation | User | Wall | Peak RSS | Terminal state |
| --- | ---: | ---: | ---: | --- |
| Compact indexes, ordinary resident bodies | 76.90 s | 92.58 s | 90,404 KiB | reference |
| Compact indexes, file archive, zero cache | 81.35 s | 97.27 s | 90,404 KiB | exact |

Both end at `Given=1001`, `Generated=1,268,285`, `Kept=33,909`,
`Usable=949`, `SOS=26,052`, `Demodulators=21,741`, and `Disabled=6,937`, with
the same hint state and compact-index counters.  The file representation costs
only 1.058 times the user CPU of the full-body compact-index control.

### 6.3 Checkpoint/resume

A current full-hint file-backed run checkpointed at given 100 and resumed to
given 301.  Concatenating the pre-checkpoint and resumed event streams produces
the same `bc38f369...322aa` hash as an uninterrupted run.  All populations,
including `Disabled=1183`, agree, and 22 integrity checks pass.

The audit found one reporting defect: format 3 intentionally omitted dead,
pre-elimination disabled scratch clauses with ID 0, but also lost their count.
The format now records `disabled_checkpoint_omitted` as metadata without
serializing any dead clause body.  This preserves statistics without
reintroducing the memory leak.

### 6.4 Full proof

The accepted current run terminates with:

```text
Given=2945 Generated=8248032 Kept=272787 proofs=1
Usable=1800 SOS=131001 Demodulators=109987 Limbo=300 Disabled=139715
Hints=88494 Active_Hints=4122
```

`prooftrans parents_only` emits 7,051 proof clauses and 3,231 new hints.  The
normalized proof has SHA-256
`9d7c9a12894c1c11ede6aeae08d1cec658ccee66a47fb9663859347a5413fd07`.
It is byte-identical to the accepted full-body `packed_fast` reference.

The older FPA run generates and keeps two additional clauses classified as
`other`, which shifts later IDs and some independent proof-line ordering.  It
nevertheless reaches the same given boundary, SOS and demodulator populations,
proof length, number of new hints, and theorem.  Therefore the strongest exact
representation claim is made against the full-body packed-fast control; the
old-P9 comparison is a proof-boundary and proof-validity comparison, not a raw
byte-for-byte trace claim.

## 7. Evaluation

### 7.1 Accepted proof result

| Measure | Old P9 OTTER/FPA | Accepted compact OTTER | Change |
| --- | ---: | ---: | ---: |
| Result | proof | proof | same theorem |
| Given | 2,945 | 2,945 | same boundary |
| Generated | 8,248,034 | 8,248,032 | -2 `other` clauses |
| Kept | 272,789 | 272,787 | -2 `other` clauses |
| Proof clauses | 7,051 | 7,051 | same length |
| New proof hints | 3,231 | 3,231 | same |
| User CPU | 765.69 s | 754.78 s | 1.42% less |
| Wall time | 14:16.20 | 14:19.48 | essentially equal |
| Peak RSS | 550,400 KiB | 127,420 KiB | 76.85% less |
| Size ratio | 4.32 | 1.00 | 4.32x smaller |

The accepted run passes the 128,000-KiB hard gate by 580 KiB.  That is a narrow
margin and should be remeasured after changing libc, compiler, or machine.  It
does not pass the optional 115-MiB stretch target.

### 7.2 Resident-memory accounting

At the frozen terminal report, resident components were:

| Component | Bytes | Function |
| --- | ---: | --- |
| Compact rewrite bank | 11,205,308 | demodulation and rewrite occurrences |
| Compact unit index | 13,166,960 | unit subsumption/conflict/unification |
| Compact backward-demodulation index | 12,109,376 | redex candidates |
| Compact nonunit index | 1,508,576 | subsumption candidates |
| Shared term pool | 16,219,320 | one serialization per indexed clause |
| Dense passive records | 9,532,864 | SOS selection/status |
| Dense selector heap | 611,368 | given-clause queues |
| Hint nodes | 65,576 | packed hint structure |
| Hint references | 2,658,640 | hint postings |
| Hint tables/cache | 10,355,712 | tables and exact query cache |
| Packed hint bodies | 2,873,239 | immutable hint clauses |
| Ancestor handles | 4,198,608 | file positions/proof IDs |
| Clause-ID structures | 2,316,400 | stable lookup |
| Prover9 allocator reservation | 24,123,712 | retained slabs/ordinary objects |
| Nonanonymous process pages | 1,025,024 | code/libraries and other pages |
| **Accounted total** | **111,970,683** | **106.78 MiB** |

Terminal PSS was 112,809 KiB (110.17 MiB), so the component model explains
96.93% of it.  Sampled RSS peaked at 123,084 KiB, while `/usr/bin/time -v`
recorded the authoritative process high-water mark of 127,420 KiB.

The 84,404,096-byte ancestor file is reported separately because its logical
length is disk backing, not process-resident memory.  Reclaimable kernel page
cache is outside this PSS accounting.

### 7.3 Structural index reduction

At 1,000 givens, the four first-generation compact indexes occupied 44,545,464
bytes.  Radix paths, shared terms, delta-coded occurrences and postings, dense
stable-ID maps, packed records, and tighter growth reduced the corresponding
five-structure total to 13,239,168 bytes, a 70.28% reduction.  The exact search
state remained unchanged.  Peak RSS fell to 90,712 KiB at that gate.

This is a within-new-design reduction, not the old-P9 total-RAM result.  It is
included because it shows why “serialize clause bodies” was insufficient: at
1,000 givens the first compact rewrite/unit/back indexes already occupied about
43 MB while the clause archive itself was only about 8 MB.

### 7.4 The newly supplied 11,000-given pair

The files [`../bob/chat_test.new.out1.gz`](../bob/chat_test.new.out1.gz) and
[`../bob/chat_test.new.out2.gz`](../bob/chat_test.new.out2.gz) use a different,
larger input profile from the final 88,494-hint acceptance problem.  Both files
contain 18,306 hints and both prove after roughly 11,000 givens.

The compressed output SHA-256 values are
`fd7a07f0a8ec1feafacb2672b664a393368ce728d0918c5e2708c7919d4a4406`
for `out1` and
`049a44cb9871fb28c3cf829011fb102dc390d5101ed28677dd7ced15ac5d64f2`
for `out2`.

`out1` is the ordinary OTTER/FPA/full representation.  `out2` explicitly uses
OTTER, dense passives, `packed_fast`, all four authoritative compact indexes,
and an mmap ancestor store.  It predates the final candidate-filter, bounded-
rebuild, file-archive, cache-sizing, transient-release, and compact-heap work.

| Measure | Ordinary `out1` | Early compact `out2` | Ratio/change |
| --- | ---: | ---: | ---: |
| Given | 11,368 | 11,369 | +1 |
| Generated | 253,338,893 | 253,302,129 | -36,764 |
| Kept | 2,216,836 | 2,207,014 | -9,822 |
| SOS | 1,454,686 | 1,520,775 | +4.5% |
| Demodulators | 1,297,818 | 1,362,847 | +5.0% |
| Disabled | 739,073 | 662,309 | -10.4% |
| Internal `Megabytes` | 3,354.28 | 355.86 | 89.39% lower |
| Terminal `RSS_kb peak` | 3,603,800 KiB | 1,492,656 KiB | 58.58% lower; 2.414x smaller |
| User CPU | 5,110.38 s | 15,144.10 s | 2.963x |
| Wall | 5,131 s | 15,178 s | 2.958x |

This is valuable negative evidence.  It proves that the compact
representation was capable of a radical reduction in Prover9-accounted memory
at a much larger boundary, but the version was not usable because it almost
tripled CPU.  Its detailed counters identify why:

- 19.190 billion unit-conflict exact tests;
- 24.503 billion backward-demodulation posting groups examined;
- 21.342 billion symbol occurrences examined;
- 494.44 million shallow path checks;
- about 6.10 billion demodulation attempts in both runs.

The proof trajectories are close but not identical, as the one-given and
population differences show.  These files contain no external
`/usr/bin/time -v` result, but their terminal `Allocator_slabs` lines do contain
a process peak-RSS diagnostic.  It reduces from 3,603,800 to 1,492,656 KiB,
or 58.58%.  This is the appropriate process-RSS comparison for the pair.
Because `out2` uses mmap, its 355.86-MiB internal counter omits resident mapped
pages and must be reported only as an **internal-accounting reduction**.  Even
the process RSS does not charge unmapped file-cache pages; a cgroup total is
still needed for whole-job memory.  Both figures describe an obsolete
intermediate build, not the speed or memory of the accepted code.

The final implementation addresses the mechanisms exposed here: shallow-path
selectivity, grouped/delta-coded occurrence streams, bounded 4,096-ID archive
snapshots during rebuild, denser stable-ID maps, coordinated term compaction,
and explicit file backing.  A current-binary rerun of this exact 11,000-given
input remains desirable; the accepted 2,945-given proof cannot substitute for
that experiment.

### 7.5 How much memory should be expected on the largest AIM runs?

There are now two answers, because the two search architectures have different
growth laws.

For trajectory-sensitive problems using accepted compact OTTER, the measured
whole-process result is 76.85% less peak RSS on the current full proof.  The
large fixed hint bank and small 131,001-clause final SOS make a literal 80%
target difficult on this particular problem.  The obsolete 11,000-given run
suggests that savings can grow when millions of passive/index records dominate,
and its embedded diagnostic measures 58.58% less peak RSS.  It must not be
extrapolated quantitatively because it used an obsolete build, a different
input profile, and no cgroup accounting for filesystem cache.

For DISCOUNT with a bounded collective frontier, the intended asymptotic model
is stronger:

```text
ordinary OTTER RAM
    = fixed hints
    + O(all retained passive bodies and eager indexes)
    + proof history

DISCOUNT/collective RAM
    = packed hints
    + O(active clauses and active indexes)
    + O(history/descriptors)
    + O(configured candidate window)
    + file-backed proof/passive bodies
```

At the archived Osborn and AAPERM boundaries, replacing 4.59 or 8.50 million
resident SOS clauses by active/history state plus a bounded cache makes an
80--95% whole-process reduction plausible.  The 90% midpoint is a capacity-
planning hypothesis, not an accepted measurement.  Search divergence and
rewrite-repair work remain the main risk.

The defensible deployment statements today are therefore:

- **Accepted and measured:** 76.85% less peak RSS, same proof boundary, no CPU
  penalty on the current `chat_test`/Osborn proof.
- **Strong intermediate indication:** at 11,000 givens, 58.58% less
  self-reported peak RSS and 89.39% less internally accounted memory, but from
  an obsolete 2.963x-slower mmap build without cgroup page-cache accounting.
- **Conditional forecast:** 80--95% less total RAM on much larger
  passive-dominated AIM runs, to be established with current-binary RSS/PSS
  and cgroup page-cache measurements, plus proof/coverage comparisons.

## 8. What worked, what failed, and why

| Idea | Outcome | Reason |
| --- | --- | --- |
| Delete rejected generated clauses | No radical gain | Most tautologies and forward-subsumed clauses were already transient. |
| Delete disabled clauses | Useful but insufficient | 4.5--30% measured; proof parents still require a representation; live SOS remains. |
| Compress disabled bodies | Kept | Exact, useful foundation for proof history, but not the SOS growth law. |
| Reclaimable allocator and compact bookkeeping | Kept | Reduced retained slabs and per-clause overhead; necessary for peak control. |
| Ancestor mmap | Rejected as final default | Statistics and scans faulted cold pages; a 13.1-GB mapping had 10.2 GB resident. |
| Explicit file archive | Kept | Logical archive stays on disk; one reusable I/O buffer; predictable residency. |
| Strict DISCOUNT active/passive split | Kept as experimental/product alternative | Best asymptotic bound, but changes demodulator availability and Osborn trajectory. |
| Collective inference descriptors | Kept as experimental alternative | Bounds generated-conclusion residency and is checkpointable; hint count alone did not recover proof. |
| Eager-interreduced DISCOUNT | Not default | Strong contraction but costly rewrite debt and different historical hint normal forms. |
| Packed hints, first version | Replaced | Saved RAM but broad postings caused severe CPU regression. |
| Packed-fast hints | Kept | Exact traces, density-adaptive filtering and bounded cache restore good prefix speed. |
| Compact OTTER bodies only | Insufficient | Compact indexes, not bodies, dominated by 1,000 givens. |
| First authoritative compact indexes | Correct but too slow | Billions of unit/back-demod candidates in the 11k run. |
| Radix paths and shallow-path filters | Kept | Smaller tries and fewer false candidates without changing exact answers. |
| Shared term pool | Kept | Removes duplicate encodings across the four indexes. |
| Delta-coded posting/occurrence streams | Kept | Cuts record overhead and improves locality. |
| Large decoded passive cache | Rejected | 0/4/32-MiB experiment saved decodes but did not save CPU. |
| 8K final packed-fast cache | Rejected | Saved only 596 KiB at full proof and increased CPU/fragmentation. |
| 16K bounded packed-fast cache | Kept | 2.23-MB table, 35.08% hits, avoids 136M candidates within RSS gate. |
| Full-array term/rebase compaction | Replaced | Temporary copies raised the high-water mark. |
| Streamed/file-sorted compaction | Kept | Bounds transient memory while preserving exact rebase order. |
| Release structures before proof replay | Kept | Prevents search-state plus proof-DAG overlap at termination. |

Three broader lessons follow.

First, exact ATP indexes must be judged on candidate volume, not merely bytes
per node.  A tenfold smaller index that causes a thousandfold larger exact-test
set is a loss.

Second, contraction scheduling is part of the heuristic meaning of an algebraic
search.  Fairness or refutational completeness does not imply that a historical
hint-guided proof remains reachable under the same resource limit.

Third, resident memory is a lifetime property.  File mappings, allocator
arenas, predecessor/successor rebuild overlap, and terminal proof expansion can
dominate even when every steady-state record is compact.

## 9. How to build and run the accepted mode

### 9.1 Build and verify

From the repository root:

```sh
make all -j4
make test1
make compact-frontier-tests
make discount-tests
```

For a comparison intended for publication, record:

```sh
git rev-parse HEAD
sha256sum bin/prover9 your-input.in
ldd bin/prover9
uname -a
```

The accepted binary had SHA-256
`78b5ef4b2e53fc5ae2ab81cc46e6455213128e8ac083d6fb11259ad7b1339a6b`.
The accepted `/project/bob/chat_test.in` had SHA-256
`9781ee07691bc62e01f67534208620ca0be3f026f55248a227e9161f1ed17e6c`.

### 9.2 Input block for proof-compatible compact OTTER

Place this block **after** automatic settings and after older experimental
assignments, so these are the final values:

```text
assign(search_loop,otter).
assign(passive_store,dense).
assign(hint_index,packed_fast).
assign(inference_frontier,clauses).
assign(ancestor_store,file).
assign(sos_limit,-1).

set(process_initial_sos).
set(back_demod).
set(back_demod_hints).
clear(unit_deletion).
clear(ancestor_subsume).
clear(eval_rewrite).
set(compact_otter_demodulation).
set(compact_otter_unit_index).
set(compact_otter_back_demod_index).
set(compact_otter_nonunit_index).

assign(compact_passive_cache,0).
assign(compact_index_stale_pct,10).
assign(compact_term_reclaim_kb,2048).
```

The measured heap policy is a process-start environment setting:

```sh
P9_COMPACT_HEAP=1 /usr/bin/time -v bin/prover9 \
  -f your-compact-input.in \
  > your-compact-output.out \
  2> your-compact-output.time
```

Do not use `ancestor_store=mmap` for the final comparison.  Do not enable only
some of the four `compact_otter_*` indexes: dense OTTER refuses such a mixed
configuration because it could omit passive clauses from an operation.

Current restrictions checked at startup include:

- `sos_limit` must be `-1`;
- `process_initial_sos` must remain set;
- `inference_frontier` must be `clauses`;
- `unit_deletion` and `ancestor_subsume` are not supported by the authoritative
  compact unit/nonunit indexes;
- `back_demod` must remain set for the compact backward-demodulation index;
- `eval_rewrite` is incompatible with the compact demodulator bank.

### 9.3 Old-P9 control

The raw compatibility control uses ordinary bodies and FPA hints:

```text
assign(search_loop,otter).
assign(passive_store,full).
assign(hint_index,fpa).
assign(inference_frontier,clauses).
assign(ancestor_store,off).
assign(sos_limit,-1).
set(process_initial_sos).
set(back_demod).
set(back_demod_hints).
clear(unit_deletion).
clear(ancestor_subsume).
clear(eval_rewrite).
```

Use the same inference rules, order, actions, hint file, selection rules, and
resource limits in both cases.  Hash the actual binary and complete input for
each run.  If an automatic mode rewrites options, inspect the echoed options at
the beginning of the output.

### 9.4 Safe progression to a full run

On a low-memory or slow machine, begin with hundreds of givens and explicit
internal and external limits.  Run independent small cases in parallel only up
to the number of real cores and within a known aggregate RSS budget.

Suggested progression:

1. 100 or 300 givens with a deterministic 10% hint sample;
2. 300 givens with all hints;
3. 1,000 givens with all hints;
4. a time-bounded 2,000--4,000-given run;
5. the uncapped proof or production run on a suitable host.

Use both internal guards and an external supervisor, for example:

```text
assign(max_given,1000).
assign(max_seconds,900).
assign(max_megs,2048).
assign(report,60).
set(clocks).
assign(stats,all).
```

```sh
timeout --signal=TERM --kill-after=10 1000 \
  env P9_COMPACT_HEAP=1 \
  /usr/bin/time -v bin/prover9 -f bounded.in \
  > bounded.out 2> bounded.time
```

For a random or deterministic hint sample, keep the exact sampled hint file and
its hash.  A sample is suitable for debugging and A/B prefix comparisons; it is
not a substitute for the full-hint proof experiment.

### 9.5 Disk backing and `/proc`

The current source hard-codes `/tmp` in both archive constructors.  It does
**not** consult `TMPDIR`.  It creates either
`/tmp/prover9-ancestors-XXXXXX` or `/tmp/prover9-passive-XXXXXX` and unlinks the
name immediately.  The open file therefore appears as `(deleted)` through the
process file descriptors and is removed when the process exits.

Find it with:

```sh
for fd in /proc/PID/fd/*; do
  printf '%s -> %s\n' "$fd" "$(readlink "$fd")"
done
```

Then distinguish logical size, allocated disk blocks, and resident pages:

```sh
stat -Lc 'logical=%s bytes, blocks=%b, block-size=%B' /proc/PID/fd/FD
du -hL /proc/PID/fd/FD
awk '/prover9-(passive|ancestors).*deleted/ {show=1} \
     show && /^(Size|Rss|Pss|Private_Clean|Private_Dirty|Swap):/ {print}' \
    /proc/PID/smaps
```

Because the location is currently fixed, `/tmp` itself must be on a local
filesystem with enough free disk space.  On the previously examined host it
was part of the 937-GB root filesystem, not `/dev/shm`.  Changing `TMPDIR`
would not move it.  Making the directory configurable is listed as future
work.

### 9.6 Interpreting a comparison

For each run report at least:

- proof/no-proof and stopping reason;
- given, generated, kept, usable, SOS, demodulator, disabled, hint, and active-
  hint counts;
- proof length and `prooftrans parents_only` result;
- user, system, and wall time;
- external maximum RSS;
- sampled RSS/PSS near a common given boundary;
- cgroup `memory.peak` and `memory.stat` file-cache fields when claiming total
  job RAM;
- archive logical bytes separately from RAM;
- compact-index current/peak bytes, exact tests, rebuilds, and validation
  failures.

Do not compare only the last `matched=` field.  It is a snapshot over currently
active hints in some reports, not a cumulative proof-progress measure.  Do not
compare only `Generated`, which includes transient rejected clauses.  Do not
compare runs at different given boundaries as if they were equal-work memory
measurements.

## 10. Future work

The accepted result closes the current proof gate, but not the research program.
The following order separates measurements needed now from more invasive
designs.

### 10.1 Re-run the large supplied profile with the accepted binary

The highest-priority experiment is a current-binary replay of the exact
18,306-hint input that produced `chat_test.new.out1/out2`.  Use the accepted
file archive and heap policy, external RSS/PSS sampling, and a generous but
finite resource envelope.  This directly tests whether the fixes after the
2.963x intermediate run control candidate amplification at 11,000 givens.

The comparison should include three columns if resources allow:

1. ordinary OTTER/FPA/full bodies;
2. full bodies with the final compact indexes, isolating index CPU;
3. full accepted file-backed compact OTTER.

This separates compact-index retrieval cost from archive I/O and body decoding.

### 10.2 Validate genuinely large AIM runs

Run current compact OTTER and the DISCOUNT/collective alternative on at least
three passive-dominated problems, including Osborn and AAPERM, with identical
input hashes and external limits.  Record equal-given prefixes as well as
equal-resource solved coverage.  A radical acceptance claim for those workloads
should require at least 80% lower external peak RSS, corresponding cgroup
whole-job/page-cache measurements, and component accounting for at least 95%
of PSS.

### 10.3 Reduce exact-test amplification further

The 11,000-given intermediate output shows that unit conflict and backward
demodulation can dominate even when records are small.  The next index work
should be justified by distributions of candidates per query, not by average
node size.  Promising directions are:

- deeper *selective* path features chosen from measured rarity, while keeping
  the ordinary exact test authoritative;
- substitution-tree or code-tree style retrieval for the specific one-way
  matching/unification operation, compared against the current radix
  discrimination representation;
- block-compressed posting intersections with skip data, so a broad feature
  need not be decoded entry by entry;
- separate policies for unit conflict, generalization, and unification instead
  of one structure optimized for their average;
- direct exact matching over serialized terms to avoid constructing an
  ordinary term graph for obvious rejections.

Any prototype must pass the 300-event oracle and be evaluated against the
current final index, not the first packed implementation.

### 10.4 True cross-clause term sharing

The measured 87.4% duplicate subterm occurrences at 1,000 givens justify a
prototype canonical term DAG (hash-consing).  A sound engineering design needs:

- immutable nodes keyed by symbol and child term IDs;
- compact atom/literal root vectors per clause;
- a build-time hash table that can be discarded or resized after interning;
- reference/liveness accounting compatible with stale-index compaction;
- direct matching, unification, ordering, and rewriting over term IDs;
- proof/archive serialization independent of process-local term IDs;
- a peak-memory analysis including the old and new DAG during compaction.

Sharing only pays if the live canonicalization table and garbage collection cost
less than the duplicated token sequences.  The instrumentation provides an
upper bound, not that proof.

### 10.5 File-backed cold posting tiers

Compact OTTER still keeps exact candidate indexes for every passive clause in
RAM.  For multi-million SOS populations, the next asymptotic step is a two-tier
index:

- a small in-memory directory and hot tail;
- immutable, compressed posting blocks on disk;
- merge/compaction in large sequential batches;
- skip metadata for intersections;
- stable clause IDs and ordinary exact checks.

This resembles an external-memory inverted index more than a C pointer tree.
It preserves eager OTTER visibility while allowing cold postings, not just cold
clause bodies, to leave RAM.  The difficult part is backward demodulation,
whose access pattern can be broad; batching new demodulators and scanning
compressed blocks sequentially may be better than millions of small reads.

### 10.6 Rewrite directly in the compact representation

Backward demodulation currently may reconstruct, rewrite, reprocess, and
rearchive a cold clause.  A direct compact rewrite pipeline could decode a
serialized term stream into a bounded workspace, apply compact rewrite rules,
and emit a new archived record without allocating a full `Topform` forest.
This should target the `back_demod` and preprocessing clocks and report both
CPU and transient RSS.  It must reproduce justifications, attributes, hint
matching, orientation, and all action-rule side effects exactly.

### 10.7 Improve collective search guidance, not just hint counts

The collective scheduler should be judged by proof-relevant ancestor progress,
not the number of distinct hints matched.  Possible signals include:

- closure over parent relationships among hints extracted from previous
  proofs;
- distance to unmatched successor hints in a proof DAG;
- rule/direction quotas learned from the source proofs;
- separate priority for conclusions that preserve the expected normal form;
- bounded replay of the old given/inference order as a diagnostic oracle.

Fair FIFO service must remain present.  A heuristic lane may reorder finite
work, but must not starve an enabled inference rule or silently consume a
previewed conclusion without exact committed processing.

### 10.8 A two-stage production workflow

Where identical trajectory is not required, a practical portfolio can run many
low-memory DISCOUNT/collective searches, extract proofs or newly discovered
hints from successful jobs, and replay only selected candidates with compact
OTTER for exact historical-style processing.  This realizes the old
`Plans-RAM-24.txt` idea of running many cheap searches and a smaller number of
proof-producing replays without throwing proof parents away unsafely.

This workflow complements rather than replaces compact OTTER.  It should be
evaluated by total solved problems per machine-day and verified proofs per GiB,
not by the success of one trajectory.

### 10.9 Operational hardening

Smaller but important product tasks are:

- honor `TMPDIR` or add an explicit `archive_directory` option;
- fail early on insufficient disk space and report the backing filesystem;
- expose archive FD/path information before unlinking when diagnostics are
  requested;
- use cgroup v2 counters in the benchmark harness;
- record binary/input hashes and `/usr/bin/time` output automatically;
- test the 128,000-KiB gate on more libc/kernel combinations;
- document unsupported option combinations in the public Prover9 manual.

These do not create the radical saving, but they make a week-long run
reproducible and prevent disk-backed memory from being misreported.

## 11. Reproducibility map

The main source files are:

- [`provers.src/search.c`](provers.src/search.c): loop integration, eager
  transactions, archive ownership, checkpointing, statistics, and lifecycle;
- [`provers.src/giv_select.c`](provers.src/giv_select.c): dense SOS selector;
- [`provers.src/cold_passive_store.c`](provers.src/cold_passive_store.c):
  serialized passive file/mmap store used by DISCOUNT;
- [`ladr/clause_store.c`](ladr/clause_store.c): disabled/proof ancestor archive
  and compact-OTTER body owner;
- [`ladr/hints.c`](ladr/hints.c) and
  [`ladr/hint_postings.c`](ladr/hint_postings.c): packed exact hints;
- [`provers.src/compact_rewrite.c`](provers.src/compact_rewrite.c): compact
  demodulator bank;
- [`provers.src/compact_unit_index.c`](provers.src/compact_unit_index.c): unit
  retrieval and conflict;
- [`provers.src/compact_back_demod.c`](provers.src/compact_back_demod.c): redex
  occurrence retrieval;
- [`provers.src/compact_feature_index.c`](provers.src/compact_feature_index.c):
  nonunit subsumption candidates;
- [`provers.src/compact_term_pool.c`](provers.src/compact_term_pool.c): shared
  token arena and coordinated rebasing;
- [`provers.src/compact_id_map.c`](provers.src/compact_id_map.c): sparse/dense
  stable-ID maps.

The authoritative engineering records are:

- [`P9-PHASE5-ACCEPTANCE-AUDIT.md`](P9-PHASE5-ACCEPTANCE-AUDIT.md);
- [`P9-PHASE5-COMPACT-FRONTIER-PLAN.md`](P9-PHASE5-COMPACT-FRONTIER-PLAN.md);
- [`P9-CHAT-TEST-REPORT.md`](P9-CHAT-TEST-REPORT.md);
- [`P9-RADICAL-RAM-REPORT.md`](P9-RADICAL-RAM-REPORT.md);
- [`P9-DISCOUNT-WALDMEISTER-PLAN.md`](P9-DISCOUNT-WALDMEISTER-PLAN.md);
- [`P9-COLLECTIVE-SCHEDULER-PLAN.md`](P9-COLLECTIVE-SCHEDULER-PLAN.md);
- [`P9-COLLECTIVE-DEMODULATION-PLAN.md`](P9-COLLECTIVE-DEMODULATION-PLAN.md);
- [`P9-BETTER-PACKED-PLAN.md`](P9-BETTER-PACKED-PLAN.md);
- [`Checkpoint-Format-Spec.txt`](Checkpoint-Format-Spec.txt).

The current full-proof artifacts are in
`/project/phase5-results/phase5-completion-audit/final-proof-current`.  The
current 300/1,000 and checkpoint audit artifacts are in the parent
`phase5-completion-audit` directory.  Generated artifacts are not substitutes
for the committed audit: both are needed to reproduce a claim.

The work from the frozen memory baseline to the final audit comprises 192
small commits.  Milestones include exact ancestor storage, the reclaimable
allocator, DISCOUNT/dense passives, collective continuations, packed and
packed-fast hints, the four compact OTTER indexes, shared terms, file backing,
bounded rebuilds, proof-boundary lifetime ordering, and final checkpoint audit.
The detailed commit messages are part of the implementation record.

## 12. Conclusion

The original question was how to reduce Prover9 RAM by 80--90%, not by another
small constant factor.  The experiments show why local deletion could not meet
that goal.  Millions of SOS clauses were not merely proof debris; under the
OTTER loop they remained logically active for contraction and therefore
appeared in multiple term and clause indexes.  Hints and proof ancestors added
large fixed and historical terms.  Any radical design had to change all of
these representations together.

The strict DISCOUNT/collective architecture gives the cleanest asymptotic
answer and remains promising for very large passive-dominated exploration.  It
also demonstrated that a theorem prover's practical behavior is not determined
by the set of inference rules alone: the timing of demodulation, passive
normalization, hint matching, and rule interleaving can decide whether a known
proof is found.

The accepted compact-OTTER design resolves that tension for the current
trajectory-sensitive problem.  It keeps the eager OTTER transaction and hint
semantics while replacing pointer identity by stable clause numbers, resident
passive bodies by an exact file archive, and pointer-heavy indexes by compact
authoritative filters.  The result is a complete validated proof at the same
2,945-given boundary, 4.32 times smaller in peak RSS, with no CPU penalty.

The literal 80--90% whole-process target is not yet a universal measured claim:
the accepted proof saves 76.85%; the obsolete slow mmap build saved 58.58% by
its terminal peak-RSS diagnostic, while its allocator-only counter fell
89.39%.  The next decisive evidence must come from current-binary 11,000-given
and multi-million-SOS AIM runs with cgroup memory accounting.  The architecture
is now in a form where those runs can distinguish remaining index/search
questions from the old, already solved problem of retaining every clause as a
live C object.

## References

1. L. Bachmair and H. Ganzinger, “Resolution Theorem Proving,” in J. A.
   Robinson and A. Voronkov (eds.), *Handbook of Automated Reasoning*, vol. 1,
   Elsevier/MIT Press, 2001.
2. J. Harrison, *Handbook of Practical Logic and Automated Reasoning*,
   Cambridge University Press, 2009, doi:10.1017/CBO9780511576430.
3. T. Hillenbrand, “Citius altius fortius: Lessons learned from the Theorem
   Prover Waldmeister,” *Electronic Notes in Theoretical Computer Science*,
   2003/2004, doi:10.1016/S1571-0661(04)80649-2.  A local copy used for this
   work is [`../Citius_altius_fortius_Lessons_learned_from_the_The.pdf`](../Citius_altius_fortius_Lessons_learned_from_the_The.pdf).
4. T. Hillenbrand and B. Löchner, “The Next Waldmeister Loop,” in A. Voronkov
   (ed.), *Proceedings of CADE-18*, LNAI, pp. 486--500, Springer, 2002.
5. W. McCune, *OTTER 3.3 Reference Manual*, Technical Memorandum
   ANL/MCS-TM-263, Argonne National Laboratory, 2003.
6. W. McCune, “Prover9 and Mace4,” Prover9 distribution and manual, 2005--2009;
   current terminology is also documented by the LADR-2026 Prover9 manual.
7. R. Nieuwenhuis and A. Rubio, “Paramodulation-Based Theorem Proving,” in
   J. A. Robinson and A. Voronkov (eds.), *Handbook of Automated Reasoning*,
   vol. 1, Elsevier/MIT Press, 2001.
8. A. Riazanov and A. Voronkov, “Limited Resource Strategy in Resolution
   Theorem Proving,” *Journal of Symbolic Computation* 36(1--2):101--115, 2003.
9. S. Schulz, “E -- A Brainiac Theorem Prover,” *AI Communications*
   15(2--3):111--126, 2002.
10. R. Veroff, “Using Hints to Increase the Effectiveness of an Automated
    Reasoning Program: Case Studies,” *Journal of Automated Reasoning*
    16(3):223--239, 1996.
11. R. Veroff, “Solving Open Questions and Other Challenge Problems Using
    Proof Sketches,” *Journal of Automated Reasoning* 27(2):157--174, 2001.
12. L. Wos, “The Resonance Strategy,” *Computers & Mathematics with
    Applications* 29(2):133--178, 1995.
