/*  Copyright (C) 2006, 2007 William McCune

    This file is part of the LADR Deduction Library.

    The LADR Deduction Library is free software; you can redistribute it
    and/or modify it under the terms of the GNU General Public License,
    version 2.

    The LADR Deduction Library is distributed in the hope that it will be
    useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with the LADR Deduction Library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "clauseid.h"
#include <limits.h>
#include <stdint.h>

/* Private definitions and types */

#define CLAUSE_ID_PAGE_BITS 6
#define CLAUSE_ID_PAGE_SIZE (1U << CLAUSE_ID_PAGE_BITS)
#define LEGACY_CLAUSE_ID_TAB_SIZE 50000
#define PAGE_TOMBSTONE ((Clause_id_page) (uintptr_t) 1)

typedef struct clause_id_page * Clause_id_page;

struct clause_id_page {
  unsigned long long page_number;
  unsigned count;
  uintptr_t entries[CLAUSE_ID_PAGE_SIZE];
};

static Clause_id_page *Topform_id_pages = NULL;
static size_t Topform_id_page_capacity = 0;
static size_t Topform_id_page_count = 0;
static size_t Topform_id_page_tombstones = 0;
static unsigned long long Topform_id_entries = 0;
static unsigned long long Topform_id_count = 0;  /* 64-bit to prevent overflow */

#define ARCHIVE_TAG ((uintptr_t) 1)

static Clause_id_page find_id_page(unsigned long long page_number);

static
BOOL archived_entry(uintptr_t entry)
{
  return (entry & ARCHIVE_TAG) != 0;
}

static
uintptr_t archive_entry(unsigned long long offset)
{
  if (offset > (unsigned long long) (UINTPTR_MAX >> 1))
    fatal_error("archive_entry: ancestor-store offset overflow");
  return ((uintptr_t) offset << 1) | ARCHIVE_TAG;
}

static
unsigned long long entry_archive_offset(uintptr_t entry)
{
  return (unsigned long long) (entry >> 1);
}

static
uintptr_t find_id_entry(unsigned long long id)
{
  Clause_id_page page;
  if (id == 0)
    return 0;
  page = find_id_page(id >> CLAUSE_ID_PAGE_BITS);
  return page == NULL ? 0 :
    page->entries[(unsigned) (id & (CLAUSE_ID_PAGE_SIZE - 1))];
}

static
size_t page_hash(unsigned long long n)
{
  n ^= n >> 30;
  n *= 0xbf58476d1ce4e5b9ULL;
  n ^= n >> 27;
  n *= 0x94d049bb133111ebULL;
  n ^= n >> 31;
  return (size_t) n;
}

static
size_t page_slot(unsigned long long page_number, BOOL inserting)
{
  size_t i, first_tombstone = (size_t) -1;
  if (Topform_id_page_capacity == 0)
    return (size_t) -1;
  i = page_hash(page_number) & (Topform_id_page_capacity - 1);
  while (Topform_id_pages[i] != NULL) {
    if (Topform_id_pages[i] == PAGE_TOMBSTONE) {
      if (first_tombstone == (size_t) -1)
        first_tombstone = i;
    }
    else if (Topform_id_pages[i]->page_number == page_number)
      return i;
    i = (i + 1) & (Topform_id_page_capacity - 1);
  }
  return inserting && first_tombstone != (size_t) -1 ? first_tombstone : i;
}

static
void resize_page_table(size_t new_capacity)
{
  Clause_id_page *old_pages = Topform_id_pages;
  size_t old_capacity = Topform_id_page_capacity;
  size_t i;

  Topform_id_pages = safe_calloc(new_capacity, sizeof(Clause_id_page));
  Topform_id_page_capacity = new_capacity;
  Topform_id_page_tombstones = 0;
  for (i = 0; i < old_capacity; i++) {
    Clause_id_page page = old_pages[i];
    if (page != NULL && page != PAGE_TOMBSTONE)
      Topform_id_pages[page_slot(page->page_number, TRUE)] = page;
  }
  safe_free(old_pages);
}

static
Clause_id_page find_id_page(unsigned long long page_number)
{
  size_t i;
  if (Topform_id_page_capacity == 0)
    return NULL;
  i = page_slot(page_number, FALSE);
  return Topform_id_pages[i] == NULL ? NULL : Topform_id_pages[i];
}

static
Clause_id_page get_id_page(unsigned long long page_number)
{
  size_t i;
  Clause_id_page page = find_id_page(page_number);
  if (page != NULL)
    return page;

  if (Topform_id_page_capacity == 0)
    resize_page_table(16);
  else if ((Topform_id_page_count + Topform_id_page_tombstones + 1) * 4 >=
           Topform_id_page_capacity * 3) {
    if ((Topform_id_page_count + 1) * 4 < Topform_id_page_capacity)
      resize_page_table(Topform_id_page_capacity);
    else {
      if (Topform_id_page_capacity > ((size_t) -1) / 2)
        fatal_error("get_id_page: capacity overflow");
      resize_page_table(Topform_id_page_capacity * 2);
    }
  }

  i = page_slot(page_number, TRUE);
  if (Topform_id_pages[i] == PAGE_TOMBSTONE)
    Topform_id_page_tombstones--;
  page = safe_calloc(1, sizeof(struct clause_id_page));
  page->page_number = page_number;
  Topform_id_pages[i] = page;
  Topform_id_page_count++;
  return page;
}

static
void register_id(Topform c)
{
  unsigned long long page_number = c->id >> CLAUSE_ID_PAGE_BITS;
  unsigned offset = (unsigned) (c->id & (CLAUSE_ID_PAGE_SIZE - 1));
  Clause_id_page page;
  uintptr_t old;

  if (c->id == 0)
    fatal_error("register_id: clause has no ID");
  old = find_id_entry(c->id);
  if (old != 0 && old != (uintptr_t) c)
    fatal_error("register_id: duplicate clause ID");
  if (old == (uintptr_t) c) {
    c->official_id = 1;
    return;
  }
  page = get_id_page(page_number);
  page->entries[offset] = (uintptr_t) c;
  page->count++;
  Topform_id_entries++;
  c->official_id = 1;
}

/*************
 *
 *   next_clause_id()
 *
 *************/

static
unsigned long long next_clause_id(void)
{
  Topform_id_count++;
  /* With 64-bit IDs, overflow is practically impossible */
  return Topform_id_count;
}  /* next_clause_id */

/*************
 *
 *   clause_ids_assigned()
 *
 *************/

/* DOCUMENTATION
What is the most recently assigned clause ID?
*/

/* PUBLIC */
unsigned long long clause_ids_assigned(void)
{
  return Topform_id_count;
}  /* clause_ids_assigned */

/*************
 *
 *   assign_clause_id(c)
 *
 *************/

/* DOCUMENTATION
This routine assigns a unique identifier to the id field of a clause.
It also inserts the clause into a hash table so that given an id
number, the corresponding clause can be retrieved quickly (see
find_clause_by_id()).
*/

/* PUBLIC */
void assign_clause_id(Topform c)
{
  if (c->id > 0) {
    p_clause(c);
    fatal_error("assign_clause_id, clause already has ID.");
  }
  c->id = next_clause_id();
  register_id(c);
}  /* assign_clause_id */

/*************
 *
 *     unassign_clause_id(c)
 *
 *************/

/* DOCUMENTATION
This routine removes a clause from the ID hash table and resets
the ID of the clause to 0.  A fatal error occurs if the clause
has not been assigned an ID.
*/

/* PUBLIC */
void unassign_clause_id(Topform c)
{
  if (c->official_id) {
    unsigned long long page_number = c->id >> CLAUSE_ID_PAGE_BITS;
    unsigned offset = (unsigned) (c->id & (CLAUSE_ID_PAGE_SIZE - 1));
    size_t i = page_slot(page_number, FALSE);
    Clause_id_page page = i == (size_t) -1 ? NULL : Topform_id_pages[i];

    if (page == NULL || page == PAGE_TOMBSTONE ||
        page->entries[offset] != (uintptr_t) c) {
      p_clause(c);
      fatal_error("unassign_clause_id, cannot find clause.");
    }
    page->entries[offset] = 0;
    page->count--;
    Topform_id_entries--;
    if (page->count == 0) {
      safe_free(page);
      Topform_id_pages[i] = PAGE_TOMBSTONE;
      Topform_id_page_count--;
      Topform_id_page_tombstones++;
      if (Topform_id_page_count == 0) {
        safe_free(Topform_id_pages);
        Topform_id_pages = NULL;
        Topform_id_page_capacity = 0;
        Topform_id_page_tombstones = 0;
      }
      else if (Topform_id_page_tombstones > Topform_id_page_count)
        resize_page_table(Topform_id_page_capacity);
    }
    c->id = 0;
    c->official_id = 0;
  }
}  /* unassign_clause_id */

/* PUBLIC */
BOOL detach_clause_id(Topform c)
{
  unsigned long long page_number;
  unsigned offset;
  size_t i;
  Clause_id_page page;
  if (c == NULL || !c->official_id || c->id == 0)
    return FALSE;
  page_number = c->id >> CLAUSE_ID_PAGE_BITS;
  offset = (unsigned) (c->id & (CLAUSE_ID_PAGE_SIZE - 1));
  i = page_slot(page_number, FALSE);
  page = i == (size_t) -1 ? NULL : Topform_id_pages[i];
  if (page == NULL || page == PAGE_TOMBSTONE ||
      page->entries[offset] != (uintptr_t) c)
    return FALSE;
  page->entries[offset] = 0;
  page->count--;
  Topform_id_entries--;
  c->official_id = 0;
  if (page->count == 0) {
    safe_free(page);
    Topform_id_pages[i] = PAGE_TOMBSTONE;
    Topform_id_page_count--;
    Topform_id_page_tombstones++;
    if (Topform_id_page_count == 0) {
      safe_free(Topform_id_pages);
      Topform_id_pages = NULL;
      Topform_id_page_capacity = 0;
      Topform_id_page_tombstones = 0;
    }
    else if (Topform_id_page_tombstones > Topform_id_page_count)
      resize_page_table(Topform_id_page_capacity);
  }
  return TRUE;
}  /* detach_clause_id */

/*************
 *
 *     find_clause_by_id(id)
 *
 *     Given a clause ID, retrieve the clause (or NULL).
 *
 *************/

/* DOCUMENTATION
This routine retrieves the clause with the given ID number
(or NULL, if there is no such clause).
*/

/* PUBLIC */
Topform find_clause_by_id(unsigned long long id)
{
  uintptr_t entry = find_id_entry(id);
  return entry == 0 || archived_entry(entry) ? NULL : (Topform) entry;
}  /* find_clause_by_id */

/* PUBLIC */
BOOL archive_clause_id(Topform c, unsigned long long offset)
{
  Clause_id_page page;
  unsigned slot;
  if (c == NULL || !c->official_id || c->id == 0)
    return FALSE;
  page = find_id_page(c->id >> CLAUSE_ID_PAGE_BITS);
  slot = (unsigned) (c->id & (CLAUSE_ID_PAGE_SIZE - 1));
  if (page == NULL || page->entries[slot] != (uintptr_t) c)
    return FALSE;
  page->entries[slot] = archive_entry(offset);
  c->official_id = 0;
  return TRUE;
}  /* archive_clause_id */

/* PUBLIC */
BOOL activate_archived_clause_id(Topform c,
                                 unsigned long long expected_offset)
{
  Clause_id_page page;
  unsigned slot;
  if (c == NULL || c->id == 0)
    return FALSE;
  page = find_id_page(c->id >> CLAUSE_ID_PAGE_BITS);
  slot = (unsigned) (c->id & (CLAUSE_ID_PAGE_SIZE - 1));
  if (page == NULL ||
      page->entries[slot] != archive_entry(expected_offset))
    return FALSE;
  page->entries[slot] = (uintptr_t) c;
  c->official_id = 1;
  return TRUE;
}  /* activate_archived_clause_id */

/* PUBLIC */
BOOL clause_id_is_archived(unsigned long long id)
{
  uintptr_t entry = find_id_entry(id);
  return entry != 0 && archived_entry(entry);
}  /* clause_id_is_archived */

/* PUBLIC */
BOOL clause_id_archive_offset(unsigned long long id,
                              unsigned long long *offset)
{
  uintptr_t entry = find_id_entry(id);
  if (entry == 0 || !archived_entry(entry) || offset == NULL)
    return FALSE;
  *offset = entry_archive_offset(entry);
  return TRUE;
}  /* clause_id_archive_offset */

/* PUBLIC */
void unassign_archived_clause_id(unsigned long long id,
                                 unsigned long long offset)
{
  unsigned long long page_number;
  unsigned slot;
  size_t i;
  Clause_id_page page;
  if (id == 0)
    return;
  page_number = id >> CLAUSE_ID_PAGE_BITS;
  slot = (unsigned) (id & (CLAUSE_ID_PAGE_SIZE - 1));
  i = page_slot(page_number, FALSE);
  page = i == (size_t) -1 ? NULL : Topform_id_pages[i];
  if (page == NULL || page == PAGE_TOMBSTONE ||
      page->entries[slot] != archive_entry(offset))
    fatal_error("unassign_archived_clause_id: cannot find archive entry");
  page->entries[slot] = 0;
  page->count--;
  Topform_id_entries--;
  if (page->count == 0) {
    safe_free(page);
    Topform_id_pages[i] = PAGE_TOMBSTONE;
    Topform_id_page_count--;
    Topform_id_page_tombstones++;
    if (Topform_id_page_count == 0) {
      safe_free(Topform_id_pages);
      Topform_id_pages = NULL;
      Topform_id_page_capacity = 0;
      Topform_id_page_tombstones = 0;
    }
    else if (Topform_id_page_tombstones > Topform_id_page_count)
      resize_page_table(Topform_id_page_capacity);
  }
}  /* unassign_archived_clause_id */

/*************
 *
 *     fprint_clause_id_tab(fp)
 *
 *************/

/* DOCUMENTATION
This routine prints (to FILE *fp) all the clauses in the ID hash table.
*/

/* PUBLIC */
void fprint_clause_id_tab(FILE *fp)
{
  size_t i;

  fprintf(fp, "\nID clause table:\n");
  for (i = 0; i < Topform_id_page_capacity; i++) {
    Clause_id_page page = Topform_id_pages[i];
    if (page != NULL && page != PAGE_TOMBSTONE) {
      unsigned j;
      for (j = 0; j < CLAUSE_ID_PAGE_SIZE; j++)
        if (page->entries[j] != 0 && !archived_entry(page->entries[j]))
          fprint_clause(fp, (Topform) page->entries[j]);
    }
  }
  fflush(fp);
}  /* fprint_clause_id_tab */

/*************
 *
 *     p_clause_id_tab(tab)
 *
 *************/

/* DOCUMENTATION
This routine prints (to stdout) all the clauses in the ID hash table.
*/

/* PUBLIC */
void p_clause_id_tab()
{
  fprint_clause_id_tab(stdout);
}  /* p_clause_id_tab */

/*************
 *
 *   insert_clause_into_plist()
 *
 *************/

/* DOCUMENTATION
This routine inserts a clause into a sorted (by ID) Plist of clauses.
Boolean paramemeter "increasing" tells whether the list is increasing
or decreasing.
The updated Plist is returned.
If the clause is already there, nothing happens.
*/

/* PUBLIC */
Plist insert_clause_into_plist(Plist p, Topform c, BOOL increasing)
{
  Plist prev, curr, new;
  prev = NULL;
  curr = p;
  while (curr != NULL && (increasing ? ((Topform) curr->v)->id < c->id
	                             : ((Topform) curr->v)->id > c->id)) {
    prev = curr;
    curr = curr->next;
  }
  if (curr == NULL || ((Topform) curr->v)->id != c->id) {
    new = get_plist();
    new->v = c;
    new->next = curr;
    if (prev != NULL)
      prev->next = new;
    else
      p = new;
  }
  return p;
}  /* insert_clause_into_plist */

/*************
 *
 *   clause_plist_member()
 *
 *************/

/* DOCUMENTATION
This routine checks if a clause occurs in a sorted (by ID) Plist of clauses.
Boolean paramemeter "increasing" tells whether the list is increasing
or decreasing.
*/

/* PUBLIC */
BOOL clause_plist_member(Plist p, Topform c, BOOL increasing)
{
  Plist curr = p;
  while (curr != NULL && (increasing ? ((Topform) curr->v)->id < c->id
	                             : ((Topform) curr->v)->id > c->id)) {
    curr = curr->next;
  }
  return (curr != NULL && ((Topform) curr->v)->id == c->id);
}  /* clause_plist_member */

/*************
 *
 *   set_clause_id_count()
 *
 *************/

/* DOCUMENTATION
Set the clause ID counter to a specific value.  Used when resuming
from a checkpoint so that new clauses get IDs that don't collide
with saved ones.
*/

/* PUBLIC */
void set_clause_id_count(unsigned long long n)
{
  Topform_id_count = n;
}  /* set_clause_id_count */

/*************
 *
 *   clear_clause_id_tab()
 *
 *************/

/* DOCUMENTATION
Clear all entries from the clause ID hash table.  Used before reloading
clauses from a checkpoint (in-process save+reload test) to prevent stale
entries from shadowing newly-loaded clauses.
Clause objects are NOT freed - they are leaked.
*/

/* PUBLIC */
void clear_clause_id_tab(void)
{
  size_t i;
  for (i = 0; i < Topform_id_page_capacity; i++) {
    Clause_id_page page = Topform_id_pages[i];
    if (page != NULL && page != PAGE_TOMBSTONE)
      safe_free(page);
  }
  safe_free(Topform_id_pages);
  Topform_id_pages = NULL;
  Topform_id_page_capacity = 0;
  Topform_id_page_count = 0;
  Topform_id_page_tombstones = 0;
  Topform_id_entries = 0;
}  /* clear_clause_id_tab */

/*************
 *
 *   register_clause_with_id()
 *
 *************/

/* DOCUMENTATION
Register a clause that already has an ID into the ID hash table.
The clause's id field must already be set.  This does NOT auto-increment
the ID counter.  Used when resuming from a checkpoint.
*/

/* PUBLIC */
void register_clause_with_id(Topform c)
{
  if (c->id == 0)
    fatal_error("register_clause_with_id, clause has no ID.");
  register_id(c);
}  /* register_clause_with_id */

/*************
 *
 *   collect_formulas_from_id_tab()
 *
 *************/

/* DOCUMENTATION
Return a Plist of all Topform entries in the clause ID hash table
that have is_formula set (i.e., non-clause formulas such as goals).
The caller should zap_plist() the result when done.
*/

/* PUBLIC */
Plist collect_formulas_from_id_tab(void)
{
  size_t i;
  Plist result = NULL;
  for (i = 0; i < Topform_id_page_capacity; i++) {
    Clause_id_page page = Topform_id_pages[i];
    if (page != NULL && page != PAGE_TOMBSTONE) {
      unsigned j;
      for (j = 0; j < CLAUSE_ID_PAGE_SIZE; j++) {
        uintptr_t entry = page->entries[j];
        Topform c = entry == 0 || archived_entry(entry) ? NULL :
                    (Topform) entry;
        if (c != NULL && c->is_formula)
          result = insert_clause_into_plist(result, c, TRUE);
      }
    }
  }
  return result;
}  /* collect_formulas_from_id_tab */

/* PUBLIC */
struct clause_id_table_stats clause_id_table_get_stats(void)
{
  struct clause_id_table_stats stats;
  stats.entries = Topform_id_entries;
  stats.pages = Topform_id_page_count;
  stats.table_capacity = Topform_id_page_capacity;
  stats.allocated_bytes =
    (unsigned long long) Topform_id_page_capacity * sizeof(Clause_id_page) +
    (unsigned long long) Topform_id_page_count * sizeof(struct clause_id_page);
  stats.legacy_bytes =
    (unsigned long long) LEGACY_CLAUSE_ID_TAB_SIZE * sizeof(Plist) +
    Topform_id_entries * sizeof(struct plist);
  return stats;
}  /* clause_id_table_get_stats */
