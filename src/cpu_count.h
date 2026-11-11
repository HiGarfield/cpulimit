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

#ifndef CPULIMIT_CPU_COUNT_H
#define CPULIMIT_CPU_COUNT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the number of online CPU cores
 *
 * @return Number of CPUs online on the host (>= 1)
 *
 * Uses sysconf(_SC_NPROCESSORS_ONLN) wherever that POSIX option is
 * available. The sysctl() branch on macOS/FreeBSD and the get_nprocs()
 * branch on Linux are compiled only when the macro is absent, so they are
 * fallbacks rather than those platforms' normal path. On Linux, the
 * sysconf() answer is cross-checked against
 * /sys/devices/system/cpu/online when it reports one core or fewer, to
 * work around older libc bugs. Returns 1 when the count cannot be
 * determined, and caches the result after the first call.
 *
 * @note Result is cached and never recalculated even if CPU hotplugging occurs
 */
int get_ncpu(void);

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
int parse_cpu_range(const char *str);
#endif

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_CPU_COUNT_H */
