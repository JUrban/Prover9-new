#include "../provers.src/rewrite_only_store.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
  Rewrite_only_store store = rewrite_only_store_init();
  unsigned long long i;

  assert(rewrite_only_store_count(store) == 0);
  for (i = 1; i <= 10000; i++) {
    Topform c = get_topform();
    c->id = i;
    assert(rewrite_only_store_insert(store, c, (int) (i % 4 + 1),
                                     100 + i % 31));
  }
  assert(rewrite_only_store_count(store) == 10000);
  assert(rewrite_only_store_peak_allocated_bytes(store) >=
         rewrite_only_store_allocated_bytes(store));

  for (i = 2; i <= 10000; i += 2) {
    int type = 0;
    unsigned long long bytes = 0;
    Topform c = rewrite_only_store_remove(store, i, &type, &bytes);
    assert(c != NULL && c->id == i);
    assert(type == (int) (i % 4 + 1));
    assert(bytes == 100 + i % 31);
    delete_clause(c);
    assert(rewrite_only_store_find(store, i, NULL, NULL) == NULL);
  }
  assert(rewrite_only_store_count(store) == 5000);

  for (i = 10001; i <= 16000; i++) {
    Topform c = get_topform();
    c->id = i;
    assert(rewrite_only_store_insert(store, c, 1, 200));
  }
  assert(rewrite_only_store_count(store) == 11000);
  for (i = 1; i <= 16000; i += 2) {
    Topform c = rewrite_only_store_find(store, i, NULL, NULL);
    if (i <= 10000)
      assert(c != NULL && c->id == i);
  }

  while (rewrite_only_store_count(store) != 0) {
    Topform c = rewrite_only_store_take_any(store, NULL, NULL);
    assert(c != NULL);
    delete_clause(c);
  }
  rewrite_only_store_free(store);
  printf("rewrite_only_store_test: PASS\n");
  return 0;
}
