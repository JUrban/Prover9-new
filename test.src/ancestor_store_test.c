/* Phase 4 record-format, corruption, proof materialization, and mmap tests. */

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
  if (SYMNUM(a) != SYMNUM(b) || ARITY(a) != ARITY(b) ||
      a->private_flags != b->private_flags)
    return FALSE;
  for (i = 0; i < ARITY(a); i++)
    if (!term_flags_ident(ARG(a, i), ARG(b, i)))
      return FALSE;
  return TRUE;
}

static BOOL clause_flags_ident(Topform a, Topform b)
{
  Literals x = a->literals, y = b->literals;
  while (x != NULL && y != NULL) {
    if (x->sign != y->sign || !term_flags_ident(x->atom, y->atom))
      return FALSE;
    x = x->next;
    y = y->next;
  }
  return x == NULL && y == NULL;
}

static Just sample_justification(Just_type type)
{
  Just j;
  if (type == IVY_JUST) {
    Plist pairs = plist_append(NULL, term("pair(x,f(a))"));
    return ivy_just(INSTANCE_JUST, 31, NULL, 0, NULL, pairs);
  }
  j = get_just();
  j->type = type;
  j->next = NULL;
  switch (type) {
  case INPUT_JUST:
  case GOAL_JUST:
    break;
  case DENY_JUST:
  case CLAUSIFY_JUST:
  case COPY_JUST:
  case PROPOSITIONAL_JUST:
  case NEW_SYMBOL_JUST:
  case BACK_DEMOD_JUST:
  case BACK_UNIT_DEL_JUST:
  case FLIP_JUST:
  case XX_JUST:
  case MERGE_JUST:
  case EVAL_JUST:
    j->u.id = 31;
    break;
  case EXPAND_DEF_JUST:
  case BINARY_RES_JUST:
  case HYPER_RES_JUST:
  case UR_RES_JUST:
  case UNIT_DEL_JUST:
  case FACTOR_JUST:
  case XXRES_JUST:
    j->u.lst = ilist_append(ilist_append(NULL, 31), -2);
    break;
  case DEMOD_JUST:
    j->u.demod = i3list_append(NULL, 31, 4, 2);
    break;
  case PARA_JUST:
  case PARA_FX_JUST:
  case PARA_IX_JUST:
  case PARA_FX_IX_JUST:
    j->u.para = get_parajust();
    j->u.para->from_id = 31;
    j->u.para->into_id = 32;
    j->u.para->from_pos = ilist_append(NULL, 1);
    j->u.para->into_pos = ilist_append(NULL, 2);
    break;
  case INSTANCE_JUST:
    j->u.instance = get_instancejust();
    j->u.instance->parent_id = 31;
    j->u.instance->pairs = plist_append(NULL, term("pair(x,f(a))"));
    break;
  case IVY_JUST:
  case UNKNOWN_JUST:
    break;
  }
  return j;
}

static void justification_all_types_test(void)
{
  int type;
  for (type = INPUT_JUST; type < UNKNOWN_JUST; type++) {
    Just original = sample_justification((Just_type) type);
    Just decoded;
    char *a = NULL, *b = NULL;
    unsigned asize = 0, bsize = 0;
    CHECK(encode_justification(original, &a, &asize),
          "every justification type encodes");
    decoded = decode_justification(a, asize);
    CHECK(decoded != NULL, "every justification type decodes");
    CHECK(decoded != NULL &&
          encode_justification(decoded, &b, &bsize) &&
          asize == bsize && memcmp(a, b, asize) == 0,
          "every justification type has a canonical exact round trip");
    safe_free(a);
    safe_free(b);
    zap_just(original);
    zap_just(decoded);
  }
}

static void justification_codec_test(void)
{
  Topform from = get_topform(), into = get_topform();
  Just j, decoded;
  Ilist from_pos = NULL, into_pos = NULL, parents;
  I3list rewrites = NULL;
  Plist pairs = NULL;
  char *data = NULL;
  unsigned size = 0;

  from->id = 11;
  into->id = 12;
  from_pos = ilist_append(from_pos, 1);
  from_pos = ilist_append(from_pos, 2);
  into_pos = ilist_append(into_pos, 3);
  j = para_just(PARA_JUST, from, from_pos, into, into_pos);
  rewrites = i3list_append(rewrites, 13, 4, 2);
  j = append_just(j, demod_just(rewrites));
  CHECK(encode_justification(j, &data, &size) && size > 4,
        "paramodulation/demodulation justification encodes");
  decoded = decode_justification(data, size);
  parents = get_parents(decoded, TRUE);
  CHECK(decoded != NULL && parents != NULL &&
        ilist_member(parents, 11) && ilist_member(parents, 12) &&
        ilist_member(parents, 13),
        "paramodulation positions and rewrite references decode");
  zap_ilist(parents);
  CHECK(decode_justification(data, size - 1) == NULL,
        "truncated compact justification is rejected");
  safe_free(data);
  zap_just(j);
  zap_just(decoded);

  pairs = plist_append(pairs, term("pair(x,f(a))"));
  j = ivy_just(INSTANCE_JUST, 21, NULL, 0, NULL, pairs);
  data = NULL;
  size = 0;
  CHECK(encode_justification(j, &data, &size),
        "IVY instance justification encodes");
  decoded = decode_justification(data, size);
  CHECK(decoded != NULL && decoded->type == IVY_JUST &&
        decoded->u.ivy->type == INSTANCE_JUST &&
        decoded->u.ivy->parent1 == 21 && decoded->u.ivy->pairs != NULL &&
        term_ident(decoded->u.ivy->pairs->v, j->u.ivy->pairs->v),
        "IVY type, parent, and substitution terms round trip");
  safe_free(data);
  zap_just(j);
  zap_just(decoded);
  zap_topform(from);
  zap_topform(into);
}

static void archive_round_trip(Clause_store_archive_mode mode)
{
  Clause_store store = clause_store_init("ancestor-test");
  Topform a, b, expected_a, expected_b, ma, mb;
  Plist proof;
  Ilist parents;
  unsigned long long a_id, b_id, a_offset;
  int attr = attribute_name_to_id("ancestor_payload");
  struct clause_store_stats stats;

  CHECK(clause_store_enable_archive(store, mode),
        "requested ancestor backing initializes");

  a = clause("p(f(x,a)) | -q(g(x)) | h(x) = k(a).");
  orient_equalities(a, FALSE);
  a->literals->atom->private_flags |= (FLAGS_TYPE) 0x40;
  a->attributes = set_term_attribute(a->attributes, attr,
                                     term("payload(f(x),a)"));
  a->justification = input_just();
  a->initial = 1;
  a->was_given = 1;
  a->simplifier_epoch = 37;
  a->weight = 17.25;
  assign_clause_id(a);
  a_id = a->id;
  expected_a = copy_clause_with_flags(a);
  expected_a->id = a_id;
  expected_a->attributes = copy_attributes(a->attributes);
  expected_a->justification = copy_justification(a->justification);
  clause_store_append(store, a);
  CHECK(clause_store_archive_clause(store, a), "input ancestor archives");
  CHECK(find_clause_by_id(a_id) == NULL && clause_id_is_archived(a_id),
        "ID slot contains an offset, not a stale Topform pointer");

  b = clause("r(x) | s(f(x)).");
  b->justification = copy_just(expected_a);
  b->goal_derived = 1;
  b->semantics = 7;
  b->last_matched_given = 99;
  b->simplifier_epoch = 41;
  assign_clause_id(b);
  b_id = b->id;
  expected_b = copy_clause_with_flags(b);
  expected_b->justification = copy_justification(b->justification);
  clause_store_append(store, b);
  CHECK(clause_store_archive_clause(store, b), "derived ancestor archives");

  CHECK(clause_store_length(store) == 2 &&
        clause_store_position_is_archived(store, 0) &&
        clause_store_position_is_archived(store, 1),
        "disabled store retains two small tagged offsets");
  parents = clause_store_parents(store, 1);
  CHECK(parents != NULL && parents->i == (int) a_id && parents->next == NULL,
        "compact parent metadata is exact without materializing a body");
  zap_ilist(parents);

  ma = clause_store_materialize(store, 0);
  mb = clause_store_materialize(store, 1);
  CHECK(ma != NULL && mb != NULL, "records materialize on demand");
  CHECK(ma != NULL && clause_ident(ma->literals, expected_a->literals) &&
        clause_flags_ident(ma, expected_a),
        "compressed body and every private term flag round trip");
  CHECK(ma != NULL && ma->id == a_id && ma->initial && ma->was_given &&
        ma->weight == 17.25 && ma->simplifier_epoch == 37,
        "stable ID, proof flags, and scalar metadata round trip");
  CHECK(ma != NULL && get_term_attribute(ma->attributes, attr, 1) != NULL,
        "term attributes round trip");
  parents = mb == NULL ? NULL : get_parents(mb->justification, TRUE);
  CHECK(parents != NULL && parents->i == (int) a_id,
        "compact justification reconstructs exactly");
  zap_ilist(parents);
  CHECK(mb != NULL && clause_ident(mb->literals, expected_b->literals) &&
        mb->goal_derived && mb->semantics == 7 &&
        mb->last_matched_given == 99 && mb->simplifier_epoch == 41,
        "derived record metadata round trips");

  proof = get_clause_ancestors(mb);
  CHECK(plist_count(proof) == 2 && proof_dag_size(mb) == 2,
        "final proof DAG is materialized completely and only on demand");
  clause_store_release_materialized_plist(proof);
  zap_plist(proof);
  clause_store_release_materialized(ma);
  clause_store_release_materialized(mb);

  CHECK(clause_id_archive_offset(a_id, &a_offset),
        "archive offset is discoverable for validation");
  CHECK(clause_store_test_corrupt(store, a_offset + 4, 0x40),
        "test can corrupt a version/header byte");
  CHECK(clause_store_materialize(store, 0) == NULL,
        "unknown/corrupt record version fails closed");
  CHECK(clause_store_test_corrupt(store, a_offset + 4, 0x40),
        "corruption is reversible for teardown");
  CHECK(clause_store_test_corrupt(store, a_offset + 8, 0x80),
        "test can corrupt the declared record bound");
  CHECK(clause_store_materialize(store, 0) == NULL,
        "malformed record bounds fail closed");
  CHECK(clause_store_test_corrupt(store, a_offset + 8, 0x80),
        "record bound corruption is reversible");
  CHECK(clause_store_test_corrupt(store, a_offset + 92, 0x01),
        "test can corrupt the archived simplifier epoch");
  CHECK(clause_store_materialize(store, 0) == NULL,
        "simplifier-epoch checksum corruption fails closed");
  CHECK(clause_store_test_corrupt(store, a_offset + 92, 0x01),
        "simplifier-epoch corruption is reversible");
  CHECK(clause_store_test_corrupt(store, a_offset + 96, 0x01),
        "test can corrupt record payload");
  CHECK(clause_store_materialize(store, 0) == NULL,
        "payload checksum corruption fails closed");
  CHECK(clause_store_test_corrupt(store, a_offset + 96, 0x01),
        "payload corruption is reversible");
  ma = clause_store_materialize(store, 0);
  CHECK(ma != NULL, "valid record remains readable after rejected corruption");
  clause_store_release_materialized(ma);

  CHECK(clause_store_sync(store), "memory or mmap backing synchronizes");
  stats = clause_store_get_stats(store);
  CHECK(stats.records == 2 && stats.record_bytes > 0 &&
        stats.handle_bytes < 512 && stats.materializations >= 5 &&
        stats.validation_failures >= 4,
        "record, handle, materialization, and validation counters are exact");

  zap_topform(expected_a);
  zap_just(expected_b->justification);
  expected_b->justification = NULL;
  zap_topform(expected_b);
  clause_store_delete_clauses(store);
  CHECK(find_clause_by_id(a_id) == NULL && find_clause_by_id(b_id) == NULL,
        "archive teardown removes every tagged ID entry");
}

static void archive_preserve_body_test(Clause_store_archive_mode mode)
{
  Clause_store store = clause_store_init("ancestor-preserve-test");
  Topform original, expected, materialized;
  Literals original_body;
  unsigned long long id;
  int attr = attribute_name_to_id("ancestor_payload");

  CHECK(clause_store_enable_archive(store, mode),
        "preserve-body ancestor backing initializes");
  original = clause("p(f(x,a)) | h(x) = k(a).");
  orient_equalities(original, FALSE);
  original->literals->atom->private_flags |= (FLAGS_TYPE) 0x80;
  original->attributes = set_term_attribute(original->attributes, attr,
                                             term("payload(g(x),b)"));
  original->justification = input_just();
  original->weight = 23.5;
  original->semantics = 9;
  original->initial = 1;
  original->was_given = 1;
  original->simplifier_epoch = 73;
  assign_clause_id(original);
  id = original->id;
  original_body = original->literals;
  expected = copy_clause_ija(original);
  expected->weight = original->weight;
  expected->semantics = original->semantics;
  expected->initial = original->initial;
  expected->was_given = original->was_given;
  expected->simplifier_epoch = original->simplifier_epoch;

  clause_store_append(store, original);
  CHECK(clause_store_archive_clause_preserve(store, original),
        "ancestor archives without consuming indexed body");
  CHECK(original->literals == original_body && original->compressed == NULL &&
        original->id == id && !original->official_id && !original->disabled,
        "preserved Topform retains the exact body nodes and stable ID");
  CHECK(clause_id_is_archived(id) && find_clause_by_id(id) == NULL,
        "preserved body is detached while archive owns the public ID");

  materialized = clause_store_materialize(store, 0);
  CHECK(materialized != NULL &&
        clause_ident(materialized->literals, expected->literals) &&
        clause_flags_ident(materialized, expected) &&
        materialized->weight == 23.5 && materialized->semantics == 9 &&
        materialized->initial && materialized->was_given &&
        materialized->simplifier_epoch == 73,
        "preserve-body archive retains body, flags, and scalar metadata");
  CHECK(materialized != NULL &&
        get_term_attribute(materialized->attributes, attr, 1) != NULL &&
        materialized->justification != NULL,
        "preserve-body archive retains proof and attribute metadata");

  clause_store_release_materialized(materialized);
  delete_clause(original);
  zap_just(expected->justification);
  expected->justification = NULL;
  zap_topform(expected);
  clause_store_delete_clauses(store);
  CHECK(find_clause_by_id(id) == NULL,
        "preserve-body archive teardown removes the tagged ID");
}

int main(void)
{
  struct clause_id_table_stats ids;
  init_standard_ladr();
  clear_clause_id_tab();
  set_clause_id_count(0);
  (void) register_attribute("ancestor_payload", TERM_ATTRIBUTE);
  justification_all_types_test();
  justification_codec_test();
  archive_round_trip(CLAUSE_STORE_ARCHIVE_MEMORY);
  archive_preserve_body_test(CLAUSE_STORE_ARCHIVE_MEMORY);
#ifndef __EMSCRIPTEN__
  archive_round_trip(CLAUSE_STORE_ARCHIVE_MMAP);
  archive_preserve_body_test(CLAUSE_STORE_ARCHIVE_MMAP);
#endif
  ids = clause_id_table_get_stats();
  CHECK(ids.entries == 0 && ids.pages == 0,
        "all memory and mmap ID pages return to baseline");
  set_clause_id_count(0);
  if (Failures != 0) {
    fprintf(stderr, "ancestor_store_test: %d failure(s)\n", Failures);
    return 1;
  }
  printf("ancestor_store_test: PASS\n");
  return 0;
}
