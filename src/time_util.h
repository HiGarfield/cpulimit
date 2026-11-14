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

#ifndef CPULIMIT_TIME_UTIL_H
#define CPULIMIT_TIME_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <time.h>

/**
 * @brief Warn about a potential Y2038 risk.
 *
 * Prints a warning when time_t is narrower than 64 bits on a platform
 * without monotonic-clock or Apple time guarantees.
 *
 * @note This is a heuristic, not a guarantee of Y2038 safety.
 */
void check_y2038(void);

/**
 * @brief Convert nanoseconds to a timespec structure.
 *
 * @param nsec Number of nanoseconds, possibly above one billion.
 * @param result_ts Structure to populate.
 *
 * Keeps tv_nsec in [0, 999999999] even when floating-point rounding shifts
 * the split, by adjusting tv_sec with it.
 */
void nsec_to_timespec(double nsec, struct timespec *result_ts);

/**
 * @brief Read a high-resolution timestamp, preferring a monotonic clock.
 *
 * @param result_ts Structure receiving the timestamp.
 * @return 0 on success, -1 on failure; a NULL argument sets errno to EINVAL.
 *
 * Uses mach_absolute_time() on macOS, CLOCK_MONOTONIC where available,
 * then CLOCK_REALTIME and finally gettimeofday(). Callers use difftime()
 * for interval math, so wall-clock based fallbacks stay safe past 2038.
 */
int get_current_time(struct timespec *result_ts);

/**
 * @brief Sleep for a specified duration.
 *
 * @param duration Sleep duration.
 * @return 0 on success, -1 on error with errno set.
 *
 * Uses clock_nanosleep() with CLOCK_MONOTONIC where available and falls
 * back to nanosleep(); on macOS that fallback follows the calendar clock.
 * Neither call is restarted by SA_RESTART, so a signal ends the sleep with
 * EINTR and the remainder is deliberately not resumed.
 */
int sleep_timespec(const struct timespec *duration);

/**
 * @brief Calculate the elapsed time between two timestamps in milliseconds.
 *
 * @param later More recent timestamp.
 * @param earlier Older timestamp.
 * @return later - earlier in milliseconds.
 *
 * Uses difftime() for the seconds part to avoid overflowing time_t.
 */
double timediff_in_ms(const struct timespec *later,
                      const struct timespec *earlier);

#ifdef __cplusplus
}
#endif

#endif
