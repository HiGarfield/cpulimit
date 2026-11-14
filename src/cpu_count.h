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
int get_ncpu(void);

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
int parse_cpu_range(const char *str);
#endif

#ifdef __cplusplus
}
#endif

#endif
