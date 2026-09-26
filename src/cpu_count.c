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
 * @brief Parse a Linux sysfs CPU range string into a CPU count
 *
 * @param str CPU range specification (e.g. "0-3", "0,2,4", "0-1,4-7")
 * @return Number of CPUs described by the range, or -1 on parse error
 *
 * Accepts single CPUs ("0"), inclusive ranges ("0-3" counts as four) and
 * comma-separated combinations ("0-3,8-11,15"); whitespace around the
 * numbers is tolerated. Returns -1 for a NULL or empty string, invalid
 * syntax, a negative number, a reversed range (end < start), or a total that
 * would overflow int; a whitespace-only string is rejected as invalid syntax.
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
            return -1;
        }
        parse_pos = endptr;

        while (isspace((unsigned char)*parse_pos)) {
            parse_pos++;
        }

        if (*parse_pos == '-') {
            long end, range_len;

            parse_pos++;

            errno = 0;
            end = strtol(parse_pos, &endptr, 10);
            if (endptr == parse_pos || errno != 0 || start > end || end < 0) {
                return -1;
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

            while (isspace((unsigned char)*parse_pos)) {
                parse_pos++;
            }
        } else {
            if (cpu_count == INT_MAX) {
                return -1; /* Would overflow int */
            }
            cpu_count++;
        }

        if (*parse_pos == ',') {
            parse_pos++;
            while (isspace((unsigned char)*parse_pos)) {
                parse_pos++;
            }
            if (*parse_pos == '\0') {
                return -1;
            }
        } else if (*parse_pos != '\0') {
            return -1;
        }
    }

    return cpu_count;
}

/**
 * @brief Get online CPU count by reading sysfs
 *
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

/**
 * @brief Get the number of online/available CPU cores
 *
 * @return Number of CPUs available to the process (>= 1)
 *
 * Queries the system for the number of online CPUs using platform-specific
 * methods (sysconf on Linux/POSIX, sysctl on macOS/FreeBSD). The result is
 * cached after the first call for efficiency. On Linux, performs additional
 * validation by reading /sys/devices/system/cpu/online to work around older
 * library bugs. Returns 1 if count cannot be determined.
 *
 * @note Result is cached and never recalculated even if CPU hotplugging occurs
 */
int get_ncpu(void) {
    /* Static cache: -1 indicates not yet initialized */
    static int cached_ncpu = -1;

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
            /*
             * Cross-check with sysfs, but only let a usable answer replace
             * what sysconf() reported: get_online_cpu_count() returns -1
             * when /sys cannot be read (a restricted container, or sysfs
             * not mounted), and taking that for the count would replace a
             * perfectly good value with 1.
             */
            long sysfs_ncpu = get_online_cpu_count();
            if (sysfs_ncpu > 0) {
                ncpu = sysfs_ncpu;
            }
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
            /* Same guard as above: only a usable sysfs answer counts. */
            int sysfs_ncpu = get_online_cpu_count();
            if (sysfs_ncpu > 0) {
                ncpu = sysfs_ncpu;
            }
        }
        cached_ncpu = (ncpu > 0) ? ncpu : 1;

#else
#error "Unsupported platform"
#endif
    }

    return cached_ncpu;
}
