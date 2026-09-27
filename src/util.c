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

#include "util.h"

#include <errno.h>
#include <sys/resource.h>

#if defined(__linux__) || defined(__FreeBSD__)
/*
 * sched_setscheduler(2) and SCHED_FIFO live in <sched.h> on Linux (glibc) and
 * FreeBSD. They are intentionally not pulled in on macOS, which has no POSIX
 * real-time scheduler and instead uses the Mach THREAD_TIME_CONSTRAINT_POLICY
 * (see try_become_realtime() below).
 */
#include <sched.h>
#endif

#ifdef __APPLE__
/*
 * macOS/Darwin has no POSIX real-time scheduler; the real-time lever is the
 * Mach thread-policy API in these headers (used by try_become_realtime()).
 */
#include <mach/mach.h>
#include <mach/thread_policy.h>
#endif

#ifdef CPULIMIT_IMPL_GETLOADAVG
#include <stdlib.h>
#include <sys/sysinfo.h>
#endif

#if defined(__linux__) || defined(__FreeBSD__)
/**
 * @brief Attempt to raise the current process to real-time priority
 *
 * @note This is a best-effort attempt; if the privilege is unavailable, the
 *       call fails silently.
 */
static void try_become_realtime(void) {
    struct sched_param sp;
    int policy = SCHED_FIFO;
    sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
#ifdef SCHED_RESET_ON_FORK
    policy |= SCHED_RESET_ON_FORK;
#endif
    (void)sched_setscheduler(0, policy, &sp);
}
#elif defined(__APPLE__)
/**
 * @brief Attempt to raise the current process to real-time priority
 *
 * @note This is a best-effort attempt; if the privilege is unavailable, the
 *       call fails silently.
 */
static void try_become_realtime(void) {
    thread_time_constraint_policy_data_t policy;
    mach_port_t thread = mach_thread_self();
    /* period/computation/constraint are in AbsoluteTime (ns on modern Darwin).
       A modest, preemptible budget: 100us period, 50us computation, 100us
       constraint. */
    policy.period = 100000;
    policy.computation = 50000;
    policy.constraint = 100000;
    policy.preemptible = 1;
    (void)thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY,
                            (thread_policy_t)&policy,
                            THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    (void)mach_port_deallocate(mach_task_self(), thread);
}
#endif /* platform selection */

/**
 * @brief Raise the scheduling priority of the current process
 *
 * Iterates nice values from PRIO_MIN upward until one succeeds, skipping levels
 * denied by permissions (RLIMIT_NICE), then best-effort real-time promotion.
 */
void increase_priority(void) {
    int old_priority, priority;
    errno = 0;
    old_priority = getpriority(PRIO_PROCESS, 0);
    if (old_priority == -1 && errno != 0) {
        /* Error getting current priority, assume default priority */
        old_priority = 0;
    }
    /* Best-effort real-time promotion so SIGSTOP is prompt; see
       try_become_realtime() above. No-op without sufficient privilege, in
       which case the portable nice() ladder below remains the only lever. */
    try_become_realtime();
    for (priority = PRIO_MIN; priority < old_priority; priority++) {
        errno = 0;
        if (setpriority(PRIO_PROCESS, 0, priority) == 0) {
            break;
        }
        /*
         * Permission denied at this level. Continue to the next
         * (less aggressive) priority: RLIMIT_NICE may allow a
         * value less negative than PRIO_MIN even without root.
         *
         * Both EPERM and EACCES mean "not allowed to raise the
         * priority this far" and POSIX permits either, so both must
         * be retried: Linux reports EACCES when CAP_SYS_NICE is
         * missing (and EPERM when RLIMIT_NICE is exceeded), while
         * the BSDs report EACCES precisely for the "lower the nice
         * value" denial. Retrying only EPERM aborted the ladder on
         * its very first rung on Linux, leaving cpulimit at default
         * priority even when a milder level would have been
         * accepted.
         */
        if (errno == EPERM || errno == EACCES) {
            continue;
        }
        break;
    }
}

#ifdef CPULIMIT_IMPL_GETLOADAVG
/**
 * @brief Get system load averages (custom implementation for old uClibc)
 *
 * @param loadavg Array to receive load average values
 * @param nelem Number of load averages to retrieve (1-3: 1min, 5min, 15min)
 * @return Number of samples retrieved (nelem), or -1 on error
 *
 * Retrieves system load averages using the sysinfo() syscall and converts
 * the fixed-point values to floating-point. This implementation is used
 * only on uClibc/uClibc-ng versions < 1.0.42 which lack getloadavg().
 */
int getloadavg_impl(double *loadavg, int nelem) {
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

    nelem = (nelem > 3) ? 3 : nelem;

    for (load_idx = 0; load_idx < nelem; load_idx++) {
        loadavg[load_idx] =
            (double)sys_info.loads[load_idx] / (1 << SI_LOAD_SHIFT);
    }

    return nelem;
}
#endif

/**
 * @brief Safely convert long to pid_t with overflow detection
 *
 * @param long_pid Long value to convert to pid_t
 * @return The pid_t value on success, or -1 if long_pid < 0 or overflow occurs
 *
 * @note The cast is implementation-defined when out of pid_t range, but the
 *       round-trip check detects overflow on Linux, macOS and FreeBSD.
 */
pid_t long_to_pid_t(long long_pid) {
    pid_t result;
    if (long_pid < 0) {
        return (pid_t)(-1);
    }
    result = (pid_t)long_pid;
    if ((long)result != long_pid) {
        return (pid_t)(-1);
    }
    return result;
}
