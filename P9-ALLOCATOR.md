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
- each slab belongs to exactly one pointer-count class and has an exact live
  count;
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
full slab makes it available again.  When live count reaches zero, the mapping
is unmapped immediately if its class has another slab.  One empty slab per
class is retained as a warm cache to avoid map/unmap churn for short-lived
objects.  `memory_release_unused()` purges those warm mappings as well.  Memory
pressure through `max_megs` performs that purge automatically before failing.

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
- current/peak slab counts and cumulative unmapped slabs/bytes;
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
