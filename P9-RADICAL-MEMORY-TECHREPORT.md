# Reducing Memory in Hint-Guided Prover9 Saturation

## From passive-clause explosion to a compact OTTER-compatible given-clause loop

**Technical report, 11 August 2026**

**Implementation branch:** `phase5-compact-frontier`

**Accepted code:** `2483a7a49e4e21c54b1692c142b55435eeefe44b` plus audit
commit `ac3f0fe`

**Status:** all required Phase-5 correctness, proof, CPU, memory, checkpoint,
and accounting checks passed on the current `chat_test`/Osborn problem

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
as many separately allocated clause, literal, term, and index objects connected
by C pointers.  They also remained in the indexes used for immediate
simplification.  Thus one passive clause could occupy memory both as a clause
and through several index entries.

This report describes two responses.  The first is a DISCOUNT-style loop with
a strict active/passive separation.  A passive clause is represented by a small
fixed-size selection record, while its full contents are stored in compressed
form.  Hints are stored similarly.  Work that would generate many
paramodulation or hyperresolution conclusions is represented by a small record
that says how to resume that work; only a bounded number of conclusions are
kept as ordinary clauses at once.  This Waldmeister-inspired design gives the
best bound on how RAM grows as the search grows.  It also changes the time at
which simplification and generating inferences take place.  On the supplied
Osborn hint chains, matching more hints did not reproduce the historical proof:
demodulation and inference timing are part of the effective search strategy.

The accepted response is therefore an eager, compact OTTER-compatible loop.
It preserves the old treatment of every retained clause for demodulation, unit
conflict, subsumption, backward demodulation, hint matching, and given-clause
selection.  It replaces the full in-memory representation of an archived passive
clause by a record in an append-only disk file and a small selection record in
RAM.  Index entries contain clause numbers rather than pointers.  Four smaller
indexes cover rewriting, unit-clause operations, backward demodulation, and
nonunit subsumption.  An ordinary Prover9 clause is reconstructed only when an
exact logical test needs it or when it is selected as the given clause.  The
new indexes return possible answers; the established Prover9 matcher,
unifier, rewriter, or subsumption test still makes every final decision.

On the accepted full proof, old P9 used 550,400 KiB peak RSS and 765.69 seconds
of user CPU.  Compact OTTER proved at the same 2,945-given boundary, with the
same 7,051 proof clauses and 3,231 new hints, in 754.78 user seconds and
127,420 KiB peak RSS.  This is a measured 76.85% whole-process memory reduction
(4.32 times smaller) with 1.42% less user CPU.  Memory counters captured at
termination explain 96.93% of the process memory after shared pages are
apportioned among their users.  A larger 11,000-given comparison now includes
ordinary Prover9, the early compact implementation, and the current file-backed
compact implementation.  The current compact run reduced its terminal
peak-RSS diagnostic from 3,603,800 to 834,780 KiB (76.84%, or 4.32 times
smaller), but used 12,201.61 rather than 5,110.38 user seconds (2.388 times the
CPU).  It is 19.43% faster and 44.07% lower in peak RSS than the early compact
run, yet it still performs 19.190 billion unit-conflict exact tests and examines
21.500 billion backward-demodulation posting groups.  Thus the statement that
compact OTTER has no CPU penalty is established only for the 2,945-given
acceptance proof; it does not generalize to this larger search.  The current
run also had allocator compact policy disabled.  A controlled allocator and
cgroup rerun remains necessary for a total-job memory claim, but the CPU and
candidate-count diagnosis is already decisive.

The main conclusion is methodological as well as quantitative.  A radical RAM
reduction could not be obtained by freeing one list or by compressing clause
bodies in isolation.  The new representation had to preserve the order and
outcome of simplification, redundancy tests, hint matching, and given-clause
selection.  At the same time it had to change how every large part of the
search state is stored: passive clauses, proof ancestors, hints, selection
queues, and the indexes for rewriting, unit clauses, and subsumption.  It also
had to limit temporary copies made while dead index entries are removed.

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
- With **hint degradation**, each previous use of the same hint adds a weight
  penalty to its next match.  This prevents one repeatedly matched hint from
  dominating selection forever.

Several implementation terms recur below.  They are defined here so that code
names later in the report can be read in terms of their theorem-proving role.

- **Serialization** means writing a clause as a compact sequence of numbers
  representing symbols, variables, literals, attributes, and justification,
  rather than keeping the usual network of C objects and pointers.
- **Materialization** is the reverse operation: reconstructing an ordinary
  Prover9 clause from that sequence.  The reconstructed clause is normally
  freed after the exact test; it remains live only if it is selected.
- A **stable clause number** is Prover9's ordinary clause ID used in an index
  instead of a memory address.  The number still identifies the clause after
  its body has moved from RAM to the file archive.
- A **dense record** is one small fixed-size entry in an array.  Here it holds
  only the weight, age, hint information, and other fields needed to select a
  given clause; it does not contain literals or terms.
- An **index feature** is a cheap necessary property of a possible answer, for
  example a literal sign, a root symbol, or a symbol at a short term position.
  A **posting list** is simply the list of clause or hint numbers that have one
  such feature.  Intersecting several lists means keeping only numbers present
  in every list, because a possible answer must have every required feature.
- A **conservative filter** never omits a possible answer.  It may return
  extra clauses, called false candidates.  The established Prover9 matcher,
  unifier, subsumption test, or rewrite test rejects those extras and makes the
  final logical decision.  The report sometimes calls that old routine the
  **authoritative** test.
- **FPA indexing** is Prover9's ordinary feature-path indexing of terms.  In
  the relevant implementation it retains live term objects and pointers.  It
  is the principal old representation against which the packed hint index is
  compared.
- A **cache** retains the answer to a recent index query so that the same work
  need not be repeated.  It is bounded here: it has a fixed maximum size and
  never grows with the number of clauses.
- A **memory allocator** is the library code that obtains RAM for Prover9 and
  later reuses or returns freed blocks.  A **priority queue** is a collection
  arranged so that the clause with the next-best selection value can be found
  without scanning every waiting clause.
- An **epoch** is a version number.  A clause tagged with an old simplifier or
  hint epoch must be checked again because the rewrite rules or hint state have
  changed since it was last processed.
- The **inference frontier** is the work waiting between choosing inference
  parents and processing the resulting clauses.  In `clauses` mode it contains
  the resulting clauses themselves; in `collective` mode it mainly contains
  small records saying how to resume groups of inference work.
- A **radix-compressed path** stores a chain of discrimination-tree labels as
  one short sequence instead of as several one-child tree nodes.
- A **token** is one 32-bit number representing a symbol, variable, or boundary
  in a serialized term.  The **shared term pool** stores one token sequence per
  retained clause and lets several compact indexes refer to it.  Equal subterms
  belonging to different clauses are not yet shared.
- **RSS** is the number of a process's pages currently in physical memory;
  peak RSS is its high-water mark.  **PSS** divides the cost of a shared page
  among the processes using it.  Neither is the logical length of a disk file.
- An **mmap** makes a file appear as part of a process's address space.  Its
  pages count in RSS when accessed.  Explicit file reads instead copy only the
  requested record into a small RAM buffer, although the kernel can still keep
  recently read data in its filesystem cache until other memory is needed.
- A **SHA-256 digest** is a 64-hexadecimal-character identifier computed from a
  file or event log.  Equal digests are used here to verify byte-for-byte
  equality; they are not a substitute for checking the logical proof.

The adjective “exact” in this report refers to the answer of the complete
operation, not necessarily to the candidate set returned by an index.  For
example, an index may conservatively return three clauses, after which the
ordinary subsumption test decides that one truly subsumes the query.  Exactness
means that this final answer agrees with the old implementation and no possible
answer was lost.

## 2. The workload and the original memory problem

The motivating searches in the supplied AIM problem collection are far outside
the scale suggested by their given counts.  A few thousand selected clauses can
generate and retain millions of
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
demodulation dominated.  In other words, most time was spent simplifying
clauses and maintaining redundancy indexes, not applying the generating
inference rules.  For such an algebraic search, the number of retained clauses
and the cost of maintaining them are more informative than the raw number of
inference-rule invocations.

### 2.1 What `Plans-RAM-24.txt` corrected

The discussion archived in [`../Plans-RAM-24.txt`](../Plans-RAM-24.txt)
prevents two tempting but incorrect readings of the statistics.

First, `Generated` is not a live-clause count.  In one 1,000-given Osborn run,
about 1.4 million printed tautologies arose while processing generated or
backward-rewritten clauses.  Such immediate tautologies and clauses rejected by
forward subsumption are normally destroyed.  They consume CPU and make
short-lived requests for memory, but their count does not explain terminal RAM.

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
proof refer to parent clause numbers.  If the clause body is simply freed,
those parent references still exist but no longer lead to a clause that can be
printed or checked.  Disabled bodies may therefore be removed from RAM only
after an alternative proof-history store can reconstruct them.

The correct conclusion is therefore:

```text
Rejected generated clauses       mostly transient
Disabled retained clauses        important proof-history cost
Live SOS clauses and indexes      main source of growing memory
Hints                             large fixed and mutable cost
Demodulator/index maintenance     dominant CPU cost on hard algebra
```

### 2.2 Why `Megabytes` is not RSS

Historical Prover9 `Megabytes` mostly counts memory requested through
Prover9's own allocator.  Memory obtained by the C library or by mapping a
file can be absent from that counter even while it occupies physical RAM.
Conversely, a 10-GB file need not occupy 10 GB of RAM merely because its
logical file length is 10 GB.  The report therefore keeps four measurements
separate where available:

1. counters for bytes used by Prover9's own data structures;
2. RSS sampled from Linux `/proc`, meaning pages currently resident for the
   process;
3. PSS from `/proc/<pid>/smaps`, which divides shared pages among their users;
4. the maximum RSS reported by `/usr/bin/time -v`.

This distinction mattered in a long DISCOUNT run.  Its open but unlinked file
had a logical length of 13,419,485,217 bytes and occupied 9.7 GiB on disk.
Nevertheless, 10,223,128 KiB of the mapped file was also in physical memory.
Mapping a file is therefore not a promise that it will stay out of RAM: reading
or scanning a mapped page asks the operating system to bring that page into
memory.  Even a statistics routine can accidentally do this.  The accepted
compact-OTTER configuration instead reads individual archive records into a
small buffer.

The operating system can still keep recently read archive blocks in its
filesystem cache.  It may discard these blocks when other programs need RAM,
and they are not charged to this process's RSS or PSS.  They can, however, be
charged to the Linux control group (cgroup) containing the job.  Thus
127,420 KiB is an exact **process peak-RSS** result, not the maximum amount of
machine RAM used for both the process and cached archive data.  A total-job
measurement should also record the cgroup's `memory.current`, `memory.peak`,
and file-cache fields from `memory.stat`.

## 3. Semantic constraints: why this was not just a compression task

A clause waiting in Prover9's SOS is passive for *generating* inferences, but
under the traditional OTTER loop it is not passive for contraction.  A newly
retained equation can become a demodulator immediately.  It can rewrite old
SOS and usable clauses, participate in backward demodulation of hints, and
change which later clauses survive forward subsumption.  Unit clauses can
participate in unit conflict and unit simplification.  Nonunit clauses must be
visible to subsumption.  Hint matching is performed while each candidate is
processed, before it enters the SOS, and the matched-hint result affects its
weight and the selection queue in which it is placed.

These observations impose four contracts.

### 3.1 Search contract

For a mode intended to reproduce the old sequence of selected clauses,
retaining a clause must trigger the same
sequence of immediate simplification and redundancy operations as ordinary
OTTER.  Selection cycles, clause weights, hint degradation, `match_once`,
matcher limits, and action rules must see the same results in the same order.
Compact storage may change memory addresses and representation; it may not
silently delay the clause's use as a simplifier or thereby turn an OTTER loop
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

Thus an archived passive clause can still be checked against every hint: the check
occurs while the newly derived clause still has its ordinary in-memory form.
It is archived only after simplification, exact hint matching, weighting, and
retention.  No hint behavior is recovered by scanning the passive archive
later.

### 3.3 Proof contract

Every proof parent must remain recoverable by clause number after its ordinary
in-memory body has been freed.  The archive must retain the clause, attributes,
flags, matching-hint information, and justification.  A damaged archive record
must cause an explicit error rather than a malformed proof.  At the end of the
search, proof extraction reconstructs only the ancestors reachable from the
proof; these parent links form the usual directed acyclic proof graph (proof
DAG).

### 3.4 Index contract

Removing a passive clause's literals and terms also invalidates the pointers
to those objects in Prover9's ordinary indexes.  Every operation that must see
passive clauses therefore needs either a smaller clause-number-based index or
the ordinary clause must remain in RAM.  Compact OTTER refuses to start unless
all four required replacement indexes are enabled.  This prevents a seemingly
working partial configuration from silently omitting passive clauses from
rewriting, unit operations, backward demodulation, or subsumption.

## 4. Waldmeister's lesson and the first architectural path

Hillenbrand's account of Waldmeister starts from a standard active/passive
completion loop.  It emphasizes three implementation lessons directly relevant
here.

1. A discrimination tree need not allocate one pointer-heavy object for every
   step of a term path.  Storing several consecutive one-child steps together
   uses less memory and lets the processor examine nearby data together.
2. A strict active/passive separation avoids normalizing the entire passive set
   on every iteration, as an OTTER loop would do.
3. The critical pairs belonging to one newly active equation need not all be
   constructed immediately.  One small record can describe the whole group,
   remember where generation should resume, and store the least weight still
   available.  Only a fixed number of promising pairs need exist as individual
   equations.  In Waldmeister's analysis this changed the growth of passive
   memory from proportional to the square of the search length to proportional
   to the search length itself.

The result reported for Waldmeister is striking: more than 500 million passive
equations and over 70,000 active equations could be represented in about
200 MB.
That result is for unit equational completion, not for Prover9's full mixture of
paramodulation and hyperresolution, and it cannot be imported as a benchmark.
It nevertheless supplied the right architectural question: can the prover
represent *delayed inference work* instead of materializing every conclusion?

### 4.1 DISCOUNT active/passive boundary

The first answer added `assign(search_loop,discount)`.  In this mode only
selected active clauses are in inference and simplification indexes.  A new
candidate is simplified, matched against hints, assigned its selection weight,
and stored passively, but does not become an inference parent or demodulator
until selection.  On selection, Prover9 checks whether the rewrite system has
changed since the clause was stored.  If so, it simplifies the clause again and
either returns the changed clause to the passive set or deletes it with the
appropriate justification.

This is a standard DISCOUNT-style distinction, not a new calculus.  With fair
selection it is a natural saturation architecture.  It does, however, order
contraction differently from Prover9's historical OTTER implementation.

### 4.2 Dense passive storage

With `assign(passive_store,dense)`, a passive clause no longer remains in RAM
as Prover9's ordinary full clause object (called `Topform` in the code), with
separately allocated literals, terms, list entries,
and priority-queue entries.  One small array record instead stores its clause
number, weight, age, semantic classification, matched hint, and the values used
by the given-clause selection rules.  The complete clause and its justification
are stored in compressed form.  The priority queues contain only 32-bit
positions of these small records.  When a queued position names a clause that
has since been deleted, it is ignored; periodically the live records are packed
together so that such dead positions do not accumulate without bound.

There is only one stored copy of the complete clause.  Both proof extraction
and passive-clause reconstruction use the archive copy; the selection machinery
does not retain a second body.

### 4.3 Packed hints

The old FPA hint index kept the complete term trees of hundreds of thousands of
hints in RAM.  With `hint_index=packed`, each hint body is instead a compact
number sequence.  Separate small arrays record whether the hint is active, how
often it has matched, and whether its weight has been degraded.  Lists keyed by
safe structural properties, such as a root symbol or a short term path, return
the hint numbers that might match.  Prover9 reconstructs only those hints and
uses its established matching and subsumption routines for the final answer.

The initial packed implementation saved memory but was too slow.  At one
1,000-given full-hint boundary it used 295,680 KiB rather than old P9's
507,108 KiB, but took 441.39 rather than 113.44 CPU seconds.  Its index lists
were too broad: billions of hint numbers survived the cheap lookup and had to
be checked by the exact matcher.

The successor, `packed_fast`, made five specific changes.

1. When several feature lists must be combined, it scans a short sparse list
   of hint numbers but uses a bitmap—one bit per possible hint number—for dense
   lists.  The choice depends on the list sizes.
2. A query records its complete set of required structural features, rather
   than a coarse hash that can confuse different queries.
3. Results of recent queries are cached in a fixed-size table.  A result is
   reused only while every part of the hint index on which it depends is
   unchanged.
4. A unit hint can be matched directly against its compact encoding, avoiding
   reconstruction of an ordinary term tree for that common case.
5. Prover9's special hint symbol `_AnyConst`, which stands for an arbitrary
   constant during hint matching, is tracked separately so it remains sound
   without forcing every ordinary query to scan all such hints.

At a boundary after 300 selected clauses in the DISCOUNT experiment,
`packed_fast` took 10.23 user seconds.  Ordinary FPA took 27.72 seconds and the
first packed implementation took 50.02 seconds.  All three produced the same
byte-for-byte sequence of hint matches and hint-state changes, so the speedup
did not change observable hint guidance at that boundary.

### 4.4 Collective inference scheduling

The collective inference scheduler delays some of the generating inferences
associated with a selected clause after that clause becomes active.  Instead of
constructing every paramodulant or hyperresolvent at once, it creates one small
**resume record**, called a descriptor in the code, for each of these four
kinds of work:

- paramodulation from the given clause;
- paramodulation into the given clause;
- positive hyperresolution;
- negative hyperresolution.

The resume record contains the parent clause numbers and the exact position at
which the inference-rule iterator—the procedure that enumerates possible
conclusions one at a time—should continue.  It also records which
clauses were active when the work was created, so later activations cannot be
used as if they had already been available.  The balanced scheduler allocates
some turns to every enabled inference rule.  It stops creating new resume
records at an upper limit and resumes only after their number falls below a
lower limit.  Each turn is also allowed to produce only a bounded number of
conclusions.  Because the iterator position is saved, the next turn continues
where the previous one stopped instead of generating and discarding the same
prefix again.

Additional experiments looked ahead at a small, bounded group of conclusions
and asked which of them would match a hint.  This preview was used only for
ordering: it did not assign a clause number, update a hint, change hint weight,
or bypass ordinary clause processing.  If look-ahead processed a conclusion
early, it recorded both that conclusion's sequence number and a checksum of its
structure.  When the normal fair scan later reached the same conclusion, those
two values proved that this was precisely the already processed clause and not
a superficially similar one.  Ordinary first-in, first-out service remained in
the schedule so that every finite item of pending inference work was eventually
processed.

A checkpoint stores the active-clause history, when each clause ceased to be
active, every pending resume record, each iterator's next position, the
checksums used to recognize already processed conclusions, and the scheduler's
current turn.  Resume verifies and restores all of this state.

### 4.5 Why the first path did not prove Osborn

The DISCOUNT/collective path kept only a small bounded number of generated
conclusions as ordinary clauses in RAM and, in
some long runs, matched more distinct hints than the historical proof.  It did
not establish the same theorem within the tested time.  That is not a
contradiction.

A hint match is local evidence that a useful clause *shape* has appeared.  It
does not say that all of the proof clause's ancestors were generated, that the
same demodulators were available when they were needed, or that the clause
survived in the same normal form.  The old OTTER loop allows an unselected SOS
equation to become a demodulator immediately.  Selected DISCOUNT delays that
rule until activation.  Eager-interreduced DISCOUNT also rewrites each
demodulator with the other demodulators.  This stronger mutual simplification
can change or remove clauses expected by a historical hint chain.
Collective expansion changes the interleaving of paramodulation and
hyperresolution conclusions again.

Three DISCOUNT variants were tested.  The `selected` variant waits until a
clause is selected before using it as a demodulator.  The `eager_legacy`
variant makes new demodulators available at the earlier OTTER-like point.  The
`eager_interreduced` variant additionally rewrites every demodulator with the
other demodulators, keeping the rewrite rules mutually simplified.  This
property is called an interreduced rewrite system.  Their behavior showed that
the time at which a demodulator
becomes available dominates this equational workload.

The more eager variants created a second obligation: whenever the rewrite
system changed, previously stored passive clauses might no longer be in normal
form and had to be simplified again.  At one stage this queue of clauses
waiting to be resimplified could consume every scheduler turn, so no generating
inference was performed.  Limits on that queue and mandatory inference turns
removed that nonprogressing behavior, but
still did not reproduce the old sequence of selected clauses and hence did not
recover the old proof.

The lesson is the same one stated in the Waldmeister paper's conclusion: a
complete refinement can change practical search behavior in an unforeseen way.
For the Osborn chain, maximum memory reduction and reproduction of the old
search order are different requirements.

## 5. The accepted architecture: eager compact OTTER

Phase 5 keeps `search_loop=otter` and `inference_frontier=clauses`.  Every
retained clause goes through Prover9's ordinary sequence of immediate
simplification, hint matching, redundancy tests, and index updates.  Only its
long-term representation changes.  Once that processing is complete, an archived
passive clause is written to the archive and the long-lived indexes refer to it
by clause number rather than by pointers to its literals and terms.

The operational flow is:

```text
infer an ordinary candidate clause
    -> demodulate and simplify literals
    -> safe unit-conflict test
    -> exact hint matching and weighting
    -> resource limits, semantic tests, and keep/delete rules
    -> forward subsumption
    -> assign stable clause number
    -> test/admit new demodulator
    -> backward subsumption and backward demodulation
    -> update the four smaller passive-clause indexes
    -> serialize body + justification to the archive
    -> retain only the small selection record and clause numbers in RAM

select a given clause
    -> locate archive record by stable number
    -> reconstruct ordinary clause
    -> remove its entries from the passive-clause indexes
    -> activate it in the ordinary inference set
    -> make ordinary inferences at the same stage as the old OTTER loop
```

### 5.1 One file archive whose records are never overwritten

With `ancestor_store=file`, records are appended to a disk file and are not
modified in place.  Each carries a checksum that detects accidental damage.
The `pread` and `pwrite` system calls transfer just the requested record between
the file and a small RAM buffer.  A record contains the compressed clause body,
attributes, flags, matched-hint state, and justification with its parent clause
numbers.  The same archived body serves both passive-clause processing and
later proof extraction; the passive selector keeps no second collection of
clause bodies.

The logical file length is not process-resident RAM.  In the accepted proof it
was 84,404,096 bytes, while the reusable I/O buffer was 4 KiB.  The process
performed about 3.8 million reads totaling 374 MB and about 412,000 writes
totaling 84.4 MB.  Because the file is not mapped into the process, scanning
the process's address space cannot accidentally bring the entire archive into
its RSS.  The operating system may nevertheless retain recently read blocks in
its filesystem cache, so total-job measurements should include the cgroup
counters described in Section 2.2.

Before reconstructing a clause, the reader checks the archive-format version,
record length, field ranges, fields that must be zero, and checksum.  A failed
check stops the run with an error instead of passing damaged data to a proof or
logical test.

### 5.2 Dense given-clause selection

The small SOS record contains everything that Prover9's given-clause selection
rules inspect, but no literals or terms.  It records the clause's weight, age,
breadth-first level, semantic classification, matched hint, and membership in
the alternating low-weight/high-weight priority queues.  As defined in
Section 1, these queues arrange records so that the next clause can be found
without scanning the full SOS.  Thus the old
selection order can be reproduced without keeping a full clause merely to ask
which clause should be selected next.  In the final proof, 131,001 such records
used 9,532,864 bytes and their priority queues used 611,368 bytes—about 77
bytes per live passive clause in these two components.

The implementation can retain recently reconstructed passive clauses in RAM,
but the accepted configuration disables this cache with
`compact_passive_cache=0`.  In a controlled 1,500-given comparison, 4-MiB and
32-MiB caches avoided reconstructing about 98,000 clauses, yet did not reduce
CPU time and did increase RSS.  The expensive work was examining too many
index candidates, not reading and decoding clause records.

### 5.3 Compact rewrite-rule store

The compact rewrite-rule store holds the encoded left- and right-hand sides of
each rewrite rule, the rule's kind, whether it is still active, and the clause
number needed for a proof.  To find rules whose left side might match a term,
it uses a discrimination-tree-style index.  A run of one-child tree nodes is
stored as one short token sequence; this is the radix compression defined in
Section 1.  For backward demodulation, additional lists map structural term
features to the numbers and term positions of clauses that may contain a
matching subterm (a redex).

The new index does not decide that a rewrite is legal.  It supplies possible
rules to the same matching, term-ordering, and rewriting code used by ordinary
Prover9.  At the final boundary the store represented 109,987 current rewrite
rules in 11,205,308 bytes.

### 5.4 Compact unit index

The unit index replaces the ordinary discrimination trees that held pointers
to unit clauses.  It must find possible generalizations, instances, and
unifiable units, and it must support forward and backward unit subsumption and
unit conflict.  The index records compact term paths, clause-number lists, the
literal sign, and one yes/no bit saying whether each symbol occurs.  These cheap properties
can reject many impossible answers.  A surviving unit clause is reconstructed
and passed to the ordinary exact operation.  The index occupied 13,166,960
bytes at the final boundary.

### 5.5 Compact backward-demodulation index

Backward demodulation must find a matching subterm anywhere in a retained
clause, not merely at a clause or literal root.  The new index groups term
occurrences by their root symbol, their clause number, and a short description
of the symbols found near that occurrence.  Clause numbers and term positions
usually increase within each stored list, so the index stores the difference
from the preceding number rather than repeating each full number.  This
**delta encoding** reduces space.  A query reads only groups whose recorded
nearby symbols are compatible with the new demodulator's left side.  The
ordinary matcher then checks every remaining occurrence.

This index exposed the main CPU danger of compact representations: small
entries save RAM, but a weak lookup can return billions of impossible
occurrences for exact checking.  The implementation therefore added more
selective descriptions of nearby term paths, kept all occurrences of one
clause together, used the difference encoding above, and rebuilt the index from
only a fixed number of archived clauses at a time.  These changes reduced both
stored bytes and false candidates.
The final index used 12,109,376 bytes and required 174,774 exact checks.

### 5.6 Compact nonunit subsumption index

For nonunit forward and backward subsumption, each clause is summarized by
standard numerical features such as literal counts and symbol occurrences.
The summaries are stored in a compact prefix tree: feature sequences with the
same beginning share the same initial tree path.  Its answers are clause
numbers.  These necessary conditions can reject a clause that cannot possibly
subsume the query, but they cannot prove subsumption.  Ordinary Prover9
subsumption checks the survivors.  This index used only 1,508,576 bytes at the
final boundary, but it is essential: without it, archived passive clauses would
be invisible to a redundancy operation that can alter the search.

### 5.7 Shared serialized terms

The first versions of the four compact indexes each stored their own encoding
of the same clause terms.  The final version stores one 32-bit token sequence
per clause in a shared pool.  The first index that needs the clause creates the
sequence; the other indexes store the position of that same sequence.  A
two-level clause-number table allocates lookup space only for ranges that are
actually used.  When the token array fills, its capacity grows by 12.5% rather
than doubling, because doubling a large array would reserve much more RAM than
the search needs.

Deleted or replaced clauses leave unused token sequences.  Periodically the
live sequences are copied together into a new, smaller pool.  This operation
is delicate because all four indexes store positions in the pool.  The code
first computes a table translating every old position to its new position,
then rebuilds every dependent index, and exposes the new pool only after all
references agree.  The lists and translation tables used during this operation
are sorted through a temporary file when they become large, so RAM does not
hold both several large input and output arrays at once.  On Linux, the
`mremap` system call can sometimes move the virtual-memory mapping without
copying the token bytes themselves.

The final pool held 3,220,436 tokens in 16,219,320 bytes.  Five rounds of
packing live sequences together had recovered 14,793,444 bytes.  The four
indexes referred to an already stored token 38,557,344 times instead of making
another copy.

This shares a clause's encoding among indexes, but it does not yet merge equal
subterms from different clauses.  At 1,000 givens the measurement found
728,510 occurrences of subterms but only 91,686 structurally different
subterms.  A directed acyclic graph (DAG) with one shared node for each
different subterm could therefore provide further sharing.  Its bare nodes and
child references were estimated at 1,304,344 bytes, compared with 4,194,304
bytes then reserved by the token pool.  This is only an optimistic upper bound
on the saving: a usable implementation would also need a table for finding an
existing equal node, information saying which nodes remain used, and spare
capacity.

### 5.8 Packed-fast hints

The accepted hint index stores compressed hint bodies and lists of hint numbers
that possess safe structural features.  For a query described by at most eight
such features, it can cache the exact combined list if that list also contains
at most eight possible hints.  Wider queries or larger answers simply combine
their feature lists again in the ordinary way.  The cache has a fixed 16,384
entries, each 136 bytes, and therefore occupies
2,228,224 bytes regardless of how long the search runs.  At the proof boundary,
35.08% of eligible queries found a valid cached answer.  Those hits avoided
examining 136,373,547 hint numbers from the individual feature lists.

Compression does not freeze logical hint state.  A hint rewritten by backward
demodulation receives new index entries.  Old entries that still name its
previous form are recognized by a version number and ignored; the lists are
rebuilt when too many such obsolete entries accumulate.  Hint degradation and
the option that permits only one match are updated only after the ordinary
exact matcher accepts a clause.  Checkpoints save these changes.  The complete
packed hint representation—bodies, structural index, mutable state, and
cache—used about 15.2 MiB for 88,494 hints at the final boundary.

### 5.9 Controlling dead records and peak memory

Small permanent records alone did not guarantee a small peak RSS.  Rebuilding
an index can temporarily require both its old and new representation, and dead
entries can accumulate between rebuilds.  The following measures bounded that
temporary and obsolete state:

- the unit, rewrite, and backward-demodulation indexes are rebuilt when the
  percentage of obsolete records reaches `compact_index_stale_pct`;
- unused term sequences are packed away after their estimated size reaches
  `compact_term_reclaim_kb`;
- rebuilding the backward-demodulation index reads only 4,096 archived clause
  numbers at a time instead of first constructing one array containing every
  retained clause number;
- large sorting jobs use a temporary file rather than equally large scratch
  arrays in RAM;
- an old array is freed as soon as its information has been transferred,
  before the next replacement array is allocated;
- storage already allocated for compact records is reused during rebuilds;
- clause-number lookup uses small direct tables allocated by occupied ID range,
  rather than a general lookup table that stores extra bookkeeping with every
  clause number;
- the passive selection records, hints, and search indexes are freed before the
  final proof graph is reconstructed;
- before any Prover9 allocation, `P9_COMPACT_HEAP=1` asks the GNU C allocator
  to return freed medium-sized arrays promptly to the operating system.

This explains why adding up the objects present at termination is not enough.
The memory maximum can occur earlier, while an old and replacement index coexist
or while the final proof is being reconstructed.  The order of allocation and
freeing is therefore part of the memory design.

## 6. Correctness and search-equivalence evidence

The implementation was checked repeatedly against the old representation,
rather than waiting for one long proof to reveal a discrepancy.  At each
bounded search prefix, the old run served as the expected answer: the tests
compared every relevant clause-processing and hint event, not merely the final
clause counts.

### 6.1 Component tests

Focused tests cover damaged archive records, allocation and release of memory,
and removal of deleted selection records.  They also check adjustment of term
positions when the shared pool is packed, clause-number lookup, and candidate
retrieval for rewriting, unit operations, backward demodulation, and nonunit
subsumption.
Other tests cover the packed hint lists, read-only hint look-ahead, matching a
unit hint without reconstructing it, and resuming a partly generated sequence
of paramodulation or hyperresolution conclusions.

The main aggregate commands are:

```sh
make all -j4
make test1
make compact-frontier-tests
make discount-tests
```

### 6.2 Exact prefix comparison

At 300 givens, the test log contains 132,567 ordered records saying which
candidate was processed, which hint operation occurred, which clause was kept,
and which given clause was selected.  This complete event log is byte-for-byte
identical to the log from an OTTER control that uses the same `packed_fast`
hint index but keeps ordinary passive clause bodies in RAM:

```text
SHA-256 bc38f369ef02271d9b8a00db0cd01a6ba8f890e9608e0abd947de68f763322aa
Given=301 Generated=120793 Kept=5737
Usable=285 SOS=4298 Demodulators=3282 Disabled=1183
```

A fresh side-by-side comparison at 1,000 givens also has identical terminal state:

| Representation | User | Wall | Peak RSS | Terminal state |
| --- | ---: | ---: | ---: | --- |
| Compact indexes, ordinary resident bodies | 76.90 s | 92.58 s | 90,404 KiB | reference |
| Compact indexes, file archive, zero cache | 81.35 s | 97.27 s | 90,404 KiB | exact |

Both end at `Given=1001`, `Generated=1,268,285`, `Kept=33,909`,
`Usable=949`, `SOS=26,052`, `Demodulators=21,741`, and `Disabled=6,937`, with
the same hint state and compact-index counters.  The file representation costs
only 1.058 times the user CPU of the full-body compact-index control.

### 6.3 Checkpoint/resume

A current full-hint file-backed run wrote a checkpoint after 100 given clauses
and resumed until given 301.  Joining the event log before the checkpoint to
the log after resume gives the same SHA-256 digest,
`bc38f369...322aa`, as an uninterrupted run.  Thus the order and contents of
all 132,567 recorded events agree, not just the final populations.  The final
counts, including `Disabled=1183`, also agree, and 22 archive/index integrity
checks pass.

The audit found one defect in reporting, not in the resumed search.  Some
temporary clauses are counted as disabled even though they are deleted before
receiving a clause number.  Checkpoint format 3 correctly omitted these dead
bodies, but initially forgot their contribution to the printed disabled count.
It now stores only that count as `disabled_checkpoint_omitted`; it still does
not store or reconstruct the dead clauses.  Resumed statistics therefore match
without reintroducing their memory cost.

### 6.4 Full proof

The accepted current run terminates with:

```text
Given=2945 Generated=8248032 Kept=272787 proofs=1
Usable=1800 SOS=131001 Demodulators=109987 Limbo=300 Disabled=139715
Hints=88494 Active_Hints=4122
```

`Limbo` is Prover9's temporary queue of newly kept clauses: forward processing
has accepted them, but backward subsumption and backward demodulation wait until
the current inference-rule call finishes.  `Active_Hints` counts hints still
available after redundancy and deactivation, not all hints in the input.

Running `prooftrans parents_only` removes clauses that are not ancestors of the
final contradiction.  It emits 7,051 proof clauses, from which 3,231 new hints
are extracted.  After irrelevant formatting differences are removed, the proof
has SHA-256
`9d7c9a12894c1c11ede6aeae08d1cec658ccee66a47fb9663859347a5413fd07`.
It is byte-identical to the accepted full-body `packed_fast` reference.

The older FPA run generates and keeps two additional clauses classified as
`other`, which shifts later clause numbers and the order of some independent
proof lines.  It
nevertheless reaches the same given boundary, SOS and demodulator populations,
proof length, number of new hints, and theorem.  Therefore the strongest exact
representation claim is made against the full-body `packed_fast`
control.  The comparison with old P9 establishes the same theorem, proof size,
and given-clause boundary, but not literally identical diagnostic output.

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

The accepted run stays below the required 128,000-KiB upper limit by 580 KiB.
That is a narrow margin and should be remeasured after changing the C memory
library, compiler, or machine.  It does not pass the optional 115-MiB stretch
target.

### 7.2 Resident-memory accounting

At the frozen terminal report, resident components were:

| Component | Bytes | Function |
| --- | ---: | --- |
| Compact rewrite-rule store | 11,205,308 | rewrite rules and possible rewrite positions |
| Compact unit index | 13,166,960 | possible unit subsumption, conflict, and unification partners |
| Compact backward-demodulation index | 12,109,376 | subterms that a new rule might rewrite |
| Compact nonunit index | 1,508,576 | possible nonunit subsumption partners |
| Shared term pool | 16,219,320 | one serialization per indexed clause |
| Dense passive records | 9,532,864 | SOS selection/status |
| Given-clause priority queues | 611,368 | ordering the small SOS records for selection |
| Hint nodes | 65,576 | packed hint structure |
| Hint references | 2,658,640 | lists of hint numbers sharing an index feature |
| Hint tables/cache | 10,355,712 | index tables and saved answers to recent queries |
| Packed hint bodies | 2,873,239 | compressed hint-clause records |
| Ancestor file-position table | 4,198,608 | archive position for each proof-parent clause number |
| Clause-ID structures | 2,316,400 | finding a clause/archive record from its number |
| Prover9 allocator reservation | 24,123,712 | ordinary live objects and reusable allocator pages |
| Other process pages | 1,025,024 | executable code, libraries, and shared/nonanonymous pages |
| **Accounted total** | **111,970,683** | **106.78 MiB** |

At the same terminal point, PSS was 112,809 KiB (110.17 MiB), so the named
components explain 96.93% of measured process memory after shared pages are
apportioned.  Periodic samples observed 123,084 KiB RSS; the operating system's
whole-run high-water counter, printed by `/usr/bin/time -v`, recorded a peak
of 127,420 KiB.

The 84,404,096-byte ancestor file is reported separately because that number
is its length on disk, not the number of its pages in process RAM.  Recently
read portions may also exist in the kernel's discardable filesystem cache;
those pages are not part of this PSS total.

### 7.3 Structural index reduction

At 1,000 givens, the first generation of the four passive indexes—for rewrite
rules, unit clauses, backward demodulation, and nonunit subsumption—occupied
44,545,464 bytes.  The improved version
collapsed one-child discrimination-tree paths, stopped copying the same clause
terms into several indexes, stored differences between increasing clause
numbers, grouped fixed-size records in arrays, and grew large arrays in smaller
steps.  Including the shared term pool, the resulting five structures occupied
13,239,168 bytes, a 70.28% reduction.  The sequence of search events and all
terminal clause counts remained unchanged.  Peak RSS fell to 90,712 KiB at
that boundary.

This is a within-new-design reduction, not the old-P9 total-RAM result.  It is
included because it shows why “serialize clause bodies” was insufficient: at
1,000 givens the first compact rewrite/unit/back indexes already occupied about
43 MB while the clause archive itself was only about 8 MB.

### 7.4 The 11,000-given three-run comparison

The files [`../bob/chat_test.new.out1.gz`](../bob/chat_test.new.out1.gz),
[`../bob/chat_test.new.out2.gz`](../bob/chat_test.new.out2.gz), and
[`../bob/chat_test.new.out3.gz`](../bob/chat_test.new.out3.gz) use a different,
larger input profile from the final 88,494-hint acceptance problem.  They
contain 18,306 hints and all prove after roughly 11,000 givens.

The compressed output SHA-256 values are
`fd7a07f0a8ec1feafacb2672b664a393368ce728d0918c5e2708c7919d4a4406`
for `out1` and
`049a44cb9871fb28c3cf829011fb102dc390d5101ed28677dd7ced15ac5d64f2`
for `out2`.  The `out3` SHA-256 is
`2b7ab154323801bd541962d2314a23a5861def3fde53271330e4ebd2a54d340e`.

`out1` is the ordinary OTTER run: full clause bodies remain in RAM and hints
use FPA indexing.  `out2` keeps the OTTER loop but uses small passive records,
the `packed_fast` hint representation, and the four clause-number-based
indexes.  Its proof ancestors are held in a mapped file.  This was an early
version.  `out3` uses the current shared-term and compact-index implementation,
explicit file reads rather than a whole-file mapping, bounded rebuild buffers,
and the final packed-hint cache.  It reproduces `out2`'s terminal search exactly:
the Given, Generated, Kept, SOS, demodulator, and disabled counts all agree.

| Measure | Ordinary `out1` | Early compact `out2` | Current compact `out3` | Current vs ordinary |
| --- | ---: | ---: | ---: | ---: |
| Given | 11,368 | 11,369 | 11,369 | +1 |
| Generated | 253,338,893 | 253,302,129 | 253,302,129 | -36,764 |
| Kept | 2,216,836 | 2,207,014 | 2,207,014 | -9,822 |
| SOS | 1,454,686 | 1,520,775 | 1,520,775 | +4.5% |
| Demodulators | 1,297,818 | 1,362,847 | 1,362,847 | +5.0% |
| Disabled | 739,073 | 662,309 | 662,309 | -10.4% |
| Internal `Megabytes` | 3,354.28 | 355.86 | 341.44 | 89.82% lower |
| Terminal `RSS_kb peak` | 3,603,800 KiB | 1,492,656 KiB | 834,780 KiB | 76.84% lower; 4.32x smaller |
| User CPU | 5,110.38 s | 15,144.10 s | 12,201.61 s | 2.388x |
| Wall | 5,131 s | 15,178 s | 12,272 s | 2.392x |

The current implementation is a real improvement over the early compact run:
19.43% less user CPU, 44.07% lower terminal peak RSS, and a slightly smaller
internal count.  It is nevertheless still impractical as a general replacement
for ordinary indexing on this workload because it takes 2.388 times the CPU.
The `out3` counters localize the remaining scaling failures:

- 19.190 billion possible opposite-sign unit pairs reached the exact
  unit-conflict test across 4.207 million queries, about 4,562 exact tests per
  query;
- backward demodulation examined 21.500 billion posting groups and 20.331
  billion symbol occurrences to produce only 695,309 exact candidates;
- compact nonunit forward subsumption performed 21.010 million exact tests;
- the file ancestor store materialized 21.055 million clauses and issued
  42.222 million reads (4.044 GB), so materialization must be attributed rather
  than assumed negligible;
- packed hint matching considered 2.380 billion postings and skipped 832.323
  million stale references, although its bounded cache avoided another 2.630
  billion postings;
- demodulation itself made about 6.10 billion attempts.

`out1` and the compact searches followed similar but not identical sequences,
as the one-given and population differences show.  The two compact searches
have identical terminal counts.  These files contain no external
`/usr/bin/time -v` result, but their terminal `Allocator_slabs` lines do contain
a process peak-RSS diagnostic.  `out3` lowers that diagnostic by 76.84% versus
ordinary `out1`.  Its 341.44-MiB internal counter is incomplete because it does
not charge cached file blocks outside the process, so a cgroup measurement is
still needed for total job memory.  In addition, `out3` reports
`compact_policy=disabled`: `P9_COMPACT_HEAP=1` was not active.  Therefore the
memory result should be repeated under the controlled allocator policy before
being promoted to a production claim.  Neither limitation explains the 2.388x
user-CPU ratio or the billions of measured candidates.

The result overturns the earlier optimistic extrapolation from the 2,945-given
proof.  The present compact representations are small and logically exact, but
their retrieval algorithms do not scale generally.  The implementation plan
for structurally selective, operation-specific indexes and training/holdout
validation is [`P9-GENERAL-COMPACT-INDEXING-PLAN.md`](P9-GENERAL-COMPACT-INDEXING-PLAN.md).

### 7.5 How much memory should be expected on the largest AIM runs?

There are now two answers, because the amount of RAM grows with different
parts of the search in the two architectures.

For problems where reproducing the old selected-clause order matters, the measured
whole-process result is 76.85% less peak RSS on the current full proof.  The
large fixed hint collection and small 131,001-clause final SOS make a literal 80%
target difficult on this particular problem.  The obsolete 11,000-given run
and the current rerun show that savings can grow when millions of passive/index
records dominate.  The current run's embedded diagnostic measures 76.84% less
peak RSS.  It must not be extrapolated quantitatively because it uses a
different input profile, had allocator compact policy disabled, and has no
cgroup accounting for filesystem cache.

For DISCOUNT with a bounded collective frontier, the intended large-search
behavior is stronger:

```text
ordinary OTTER RAM
    = fixed hints
    + memory growing with every retained passive body and its eager index entries
    + proof history

DISCOUNT/collective RAM
    = packed hints
    + memory growing with selected active clauses and their indexes
    + compact parent history and resume records for delayed inference work
    + a fixed configured number of not-yet-processed conclusions
    + file-backed proof/passive bodies
```

At the archived Osborn and AAPERM boundaries, this would replace 4.59 or 8.50
million complete in-memory SOS clauses by the much smaller active set, compact
proof history, resume records, and a fixed-size group of conclusions.  An
80--95% whole-process reduction is therefore plausible.  The 90% midpoint is
only a planning estimate, not a measurement.  The principal risk is that the
changed time of inference and rewriting may lead the search away from the old
proof or require repeated resimplification of passive clauses.

The defensible deployment statements today are therefore:

- **Accepted and measured:** 76.85% less peak RSS, same proof boundary, no CPU
  penalty on the current `chat_test`/Osborn proof.
- **Current large-profile diagnosis:** at 11,000 givens, 76.84% less
  self-reported peak RSS and 89.82% less internally accounted memory, but with
  2.388 times the CPU, allocator compact policy disabled, and no cgroup
  page-cache accounting.
- **Conditional forecast:** 80--95% less total RAM on much larger
  AIM runs with millions of passive clauses, to be established with
  current-binary RSS/PSS and cgroup page-cache measurements, plus
  proof/coverage comparisons.

## 8. What worked, what failed, and why

| Idea | Outcome | Reason |
| --- | --- | --- |
| Delete rejected generated clauses | No radical gain | Most tautologies and forward-subsumed clauses were already destroyed immediately. |
| Delete disabled clauses | Useful but insufficient | 4.5--30% measured; proof parents still require a representation; live SOS remains. |
| Compress disabled bodies | Kept | Exact, useful foundation for proof history, but does not stop memory from growing with the live SOS. |
| Allocator that returns empty blocks to the OS, plus smaller clause records | Kept | Reduced memory held for reuse and reduced per-clause overhead; both matter to peak RSS. |
| Map the ancestor file into the process (`mmap`) | Rejected as final default | Statistics and scans brought old, infrequently needed pages into RAM; a 13.1-GB mapping had 10.2 GB resident. |
| Read individual archive records from a file | Kept | The full logical archive remains on disk; only one small reusable I/O buffer is required in process RAM. |
| Strict DISCOUNT active/passive split | Kept as experimental/product alternative | RAM grows with active rather than all passive clauses, but demodulators become available at different times and the Osborn search changes. |
| Small resume records for groups of delayed inferences | Kept as experimental alternative | Bounds how many generated conclusions exist in RAM and can be checkpointed; hint count alone did not recover the proof. |
| DISCOUNT that keeps every rewrite rule simplified by the other rules | Not default | Simplifies strongly, but repeatedly resimplifies stored clauses and changes normal forms expected by the historical hints. |
| Packed hints, first version | Replaced | Saved RAM, but its structural lookups returned far too many possible hints for exact checking. |
| `packed_fast` hints | Kept | Produces the same hint-event log; chooses sparse-list or bit-set combination by list size and uses a fixed-size cache to recover good prefix speed. |
| Compact OTTER bodies only | Insufficient | Compact indexes, not bodies, dominated by 1,000 givens. |
| First complete set of compact passive indexes | Correct but too slow | Billions of possible unit-conflict and backward-demodulation answers reached later checks in the 11k run. |
| Collapsed one-child tree paths and nearby-symbol tests | Kept | Use less tree memory and reject more impossible answers without changing the final exact answer. |
| One term encoding shared by four indexes | Kept | Removes four copies of the same clause-term encoding. |
| Store differences between increasing clause numbers and term positions | Kept | Uses fewer bytes and places sequentially read data together. |
| Large decoded passive cache | Rejected | 0/4/32-MiB experiment saved decodes but did not save CPU. |
| 8K final packed-fast cache | Rejected | Saved only 596 KiB at full proof, increased CPU, and left more partly empty allocator blocks in RAM. |
| 16K-entry bounded `packed_fast` cache | Kept | A fixed 2.23-MB table answered 35.08% of eligible repeated queries and avoided examining 136 million hint numbers. |
| Repack terms using complete in-RAM old/new-position arrays | Replaced | Temporary copies raised peak RSS even though the final index was small. |
| Repack terms in batches and sort large translation tables through a file | Kept | Bounds temporary RAM while preserving the exact mapping from old to new term positions. |
| Free search-only structures before reconstructing the proof | Kept | Prevents the full search state and full proof graph from occupying RAM simultaneously. |

Three broader lessons follow.

First, an ATP index must be judged both by its size and by how many possible
answers it returns.  Every false candidate is sent to a more expensive exact
logical test.  A tenfold smaller index that causes a thousandfold more exact
tests is a net loss.

Second, contraction scheduling is part of the heuristic meaning of an algebraic
search.  Fairness or refutational completeness does not imply that a historical
hint-guided proof remains reachable under the same resource limit.

Third, peak memory depends on when objects coexist, not only on their final
sizes.  Several effects can dominate the peak even when each final record is
small: accessed pages of a mapped file, memory retained by an allocator for
reuse, simultaneous old and replacement indexes, and reconstructing a proof
before freeing the search state.

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

The first six assignments select the old OTTER inference order, small passive
records, compact hints, and ordinary eager clause-by-clause inference
generation.  They also select the explicit disk-file archive and remove the SOS
population limit.  `process_initial_sos` passes input SOS clauses through
ordinary simplification and indexing before the search.  `back_demod` and
`back_demod_hints` preserve backward
rewriting of retained clauses and hints.  The three `clear` commands disable
unit deletion, the proof-ancestor-aware refinement of subsumption, and the
alternative evaluation-based rewrite mode.  The present compact indexes do not
implement those three combinations.

The four `compact_otter_*` flags replace the passive-clause indexes for,
respectively, ordinary demodulation, unit-clause operations, backward
demodulation, and nonunit subsumption.  `compact_passive_cache=0` says not to
retain reconstructed passive bodies in RAM.  The last two assignments rebuild
an index when at least 10% of its entries are obsolete and reclaim unused term
encodings after their estimated total reaches 2,048 KiB.

The measured configuration also uses a process-start environment setting:

```sh
P9_COMPACT_HEAP=1 /usr/bin/time -v bin/prover9 \
  -f your-compact-input.in \
  > your-compact-output.out \
  2> your-compact-output.time
```

On systems using the GNU C library, `P9_COMPACT_HEAP=1` asks the allocator to
obtain medium-sized arrays in a form that can be returned promptly to the
operating system after an index rebuild.  This option does not change logical
search behavior, but it is part of the measured memory configuration.

Do not use `ancestor_store=mmap` for the final comparison.  Do not enable only
some of the four `compact_otter_*` indexes: dense OTTER refuses such a mixed
configuration because it could omit passive clauses from an operation.

Current restrictions checked at startup include:

- `sos_limit` must be `-1`;
- `process_initial_sos` must remain set;
- `inference_frontier` must be `clauses`;
- `unit_deletion` and `ancestor_subsume` are not supported by the current
  compact unit/nonunit indexes;
- `back_demod` must remain set for the compact backward-demodulation index;
- `eval_rewrite` is incompatible with the compact rewrite-rule store.

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
limits both inside Prover9 and in the invoking shell.  Independent small cases
can run in parallel, but their number should not exceed the real CPU-core count
and the sum of their expected peak RSS must fit in physical memory.

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
its SHA-256 digest.  A sample is suitable for debugging and side-by-side prefix
comparisons; it is not a substitute for the full-hint proof experiment.

### 9.5 Disk backing and `/proc`

The current source hard-codes `/tmp` in both places that create archive files.
It does
**not** consult `TMPDIR`.  It creates either
`/tmp/prover9-ancestors-XXXXXX` or `/tmp/prover9-passive-XXXXXX` and unlinks the
name immediately.  “Unlinking” removes the directory name but does not delete
the contents while Prover9 still holds the file open.  The file therefore
appears as `(deleted)` through the process file descriptors and is finally
removed when the process exits.

Find it with:

```sh
for fd in /proc/PID/fd/*; do
  printf '%s -> %s\n' "$fd" "$(readlink "$fd")"
done
```

Then distinguish three quantities: the logical file length, the disk blocks
actually allocated to the file, and the subset of mapped pages currently in
physical RAM:

```sh
stat -Lc 'logical=%s bytes, blocks=%b, block-size=%B' /proc/PID/fd/FD
du -hL /proc/PID/fd/FD
awk '/prover9-(passive|ancestors).*deleted/ {show=1} \
     show && /^(Size|Rss|Pss|Private_Clean|Private_Dirty|Swap):/ {print}' \
    /proc/PID/smaps
```

Replace `PID` by Prover9's process number and `FD` by the open-file number found
by the first loop.

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

Do not compare only the last `matched=` field.  In some reports it counts only
hints that are active at that moment, not every hint matched earlier and not
cumulative proof progress.  Do not compare only `Generated`, which includes
rejected clauses that are destroyed immediately.  Do not compare runs at
different given boundaries as if they were equal-work memory measurements.

## 10. Future work

The accepted result satisfies the current proof acceptance criteria, but does
not end the research program.
The following order separates measurements needed now from more invasive
designs.

### 10.1 Replace broad compact retrieval with selective indexes

The current-binary `chat_test.new.out3` run has supplied the previously missing
large-profile result.  It preserves the compact run's exact terminal search and
reduces the embedded peak-RSS diagnostic by 76.84%, but remains 2.388 times
slower than ordinary Prover9.  Parameter tuning alone is not an adequate
response: unit conflict and backward demodulation still traverse 19.190 billion
exact candidates and 21.500 billion posting groups, respectively.

The highest-priority work is therefore the phased implementation in
[`P9-GENERAL-COMPACT-INDEXING-PLAN.md`](P9-GENERAL-COMPACT-INDEXING-PLAN.md):
per-operation attribution and candidate distributions; structurally selective
unit retrieval; exact-path backward-demodulation traversal; fewer nonunit and
archive materializations; and a packed-hint maintenance policy validated on a
frozen training/holdout split.  Each component must pass candidate, CPU, RAM,
and proof/search-trace gates before it becomes the default.

After those gates, rerun the same three configurations: ordinary OTTER/FPA/full
bodies; full bodies with compact indexes, to isolate retrieval CPU from archive
I/O; and file-backed compact OTTER.  Use `P9_COMPACT_HEAP=1`, sample RSS/PSS
externally, charge the cgroup's page cache, and impose generous but finite time
and memory limits.

### 10.2 Validate genuinely large AIM runs

Run current compact OTTER and the DISCOUNT/collective alternative on at least
three problems with very large passive sets, including Osborn and AAPERM, with
identical input digests and external limits.  Compare memory and CPU after the
same number of selected clauses.  Also compare how many problems each mode
solves under the same time and memory limit, because a mode that follows a
different search path may do unequal logical work at an equal given count.  A
radical acceptance claim should require at least 80% lower external peak RSS,
the corresponding cgroup total including file cache, and an itemized account
of at least 95% of measured PSS.

### 10.3 Reduce false candidates reaching exact tests

The 11,000-given current output shows that unit conflict and backward
demodulation can dominate even when each index entry is small.  Future index
work should report how often a query returns 0, 1, 2--7, 8--31, or much larger
sets of possible answers.  Average bytes per index node do not reveal a small
number of catastrophic billion-candidate searches.  Promising directions are:

- record symbols at deeper term positions only when measurements show that
  those symbols are rare enough to reject many impossible answers; the
  ordinary exact test must still make the final decision;
- compare the current discrimination tree with standard substitution-tree or
  code-tree term indexes, which incorporate more of matching or unification
  while traversing the index and can therefore prune earlier;
- store clause-number lists in compressed blocks with markers that permit the
  intersection algorithm to jump over a whole irrelevant block rather than
  decode every number;
- separate policies for unit conflict, generalization, and unification instead
  of one structure optimized for their average;
- perform more exact matching directly on the compact number sequence, avoiding
  construction of ordinary term objects for candidates that fail quickly.

Any prototype must reproduce the complete 300-given event log and be compared
with the accepted final index, not with the much slower first packed version.

### 10.4 True cross-clause term sharing

At 1,000 givens, 87.4% of subterm occurrences were duplicates of a subterm seen
elsewhere.  This justifies testing a shared term DAG: one node is stored for
each structurally different term, and every equal occurrence points to that
node.  This technique is often called hash-consing.  A sound design
needs:

- nodes that cannot change after creation, identified by their top symbol and
  the node numbers of their immediate subterms;
- one short list of atom or literal root nodes for each clause;
- a construction-time lookup table for finding an already stored equal term,
  which can be reduced or discarded after construction;
- a way to determine which nodes are still used when clauses or index entries
  are deleted;
- matching, unification, term ordering, and rewriting that operate on shared
  node numbers without first copying the terms;
- a disk format that does not depend on node numbers valid only in one process;
- measurement of the temporary peak while old and replacement DAGs coexist.

Sharing saves RAM only if the lookup table and the information needed to remove
unused nodes cost less than the duplicate term sequences they replace.  The
current measurement shows the maximum plausible saving; it does not yet prove
that a complete implementation will achieve it.

### 10.5 Move old feature lists to disk

Compact OTTER still keeps in RAM the indexes that must return every possible
passive-clause candidate.  For an SOS containing millions of clauses, the next
step is to divide an index into a small RAM part and a large disk part:

- a small directory in RAM plus the most recently added entries;
- compressed, read-only blocks of clause-number lists on disk;
- occasional large sequential merges that combine old and new blocks and
  remove obsolete entries;
- block summaries that let list intersection skip irrelevant disk blocks;
- stable clause IDs and ordinary exact checks.

This is the same general organization used by a disk-based inverted index: a
feature points to the stored numbers of all clauses having that feature.  It
preserves the OTTER requirement that every passive clause be visible while
allowing old index lists, not just old clause bodies, to leave RAM.  Backward
demodulation is the difficult case because one rewrite rule can require a broad
search.  Processing several new demodulators together and scanning compressed
blocks sequentially may be cheaper than issuing millions of small disk reads.

### 10.6 Rewrite directly in the compact representation

Backward demodulation currently reconstructs an archived clause as ordinary C term
objects, rewrites and reprocesses it, and writes it back to the archive.  A
direct compact implementation could read the number sequence into one
fixed-size work area, apply the rewrite rules to that representation, and write
the replacement record without building an ordinary full clause object
(`Topform`) and term tree.  The
experiment should measure the reported backward-demodulation and preprocessing
times as well as temporary peak RSS.  It must produce exactly the same
justification, attributes, hint matches, equation orientation, and action-rule
effects as the ordinary path.

### 10.7 Improve collective search guidance, not just hint counts

The collective scheduler should be judged by whether it constructs connected
parts of a proof, not merely by how many unrelated hint shapes appear.  A hint
match says nothing about whether the parents needed for the next proof step are
present.  Possible priority signals include:

- whether a candidate completes the known parent requirements of a later hint
  extracted from a previous proof;
- how many known proof steps separate the candidate from a not-yet-matched
  successor hint;
- minimum service shares for the inference rules and paramodulation directions
  that occur in the source proofs;
- separate priority for conclusions that preserve the expected normal form;
- a bounded rerun forced to follow the old given/inference order, used as a
  reference comparison.

Some turns must continue to process pending work in creation order.  A
heuristic queue may process a promising conclusion early, but it must not
prevent any enabled inference rule from eventually running.  Every previewed
conclusion must also pass through ordinary exact clause processing exactly once.

### 10.8 A two-stage production workflow

Where an identical sequence of selected clauses is not required, a practical
portfolio can first run many low-memory DISCOUNT/collective searches.  Proofs
or newly discovered hints from successful jobs can then guide a smaller number
of compact-OTTER reruns that use exact historical-style processing.  This
realizes the old
`Plans-RAM-24.txt` idea of running many cheap searches and a smaller number of
proof-producing reruns without throwing proof parents away unsafely.

This workflow complements rather than replaces compact OTTER.  It should be
evaluated by total solved problems per machine-day and verified proofs per GiB,
not by the success of one particular search path.

### 10.9 Operational hardening

Smaller but important product tasks are:

- honor `TMPDIR` or add an explicit `archive_directory` option;
- fail early on insufficient disk space and report the backing filesystem;
- expose the archive's open-file number and path before unlinking when
  diagnostics are requested;
- use cgroup v2 counters in the benchmark harness;
- record binary/input hashes and `/usr/bin/time` output automatically;
- test the required 128,000-KiB upper limit on more C-library/kernel
  combinations;
- document unsupported option combinations in the public Prover9 manual.

These do not create the radical saving, but they make a week-long run
reproducible and prevent disk-backed memory from being misreported.

## 11. Reproducibility map

The main source files are:

- [`provers.src/search.c`](provers.src/search.c): the main given-clause loop,
  the order of immediate simplification operations, archive use, checkpoints,
  statistics, and allocation/release of search structures;
- [`provers.src/giv_select.c`](provers.src/giv_select.c): the small SOS records
  and given-clause priority queues;
- [`provers.src/cold_passive_store.c`](provers.src/cold_passive_store.c):
  compressed passive-clause store used by DISCOUNT, with either RAM or a mapped
  file as backing;
- [`ladr/clause_store.c`](ladr/clause_store.c): archive holding both disabled
  proof parents and complete compact-OTTER passive clause bodies;
- [`ladr/hints.c`](ladr/hints.c) and
  [`ladr/hint_postings.c`](ladr/hint_postings.c): compressed hints and lists of
  hint numbers indexed by structural features;
- [`provers.src/compact_rewrite.c`](provers.src/compact_rewrite.c): compact
  rewrite-rule store;
- [`provers.src/compact_unit_index.c`](provers.src/compact_unit_index.c):
  possible unit matches, subsumption partners, and conflicts;
- [`provers.src/compact_back_demod.c`](provers.src/compact_back_demod.c):
  finding clause subterms that a new demodulator might rewrite;
- [`provers.src/compact_feature_index.c`](provers.src/compact_feature_index.c):
  nonunit subsumption candidates;
- [`provers.src/compact_term_pool.c`](provers.src/compact_term_pool.c): term
  encodings shared among indexes and adjustment of their positions after
  unused encodings are removed;
- [`provers.src/compact_id_map.c`](provers.src/compact_id_map.c): tables mapping
  clause numbers to compact records without allocating every unused ID range.

The detailed engineering records supporting this synthesis are:

- [`P9-PHASE5-ACCEPTANCE-AUDIT.md`](P9-PHASE5-ACCEPTANCE-AUDIT.md);
- [`P9-PHASE5-COMPACT-FRONTIER-PLAN.md`](P9-PHASE5-COMPACT-FRONTIER-PLAN.md);
- [`P9-CHAT-TEST-REPORT.md`](P9-CHAT-TEST-REPORT.md);
- [`P9-RADICAL-RAM-REPORT.md`](P9-RADICAL-RAM-REPORT.md);
- [`P9-DISCOUNT-WALDMEISTER-PLAN.md`](P9-DISCOUNT-WALDMEISTER-PLAN.md);
- [`P9-COLLECTIVE-SCHEDULER-PLAN.md`](P9-COLLECTIVE-SCHEDULER-PLAN.md);
- [`P9-COLLECTIVE-DEMODULATION-PLAN.md`](P9-COLLECTIVE-DEMODULATION-PLAN.md);
- [`P9-BETTER-PACKED-PLAN.md`](P9-BETTER-PACKED-PLAN.md);
- [`Checkpoint-Format-Spec.txt`](Checkpoint-Format-Spec.txt).

The current full-proof output files are in
`/project/phase5-results/phase5-completion-audit/final-proof-current`.  The
current 300/1,000 and checkpoint output files are in the parent
`phase5-completion-audit` directory.  Generated output files are not substitutes
for the committed written audit: both are needed to reproduce a claim.

The work from the frozen memory baseline to the final audit comprises 192
small commits.  Early stages added exact archived proof parents, returned
unused allocator blocks to the operating system, and introduced DISCOUNT,
small passive records, resumable inference generation, and compressed hints.
Later stages added four clause-number-based OTTER indexes, shared term
encodings, explicit file reads, bounded index rebuilds, and release of search
structures before proof reconstruction.  A checkpoint audit closed the work.
The detailed commit messages are part of the implementation record.

## 12. Conclusion

The original question was how to reduce Prover9 RAM by 80--90%, not by another
small constant factor.  The experiments show why local deletion could not meet
that goal.  Millions of SOS clauses were not merely proof debris; under the
OTTER loop they remained logically active for contraction and therefore
appeared in multiple term and clause indexes.  Hints and proof ancestors added
large fixed and historical terms.  Any radical design had to change all of
these representations together.

The strict DISCOUNT/collective architecture gives the best bound on how RAM
grows with the search and remains promising for exploration with very large
passive sets.  It also demonstrated that a theorem prover's practical behavior
is not determined by its inference rules alone.  The timing of demodulation,
passive normalization, hint matching, and rule interleaving can decide whether
a known proof is found.

The accepted compact-OTTER design resolves that tension for the current problem,
where reproducing the old sequence of selected clauses matters.  It preserves
the old order of immediate simplification and hint operations.  Complete
passive clauses move to an exact disk archive, and smaller indexes refer to them
by clause number rather than by pointers.  Those indexes only propose possible
answers; the old exact routines still decide them.  The result is a validated
proof at the same 2,945-given boundary, with peak RSS 4.32 times smaller and no
CPU penalty on that acceptance case.  The larger 11,000-given run shows that
this CPU conclusion does not generalize: current compact OTTER is 2.388 times
slower there despite a 76.84% lower embedded peak-RSS diagnostic.

The literal 80--90% whole-process target is not yet a universal measured claim:
the accepted proof saves 76.85%; the current large-profile run reports 76.84%
but is too slow, used no compact allocator policy, and did not charge the
cgroup's cached file data.  The next decisive evidence must come after the
general-purpose indexing plan passes its frozen training and holdout gates,
followed by controlled 11,000-given and multi-million-SOS AIM runs.  Those runs
must distinguish index speed and search behavior from the already solved
problem of keeping every passive clause as a complete in-memory C object.

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
