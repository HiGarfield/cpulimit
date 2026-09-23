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

#ifdef CPULIMIT_IMPL_GETLOADAVG
#include <stdlib.h>
#include <sys/sysinfo.h>
#endif

void increase_priority(void) {
    int old_priority, priority;
    errno = 0;
    old_priority = getpriority(PRIO_PROCESS, 0);
    if (old_priority == -1 && errno != 0) {
        /* Error getting current priority, assume default priority */
        old_priority = 0;
    }
    /* Try to set highest priority, working upward if denied */
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
