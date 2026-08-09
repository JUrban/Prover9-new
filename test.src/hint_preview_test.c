#include "../ladr/ladr.h"

static int Failures;

#define CHECK(test, message) do {                                      \
  if (!(test)) {                                                       \
    fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__);   \
    Failures++;                                                        \
  }                                                                    \
} while (0)

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
  init_hints(ORDINARY_UNIF, bsub, FALSE, FALSE, 2, packed, better, fast,
             NULL);
  index_hint(hint);
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
  done_with_hints();
  delete_clause(candidate);
  delete_clause(hint);
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
  if (Failures != 0) {
    fprintf(stderr, "hint_preview_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("hint_preview_test: PASS\n");
  return 0;
}
