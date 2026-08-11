/* Versioned append-only proof-ancestor store. */

#include "clause_store.h"
#include "clist.h"
#include "clause_misc.h"
#include "clauseid.h"
#include "compress.h"
#include "just.h"
#include "memory.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#ifndef __EMSCRIPTEN__
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

#define ANCESTOR_MAGIC "P9AR"
#define ANCESTOR_VERSION 2
#define ANCESTOR_HEADER_SIZE 96
#define STORE_REF_TAG ((uintptr_t) 1)
#define MMAP_EVICT_STEP (8U * 1024U * 1024U)
#define MMAP_HOT_WINDOW (8U * 1024U * 1024U)

#define AF_IS_FORMULA  0x0001U
#define AF_NORMAL_VARS 0x0002U
#define AF_USED        0x0004U
#define AF_INITIAL     0x0008U
#define AF_NEGATIVE    0x0010U
#define AF_SUBSUMER    0x0020U
#define AF_WAS_GIVEN   0x0040U
#define AF_GOAL        0x0080U

struct clause_store {
  uintptr_t *refs;              /* Topform pointer or tagged record offset */
  size_t length;
  size_t capacity;
  Clause_store_archive_mode mode;
  unsigned char *backing;
  size_t backing_size;
  size_t backing_capacity;
  int fd;
  unsigned char *io_buffer;
  size_t io_capacity;
  size_t current_records;
  unsigned long long materializations;
  unsigned long long validation_failures;
  unsigned long long archive_records;
  unsigned long long archive_body_bytes;
  unsigned long long archive_logical_body_bytes;
  size_t mmap_synced_bytes;
  size_t mmap_last_evict_size;
  unsigned long long mmap_eviction_passes;
  unsigned long long mmap_eviction_bytes;
  unsigned long long mmap_scan_eviction_passes;
  unsigned long long mmap_scan_eviction_bytes;
  unsigned long long file_reads;
  unsigned long long file_read_bytes;
  unsigned long long file_writes;
  unsigned long long file_write_bytes;
};

/* The ID table stores only a tagged offset, so one archive-enabled store is
   active per LADR process.  Prover9 has exactly one disabled-clause store. */
static Clause_store Active_archive_store = NULL;

static BOOL ref_is_archive(uintptr_t ref)
{
  return (ref & STORE_REF_TAG) != 0;
}

static uintptr_t offset_ref(unsigned long long offset)
{
  if (offset > (unsigned long long) (UINTPTR_MAX >> 1))
    fatal_error("offset_ref: ancestor-store offset overflow");
  return ((uintptr_t) offset << 1) | STORE_REF_TAG;
}

static unsigned long long ref_offset(uintptr_t ref)
{
  return (unsigned long long) (ref >> 1);
}

static void put16(unsigned char *p, unsigned value)
{
  p[0] = (unsigned char) value;
  p[1] = (unsigned char) (value >> 8);
}

static void put32(unsigned char *p, uint32_t value)
{
  int i;
  for (i = 0; i < 4; i++)
    p[i] = (unsigned char) (value >> (8 * i));
}

static void put64(unsigned char *p, uint64_t value)
{
  int i;
  for (i = 0; i < 8; i++)
    p[i] = (unsigned char) (value >> (8 * i));
}

static unsigned get16(const unsigned char *p)
{
  return (unsigned) p[0] | ((unsigned) p[1] << 8);
}

static uint32_t get32(const unsigned char *p)
{
  uint32_t value = 0;
  int i;
  for (i = 3; i >= 0; i--)
    value = (value << 8) | p[i];
  return value;
}

static uint64_t get64(const unsigned char *p)
{
  uint64_t value = 0;
  int i;
  for (i = 7; i >= 0; i--)
    value = (value << 8) | p[i];
  return value;
}

static uint32_t crc32_bytes(const unsigned char *data, size_t size)
{
  uint32_t crc = 0xffffffffU;
  size_t i;
  for (i = 0; i < size; i++) {
    unsigned bit;
    crc ^= data[i];
    for (bit = 0; bit < 8; bit++)
      crc = (crc >> 1) ^ (0xedb88320U & (uint32_t) -(int) (crc & 1));
  }
  return ~crc;
}

#ifndef __EMSCRIPTEN__
static BOOL file_read_exact(Clause_store store, void *buffer,
                            size_t size, size_t position)
{
  size_t done = 0;
  while (done < size) {
    ssize_t n = pread(store->fd, (unsigned char *) buffer + done,
                      size - done, (off_t) (position + done));
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return FALSE;
    done += (size_t) n;
  }
  store->file_reads++;
  store->file_read_bytes += size;
  return TRUE;
}

static BOOL file_write_exact(Clause_store store, const void *buffer,
                             size_t size, size_t position)
{
  size_t done = 0;
  while (done < size) {
    ssize_t n = pwrite(store->fd, (const unsigned char *) buffer + done,
                       size - done, (off_t) (position + done));
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return FALSE;
    done += (size_t) n;
  }
  store->file_writes++;
  store->file_write_bytes += size;
  return TRUE;
}
#endif

static BOOL ensure_io_buffer(Clause_store store, size_t needed)
{
  size_t capacity = store->io_capacity == 0 ? 4096 : store->io_capacity;
  if (needed <= store->io_capacity)
    return TRUE;
  while (capacity < needed) {
    size_t grown = capacity + capacity / 2;
    if (grown <= capacity)
      return FALSE;
    capacity = grown;
  }
  store->io_buffer = safe_realloc(store->io_buffer, capacity);
  store->io_capacity = capacity;
  return TRUE;
}

static BOOL ensure_backing(Clause_store store, size_t needed)
{
  size_t capacity;
  if (needed <= store->backing_capacity)
    return TRUE;
  capacity = store->backing_capacity == 0 ? 4096 : store->backing_capacity;
  while (capacity < needed) {
    if (capacity > ((size_t) -1) / 2)
      return FALSE;
    capacity *= 2;
  }
  if (store->mode == CLAUSE_STORE_ARCHIVE_MEMORY) {
    store->backing = safe_realloc(store->backing, capacity);
    store->backing_capacity = capacity;
    return TRUE;
  }
#ifndef __EMSCRIPTEN__
  if (store->mode == CLAUSE_STORE_ARCHIVE_FILE) {
    /* pwrite extends the file at commit time.  Track only its logical
       capacity; no process-resident arena is reserved here. */
    store->backing_capacity = needed;
    return TRUE;
  }
  if (store->mode == CLAUSE_STORE_ARCHIVE_MMAP) {
    void *mapping;
    if (ftruncate(store->fd, (off_t) capacity) != 0)
      return FALSE;
    if (store->backing != NULL &&
        munmap(store->backing, store->backing_capacity) != 0)
      return FALSE;
    store->backing = NULL;
    mapping = mmap(NULL, capacity, PROT_READ | PROT_WRITE, MAP_SHARED,
                   store->fd, 0);
    if (mapping == MAP_FAILED)
      return FALSE;
    store->backing = mapping;
    store->backing_capacity = capacity;
    return TRUE;
  }
#endif
  return FALSE;
}

/* Keep only a bounded writable tail mapped resident.  The ancestor file is
   still the authority: complete cold pages are synchronized before their
   PTEs are discarded, and later random proof/materialization reads fault
   those pages back through the ordinary MAP_SHARED mapping. */
static void evict_cold_mmap_pages(Clause_store store)
{
#ifndef __EMSCRIPTEN__
  long page_size_long;
  size_t page_size, cold_end, sync_start;
  if (store == NULL || store->mode != CLAUSE_STORE_ARCHIVE_MMAP ||
      store->backing == NULL ||
      store->backing_size <= MMAP_HOT_WINDOW)
    return;
  if ((store->mmap_last_evict_size == 0 &&
       store->backing_size < MMAP_HOT_WINDOW + MMAP_EVICT_STEP) ||
      (store->mmap_last_evict_size != 0 &&
       store->backing_size - store->mmap_last_evict_size < MMAP_EVICT_STEP))
    return;
  page_size_long = sysconf(_SC_PAGESIZE);
  if (page_size_long <= 0)
    return;
  page_size = (size_t) page_size_long;
  cold_end = (store->backing_size - MMAP_HOT_WINDOW) / page_size * page_size;
  sync_start = store->mmap_synced_bytes / page_size * page_size;
  if (cold_end == 0)
    return;
  if (sync_start < cold_end &&
      msync(store->backing + sync_start, cold_end - sync_start, MS_SYNC) != 0)
    return;
  store->mmap_synced_bytes = cold_end;
  if (cold_end != 0 &&
      madvise(store->backing, cold_end, MADV_DONTNEED) == 0) {
    store->mmap_eviction_passes++;
    store->mmap_eviction_bytes += cold_end;
    store->mmap_last_evict_size = store->backing_size;
  }
#else
  (void) store;
#endif
}

static unsigned clause_flags(Topform c)
{
  unsigned flags = 0;
  if (c->is_formula) flags |= AF_IS_FORMULA;
  if (c->normal_vars) flags |= AF_NORMAL_VARS;
  if (c->used) flags |= AF_USED;
  if (c->initial) flags |= AF_INITIAL;
  if (negative_clause_possibly_compressed(c)) flags |= AF_NEGATIVE;
  if (c->subsumer) flags |= AF_SUBSUMER;
  if (c->was_given) flags |= AF_WAS_GIVEN;
  if (c->goal_derived) flags |= AF_GOAL;
  return flags;
}

static BOOL term_blob(Term t, char **data, unsigned *size)
{
  if (t == NULL) {
    *data = NULL;
    *size = 0;
    return TRUE;
  }
  return encode_term_versioned(t, data, size);
}

static BOOL append_record(Clause_store store, Topform c,
                          unsigned long long *record_offset)
{
  char *formula_data = NULL, *attribute_data = NULL, *just_data = NULL;
  unsigned formula_size = 0, attribute_size = 0, just_size = 0;
  Term formula_term = NULL, attribute_term = NULL;
  Ilist parents = NULL, p;
  unsigned parent_count = 0;
  uint64_t payload_size, total_size, offset;
  unsigned char *record, *out;
  uint64_t weight_bits;
  unsigned flags;
  BOOL ok = FALSE;

  if (c == NULL || c->id == 0 || store->mode == CLAUSE_STORE_ARCHIVE_OFF)
    return FALSE;
  if (sizeof(double) != sizeof(uint64_t))
    fatal_error("append_record: ancestor format requires 64-bit double");
  /* Cold DISCOUNT payloads co-locate their justification with the body.
     Ancestor records have a separate, parent-indexed justification field,
     so materialize before producing the persistent record. */
  if (c->compressed != NULL && c->packed_justification &&
      !materialize_clause(c))
    return FALSE;
  if (!c->is_formula && c->literals != NULL && c->compressed == NULL) {
    Clause_compress_result cr = compress_clause(c);
    if (cr != CLAUSE_COMPRESS_OK && cr != CLAUSE_COMPRESS_ALREADY)
      return FALSE;
  }
  if (c->is_formula && c->formula != NULL) {
    formula_term = formula_to_term(c->formula);
    if (!term_blob(formula_term, &formula_data, &formula_size))
      goto done;
  }
  if (c->attributes != NULL) {
    attribute_term = attributes_to_term(c->attributes, "#");
    if (!term_blob(attribute_term, &attribute_data, &attribute_size))
      goto done;
  }
  if (!encode_justification(c->justification, &just_data, &just_size))
    goto done;
  parents = get_parents(c->justification, TRUE);
  for (p = parents; p != NULL; p = p->next)
    parent_count++;

  payload_size = (uint64_t) c->compressed_size + formula_size +
                 attribute_size + just_size + (uint64_t) parent_count * 8;
  total_size = ANCESTOR_HEADER_SIZE + payload_size;
  if (total_size > (uint64_t) SIZE_MAX ||
      store->backing_size > SIZE_MAX - (size_t) total_size)
    goto done;
  offset = store->backing_size;
  if (!ensure_backing(store, store->backing_size + (size_t) total_size))
    goto done;
#ifndef __EMSCRIPTEN__
  if (store->mode == CLAUSE_STORE_ARCHIVE_FILE) {
    if (!ensure_io_buffer(store, (size_t) total_size))
      goto done;
    record = store->io_buffer;
  }
  else
#endif
    record = store->backing + store->backing_size;
  memset(record, 0, ANCESTOR_HEADER_SIZE);
  memcpy(record, ANCESTOR_MAGIC, 4);
  put16(record + 4, ANCESTOR_VERSION);
  put16(record + 6, ANCESTOR_HEADER_SIZE);
  put64(record + 8, total_size);
  put64(record + 16, c->id);
  put64(record + 24, c->matching_hint == NULL ? 0 : c->matching_hint->id);
  put64(record + 32, c->last_matched_given);
  memcpy(&weight_bits, &c->weight, sizeof(weight_bits));
  put64(record + 40, weight_bits);
  flags = clause_flags(c);
  put32(record + 48, flags);
  put32(record + 52, (uint32_t) c->semantics);
  put32(record + 56, (uint32_t) c->proof_tree_weight_cache);
  put32(record + 60, c->uncompressed_body_bytes);
  put32(record + 64, c->compressed_size);
  put32(record + 68, formula_size);
  put32(record + 72, attribute_size);
  put32(record + 76, just_size);
  put32(record + 80, parent_count);

  out = record + ANCESTOR_HEADER_SIZE;
  if (c->compressed_size != 0) {
    memcpy(out, c->compressed, c->compressed_size);
    out += c->compressed_size;
  }
  if (formula_size != 0) {
    memcpy(out, formula_data, formula_size);
    out += formula_size;
  }
  if (attribute_size != 0) {
    memcpy(out, attribute_data, attribute_size);
    out += attribute_size;
  }
  if (just_size != 0) {
    memcpy(out, just_data, just_size);
    out += just_size;
  }
  for (p = parents; p != NULL; p = p->next) {
    put64(out, (uint64_t) (unsigned) p->i);
    out += 8;
  }
  /* Version 2 uses the formerly reserved final word for the DISCOUNT
     activation epoch.  Fold it into the payload checksum so corruption of
     this scheduling-critical value is detected without growing the header. */
  put32(record + 92, c->simplifier_epoch);
  put32(record + 84,
        crc32_bytes(record + ANCESTOR_HEADER_SIZE, (size_t) payload_size) ^
        c->simplifier_epoch);
  put32(record + 88, crc32_bytes(record, 88));
#ifndef __EMSCRIPTEN__
  if (store->mode == CLAUSE_STORE_ARCHIVE_FILE &&
      !file_write_exact(store, record, (size_t) total_size,
                        store->backing_size))
    goto done;
#endif
  store->backing_size += (size_t) total_size;
  store->archive_records++;
  store->archive_body_bytes += c->compressed_size;
  store->archive_logical_body_bytes += c->uncompressed_body_bytes;
  *record_offset = offset;
  evict_cold_mmap_pages(store);
  ok = TRUE;

done:
  if (formula_term != NULL) zap_term(formula_term);
  if (attribute_term != NULL) zap_term(attribute_term);
  safe_free(formula_data);
  safe_free(attribute_data);
  safe_free(just_data);
  zap_ilist(parents);
  return ok;
}

struct record_view {
  const unsigned char *record;
  const unsigned char *body;
  const unsigned char *formula;
  const unsigned char *attributes;
  const unsigned char *justification;
  const unsigned char *parents;
  uint64_t total;
  uint64_t id;
  uint64_t hint_id;
  uint64_t last_matched;
  uint64_t weight_bits;
  uint32_t flags;
  int semantics;
  int proof_cache;
  uint32_t logical_body;
  uint32_t body_size;
  uint32_t formula_size;
  uint32_t attribute_size;
  uint32_t just_size;
  uint32_t parent_count;
  uint32_t simplifier_epoch;
};

/* Teardown needs only the stable ID.  Do not read and checksum an entire
   file-backed record merely to decide whether its ID-table entry is still
   current; long searches retain many superseded archive versions. */
static BOOL record_identity(Clause_store store, unsigned long long offset,
                            unsigned long long *id)
{
  const unsigned char *r;
  unsigned char header[ANCESTOR_HEADER_SIZE];
  uint64_t total;
  unsigned version;
  if (store == NULL || id == NULL || offset > store->backing_size ||
      store->backing_size - (size_t) offset < ANCESTOR_HEADER_SIZE)
    goto bad;
#ifndef __EMSCRIPTEN__
  if (store->mode == CLAUSE_STORE_ARCHIVE_FILE) {
    if (!file_read_exact(store, header, sizeof(header), (size_t) offset))
      goto bad;
    r = header;
  }
  else
#endif
    r = store->backing + (size_t) offset;
  version = get16(r + 4);
  total = get64(r + 8);
  if (memcmp(r, ANCESTOR_MAGIC, 4) != 0 ||
      (version != 1 && version != ANCESTOR_VERSION) ||
      get16(r + 6) != ANCESTOR_HEADER_SIZE ||
      get32(r + 88) != crc32_bytes(r, 88) ||
      (version == 1 && get32(r + 92) != 0) ||
      total < ANCESTOR_HEADER_SIZE ||
      total > store->backing_size - (size_t) offset || get64(r + 16) == 0)
    goto bad;
  *id = get64(r + 16);
  return TRUE;
bad:
  if (store != NULL)
    store->validation_failures++;
  return FALSE;
}

static BOOL record_view(Clause_store store, unsigned long long offset,
                        struct record_view *v)
{
  const unsigned char *r;
  unsigned char header[ANCESTOR_HEADER_SIZE];
  uint64_t payload, sum;
  unsigned version;
  if (store == NULL || v == NULL || offset > store->backing_size ||
      store->backing_size - (size_t) offset < ANCESTOR_HEADER_SIZE)
    goto bad;
#ifndef __EMSCRIPTEN__
  if (store->mode == CLAUSE_STORE_ARCHIVE_FILE) {
    uint64_t total;
    if (!file_read_exact(store, header, sizeof(header), (size_t) offset))
      goto bad;
    version = get16(header + 4);
    if (memcmp(header, ANCESTOR_MAGIC, 4) != 0 ||
        (version != 1 && version != ANCESTOR_VERSION) ||
        get16(header + 6) != ANCESTOR_HEADER_SIZE ||
        get32(header + 88) != crc32_bytes(header, 88) ||
        (version == 1 && get32(header + 92) != 0))
      goto bad;
    total = get64(header + 8);
    if (total < ANCESTOR_HEADER_SIZE || total > SIZE_MAX ||
        total > store->backing_size - (size_t) offset ||
        !ensure_io_buffer(store, (size_t) total))
      goto bad;
    memcpy(store->io_buffer, header, sizeof(header));
    if (total > sizeof(header) &&
        !file_read_exact(store, store->io_buffer + sizeof(header),
                         (size_t) total - sizeof(header),
                         (size_t) offset + sizeof(header)))
      goto bad;
    r = store->io_buffer;
  }
  else
#endif
    r = store->backing + (size_t) offset;
  version = get16(r + 4);
  if (memcmp(r, ANCESTOR_MAGIC, 4) != 0 ||
      (version != 1 && version != ANCESTOR_VERSION) ||
      get16(r + 6) != ANCESTOR_HEADER_SIZE ||
      get32(r + 88) != crc32_bytes(r, 88) ||
      (version == 1 && get32(r + 92) != 0))
    goto bad;
  memset(v, 0, sizeof(*v));
  v->record = r;
  v->total = get64(r + 8);
  v->id = get64(r + 16);
  v->hint_id = get64(r + 24);
  v->last_matched = get64(r + 32);
  v->weight_bits = get64(r + 40);
  v->flags = get32(r + 48);
  v->semantics = (int32_t) get32(r + 52);
  v->proof_cache = (int32_t) get32(r + 56);
  v->logical_body = get32(r + 60);
  v->body_size = get32(r + 64);
  v->formula_size = get32(r + 68);
  v->attribute_size = get32(r + 72);
  v->just_size = get32(r + 76);
  v->parent_count = get32(r + 80);
  v->simplifier_epoch = version >= 2 ? get32(r + 92) : 0;
  if (v->id == 0 || (v->flags & ~0x00ffU) != 0 || v->just_size < 5)
    goto bad;
  sum = (uint64_t) v->body_size + v->formula_size + v->attribute_size +
        v->just_size;
  if (v->parent_count > (UINT64_MAX - sum) / 8)
    goto bad;
  payload = sum + (uint64_t) v->parent_count * 8;
  if (v->total != ANCESTOR_HEADER_SIZE + payload ||
      v->total > store->backing_size - (size_t) offset)
    goto bad;
  v->body = r + ANCESTOR_HEADER_SIZE;
  v->formula = v->body + v->body_size;
  v->attributes = v->formula + v->formula_size;
  v->justification = v->attributes + v->attribute_size;
  v->parents = v->justification + v->just_size;
  if (get32(r + 84) !=
      (crc32_bytes(v->body, (size_t) payload) ^ v->simplifier_epoch))
    goto bad;
  return TRUE;
bad:
  if (store != NULL)
    store->validation_failures++;
  return FALSE;
}

/* PUBLIC */
Clause_store clause_store_init(const char *name)
{
  Clause_store store = safe_calloc(1, sizeof(struct clause_store));
  (void) name;
  store->fd = -1;
  return store;
}

/* PUBLIC */
BOOL clause_store_enable_archive(Clause_store store,
                                 Clause_store_archive_mode mode)
{
  if (store == NULL || store->length != 0 ||
      mode == CLAUSE_STORE_ARCHIVE_OFF)
    return store != NULL && mode == CLAUSE_STORE_ARCHIVE_OFF;
  if (Active_archive_store != NULL && Active_archive_store != store)
    return FALSE;
  store->mode = mode;
  if (mode == CLAUSE_STORE_ARCHIVE_MEMORY) {
    Active_archive_store = store;
    return TRUE;
  }
#ifndef __EMSCRIPTEN__
  if (mode == CLAUSE_STORE_ARCHIVE_MMAP ||
      mode == CLAUSE_STORE_ARCHIVE_FILE) {
    store->fd = open_private_temp_file("prover9-ancestors-XXXXXX");
    if (store->fd < 0) {
      store->mode = CLAUSE_STORE_ARCHIVE_OFF;
      return FALSE;
    }
    Active_archive_store = store;
    return TRUE;
  }
#endif
  store->mode = CLAUSE_STORE_ARCHIVE_OFF;
  return FALSE;
}

static void release_backing(Clause_store store)
{
  if (store->mode == CLAUSE_STORE_ARCHIVE_MEMORY)
    safe_free(store->backing);
#ifndef __EMSCRIPTEN__
  else if (store->mode == CLAUSE_STORE_ARCHIVE_MMAP) {
    if (store->backing != NULL)
      munmap(store->backing, store->backing_capacity);
    if (store->fd >= 0)
      close(store->fd);
  }
  else if (store->mode == CLAUSE_STORE_ARCHIVE_FILE && store->fd >= 0)
    close(store->fd);
#endif
  safe_free(store->io_buffer);
  store->backing = NULL;
  store->backing_size = 0;
  store->backing_capacity = 0;
  store->mmap_synced_bytes = 0;
  store->mmap_last_evict_size = 0;
  store->io_buffer = NULL;
  store->io_capacity = 0;
  store->fd = -1;
  if (Active_archive_store == store)
    Active_archive_store = NULL;
}

/* PUBLIC */
void clause_store_free(Clause_store store)
{
  size_t i;
  if (store == NULL)
    return;
  for (i = 0; i < store->length; i++) {
    uintptr_t ref = store->refs[i];
    if (ref_is_archive(ref)) {
      unsigned long long id;
      unsigned long long current;
      if (!record_identity(store, ref_offset(ref), &id))
        fatal_error("clause_store_free: corrupt ancestor record");
      if (clause_id_archive_offset(id, &current) &&
          current == ref_offset(ref))
        unassign_archived_clause_id(id, ref_offset(ref));
    }
    else
      ((Topform) ref)->disabled = 0;
  }
  safe_free(store->refs);
  release_backing(store);
  safe_free(store);
}

/* PUBLIC */
void clause_store_delete_clauses(Clause_store store)
{
  size_t i;
  if (store == NULL)
    return;
  for (i = 0; i < store->length; i++) {
    uintptr_t ref = store->refs[i];
    if (ref_is_archive(ref)) {
      unsigned long long id;
      unsigned long long current;
      if (!record_identity(store, ref_offset(ref), &id))
        fatal_error("clause_store_delete_clauses: corrupt ancestor record");
      if (clause_id_archive_offset(id, &current) &&
          current == ref_offset(ref))
        unassign_archived_clause_id(id, ref_offset(ref));
    }
    else {
      Topform c = (Topform) ref;
      c->disabled = 0;
      if (c->containers == NULL)
        delete_clause(c);
    }
  }
  store->length = 0;
  store->current_records = 0;
  safe_free(store->refs);
  store->refs = NULL;
  store->capacity = 0;
  release_backing(store);
  safe_free(store);
}

/* PUBLIC */
void clause_store_append(Clause_store store, Topform c)
{
  if (store == NULL || c == NULL)
    fatal_error("clause_store_append: null argument");
  if (c->disabled)
    fatal_error("clause_store_append: clause is already disabled");
  if (store->length == store->capacity) {
    size_t new_capacity = store->capacity == 0 ? 16 : store->capacity * 2;
    if (new_capacity < store->capacity ||
        new_capacity > ((size_t) -1) / sizeof(uintptr_t))
      fatal_error("clause_store_append: capacity overflow");
    store->refs = safe_realloc(store->refs,
                               new_capacity * sizeof(uintptr_t));
    store->capacity = new_capacity;
  }
  store->refs[store->length++] = (uintptr_t) c;
  store->current_records++;
  c->disabled = 1;
}

/* PUBLIC */
BOOL clause_store_archive_clause(Clause_store store, Topform c)
{
  size_t i;
  unsigned long long offset;
  if (store == NULL || c == NULL || store->mode == CLAUSE_STORE_ARCHIVE_OFF)
    return FALSE;
  for (i = store->length; i > 0; i--)
    if (store->refs[i-1] == (uintptr_t) c)
      break;
  if (i == 0 || !append_record(store, c, &offset))
    return FALSE;
  if (!archive_clause_id(c, offset))
    fatal_error("clause_store_archive_clause: ID-table replacement failed");
  store->refs[i-1] = offset_ref(offset);
  c->disabled = 0;
  c->id = 0;
  delete_clause(c);
  return TRUE;
}

/* PUBLIC */
BOOL clause_store_archive_clause_preserve(Clause_store store, Topform c)
{
  size_t i;
  unsigned long long offset;
  Topform source;

  if (store == NULL || c == NULL || store->mode == CLAUSE_STORE_ARCHIVE_OFF ||
      c->is_formula || c->compressed != NULL || c->literals == NULL)
    return FALSE;
  for (i = store->length; i > 0; i--)
    if (store->refs[i-1] == (uintptr_t) c)
      break;
  if (i == 0)
    return FALSE;

  /* append_record compresses its input destructively.  Serialize a transient
     deep copy so indexes can continue to refer to the original term nodes. */
  source = copy_clause_ija(c);
  source->matching_hint = c->matching_hint;
  source->last_matched_given = c->last_matched_given;
  source->weight = c->weight;
  source->proof_tree_weight_cache = c->proof_tree_weight_cache;
  source->semantics = c->semantics;
  source->simplifier_epoch = c->simplifier_epoch;
  source->normal_vars = c->normal_vars;
  source->used = c->used;
  source->initial = c->initial;
  source->subsumer = c->subsumer;
  source->was_given = c->was_given;
  source->goal_derived = c->goal_derived;
  if (!append_record(store, source, &offset)) {
    delete_clause(source);
    return FALSE;
  }
  delete_clause(source);

  if (!archive_clause_id(c, offset))
    fatal_error("clause_store_archive_clause_preserve: ID replacement failed");
  store->refs[i-1] = offset_ref(offset);
  c->disabled = 0;
  return TRUE;
}

/* PUBLIC */
BOOL clause_store_member(Clause_store store, Topform c)
{
  return store != NULL && c != NULL && c->disabled;
}

/* PUBLIC */
size_t clause_store_length(Clause_store store)
{
  return store == NULL ? 0 : store->length;
}

/* PUBLIC */
size_t clause_store_current_length(Clause_store store)
{
  return store == NULL ? 0 : store->current_records;
}

/* PUBLIC */
BOOL clause_store_position_is_archived(Clause_store store, size_t position)
{
  if (store == NULL || position >= store->length)
    return FALSE;
  return ref_is_archive(store->refs[position]);
}

/* PUBLIC */
BOOL clause_store_position_is_current(Clause_store store, size_t position)
{
  uintptr_t ref;
  struct record_view v;
  unsigned long long current;
  if (store == NULL || position >= store->length)
    return FALSE;
  ref = store->refs[position];
  if (!ref_is_archive(ref))
    return TRUE;
  if (!record_view(store, ref_offset(ref), &v))
    fatal_error("clause_store_position_is_current: corrupt ancestor record");
  return clause_id_archive_offset(v.id, &current) &&
         current == ref_offset(ref);
}

/* PUBLIC */
BOOL clause_store_payload_sizes(Clause_store store, size_t position,
                                unsigned long long *body_bytes,
                                unsigned long long *justification_bytes,
                                unsigned long long *logical_body_bytes)
{
  uintptr_t ref;
  struct record_view v;
  if (store == NULL || position >= store->length)
    return FALSE;
  ref = store->refs[position];
  if (!ref_is_archive(ref))
    return FALSE;
  if (!record_view(store, ref_offset(ref), &v))
    return FALSE;
  if (body_bytes != NULL)
    *body_bytes = v.body_size;
  if (justification_bytes != NULL)
    *justification_bytes = v.just_size;
  if (logical_body_bytes != NULL)
    *logical_body_bytes = v.logical_body;
  return TRUE;
}  /* clause_store_payload_sizes */

/* PUBLIC */
unsigned long long clause_store_id(Clause_store store, size_t position)
{
  uintptr_t ref;
  struct record_view v;
  if (store == NULL || position >= store->length)
    fatal_error("clause_store_id: position out of range");
  ref = store->refs[position];
  if (!ref_is_archive(ref))
    return ((Topform) ref)->id;
  if (!record_view(store, ref_offset(ref), &v))
    fatal_error("clause_store_id: corrupt ancestor record");
  return v.id;
}

/* PUBLIC */
BOOL clause_store_negative(Clause_store store, size_t position)
{
  uintptr_t ref;
  struct record_view v;
  if (store == NULL || position >= store->length)
    return FALSE;
  ref = store->refs[position];
  if (!ref_is_archive(ref))
    return negative_clause_possibly_compressed((Topform) ref);
  if (!record_view(store, ref_offset(ref), &v))
    fatal_error("clause_store_negative: corrupt ancestor record");
  return (v.flags & AF_NEGATIVE) != 0;
}

/* PUBLIC */
Ilist clause_store_parents(Clause_store store, size_t position)
{
  uintptr_t ref;
  struct record_view v;
  Ilist parents = NULL;
  Ilist *tail = &parents;
  unsigned i;
  if (store == NULL || position >= store->length)
    return NULL;
  ref = store->refs[position];
  if (!ref_is_archive(ref))
    return get_parents(((Topform) ref)->justification, TRUE);
  if (!record_view(store, ref_offset(ref), &v))
    fatal_error("clause_store_parents: corrupt ancestor record");
  for (i = 0; i < v.parent_count; i++) {
    uint64_t id = get64(v.parents + (size_t) i * 8);
    Ilist n;
    if (id > INT_MAX)
      fatal_error("clause_store_parents: parent ID exceeds justification range");
    n = get_ilist();
    n->i = (int) id;
    n->next = NULL;
    *tail = n;
    tail = &n->next;
  }
  return parents;
}

/* PUBLIC */
Topform clause_store_get(Clause_store store, size_t position)
{
  if (store == NULL || position >= store->length)
    fatal_error("clause_store_get: position out of range");
  if (ref_is_archive(store->refs[position]))
    fatal_error("clause_store_get: archived clause requires materialization");
  return (Topform) store->refs[position];
}

/* PUBLIC */
Topform clause_store_materialize(Clause_store store, size_t position)
{
  uintptr_t ref;
  struct record_view v;
  Topform c;
  Term t;
  double weight;
  Ilist parents = NULL, p;
  unsigned parent_count = 0;
  if (store == NULL || position >= store->length)
    return NULL;
  ref = store->refs[position];
  if (!ref_is_archive(ref))
    return (Topform) ref;
  if (!record_view(store, ref_offset(ref), &v))
    return NULL;
  if (sizeof(double) != sizeof(uint64_t))
    fatal_error("clause_store_materialize: ancestor format requires 64-bit double");
  c = get_topform();
  c->id = v.id;
  c->semantics = v.semantics;
  c->simplifier_epoch = v.simplifier_epoch;
  memcpy(&weight, &v.weight_bits, sizeof(weight));
  c->weight = weight;
  c->last_matched_given = v.last_matched;
  c->proof_tree_weight_cache = v.proof_cache;
  c->is_formula = (v.flags & AF_IS_FORMULA) != 0;
  c->normal_vars = (v.flags & AF_NORMAL_VARS) != 0;
  c->used = (v.flags & AF_USED) != 0;
  c->initial = (v.flags & AF_INITIAL) != 0;
  c->subsumer = (v.flags & AF_SUBSUMER) != 0;
  c->was_given = (v.flags & AF_WAS_GIVEN) != 0;
  c->goal_derived = (v.flags & AF_GOAL) != 0;
  c->disabled = 1;
  c->archive_materialized = 1;
  if (v.body_size != 0) {
    c->compressed = safe_malloc(v.body_size);
    memcpy(c->compressed, v.body, v.body_size);
    c->compressed_size = v.body_size;
    c->uncompressed_body_bytes = v.logical_body;
    c->neg_compressed = (v.flags & AF_NEGATIVE) != 0;
    if (!materialize_clause(c))
      goto bad;
  }
  if (v.formula_size != 0) {
    t = decode_term_versioned((const char *) v.formula, v.formula_size);
    if (t == NULL)
      goto bad;
    c->formula = term_to_formula(t);
    zap_term(t);
    if (c->formula == NULL)
      goto bad;
  }
  if (v.attribute_size != 0) {
    t = decode_term_versioned((const char *) v.attributes, v.attribute_size);
    if (t == NULL)
      goto bad;
    c->attributes = term_to_attributes(t, "#");
    zap_term(t);
  }
  c->justification = decode_justification((const char *) v.justification,
                                          v.just_size);
  if (c->justification == NULL &&
      !(v.just_size == 5 && v.justification[0] == 'P' &&
        v.justification[1] == '9' && v.justification[2] == 'J' &&
        v.justification[3] == 1 && v.justification[4] == 0))
    goto bad;
  parents = get_parents(c->justification, TRUE);
  for (p = parents; p != NULL; p = p->next, parent_count++)
    if (parent_count >= v.parent_count || p->i < 0 ||
        (uint64_t) (unsigned) p->i !=
        get64(v.parents + (size_t) parent_count * 8))
      goto bad;
  if (parent_count != v.parent_count)
    goto bad;
  zap_ilist(parents);
  store->materializations++;
  return c;
bad:
  zap_ilist(parents);
  store->validation_failures++;
  c->id = 0;
  c->disabled = 0;
  delete_clause(c);
  return NULL;
}

/* PUBLIC */
Topform clause_store_activate(Clause_store store, size_t position)
{
  uintptr_t ref;
  Topform c;
  if (store == NULL || position >= store->length)
    return NULL;
  ref = store->refs[position];
  if (!ref_is_archive(ref))
    return (Topform) ref;
  c = clause_store_materialize(store, position);
  if (c == NULL)
    return NULL;
  if (!activate_archived_clause_id(c, ref_offset(ref))) {
    clause_store_release_materialized(c);
    return NULL;
  }
  if (store->current_records == 0)
    fatal_error("clause_store_activate: current-record underflow");
  store->current_records--;
  c->archive_materialized = 0;
  c->disabled = 0;
  return c;
}

/* PUBLIC */
Topform clause_store_materialize_by_id(unsigned long long id)
{
  unsigned long long offset;
  size_t i;
  if (Active_archive_store == NULL ||
      !clause_id_archive_offset(id, &offset))
    return NULL;
  for (i = 0; i < Active_archive_store->length; i++)
    if (ref_is_archive(Active_archive_store->refs[i]) &&
        ref_offset(Active_archive_store->refs[i]) == offset) {
      Topform c = clause_store_materialize(Active_archive_store, i);
      if (c != NULL && c->id != id) {
        Active_archive_store->validation_failures++;
        clause_store_release_materialized(c);
        return NULL;
      }
      return c;
    }
  Active_archive_store->validation_failures++;
  return NULL;
}

/* PUBLIC */
Ilist clause_parents_by_id(unsigned long long id)
{
  Topform c = find_clause_by_id(id);
  unsigned long long offset;
  size_t i;
  if (c != NULL) {
    BOOL was_packed = c->compressed != NULL && c->packed_justification;
    Ilist parents;
    if (was_packed && !materialize_clause(c))
      fatal_error("clause_parents_by_id: corrupt cold clause");
    parents = get_parents(c->justification, TRUE);
    if (was_packed && !recompress_clause(c))
      fatal_error("clause_parents_by_id: cannot restore cold clause");
    return parents;
  }
  if (Active_archive_store == NULL ||
      !clause_id_archive_offset(id, &offset))
    return NULL;
  for (i = 0; i < Active_archive_store->length; i++)
    if (ref_is_archive(Active_archive_store->refs[i]) &&
        ref_offset(Active_archive_store->refs[i]) == offset) {
      struct record_view v;
      if (!record_view(Active_archive_store, offset, &v) || v.id != id)
        fatal_error("clause_parents_by_id: corrupt ancestor identity");
      return clause_store_parents(Active_archive_store, i);
    }
  fatal_error("clause_parents_by_id: missing ancestor record");
  return NULL;
}

/* PUBLIC */
BOOL clause_negative_by_id(unsigned long long id, BOOL *known)
{
  Topform c = find_clause_by_id(id);
  unsigned long long offset;
  size_t i;
  if (known != NULL)
    *known = FALSE;
  if (c != NULL) {
    if (known != NULL) *known = TRUE;
    return negative_clause_possibly_compressed(c);
  }
  if (Active_archive_store == NULL ||
      !clause_id_archive_offset(id, &offset))
    return FALSE;
  for (i = 0; i < Active_archive_store->length; i++) {
    uintptr_t ref = Active_archive_store->refs[i];
    if (ref_is_archive(ref) && ref_offset(ref) == offset) {
      struct record_view v;
      if (!record_view(Active_archive_store, offset, &v) || v.id != id)
        fatal_error("clause_negative_by_id: corrupt ancestor record");
      if (known != NULL) *known = TRUE;
      return (v.flags & AF_NEGATIVE) != 0;
    }
  }
  fatal_error("clause_negative_by_id: missing ancestor record");
  return FALSE;
}

/* PUBLIC */
unsigned long long clause_store_matching_hint_id(unsigned long long id)
{
  unsigned long long offset;
  size_t i;
  if (Active_archive_store == NULL ||
      !clause_id_archive_offset(id, &offset))
    return 0;
  for (i = 0; i < Active_archive_store->length; i++) {
    uintptr_t ref = Active_archive_store->refs[i];
    if (ref_is_archive(ref) && ref_offset(ref) == offset) {
      struct record_view v;
      if (!record_view(Active_archive_store, offset, &v) || v.id != id)
        fatal_error("clause_store_matching_hint_id: corrupt ancestor record");
      return v.hint_id;
    }
  }
  fatal_error("clause_store_matching_hint_id: missing ancestor record");
  return 0;
}

/* PUBLIC */
void clause_store_release_materialized(Topform c)
{
  if (c != NULL && c->archive_materialized) {
    c->archive_materialized = 0;
    c->disabled = 0;
    c->id = 0;
    delete_clause(c);
  }
}

/* PUBLIC */
void clause_store_release_materialized_plist(Plist clauses)
{
  Plist p;
  for (p = clauses; p != NULL; p = p->next)
    clause_store_release_materialized((Topform) p->v);
}

static Clause_store Sort_store;
static int compare_refs(const void *a, const void *b)
{
  uintptr_t ra = *(const uintptr_t *) a;
  uintptr_t rb = *(const uintptr_t *) b;
  unsigned long long ia, ib;
  if (ref_is_archive(ra)) {
    struct record_view v;
    if (!record_view(Sort_store, ref_offset(ra), &v))
      fatal_error("compare_refs: corrupt ancestor record");
    ia = v.id;
  }
  else
    ia = ((Topform) ra)->id;
  if (ref_is_archive(rb)) {
    struct record_view v;
    if (!record_view(Sort_store, ref_offset(rb), &v))
      fatal_error("compare_refs: corrupt ancestor record");
    ib = v.id;
  }
  else
    ib = ((Topform) rb)->id;
  return ia < ib ? -1 : ia > ib ? 1 : 0;
}

/* PUBLIC */
void clause_store_sort_by_id(Clause_store store)
{
  if (store != NULL && store->length > 1) {
    Sort_store = store;
    qsort(store->refs, store->length, sizeof(uintptr_t), compare_refs);
    Sort_store = NULL;
  }
}

/* PUBLIC */
BOOL clause_store_sync(Clause_store store)
{
  if (store == NULL)
    return FALSE;
#ifndef __EMSCRIPTEN__
  if (store->mode == CLAUSE_STORE_ARCHIVE_MMAP && store->backing != NULL)
    return msync(store->backing, store->backing_size, MS_SYNC) == 0;
  if (store->mode == CLAUSE_STORE_ARCHIVE_FILE)
    return fsync(store->fd) == 0;
#endif
  return TRUE;
}

/* PUBLIC */
void clause_store_advise_mmap_range_cold(Clause_store store,
                                         size_t first_position,
                                         size_t last_position)
{
#ifndef __EMSCRIPTEN__
  uintptr_t first_ref, last_ref;
  struct record_view last_view;
  long page_size_long;
  size_t page_size, begin, end, last_offset, sync_start;
  if (store == NULL || store->mode != CLAUSE_STORE_ARCHIVE_MMAP ||
      store->backing == NULL || first_position > last_position ||
      last_position >= store->length)
    return;
  first_ref = store->refs[first_position];
  last_ref = store->refs[last_position];
  if (!ref_is_archive(first_ref) || !ref_is_archive(last_ref))
    return;
  page_size_long = sysconf(_SC_PAGESIZE);
  if (page_size_long <= 0)
    return;
  page_size = (size_t) page_size_long;
  last_offset = (size_t) ref_offset(last_ref);
  if (!record_view(store, ref_offset(last_ref), &last_view) ||
      last_view.total > SIZE_MAX - last_offset)
    return;
  begin = (size_t) ref_offset(first_ref) / page_size * page_size;
  end = last_offset + (size_t) last_view.total;
  if (end % page_size != 0) {
    if (end > SIZE_MAX - (page_size - end % page_size))
      return;
    end += page_size - end % page_size;
  }
  if (end > store->backing_capacity)
    end = store->backing_capacity;
  if (begin >= end)
    return;

  /* MADV_DONTNEED on a shared mapping must not be allowed to race ahead of
     dirty archive bytes.  mmap_synced_bytes names the contiguous durable
     prefix, so synchronize any newly covered suffix exactly once. */
  if (store->mmap_synced_bytes < end) {
    sync_start = store->mmap_synced_bytes / page_size * page_size;
    if (msync(store->backing + sync_start, end - sync_start, MS_SYNC) != 0)
      return;
    store->mmap_synced_bytes = end;
  }
  if (madvise(store->backing + begin, end - begin, MADV_DONTNEED) == 0) {
    store->mmap_eviction_passes++;
    store->mmap_eviction_bytes += end - begin;
    store->mmap_scan_eviction_passes++;
    store->mmap_scan_eviction_bytes += end - begin;
  }
#else
  (void) store;
  (void) first_position;
  (void) last_position;
#endif
}

/* PUBLIC */
struct clause_store_stats clause_store_get_stats(Clause_store store)
{
  struct clause_store_stats stats;
  memset(&stats, 0, sizeof(stats));
  if (store != NULL) {
    stats.records = store->archive_records;
    stats.body_bytes = store->archive_body_bytes;
    stats.logical_body_bytes = store->archive_logical_body_bytes;
    stats.record_bytes = store->backing_size;
    stats.backing_bytes = store->backing_capacity;
    stats.handle_bytes = sizeof(*store) + store->capacity * sizeof(uintptr_t) +
      store->io_capacity;
    stats.materializations = store->materializations;
    stats.validation_failures = store->validation_failures;
    stats.mmap_eviction_passes = store->mmap_eviction_passes;
    stats.mmap_eviction_bytes = store->mmap_eviction_bytes;
    stats.mmap_scan_eviction_passes = store->mmap_scan_eviction_passes;
    stats.mmap_scan_eviction_bytes = store->mmap_scan_eviction_bytes;
    stats.io_buffer_bytes = store->io_capacity;
    stats.file_reads = store->file_reads;
    stats.file_read_bytes = store->file_read_bytes;
    stats.file_writes = store->file_writes;
    stats.file_write_bytes = store->file_write_bytes;
  }
  return stats;
}

/* PUBLIC */
unsigned long long clause_store_allocated_bytes(Clause_store store)
{
  unsigned long long backing;
  if (store == NULL)
    return 0;
  backing = store->mode == CLAUSE_STORE_ARCHIVE_FILE ?
    store->io_capacity : store->backing_capacity;
  return (unsigned long long) sizeof(struct clause_store) +
    (unsigned long long) store->capacity * sizeof(uintptr_t) + backing;
}

/* PUBLIC */
unsigned long long clause_store_legacy_clist_bytes(Clause_store store)
{
  return store == NULL ? 0 :
    (unsigned long long) sizeof(struct clist) +
    (unsigned long long) store->length * sizeof(struct clist_pos);
}

/* PUBLIC */
BOOL clause_store_test_corrupt(Clause_store store,
                               unsigned long long absolute_offset,
                               unsigned char mask)
{
  unsigned char byte;
  if (store == NULL || absolute_offset >= store->backing_size || mask == 0)
    return FALSE;
#ifndef __EMSCRIPTEN__
  if (store->mode == CLAUSE_STORE_ARCHIVE_FILE) {
    if (!file_read_exact(store, &byte, 1, (size_t) absolute_offset))
      return FALSE;
    byte ^= mask;
    return file_write_exact(store, &byte, 1, (size_t) absolute_offset);
  }
#endif
  store->backing[(size_t) absolute_offset] ^= mask;
  return TRUE;
}
