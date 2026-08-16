#include "../ladr/hint_postings.h"

static void require(BOOL condition, const char *message)
{
  if (!condition)
    fatal_error((char *) message);
}

static BOOL contains(const unsigned *refs, unsigned count, unsigned id)
{
  unsigned i;
  for (i = 0; i < count; i++) {
    if (refs[i] == id)
      return TRUE;
  }
  return FALSE;
}

int main(void)
{
  Hint_postings index = hint_postings_init();
  struct hint_postings_stats stats;
  struct hint_dense_view dense;
  const unsigned *refs;
  unsigned count;
  unsigned i;

  hint_postings_add(index, 17, 3);
  hint_postings_add(index, 17, 91);
  hint_postings_add(index, 99, 3);
  hint_postings_add(index, 0, 7);
  refs = hint_postings_get(index, 17, &count);
  require(count == 2, "posting 17 count");
  require(contains(refs, count, 3), "posting 17 ID 3");
  require(contains(refs, count, 91), "posting 17 ID 91");
  require(hint_postings_generation(index, 17) == 2,
          "posting generation counts insertions");
  require(hint_postings_dense_view(index, 17, 256, TRUE, &dense),
          "dense view promotion");
  require((dense.bits[3 / 64] & (1ULL << (3 % 64))) != 0 &&
          (dense.bits[91 / 64] & (1ULL << (91 % 64))) != 0,
          "dense view contains existing IDs");
  hint_postings_add(index, 17, 200);
  require((dense.bits[200 / 64] & (1ULL << (200 % 64))) != 0,
          "dense view tracks later insertion");
  refs = hint_postings_get(index, 17, &count);
  require(count == 3, "promoted posting remains appendable");
  refs = hint_postings_get(index, 0, &count);
  require(count == 1 && refs[0] == 7,
          "zero-valued feature key");

  /* Force table rehashes and posting-vector growth. */
  for (i = 1; i <= 2000; i++) {
    hint_postings_add(index, 1000 + i, i);
    hint_postings_add(index, 500000, i);
  }
  for (i = 1; i <= 2000; i += 137) {
    refs = hint_postings_get(index, 1000 + i, &count);
    require(count == 1, "rehash singleton count");
    require(refs[0] == i, "rehash singleton ID");
  }
  refs = hint_postings_get(index, 500000, &count);
  require(count == 2000, "grown posting count");
  require(contains(refs, count, 1), "grown posting first ID");
  require(contains(refs, count, 2000), "grown posting last ID");
  require(hint_postings_get(index, 123456789, &count) == NULL && count == 0,
          "absent posting");

  hint_postings_get_stats(index, &stats);
  require(stats.keys == 2004, "key statistics");
  require(stats.references == 4005, "reference statistics");
  require(stats.maximum_posting == 2000, "maximum posting statistics");
  require(stats.table_bytes > 0 && stats.reference_bytes > 0,
          "byte statistics");
  require(stats.dense_keys == 1 && stats.dense_bit_bytes > 0 &&
          stats.dense_summary_bytes > 0,
          "dense byte statistics");

  hint_postings_destroy(index);

  /* Profile postings keep their sidecars aligned across posting growth and
     table rehashes.  Masks are block-major bit planes: plane B says which
     of the next 64 IDs contain feature bit B. */
  index = hint_postings_init();
  for (i = 0; i < 130; i++)
    hint_postings_add_profile(index, 700, i + 1,
                              (i % 3 == 0 ? 1ULL << 2 : 0) |
                              (i % 5 == 1 ? 1ULL << 47 : 0),
                              i % 11, i % 7);
  for (i = 1; i <= 1000; i++)
    hint_postings_add_profile(index, 10000 + i, i,
                              1ULL << (i % 64), i % 9, i % 5);
  {
    struct hint_profile_view profile;
    require(hint_postings_get_profile(index, 700, &profile),
            "profile posting lookup after rehash");
    require(profile.count == 130 && profile.mask_blocks == 3,
            "profile posting spans three mask blocks");
    require(profile.mask_planes != NULL,
            "profile exposes its mask bit planes");
    for (i = 0; i < profile.count; i++) {
      unsigned block = i / 64;
      unsigned long long flag = 1ULL << (i % 64);
      require(profile.ids[i] == i + 1,
              "profile IDs stay aligned after growth");
      require(profile.literal_counts[i] ==
                (((i % 11) << 16) | (i % 7)),
              "profile literal counts stay aligned after growth");
      require(((profile.mask_planes[(size_t) block * 64 + 2] & flag) != 0) ==
                (i % 3 == 0),
              "low profile mask plane is exact");
      require(((profile.mask_planes[(size_t) block * 64 + 47] & flag) != 0) ==
                (i % 5 == 1),
              "high profile mask plane is exact");
    }
    require(!hint_postings_get_profile(index, 123456789, &profile) &&
            profile.count == 0,
            "absent profile posting returns an empty view");
    require(hint_postings_get_profile(index, 10001, &profile) &&
            profile.count == 1 &&
            (profile.mask_planes[1] & 1ULL) != 0,
            "singleton profile keeps its exact mask plane");
  }
  hint_postings_get_stats(index, &stats);
  require(stats.keys == 1001 && stats.references == 1130 &&
          stats.profile_bytes > 0 && stats.profile_mask_words == 64192 &&
          stats.profile_key_histogram[0] == 1000 &&
          stats.profile_reference_histogram[0] == 1000 &&
          stats.profile_key_histogram[6] == 1 &&
          stats.profile_reference_histogram[6] == 130,
          "profile key, reference, and sidecar byte statistics");
  hint_postings_destroy(index);

  /* A dense cache that cannot grow must disappear rather than omit a later
     ID.  The sparse posting is always the complete fallback. */
  index = hint_postings_init();
  hint_postings_set_dense_budget(index, 40);
  hint_postings_add(index, 17, 3);
  require(hint_postings_dense_view(index, 17, 256, TRUE, &dense),
          "bounded dense view fits exact budget");
  hint_postings_add(index, 17, 300);
  require(!hint_postings_dense_view(index, 17, 301, FALSE, &dense),
          "failed dense growth discards incomplete view");
  refs = hint_postings_get(index, 17, &count);
  require(count == 2 && contains(refs, count, 3) &&
          contains(refs, count, 300),
          "dense denial preserves complete sparse posting");
  hint_postings_get_stats(index, &stats);
  require(stats.dense_keys == 0 && stats.dense_bit_bytes == 0 &&
          stats.dense_summary_bytes == 0 &&
          stats.dense_budget_bytes == 40 &&
          stats.dense_budget_denials == 1,
          "dense budget and denial statistics");
  hint_postings_destroy(index);
  printf("hint_postings_test: PASS\n");
  return 0;
}
