# Experimental generalized-hint hash

Branch: `experimental-generalized-hint-hash`

This branch adds a deliberately incomplete, hash-only definition of hint
matching.  Its purpose is to measure the speed and search usefulness of
precomputing subsumption answers, not to preserve the legacy search.

## Mode and semantics

Select the mode with:

```prover9
assign(hint_index,generalized_hash).
```

The initial hint bank is still deduplicated and compressed by the packed
owner.  The experimental matcher then constructs one flat, static hash table:

1. Every active hint contributes its exact clause key.
2. Every hint whose total atom-symbol count is at most
   `hint_hash_complete_nodes` contributes all of its first-order
   generalizations.  This includes replacing subterms by variables, reusing
   variables for equal target subterms, splitting repeated target variables,
   deleting literals, and permuting the retained literals.
3. Every longer hint contributes its exact key plus at most
   `hint_hash_partial_per_hint` one-subterm abstractions.  Shallower positions
   are admitted first.
4. A generated clause is normalized and looked up once.  A miss is a final
   non-match: there is no packed/FPA candidate generation and no subsumption
   fallback.  The existing unoriented-equality flip remains a second hash
   lookup after a normal miss.

The table stores the selected stable hint ID directly.  Exact keys take
priority over proper-generalization keys; duplicate proper keys retain the
lowest hint ID, matching the packed matcher's proper-match preference.

The table uses a 96-bit clause fingerprint and 16-byte flat slots with double
hashing.  Duplicate generalizations share one slot.

## Recommended first Josef_04 run

Keep the previous compact OTTER configuration, change the hint-index line,
and add the following settings:

```prover9
assign(hint_index,generalized_hash).
assign(hint_hash_complete_nodes,8).
assign(hint_hash_partial_per_hint,32).
assign(hint_hash_max_entries,100000000).

clear(back_demod_hints).
```

`hint_hash_complete_nodes=8` is a conservative first exhaustive boundary.
`hint_hash_partial_per_hint=32` retains exact matching plus up to 32 shallow
one-hole generalizations for each longer active hint.  The 100-million unique
entry limit is a cap, not an up-front allocation.

The existing `hint_conjunction_kb`, `hint_cache_kb`, and
`hint_cache_min_candidates` settings do not control hash queries.  The mode
automatically suppresses conjunction construction, so those lines may be
left in the input or removed.

This first implementation requires a static hint bank:

- `clear(back_demod_hints)` is mandatory;
- `hint_match_once` must remain clear;
- `hint_expiry` must remain disabled.

Ordinary clause demodulation and the compact OTTER indexes are unchanged.
Hint weights, labels, degradation counters, and given-selection tests still
use the hint returned by the hash.

## Building

```sh
make -C ladr -j4 libladr.a
make -C provers.src -j4 prover9
cp -p provers.src/prover9 bin/prover9
```

## What to watch in output

Construction prints progress to standard error.  The ordinary statistics
contain a line beginning with `Generalized_hint_hash:`.  The main fields are:

- `entries`, `capacity`, and `bytes`: actual table size;
- `complete_hints`: hints below the exhaustive boundary;
- `partial_hints`: longer hints receiving bounded abstractions;
- `generated_attempts`, `generated_new_entries`, and `duplicates`: how much
  sharing occurred;
- `queries`, `hits`, and `hit_rate`: how much of the generated search the
  incomplete table recognizes;
- `mean_probes` and `max_probe`: raw hash-table lookup cost.

If exhaustive short-hint coverage cannot fit below
`hint_hash_max_entries`, startup stops with an explicit fatal error rather
than silently violating the completeness boundary.  Once all short hints are
covered, reaching the cap merely stops admission of partial long-hint keys.

## Initial smoke measurement

On a 5,000-hint Josef_04 prefix with 100 selected givens, the initial
`8/32` configuration constructed 50,881 unique entries from 107,647 exact
and generated insertion attempts.  The run made 9,228 hash queries, with a
mean of 4.19 slot probes at 77.6% occupancy.  The old packed matcher performed
zero search-time match queries in this mode, confirming that misses do not
fall back.

This prefix is only a functional and cost smoke test.  The full run is needed
to measure final table size, construction time, hit coverage, search speed,
and whether the incomplete hint semantics still find the proof.
