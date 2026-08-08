/* Differential and continuation-boundary tests for bounded paramodulation. */

#include "../ladr/ladr.h"

#define MAX_RESULTS 4096

static int Failures;
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
    fatal_error("paramod iterator test result capacity exceeded");
  Results[Result_count++] = c;
}

static void clear_results(void)
{
  unsigned i;
  for (i = 0; i < Result_count; i++)
    zap_topform(Results[i]);
  Result_count = 0;
}

static Topform test_clause(char *text, unsigned long long id)
{
  Topform c = parse_clause_from_string(text);
  CHECK(c != NULL, "parse test clause");
  c->id = id;
  return c;
}

static unsigned eager_results(
  Topform from, Topform into, BOOL check_top, Topform *copy)
{
  Context cf = get_context();
  Context ci = get_context();
  unsigned i, count;
  Result_count = 0;
  para_from_into(from, cf, into, ci, check_top, collect_result);
  free_context(cf);
  free_context(ci);
  count = Result_count;
  for (i = 0; i < count; i++)
    copy[i] = copy_clause(Results[i]);
  clear_results();
  return count;
}

static void clone_iterator(Para_iterator *to, const Para_iterator *from)
{
  *to = *from;
  to->path = NULL;
  to->path_capacity = 0;
  if (from->path_depth != 0) {
    to->path = safe_calloc(from->path_depth, sizeof(*to->path));
    memcpy(to->path, from->path, from->path_depth * sizeof(*to->path));
    to->path_capacity = from->path_depth;
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
  Topform from, Topform into, BOOL check_top,
  const Para_iterator *saved, Topform *expected,
  unsigned expected_count, unsigned expected_offset)
{
  Para_iterator restored;
  BOOL complete = FALSE;
  clone_iterator(&restored, saved);
  Result_count = 0;
  while (!complete) {
    unsigned long long steps, yielded;
    complete = para_from_into_bounded(
      from, into, check_top, &restored, 3, 2, collect_result,
      &steps, &yielded);
    CHECK(steps <= 3 && yielded <= 2,
          "restored iterator respects both budgets");
  }
  compare_sequence(expected, expected_count - expected_offset,
                   expected_offset, "restored suffix equals eager suffix");
  clear_results();
  para_iterator_zap(&restored);
}

static void one_case(Topform from, Topform into, BOOL check_top)
{
  Topform expected[MAX_RESULTS];
  unsigned expected_count = eager_results(from, into, check_top, expected);
  unsigned budget;
  CHECK(expected_count > 0, "fixture produces at least one conclusion");

  for (budget = 1; budget <= 64; budget++) {
    Para_iterator it;
    BOOL complete = FALSE;
    para_iterator_init(&it);
    Result_count = 0;
    while (!complete) {
      unsigned long long steps, yielded;
      complete = para_from_into_bounded(
        from, into, check_top, &it, budget, budget, collect_result,
        &steps, &yielded);
      CHECK(steps <= budget, "raw work never exceeds forced budget");
      CHECK(yielded <= budget, "yield count never exceeds forced budget");
      CHECK(complete || steps != 0,
            "an incomplete iterator always advances raw work");
    }
    compare_sequence(expected, expected_count, 0,
                     "bounded sequence equals eager sequence");
    clear_results();
    para_iterator_zap(&it);
  }

  /* Budget one places a save boundary after every visited coordinate,
     including failed unifications and each term-path transition. */
  {
    Para_iterator it;
    BOOL complete = FALSE;
    unsigned yielded_total = 0;
    para_iterator_init(&it);
    while (!complete) {
      unsigned long long steps, yielded;
      Result_count = 0;
      complete = para_from_into_bounded(
        from, into, check_top, &it, 1, 1, collect_result,
        &steps, &yielded);
      yielded_total += (unsigned) yielded;
      clear_results();
      checkpoint_suffix_test(from, into, check_top, &it, expected,
                             expected_count, yielded_total);
    }
    CHECK(yielded_total == expected_count,
          "all eager conclusions appear across unit-budget turns");
    para_iterator_zap(&it);
  }

  for (budget = 0; budget < expected_count; budget++)
    zap_topform(expected[budget]);
}

int main(void)
{
  Topform a, b, c, d;
  init_standard_ladr();
  paramodulation_options(FALSE, FALSE, FALSE, FALSE,
                         TRUE, FALSE, FALSE);

  a = test_clause("f(x) = x | g(x) = h(x).", 1);
  b = test_clause("P(f(a),g(f(b))) | f(a) = g(b).", 2);
  one_case(a, b, FALSE);
  one_case(a, b, TRUE);
  one_case(b, a, FALSE);

  /* Exercise the instance/maximality checks used by ordered search and the
     variable-position branch with the same eager oracle. */
  paramodulation_options(TRUE, TRUE, FALSE, FALSE,
                         TRUE, TRUE, FALSE);
  c = test_clause("f(x) = x.", 3);
  d = test_clause("P(f(a)).", 4);
  orient_equalities(c, FALSE);
  orient_equalities(d, FALSE);
  mark_maximal_literals(c->literals);
  mark_maximal_literals(d->literals);
  one_case(c, d, FALSE);

  zap_topform(a);
  zap_topform(b);
  zap_topform(c);
  zap_topform(d);
  if (Failures != 0) {
    fprintf(stderr, "paramod_iterator_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("paramod_iterator_test: PASS\n");
  return 0;
}
