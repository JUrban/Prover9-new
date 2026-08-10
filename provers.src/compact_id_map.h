#ifndef TP_COMPACT_ID_MAP_H
#define TP_COMPACT_ID_MAP_H

#include "../ladr/ladr.h"

#include <stddef.h>
#include <stdint.h>

typedef struct compact_id_map * Compact_id_map;
typedef struct compact_id_set * Compact_id_set;

typedef void (*Compact_id_map_visit_fn)(unsigned long long proof_id,
                                        const uint32_t *values,
                                        void *context);
typedef void (*Compact_id_set_visit_fn)(unsigned long long proof_id,
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

/* Sparse full-width ID membership using 4-KiB bit pages. */
Compact_id_set compact_id_set_init(void);

/* Returns TRUE only when PROOF_ID was not already present. */
BOOL compact_id_set_add(Compact_id_set set,
                        unsigned long long proof_id);

BOOL compact_id_set_contains(Compact_id_set set,
                             unsigned long long proof_id);

void compact_id_set_foreach(Compact_id_set set,
                            Compact_id_set_visit_fn visit, void *context);

size_t compact_id_set_count(Compact_id_set set);

unsigned long long compact_id_set_bytes(Compact_id_set set);

/* Exact allocation size of a map populated with SET's IDs. */
unsigned long long compact_id_set_projected_map_bytes(Compact_id_set set,
                                                       unsigned value_words);

void compact_id_set_free(Compact_id_set set);

#endif
