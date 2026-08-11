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

#include "search.h"
#include "provers.h"
#include "cold_passive_store.h"
#include "rewrite_only_store.h"
#include "compact_rewrite.h"
#include "../ladr/ac_redun.h"
#include "../ladr/std_options.h"
#include "../ladr/memory.h"
#include "../ladr/string.h"
#include "../ladr/sine.h"
#include "../ladr/clash.h"
#include "../ladr/tptp_parse.h"
#include "../ladr/xproofs.h"
#include "../VERSION_DATE.h"

// system includes

#include <sys/types.h>
#include <sys/stat.h>
#ifndef __EMSCRIPTEN__
#include <sys/wait.h>
#endif
#include <unistd.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include <float.h>
#include <math.h>
#include <setjmp.h>  /* Yikes! */
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <stdint.h>

// Private definitions and types

static jmp_buf Jump_env;                 // for setjmp/longjmp

static Prover_options Opt;               // Prover9 options
static struct prover_attributes Att;     // Prover9 accepted attributes
static struct prover_stats Stats;        // Prover9 statistics
static struct prover_clocks Clocks;      // Prover9 clocks
static Cold_passive_store Dense_body_store = NULL;
static unsigned long long Dense_arena_bytes_reclaimed = 0;
static Rewrite_only_store Rewrite_only_rules = NULL;
static Compact_rewrite_bank Compact_rewrite_rules = NULL;
static Compact_term_pool Compact_terms = NULL;
static unsigned long long Compact_term_next_reclaim_serialization = 0;
static unsigned long long Compact_term_reclaim_cooldown_skips = 0;
static unsigned long long Compact_term_reclaim_deferrals = 0;
static unsigned long long Compact_term_last_predicted_reclaim = 0;
static FILE *Deferred_terminal_stats = NULL;
static BOOL Terminal_stats_frozen = FALSE;
static BOOL Terminal_compact_indexes_released = FALSE;
static BOOL Terminal_hint_index_released = FALSE;
static unsigned Rewrite_epoch = 1;    /* compact rewrite state seen by SOS */
static size_t Rewrite_refresh_hot_cursor = 0;
static size_t Rewrite_refresh_general_cursor = 0;
static size_t Rewrite_interreduce_cursor = 0;
static unsigned Rewrite_refresh_hot_streak = 0;
static unsigned Rewrite_interreduce_streak = 0;
static unsigned Rewrite_refresh_inference_streak = 0;
static BOOL Rewrite_drain_mode = FALSE;
static unsigned Rewrite_drain_streak = 0;
static unsigned Rewrite_repair_depth = 0;
static BOOL Resume_rewrite_cursor_ids = FALSE;
static unsigned long long Resume_rewrite_hot_cursor_id = 0;
static unsigned long long Resume_rewrite_general_cursor_id = 0;
static unsigned long long Resume_rewrite_interreduce_cursor_id = 0;

#define COMPACT_PASSIVE_CACHE_WAYS 4
#define COMPACT_PASSIVE_CACHE_MIN_CHARGE 512
#define COMPACT_PASSIVE_CACHE_MAX_SLOTS 8192

struct compact_passive_cache_entry {
  unsigned long long id;
  size_t position;
  size_t charge;
  unsigned long long stamp;
  Topform clause;
  unsigned pins;
};

static struct compact_passive_cache_entry *Compact_passive_cache;
static size_t Compact_passive_cache_sets;
static size_t Compact_passive_cache_slots;
static size_t Compact_passive_cache_budget;
static size_t Compact_passive_cache_bytes;
static size_t Compact_passive_cache_peak_bytes;
static size_t Compact_passive_cache_entries;
static size_t Compact_passive_cache_peak_entries;
static size_t Compact_passive_cache_eviction_cursor;
static unsigned long long Compact_passive_cache_clock;
static unsigned long long Compact_passive_cache_hits;
static unsigned long long Compact_passive_cache_misses;
static unsigned long long Compact_passive_cache_bypasses;
static unsigned long long Compact_passive_cache_evictions;
static unsigned long long Compact_passive_cache_invalidations;

static void update_rewrite_only_stats(void);
static void current_demodulate_clause(Topform, int, int, BOOL, BOOL);
static Topform compact_otter_resolve_clause(unsigned long long, void *);
static void compact_otter_release_clause(Topform, void *);
static void compact_otter_advise_rebuild_batch(
  const unsigned long long *, size_t, void *);

/* Progress callback for shared-memory IPC (set by -cores scheduler) */
static Search_progress_fn Progress_callback = NULL;

#ifdef __EMSCRIPTEN__
static double Wasm_deadline_ms = 0;
#endif

/* "% " prefix for diagnostic output in TPTP mode (makes it a TPTP comment) */
#define TPTP_PFX  (Opt && flag(Opt->tptp_output) ? "% " : "")

static BOOL discount_mode(void)
{
  return Opt != NULL && str_ident(stringparm1(Opt->search_loop), "discount");
}

static BOOL compressed_passive_mode(void)
{
  return discount_mode() &&
         (str_ident(stringparm1(Opt->passive_store), "compressed") ||
          str_ident(stringparm1(Opt->passive_store), "dense"));
}

/* A configured compact back-demod index has no logical consumer when backward
   demodulation itself is disabled.  Treat it as an inactive capability rather
   than allocating and populating a dead index or rejecting a general compact
   configuration on non-demodulation workloads. */
static BOOL compact_back_demod_authoritative_mode(void)
{
  return Opt != NULL && flag(Opt->back_demod) &&
         flag(Opt->compact_otter_back_demod_index);
}

static BOOL dense_passive_mode(void)
{
  return Opt != NULL &&
         str_ident(stringparm1(Opt->passive_store), "dense") &&
         (discount_mode() ||
          (!discount_mode() &&
           flag(Opt->compact_otter_demodulation) &&
           flag(Opt->compact_otter_unit_index) &&
           (!flag(Opt->back_demod) ||
            flag(Opt->compact_otter_back_demod_index)) &&
           flag(Opt->compact_otter_nonunit_index)));
}

static BOOL compact_otter_passive_mode(void)
{
  return dense_passive_mode() && !discount_mode();
}

static BOOL eager_legacy_demod_mode(void)
{
  return discount_mode() && Opt != NULL &&
    str_ident(stringparm1(Opt->discount_demodulation), "eager_legacy");
}

static BOOL eager_interreduced_demod_mode(void)
{
  return discount_mode() && Opt != NULL &&
    str_ident(stringparm1(Opt->discount_demodulation),
              "eager_interreduced");
}

/* Stage-2 Phase-5 oracle: keep the legacy OTTER demodulation index
   authoritative while maintaining and querying the compact bank in lockstep.
   A mismatch stops immediately instead of perturbing the reference run. */
static BOOL compact_otter_audit_mode(void)
{
  return Opt != NULL && flag(Opt->compact_otter_audit);
}

/* Stage-2 Phase-5 implementation mode: preserve the ordinary eager OTTER
   rule lifecycle and ordering, but replace its pointer-heavy discrimination
   tree with the already-audited compact rewrite bank. */
static BOOL compact_otter_demod_mode(void)
{
  return Opt != NULL && flag(Opt->compact_otter_demodulation);
}

static BOOL compact_otter_bank_mode(void)
{
  return compact_otter_audit_mode() || compact_otter_demod_mode();
}

static BOOL maximum_discount_demod_mode(void)
{
  return eager_legacy_demod_mode() || eager_interreduced_demod_mode();
}

static void advance_rewrite_epoch(void)
{
  unsigned long long stale = dense_passive_size();
  if (Rewrite_epoch != UINT_MAX)
    Rewrite_epoch++;
  dense_passive_set_rewrite_epoch(Rewrite_epoch);
  if (stale > Stats.rewrite_refresh_stale_peak)
    Stats.rewrite_refresh_stale_peak = stale;
}

static BOOL demodulation_rules_available(void);

static BOOL collective_frontier_mode(void)
{
  return discount_mode() && Opt != NULL &&
         str_ident(stringparm1(Opt->inference_frontier), "collective");
}

static BOOL collective_balanced_mode(void)
{
  return collective_frontier_mode() &&
         str_ident(stringparm1(Opt->collective_scheduler), "balanced_hint");
}

static BOOL collective_hyper_enabled(void)
{
  return collective_frontier_mode() &&
         (flag(Opt->pos_hyper_resolution) ||
           flag(Opt->neg_hyper_resolution));
}

static BOOL packed_hint_bank_mode(void)
{
  return Opt != NULL &&
    (str_ident(stringparm1(Opt->hint_index), "packed") ||
     str_ident(stringparm1(Opt->hint_index), "packed_fast") ||
     str_ident(stringparm1(Opt->hint_index), "hybrid") ||
     str_ident(stringparm1(Opt->hint_index), "packed_legacy"));
}

static BOOL better_packed_hint_mode(void)
{
  return Opt != NULL &&
    (str_ident(stringparm1(Opt->hint_index), "packed") ||
     str_ident(stringparm1(Opt->hint_index), "packed_fast") ||
     str_ident(stringparm1(Opt->hint_index), "hybrid"));
}

static BOOL fast_packed_hint_mode(void)
{
  return Opt != NULL &&
    str_ident(stringparm1(Opt->hint_index), "packed_fast");
}

static BOOL live_clash_index_needed(void)
{
  return flag(Opt->binary_resolution) ||
         flag(Opt->neg_binary_resolution) ||
         flag(Opt->pos_ur_resolution) ||
         flag(Opt->neg_ur_resolution) ||
         (!collective_frontier_mode() &&
          (flag(Opt->pos_hyper_resolution) ||
           flag(Opt->neg_hyper_resolution)));
}

static int configured_hint_fpa_depth(void)
{
  if (packed_hint_bank_mode())
    return 0;
  else if (str_ident(stringparm1(Opt->hint_index), "compact"))
    return 2;
  else if (str_ident(stringparm1(Opt->hint_index), "shallow"))
    return 1;
  else
    return parm(Opt->hints_fpa_depth);
}

/* Saved selector state from checkpoint (used during resume) */
static char Resume_low_selector_name[32] = "";
static int  Resume_low_selector_count = 0;
static char Resume_high_selector_name[32] = "";
static int  Resume_high_selector_count = 0;

/* Saved hint_match data for restoring matching_hint pointers */
static struct clause_meta *Resume_meta = NULL;
static int Resume_meta_count = 0;
static unsigned long long Resume_hint_epoch = 1;

/* In-loop checkpoint loading state */
static BOOL Load_checkpoint = FALSE;  /* TRUE on first loop iteration during resume */
static const char *Resume_dir = NULL; /* checkpoint directory for in-loop loading */

/* Saved cac_clauses IDs from checkpoint (restored after clause loading) */
static unsigned long long *Resume_cac_ids = NULL;
static int Resume_cac_count = 0;

/* Saved desc_to_be_disabled IDs from checkpoint */
static unsigned long long *Resume_dtbd_ids = NULL;
static int Resume_dtbd_count = 0;

/* Hoisted from make_inferences() for checkpoint save/restore */
static int Bf_level = 0;             /* breadth-first level counter */
static int Bf_last_of_level = 0;     /* last clause ID of current level */
static int Nohints_count = 0;        /* consecutive givens without hint match */
static unsigned Simplifier_epoch = 1; /* active state visible to DISCOUNT SOS */
/* Format 3 deliberately omits disabled pre-elimination scratch clauses with
   ID 0.  They can never be proof ancestors, but they still contributed to
   the uninterrupted Disabled statistic.  Preserve only that count across a
   resume so statistics remain deterministic without retaining dead bodies. */
static unsigned long long Disabled_checkpoint_omitted = 0;

enum inference_source {
  INFER_SOURCE_OTHER,
  INFER_SOURCE_BINARY,
  INFER_SOURCE_HYPER,
  INFER_SOURCE_UR,
  INFER_SOURCE_PARAMOD
};
static enum inference_source Current_inference_source = INFER_SOURCE_OTHER;

/* A Waldmeister-style collective frontier delays the Cartesian product of
   one newly active clause with the active clauses visible at that moment.
   One descriptor is constant size.  The append-only activation history and
   a sparse deactivation-epoch side table identify the exact historical
   partner set.  Active clause bodies are shared with the persistent index;
   disabled versions transfer to history ownership, so history grows with
   givens rather than with generated/passive clauses. */
#define COLLECTIVE_ACT_PAGE_BITS 10
#define COLLECTIVE_ACT_PAGE_SIZE (1U << COLLECTIVE_ACT_PAGE_BITS)

struct collective_activation_page {
  unsigned long long ids[COLLECTIVE_ACT_PAGE_SIZE];
  Topform clauses[COLLECTIVE_ACT_PAGE_SIZE];
  unsigned char clashable[COLLECTIVE_ACT_PAGE_SIZE];
};

struct collective_deactivation_entry {
  unsigned long long id;
  unsigned epoch;
  unsigned reserved;
};

enum collective_batch_kind {
  COLLECTIVE_PARAMOD   = 1U,
  COLLECTIVE_POS_HYPER = 2U,
  COLLECTIVE_NEG_HYPER = 4U,
  COLLECTIVE_HINT_PROBE = 8U,
  /* This descriptor's current inference unit is being enumerated in
     deterministic raw-weight order rather than raw generator order. */
  COLLECTIVE_PROMISING_CURSOR = 16U,
  /* Balanced mode splits the two historical paramodulation directions.
     The legacy bit remains unchanged for old queues and checkpoints. */
  COLLECTIVE_PARAMOD_FROM = 32U,
  COLLECTIVE_PARAMOD_INTO = 64U
};

struct collective_consumed_ordinal {
  unsigned long long ordinal;
  unsigned long long raw_fingerprint;
};

#define COLLECTIVE_INFERENCE_MASK \
  (COLLECTIVE_PARAMOD | COLLECTIVE_POS_HYPER | COLLECTIVE_NEG_HYPER | \
   COLLECTIVE_PARAMOD_FROM | COLLECTIVE_PARAMOD_INTO)
#define COLLECTIVE_KIND_MASK \
  (COLLECTIVE_INFERENCE_MASK | COLLECTIVE_HINT_PROBE | \
   COLLECTIVE_PROMISING_CURSOR)

struct collective_batch {
  unsigned long long given_id;
  unsigned long long cursor;
  unsigned long long activation_limit;
  unsigned long long conclusion_cursor;
  unsigned long long conclusion_prefix_hash;
  double conclusion_order_weight;
  unsigned long long conclusion_order_ordinal;
  double conclusion_next_weight;
  unsigned long long conclusion_next_ordinal;
  unsigned snapshot_epoch;
  unsigned kind;
  unsigned long long candidate_ordinal;
  Para_iterator para_iterator;
  Hyper_iterator hyper_iterator;
  unsigned long long discovery_cursor;
  unsigned long long discovery_ordinal;
  Para_iterator discovery_para_iterator;
  Hyper_iterator discovery_hyper_iterator;
  struct collective_consumed_ordinal *consumed;
  unsigned consumed_count;
  unsigned consumed_capacity;
  BOOL discovery_initialized;
  BOOL discovery_complete;
  BOOL discovery_hot;
  size_t priority_heap_index;
  struct collective_batch *prev;
  struct collective_batch *next;
};

#define COLLECTIVE_NO_HEAP_INDEX ((size_t) -1)

static struct collective_activation_page **Collective_activation_pages = NULL;
static size_t Collective_activation_page_capacity = 0;
static size_t Collective_activation_page_count = 0;
static unsigned long long Collective_activation_count = 0;
static Lindex Collective_historical_idx = NULL;
static struct collective_deactivation_entry *Collective_deactivations = NULL;
static size_t Collective_deactivation_capacity = 0;
static size_t Collective_deactivation_count = 0;
static struct collective_batch *Collective_batch_head = NULL;
static struct collective_batch *Collective_batch_tail = NULL;
static unsigned long long Collective_batch_count = 0;
static struct collective_batch **Collective_priority_heap = NULL;
static size_t Collective_priority_heap_count = 0;
static size_t Collective_priority_heap_capacity = 0;
static unsigned Collective_priority_turns_since_fair = 0;
static BOOL Collective_batch_turn = FALSE;
static unsigned Collective_givens_since_batch = 0;
static BOOL Collective_hint_probe_credit = TRUE;
enum collective_balanced_lane {
  COLLECTIVE_LANE_PARAMOD = 0,
  COLLECTIVE_LANE_POS_HYPER = 1,
  COLLECTIVE_LANE_NEG_HYPER = 2,
  COLLECTIVE_LANE_COUNT = 3
};
static unsigned Collective_balanced_lane = COLLECTIVE_LANE_PARAMOD;
static unsigned Collective_balanced_lane_credit = 0;
static unsigned Collective_balanced_turns_since_oldest = 0;
static BOOL Collective_drain_mode = FALSE;

/* Phase-4 candidate windows.  This is one global pool shared by every rule
   lane; descriptors retain no raw Topforms.  Heap keys are advisory preview
   results, while ownership of every clause ends only in the authoritative
   cl_process() path. */
struct collective_pool_entry {
  Topform clause;
  enum inference_source source;
  unsigned kind;
  unsigned selector_priority;
  unsigned long long selector_mask;
  unsigned long long given_id;
  unsigned long long raw_ordinal;
  unsigned long long insertion_ordinal;
  unsigned long long hint_id;
  unsigned long long fingerprint;
  unsigned long long hint_epoch;
  unsigned simplifier_epoch;
  BOOL discovery_promotion;
  double adjusted_weight;
  unsigned long long bytes;
};

static struct collective_pool_entry **Collective_candidate_heap = NULL;
static size_t Collective_candidate_heap_count = 0;
static size_t Collective_candidate_heap_capacity = 0;
static unsigned long long Collective_candidate_pool_bytes = 0;
static unsigned long long Collective_candidate_insertion_ordinal = 0;
static unsigned Collective_pool_expansions_since_commit = 0;
static unsigned Collective_pool_commits_since_fair = 0;
static struct collective_batch *Current_collective_pool_batch = NULL;
static struct collective_pool_entry *Current_collective_commit_entry = NULL;
static BOOL Current_collective_discovery = FALSE;
static unsigned Collective_discovery_turns_since_fair = 0;
static unsigned Collective_discovery_turns_since_general = 0;

static void collective_candidate_pool_clear(void);
static unsigned long long collective_candidate_pool_allocated_bytes(void);
static unsigned long long hint_trace_clause_hash(Topform c);

/* Periodic automatic checkpoint state */
static time_t Last_auto_ckpt_time = 0;       // wall-clock of last auto checkpoint
static char **Auto_ckpt_dirs = NULL;          // circular buffer of dir names
static int    Auto_ckpt_capacity = 0;
static int    Auto_ckpt_head = 0;             // index of oldest entry
static int    Auto_ckpt_count = 0;            // number of entries in buffer

/* Clause tracing (cl_to_trace parameter) */
static unsigned long long To_trace_id = 0;
static Topform To_trace_cl = NULL;

/* Hitlist for print_derivations (Veroff feature) */
#define MAX_HSIZE 5000
static int HIT_LIST[MAX_HSIZE];
static int Hsize = 0;

/* Callback for forward_subsumption_filter: accept a candidate subsumer
   if anc_subsume allows it; otherwise increment the blocked counter and
   reject so the iteration continues to the next candidate.  Matches
   Otter's forward_subsume behavior (clause.c:1380-1466) of skipping past
   anc_subsume-blocked candidates rather than bailing on the first hit. */
static
BOOL anc_subsume_accept_cb(Topform subsumer, Topform new_clause, void *arg)
{
  BOOL use_prf_weight = arg ? *(BOOL *)arg : FALSE;
  if (anc_subsume(subsumer, new_clause, use_prf_weight))
    return TRUE;
  Stats.anc_subsume_blocked++;
  return FALSE;
}

// The following is a global structure for this file.

static struct {

  // basic clause lists

  Clist sos;
  Clist usable;
  Clist demods;
  Clist hints;

  // other lists

  Plist actions;
  Plist weights;
  Plist resonators;
  Plist kbo_weights;
  Plist interps;
  Plist given_selection;
  Plist keep_rules;
  Plist delete_rules;

  // auxiliary clause lists

  Clist limbo;
  Clause_store disabled;
  Plist empties;

  // indexing

  Lindex clashable_idx;  // literal index for resolution rules
  BOOL use_clash_idx;    // GET RID OF THIS VARIABLE!!

  // basic properties of usable+sos

  BOOL horn, unit, equality;
  unsigned number_of_clauses, number_of_neg_clauses;

  // other stuff

  Ilist desc_to_be_disabled;   // Stable IDs whose descendants are disabled
  Ilist cac_clauses;           // Stable IDs that trigger back CAC check

  BOOL searching;      // set to TRUE when first given is selected
  BOOL initialized;    // has this structure been initialized?
  BOOL has_goals;      // goals/conjecture was present in input
  BOOL has_neg_conj;   // CNF negated_conjecture was present (refutation problem)
  char *problem_name;  // TPTP problem name for SZS lines (e.g., "PUZ001-2")
  double start_time;   // when was it initialized? 
  int start_ticks;     // quasi-clock that times the same for all machines

  int return_code;     // result of search
} Glob;

static BOOL demodulation_rules_available(void)
{
  struct compact_rewrite_stats compact;
  compact_rewrite_get_stats(Compact_rewrite_rules, &compact);
  return (Glob.demods != NULL && !clist_empty(Glob.demods)) ||
    rewrite_only_store_count(Rewrite_only_rules) != 0 ||
    compact.rules_current != 0;
}

/* Keep one demodulation callback at every consumer boundary.  In compact
   mode the proof-producing search path and the hint-rewrite path use the
   pointer-free bank; the selected/eager-legacy policies retain LADR's
   established discrimination-tree implementation. */
static void current_demodulate_clause(Topform c, int step_limit,
                                      int increase_limit, BOOL print,
                                      BOOL lex_order_vars)
{
  if (eager_interreduced_demod_mode() || compact_otter_demod_mode()) {
    (void) print;
    compact_rewrite_clause(Compact_rewrite_rules, c, step_limit,
                           increase_limit, lex_order_vars, TRUE);
  }
  else if (compact_otter_audit_mode()) {
    Topform compact = copy_clause_ija(c);
    char *legacy_just = NULL, *compact_just = NULL;
    unsigned legacy_size = 0, compact_size = 0;
    BOOL same;

    compact_rewrite_clause(Compact_rewrite_rules, compact, step_limit,
                           increase_limit, lex_order_vars, TRUE);
    demodulate_clause(c, step_limit, increase_limit, print, lex_order_vars);
    same = clause_ident(c->literals, compact->literals) &&
      encode_justification(c->justification, &legacy_just, &legacy_size) &&
      encode_justification(compact->justification,
                           &compact_just, &compact_size) &&
      legacy_size == compact_size &&
      memcmp(legacy_just, compact_just, legacy_size) == 0;
    Stats.compact_otter_audit_queries++;
    if (!same) {
      Stats.compact_otter_audit_failures++;
      fprintf(stderr,
              "compact_otter_audit: demodulation mismatch before clause ID %llu\n",
              c->id);
      fprintf(stderr, "legacy: ");
      fwrite_clause(stderr, c, CL_FORM_STD);
      fprintf(stderr, "compact: ");
      fwrite_clause(stderr, compact, CL_FORM_STD);
    }
    safe_free(legacy_just);
    safe_free(compact_just);
    delete_clause(compact);
    if (!same)
      fatal_error("compact OTTER demodulation audit failed");
  }
  else
    demodulate_clause(c, step_limit, increase_limit, print, lex_order_vars);
}

/* The ordinary selector is the promising-candidate cache for collective
   search.  Count both committed passives and the current turn's limbo so a
   descriptor cannot overfill the configured bound before limbo drains. */
static
unsigned long long collective_candidate_occupancy(void)
{
  unsigned long long passives;
  unsigned long long limbo = Glob.limbo == NULL ? 0 : Glob.limbo->length;
  if (!collective_frontier_mode())
    return 0;
  /* Dense passives have already surrendered their transient Topform bodies
     to the compact store.  In balanced mode the cache is the global bound
     on exposed, not-yet-compacted candidate bodies; counting all dense SOS
     records here would couple descriptor draining to passive cardinality and
     can deadlock at the descriptor and passive limits simultaneously. */
  if (collective_balanced_mode() && dense_passive_mode())
    return (unsigned long long) Collective_candidate_heap_count + limbo;
  passives = dense_passive_mode() ?
    (unsigned long long) dense_passive_size() :
    (Glob.sos == NULL ? 0 : (unsigned long long) Glob.sos->length);
  return passives + limbo;
}

static
unsigned long long collective_candidate_budget(void)
{
  unsigned long long occupied = collective_candidate_occupancy();
  unsigned long long cache_limit =
    (unsigned long long) parm(Opt->collective_candidate_cache);
  unsigned long long chunk_limit =
    (unsigned long long) parm(Opt->collective_candidate_chunk);
  unsigned long long available = occupied >= cache_limit ?
    0 : cache_limit - occupied;
  return available < chunk_limit ? available : chunk_limit;
}

static
void collective_note_candidate_cache_peak(void)
{
  unsigned long long occupied;
  if (!collective_frontier_mode())
    return;
  occupied = collective_candidate_occupancy();
  if (occupied > Stats.collective_candidate_cache_peak)
    Stats.collective_candidate_cache_peak = occupied;
}

static
void collective_clear_state(void)
{
  size_t i;
  struct collective_batch *b;

  collective_candidate_pool_clear();

  if (Collective_historical_idx != NULL) {
    unsigned long long position;
    for (position = 0; position < Collective_activation_count; position++) {
      size_t page_number =
        (size_t) (position >> COLLECTIVE_ACT_PAGE_BITS);
      unsigned offset =
        (unsigned) (position & (COLLECTIVE_ACT_PAGE_SIZE - 1));
      Topform c = Collective_activation_pages[page_number]->clauses[offset];
      if (c != NULL) {
        if (Collective_activation_pages[page_number]->clashable[offset])
          lindex_update(Collective_historical_idx, c, DELETE);
        /* Active clauses are shared with Usable and remain owned by the
           ordinary search state.  Archived/deactivated clauses are detached
           from the ID table and owned solely by collective history. */
        if (!c->official_id)
          delete_clause(c);
        else
          c->collective_history = 0;
      }
    }
    lindex_destroy(Collective_historical_idx);
    Collective_historical_idx = NULL;
  }

  for (i = 0; i < Collective_activation_page_count; i++)
    safe_free(Collective_activation_pages[i]);
  safe_free(Collective_activation_pages);
  Collective_activation_pages = NULL;
  Collective_activation_page_capacity = 0;
  Collective_activation_page_count = 0;
  Collective_activation_count = 0;

  safe_free(Collective_deactivations);
  Collective_deactivations = NULL;
  Collective_deactivation_capacity = 0;
  Collective_deactivation_count = 0;

  while (Collective_batch_head != NULL) {
    b = Collective_batch_head;
    Collective_batch_head = b->next;
    para_iterator_zap(&b->para_iterator);
    hyper_iterator_zap(&b->hyper_iterator);
    para_iterator_zap(&b->discovery_para_iterator);
    hyper_iterator_zap(&b->discovery_hyper_iterator);
    safe_free(b->consumed);
    safe_free(b);
  }
  Collective_batch_tail = NULL;
  Collective_batch_count = 0;
  safe_free(Collective_priority_heap);
  Collective_priority_heap = NULL;
  Collective_priority_heap_count = 0;
  Collective_priority_heap_capacity = 0;
  Collective_priority_turns_since_fair = 0;
  Collective_batch_turn = FALSE;
  Collective_givens_since_batch = 0;
  Collective_hint_probe_credit = TRUE;
  Collective_balanced_lane = COLLECTIVE_LANE_PARAMOD;
  Collective_balanced_lane_credit = 0;
  Collective_balanced_turns_since_oldest = 0;
  Collective_drain_mode = FALSE;
  Collective_candidate_insertion_ordinal = 0;
  Collective_pool_expansions_since_commit = 0;
  Collective_pool_commits_since_fair = 0;
  Current_collective_pool_batch = NULL;
  Current_collective_commit_entry = NULL;
  Current_collective_discovery = FALSE;
  Collective_discovery_turns_since_fair = 0;
  Collective_discovery_turns_since_general = 0;
}

static
void collective_reset_state(void)
{
  collective_clear_state();
  Stats.collective_batches_created = 0;
  Stats.collective_batches_completed = 0;
  Stats.collective_pair_expansions = 0;
  Stats.collective_pair_turns = 0;
  Stats.collective_hyper_expansions = 0;
  Stats.collective_hyper_sets_completed = 0;
  Stats.collective_candidates_emitted = 0;
  Stats.collective_candidates_replayed = 0;
  Stats.collective_deferred_turns = 0;
  Stats.collective_raw_candidates_peak = 0;
  Stats.collective_raw_candidates_seen = 0;
  Stats.collective_candidate_cache_peak = 0;
  Stats.collective_candidate_cache_stalls = 0;
  Stats.collective_candidate_cache_current = 0;
  Stats.collective_promising_scans = 0;
  Stats.collective_promising_considered = 0;
  Stats.collective_promising_buffer_peak = 0;
  Stats.collective_promising_priority_turns = 0;
  Stats.collective_promising_fair_turns = 0;
  Stats.collective_promising_heap_peak = 0;
  Stats.collective_partners_skipped = 0;
  Stats.collective_parent_materializations = 0;
  Stats.collective_snapshot_rebuilds = 0;
  Stats.collective_snapshot_clauses = 0;
  Stats.collective_snapshot_clauses_peak = 0;
  Stats.collective_history_clauses = 0;
  Stats.collective_history_indexed_clauses = 0;
  Stats.collective_history_shared_clauses = 0;
  Stats.collective_history_retained_clauses = 0;
  Stats.collective_history_clause_bytes = 0;
  Stats.collective_history_queries = 0;
  Stats.collective_history_candidates = 0;
  Stats.collective_history_rejected_future = 0;
  Stats.collective_history_rejected_inactive = 0;
  Stats.collective_hint_probes_scheduled = 0;
  Stats.collective_hint_probes_expanded = 0;
  Stats.collective_hint_selected_total = 0;
  Stats.collective_hint_selected_hha = 0;
  Stats.collective_hint_selected_hw = 0;
  Stats.collective_hint_selected_lh = 0;
  Stats.collective_hint_selected_other = 0;
  Stats.collective_distinct_hints_matched = 0;
  Stats.collective_batches_pending = 0;
  Stats.collective_pending_paramod = 0;
  Stats.collective_pending_pos_hyper = 0;
  Stats.collective_pending_neg_hyper = 0;
  Stats.collective_batches_peak = 0;
  Stats.collective_descriptor_lag_sum = 0;
  Stats.collective_descriptor_lag_p50 = 0;
  Stats.collective_descriptor_lag_p95 = 0;
  Stats.collective_descriptor_lag_max = 0;
  Stats.collective_activation_entries = 0;
  Stats.collective_deactivation_entries = 0;
  Stats.collective_descriptor_bytes = 0;
  Stats.collective_history_bytes = 0;
  Stats.collective_created_paramod = 0;
  Stats.collective_created_pos_hyper = 0;
  Stats.collective_created_neg_hyper = 0;
  Stats.collective_completed_paramod = 0;
  Stats.collective_completed_pos_hyper = 0;
  Stats.collective_completed_neg_hyper = 0;
  Stats.collective_balanced_paramod_turns = 0;
  Stats.collective_balanced_pos_hyper_turns = 0;
  Stats.collective_balanced_neg_hyper_turns = 0;
  Stats.collective_balanced_oldest_turns = 0;
  Stats.collective_balanced_lane_turns = 0;
  Stats.collective_drain_entries = 0;
  Stats.collective_drain_exits = 0;
  Stats.collective_givens_withheld = 0;
  Stats.collective_paramod_from_turns = 0;
  Stats.collective_paramod_into_turns = 0;
  Stats.collective_iterator_raw_steps = 0;
  Stats.collective_iterator_candidates = 0;
  Stats.collective_iterator_completions = 0;
  Stats.collective_iterator_invalidations = 0;
  Stats.collective_iterator_raw_peak = 0;
  Stats.collective_iterator_path_bytes = 0;
  Stats.collective_hyper_iterator_raw_steps = 0;
  Stats.collective_hyper_iterator_candidates = 0;
  Stats.collective_hyper_iterator_completions = 0;
  Stats.collective_hyper_iterator_choice_bytes = 0;
  Stats.collective_candidate_pool_bytes = 0;
  Stats.collective_candidate_pool_peak_bytes = 0;
  Stats.collective_preview_calls = 0;
  Stats.collective_preview_hint_matches = 0;
  Stats.collective_preview_authoritative_matches = 0;
  Stats.collective_preview_false_positives = 0;
  Stats.collective_preview_changed_hint_ids = 0;
  Stats.collective_preview_stale_refreshes = 0;
  Stats.collective_candidate_pool_commits = 0;
  Stats.collective_candidate_priority_commits = 0;
  Stats.collective_candidate_fair_commits = 0;
  Stats.collective_discovery_turns = 0;
  Stats.collective_discovery_hot_turns = 0;
  Stats.collective_discovery_general_turns = 0;
  Stats.collective_discovery_forced_fair_turns = 0;
  Stats.collective_discovery_raw_steps = 0;
  Stats.collective_discovery_candidates = 0;
  Stats.collective_discovery_promotions = 0;
  Stats.collective_discovery_confirmed = 0;
  Stats.collective_discovery_false_positives = 0;
  Stats.collective_discovery_duplicate_skips = 0;
  Stats.collective_discovery_catchups = 0;
  Stats.collective_discovery_distance_max = 0;
  Stats.collective_discovery_cap_stalls = 0;
  Stats.collective_discovery_consumed_records = 0;
  Stats.collective_discovery_consumed_bytes = 0;
}

static
void collective_grow_activation_pages(size_t need)
{
  size_t old_capacity = Collective_activation_page_capacity;
  size_t new_capacity = old_capacity == 0 ? 16 : old_capacity;
  struct collective_activation_page **p;
  while (new_capacity < need) {
    if (new_capacity > ((size_t) -1) / 2)
      fatal_error("collective activation page table overflow");
    new_capacity *= 2;
  }
  p = safe_calloc(new_capacity, sizeof(*p));
  if (Collective_activation_pages != NULL) {
    memcpy(p, Collective_activation_pages, old_capacity * sizeof(*p));
    safe_free(Collective_activation_pages);
  }
  Collective_activation_pages = p;
  Collective_activation_page_capacity = new_capacity;
}

static size_t collective_deactivation_hash(unsigned long long id)
{
  id ^= id >> 30;
  id *= 0xbf58476d1ce4e5b9ULL;
  id ^= id >> 27;
  id *= 0x94d049bb133111ebULL;
  id ^= id >> 31;
  return (size_t) id;
}

static size_t collective_deactivation_slot(
  struct collective_deactivation_entry *table, size_t capacity,
  unsigned long long id)
{
  size_t slot = collective_deactivation_hash(id) & (capacity - 1);
  while (table[slot].id != 0 && table[slot].id != id)
    slot = (slot + 1) & (capacity - 1);
  return slot;
}

static void collective_resize_deactivations(size_t new_capacity)
{
  struct collective_deactivation_entry *old = Collective_deactivations;
  size_t old_capacity = Collective_deactivation_capacity;
  size_t i;
  Collective_deactivations = safe_calloc(new_capacity, sizeof(*old));
  Collective_deactivation_capacity = new_capacity;
  for (i = 0; i < old_capacity; i++)
    if (old[i].id != 0) {
      size_t slot = collective_deactivation_slot(
        Collective_deactivations, new_capacity, old[i].id);
      Collective_deactivations[slot] = old[i];
    }
  safe_free(old);
}

static
unsigned collective_deactivation_epoch(unsigned long long id)
{
  size_t slot;
  if (id == 0 || Collective_deactivation_capacity == 0)
    return 0;
  slot = collective_deactivation_slot(Collective_deactivations,
                                      Collective_deactivation_capacity, id);
  return Collective_deactivations[slot].id == id ?
         Collective_deactivations[slot].epoch : 0;
}

static
void collective_set_deactivation_epoch(unsigned long long id, unsigned epoch)
{
  size_t slot;
  if (id == 0)
    fatal_error("collective deactivation requires a stable clause ID");
  if (epoch == 0) {
    if (Collective_deactivation_capacity != 0) {
      slot = collective_deactivation_slot(Collective_deactivations,
                                          Collective_deactivation_capacity,
                                          id);
      if (Collective_deactivations[slot].id == id)
        Collective_deactivations[slot].epoch = 0;
    }
    return;
  }
  if (Collective_deactivation_capacity == 0)
    collective_resize_deactivations(16);
  else if ((Collective_deactivation_count + 1) * 4 >=
           Collective_deactivation_capacity * 3) {
    if (Collective_deactivation_capacity > ((size_t) -1) / 2)
      fatal_error("collective deactivation table overflow");
    collective_resize_deactivations(Collective_deactivation_capacity * 2);
  }
  slot = collective_deactivation_slot(Collective_deactivations,
                                      Collective_deactivation_capacity, id);
  if (Collective_deactivations[slot].id == 0) {
    Collective_deactivations[slot].id = id;
    Collective_deactivation_count++;
  }
  Collective_deactivations[slot].epoch = epoch;
}

static
unsigned long long collective_activation_id(unsigned long long position)
{
  size_t page_number = (size_t) (position >> COLLECTIVE_ACT_PAGE_BITS);
  unsigned offset = (unsigned) (position & (COLLECTIVE_ACT_PAGE_SIZE - 1));
  if (position >= Collective_activation_count ||
      page_number >= Collective_activation_page_count)
    fatal_error("collective activation cursor out of bounds");
  return Collective_activation_pages[page_number]->ids[offset];
}

static
BOOL collective_activation_clashable(unsigned long long position)
{
  size_t page_number = (size_t) (position >> COLLECTIVE_ACT_PAGE_BITS);
  unsigned offset = (unsigned) (position & (COLLECTIVE_ACT_PAGE_SIZE - 1));
  if (position >= Collective_activation_count ||
      page_number >= Collective_activation_page_count)
    fatal_error("collective clashable cursor out of bounds");
  return Collective_activation_pages[page_number]->clashable[offset] != 0;
}

static
Topform collective_activation_clause(unsigned long long position)
{
  size_t page_number = (size_t) (position >> COLLECTIVE_ACT_PAGE_BITS);
  unsigned offset = (unsigned) (position & (COLLECTIVE_ACT_PAGE_SIZE - 1));
  Topform c;
  if (position >= Collective_activation_count ||
      page_number >= Collective_activation_page_count)
    fatal_error("collective historical clause cursor out of bounds");
  c = Collective_activation_pages[page_number]->clauses[offset];
  if (c == NULL)
    fatal_error("collective historical clause is not materialized");
  return c;
}

static
void collective_install_history_clause(unsigned long long position, Topform c)
{
  size_t page_number = (size_t) (position >> COLLECTIVE_ACT_PAGE_BITS);
  unsigned offset = (unsigned) (position & (COLLECTIVE_ACT_PAGE_SIZE - 1));
  if (position >= Collective_activation_count ||
      page_number >= Collective_activation_page_count)
    fatal_error("collective history installation cursor out of bounds");
  if (Collective_activation_pages[page_number]->clauses[offset] != NULL)
    fatal_error("collective historical clause installed twice");
  if (c->id != Collective_activation_pages[page_number]->ids[offset])
    fatal_error("collective historical clause ID mismatch");
  if (c->compressed != NULL || c->literals == NULL)
    fatal_error("collective history requires a materialized nonempty clause");
  if (Collective_historical_idx == NULL) {
    int fpa_depth = parm(Opt->fpa_depth);
    Collective_historical_idx = lindex_init(
      FPA, ORDINARY_UNIF, fpa_depth, FPA, ORDINARY_UNIF, fpa_depth);
  }
  /* The active clause body is immutable.  Share it until disable/archive,
     when ownership is transferred to collective history. */
  c->collective_history = 1;
  Collective_activation_pages[page_number]->clauses[offset] = c;
  if (Collective_activation_pages[page_number]->clashable[offset])
    lindex_update(Collective_historical_idx, c, INSERT);
}

static
void collective_append_activation_id(unsigned long long id, BOOL clashable)
{
  size_t page_number =
    (size_t) (Collective_activation_count >> COLLECTIVE_ACT_PAGE_BITS);
  unsigned offset =
    (unsigned) (Collective_activation_count & (COLLECTIVE_ACT_PAGE_SIZE - 1));
  if (page_number == Collective_activation_page_count) {
    if (page_number >= Collective_activation_page_capacity)
      collective_grow_activation_pages(page_number + 1);
    Collective_activation_pages[page_number] = safe_calloc(
      1, sizeof(struct collective_activation_page));
    Collective_activation_page_count++;
  }
  Collective_activation_pages[page_number]->ids[offset] = id;
  Collective_activation_pages[page_number]->clashable[offset] =
    clashable ? 1 : 0;
  Collective_activation_count++;
}

static
void collective_note_activation(Topform c, BOOL clashable)
{
  if (!collective_frontier_mode())
    return;
  if (c == NULL || c->id == 0)
    fatal_error("collective activation requires a stable clause ID");
  c->simplifier_epoch = Simplifier_epoch;
  collective_set_deactivation_epoch(c->id, 0);
  collective_append_activation_id(c->id, clashable);
  collective_install_history_clause(Collective_activation_count - 1, c);
}

static
void collective_note_deactivation(Topform c)
{
  if (!collective_frontier_mode() || c == NULL || c->id == 0)
    return;
  if (collective_deactivation_epoch(c->id) == 0)
    collective_set_deactivation_epoch(c->id, Simplifier_epoch);
}

static struct collective_batch *collective_new_batch(void)
{
  struct collective_batch *b = safe_calloc(1, sizeof(*b));
  para_iterator_init(&b->para_iterator);
  hyper_iterator_init(&b->hyper_iterator);
  para_iterator_init(&b->discovery_para_iterator);
  hyper_iterator_init(&b->discovery_hyper_iterator);
  b->priority_heap_index = COLLECTIVE_NO_HEAP_INDEX;
  return b;
}

static void collective_free_batch(struct collective_batch *b)
{
  para_iterator_zap(&b->para_iterator);
  hyper_iterator_zap(&b->hyper_iterator);
  para_iterator_zap(&b->discovery_para_iterator);
  hyper_iterator_zap(&b->discovery_hyper_iterator);
  safe_free(b->consumed);
  safe_free(b);
}

static BOOL collective_batch_priority_less(
  struct collective_batch *a, struct collective_batch *b)
{
  if (a->conclusion_next_weight < b->conclusion_next_weight)
    return TRUE;
  else if (a->conclusion_next_weight > b->conclusion_next_weight)
    return FALSE;
  else if (a->given_id < b->given_id)
    return TRUE;
  else if (a->given_id > b->given_id)
    return FALSE;
  else
    return a->conclusion_next_ordinal < b->conclusion_next_ordinal;
}

static void collective_priority_heap_swap(size_t a, size_t b)
{
  struct collective_batch *tmp = Collective_priority_heap[a];
  Collective_priority_heap[a] = Collective_priority_heap[b];
  Collective_priority_heap[b] = tmp;
  Collective_priority_heap[a]->priority_heap_index = a;
  Collective_priority_heap[b]->priority_heap_index = b;
}

static void collective_priority_heap_grow(void)
{
  size_t old_capacity = Collective_priority_heap_capacity;
  size_t new_capacity = old_capacity == 0 ? 64 : old_capacity * 2;
  struct collective_batch **heap;
  if (new_capacity < old_capacity)
    fatal_error("collective priority heap capacity overflow");
  heap = safe_calloc(new_capacity, sizeof(*heap));
  if (old_capacity != 0) {
    memcpy(heap, Collective_priority_heap,
           old_capacity * sizeof(*heap));
    safe_free(Collective_priority_heap);
  }
  Collective_priority_heap = heap;
  Collective_priority_heap_capacity = new_capacity;
}

static void collective_priority_heap_insert(struct collective_batch *b)
{
  size_t at;
  if ((b->kind & COLLECTIVE_PROMISING_CURSOR) == 0)
    return;
  if (b->priority_heap_index != COLLECTIVE_NO_HEAP_INDEX)
    fatal_error("collective descriptor inserted into priority heap twice");
  if (Collective_priority_heap_count == Collective_priority_heap_capacity)
    collective_priority_heap_grow();
  at = Collective_priority_heap_count++;
  Collective_priority_heap[at] = b;
  b->priority_heap_index = at;
  while (at != 0) {
    size_t parent = (at - 1) / 2;
    if (!collective_batch_priority_less(Collective_priority_heap[at],
                                        Collective_priority_heap[parent]))
      break;
    collective_priority_heap_swap(at, parent);
    at = parent;
  }
  if (Collective_priority_heap_count > Stats.collective_promising_heap_peak)
    Stats.collective_promising_heap_peak = Collective_priority_heap_count;
}

static void collective_priority_heap_remove(struct collective_batch *b)
{
  size_t at, parent;
  if (b->priority_heap_index == COLLECTIVE_NO_HEAP_INDEX)
    return;
  at = b->priority_heap_index;
  if (at >= Collective_priority_heap_count ||
      Collective_priority_heap[at] != b)
    fatal_error("collective priority heap index corrupted");
  b->priority_heap_index = COLLECTIVE_NO_HEAP_INDEX;
  Collective_priority_heap_count--;
  if (at == Collective_priority_heap_count) {
    Collective_priority_heap[at] = NULL;
    return;
  }
  Collective_priority_heap[at] =
    Collective_priority_heap[Collective_priority_heap_count];
  Collective_priority_heap[Collective_priority_heap_count] = NULL;
  Collective_priority_heap[at]->priority_heap_index = at;

  parent = at == 0 ? 0 : (at - 1) / 2;
  if (at != 0 &&
      collective_batch_priority_less(Collective_priority_heap[at],
                                      Collective_priority_heap[parent])) {
    while (at != 0) {
      parent = (at - 1) / 2;
      if (!collective_batch_priority_less(Collective_priority_heap[at],
                                          Collective_priority_heap[parent]))
        break;
      collective_priority_heap_swap(at, parent);
      at = parent;
    }
  }
  else {
    while (TRUE) {
      size_t left = at * 2 + 1;
      size_t right = left + 1;
      size_t smallest = at;
      if (left < Collective_priority_heap_count &&
          collective_batch_priority_less(Collective_priority_heap[left],
                                          Collective_priority_heap[smallest]))
        smallest = left;
      if (right < Collective_priority_heap_count &&
          collective_batch_priority_less(Collective_priority_heap[right],
                                          Collective_priority_heap[smallest]))
        smallest = right;
      if (smallest == at)
        break;
      collective_priority_heap_swap(at, smallest);
      at = smallest;
    }
  }
}

static
void collective_append_batch(struct collective_batch *b)
{
  if (b->priority_heap_index != COLLECTIVE_NO_HEAP_INDEX)
    fatal_error("collective queued descriptor is still in priority heap");
  b->prev = Collective_batch_tail;
  b->next = NULL;
  if (Collective_batch_tail == NULL)
    Collective_batch_head = b;
  else
    Collective_batch_tail->next = b;
  Collective_batch_tail = b;
  Collective_batch_count++;
  if (collective_balanced_mode() &&
      Collective_batch_count >
        (unsigned long long) parm(Opt->collective_descriptor_high_water))
    fatal_error("collective balanced descriptor high-water exceeded");
  if (Collective_batch_count > Stats.collective_batches_peak)
    Stats.collective_batches_peak = Collective_batch_count;
  collective_priority_heap_insert(b);
}

/* Give a newly activated hint-matched clause one prompt descriptor turn.
   This is deliberately a bounded probe, not a second priority queue: the
   probe consumes a credit which only an ordinary FIFO expansion restores.
   Therefore even an infinite stream of hint matches cannot starve an older
   finite descriptor. */
static
void collective_maybe_schedule_hint_probe(
  Topform given, struct collective_batch *tail_before)
{
  struct collective_batch *b = Collective_batch_tail;

  if (!flag(Opt->collective_hint_probes) ||
      !Collective_hint_probe_credit ||
      given->matching_hint == NULL ||
      b == NULL || b == tail_before ||
      b->given_id != given->id ||
      b->snapshot_epoch != Simplifier_epoch ||
      (b->kind & COLLECTIVE_PARAMOD) == 0)
    return;

  /* given_infer appends at most one combined descriptor.  If other work was
     already queued, unlink that new tail and move it to the head in O(1). */
  if (Collective_batch_head != b) {
    if (tail_before == NULL || tail_before->next != b)
      fatal_error("collective hint probe queue order corrupted");
    tail_before->next = NULL;
    Collective_batch_tail = tail_before;
    b->prev = NULL;
    b->next = Collective_batch_head;
    Collective_batch_head->prev = b;
    Collective_batch_head = b;
  }

  b->kind |= COLLECTIVE_HINT_PROBE;
  Collective_hint_probe_credit = FALSE;
  Collective_batch_turn = TRUE;
  Stats.collective_hint_probes_scheduled++;
}

static
void collective_enqueue_batch(Topform given)
{
  struct collective_batch *b;
  if (collective_balanced_mode()) {
    unsigned directions[2] = {
      COLLECTIVE_PARAMOD_FROM, COLLECTIVE_PARAMOD_INTO
    };
    int i;
    for (i = 0; i < 2; i++) {
      b = collective_new_batch();
      b->given_id = given->id;
      b->cursor = 0;
      b->activation_limit = Collective_activation_count;
      b->snapshot_epoch = Simplifier_epoch;
      b->kind = directions[i];
      b->discovery_hot = given->matching_hint != NULL;
      collective_append_batch(b);
      Stats.collective_batches_created++;
      Stats.collective_created_paramod++;
    }
    return;
  }
  Stats.collective_created_paramod++;
  if (Collective_batch_tail != NULL &&
      Collective_batch_tail->given_id == given->id &&
      Collective_batch_tail->snapshot_epoch == Simplifier_epoch) {
    Collective_batch_tail->kind |= COLLECTIVE_PARAMOD;
    Collective_batch_tail->activation_limit = Collective_activation_count;
    return;
  }
  b = collective_new_batch();
  b->given_id = given->id;
  b->cursor = 0;
  b->activation_limit = Collective_activation_count;
  b->snapshot_epoch = Simplifier_epoch;
  b->kind = COLLECTIVE_PARAMOD;
  b->discovery_hot = given->matching_hint != NULL;
  collective_append_batch(b);
  Stats.collective_batches_created++;
}

static
void collective_enqueue_hyper_batch(Topform given, unsigned kind)
{
  struct collective_batch *b;
  if (kind == COLLECTIVE_POS_HYPER)
    Stats.collective_created_pos_hyper++;
  else if (kind == COLLECTIVE_NEG_HYPER)
    Stats.collective_created_neg_hyper++;
  if (!collective_balanced_mode() && Collective_batch_tail != NULL &&
      Collective_batch_tail->given_id == given->id &&
      Collective_batch_tail->snapshot_epoch == Simplifier_epoch) {
    Collective_batch_tail->kind |= kind;
    Collective_batch_tail->activation_limit = Collective_activation_count;
    return;
  }
  b = collective_new_batch();
  b->given_id = given->id;
  b->activation_limit = Collective_activation_count;
  b->snapshot_epoch = Simplifier_epoch;
  b->kind = kind;
  b->discovery_hot = given->matching_hint != NULL;
  collective_append_batch(b);
  Stats.collective_batches_created++;
}

static
void collective_rotate_or_complete_batch(struct collective_batch *b)
{
  BOOL hyper_pending =
    (b->kind & (COLLECTIVE_POS_HYPER | COLLECTIVE_NEG_HYPER)) != 0;
  BOOL paramod_pending =
    (b->kind & (COLLECTIVE_PARAMOD | COLLECTIVE_PARAMOD_FROM |
                COLLECTIVE_PARAMOD_INTO)) != 0 &&
    b->cursor < b->activation_limit;
  if (!hyper_pending && !paramod_pending) {
    if (b->consumed_count != 0)
      fatal_error("collective descriptor completed with promoted ordinals ahead");
    if ((b->kind & (COLLECTIVE_PARAMOD | COLLECTIVE_PARAMOD_FROM |
                    COLLECTIVE_PARAMOD_INTO)) != 0)
      Stats.collective_completed_paramod++;
    collective_free_batch(b);
    Stats.collective_batches_completed++;
  }
  else
    collective_append_batch(b);
}

static
void collective_finish_batch_turn(struct collective_batch *b)
{
  if (Collective_batch_head != b)
    fatal_error("collective batch queue order corrupted");
  collective_priority_heap_remove(b);
  if ((b->kind & COLLECTIVE_HINT_PROBE) != 0) {
    b->kind &= ~COLLECTIVE_HINT_PROBE;
    Stats.collective_hint_probes_expanded++;
  }
  else
    Collective_hint_probe_credit = TRUE;
  Collective_batch_head = b->next;
  if (Collective_batch_head == NULL)
    Collective_batch_tail = NULL;
  else
    Collective_batch_head->prev = NULL;
  b->prev = NULL;
  b->next = NULL;
  Collective_batch_count--;
  collective_rotate_or_complete_batch(b);
}

/* Select the least next raw key among already-scanned descriptors, but force
   one FIFO head expansion per configured interval.  The FIFO turn discovers
   unknown keys and advances every finite descriptor even under an unbounded
   stream of lower-weight work. */
static BOOL collective_choose_batch_for_turn(void)
{
  unsigned interval;
  struct collective_batch *b;

  if (!flag(Opt->collective_promising_candidates) ||
      !flag(Opt->collective_promising_scheduler)) {
    Collective_priority_turns_since_fair = 0;
    return FALSE;
  }
  if (Collective_batch_head == NULL ||
      (Collective_batch_head->kind & COLLECTIVE_HINT_PROBE) != 0)
    return FALSE;

  interval = (unsigned) parm(Opt->collective_promising_fair_interval);
  if (Collective_priority_heap_count != 0 && interval > 1 &&
      Collective_priority_turns_since_fair < interval - 1) {
    b = Collective_priority_heap[0];
    if (b != Collective_batch_head) {
      if (b->prev == NULL)
        fatal_error("collective priority descriptor has no predecessor");
      b->prev->next = b->next;
      if (b->next == NULL)
        Collective_batch_tail = b->prev;
      else
        b->next->prev = b->prev;
      b->prev = NULL;
      b->next = Collective_batch_head;
      Collective_batch_head->prev = b;
      Collective_batch_head = b;
    }
    Collective_priority_turns_since_fair++;
    Stats.collective_promising_priority_turns++;
    return TRUE;
  }

  Collective_priority_turns_since_fair = 0;
  Stats.collective_promising_fair_turns++;
  return FALSE;
}

static BOOL collective_batch_in_balanced_lane(
  struct collective_batch *b, unsigned lane)
{
  if (lane == COLLECTIVE_LANE_PARAMOD)
    return (b->kind & (COLLECTIVE_PARAMOD_FROM |
                       COLLECTIVE_PARAMOD_INTO)) != 0;
  else if (lane == COLLECTIVE_LANE_POS_HYPER)
    return (b->kind & COLLECTIVE_POS_HYPER) != 0;
  else if (lane == COLLECTIVE_LANE_NEG_HYPER)
    return (b->kind & COLLECTIVE_NEG_HYPER) != 0;
  else
    fatal_error("unknown collective balanced lane");
  return FALSE;
}

static unsigned collective_balanced_lane_share(unsigned lane)
{
  if (lane == COLLECTIVE_LANE_PARAMOD)
    return (unsigned) parm(Opt->collective_paramod_share);
  else if (lane == COLLECTIVE_LANE_POS_HYPER)
    return (unsigned) parm(Opt->collective_pos_hyper_share);
  else if (lane == COLLECTIVE_LANE_NEG_HYPER)
    return (unsigned) parm(Opt->collective_neg_hyper_share);
  else
    fatal_error("unknown collective balanced lane");
  return 0;
}

static void collective_move_batch_to_head(struct collective_batch *b)
{
  if (b == Collective_batch_head)
    return;
  if (b == NULL || b->prev == NULL)
    fatal_error("collective balanced queue link corrupted");
  b->prev->next = b->next;
  if (b->next == NULL)
    Collective_batch_tail = b->prev;
  else
    b->next->prev = b->prev;
  b->prev = NULL;
  b->next = Collective_batch_head;
  Collective_batch_head->prev = b;
  Collective_batch_head = b;
}

/* Deterministic weighted round robin chooses the oldest queued descriptor
   in each rule lane.  The mandatory FIFO turn is independent of lane
   weights, so even a continuously replenished high-share lane cannot hide
   an older finite descriptor.  Phase 1 charges one legacy generator call;
   native iterator phases replace that opaque charge with raw work units. */
static void collective_choose_balanced_batch_for_turn(void)
{
  unsigned interval =
    (unsigned) parm(Opt->collective_balanced_fair_interval);
  unsigned attempts;

  if (Collective_batch_head == NULL)
    return;

  if (interval == 1 ||
      Collective_balanced_turns_since_oldest >= interval - 1) {
    Collective_balanced_turns_since_oldest = 0;
    Stats.collective_balanced_oldest_turns++;
    return;
  }

  for (attempts = 0; attempts < COLLECTIVE_LANE_COUNT; attempts++) {
    struct collective_batch *b;
    unsigned lane = Collective_balanced_lane;
    if (Collective_balanced_lane_credit == 0)
      Collective_balanced_lane_credit =
        collective_balanced_lane_share(lane);
    if (Collective_balanced_lane_credit != 0) {
      for (b = Collective_batch_head; b != NULL; b = b->next)
        if (collective_batch_in_balanced_lane(b, lane))
          break;
      if (b != NULL) {
        collective_move_batch_to_head(b);
        Collective_balanced_lane_credit--;
        if (lane == COLLECTIVE_LANE_PARAMOD)
          Stats.collective_balanced_paramod_turns++;
        else if (lane == COLLECTIVE_LANE_POS_HYPER)
          Stats.collective_balanced_pos_hyper_turns++;
        else
          Stats.collective_balanced_neg_hyper_turns++;
        Stats.collective_balanced_lane_turns++;
        Collective_balanced_turns_since_oldest++;
        if (Collective_balanced_lane_credit == 0)
          Collective_balanced_lane =
            (Collective_balanced_lane + 1) % COLLECTIVE_LANE_COUNT;
        return;
      }
    }
    Collective_balanced_lane_credit = 0;
    Collective_balanced_lane =
      (Collective_balanced_lane + 1) % COLLECTIVE_LANE_COUNT;
  }

  fatal_error("collective balanced scheduler found no eligible lane");
}

static
unsigned long long collective_history_bytes(void)
{
  return (unsigned long long) Collective_activation_page_count *
           sizeof(struct collective_activation_page) +
         (unsigned long long) Collective_activation_page_capacity *
           sizeof(*Collective_activation_pages) +
         (unsigned long long) Collective_deactivation_capacity *
           sizeof(*Collective_deactivations);
}

static unsigned long long collective_iterator_path_bytes(void)
{
  struct collective_batch *b;
  unsigned long long bytes = 0;
  for (b = Collective_batch_head; b != NULL; b = b->next)
    bytes += (unsigned long long) b->para_iterator.path_capacity *
             sizeof(*b->para_iterator.path);
  for (b = Collective_batch_head; b != NULL; b = b->next)
    bytes += (unsigned long long) b->discovery_para_iterator.path_capacity *
             sizeof(*b->discovery_para_iterator.path);
  return bytes;
}

static unsigned long long collective_hyper_iterator_choice_bytes(void)
{
  struct collective_batch *b;
  unsigned long long bytes = 0;
  for (b = Collective_batch_head; b != NULL; b = b->next)
    bytes += (unsigned long long) b->hyper_iterator.choice_capacity *
             sizeof(*b->hyper_iterator.choices);
  for (b = Collective_batch_head; b != NULL; b = b->next)
    bytes +=
      (unsigned long long) b->discovery_hyper_iterator.choice_capacity *
      sizeof(*b->discovery_hyper_iterator.choices);
  return bytes;
}

static unsigned long long collective_consumed_bytes(void)
{
  struct collective_batch *b;
  unsigned long long bytes = 0;
  for (b = Collective_batch_head; b != NULL; b = b->next)
    bytes += (unsigned long long) b->consumed_capacity * sizeof(*b->consumed);
  return bytes;
}

static unsigned long long collective_consumed_records(void)
{
  struct collective_batch *b;
  unsigned long long records = 0;
  for (b = Collective_batch_head; b != NULL; b = b->next)
    records += b->consumed_count;
  return records;
}

static void collective_clone_para_iterator(Para_iterator *to,
					   const Para_iterator *from)
{
  unsigned *path = NULL;
  unsigned depth = from->path_depth;
  para_iterator_zap(to);
  *to = *from;
  if (depth != 0) {
    path = safe_calloc(depth, sizeof(*path));
    memcpy(path, from->path, depth * sizeof(*path));
  }
  to->path = path;
  to->path_capacity = depth;
}

static void collective_clone_hyper_iterator(Hyper_iterator *to,
					    const Hyper_iterator *from)
{
  Hyper_iterator_choice *choices = NULL;
  unsigned depth = from->depth;
  hyper_iterator_zap(to);
  *to = *from;
  if (depth != 0) {
    choices = safe_calloc(depth, sizeof(*choices));
    memcpy(choices, from->choices, depth * sizeof(*choices));
  }
  to->choices = choices;
  to->choice_capacity = depth;
}

static void collective_discovery_sync(struct collective_batch *b)
{
  if (!b->discovery_initialized ||
      b->candidate_ordinal > b->discovery_ordinal) {
    if (b->candidate_ordinal > b->discovery_ordinal &&
        b->consumed_count != 0)
      fatal_error("collective discovery cursor passed unconsumed promotion");
    b->discovery_cursor = b->cursor;
    b->discovery_ordinal = b->candidate_ordinal;
    collective_clone_para_iterator(&b->discovery_para_iterator,
                                   &b->para_iterator);
    collective_clone_hyper_iterator(&b->discovery_hyper_iterator,
                                    &b->hyper_iterator);
    b->discovery_initialized = TRUE;
    b->discovery_complete = FALSE;
    Stats.collective_discovery_catchups++;
  }
}

static void collective_add_consumed(struct collective_batch *b,
				    unsigned long long ordinal,
				    unsigned long long raw_fingerprint)
{
  if (b->consumed_count != 0 &&
      b->consumed[b->consumed_count-1].ordinal >= ordinal)
    fatal_error("collective discovery ordinals are not increasing");
  if (b->consumed_count == b->consumed_capacity) {
    unsigned capacity = b->consumed_capacity == 0 ? 8 :
                        b->consumed_capacity * 2;
    unsigned cap_limit =
      (unsigned) parm(Opt->collective_discovery_promotion_cap);
    if (capacity > cap_limit)
      capacity = cap_limit;
    if (capacity <= b->consumed_capacity)
      fatal_error("collective discovery promotion cap exceeded");
    b->consumed = safe_realloc(
      b->consumed, (size_t) capacity * sizeof(*b->consumed));
    b->consumed_capacity = capacity;
  }
  b->consumed[b->consumed_count].ordinal = ordinal;
  b->consumed[b->consumed_count].raw_fingerprint = raw_fingerprint;
  b->consumed_count++;
}

static BOOL collective_consume_if_promoted(struct collective_batch *b,
					   unsigned long long ordinal,
					   Topform c)
{
  if (b->consumed_count == 0 || b->consumed[0].ordinal != ordinal)
    return FALSE;
  if (b->consumed[0].raw_fingerprint !=
        hint_trace_clause_hash(c))
    fatal_error("collective discovery raw conclusion changed at fair cursor");
  if (b->consumed_count > 1)
    memmove(b->consumed, b->consumed + 1,
            (size_t) (b->consumed_count - 1) * sizeof(*b->consumed));
  b->consumed_count--;
  Stats.collective_discovery_duplicate_skips++;
  Stats.collective_discovery_catchups++;
  delete_clause(c);
  return TRUE;
}

static
unsigned long long collective_descriptor_lag(struct collective_batch *b)
{
  return Collective_activation_count > b->activation_limit ?
    Collective_activation_count - b->activation_limit : 0;
}

static unsigned collective_balanced_max_activation_descriptors(void)
{
  unsigned n = 0;
  if (flag(Opt->pos_hyper_resolution))
    n++;
  if (flag(Opt->neg_hyper_resolution))
    n++;
  if (flag(Opt->paramodulation))
    n += 2;
  return n;
}

static unsigned long long collective_oldest_pending_lag(void)
{
  struct collective_batch *b;
  unsigned long long maximum = 0;
  for (b = Collective_batch_head; b != NULL; b = b->next) {
    unsigned long long lag = collective_descriptor_lag(b);
    if (lag > maximum)
      maximum = lag;
  }
  return maximum;
}

static BOOL collective_balanced_admission_allowed(void)
{
  unsigned long long reserve =
    collective_balanced_max_activation_descriptors();
  unsigned long long high =
    (unsigned long long) parm(Opt->collective_descriptor_high_water);
  return Collective_batch_count <= high && reserve <= high - Collective_batch_count;
}

static BOOL collective_update_drain_mode(void)
{
  unsigned long long lag;
  BOOL pressure;
  if (!collective_balanced_mode())
    return FALSE;
  if (Collective_batch_count == 0) {
    if (Collective_drain_mode) {
      Collective_drain_mode = FALSE;
      Stats.collective_drain_exits++;
    }
    return FALSE;
  }
  lag = collective_oldest_pending_lag();
  pressure = !collective_balanced_admission_allowed() ||
    lag >= (unsigned long long) parm(Opt->collective_oldest_lag_limit);
  if (!Collective_drain_mode && pressure) {
    Collective_drain_mode = TRUE;
    Stats.collective_drain_entries++;
  }
  else if (Collective_drain_mode &&
           Collective_batch_count <=
             (unsigned long long) parm(Opt->collective_descriptor_low_water) &&
           lag < (unsigned long long) parm(Opt->collective_oldest_lag_limit) &&
           collective_balanced_admission_allowed()) {
    Collective_drain_mode = FALSE;
    Stats.collective_drain_exits++;
  }
  return Collective_drain_mode;
}

/* Return the exact zero-based order statistic without allocating an array
   proportional to the pending frontier.  The extra queue passes occur only
   when aggregate statistics are requested. */
static
unsigned long long collective_descriptor_lag_at_rank(
  unsigned long long rank, unsigned long long maximum)
{
  unsigned long long low = 0;
  unsigned long long high = maximum;

  while (low < high) {
    unsigned long long midpoint = low + (high - low) / 2;
    unsigned long long at_or_below = 0;
    struct collective_batch *b;
    for (b = Collective_batch_head; b != NULL; b = b->next) {
      if (collective_descriptor_lag(b) <= midpoint)
        at_or_below++;
    }
    if (at_or_below > rank)
      high = midpoint;
    else
      low = midpoint + 1;
  }
  return low;
}

static
void collective_descriptor_stats(
  unsigned long long *paramod,
  unsigned long long *pos_hyper,
  unsigned long long *neg_hyper,
  unsigned long long *lag_sum,
  unsigned long long *lag_p50,
  unsigned long long *lag_p95,
  unsigned long long *lag_max)
{
  struct collective_batch *b;
  unsigned long long count = 0;

  *paramod = 0;
  *pos_hyper = 0;
  *neg_hyper = 0;
  *lag_sum = 0;
  *lag_p50 = 0;
  *lag_p95 = 0;
  *lag_max = 0;

  for (b = Collective_batch_head; b != NULL; b = b->next) {
    unsigned long long lag = collective_descriptor_lag(b);
    count++;
    if ((b->kind & (COLLECTIVE_PARAMOD | COLLECTIVE_PARAMOD_FROM |
                    COLLECTIVE_PARAMOD_INTO)) != 0)
      (*paramod)++;
    if ((b->kind & COLLECTIVE_POS_HYPER) != 0)
      (*pos_hyper)++;
    if ((b->kind & COLLECTIVE_NEG_HYPER) != 0)
      (*neg_hyper)++;
    *lag_sum += lag;
    if (lag > *lag_max)
      *lag_max = lag;
  }

  if (count != 0) {
    unsigned long long p50_rank = (count - 1) / 2;
    unsigned long long p95_count =
      (count / 100) * 95 + ((count % 100) * 95 + 99) / 100;
    *lag_p50 = collective_descriptor_lag_at_rank(p50_rank, *lag_max);
    *lag_p95 = collective_descriptor_lag_at_rank(p95_count - 1, *lag_max);
  }
}

static
void collective_history_clause_stats(unsigned long long *clauses,
                                     unsigned long long *indexed,
                                     unsigned long long *shared,
                                     unsigned long long *retained,
                                     unsigned long long *bytes)
{
  unsigned long long position;
  *clauses = 0;
  *indexed = 0;
  *shared = 0;
  *retained = 0;
  *bytes = 0;
  for (position = 0; position < Collective_activation_count; position++) {
    size_t page_number = (size_t) (position >> COLLECTIVE_ACT_PAGE_BITS);
    unsigned offset =
      (unsigned) (position & (COLLECTIVE_ACT_PAGE_SIZE - 1));
    Topform c = Collective_activation_pages[page_number]->clauses[offset];
    if (c != NULL) {
      (*clauses)++;
      if (Collective_activation_pages[page_number]->clashable[offset])
        (*indexed)++;
      if (c->official_id)
        (*shared)++;
      else {
        (*retained)++;
        *bytes += sizeof(struct topform) + clause_body_storage_bytes(c);
      }
    }
  }
}

// How many statics are to be output?

/*************
 *
 *    init_prover_options()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
Prover_options init_prover_options(void)
{
  Prover_options p = safe_calloc(1, sizeof(struct prover_options));
  // FLAGS:
  //   internal name                    external name            default

  // The following are now in ../ladr/std_options.c.
  // ?? = init_flag("prolog_style_variables", FALSE);
  // ?? = init_flag("clocks",                 FALSE);

  p->binary_resolution      = init_flag("binary_resolution",      FALSE);
  p->neg_binary_resolution  = init_flag("neg_binary_resolution",  FALSE);
  p->hyper_resolution       = init_flag("hyper_resolution",       FALSE);
  p->pos_hyper_resolution   = init_flag("pos_hyper_resolution",   FALSE);
  p->neg_hyper_resolution   = init_flag("neg_hyper_resolution",   FALSE);
  p->ur_resolution          = init_flag("ur_resolution",          FALSE);
  p->pos_ur_resolution      = init_flag("pos_ur_resolution",      FALSE);
  p->neg_ur_resolution      = init_flag("neg_ur_resolution",      FALSE);
  p->paramodulation         = init_flag("paramodulation",         FALSE);
  p->eval_rewrite           = init_flag("eval_rewrite",           FALSE);

  p->ordered_res            = init_flag("ordered_res",            TRUE);
  p->check_res_instances    = init_flag("check_res_instances",    FALSE);
  p->ordered_para           = init_flag("ordered_para",           TRUE);
  p->check_para_instances   = init_flag("check_para_instances",   FALSE);
  p->para_units_only        = init_flag("para_units_only",        FALSE);
  p->para_from_vars         = init_flag("para_from_vars",         TRUE);
  p->para_into_vars         = init_flag("para_into_vars",         FALSE);
  p->para_from_small        = init_flag("para_from_small",        FALSE);
  p->basic_paramodulation   = init_flag("basic_paramodulation",   FALSE);
  p->initial_nuclei         = init_flag("initial_nuclei",         FALSE);

  p->process_initial_sos    = init_flag("process_initial_sos",     TRUE);
  p->back_demod             = init_flag("back_demod",              TRUE);
  p->lex_dep_demod          = init_flag("lex_dep_demod",           TRUE);
  p->lex_dep_demod_sane     = init_flag("lex_dep_demod_sane",      TRUE);
  p->safe_unit_conflict     = init_flag("safe_unit_conflict",     FALSE);
  p->reuse_denials          = init_flag("reuse_denials",          FALSE);
  p->back_subsume           = init_flag("back_subsume",            TRUE);
  p->back_subsume_skip_used = init_flag("back_subsume_skip_used", FALSE);
  p->back_subsume_skip_limbo= init_flag("back_subsume_skip_limbo",FALSE);
  p->ancestor_subsume       = init_flag("ancestor_subsume",       FALSE);
  p->proof_weight           = init_flag("proof_weight",           FALSE);
  p->unit_deletion          = init_flag("unit_deletion",          FALSE);
  p->factor                 = init_flag("factor",                 FALSE);
  p->cac_redundancy         = init_flag("cac_redundancy",          TRUE);
  p->degrade_hints          = init_flag("degrade_hints",           TRUE);
  p->limit_hint_matchers    = init_flag("limit_hint_matchers",    FALSE);
  p->back_demod_hints       = init_flag("back_demod_hints",        TRUE);
  p->collect_hint_labels    = init_flag("collect_hint_labels",    FALSE);
  p->hint_match_stats       = init_flag("hint_match_stats",       FALSE);
  p->hint_match_once        = init_flag("hint_match_once",        FALSE);
  p->hint_trace             = init_flag("hint_trace",             FALSE);
  p->search_event_trace     = init_flag("search_event_trace",     FALSE);
  p->compact_otter_audit    = init_flag("compact_otter_audit",    FALSE);
  p->compact_otter_demodulation =
    init_flag("compact_otter_demodulation", FALSE);
  p->compact_unit_subsumption_audit =
    init_flag("compact_unit_subsumption_audit", FALSE);
  p->compact_otter_unit_index = init_flag("compact_otter_unit_index", FALSE);
  p->compact_back_demod_audit =
    init_flag("compact_back_demod_audit", FALSE);
  p->compact_otter_back_demod_index =
    init_flag("compact_otter_back_demod_index", FALSE);
  p->compact_nonunit_subsumption_audit =
    init_flag("compact_nonunit_subsumption_audit", FALSE);
  p->compact_otter_nonunit_index =
    init_flag("compact_otter_nonunit_index", FALSE);
  p->compact_term_sharing_stats =
    init_flag("compact_term_sharing_stats", FALSE);
  p->collective_trace       = init_flag("collective_trace",       FALSE);
  p->collective_hint_probes = init_flag("collective_hint_probes",  FALSE);
  p->collective_promising_candidates =
    init_flag("collective_promising_candidates", FALSE);
  p->collective_promising_scheduler =
    init_flag("collective_promising_scheduler", FALSE);
  p->collective_hint_discovery =
    init_flag("collective_hint_discovery", TRUE);
  p->print_matched_hints    = init_flag("print_matched_hints",    FALSE);
  p->print_derivations      = init_flag("print_derivations",      FALSE);
  p->derivations_only       = init_flag("derivations_only",        TRUE);
  p->print_new_hints        = init_flag("print_new_hints",        FALSE);
  p->dont_flip_input        = init_flag("dont_flip_input",        FALSE);

  p->echo_input             = init_flag("echo_input",              TRUE);
  p->bell                   = init_flag("bell",                    TRUE);
  p->quiet                  = init_flag("quiet",                  FALSE);
  p->print_initial_clauses  = init_flag("print_initial_clauses",   TRUE);
  p->print_given            = init_flag("print_given",             TRUE);
  p->print_gen              = init_flag("print_gen",              FALSE);
  p->print_kept             = init_flag("print_kept",             FALSE);
  p->print_labeled          = init_flag("print_labeled",          FALSE);
  p->print_proofs           = init_flag("print_proofs",            TRUE);
  p->print_proof_goal       = init_flag("print_proof_goal",       FALSE);
  p->print_expanded_proof   = init_flag("print_expanded_proof",   FALSE);
  p->print_substitutions    = init_flag("print_substitutions",    FALSE);
  p->default_output         = init_flag("default_output",          TRUE);
  p->print_clause_properties= init_flag("print_clause_properties",FALSE);

  p->expand_relational_defs = init_flag("expand_relational_defs", FALSE);
  p->predicate_elim         = init_flag("predicate_elim",          TRUE);
  p->inverse_order          = init_flag("inverse_order",           TRUE);
  p->sort_initial_sos       = init_flag("sort_initial_sos",       FALSE);
  p->restrict_denials       = init_flag("restrict_denials",       FALSE);

  p->input_sos_first        = init_flag("input_sos_first",         TRUE);
  p->breadth_first          = init_flag("breadth_first",          FALSE);
  p->lightest_first         = init_flag("lightest_first",         FALSE);
  p->random_given           = init_flag("random_given",           FALSE);
  p->breadth_first_hints    = init_flag("breadth_first_hints",    FALSE);
  p->default_parts          = init_flag("default_parts",           TRUE);

  p->automatic              = init_flag("auto",                    TRUE);
  p->auto_setup             = init_flag("auto_setup",              TRUE);
  p->auto_limits            = init_flag("auto_limits",             TRUE);
  p->auto_denials           = init_flag("auto_denials",            TRUE);
  p->auto_inference         = init_flag("auto_inference",          TRUE);
  p->auto_process           = init_flag("auto_process",            TRUE);
  p->auto2                  = init_flag("auto2",                  FALSE);
  p->raw                    = init_flag("raw",                    FALSE);
  p->production             = init_flag("production",             FALSE);

  p->lex_order_vars         = init_flag("lex_order_vars",         FALSE);
  p->comma_stats            = init_flag("comma_stats",            FALSE);
  p->report_index_stats     = init_flag("report_index_stats",     FALSE);
  p->compress_disabled      = init_flag("compress_disabled",      FALSE);

  p->checkpoint_exit        = init_flag("checkpoint_exit",        FALSE);
  p->checkpoint_ancestors   = init_flag("checkpoint_ancestors",    TRUE);
  p->checkpoint_verify      = init_flag("checkpoint_verify",      FALSE);
  p->tptp_output            = init_flag("tptp_output",            FALSE);
  p->multi_order_trial      = init_flag("multi_order_trial",      FALSE);
  p->fast_pred_elim         = init_flag("fast_pred_elim",         FALSE);

  // PARMS:
  //  internal name               external name      default    min      max

  p->max_given =        init_parm("max_given",            -1,     -1,INT_MAX);
  p->max_kept =         init_parm("max_kept",             -1,     -1,INT_MAX);
  p->max_proofs =       init_parm("max_proofs",            1,     -1,INT_MAX);
#ifdef __EMSCRIPTEN__
  p->max_megs =         init_parm("max_megs",           2048,     -1,   4096);
#else
  p->max_megs =         init_parm("max_megs",          49152,     -1,INT_MAX);
#endif
  p->cnf_clause_limit = init_parm("cnf_clause_limit",      0,      0,INT_MAX);
  p->definitional_cnf = init_parm("definitional_cnf",      0,      0,INT_MAX);
  p->max_seconds =      init_parm("max_seconds",          -1,     -1,INT_MAX);
  p->max_minutes =      init_parm("max_minutes",          -1,     -1,INT_MAX);
  p->max_hours =        init_parm("max_hours",          -1,     -1,INT_MAX);
  p->max_days =         init_parm("max_days",          -1,     -1,INT_MAX);

  p->new_constants =    init_parm("new_constants",         0,     -1,INT_MAX);
  p->para_lit_limit =   init_parm("para_lit_limit",       -1,     -1,INT_MAX);
  p->ur_nucleus_limit = init_parm("ur_nucleus_limit",     -1,     -1,INT_MAX);
  p->collective_given_ratio =
    init_parm("collective_given_ratio", 1, 1, 1000);
  p->collective_candidate_chunk =
    init_parm("collective_candidate_chunk", 64, 1, INT_MAX);
  p->collective_candidate_cache =
    init_parm("collective_candidate_cache", 4096, 1, INT_MAX);
  p->collective_raw_work_budget =
    init_parm("collective_raw_work_budget", 64, 1, INT_MAX);
  p->collective_promising_fair_interval =
    init_parm("collective_promising_fair_interval", 8, 1, 1000);
  p->collective_descriptor_high_water =
    init_parm("collective_descriptor_high_water", 256, 4, INT_MAX);
  p->collective_descriptor_low_water =
    init_parm("collective_descriptor_low_water", 192, 0, INT_MAX);
  p->collective_oldest_lag_limit =
    init_parm("collective_oldest_lag_limit", 256, 1, INT_MAX);
  p->collective_balanced_fair_interval =
    init_parm("collective_balanced_fair_interval", 8, 1, 1000);
  p->collective_paramod_share =
    init_parm("collective_paramod_share", 2, 0, 1000);
  p->collective_pos_hyper_share =
    init_parm("collective_pos_hyper_share", 1, 0, 1000);
  p->collective_neg_hyper_share =
    init_parm("collective_neg_hyper_share", 1, 0, 1000);
  p->collective_candidate_window =
    init_parm("collective_candidate_window", 64, 1, INT_MAX);
  p->collective_candidate_commit_interval =
    init_parm("collective_candidate_commit_interval", 4, 1, 1000);
  p->collective_candidate_fair_interval =
    init_parm("collective_candidate_fair_interval", 8, 1, 1000);
  p->collective_discovery_raw_budget =
    init_parm("collective_discovery_raw_budget", 32, 1, INT_MAX);
  p->collective_discovery_distance =
    init_parm("collective_discovery_distance", 256, 1, INT_MAX);
  p->collective_discovery_promotion_cap =
    init_parm("collective_discovery_promotion_cap", 32, 1, INT_MAX);
  p->collective_discovery_general_interval =
    init_parm("collective_discovery_general_interval", 8, 1, 1000);
  p->collective_discovery_turn_interval =
    init_parm("collective_discovery_turn_interval", 2, 1, 1000);

  p->fold_denial_max =  init_parm("fold_denial_max",       0,     -1,INT_MAX);

  p->pick_given_ratio  = init_parm("pick_given_ratio",    -1,     -1,INT_MAX);
  p->hints_part        = init_parm("hints_part",     INT_MAX,      0,INT_MAX);
  p->age_part          = init_parm("age_part",             1,      0,INT_MAX);
  p->weight_part       = init_parm("weight_part",          0,      0,INT_MAX);
  p->false_part        = init_parm("false_part",           4,      0,INT_MAX);
  p->true_part         = init_parm("true_part",            4,      0,INT_MAX);
  p->random_part       = init_parm("random_part",          0,      0,INT_MAX);
  p->random_seed       = init_parm("random_seed",          0,     -1,INT_MAX);
  p->eval_limit        = init_parm("eval_limit",        1024,     -1,INT_MAX);
  p->eval_var_limit    = init_parm("eval_var_limit",      -1,     -1,INT_MAX);

  p->max_depth =        init_parm("max_depth",            -1,     -1,INT_MAX);
  p->lex_dep_demod_lim =init_parm("lex_dep_demod_lim",    11,     -1,INT_MAX);
  p->max_literals =     init_parm("max_literals",         -1,     -1,INT_MAX);
  p->max_vars =         init_parm("max_vars",             -1,     -1,INT_MAX);
  p->demod_step_limit = init_parm("demod_step_limit",   1000,     -1,INT_MAX);
  p->demod_increase_limit = init_parm("demod_increase_limit",1000,-1,INT_MAX);
  p->max_nohints  = init_parm("max_nohints",   -1, -1, INT_MAX);
  p->degrade_limit = init_parm("degrade_limit",  0, -1, INT_MAX);
  p->para_restr_beg = init_parm("para_restr_beg", INT_MAX, -1, INT_MAX);
  p->para_restr_end = init_parm("para_restr_end",      -1, -1, INT_MAX);
  p->backsub_check    = init_parm("backsub_check",       500,     -1,INT_MAX);

  p->variable_weight =  init_floatparm("variable_weight",       1.0,-DBL_LARGE,DBL_LARGE);
  p->constant_weight =  init_floatparm("constant_weight",       1.0,-DBL_LARGE,DBL_LARGE);
  p->not_weight =       init_floatparm("not_weight",            0.0,-DBL_LARGE,DBL_LARGE);
  p->or_weight =        init_floatparm("or_weight",             0.0,-DBL_LARGE,DBL_LARGE);
  p->sk_constant_weight=init_floatparm("sk_constant_weight",    1.0,-DBL_LARGE,DBL_LARGE);
  p->prop_atom_weight = init_floatparm("prop_atom_weight",      1.0,-DBL_LARGE,DBL_LARGE);
  p->nest_penalty =     init_floatparm("nest_penalty",          0.0,     0.0,DBL_LARGE);
  p->depth_penalty =    init_floatparm("depth_penalty",         0.0,-DBL_LARGE,DBL_LARGE);
  p->var_penalty =      init_floatparm("var_penalty",           0.0,-DBL_LARGE,DBL_LARGE);
  p->default_weight =   init_floatparm("default_weight",  DBL_LARGE,-DBL_LARGE,DBL_LARGE);
  p->complexity =       init_floatparm("complexity",            0.0,-DBL_LARGE,DBL_LARGE);
  p->sine_weight =      init_parm("sine_weight",               0,      0,INT_MAX);

  p->sos_limit =        init_parm("sos_limit",         20000,     -1,INT_MAX);
  p->sos_keep_factor =  init_parm("sos_keep_factor",       3,      2,10);
  p->min_sos_limit =    init_parm("min_sos_limit",         0,      0,INT_MAX);
  p->lrs_interval =     init_parm("lrs_interval",         50,      1,INT_MAX);
  p->lrs_ticks =        init_parm("lrs_ticks",            -1,     -1,INT_MAX);

  p->report =           init_parm("report",               -1,     -1,INT_MAX);
  p->report_stderr =    init_parm("report_stderr",        -1,     -1,INT_MAX);
  p->report_given =     init_parm("report_given",         -1,     -1,INT_MAX);
  p->report_preprocessing = init_parm("report_preprocessing", -1, -1,INT_MAX);
  p->compact_passive_cache = init_parm("compact_passive_cache", 4, 0, 1024);
  p->compact_term_reclaim_kb =
    init_parm("compact_term_reclaim_kb", 8192, 1, INT_MAX);
  p->compact_index_stale_pct =
    init_parm("compact_index_stale_pct", 25, 1, 1000);
  p->fpa_depth =        init_parm("fpa_depth",            10,      1,    100);
  p->candidate_warn_limit = init_parm("candidate_warn_limit", -1,   -1,INT_MAX);
  p->candidate_hard_limit = init_parm("candidate_hard_limit", -1,   -1,INT_MAX);
  p->checkpoint_minutes = init_parm("checkpoint_minutes",    -1,     -1,INT_MAX);
  p->checkpoint_given =   init_parm("checkpoint_given",      -1,     -1,INT_MAX);
  p->checkpoint_candidate_pool =
    init_parm("checkpoint_candidate_pool", -1, -1, INT_MAX);
  p->checkpoint_discovery_promotions =
    init_parm("checkpoint_discovery_promotions", -1, -1, INT_MAX);
  p->checkpoint_keep =    init_parm("checkpoint_keep",        3,      1,INT_MAX);
  p->sine =               init_parm("sine",                  -1,     -1,INT_MAX);
  p->sine_depth =         init_parm("sine_depth",             0,      0,INT_MAX);
  p->sine_max_axioms =    init_parm("sine_max_axioms",        0,      0,INT_MAX);
  p->cl_to_trace =        init_parm("cl_to_trace",            0,      0,INT_MAX);
  p->hint_derivations =   init_parm("hint_derivations",       0,      0,INT_MAX);
  p->cores =              init_parm("cores",                  0,      0,     64);
  p->hint_expiry =        init_parm("hint_expiry",           -1,     -1,INT_MAX);
  p->hint_sweep_interval = init_parm("hint_sweep_interval", 1000,     1,INT_MAX);
  p->hint_expiry_min =    init_parm("hint_expiry_min",       1,      1,INT_MAX);
  p->hints_fpa_depth =    init_parm("hints_fpa_depth",      10,      1,    100);
  p->rewrite_refresh_hot_ratio =
    init_parm("rewrite_refresh_hot_ratio", 7, 0, INT_MAX);
  p->rewrite_refresh_raw_budget =
    init_parm("rewrite_refresh_raw_budget", 64, 1, INT_MAX);
  p->rewrite_refresh_inference_ratio =
    init_parm("rewrite_refresh_inference_ratio", 8, 1, INT_MAX);
  p->rewrite_refresh_high_water =
    init_parm("rewrite_refresh_high_water", 4096, 1, INT_MAX);
  p->rewrite_refresh_low_water =
    init_parm("rewrite_refresh_low_water", 3072, 0, INT_MAX);
  p->rewrite_refresh_drain_burst =
    init_parm("rewrite_refresh_drain_burst", 64, 1, INT_MAX);
  p->fpa_hash_threshold = init_parm("fpa_hash_threshold",   4,      0,   1000);
  p->discrim_hash_threshold = init_parm("discrim_hash_threshold", -1,  -1,  1000);

  // FLOATPARMS:
  //  internal name      external name           default    min      max )

  p->max_weight =   init_floatparm("max_weight",  100.0, -DBL_LARGE, DBL_LARGE);

  // STRINGPARMS:
  // (internal-name, external-name, number-of-strings, str1, str2, ... )
  // str1 is always the default

  p->order = init_stringparm("order", 3,
			     "lpo",
			     "rpo",
			     "kbo");

  p->eq_defs = init_stringparm("eq_defs", 3,
			       "unfold",
			       "fold",
			       "pass");

  p->literal_selection = init_stringparm("literal_selection", 3,
					 "max_negative",
					 "all_negative",
					 "none");

  p->stats = init_stringparm("stats", 4,
			     "lots",
			     "some",
			     "all",
			     "none");

  p->multiple_interps = init_stringparm("multiple_interps", 2,
					"false_in_all",
					"false_in_some");

  p->search_loop = init_stringparm("search_loop", 2,
				   "otter",
				   "discount");

  p->passive_store = init_stringparm("passive_store", 3,
				     "full",
				     "compressed",
				     "dense");

  p->compact_unit_strategy =
    init_stringparm("compact_unit_strategy", 2,
                    "root_scan",
                    "position");

  p->discount_demodulation =
    init_stringparm("discount_demodulation", 3,
			"selected",
			"eager_legacy",
			"eager_interreduced");

  p->hint_index = init_stringparm("hint_index", 7,
				  "fpa",
				  "compact",
				  "shallow",
				  "packed",
				  "packed_fast",
				  "hybrid",
				  "packed_legacy");

  p->inference_frontier = init_stringparm("inference_frontier", 2,
					  "clauses",
					  "collective");

  p->collective_scheduler = init_stringparm("collective_scheduler", 2,
					    "legacy",
					    "balanced_hint");

  p->ancestor_store = init_stringparm("ancestor_store", 4,
			      "off",
			      "memory",
			      "mmap",
			      "file");

  /* Keep the historical coupling by default.  `file` is a genuinely cold
     passive-body backend; it does not change the proof ancestor backend. */
  p->passive_backing = init_stringparm("passive_backing", 4,
			       "auto",
			       "memory",
			       "mmap",
			       "file");

  // Flag and parm Dependencies.  These cause other flags and parms
  // to be changed.  The changes happen immediately and can be undone
  // by later settings in the input.
  // DEPENDENCIES ARE NOT APPLIED TO DEFAULT SETTINGS!!!

  parm_parm_dependency(p->max_minutes, p->max_seconds,         60, TRUE);
  parm_parm_dependency(p->max_hours,   p->max_seconds,       3600, TRUE);
  parm_parm_dependency(p->max_days,    p->max_seconds,      86400, TRUE);

  flag_parm_dependency(p->para_units_only,    TRUE,  p->para_lit_limit,      1);
  flag_parm_dep_default(p->para_units_only,   FALSE, p->para_lit_limit);

  flag_flag_dependency(p->hyper_resolution, TRUE,  p->pos_hyper_resolution, TRUE);
  flag_flag_dependency(p->hyper_resolution, FALSE, p->pos_hyper_resolution, FALSE);

  flag_flag_dependency(p->ur_resolution, TRUE,  p->pos_ur_resolution, TRUE);
  flag_flag_dependency(p->ur_resolution, TRUE,  p->neg_ur_resolution, TRUE);
  flag_flag_dependency(p->ur_resolution, FALSE,  p->pos_ur_resolution, FALSE);
  flag_flag_dependency(p->ur_resolution, FALSE,  p->neg_ur_resolution, FALSE);

  flag_parm_dependency(p->lex_dep_demod, FALSE, p->lex_dep_demod_lim, 0);
  flag_parm_dependency(p->lex_dep_demod,  TRUE, p->lex_dep_demod_lim, 11);

  /***********************/

  parm_parm_dependency(p->pick_given_ratio, p->age_part,          1, FALSE);
  parm_parm_dependency(p->pick_given_ratio, p->weight_part,       1,  TRUE);
  parm_parm_dependency(p->pick_given_ratio, p->false_part,        0, FALSE);
  parm_parm_dependency(p->pick_given_ratio, p->true_part,         0, FALSE);
  parm_parm_dependency(p->pick_given_ratio, p->random_part,       0, FALSE);

  flag_parm_dependency(p->lightest_first,    TRUE,  p->weight_part,     1);
  flag_parm_dependency(p->lightest_first,    TRUE,  p->age_part,        0);
  flag_parm_dependency(p->lightest_first,    TRUE,  p->false_part,      0);
  flag_parm_dependency(p->lightest_first,    TRUE,  p->true_part,       0);
  flag_parm_dependency(p->lightest_first,    TRUE,  p->random_part,     0);
  flag_flag_dependency(p->lightest_first,   FALSE,  p->default_parts, TRUE);

  flag_parm_dependency(p->random_given,    TRUE,  p->weight_part,     0);
  flag_parm_dependency(p->random_given,    TRUE,  p->age_part,        0);
  flag_parm_dependency(p->random_given,    TRUE,  p->false_part,      0);
  flag_parm_dependency(p->random_given,    TRUE,  p->true_part,       0);
  flag_parm_dependency(p->random_given,    TRUE,  p->random_part,     1);
  flag_flag_dependency(p->random_given,   FALSE,  p->default_parts, TRUE);

  flag_parm_dependency(p->breadth_first,    TRUE,  p->age_part,        1);
  flag_parm_dependency(p->breadth_first,    TRUE,  p->weight_part,     0);
  flag_parm_dependency(p->breadth_first,    TRUE,  p->false_part,      0);
  flag_parm_dependency(p->breadth_first,    TRUE,  p->true_part,       0);
  flag_parm_dependency(p->breadth_first,    TRUE,  p->random_part,     0);
  flag_flag_dependency(p->breadth_first,    FALSE, p->default_parts, TRUE);

  /* flag_parm_dependency(p->default_parts, TRUE,  p->hints_part, INT_MAX); */
  flag_parm_dependency(p->default_parts,    TRUE,  p->age_part,          1);
  flag_parm_dependency(p->default_parts,    TRUE,  p->weight_part,       0);
  flag_parm_dependency(p->default_parts,    TRUE,  p->false_part,        4);
  flag_parm_dependency(p->default_parts,    TRUE,  p->true_part,         4);
  flag_parm_dependency(p->default_parts,    TRUE,  p->random_part,       0);

  /* flag_parm_dependency(p->default_parts,    FALSE,  p->hints_part,  0); */
  flag_parm_dependency(p->default_parts,    FALSE,  p->age_part,         0);
  flag_parm_dependency(p->default_parts,    FALSE,  p->weight_part,      0);
  flag_parm_dependency(p->default_parts,    FALSE,  p->false_part,       0);
  flag_parm_dependency(p->default_parts,    FALSE,  p->true_part,        0);
  flag_parm_dependency(p->default_parts,    FALSE,  p->random_part,      0);

  /***********************/

  flag_flag_dependency(p->default_output, TRUE, p->quiet,               FALSE);
  flag_flag_dependency(p->default_output, TRUE, p->echo_input,           TRUE);
  flag_flag_dependency(p->default_output, TRUE, p->print_initial_clauses,TRUE);
  flag_flag_dependency(p->default_output, TRUE, p->print_given,          TRUE);
  flag_flag_dependency(p->default_output, TRUE, p->print_proofs,         TRUE);
  flag_stringparm_dependency(p->default_output, TRUE, p->stats,        "lots");

  flag_flag_dependency(p->default_output, TRUE, p->print_kept,          FALSE);
  flag_flag_dependency(p->default_output, TRUE, p->print_gen,           FALSE);
  flag_flag_dependency(p->default_output, TRUE, p->print_matched_hints, FALSE);

  // auto_setup

  flag_flag_dependency(p->auto_setup,  TRUE, p->predicate_elim,    TRUE);
  flag_stringparm_dependency(p->auto_setup, TRUE, p->eq_defs,    "unfold");

  flag_flag_dependency(p->auto_setup,  FALSE, p->predicate_elim,    FALSE);
  flag_stringparm_dependency(p->auto_setup, FALSE, p->eq_defs,   "pass");

  // auto_limits

  flag_floatparm_dependency(p->auto_limits,  TRUE, p->max_weight,    100.0);
  flag_parm_dependency(p->auto_limits,       TRUE, p->sos_limit,        -1);

  flag_floatparm_dependency(p->auto_limits, FALSE, p->max_weight, DBL_LARGE);
  flag_parm_dependency(p->auto_limits,      FALSE, p->sos_limit,         -1);

  // automatic

  flag_flag_dependency(p->automatic,       TRUE, p->auto_inference,     TRUE);
  flag_flag_dependency(p->automatic,       TRUE, p->auto_setup,         TRUE);
  flag_flag_dependency(p->automatic,       TRUE, p->auto_limits,        TRUE);
  flag_flag_dependency(p->automatic,       TRUE, p->auto_denials,       TRUE);
  flag_flag_dependency(p->automatic,       TRUE, p->auto_process,       TRUE);

  flag_flag_dependency(p->automatic,       FALSE, p->auto_inference,    FALSE);
  flag_flag_dependency(p->automatic,       FALSE, p->auto_setup,        FALSE);
  flag_flag_dependency(p->automatic,       FALSE, p->auto_limits,       FALSE);
  flag_flag_dependency(p->automatic,       FALSE, p->auto_denials,      FALSE);
  flag_flag_dependency(p->automatic,       FALSE, p->auto_process,      FALSE);

  // auto2  (also triggered by -x on the command line)

  flag_flag_dependency(p->auto2, TRUE, p->automatic,                 TRUE);
  flag_parm_dependency(p->auto2, TRUE, p->new_constants,            1);
  flag_parm_dependency(p->auto2, TRUE, p->fold_denial_max,          3);
  flag_floatparm_dependency(p->auto2, TRUE, p->max_weight,      200.0);
  flag_parm_dependency(p->auto2, TRUE, p->nest_penalty,             1);
  flag_parm_dependency(p->auto2, TRUE, p->sk_constant_weight,       0);
  flag_parm_dependency(p->auto2, TRUE, p->prop_atom_weight,         5);
  flag_flag_dependency(p->auto2, TRUE, p->sort_initial_sos,       TRUE);
  flag_parm_dependency(p->auto2, TRUE, p->sos_limit,                -1);
  flag_parm_dependency(p->auto2, TRUE, p->lrs_ticks,              3000);
  flag_parm_dependency(p->auto2, TRUE, p->max_megs,                400);
  flag_stringparm_dependency(p->auto2, TRUE, p->stats,          "some");
  flag_flag_dependency(p->auto2, TRUE, p->echo_input,            FALSE);
  flag_flag_dependency(p->auto2, TRUE, p->quiet,                  TRUE);
  flag_flag_dependency(p->auto2, TRUE, p->print_initial_clauses, FALSE);
  flag_flag_dependency(p->auto2, TRUE, p->print_given,           FALSE);

  flag_flag_dep_default(p->auto2, FALSE, p->automatic);
  flag_parm_dep_default(p->auto2, FALSE, p->new_constants);
  flag_parm_dep_default(p->auto2, FALSE, p->fold_denial_max);
  flag_floatparm_dep_default(p->auto2, FALSE, p->max_weight);
  flag_parm_dep_default(p->auto2, FALSE, p->nest_penalty);
  flag_parm_dep_default(p->auto2, FALSE, p->sk_constant_weight);
  flag_parm_dep_default(p->auto2, FALSE, p->prop_atom_weight);
  flag_flag_dep_default(p->auto2, FALSE, p->sort_initial_sos);
  flag_parm_dep_default(p->auto2, FALSE, p->sos_limit);
  flag_parm_dep_default(p->auto2, FALSE, p->lrs_ticks);
  flag_parm_dep_default(p->auto2, FALSE, p->max_megs);
  flag_stringparm_dep_default(p->auto2, FALSE, p->stats);
  flag_flag_dep_default(p->auto2, FALSE, p->echo_input);
  flag_flag_dep_default(p->auto2, FALSE, p->quiet);
  flag_flag_dep_default(p->auto2, FALSE, p->print_initial_clauses);
  flag_flag_dep_default(p->auto2, FALSE, p->print_given);

  // tptp_output - suppress native output, TSTP proof is printed separately.

  flag_flag_dependency(p->tptp_output, TRUE, p->default_output,        FALSE);
  flag_flag_dependency(p->tptp_output, TRUE, p->quiet,                  TRUE);
  flag_flag_dependency(p->tptp_output, TRUE, p->echo_input,            FALSE);
  flag_flag_dependency(p->tptp_output, TRUE, p->print_initial_clauses, FALSE);
  flag_flag_dependency(p->tptp_output, TRUE, p->print_given,           FALSE);
  flag_flag_dependency(p->tptp_output, TRUE, p->bell,                  FALSE);
  flag_stringparm_dependency(p->tptp_output, TRUE, p->stats,         "none");

  flag_flag_dep_default(p->tptp_output, FALSE, p->quiet);
  flag_flag_dep_default(p->tptp_output, FALSE, p->echo_input);
  flag_flag_dep_default(p->tptp_output, FALSE, p->print_initial_clauses);
  flag_flag_dep_default(p->tptp_output, FALSE, p->print_given);
  flag_flag_dep_default(p->tptp_output, FALSE, p->bell);
  flag_stringparm_dep_default(p->tptp_output, FALSE, p->stats);

  // raw

  flag_flag_dependency(p->raw, TRUE, p->automatic,           FALSE);
  flag_flag_dependency(p->raw, TRUE, p->ordered_res,         FALSE);
  flag_flag_dependency(p->raw, TRUE, p->ordered_para,        FALSE);
  flag_flag_dependency(p->raw, TRUE, p->para_into_vars,      TRUE);
  flag_flag_dependency(p->raw, TRUE, p->para_from_small,     TRUE);
  flag_flag_dependency(p->raw, TRUE, p->ordered_para,        FALSE);
  flag_flag_dependency(p->raw, TRUE, p->back_demod,          FALSE);
  flag_flag_dependency(p->raw, TRUE, p->cac_redundancy,      FALSE);
  flag_parm_dependency(p->raw, TRUE, p->backsub_check,     INT_MAX);
  flag_flag_dependency(p->raw, TRUE, p->lightest_first,       TRUE);
  flag_stringparm_dependency(p->raw, TRUE, p->literal_selection, "none");
  
  flag_flag_dep_default(p->raw, FALSE, p->automatic);
  flag_flag_dep_default(p->raw, FALSE, p->ordered_res);
  flag_flag_dep_default(p->raw, FALSE, p->ordered_para);
  flag_flag_dep_default(p->raw, FALSE, p->para_into_vars);
  flag_flag_dep_default(p->raw, FALSE, p->para_from_small);
  flag_flag_dep_default(p->raw, FALSE, p->ordered_para);
  flag_flag_dep_default(p->raw, FALSE, p->back_demod);
  flag_flag_dep_default(p->raw, FALSE, p->cac_redundancy);
  flag_parm_dep_default(p->raw, FALSE, p->backsub_check);
  flag_flag_dep_default(p->raw, FALSE, p->lightest_first);
  flag_stringparm_dep_default(p->raw, FALSE, p->literal_selection);

  // production mode

  flag_flag_dependency(p->production,   TRUE,  p->raw,               TRUE);
  flag_flag_dependency(p->production,   TRUE,  p->eval_rewrite,      TRUE);
  flag_flag_dependency(p->production,   TRUE,  p->hyper_resolution,  TRUE);
  flag_flag_dependency(p->production,   TRUE,  p->back_subsume,     FALSE);
  
  return p;
  
}  // init_prover_options

/*************
 *
 *   init_prover_attributes()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void init_prover_attributes(void)
{
  Att.label            = register_attribute("label",        STRING_ATTRIBUTE);
  Att.bsub_hint_wt     = register_attribute("bsub_hint_wt",    INT_ATTRIBUTE);
  Att.answer           = register_attribute("answer",         TERM_ATTRIBUTE);
  Att.properties       = register_attribute("props",          TERM_ATTRIBUTE);

  Att.action           = register_attribute("action",         TERM_ATTRIBUTE);
  Att.action2          = register_attribute("action2",        TERM_ATTRIBUTE);

  declare_term_attribute_inheritable(Att.answer);
  declare_term_attribute_inheritable(Att.action2);
}  // Init_prover_attributes

/*************
 *
 *   get_attrib_id()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
int get_attrib_id(char *str)
{
  if (str_ident(str, "label"))
    return Att.label;
  else if (str_ident(str, "bsub_hint_wt"))
    return Att.bsub_hint_wt;
  else if (str_ident(str, "answer"))
    return Att.answer;
  else if (str_ident(str, "action"))
    return Att.action;
  else if (str_ident(str, "action2"))
    return Att.action2;
  else {
    fatal_error("get_attrib_id, unknown attribute string");
    return -1;
  }
}  /* get_attrib_id */

/*************
 *
 *   update_stats()
 *
 *************/

static
unsigned long long clist_body_bytes(Clist list)
{
  unsigned long long bytes = 0;
  Clist_pos p;
  if (list == NULL)
    return 0;
  for (p = list->first; p != NULL; p = p->next) {
    Topform c = p->c;
    if (c->compressed != NULL)
      bytes += c->compressed_size -
               compressed_clause_justification_bytes(c);
    else
      bytes += clause_body_storage_bytes(c);
  }
  return bytes;
}  /* clist_body_bytes */

static
unsigned long long clist_estimated_body_bytes(Clist list)
{
  unsigned long long bytes = 0;
  Clist_pos p;
  if (list == NULL)
    return 0;
  for (p = list->first; p != NULL; p = p->next) {
    Topform c = p->c;
    bytes += c->compressed != NULL ? c->uncompressed_body_bytes :
                                      clause_body_storage_bytes(c);
  }
  return bytes;
}  /* clist_estimated_body_bytes */

static
unsigned long long clist_compressed_count(Clist list)
{
  unsigned long long count = 0;
  Clist_pos p;
  if (list != NULL)
    for (p = list->first; p != NULL; p = p->next)
      if (p->c->compressed != NULL)
        count++;
  return count;
}  /* clist_compressed_count */

static
unsigned long long clist_compressed_justification_bytes(Clist list)
{
  unsigned long long bytes = 0;
  Clist_pos p;
  if (list != NULL)
    for (p = list->first; p != NULL; p = p->next)
      bytes += compressed_clause_justification_bytes(p->c);
  return bytes;
}  /* clist_compressed_justification_bytes */

static
unsigned long long delayed_demodulator_count(void)
{
  unsigned long long count = 0;
  Clist_pos p;

  if (!discount_mode() || !flag(Opt->back_demod) || Glob.sos == NULL)
    return 0;

  if (dense_passive_mode())
    return dense_passive_delayed_demodulators();

  for (p = Glob.sos->first; p != NULL; p = p->next) {
    if (p->c->delayed_demodulator)
      count++;
  }
  return count;
}  /* delayed_demodulator_count */

static
void update_memory_stats(void)
{
  struct clause_compression_stats cs = clause_compression_get_stats();
  struct clause_id_table_stats ids = clause_id_table_get_stats();
  struct clause_store_stats as = clause_store_get_stats(Glob.disabled);
  struct cold_passive_store_stats ps =
    cold_passive_store_get_stats(Dense_body_store);
  struct memory_stats ms;
  struct memory_process_stats process;
  memory_get_stats(&ms);
  memory_get_process_stats(&process);
  Clist_pos p;
  size_t i;

  Stats.compression_attempted = cs.attempted;
  Stats.compression_successful = cs.successful;
  Stats.compression_skipped = cs.skipped;
  Stats.compression_materialized = cs.materialized;
  Stats.compression_recompressed = cs.recompressed;

  Stats.passive_body_bytes = clist_body_bytes(Glob.sos);
  Stats.passive_justification_bytes =
    clist_compressed_justification_bytes(Glob.sos);
  Stats.passive_estimated_full_body_bytes =
    clist_estimated_body_bytes(Glob.sos);
  Stats.passive_compressed_clauses = clist_compressed_count(Glob.sos);
  dense_passive_memory(&Stats.dense_passive_record_bytes,
                       &Stats.dense_passive_heap_bytes,
                       &Stats.dense_passive_records);
  Stats.dense_passive_arena_records = ps.records;
  Stats.dense_passive_arena_record_bytes = ps.record_bytes;
  Stats.dense_passive_arena_backing_bytes = ps.backing_bytes;
  Stats.dense_passive_arena_physical_bytes = ps.physical_bytes;
  Stats.dense_passive_arena_materializations = ps.materializations;
  Stats.dense_passive_arena_validation_failures = ps.validation_failures;
  Stats.dense_passive_arena_file_reads = ps.file_reads;
  Stats.dense_passive_arena_file_read_bytes = ps.file_read_bytes;
  Stats.dense_passive_arena_file_writes = ps.file_writes;
  Stats.dense_passive_arena_file_write_bytes = ps.file_write_bytes;
  dense_passive_compaction_stats(&Stats.dense_passive_compactions,
                                 &Stats.dense_passive_records_reclaimed);
  Stats.dense_passive_arena_bytes_reclaimed =
    Dense_arena_bytes_reclaimed;
  if (dense_passive_mode()) {
    dense_passive_payload_memory(&Stats.passive_body_bytes,
                                 &Stats.passive_justification_bytes,
                                 &Stats.passive_estimated_full_body_bytes);
    Stats.passive_compressed_clauses = Stats.dense_passive_records;
  }
  if (discount_mode()) {
    Stats.active_body_bytes = clist_body_bytes(Glob.usable);
    if (Glob.limbo != NULL) {
      for (p = Glob.limbo->first; p != NULL; p = p->next) {
        if (p->c->was_given)
          Stats.active_body_bytes += clause_body_storage_bytes(p->c);
        else
          Stats.passive_body_bytes += clause_body_storage_bytes(p->c);
        if (!p->c->was_given) {
          Stats.passive_estimated_full_body_bytes +=
            p->c->compressed != NULL ? p->c->uncompressed_body_bytes :
                                       clause_body_storage_bytes(p->c);
          if (p->c->compressed != NULL)
            Stats.passive_compressed_clauses++;
        }
      }
    }
  }
  else {
    /* Preserve the historical meaning of active_body_bytes in the Otter
       loop: every materialized search clause, including indexed SOS. */
    Stats.active_body_bytes = clist_body_bytes(Glob.usable) +
                              clist_body_bytes(Glob.sos) +
                              clist_body_bytes(Glob.limbo);
  }
  Stats.passive_total_payload_bytes = Stats.passive_body_bytes +
                                      Stats.passive_justification_bytes;
  if (Glob.demods != NULL) {
    for (p = Glob.demods->first; p != NULL; p = p->next) {
      Topform c = p->c;
      if ((Glob.usable == NULL || !clist_member(c, Glob.usable)) &&
          (Glob.sos == NULL || !clist_member(c, Glob.sos)) &&
          (Glob.limbo == NULL || !clist_member(c, Glob.limbo)))
        Stats.active_body_bytes += clause_body_storage_bytes(c);
    }
  }
  Stats.hint_body_bytes = clist_body_bytes(Glob.hints);
  Stats.hint_estimated_full_body_bytes =
    clist_estimated_body_bytes(Glob.hints);
  Stats.hint_compressed_clauses = clist_compressed_count(Glob.hints);
  packed_hint_index_stats(&Stats.hint_index_node_bytes,
                          &Stats.hint_index_reference_bytes,
                          &Stats.hint_index_table_bytes,
                          &Stats.hint_candidate_checks);

  Stats.disabled_full_body_bytes = 0;
  Stats.disabled_compressed_bytes = 0;
  Stats.disabled_estimated_uncompressed_bytes = 0;
  Stats.disabled_full_clauses = 0;
  Stats.disabled_compressed_clauses = 0;
  if (Glob.disabled != NULL) {
    for (i = 0; i < clause_store_length(Glob.disabled); i++) {
      Topform c;
      if (clause_store_position_is_archived(Glob.disabled, i))
        continue;
      c = clause_store_get(Glob.disabled, i);
      if (c->compressed != NULL) {
        Stats.disabled_compressed_clauses++;
        Stats.disabled_compressed_bytes += c->compressed_size;
        Stats.disabled_estimated_uncompressed_bytes +=
          c->uncompressed_body_bytes;
      }
      else {
        unsigned long long bytes = clause_body_storage_bytes(c);
        Stats.disabled_full_clauses++;
        Stats.disabled_full_body_bytes += bytes;
        Stats.disabled_estimated_uncompressed_bytes += bytes;
      }
    }
  }
  Stats.disabled_compressed_clauses += as.records;
  Stats.disabled_compressed_bytes += as.body_bytes;
  Stats.disabled_estimated_uncompressed_bytes += as.logical_body_bytes;
  Stats.ancestor_records = as.records;
  Stats.ancestor_record_bytes = as.record_bytes;
  Stats.ancestor_backing_bytes = as.backing_bytes;
  Stats.ancestor_handle_bytes = as.handle_bytes;
  Stats.ancestor_materializations = as.materializations;
  Stats.ancestor_validation_failures = as.validation_failures;
  Stats.ancestor_mmap_eviction_passes = as.mmap_eviction_passes;
  Stats.ancestor_mmap_eviction_bytes = as.mmap_eviction_bytes;
  Stats.ancestor_mmap_scan_eviction_passes = as.mmap_scan_eviction_passes;
  Stats.ancestor_mmap_scan_eviction_bytes = as.mmap_scan_eviction_bytes;
  Stats.ancestor_io_buffer_bytes = as.io_buffer_bytes;
  Stats.ancestor_file_reads = as.file_reads;
  Stats.ancestor_file_read_bytes = as.file_read_bytes;
  Stats.ancestor_file_writes = as.file_writes;
  Stats.ancestor_file_write_bytes = as.file_write_bytes;

  Stats.disabled_store_bytes = clause_store_allocated_bytes(Glob.disabled);
  Stats.disabled_legacy_clist_bytes =
    clause_store_legacy_clist_bytes(Glob.disabled);
  Stats.clause_id_entries = ids.entries;
  Stats.clause_id_pages = ids.pages;
  Stats.clause_id_table_capacity = ids.table_capacity;
  Stats.clause_id_table_bytes = ids.allocated_bytes;
  Stats.clause_id_legacy_bytes = ids.legacy_bytes;

  Stats.allocator_reserved_kbytes = ms.reserved_bytes / 1024;
  Stats.allocator_logical_live_bytes = ms.logical_live_bytes;
  Stats.allocator_logical_peak_bytes = ms.logical_peak_bytes;
  Stats.allocator_reserved_bytes = ms.reserved_bytes;
  Stats.allocator_peak_reserved_bytes = ms.peak_reserved_bytes;
  Stats.allocator_reusable_bytes = ms.reusable_bytes;
  Stats.allocator_unallocated_bytes = ms.unallocated_bytes;
  Stats.allocator_metadata_bytes = ms.metadata_bytes;
  Stats.allocator_fragmentation_bytes = ms.fragmentation_bytes;
  Stats.allocator_direct_live_bytes = ms.direct_live_bytes;
  Stats.allocator_permanent_live_bytes = ms.permanent_live_bytes;
  Stats.allocator_slab_count = ms.slab_count;
  Stats.allocator_peak_slab_count = ms.peak_slab_count;
  Stats.allocator_reclaimed_slabs = ms.reclaimed_slabs;
  Stats.allocator_reclaimed_bytes = ms.reclaimed_bytes;
  Stats.process_smaps_supported = process.smaps_supported;
  Stats.process_libc_heap_supported = process.libc_heap_supported;
  Stats.process_compact_heap_enabled = memory_compact_system_heap_enabled();
  Stats.process_pss_kbytes = process.pss_kbytes;
  Stats.process_anonymous_kbytes = process.anonymous_kbytes;
  Stats.process_shared_clean_kbytes = process.shared_clean_kbytes;
  Stats.process_shared_dirty_kbytes = process.shared_dirty_kbytes;
  Stats.process_private_clean_kbytes = process.private_clean_kbytes;
  Stats.process_private_dirty_kbytes = process.private_dirty_kbytes;
  Stats.process_swap_kbytes = process.swap_kbytes;
  Stats.libc_arena_bytes = process.libc_arena_bytes;
  Stats.libc_mmap_bytes = process.libc_mmap_bytes;
  Stats.libc_in_use_bytes = process.libc_in_use_bytes;
  Stats.libc_free_bytes = process.libc_free_bytes;
  Stats.libc_releasable_bytes = process.libc_releasable_bytes;
  Stats.current_rss_kbytes = memory_current_rss_kbytes();
  Stats.peak_rss_kbytes = memory_peak_rss_kbytes();
  Stats.allocator_cumulative_bytes = ms.cumulative_bytes;
  Stats.allocator_allocation_calls = memory_allocation_calls();
  Stats.palloc_cumulative_bytes = bytes_palloced();
  Stats.fpa_live_nodes = fpa_live_trie_nodes();
  Stats.fpa_peak_nodes = fpa_peak_trie_nodes();
  Stats.fpa_live_lists = fpalist_live_lists();
  Stats.fpa_peak_lists = fpalist_peak_lists();
}  /* update_memory_stats */

static
void update_stats(void)
{
  struct compact_rewrite_stats compact;
  unsigned long long rewrite_lag = 0;
  update_rewrite_only_stats();
  compact_rewrite_get_stats(Compact_rewrite_rules, &compact);
  Stats.demod_attempts = demod_attempts() + fdemod_attempts() +
                         compact.attempts;
  Stats.demod_rewrites = demod_rewrites() + fdemod_rewrites() +
                         compact.rewrites;
  Stats.res_instance_prunes = res_instance_prunes();
  Stats.para_instance_prunes = para_instance_prunes();
  Stats.basic_para_prunes = basic_paramodulation_prunes();
  Stats.sos_removed = 0; // control_sos_removed();
  Stats.nonunit_fsub = nonunit_fsub_tests();
  Stats.nonunit_bsub = nonunit_bsub_tests();
  Stats.usable_size = Glob.usable ? Glob.usable->length : 0;
  Stats.sos_size = dense_passive_mode() ? dense_passive_size() :
                   (Glob.sos ? Glob.sos->length : 0);
  Stats.demodulators_size = compact_otter_passive_mode() ?
    compact.rules_current : (Glob.demods ? Glob.demods->length : 0);
  Stats.limbo_size = Glob.limbo ? Glob.limbo->length : 0;
  Stats.disabled_size = clause_store_current_length(Glob.disabled);
  if (compact_otter_passive_mode()) {
    size_t passive = dense_passive_size();
    if (Stats.disabled_size < passive)
      fatal_error("update_stats: archived passive count exceeds archive size");
    Stats.disabled_size -= passive;
  }
  Stats.disabled_size += Disabled_checkpoint_omitted;
  Stats.hints_size = Glob.hints ? Glob.hints->length : 0;
  Stats.active_indexed_clauses = Stats.usable_size;
  Stats.passive_indexed_clauses = discount_mode() ? 0 : Stats.sos_size;
  Stats.delayed_demodulators = delayed_demodulator_count();
  Stats.rewrite_debt_current = eager_interreduced_demod_mode() ?
    dense_passive_rewrite_debt() : 0;
  if (Stats.rewrite_debt_current > Stats.rewrite_debt_peak)
    Stats.rewrite_debt_peak = Stats.rewrite_debt_current;
  Stats.rewrite_refresh_stale_current = eager_interreduced_demod_mode() ?
    dense_passive_stale_count(Rewrite_epoch, &rewrite_lag) : 0;
  if (rewrite_lag > Stats.rewrite_refresh_lag_max)
    Stats.rewrite_refresh_lag_max = rewrite_lag;
  if (Stats.rewrite_refresh_stale_current >
      Stats.rewrite_refresh_stale_peak)
    Stats.rewrite_refresh_stale_peak =
      Stats.rewrite_refresh_stale_current;
  Stats.collective_batches_pending = Collective_batch_count;
  collective_descriptor_stats(&Stats.collective_pending_paramod,
                              &Stats.collective_pending_pos_hyper,
                              &Stats.collective_pending_neg_hyper,
                              &Stats.collective_descriptor_lag_sum,
                              &Stats.collective_descriptor_lag_p50,
                              &Stats.collective_descriptor_lag_p95,
                              &Stats.collective_descriptor_lag_max);
  Stats.collective_activation_entries = Collective_activation_count;
  Stats.collective_deactivation_entries = Collective_deactivation_count;
  Stats.collective_candidate_cache_current =
    collective_candidate_occupancy();
  Stats.collective_candidate_pool_bytes =
    collective_candidate_pool_allocated_bytes();
  Stats.collective_distinct_hints_matched = matched_hints(Glob.hints);
  Stats.collective_descriptor_bytes = Collective_batch_count *
                                      sizeof(struct collective_batch) +
    Collective_priority_heap_capacity * sizeof(*Collective_priority_heap);
  Stats.collective_iterator_path_bytes = collective_iterator_path_bytes();
  Stats.collective_descriptor_bytes += Stats.collective_iterator_path_bytes;
  Stats.collective_hyper_iterator_choice_bytes =
    collective_hyper_iterator_choice_bytes();
  Stats.collective_descriptor_bytes +=
    Stats.collective_hyper_iterator_choice_bytes;
  Stats.collective_discovery_consumed_records =
    collective_consumed_records();
  Stats.collective_discovery_consumed_bytes = collective_consumed_bytes();
  Stats.collective_descriptor_bytes += Stats.collective_discovery_consumed_bytes;
  Stats.collective_history_bytes = collective_history_bytes();
  collective_history_clause_stats(&Stats.collective_history_clauses,
                                   &Stats.collective_history_indexed_clauses,
                                   &Stats.collective_history_shared_clauses,
                                   &Stats.collective_history_retained_clauses,
                                   &Stats.collective_history_clause_bytes);
  Stats.kbyte_usage = bytes_palloced() / 1000;
  update_memory_stats();
}  /* update_stats */

/*************
 *
 *   fprint_prover_stats()
 *
 *************/

static
void fprint_prover_stats(FILE *fp, struct prover_stats s, char *stats_level)
{
  fprintf(fp,"\nGiven=%s. Generated=%s. Kept=%s. proofs=%s.\n",
	  comma_num(s.given), comma_num(s.generated),
	  comma_num(s.kept), comma_num(s.proofs));
  fprintf(fp,
          "Generated_by_rule: binary=%s, hyper=%s, ur=%s, paramod=%s, "
          "other=%s.\n",
          comma_num(s.generated_binary), comma_num(s.generated_hyper),
          comma_num(s.generated_ur), comma_num(s.generated_paramod),
          comma_num(s.generated_other));
  fprintf(fp,"Usable=%s. Sos=%s. Demods=%s. Limbo=%s, "
	  "Disabled=%s. Hints=%s. Active_Hints=%s.\n",
	  comma_num(s.usable_size), comma_num(s.sos_size),
	  comma_num(s.demodulators_size), comma_num(s.limbo_size),
	  comma_num(s.disabled_size), comma_num(s.hints_size),
	  comma_num(active_hints()));
  fprintf(fp,
          "Search_loop: mode=%s, frontier=%s, active_indexed=%s, passive_indexed=%s, "
          "delayed_demodulators=%s.\n",
          discount_mode() ? "discount" : "otter",
          stringparm1(Opt->inference_frontier),
          comma_num(s.active_indexed_clauses),
          comma_num(s.passive_indexed_clauses),
          comma_num(s.delayed_demodulators));
  if (discount_mode()) {
    fprintf(fp,
            "Discount_demodulation: policy=%s, candidates=%s, oriented=%s, "
            "lex=%s, rewrite_only_admitted=%s, retired=%s, selected=%s, "
            "current=%s, peak=%s, bytes=%s, peak_bytes=%s.\n",
            stringparm1(Opt->discount_demodulation),
            comma_num(s.passive_demodulator_candidates),
            comma_num(s.passive_oriented_demodulator_candidates),
            comma_num(s.passive_lex_demodulator_candidates),
            comma_num(s.rewrite_only_demodulators_admitted),
            comma_num(s.rewrite_only_demodulators_retired),
            comma_num(s.rewrite_only_demodulators_selected),
            comma_num(s.rewrite_only_demodulators_current),
            comma_num(s.rewrite_only_demodulators_peak),
            comma_num(s.rewrite_bank_bytes),
            comma_num(s.rewrite_bank_peak_bytes));
    if (eager_interreduced_demod_mode()) {
      fprintf(fp,
              "Compact_rewrite: current=%s, peak=%s, retired=%s, "
              "physical=%s, compactions=%s, reclaimed=%s, attempts=%s, "
              "rewrites=%s, nodes=%s, postings=%s, occurrences=%s, "
              "rules=%s, terms=%s, "
              "hash=%s.\n",
              comma_num(s.compact_rewrite_rules_current),
              comma_num(s.compact_rewrite_rules_peak),
              comma_num(s.compact_rewrite_rules_retired),
              comma_num(s.compact_rewrite_rules_physical),
              comma_num(s.compact_rewrite_compactions),
              comma_num(s.compact_rewrite_bytes_reclaimed),
              comma_num(s.compact_rewrite_attempts),
              comma_num(s.compact_rewrite_rewrites),
              comma_num(s.compact_rewrite_node_bytes),
              comma_num(s.compact_rewrite_posting_bytes),
              comma_num(s.compact_rewrite_occurrence_bytes),
              comma_num(s.compact_rewrite_rule_bytes),
              comma_num(s.compact_rewrite_term_bytes),
              comma_num(s.compact_rewrite_hash_bytes));
      fprintf(fp,
              "Compact_rewrite_occurrence_stream: used=%s, bytes=%s.\n",
              comma_num(s.compact_rewrite_occurrence_stream_used),
              comma_num(s.compact_rewrite_occurrence_stream_bytes));
    }
    fprintf(fp,
            "Passive_refresh: epoch=%u, checks=%s, requeued=%s, "
            "subsumed=%s.\n",
            Simplifier_epoch,
            comma_num(s.passive_refresh_checks),
            comma_num(s.passive_refresh_requeued),
            comma_num(s.passive_refresh_subsumed));
    if (eager_interreduced_demod_mode()) {
      fprintf(fp,
              "Rewrite_refresh: epoch=%u, inference_ratio=%d, stale=%s, stale_peak=%s, "
              "lag_max=%s, scanned=%s, materialized=%s, rewritten=%s, "
              "unchanged=%s, subsumed=%s, hot_turns=%s, general_turns=%s.\n",
              Rewrite_epoch,
              parm(Opt->rewrite_refresh_inference_ratio),
              comma_num(s.rewrite_refresh_stale_current),
              comma_num(s.rewrite_refresh_stale_peak),
              comma_num(s.rewrite_refresh_lag_max),
              comma_num(s.rewrite_refresh_scanned),
              comma_num(s.rewrite_refresh_materialized),
              comma_num(s.rewrite_refresh_rewritten),
              comma_num(s.rewrite_refresh_unchanged),
              comma_num(s.rewrite_refresh_subsumed),
              comma_num(s.rewrite_refresh_hot_turns),
              comma_num(s.rewrite_refresh_general_turns));
      fprintf(fp,
              "Rewrite_interreduce: rule_turns=%s, rule_changed=%s, "
              "rule_unchanged=%s, rule_collapsed=%s, debt=%s, "
              "overlap_visits=%s, dirty_marks=%s, cascade_suppressed=%s, "
              "debt_peak=%s, drain=%d, "
              "drain_entries=%s, drain_exits=%s, drain_turns=%s, "
              "drain_burst=%d, drain_streak=%u, drain_yields=%s, "
              "inference_turns=%s.\n",
              comma_num(s.rewrite_interreduce_turns),
              comma_num(s.rewrite_interreduce_changed),
              comma_num(s.rewrite_interreduce_unchanged),
              comma_num(s.rewrite_interreduce_collapsed),
              comma_num(s.rewrite_debt_current),
              comma_num(s.rewrite_overlap_visits),
              comma_num(s.rewrite_overlap_dirty_marks),
              comma_num(s.rewrite_cascade_suppressed),
              comma_num(s.rewrite_debt_peak), Rewrite_drain_mode,
              comma_num(s.rewrite_drain_entries),
              comma_num(s.rewrite_drain_exits),
              comma_num(s.rewrite_drain_turns),
              parm(Opt->rewrite_refresh_drain_burst),
              Rewrite_drain_streak,
              comma_num(s.rewrite_drain_yields),
              comma_num(s.rewrite_inference_turns));
    }
  }
  if (compact_otter_audit_mode()) {
    fprintf(fp,
            "Compact_otter_audit: queries=%s, failures=%s, current_rules=%s, "
            "peak_rules=%s, physical_rules=%s, attempts=%s, rewrites=%s, "
            "bytes=%s.\n",
            comma_num(s.compact_otter_audit_queries),
            comma_num(s.compact_otter_audit_failures),
            comma_num(s.compact_rewrite_rules_current),
            comma_num(s.compact_rewrite_rules_peak),
            comma_num(s.compact_rewrite_rules_physical),
            comma_num(s.compact_rewrite_attempts),
            comma_num(s.compact_rewrite_rewrites),
            comma_num(s.rewrite_bank_bytes));
  }
  if (compact_otter_demod_mode()) {
    fprintf(fp,
            "Compact_otter_demodulation: current_rules=%s, peak_rules=%s, "
            "retired_rules=%s, physical_rules=%s, compactions=%s, "
            "attempts=%s, rewrites=%s, bytes=%s, peak_bytes=%s, "
            "nodes=%s, postings=%s, occurrences=%s, rules=%s, terms=%s, "
            "hash=%s.\n",
            comma_num(s.compact_rewrite_rules_current),
            comma_num(s.compact_rewrite_rules_peak),
            comma_num(s.compact_rewrite_rules_retired),
            comma_num(s.compact_rewrite_rules_physical),
            comma_num(s.compact_rewrite_compactions),
            comma_num(s.compact_rewrite_attempts),
            comma_num(s.compact_rewrite_rewrites),
            comma_num(s.rewrite_bank_bytes),
            comma_num(s.rewrite_bank_peak_bytes),
            comma_num(s.compact_rewrite_node_bytes),
            comma_num(s.compact_rewrite_posting_bytes),
            comma_num(s.compact_rewrite_occurrence_bytes),
            comma_num(s.compact_rewrite_rule_bytes),
            comma_num(s.compact_rewrite_term_bytes),
            comma_num(s.compact_rewrite_hash_bytes));
    fprintf(fp,
            "Compact_rewrite_shape: node_items=%llu, posting_items=%llu.\n",
            s.compact_rewrite_node_items,
            s.compact_rewrite_posting_items);
    fprintf(fp,
            "Compact_rewrite_occurrence_stream: used=%s, bytes=%s.\n",
            comma_num(s.compact_rewrite_occurrence_stream_used),
            comma_num(s.compact_rewrite_occurrence_stream_bytes));
  }
  if (flag(Opt->compact_unit_subsumption_audit) ||
      flag(Opt->compact_otter_unit_index))
    fprint_compact_unit_index(fp);
  if (flag(Opt->compact_back_demod_audit) ||
      flag(Opt->compact_otter_back_demod_index))
    fprint_compact_back_demod(fp);
  if (flag(Opt->compact_otter_back_demod_index) && !flag(Opt->back_demod))
    fprintf(fp,
            "Compact_index_inactive: component=back_demod, "
            "reason=back_demod_disabled.\n");
  if (flag(Opt->compact_nonunit_subsumption_audit) ||
      flag(Opt->compact_otter_nonunit_index))
    fprint_compact_nonunit_index(fp);
  if (Compact_terms != NULL) {
    struct compact_term_pool_stats terms;
    compact_term_pool_get_stats(Compact_terms, &terms);
    fprintf(fp,
            "Compact_term_pool: clauses=%s, serializations=%s, lookups=%s, "
            "hits=%s, reused_tokens=%s, logical_tokens=%s, tokens=%s, "
            "token_growths=%s, token_copy_bytes=%s, rebase_growths=%s, "
            "rebase_copy_bytes=%s, streamed_rebases=%llu, "
            "file_sorted_rebases=%llu, directory=%s, "
            "bytes=%s, "
            "peak_bytes=%s, compactions=%s, reclaimed=%s, "
            "reclaim_kb=%d.\n",
            comma_num(terms.clause_entries),
            comma_num(terms.serializations), comma_num(terms.lookups),
            comma_num(terms.hits), comma_num(terms.reused_tokens),
            comma_num(terms.logical_tokens), comma_num(terms.token_bytes),
            comma_num(terms.token_growths), comma_num(terms.token_copy_bytes),
            comma_num(terms.rebase_growths),
            comma_num(terms.rebase_copy_bytes),
            terms.streamed_rebases,
            terms.file_sorted_rebases,
            comma_num(terms.directory_bytes), comma_num(terms.total_bytes),
            comma_num(terms.peak_bytes), comma_num(terms.compactions),
            comma_num(terms.bytes_reclaimed),
            parm(Opt->compact_term_reclaim_kb));
    fprintf(fp, "Compact_index_policy: stale_pct=%d.\n",
            parm(Opt->compact_index_stale_pct));
    fprintf(fp,
            "Compact_term_trigger: next_serialization=%s, "
            "cooldown_skips=%s, capacity_deferrals=%s, "
            "last_predicted_reclaim=%s.\n",
            comma_num(Compact_term_next_reclaim_serialization),
            comma_num(Compact_term_reclaim_cooldown_skips),
            comma_num(Compact_term_reclaim_deferrals),
            comma_num(Compact_term_last_predicted_reclaim));
    if (terms.sharing_profile_enabled)
      fprintf(fp,
              "Compact_term_sharing: occurrences=%s, unique=%s, "
              "duplicates=%s, child_refs=%s, atom_roots=%s, "
              "dag_payload=%s, profile_table=%s.\n",
              comma_num(terms.profile_term_occurrences),
              comma_num(terms.profile_unique_terms),
              comma_num(terms.profile_term_occurrences -
                        terms.profile_unique_terms),
              comma_num(terms.profile_child_references),
              comma_num(terms.profile_atom_roots),
              comma_num(terms.profile_dag_payload_bytes),
              comma_num(terms.profile_table_bytes));
  }
  if (collective_frontier_mode()) {
    fprintf(fp,
            "Collective_scheduler: policy=%s, drain=%d, high=%d, low=%d, "
            "lag_limit=%d.\n",
            stringparm1(Opt->collective_scheduler), Collective_drain_mode,
            parm(Opt->collective_descriptor_high_water),
            parm(Opt->collective_descriptor_low_water),
            parm(Opt->collective_oldest_lag_limit));
    fprintf(fp,
            "Collective_frontier: batches_created=%s, completed=%s, "
            "pending=%s, peak=%s, ratio=%d, skipped=%s, "
            "parent_materializations=%s, activations=%s.\n",
            comma_num(s.collective_batches_created),
            comma_num(s.collective_batches_completed),
            comma_num(s.collective_batches_pending),
            comma_num(s.collective_batches_peak),
            parm(Opt->collective_given_ratio),
            comma_num(s.collective_partners_skipped),
            comma_num(s.collective_parent_materializations),
            comma_num(s.collective_activation_entries));
    fprintf(fp,
            "Collective_backlog: paramod=%s, pos_hyper=%s, neg_hyper=%s, "
            "lag_mean=%.1f, lag_p50=%s, lag_p95=%s, lag_max=%s.\n",
            comma_num(s.collective_pending_paramod),
            comma_num(s.collective_pending_pos_hyper),
            comma_num(s.collective_pending_neg_hyper),
            s.collective_batches_pending == 0 ? 0.0 :
              (double) s.collective_descriptor_lag_sum /
                (double) s.collective_batches_pending,
            comma_num(s.collective_descriptor_lag_p50),
            comma_num(s.collective_descriptor_lag_p95),
            comma_num(s.collective_descriptor_lag_max));
    fprintf(fp,
            "Collective_rule_descriptors: created_paramod=%s, "
            "created_pos_hyper=%s, created_neg_hyper=%s, "
            "completed_paramod=%s, completed_pos_hyper=%s, "
            "completed_neg_hyper=%s.\n",
            comma_num(s.collective_created_paramod),
            comma_num(s.collective_created_pos_hyper),
            comma_num(s.collective_created_neg_hyper),
            comma_num(s.collective_completed_paramod),
            comma_num(s.collective_completed_pos_hyper),
            comma_num(s.collective_completed_neg_hyper));
    fprintf(fp,
            "Collective_work: paramod_turns=%s, paramod_pairs_completed=%s, "
            "hyper_turns=%s, hyper_sets_completed=%s.\n",
            comma_num(s.collective_pair_turns),
            comma_num(s.collective_pair_expansions),
            comma_num(s.collective_hyper_expansions),
            comma_num(s.collective_hyper_sets_completed));
    fprintf(fp,
            "Collective_balanced: lane_paramod=%s, lane_pos_hyper=%s, "
            "lane_neg_hyper=%s, lane_turns=%s, oldest_turns=%s, "
            "drain_entries=%s, drain_exits=%s, givens_withheld=%s, "
            "paramod_from_turns=%s, paramod_into_turns=%s.\n",
            comma_num(s.collective_balanced_paramod_turns),
            comma_num(s.collective_balanced_pos_hyper_turns),
            comma_num(s.collective_balanced_neg_hyper_turns),
            comma_num(s.collective_balanced_lane_turns),
            comma_num(s.collective_balanced_oldest_turns),
            comma_num(s.collective_drain_entries),
            comma_num(s.collective_drain_exits),
            comma_num(s.collective_givens_withheld),
            comma_num(s.collective_paramod_from_turns),
            comma_num(s.collective_paramod_into_turns));
    fprintf(fp,
            "Collective_chunks: limit=%d, emitted=%s, replayed=%s, "
            "raw_seen=%s, deferred_turns=%s, raw_peak=%s.\n",
            parm(Opt->collective_candidate_chunk),
            comma_num(s.collective_candidates_emitted),
            comma_num(s.collective_candidates_replayed),
            comma_num(s.collective_raw_candidates_seen),
            comma_num(s.collective_deferred_turns),
            comma_num(s.collective_raw_candidates_peak));
    fprintf(fp,
            "Collective_iterators: raw_budget=%d, raw_steps=%s, "
            "candidates=%s, completions=%s, invalidations=%s, "
            "raw_turn_peak=%s, path_bytes=%s, hyper_raw_steps=%s, "
            "hyper_candidates=%s, hyper_completions=%s, "
            "hyper_choice_bytes=%s.\n",
            parm(Opt->collective_raw_work_budget),
            comma_num(s.collective_iterator_raw_steps),
            comma_num(s.collective_iterator_candidates),
            comma_num(s.collective_iterator_completions),
            comma_num(s.collective_iterator_invalidations),
            comma_num(s.collective_iterator_raw_peak),
            comma_num(s.collective_iterator_path_bytes),
            comma_num(s.collective_hyper_iterator_raw_steps),
            comma_num(s.collective_hyper_iterator_candidates),
            comma_num(s.collective_hyper_iterator_completions),
            comma_num(s.collective_hyper_iterator_choice_bytes));
    fprintf(fp,
            "Collective_candidate_cache: limit=%d, current=%s, peak=%s, "
            "stalls=%s.\n",
            parm(Opt->collective_candidate_cache),
            comma_num(s.collective_candidate_cache_current),
            comma_num(s.collective_candidate_cache_peak),
            comma_num(s.collective_candidate_cache_stalls));
    fprintf(fp,
            "Collective_candidate_pool: window=%d, commit_interval=%d, "
            "fair_interval=%d, entries=%s, bytes=%s, peak_bytes=%s, "
            "commits=%s, priority_commits=%s, fair_commits=%s.\n",
            parm(Opt->collective_candidate_window),
            parm(Opt->collective_candidate_commit_interval),
            parm(Opt->collective_candidate_fair_interval),
            comma_num(Collective_candidate_heap_count),
            comma_num(s.collective_candidate_pool_bytes),
            comma_num(s.collective_candidate_pool_peak_bytes),
            comma_num(s.collective_candidate_pool_commits),
            comma_num(s.collective_candidate_priority_commits),
            comma_num(s.collective_candidate_fair_commits));
    fprintf(fp,
            "Collective_preview: calls=%s, predicted_hints=%s, "
            "authoritative_hints=%s, false_positives=%s, "
            "changed_hint_ids=%s, stale_refreshes=%s.\n",
            comma_num(s.collective_preview_calls),
            comma_num(s.collective_preview_hint_matches),
            comma_num(s.collective_preview_authoritative_matches),
            comma_num(s.collective_preview_false_positives),
            comma_num(s.collective_preview_changed_hint_ids),
            comma_num(s.collective_preview_stale_refreshes));
    fprintf(fp,
            "Collective_discovery: enabled=%d, raw_budget=%d, distance_limit=%d, "
            "promotion_cap=%d, turns=%s, hot_turns=%s, general_turns=%s, "
            "forced_fair=%s, raw_steps=%s, inspected=%s, promotions=%s, "
            "confirmed=%s, false_positives=%s, duplicate_skips=%s, "
            "catchups=%s, distance_max=%s, cap_stalls=%s, "
            "consumed_records=%s, consumed_bytes=%s.\n",
            collective_balanced_mode() &&
              flag(Opt->collective_hint_discovery),
            parm(Opt->collective_discovery_raw_budget),
            parm(Opt->collective_discovery_distance),
            parm(Opt->collective_discovery_promotion_cap),
            comma_num(s.collective_discovery_turns),
            comma_num(s.collective_discovery_hot_turns),
            comma_num(s.collective_discovery_general_turns),
            comma_num(s.collective_discovery_forced_fair_turns),
            comma_num(s.collective_discovery_raw_steps),
            comma_num(s.collective_discovery_candidates),
            comma_num(s.collective_discovery_promotions),
            comma_num(s.collective_discovery_confirmed),
            comma_num(s.collective_discovery_false_positives),
            comma_num(s.collective_discovery_duplicate_skips),
            comma_num(s.collective_discovery_catchups),
            comma_num(s.collective_discovery_distance_max),
            comma_num(s.collective_discovery_cap_stalls),
            comma_num(s.collective_discovery_consumed_records),
            comma_num(s.collective_discovery_consumed_bytes));
    fprintf(fp,
            "Collective_promising: enabled=%d, scans=%s, considered=%s, "
            "buffer_peak=%s.\n",
            flag(Opt->collective_promising_candidates),
            comma_num(s.collective_promising_scans),
            comma_num(s.collective_promising_considered),
            comma_num(s.collective_promising_buffer_peak));
    fprintf(fp,
            "Collective_promising_scheduler: enabled=%d, fair_interval=%d, "
            "priority_turns=%s, fair_turns=%s, heap_peak=%s, "
            "heap_pending=%s.\n",
            flag(Opt->collective_promising_scheduler),
            parm(Opt->collective_promising_fair_interval),
            comma_num(s.collective_promising_priority_turns),
            comma_num(s.collective_promising_fair_turns),
            comma_num(s.collective_promising_heap_peak),
            comma_num(Collective_priority_heap_count));
    fprintf(fp,
            "Collective_hint_probes: scheduled=%s, expanded=%s, "
            "credit=%s.\n",
            comma_num(s.collective_hint_probes_scheduled),
            comma_num(s.collective_hint_probes_expanded),
            Collective_hint_probe_credit ? "available" : "consumed");
    fprintf(fp,
            "Collective_hint_selection: selected=%s, Hha=%s, Hw=%s, "
            "LH=%s, other=%s, distinct_matched=%s.\n",
            comma_num(s.collective_hint_selected_total),
            comma_num(s.collective_hint_selected_hha),
            comma_num(s.collective_hint_selected_hw),
            comma_num(s.collective_hint_selected_lh),
            comma_num(s.collective_hint_selected_other),
            comma_num(s.collective_distinct_hints_matched));
    fprintf(fp,
            "Collective_memory: descriptor_bytes=%s, history_bytes=%s, "
            "deactivations=%s.\n",
            comma_num(s.collective_descriptor_bytes),
            comma_num(s.collective_history_bytes),
            comma_num(s.collective_deactivation_entries));
    if (s.collective_snapshot_rebuilds != 0)
      fprintf(fp,
              "Collective_snapshots: rebuilds=%s, clauses_indexed=%s, "
              "peak=%s.\n",
              comma_num(s.collective_snapshot_rebuilds),
              comma_num(s.collective_snapshot_clauses),
              comma_num(s.collective_snapshot_clauses_peak));
    fprintf(fp,
            "Collective_history_index: clauses=%s, indexed=%s, shared=%s, "
            "retained=%s, retained_clause_bytes=%s, queries=%s, candidates=%s, "
            "future_rejected=%s, inactive_rejected=%s.\n",
            comma_num(s.collective_history_clauses),
            comma_num(s.collective_history_indexed_clauses),
            comma_num(s.collective_history_shared_clauses),
            comma_num(s.collective_history_retained_clauses),
            comma_num(s.collective_history_clause_bytes),
            comma_num(s.collective_history_queries),
            comma_num(s.collective_history_candidates),
            comma_num(s.collective_history_rejected_future),
            comma_num(s.collective_history_rejected_inactive));
  }
  fprintf(fp, "Hint_index: mode=%s, fpa_depth=%d, epoch=%llu.\n",
          stringparm1(Opt->hint_index), configured_hint_fpa_depth(),
          hint_state_epoch());

  if (str_ident(stats_level, "lots") || str_ident(stats_level, "all")) {

    fprintf(fp,"Kept_by_rule=%s, Deleted_by_rule=%s.\n",
	    comma_num(s.kept_by_rule), comma_num(s.deleted_by_rule));
    fprintf(fp,"Forward_subsumed=%s. Back_subsumed=%s.\n",
	    comma_num(s.subsumed), comma_num(s.back_subsumed));
    if (s.anc_subsume_blocked > 0)
      fprintf(fp,"Anc_subsume_blocked=%s.\n",
	      comma_num(s.anc_subsume_blocked));
    fprintf(fp,"Sos_limit_deleted=%s. Sos_displaced=%s. Sos_removed=%s.\n",
	    comma_num(s.sos_limit_deleted), comma_num(s.sos_displaced),
	    comma_num(s.sos_removed));
    fprintf(fp,"New_demodulators=%s (%s lex), Back_demodulated=%s. Back_unit_deleted=%s.\n",
	    comma_num(s.new_demodulators), comma_num(s.new_lex_demods),
	    comma_num(s.back_demodulated), comma_num(s.back_unit_deleted));
    fprintf(fp,"Demod_attempts=%s. Demod_rewrites=%s.\n",
	    comma_num(s.demod_attempts), comma_num(s.demod_rewrites));
    fprintf(fp,"Res_instance_prunes=%s. Para_instance_prunes=%s. Basic_paramod_prunes=%s.\n",
	    comma_num(s.res_instance_prunes), comma_num(s.para_instance_prunes),
	    comma_num(s.basic_para_prunes));
    fprintf(fp,"Nonunit_fsub_feature_tests=%s. ", comma_num(s.nonunit_fsub));
    fprintf(fp,"Nonunit_bsub_feature_tests=%s.\n", comma_num(s.nonunit_bsub));
    if (mindex_queries_skipped() > 0 || mindex_queries_warned() > 0) {
      fprintf(fp,"Candidate_limits: warned=%s, skipped=%s.\n",
              comma_num(mindex_queries_warned()),
              comma_num(mindex_queries_skipped()));
    }
  }

  fprintf(fp,"Max_Clause_ID=%s.\n", comma_num(clause_ids_assigned()));
  fprintf(fp,"Megabytes=%.2f.\n", s.kbyte_usage / 1000.0);

  fprintf(fp,
          "Disabled_compression: attempted=%s, successful=%s, skipped=%s, "
          "materialized=%s, recompressed=%s.\n",
          comma_num(s.compression_attempted),
          comma_num(s.compression_successful),
          comma_num(s.compression_skipped),
          comma_num(s.compression_materialized),
          comma_num(s.compression_recompressed));
  fprintf(fp,
          "Clause_body_bytes: active=%s, passive=%s, hints=%s, "
          "disabled_full=%s (%s clauses), "
          "disabled_compressed=%s (%s clauses), disabled_estimated_full=%s.\n",
          comma_num(s.active_body_bytes), comma_num(s.passive_body_bytes),
          comma_num(s.hint_body_bytes),
          comma_num(s.disabled_full_body_bytes),
          comma_num(s.disabled_full_clauses),
          comma_num(s.disabled_compressed_bytes),
          comma_num(s.disabled_compressed_clauses),
          comma_num(s.disabled_estimated_uncompressed_bytes));
  fprintf(fp,
          "Passive_store: compressed=%s, body_bytes=%s, "
          "justification_bytes=%s, total_bytes=%s, estimated_full=%s.\n",
          comma_num(s.passive_compressed_clauses),
          comma_num(s.passive_body_bytes),
          comma_num(s.passive_justification_bytes),
          comma_num(s.passive_total_payload_bytes),
          comma_num(s.passive_estimated_full_body_bytes));
  if (dense_passive_mode())
    fprintf(fp,
            "Dense_passive: backing=%s, records=%s, record_bytes=%s, "
            "heap_bytes=%s, arena_records=%s, arena_record_bytes=%s, "
            "arena_backing=%s, arena_physical=%s, "
            "allocated_bytes_per_active=%.2f.\n",
            compact_otter_passive_mode() ?
              (str_ident(stringparm1(Opt->ancestor_store), "mmap") ?
                 "ancestor-mmap" :
               str_ident(stringparm1(Opt->ancestor_store), "file") ?
                 "ancestor-file" : "ancestor-memory") :
              cold_passive_store_mode_name(
                cold_passive_store_get_stats(Dense_body_store).mode),
            comma_num(s.dense_passive_records),
            comma_num(s.dense_passive_record_bytes),
            comma_num(s.dense_passive_heap_bytes),
            comma_num(s.dense_passive_arena_records),
            comma_num(s.dense_passive_arena_record_bytes),
            comma_num(s.dense_passive_arena_backing_bytes),
            comma_num(s.dense_passive_arena_physical_bytes),
            s.dense_passive_records == 0 ? 0.0 :
            (double) (s.dense_passive_record_bytes +
                      s.dense_passive_heap_bytes +
                      s.dense_passive_arena_backing_bytes) /
            s.dense_passive_records);
  if (dense_passive_mode())
    fprintf(fp,
            "Dense_passive_gc: arena_materialized=%s, "
            "validation_failures=%s, compactions=%s, "
            "records_reclaimed=%s, arena_bytes_reclaimed=%s, "
            "file_reads=%s (%s bytes), file_writes=%s (%s bytes).\n",
            comma_num(s.dense_passive_arena_materializations),
            comma_num(s.dense_passive_arena_validation_failures),
            comma_num(s.dense_passive_compactions),
            comma_num(s.dense_passive_records_reclaimed),
            comma_num(s.dense_passive_arena_bytes_reclaimed),
            comma_num(s.dense_passive_arena_file_reads),
            comma_num(s.dense_passive_arena_file_read_bytes),
            comma_num(s.dense_passive_arena_file_writes),
            comma_num(s.dense_passive_arena_file_write_bytes));
  fprintf(fp,
          "Hint_store: compressed=%s, body_bytes=%s, estimated_full=%s.\n",
          comma_num(s.hint_compressed_clauses),
          comma_num(s.hint_body_bytes),
          comma_num(s.hint_estimated_full_body_bytes));
  if (packed_hints_enabled())
    {
      fprintf(fp,
              "Packed_hint_index: nodes=%s, references=%s, tables=%s, "
              "candidate_checks=%s.\n",
              comma_num(s.hint_index_node_bytes),
              comma_num(s.hint_index_reference_bytes),
              comma_num(s.hint_index_table_bytes),
              comma_num(s.hint_candidate_checks));
      fprint_packed_hint_operation_stats(fp);
    }
  fprintf(fp,
          "Ancestor_store: records=%s, record_bytes=%s, backing_bytes=%s, "
          "handle_bytes=%s, materialized=%s, validation_failures=%s, "
          "mmap_eviction_passes=%s, mmap_eviction_bytes=%s, "
          "mmap_scan_eviction_passes=%s, mmap_scan_eviction_bytes=%s, "
          "io_buffer=%s, file_reads=%s (%s bytes), "
          "file_writes=%s (%s bytes).\n",
          comma_num(s.ancestor_records), comma_num(s.ancestor_record_bytes),
          comma_num(s.ancestor_backing_bytes), comma_num(s.ancestor_handle_bytes),
          comma_num(s.ancestor_materializations),
          comma_num(s.ancestor_validation_failures),
          comma_num(s.ancestor_mmap_eviction_passes),
          comma_num(s.ancestor_mmap_eviction_bytes),
          comma_num(s.ancestor_mmap_scan_eviction_passes),
          comma_num(s.ancestor_mmap_scan_eviction_bytes),
          comma_num(s.ancestor_io_buffer_bytes),
          comma_num(s.ancestor_file_reads),
          comma_num(s.ancestor_file_read_bytes),
          comma_num(s.ancestor_file_writes),
          comma_num(s.ancestor_file_write_bytes));
  if (compact_otter_passive_mode())
    fprintf(fp,
            "Compact_passive_cache: budget=%s, metadata_bytes=%s, "
            "entries=%s, peak_entries=%s, charged_bytes=%s, "
            "peak_charged_bytes=%s, hits=%s, misses=%s, bypasses=%s, "
            "evictions=%s, invalidations=%s.\n",
            comma_num(Compact_passive_cache_budget),
            comma_num(Compact_passive_cache_slots *
                      sizeof(*Compact_passive_cache)),
            comma_num(Compact_passive_cache_entries),
            comma_num(Compact_passive_cache_peak_entries),
            comma_num(Compact_passive_cache_bytes),
            comma_num(Compact_passive_cache_peak_bytes),
            comma_num(Compact_passive_cache_hits),
            comma_num(Compact_passive_cache_misses),
            comma_num(Compact_passive_cache_bypasses),
            comma_num(Compact_passive_cache_evictions),
            comma_num(Compact_passive_cache_invalidations));
  fprintf(fp,
          "Bookkeeping_bytes: disabled_store=%s (legacy_clist=%s, %.2f/clause), "
          "clause_id_table=%s (legacy_hash=%s, entries=%s, pages=%s, "
          "slots=%s, %.2f/entry).\n",
          comma_num(s.disabled_store_bytes),
          comma_num(s.disabled_legacy_clist_bytes),
          s.disabled_size == 0 ? 0.0 :
            (double) s.disabled_store_bytes / s.disabled_size,
          comma_num(s.clause_id_table_bytes),
          comma_num(s.clause_id_legacy_bytes),
          comma_num(s.clause_id_entries),
          comma_num(s.clause_id_pages),
          comma_num(s.clause_id_table_capacity),
          s.clause_id_entries == 0 ? 0.0 :
            (double) s.clause_id_table_bytes / s.clause_id_entries);
  fprintf(fp,
          "Allocator_bytes: live=%s, live_peak=%s, reserved_current=%s, "
          "reserved_peak=%s, fragmentation=%s, reusable=%s, ",
          comma_num(s.allocator_logical_live_bytes),
          comma_num(s.allocator_logical_peak_bytes),
          comma_num(s.allocator_reserved_bytes),
          comma_num(s.allocator_peak_reserved_bytes),
          comma_num(s.allocator_fragmentation_bytes),
          comma_num(s.allocator_reusable_bytes));
  fprintf(fp,
          "unallocated=%s, metadata=%s, direct_live=%s, permanent_live=%s, "
          "palloc_cumulative=%s.\n",
          comma_num(s.allocator_unallocated_bytes),
          comma_num(s.allocator_metadata_bytes),
          comma_num(s.allocator_direct_live_bytes),
          comma_num(s.allocator_permanent_live_bytes),
          comma_num(s.palloc_cumulative_bytes));
  fprintf(fp,
          "Allocator_slabs: current=%s, peak=%s, reclaimed=%s (%s bytes), "
          "RSS_kb: current=%s, peak=%s. ",
          comma_num(s.allocator_slab_count),
          comma_num(s.allocator_peak_slab_count),
          comma_num(s.allocator_reclaimed_slabs),
          comma_num(s.allocator_reclaimed_bytes),
          comma_num(s.current_rss_kbytes), comma_num(s.peak_rss_kbytes));
  fprintf(fp,
          "Allocator_traffic: calls=%s, bytes=%s. "
          "FPA: nodes_live=%s, nodes_peak=%s, lists_live=%s, lists_peak=%s.\n",
          comma_num(s.allocator_allocation_calls),
          comma_num(s.allocator_cumulative_bytes),
          comma_num(s.fpa_live_nodes), comma_num(s.fpa_peak_nodes),
          comma_num(s.fpa_live_lists), comma_num(s.fpa_peak_lists));
  if (s.process_smaps_supported)
    fprintf(fp,
            "Process_residency_kb: pss=%s, anonymous=%s, shared_clean=%s, "
            "shared_dirty=%s, private_clean=%s, private_dirty=%s, swap=%s.\n",
            comma_num(s.process_pss_kbytes),
            comma_num(s.process_anonymous_kbytes),
            comma_num(s.process_shared_clean_kbytes),
            comma_num(s.process_shared_dirty_kbytes),
            comma_num(s.process_private_clean_kbytes),
            comma_num(s.process_private_dirty_kbytes),
            comma_num(s.process_swap_kbytes));
  if (s.process_libc_heap_supported)
    fprintf(fp,
            "Libc_heap_bytes: arena=%s, mmap=%s, in_use=%s, free=%s, "
            "releasable=%s, compact_policy=%s.\n",
            comma_num(s.libc_arena_bytes), comma_num(s.libc_mmap_bytes),
            comma_num(s.libc_in_use_bytes), comma_num(s.libc_free_bytes),
            comma_num(s.libc_releasable_bytes),
            s.process_compact_heap_enabled ? "enabled" : "disabled");

  fprintf(fp,"User_CPU=%.2f, System_CPU=%.2f, Wall_clock=%u.\n",
	  user_seconds(), system_seconds(), wallclock());
}  /* fprint_prover_stats */

/*************
 *
 *   fprint_prover_clocks()
 *
 *************/

/* DOCUMENTATION
Given an arroy of clock values (type double) indexed by
the ordinary clock indexes, print a report to a file.
*/

/* PUBLIC */
void fprint_prover_clocks(FILE *fp, struct prover_clocks clks)
{
  if (clocks_enabled()) {
    fprintf(fp, "\n");
    fprint_clock(fp, clks.pick_given);
    fprint_clock(fp, clks.infer);
    fprint_clock(fp, clks.preprocess);
    fprint_clock(fp, clks.demod);
    fprint_clock(fp, clks.unit_del);
    fprint_clock(fp, clks.redundancy);
    fprint_clock(fp, clks.conflict);
    fprint_clock(fp, clks.weigh);
    fprint_clock(fp, clks.hints);
    fprint_clock(fp, clks.subsume);
    fprint_clock(fp, clks.semantics);
    fprint_clock(fp, clks.back_subsume);
    fprint_clock(fp, clks.back_demod);
    fprint_clock(fp, clks.back_unit_del);
    fprint_clock(fp, clks.index);
    fprint_clock(fp, clks.disable);
  }
}  /* fprint_prover_clocks */

/*************
 *
 *   fprint_all_stats()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void fprint_all_stats(FILE *fp, char *stats_level)
{
  /* In TPTP output mode, suppress statistics on stdout */
  if (Opt && flag(Opt->tptp_output) && fp == stdout)
    return;

  update_stats();

  print_separator(fp, "STATISTICS", TRUE);

  fprint_prover_stats(fp, Stats, stats_level);

  fprint_prover_clocks(fp, Clocks);

  if (!clist_empty(Glob.hints))
    print_hint_match_stats(fp, Glob.hints);

  if (str_ident(stats_level, "all")) {
    print_memory_stats(fp);
    fprint_selector_report(fp);
    /* p_sos_dist(); */
  }
  print_separator(fp, "end of statistics", TRUE);
  fflush(fp);
}  // fprint_all_stats

/*************
 *
 *   exit_string()
 *
 *************/

static
char *exit_string(int code)
{
  char *message;
  switch (code) {
  case MAX_PROOFS_EXIT:  message = "max_proofs";  break;
  case FATAL_EXIT:       message = "fatal_error"; break;
  case SOS_EMPTY_EXIT:   message = "sos_empty";   break;
  case MAX_MEGS_EXIT:    message = "max_megs";    break;
  case MAX_SECONDS_EXIT: message = "max_seconds"; break;
  case MAX_GIVEN_EXIT:   message = "max_given";   break;
  case MAX_KEPT_EXIT:    message = "max_kept";    break;
  case ACTION_EXIT:      message = "action";      break;
  case MAX_NOHINTS_EXIT: message = "max_nohints"; break;
  case SIGSEGV_EXIT:     message = "SIGSEGV";     break;
  case SIGINT_EXIT:      message = "SIGINT";      break;
  case SIGTERM_EXIT:     message = "SIGTERM";     break;
  case CHECKPOINT_EXIT:  message = "checkpoint";  break;
  default: message = "???";
  }
  return message;
}  /* exit_string */

/*************
 *
 *   szs_status_string()
 *
 *   Map Prover9 exit code to SZS status string.
 *
 *************/

static
char *szs_status_string(int code)
{
  switch (code) {
  case MAX_PROOFS_EXIT:
    return Glob.has_goals ? "Theorem" : "Unsatisfiable";
  case SOS_EMPTY_EXIT:
    return "GaveUp";
  case MAX_SECONDS_EXIT:
    return "Timeout";
  case MAX_MEGS_EXIT:
    return "MemoryOut";
  case MAX_GIVEN_EXIT:
  case MAX_KEPT_EXIT:
  case ACTION_EXIT:
  case MAX_NOHINTS_EXIT:
    return "GaveUp";
  case SIGSEGV_EXIT:
  case FATAL_EXIT:
    return "Error";
  case SIGINT_EXIT:
    return "User";
  case SIGTERM_EXIT:
    /* External wall-limit kill (competition infrastructure). */
    return "Timeout";
  case CHECKPOINT_EXIT:
    return "Unknown";
  default:
    return "Unknown";
  }
}  /* szs_status_string */

/*************
 *
 *   fprint_clause_tptp()
 *
 *   Print one derivation node in TSTP annotated-formula format: an FOF
 *   input formula as an fof(name, role, formula, file(...)) leaf, or a
 *   clause as cnf(c_ID, role, literals, source/inference).
 *
 *************/

/*************
 *
 *   fwrite_term_tptp()
 *
 *   Write a term, but replace the LADR "-" (negation) symbol with
 *   TPTP "~" for TSTP compliance.  We use sb_write_term to get
 *   the string, then do the replacement.
 *
 *************/

/* Quote ONE symbol occurrence if its name isn't a valid TPTP identifier.
   E.g., + becomes '+', >= becomes '>=', ==> becomes '==>'.
   This also forces arrange_term() to use prefix notation for
   these symbols, since quoted names aren't registered as infix. */
static
void tptp_quote_bad_sym_node(Term t)
{
  char *s = sn_to_str(SYMNUM(t));
  /* Restore distinct objects: do_foo -> "foo" */
  if (is_distinct_object(SYMNUM(t))) {
    const char *base = s + 3;  /* skip "do_" prefix */
    int n = strlen(base);
    char *new_str = safe_malloc(n + 3);
    new_str[0] = '"';
    strcpy(new_str + 1, base);
    new_str[n + 1] = '"';
    new_str[n + 2] = '\0';
    int new_sn = str_to_sn(new_str, sn_to_arity(SYMNUM(t)));
    safe_free(new_str);
    t->private_symbol = -(new_sn);
    return;
  }
  /* Check if symbol needs quoting: not already quoted, not a $keyword,
     not a LADR built-in connective (=, |, -, #), and not matching
     [a-z][a-zA-Z0-9_]* (valid TPTP lower_word). */
  BOOL bad = FALSE;
  if (s[0] != '\'' && s[0] != '$' &&
      strcmp(s, "=") != 0 && strcmp(s, "!=") != 0 &&
      strcmp(s, "|") != 0 && strcmp(s, "-") != 0 &&
      strcmp(s, "#") != 0) {
    if (!(s[0] >= 'a' && s[0] <= 'z'))
      bad = TRUE;
    else {
      int k;
      for (k = 1; s[k]; k++) {
        char c = s[k];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_'))
          { bad = TRUE; break; }
      }
    }
  }
  if (bad) {
    /* Build quoted version: '+' etc. */
    char *escaped = escape_char(s, '\'');
    int n = strlen(escaped);
    char *new_str = safe_malloc(n + 3);
    new_str[0] = '\'';
    strcpy(new_str + 1, escaped);
    new_str[n + 1] = '\'';
    new_str[n + 2] = '\0';
    int new_sn = str_to_sn(new_str, sn_to_arity(SYMNUM(t)));
    safe_free(new_str);
    safe_free(escaped);
    t->private_symbol = -(new_sn);
  }
}

static
void tptp_quote_bad_syms(Term t)
{
  int i;
  if (t == NULL || VARIABLE(t))
    return;
  tptp_quote_bad_sym_node(t);
  for (i = 0; i < ARITY(t); i++)
    tptp_quote_bad_syms(ARG(t, i));
}

/* TRUE if s is the name of one of the quantified variables in names. */
static
BOOL qvar_name_member(Plist names, const char *s)
{
  Plist p;
  for (p = names; p; p = p->next)
    if (str_ident((char *) p->v, (char *) s))
      return TRUE;
  return FALSE;
}

/* Like tptp_quote_bad_syms, but skip arity-0 terms whose name is a
   quantified variable of the enclosing formula: in a Formula atom a bound
   variable is an arity-0 rigid term, indistinguishable from a constant
   except by its enclosing quantifier, and it must print unquoted. */
static
void tptp_quote_bad_syms_skip_qvars(Term t, Plist qvars)
{
  int i;
  if (t == NULL || VARIABLE(t))
    return;
  if (!(ARITY(t) == 0 && qvar_name_member(qvars, sn_to_str(SYMNUM(t)))))
    tptp_quote_bad_sym_node(t);
  for (i = 0; i < ARITY(t); i++)
    tptp_quote_bad_syms_skip_qvars(ARG(t, i), qvars);
}

static
void fwrite_term_tptp(FILE *fp, Term t)
{
  String_buf sb = get_string_buf();
  char *s;
  int i;

  sb_write_term(sb, t);
  s = sb_to_malloc_string(sb);
  zap_string_buf(sb);

  /* Replace "-" that acts as negation (prefix operator) with "~".
     In LADR clause output, negation is printed as "-atom" (no space).
     Negation appears at start of string or after " | " (disjunction).
     We detect it by position context: beginning or after a disjunction
     separator, followed by a letter (predicate) or "(". */
  for (i = 0; s[i] != '\0'; i++) {
    if (s[i] == '-') {
      /* Is this at a negation position? */
      BOOL at_neg_pos = (i == 0 ||
                         (i >= 2 && s[i-1] == ' ' && s[i-2] == '|') ||
                         s[i-1] == '(');
      /* Is the next char a letter, open-paren, or quote (start of an atom)?
         Quote handles negation of quoted predicates: -'>='(A,B) -> ~'>='(A,B) */
      BOOL before_atom = (s[i+1] != '\0' &&
                          (s[i+1] == '(' ||
                           s[i+1] == '\'' ||
                           (s[i+1] >= 'a' && s[i+1] <= 'z') ||
                           (s[i+1] >= 'A' && s[i+1] <= 'Z') ||
                           s[i+1] == '$'));
      if (at_neg_pos && before_atom)
        fputc('~', fp);
      else
        fputc('-', fp);
    }
    else
      fputc(s[i], fp);
  }
  safe_free(s);  /* safe_malloc'd by sb_to_malloc_string */
}

/*************
 *
 *   fwrite_formula_tptp()
 *
 *   Recursively print a Formula in TPTP syntax.
 *   Quantifiers: ! [X,Y,Z] : (body), ? [X] : (body)
 *   Connectives: ~, &, |, =>, <=>
 *   Atoms: printed via fwrite_term_tptp (handles - to ~ replacement).
 *
 *************/

static
void fwrite_formula_tptp2(FILE *fp, Formula f, Plist qvars);   /* forward decl */

/* Collect the quantified-variable names of f (interned strings, no
   duplicates) so the atom printer can tell variables from constants. */
static
void collect_qvar_names(Formula f, Plist *names)
{
  int i;
  if (f == NULL)
    return;
  if (quant_form(f) && !qvar_name_member(*names, f->qvar))
    *names = plist_append(*names, f->qvar);
  for (i = 0; i < f->arity; i++)
    collect_qvar_names(f->kids[i], names);
}

/* Flatten a nested same-type disjunction/conjunction while printing, so a
   left-nested OR/AND tree prints as the canonical flat (a | b | c) rather
   than ((a | b) | c).  Operands of a different type print via
   fwrite_formula_tptp2, which parenthesizes binary connectives as needed. */
static
void fwrite_disj_operands(FILE *fp, Formula f, BOOL *first, Plist qvars)
{
  if (f->type == OR_FORM) {
    int i;
    for (i = 0; i < f->arity; i++)
      fwrite_disj_operands(fp, f->kids[i], first, qvars);
  }
  else {
    if (!*first) fprintf(fp, " | ");
    *first = FALSE;
    fwrite_formula_tptp2(fp, f, qvars);
  }
}

static
void fwrite_conj_operands(FILE *fp, Formula f, BOOL *first, Plist qvars)
{
  if (f->type == AND_FORM) {
    int i;
    for (i = 0; i < f->arity; i++)
      fwrite_conj_operands(fp, f->kids[i], first, qvars);
  }
  else {
    if (!*first) fprintf(fp, " & ");
    *first = FALSE;
    fwrite_formula_tptp2(fp, f, qvars);
  }
}

static
void fwrite_formula_tptp2(FILE *fp, Formula f, Plist qvars)
{
  if (f->type == ATOM_FORM) {
    /* Print via a copy with non-TPTP symbol names quoted ('+', '0', ...);
       quantified variables are skipped (they must print unquoted).  Quoting
       repoints to fresh symbols with no parse type, so quoted operators
       also print in prefix form. */
    Term a = copy_term(f->atom);
    tptp_quote_bad_syms_skip_qvars(a, qvars);
    fwrite_term_tptp(fp, a);
    zap_term(a);
  }
  else if (f->type == NOT_FORM) {
    fprintf(fp, "~ (");
    fwrite_formula_tptp2(fp, f->kids[0], qvars);
    fprintf(fp, ")");
  }
  else if (f->type == AND_FORM) {
    if (f->arity == 0)
      fprintf(fp, "$true");
    else {
      BOOL first = TRUE;
      fprintf(fp, "(");
      fwrite_conj_operands(fp, f, &first, qvars);
      fprintf(fp, ")");
    }
  }
  else if (f->type == OR_FORM) {
    if (f->arity == 0)
      fprintf(fp, "$false");
    else {
      BOOL first = TRUE;
      fprintf(fp, "(");
      fwrite_disj_operands(fp, f, &first, qvars);
      fprintf(fp, ")");
    }
  }
  else if (f->type == IMP_FORM) {
    fprintf(fp, "(");
    fwrite_formula_tptp2(fp, f->kids[0], qvars);
    fprintf(fp, " => ");
    fwrite_formula_tptp2(fp, f->kids[1], qvars);
    fprintf(fp, ")");
  }
  else if (f->type == IMPBY_FORM) {
    fprintf(fp, "(");
    fwrite_formula_tptp2(fp, f->kids[1], qvars);
    fprintf(fp, " => ");
    fwrite_formula_tptp2(fp, f->kids[0], qvars);
    fprintf(fp, ")");
  }
  else if (f->type == IFF_FORM) {
    fprintf(fp, "(");
    fwrite_formula_tptp2(fp, f->kids[0], qvars);
    fprintf(fp, " <=> ");
    fwrite_formula_tptp2(fp, f->kids[1], qvars);
    fprintf(fp, ")");
  }
  else if (f->type == ALL_FORM) {
    /* Collect consecutive universal quantifiers.  No parens around the
       body: a binary connective self-parenthesizes, and an atom or nested
       quantifier is already a TPTP unitary formula. */
    Formula body = f;
    BOOL first = TRUE;
    fprintf(fp, "! [");
    while (body->type == ALL_FORM) {
      if (!first) fprintf(fp, ",");
      fprintf(fp, "%s", body->qvar);
      first = FALSE;
      body = body->kids[0];
    }
    fprintf(fp, "] : ");
    fwrite_formula_tptp2(fp, body, qvars);
  }
  else if (f->type == EXISTS_FORM) {
    /* Collect consecutive existential quantifiers (see ALL_FORM). */
    Formula body = f;
    BOOL first = TRUE;
    fprintf(fp, "? [");
    while (body->type == EXISTS_FORM) {
      if (!first) fprintf(fp, ",");
      fprintf(fp, "%s", body->qvar);
      first = FALSE;
      body = body->kids[0];
    }
    fprintf(fp, "] : ");
    fwrite_formula_tptp2(fp, body, qvars);
  }
}

/* Print a Formula in TPTP syntax (see fwrite_formula_tptp2), quoting
   non-TPTP symbol names but never the formula's own quantified variables. */
static
void fwrite_formula_tptp(FILE *fp, Formula f)
{
  Plist qvars = NULL;
  collect_qvar_names(f, &qvars);
  fwrite_formula_tptp2(fp, f, qvars);
  zap_plist(qvars);
}

/*************
 *
 *   term_has_skolem() / clause_has_skolem()
 *
 *   Whether a term (or clause) contains a symbol introduced by
 *   clausification: a Skolem function/constant, or a Tseitin-style
 *   definition predicate (defn_N, from cnf.c introduce_definition).
 *   Used to choose the SZS status of a clausify step: a CNF clause that
 *   is a logical consequence of its FOF parent (no such fresh symbol) is
 *   status(thm); a clause that carries a Skolem witness or a definition
 *   predicate is only equisatisfiable with the parent, so it is
 *   status(esa).
 *
 *************/

static
BOOL term_has_skolem(Term t)
{
  int i;
  if (VARIABLE(t))
    return FALSE;
  if (is_skolem(SYMNUM(t)))
    return TRUE;
  /* Definition predicates introduced by Tseitin clausification (defn_N)
     are fresh symbols too, so their clauses are equisatisfiable (esa),
     not entailed (thm). */
  {
    char *nm = sn_to_str(SYMNUM(t));
    if (nm != NULL && strncmp(nm, "defn_", 5) == 0)
      return TRUE;
  }
  for (i = 0; i < ARITY(t); i++)
    if (term_has_skolem(ARG(t,i)))
      return TRUE;
  return FALSE;
}

static
BOOL clause_has_skolem(Topform c)
{
  Term t = topform_to_term_without_attributes(c);
  BOOL sk = (t != NULL) ? term_has_skolem(t) : FALSE;
  if (t)
    zap_term(t);
  return sk;
}

/*************
 *
 *   collect_fresh_symbols() / fprint_new_symbols() / fprint_clausify_status()
 *
 *   Support for TSTP new_symbols() annotations on esa clausify steps, so a
 *   derivation checker (e.g. GDV) can identify the freshly-introduced
 *   Skolem functions and Tseitin definition predicates.  Each fresh symbol
 *   is declared exactly once, at its first appearance in the proof, so it
 *   is not mistaken for a Skolem/definition reuse.
 *
 *************/

/* Symbols already declared via new_symbols earlier in this proof. */
static Ilist Declared_fresh_syms = NULL;

static
void collect_fresh_symbols(Term t, Ilist *skolems, Ilist *defs)
{
  int i, sn;
  if (VARIABLE(t))
    return;
  sn = SYMNUM(t);
  if (is_skolem(sn)) {
    if (!ilist_member(*skolems, sn))
      *skolems = ilist_append(*skolems, sn);
  }
  else if (find_introduced_definition(sn) != NULL) {
    /* A recorded introduced definition -- a name test would also catch
       problem symbols that happen to start with defn_. */
    if (!ilist_member(*defs, sn))
      *defs = ilist_append(*defs, sn);
  }
  for (i = 0; i < ARITY(t); i++)
    collect_fresh_symbols(ARG(t,i), skolems, defs);
}

/* Print new_symbols(kind, [syms not yet declared]) if any, and record them
   as declared.  Nothing is printed if every symbol was already declared. */
static
void fprint_new_symbols(FILE *fp, const char *kind, Ilist syms)
{
  Ilist p;
  BOOL any = FALSE;
  for (p = syms; p; p = p->next)
    if (!ilist_member(Declared_fresh_syms, p->i)) { any = TRUE; break; }
  if (!any)
    return;
  fprintf(fp, ", new_symbols(%s, [", kind);
  {
    BOOL first = TRUE;
    for (p = syms; p; p = p->next) {
      if (ilist_member(Declared_fresh_syms, p->i))
        continue;
      if (!first)
        fprintf(fp, ",");
      fprintf(fp, "%s", sn_to_str(p->i));
      Declared_fresh_syms = ilist_append(Declared_fresh_syms, p->i);
      first = FALSE;
    }
  }
  fprintf(fp, "])");
}

/* Print "[status(st)" plus, for esa steps, new_symbols() records for the
   clause's not-yet-declared fresh symbols, then "]". */
static
void fprint_clausify_status(FILE *fp, Topform c, const char *st)
{
  fprintf(fp, "[status(%s)", st);
  if (strcmp(st, "esa") == 0) {
    Term t = topform_to_term_without_attributes(c);
    Ilist skolems = NULL, defs = NULL;
    if (t != NULL) {
      collect_fresh_symbols(t, &skolems, &defs);
      zap_term(t);
    }
    fprint_new_symbols(fp, "skolem", skolems);
    fprint_new_symbols(fp, "definition", defs);
    zap_ilist(skolems);
    zap_ilist(defs);
  }
  fprintf(fp, "]");
}

static void make_neg_name(char *buf, size_t bufsz, const char *tptp_name);
static void emit_definition_leaves(FILE *fp, Ilist syms);
static void rename_bad_qvars_formula(Formula f, I2list *map);

/*************
 *
 *   Skolemization grouping.
 *
 *   Prover9 collapses NNF + skolemize + CNF, so a single existential parent
 *   yields several clauses, each emitted as its own esa clausify step.  No
 *   single such clause is equisatisfiable with the parent, so a derivation
 *   checker cannot verify it.  Instead, for a parent that Skolemizes, we emit
 *   ONE skolemize step whose body is the conjunction of that parent's
 *   (universally closed) clauses -- which IS equisatisfiable with the parent
 *   (GDV verifies it by the backward direction) -- followed by split_conjunct
 *   (thm) steps for the individual clauses.
 *
 *************/

struct sk_group {
  char parent[520];   /* citation the skolemize node uses (leaf or _neg node) */
  char skname[540];   /* name of the skolemize node */
  int parent_id;      /* clause ID of the input formula (justification u.id) */
  Plist clauses;      /* Topforms sharing this parent, in proof order */
  BOOL skolemizing;   /* TRUE if some clause introduces a Skolem function */
  BOOL emitted;       /* skolemize node already printed */
};

static Plist Sk_groups = NULL;

static
BOOL term_has_skolem_function(Term t)
{
  int i;
  if (VARIABLE(t))
    return FALSE;
  if (is_skolem(SYMNUM(t)))
    return TRUE;
  for (i = 0; i < ARITY(t); i++)
    if (term_has_skolem_function(ARG(t,i)))
      return TRUE;
  return FALSE;
}

static
BOOL clause_has_skolem_function(Topform c)
{
  Term t = topform_to_term_without_attributes(c);
  BOOL sk = (t != NULL) ? term_has_skolem_function(t) : FALSE;
  if (t)
    zap_term(t);
  return sk;
}

static
void quote_tptp_name(char *buf, size_t sz, const char *nm)
{
  BOOL q = FALSE;
  const char *p;
  for (p = nm; *p; p++)
    if (*p=='(' || *p==')' || *p==',' || *p==' ') { q = TRUE; break; }
  if (q) snprintf(buf, sz, "'%s'", nm);
  else   snprintf(buf, sz, "%s", nm);
}

static
void make_sk_name(char *buf, size_t sz, const char *nm, BOOL deny)
{
  BOOL q = FALSE;
  const char *p;
  for (p = nm; *p; p++)
    if (*p=='(' || *p==')' || *p==',' || *p==' ') { q = TRUE; break; }
  if (q) snprintf(buf, sz, "'sk_%s%s'", nm, deny ? "_neg" : "");
  else   snprintf(buf, sz, "sk_%s%s", nm, deny ? "_neg" : "");
}

static
struct sk_group *find_sk_group(const char *parent)
{
  Plist p;
  for (p = Sk_groups; p; p = p->next) {
    struct sk_group *g = (struct sk_group *) p->v;
    if (strcmp(g->parent, parent) == 0)
      return g;
  }
  return NULL;
}

/* The Skolemizing group c belongs to, or NULL. */
static
struct sk_group *skolem_group_of(Topform c)
{
  Just_type jt = c->justification ? c->justification->type : UNKNOWN_JUST;
  char *tn;
  char parent[520];
  struct sk_group *g;
  if (jt != CLAUSIFY_JUST && jt != DENY_JUST)
    return NULL;
  tn = get_string_attribute(c->attributes, get_tptp_name_attr(), 1);
  if (!tn)
    return NULL;
  if (jt == DENY_JUST) make_neg_name(parent, sizeof(parent), tn);
  else                 quote_tptp_name(parent, sizeof(parent), tn);
  g = find_sk_group(parent);
  return (g && g->skolemizing) ? g : NULL;
}

static
void reset_sk_groups(void)
{
  Plist p;
  for (p = Sk_groups; p; p = p->next) {
    struct sk_group *g = (struct sk_group *) p->v;
    zap_plist(g->clauses);
    free(g);
  }
  zap_plist(Sk_groups);
  Sk_groups = NULL;
}

/* Group clausify/deny clauses by their (Skolemizing) parent. */
static
void build_sk_groups(Plist proof)
{
  Plist p;
  for (p = proof; p; p = p->next) {
    Topform c = (Topform) p->v;
    Just_type jt = c->justification ? c->justification->type : UNKNOWN_JUST;
    char *tn;
    char parent[520];
    struct sk_group *g;
    if (jt != CLAUSIFY_JUST && jt != DENY_JUST)
      continue;
    tn = get_string_attribute(c->attributes, get_tptp_name_attr(), 1);
    if (!tn)
      continue;
    if (jt == DENY_JUST) make_neg_name(parent, sizeof(parent), tn);
    else                 quote_tptp_name(parent, sizeof(parent), tn);
    g = find_sk_group(parent);
    if (g == NULL) {
      g = (struct sk_group *) safe_malloc(sizeof(struct sk_group));
      strncpy(g->parent, parent, sizeof(g->parent)-1);
      g->parent[sizeof(g->parent)-1] = '\0';
      make_sk_name(g->skname, sizeof(g->skname), tn, jt == DENY_JUST);
      g->parent_id = c->justification->u.id;
      g->clauses = NULL;
      g->skolemizing = FALSE;
      g->emitted = FALSE;
      Sk_groups = plist_append(Sk_groups, g);
    }
    g->clauses = plist_append(g->clauses, c);
    if (clause_has_skolem_function(c))
      g->skolemizing = TRUE;
  }
}

/* Collect fresh (Skolem/definition) symbols from a formula tree. */
static
void collect_fresh_symbols_formula(Formula f, Ilist *skolems, Ilist *defs)
{
  if (f == NULL)
    return;
  if (f->type == ATOM_FORM)
    collect_fresh_symbols(f->atom, skolems, defs);
  else {
    int i;
    for (i = 0; i < f->arity; i++)
      collect_fresh_symbols_formula(f->kids[i], skolems, defs);
  }
}

/* TRUE if f is ALREADY in CNF -- a conjunction (possibly under universals) of
   clauses -- so no distribution is needed and split_conjunct can cite the
   skolemize node directly.  FALSE only when cnf() would DISTRIBUTE (an OR over
   an AND), which needs a separate cnf_transformation (thm) step.  Robust to the
   variable-representation and parenthesization differences between the recorded
   Skolemized form and the clause-built conjunction, which formula_ident is not. */
static
BOOL formula_already_cnf(Formula f)
{
  if (f->type == ALL_FORM)
    return formula_already_cnf(f->kids[0]);
  if (f->type == AND_FORM) {
    int i;
    for (i = 0; i < f->arity; i++)
      if (!formula_already_cnf(f->kids[i]))
	return FALSE;
    return TRUE;
  }
  return clausal_formula(f);
}

/* Emit the skolemize node for group g, once.  The body is the input
   formula's COMPLETE clausification (recorded at clausify time), so that
   the conjunction entails the parent even when the proof uses only some
   of the clauses; the used-clause conjunction is a fallback. */
static
void emit_skolemize_node(FILE *fp, struct sk_group *g)
{
  Plist p, fs = NULL;
  Formula conj = NULL;    /* recorded full clausification (CNF) or fallback */
  Formula built = NULL;   /* owned fallback conjunction, if we build one */
  Formula skolem_form;    /* recorded Skolemized form (before CNF), or NULL */
  Ilist skolems = NULL, defs = NULL;
  if (g->emitted)
    return;
  conj = find_full_clausification(g->parent_id);
  if (conj == NULL) {
    for (p = g->clauses; p; p = p->next) {
      Topform c = (Topform) p->v;
      fs = plist_append(fs, universal_closure(clause_to_formula(c)));
    }
    built = formulas_to_conjunction(fs);
    conj = built;
  }
  collect_fresh_symbols_formula(conj, &skolems, &defs);
  skolem_form = find_full_clausification_skolem(g->parent_id);

  /* Preferred (CASC- and checker-compliant): emit the clausification pipeline
     as SEPARATE documented steps -- skolemize (esa, whose body is a PLAIN
     Skolemization a strict structural skolemize check can verify) then, when
     CNF distribution changed anything, cnf_transformation (thm).  This
     replaces the single collapsed clausify(esa) node that a CASC panel / GDV
     rejects and that proofcheck could only hedge.  Use the legacy collapsed
     path only when the Skolemized form was not recorded. */
  if (skolem_form != NULL) {
    Ilist sk_skolems = NULL, sk_defs = NULL;
    /* Definitional (Tseitin) predicates are introduced during distribution,
       so their presence means distribution changed something. */
    BOOL need_cnf = (conj != NULL &&
                     (defs != NULL || !formula_already_cnf(skolem_form)));
    char skname_sk[560], nnf_name[560];
    const char *sk_last;    /* node the split_conjunct clauses cite */
    const char *sk_parent;  /* node the skolemize step cites (the NNF) */
    Formula nnf_form = find_full_clausification_nnf(g->parent_id);

    collect_fresh_symbols_formula(skolem_form, &sk_skolems, &sk_defs);
    if (need_cnf) {
      snprintf(skname_sk, sizeof(skname_sk), "%s_sk", g->skname);
      sk_last = skname_sk;
    } else
      sk_last = g->skname;

    /* fof_nnf (thm): raw parent -> NNF.  A Skolemization matches the NNF of the
       parent (existentials explicit, =>/<=> eliminated), NOT the raw axiom, so
       emit the NNF as a thm step and point the skolemize step at it -- else any
       axiom needing NNF fails a checker's structural skolemize check. */
    if (nnf_form != NULL) {
      snprintf(nnf_name, sizeof(nnf_name), "nnf_%d", g->parent_id);
      fprintf(fp, "fof(%s, plain, ", nnf_name);
      fwrite_formula_tptp(fp, nnf_form);
      fprintf(fp, ", inference(fof_nnf, [status(thm)], [%s])).\n", g->parent);
      sk_parent = nnf_name;
    } else
      sk_parent = g->parent;

    /* skolemize (esa): the one equisatisfiable step, plain Skolemized body */
    fprintf(fp, "fof(%s, plain, ", sk_last);
    fwrite_formula_tptp(fp, skolem_form);
    fprintf(fp, ", inference(skolemize, [status(esa)");
    fprint_new_symbols(fp, "skolem", sk_skolems);
    /* skolemize(Var, Term) records: the existential-var -> Skolem-term map, so
       a strict checker can verify which variable each Skolem term eliminates. */
    {
      Plist sm = find_full_clausification_skmap(g->parent_id);
      Plist mp;
      for (mp = sm; mp; mp = mp->next) {
	fprintf(fp, ", ");
	fwrite_term_tptp(fp, (Term) mp->v);
      }
    }
    fprintf(fp, "], [%s])).\n", sk_parent);

    /* cnf_transformation (thm): equivalence-preserving CNF distribution.
       Definitional clauses are not consequences of the Skolemized form
       alone, so each introduced definition is emitted as an
       introduced(definition) leaf and cited as a co-parent, keeping the
       step thm. */
    if (need_cnf) {
      if (defs != NULL)
        emit_definition_leaves(fp, defs);
      fprintf(fp, "fof(%s, plain, ", g->skname);
      fwrite_formula_tptp(fp, conj);
      fprintf(fp, ", inference(cnf_transformation, [status(thm)], [%s",
	      sk_last);
      {
        Ilist q;
        for (q = defs; q; q = q->next)
          fprintf(fp, ", def_%s", sn_to_str(q->i));
      }
      fprintf(fp, "])).\n");
    }
    zap_ilist(sk_skolems);
    zap_ilist(sk_defs);
  } else {
    /* Legacy collapsed node: no recorded Skolem form. */
    fprintf(fp, "fof(%s, plain, ", g->skname);
    fwrite_formula_tptp(fp, conj);
    fprintf(fp, ", inference(clausify, [status(esa)");
    fprint_new_symbols(fp, "skolem", skolems);
    fprint_new_symbols(fp, "definition", defs);
    fprintf(fp, "], [%s])).\n", g->parent);
  }

  if (built != NULL) {
    zap_formula(built);
    zap_plist(fs);
  }
  zap_ilist(skolems);
  zap_ilist(defs);
  g->emitted = TRUE;
}

/*************
 *
 *   Introduced-definition citations.
 *
 *   Definitional CNF introduces fresh predicates defn_N naming
 *   subformulas.  A definitional clause is not a consequence of its
 *   parent formula alone (and neither esa direction holds without the
 *   definition present), so emit each definition as an
 *   introduced(definition) leaf, in the derivations-guide form a checker
 *   verifies structurally, and cite it as a co-parent; the clausify step
 *   is then a plain thm step.
 *
 *************/

static Ilist Emitted_defn_syms = NULL;   /* reset per proof */

static
void collect_defn_syms(Term t, Ilist *syms)
{
  int i;
  if (VARIABLE(t))
    return;
  if (find_introduced_definition(SYMNUM(t)) != NULL &&
      !ilist_member(*syms, SYMNUM(t)))
    *syms = ilist_append(*syms, SYMNUM(t));
  for (i = 0; i < ARITY(t); i++)
    collect_defn_syms(ARG(t,i), syms);
}

/* Definition predicates occurring in c that have recorded definitions. */
static
Ilist clause_defn_syms(Topform c)
{
  Ilist syms = NULL;
  Term t = topform_to_term_without_attributes(c);
  if (t) {
    collect_defn_syms(t, &syms);
    zap_term(t);
  }
  return syms;
}

/* Emit introduced(definition) leaves for the given definition symbols
   (once per proof each), registering them as declared fresh symbols. */
static
void emit_definition_leaves(FILE *fp, Ilist syms)
{
  Ilist q;
  for (q = syms; q; q = q->next) {
    char *nm;
    Formula defn;
    if (ilist_member(Emitted_defn_syms, q->i))
      continue;
    nm = sn_to_str(q->i);
    defn = find_introduced_definition(q->i);
    /* The stored definition can quantify clausify-renamed (x0-style)
       variables; rename them to valid TPTP variables before printing. */
    {
      I2list qm = NULL;
      rename_bad_qvars_formula(defn, &qm);
      if (qm != NULL)
        zap_i2list(qm);
    }
    fprintf(fp, "fof(def_%s, definition, ", nm);
    fwrite_formula_tptp(fp, defn);
    fprintf(fp,
            ", introduced(definition, [new_symbols(definition, [%s])], [])).\n",
            nm);
    Emitted_defn_syms = ilist_append(Emitted_defn_syms, q->i);
    if (!ilist_member(Declared_fresh_syms, q->i))
      Declared_fresh_syms = ilist_append(Declared_fresh_syms, q->i);
  }
}

/*************
 *
 *   tptp_problem_file()
 *
 *   Problem file name for file() source annotations, e.g. "PUZ001+1.p".
 *
 *************/

static
const char *tptp_problem_file(void)
{
  static char buf[512];
  if (Glob.problem_name) {
    snprintf(buf, sizeof(buf), "%s.p", Glob.problem_name);
    return buf;
  }
  return "unknown.p";
}

/*************
 *
 *   make_neg_name()
 *
 *   Build the assume_negation node name "<tptp_name>_neg" (quoted if the
 *   name contains TPTP-special characters), into buf.
 *
 *************/

static
void make_neg_name(char *buf, size_t bufsz, const char *tptp_name)
{
  BOOL needs_quote = FALSE;
  const char *p;
  for (p = tptp_name; *p; p++)
    if (*p == '(' || *p == ')' || *p == ',' || *p == ' ') {
      needs_quote = TRUE;
      break;
    }
  if (needs_quote)
    snprintf(buf, bufsz, "'%s_neg'", tptp_name);
  else
    snprintf(buf, bufsz, "%s_neg", tptp_name);
}

/*************
 *
 *   fprint_tptp_parents()
 *
 *   Print the comma-separated c_<id> parent list of a justification.
 *
 *************/

static
void fprint_tptp_parents(FILE *fp, Just just)
{
  Ilist parents = get_parents(just, TRUE);
  Ilist p;
  BOOL first = TRUE;
  for (p = parents; p; p = p->next) {
    if (!first)
      fprintf(fp, ", ");
    fprintf(fp, "c_%d", p->i);
    first = FALSE;
  }
  zap_ilist(parents);
}

/*************
 *
 *   fprint_clause_tptp()
 *
 *   Print one derivation node in TSTP format.  FOF input formulas (axioms
 *   and the conjecture) are emitted as named leaf nodes citing the problem
 *   file, so the clausify children have real parents and
 *   GDV can verify the full FOF-to-CNF derivation.  Clausify steps carry
 *   status(thm) when the clause is entailed by its parent formula, and
 *   status(esa) only when the clause introduces a Skolem symbol.
 *
 *************/

static
void fprint_clause_tptp(FILE *fp, Topform c, BOOL full_fof)
{
  Just just = c->justification;
  Just_type primary_type = just ? just->type : UNKNOWN_JUST;
  BOOL is_fof = c->is_formula;
  int tna = get_tptp_name_attr();
  char *tptp_name = get_string_attribute(c->attributes, tna, 1);
  char qname[512];

  (void) full_fof;   /* the full FOF derivation is always emitted now */

  qname[0] = '\0';

  /* TPTP names containing parens, commas, or spaces must be quoted. */
  if (tptp_name) {
    BOOL needs_quote = FALSE;
    char *p;
    for (p = tptp_name; *p && !needs_quote; p++) {
      if (*p == '(' || *p == ')' || *p == ',' || *p == ' ')
        needs_quote = TRUE;
    }
    if (needs_quote)
      snprintf(qname, sizeof(qname), "'%s'", tptp_name);
    else
      snprintf(qname, sizeof(qname), "%s", tptp_name);
  }

  /* Input FOF formula leaf.  Prover9 clausifies the input and clears the
     formula on the proof's placeholder node, but the original formula is
     still reachable from the ID table.  Emit it as a named FOF leaf citing
     the problem file, so that its clausify children have
     a real parent and GDV can verify the whole FOF-to-CNF derivation. */
  if (tptp_name &&
      (primary_type == INPUT_JUST || primary_type == GOAL_JUST)) {
    Topform orig = find_clause_by_id((int) c->id);
    if (orig == NULL && c->archive_materialized)
      orig = c;
    if (orig && orig->formula) {
      if (primary_type == GOAL_JUST) {
        /* Conjecture leaf, followed by its assume_negation node.  The
           negated-conjecture clauses cite <name>_neg (see DENY_JUST below),
           not the conjecture, so the conjecture is consumed only through
           assume_negation and the negation node carries the literal
           ~(conjecture) -- what GDV and proofcheck require. */
        char nqname[520];
        make_neg_name(nqname, sizeof(nqname), tptp_name);
        fprintf(fp, "fof(%s, conjecture, ", qname);
        fwrite_formula_tptp(fp, orig->formula);
        fprintf(fp, ", file('%s',%s)).\n", tptp_problem_file(), qname);
        fprintf(fp, "fof(%s, negated_conjecture, ~(", nqname);
        fwrite_formula_tptp(fp, orig->formula);
        fprintf(fp, "), inference(assume_negation, [status(cth)], [%s])).\n",
                qname);
      }
      else {
        fprintf(fp, "fof(%s, axiom, ", qname);
        fwrite_formula_tptp(fp, orig->formula);
        fprintf(fp, ", file('%s',%s)).\n", tptp_problem_file(), qname);
      }
      return;
    }
  }

  /* Skip $false input leaves: the empty clause cannot be a legitimate
     input axiom or goal.  These arise when fastPE resolves clauses
     during preprocessing before the search starts. */
  if (c->literals == NULL &&
      (primary_type == INPUT_JUST || primary_type == GOAL_JUST))
    return;

  /* Clausal fof axiom (marked at input time): clausify() was a no-op, so the
     clause would otherwise be a bare cnf leaf citing the file -- an
     UNDOCUMENTED fof-to-cnf translation a CASC panel / proofcheck rejects.
     Emit a fof leaf + a clausify(thm) step instead, so the (trivial)
     clausification is documented and verifiable. */
  if (primary_type == INPUT_JUST && tptp_name && !is_fof &&
      c->literals != NULL &&
      get_int_attribute(c->attributes, get_clausal_fof_attr(), 1) == 1) {
    Formula ff = universal_closure(clause_to_formula(c));
    Term t = topform_to_term_without_attributes(c);
    fprintf(fp, "fof(%s, axiom, ", qname);
    fwrite_formula_tptp(fp, ff);
    fprintf(fp, ", file('%s',%s)).\n", tptp_problem_file(), qname);
    fprintf(fp, "cnf(c_%llu, plain, ", (unsigned long long) c->id);
    if (t) {
      tptp_quote_bad_syms(t);
      fwrite_term_tptp(fp, t);
      zap_term(t);
    }
    fprintf(fp, ", inference(clausify, [status(thm)], [%s])).\n", qname);
    zap_formula(ff);
    return;
  }

  /* A residual FOF formula node still carrying its formula is emitted as a
     named leaf too (the placeholder path above normally handles inputs). */
  BOOL fof_leaf = is_fof && tptp_name != NULL &&
                  (primary_type == INPUT_JUST || primary_type == GOAL_JUST);

  /* Print: fof(<name>| c_ID, role, */
  if (fof_leaf)
    fprintf(fp, "fof(%s, ", qname);
  else
    fprintf(fp, "%s(c_%llu, ", is_fof ? "fof" : "cnf", c->id);

  /* Determine role.  An FOF conjecture keeps role conjecture; its negated
     CNF form and any goal-derived clause is negated_conjecture. */
  if (is_fof && primary_type == GOAL_JUST)
    fprintf(fp, "conjecture, ");
  else if (primary_type == DENY_JUST || c->goal_derived)
    fprintf(fp, "negated_conjecture, ");
  else if (primary_type == INPUT_JUST)
    fprintf(fp, "axiom, ");
  else
    fprintf(fp, "plain, ");

  /* Print the formula/clause body. */
  if (is_fof && c->formula != NULL) {
    fwrite_formula_tptp(fp, c->formula);
  }
  else {
    Term t = topform_to_term_without_attributes(c);
    if (c->literals == NULL)
      fprintf(fp, "$false");
    else {
      tptp_quote_bad_syms(t);
      fwrite_term_tptp(fp, t);
    }
    if (t)
      zap_term(t);
  }

  /* Print the source / inference annotation. */
  if (fof_leaf) {
    /* Input formula leaf: cite the problem file. */
    fprintf(fp, ", file('%s',%s)).\n", tptp_problem_file(), qname);
  }
  else if (primary_type == INPUT_JUST || primary_type == GOAL_JUST) {
    /* CNF input clause (no clausification): a leaf citing the problem. */
    if (tptp_name)
      fprintf(fp, ", file('%s',%s)).\n", tptp_problem_file(), qname);
    else if (primary_type == GOAL_JUST)
      fprintf(fp, ", introduced(conjecture,[],[])).\n");
    else
      fprintf(fp, ", introduced(assumption,[],[])).\n");
  }
  else if (primary_type == CLAUSIFY_JUST) {
    /* If this clause is part of a Skolemizing group, its skolemize node was
       already emitted; print it as a split_conjunct (thm) of that node. */
    struct sk_group *skg = skolem_group_of(c);
    if (skg != NULL) {
      fprintf(fp, ", inference(split_conjunct, [status(thm)], [%s])).\n",
              skg->skname);
      return;
    }
    /* A definitional clause cites its introduced(definition) leaves as
       co-parents and is then a plain thm step. */
    if (tptp_name) {
      Ilist ds = clause_defn_syms(c);
      if (ds != NULL) {
        Ilist q;
        fprintf(fp, ", inference(clausify, [status(thm)], [%s", qname);
        for (q = ds; q; q = q->next)
          fprintf(fp, ", def_%s", sn_to_str(q->i));
        fprintf(fp, "])).\n");
        zap_ilist(ds);
        return;
      }
    }
    /* FOF-to-CNF: status(thm) when the clause is a logical consequence of
       its parent formula, status(esa) when it carries a Skolem witness. */
    const char *st = clause_has_skolem(c) ? "esa" : "thm";
    if (tptp_name) {
      fprintf(fp, ", inference(clausify, ");
      fprint_clausify_status(fp, c, st);
      fprintf(fp, ", [%s])).\n", qname);
    }
    else {
      fprintf(fp, ", inference(clausify, ");
      fprint_clausify_status(fp, c, st);
      fprintf(fp, ", [");
      fprint_tptp_parents(fp, just);
      fprintf(fp, "])).\n");
    }
  }
  else if (primary_type == DENY_JUST) {
    /* Negated conjecture clause: the clausification (possibly Skolemized)
       of the assume_negation node <conjecture>_neg, NOT of the conjecture
       itself.  Citing the conjecture directly would trip a checker's
       conjecture-as-parent / body-mismatch guards; citing the negation
       node keeps this a normal clausify step.  status(thm) unless it
       introduces a Skolem witness. */
    struct sk_group *skg = skolem_group_of(c);
    if (skg != NULL) {
      fprintf(fp, ", inference(split_conjunct, [status(thm)], [%s])).\n",
              skg->skname);
      return;
    }
    if (tptp_name) {
      Ilist ds = clause_defn_syms(c);
      if (ds != NULL) {
        char nqname[520];
        Ilist q;
        make_neg_name(nqname, sizeof(nqname), tptp_name);
        fprintf(fp, ", inference(clausify, [status(thm)], [%s", nqname);
        for (q = ds; q; q = q->next)
          fprintf(fp, ", def_%s", sn_to_str(q->i));
        fprintf(fp, "])).\n");
        zap_ilist(ds);
        return;
      }
    }
    const char *st = clause_has_skolem(c) ? "esa" : "thm";
    if (tptp_name) {
      char nqname[520];
      make_neg_name(nqname, sizeof(nqname), tptp_name);
      fprintf(fp, ", inference(clausify, ");
      fprint_clausify_status(fp, c, st);
      fprintf(fp, ", [%s])).\n", nqname);
    }
    else {
      fprintf(fp, ", inference(clausify, ");
      fprint_clausify_status(fp, c, st);
      fprintf(fp, ", [");
      fprint_tptp_parents(fp, just);
      fprintf(fp, "])).\n");
    }
  }
  else {
    /* Ordinary inference: inference(rule, [status(thm)], [parents]) */
    const char *rule = tptp_rule_name(primary_type);
    fprintf(fp, ",\n    inference(%s, [status(thm)], [", rule);
    fprint_tptp_parents(fp, just);
    fprintf(fp, "])).\n");
  }
}  /* fprint_clause_tptp */

/*************
 *
 *   fprint_proof_tptp()
 *
 *   Print a complete proof in TSTP format with SZS output delimiters.
 *
 *************/

/* --- Output-only Skolem-symbol rename (c-N / f-N  ->  sK-N) ----------------
   proofcheck/GDV recognize Skolem symbols by NAME pattern (esk, sK, sF
   prefixes), not the new_symbols() declaration, and reject Prover9's c-N/f-N
   Skolem names.  is_skolem()
   is a symbol PROPERTY, so we repoint the proof's terms to fresh sK<n> symbols
   (also marked Skolem) for the TSTP output ONLY -- the prover's Skolem
   generation and the LADR-format proof output are untouched. */

static void collect_sk_syms_term(Term t, I2list *info)
{
  int i;
  if (VARIABLE(t)) return;
  if (is_skolem(SYMNUM(t)) && assoc(*info, SYMNUM(t)) == INT_MIN)
    *info = i2list_append(*info, SYMNUM(t), ARITY(t));
  for (i = 0; i < ARITY(t); i++)
    collect_sk_syms_term(ARG(t,i), info);
}

static void repoint_sk_term(Term t, I2list map)
{
  int i, ns;
  if (t == NULL || VARIABLE(t)) return;
  ns = assoc(map, SYMNUM(t));
  if (ns != INT_MIN)
    t->private_symbol = -ns;
  for (i = 0; i < ARITY(t); i++)
    repoint_sk_term(ARG(t,i), map);
}

static void repoint_sk_formula(Formula f, I2list map)
{
  int i;
  if (f == NULL) return;
  if (f->type == ATOM_FORM)
    repoint_sk_term(f->atom, map);
  else
    for (i = 0; i < f->arity; i++)
      repoint_sk_formula(f->kids[i], map);
}

/* --- Output-only bound-variable rename (x<n> -> fresh upper word) ----------
   unique_quantified_vars() (cnf.c) renames clashing quantified variables to
   x0, x1, ... during clausification, and those names survive in the recorded
   NNF / Skolemized forms, the skolemize(Var,Term) records, and introduced
   definitions.  A lower_word is not a TPTP <variable>, so an emitted fof
   body quantifying one would be ill-formed.  Rename each such variable to a
   fresh upper word (X<n>), consistently across a group's recorded forms, at
   TSTP-emit time only. */

/* TRUE if s is a syntactically valid TPTP variable: [A-Z][a-zA-Z0-9_]* */
static
BOOL tptp_valid_var_name(const char *s)
{
  int k;
  if (s == NULL || !(s[0] >= 'A' && s[0] <= 'Z'))
    return FALSE;
  for (k = 1; s[k]; k++) {
    char c = s[k];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_'))
      return FALSE;
  }
  return TRUE;
}

/* The (fresh upper word) rename target for old_sn, added to *map if new. */
static
int bad_qvar_target(I2list *map, int old_sn)
{
  int ns = assoc(*map, old_sn);
  if (ns == INT_MIN) {
    char nm[32];
    int n = 0;
    do { snprintf(nm, sizeof(nm), "X%d", n++); } while (str_exists(nm));
    ns = str_to_sn(nm, 0);
    *map = i2list_append(*map, old_sn, ns);
  }
  return ns;
}

/* Rename TPTP-invalid quantified variables of f to fresh upper words,
   extending *map (old SYMNUM -> new SYMNUM) so the same variable renames
   identically across the recorded forms it appears in.  Quantified names
   are unique within a recorded formula (unique_quantified_vars) and
   globally fresh, so occurrences can be repointed by symbol alone. */
static
void rename_bad_qvars_formula(Formula f, I2list *map)
{
  int i;
  if (f == NULL)
    return;
  if (f->type == ATOM_FORM) {
    repoint_sk_term(f->atom, *map);
    return;
  }
  if (quant_form(f) && !tptp_valid_var_name(f->qvar)) {
    int old_sn = str_to_sn(f->qvar, 0);
    f->qvar = sn_to_str(bad_qvar_target(map, old_sn));
  }
  for (i = 0; i < f->arity; i++)
    rename_bad_qvars_formula(f->kids[i], map);
}

static void collect_sk_syms_formula(Formula f, I2list *info)
{
  int i;
  if (f == NULL) return;
  if (f->type == ATOM_FORM)
    collect_sk_syms_term(f->atom, info);
  else
    for (i = 0; i < f->arity; i++)
      collect_sk_syms_formula(f->kids[i], info);
}

/* Allocate a fresh sK<n> for each Skolem symbol in the proof; return the
   orig-SYMNUM -> new-SYMNUM map (caller zaps), or NULL if none.  Symbols
   are collected from the proof clauses AND from the groups' recorded
   Skolemized / complete-clausification forms and skolemize(Var,Term)
   records: the skolemize node body is the COMPLETE clausification, so it
   can contain Skolem symbols that occur in no proof clause, and an
   unrenamed one would print under its internal c<n>/f<n> name --
   indistinguishable from a problem constant. */
static I2list build_skolem_rename(Plist expanded)
{
  I2list info = NULL, map = NULL, e;
  Plist p, g;
  int n = 0;
  for (p = expanded; p; p = p->next) {
    Topform c = (Topform) p->v;
    Literals lit;
    for (lit = c->literals; lit != NULL; lit = lit->next)
      collect_sk_syms_term(lit->atom, &info);
  }
  for (g = Sk_groups; g; g = g->next) {
    struct sk_group *grp = (struct sk_group *) g->v;
    Plist mp;
    collect_sk_syms_formula(find_full_clausification_skolem(grp->parent_id),
			    &info);
    collect_sk_syms_formula(find_full_clausification(grp->parent_id), &info);
    for (mp = find_full_clausification_skmap(grp->parent_id); mp; mp = mp->next)
      collect_sk_syms_term((Term) mp->v, &info);
  }
  if (info == NULL)
    return NULL;
  for (e = info; e; e = e->next) {
    char nm[32];
    int newsn;
    do { snprintf(nm, sizeof(nm), "sK%d", n++); } while (str_exists(nm));
    newsn = str_to_sn(nm, e->j);   /* e->i = orig SYMNUM, e->j = arity */
    set_skolem(newsn);
    map = i2list_append(map, e->i, newsn);
  }
  zap_i2list(info);
  return map;
}

static
void fprint_proof_tptp(FILE *fp, Plist proof)
{
  Plist p;
  Variable_style orig_style = variable_style();

  /* Reset the per-proof set of already-declared fresh (Skolem/definition)
     symbols, so new_symbols() declarations are emitted exactly once. */
  zap_ilist(Declared_fresh_syms);
  Declared_fresh_syms = NULL;
  zap_ilist(Emitted_defn_syms);
  Emitted_defn_syms = NULL;
  reset_sk_groups();

  if (Glob.problem_name)
    fprintf(fp, "%% SZS output start CNFRefutation for %s\n", Glob.problem_name);
  else
    fprintf(fp, "%% SZS output start CNFRefutation\n");

  /* Expand compound justifications into separate steps.
     This unrolls resolve+rewrite+unit_del chains so that each TSTP
     inference step has a single operation with correct intermediate
     clause bodies.  Without this, back-demodulated parent bodies
     make compound steps un-verifiable. */
  I3list jmap = NULL;
  Plist expanded = expand_proof(proof, &jmap);
  if (expanded == NULL)
    /* Replay failure (see expand_proof): print the original steps.
       Compound justifications stay collapsed, but the output is complete
       and well-formed. */
    expanded = proof;

  /* Group clausify/deny clauses by their Skolemizing parent, so each such
     group is emitted as one skolemize step + split_conjunct steps. */
  build_sk_groups(expanded);

  /* Rename Prover9 Skolem symbols (c-N / f-N) to the checker-recognized sK-N
     convention -- TSTP output ONLY.  Repoint the clause terms and the recorded
     intermediate forms (skolemize-step body, cnf body, skolemize(Var,Term)
     records); is_skolem() stays true on the new symbols so new_symbols() still
     declares them. */
  {
    I2list sk_map = build_skolem_rename(expanded);
    if (sk_map != NULL) {
      Plist g;
      for (p = expanded; p; p = p->next) {
	Topform c = (Topform) p->v;
	Literals lit;
	for (lit = c->literals; lit != NULL; lit = lit->next)
	  repoint_sk_term(lit->atom, sk_map);
      }
      for (g = Sk_groups; g; g = g->next) {
	struct sk_group *grp = (struct sk_group *) g->v;
	Plist mp;
	repoint_sk_formula(find_full_clausification_skolem(grp->parent_id), sk_map);
	repoint_sk_formula(find_full_clausification(grp->parent_id), sk_map);
	for (mp = find_full_clausification_skmap(grp->parent_id); mp; mp = mp->next)
	  repoint_sk_term((Term) mp->v, sk_map);
      }
      /* Introduced definitions can name subformulas containing Skolem
	 terms (definitional renaming runs after Skolemization), so their
	 recorded bodies need the same repointing.  Their symbols are
	 collected from each group's recorded full clausification: a proof
	 can cite a definition leaf whose predicate occurs in no proof
	 clause (the complete-clausification cnf body cites it). */
      for (g = Sk_groups; g; g = g->next) {
	struct sk_group *grp = (struct sk_group *) g->v;
	Ilist sks = NULL, dfs = NULL, q;
	collect_fresh_symbols_formula(find_full_clausification(grp->parent_id),
				      &sks, &dfs);
	for (q = dfs; q; q = q->next)
	  repoint_sk_formula(find_introduced_definition(q->i), sk_map);
	zap_ilist(sks);
	zap_ilist(dfs);
      }
      /* Definitional clauses outside a Skolemizing group cite definition
	 leaves too; collect their symbols from the proof clauses. */
      for (p = expanded; p; p = p->next) {
	Ilist ds = clause_defn_syms((Topform) p->v);
	Ilist q;
	for (q = ds; q; q = q->next)
	  repoint_sk_formula(find_introduced_definition(q->i), sk_map);
	zap_ilist(ds);
      }
      zap_i2list(sk_map);
    }
  }

  /* Rename TPTP-invalid bound variable names (x0-style, introduced by
     unique_quantified_vars) in the recorded NNF / Skolemized forms and
     skolemize(Var,Term) records to fresh upper words.  One map across
     groups: the generated names are globally fresh, so a shared old name
     denotes the same interned symbol wherever it appears. */
  {
    I2list qmap = NULL;
    Plist g;
    for (g = Sk_groups; g; g = g->next) {
      struct sk_group *grp = (struct sk_group *) g->v;
      Plist mp;
      rename_bad_qvars_formula(find_full_clausification_nnf(grp->parent_id), &qmap);
      rename_bad_qvars_formula(find_full_clausification_skolem(grp->parent_id), &qmap);
      for (mp = find_full_clausification_skmap(grp->parent_id); mp; mp = mp->next)
	repoint_sk_term((Term) mp->v, qmap);
    }
    if (qmap != NULL)
      zap_i2list(qmap);
  }

  /* Detect whether the proof contains FOF entries (axioms or conjectures).
     When it does, fprint_clause_tptp emits the full FOF-to-CNF derivation:
     the FOF input formulas appear as named leaf nodes citing the problem
     file, and their clausify children cite them with the
     correct SZS status (thm, or esa for Skolemization).  This gives GDV a
     complete, verifiable derivation rather than dangling parent names. */
  BOOL has_fof = FALSE;
  for (p = expanded; p && !has_fof; p = p->next) {
    Topform c = (Topform) p->v;
    if (c->is_formula && c->justification &&
	(c->justification->type == INPUT_JUST ||
	 c->justification->type == GOAL_JUST))
      has_fof = TRUE;
  }

  for (p = expanded; p; p = p->next) {
    Topform c = (Topform) p->v;
    /* If c belongs to a Skolemizing group, emit that group's skolemize node
       (once, in FOF variable style) before the clause; the clause then prints
       as a split_conjunct of it.  Otherwise, if c is a definitional clausify
       step, emit its introduced(definition) leaves (once each) first. */
    struct sk_group *skg = skolem_group_of(c);
    if (skg != NULL && !skg->emitted) {
      set_variable_style(orig_style);
      emit_skolemize_node(fp, skg);
    }
    else if (skg == NULL && c->justification &&
             (c->justification->type == CLAUSIFY_JUST ||
              c->justification->type == DENY_JUST)) {
      Ilist ds = clause_defn_syms(c);
      if (ds != NULL) {
        set_variable_style(orig_style);
        emit_definition_leaves(fp, ds);
        zap_ilist(ds);
      }
    }
    /* An input/goal placeholder whose original formula is still in the ID
       table is printed as an FOF leaf, so it needs FOF (named) variable
       style; genuine CNF clauses use PROLOG_STYLE for uppercase variables. */
    Topform orig = find_clause_by_id((int) c->id);
    if (orig == NULL && c->archive_materialized)
      orig = c;
    BOOL fof_out = c->is_formula ||
                   (orig != NULL && orig->formula != NULL && c->justification &&
                    (c->justification->type == INPUT_JUST ||
                     c->justification->type == GOAL_JUST));
    if (fof_out)
      set_variable_style(orig_style);
    else
      set_variable_style(PROLOG_STYLE);
    fprint_clause_tptp(fp, c, has_fof);
  }

  reset_sk_groups();
  if (expanded != proof)
    delete_clauses(expanded);
  zap_i3list(jmap);
  set_variable_style(orig_style);
  if (Glob.problem_name)
    fprintf(fp, "%% SZS output end CNFRefutation for %s\n", Glob.problem_name);
  else
    fprintf(fp, "%% SZS output end CNFRefutation\n");
}  /* fprint_proof_tptp */

/*************
 *
 *   print_exit_message()
 *
 *   Print the exit/status message to fp and flush, but do NOT
 *   call exit().  Used by child_exit() in fork children.
 *
 *************/

/* PUBLIC */
/* One-SZS-status-line guard, shared with the async signal handlers
   (provers.c): whoever prints or write()s a status line first sets it,
   and everyone else checks it, so an interrupted run never emits two
   status lines (competition graders require exactly one). */
volatile sig_atomic_t Szs_line_written = 0;

void print_exit_message(FILE *fp, int code)
{
  int proofs = Glob.initialized ? Stats.proofs : -1;

  if (Opt && flag(Opt->tptp_output)) {
    /* TPTP/SZS output mode */
    if (!Szs_line_written) {
      Szs_line_written = 1;
      if (Glob.problem_name)
        fprintf(fp, "\n%% SZS status %s for %s\n",
	        szs_status_string(code), Glob.problem_name);
      else
        fprintf(fp, "\n%% SZS status %s\n", szs_status_string(code));
    }

    if (!Opt || !flag(Opt->quiet)) {
      fflush(stdout);
      fprintf(stderr, "\n------ process %d exit (%s) ------\n",
	      my_process_id(), exit_string(code));
      if (!Opt || flag(Opt->bell))
        bell(stderr);
    }

    if (Opt && parm(Opt->report_stderr) > 0)
      report(stderr, "some");

    /* Print memory logging summary if logging was enabled */
    if (!flag(Opt->quiet))
      memory_logging_summary(stderr);

    fflush(fp);
    fflush(stderr);
    return;
  }

  /* Native Prover9 output mode */
  if (proofs == -1)
    fprintf(fp, "\nExiting.\n");
  else if (proofs == 0)
    fprintf(fp, "\nExiting with failure.\n");
  else
    fprintf(fp, "\nExiting with %d proof%s.\n",
	    proofs, proofs == 1 ? "" : "s");

  if (!Opt || !flag(Opt->quiet)) {
    fflush(stdout);
    fprintf(stderr, "\n------ process %d exit (%s) ------\n",
	    my_process_id(), exit_string(code));
    if (!Opt || flag(Opt->bell))
      bell(stderr);
  }

  if (Opt && parm(Opt->report_stderr) > 0)
    report(stderr, "some");

  fprintf(fp, "\nProcess %d exit (%s) %s",
	  my_process_id(), exit_string(code), get_date());

  /* Print memory logging summary if logging was enabled */
  if (!Opt || !flag(Opt->quiet))
    memory_logging_summary(stderr);

  fflush(fp);
  fflush(stderr);
}  /* print_exit_message */

/*************
 *
 *   exit_with_message()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void exit_with_message(FILE *fp, int code)
{
  set_no_kill();  /* protect exit output from signal truncation */
  print_exit_message(fp, code);
  exit(code);
}  /* exit_with_message */

/*************
 *
 *   report()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void report(FILE *fp, char *level)
{
  double seconds = user_seconds();
  static unsigned long long prev_query_special = 0;
  static unsigned long long prev_intersect_merge = 0;
  static unsigned long long prev_given = 0;
  static unsigned long long prev_collective_raw[2] = {0, 0};
  static unsigned long long prev_collective_generated[2] = {0, 0};
  static unsigned long long prev_collective_hint_selected[2] = {0, 0};
  static unsigned long long prev_collective_distinct_hints[2] = {0, 0};

  if (fp != stderr)
    fprintf(fp, "\nNOTE: Report at %.2f seconds, %s", seconds, get_date());

  if (str_ident(level, ""))
    level = (Opt ? stringparm1(Opt->stats) : "lots");

  fprint_all_stats(fp, level);

  if (collective_frontier_mode()) {
    int slot = fp == stderr ? 1 : 0;
    fprintf(fp,
            "Collective_interval: raw_seen=+%s, generated=+%s, "
            "hint_selected=+%s, distinct_hints=+%s.\n",
            comma_num(Stats.collective_raw_candidates_seen -
                      prev_collective_raw[slot]),
            comma_num(Stats.generated - prev_collective_generated[slot]),
            comma_num(Stats.collective_hint_selected_total -
                      prev_collective_hint_selected[slot]),
            comma_num(Stats.collective_distinct_hints_matched -
                      prev_collective_distinct_hints[slot]));
    prev_collective_raw[slot] = Stats.collective_raw_candidates_seen;
    prev_collective_generated[slot] = Stats.generated;
    prev_collective_hint_selected[slot] =
      Stats.collective_hint_selected_total;
    prev_collective_distinct_hints[slot] =
      Stats.collective_distinct_hints_matched;
  }

  /* Print FPA index statistics if requested */
  if (flag(Opt->report_index_stats)) {
    unsigned long long qs = fpa_query_special_calls();
    unsigned long long im = fpa_intersect_merge_ops();
    unsigned long long g = Stats.given;
    unsigned long long delta_qs = qs - prev_query_special;
    unsigned long long delta_im = im - prev_intersect_merge;
    unsigned long long delta_g = g - prev_given;

    fprintf(fp, "\nFPA index stats: query_special=%s", comma_num(qs));
    fprintf(fp, " (+%s)", comma_num(delta_qs));
    fprintf(fp, ", intersect_merge=%s", comma_num(im));
    fprintf(fp, " (+%s)", comma_num(delta_im));
    if (delta_g > 0) {
      fprintf(fp, ", per_given: qs=%.1f, im=%.1f",
              (double)delta_qs / delta_g, (double)delta_im / delta_g);
    }
    fprintf(fp, "\n");

    prev_query_special = qs;
    prev_intersect_merge = im;
    prev_given = g;
  }

  if (!flag(Opt->quiet) && fp != stderr) {
    fflush(stdout);
  }
  fflush(fp);
  fflush(stderr);
}  /* report */

/*************
 *
 *   possible_report()
 *
 *************/

static
void possible_report(void)
{
  static int Next_report, Next_report_stderr;
  static unsigned long long Next_report_given;
  int runtime;

  runtime = user_time() / 1000;

  if (parm(Opt->report) > 0) {
    if (Next_report == 0)
      Next_report = parm(Opt->report);
    if (runtime >= Next_report) {
      report(stdout, stringparm1(Opt->stats));
      while (runtime >= Next_report)
	Next_report += parm(Opt->report);
    }
  }

  if (parm(Opt->report_stderr) > 0) {
    if (Next_report_stderr == 0)
      Next_report_stderr = parm(Opt->report_stderr);
    if (runtime >= Next_report_stderr) {
      report(stderr, "some");
      while (runtime >= Next_report_stderr)
	Next_report_stderr += parm(Opt->report_stderr);
    }
  }

  if (parm(Opt->report_given) > 0) {
    if (Next_report_given == 0)
      Next_report_given = parm(Opt->report_given);
    if (Stats.given >= Next_report_given) {
      report(stdout, stringparm1(Opt->stats));
      while (Stats.given >= Next_report_given)
	Next_report_given += parm(Opt->report_given);
    }
  }
}  /* possible_report */

/*************
 *
 *   done_with_search()
 *
 *************/

static void freeze_terminal_statistics(void)
{
  if (Terminal_stats_frozen)
    return;
  if (flag(Opt->tptp_output))
    update_stats();
  else {
    Deferred_terminal_stats = tmpfile();
    if (Deferred_terminal_stats == NULL)
      fatal_error("freeze_terminal_statistics: cannot create temporary report");
    fprint_all_stats(Deferred_terminal_stats,
                     Opt ? stringparm1(Opt->stats) : "lots");
    rewind(Deferred_terminal_stats);
  }
  Terminal_stats_frozen = TRUE;
}

static void print_deferred_terminal_statistics(FILE *fp)
{
  char buffer[8192];
  size_t n;
  if (Deferred_terminal_stats == NULL)
    return;
  while ((n = fread(buffer, 1, sizeof(buffer),
                    Deferred_terminal_stats)) != 0)
    if (fwrite(buffer, 1, n, fp) != n)
      fatal_error("print_deferred_terminal_statistics: write failed");
  if (ferror(Deferred_terminal_stats))
    fatal_error("print_deferred_terminal_statistics: read failed");
  fclose(Deferred_terminal_stats);
  Deferred_terminal_stats = NULL;
  fflush(fp);
}

static void compact_passive_cache_free(void);

static void release_terminal_compact_indexes(void)
{
  if (Terminal_compact_indexes_released || !compact_otter_passive_mode())
    return;
  /* Freeze has already captured the final SOS statistics.  No still-passive
     clause can be an ancestor of the terminal empty clause, so its dense
     selector record and heap entries are search-only at this point.  Drop
     them before materializing the proof DAG instead of overlapping roughly
     ten MiB of passive metadata with thousands of proof clauses. */
  compact_passive_cache_free();
  reset_selector_indexes();
  cold_passive_store_free(Dense_body_store);
  Dense_body_store = NULL;
  Dense_arena_bytes_reclaimed = 0;
  destroy_demodulation_index();
  compact_rewrite_free(Compact_rewrite_rules);
  Compact_rewrite_rules = NULL;
  destroy_literals_index();
  destroy_back_demod_index();
  compact_term_pool_free(Compact_terms);
  Compact_terms = NULL;
  memory_release_unused();
  Terminal_compact_indexes_released = TRUE;
}

static void release_terminal_hint_index(void)
{
  Clist_pos p;
  if (Terminal_hint_index_released || Glob.hints == NULL)
    return;
  /* The archive stores a stable matching-hint ID, and the caller has just
     restored those IDs to pointers into Glob.hints.  The search is terminal,
     so no further candidate needs hint matching or hint back demodulation.
     Keep the hint clauses themselves for proof printing and result
     collection, but discard the search-only index before materializing the
     proof DAG.  collect_prover_results() can restore links again with the
     retained, linear ID lookup and does not require this index. */
  for (p = Glob.hints->first; p != NULL; p = p->next)
    unindex_hint(p->c);
  done_with_hints();
  memory_release_unused();
  Terminal_hint_index_released = TRUE;
}

static
void done_with_search(int return_code)
{
  if (Deferred_terminal_stats != NULL)
    print_deferred_terminal_statistics(stdout);
  else if (!Terminal_stats_frozen)
    fprint_all_stats(stdout, Opt ? stringparm1(Opt->stats) : "lots");
  /* If we need to return 0, we have to encode it as something else. */
  longjmp(Jump_env, return_code == 0 ? INT_MAX : return_code);
}  /* done_with_search */

/*************
 *
 *   exit_if_over_limit()
 *
 *************/

static
void exit_if_over_limit(void)
{
  /* max_megs is handled elsewhere */
  /* max_seconds is handled by SIGALRM (setup_timeout_signal) */

  if (over_parm_limit(Stats.kept, Opt->max_kept))
    done_with_search(MAX_KEPT_EXIT);
  else if (over_parm_limit(Stats.given, Opt->max_given))
    done_with_search(MAX_GIVEN_EXIT);
}  /* exit_if_over_limit */

/*************
 *
 *   inferences_to_make()
 *
 *************/

static
BOOL inferences_to_make(void)
{
  return givens_available() ||
         (collective_frontier_mode() &&
          (Collective_batch_head != NULL ||
           Collective_candidate_heap_count != 0));
}  // inferences_to_make

/*************
 *
 *   index_clashable()
 *
 *   Insert/delete a clause into/from resolution index.
 *
 *************/

static
void index_clashable(Topform c, Indexop operation)
{
  if (Glob.use_clash_idx) {
    clock_start(Clocks.index);
    lindex_update(Glob.clashable_idx, c, operation);
    clock_stop(Clocks.index);
  }
}  /* index_clashable */

/*************
 *
 *   restricted_denial()
 *
 *************/

static
BOOL restricted_denial(Topform c)
{
  /* At one time we also required all clauses to be Horn. */
  return
    flag(Opt->restrict_denials) &&
    negative_clause(c->literals);
}  /* restricted_denial */

static
BOOL active_or_indexable_clause(Topform c)
{
  return (Glob.usable != NULL && clist_member(c, Glob.usable)) ||
         (Glob.sos != NULL && clist_member(c, Glob.sos)) ||
         (Glob.limbo != NULL && clist_member(c, Glob.limbo)) ||
         (Glob.demods != NULL && clist_member(c, Glob.demods)) ||
         (Glob.hints != NULL && clist_member(c, Glob.hints));
}  /* active_or_indexable_clause */

static
Topform hint_by_id(unsigned long long id)
{
  Clist_pos p;
  if (id == 0 || Glob.hints == NULL)
    return NULL;
  for (p = Glob.hints->first; p != NULL; p = p->next)
    if (p->c->id == id)
      return p->c;
  return NULL;
}

static
void restore_archive_hint_links(Plist clauses)
{
  Plist p;
  for (p = clauses; p != NULL; p = p->next) {
    Topform c = p->v;
    if (c->archive_materialized) {
      unsigned long long id = clause_store_matching_hint_id(c->id);
      c->matching_hint = hint_by_id(id);
    }
  }
}

/* Return the same first negative proof clause that first_negative_clause()
   would see in the ID-sorted materialized DAG, but consult only resident or
   archived parent/negative metadata.  Terminal compact runs use this before
   releasing search-only indexes, so proof reconstruction cannot overlap
   those indexes merely to decide denial reuse. */
static unsigned first_negative_ancestor_id(Topform root)
{
  unsigned char *seen;
  unsigned *stack;
  size_t count = 0, capacity = 1024;
  unsigned result = 0;
  if (root == NULL || root->id == 0 || root->id > UINT_MAX)
    fatal_error("first_negative_ancestor_id: invalid proof root");
  seen = safe_calloc((size_t) root->id + 1, sizeof(*seen));
  stack = safe_malloc(capacity * sizeof(*stack));
  stack[count++] = (unsigned) root->id;
  while (count != 0) {
    unsigned id = stack[--count];
    Ilist parents, p;
    BOOL known;
    if (id == 0 || id > root->id)
      fatal_error("first_negative_ancestor_id: invalid parent ID");
    if (seen[id])
      continue;
    seen[id] = 1;
    if (clause_negative_by_id(id, &known) && known &&
        (result == 0 || id < result))
      result = id;
    parents = clause_parents_by_id(id);
    for (p = parents; p != NULL; p = p->next) {
      unsigned parent = (unsigned) p->i;
      if (parent == 0 || parent > root->id)
        fatal_error("first_negative_ancestor_id: corrupt parent ID");
      if (!seen[parent]) {
        if (count == capacity) {
          if (capacity > SIZE_MAX / 2 / sizeof(*stack))
            fatal_error("first_negative_ancestor_id: proof stack overflow");
          capacity *= 2;
          stack = safe_realloc(stack, capacity * sizeof(*stack));
        }
        stack[count++] = parent;
      }
    }
    zap_ilist(parents);
  }
  safe_free(stack);
  safe_free(seen);
  return result;
}

static
Clause_store new_disabled_store(void)
{
  Clause_store store = clause_store_init("disabled");
  if (str_ident(stringparm1(Opt->ancestor_store), "memory")) {
    if (!clause_store_enable_archive(store, CLAUSE_STORE_ARCHIVE_MEMORY))
      fatal_error("new_disabled_store: cannot initialize memory backing");
  }
  else if (str_ident(stringparm1(Opt->ancestor_store), "mmap")) {
    if (!clause_store_enable_archive(store, CLAUSE_STORE_ARCHIVE_MMAP))
      fatal_error("new_disabled_store: cannot initialize mmap backing");
  }
  else if (str_ident(stringparm1(Opt->ancestor_store), "file")) {
    if (!clause_store_enable_archive(store, CLAUSE_STORE_ARCHIVE_FILE))
      fatal_error("new_disabled_store: cannot initialize file backing");
  }
  return store;
}  /* new_disabled_store */

static Cold_passive_store new_dense_body_store(void)
{
  char *requested = stringparm1(Opt->passive_backing);
  Cold_passive_store_mode mode;
  if (str_ident(requested, "file"))
    mode = COLD_PASSIVE_FILE;
  else if (str_ident(requested, "mmap"))
    mode = COLD_PASSIVE_MMAP;
  else if (str_ident(requested, "memory"))
    mode = COLD_PASSIVE_MEMORY;
  else
    mode = str_ident(stringparm1(Opt->ancestor_store), "mmap") ?
      COLD_PASSIVE_MMAP :
      str_ident(stringparm1(Opt->ancestor_store), "file") ?
        COLD_PASSIVE_FILE : COLD_PASSIVE_MEMORY;
  Cold_passive_store store = cold_passive_store_init(mode);
  if (store == NULL)
    fatal_error("new_dense_body_store: cannot initialize compact arena");
  return store;
}

struct dense_relocate_context {
  Cold_passive_store source;
  Cold_passive_store destination;
};

static size_t relocate_dense_passive(size_t old_position, void *context)
{
  struct dense_relocate_context *ctx = context;
  return cold_passive_store_clone_record(ctx->source, old_position,
                                         ctx->destination);
}

/* The compact OTTER archive is append-only and its record positions remain
   stable.  Selector compaction therefore only has to discard inactive dense
   metadata; the backing positions relocate to themselves. */
static size_t retain_dense_archive_position(size_t old_position,
                                            void *context)
{
  (void) context;
  return old_position;
}

static void compact_dense_passive_store(void)
{
  Cold_passive_store old_store = Dense_body_store;
  Cold_passive_store new_store = new_dense_body_store();
  unsigned long long hot_cursor_id =
    dense_passive_cursor_id(Rewrite_refresh_hot_cursor);
  unsigned long long general_cursor_id =
    dense_passive_cursor_id(Rewrite_refresh_general_cursor);
  unsigned long long interreduce_cursor_id =
    dense_passive_cursor_id(Rewrite_interreduce_cursor);
  struct cold_passive_store_stats old_stats =
    cold_passive_store_get_stats(old_store);
  struct cold_passive_store_stats new_stats;
  struct dense_relocate_context ctx;
  ctx.source = old_store;
  ctx.destination = new_store;
  dense_passive_compact(relocate_dense_passive, &ctx);
  Rewrite_refresh_hot_cursor =
    dense_passive_cursor_from_id(hot_cursor_id);
  Rewrite_refresh_general_cursor =
    dense_passive_cursor_from_id(general_cursor_id);
  Rewrite_interreduce_cursor =
    dense_passive_cursor_from_id(interreduce_cursor_id);
  cold_passive_store_inherit_counters(new_store, old_store);
  new_stats = cold_passive_store_get_stats(new_store);
  if (old_stats.record_bytes >= new_stats.record_bytes)
    Dense_arena_bytes_reclaimed +=
      old_stats.record_bytes - new_stats.record_bytes;
  Dense_body_store = new_store;
  cold_passive_store_free(old_store);
}  /* compact_dense_passive_store */

static void update_rewrite_only_stats(void)
{
  struct compact_rewrite_stats compact;
  unsigned long long shell_bytes =
    rewrite_only_store_allocated_bytes(Rewrite_only_rules);
  compact_rewrite_get_stats(Compact_rewrite_rules, &compact);
  Stats.rewrite_only_demodulators_current =
    rewrite_only_store_count(Rewrite_only_rules);
  Stats.compact_rewrite_rules_current = compact.rules_current;
  Stats.compact_rewrite_rules_peak = compact.rules_peak;
  Stats.compact_rewrite_rules_retired = compact.rules_retired;
  Stats.compact_rewrite_rules_physical = compact.rules_physical;
  Stats.compact_rewrite_compactions = compact.compactions;
  Stats.compact_rewrite_bytes_reclaimed = compact.bytes_reclaimed;
  Stats.compact_rewrite_attempts = compact.attempts;
  Stats.compact_rewrite_rewrites = compact.rewrites;
  Stats.compact_rewrite_node_items = compact.node_items;
  Stats.compact_rewrite_posting_items = compact.posting_items;
  Stats.compact_rewrite_node_bytes = compact.node_bytes;
  Stats.compact_rewrite_posting_bytes = compact.posting_bytes;
  Stats.compact_rewrite_occurrence_bytes = compact.occurrence_bytes;
  Stats.compact_rewrite_occurrence_stream_used =
    compact.occurrence_stream_used;
  Stats.compact_rewrite_occurrence_stream_bytes =
    compact.occurrence_stream_bytes;
  Stats.compact_rewrite_rule_bytes = compact.rule_bytes;
  Stats.compact_rewrite_term_bytes = compact.term_bytes;
  Stats.compact_rewrite_hash_bytes = compact.hash_bytes;
  Stats.rewrite_bank_bytes = shell_bytes + compact.total_bytes;
  if (Stats.rewrite_only_demodulators_current >
      Stats.rewrite_only_demodulators_peak)
    Stats.rewrite_only_demodulators_peak =
      Stats.rewrite_only_demodulators_current;
  if (Stats.rewrite_bank_bytes > Stats.rewrite_bank_peak_bytes)
    Stats.rewrite_bank_peak_bytes = Stats.rewrite_bank_bytes;
}

/* Remove the rewrite-only representation before a dense passive reclaims
   its proof ID.  The caller keeps the returned full clone alive long enough
   to transfer proof-facing state to the materialized clause. */
static Topform take_rewrite_only_rule(unsigned long long id, int *type)
{
  Topform clone = rewrite_only_store_find(Rewrite_only_rules, id, type, NULL);
  if (clone == NULL)
    return NULL;
  if (eager_interreduced_demod_mode()) {
    if (!compact_rewrite_suspend(Compact_rewrite_rules, id))
      fatal_error("take_rewrite_only_rule: compact rule is missing");
  }
  else
    index_demodulator(clone, *type, DELETE, Clocks.index);
  clone = rewrite_only_store_remove(Rewrite_only_rules, id, type, NULL);
  if (clone == NULL)
    fatal_error("take_rewrite_only_rule: store changed during removal");
  if (!detach_clause_id(clone))
    fatal_error("take_rewrite_only_rule: clone does not own proof ID");
  update_rewrite_only_stats();
  return clone;
}

static size_t archive_dense_passive(Topform c, unsigned *body_bytes,
                                    unsigned *justification_bytes,
                                    unsigned *logical_body_bytes)
{
  unsigned long long id;
  size_t position;
  Topform clone;
  if (c == NULL || c->id == 0 || active_or_indexable_clause(c))
    fatal_error("archive_dense_passive: clause is not a detached passive");
  id = c->id;
  if (dense_passive_compaction_needed())
    compact_dense_passive_store();
  position = cold_passive_store_archive(Dense_body_store, c, body_bytes,
                                        justification_bytes,
                                        logical_body_bytes);
  if (position == SIZE_MAX)
    return SIZE_MAX;
  clone = rewrite_only_store_find(Rewrite_only_rules, id, NULL, NULL);
  if (clone != NULL) {
    if (find_clause_by_id(id) != NULL)
      fatal_error("archive_dense_passive: proof ID was not detached");
    register_clause_with_id(clone);
  }
  return position;
}  /* archive_dense_passive */

static Topform activate_dense_passive_internal(size_t position,
                                               unsigned long long id,
                                               unsigned long long hint_id,
                                               BOOL selection)
{
  int rewrite_type = NOT_DEMODULATOR;
  Topform rewrite_clone = take_rewrite_only_rule(id, &rewrite_type);
  Topform c = cold_passive_store_materialize(Dense_body_store, position,
                                              id, TRUE);
  if (c == NULL)
    return NULL;
  if (rewrite_clone != NULL) {
    c->used = c->used || rewrite_clone->used;
    delete_clause(rewrite_clone);
    if (selection)
      Stats.rewrite_only_demodulators_selected++;
  }
  c->matching_hint = hint_by_id(hint_id);
  return c;
}  /* activate_dense_passive_internal */

static Topform activate_dense_passive(size_t position,
                                      unsigned long long id,
                                      unsigned long long hint_id)
{
  return activate_dense_passive_internal(position, id, hint_id, TRUE);
}  /* activate_dense_passive */

/* Decode a dense passive without changing its archived ID-table entry.
   The selector metadata is authoritative for fields that are deliberately
   kept outside the immutable clause record. */
static Topform materialize_dense_passive(
  const struct dense_passive_view *view)
{
  Topform c = cold_passive_store_materialize(Dense_body_store,
                                              view->store_position,
                                              view->id, FALSE);
  if (c == NULL || c->id != view->id)
    fatal_error("materialize_dense_passive: archive identity mismatch");
  c->matching_hint = hint_by_id(view->hint_id);
  c->weight = view->weight;
  c->semantics = view->semantics;
  c->simplifier_epoch = view->simplifier_epoch;
  c->rewrite_epoch = view->rewrite_epoch;
  c->used = view->used;
  c->delayed_demodulator = view->delayed_demodulator;
  c->rewrite_rule_dirty = view->rewrite_rule_dirty;
  {
    Topform clone = rewrite_only_store_find(Rewrite_only_rules, view->id,
                                             NULL, NULL);
    if (clone != NULL)
      c->used = c->used || clone->used;
  }
  return c;
}  /* materialize_dense_passive */

static size_t compact_passive_cache_set(unsigned long long id)
{
  id ^= id >> 33;
  id *= 0xff51afd7ed558ccdULL;
  id ^= id >> 33;
  return (size_t) id & (Compact_passive_cache_sets - 1);
}

static struct compact_passive_cache_entry *compact_passive_cache_find(
  unsigned long long id)
{
  size_t i, base;
  if (Compact_passive_cache == NULL)
    return NULL;
  base = compact_passive_cache_set(id) * COMPACT_PASSIVE_CACHE_WAYS;
  for (i = 0; i < COMPACT_PASSIVE_CACHE_WAYS; i++) {
    struct compact_passive_cache_entry *entry =
      &Compact_passive_cache[base + i];
    if (entry->clause != NULL && entry->id == id)
      return entry;
  }
  return NULL;
}

static void compact_passive_cache_clear_entry(
  struct compact_passive_cache_entry *entry)
{
  if (entry == NULL || entry->clause == NULL)
    return;
  if (entry->pins != 0)
    fatal_error("compact passive cache: attempted to evict a pinned clause");
  clause_store_release_materialized(entry->clause);
  Compact_passive_cache_bytes -= entry->charge;
  Compact_passive_cache_entries--;
  memset(entry, 0, sizeof(*entry));
}

static void compact_passive_cache_free(void)
{
  size_t i;
  if (Compact_passive_cache != NULL) {
    for (i = 0; i < Compact_passive_cache_slots; i++)
      compact_passive_cache_clear_entry(&Compact_passive_cache[i]);
    safe_free(Compact_passive_cache);
  }
  Compact_passive_cache = NULL;
  Compact_passive_cache_sets = 0;
  Compact_passive_cache_slots = 0;
  Compact_passive_cache_budget = 0;
  Compact_passive_cache_bytes = 0;
  Compact_passive_cache_entries = 0;
}

static void compact_passive_cache_init(unsigned megs)
{
  size_t desired, sets = 1;
  compact_passive_cache_free();
  Compact_passive_cache_peak_bytes = 0;
  Compact_passive_cache_peak_entries = 0;
  Compact_passive_cache_clock = 0;
  Compact_passive_cache_eviction_cursor = 0;
  Compact_passive_cache_hits = 0;
  Compact_passive_cache_misses = 0;
  Compact_passive_cache_bypasses = 0;
  Compact_passive_cache_evictions = 0;
  Compact_passive_cache_invalidations = 0;
  if (megs == 0)
    return;
  Compact_passive_cache_budget = (size_t) megs * 1024 * 1024;
  desired = Compact_passive_cache_budget /
            COMPACT_PASSIVE_CACHE_MIN_CHARGE;
  if (desired < COMPACT_PASSIVE_CACHE_WAYS)
    desired = COMPACT_PASSIVE_CACHE_WAYS;
  if (desired > COMPACT_PASSIVE_CACHE_MAX_SLOTS)
    desired = COMPACT_PASSIVE_CACHE_MAX_SLOTS;
  while (sets <= desired / (COMPACT_PASSIVE_CACHE_WAYS * 2))
    sets *= 2;
  Compact_passive_cache_sets = sets;
  Compact_passive_cache_slots = sets * COMPACT_PASSIVE_CACHE_WAYS;
  Compact_passive_cache = safe_malloc(
    Compact_passive_cache_slots * sizeof(*Compact_passive_cache));
  memset(Compact_passive_cache, 0,
         Compact_passive_cache_slots * sizeof(*Compact_passive_cache));
}

static void restore_compact_passive_metadata(
  Topform c, const struct dense_passive_view *view)
{
  c->matching_hint = hint_by_id(view->hint_id);
  c->weight = view->weight;
  c->semantics = view->semantics;
  c->simplifier_epoch = view->simplifier_epoch;
  c->rewrite_epoch = view->rewrite_epoch;
  c->used = view->used;
  c->delayed_demodulator = view->delayed_demodulator;
  c->rewrite_rule_dirty = view->rewrite_rule_dirty;
}

static Topform materialize_compact_otter_passive(
  const struct dense_passive_view *view)
{
  Topform c = clause_store_materialize(Glob.disabled, view->store_position);
  if (c == NULL || c->id != view->id || !c->archive_materialized)
    fatal_error("materialize_compact_otter_passive: archive identity mismatch");
  restore_compact_passive_metadata(c, view);
  return c;
}

/* Checkpoint writers operate on either kind of dense frontier.  DISCOUNT
   owns passive bodies in Cold_passive_store; compact OTTER deliberately
   makes the ancestor archive their sole owner.  Keep that ownership choice
   behind one paired materialize/release interface so checkpoint traversal
   cannot accidentally decode an archive position through the wrong store. */
static Topform materialize_checkpoint_passive(
  const struct dense_passive_view *view)
{
  return compact_otter_passive_mode() ?
    materialize_compact_otter_passive(view) : materialize_dense_passive(view);
}

static void release_checkpoint_passive(Topform c)
{
  if (compact_otter_passive_mode())
    clause_store_release_materialized(c);
  else
    cold_passive_store_release(c);
}

static Topform compact_passive_cache_resolve(
  const struct dense_passive_view *view)
{
  struct compact_passive_cache_entry *entry;
  size_t i, base, charge;
  entry = compact_passive_cache_find(view->id);
  if (entry != NULL) {
    if (entry->position != view->store_position)
      fatal_error("compact passive cache: archive position changed");
    entry->pins++;
    entry->stamp = ++Compact_passive_cache_clock;
    restore_compact_passive_metadata(entry->clause, view);
    Compact_passive_cache_hits++;
    return entry->clause;
  }

  Compact_passive_cache_misses++;
  if (Compact_passive_cache == NULL) {
    Compact_passive_cache_bypasses++;
    return materialize_compact_otter_passive(view);
  }

  charge = (size_t) view->logical_body_bytes + view->body_bytes +
           view->justification_bytes + COMPACT_PASSIVE_CACHE_MIN_CHARGE;
  if (charge < COMPACT_PASSIVE_CACHE_MIN_CHARGE ||
      charge > Compact_passive_cache_budget) {
    Compact_passive_cache_bypasses++;
    return materialize_compact_otter_passive(view);
  }

  base = compact_passive_cache_set(view->id) * COMPACT_PASSIVE_CACHE_WAYS;
  entry = NULL;
  for (i = 0; i < COMPACT_PASSIVE_CACHE_WAYS; i++) {
    struct compact_passive_cache_entry *candidate =
      &Compact_passive_cache[base + i];
    if (candidate->clause == NULL) {
      entry = candidate;
      break;
    }
    if (candidate->pins == 0 &&
        (entry == NULL || candidate->stamp < entry->stamp))
      entry = candidate;
  }
  if (entry == NULL) {
    Compact_passive_cache_bypasses++;
    return materialize_compact_otter_passive(view);
  }
  if (entry->clause != NULL) {
    compact_passive_cache_clear_entry(entry);
    Compact_passive_cache_evictions++;
  }
  while (Compact_passive_cache_bytes >
         Compact_passive_cache_budget - charge) {
    size_t scanned;
    BOOL evicted = FALSE;
    for (scanned = 0; scanned < Compact_passive_cache_slots; scanned++) {
      struct compact_passive_cache_entry *candidate =
        &Compact_passive_cache[Compact_passive_cache_eviction_cursor];
      Compact_passive_cache_eviction_cursor =
        (Compact_passive_cache_eviction_cursor + 1) %
        Compact_passive_cache_slots;
      if (candidate->clause != NULL && candidate->pins == 0) {
        compact_passive_cache_clear_entry(candidate);
        Compact_passive_cache_evictions++;
        evicted = TRUE;
        break;
      }
    }
    if (!evicted) {
      Compact_passive_cache_bypasses++;
      return materialize_compact_otter_passive(view);
    }
  }

  entry->clause = materialize_compact_otter_passive(view);
  entry->id = view->id;
  entry->position = view->store_position;
  entry->charge = charge;
  entry->stamp = ++Compact_passive_cache_clock;
  entry->pins = 1;
  Compact_passive_cache_bytes += charge;
  Compact_passive_cache_entries++;
  if (Compact_passive_cache_bytes > Compact_passive_cache_peak_bytes)
    Compact_passive_cache_peak_bytes = Compact_passive_cache_bytes;
  if (Compact_passive_cache_entries > Compact_passive_cache_peak_entries)
    Compact_passive_cache_peak_entries = Compact_passive_cache_entries;
  return entry->clause;
}

static void compact_passive_cache_discard(unsigned long long id,
                                          Topform expected)
{
  struct compact_passive_cache_entry *entry =
    compact_passive_cache_find(id);
  if (entry != NULL) {
    if (expected != NULL && entry->clause != expected)
      fatal_error("compact passive cache: materialized identity mismatch");
    if (entry->pins != (expected == NULL ? 0U : 1U))
      fatal_error("compact passive cache: invalid activation pin count");
    entry->pins = 0;
    compact_passive_cache_clear_entry(entry);
    Compact_passive_cache_invalidations++;
  }
  else if (expected != NULL)
    clause_store_release_materialized(expected);
}

/* Compact OTTER uses the ancestor archive as the single immutable owner of
   a passive body.  This keeps proof ancestry and on-demand clause lookup in
   the established archived-ID namespace instead of duplicating every body
   in the DISCOUNT cold store. */
static size_t archive_compact_otter_passive(
  Topform c, unsigned *body_bytes, unsigned *justification_bytes,
  unsigned *logical_body_bytes)
{
  size_t position;
  unsigned just_bytes;
  Clause_compress_result result;
  if (c == NULL || c->id == 0 || active_or_indexable_clause(c))
    fatal_error("archive_compact_otter_passive: clause is still on a live list");
  if (dense_passive_compaction_needed())
    dense_passive_compact(retain_dense_archive_position, NULL);
  result = compress_clause_with_justification(c);
  if (result != CLAUSE_COMPRESS_OK && result != CLAUSE_COMPRESS_ALREADY)
    return SIZE_MAX;
  just_bytes = compressed_clause_justification_bytes(c);
  if (just_bytes > c->compressed_size)
    return SIZE_MAX;
  if (body_bytes != NULL)
    *body_bytes = c->compressed_size - just_bytes;
  if (justification_bytes != NULL)
    *justification_bytes = just_bytes;
  if (logical_body_bytes != NULL)
    *logical_body_bytes = c->uncompressed_body_bytes;
  position = clause_store_length(Glob.disabled);
  clause_store_append(Glob.disabled, c);
  if (!clause_store_archive_clause(Glob.disabled, c))
    return SIZE_MAX;
  return position;
}

static Topform activate_compact_otter_passive(
  size_t position, unsigned long long id, unsigned long long hint_id)
{
  compact_passive_cache_discard(id, NULL);
  Topform c = clause_store_activate(Glob.disabled, position);
  if (c == NULL || c->id != id)
    return NULL;
  c->matching_hint = hint_by_id(hint_id);
  if (compact_rewrite_contains(Compact_rewrite_rules, id) &&
      !clist_member(c, Glob.demods))
    clist_append(c, Glob.demods);
  return c;
}

static Topform compact_otter_resolve_clause(unsigned long long id,
                                             void *context)
{
  struct dense_passive_view view;
  Topform c;
  (void) context;
  if (!compact_otter_passive_mode() ||
      !dense_passive_view_id(id, &view))
    return NULL;
  c = compact_passive_cache_resolve(&view);
  return c;
}

static void compact_otter_release_clause(Topform c, void *context)
{
  (void) context;
  if (c == NULL || !c->archive_materialized)
    return;
  if (c->used && dense_passive_contains_id(c->id) &&
      !dense_passive_mark_used(c->id))
    fatal_error("compact_otter_release_clause: passive used bit was lost");
  {
    struct compact_passive_cache_entry *entry =
      compact_passive_cache_find(c->id);
    if (entry != NULL && entry->clause == c) {
      if (entry->pins == 0)
        fatal_error("compact passive cache: release of unpinned clause");
      entry->pins--;
      entry->stamp = ++Compact_passive_cache_clock;
    }
    else
      clause_store_release_materialized(c);
  }
}

/* Back-demod reconstruction visits records in stable insertion order.  Once
   a bounded batch has been decoded and copied into the replacement index,
   its immutable ancestor pages no longer need process residency. */
static void compact_otter_advise_rebuild_batch(
  const unsigned long long *ids, size_t count, void *context)
{
  size_t i, first = SIZE_MAX, last = 0;
  (void) context;
  if (!compact_otter_passive_mode() || ids == NULL)
    return;
  for (i = 0; i < count; i++) {
    struct dense_passive_view view;
    if (dense_passive_view_id(ids[i], &view)) {
      if (view.store_position < first)
        first = view.store_position;
      if (view.store_position > last)
        last = view.store_position;
    }
  }
  if (first != SIZE_MAX)
    clause_store_advise_mmap_range_cold(Glob.disabled, first, last);
}

static
void compress_retained_clause(Topform c)
{
  if (!flag(Opt->compress_disabled))
    return;
  if (active_or_indexable_clause(c))
    fatal_error("compress_retained_clause: clause is still active or indexed");
  if (compress_clause(c) == CLAUSE_COMPRESS_INVALID)
    fatal_error("compress_retained_clause: invalid clause");
}  /* compress_retained_clause */

/* Archive proof/attribute state without destroying a body that the
   persistent collective index still references.  Once the archive owns the
   proof-facing representation, history needs only the immutable inference
   body and stable ID. */
static
BOOL archive_retained_clause(Clause_store store, Topform c)
{
  if (!c->collective_history)
    return clause_store_archive_clause(store, c);

  if (!clause_store_archive_clause_preserve(store, c))
    return FALSE;
  zap_just(c->justification);
  c->justification = NULL;
  zap_attributes(c->attributes);
  c->attributes = NULL;
  c->matching_hint = NULL;
  return TRUE;
}

static
void retain_disabled_clause(Topform c)
{
  if (active_or_indexable_clause(c))
    fatal_error("retain_disabled_clause: clause is still active or indexed");
  clause_store_append(Glob.disabled, c);
  /* Pre-elimination scratch clauses can be disabled before IDs are assigned.
     They cannot be proof ancestors or checkpoint entries; keep the Phase 2
     representation rather than perturbing the deterministic ID sequence.
     Formula placeholders likewise remain live because formulas.txt is the
     established checkpoint namespace for them. */
  if (c->id == 0 || c->is_formula) {
    if (!str_ident(stringparm1(Opt->ancestor_store), "off") &&
        compress_clause(c) == CLAUSE_COMPRESS_INVALID)
      fatal_error("retain_disabled_clause: invalid pre-ID clause");
    else if (str_ident(stringparm1(Opt->ancestor_store), "off"))
      compress_retained_clause(c);
  }
  else if (!str_ident(stringparm1(Opt->ancestor_store), "off")) {
    if (!archive_retained_clause(Glob.disabled, c))
      fatal_error("retain_disabled_clause: ancestor archive failed");
  }
  else
    compress_retained_clause(c);
}  /* retain_disabled_clause */

static
void compress_retained_store(Clause_store store)
{
  size_t i;
  BOOL archive = !str_ident(stringparm1(Opt->ancestor_store), "off");
  if ((!flag(Opt->compress_disabled) && !archive) || store == NULL)
    return;
  for (i = 0; i < clause_store_length(store); i++) {
    if (clause_store_position_is_archived(store, i))
      continue;
    Topform c = clause_store_get(store, i);
    if (archive && c->id != 0 && !c->is_formula) {
      if (!archive_retained_clause(store, c))
        fatal_error("compress_retained_store: ancestor archive failed");
    }
    else if (c->compressed == NULL) {
      if (archive) {
        if (compress_clause(c) == CLAUSE_COMPRESS_INVALID)
          fatal_error("compress_retained_store: invalid pre-ID clause");
      }
      else
        compress_retained_clause(c);
    }
  }
}  /* compress_retained_store */

static void maybe_compact_shared_term_pool(void)
{
  struct compact_term_pool_stats terms;
  struct compact_rewrite_stats rewrite;
  unsigned long long retained, stale;
  unsigned long long stale_tokens, estimated_reclaimable_bytes;
  unsigned long long configured_reclaim_bytes, predicted_reclaim_bytes;
  Compact_term_rebase_map map;
  if (!compact_otter_passive_mode() || Compact_terms == NULL)
    return;
  compact_term_pool_get_stats(Compact_terms, &terms);
  if (terms.sharing_profile_enabled)
    return;
  if (terms.serializations < Compact_term_next_reclaim_serialization) {
    Compact_term_reclaim_cooldown_skips++;
    return;
  }
  compact_rewrite_get_stats(Compact_rewrite_rules, &rewrite);
  /* Every physical record still owns a valid term slice even when it is
     inactive.  The index-specific 25%-stale policies rebuild those records
     independently; count the physical populations here so a pool reclaim is
     delayed until it can drop clauses absent from every current index. */
  retained = compact_back_demod_physical_count();
  if (compact_unit_physical_count() > retained)
    retained = compact_unit_physical_count();
  if (rewrite.rules_physical > retained)
    retained = rewrite.rules_physical;
  if (terms.clause_entries <= retained)
    return;
  stale = terms.clause_entries - retained;
  if (stale < 1024)
    return;
  /* Rebuilding all three indexes is deliberately a cold operation.  A
     clause-count ratio alone fired much too early on CHAT-sized prefixes,
     while a 25% ratio later delayed a configured 2-MiB recovery until the
     token array had already crossed another realloc high-water mark.  Keep a
     1,024-clause noise floor, then make the conservative stale-token payload
     budget authoritative.  Directory/index savings are still ignored. */
  stale_tokens =
    (terms.logical_tokens / terms.clause_entries) * stale +
    ((terms.logical_tokens % terms.clause_entries) * stale) /
      terms.clause_entries;
  estimated_reclaimable_bytes = stale_tokens * sizeof(uint32_t);
  configured_reclaim_bytes =
    (unsigned long long) parm(Opt->compact_term_reclaim_kb) * 1024;
  if (estimated_reclaimable_bytes < configured_reclaim_bytes)
    return;
  map = compact_term_rebase_map_init();
  /* Back-demod records cover nearly the whole retained clause population;
     unit and rewrite records add any exceptional clauses.  Preserve inactive
     physical records too: their posting paths can still refer to the shared
     token slice until the index's own bounded compaction removes them. */
  compact_back_demod_retain_term_clauses(map);
  compact_unit_retain_term_clauses(map);
  compact_rewrite_retain_live_clauses(Compact_rewrite_rules, map);
  predicted_reclaim_bytes =
    compact_term_pool_retained_reclaimable_bytes(Compact_terms, map);
  Compact_term_last_predicted_reclaim = predicted_reclaim_bytes;
  if (predicted_reclaim_bytes < configured_reclaim_bytes) {
    Compact_term_reclaim_deferrals++;
    Compact_term_next_reclaim_serialization = terms.serializations + 1024;
    compact_term_rebase_map_free(map);
    return;
  }
  compact_term_pool_compact_retained(Compact_terms, map);
  compact_rewrite_rebase_term_pool(Compact_rewrite_rules,
                                   Compact_terms, map);
  compact_unit_rebase_term_pool(Compact_terms, map);
  compact_back_demod_rebase_shared_term_pool(Compact_terms, map);
  compact_term_rebase_map_free(map);
  Compact_term_next_reclaim_serialization = terms.serializations + 1024;
}

/*************
 *
 *   disable_clause()
 *
 *************/

static
void disable_clause(Topform c)
{
  // Assume c is in Usable, Sos, Denials, or none of those.
  // Also, c may be in Demodulators.
  //
  // Unindex c according to which lists it is on and
  // the flags that are set, remove c from the lists,
  // and append c to Disabled.  Make sure you don't
  // have a Clist_pos for c during the call, because
  // it will be freed during the call.

  clock_start(Clocks.disable);

  if (compact_otter_passive_mode() && c->archive_materialized &&
      dense_passive_contains_id(c->id)) {
    struct dense_passive_view view;
    unsigned long long id = c->id;
    if (!dense_passive_view_id(id, &view))
      fatal_error("disable_clause: cold passive metadata is missing");
    compact_passive_cache_discard(id, c);
    c = clause_store_activate(Glob.disabled, view.store_position);
    if (c == NULL || c->id != id)
      fatal_error("disable_clause: cannot activate cold passive");
    c->matching_hint = hint_by_id(view.hint_id);
    c->weight = view.weight;
    c->semantics = view.semantics;
    c->simplifier_epoch = view.simplifier_epoch;
    c->rewrite_epoch = view.rewrite_epoch;
    c->used = view.used;
    c->delayed_demodulator = view.delayed_demodulator;
    c->rewrite_rule_dirty = view.rewrite_rule_dirty;
    if (!dense_passive_deactivate_id(id, NULL))
      fatal_error("disable_clause: cold passive selector record is missing");
    if (compact_rewrite_contains(Compact_rewrite_rules, id)) {
      if (!compact_rewrite_remove(Compact_rewrite_rules, id))
        fatal_error("disable_clause: compact cold demodulator is missing");
      if (compact_rewrite_compaction_needed(Compact_rewrite_rules))
        compact_rewrite_compact(Compact_rewrite_rules);
      update_rewrite_only_stats();
    }
    index_literals(c, DELETE, Clocks.index, FALSE);
    index_back_demod(c, DELETE, Clocks.index, flag(Opt->back_demod));
    maybe_compact_shared_term_pool();
    retain_disabled_clause(c);
    clock_stop(Clocks.disable);
    return;
  }

  if (clist_member(c, Glob.demods)) {
    if (eager_interreduced_demod_mode() || compact_otter_demod_mode()) {
      if (!compact_rewrite_remove(Compact_rewrite_rules, c->id))
        fatal_error("disable_clause: compact demodulator is missing");
      if (compact_rewrite_compaction_needed(Compact_rewrite_rules))
        compact_rewrite_compact(Compact_rewrite_rules);
      update_rewrite_only_stats();
    }
    else {
      if (compact_otter_audit_mode()) {
        if (!compact_rewrite_remove(Compact_rewrite_rules, c->id))
          fatal_error("disable_clause: audited compact demodulator is missing");
        if (compact_rewrite_compaction_needed(Compact_rewrite_rules))
          compact_rewrite_compact(Compact_rewrite_rules);
        update_rewrite_only_stats();
      }
      index_demodulator(c, demodulator_type(c,
					    parm(Opt->lex_dep_demod_lim),
					    flag(Opt->lex_dep_demod_sane)),
			DELETE, Clocks.index);
    }
    clist_remove(c, Glob.demods);
  }

  if (clist_member(c, Glob.usable)) {
    collective_note_deactivation(c);
    index_literals(c, DELETE, Clocks.index, FALSE);
    index_back_demod(c, DELETE, Clocks.index, flag(Opt->back_demod));
    if (!restricted_denial(c))
      index_clashable(c, DELETE);
    clist_remove(c, Glob.usable);
  }
  else if (clist_member(c, Glob.sos)) {
    if (!discount_mode()) {
      index_literals(c, DELETE, Clocks.index, FALSE);
      index_back_demod(c, DELETE, Clocks.index, flag(Opt->back_demod));
    }
    remove_from_sos2(c, Glob.sos);
  }
  else if (clist_member(c, Glob.limbo)) {
    if (!discount_mode() || c->was_given || restricted_denial(c))
      index_literals(c, DELETE, Clocks.index, FALSE);
    clist_remove(c, Glob.limbo);
  }

  maybe_compact_shared_term_pool();
  retain_disabled_clause(c);
  clock_stop(Clocks.disable);
}  // disable_clause

/*************
 *
 *   free_search_memory()
 *
 *   This frees memory so that we can check for memory leaks.
 *
 *************/

/* DOCUMENTATION
This is intended for debugging only.
*/

/* PUBLIC */
void free_search_memory(void)
{
  while (rewrite_only_store_count(Rewrite_only_rules) != 0) {
    int type;
    Topform c = rewrite_only_store_take_any(Rewrite_only_rules, &type, NULL);
    if (c == NULL)
      fatal_error("free_search_memory: corrupt rewrite-only store");
    if (!eager_interreduced_demod_mode())
      index_demodulator(c, type, DELETE, Clocks.index);
    delete_clause(c);
  }
  rewrite_only_store_free(Rewrite_only_rules);
  Rewrite_only_rules = NULL;

  // Demodulators

  while (Glob.demods->first) {
    Topform c = Glob.demods->first->c;
    if (eager_interreduced_demod_mode() || compact_otter_demod_mode()) {
      if (!compact_rewrite_remove(Compact_rewrite_rules, c->id))
        fatal_error("free_search_memory: compact demodulator is missing");
    }
    else {
      if (compact_otter_audit_mode() &&
          !compact_rewrite_remove(Compact_rewrite_rules, c->id))
        fatal_error("free_search_memory: audited compact demodulator is missing");
      index_demodulator(c, demodulator_type(c,
					    parm(Opt->lex_dep_demod_lim),
					    flag(Opt->lex_dep_demod_sane)),
			DELETE, Clocks.index);
    }
    clist_remove(c, Glob.demods);
    if (c->containers == NULL && !c->disabled)
      delete_clause(c);
  }
  clist_free(Glob.demods);
  
  destroy_demodulation_index();
  compact_rewrite_free(Compact_rewrite_rules);
  Compact_rewrite_rules = NULL;

  // Usable, Sos, Limbo

  while (Glob.usable->first)
    disable_clause(Glob.usable->first->c);
  clist_free(Glob.usable);
  Glob.usable = NULL;

  while (Glob.sos->first)
    disable_clause(Glob.sos->first->c);
  clist_free(Glob.sos);
  Glob.sos = NULL;

  while (Glob.limbo->first)
    disable_clause(Glob.limbo->first->c);
  clist_free(Glob.limbo);
  Glob.limbo = NULL;

  destroy_literals_index();
  destroy_back_demod_index();
  compact_term_pool_free(Compact_terms);
  Compact_terms = NULL;
  lindex_destroy(Glob.clashable_idx);
  Glob.clashable_idx = NULL;

  zap_ilist(Glob.cac_clauses);
  Glob.cac_clauses = NULL;
  zap_ilist(Glob.desc_to_be_disabled);
  Glob.desc_to_be_disabled = NULL;

  compact_passive_cache_free();
  clause_store_delete_clauses(Glob.disabled);
  Glob.disabled = NULL;
  reset_selector_indexes();
  cold_passive_store_free(Dense_body_store);
  Dense_body_store = NULL;
  Dense_arena_bytes_reclaimed = 0;

  if (Glob.hints->first) {
    Clist_pos p;
    for(p = Glob.hints->first; p; p = p->next)
      unindex_hint(p->c);
    done_with_hints();
  }
  delete_clist(Glob.hints);
  Glob.hints = NULL;

  collective_clear_state();

}  // free_search_memory

/*************
 *
 *   handle_proof_and_maybe_exit()
 *
 *************/

static
void handle_proof_and_maybe_exit(Topform empty_clause)
{
  Term answers;
  Plist proof, materialized, p;
  BOOL terminal_proof;

  assign_clause_id(empty_clause);

  if (!flag(Opt->reuse_denials) && Glob.horn) {
    unsigned negative_id = first_negative_ancestor_id(empty_clause);
    if (negative_id == 0)
      fatal_error("handle_proof_and_maybe_exit: negative ancestor is missing");
    if (ilist_member(Glob.desc_to_be_disabled, (int) negative_id)) {
      if (!flag(Opt->quiet)) {
	printf("%% Redundant proof: ");
	f_clause(empty_clause);
      }
      return;
    }
    else
      /* Descendants of this denial will be disabled when it is safe. */
      Glob.desc_to_be_disabled =
        ilist_prepend(Glob.desc_to_be_disabled, (int) negative_id);
  }

  /* Mark parents as used only for non-redundant proofs.  If done earlier,
     parents could be marked as used but then escape forward subsumption
     (which skips used clauses), leading to back subsumption finding them
     in limbo. */
  mark_parents_as_used(empty_clause);

  Glob.empties = plist_append(Glob.empties, empty_clause);
  Stats.proofs++;
  terminal_proof = at_parm_limit(Stats.proofs, Opt->max_proofs);

  /* A terminal compact proof needs the ancestor archive and hints, but it
     cannot issue another rewrite/subsumption/redex query.  Preserve the
     pre-release statistics, then remove search-only compact indexes before
     materializing the proof DAG. */
  if (terminal_proof && compact_otter_passive_mode()) {
    freeze_terminal_statistics();
    release_terminal_compact_indexes();
  }

  proof = get_clause_ancestors(empty_clause);
  restore_archive_hint_links(proof);
  if (terminal_proof && compact_otter_passive_mode())
    release_terminal_hint_index();
  materialized = materialize_clauses(proof);

  answers = get_term_attributes(empty_clause->attributes, Att.answer);

  /* message to stderr */

  if (!flag(Opt->quiet)) {
    fflush(stdout);
    if (flag(Opt->bell))
      bell(stderr);
    fprintf(stderr, "-------- Proof %s -------- ", comma_num(Stats.proofs));
    if (answers != NULL)
      fwrite_term_nl(stderr, answers);
    else if (flag(Opt->print_proof_goal)) {
      /* Find and print goal clause(s) from the proof */
      Plist q;
      BOOL printed_any = FALSE;
      for (q = proof; q; q = q->next) {
        Topform c = q->v;
        if (c->justification && c->justification->type == GOAL_JUST) {
          /* Find a meaningful label (skip "non_clause" and "goal") */
          char *label = NULL;
          int i;
          for (i = 1; ; i++) {
            char *l = get_string_attribute(c->attributes, Att.label, i);
            if (l == NULL)
              break;
            if (!str_ident(l, "non_clause") && !str_ident(l, "goal")) {
              label = l;
              break;
            }
          }
          if (printed_any)
            fprintf(stderr, ", ");
          if (label)
            fprintf(stderr, "\"%s\"", label);
          else {
            /* Print the goal formula without attributes */
            Term t = topform_to_term_without_attributes(c);
            fwrite_term(stderr, t);
            zap_term(t);
          }
          printed_any = TRUE;
        }
      }
      fprintf(stderr, "\n");
    }
    else
      fprintf(stderr, "\n");
  }

  /* print proof to stdout -- protect from signal truncation */

  set_no_kill();
  fflush(stderr);
  if (flag(Opt->tptp_output)) {
    /* TSTP format proof output - always printed in TPTP mode.
       CASC requires the SZS status line to PRECEDE the SZS output block
       (https://tptp.org/CASC/J13/Design.html#SystemProperties), so a solve
       is credited even if the proof print is later truncated by a
       wall-clock / CPU-limit kill.  Emit it here, once: the Szs_line_written
       guard makes print_exit_message and the async signal handlers skip a
       second one.  set_no_kill() above has already deferred termination, so
       this status line and the proof body that follows go out as a unit. */
    if (!Szs_line_written) {
      Szs_line_written = 1;
      if (Glob.problem_name)
        printf("\n%% SZS status %s for %s\n",
               szs_status_string(MAX_PROOFS_EXIT), Glob.problem_name);
      else
        printf("\n%% SZS status %s\n", szs_status_string(MAX_PROOFS_EXIT));
    }
    printf("\n%% Proof %s at %.2f (+ %.2f) seconds.\n",
	   comma_num(Stats.proofs), user_seconds(), system_seconds());
    printf("%% Length of proof is %d.\n", proof_length(proof));
    printf("%% Level of proof is %d.\n", clause_level(empty_clause));
    printf("%% Maximum clause weight is %.3f.\n", max_clause_weight(proof));
    printf("%% Given clauses %s.\n\n", comma_num(Stats.given));
    fprint_proof_tptp(stdout, proof);
  }
  else if (flag(Opt->print_proofs)) {
    /* Native Prover9 proof output */
    print_separator(stdout, "PROOF", TRUE);
    printf("\n%% Proof %s at %.2f (+ %.2f) seconds",
	   comma_num(Stats.proofs), user_seconds(), system_seconds());
    if (answers != NULL) {
      printf(": ");
      fwrite_term(stdout, answers);
    }
    printf(".\n");

    {
      int pf_level;
      double max_pf_wt;
      Topform proof_cl;
      int pf_nothint_ct = 0;
      int pf_total_given = 0;
      int pf_nothint_given = 0;

      for (p = proof; p; p = p->next) {
	proof_cl = (Topform) p->v;
	if (proof_cl->was_given)
	  pf_total_given++;
	if (proof_cl->matching_hint == NULL
	    && !has_input_just(proof_cl)
	    && !has_goal_just(proof_cl)
	    && !proof_cl->initial
	    && !has_deny_just(proof_cl)
	    && number_of_literals(proof_cl->literals) != 0) {
	  pf_nothint_ct++;
	  if (proof_cl->was_given)
	    pf_nothint_given++;
	}
      }

      printf("%% Length of proof: %d (%d new hints)\n",
	     proof_length(proof), pf_nothint_ct);

      pf_level = clause_level(empty_clause);
      printf("%% Level of proof: %d\n", pf_level);

      max_pf_wt = max_clause_weight(proof);
      if (max_pf_wt > 500.00)
	printf("%% Maximum clause weight: %.3f (%d w/o degradation)\n",
	       max_pf_wt, imax_clause_weight(proof));
      else
	printf("%% Maximum clause weight: %.3f\n", max_pf_wt);

      printf("%% Given clauses in run: %s\n", comma_num(Stats.given));
      printf("%% Given clauses in proof: %d (%d new hints)\n\n",
	     pf_total_given, pf_nothint_given);
    }
    {
      /* Expand compound justifications (rewrite chains, etc.) into
         separate paramodulation steps when set(print_expanded_proof)
         or the -expand CLI switch is in effect.  Helpful for users
         learning Prover9: a step like [copy(16),rewrite([7(2),4(6)])]
         becomes one paramod inference per rewrite, with the
         intermediate clause body shown explicitly.

         expand_proof rewrites goal/non-clause formula bodies as a
         side effect of its internal CNF-style processing; for the LADR
         print we restore the original body of any such clause so the
         user sees the goal as written.  TPTP output is unaffected
         (it relies on the rewritten form for clausify inferences). */
      Plist proof_to_print = proof;
      I3list jmap = NULL;
      if (flag(Opt->print_expanded_proof)) {
        proof_to_print = expand_proof(proof, &jmap);
        if (proof_to_print == NULL)
          /* Replay failure (see expand_proof): print the unexpanded proof. */
          proof_to_print = proof;
        else {
        /* Restore original goal/input bodies - expand_proof may rewrite
           a goal body as a side effect of CNF processing.  Deep-copy
           literals/formula so renumber_proof's uplink check stays
           consistent. */
        Plist q;
        for (q = proof_to_print; q; q = q->next) {
          Topform c = (Topform) q->v;
          if (c->is_formula || has_goal_just(c) || has_input_just(c)) {
            Topform orig = proof_id_to_clause(proof, c->id);
            if (orig != NULL && orig != c) {
              if (orig->is_formula && orig->formula) {
                c->formula = formula_copy(orig->formula);
                c->is_formula = TRUE;
              } else if (orig->literals) {
                c->literals = copy_literals(orig->literals);
                upward_clause_links(c);
              }
            }
          }
        }
        /* Sequential numbering from 1 so intermediate steps don't get
           awkward high IDs (12, 13 inserted between 6 and 7). */
        renumber_proof(proof_to_print, 1);
        }
      }
      if (flag(Opt->print_substitutions))
        set_para_subst_proof(proof_to_print);
      for (p = proof_to_print; p; p = p->next)
        fwrite_clause(stdout, p->v, CL_FORM_STD);
      set_para_subst_proof(NULL);  /* restore: avoid stale references */
      if (jmap)
        zap_i3list(jmap);
    }
    print_separator(stdout, "end of proof", TRUE);
  }
  else {
    printf("\n-------- Proof %s at (%.2f + %.2f seconds) ",
	   comma_num(Stats.proofs), user_seconds(), system_seconds());
    if (answers != NULL)
      fwrite_term_nl(stdout, answers);
    else
      fprintf(stdout, "\n");
  }
  /* print_matched_hints: three lists per proof (Veroff feature) */
  if (flag(Opt->print_matched_hints)) {
    int pmh_count = 0;
    print_separator(stdout, "MATCHED HINTS", TRUE);
    fprintf(stdout,
	    "\nformulas(hints).  %% Hints matched by proof clauses.\n");
    for (p = proof; p; p = p->next) {
      Topform h = ((Topform) p->v)->matching_hint;
      if (h != NULL) {
	if (true_clause(h->literals))
	  pmh_count++;
	else
	  fwrite_clause(stdout, h, CL_FORM_BARE);
      }
    }
    printf("%% *** Not including %d hints that were back demodulated. ***\n",
	   pmh_count);
    fprintf(stdout, "end_of_list.\n");
    print_separator(stdout, "end of matched hints", TRUE);

    print_separator(stdout, "HINT MATCHERS", TRUE);
    fprintf(stdout,
	    "\nformulas(hints).  %% Proof clauses that match a hint.\n");
    for (p = proof; p; p = p->next) {
      if (((Topform) p->v)->matching_hint != NULL)
	fwrite_clause(stdout, p->v, CL_FORM_BARE);
    }
    fprintf(stdout, "end_of_list.\n");
    print_separator(stdout, "end of hint matchers", TRUE);

    print_separator(stdout, "NON HINT MATCHERS", TRUE);
    fprintf(stdout,
	    "\nformulas(hints).  %% Proof clauses that do not match any hints.\n");
    for (p = proof; p; p = p->next) {
      if (((Topform) p->v)->matching_hint == NULL)
	fwrite_clause(stdout, p->v, CL_FORM_BARE);
    }
    fprintf(stdout, "end_of_list.\n");
    print_separator(stdout, "end of non hint matchers", TRUE);
  }

  if (flag(Opt->print_new_hints)) {
    Topform proof_cl;
    print_separator(stdout, "NEW HINTS", TRUE);
    fprintf(stdout, "\nGiven clauses:\n\n");
    for (p = proof; p; p = p->next) {
      proof_cl = (Topform) p->v;
      if (proof_cl->matching_hint == NULL
	  && !has_input_just(proof_cl)
	  && !has_goal_just(proof_cl)
	  && !proof_cl->initial
	  && !has_deny_just(proof_cl)
	  && number_of_literals(proof_cl->literals) != 0) {
	if (proof_cl->was_given)
	  fwrite_clause(stdout, p->v, CL_FORM_STD);
      }
    }
    print_separator(stdout, "end of proof", TRUE);
    fprintf(stdout, "\nProof clauses not given:\n\n");
    for (p = proof; p; p = p->next) {
      proof_cl = (Topform) p->v;
      if (proof_cl->matching_hint == NULL
	  && !has_input_just(proof_cl)
	  && !has_goal_just(proof_cl)
	  && !proof_cl->initial
	  && !has_deny_just(proof_cl)
	  && number_of_literals(proof_cl->literals) != 0) {
	if (!proof_cl->was_given)
	  fwrite_clause(stdout, p->v, CL_FORM_STD);
      }
    }
    print_separator(stdout, "end of proof", TRUE);
  }

  fflush(stdout);
  clear_no_kill_and_check();
  if (answers)
    zap_term(answers);

  recompress_clauses(materialized);
  zap_plist(materialized);
  actions_in_proof(proof, &Att);  /* this can exit */

  clause_store_release_materialized_plist(proof);
  zap_plist(proof);

  if (terminal_proof)
    done_with_search(MAX_PROOFS_EXIT);  /* does not return */
}  // handle_proof_and_maybe_exit

/*************
 *
 *   clause_wt_with_adjustments()
 *
 *************/

static
void clause_wt_with_adjustments(Topform c)
{
  clock_start(Clocks.weigh);
  c->weight = clause_weight(c->literals);
  clock_stop(Clocks.weigh);

  if (!clist_empty(Glob.hints)) {
    clock_start(Clocks.hints);
    if (!c->normal_vars)
      renumber_variables(c, MAX_VARS);
    adjust_weight_with_hints(c,
			     flag(Opt->degrade_hints),
			     flag(Opt->breadth_first_hints));
    clock_stop(Clocks.hints);
  }

  {
    int sw = parm(Opt->sine_weight);
    if (sw > 0) {
      int sd = get_int_attribute(c->attributes, sine_depth_attr(), 1);
      if (sd != INT_MAX && sd > 1)
        c->weight += sw * (sd - 1);
    }
  }

  if (c->weight > floatparm(Opt->default_weight) &&
      c->weight <= floatparm(Opt->max_weight))
    c->weight = floatparm(Opt->default_weight);
}  /* clause_wt_with_adjustments */

/*************
 *
 *   cl_process()
 *
 *   Process a newly inferred (or input) clause.
 *
 *   It is likely that a pointer to this routine was passed to
 *   an inference rule, and that inference rule called this routine
 *   with a new clause.
 *
 *   If this routine decides to keep the clause, it is appended
 *   to the Limbo list rather than to Sos.  Clauses in Limbo have been
 *   kept, but operations that can delete clauses (back subsumption,
 *   back demoulation) have not yet been applied.  The Limbo list
 *   is processed after the inference rule is finished.
 *
 *   Why use the Limbo list?  Because we're not allowed to delete
 *   clauses (back subsumption, back demodulation, back unit deletion)
 *   while inferring clauses.
 *
 *   Why not infer the whole batch of clauses, and then process them?
 *   Because there can be too many.  We have to do demodulation and
 *   subsumption right away, and get kept clauses indexed for
 *   forward demodulation and forward subsumption right away,
 *   so they can be used on the next inferred clause.
 *
 *************/

/* First, some helper routines. */

static
void cl_process_simplify(Topform c)
{
  if (flag(Opt->eval_rewrite)) {
    int count = 0;
    clock_start(Clocks.demod);
    rewrite_with_eval(c);
    if (flag(Opt->print_gen)) {
      printf("%srewrites %d:     ", TPTP_PFX, count);
      fwrite_clause(stdout, c, CL_FORM_STD);
    }
    clock_stop(Clocks.demod);
  }
  else if (demodulation_rules_available()) {
    if (flag(Opt->lex_order_vars)) {
      renumber_variables(c, MAX_VARS);
      c->normal_vars = FALSE;  // demodulation can make vars non-normal
    }
    clock_start(Clocks.demod);
      current_demodulate_clause(c,
			       parm(Opt->demod_step_limit),
			       parm(Opt->demod_increase_limit),
			       !flag(Opt->quiet),
			       flag(Opt->lex_order_vars));
    if (flag(Opt->print_gen)) {
      printf("%srewrite:     ", TPTP_PFX);
      fwrite_clause(stdout, c, CL_FORM_STD);
    }
    clock_stop(Clocks.demod);

    /* Otter-style demod: after subterm demodulators rewrite arguments
       to "junk", any positive literal whose argument contains "junk"
       is effectively a tautology (matching Otter's t(junk) = $T).
       Replace such literals with $T so the tautology check deletes
       the clause. */
    if (flag(otter_style_demod_id())) {
      static int junk_sn = -2;  /* -2 = not yet looked up */
      if (junk_sn == -2)
        junk_sn = str_to_sn("junk", 0);
      if (junk_sn >= 0) {
        Literals lit;
        for (lit = c->literals; lit; lit = lit->next) {
          if (lit->sign && symbol_in_term(junk_sn, lit->atom)) {
            zap_term(lit->atom);
            lit->atom = get_rigid_term(true_sym(), 0);
          }
        }
      }
    }
  }

  orient_equalities(c, TRUE);
  simplify_literals2(c);  // with x=x, and simplify tautologies to $T
  merge_literals(c);

  if (flag(Opt->unit_deletion)) {
    clock_start(Clocks.unit_del);
    unit_deletion(c);
    clock_stop(Clocks.unit_del);
  }

  if (flag(Opt->cac_redundancy)) {
    clock_start(Clocks.redundancy);
    // If comm or assoc, make a note of it.
    // Also simplify C or AC redundant literals to $T.
    if (cac_redundancy(c, !flag(Opt->quiet)))
      c->cac_candidate = 1;
    clock_stop(Clocks.redundancy);
  }
}  // cl_process_simplify

/*************
 *
 *   get_hit_list() -- read "hitlist" file of clause IDs (Veroff feature)
 *
 *************/

static
void get_hit_list(void)
{
  FILE *fp;
  int n;
  fp = fopen("hitlist", "r");
  if (fp == NULL)
    fatal_error("get_hit_list: cannot open file \"hitlist\"");
  Hsize = 0;
  while (fscanf(fp, "%d", &n) == 1) {
    if (Hsize >= MAX_HSIZE) {
      printf("WARNING: hitlist truncated at %d entries.\n", MAX_HSIZE);
      break;
    }
    /* insertion sort to keep list sorted */
    {
      int i, j;
      for (i = 0; i < Hsize && HIT_LIST[i] < n; i++)
	;
      for (j = Hsize; j > i; j--)
	HIT_LIST[j] = HIT_LIST[j-1];
      HIT_LIST[i] = n;
      Hsize++;
    }
  }
  fclose(fp);
  printf("\n%% Hit list (%d entries):", Hsize);
  {
    int i;
    for (i = 0; i < Hsize; i++)
      printf(" %d", HIT_LIST[i]);
  }
  printf("\n");
}  /* get_hit_list */

/*************
 *
 *   on_hit_list() -- check if clause ID is next on the sorted hitlist
 *
 *************/

static
BOOL on_hit_list(int x)
{
  static int next_cl_pos = 0;
  while (next_cl_pos < Hsize && HIT_LIST[next_cl_pos] < x)
    next_cl_pos++;
  if (next_cl_pos < Hsize && HIT_LIST[next_cl_pos] == x) {
    next_cl_pos++;
    /* skip duplicates */
    while (next_cl_pos < Hsize && HIT_LIST[next_cl_pos] == x)
      next_cl_pos++;
    return TRUE;
  }
  return FALSE;
}  /* on_hit_list */

/*************
 *
 *   print_derivation() -- print derivation for a hitlist clause (Veroff feature)
 *
 *************/

static
void print_derivation(Topform cl)
{
  Plist proof, materialized, p;
  static int pfcount = 0;
  proof = get_clause_ancestors(cl);
  restore_archive_hint_links(proof);
  materialized = materialize_clauses(proof);
  pfcount++;
  print_separator(stdout, "PROOF", TRUE);
  printf("\n%% Derivation (Proof) %d (Clause #%llu): ", pfcount, cl->id);
  fwrite_clause(stdout, cl, CL_FORM_BARE);
  printf("\n%% Length of derivation is %d.\n\n", proof_length(proof));
  for (p = proof; p; p = p->next)
    fwrite_clause(stdout, p->v, CL_FORM_STD);
  print_separator(stdout, "end of proof", TRUE);
  recompress_clauses(materialized);
  zap_plist(materialized);
  clause_store_release_materialized_plist(proof);
  zap_plist(proof);

  if (flag(Opt->derivations_only) && Hsize > 0
      && cl->id >= (unsigned) HIT_LIST[Hsize - 1]) {
    printf("\n%% %d derivations completed.  Terminating execution.\n", Hsize);
    done_with_search(ACTION_EXIT);  /* clean exit via longjmp */
  }
}  /* print_derivation */

static
void hint_derivation(Topform cl)
{
  Plist proof, materialized, p;
  static int pfcount = 0;
  proof = get_clause_ancestors(cl);
  restore_archive_hint_links(proof);
  materialized = materialize_clauses(proof);
  pfcount++;
  print_separator(stdout, "PROOF", TRUE);
  printf("\n%% Hint derivation (Proof) %d: ", pfcount);
  fwrite_clause(stdout, cl, CL_FORM_BARE);
  printf("\n%% Length of derivation is %d.\n\n", proof_length(proof));
  for (p = proof; p; p = p->next)
    fwrite_clause(stdout, p->v, CL_FORM_STD);
  print_separator(stdout, "end of proof", TRUE);
  recompress_clauses(materialized);
  zap_plist(materialized);
  clause_store_release_materialized_plist(proof);
  zap_plist(proof);
}  /* hint_derivation */

static
unsigned long long hint_trace_mix_u32(unsigned long long h, unsigned value)
{
  int i;
  for (i = 0; i < 4; i++) {
    h ^= (value >> (8 * i)) & 0xff;
    h *= 1099511628211ULL;
  }
  return h;
}

static
unsigned long long hint_trace_term_hash(Term t, unsigned long long h)
{
  int i;
  h = hint_trace_mix_u32(h, VARIABLE(t) ? 0U : 1U);
  if (VARIABLE(t))
    h = hint_trace_mix_u32(h, (unsigned) VARNUM(t));
  else {
    const unsigned char *s =
      (const unsigned char *) sn_to_str(SYMNUM(t));
    while (*s != '\0') {
      h ^= *s++;
      h *= 1099511628211ULL;
    }
    h ^= 0xfe;
    h *= 1099511628211ULL;
  }
  h = hint_trace_mix_u32(h, (unsigned) ARITY(t));
  for (i = 0; i < ARITY(t); i++)
    h = hint_trace_term_hash(ARG(t, i), h);
  return h;
}

static
unsigned long long hint_trace_clause_hash(Topform c)
{
  unsigned long long h = 1469598103934665603ULL;
  Literals lit;
  for (lit = c->literals; lit != NULL; lit = lit->next) {
    h = hint_trace_mix_u32(h, lit->sign ? 1U : 0U);
    h = hint_trace_term_hash(lit->atom, h);
  }
  return hint_trace_mix_u32(h, 0xffffffffU);
}

static
unsigned long long hint_trace_label_hash(Topform c)
{
  unsigned long long h = 1469598103934665603ULL;
  int occurrence = 1;
  char *label;
  while ((label = get_string_attribute(c->attributes, label_att(),
                                       occurrence++)) != NULL) {
    const unsigned char *s = (const unsigned char *) label;
    while (*s != '\0') {
      h ^= *s++;
      h *= 1099511628211ULL;
    }
    h ^= 0xff;
    h *= 1099511628211ULL;
  }
  return h;
}

/* Normalize a scratch copy enough to predict the authoritative selector and
   exact hint key.  Every operation below is read-only with respect to search
   state: demodulation accounting is suppressed, unit deletion only queries
   its index, and CAC discovery is deliberately excluded (known CAC symbols
   can still simplify the scratch clause). */
static void collective_preview_normalize(Topform c)
{
  if (flag(Opt->eval_rewrite))
    rewrite_with_eval(c);
  else if (demodulation_rules_available()) {
    if (flag(Opt->lex_order_vars)) {
      renumber_variables(c, MAX_VARS);
      c->normal_vars = FALSE;
    }
    if (eager_interreduced_demod_mode())
      compact_rewrite_clause(Compact_rewrite_rules, c,
			     parm(Opt->demod_step_limit),
			     parm(Opt->demod_increase_limit),
			     flag(Opt->lex_order_vars), FALSE);
    else
      demodulate_clause_preview(c,
			       parm(Opt->demod_step_limit),
			       parm(Opt->demod_increase_limit),
			       flag(Opt->lex_order_vars));
    if (flag(otter_style_demod_id())) {
      int junk_sn = str_to_sn("junk", 0);
      Literals lit;
      for (lit = c->literals; lit != NULL; lit = lit->next) {
        if (lit->sign && symbol_in_term(junk_sn, lit->atom)) {
          zap_term(lit->atom);
          lit->atom = get_rigid_term(true_sym(), 0);
        }
      }
    }
  }

  orient_equalities(c, TRUE);
  simplify_literals2(c);
  merge_literals(c);
  if (flag(Opt->unit_deletion))
    unit_deletion(c);
  if (flag(Opt->cac_redundancy) && cac_tautology(c->literals)) {
    zap_literals(c->literals);
    c->literals = get_literals();
    c->literals->sign = TRUE;
    c->literals->atom = get_rigid_term(true_sym(), 0);
    upward_clause_links(c);
  }
}

static void collective_preview_pool_entry(struct collective_pool_entry *e)
{
  Topform scratch = copy_clause_ija(e->clause);
  Topform hint = NULL;
  double weight;
  BOOL flipped = FALSE;
  BOOL tautology;

  collective_preview_normalize(scratch);
  tautology = true_clause(scratch->literals);
  weight = clause_weight(scratch->literals);
  /* cl_process_delete() rejects a true clause before authoritative hint
     matching.  Treat it the same way here: matching the normalized $T body
     would manufacture an advisory hint that can never be confirmed. */
  if (!tautology && !clist_empty(Glob.hints)) {
    if (!scratch->normal_vars)
      renumber_variables(scratch, MAX_VARS);
    hint = preview_weight_with_hints(
      scratch, weight, flag(Opt->degrade_hints),
      flag(Opt->breadth_first_hints), &weight, &flipped);
  }
  scratch->weight = weight;
  scratch->matching_hint = hint;
  {
    int sw = parm(Opt->sine_weight);
    if (sw > 0) {
      int sd = get_int_attribute(scratch->attributes, sine_depth_attr(), 1);
      if (sd != INT_MAX && sd > 1)
        scratch->weight += sw * (sd - 1);
    }
  }
  if (scratch->weight > floatparm(Opt->default_weight) &&
      scratch->weight <= floatparm(Opt->max_weight))
    scratch->weight = floatparm(Opt->default_weight);

  given_selection_preview(scratch, &e->selector_mask,
                          &e->selector_priority);
  e->adjusted_weight = scratch->weight;
  e->hint_id = hint == NULL ? 0 : hint->id;
  e->fingerprint = hint_trace_clause_hash(scratch);
  e->hint_epoch = hint_state_epoch();
  e->simplifier_epoch = Simplifier_epoch;
  Stats.collective_preview_calls++;
  if (hint != NULL)
    Stats.collective_preview_hint_matches++;
  (void) flipped;  /* Identity/weight already capture the flip result. */
  scratch->matching_hint = NULL;
  delete_clause(scratch);
}

static BOOL collective_pool_entry_less(const struct collective_pool_entry *a,
				       const struct collective_pool_entry *b)
{
  unsigned long long ah = a->hint_id == 0 ? ULLONG_MAX : a->hint_id;
  unsigned long long bh = b->hint_id == 0 ? ULLONG_MAX : b->hint_id;
  if (a->selector_priority != b->selector_priority)
    return a->selector_priority < b->selector_priority;
  if (ah != bh)
    return ah < bh;
  if (a->adjusted_weight != b->adjusted_weight)
    return a->adjusted_weight < b->adjusted_weight;
  if (a->given_id != b->given_id)
    return a->given_id < b->given_id;
  if (a->raw_ordinal != b->raw_ordinal)
    return a->raw_ordinal < b->raw_ordinal;
  return a->insertion_ordinal < b->insertion_ordinal;
}

static void collective_candidate_heap_swap(size_t a, size_t b)
{
  struct collective_pool_entry *tmp = Collective_candidate_heap[a];
  Collective_candidate_heap[a] = Collective_candidate_heap[b];
  Collective_candidate_heap[b] = tmp;
}

static void collective_candidate_heap_sift_up(size_t at)
{
  while (at != 0) {
    size_t parent = (at - 1) / 2;
    if (!collective_pool_entry_less(Collective_candidate_heap[at],
                                    Collective_candidate_heap[parent]))
      break;
    collective_candidate_heap_swap(at, parent);
    at = parent;
  }
}

static void collective_candidate_heap_sift_down(size_t at)
{
  while (TRUE) {
    size_t left = at * 2 + 1;
    size_t right = left + 1;
    size_t smallest = at;
    if (left < Collective_candidate_heap_count &&
        collective_pool_entry_less(Collective_candidate_heap[left],
                                   Collective_candidate_heap[smallest]))
      smallest = left;
    if (right < Collective_candidate_heap_count &&
        collective_pool_entry_less(Collective_candidate_heap[right],
                                   Collective_candidate_heap[smallest]))
      smallest = right;
    if (smallest == at)
      break;
    collective_candidate_heap_swap(at, smallest);
    at = smallest;
  }
}

static void collective_candidate_heap_grow(void)
{
  size_t old = Collective_candidate_heap_capacity;
  size_t capacity = old == 0 ? 64 : old * 2;
  if (capacity < old)
    fatal_error("collective candidate heap capacity overflow");
  Collective_candidate_heap = safe_realloc(
    Collective_candidate_heap, capacity * sizeof(*Collective_candidate_heap));
  Collective_candidate_heap_capacity = capacity;
}

static unsigned long long collective_candidate_pool_allocated_bytes(void)
{
  return Collective_candidate_pool_bytes +
    (unsigned long long) Collective_candidate_heap_capacity *
      sizeof(*Collective_candidate_heap);
}

static void collective_candidate_heap_insert(struct collective_pool_entry *e)
{
  size_t at;
  if (Collective_candidate_heap_count == Collective_candidate_heap_capacity)
    collective_candidate_heap_grow();
  at = Collective_candidate_heap_count++;
  Collective_candidate_heap[at] = e;
  collective_candidate_heap_sift_up(at);
}

static struct collective_pool_entry *collective_candidate_heap_remove(size_t at)
{
  struct collective_pool_entry *e;
  if (at >= Collective_candidate_heap_count)
    return NULL;
  e = Collective_candidate_heap[at];
  Collective_candidate_heap_count--;
  if (at != Collective_candidate_heap_count) {
    size_t parent;
    Collective_candidate_heap[at] =
      Collective_candidate_heap[Collective_candidate_heap_count];
    parent = at == 0 ? 0 : (at - 1) / 2;
    if (at != 0 &&
        collective_pool_entry_less(Collective_candidate_heap[at],
                                   Collective_candidate_heap[parent]))
      collective_candidate_heap_sift_up(at);
    else
      collective_candidate_heap_sift_down(at);
  }
  return e;
}

static void collective_candidate_pool_push_at(
  Topform c, struct collective_batch *b, unsigned long long raw_ordinal)
{
  struct collective_pool_entry *e;
  unsigned long long occupied = collective_candidate_occupancy();
  unsigned long long limit =
    (unsigned long long) parm(Opt->collective_candidate_cache);
  unsigned long long bytes;

  if (!collective_balanced_mode() || b == NULL)
    fatal_error("collective candidate callback has no balanced descriptor");
  if (occupied >= limit)
    fatal_error("collective candidate pool bound exceeded");
  e = safe_calloc(1, sizeof(*e));
  e->clause = c;
  e->source = Current_inference_source;
  e->kind = b->kind;
  e->given_id = b->given_id;
  e->raw_ordinal = raw_ordinal;
  e->insertion_ordinal = Collective_candidate_insertion_ordinal++;
  bytes = sizeof(*e) + sizeof(struct topform) +
          clause_body_storage_bytes(c);
  e->bytes = bytes;
  collective_preview_pool_entry(e);
  if (e->hint_id != 0)
    b->discovery_hot = TRUE;
  Collective_candidate_pool_bytes += bytes;
  collective_candidate_heap_insert(e);
  if (Collective_candidate_heap_count >
        (size_t) parm(Opt->collective_candidate_cache))
    fatal_error("collective candidate pool count exceeded hard limit");
  if (collective_candidate_pool_allocated_bytes() >
        Stats.collective_candidate_pool_peak_bytes)
    Stats.collective_candidate_pool_peak_bytes =
      collective_candidate_pool_allocated_bytes();
}

static void collective_candidate_pool_push(Topform c)
{
  struct collective_batch *b = Current_collective_pool_batch;
  unsigned long long ordinal;
  if (b == NULL || Current_collective_discovery)
    fatal_error("collective fair candidate callback has invalid context");
  ordinal = b->candidate_ordinal++;
  if (!collective_consume_if_promoted(b, ordinal, c))
    collective_candidate_pool_push_at(c, b, ordinal);
}

static void collective_discovery_candidate(Topform c)
{
  struct collective_batch *b = Current_collective_pool_batch;
  struct collective_pool_entry *e;
  unsigned long long ordinal, raw_fingerprint, distance;
  unsigned long long limit =
    (unsigned long long) parm(Opt->collective_candidate_cache);
  unsigned promotion_cap =
    (unsigned) parm(Opt->collective_discovery_promotion_cap);

  if (b == NULL || !Current_collective_discovery)
    fatal_error("collective discovery callback has invalid context");
  ordinal = b->discovery_ordinal++;
  raw_fingerprint = hint_trace_clause_hash(c);
  e = safe_calloc(1, sizeof(*e));
  e->clause = c;
  e->source = Current_inference_source;
  e->kind = b->kind;
  e->given_id = b->given_id;
  e->raw_ordinal = ordinal;
  e->discovery_promotion = TRUE;
  collective_preview_pool_entry(e);
  Stats.collective_discovery_candidates++;

  if (e->hint_id != 0)
    b->discovery_hot = TRUE;
  if (e->hint_id != 0 &&
      b->consumed_count < promotion_cap &&
      collective_candidate_occupancy() < limit) {
    e->insertion_ordinal = Collective_candidate_insertion_ordinal++;
    e->bytes = sizeof(*e) + sizeof(struct topform) +
               clause_body_storage_bytes(c);
    collective_add_consumed(b, ordinal, raw_fingerprint);
    Collective_candidate_pool_bytes += e->bytes;
    collective_candidate_heap_insert(e);
    Stats.collective_discovery_promotions++;
    distance = ordinal + 1 > b->candidate_ordinal ?
      ordinal + 1 - b->candidate_ordinal : 0;
    if (distance > Stats.collective_discovery_distance_max)
      Stats.collective_discovery_distance_max = distance;
    if (collective_candidate_pool_allocated_bytes() >
          Stats.collective_candidate_pool_peak_bytes)
      Stats.collective_candidate_pool_peak_bytes =
        collective_candidate_pool_allocated_bytes();
  }
  else {
    if (e->hint_id != 0 && b->consumed_count >= promotion_cap)
      Stats.collective_discovery_cap_stalls++;
    delete_clause(c);
    safe_free(e);
  }
}

static void collective_candidate_pool_clear(void)
{
  size_t i;
  for (i = 0; i < Collective_candidate_heap_count; i++) {
    struct collective_pool_entry *e = Collective_candidate_heap[i];
    delete_clause(e->clause);
    safe_free(e);
  }
  safe_free(Collective_candidate_heap);
  Collective_candidate_heap = NULL;
  Collective_candidate_heap_count = 0;
  Collective_candidate_heap_capacity = 0;
  Collective_candidate_pool_bytes = 0;
}

static void collective_candidate_pool_refresh_top(void)
{
  struct collective_pool_entry *e;
  if (Collective_candidate_heap_count == 0)
    return;
  e = Collective_candidate_heap[0];
  if (e->hint_epoch != hint_state_epoch() ||
      e->simplifier_epoch != Simplifier_epoch) {
    collective_preview_pool_entry(e);
    Stats.collective_preview_stale_refreshes++;
    /* A refreshed top can move down.  A stale non-top is validated if and
       when it reaches the top; this is the deterministic lazy-refresh rule. */
    collective_candidate_heap_sift_down(0);
  }
}

static
void print_hint_trace(Topform c)
{
  unsigned long long matcher = c->matching_hint == NULL ? 0 :
                               c->matching_hint->id;
  unsigned long long degradation = c->matching_hint == NULL ? 0 :
                                   (unsigned long long) c->matching_hint->weight;
  double raw_weight = clause_weight(c->literals);
  printf("%sHINT_TRACE clause=%llu fingerprint=%016llx matcher=%llu "
         "raw=%.17g adjusted=%.17g labels=%016llx degradation=%llu "
         "epoch=%llu\n",
         TPTP_PFX, c->id, hint_trace_clause_hash(c), matcher,
         raw_weight, c->weight, hint_trace_label_hash(c), degradation,
         hint_state_epoch());
}  /* print_hint_trace */

static
void cl_process_keep(Topform c)
{
  Stats.kept++;
  if (!c->normal_vars)
    renumber_variables(c, MAX_VARS);
  if (c->id == 0)
    assign_clause_id(c);  // unit conflict or input: already has ID
  if (c->cac_candidate && !ilist_member(Glob.cac_clauses, (int) c->id))
    Glob.cac_clauses = ilist_prepend(Glob.cac_clauses, (int) c->id);
  if (flag(Opt->print_derivations) && on_hit_list(c->id))
    print_derivation(c);
  if (To_trace_id != 0 && c->id == To_trace_id) {
    To_trace_cl = c;
    printf("\n*** Trace: clause %llu kept.\n", c->id);
  }
  mark_parents_as_used(c);
  mark_maximal_literals(c->literals);
  mark_selected_literals(c->literals, stringparm1(Opt->literal_selection));
  if (c->matching_hint != NULL) {
    keep_hint_matcher(c);
    if (parm(Opt->hint_derivations) > 0
	&& c->matching_hint->id < (unsigned) parm(Opt->hint_derivations))
      hint_derivation(c);
  }
  if (flag(Opt->hint_trace))
    print_hint_trace(c);
  if (flag(Opt->search_event_trace))
    printf("%sKEPT_TRACE kept=%llu generated=%llu clause=%llu "
           "fingerprint=%016llx\n",
           TPTP_PFX, Stats.kept, Stats.generated, c->id,
           hint_trace_clause_hash(c));

  if (flag(Opt->print_clause_properties))
      c->attributes = set_term_attribute(c->attributes,
					 Att.properties,
					 topform_properties(c));
  if (flag(Opt->print_kept) || flag(Opt->print_gen) ||
      (!Glob.searching && flag(Opt->print_initial_clauses))) {
    printf("%skept:      ", TPTP_PFX);
    fwrite_clause(stdout, c, CL_FORM_STD);
  }
  else if (flag(Opt->print_labeled) &&
	   get_string_attribute(c->attributes, Att.label, 1)) {
    printf("\n%skept:      ", TPTP_PFX);
    fwrite_clause(stdout, c, CL_FORM_STD);
  }
  statistic_actions("kept", clause_ids_assigned());  /* Note different stat */
}  // cl_process_keep

static
void cl_process_conflict(Topform c, BOOL denial)
{
  if (number_of_literals(c->literals) == 1) {
    if (!c->normal_vars)
      renumber_variables(c, MAX_VARS);
    clock_start(Clocks.conflict);
    unit_conflict(c, handle_proof_and_maybe_exit);
    clock_stop(Clocks.conflict);
  }
}  // cl_process_conflict

static unsigned long long rewrite_only_clone_bytes(Topform c)
{
  /* The legacy oracle intentionally owns a complete proof clone.  This
     accounted figure includes its hash slot, Topform, and term/literal body;
     allocator totals and RSS remain authoritative for copied justification
     and attribute payloads that do not expose per-object byte walkers. */
  return sizeof(struct topform) + clause_body_storage_bytes(c);
}

static unsigned long long rewrite_only_shell_bytes(Topform c)
{
  /* The compact bank owns the matching terms.  The shell retains only a
     compressed printable body plus live attributes/ancestry for proof-ID
     lookup; allocator/RSS statistics account for the latter payloads. */
  return sizeof(struct topform) + c->compressed_size;
}

static void print_new_demodulator(Topform c, int type,
                                  const char *ownership)
{
  if (flag(Opt->print_kept)) {
    char *s;
    switch(type) {
    case ORIENTED:     s = ""; break;
    case LEX_DEP_LR:   s = " (lex_dep_lr)"; break;
    case LEX_DEP_RL:   s = " (lex_dep_rl)"; break;
    case LEX_DEP_BOTH: s = " (lex_dep_both)"; break;
    default:           s = " (?)";
    }
    printf("%s    new %sdemodulator%s: %llu.\n", TPTP_PFX,
           ownership, s, c->id);
  }
}

static Topform store_rewrite_only_clone(Topform c, int type)
{
  Topform clone = copy_clause_ija(c);
  clone->normal_vars = c->normal_vars;
  clone->initial = c->initial;
  clone->weight = c->weight;
  clone->semantics = c->semantics;
  clone->simplifier_epoch = c->simplifier_epoch;
  clone->rewrite_epoch = c->rewrite_epoch;
  if (!rewrite_only_store_insert(Rewrite_only_rules, clone, type,
                                 rewrite_only_clone_bytes(clone))) {
    clone->id = 0;
    delete_clause(clone);
    fatal_error("store_rewrite_only_clone: duplicate proof ID");
  }
  update_rewrite_only_stats();
  return clone;
}

static Topform store_rewrite_only_shell(Topform c, int type)
{
  Topform shell = copy_clause_ija(c);
  shell->normal_vars = c->normal_vars;
  shell->initial = c->initial;
  shell->weight = c->weight;
  shell->semantics = c->semantics;
  shell->simplifier_epoch = c->simplifier_epoch;
  shell->rewrite_epoch = c->rewrite_epoch;
  if (compress_clause(shell) != CLAUSE_COMPRESS_OK) {
    shell->id = 0;
    delete_clause(shell);
    fatal_error("store_rewrite_only_shell: body cannot be compressed");
  }
  if (!rewrite_only_store_insert(Rewrite_only_rules, shell, type,
                                 rewrite_only_shell_bytes(shell))) {
    shell->id = 0;
    delete_clause(shell);
    fatal_error("store_rewrite_only_shell: duplicate proof ID");
  }
  update_rewrite_only_stats();
  return shell;
}

static Topform resolve_rewrite_only_demodulator(unsigned long long id,
                                                void *context)
{
  (void) context;
  return rewrite_only_store_find(Rewrite_only_rules, id, NULL, NULL);
}

struct compact_restore_rule {
  Topform clause;
  int type;
  BOOL cold;
};

static int compact_restore_rule_compare(const void *left, const void *right)
{
  const struct compact_restore_rule *a = left;
  const struct compact_restore_rule *b = right;
  return a->clause->id < b->clause->id ? -1 :
         a->clause->id > b->clause->id ? 1 : 0;
}

static void restore_compact_rewrite_bank(void)
{
  struct compact_restore_rule *rules;
  size_t capacity = (size_t) Glob.demods->length + Glob.sos->length;
  size_t count = 0, i;
  Clist_pos p;

  rules = capacity == 0 ? NULL :
    safe_malloc(capacity * sizeof(*rules));
  for (p = Glob.demods->first; p != NULL; p = p->next) {
    int type = demodulator_type(p->c, parm(Opt->lex_dep_demod_lim),
                                flag(Opt->lex_dep_demod_sane));
    if (type == NOT_DEMODULATOR)
      fatal_error("resume: active compact demodulator changed type");
    rules[count].clause = p->c;
    rules[count].type = type;
    rules[count++].cold = FALSE;
  }
  for (p = Glob.sos->first; p != NULL; p = p->next) {
    int type = (compact_otter_demod_mode() || p->c->delayed_demodulator) ?
      demodulator_type(p->c, parm(Opt->lex_dep_demod_lim),
                       flag(Opt->lex_dep_demod_sane)) : NOT_DEMODULATOR;
    if (type != NOT_DEMODULATOR) {
      rules[count].clause = p->c;
      rules[count].type = type;
      rules[count++].cold = eager_interreduced_demod_mode();
    }
  }
  if (count > 1)
    qsort(rules, count, sizeof(*rules), compact_restore_rule_compare);
  compact_rewrite_restore_counters(
    Compact_rewrite_rules, Stats.compact_rewrite_rules_peak,
    Stats.compact_rewrite_rules_retired, Stats.compact_rewrite_attempts,
    Stats.compact_rewrite_rewrites, Stats.compact_rewrite_compactions,
    Stats.compact_rewrite_bytes_reclaimed);
  for (i = 0; i < count; i++) {
    if (!compact_rewrite_add(Compact_rewrite_rules, rules[i].clause,
                             rules[i].type))
      fatal_error("resume: duplicate compact rewrite proof ID");
    if (rules[i].cold)
      store_rewrite_only_shell(rules[i].clause, rules[i].type);
  }
  safe_free(rules);
  update_rewrite_only_stats();
}

static void admit_rewrite_only_demodulator(Topform c, int type)
{
  Topform clone = store_rewrite_only_clone(c, type);
  index_demodulator(clone, type, INSERT, Clocks.index);
  Stats.rewrite_only_demodulators_admitted++;
  Stats.new_demodulators++;
  if (type != ORIENTED)
    Stats.new_lex_demods++;
  print_new_demodulator(c, type, "rewrite-only ");
  back_demod_hints(clone, type, flag(Opt->lex_order_vars));
  if (Simplifier_epoch != UINT_MAX)
    Simplifier_epoch++;
  advance_rewrite_epoch();
}

static void mark_compact_overlap(unsigned long long proof_id, void *context)
{
  (void) context;
  Stats.rewrite_overlap_visits++;
  if (dense_passive_mark_rule_dirty(proof_id))
    Stats.rewrite_overlap_dirty_marks++;
}

static void mark_compact_interreduction_candidates(Topform new_rule)
{
  compact_rewrite_visit_overlaps(Compact_rewrite_rules, new_rule->id,
                                 mark_compact_overlap, NULL);
}

static void admit_compact_rewrite_demodulator(Topform c, int type)
{
  if (!compact_rewrite_add(Compact_rewrite_rules, c, type))
    fatal_error("admit_compact_rewrite_demodulator: duplicate proof ID");
  /* A primary inference starts one exact backward-composition wave.  A rule
     produced by that wave is already normalized against the current bank;
     installing it makes the stronger rule immediately available to every
     clause and hint consumer.  Do not recursively start another rule-only
     wave from the replacement.  Osborn showed that transitive replacement
     propagation can replenish one dirty rule per repair forever, creating
     proof-only clauses and mmap ancestry without advancing inference.

     This bound does not defer forward demodulation: the replacement is in
     the bank before cl_process continues, hints are still back-demodulated
     below, and stale passives are normalized on repair or selection. */
  if (Rewrite_repair_depth == 0)
    mark_compact_interreduction_candidates(c);
  else
    Stats.rewrite_cascade_suppressed++;
  store_rewrite_only_shell(c, type);
  Stats.rewrite_only_demodulators_admitted++;
  Stats.new_demodulators++;
  if (type != ORIENTED)
    Stats.new_lex_demods++;
  print_new_demodulator(c, type, "compact rewrite-only ");
  back_demod_hints(c, type, flag(Opt->lex_order_vars));
  if (Simplifier_epoch != UINT_MAX)
    Simplifier_epoch++;
  advance_rewrite_epoch();
  update_rewrite_only_stats();
}

static
void cl_process_new_demod(Topform c, BOOL rewrite_transition)
{
  /* In the DISCOUNT loop, an ordinary kept clause remains passive until it
     is selected.  Promoting it here would make the passive frontier part of
     the active rewrite system, which is precisely the ownership violation
     this mode removes.  Restricted denials are placed directly in Usable and
     retain their historical treatment. */
  if (discount_mode() && !c->was_given && !restricted_denial(c) &&
      !maximum_discount_demod_mode())
    return;

  // If the clause should be a demodulator, make it so.
  if (flag(Opt->back_demod)) {
    int type = demodulator_type(c,
				parm(Opt->lex_dep_demod_lim),
				flag(Opt->lex_dep_demod_sane));
    if (type != NOT_DEMODULATOR) {
      if (maximum_discount_demod_mode() && !c->was_given &&
          !restricted_denial(c)) {
        if (eager_interreduced_demod_mode())
          admit_compact_rewrite_demodulator(c, type);
        else
          admit_rewrite_only_demodulator(c, type);
        return;
      }
      print_new_demodulator(c, type, "");
      clist_append(c, Glob.demods);
      if (eager_interreduced_demod_mode() || compact_otter_demod_mode()) {
        if (!compact_rewrite_add(Compact_rewrite_rules, c, type))
          fatal_error("cl_process_new_demod: compact rule already present");
        if (eager_interreduced_demod_mode() && !rewrite_transition)
          mark_compact_interreduction_candidates(c);
        if (compact_rewrite_compaction_needed(Compact_rewrite_rules))
          compact_rewrite_compact(Compact_rewrite_rules);
        update_rewrite_only_stats();
      }
      else {
        if (compact_otter_audit_mode()) {
          if (!compact_rewrite_add(Compact_rewrite_rules, c, type))
            fatal_error("cl_process_new_demod: audited compact rule already present");
          update_rewrite_only_stats();
        }
        index_demodulator(c, type, INSERT, Clocks.index);
      }
      if (!rewrite_transition) {
        Stats.new_demodulators++;
        if (type != ORIENTED)
          Stats.new_lex_demods++;
        back_demod_hints(c, type, flag(Opt->lex_order_vars));
      }
    }
    else if (rewrite_transition)
      fatal_error("cl_process_new_demod: selected rewrite rule changed type");
  }
}  // cl_process_new_demod

static
void prepare_discount_passive(Topform c, BOOL stamp_epoch)
{
  int type = NOT_DEMODULATOR;

  if (stamp_epoch)
    c->simplifier_epoch = Simplifier_epoch;
  if (stamp_epoch)
    c->rewrite_epoch = Rewrite_epoch;
  if (flag(Opt->back_demod) &&
      !(eager_interreduced_demod_mode() && !stamp_epoch &&
        !c->delayed_demodulator))
    type = demodulator_type(c,
			    parm(Opt->lex_dep_demod_lim),
			    flag(Opt->lex_dep_demod_sane));
  c->delayed_demodulator = type != NOT_DEMODULATOR;

  /* stamp_epoch is TRUE only for a newly admitted passive.  Checkpoint
     reconstruction deliberately passes FALSE, so these are cumulative
     admissions rather than repeated observations of restored bodies. */
  if (stamp_epoch && type != NOT_DEMODULATOR) {
    Stats.passive_demodulator_candidates++;
    if (type == ORIENTED)
      Stats.passive_oriented_demodulator_candidates++;
    else
      Stats.passive_lex_demodulator_candidates++;
  }

  if (compressed_passive_mode()) {
    Clause_compress_result result = compress_clause_with_justification(c);
    if (result != CLAUSE_COMPRESS_OK && result != CLAUSE_COMPRESS_ALREADY)
      fatal_error("prepare_discount_passive: clause body cannot be compressed");
  }
}  /* prepare_discount_passive */

static
BOOL skip_black_white_tests(Topform c)
{
  return (!Glob.searching ||
	  c->used ||
	  restricted_denial(c) ||
	  (c->matching_hint  != NULL && !flag(Opt->limit_hint_matchers)));
}  /* skip_black_white_tests */


static
BOOL cl_process_delete(Topform c)
{
  // Should the clause be deleted (tautology, limits, subsumption)?

  if (true_clause(c->literals)) {  // tautology
    if (flag(Opt->print_gen))
      printf("%stautology\n", TPTP_PFX);
    Stats.subsumed++;
    return TRUE;  // delete
  }

  clause_wt_with_adjustments(c);  // possibly sets c->matching_hint

  // White-black tests

  if (!skip_black_white_tests(c)) {
    if (white_tests(c)) {
      if (flag(Opt->print_gen))
	printf("%skeep_rule applied.\n", TPTP_PFX);
      Stats.kept_by_rule++;
    }
    else {
      if (black_tests(c)) {
	if (flag(Opt->print_gen))
	  printf("%sdelete_rule applied.\n", TPTP_PFX);
	Stats.deleted_by_rule++;
	return TRUE;  //delete
      }
      else if (!sos_keep2(c, Glob.sos, Opt)) {
	if (flag(Opt->print_gen))
	  printf("%ssos_limit applied.\n", TPTP_PFX);
	Stats.sos_limit_deleted++;
	return TRUE;  // delete
      }
    }
  }
      
  // Forward subsumption

  {
    Topform subsumer;
    clock_start(Clocks.subsume);
    if (flag(Opt->ancestor_subsume)) {
      /* Iterate through ALL generalizers (not just the first the index
         returns) and accept the first whose proof is no longer than c's.
         If the index's first hit happens to be a high-cost variant,
         anc_subsume blocks it -- without iteration we'd miss a low-cost
         variant later in the same index that would have been a valid
         subsumer.  Otter's clause.c:1380-1466 does this iteration. */
      BOOL use_prf_weight = flag(Opt->proof_weight);
      subsumer = forward_subsumption_filter(c, anc_subsume_accept_cb,
                                            &use_prf_weight);
    } else {
      subsumer = forward_subsumption(c);
    }
    clock_stop(Clocks.subsume);
    if (subsumer != NULL && !c->used) {
      unsigned long long subsumer_id = subsumer->id;
      release_compact_index_clause(subsumer);
      if (flag(Opt->print_gen))
	printf("%ssubsumed by %llu.\n", TPTP_PFX, subsumer_id);
      Stats.subsumed++;
      return TRUE;  // delete
    }
    else {
      release_compact_index_clause(subsumer);
      return FALSE;  // keep the clause
    }
  }
}  // cl_process_delete

static
void cl_process(Topform c)
{
  // If the infer_clock is running, stop it and restart it when done.

  BOOL infer_clock_stopped = FALSE;
  if (clock_running(Clocks.infer)) {
    clock_stop(Clocks.infer);
    infer_clock_stopped = TRUE;
  }
  clock_start(Clocks.preprocess);

  exit_if_over_limit();
  if (parm(Opt->report) > 0 || parm(Opt->report_stderr) > 0 || parm(Opt->report_given) > 0)
    possible_report();

  Stats.generated++;
  switch (Current_inference_source) {
  case INFER_SOURCE_BINARY: Stats.generated_binary++; break;
  case INFER_SOURCE_HYPER:  Stats.generated_hyper++; break;
  case INFER_SOURCE_UR:     Stats.generated_ur++; break;
  case INFER_SOURCE_PARAMOD: Stats.generated_paramod++; break;
  default: Stats.generated_other++; break;
  }
  statistic_actions("generated", Stats.generated);
  if (flag(Opt->print_gen)) {
    printf("\n%sgenerated: ", TPTP_PFX);
    fwrite_clause(stdout, c, CL_FORM_STD);
  }

  cl_process_simplify(c);            // all simplification

  if (number_of_literals(c->literals) == 0)    // empty clause
    handle_proof_and_maybe_exit(c);
  else {
    // Do safe unit conflict before any deletion checks.
    if (flag(Opt->safe_unit_conflict))
      cl_process_conflict(c, FALSE);  // marked as used if conflict

    {
      BOOL deleted = cl_process_delete(c);
      if (Current_collective_commit_entry != NULL) {
        unsigned long long actual_hint = c->matching_hint == NULL ? 0 :
                                         c->matching_hint->id;
        unsigned long long predicted_hint =
          Current_collective_commit_entry->hint_id;
        if (actual_hint != 0)
          Stats.collective_preview_authoritative_matches++;
        if (predicted_hint != 0 && actual_hint == 0)
          Stats.collective_preview_false_positives++;
        if (predicted_hint != 0 && actual_hint != 0 &&
            predicted_hint != actual_hint)
          Stats.collective_preview_changed_hint_ids++;
        if (Current_collective_commit_entry->discovery_promotion) {
          if (actual_hint != 0)
            Stats.collective_discovery_confirmed++;
          else
            Stats.collective_discovery_false_positives++;
        }
      }
      if (flag(Opt->hint_trace))
        {
          unsigned long long fingerprint = hint_trace_clause_hash(c);
          printf("%sCANDIDATE_TRACE generated=%llu fingerprint=%016llx "
                 "outcome=%s\n",
                 TPTP_PFX, Stats.generated, fingerprint,
                 deleted ? "deleted" : "kept");
        }
      if (deleted)
        delete_clause(c);
      else {
        cl_process_keep(c);
        // Ordinary unit conflict.
        if (!flag(Opt->safe_unit_conflict))
	  cl_process_conflict(c, FALSE);
        cl_process_new_demod(c, FALSE);
        // We insert c into the literal index now so that it will be
        // available for unit conflict and forward subsumption while
        // it's in limbo.  (It should not be back subsumed while in limbo.
        // See fatal error in limbo_process).
        if (!discount_mode() || c->was_given || restricted_denial(c))
          index_literals(c, INSERT, Clocks.index, FALSE);
        clist_append(c, Glob.limbo);
      }  // not deleted
    }
  }  // not empty clause
  
  clock_stop(Clocks.preprocess);
  if (infer_clock_stopped)
    clock_start(Clocks.infer);
}  // cl_process

static BOOL collective_candidate_pool_commit(void)
{
  struct collective_pool_entry *e;
  enum inference_source saved_source;
  unsigned interval =
    (unsigned) parm(Opt->collective_candidate_fair_interval);
  BOOL fair = interval == 1 ||
              Collective_pool_commits_since_fair >= interval - 1;
  size_t at = 0;

  if (fair && Collective_candidate_heap_count != 0) {
    size_t i;
    for (i = 1; i < Collective_candidate_heap_count; i++)
      if (Collective_candidate_heap[i]->insertion_ordinal <
            Collective_candidate_heap[at]->insertion_ordinal)
        at = i;
    e = Collective_candidate_heap[at];
    if (e->hint_epoch != hint_state_epoch() ||
        e->simplifier_epoch != Simplifier_epoch) {
      collective_preview_pool_entry(e);
      Stats.collective_preview_stale_refreshes++;
    }
  }
  else {
    while (Collective_candidate_heap_count != 0 &&
           (Collective_candidate_heap[0]->hint_epoch != hint_state_epoch() ||
            Collective_candidate_heap[0]->simplifier_epoch !=
              Simplifier_epoch))
      collective_candidate_pool_refresh_top();
    at = 0;
  }
  e = collective_candidate_heap_remove(at);
  if (e == NULL)
    return FALSE;
  if (Collective_candidate_pool_bytes < e->bytes)
    fatal_error("collective candidate pool byte accounting underflow");
  Collective_candidate_pool_bytes -= e->bytes;
  saved_source = Current_inference_source;
  Current_inference_source = e->source;
  Current_collective_commit_entry = e;
  cl_process(e->clause);
  Current_collective_commit_entry = NULL;
  Current_inference_source = saved_source;
  safe_free(e);
  Stats.collective_candidate_pool_commits++;
  if (fair) {
    Stats.collective_candidate_fair_commits++;
    Collective_pool_commits_since_fair = 0;
  }
  else {
    Stats.collective_candidate_priority_commits++;
    Collective_pool_commits_since_fair++;
  }
  Collective_pool_expansions_since_commit = 0;
  return TRUE;
}

/*************
 *
 *   back_demod()
 *
 *   For each clause that can be back demodulated, make a copy,
 *   disable the original, cl_process the copy (as if it
 *   had just been inferred).
 *
 *************/

static
void back_demod(Topform demod)
{
  Plist results, p, prev;

  clock_start(Clocks.back_demod);
  results = back_demodulatable(demod,
			       demodulator_type(demod,
						parm(Opt->lex_dep_demod_lim),
						flag(Opt->lex_dep_demod_sane)),
			       flag(Opt->lex_order_vars));
  clock_stop(Clocks.back_demod);
  p = results;
  while(p != NULL) {
    Topform old = p->v;
    if (!clause_store_member(Glob.disabled, old) ||
        dense_passive_contains_id(old->id)) {
      Topform new;
      if (flag(Opt->basic_paramodulation))
	new = copy_clause_with_flag(old, nonbasic_flag());
      else
	new = copy_clause(old);
      Stats.back_demodulated++;
      if (flag(Opt->print_kept))
	printf("%s        %llu back demodulating %llu.\n", TPTP_PFX, demod->id, old->id);
      if (To_trace_cl == old) {
	printf("\n*** Trace: clause %llu back demodulated by %llu.\n",
	       old->id, demod->id);
	To_trace_cl = NULL;
      }
      new->justification = back_demod_just(old);
      new->attributes = inheritable_att_instances(old->attributes, NULL);
      disable_clause(old);
      cl_process(new);
    }
    else
      compact_otter_release_clause(old, NULL);
    prev = p;
    p = p->next;
    free_plist(prev);
  }
}  // back_demod

/*************
 *
 *   back_unit_deletion()
 *
 *   For each clause that can be back unit deleted, make a copy,
 *   disable the original, cl_process the copy (as if it
 *   had just been inferred).
 *
 *************/

static
void back_unit_deletion(Topform unit)
{
  Plist results, p, prev;

  clock_start(Clocks.back_unit_del);
  results = back_unit_deletable(unit);
  clock_stop(Clocks.back_unit_del);
  p = results;
  while(p != NULL) {
    Topform old = p->v;
    if (!clause_store_member(Glob.disabled, old)) {
      Topform new;
      if (flag(Opt->basic_paramodulation))
	new = copy_clause_with_flag(old, nonbasic_flag());
      else
	new = copy_clause(old);
      Stats.back_unit_deleted++;
      if (flag(Opt->print_kept))
	printf("%s        %llu back unit deleting %llu.\n", TPTP_PFX, unit->id, old->id);
      new->justification = back_unit_deletion_just(old);
      new->attributes = inheritable_att_instances(old->attributes, NULL);
      disable_clause(old);
      cl_process(new);
    }
    prev = p;
    p = p->next;
    free_plist(prev);
  }
}  // back_unit_deletion

/*************
 *
 *   back_cac_simplify()
 *
 *   For each clause that can be back unit deleted, make a copy,
 *   disable the original, cl_process the copy (as if it
 *   had just been inferred).
 *
 *************/

static
void back_cac_simplify(void)
{
  Plist to_delete = NULL;
  Plist a;
  Clist_pos p;
  for (p = Glob.sos->first; p; p = p->next) {
    if (cac_tautology(p->c->literals))
      to_delete = plist_prepend(to_delete, p->c);
  }
  for (p = Glob.usable->first; p; p = p->next) {
    if (cac_tautology(p->c->literals))
      to_delete = plist_prepend(to_delete, p->c);
  }
  for (p = Glob.limbo->first; p; p = p->next) {
    if (cac_tautology(p->c->literals))
      to_delete = plist_prepend(to_delete, p->c);
  }
  for (a = to_delete; a; a = a->next) {
    if (!flag(Opt->quiet)) {
      printf("%% back CAC tautology: "); f_clause(a->v);
    }
    disable_clause(a->v);
  }
  zap_plist(to_delete);  /* shallow */
}  // back_cac_simplify

/*************
 *
 *   disable_to_be_disabled()
 *
 *************/

static
void disable_to_be_disabled(void)
{
  if (Glob.desc_to_be_disabled) {

    Ilist descendants = NULL;
    Ilist p;

    clause_store_sort_by_id(Glob.disabled);

    for (p = Glob.desc_to_be_disabled; p; p = p->next) {
      Ilist x = neg_descendant_ids(p->i, Glob.usable, Glob.sos,
                                  Glob.disabled);
      Ilist q;
      for (q = x; q != NULL; q = q->next)
        if (!ilist_member(descendants, q->i))
          descendants = ilist_insert_up(descendants, q->i);
      zap_ilist(x);
    }
    
    if (!flag(Opt->quiet)) {
      int n = 0;
      printf("\n%% Disable descendants (x means already disabled):\n");
      for (p = descendants; p; p = p->next) {
	printf(" %d%s", p->i,
               clause_id_is_archived((unsigned) p->i) ? "x" : "");
	if (++n % 10 == 0)
	  printf("\n");
      }
      printf("\n");
    }

    for (p = descendants; p; p = p->next) {
      Topform d = find_clause_by_id((unsigned) p->i);
      if (d != NULL)
	disable_clause(d);
    }

    zap_ilist(descendants);
    zap_ilist(Glob.desc_to_be_disabled);
    Glob.desc_to_be_disabled = NULL;
  }
}  /* disable_to_be_disabled */

/*************
 *
 *   degradation_count() -- BV(2016-feb-02)
 *
 *   Degraded weight of c is weight(c) + degradation_count * 1000.
 *
 *************/

static int degradation_count(Topform c)
{
  return (int)(c->weight) / 1000;
}  /* degradation_count */

/*************
 *
 *   limbo_process()
 *
 *   Apply back subsumption and back demodulation to the Limbo
 *   list.  Since back demodulated clauses are cl_processed,
 *   the Limbo list can grow while it is being processed.
 *
 *   If this prover had a hot-list, or any other feature that
 *   generates clauses from newly kept clauses, it probably would
 *   be done here.
 *
 *   The Limbo list operates as a queue.  An important property
 *   of the Limbo list is that if A is ahead of B, then A does
 *   not simplify or subsume B.  However, B can simplify or subsume A.
 *
 *************/

static
void limbo_process(BOOL pre_search)
{
  int lp_count = 0;
  double lp_next = preprocessing_report_starting();

  while (Glob.limbo->first) {
    Topform c = Glob.limbo->first->c;
    BOOL discount_activation = discount_mode() && c->was_given;
    double iter_start = (lp_next > 0) ? user_seconds() : 0;

    /* Timeout is handled by SIGALRM (setup_timeout_signal) */

    lp_count++;
    if (lp_next > 0) {
      double now = iter_start;
      if (now >= lp_next) {
	fprintf(stderr,
		"NOTE: preprocessing limbo processed %d, remaining %d"
		" (%.1f sec, %lld MB)\n",
		lp_count, clist_length(Glob.limbo), now, megs_malloced());
	fflush(stderr);
	lp_next = now + parm(Opt->report_preprocessing);
      }
    }

    /* A DISCOUNT passive has already been simplified, weighed, and matched
       against hints by cl_process().  Commit it only to the selector here.
       Factoring and every backward operation are activation-time work; doing
       them now would turn an unselected clause into active search state. */
    if (discount_mode() && !discount_activation && !restricted_denial(c)) {
      clist_remove(c, Glob.limbo);
      if (parm(Opt->sos_limit) != -1 &&
	  clist_length(Glob.sos) >= parm(Opt->sos_limit)) {
	sos_displace2(disable_clause, flag(Opt->quiet));
	Stats.sos_displaced++;
      }
      c->initial = pre_search ? TRUE : FALSE;
      prepare_discount_passive(c, TRUE);
      if (maximum_discount_demod_mode() && c->delayed_demodulator) {
        int type;
        Topform owner = rewrite_only_store_find(Rewrite_only_rules, c->id,
                                                &type, NULL);
        if (owner == NULL)
          fatal_error("limbo_process: eager rewrite rule is missing");
        if (flag(Opt->print_kept))
          printf("%s    starting rewrite-only back demodulation with %llu.\n",
                 TPTP_PFX, c->id);
        if (eager_interreduced_demod_mode()) {
          /* prepare_discount_passive() has already packed the body for the
             dense archive.  Materialize it only across the active back-demod
             query; the proof shell intentionally cannot supply match terms. */
          if (!materialize_clause(c))
            fatal_error("limbo_process: cannot materialize compact demodulator");
          back_demod(c);
          if (!recompress_clause(c))
            fatal_error("limbo_process: cannot repack compact demodulator");
        }
        else
          back_demod(owner);
      }
      insert_into_sos2(c, Glob.sos);
      continue;
    }

    // factoring

    if (flag(Opt->factor))
      binary_factors(c, cl_process);

    // Try to apply new_constant rule.

    if (!at_parm_limit(Stats.new_constants, Opt->new_constants)) {
      Topform new = new_constant(c, INT_MAX);
      if (new) {
	Stats.new_constants++;
	if (!flag(Opt->quiet)) {
	  printf("\nNOTE: New constant: ");
	  f_clause(new);
	  printf("NOTE: New ");
	  print_fsym_precedence(stdout);
	}
	if (Glob.interps)
	  update_semantics_new_constant(new);
	cl_process(new);
      }
    }

    // fold denial (for input clauses only)

    if (parm(Opt->fold_denial_max) > 1 &&
	(has_input_just(c) || has_copy_just(c))) {
      Topform new = fold_denial(c, parm(Opt->fold_denial_max));
      if (new) {
	if (!flag(Opt->quiet)) {
	  printf("\nNOTE: Fold denial: ");
	  f_clause(new);
	}
	cl_process(new);
      }
    }

    // Disable clauses subsumed by c (back subsumption).

    if (flag(Opt->back_subsume)) {
      Plist subsumees;
      /* BV(2016-feb-02): degradation count tracking for weight reset */
      int Dcount_c = degradation_count(c);
      int Dcount_min_sos = Dcount_c;
      int Dcount_min_not_sos = Dcount_c;

      clock_start(Clocks.back_subsume);
      subsumees = back_subsumption(c);
      if (subsumees != NULL)
	c->subsumer = TRUE;
      while (subsumees != NULL) {
	Topform d = subsumees->v;
	/* Skip used clauses for consistency with forward subsumption, which
	   also skips used clauses.  This prevents a clause from escaping
	   forward subsumption (due to being marked used) but then being
	   caught by back subsumption. */
	if (flag(Opt->back_subsume_skip_used) && d->used) {
	  compact_otter_release_clause(d, NULL);
	  subsumees = plist_pop(subsumees);
	  continue;
	}
	/* Skip limbo clauses: they are in the literal index (for forward
	   subsumption and unit conflict) but have not yet been fully
	   processed.  This can happen when cl_process() adds a new clause
	   to limbo (e.g., via back_demod, back_unit_deletion, factoring,
	   or new_constant) and that clause is then found by
	   back_subsumption of a different limbo clause being processed
	   in the same limbo_process() loop.  The limbo clause will be
	   handled in its own iteration of the loop. */
	if (flag(Opt->back_subsume_skip_limbo) && clist_member(d, Glob.limbo)) {
	  compact_otter_release_clause(d, NULL);
	  subsumees = plist_pop(subsumees);
	  continue;
	}
	/* Ancestor-subsumption refinement (Otter anc_subsume): when c
	   and d are alphabetic variants, keep d if d's proof is strictly
	   shorter.  Always safe; merely retains more clauses. */
	if (flag(Opt->ancestor_subsume) &&
	    !anc_subsume(c, d, flag(Opt->proof_weight))) {
	  Stats.anc_subsume_blocked++;
	  if (flag(Opt->print_kept))
	    printf("%s    back subsumption of %llu by %llu blocked"
		   " by ancestor_subsume.\n",
		   TPTP_PFX, d->id, c->id);
	  compact_otter_release_clause(d, NULL);
	  subsumees = plist_pop(subsumees);
	  continue;
	}
	Stats.back_subsumed++;

	/* BV(2016-feb-02): when degraded hint matcher c subsumes
	   hint matcher d, track the minimum degradation counts. */
	if (c->matching_hint != NULL
	    && d->matching_hint != NULL
	    && Dcount_c > 0) {
	  int Dcount_d = degradation_count(d);
	  if (clist_member(d, Glob.sos) || dense_passive_contains_id(d->id)) {
	    if (Dcount_d < Dcount_min_sos)
	      Dcount_min_sos = Dcount_d;
	  }
	  else {
	    if (Dcount_d < Dcount_min_not_sos)
	      Dcount_min_not_sos = Dcount_d;
	  }
	}

	if (flag(Opt->print_kept)) {
	  if (d->matching_hint != NULL)
	    printf("%s    %llu back subsumes hint matcher %llu.\n",
		   TPTP_PFX, c->id, d->id);
	  else
	    printf("%s    %llu back subsumes %llu.\n", TPTP_PFX, c->id, d->id);
	}
	if (To_trace_cl == d) {
	  printf("\n*** Trace: clause %llu back subsumed by %llu.\n",
		 d->id, c->id);
	  To_trace_cl = NULL;
	}
	disable_clause(d);
	subsumees = plist_pop(subsumees);
      }

      /* BV(2016-feb-02): adjust degradation of c if a subsumed hint
	 matcher has a lower degradation count. */
      if (Dcount_min_sos < Dcount_c || Dcount_min_not_sos < Dcount_c) {
	c->weight = (int)(c->weight) % 1000;  /* original computed weight */
	if (Dcount_min_sos <= Dcount_min_not_sos) {
	  c->weight = c->weight + Dcount_min_sos * 1000;
	  if (flag(Opt->print_given))
	    printf("%s => %llu back subsumes a hint matcher on Sos."
		   "  Reset weight to %.3f.\n", TPTP_PFX, c->id, c->weight);
	}
	else {
	  c->weight = c->weight + Dcount_min_not_sos * 1000 + 500;
	  if (flag(Opt->print_given))
	    printf("%s => %llu back subsumes hint matchers not on Sos."
		   "  Reset weight to %.3f.\n", TPTP_PFX, c->id, c->weight);
	}
      }

      clock_stop(Clocks.back_subsume);
    }

    // If demodulator, rewrite other clauses (back demodulation).

    if (clist_member(c, Glob.demods)) {
      if (flag(Opt->print_kept))
	printf("%s    starting back demodulation with %llu.\n", TPTP_PFX, c->id);
      back_demod(c);  // This calls cl_process on rewritable clauses.
    }

    // If unit, use it to simplify other clauses (back unit_deletion)

    if (flag(Opt->unit_deletion) && unit_clause(c->literals)) {
      back_unit_deletion(c);  // This calls cl_process on rewritable clauses.
    }

    // Check if we should do back CAC simplification.

    if (ilist_member(Glob.cac_clauses, (int) c->id)) {
      back_cac_simplify();
    }

    // Remove from limbo

    clist_remove(c, Glob.limbo);

    // If restricted_denial, append to usable, else append to sos.

    if (restricted_denial(c) || discount_activation) {
      // do not index_clashable!  disable_clause should not unindex_clashable!
      clist_append(c, Glob.usable);
      index_back_demod(c, INSERT, Clocks.index, flag(Opt->back_demod));
    }
    else {
      // Move to Sos and index to be found for back demodulation.
      if (parm(Opt->sos_limit) != -1 &&
	  clist_length(Glob.sos) >= parm(Opt->sos_limit)) {
	sos_displace2(disable_clause, flag(Opt->quiet));
	Stats.sos_displaced++;
      }
      if (pre_search)
	c->initial = TRUE;
      else
	c->initial = FALSE;
      if (compact_otter_passive_mode()) {
        /* Every semantic transaction above has completed.  Populate the
           final pointer-free sidecar before surrendering the body, and drop
           the transient demod-list owner; the compact rewrite bank remains
           authoritative while this rule is cold. */
        index_back_demod(c, INSERT, Clocks.index, flag(Opt->back_demod));
        if (clist_member(c, Glob.demods))
          clist_remove(c, Glob.demods);
        if (To_trace_cl == c) {
          printf("\n*** Trace: clause %llu entered the compact passive archive.\n",
                 c->id);
          To_trace_cl = NULL;
        }
        insert_into_sos2(c, Glob.sos);  /* archives and deletes C */
        continue;
      }
      else {
        insert_into_sos2(c, Glob.sos);
        index_back_demod(c, INSERT, Clocks.index, flag(Opt->back_demod));
      }
    }

    // Report if this single iteration took a long time
    if (lp_next > 0) {
      double iter_elapsed = user_seconds() - iter_start;
      if (iter_elapsed >= parm(Opt->report_preprocessing)) {
	fprintf(stderr,
		"NOTE: preprocessing limbo clause %llu took %.1f sec"
		" (remaining %d, %.1f sec, %lld MB)\n",
		c->id, iter_elapsed, clist_length(Glob.limbo),
		user_seconds(), megs_malloced());
	fflush(stderr);
	lp_next = user_seconds() + parm(Opt->report_preprocessing);
      }
    }
  }
  // Now it is safe to disable descendants of desc_to_be_disabled clauses.
  disable_to_be_disabled();

  if (pre_search && lp_count > 0) {
#ifdef DEBUG
    fprintf(stderr, "%% limbo_process done: %d clauses (%.2f sec, %lld MB)\n",
	    lp_count, user_seconds(), megs_malloced());
    fflush(stderr);
#endif
  }
}  // limbo_process

/*************
 *
 *   infer_outside_loop()
 *
 *************/

static
void infer_outside_loop(Topform c)
{
  /* If simplification changes the clause, we want to do a "copy"
   inference first, so that a proof does not contain a justification
   like  [assumption,rewrite[...],...]. */
  Topform copy = copy_inference(c);  /* Note: c has no ID yet. */
  cl_process_simplify(copy);
  if (copy->justification->next == NULL) {
    /* Simplification does nothing, so we can just process the original. */
    delete_clause(copy);
    cl_process(c);
  }
  else {
    if (c->id == 0)   /* see the guard note in the Usable loop */
      assign_clause_id(c);
    copy->justification->u.id = c->id;
    retain_disabled_clause(c);
    cl_process(copy);  /* This re-simplifies, but that's ok. */
  }

  limbo_process(FALSE);
}  /* infer_outside_loop */

struct collective_history_filter {
  unsigned snapshot_epoch;
  unsigned long long accepted;
  unsigned long long rejected_future;
  unsigned long long rejected_inactive;
};

struct collective_hyper_source_data {
  struct collective_history_filter *filter;
  unsigned long long activation_limit;
};

static unsigned long long collective_hyper_source_count(void *data)
{
  struct collective_hyper_source_data *source = data;
  return source->activation_limit;
}

static Topform collective_hyper_source_clause(
  unsigned long long position, void *data)
{
  struct collective_hyper_source_data *source = data;
  if (position >= source->activation_limit)
    fatal_error("collective hyper iterator parent cursor out of bounds");
  return collective_activation_clause(position);
}

struct collective_candidate_chunk {
  unsigned long long skip;
  unsigned long long limit;
  unsigned long long seen;
  unsigned long long emitted;
  unsigned long long eligible;
  unsigned long long replayed;
  unsigned long long rolling_hash;
  unsigned long long expected_prefix_hash;
  unsigned long long replay_prefix_hash;
  unsigned long long committed_prefix_hash;
  BOOL replay_prefix_captured;
  BOOL committed_prefix_captured;
  BOOL promising;
  BOOL threshold_present;
  double threshold_weight;
  unsigned long long threshold_ordinal;
  struct collective_promising_candidate *buffer;
  unsigned long long buffer_count;
  BOOL next_key_present;
  double next_weight;
  unsigned long long next_ordinal;
};

struct collective_promising_candidate {
  Topform clause;
  double weight;
  unsigned long long ordinal;
};

#define COLLECTIVE_REPLAY_HASH_SEED 1469598103934665603ULL

/* Extend a sequence hash with the structural fingerprint of one raw
   conclusion.  This is deliberately computed before cl_process() can assign
   an ID, simplify, or delete the clause. */
static
unsigned long long collective_replay_hash_clause(
  unsigned long long h, Topform c)
{
  unsigned long long clause_hash = hint_trace_clause_hash(c);
  h = hint_trace_mix_u32(h, (unsigned) clause_hash);
  h = hint_trace_mix_u32(h, (unsigned) (clause_hash >> 32));
  return hint_trace_mix_u32(h, 0xc011ec7eU);
}

static int collective_candidate_key_compare(
  double weight_a, unsigned long long ordinal_a,
  double weight_b, unsigned long long ordinal_b)
{
  if (weight_a < weight_b)
    return -1;
  else if (weight_a > weight_b)
    return 1;
  else if (ordinal_a < ordinal_b)
    return -1;
  else if (ordinal_a > ordinal_b)
    return 1;
  else
    return 0;
}

static void collective_promising_note_next(
  struct collective_candidate_chunk *chunk,
  double weight, unsigned long long ordinal)
{
  if (!chunk->next_key_present ||
      collective_candidate_key_compare(weight, ordinal,
                                       chunk->next_weight,
                                       chunk->next_ordinal) < 0) {
    chunk->next_key_present = TRUE;
    chunk->next_weight = weight;
    chunk->next_ordinal = ordinal;
  }
}

/* Retain the smallest `limit` raw keys after the committed threshold.  The
   buffer is fixed-size; a displaced or uncompetitive raw conclusion is
   deleted before any clause ID or hint state can observe it. */
static void collective_promising_insert(
  struct collective_candidate_chunk *chunk, Topform c,
  double weight, unsigned long long ordinal)
{
  unsigned long long at = 0;
  struct collective_promising_candidate candidate;

  while (at < chunk->buffer_count &&
         collective_candidate_key_compare(
           chunk->buffer[at].weight, chunk->buffer[at].ordinal,
           weight, ordinal) < 0)
    at++;

  candidate.clause = c;
  candidate.weight = weight;
  candidate.ordinal = ordinal;

  if (chunk->buffer_count < chunk->limit) {
    if (at < chunk->buffer_count)
      memmove(chunk->buffer + at + 1, chunk->buffer + at,
              (size_t) (chunk->buffer_count - at) *
                sizeof(*chunk->buffer));
    chunk->buffer[at] = candidate;
    chunk->buffer_count++;
  }
  else if (at >= chunk->limit) {
    collective_promising_note_next(chunk, weight, ordinal);
    delete_clause(c);
  }
  else {
    struct collective_promising_candidate displaced =
      chunk->buffer[chunk->limit - 1];
    collective_promising_note_next(
      chunk, displaced.weight, displaced.ordinal);
    delete_clause(displaced.clause);
    if (at + 1 < chunk->limit)
      memmove(chunk->buffer + at + 1, chunk->buffer + at,
              (size_t) (chunk->limit - at - 1) *
                sizeof(*chunk->buffer));
    chunk->buffer[at] = candidate;
  }
}

/* A collective hyper set is replayable from immutable historical parents.
   Discard the already-committed prefix before it reaches clause IDs, hints,
   or passive storage, commit at most one bounded chunk, and discard the
   remainder so the descriptor can revisit it on a later fair turn. */
static void collective_chunk_process(Topform c, void *data)
{
  struct collective_candidate_chunk *chunk = data;
  unsigned long long ordinal = chunk->seen++;

  if (!chunk->promising && ordinal == chunk->skip) {
    chunk->replay_prefix_hash = chunk->rolling_hash;
    chunk->replay_prefix_captured = TRUE;
    if (chunk->replay_prefix_hash != chunk->expected_prefix_hash)
      fatal_error("collective conclusion replay prefix changed");
  }
  chunk->rolling_hash =
    collective_replay_hash_clause(chunk->rolling_hash, c);

  if (chunk->promising) {
    double weight = clause_weight(c->literals);
    if (!isfinite(weight))
      fatal_error("collective promising candidate has non-finite weight");
    if (chunk->threshold_present &&
        collective_candidate_key_compare(
          weight, ordinal, chunk->threshold_weight,
          chunk->threshold_ordinal) <= 0) {
      chunk->replayed++;
      delete_clause(c);
    }
    else {
      chunk->eligible++;
      collective_promising_insert(chunk, c, weight, ordinal);
    }
    return;
  }

  if (ordinal < chunk->skip)
    delete_clause(c);
  else if (chunk->emitted < chunk->limit) {
    chunk->emitted++;
    if (chunk->emitted == chunk->limit) {
      chunk->committed_prefix_hash = chunk->rolling_hash;
      chunk->committed_prefix_captured = TRUE;
    }
    cl_process(c);
  }
  else
    delete_clause(c);
}

static void collective_reset_conclusion_state(struct collective_batch *b)
{
  b->kind &= ~COLLECTIVE_PROMISING_CURSOR;
  b->conclusion_cursor = 0;
  b->conclusion_prefix_hash = 0;
  b->conclusion_order_weight = 0;
  b->conclusion_order_ordinal = 0;
  b->conclusion_next_weight = 0;
  b->conclusion_next_ordinal = 0;
}

static void collective_chunk_init(
  struct collective_candidate_chunk *chunk, struct collective_batch *b)
{
  memset(chunk, 0, sizeof(*chunk));
  chunk->skip = b->conclusion_cursor;
  chunk->limit = collective_candidate_budget();
  if (chunk->limit == 0)
    fatal_error("collective expansion has no candidate-cache space");
  chunk->rolling_hash = COLLECTIVE_REPLAY_HASH_SEED;
  chunk->promising =
    (b->kind & COLLECTIVE_PROMISING_CURSOR) != 0 ||
    (b->conclusion_cursor == 0 &&
     flag(Opt->collective_promising_candidates));

  if (chunk->promising) {
    if (b->conclusion_cursor != 0) {
      chunk->threshold_present = TRUE;
      chunk->threshold_weight = b->conclusion_order_weight;
      chunk->threshold_ordinal = b->conclusion_order_ordinal;
      chunk->expected_prefix_hash = b->conclusion_prefix_hash;
    }
    chunk->buffer = safe_calloc((size_t) chunk->limit,
                                sizeof(*chunk->buffer));
  }
  else
    chunk->expected_prefix_hash = b->conclusion_cursor == 0 ?
      COLLECTIVE_REPLAY_HASH_SEED : b->conclusion_prefix_hash;
}

/* Finish one complete raw enumeration.  Prefix mode commits generator-order
   chunks.  Promising mode instead commits the smallest raw-weight/ordinal
   keys after its saved threshold and verifies the checksum of the entire
   immutable sequence on every rescan. */
static BOOL collective_chunk_finish(
  struct collective_candidate_chunk *chunk, struct collective_batch *b,
  char *inference_name)
{
  BOOL complete;

  if (!chunk->promising) {
    if (chunk->seen < chunk->skip)
      fatal_error(inference_name);
    if (!chunk->replay_prefix_captured) {
      chunk->replay_prefix_hash = chunk->rolling_hash;
      chunk->replay_prefix_captured = TRUE;
    }
    if (chunk->replay_prefix_hash != chunk->expected_prefix_hash)
      fatal_error(inference_name);
    if (!chunk->committed_prefix_captured) {
      chunk->committed_prefix_hash = chunk->rolling_hash;
      chunk->committed_prefix_captured = TRUE;
    }
    chunk->replayed = chunk->skip;
    complete = chunk->seen <= chunk->skip + chunk->emitted;
    if (!complete) {
      b->kind &= ~COLLECTIVE_PROMISING_CURSOR;
      b->conclusion_cursor = chunk->skip + chunk->emitted;
      b->conclusion_prefix_hash = chunk->committed_prefix_hash;
      b->conclusion_order_weight = 0;
      b->conclusion_order_ordinal = 0;
      b->conclusion_next_weight = 0;
      b->conclusion_next_ordinal = 0;
    }
    else
      collective_reset_conclusion_state(b);
  }
  else {
    unsigned long long i;
    double last_weight = 0;
    unsigned long long last_ordinal = 0;

    if (chunk->threshold_present &&
        chunk->rolling_hash != chunk->expected_prefix_hash)
      fatal_error(inference_name);
    if (chunk->replayed != b->conclusion_cursor)
      fatal_error("collective promising replay rank changed");

    chunk->emitted = chunk->buffer_count;
    if (chunk->emitted > 0) {
      last_weight = chunk->buffer[chunk->emitted - 1].weight;
      last_ordinal = chunk->buffer[chunk->emitted - 1].ordinal;
    }
    for (i = 0; i < chunk->emitted; i++)
      cl_process(chunk->buffer[i].clause);

    complete = chunk->eligible == chunk->emitted;
    Stats.collective_promising_scans++;
    Stats.collective_promising_considered += chunk->eligible;
    if (chunk->emitted > Stats.collective_promising_buffer_peak)
      Stats.collective_promising_buffer_peak = chunk->emitted;
    if (!complete) {
      if (chunk->emitted == 0 || !chunk->next_key_present)
        fatal_error("collective promising scan lost its next key");
      b->kind |= COLLECTIVE_PROMISING_CURSOR;
      b->conclusion_cursor += chunk->emitted;
      b->conclusion_prefix_hash = chunk->rolling_hash;
      b->conclusion_order_weight = last_weight;
      b->conclusion_order_ordinal = last_ordinal;
      b->conclusion_next_weight = chunk->next_weight;
      b->conclusion_next_ordinal = chunk->next_ordinal;
      if (collective_candidate_key_compare(
            b->conclusion_order_weight, b->conclusion_order_ordinal,
            b->conclusion_next_weight, b->conclusion_next_ordinal) >= 0)
        fatal_error("collective promising next key is not increasing");
    }
    else
      collective_reset_conclusion_state(b);
    safe_free(chunk->buffer);
    chunk->buffer = NULL;
  }

  if (chunk->seen > Stats.collective_raw_candidates_peak)
    Stats.collective_raw_candidates_peak = chunk->seen;
  Stats.collective_raw_candidates_seen += chunk->seen;
  Stats.collective_candidates_emitted += chunk->emitted;
  Stats.collective_candidates_replayed += chunk->replayed;
  if (!complete)
    Stats.collective_deferred_turns++;
  return complete;
}

static struct collective_candidate_chunk *Current_collective_chunk = NULL;

static
void collective_chunk_cl_process(Topform c)
{
  if (Current_collective_chunk == NULL)
    fatal_error("collective chunk callback has no active context");
  collective_chunk_process(c, Current_collective_chunk);
}

/* The persistent index contains every historically clashable activation.
   Filter each retrieved candidate against the descriptor epoch: a clone's
   simplifier_epoch is its activation epoch, and the side table records the
   first epoch at which it ceased to be active. */
static
BOOL collective_history_clause_test(Topform c, void *data)
{
  struct collective_history_filter *filter = data;
  unsigned deactivated;
  if (c->simplifier_epoch > filter->snapshot_epoch) {
    filter->rejected_future++;
    return FALSE;
  }
  deactivated = collective_deactivation_epoch(c->id);
  if (deactivated != 0 && deactivated <= filter->snapshot_epoch) {
    filter->rejected_inactive++;
    return FALSE;
  }
  filter->accepted++;
  return TRUE;
}

static BOOL collective_hyper_source_test(Topform c, void *data)
{
  struct collective_hyper_source_data *source = data;
  if (restricted_denial(c))
    return FALSE;
  return collective_history_clause_test(c, source->filter);
}

static BOOL collective_discovery_eligible(struct collective_batch *b)
{
  unsigned long long distance;
  if (!collective_balanced_mode() ||
      !flag(Opt->collective_hint_discovery) ||
      clist_empty(Glob.hints) || b->discovery_complete)
    return FALSE;
  if (b->consumed_count >=
        (unsigned) parm(Opt->collective_discovery_promotion_cap))
    return FALSE;
  if (!b->discovery_initialized ||
      b->candidate_ordinal > b->discovery_ordinal)
    return TRUE;
  distance = b->discovery_ordinal - b->candidate_ordinal;
  return distance <
    (unsigned long long) parm(Opt->collective_discovery_distance);
}

static struct collective_batch *collective_choose_discovery_batch(
  BOOL *general_turn)
{
  struct collective_batch *b, *first = NULL, *ordinary = NULL, *hot = NULL;
  unsigned interval =
    (unsigned) parm(Opt->collective_discovery_general_interval);
  BOOL force_general = interval == 1 ||
    Collective_discovery_turns_since_general >= interval - 1;

  for (b = Collective_batch_head; b != NULL; b = b->next) {
    if (!collective_discovery_eligible(b))
      continue;
    if (first == NULL)
      first = b;
    if (b->discovery_hot && hot == NULL)
      hot = b;
    if (!b->discovery_hot && ordinary == NULL)
      ordinary = b;
  }
  if (first == NULL)
    return NULL;
  if (force_general) {
    *general_turn = TRUE;
    return ordinary == NULL ? first : ordinary;
  }
  if (hot != NULL) {
    *general_turn = FALSE;
    return hot;
  }
  *general_turn = TRUE;
  return ordinary == NULL ? first : ordinary;
}

static BOOL collective_expand_discovery_one(void)
{
  struct collective_batch *b;
  BOOL general_turn = FALSE;
  unsigned turn_interval =
    (unsigned) parm(Opt->collective_discovery_turn_interval);
  unsigned long long raw_budget =
    (unsigned long long) parm(Opt->collective_discovery_raw_budget);
  unsigned long long distance_limit =
    (unsigned long long) parm(Opt->collective_discovery_distance);
  unsigned long long distance, candidate_budget;

  if (!flag(Opt->collective_hint_discovery) || clist_empty(Glob.hints))
    return FALSE;
  if (turn_interval == 1 ||
      Collective_discovery_turns_since_fair >= turn_interval - 1) {
    Stats.collective_discovery_forced_fair_turns++;
    return FALSE;
  }
  b = collective_choose_discovery_batch(&general_turn);
  if (b == NULL)
    return FALSE;
  collective_discovery_sync(b);
  distance = b->discovery_ordinal >= b->candidate_ordinal ?
    b->discovery_ordinal - b->candidate_ordinal : 0;
  if (distance >= distance_limit)
    return FALSE;
  candidate_budget = distance_limit - distance;
  if (candidate_budget > raw_budget)
    candidate_budget = raw_budget;

  Current_collective_pool_batch = b;
  Current_collective_discovery = TRUE;
  if (b->kind == COLLECTIVE_POS_HYPER ||
      b->kind == COLLECTIVE_NEG_HYPER) {
    struct collective_history_filter filter;
    struct collective_hyper_source_data source_data;
    Hyper_parent_source source;
    unsigned long long raw = 0, yielded = 0;
    Topform given = collective_activation_clause(b->activation_limit - 1);
    int direction = b->kind == COLLECTIVE_POS_HYPER ? POS_RES : NEG_RES;
    BOOL complete;
    memset(&filter, 0, sizeof(filter));
    filter.snapshot_epoch = b->snapshot_epoch;
    source_data.filter = &filter;
    source_data.activation_limit = b->activation_limit;
    source.count = collective_hyper_source_count;
    source.clause = collective_hyper_source_clause;
    source.test = collective_hyper_source_test;
    source.data = &source_data;
    clock_start(Clocks.infer);
    Current_inference_source = INFER_SOURCE_HYPER;
    complete = hyper_resolution_bounded(
      given, direction, &source, &b->discovery_hyper_iterator,
      raw_budget, candidate_budget, collective_discovery_candidate,
      &raw, &yielded);
    Current_inference_source = INFER_SOURCE_OTHER;
    clock_stop(Clocks.infer);
    Stats.collective_discovery_raw_steps += raw;
    if (complete) {
      hyper_iterator_reset(&b->discovery_hyper_iterator);
      b->discovery_complete = TRUE;
    }
  }
  else {
    unsigned long long raw = 0, yielded = 0;
    BOOL complete = FALSE;
    if (b->discovery_cursor >= b->activation_limit)
      b->discovery_complete = TRUE;
    else {
      unsigned long long position = b->discovery_cursor;
      unsigned long long partner_id = collective_activation_id(position);
      unsigned deactivated = collective_deactivation_epoch(partner_id);
      Topform given = collective_activation_clause(b->activation_limit - 1);
      Topform partner = collective_activation_clause(position);
      BOOL good_given =
        b->given_id < (unsigned long long) parm(Opt->para_restr_beg) ||
        b->given_id > (unsigned long long) parm(Opt->para_restr_end);
      BOOL good_pair = good_given ||
        partner_id < (unsigned long long) parm(Opt->para_restr_beg) ||
        partner_id > (unsigned long long) parm(Opt->para_restr_end);
      if (deactivated != 0 && deactivated <= b->snapshot_epoch) {
        para_iterator_reset(&b->discovery_para_iterator);
        b->discovery_cursor++;
        raw = 1;
      }
      else if (restricted_denial(partner) || !good_pair ||
               over_parm_limit(number_of_literals(partner->literals),
                               Opt->para_lit_limit)) {
        para_iterator_reset(&b->discovery_para_iterator);
        b->discovery_cursor++;
        raw = 1;
      }
      else {
        Topform from = b->kind == COLLECTIVE_PARAMOD_FROM ? given : partner;
        Topform into = b->kind == COLLECTIVE_PARAMOD_FROM ? partner : given;
        BOOL check_top = b->kind == COLLECTIVE_PARAMOD_INTO;
        clock_start(Clocks.infer);
        Current_inference_source = INFER_SOURCE_PARAMOD;
        complete = para_from_into_bounded(
          from, into, check_top, &b->discovery_para_iterator,
          raw_budget, candidate_budget, collective_discovery_candidate,
          &raw, &yielded);
        Current_inference_source = INFER_SOURCE_OTHER;
        clock_stop(Clocks.infer);
        if (complete) {
          para_iterator_reset(&b->discovery_para_iterator);
          b->discovery_cursor++;
        }
      }
      if (b->discovery_cursor >= b->activation_limit)
        b->discovery_complete = TRUE;
    }
    Stats.collective_discovery_raw_steps += raw;
  }
  Current_collective_discovery = FALSE;
  Current_collective_pool_batch = NULL;
  Stats.collective_discovery_turns++;
  if (general_turn) {
    Stats.collective_discovery_general_turns++;
    Collective_discovery_turns_since_general = 0;
  }
  else {
    Stats.collective_discovery_hot_turns++;
    Collective_discovery_turns_since_general++;
  }
  Collective_discovery_turns_since_fair++;
  return TRUE;
}

/* Expand at most one historical active partner from the oldest round-robin
   batch.  Both paramodulation directions for that pair are performed before
   yielding.  Conclusions still go through cl_process(), including exact
   normalization, hint matching, weighting, and proof construction. */
static
BOOL collective_expand_one_batch(void)
{
  struct collective_batch *b;
  BOOL hint_probe;

  if (Collective_batch_head == NULL)
    return FALSE;

  if (collective_balanced_mode())
    collective_choose_balanced_batch_for_turn();
  else
    collective_choose_batch_for_turn();
  b = Collective_batch_head;
  hint_probe = (b->kind & COLLECTIVE_HINT_PROBE) != 0;

  if ((b->kind & COLLECTIVE_INFERENCE_MASK) == 0 ||
      (b->kind & ~COLLECTIVE_KIND_MASK) != 0)
    fatal_error("unknown collective batch kind");
  if (collective_balanced_mode() &&
      b->kind != COLLECTIVE_PARAMOD_FROM &&
      b->kind != COLLECTIVE_PARAMOD_INTO &&
      b->kind != COLLECTIVE_POS_HYPER &&
      b->kind != COLLECTIVE_NEG_HYPER)
    fatal_error("balanced collective descriptor is not split by rule");

  /* A hint probe advances one paramodulation pair before any hyper work.
     Hyper now commits only a bounded conclusion chunk, but regenerating and
     checking the raw set can still be arbitrarily expensive.  It therefore
     remains on the ordinary fair queue and cannot masquerade as a cheap
     probe. */
  if ((b->kind & (COLLECTIVE_POS_HYPER | COLLECTIVE_NEG_HYPER)) != 0 &&
      (!hint_probe ||
       (b->kind & COLLECTIVE_PARAMOD) == 0 ||
       b->cursor >= b->activation_limit)) {
    struct collective_history_filter filter;
    unsigned long long given_position;
    unsigned long long generated_before = Stats.generated;
    unsigned long long kept_before = Stats.kept;
    unsigned long long raw = 0, emitted = 0;
    BOOL complete, native = FALSE;
    Topform given;
    unsigned hyper_kind = (b->kind & COLLECTIVE_POS_HYPER) != 0 ?
                          COLLECTIVE_POS_HYPER : COLLECTIVE_NEG_HYPER;
    int direction = hyper_kind == COLLECTIVE_POS_HYPER ? POS_RES : NEG_RES;
    if (b->activation_limit == 0)
      fatal_error("collective hyper batch has no activation history");
    given_position = b->activation_limit - 1;
    given = collective_activation_clause(given_position);
    if (given->id != b->given_id)
      fatal_error("collective hyper batch is not anchored by its given");
    memset(&filter, 0, sizeof(filter));
    filter.snapshot_epoch = b->snapshot_epoch;
    if (collective_balanced_mode() && b->conclusion_cursor == 0) {
      struct collective_hyper_source_data source_data;
      Hyper_parent_source source;
      unsigned long long candidate_budget = collective_candidate_budget();
      if (candidate_budget == 0)
        fatal_error("bounded hyperresolution has no candidate-pool space");
      source_data.filter = &filter;
      source_data.activation_limit = b->activation_limit;
      source.count = collective_hyper_source_count;
      source.clause = collective_hyper_source_clause;
      source.test = collective_hyper_source_test;
      source.data = &source_data;
      clock_start(Clocks.infer);
      Current_inference_source = INFER_SOURCE_HYPER;
      Current_collective_pool_batch = b;
      complete = hyper_resolution_bounded(
        given, direction, &source, &b->hyper_iterator,
        (unsigned long long) parm(Opt->collective_raw_work_budget),
        candidate_budget, collective_candidate_pool_push, &raw, &emitted);
      Current_collective_pool_batch = NULL;
      Current_inference_source = INFER_SOURCE_OTHER;
      clock_stop(Clocks.infer);
      native = TRUE;
      Stats.collective_iterator_raw_steps += raw;
      Stats.collective_iterator_candidates += emitted;
      Stats.collective_hyper_iterator_raw_steps += raw;
      Stats.collective_hyper_iterator_candidates += emitted;
      Stats.collective_candidates_emitted += emitted;
      Stats.collective_raw_candidates_seen += emitted;
      if (raw > Stats.collective_iterator_raw_peak)
        Stats.collective_iterator_raw_peak = raw;
      if (emitted > Stats.collective_raw_candidates_peak)
        Stats.collective_raw_candidates_peak = emitted;
      if (!complete)
        Stats.collective_deferred_turns++;
      else {
        Stats.collective_iterator_completions++;
        Stats.collective_hyper_iterator_completions++;
        hyper_iterator_reset(&b->hyper_iterator);
      }
    }
    else {
      struct collective_candidate_chunk chunk;
      collective_chunk_init(&chunk, b);
      clock_start(Clocks.infer);
      Current_inference_source = INFER_SOURCE_HYPER;
      Current_collective_chunk = &chunk;
      hyper_resolution_with_clause_test(
        given, direction, Collective_historical_idx,
        collective_history_clause_test, &filter, collective_chunk_cl_process);
      complete = collective_chunk_finish(
        &chunk, b, "collective hyper replay order changed");
      Current_collective_chunk = NULL;
      Current_inference_source = INFER_SOURCE_OTHER;
      clock_stop(Clocks.infer);
      raw = chunk.seen;
      emitted = chunk.emitted;
    }
    if (complete) {
      b->kind &= ~hyper_kind;
      Stats.collective_hyper_sets_completed++;
      if (hyper_kind == COLLECTIVE_POS_HYPER)
        Stats.collective_completed_pos_hyper++;
      else
        Stats.collective_completed_neg_hyper++;
    }
    Stats.collective_hyper_expansions++;
    Stats.collective_history_queries++;
    Stats.collective_history_candidates += filter.accepted;
    Stats.collective_history_rejected_future += filter.rejected_future;
    Stats.collective_history_rejected_inactive += filter.rejected_inactive;
    if (flag(Opt->collective_trace)) {
      printf("%sCOLLECTIVE_TRACE kind=%s given=%llu epoch=%u "
             "history_candidates=%llu future_rejected=%llu "
             "inactive_rejected=%llu generated=%llu kept=%llu "
             "hint_probe=%d raw=%llu emitted=%llu cursor=%llu "
             "complete=%d",
             TPTP_PFX,
             hyper_kind == COLLECTIVE_POS_HYPER ? "pos_hyper" : "neg_hyper",
             b->given_id, b->snapshot_epoch, filter.accepted,
             filter.rejected_future, filter.rejected_inactive,
             Stats.generated - generated_before, Stats.kept - kept_before,
             hint_probe, raw, emitted, b->conclusion_cursor,
             (b->kind & hyper_kind) == 0);
      if (native)
        printf(" native=1");
      printf(".\n");
    }
    collective_finish_batch_turn(b);
    return TRUE;
  }

  if ((b->kind & (COLLECTIVE_PARAMOD | COLLECTIVE_PARAMOD_FROM |
                  COLLECTIVE_PARAMOD_INTO)) == 0)
    fatal_error("unknown collective batch kind");

  while (b->cursor < b->activation_limit) {
    unsigned long long partner_position = b->cursor;
    unsigned long long partner_id = collective_activation_id(partner_position);
    unsigned deactivated = collective_deactivation_epoch(partner_id);
    Topform given, partner;
    BOOL good_given, good_pair;

    if (deactivated != 0 && deactivated <= b->snapshot_epoch) {
      if (b->conclusion_cursor != 0 ||
          !para_iterator_at_start(&b->para_iterator))
        fatal_error("partial collective pair became inactive in its snapshot");
      b->cursor++;
      Stats.collective_partners_skipped++;
      continue;
    }

    given = collective_activation_clause(b->activation_limit - 1);
    partner = collective_activation_clause(partner_position);
    if (given->id != b->given_id || partner->id != partner_id)
      fatal_error("collective paramodulation history ID mismatch");

    good_given = (b->given_id <
                    (unsigned long long) parm(Opt->para_restr_beg) ||
                  b->given_id >
                    (unsigned long long) parm(Opt->para_restr_end));
    good_pair = (good_given ||
                 partner_id <
                   (unsigned long long) parm(Opt->para_restr_beg) ||
                 partner_id >
                   (unsigned long long) parm(Opt->para_restr_end));

    if (!restricted_denial(partner) && good_pair &&
        !over_parm_limit(number_of_literals(partner->literals),
                         Opt->para_lit_limit)) {
      unsigned long long generated_before = Stats.generated;
      unsigned long long kept_before = Stats.kept;
      BOOL complete;
      if (collective_balanced_mode() && b->conclusion_cursor == 0) {
        Topform from = b->kind == COLLECTIVE_PARAMOD_FROM ? given : partner;
        Topform into = b->kind == COLLECTIVE_PARAMOD_FROM ? partner : given;
        BOOL check_top = b->kind == COLLECTIVE_PARAMOD_INTO;
        unsigned long long raw_steps, yielded;
        unsigned long long candidate_budget = collective_candidate_budget();
        if (candidate_budget == 0)
          fatal_error("bounded paramodulation has no candidate-pool space");
        clock_start(Clocks.infer);
        Current_inference_source = INFER_SOURCE_PARAMOD;
        Current_collective_pool_batch = b;
        complete = para_from_into_bounded(
          from, into, check_top, &b->para_iterator,
          (unsigned long long) parm(Opt->collective_raw_work_budget),
          candidate_budget, collective_candidate_pool_push,
          &raw_steps, &yielded);
        Current_collective_pool_batch = NULL;
        Current_inference_source = INFER_SOURCE_OTHER;
        clock_stop(Clocks.infer);
        Stats.collective_iterator_raw_steps += raw_steps;
        Stats.collective_iterator_candidates += yielded;
        Stats.collective_candidates_emitted += yielded;
        Stats.collective_raw_candidates_seen += yielded;
        if (raw_steps > Stats.collective_iterator_raw_peak)
          Stats.collective_iterator_raw_peak = raw_steps;
        if (yielded > Stats.collective_raw_candidates_peak)
          Stats.collective_raw_candidates_peak = yielded;
        if (!complete) {
          Stats.collective_deferred_turns++;
        }
        else {
          Stats.collective_iterator_completions++;
          para_iterator_reset(&b->para_iterator);
          b->cursor++;
          Stats.collective_pair_expansions++;
        }
        if (b->kind == COLLECTIVE_PARAMOD_FROM)
          Stats.collective_paramod_from_turns++;
        else
          Stats.collective_paramod_into_turns++;
        Stats.collective_pair_turns++;
        if (flag(Opt->collective_trace))
          printf("%sCOLLECTIVE_TRACE kind=%s given=%llu partner=%llu "
                 "epoch=%u generated=%llu kept=%llu historical=1 "
                 "hint_probe=0 raw=%llu emitted=%llu cursor=0 "
                 "complete=%d native=1.\n",
                 TPTP_PFX,
                 b->kind == COLLECTIVE_PARAMOD_FROM ?
                   "paramod_from" : "paramod_into",
                 b->given_id, partner_id, b->snapshot_epoch,
                 Stats.generated - generated_before,
                 Stats.kept - kept_before, raw_steps, yielded, complete);
      }
      else {
        Context cf = get_context();
        Context ci = get_context();
        struct collective_candidate_chunk chunk;
        collective_chunk_init(&chunk, b);
        clock_start(Clocks.infer);
        Current_inference_source = INFER_SOURCE_PARAMOD;
        Current_collective_chunk = &chunk;
        if ((b->kind &
             (COLLECTIVE_PARAMOD | COLLECTIVE_PARAMOD_FROM)) != 0) {
          para_from_into(given, cf, partner, ci, FALSE,
                         collective_chunk_cl_process);
          Stats.collective_paramod_from_turns++;
        }
        if ((b->kind &
             (COLLECTIVE_PARAMOD | COLLECTIVE_PARAMOD_INTO)) != 0) {
          para_from_into(partner, cf, given, ci, TRUE,
                         collective_chunk_cl_process);
          Stats.collective_paramod_into_turns++;
        }
        complete = collective_chunk_finish(
          &chunk, b, "collective paramodulation replay order changed");
        Current_collective_chunk = NULL;
        Current_inference_source = INFER_SOURCE_OTHER;
        clock_stop(Clocks.infer);
        free_context(cf);
        free_context(ci);
        if (complete) {
          b->cursor++;
          Stats.collective_pair_expansions++;
        }
        Stats.collective_pair_turns++;
        if (flag(Opt->collective_trace))
          printf("%sCOLLECTIVE_TRACE kind=%s given=%llu partner=%llu "
                 "epoch=%u generated=%llu kept=%llu historical=1 "
                 "hint_probe=%d raw=%llu emitted=%llu cursor=%llu "
                 "complete=%d.\n",
                 TPTP_PFX,
                 b->kind == COLLECTIVE_PARAMOD_FROM ? "paramod_from" :
                 b->kind == COLLECTIVE_PARAMOD_INTO ? "paramod_into" :
                                                      "paramod",
                 b->given_id, partner_id, b->snapshot_epoch,
                 Stats.generated - generated_before,
                 Stats.kept - kept_before, hint_probe, chunk.seen,
                 chunk.emitted, b->conclusion_cursor, complete);
      }
    }
    else {
      if (b->conclusion_cursor != 0 ||
          !para_iterator_at_start(&b->para_iterator))
        fatal_error("partial collective pair became ineligible");
      b->cursor++;
      Stats.collective_partners_skipped++;
    }

    collective_finish_batch_turn(b);
    return TRUE;
  }

  collective_finish_batch_turn(b);
  return TRUE;
}

/*************
 *
 *   given_infer()
 *
 *   Make given_clause inferences according to the flags that are set.
 *   Inferred clauses are sent to cl_process().
 *
 *   We could process the Limbo list after each inference rule,
 *   and this might improve performance in some cases.  But it seems
 *   conceptually simplier if we process the Limbo clauses after
 *   all of the inferences have been made.
 *
 *************/

static
void given_infer(Topform given)
{
  struct collective_batch *tail_before =
    collective_frontier_mode() ? Collective_batch_tail : NULL;

  clock_start(Clocks.infer);

  if (flag(Opt->binary_resolution)) {
    Current_inference_source = INFER_SOURCE_BINARY;
    binary_resolution(given,
		      ANY_RES,
		      Glob.clashable_idx,
		      cl_process);
  }

  if (flag(Opt->neg_binary_resolution)) {
    Current_inference_source = INFER_SOURCE_BINARY;
    binary_resolution(given,
		      NEG_RES,
		      Glob.clashable_idx,
		      cl_process);
  }

  if (flag(Opt->pos_hyper_resolution)) {
    if (collective_frontier_mode())
      collective_enqueue_hyper_batch(given, COLLECTIVE_POS_HYPER);
    else {
      Current_inference_source = INFER_SOURCE_HYPER;
      hyper_resolution(given, POS_RES, Glob.clashable_idx, cl_process);
    }
  }

  if (flag(Opt->neg_hyper_resolution)) {
    if (collective_frontier_mode())
      collective_enqueue_hyper_batch(given, COLLECTIVE_NEG_HYPER);
    else {
      Current_inference_source = INFER_SOURCE_HYPER;
      hyper_resolution(given, NEG_RES, Glob.clashable_idx, cl_process);
    }
  }

  if (flag(Opt->pos_ur_resolution)) {
    Current_inference_source = INFER_SOURCE_UR;
    ur_resolution(given, POS_RES, Glob.clashable_idx, cl_process);
  }

  if (flag(Opt->neg_ur_resolution)) {
    Current_inference_source = INFER_SOURCE_UR;
    ur_resolution(given, NEG_RES, Glob.clashable_idx, cl_process);
  }

  if (flag(Opt->paramodulation) &&
      !over_parm_limit(number_of_literals(given->literals),
		       Opt->para_lit_limit)) {
    if (collective_frontier_mode())
      collective_enqueue_batch(given);
    else {
      /* This paramodulation does not use indexing. */
      Context cf = get_context();
      Context ci = get_context();
      Clist_pos p;
      BOOL good_given =
	(given->id < (unsigned long long) parm(Opt->para_restr_beg) ||
	 given->id > (unsigned long long) parm(Opt->para_restr_end));
      for (p = Glob.usable->first; p; p = p->next) {
	if (!restricted_denial(p->c) &&
	    !over_parm_limit(number_of_literals(p->c->literals),
			     Opt->para_lit_limit)) {
	  BOOL good_pair =
	    (good_given ||
	     p->c->id < (unsigned long long) parm(Opt->para_restr_beg) ||
	     p->c->id > (unsigned long long) parm(Opt->para_restr_end));
	  if (good_pair) {
	    Current_inference_source = INFER_SOURCE_PARAMOD;
	    para_from_into(given, cf, p->c, ci, FALSE, cl_process);
	    para_from_into(p->c, cf, given, ci, TRUE, cl_process);
	  }
	}
      }
      free_context(cf);
      free_context(ci);
    }
  }

  if (collective_frontier_mode())
    collective_maybe_schedule_hint_probe(given, tail_before);

  Current_inference_source = INFER_SOURCE_OTHER;

  clock_stop(Clocks.infer);
}  // given_infer

/* Refresh a cold clause only if the active set has changed since it entered
   SOS.  A rewrite/unit/CAC change is represented as a copy inference, just
   as eager back simplification represents it in the compatibility loop.  A
   changed clause is sent through cl_process() again, which deliberately
   repeats exact hint matching and selector evaluation for the new body. */
static
BOOL discount_refresh_selected(Topform c)
{
  Topform copy;
  Topform subsumer;

  if (c->simplifier_epoch == Simplifier_epoch &&
      c->rewrite_epoch == Rewrite_epoch)
    return TRUE;

  Stats.passive_refresh_checks++;
  copy = copy_inference(c);
  cl_process_simplify(copy);

  if (copy->justification->next != NULL) {
    BOOL repairing_rule = eager_interreduced_demod_mode() &&
                          c->delayed_demodulator;
    copy->justification->u.id = c->id;
    if (eager_interreduced_demod_mode() && c->delayed_demodulator) {
      compact_rewrite_note_suspended_retirement(Compact_rewrite_rules);
      Stats.rewrite_only_demodulators_retired++;
      if (compact_rewrite_compaction_needed(Compact_rewrite_rules))
        compact_rewrite_compact(Compact_rewrite_rules);
      update_rewrite_only_stats();
    }
    retain_disabled_clause(c);
    Stats.passive_refresh_requeued++;
    if (repairing_rule)
      Rewrite_repair_depth++;
    cl_process(copy);
    limbo_process(FALSE);
    if (repairing_rule)
      Rewrite_repair_depth--;
    return FALSE;
  }

  delete_clause(copy);

  /* The clause was forward-subsumption checked when first retained, so this
     is necessary only after an active-state epoch change. */
  clock_start(Clocks.subsume);
  if (flag(Opt->ancestor_subsume)) {
    BOOL use_prf_weight = flag(Opt->proof_weight);
    subsumer = forward_subsumption_filter(c, anc_subsume_accept_cb,
                                          &use_prf_weight);
  }
  else
    subsumer = forward_subsumption(c);
  clock_stop(Clocks.subsume);

  if (subsumer != NULL && !c->used) {
    unsigned long long subsumer_id = subsumer->id;
    release_compact_index_clause(subsumer);
    if (flag(Opt->print_gen))
      printf("%sDISCOUNT refresh: %llu subsumed by %llu.\n",
             TPTP_PFX, c->id, subsumer_id);
    Stats.subsumed++;
    Stats.passive_refresh_subsumed++;
    if (eager_interreduced_demod_mode() && c->delayed_demodulator) {
      compact_rewrite_note_suspended_retirement(Compact_rewrite_rules);
      Stats.rewrite_only_demodulators_retired++;
      if (compact_rewrite_compaction_needed(Compact_rewrite_rules))
        compact_rewrite_compact(Compact_rewrite_rules);
      update_rewrite_only_stats();
    }
    retain_disabled_clause(c);
    return FALSE;
  }
  release_compact_index_clause(subsumer);

  c->simplifier_epoch = Simplifier_epoch;
  c->rewrite_epoch = Rewrite_epoch;
  return TRUE;
}  /* discount_refresh_selected */

static void restore_unchanged_dense_passive(
  Topform c, const struct dense_passive_view *view)
{
  Topform shell = NULL;
  if (view->delayed_demodulator) {
    int type = demodulator_type(c, parm(Opt->lex_dep_demod_lim),
                                flag(Opt->lex_dep_demod_sane));
    if (type == NOT_DEMODULATOR ||
        !compact_rewrite_add(Compact_rewrite_rules, c, type))
      fatal_error("rewrite refresh: cannot restore compact demodulator");
    shell = store_rewrite_only_shell(c, type);
    c->delayed_demodulator = TRUE;
  }
  else
    c->delayed_demodulator = FALSE;

  if (shell != NULL) {
    if (!detach_clause_id(c))
      fatal_error("rewrite refresh: materialized clause does not own proof ID");
    register_clause_with_id(shell);
  }
  else if (!detach_clause_id(c))
    fatal_error("rewrite refresh: ordinary clause does not own proof ID");
  c->id = 0;
  delete_clause(c);
  if (!dense_passive_reactivate_id(view->id, Simplifier_epoch,
                                   Rewrite_epoch,
                                   view->delayed_demodulator, FALSE))
    fatal_error("rewrite refresh: cannot reactivate dense record");
  update_rewrite_only_stats();
}

static BOOL update_rewrite_drain_mode(void)
{
  unsigned long long debt = dense_passive_rewrite_debt();
  unsigned high = (unsigned) parm(Opt->rewrite_refresh_high_water);
  unsigned low = (unsigned) parm(Opt->rewrite_refresh_low_water);
  Stats.rewrite_debt_current = debt;
  if (debt > Stats.rewrite_debt_peak)
    Stats.rewrite_debt_peak = debt;
  if (!Rewrite_drain_mode && debt >= high) {
    Rewrite_drain_mode = TRUE;
    Stats.rewrite_drain_entries++;
  }
  else if (Rewrite_drain_mode && debt <= low) {
    Rewrite_drain_mode = FALSE;
    Rewrite_drain_streak = 0;
    Stats.rewrite_drain_exits++;
    Rewrite_refresh_inference_streak = 0;
  }
  return Rewrite_drain_mode;
}

/* Spend one bounded, pointer-free repair turn.  Hinted passives receive the
   configured burst share, while a mandatory general turn advances an
   independent wraparound cursor.  Outside urgent debt drain, an inference
   ratio guarantees that both repair and inference make progress. */
static BOOL rewrite_refresh_turn(void)
{
  struct dense_passive_view view;
  unsigned scanned = 0;
  unsigned budget;
  unsigned hot_ratio;
  unsigned inference_ratio;
  BOOL hot = FALSE;
  BOOL rule_lane = FALSE;
  BOOL drain;
  Topform c;
  unsigned long long requeued_before, subsumed_before, kept_before;

  if (!eager_interreduced_demod_mode())
    return FALSE;
  drain = update_rewrite_drain_mode();
  inference_ratio = (unsigned) parm(Opt->rewrite_refresh_inference_ratio);
  /* A high-water mark raises repair priority; it must never become an
     unbounded exclusive loop.  Interreduction can legitimately replenish
     its own exact debt when a composed replacement overlaps another rule.
     Yield one ordinary scheduler turn after every bounded urgent burst so
     givens and collective descriptors remain fair even when debt does not
     converge. */
  if (drain && Rewrite_drain_streak >=
                 (unsigned) parm(Opt->rewrite_refresh_drain_burst)) {
    Rewrite_drain_streak = 0;
    Stats.rewrite_drain_yields++;
    return FALSE;
  }
  if (!drain && Rewrite_refresh_inference_streak < inference_ratio)
    return FALSE;
  memset(&view, 0, sizeof(view));
  budget = (unsigned) parm(Opt->rewrite_refresh_raw_budget);
  hot_ratio = (unsigned) parm(Opt->rewrite_refresh_hot_ratio);
  if (drain ||
      (hot_ratio != 0 && Rewrite_interreduce_streak < hot_ratio)) {
    scanned = dense_passive_scan_stale(&Rewrite_interreduce_cursor,
                                       Rewrite_epoch,
                                       DENSE_STALE_REWRITE,
                                       budget, &view);
    rule_lane = view.id != 0;
  }
  if (!drain && view.id == 0 &&
      hot_ratio != 0 && Rewrite_refresh_hot_streak < hot_ratio &&
      Rewrite_interreduce_streak < hot_ratio) {
    scanned += dense_passive_scan_stale(&Rewrite_refresh_hot_cursor,
                                        Rewrite_epoch, DENSE_STALE_HINTED,
                                        budget, &view);
    hot = view.id != 0;
  }
  if (!drain && view.id == 0) {
    unsigned general_scanned = dense_passive_scan_stale(
      &Rewrite_refresh_general_cursor, Rewrite_epoch, DENSE_STALE_GENERAL,
      budget, &view);
    scanned += general_scanned;
    hot = FALSE;
    Rewrite_interreduce_streak = 0;
  }
  Stats.rewrite_refresh_scanned += scanned;
  if (view.id == 0) {
    if (drain) {
      Stats.rewrite_drain_turns++;
      Rewrite_drain_streak++;
      return TRUE;
    }
    Rewrite_refresh_inference_streak = 0;
    return FALSE;
  }
  if (view.rewrite_epoch < Rewrite_epoch) {
    unsigned long long lag = Rewrite_epoch - view.rewrite_epoch;
    if (lag > Stats.rewrite_refresh_lag_max)
      Stats.rewrite_refresh_lag_max = lag;
  }

  if (!dense_passive_deactivate_id(view.id, NULL))
    fatal_error("rewrite refresh: stale record disappeared");
  c = activate_dense_passive_internal(view.store_position, view.id,
                                      view.hint_id, FALSE);
  if (c == NULL)
    fatal_error("rewrite refresh: cannot materialize dense passive");
  c->weight = view.weight;
  c->semantics = view.semantics;
  c->simplifier_epoch = view.simplifier_epoch;
  c->rewrite_epoch = view.rewrite_epoch;
  c->delayed_demodulator = view.delayed_demodulator;
  c->rewrite_rule_dirty = view.rewrite_rule_dirty;
  Stats.rewrite_refresh_materialized++;
  if (rule_lane)
    Rewrite_interreduce_streak++;
  else if (hot) {
    Stats.rewrite_refresh_hot_turns++;
    Rewrite_refresh_hot_streak++;
  }
  else {
    Stats.rewrite_refresh_general_turns++;
    Rewrite_refresh_hot_streak = 0;
  }

  requeued_before = Stats.passive_refresh_requeued;
  subsumed_before = Stats.passive_refresh_subsumed;
  kept_before = Stats.kept;
  if (view.delayed_demodulator)
    Stats.rewrite_interreduce_turns++;
  if (discount_refresh_selected(c)) {
    restore_unchanged_dense_passive(c, &view);
    Stats.rewrite_refresh_unchanged++;
    if (view.delayed_demodulator)
      Stats.rewrite_interreduce_unchanged++;
  }
  else {
    if (Stats.passive_refresh_requeued != requeued_before)
      Stats.rewrite_refresh_rewritten++;
    else if (Stats.passive_refresh_subsumed != subsumed_before)
      Stats.rewrite_refresh_subsumed++;
    if (view.delayed_demodulator) {
      if (Stats.passive_refresh_requeued != requeued_before &&
          Stats.kept != kept_before)
        Stats.rewrite_interreduce_changed++;
      else
        Stats.rewrite_interreduce_collapsed++;
    }
  }
  if (compact_rewrite_compaction_needed(Compact_rewrite_rules)) {
    compact_rewrite_compact(Compact_rewrite_rules);
    update_rewrite_only_stats();
  }
  if (drain) {
    Stats.rewrite_drain_turns++;
    Rewrite_drain_streak++;
    update_rewrite_drain_mode();
  }
  else
    Rewrite_refresh_inference_streak = 0;
  return TRUE;
}

/*************
 *
 *   rebuild_sos_index()
 *
 *************/

static
void rebuild_sos_index(void)
{
  fatal_error("rebuild_sos_index not implemented for given_selection");
#if 0
  Clist_pos p;
  printf("\nNOTE: Reweighing all SOS clauses and rebuilding SOS indexes.\n");
  zap_picker_indexes();
  update_picker_ratios(Opt);  /* in case they've been changed */
  for (p = Glob.sos->first; p; p = p->next) {
    Topform c = p->c;
    clause_wt_with_adjustments(c); /* weigh the clause (wt stored in c) */
    update_pickers(c, TRUE); /* insert (lower-level) into picker indexes */
#endif
}  /* rebuild_sos_index */

/*************
 *
 *   make_inferences()
 *
 *   Assume that there are inferences to make.
 *
 *   If we had the option of using the pair algorithm instead of
 *   the given algorithm, we could make that decision here.
 *
 *************/

static
void make_inferences(void)
{
  Topform given_clause;
  char *selection_type;
  BOOL givens = givens_available();
  BOOL balanced_drain = collective_update_drain_mode();
  BOOL collective_due = collective_frontier_mode() &&
    Collective_batch_head != NULL &&
    (Collective_batch_turn || !givens || balanced_drain);
  BOOL collective_space = !collective_frontier_mode() ||
    collective_candidate_occupancy() <
      (unsigned long long) parm(Opt->collective_candidate_cache);

  if (rewrite_refresh_turn())
    return;
  if (eager_interreduced_demod_mode())
    Stats.rewrite_inference_turns++;
  if (eager_interreduced_demod_mode() &&
      Rewrite_refresh_inference_streak != UINT_MAX)
    Rewrite_refresh_inference_streak++;

  if (collective_balanced_mode() &&
      Collective_candidate_heap_count != 0) {
    unsigned long long target =
      (unsigned long long) parm(Opt->collective_candidate_window);
    unsigned long long cache =
      (unsigned long long) parm(Opt->collective_candidate_cache);
    BOOL commit_due;
    if (target > cache)
      target = cache;
    commit_due =
      Collective_candidate_heap_count >= target ||
      Collective_batch_head == NULL ||
      !collective_space ||
      Collective_pool_expansions_since_commit >=
        (unsigned) parm(Opt->collective_candidate_commit_interval);
    if (commit_due) {
      collective_candidate_pool_commit();
      return;
    }
  }

  /* Spend one descriptor-expansion turn after the configured number of given
     activations.  If SOS is empty, drain the finite batch queue.  The
     round-robin queue is fair: every finite descriptor cursor is advanced
     repeatedly. */
  if (collective_due && (collective_space || !givens)) {
    if (balanced_drain && givens)
      Stats.collective_givens_withheld++;
    if (collective_balanced_mode() && collective_space &&
        collective_expand_discovery_one()) {
      Collective_pool_expansions_since_commit++;
      Collective_batch_turn = FALSE;
      Collective_givens_since_batch = 0;
      return;
    }
    collective_expand_one_batch();
    if (collective_balanced_mode()) {
      Collective_pool_expansions_since_commit++;
      Collective_discovery_turns_since_fair = 0;
    }
    Collective_batch_turn = FALSE;
    Collective_givens_since_batch = 0;
    return;
  }

  if (collective_due && !collective_space)
    Stats.collective_candidate_cache_stalls++;

  /* The normal end-of-iteration limbo pass frees transient candidate-body
     capacity.  Never violate the descriptor bound merely because that pass
     has not happened yet. */
  if (collective_balanced_mode() &&
      !collective_balanced_admission_allowed()) {
    if (givens)
      Stats.collective_givens_withheld++;
    return;
  }

  if (!givens)
    return;

  clock_start(Clocks.pick_given);
  given_clause = get_given_clause2(Glob.sos,Stats.given, Opt, &selection_type);
  clock_stop(Clocks.pick_given);

  if (given_clause != NULL) {

    if (discount_mode() && !discount_refresh_selected(given_clause))
      return;

    // Print "level" message for breadth-first; also "level" actions.

    if (flag(Opt->breadth_first) &&
	parm(Opt->true_part) == 0 &&
	parm(Opt->false_part) == 0 &&
	parm(Opt->weight_part) == 0 &&
	parm(Opt->random_part) == 0 &&
	str_ident(selection_type, "A") &&
	given_clause->id > Bf_last_of_level) {
      Bf_level++;
      Bf_last_of_level = clause_ids_assigned();
      if (!flag(Opt->quiet)) {
	printf("\nNOTE: Starting on level %d, last clause "
	       "of level %d is %d.\n",
	       Bf_level, Bf_level-1, Bf_last_of_level);
	fflush(stdout);
	fprintf(stderr, "\nStarting on level %d, last clause "
		"of level %d is %d.\n",
		Bf_level, Bf_level-1, Bf_last_of_level);
	fflush(stderr);
      }
      statistic_actions("level", Bf_level);
    }

    Stats.given++;
    given_clause->was_given = TRUE;
    set_hints_given_count(Stats.given);
    if (flag(Opt->search_event_trace))
      printf("%sGIVEN_TRACE given=%llu clause=%llu fingerprint=%016llx "
             "generated=%llu kept=%llu\n",
             TPTP_PFX, Stats.given, given_clause->id,
             hint_trace_clause_hash(given_clause), Stats.generated,
             Stats.kept);
    if (collective_frontier_mode() && given_clause->matching_hint != NULL) {
      Stats.collective_hint_selected_total++;
      if (strcmp(selection_type, "Hha") == 0)
        Stats.collective_hint_selected_hha++;
      else if (strcmp(selection_type, "Hw") == 0)
        Stats.collective_hint_selected_hw++;
      else if (strcmp(selection_type, "LH") == 0)
        Stats.collective_hint_selected_lh++;
      else
        Stats.collective_hint_selected_other++;
    }

    /* max_nohints: exit after N consecutive givens w/o hint match (Veroff) */
    {
      BOOL hint_matcher = given_clause->matching_hint != NULL
	&& (parm(Opt->degrade_limit) == -1
	    || (int)(given_clause->weight / 1000) <= parm(Opt->degrade_limit));
      if (given_clause->initial || hint_matcher)
	Nohints_count = 0;
      else
	Nohints_count++;
      if (parm(Opt->max_nohints) > 0
	  && Nohints_count > parm(Opt->max_nohints)) {
	printf("\n%% %d givens in a row w/o an input clause or a hint matcher "
	       "(max_nohints).\n", parm(Opt->max_nohints));
	done_with_search(MAX_NOHINTS_EXIT);
      }
    }

    // Clause-count-based reporting
    if (parm(Opt->report_given) > 0)
      possible_report();

    // Maybe disable back subsumption.

    if (over_parm_limit(Stats.given, Opt->max_given))
      done_with_search(MAX_GIVEN_EXIT);

    if (Stats.given == parm(Opt->backsub_check)) {
      int ratio = (Stats.back_subsumed == 0 ?
		   INT_MAX :
		   Stats.kept / Stats.back_subsumed);
      if (ratio > 20) {
	clear_flag(Opt->back_subsume, !flag(Opt->quiet));
	if (!flag(Opt->quiet)) {
	  printf("\nNOTE: Back_subsumption disabled, ratio of kept"
		 " to back_subsumed is %d (%.2f of %.2f sec).\n",
		 ratio, clock_seconds(Clocks.back_subsume), user_seconds());
	  fflush(stdout);
	}
      }
    }
    
    if (flag(Opt->print_given) || Stats.given % 500 == 0) {
      if (given_clause->weight == round(given_clause->weight))
	printf("\n%sgiven #%s (%s,wt=%d): ",
	       TPTP_PFX, comma_num(Stats.given), selection_type, (int) given_clause->weight);
      else
	printf("\n%sgiven #%s (%s,wt=%.3f): ",
	       TPTP_PFX, comma_num(Stats.given), selection_type, given_clause->weight);
      fwrite_clause(stdout, given_clause, CL_FORM_STD);
    }

    statistic_actions("given", Stats.given);

    if (To_trace_cl != NULL &&
	!clist_member(To_trace_cl, Glob.sos) &&
	!clist_member(To_trace_cl, Glob.usable) &&
	!clist_member(To_trace_cl, Glob.limbo)) {
      printf("\n*** Trace: clause %llu has disappeared.\n", To_trace_cl->id);
      To_trace_cl = NULL;
    }

    if (discount_mode()) {
      unsigned long long given_id = given_clause->id;
      BOOL rewrite_transition =
        maximum_discount_demod_mode() && given_clause->delayed_demodulator;
      Topform activated;

      /* The selector owns passive clauses without active indexes.  Reuse the
         established limbo activation order now that this clause has actually
         been selected: install its literal index and possible demodulator,
         perform factoring/backward work against active clauses only, then
         put it in Usable and the clashable inference index.  Any conclusions
         generated during activation take the ordinary passive shortcut in
         limbo_process(). */
      /* Advancing before activation means every conclusion generated from
         this given is stamped with the state that already includes it. */
      if (Simplifier_epoch != UINT_MAX)
        Simplifier_epoch++;
      clist_append(given_clause, Glob.limbo);
      given_clause->delayed_demodulator = FALSE;
      cl_process_new_demod(given_clause, rewrite_transition);
      index_literals(given_clause, INSERT, Clocks.index, FALSE);
      limbo_process(FALSE);

      activated = find_clause_by_id(given_id);
      if (activated != NULL && clist_member(activated, Glob.usable)) {
	if (!restricted_denial(activated))
	  index_clashable(activated, INSERT);
	collective_note_activation(
	  activated,
	  collective_hyper_enabled() && !restricted_denial(activated));
	given_infer(activated);
      }
    }
    else {
      clist_append(given_clause, Glob.usable);
      index_clashable(given_clause, INSERT);
      given_infer(given_clause);
    }

    if (collective_frontier_mode()) {
      if (Collective_givens_since_batch < UINT_MAX)
        Collective_givens_since_batch++;
      if (Collective_batch_head != NULL &&
          Collective_givens_since_batch >=
            (unsigned) parm(Opt->collective_given_ratio))
        Collective_batch_turn = TRUE;
    }
  }
}  // make_inferences

/*************
 *
 *   orient_input_eq()
 *
 *   This is designed for input clauses, and it's a bit tricky.  If any
 *   equalities are flipped, we make the flip operations info inferences
 *   so that proofs are complete.  This involves replacing and hiding
 *   (disabling) the original clause.
 *
 *************/

static
Topform orient_input_eq(Topform c)
{
  Topform new = copy_inference(c);
  orient_equalities(new, TRUE);
  if (clause_ident(c->literals, new->literals)) {
    delete_clause(new);
    /* the following puts "oriented" marks on c */
    orient_equalities(c, TRUE);
    return c;
  }
  else {
    /* Replace c with new in Usable. */
    assign_clause_id(new);
    mark_parents_as_used(new);
    clist_swap(c, new);
    retain_disabled_clause(c);
    return new;
  }
}  /* orient_input_eq */

/*************
 *
 *   auto_inference()
 *
 *   This looks at the clauses and decides which inference rules to use.
 *
 *************/

static
void auto_inference(Clist sos, Clist usable, Prover_options opt)
{
  BOOL print = !flag(opt->quiet);
  if (print)
    printf("\nAuto_inference settings:\n");

  if (Glob.equality) {
    if (print)
      printf("  %% set(paramodulation).  %% (positive equality literals)\n");
    set_flag(opt->paramodulation, print);
  }

  if (!Glob.equality || !Glob.unit) {
    if (Glob.horn) {
      Plist clauses = NULL;
      clauses = prepend_clist_to_plist(clauses, sos);
      clauses = prepend_clist_to_plist(clauses, usable);

      if (Glob.equality) {
	if (print)
	  printf("  %% set(hyper_resolution)."
		 "  %% (nonunit Horn with equality)\n");
	set_flag(opt->hyper_resolution, print);
	if (print)
	  printf("  %% set(neg_ur_resolution)."
		 "  %% (nonunit Horn with equality)\n");
	set_flag(opt->neg_ur_resolution, print);

	if (parm(opt->para_lit_limit) == -1) {
	  int para_lit_limit = most_literals(clauses);
	  if (print)
	    printf("  %% assign(para_lit_limit, %d)."
		   "  %% (nonunit Horn with equality)\n",
		   para_lit_limit);
	  assign_parm(opt->para_lit_limit, para_lit_limit, print);
	}
      }
      else {
	int diff = neg_pos_depth_difference(clauses);
	if (diff > 0) {
	  if (print)
	    printf("  %% set(hyper_resolution)."
		   "  %% (HNE depth_diff=%d)\n", diff);
	  set_flag(opt->hyper_resolution, print);
	}
	else {
	  if (print)
	    printf("  %% set(neg_binary_resolution)."
		   "  %% (HNE depth_diff=%d)\n", diff);
	  set_flag(opt->neg_binary_resolution, print);
	  if (print)
	    printf("  %% clear(ordered_res)."
		   "  %% (HNE depth_diff=%d)\n", diff);
	  clear_flag(opt->ordered_res, print);
	  if (print)
	    printf("  %% set(ur_resolution)."
		   "  %% (HNE depth_diff=%d)\n", diff);
	  set_flag(opt->ur_resolution, print);
	}
      }
      zap_plist(clauses);
    }
    else {
      // there are nonhorn clauses
      if (print) {
	printf("  %% set(binary_resolution).  %% (non-Horn)\n");
      }
      set_flag(opt->binary_resolution, print);
      if (Glob.number_of_clauses < 100) {
	if (print)
	  printf("  %% set(neg_ur_resolution)."
		 "  %% (non-Horn, less than 100 clauses)\n");
	set_flag(opt->neg_ur_resolution, print);
      }
    }
  }
}  /* auto_inference */

/*************
 *
 *   auto_process()
 *
 *   This looks at the clauses and decides some processing options.
 *
 *************/

static
void auto_process(Clist sos, Clist usable, Prover_options opt)
{
  BOOL print = !flag(opt->quiet);
  Plist clauses;
  BOOL horn;

  clauses = prepend_clist_to_plist(NULL, sos);
  clauses = prepend_clist_to_plist(clauses, usable);

  horn  = all_clauses_horn(clauses);

  if (print)
    printf("\nAuto_process settings:");

  if (horn) {
    if (neg_nonunit_clauses(clauses) > 0) {
      if (print)
	printf("\n  %% set(unit_deletion)."
	       "  %% (Horn set with negative nonunits)\n");
      set_flag(opt->unit_deletion, print);
    }
    else {
      if (print)
	printf("  (no changes).\n");
    }
  }

  else {
    // there are nonhorn clauses
    if (print)
      printf("\n  %% set(factor).  %% (non-Horn)\n");
    set_flag(opt->factor, print);
    if (print)
      printf("  %% set(unit_deletion).  %% (non-Horn)\n");
    set_flag(opt->unit_deletion, print);
  }
  zap_plist(clauses);
}  /* auto_process */

/*************
 *
 *   auto_denials()
 *
 *************/

static
void auto_denials(Clist sos, Clist usable, Prover_options opt)
{
  int changes = 0;
  int echo_id = str_to_flag_id("echo_input");
  BOOL echo = (echo_id == -1 ? TRUE : flag(echo_id));
  BOOL quiet = flag(opt->quiet);

  if (!quiet)
    printf("\nAuto_denials:");

  if (Glob.horn) {
    Plist neg_clauses = plist_cat(neg_clauses_in_clist(sos),
				  neg_clauses_in_clist(usable));
    Plist p;
    for (p = neg_clauses; p; p = p->next) {
      Topform c = p->v;
      char *label = get_string_attribute(c->attributes, Att.label, 1);
      Term answer = get_term_attribute(c->attributes, Att.answer, 1);
      if (label && !answer) {
	Term t = get_rigid_term(label, 0);
	c->attributes = set_term_attribute(c->attributes, Att.answer, t);
	if (echo && !quiet) {
	  printf("%s", changes == 0 ? "\n" : "");
	  printf("  %% copying label %s to answer in negative clause\n", label);
	}
	changes++;
      }
    }

    if (!quiet && changes > 0 && !echo)
      printf("\n  %% copied labels to answers in %d negative clauses (not echoed)\n",
	     changes);

    if (Glob.number_of_neg_clauses > 1 && parm(opt->max_proofs) == 1
        && !flag(opt->tptp_output)) {
      /* In TPTP mode, max_proofs stays at 1 (standard ATP behavior). */
      if (!quiet) {
        printf("%s", changes == 0 ? "\n" : "");
        printf("  %% assign(max_proofs, %d)."
	       "  %% (Horn set with more than one neg. clause)\n",
	       Glob.number_of_neg_clauses);
      }
      assign_parm(opt->max_proofs, Glob.number_of_neg_clauses, TRUE);
      check_constant_sharing(neg_clauses);
      changes++;
    }
    zap_plist(neg_clauses);
  }

  if (!quiet && changes == 0)
    printf("  (%sno changes).\n", Glob.horn ? "" : "non-Horn, ");
}  /* auto_denials */

/*************
 *
 *   init_search()
 *
 *************/

static
void init_search_early(void)
{
  // Phase 1: Initialize clocks, weights, given-clause selection,
  // delete rules, actions, and term ordering MODE.
  // These do NOT depend on clauses being loaded.

  // Initialize clocks.

  Clocks.pick_given    = clock_init("pick_given");
  Clocks.infer         = clock_init("infer");
  Clocks.preprocess    = clock_init("preprocess");
  Clocks.demod         = clock_init("demod");
  Clocks.unit_del      = clock_init("unit_deletion");
  Clocks.redundancy    = clock_init("redundancy");
  Clocks.conflict      = clock_init("conflict");
  Clocks.weigh         = clock_init("weigh");
  Clocks.hints         = clock_init("hints");
  Clocks.subsume       = clock_init("subsume");
  Clocks.semantics     = clock_init("semantics");
  Clocks.back_subsume  = clock_init("back_subsume");
  Clocks.back_demod    = clock_init("back_demod");
  Clocks.back_unit_del = clock_init("back_unit_del");
  Clocks.index         = clock_init("index");
  Clocks.disable       = clock_init("disable");

  init_actions(Glob.actions,
	       rebuild_sos_index, done_with_search, infer_outside_loop);
  init_weight(Glob.weights,
	      floatparm(Opt->variable_weight),
	      floatparm(Opt->constant_weight),
	      floatparm(Opt->not_weight),
	      floatparm(Opt->or_weight),
	      floatparm(Opt->sk_constant_weight),
	      floatparm(Opt->prop_atom_weight),
	      floatparm(Opt->nest_penalty),
	      floatparm(Opt->depth_penalty),
	      floatparm(Opt->var_penalty),
	      floatparm(Opt->complexity));
  init_resonators(Glob.resonators);
  if (number_of_resonators() > 0 && !flag(Opt->quiet))
    printf("%% %d resonator%s installed (Wos wildcard matching).\n",
	   number_of_resonators(),
	   number_of_resonators() == 1 ? "" : "s");

  if (Glob.given_selection == NULL)
    Glob.given_selection = selector_rules_from_options(Opt);
  else if (Resume_dir == NULL && flag(Opt->input_sos_first))
    /* Skip on resume: saved_input.txt already has the complete
       given_selection list including the "I" rule from the original run.
       Re-prepending would duplicate it, corrupting the picker cycle. */
    Glob.given_selection = plist_prepend(Glob.given_selection,
					 selector_rule_term("I", "high", "age",
							    "initial",
							    INT_MAX));
  init_giv_select(Glob.given_selection);

  /* On resume, saved_input.txt already has the complete delete_rules.
     Skip merging defaults to avoid duplication. */
  if (Resume_dir == NULL)
    Glob.delete_rules = plist_cat(delete_rules_from_options(Opt),
				  Glob.delete_rules);
  else if (Glob.delete_rules == NULL)
    Glob.delete_rules = delete_rules_from_options(Opt);

  init_white_black(Glob.keep_rules, Glob.delete_rules);

  // Term ordering mode (LPO/RPO/KBO assignment).
  // symbol_order and KBO weight computation are deferred to
  // init_search_late() because they examine clause lists.

  if (!flag(Opt->quiet))
    printf("\nTerm ordering decisions:\n");

  if (stringparm(Opt->order, "lpo")) {
    assign_order_method(LPO_METHOD);
    all_symbols_lrpo_status(LRPO_LR_STATUS);
    set_lrpo_status(str_to_sn(eq_sym(), 2), LRPO_MULTISET_STATUS);
  }
  else if (stringparm(Opt->order, "rpo")) {
    assign_order_method(RPO_METHOD);
    all_symbols_lrpo_status(LRPO_MULTISET_STATUS);
  }
  else if (stringparm(Opt->order, "kbo")) {
    assign_order_method(KBO_METHOD);
  }

}  /* init_search_early */

/*************
 *
 *   init_search_late()
 *
 *   Phase 2 of search initialization.  These steps depend on
 *   Glob.sos/usable/demods being populated and on Glob.equality/horn/unit
 *   being set by basic_clause_properties().
 *
 *************/

static
void init_search_late(void)
{
  /* On resume, skip all clause-dependent initialization.  The checkpoint
     captures the final state of precedence, KBO weights, unfold symbols,
     and inference/process flags.  load_checkpoint_into_loop() calls
     symbol_order() on the loaded clauses.  Only the package option calls
     (resolution_options, paramodulation_options) are needed here. */
  if (Resume_dir != NULL)
    goto package_options;

  // Symbol precedence (examines clauses)

  symbol_order(Glob.usable, Glob.sos, Glob.demods, !flag(Opt->quiet));
  if (flag(Opt->multi_order_trial))
    multi_order_trial(Glob.usable, Glob.sos, !flag(Opt->quiet));

  if (Glob.kbo_weights) {
    if (!stringparm(Opt->order, "kbo")) {
      assign_stringparm(Opt->order, "kbo", TRUE);
      if (!flag(Opt->quiet))
        printf("assign(order, kbo), because KB weights were given.\n");
    }
    init_kbo_weights(Glob.kbo_weights);
    if (!flag(Opt->quiet))
      print_kbo_weights(stdout);
  }
  else if (stringparm(Opt->order, "kbo")) {
    auto_kbo_weights(Glob.usable, Glob.sos);
    if (!flag(Opt->quiet))
      print_kbo_weights(stdout);
  }

  if (!flag(Opt->quiet)) {
    print_rsym_precedence(stdout);
    print_fsym_precedence(stdout);
  }

  if (flag(Opt->inverse_order)) {
    if (exists_preliminary_precedence(FUNCTION_SYMBOL)) {  // lex command
      if (!flag(Opt->quiet))
	printf("Skipping inverse_order, because there is a function_order (lex) command.\n");
    }
    else if (stringparm(Opt->order, "kbo")) {
      if (!flag(Opt->quiet))
	printf("Skipping inverse_order, because term ordering is KBO.\n");
    }
    else {
      BOOL change = inverse_order(Glob.sos);
      if (!flag(Opt->quiet)) {
	printf("After inverse_order: ");
	if (change)
	  print_fsym_precedence(stdout);
	else
	  printf(" (no changes).\n");
      }
    }
  }

  if (stringparm(Opt->eq_defs, "unfold")) {
    if (exists_preliminary_precedence(FUNCTION_SYMBOL)) {  // lex command
      if (!flag(Opt->quiet))
	printf("Skipping unfold_eq, because there is a function_order (lex) command.\n");
    }
    else
      unfold_eq_defs(Glob.sos, INT_MAX, 3, !flag(Opt->quiet));
  }
  else if (stringparm(Opt->eq_defs, "fold")) {
    if (exists_preliminary_precedence(FUNCTION_SYMBOL)) {  // lex command
      if (!flag(Opt->quiet))
	printf("Skipping fold_eq, because there is a function_order (lex) command.\n");
    }
    else {
      BOOL change = fold_eq_defs(Glob.sos, stringparm(Opt->order, "kbo"));
      if (!flag(Opt->quiet)) {
	printf("After fold_eq: ");
	if (change)
	  print_fsym_precedence(stdout);
	else
	  printf(" (no changes).\n");
      }
    }
  }

  // Automatic inference and processing settings

  if (flag(Opt->auto_inference))
    auto_inference(Glob.sos, Glob.usable, Opt);

  if (flag(Opt->auto_process))
    auto_process(Glob.sos, Glob.usable, Opt);

  // Tell packages about options and other things.

package_options:
  resolution_options(flag(Opt->ordered_res),
		     flag(Opt->check_res_instances),
		     flag(Opt->initial_nuclei),
		     parm(Opt->ur_nucleus_limit),
		     flag(Opt->eval_rewrite));

  paramodulation_options(flag(Opt->ordered_para),
			 flag(Opt->check_para_instances),
			 FALSE,
			 flag(Opt->basic_paramodulation),
			 flag(Opt->para_from_vars),
			 flag(Opt->para_into_vars),
			 flag(Opt->para_from_small));

}  /* init_search_late */

/*************
 *
 *   init_search()
 *
 *   Full search initialization (both phases).
 *   Used by the normal (non-resume) path where clauses are already loaded.
 *
 *************/

static
void init_search(void)
{
  init_search_early();
  init_search_late();
}  /* init_search */

/* Configure the optional compact indexes before their ordinary index shells
   are initialized.  Normal startup and checkpoint resume must make exactly
   the same ownership decision; previously these calls lived only in initial
   clause processing, which made a resume process silently prepare legacy
   indexes instead. */
static void configure_search_indexes(void)
{
  compact_rewrite_set_compaction_stale_pct(
    (unsigned) parm(Opt->compact_index_stale_pct));
  configure_compact_unit_stale_pct(
    (unsigned) parm(Opt->compact_index_stale_pct));
  compact_unit_index_set_strategy(
    str_ident(stringparm1(Opt->compact_unit_strategy), "position") ?
      COMPACT_UNIT_POSITION : COMPACT_UNIT_ROOT_SCAN);
  configure_compact_back_demod_stale_pct(
    (unsigned) parm(Opt->compact_index_stale_pct));
  configure_compact_unit_term_pool(Compact_terms);
  configure_compact_unit_index(
    flag(Opt->compact_unit_subsumption_audit),
    flag(Opt->compact_otter_unit_index));
  configure_compact_nonunit_index(
    flag(Opt->compact_nonunit_subsumption_audit),
    flag(Opt->compact_otter_nonunit_index));
  configure_compact_clause_access(compact_otter_resolve_clause,
                                  compact_otter_release_clause, NULL);
  configure_compact_back_demod(
    flag(Opt->compact_back_demod_audit),
    compact_back_demod_authoritative_mode());
  configure_compact_back_demod_term_pool(Compact_terms);
  configure_compact_back_demod_access(compact_otter_resolve_clause,
                                      compact_otter_release_clause,
                                      compact_otter_advise_rebuild_batch,
                                      NULL);
}

/*************
 *
 *   index_and_process_initial_clauses()
 *
 *************/

static
void index_and_process_initial_clauses(void)
{
  Clist_pos p;
  Clist temp_sos;

  // Index Usable clauses if hyper, UR, or binary-res are set.

  Glob.use_clash_idx = live_clash_index_needed();

  // Allocate and initialize indexes (even if they won't be used).

  set_fpa_hash_threshold(parm(Opt->fpa_hash_threshold));
  set_discrim_hash_threshold(parm(Opt->discrim_hash_threshold));

  int fpa_depth = parm(Opt->fpa_depth);
  configure_search_indexes();
  init_literals_index(fpa_depth);  // fsub, bsub, fudel, budel, ucon

  init_demodulator_index(DISCRIM_BIND, ORDINARY_UNIF, 0);

  init_back_demod_index(FPA, ORDINARY_UNIF, fpa_depth);

  Glob.clashable_idx = lindex_init(FPA, ORDINARY_UNIF, fpa_depth,
				   FPA, ORDINARY_UNIF, fpa_depth);

  init_hints(ORDINARY_UNIF, Att.bsub_hint_wt,
	     flag(Opt->collect_hint_labels),
	     flag(Opt->back_demod_hints),
	     configured_hint_fpa_depth(),
	     packed_hint_bank_mode(),
	     better_packed_hint_mode(),
	     fast_packed_hint_mode(),
	     current_demodulate_clause);
  set_hint_match_stats(flag(Opt->hint_match_stats));
  set_hint_match_once(flag(Opt->hint_match_once));
  init_semantics(Glob.interps, Clocks.semantics,
		 stringparm1(Opt->multiple_interps),
		 parm(Opt->eval_limit),
		 parm(Opt->eval_var_limit));

  // Do Sos and Denials last, in case we PROCESS_INITIAL_SOS.

  ////////////////////////////////////////////////////////////////////////////
  // Usable

  for (p = Glob.usable->first; p != NULL; p = p->next) {
    Topform c = p->c;
    /* Guard: with -cores the parent runs predicate elimination after
     clausification, so the shared input can contain derived clauses
     that already carry IDs; children must not re-assign them (fatal). */
    if (c->id == 0)
      assign_clause_id(c);
    mark_maximal_literals(c->literals);
    mark_selected_literals(c->literals, stringparm1(Opt->literal_selection));
    if (flag(Opt->dont_flip_input))
      orient_equalities(c, FALSE);  // mark, but don't allow flips
    else
      c = orient_input_eq(c);  /* this replaces c if any flipping occurs */
    index_literals(c, INSERT, Clocks.index, FALSE);
    index_back_demod(c, INSERT, Clocks.index, flag(Opt->back_demod));
    index_clashable(c, INSERT);
  }

  ////////////////////////////////////////////////////////////////////////////
  // Demodulators

  if (!clist_empty(Glob.demods) && !flag(Opt->eval_rewrite)) {
    fflush(stdout);
    bell(stderr);
    fprintf(stderr,
	    "\nWARNING: The use of input demodulators is not well tested\n"
	    "and discouraged.  You might need to clear(process_initial_sos)\n"
	    "so that sos clauses are not rewritten and deleted.\n");
    fflush(stderr);
  }

  for (p = Glob.demods->first; p != NULL; p = p->next) {
    Topform c = p->c;
    if (c->id == 0)   /* see the guard note in the Usable loop above */
      assign_clause_id(c);
    if (flag(Opt->eval_rewrite)) {
      if (c->is_formula) {
	Formula old_formula = c->formula;
	/* make it into a pseudo-clause */
	c->literals = new_literal(TRUE, formula_to_term(old_formula));
	upward_clause_links(c);
	zap_formula(old_formula);
	c->is_formula = FALSE;
	clause_set_variables(c, MAX_VARS);
	mark_oriented_eq(c->literals->atom);
      }
    }
    else {
      if (!pos_eq_unit(c->literals))
	fatal_error("input demodulator is not equation");
      else {
	int type;
	if (flag(Opt->dont_flip_input))
	  orient_equalities(c, FALSE);  /* don't allow flips */
	else
	  c = orient_input_eq(c);  /* this replaces c if any flipping occurs */
	if (c->justification->next != NULL) {
	  if (!flag(Opt->quiet)) {
	    printf("\nNOTE: input demodulator %llu has been flipped.\n", c->id);
	    fflush(stdout);
	  }
	  fprintf(stderr, "\nNOTE: input demodulator %llu has been flipped.\n",
		  c->id);
	  if (flag(Opt->bell))
	    bell(stderr);
	  fflush(stderr);
	}
	type = demodulator_type(c,
				parm(Opt->lex_dep_demod_lim),
				flag(Opt->lex_dep_demod_sane));
	if (flag(Opt->dont_flip_input) &&
	    type != ORIENTED &&
	    !renamable_flip_eq(c->literals->atom)) {
	  type = ORIENTED;  /* let the user beware */
	  mark_oriented_eq(c->literals->atom);
	  bell(stderr);
	  fprintf(stderr,"\nWARNING: demodulator does not satisfy term order\n");
	  fflush(stderr);
	  if (!flag(Opt->quiet)) {
	    printf("\nWARNING: demodulator does not satisfy term order: ");
	    f_clause(c);
	    fflush(stdout);
	  }
	}
	else if (type == NOT_DEMODULATOR) {
	  Term a = ARG(c->literals->atom,0);
	  Term b = ARG(c->literals->atom,1);
	  if (!flag(Opt->quiet)) {
	    printf("bad input demodulator: "); f_clause(c);
	  }
	  if (term_ident(a, b))
	    fatal_error("input demodulator is instance of x=x");
	  else if (!variables_subset(a, b) && !variables_subset(b, a))
	    fatal_error("input demoulator does not have var-subset property");
	  else
	    fatal_error("input demoulator not allowed");
	}
	if (eager_interreduced_demod_mode() || compact_otter_demod_mode()) {
	  if (!compact_rewrite_add(Compact_rewrite_rules, c, type))
	    fatal_error("index_and_process_initial_clauses: duplicate compact demodulator");
	  update_rewrite_only_stats();
	}
	else {
	  if (compact_otter_audit_mode()) {
	    if (!compact_rewrite_add(Compact_rewrite_rules, c, type))
	      fatal_error("index_and_process_initial_clauses: duplicate audited compact demodulator");
	    update_rewrite_only_stats();
	  }
	  index_demodulator(c, type, INSERT, Clocks.index);
	}
      }
    }
  }

  if (flag(Opt->eval_rewrite))
    init_dollar_eval(Glob.demods);

  ////////////////////////////////////////////////////////////////////////////
  // Hints
  
  if (Glob.hints->first) {
    int hint_id_number = 1;
    for (p = Glob.hints->first; p != NULL; p = p->next) {
      Topform h = p->c;
      if (h->compressed != NULL && !materialize_clause(h))
        fatal_error("index_and_process_initial_clauses: invalid packed hint");
      h->id = hint_id_number++;
      orient_equalities(h, TRUE);
      renumber_variables(h, MAX_VARS);
      index_hint(h);
    }
  }

  ////////////////////////////////////////////////////////////////////////////
  // Sos

  // Move Sos to a temporary list, then process that temporary list,
  // putting the clauses back into Sos in the "correct" way, either
  // by calling cl_process() or doing it here.

  temp_sos = Glob.sos;                    // move Sos to a temporary list
  name_clist(temp_sos, "temp_sos");       // not really necessary
  Glob.sos = clist_init("sos");           // get a new (empty) Sos list

  if (flag(Opt->process_initial_sos)) {

    int rp_interval = parm(Opt->report_preprocessing);
    int rp_total = temp_sos->length;
    int rp_count = 0;
    double rp_next = (rp_interval > 0) ? user_seconds() + rp_interval : -1;

    if (flag(Opt->print_initial_clauses))
      printf("\n");

    while (temp_sos->first) {
      Topform c = temp_sos->first->c;
      Topform new;
      clist_remove(c, temp_sos);

      new = copy_inference(c);  // c has no ID, so this is tricky
      cl_process_simplify(new);
      if (new->justification->next == NULL) {
	// No simplification occurred, so make it a clone of the parent.
	zap_just(new->justification);
	new->justification = copy_justification(c->justification);
	// Get all attributes, not just inheritable ones.
	zap_attributes(new->attributes);
	new->attributes = copy_attributes(c->attributes);
      }
      else {
	// Simplification occurs, so make it a child of the parent.
	if (c->id == 0)   /* see the guard note in the Usable loop */
	  assign_clause_id(c);
	new->justification->u.id = c->id;
	// Copy SInE depth attribute from parent (not inheritable).
	new->attributes = copy_int_attribute(c->attributes,
					     new->attributes,
					     sine_depth_attr());
	if (flag(Opt->print_initial_clauses)) {
	  printf("%s           ", TPTP_PFX);
	  fwrite_clause(stdout, c, CL_FORM_STD);
	}
      }
      retain_disabled_clause(c);
      cl_process(new);  // This re-simplifies, but that's ok.

      rp_count++;
      if (rp_next > 0 && (rp_count % 1000) == 0) {
	double now = user_seconds();
	if (now >= rp_next) {
	  fprintf(stderr,
		  "NOTE: preprocessing processed %d of %d sos clauses, "
		  "kept %llu (%.1f sec, %lld MB)\n",
		  rp_count, rp_total, Stats.kept, now, megs_malloced());
	  fflush(stderr);
	  rp_next = now + rp_interval;
	}
      }
    }
    // This will put processed clauses back into Sos.
    limbo_process(TRUE);  // back subsumption and back demodulation.

  }
  else {
    /* not processing initial sos */
    fflush(stdout);
    bell(stderr);
    fprintf(stderr,
	    "\nWARNING: clear(process_initial_sos) is not well tested.\n"
	    "We usually recommend against using it.\n");
    fflush(stderr);
    
    /* not applying full processing to initial sos */
    while (temp_sos->first) {
      Topform c = temp_sos->first->c;
      clist_remove(c, temp_sos);

      if (number_of_literals(c->literals) == 0)
	/* in case $F is in input, or if predicate elimination finds proof */
	handle_proof_and_maybe_exit(c);
      else {
	assign_clause_id(c);
	if (flag(Opt->dont_flip_input))
	  orient_equalities(c, FALSE);
	else
	  c = orient_input_eq(c);
	mark_maximal_literals(c->literals);
	mark_selected_literals(c->literals,
			       stringparm1(Opt->literal_selection));
	c->weight = clause_weight(c->literals);
	if (!clist_empty(Glob.hints)) {
	  clock_start(Clocks.hints);
	  adjust_weight_with_hints(c,
				   flag(Opt->degrade_hints),
				   flag(Opt->breadth_first_hints));
	  clock_stop(Clocks.hints);
	}

	c->initial = TRUE;
	if (discount_mode()) {
	  prepare_discount_passive(c, TRUE);
	  insert_into_sos2(c, Glob.sos);
	}
	else {
	  insert_into_sos2(c, Glob.sos);
	  index_literals(c, INSERT, Clocks.index, FALSE);
	  index_back_demod(c, INSERT, Clocks.index, flag(Opt->back_demod));
	}
      }
    }
  }

  clist_zap(temp_sos);  // free the temporary list

  /* Establish the epoch-1 historical active set only after preprocessing,
     because that phase can replace or disable input usable clauses. */
  if (collective_frontier_mode())
    for (p = Glob.usable->first; p != NULL; p = p->next)
      /* The initial indexing loop above includes restricted denials; later
         activations deliberately do not.  Record what was actually indexed,
         rather than recomputing eligibility during historical replay. */
      collective_note_activation(p->c, collective_hyper_enabled());

  {
    int rp_interval = parm(Opt->report_preprocessing);
    if (rp_interval > 0) {
      fprintf(stderr,
	      "NOTE: preprocessing writing initial clauses to output"
	      " (%.1f sec, %lld MB)\n",
	      user_seconds(), megs_malloced());
      fflush(stderr);
    }
  }

  ////////////////////////////////////////////////////////////////////////////
  // Print

  if (!flag(Opt->quiet)) {
    print_separator(stdout, "end of process initial clauses", TRUE);
    print_separator(stdout, "CLAUSES FOR SEARCH", TRUE);
  }

  if (flag(Opt->print_initial_clauses)) {
    printf("\n%% Clauses after input processing:\n");
    fwrite_clause_clist(stdout,Glob.usable,  CL_FORM_STD);
    fwrite_clause_clist(stdout,Glob.sos,     CL_FORM_STD);
    fwrite_demod_clist(stdout,Glob.demods,   CL_FORM_STD);
  }
  if (!flag(Opt->quiet) && Glob.hints->length > 0) {
      int redundant = redundant_hints();
      printf("\n%% %d hints (%d processed, %d redundant).\n",
	     Glob.hints->length - redundant, Glob.hints->length, redundant);
    }

  if (!flag(Opt->quiet))
    print_separator(stdout, "end of clauses for search", TRUE);

  {
    int rp_interval = parm(Opt->report_preprocessing);
    if (rp_interval > 0) {
      fprintf(stderr,
	      "NOTE: preprocessing finished, entering search"
	      " (%.1f sec, %lld MB)\n",
	      user_seconds(), megs_malloced());
      fflush(stderr);
    }
  }

}  // index_and_process_initial_clauses

/*************
 *
 *   fatal_setjmp()
 *
 *************/

static
void fatal_setjmp(void)
{
  int return_code = setjmp(Jump_env);
  if (return_code != 0)
    fatal_error("longjmp called outside of search");
}  /* fatal_setjmp */

/*************
 *
 *   collect_prover_results()
 *
 *************/

static
Prover_results collect_prover_results(BOOL xproofs)
{
  Plist p;
  Prover_results results = safe_calloc(1, sizeof(struct prover_results));

  for (p = Glob.empties; p; p = p->next) {
    Plist proof = get_clause_ancestors(p->v);
    Plist materialized;
    /* Results own a printable proof DAG, so materialization is intentional
       here and lasts until zap_prover_results(). */
    restore_archive_hint_links(proof);
    materialized = materialize_clauses(proof);
    zap_plist(materialized);
    results->proofs = plist_append(results->proofs, proof);
    if (xproofs) {
      Plist xproof = proof_to_xproof(proof);
      results->xproofs = plist_append(results->xproofs, xproof);
    }
  }
  if (!Terminal_stats_frozen)
    update_stats();  /* puts package stats into Stats */
  results->stats = Stats;  /* structure copy */
  results->user_seconds = user_seconds();
  results->system_seconds = system_seconds();
  results->return_code = Glob.return_code;
  return results;
}  /* collect_prover_results */

/*************
 *
 *   zap_prover_results()
 *
 *************/

/* DOCUMENTATION
Free the dynamically allocated memory associated with a Prover_result.
*/

/* PUBLIC */
void zap_prover_results(Prover_results results)
{
  Plist a, b;  /* results->proofs is a Plist of Plist of clauses */
  for (a = results->proofs; a; a = a->next) {
    for (b = a->v; b; b = b->next) {
      Topform c = b->v;
      /* There is a tricky thing going on with the ID.  If you try
	 to delete a clause with an ID not in the clause ID table,
	 a fatal error occurs.  If IDs in these clauses came from
	 a child process, they will not be in the table.  Setting
	 the ID to 0 gets around that problem.
       */
      c->id = 0;
      delete_clause(c);  /* zaps justification, attributes */
    }
  }
  safe_free(results);
}  /* zap_prover_results */

/*************
 *
 *   basic_clause_properties()
 *
 *************/

static
void basic_clause_properties(Clist sos, Clist usable)
{
  Plist sos_temp    = copy_clist_to_plist_shallow(sos);
  Plist usable_temp = copy_clist_to_plist_shallow(usable);

  Glob.equality = 
    pos_equality_in_clauses(sos_temp) || pos_equality_in_clauses(usable_temp);
    
  Glob.unit =
    all_clauses_unit(sos_temp) && all_clauses_unit(usable_temp);

  Glob.horn =
    all_clauses_horn(sos_temp) && all_clauses_horn(usable_temp);

  Glob.number_of_clauses =
    plist_count(sos_temp) + plist_count(usable_temp);

  Glob.number_of_neg_clauses =
    negative_clauses(sos_temp) + negative_clauses(usable_temp);

  zap_plist(sos_temp);
  zap_plist(usable_temp);
}  /* basic_clause_properties */

/*************
 *
 *   write_clist_bare()
 *
 *   Write clauses from a Clist in CL_FORM_BARE format.
 *   Also writes clause_data lines to data_fp.
 *
 *************/

static
BOOL write_bare_clause(FILE *clause_fp, FILE *data_fp, Topform c,
                       const char *list_name, int list_pos, int *file_pos,
                       Plist *seen_tab, int seen_tab_size)
{
  BOOL was_compressed = c->compressed != NULL;
  int is_shared;
  Literals lit;
  if (c->id == 0)
    return FALSE;  /* skip clauses without IDs */
  if (was_compressed && !materialize_clause(c))
    fatal_error("write_bare_clause: invalid compressed clause");

  is_shared = seen_tab != NULL && clause_plist_member(
    seen_tab[c->id % seen_tab_size], c, TRUE);
  if (!is_shared) {
    if (seen_tab != NULL)
      seen_tab[c->id % seen_tab_size] = insert_clause_into_plist(
        seen_tab[c->id % seen_tab_size], c, TRUE);
    fwrite_clause(clause_fp, c, CL_FORM_BARE);
  }
  fprintf(data_fp, "%s %d %llu %.17g %d", list_name,
          is_shared ? list_pos : *file_pos,
          c->id, c->weight, (int)c->initial);
  if (is_shared)
    fprintf(data_fp, " shared");
  if (c->matching_hint != NULL)
    fprintf(data_fp, " hint_match %d", (int)c->matching_hint->id);
  if (c->used)
    fprintf(data_fp, " used");
  if (c->was_given)
    fprintf(data_fp, " was_given");
  if (c->simplifier_epoch > 0)
    fprintf(data_fp, " simplifier_epoch %u", c->simplifier_epoch);
  if (c->rewrite_epoch > 0)
    fprintf(data_fp, " rewrite_epoch %u", c->rewrite_epoch);
  if (c->delayed_demodulator)
    fprintf(data_fp, " delayed_demodulator");
  if (c->rewrite_rule_dirty)
    fprintf(data_fp, " rewrite_rule_dirty");
  if (c->last_matched_given > 0)
    fprintf(data_fp, " last_matched %llu", c->last_matched_given);
  if (strcmp(list_name, "hints") == 0 && hint_is_redundant(c))
    fprintf(data_fp, " redundant_hint");
  for (lit = c->literals; lit != NULL; lit = lit->next)
    fprintf(data_fp, " aflags %u", (unsigned)lit->atom->private_flags);
  fprintf(data_fp, "\n");
  if (!is_shared)
    (*file_pos)++;
  if (was_compressed && !recompress_clause(c))
    fatal_error("write_bare_clause: could not recompress clause");
  return TRUE;
}

static
int write_clist_bare(FILE *clause_fp, FILE *data_fp,
                     Clist lst, const char *list_name,
                     Plist *seen_tab, int seen_tab_size)
{
  Clist_pos p;
  int file_pos = 0;
  int list_pos = 0;
  for (p = lst->first; p != NULL; p = p->next) {
    if (write_bare_clause(clause_fp, data_fp, p->c, list_name, list_pos,
                          &file_pos, seen_tab, seen_tab_size))
      list_pos++;
  }
  fprintf(clause_fp, "end_of_list.\n");
  return list_pos;
}  /* write_clist_bare */

static
int write_clause_store_bare(FILE *clause_fp, FILE *data_fp,
                            Clause_store store, const char *list_name)
{
  size_t i;
  int file_pos = 0;
  int list_pos = 0;
  for (i = 0; i < clause_store_length(store); i++) {
    unsigned long long id;
    if (!clause_store_position_is_current(store, i))
      continue;  /* superseded immutable version */
    id = clause_store_id(store, i);
    if (dense_passive_contains_id(id))
      continue;  /* serialized through sos.clauses */
    Topform c = clause_store_materialize(store, i);
    if (c == NULL)
      fatal_error("write_clause_store_bare: corrupt ancestor record");
    if (c->archive_materialized) {
      unsigned long long hint_id = clause_store_matching_hint_id(c->id);
      c->matching_hint = hint_by_id(hint_id);
    }
    if (write_bare_clause(clause_fp, data_fp, c,
                          list_name, list_pos, &file_pos, NULL, 0))
      list_pos++;
    clause_store_release_materialized(c);
  }
  fprintf(clause_fp, "end_of_list.\n");
  return list_pos;
}  /* write_clause_store_bare */

struct dense_bare_context {
  FILE *clause_fp;
  FILE *data_fp;
  const char *list_name;
  int file_pos;
  int list_pos;
  Plist *seen_tab;
  int seen_tab_size;
};

static void write_dense_bare_visit(const struct dense_passive_view *view,
                                   void *context)
{
  struct dense_bare_context *ctx = context;
  Topform c = materialize_checkpoint_passive(view);
  if (write_bare_clause(ctx->clause_fp, ctx->data_fp, c,
                        ctx->list_name, ctx->list_pos, &ctx->file_pos,
                        ctx->seen_tab, ctx->seen_tab_size))
    ctx->list_pos++;
  release_checkpoint_passive(c);
}

static int write_dense_bare(FILE *clause_fp, FILE *data_fp,
                            const char *list_name,
                            Plist *seen_tab, int seen_tab_size)
{
  struct dense_bare_context ctx;
  ctx.clause_fp = clause_fp;
  ctx.data_fp = data_fp;
  ctx.list_name = list_name;
  ctx.file_pos = 0;
  ctx.list_pos = 0;
  ctx.seen_tab = seen_tab;
  ctx.seen_tab_size = seen_tab_size;
  dense_passive_foreach(write_dense_bare_visit, &ctx);
  fprintf(clause_fp, "end_of_list.\n");
  return ctx.list_pos;
}  /* write_dense_bare */

/*************
 *
 *   Checkpoint verification hashes.
 *
 *   XOR-rotate hash of clause IDs in list order.  Two lists with the
 *   same clauses in the same order produce the same hash.  Used by
 *   set(checkpoint_verify) to detect restore divergence.
 *
 *************/

static
unsigned long long hash_clist_ids(Clist lst)
{
  unsigned long long h = 0;
  Clist_pos p;
  for (p = lst->first; p != NULL; p = p->next) {
    h ^= p->c->id;
    h = (h << 7) | (h >> 57);  /* rotate left 7 */
  }
  return h;
}

static
unsigned long long hash_clause_store_ids(Clause_store store)
{
  unsigned long long h = 0;
  size_t i;
  for (i = 0; i < clause_store_length(store); i++) {
    unsigned long long id = clause_store_id(store, i);
    /* Format 3 intentionally omits pre-elimination clauses without IDs. */
    if (clause_store_position_is_current(store, i) && id != 0 &&
        !dense_passive_contains_id(id)) {
      h ^= id;
      h = (h << 7) | (h >> 57);
    }
  }
  return h;
}

static
unsigned long long checkpointed_clause_store_count(Clause_store store)
{
  unsigned long long count = 0;
  size_t i;
  for (i = 0; i < clause_store_length(store); i++)
    if (clause_store_position_is_current(store, i) &&
        clause_store_id(store, i) != 0 &&
        !dense_passive_contains_id(clause_store_id(store, i)))
      count++;
  return count;
}

static
unsigned long long checkpoint_omitted_disabled_count(Clause_store store)
{
  unsigned long long physical = clause_store_current_length(store);
  unsigned long long passive = compact_otter_passive_mode() ?
    dense_passive_size() : 0;
  unsigned long long written = checkpointed_clause_store_count(store);
  if (physical < passive || physical - passive < written)
    fatal_error("checkpoint disabled-count accounting underflow");
  return Disabled_checkpoint_omitted + physical - passive - written;
}

struct dense_hash_context {
  unsigned long long hash;
};

static void hash_dense_ids_visit(const struct dense_passive_view *view,
                                 void *context)
{
  struct dense_hash_context *ctx = context;
  ctx->hash ^= view->id;
  ctx->hash = (ctx->hash << 7) | (ctx->hash >> 57);
}

static unsigned long long hash_dense_ids(void)
{
  struct dense_hash_context ctx;
  ctx.hash = 0;
  dense_passive_foreach(hash_dense_ids_visit, &ctx);
  return ctx.hash;
}

static void hash_dense_hints_visit(const struct dense_passive_view *view,
                                   void *context)
{
  struct dense_hash_context *ctx = context;
  ctx->hash ^= (view->id * 2654435761ULL) ^ view->hint_id;
  ctx->hash = (ctx->hash << 11) | (ctx->hash >> 53);
}

static unsigned long long hash_dense_hint_matches(void)
{
  struct dense_hash_context ctx;
  ctx.hash = 0;
  dense_passive_foreach(hash_dense_hints_visit, &ctx);
  return ctx.hash;
}

static
unsigned long long hash_clist_fpa_ids(Clist lst)
{
  unsigned long long h = 0;
  Clist_pos p;
  for (p = lst->first; p != NULL; p = p->next) {
    Literals lit;
    for (lit = p->c->literals; lit; lit = lit->next) {
      /* Walk atom DFS, hash FPA_IDs (skip variables -- shared objects) */
      Term stack[1000];
      int top = 0;
      stack[top++] = lit->atom;
      while (top > 0) {
        Term t = stack[--top];
        int i;
        /* Only hash non-zero FPA_IDs (root atoms that were FPA-indexed).
           Variables and subterms keep FPA_ID=0 and may change due to
           shared variable replacement during hint renumbering. */
        if (FPA_ID(t) != 0)
          h ^= (unsigned long long) FPA_ID(t);
        h = (h << 5) | (h >> 59);
        for (i = ARITY(t) - 1; i >= 0; i--)
          stack[top++] = ARG(t, i);
      }
    }
  }
  return h;
}

static
unsigned long long hash_clist_hint_matches(Clist lst)
{
  unsigned long long h = 0;
  Clist_pos p;
  for (p = lst->first; p != NULL; p = p->next) {
    unsigned long long hint_id = p->c->matching_hint
        ? p->c->matching_hint->id : 0;
    h ^= (p->c->id * 2654435761ULL) ^ hint_id;
    h = (h << 11) | (h >> 53);
  }
  return h;
}

static
void write_checkpoint_hashes(const char *dir)
{
  char path[600];
  FILE *fp;

  snprintf(path, sizeof(path), "%s/verify.txt", dir);
  fp = fopen(path, "w");
  if (!fp) return;

  fprintf(fp, "sos_ids %llu\n",       dense_passive_mode() ?
          hash_dense_ids() : hash_clist_ids(Glob.sos));
  fprintf(fp, "usable_ids %llu\n",    hash_clist_ids(Glob.usable));
  fprintf(fp, "demods_ids %llu\n",    hash_clist_ids(Glob.demods));
  fprintf(fp, "rewrite_only_ids %llu\n",
          rewrite_only_store_identity_hash(Rewrite_only_rules));
  fprintf(fp, "compact_rewrite_ids %llu\n",
          compact_rewrite_identity_hash(Compact_rewrite_rules));
  fprintf(fp, "hints_ids %llu\n",     hash_clist_ids(Glob.hints));
  fprintf(fp, "limbo_ids %llu\n",     hash_clist_ids(Glob.limbo));
  fprintf(fp, "disabled_ids %llu\n",  hash_clause_store_ids(Glob.disabled));
  fprintf(fp, "sos_fpa %llu\n",       dense_passive_mode() ? 0ULL :
          hash_clist_fpa_ids(Glob.sos));
  fprintf(fp, "usable_fpa %llu\n",    hash_clist_fpa_ids(Glob.usable));
  fprintf(fp, "hints_fpa %llu\n",     hash_clist_fpa_ids(Glob.hints));
  fprintf(fp, "limbo_fpa %llu\n",     hash_clist_fpa_ids(Glob.limbo));
  fprintf(fp, "sos_hints %llu\n",     dense_passive_mode() ?
          hash_dense_hint_matches() : hash_clist_hint_matches(Glob.sos));
  fprintf(fp, "usable_hints %llu\n",  hash_clist_hint_matches(Glob.usable));
  fprintf(fp, "sos_count %d\n",       dense_passive_mode() ?
          dense_passive_size() : Glob.sos->length);
  fprintf(fp, "usable_count %d\n",    Glob.usable->length);
  fprintf(fp, "demods_count %d\n",    Glob.demods->length);
  fprintf(fp, "rewrite_only_count %llu\n",
          rewrite_only_store_count(Rewrite_only_rules));
  {
    struct compact_rewrite_stats compact;
    compact_rewrite_get_stats(Compact_rewrite_rules, &compact);
    fprintf(fp, "compact_rewrite_count %llu\n", compact.rules_current);
  }
  fprintf(fp, "hints_count %d\n",     Glob.hints->length);
  fprintf(fp, "limbo_count %d\n",     Glob.limbo->length);
  fprintf(fp, "disabled_count %llu\n",
          checkpointed_clause_store_count(Glob.disabled));

  fclose(fp);
}

static
void verify_checkpoint_hashes(const char *dir)
{
  char path[600], key[64];
  unsigned long long val;
  FILE *fp;
  int pass = 0, fail = 0;

  snprintf(path, sizeof(path), "%s/verify.txt", dir);
  fp = fopen(path, "r");
  if (!fp) {
    printf("%% checkpoint_verify: no verify.txt in checkpoint.\n");
    return;
  }

  printf("\n%% Checkpoint verification:\n");
  while (fscanf(fp, " %63s %llu", key, &val) == 2) {
    unsigned long long actual = 0;
    const char *status;

    if (strcmp(key, "sos_ids") == 0)
      actual = dense_passive_mode() ? hash_dense_ids() :
               hash_clist_ids(Glob.sos);
    else if (strcmp(key, "usable_ids") == 0)
      actual = hash_clist_ids(Glob.usable);
    else if (strcmp(key, "demods_ids") == 0)
      actual = hash_clist_ids(Glob.demods);
    else if (strcmp(key, "rewrite_only_ids") == 0)
      actual = rewrite_only_store_identity_hash(Rewrite_only_rules);
    else if (strcmp(key, "compact_rewrite_ids") == 0)
      actual = compact_rewrite_identity_hash(Compact_rewrite_rules);
    else if (strcmp(key, "hints_ids") == 0)
      actual = hash_clist_ids(Glob.hints);
    else if (strcmp(key, "limbo_ids") == 0)
      actual = hash_clist_ids(Glob.limbo);
    else if (strcmp(key, "disabled_ids") == 0)
      actual = hash_clause_store_ids(Glob.disabled);
    else if (strcmp(key, "sos_fpa") == 0)
      actual = dense_passive_mode() ? 0ULL : hash_clist_fpa_ids(Glob.sos);
    else if (strcmp(key, "usable_fpa") == 0)
      actual = hash_clist_fpa_ids(Glob.usable);
    else if (strcmp(key, "hints_fpa") == 0)
      actual = hash_clist_fpa_ids(Glob.hints);
    else if (strcmp(key, "limbo_fpa") == 0)
      actual = hash_clist_fpa_ids(Glob.limbo);
    else if (strcmp(key, "sos_hints") == 0)
      actual = dense_passive_mode() ? hash_dense_hint_matches() :
               hash_clist_hint_matches(Glob.sos);
    else if (strcmp(key, "usable_hints") == 0)
      actual = hash_clist_hint_matches(Glob.usable);
    else if (strcmp(key, "sos_count") == 0)
      actual = (unsigned long long) (dense_passive_mode() ?
               dense_passive_size() : Glob.sos->length);
    else if (strcmp(key, "usable_count") == 0)
      actual = (unsigned long long) Glob.usable->length;
    else if (strcmp(key, "demods_count") == 0)
      actual = (unsigned long long) Glob.demods->length;
    else if (strcmp(key, "rewrite_only_count") == 0)
      actual = rewrite_only_store_count(Rewrite_only_rules);
    else if (strcmp(key, "compact_rewrite_count") == 0) {
      struct compact_rewrite_stats compact;
      compact_rewrite_get_stats(Compact_rewrite_rules, &compact);
      actual = compact.rules_current;
    }
    else if (strcmp(key, "hints_count") == 0)
      actual = (unsigned long long) Glob.hints->length;
    else if (strcmp(key, "limbo_count") == 0)
      actual = (unsigned long long) Glob.limbo->length;
    else if (strcmp(key, "disabled_count") == 0)
      actual = checkpointed_clause_store_count(Glob.disabled);
    else
      continue;

    status = (actual == val) ? "OK" : "MISMATCH";
    if (actual != val) {
      printf("%%   %s: %s (expected %llu, got %llu)\n",
             key, status, val, actual);
      fail++;
    }
    else {
      printf("%%   %s: %s\n", key, status);
      pass++;
    }
  }
  fclose(fp);

  printf("%%   Verification: %d passed, %d failed.\n", pass, fail);
  fflush(stdout);
  if (fail > 0) {
    fprintf(stderr, "FATAL: checkpoint verification failed (%d mismatches).\n"
            "The checkpoint data does not match the restored state.\n", fail);
    fatal_error("checkpoint_verify: verification failed");
  }
}

/*************
 *
 *   write_term_fpa_ids()
 *
 *   Write FPA_ID for a term and all subterms (pre-order DFS).
 *   Returns the number of IDs written.
 *
 *************/

static
int write_term_fpa_ids(FILE *fp, Term t)
{
  int i, count = 1;
  /* Skip variables: they are shared objects whose FPA_ID depends on
     indexing order, not on individual clause state.  Write 0 for vars. */
  fprintf(fp, " %u", VARIABLE(t) ? 0 : (unsigned) FPA_ID(t));
  for (i = 0; i < ARITY(t); i++)
    count += write_term_fpa_ids(fp, ARG(t, i));
  return count;
}  /* write_term_fpa_ids */

static void write_clause_fpa_ids(FILE *fp, Topform c)
{
  Literals lit;
  int term_count = 0;
  for (lit = c->literals; lit; lit = lit->next)
    term_count += symbol_count(lit->atom);
  fprintf(fp, "%d", term_count);
  for (lit = c->literals; lit; lit = lit->next)
    write_term_fpa_ids(fp, lit->atom);
  fprintf(fp, "\n");
}

struct dense_fpa_context {
  FILE *fp;
};

static void write_dense_fpa_visit(const struct dense_passive_view *view,
                                  void *context)
{
  struct dense_fpa_context *ctx = context;
  Topform c = materialize_checkpoint_passive(view);
  write_clause_fpa_ids(ctx->fp, c);
  release_checkpoint_passive(c);
}

/*************
 *
 *   write_fpa_ids()
 *
 *   Write FPA_IDs for all terms in the given clause lists to a file.
 *   Section-based format keyed by list position (not clause ID):
 *     fpa_id_count <N>
 *     LIST <name> <count>
 *     <term_count> <id1> <id2> ... <idN>
 *     ...
 *
 *************/

static
void write_fpa_ids(const char *dir)
{
  char path[600];
  FILE *fp;
  Clist_pos p;

  snprintf(path, sizeof(path), "%s/fpa_ids.txt", dir);
  fp = fopen(path, "w");
  if (!fp) {
    fprintf(stderr, "WARNING: cannot write %s\n", path);
    return;
  }

  fprintf(fp, "fpa_id_count %u\n", get_fpa_id_count());

  /* Save shared variable FPA_IDs.  Variables are shared across all
     clauses, so their FPA_IDs must be saved/restored separately. */
  {
    int i, nvar = 0;
    for (i = 0; i < MAX_VNUM; i++)
      if (get_variable_term_if_exists(i) != NULL)
        nvar = i + 1;
    fprintf(fp, "VARS %d\n", nvar);
    for (i = 0; i < nvar; i++) {
      Term v = get_variable_term_if_exists(i);
      fprintf(fp, " %u", v != NULL ? (unsigned) FPA_ID(v) : 0);
    }
    fprintf(fp, "\n");
  }

  /* Save FPA_IDs for all FPA-indexed clause lists:
     usable, sos, hints, limbo (demods use DISCRIM, not FPA).
     Section-based: reader iterates by list position, not clause ID. */
  {
    const char *names[] = {"usable", "sos", "hints", "limbo"};
    Clist lists[] = {Glob.usable, Glob.sos, Glob.hints, Glob.limbo};
    int nlist = 4, i;
    for (i = 0; i < nlist; i++) {
      int count = (i == 1 && dense_passive_mode()) ?
                  dense_passive_size() : lists[i]->length;
      fprintf(fp, "LIST %s %d\n", names[i], count);
      if (i == 1 && dense_passive_mode()) {
        struct dense_fpa_context ctx;
        ctx.fp = fp;
        dense_passive_foreach(write_dense_fpa_visit, &ctx);
        continue;
      }
      for (p = lists[i]->first; p != NULL; p = p->next) {
        write_clause_fpa_ids(fp, p->c);
      }
    }
  }

  fclose(fp);
}  /* write_fpa_ids */

/*************
 *
 *   read_term_fpa_ids()
 *
 *   Read FPA_IDs for a term and all subterms (pre-order DFS).
 *   Returns the number of IDs read.
 *
 *************/

static
int read_term_fpa_ids(FILE *fp, Term t)
{
  unsigned id;
  int i, count = 1;
  if (fscanf(fp, " %u", &id) != 1)
    fatal_error("restore_fpa_ids: unexpected end of data");
  /* Skip variables: they are shared objects.  Don't write to them. */
  if (!VARIABLE(t))
    FPA_ID(t) = id;
  for (i = 0; i < ARITY(t); i++)
    count += read_term_fpa_ids(fp, ARG(t, i));
  return count;
}  /* read_term_fpa_ids */

/*************
 *
 *   restore_fpa_ids()
 *
 *   Read FPA_IDs from checkpoint directory and assign them to terms.
 *   Section-based format: iterates by list position, not clause ID.
 *   Must be called after resume_load_clauses() but before resume_index_clauses().
 *
 *************/

static
void restore_fpa_ids(const char *dir)
{
  char path[600], buf[64], list_name[32];
  FILE *fp;
  unsigned saved_count;
  int section_count, restored = 0, skipped = 0;

  snprintf(path, sizeof(path), "%s/fpa_ids.txt", dir);
  fp = fopen(path, "r");
  if (!fp) {
    fprintf(stderr, "NOTE: no fpa_ids.txt in checkpoint; "
            "FPA ordering may differ.\n");
    return;
  }

  /* Read the fpa_id_count header */
  if (fscanf(fp, " %63s %u", buf, &saved_count) != 2 ||
      strcmp(buf, "fpa_id_count") != 0)
    fatal_error("restore_fpa_ids: bad header in fpa_ids.txt");
  set_fpa_id_count(saved_count);

  /* Read sections (VARS or LIST) */
  while (fscanf(fp, " %63s", buf) == 1) {
    Clist lst;
    Clist_pos cp;
    int i;

    /* Restore shared variable FPA_IDs */
    if (strcmp(buf, "VARS") == 0) {
      int nvar;
      if (fscanf(fp, " %d", &nvar) != 1) break;
      for (i = 0; i < nvar; i++) {
        unsigned id;
        if (fscanf(fp, " %u", &id) != 1) break;
        if (id != 0) {
          Term v = get_variable_term(i);
          FPA_ID(v) = id;
        }
      }
      continue;
    }

    if (strcmp(buf, "LIST") != 0)
      break;  /* not a section header -- old format or corruption */
    if (fscanf(fp, " %31s %d", list_name, &section_count) != 2)
      break;

    /* Match list name to Glob list */
    lst = NULL;
    if (strcmp(list_name, "usable") == 0) lst = Glob.usable;
    else if (strcmp(list_name, "sos") == 0) lst = Glob.sos;
    else if (strcmp(list_name, "hints") == 0) lst = Glob.hints;
    else if (strcmp(list_name, "limbo") == 0) lst = Glob.limbo;

    if (lst == NULL || lst->length != section_count) {
      /* List mismatch -- skip this section's data */
      for (i = 0; i < section_count; i++) {
        int term_count, j;
        unsigned dummy;
        if (fscanf(fp, " %d", &term_count) != 1) break;
        for (j = 0; j < term_count; j++)
          fscanf(fp, " %u", &dummy);
      }
      skipped += section_count;
      continue;
    }

    /* Walk list by position, read FPA IDs for each clause */
    cp = lst->first;
    for (i = 0; i < section_count && cp != NULL; i++, cp = cp->next) {
      Topform c = cp->c;
      Literals lit;
      int term_count, actual_count;

      if (fscanf(fp, " %d", &term_count) != 1) break;

      /* Verify DFS node count matches */
      actual_count = 0;
      for (lit = c->literals; lit; lit = lit->next)
        actual_count += symbol_count(lit->atom);

      if (actual_count != term_count) {
        /* Term structure mismatch -- skip this clause's FPA IDs */
        int j;
        unsigned dummy;
        for (j = 0; j < term_count; j++)
          fscanf(fp, " %u", &dummy);
        skipped++;
        continue;
      }

      for (lit = c->literals; lit; lit = lit->next)
        read_term_fpa_ids(fp, lit->atom);
      restored++;
    }
  }

  fclose(fp);

  if (restored > 0)
    printf("%% Restored FPA_IDs for %d clauses (fpa_id_count=%u).\n",
           restored, saved_count);
  if (skipped > 0)
    printf("%% WARNING: skipped FPA_IDs for %d clauses (list mismatch).\n",
           skipped);
  fflush(stdout);
}  /* restore_fpa_ids */

/*************
 *
 *   remove_checkpoint_dir()
 *
 *   Safely remove a checkpoint directory (contains only regular files).
 *
 *************/

static void remove_checkpoint_dir(const char *path)
{
  DIR *d = opendir(path);
  if (!d) return;
  struct dirent *ent;
  char filepath[1024];
  while ((ent = readdir(d)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;
    snprintf(filepath, sizeof(filepath), "%s/%s", path, ent->d_name);
    unlink(filepath);
  }
  closedir(d);
  rmdir(path);
}  /* remove_checkpoint_dir */

#ifndef PRIMITIVE_ENVIRONMENT

/*************
 *
 *   record_auto_checkpoint()
 *
 *   Track an automatic checkpoint directory name in a circular buffer
 *   and rotate (delete) the oldest when the buffer is full.
 *
 *************/

static void record_auto_checkpoint(const char *dirname)
{
  int keep = parm(Opt->checkpoint_keep);

  /* Initialize or resize circular buffer if needed */
  if (Auto_ckpt_dirs == NULL || Auto_ckpt_capacity < keep) {
    int new_cap = keep;
    char **new_buf = safe_calloc(new_cap, sizeof(char *));
    /* Copy existing entries if any */
    int i;
    for (i = 0; i < Auto_ckpt_count && i < new_cap; i++) {
      int idx = (Auto_ckpt_head + i) % Auto_ckpt_capacity;
      new_buf[i] = Auto_ckpt_dirs[idx];
    }
    safe_free(Auto_ckpt_dirs);
    Auto_ckpt_dirs = new_buf;
    Auto_ckpt_head = 0;
    Auto_ckpt_capacity = new_cap;
    if (Auto_ckpt_count > new_cap)
      Auto_ckpt_count = new_cap;
  }

  /* If buffer is full, delete the oldest checkpoint directory */
  if (Auto_ckpt_count == keep) {
    int oldest = Auto_ckpt_head;
    if (Auto_ckpt_dirs[oldest]) {
      fprintf(stderr, "  Removing old checkpoint: %s\n", Auto_ckpt_dirs[oldest]);
      remove_checkpoint_dir(Auto_ckpt_dirs[oldest]);
      safe_free(Auto_ckpt_dirs[oldest]);
      Auto_ckpt_dirs[oldest] = NULL;
    }
    Auto_ckpt_head = (Auto_ckpt_head + 1) % Auto_ckpt_capacity;
    Auto_ckpt_count--;
  }

  /* Add new entry */
  int slot = (Auto_ckpt_head + Auto_ckpt_count) % Auto_ckpt_capacity;
  Auto_ckpt_dirs[slot] = strdup(dirname);
  Auto_ckpt_count++;
}  /* record_auto_checkpoint */

/*************
 *
 *   write_checkpoint_formulas()
 *
 *   Write non-clause formulas (e.g., goal formulas) from the clause ID
 *   hash table to the checkpoint directory.  These are needed for proof
 *   reconstruction because denial clauses reference goal formula IDs
 *   via [deny(N)] justifications.
 *
 *   Format: one entry per formula:
 *     <id>\n<formula_term>.\n<justification>.\n
 *
 *************/

static
void write_checkpoint_formulas(const char *dir)
{
  char path[600];
  FILE *fp;
  Plist formulas, p;

  formulas = collect_formulas_from_id_tab();
  if (formulas == NULL)
    return;

  snprintf(path, sizeof(path), "%s/formulas.txt", dir);
  fp = fopen(path, "w");
  if (!fp) {
    fprintf(stderr, "WARNING: cannot write %s\n", path);
    zap_plist(formulas);
    return;
  }

  for (p = formulas; p; p = p->next) {
    Topform c = p->v;
    Term t = topform_to_term(c);
    String_buf sb;

    /* Write ID on its own line */
    fprintf(fp, "%llu\n", c->id);

    /* Write formula term (with attributes) terminated by '.' */
    sb = get_string_buf();
    sb_write_term(sb, t);
    sb_append(sb, ".");
    fprint_sb(fp, sb);
    fprintf(fp, "\n");
    zap_string_buf(sb);
    zap_term(t);

    /* Write justification terminated by '.' */
    sb = get_string_buf();
    sb_write_just(sb, c->justification, NULL);  /* appends "]." */
    fprint_sb(fp, sb);
    fprintf(fp, "\n");
    zap_string_buf(sb);

  }

  fclose(fp);
  zap_plist(formulas);
}  /* write_checkpoint_formulas */

/*************
 *
 *   restore_checkpoint_formulas()
 *
 *   Read non-clause formulas (goals) from checkpoint and register them
 *   in the clause ID hash table.  Must be called before restore_justifications().
 *
 *************/

static
void restore_checkpoint_formulas(const char *dir)
{
  char path[600];
  FILE *fp;
  unsigned long long id;
  int count = 0;

  snprintf(path, sizeof(path), "%s/formulas.txt", dir);
  fp = fopen(path, "r");
  if (!fp) {
    /* Old checkpoint without formulas.txt - justification restore will
       still work but proofs may show [assumption] for goal-derived clauses. */
    return;
  }

  while (fscanf(fp, " %llu", &id) == 1) {
    Term formula_t = read_term(fp, stderr);  /* reads formula up to '.' */
    Term just_t    = read_term(fp, stderr);  /* reads justification up to '.' */
    Topform c;

    if (formula_t == NULL || just_t == NULL) {
      if (formula_t) zap_term(formula_t);
      if (just_t)    zap_term(just_t);
      break;
    }

    c = term_to_topform(formula_t, TRUE);  /* is_formula = TRUE */
    zap_term(formula_t);

    c->id = id;
    c->justification = term_to_just(just_t);
    zap_term(just_t);

    register_clause_with_id(c);
    count++;
  }

  fclose(fp);

  printf("%% Restored %d formulas from checkpoint.\n", count);
  fflush(stdout);
}  /* restore_checkpoint_formulas */

/*************
 *
 *   write_clist_justifications()
 *
 *   Write justifications for all clauses in a Clist to a file.
 *   Format: "<clause_id> <justification_term>\n"
 *   e.g., "17 [hyper(1,a,2,a)].\n"
 *
 *************/

static
void write_clause_justification(FILE *fp, Topform c)
{
  BOOL was_packed = c->compressed != NULL && c->packed_justification;
  if (was_packed && !materialize_clause(c))
    fatal_error("write_clause_justification: invalid cold clause");
  if (c->id != 0 && c->justification != NULL) {
    String_buf sb = get_string_buf();
    sb_write_just(sb, c->justification, NULL);
    fprintf(fp, "%llu ", c->id);
    fprint_sb(fp, sb);
    fprintf(fp, "\n");
    zap_string_buf(sb);
  }
  if (was_packed && !recompress_clause(c))
    fatal_error("write_clause_justification: cannot restore cold clause");
}

static
void write_clist_justifications(FILE *fp, Clist lst)
{
  Clist_pos p;
  for (p = lst->first; p != NULL; p = p->next)
    write_clause_justification(fp, p->c);
}  /* write_clist_justifications */

static void write_dense_justification_visit(
  const struct dense_passive_view *view, void *context)
{
  FILE *fp = context;
  Topform c = materialize_checkpoint_passive(view);
  write_clause_justification(fp, c);
  release_checkpoint_passive(c);
}

static
void write_clause_store_justifications(FILE *fp, Clause_store store)
{
  size_t i;
  for (i = 0; i < clause_store_length(store); i++) {
    unsigned long long id;
    if (!clause_store_position_is_current(store, i))
      continue;
    id = clause_store_id(store, i);
    if (dense_passive_contains_id(id))
      continue;
    Topform c = clause_store_materialize(store, i);
    if (c == NULL)
      fatal_error("write_clause_store_justifications: corrupt ancestor record");
    write_clause_justification(fp, c);
    clause_store_release_materialized(c);
  }
}

/*************
 *
 *   write_justifications()
 *
 *   Write justifications for all checkpoint clause lists to a file.
 *   Called from write_checkpoint().
 *
 *************/

static
void write_justifications(const char *dir)
{
  char path[600];
  FILE *fp;

  snprintf(path, sizeof(path), "%s/justifications.txt", dir);
  fp = fopen(path, "w");
  if (!fp) {
    fprintf(stderr, "WARNING: cannot write %s\n", path);
    return;
  }

  if (dense_passive_mode())
    dense_passive_foreach(write_dense_justification_visit, fp);
  else
    write_clist_justifications(fp, Glob.sos);
  write_clist_justifications(fp, Glob.usable);
  write_clist_justifications(fp, Glob.demods);
  if (Glob.hints->length > 0)
    write_clist_justifications(fp, Glob.hints);
  if (Glob.limbo->length > 0)
    write_clist_justifications(fp, Glob.limbo);
  if (flag(Opt->checkpoint_ancestors) &&
      clause_store_length(Glob.disabled) > 0)
    write_clause_store_justifications(fp, Glob.disabled);

  fclose(fp);
}  /* write_justifications */

/*************
 *
 *   restore_justifications()
 *
 *   Read justifications from checkpoint directory and replace the
 *   input_just() placeholders assigned during clause loading.
 *   Must be called after resume_load_clauses().
 *
 *************/

static
void restore_justifications(const char *dir)
{
  char path[600];
  FILE *fp;
  unsigned long long id;
  int count = 0;

  snprintf(path, sizeof(path), "%s/justifications.txt", dir);
  fp = fopen(path, "r");
  if (!fp) {
    fprintf(stderr, "NOTE: no justifications.txt in checkpoint (old format); "
            "proof output will show [assumption] for checkpoint clauses.\n");
    return;
  }

  while (fscanf(fp, " %llu", &id) == 1) {
    Term t = read_term(fp, stderr);
    if (t == NULL)
      break;
    {
      Topform c = find_clause_by_id(id);
      if (c) {
        Just new_just = term_to_just(t);
        zap_term(t);
        zap_just(c->justification);
        c->justification = new_just;
        count++;
      }
      else {
        zap_term(t);
      }
    }
  }

  fclose(fp);

  printf("%% Restored justifications for %d clauses from checkpoint.\n", count);
  fflush(stdout);
}  /* restore_justifications */

/*************
 *
 *   write_checkpoint_input()
 *
 *   Write a synthetic LADR input file (saved_input.txt) into the checkpoint
 *   directory, capturing the current runtime options and configuration lists.
 *   This makes the checkpoint self-contained: resume works without the
 *   original input file.
 *
 *************/

static
void write_checkpoint_input(const char *dir)
{
  char path[600];
  FILE *fp;

  snprintf(path, sizeof(path), "%s/saved_input.txt", dir);
  fp = fopen(path, "w");
  if (!fp) return;

  fprintf(fp, "%% Saved input from checkpoint (auto-generated)\n");
  fprintf(fp, "%% Runtime state including auto-mode changes\n\n");

  /* Disable option dependencies during read-back.  The saved options
     represent the final runtime state (post-dependency-resolution), so
     re-triggering dependencies would produce incorrect cascading values
     (e.g. assign(max_minutes, -1) -> max_seconds = -60, fatal). */
  fprintf(fp, "set(ignore_option_dependencies).\n\n");

  /* Options - current runtime state (includes auto-mode changes).
     fwrite_options_input writes parseable set()/clear()/assign() commands
     without the banner line (which would confuse the LADR parser). */
  fwrite_options_input(fp);
  /* Auto-mode flags have already been resolved at checkpoint time.
     Prevent re-running auto_inference/auto_process on resume, which
     would make incorrect decisions with different (empty) clause lists. */
  fprintf(fp, "clear(auto_inference).\n");
  fprintf(fp, "clear(auto_process).\n");
  fprintf(fp, "\n");

  /* Configuration lists - only write non-empty ones.
     fwrite_term_list writes list(name). term. ... end_of_list. format. */
  if (Glob.weights)
    fwrite_term_list(fp, Glob.weights, "weights");
  if (Glob.resonators)
    fwrite_term_list(fp, Glob.resonators, "resonators");
  if (Glob.kbo_weights)
    fwrite_term_list(fp, Glob.kbo_weights, "kbo_weights");
  else if (stringparm(Opt->order, "kbo")) {
    /* KBO with auto-generated weights: reconstruct list(kbo_weights) from
       current symbol table so the resume process gets the exact weights
       instead of re-running auto_kbo_weights on empty clause lists. */
    Ilist fsyms = current_fsym_precedence();
    Ilist p;
    BOOL any_nondefault = FALSE;
    for (p = fsyms; p; p = p->next) {
      if (sn_to_kb_wt(p->i) != 1) { any_nondefault = TRUE; break; }
    }
    if (any_nondefault) {
      fprintf(fp, "\nlist(kbo_weights).\n");
      for (p = fsyms; p; p = p->next) {
        if (sn_to_kb_wt(p->i) != 1)
          fprintf(fp, "%s = %d.\n", sn_to_str(p->i), sn_to_kb_wt(p->i));
      }
      fprintf(fp, "end_of_list.\n");
    }
    zap_ilist(fsyms);
  }
  if (Glob.actions)
    fwrite_term_list(fp, Glob.actions, "actions");
  if (Glob.interps)
    fwrite_term_list(fp, Glob.interps, "interpretations");
  if (Glob.given_selection)
    fwrite_term_list(fp, Glob.given_selection, "given_selection");
  if (Glob.keep_rules)
    fwrite_term_list(fp, Glob.keep_rules, "keep");
  if (Glob.delete_rules)
    fwrite_term_list(fp, Glob.delete_rules, "delete");

  fclose(fp);
}  /* write_checkpoint_input */

static void write_collective_candidate_pool(const char *dir)
{
  char path[600];
  FILE *fp;
  size_t i;
  snprintf(path, sizeof(path), "%s/collective_candidates.txt", dir);
  fp = fopen(path, "w");
  if (fp == NULL)
    fatal_error("write_collective_candidate_pool: cannot create state file");
  fprintf(fp, "P9CPOOL3 %llu %llu %u %u\n",
          (unsigned long long) Collective_candidate_heap_count,
          Collective_candidate_insertion_ordinal,
          Collective_pool_expansions_since_commit,
          Collective_pool_commits_since_fair);
  for (i = 0; i < Collective_candidate_heap_count; i++) {
    struct collective_pool_entry *e = Collective_candidate_heap[i];
    Term t = topform_to_term(e->clause);
    String_buf sb = get_string_buf();
    fprintf(fp,
            "%u %u %u %llu %llu %llu %llu %llu %llu %llu %u %u %a\n",
            (unsigned) e->source, e->kind, e->selector_priority,
            e->selector_mask, e->given_id, e->raw_ordinal,
            e->insertion_ordinal, e->hint_id, e->fingerprint,
            e->hint_epoch, e->simplifier_epoch,
            e->discovery_promotion ? 1U : 0U, e->adjusted_weight);
    fwrite_term(fp, t);
    fprintf(fp, ".\n");
    sb_write_just(sb, e->clause->justification, NULL);
    fprint_sb(fp, sb);
    fprintf(fp, "\n");
    zap_string_buf(sb);
    zap_term(t);
  }
  if (fclose(fp) != 0)
    fatal_error("write_collective_candidate_pool: close failed");
}

static void read_collective_candidate_pool(const char *dir, BOOL discovery_format)
{
  char path[600], magic[16];
  FILE *fp;
  unsigned long long count, insertion, i;
  unsigned expansions, commits_since_fair;
  BOOL pool3;

  snprintf(path, sizeof(path), "%s/collective_candidates.txt", dir);
  fp = fopen(path, "r");
  if (fp == NULL)
    fatal_error("resume: balanced candidate-pool state is missing");
  if (fscanf(fp, " %15s %llu %llu %u %u", magic, &count, &insertion,
             &expansions, &commits_since_fair) != 5)
    fatal_error("resume: corrupt collective candidate-pool header");
  pool3 = strcmp(magic, "P9CPOOL3") == 0;
  if ((!pool3 && strcmp(magic, "P9CPOOL2") != 0) ||
      pool3 != discovery_format)
    fatal_error("resume: candidate-pool version does not match frontier");
  if (count > (unsigned long long) parm(Opt->collective_candidate_cache) ||
      expansions >
        (unsigned) parm(Opt->collective_candidate_commit_interval) ||
      commits_since_fair >=
        (unsigned) parm(Opt->collective_candidate_fair_interval))
    fatal_error("resume: invalid collective candidate-pool bounds");
  for (i = 0; i < count; i++) {
    struct collective_pool_entry *e = safe_calloc(1, sizeof(*e));
    unsigned source, kind, priority, simplifier_epoch, discovery_promotion;
    unsigned long long selector_mask, given_id, raw_ordinal, entry_insertion;
    unsigned long long hint_id, fingerprint, hint_epoch;
    double adjusted_weight;
    Term clause_term, just_term;
    if (pool3) {
      if (fscanf(fp,
                 " %u %u %u %llu %llu %llu %llu %llu %llu %llu %u %u %la",
                 &source, &kind, &priority, &selector_mask, &given_id,
                 &raw_ordinal, &entry_insertion, &hint_id, &fingerprint,
                 &hint_epoch, &simplifier_epoch, &discovery_promotion,
                 &adjusted_weight) != 13)
        fatal_error("resume: truncated collective candidate metadata");
    }
    else {
      if (fscanf(fp,
                 " %u %u %u %llu %llu %llu %llu %llu %llu %llu %u %la",
                 &source, &kind, &priority, &selector_mask, &given_id,
                 &raw_ordinal, &entry_insertion, &hint_id, &fingerprint,
                 &hint_epoch, &simplifier_epoch, &adjusted_weight) != 12)
        fatal_error("resume: truncated collective candidate metadata");
      discovery_promotion = 0;
    }
    clause_term = read_term(fp, stderr);
    just_term = read_term(fp, stderr);
    if (clause_term == NULL || just_term == NULL)
      fatal_error("resume: truncated collective candidate clause");
    e->clause = term_to_topform(clause_term, FALSE);
    zap_term(clause_term);
    e->clause->justification = term_to_just(just_term);
    zap_term(just_term);
    if (source > INFER_SOURCE_PARAMOD ||
        (kind != COLLECTIVE_PARAMOD_FROM &&
         kind != COLLECTIVE_PARAMOD_INTO &&
         kind != COLLECTIVE_POS_HYPER &&
         kind != COLLECTIVE_NEG_HYPER) ||
        priority > 2 || discovery_promotion > 1 ||
        !isfinite(adjusted_weight) ||
        entry_insertion >= insertion)
      fatal_error("resume: invalid collective candidate metadata");
    e->source = (enum inference_source) source;
    e->kind = kind;
    e->selector_priority = priority;
    e->selector_mask = selector_mask;
    e->given_id = given_id;
    e->raw_ordinal = raw_ordinal;
    e->insertion_ordinal = entry_insertion;
    e->hint_id = hint_id;
    e->fingerprint = fingerprint;
    e->hint_epoch = hint_epoch;
    e->simplifier_epoch = simplifier_epoch;
    e->discovery_promotion = discovery_promotion != 0;
    e->adjusted_weight = adjusted_weight;
    e->bytes = sizeof(*e) + sizeof(struct topform) +
               clause_body_storage_bytes(e->clause);
    Collective_candidate_pool_bytes += e->bytes;
    collective_candidate_heap_insert(e);
  }
  if (fscanf(fp, " %15s", magic) == 1)
    fatal_error("resume: trailing collective candidate-pool data");
  fclose(fp);
  Collective_candidate_insertion_ordinal = insertion;
  Collective_pool_expansions_since_commit = expansions;
  Collective_pool_commits_since_fair = commits_since_fair;
  if (collective_candidate_pool_allocated_bytes() >
        Stats.collective_candidate_pool_peak_bytes)
    Stats.collective_candidate_pool_peak_bytes =
      collective_candidate_pool_allocated_bytes();
}

static void write_collective_para_iterator(
  FILE *fp, const Para_iterator *pi, const char *error)
{
  uint32_t from_literal = pi->from_literal;
  uint32_t into_literal = pi->into_literal;
  uint32_t from_side = pi->from_side;
  uint32_t into_argument = pi->into_argument;
  uint32_t path_depth = pi->path_depth;
  uint32_t positioned = pi->positioned ? 1U : 0U;
  uint32_t complete = pi->complete ? 1U : 0U;
  if (fwrite(&from_literal, sizeof(from_literal), 1, fp) != 1 ||
      fwrite(&into_literal, sizeof(into_literal), 1, fp) != 1 ||
      fwrite(&from_side, sizeof(from_side), 1, fp) != 1 ||
      fwrite(&into_argument, sizeof(into_argument), 1, fp) != 1 ||
      fwrite(&path_depth, sizeof(path_depth), 1, fp) != 1 ||
      fwrite(&positioned, sizeof(positioned), 1, fp) != 1 ||
      fwrite(&complete, sizeof(complete), 1, fp) != 1 ||
      (path_depth != 0 &&
       fwrite(pi->path, sizeof(*pi->path), path_depth, fp) != path_depth))
    fatal_error((char *) error);
}

static void write_collective_hyper_iterator(
  FILE *fp, const Hyper_iterator *hi, const char *error)
{
  uint32_t initialized = hi->initialized;
  uint32_t satellite_mode = hi->satellite_mode;
  uint32_t complete = hi->complete;
  uint32_t given_literal = hi->given_literal;
  uint32_t given_phase = hi->given_phase;
  uint32_t outer_literal = hi->outer_literal;
  uint32_t nucleus_selected = hi->nucleus_selected;
  uint32_t nucleus_literal = hi->nucleus_literal;
  uint32_t depth = hi->depth;
  uint32_t mate_phase = hi->mate_phase;
  uint32_t mate_literal = hi->mate_literal;
  unsigned i;
  if (fwrite(&initialized, sizeof(initialized), 1, fp) != 1 ||
      fwrite(&satellite_mode, sizeof(satellite_mode), 1, fp) != 1 ||
      fwrite(&complete, sizeof(complete), 1, fp) != 1 ||
      fwrite(&given_literal, sizeof(given_literal), 1, fp) != 1 ||
      fwrite(&given_phase, sizeof(given_phase), 1, fp) != 1 ||
      fwrite(&outer_literal, sizeof(outer_literal), 1, fp) != 1 ||
      fwrite(&hi->outer_parent, sizeof(hi->outer_parent), 1, fp) != 1 ||
      fwrite(&nucleus_selected, sizeof(nucleus_selected), 1, fp) != 1 ||
      fwrite(&nucleus_literal, sizeof(nucleus_literal), 1, fp) != 1 ||
      fwrite(&hi->nucleus_parent, sizeof(hi->nucleus_parent), 1, fp) != 1 ||
      fwrite(&depth, sizeof(depth), 1, fp) != 1 ||
      fwrite(&mate_phase, sizeof(mate_phase), 1, fp) != 1 ||
      fwrite(&mate_literal, sizeof(mate_literal), 1, fp) != 1 ||
      fwrite(&hi->mate_parent, sizeof(hi->mate_parent), 1, fp) != 1)
    fatal_error((char *) error);
  for (i = 0; i < hi->depth; i++) {
    const Hyper_iterator_choice *choice = &hi->choices[i];
    uint32_t literal = choice->literal;
    uint32_t phase = choice->phase;
    uint32_t resume_literal = choice->resume_literal;
    uint32_t resume_phase = choice->resume_phase;
    if (fwrite(&choice->parent_position,
               sizeof(choice->parent_position), 1, fp) != 1 ||
        fwrite(&choice->resume_parent,
               sizeof(choice->resume_parent), 1, fp) != 1 ||
        fwrite(&literal, sizeof(literal), 1, fp) != 1 ||
        fwrite(&phase, sizeof(phase), 1, fp) != 1 ||
        fwrite(&resume_literal, sizeof(resume_literal), 1, fp) != 1 ||
        fwrite(&resume_phase, sizeof(resume_phase), 1, fp) != 1)
      fatal_error((char *) error);
  }
}

static void read_collective_para_iterator(
  FILE *fp, Para_iterator *pi, const char *error)
{
  uint32_t from_literal, into_literal, from_side, into_argument;
  uint32_t path_depth, positioned, complete;
  if (fread(&from_literal, sizeof(from_literal), 1, fp) != 1 ||
      fread(&into_literal, sizeof(into_literal), 1, fp) != 1 ||
      fread(&from_side, sizeof(from_side), 1, fp) != 1 ||
      fread(&into_argument, sizeof(into_argument), 1, fp) != 1 ||
      fread(&path_depth, sizeof(path_depth), 1, fp) != 1 ||
      fread(&positioned, sizeof(positioned), 1, fp) != 1 ||
      fread(&complete, sizeof(complete), 1, fp) != 1)
    fatal_error((char *) error);
  if (from_side > 1 || positioned > 1 || complete != 0 ||
      (!positioned && path_depth != 0))
    fatal_error("resume: invalid collective paramodulation iterator");
  pi->from_literal = from_literal;
  pi->into_literal = into_literal;
  pi->from_side = from_side;
  pi->into_argument = into_argument;
  pi->path_depth = path_depth;
  pi->positioned = positioned != 0;
  if (path_depth != 0) {
    pi->path = safe_calloc(path_depth, sizeof(*pi->path));
    pi->path_capacity = path_depth;
    if (fread(pi->path, sizeof(*pi->path), path_depth, fp) != path_depth)
      fatal_error((char *) error);
  }
}

static void read_collective_hyper_iterator(
  FILE *fp, Hyper_iterator *hi, unsigned long long activation_limit,
  const char *error)
{
  uint32_t initialized, satellite_mode, complete;
  uint32_t given_literal, given_phase, outer_literal;
  uint32_t nucleus_selected, nucleus_literal, depth;
  uint32_t mate_phase, mate_literal;
  unsigned i;
  if (fread(&initialized, sizeof(initialized), 1, fp) != 1 ||
      fread(&satellite_mode, sizeof(satellite_mode), 1, fp) != 1 ||
      fread(&complete, sizeof(complete), 1, fp) != 1 ||
      fread(&given_literal, sizeof(given_literal), 1, fp) != 1 ||
      fread(&given_phase, sizeof(given_phase), 1, fp) != 1 ||
      fread(&outer_literal, sizeof(outer_literal), 1, fp) != 1 ||
      fread(&hi->outer_parent, sizeof(hi->outer_parent), 1, fp) != 1 ||
      fread(&nucleus_selected, sizeof(nucleus_selected), 1, fp) != 1 ||
      fread(&nucleus_literal, sizeof(nucleus_literal), 1, fp) != 1 ||
      fread(&hi->nucleus_parent, sizeof(hi->nucleus_parent), 1, fp) != 1 ||
      fread(&depth, sizeof(depth), 1, fp) != 1 ||
      fread(&mate_phase, sizeof(mate_phase), 1, fp) != 1 ||
      fread(&mate_literal, sizeof(mate_literal), 1, fp) != 1 ||
      fread(&hi->mate_parent, sizeof(hi->mate_parent), 1, fp) != 1)
    fatal_error((char *) error);
  if (initialized > 1 || satellite_mode > 1 || complete != 0 ||
      given_phase > 1 || nucleus_selected > 1 || mate_phase > 3 ||
      hi->outer_parent > activation_limit ||
      hi->mate_parent > activation_limit ||
      (nucleus_selected && hi->nucleus_parent >= activation_limit))
    fatal_error("resume: invalid collective hyperresolution iterator");
  hi->initialized = initialized;
  hi->satellite_mode = satellite_mode;
  hi->given_literal = given_literal;
  hi->given_phase = given_phase;
  hi->outer_literal = outer_literal;
  hi->nucleus_selected = nucleus_selected;
  hi->nucleus_literal = nucleus_literal;
  hi->depth = depth;
  hi->mate_phase = mate_phase;
  hi->mate_literal = mate_literal;
  if (depth != 0) {
    hi->choices = safe_calloc(depth, sizeof(*hi->choices));
    hi->choice_capacity = depth;
  }
  for (i = 0; i < depth; i++) {
    Hyper_iterator_choice *choice = &hi->choices[i];
    uint32_t literal, phase, resume_literal, resume_phase;
    if (fread(&choice->parent_position,
              sizeof(choice->parent_position), 1, fp) != 1 ||
        fread(&choice->resume_parent,
              sizeof(choice->resume_parent), 1, fp) != 1 ||
        fread(&literal, sizeof(literal), 1, fp) != 1 ||
        fread(&phase, sizeof(phase), 1, fp) != 1 ||
        fread(&resume_literal, sizeof(resume_literal), 1, fp) != 1 ||
        fread(&resume_phase, sizeof(resume_phase), 1, fp) != 1)
      fatal_error((char *) error);
    if (phase > 2 || resume_phase > 3 ||
        (phase <= 1 && choice->parent_position >= activation_limit) ||
        choice->resume_parent > activation_limit)
      fatal_error("resume: invalid collective hyper iterator choice");
    choice->literal = literal;
    choice->phase = phase;
    choice->resume_literal = resume_literal;
    choice->resume_phase = resume_phase;
  }
}

static
void write_collective_checkpoint(const char *dir)
{
  char path[600];
  FILE *fp;
  unsigned long long i;
  struct collective_batch *b;
  char magic[8] = {'P','9','C','O','L','L','A','\0'};
  uint32_t turn = Collective_batch_turn ? 1U : 0U;
  uint32_t givens_since_batch = Collective_givens_since_batch;
  uint32_t hint_probe_credit = Collective_hint_probe_credit ? 1U : 0U;
  uint32_t priority_turns_since_fair =
    flag(Opt->collective_promising_scheduler) ?
      Collective_priority_turns_since_fair : 0U;
  uint32_t balanced_lane = Collective_balanced_lane;
  uint32_t balanced_lane_credit = Collective_balanced_lane_credit;
  uint32_t balanced_turns_since_oldest =
    Collective_balanced_turns_since_oldest;
  uint32_t drain_mode = Collective_drain_mode ? 1U : 0U;
  uint32_t discovery_turns_since_fair =
    Collective_discovery_turns_since_fair;
  uint32_t discovery_turns_since_general =
    Collective_discovery_turns_since_general;

  if (!collective_frontier_mode())
    return;
  if (collective_balanced_mode())
    magic[6] = 'F';
  snprintf(path, sizeof(path), "%s/collective_frontier.bin", dir);
  fp = fopen(path, "wb");
  if (fp == NULL)
    fatal_error("write_collective_checkpoint: cannot create state file");
  if (fwrite(magic, sizeof(magic), 1, fp) != 1 ||
      fwrite(&Collective_activation_count,
             sizeof(Collective_activation_count), 1, fp) != 1 ||
      fwrite(&Collective_batch_count, sizeof(Collective_batch_count), 1, fp) != 1 ||
      fwrite(&turn, sizeof(turn), 1, fp) != 1 ||
      fwrite(&givens_since_batch, sizeof(givens_since_batch), 1, fp) != 1 ||
      fwrite(&hint_probe_credit, sizeof(hint_probe_credit), 1, fp) != 1 ||
      fwrite(&priority_turns_since_fair,
             sizeof(priority_turns_since_fair), 1, fp) != 1)
    fatal_error("write_collective_checkpoint: header write failed");
  if (collective_balanced_mode() &&
      (fwrite(&balanced_lane, sizeof(balanced_lane), 1, fp) != 1 ||
       fwrite(&balanced_lane_credit,
              sizeof(balanced_lane_credit), 1, fp) != 1 ||
       fwrite(&balanced_turns_since_oldest,
              sizeof(balanced_turns_since_oldest), 1, fp) != 1 ||
       fwrite(&drain_mode, sizeof(drain_mode), 1, fp) != 1))
    fatal_error("write_collective_checkpoint: balanced state write failed");
  if (collective_balanced_mode() &&
      (fwrite(&discovery_turns_since_fair,
              sizeof(discovery_turns_since_fair), 1, fp) != 1 ||
       fwrite(&discovery_turns_since_general,
              sizeof(discovery_turns_since_general), 1, fp) != 1))
    fatal_error("write_collective_checkpoint: discovery state write failed");

  for (i = 0; i < Collective_activation_count; i++) {
    unsigned long long id = collective_activation_id(i);
    uint32_t deactivated = collective_deactivation_epoch(id);
    uint32_t clashable = collective_activation_clashable(i) ? 1U : 0U;
    if (fwrite(&id, sizeof(id), 1, fp) != 1 ||
        fwrite(&deactivated, sizeof(deactivated), 1, fp) != 1 ||
        fwrite(&clashable, sizeof(clashable), 1, fp) != 1)
      fatal_error("write_collective_checkpoint: history write failed");
  }
  for (b = Collective_batch_head; b != NULL; b = b->next) {
    uint32_t epoch = b->snapshot_epoch;
    uint32_t kind = b->kind;
    if (fwrite(&b->given_id, sizeof(b->given_id), 1, fp) != 1 ||
        fwrite(&b->cursor, sizeof(b->cursor), 1, fp) != 1 ||
        fwrite(&b->activation_limit, sizeof(b->activation_limit), 1, fp) != 1 ||
        fwrite(&b->conclusion_cursor,
               sizeof(b->conclusion_cursor), 1, fp) != 1 ||
        fwrite(&b->conclusion_prefix_hash,
               sizeof(b->conclusion_prefix_hash), 1, fp) != 1 ||
        fwrite(&b->conclusion_order_weight,
               sizeof(b->conclusion_order_weight), 1, fp) != 1 ||
        fwrite(&b->conclusion_order_ordinal,
               sizeof(b->conclusion_order_ordinal), 1, fp) != 1 ||
        fwrite(&b->conclusion_next_weight,
               sizeof(b->conclusion_next_weight), 1, fp) != 1 ||
        fwrite(&b->conclusion_next_ordinal,
               sizeof(b->conclusion_next_ordinal), 1, fp) != 1 ||
        (collective_balanced_mode() &&
         fwrite(&b->candidate_ordinal,
                sizeof(b->candidate_ordinal), 1, fp) != 1) ||
        fwrite(&epoch, sizeof(epoch), 1, fp) != 1 ||
        fwrite(&kind, sizeof(kind), 1, fp) != 1)
      fatal_error("write_collective_checkpoint: descriptor write failed");
    if (collective_balanced_mode()) {
      uint32_t from_literal = b->para_iterator.from_literal;
      uint32_t into_literal = b->para_iterator.into_literal;
      uint32_t from_side = b->para_iterator.from_side;
      uint32_t into_argument = b->para_iterator.into_argument;
      uint32_t path_depth = b->para_iterator.path_depth;
      uint32_t positioned = b->para_iterator.positioned ? 1U : 0U;
      uint32_t complete = b->para_iterator.complete ? 1U : 0U;
      if (fwrite(&from_literal, sizeof(from_literal), 1, fp) != 1 ||
          fwrite(&into_literal, sizeof(into_literal), 1, fp) != 1 ||
          fwrite(&from_side, sizeof(from_side), 1, fp) != 1 ||
          fwrite(&into_argument, sizeof(into_argument), 1, fp) != 1 ||
          fwrite(&path_depth, sizeof(path_depth), 1, fp) != 1 ||
          fwrite(&positioned, sizeof(positioned), 1, fp) != 1 ||
          fwrite(&complete, sizeof(complete), 1, fp) != 1 ||
          (path_depth != 0 &&
           fwrite(b->para_iterator.path, sizeof(*b->para_iterator.path),
                  path_depth, fp) != path_depth))
        fatal_error("write_collective_checkpoint: iterator write failed");
      {
        Hyper_iterator *hi = &b->hyper_iterator;
        uint32_t initialized = hi->initialized;
        uint32_t satellite_mode = hi->satellite_mode;
        uint32_t complete_hyper = hi->complete;
        uint32_t given_literal = hi->given_literal;
        uint32_t given_phase = hi->given_phase;
        uint32_t outer_literal = hi->outer_literal;
        uint32_t nucleus_selected = hi->nucleus_selected;
        uint32_t nucleus_literal = hi->nucleus_literal;
        uint32_t depth = hi->depth;
        uint32_t mate_phase = hi->mate_phase;
        uint32_t mate_literal = hi->mate_literal;
        unsigned i;
        if (fwrite(&initialized, sizeof(initialized), 1, fp) != 1 ||
            fwrite(&satellite_mode, sizeof(satellite_mode), 1, fp) != 1 ||
            fwrite(&complete_hyper, sizeof(complete_hyper), 1, fp) != 1 ||
            fwrite(&given_literal, sizeof(given_literal), 1, fp) != 1 ||
            fwrite(&given_phase, sizeof(given_phase), 1, fp) != 1 ||
            fwrite(&outer_literal, sizeof(outer_literal), 1, fp) != 1 ||
            fwrite(&hi->outer_parent, sizeof(hi->outer_parent), 1, fp) != 1 ||
            fwrite(&nucleus_selected,
                   sizeof(nucleus_selected), 1, fp) != 1 ||
            fwrite(&nucleus_literal, sizeof(nucleus_literal), 1, fp) != 1 ||
            fwrite(&hi->nucleus_parent,
                   sizeof(hi->nucleus_parent), 1, fp) != 1 ||
            fwrite(&depth, sizeof(depth), 1, fp) != 1 ||
            fwrite(&mate_phase, sizeof(mate_phase), 1, fp) != 1 ||
            fwrite(&mate_literal, sizeof(mate_literal), 1, fp) != 1 ||
            fwrite(&hi->mate_parent, sizeof(hi->mate_parent), 1, fp) != 1)
          fatal_error("write_collective_checkpoint: hyper iterator write failed");
        for (i = 0; i < hi->depth; i++) {
          Hyper_iterator_choice *choice = &hi->choices[i];
          uint32_t literal = choice->literal;
          uint32_t phase = choice->phase;
          uint32_t resume_literal = choice->resume_literal;
          uint32_t resume_phase = choice->resume_phase;
          if (fwrite(&choice->parent_position,
                     sizeof(choice->parent_position), 1, fp) != 1 ||
              fwrite(&choice->resume_parent,
                     sizeof(choice->resume_parent), 1, fp) != 1 ||
              fwrite(&literal, sizeof(literal), 1, fp) != 1 ||
              fwrite(&phase, sizeof(phase), 1, fp) != 1 ||
              fwrite(&resume_literal,
                     sizeof(resume_literal), 1, fp) != 1 ||
              fwrite(&resume_phase, sizeof(resume_phase), 1, fp) != 1)
            fatal_error("write_collective_checkpoint: hyper choice write failed");
        }
      }
      {
        uint32_t initialized = b->discovery_initialized ? 1U : 0U;
        uint32_t complete = b->discovery_complete ? 1U : 0U;
        uint32_t hot = b->discovery_hot ? 1U : 0U;
        uint32_t consumed = b->consumed_count;
        unsigned i;
        if (fwrite(&b->discovery_cursor,
                   sizeof(b->discovery_cursor), 1, fp) != 1 ||
            fwrite(&b->discovery_ordinal,
                   sizeof(b->discovery_ordinal), 1, fp) != 1 ||
            fwrite(&initialized, sizeof(initialized), 1, fp) != 1 ||
            fwrite(&complete, sizeof(complete), 1, fp) != 1 ||
            fwrite(&hot, sizeof(hot), 1, fp) != 1 ||
            fwrite(&consumed, sizeof(consumed), 1, fp) != 1)
          fatal_error("write_collective_checkpoint: discovery descriptor failed");
        write_collective_para_iterator(
          fp, &b->discovery_para_iterator,
          "write_collective_checkpoint: discovery para iterator failed");
        write_collective_hyper_iterator(
          fp, &b->discovery_hyper_iterator,
          "write_collective_checkpoint: discovery hyper iterator failed");
        for (i = 0; i < b->consumed_count; i++)
          if (fwrite(&b->consumed[i], sizeof(b->consumed[i]), 1, fp) != 1)
            fatal_error("write_collective_checkpoint: consumed ordinal failed");
      }
    }
  }
  if (fclose(fp) != 0)
    fatal_error("write_collective_checkpoint: close failed");
  if (collective_balanced_mode())
    write_collective_candidate_pool(dir);
}

static
void read_collective_checkpoint(const char *dir)
{
  char path[600], magic[8];
  FILE *fp;
  unsigned long long activations, batches, i;
  uint32_t turn, givens_since_batch, hint_probe_credit;
  uint32_t priority_turns_since_fair;
  uint32_t balanced_lane = 0, balanced_lane_credit = 0;
  uint32_t balanced_turns_since_oldest = 0, drain_mode = 0;
  uint32_t discovery_turns_since_fair = 0;
  uint32_t discovery_turns_since_general = 0;
  BOOL format6, format7, format8, format9, format_a, format_b, format_c,
       format_d, format_e, format_f;

  if (!collective_frontier_mode())
    return;
  snprintf(path, sizeof(path), "%s/collective_frontier.bin", dir);
  fp = fopen(path, "rb");
  if (fp == NULL)
    fatal_error("resume: collective frontier state is missing");
  if (fread(magic, sizeof(magic), 1, fp) != 1)
    fatal_error("resume: corrupt collective frontier header");
  format_f = memcmp(magic, "P9COLLF", 7) == 0;
  format_e = memcmp(magic, "P9COLLE", 7) == 0;
  format_d = memcmp(magic, "P9COLLD", 7) == 0;
  format_c = memcmp(magic, "P9COLLC", 7) == 0;
  format_b = memcmp(magic, "P9COLLB", 7) == 0;
  format_a = memcmp(magic, "P9COLLA", 7) == 0;
  format9 = memcmp(magic, "P9COLL9", 7) == 0;
  format8 = memcmp(magic, "P9COLL8", 7) == 0;
  format7 = memcmp(magic, "P9COLL7", 7) == 0;
  format6 = memcmp(magic, "P9COLL6", 7) == 0;
  if ((!format_f && !format_e && !format_d && !format_c && !format_b && !format_a && !format9 && !format8 &&
       !format7 && !format6 &&
       memcmp(magic, "P9COLL5", 7) != 0) ||
      fread(&activations, sizeof(activations), 1, fp) != 1 ||
      fread(&batches, sizeof(batches), 1, fp) != 1 ||
      fread(&turn, sizeof(turn), 1, fp) != 1 ||
      fread(&givens_since_batch, sizeof(givens_since_batch), 1, fp) != 1)
    fatal_error("resume: corrupt collective frontier header");
  if (format6 || format7 || format8 || format9 || format_a || format_b ||
      format_e || format_f ||
      format_c || format_d) {
    if (fread(&hint_probe_credit, sizeof(hint_probe_credit), 1, fp) != 1)
      fatal_error("resume: corrupt collective frontier probe state");
  }
  else
    hint_probe_credit = 1U;
  if (format_a || format_b || format_c || format_d || format_e || format_f) {
    if (fread(&priority_turns_since_fair,
              sizeof(priority_turns_since_fair), 1, fp) != 1)
      fatal_error("resume: corrupt collective priority scheduler state");
  }
  else
    priority_turns_since_fair = 0;
  if (format_b || format_c || format_d || format_e || format_f) {
    if (fread(&balanced_lane, sizeof(balanced_lane), 1, fp) != 1 ||
        fread(&balanced_lane_credit,
              sizeof(balanced_lane_credit), 1, fp) != 1 ||
        fread(&balanced_turns_since_oldest,
              sizeof(balanced_turns_since_oldest), 1, fp) != 1 ||
        fread(&drain_mode, sizeof(drain_mode), 1, fp) != 1)
      fatal_error("resume: corrupt balanced collective scheduler state");
  }
  if (format_f &&
      (fread(&discovery_turns_since_fair,
             sizeof(discovery_turns_since_fair), 1, fp) != 1 ||
       fread(&discovery_turns_since_general,
             sizeof(discovery_turns_since_general), 1, fp) != 1))
    fatal_error("resume: corrupt collective discovery scheduler state");
  if ((format_b || format_c || format_d || format_e || format_f) !=
        collective_balanced_mode())
    fatal_error("resume: collective scheduler policy does not match checkpoint");
  if (turn > 1 || hint_probe_credit > 1)
    fatal_error("resume: invalid collective scheduler state");
  if (priority_turns_since_fair >=
        (unsigned) parm(Opt->collective_promising_fair_interval) ||
      (!flag(Opt->collective_promising_scheduler) &&
       priority_turns_since_fair != 0))
    fatal_error("resume: invalid collective priority scheduler state");
  if ((format_b || format_c || format_d || format_e || format_f) &&
      (balanced_lane >= COLLECTIVE_LANE_COUNT ||
       balanced_lane_credit >
         collective_balanced_lane_share(balanced_lane) ||
       balanced_turns_since_oldest >=
         (unsigned) parm(Opt->collective_balanced_fair_interval) ||
       drain_mode > 1))
    fatal_error("resume: invalid balanced collective scheduler state");
  if (format_f &&
      (discovery_turns_since_fair >=
         (unsigned) parm(Opt->collective_discovery_turn_interval) ||
       discovery_turns_since_general >=
         (unsigned) parm(Opt->collective_discovery_general_interval)))
    fatal_error("resume: invalid collective discovery scheduler state");

  for (i = 0; i < activations; i++) {
    unsigned long long id;
    uint32_t deactivated, clashable;
    if (fread(&id, sizeof(id), 1, fp) != 1 ||
        fread(&deactivated, sizeof(deactivated), 1, fp) != 1 ||
        fread(&clashable, sizeof(clashable), 1, fp) != 1)
      fatal_error("resume: truncated collective activation history");
    if (clashable > 1)
      fatal_error("resume: invalid collective clashable flag");
    collective_append_activation_id(id, clashable != 0);
    collective_set_deactivation_epoch(id, deactivated);
  }
  for (i = 0; i < batches; i++) {
    struct collective_batch *b = collective_new_batch();
    uint32_t epoch, kind;
    if (fread(&b->given_id, sizeof(b->given_id), 1, fp) != 1 ||
        fread(&b->cursor, sizeof(b->cursor), 1, fp) != 1 ||
        fread(&b->activation_limit, sizeof(b->activation_limit), 1, fp) != 1)
      fatal_error("resume: truncated collective batch queue");
    if (format7 || format8 || format9 || format_a || format_b || format_c ||
        format_e || format_f ||
        format_d) {
      if (fread(&b->conclusion_cursor,
                sizeof(b->conclusion_cursor), 1, fp) != 1 ||
          fread(&b->conclusion_prefix_hash,
                sizeof(b->conclusion_prefix_hash), 1, fp) != 1)
        fatal_error("resume: truncated collective conclusion cursor");
    }
    else {
      b->conclusion_cursor = 0;
      b->conclusion_prefix_hash = 0;
    }
    if (format9 || format_a || format_b || format_c || format_d || format_e || format_f) {
      if (fread(&b->conclusion_order_weight,
                sizeof(b->conclusion_order_weight), 1, fp) != 1 ||
          fread(&b->conclusion_order_ordinal,
                sizeof(b->conclusion_order_ordinal), 1, fp) != 1 ||
          fread(&b->conclusion_next_weight,
                sizeof(b->conclusion_next_weight), 1, fp) != 1 ||
          fread(&b->conclusion_next_ordinal,
                sizeof(b->conclusion_next_ordinal), 1, fp) != 1)
        fatal_error("resume: truncated collective promising cursor");
    }
    if ((format_e || format_f) &&
        fread(&b->candidate_ordinal,
              sizeof(b->candidate_ordinal), 1, fp) != 1)
      fatal_error("resume: truncated collective candidate ordinal");
    if (fread(&epoch, sizeof(epoch), 1, fp) != 1 ||
        fread(&kind, sizeof(kind), 1, fp) != 1)
      fatal_error("resume: truncated collective batch queue");
    if (b->cursor > b->activation_limit ||
        b->activation_limit > Collective_activation_count)
      fatal_error("resume: invalid collective batch cursor");
    b->snapshot_epoch = epoch;
    if ((kind & COLLECTIVE_INFERENCE_MASK) == 0 ||
        (kind & ~COLLECTIVE_KIND_MASK) != 0 ||
        (!format_f && !format_e && !format_d && !format_c && !format_b && !format_a && !format9 &&
         !format8 &&
         !format7 && !format6 &&
         (kind & COLLECTIVE_HINT_PROBE) != 0) ||
        (!format_f && !format_e && !format_d && !format_c && !format_b && !format_a && !format9 &&
         (kind & COLLECTIVE_PROMISING_CURSOR) != 0) ||
        (format7 && b->conclusion_cursor != 0 &&
         (kind & (COLLECTIVE_POS_HYPER | COLLECTIVE_NEG_HYPER)) == 0) ||
        (b->conclusion_cursor == 0 && b->conclusion_prefix_hash != 0) ||
        (b->conclusion_cursor == 0 &&
         ((kind & COLLECTIVE_PROMISING_CURSOR) != 0 ||
          b->conclusion_order_weight != 0 ||
          b->conclusion_order_ordinal != 0 ||
          b->conclusion_next_weight != 0 ||
          b->conclusion_next_ordinal != 0)) ||
        (b->conclusion_cursor != 0 &&
         (kind & COLLECTIVE_PROMISING_CURSOR) == 0 &&
         (b->conclusion_order_weight != 0 ||
          b->conclusion_order_ordinal != 0 ||
          b->conclusion_next_weight != 0 ||
          b->conclusion_next_ordinal != 0)) ||
        ((kind & COLLECTIVE_PROMISING_CURSOR) != 0 &&
         (!isfinite(b->conclusion_order_weight) ||
          !isfinite(b->conclusion_next_weight) ||
          collective_candidate_key_compare(
            b->conclusion_order_weight, b->conclusion_order_ordinal,
            b->conclusion_next_weight, b->conclusion_next_ordinal) >= 0)) ||
        ((kind & COLLECTIVE_HINT_PROBE) != 0 &&
         (i != 0 || hint_probe_credit != 0)) ||
        ((format_b || format_c || format_d || format_e || format_f) &&
         kind != COLLECTIVE_PARAMOD_FROM &&
         kind != COLLECTIVE_PARAMOD_INTO &&
         kind != COLLECTIVE_POS_HYPER &&
         kind != COLLECTIVE_NEG_HYPER) ||
        (!format_b && !format_c && !format_d && !format_e && !format_f &&
         (kind & (COLLECTIVE_PARAMOD_FROM | COLLECTIVE_PARAMOD_INTO)) != 0))
      fatal_error("resume: invalid collective batch kind");
    b->kind = kind;
    if (format_c || format_d || format_e || format_f) {
      uint32_t from_literal, into_literal, from_side, into_argument;
      uint32_t path_depth, positioned, complete;
      if (fread(&from_literal, sizeof(from_literal), 1, fp) != 1 ||
          fread(&into_literal, sizeof(into_literal), 1, fp) != 1 ||
          fread(&from_side, sizeof(from_side), 1, fp) != 1 ||
          fread(&into_argument, sizeof(into_argument), 1, fp) != 1 ||
          fread(&path_depth, sizeof(path_depth), 1, fp) != 1 ||
          fread(&positioned, sizeof(positioned), 1, fp) != 1 ||
          fread(&complete, sizeof(complete), 1, fp) != 1)
        fatal_error("resume: truncated paramodulation iterator");
      if (from_side > 1 || positioned > 1 || complete != 0 ||
          (!positioned && path_depth != 0) ||
          ((kind == COLLECTIVE_POS_HYPER ||
            kind == COLLECTIVE_NEG_HYPER) &&
           (from_literal != 0 || into_literal != 0 || from_side != 0 ||
            into_argument != 0 || path_depth != 0 || positioned != 0)))
        fatal_error("resume: invalid paramodulation iterator");
      b->para_iterator.from_literal = from_literal;
      b->para_iterator.into_literal = into_literal;
      b->para_iterator.from_side = from_side;
      b->para_iterator.into_argument = into_argument;
      b->para_iterator.path_depth = path_depth;
      b->para_iterator.positioned = positioned != 0;
      if (path_depth != 0) {
        b->para_iterator.path =
          safe_calloc(path_depth, sizeof(*b->para_iterator.path));
        b->para_iterator.path_capacity = path_depth;
        if (fread(b->para_iterator.path,
                  sizeof(*b->para_iterator.path), path_depth, fp) != path_depth)
          fatal_error("resume: truncated paramodulation iterator path");
      }
    }
    if (format_d || format_e || format_f) {
      Hyper_iterator *hi = &b->hyper_iterator;
      uint32_t initialized, satellite_mode, complete_hyper;
      uint32_t given_literal, given_phase, outer_literal;
      uint32_t nucleus_selected, nucleus_literal, depth;
      uint32_t mate_phase, mate_literal;
      unsigned j;
      if (fread(&initialized, sizeof(initialized), 1, fp) != 1 ||
          fread(&satellite_mode, sizeof(satellite_mode), 1, fp) != 1 ||
          fread(&complete_hyper, sizeof(complete_hyper), 1, fp) != 1 ||
          fread(&given_literal, sizeof(given_literal), 1, fp) != 1 ||
          fread(&given_phase, sizeof(given_phase), 1, fp) != 1 ||
          fread(&outer_literal, sizeof(outer_literal), 1, fp) != 1 ||
          fread(&hi->outer_parent, sizeof(hi->outer_parent), 1, fp) != 1 ||
          fread(&nucleus_selected,
                sizeof(nucleus_selected), 1, fp) != 1 ||
          fread(&nucleus_literal, sizeof(nucleus_literal), 1, fp) != 1 ||
          fread(&hi->nucleus_parent,
                sizeof(hi->nucleus_parent), 1, fp) != 1 ||
          fread(&depth, sizeof(depth), 1, fp) != 1 ||
          fread(&mate_phase, sizeof(mate_phase), 1, fp) != 1 ||
          fread(&mate_literal, sizeof(mate_literal), 1, fp) != 1 ||
          fread(&hi->mate_parent, sizeof(hi->mate_parent), 1, fp) != 1)
        fatal_error("resume: truncated hyperresolution iterator");
      if (initialized > 1 || satellite_mode > 1 || complete_hyper != 0 ||
          given_phase > 1 || nucleus_selected > 1 || mate_phase > 3 ||
          hi->outer_parent > b->activation_limit ||
          hi->mate_parent > b->activation_limit ||
          (nucleus_selected &&
           hi->nucleus_parent >= b->activation_limit) ||
          ((kind == COLLECTIVE_PARAMOD_FROM ||
            kind == COLLECTIVE_PARAMOD_INTO) &&
           (initialized != 0 || satellite_mode != 0 || given_literal != 0 ||
            given_phase != 0 || outer_literal != 0 ||
            hi->outer_parent != 0 || nucleus_selected != 0 ||
            nucleus_literal != 0 || hi->nucleus_parent != 0 || depth != 0 ||
            mate_phase != 0 || mate_literal != 0 || hi->mate_parent != 0)))
        fatal_error("resume: invalid hyperresolution iterator");
      hi->initialized = initialized;
      hi->satellite_mode = satellite_mode;
      hi->given_literal = given_literal;
      hi->given_phase = given_phase;
      hi->outer_literal = outer_literal;
      hi->nucleus_selected = nucleus_selected;
      hi->nucleus_literal = nucleus_literal;
      hi->depth = depth;
      hi->mate_phase = mate_phase;
      hi->mate_literal = mate_literal;
      if (depth != 0) {
        hi->choices = safe_calloc(depth, sizeof(*hi->choices));
        hi->choice_capacity = depth;
      }
      for (j = 0; j < depth; j++) {
        Hyper_iterator_choice *choice = &hi->choices[j];
        uint32_t literal, phase, resume_literal, resume_phase;
        if (fread(&choice->parent_position,
                  sizeof(choice->parent_position), 1, fp) != 1 ||
            fread(&choice->resume_parent,
                  sizeof(choice->resume_parent), 1, fp) != 1 ||
            fread(&literal, sizeof(literal), 1, fp) != 1 ||
            fread(&phase, sizeof(phase), 1, fp) != 1 ||
            fread(&resume_literal, sizeof(resume_literal), 1, fp) != 1 ||
            fread(&resume_phase, sizeof(resume_phase), 1, fp) != 1)
          fatal_error("resume: truncated hyperresolution iterator choice");
        if (phase > 2 || resume_phase > 3 ||
            (phase <= 1 &&
             choice->parent_position >= b->activation_limit) ||
            choice->resume_parent > b->activation_limit)
          fatal_error("resume: invalid hyperresolution iterator choice");
        choice->literal = literal;
        choice->phase = phase;
        choice->resume_literal = resume_literal;
        choice->resume_phase = resume_phase;
      }
    }
    if (format_f) {
      uint32_t initialized, complete, hot, consumed;
      unsigned j;
      if (fread(&b->discovery_cursor,
                sizeof(b->discovery_cursor), 1, fp) != 1 ||
          fread(&b->discovery_ordinal,
                sizeof(b->discovery_ordinal), 1, fp) != 1 ||
          fread(&initialized, sizeof(initialized), 1, fp) != 1 ||
          fread(&complete, sizeof(complete), 1, fp) != 1 ||
          fread(&hot, sizeof(hot), 1, fp) != 1 ||
          fread(&consumed, sizeof(consumed), 1, fp) != 1)
        fatal_error("resume: truncated collective discovery descriptor");
      if (initialized > 1 || complete > 1 || hot > 1 ||
          b->discovery_cursor > b->activation_limit ||
          consumed >
            (unsigned) parm(Opt->collective_discovery_promotion_cap))
        fatal_error("resume: invalid collective discovery descriptor");
      b->discovery_initialized = initialized != 0;
      b->discovery_complete = complete != 0;
      b->discovery_hot = hot != 0;
      read_collective_para_iterator(
        fp, &b->discovery_para_iterator,
        "resume: truncated discovery paramodulation iterator");
      read_collective_hyper_iterator(
        fp, &b->discovery_hyper_iterator, b->activation_limit,
        "resume: truncated discovery hyperresolution iterator");
      if ((kind == COLLECTIVE_POS_HYPER ||
           kind == COLLECTIVE_NEG_HYPER) &&
          !para_iterator_at_start(&b->discovery_para_iterator))
        fatal_error("resume: hyper discovery has paramodulation state");
      if ((kind == COLLECTIVE_PARAMOD_FROM ||
           kind == COLLECTIVE_PARAMOD_INTO) &&
          (b->discovery_hyper_iterator.initialized ||
           b->discovery_hyper_iterator.depth != 0))
        fatal_error("resume: paramodulation discovery has hyper state");
      if (!b->discovery_initialized &&
          (consumed != 0 || b->discovery_cursor != 0 ||
           b->discovery_ordinal != 0 ||
           !para_iterator_at_start(&b->discovery_para_iterator) ||
           b->discovery_hyper_iterator.initialized))
        fatal_error("resume: uninitialized discovery has cursor state");
      if (consumed != 0) {
        b->consumed = safe_calloc(consumed, sizeof(*b->consumed));
        b->consumed_capacity = consumed;
      }
      for (j = 0; j < consumed; j++) {
        if (fread(&b->consumed[j], sizeof(b->consumed[j]), 1, fp) != 1)
          fatal_error("resume: truncated collective consumed ordinal");
        if (b->consumed[j].ordinal < b->candidate_ordinal ||
            b->consumed[j].ordinal >= b->discovery_ordinal ||
            (j != 0 && b->consumed[j-1].ordinal >=
                         b->consumed[j].ordinal))
          fatal_error("resume: invalid collective consumed ordinal");
      }
      b->consumed_count = consumed;
    }
    collective_append_batch(b);
  }
  Collective_batch_turn = turn != 0;
  Collective_givens_since_batch = givens_since_batch;
  Collective_hint_probe_credit = hint_probe_credit != 0;
  Collective_priority_turns_since_fair = priority_turns_since_fair;
  Collective_balanced_lane = balanced_lane;
  Collective_balanced_lane_credit = balanced_lane_credit;
  Collective_balanced_turns_since_oldest = balanced_turns_since_oldest;
  Collective_drain_mode = drain_mode != 0;
  Collective_discovery_turns_since_fair = discovery_turns_since_fair;
  Collective_discovery_turns_since_general = discovery_turns_since_general;
  if (fgetc(fp) != EOF)
    fatal_error("resume: trailing data in collective frontier state");
  fclose(fp);
  if (format_e || format_f)
    read_collective_candidate_pool(dir, format_f);
}

/*************
 *
 *   write_checkpoint()
 *
 *   Write the current search state to a checkpoint directory.
 *
 *************/

static
void write_checkpoint(void)
{
  char tmpdir[520], finaldir[512];
  FILE *fp;
  int n;

  /* Compact rewrite attempts happen in the hot demodulation callback, so
     synchronize its private counters before serializing Stats. */
  if (eager_interreduced_demod_mode())
    update_rewrite_only_stats();
  if (!clause_store_sync(Glob.disabled))
    fatal_error("write_checkpoint: cannot synchronize ancestor store");
  if (!cold_passive_store_sync(Dense_body_store))
    fatal_error("write_checkpoint: cannot synchronize passive store");

  /* Build directory names */
  n = snprintf(finaldir, sizeof(finaldir),
               "prover9_%d_ckpt_%llu", getpid(), Stats.given);
  if (n < 0 || n >= (int)sizeof(finaldir))
    fatal_error("write_checkpoint: directory name too long");

  snprintf(tmpdir, sizeof(tmpdir), "%s.tmp", finaldir);

  /* Remove stale temp dir if it exists, then create */
  rmdir(tmpdir);  /* ignore errors */
  if (mkdir(tmpdir, 0755) != 0) {
    fprintf(stderr, "ERROR: cannot create checkpoint directory %s: %s\n",
            tmpdir, strerror(errno));
    return;
  }

  /* 1. Write metadata.txt */
  {
    char path[600];
    snprintf(path, sizeof(path), "%s/metadata.txt", tmpdir);
    fp = fopen(path, "w");
    if (!fp) { fprintf(stderr, "ERROR: cannot write %s\n", path); return; }

    fprintf(fp, "checkpoint_format 3\n");
    fprintf(fp, "version %s\n", PROGRAM_VERSION);
    fprintf(fp, "date %s\n", PROGRAM_DATE);
    fprintf(fp, "max_clause_id %llu\n", clause_ids_assigned());
    fprintf(fp, "given %llu\n", Stats.given);
    fprintf(fp, "generated %llu\n", Stats.generated);
    fprintf(fp, "generated_binary %llu\n", Stats.generated_binary);
    fprintf(fp, "generated_hyper %llu\n", Stats.generated_hyper);
    fprintf(fp, "generated_ur %llu\n", Stats.generated_ur);
    fprintf(fp, "generated_paramod %llu\n", Stats.generated_paramod);
    fprintf(fp, "generated_other %llu\n", Stats.generated_other);
    fprintf(fp, "kept %llu\n", Stats.kept);
    fprintf(fp, "proofs %llu\n", Stats.proofs);
    fprintf(fp, "disabled_checkpoint_omitted %llu\n",
            checkpoint_omitted_disabled_count(Glob.disabled));
    fprintf(fp, "back_subsumed %llu\n", Stats.back_subsumed);
    fprintf(fp, "anc_subsume_blocked %llu\n", Stats.anc_subsume_blocked);
    fprintf(fp, "back_demodulated %llu\n", Stats.back_demodulated);
    fprintf(fp, "subsumed %llu\n", Stats.subsumed);
    fprintf(fp, "sos_limit_deleted %llu\n", Stats.sos_limit_deleted);
    fprintf(fp, "new_demodulators %llu\n", Stats.new_demodulators);
    fprintf(fp, "new_lex_demods %llu\n", Stats.new_lex_demods);
    fprintf(fp, "back_unit_deleted %llu\n", Stats.back_unit_deleted);
    fprintf(fp, "demod_attempts %llu\n", Stats.demod_attempts);
    fprintf(fp, "demod_rewrites %llu\n", Stats.demod_rewrites);
    fprintf(fp, "res_instance_prunes %llu\n", Stats.res_instance_prunes);
    fprintf(fp, "para_instance_prunes %llu\n", Stats.para_instance_prunes);
    fprintf(fp, "basic_para_prunes %llu\n", Stats.basic_para_prunes);
    fprintf(fp, "nonunit_fsub %llu\n", Stats.nonunit_fsub);
    fprintf(fp, "nonunit_bsub %llu\n", Stats.nonunit_bsub);
    fprintf(fp, "new_constants %llu\n", Stats.new_constants);
    fprintf(fp, "kept_by_rule %llu\n", Stats.kept_by_rule);
    fprintf(fp, "deleted_by_rule %llu\n", Stats.deleted_by_rule);
    fprintf(fp, "sos_displaced %llu\n", Stats.sos_displaced);
    fprintf(fp, "sos_removed %llu\n", Stats.sos_removed);
    fprintf(fp, "passive_demodulator_candidates %llu\n",
            Stats.passive_demodulator_candidates);
    fprintf(fp, "passive_oriented_demodulator_candidates %llu\n",
            Stats.passive_oriented_demodulator_candidates);
    fprintf(fp, "passive_lex_demodulator_candidates %llu\n",
            Stats.passive_lex_demodulator_candidates);
    fprintf(fp, "rewrite_only_demodulators_admitted %llu\n",
            Stats.rewrite_only_demodulators_admitted);
    fprintf(fp, "rewrite_only_demodulators_retired %llu\n",
            Stats.rewrite_only_demodulators_retired);
    fprintf(fp, "rewrite_only_demodulators_selected %llu\n",
            Stats.rewrite_only_demodulators_selected);
    fprintf(fp, "rewrite_only_demodulators_current %llu\n",
            Stats.rewrite_only_demodulators_current);
    fprintf(fp, "rewrite_only_demodulators_peak %llu\n",
            Stats.rewrite_only_demodulators_peak);
    fprintf(fp, "rewrite_bank_bytes %llu\n", Stats.rewrite_bank_bytes);
    fprintf(fp, "rewrite_bank_peak_bytes %llu\n",
            Stats.rewrite_bank_peak_bytes);
    fprintf(fp, "compact_rewrite_rules_current %llu\n",
            Stats.compact_rewrite_rules_current);
    fprintf(fp, "compact_rewrite_rules_peak %llu\n",
            Stats.compact_rewrite_rules_peak);
    fprintf(fp, "compact_rewrite_rules_retired %llu\n",
            Stats.compact_rewrite_rules_retired);
    fprintf(fp, "compact_rewrite_attempts %llu\n",
            Stats.compact_rewrite_attempts);
    fprintf(fp, "compact_rewrite_rewrites %llu\n",
            Stats.compact_rewrite_rewrites);
    fprintf(fp, "compact_rewrite_compactions %llu\n",
            Stats.compact_rewrite_compactions);
    fprintf(fp, "compact_rewrite_bytes_reclaimed %llu\n",
            Stats.compact_rewrite_bytes_reclaimed);
    fprintf(fp, "rewrite_refresh_scanned %llu\n",
            Stats.rewrite_refresh_scanned);
    fprintf(fp, "rewrite_refresh_materialized %llu\n",
            Stats.rewrite_refresh_materialized);
    fprintf(fp, "rewrite_refresh_rewritten %llu\n",
            Stats.rewrite_refresh_rewritten);
    fprintf(fp, "rewrite_refresh_unchanged %llu\n",
            Stats.rewrite_refresh_unchanged);
    fprintf(fp, "rewrite_refresh_subsumed %llu\n",
            Stats.rewrite_refresh_subsumed);
    fprintf(fp, "rewrite_refresh_hot_turns %llu\n",
            Stats.rewrite_refresh_hot_turns);
    fprintf(fp, "rewrite_refresh_general_turns %llu\n",
            Stats.rewrite_refresh_general_turns);
    fprintf(fp, "rewrite_interreduce_turns %llu\n",
            Stats.rewrite_interreduce_turns);
    fprintf(fp, "rewrite_interreduce_changed %llu\n",
            Stats.rewrite_interreduce_changed);
    fprintf(fp, "rewrite_interreduce_unchanged %llu\n",
            Stats.rewrite_interreduce_unchanged);
    fprintf(fp, "rewrite_interreduce_collapsed %llu\n",
            Stats.rewrite_interreduce_collapsed);
    fprintf(fp, "rewrite_overlap_visits %llu\n",
            Stats.rewrite_overlap_visits);
    fprintf(fp, "rewrite_overlap_dirty_marks %llu\n",
            Stats.rewrite_overlap_dirty_marks);
    fprintf(fp, "rewrite_cascade_suppressed %llu\n",
            Stats.rewrite_cascade_suppressed);
    fprintf(fp, "rewrite_debt_peak %llu\n", Stats.rewrite_debt_peak);
    fprintf(fp, "rewrite_drain_entries %llu\n", Stats.rewrite_drain_entries);
    fprintf(fp, "rewrite_drain_exits %llu\n", Stats.rewrite_drain_exits);
    fprintf(fp, "rewrite_drain_turns %llu\n", Stats.rewrite_drain_turns);
    fprintf(fp, "rewrite_drain_yields %llu\n", Stats.rewrite_drain_yields);
    fprintf(fp, "rewrite_inference_turns %llu\n",
            Stats.rewrite_inference_turns);
    fprintf(fp, "rewrite_refresh_stale_peak %llu\n",
            Stats.rewrite_refresh_stale_peak);
    fprintf(fp, "rewrite_refresh_lag_max %llu\n",
            Stats.rewrite_refresh_lag_max);
    fprintf(fp, "passive_refresh_checks %llu\n",
            Stats.passive_refresh_checks);
    fprintf(fp, "passive_refresh_requeued %llu\n",
            Stats.passive_refresh_requeued);
    fprintf(fp, "passive_refresh_subsumed %llu\n",
            Stats.passive_refresh_subsumed);
    fprintf(fp, "collective_batches_created %llu\n",
            Stats.collective_batches_created);
    fprintf(fp, "collective_batches_completed %llu\n",
            Stats.collective_batches_completed);
    fprintf(fp, "collective_pair_expansions %llu\n",
            Stats.collective_pair_expansions);
    fprintf(fp, "collective_pair_turns %llu\n",
            Stats.collective_pair_turns);
    fprintf(fp, "collective_hyper_expansions %llu\n",
            Stats.collective_hyper_expansions);
    fprintf(fp, "collective_hyper_sets_completed %llu\n",
            Stats.collective_hyper_sets_completed);
    fprintf(fp, "collective_candidates_emitted %llu\n",
            Stats.collective_candidates_emitted);
    fprintf(fp, "collective_candidates_replayed %llu\n",
            Stats.collective_candidates_replayed);
    fprintf(fp, "collective_deferred_turns %llu\n",
            Stats.collective_deferred_turns);
    fprintf(fp, "collective_raw_candidates_peak %llu\n",
            Stats.collective_raw_candidates_peak);
    fprintf(fp, "collective_raw_candidates_seen %llu\n",
            Stats.collective_raw_candidates_seen);
    fprintf(fp, "collective_candidate_cache_peak %llu\n",
            Stats.collective_candidate_cache_peak);
    fprintf(fp, "collective_candidate_cache_stalls %llu\n",
            Stats.collective_candidate_cache_stalls);
    fprintf(fp, "collective_promising_scans %llu\n",
            Stats.collective_promising_scans);
    fprintf(fp, "collective_promising_considered %llu\n",
            Stats.collective_promising_considered);
    fprintf(fp, "collective_promising_buffer_peak %llu\n",
            Stats.collective_promising_buffer_peak);
    fprintf(fp, "collective_promising_priority_turns %llu\n",
            Stats.collective_promising_priority_turns);
    fprintf(fp, "collective_promising_fair_turns %llu\n",
            Stats.collective_promising_fair_turns);
    fprintf(fp, "collective_promising_heap_peak %llu\n",
            Stats.collective_promising_heap_peak);
    fprintf(fp, "collective_partners_skipped %llu\n",
            Stats.collective_partners_skipped);
    fprintf(fp, "collective_parent_materializations %llu\n",
            Stats.collective_parent_materializations);
    fprintf(fp, "collective_snapshot_rebuilds %llu\n",
            Stats.collective_snapshot_rebuilds);
    fprintf(fp, "collective_snapshot_clauses %llu\n",
            Stats.collective_snapshot_clauses);
    fprintf(fp, "collective_snapshot_clauses_peak %llu\n",
            Stats.collective_snapshot_clauses_peak);
    fprintf(fp, "collective_history_queries %llu\n",
            Stats.collective_history_queries);
    fprintf(fp, "collective_history_candidates %llu\n",
            Stats.collective_history_candidates);
    fprintf(fp, "collective_history_rejected_future %llu\n",
            Stats.collective_history_rejected_future);
    fprintf(fp, "collective_history_rejected_inactive %llu\n",
            Stats.collective_history_rejected_inactive);
    fprintf(fp, "collective_hint_probes_scheduled %llu\n",
            Stats.collective_hint_probes_scheduled);
    fprintf(fp, "collective_hint_probes_expanded %llu\n",
            Stats.collective_hint_probes_expanded);
    fprintf(fp, "collective_hint_selected_total %llu\n",
            Stats.collective_hint_selected_total);
    fprintf(fp, "collective_hint_selected_hha %llu\n",
            Stats.collective_hint_selected_hha);
    fprintf(fp, "collective_hint_selected_hw %llu\n",
            Stats.collective_hint_selected_hw);
    fprintf(fp, "collective_hint_selected_lh %llu\n",
            Stats.collective_hint_selected_lh);
    fprintf(fp, "collective_hint_selected_other %llu\n",
            Stats.collective_hint_selected_other);
    fprintf(fp, "collective_batches_peak %llu\n",
            Stats.collective_batches_peak);
    fprintf(fp, "collective_created_paramod %llu\n",
            Stats.collective_created_paramod);
    fprintf(fp, "collective_created_pos_hyper %llu\n",
            Stats.collective_created_pos_hyper);
    fprintf(fp, "collective_created_neg_hyper %llu\n",
            Stats.collective_created_neg_hyper);
    fprintf(fp, "collective_completed_paramod %llu\n",
            Stats.collective_completed_paramod);
    fprintf(fp, "collective_completed_pos_hyper %llu\n",
            Stats.collective_completed_pos_hyper);
    fprintf(fp, "collective_completed_neg_hyper %llu\n",
            Stats.collective_completed_neg_hyper);
    fprintf(fp, "collective_balanced_paramod_turns %llu\n",
            Stats.collective_balanced_paramod_turns);
    fprintf(fp, "collective_balanced_pos_hyper_turns %llu\n",
            Stats.collective_balanced_pos_hyper_turns);
    fprintf(fp, "collective_balanced_neg_hyper_turns %llu\n",
            Stats.collective_balanced_neg_hyper_turns);
    fprintf(fp, "collective_balanced_oldest_turns %llu\n",
            Stats.collective_balanced_oldest_turns);
    fprintf(fp, "collective_balanced_lane_turns %llu\n",
            Stats.collective_balanced_lane_turns);
    fprintf(fp, "collective_drain_entries %llu\n",
            Stats.collective_drain_entries);
    fprintf(fp, "collective_drain_exits %llu\n",
            Stats.collective_drain_exits);
    fprintf(fp, "collective_givens_withheld %llu\n",
            Stats.collective_givens_withheld);
    fprintf(fp, "collective_paramod_from_turns %llu\n",
            Stats.collective_paramod_from_turns);
    fprintf(fp, "collective_paramod_into_turns %llu\n",
            Stats.collective_paramod_into_turns);
    fprintf(fp, "collective_iterator_raw_steps %llu\n",
            Stats.collective_iterator_raw_steps);
    fprintf(fp, "collective_iterator_candidates %llu\n",
            Stats.collective_iterator_candidates);
    fprintf(fp, "collective_iterator_completions %llu\n",
            Stats.collective_iterator_completions);
    fprintf(fp, "collective_iterator_invalidations %llu\n",
            Stats.collective_iterator_invalidations);
    fprintf(fp, "collective_iterator_raw_peak %llu\n",
            Stats.collective_iterator_raw_peak);
    fprintf(fp, "collective_hyper_iterator_raw_steps %llu\n",
            Stats.collective_hyper_iterator_raw_steps);
    fprintf(fp, "collective_hyper_iterator_candidates %llu\n",
            Stats.collective_hyper_iterator_candidates);
    fprintf(fp, "collective_hyper_iterator_completions %llu\n",
            Stats.collective_hyper_iterator_completions);
    fprintf(fp, "collective_candidate_pool_peak_bytes %llu\n",
            Stats.collective_candidate_pool_peak_bytes);
    fprintf(fp, "collective_preview_calls %llu\n",
            Stats.collective_preview_calls);
    fprintf(fp, "collective_preview_hint_matches %llu\n",
            Stats.collective_preview_hint_matches);
    fprintf(fp, "collective_preview_authoritative_matches %llu\n",
            Stats.collective_preview_authoritative_matches);
    fprintf(fp, "collective_preview_false_positives %llu\n",
            Stats.collective_preview_false_positives);
    fprintf(fp, "collective_preview_changed_hint_ids %llu\n",
            Stats.collective_preview_changed_hint_ids);
    fprintf(fp, "collective_preview_stale_refreshes %llu\n",
            Stats.collective_preview_stale_refreshes);
    fprintf(fp, "collective_candidate_pool_commits %llu\n",
            Stats.collective_candidate_pool_commits);
    fprintf(fp, "collective_candidate_priority_commits %llu\n",
            Stats.collective_candidate_priority_commits);
    fprintf(fp, "collective_candidate_fair_commits %llu\n",
            Stats.collective_candidate_fair_commits);
    fprintf(fp, "collective_discovery_turns %llu\n",
            Stats.collective_discovery_turns);
    fprintf(fp, "collective_discovery_hot_turns %llu\n",
            Stats.collective_discovery_hot_turns);
    fprintf(fp, "collective_discovery_general_turns %llu\n",
            Stats.collective_discovery_general_turns);
    fprintf(fp, "collective_discovery_forced_fair_turns %llu\n",
            Stats.collective_discovery_forced_fair_turns);
    fprintf(fp, "collective_discovery_raw_steps %llu\n",
            Stats.collective_discovery_raw_steps);
    fprintf(fp, "collective_discovery_candidates %llu\n",
            Stats.collective_discovery_candidates);
    fprintf(fp, "collective_discovery_promotions %llu\n",
            Stats.collective_discovery_promotions);
    fprintf(fp, "collective_discovery_confirmed %llu\n",
            Stats.collective_discovery_confirmed);
    fprintf(fp, "collective_discovery_false_positives %llu\n",
            Stats.collective_discovery_false_positives);
    fprintf(fp, "collective_discovery_duplicate_skips %llu\n",
            Stats.collective_discovery_duplicate_skips);
    fprintf(fp, "collective_discovery_catchups %llu\n",
            Stats.collective_discovery_catchups);
    fprintf(fp, "collective_discovery_distance_max %llu\n",
            Stats.collective_discovery_distance_max);
    fprintf(fp, "collective_discovery_cap_stalls %llu\n",
            Stats.collective_discovery_cap_stalls);
    fprintf(fp, "simplifier_epoch %u\n", Simplifier_epoch);
    fprintf(fp, "rewrite_epoch %u\n", Rewrite_epoch);
    fprintf(fp, "rewrite_refresh_hot_cursor %llu\n",
            (unsigned long long) Rewrite_refresh_hot_cursor);
    fprintf(fp, "rewrite_refresh_general_cursor %llu\n",
            (unsigned long long) Rewrite_refresh_general_cursor);
    fprintf(fp, "rewrite_interreduce_cursor %llu\n",
            (unsigned long long) Rewrite_interreduce_cursor);
    fprintf(fp, "rewrite_refresh_hot_cursor_id %llu\n",
            dense_passive_cursor_id(Rewrite_refresh_hot_cursor));
    fprintf(fp, "rewrite_refresh_general_cursor_id %llu\n",
            dense_passive_cursor_id(Rewrite_refresh_general_cursor));
    fprintf(fp, "rewrite_interreduce_cursor_id %llu\n",
            dense_passive_cursor_id(Rewrite_interreduce_cursor));
    fprintf(fp, "rewrite_refresh_hot_streak %u\n",
            Rewrite_refresh_hot_streak);
    fprintf(fp, "rewrite_interreduce_streak %u\n",
            Rewrite_interreduce_streak);
    fprintf(fp, "rewrite_refresh_lane_turn %u\n",
            Rewrite_refresh_inference_streak >=
              (unsigned) parm(Opt->rewrite_refresh_inference_ratio) ?
              1U : 0U);
    fprintf(fp, "rewrite_refresh_inference_streak %u\n",
            Rewrite_refresh_inference_streak);
    fprintf(fp, "rewrite_drain_mode %u\n", Rewrite_drain_mode ? 1U : 0U);
    fprintf(fp, "rewrite_drain_streak %u\n", Rewrite_drain_streak);
    fprintf(fp, "hint_state_epoch %llu\n", hint_state_epoch());
    fprintf(fp, "user_seconds %.2f\n", user_seconds());
    /* Save Low selector cycle state for deterministic resume */
    {
      const char *sel_name;
      int sel_count;
      get_low_selector_state(&sel_name, &sel_count);
      fprintf(fp, "low_selector %s\n", sel_name);
      fprintf(fp, "low_selector_count %d\n", sel_count);
      get_high_selector_state(&sel_name, &sel_count);
      fprintf(fp, "high_selector %s\n", sel_name);
      fprintf(fp, "high_selector_count %d\n", sel_count);
    }
    /* Save cac_clauses IDs (commutativity/associativity/AC triggers) */
    if (Glob.cac_clauses != NULL) {
      Ilist p;
      fprintf(fp, "cac_clauses");
      for (p = Glob.cac_clauses; p; p = p->next)
        fprintf(fp, " %d", p->i);
      fprintf(fp, "\n");
    }
    /* Save desc_to_be_disabled clause IDs (may be NULL at checkpoint time) */
    if (Glob.desc_to_be_disabled != NULL) {
      Ilist p;
      fprintf(fp, "desc_to_be_disabled");
      for (p = Glob.desc_to_be_disabled; p; p = p->next)
        fprintf(fp, " %d", p->i);
      fprintf(fp, "\n");
    }
    /* Save hoisted function-local statics */
    fprintf(fp, "bf_level %d\n", Bf_level);
    fprintf(fp, "bf_last_of_level %d\n", Bf_last_of_level);
    fprintf(fp, "nohints_count %d\n", Nohints_count);
    fclose(fp);
  }

  write_collective_checkpoint(tmpdir);

  /* 2. Write clause_data.txt and clause files */
  {
    char cdata_path[600];
    char cpath[600];
    FILE *data_fp;
    int total_clauses = 0;

    snprintf(cdata_path, sizeof(cdata_path), "%s/clause_data.txt", tmpdir);
    data_fp = fopen(cdata_path, "w");
    if (!data_fp) {
      fprintf(stderr, "ERROR: cannot write %s\n", cdata_path);
      return;
    }

    /* Write-side dedup: track clause IDs already written to any .clauses
       file.  When demods writes a clause already in usable/sos, it marks
       the clause_data entry as "shared" and skips the clause text. */
#define SEEN_TAB_SIZE 50000
    {
      Plist seen_tab[SEEN_TAB_SIZE];
      memset(seen_tab, 0, sizeof(seen_tab));

    /* SOS */
    snprintf(cpath, sizeof(cpath), "%s/sos.clauses", tmpdir);
    fp = fopen(cpath, "w");
    if (fp) {
      if (dense_passive_mode())
        total_clauses += write_dense_bare(fp, data_fp, "sos",
                                          seen_tab, SEEN_TAB_SIZE);
      else
        total_clauses += write_clist_bare(fp, data_fp, Glob.sos, "sos",
                                          seen_tab, SEEN_TAB_SIZE);
      fclose(fp);
    }

    /* Usable */
    snprintf(cpath, sizeof(cpath), "%s/usable.clauses", tmpdir);
    fp = fopen(cpath, "w");
    if (fp) {
      total_clauses += write_clist_bare(fp, data_fp, Glob.usable, "usable",
                                        seen_tab, SEEN_TAB_SIZE);
      fclose(fp);
    }

    /* Demodulators - uses seen_tab to detect shared clauses */
    snprintf(cpath, sizeof(cpath), "%s/demods.clauses", tmpdir);
    fp = fopen(cpath, "w");
    if (fp) {
      total_clauses += write_clist_bare(fp, data_fp, Glob.demods,
                                        "demodulators",
                                        seen_tab, SEEN_TAB_SIZE);
      fclose(fp);
    }

    /* Hints - no dedup needed (separate ID namespace) */
    if (Glob.hints->length > 0) {
      snprintf(cpath, sizeof(cpath), "%s/hints.clauses", tmpdir);
      fp = fopen(cpath, "w");
      if (fp) {
        total_clauses += write_clist_bare(fp, data_fp, Glob.hints, "hints",
                                          NULL, 0);
        fclose(fp);
      }
    }

    /* Limbo */
    if (Glob.limbo->length > 0) {
      snprintf(cpath, sizeof(cpath), "%s/limbo.clauses", tmpdir);
      fp = fopen(cpath, "w");
      if (fp) {
        total_clauses += write_clist_bare(fp, data_fp, Glob.limbo, "limbo",
                                          NULL, 0);
        fclose(fp);
      }
    }

    /* Disabled (ancestors for proof reconstruction) */
    if (flag(Opt->checkpoint_ancestors) &&
        clause_store_length(Glob.disabled) > 0) {
      snprintf(cpath, sizeof(cpath), "%s/disabled.clauses", tmpdir);
      fp = fopen(cpath, "w");
      if (fp) {
        total_clauses += write_clause_store_bare(fp, data_fp, Glob.disabled,
                                                 "disabled");
        fclose(fp);
      }
    }

    /* Empties (proof clauses) - Plist, not Clist.  Build temporary Clist. */
    if (Glob.empties != NULL) {
      Plist ep;
      Clist tmp_empties = clist_init("empties");
      for (ep = Glob.empties; ep; ep = ep->next)
        clist_append((Topform) ep->v, tmp_empties);
      snprintf(cpath, sizeof(cpath), "%s/empties.clauses", tmpdir);
      fp = fopen(cpath, "w");
      if (fp) {
        total_clauses += write_clist_bare(fp, data_fp, tmp_empties,
                                          "empties", NULL, 0);
        fclose(fp);
      }
      /* Remove from temp Clist without freeing clauses */
      while (tmp_empties->first)
        clist_remove(tmp_empties->first->c, tmp_empties);
      clist_zap(tmp_empties);
    }

    }  /* end seen_tab scope */

    fclose(data_fp);

    /* 3. Write options.txt for human reference */
    snprintf(cpath, sizeof(cpath), "%s/options.txt", tmpdir);
    fp = fopen(cpath, "w");
    if (fp) {
      fprint_options(fp);
      fclose(fp);
    }

    /* 3b. Write precedence.txt - symbol ordering for deterministic resume.
       Format: "F name arity [flags]" for functions and "R name arity" for
       predicates.  S marks Skolem symbols and U preserves unfold ordering. */
    snprintf(cpath, sizeof(cpath), "%s/precedence.txt", tmpdir);
    fp = fopen(cpath, "w");
    if (fp) {
      Ilist fsyms = current_fsym_precedence();
      Ilist rsyms = current_rsym_precedence();
      Ilist p;
      for (p = fsyms; p; p = p->next) {
        char flags[3];
        int fi = 0;
        if (is_skolem(p->i))
          flags[fi++] = 'S';
        if (is_unfold_symbol(p->i))
          flags[fi++] = 'U';
        flags[fi] = '\0';
        fprintf(fp, "F %s %d%s%s\n", sn_to_str(p->i), sn_to_arity(p->i),
                fi > 0 ? " " : "", flags);
      }
      for (p = rsyms; p; p = p->next)
        fprintf(fp, "R %s %d\n", sn_to_str(p->i), sn_to_arity(p->i));
      zap_ilist(fsyms);
      zap_ilist(rsyms);
      fclose(fp);
    }

    /* 3b2. Write symbol table (symnum assignments) for deterministic
       term_compare_vcp on resume.  The secondary ordering for
       NOT_COMPARABLE equations uses raw SYMNUM, not lex_val. */
    {
      char spath[600];
      FILE *sfp;
      int sn;
      snprintf(spath, sizeof(spath), "%s/symbols.txt", tmpdir);
      sfp = fopen(spath, "w");
      if (sfp) {
        int max_sn = greatest_symnum();
        fprintf(sfp, "%d\n", max_sn);
        for (sn = 1; sn <= max_sn; sn++) {
          char *name = sn_to_str(sn);
          if (name != NULL)
            fprintf(sfp, "%d %d %s\n", sn, sn_to_arity(sn), name);
        }
        fclose(sfp);
      }
    }

    /* 3c. Write FPA_IDs for deterministic FPA leaf ordering on resume */
    write_fpa_ids(tmpdir);

    /* 3c2-3. Legacy indexes preserve pointer-leaf order in serialized trie
       files.  A compact OTTER frontier instead rebuilds all pointer-free
       indexes from the ID-ordered resident checkpoint clauses.  Its literal
       and back-demod FPA indexes do not exist, so calling their legacy
       serializers would be both misleading and invalid. */
    if (!compact_otter_passive_mode()) {
      write_demod_index(tmpdir);
      write_unit_discrim_index(tmpdir);

      /* Write FPA trie structure for fast resume (avoids rebuilding from
         scratch, which is O(n * paths * depth) for millions of clauses). */
      write_fpa_lits_index(tmpdir);
      write_fpa_back_demod_index(tmpdir);
      /* Clashable FPA index */
      {
        char cpath[600];
        FILE *cfp;
        snprintf(cpath, sizeof(cpath), "%s/fpa_clashable_index.txt", tmpdir);
        cfp = fopen(cpath, "w");
        if (cfp) {
          fprintf(cfp, "SECTION pos\n");
          fpa_write_index(cfp, Glob.clashable_idx->pos->fpa);
          fprintf(cfp, "SECTION neg\n");
          fpa_write_index(cfp, Glob.clashable_idx->neg->fpa);
          fprintf(cfp, "END\n");
          fclose(cfp);
        }
      }
    }

    /* 3d. Write justifications for proof reconstruction on resume */
    write_justifications(tmpdir);

    /* 3e. Write non-clause formulas (goals) for proof ancestor tracing */
    write_checkpoint_formulas(tmpdir);

    /* 3f. Write saved_input.txt for self-contained resume */
    write_checkpoint_input(tmpdir);

    /* 3g. Write verification hashes (if checkpoint_verify enabled) */
    if (flag(Opt->checkpoint_verify))
      write_checkpoint_hashes(tmpdir);

    /* 4. Rename temp dir to final name */
    if (rename(tmpdir, finaldir) != 0) {
      fprintf(stderr, "ERROR: cannot rename %s to %s: %s\n",
              tmpdir, finaldir, strerror(errno));
      return;
    }

    fprintf(stderr, "\nCheckpoint written (experimental): %s (%d clauses, %lld MB)\n",
            finaldir, total_clauses, megs_malloced());
    fflush(stderr);
    printf("\n%% Checkpoint written: %s (%d clauses)\n", finaldir, total_clauses);
    fflush(stdout);
  }
}  /* write_checkpoint */

#endif /* !PRIMITIVE_ENVIRONMENT */

/*************
 *
 *   read_metadata_ull()
 *
 *   Read a named unsigned long long value from metadata.
 *
 *************/

static
unsigned long long read_metadata_ull(FILE *fp, const char *key)
{
  char buf[256], name[128];
  unsigned long long val;
  while (fgets(buf, sizeof(buf), fp)) {
    if (sscanf(buf, "%127s %llu", name, &val) == 2) {
      if (strcmp(name, key) == 0)
        return val;
    }
  }
  fprintf(stderr, "WARNING: metadata key '%s' not found, using 0\n", key);
  return 0;
}  /* read_metadata_ull */

/* Optional cumulative fields let a newer binary resume an older checkpoint
   without emitting a warning for counters that did not exist in that format. */
static
BOOL read_metadata_ull_if_present(FILE *fp, const char *key,
                                  unsigned long long *value)
{
  char buf[256], name[128];
  unsigned long long val;
  while (fgets(buf, sizeof(buf), fp)) {
    if (sscanf(buf, "%127s %llu", name, &val) == 2 &&
        strcmp(name, key) == 0) {
      *value = val;
      return TRUE;
    }
  }
  return FALSE;
}

/*************
 *
 *   read_metadata_str()
 *
 *   Read a named string value from metadata.
 *   Returns TRUE if found, FALSE otherwise.
 *   Caller must provide buffer and size.
 *
 *************/

static
BOOL read_metadata_str(FILE *fp, const char *key, char *val, int val_size)
{
  char buf[256], name[128], sval[128];
  while (fgets(buf, sizeof(buf), fp)) {
    if (sscanf(buf, "%127s %127s", name, sval) == 2) {
      if (strcmp(name, key) == 0) {
        {
          int n = strlen(sval);
          if (n >= val_size) n = val_size - 1;
          memcpy(val, sval, n);
          val[n] = '\0';
        }
        return TRUE;
      }
    }
  }
  return FALSE;
}  /* read_metadata_str */

/*************
 *
 *   read_metadata_double()
 *
 *   Read a named double value from metadata.
 *
 *************/

static
double read_metadata_double(FILE *fp, const char *key)
{
  char buf[256], name[128];
  double val;
  while (fgets(buf, sizeof(buf), fp)) {
    if (sscanf(buf, "%127s %lf", name, &val) == 2) {
      if (strcmp(name, key) == 0)
        return val;
    }
  }
  return 0.0;  /* not found - no warning, old checkpoints may lack this */
}  /* read_metadata_double */

/*************
 *
 *   load_clause_data()
 *
 *   Read clause_data.txt into parallel arrays indexed by (list, position).
 *   Returns total number of entries read.
 *
 *************/

struct clause_meta {
  char list_name[32];
  int position;
  unsigned long long id;
  double weight;
  int initial;    /* c->initial flag (0 or 1) */
  int shared;     /* 1 if clause is shared with another list (dedup) */
  int hint_match; /* matched hint ID (1-based), 0 if none */
  int used;       /* c->used flag */
  int was_given;  /* c->was_given flag */
  unsigned simplifier_epoch; /* DISCOUNT active-state generation */
  unsigned rewrite_epoch; /* DISCOUNT rewrite-bank generation */
  int delayed_demodulator; /* live rewrite-only membership */
  int rewrite_rule_dirty; /* targeted compact interreduction debt */
  unsigned long long last_matched; /* hint last_matched_given (for expiry) */
  int redundant_hint; /* hint was in Redundant_hints at checkpoint time */
  unsigned aflags[10]; /* atom private_flags per literal (max 10 lits) */
  int aflags_count;    /* number of aflags entries */
};

static
int load_clause_data(const char *dir, struct clause_meta **out)
{
  char path[600], line[512];
  FILE *fp;
  int capacity = 4096, count = 0;
  struct clause_meta *arr;

  snprintf(path, sizeof(path), "%s/clause_data.txt", dir);
  fp = fopen(path, "r");
  if (!fp)
    fatal_error("resume: cannot open clause_data.txt");

  arr = safe_malloc(capacity * sizeof(struct clause_meta));

  while (fgets(line, sizeof(line), fp)) {
    struct clause_meta *m = &arr[count];
    char *p;
    m->shared = 0;
    m->hint_match = 0;
    m->used = 0;
    m->was_given = 0;
    m->simplifier_epoch = 0;
    m->rewrite_epoch = 0;
    m->delayed_demodulator = 0;
    m->rewrite_rule_dirty = 0;
    m->last_matched = 0;
    m->redundant_hint = 0;
    m->aflags_count = 0;
    if (sscanf(line, "%31s %d %llu %lf %d",
               m->list_name, &m->position,
               &m->id, &m->weight, &m->initial) == 5) {
      if (strstr(line, "shared") != NULL)
        m->shared = 1;
      p = strstr(line, "hint_match");
      if (p != NULL)
        sscanf(p, "hint_match %d", &m->hint_match);
      if (strstr(line, " used") != NULL)
        m->used = 1;
      if (strstr(line, "was_given") != NULL)
        m->was_given = 1;
      p = strstr(line, "simplifier_epoch");
      if (p != NULL)
        sscanf(p, "simplifier_epoch %u", &m->simplifier_epoch);
      p = strstr(line, "rewrite_epoch");
      if (p != NULL)
        sscanf(p, "rewrite_epoch %u", &m->rewrite_epoch);
      if (strstr(line, "delayed_demodulator") != NULL)
        m->delayed_demodulator = 1;
      if (strstr(line, "rewrite_rule_dirty") != NULL)
        m->rewrite_rule_dirty = 1;
      p = strstr(line, "last_matched");
      if (p != NULL)
        sscanf(p, "last_matched %llu", &m->last_matched);
      if (strstr(line, "redundant_hint") != NULL)
        m->redundant_hint = 1;
      /* Parse atom private_flags: "aflags N [aflags N ...]" */
      p = strstr(line, "aflags");
      while (p != NULL && m->aflags_count < 10) {
        unsigned fval;
        if (sscanf(p, "aflags %u", &fval) == 1)
          m->aflags[m->aflags_count++] = fval;
        p = strstr(p + 6, "aflags");
      }
      count++;
      if (count >= capacity) {
        capacity *= 2;
        arr = safe_realloc(arr, capacity * sizeof(struct clause_meta));
      }
    }
  }
  fclose(fp);
  *out = arr;
  return count;
}  /* load_clause_data */

/*************
 *
 *   load_clauses_from_file()
 *
 *   Read clauses from a .clauses file.  Returns a Clist.
 *   Assigns IDs and weights from clause_data metadata.
 *
 *************/

static
Clist load_clauses_from_file(const char *dir, const char *filename,
                             const char *list_name,
                             struct clause_meta *meta, int meta_count,
                             int *meta_offset, BOOL skip_register)
{
  char path[600];
  FILE *fp;
  Clist lst;
  int pos = 0;

  snprintf(path, sizeof(path), "%s/%s", dir, filename);
  fp = fopen(path, "r");
  if (!fp)
    return NULL;  /* file doesn't exist -- ok for optional lists */

  lst = read_clause_clist(fp, stderr, (char *)list_name, FALSE);
  fclose(fp);

  /* Now assign IDs and weights from metadata.
     Skip shared entries (they don't appear in the .clauses file). */
  {
    Clist_pos cp;
    int meta_pos = *meta_offset;

    /* Advance meta_pos past shared entries to find first non-shared */
    for (cp = lst->first; cp != NULL; cp = cp->next) {
      Topform c = cp->c;

      /* Skip shared metadata entries for this list */
      while (meta_pos < meta_count &&
             strcmp(meta[meta_pos].list_name, list_name) == 0 &&
             meta[meta_pos].shared)
        meta_pos++;

      if (meta_pos < meta_count &&
          strcmp(meta[meta_pos].list_name, list_name) == 0 &&
          !meta[meta_pos].shared &&
          meta[meta_pos].position == pos) {
        c->id = meta[meta_pos].id;
        c->weight = meta[meta_pos].weight;
        c->initial = meta[meta_pos].initial;
        c->used = meta[meta_pos].used;
        c->was_given = meta[meta_pos].was_given;
        c->simplifier_epoch = meta[meta_pos].simplifier_epoch;
        c->rewrite_epoch = meta[meta_pos].rewrite_epoch;
        c->delayed_demodulator = meta[meta_pos].delayed_demodulator;
        c->rewrite_rule_dirty = meta[meta_pos].rewrite_rule_dirty;
        c->last_matched_given = meta[meta_pos].last_matched;
        if (!skip_register)
          register_clause_with_id(c);
        meta_pos++;
      }
      else {
        /* Fallback: search all non-shared entries for this list+pos */
        int j;
        for (j = 0; j < meta_count; j++) {
          if (strcmp(meta[j].list_name, list_name) == 0 &&
              !meta[j].shared && meta[j].position == pos) {
            c->id = meta[j].id;
            c->weight = meta[j].weight;
            c->initial = meta[j].initial;
            c->used = meta[j].used;
            c->was_given = meta[j].was_given;
            c->simplifier_epoch = meta[j].simplifier_epoch;
            c->rewrite_epoch = meta[j].rewrite_epoch;
            c->delayed_demodulator = meta[j].delayed_demodulator;
            c->rewrite_rule_dirty = meta[j].rewrite_rule_dirty;
            c->last_matched_given = meta[j].last_matched;
            if (!skip_register)
              register_clause_with_id(c);
            break;
          }
        }
        if (c->id == 0) {
          fprintf(stderr, "WARNING: no metadata for %s clause %d\n",
                  list_name, pos);
          if (!skip_register)
            assign_clause_id(c);
        }
      }
      pos++;
    }
  }
  /* Advance meta_offset past ALL entries for this list (shared + non-shared) */
  while (*meta_offset < meta_count &&
         strcmp(meta[*meta_offset].list_name, list_name) == 0)
    (*meta_offset)++;
  return lst;
}  /* load_clauses_from_file */

/*************
 *
 *   resume_load_precedence()
 *
 *   Read precedence.txt from checkpoint directory and set up
 *   preliminary symbol precedence.  This ensures symbol ordering
 *   on resume matches the original run exactly.
 *
 *   Must be called AFTER loading clauses (so symbols exist in the
 *   symbol table) and BEFORE init_search() (which calls symbol_order).
 *
 *************/

static
void resume_load_precedence(const char *dir)
{
  char path[600];
  FILE *fp;
  Ilist fsyms = NULL, rsyms = NULL;

  snprintf(path, sizeof(path), "%s/precedence.txt", dir);
  fp = fopen(path, "r");
  if (!fp)
    return;  /* old checkpoint without precedence - fall through to default */

  {
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
      char type_ch;
      char name[512];
      int arity;
      char skolem_flag[16];
      int n;

      skolem_flag[0] = '\0';
      n = sscanf(line, " %c %511s %d %15s", &type_ch, name, &arity, skolem_flag);
      if (n < 3)
        continue;

      {
        int sn = str_to_sn(name, arity);
        if (type_ch == 'F') {
          set_symbol_type(sn, FUNCTION_SYMBOL);
          if (strchr(skolem_flag, 'S') != NULL)
            set_skolem(sn);
          if (strchr(skolem_flag, 'U') != NULL)
            set_unfold_symbol(sn);
          fsyms = ilist_prepend(fsyms, sn);
        }
        else if (type_ch == 'R') {
          set_symbol_type(sn, PREDICATE_SYMBOL);
          rsyms = ilist_prepend(rsyms, sn);
        }
      }
    }
  }
  fclose(fp);

  fsyms = reverse_ilist(fsyms);
  rsyms = reverse_ilist(rsyms);

  /* Set as preliminary precedence so symbol_order uses this ordering */
  if (fsyms)
    set_preliminary_precedence_ilist(fsyms, FUNCTION_SYMBOL);
  if (rsyms)
    set_preliminary_precedence_ilist(rsyms, PREDICATE_SYMBOL);
}  /* resume_load_precedence */

/*************
 *
 *   resume_load_clauses()
 *
 *   Phase 1 of resume: load clauses from checkpoint directory into
 *   Glob lists.  No orient_equalities, mark_maximal_literals, or
 *   indexing - those require init_search() to have set up the term
 *   ordering and inference rules first.
 *
 *************/

static
void resume_load_clauses(const char *dir)
{
  char path[600];
  FILE *fp;
  struct clause_meta *meta;
  int meta_count, meta_offset;
  unsigned long long max_clause_id;
  Clist loaded_sos, loaded_usable, loaded_demods, loaded_hints;
  Clist loaded_limbo, loaded_disabled, loaded_empties;

  fprintf(stderr, "Resuming from checkpoint (experimental): %s\n", dir);
  fflush(stderr);

  /* 1. Read metadata */
  snprintf(path, sizeof(path), "%s/metadata.txt", dir);
  fp = fopen(path, "r");
  if (!fp)
    fatal_error("resume: cannot open metadata.txt");

  /* Check checkpoint format version */
  {
    unsigned long long fmt = read_metadata_ull(fp, "checkpoint_format");
    if (fmt != 3) {
      fprintf(stderr, "resume: checkpoint format %llu not supported "
              "(this version requires format 3)\n", (unsigned long long)fmt);
      fatal_error("resume: unsupported checkpoint format");
    }
  }

  /* Read max_clause_id */
  rewind(fp);
  max_clause_id = read_metadata_ull(fp, "max_clause_id");

  /* Read stats */
  rewind(fp); Stats.given = read_metadata_ull(fp, "given");
  rewind(fp); Stats.generated = read_metadata_ull(fp, "generated");
  rewind(fp); Stats.generated_binary = read_metadata_ull(fp, "generated_binary");
  rewind(fp); Stats.generated_hyper = read_metadata_ull(fp, "generated_hyper");
  rewind(fp); Stats.generated_ur = read_metadata_ull(fp, "generated_ur");
  rewind(fp); Stats.generated_paramod = read_metadata_ull(fp, "generated_paramod");
  rewind(fp); Stats.generated_other = read_metadata_ull(fp, "generated_other");
  rewind(fp); Stats.kept = read_metadata_ull(fp, "kept");
  rewind(fp); Stats.proofs = read_metadata_ull(fp, "proofs");
  Disabled_checkpoint_omitted = 0;
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "disabled_checkpoint_omitted", &Disabled_checkpoint_omitted);
  rewind(fp); Stats.back_subsumed = read_metadata_ull(fp, "back_subsumed");
  rewind(fp); Stats.anc_subsume_blocked = read_metadata_ull(fp, "anc_subsume_blocked");
  rewind(fp); Stats.back_demodulated = read_metadata_ull(fp, "back_demodulated");
  rewind(fp); Stats.subsumed = read_metadata_ull(fp, "subsumed");
  rewind(fp); Stats.sos_limit_deleted = read_metadata_ull(fp, "sos_limit_deleted");
  rewind(fp); Stats.new_demodulators = read_metadata_ull(fp, "new_demodulators");
  rewind(fp); Stats.new_lex_demods = read_metadata_ull(fp, "new_lex_demods");
  rewind(fp); Stats.back_unit_deleted = read_metadata_ull(fp, "back_unit_deleted");
  rewind(fp); Stats.demod_attempts = read_metadata_ull(fp, "demod_attempts");
  rewind(fp); Stats.demod_rewrites = read_metadata_ull(fp, "demod_rewrites");
  rewind(fp); Stats.res_instance_prunes = read_metadata_ull(fp, "res_instance_prunes");
  rewind(fp); Stats.para_instance_prunes = read_metadata_ull(fp, "para_instance_prunes");
  rewind(fp); Stats.basic_para_prunes = read_metadata_ull(fp, "basic_para_prunes");
  rewind(fp); Stats.nonunit_fsub = read_metadata_ull(fp, "nonunit_fsub");
  rewind(fp); Stats.nonunit_bsub = read_metadata_ull(fp, "nonunit_bsub");
  rewind(fp); Stats.new_constants = read_metadata_ull(fp, "new_constants");
  rewind(fp); Stats.kept_by_rule = read_metadata_ull(fp, "kept_by_rule");
  rewind(fp); Stats.deleted_by_rule = read_metadata_ull(fp, "deleted_by_rule");
  rewind(fp); Stats.sos_displaced = read_metadata_ull(fp, "sos_displaced");
  rewind(fp); Stats.sos_removed = read_metadata_ull(fp, "sos_removed");
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "passive_demodulator_candidates",
    &Stats.passive_demodulator_candidates);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "passive_oriented_demodulator_candidates",
    &Stats.passive_oriented_demodulator_candidates);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "passive_lex_demodulator_candidates",
    &Stats.passive_lex_demodulator_candidates);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_only_demodulators_admitted",
    &Stats.rewrite_only_demodulators_admitted);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_only_demodulators_retired",
    &Stats.rewrite_only_demodulators_retired);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_only_demodulators_selected",
    &Stats.rewrite_only_demodulators_selected);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_only_demodulators_current",
    &Stats.rewrite_only_demodulators_current);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_only_demodulators_peak",
    &Stats.rewrite_only_demodulators_peak);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_bank_bytes", &Stats.rewrite_bank_bytes);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_bank_peak_bytes", &Stats.rewrite_bank_peak_bytes);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "compact_rewrite_rules_current",
    &Stats.compact_rewrite_rules_current);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "compact_rewrite_rules_peak", &Stats.compact_rewrite_rules_peak);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "compact_rewrite_rules_retired",
    &Stats.compact_rewrite_rules_retired);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "compact_rewrite_attempts", &Stats.compact_rewrite_attempts);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "compact_rewrite_rewrites", &Stats.compact_rewrite_rewrites);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "compact_rewrite_compactions",
    &Stats.compact_rewrite_compactions);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "compact_rewrite_bytes_reclaimed",
    &Stats.compact_rewrite_bytes_reclaimed);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_refresh_scanned", &Stats.rewrite_refresh_scanned);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_refresh_materialized", &Stats.rewrite_refresh_materialized);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_refresh_rewritten", &Stats.rewrite_refresh_rewritten);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_refresh_unchanged", &Stats.rewrite_refresh_unchanged);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_refresh_subsumed", &Stats.rewrite_refresh_subsumed);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_refresh_hot_turns", &Stats.rewrite_refresh_hot_turns);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_refresh_general_turns", &Stats.rewrite_refresh_general_turns);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_interreduce_turns", &Stats.rewrite_interreduce_turns);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_interreduce_changed", &Stats.rewrite_interreduce_changed);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_interreduce_unchanged",
    &Stats.rewrite_interreduce_unchanged);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_interreduce_collapsed",
    &Stats.rewrite_interreduce_collapsed);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_overlap_visits", &Stats.rewrite_overlap_visits);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_overlap_dirty_marks", &Stats.rewrite_overlap_dirty_marks);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_cascade_suppressed", &Stats.rewrite_cascade_suppressed);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_debt_peak", &Stats.rewrite_debt_peak);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_drain_entries", &Stats.rewrite_drain_entries);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_drain_exits", &Stats.rewrite_drain_exits);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_drain_turns", &Stats.rewrite_drain_turns);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_drain_yields", &Stats.rewrite_drain_yields);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_inference_turns", &Stats.rewrite_inference_turns);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_refresh_stale_peak", &Stats.rewrite_refresh_stale_peak);
  rewind(fp); (void) read_metadata_ull_if_present(
    fp, "rewrite_refresh_lag_max", &Stats.rewrite_refresh_lag_max);
  rewind(fp); Stats.passive_refresh_checks =
    read_metadata_ull(fp, "passive_refresh_checks");
  rewind(fp); Stats.passive_refresh_requeued =
    read_metadata_ull(fp, "passive_refresh_requeued");
  rewind(fp); Stats.passive_refresh_subsumed =
    read_metadata_ull(fp, "passive_refresh_subsumed");
  if (collective_frontier_mode()) {
    rewind(fp); Stats.collective_batches_created =
      read_metadata_ull(fp, "collective_batches_created");
    rewind(fp); Stats.collective_batches_completed =
      read_metadata_ull(fp, "collective_batches_completed");
    rewind(fp); Stats.collective_pair_expansions =
      read_metadata_ull(fp, "collective_pair_expansions");
    Stats.collective_pair_turns = Stats.collective_pair_expansions;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_pair_turns", &Stats.collective_pair_turns);
    rewind(fp); Stats.collective_hyper_expansions =
      read_metadata_ull(fp, "collective_hyper_expansions");
    /* Before conclusion chunking, every physical hyper expansion completed
       its set in one turn.  This reconstructs the exact old cumulative
       counter while newer metadata overrides it. */
    Stats.collective_hyper_sets_completed =
      Stats.collective_hyper_expansions;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_hyper_sets_completed",
      &Stats.collective_hyper_sets_completed);
    Stats.collective_candidates_emitted = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_candidates_emitted",
      &Stats.collective_candidates_emitted);
    Stats.collective_candidates_replayed = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_candidates_replayed",
      &Stats.collective_candidates_replayed);
    Stats.collective_deferred_turns = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_deferred_turns",
      &Stats.collective_deferred_turns);
    Stats.collective_raw_candidates_peak = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_raw_candidates_peak",
      &Stats.collective_raw_candidates_peak);
    Stats.collective_raw_candidates_seen = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_raw_candidates_seen",
      &Stats.collective_raw_candidates_seen);
    Stats.collective_candidate_cache_peak = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_candidate_cache_peak",
      &Stats.collective_candidate_cache_peak);
    Stats.collective_candidate_cache_stalls = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_candidate_cache_stalls",
      &Stats.collective_candidate_cache_stalls);
    Stats.collective_promising_scans = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_promising_scans",
      &Stats.collective_promising_scans);
    Stats.collective_promising_considered = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_promising_considered",
      &Stats.collective_promising_considered);
    Stats.collective_promising_buffer_peak = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_promising_buffer_peak",
      &Stats.collective_promising_buffer_peak);
    Stats.collective_promising_priority_turns = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_promising_priority_turns",
      &Stats.collective_promising_priority_turns);
    Stats.collective_promising_fair_turns = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_promising_fair_turns",
      &Stats.collective_promising_fair_turns);
    Stats.collective_promising_heap_peak = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_promising_heap_peak",
      &Stats.collective_promising_heap_peak);
    rewind(fp); Stats.collective_partners_skipped =
      read_metadata_ull(fp, "collective_partners_skipped");
    rewind(fp); Stats.collective_parent_materializations =
      read_metadata_ull(fp, "collective_parent_materializations");
    rewind(fp); Stats.collective_snapshot_rebuilds =
      read_metadata_ull(fp, "collective_snapshot_rebuilds");
    rewind(fp); Stats.collective_snapshot_clauses =
      read_metadata_ull(fp, "collective_snapshot_clauses");
    rewind(fp); Stats.collective_snapshot_clauses_peak =
      read_metadata_ull(fp, "collective_snapshot_clauses_peak");
    rewind(fp); Stats.collective_history_queries =
      read_metadata_ull(fp, "collective_history_queries");
    rewind(fp); Stats.collective_history_candidates =
      read_metadata_ull(fp, "collective_history_candidates");
    rewind(fp); Stats.collective_history_rejected_future =
      read_metadata_ull(fp, "collective_history_rejected_future");
    rewind(fp); Stats.collective_history_rejected_inactive =
      read_metadata_ull(fp, "collective_history_rejected_inactive");
    Stats.collective_hint_probes_scheduled = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_hint_probes_scheduled",
      &Stats.collective_hint_probes_scheduled);
    Stats.collective_hint_probes_expanded = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_hint_probes_expanded",
      &Stats.collective_hint_probes_expanded);
    Stats.collective_hint_selected_total = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_hint_selected_total",
      &Stats.collective_hint_selected_total);
    Stats.collective_hint_selected_hha = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_hint_selected_hha",
      &Stats.collective_hint_selected_hha);
    Stats.collective_hint_selected_hw = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_hint_selected_hw",
      &Stats.collective_hint_selected_hw);
    Stats.collective_hint_selected_lh = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_hint_selected_lh",
      &Stats.collective_hint_selected_lh);
    Stats.collective_hint_selected_other = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_hint_selected_other",
      &Stats.collective_hint_selected_other);
    rewind(fp); Stats.collective_batches_peak =
      read_metadata_ull(fp, "collective_batches_peak");
    Stats.collective_created_paramod = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_created_paramod", &Stats.collective_created_paramod);
    Stats.collective_created_pos_hyper = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_created_pos_hyper",
      &Stats.collective_created_pos_hyper);
    Stats.collective_created_neg_hyper = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_created_neg_hyper",
      &Stats.collective_created_neg_hyper);
    Stats.collective_completed_paramod = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_completed_paramod",
      &Stats.collective_completed_paramod);
    Stats.collective_completed_pos_hyper = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_completed_pos_hyper",
      &Stats.collective_completed_pos_hyper);
    Stats.collective_completed_neg_hyper = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_completed_neg_hyper",
      &Stats.collective_completed_neg_hyper);
    Stats.collective_balanced_paramod_turns = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_balanced_paramod_turns",
      &Stats.collective_balanced_paramod_turns);
    Stats.collective_balanced_pos_hyper_turns = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_balanced_pos_hyper_turns",
      &Stats.collective_balanced_pos_hyper_turns);
    Stats.collective_balanced_neg_hyper_turns = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_balanced_neg_hyper_turns",
      &Stats.collective_balanced_neg_hyper_turns);
    Stats.collective_balanced_oldest_turns = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_balanced_oldest_turns",
      &Stats.collective_balanced_oldest_turns);
    Stats.collective_balanced_lane_turns = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_balanced_lane_turns",
      &Stats.collective_balanced_lane_turns);
    Stats.collective_drain_entries = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_drain_entries", &Stats.collective_drain_entries);
    Stats.collective_drain_exits = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_drain_exits", &Stats.collective_drain_exits);
    Stats.collective_givens_withheld = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_givens_withheld", &Stats.collective_givens_withheld);
    Stats.collective_paramod_from_turns = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_paramod_from_turns",
      &Stats.collective_paramod_from_turns);
    Stats.collective_paramod_into_turns = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_paramod_into_turns",
      &Stats.collective_paramod_into_turns);
    Stats.collective_iterator_raw_steps = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_iterator_raw_steps",
      &Stats.collective_iterator_raw_steps);
    Stats.collective_iterator_candidates = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_iterator_candidates",
      &Stats.collective_iterator_candidates);
    Stats.collective_iterator_completions = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_iterator_completions",
      &Stats.collective_iterator_completions);
    Stats.collective_iterator_invalidations = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_iterator_invalidations",
      &Stats.collective_iterator_invalidations);
    Stats.collective_iterator_raw_peak = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_iterator_raw_peak",
      &Stats.collective_iterator_raw_peak);
    Stats.collective_hyper_iterator_raw_steps = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_hyper_iterator_raw_steps",
      &Stats.collective_hyper_iterator_raw_steps);
    Stats.collective_hyper_iterator_candidates = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_hyper_iterator_candidates",
      &Stats.collective_hyper_iterator_candidates);
    Stats.collective_hyper_iterator_completions = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_hyper_iterator_completions",
      &Stats.collective_hyper_iterator_completions);
    Stats.collective_candidate_pool_peak_bytes = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_candidate_pool_peak_bytes",
      &Stats.collective_candidate_pool_peak_bytes);
    Stats.collective_preview_calls = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_preview_calls", &Stats.collective_preview_calls);
    Stats.collective_preview_hint_matches = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_preview_hint_matches",
      &Stats.collective_preview_hint_matches);
    Stats.collective_preview_authoritative_matches = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_preview_authoritative_matches",
      &Stats.collective_preview_authoritative_matches);
    Stats.collective_preview_false_positives = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_preview_false_positives",
      &Stats.collective_preview_false_positives);
    Stats.collective_preview_changed_hint_ids = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_preview_changed_hint_ids",
      &Stats.collective_preview_changed_hint_ids);
    Stats.collective_preview_stale_refreshes = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_preview_stale_refreshes",
      &Stats.collective_preview_stale_refreshes);
    Stats.collective_candidate_pool_commits = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_candidate_pool_commits",
      &Stats.collective_candidate_pool_commits);
    Stats.collective_candidate_priority_commits = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_candidate_priority_commits",
      &Stats.collective_candidate_priority_commits);
    Stats.collective_candidate_fair_commits = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_candidate_fair_commits",
      &Stats.collective_candidate_fair_commits);
    Stats.collective_discovery_turns = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_discovery_turns", &Stats.collective_discovery_turns);
    Stats.collective_discovery_hot_turns = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_discovery_hot_turns",
      &Stats.collective_discovery_hot_turns);
    Stats.collective_discovery_general_turns = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_discovery_general_turns",
      &Stats.collective_discovery_general_turns);
    Stats.collective_discovery_forced_fair_turns = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_discovery_forced_fair_turns",
      &Stats.collective_discovery_forced_fair_turns);
    Stats.collective_discovery_raw_steps = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_discovery_raw_steps",
      &Stats.collective_discovery_raw_steps);
    Stats.collective_discovery_candidates = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_discovery_candidates",
      &Stats.collective_discovery_candidates);
    Stats.collective_discovery_promotions = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_discovery_promotions",
      &Stats.collective_discovery_promotions);
    Stats.collective_discovery_confirmed = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_discovery_confirmed",
      &Stats.collective_discovery_confirmed);
    Stats.collective_discovery_false_positives = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_discovery_false_positives",
      &Stats.collective_discovery_false_positives);
    Stats.collective_discovery_duplicate_skips = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_discovery_duplicate_skips",
      &Stats.collective_discovery_duplicate_skips);
    Stats.collective_discovery_catchups = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_discovery_catchups",
      &Stats.collective_discovery_catchups);
    Stats.collective_discovery_distance_max = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_discovery_distance_max",
      &Stats.collective_discovery_distance_max);
    Stats.collective_discovery_cap_stalls = 0;
    rewind(fp); (void) read_metadata_ull_if_present(
      fp, "collective_discovery_cap_stalls",
      &Stats.collective_discovery_cap_stalls);
  }
  rewind(fp); Simplifier_epoch =
    (unsigned) read_metadata_ull(fp, "simplifier_epoch");
  if (Simplifier_epoch == 0)
    Simplifier_epoch = 1;
  {
    unsigned long long value = 0;
    rewind(fp);
    if (read_metadata_ull_if_present(fp, "rewrite_epoch", &value) &&
        value != 0)
      Rewrite_epoch = value > UINT_MAX ? UINT_MAX : (unsigned) value;
    else
      Rewrite_epoch = Simplifier_epoch;
    dense_passive_set_rewrite_epoch(Rewrite_epoch);
    value = 0;
    rewind(fp);
    if (read_metadata_ull_if_present(fp, "rewrite_refresh_hot_cursor",
                                     &value))
      Rewrite_refresh_hot_cursor = (size_t) value;
    value = 0;
    rewind(fp);
    if (read_metadata_ull_if_present(fp, "rewrite_refresh_general_cursor",
                                     &value))
      Rewrite_refresh_general_cursor = (size_t) value;
    value = 0;
    rewind(fp);
    if (read_metadata_ull_if_present(fp, "rewrite_interreduce_cursor",
                                     &value))
      Rewrite_interreduce_cursor = (size_t) value;
    Resume_rewrite_cursor_ids = FALSE;
    Resume_rewrite_hot_cursor_id = 0;
    Resume_rewrite_general_cursor_id = 0;
    Resume_rewrite_interreduce_cursor_id = 0;
    value = 0;
    rewind(fp);
    if (read_metadata_ull_if_present(fp, "rewrite_refresh_hot_cursor_id",
                                     &value)) {
      Resume_rewrite_cursor_ids = TRUE;
      Resume_rewrite_hot_cursor_id = value;
      rewind(fp);
      (void) read_metadata_ull_if_present(
        fp, "rewrite_refresh_general_cursor_id",
        &Resume_rewrite_general_cursor_id);
      rewind(fp);
      (void) read_metadata_ull_if_present(
        fp, "rewrite_interreduce_cursor_id",
        &Resume_rewrite_interreduce_cursor_id);
    }
    value = 0;
    rewind(fp);
    if (read_metadata_ull_if_present(fp, "rewrite_refresh_hot_streak",
                                     &value))
      Rewrite_refresh_hot_streak = value > UINT_MAX ? UINT_MAX :
                                   (unsigned) value;
    value = 1;
    rewind(fp);
    if (read_metadata_ull_if_present(fp, "rewrite_interreduce_streak",
                                     &value))
      Rewrite_interreduce_streak = value > UINT_MAX ? UINT_MAX :
                                   (unsigned) value;
    value = 0;
    rewind(fp);
    if (read_metadata_ull_if_present(fp,
                                     "rewrite_refresh_inference_streak",
                                     &value))
      Rewrite_refresh_inference_streak = value > UINT_MAX ? UINT_MAX :
                                          (unsigned) value;
    else {
      value = 0;
      rewind(fp);
      if (read_metadata_ull_if_present(fp, "rewrite_refresh_lane_turn",
                                       &value))
        Rewrite_refresh_inference_streak = value != 0 ?
          (unsigned) parm(Opt->rewrite_refresh_inference_ratio) : 0;
    }
    value = 0;
    rewind(fp);
    if (read_metadata_ull_if_present(fp, "rewrite_drain_mode", &value))
      Rewrite_drain_mode = value != 0;
    value = 0;
    rewind(fp);
    if (read_metadata_ull_if_present(fp, "rewrite_drain_streak", &value))
      Rewrite_drain_streak = value > UINT_MAX ? UINT_MAX : (unsigned) value;
  }
  rewind(fp); Resume_hint_epoch = read_metadata_ull(fp, "hint_state_epoch");
  if (Resume_hint_epoch == 0)
    Resume_hint_epoch = 1;

  /* Restore accumulated CPU time from original run so max_seconds and
     reporting account for total time across checkpoint/resume cycles. */
  {
    double saved_seconds;
    rewind(fp);
    saved_seconds = read_metadata_double(fp, "user_seconds");
    if (saved_seconds > 0.0) {
      /* Don't restore wall clock offset - it causes checkpoint_minutes
         to trigger immediately on resume if the saved time exceeds the
         interval.  CPU time is tracked separately in statistics. */
      /* set_user_seconds_offset(saved_seconds); */
      printf("%%   Saved user_seconds: %.2f (not restored)\n", saved_seconds);
    }
  }

  /* Read selector cycle state (may not be present in old checkpoints) */
  rewind(fp);
  if (!read_metadata_str(fp, "low_selector", Resume_low_selector_name,
                          sizeof(Resume_low_selector_name)))
    Resume_low_selector_name[0] = '\0';
  rewind(fp);
  Resume_low_selector_count = (int) read_metadata_ull(fp, "low_selector_count");
  rewind(fp);
  if (!read_metadata_str(fp, "high_selector", Resume_high_selector_name,
                          sizeof(Resume_high_selector_name)))
    Resume_high_selector_name[0] = '\0';
  rewind(fp);
  Resume_high_selector_count = (int) read_metadata_ull(fp, "high_selector_count");

  /* Read cac_clauses IDs (restored after clause loading via find_clause_by_id) */
  rewind(fp);
  {
    char buf[4096], name[128];
    Resume_cac_ids = NULL;
    Resume_cac_count = 0;
    while (fgets(buf, sizeof(buf), fp)) {
      if (sscanf(buf, "%127s", name) == 1 && strcmp(name, "cac_clauses") == 0) {
        char *p = buf + strlen("cac_clauses");
        unsigned long long id;
        int cap = 0;
        while (sscanf(p, " %llu%n", &id, &cap) == 1) {
          Resume_cac_count++;
          Resume_cac_ids = (unsigned long long *)
            safe_realloc(Resume_cac_ids,
                         Resume_cac_count * sizeof(unsigned long long));
          Resume_cac_ids[Resume_cac_count - 1] = id;
          p += cap;
        }
        break;
      }
    }
  }

  /* Read desc_to_be_disabled IDs (same pattern as cac_clauses) */
  rewind(fp);
  {
    char buf[4096], name[128];
    Resume_dtbd_ids = NULL;
    Resume_dtbd_count = 0;
    while (fgets(buf, sizeof(buf), fp)) {
      if (sscanf(buf, "%127s", name) == 1 &&
          strcmp(name, "desc_to_be_disabled") == 0) {
        char *p = buf + strlen("desc_to_be_disabled");
        unsigned long long id;
        int cap = 0;
        while (sscanf(p, " %llu%n", &id, &cap) == 1) {
          Resume_dtbd_count++;
          Resume_dtbd_ids = (unsigned long long *)
            safe_realloc(Resume_dtbd_ids,
                         Resume_dtbd_count * sizeof(unsigned long long));
          Resume_dtbd_ids[Resume_dtbd_count - 1] = id;
          p += cap;
        }
        break;
      }
    }
  }

  /* Restore hoisted function-local statics */
  rewind(fp);
  Bf_level = (int) read_metadata_ull(fp, "bf_level");
  rewind(fp);
  Bf_last_of_level = (int) read_metadata_ull(fp, "bf_last_of_level");
  rewind(fp);
  Nohints_count = (int) read_metadata_ull(fp, "nohints_count");

  fclose(fp);

  /* 2. Load clause_data */
  meta_count = load_clause_data(dir, &meta);

  /* 3. Load clause files into Glob lists (no orient/index yet).
     Hints use skip_register=TRUE (separate ID namespace). */
  meta_offset = 0;
  loaded_sos = load_clauses_from_file(dir, "sos.clauses", "sos",
                                       meta, meta_count, &meta_offset,
                                       FALSE);
  loaded_usable = load_clauses_from_file(dir, "usable.clauses", "usable",
                                          meta, meta_count, &meta_offset,
                                          FALSE);
  loaded_demods = load_clauses_from_file(dir, "demods.clauses",
                                          "demodulators",
                                          meta, meta_count, &meta_offset,
                                          FALSE);
  loaded_hints = load_clauses_from_file(dir, "hints.clauses", "hints",
                                         meta, meta_count, &meta_offset,
                                         TRUE);  /* skip register */
  loaded_limbo = load_clauses_from_file(dir, "limbo.clauses", "limbo",
                                         meta, meta_count, &meta_offset,
                                         FALSE);
  loaded_disabled = load_clauses_from_file(dir, "disabled.clauses",
                                            "disabled",
                                            meta, meta_count, &meta_offset,
                                            FALSE);
  loaded_empties = load_clauses_from_file(dir, "empties.clauses",
                                           "empties",
                                           meta, meta_count, &meta_offset,
                                           FALSE);

  /* 4. Set clause ID counter past all loaded IDs */
  set_clause_id_count(max_clause_id);

  /* 5. Move loaded clauses into Glob lists (no orient/index) */
  if (loaded_sos) {
    while (loaded_sos->first) {
      Topform c = loaded_sos->first->c;
      clist_remove(c, loaded_sos);
      clist_append(c, Glob.sos);
    }
    clist_zap(loaded_sos);
  }
  if (loaded_usable) {
    while (loaded_usable->first) {
      Topform c = loaded_usable->first->c;
      clist_remove(c, loaded_usable);
      clist_append(c, Glob.usable);
    }
    clist_zap(loaded_usable);
  }
  /* Demods: interleave shared and non-shared in original list order.
     Shared clauses (in both demods and usable/sos) were not written to
     demods.clauses; resolve them by ID from already-loaded lists. */
  {
    int shared_count = 0, i;
    Clist_pos next_loaded = loaded_demods ? loaded_demods->first : NULL;
    for (i = 0; i < meta_count; i++) {
      if (strcmp(meta[i].list_name, "demodulators") != 0)
        continue;
      if (meta[i].shared) {
        Topform existing = find_clause_by_id(meta[i].id);
        if (existing != NULL) {
          clist_append(existing, Glob.demods);
          shared_count++;
        }
      }
      else if (next_loaded != NULL) {
        Topform c = next_loaded->c;
        next_loaded = next_loaded->next;
        clist_remove(c, loaded_demods);
        clist_append(c, Glob.demods);
      }
    }
    /* Append any remaining loaded demods (safety) */
    if (loaded_demods) {
      while (loaded_demods->first) {
        Topform c = loaded_demods->first->c;
        clist_remove(c, loaded_demods);
        clist_append(c, Glob.demods);
      }
      clist_zap(loaded_demods);
    }
    if (shared_count > 0)
      printf("%%   (%d shared demods resolved from usable/sos)\n",
             shared_count);
  }
  if (loaded_hints) {
    while (loaded_hints->first) {
      Topform c = loaded_hints->first->c;
      clist_remove(c, loaded_hints);
      clist_append(c, Glob.hints);
    }
    clist_zap(loaded_hints);
  }
  if (loaded_limbo) {
    while (loaded_limbo->first) {
      Topform c = loaded_limbo->first->c;
      clist_remove(c, loaded_limbo);
      clist_append(c, Glob.limbo);
    }
    clist_zap(loaded_limbo);
  }
  if (loaded_disabled) {
    while (loaded_disabled->first) {
      Topform c = loaded_disabled->first->c;
      clist_remove(c, loaded_disabled);
      clause_store_append(Glob.disabled, c);
    }
    clist_zap(loaded_disabled);
  }
  /* Empties (proof clauses) - load into Glob.empties Plist */
  if (loaded_empties) {
    int n = 0;
    while (loaded_empties->first) {
      Topform c = loaded_empties->first->c;
      clist_remove(c, loaded_empties);
      Glob.empties = plist_append(Glob.empties, c);
      n++;
    }
    clist_zap(loaded_empties);
    if (n > 0)
      printf("%%   Restored %d empty (proof) clauses from checkpoint.\n", n);
  }
  /* Keep meta for hint_match restoration in resume_index_clauses */
  Resume_meta = meta;
  Resume_meta_count = meta_count;

  printf("\n%% Loaded from checkpoint: %s\n", dir);
  printf("%%   sos=%d, usable=%d, demods=%d, hints=%d, limbo=%d, disabled=%d\n",
         Glob.sos->length, Glob.usable->length, Glob.demods->length,
         Glob.hints->length, Glob.limbo->length,
         (int) clause_store_length(Glob.disabled));
  printf("%%   Stats: given=%llu, generated=%llu, kept=%llu, proofs=%llu\n",
         Stats.given, Stats.generated, Stats.kept, Stats.proofs);
  printf("%%   max_clause_id=%llu\n", max_clause_id);
  fflush(stdout);
}  /* resume_load_clauses */

/*************
 *
 *   topform_id_qsort_compare() -- qsort wrapper for sorting by clause ID
 *
 *************/

static
int topform_id_qsort_compare(const void *a, const void *b)
{
  Topform ca = *(Topform *)a;
  Topform cb = *(Topform *)b;
  if (ca->id < cb->id) return -1;
  if (ca->id > cb->id) return  1;
  return 0;
}

static
void collect_clause_fpa_ids(Topform c, Term *id_table, unsigned id_count)
{
  Literals lit;
  for (lit = c->literals; lit != NULL; lit = lit->next) {
    struct { Term t; int i; } stack[256];
    int depth = 0;
    stack[depth].t = lit->atom;
    stack[depth].i = 0;
    depth++;
    while (depth > 0) {
      Term t = stack[depth-1].t;
      int child = stack[depth-1].i;
      if (child >= ARITY(t)) {
        if (FPA_ID(t) != 0 && FPA_ID(t) <= id_count)
          id_table[FPA_ID(t)] = t;
        depth--;
      }
      else {
        stack[depth-1].i = child + 1;
        if (depth < 256) {
          stack[depth].t = ARG(t, child);
          stack[depth].i = 0;
          depth++;
        }
      }
    }
  }
}

/*************
 *
 *   load_checkpoint_into_loop()
 *
 *   Load checkpoint data and rebuild indexes INSIDE the main search loop.
 *   Called at the same program point as the checkpoint save (before
 *   make_inferences, after limbo_process from the previous iteration).
 *   This ensures save and restore happen at the same loop location,
 *   producing bit-identical state.
 *
 *************/

static
void load_checkpoint_into_loop(void)
{
  Clist_pos p;
  int fpa_depth;

  collective_clear_state();

  /* 0a. Drop the old disabled/archive store before clearing its tagged ID
     entries.  This matters for the in-process save+reload harness; a fresh
     resume process reaches the same empty state. */
  clause_store_delete_clauses(Glob.disabled);
  Glob.disabled = new_disabled_store();
  cold_passive_store_free(Dense_body_store);
  Dense_body_store = dense_passive_mode() && !compact_otter_passive_mode() ?
    new_dense_body_store() : NULL;
  Dense_arena_bytes_reclaimed = 0;

  /* Clear clause ID hash table so stale entries don't shadow
     newly-loaded clauses (critical for in-process save+reload). */
  clear_clause_id_tab();

  /* 0b. Restore symbol table (symnum assignments) BEFORE any clause loading.
     Ensures str_to_sn assigns the same symnums as the original run, which
     is critical for term_compare_vcp secondary ordering. */
  {
    char spath[600];
    FILE *sfp;
    snprintf(spath, sizeof(spath), "%s/symbols.txt", Resume_dir);
    sfp = fopen(spath, "r");
    if (sfp) {
      char line[4096];
      int restored = 0;
      if (fgets(line, sizeof(line), sfp) != NULL) {
        while (fgets(line, sizeof(line), sfp) != NULL) {
          int sn, arity, used = 0;
          if (sscanf(line, "%d %d %n", &sn, &arity, &used) == 2) {
            char *name = line + used;
            size_t len = strlen(name);
            int actual_sn;
            while (len > 0 && (name[len-1] == '\n' || name[len-1] == '\r'))
              name[--len] = '\0';
            if (len == 0)
              fatal_error("resume: empty symbol name in symbols.txt");
            actual_sn = str_to_sn(name, arity);
            if (actual_sn != sn)
              fatal_error("resume: symbols.txt symnum mismatch");
            restored++;
          }
        }
      }
      fclose(sfp);
      printf("%%   Restored %d symbols from checkpoint (%d max)\n",
             restored, greatest_symnum());
    }
  }

  /* 1. Clear Glob lists and selector AVL trees.  Selectors are rebuilt
     from scratch via insert_into_sos2 later in this function. */
  reset_selector_indexes();
  while (Glob.sos->first)
    clist_remove(Glob.sos->first->c, Glob.sos);
  while (Glob.usable->first)
    clist_remove(Glob.usable->first->c, Glob.usable);
  while (Glob.demods->first)
    clist_remove(Glob.demods->first->c, Glob.demods);
  while (Glob.hints->first)
    clist_remove(Glob.hints->first->c, Glob.hints);

  /* 2. Load checkpoint data */
  resume_load_clauses(Resume_dir);
#ifndef PRIMITIVE_ENVIRONMENT
  read_collective_checkpoint(Resume_dir);
#endif
  restore_fpa_ids(Resume_dir);
#ifndef PRIMITIVE_ENVIRONMENT
  restore_checkpoint_formulas(Resume_dir);
  restore_justifications(Resume_dir);
#endif
  resume_load_precedence(Resume_dir);

  /* 2b. Restore atom private_flags (oriented_eq, renamable_flip, maximal,
     selected marks) from checkpoint metadata.  These flags are stored in
     term->private_flags and are lost during text serialization. */
  if (Resume_meta != NULL) {
    int i, restored = 0, not_found = 0;
    for (i = 0; i < Resume_meta_count; i++) {
      /* Skip hint entries - hints use a separate ID namespace (1..N)
         that collides with regular clause IDs.  Hint aflags would
         corrupt regular clauses found by find_clause_by_id. */
      if (strcmp(Resume_meta[i].list_name, "hints") == 0)
        continue;
      if (Resume_meta[i].aflags_count > 0) {
        Topform c = find_clause_by_id(Resume_meta[i].id);
        if (c != NULL) {
          Literals lit;
          int j = 0;
          for (lit = c->literals; lit != NULL && j < Resume_meta[i].aflags_count;
               lit = lit->next, j++) {
            lit->atom->private_flags = (FLAGS_TYPE) Resume_meta[i].aflags[j];
          }
          restored++;
        }
        else
          not_found++;
      }
    }
    printf("%%   Restored aflags: %d clauses (%d not found)\n",
           restored, not_found);
  }

  /* Clear and restore Glob.cac_clauses from saved IDs.
     Must clear first - in-process reload leaves stale entries. */
  if (Glob.cac_clauses != NULL) {
    zap_ilist(Glob.cac_clauses);
    Glob.cac_clauses = NULL;
  }
  if (Resume_cac_ids != NULL) {
    int i, restored = 0;
    /* Iterate backward: IDs were written head-to-tail, plist_prepend
       reverses, so backward iteration preserves original order. */
    for (i = Resume_cac_count - 1; i >= 0; i--) {
      if (find_clause_by_id(Resume_cac_ids[i]) != NULL) {
        Glob.cac_clauses =
          ilist_prepend(Glob.cac_clauses, (int) Resume_cac_ids[i]);
        restored++;
      }
    }
    safe_free(Resume_cac_ids);
    Resume_cac_ids = NULL;
    if (restored > 0)
      printf("%%   Restored %d cac_clauses from checkpoint.\n", restored);
  }

  /* Clear and restore Glob.desc_to_be_disabled from saved IDs. */
  if (Glob.desc_to_be_disabled != NULL) {
    zap_ilist(Glob.desc_to_be_disabled);
    Glob.desc_to_be_disabled = NULL;
  }
  if (Resume_dtbd_ids != NULL) {
    int i, restored = 0;
    for (i = Resume_dtbd_count - 1; i >= 0; i--) {
      if (find_clause_by_id(Resume_dtbd_ids[i]) != NULL) {
        Glob.desc_to_be_disabled =
          ilist_prepend(Glob.desc_to_be_disabled, (int) Resume_dtbd_ids[i]);
        restored++;
      }
    }
    safe_free(Resume_dtbd_ids);
    Resume_dtbd_ids = NULL;
    if (restored > 0)
      printf("%%   Restored %d desc_to_be_disabled from checkpoint.\n",
             restored);
  }

  /* Recompute clause properties from loaded clauses (the call in the
     resume setup block ran on empty lists before clauses were loaded). */
  basic_clause_properties(Glob.sos, Glob.usable);

  /* Rebuild AC/C redundancy detection state from loaded clauses.
     The ac_redun.c static lists (C_symbols, A1_symbols, A2_symbols,
     AC_symbols) are process-local and empty in a fresh process.
     Scan for commutativity/associativity axioms to re-seed them. */
  if (flag(Opt->cac_redundancy)) {
    Clist scan_lists[3];
    int li, seeded = 0;
    size_t di;
    scan_lists[0] = Glob.usable;
    scan_lists[1] = Glob.sos;
    scan_lists[2] = Glob.demods;
    for (li = 0; li < 3; li++) {
      Clist_pos cp;
      for (cp = scan_lists[li]->first; cp != NULL; cp = cp->next) {
        if (seed_cac_properties(cp->c))
          seeded++;
      }
    }
    for (di = 0; di < clause_store_length(Glob.disabled); di++)
      if (seed_cac_properties(clause_store_get(Glob.disabled, di)))
        seeded++;
    if (seeded > 0)
      printf("%%   Seeded %d CAC properties from checkpoint clauses.\n",
             seeded);
  }

  /* Convert preliminary precedence to actual lex_val ordering.
     symbol_order uses the preliminary precedence + clause symbols
     to assign lex_val (the actual precedence used by term ordering). */
  symbol_order(Glob.usable, Glob.sos, Glob.demods, !flag(Opt->quiet));
  /* 3. Re-initialize indexes (old indexes are leaked - acceptable for
     in-process save+reload testing and cross-process resume alike) */

  /* Initialize indexes */
  Glob.use_clash_idx = live_clash_index_needed();

  set_fpa_hash_threshold(parm(Opt->fpa_hash_threshold));
  set_discrim_hash_threshold(parm(Opt->discrim_hash_threshold));

  fpa_depth = parm(Opt->fpa_depth);
  configure_search_indexes();
  init_literals_index(fpa_depth);
  init_demodulator_index(DISCRIM_BIND, ORDINARY_UNIF, 0);
  init_back_demod_index(FPA, ORDINARY_UNIF, fpa_depth);
  Glob.clashable_idx = lindex_init(FPA, ORDINARY_UNIF, fpa_depth,
                                   FPA, ORDINARY_UNIF, fpa_depth);
  init_hints(ORDINARY_UNIF, Att.bsub_hint_wt,
             flag(Opt->collect_hint_labels),
             flag(Opt->back_demod_hints),
             configured_hint_fpa_depth(),
             packed_hint_bank_mode(),
             better_packed_hint_mode(),
             fast_packed_hint_mode(),
             current_demodulate_clause);
  set_hint_match_stats(flag(Opt->hint_match_stats));
  set_hint_match_once(flag(Opt->hint_match_once));
  init_semantics(Glob.interps, Clocks.semantics,
                 stringparm1(Opt->multiple_interps),
                 parm(Opt->eval_limit),
                 parm(Opt->eval_var_limit));

  /* Rebuild the immutable inference history before resuming any delayed
     descriptor.  At this point active, passive, and disabled checkpoint
     clauses are still materialized and registered by ID; replaying activation
     order gives the persistent FPA leaves the same relative ID ordering as
     the uninterrupted run. */
  if (collective_frontier_mode()) {
    unsigned long long position;
    for (position = 0; position < Collective_activation_count; position++) {
      unsigned long long id = collective_activation_id(position);
      Topform c = find_clause_by_id(id);
      if (c == NULL)
        fatal_error("resume: historical activation clause is missing");
      collective_install_history_clause(position, c);
    }
  }

  /* 4-8. Orient, index all checkpoint clauses in CLAUSE ID ORDER.
     The original run indexed clauses in the order they were discovered
     (which is strictly increasing by clause ID).  DISCRIM tree leaf-list
     ordering depends on insertion order, so we must reproduce it exactly
     for deterministic forward subsumption (backsub_check is bounded). */
  {
    int n_usable, n_sos, n_all, idx;
    Topform *all_clauses;  /* merged usable+SOS for ID-sorted indexing */
    char *is_usable;

    /* Build merged array of usable + SOS clauses */
    n_usable = Glob.usable->length;
    n_sos    = Glob.sos->length;
    n_all    = n_usable + n_sos;
    all_clauses = (Topform *) safe_malloc(n_all * sizeof(Topform));
    idx = 0;
    for (p = Glob.usable->first; p != NULL; p = p->next)
      all_clauses[idx++] = p->c;
    for (p = Glob.sos->first; p != NULL; p = p->next)
      all_clauses[idx++] = p->c;

    /* Sort by clause ID (reproduces original insertion order) */
    qsort(all_clauses, n_all, sizeof(Topform), topform_id_qsort_compare);

    is_usable = (char *) safe_calloc(n_all, sizeof(char));
    for (p = Glob.usable->first; p != NULL; p = p->next) {
      int lo = 0, hi = n_all - 1;
      while (lo <= hi) {
	int mid = lo + (hi - lo) / 2;
	if (all_clauses[mid]->id == p->c->id) {
	  is_usable[mid] = 1;
	  break;
	}
	else if (all_clauses[mid]->id < p->c->id)
	  lo = mid + 1;
	else
	  hi = mid - 1;
      }
    }

    /* Set container links for all clauses (needed regardless of index method) */
    for (idx = 0; idx < n_all; idx++)
      upward_clause_links(all_clauses[idx]);

    /* Try fast FPA restore from serialized trie files.
       Falls back to per-clause FPA rebuild if files are missing. */
    {
      BOOL fpa_restored = FALSE;

      /* Collective hyper batches are sensitive to complete clashable-leaf
         multiplicity.  Until the serialized Lindex format has a dedicated
         multiplicity check, rebuild the comparatively small active indexes
         from clauses instead of accepting a structurally valid but lossy
         fast restore. */
      if (!collective_frontier_mode() && !compact_otter_passive_mode()) {
      /* Build FPA_ID -> Term* lookup table for trie restore */
      {
        unsigned id_count = get_fpa_id_count();
        Term *id_table = (Term *) safe_calloc(id_count + 1, sizeof(Term));
        int vi;
        Term v;

        /* Collect variable term IDs */
        for (vi = 0; (v = get_variable_term_if_exists(vi)) != NULL; vi++) {
          if (FPA_ID(v) != 0 && FPA_ID(v) <= id_count)
            id_table[FPA_ID(v)] = v;
        }
        /* Collect all clause term IDs */
        for (idx = 0; idx < n_all; idx++)
          collect_clause_fpa_ids(all_clauses[idx], id_table, id_count);
        /* Also walk hint and disabled clause terms */
        {
          Clist_pos cp;
          size_t di;
          for (cp = Glob.hints->first; cp != NULL; cp = cp->next)
            collect_clause_fpa_ids(cp->c, id_table, id_count);
          for (di = 0; di < clause_store_length(Glob.disabled); di++)
            collect_clause_fpa_ids(clause_store_get(Glob.disabled, di),
                                   id_table, id_count);
        }

        fpa_set_id_table(id_table, id_count);
        fprintf(stderr, "%% Built FPA_ID lookup table (%u entries).\n", id_count);
        fflush(stderr);
      }

      /* Try restoring FPA indexes from serialized trie files */
      {
        BOOL lits_ok, bdemod_ok, clash_ok;
        fprintf(stderr, "%% Restoring FPA indexes from checkpoint...\n");
        fflush(stderr);

        lits_ok = restore_fpa_lits_index(Resume_dir);
        bdemod_ok = restore_fpa_back_demod_index(Resume_dir);

        /* Clashable FPA index */
        clash_ok = FALSE;
        {
          char cpath[600];
          FILE *cfp;
          snprintf(cpath, sizeof(cpath), "%s/fpa_clashable_index.txt", Resume_dir);
          cfp = fopen(cpath, "r");
          if (cfp) {
            char buf[64];
            int restored = 0;
            while (fscanf(cfp, " %63s", buf) == 1) {
              if (strcmp(buf, "END") == 0) break;
              if (strcmp(buf, "SECTION") != 0) continue;
              if (fscanf(cfp, " %63s", buf) != 1) break;
              if (strcmp(buf, "pos") == 0) {
                if (fpa_restore_index(cfp, Glob.clashable_idx->pos->fpa))
                  restored++;
              } else if (strcmp(buf, "neg") == 0) {
                if (fpa_restore_index(cfp, Glob.clashable_idx->neg->fpa))
                  restored++;
              }
            }
            fclose(cfp);
            clash_ok = (restored == 2);
            if (clash_ok)
              printf("%%   Restored FPA clashable index from %s\n", cpath);
          }
        }

        fpa_restored = lits_ok && bdemod_ok && clash_ok;
      }

      fpa_free_id_table();
      }

      if (!fpa_restored) {
        /* Fallback: rebuild FPA indexes from scratch (format 3 checkpoints
           or missing FPA trie files). */
        fprintf(stderr, "%% FPA trie files not found, rebuilding indexes...\n");
        fprintf(stderr, "%% Indexing %d %s clauses...\n",
                discount_mode() ? n_usable : n_all,
                discount_mode() ? "active" : "active/passive");
        fflush(stderr);
        for (idx = 0; idx < n_all; idx++) {
          Topform c = all_clauses[idx];
          if (compact_otter_passive_mode()) {
            /* Reproduce the original eager admission sequence.  Compact
               literal/nonunit and back-demod records retain IDs and term
               slices only, so all bodies can be archived again after this
               deterministic replay. */
            index_literals(c, INSERT, Clocks.index, FALSE);
            index_back_demod(c, INSERT, Clocks.index,
                             flag(Opt->back_demod));
          }
          else if ((!discount_mode() || is_usable[idx]) &&
                   !collective_frontier_mode()) {
            index_literals_fpa_only(c, INSERT, Clocks.index, FALSE);
            index_back_demod(c, INSERT, Clocks.index, flag(Opt->back_demod));
          }
          /* In collective mode, rebuild the clashable index below by
             replaying activation history.  Its leaf ordering affects the
             order in which a hyper batch presents conclusions to
             cl_process(), and therefore which equivalent conclusion is
             retained first.  Clause-ID order is not activation order. */
          if (is_usable[idx] && !collective_frontier_mode())
            index_clashable(c, INSERT);
          if ((idx + 1) % 1000000 == 0) {
            fprintf(stderr, "%%   %d / %d clauses indexed...\n", idx + 1, n_all);
            fflush(stderr);
          }
        }

        if (collective_frontier_mode()) {
          unsigned long long position;
          for (position = 0; position < Collective_activation_count;
               position++) {
            unsigned long long id = collective_activation_id(position);
            Topform c;
            if (collective_deactivation_epoch(id) != 0)
              continue;
            c = find_clause_by_id(id);
            if (c == NULL || !clist_member(c, Glob.usable))
              fatal_error("resume: active history does not match usable set");
            index_literals_fpa_only(c, INSERT, Clocks.index, FALSE);
            index_back_demod(c, INSERT, Clocks.index, flag(Opt->back_demod));
            if (collective_activation_clashable(position))
              index_clashable(c, INSERT);
          }
        }
      }

      /* The serialized FPA tries do not include the nonunit feature tree
         used by forward/back subsumption.  Rebuild it for both the fast
         restore and fallback paths. */
      if (compact_otter_passive_mode()) {
        /* index_literals() above rebuilt both compact subsumption indexes. */
      }
      else if (collective_frontier_mode()) {
        unsigned long long position;
        for (position = 0; position < Collective_activation_count;
             position++) {
          unsigned long long id = collective_activation_id(position);
          Topform c;
          if (collective_deactivation_epoch(id) != 0)
            continue;
          c = find_clause_by_id(id);
          if (c == NULL || !clist_member(c, Glob.usable))
            fatal_error("resume: active history does not match usable set");
          index_literals_features_only(c, INSERT, Clocks.index);
        }
      }
      else {
        for (idx = 0; idx < n_all; idx++) {
          if (!discount_mode() || is_usable[idx])
            index_literals_features_only(all_clauses[idx], INSERT, Clocks.index);
        }
      }
    }
    safe_free(is_usable);
    safe_free(all_clauses);

    /* Recreate rewrite-only proof owners while SOS bodies are resident.  The
       compact bank is rebuilt in global clause-ID order so first-match rule
       ordering is identical to uninterrupted admission order.  Owners remain
       unregistered until dense bulk archival transfers each proof ID. */
    if (eager_interreduced_demod_mode() || compact_otter_demod_mode())
      restore_compact_rewrite_bank();
    else if (eager_legacy_demod_mode()) {
      for (p = Glob.sos->first; p != NULL; p = p->next) {
        int type = demodulator_type(p->c,
                                    parm(Opt->lex_dep_demod_lim),
                                    flag(Opt->lex_dep_demod_sane));
        if (type != NOT_DEMODULATOR)
          store_rewrite_only_clone(p->c, type);
      }
    }

    /* Set up container links for demodulators (needed for demod index
       serialization which navigates term->clause).  Skip orient_equalities
       - atom flags restored from aflags in checkpoint metadata. */
    for (p = Glob.demods->first; p != NULL; p = p->next)
      upward_clause_links(p->c);

    /* Restore DISCRIM index leaf orderings from serialized data.
       This preserves the exact leaf-list order from the original run,
       which determines forward demodulation and subsumption behavior. */
    if (!eager_interreduced_demod_mode() && !compact_otter_demod_mode())
      restore_demod_index(Resume_dir, Clocks.index,
                          eager_legacy_demod_mode() ?
                            resolve_rewrite_only_demodulator : NULL,
                          NULL);
    if (!flag(Opt->compact_otter_unit_index))
      restore_unit_discrim_index(Resume_dir);

    if (flag(Opt->eval_rewrite))
      init_dollar_eval(Glob.demods);

    /* Index hints, preserving the redundant/active partition from the
       original run.  Hints marked redundant_hint in clause_data are put
       directly into Redundant_hints without the subsumption check that
       index_hint normally performs (which gives different results when
       back-demodulated hints are re-indexed from scratch). */
    {
      int hint_id_number = 1;
      int meta_hint_pos = 0;  /* tracks position in Resume_meta hints section */
      fprintf(stderr, "%% Indexing %d hints...\n", Glob.hints->length);
      fflush(stderr);
      for (p = Glob.hints->first; p != NULL; p = p->next) {
        Topform h = p->c;
        int is_redundant = 0;

        /* Find this hint's redundant status from metadata */
        if (Resume_meta != NULL) {
          while (meta_hint_pos < Resume_meta_count &&
                 strcmp(Resume_meta[meta_hint_pos].list_name, "hints") != 0)
            meta_hint_pos++;
          if (meta_hint_pos < Resume_meta_count)
            is_redundant = Resume_meta[meta_hint_pos].redundant_hint;
          meta_hint_pos++;
        }

        h->id = hint_id_number++;
        orient_equalities(h, FALSE);
        renumber_variables(h, MAX_VARS);
        if (is_redundant)
          index_hint_as_redundant(h);
        else
          index_hint(h);  /* NOTE: this zeroes h->weight */
      }
      /* Index reconstruction advances the epoch internally; restore the
         logical search-state epoch saved at the checkpoint boundary. */
      set_hint_state_epoch(Resume_hint_epoch);
    }

    /* Restore hint degradation weights and last_matched_given from
       checkpoint metadata.  index_hint zeroes h->weight, so we must
       re-apply the saved values.  last_matched_given drives hint expiry
       and re-match statistics.  Hints matched by position. */
    {
      int hint_pos = 0, restored = 0;
      int i;
      for (i = 0; i < Resume_meta_count; i++) {
        if (strcmp(Resume_meta[i].list_name, "hints") == 0) {
          if (Resume_meta[i].weight != 0.0 || Resume_meta[i].last_matched != 0) {
            int hid = hint_pos + 1;
            Clist_pos hp;
            for (hp = Glob.hints->first; hp != NULL; hp = hp->next) {
              if (hp->c->id == (unsigned long long)hid) {
                if (Resume_meta[i].weight != 0.0)
                  hp->c->weight = Resume_meta[i].weight;
                if (Resume_meta[i].last_matched != 0)
                  hp->c->last_matched_given = Resume_meta[i].last_matched;
                restored++;
                break;
              }
            }
          }
          hint_pos++;
        }
      }
      if (restored > 0)
        printf("%%   Restored %d hint degradation weights.\n", restored);
    }

    /* Restore matching_hint pointers from checkpoint metadata */
    if (Resume_meta != NULL) {
      int i, restored = 0;
      Topform *hint_arr = NULL;
      int hint_count = Glob.hints->length;
      if (hint_count > 0) {
        hint_arr = (Topform *) safe_malloc((hint_count+1) * sizeof(Topform));
        for (p = Glob.hints->first; p != NULL; p = p->next) {
          Topform h = p->c;
          if (h->id >= 1 && h->id <= (unsigned long long)hint_count)
            hint_arr[h->id] = h;
        }
      }
      for (i = 0; i < Resume_meta_count; i++) {
        if (Resume_meta[i].hint_match > 0 && hint_arr != NULL &&
            Resume_meta[i].hint_match <= hint_count) {
          Topform c = find_clause_by_id(Resume_meta[i].id);
          if (c != NULL) {
            c->matching_hint = hint_arr[Resume_meta[i].hint_match];
            restored++;
          }
        }
      }
      if (hint_arr) safe_free(hint_arr);
      safe_free(Resume_meta);
      Resume_meta = NULL;
      if (restored > 0)
        printf("%%   Restored %d hint matches from checkpoint.\n", restored);
    }

    /* Insert SOS clauses into selection AVL trees.
       Use bulk construction: evaluate semantics + qsort + build balanced
       AVL in O(n log n), vs O(n log n) individual inserts with per-insert
       rebalancing overhead.  Major speedup for 10M+ clause resumes. */
    {
      fprintf(stderr, "%% Bulk-inserting %d SOS clauses into selection queue...\n",
              Glob.sos->length);
      fflush(stderr);
      /* Dense insertion destroys each resident Topform after archiving it,
         so compute the DISCOUNT generation stamp/delayed-demodulator bit
         while the restored body is still resident. */
      if (discount_mode() && dense_passive_mode()) {
        for (p = Glob.sos->first; p != NULL; p = p->next)
          prepare_discount_passive(p->c, FALSE);
      }
      bulk_insert_into_sos2(Glob.sos);
      if (Resume_rewrite_cursor_ids) {
        Rewrite_refresh_hot_cursor = dense_passive_cursor_from_id(
          Resume_rewrite_hot_cursor_id);
        Rewrite_refresh_general_cursor = dense_passive_cursor_from_id(
          Resume_rewrite_general_cursor_id);
        Rewrite_interreduce_cursor = dense_passive_cursor_from_id(
          Resume_rewrite_interreduce_cursor_id);
        Resume_rewrite_cursor_ids = FALSE;
      }
      if (discount_mode() && !dense_passive_mode()) {
        for (p = Glob.sos->first; p != NULL; p = p->next)
          prepare_discount_passive(p->c, FALSE);
      }
    }
  }

  /* 9. Restore selector cycle state */
  if (Resume_low_selector_name[0] != '\0')
    set_low_selector_state(Resume_low_selector_name,
                           Resume_low_selector_count);
  if (Resume_high_selector_name[0] != '\0')
    set_high_selector_state(Resume_high_selector_name,
                            Resume_high_selector_count);

  /* 10. Verify checkpoint hashes */
  if (flag(Opt->checkpoint_verify))
    verify_checkpoint_hashes(Resume_dir);

  if (flag(Opt->print_derivations))
    get_hit_list();

  /* Restored disabled clauses must remain materialized through FPA-ID,
     atom-flag, justification, hash, and hit-list restoration. */
  compress_retained_store(Glob.disabled);

  /* 11. Update stats and print status */
  update_stats();

  if (!flag(Opt->quiet)) {
    printf("%% Resumed: sos=%d, usable=%d, demods=%d, disabled=%d\n",
           Glob.sos->length, Glob.usable->length, Glob.demods->length,
           (int) clause_store_length(Glob.disabled));
    print_separator(stdout, "end of process initial clauses", TRUE);
    print_separator(stdout, "CLAUSES FOR SEARCH", TRUE);
  }

  if (flag(Opt->print_initial_clauses)) {
    printf("\n%% Clauses after checkpoint restore:\n");
    fwrite_clause_clist(stdout, Glob.usable, CL_FORM_STD);
    fwrite_clause_clist(stdout, Glob.sos, CL_FORM_STD);
    fwrite_demod_clist(stdout, Glob.demods, CL_FORM_STD);
  }
  if (!flag(Opt->quiet) && Glob.hints->length > 0) {
    int redundant = redundant_hints();
    printf("\n%% %d hints (%d processed, %d redundant).\n",
           Glob.hints->length - redundant, Glob.hints->length, redundant);
  }

  if (!flag(Opt->quiet))
    print_separator(stdout, "end of clauses for search", TRUE);
}  /* load_checkpoint_into_loop */

/*************
 *
 *   set_progress_callback()
 *
 *   Install a callback that search() invokes at key preprocessing
 *   stages and periodically during the main loop.  Used by the
 *   -cores N scheduler for shared-memory progress reporting.
 *
 *************/

/* PUBLIC */
void set_progress_callback(Search_progress_fn fn)
{
  Progress_callback = fn;
}  /* set_progress_callback */

/*************
 *
 *   search()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
Prover_results search(Prover_input p)
{
  int return_code = setjmp(Jump_env);
  if (return_code != 0) {
    // we just landed from longjmp(); fix return code and return
    if (!Opt || !flag(Opt->quiet))
      print_separator(stdout, "end of search", TRUE);
    Glob.return_code = (return_code == INT_MAX ? 0 : return_code);
    fatal_setjmp();  /* This makes longjmps cause a fatal_error. */
    return collect_prover_results(p->xproofs);
  }
  else {
    // search for a proof

    if (!flag(p->options->quiet))
      print_separator(stdout, "PROCESS INITIAL CLAUSES", TRUE);

    Opt = p->options;          // put options into a global variable
    if (Deferred_terminal_stats != NULL) {
      fclose(Deferred_terminal_stats);
      Deferred_terminal_stats = NULL;
    }
    Terminal_stats_frozen = FALSE;
    Terminal_compact_indexes_released = FALSE;
    Terminal_hint_index_released = FALSE;
    Current_inference_source = INFER_SOURCE_OTHER;
    collective_reset_state();
    Simplifier_epoch = 1;
    Rewrite_epoch = 1;
    dense_passive_set_rewrite_epoch(Rewrite_epoch);
    Rewrite_refresh_hot_cursor = 0;
    Rewrite_refresh_general_cursor = 0;
    Rewrite_interreduce_cursor = 0;
    Rewrite_refresh_hot_streak = 0;
    Rewrite_interreduce_streak = 0;
    Rewrite_refresh_inference_streak = 0;
    Rewrite_drain_mode = FALSE;
    Rewrite_drain_streak = 0;
    Rewrite_repair_depth = 0;
    Resume_rewrite_cursor_ids = FALSE;
    Resume_rewrite_hot_cursor_id = 0;
    Resume_rewrite_general_cursor_id = 0;
    Resume_rewrite_interreduce_cursor_id = 0;
    if (flag(Opt->collective_promising_scheduler) &&
        !str_ident(stringparm1(Opt->inference_frontier), "collective"))
      fatal_error("collective_promising_scheduler requires inference_frontier=collective");
    if (str_ident(stringparm1(Opt->collective_scheduler), "balanced_hint") &&
        !str_ident(stringparm1(Opt->inference_frontier), "collective"))
      fatal_error("collective_scheduler=balanced_hint requires inference_frontier=collective");
    if (!str_ident(stringparm1(Opt->discount_demodulation), "selected") &&
        !discount_mode())
      fatal_error("eager DISCOUNT demodulation requires search_loop=discount");
    if (flag(Opt->compact_term_sharing_stats) &&
        !(compact_otter_bank_mode() ||
          flag(Opt->compact_unit_subsumption_audit) ||
          flag(Opt->compact_otter_unit_index) ||
          flag(Opt->compact_back_demod_audit) ||
          compact_back_demod_authoritative_mode()))
      fatal_error("compact_term_sharing_stats requires a compact term index");
    if (compact_otter_audit_mode()) {
      if (discount_mode())
        fatal_error("compact_otter_audit requires search_loop=otter");
      if (flag(Opt->eval_rewrite))
        fatal_error("compact_otter_audit is incompatible with eval_rewrite");
      if (!str_ident(stringparm1(Opt->inference_frontier), "clauses"))
        fatal_error("compact_otter_audit requires inference_frontier=clauses");
      if (p->resume_dir != NULL)
        fatal_error("compact_otter_audit does not yet support checkpoint resume");
    }
    if (compact_otter_demod_mode()) {
      if (discount_mode())
        fatal_error("compact_otter_demodulation requires search_loop=otter");
      if (flag(Opt->eval_rewrite))
        fatal_error("compact_otter_demodulation is incompatible with eval_rewrite");
      if (!str_ident(stringparm1(Opt->inference_frontier), "clauses"))
        fatal_error("compact_otter_demodulation requires inference_frontier=clauses");
      if (compact_otter_audit_mode())
        fatal_error("compact_otter_demodulation and compact_otter_audit are mutually exclusive");
    }
    if (flag(Opt->compact_unit_subsumption_audit)) {
      if (discount_mode())
        fatal_error("compact_unit_subsumption_audit requires search_loop=otter");
      if (!str_ident(stringparm1(Opt->inference_frontier), "clauses"))
        fatal_error("compact_unit_subsumption_audit requires inference_frontier=clauses");
      if (flag(Opt->ancestor_subsume))
        fatal_error("compact_unit_subsumption_audit does not yet support ancestor_subsume");
      if (p->resume_dir != NULL)
        fatal_error("compact_unit_subsumption_audit does not support checkpoint resume");
    }
    if (flag(Opt->compact_otter_unit_index)) {
      if (discount_mode())
        fatal_error("compact_otter_unit_index requires search_loop=otter");
      if (!str_ident(stringparm1(Opt->inference_frontier), "clauses"))
        fatal_error("compact_otter_unit_index requires inference_frontier=clauses");
      if (flag(Opt->ancestor_subsume))
        fatal_error("compact_otter_unit_index does not yet support ancestor_subsume");
      if (flag(Opt->unit_deletion))
        fatal_error("compact_otter_unit_index does not yet support unit_deletion");
      if (flag(Opt->compact_unit_subsumption_audit))
        fatal_error("compact_otter_unit_index and its audit are mutually exclusive");
    }
    if (flag(Opt->compact_back_demod_audit)) {
      if (discount_mode())
        fatal_error("compact_back_demod_audit requires search_loop=otter");
      if (!str_ident(stringparm1(Opt->inference_frontier), "clauses"))
        fatal_error("compact_back_demod_audit requires inference_frontier=clauses");
      if (!flag(Opt->back_demod))
        fatal_error("compact_back_demod_audit requires set(back_demod)");
      if (p->resume_dir != NULL)
        fatal_error("compact_back_demod_audit does not support checkpoint resume");
    }
    if (flag(Opt->compact_otter_back_demod_index)) {
      if (discount_mode())
        fatal_error("compact_otter_back_demod_index requires search_loop=otter");
      if (!str_ident(stringparm1(Opt->inference_frontier), "clauses"))
        fatal_error("compact_otter_back_demod_index requires inference_frontier=clauses");
      if (flag(Opt->compact_back_demod_audit))
        fatal_error("compact_otter_back_demod_index and its audit are mutually exclusive");
    }
    if (flag(Opt->compact_nonunit_subsumption_audit)) {
      if (discount_mode())
        fatal_error("compact_nonunit_subsumption_audit requires search_loop=otter");
      if (!str_ident(stringparm1(Opt->inference_frontier), "clauses"))
        fatal_error("compact_nonunit_subsumption_audit requires inference_frontier=clauses");
      if (flag(Opt->ancestor_subsume))
        fatal_error("compact_nonunit_subsumption_audit does not support ancestor_subsume");
      if (p->resume_dir != NULL)
        fatal_error("compact_nonunit_subsumption_audit does not support checkpoint resume");
    }
    if (flag(Opt->compact_otter_nonunit_index)) {
      if (discount_mode())
        fatal_error("compact_otter_nonunit_index requires search_loop=otter");
      if (!str_ident(stringparm1(Opt->inference_frontier), "clauses"))
        fatal_error("compact_otter_nonunit_index requires inference_frontier=clauses");
      if (!flag(Opt->compact_otter_unit_index))
        fatal_error("compact_otter_nonunit_index requires compact_otter_unit_index");
      if (flag(Opt->unit_deletion))
        fatal_error("compact_otter_nonunit_index does not support unit_deletion");
      if (flag(Opt->ancestor_subsume))
        fatal_error("compact_otter_nonunit_index does not support ancestor_subsume");
      if (flag(Opt->compact_nonunit_subsumption_audit))
        fatal_error("compact_otter_nonunit_index and its audit are mutually exclusive");
    }
    if (!discount_mode() &&
        str_ident(stringparm1(Opt->passive_store), "dense")) {
      if (!compact_otter_passive_mode())
        fatal_error("OTTER passive_store=dense requires all four authoritative compact indexes");
      if (str_ident(stringparm1(Opt->ancestor_store), "off"))
        fatal_error("compact OTTER passive_store=dense requires an ancestor store");
      if (parm(Opt->sos_limit) != -1)
        fatal_error("compact OTTER passive_store=dense requires sos_limit=-1");
      if (!flag(Opt->process_initial_sos))
        fatal_error("compact OTTER passive_store=dense requires process_initial_sos");
    }
    if (maximum_discount_demod_mode()) {
      if (!dense_passive_mode())
        fatal_error(eager_legacy_demod_mode() ?
          "discount_demodulation=eager_legacy requires passive_store=dense" :
          "discount_demodulation=eager_interreduced requires passive_store=dense");
      if (!flag(Opt->back_demod))
        fatal_error(eager_legacy_demod_mode() ?
          "discount_demodulation=eager_legacy requires set(back_demod)" :
          "discount_demodulation=eager_interreduced requires set(back_demod)");
      if (flag(Opt->eval_rewrite))
        fatal_error(eager_legacy_demod_mode() ?
          "discount_demodulation=eager_legacy is incompatible with eval_rewrite" :
          "discount_demodulation=eager_interreduced is incompatible with eval_rewrite");
      if (eager_interreduced_demod_mode() &&
          parm(Opt->rewrite_refresh_low_water) >=
            parm(Opt->rewrite_refresh_high_water))
        fatal_error("rewrite_refresh_low_water must be below high_water");
    }
    if (str_ident(stringparm1(Opt->inference_frontier), "collective")) {
      if (!discount_mode())
	fatal_error("inference_frontier=collective requires search_loop=discount");
      if (str_ident(stringparm1(Opt->ancestor_store), "off"))
	fatal_error("inference_frontier=collective requires an ancestor store");
      if (flag(Opt->collective_promising_scheduler) &&
          !flag(Opt->collective_promising_candidates))
        fatal_error("collective_promising_scheduler requires collective_promising_candidates");
      if (str_ident(stringparm1(Opt->collective_scheduler), "balanced_hint")) {
        unsigned reserve = collective_balanced_max_activation_descriptors();
        int high = parm(Opt->collective_descriptor_high_water);
        int low = parm(Opt->collective_descriptor_low_water);
        if (!dense_passive_mode())
          fatal_error("collective_scheduler=balanced_hint requires passive_store=dense");
        if (low >= high)
          fatal_error("collective_descriptor_low_water must be below high_water");
        if (parm(Opt->collective_candidate_window) >
              parm(Opt->collective_candidate_cache))
          fatal_error("collective_candidate_window must not exceed candidate_cache");
        if (reserve > (unsigned) high)
          fatal_error("collective descriptor high-water cannot admit one activation");
        if (flag(Opt->collective_hint_probes) ||
            flag(Opt->collective_promising_candidates) ||
            flag(Opt->collective_promising_scheduler))
          fatal_error("balanced_hint replaces the legacy collective discovery aids");
        if ((flag(Opt->paramodulation) &&
             parm(Opt->collective_paramod_share) == 0) ||
            (flag(Opt->pos_hyper_resolution) &&
             parm(Opt->collective_pos_hyper_share) == 0) ||
            (flag(Opt->neg_hyper_resolution) &&
             parm(Opt->collective_neg_hyper_share) == 0))
          fatal_error("each enabled balanced collective rule needs a nonzero lane share");
      }
      /* A delayed parent may already be disabled when a checkpoint is
	 resumed, so collective checkpoints necessarily include ancestors. */
      if (!flag(Opt->checkpoint_ancestors))
	set_flag(Opt->checkpoint_ancestors, !flag(Opt->quiet));
    }
    Glob.initialized = TRUE;   // this signifies that Glob is being used
    Glob.has_goals = p->has_goals;  // for SZS status: Theorem vs Unsatisfiable
    Glob.has_neg_conj = p->has_neg_conj; // CNF negated_conjecture (refutation)
    Glob.problem_name = p->problem_name;  // for SZS "for <name>" suffix

    enable_sigusr1_report();   // SIGUSR1 now safe (Opt/Glob ready)
    enable_sigusr2_checkpoint(); // SIGUSR2 now safe (Opt/Glob ready)

    // Arm wall-clock timeout via SIGALRM (replaces polling in hot loop)
    setup_timeout_signal(parm(Opt->max_seconds));

#ifdef __EMSCRIPTEN__
    {
      int wasm_max_sec = parm(Opt->max_seconds);
      Wasm_deadline_ms = (wasm_max_sec > 0)
        ? emscripten_get_now() + wasm_max_sec * 1000.0
        : 0;
    }
#endif

    // Enable comma formatting for statistics if requested
    if (flag(Opt->comma_stats))
      set_comma_formatting(TRUE);

    // Set candidate limits for index queries
    set_candidate_limits(parm(Opt->candidate_warn_limit),
                         parm(Opt->candidate_hard_limit));

    To_trace_id = parm(Opt->cl_to_trace);

    Glob.start_time  = user_seconds();
    Glob.start_ticks = bogo_ticks();

    if (flag(Opt->sort_initial_sos) && plist_count(p->sos) <= 100)
      p->sos = sort_plist(p->sos,
			  (Ordertype (*)(void*, void*)) clause_compare_m4);

    // Move clauses and term lists into Glob; do not assign IDs to clauses.

    Glob.usable  = move_clauses_to_clist(p->usable, "usable", FALSE);
    Glob.sos     = move_clauses_to_clist(p->sos, "sos", FALSE);
    Glob.demods  = move_clauses_to_clist(p->demods,"demodulators",FALSE);
    Glob.hints   = move_clauses_to_clist(p->hints, "hints", FALSE);

    /* Do not let parsed hint term forests overlap the packed feature bank.
       The indexing pass below materializes one hint at a time.  This is an
       important peak-memory property for large AIM hint files, not merely a
       steady-state optimization. */
    if (packed_hint_bank_mode()) {
      Clist_pos hp;
      for (hp = Glob.hints->first; hp != NULL; hp = hp->next) {
        if (compress_clause(hp->c) == CLAUSE_COMPRESS_INVALID)
          fatal_error("search: cannot precompress packed hint");
      }
    }

    Glob.weights          = tlist_copy(p->weights);
    Glob.resonators       = tlist_copy(p->resonators);
    Glob.kbo_weights      = tlist_copy(p->kbo_weights);
    Glob.actions          = tlist_copy(p->actions);
    Glob.interps          = tlist_copy(p->interps);
    Glob.given_selection  = tlist_copy(p->given_selection);
    Glob.keep_rules       = tlist_copy(p->keep_rules);
    Glob.delete_rules     = tlist_copy(p->delete_rules);

    // Allocate auxiliary clause lists.

    Glob.limbo    = clist_init("limbo");
    Glob.disabled = new_disabled_store();
    Disabled_checkpoint_omitted = 0;
    if (Rewrite_only_rules != NULL)
      fatal_error("search: previous rewrite-only store was not released");
    Rewrite_only_rules = maximum_discount_demod_mode() ?
      rewrite_only_store_init() : NULL;
    if (Compact_terms != NULL)
      fatal_error("search: previous compact term pool was not released");
    Compact_terms =
      (eager_interreduced_demod_mode() || compact_otter_bank_mode() ||
       flag(Opt->compact_unit_subsumption_audit) ||
       flag(Opt->compact_otter_unit_index) ||
       flag(Opt->compact_back_demod_audit) ||
       compact_back_demod_authoritative_mode()) ?
      compact_term_pool_init() : NULL;
    Compact_term_next_reclaim_serialization = 0;
    Compact_term_reclaim_cooldown_skips = 0;
    Compact_term_reclaim_deferrals = 0;
    Compact_term_last_predicted_reclaim = 0;
    if (Compact_terms != NULL && flag(Opt->compact_term_sharing_stats))
      compact_term_pool_enable_sharing_profile(Compact_terms);
    if (Compact_rewrite_rules != NULL)
      fatal_error("search: previous compact rewrite bank was not released");
    Compact_rewrite_rules =
      (eager_interreduced_demod_mode() || compact_otter_bank_mode()) ?
      compact_rewrite_init_with_pool(Compact_terms) : NULL;
    Glob.empties  = NULL;
    cold_passive_store_free(Dense_body_store);
    Dense_body_store = NULL;
    Dense_arena_bytes_reclaimed = 0;
    if (dense_passive_mode()) {
      if (str_ident(stringparm1(Opt->ancestor_store), "off"))
        fatal_error("passive_store=dense requires an ancestor store");
      if (parm(Opt->sos_limit) != -1)
        fatal_error("passive_store=dense currently requires sos_limit=-1");
      if (compact_otter_passive_mode())
        configure_dense_passive(TRUE, archive_compact_otter_passive,
                                activate_compact_otter_passive);
      else {
        Dense_body_store = new_dense_body_store();
        configure_dense_passive(TRUE, archive_dense_passive,
                                activate_dense_passive);
      }
    }
    else
      configure_dense_passive(FALSE, NULL, NULL);
    compact_passive_cache_init(compact_otter_passive_mode() ?
                               (unsigned) parm(Opt->compact_passive_cache) : 0);

    if (p->resume_dir) {
      // Resume from checkpoint.  Minimal setup here - the actual checkpoint
      // data is loaded inside the main loop's first iteration, at the same
      // program point as the checkpoint save (before make_inferences).
      // saved_input.txt has clear(auto_inference)/clear(auto_process) so
      // init_search won't re-run auto-mode on the empty clause lists.

      Resume_dir = p->resume_dir;  // set BEFORE init_search so it can
                                    // skip duplicate given_selection/delete_rules
      resume_load_precedence(p->resume_dir);  // set preliminary precedence
                                               // BEFORE init_search so
                                               // symbol_order uses it
      basic_clause_properties(Glob.sos, Glob.usable);
      init_search();
      Load_checkpoint = TRUE;
    }
    else {
      // Normal path.

      if (flag(Opt->print_initial_clauses)) {
        printf("\n%% Clauses before input processing:\n");
        fwrite_clause_clist(stdout, Glob.usable,  CL_FORM_STD);
        fwrite_clause_clist(stdout, Glob.sos,     CL_FORM_STD);
        fwrite_clause_clist(stdout, Glob.demods,  CL_FORM_STD);
        if (Glob.hints->length > 0)
	  printf("\n%% %d hints input.\n", Glob.hints->length);
      }

      // Predicate elimination (may add to sos and move clauses to disabled)

      if (Progress_callback)
        Progress_callback(STAGE_PRED_ELIM, (int) Stats.given, (int) Stats.kept,
                          (int) Stats.sos_size, (int) Stats.usable_size,
                          (int) megs_malloced());

      if (flag(p->options->predicate_elim) && clist_empty(Glob.usable)) {
        if (flag(Opt->fast_pred_elim))
          set_pred_elim_timeout(3);  /* 3s limit */
        if (!flag(Opt->quiet))
          print_separator(stdout, "PREDICATE ELIMINATION", TRUE);
        predicate_elimination(Glob.sos, Glob.disabled, !flag(Opt->quiet));
        compress_retained_store(Glob.disabled);
        if (!flag(Opt->quiet))
          print_separator(stdout, "end predicate elimination", TRUE);
      }

      if (Progress_callback)
        Progress_callback(STAGE_BASIC_PROPS, (int) Stats.given, (int) Stats.kept,
                          (int) Stats.sos_size, (int) Stats.usable_size,
                          (int) megs_malloced());

      basic_clause_properties(Glob.sos, Glob.usable);

      // Possible special treatment for denials (negative in Horn sets)

      if (flag(Opt->auto_denials))
        auto_denials(Glob.sos, Glob.usable, Opt);

      if (Progress_callback)
        Progress_callback(STAGE_INIT_SEARCH, (int) Stats.given, (int) Stats.kept,
                          (int) Stats.sos_size, (int) Stats.usable_size,
                          (int) megs_malloced());

      init_search();  // init clocks, ordering, auto-mode, init packages

      if (Progress_callback)
        Progress_callback(STAGE_INDEX_INITIAL, (int) Stats.given, (int) Stats.kept,
                          (int) Stats.sos_size, (int) Stats.usable_size,
                          (int) megs_malloced());

      index_and_process_initial_clauses();
      if (flag(Opt->print_derivations))
	get_hit_list();
    }

    if (!flag(Opt->quiet))
      print_separator(stdout, "SEARCH", TRUE);

    if (!flag(Opt->quiet))
      printf("\n%% Starting search at %.2f seconds.\n", user_seconds());
    fflush(stdout);
    Glob.start_time = user_seconds();
    Glob.searching = TRUE;

    /* Signal that preprocessing is complete and search is starting. */
    if (Progress_callback)
      Progress_callback(STAGE_SEARCHING, 0, (int) Stats.kept,
                        (int) Stats.sos_size, (int) Stats.usable_size,
                        (int) megs_malloced());

    if (parm(Opt->checkpoint_minutes) > 0) {
      int mins = parm(Opt->checkpoint_minutes);
      if (mins < 1) {
        fprintf(stderr,
          "WARNING: checkpoint_minutes=%d too small, using 1.\n", mins);
        assign_parm(Opt->checkpoint_minutes, 1, TRUE);
      }
      Last_auto_ckpt_time = time(NULL);
    }

    /* Set wall-clock deadline for clash_recurse() timeout.
       Prevents indefinite hangs inside hyper/UR/binary resolution. */
    if (parm(Opt->max_seconds) >= 0)
      set_clash_deadline(time(NULL) + (time_t)parm(Opt->max_seconds));


    // ****************************** Main Loop ******************************
    //
    // Checkpoint save and load happen at the TOP of the loop, BEFORE
    // make_inferences().  At this point limbo is empty (drained by the
    // previous iteration's limbo_process).  Save and load at the same
    // program point guarantees bit-identical state on resume.

    while (Load_checkpoint || inferences_to_make()) {

      unsigned long long given_before_iteration = Stats.given;

#ifndef __EMSCRIPTEN__
      // Checkpoint save triggers (periodic / SIGUSR2).
      // Skip on the first iteration if we are loading a checkpoint.
      if (!Load_checkpoint) {

        /* Deterministic one-shot checkpointing is useful for regression
           tests and reproducible handoffs.  Disable the option before
           writing so saved_input.txt cannot retrigger it on resume. */
        if (parm(Opt->checkpoint_given) >= 0 &&
            Stats.given >= (unsigned long long)
              parm(Opt->checkpoint_given)) {
          fprintf(stderr, "\nScheduled checkpoint at given #%llu...\n",
                  Stats.given);
          fflush(stderr);
          assign_parm(Opt->checkpoint_given, -1, TRUE);
          write_checkpoint();
          fprintf(stderr, "\nCheckpoint saved at given #%llu.\n",
                  Stats.given);
          fflush(stderr);
          if (flag(Opt->checkpoint_exit))
            done_with_search(CHECKPOINT_EXIT);
        }

        /* Deterministically exercise serialization of live raw candidate
           windows.  This is also useful for bounded operational handoffs
           that want a checkpoint only after discovery work is resident. */
        if (parm(Opt->checkpoint_candidate_pool) >= 0 &&
            Collective_candidate_heap_count != 0 &&
            Collective_candidate_heap_count >=
              (size_t) parm(Opt->checkpoint_candidate_pool)) {
          fprintf(stderr,
                  "\nScheduled checkpoint at candidate-pool occupancy %llu...\n",
                  (unsigned long long) Collective_candidate_heap_count);
          fflush(stderr);
          assign_parm(Opt->checkpoint_candidate_pool, -1, TRUE);
          write_checkpoint();
          fprintf(stderr,
                  "\nCheckpoint saved with %llu candidate-pool entries.\n",
                  (unsigned long long) Collective_candidate_heap_count);
          fflush(stderr);
          if (flag(Opt->checkpoint_exit))
            done_with_search(CHECKPOINT_EXIT);
        }

        if (parm(Opt->checkpoint_discovery_promotions) >= 0 &&
            Collective_candidate_heap_count != 0 &&
            Stats.collective_discovery_promotions >=
              (unsigned long long)
                parm(Opt->checkpoint_discovery_promotions)) {
          fprintf(stderr,
                  "\nScheduled checkpoint after %llu discovery promotions...\n",
                  Stats.collective_discovery_promotions);
          fflush(stderr);
          assign_parm(Opt->checkpoint_discovery_promotions, -1, TRUE);
          write_checkpoint();
          fprintf(stderr,
                  "\nCheckpoint saved with discovery work ahead of fair cursor.\n");
          fflush(stderr);
          if (flag(Opt->checkpoint_exit))
            done_with_search(CHECKPOINT_EXIT);
        }

        // Check for periodic automatic checkpoint
        if (parm(Opt->checkpoint_minutes) > 0) {
          time_t now = time(NULL);
          if (now - Last_auto_ckpt_time >=
              (time_t)parm(Opt->checkpoint_minutes) * 60) {
            char auto_dir[512];
            fprintf(stderr, "\nPeriodic checkpoint at given #%llu...\n",
                    Stats.given);
            fflush(stderr);
            write_checkpoint();
            fprintf(stderr, "\nCheckpoint saved at given #%llu.\n",
                    Stats.given);
            fflush(stderr);
            Last_auto_ckpt_time = now;
            snprintf(auto_dir, sizeof(auto_dir),
                     "prover9_%d_ckpt_%llu", getpid(), Stats.given);
            record_auto_checkpoint(auto_dir);
            if (flag(Opt->checkpoint_exit))
              done_with_search(CHECKPOINT_EXIT);
          }
        }

        // Check for SIGUSR2 checkpoint request
        if (checkpoint_requested()) {
          fprintf(stderr, "\nCheckpoint requested at given #%llu...\n",
                  Stats.given);
          fflush(stderr);
          write_checkpoint();
          fprintf(stderr, "\nCheckpoint saved at given #%llu.\n",
                  Stats.given);
          fflush(stderr);
          clear_checkpoint_request();
          if (flag(Opt->checkpoint_exit))
            done_with_search(CHECKPOINT_EXIT);
        }


      }  /* !Load_checkpoint */
#endif /* !__EMSCRIPTEN__ */


      // Checkpoint load (resume - first iteration only).
      if (Load_checkpoint) {
        load_checkpoint_into_loop();
        Load_checkpoint = FALSE;
      }

      // make_inferences: each inferred clause is cl_processed, which
      // does forward demodulation and subsumption; if the clause is kept
      // it is put on the Limbo list, and it is indexed so that it can be
      // used immediately with subsequent newly inferred clauses.


      make_inferences();

#ifdef __EMSCRIPTEN__
      if (Wasm_deadline_ms > 0 && emscripten_get_now() > Wasm_deadline_ms)
        done_with_search(MAX_SECONDS_EXIT);
#endif

      if (Progress_callback && Stats.given != given_before_iteration &&
	  Stats.given % 100 == 0)
        Progress_callback(STAGE_SEARCHING, (int) Stats.given, (int) Stats.kept,
                          (int) Stats.sos_size, (int) Stats.usable_size,
                          (int) megs_malloced());

      /* Periodic hint expiry sweep */
      if (Stats.given != given_before_iteration &&
	  parm(Opt->hint_expiry) > 0 &&
	  Stats.given % (unsigned long long) parm(Opt->hint_sweep_interval) == 0) {
	int expired = expire_old_hints(Stats.given,
				       (unsigned long long) parm(Opt->hint_expiry),
				       parm(Opt->hint_expiry_min),
				       Glob.hints);
	if (expired > 0)
	  fprintf(stderr, "%% Expired %d hints at given #%llu (%d active).\n",
		  expired, Stats.given, active_hints());
      }

      // limbo_process: this applies back subsumption, back demodulation,
      // and other operations that can disable clauses.  Limbo clauses
      // are moved to the Sos list.

      limbo_process(FALSE);
      collective_note_candidate_cache_peak();

    }  // ************************ end of main loop ************************

    if (Progress_callback)
      Progress_callback(STAGE_DONE, (int) Stats.given, (int) Stats.kept,
                        (int) Stats.sos_size, (int) Stats.usable_size,
                        (int) megs_malloced());

    fprint_all_stats(stdout, Opt ? stringparm1(Opt->stats) : "lots");
    if (!flag(Opt->quiet))
      print_separator(stdout, "end of search", TRUE);
    fatal_setjmp();  /* This makes longjmps cause a fatal_error. */
    Glob.return_code = Glob.empties ? MAX_PROOFS_EXIT : SOS_EMPTY_EXIT;
    return collect_prover_results(p->xproofs);
  }
}  /* search */

#ifndef __EMSCRIPTEN__

/*************
 *
 *   forking_search()
 *
 *************/

/* DOCUMENTATION
This is similar to search(), except that a child process is created
to do the search, and the child sends its results to the parent on
a pipe.

<P>
The parameters and results are the same as search().
As in search(), the Plists lists of objets (the parameters) are not changed.
*/

/* PUBLIC */
Prover_results forking_search(Prover_input input)
{
  Prover_results results;

  int rc;
  int fd[2];          /* pipe for child -> parent data */

  rc = pipe(fd);
  if (rc != 0) {
    perror("");
    fatal_error("forking_search: pipe creation failed");
  }

  fflush(stdout);
  fflush(stderr);
  rc = fork();
  if (rc < 0) {
    perror("");
    fatal_error("forking_search: fork failed");
  }

  /* kludge to get labels that might be introduced by child into symtab */
  (void) str_to_sn("flip_matches_hint", 0);

  if (rc == 0) {

    /*********************************************************************/
    /* This is the child process.  Search, send results to parent, exit. */
    /*********************************************************************/

    int to_parent = fd[1];  /* fd for writing data to parent */
    close(fd[0]);           /* close "read" end of pipe */

    fprintf(stdout,"\nChild search process %d started.\n", my_process_id());

    /* Remember how many symbols are in the symbol table.  If new symbols
       are introduced during the search, we have to send them to the
       parent so that clauses sent to the parent can be reconstructed.
    */

    mark_for_new_symbols();

    /* search */
    
    results = search(input);

    /* send results to the parent */

    {
      /* Format of data (all integers) sent to parent:
	---------------------- 
	nymber-of-new-symbols
	  symnum
	  arity
          ...
	number-of-proofs
	  number-of-steps
	    [clauses-in-proof]
	  number-of-steps
	    [clauses-in-proof]
          ...
        [same for xproofs]

	stats  (MAX_STATS of them)
	user_milliseconds
	system_milliseconds
	return_code

      */

      Ibuffer ibuf = ibuf_init();
      Plist p, a;
      I2list new_symbols, q;

      /* collect and write new_symbols */

      new_symbols = new_symbols_since_mark();
      ibuf_write(ibuf, i2list_count(new_symbols));
      for (q = new_symbols; q; q = q->next) {
	ibuf_write(ibuf, q->i);
	ibuf_write(ibuf, q->j);
      }
      zap_i2list(new_symbols);

      /* collect and write proofs */

      ibuf_write(ibuf, plist_count(results->proofs));  /* number of proofs */
      for (p = results->proofs; p; p = p->next) {
	ibuf_write(ibuf, plist_count(p->v));  /* steps in this proof */
	for (a = p->v; a; a = a->next) {
	  put_clause_to_ibuf(ibuf, a->v);
	}
      }
      
      /* collect and write xproofs */

      ibuf_write(ibuf, plist_count(results->xproofs));  /* number of xproofs */
      for (p = results->xproofs; p; p = p->next) {
	ibuf_write(ibuf, plist_count(p->v));  /* steps in this proof */
	for (a = p->v; a; a = a->next)
	  put_clause_to_ibuf(ibuf, a->v);
      }
      
      {
	/* collect stats (shortcut: handle stats struct as sequence of ints) */
	int *x = (void *) &(results->stats);
	int n = sizeof(struct prover_stats) / sizeof(int);
	int i;
	for (i = 0; i < n; i++)
	  ibuf_write(ibuf, x[i]);
      }

      /* collect clocks */
      ibuf_write(ibuf, (int) (results->user_seconds * 1000));
      ibuf_write(ibuf, (int) (results->system_seconds * 1000));
      /* collect return_code */
      ibuf_write(ibuf, results->return_code);

      /* write the data to the pipe */

      rc = write(to_parent,
		 ibuf_buffer(ibuf),
		 ibuf_length(ibuf) * sizeof(int));
      if (rc == -1) {
	perror("");
	fatal_error("forking_search, write error");
      }
      else if (rc != ibuf_length(ibuf) * sizeof(int))
	fatal_error("forking_search, incomplete write from child to parent");

      rc = close(to_parent);
      
      ibuf_free(ibuf);  /* not necessary, because we're going to exit now */
    }

    /* child exits */

    exit_with_message(stdout, results->return_code);
    
    return NULL;  /* won't happen */

  }  /* end of child code */

  else {

    /*********************************************************************/
    /* This is the parent process.  Get results from child, then return. */
    /*********************************************************************/

    int from_child = fd[0];  /* fd for reading data from child */
    close(fd[1]);            /* close "write" end of pipe */

    /* read results from child (read waits until data are available) */

    {
      Ibuffer ibuf = fd_read_to_ibuf(from_child);
      int num_proofs, num_steps, i, j;
      int num_new_symbols;
      I2list new_syms = NULL;

      results = safe_calloc(1, sizeof(struct prover_results));
      
      /* read new_symbols */

      num_new_symbols = ibuf_read(ibuf);
      for (i = 0; i < num_new_symbols; i++) {
	int symnum = ibuf_read(ibuf);
	int arity = ibuf_read(ibuf);
	new_syms = i2list_append(new_syms, symnum, arity);
      }
      add_new_symbols(new_syms);  /* add new symbols to symbol table */
      zap_i2list(new_syms);

      /* read proofs */

      num_proofs = ibuf_read(ibuf);
      for (i = 0; i < num_proofs; i++) {
	Plist proof = NULL;
	num_steps = ibuf_read(ibuf);
	for (j = 0; j < num_steps; j++) {
	  Topform c = get_clause_from_ibuf(ibuf);
	  proof = plist_prepend(proof, c);  /* build backward, reverse later */
	}
	results->proofs = plist_append(results->proofs, reverse_plist(proof));
      }

      /* read xproofs */

      num_proofs = ibuf_read(ibuf);
      for (i = 0; i < num_proofs; i++) {
	Plist proof = NULL;
	num_steps = ibuf_read(ibuf);
	for (j = 0; j < num_steps; j++) {
	  Topform c = get_clause_from_ibuf(ibuf);
	  proof = plist_prepend(proof, c);  /* build backward, reverse later */
	}
	results->xproofs = plist_append(results->xproofs,reverse_plist(proof));
      }

      {
	/* read stats (shortcut: handle stats struct as sequence of ints) */
	int *x = (void *) &(results->stats);
	int n = sizeof(struct prover_stats) / sizeof(int);
	int i;
	for (i = 0; i < n; i++)
	  x[i] = ibuf_read(ibuf);
      }

      /* read clocks */
      results->user_seconds = ibuf_read(ibuf) / 1000.0;
      results->system_seconds = ibuf_read(ibuf) / 1000.0;
      /* read return_code */
      results->return_code = ibuf_read(ibuf);
    }

    /* Wait for child to exit and get the exit code.  We should not
       have to wait long, because we already have its results. */

    {
      int child_status, child_exit_code;
      wait(&child_status);
      if (!WIFEXITED(child_status))
	fatal_error("forking_search: child terminated abnormally");
      child_exit_code = WEXITSTATUS(child_status);
      results->return_code = child_exit_code;
    }

    rc = close(from_child);

    return results;
  }  /* end of parent code */
}  /* forking_search */

#endif /* !__EMSCRIPTEN__ */
