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
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(__UCLIBC__)
#include <ctype.h>
#endif
#endif

#ifdef CPULIMIT_IMPL_GETLOADAVG
#include <stdlib.h>
#include <sys/sysinfo.h>
#endif

#if defined(__linux__) && !defined(_SC_NPROCESSORS_ONLN)
#include <sys/sysinfo.h>
#endif

/**
 * @brief Convert nanoseconds to timespec structure
 * @param nsec Number of nanoseconds (can be >= 1 billion)
 * @param result_ts Pointer to timespec structure to populate
 *
 * Splits the nanosecond value into seconds and nanoseconds components.
 * The seconds component is the integer division by 1 billion, and the
 * nanoseconds component is the remainder. Adjusts tv_sec and tv_nsec
 * together to keep tv_nsec in [0, 999999999], guarding against
 * floating-point rounding errors.
 */
void nsec2timespec(double nsec, struct timespec *result_ts) {
    result_ts->tv_sec = (time_t)(nsec / 1e9);
    result_ts->tv_nsec = (long)(nsec - (double)result_ts->tv_sec * 1e9);
    /* Correct tv_sec when floating-point rounding shifts tv_nsec out of
     * range */
    if (result_ts->tv_nsec < 0L) {
        result_ts->tv_sec--;
        result_ts->tv_nsec += 1000000000L;
    } else if (result_ts->tv_nsec >= 1000000000L) {
        result_ts->tv_sec++;
        result_ts->tv_nsec -= 1000000000L;
    }
}

/**
 * @brief Get a high-resolution timestamp, preferring a monotonic clock
 * @param result_ts Pointer to timespec structure to receive current time
 * @return 0 on success, -1 on failure
 *
 * Uses CLOCK_MONOTONIC if available (unaffected by system time changes) to
 * return a monotonic timestamp, otherwise falls back to CLOCK_REALTIME, or
 * gettimeofday() as a final fallback. Provides at least microsecond
 * resolution on all supported platforms.
 */
int get_current_time(struct timespec *result_ts) {
#if defined(CLOCK_MONOTONIC)
    /* Prefer monotonic clock: immune to system time adjustments */
    return clock_gettime(CLOCK_MONOTONIC, result_ts);
#elif defined(CLOCK_REALTIME)
    /* Fall back to real-time clock if monotonic unavailable */
    return clock_gettime(CLOCK_REALTIME, result_ts);
#else
    /* Final fallback: use gettimeofday and convert to timespec */
    struct timeval time_val;
    if (gettimeofday(&time_val, NULL)) {
        return -1;
    }
    result_ts->tv_sec = time_val.tv_sec;
    result_ts->tv_nsec = time_val.tv_usec * 1000L;
    return 0;
#endif
}

/**
 * @brief Sleep for a specified duration
 * @param duration Pointer to timespec specifying sleep duration
 * @return 0 on success, -1 on error (errno set by underlying call)
 *
 * Uses clock_nanosleep() with CLOCK_MONOTONIC if available to provide sleep
 * durations that are unaffected by system time changes, otherwise falls back
 * to nanosleep(). The underlying call may return early (for example, with
 * errno set to EINTR if interrupted by a signal); this function does not
 * automatically resume sleeping in that case.
 */
int sleep_timespec(const struct timespec *duration) {
#if (defined(__linux__) || defined(__FreeBSD__)) &&                            \
    defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L &&                  \
    defined(CLOCK_MONOTONIC)
    /*
     * Use monotonic clock sleep if available.
     * clock_nanosleep returns 0 on success or a positive error number
     * on failure. Convert to -1/errno convention for consistency with
     * nanosleep and the documented return value contract.
     */
    int ret = clock_nanosleep(CLOCK_MONOTONIC, 0, duration, NULL);
    if (ret != 0) {
        errno = ret;
        return -1;
    }
    return 0;
#else
    /* Fall back to standard nanosleep */
    return nanosleep(duration, NULL);
#endif
}

/**
 * @brief Calculate elapsed time between two timestamps in milliseconds
 * @param later Pointer to the more recent timestamp
 * @param earlier Pointer to the older timestamp
 * @return Time difference in milliseconds (later - earlier)
 *
 * Computes the difference accounting for both seconds and nanoseconds fields.
 * Returns a positive value when later > earlier. The nanosecond component is
 * divided by 1e6, giving sub-microsecond precision in the returned value.
 */
double timediff_in_ms(const struct timespec *later,
                      const struct timespec *earlier) {
    return difftime(later->tv_sec, earlier->tv_sec) * 1e3 +
           ((double)later->tv_nsec - (double)earlier->tv_nsec) / 1e6;
}

/**
 * @brief Extract filename from a full path
 * @param path Full file path (may contain directory separators), or NULL
 * @return Pointer to the filename portion within the path string
 *
 * Returns a pointer to the substring after the last '/' character, or the
 * original string if no '/' is found. Does not allocate memory; the returned
 * pointer references part of the input string.
 *
 * If path is NULL, returns an empty string.
 */
const char *file_basename(const char *path) {
    const char *last_slash;
    if (path == NULL) {
        return "";
    }
    last_slash = strrchr(path, '/');
    return last_slash != NULL ? last_slash + 1 : path;
}

/**
 * @brief Attempt to increase the scheduling priority of the current process
 *
 * Tries to set the process nice value to -20 (highest priority) to minimize
 * scheduling latency when controlling target processes. Iterates through
 * priority values from -20 upward until one succeeds, skipping levels that
 * are denied by permissions (RLIMIT_NICE may allow a value less negative
 * than PRIO_MIN even without root). Silently continues if no priority
 * improvement is possible; cpulimit can function at normal priority, just
 * with potentially higher latency.
 */
void increase_priority(void) {
    int old_priority, priority;
    errno = 0;
    old_priority = getpriority(PRIO_PROCESS, 0);
    if (old_priority == -1 && errno != 0) {
        /* Error getting current priority, assume default priority */
        old_priority = 0;
    }
    /* Try to set highest priority, working upward if denied */
    for (priority = PRIO_MIN; priority < old_priority; priority++) {
        errno = 0;
        if (setpriority(PRIO_PROCESS, 0, priority) == 0) {
            break; /* Successfully set priority */
        }
        if (errno == EPERM) {
            /*
             * Permission denied at this level. Continue to the next
             * (less aggressive) priority: RLIMIT_NICE may allow a
             * value less negative than PRIO_MIN even without root.
             */
            continue;
        }
        /* Any other error is unexpected; stop trying */
        break;
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
 * Returns -1 if the string is NULL or zero-length, contains invalid syntax,
 * negative numbers, reversed ranges (end < start), or if the CPU count
 * would overflow int. Note: a whitespace-only string (e.g., " ") is also
 * rejected due to invalid syntax (strtol finds no number).
 */
static int parse_cpu_range(const char *str) {
    const char *pos = str;
    char *endptr;
    int cpu_count = 0;

    if (str == NULL || str[0] == '\0') {
        return -1;
    }

    while (*pos != '\0') {
        long start;
        /* Parse first number (strtol automatically skips leading whitespace) */
        errno = 0;
        start = strtol(pos, &endptr, 10);
        if (endptr == pos || errno != 0 || start < 0) {
            return -1; /* Parse error or invalid value */
        }
        pos = endptr;

        /* Skip trailing whitespace after number */
        while (isspace((unsigned char)*pos)) {
            pos++;
        }

        if (*pos == '-') {
            /* Range format: start-end */
            long end, range_len;

            pos++; /* Skip the dash */

            /* Parse end of range */
            errno = 0;
            end = strtol(pos, &endptr, 10);
            if (endptr == pos || errno != 0 || start > end || end < 0) {
                return -1; /* Parse error or invalid range */
            }
            /* Compute range length safely (start <= end and both >= 0 here) */
            range_len = end - start;
            /*
             * Check for integer overflow in cpu_count accumulation:
             * ensure cpu_count + (range_len + 1) <= INT_MAX without
             * forming an overflowing signed expression.
             */
            if (range_len > (long)INT_MAX - (long)cpu_count - 1L) {
                return -1; /* Would overflow int */
            }
            cpu_count += (int)(range_len + 1L);

            pos = endptr;

            /* Skip trailing whitespace */
            while (isspace((unsigned char)*pos)) {
                pos++;
            }
        } else {
            /* Single CPU number */
            if (cpu_count == INT_MAX) {
                return -1; /* Would overflow int */
            }
            cpu_count++;
        }

        /* Expect comma or end of string */
        if (*pos == ',') {
            pos++; /* Move past comma to parse next segment */
        } else if (*pos != '\0') {
            return -1; /* Unexpected character */
        }
    }

    return cpu_count;
}

/**
 * @brief Get online CPU count by reading sysfs
 * @return Number of online CPUs on success, or -1 on error (failed to
 *         open/read /sys/devices/system/cpu/online or invalid format)
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
        size_t ncpu_size = sizeof(ncpu);
        int sysctl_mib[2];

        sysctl_mib[0] = CTL_HW;
#if defined(HW_AVAILCPU)
        /* Try HW_AVAILCPU first (available CPUs) */
        sysctl_mib[1] = HW_AVAILCPU;
#else
        /* Fall back to HW_NCPU if HW_AVAILCPU unavailable */
        sysctl_mib[1] = HW_NCPU;
#endif
        if (sysctl(sysctl_mib, 2, &ncpu, &ncpu_size, NULL, 0) != 0 ||
            ncpu < 1) {
            /* Fallback: try HW_NCPU directly */
            sysctl_mib[1] = HW_NCPU;
            if (sysctl(sysctl_mib, 2, &ncpu, &ncpu_size, NULL, 0) != 0 ||
                ncpu < 1) {
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

#ifdef CPULIMIT_IMPL_GETLOADAVG
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
int cpulimit_getloadavg(double *loadavg, int nelem) {
    struct sysinfo sys_info;
    int load_idx;

    if (nelem < 0) {
        return -1;
    }
    if (nelem == 0) {
        return 0;
    }

    if (sysinfo(&sys_info) != 0) {
        return -1;
    }

    /* Retrieve at most 3 load averages */
    nelem = (nelem > 3) ? 3 : nelem;

    /* Convert fixed-point to floating-point using SI_LOAD_SHIFT */
    for (load_idx = 0; load_idx < nelem; load_idx++) {
        loadavg[load_idx] =
            (double)sys_info.loads[load_idx] / (1 << SI_LOAD_SHIFT);
    }

    return nelem;
}
#endif

#if defined(__linux__)
/**
 * @brief Read the first line from a text file
 * @param file_name Path to the file to read
 * @return Newly allocated string containing the first line with trailing
 *         newlines stripped, or NULL if file_name is NULL, the file cannot be
 *         opened, or reading fails (including when the file is empty and
 *         contains no bytes at all)
 *
 * Opens the specified file, reads the first line using getline(), strips
 * trailing newlines, and returns the result. The caller must free() the
 * returned string. Used for reading single-line text files such as procfs
 * stat files and sysfs entries.
 *
 * @note A file containing only a newline character returns an empty string
 *       (non-NULL), not NULL.
 */
char *read_line_from_file(const char *file_name) {
    FILE *input_file;
    char *line = NULL;
    size_t line_size = 0;
    if (file_name == NULL || (input_file = fopen(file_name, "r")) == NULL) {
        return NULL;
    }
    if (getline(&line, &line_size, input_file) < 0) {
        free(line);
        fclose(input_file);
        return NULL;
    }
    fclose(input_file);
    /* Strip trailing newline characters */
    line[strcspn(line, "\r\n")] = '\0';
    return line;
}
#endif /* __linux__ */

/**
 * @brief Safely convert long to pid_t with overflow detection
 * @param long_pid Long value to convert to pid_t
 * @return The pid_t value on success, or -1 if long_pid < 0 or overflow occurs
 *
 * Validates that the long value can be safely converted to pid_t without
 * overflow. Returns -1 if the input is negative or if the conversion would
 * result in data loss due to pid_t having a smaller range than long on the
 * platform. This prevents incorrect PID values on systems where pid_t is
 * smaller than long (e.g., 32-bit pid_t with 64-bit long).
 *
 * @note The conversion uses implementation-defined behavior when the value
 *       cannot be represented in pid_t (C89 section 3.2.1.2). However, the
 *       round-trip check correctly detects overflow on all common platforms
 *       (Linux, macOS, FreeBSD) where pid_t is a signed integer type. This
 *       approach is preferred over no overflow checking, as there is no
 *       portable way to check pid_t limits at compile time in C89/POSIX.1-2001.
 */
pid_t long2pid_t(long long_pid) {
    pid_t result;
    /* Reject negative values */
    if (long_pid < 0) {
        return (pid_t)(-1);
    }
    /* Cast to pid_t and verify no overflow occurred */
    result = (pid_t)long_pid;
    if ((long)result != long_pid) {
        return (pid_t)(-1);
    }
    return result;
}
