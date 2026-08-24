/*  Copyright (C) 2006, 2007 William McCune

    This file is part of the LADR Deduction Library.

    The LADR Deduction Library is free software; you can redistribute it
    and/or modify it under the terms of the GNU General Public License,
    version 2.

    The LADR Deduction Library is distributed in the hope that it will be
    useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with the LADR Deduction Library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "compress.h"

/* Private definitions and types */

#define CLAUSE_COMPRESS_MAGIC   0xd7
#define CLAUSE_COMPRESS_VERSION 2
#define CLAUSE_PACKED_VERSION   3
#define CLAUSE_PACKED_HEADER   10

static struct clause_compression_stats Compression_stats;

static
void put32le(String_buf sb, unsigned value)
{
  sb_append_char(sb, (char) (value & 0xff));
  sb_append_char(sb, (char) ((value >> 8) & 0xff));
  sb_append_char(sb, (char) ((value >> 16) & 0xff));
  sb_append_char(sb, (char) ((value >> 24) & 0xff));
}

static
unsigned get32le(const unsigned char *p)
{
  return (unsigned) p[0] |
         ((unsigned) p[1] << 8) |
         ((unsigned) p[2] << 16) |
         ((unsigned) p[3] << 24);
}

static
BOOL empty_justification_encoding(const unsigned char *data, unsigned size)
{
  return size == 5 && memcmp(data, "P9J\1", 4) == 0 && data[4] == 0;
}

struct term_frame {
  Term node;
  int next_child;
};

static
void zap_decoded_term(Term t)
{
  int cap = 128;
  int top = 0;
  Term fixed_stack[128];
  Term *stack = fixed_stack;

  if (t == NULL)
    return;
  stack[top++] = t;
  while (top > 0) {
    Term cur = stack[--top];
    int i;
    for (i = ARITY(cur) - 1; i >= 0; i--) {
      if (top >= cap) {
        Term *new_stack;
        cap *= 2;
        if (stack == fixed_stack) {
          new_stack = (Term *) safe_malloc(cap * sizeof(Term));
          memcpy(new_stack, fixed_stack, top * sizeof(Term));
        }
        else
          new_stack = (Term *) safe_realloc(stack, cap * sizeof(Term));
        stack = new_stack;
      }
      stack[top++] = ARG(cur, i);
    }
    free_term(cur);
  }
  if (stack != fixed_stack)
    safe_free(stack);
}  /* zap_decoded_term */

static
void append_uvarint(String_buf sb, unsigned value)
{
  do {
    unsigned byte = value & 0x7f;
    value >>= 7;
    if (value != 0)
      byte |= 0x80;
    sb_append_char(sb, (char) byte);
  } while (value != 0);
}  /* append_uvarint */

static
BOOL read_uvarint(const unsigned char *data, unsigned size,
                  unsigned *offset, unsigned *value)
{
  unsigned long long result = 0;
  int shift = 0;
  int count = 0;

  while (*offset < size && count < 5) {
    unsigned byte = data[(*offset)++];
    result |= ((unsigned long long) (byte & 0x7f)) << shift;
    count++;
    if ((byte & 0x80) == 0) {
      if (result > UINT_MAX)
        return FALSE;
      *value = (unsigned) result;
      return TRUE;
    }
    shift += 7;
  }
  return FALSE;
}  /* read_uvarint */

/* Read one preorder node from the versioned body stream, deliberately
   ignoring private_flags because term_ident()/match_hints() do not include
   them in term identity. */
static BOOL read_packed_node(const unsigned char *data, unsigned size,
                             unsigned *offset, BOOL *variable,
                             unsigned *number, unsigned *arity)
{
  unsigned code;
  unsigned flags;
  int a;
  if (!read_uvarint(data, size, offset, &code))
    return FALSE;
  *variable = (code & 1) == 0;
  *number = code >> 1;
  if (*variable) {
    if (*number >= MAX_VNUM)
      return FALSE;
    *arity = 0;
    return TRUE;
  }
  if (*number == 0 || *number > (unsigned) greatest_symnum() ||
      !read_uvarint(data, size, offset, &flags) || flags > UCHAR_MAX)
    return FALSE;
  a = sn_to_arity((int) *number);
  if (a < 0)
    return FALSE;
  *arity = (unsigned) a;
  return TRUE;
}

static BOOL skip_packed_children(const unsigned char *data, unsigned size,
                                 unsigned *offset, unsigned children)
{
  unsigned pending = children;
  while (pending != 0) {
    BOOL variable;
    unsigned number, arity;
    if (!read_packed_node(data, size, offset, &variable, &number, &arity))
      return FALSE;
    pending--;
    if (arity > UINT_MAX - pending)
      return FALSE;
    pending += arity;
  }
  return TRUE;
}

static BOOL packed_terms_ident(const unsigned char *data,
                               unsigned first_start, unsigned first_end,
                               unsigned second_start, unsigned second_end)
{
  unsigned a = first_start;
  unsigned b = second_start;
  while (a < first_end && b < second_end) {
    BOOL avar, bvar;
    unsigned anum, bnum, aarity, barity;
    if (!read_packed_node(data, first_end, &a, &avar, &anum, &aarity) ||
        !read_packed_node(data, second_end, &b, &bvar, &bnum, &barity) ||
        avar != bvar || anum != bnum || aarity != barity)
      return FALSE;
  }
  return a == first_end && b == second_end;
}

static BOOL skip_packed_children_profile(
  const unsigned char *data, unsigned size, unsigned *offset,
  unsigned children, unsigned long long *nodes)
{
  unsigned pending = children;
  while (pending != 0) {
    BOOL variable;
    unsigned number, arity;
    if (!read_packed_node(data, size, offset, &variable, &number, &arity))
      return FALSE;
    (*nodes)++;
    pending--;
    if (arity > UINT_MAX - pending)
      return FALSE;
    pending += arity;
  }
  return TRUE;
}

static BOOL packed_terms_ident_profile(
  const unsigned char *data,
  unsigned first_start, unsigned first_end,
  unsigned second_start, unsigned second_end,
  unsigned long long *nodes)
{
  unsigned a = first_start;
  unsigned b = second_start;
  while (a < first_end && b < second_end) {
    BOOL avar, bvar;
    unsigned anum, bnum, aarity, barity;
    if (!read_packed_node(data, first_end, &a, &avar, &anum, &aarity) ||
        !read_packed_node(data, second_end, &b, &bvar, &bnum, &barity))
      return FALSE;
    (*nodes)++;
    if (avar != bvar || anum != bnum || aarity != barity)
      return FALSE;
  }
  return a == first_end && b == second_end;
}

/* Locate the unit atom inside the right-associated clause encoding. */
static BOOL packed_unit_atom(Topform c, BOOL positive,
                             const unsigned char **data_out,
                             unsigned *start_out, unsigned *end_out)
{
  const unsigned char *data;
  unsigned size, offset = 2;
  if (c == NULL || c->compressed == NULL || c->compressed_size < 3 ||
      c->packed_justification)
    return FALSE;
  data = (const unsigned char *) c->compressed;
  size = c->compressed_size;
  if (data[0] != CLAUSE_COMPRESS_MAGIC ||
      data[1] != CLAUSE_COMPRESS_VERSION)
    return FALSE;
  if (!positive) {
    BOOL variable;
    unsigned number, arity;
    if (!read_packed_node(data, size, &offset, &variable, &number, &arity) ||
        variable || number != (unsigned) not_symnum() || arity != 1)
      return FALSE;
  }
  *data_out = data;
  *start_out = offset;
  *end_out = size;
  return TRUE;
}

BOOL compressed_unit_atom_visit(Topform compressed, BOOL *positive,
                                compressed_term_node_visitor visitor,
                                void *context)
{
  const unsigned char *data;
  unsigned offset, end, pending = 1;
  BOOL sign;
  if (compressed == NULL || positive == NULL || visitor == NULL)
    return FALSE;
  sign = !compressed->neg_compressed;
  if (!packed_unit_atom(compressed, sign, &data, &offset, &end))
    return FALSE;
  while (pending != 0) {
    BOOL variable;
    unsigned number, arity;
    if (!read_packed_node(
          data, end, &offset, &variable, &number, &arity) ||
        !visitor(context, variable, number, arity))
      return FALSE;
    pending--;
    if (arity > UINT_MAX - pending)
      return FALSE;
    pending += arity;
  }
  if (offset != end)
    return FALSE;
  *positive = sign;
  return TRUE;
}

/* Match an ordinary Term pattern against a preorder packed target. */
static BOOL resident_matches_packed(Term pattern,
                                    const unsigned char *data,
                                    unsigned start, unsigned end,
                                    BOOL *matched)
{
  Term stack[1000];
  unsigned bind_start[MAX_VARS];
  unsigned bind_end[MAX_VARS];
  unsigned char bound[MAX_VARS];
  int top = 0;
  unsigned offset = start;
  memset(bound, 0, sizeof(bound));
  stack[top++] = pattern;
  while (top > 0) {
    Term p = stack[--top];
    unsigned node_start = offset;
    BOOL variable;
    unsigned number, arity;
    if (!read_packed_node(data, end, &offset, &variable, &number, &arity))
      return FALSE;
    if (VARIABLE(p)) {
      unsigned vn = (unsigned) VARNUM(p);
      unsigned node_end;
      if (vn >= MAX_VARS ||
          !skip_packed_children(data, end, &offset, arity))
        return FALSE;
      node_end = offset;
      if (!bound[vn]) {
        bound[vn] = 1;
        bind_start[vn] = node_start;
        bind_end[vn] = node_end;
      }
      else if (!packed_terms_ident(data, bind_start[vn], bind_end[vn],
                                   node_start, node_end)) {
        *matched = FALSE;
        return TRUE;
      }
    }
    else {
      int i;
      if (variable || number != (unsigned) SYMNUM(p) ||
          arity != (unsigned) ARITY(p)) {
        *matched = FALSE;
        return TRUE;
      }
      if (top + ARITY(p) > (int) (sizeof(stack) / sizeof(stack[0])))
        return FALSE;
      for (i = ARITY(p) - 1; i >= 0; i--)
        stack[top++] = ARG(p,i);
    }
  }
  if (offset != end)
    return FALSE;
  *matched = TRUE;
  return TRUE;
}

/* Diagnostic traversal for the compiled-matcher census.  Unlike the hot
   matcher above, it continues after a repeated-variable mismatch so a later
   rigid mismatch is also visible.  That tells us how many candidates a
   rarity-ordered rigid test could reject before binding work. */
static BOOL resident_matches_packed_profile(
  Term pattern, const unsigned char *data, unsigned start, unsigned end,
  BOOL *matched, struct compressed_unit_match_profile *profile)
{
  Term stack[1000];
  unsigned bind_start[MAX_VARS];
  unsigned bind_end[MAX_VARS];
  unsigned char bound[MAX_VARS];
  int top = 0;
  unsigned offset = start;
  memset(bound, 0, sizeof(bound));
  memset(profile, 0, sizeof(*profile));
  stack[top++] = pattern;
  while (top > 0) {
    Term p = stack[--top];
    unsigned node_start = offset;
    BOOL variable;
    unsigned number, arity;
    if (!read_packed_node(data, end, &offset, &variable, &number, &arity))
      return FALSE;
    profile->stream_nodes++;
    if (VARIABLE(p)) {
      unsigned vn = (unsigned) VARNUM(p);
      unsigned node_end;
      unsigned long long child_nodes = 0;
      if (vn >= MAX_VARS ||
          !skip_packed_children_profile(data, end, &offset, arity,
                                        &child_nodes))
        return FALSE;
      profile->stream_nodes += child_nodes;
      profile->skipped_subterms++;
      profile->skipped_nodes += child_nodes + 1;
      profile->skipped_bytes += offset - node_start;
      node_end = offset;
      if (!bound[vn]) {
        bound[vn] = 1;
        bind_start[vn] = node_start;
        bind_end[vn] = node_end;
        profile->first_bindings++;
      }
      else {
        unsigned long long compared = 0;
        BOOL identical;
        profile->repeated_tests++;
        identical = packed_terms_ident_profile(
          data, bind_start[vn], bind_end[vn], node_start, node_end,
          &compared);
        profile->repeated_compare_nodes += compared;
        if (!identical)
          profile->reject_reasons |= COMPRESSED_UNIT_REJECT_REPEATED;
      }
    }
    else {
      int i;
      profile->rigid_tests++;
      if (variable || number != (unsigned) SYMNUM(p) ||
          arity != (unsigned) ARITY(p)) {
        profile->reject_reasons |= COMPRESSED_UNIT_REJECT_RIGID;
        *matched = FALSE;
        return TRUE;
      }
      if (top + ARITY(p) > (int) (sizeof(stack) / sizeof(stack[0])))
        return FALSE;
      for (i = ARITY(p) - 1; i >= 0; i--)
        stack[top++] = ARG(p,i);
    }
  }
  if (offset != end)
    return FALSE;
  *matched = profile->reject_reasons == 0;
  return TRUE;
}

/* Match a preorder packed pattern against an ordinary Term target. */
static BOOL packed_matches_resident(const unsigned char *data,
                                    unsigned start, unsigned end,
                                    Term target, BOOL *matched)
{
  Term stack[1000];
  Term bindings[MAX_VARS];
  int top = 0;
  unsigned offset = start;
  memset(bindings, 0, sizeof(bindings));
  stack[top++] = target;
  while (top > 0) {
    Term t = stack[--top];
    BOOL variable;
    unsigned number, arity;
    if (!read_packed_node(data, end, &offset, &variable, &number, &arity))
      return FALSE;
    if (variable) {
      if (number >= MAX_VARS)
        return FALSE;
      if (bindings[number] == NULL)
        bindings[number] = t;
      else if (!term_ident(bindings[number], t)) {
        *matched = FALSE;
        return TRUE;
      }
    }
    else {
      int i;
      if (VARIABLE(t) || number != (unsigned) SYMNUM(t) ||
          arity != (unsigned) ARITY(t)) {
        *matched = FALSE;
        return TRUE;
      }
      if (top + ARITY(t) > (int) (sizeof(stack) / sizeof(stack[0])))
        return FALSE;
      for (i = ARITY(t) - 1; i >= 0; i--)
        stack[top++] = ARG(t,i);
    }
  }
  if (offset != end)
    return FALSE;
  *matched = TRUE;
  return TRUE;
}

BOOL compressed_unit_target_matches(Literals resident,
                                    Topform compressed,
                                    BOOL *matched)
{
  const unsigned char *data;
  unsigned start, end;
  if (matched == NULL || compressed == NULL || resident == NULL ||
      resident->next != NULL)
    return FALSE;
  if (resident->sign == compressed->neg_compressed) {
    *matched = FALSE;
    return TRUE;
  }
  if (!packed_unit_atom(compressed, resident->sign, &data, &start, &end))
    return FALSE;
  return resident_matches_packed(resident->atom, data, start, end, matched);
}

BOOL compressed_unit_target_match_profile(
  Literals resident, Topform compressed, BOOL *matched,
  struct compressed_unit_match_profile *profile)
{
  const unsigned char *data;
  unsigned start, end;
  if (matched == NULL || profile == NULL || compressed == NULL ||
      resident == NULL || resident->next != NULL)
    return FALSE;
  memset(profile, 0, sizeof(*profile));
  if (resident->sign == compressed->neg_compressed) {
    profile->reject_reasons = COMPRESSED_UNIT_REJECT_SIGN;
    *matched = FALSE;
    return TRUE;
  }
  if (!packed_unit_atom(compressed, resident->sign, &data, &start, &end))
    return FALSE;
  return resident_matches_packed_profile(
    resident->atom, data, start, end, matched, profile);
}

BOOL compressed_unit_pattern_matches(Topform compressed,
                                     Literals resident,
                                     BOOL *matched)
{
  const unsigned char *data;
  unsigned start, end;
  if (matched == NULL || compressed == NULL || resident == NULL ||
      resident->next != NULL)
    return FALSE;
  if (resident->sign == compressed->neg_compressed) {
    *matched = FALSE;
    return TRUE;
  }
  if (!packed_unit_atom(compressed, resident->sign, &data, &start, &end))
    return FALSE;
  return packed_matches_resident(data, start, end, resident->atom, matched);
}

static
void append_varint_term(String_buf sb, Term t)
{
  int cap = 128;
  int top = 0;
  Term *stack = (Term *) safe_malloc(cap * sizeof(Term));

  stack[top++] = t;
  while (top > 0) {
    Term cur = stack[--top];
    unsigned code;
    int i;

    if (VARIABLE(cur))
      code = ((unsigned) VARNUM(cur)) << 1;
    else
      code = (((unsigned) SYMNUM(cur)) << 1) | 1;
    append_uvarint(sb, code);
    if (!VARIABLE(cur))
      append_uvarint(sb, (unsigned) cur->private_flags);

    for (i = ARITY(cur) - 1; i >= 0; i--) {
      if (top >= cap) {
        cap *= 2;
        stack = (Term *) safe_realloc(stack, cap * sizeof(Term));
      }
      stack[top++] = ARG(cur, i);
    }
  }
  safe_free(stack);
}  /* append_varint_term */

static
Term decode_varint_term(const unsigned char *data, unsigned size)
{
  int cap = 128;
  int top = 0;
  unsigned offset = 2;
  unsigned version;
  Term root = NULL;
  struct term_frame *stack;
  BOOL valid = TRUE;

  if (size < 3 || data[0] != CLAUSE_COMPRESS_MAGIC ||
      (data[1] != 1 && data[1] != CLAUSE_COMPRESS_VERSION))
    return NULL;
  version = data[1];

  stack = (struct term_frame *)
    safe_malloc(cap * sizeof(struct term_frame));

  while (offset < size && valid) {
    unsigned code;
    int arity;
    Term t;
    int i;

    if (!read_uvarint(data, size, &offset, &code)) {
      valid = FALSE;
      break;
    }

    if ((code & 1) == 0) {
      unsigned varnum = code >> 1;
      if (varnum >= MAX_VNUM) {
        valid = FALSE;
        break;
      }
      t = get_variable_term((int) varnum);
      arity = 0;
    }
    else {
      unsigned symnum = code >> 1;
      unsigned flags = 0;
      if (symnum == 0 || symnum > (unsigned) greatest_symnum()) {
        valid = FALSE;
        break;
      }
      arity = sn_to_arity((int) symnum);
      if (arity < 0) {
        valid = FALSE;
        break;
      }
      if (version >= 2 &&
          (!read_uvarint(data, size, &offset, &flags) || flags > UCHAR_MAX)) {
        valid = FALSE;
        break;
      }
      t = get_rigid_term_dangerously((int) symnum, arity);
      t->private_flags = (FLAGS_TYPE) flags;
      /* Make a partially built tree safe to destroy on malformed input. */
      for (i = 0; i < arity; i++)
        ARG(t, i) = get_variable_term(0);
    }

    if (root == NULL)
      root = t;
    else if (top == 0) {
      if (!VARIABLE(t))
        zap_decoded_term(t);
      valid = FALSE;
      break;
    }
    else
      ARG(stack[top-1].node, stack[top-1].next_child++) = t;

    if (arity > 0) {
      if (top >= cap) {
        cap *= 2;
        stack = (struct term_frame *)
          safe_realloc(stack, cap * sizeof(struct term_frame));
      }
      stack[top].node = t;
      stack[top].next_child = 0;
      top++;
    }

    while (top > 0 &&
           stack[top-1].next_child >= ARITY(stack[top-1].node))
      top--;

    if (root != NULL && top == 0)
      break;
  }

  if (root == NULL || top != 0 || offset != size)
    valid = FALSE;

  safe_free(stack);
  if (!valid) {
    if (root != NULL)
      zap_decoded_term(root);
    return NULL;
  }
  return root;
}  /* decode_varint_term */

/* PUBLIC */
BOOL encode_term_versioned(Term t, char **data, unsigned *size)
{
  String_buf sb;
  unsigned n;

  if (t == NULL || data == NULL || size == NULL)
    return FALSE;
  sb = get_string_buf();
  sb_append_char(sb, (char) CLAUSE_COMPRESS_MAGIC);
  sb_append_char(sb, (char) CLAUSE_COMPRESS_VERSION);
  append_varint_term(sb, t);
  n = (unsigned) sb_size(sb);
  *data = sb_to_malloc_char_array(sb);
  *size = n;
  zap_string_buf(sb);
  return TRUE;
}  /* encode_term_versioned */

/* PUBLIC */
Term decode_term_versioned(const char *data, unsigned size)
{
  if (data == NULL)
    return NULL;
  return decode_varint_term((const unsigned char *) data, size);
}  /* decode_term_versioned */

/* Convert the right-associated clause term produced by lits_to_term() back
   to literals by transferring atom ownership.  Unlike term_to_literals(),
   this does not copy atoms and therefore retains every private term flag. */
static
Literals take_literals_from_term(Term t)
{
  Literals first = NULL;
  Literals *last = &first;
  Term p = t;

  while (is_term(p, or_sym(), 2)) {
    Term item = ARG(p, 0);
    Literals lit = get_literals();
    lit->sign = !is_term(item, not_sym(), 1);
    lit->atom = lit->sign ? item : ARG(item, 0);
    *last = lit;
    last = &lit->next;
    p = ARG(p, 1);
  }
  {
    Literals lit = get_literals();
    lit->sign = !is_term(p, not_sym(), 1);
    lit->atom = lit->sign ? p : ARG(p, 0);
    *last = lit;
  }
  return first;
}  /* take_literals_from_term */

static
Clause_compress_result compress_clause_raw(Topform c, BOOL pack_justification)
{
  Term t;
  String_buf sb;
  unsigned long long body_bytes;
  unsigned n;
  char *body_data = NULL;
  char *just_data = NULL;
  unsigned body_size = 0;
  unsigned just_size = 0;

  if (c == NULL)
    return CLAUSE_COMPRESS_INVALID;
  if (c->compressed != NULL)
    return CLAUSE_COMPRESS_ALREADY;
  if (c->literals == NULL || c->is_formula)
    return CLAUSE_COMPRESS_NO_BODY;

  body_bytes = clause_body_storage_bytes(c);
  t = lits_to_term(c->literals);
  sb = get_string_buf();
  if (pack_justification) {
    unsigned i;
    if (!encode_term_versioned(t, &body_data, &body_size) ||
        !encode_justification(c->justification, &just_data, &just_size)) {
      safe_free(body_data);
      safe_free(just_data);
      zap_string_buf(sb);
      free_lits_to_term(t);
      return CLAUSE_COMPRESS_INVALID;
    }
    if (body_size > UINT_MAX - just_size - CLAUSE_PACKED_HEADER) {
      safe_free(body_data);
      safe_free(just_data);
      zap_string_buf(sb);
      free_lits_to_term(t);
      return CLAUSE_COMPRESS_INVALID;
    }
    sb_append_char(sb, (char) CLAUSE_COMPRESS_MAGIC);
    sb_append_char(sb, (char) CLAUSE_PACKED_VERSION);
    put32le(sb, body_size);
    put32le(sb, just_size);
    for (i = 0; i < body_size; i++)
      sb_append_char(sb, body_data[i]);
    for (i = 0; i < just_size; i++)
      sb_append_char(sb, just_data[i]);
    safe_free(body_data);
    safe_free(just_data);
  }
  else {
    sb_append_char(sb, (char) CLAUSE_COMPRESS_MAGIC);
    sb_append_char(sb, (char) CLAUSE_COMPRESS_VERSION);
    append_varint_term(sb, t);
  }
  n = (unsigned) sb_size(sb);
  c->compressed = (char *) safe_malloc(n);
  {
    unsigned i;
    for (i = 0; i < n; i++)
      c->compressed[i] = sb_char(sb, (int) i);
  }
  c->compressed_size = n;
  c->uncompressed_body_bytes =
    body_bytes > UINT_MAX ? UINT_MAX : (unsigned) body_bytes;
  zap_string_buf(sb);
  free_lits_to_term(t);
  c->neg_compressed = negative_clause(c->literals);
  zap_literals(c->literals);
  c->literals = NULL;
  c->packed_justification = pack_justification;
  if (pack_justification) {
    zap_just(c->justification);
    c->justification = NULL;
  }
  return CLAUSE_COMPRESS_OK;
}  /* compress_clause_raw */

/*************
 *
 *   uncompress_term()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
Term uncompress_term(char *s, int *ip)
{
  /* Iterative term construction using explicit stack. */
  struct { Term node; int child; } stack[1000];
  int top = -1;
  Term result = NULL;

  for (;;) {
    char c = s[(*ip)++];
    Term t;
    if (c <= 0) {
      t = get_variable_term(-c);
    }
    else {
      int arity = sn_to_arity(c);
      t = get_rigid_term_dangerously(c, arity);
      if (arity > 0) {
        top++;
        stack[top].node = t;
        stack[top].child = 0;
        continue;  /* need to build children */
      }
    }

    /* t is a leaf (variable or constant); attach it and backtrack. */
    if (top < 0) {
      result = t;
      break;
    }

    ARG(stack[top].node, stack[top].child) = t;
    stack[top].child++;

    /* Pop completed frames. */
    while (top >= 0 && stack[top].child >= ARITY(stack[top].node)) {
      t = stack[top].node;
      top--;
      if (top < 0) {
        result = t;
        goto done;
      }
      ARG(stack[top].node, stack[top].child) = t;
      stack[top].child++;
    }
  }
done:
  return result;
}  /* uncompress_term */

/*************
 *
 *   compress_term_recurse()
 *
 *************/

static
void compress_term_recurse(String_buf sb, Term t)
{
  /* Iterative pre-order traversal using explicit stack. */
  Term stack[1000];
  int top = 0;
  stack[0] = t;

  while (top >= 0) {
    Term cur = stack[top];
    top--;

    if (VARIABLE(cur)) {
      sb_append_char(sb, -VARNUM(cur));
    }
    else {
      int i;
      sb_append_char(sb, SYMNUM(cur));
      /* Push children in reverse order so leftmost is processed first. */
      for (i = ARITY(cur) - 1; i >= 0; i--) {
        top++;
        stack[top] = ARG(cur, i);
      }
    }
  }
}  /* compress_term_recurse */

/*************
 *
 *   compress_term()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
char *compress_term(Term t)
{
  String_buf sb = get_string_buf();
  compress_term_recurse(sb, t);
  {
    char *s;
    s = sb_to_malloc_char_array(sb);
    zap_string_buf(sb);
    return s;
  }
}  /* compress_term */

/*************
 *
 *   compress_clause()
 *
 *************/

/* DOCUMENTATION
The versioned compact representation preserves private flags on every rigid
term, including equality-orientation marks.  The decoder also accepts the
older version 1 representation and reconstructs its missing orientation marks.
*/

/* PUBLIC */
Clause_compress_result compress_clause(Topform c)
{
  Clause_compress_result result;
  Compression_stats.attempted++;
  result = compress_clause_raw(c, FALSE);
  if (result == CLAUSE_COMPRESS_OK)
    Compression_stats.successful++;
  else
    Compression_stats.skipped++;
  return result;
}  /* compress_clause */

/* PUBLIC */
Clause_compress_result compress_clause_with_justification(Topform c)
{
  Clause_compress_result result;
  Compression_stats.attempted++;
  result = compress_clause_raw(c, TRUE);
  if (result == CLAUSE_COMPRESS_OK)
    Compression_stats.successful++;
  else
    Compression_stats.skipped++;
  return result;
}  /* compress_clause_with_justification */

/*************
 *
 *   uncompress_clause()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void uncompress_clause(Topform c)
{
  if (c->compressed) {
    if (!materialize_clause(c))
      fatal_error("uncompress_clause: invalid compressed clause");
  }
}  /* uncompress_clause */

/*************
 *
 *   uncompress_clauses()
 *
 *************/

/* DOCUMENTATION
Given a Plist of clauses, uncompress the compressed ones.
*/

/* PUBLIC */
void uncompress_clauses(Plist p)
{
  Plist a;
  for (a = p; a; a = a->next) {
    Topform c = a->v;
    if (c->compressed && !materialize_clause(c))
      fatal_error("uncompress_clauses: invalid compressed clause");
  }
}  /* uncompress_clauses */

/* PUBLIC */
BOOL materialize_clause(Topform c)
{
  Term t;
  unsigned version;
  Just justification = NULL;
  BOOL packed = FALSE;

  if (c == NULL || c->compressed == NULL || c->literals != NULL ||
      c->compressed_size < 2)
    return FALSE;
  version = (unsigned char) c->compressed[1];
  if (version == CLAUSE_PACKED_VERSION) {
    const unsigned char *data = (const unsigned char *) c->compressed;
    unsigned body_size, just_size;
    if (c->compressed_size < CLAUSE_PACKED_HEADER)
      return FALSE;
    body_size = get32le(data + 2);
    just_size = get32le(data + 6);
    if (body_size > c->compressed_size - CLAUSE_PACKED_HEADER ||
        just_size != c->compressed_size - CLAUSE_PACKED_HEADER - body_size)
      return FALSE;
    t = decode_term_versioned((const char *) data + CLAUSE_PACKED_HEADER,
                              body_size);
    if (t == NULL)
      return FALSE;
    justification = decode_justification(
      (const char *) data + CLAUSE_PACKED_HEADER + body_size, just_size);
    if (justification == NULL &&
        !empty_justification_encoding(
          data + CLAUSE_PACKED_HEADER + body_size, just_size)) {
      zap_decoded_term(t);
      return FALSE;
    }
    packed = TRUE;
  }
  else
    t = decode_varint_term((unsigned char *) c->compressed,
                           c->compressed_size);
  if (t == NULL)
    return FALSE;
  c->literals = take_literals_from_term(t);
  if (packed)
    c->justification = justification;
  upward_clause_links(c);
  free_lits_to_term(t);  /* frees only OR/NOT wrappers, not transferred atoms */
  safe_free(c->compressed);
  c->compressed = NULL;
  c->compressed_size = 0;
  c->uncompressed_body_bytes = 0;
  c->neg_compressed = FALSE;
  c->packed_justification = packed;
  if (version == 1)
    orient_equalities(c, FALSE);  /* version 1 did not retain term flags */
  Compression_stats.materialized++;
  return TRUE;
}  /* materialize_clause */

/* PUBLIC */
BOOL recompress_clause(Topform c)
{
  Clause_compress_result result =
    compress_clause_raw(c, c != NULL && c->packed_justification);
  if (result == CLAUSE_COMPRESS_OK) {
    Compression_stats.recompressed++;
    return TRUE;
  }
  return FALSE;
}  /* recompress_clause */

/* PUBLIC */
Plist materialize_clauses(Plist p)
{
  Plist changed = NULL;
  Plist q;
  for (q = p; q != NULL; q = q->next) {
    Topform c = q->v;
    if (c->compressed != NULL) {
      if (!materialize_clause(c))
        fatal_error("materialize_clauses: invalid compressed clause");
      changed = plist_prepend(changed, c);
    }
  }
  return changed;
}  /* materialize_clauses */

/* PUBLIC */
void recompress_clauses(Plist p)
{
  Plist q;
  for (q = p; q != NULL; q = q->next)
    if (!recompress_clause((Topform) q->v))
      fatal_error("recompress_clauses: clause could not be recompressed");
}  /* recompress_clauses */

/* PUBLIC */
BOOL compressed_clause_is_valid(Topform c)
{
  Term t;
  Just justification = NULL;
  if (c == NULL || c->compressed == NULL)
    return FALSE;
  if ((unsigned char) c->compressed[1] == CLAUSE_PACKED_VERSION) {
    const unsigned char *data = (const unsigned char *) c->compressed;
    unsigned body_size, just_size;
    if (c->compressed_size < CLAUSE_PACKED_HEADER)
      return FALSE;
    body_size = get32le(data + 2);
    just_size = get32le(data + 6);
    if (body_size > c->compressed_size - CLAUSE_PACKED_HEADER ||
        just_size != c->compressed_size - CLAUSE_PACKED_HEADER - body_size)
      return FALSE;
    t = decode_term_versioned((const char *) data + CLAUSE_PACKED_HEADER,
                              body_size);
    if (t == NULL)
      return FALSE;
    justification = decode_justification(
      (const char *) data + CLAUSE_PACKED_HEADER + body_size, just_size);
    if (justification == NULL &&
        !empty_justification_encoding(
          data + CLAUSE_PACKED_HEADER + body_size, just_size)) {
      zap_decoded_term(t);
      return FALSE;
    }
  }
  else
    t = decode_varint_term((unsigned char *) c->compressed,
                           c->compressed_size);
  if (t == NULL)
    return FALSE;
  zap_decoded_term(t);
  zap_just(justification);
  return TRUE;
}  /* compressed_clause_is_valid */

/* PUBLIC */
unsigned compressed_clause_justification_bytes(Topform c)
{
  const unsigned char *data;
  if (c == NULL || c->compressed == NULL || c->compressed_size < 10 ||
      (unsigned char) c->compressed[1] != CLAUSE_PACKED_VERSION)
    return 0;
  data = (const unsigned char *) c->compressed;
  return get32le(data + 6);
}  /* compressed_clause_justification_bytes */

/* PUBLIC */
unsigned long long clause_body_storage_bytes(Topform c)
{
  unsigned long long bytes = 0;
  Literals lit;
  int cap = 128;
  Term fixed_stack[128];
  Term *stack = fixed_stack;

  if (c == NULL || c->literals == NULL)
    return 0;

  for (lit = c->literals; lit != NULL; lit = lit->next) {
    int top = 0;
    bytes += PTRS(sizeof(struct literals)) * BYTES_POINTER;
    stack[top++] = lit->atom;
    while (top > 0) {
      Term t = stack[--top];
      int i;
      if (!VARIABLE(t))
        bytes += (PTRS(sizeof(struct term)) + ARITY(t)) * BYTES_POINTER;
      for (i = ARITY(t) - 1; i >= 0; i--) {
        if (top >= cap) {
          Term *new_stack;
          cap *= 2;
          if (stack == fixed_stack) {
            new_stack = (Term *) safe_malloc(cap * sizeof(Term));
            memcpy(new_stack, fixed_stack, top * sizeof(Term));
          }
          else
            new_stack = (Term *) safe_realloc(stack, cap * sizeof(Term));
          stack = new_stack;
        }
        stack[top++] = ARG(t, i);
      }
    }
  }
  if (stack != fixed_stack)
    safe_free(stack);
  return bytes;
}  /* clause_body_storage_bytes */

/* PUBLIC */
struct clause_compression_stats clause_compression_get_stats(void)
{
  return Compression_stats;
}  /* clause_compression_get_stats */

/* PUBLIC */
void clause_compression_reset_stats(void)
{
  memset(&Compression_stats, 0, sizeof(Compression_stats));
}  /* clause_compression_reset_stats */

/* PUBLIC */
void discard_compressed_clause(Topform c)
{
  if (c != NULL && c->compressed != NULL) {
    safe_free(c->compressed);
    c->compressed = NULL;
    c->compressed_size = 0;
    c->uncompressed_body_bytes = 0;
    c->neg_compressed = FALSE;
    c->packed_justification = FALSE;
  }
}  /* discard_compressed_clause */
