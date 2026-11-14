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
 * @brief Parse a Linux sysfs CPU range string into a CPU count.
 *
 * @param str CPU range, e.g. "0-3", "0,2,4" or "0-1,4-7".
 * @return Number of CPUs described, or -1 on a parse error.
 *
 * Accepts single CPUs, inclusive ranges and comma-separated combinations,
 * with whitespace around numbers. Rejects NULL, empty, signed, reversed or
 * overflowing input.
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
        const char *number_pos = parse_pos;
        /* strtol() accepts signs, but the sysfs grammar does not. */
        while (isspace((unsigned char)*number_pos)) {
            number_pos++;
        }
        if (*number_pos == '+' || *number_pos == '-') {
            return -1;
        }
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
            const char *end_pos = parse_pos + 1;

            while (isspace((unsigned char)*end_pos)) {
                end_pos++;
            }
            if (*end_pos == '+' || *end_pos == '-') {
                return -1;
            }

            parse_pos++;

            errno = 0;
            end = strtol(parse_pos, &endptr, 10);
            if (endptr == parse_pos || errno != 0 || start > end || end < 0) {
                return -1;
            }
            range_len = end - start;
            /* Check overflow without forming an overflowing sum. */
            if (range_len > (long)INT_MAX - (long)cpu_count - 1L) {
                return -1;
            }
            cpu_count += (int)(range_len + 1L);

            parse_pos = endptr;

            while (isspace((unsigned char)*parse_pos)) {
                parse_pos++;
            }
        } else {
            if (cpu_count == INT_MAX) {
                return -1;
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
 * @brief Read the online CPU count from Linux sysfs.
 *
 * @return Number of online CPUs, or -1 when sysfs cannot be read or parsed.
 */
static int get_online_cpu_count(void) {
    char *line;
    int cpu_count;

    line = read_file_contents("/sys/devices/system/cpu/online");
    if (line == NULL) {
        return -1;
    }
    cpu_count = parse_cpu_range(line);
    free(line);
    return cpu_count;
}
#endif

/**
 * @brief Get the number of online CPU cores.
 *
 * @return Number of online CPUs, at least 1.
 *
 * Uses sysconf(_SC_NPROCESSORS_ONLN) where available; the sysctl() and
 * get_nprocs() branches are fallbacks for hosts without it. On Linux a
 * result of one core or fewer is cross-checked against sysfs to work around
 * older libc bugs. The result is cached after the first call.
 *
 * @note The cached value is never refreshed after CPU hotplugging.
 */
int get_ncpu(void) {
    static int cached_ncpu = -1;

    if (cached_ncpu < 0) {
#if defined(_SC_NPROCESSORS_ONLN)
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
#if defined(__linux__)
        /* Some older libc versions report one CPU even when more are online. */
        if (ncpu <= 1) {
            /* Only a usable sysfs answer may replace the sysconf() value. */
            long sysfs_ncpu = get_online_cpu_count();
            if (sysfs_ncpu > 0) {
                ncpu = sysfs_ncpu;
            }
        }
#endif
        cached_ncpu = (ncpu > 0 && ncpu <= INT_MAX) ? (int)ncpu : 1;

#elif defined(__APPLE__) || defined(__FreeBSD__)
        int ncpu = 0;
        size_t ncpu_size = sizeof(ncpu);
        int sysctl_mib[2];

        sysctl_mib[0] = CTL_HW;
#if defined(HW_AVAILCPU)
        sysctl_mib[1] = HW_AVAILCPU;
#else
        sysctl_mib[1] = HW_NCPU;
#endif
        if (sysctl(sysctl_mib, 2, &ncpu, &ncpu_size, NULL, 0) != 0 ||
            ncpu_size != sizeof(ncpu) || ncpu < 1) {
            sysctl_mib[1] = HW_NCPU;
            /* Reset the length and value before the HW_NCPU retry. */
            ncpu = 0;
            ncpu_size = sizeof(ncpu);
            if (sysctl(sysctl_mib, 2, &ncpu, &ncpu_size, NULL, 0) != 0 ||
                ncpu_size != sizeof(ncpu) || ncpu < 1) {
                ncpu = 1;
            }
        }
        cached_ncpu = ncpu;

#elif defined(__linux__)
        int ncpu;
        ncpu = get_nprocs();
        if (ncpu <= 1) {
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
