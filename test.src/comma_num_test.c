#include "../ladr/string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keep this at least as large as the longest statistics fprintf in search.c.
 * The regression failed with the former 16-slot comma_num ring: later calls
 * overwrote the strings returned for the first arguments before fprintf had
 * consumed them. */
#define REPORT_ARGUMENTS 24

int main(void)
{
  char *formatted[REPORT_ARGUMENTS];
  int i;

  set_comma_formatting(FALSE);
  for (i = 0; i < REPORT_ARGUMENTS; i++)
    formatted[i] = comma_num(1000000000000ULL + (unsigned long long) i);

  for (i = 0; i < REPORT_ARGUMENTS; i++) {
    char expected[32];
    snprintf(expected, sizeof(expected), "%llu",
             1000000000000ULL + (unsigned long long) i);
    if (strcmp(formatted[i], expected) != 0) {
      fprintf(stderr,
              "comma_num_test: slot %d changed from %s to %s\n",
              i, expected, formatted[i]);
      return EXIT_FAILURE;
    }
  }

  puts("comma_num_test: PASS");
  return EXIT_SUCCESS;
}
