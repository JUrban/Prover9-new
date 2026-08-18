/* Focused regression checks for dense symbol-number lookup. */

#include "../ladr/symbols.h"
#include <stdio.h>
#include <stdlib.h>

#define SYMBOLS_TO_ADD 4096

static
void require(BOOL condition, const char *message)
{
  if (!condition) {
    fprintf(stderr, "symbol_table_test: %s\n", message);
    exit(1);
  }
}

int main(void)
{
  int ids[SYMBOLS_TO_ADD];
  int arities[SYMBOLS_TO_ADD];
  int i;

  require(sn_to_arity(0) == -1, "zero ID must remain invalid");
  require(sn_to_arity(-1) == -1, "negative ID must remain invalid");
  require(sn_to_arity(1) == -1, "unknown positive ID must remain invalid");
  require(sn_to_str(1)[0] == '\0', "unknown ID must retain empty name");

  for (i = 0; i < SYMBOLS_TO_ADD; i++) {
    char name[64];
    int arity = i % 17;
    snprintf(name, sizeof(name), "direct_symbol_%d", i);
    ids[i] = str_to_sn(name, arity);
    arities[i] = arity;
  }

  for (i = 0; i < SYMBOLS_TO_ADD; i++) {
    char name[64];
    snprintf(name, sizeof(name), "direct_symbol_%d", i);
    require(sn_to_arity(ids[i]) == arities[i],
            "arity changed while direct table grew");
    require(is_symbol(ids[i], name, arities[i]),
            "symbol identity changed while direct table grew");
    require(str_to_sn(name, arities[i]) == ids[i],
            "existing string/arity pair received a new ID");
  }

  require(sn_to_arity(greatest_symnum() + 1) == -1,
          "ID above the populated range must remain invalid");

  printf("symbol_table_test: PASS (%d symbols)\n", SYMBOLS_TO_ADD);
  return 0;
}
