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

/*
 * LD_PRELOAD hook used by tools/behaviour_baseline.sh to reach the error
 * paths of cpulimit that cannot be provoked with ordinary processes: a
 * target that exists but cannot be signalled, one that can be suspended but
 * not resumed, and the choice between two same-named processes when only one
 * of them is controllable.
 *
 * It interposes kill() only. Nothing else in the process image is touched,
 * and a call that the current configuration does not select is forwarded to
 * the real implementation unchanged.
 *
 * Configuration comes from the environment so the same object serves every
 * injection scenario:
 *
 *   CPULIMIT_HOOK_MODE       what to reject (see below); unset disables all
 *   CPULIMIT_HOOK_TARGET_PID restrict the rejection to this PID; unset means
 *                            every PID, including signal number 0 probes
 *
 * Modes:
 *   eperm_all  every signal to the selected PID fails with EPERM, including
 *              kill(pid, 0), which is how cpulimit probes controllability
 *   deny_cont  only SIGCONT fails; SIGSTOP still works, leaving the target
 *              suspended with no way to resume it
 *   deny_stop  only SIGSTOP fails
 *
 * Build: gcc -shared -fPIC -o hook.so hook.c -ldl
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/* The real kill(), resolved lazily on the first call. */
static int (*real_kill)(pid_t, int) = NULL;

/**
 * @brief Interposed kill() implementing the configured rejection policy
 * @param pid Target process or process group
 * @param sig Signal number to deliver
 * @return 0 when the call succeeded, -1 with errno set otherwise
 *
 * Every path that is not selected by the environment configuration is passed
 * straight through to the real kill(), so the hook is inert unless the
 * scenario explicitly arms it.
 */
int kill(pid_t pid, int sig) {
    const char *mode;
    const char *target;
    pid_t restricted_pid;
    int reject;

    if (real_kill == NULL) {
        /*
         * The union avoids casting between an object pointer and a function
         * pointer, which C89 does not define and pedantic builds reject.
         */
        union {
            void *obj;
            int (*fn)(pid_t, int);
        } pun;
        pun.obj = dlsym(RTLD_NEXT, "kill");
        real_kill = pun.fn;
        if (real_kill == NULL) {
            errno = ENOSYS;
            return -1;
        }
    }

    mode = getenv("CPULIMIT_HOOK_MODE");
    if (mode == NULL || mode[0] == '\0') {
        return real_kill(pid, sig);
    }

    target = getenv("CPULIMIT_HOOK_TARGET_PID");
    if (target != NULL && target[0] != '\0') {
        restricted_pid = (pid_t)atol(target);
        if (pid != restricted_pid) {
            return real_kill(pid, sig);
        }
    }

    reject = 0;
    if (strcmp(mode, "eperm_all") == 0) {
        reject = 1;
    } else if (strcmp(mode, "deny_cont") == 0) {
        reject = (sig == SIGCONT);
    } else if (strcmp(mode, "deny_stop") == 0) {
        reject = (sig == SIGSTOP);
    }

    if (reject) {
        errno = EPERM;
        return -1;
    }
    return real_kill(pid, sig);
}
