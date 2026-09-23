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

#include "time_util.h"

#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#if !defined(__APPLE__) &&                                                     \
    !(defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0 &&                           \
      (defined(CLOCK_MONOTONIC) || defined(CLOCK_REALTIME)))
#include <sys/time.h>
#endif
#if defined(__APPLE__)
#include <mach/mach_time.h>
#include <sys/time.h>
#endif

void check_y2038(void) {
#if !defined(__APPLE__) &&                                                     \
    !(defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0 && defined(CLOCK_MONOTONIC))
    if (sizeof(time_t) < 8) {
        fprintf(stderr, "Y2038 risk detected.\n");
    }
#endif
}

void nsec_to_timespec(double nsec, struct timespec *result_ts) {
    result_ts->tv_sec = (time_t)(nsec / 1e9);
    result_ts->tv_nsec = (long)(nsec - (double)result_ts->tv_sec * 1e9);
    /*
     * Correct tv_sec when floating-point rounding shifts tv_nsec out of
     * range.
     */
    if (result_ts->tv_nsec < 0L) {
        result_ts->tv_sec--;
        result_ts->tv_nsec += 1000000000L;
    } else if (result_ts->tv_nsec >= 1000000000L) {
        result_ts->tv_sec++;
        result_ts->tv_nsec -= 1000000000L;
    }
}

int get_current_time(struct timespec *result_ts) {
#if defined(__APPLE__)
    static long double factor = -1;
    long double nsec;
    if (result_ts == NULL) {
        return -1;
    }
    if (factor < 0) {
        mach_timebase_info_data_t timebase_info;
        kern_return_t ret = mach_timebase_info(&timebase_info);
        if (ret != KERN_SUCCESS) {
            return -1;
        }
        factor = (long double)timebase_info.numer / timebase_info.denom;
    }
    nsec = mach_absolute_time() * factor;
    nsec_to_timespec((double)nsec, result_ts);
    return 0;
#elif defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0 && defined(CLOCK_MONOTONIC)
    /* Prefer monotonic clock: immune to system time adjustments */
    return clock_gettime(CLOCK_MONOTONIC, result_ts);
#elif defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0 && defined(CLOCK_REALTIME)
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

int sleep_timespec(const struct timespec *duration) {
    struct timespec request, remaining;
    request = *duration;
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L &&                  \
    defined(_POSIX_CLOCK_SELECTION) && _POSIX_CLOCK_SELECTION > 0 &&           \
    defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0 && defined(CLOCK_MONOTONIC) && \
    (defined(__linux__) || defined(__FreeBSD__))
    /*
     * Use monotonic clock sleep if available.
     * clock_nanosleep returns 0 on success or a positive error number
     * on failure. Convert to -1/errno convention for consistency with
     * nanosleep and the documented return value contract. On EINTR it
     * writes the unslept remainder to 'remaining', which we feed back in.
     */
    for (;;) {
        int ret = clock_nanosleep(CLOCK_MONOTONIC, 0, &request, &remaining);
        if (ret == 0) {
            return 0;
        }
        if (ret == EINTR) {
            request = remaining;
            continue;
        }
        errno = ret;
        return -1;
    }
#else
    /* Fall back to standard nanosleep, resuming on EINTR. */
    for (;;) {
        if (nanosleep(&request, &remaining) == 0) {
            return 0;
        }
        if (errno == EINTR) {
            request = remaining;
            continue;
        }
        return -1;
    }
#endif
}

double timediff_in_ms(const struct timespec *later,
                      const struct timespec *earlier) {
    return difftime(later->tv_sec, earlier->tv_sec) * 1e3 +
           ((double)later->tv_nsec - (double)earlier->tv_nsec) / 1e6;
}
