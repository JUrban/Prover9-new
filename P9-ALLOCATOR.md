# Phase 5 allocator and exact-term design

Date: 2026-08-07

This document records the ownership, reclamation, accounting, compatibility,
and representation decisions for Phase 5.  `CLAUDE-P9.md` remains the
authoritative roadmap.

## Scope and invariants

The allocator continues to expose the existing pointer-count interface:
`get_mem(n)`, `get_cmem(n)`, and `free_mem(p,n)`.  Callers still own stable,
ordinary pointers and must free with the original pointer count.  The change
does not add handles, relocation, compaction, tracing, or a new proof/checkpoint
representation.

The required invariants are:

- a live allocation never moves;
- an empty slab contains no live caller pointer before it is unmapped;
- no free-list link outside a slab points into that slab;
- each active slab belongs to exactly one pointer-count class and has an exact
  live count; an empty cached slab belongs to no class;
- the last free makes a slab reclaimable without scanning or changing any
  other live pointer;
- the `max_megs` check counts current reservations and purges cached empty
  slabs before reporting exhaustion;
- `megs_malloced()` remains monotonic for Mace4 and progress callers by
  reporting peak reservation rounded to MiB; current reservation comes from
  `memory_get_stats()`.

## Allocation layout and lifecycle

Classes 1 through 127 use independent 256 KiB slabs.  Larger pointer-count
requests remain individually allocated and freed.  `tp_alloc()` remains
permanent by contract and uses a direct allocation rather than sharing a slab
whose lifetime could otherwise be misrepresented.

On native POSIX builds, a slab is an anonymous private mapping aligned to its
own 256 KiB size.  Alignment is obtained by temporarily mapping 512 KiB,
retaining the aligned 256 KiB interval, and unmapping the prefix and suffix.  The slab for
a pointer is therefore recovered with an address mask; no per-object header is
needed.  On Emscripten, a slab uses `safe_malloc()`/`safe_free()` and is found
by a bounded walk of the requested class, preserving the existing build path
without relying on unavailable native mappings.

Each slab header owns:

- its class and slot size;
- capacity, next-unallocated index, live count, and free count;
- a slab-local free list;
- links in the class's all-slabs and available-slabs lists.

Allocation uses a free slot from the first available slab or carves its next
slot.  A full slab leaves the available list.  Freeing a slot in a formerly
full slab makes it available again.  Each class retains one wholly empty warm
slab so repeated short-lived allocations continue to reuse its local free list
without reinitialization.  An additional empty slab leaves its class and enters
a bounded cross-class recycle pool.  The pool holds at most 256 256-KiB
mappings (64 MiB); a later allocation can reinitialize one for any
pointer-count class.  This avoids repeated aligned `mmap`/`munmap` cycles across
changing transient object mixes without restoring the old allocator's
unbounded retention.  `memory_release_unused()` purges both the class-warm
slabs and the pool.  Memory pressure through `max_megs` performs that purge
automatically before failing.

This local-free-list rule is what makes reclamation pointer-safe: unmapping a
slab cannot leave a global free-list node pointing into returned storage.

## Accounting contract

`memory_get_stats()` distinguishes the following quantities:

- `logical_live_bytes`: outstanding bytes requested through the pointer-count
  interface, direct path, and permanent path;
- `logical_peak_bytes`: high-water of logical live bytes;
- `reserved_bytes` / `peak_reserved_bytes`: current and peak slab mappings
  plus requested bytes for direct/permanent allocations;
- `reusable_bytes`: allocated slab slots currently on local free lists;
- `unallocated_bytes`: capacity in mapped slabs not yet carved into slots;
- `metadata_bytes`: slab header and unusable tail bytes;
- `fragmentation_bytes`: reserved minus logical live, exactly decomposed by
  reusable, unallocated, and metadata bytes for the slab path;
- direct and permanent live bytes;
- current/peak slab counts, current/peak cached slabs, mapping reuses and
  cache evictions, and cumulative unmapped slabs/bytes;
- cumulative requested allocation traffic.

The direct-path reservation excludes allocator-private libc headers because
their usable capacity is not portable.  Current RSS is read from
`/proc/self/statm` on Linux; peak RSS uses `getrusage`, normalized for macOS.
Unavailable platforms report zero rather than inventing a value.  Normal
Prover9 statistics print logical live/peak, current/peak reservation,
fragmentation components, reclamation, allocation traffic, and current/peak
RSS separately.  `bytes_palloced()` retains its compatibility meaning: newly
carved slab bytes plus permanent `tp_alloc()` bytes, not every free-list reuse.

## Lifetime audit finding

Real reclamation exposed a pre-existing lifetime error in `zap_flatterm()`.
Its loop freed the first flatterm node and then reread `f->end->next` through
that freed node on every iteration.  The old append-only allocator masked the
error because freed storage stayed mapped.  The boundary is now captured
before the first free, so reclamation does not change flatterm semantics.

## Exact representation decision

The representation work is a separate commit from the allocator.  Rigid term
allocations already placed their argument array directly after `struct term`,
yet stored a redundant pointer to that known address in every header.  `ARG`
and `ARGS` now derive the array from `t + 1`.  On the measured 64-bit host this
shrinks the header from 32 to 24 bytes while retaining the same symbol, arity,
private flags, container, auxiliary value, allocation ownership, and argument
order.  Checkpoints and proof/archive encodings are semantic and never store a
raw term struct, so their formats are unchanged.

The bounded aK `stats=all` profile justified this change: 2,005,677 term
headers were allocated, 53,295 remained live, and the redundant pointer cost
16,045,416 bytes of allocation traffic and 426,360 live bytes.  Removing it
reduced current/peak reservation by one then-1-MiB slab and process peak RSS by
about 536 KiB in the paired run, with identical search work and runtime.

The other evaluated changes were not made:

- term hash-consing/immutable interning conflicts with existing mutation of
  term flags, containers, auxiliary/FPA state, and incrementally filled
  arguments; making this safe is a broad ownership redesign;
- a literal is 24 bytes and the aK prefix allocated 106,632, with only 6,672
  live at the cap.  Shrinking it to 16 bytes would require pointer tagging or
  widespread accessors across 95 direct sign and 421 direct atom accesses for
  at most about 53 KiB live savings in that measurement;
- attributes were only 134 allocations / 121 live; a dedicated arena has no
  measured payoff;
- justifications were 119,767 allocations / 6,738 live and parajustifications
  40,591 / 202 live.  Their aligned 24-byte layouts cannot lose a pointer-sized
  unit without tagging or a different ownership representation.  Phase 4's
  cold archive already compacts retained proof metadata.

Combining any of those speculative layouts with the allocator and compact term
patch would make soundness failures harder to isolate, contrary to the Phase 5
review boundary.

## Test contract and limits

`allocator_churn_test` allocates and touches 300,000 256-byte objects across
many slabs, frees earlier slabs while a late pointer remains live, checks exact
live/reserved/fragmentation counters, observes RSS growth and fall, purges the
warm slab, exercises zeroed and direct allocations, and performs four
deterministically shuffled mixed-class rounds over classes 1 through 127.
The compact-term lifecycle test locks the 24-byte 64-bit header and verifies
that parsed argument access still addresses the contiguous array.

The allocator remains process-global and non-thread-safe, matching the old
allocator.  A slab reservation is virtual address space; untouched pages do
not equal RSS.  A warm slab can make logical fragmentation look large for a
small class, which is why all accounting components and RSS are reported
separately.  Phase 5 does not repair the already documented checkpoint
selector/AVL restart divergence and does not begin Phase 6.

## Phase-5 resident-memory follow-up (2026-08-10)

The file-backed full Osborn proof exposed 35,658,048 allocator-reserved bytes
for 20,105,440 live bytes.  Four classes retained completely empty one-MiB
warm slabs, and several low-occupancy classes retained almost a MiB for only
bytes or kilobytes of live data.  Reducing the fixed power-of-two slab to 256
KiB preserves mask-based owner recovery and immediate empty-slab unmapping
while substantially lowering this per-class floor.

At the exact 1,000-given CHAT boundary, current reservation falls from
32,512,320 to 21,502,272 bytes and fragmentation from 14,747,728 to 3,737,680
bytes.  The complete 300-given candidate/hint/kept/given trace is
byte-identical, the 1,000-given terminal search state is exact, and all
lifecycle tests pass.  A controlled 1,000-given run takes 84.38 user seconds versus 74.63
seconds with one-MiB slabs (+13.1%); this remains inside the full-proof CPU
budget but requires final-boundary validation.  Peak prefix RSS changes only
from 90,632 to 90,256 KiB because that peak is the fixed 84.8-MB initialization
wave; the final current RSS probe falls from 84,504 to 80,852 KiB.

The full Osborn proof confirms that the reservation reduction is real but not
fully resident.  At the exact final boundary, reservation falls from
35,658,048 to 24,123,712 bytes and reported fragmentation from 15,552,608 to
4,018,272 bytes.  Internal current RSS falls from 177,652 to 167,796 KiB,
while external peak RSS falls from 188,736 to 178,704 KiB.  The 11.00-MiB
virtual-reservation saving therefore yields 9.63 MiB less current RSS, not an
11-MiB peak guarantee: a material part of the old unallocated slab capacity
had never faulted resident.  The exact proof takes 804.04 user seconds versus
786.23 seconds with one-MiB slabs and the old array capacities (+2.3%).

Whole-process reports now include Linux `smaps_rollup` residency classes and,
when glibc provides `mallinfo2`, libc arena/mmap accounting.  These probes run
only at an existing statistics snapshot and are zero-filled on unsupported
platforms.  At the exact 1,000-given CHAT state, process RSS is 73,496 KiB:
70,356 KiB is anonymous, 896 KiB private-clean, and 2,244 KiB shared-clean.
Glibc owns a 29,949,952-byte arena plus 23,912,448 malloc-mmap bytes; the
arena contains 21,336,336 in-use and 8,613,616 free bytes.  The pointer-count
allocator separately reserves 21,502,272 bytes in direct slabs.  Thus libc
fragmentation is measurable, but its free arena bytes are not automatically
reclaimable or additive to an RSS saving; only 131,824 bytes are reported as
the top releasable block at this boundary.

For long compact-frontier runs on glibc, set `P9_COMPACT_HEAP=1` in the
process environment.  Before building its saved command line or reading the
problem, Prover9 applies a 64-KiB `M_MMAP_THRESHOLD` and a zero
`M_TRIM_THRESHOLD`.  This keeps medium and large transient arrays in
independently returnable mappings, prevents glibc's dynamic mmap threshold
from rising, and avoids leaving their released capacities as holes in the
main arena.  Unsupported libcs safely leave the policy disabled;
`Libc_heap_bytes` reports `compact_policy=enabled|disabled` for audit.

A parallel exact 1,000-given CHAT comparison measured the explicit threshold
before it became part of the product switch.  The default allocator ended at
71,162 KiB PSS and 90.64 user seconds; 64 KiB ended at 68,318 KiB and 92.41
seconds.  The search state was identical, and the arena fell from 31,346,688
to 23,064,576 bytes while total live arena-plus-mmap allocation stayed the
same.  A 256-KiB control ended at 69,018 KiB and 89.64 seconds.  The 64-KiB
boundary is selected because it returns the most resident memory in this
bounded test; the exact full proof remains the acceptance gate.

At 1,000 givens the P9 control and the raw
`GLIBC_TUNABLES=glibc.malloc.trim_threshold=0` control produce identical
search state and essentially identical arena/mmap/RSS values.  The raw-tunable
full proof is exact at 2,945 givens and takes 788.08 user seconds, 886.05 wall
seconds, and 152,616 KiB peak RSS.  The product-facing P9 switch has now passed
the same full proof: its 7,051 normalized proof clauses are byte-identical, it
takes 833.73 user seconds on the slower validation run, and it peaks at
152,904 KiB.  The 288-KiB peak difference is measurement noise at this scale;
both controls save about 18 MiB from the preceding 170,968-KiB proof.  The P9
switch is therefore the accepted invocation for later comparisons.

## Mature allocator CPU follow-up (2026-08-18)

The complete Josef 01 compact proof exposed a scaling failure that short
prefixes could not show.  It returned 11,942,479 256-KiB slabs to the kernel,
or 3.13 TB cumulatively, and used 8,532 system-CPU seconds.  The old P9 run of
the exact same 30,827-given trajectory used only 288 system-CPU seconds.  The
compact run's buffered archive traffic is far too small to explain that gap by
itself; millions of aligned mapping creations and releases are a direct,
measured kernel-work source.

The bounded cross-class recycle pool described above addresses that source.
An initial implementation routed even a class's sole warm slab through the
pool.  It was rejected: at the exact 2,000-given Josef boundary it performed
4.55 million needless detach/reinitialize cycles and took 242.79 CPU seconds,
versus 237.98 for the same unit-index code without the pool.  The accepted
hybrid retains one local warm slab per class and pools only excess slabs that
the previous allocator would have unmapped.

At 1,001 givens the hybrid run has the exact reference state (1,628,048
generated and 320,239 kept).  Of the 564 excess-slab releases reported by the
old allocator, the hybrid has reused 171 mappings, retained 252 for later
reuse, and evicted/unmapped 141.  Its allocator reservation rises from about
52 MiB to 118 MiB and external peak RSS from about 540 MiB to 606 MiB; the
increase is bounded by the 64-MiB pool plus measurement noise.  On the full
compact proof this bound is less than one percent of the measured 9.08-GiB
PSS, so it does not materially weaken the roughly 80-percent resident-memory
reduction.

This prefix validates exactness, accounting, and the absence of an early-run
hot-path regression.  It does not establish the full CPU saving: only a new
30,827-given proof can measure how many of the 11.94 million mature releases
are reused rather than evicted.  The final report must therefore treat that
full run as an acceptance gate, not extrapolate all 8,244 excess system-CPU
seconds as already recovered.

Compact OTTER's shared term pool uses
`assign(compact_term_reclaim_kb,8192)` by default.  This is the conservative
estimated stale-token payload required before coordinating rewrite, unit, and
back-demod index compaction.  Lower values are intended for measured peak-RSS
experiments.  The compactor retains live proof-directory slots, moves their
token intervals downward in the existing array, rebuilds the directory, and
then rebases all three indexes; it no longer allocates a second complete term
pool.  `Compact_term_pool` reports both the configured `reclaim_kb` and the
actual compaction/reclaimed-byte counts.

On Linux, shared term tokens live in a private anonymous mapping.  Capacity
changes use `mremap(MREMAP_MAYMOVE)`, so growing a 10--20 MiB token array moves
page tables rather than holding and copying old and new arrays simultaneously.
Other platforms retain the portable `realloc` path.  `token_growths` still
counts logical capacity changes; `token_copy_bytes=0` confirms the Linux
zero-copy path.  After the 1,024-stale-clause noise floor, the configured
stale-token byte budget is authoritative; a percentage-of-live-clauses guard
no longer delays a requested reclaim past another token growth boundary.
