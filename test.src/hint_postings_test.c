#include "../ladr/hint_postings.h"

static void require(BOOL condition, const char *message)
{
  if (!condition)
    fatal_error((char *) message);
}

static BOOL contains(const unsigned long long *refs, unsigned count,
                     unsigned id, unsigned version)
{
  unsigned i;
  for (i = 0; i < count; i++) {
    if (hint_posting_id(refs[i]) == id &&
        hint_posting_version(refs[i]) == version)
      return TRUE;
  }
  return FALSE;
}

int main(void)
{
  Hint_postings index = hint_postings_init();
  struct hint_postings_stats stats;
  const unsigned long long *refs;
  unsigned count;
  unsigned i;

  hint_postings_add(index, 17, 3, 1);
  hint_postings_add(index, 17, 91, 7);
  hint_postings_add(index, 99, 3, 2);
  hint_postings_add(index, 0, 7, 1);
  refs = hint_postings_get(index, 17, &count);
  require(count == 2, "posting 17 count");
  require(contains(refs, count, 3, 1), "posting 17 ID 3/version 1");
  require(contains(refs, count, 91, 7), "posting 17 ID 91/version 7");
  refs = hint_postings_get(index, 0, &count);
  require(count == 1 && hint_posting_id(refs[0]) == 7,
          "zero-valued feature key");

  /* Force table rehashes and posting-vector growth. */
  for (i = 1; i <= 2000; i++) {
    hint_postings_add(index, 1000 + i, i, i + 1);
    hint_postings_add(index, 500000, i, 9);
  }
  for (i = 1; i <= 2000; i += 137) {
    refs = hint_postings_get(index, 1000 + i, &count);
    require(count == 1, "rehash singleton count");
    require(hint_posting_id(refs[0]) == i, "rehash singleton ID");
    require(hint_posting_version(refs[0]) == i + 1,
            "rehash singleton version");
  }
  refs = hint_postings_get(index, 500000, &count);
  require(count == 2000, "grown posting count");
  require(contains(refs, count, 1, 9), "grown posting first ID");
  require(contains(refs, count, 2000, 9), "grown posting last ID");
  require(hint_postings_get(index, 123456789, &count) == NULL && count == 0,
          "absent posting");

  hint_postings_get_stats(index, &stats);
  require(stats.keys == 2004, "key statistics");
  require(stats.references == 4004, "reference statistics");
  require(stats.maximum_posting == 2000, "maximum posting statistics");
  require(stats.table_bytes > 0 && stats.reference_bytes > 0,
          "byte statistics");

  hint_postings_destroy(index);
  printf("hint_postings_test: PASS\n");
  return 0;
}
