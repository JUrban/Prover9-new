#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#define _GNU_SOURCE
#endif
#include "compact_id_map.h"

#include <stdint.h>
#include <string.h>
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <sys/mman.h>
#endif

#define CIDM_PAGE_BYTES (64U * 1024U)
#define CIDM_BASE_SHIFT 14U
#define CIDS_PAGE_BYTES (4U * 1024U)
#define CIDS_PAGE_SHIFT 15U
#define CIDS_WORD_BITS 64U

struct compact_id_page {
  uint64_t number;
  uint32_t *values;
  BOOL mapped;
};

struct compact_id_map {
  uint64_t *page_keys;
  struct compact_id_page **pages;
  size_t directory_capacity;
  size_t page_count;
  size_t count;
  unsigned value_words;
  unsigned page_shift;
  size_t entries_per_page;
  unsigned long long peak_bytes;
};

struct compact_id_set_page {
  uint64_t number;
  uint64_t *bits;
  BOOL mapped;
};

struct compact_id_set {
  uint64_t *page_keys;
  struct compact_id_set_page **pages;
  size_t directory_capacity;
  size_t page_count;
  size_t count;
};

static uint64_t hash_id(uint64_t x)
{
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  x ^= x >> 31;
  return x;
}

static unsigned long long map_bytes(Compact_id_map map)
{
  if (map == NULL)
    return 0;
  return sizeof(*map) +
    (unsigned long long) map->directory_capacity *
      (sizeof(*map->page_keys) + sizeof(*map->pages)) +
    (unsigned long long) map->page_count *
      (sizeof(struct compact_id_page) + CIDM_PAGE_BYTES);
}

static void update_peak(Compact_id_map map)
{
  unsigned long long bytes = map_bytes(map);
  if (bytes > map->peak_bytes)
    map->peak_bytes = bytes;
}

static size_t directory_slot(Compact_id_map map, uint64_t page_number)
{
  uint64_t key = page_number + 1;
  size_t at = (size_t) hash_id(page_number) &
              (map->directory_capacity - 1);
  while (map->page_keys[at] != 0 && map->page_keys[at] != key)
    at = (at + 1) & (map->directory_capacity - 1);
  return at;
}

static void resize_directory(Compact_id_map map, size_t capacity)
{
  uint64_t *old_keys = map->page_keys;
  struct compact_id_page **old_pages = map->pages;
  size_t old_capacity = map->directory_capacity;
  size_t i;
  map->page_keys = safe_calloc(capacity, sizeof(*map->page_keys));
  map->pages = safe_calloc(capacity, sizeof(*map->pages));
  map->directory_capacity = capacity;
  for (i = 0; i < old_capacity; i++)
    if (old_keys[i] != 0) {
      struct compact_id_page *page = old_pages[i];
      size_t at = directory_slot(map, page->number);
      map->page_keys[at] = page->number + 1;
      map->pages[at] = page;
    }
  safe_free(old_keys);
  safe_free(old_pages);
  update_peak(map);
}

static struct compact_id_page *new_page(uint64_t page_number)
{
  struct compact_id_page *page = safe_calloc(1, sizeof(*page));
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
  void *p = mmap(NULL, CIDM_PAGE_BYTES, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED)
    fatal_error("compact_id_map: cannot allocate direct page");
  page->values = p;
  page->mapped = TRUE;
#else
  page->values = safe_calloc(CIDM_PAGE_BYTES / sizeof(uint32_t),
                             sizeof(*page->values));
#endif
  page->number = page_number;
  return page;
}

static void free_page(struct compact_id_page *page)
{
  if (page == NULL)
    return;
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
  if (page->mapped) {
    if (munmap(page->values, CIDM_PAGE_BYTES) != 0)
      fatal_error("compact_id_map: cannot release direct page");
  }
  else
    safe_free(page->values);
#else
  safe_free(page->values);
#endif
  safe_free(page);
}

static struct compact_id_page *find_page(Compact_id_map map,
                                         uint64_t page_number,
                                         BOOL create)
{
  size_t at;
  if (map->directory_capacity == 0) {
    if (!create)
      return NULL;
    resize_directory(map, 16);
  }
  at = directory_slot(map, page_number);
  if (map->page_keys[at] == 0 && create) {
    if ((map->page_count + 1) * 20 >= map->directory_capacity * 17) {
      if (map->directory_capacity > SIZE_MAX / 2)
        fatal_error("compact_id_map: page directory overflow");
      resize_directory(map, map->directory_capacity * 2);
      at = directory_slot(map, page_number);
    }
    struct compact_id_page *page = new_page(page_number);
    map->page_keys[at] = page_number + 1;
    map->pages[at] = page;
    map->page_count++;
    update_peak(map);
  }
  return map->page_keys[at] == 0 ? NULL : map->pages[at];
}

static uint32_t *value_address(Compact_id_map map,
                               unsigned long long proof_id, BOOL create)
{
  uint64_t page_number = proof_id >> map->page_shift;
  size_t entry = (size_t) proof_id & (map->entries_per_page - 1);
  struct compact_id_page *page = find_page(map, page_number, create);
  return page == NULL ? NULL : page->values + entry * map->value_words;
}

Compact_id_map compact_id_map_init(unsigned value_words)
{
  Compact_id_map map;
  unsigned shift = CIDM_BASE_SHIFT;
  unsigned words = value_words;
  if (value_words == 0 || value_words > 8 ||
      (value_words & (value_words - 1)) != 0)
    fatal_error("compact_id_map: value width must be a power of two up to 8");
  while (words > 1) {
    shift--;
    words >>= 1;
  }
  map = safe_calloc(1, sizeof(*map));
  map->value_words = value_words;
  map->page_shift = shift;
  map->entries_per_page = (size_t) 1 << shift;
  update_peak(map);
  return map;
}

BOOL compact_id_map_get(Compact_id_map map, unsigned long long proof_id,
                        uint32_t *values)
{
  uint32_t *stored;
  if (map == NULL || proof_id == 0)
    return FALSE;
  stored = value_address(map, proof_id, FALSE);
  if (stored == NULL || stored[0] == 0)
    return FALSE;
  if (values != NULL)
    memcpy(values, stored, map->value_words * sizeof(*values));
  return TRUE;
}

BOOL compact_id_map_put(Compact_id_map map, unsigned long long proof_id,
                        const uint32_t *values)
{
  uint32_t *stored;
  BOOL inserted;
  if (map == NULL || proof_id == 0 || values == NULL || values[0] == 0)
    fatal_error("compact_id_map_put: invalid key or sentinel value");
  stored = value_address(map, proof_id, TRUE);
  inserted = stored[0] == 0;
  memcpy(stored, values, map->value_words * sizeof(*values));
  if (inserted)
    map->count++;
  return inserted;
}

BOOL compact_id_map_remove(Compact_id_map map,
                           unsigned long long proof_id)
{
  uint32_t *stored;
  if (map == NULL || proof_id == 0)
    return FALSE;
  stored = value_address(map, proof_id, FALSE);
  if (stored == NULL || stored[0] == 0)
    return FALSE;
  memset(stored, 0, map->value_words * sizeof(*stored));
  map->count--;
  return TRUE;
}

void compact_id_map_foreach(Compact_id_map map,
                            Compact_id_map_visit_fn visit, void *context)
{
  size_t slot;
  if (map == NULL || visit == NULL)
    return;
  for (slot = 0; slot < map->directory_capacity; slot++) {
    struct compact_id_page *page = map->pages[slot];
    size_t entry;
    if (page == NULL)
      continue;
    for (entry = 0; entry < map->entries_per_page; entry++) {
      const uint32_t *values =
        page->values + entry * map->value_words;
      if (values[0] != 0) {
        unsigned long long proof_id =
          (page->number << map->page_shift) | entry;
        visit(proof_id, values, context);
      }
    }
  }
}

size_t compact_id_map_count(Compact_id_map map)
{
  return map == NULL ? 0 : map->count;
}

unsigned long long compact_id_map_bytes(Compact_id_map map)
{
  return map_bytes(map);
}

unsigned long long compact_id_map_peak_bytes(Compact_id_map map)
{
  return map == NULL ? 0 : map->peak_bytes;
}

void compact_id_map_free(Compact_id_map map)
{
  size_t i;
  if (map == NULL)
    return;
  for (i = 0; i < map->directory_capacity; i++)
    free_page(map->pages[i]);
  safe_free(map->page_keys);
  safe_free(map->pages);
  safe_free(map);
}

static size_t set_directory_slot(Compact_id_set set, uint64_t page_number)
{
  uint64_t key = page_number + 1;
  size_t at = (size_t) hash_id(page_number) &
              (set->directory_capacity - 1);
  while (set->page_keys[at] != 0 && set->page_keys[at] != key)
    at = (at + 1) & (set->directory_capacity - 1);
  return at;
}

static void resize_set_directory(Compact_id_set set, size_t capacity)
{
  uint64_t *old_keys = set->page_keys;
  struct compact_id_set_page **old_pages = set->pages;
  size_t old_capacity = set->directory_capacity;
  size_t i;
  set->page_keys = safe_calloc(capacity, sizeof(*set->page_keys));
  set->pages = safe_calloc(capacity, sizeof(*set->pages));
  set->directory_capacity = capacity;
  for (i = 0; i < old_capacity; i++)
    if (old_keys[i] != 0) {
      struct compact_id_set_page *page = old_pages[i];
      size_t at = set_directory_slot(set, page->number);
      set->page_keys[at] = page->number + 1;
      set->pages[at] = page;
    }
  safe_free(old_keys);
  safe_free(old_pages);
}

static struct compact_id_set_page *new_set_page(uint64_t page_number)
{
  struct compact_id_set_page *page = safe_calloc(1, sizeof(*page));
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
  void *p = mmap(NULL, CIDS_PAGE_BYTES, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED)
    fatal_error("compact_id_set: cannot allocate bit page");
  page->bits = p;
  page->mapped = TRUE;
#else
  page->bits = safe_calloc(CIDS_PAGE_BYTES / sizeof(uint64_t),
                           sizeof(*page->bits));
#endif
  page->number = page_number;
  return page;
}

static void free_set_page(struct compact_id_set_page *page)
{
  if (page == NULL)
    return;
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
  if (page->mapped) {
    if (munmap(page->bits, CIDS_PAGE_BYTES) != 0)
      fatal_error("compact_id_set: cannot release bit page");
  }
  else
    safe_free(page->bits);
#else
  safe_free(page->bits);
#endif
  safe_free(page);
}

static struct compact_id_set_page *find_set_page(Compact_id_set set,
                                                  uint64_t page_number,
                                                  BOOL create)
{
  size_t at;
  if (set->directory_capacity == 0) {
    if (!create)
      return NULL;
    resize_set_directory(set, 16);
  }
  at = set_directory_slot(set, page_number);
  if (set->page_keys[at] == 0 && create) {
    if ((set->page_count + 1) * 20 >= set->directory_capacity * 17) {
      if (set->directory_capacity > SIZE_MAX / 2)
        fatal_error("compact_id_set: page directory overflow");
      resize_set_directory(set, set->directory_capacity * 2);
      at = set_directory_slot(set, page_number);
    }
    set->page_keys[at] = page_number + 1;
    set->pages[at] = new_set_page(page_number);
    set->page_count++;
  }
  return set->page_keys[at] == 0 ? NULL : set->pages[at];
}

Compact_id_set compact_id_set_init(void)
{
  return safe_calloc(1, sizeof(struct compact_id_set));
}

BOOL compact_id_set_add(Compact_id_set set,
                        unsigned long long proof_id)
{
  uint64_t page_number;
  size_t bit_number, word;
  uint64_t mask;
  struct compact_id_set_page *page;
  if (set == NULL || proof_id == 0)
    fatal_error("compact_id_set_add: invalid proof ID");
  page_number = proof_id >> CIDS_PAGE_SHIFT;
  bit_number = (size_t) proof_id &
    (((size_t) 1 << CIDS_PAGE_SHIFT) - 1);
  word = bit_number / CIDS_WORD_BITS;
  mask = UINT64_C(1) << (bit_number % CIDS_WORD_BITS);
  page = find_set_page(set, page_number, TRUE);
  if ((page->bits[word] & mask) != 0)
    return FALSE;
  page->bits[word] |= mask;
  set->count++;
  return TRUE;
}

BOOL compact_id_set_contains(Compact_id_set set,
                             unsigned long long proof_id)
{
  uint64_t page_number;
  size_t bit_number, word;
  uint64_t mask;
  struct compact_id_set_page *page;
  if (set == NULL || proof_id == 0)
    return FALSE;
  page_number = proof_id >> CIDS_PAGE_SHIFT;
  bit_number = (size_t) proof_id &
    (((size_t) 1 << CIDS_PAGE_SHIFT) - 1);
  word = bit_number / CIDS_WORD_BITS;
  mask = UINT64_C(1) << (bit_number % CIDS_WORD_BITS);
  page = find_set_page(set, page_number, FALSE);
  return page != NULL && (page->bits[word] & mask) != 0;
}

void compact_id_set_foreach(Compact_id_set set,
                            Compact_id_set_visit_fn visit, void *context)
{
  size_t slot;
  if (set == NULL || visit == NULL)
    return;
  for (slot = 0; slot < set->directory_capacity; slot++) {
    struct compact_id_set_page *page = set->pages[slot];
    size_t word;
    if (page == NULL)
      continue;
    for (word = 0; word < CIDS_PAGE_BYTES / sizeof(*page->bits); word++) {
      uint64_t bits = page->bits[word];
      unsigned bit;
      for (bit = 0; bits != 0; bit++, bits >>= 1)
        if ((bits & 1) != 0) {
          unsigned long long proof_id =
            (page->number << CIDS_PAGE_SHIFT) |
            (word * CIDS_WORD_BITS + bit);
          visit(proof_id, context);
        }
    }
  }
}

size_t compact_id_set_count(Compact_id_set set)
{
  return set == NULL ? 0 : set->count;
}

unsigned long long compact_id_set_bytes(Compact_id_set set)
{
  if (set == NULL)
    return 0;
  return sizeof(*set) +
    (unsigned long long) set->directory_capacity *
      (sizeof(*set->page_keys) + sizeof(*set->pages)) +
    (unsigned long long) set->page_count *
      (sizeof(struct compact_id_set_page) + CIDS_PAGE_BYTES);
}

unsigned long long compact_id_set_projected_map_bytes(Compact_id_set set,
                                                       unsigned value_words)
{
  size_t slot, page_count = 0, directory_capacity;
  unsigned shift = CIDM_BASE_SHIFT, words = value_words;
  unsigned subdivisions, subdivision;
  if (value_words == 0 || value_words > 8 ||
      (value_words & (value_words - 1)) != 0)
    fatal_error("compact_id_set: invalid projected map width");
  while (words > 1) {
    shift--;
    words >>= 1;
  }
  if (set == NULL || set->count == 0)
    return sizeof(struct compact_id_map);
  subdivisions = 1U << (CIDS_PAGE_SHIFT - shift);
  for (slot = 0; slot < set->directory_capacity; slot++) {
    struct compact_id_set_page *page = set->pages[slot];
    if (page == NULL)
      continue;
    for (subdivision = 0; subdivision < subdivisions; subdivision++) {
      size_t first_bit = subdivision *
        ((size_t) 1 << CIDS_PAGE_SHIFT) / subdivisions;
      size_t last_bit = first_bit +
        ((size_t) 1 << CIDS_PAGE_SHIFT) / subdivisions;
      size_t word;
      for (word = first_bit / CIDS_WORD_BITS;
           word < last_bit / CIDS_WORD_BITS; word++)
        if (page->bits[word] != 0) {
          page_count++;
          break;
        }
    }
  }
  directory_capacity = 16;
  while (page_count * 20 >= directory_capacity * 17) {
    if (directory_capacity > SIZE_MAX / 2)
      fatal_error("compact_id_set: projected directory overflow");
    directory_capacity *= 2;
  }
  return sizeof(struct compact_id_map) +
    (unsigned long long) directory_capacity *
      (sizeof(uint64_t) + sizeof(struct compact_id_page *)) +
    (unsigned long long) page_count *
      (sizeof(struct compact_id_page) + CIDM_PAGE_BYTES);
}

void compact_id_set_free(Compact_id_set set)
{
  size_t i;
  if (set == NULL)
    return;
  for (i = 0; i < set->directory_capacity; i++)
    free_set_page(set->pages[i]);
  safe_free(set->page_keys);
  safe_free(set->pages);
  safe_free(set);
}
