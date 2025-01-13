#ifndef __UTIL_H
#define __UTIL_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <string.h>
#include <unistd.h>

/* Useful macros */

#if defined(__GNUC__) && !defined(__UNIQUE_ID)
/**
 *  Helper macros to concatenate tokens
 */
#define ___PASTE(a, b) a##b
#define __PASTE(a, b) ___PASTE(a, b)

/**
 *  Generates a unique ID based on a prefix
 */
#define __UNIQUE_ID(prefix) \
    __PASTE(__PASTE(__UNIQUE_ID_, prefix), __COUNTER__)
#endif

#ifndef MIN
#ifdef __GNUC__
/**
 *  Helper for finding the minimum of two values, utilizing type safety
 */
#define __min(t1, t2, min1, min2, x, y) \
    (__extension__({                    \
        t1 min1 = (x);                  \
        t2 min2 = (y);                  \
        (void)(&min1 == &min2);         \
        min1 < min2 ? min1 : min2;      \
    }))
/**
 * Macro to find the minimum of two values
 */
#define MIN(x, y)                                 \
    __min(__typeof__(x), __typeof__(y),           \
          __UNIQUE_ID(min1_), __UNIQUE_ID(min2_), \
          x, y)
#else
/**
 * Simple macro to find the minimum of two values
 */
#define MIN(a, b) \
    (((a) < (b)) ? (a) : (b))
#endif
#endif

#ifndef MAX
#ifdef __GNUC__
/**
 * Helper for finding the maximum of two values, utilizing type safety
 */
#define __max(t1, t2, max1, max2, x, y) \
    (__extension__({                    \
        t1 max1 = (x);                  \
        t2 max2 = (y);                  \
        (void)(&max1 == &max2);         \
        max1 > max2 ? max1 : max2;      \
    }))
/**
 * Macro to find the maximum of two values
 */
#define MAX(x, y)                                 \
    __max(__typeof__(x), __typeof__(y),           \
          __UNIQUE_ID(max1_), __UNIQUE_ID(max2_), \
          x, y)
#else
/**
 * Simple macro to find the maximum of two values
 */
#define MAX(a, b) \
    (((a) > (b)) ? (a) : (b))
#endif
#endif

#ifndef basename
#ifdef __GNUC__
/**
 * Helper macro to get the basename of a path
 */
#define __basename(full_path, last_slash, path)              \
    (__extension__({                                         \
        const char *full_path, *last_slash;                  \
        full_path = (path);                                  \
        last_slash = strrchr(full_path, '/');                \
        (last_slash != NULL) ? (last_slash + 1) : full_path; \
    }))
#define basename(path) \
    __basename(__UNIQUE_ID(full_path_), __UNIQUE_ID(last_slash_), path)
#else
/**
 * Get the basename of a path
 */
const char *__basename(const char *path);
#define basename(path) __basename(path)
#define __IMPL_BASENAME
#endif
#endif

/**
 * Converts nanoseconds to a timespec structure
 */
#ifndef nsec2timespec
#define nsec2timespec(nsec, t)                                     \
    do                                                             \
    {                                                              \
        (t)->tv_sec = (time_t)((nsec) / 1e9);                      \
        (t)->tv_nsec = (long)((nsec) - (double)(t)->tv_sec * 1e9); \
    } while (0)
#endif

/**
 * Sleep for a specified timespec duration
 */
#ifndef sleep_timespec
#if defined(__linux__) && defined(_POSIX_C_SOURCE) && \
    _POSIX_C_SOURCE >= 200112L && defined(CLOCK_TAI)
#define sleep_timespec(t) clock_nanosleep(CLOCK_TAI, 0, (t), NULL)
#else
#define sleep_timespec(t) nanosleep((t), NULL)
#endif
#endif

/**
 * Retrieves the current time into a timespec structure
 */
#ifndef get_time
#if defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0
#if defined(CLOCK_TAI)
#define get_time(ts) clock_gettime(CLOCK_TAI, (ts))
#elif defined(CLOCK_MONOTONIC)
#define get_time(ts) clock_gettime(CLOCK_MONOTONIC, (ts))
#endif
#endif
#endif

/**
 * Fallback function for getting time if not defined
 */
#ifndef get_time
int __get_time(struct timespec *ts);
#define get_time(ts) __get_time(ts)
#define __IMPL_GET_TIME
#endif

/**
 * Returns the difference between two timespecs in milliseconds
 */
#ifndef timediff_in_ms
#define timediff_in_ms(t1, t2) \
    ((double)((t1)->tv_sec - (t2)->tv_sec) * 1e3 + (double)((t1)->tv_nsec - (t2)->tv_nsec) / 1e6)
#endif

/**
 * Increases the priority of the current process
 */
void increase_priority(void);

/**
 * Retrieves the number of available CPUs
 */
int get_ncpu(void);

/**
 * Retrieves the maximum process ID
 */
pid_t get_pid_max(void);

#if defined(__linux__) && defined(__UCLIBC__)
/**
 * Retrieves up to nelem load averages for system processes over the
 * last 1, 5, and 15 minutes.
 *
 * @param loadavg Pointer to an array for storing the load averages.
 *                It must have enough space for nelem samples.
 * @param nelem   Number of samples to retrieve (1 to 3).
 *
 * @return The number of samples retrieved, or -1 if the load
 *         average could not be obtained.
 */
int getloadavg(double *loadavg, int nelem);
#endif

#endif
