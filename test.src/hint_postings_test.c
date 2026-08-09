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
  printf("hint_postings_test: PASS\n");
  return 0;
}
