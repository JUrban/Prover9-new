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

#include "clock.h"

/* Private definitions and types */

struct clock {
  char       *name;                 /* name of clock */
  unsigned   accum_msec;            /* compatibility exact-mode total */
  unsigned   curr_msec;             /* compatibility exact-mode start */
  unsigned long long exact_usec;    /* exactly timed intervals */
  unsigned long long sample_usec;   /* timed post-warmup samples */
  unsigned long long exact_intervals;
  unsigned long long sample_intervals;
  unsigned long long post_intervals;
  unsigned long long curr_usec;     /* start of current sampled interval */
  unsigned   sample_state;
  BOOL       curr_sampled;
  BOOL       curr_exact;
  int        level;                 /* STARTs - STOPs */
};

static BOOL Clocks_enabled = TRUE;   /* getrusage can be slow */
static unsigned Clock_sample_rate = 1;
static unsigned Clock_sample_threshold = UINT_MAX;
static double User_seconds_offset = 0.0;  /* added to user_seconds for resume */
static unsigned Clock_starts = 0;         /* compatibility exact-mode count */
static unsigned long long Sample_clock_starts = 0;
static unsigned long long Clock_samples = 0;
static unsigned long long Clock_time_reads = 0;

#define CLOCK_SAMPLE_WARMUP 1024

#if defined(__GNUC__) || defined(__clang__)
#define CLOCK_NOINLINE __attribute__((noinline))
#else
#define CLOCK_NOINLINE
#endif

static unsigned Wall_start;          /* for measuring wall-clock time */

/* Keep the compatibility path identical to the original millisecond clock:
   it is deliberately a macro so rate 1 does not pay a new helper call. */

#ifdef PRIMITIVE_ENVIRONMENT
#define CPU_TIME_MSEC(msec) \
  { msec = (unsigned) (clock() / (CLOCKS_PER_SEC / 1000)); }
#else
#define CPU_TIME_MSEC(msec) \
  { struct rusage r; getrusage(RUSAGE_SELF, &r); \
    msec = r.ru_utime.tv_sec * 1000 + r.ru_utime.tv_usec / 1000; }
#endif

/* Return user CPU microseconds for a detailed clock sample.  This remains
   getrusage-based so exact mode has the same user-time semantics as before.
   On Linux the call enters the kernel, which is why high-frequency clients
   can opt into deterministic interval sampling. */

static
unsigned long long clock_user_usec(void)
{
  Clock_time_reads++;
#ifdef PRIMITIVE_ENVIRONMENT
  return ((unsigned long long) clock() * 1000000) / CLOCKS_PER_SEC;
#else
  {
    struct rusage r;
    getrusage(RUSAGE_SELF, &r);
    return (unsigned long long) r.ru_utime.tv_sec * 1000000 +
      (unsigned long long) r.ru_utime.tv_usec;
  }
#endif
}  /* clock_user_usec */

static
double clock_value_usec(Clock p)
{
  unsigned long long exact = p->exact_usec;
  unsigned long long sampled = p->sample_usec;

  if (p->level > 0 && p->curr_sampled) {
    unsigned long long delta = clock_user_usec() - p->curr_usec;
    if (p->curr_exact)
      exact += delta;
    else
      sampled += delta;
  }

  if (p->post_intervals == 0 || p->sample_intervals == 0)
    return (double) exact;
  else
    return (double) exact +
      ((double) sampled * (double) p->post_intervals /
       (double) p->sample_intervals);
}  /* clock_value_usec */

static
unsigned clock_name_seed(const char *s)
{
  unsigned seed = 2166136261U;
  while (*s) {
    seed ^= (unsigned char) *s++;
    seed *= 16777619U;
  }
  return seed == 0 ? 1 : seed;
}  /* clock_name_seed */

static
BOOL clock_choose_sample(Clock p)
{
  unsigned x = p->sample_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  p->sample_state = x == 0 ? 1 : x;
  return p->sample_intervals == 0 || x <= Clock_sample_threshold;
}  /* clock_choose_sample */

static CLOCK_NOINLINE
void sampled_clock_start(Clock p)
{
  p->level++;
  if (p->level == 1) {
    Sample_clock_starts++;
    if (p->exact_intervals < CLOCK_SAMPLE_WARMUP) {
      p->exact_intervals++;
      p->curr_sampled = TRUE;
      p->curr_exact = TRUE;
    }
    else {
      p->post_intervals++;
      p->curr_exact = FALSE;
      if (clock_choose_sample(p)) {
        p->sample_intervals++;
        p->curr_sampled = TRUE;
      }
      else
        p->curr_sampled = FALSE;
    }
    if (p->curr_sampled) {
      p->curr_usec = clock_user_usec();
      Clock_samples++;
    }
  }
}  /* sampled_clock_start */

static CLOCK_NOINLINE
void sampled_clock_stop(Clock p)
{
  if (p->level <= 0)
    fprintf(stderr,"WARNING, clock_stop: clock %s not running.\n",p->name);
  else {
    p->level--;
    if (p->level == 0 && p->curr_sampled) {
      unsigned long long usec = clock_user_usec();
      if (p->curr_exact)
        p->exact_usec += usec - p->curr_usec;
      else
        p->sample_usec += usec - p->curr_usec;
    }
  }
}  /* sampled_clock_stop */

/*
 * memory management
 */

#define PTRS_CLOCK PTRS(sizeof(struct clock))
static unsigned Clock_gets, Clock_frees;

/*************
 *
 *   Clock get_clock()
 *
 *************/

static
Clock get_clock(void)
{
  Clock p = get_cmem(PTRS_CLOCK);
  Clock_gets++;
  return(p);
}  /* get_clock */

/*************
 *
 *    free_clock()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void free_clock(Clock p)
{
  if (p != NULL) {
    free_mem(p, PTRS_CLOCK);
    Clock_frees++;
  }
}  /* free_clock */

/*
 *  end of memory management
 */

/*************
 *
 *   clock_init()
 *
 *************/

/* DOCUMENTATION
This routine initializes a clock.  You give it a string
(which is not copied), representing the name of the new clock,
and it returns a Clock to be used for all operations on the clock.
<P>
The clock operations are clock_start(), clock_stop(),
clock_seconds(), clock_milliseconds(), and clock_reset().
*/

/* PUBLIC */
Clock clock_init(char *str)
{
  Clock p = get_clock();
  p->name = str;
  p->level = 0;
  p->accum_msec = 0;
  p->curr_msec = 0;
  p->exact_usec = 0;
  p->sample_usec = 0;
  p->exact_intervals = 0;
  p->sample_intervals = 0;
  p->post_intervals = 0;
  p->curr_usec = 0;
  p->sample_state = clock_name_seed(str);
  p->curr_sampled = FALSE;
  p->curr_exact = FALSE;
  return p;
}  /* clock_init */

/*************
 *
 *   clock_start()
 *
 *************/

/* DOCUMENTATION
This routine starts clock n.  It is okay if the clock is already going.
*/

/* PUBLIC */
void clock_start(Clock p)
{
  if (Clocks_enabled) {
    if (Clock_sample_rate > 1) {
      sampled_clock_start(p);
      return;
    }
    p->level++;
    if (p->level == 1) {
      CPU_TIME_MSEC(p->curr_msec);
      Clock_starts++;
    }
  }
}  /* clock_start */

/*************
 *
 *   clock_stop()
 *
 *************/

/* DOCUMENTATION
This routine stops clock n and adds the time to the accumulated total,
<I>unless there have been too many starts and not enough stops</I>.
See the introduction.
*/

/* PUBLIC */
void clock_stop(Clock p)
{
  if (Clocks_enabled) {
    if (Clock_sample_rate > 1) {
      sampled_clock_stop(p);
      return;
    }
    if (p->level <= 0)
      fprintf(stderr,"WARNING, clock_stop: clock %s not running.\n",p->name); 
    else {
      p->level--;
      if (p->level == 0) {
	unsigned msec;
	CPU_TIME_MSEC(msec);
	p->accum_msec += msec - p->curr_msec;
      }
    }
  }
}  /* clock_stop */

/*************
 *
 *   clock_milliseconds()
 *
 *************/

/* DOCUMENTATION
This routine returns the current value of a clock, in milliseconds.
The value is in milliseconds.
*/

/* PUBLIC */
unsigned clock_milliseconds(Clock p)
{
  if (p == NULL)
    return 0;
  else if (Clock_sample_rate == 1) {
    unsigned i = p->accum_msec;
    if (p->level == 0)
      return i;
    else {
      unsigned msec;
      CPU_TIME_MSEC(msec);
      return i + (msec - p->curr_msec);
    }
  }
  else
    return (unsigned) (clock_value_usec(p) / 1000.0);
}  /* clock_milliseconds */

/*************
 *
 *   clock_seconds()
 *
 *************/

/* DOCUMENTATION
This routine returns the current value of a clock, in seconds.
The clock need not be stopped.
*/

/* PUBLIC */
double clock_seconds(Clock p)
{
  if (p == NULL)
    return 0.0;
  else if (Clock_sample_rate == 1) {
    unsigned i = p->accum_msec;
    if (p->level == 0)
      return i / 1000.0;
    else {
      unsigned msec;
      CPU_TIME_MSEC(msec);
      return (i + (msec - p->curr_msec)) / 1000.0;
    }
  }
  else
    return clock_value_usec(p) / 1000000.0;
}  /* clock_seconds */

/*************
 *
 *   clock_running()
 *
 *************/

/* DOCUMENTATION
This routine tells you whether or not a clock is running.
*/

/* PUBLIC */
BOOL clock_running(Clock p)
{
  return p->level > 0;
}  /* clock_running */

/*************
 *
 *   clock_reset()
 *
 *************/

/* DOCUMENTATION
This routine resets a clock, as if it had just been initialized.
(You should not need this routine under normal circumstances.)
*/
  
/* PUBLIC */
void clock_reset(Clock p)
{
  if (p != NULL) {
    p->level = 0;
    p->accum_msec = 0;
    p->curr_msec = 0;
    p->exact_usec = 0;
    p->sample_usec = 0;
    p->exact_intervals = 0;
    p->sample_intervals = 0;
    p->post_intervals = 0;
    p->curr_usec = 0;
    p->sample_state = clock_name_seed(p->name);
    p->curr_sampled = FALSE;
    p->curr_exact = FALSE;
  }
}  /* clock_reset */

/*************
 *
 *   fprint_clock()
 *
 *************/

/* DOCUMENTATION
This routine
*/

/* PUBLIC */
void fprint_clock(FILE *fp, Clock p)
{
  if (p != NULL) {
    fprintf(fp, "clock %-15s: %6.2f seconds.",
            p->name, clock_seconds(p));
    if (Clock_sample_rate > 1)
      fprintf(fp, " [estimated: intervals=%llu, timed=%llu]",
              p->exact_intervals + p->post_intervals,
              p->exact_intervals + p->sample_intervals);
    fprintf(fp, "\n");
  }
}  /* fprint_clock */

/*************
 *
 *   sampled detailed clocks
 *
 *************/

/* PUBLIC */
void set_clock_sample_rate(unsigned rate)
{
  Clock_sample_rate = rate == 0 ? 1 : rate;
  Clock_sample_threshold = UINT_MAX / Clock_sample_rate;
}  /* set_clock_sample_rate */

/* PUBLIC */
unsigned clock_sample_rate(void)
{
  return Clock_sample_rate;
}  /* clock_sample_rate */

/* PUBLIC */
void fprint_clock_sampling(FILE *fp)
{
  if (Clocks_enabled && Clock_sample_rate > 1)
    fprintf(fp,
            "Clock_sampling: rate=1/%u, warmup=%u, intervals=%llu, "
            "samples=%llu, cpu_time_reads=%llu, values=estimated.\n",
            Clock_sample_rate, CLOCK_SAMPLE_WARMUP, Sample_clock_starts,
            Clock_samples, Clock_time_reads);
}  /* fprint_clock_sampling */

/*************
 *
 *   get_date()
 *
 *************/

/* DOCUMENTATION
This routine returns a string representation of the current date and time.
*/

/* PUBLIC */
char * get_date(void)
{
  time_t i = time(NULL);
  return asctime(localtime(&i));
}  /* get_date */

/*************
 *
 *   user_time()
 *
 *************/

/* DOCUMENTATION
This routine returns the user CPU time, in milliseconds, that the
current process has used so far.
*/

/* PUBLIC */
unsigned user_time()
{
#ifdef PRIMITIVE_ENVIRONMENT
  return (unsigned)(clock() / (CLOCKS_PER_SEC / 1000));
#else
  struct rusage r;
  unsigned sec, usec;

  getrusage(RUSAGE_SELF, &r);
  sec = r.ru_utime.tv_sec;
  usec = r.ru_utime.tv_usec;

  return((sec * 1000) + (usec / 1000));
#endif
}  /* user_time */

/*************
 *
 *   user_seconds()
 *
 *************/

/* DOCUMENTATION
This routine returns the user CPU time, in seconds, that the
current process has used so far.
*/

/* PUBLIC */
double user_seconds()
{
#ifdef PRIMITIVE_ENVIRONMENT
  return User_seconds_offset + (double)clock() / CLOCKS_PER_SEC;
#else
  struct rusage r;
  unsigned sec, usec;

  getrusage(RUSAGE_SELF, &r);
  sec = r.ru_utime.tv_sec;
  usec = r.ru_utime.tv_usec;

  return User_seconds_offset + sec + (usec / 1000000.0);
#endif
}  /* user_seconds */

/*************
 *
 *   set_user_seconds_offset()
 *
 *************/

/* DOCUMENTATION
Set an offset that is added to user_seconds().  Used on checkpoint
resume so the reported CPU time includes time from the original run.
*/

/* PUBLIC */
void set_user_seconds_offset(double offset)
{
  User_seconds_offset = offset;
}  /* set_user_seconds_offset */

/*************
 *
 *   system_time()
 *
 *************/

/* DOCUMENTATION
This routine returns the system CPU time, in milliseconds, that has
been spent on the current process.
(System time measures low-level operations such
as system calls, paging, and I/O that the operating systems does
on behalf of the process.)
*/

/* PUBLIC */
unsigned system_time()
{
#ifdef PRIMITIVE_ENVIRONMENT
  return 0;  /* no system time in WASM */
#else
  struct rusage r;
  unsigned sec, usec;

  getrusage(RUSAGE_SELF, &r);
  sec = r.ru_stime.tv_sec;
  usec = r.ru_stime.tv_usec;

  return((sec * 1000) + (usec / 1000));
#endif
}  /* system_time */

/*************
 *
 *   system_seconds()
 *
 *************/

/* DOCUMENTATION
This routine returns the system CPU time, in seconds, that has
been spent on the current process.
(System time measures low-level operations such
as system calls, paging, and I/O that the operating systems does
on behalf of the process.)
*/

/* PUBLIC */
double system_seconds()
{
#ifdef PRIMITIVE_ENVIRONMENT
  return 0.0;  /* no system time in WASM */
#else
  struct rusage r;
  unsigned sec, usec;

  getrusage(RUSAGE_SELF, &r);
  sec = r.ru_stime.tv_sec;
  usec = r.ru_stime.tv_usec;

  return(sec + (usec / 1000000.0));
#endif
}  /* system_seconds */

/*************
 *
 *   absolute_wallclock()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
unsigned absolute_wallclock(void)
{
#ifdef PRIMITIVE_ENVIRONMENT
  return (unsigned)(clock() / CLOCKS_PER_SEC);
#else
  time_t t = time((time_t *) NULL);
  return (unsigned) t;
#endif
}  /* absolute_wallclock */

/*************
 *
 *   init_wallclock()
 *
 *************/

/* DOCUMENTATION
This routine initializes the wall-clock timer.
*/

/* PUBLIC */
void init_wallclock()
{
  Wall_start = absolute_wallclock();
}  /* init_wallclock */

/*************
 *
 *   wallclock()
 *
 *************/

/* DOCUMENTATION
This routine returns the number of wall-clock seconds since
init_wallclock() was called.  The result is unsigned.
*/

/* PUBLIC */
unsigned wallclock()
{
  return absolute_wallclock() - Wall_start;
}  /* wallclock */

/*************
 *
 *   disable_clocks()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void disable_clocks(void)
{
  Clocks_enabled = FALSE;
}  /* disable_clocks */

/*************
 *
 *   enable_clocks()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
void enable_clocks(void)
{
  Clocks_enabled = TRUE;
}  /* enable_clocks */

/*************
 *
 *   clocks_enabled()
 *
 *************/

/* DOCUMENTATION
*/

/* PUBLIC */
BOOL clocks_enabled(void)
{
  return Clocks_enabled;
}  /* clocks_enabled */
