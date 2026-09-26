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
 * FreeBSD.  They are intentionally not pulled in on macOS, which has no POSIX
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
/*
 * Best-effort: raise cpulimit to SCHED_FIFO (lowest RT priority) so it can
 * preempt the throttled process the instant it wakes from nanosleep and deliver
 * SIGSTOP promptly, instead of waiting for the target to yield.  On a
 * non-fully-preemptible kernel (e.g. 2.6.9 PREEMPT_VOLUNTARY) a busy-looping
 * target otherwise starves cpulimit and the enforced duty cycle becomes biased
 * and noisy -- which is exactly the limit-cycle oscillation seen on such
 * kernels.
 *
 * Both Linux and FreeBSD implement the POSIX real-time scheduler; priority 1 is
 * the lowest FIFO priority on either.  cpulimit sleeps between signals, so an
 * RT priority only matters during the brief wakeup-to-signal window and never
 * monopolizes the CPU.  If CAP_SYS_NICE (Linux) or the required privilege
 * (FreeBSD) is unavailable the call fails silently and the portable nice()
 * ladder in increase_priority() remains the only lever.
 */
static void try_become_realtime(void) {
    struct sched_param sp;
    sp.sched_priority = 1;
    (void)sched_setscheduler(0, SCHED_FIFO, &sp);
}
#elif defined(__APPLE__)
/*
 * macOS/Darwin has no POSIX SCHED_FIFO/SCHED_RR, so the equivalent real-time
 * lever is the Mach THREAD_TIME_CONSTRAINT_POLICY: it schedules the calling
 * thread with a bounded time constraint so it can preempt the throttled busy
 * loop promptly and deliver SIGSTOP.  cpulimit sleeps between signals, so the
 * thread is only "real-time" during the brief wakeup-to-signal window and does
 * not monopolize the CPU.  If the policy cannot be applied the call is silently
 * ignored and the portable nice() ladder below remains the only lever.
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

void increase_priority(void) {
    int old_priority, priority;
    errno = 0;
    old_priority = getpriority(PRIO_PROCESS, 0);
    if (old_priority == -1 && errno != 0) {
        /* Error getting current priority, assume default priority */
        old_priority = 0;
    }
    /* Best-effort real-time promotion so SIGSTOP is prompt; see
       try_become_realtime() above.  No-op without sufficient privilege, in
       which case the portable nice() ladder below remains the only lever. */
    try_become_realtime();
    /* Portable priority boost: raise the nice priority as far as permitted.
       Used on macOS, FreeBSD, and Linux without CAP_SYS_NICE. */
    for (priority = PRIO_MIN; priority < old_priority; priority++) {
        errno = 0;
        if (setpriority(PRIO_PROCESS, 0, priority) == 0) {
            break; /* Successfully set priority */
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
         * value" denial.  Retrying only EPERM aborted the ladder on
         * its very first rung on Linux, leaving cpulimit at default
         * priority even when a milder level would have been
         * accepted.
         */
        if (errno == EPERM || errno == EACCES) {
            continue;
        }
        /* Any other error is unexpected; stop trying */
        break;
    }
}

#ifdef CPULIMIT_IMPL_GETLOADAVG
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

    /* Retrieve at most 3 load averages */
    nelem = (nelem > 3) ? 3 : nelem;

    /* Convert fixed-point to floating-point using SI_LOAD_SHIFT */
    for (load_idx = 0; load_idx < nelem; load_idx++) {
        loadavg[load_idx] =
            (double)sys_info.loads[load_idx] / (1 << SI_LOAD_SHIFT);
    }

    return nelem;
}
#endif

pid_t long_to_pid_t(long long_pid) {
    pid_t result;
    /* Reject negative values */
    if (long_pid < 0) {
        return (pid_t)(-1);
    }
    /* Cast to pid_t and verify no overflow occurred */
    result = (pid_t)long_pid;
    if ((long)result != long_pid) {
        return (pid_t)(-1);
    }
    return result;
}
