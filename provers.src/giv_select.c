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

#include "giv_select.h"
#include "semantics.h"
#include "../ladr/avltree.h"
#include "../ladr/clause_eval.h"
#include <stdint.h>

/* Private definitions and types */

enum { GS_ORDER_WEIGHT,
       GS_ORDER_AGE,
       GS_ORDER_HINT_AGE,
       GS_ORDER_RANDOM
};  /* order */

typedef struct giv_select *Giv_select;

struct giv_select {
  char         *name;
  int          order;
  Clause_eval  property;
  int          part;
  int          selected;
  Ordertype (*compare) (void *, void *);  /* function for ordering idx */
  Avl_node idx;          /* index of clauses (binary search (AVL) tree) */
  uint32_t *dense_heap;  /* record indexes; lazy deletion */
  size_t dense_size;
  size_t dense_capacity;
  size_t dense_active;
  unsigned dense_bit;
};  /* struct giv_select */

#define DENSE_PASSIVE_ACTIVE  0x01U
#define DENSE_PASSIVE_DELAYED 0x02U

struct dense_passive_record {
  unsigned long long id;
  unsigned long long hint_id;
  unsigned long long selector_mask;
  size_t store_position;
  double weight;
  unsigned simplifier_epoch;
  unsigned rewrite_epoch;
  int semantics;
  unsigned flags;
};

typedef struct select_state *Select_state;

/* Static variables */

static struct select_state {
  Plist selectors;    /* list of Giv_select */
  int occurrences;    /* occurrences of clauses in selectors */
  Plist current;      /* for ratio state */
  int  count;         /* for ratio state */
  int  cycle_size;
} High, Low; /* The two lists of selectors and their positions */

static BOOL Rule_needs_semantics = FALSE;
static int Sos_size = 0;
static double Low_water_keep = INT_MAX;
static double Low_water_displace = INT_MAX;
static int Sos_deleted = 0;
static int Sos_displaced = 0;

static BOOL Debug = FALSE;

static BOOL Dense_passive = FALSE;
static Dense_passive_archive_fn Dense_archive = NULL;
static Dense_passive_activate_fn Dense_activate = NULL;
static struct dense_passive_record *Dense_records = NULL;
static size_t Dense_record_count = 0;
static size_t Dense_record_capacity = 0;
static size_t Dense_active_count = 0;
static unsigned Dense_selector_count = 0;
static unsigned long long Dense_compactions = 0;
static unsigned long long Dense_records_reclaimed = 0;

static size_t dense_grow_capacity(size_t current, size_t element_size,
                                  char *where)
{
  size_t capacity = current == 0 ? 64 : current + current / 2;
  if (capacity <= current || capacity > SIZE_MAX / element_size)
    fatal_error(where);
  return capacity;
}

/*
 * memory management
 */

#define PTRS_GIV_SELECT CEILING(sizeof(struct giv_select), BYTES_POINTER)
static unsigned Giv_select_gets, Giv_select_frees;

/*************
 *
 *   Giv_select get_giv_select()
 *
 *************/

static
Giv_select get_giv_select(void)
{
  Giv_select p = get_cmem(PTRS_GIV_SELECT);
  Giv_select_gets++;
  return(p);
}  /* get_giv_select */

/*************
 *
 *    free_giv_select()
 *
 *************/

static
void free_giv_select(Giv_select p)
{
  free_mem(p, PTRS_GIV_SELECT);
  Giv_select_frees++;
}  /* free_giv_select */

/* PUBLIC */
void configure_dense_passive(BOOL enabled,
                             Dense_passive_archive_fn archive_fn,
                             Dense_passive_activate_fn activate_fn)
{
  Dense_passive = enabled;
  Dense_archive = archive_fn;
  Dense_activate = activate_fn;
  if (enabled && (archive_fn == NULL || activate_fn == NULL))
    fatal_error("configure_dense_passive: callbacks are required");
}  /* configure_dense_passive */

/* PUBLIC */
BOOL dense_passive_enabled(void)
{
  return Dense_passive;
}

/* PUBLIC */
int dense_passive_size(void)
{
  return Dense_active_count > INT_MAX ? INT_MAX : (int) Dense_active_count;
}

/* PUBLIC */
BOOL dense_passive_contains_id(unsigned long long id)
{
  size_t lo = 0, hi = Dense_record_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (Dense_records[mid].id < id)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo < Dense_record_count && Dense_records[lo].id == id &&
         (Dense_records[lo].flags & DENSE_PASSIVE_ACTIVE) != 0;
}  /* dense_passive_contains_id */

static size_t dense_find_record(unsigned long long id)
{
  size_t lo = 0, hi = Dense_record_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (Dense_records[mid].id < id)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo < Dense_record_count && Dense_records[lo].id == id ? lo :
         SIZE_MAX;
}

/* PUBLIC */
void dense_passive_foreach(Dense_passive_visit_fn visit, void *context)
{
  size_t i;
  if (visit == NULL)
    fatal_error("dense_passive_foreach: null visitor");
  for (i = 0; i < Dense_record_count; i++) {
    struct dense_passive_record *r = &Dense_records[i];
    if ((r->flags & DENSE_PASSIVE_ACTIVE) != 0) {
      struct dense_passive_view view;
      view.id = r->id;
      view.hint_id = r->hint_id;
      view.store_position = r->store_position;
      view.weight = r->weight;
      view.simplifier_epoch = r->simplifier_epoch;
      view.rewrite_epoch = r->rewrite_epoch;
      view.semantics = r->semantics;
      view.delayed_demodulator =
        (r->flags & DENSE_PASSIVE_DELAYED) != 0;
      visit(&view, context);
    }
  }
}  /* dense_passive_foreach */

/* PUBLIC */
unsigned dense_passive_scan_stale(size_t *cursor, unsigned rewrite_epoch,
                                  int filter, unsigned scan_limit,
                                  struct dense_passive_view *view)
{
  unsigned scanned = 0;
  size_t at;
  if (!Dense_passive || cursor == NULL || view == NULL ||
      Dense_record_count == 0 || scan_limit == 0)
    return 0;
  at = *cursor < Dense_record_count ? *cursor : 0;
  while (scanned < scan_limit && scanned < Dense_record_count) {
    struct dense_passive_record *r = &Dense_records[at];
    scanned++;
    at++;
    if (at == Dense_record_count)
      at = 0;
    if ((r->flags & DENSE_PASSIVE_ACTIVE) != 0 &&
        r->rewrite_epoch < rewrite_epoch &&
        (filter == DENSE_STALE_GENERAL ||
         (filter == DENSE_STALE_HINTED && r->hint_id != 0) ||
         (filter == DENSE_STALE_REWRITE &&
          (r->flags & DENSE_PASSIVE_DELAYED) != 0))) {
      view->id = r->id;
      view->hint_id = r->hint_id;
      view->store_position = r->store_position;
      view->weight = r->weight;
      view->simplifier_epoch = r->simplifier_epoch;
      view->rewrite_epoch = r->rewrite_epoch;
      view->semantics = r->semantics;
      view->delayed_demodulator =
        (r->flags & DENSE_PASSIVE_DELAYED) != 0;
      *cursor = at;
      return scanned;
    }
  }
  *cursor = at;
  return scanned;
}

/* PUBLIC */
unsigned long long dense_passive_stale_count(unsigned rewrite_epoch,
                                             unsigned long long *max_lag)
{
  unsigned long long count = 0, lag = 0;
  size_t i;
  for (i = 0; i < Dense_record_count; i++) {
    struct dense_passive_record *r = &Dense_records[i];
    if ((r->flags & DENSE_PASSIVE_ACTIVE) != 0 &&
        r->rewrite_epoch < rewrite_epoch) {
      unsigned long long current_lag = rewrite_epoch - r->rewrite_epoch;
      count++;
      if (current_lag > lag)
        lag = current_lag;
    }
  }
  if (max_lag != NULL)
    *max_lag = lag;
  return count;
}

/* PUBLIC */
void dense_passive_memory(unsigned long long *record_bytes,
                          unsigned long long *heap_bytes,
                          unsigned long long *records)
{
  unsigned long long heaps = 0;
  Plist p;
  for (p = High.selectors; p != NULL; p = p->next) {
    Giv_select gs = p->v;
    heaps += (unsigned long long) gs->dense_capacity * sizeof(uint32_t);
  }
  for (p = Low.selectors; p != NULL; p = p->next) {
    Giv_select gs = p->v;
    heaps += (unsigned long long) gs->dense_capacity * sizeof(uint32_t);
  }
  if (record_bytes != NULL)
    *record_bytes = (unsigned long long) Dense_record_capacity *
                    sizeof(struct dense_passive_record);
  if (heap_bytes != NULL)
    *heap_bytes = heaps;
  if (records != NULL)
    *records = Dense_active_count;
}  /* dense_passive_memory */

/* PUBLIC */
unsigned long long dense_passive_delayed_demodulators(void)
{
  unsigned long long count = 0;
  size_t i;
  for (i = 0; i < Dense_record_count; i++)
    if ((Dense_records[i].flags &
         (DENSE_PASSIVE_ACTIVE | DENSE_PASSIVE_DELAYED)) ==
        (DENSE_PASSIVE_ACTIVE | DENSE_PASSIVE_DELAYED))
      count++;
  return count;
}  /* dense_passive_delayed_demodulators */

/* PUBLIC */
BOOL dense_passive_compaction_needed(void)
{
  size_t inactive = Dense_record_count - Dense_active_count;
  return Dense_passive && Dense_record_count >= 1024 && inactive >= 256 &&
         inactive >= (Dense_active_count + 1) / 2;
}

/* PUBLIC */
void dense_passive_compaction_stats(unsigned long long *compactions,
                                    unsigned long long *records_reclaimed)
{
  if (compactions != NULL)
    *compactions = Dense_compactions;
  if (records_reclaimed != NULL)
    *records_reclaimed = Dense_records_reclaimed;
}

static int dense_compare(Giv_select gs, uint32_t ai, uint32_t bi)
{
  struct dense_passive_record *a = &Dense_records[ai];
  struct dense_passive_record *b = &Dense_records[bi];
  if (gs->order == GS_ORDER_WEIGHT) {
    if (a->weight < b->weight) return -1;
    if (a->weight > b->weight) return 1;
  }
  else if (gs->order == GS_ORDER_HINT_AGE) {
    if (a->hint_id != 0 && b->hint_id == 0) return -1;
    if (a->hint_id == 0 && b->hint_id != 0) return 1;
    if (a->hint_id < b->hint_id) return -1;
    if (a->hint_id > b->hint_id) return 1;
  }
  if (a->id < b->id) return -1;
  if (a->id > b->id) return 1;
  return 0;
}

static void dense_heap_push(Giv_select gs, uint32_t record)
{
  size_t i;
  if (gs->dense_size == gs->dense_capacity) {
    size_t capacity = dense_grow_capacity(gs->dense_capacity,
                                          sizeof(uint32_t),
                                          "dense_heap_push: capacity overflow");
    gs->dense_heap = safe_realloc(gs->dense_heap,
                                  capacity * sizeof(uint32_t));
    gs->dense_capacity = capacity;
  }
  i = gs->dense_size++;
  while (i > 0) {
    size_t parent = (i - 1) / 2;
    if (dense_compare(gs, gs->dense_heap[parent], record) <= 0)
      break;
    gs->dense_heap[i] = gs->dense_heap[parent];
    i = parent;
  }
  gs->dense_heap[i] = record;
  gs->dense_active++;
}

static void dense_heap_remove_root(Giv_select gs)
{
  uint32_t last;
  size_t i = 0;
  if (gs->dense_size == 0)
    return;
  last = gs->dense_heap[--gs->dense_size];
  while (i * 2 + 1 < gs->dense_size) {
    size_t child = i * 2 + 1;
    if (child + 1 < gs->dense_size &&
        dense_compare(gs, gs->dense_heap[child+1],
                      gs->dense_heap[child]) < 0)
      child++;
    if (dense_compare(gs, last, gs->dense_heap[child]) <= 0)
      break;
    gs->dense_heap[i] = gs->dense_heap[child];
    i = child;
  }
  if (gs->dense_size != 0)
    gs->dense_heap[i] = last;
}

static void dense_heap_prune(Giv_select gs)
{
  unsigned long long bit = 1ULL << gs->dense_bit;
  while (gs->dense_size != 0) {
    struct dense_passive_record *r =
      &Dense_records[gs->dense_heap[0]];
    if ((r->flags & DENSE_PASSIVE_ACTIVE) != 0 &&
        (r->selector_mask & bit) != 0)
      break;
    dense_heap_remove_root(gs);
  }
}

/* PUBLIC */
void dense_passive_compact(Dense_passive_relocate_fn relocate,
                           void *context)
{
  struct dense_passive_record *old_records = Dense_records;
  size_t old_count = Dense_record_count;
  size_t active = Dense_active_count;
  size_t capacity = 0;
  size_t i, n = 0;
  Plist p;

  if (!Dense_passive || relocate == NULL)
    fatal_error("dense_passive_compact: invalid state or callback");
  if (active != 0) {
    capacity = active + active / 8 + 16;
    if (capacity < active ||
        capacity > SIZE_MAX / sizeof(struct dense_passive_record))
      fatal_error("dense_passive_compact: capacity overflow");
    Dense_records = safe_malloc(capacity * sizeof(*Dense_records));
  }
  else
    Dense_records = NULL;

  for (i = 0; i < old_count; i++) {
    if ((old_records[i].flags & DENSE_PASSIVE_ACTIVE) != 0) {
      struct dense_passive_record r = old_records[i];
      r.store_position = relocate(r.store_position, context);
      if (r.store_position == SIZE_MAX)
        fatal_error("dense_passive_compact: passive relocation failed");
      Dense_records[n++] = r;
    }
  }
  if (n != active)
    fatal_error("dense_passive_compact: active-record count mismatch");
  safe_free(old_records);
  Dense_record_count = active;
  Dense_record_capacity = capacity;

  High.occurrences = 0;
  for (p = High.selectors; p != NULL; p = p->next) {
    Giv_select gs = p->v;
    safe_free(gs->dense_heap);
    gs->dense_heap = NULL;
    gs->dense_size = 0;
    gs->dense_capacity = 0;
    gs->dense_active = 0;
  }
  Low.occurrences = 0;
  for (p = Low.selectors; p != NULL; p = p->next) {
    Giv_select gs = p->v;
    safe_free(gs->dense_heap);
    gs->dense_heap = NULL;
    gs->dense_size = 0;
    gs->dense_capacity = 0;
    gs->dense_active = 0;
  }

  for (i = 0; i < active; i++) {
    struct dense_passive_record *r = &Dense_records[i];
    for (p = High.selectors; p != NULL; p = p->next) {
      Giv_select gs = p->v;
      if ((r->selector_mask & (1ULL << gs->dense_bit)) != 0) {
        dense_heap_push(gs, (uint32_t) i);
        High.occurrences++;
      }
    }
    for (p = Low.selectors; p != NULL; p = p->next) {
      Giv_select gs = p->v;
      if ((r->selector_mask & (1ULL << gs->dense_bit)) != 0) {
        dense_heap_push(gs, (uint32_t) i);
        Low.occurrences++;
      }
    }
  }
  Dense_compactions++;
  Dense_records_reclaimed += old_count - active;
}  /* dense_passive_compact */

static size_t selector_size(Giv_select gs)
{
  return Dense_passive ? gs->dense_active : (size_t) avl_size(gs->idx);
}

/*************
 *
 *   current_cycle_size()
 *
 *************/

static
int current_cycle_size(Select_state s)
{
  int sum = 0;
  Plist p;
  for (p = s->selectors; p; p = p->next) {
    Giv_select gs = p->v;
    if (selector_size(gs) > 0)
      sum += gs->part;
  }
  return sum;
}  /* current_cycle_size */

/*************
 *
 *   reset_selector_indexes()
 *
 *   Clear all selector AVL trees and counters.  Used by checkpoint
 *   restore to discard stale entries before reinserting from checkpoint.
 *
 *************/

/* PUBLIC */
void reset_selector_indexes(void)
{
  Plist p;
  for (p = High.selectors; p; p = p->next) {
    Giv_select gs = p->v;
    gs->idx = NULL;  /* leak old AVL nodes (small, one-time) */
    safe_free(gs->dense_heap);
    gs->dense_heap = NULL;
    gs->dense_size = 0;
    gs->dense_capacity = 0;
    gs->dense_active = 0;
    gs->selected = 0;
  }
  High.occurrences = 0;
  for (p = Low.selectors; p; p = p->next) {
    Giv_select gs = p->v;
    gs->idx = NULL;
    safe_free(gs->dense_heap);
    gs->dense_heap = NULL;
    gs->dense_size = 0;
    gs->dense_capacity = 0;
    gs->dense_active = 0;
    gs->selected = 0;
  }
  Low.occurrences = 0;
  safe_free(Dense_records);
  Dense_records = NULL;
  Dense_record_count = 0;
  Dense_record_capacity = 0;
  Dense_active_count = 0;
  Dense_compactions = 0;
  Dense_records_reclaimed = 0;
  Sos_size = 0;
}  /* reset_selector_indexes */

/*************
 *
 *   init_giv_select()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void init_giv_select(Plist rules)
{
  Plist p;

  for (p = rules; p; p = p->next) {
    Term t = p->v;
    int n = 0;
    Term order_term;
    Term property_term;
    Giv_select gs;
    if (!is_term(t, "=", 2) ||
	!is_term(ARG(t,0), "part", 4) ||
	!CONSTANT(ARG(ARG(t,0),0)) ||
	!(is_constant(ARG(ARG(t,0),1), "high") ||
	  is_constant(ARG(ARG(t,0),1), "low")) ||
	!((n = natural_constant_term(ARG(t,1))) > 0))
      fatal_error("Given selection rule must be: "
		  "part(<name>,high|low,age|wt|random,<property>)=<n>");

    order_term = ARG(ARG(t,0),2);
    property_term = ARG(ARG(t,0),3);
    gs = get_giv_select();
    if (Dense_selector_count >= 64)
      fatal_error("dense passive supports at most 64 given selectors");
    gs->dense_bit = Dense_selector_count++;
    
    if (is_constant(ARG(ARG(t,0),1), "high")) {
      High.selectors = plist_append(High.selectors, gs);
      if (n > INT_MAX - High.cycle_size)
	High.cycle_size = INT_MAX;  /* saturate to avoid overflow */
      else
	High.cycle_size += n;
    }
    else {
      Low.selectors  = plist_append(Low.selectors,  gs);
      if (n > INT_MAX - Low.cycle_size)
	Low.cycle_size = INT_MAX;  /* saturate to avoid overflow */
      else
	Low.cycle_size += n;
    }

    gs->name = term_symbol(ARG(ARG(t,0),0));
    gs->part = n;
    if (is_constant(order_term,"weight")) {
      gs->order = GS_ORDER_WEIGHT;
      gs->compare = (Ordertype (*) (void *, void *)) cl_wt_id_compare;
    }
    else if (is_constant(order_term,"age")) {
      gs->order = GS_ORDER_AGE;
      gs->compare = (Ordertype (*) (void *, void *)) cl_id_compare;
    }
    else if (is_constant(order_term,"hint_age")) {
      gs->order = GS_ORDER_HINT_AGE;
      gs->compare = (Ordertype (*) (void *, void *)) cl_hint_id_compare;
    }
    else if (is_constant(order_term,"random")) {
      if (Dense_passive)
        fatal_error("passive_store=dense does not yet support random selection");
      gs->order = GS_ORDER_RANDOM;
      gs->compare = (Ordertype (*) (void *, void *)) cl_id_compare;
    }
    else
      fatal_error("Given selection order must be weight, age, hint_age, or random.");
    gs->property = compile_clause_eval_rule(property_term);
    if (gs->property == NULL)
      fatal_error("Error in clause-property expression of given selection rule");
    else if (rule_contains_semantics(gs->property))
      Rule_needs_semantics = TRUE;
  }
  High.current = High.selectors;
  Low.current = Low.selectors;
}  /* init_giv_select */

/*************
 *
 *   update_selectors()
 *
 *************/

static
void update_selectors(Topform c, BOOL insert)
{
  BOOL matched = FALSE;
  Plist p;
  for (p = High.selectors; p; p = p->next) {
    Giv_select gs = p->v;
    if (eval_clause_in_rule(c, gs->property)) {
      matched = TRUE;
      if (insert) {
	gs->idx = avl_insert(gs->idx, c, gs->compare);
	High.occurrences++;
      }
      else {
	gs->idx = avl_delete(gs->idx, c, gs->compare);
	High.occurrences--;
      }
    }
  }
  /* If it is high-priority, don't let it also be low priority. */
  if (!matched) {
    for (p = Low.selectors; p; p = p->next) {
      Giv_select gs = p->v;
      if (eval_clause_in_rule(c, gs->property)) {
	matched = TRUE;
	if (insert) {
	  gs->idx = avl_insert(gs->idx, c, gs->compare);
	  Low.occurrences++;
	}
	else {
	  gs->idx = avl_delete(gs->idx, c, gs->compare);
	  Low.occurrences--;
	}
      }
    }
  }
  if (!matched) {
    static BOOL Already_warned = FALSE;

    if (!Already_warned) {
      fprintf(stderr, "\n\nWARNING: one or more kept clauses do not match "
	     "any given_selection rules (see output).\n\n");
      printf("\nWARNING: the following clause does not match "
	     "any given_selection rules.\n"
	     "This message will not be repeated.\n");
      f_clause(c);
      Already_warned = TRUE;
    }
  }
}  /* update_selectors */

static unsigned long long dense_selector_mask(Topform c)
{
  unsigned long long mask = 0;
  BOOL matched = FALSE;
  Plist p;
  for (p = High.selectors; p != NULL; p = p->next) {
    Giv_select gs = p->v;
    if (eval_clause_in_rule(c, gs->property)) {
      matched = TRUE;
      mask |= 1ULL << gs->dense_bit;
    }
  }
  if (!matched) {
    for (p = Low.selectors; p != NULL; p = p->next) {
      Giv_select gs = p->v;
      if (eval_clause_in_rule(c, gs->property)) {
        matched = TRUE;
        mask |= 1ULL << gs->dense_bit;
      }
    }
  }
  if (!matched) {
    static BOOL Already_warned_dense = FALSE;
    if (!Already_warned_dense) {
      fprintf(stderr, "\n\nWARNING: one or more kept clauses do not match "
              "any given_selection rules (see output).\n\n");
      printf("\nWARNING: the following clause does not match "
             "any given_selection rules.\n"
             "This message will not be repeated.\n");
      f_clause(c);
      Already_warned_dense = TRUE;
    }
  }
  return mask;
}

/* PUBLIC */
void given_selection_preview(Topform c,
			     unsigned long long *selector_mask,
			     unsigned *priority)
{
  unsigned long long mask = 0;
  BOOL matched = FALSE;
  Plist p;

  if (Rule_needs_semantics)
    set_semantics(c);
  for (p = High.selectors; p != NULL; p = p->next) {
    Giv_select gs = p->v;
    if (eval_clause_in_rule(c, gs->property)) {
      matched = TRUE;
      mask |= 1ULL << gs->dense_bit;
    }
  }
  if (matched) {
    if (priority != NULL)
      *priority = 0;
  }
  else {
    for (p = Low.selectors; p != NULL; p = p->next) {
      Giv_select gs = p->v;
      if (eval_clause_in_rule(c, gs->property)) {
        matched = TRUE;
        mask |= 1ULL << gs->dense_bit;
      }
    }
    if (priority != NULL)
      *priority = matched ? 1 : 2;
  }
  if (selector_mask != NULL)
    *selector_mask = mask;
}  /* given_selection_preview */

static void dense_insert_passive(Topform c)
{
  struct dense_passive_record r;
  uint32_t record;
  Plist p;
  if (c->id == 0)
    fatal_error("dense_insert_passive: clause has no ID");
  if (Dense_record_count >= UINT32_MAX)
    fatal_error("dense_insert_passive: record index overflow");
  if (Dense_record_count != 0 &&
      Dense_records[Dense_record_count-1].id >= c->id)
    fatal_error("dense_insert_passive: clause IDs are not increasing");
  if (Rule_needs_semantics)
    set_semantics(c);
  memset(&r, 0, sizeof(r));
  r.id = c->id;
  r.hint_id = c->matching_hint == NULL ? 0 : c->matching_hint->id;
  r.selector_mask = dense_selector_mask(c);
  r.weight = c->weight;
  r.simplifier_epoch = c->simplifier_epoch;
  r.rewrite_epoch = c->rewrite_epoch;
  r.semantics = c->semantics;
  r.flags = DENSE_PASSIVE_ACTIVE |
            (c->delayed_demodulator ? DENSE_PASSIVE_DELAYED : 0);
  r.store_position = Dense_archive(c);
  if (r.store_position == SIZE_MAX)
    fatal_error("dense_insert_passive: archive failed");
  if (Dense_record_count == Dense_record_capacity) {
    size_t capacity = dense_grow_capacity(
      Dense_record_capacity, sizeof(struct dense_passive_record),
      "dense_insert_passive: capacity overflow");
    Dense_records = safe_realloc(Dense_records,
                                  capacity * sizeof(*Dense_records));
    Dense_record_capacity = capacity;
  }
  record = (uint32_t) Dense_record_count;
  Dense_records[Dense_record_count++] = r;
  Dense_active_count++;
  for (p = High.selectors; p != NULL; p = p->next) {
    Giv_select gs = p->v;
    if ((r.selector_mask & (1ULL << gs->dense_bit)) != 0) {
      dense_heap_push(gs, record);
      High.occurrences++;
    }
  }
  for (p = Low.selectors; p != NULL; p = p->next) {
    Giv_select gs = p->v;
    if ((r.selector_mask & (1ULL << gs->dense_bit)) != 0) {
      dense_heap_push(gs, record);
      Low.occurrences++;
    }
  }
  Sos_size++;
}

static void dense_deactivate_record(uint32_t record)
{
  struct dense_passive_record *r = &Dense_records[record];
  Plist p;
  if ((r->flags & DENSE_PASSIVE_ACTIVE) == 0)
    fatal_error("dense_deactivate_record: inactive record");
  r->flags &= ~DENSE_PASSIVE_ACTIVE;
  Dense_active_count--;
  for (p = High.selectors; p != NULL; p = p->next) {
    Giv_select gs = p->v;
    if ((r->selector_mask & (1ULL << gs->dense_bit)) != 0) {
      gs->dense_active--;
      High.occurrences--;
    }
  }
  for (p = Low.selectors; p != NULL; p = p->next) {
    Giv_select gs = p->v;
    if ((r->selector_mask & (1ULL << gs->dense_bit)) != 0) {
      gs->dense_active--;
      Low.occurrences--;
    }
  }
  Sos_size--;
}

static void dense_reactivate_record(uint32_t record)
{
  struct dense_passive_record *r = &Dense_records[record];
  Plist p;
  if ((r->flags & DENSE_PASSIVE_ACTIVE) != 0)
    fatal_error("dense_reactivate_record: active record");
  r->flags |= DENSE_PASSIVE_ACTIVE;
  Dense_active_count++;
  for (p = High.selectors; p != NULL; p = p->next) {
    Giv_select gs = p->v;
    if ((r->selector_mask & (1ULL << gs->dense_bit)) != 0) {
      /* Deactivation is lazy: the original heap entry is still present and
         the unchanged selector key remains valid. */
      gs->dense_active++;
      High.occurrences++;
    }
  }
  for (p = Low.selectors; p != NULL; p = p->next) {
    Giv_select gs = p->v;
    if ((r->selector_mask & (1ULL << gs->dense_bit)) != 0) {
      gs->dense_active++;
      Low.occurrences++;
    }
  }
  Sos_size++;
}

/* PUBLIC */
BOOL dense_passive_deactivate_id(unsigned long long id,
                                 struct dense_passive_view *view)
{
  size_t at = dense_find_record(id);
  struct dense_passive_record *r;
  if (at == SIZE_MAX ||
      (Dense_records[at].flags & DENSE_PASSIVE_ACTIVE) == 0)
    return FALSE;
  r = &Dense_records[at];
  if (view != NULL) {
    view->id = r->id;
    view->hint_id = r->hint_id;
    view->store_position = r->store_position;
    view->weight = r->weight;
    view->simplifier_epoch = r->simplifier_epoch;
    view->rewrite_epoch = r->rewrite_epoch;
    view->semantics = r->semantics;
    view->delayed_demodulator =
      (r->flags & DENSE_PASSIVE_DELAYED) != 0;
  }
  dense_deactivate_record((uint32_t) at);
  return TRUE;
}

/* PUBLIC */
BOOL dense_passive_reactivate_id(unsigned long long id,
                                 unsigned simplifier_epoch,
                                 unsigned rewrite_epoch,
                                 BOOL delayed_demodulator)
{
  size_t at = dense_find_record(id);
  struct dense_passive_record *r;
  if (at == SIZE_MAX ||
      (Dense_records[at].flags & DENSE_PASSIVE_ACTIVE) != 0)
    return FALSE;
  r = &Dense_records[at];
  r->simplifier_epoch = simplifier_epoch;
  r->rewrite_epoch = rewrite_epoch;
  if (delayed_demodulator)
    r->flags |= DENSE_PASSIVE_DELAYED;
  else
    r->flags &= ~DENSE_PASSIVE_DELAYED;
  dense_reactivate_record((uint32_t) at);
  return TRUE;
}

/*************
 *
 *   insert_into_sos2()
 *
 *************/

/* DOCUMENTATION
This routine appends a clause to the sos list and updates
the (private) index for extracting sos clauses.
*/

/* PUBLIC */
void insert_into_sos2(Topform c, Clist sos)
{
  if (Dense_passive) {
    dense_insert_passive(c);
    return;
  }
  if (Rule_needs_semantics)
    set_semantics(c);  /* in case not yet evaluated */

  update_selectors(c, TRUE);
  clist_append(c, sos);
  Sos_size++;
}  /* insert_into_sos2 */

/*************
 *
 *   remove_from_sos2()
 *
 *************/

/* DOCUMENTATION
This routine removes a clause from the sos list and updates
the index for extracting the lightest and heaviest clauses.
*/

/* PUBLIC */
void remove_from_sos2(Topform c, Clist sos)
{
  if (Dense_passive)
    fatal_error("remove_from_sos2: dense passives are removed by record ID");
  /* A cold DISCOUNT passive keeps selector metadata in Topform but may not
     have a materialized literal body.  Selector membership rules are
     re-evaluated during removal, so restore the body before touching the AVL
     trees.  The caller is selecting or disabling the clause and needs the
     body in either case. */
  if (c->compressed != NULL && !materialize_clause(c))
    fatal_error("remove_from_sos2: invalid compressed passive clause");
  update_selectors(c, FALSE);
  clist_remove(c, sos);
  Sos_size--;
}  /* remove_from_sos2 */

/*************
 *
 *   bulk_insert_into_sos2()
 *
 *   Bulk-insert all clauses from a Clist into the SOS selector AVL trees.
 *   Uses sorted-array AVL construction: O(n log n) for qsort + O(n) for
 *   tree build, vs O(n log n) for n individual AVL inserts but with much
 *   better constant factor (no rotations, no per-insert rule evaluation
 *   overhead via batching).
 *
 *************/

/* qsort comparator wrapper - uses a static function pointer */
static Ordertype (*Bulk_compare)(void *, void *);

static int bulk_qsort_compare(const void *a, const void *b)
{
  Ordertype r = Bulk_compare(*(void **)a, *(void **)b);
  return (r == LESS_THAN) ? -1 : (r == GREATER_THAN) ? 1 : 0;
}

/* PUBLIC */
void bulk_insert_into_sos2(Clist sos)
{
  int n, i;
  void **all;
  BOOL *high_matched;
  Clist_pos cp;
  Plist p;

  n = sos->length;
  if (n == 0) return;

  if (Dense_passive) {
    while (sos->first != NULL) {
      Topform c = sos->first->c;
      clist_remove(c, sos);
      dense_insert_passive(c);
    }
    return;
  }

  /* Build array of all clauses, evaluating semantics if needed */
  all = (void **) safe_malloc(n * sizeof(void *));
  high_matched = (BOOL *) safe_malloc(n * sizeof(BOOL));
  i = 0;
  for (cp = sos->first; cp != NULL; cp = cp->next) {
    Topform c = cp->c;
    if (Rule_needs_semantics)
      set_semantics(c);
    all[i++] = c;
  }
  for (i = 0; i < n; i++)
    high_matched[i] = FALSE;

  /* For each selector, filter matching clauses, sort, build AVL */
  {
    void **matched = (void **) safe_malloc(n * sizeof(void *));
    Select_state states[2];
    int si;

    states[0] = &High;
    states[1] = &Low;

    for (si = 0; si < 2; si++) {
      for (p = states[si]->selectors; p; p = p->next) {
        Giv_select gs = p->v;
        int nm = 0;

        /* Normal insertion puts a clause in matching high selectors, or in
           matching low selectors if and only if no high selector matched. */
        for (i = 0; i < n; i++) {
          if (si == 0) {
            if (eval_clause_in_rule((Topform) all[i], gs->property)) {
              matched[nm++] = all[i];
              high_matched[i] = TRUE;
            }
          }
          else if (!high_matched[i] &&
                   eval_clause_in_rule((Topform) all[i], gs->property))
              matched[nm++] = all[i];
        }

        if (nm > 0) {
          /* Sort by selector's compare function */
          Bulk_compare = gs->compare;
          qsort(matched, nm, sizeof(void *), bulk_qsort_compare);

          /* Build balanced AVL tree from sorted array */
          gs->idx = avl_build_sorted(matched, nm);

          states[si]->occurrences += nm;
        }
      }
    }
    safe_free(matched);
  }

  Sos_size = n;
  safe_free(high_matched);
  safe_free(all);
}  /* bulk_insert_into_sos2 */

/*************
 *
 *   next_selector()
 *
 *************/

static
Giv_select next_selector(Select_state s)
{
  if (s->selectors == NULL)
    return NULL;
  else {
    Plist start = s->current;
    Giv_select gs = s->current->v;
    if (Dense_passive)
      dense_heap_prune(gs);
    while (selector_size(gs) == 0 || s->count >= gs->part) {
      s->current = s->current->next;
      if (!s->current)
	s->current = s->selectors;
      gs = s->current->v;
      if (Dense_passive)
        dense_heap_prune(gs);
      s->count = 0;
      if (s->current == start)
	break;  /* we're back to the start */
    }
    if (selector_size(gs) == 0)
      return NULL;
    else {
      s->count++;  /* for next call */
      return gs;
    }
  }
}  /* next_selector */

/*************
 *
 *   givens_available()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
BOOL givens_available(void)
{
  return (High.occurrences > 0 || Low.occurrences > 0);
}  /* givens_available */

/*************
 *
 *   get_given_clause2()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
Topform get_given_clause2(Clist sos, int num_given,
			 Prover_options opt, char **type)
{
  Topform giv;
  Giv_select gs = next_selector(&High);
  if (gs == NULL)
    gs = next_selector(&Low);
  if (gs == NULL)
    return NULL;  /* no clauses are available */

  if (Dense_passive) {
    uint32_t record = gs->dense_heap[0];
    struct dense_passive_record r = Dense_records[record];
    dense_deactivate_record(record);
    giv = Dense_activate(r.store_position, r.id, r.hint_id);
    if (giv == NULL || giv->id != r.id)
      fatal_error("get_given_clause2: dense archive identity mismatch");
    giv->weight = r.weight;
    giv->semantics = r.semantics;
    giv->simplifier_epoch = r.simplifier_epoch;
    giv->rewrite_epoch = r.rewrite_epoch;
    giv->delayed_demodulator =
      (r.flags & DENSE_PASSIVE_DELAYED) != 0;
    *type = gs->name;
    gs->selected += 1;
    return giv;
  }
    
  if (gs->order == GS_ORDER_RANDOM) {
    int n = avl_size(gs->idx);
    int i = (rand() % n) + 1;
    giv = avl_nth_item(gs->idx, i);
  }
  else
    giv = avl_smallest(gs->idx);

  *type = gs->name;
  gs->selected += 1;

  remove_from_sos2(giv, sos);
  return giv;
}  /* get_given_clause2 */

/*************
 *
 *   iterations_to_selection()
 *
 *************/

static
double iterations_to_selection(int part, int n,
			       int cycle_size, int occurrences, int sos_size)
{
  /* This approximates the number of iterations (of given selection) until
     the n-th clause in the selector is selected.  Simplyfying assumptions:
       1. High-priority selectors are empty.
       2. Other selectors don't become empty.
       3. No clauses are inserted before the n-th clause.  (unrealistic)
   */
  double x = n * ((double) cycle_size / part);
  return x / ((double) occurrences / sos_size);
}  /* iterations_to_selection */

/*************
 *
 *   least_iters_to_selection()
 *
 *************/

static
double least_iters_to_selection(Topform c, Select_state s, Plist ignore)
{
  Plist p;
  double least = INT_MAX;  /* where is DOUBLE_MAX?? */
  for (p = s->selectors; p; p = p->next) {
    if (p != ignore) {
      Giv_select gs = p->v;
      if (Rule_needs_semantics)
	set_semantics(c);  /* in case not yet evaluated */

      if (eval_clause_in_rule(c, gs->property)) {
	int n, cycle;
	double x;
	if (gs->order == GS_ORDER_AGE && c->id == INT_MAX)
	  n = avl_size(gs->idx) + 1;
	else
	  n = avl_place(gs->idx, c, gs->compare);
	cycle = current_cycle_size(s);
	x = iterations_to_selection(gs->part, n, cycle,
				    s->occurrences, Sos_size);
	if (Debug)
	  printf("%s(%.3f),cycle=%d,part=%d,place=%d,size=%d,iters=%.2f\n",
		 gs->name, c->weight, cycle, gs->part, n,avl_size(gs->idx),x);
	least = (x < least ? x : least);
      }
    }
  }
  return least;
}  /* least_iters_to_selection */

/***************
 *
 *   sos_keep2()
 *
 **************/

/* DOCUMENTATION
*/

/* PUBLIC */
BOOL sos_keep2(Topform c, Clist sos, Prover_options opt)
{
  int keep_factor = parm(opt->sos_keep_factor);
  int sos_size = clist_length(sos);
  int sos_limit = (parm(opt->sos_limit)== -1 ? INT_MAX : parm(opt->sos_limit));
  BOOL keep;
  if (sos_size < sos_limit / keep_factor)
    keep = TRUE;
  else {
    int iters;
    c->id = INT_MAX;
    iters = least_iters_to_selection(c, &Low, NULL);
    if (Debug)
      printf("iters=%d, wt=%.3f\n", iters, c->weight);
    if (iters < sos_limit / keep_factor)
      keep = TRUE;
    else {
      if (c->weight < Low_water_keep) {
	Low_water_keep = c->weight;
	if (!flag(opt->quiet)) {
	  printf("\nLow Water (keep): wt=%.3f, iters=%d\n", c->weight, iters);
	  if (stringparm(opt->stats, "all"))
	    selector_report();
	  fflush(stdout);
	}
      }
      Sos_deleted++;
      keep = FALSE;  /* delete clause */
    }
    c->id = 0;
  }
  return keep;
}  /* sos_keep2 */

/*************
 *
 *   worst_clause_of_priority_group()
 *
 *************/

static
Topform worst_clause_of_priority_group(Select_state ss)
{
  Topform worst = NULL; /* worst clause (with most iterations_to_selection)  */
  double max = 0.0;     /* iterations_to_selection for current worst clause  */
  Plist p;
  for (p = ss->selectors; p; p = p->next) {
    Giv_select gs = p->v;
    if (gs->idx) {
      Topform c = avl_largest(gs->idx);
      double x = iterations_to_selection(gs->part, avl_size(gs->idx),
					 current_cycle_size(ss),
					 ss->occurrences,
					 Sos_size);

      /* If that clause occurs in other selectors,
         find the lowest iterations_to_selection. */

      double y = least_iters_to_selection(c, ss, p);  /* ignore p */

      double least = (x < y ? x : y);

      if (least > max) {
	max = least;
	worst = c;
      }
    }
  }
  return worst;
}  /* worst_clause_of_priority_group */

/*************
 *
 *   worst_clause()
 *
 *************/

static
Topform worst_clause(void)
{
  Topform worst = worst_clause_of_priority_group(&Low);
  if (worst == NULL) {
    worst = worst_clause_of_priority_group(&High);
    if (worst)
      printf("\nWARNING: worst clause (id=%llu, wt=%.3f) has high priority.\n",
	     worst->id, worst->weight);
  }
  return worst;
}  /* worst_clause */

/*************
 *
 *   sos_displace2() - delete the worst sos clause
 *
 *************/

/* DOCUMENTATION
Disable the "worst" clause.
*/

/* PUBLIC */
void sos_displace2(void (*disable_proc) (Topform), BOOL quiet)
{
  Topform worst = worst_clause();
  if (worst == NULL) {
    selector_report();
    fatal_error("sos_displace2, cannot find worst clause");
  }
  else {
    if (worst->weight < Low_water_displace) {
      Low_water_displace = worst->weight;
      if (!quiet) {
	printf("\nLow Water (displace): id=%llu, wt=%.3f\n",
	       worst->id, worst->weight);
	fflush(stdout);
      }
    }
    Sos_displaced++;
    disable_proc(worst);
  }
}  /* sos_displace2 */

/*************
 *
 *   zap_given_selectors()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void zap_given_selectors(void)
{
  Plist p;
  for (p = High.selectors; p; p = p->next) {
    Giv_select gs = p->v;
    zap_clause_eval_rule(gs->property);
    avl_zap(gs->idx);
    safe_free(gs->dense_heap);
    free_giv_select(gs);
  }
  zap_plist(High.selectors);  /* shallow */
  for (p = Low.selectors; p; p = p->next) {
    Giv_select gs = p->v;
    zap_clause_eval_rule(gs->property);
    avl_zap(gs->idx);
    safe_free(gs->dense_heap);
    free_giv_select(gs);
  }
  zap_plist(Low.selectors);  /* shallow */
  safe_free(Dense_records);
  Dense_records = NULL;
  Dense_record_count = 0;
  Dense_record_capacity = 0;
  Dense_active_count = 0;
}  /* zap_given_selectors */

/*************
 *
 *   get_low_selector_state()
 *
 *************/

/* PUBLIC */
void get_low_selector_state(const char **name, int *count)
{
  if (Low.current) {
    Giv_select gs = Low.current->v;
    *name = gs->name;
    *count = Low.count;
  }
  else {
    *name = "";
    *count = 0;
  }
}  /* get_low_selector_state */

/*************
 *
 *   set_low_selector_state()
 *
 *************/

/* PUBLIC */
void set_low_selector_state(const char *name, int count)
{
  Plist p;
  for (p = Low.selectors; p; p = p->next) {
    Giv_select gs = p->v;
    if (strcmp(gs->name, name) == 0) {
      Low.current = p;
      Low.count = count;
      return;
    }
  }
  /* Name not found - leave at default (first selector, count=0). */
}  /* set_low_selector_state */

/*************
 *
 *   get_high_selector_state()
 *
 *************/

/* PUBLIC */
void get_high_selector_state(const char **name, int *count)
{
  if (High.current) {
    Giv_select gs = High.current->v;
    *name = gs->name;
    *count = High.count;
  }
  else {
    *name = "";
    *count = 0;
  }
}  /* get_high_selector_state */

/*************
 *
 *   set_high_selector_state()
 *
 *************/

/* PUBLIC */
void set_high_selector_state(const char *name, int count)
{
  Plist p;
  for (p = High.selectors; p; p = p->next) {
    Giv_select gs = p->v;
    if (strcmp(gs->name, name) == 0) {
      High.current = p;
      High.count = count;
      return;
    }
  }
}  /* set_high_selector_state */

/*************
 *
 *   selector_report()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void selector_report(void)
{
  Plist p;
  print_separator(stdout, "SELECTOR REPORT", TRUE);
  printf("Sos_deleted=%d, Sos_displaced=%d, Sos_size=%d\n",
	 Sos_deleted, Sos_displaced, Sos_size);
  printf("%10s %10s %10s %10s %10s %10s\n",
	 "SELECTOR", "PART", "PRIORITY", "ORDER", "SIZE", "SELECTED");
  for (p = High.selectors; p; p = p->next) {
    Giv_select gs = p->v;
    char *s1, *s2;
    s1 = "high";
    switch (gs->order) {
    case GS_ORDER_WEIGHT: s2 = "weight"; break;
    case GS_ORDER_AGE: s2 = "age"; break;
    case GS_ORDER_HINT_AGE: s2 = "hint_age"; break;
    case GS_ORDER_RANDOM: s2 = "random"; break;
    default: s2 = "???"; break;
    }
    printf("%10s %10d %10s %10s %10d %10d\n",
	   gs->name, gs->part, s1, s2, (int) selector_size(gs), gs->selected);
  }
  for (p = Low.selectors; p; p = p->next) {
    Giv_select gs = p->v;
    char *s1, *s2;
    s1 = "low";
    switch (gs->order) {
    case GS_ORDER_WEIGHT: s2 = "weight"; break;
    case GS_ORDER_AGE: s2 = "age"; break;
    case GS_ORDER_HINT_AGE: s2 = "hint_age"; break;
    case GS_ORDER_RANDOM: s2 = "random"; break;
    default: s2 = "???"; break;
    }
    printf("%10s %10d %10s %10s %10d %10d\n",
	   gs->name, gs->part, s1, s2, (int) selector_size(gs), gs->selected);
  }
  print_separator(stdout, "end of selector report", FALSE);  
  fflush(stdout);
}  /* selector_report */

/*************
 *
 *   selector_rule_term()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
Term selector_rule_term(char *name, char *priority,
			char *order, char *rule, int part)
{
  Term left =  get_rigid_term("part", 4);
  Term right = nat_to_term(part);
  ARG(left,0) = get_rigid_term(name, 0);
  ARG(left,1) = get_rigid_term(priority, 0);
  ARG(left,2) = get_rigid_term(order, 0);
  ARG(left,3) = get_rigid_term(rule, 0);
  return build_binary_term_safe("=", left, right);
}  /* selector_rule_term */

/*************
 *
 *   selector_rules_from_options()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
Plist selector_rules_from_options(Prover_options opt)
{
  Plist p = NULL;

  if (flag(opt->input_sos_first)) {
    p = plist_append(p, selector_rule_term("I", "high", "age",
					   "initial", INT_MAX));
  }

  if (parm(opt->hints_part) == INT_MAX) {
    p = plist_append(p, selector_rule_term("H", "high", "weight",
					   "hint", 1));
  }
  else if (parm(opt->hints_part) > 0) {
    p = plist_append(p, selector_rule_term("H", "low", "weight",
					   "hint", parm(opt->hints_part)));
  }

  if (parm(opt->age_part) > 0) {
    p = plist_append(p, selector_rule_term("A", "low", "age",
					   "all", parm(opt->age_part)));
  }
  if (parm(opt->false_part) > 0) {
    p = plist_append(p, selector_rule_term("F", "low", "weight",
					   "false", parm(opt->false_part)));
  }
  if (parm(opt->true_part) > 0) {
    p = plist_append(p, selector_rule_term("T", "low", "weight",
					   "true", parm(opt->true_part)));
  }
  if (parm(opt->weight_part) > 0) {
    p = plist_append(p, selector_rule_term("W", "low", "weight",
					   "all", parm(opt->weight_part)));
  }
  if (parm(opt->random_part) > 0) {
    p = plist_append(p, selector_rule_term("R", "low", "random",
					   "all", parm(opt->random_part)));
  }

  return p;
}  /* selector_rules_from_options */
