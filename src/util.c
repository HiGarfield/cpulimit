/*
 * cpulimit - a CPU usage limiter for Linux, macOS, and FreeBSD
 *
 * Copyright (C) 2005-2012  Angelo Marletta
 * <angelo dot marletta at gmail dot com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see
 * <https://www.gnu.org/licenses/>.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "util.h" /* Must be included at first!!! */

#include <errno.h>
#include <string.h>
#include <sys/resource.h>
#if !defined(CLOCK_MONOTONIC) && !defined(CLOCK_REALTIME)
#include <sys/time.h>
#endif
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <stddef.h>
#include <sys/sysctl.h>
#endif
#if defined(__linux__)
#if defined(__UCLIBC__)
#include <ctype.h>
#include <errno.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#endif

#ifdef __IMPL_GETLOADAVG
#include <stdlib.h>
#include <sys/sysinfo.h>
#endif

#if defined(__linux__) && !defined(_SC_NPROCESSORS_ONLN)
#include <sys/sysinfo.h>
#endif

/**
 * @brief Convert nanoseconds to timespec structure
 * @param nsec Number of nanoseconds (can be >= 1 billion)
 * @param t Pointer to timespec structure to populate
 *
 * Splits the nanosecond value into seconds and nanoseconds components.
 * The seconds component is the integer division by 1 billion, and the
 * nanoseconds component is the remainder.
 */
void nsec2timespec(double nsec, struct timespec *t) {
    t->tv_sec = (time_t)(nsec / 1e9);
    t->tv_nsec = (long)(nsec - (double)t->tv_sec * 1e9);
}

/**
 * @brief Get a high-resolution timestamp, preferring a monotonic clock
 * @param ts Pointer to timespec structure to receive current time
 * @return 0 on success, -1 on failure
 *
 * Uses CLOCK_MONOTONIC if available (unaffected by system time changes) to
 * return a monotonic timestamp, otherwise falls back to CLOCK_REALTIME, or
 * gettimeofday() as a final fallback. Provides at least microsecond
 * resolution on all supported platforms.
 */
int get_current_time(struct timespec *ts) {
#if defined(CLOCK_MONOTONIC)
    /* Prefer monotonic clock: immune to system time adjustments */
    return clock_gettime(CLOCK_MONOTONIC, ts);
#elif defined(CLOCK_REALTIME)
    /* Fall back to real-time clock if monotonic unavailable */
    return clock_gettime(CLOCK_REALTIME, ts);
#else
    /* Final fallback: use gettimeofday and convert to timespec */
    struct timeval tv;
    if (gettimeofday(&tv, NULL)) {
        return -1;
    }
    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = tv.tv_usec * 1000L;
    return 0;
#endif
}

/**
 * @brief Sleep for a specified duration
 * @param t Pointer to timespec specifying sleep duration
 * @return 0 on success, -1 on error (errno set by underlying call)
 *
 * Uses clock_nanosleep() with CLOCK_MONOTONIC if available to provide sleep
 * durations that are unaffected by system time changes, otherwise falls back
 * to nanosleep(). The underlying call may return early (for example, with
 * errno set to EINTR if interrupted by a signal); this function does not
 * automatically resume sleeping in that case.
 */
int sleep_timespec(const struct timespec *t) {
#if (defined(__linux__) || defined(__FreeBSD__)) &&                            \
    defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L &&                  \
    defined(CLOCK_MONOTONIC)
    /* Use monotonic clock sleep if available */
    return clock_nanosleep(CLOCK_MONOTONIC, 0, t, NULL);
#else
    /* Fall back to standard nanosleep */
    return nanosleep(t, NULL);
#endif
}

/**
 * @brief Calculate elapsed time between two timestamps in milliseconds
 * @param later Pointer to the more recent timestamp
 * @param earlier Pointer to the older timestamp
 * @return Time difference in milliseconds (later - earlier)
 *
 * Computes the difference accounting for both seconds and nanoseconds fields.
 * Returns a positive value when later > earlier. Precision is microseconds
 * due to nanosecond conversion to milliseconds.
 */
double timediff_in_ms(const struct timespec *later,
                      const struct timespec *earlier) {
    return difftime(later->tv_sec, earlier->tv_sec) * 1e3 +
           ((double)later->tv_nsec - (double)earlier->tv_nsec) / 1e6;
}

/**
 * @brief Extract filename from a full path
 * @param path Full file path (may contain directory separators)
 * @return Pointer to the filename portion within the path string
 *
 * Returns a pointer to the substring after the last '/' character, or the
 * original string if no '/' is found. Does not allocate memory; the returned
 * pointer references part of the input string.
 *
 * @note The caller must pass a non-NULL path; behavior is undefined for NULL.
 */
const char *file_basename(const char *path) {
    const char *p = strrchr(path, '/');
    return p != NULL ? p + 1 : path;
}

/**
 * @brief Attempt to increase the scheduling priority of the current process
 *
 * Tries to set the process nice value to -20 (highest priority) to minimize
 * scheduling latency when controlling target processes. Iterates through
 * priority values from -20 upward until one succeeds or permission is denied.
 * Silently continues if permission is denied; cpulimit can function at normal
 * priority, just with potentially higher latency.
 */
void increase_priority(void) {
    static const int MAX_PRIORITY = -20;
    int old_priority, priority;
    old_priority = getpriority(PRIO_PROCESS, 0);
    /* Try to set highest priority, working upward if denied */
    for (priority = MAX_PRIORITY; priority < old_priority; priority++) {
        errno = 0;
        if (setpriority(PRIO_PROCESS, 0, priority) == 0) {
            break; /* Successfully set priority */
        }
        if (errno == EPERM) {
            break; /* Permission denied; stop trying */
        }
    }
}

#if defined(__linux__) && defined(__UCLIBC__)
/**
 * @brief Parse CPU range string from sysfs format to count
 * @param str CPU range specification string (e.g., "0-3", "0,2,4", "0-1,4-7")
 * @return Number of CPUs specified in the range, or -1 on parse error
 *
 * Parses CPU range strings in the format used by Linux sysfs. Supports:
 * - Single CPUs: "0", "2"
 * - Ranges: "0-3" (inclusive, counts as 4 CPUs)
 * - Combinations: "0-3,8-11,15" (separated by commas)
 * - Spaces around numbers are tolerated
 *
 * Returns -1 if the string contains invalid syntax, negative numbers,
 * or reversed ranges (end < start).
 */
static int parse_cpu_range(const char *str) {
    const char *p = str;
    char *endptr;
    int cpu_count = 0;

    if (str == NULL) {
        return -1;
    }

    while (*p != '\0') {
        long start;
        /* Parse first number (strtol automatically skips leading whitespace) */
        errno = 0;
        start = strtol(p, &endptr, 10);
        if (endptr == p || errno != 0 || start < 0) {
            return -1; /* Parse error or invalid value */
        }
        p = endptr;

        /* Skip trailing whitespace after number */
        while (isspace(*p)) {
            p++;
        }

        if (*p == '-') {
            /* Range format: start-end */
            long end;

            p++; /* Skip the dash */

            /* Parse end of range */
            errno = 0;
            end = strtol(p, &endptr, 10);
            if (endptr == p || errno != 0 || start > end || end < 0) {
                return -1; /* Parse error or invalid range */
            }

            cpu_count += (int)(end - start + 1);
            p = endptr;

            /* Skip trailing whitespace */
            while (isspace(*p)) {
                p++;
            }
        } else {
            /* Single CPU number */
            cpu_count++;
        }

        /* Expect comma or end of string */
        if (*p == ',') {
            p++; /* Move past comma to parse next segment */
        } else if (*p != '\0') {
            return -1; /* Unexpected character */
        }
    }

    return cpu_count;
}

/**
 * @brief Get online CPU count by reading sysfs
 * @return Number of online CPUs on success, or negative on error:
 *  -1: cannot open/read /sys/devices/system/cpu/online
 *  -2: invalid format in the file
 *
 * Reads /sys/devices/system/cpu/online and parses the CPU range string.
 * This file uses the same format as parse_cpu_range() supports:
 * - "0" for single CPU
 * - "0-3" for range (4 CPUs)
 * - "0,2-4,7" for complex patterns
 *
 * Used as a workaround for older uClibc versions where sysconf() or
 * get_nprocs() may return incorrect values.
 */
static int get_online_cpu_count(void) {
    char *line;
    int cpu_count;

    line = read_line_from_file("/sys/devices/system/cpu/online");
    if (line == NULL) {
        return -1; /* Failed to read file */
    }
    cpu_count = parse_cpu_range(line);
    free(line);
    return cpu_count;
}
#endif

/**
 * @brief Get the number of online/available CPU cores
 * @return Number of CPUs available to the process (>= 1)
 *
 * Queries the system for the number of online CPUs using platform-specific
 * methods (sysconf on Linux/POSIX, sysctl on macOS/FreeBSD). The result is
 * cached after the first call for efficiency. On Linux with uClibc, performs
 * additional validation by reading /sys/devices/system/cpu/online to work
 * around older library bugs. Returns 1 if count cannot be determined.
 *
 * @note Result is cached and never recalculated even if CPU hotplugging occurs
 */
int get_ncpu(void) {
    /* Static cache: -1 indicates not yet initialized */
    static int cached_ncpu = -1;

    /* Return cached value if already computed */
    if (cached_ncpu < 0) {
#if defined(_SC_NPROCESSORS_ONLN)
        /* POSIX-compliant systems: use sysconf */
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
#if defined(__linux__) && defined(__UCLIBC__)
        /*
         * Workaround for older uClibc bug: sysconf may incorrectly return 1
         * even when multiple CPUs are online. Verify by reading sysfs.
         */
        if (ncpu <= 1) {
            /* Cross-check with sysfs; use sysfs value if valid */
            ncpu = get_online_cpu_count();
        }
#endif
        cached_ncpu = (ncpu > 0) ? (int)ncpu : 1;

#elif defined(__APPLE__) || defined(__FreeBSD__)
        /* macOS and FreeBSD: use sysctl interface */
        int ncpu = 0;
        size_t len = sizeof(ncpu);
        int mib[2];

        mib[0] = CTL_HW;
#if defined(HW_AVAILCPU)
        /* Try HW_AVAILCPU first (available CPUs) */
        mib[1] = HW_AVAILCPU;
#else
        /* Fall back to HW_NCPU if HW_AVAILCPU unavailable */
        mib[1] = HW_NCPU;
#endif
        if (sysctl(mib, 2, &ncpu, &len, NULL, 0) != 0 || ncpu < 1) {
            /* Fallback: try HW_NCPU directly */
            mib[1] = HW_NCPU;
            if (sysctl(mib, 2, &ncpu, &len, NULL, 0) != 0 || ncpu < 1) {
                ncpu = 1; /* Complete failure; assume 1 CPU */
            }
        }
        cached_ncpu = ncpu;

#elif defined(__linux__)
        /* Linux without _SC_NPROCESSORS_ONLN: use get_nprocs */
        int ncpu;
        ncpu = get_nprocs();
#if defined(__UCLIBC__)
        /*
         * Workaround for older uClibc bug: get_nprocs may incorrectly return 1.
         * Verify by reading sysfs.
         */
        if (ncpu <= 1) {
            /* Cross-check with sysfs; use sysfs value if valid */
            ncpu = get_online_cpu_count();
        }
#endif
        cached_ncpu = (ncpu > 0) ? ncpu : 1;

#else
#error "Unsupported platform"
#endif
    }

    return cached_ncpu;
}

#ifdef __IMPL_GETLOADAVG
/**
 * @brief Get system load averages (custom implementation for old uClibc)
 * @param loadavg Array to receive load average values
 * @param nelem Number of load averages to retrieve (1-3: 1min, 5min, 15min)
 * @return Number of samples retrieved (nelem), or -1 on error
 *
 * Retrieves system load averages using the sysinfo() syscall and converts
 * the fixed-point values to floating-point. This implementation is used
 * only on uClibc/uClibc-ng versions < 1.0.42 which lack getloadavg().
 */
int __getloadavg(double *loadavg, int nelem) {
    struct sysinfo si;
    int i;

    if (nelem < 0) {
        return -1;
    }
    if (nelem == 0) {
        return 0;
    }

    if (sysinfo(&si) != 0) {
        return -1;
    }

    /* Retrieve at most 3 load averages */
    nelem = (nelem > 3) ? 3 : nelem;

    /* Convert fixed-point to floating-point using SI_LOAD_SHIFT */
    for (i = 0; i < nelem; i++) {
        loadavg[i] = (double)si.loads[i] / (1 << SI_LOAD_SHIFT);
    }

    return nelem;
}
#endif

#if defined(__linux__)
/**
 * @brief Read the first line from a text file
 * @param file_name Path to the file to read
 * @return Newly allocated string containing the first line (no newline),
 *  or NULL on error or if file is empty
 *
 * Opens the specified file, reads the first line using getline(), strips
 * trailing newlines, and returns the result. The caller must free() the
 * returned string. Used primarily for reading single-line sysfs files.
 */
char *read_line_from_file(const char *file_name) {
    FILE *fp;
    char *line = NULL;
    size_t line_size = 0;
    if (file_name == NULL || (fp = fopen(file_name, "r")) == NULL) {
        return NULL;
    }
    if (getline(&line, &line_size, fp) < 0) {
        free(line);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    /* Strip trailing newline characters */
    line[strcspn(line, "\r\n")] = '\0';
    return line;
}
#endif /* __linux__ */
