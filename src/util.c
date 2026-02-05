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
 * @brief Convert nanoseconds to a timespec structure
 * @param nsec Number of nanoseconds to convert
 * @param t Pointer to a timespec structure where the converted time
 *          will be stored
 */
void nsec2timespec(double nsec, struct timespec *t) {
    t->tv_sec = (time_t)(nsec / 1e9);
    t->tv_nsec = (long)(nsec - (double)t->tv_sec * 1e9);
}

/**
 * @brief Get the current time
 * @param ts Pointer to timespec structure to store the current time
 * @return 0 on success, -1 on failure
 */
int get_current_time(struct timespec *ts) {
#if defined(CLOCK_MONOTONIC)
    return clock_gettime(CLOCK_MONOTONIC, ts);
#elif defined(CLOCK_REALTIME)
    return clock_gettime(CLOCK_REALTIME, ts);
#else
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
 * @brief Sleep for a specified timespec duration
 * @param t Pointer to a timespec structure that specifies the duration
 *          to sleep
 * @return 0 for success, or -1 for failure
 */
int sleep_timespec(const struct timespec *t) {
#if (defined(__linux__) || defined(__FreeBSD__)) &&                            \
    defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L &&                  \
    defined(CLOCK_MONOTONIC)
    return clock_nanosleep(CLOCK_MONOTONIC, 0, t, NULL);
#else
    return nanosleep(t, NULL);
#endif
}

/**
 * @brief Compute the difference between two timespec structures in milliseconds
 * @param later Pointer to the timespec for the later time
 * @param earlier Pointer to the timespec for the earlier time
 * @return The difference in milliseconds (later - earlier)
 */
double timediff_in_ms(const struct timespec *later,
                      const struct timespec *earlier) {
    return difftime(later->tv_sec, earlier->tv_sec) * 1e3 +
           ((double)later->tv_nsec - (double)earlier->tv_nsec) / 1e6;
}

/**
 * @brief Get the basename of a file from the full path
 * @param path Pointer to a string containing the full path of the file
 * @return A pointer to the basename of the file
 * @note Returns a pointer to the substring after the last '/', or the
 *       original string if no '/' is found
 */
const char *file_basename(const char *path) {
    const char *p = strrchr(path, '/');
    return p != NULL ? p + 1 : path;
}

/**
 * @brief Increase the priority of the current process
 * @note Attempts to set the process priority to the maximum
 */
void increase_priority(void) {
    static const int MAX_PRIORITY = -20;
    int old_priority, priority;
    old_priority = getpriority(PRIO_PROCESS, 0);
    for (priority = MAX_PRIORITY; priority < old_priority; priority++) {
        if (setpriority(PRIO_PROCESS, 0, priority) == 0) {
            break;
        }
    }
}

#if defined(__linux__) && defined(__UCLIBC__)
/**
 * @brief Parse a CPU range string and return the number of CPUs specified.
 * @param str: String containing CPU range specification
 * @return >0: Number of CPUs, -1: Invalid format, -2: Memory allocation error
 */
static int parse_cpu_range(const char *str) {
    char *str_copy, *token, *saveptr, *dash, *endptr;
    const char *range_start, *range_end;
    int cpu_count = 0, result = 0;
    long start, end;
    size_t str_len = 0;

    if (str == NULL) {
        return -1;
    }

    str_len = strlen(str);
    if (str_len == 0) {
        return 0;
    }

    /* Make a copy of the string for tokenization */
    str_copy = (char *)malloc(str_len + 1);
    if (str_copy == NULL) {
        return -2; /* Memory allocation error */
    }

    strcpy(str_copy, str);

    /* Remove trailing newline if present */
    if (str_copy[str_len - 1] == '\n') {
        str_copy[str_len - 1] = '\0';
    }

    /* First token (comma separated) */
    token = strtok_r(str_copy, ",", &saveptr);

    while (token != NULL) {
        /* Skip leading whitespace */
        while (isspace(*token)) {
            token++;
        }

        /* Remove trailing whitespace */
        if (*token != '\0') {
            char *end_ptr = token + strlen(token) - 1;
            while (end_ptr > token && isspace(*end_ptr)) {
                *end_ptr = '\0';
                end_ptr--;
            }
        }

        /* Check if token is empty after trimming */
        if (token[0] != '\0') {
            /* Check for range format (contains dash) */
            dash = strchr(token, '-');
            if (dash != NULL) {
                /* Range format: "start-end" */
                *dash = '\0'; /* Split at dash position */

                range_start = token;
                range_end = dash + 1;

                /* Skip whitespace in start string */
                while (isspace(*range_start)) {
                    range_start++;
                }

                /* Skip whitespace in end string */
                while (isspace(*range_end)) {
                    range_end++;
                }

                /* Convert start to integer */
                errno = 0;
                start = strtol(range_start, &endptr, 10);
                if (endptr == range_start || *endptr != '\0' || errno != 0 ||
                    start < 0) {
                    result = -1; /* Invalid format */
                    break;
                }

                /* Convert end to integer */
                errno = 0;
                end = strtol(range_end, &endptr, 10);
                if (endptr == range_end || *endptr != '\0' || errno != 0 ||
                    end < 0) {
                    result = -1; /* Invalid format */
                    break;
                }

                /* Calculate number of CPUs in this range */
                if (start <= end) {
                    cpu_count += (int)(end - start + 1);
                } else {
                    result = -1; /* Invalid range: start > end */
                    break;
                }
            } else {
                /* Single CPU number */
                errno = 0;
                start = strtol(token, &endptr, 10);
                if (endptr == token || *endptr != '\0' || errno != 0 ||
                    start < 0) {
                    result = -1; /* Invalid format */
                    break;
                }

                cpu_count++;
            }
        }

        /* Get next token */
        token = strtok_r(NULL, ",", &saveptr);
    }

    free(str_copy);

    /* Return result */
    if (result < 0) {
        return result; /* Error occurred during parsing */
    }

    if (cpu_count <= 0) {
        return 0; /* No CPUs found */
    }

    return cpu_count;
}

/**
 * @brief Get the number of online CPUs from the sysfs file.
 * @return >0: Number of online CPUs, 0: No online CPUs,
 *         -1: Cannot open/read file,
 *         -2: Invalid format,
 *         -3: Memory allocation error
 * @note This function reads from /sys/devices/system/cpu/online
 *       and parses the CPU range string to determine the count.
 *       The file format can be:
 *         "0"                - Only CPU 0 is online
 *         "0-3"              - CPUs 0-3 are online (continuous range)
 *         "0,2,4,6"          - Discontinuous individual CPUs
 *         "0-2,8-10,15-17"   - Multiple ranges
 *         "0-3,5,7-9"        - Mixed format
 */
static int get_online_cpu_count(void) {
    char *line;
    int cpu_count;

    line = read_line_from_file("/sys/devices/system/cpu/online");
    if (line == NULL) {
        return -1; /* Cannot open/read file */
    }
    cpu_count = parse_cpu_range(line);
    free(line);
    return cpu_count;
}
#endif

/**
 * @brief Get the number of available CPUs
 * @return The number of available CPUs, or 1 if the count could not be
 *         obtained
 * @note The result is cached after the first call
 */
int get_ncpu(void) {
    /* Static cache: -1 means uninitialized */
    static int cached_ncpu = -1;

    /* Only compute if not cached yet */
    if (cached_ncpu < 0) {
#if defined(_SC_NPROCESSORS_ONLN)
        /* POSIX systems using sysconf */
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
#if defined(__linux__) && defined(__UCLIBC__)
        /* Workaround: sysconf may return 1 with older uClibc even if more
         * CPUs are online, so we double-check by reading from sysfs */
        if (ncpu <= 1) {
            /* Fallback to reading from sysfs */
            ncpu = get_online_cpu_count();
        }
#endif
        cached_ncpu = (ncpu > 0) ? (int)ncpu : 1;

#elif defined(__APPLE__) || defined(__FreeBSD__)
        /* macOS / FreeBSD using sysctl */
        int ncpu = 0; /* CPU count */
        size_t len;   /* Length of value for sysctl */
        int mib[2];   /* Management Information Base array */

        len = sizeof(ncpu);

        /* Try to get available CPUs first */
        mib[0] = CTL_HW;
#if defined(HW_AVAILCPU)
        mib[1] = HW_AVAILCPU;
#else
        mib[1] = HW_NCPU;
#endif
        if (sysctl(mib, 2, &ncpu, &len, NULL, 0) != 0 || ncpu < 1) {
            /* Fallback to total CPUs */
            mib[1] = HW_NCPU;
            if (sysctl(mib, 2, &ncpu, &len, NULL, 0) != 0 || ncpu < 1) {
                ncpu = 1; /* Complete failure fallback */
            }
        }
        cached_ncpu = ncpu;

#elif defined(__linux__)
        /* Linux using get_nprocs */
        int ncpu;
        ncpu = get_nprocs();
#if defined(__UCLIBC__)
        /* Workaround: get_nprocs may return 1 with older uClibc even if more
         * CPUs are online, so we double-check by reading from sysfs */
        if (ncpu <= 1) {
            /* Fallback to reading from sysfs */
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
 * @brief Get up to nelem load averages for system processes
 * @param loadavg Pointer to an array for storing the load averages
 *                It must have enough space for nelem samples
 * @param nelem Number of samples to retrieve (1 to 3)
 * @return The number of samples retrieved, or -1 if the load
 *         average could not be obtained
 * @note Only available on uClibc/uClibc-ng below version 1.0.42
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

    nelem = (nelem > 3) ? 3 : nelem;

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
 * @return Pointer to the allocated line buffer on success,
 *         or NULL on failure or if no characters are read
 * @note The caller is responsible for freeing the returned buffer
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
    line[strcspn(line, "\r\n")] = '\0';
    return line;
}
#endif /* __linux__ */
