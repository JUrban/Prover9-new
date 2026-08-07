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

#ifndef TP_COMPRESS_H
#define TP_COMPRESS_H

#include "parautil.h"

/* INTRODUCTION
*/

/* Public definitions */

typedef enum {
  CLAUSE_COMPRESS_OK,
  CLAUSE_COMPRESS_ALREADY,
  CLAUSE_COMPRESS_NO_BODY,
  CLAUSE_COMPRESS_INVALID
} Clause_compress_result;

struct clause_compression_stats {
  unsigned long long attempted;
  unsigned long long successful;
  unsigned long long skipped;
  unsigned long long materialized;
  unsigned long long recompressed;
};

/* End of public definitions */

/* Public function prototypes from compress.c */

Term uncompress_term(char *s, int *ip);

char *compress_term(Term t);

/* Versioned, bounds-checkable term representation used by persistent
   ancestor records.  The returned byte array is owned by the caller. */
BOOL encode_term_versioned(Term t, char **data, unsigned *size);

Term decode_term_versioned(const char *data, unsigned size);

Clause_compress_result compress_clause(Topform c);

Clause_compress_result compress_clause_with_justification(Topform c);

void uncompress_clause(Topform c);

void uncompress_clauses(Plist p);

BOOL materialize_clause(Topform c);

BOOL recompress_clause(Topform c);

Plist materialize_clauses(Plist p);

void recompress_clauses(Plist p);

BOOL compressed_clause_is_valid(Topform c);

unsigned compressed_clause_justification_bytes(Topform c);

unsigned long long clause_body_storage_bytes(Topform c);

struct clause_compression_stats clause_compression_get_stats(void);

void clause_compression_reset_stats(void);

void discard_compressed_clause(Topform c);

#endif  /* conditional compilation of whole file */
