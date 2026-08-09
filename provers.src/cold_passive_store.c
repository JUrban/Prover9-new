/* Dedicated compact arena for passive clauses.

   Unlike the proof-ancestor store, this format does not carry an ID,
   matching-hint metadata, selector keys, parent vector, or a 96-byte general
   record header.  Those values already live in the dense selector record,
   and the packed clause blob already contains its complete justification. */

#include "cold_passive_store.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#ifndef __EMSCRIPTEN__
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

#define COLD_MAGIC "P9PS"
#define COLD_VERSION 1
#define COLD_HEADER_SIZE 40

#define CF_NORMAL_VARS 0x0001U
#define CF_USED        0x0002U
#define CF_INITIAL     0x0004U
#define CF_SUBSUMER    0x0008U
#define CF_GOAL        0x0010U

struct cold_passive_store {
  Cold_passive_store_mode mode;
  unsigned char *backing;
  size_t size;
  size_t capacity;
  int fd;
  unsigned char *io_buffer;
  size_t io_capacity;
  unsigned long long records;
  unsigned long long materializations;
  unsigned long long validation_failures;
  unsigned long long file_reads;
  unsigned long long file_read_bytes;
  unsigned long long file_writes;
  unsigned long long file_write_bytes;
};

struct cold_record_view {
  const unsigned char *record;
  const unsigned char *body;
  const unsigned char *attributes;
  uint32_t total;
  uint32_t body_size;
  uint32_t attribute_size;
  uint32_t logical_body_size;
  int proof_cache;
  uint16_t flags;
  uint64_t last_matched;
};

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
static BOOL file_read_exact(Cold_passive_store store, void *buffer,
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

static BOOL file_write_exact(Cold_passive_store store, const void *buffer,
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

static BOOL ensure_io_buffer(Cold_passive_store store, size_t needed)
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

static BOOL ensure_backing(Cold_passive_store store, size_t needed)
{
  size_t capacity;
  if (needed <= store->capacity)
    return TRUE;
  capacity = store->capacity == 0 ? 4096 : store->capacity;
  while (capacity < needed) {
    size_t grown = capacity + capacity / 2;
    if (grown <= capacity)
      return FALSE;
    capacity = grown;
  }
  if (store->mode == COLD_PASSIVE_MEMORY) {
    store->backing = safe_realloc(store->backing, capacity);
    store->capacity = capacity;
    return TRUE;
  }
#ifndef __EMSCRIPTEN__
  if (store->mode == COLD_PASSIVE_FILE) {
    /* pwrite extends the file at commit time.  Capacity is logical only and
       deliberately does not reserve or map every future byte. */
    store->capacity = needed;
    return TRUE;
  }
#endif
#ifndef __EMSCRIPTEN__
  if (store->mode == COLD_PASSIVE_MMAP) {
    void *mapping;
    if (ftruncate(store->fd, (off_t) capacity) != 0)
      return FALSE;
    if (store->backing != NULL &&
        munmap(store->backing, store->capacity) != 0)
      return FALSE;
    store->backing = NULL;
    mapping = mmap(NULL, capacity, PROT_READ | PROT_WRITE, MAP_SHARED,
                   store->fd, 0);
    if (mapping == MAP_FAILED)
      return FALSE;
    store->backing = mapping;
    store->capacity = capacity;
    return TRUE;
  }
#endif
  return FALSE;
}

static BOOL append_record(Cold_passive_store store,
                          const unsigned char *record, size_t size)
{
  if (store == NULL || record == NULL || size == 0 ||
      store->size > SIZE_MAX - size ||
      !ensure_backing(store, store->size + size))
    return FALSE;
#ifndef __EMSCRIPTEN__
  if (store->mode == COLD_PASSIVE_FILE) {
    if (!file_write_exact(store, record, size, store->size))
      return FALSE;
  }
  else
#endif
    memcpy(store->backing + store->size, record, size);
  store->size += size;
  return TRUE;
}

/* Return a contiguous record image.  Memory and mmap stores already own one;
   the file store reads into its single reusable scratch buffer. */
static const unsigned char *record_image(Cold_passive_store store,
                                         size_t position)
{
  const unsigned char *r;
  uint32_t total;
  if (store == NULL || position > store->size ||
      store->size - position < COLD_HEADER_SIZE)
    return NULL;
#ifndef __EMSCRIPTEN__
  if (store->mode == COLD_PASSIVE_FILE) {
    unsigned char header[COLD_HEADER_SIZE];
    if (!file_read_exact(store, header, sizeof(header), position) ||
        memcmp(header, COLD_MAGIC, 4) != 0 ||
        get16(header + 4) != COLD_VERSION)
      return NULL;
    total = get32(header + 8);
    if (total < COLD_HEADER_SIZE || total > store->size - position ||
        !ensure_io_buffer(store, total) ||
        !file_read_exact(store, store->io_buffer, total, position))
      return NULL;
    return store->io_buffer;
  }
#endif
  r = store->backing + position;
  return r;
}

static BOOL record_view(Cold_passive_store store, size_t position,
                        struct cold_record_view *view)
{
  const unsigned char *r;
  uint64_t payload;
  if (store == NULL || view == NULL || position > store->size ||
      store->size - position < COLD_HEADER_SIZE)
    goto bad;
  r = record_image(store, position);
  if (r == NULL)
    goto bad;
  if (memcmp(r, COLD_MAGIC, 4) != 0 ||
      get16(r + 4) != COLD_VERSION ||
      (get16(r + 6) & ~0x001fU) != 0)
    goto bad;
  memset(view, 0, sizeof(*view));
  view->record = r;
  view->flags = (uint16_t) get16(r + 6);
  view->total = get32(r + 8);
  view->body_size = get32(r + 12);
  view->attribute_size = get32(r + 16);
  view->logical_body_size = get32(r + 20);
  view->proof_cache = (int32_t) get32(r + 24);
  view->last_matched = get64(r + 32);
  payload = (uint64_t) view->body_size + view->attribute_size;
  if (view->body_size == 0 ||
      view->total != COLD_HEADER_SIZE + payload ||
      view->total > store->size - position)
    goto bad;
  view->body = r + COLD_HEADER_SIZE;
  view->attributes = view->body + view->body_size;
  if (get32(r + 28) != crc32_bytes(view->body, (size_t) payload))
    goto bad;
  return TRUE;
bad:
  if (store != NULL)
    store->validation_failures++;
  return FALSE;
}

Cold_passive_store cold_passive_store_init(Cold_passive_store_mode mode)
{
  Cold_passive_store store = safe_calloc(1, sizeof(*store));
  store->mode = mode;
  store->fd = -1;
#ifndef __EMSCRIPTEN__
  if (mode == COLD_PASSIVE_MMAP || mode == COLD_PASSIVE_FILE) {
    char path[] = "/tmp/prover9-passive-XXXXXX";
    store->fd = mkstemp(path);
    if (store->fd < 0) {
      safe_free(store);
      return NULL;
    }
    unlink(path);
  }
#else
  if (mode == COLD_PASSIVE_MMAP || mode == COLD_PASSIVE_FILE) {
    safe_free(store);
    return NULL;
  }
#endif
  return store;
}

void cold_passive_store_free(Cold_passive_store store)
{
  if (store == NULL)
    return;
  if (store->mode == COLD_PASSIVE_MEMORY)
    safe_free(store->backing);
#ifndef __EMSCRIPTEN__
  else if (store->mode == COLD_PASSIVE_MMAP) {
    if (store->backing != NULL)
      munmap(store->backing, store->capacity);
    if (store->fd >= 0)
      close(store->fd);
  }
  else if (store->fd >= 0)
    close(store->fd);
#endif
  safe_free(store->io_buffer);
  safe_free(store);
}

size_t cold_passive_store_archive(Cold_passive_store store, Topform c,
                                  unsigned *body_bytes,
                                  unsigned *justification_bytes,
                                  unsigned *logical_body_bytes)
{
  char *attribute_data = NULL;
  unsigned attribute_size = 0;
  Term attribute_term = NULL;
  unsigned char *record = NULL;
  uint64_t total;
  size_t position;
  unsigned flags = 0;
  unsigned justification_size;
  if (store == NULL || c == NULL || c->id == 0)
    return SIZE_MAX;
  if (c->compressed == NULL) {
    Clause_compress_result cr = compress_clause_with_justification(c);
    if (cr != CLAUSE_COMPRESS_OK && cr != CLAUSE_COMPRESS_ALREADY)
      return SIZE_MAX;
  }
  if (!compressed_clause_is_valid(c) || !c->packed_justification)
    return SIZE_MAX;
  justification_size = compressed_clause_justification_bytes(c);
  if (justification_size > c->compressed_size)
    return SIZE_MAX;
  if (c->attributes != NULL) {
    attribute_term = attributes_to_term(c->attributes, "#");
    if (!encode_term_versioned(attribute_term, &attribute_data,
                               &attribute_size))
      goto bad;
  }
  total = COLD_HEADER_SIZE + (uint64_t) c->compressed_size + attribute_size;
  if (total > UINT32_MAX || store->size > SIZE_MAX - (size_t) total)
    goto bad;
  position = store->size;
  record = safe_malloc((size_t) total);
  memset(record, 0, COLD_HEADER_SIZE);
  memcpy(record, COLD_MAGIC, 4);
  if (c->normal_vars) flags |= CF_NORMAL_VARS;
  if (c->used) flags |= CF_USED;
  if (c->initial) flags |= CF_INITIAL;
  if (c->subsumer) flags |= CF_SUBSUMER;
  if (c->goal_derived) flags |= CF_GOAL;
  put16(record + 4, COLD_VERSION);
  put16(record + 6, flags);
  put32(record + 8, (uint32_t) total);
  put32(record + 12, c->compressed_size);
  put32(record + 16, attribute_size);
  put32(record + 20, c->uncompressed_body_bytes);
  put32(record + 24, (uint32_t) c->proof_tree_weight_cache);
  put64(record + 32, c->last_matched_given);
  memcpy(record + COLD_HEADER_SIZE, c->compressed, c->compressed_size);
  if (attribute_size != 0)
    memcpy(record + COLD_HEADER_SIZE + c->compressed_size,
           attribute_data, attribute_size);
  put32(record + 28,
        crc32_bytes(record + COLD_HEADER_SIZE,
                    c->compressed_size + attribute_size));
  if (!append_record(store, record, (size_t) total))
    goto bad;
  if (!detach_clause_id(c))
    goto bad;
  store->records++;
  if (body_bytes != NULL)
    *body_bytes = c->compressed_size - justification_size;
  if (justification_bytes != NULL)
    *justification_bytes = justification_size;
  if (logical_body_bytes != NULL)
    *logical_body_bytes = c->uncompressed_body_bytes;
  if (attribute_term != NULL)
    zap_term(attribute_term);
  safe_free(attribute_data);
  safe_free(record);
  delete_clause(c);
  return position;
bad:
  if (attribute_term != NULL)
    zap_term(attribute_term);
  safe_free(attribute_data);
  safe_free(record);
  return SIZE_MAX;
}

Topform cold_passive_store_materialize(Cold_passive_store store,
                                       size_t position,
                                       unsigned long long id,
                                       BOOL activate)
{
  struct cold_record_view view;
  Topform c;
  Term t;
  if (id == 0 || !record_view(store, position, &view))
    return NULL;
  c = get_topform();
  c->id = id;
  c->normal_vars = (view.flags & CF_NORMAL_VARS) != 0;
  c->used = (view.flags & CF_USED) != 0;
  c->initial = (view.flags & CF_INITIAL) != 0;
  c->subsumer = (view.flags & CF_SUBSUMER) != 0;
  c->goal_derived = (view.flags & CF_GOAL) != 0;
  c->proof_tree_weight_cache = view.proof_cache;
  c->last_matched_given = view.last_matched;
  c->compressed = safe_malloc(view.body_size);
  memcpy(c->compressed, view.body, view.body_size);
  c->compressed_size = view.body_size;
  c->uncompressed_body_bytes = view.logical_body_size;
  c->packed_justification = TRUE;
  if (view.attribute_size != 0) {
    t = decode_term_versioned((const char *) view.attributes,
                              view.attribute_size);
    if (t == NULL)
      goto bad;
    c->attributes = term_to_attributes(t, "#");
    zap_term(t);
  }
  if (!materialize_clause(c))
    goto bad;
  if (activate)
    register_clause_with_id(c);
  else
    c->archive_materialized = 1;
  store->materializations++;
  return c;
bad:
  store->validation_failures++;
  c->id = 0;
  delete_clause(c);
  return NULL;
}

void cold_passive_store_release(Topform c)
{
  if (c != NULL && c->archive_materialized) {
    c->archive_materialized = 0;
    c->id = 0;
    delete_clause(c);
  }
}

BOOL cold_passive_store_payload_sizes(
  Cold_passive_store store, size_t position,
  unsigned long long *body_bytes,
  unsigned long long *justification_bytes,
  unsigned long long *logical_body_bytes)
{
  struct cold_record_view view;
  Topform temporary;
  unsigned just_bytes;
  if (!record_view(store, position, &view))
    return FALSE;
  /* Read the packed-justification length through the codec's checked helper
     without decoding the clause. */
  temporary = get_topform();
  temporary->compressed = (char *) view.body;
  temporary->compressed_size = view.body_size;
  just_bytes = compressed_clause_justification_bytes(temporary);
  temporary->compressed = NULL;
  temporary->compressed_size = 0;
  zap_topform(temporary);
  if (body_bytes != NULL)
    *body_bytes = view.body_size - just_bytes;
  if (justification_bytes != NULL)
    *justification_bytes = just_bytes;
  if (logical_body_bytes != NULL)
    *logical_body_bytes = view.logical_body_size;
  return TRUE;
}

size_t cold_passive_store_clone_record(Cold_passive_store source,
                                       size_t position,
                                       Cold_passive_store destination)
{
  struct cold_record_view view;
  size_t new_position;
  if (source == NULL || destination == NULL || source == destination ||
      !record_view(source, position, &view) ||
      destination->size > SIZE_MAX - view.total)
    return SIZE_MAX;
  new_position = destination->size;
  if (!append_record(destination, view.record, view.total))
    return SIZE_MAX;
  destination->records++;
  return new_position;
}

void cold_passive_store_inherit_counters(Cold_passive_store destination,
                                         Cold_passive_store source)
{
  if (destination != NULL && source != NULL) {
    destination->materializations += source->materializations;
    destination->validation_failures += source->validation_failures;
    destination->file_reads += source->file_reads;
    destination->file_read_bytes += source->file_read_bytes;
    destination->file_writes += source->file_writes;
    destination->file_write_bytes += source->file_write_bytes;
  }
}

BOOL cold_passive_store_sync(Cold_passive_store store)
{
  if (store == NULL)
    return TRUE;
#ifndef __EMSCRIPTEN__
  if (store->mode == COLD_PASSIVE_MMAP && store->backing != NULL)
    return msync(store->backing, store->capacity, MS_SYNC) == 0;
  if (store->mode == COLD_PASSIVE_FILE)
    return fsync(store->fd) == 0;
#endif
  return TRUE;
}

const char *cold_passive_store_mode_name(Cold_passive_store_mode mode)
{
  switch (mode) {
  case COLD_PASSIVE_MEMORY: return "memory";
  case COLD_PASSIVE_MMAP:   return "mmap";
  case COLD_PASSIVE_FILE:   return "file";
  default:                  return "unknown";
  }
}

struct cold_passive_store_stats
cold_passive_store_get_stats(Cold_passive_store store)
{
  struct cold_passive_store_stats stats;
  memset(&stats, 0, sizeof(stats));
  if (store != NULL) {
    struct stat st;
    stats.mode = store->mode;
    stats.records = store->records;
    stats.record_bytes = store->size;
    stats.backing_bytes = store->mode == COLD_PASSIVE_FILE ?
      store->io_capacity : store->capacity;
#ifndef __EMSCRIPTEN__
    if (store->fd >= 0 && fstat(store->fd, &st) == 0)
      stats.physical_bytes = (unsigned long long) st.st_blocks * 512ULL;
    else
#endif
      stats.physical_bytes = store->size;
    stats.materializations = store->materializations;
    stats.validation_failures = store->validation_failures;
    stats.file_reads = store->file_reads;
    stats.file_read_bytes = store->file_read_bytes;
    stats.file_writes = store->file_writes;
    stats.file_write_bytes = store->file_write_bytes;
  }
  return stats;
}
