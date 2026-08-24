#include "../ladr/ladr.h"

static int Failures;

#define CHECK(test, message) do {                                      \
  if (!(test)) {                                                       \
    fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__);   \
    Failures++;                                                        \
  }                                                                    \
} while (0)

static void no_op_demod(Topform clause, int step_limit, int increase_limit,
                        BOOL print, BOOL lex_order_vars)
{
  (void) clause;
  (void) step_limit;
  (void) increase_limit;
  (void) print;
  (void) lex_order_vars;
}

static void run_case(BOOL packed, BOOL better, BOOL fast, int bsub)
{
  Topform hint = parse_clause_from_string("p(f(a)).");
  Topform candidate = parse_clause_from_string("p(f(a)).");
  Topform match;
  unsigned long long epoch_before, checks_before, checks_after;
  unsigned long long nb, rb, tb, nb_before, rb_before, tb_before;
  double adjusted = -1;
  BOOL flipped = TRUE;
  Attribute candidate_attributes = candidate->attributes;

  hint->attributes = set_int_attribute(hint->attributes, bsub, 7);
  init_hints(ORDINARY_UNIF, bsub, FALSE, FALSE, 2, packed, better, fast, 2048,
             327680, 0, 8, NULL);
  index_hint(hint);
  if (packed)
    CHECK(packed_hint_by_id(hint->id) == hint,
          "packed hint ID lookup returns the stable owner");
  else
    CHECK(packed_hint_by_id(hint->id) == NULL,
          "packed hint ID lookup is disabled outside packed mode");
  epoch_before = hint_state_epoch();
  packed_hint_index_stats(&nb, &rb, &tb, &checks_before);
  nb_before = nb;
  rb_before = rb;
  tb_before = tb;

  match = preview_weight_with_hints(candidate, 42.0, TRUE, FALSE,
                                    &adjusted, &flipped);
  packed_hint_index_stats(&nb, &rb, &tb, &checks_after);
  CHECK(match == hint, "preview returns authoritative matcher identity");
  CHECK(adjusted == 7.0, "preview applies bsub_wt");
  CHECK(!flipped, "ordinary preview is not a flip match");
  CHECK(candidate->matching_hint == NULL, "preview does not assign matcher");
  CHECK(candidate->weight == 0, "preview does not assign candidate weight");
  CHECK(candidate->attributes == candidate_attributes,
        "preview does not assign candidate attributes");
  CHECK(hint->weight == 0 && hint->last_matched_given == 0,
        "preview does not update hint match state");
  CHECK(hint_state_epoch() == epoch_before,
        "preview does not advance hint epoch");
  CHECK(checks_after == checks_before,
        "preview does not charge packed candidate checks");
  CHECK(nb == nb_before && rb == rb_before && tb == tb_before,
        "preview does not change packed index allocations");

  candidate->weight = 42.0;
  adjust_weight_with_hints(candidate, TRUE, FALSE);
  CHECK(candidate->matching_hint == match,
        "preview and authoritative matcher identities agree");
  CHECK(candidate->weight == adjusted,
        "preview and authoritative adjusted weights agree");

  unindex_hint(hint);
  if (packed)
    CHECK(packed_hint_by_id(hint->id) == hint,
          "packed hint ID lookup retains an inactive owner");
  done_with_hints();
  CHECK(packed_hint_by_id(hint->id) == NULL,
        "packed hint ID lookup is empty after teardown");
  delete_clause(candidate);
  delete_clause(hint);
}

static void run_variable_cache_case(int bsub, unsigned conjunction_kb,
                                    const char *path)
{
  const char *text = "cache_probe(wide(a,b,c,d,e,f,g,h,i,j)).";
  Topform hint = parse_clause_from_string((char *) text);
  Topform first = parse_clause_from_string((char *) text);
  Topform second = parse_clause_from_string((char *) text);
  unsigned long long maximum_keys = 0;
  unsigned long long cache_bytes = 0;
  unsigned long long cache_hits = 0;
  BOOL saw_cache = FALSE, saw_no_overflow = FALSE;
  FILE *stats = tmpfile();
  char line[4096];

  hint->attributes = set_int_attribute(hint->attributes, bsub, 5);
  init_hints(ORDINARY_UNIF, bsub, FALSE, FALSE, 2,
             TRUE, TRUE, TRUE, 64, conjunction_kb, 0, 8, NULL);
  index_hint(hint);
  adjust_weight_with_hints(first, FALSE, FALSE);
  adjust_weight_with_hints(second, FALSE, FALSE);
  CHECK(first->matching_hint == hint && second->matching_hint == hint,
        "variable-length cache preserves authoritative matches");
  CHECK(stats != NULL, "open cache statistics stream");
  if (stats != NULL) {
    fprint_packed_hint_operation_stats(stats);
    rewind(stats);
    while (fgets(line, sizeof(line), stats) != NULL) {
      char *maximum = strstr(line, "max_keys=");
      char *table = strstr(line, "table_bytes=");
      if (strstr(line, "Packed_fast_cache:") != NULL) {
        char *hits = strstr(line, "hits=");
        saw_cache = TRUE;
        saw_no_overflow = strstr(line, "key_overflow=0,") != NULL;
        if (hits != NULL)
          cache_hits = strtoull(hits + strlen("hits="), NULL, 10);
      }
      if (maximum != NULL)
        (void) sscanf(maximum, "max_keys=%llu", &maximum_keys);
      if (table != NULL)
        (void) sscanf(table, "table_bytes=%llu", &cache_bytes);
    }
    fclose(stats);
  }
  CHECK(saw_cache && saw_no_overflow,
        "complete cache key fits without a fixed-profile overflow");
  CHECK(maximum_keys > 8,
        "cache accepts a complete profile beyond the former eight-key cap");
  CHECK(cache_bytes <= 64 * 1024 && cache_bytes > 0,
        "fast cache allocation obeys its KiB budget");
  if (cache_hits == 0) {
    fprintf(stderr, "FAIL: %s cache profile produces an exact hit (line %d)\n",
            path, __LINE__);
    Failures++;
  }
  unindex_hint(hint);
  done_with_hints();
  delete_clause(first);
  delete_clause(second);
  delete_clause(hint);
}

static void run_cache_admission_case(int bsub)
{
  Topform hint = parse_clause_from_string("cache_admit(f(a)).");
  Topform first = parse_clause_from_string("cache_admit(f(a)).");
  Topform second = parse_clause_from_string("cache_admit(f(a)).");
  unsigned long long skips = 0, stores = ULLONG_MAX, hits = ULLONG_MAX;
  FILE *stats = tmpfile();
  char line[4096];

  hint->attributes = set_int_attribute(hint->attributes, bsub, 4);
  set_hint_cache_min_candidates(UINT_MAX);
  init_hints(ORDINARY_UNIF, bsub, FALSE, FALSE, 2,
             TRUE, TRUE, TRUE, 64, 327680, 0, 8, NULL);
  index_hint(hint);
  adjust_weight_with_hints(first, FALSE, FALSE);
  adjust_weight_with_hints(second, FALSE, FALSE);
  CHECK(first->matching_hint == hint && second->matching_hint == hint,
        "cache admission threshold preserves authoritative matches");
  CHECK(stats != NULL, "open cache admission statistics stream");
  if (stats != NULL) {
    fprint_packed_hint_operation_stats(stats);
    rewind(stats);
    while (fgets(line, sizeof(line), stats) != NULL)
      if (strstr(line, "Packed_fast_cache:") != NULL) {
        char *field = strstr(line, "admission_skips=");
        if (field != NULL)
          skips = strtoull(field + strlen("admission_skips="), NULL, 10);
        field = strstr(line, "stores=");
        if (field != NULL)
          stores = strtoull(field + strlen("stores="), NULL, 10);
        field = strstr(line, "hits=");
        if (field != NULL)
          hits = strtoull(field + strlen("hits="), NULL, 10);
      }
    fclose(stats);
  }
  CHECK(skips >= 2 && stores == 0 && hits == 0,
        "cache admission rejects cheap profiles before storing them");
  unindex_hint(hint);
  done_with_hints();
  delete_clause(first);
  delete_clause(second);
  delete_clause(hint);
}

static void run_early_profile_filter_case(int bsub)
{
  Topform short_hint =
    parse_clause_from_string("early_profile(f(g(a))).");
  Topform feature_hint =
    parse_clause_from_string(
      "early_profile(f(g(b))) | early_guard(a).");
  Topform matching_hint =
    parse_clause_from_string(
      "early_profile(f(g(a))) | early_guard(a).");
  Topform candidate =
    parse_clause_from_string(
      "early_profile(f(g(a))) | early_guard(a).");
  unsigned long long checks = 0, literal_rejects = 0, feature_rejects = 0;
  FILE *stats = tmpfile();
  char line[4096];

  short_hint->attributes =
    set_int_attribute(short_hint->attributes, bsub, 9);
  feature_hint->attributes =
    set_int_attribute(feature_hint->attributes, bsub, 8);
  matching_hint->attributes =
    set_int_attribute(matching_hint->attributes, bsub, 7);
  init_hints(ORDINARY_UNIF, bsub, FALSE, FALSE, 2,
             TRUE, TRUE, TRUE, 64, 0, 0, 8, NULL);
  index_hint(short_hint);
  index_hint(feature_hint);
  index_hint(matching_hint);
  adjust_weight_with_hints(candidate, FALSE, FALSE);
  CHECK(candidate->matching_hint == matching_hint,
        "early profile rejection preserves authoritative hint identity");
  CHECK(stats != NULL, "open early-profile statistics stream");
  if (stats != NULL) {
    fprint_packed_hint_operation_stats(stats);
    rewind(stats);
    while (fgets(line, sizeof(line), stats) != NULL)
      if (strstr(line, "Packed_fast_dense:") != NULL) {
        char *field = strstr(line, "early_profile_checks=");
        if (field != NULL)
          checks = strtoull(
            field + strlen("early_profile_checks="), NULL, 10);
        field = strstr(line, "early_literal_rejects=");
        if (field != NULL)
          literal_rejects = strtoull(
            field + strlen("early_literal_rejects="), NULL, 10);
        field = strstr(line, "early_feature_rejects=");
        if (field != NULL)
          feature_rejects = strtoull(
            field + strlen("early_feature_rejects="), NULL, 10);
      }
    fclose(stats);
  }
  CHECK(checks >= 3 && literal_rejects > 0 && feature_rejects > 0,
        "dense/sparse fallback rejects impossible profiles before insertion");
  unindex_hint(matching_hint);
  unindex_hint(feature_hint);
  unindex_hint(short_hint);
  done_with_hints();
  delete_clause(candidate);
  delete_clause(matching_hint);
  delete_clause(feature_hint);
  delete_clause(short_hint);
}

static void run_cache_ring_wrap_case(int bsub)
{
  enum { PROBES = 700, REPLAYS = 64 };
  Topform hints[PROBES];
  Topform candidates[PROBES];
  unsigned long long wraps = 0, expired = 0, hits = 0;
  BOOL saw_cache = FALSE;
  FILE *stats;
  char text[160], line[4096];
  int i;

  set_hint_cache_min_candidates(0);
  init_hints(ORDINARY_UNIF, bsub, FALSE, FALSE, 2,
             TRUE, TRUE, TRUE, 64, 327680, 0, 8, NULL);
  for (i = 0; i < PROBES; i++) {
    snprintf(text, sizeof(text),
             "ring_probe_%d(ring_symbol_%d(a,b,c,d,e,f,g,h,i,j)).", i, i);
    hints[i] = parse_clause_from_string(text);
    candidates[i] = parse_clause_from_string(text);
    hints[i]->attributes = set_int_attribute(hints[i]->attributes, bsub, 6);
    index_hint(hints[i]);
  }
  for (i = 0; i < PROBES; i++) {
    adjust_weight_with_hints(candidates[i], FALSE, FALSE);
    CHECK(candidates[i]->matching_hint == hints[i],
          "cache-ring fill preserves exact hint identity");
  }

  /* The newest profiles should still be resident after wrapping.  Replaying
     old profiles then forces exact generation checks wherever their direct
     slots survived but their key segments have been overwritten. */
  for (i = PROBES - REPLAYS; i < PROBES; i++)
    adjust_weight_with_hints(candidates[i], FALSE, FALSE);
  for (i = 0; i < REPLAYS; i++) {
    adjust_weight_with_hints(candidates[i], FALSE, FALSE);
    CHECK(candidates[i]->matching_hint == hints[i],
          "expired cache-ring profile falls back to exact hint matching");
  }

  stats = tmpfile();
  CHECK(stats != NULL, "open cache-ring statistics stream");
  if (stats != NULL) {
    fprint_packed_hint_operation_stats(stats);
    rewind(stats);
    while (fgets(line, sizeof(line), stats) != NULL)
      if (strstr(line, "Packed_fast_cache:") != NULL) {
        char *field;
        saw_cache = TRUE;
        field = strstr(line, "hits=");
        if (field != NULL)
          hits = strtoull(field + strlen("hits="), NULL, 10);
        field = strstr(line, "arena_wraps=");
        if (field != NULL)
          wraps = strtoull(field + strlen("arena_wraps="), NULL, 10);
        field = strstr(line, "arena_expired_misses=");
        if (field != NULL)
          expired = strtoull(
            field + strlen("arena_expired_misses="), NULL, 10);
      }
    fclose(stats);
  }
  CHECK(saw_cache && wraps > 0,
        "tiny exact-key arena exercises circular wraparound");
  CHECK(hits > 0,
        "current cache-ring generation retains exact reusable profiles");
  CHECK(expired > 0,
        "overwritten cache-ring generations are rejected before comparison");
  if (!saw_cache || wraps == 0 || hits == 0 || expired == 0)
    fprintf(stderr,
            "cache-ring diagnostics: saw=%d wraps=%llu hits=%llu expired=%llu\n",
            saw_cache, wraps, hits, expired);

  for (i = 0; i < PROBES; i++)
    unindex_hint(hints[i]);
  done_with_hints();
  for (i = 0; i < PROBES; i++) {
    delete_clause(candidates[i]);
    delete_clause(hints[i]);
  }
}

static void run_observed_stale_rebuild_case(int bsub)
{
  Topform stale = parse_clause_from_string("stale_probe(f(a)).");
  Topform candidate = parse_clause_from_string("stale_probe(f(a)).");
  unsigned long long rebuilds = 0, scan_triggers = 0;
  BOOL saw_maintenance = FALSE, saw_clean_postings = FALSE;
  FILE *stats;
  char line[4096];
  unsigned i;

  init_hints(ORDINARY_UNIF, bsub, FALSE, FALSE, 2,
             TRUE, TRUE, FALSE, 0, 327680, 0, 1, NULL);
  index_hint(stale);
  unindex_hint(stale);

  /* The production floor intentionally avoids rebuild churn on short-lived
     garbage.  Cross it with a tiny one-posting query so this test exercises
     the observed-work trigger rather than the storage-volume trigger. */
  for (i = 0; i < 66000; i++)
    adjust_weight_with_hints(candidate, FALSE, FALSE);

  stats = tmpfile();
  CHECK(stats != NULL, "open posting-maintenance statistics stream");
  if (stats != NULL) {
    fprint_packed_hint_operation_stats(stats);
    rewind(stats);
    while (fgets(line, sizeof(line), stats) != NULL) {
      char *field;
      if (strstr(line, "Better_packed_postings:") != NULL) {
        field = strstr(line, "stale_features=");
        saw_clean_postings = field != NULL &&
          strtoull(field + strlen("stale_features="), NULL, 10) == 0;
        field = strstr(line, "rebuilds=");
        if (field != NULL)
          rebuilds = strtoull(field + strlen("rebuilds="), NULL, 10);
      }
      if (strstr(line, "Better_packed_maintenance:") != NULL) {
        saw_maintenance = TRUE;
        field = strstr(line, "scan_triggers=");
        if (field != NULL)
          scan_triggers = strtoull(field + strlen("scan_triggers="),
                                   NULL, 10);
      }
    }
    fclose(stats);
  }
  CHECK(saw_maintenance, "report observed-work maintenance statistics");
  CHECK(scan_triggers > 0 && rebuilds > 0,
        "observed stale scans trigger a posting rebuild");
  CHECK(saw_clean_postings, "posting rebuild removes stale references");

  done_with_hints();
  delete_clause(candidate);
  delete_clause(stale);
}

static void run_back_fingerprint_case(int bsub)
{
  Topform exact = parse_clause_from_string("p(f(g(h(a)))).");
  Topform deep_mismatch = parse_clause_from_string("p(f(g(h(b)))).");
  Topform demod = parse_clause_from_string("f(g(h(a))) = z.");
  unsigned long long rejects = 0, positives = 0, rewrites = 0;
  BOOL saw_back = FALSE;
  FILE *stats;
  char line[4096];

  init_hints(ORDINARY_UNIF, bsub, FALSE, TRUE, 2,
             TRUE, TRUE, TRUE, 2048, 327680, 0, 8, no_op_demod);
  index_hint(exact);
  index_hint(deep_mismatch);
  back_demod_hints(demod, ORIENTED, FALSE);
  stats = tmpfile();
  CHECK(stats != NULL, "open back-fingerprint statistics stream");
  if (stats != NULL) {
    fprint_packed_hint_operation_stats(stats);
    rewind(stats);
    while (fgets(line, sizeof(line), stats) != NULL)
      if (strstr(line, "Packed_hint_operation: op=back_demod,") != NULL) {
        char *field;
        saw_back = TRUE;
        field = strstr(line, "fingerprint_rejects=");
        if (field != NULL)
          rejects = strtoull(
            field + strlen("fingerprint_rejects="), NULL, 10);
        field = strstr(line, "exact_positive=");
        if (field != NULL)
          positives = strtoull(
            field + strlen("exact_positive="), NULL, 10);
        field = strstr(line, "rewrites=");
        if (field != NULL)
          rewrites = strtoull(field + strlen("rewrites="), NULL, 10);
      }
    fclose(stats);
  }
  CHECK(saw_back && rejects == 1,
        "deep back fingerprint rejects only the impossible hint");
  CHECK(positives == 1 && rewrites == 0,
        "deep back fingerprint preserves exact rewrite authority");

  unindex_hint(exact);
  unindex_hint(deep_mismatch);
  done_with_hints();
  delete_clause(demod);
  delete_clause(deep_mismatch);
  delete_clause(exact);
}

static void run_conjunction_budget_case(int bsub)
{
  Topform hint = parse_clause_from_string("budget_probe(f(a,b,c)).");
  Topform candidate = parse_clause_from_string("budget_probe(f(a,b,c)).");
  BOOL saw_denial = FALSE;
  FILE *stats;
  char line[4096];

  init_hints(ORDINARY_UNIF, bsub, FALSE, FALSE, 2,
             TRUE, TRUE, TRUE, 0, 1, 1, 8, NULL);
  index_hint(hint);
  finalize_hint_conjunction_index();
  adjust_weight_with_hints(candidate, FALSE, FALSE);
  CHECK(candidate->matching_hint == hint,
        "budget denial falls back to exact packed hint matching");
  stats = tmpfile();
  CHECK(stats != NULL, "open conjunction-budget statistics stream");
  if (stats != NULL) {
    fprint_packed_hint_operation_stats(stats);
    rewind(stats);
    while (fgets(line, sizeof(line), stats) != NULL)
      if (strstr(line, "Packed_fast_conjunction:") != NULL)
        saw_denial = strstr(line, "enabled=no,") != NULL &&
          strstr(line, "budget_denials=1,") != NULL;
    fclose(stats);
  }
  CHECK(saw_denial,
        "conjunction budget reports one permanent conservative fallback");
  unindex_hint(hint);
  done_with_hints();
  delete_clause(candidate);
  delete_clause(hint);
}

static void run_conjunction_plan_case(int bsub)
{
  Topform hint = parse_clause_from_string("plan_probe(f(a,b,c)).");
  Topform candidate = parse_clause_from_string("plan_probe(f(a,b,c)).");
  BOOL saw_plan = FALSE;
  FILE *stats;
  char line[4096];

  init_hints(ORDINARY_UNIF, bsub, FALSE, FALSE, 2,
             TRUE, TRUE, TRUE, 0, 1024, 1, 8, NULL);
  index_hint(hint);
  finalize_hint_conjunction_index();
  adjust_weight_with_hints(candidate, FALSE, FALSE);
  CHECK(candidate->matching_hint == hint,
        "accepted population plan preserves exact hint matching");
  stats = tmpfile();
  CHECK(stats != NULL, "open conjunction-plan statistics stream");
  if (stats != NULL) {
    fprint_packed_hint_operation_stats(stats);
    rewind(stats);
    while (fgets(line, sizeof(line), stats) != NULL)
      if (strstr(line, "Packed_fast_conjunction:") != NULL)
        saw_plan = strstr(line, "enabled=yes,") != NULL &&
          strstr(line, "budget_denials=0,") != NULL &&
          strstr(line, "planned_profiles=1,") != NULL &&
          strstr(line, "plan_scans=1,") != NULL;
    fclose(stats);
  }
  CHECK(saw_plan,
        "accepted conjunction plan reports its complete population scan");
  unindex_hint(hint);
  done_with_hints();
  delete_clause(candidate);
  delete_clause(hint);
}

static void run_candidate_order_case(int bsub)
{
  Topform first_hint = parse_clause_from_string("order_probe(f(a)).");
  Topform second_hint = parse_clause_from_string("order_probe(f(b)).");
  Topform ascending_query = parse_clause_from_string("order_probe(x).");
  Topform mixed_query = parse_clause_from_string("order_probe(x).");

  first_hint->attributes = set_int_attribute(
    first_hint->attributes, bsub, 11);
  second_hint->attributes = set_int_attribute(
    second_hint->attributes, bsub, 22);
  init_hints(ORDINARY_UNIF, bsub, FALSE, FALSE, 2,
             TRUE, TRUE, TRUE, 0, 0, 0, 8, NULL);
  index_hint(first_hint);
  index_hint(second_hint);

  /* Monotone posting IDs are produced as [first, second], then converted to
     the legacy decreasing-ID scan order.  Both hints are proper subsumees,
     so the last one visited must be the lower stable ID. */
  adjust_weight_with_hints(ascending_query, FALSE, FALSE);
  CHECK(ascending_query->matching_hint == first_hint,
        "monotone candidate reversal preserves legacy hint tie order");

  /* Re-indexing the lower stable ID appends it after the higher one in the
     conservative postings.  This deliberately creates mixed/decreasing raw
     input and exercises the comparison-sort fallback. */
  unindex_hint(first_hint);
  CHECK(materialize_clause(first_hint),
        "stable-ID order test materializes the hint before re-indexing");
  index_hint(first_hint);
  adjust_weight_with_hints(mixed_query, FALSE, FALSE);
  CHECK(mixed_query->matching_hint == first_hint,
        "mixed candidate fallback preserves legacy hint tie order");

  unindex_hint(first_hint);
  unindex_hint(second_hint);
  done_with_hints();
  delete_clause(mixed_query);
  delete_clause(ascending_query);
  delete_clause(second_hint);
  delete_clause(first_hint);
}

static void run_hint_lifecycle_case(BOOL packed, BOOL better, BOOL fast,
                                    int bsub)
{
  Topform once = parse_clause_from_string("once_probe(f(a)).");
  Topform first = parse_clause_from_string("once_probe(f(a)).");
  Topform second = parse_clause_from_string("once_probe(f(a)).");
  Topform expiring = parse_clause_from_string("expiry_probe(g(b)).");
  Clist owners = clist_init("hint_lifecycle_owners");
  unsigned long long retired_epoch;

  clist_append(once, owners);
  clist_append(expiring, owners);
  init_hints(ORDINARY_UNIF, bsub, FALSE, FALSE, 2,
             packed, better, fast, fast ? 64 : 0, 327680, 0, 8, NULL);
  set_hint_match_once(TRUE);
  index_hint(once);
  set_hints_given_count(10);

  /* Both clauses may enter limbo before either one is retained.  They then
     carry the same matching_hint pointer into keep_hint_matcher(). */
  adjust_weight_with_hints(first, FALSE, FALSE);
  adjust_weight_with_hints(second, FALSE, FALSE);
  CHECK(first->matching_hint == once && second->matching_hint == once,
        "two pending clauses can share one match-once hint");
  keep_hint_matcher(first);
  retired_epoch = hint_state_epoch();
  CHECK(!hint_is_active(once) && active_hints() == 0,
        "first retained matcher retires a match-once hint");
  keep_hint_matcher(second);
  CHECK(!hint_is_active(once) && active_hints() == 0,
        "second pending matcher does not retire the hint twice");
  CHECK(hint_state_epoch() == retired_epoch,
        "duplicate retirement does not publish a false state change");
  unindex_hint(once);
  CHECK(active_hints() == 0,
        "ordinary cleanup is idempotent for a retired match-once hint");

  set_hint_match_once(FALSE);
  index_hint(expiring);
  expiring->weight = 1;
  expiring->last_matched_given = 1;
  CHECK(expire_old_hints(20, 5, 1, owners) == 1,
        "expiry retires an active matched hint");
  CHECK(!hint_is_active(expiring) && owners->length == 2,
        "expired hint remains owned for stable-ID proof restoration");
  CHECK(expire_old_hints(40, 5, 1, owners) == 0,
        "later expiry sweep does not retire the same hint twice");

  if (packed)
    discard_packed_hint_indexes();
  else
    done_with_hints();
  clist_remove_all_clauses(owners);
  clist_free(owners);
  delete_clause(second);
  delete_clause(first);
  delete_clause(expiring);
  delete_clause(once);
}

static void run_terminal_bulk_discard_case(int bsub)
{
  enum { HINTS = 256 };
  Topform hints[HINTS];
  char text[80];
  int i;

  init_hints(ORDINARY_UNIF, bsub, FALSE, FALSE, 2,
             TRUE, TRUE, TRUE, 64, 327680, 0, 8, NULL);
  for (i = 0; i < HINTS; i++) {
    snprintf(text, sizeof(text), "terminal_hint_%d(f(a)).", i);
    hints[i] = parse_clause_from_string(text);
    index_hint(hints[i]);
  }
  CHECK(active_hints() == HINTS,
        "terminal stress bank is fully active before bulk discard");
  discard_packed_hint_indexes();
  CHECK(active_hints() == 0,
        "terminal bulk discard resets the logical active count");
  for (i = 0; i < HINTS; i++) {
    CHECK(!hint_is_active(hints[i]),
          "terminal bulk discard clears every Topform lifecycle bit");
    delete_clause(hints[i]);
  }
}

static void run_compiled_same_cache_lifecycle_case(int bsub)
{
  Topform equal = parse_clause_from_string("same_cache(f(a,a)).");
  Topform unequal = parse_clause_from_string("same_cache(f(a,b)).");
  Topform late = parse_clause_from_string("same_cache(f(c,c)).");
  Topform first = parse_clause_from_string("same_cache(f(x,x)).");
  Topform second = parse_clause_from_string("same_cache(f(x,x)).");
  Topform cached = parse_clause_from_string("same_cache(f(x,x)).");
  Topform after_remove = parse_clause_from_string("same_cache(f(x,x)).");
  Topform after_add = parse_clause_from_string("same_cache(f(x,x)).");
  unsigned long long builds = 0, hits = 0;
  BOOL saw_cache = FALSE;
  FILE *stats;
  char line[4096];

  init_hints(ORDINARY_UNIF, bsub, FALSE, FALSE, 2,
             TRUE, TRUE, TRUE, 0, 0, 0, 8, NULL);
  set_hint_compiled_term_table(TRUE);
  set_hint_compiled_filter(TRUE);
  set_hint_compiled_min_candidates(0);
  set_hint_compiled_cache_build_factor(0);
  index_hint(equal);
  index_hint(unequal);
  finalize_hint_conjunction_index();

  adjust_weight_with_hints(first, FALSE, FALSE);
  adjust_weight_with_hints(second, FALSE, FALSE);
  adjust_weight_with_hints(cached, FALSE, FALSE);
  CHECK(first->matching_hint == equal && second->matching_hint == equal &&
        cached->matching_hint == equal,
        "compiled SAME cache preserves the authoritative repeated match");

  /* A retired match may remain a stale positive in a built bitset.  The
     ordinary active-state check and exact matcher must still reject it. */
  unindex_hint(equal);
  adjust_weight_with_hints(after_remove, FALSE, FALSE);
  CHECK(after_remove->matching_hint == NULL,
        "compiled SAME cache removal leaves only a conservative stale bit");

  /* Hints admitted after the bitset was built must be tested against every
     built path pair so that a new true member cannot become a false negative. */
  index_hint(late);
  adjust_weight_with_hints(after_add, FALSE, FALSE);
  CHECK(after_add->matching_hint == late,
        "compiled SAME cache indexes a matching late hint");

  stats = tmpfile();
  CHECK(stats != NULL, "open compiled SAME cache statistics stream");
  if (stats != NULL) {
    fprint_packed_hint_operation_stats(stats);
    rewind(stats);
    while (fgets(line, sizeof(line), stats) != NULL)
      if (strstr(line, "Compiled_hint_same_cache:") != NULL) {
        char *field;
        saw_cache = TRUE;
        field = strstr(line, "cache_hits=");
        if (field != NULL)
          hits = strtoull(field + strlen("cache_hits="), NULL, 10);
        field = strstr(line, "builds=");
        if (field != NULL)
          builds = strtoull(field + strlen("builds="), NULL, 10);
      }
    fclose(stats);
  }
  CHECK(saw_cache && builds == 1 && hits >= 3,
        "compiled SAME cache reports one build and reused lookups");

  unindex_hint(unequal);
  unindex_hint(late);
  done_with_hints();
  delete_clause(after_add);
  delete_clause(after_remove);
  delete_clause(cached);
  delete_clause(second);
  delete_clause(first);
  delete_clause(late);
  delete_clause(unequal);
  delete_clause(equal);
}

static void run_compiled_rigid_cache_lifecycle_case(int bsub)
{
  Topform matching =
    parse_clause_from_string("rigid_cache(f(g(h(a)))).");
  Topform mismatch =
    parse_clause_from_string("rigid_cache(f(g(k(a)))).");
  Topform late =
    parse_clause_from_string("rigid_cache(f(g(h(b)))).");
  Topform first =
    parse_clause_from_string("rigid_cache(f(g(h(x)))).");
  Topform second =
    parse_clause_from_string("rigid_cache(f(g(h(x)))).");
  Topform cached =
    parse_clause_from_string("rigid_cache(f(g(h(x)))).");
  Topform after_remove =
    parse_clause_from_string("rigid_cache(f(g(h(x)))).");
  Topform after_add =
    parse_clause_from_string("rigid_cache(f(g(h(x)))).");
  unsigned long long rigid_built = 0, rigid_hits = 0;
  BOOL saw_cache = FALSE;
  FILE *stats;
  char line[4096];

  init_hints(ORDINARY_UNIF, bsub, FALSE, FALSE, 2,
             TRUE, TRUE, TRUE, 0, 0, 0, 8, NULL);
  set_hint_compiled_term_table(TRUE);
  set_hint_compiled_filter(TRUE);
  set_hint_compiled_min_candidates(0);
  set_hint_compiled_cache_build_factor(0);
  index_hint(matching);
  index_hint(mismatch);
  finalize_hint_conjunction_index();

  adjust_weight_with_hints(first, FALSE, FALSE);
  adjust_weight_with_hints(second, FALSE, FALSE);
  adjust_weight_with_hints(cached, FALSE, FALSE);
  CHECK(first->matching_hint == matching &&
        second->matching_hint == matching &&
        cached->matching_hint == matching,
        "compiled RIGID cache preserves the authoritative deep match");

  /* Retired IDs can remain conservative positives until another structure
     is rebuilt; active-state and exact matching remain authoritative. */
  unindex_hint(matching);
  adjust_weight_with_hints(after_remove, FALSE, FALSE);
  CHECK(after_remove->matching_hint == NULL,
        "compiled RIGID cache removal leaves only a stale positive");

  /* Every built condition is evaluated for a late hint, so its stable ID is
     inserted when the new target satisfies the deep fixed-symbol test. */
  index_hint(late);
  adjust_weight_with_hints(after_add, FALSE, FALSE);
  CHECK(after_add->matching_hint == late,
        "compiled RIGID cache indexes a matching late hint");

  stats = tmpfile();
  CHECK(stats != NULL, "open compiled RIGID cache statistics stream");
  if (stats != NULL) {
    fprint_packed_hint_operation_stats(stats);
    rewind(stats);
    while (fgets(line, sizeof(line), stats) != NULL)
      if (strstr(line, "Compiled_hint_same_cache:") != NULL) {
        char *field;
        saw_cache = TRUE;
        field = strstr(line, "rigid_built=");
        if (field != NULL)
          rigid_built = strtoull(
            field + strlen("rigid_built="), NULL, 10);
        field = strstr(line, "rigid_hits=");
        if (field != NULL)
          rigid_hits = strtoull(
            field + strlen("rigid_hits="), NULL, 10);
      }
    fclose(stats);
  }
  CHECK(saw_cache && rigid_built == 1 && rigid_hits >= 3,
        "compiled RIGID cache reports one build and reused lookups");

  unindex_hint(mismatch);
  unindex_hint(late);
  done_with_hints();
  delete_clause(after_add);
  delete_clause(after_remove);
  delete_clause(cached);
  delete_clause(second);
  delete_clause(first);
  delete_clause(late);
  delete_clause(mismatch);
  delete_clause(matching);
}

int main(void)
{
  init_standard_ladr();
  (void) register_attribute("label", STRING_ATTRIBUTE);
  int bsub = register_attribute("preview_bsub_wt", INT_ATTRIBUTE);
  run_case(FALSE, FALSE, FALSE, bsub);
  run_case(TRUE, FALSE, FALSE, bsub);
  run_case(TRUE, TRUE, FALSE, bsub);
  run_case(TRUE, TRUE, TRUE, bsub);
  run_variable_cache_case(bsub, 327680, "stable conjunction-order");
  run_variable_cache_case(bsub, 0, "canonical sparse-fallback");
  run_cache_admission_case(bsub);
  run_early_profile_filter_case(bsub);
  run_observed_stale_rebuild_case(bsub);
  run_back_fingerprint_case(bsub);
  run_conjunction_budget_case(bsub);
  run_conjunction_plan_case(bsub);
  run_candidate_order_case(bsub);
  run_hint_lifecycle_case(FALSE, FALSE, FALSE, bsub);
  run_hint_lifecycle_case(TRUE, FALSE, FALSE, bsub);
  run_hint_lifecycle_case(TRUE, TRUE, FALSE, bsub);
  run_hint_lifecycle_case(TRUE, TRUE, TRUE, bsub);
  run_terminal_bulk_discard_case(bsub);
  run_cache_ring_wrap_case(bsub);
  run_compiled_same_cache_lifecycle_case(bsub);
  run_compiled_rigid_cache_lifecycle_case(bsub);
  if (Failures != 0) {
    fprintf(stderr, "hint_preview_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("hint_preview_test: PASS\n");
  return 0;
}
