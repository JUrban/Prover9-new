/* Differential and continuation-boundary tests for bounded hyperresolution. */

#include "../ladr/ladr.h"

#define MAX_RESULTS 4096
#define MAX_PARENTS 32

static int Failures;
static Topform Parents[MAX_PARENTS];
static unsigned Parent_count;
static Topform Results[MAX_RESULTS];
static unsigned Result_count;

#define CHECK(test, message) do {                                        \
  if (!(test)) {                                                         \
    fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__);      \
    Failures++;                                                          \
  }                                                                      \
} while (0)

static void collect_result(Topform c)
{
  if (Result_count >= MAX_RESULTS)
    fatal_error("hyper iterator test result capacity exceeded");
  Results[Result_count++] = c;
}

static void clear_results(void)
{
  unsigned i;
  for (i = 0; i < Result_count; i++)
    zap_topform(Results[i]);
  Result_count = 0;
}

static Topform add_parent(char *text)
{
  Topform c = parse_clause_from_string(text);
  CHECK(c != NULL, "parse hyper parent");
  if (Parent_count >= MAX_PARENTS)
    fatal_error("hyper iterator test parent capacity exceeded");
  c->id = Parent_count + 1;
  Parents[Parent_count++] = c;
  return c;
}

static unsigned long long source_count(void *data)
{
  (void) data;
  return Parent_count;
}

static Topform source_clause(unsigned long long position, void *data)
{
  (void) data;
  return Parents[position];
}

static BOOL source_test(Topform c, void *data)
{
  (void) c;
  (void) data;
  return TRUE;
}

static BOOL eager_test(Topform c, void *data)
{
  return source_test(c, data);
}

static unsigned eager_results(
  Topform given, int direction, Lindex idx, Topform *expected)
{
  unsigned i, count;
  Result_count = 0;
  hyper_resolution_with_clause_test(
    given, direction, idx, eager_test, NULL, collect_result);
  count = Result_count;
  for (i = 0; i < count; i++)
    expected[i] = copy_clause(Results[i]);
  clear_results();
  return count;
}

static void clone_iterator(Hyper_iterator *to, const Hyper_iterator *from)
{
  *to = *from;
  to->choices = NULL;
  to->choice_capacity = 0;
  if (from->depth != 0) {
    to->choices = safe_calloc(from->depth, sizeof(*to->choices));
    memcpy(to->choices, from->choices,
           from->depth * sizeof(*to->choices));
    to->choice_capacity = from->depth;
  }
}

static void compare_sequence(
  Topform *expected, unsigned expected_count, unsigned expected_offset,
  const char *message)
{
  unsigned i;
  CHECK(Result_count == expected_count, message);
  for (i = 0; i < Result_count && i < expected_count; i++)
    CHECK(clause_ident(Results[i]->literals,
                       expected[expected_offset + i]->literals), message);
}

static void checkpoint_suffix_test(
  Topform given, int direction, const Hyper_parent_source *source,
  const Hyper_iterator *saved, Topform *expected,
  unsigned expected_count, unsigned expected_offset)
{
  Hyper_iterator restored;
  BOOL complete = FALSE;
  clone_iterator(&restored, saved);
  Result_count = 0;
  while (!complete) {
    unsigned long long steps, yielded;
    complete = hyper_resolution_bounded(
      given, direction, source, &restored, 5, 2,
      collect_result, &steps, &yielded);
    CHECK(steps <= 5 && yielded <= 2,
          "restored hyper iterator respects both budgets");
    CHECK(complete || steps != 0 || yielded != 0,
          "restored hyper iterator makes progress");
  }
  compare_sequence(expected, expected_count - expected_offset,
                   expected_offset, "restored hyper suffix equals eager suffix");
  clear_results();
  hyper_iterator_zap(&restored);
}

static void one_case(
  Topform given, int direction, Lindex idx,
  const Hyper_parent_source *source)
{
  Topform expected[MAX_RESULTS];
  unsigned expected_count = eager_results(given, direction, idx, expected);
  unsigned budget;
  CHECK(expected_count > 0, "hyper fixture produces conclusions");

  for (budget = 1; budget <= 64; budget++) {
    Hyper_iterator it;
    BOOL complete = FALSE;
    hyper_iterator_init(&it);
    Result_count = 0;
    while (!complete) {
      unsigned long long steps, yielded;
      complete = hyper_resolution_bounded(
        given, direction, source, &it, budget, budget,
        collect_result, &steps, &yielded);
      CHECK(steps <= budget, "hyper raw work never exceeds budget");
      CHECK(yielded <= budget, "hyper yields never exceed budget");
      CHECK(complete || steps != 0 || yielded != 0,
            "incomplete hyper iterator makes progress");
    }
    compare_sequence(expected, expected_count, 0,
                     "bounded hyper sequence equals eager sequence");
    clear_results();
    hyper_iterator_zap(&it);
  }

  {
    Hyper_iterator it;
    BOOL complete = FALSE;
    unsigned yielded_total = 0;
    hyper_iterator_init(&it);
    while (!complete) {
      unsigned long long steps, yielded;
      Result_count = 0;
      complete = hyper_resolution_bounded(
        given, direction, source, &it, 1, 1,
        collect_result, &steps, &yielded);
      yielded_total += (unsigned) yielded;
      clear_results();
      checkpoint_suffix_test(given, direction, source, &it, expected,
                             expected_count, yielded_total);
    }
    CHECK(yielded_total == expected_count,
          "unit-budget hyper turns produce every eager conclusion");
    hyper_iterator_zap(&it);
  }

  for (budget = 0; budget < expected_count; budget++)
    zap_topform(expected[budget]);
}

int main(void)
{
  Hyper_parent_source source;
  Lindex idx;
  unsigned i;
  Topform pos_nucleus, neg_nucleus, pos_satellite, neg_satellite;
  init_standard_ladr();
  resolution_options(FALSE, FALSE, FALSE, -1, FALSE);

  pos_satellite = add_parent("P(a).");
  add_parent("Q(a).");
  add_parent("P(b).");
  add_parent("Q(b).");
  pos_nucleus = add_parent("-P(x) | -Q(x) | R(x).");
  neg_satellite = add_parent("-P(a).");
  add_parent("-Q(a).");
  add_parent("-P(b).");
  add_parent("-Q(b).");
  neg_nucleus = add_parent("P(x) | Q(x) | -R(x).");

  idx = lindex_init(FPA, ORDINARY_UNIF, 10,
                    FPA, ORDINARY_UNIF, 10);
  for (i = 0; i < Parent_count; i++)
    lindex_update(idx, Parents[i], INSERT);
  source.count = source_count;
  source.clause = source_clause;
  source.test = source_test;
  source.data = NULL;

  one_case(pos_nucleus, POS_RES, idx, &source);
  one_case(pos_satellite, POS_RES, idx, &source);
  one_case(neg_nucleus, NEG_RES, idx, &source);
  one_case(neg_satellite, NEG_RES, idx, &source);

  for (i = 0; i < Parent_count; i++)
    lindex_update(idx, Parents[i], DELETE);
  lindex_destroy(idx);
  for (i = 0; i < Parent_count; i++)
    zap_topform(Parents[i]);

  if (Failures != 0) {
    fprintf(stderr, "hyper_iterator_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("hyper_iterator_test: PASS\n");
  return 0;
}
