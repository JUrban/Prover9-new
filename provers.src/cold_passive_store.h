/* Compact body arena for DISCOUNT passive clauses. */

#ifndef TP_COLD_PASSIVE_STORE_H
#define TP_COLD_PASSIVE_STORE_H

#include "../ladr/ladr.h"

typedef struct cold_passive_store * Cold_passive_store;

typedef enum {
  COLD_PASSIVE_MEMORY,
  COLD_PASSIVE_MMAP,
  /* Anonymous-to-the-process file storage.  Records are accessed with
     pread/pwrite through one bounded scratch buffer, so appended pages do
     not become part of the process RSS as they do with MAP_SHARED. */
  COLD_PASSIVE_FILE
} Cold_passive_store_mode;

struct cold_passive_store_stats {
  Cold_passive_store_mode mode;
  unsigned long long records;
  unsigned long long record_bytes;
  unsigned long long backing_bytes;
  unsigned long long physical_bytes;
  unsigned long long materializations;
  unsigned long long validation_failures;
  unsigned long long file_reads;
  unsigned long long file_read_bytes;
  unsigned long long file_writes;
  unsigned long long file_write_bytes;
};

const char *cold_passive_store_mode_name(Cold_passive_store_mode mode);

Cold_passive_store cold_passive_store_init(Cold_passive_store_mode mode);

void cold_passive_store_free(Cold_passive_store store);

/* Archive destroys c after detaching its official ID-table entry and returns
   its three payload sizes without requiring a later mmap record scan. */
size_t cold_passive_store_archive(Cold_passive_store store, Topform c,
                                  unsigned *body_bytes,
                                  unsigned *justification_bytes,
                                  unsigned *logical_body_bytes);

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

/* Copy one validated immutable record into a fresh arena. */
size_t cold_passive_store_clone_record(Cold_passive_store source,
                                       size_t position,
                                       Cold_passive_store destination);

/* Preserve cumulative diagnostic counters when replacing an arena by a
   compacted copy. */
void cold_passive_store_inherit_counters(Cold_passive_store destination,
                                         Cold_passive_store source);

BOOL cold_passive_store_sync(Cold_passive_store store);

struct cold_passive_store_stats
cold_passive_store_get_stats(Cold_passive_store store);

#endif  /* TP_COLD_PASSIVE_STORE_H */
