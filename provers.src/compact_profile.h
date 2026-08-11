#ifndef TP_COMPACT_PROFILE_H
#define TP_COMPACT_PROFILE_H

#include <limits.h>
#include <stddef.h>
#include <stdio.h>

#define COMPACT_PROFILE_BUCKETS 8
#define COMPACT_PROFILE_LOG_BUCKETS 65

/* Constant-space query distributions shared by the compact indexes.  The
   eight public buckets make reports comparable with packed hints.  The
   power-of-two histograms retain enough tail detail to report conservative
   percentile upper bounds without storing one value per query. */
struct compact_query_profile {
  unsigned long long queries;
  unsigned long long candidates;
  unsigned long long work;
  unsigned long long live_examined;
  unsigned long long dead_examined;
  unsigned long long duplicates;
  unsigned long long successes;
  unsigned long long bytes_decoded;
  unsigned long long exact_tests;
  unsigned long long exact_successes;
  unsigned long long materializations;
  unsigned long long candidate_max;
  unsigned long long work_max;
  unsigned long long exact_max;
  unsigned long long candidate_buckets[COMPACT_PROFILE_BUCKETS];
  unsigned long long candidate_log[COMPACT_PROFILE_LOG_BUCKETS];
  unsigned long long work_log[COMPACT_PROFILE_LOG_BUCKETS];
  unsigned long long exact_log[COMPACT_PROFILE_LOG_BUCKETS];
};

static inline unsigned compact_profile_answer_bucket(unsigned long long value)
{
  if (value == 0) return 0;
  else if (value == 1) return 1;
  else if (value <= 7) return 2;
  else if (value <= 31) return 3;
  else if (value <= 127) return 4;
  else if (value <= 1023) return 5;
  else if (value <= 16383) return 6;
  else return 7;
}

static inline unsigned compact_profile_log_bucket(unsigned long long value)
{
  unsigned bucket = 0;
  if (value != 0) {
    value--;
    bucket = 1;
    while (value != 0 && bucket + 1 < COMPACT_PROFILE_LOG_BUCKETS) {
      value >>= 1;
      bucket++;
    }
  }
  return bucket;
}

static inline void compact_profile_note(
  struct compact_query_profile *profile,
  unsigned long long candidates,
  unsigned long long work,
  unsigned long long live_examined,
  unsigned long long dead_examined,
  unsigned long long duplicates,
  unsigned long long successes,
  unsigned long long bytes_decoded)
{
  profile->queries++;
  profile->candidates += candidates;
  profile->work += work;
  profile->live_examined += live_examined;
  profile->dead_examined += dead_examined;
  profile->duplicates += duplicates;
  profile->successes += successes;
  profile->bytes_decoded += bytes_decoded;
  if (candidates > profile->candidate_max)
    profile->candidate_max = candidates;
  if (work > profile->work_max)
    profile->work_max = work;
  profile->candidate_buckets[compact_profile_answer_bucket(candidates)]++;
  profile->candidate_log[compact_profile_log_bucket(candidates)]++;
  profile->work_log[compact_profile_log_bucket(work)]++;
}

static inline void compact_profile_note_exact(
  struct compact_query_profile *profile,
  unsigned long long tests,
  unsigned long long successes,
  unsigned long long materializations)
{
  profile->exact_tests += tests;
  profile->exact_successes += successes;
  profile->materializations += materializations;
  if (tests > profile->exact_max)
    profile->exact_max = tests;
  profile->exact_log[compact_profile_log_bucket(tests)]++;
}

/* Return the inclusive upper bound of the power-of-two bucket containing the
   requested percentile.  A return of ULLONG_MAX means the value reached the
   open-ended final bucket. */
static inline unsigned long long compact_profile_quantile_upper(
  const unsigned long long histogram[COMPACT_PROFILE_LOG_BUCKETS],
  unsigned long long observations, unsigned numerator, unsigned denominator)
{
  unsigned long long target, remainder, cumulative = 0;
  unsigned bucket;
  if (observations == 0 || denominator == 0)
    return 0;
  target = (observations / denominator) * numerator;
  remainder = (observations % denominator) * numerator;
  target += remainder / denominator;
  if (remainder % denominator != 0)
    target++;
  if (target == 0)
    target = 1;
  for (bucket = 0; bucket < COMPACT_PROFILE_LOG_BUCKETS; bucket++) {
    cumulative += histogram[bucket];
    if (cumulative >= target) {
      if (bucket == 0)
        return 0;
      if (bucket >= sizeof(unsigned long long) * CHAR_BIT)
        return ULLONG_MAX;
      return 1ULL << (bucket - 1);
    }
  }
  return ULLONG_MAX;
}

static inline void compact_profile_fprint(
  FILE *fp, const char *component, const char *operation,
  const struct compact_query_profile *profile, double seconds)
{
  fprintf(fp,
          "Compact_query_profile: component=%s, op=%s, seconds=%.3f, "
          "queries=%llu, candidates=%llu, work=%llu, live=%llu, dead=%llu, "
          "duplicates=%llu, successes=%llu, exact_tests=%llu, "
          "exact_successes=%llu, materialized=%llu, bytes_decoded=%llu, "
          "candidate_p50_upper=%llu, candidate_p95_upper=%llu, "
          "candidate_p99_upper=%llu, candidate_max=%llu, "
          "work_p50_upper=%llu, work_p95_upper=%llu, work_p99_upper=%llu, "
          "work_max=%llu, exact_p50_upper=%llu, exact_p95_upper=%llu, "
          "exact_p99_upper=%llu, exact_max=%llu, "
          "buckets=0:%llu/1:%llu/2-7:%llu/8-31:%llu/32-127:%llu/"
          "128-1023:%llu/1024-16383:%llu/16384+:%llu.\n",
          component, operation, seconds, profile->queries,
          profile->candidates, profile->work, profile->live_examined,
          profile->dead_examined, profile->duplicates, profile->successes,
          profile->exact_tests, profile->exact_successes,
          profile->materializations, profile->bytes_decoded,
          compact_profile_quantile_upper(profile->candidate_log,
                                         profile->queries, 50, 100),
          compact_profile_quantile_upper(profile->candidate_log,
                                         profile->queries, 95, 100),
          compact_profile_quantile_upper(profile->candidate_log,
                                         profile->queries, 99, 100),
          profile->candidate_max,
          compact_profile_quantile_upper(profile->work_log,
                                         profile->queries, 50, 100),
          compact_profile_quantile_upper(profile->work_log,
                                         profile->queries, 95, 100),
          compact_profile_quantile_upper(profile->work_log,
                                         profile->queries, 99, 100),
          profile->work_max,
          compact_profile_quantile_upper(profile->exact_log,
                                         profile->queries, 50, 100),
          compact_profile_quantile_upper(profile->exact_log,
                                         profile->queries, 95, 100),
          compact_profile_quantile_upper(profile->exact_log,
                                         profile->queries, 99, 100),
          profile->exact_max,
          profile->candidate_buckets[0], profile->candidate_buckets[1],
          profile->candidate_buckets[2], profile->candidate_buckets[3],
          profile->candidate_buckets[4], profile->candidate_buckets[5],
          profile->candidate_buckets[6], profile->candidate_buckets[7]);
}

#endif
