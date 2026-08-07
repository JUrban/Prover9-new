/* Compact body arena for DISCOUNT passive clauses. */

#ifndef TP_COLD_PASSIVE_STORE_H
#define TP_COLD_PASSIVE_STORE_H

#include "../ladr/ladr.h"

typedef struct cold_passive_store * Cold_passive_store;

typedef enum {
  COLD_PASSIVE_MEMORY,
  COLD_PASSIVE_MMAP
} Cold_passive_store_mode;

struct cold_passive_store_stats {
  unsigned long long records;
  unsigned long long record_bytes;
  unsigned long long backing_bytes;
  unsigned long long materializations;
  unsigned long long validation_failures;
};

Cold_passive_store cold_passive_store_init(Cold_passive_store_mode mode);

void cold_passive_store_free(Cold_passive_store store);

/* Archive destroys c after detaching its official ID-table entry. */
size_t cold_passive_store_archive(Cold_passive_store store, Topform c);

/* Decode one record.  Activation registers the supplied stable clause ID;
   peek materialization leaves it unregistered for checkpoint/stat use. */
Topform cold_passive_store_materialize(Cold_passive_store store,
                                       size_t position,
                                       unsigned long long id,
                                       BOOL activate);

void cold_passive_store_release(Topform c);

BOOL cold_passive_store_payload_sizes(
  Cold_passive_store store, size_t position,
  unsigned long long *body_bytes,
  unsigned long long *justification_bytes,
  unsigned long long *logical_body_bytes);

BOOL cold_passive_store_sync(Cold_passive_store store);

struct cold_passive_store_stats
cold_passive_store_get_stats(Cold_passive_store store);

#endif  /* TP_COLD_PASSIVE_STORE_H */
