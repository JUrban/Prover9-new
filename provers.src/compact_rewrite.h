#ifndef TP_COMPACT_REWRITE_H
#define TP_COMPACT_REWRITE_H

#include "../ladr/ladr.h"

typedef struct compact_rewrite_bank * Compact_rewrite_bank;

struct compact_rewrite_stats {
  unsigned long long rules_current;
  unsigned long long rules_peak;
  unsigned long long rules_retired;
  unsigned long long attempts;
  unsigned long long rewrites;
  unsigned long long node_bytes;
  unsigned long long posting_bytes;
  unsigned long long rule_bytes;
  unsigned long long term_bytes;
  unsigned long long hash_bytes;
  unsigned long long total_bytes;
  unsigned long long peak_bytes;
};

Compact_rewrite_bank compact_rewrite_init(void);

BOOL compact_rewrite_add(Compact_rewrite_bank bank, Topform clause, int type);

BOOL compact_rewrite_remove(Compact_rewrite_bank bank,
                            unsigned long long proof_id);

BOOL compact_rewrite_contains(Compact_rewrite_bank bank,
                              unsigned long long proof_id);

void compact_rewrite_clause(Compact_rewrite_bank bank, Topform clause,
                            int step_limit, int increase_limit,
                            BOOL lex_order_vars, BOOL count_stats);

void compact_rewrite_get_stats(Compact_rewrite_bank bank,
                               struct compact_rewrite_stats *stats);

void compact_rewrite_free(Compact_rewrite_bank bank);

#endif
