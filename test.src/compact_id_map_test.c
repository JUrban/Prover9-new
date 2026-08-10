#include "../provers.src/compact_id_map.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

struct visit_state {
  size_t count;
  unsigned long long xor_ids;
  unsigned long long previous_id;
  BOOL increasing;
};

static void visit(unsigned long long proof_id, const uint32_t *values,
                  void *context)
{
  struct visit_state *state = context;
  assert(values[0] != 0);
  state->count++;
  state->xor_ids ^= proof_id;
}

static void visit_set(unsigned long long proof_id, void *context)
{
  struct visit_state *state = context;
  if (state->count != 0 && proof_id <= state->previous_id)
    state->increasing = FALSE;
  state->previous_id = proof_id;
  state->count++;
  state->xor_ids ^= proof_id;
}

int main(void)
{
  Compact_id_map one = compact_id_map_init(1);
  Compact_id_map two = compact_id_map_init(2);
  Compact_id_set set = compact_id_set_init();
  const unsigned long long ids[] = {
    1, 2, 16383, 16384, 16385, 272786,
    (UINT64_C(1) << 40) + 17
  };
  struct visit_state state = {0, 0, 0, TRUE};
  unsigned long long expected_xor = 0;
  size_t i;

  for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
    uint32_t value = (uint32_t) i + 1;
    uint32_t pair[2] = {value, value * 3};
    assert(compact_id_map_put(one, ids[i], &value));
    assert(compact_id_map_put(two, ids[i], pair));
    assert(compact_id_set_add(set, ids[i]));
    assert(!compact_id_set_add(set, ids[i]));
    expected_xor ^= ids[i];
  }
  assert(compact_id_map_count(one) == 7);
  assert(compact_id_map_count(two) == 7);
  assert(compact_id_set_count(set) == 7);
  for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
    uint32_t value = 0, pair[2] = {0, 0};
    assert(compact_id_map_get(one, ids[i], &value));
    assert(value == i + 1);
    assert(compact_id_map_get(two, ids[i], pair));
    assert(pair[0] == i + 1 && pair[1] == (i + 1) * 3);
    assert(compact_id_set_contains(set, ids[i]));
  }
  {
    uint32_t replacement = 99;
    assert(!compact_id_map_put(one, ids[2], &replacement));
    replacement = 0;
    assert(compact_id_map_get(one, ids[2], &replacement));
    assert(replacement == 99);
  }
  assert(compact_id_map_remove(one, ids[1]));
  assert(!compact_id_map_remove(one, ids[1]));
  assert(!compact_id_map_get(one, ids[1], NULL));
  assert(compact_id_map_count(one) == 6);

  compact_id_map_foreach(two, visit, &state);
  assert(state.count == 7);
  assert(state.xor_ids == expected_xor);
  assert(compact_id_map_bytes(one) < 5 * 64 * 1024);
  assert(compact_id_map_peak_bytes(one) >= compact_id_map_bytes(one));
  state.count = 0;
  state.xor_ids = 0;
  state.previous_id = 0;
  state.increasing = TRUE;
  compact_id_set_foreach(set, visit_set, &state);
  assert(state.count == 7);
  assert(state.xor_ids == expected_xor);
  assert(state.increasing);
  assert(compact_id_set_projected_map_bytes(set, 2) ==
         compact_id_map_bytes(two));
  assert(compact_id_set_bytes(set) < 6 * 4096);

  compact_id_map_free(one);
  compact_id_map_free(two);
  compact_id_set_free(set);
  puts("compact_id_map_test: PASS");
  return 0;
}
