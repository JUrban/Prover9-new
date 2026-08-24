#include "../ladr/ladr.h"

static int Failures;

#define CHECK(test, message) do {                                    \
  if (!(test)) {                                                     \
    fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__); \
    Failures++;                                                      \
  }                                                                 \
} while (0)

static Topform packed_clause(const char *text)
{
  Topform c = parse_clause_from_string((char *) text);
  CHECK(compress_clause(c) == CLAUSE_COMPRESS_OK,
        "test clause compresses");
  return c;
}

static void target_case(const char *pattern_text, const char *target_text,
                        BOOL expected)
{
  Topform pattern = parse_clause_from_string((char *) pattern_text);
  Topform target = packed_clause(target_text);
  BOOL matched = !expected;
  CHECK(compressed_unit_target_matches(pattern->literals, target, &matched),
        "resident-pattern direction is supported");
  CHECK(matched == expected, "resident pattern has expected result");
  delete_clause(pattern);
  delete_clause(target);
}

static void pattern_case(const char *pattern_text, const char *target_text,
                         BOOL expected)
{
  Topform pattern = packed_clause(pattern_text);
  Topform target = parse_clause_from_string((char *) target_text);
  BOOL matched = !expected;
  CHECK(compressed_unit_pattern_matches(pattern, target->literals, &matched),
        "packed-pattern direction is supported");
  CHECK(matched == expected, "packed pattern has expected result");
  delete_clause(pattern);
  delete_clause(target);
}

static void profile_case(const char *pattern_text, const char *target_text,
                         BOOL expected, unsigned expected_reasons)
{
  Topform pattern = parse_clause_from_string((char *) pattern_text);
  Topform target = packed_clause(target_text);
  struct compressed_unit_match_profile profile;
  BOOL ordinary = !expected;
  BOOL profiled = !expected;
  CHECK(compressed_unit_target_matches(
          pattern->literals, target, &ordinary),
        "ordinary resident-pattern direction is supported");
  CHECK(compressed_unit_target_match_profile(
          pattern->literals, target, &profiled, &profile),
        "profiled resident-pattern direction is supported");
  CHECK(ordinary == expected && profiled == expected,
        "profiled match preserves the ordinary answer");
  CHECK(profile.reject_reasons == expected_reasons,
        "profile reports the expected rejection reasons");
  delete_clause(pattern);
  delete_clause(target);
}

int main(void)
{
  Topform flagged;
  Topform pattern;
  BOOL matched = FALSE;

  init_standard_ladr();
  target_case("p(f(x,x)).", "p(f(a,a)).", TRUE);
  target_case("p(f(x,x)).", "p(f(a,b)).", FALSE);
  target_case("p(f(x,y)).", "p(f(a,b)).", TRUE);
  target_case("p(a).", "-p(a).", FALSE);
  target_case("-p(x).", "-p(a).", TRUE);
  target_case("p(a).", "p(x).", FALSE);

  profile_case("p(f(x,x,a)).", "p(f(g(c),g(c),a)).", TRUE, 0);
  profile_case("p(f(x,x,a)).", "p(f(g(c),g(d),a)).", FALSE,
               COMPRESSED_UNIT_REJECT_REPEATED);
  profile_case("p(f(x,x,a)).", "p(f(g(c),g(c),b)).", FALSE,
               COMPRESSED_UNIT_REJECT_RIGID);
  profile_case("p(f(x,x,a)).", "p(f(g(c),g(d),b)).", FALSE,
               COMPRESSED_UNIT_REJECT_RIGID |
                 COMPRESSED_UNIT_REJECT_REPEATED);
  profile_case("p(x).", "-p(a).", FALSE,
               COMPRESSED_UNIT_REJECT_SIGN);

  {
    Topform profiled_pattern = parse_clause_from_string("p(f(x,x,a)).");
    Topform profiled_target = packed_clause("p(f(g(c),g(c),a)).");
    struct compressed_unit_match_profile profile;
    BOOL profiled_match = FALSE;
    CHECK(compressed_unit_target_match_profile(
            profiled_pattern->literals, profiled_target,
            &profiled_match, &profile),
          "profile work counters are supported");
    CHECK(profiled_match, "profile work-counter case matches");
    CHECK(profile.first_bindings == 1 && profile.repeated_tests == 1,
          "profile counts first and repeated generated variables");
    CHECK(profile.skipped_subterms == 2 && profile.skipped_nodes == 4,
          "profile counts whole stored subterms skipped");
    CHECK(profile.repeated_compare_nodes == 2,
          "profile counts compact nodes compared for repeated binding");
    CHECK(profile.rigid_tests == 3 && profile.stream_nodes == 7,
          "profile counts rigid tests and consumed target nodes");
    delete_clause(profiled_pattern);
    delete_clause(profiled_target);
  }

  pattern_case("p(f(x,x)).", "p(f(a,a)).", TRUE);
  pattern_case("p(f(x,x)).", "p(f(a,b)).", FALSE);
  pattern_case("p(f(x,y)).", "p(f(a,b)).", TRUE);
  pattern_case("p(a).", "-p(a).", FALSE);
  pattern_case("-p(x).", "-p(a).", TRUE);
  pattern_case("p(a).", "p(x).", FALSE);

  /* Compression preserves private term flags for checkpoint fidelity, but
     ordinary term identity and hint matching intentionally ignore them. */
  flagged = parse_clause_from_string("p(f(a)).");
  flagged->literals->atom->private_flags = (FLAGS_TYPE) 7;
  CHECK(compress_clause(flagged) == CLAUSE_COMPRESS_OK,
        "flagged target compresses");
  pattern = parse_clause_from_string("p(f(a)).");
  CHECK(compressed_unit_target_matches(pattern->literals, flagged, &matched),
        "flagged target is supported");
  CHECK(matched, "private flags do not affect direct term identity");
  delete_clause(pattern);
  delete_clause(flagged);

  if (Failures != 0) {
    fprintf(stderr, "compressed_unit_match_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("compressed_unit_match_test: PASS\n");
  return 0;
}
