# Phase 5: compact OTTER-compatible frontier

Date: 2026-08-10 (Europe/Berlin)

## Objective

Recover the historical Osborn proof trajectory and proof while retaining the
radical-memory representation developed on `packed-fast`.  This phase is not
another DISCOUNT scheduler experiment.  It preserves OTTER's eager treatment
of every retained clause while replacing pointer-rich passive bodies and
indexes with stable-ID compact structures.

The new mode is isolated behind the proposed configuration:

```text
assign(search_loop,otter).
assign(passive_store,dense).
assign(hint_index,packed_fast).
assign(inference_frontier,clauses).
assign(ancestor_store,mmap).
assign(sos_limit,-1).
set(back_demod_hints).
set(compact_otter_demodulation).
set(compact_otter_unit_index).
set(compact_otter_back_demod_index).
set(compact_otter_nonunit_index).
assign(compact_passive_cache,4).
```

`search_loop=otter` without all four authoritative compact flags remains the
unchanged compatibility reference.  OTTER `passive_store=dense` fails at
startup unless every pointer-free index is authoritative; it never silently
falls back to DISCOUNT timing or a resident passive index.

## Implementation and measured status (2026-08-10)

Stages 1--4 are implemented on `phase5-compact-frontier`.  Compact rewrite,
unit, back-demodulation, and nonunit indexes first passed differential audit
at 10, 100, and 300 givens and are now authoritative.  A retained OTTER
clause completes the ordinary eager backward transaction before its body is
archived.  The dense selector and every index retain stable IDs and compact
metadata only.  Exact probes decode an immutable body on demand; selection
activates that same archived proof record.

The ancestor archive is the sole owner of a cold passive body and its proof
data.  This avoids duplicating bodies between a passive arena and proof
archive.  A 4-way hot materialization cache is bounded by
`compact_passive_cache` MiB (default 4, zero disables it).  It is an
optimization only: entries are pinned during exact probes and invalidated
before selection or backward retirement.

The bounded gates use the same clauses, hints, and selector rules in both
columns:

| Boundary | Representation | User | Wall | Peak RSS | Live P9 allocation |
|---|---:|---:|---:|---:|---:|
| 300 | compact indexes, full bodies | 19.55 s | 22.53 s | 90,612 KiB | 20.76 MB |
| 300 | archive, cache disabled | 23.22 s | 26.92 s | 90,484 KiB | 16.43 MB |
| 300 | archive, 4 MiB cache | 19.32--20.34 s | 22.04--23.01 s | 90,636 KiB | 16.89 MB |
| 1,000 | compact indexes, full bodies | 118.56 s | 133.05 s | 123,236 KiB | 45.73 MB |
| 1,000 | archive, 4 MiB cache | 140.45 s | 156.25 s | 113,264 KiB | 19.17 MB |
| 1,000 | archive, shared clause term pool | 142.07 s | 156.66 s | 94,652 KiB | 19.17 MB |
| 1,000 | archive, delta back-posting stream | 126.12 s | 140.04 s | 93,524 KiB | 19.17 MB |
| 1,000 | archive, delta rewrite-occurrence stream | 125.53 s | 138.90 s | 92,164 KiB | 19.17 MB |
| 1,000 | archive, 12.5% shared-token growth | 133.58 s | 147.56 s | 91,904 KiB | 19.17 MB |
| 1,000 | archive, dense stable-ID hashes | 131.09 s | 145.37 s | 91,760 KiB | 19.17 MB |
| 1,000 | archive, packed/tightly-grown records | 131.01 s | 145.64 s | 90,712 KiB | 19.17 MB |

All compared runs have identical given, generated, kept, usable, SOS,
demodulator, disabled, hint, and active-hint counts.  At 1,000 givens both
runs end at `Generated=1,268,285`, `Kept=33,909`, and `Sos=26,052`.  The
archive run is 1.185 times the compact/full-body CPU, within the 1.25 gate,
and is below the 125 MiB prefix RSS gate.  Its cache served 60,667 hits from
8,893 misses, peaked at about 2.03 MiB charged, and the archive reported zero
validation failures.

The 300-given component accounting explains why body eviction alone is not
the final radical reduction:

| Structure | Total | Dominant component |
|---|---:|---:|
| compact unit index | 3.57 MB | 2.62 MB trie nodes (73%) |
| compact back-demod index | 1.74 MB | 0.79 MB postings + 0.52 MB tokens |
| compact nonunit index | 0.56 MB | 0.52 MB trie nodes (93%) |

At 1,000 givens the rewrite, unit, and back-demod structures total about
43.4 MB, while the complete shared clause archive is only 8 MB.  These
indexes grow with the passive population, so extrapolating the current
representation to the historical 131,001-clause proof boundary would miss
the final 125 MiB target even though passive bodies are cold.

The first structural compression slices are now implemented.  Radix edges
preserve the former child and posting order, and the exact 300-given trace is
unchanged:

| Index | Before | After | Reduction |
|---|---:|---:|---:|
| unit | 3,573,496 B | 1,148,664 B | 67.9% |
| nonunit | 562,376 B | 189,664 B | 66.3% |
| rewrite | 1,775,880 B | 1,317,128 B | 25.8% |
| back demod | 1,741,040 B | 1,282,296 B | 26.4% |
| all four indexes | 7,652,792 B | 3,937,752 B | 48.5% |
| all four plus shared clause term pool | 7,652,792 B | 3,020,360 B | 60.5% |
| plus delta back-posting stream | 7,652,792 B | 2,760,368 B | 63.9% |
| plus delta rewrite-occurrence stream | 7,652,792 B | 2,663,112 B | 65.2% |
| plus 12.5% shared-token growth | 7,652,792 B | 2,655,900 B | 65.3% |
| plus dense stable-ID hashes | 7,652,792 B | 2,606,748 B | 65.9% |
| plus packed/tightly-grown records | 7,652,792 B | 2,399,132 B | 68.7% |

The rewrite node pool itself falls from 655,360 to 196,608 bytes (70.0%).
The optimized rewrite traversal uses one binding trail per query; allocating
a `MAX_VARS` trail in every recursive frame was measured and rejected because
it raised the 300-given user time to 23.44--24.40 seconds.  The shared-trail
version takes 18.51--20.08 seconds versus a controlled 19.46-second
token-trie run, so the accepted representation has no measured CPU penalty.
The grouped back-demodulation representation turns 53,028 raw symbol
occurrences into 25,069 `(symbol, clause ID)` groups and a 53,028-byte
delta-varint offset stream.  A same-host control took 18.52 user seconds and
the grouped run took 18.47 seconds.

Rewrite, unit, and back-demodulation terms now use one clause-coalescing term
pool.  A clause is serialized once, and every later request from another
index returns a stable slice in that serialization.  At 300 givens, 26,855
term requests require only 5,737 physical clause serializations; the pool
reuses 222,332 tokens and occupies 655,464 bytes.  The exact CHAT/Osborn
boundary remains `Given=301`, `Generated=120,793`, and `Kept=5,737`.

The 1,000-given gate exposes the remaining work.  Before these structural
changes, the four indexes occupied 44,545,464 bytes.  Their compact metadata
plus the shared pool now occupy 20,248,136 bytes, a **54.5%** reduction rather
than the required 70%.  User CPU is 142.07 seconds versus 140.45 seconds for
the pre-structural archive run (+1.2%), while peak RSS falls from 113,264 KiB
to 94,652 KiB (-16.4%).  Search state is exact at `Given=1001`,
`Generated=1,268,285`, `Kept=33,909`, `Usable=949`, `Sos=26,052`,
`Demods=21,741`, and `Disabled=6,937`.

The pool currently removes duplication among indexes for the same clause; it
does not yet hash-cons equal subterms across different clauses.  Its
1,000-given logical content is 728,510 32-bit tokens, but power-of-two backing
holds 1,048,576 tokens and its proof-ID directory uses another 1,048,576
bytes.  This capacity slack, per-index stable-record tables, and repeated
posting metadata are now larger targets than private token copies.

Exact opt-in instrumentation (`set(compact_term_sharing_stats)`) now bounds
the cross-clause opportunity instead of assuming it.  At 300 givens, 115,486
subterm occurrences contain 16,185 unique terms; at 1,000 givens, 728,510
occurrences contain 91,686 unique terms (87.4% duplicate occurrences).  A
variable-record canonical DAG whose term ID is its word offset would need
1,304,344 logical bytes for symbol words, child IDs, and clause atom roots at
1,000 givens, versus the current 4,194,304-byte token capacity.  This
2,889,960-byte difference is an upper bound, not a promised saving: a live
production hash-cons table, fingerprints, and arena slack must be included.
The diagnostic table itself is reported separately and is never enabled in
ordinary CPU/RSS measurements.

The other immediate large target is the redex posting directory.  Its 152,909
logical `(symbol, clause)` groups occupy 3,672,064 bytes at 1,000 givens
because each group is a 12-byte linked record in a doubled array.  A
per-symbol block stream can delta-varint the monotonically increasing record
and occurrence positions while retaining insertion traversal.  This avoids
coupling the first production reduction to the much broader DAG matcher
rewrite.

That stream is now implemented and accepted.  At 300 givens its logical
75,654-byte posting stream occupies 131,072 bytes of blocks; complete
back-demod storage falls from 758,016 to 497,952 bytes, and all 132,267 CHAT
candidate/hint/kept/given trace lines remain byte-identical.  At 1,000 givens
the 152,909 posting groups encode into 461,366 logical bytes and 524,288
allocated block bytes.  Complete back-demod storage falls from 6,033,664 to
3,414,304 bytes (-43.4%), making the four indexes plus shared pool
17,628,848 bytes: **60.4% below** the pre-structural 44,545,464 bytes.  The
exact gate takes 126.12 user seconds and 93,524 KiB peak RSS, improvements
over the preceding shared-pool run's 142.07 seconds and 94,652 KiB.

Rewrite overlap occurrences now use the same per-symbol block principle, with
one monotone varint rule delta per entry.  At 300 givens the 16,174 logical
bytes occupy 32,768 block bytes, the complete rewrite bank falls from 792,840
to 695,584 bytes, and the full 132,267-line CHAT trace remains identical.  At
1,000 givens the stream is 108,440 logical and 131,072 allocated bytes; the
rewrite bank falls from 4,856,072 to 3,939,616 bytes (-18.9%).  Combined
structural storage is now 16,712,392 bytes, **62.5% below** the pre-structural
baseline.  The gate remains exact at 125.53 user seconds and 92,164 KiB peak
RSS.

The append-only shared token arena now grows by 12.5% instead of doubling.
At 1,000 givens, 728,510 logical tokens use 3,025,656 capacity bytes rather
than 4,194,304, saving 1,168,648 bytes.  Seventy growths imply 24,196,880
bytes of worst-case predecessor copying.  The exact gate takes 133.58 user
seconds and 91,904 KiB peak RSS: slower than the 125.53-second doubled-arena
run, but below the accepted 140.45-second archived-cache gate.  The rejected
8.33% prototype saved only another 44,528 bytes while taking 142.86 seconds.
Combined structural storage is 15,543,760 bytes, **65.1% below** baseline.

The four stable-ID maps now use an 85% live-load threshold and rehash at the
same capacity when tombstones alone create pressure.  At 300 givens, the
rewrite map stays at 4,096 slots (49,152 rather than 98,304 bytes), and the
full CHAT trace remains identical.  At 1,000 givens, rewrite, unit, and
back-demodulation each stay at 32,768 slots, saving 393,216 bytes apiece.
Combined structural storage is 14,364,112 bytes, **67.8% below** baseline;
the exact gate takes 131.09 user seconds and 91,760 KiB peak RSS.

Rewrite rule records now pack the rule kind and active bit into the guarded
length word, reducing each record from 32 to 24 bytes without narrowing the
64-bit proof ID or 32-bit term offsets.  Rewrite and back-demod record arenas
also grow by 25% instead of doubling.  At 300 givens the five compact
structures occupy 2,399,132 bytes, **68.7% below** the original four-index
baseline, and all 132,267 candidate/hint/kept/given trace lines remain
byte-identical.  At 1,000 givens the rewrite bank is 3,162,936 bytes and the
back-demod bank is 2,279,608 bytes.  Together with the unit and nonunit
indexes and shared term pool, the structural total is 13,239,168 bytes,
**70.28% below** the 44,545,464-byte pre-structural baseline.  The exact
search state remains `Given=1001`, `Generated=1,268,285`, `Kept=33,909`,
`Usable=949`, `Sos=26,052`, `Demods=21,741`, and `Disabled=6,937`; the gate
takes 131.01 user seconds and 90,712 KiB peak RSS.  This completes the
radical index-reduction gate while retaining the old OTTER trajectory.

### Bounded proof-boundary result

The first long validation used the same full CHAT/Osborn clauses and hints,
with `max_given=4000`, `max_seconds=1200`, and `max_megs=512`.  It stopped at
the time bound without a proof.  The last complete report is `Given=2025`,
`Generated=4,572,040`, `Kept=148,200`, `Sos=71,683`, `Demods=60,912`, and
`Disabled=75,226`; all archive and authoritative-index validation failure
counts remain zero.  External timing is 1,154.57 user seconds, 48.45 system
seconds, 1,203.25 wall seconds, and 177,116 KiB peak RSS.

This result rejects a simple extrapolation from the 1,000-given gate.  It is
still a 60.2% RSS reduction from current FPA's 445,576-KiB proof run, but it
misses both the 957-second proof CPU gate and the 125-MiB final RSS gate.  At
the last report, the five compact structures occupy 58,121,952 bytes:
8,850,856 rewrite, 18,351,864 unit, 11,898,464 back-demod, 836,832 nonunit,
and 18,183,936 shared-term-pool bytes.  The ancestor archive has 44,308,928
logical record bytes in a 67,108,864-byte mapping; the packed hint index uses
29,282,780 bytes of nodes, references, and tables; dense selector metadata
and its cache account for about 10.3 MB; and the general allocator retains
36,706,624 reserved bytes.  These categories explain the measured resident
set to within mapping residency and ordinary process overhead.

The CPU diagnosis is sharper than the aggregate time: `back_demod` accounts
for 701.29 seconds, with 389.67 seconds in the nested preprocessing path,
while inference itself accounts for only 22.04 seconds.  Packed-hint
back-demodulation is 62.14 seconds, so hint lookup is not the dominant
regression.  By this point the 4-MiB materialization cache has 165,562 hits,
101,404 misses, 23,348 capacity evictions, and 74,453 invalidations.  Eager
backward rewriting repeatedly activates, preprocesses, and rearchives cold
clauses; compacting another small posting array cannot make this comparable
to resident-term FPA.  The next CPU experiment must therefore measure cache
capacity against decode/rearchive work, followed by a direct compact rewrite
pipeline if cache misses are not responsible for most of the gap.  In
parallel, inactive unit/back records and shared term-pool slices must be
reclaimed, and cold proof/archive pages must cease being permanently resident.

A three-way full-hint CHAT replay to exactly 1,500 givens rejects cache
capacity as the explanation.  With 0, 4, and 32 MiB configured, all runs end
at `Generated=2,947,136`, `Kept=66,933`, `Sos=42,583`, and `Demods=36,145`.
Their user times are respectively 366.96, 379.28, and 367.32 seconds, with
peak RSS of 114,468, 115,400, and 115,944 KiB.  The cached runs perform
50,644 ancestor materializations versus 148,691 with no cache, avoiding
98,047 decodes, yet are not faster.  Both nonzero settings reach only about
3.65 MB peak cache charge; the identical 328 evictions are set-associative
collisions, not exhaustion of either byte budget.  Thus increasing this
cache cannot repair proof throughput.  `compact_passive_cache=0` is the
current minimum-RAM setting, while the next diagnostic separates compact
index matching from the cost of retaining and reprocessing rewritten bodies.

That partition identifies the compact indexes, not archiving, as the main
CPU regression.  Full-body packed-fast OTTER with ordinary indexes reaches
1,500 givens in 154.79 user seconds and 179,400 KiB peak RSS.  The new
`new_otter_compact_full` CHAT case keeps the same full bodies but switches on
all four authoritative compact indexes; it takes 370.47 seconds and 140,048
KiB.  Its `back_demod` clock is 166.24 versus 4.91 seconds, unit `conflict`
is 18.62 versus 0.29, and `demod` is 77.44 versus 43.99.  Body
archive/materialization is therefore not responsible for the roughly 2.4x
slowdown at this boundary.

This comparison also exposes the first post-1,000 exactness question.  Both
runs finish with `Given=1501`, `Sos=42,583`, and `Demods=36,145`, but ordinary
indexes report `Generated=2,947,138`, `Kept=66,935`, and `Disabled=23,091`,
while combined authoritative compact indexes are lower by two in each of
those three cumulative/retired counts.  Four parallel full-hint audit runs
through the same boundary all pass: compact rewrite, unit, back-demod, and
nonunit answers agree with their ordinary counterparts on the ordinary
trajectory.  Their user times are 288.51, 185.97, 349.47, and 159.53 seconds,
respectively, versus 154.79 for ordinary packed-fast OTTER.  Thus the next
correctness step is a lightweight kept/given event comparison in combined
authoritative mode; the answer-set audits rule out treating this as a known
single-index omission.

The lightweight oracle shows that all 1,501 selected clause fingerprints are
identical and in the same order.  Local kept-clause order first permutes in
the hyper-resolution wave after given 412, so clause IDs subsequently differ
even though the selected formula trajectory does not.  The four standalone
answer audits still pass; the permutation is consistent with allocation- and
index-layout-sensitive legacy inference enumeration rather than a missing
compact candidate.  Exact formula/given behavior is preserved, but an
ID-identical proof remains an open acceptance item.

Back-index scan instrumentation supplies the immediate CPU target.  At only
413 givens, 7,068 back-demodulator queries yield 1,796 candidate clauses, but
the root-symbol posting implementation decodes 21,442,169 clause groups and
tests 41,497,798 occurrences.  The 23,100 occurrence probes per surviving
candidate explain why the compact `back_demod` clock grows from 4.91 to
166.24 seconds by given 1,500.  The next representation must restore bounded
path discrimination over the compact occurrence stream; cache sizing and
record packing cannot compensate for this loss of selectivity.

The first selective version stored a 16-bit fixed-path Bloom signature beside
each delta-packed
occurrence.  It encodes fixed `(path, symbol)` features through depth three;
pattern variables contribute no required bits, and every filter survivor is
still checked by the existing exact matcher, so collisions can produce only
false positives.  At 413 givens it rejects 41,365,527 of 41,497,798 probes
(99.68%) and reduces the back-demod clock from 3.26 to 0.70 seconds.  At
1,000 givens it rejects 475,277,784 of 476,702,937 probes, reduces total user
CPU from 131.01 to 103.27 seconds and `back_demod` from 41.29 to 11.31
seconds, with exact terminal state and 91,360 KiB peak RSS.  All 132,267 CHAT
candidate/hint/kept/given oracle lines remain byte-identical at 300 givens.
The back index grows by 524,320 bytes at the 1,000 boundary, making the five
compact structures 13,763,488 bytes, **69.1% below** the original
44,545,464-byte baseline.  Recovering roughly 400 KiB in the other compact
arenas will restore the 70% structural gate without giving back this 21.2%
CPU improvement.

Unit-conflict retrieval now links records by literal sign and root symbol,
using four bytes that previously fell inside each 32-byte record's padding.
The 1,000-given conflict clock falls from 4.32 to 0.20 seconds; total user CPU
is 102.78 seconds, and root heads add only 512 accounted bytes.  Terminal
state and the complete 132,267-line CHAT oracle remain exact.  This removes
the unit index's physical-record scan, although the saved clock is nested in
preprocessing and therefore changes aggregate CPU by only about half a
second in this prefix.

Rewrite and unit radix-node arenas now grow by 25% rather than doubling;
posting arrays remain unchanged because their measured occupancy is already
high.  At 1,000 givens the rewrite trie uses 40,355 nodes in 40,822 slots
(979,728 bytes instead of 1,572,864), and the unit trie uses 51,163 nodes in
63,783 slots (1,530,792 instead of 1,572,864).  The exact run takes 92.77
user seconds and 91,076 KiB peak RSS.  Including the path signatures and unit
root heads, the five compact structures now total 13,128,816 bytes,
**70.53% below** the original 44,545,464-byte baseline.  The focused rewrite
and unit tests pass, and the 300-given full CHAT trace remains byte-identical.

The combined fixes were then replayed to 1,500 givens with the full Osborn
hint set from `bob/chat_test.in`.  The full-body and zero-cache dense-archive
cases ran in parallel on separate physical CPUs.  Both stop normally at
`max_given` with the same compact terminal state: `Generated=2,947,136`,
`Kept=66,933`, `Sos=42,583`, `Demods=36,145`, and `Disabled=23,089`.  The
full-body case takes 254.36 user seconds and 143,480 KiB peak RSS; the dense
archive takes 256.53 seconds and 117,940 KiB.  Thus archiving adds only 0.9%
CPU at this boundary while saving another 25,540 KiB.  Relative to the
pre-filter full-body compact run, user CPU falls from 370.47 to 254.36
seconds (31.3%).  The ordinary packed-fast OTTER baseline is still much
faster at 154.79 seconds, however: the optimized archive is 1.66 times that
CPU.  Compact `back_demod` remains 63.66 seconds versus 4.91 seconds in the
ordinary index, and compact `demod` is 79.36 versus 43.99 seconds.  Those two
measured paths, rather than archive materialization or unit conflict lookup,
are the remaining throughput targets.

The next back-index layout moves the shallow signature in front of the
posting scan.  Occurrences are partitioned into sparse `(root symbol,
8-bit path signature)` buckets, and a query visits only buckets whose exact
stored signature contains every bit required by its fixed paths.  Bloom
collisions remain one-sided: they can admit extra work, but the unchanged
structural matcher checks every survivor, so no candidate can be lost.  Each
occurrence is stored in exactly one bucket, its signature byte is no longer
repeated in the occurrence stream, and 64-byte posting chunks grow by 25%.

The full-hint `chat_test.in` oracle is byte-identical across all 126,530
candidate/hint/kept/given lines at 300 givens, and the focused and
compact-vs-legacy audit suites pass.  At that boundary the back index is
610,440 bytes and `back_demod` is 0.19 seconds.  At 1,000 givens the
zero-cache dense archive again has the exact terminal state; it takes 103.31
user seconds, peaks at 91,048 KiB RSS, and spends 3.99 seconds in
`back_demod`.  The back index is 2,961,192 bytes.  The five compact structures
now total 13,286,080 bytes, **70.17% below** the 44,545,464-byte baseline,
while this complete prefix remains 21.1% faster than the earlier 131.01-second
packed-record archive.  A parallel full-body replay reaches the same state in
101.87 seconds and 103,572 KiB peak RSS.

At 1,500 givens the same parallel CHAT comparison confirms that the gain
survives the larger passive population.  Full-body compact OTTER takes 194.09
user seconds and 142,996 KiB peak RSS; the zero-cache dense archive takes
202.51 seconds and 115,752 KiB.  These are 23.7% and 21.1% faster than their
respective pre-bucket 254.36/256.53-second runs.  Both end at the exact compact
state (`Generated=2,947,136`, `Kept=66,933`, `Sos=42,583`,
`Demods=36,145`, `Disabled=23,089`).  The archive `back_demod` clock falls
from 63.66 to 16.03 seconds.  Ordinary packed-fast OTTER still takes only
154.79 seconds, so full-body compact is 1.25 times and the radical archive is
1.31 times ordinary CPU.  Compact demodulation is now the principal measured
gap: the archive spends 78.61 seconds there versus 43.99 in the ordinary
baseline.

Compact rewrite retrieval now uses the ordering already maintained by the
radix trie.  At each node, variable-leading siblings are tried in their
existing order, rigid siblings below the subject root are skipped, the one
equal rigid edge is tried, and traversal stops before larger roots.  Skipped
edges cannot match their first token, so posting order and rewrite semantics
are unchanged.  The compact component suites, compact-vs-legacy audit, and
the full 300-given CHAT oracle pass.  At 1,000 givens the full-body and dense
archive cases retain the exact state and take 75.54/78.33 user seconds,
versus 101.87/103.31 before the change.  Their `demod` clocks fall from
35.74/35.48 to 19.48/19.32 seconds, with peak RSS unchanged at 103,572/91,024
KiB.

At 1,500 givens the ordered-sibling gain also survives: full-body and archive
take 167.73/176.85 user seconds, 13.6%/12.7% below the preceding
194.09/202.51-second path-bucket runs.  Both preserve the exact compact state
and their peak RSS remains 142,996/115,752 KiB.  The archive `demod` clock
falls from 78.61 to 53.60 seconds.  Relative to the 154.79-second ordinary
packed-fast baseline, full-body compact is now 1.08 times and the radical
archive is 1.14 times ordinary CPU, inside the Phase 5 1.25 throughput gate.

The first bounded full proof with all accepted compact features succeeds at
given 2,945.  It generates 8,248,032 clauses, keeps 272,787, and produces the
same 7,051-step proof with 3,231 new hints as ordinary OTTER; the two-count
difference from the ordinary cumulative generated/kept totals is the already
audited transient kept-order permutation.  `prooftrans parents_only` exits
successfully and reconstructs the final `$F` clause.  End-to-end time is
919.70 user seconds, 92.24 system seconds, and 1,012.62 wall seconds, with
277,160 KiB peak RSS.  This is 1.20 times the old 765.69-second proof and
therefore passes the 1.25 CPU gate, but it saves only 49.6% of the old
550,400-KiB RSS and does not pass the radical final memory target.

The final statistics make the remaining RAM concrete.  Rewrite, unit,
back-demod, nonunit, and shared term-pool structures total 109,582,648 bytes
(104.5 MiB).  The unit index retains 266,794 physical records for 128,653
active clauses, while back-demod retains 272,486 for 132,801 active clauses;
the shared pool still contains all 272,786 serialized clauses.  The ancestor
store has 84,404,096 logical record bytes in a 134,217,728-byte mmap, packed
hints retain their roughly 29-MB tables/cache/postings, and the allocator has
35,658,048 bytes currently reserved.  The next RAM pass must therefore
compact unit/back records and the shared pool together, and make cold ancestor
mmap pages genuinely evictable.  Further passive-body micro-tuning cannot
recover the required roughly 150 MiB.

The mmap ancestor store now maintains record/body counters incrementally,
instead of decoding and checksumming every archived record whenever
STATISTICS is printed.  Once the append-only file reaches 16 MiB, each
additional 8 MiB step synchronizes newly cold complete pages and applies
`MADV_DONTNEED` to the cold prefix, retaining an 8 MiB writable hot tail.
Random proof and materialization reads still use the same validated
`MAP_SHARED` records.  Memory and mmap ancestor tests pass; a 200,000-record,
22.2-MB mmap test evicts an 8-MiB prefix and tears down with an empty ID table.
The 1,000-given CHAT archive is below the first threshold at 7,973,611 record
bytes, preserves the exact state, takes 71.61 user seconds, and peaks at
91,028 KiB RSS.  The lower CPU versus 78.33 seconds reflects removal of the
periodic full archive scan; the full proof is needed to measure RSS eviction.

Unit and back-demod indexes now rebuild in stable record order whenever stale
physical records reach 25% of the active population (with a 1,024-record
floor).  Unit radix/posting/root state is regenerated from live records while
retaining the shared token offsets.  Back-demod rebuild copies only live
delta-coded occurrence slices into new sparse path buckets, without clause
materialization.  At 300 givens each index compacts once: unit bytes fall from
a 633,832-byte peak to 428,904, and back-demod falls from 610,456 to 561,304.
All 126,530 CHAT oracle lines remain byte-identical, and focused plus
compact-vs-legacy audit tests pass.  Shared pool tokens are deliberately not
reclaimed by this commit; offset rebasing is the next isolated layer.

Shared-pool compaction now coordinates that missing layer.  Before replacing
the append-only pool it forces rewrite, unit, and back-demod records to their
live subsets, copies every retained clause serialization exactly once, builds
an old-to-new token-interval map, and rebases rule sides, radix labels, and
record offsets before freeing the old pool.  The operation is proof-ID based;
no archived clause is materialized and all index insertion order is preserved.
`Compact_term_pool` statistics report cumulative compaction and reclaimed-byte
counts.

The first clause-ratio trigger was measured and rejected: it rebuilt all
three indexes at 300 givens to reclaim only 45,392 pool bytes, raising the
1,000-given user time from 71.61 to 88.02 seconds without reducing peak RSS.
The accepted trigger still requires at least 25% apparently stale clauses,
but additionally estimates their serialized token payload and requires a
conservative 8-MiB recovery (directory and index savings are not counted).
Consequently the exact 1,000-given CHAT replay performs no coordinated
compaction and takes 71.95 user seconds, 13.18 system seconds, and 85.22 wall
seconds at 92,068 KiB peak RSS.  This is +0.5% user CPU versus the 71.61-second
accepted ancestor-eviction baseline.  Its candidate, hint, kept, and given
trace is byte-identical, and the focused pool/rewrite/unit/back tests plus the
compact-vs-legacy audit pass.  The full proof boundary is the intended
measurement: only there does the previous 33.6-MB pool contain enough stale
payload to justify a global rebuild.

That bounded full proof succeeds at the unchanged boundary and validates the
transaction under real pressure.  It reaches `Given=2,945`,
`Generated=8,248,032`, and `Kept=272,787`; `prooftrans parents_only`
reconstructs the same 7,051-step proof through clause `272791 $F`.  One
coordinated pool compaction fires near given 2,700 and reports 8,865,168 bytes
reclaimed.  The final rewrite, unit, back-demod, nonunit, and shared-pool
structures occupy 67,621,976 bytes (64.49 MiB), down 38.3% from the preceding
proof's 109,582,648 bytes.  All compact-index and archive validation failure
counts remain zero.

End-to-end time improves to 807.22 user seconds, 95.66 system seconds, and
903.20 wall seconds.  That is 1.054 times the 765.69-second ordinary proof and
0.878 times the preceding 919.70-second compact proof.  Peak RSS, however, is
271,684 KiB: only 2.0% below the preceding 277,160 KiB, and a 50.6% saving
from old OTTER's 550,400 KiB rather than the required 80--90%.  The ancestor
store reports nine eviction passes and 377,487,360 cumulative advised bytes,
but 641,248 later materializations can fault cold MAP_SHARED pages resident
again.  The small peak-RSS response despite 42.0 MB less final compact
metadata rejects further index packing as the next radical lever.  The next
experiment must move the ancestor archive to bounded `pread`/`pwrite` I/O (or
measure and eliminate coordinated-compaction transient peaks); mapped cold
pages cannot be treated as nonresident merely because they were advised once.

`ancestor_store=file` implements that experiment without changing the record
format or archived-ID contract.  Appends use `pwrite`; materialization uses
`pread` through one grow-on-demand record buffer; and the anonymous temporary
file is never mapped into the process.  The current disabled-record count is
also maintained incrementally, and teardown reads only fixed headers instead
of complete obsolete records.  Memory, mmap, and file 200,000-record scale
tests pass; file mode stores 22,200,000 logical record bytes with a 4,096-byte
I/O buffer and the same roughly 3.8-MB resident handle/ID accounting.

Parallel 300/1,000-given CHAT replays and the compact-vs-legacy audit pass, with
byte-identical candidate, hint, kept, and given traces.  A controlled
1,000-given file run takes 74.63 user seconds and peaks at 90,632 KiB, versus
71.95 seconds and 92,068 KiB for mmap: +3.7% CPU while the archive is still
only 7.97 MB.  It performs 40,819 record writes and 154,834 bounded reads with
zero validation failures.  The full proof is required to measure the intended
benefit once mmap would otherwise retain/refault an 84.4-MB logical archive.

The file-backed full run proves Osborn at the exact final state:
`Given=2,945`, `Generated=8,248,032`, `Kept=272,787`, `Sos=131,001`, and
`Demods=109,987`.  Normalizing away clause numbers, all 7,051 transformed
proof clauses are byte-identical to the mmap proof, and `prooftrans
parents_only` again ends at `272791 $F` with no diagnostics.  The archive
contains 84,404,096 logical bytes but keeps only a 4,096-byte record buffer;
1,340,586 read operations transfer 125,362,225 bytes and 412,174 writes
transfer exactly the archive bytes, with zero validation failures.

End-to-end time is 786.23 user seconds, 98.79 system seconds, and 885.26 wall
seconds.  This is 1.027 times old OTTER and 0.974 times the compact mmap run.
Peak RSS is 188,736 KiB, a 30.5% reduction from mmap's 271,684 KiB and a 65.7%
reduction from old OTTER's 550,400 KiB.  Two-second `/proc` sampling shows a
brief 189-MiB rebuild peak followed by roughly 177 MiB steady RSS; the 125-MiB
stretch gate is still missed by about 59.3 MiB.

With archive residency removed, the remaining targets are resident and
measured: the five compact search structures total 67,621,976 bytes (64.49
MiB), packed hint bodies/index total about 24.34 MiB, and the Prover9 allocator
reserves 35,658,048 bytes for 20,105,440 live bytes (15,552,608 bytes reported
fragmentation).  Further archive work cannot close the gap.  The next radical
pass must combine cross-clause term sharing/capacity tightening, smaller or
returnable allocator slabs, and a lower-residency packed-hint representation;
it must also eliminate the short rebuild double-allocation peak.

The first resident follow-up reduces allocator slabs from 1 MiB to 256 KiB.
Pointer ownership remains a mask operation because the new size is also a
power of two; empty slabs are still unmapped immediately and the public
pointer-count API is unchanged.  At 1,000 givens allocator reservation falls
from 32,512,320 to 21,502,272 bytes and fragmentation from 14,747,728 to
3,737,680 bytes.  The exact 300-given CHAT trace, 1,000-given terminal state,
and allocator/lifecycle/audit tests pass.  Controlled 1,000-given user CPU
rises from 74.63 to 84.38 seconds (+13.1%), still
inside the final 957-second gate, while current RSS falls by 3.65 MiB.  Peak
prefix RSS is dominated by initialization and therefore does not measure the
expected late-proof saving.

Persistent compact arrays now use 25% growth for unit records/postings,
rewrite postings, and the back-demod occurrence stream.  At 1,000 givens this
already removes 187,420 bytes net despite a unit-capacity boundary landing
slightly above the old power of two.  The 300-given CHAT trace, the
1,000-given terminal search state, and all focused audits are exact.  The
combined controlled run takes 92.62 user seconds, 1.241 times the accepted
74.63-second file-backed reference and therefore just inside the 1.25 prefix
gate.  At the measured full boundary the same growth sequences project
roughly 5.5 MiB less allocated capacity, which must be confirmed rather than
counted as achieved before the next proof run.

That full proof run is now complete.  It reaches the exact state
`Given=2,945`, `Generated=8,248,032`, `Kept=272,787`, `Sos=131,001`, and
`Demods=109,987`; all 7,051 normalized `prooftrans parents_only` clauses are
byte-identical to the accepted file-backed proof and end at `272791 $F` with
no diagnostics.  Time is 804.04 user, 104.10 system, and 908.43 wall seconds,
only 1.023 times the accepted user-CPU result and safely inside the 957-second
CPU and 18-minute wall gates.

Peak RSS is 178,704 KiB, 10,032 KiB (5.3%) below the preceding 188,736-KiB
file-backed result but still 50,704 KiB above the 125-MiB gate.  The five
compact structures fall from 67,621,976 to 63,197,244 bytes (-4.22 MiB), and
final allocator reservation falls from 35,658,048 to 24,123,712 bytes
(-11.00 MiB).  Internal current RSS falls by 9,856 KiB.  The fact that RSS
does not fall by the sum of the capacity reductions is expected: much of the
old unallocated slab capacity was virtual but untouched, while tighter growth
copies and touches predecessor arrays.  A two-second sample also catches a
late rebuild peak before compaction releases pages.  This slice is accepted
as an exact reduction, but it does **not** close the Phase-5 RSS gate.

The next packed-hint cleanup removes two better-mode structures that were
provably dead after construction: the legacy 128-feature bitset and the
legacy rewrite-symbol array.  Collective preview candidate/intersection
scratch is now lazy and separately reported, so ordinary OTTER does not pay
for a second full workspace.  Persistent packed storage falls by 5,767,424
bytes (5.50 MiB) at both CHAT boundaries.  The 300-given oracle retains all
126,530 exact event records; the isolated 1,000-given run retains the exact
terminal state, reduces current RSS from 80,284 to 73,540 KiB, and improves
user CPU from 92.62 to 87.26 seconds.  Dedicated preview, hint trace,
checkpoint, collective scheduler, hint-postings, and compact audit tests all
pass.  Full-proof RSS remains to be measured before counting the saving at
the acceptance boundary.

The full packed-cleanup proof reaches the exact 2,945-given state, and all
7,051 normalized proof clauses are byte-identical to the accepted proof.
It takes 796.01 user, 105.39 system, and 901.71 wall seconds.  Peak RSS is
170,968 KiB, 7,736 KiB below the preceding 178,704-KiB result and 68.9% below
old OTTER's 550,400 KiB, while user CPU is only 1.012 times the accepted
786.23-second file-backed boundary.  The result still exceeds the 125-MiB
gate by 42,968 KiB.  A two-second sample peaks at 166,512 KiB and therefore
also confirms that the external maximum includes a shorter allocation or
rebuild transient.  The packed cleanup is accepted, but Phase 5 remains open.

To avoid guessing at the remaining 42-MiB gap, statistics now report Linux
`smaps_rollup` classes and glibc `mallinfo2` arena/mmap state.  At the exact
1,000-given CHAT boundary, 70,356 of 73,496 KiB is anonymous.  Glibc reports
a 29,949,952-byte arena, 23,912,448 malloc-mmap bytes, and 8,613,616 free
arena bytes; the slab allocator separately reserves 21,502,272 bytes.  Only
131,824 libc bytes are the top releasable block, so simply calling
`malloc_trim` cannot be credited with the full 8.6-MiB arena slack.  The
300-given full trace and 1,000-given terminal state remain exact, and the
memory lifecycle, allocator churn, and bookkeeping tests pass.

The packed-fast cache working set is also much smaller than its original
32,768-entry allocation.  An 8,192-entry cache removes 4,521,984 persistent
bytes while changing the 1,000-given hit rate only from 35.88% to 35.57%.
The 300-given full trace and 1,000-given terminal state are exact; isolated
1,000-given current RSS falls from 73,496 to 69,140 KiB and user CPU changes
from 84.33 to 84.07 seconds.  This exact reduction is accepted for a full
proof measurement; it cannot by itself close the remaining RSS gap.

The full proof rejects that cache reduction.  Although the exact proof and
all 7,051 normalized clauses remain unchanged, the 8,192-entry run takes
816.35 user seconds versus 796.01 and peaks at 170,372 versus 170,968 KiB:
only 596 KiB is saved.  Its final glibc arena contains 41,995,552 free bytes,
showing that extra miss/materialization churn turns the nominal 4.52-MiB
table saving into persistent heap fragmentation.  The cache is restored to
32,768 entries; this is a measured rejection, not an accepted Phase-5 gain.

Keeping that cache and setting glibc's trim threshold to zero produces the
opposite full-run result.  The exact 2,945-given/7,051-clause proof takes
788.08 user and 886.05 wall seconds and peaks at 152,616 KiB, down 18,352 KiB
from 170,968 with no CPU penalty.  Its sampled compaction peak is 141,072 KiB
and final internal RSS is 132,096 KiB.  The glibc arena stays near 24.9 MB
while large arrays remain in independently returnable malloc-mmap regions,
rather than growing past 100 MB with tens of megabytes stranded free.

`P9_COMPACT_HEAP=1` now requests the same policy through `mallopt` at the
start of `prover9` and reports `compact_policy=enabled` when honored.  At the
exact 1,000-given CHAT boundary, the P9 switch, raw glibc tunable, and both
together have identical search states and essentially identical memory
breakdowns.  The full P9-switch proof is also exact: it reaches
`Given=2,945`, `Generated=8,248,032`, and `Kept=272,787`, and all 7,051
normalized proof clauses are byte-identical to the accepted reference.  On
the slower validation run it takes 833.73 user, 103.30 system, and 937.35 wall
seconds and peaks at 152,904 KiB.  That is only 288 KiB above the raw-tunable
peak and remains below the 957-second CPU gate, so `P9_COMPACT_HEAP=1` replaces
the raw tunable as the product acceptance boundary.  It is still 24,904 KiB
above the 125-MiB gate.

The 2-second sampler catches 141,156 KiB while the external high-water mark
catches 152,904 KiB.  Immediately before the one coordinated term-pool
compaction, resident memory has already grown to roughly 140 MiB; the
replacement pool/rebase transaction then creates the short additional peak.
The next slice therefore targets both causes: retain and rebase the live token
intervals in place, and test an earlier bounded reclaim threshold so stale
payload cannot regrow to the same pre-compaction high-water mark.

The first half is now implemented.  A bitset marks retained source-directory
slots, the corresponding nonoverlapping token intervals are sorted and moved
downward in the existing array, and the directory is rebuilt before rewrite,
unit, and back-demod offsets are rebased.  There is no replacement pool, and
the temporary mark set is one bit per source-directory slot.  A configurable
`compact_term_reclaim_kb` keeps the accepted 8-MiB default while allowing
bounded earlier-trigger experiments.

Parallel 300-given `chat_test.in` runs validate the actual coordinated search
path.  The 8-MiB control performs no pool compaction; a forced 32-KiB run
performs one in-place compaction and reclaims 45,392 bytes.  Both emit exactly
132,567 candidate/hint/kept/given trace lines with SHA-256
`bc38f369ef02271d9b8a00db0cd01a6ba8f890e9608e0abd947de68f763322aa`.
User time is 18.04 versus 18.32 seconds, and every compact-index validation
failure remains zero.  This clears the semantic gate; larger prefixes must
now choose a useful threshold before another full proof.

At 1,000 givens the same parallel 8-MiB/32-KiB pair ends at the identical
`Generated=1,268,285`, `Kept=33,909` state.  The forced run retains its one
early compaction and takes 85.10 user seconds versus 87.17 for the control;
in-place rebasing has no measurable continuation penalty.

The threshold separates at a bounded 2,000-given comparison:

| Reclaim threshold | Pool compactions | Pool bytes reclaimed | User | Peak RSS |
|---:|---:|---:|---:|---:|
| 8 MiB | 0 | 0 | 320.72 s | 142,892 KiB |
| 4 MiB | 1 | 4,982,388 | 312.87 s | 137,472 KiB |
| 2 MiB | 3 | 9,252,684 | 314.31 s | 137,604 KiB |

All three finish at `Generated=4,497,690`, `Kept=144,512` with the same rule
counters.  Four MiB removes about 5.3 MiB of peak RSS without a CPU penalty.
Two MiB leaves a smaller final pool but its two extra coordinated rebuilds do
not lower the external high-water mark at this boundary.  The full-proof gate
must compare 8, 4, and 2 MiB directly: 4 MiB is the bounded-prefix leader,
while 2 MiB remains useful for determining whether later pool regrowth, rather
than the first compaction, sets the final high-water mark.

The three full proofs are exact.  Each reaches `Given=2,945`,
`Generated=8,248,032`, and `Kept=272,787`; each reconstructed 7,051-clause
proof is byte-identical to the accepted reference.  CPU remains essentially
flat despite the extra rebuilds:

| Reclaim threshold | Pool compactions | User | Sampled peak | External peak | Final PSS |
|---:|---:|---:|---:|---:|---:|
| 8 MiB | 1 | 741.29 s | 150,180 KiB | 153,880 KiB | 129,960 KiB |
| 4 MiB | 2 | 741.18 s | 145,192 KiB | 153,040 KiB | 129,838 KiB |
| 2 MiB | 5 | 743.11 s | 142,640 KiB | 147,980 KiB | 123,724 KiB |

The 2-MiB steady state is already 4,276 KiB below the formal 128,000-KiB
gate, but the process high-water mark still misses it by 19,980 KiB.  The
0.2-second trace locates the decisive late overlap: after the empty clause is
found, `get_clause_ancestors()` materializes the complete 7,051-clause
archived proof while all compact search indexes and the shared term pool are
still resident.  A second, shorter gap between the sampled and `time -v`
maxima occurs within the same reconstruction/printing transaction.

The next accepted slice is therefore terminal-only and cannot change search
semantics: determine nonredundancy from archived ID/parent/negative metadata,
freeze the final statistics, release compact rewrite/unit/nonunit/back-demod
indexes and their shared term pool, and only then reconstruct and print the
proof DAG.  The frozen report must retain the pre-release index accounting,
and all 7,051 proof clauses must remain exact.

That terminal lifetime split is implemented.  Archived parent and negativity
metadata identify the same first denial without constructing the proof DAG;
the complete final statistics and selector report are frozen to a temporary
stream; compact search indexes and the shared pool are released; and the
proof is then materialized.  The deferred report is emitted in its original
post-proof position, and `collect_prover_results` retains the frozen component
counters.  The compact archive proof audit and all of its normalized proof
comparisons pass.

A distinct mid-search spike came from growing the shared token array with
`realloc`.  Linux now stores it in an anonymous mapping and uses
`mremap(MREMAP_MAYMOVE)` for zero-copy capacity changes; non-Linux builds keep
the portable fallback.  The stale-payload byte budget is authoritative after
a 1,024-clause noise floor, replacing the 25%-stale guard that delayed a
configured reclaim past the next growth boundary.  A forced 32-KiB,
300-given `chat_test.in` replay again emits the exact 132,567-line oracle,
performs one compaction, takes 17.95 user seconds, and reports
`token_copy_bytes=0`.  The terminal release plus the 2-MiB policy now require
one final exact full-proof/RSS measurement.

That combined 2-MiB measurement is logically exact and fast, but it rejects
the current peak policy.  It reaches `Given=2,945`,
`Generated=8,248,032`, and `Kept=272,787`; all 7,051 normalized proof clauses
are byte-identical to the accepted reference.  It takes 736.77 user, 98.82
system, and 836.27 wall seconds, so it is faster than old OTTER's 765.69 user
seconds and comfortably inside both time gates.  The terminal lifetime split
also works: sampled RSS falls from 131,684 to 74,788 KiB before archived proof
materialization begins.

Peak RSS nevertheless reaches 151,080 KiB externally and 147,124 KiB in the
0.2-second sampler.  The sampled maximum lasts for only three observations
inside the fifth coordinated term-pool compaction: its surrounding resident
set is about 139,032 KiB, temporary rebase/directory work raises it above 147
MiB, and compaction then lowers it to 132,600 KiB.  Thus terminal proof overlap
is no longer the maximum.  The remaining work is precisely bounded: trigger
reclaim before the 139-MiB pre-compaction state and eliminate growth copies in
the temporary rebase-entry vector, then repeat the exact full gate.

The rebase vector now uses the same private anonymous mapping and
`mremap(MREMAP_MAYMOVE)` growth policy as the token array on Linux, with the
portable `realloc` fallback retained and copy bytes reported explicitly.
The forced 32-KiB, 300-given `chat_test.in` run performs one real coordinated
compaction and reports eight rebase-vector growths with
`rebase_copy_bytes=0`.  Its 132,567-event trace remains byte-identical to the
oracle (SHA-256 `bc38f369ef02271d9b8a00db0cd01a6ba8f890e9608e0abd947de68f763322aa`),
and the compact archive proof audit passes.  Parallel 1-MiB and 2-MiB CHAT
runs also reach the exact 1,000-given state without firing; the reclaim
threshold therefore needs a later bounded prefix to choose between them.

The retained-slot bitset now serves as an exact first pass over the union of
the three compact indexes.  Instead of doubling the rebase vector to the next
power of two while that union is discovered, compaction counts marked slots
and maps the vector once at its final size.  The forced CHAT replay remains
the same 132,567-event oracle and its real compaction now needs one rebase
growth rather than eight, still with zero copied bytes.

At 1,500 givens, parallel 512-KiB and 1-MiB `chat_test.in` runs finish at the
identical `Generated=2,947,136`, `Kept=66,933` state and exact rule/demodulation
counters.  The 512-KiB policy performs three compactions, reclaims 2,046,068
pool bytes, and takes 177.67 user seconds with 101,788-KiB peak RSS.  One MiB
performs one compaction, reclaims 1,017,164 bytes, and takes 181.34 user
seconds with 101,952-KiB peak RSS.  Neither adds CPU cost, but this boundary
does not distinguish their peaks; a 2,000-given comparison is the last
bounded filter before choosing the full-proof policy.

That 2,000-given filter selects 512 KiB: both policies finish at the exact
`Generated=4,497,690`, `Kept=144,512` state, but 512 KiB takes 320.93 user
seconds and peaks at 136,476 KiB versus 329.59 seconds and 138,628 KiB for one
MiB.  Its final PSS is only 122,393 KiB.  Inspection shows why the external
peak remains higher: every pool reclaim first forced replacement rebuilds of
all three compact indexes even though each index already has its own bounded
25%-stale policy.

Pool reclamation now counts physical—not merely active—index records and
preserves all of their shared token intervals.  Rewrite, unit, and back-demod
indexes compact independently at their existing deterministic thresholds;
the pool no longer rebuilds all three merely to discard unrelated token
clauses.  A 32-KiB 300-given CHAT replay remains byte-identical to the
132,567-event oracle, the compact archive audit passes, and the one pool
compaction reports zero reclaimed capacity because this small prefix stays
within the same token/directory size classes.  The next 2,000-given run must
measure whether removing the forced replacement overlap closes the transient
gap while retaining the selected 512-KiB policy.

That decoupled 2,000-given run remains exact and takes 316.80 user seconds,
but it does not lower the peak: external RSS is 136,448 KiB and the
0.2-second sampler observes 136,764 KiB.  Final PSS is only 125,229 KiB.  The
high-frequency trace rises gradually for several seconds and then drops by
about 30 MiB at an index-replacement commit; the remaining high-water mark is
therefore the old/new replacement overlap inside an independently required
stale-index compaction, not the term pool transaction.

`compact_index_stale_pct` now makes the rebuild point measurable instead of
hard-coding 25%.  It applies the same inactive/active percentage to compact
rewrite, unit, and back-demod indexes and is reported as
`Compact_index_policy`.  Higher values trade bounded inactive postings and
extra exact-filter traversal for fewer replacement transactions.  At 100%,
the 300-given CHAT trace remains the byte-identical 132,567-event oracle and
performs no index rebuild; bounded 50%/100% runs must determine whether the
larger steady state is still below the avoided replacement peak.

The higher thresholds are rejected by the 2,000-given comparison.  Both are
exact, but 50% takes 351.66 user seconds and peaks at 140,776 KiB; 100% takes
350.55 seconds and peaks at 143,584 KiB.  The accepted 25% policy takes
316.80 seconds and peaks at 136,448 KiB in the corresponding decoupled run.
At 100%, final PSS rises to 132,585 KiB as inactive unit/back-demod records
and their term clauses accumulate.  Delaying replacement therefore loses on
both CPU and memory; `compact_index_stale_pct` remains an experimental audit
control with its 25% default.  The next implementation must reduce the
old/new overlap itself, beginning with rebuilding the unit index from a
packed record snapshot after releasing its predecessor search structures.

That unit-index lifetime split is implemented.  Live records are compacted
downward in their existing array, all predecessor nodes/postings/root heads,
hashes, and query scratch are released, and the replacement is rebuilt in
the same record order while only the packed record array overlaps it.  The
shared term pool and ownership semantics are preserved.  The compact archive
audit passes, and the 300-given CHAT replay again emits the byte-identical
132,567-event oracle; a bounded peak measurement must determine how much of
the late replacement transient belonged to the unit index before applying
the same lifetime split to the more involved back-demod occurrence stream.

Rewrite-bank compaction now uses the same lifetime discipline.  Live rule
records are packed in original order, predecessor radix/posting/occurrence
and hash arrays are released, and the replacement is rebuilt from the packed
rules plus the shared token pool.  A private owning bank retains its source
pool only until token copying completes.  Dedicated rewrite/unit tests and
the compact archive audit pass; the 300-given CHAT oracle remains exact.

Back-demod compaction now has a bounded-memory archive path as well.  It
snapshots only active stable proof IDs, releases every predecessor posting,
path, occurrence, record, hash, and result array, then materializes at most
one active clause at a time in original record order and rebuilds against the
unchanged shared term pool.  The encoded-stream implementation remains the
standalone fallback when no resolver is configured.  Dedicated back-demod,
rewrite, and unit tests pass.  The actual 300-given dense CHAT path performs
one materialized rebuild and preserves the byte-identical 132,567-event
oracle, although user time rises from roughly 18 to 21.6 seconds; the
2,000-given CPU/RSS gate must decide whether the eliminated old/new overlap
justifies that archive-decode cost.

### Next radical index reduction

The next implementation slice is structural, not another cache-size tweak:

1. **First layer completed:** rewrite rules, unit matching, and passive redex
   occurrences share one clause serialization and retain 32-bit offsets
   instead of private flattened token copies.  Cross-clause hash-consing of
   equal subterms remains to be implemented and measured; its hash/directory
   overhead is included in the reported total.
2. **Completed for the existing token arenas:** replace token-per-node unit
   and rewrite discrimination paths with radix edges.  Unary paths become one
   edge; child order and terminal posting order remain the audited legacy
   order.  Sharing those labels through item 1 remains outstanding.
3. **Completed:** radix-compress the fixed-length nonunit feature trie.  Its
   node pool fell by 96.1% at 300 givens without adding exact probes.
4. **Completed:** group back-demod occurrences by `(symbol, clause ID)` and
   delta-pack matching subterm offsets.  The existing structural exact filter
   remains, while the 12-byte posting per raw symbol occurrence and the
   redundant per-argument root directory are gone.
5. **Partly completed:** compact inactive rewrite, unit, and back-demod records
   at deterministic thresholds and coordinate their shared-token offset
   rebasing.  Rebuilds preserve result ordering and pass the CHAT event oracle.
   A single packed stable-ID directory shared by all indexes remains optional
   future work.
6. **Completed for mmap:** synchronize newly cold archive pages and advise the
   kernel to discard cold prefixes while preserving an 8-MiB writable hot
   tail.  A true `pread`/`pwrite` backend remains a fallback if full-proof RSS
   shows that the kernel does not honor the advice adequately.

The gate for this slice is at least a 70% reduction of combined rewrite,
unit, nonunit, and redex-index bytes at 1,000 givens, with the exact 300 trace
unchanged and 1,000-given CPU no worse than the current archive run.  That is
large enough to matter at the proof boundary; 5--10% container tuning is not
accepted as completion.

## Why selected DISCOUNT did not solve Osborn

At the old proof boundary OTTER has processed 2,945 givens, retains 131,001
SOS clauses, and proves the problem in 764.94 user seconds with 536.99 MiB of
Prover9-accounted memory.  The guarded packed-fast DISCOUNT run reaches Given
2,916 in 330.26 user seconds, but already retains 569,005 passives and has not
found the proof.  By Given 4,111 it has 1,046,419 passives and uses 198,112
KiB peak RSS.

Packed-fast has removed hint lookup as the cause: at the 1,000-given exact
DISCOUNT boundary it reduces ordinary/flipped hint time from 353.586 to 7.406
seconds without changing the selected clause.  The remaining divergence is
semantic timing.  OTTER eagerly factors, installs demodulators, performs
backward subsumption/demodulation/unit deletion, and exposes every retained
SOS clause to forward simplification before it is selected.  DISCOUNT delays
those operations until selection.  A hint count therefore cannot stand in
for the old proof trajectory.

## Compatibility contract

For each newly retained clause, compact OTTER performs the same transaction
and in the same order as the reference loop:

1. simplify, orient, merge, and apply unit/CAC simplification;
2. perform exact forward subsumption and unit conflict;
3. weigh and perform exact hint matching/degradation;
4. assign the same clause ID and append it to limbo;
5. factor and apply the other limbo generators;
6. perform backward subsumption;
7. if eligible, install it immediately as a demodulator and back-demodulate
   both active and passive clauses;
8. perform backward unit deletion/CAC processing;
9. insert the unchanged survivor into the given selector.

Only after this transaction may an SOS body become cold.  Selection restores
the exact body and metadata without a refresh/requeue step.  Therefore given
selection and inference ordering remain OTTER ordering rather than DISCOUNT
ordering.

Stable clause ID is the public identity.  No evictable term, literal,
`Topform`, or mmap address may be retained by a selector or compact index.
All candidate lists are ordered to reproduce the reference index's answer
order before an operation with visible effects is performed.

## Representation

### Active and transient clauses

Usable, limbo, the current given, and bounded query scratch retain ordinary
materialized clauses and the existing inference indexes.  The active set is
small on Osborn (1,800 clauses at the measured proof boundary), so replacing
it is neither necessary nor worth compatibility risk.

### Cold passive records

Each SOS survivor has an append-only body record and a compact mutable
sidecar.  The body record owns literals, attributes, and justification.  The
sidecar owns only selection/query metadata: stable ID, body position, exact
weight key, matching-hint ID, selector membership, liveness, and compact
feature roots.  Fields needed only for reports are read from the body header
instead of duplicated in every resident sidecar.

The body arena gets a true file-I/O backend.  The current writable `MAP_SHARED`
arena makes every appended page resident in the process and explains much of
the 198-MiB guarded peak.  The file backend uses bounded write/read buffers and
`pwrite`/`pread`; kernel page cache is not charged as process RSS.  The mmap
ancestor store remains separate and proof-complete.

### Compact exact indexes

The implementation is split so each structure can be differentially tested:

- a packed unit index retrieves forward subsumers, backward subsumees, unit
  conflicts, and unit-deletion candidates using structural postings followed
  by allocation-free exact compressed matching;
- a compact nonunit feature index stores stable IDs and materializes only the
  conservative candidate set for the existing exact subsumption test;
- the existing compact rewrite trie stores demodulator sides in reference
  insertion order and returns the same first applicable rule as the ordinary
  demodulation index;
- a compact redex occurrence index maps conservative symbol/path features to
  passive IDs for back demodulation, followed by the ordinary exact rewrite
  eligibility check on a materialized candidate;
- selector heaps contain compact record ordinals, never clause pointers.

Tombstones preserve stable ordinals.  Rebuilds/compactions may change physical
positions only after all ID-based indexes and selector entries have been
relocated atomically.  A bounded materialization cache is an optimization,
not an owner; eviction cannot change semantics.

## Implementation stages

### Stage 0: frozen trajectory oracle and accounting

Add a compact search-event trace covering retained IDs/body hashes, selector
keys, given IDs, demodulator admissions, backward rewrites, subsumption
deactivations, and hint identity.  Freeze reference traces at 10, 100, and
300 givens.  Report resident bytes separately for sidecars, selector heaps,
unit/nonunit/redex indexes, rewrite trie, body buffers, and file logical and
physical bytes.

### Stage 1: truly cold file backend

Add a file-backed cold passive arena and validate byte-identical
archive/materialize/clone behavior against memory and mmap backends.  First
use it with selected DISCOUNT to isolate RSS impact; this stage is not claimed
to fix the proof trajectory.

### Stage 2: exact compact demodulator bank

Run the ordinary OTTER transaction with the compact rewrite trie while full
passive bodies still exist.  Differentially compare every rewrite rule ID,
rewritten body, and back-demodulation wave.  Fix ordering or representation
until the 300-given reference trace is identical before cold eviction is
enabled.

### Stage 3: stable-ID compact subsumption and redex indexes

Introduce the unit index first, because Osborn is dominated by unit
equations, then the nonunit and redex indexes.  During this stage reference
and compact indexes run together in audit mode and their ordered exact result
IDs must agree.  Only after agreement may the passive pointer index be
removed.

### Stage 4: archive OTTER SOS bodies

Enable `compact_otter`: complete eager processing, publish the survivor to
all compact indexes and selectors, archive the body, and release its term
tree.  Selection and every backward mutation deactivate the stable ID before
materialization so no stale candidate can be observed.  Add checkpoint
serialization only after uninterrupted traces agree.

### Stage 5: bounded and proof runs

Run 10 and 100 givens first, then the exact 300-given gate.  Continue to
1,000 only after exactness and RAM gates pass.  The final measurement is a
proof run with the same clauses, hints, selection rules, and limits as the
old OTTER reference.

## Acceptance gates

### Correctness and trajectory

- Existing proof, hint, iterator, selector, rewrite, ancestor, and checkpoint
  tests remain green in their default modes.
- Reference and compact search-event traces are byte-identical at 10, 100,
  and 300 givens.
- Given IDs, generated/kept counts, SOS/usable/demodulator/disabled counts,
  hint IDs and degradation, and all backward rewrite/subsumption events agree.
- Checkpoint/resume produces the same continuation trace as uninterrupted
  compact OTTER.
- The final proof is accepted by `prooftrans`; use `directproof` where its
  supported inference subset permits.

### CPU and memory

- At 300 givens, total user CPU is at most 1.25 times packed-fast selected
  DISCOUNT's 10.23 seconds as a stretch target and never slower than current
  FPA OTTER's approximately 28--35 seconds.
- At 1,000 givens, total CPU is no more than 1.25 times the reference OTTER
  prefix and peak RSS is at most 125 MiB.
- The final run must produce the old proof with user CPU at most 957 seconds
  and wall time at most 18 minutes on the measured machine.
- Final peak RSS is at most 125 MiB, with 115 MiB as the stretch target.
- Accounting must explain at least 95% of resident compact-frontier bytes;
  logical file size is reported separately and is never called RAM.

A failed trace gate means the compact operation is not enabled.  A faster
prefix with a different trajectory, or a low RSS run without the proof, does
not complete Phase 5.

## Commit discipline

Commit the plan, file backend, each compact index, compatibility integration,
checkpoint support, and measured report separately.  Commit messages include
ownership invariants, ordering decisions, test evidence, and measured
tradeoffs so later reviewers can audit the implementation without relying on
this conversation.
