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
/* <sched.h> is Linux/FreeBSD only; macOS uses the Mach thread policy API. */
#include <sched.h>
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/thread_policy.h>
#endif

#include <stdlib.h>
#ifdef CPULIMIT_IMPL_GETLOADAVG
#include <sys/sysinfo.h>
#endif

#if defined(__linux__) || defined(__FreeBSD__)
/**
 * @brief Try to raise the current process to a real-time policy.
 *
 * @note Best effort; an unavailable privilege fails silently.
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
 * @brief Try to raise the current process to a real-time policy.
 *
 * @note Best effort; an unavailable privilege fails silently.
 */
static void try_become_realtime(void) {
    thread_time_constraint_policy_data_t policy;
    mach_port_t thread = mach_thread_self();
    /* Mach budget: 100us period, 50us computation, 100us constraint. */
    policy.period = 100000;
    policy.computation = 50000;
    policy.constraint = 100000;
    policy.preemptible = 1;
    (void)thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY,
                            (thread_policy_t)&policy,
                            THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    (void)mach_port_deallocate(mach_task_self(), thread);
}
#endif

/**
 * @brief Raise the scheduling priority of the current process.
 *
 * Tries a real-time policy first, then lowers the nice value one step at a
 * time, retrying levels denied by EPERM or EACCES. Best effort throughout.
 */
void increase_priority(void) {
    int old_priority, priority;
    try_become_realtime();
    errno = 0;
    old_priority = getpriority(PRIO_PROCESS, 0);
    if (old_priority == -1 && errno != 0) {
        old_priority = 0;
    }
    /* Clamp to the nice range so the number of ladder steps stays bounded. */
    old_priority = CLAMP(old_priority, PRIO_MIN, PRIO_MAX);
    for (priority = old_priority; priority > PRIO_MIN; priority--) {
        errno = 0;
        if (setpriority(PRIO_PROCESS, 0, priority - 1) != 0 && errno != EPERM &&
            errno != EACCES) {
            break;
        }
    }
}

/**
 * @brief Get system load averages.
 *
 * @param loadavg Array receiving the samples.
 * @param nelem Number of samples requested (1-3: 1, 5 and 15 minutes).
 * @return Number of samples written, capped at 3; 0 for nelem 0; -1 for a
 *         negative count or a system error.
 *
 * Wraps getloadavg(), or derives the values from sysinfo() on uClibc
 * versions that lack it. Both paths validate nelem before writing.
 */
int cpulimit_getloadavg(double *loadavg, int nelem) {
#ifdef CPULIMIT_IMPL_GETLOADAVG
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
#else
    /* Some libc getloadavg() skip validation; bound and clamp nelem. */
    if (nelem < 0) {
        return -1;
    }
    if (nelem == 0) {
        return 0;
    }
    if (nelem > 3) {
        nelem = 3;
    }
    return getloadavg(loadavg, nelem);
#endif
}

/**
 * @brief Convert a long to pid_t with overflow detection.
 *
 * @param long_pid Value to convert.
 * @return The converted PID, or -1 for a negative value or overflow.
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
