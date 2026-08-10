/* Focused regression tests for disabled-clause compression and FPA pruning. */

#include "../ladr/ladr.h"

static int Failures;

#define CHECK(test, message) do {                                         \
  if (!(test)) {                                                          \
    fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__);        \
    Failures++;                                                           \
  }                                                                       \
} while (0)

static Topform clause(char *s)
{
  Topform c = parse_clause_from_string(s);
  CHECK(c != NULL, "clause parse");
  return c;
}

static Term term(char *s)
{
  Term t = parse_term_from_string(s);
  CHECK(t != NULL, "term parse");
  return t;
}

static BOOL term_flags_ident(Term a, Term b)
{
  int i;
  if (VARIABLE(a) || VARIABLE(b))
    return VARIABLE(a) && VARIABLE(b);
  if (a->private_flags != b->private_flags || ARITY(a) != ARITY(b))
    return FALSE;
  for (i = 0; i < ARITY(a); i++)
    if (!term_flags_ident(ARG(a, i), ARG(b, i)))
      return FALSE;
  return TRUE;
}

static BOOL literal_flags_ident(Literals a, Literals b)
{
  while (a != NULL && b != NULL) {
    if (!term_flags_ident(a->atom, b->atom))
      return FALSE;
    a = a->next;
    b = b->next;
  }
  return a == NULL && b == NULL;
}

static void compact_term_layout_test(void)
{
  Term t = term("compact_layout_f(a,b)");
  size_t expected = BYTES_POINTER == 8 ? 24 : 16;
  CHECK(sizeof(struct term) == expected,
        "term header omits a redundant argument-array pointer");
  CHECK(ARGS(t) == (Term *) (t + 1),
        "term argument array immediately follows its compact header");
  CHECK(ARITY(t) == 2 && CONSTANT(ARG(t, 0)) && CONSTANT(ARG(t, 1)),
        "compact term argument access preserves parsed structure");
  zap_term(t);
}

static void process_accounting_test(void)
{
  struct memory_process_stats stats;
  memory_get_process_stats(&stats);
  if (stats.smaps_supported) {
    CHECK(stats.rss_kbytes > 0 && stats.pss_kbytes > 0,
          "smaps rollup reports resident and proportional bytes");
    CHECK(stats.private_clean_kbytes + stats.private_dirty_kbytes <=
          stats.rss_kbytes,
          "private smaps components do not exceed RSS");
  }
  if (stats.libc_heap_supported) {
    CHECK(stats.libc_arena_bytes >= stats.libc_in_use_bytes,
          "libc arena covers its allocated heap bytes");
    CHECK(stats.libc_arena_bytes >= stats.libc_free_bytes,
          "libc arena covers its free heap bytes");
  }
}

static void compression_shape_test(void)
{
  enum { WIDTH = 300, DEPTH = 300, LITERALS = 1100 };
  Topform wide = get_topform();
  Topform deep = get_topform();
  Topform many = get_topform();
  Topform deep_expected;
  Term root, p;
  Literals *next_lit = &many->literals;
  int wide_sn = str_to_sn("memory_test_wide_atom", WIDTH);
  int leaf_sn = str_to_sn("memory_test_shape_leaf", 0);
  int unary_sn = str_to_sn("memory_test_deep_atom", 1);
  int i;

  root = get_rigid_term_dangerously(wide_sn, WIDTH);
  for (i = 0; i < WIDTH; i++)
    ARG(root, i) = get_rigid_term_dangerously(leaf_sn, 0);
  wide->literals = new_literal(TRUE, root);
  upward_clause_links(wide);

  p = get_rigid_term_dangerously(leaf_sn, 0);
  for (i = 0; i < DEPTH; i++) {
    Term parent = get_rigid_term_dangerously(unary_sn, 1);
    ARG(parent, 0) = p;
    p = parent;
  }
  deep->literals = new_literal(TRUE, p);
  upward_clause_links(deep);
  deep_expected = copy_clause(deep);

  for (i = 0; i < LITERALS; i++) {
    *next_lit = new_literal(TRUE,
                 get_rigid_term_dangerously(leaf_sn, 0));
    next_lit = &(*next_lit)->next;
  }
  upward_clause_links(many);

  CHECK(compress_clause(wide) == CLAUSE_COMPRESS_OK &&
        compressed_clause_is_valid(wide) && materialize_clause(wide),
        "wide clause exercises growing traversal stacks");
  CHECK(ARITY(wide->literals->atom) == WIDTH,
        "wide clause retains its full arity");
  for (i = 0; i < WIDTH; i++)
    CHECK(SYMNUM(ARG(wide->literals->atom, i)) == leaf_sn,
          "wide clause retains every argument");
  CHECK(compress_clause(deep) == CLAUSE_COMPRESS_OK &&
        compressed_clause_is_valid(deep) && materialize_clause(deep),
        "deep clause exercises growing decode stack");
  CHECK(clause_ident(deep->literals, deep_expected->literals),
        "deep clause round trips exactly");
  CHECK(compress_clause(many) == CLAUSE_COMPRESS_OK &&
        compressed_clause_is_valid(many) && materialize_clause(many),
        "many-literal clause crosses the legacy fixed-array boundary");
  CHECK(number_of_literals(many->literals) == LITERALS,
        "many-literal clause retains every literal");

  zap_topform(wide);
  zap_topform(deep);
  zap_topform(many);
  zap_topform(deep_expected);
}

static void compression_round_trip_test(void)
{
  Topform c, expected, negative, empty, high_clause, high_expected, a, b;
  Attribute saved_attributes;
  unsigned saved_atom_flags;
  unsigned char saved_version;
  unsigned saved_size;
  unsigned long long logical_bytes;
  struct clause_compression_stats stats;
  Plist clauses = NULL, changed;
  FILE *fp;
  int attr, i, high_symnum;

  clause_compression_reset_stats();
  attr = register_attribute("memory_test_attribute", TERM_ATTRIBUTE);

  c = clause("p(f(x,a),g(y,h(x))) | -q(y) | f(y,b) = x.");
  orient_equalities(c, FALSE);
  c->literals->atom->private_flags |= (FLAGS_TYPE) 0x40;
  ARG(c->literals->atom, 0)->private_flags |= (FLAGS_TYPE) 0x20;
  c->attributes = set_term_attribute(c->attributes, attr,
                                     term("payload(f(a),x)"));
  saved_attributes = c->attributes;
  saved_atom_flags = c->literals->next->next->atom->private_flags;
  expected = copy_clause_with_flags(c);
  logical_bytes = clause_body_storage_bytes(c);
  CHECK(logical_bytes > 0, "logical clause bytes recorded");

  for (i = 0; i < 40; i++) {
    CHECK(compress_clause(c) == CLAUSE_COMPRESS_OK, "compress succeeds");
    CHECK(c->literals == NULL && c->compressed != NULL,
          "compressed clause has one representation");
    CHECK(c->compressed_size > 2, "versioned payload has content");
    CHECK(c->uncompressed_body_bytes == logical_bytes,
          "logical byte estimate survives compression");
    CHECK(compressed_clause_is_valid(c), "payload validates");
    CHECK(compress_clause(c) == CLAUSE_COMPRESS_ALREADY,
          "double compression is a safe skip");
    CHECK(materialize_clause(c), "materialization succeeds");
    CHECK(c->compressed == NULL && c->literals != NULL,
          "materialized clause has one representation");
    CHECK(clause_ident(c->literals, expected->literals),
          "literal structure round trips exactly");
    CHECK(literal_flags_ident(c->literals, expected->literals),
          "atom and nested term flags round trip exactly");
    CHECK(check_upward_clause_links(c), "upward links restored");
    CHECK(c->attributes == saved_attributes &&
          get_term_attribute(c->attributes, attr, 1) != NULL,
          "unusual attribute storage is untouched");
    CHECK(c->literals->next->next->atom->private_flags == saved_atom_flags,
          "equality orientation is restored");
  }

  CHECK(compress_clause(c) == CLAUSE_COMPRESS_OK,
        "compress before malformed-input tests");
  saved_version = (unsigned char) c->compressed[1];
  c->compressed[1] = (char) (saved_version + 1);
  CHECK(!compressed_clause_is_valid(c), "unknown payload version rejected");
  CHECK(!materialize_clause(c) && c->compressed != NULL && c->literals == NULL,
        "failed decode leaves compact representation intact");
  c->compressed[1] = (char) saved_version;
  saved_size = c->compressed_size;
  c->compressed_size = 2;
  CHECK(!materialize_clause(c), "truncated payload rejected");
  CHECK(c->compressed != NULL && c->literals == NULL,
        "truncated decode is non-destructive");
  c->compressed_size = saved_size;
  CHECK(materialize_clause(c), "valid payload still materializes after rejection");

  negative = clause("-p(f(x,a)) | -q(g(x)).");
  CHECK(compress_clause(negative) == CLAUSE_COMPRESS_OK,
        "negative clause compresses");
  CHECK(negative_clause_possibly_compressed(negative),
        "negative-clause metadata works while compact");
  fp = tmpfile();
  CHECK(fp != NULL, "temporary print stream");
  if (fp != NULL) {
    fwrite_clause(fp, negative, CL_FORM_STD);
    fclose(fp);
  }
  CHECK(negative->compressed != NULL && negative->literals == NULL,
        "printing materializes only for the print scope");
  CHECK(materialize_clause(negative), "negative clause materializes");

  empty = clause("$F.");
  CHECK(compress_clause(empty) == CLAUSE_COMPRESS_NO_BODY,
        "empty clause is skipped safely");

  for (i = 0; i < 180; i++) {
    char name[40];
    snprintf(name, sizeof(name), "memory_test_symbol_%d", i);
    (void) str_to_sn(name, 0);
  }
  high_symnum = str_to_sn("memory_test_high_symbol", 0);
  CHECK(high_symnum > 127, "test symbol exceeds legacy signed-char range");
  high_clause = clause("p(memory_test_high_symbol).");
  high_expected = copy_clause(high_clause);
  CHECK(compress_clause(high_clause) == CLAUSE_COMPRESS_OK,
        "large symbol number compresses");
  CHECK(materialize_clause(high_clause), "large symbol number materializes");
  CHECK(clause_ident(high_clause->literals, high_expected->literals),
        "large symbol number round trips");

  a = clause("r(f(x,a)).");
  b = clause("-s(g(y,b)).");
  CHECK(compress_clause(a) == CLAUSE_COMPRESS_OK, "scope clause a compresses");
  CHECK(compress_clause(b) == CLAUSE_COMPRESS_OK, "scope clause b compresses");
  clauses = plist_append(clauses, a);
  clauses = plist_append(clauses, b);
  changed = materialize_clauses(clauses);
  CHECK(plist_count(changed) == 2, "scope records exactly materialized clauses");
  recompress_clauses(changed);
  CHECK(a->compressed != NULL && b->compressed != NULL,
        "scope recompresses both clauses");
  zap_plist(changed);
  zap_plist(clauses);

  stats = clause_compression_get_stats();
  CHECK(stats.attempted > stats.successful && stats.successful > 0,
        "compression attempt/success/skip counters move");
  CHECK(stats.materialized > 0 && stats.recompressed >= 3,
        "materialize/recompress counters move");

  compression_shape_test();

  zap_topform(expected);
  zap_topform(c);
  zap_topform(negative);
  zap_topform(empty);
  zap_topform(high_clause);
  zap_topform(high_expected);
  zap_topform(a);
  zap_topform(b);
}

static BOOL query_contains(Fpa_index idx, Term query, Term wanted)
{
  Fpa_state state = NULL;
  Term answer = fpa_first_answer(query, NULL, IDENTICAL, idx, &state);
  BOOL found = FALSE;
  while (answer != NULL) {
    if (answer == wanted)
      found = TRUE;
    answer = fpa_next_answer(state);
  }
  return found;
}

static void fpa_case(int depth, int hash_threshold, int order_kind)
{
  static char *texts[] = {
    "f(a,b)", "f(a,b)", "f(a,c)", "f(g(a),h(b))",
    "f(g(a),h(c))", "g(a,b)", "h(a,b)", "k(a,b)"
  };
  static int mixed_order[] = {3, 0, 6, 1, 7, 4, 2, 5};
  Term terms[8];
  Fpa_index idx;
  unsigned long long nodes_before = fpa_live_trie_nodes();
  unsigned long long lists_before = fpalist_live_lists();
  int i;

  set_fpa_hash_threshold(hash_threshold);
  idx = fpa_init_index(depth);
  CHECK(fpa_live_trie_nodes() == nodes_before + 1,
        "new FPA index owns exactly its root node");
  for (i = 0; i < 8; i++) {
    terms[i] = term(texts[i]);
    fpa_update(terms[i], idx, INSERT);
  }
  CHECK(!fpa_empty(idx), "populated FPA index is nonempty");
  CHECK(query_contains(idx, terms[0], terms[0]) &&
        query_contains(idx, terms[0], terms[1]),
        "structurally duplicate terms are independently retrievable");

  for (i = 0; i < 8; i++) {
    int n = order_kind == 0 ? i :
            order_kind == 1 ? 7 - i : mixed_order[i];
    CHECK(query_contains(idx, terms[n], terms[n]),
          "term is retrievable immediately before deletion");
    fpa_update(terms[n], idx, DELETE);
    CHECK(!query_contains(idx, terms[n], terms[n]),
          "deleted pointer is absent from later retrieval");
  }

  CHECK(fpa_empty(idx), "FPA index is empty after all deletions");
  CHECK(fpa_live_trie_nodes() == nodes_before + 1,
        "bottom-up pruning returns trie to root only");
  CHECK(fpalist_live_lists() == lists_before,
        "empty FPA list headers are freed immediately");
  zap_fpa_index(idx);
  CHECK(fpa_live_trie_nodes() == nodes_before,
        "FPA index destruction frees its root");
  CHECK(fpalist_live_lists() == lists_before,
        "FPA index destruction preserves list baseline");

  for (i = 0; i < 8; i++)
    zap_term(terms[i]);
}

static void fpa_pruning_test(void)
{
  int depth, hashing, order, cycle;
  unsigned long long nodes_before = fpa_live_trie_nodes();
  unsigned long long lists_before = fpalist_live_lists();

  for (cycle = 0; cycle < 12; cycle++)
    for (depth = 0; depth < 2; depth++)
      for (hashing = 0; hashing < 2; hashing++)
        for (order = 0; order < 3; order++)
          fpa_case(depth == 0 ? 0 : 10,
                   hashing == 0 ? 2 : 1000000, order);

  CHECK(fpa_live_trie_nodes() == nodes_before,
        "repeated FPA churn has no residual trie nodes");
  CHECK(fpalist_live_lists() == lists_before,
        "repeated FPA churn has no residual list headers");
  CHECK(fpa_peak_trie_nodes() > nodes_before &&
        fpalist_peak_lists() > lists_before,
        "FPA peak counters observe churn");
}

static void fpa_hash_invariant_test(void)
{
  enum { NTERMS = 96 };
  Term terms[NTERMS];
  Fpa_index idx;
  unsigned long long nodes_before = fpa_live_trie_nodes();
  unsigned long long lists_before = fpalist_live_lists();
  int present[NTERMS];
  int i, pass;

  set_fpa_hash_threshold(2);
  idx = fpa_init_index(4);
  for (i = 0; i < NTERMS; i++) {
    char text[64];
    snprintf(text, sizeof(text), "hash_stress_%d(shared(a))", i);
    terms[i] = term(text);
    present[i] = TRUE;
    fpa_update(terms[i], idx, INSERT);
  }

  /* Three relatively-prime strides exercise clustered deletion, hash-table
     shrink/rebuild, and reinsertion with stable FPA IDs. */
  for (pass = 0; pass < 3; pass++) {
    for (i = pass; i < NTERMS; i += 3) {
      fpa_update(terms[i], idx, DELETE);
      present[i] = FALSE;
    }
    for (i = 0; i < NTERMS; i++)
      CHECK(query_contains(idx, terms[i], terms[i]) == present[i],
            "hashed child lookup remains correct after clustered deletion");
  }
  CHECK(fpa_empty(idx), "hash stress deletes every term");
  CHECK(fpa_live_trie_nodes() == nodes_before + 1 &&
        fpalist_live_lists() == lists_before,
        "hash stress prunes to a list-free root");

  for (i = NTERMS - 1; i >= 0; i--)
    fpa_update(terms[i], idx, INSERT);
  for (i = 0; i < NTERMS; i++)
    CHECK(query_contains(idx, terms[i], terms[i]),
          "hashed term is retrievable after reinsertion");
  for (i = 0; i < NTERMS; i++)
    fpa_update(terms[i], idx, DELETE);
  zap_fpa_index(idx);
  CHECK(fpa_live_trie_nodes() == nodes_before &&
        fpalist_live_lists() == lists_before,
        "hash stress destruction returns exact live baselines");
  for (i = 0; i < NTERMS; i++)
    zap_term(terms[i]);
}

int main(void)
{
  init_standard_ladr();
  process_accounting_test();
  compact_term_layout_test();
  compression_round_trip_test();
  fpa_pruning_test();
  fpa_hash_invariant_test();
  if (Failures != 0) {
    fprintf(stderr, "memory_lifecycle_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("memory_lifecycle_test: PASS "
         "fpa_nodes_live=%llu fpa_nodes_peak=%llu "
         "fpa_lists_live=%llu fpa_lists_peak=%llu\n",
         fpa_live_trie_nodes(), fpa_peak_trie_nodes(),
         fpalist_live_lists(), fpalist_peak_lists());
  return 0;
}
