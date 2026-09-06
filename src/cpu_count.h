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

#ifndef CPULIMIT_CPU_COUNT_H
#define CPULIMIT_CPU_COUNT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the number of online/available CPU cores
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
int get_ncpu(void);

#if defined(__linux__)
/**
 * @brief Parse a Linux sysfs CPU range string into a CPU count
 * @param str CPU range specification (e.g. "0-3", "0,2,4", "0-1,4-7")
 * @return Number of CPUs described by the range, or -1 on parse error
 *
 * Public only so the unit tests can verify its boundary behaviour.
 * Behaviour and contract are documented at the definition in cpu_count.c.
 */
int parse_cpu_range(const char *str);
#endif

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_CPU_COUNT_H */
