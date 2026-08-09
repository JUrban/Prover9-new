/* Differential and residency-accounting tests for passive body backends. */

#include "../ladr/ladr.h"
#include "../provers.src/cold_passive_store.h"
#include <stdint.h>

#define BULK_RECORDS 5000

static int Failures;

#define CHECK(test, message) do {                                      \
  if (!(test)) {                                                       \
    fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__);     \
    Failures++;                                                        \
  }                                                                    \
} while (0)

static Topform make_clause(unsigned serial)
{
  Topform c = parse_clause_from_string("f(x,a) = g(x,b).");
  if (c == NULL)
    return NULL;
  c->justification = input_just();
  c->normal_vars = TRUE;
  c->initial = (serial & 1U) != 0;
  c->subsumer = (serial & 2U) != 0;
  c->goal_derived = (serial & 4U) != 0;
  c->proof_tree_weight_cache = (int) serial + 17;
  c->last_matched_given = (unsigned long long) serial * 13;
  assign_clause_id(c);
  return c;
}

static void check_materialized(Topform c, Topform expected,
                               unsigned long long id, char *where)
{
  CHECK(c != NULL, where);
  if (c == NULL)
    return;
  CHECK(c->id == id && clause_ident(c->literals, expected->literals),
        "cold body and stable ID round trip");
  CHECK(c->normal_vars == expected->normal_vars &&
        c->initial == expected->initial &&
        c->subsumer == expected->subsumer &&
        c->goal_derived == expected->goal_derived &&
        c->proof_tree_weight_cache == expected->proof_tree_weight_cache &&
        c->last_matched_given == expected->last_matched_given,
        "cold scalar metadata round trips");
  CHECK(c->justification != NULL && c->justification->type == INPUT_JUST,
        "packed justification round trips");
}

static void round_trip(Cold_passive_store_mode mode)
{
  Cold_passive_store store = cold_passive_store_init(mode);
  Cold_passive_store clone = cold_passive_store_init(mode);
  Topform original = make_clause((unsigned) mode + 1);
  Topform expected;
  Topform peek, active, copied;
  unsigned body = 0, justification = 0, logical = 0;
  unsigned long long id;
  size_t position, cloned_position;
  struct cold_passive_store_stats stats;

  CHECK(store != NULL && clone != NULL && original != NULL,
        "cold backend initializes");
  if (store == NULL || clone == NULL || original == NULL)
    return;
  id = original->id;
  expected = copy_clause_ija(original);
  expected->normal_vars = original->normal_vars;
  expected->initial = original->initial;
  expected->subsumer = original->subsumer;
  expected->goal_derived = original->goal_derived;
  expected->proof_tree_weight_cache = original->proof_tree_weight_cache;
  expected->last_matched_given = original->last_matched_given;

  position = cold_passive_store_archive(store, original, &body,
                                         &justification, &logical);
  CHECK(position == 0 && body != 0 && justification != 0 && logical != 0,
        "cold archive reports its exact payload classes");
  CHECK(find_clause_by_id(id) == NULL,
        "cold archive detaches the official ID");
  CHECK(cold_passive_store_payload_sizes(store, position, NULL, NULL, NULL),
        "cold payload header validates without materialization");

  peek = cold_passive_store_materialize(store, position, id, FALSE);
  check_materialized(peek, expected, id, "cold peek materializes");
  CHECK(peek != NULL && peek->archive_materialized,
        "cold peek is explicitly releasable scratch");
  cold_passive_store_release(peek);

  cloned_position = cold_passive_store_clone_record(store, position, clone);
  CHECK(cloned_position == 0, "cold record clones between equal backends");
  copied = cold_passive_store_materialize(clone, cloned_position, id, FALSE);
  check_materialized(copied, expected, id, "cloned cold body materializes");
  cold_passive_store_release(copied);

  active = cold_passive_store_materialize(store, position, id, TRUE);
  check_materialized(active, expected, id, "cold activation materializes");
  CHECK(active != NULL && find_clause_by_id(id) == active,
        "cold activation restores official ID ownership");
  delete_clause(active);

  CHECK(cold_passive_store_sync(store), "cold backend synchronizes");
  stats = cold_passive_store_get_stats(store);
  CHECK(stats.mode == mode && stats.records == 1 &&
        stats.record_bytes > body + justification &&
        stats.materializations == 2 && stats.validation_failures == 0,
        "cold backend accounting is internally consistent");
  if (mode == COLD_PASSIVE_FILE)
    CHECK(stats.file_writes == 1 && stats.file_write_bytes == stats.record_bytes &&
          stats.file_reads >= 6 && stats.file_read_bytes > stats.record_bytes,
          "file backend accounts bounded explicit I/O");

  zap_just(expected->justification);
  expected->justification = NULL;
  zap_topform(expected);
  cold_passive_store_free(clone);
  cold_passive_store_free(store);
}

/* The important residency invariant is structural: increasing the file by
   thousands of records must not increase its in-process backing by the same
   amount. */
static void file_backing_bound(void)
{
  Cold_passive_store store = cold_passive_store_init(COLD_PASSIVE_FILE);
  size_t first = SIZE_MAX, last = SIZE_MAX;
  unsigned i;
  struct cold_passive_store_stats before_read, after_read;
  CHECK(store != NULL, "bulk file backend initializes");
  if (store == NULL)
    return;
  for (i = 0; i < BULK_RECORDS; i++) {
    Topform c = make_clause(i);
    size_t position = cold_passive_store_archive(store, c, NULL, NULL, NULL);
    if (i == 0)
      first = position;
    last = position;
    if (position == SIZE_MAX)
      break;
  }
  before_read = cold_passive_store_get_stats(store);
  CHECK(i == BULK_RECORDS && first == 0 && last != SIZE_MAX,
        "bulk file archive completes");
  CHECK(before_read.record_bytes > 250000 && before_read.backing_bytes == 0,
        "file growth does not allocate a resident mirror");
  {
    Topform c = cold_passive_store_materialize(
      store, last, clause_ids_assigned(), FALSE);
    CHECK(c != NULL, "last bulk file record materializes");
    cold_passive_store_release(c);
  }
  after_read = cold_passive_store_get_stats(store);
  CHECK(after_read.backing_bytes < after_read.record_bytes / 8,
        "file read scratch remains bounded below arena size");
  CHECK(after_read.physical_bytes >= after_read.record_bytes,
        "file physical accounting covers written logical records");
  cold_passive_store_free(store);
}

int main(void)
{
  init_standard_ladr();
  clear_clause_id_tab();
  set_clause_id_count(0);
  round_trip(COLD_PASSIVE_MEMORY);
#ifndef __EMSCRIPTEN__
  round_trip(COLD_PASSIVE_MMAP);
  round_trip(COLD_PASSIVE_FILE);
  file_backing_bound();
#endif
  clear_clause_id_tab();
  set_clause_id_count(0);
  if (Failures != 0) {
    fprintf(stderr, "cold_passive_store_test: %d failure(s)\n", Failures);
    return 1;
  }
  puts("cold_passive_store_test: PASS");
  return 0;
}
