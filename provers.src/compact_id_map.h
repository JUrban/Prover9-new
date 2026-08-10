#ifndef TP_COMPACT_ID_MAP_H
#define TP_COMPACT_ID_MAP_H

#include "../ladr/ladr.h"

#include <stddef.h>
#include <stdint.h>

typedef struct compact_id_map * Compact_id_map;

typedef void (*Compact_id_map_visit_fn)(unsigned long long proof_id,
                                        const uint32_t *values,
                                        void *context);

/* Pointer-free values keyed by the full 64-bit proof-ID namespace.  Values
   are stored in sparse 64-KiB direct pages; the first word is the occupancy
   sentinel and must therefore be nonzero. */
Compact_id_map compact_id_map_init(unsigned value_words);

BOOL compact_id_map_get(Compact_id_map map, unsigned long long proof_id,
                        uint32_t *values);

/* Insert or replace one value tuple.  Returns TRUE only for a new proof ID. */
BOOL compact_id_map_put(Compact_id_map map, unsigned long long proof_id,
                        const uint32_t *values);

BOOL compact_id_map_remove(Compact_id_map map,
                           unsigned long long proof_id);

void compact_id_map_foreach(Compact_id_map map,
                            Compact_id_map_visit_fn visit, void *context);

size_t compact_id_map_count(Compact_id_map map);

unsigned long long compact_id_map_bytes(Compact_id_map map);

unsigned long long compact_id_map_peak_bytes(Compact_id_map map);

void compact_id_map_free(Compact_id_map map);

#endif
