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

#include "cpu_count.h"

#include "file_io.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif
#if defined(__linux__)
#include <ctype.h>
#endif
#if defined(__linux__) && !defined(_SC_NPROCESSORS_ONLN)
#include <sys/sysinfo.h>
#endif

#if defined(__linux__)
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
int parse_cpu_range(const char *str) {
    const char *parse_pos = str;
    char *endptr;
    int cpu_count = 0;

    if (str == NULL || str[0] == '\0') {
        return -1;
    }

    while (*parse_pos != '\0') {
        long start;
        /* Parse first number (strtol automatically skips leading whitespace) */
        errno = 0;
        start = strtol(parse_pos, &endptr, 10);
        if (endptr == parse_pos || errno != 0 || start < 0) {
            return -1; /* Parse error or invalid value */
        }
        parse_pos = endptr;

        /* Skip trailing whitespace after number */
        while (isspace((unsigned char)*parse_pos)) {
            parse_pos++;
        }

        if (*parse_pos == '-') {
            /* Range format: start-end */
            long end, range_len;

            parse_pos++; /* Skip the dash */

            /* Parse end of range */
            errno = 0;
            end = strtol(parse_pos, &endptr, 10);
            if (endptr == parse_pos || errno != 0 || start > end || end < 0) {
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

            parse_pos = endptr;

            /* Skip trailing whitespace */
            while (isspace((unsigned char)*parse_pos)) {
                parse_pos++;
            }
        } else {
            /* Single CPU number */
            if (cpu_count == INT_MAX) {
                return -1; /* Would overflow int */
            }
            cpu_count++;
        }

        /* Expect comma or end of string */
        if (*parse_pos == ',') {
            parse_pos++; /* Move past comma to parse next segment */
            while (isspace((unsigned char)*parse_pos)) {
                parse_pos++;
            }
            if (*parse_pos == '\0') {
                return -1; /* Trailing comma in CPU range is invalid */
            }
        } else if (*parse_pos != '\0') {
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

    line = read_file_contents("/sys/devices/system/cpu/online");
    if (line == NULL) {
        return -1; /* Failed to read file */
    }
    cpu_count = parse_cpu_range(line);
    free(line);
    return cpu_count;
}
#endif

int get_ncpu(void) {
    /* Static cache: -1 indicates not yet initialized */
    static int cached_ncpu = -1;

    /* Return cached value if already computed */
    if (cached_ncpu < 0) {
#if defined(_SC_NPROCESSORS_ONLN)
        /* POSIX-compliant systems: use sysconf */
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
#if defined(__linux__)
        /*
         * Workaround for older libc bug: sysconf may incorrectly return 1
         * even when multiple CPUs are online. Verify by reading sysfs.
         */
        if (ncpu <= 1) {
            /* Cross-check with sysfs; use sysfs value if valid */
            ncpu = get_online_cpu_count();
        }
#endif
        cached_ncpu = (ncpu > 0 && ncpu <= INT_MAX) ? (int)ncpu : 1;

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
        /*
         * Workaround for older libc bug: get_nprocs may incorrectly return 1.
         * Verify by reading sysfs.
         */
        if (ncpu <= 1) {
            /* Cross-check with sysfs; use sysfs value if valid */
            ncpu = get_online_cpu_count();
        }
        cached_ncpu = (ncpu > 0) ? ncpu : 1;

#else
#error "Unsupported platform"
#endif
    }

    return cached_ncpu;
}
