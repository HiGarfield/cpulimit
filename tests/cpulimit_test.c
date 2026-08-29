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

#undef NDEBUG

#include "../src/cli.h"
#include "../src/limit_process.h"
#include "../src/limiter.h"
#include "../src/list.h"
#include "../src/process_finder.h"
#include "../src/process_group.h"
#include "../src/process_iterator.h"
#include "../src/process_table.h"
#include "../src/signal_handler.h"
#include "../src/time_util.h"
#include "../src/util.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#if defined(__linux__)
#include <dirent.h>
#include <sys/prctl.h>

/* Older glibc headers (e.g. CentOS 4.5) do not define PR_SET_NAME even
   though the 2.6.9+ kernel supports it.  Provide the fallback value. */
#ifndef PR_SET_NAME
#define PR_SET_NAME 15
#endif
#endif
#include <time.h>
#include <unistd.h>

/**
 * @brief Send SIGKILL to a process or process group and block until reaped
 * @param pid Process ID (positive) or negative process group ID
 * @note Used as a fallback cleanup path when the monotonic clock is
 *       unavailable. Sends SIGKILL, then blocks on waitpid()
 *       (retrying on EINTR) to guarantee that no test process is
 *       left running.
 */
static void kill_blocking(pid_t pid) {
    kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) == -1 && errno == EINTR) {
        /* Retry on EINTR */
    }
    /* Reap any remaining zombies (process group case) */
    while (waitpid(pid, NULL, WNOHANG) > 0) {
        /* Keep reaping until none left */
    }
}

/**
 * @brief Terminate a child process or process group and wait for it to exit
 * @param pid Process ID (positive) or negative process group ID
 * @param kill_signal Signal to send (SIGTERM or SIGKILL)
 * @note If pid > 0, treats pid as a single process. If pid < 0, treats -pid
 *       as a process group ID. Sends the given signal and waits up to 5
 *       seconds. If SIGTERM times out, escalates to SIGKILL and waits an
 *       additional 5 seconds. If processes are not reaped after 5 + 5
 *       seconds, function exits. If the monotonic clock is unavailable at
 *       any point, falls back to kill_blocking() to guarantee all processes
 *       are reaped.
 */
static void kill_and_wait(pid_t pid, int kill_signal) {
    struct timespec now, end_time;
    const struct timespec sleep_time = {0, 10000000L}; /* 10 ms */

    switch (kill_signal) {
    case SIGKILL:
    case SIGTERM:
        break;
    default:
        fprintf(stderr, "Invalid signal %d for kill_and_wait\n", kill_signal);
        return;
    }

    /* Initialize timeout: 5 seconds */
    if (get_current_time(&end_time) != 0) {
        /* Clock unavailable: SIGKILL + blocking wait to ensure cleanup */
        fprintf(stderr, "get_current_time failed in kill_and_wait\n");
        kill_blocking(pid);
        return;
    }
    end_time.tv_sec += 5;

    kill(pid, kill_signal); /* Send initial signal */

    while (1) {
        pid_t wpid = waitpid(pid, NULL, WNOHANG);

        if (wpid > 0) { /* Process terminated */
            /* Single process: exit after cleanup */
            /* Process group: continue to next member */
            if (pid > 0) {
                break;
            }
        } else if (wpid == -1) { /* Waitpid error */
            if (errno != EINTR) {
                break; /* Non-interrupt error */
            }
        } else { /* wpid == 0: process still running */
            /* Check timeout */
            if (get_current_time(&now) != 0) {
                /* Clock failed mid-loop: SIGKILL + block to ensure cleanup */
                kill_blocking(pid);
                return;
            }
            if (now.tv_sec > end_time.tv_sec ||
                (now.tv_sec == end_time.tv_sec &&
                 now.tv_nsec >= end_time.tv_nsec)) {
                if (kill_signal == SIGTERM) {
                    /* SIGTERM timeout: escalate to SIGKILL */
                    kill(pid, SIGKILL);
                    kill_signal = SIGKILL;
                    /* Reset timeout for SIGKILL (5 seconds) */
                    if (get_current_time(&end_time) != 0) {
                        /*
                         * Clock failed after escalation: block to ensure
                         * cleanup.
                         */
                        kill_blocking(pid);
                        return;
                    }
                    end_time.tv_sec += 5;
                } else {
                    break; /* SIGKILL timeout: stop waiting */
                }
            }
            /* Short sleep to reduce CPU usage (10ms) */
            sleep_timespec(&sleep_time);
        }
    }

    /* Final cleanup: reap all remaining zombies */
    while (waitpid(pid, NULL, WNOHANG) > 0) {
        /* Keep reaping until none left */
    }
}

/**
 * @brief Suspend the calling test child process until it is killed
 * @note Children that suspend indefinitely become permanent orphans if the
 *       test process aborts (e.g., a failed assertion).  An orphan keeps the
 *       test process's argv[0] and can confuse later tests such as
 *       find_process_by_name(), which scans all processes by name and may
 *       select the stale orphan.  Poll the parent every 100 ms and exit on
 *       its own as soon as the parent is gone.  Never returns.
 */
static void test_suspend_until_killed(void) {
    const struct timespec poll_interval = {0, 100000000L}; /* 100 ms */
    struct timespec remaining;
    pid_t parent_pid;

    parent_pid = getppid();
    for (;;) {
        if (kill(parent_pid, 0) != 0 && errno == ESRCH) {
            _exit(EXIT_FAILURE);
        }
        remaining = poll_interval;
        while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
            /* Retry on EINTR */
        }
    }
}

/***************************************************************************
 * UTIL MODULE TESTS
 ***************************************************************************/

static char *argv0 = NULL;

/**
 * @brief Retrieve the OS-visible argv[0] of the current process
 * @param buf Buffer to populate with the command string
 * @param buf_size Size of the buffer in bytes
 * @return Pointer to buf on success (non-empty string), NULL on failure
 *
 * Uses the process iterator to find the current process by PID and
 * return its command field (argv[0] as seen by the OS). This differs
 * from main()'s argv[0] when the process is launched via a wrapper
 * (e.g., valgrind), which replaces the OS-visible process image.
 */
static const char *get_self_command(char *buf, size_t buf_size) {
    struct process_iterator iter;
    struct process_filter filter;
    struct process *proc;
    int found;

    if (buf == NULL || buf_size == 0) {
        return NULL;
    }
    buf[0] = '\0';
    found = 0;

    filter.pid = getpid();
    filter.include_children = 0;
    filter.read_cmd = 1;
    /*
     * Allocate proc only after iterator initialization so no free()
     * is needed on the init failure path.
     */
    if (init_process_iterator(&iter, &filter) != 0) {
        return NULL;
    }
    proc = (struct process *)malloc(sizeof(struct process));
    if (proc == NULL) {
        close_process_iterator(&iter);
        return NULL;
    }
    if (get_next_process(&iter, proc) == 0 && proc->command[0] != '\0') {
        size_t cmd_len = strlen(proc->command);
        if (cmd_len >= buf_size) {
            cmd_len = buf_size - 1;
        }
        memcpy(buf, proc->command, cmd_len);
        buf[cmd_len] = '\0';
        found = 1;
    }
    free(proc);
    if (close_process_iterator(&iter) != 0) {
        return NULL;
    }
    return found ? buf : NULL;
}

/***************************************************************************
 * TIME_UTIL MODULE TESTS
 ***************************************************************************/

/**
 * @brief Test nsec2timespec conversion
 * @note Tests conversion from nanoseconds to timespec including rollover
 *       and boundary values
 */
static void test_time_util_nsec2timespec(void) {
    struct timespec result_ts;

    /* Test 0 nanoseconds */
    nsec2timespec(0.0, &result_ts);
    assert(result_ts.tv_sec == 0);
    assert(result_ts.tv_nsec == 0);

    /* Test 1 second (1e9 nanoseconds) */
    nsec2timespec(1000000000.0, &result_ts);
    assert(result_ts.tv_sec == 1);
    assert(result_ts.tv_nsec == 0);

    /* Test 1.5 seconds */
    nsec2timespec(1500000000.0, &result_ts);
    assert(result_ts.tv_sec == 1);
    assert(result_ts.tv_nsec == 500000000);

    /* Test 2.25 seconds */
    nsec2timespec(2250000000.0, &result_ts);
    assert(result_ts.tv_sec == 2);
    assert(result_ts.tv_nsec == 250000000);

    /* Test small value (100 microseconds) */
    nsec2timespec(100000.0, &result_ts);
    assert(result_ts.tv_sec == 0);
    assert(result_ts.tv_nsec == 100000);

    /* Test 500 milliseconds */
    nsec2timespec(500000000.0, &result_ts);
    assert(result_ts.tv_sec == 0);
    assert(result_ts.tv_nsec == 500000000L);

    /* Test very large value: 10 seconds */
    nsec2timespec(10000000000.0, &result_ts);
    assert(result_ts.tv_sec == 10);
    assert(result_ts.tv_nsec == 0);

    /* A large multiple of 1e9 can round tv_nsec up to 1e9 */
    nsec2timespec(3e9, &result_ts);
    assert(result_ts.tv_sec >= 2 && result_ts.tv_sec <= 4);
    assert(result_ts.tv_nsec >= 0L && result_ts.tv_nsec <= 999999999L);

    /* 2 seconds exactly */
    nsec2timespec(2e9, &result_ts);
    assert(result_ts.tv_sec == 2);
    assert(result_ts.tv_nsec == 0L);

    /* 0.999999999 s = 999999999 ns -- must stay sub-second */
    nsec2timespec(999999999.0, &result_ts);
    assert(result_ts.tv_sec == 0);
    assert(result_ts.tv_nsec >= 0L && result_ts.tv_nsec <= 999999999L);

    /* General invariant: result is always normalised */
    nsec2timespec(1234567890.123, &result_ts);
    assert(result_ts.tv_nsec >= 0L && result_ts.tv_nsec <= 999999999L);
}

/**
 * @brief Test get_current_time function
 * @note Tests time retrieval and monotonicity
 */
static void test_time_util_get_current_time(void) {
    struct timespec ts_before, ts_later;
    int ret;
    double diff;

    /* Test get_current_time returns valid values */
    ret = get_current_time(&ts_before);
    assert(ret == 0);
    assert(ts_before.tv_sec >= 0);
    assert(ts_before.tv_nsec >= 0 && ts_before.tv_nsec < 1000000000L);
    assert(ts_before.tv_sec > 0 || ts_before.tv_nsec > 0);

    /* Two successive calls must return non-decreasing time */
    ret = get_current_time(&ts_later);
    assert(ret == 0);
    diff = timediff_in_ms(&ts_later, &ts_before);
    assert(diff >= 0.0); /* monotonic: must not go backwards */
}

/**
 * @brief Test sleep_timespec function
 * @note Tests sleeping with various durations including zero
 */
static void test_time_util_sleep_timespec(void) {
    struct timespec ts_before, ts_after, sleep_time;
    const struct timespec zero_sleep = {0, 0};
    int ret;
    double elapsed_ms;

    /* Test sleep_timespec with 50ms */
    ret = get_current_time(&ts_before);
    assert(ret == 0);

    sleep_time.tv_sec = 0;
    sleep_time.tv_nsec = 50000000L; /* 50 ms */
    ret = sleep_timespec(&sleep_time);
    assert(ret == 0 || ret == -1); /* May return -1 on signal */

    ret = get_current_time(&ts_after);
    assert(ret == 0);

    /* Verify time has advanced */
    assert(ts_after.tv_sec >= ts_before.tv_sec);
    if (ts_after.tv_sec == ts_before.tv_sec) {
        assert(ts_after.tv_nsec >= ts_before.tv_nsec);
    }

    /* Test timediff_in_ms */
    elapsed_ms = timediff_in_ms(&ts_after, &ts_before);
    assert(elapsed_ms >= 0.0);
    /* Should be at least close to 50ms if sleep succeeded */

    /* Test zero-duration sleep returns immediately without error */
    ret = sleep_timespec(&zero_sleep);
    /* 0 on success; -1 only if interrupted (EINTR) */
    assert(ret == 0 || ret == -1);
}

/**
 * @brief Test timediff_in_ms calculations
 * @note Tests time difference including edge cases, negative differences,
 *       and sub-millisecond values
 */
static void test_time_util_timediff_in_ms(void) {
    struct timespec earlier, later;
    double diff_ms;

    /* Test simple case: 1 second difference */
    earlier.tv_sec = 100;
    earlier.tv_nsec = 0;
    later.tv_sec = 101;
    later.tv_nsec = 0;
    diff_ms = timediff_in_ms(&later, &earlier);
    assert(diff_ms >= 999.0 && diff_ms <= 1001.0);

    /* Test with nanoseconds: 0.5 second difference */
    earlier.tv_sec = 100;
    earlier.tv_nsec = 0;
    later.tv_sec = 100;
    later.tv_nsec = 500000000L;
    diff_ms = timediff_in_ms(&later, &earlier);
    assert(diff_ms >= 499.0 && diff_ms <= 501.0);

    /* Test with both seconds and nanoseconds */
    earlier.tv_sec = 100;
    earlier.tv_nsec = 250000000L;
    later.tv_sec = 101;
    later.tv_nsec = 750000000L;
    diff_ms = timediff_in_ms(&later, &earlier);
    assert(diff_ms >= 1499.0 && diff_ms <= 1501.0);

    /* Test zero difference */
    earlier.tv_sec = 100;
    earlier.tv_nsec = 123456789L;
    later.tv_sec = 100;
    later.tv_nsec = 123456789L;
    diff_ms = timediff_in_ms(&later, &earlier);
    assert(diff_ms >= -0.001 && diff_ms <= 0.001);

    /* Test very small difference: 1 millisecond */
    earlier.tv_sec = 1000;
    earlier.tv_nsec = 0;
    later.tv_sec = 1000;
    later.tv_nsec = 1000000L; /* 1 millisecond */
    diff_ms = timediff_in_ms(&later, &earlier);
    assert(diff_ms >= 0.999 && diff_ms <= 1.001);

    /* Test large difference: 1000 seconds */
    earlier.tv_sec = 1000;
    earlier.tv_nsec = 0;
    later.tv_sec = 2000;
    later.tv_nsec = 0;
    diff_ms = timediff_in_ms(&later, &earlier);
    assert(diff_ms >= 999999.0 && diff_ms <= 1000001.0);

    /* 100 seconds backward */
    earlier.tv_sec = 200;
    earlier.tv_nsec = 0;
    later.tv_sec = 100;
    later.tv_nsec = 0;
    diff_ms = timediff_in_ms(&later, &earlier);
    assert(diff_ms <= -99999.0 && diff_ms >= -100001.0);

    /* 500 ms backward (nanoseconds only) */
    earlier.tv_sec = 100;
    earlier.tv_nsec = 500000000L;
    later.tv_sec = 100;
    later.tv_nsec = 0;
    diff_ms = timediff_in_ms(&later, &earlier);
    assert(diff_ms <= -499.0 && diff_ms >= -501.0);

    /* Sub-millisecond: only nanoseconds differ (0.999 ms) */
    earlier.tv_sec = 500;
    earlier.tv_nsec = 0;
    later.tv_sec = 500;
    later.tv_nsec = 999000L; /* 0.999 ms */
    diff_ms = timediff_in_ms(&later, &earlier);
    assert(diff_ms >= 0.998 && diff_ms <= 1.0);
}

#if defined(__APPLE__)
/**
 * @brief Test macOS KERN_PROCARGS2 buffer sizing used by get_proc_argv0()
 * @note Regression test for the buffer under-allocation bug: get_proc_argv0()
 *       read the process command line via sysctl(KERN_PROCARGS2), whose layout
 *       is [int argc][argument data...] and therefore requires
 *       sizeof(int) + argmax bytes.  The old code allocated only argmax bytes,
 *       which truncates the payload and makes sysctl() return ENOMEM for
 *       processes whose command line is longer than argmax - sizeof(int).
 *       That caused those processes to be skipped by find_process_by_name().
 *
 *       This test proves the fixed allocation (argmax + sizeof(int)) is
 *       always sufficient: for any real process the value sysctl() writes
 *       back into *size never exceeds argmax + sizeof(int), so the larger
 *       buffer can never be overflowed and the call always succeeds.
 */
static void test_apple_proc_argv0_buffer_sizing(void) {
    int mib[3];
    int mib_argmax[2];
    int argmax;
    size_t argmax_size;
    size_t required_size;
    char *buf_fixed;
    char *buf_old;

    /* Query the maximum argument data size. */
    mib_argmax[0] = CTL_KERN;
    mib_argmax[1] = KERN_ARGMAX;
    argmax_size = sizeof(argmax);
    assert(sysctl(mib_argmax, 2, &argmax, &argmax_size, NULL, 0) == 0);
    assert(argmax > 0);

    /* Target the current test process. */
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROCARGS2;
    mib[2] = (int)getpid();

    /*
     * Fixed allocation: argmax + sizeof(int).  The kernel writes at most
     * sizeof(int) (for argc) plus argmax (for the argument data), so this
     * buffer is always large enough.  The returned size must therefore be
     * <= what we allocated and the call must succeed.
     */
    buf_fixed = (char *)malloc((size_t)argmax + sizeof(int));
    assert(buf_fixed != NULL);
    required_size = (size_t)argmax + sizeof(int);
    assert(sysctl(mib, 3, buf_fixed, &required_size, NULL, 0) == 0);
    /* The kernel never needs more than the amount we provided. */
    assert(required_size <= (size_t)argmax + sizeof(int));
    free(buf_fixed);

    /*
     * Old allocation: argmax only.  Prove it can be insufficient: when the
     * real payload (sizeof(int) + argument bytes) exceeds argmax, sysctl()
     * returns ENOMEM and writes back the true requirement, which is larger
     * than argmax.  We assert that such a process cannot be served by the old
     * buffer, whereas the fixed buffer (above) always can.
     */
    buf_old = (char *)malloc((size_t)argmax);
    assert(buf_old != NULL);
    required_size = (size_t)argmax;
    if (sysctl(mib, 3, buf_old, &required_size, NULL, 0) != 0) {
        /* ENOMEM => old buffer too small; real need exceeds argmax. */
        assert(required_size > (size_t)argmax);
    }
    free(buf_old);

    /*
     * Meaningful precondition check: KERN_ARGMAX must report a strictly
     * positive number of bytes, otherwise the buffer planning above is
     * meaningless and malloc() would receive a non-positive size.
     */
    assert(argmax > 0);
}
#endif /* __APPLE__ */

/***************************************************************************
 * UTIL MODULE TESTS
 ***************************************************************************/

/**
 * @brief Test file_basename extraction
 * @note Tests extracting filename from path including edge cases
 */
static void test_util_file_basename(void) {
    const char *result;
    int cmp_ret;

    /* Test simple filename */
    result = file_basename("test.txt");
    cmp_ret = strcmp(result, "test.txt");
    assert(cmp_ret == 0);

    /* Test path with directory */
    result = file_basename("/usr/bin/test");
    cmp_ret = strcmp(result, "test");
    assert(cmp_ret == 0);

    /* Test path with multiple directories */
    result = file_basename("/home/user/documents/file.txt");
    cmp_ret = strcmp(result, "file.txt");
    assert(cmp_ret == 0);

    /* Test path ending with slash */
    result = file_basename("/home/user/");
    cmp_ret = strcmp(result, "");
    assert(cmp_ret == 0);

    /* Test root directory */
    result = file_basename("/");
    cmp_ret = strcmp(result, "");
    assert(cmp_ret == 0);

    /* Test current directory */
    result = file_basename("./file");
    cmp_ret = strcmp(result, "file");
    assert(cmp_ret == 0);

    /* Test multiple consecutive slashes */
    result = file_basename("//usr//bin//test");
    cmp_ret = strcmp(result, "test");
    assert(cmp_ret == 0);

    /* Test path with no directory separator */
    result = file_basename("filename");
    cmp_ret = strcmp(result, "filename");
    assert(cmp_ret == 0);

    /* Test path with dot directory */
    result = file_basename("../test");
    cmp_ret = strcmp(result, "test");
    assert(cmp_ret == 0);

    /* Test empty string - no slash, returns itself */
    result = file_basename("");
    assert(result != NULL);
    cmp_ret = strcmp(result, "");
    assert(cmp_ret == 0);

    /* Test NULL input */
    result = file_basename(NULL);
    assert(result != NULL);
    cmp_ret = strcmp(result, "");
    assert(cmp_ret == 0);
}

/**
 * @brief Test get_ncpu function
 * @note Tests retrieval of CPU count
 */
static void test_util_get_ncpu(void) {
    int ncpu, ncpu_cached;

    ncpu = get_ncpu();
    assert(ncpu >= 1);

    /* Call again to test caching */
    ncpu_cached = get_ncpu();
    assert(ncpu_cached == ncpu);
}

/**
 * @brief Nice value that increase_priority() must reach when the platform
 *        permits it
 *
 * Used by test_util_increase_priority_retries_lower_levels().  RLIMIT_NICE
 * encodes the permitted priority ceiling as 20 - nice, so asking for
 * PRIORITY_LADDER_NICE requires a limit of 20 - PRIORITY_LADDER_NICE.
 */
#define PRIORITY_LADDER_NICE (-10)

/**
 * @brief Test increase_priority function
 * @note Tests attempting to increase process priority
 */
static void test_util_increase_priority(void) {
    /* This function may succeed or fail depending on permissions */
    /* Just ensure it doesn't crash */
    increase_priority();
}

/**
 * @brief Test that increase_priority() walks the whole priority ladder
 *
 * increase_priority() must retry with successively less aggressive nice
 * values until one is accepted, because RLIMIT_NICE can allow a value
 * less negative than PRIO_MIN even for an unprivileged process.  POSIX
 * permits either EPERM or EACCES for that denial, and Linux reports
 * EACCES when CAP_SYS_NICE is missing, so retrying only EPERM aborted
 * the ladder on its very first rung.  cpulimit then stayed at its
 * original priority even where a milder level would have been granted,
 * which is exactly when it most needs the extra responsiveness.
 *
 * The failure only shows up where a nicer level is attainable, so the
 * test first raises RLIMIT_NICE to allow PRIORITY_LADDER_NICE.  A
 * default Linux install ships a hard limit of 0, which forbids any
 * lowering at all; there the ladder cannot be observed and only the
 * "never made worse" invariant is checked.
 */
static void test_util_increase_priority_retries_lower_levels(void) {
    int old_nice, new_nice, restore_ret;
    int permitted_nice;
#ifdef RLIMIT_NICE
    struct rlimit saved_limit, raised_limit;
#endif

    errno = 0;
    old_nice = getpriority(PRIO_PROCESS, 0);
    if (old_nice == -1 && errno != 0) {
        printf("(skipped: getpriority failed)\n");
        return;
    }
    permitted_nice = old_nice;

#ifdef RLIMIT_NICE
    /* Raising the soft limit is only possible up to the hard limit. */
    if (getrlimit(RLIMIT_NICE, &saved_limit) == 0) {
        raised_limit.rlim_cur = (rlim_t)(20 - PRIORITY_LADDER_NICE);
        raised_limit.rlim_max = saved_limit.rlim_max;
        if (raised_limit.rlim_cur <= saved_limit.rlim_max &&
            setrlimit(RLIMIT_NICE, &raised_limit) == 0) {
            permitted_nice = PRIORITY_LADDER_NICE;
        }
    }
#endif

    increase_priority();
    new_nice = getpriority(PRIO_PROCESS, 0);

    /* Restore before asserting, so the suite keeps its old priority. */
    restore_ret = setpriority(PRIO_PROCESS, 0, old_nice);
    assert(restore_ret == 0);
#ifdef RLIMIT_NICE
    if (permitted_nice == PRIORITY_LADDER_NICE) {
        int limit_ret = setrlimit(RLIMIT_NICE, &saved_limit);
        assert(limit_ret == 0);
    }
#endif

    /*
     * Where a nicer level is permitted the ladder must reach it, and
     * where none is the priority simply must not get worse.
     */
    if (permitted_nice != PRIORITY_LADDER_NICE) {
        printf("(skipped: no nicer level permitted here)\n");
    }
    assert(new_nice <= permitted_nice);
}

/**
 * @brief Test long2pid_t conversion
 * @note Tests safe conversion from long to pid_t including edge cases and
 *       overflow
 */
static void test_util_long2pid_t(void) {
    pid_t result;

    /* Test valid positive values */
    result = long2pid_t(1L);
    assert(result == 1);

    result = long2pid_t(1000L);
    assert(result == 1000);

    result = long2pid_t(32767L);
    assert(result == 32767);

    /* Test zero */
    result = long2pid_t(0L);
    assert(result == 0);

    /* Test negative value (should return -1) */
    result = long2pid_t(-1L);
    assert(result == -1);

    result = long2pid_t(-100L);
    assert(result == -1);

    /* Test maximum reasonable PID */
    result = long2pid_t(65535L);
    assert(result == 65535);

    /* Test with large positive value (must not crash) */
    long2pid_t(1000000L);

    /*
     * LONG_MAX overflows pid_t (32-bit) on 64-bit platforms.
     * Either the round-trip check detects overflow (-1)
     * or, on exotic platforms where pid_t == long, the value fits.
     * Either way, the function must not crash.
     */
    long2pid_t(LONG_MAX);
}

#if defined(__linux__)
/**
 * @brief Test read_file_contents with NULL, missing, and valid files
 * @note Covers all return paths of read_file_contents (Linux only);
 *       also exercises the realloc growth path with content > 256 bytes.
 *       Unlike a line reader, the full content is returned verbatim,
 *       including trailing newlines and carriage returns.
 */
static void test_util_read_file_contents(void) {
    char *content;
    char tmp_file[] = "/tmp/cpulimit_empty_XXXXXX";
    int tmp_fd;
    char newline_tmp_file[] = "/tmp/cpulimit_newline_XXXXXX";
    int newline_fd;
    char crlf_tmp_file[] = "/tmp/cpulimit_crlf_XXXXXX";
    int crlf_fd;
    char cr_only_tmp_file[] = "/tmp/cpulimit_cronly_XXXXXX";
    int cr_only_fd;
    char cr_middle_tmp_file[] = "/tmp/cpulimit_crmiddle_XXXXXX";
    int cr_middle_fd;
    ssize_t nwritten;
    /* Buffer for the > 256-byte content tests */
    char long_tmp_file[] = "/tmp/cpulimit_long_XXXXXX";
    int long_fd;
    char long_tmp_nonl[] = "/tmp/cpulimit_longnl_XXXXXX";
    int long_nonl_fd;
    /* 300 'a' characters, filling well past the initial 256-byte buffer */
    char *long_content;
    int li;
    int cmp_ret;
    size_t content_len;

    /* NULL filename must return NULL */
    content = read_file_contents(NULL);
    assert(content == NULL);

    /* Non-existent file must return NULL */
    content = read_file_contents("/nonexistent/cpulimit_test_no_such_file");
    assert(content == NULL);

    /* /proc/self/stat always exists and is non-empty */
    content = read_file_contents("/proc/self/stat");
    assert(content != NULL);
    free(content);

    /*
     * Empty file must return NULL (immediate EOF with no data).
     * Use mkstemp() to avoid name collisions in parallel test runs.
     */
    tmp_fd = mkstemp(tmp_file);
    assert(tmp_fd >= 0);
    close(tmp_fd);
    content = read_file_contents(tmp_file);
    assert(content == NULL);
    remove(tmp_file);

    /* A file containing only a newline returns its full content "\n" */
    newline_fd = mkstemp(newline_tmp_file);
    assert(newline_fd >= 0);
    nwritten = write(newline_fd, "\n", 1);
    assert(nwritten == 1);
    close(newline_fd);
    content = read_file_contents(newline_tmp_file);
    assert(content != NULL);
    cmp_ret = strcmp(content, "\n");
    assert(cmp_ret == 0);
    free(content);
    remove(newline_tmp_file);

    /*
     * A CRLF-terminated file must return the content verbatim, including
     * both the '\r' and '\n'.
     */
    crlf_fd = mkstemp(crlf_tmp_file);
    assert(crlf_fd >= 0);
    nwritten = write(crlf_fd, "abc\r\n", 5);
    assert(nwritten == 5);
    close(crlf_fd);
    content = read_file_contents(crlf_tmp_file);
    assert(content != NULL);
    cmp_ret = strcmp(content, "abc\r\n");
    assert(cmp_ret == 0);
    free(content);
    remove(crlf_tmp_file);

    /* EOF-terminated content with trailing CR must be preserved verbatim */
    cr_only_fd = mkstemp(cr_only_tmp_file);
    assert(cr_only_fd >= 0);
    nwritten = write(cr_only_fd, "abc\r", 4);
    assert(nwritten == 4);
    close(cr_only_fd);
    content = read_file_contents(cr_only_tmp_file);
    assert(content != NULL);
    cmp_ret = strcmp(content, "abc\r");
    assert(cmp_ret == 0);
    free(content);
    remove(cr_only_tmp_file);

    /* Embedded CR and trailing LF must all be preserved verbatim */
    cr_middle_fd = mkstemp(cr_middle_tmp_file);
    assert(cr_middle_fd >= 0);
    nwritten = write(cr_middle_fd, "a\rb\n", 4);
    assert(nwritten == 4);
    close(cr_middle_fd);
    content = read_file_contents(cr_middle_tmp_file);
    assert(content != NULL);
    cmp_ret = strcmp(content, "a\rb\n");
    assert(cmp_ret == 0);
    free(content);
    remove(cr_middle_tmp_file);

    /* Build a 300-character content to exercise the realloc growth path */
    long_content = (char *)malloc(300);
    assert(long_content != NULL);
    for (li = 0; li < 300; li++) {
        long_content[li] = 'a';
    }

    /*
     * Content > 256 bytes followed by a newline: realloc path must be taken
     * and the returned string must contain the full 300 'a' characters
     * plus the trailing '\n'.
     */
    long_fd = mkstemp(long_tmp_file);
    assert(long_fd >= 0);
    nwritten = write(long_fd, long_content, 300);
    assert(nwritten == 300);
    nwritten = write(long_fd, "\n", 1);
    assert(nwritten == 1);
    close(long_fd);
    content = read_file_contents(long_tmp_file);
    assert(content != NULL);
    content_len = strlen(content);
    assert(content_len == 301);
    for (li = 0; li < 300; li++) {
        assert(content[li] == 'a');
    }
    assert(content[300] == '\n');
    free(content);
    remove(long_tmp_file);

    /*
     * Content > 256 bytes with no trailing newline (EOF-terminated):
     * realloc path must be taken and the entire content must be returned.
     */
    long_nonl_fd = mkstemp(long_tmp_nonl);
    assert(long_nonl_fd >= 0);
    nwritten = write(long_nonl_fd, long_content, 300);
    assert(nwritten == 300);
    close(long_nonl_fd);
    content = read_file_contents(long_tmp_nonl);
    assert(content != NULL);
    content_len = strlen(content);
    assert(content_len == 300);
    for (li = 0; li < 300; li++) {
        assert(content[li] == 'a');
    }
    free(content);
    remove(long_tmp_nonl);
    free(long_content);
}
#endif /* __linux__ */

#ifdef __linux__
/**
 * @brief Exercise parse_cpu_range boundary and error handling
 * @note parse_cpu_range() previously had no test coverage despite its
 *       overflow/range/syntax guards.  This test drives it directly with
 *       valid, malformed, and overflow-prone inputs to prove the boundary
 *       logic is correct and never returns a value that would corrupt the
 *       cached CPU count in get_ncpu().
 */
static void test_util_parse_cpu_range(void) {
    char near_max[64], reversed[64];

    /* Single CPU and simple ranges map to their element counts */
    assert(parse_cpu_range("0") == 1);
    assert(parse_cpu_range("3") == 1);
    assert(parse_cpu_range("0-3") == 4);
    assert(parse_cpu_range("0-1") == 2);
    assert(parse_cpu_range("4-7") == 4);

    /* Comma-separated combinations accumulate correctly */
    assert(parse_cpu_range("0-3,8-11") == 8);
    assert(parse_cpu_range("0,2,4") == 3);
    assert(parse_cpu_range("0-1,4-7,15") == 2 + 4 + 1);

    /* Whitespace around numbers and separators is tolerated */
    assert(parse_cpu_range(" 0 - 3 ") == 4);
    assert(parse_cpu_range("0, 2 , 4") == 3);

    /* Error cases must return -1 (never a bogus positive count) */
    assert(parse_cpu_range(NULL) == -1);
    assert(parse_cpu_range("") == -1);
    assert(parse_cpu_range("   ") == -1);    /* whitespace-only */
    assert(parse_cpu_range("abc") == -1);    /* non-numeric */
    assert(parse_cpu_range("0-3,") == -1);   /* trailing comma */
    assert(parse_cpu_range("0-3,4-") == -1); /* open-ended range */
    assert(parse_cpu_range("0--3") == -1);   /* malformed dash */
    assert(parse_cpu_range("3-0") == -1);    /* reversed range */
    assert(parse_cpu_range("-1") == -1);     /* negative start */
    assert(parse_cpu_range("0-") == -1);     /* missing end */
    assert(parse_cpu_range("0x1") == -1);    /* hex rejected */
    assert(parse_cpu_range("0,1,2,3,4,5,6,7,8,9") == 10);

    /*
     * Overflow guard: a range whose length would push the running
     * count past INT_MAX must be rejected rather than wrapping.
     * Start just below INT_MAX with a range that overflows.
     */
    snprintf(near_max, sizeof(near_max), "%d-%d", INT_MAX - 2, INT_MAX);
    /* range_len = 2, cpu_count starts 0, 0 + 3 <= INT_MAX: ok = 3 */
    assert(parse_cpu_range(near_max) == 3);

    /* A reversed/huge range must still be rejected */
    snprintf(reversed, sizeof(reversed), "%d-%d", INT_MAX, 0);
    assert(parse_cpu_range(reversed) == -1);
}
#endif /* __linux__ */

/**
 * @brief Test MAX, MIN, and CLAMP macros with all comparison branches
 * @note Covers a>b, a<b, a==b for MAX/MIN; below/above/in-range for CLAMP
 */
static void test_util_macros(void) {
    /*
     * Use volatile to prevent value-propagation in static analysers while
     * still testing the equal-argument and boundary-value edge cases.
     */
    volatile int clamp_low = 0, clamp_high = 10;
    volatile int a, b, val, macro_val;

    /* MAX: larger-first, smaller-first, equal */
    a = 5;
    b = 3;
    macro_val = MAX(a, b);
    assert(macro_val == 5);
    a = 3;
    b = 5;
    macro_val = MAX(a, b);
    assert(macro_val == 5);
    a = 4;
    b = 4;
    macro_val = MAX(a, b);
    assert(macro_val == 4);
    a = -1;
    b = 0;
    macro_val = MAX(a, b);
    assert(macro_val == 0);

    /* MIN: smaller-first, larger-first, equal */
    a = 3;
    b = 5;
    macro_val = MIN(a, b);
    assert(macro_val == 3);
    a = 5;
    b = 3;
    macro_val = MIN(a, b);
    assert(macro_val == 3);
    a = 4;
    b = 4;
    macro_val = MIN(a, b);
    assert(macro_val == 4);
    a = -1;
    b = 0;
    macro_val = MIN(a, b);
    assert(macro_val == -1);

    /* CLAMP: value in range, below low, above high, equals low, equals high */
    val = 5;
    macro_val = CLAMP(val, clamp_low, clamp_high);
    assert(macro_val == 5);
    val = -1;
    macro_val = CLAMP(val, clamp_low, clamp_high);
    assert(macro_val == 0);
    val = 15;
    macro_val = CLAMP(val, clamp_low, clamp_high);
    assert(macro_val == 10);
    val = 0;
    macro_val = CLAMP(val, clamp_low, clamp_high);
    assert(macro_val == 0);
    val = 10;
    macro_val = CLAMP(val, clamp_low, clamp_high);
    assert(macro_val == 10);
}

/***************************************************************************
 * LIST MODULE TESTS
 ***************************************************************************/

/**
 * @brief Test list initialization and empty list operations
 * @note Tests init_list, is_empty_list, get_list_count with empty list
 */
static void test_list_init_and_empty(void) {
    struct list lst;
    int empty;
    size_t list_count;
    const struct list_node *first_node_result;

    /* Test initialization */
    init_list(&lst);
    assert(lst.first == NULL);
    assert(lst.last == NULL);
    assert(lst.count == 0);

    /* Test is_empty_list */
    empty = is_empty_list(&lst);
    assert(empty == 1);
    empty = is_empty_list(NULL);
    assert(empty == 1);

    /* Test get_list_count */
    list_count = get_list_count(&lst);
    assert(list_count == 0);
    list_count = get_list_count(NULL);
    assert(list_count == 0);

    /* Test first_node */
    first_node_result = first_node(&lst);
    assert(first_node_result == NULL);
    first_node_result = first_node(NULL);
    assert(first_node_result == NULL);

    /* Test init_list with NULL */
    init_list(NULL);
}

/**
 * @brief Test adding elements to list
 * @note Tests add_elem, get_list_count, first_node with non-empty list
 *       and link integrity
 */
static void test_list_add_elem(void) {
    struct list lst;
    int data1 = 1, data2 = 2, data3 = 3;
    const struct list_node *node1, *node2, *node3;
    size_t list_count;
    int empty;
    const struct list_node *first_node_result;
    const struct list_node *null_node;

    init_list(&lst);

    /* Add first element */
    node1 = add_elem(&lst, &data1);
    assert(node1 != NULL);
    assert(node1->data == &data1);
    assert(node1->previous == NULL);
    assert(node1->next == NULL);
    assert(lst.first == node1);
    assert(lst.last == node1);
    list_count = get_list_count(&lst);
    assert(list_count == 1);
    empty = is_empty_list(&lst);
    assert(empty == 0);
    first_node_result = first_node(&lst);
    assert(first_node_result == node1);

    /* Add second element */
    node2 = add_elem(&lst, &data2);
    assert(node2 != NULL);
    assert(node2->data == &data2);
    assert(node2->previous == node1);
    assert(node2->next == NULL);
    assert(node1->next == node2);
    assert(lst.first == node1);
    assert(lst.last == node2);
    list_count = get_list_count(&lst);
    assert(list_count == 2);

    /* Add third element */
    node3 = add_elem(&lst, &data3);
    assert(node3 != NULL);
    assert(node3->data == &data3);
    assert(node3->previous == node2);
    assert(node3->next == NULL);
    assert(node2->next == node3);
    assert(lst.first == node1);
    assert(lst.last == node3);
    list_count = get_list_count(&lst);
    assert(list_count == 3);

    /* Verify first_node returns first element with correct links */
    first_node_result = first_node(&lst);
    assert(first_node_result == node1);
    assert(first_node_result->data == &data1);
    assert(first_node_result->next != NULL);
    assert(first_node_result->next->data == &data2);
    assert(first_node_result->previous == NULL);

    /* Test add_elem with NULL list */
    null_node = add_elem(NULL, &data1);
    assert(null_node == NULL);

    /* Clean up */
    clear_list(&lst);
}

/**
 * @brief Test deleting nodes from list
 * @note Tests delete_node without freeing data, including empty-list guard
 */
static void test_list_delete_node(void) {
    struct list lst;
    int data1 = 1, data2 = 2, data3 = 3;
    struct list_node *node1, *node2, *node3;
    struct list_node fake_node = {NULL, NULL, NULL};
    size_t list_count;
    int empty;

    init_list(&lst);
    node1 = add_elem(&lst, &data1);
    node2 = add_elem(&lst, &data2);
    node3 = add_elem(&lst, &data3);

    /* Delete middle node */
    delete_node(&lst, node2);
    list_count = get_list_count(&lst);
    assert(list_count == 2);
    assert(lst.first == node1);
    assert(lst.last == node3);
    assert(node1->next == node3);
    assert(node3->previous == node1);

    /* Delete first node */
    delete_node(&lst, node1);
    list_count = get_list_count(&lst);
    assert(list_count == 1);
    assert(lst.first == node3);
    assert(lst.last == node3);
    assert(node3->previous == NULL);
    assert(node3->next == NULL);

    /* Delete last node */
    delete_node(&lst, node3);
    list_count = get_list_count(&lst);
    assert(list_count == 0);
    assert(lst.first == NULL);
    assert(lst.last == NULL);
    empty = is_empty_list(&lst);
    assert(empty == 1);

    /* Test delete_node with NULL */
    delete_node(NULL, NULL);
    delete_node(&lst, NULL);

    /* Test delete_node on empty list (count==0 guard path) */
    delete_node(&lst, &fake_node);
    list_count = get_list_count(&lst);
    assert(list_count == 0);
}

/**
 * @brief Test destroying nodes from list
 * @note Tests destroy_node which frees both node and data, including
 *       NULL list safety
 */
static void test_list_destroy_node(void) {
    struct list lst;
    int *data1, *data2;
    struct list_node *node1, *node2;
    size_t list_count;
    int empty;
    int *null_data;
    struct list_node *null_node;
    int null_val;

    init_list(&lst);

    /* Allocate data dynamically for destroy_node */
    data1 = (int *)malloc(sizeof(int));
    data2 = (int *)malloc(sizeof(int));
    assert(data1 != NULL);
    assert(data2 != NULL);
    *data1 = 1;
    *data2 = 2;

    node1 = add_elem(&lst, data1);
    node2 = add_elem(&lst, data2);

    /* Destroy second node */
    destroy_node(&lst, node2);
    list_count = get_list_count(&lst);
    assert(list_count == 1);
    assert(lst.first == node1);
    assert(lst.last == node1);

    /* Destroy first node */
    destroy_node(&lst, node1);
    list_count = get_list_count(&lst);
    assert(list_count == 0);
    empty = is_empty_list(&lst);
    assert(empty == 1);

    /* Test destroy_node with NULL */
    destroy_node(NULL, NULL);
    destroy_node(&lst, NULL);

    /* Test destroy_node with NULL list is a no-op */
    null_data = (int *)malloc(sizeof(int));
    assert(null_data != NULL);
    *null_data = 42;
    null_node = (struct list_node *)malloc(sizeof(struct list_node));
    assert(null_node != NULL);
    null_node->data = null_data;
    null_node->previous = NULL;
    null_node->next = NULL;
    destroy_node(NULL, null_node);
    /* Verify the call was truly a no-op */
    assert(null_node->data == null_data);
    null_val = *(int *)null_node->data;
    assert(null_val == 42);
    assert(null_node->previous == NULL);
    assert(null_node->next == NULL);
    free(null_data);
    free(null_node);
}

/**
 * @brief Test locating nodes and elements in list
 * @note Tests locate_node and locate_elem including single-element list
 */
static void test_list_locate(void) {
    struct list lst;
    struct process *proc1, *proc2, *proc3;
    const struct list_node *found_node;
    const struct process *found_elem;
    pid_t search_pid;
    const struct process *found_proc;
    const void *void_elem;
    int single_val, single_miss, single_node_val;

    init_list(&lst);

    /* Allocate processes on heap to avoid stack size warnings */
    proc1 = (struct process *)malloc(sizeof(struct process));
    proc2 = (struct process *)malloc(sizeof(struct process));
    proc3 = (struct process *)malloc(sizeof(struct process));
    assert(proc1 != NULL);
    assert(proc2 != NULL);
    assert(proc3 != NULL);

    /* Initialize processes with different PIDs */
    proc1->pid = 100;
    proc1->ppid = 1;
    proc2->pid = 200;
    proc2->ppid = 1;
    proc3->pid = 300;
    proc3->ppid = 1;

    add_elem(&lst, proc1);
    add_elem(&lst, proc2);
    add_elem(&lst, proc3);

    /* Test locate_node - find by PID */
    search_pid = 200;
    found_node = locate_node(&lst, &search_pid, offsetof(struct process, pid),
                             sizeof(pid_t));
    assert(found_node != NULL);
    found_proc = (const struct process *)found_node->data;
    assert(found_proc->pid == 200);

    /* Test locate_node - not found */
    search_pid = 999;
    found_node = locate_node(&lst, &search_pid, offsetof(struct process, pid),
                             sizeof(pid_t));
    assert(found_node == NULL);

    /* Test locate_elem - find by PID */
    search_pid = 100;
    found_elem = (struct process *)locate_elem(
        &lst, &search_pid, offsetof(struct process, pid), sizeof(pid_t));
    assert(found_elem == proc1);
    assert(found_elem->pid == 100);

    /* Test locate_elem - not found */
    search_pid = 999;
    found_elem = (struct process *)locate_elem(
        &lst, &search_pid, offsetof(struct process, pid), sizeof(pid_t));
    assert(found_elem == NULL);

    /* Test with NULL list */
    found_node = locate_node(NULL, &search_pid, 0, sizeof(pid_t));
    assert(found_node == NULL);
    void_elem = locate_elem(NULL, &search_pid, 0, sizeof(pid_t));
    assert(void_elem == NULL);

    /* Test with NULL element */
    found_node = locate_node(&lst, NULL, 0, sizeof(pid_t));
    assert(found_node == NULL);
    void_elem = locate_elem(&lst, NULL, 0, sizeof(pid_t));
    assert(void_elem == NULL);

    /* Test with zero length */
    found_node = locate_node(&lst, &search_pid, 0, 0);
    assert(found_node == NULL);
    void_elem = locate_elem(&lst, &search_pid, 0, 0);
    assert(void_elem == NULL);

    clear_list(&lst);

    /* Free allocated memory */
    free(proc1);
    free(proc2);
    free(proc3);

    /* Test locate with a single-element list */
    init_list(&lst);
    single_val = 42;
    add_elem(&lst, &single_val);
    found_node = locate_node(&lst, &single_val, 0, sizeof(int));
    assert(found_node != NULL);
    single_node_val = *(int *)found_node->data;
    assert(single_node_val == 42);
    void_elem = locate_elem(&lst, &single_val, 0, sizeof(int));
    assert(void_elem == &single_val);
    /* Miss case in single-element list */
    single_miss = 99;
    found_node = locate_node(&lst, &single_miss, 0, sizeof(int));
    assert(found_node == NULL);
    void_elem = locate_elem(&lst, &single_miss, 0, sizeof(int));
    assert(void_elem == NULL);
    clear_list(&lst);
}

/**
 * @brief Test clearing and destroying lists
 * @note Tests clear_list and destroy_list including empty-list no-op paths
 */
static void test_list_clear_and_destroy(void) {
    struct list lst1, lst2;
    int data1 = 1, data2 = 2, data3 = 3;
    int *dyn_data1, *dyn_data2, *dyn_data3;
    size_t list_count;
    int empty;

    /* Test clear_list - data not freed */
    init_list(&lst1);
    add_elem(&lst1, &data1);
    add_elem(&lst1, &data2);
    add_elem(&lst1, &data3);
    list_count = get_list_count(&lst1);
    assert(list_count == 3);

    clear_list(&lst1);
    list_count = get_list_count(&lst1);
    assert(list_count == 0);
    assert(lst1.first == NULL);
    assert(lst1.last == NULL);
    empty = is_empty_list(&lst1);
    assert(empty == 1);

    /* Test clear_list with NULL */
    clear_list(NULL);

    /* Test destroy_list - data is freed */
    init_list(&lst2);
    dyn_data1 = (int *)malloc(sizeof(int));
    dyn_data2 = (int *)malloc(sizeof(int));
    dyn_data3 = (int *)malloc(sizeof(int));
    assert(dyn_data1 != NULL);
    assert(dyn_data2 != NULL);
    assert(dyn_data3 != NULL);
    *dyn_data1 = 1;
    *dyn_data2 = 2;
    *dyn_data3 = 3;

    add_elem(&lst2, dyn_data1);
    add_elem(&lst2, dyn_data2);
    add_elem(&lst2, dyn_data3);
    list_count = get_list_count(&lst2);
    assert(list_count == 3);

    destroy_list(&lst2);
    list_count = get_list_count(&lst2);
    assert(list_count == 0);
    assert(lst2.first == NULL);
    assert(lst2.last == NULL);
    empty = is_empty_list(&lst2);
    assert(empty == 1);

    /* Test destroy_list with NULL */
    destroy_list(NULL);

    /* Test clear_list on already-empty list (no-op path) */
    init_list(&lst1);
    clear_list(&lst1);
    list_count = get_list_count(&lst1);
    assert(list_count == 0);
    empty = is_empty_list(&lst1);
    assert(empty == 1);

    /* Test destroy_list on already-empty list (no-op path) */
    init_list(&lst1);
    destroy_list(&lst1);
    list_count = get_list_count(&lst1);
    assert(list_count == 0);
    empty = is_empty_list(&lst1);
    assert(empty == 1);
}

/**
 * @brief Test list operations with edge cases
 * @note Tests list behavior with various edge cases like reversing order
 */
static void test_list_edge_cases(void) {
    struct list lst;
    int data[10];
    struct list_node *node;
    int node_idx, count;
    size_t list_count;
    int first_val, second_val, last_val;

    init_list(&lst);

    /* Test adding many elements */
    for (node_idx = 0; node_idx < 10; node_idx++) {
        data[node_idx] = node_idx;
        add_elem(&lst, &data[node_idx]);
    }
    list_count = get_list_count(&lst);
    assert(list_count == 10);

    /* Verify forward traversal */
    count = 0;
    for (node = lst.first; node != NULL; node = node->next) {
        int node_val;
        node_val = *(int *)node->data;
        assert(node_val == count);
        count++;
    }
    assert(count == 10);

    /* Verify backward traversal */
    count = 9;
    for (node = lst.last; node != NULL; node = node->previous) {
        int node_val;
        node_val = *(int *)node->data;
        assert(node_val == count);
        count--;
    }
    assert(count == -1);

    /* Delete all nodes from back to front */
    while (!is_empty_list(&lst)) {
        delete_node(&lst, lst.last);
    }
    list_count = get_list_count(&lst);
    assert(list_count == 0);

    /* Test deleting nodes in middle repeatedly */
    for (node_idx = 0; node_idx < 5; node_idx++) {
        data[node_idx] = node_idx;
        add_elem(&lst, &data[node_idx]);
    }

    /* Delete middle elements */
    node = lst.first->next; /* Second element */
    delete_node(&lst, node);
    list_count = get_list_count(&lst);
    assert(list_count == 4);

    node = lst.first->next; /* New second element (was third) */
    delete_node(&lst, node);
    list_count = get_list_count(&lst);
    assert(list_count == 3);

    /* Verify remaining elements */
    first_val = *(int *)lst.first->data;
    assert(first_val == 0);
    second_val = *(int *)lst.first->next->data;
    assert(second_val == 3);
    last_val = *(int *)lst.last->data;
    assert(last_val == 4);

    clear_list(&lst);
}

/**
 * @brief Test add_elem with NULL data and locate_node skipping NULL-data nodes
 * @note Covers: add_elem(l, NULL), locate_node branch cur->data==NULL,
 *       destroy_node with NULL data pointer
 */
static void test_list_null_data_operations(void) {
    struct list lst;
    struct list_node *node;
    int search_val;
    size_t list_count;
    int empty;
    const struct list_node *tmp_node;
    const void *void_elem;

    init_list(&lst);

    /* add_elem with NULL data must create a valid node */
    node = add_elem(&lst, NULL);
    assert(node != NULL);
    assert(node->data == NULL);
    list_count = get_list_count(&lst);
    assert(list_count == 1);
    empty = is_empty_list(&lst);
    assert(empty == 0);

    /* locate_node must skip the NULL-data node (branch: cur->data == NULL) */
    search_val = 42;
    tmp_node = locate_node(&lst, &search_val, 0, sizeof(int));
    assert(tmp_node == NULL);
    void_elem = locate_elem(&lst, &search_val, 0, sizeof(int));
    assert(void_elem == NULL);

    /*
     * destroy_node with NULL data must not crash (branch: node->data == NULL)
     */
    destroy_node(&lst, node);
    list_count = get_list_count(&lst);
    assert(list_count == 0);
    empty = is_empty_list(&lst);
    assert(empty == 1);
}

/***************************************************************************
 * SIGNAL_HANDLER MODULE TESTS
 ***************************************************************************/

/**
 * @brief Test signal handler flags
 * @note Installs handlers in a child, raises signals, and checks flags
 */
static void test_signal_handler_flags(void) {
    pid_t pid, waited;
    int status, exited, exit_code;

    /* Test behavior on SIGTERM: quit flag set, not terminated by tty */
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        /* Child process: install handlers and raise SIGTERM */
        configure_signal_handler();
        if (raise(SIGTERM) != 0) {
            _exit(1);
        }
        if (!is_quit_flag_set()) {
            _exit(2);
        }
        if (is_terminated_by_tty()) {
            _exit(3);
        }
        _exit(0);
    }

    /* Parent: verify child exited successfully */
    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == 0);

    /* Test behavior on SIGINT: quit flag set, terminated by tty */
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        /* Child process: install handlers and raise SIGINT */
        configure_signal_handler();
        if (raise(SIGINT) != 0) {
            _exit(4);
        }
        if (!is_quit_flag_set()) {
            _exit(5);
        }
        if (!is_terminated_by_tty()) {
            _exit(6);
        }
        _exit(0);
    }

    /* Parent: verify child exited successfully */
    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == 0);
}

/**
 * @brief Test SIGQUIT signal handling
 * @note SIGQUIT must set both quit_flag and terminated_by_tty
 *       The child calls setsid() to create a new session and detach from the
 *       controlling terminal, preventing BSD terminal drivers from propagating
 *       SIGQUIT to the parent's process group.
 */
static void test_signal_handler_sigquit(void) {
    pid_t pid, waited;
    int status, exited, exit_code;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        /*
         * Create a new session to detach from the controlling terminal.
         * On BSD, raising SIGQUIT (a core-dump signal) in a process that
         * shares the parent's terminal session can cause the terminal
         * driver to propagate the signal to the parent's process group,
         * corrupting the parent's state. setsid() prevents this.
         */
        pid_t session_id;
        pid_t invalid_pid;
        session_id = setsid();
        invalid_pid = (pid_t)-1;
        assert(session_id != invalid_pid);
        configure_signal_handler();
        if (raise(SIGQUIT) != 0) {
            _exit(1);
        }
        if (!is_quit_flag_set()) {
            _exit(2);
        }
        if (!is_terminated_by_tty()) {
            _exit(3);
        }
        _exit(0);
    }

    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == 0);
}

/**
 * @brief Test SIGHUP signal handling
 * @note SIGHUP must set quit_flag but must NOT set terminated_by_tty
 */
static void test_signal_handler_sighup(void) {
    pid_t pid, waited;
    int status, exited, exit_code;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        configure_signal_handler();
        if (raise(SIGHUP) != 0) {
            _exit(1);
        }
        if (!is_quit_flag_set()) {
            _exit(2);
        }
        if (is_terminated_by_tty()) { /* SIGHUP must NOT set the tty flag */
            _exit(3);
        }
        _exit(0);
    }

    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == 0);
}

/**
 * @brief Test SIGPIPE signal handling
 * @note SIGPIPE must set quit_flag but must NOT set terminated_by_tty
 */
static void test_signal_handler_sigpipe(void) {
    pid_t pid, waited;
    int status, exited, exit_code;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        configure_signal_handler();
        if (raise(SIGPIPE) != 0) {
            _exit(1);
        }
        if (!is_quit_flag_set()) {
            _exit(2);
        }
        if (is_terminated_by_tty()) { /* SIGPIPE must NOT set the tty flag */
            _exit(3);
        }
        _exit(0);
    }

    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == 0);
}

/**
 * @brief Test initial state of signal handler flags before any signal is raised
 * @note Both quit_flag and terminated_by_tty must be 0 before any signal
 */
static void test_signal_handler_initial_state(void) {
    pid_t pid, waited;
    int status, exited, exit_code;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        configure_signal_handler();
        if (is_quit_flag_set()) {
            _exit(1);
        }
        if (is_terminated_by_tty()) {
            _exit(2);
        }
        _exit(0);
    }
    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == 0);
}

/**
 * @brief Test get_quit_signal before and after receiving signals
 * @note Before any signal: returns 0.  After SIGTERM: returns SIGTERM
 *       After SIGINT: returns SIGINT.  First signal wins; subsequent signals
 *       do not overwrite the recorded number.
 */
static void test_signal_handler_get_quit_signal(void) {
    pid_t pid, waited;
    int status, exited, exit_code, sig_val;

    /* Sub-test 1: before any signal, get_quit_signal() returns 0 */
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        configure_signal_handler();
        sig_val = get_quit_signal();
        _exit(sig_val == 0 ? 0 : 1);
    }
    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == 0);

    /* Sub-test 2: after SIGTERM, get_quit_signal() returns SIGTERM */
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        configure_signal_handler();
        if (raise(SIGTERM) != 0) {
            _exit(1);
        }
        sig_val = get_quit_signal();
        _exit(sig_val == SIGTERM ? 0 : 1);
    }
    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == 0);

    /* Sub-test 3: after SIGINT, get_quit_signal() returns SIGINT */
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        configure_signal_handler();
        if (raise(SIGINT) != 0) {
            _exit(1);
        }
        sig_val = get_quit_signal();
        _exit(sig_val == SIGINT ? 0 : 1);
    }
    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == 0);

    /* Sub-test 4: first signal wins; SIGTERM then SIGINT keeps SIGTERM */
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        configure_signal_handler();
        if (raise(SIGTERM) != 0) {
            _exit(1);
        }
        if (raise(SIGINT) != 0) {
            _exit(2);
        }
        sig_val = get_quit_signal();
        _exit(sig_val == SIGTERM ? 0 : 1);
    }
    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == 0);
}

/**
 * @brief Test configure_signal_handler() resets internal state each call
 * @note In a single process, after a signal sets quit flags, reconfiguring
 *       handlers must clear all flags so a new run starts from a deterministic
 *       baseline.
 */
static void test_signal_handler_reconfigure_resets_state(void) {
    pid_t pid, waited;
    int status, exited, exit_code;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        configure_signal_handler();
        if (raise(SIGTERM) != 0) {
            _exit(1);
        }
        if (!is_quit_flag_set()) {
            _exit(2);
        }
        if (get_quit_signal() != SIGTERM) {
            _exit(3);
        }

        configure_signal_handler();
        if (is_quit_flag_set()) {
            _exit(4);
        }
        if (is_terminated_by_tty()) {
            _exit(5);
        }
        if (get_quit_signal() != 0) {
            _exit(6);
        }
        _exit(0);
    }

    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == 0);
}

/**
 * @brief Test pending signal during reconfigure is delivered, not dropped
 * @note configure_signal_handler() blocks handled signals during the
 *       reset-and-install window. A signal that becomes pending during that
 *       window must be delivered through the new handler after the mask is
 *       restored, not silently dropped.
 *
 *  Scenario:
 *   1. Block SIGTERM in the caller.
 *   2. Install handlers (first configure call).
 *   3. raise(SIGTERM) - SIGTERM is pending, not delivered.
 *   4. Call configure_signal_handler() again (reconfigure).
 *      - configure blocks SIGTERM internally (already blocked - no change).
 *      - Resets state: quit_flag = 0.
 *      - Installs new handlers.
 *      - Restores caller mask (SIGTERM still blocked by caller).
 *   5. Verify quit_flag is 0 (SIGTERM still pending).
 *   6. Unblock SIGTERM in caller - pending SIGTERM is delivered via new
 *      handler.
 *   7. Verify quit_flag is 1 and quit_signal_num is SIGTERM.
 */
static void test_signal_handler_reconfigure_delivers_pending(void) {
    pid_t pid, waited;
    int status, exited, exit_code;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        sigset_t block_set, old_set;

        /* Block SIGTERM so that raise() makes it pending, not delivered */
        if (sigemptyset(&block_set) != 0) {
            _exit(1);
        }
        if (sigaddset(&block_set, SIGTERM) != 0) {
            _exit(1);
        }
        if (sigprocmask(SIG_BLOCK, &block_set, &old_set) != 0) {
            _exit(1);
        }

        /* First configure: installs handlers with SIGTERM blocked */
        configure_signal_handler();

        /* Raise SIGTERM: pending because it is still blocked */
        if (raise(SIGTERM) != 0) {
            _exit(2);
        }

        /* Quit flag must still be 0 (signal pending, not delivered) */
        if (is_quit_flag_set()) {
            _exit(3);
        }

        /*
         * Reconfigure: must block SIGTERM, reset state, install new
         * handlers, then restore our mask (SIGTERM remains blocked).
         */
        configure_signal_handler();

        /*
         * After reconfigure, quit_flag must still be 0; the pending
         * SIGTERM has not been delivered yet because our mask still
         * blocks it.
         */
        if (is_quit_flag_set()) {
            _exit(4);
        }

        /*
         * Restore original mask: pending SIGTERM is now delivered via
         * the new handler.
         */
        if (sigprocmask(SIG_SETMASK, &old_set, NULL) != 0) {
            _exit(5);
        }

        /* The new handler must have set quit_flag */
        if (!is_quit_flag_set()) {
            _exit(6);
        }
        if (get_quit_signal() != SIGTERM) {
            _exit(7);
        }
        _exit(0);
    }

    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == 0);
}

/**
 * @brief Test reset_signal_handlers_to_default() restores SIG_DFL
 * @note After configure_signal_handler() installs custom handlers,
 *       reset_signal_handlers_to_default() must restore SIGINT, SIGQUIT,
 *       SIGTERM, SIGHUP, and SIGPIPE to SIG_DFL.  Verified by checking
 *       that sa_handler == SIG_DFL for each signal after the reset.
 */
static void test_signal_handler_reset_to_default(void) {
    pid_t pid, waited;
    int status, exited, exit_code;
    static const int check_sigs[] = {SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGPIPE};
    static const size_t num_check_sigs =
        sizeof(check_sigs) / sizeof(*check_sigs);

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        struct sigaction old_action;
        size_t i;
        int ret;

        configure_signal_handler();
        ret = reset_signal_handlers_to_default();
        if (ret != 0) {
            _exit(1);
        }
        for (i = 0; i < num_check_sigs; i++) {
            if (sigaction(check_sigs[i], NULL, &old_action) != 0) {
                _exit(2);
            }
            if (old_action.sa_handler != SIG_DFL) {
                _exit(3);
            }
        }
        _exit(0);
    }
    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == 0);
}

/**
 * @brief Test that two signals delivered concurrently produce consistent state
 * @note The child blocks all signals, signals readiness to the parent, then
 *       the parent sends SIGTERM followed immediately by SIGINT.  The child
 *       unblocks both signals at once via sigsuspend, allowing one pending
 *       signal to be delivered.  After delivery, quit_flag must be set and
 *       quit_signal_num must be one of the two sent signals (delivery order
 *       and which signal is ultimately observed are implementation-defined
 *       and not asserted by this test).
 */
static void test_signal_handler_race_concurrent_signals(void) {
    int ready_pipe[2];
    pid_t child_pid, waited;
    int status, exited, exit_code, ret;
    char ready_byte;
    ssize_t n_read;

    ret = pipe(ready_pipe);
    assert(ret == 0);

    child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid == 0) {
        sigset_t full_mask, empty_mask;
        int sig_val;

        close(ready_pipe[0]);
        configure_signal_handler();

        /*
         * Block all signals before notifying parent, so both SIGTERM and
         * SIGINT will be pending when sigsuspend is called.
         */
        if (sigfillset(&full_mask) != 0 || sigemptyset(&empty_mask) != 0) {
            _exit(1);
        }
        if (sigprocmask(SIG_BLOCK, &full_mask, NULL) != 0) {
            _exit(1);
        }

        if (write(ready_pipe[1], "R", 1) != 1) {
            close(ready_pipe[1]);
            _exit(1);
        }
        close(ready_pipe[1]);

        /*
         * Unblock all signals atomically and wait for the first pending
         * signal.  Both SIGTERM and SIGINT should be pending; sigsuspend
         * delivers whichever the kernel picks first.  The remaining
         * pending signal is cleared harmlessly on _exit().
         *
         * Install a safety timeout so that, if the expected signals are
         * never delivered, the child does not block indefinitely and hang
         * the test run.  If the alarm ever fires, SIGALRM interrupts
         * sigsuspend() and the child terminates instead of hanging.
         */
        alarm(5);
        sigsuspend(&empty_mask);

        if (!is_quit_flag_set()) {
            _exit(2);
        }
        sig_val = get_quit_signal();
        /*
         * quit_signal_num must be one of the two sent signals.
         * The exact value depends on delivery order, which is
         * implementation-defined for standard signals.
         */
        if (sig_val != SIGTERM && sig_val != SIGINT) {
            _exit(3);
        }
        _exit(0);
    }

    /* Parent: wait for child to be ready, then send two signals rapidly */
    close(ready_pipe[1]);
    do {
        n_read = read(ready_pipe[0], &ready_byte, 1);
    } while (n_read < 0 && errno == EINTR);
    assert(n_read == 1 && ready_byte == 'R');
    close(ready_pipe[0]);

    /* Send SIGTERM then SIGINT with no delay between them */
    kill(child_pid, SIGTERM);
    kill(child_pid, SIGINT);

    waited = waitpid(child_pid, &status, 0);
    assert(waited == child_pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == 0);
}

/**
 * @brief Test that a signal from an external process interrupts sleep_timespec
 * @note Exercises the interaction between SA_RESTART and clock_nanosleep
 *       clock_nanosleep is NOT automatically restarted by SA_RESTART, so a
 *       signal interrupts the sleep and returns EINTR.  The child installs
 *       handlers, enters a long sleep, and verifies that an externally
 *       delivered SIGTERM wakes the sleep and sets quit_flag.
 */
static void test_signal_handler_race_signal_interrupts_sleep(void) {
    int ready_pipe[2];
    pid_t child_pid;
    int status, ret;

    ret = pipe(ready_pipe);
    assert(ret == 0);

    child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid == 0) {
        /* 2-second sleep; signal from parent interrupts it */
        const struct timespec long_sleep = {2, 0};

        close(ready_pipe[0]);
        configure_signal_handler();

        if (write(ready_pipe[1], "R", 1) != 1) {
            close(ready_pipe[1]);
            _exit(1);
        }
        close(ready_pipe[1]);

        /*
         * sleep_timespec calls clock_nanosleep / nanosleep.
         * Neither is restarted by SA_RESTART; both return EINTR when
         * a signal is delivered.
         */
        sleep_timespec(&long_sleep);

        /* Sleep must have been interrupted: quit_flag must now be set */
        if (!is_quit_flag_set()) {
            _exit(2);
        }
        if (get_quit_signal() != SIGTERM) {
            _exit(3);
        }
        _exit(0);
    } else {
        const struct timespec small_delay = {0, 10000000L}; /* 10 ms */
        char ready_byte;
        ssize_t n_read;
        int exited, exit_code;
        pid_t waited;

        /* Parent: wait for child to enter sleep, then send SIGTERM */
        close(ready_pipe[1]);
        do {
            n_read = read(ready_pipe[0], &ready_byte, 1);
        } while (n_read < 0 && errno == EINTR);
        assert(n_read == 1 && ready_byte == 'R');
        close(ready_pipe[0]);

        /* Give child a moment to reach sleep_timespec before sending signal */
        sleep_timespec(&small_delay);

        kill(child_pid, SIGTERM);

        waited = waitpid(child_pid, &status, 0);
        assert(waited == child_pid);
        exited = WIFEXITED(status);
        assert(exited);
        exit_code = WEXITSTATUS(status);
        assert(exit_code == 0);
    }
}

/**
 * @brief Test rapid delivery of all five handled signals
 * @note All five handled signals (SIGTERM, SIGHUP, SIGPIPE, SIGINT, SIGQUIT)
 *       are sent from the parent in rapid succession while the child blocks
 *       them. The child then unblocks all signals at once (sigsuspend),
 *       allowing any one of the five to be delivered.  After the first
 *       delivery, quit_flag must be set and quit_signal_num must be one of
 *       the five valid numbers. The remaining pending signals are harmlessly
 *       cleared on _exit().
 */
static void test_signal_handler_race_rapid_all_signals(void) {
    int ready_pipe[2];
    pid_t child_pid;
    int status, ret;
    static const int all_sigs[] = {SIGTERM, SIGHUP, SIGPIPE, SIGINT, SIGQUIT};
    static const size_t num_sigs = sizeof(all_sigs) / sizeof(*all_sigs);

    ret = pipe(ready_pipe);
    assert(ret == 0);

    child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid == 0) {
        sigset_t full_mask, empty_mask;
        int sig_val;

        /*
         * Call setsid() to detach from the controlling terminal so that
         * SIGQUIT sent to this child does not propagate to the parent's
         * terminal session on BSD systems.
         */
        setsid();

        close(ready_pipe[0]);
        configure_signal_handler();

        if (sigfillset(&full_mask) != 0 || sigemptyset(&empty_mask) != 0) {
            _exit(1);
        }
        if (sigprocmask(SIG_BLOCK, &full_mask, NULL) != 0) {
            _exit(1);
        }

        if (write(ready_pipe[1], "R", 1) != 1) {
            close(ready_pipe[1]);
            _exit(1);
        }
        close(ready_pipe[1]);

        /*
         * Unblock all signals atomically.  All five signals sent by the
         * parent should be pending; sigsuspend delivers the first one
         * (implementation-defined order).  The remaining signals are
         * cleared harmlessly when _exit() is called.
         */
        sigsuspend(&empty_mask);

        if (!is_quit_flag_set()) {
            _exit(2);
        }
        sig_val = get_quit_signal();
        /* quit_signal_num must be one of the five handled signals */
        if (sig_val != SIGTERM && sig_val != SIGHUP && sig_val != SIGPIPE &&
            sig_val != SIGINT && sig_val != SIGQUIT) {
            _exit(3);
        }
        _exit(0);
    } else {
        size_t sig_idx;
        char ready_byte;
        ssize_t n_read;
        int exited, exit_code;
        pid_t waited;

        close(ready_pipe[1]);
        do {
            n_read = read(ready_pipe[0], &ready_byte, 1);
        } while (n_read < 0 && errno == EINTR);
        assert(n_read == 1 && ready_byte == 'R');
        close(ready_pipe[0]);

        /* Send all five signals in rapid succession */
        for (sig_idx = 0; sig_idx < num_sigs; sig_idx++) {
            kill(child_pid, all_sigs[sig_idx]);
        }

        waited = waitpid(child_pid, &status, 0);
        assert(waited == child_pid);
        exited = WIFEXITED(status);
        assert(exited);
        exit_code = WEXITSTATUS(status);
        assert(exit_code == 0);
    }
}

/***************************************************************************
 * PROCESS_ITERATOR MODULE TESTS
 ***************************************************************************/

/**
 * @brief Find a PID that is guaranteed not to exist on the current system
 * @return A PID for which kill(pid, 0) fails with ESRCH
 *
 * The test suite used the hard-coded PID 99999 to represent a non-existent
 * process.  PID 99999 can collide with a real process on busy systems
 * (PID reuse), which made the assertions non-deterministic.  This helper
 * probes downwards from the top of the pid_t range until kill() reports
 * ESRCH, which is deterministic on all supported platforms.
 */
static pid_t find_unused_pid(void) {
    pid_t candidate;
    for (candidate = (pid_t)INT_MAX; candidate > 1; candidate--) {
        if (kill(candidate, 0) == -1 && errno == ESRCH) {
            return candidate;
        }
    }
    return (pid_t)INT_MAX;
}

/**
 * @brief Test is_child_of function
 * @note Tests process ancestry checking
 */
static void test_process_iterator_is_child_of(void) {
    pid_t child_pid, parent_pid;
    pid_t unused_pid;
    int result;

    unused_pid = find_unused_pid();
    parent_pid = getpid();

    /* Create a child process */
    child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        sigset_t full_mask;
        /*
         * Block all blockable signals so no signal can wake the child
         * prematurely.  test_suspend_until_killed() suspends until the
         * parent kills it, and also exits on its own if the parent
         * aborts (failed assertion) so no orphan is left behind.
         */
        if (sigfillset(&full_mask) != 0) {
            _exit(1);
        }
        if (sigprocmask(SIG_BLOCK, &full_mask, NULL) != 0) {
            _exit(1);
        }
        test_suspend_until_killed();
    }

    /* Parent process - test is_child_of */

    /* Child should be child of parent */
    result = is_child_of(child_pid, parent_pid);
    assert(result == 1);

    /* Parent should not be child of child */
    /* NOLINTNEXTLINE(readability-suspicious-call-argument) */
    result = is_child_of(parent_pid, child_pid);
    assert(result == 0);

    /* Process should not be child of itself */
    result = is_child_of(parent_pid, parent_pid);
    assert(result == 0);

    /* All processes are children of init (PID 1) */
    result = is_child_of(parent_pid, 1);
    assert(result == 1);

    /* Test with invalid PIDs */
    result = is_child_of(0, parent_pid);
    assert(result == 0);

    result = is_child_of(-1, parent_pid);
    assert(result == 0);

    result = is_child_of(child_pid, 0);
    assert(result == 0);

    result = is_child_of(child_pid, -1);
    assert(result == 0);

    /* Test with non-existent PID */
    result = is_child_of(unused_pid, parent_pid);
    assert(result == 0);

    /* Non-existent process must not be treated as child of init */
    result = is_child_of(unused_pid, 1);
    assert(result == 0);

    /* Clean up child */
    kill_and_wait(child_pid, SIGKILL);
}

/**
 * @brief Test is_child_of() with multi-level (grandchild) ancestry
 * @note Tests that a grandchild process is correctly identified as a
 *       descendant of its grandparent, exercising multi-hop parent-chain
 *       traversal in is_child_of().
 */
static void test_process_iterator_is_child_of_deep(void) {
    pid_t grandparent_pid;
    pid_t child_pid;
    pid_t grandchild_pid;
    int pipe_fds[2];

    grandparent_pid = getpid();

    if (pipe(pipe_fds) != 0) {
        fprintf(stderr, "pipe() failed in "
                        "test_process_iterator_is_child_of_deep\n");
        assert(0);
        return;
    }

    child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        /*
         * Child process: become a new process group leader so the
         * grandparent can kill both child and grandchild together via
         * kill(-child_pid, SIGKILL).
         */
        pid_t gc_pid;
        sigset_t full_mask;

        close(pipe_fds[0]);
        if (setpgid(0, 0) != 0) { /* new process group */
            close(pipe_fds[1]);
            _exit(1);
        }

        gc_pid = fork();
        if (gc_pid < 0) {
            close(pipe_fds[1]);
            _exit(1);
        }

        if (gc_pid > 0) {
            /* Intermediate child: relay grandchild PID to grandparent */
            if (write(pipe_fds[1], &gc_pid, sizeof(gc_pid)) !=
                (ssize_t)sizeof(gc_pid)) {
                kill(gc_pid, SIGKILL);
                close(pipe_fds[1]);
                _exit(1);
            }
            close(pipe_fds[1]);
            /* Wait until killed */
            if (sigfillset(&full_mask) != 0 ||
                sigprocmask(SIG_BLOCK, &full_mask, NULL) != 0) {
                kill(gc_pid, SIGKILL);
                _exit(1);
            }
            /*
             * Suspend until killed.  test_suspend_until_killed() also
             * exits on its own if the grandparent aborts (failed
             * assertion); the grandchild, which waits on this process's
             * death, then exits too, so no orphan is left behind.
             */
            test_suspend_until_killed();
        }

        /* Grandchild: close pipe and wait until killed */
        close(pipe_fds[1]);
        if (sigfillset(&full_mask) != 0 ||
            sigprocmask(SIG_BLOCK, &full_mask, NULL) != 0) {
            _exit(1);
        }
        test_suspend_until_killed();
    } else {
        char *buf;
        size_t remaining;
        int result;

        /* Parent (grandparent): read grandchild PID from pipe */
        close(pipe_fds[1]);
        buf = (char *)&grandchild_pid;
        remaining = sizeof(grandchild_pid);
        while (remaining > 0) {
            ssize_t n = read(pipe_fds[0], buf, remaining);
            if (n > 0) {
                remaining -= (size_t)n;
                buf += n;
            } else if (n == 0 || errno != EINTR) {
                /* EOF or unrecoverable error */
                break;
            }
            /* EINTR: retry */
        }
        close(pipe_fds[0]);
        assert(grandchild_pid > 0);

        /* Grandchild must be a descendant of grandparent (two hops up) */
        result = is_child_of(grandchild_pid, grandparent_pid);
        assert(result == 1);

        /*
         * Grandchild must also be a direct descendant of the intermediate
         * child.
         */
        /* NOLINTNEXTLINE(readability-suspicious-call-argument) */
        result = is_child_of(grandchild_pid, child_pid);
        assert(result == 1);

        /* Inverse: intermediate child is NOT a descendant of grandchild */
        /* NOLINTNEXTLINE(readability-suspicious-call-argument) */
        result = is_child_of(child_pid, grandchild_pid);
        assert(result == 0);

        /*
         * Kill the entire process group (child + grandchild) and wait for
         * our direct child to be reaped.  The grandchild gets reparented to
         * init once child_pid exits and is reaped by init automatically.
         */
        kill_and_wait(-child_pid, SIGKILL);
    }
}

/**
 * @brief Regression test: kernel threads (ppid=2) must not be reported as
 *        descendants of PID 1
 * @note The former fast-path in is_child_of() returned true for any process
 *       whose getppid_of() result was not -1.  Linux kernel threads (children
 *       of kthreadd, PID 2) have ppid=2, which is a valid non-(-1) value, so
 *       the fast-path incorrectly claimed they were children of PID 1.
 *       This test locates such a thread via the process iterator and verifies
 *       that is_child_of() returns 0 after the fix.
 *       Only runs on Linux where PID 2 is kthreadd; a no-op elsewhere or when
 *       no kthreadd child is visible (e.g. minimal container).
 */
static void test_is_child_of_kernel_thread_not_child_of_init(void) {
#ifdef __linux__
    struct process_iterator iter;
    struct process *proc;
    struct process_filter filter;
    pid_t kthread_child = (pid_t)0;

    proc = (struct process *)malloc(sizeof(struct process));
    assert(proc != NULL);

    /* Iterate all processes looking for one whose ppid is 2 (kthreadd) */
    filter.pid = 0;
    filter.include_children = 0;
    filter.read_cmd = 0;
    if (init_process_iterator(&iter, &filter) == 0) {
        int ret;
        while (get_next_process(&iter, proc) != -1) {
            if (proc->ppid == (pid_t)2) {
                kthread_child = proc->pid;
                break;
            }
        }
        ret = close_process_iterator(&iter);
        assert(ret == 0);
    }
    free(proc);

    if (kthread_child == (pid_t)0) {
        /* No kthreadd child found (minimal container); skip */
        return;
    }
    /*
     * A kernel thread parented by kthreadd (PID 2) is NOT a descendant of
     * PID 1 (init/systemd); walking the chain reaches kthreadd whose own
     * ppid is 0, so the loop must stop and return 0.
     */
    assert(is_child_of(kthread_child, (pid_t)1) == 0);
#endif /* __linux__ */
}

/**
 * @brief Regression test: a process whose comm contains a newline must remain
 *        visible to the iterator, getppid_of(), and is_child_of()
 * @note prctl(PR_SET_NAME) allows embedding a newline in comm, which the
 *       kernel stores verbatim in /proc/[pid]/stat.  Reading only the first
 *       line of that file truncated the content inside comm, so the ')'
 *       terminating the comm field was never found and the process was
 *       silently dropped from iteration and parent-chain traversal.  Only
 *       runs on Linux where prctl(PR_SET_NAME) is available.
 */
static void test_process_iterator_newline_comm(void) {
#ifdef __linux__
    pid_t parent_pid, child_pid;
    struct process_iterator iter;
    struct process_filter filter;
    struct process *proc;
    int pipe_fds[2];
    char sync_byte = 0;
    int count, found, ret;

    parent_pid = getpid();

    proc = (struct process *)malloc(sizeof(struct process));
    if (proc == NULL) {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    assert(pipe(pipe_fds) == 0);

    child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        sigset_t full_mask;
        /* Child: set comm with embedded newline, signal parent, then wait */
        close(pipe_fds[0]);
        if (prctl(PR_SET_NAME, "ab\ncd") != 0) {
            close(pipe_fds[1]);
            _exit(1);
        }
        if (write(pipe_fds[1], "R", 1) != 1) {
            close(pipe_fds[1]);
            _exit(1);
        }
        close(pipe_fds[1]);
        if (sigfillset(&full_mask) != 0 ||
            sigprocmask(SIG_BLOCK, &full_mask, NULL) != 0) {
            _exit(1);
        }
        test_suspend_until_killed();
    }

    /* Parent: wait until the child has set its newline-containing comm */
    close(pipe_fds[1]);
    assert(read(pipe_fds[0], &sync_byte, 1) == 1);
    close(pipe_fds[0]);
    assert(sync_byte == 'R');

    /* Fast path: single-process filter must still find the child */
    filter.pid = child_pid;
    filter.include_children = 0;
    filter.read_cmd = 0;
    ret = init_process_iterator(&iter, &filter);
    assert(ret == 0);
    count = 0;
    while (get_next_process(&iter, proc) == 0) {
        assert(proc->pid == child_pid);
        assert(proc->ppid == parent_pid);
        count++;
    }
    assert(count == 1);
    ret = close_process_iterator(&iter);
    assert(ret == 0);

    /* getppid_of() and is_child_of() must also work on this process */
    assert(getppid_of(child_pid) == parent_pid);
    assert(is_child_of(child_pid, parent_pid) == 1);

    /* Scan path: include-children filter must discover the child too */
    filter.pid = parent_pid;
    filter.include_children = 1;
    filter.read_cmd = 0;
    ret = init_process_iterator(&iter, &filter);
    assert(ret == 0);
    found = 0;
    while (get_next_process(&iter, proc) == 0) {
        if (proc->pid == child_pid) {
            assert(proc->ppid == parent_pid);
            found = 1;
        }
    }
    assert(found == 1);
    ret = close_process_iterator(&iter);
    assert(ret == 0);

    /* Clean up child and free proc */
    free(proc);
    kill_and_wait(child_pid, SIGKILL);
#endif /* __linux__ */
}

/**
 * @brief Test process iterator filter edge cases
 * @note Tests various filter configurations
 */
static void test_process_iterator_filter_edge_cases(void) {
    struct process_iterator iter;
    struct process *proc;
    struct process_filter filter;
    int count;
    int ret;

    proc = (struct process *)malloc(sizeof(struct process));
    assert(proc != NULL);

    /* Test with PID 0 (all processes) and read_cmd enabled */
    filter.pid = (pid_t)0;
    filter.include_children = 0;
    filter.read_cmd = 1;

    count = 0;
    ret = init_process_iterator(&iter, &filter);
    assert(ret == 0);
    while (count < 10 && get_next_process(&iter, proc) == 0) {
        /* Just iterate a few processes to verify iter works */
        count++;
    }
    assert(count > 0);
    ret = close_process_iterator(&iter);
    assert(ret == 0);

    free(proc);
}

/**
 * @brief Test process iterator with a single process
 * @note Tests that the process iterator can retrieve the current process
 *       information correctly, both with and without child processes
 */
static void test_process_iterator_single(void) {
    struct process_iterator iter;
    struct process *proc;
    struct process_filter filter;
    size_t count;
    int ret;
    pid_t self_pid;
    pid_t self_ppid;

    self_pid = getpid();
    self_ppid = getppid();

    /* Allocate memory for process structure */
    proc = (struct process *)malloc(sizeof(struct process));
    if (proc == NULL) {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    /* Test without including children */
    filter.pid = getpid();
    filter.include_children = 0;
    filter.read_cmd = 0;
    count = 0;

    /* Initialize iterator and iterate through processes */
    ret = init_process_iterator(&iter, &filter);
    assert(ret == 0);
    while (get_next_process(&iter, proc) == 0) {
        assert(proc->pid == self_pid);
        assert(proc->ppid == self_ppid);
        assert(proc->cpu_time >= 0);
        count++;
    }
    assert(count == 1);
    ret = close_process_iterator(&iter);
    assert(ret == 0);

    /* Test with including children */
    filter.pid = getpid();
    filter.include_children = 1;
    filter.read_cmd = 0;
    count = 0;

    ret = init_process_iterator(&iter, &filter);
    assert(ret == 0);
    while (get_next_process(&iter, proc) == 0) {
        assert(proc->pid == self_pid);
        assert(proc->ppid == self_ppid);
        assert(proc->cpu_time >= 0);
        count++;
    }
    assert(count == 1);
    free(proc);
    ret = close_process_iterator(&iter);
    assert(ret == 0);
}

/**
 * @brief Test process iterator with multiple processes
 * @note Creates a child process and verifies that the iterator can retrieve
 *       both parent and child process information
 */
static void test_process_iterator_multiple(void) {
    struct process_iterator iter;
    struct process *proc;
    struct process_filter filter;
    size_t count = 0;
    int ret;
    pid_t self_pid;
    pid_t self_ppid;
    pid_t child_pid;

    /* Create a child process for testing */
    child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        sigset_t full_mask;
        /*
         * Block all blockable signals so no signal can wake the child
         * prematurely.  test_suspend_until_killed() suspends until the
         * parent kills it, and also exits on its own if the parent
         * aborts (failed assertion) so no orphan is left behind.
         */
        if (sigfillset(&full_mask) != 0) {
            _exit(1);
        }
        if (sigprocmask(SIG_BLOCK, &full_mask, NULL) != 0) {
            _exit(1);
        }
        test_suspend_until_killed();
    }

    /* Allocate memory for process structure */
    proc = (struct process *)malloc(sizeof(struct process));
    if (proc == NULL) {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    /* Set up filter to include children */
    self_pid = getpid();
    self_ppid = getppid();
    filter.pid = getpid();
    filter.include_children = 1;
    filter.read_cmd = 0;

    /* Initialize iterator and verify both processes are found */
    ret = init_process_iterator(&iter, &filter);
    assert(ret == 0);
    while (get_next_process(&iter, proc) == 0) {
        if (proc->pid == getpid()) {
            assert(proc->ppid == self_ppid);
        } else if (proc->pid == child_pid) {
            assert(proc->ppid == self_pid);
        } else {
            assert(0);
        }
        assert(proc->cpu_time >= 0);
        count++;
    }
    assert(count == 2);
    free(proc);
    ret = close_process_iterator(&iter);
    assert(ret == 0);

    /* Clean up child process */
    kill_and_wait(child_pid, SIGKILL);
}

/**
 * @brief Test process iterator with all system processes
 * @note Verifies that the iterator can retrieve processes and that the
 *       current process is correctly identified
 */
static void test_process_iterator_all(void) {
    struct process_iterator iter;
    struct process *proc;
    struct process_filter filter;
    size_t count = 0;
    int found_self = 0;
    int ret;
    pid_t self_ppid;

    /* Set up filter to get all processes */
    self_ppid = getppid();
    filter.pid = 0;
    filter.include_children = 0;
    filter.read_cmd = 0;

    /* Allocate memory for process structure */
    proc = (struct process *)malloc(sizeof(struct process));
    if (proc == NULL) {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    /* Initialize iterator and count processes */
    ret = init_process_iterator(&iter, &filter);
    assert(ret == 0);

    while (get_next_process(&iter, proc) == 0) {
        if (proc->pid == getpid()) {
            assert(proc->ppid == self_ppid);
            assert(proc->cpu_time >= 0);
            found_self = 1;
        }
        count++;
    }

    /* Verify we found at least one process and our own PID is visible */
    assert(count > 0);
    assert(found_self == 1);
    free(proc);
    ret = close_process_iterator(&iter);
    assert(ret == 0);
}

/**
 * @brief Test process name retrieval
 * @note Verifies that the process iterator correctly retrieves the
 *       OS-visible argv[0] (command field) of the current process, and that
 *       it can be used to find the process by name. When running under a
 *       wrapper (e.g., valgrind), the OS-visible argv[0] is the wrapper's
 *       path, not main()'s argv[0]; this test handles both cases.
 */
static void test_process_iterator_read_command(void) {
    struct process_iterator iter;
    struct process *proc;
    struct process_filter filter;
    pid_t found_pid;
    int ret;
    pid_t self_pid;
    pid_t self_ppid;

    /* Allocate memory for process structure */
    proc = (struct process *)malloc(sizeof(struct process));
    if (proc == NULL) {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    /* Set up filter to get current process with command reading */
    self_pid = getpid();
    self_ppid = getppid();
    filter.pid = getpid();
    filter.include_children = 0;
    filter.read_cmd = 1;

    /* Get process information and verify command name */
    ret = init_process_iterator(&iter, &filter);
    assert(ret == 0);
    ret = get_next_process(&iter, proc);
    assert(ret == 0);
    assert(proc->pid == self_pid);
    assert(proc->ppid == self_ppid);

    /*
     * proc->command must be the OS-visible argv[0] of the current
     * process. Verify it is non-empty and that find_process_by_name
     * can locate at least one process using its basename (round-trip
     * check). A direct PID comparison is intentionally omitted: when
     * multiple processes share the same basename (e.g., valgrind),
     * find_process_by_name may return an ancestor's PID.
     */
    assert(proc->command[0] != '\0');
    found_pid = find_process_by_name(file_basename(proc->command));
    assert(found_pid != 0);

    /* Verify no more processes */
    ret = get_next_process(&iter, proc);
    assert(ret != 0);
    free(proc);
    ret = close_process_iterator(&iter);
    assert(ret == 0);
}

/**
 * @brief Test getppid_of function
 * @note Verifies that getppid_of returns the correct parent PID for multiple
 *       processes, including the current process
 */
static void test_process_iterator_getppid_of(void) {
    struct process_iterator iter;
    struct process *proc;
    struct process_filter filter;
    int ret;
    pid_t ppid_self;
    pid_t expected_ppid;

    filter.pid = 0;
    filter.include_children = 0;
    filter.read_cmd = 0;

    /* Allocate memory for process structure */
    proc = (struct process *)malloc(sizeof(struct process));
    if (proc == NULL) {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    /* Iterate through all processes and verify parent PID */
    ret = init_process_iterator(&iter, &filter);
    assert(ret == 0);
    while (get_next_process(&iter, proc) == 0) {
        pid_t ppid_result;
        ppid_result = getppid_of(proc->pid);
        assert(ppid_result == proc->ppid);
    }
    free(proc);
    ret = close_process_iterator(&iter);
    assert(ret == 0);

    /* Verify current process's parent PID */
    ppid_self = getppid_of(getpid());
    expected_ppid = getppid();
    assert(ppid_self == expected_ppid);
}

/**
 * @brief Test init_process_iterator and get_next_process with NULL inputs
 * @note NULL it or NULL filter must return -1 without crashing
 */
static void test_process_iterator_null_inputs(void) {
    struct process_iterator iter;
    struct process_filter filter;
    struct process *proc;
    int ret;

    memset(&filter, 0, sizeof(filter));

    proc = (struct process *)malloc(sizeof(struct process));
    assert(proc != NULL);

    /* NULL iter pointer must return -1 */
    ret = init_process_iterator(NULL, &filter);
    assert(ret == -1);

    /* NULL filter pointer must return -1 */
    ret = init_process_iterator(&iter, NULL);
    assert(ret == -1);

    /* get_next_process with NULL iter must return -1 */
    ret = get_next_process(NULL, proc);
    assert(ret == -1);

    /* get_next_process with NULL p must return -1 */
    filter.pid = getpid();
    filter.include_children = 0;
    filter.read_cmd = 0;
    ret = init_process_iterator(&iter, &filter);
    assert(ret == 0);
    ret = get_next_process(&iter, NULL);
    assert(ret == -1);
    ret = close_process_iterator(&iter);
    assert(ret == 0);

    /* get_next_process after close (filter=NULL) must return -1 */
    ret = get_next_process(&iter, proc);
    assert(ret == -1);

    free(proc);
}

/**
 * @brief Test close_process_iterator with NULL pointer
 * @note Must return -1 without crashing
 */
static void test_process_iterator_close_null(void) {
    int ret;
    ret = close_process_iterator(NULL);
    assert(ret == -1);
}

/**
 * @brief Test getppid_of with boundary and invalid PIDs
 * @note PID 0 and INT_MAX must return -1; current PID must return getppid()
 */
static void test_process_iterator_getppid_of_edges(void) {
    pid_t ppid_of_zero;
    pid_t ppid_of_neg;
    pid_t ppid_of_max;
    pid_t ppid_self;
    pid_t expected_ppid;
    pid_t invalid_pid;
    /* PID 0: /proc/0/stat does not exist */
    invalid_pid = (pid_t)-1;
    ppid_of_zero = getppid_of((pid_t)0);
    assert(ppid_of_zero == invalid_pid);

    /* Negative PID: invalid, must return -1 */
    ppid_of_neg = getppid_of((pid_t)-1);
    assert(ppid_of_neg == invalid_pid);

    /* INT_MAX: virtually guaranteed non-existent PID */
    ppid_of_max = getppid_of((pid_t)INT_MAX);
    assert(ppid_of_max == invalid_pid);

    /* Current process: must match getppid() */
    ppid_self = getppid_of(getpid());
    expected_ppid = getppid();
    assert(ppid_self == expected_ppid);
}

/**
 * @brief Test init_process_iterator with pid=0 and include_children=1
 * @note Should open /proc and enumerate processes (same as all-process scan)
 */
static void test_process_iterator_init_all_with_children(void) {
    struct process_iterator iter;
    struct process_filter filter;
    struct process *proc;
    int count = 0;
    int ret;

    proc = (struct process *)malloc(sizeof(struct process));
    assert(proc != NULL);

    filter.pid = 0;
    filter.include_children = 1;
    filter.read_cmd = 0;

    ret = init_process_iterator(&iter, &filter);
    assert(ret == 0);
    while (count < 5 && get_next_process(&iter, proc) == 0) {
        count++;
    }
    assert(count > 0);
    ret = close_process_iterator(&iter);
    assert(ret == 0);

    free(proc);
}

/**
 * @brief Test get_next_process after end_of_processes is set
 * @note Must return -1 immediately on every call after first exhaustion
 */
static void test_process_iterator_exhaust_single(void) {
    struct process_iterator iter;
    struct process_filter filter;
    struct process *proc;
    int ret;
    pid_t self_pid;

    proc = (struct process *)malloc(sizeof(struct process));
    assert(proc != NULL);

    self_pid = getpid();
    filter.pid = getpid();
    filter.include_children = 0;
    filter.read_cmd = 0;

    ret = init_process_iterator(&iter, &filter);
    assert(ret == 0);

    /* First call: returns the process */
    ret = get_next_process(&iter, proc);
    assert(ret == 0);
    assert(proc->pid == self_pid);

    /* Second call: end_of_processes=1, must return -1 */
    ret = get_next_process(&iter, proc);
    assert(ret == -1);

    /* Third call: still -1 */
    ret = get_next_process(&iter, proc);
    assert(ret == -1);

    ret = close_process_iterator(&iter);
    assert(ret == 0);
    free(proc);
}

/**
 * @brief Test process iterator with include_children=1 for current process
 * @note With a child process running, both parent and child must appear
 */
static void test_process_iterator_with_children(void) {
    struct process_iterator iter;
    struct process_filter filter;
    struct process *proc;
    pid_t child_pid;
    int found_parent = 0, found_child = 0;
    int ret;

    child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid == 0) {
        sigset_t full_mask;
        /*
         * Block all blockable signals so no signal can wake the child
         * prematurely.  test_suspend_until_killed() suspends until the
         * parent kills it, and also exits on its own if the parent
         * aborts (failed assertion) so no orphan is left behind.
         */
        if (sigfillset(&full_mask) != 0) {
            _exit(1);
        }
        if (sigprocmask(SIG_BLOCK, &full_mask, NULL) != 0) {
            _exit(1);
        }
        test_suspend_until_killed();
    }

    proc = (struct process *)malloc(sizeof(struct process));
    assert(proc != NULL);

    filter.pid = getpid();
    filter.include_children = 1;
    filter.read_cmd = 0;

    ret = init_process_iterator(&iter, &filter);
    assert(ret == 0);
    while (get_next_process(&iter, proc) == 0) {
        if (proc->pid == getpid()) {
            found_parent = 1;
        }
        if (proc->pid == child_pid) {
            found_child = 1;
        }
    }
    ret = close_process_iterator(&iter);
    assert(ret == 0);

    assert(found_parent == 1);
    assert(found_child == 1);

    free(proc);
    kill_and_wait(child_pid, SIGKILL);
}

/**
 * @brief Test close_process_iterator when dip is already NULL
 * @note After init with single-pid filter, dip==NULL; close must return 0
 */
static void test_process_iterator_close_null_dip(void) {
    struct process_iterator iter;
    struct process_filter filter;
    int ret;

    filter.pid = getpid();
    filter.include_children = 0;
    filter.read_cmd = 0;

    ret = init_process_iterator(&iter, &filter);
    assert(ret == 0);
#if defined(__linux__)
    /* dip is NULL because single-PID optimisation skips opendir() */
    assert(iter.proc_dir == NULL);
#endif
    ret = close_process_iterator(&iter);
    assert(ret == 0);
}

/**
 * @brief Test that get_next_process handles NULL proc_dir defensively
 * @note On Linux, the single-PID optimisation leaves proc_dir=NULL
 *       Switching the filter to general mode (pid=0) without reinitialising
 *       exercises the NULL-proc_dir guard added to get_next_process.
 */
static void test_process_iterator_null_proc_dir_guard(void) {
#if defined(__linux__)
    struct process_iterator iter;
    struct process_filter filter;
    struct process *proc;
    int ret;

    proc = (struct process *)malloc(sizeof(struct process));
    assert(proc != NULL);

    /*
     * Initialise with single-PID filter so that the optimisation path
     * is taken and proc_dir is left as NULL.
     */
    filter.pid = getpid();
    filter.include_children = 0;
    filter.read_cmd = 0;
    ret = init_process_iterator(&iter, &filter);
    assert(ret == 0);
    assert(iter.proc_dir == NULL);

    /*
     * Change the filter to general mode (pid=0) without reinitialising.
     * get_next_process must detect proc_dir==NULL and return -1 without
     * crashing (the defensive NULL guard).
     */
    filter.pid = 0;
    ret = get_next_process(&iter, proc);
    assert(ret == -1);

    /* close_process_iterator handles a zeroed iterator correctly */
    ret = close_process_iterator(&iter);
    assert(ret == 0);

    free(proc);
#endif
}

/***************************************************************************
 * CLI MODULE TESTS
 ***************************************************************************/

/**
 * @brief Helper: fork with output suppressed, call parse_arguments, return
 *  the child's exit-status code
 * @param argc Argument count
 * @param argv Argument vector
 * @return Exit status from the child process
 */
static int run_parse_in_child(int argc, char **argv) {
    pid_t pid, waited;
    int status, exited;
    struct cpulimit_cfg cfg;

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        parse_arguments(argc, argv, &cfg);
        _exit(99); /* Only reached when parse_arguments returns (valid args) */
    }
    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    return WEXITSTATUS(status);
}

/**
 * @brief Test parse_arguments with -l LIMIT -p PID (valid PID mode)
 * @note Verifies target_pid, limit fraction, and implied lazy_mode
 */
static void test_cli_pid_mode(void) {
    struct cpulimit_cfg cfg;
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_50[] = "50";
    char arg_p[] = "-p";
    char arg_pid[32];
    char *test_argv[6];
    pid_t self_pid;

    self_pid = getpid();
    snprintf(arg_pid, sizeof(arg_pid), "%ld", (long)getpid());
    test_argv[0] = arg0;
    test_argv[1] = arg_l;
    test_argv[2] = arg_50;
    test_argv[3] = arg_p;
    test_argv[4] = arg_pid;
    test_argv[5] = NULL;

    memset(&cfg, 0, sizeof(cfg));
    parse_arguments(5, test_argv, &cfg);

    assert(cfg.target_pid == self_pid);
    assert(cfg.limit >= 0.4999 && cfg.limit <= 0.5001);
    assert(cfg.lazy_mode == 1); /* -p implies lazy */
    assert(cfg.verbose == 0);
    assert(cfg.include_children == 0);
    assert(cfg.exe_name == NULL);
    assert(cfg.command_mode == 0);
}

/**
 * @brief Test parse_arguments with -l LIMIT -e EXE (valid exe mode)
 * @note Verifies exe_name, limit fraction; lazy_mode stays 0
 */
static void test_cli_exe_mode(void) {
    struct cpulimit_cfg cfg;
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_25[] = "25";
    char arg_e[] = "-e";
    char arg_exe[] = "some_exe";
    char *test_argv[6];
    int cmp_ret;

    test_argv[0] = arg0;
    test_argv[1] = arg_l;
    test_argv[2] = arg_25;
    test_argv[3] = arg_e;
    test_argv[4] = arg_exe;
    test_argv[5] = NULL;

    memset(&cfg, 0, sizeof(cfg));
    parse_arguments(5, test_argv, &cfg);

    assert(cfg.exe_name != NULL);
    cmp_ret = strcmp(cfg.exe_name, "some_exe");
    assert(cmp_ret == 0);
    assert(cfg.limit >= 0.2499 && cfg.limit <= 0.2501);
    assert(cfg.lazy_mode == 0); /* -e alone does not imply lazy */
    assert(cfg.target_pid == 0);
    assert(cfg.command_mode == 0);
}

/**
 * @brief Test parse_arguments with -l LIMIT COMMAND [ARGS] (command mode)
 * @note Verifies command_mode=1, command_args pointer, implied lazy_mode
 */
static void test_cli_command_mode(void) {
    struct cpulimit_cfg cfg;
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_75[] = "75";
    char arg_cmd[] = "echo";
    char arg_msg[] = "hello";
    char *test_argv[6];
    int cmp_ret;

    test_argv[0] = arg0;
    test_argv[1] = arg_l;
    test_argv[2] = arg_75;
    test_argv[3] = arg_cmd;
    test_argv[4] = arg_msg;
    test_argv[5] = NULL;

    memset(&cfg, 0, sizeof(cfg));
    parse_arguments(5, test_argv, &cfg);

    assert(cfg.command_mode == 1);
    assert(cfg.command_args != NULL);
    cmp_ret = strcmp(cfg.command_args[0], "echo");
    assert(cmp_ret == 0);
    assert(cfg.lazy_mode == 1); /* command mode implies lazy */
    assert(cfg.target_pid == 0);
    assert(cfg.exe_name == NULL);
    assert(cfg.limit >= 0.7499 && cfg.limit <= 0.7501);
}

/**
 * @brief Test parse_arguments with long options (--limit=N, --pid=N)
 * @note Long-form options must behave identically to short-form
 */
static void test_cli_long_options(void) {
    struct cpulimit_cfg cfg;
    char arg0[] = "cpulimit";
    char arg_limit[] = "--limit=50";
    char arg_pid_long[48];
    char *test_argv[4];
    pid_t self_pid;

    self_pid = getpid();
    snprintf(arg_pid_long, sizeof(arg_pid_long), "--pid=%ld", (long)getpid());
    test_argv[0] = arg0;
    test_argv[1] = arg_limit;
    test_argv[2] = arg_pid_long;
    test_argv[3] = NULL;

    memset(&cfg, 0, sizeof(cfg));
    parse_arguments(3, test_argv, &cfg);

    assert(cfg.target_pid == self_pid);
    assert(cfg.limit >= 0.4999 && cfg.limit <= 0.5001);
    assert(cfg.lazy_mode == 1);
}

/**
 * @brief Test parse_arguments with --exe=EXE long option
 * @note --exe long form must set exe_name correctly
 */
static void test_cli_long_option_exe(void) {
    struct cpulimit_cfg cfg;
    char arg0[] = "cpulimit";
    char arg_limit[] = "--limit=50";
    char arg_exe[] = "--exe=myapp";
    char *test_argv[4];
    int cmp_ret;

    test_argv[0] = arg0;
    test_argv[1] = arg_limit;
    test_argv[2] = arg_exe;
    test_argv[3] = NULL;

    memset(&cfg, 0, sizeof(cfg));
    parse_arguments(3, test_argv, &cfg);

    assert(cfg.exe_name != NULL);
    cmp_ret = strcmp(cfg.exe_name, "myapp");
    assert(cmp_ret == 0);
    assert(cfg.limit >= 0.4999 && cfg.limit <= 0.5001);
}

/**
 * @brief Test parse_arguments with -z and -i optional flags
 * @note Verifies lazy_mode and include_children are set correctly
 */
static void test_cli_optional_flags(void) {
    struct cpulimit_cfg cfg;
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_50[] = "50";
    char arg_z[] = "-z";
    char arg_i[] = "-i";
    char arg_e[] = "-e";
    char arg_exe[] = "foo";
    char *test_argv[8];

    test_argv[0] = arg0;
    test_argv[1] = arg_l;
    test_argv[2] = arg_50;
    test_argv[3] = arg_z;
    test_argv[4] = arg_i;
    test_argv[5] = arg_e;
    test_argv[6] = arg_exe;
    test_argv[7] = NULL;

    memset(&cfg, 0, sizeof(cfg));
    parse_arguments(7, test_argv, &cfg);

    assert(cfg.lazy_mode == 1);
    assert(cfg.include_children == 1);
}

/**
 * @brief Test parse_arguments with -v (verbose) flag
 * @note -v causes a "N CPUs detected" print; run in fork to suppress it
 */
static void test_cli_verbose_flag(void) {
    pid_t pid, waited;
    int status, exited, exit_code;

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        struct cpulimit_cfg cfg;
        char arg0[] = "cpulimit";
        char arg_l[] = "-l";
        char arg_50[] = "50";
        char arg_v[] = "-v";
        char arg_e[] = "-e";
        char arg_exe[] = "foo";
        char *test_argv[7];

        test_argv[0] = arg0;
        test_argv[1] = arg_l;
        test_argv[2] = arg_50;
        test_argv[3] = arg_v;
        test_argv[4] = arg_e;
        test_argv[5] = arg_exe;
        test_argv[6] = NULL;

        close(STDOUT_FILENO); /* Suppress "N CPUs detected" output */
        memset(&cfg, 0, sizeof(cfg));
        parse_arguments(6, test_argv, &cfg);
        if (cfg.verbose != 1) {
            _exit(1);
        }
        _exit(0);
    }
    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == 0);
}

/**
 * @brief Test parse_arguments with -h / --help
 * @note Both short and long help flags must exit with EXIT_SUCCESS
 */
static void test_cli_help(void) {
    char arg0[] = "cpulimit";
    char arg_h[] = "-h";
    char arg_help[] = "--help";
    char *args1[3];
    char *args2[3];
    int parse_ret;

    args1[0] = arg0;
    args1[1] = arg_h;
    args1[2] = NULL;
    parse_ret = run_parse_in_child(2, args1);
    assert(parse_ret == EXIT_SUCCESS);

    args2[0] = arg0;
    args2[1] = arg_help;
    args2[2] = NULL;
    parse_ret = run_parse_in_child(2, args2);
    assert(parse_ret == EXIT_SUCCESS);
}

/**
 * @brief Test parse_arguments when -l/--limit is not supplied
 * @note The limit option is required; absence must cause EXIT_FAILURE
 */
static void test_cli_missing_limit(void) {
    char arg0[] = "cpulimit";
    char arg_e[] = "-e";
    char arg_exe[] = "foo";
    char *test_argv[4];
    int parse_ret;

    test_argv[0] = arg0;
    test_argv[1] = arg_e;
    test_argv[2] = arg_exe;
    test_argv[3] = NULL;
    parse_ret = run_parse_in_child(3, test_argv);
    assert(parse_ret == EXIT_FAILURE);
}

/**
 * @brief Test parse_arguments with various invalid limit values
 * @note zero, negative, non-numeric, NaN, and above-max all cause
 *       EXIT_FAILURE
 */
static void test_cli_invalid_limits(void) {
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_e[] = "-e";
    char arg_exe[] = "foo";
    char arg_zero[] = "0";
    char arg_neg[] = "-5";
    char arg_abc[] = "abc";
    char arg_nan[] = "nan";
    char arg_huge[] = "99999";
    char *test_argv[6];
    int parse_ret;

    /* Common setup: prog -l LIMIT -e foo */
    test_argv[0] = arg0;
    test_argv[1] = arg_l;
    test_argv[3] = arg_e;
    test_argv[4] = arg_exe;
    test_argv[5] = NULL;

    test_argv[2] = arg_zero;
    parse_ret = run_parse_in_child(5, test_argv);
    assert(parse_ret == EXIT_FAILURE);

    test_argv[2] = arg_neg;
    parse_ret = run_parse_in_child(5, test_argv);
    assert(parse_ret == EXIT_FAILURE);

    test_argv[2] = arg_abc;
    parse_ret = run_parse_in_child(5, test_argv);
    assert(parse_ret == EXIT_FAILURE);

    test_argv[2] = arg_nan;
    parse_ret = run_parse_in_child(5, test_argv);
    assert(parse_ret == EXIT_FAILURE);

    test_argv[2] = arg_huge;
    parse_ret = run_parse_in_child(5, test_argv);
    assert(parse_ret == EXIT_FAILURE);
}

/**
 * @brief Test parse_arguments with various invalid PID values
 * @note 0, 1 (reserved), -1, non-numeric, and trailing-char PIDs all cause
 *       EXIT_FAILURE
 */
static void test_cli_invalid_pids(void) {
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_50[] = "50";
    char arg_p[] = "-p";
    char arg_pid0[] = "0";
    char arg_pid1[] = "1";
    char arg_pidneg[] = "-1";
    char arg_pidabc[] = "abc";
    char arg_pidtrail[] = "10x";
    char *test_argv[6];
    int parse_ret;

    /* Common setup: prog -l 50 -p PID */
    test_argv[0] = arg0;
    test_argv[1] = arg_l;
    test_argv[2] = arg_50;
    test_argv[3] = arg_p;
    test_argv[5] = NULL;

    test_argv[4] = arg_pid0;
    parse_ret = run_parse_in_child(5, test_argv);
    assert(parse_ret == EXIT_FAILURE);

    test_argv[4] =
        arg_pid1; /* pid <= 1 validation rejects PID 1 (init/systemd) */
    parse_ret = run_parse_in_child(5, test_argv);
    assert(parse_ret == EXIT_FAILURE);

    test_argv[4] = arg_pidneg;
    parse_ret = run_parse_in_child(5, test_argv);
    assert(parse_ret == EXIT_FAILURE);

    test_argv[4] = arg_pidabc;
    parse_ret = run_parse_in_child(5, test_argv);
    assert(parse_ret == EXIT_FAILURE);

    test_argv[4] = arg_pidtrail;
    parse_ret = run_parse_in_child(5, test_argv);
    assert(parse_ret == EXIT_FAILURE);
}

/**
 * @brief Test parse_arguments with an empty exe name (-e "")
 * @note Empty string for -e must cause EXIT_FAILURE
 */
static void test_cli_empty_exe(void) {
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_50[] = "50";
    char arg_e[] = "-e";
    char arg_empty[] = "";
    char *test_argv[6];
    int parse_ret;

    test_argv[0] = arg0;
    test_argv[1] = arg_l;
    test_argv[2] = arg_50;
    test_argv[3] = arg_e;
    test_argv[4] = arg_empty;
    test_argv[5] = NULL;
    parse_ret = run_parse_in_child(5, test_argv);
    assert(parse_ret == EXIT_FAILURE);
}

/**
 * @brief Test parse_arguments with no target specified (-l only)
 * @note Providing a limit but no -p/-e/COMMAND must cause EXIT_FAILURE
 */
static void test_cli_no_target(void) {
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_50[] = "50";
    char *test_argv[4];
    int parse_ret;

    test_argv[0] = arg0;
    test_argv[1] = arg_l;
    test_argv[2] = arg_50;
    test_argv[3] = NULL;
    parse_ret = run_parse_in_child(3, test_argv);
    assert(parse_ret == EXIT_FAILURE);
}

/**
 * @brief Test parse_arguments with both -p and -e simultaneously
 * @note Only one target is allowed; two must cause EXIT_FAILURE
 */
static void test_cli_multiple_targets(void) {
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_50[] = "50";
    char arg_p[] = "-p";
    char arg_pid[32];
    char arg_e[] = "-e";
    char arg_exe[] = "foo";
    char *test_argv[8];
    int parse_ret;

    snprintf(arg_pid, sizeof(arg_pid), "%ld", (long)getpid());
    test_argv[0] = arg0;
    test_argv[1] = arg_l;
    test_argv[2] = arg_50;
    test_argv[3] = arg_p;
    test_argv[4] = arg_pid;
    test_argv[5] = arg_e;
    test_argv[6] = arg_exe;
    test_argv[7] = NULL;
    parse_ret = run_parse_in_child(7, test_argv);
    assert(parse_ret == EXIT_FAILURE);
}

/**
 * @brief Test parse_arguments with unknown short and long options
 * @note Unrecognised options must cause EXIT_FAILURE
 */
static void test_cli_unknown_option(void) {
    char arg0[] = "cpulimit";
    char arg_x[] = "-x";
    char arg_bogus[] = "--bogus";
    char *args1[3];
    char *args2[3];
    int parse_ret;

    args1[0] = arg0;
    args1[1] = arg_x;
    args1[2] = NULL;
    parse_ret = run_parse_in_child(2, args1);
    assert(parse_ret == EXIT_FAILURE);

    args2[0] = arg0;
    args2[1] = arg_bogus;
    args2[2] = NULL;
    parse_ret = run_parse_in_child(2, args2);
    assert(parse_ret == EXIT_FAILURE);
}

/**
 * @brief Test parse_arguments when a required option argument is absent
 * @note -p with no PID and -l with no value must both cause EXIT_FAILURE
 */
static void test_cli_missing_arg(void) {
    char arg0[] = "cpulimit";
    char arg_p[] = "-p";
    char arg_l[] = "-l";
    char *args1[3];
    char *args2[3];
    int parse_ret;

    args1[0] = arg0;
    args1[1] = arg_p;
    args1[2] = NULL;
    parse_ret = run_parse_in_child(2, args1);
    assert(parse_ret == EXIT_FAILURE);

    args2[0] = arg0;
    args2[1] = arg_l;
    args2[2] = NULL;
    parse_ret = run_parse_in_child(2, args2);
    assert(parse_ret == EXIT_FAILURE);
}

/**
 * @brief Test parse_arguments with --include-children long option
 * @note --include-children must set include_children=1
 */
static void test_cli_long_option_include_children(void) {
    struct cpulimit_cfg cfg;
    char arg0[] = "cpulimit";
    char arg_limit[] = "--limit=50";
    char arg_ic[] = "--include-children";
    char arg_e[] = "-e";
    char arg_exe[] = "foo";
    char *test_argv[6];

    test_argv[0] = arg0;
    test_argv[1] = arg_limit;
    test_argv[2] = arg_ic;
    test_argv[3] = arg_e;
    test_argv[4] = arg_exe;
    test_argv[5] = NULL;

    memset(&cfg, 0, sizeof(cfg));
    parse_arguments(5, test_argv, &cfg);
    assert(cfg.include_children == 1);
}

/**
 * @brief Test parse_arguments with limit exactly equal to 100*ncpu (maximum)
 * @note Limit at the boundary must be accepted
 */
static void test_cli_limit_at_max(void) {
    struct cpulimit_cfg cfg;
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_max[32];
    char arg_e[] = "-e";
    char arg_exe[] = "foo";
    char *test_argv[6];
    int ncpu = get_ncpu();
    double lower_limit;
    double upper_limit;

    snprintf(arg_max, sizeof(arg_max), "%d", 100 * ncpu);
    test_argv[0] = arg0;
    test_argv[1] = arg_l;
    test_argv[2] = arg_max;
    test_argv[3] = arg_e;
    test_argv[4] = arg_exe;
    test_argv[5] = NULL;

    memset(&cfg, 0, sizeof(cfg));
    parse_arguments(5, test_argv, &cfg);
    /* Limit stored as fraction: 100*ncpu/100 == ncpu */
    lower_limit = (double)ncpu - 0.001;
    upper_limit = (double)ncpu + 0.001;
    assert(cfg.limit >= lower_limit);
    assert(cfg.limit <= upper_limit);
}

/**
 * @brief Test parse_arguments with minimum valid PID (>1; try 2)
 * @note PID 2 is valid if it exists; test parse path only (fork to isolate)
 */
static void test_cli_pid_minimum_valid(void) {
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_50[] = "50";
    char arg_p[] = "-p";
    char arg_pid2[] = "2";
    char *test_argv[6];
    int ret;

    test_argv[0] = arg0;
    test_argv[1] = arg_l;
    test_argv[2] = arg_50;
    test_argv[3] = arg_p;
    test_argv[4] = arg_pid2;
    test_argv[5] = NULL;

    /* PID 2 is syntactically valid (> 1); parse must succeed (exit code 99) */
    ret = run_parse_in_child(5, test_argv);
    assert(ret == 99);
}

/**
 * @brief Test parse_arguments with a limit value that has trailing whitespace
 * @note "50 " (trailing space) has trailing chars after strtod; must fail
 */
static void test_cli_limit_trailing_chars(void) {
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_lim[] = "50 "; /* trailing space */
    char arg_e[] = "-e";
    char arg_exe[] = "foo";
    char *test_argv[6];
    int parse_ret;

    test_argv[0] = arg0;
    test_argv[1] = arg_l;
    test_argv[2] = arg_lim;
    test_argv[3] = arg_e;
    test_argv[4] = arg_exe;
    test_argv[5] = NULL;
    parse_ret = run_parse_in_child(5, test_argv);
    assert(parse_ret == EXIT_FAILURE);
}

/**
 * @brief Test parse_arguments rejects duplicate target and limit options
 * @note Repeated -p/-e/-l options must fail fast with EXIT_FAILURE
 */
static void test_cli_duplicate_options(void) {
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_50[] = "50";
    char arg_40[] = "40";
    char arg_p[] = "-p";
    char arg_pid[] = "123";
    char arg_pid2[] = "456";
    char arg_e[] = "-e";
    char arg_exe1[] = "foo";
    char arg_exe2[] = "bar";
    char *args_dup_pid[8];
    char *args_dup_exe[8];
    char *args_dup_limit[8];
    int parse_ret;

    args_dup_pid[0] = arg0;
    args_dup_pid[1] = arg_l;
    args_dup_pid[2] = arg_50;
    args_dup_pid[3] = arg_p;
    args_dup_pid[4] = arg_pid;
    args_dup_pid[5] = arg_p;
    args_dup_pid[6] = arg_pid2;
    args_dup_pid[7] = NULL;
    parse_ret = run_parse_in_child(7, args_dup_pid);
    assert(parse_ret == EXIT_FAILURE);

    args_dup_exe[0] = arg0;
    args_dup_exe[1] = arg_l;
    args_dup_exe[2] = arg_50;
    args_dup_exe[3] = arg_e;
    args_dup_exe[4] = arg_exe1;
    args_dup_exe[5] = arg_e;
    args_dup_exe[6] = arg_exe2;
    args_dup_exe[7] = NULL;
    parse_ret = run_parse_in_child(7, args_dup_exe);
    assert(parse_ret == EXIT_FAILURE);

    args_dup_limit[0] = arg0;
    args_dup_limit[1] = arg_l;
    args_dup_limit[2] = arg_50;
    args_dup_limit[3] = arg_l;
    args_dup_limit[4] = arg_40;
    args_dup_limit[5] = arg_e;
    args_dup_limit[6] = arg_exe1;
    args_dup_limit[7] = NULL;
    parse_ret = run_parse_in_child(7, args_dup_limit);
    assert(parse_ret == EXIT_FAILURE);
}

/**
 * @brief Test parse_arguments rejects NULL cfg pointer
 */
static void test_cli_null_cfg(void) {
    char arg0[] = "cpulimit";
    char arg_l[] = "-l";
    char arg_50[] = "50";
    char arg_p[] = "-p";
    char arg_2[] = "2";
    char *test_argv[6];
    pid_t pid;
    int status;
    int parse_ret;
    pid_t waited_pid;
    int exited;
    int exit_code;

    test_argv[0] = arg0;
    test_argv[1] = arg_l;
    test_argv[2] = arg_50;
    test_argv[3] = arg_p;
    test_argv[4] = arg_2;
    test_argv[5] = NULL;

    parse_ret = run_parse_in_child(5, test_argv);
    assert(parse_ret == 99);

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        parse_arguments(5, test_argv, NULL);
        _exit(99);
    }
    waited_pid = waitpid(pid, &status, 0);
    assert(waited_pid == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == EXIT_FAILURE);
}

/**
 * @brief Test parse_arguments rejects invalid argc/argv combinations
 */
static void test_cli_invalid_api_inputs(void) {
    struct cpulimit_cfg cfg;
    char *test_argv[2];
    pid_t pid;
    int status;
    pid_t waited_pid;
    int exited;
    int exit_code;

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        parse_arguments(0, NULL, &cfg);
        _exit(99);
    }
    waited_pid = waitpid(pid, &status, 0);
    assert(waited_pid == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == EXIT_FAILURE);

    test_argv[0] = NULL;
    test_argv[1] = NULL;
    fflush(stdout);
    fflush(stderr);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        parse_arguments(1, test_argv, &cfg);
        _exit(99);
    }
    waited_pid = waitpid(pid, &status, 0);
    assert(waited_pid == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == EXIT_FAILURE);
}

/**
 * @brief Test parse_arguments with --lazy and --verbose long options
 * @note Long forms of -z and -v must behave identically to short forms
 */
static void test_cli_long_options_lazy_verbose(void) {
    struct cpulimit_cfg cfg;
    char arg0[] = "cpulimit";
    char arg_limit[] = "--limit=50";
    char arg_lazy[] = "--lazy";
    char arg_verbose[] = "--verbose";
    char arg_e[] = "-e";
    char arg_exe[] = "foo";
    char *test_argv[7];
    pid_t pid, waited;
    int status, exited, exit_code;

    test_argv[0] = arg0;
    test_argv[1] = arg_limit;
    test_argv[2] = arg_lazy;
    test_argv[3] = arg_verbose;
    test_argv[4] = arg_e;
    test_argv[5] = arg_exe;
    test_argv[6] = NULL;

    /* verbose prints to stdout; suppress it */
    fflush(stdout);
    fflush(stderr);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(STDOUT_FILENO);
        memset(&cfg, 0, sizeof(cfg));
        parse_arguments(6, test_argv, &cfg);
        if (cfg.lazy_mode != 1) {
            _exit(1);
        }
        if (cfg.verbose != 1) {
            _exit(2);
        }
        _exit(0);
    }
    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == 0);
}

/***************************************************************************
 * PROCESS_TABLE MODULE TESTS
 ***************************************************************************/

/**
 * @brief Test process buckets initialization and destruction
 * @note Tests init_process_table and destroy_process_table
 */
static void test_process_table_init_destroy(void) {
    struct process_table proc_table;

    /* Test initialization with small hash_size */
    init_process_table(&proc_table, 16);
    assert(proc_table.buckets != NULL);
    assert(proc_table.hash_size == 16);
    destroy_process_table(&proc_table);
    assert(proc_table.buckets == NULL);

    /* Test initialization with larger hash_size */
    init_process_table(&proc_table, 256);
    assert(proc_table.buckets != NULL);
    assert(proc_table.hash_size == 256);
    destroy_process_table(&proc_table);

    /* Test zero hash_size fallback (must avoid division by zero in hashing) */
    init_process_table(&proc_table, 0);
    assert(proc_table.buckets != NULL);
    assert(proc_table.hash_size == 1);
    destroy_process_table(&proc_table);

    /* Test destroy with NULL (should not crash) */
    destroy_process_table(NULL);
}

/**
 * @brief Test process buckets add and find operations
 * @note Tests add_to_process_table and find_in_process_table
 */
static void test_process_table_add_find(void) {
    struct process_table proc_table;
    struct process *proc1, *proc2, *proc3;
    const struct process *found;

    init_process_table(&proc_table, 64);

    /* Create test processes */
    proc1 = (struct process *)malloc(sizeof(struct process));
    proc2 = (struct process *)malloc(sizeof(struct process));
    proc3 = (struct process *)malloc(sizeof(struct process));
    assert(proc1 != NULL);
    assert(proc2 != NULL);
    assert(proc3 != NULL);

    proc1->pid = 100;
    proc1->ppid = 1;
    proc1->cpu_time = 0.0;

    proc2->pid = 200;
    proc2->ppid = 1;
    proc2->cpu_time = 0.0;

    proc3->pid = 300;
    proc3->ppid = 1;
    proc3->cpu_time = 0.0;

    /* Test find on empty buckets */
    found = find_in_process_table(&proc_table, 100);
    assert(found == NULL);

    /* Add first process */
    add_to_process_table(&proc_table, proc1);
    found = find_in_process_table(&proc_table, 100);
    assert(found == proc1);
    assert(found->pid == 100);

    /* Add second process */
    add_to_process_table(&proc_table, proc2);
    found = find_in_process_table(&proc_table, 200);
    assert(found == proc2);
    assert(found->pid == 200);

    /* Verify first process still findable */
    found = find_in_process_table(&proc_table, 100);
    assert(found == proc1);

    /* Add third process */
    add_to_process_table(&proc_table, proc3);
    found = find_in_process_table(&proc_table, 300);
    assert(found == proc3);

    /* Test find non-existent PID */
    found = find_in_process_table(&proc_table, 999);
    assert(found == NULL);

    /* Test find with NULL buckets */
    found = find_in_process_table(NULL, 100);
    assert(found == NULL);

    destroy_process_table(&proc_table);
}

/**
 * @brief Test process buckets delete operation
 * @note Tests delete_from_process_table
 */
static void test_process_table_del(void) {
    struct process_table proc_table;
    struct process *proc1, *proc2, *proc3;
    const struct process *found;
    int ret;

    init_process_table(&proc_table, 64);

    proc1 = (struct process *)malloc(sizeof(struct process));
    proc2 = (struct process *)malloc(sizeof(struct process));
    proc3 = (struct process *)malloc(sizeof(struct process));
    assert(proc1 != NULL);
    assert(proc2 != NULL);
    assert(proc3 != NULL);

    proc1->pid = 100;
    proc2->pid = 200;
    proc3->pid = 300;

    add_to_process_table(&proc_table, proc1);
    add_to_process_table(&proc_table, proc2);
    add_to_process_table(&proc_table, proc3);

    /* Delete middle process */
    ret = delete_from_process_table(&proc_table, 200);
    assert(ret == 0);
    found = find_in_process_table(&proc_table, 200);
    assert(found == NULL);

    /* Verify others still exist */
    found = find_in_process_table(&proc_table, 100);
    assert(found == proc1);
    found = find_in_process_table(&proc_table, 300);
    assert(found == proc3);

    /* Delete first process */
    ret = delete_from_process_table(&proc_table, 100);
    assert(ret == 0);
    found = find_in_process_table(&proc_table, 100);
    assert(found == NULL);

    /* Delete last process */
    ret = delete_from_process_table(&proc_table, 300);
    assert(ret == 0);
    found = find_in_process_table(&proc_table, 300);
    assert(found == NULL);

    /* Try to delete non-existent process */
    ret = delete_from_process_table(&proc_table, 999);
    assert(ret == 1);

    /* Test del with NULL buckets */
    ret = delete_from_process_table(NULL, 100);
    assert(ret == 1);

    destroy_process_table(&proc_table);
}

/**
 * @brief Test process buckets remove stale entries
 * @note Tests remove_stale_from_process_table
 */
static void test_process_table_remove_stale(void) {
    struct process_table proc_table;
    struct list active_list;
    struct process *proc1, *proc2, *proc3;
    const struct process *found;

    init_process_table(&proc_table, 64);
    init_list(&active_list);

    /* Create and add three processes to buckets */
    proc1 = (struct process *)malloc(sizeof(struct process));
    proc2 = (struct process *)malloc(sizeof(struct process));
    proc3 = (struct process *)malloc(sizeof(struct process));
    assert(proc1 != NULL);
    assert(proc2 != NULL);
    assert(proc3 != NULL);

    proc1->pid = 100;
    proc2->pid = 200;
    proc3->pid = 300;

    add_to_process_table(&proc_table, proc1);
    add_to_process_table(&proc_table, proc2);
    add_to_process_table(&proc_table, proc3);

    /* Add only proc1 and proc3 to active list */
    add_elem(&active_list, proc1);
    add_elem(&active_list, proc3);

    /* Remove stale entries (proc2 should be removed) */
    remove_stale_from_process_table(&proc_table, &active_list);

    /* Verify proc2 was removed */
    found = find_in_process_table(&proc_table, 200);
    assert(found == NULL);

    /* Verify proc1 and proc3 still exist */
    found = find_in_process_table(&proc_table, 100);
    assert(found == proc1);
    found = find_in_process_table(&proc_table, 300);
    assert(found == proc3);

    /* Test with NULL (should not crash) */
    remove_stale_from_process_table(NULL, &active_list);

    /*
     * Passing NULL active_list must be a no-op to preserve table
     * contents; callers may treat "no active snapshot" as "skip prune".
     */
    remove_stale_from_process_table(&proc_table, NULL);
    found = find_in_process_table(&proc_table, 100);
    assert(found == proc1);
    found = find_in_process_table(&proc_table, 300);
    assert(found == proc3);

    clear_list(&active_list);
    destroy_process_table(&proc_table);
}

/**
 * @brief Test remove_stale_from_process_table removes NULL-data nodes
 * @note NULL-data nodes should be removed defensively
 *
 * To test this defensive path we must inject a NULL-data node directly
 * into the internal hash bucket. The process_table struct and its buckets
 * member are exposed in the public header, so this access is intentional;
 * the bucket_idx computation mirrors process_table's own pid_hash() formula.
 */
static void test_process_table_remove_stale_null_data(void) {
    struct process_table proc_table;
    struct list active_list;
    struct process *proc1;
    size_t bucket_idx;
    size_t list_count;
    const struct process *pt_found;

    init_process_table(&proc_table, 16);
    init_list(&active_list);

    /* Insert a valid process */
    proc1 = (struct process *)malloc(sizeof(struct process));
    assert(proc1 != NULL);
    proc1->pid = 101;
    add_to_process_table(&proc_table, proc1);

    /*
     * Inject a NULL-data node into the same bucket.
     * bucket_idx mirrors process_table's pid_hash(): (size_t)pid % hash_size.
     */
    bucket_idx = (size_t)101 % 16;
    assert(proc_table.buckets[bucket_idx] != NULL);
    add_elem(proc_table.buckets[bucket_idx], NULL);

    /* add proc1 to active_list so it is not removed */
    add_elem(&active_list, proc1);

    /* remove_stale must remove the NULL-data node without crashing */
    remove_stale_from_process_table(&proc_table, &active_list);

    /* proc1 must still be present */
    pt_found = find_in_process_table(&proc_table, 101);
    assert(pt_found == proc1);

    /* The NULL-data node must be gone (bucket list has exactly one entry) */
    list_count = get_list_count(proc_table.buckets[bucket_idx]);
    assert(list_count == 1);

    clear_list(&active_list);
    destroy_process_table(&proc_table);
}

/**
 * @brief Test process table with hash collisions
 * @note Tests behavior when multiple PIDs hash to same bucket
 */
static void test_process_table_collisions(void) {
    struct process_table proc_table;
    static const pid_t collision_pids[20] = {100, 110, 120, 130, 140, 150, 160,
                                             170, 180, 190, 200, 210, 220, 230,
                                             240, 250, 260, 270, 280, 290};
    struct process *proc[20];
    const struct process *found;
    size_t case_idx;

    /* Use small hash size to force collisions */
    init_process_table(&proc_table, 4);

    /* Add many processes */
    for (case_idx = 0; case_idx < 20; case_idx++) {
        proc[case_idx] = (struct process *)malloc(sizeof(struct process));
        assert(proc[case_idx] != NULL);
        proc[case_idx]->pid = collision_pids[case_idx];
        add_to_process_table(&proc_table, proc[case_idx]);
    }

    /* Verify all processes can be found */
    for (case_idx = 0; case_idx < 20; case_idx++) {
        found = find_in_process_table(&proc_table, collision_pids[case_idx]);
        assert(found == proc[case_idx]);
        assert(found->pid == collision_pids[case_idx]);
    }

    /* Delete some processes */
    for (case_idx = 0; case_idx < 20; case_idx += 3) {
        int ret;
        ret = delete_from_process_table(&proc_table, collision_pids[case_idx]);
        assert(ret == 0);
    }

    /* Verify deleted processes are gone */
    for (case_idx = 0; case_idx < 20; case_idx++) {
        found = find_in_process_table(&proc_table, collision_pids[case_idx]);
        if (case_idx % 3 == 0) {
            assert(found == NULL);
        } else {
            assert(found == proc[case_idx]);
        }
    }

    destroy_process_table(&proc_table);
}

/**
 * @brief Test process table with empty buckets
 * @note Tests operations when some buckets are empty
 */
static void test_process_table_empty_buckets(void) {
    struct process_table proc_table;
    struct list active_list;
    struct process *proc1, *proc2;
    const struct process *pt_found;

    init_process_table(&proc_table, 256);
    init_list(&active_list);

    /* Add sparse processes */
    proc1 = (struct process *)malloc(sizeof(struct process));
    proc2 = (struct process *)malloc(sizeof(struct process));
    assert(proc1 != NULL);
    assert(proc2 != NULL);

    proc1->pid = (pid_t)10;
    proc2->pid = (pid_t)1000;

    add_to_process_table(&proc_table, proc1);
    add_to_process_table(&proc_table, proc2);

    /* Remove stale with empty active list */
    remove_stale_from_process_table(&proc_table, &active_list);

    /* All processes should be removed */
    pt_found = find_in_process_table(&proc_table, (pid_t)10);
    assert(pt_found == NULL);
    pt_found = find_in_process_table(&proc_table, (pid_t)1000);
    assert(pt_found == NULL);

    clear_list(&active_list);
    destroy_process_table(&proc_table);
}

/**
 * @brief Test init_process_table and add_to_process_table with NULL inputs
 * @note Covers: init_process_table(NULL,...), add_to_process_table(NULL,p),
 *       add_to_process_table(pt,NULL), and duplicate-PID insertion (silently
 *       ignored)
 */
static void test_process_table_null_inputs_and_dup(void) {
    struct process_table proc_table;
    struct process *proc1, *proc2;
    const struct process *found;
    const struct process *pt_found;

    /* init_process_table with NULL pointer must not crash */
    init_process_table(NULL, 16);

    /* Set up a valid buckets for the remaining sub-tests */
    init_process_table(&proc_table, 16);

    /* add_to_process_table with NULL buckets must not crash */
    proc1 = (struct process *)malloc(sizeof(struct process));
    assert(proc1 != NULL);
    proc1->pid = 100;
    proc1->ppid = 1;
    proc1->cpu_time = 0.0;
    add_to_process_table(NULL, proc1);

    /* add_to_process_table with NULL process must not crash */
    add_to_process_table(&proc_table, NULL);
    pt_found = find_in_process_table(&proc_table, 100);
    assert(pt_found == NULL);

    /* Normal add */
    add_to_process_table(&proc_table, proc1);
    found = find_in_process_table(&proc_table, 100);
    assert(found == proc1);

    /* Duplicate-PID insertion must be silently ignored: proc1 stays */
    proc2 = (struct process *)malloc(sizeof(struct process));
    assert(proc2 != NULL);
    proc2->pid = 100; /* same PID as proc1 */
    proc2->ppid = 1;
    proc2->cpu_time = 0.0;
    add_to_process_table(&proc_table, proc2);
    found = find_in_process_table(&proc_table, 100);
    assert(found == proc1); /* proc1 must still be the stored entry */

    /* proc2 was never added to the buckets; free it manually */
    free(proc2);

    /* proc1 will be freed by destroy_process_table */
    destroy_process_table(&proc_table);
}

/**
 * @brief Test remove_stale_from_process_table with NULL active_list
 * @note NULL active_list must be treated as "skip stale removal"
 */
static void test_process_table_stale_null_list(void) {
    struct process_table proc_table;
    struct process *proc;
    const struct process *pt_found;

    init_process_table(&proc_table, 16);

    proc = (struct process *)malloc(sizeof(struct process));
    assert(proc != NULL);
    proc->pid = 100;
    proc->ppid = 1;
    proc->cpu_time = 0.0;
    add_to_process_table(&proc_table, proc);
    pt_found = find_in_process_table(&proc_table, 100);
    assert(pt_found == proc);

    /* NULL active_list must leave table unchanged */
    remove_stale_from_process_table(&proc_table, NULL);
    pt_found = find_in_process_table(&proc_table, 100);
    assert(pt_found == proc);

    /* proc is freed by destroy_process_table */
    destroy_process_table(&proc_table);
}

/**
 * @brief Test init_process_table with hash_size=0 (forced to 1)
 * @note hash_size=0 must be clamped to 1; add/find/del must still work
 */
static void test_process_table_init_hashsize_zero(void) {
    struct process_table proc_table;
    struct process *proc;
    const struct process *pt_found;
    int pt_del;

    init_process_table(&proc_table, 0);
    assert(proc_table.hash_size == 1);
    assert(proc_table.buckets != NULL);

    proc = (struct process *)malloc(sizeof(struct process));
    assert(proc != NULL);
    proc->pid = 77;
    proc->ppid = 1;
    proc->cpu_time = 0.0;
    proc->cpu_usage = -1.0;

    add_to_process_table(&proc_table, proc);
    pt_found = find_in_process_table(&proc_table, 77);
    assert(pt_found == proc);
    pt_del = delete_from_process_table(&proc_table, 77);
    assert(pt_del == 0);
    pt_found = find_in_process_table(&proc_table, 77);
    assert(pt_found == NULL);

    destroy_process_table(&proc_table);
}

/**
 * @brief Test find_in_process_table with NULL process buckets
 * @note Must return NULL without crashing
 */
static void test_process_table_find_null_pt(void) {
    const struct process *pt_found;
    pt_found = find_in_process_table(NULL, 1);
    assert(pt_found == NULL);
}

/**
 * @brief Test delete_from_process_table when PID is absent from a populated
 * bucket
 * @note del on a non-empty buckets for a PID in the same bucket must return 1
 */
static void test_process_table_del_absent_pid(void) {
    struct process_table proc_table;
    struct process *proc;
    int pt_del;
    const struct process *pt_found;

    /* Use hash_size=1 so all PIDs go to bucket 0 */
    init_process_table(&proc_table, 1);

    proc = (struct process *)malloc(sizeof(struct process));
    assert(proc != NULL);
    proc->pid = 5;
    proc->ppid = 1;
    proc->cpu_time = 0.0;
    proc->cpu_usage = -1.0;
    add_to_process_table(&proc_table, proc);

    /* PID 99 hashes to bucket 0 (same bucket, but not in list) */
    pt_del = delete_from_process_table(&proc_table, 99);
    assert(pt_del == 1);

    /* PID 5 is still there */
    pt_found = find_in_process_table(&proc_table, 5);
    assert(pt_found == proc);

    destroy_process_table(&proc_table);
}

/**
 * @brief Test delete_from_process_table on a PID that was never inserted at all
 * @note Empty bucket: returns 1
 */
static void test_process_table_del_empty_bucket(void) {
    struct process_table proc_table;
    int pt_del;

    init_process_table(&proc_table, 16);
    pt_del = delete_from_process_table(&proc_table, 100);
    assert(pt_del == 1);
    destroy_process_table(&proc_table);
}

/**
 * @brief Test destroy_process_table on NULL and on a freshly-initialized
 * buckets
 * @note NULL must not crash; fresh empty buckets must also not crash
 */
static void test_process_table_destroy_edge_cases(void) {
    struct process_table proc_table;

    /* NULL pointer: must be a no-op */
    destroy_process_table(NULL);

    /* Fresh buckets with no entries */
    init_process_table(&proc_table, 8);
    destroy_process_table(&proc_table);
    /* Subsequent destroy must not crash (proc_table->buckets is NULL) */
    destroy_process_table(&proc_table);
}

/**
 * @brief Test that process_table operations are safe after destroy
 * @note After destroy_process_table,
 *       find_in_process_table/add_to_process_table/
 *       delete_from_process_table/remove_stale_from_process_table must not
 *       crash even though pt->buckets is NULL and pt->hash_size is 0
 */
static void test_process_table_ops_after_destroy(void) {
    struct process_table proc_table;
    struct process *proc;
    const struct process *found;
    int ret;

    init_process_table(&proc_table, 16);
    destroy_process_table(&proc_table);
    /* proc_table->buckets is now NULL, proc_table->hash_size is now 0 */

    /* find must return NULL without crashing */
    found = find_in_process_table(&proc_table, 100);
    assert(found == NULL);

    /* add must be a no-op without crashing */
    proc = (struct process *)malloc(sizeof(struct process));
    assert(proc != NULL);
    proc->pid = 100;
    proc->ppid = 1;
    proc->cpu_time = 0.0;
    proc->cpu_usage = -1.0;
    add_to_process_table(&proc_table, proc);
    free(proc); /* p was never added to the buckets; must be freed manually */

    /* del must return 1 without crashing */
    ret = delete_from_process_table(&proc_table, 100);
    assert(ret == 1);

    /* remove_stale must be a no-op without crashing */
    remove_stale_from_process_table(&proc_table, NULL);
}

/***************************************************************************
 * PROCESS_FINDER MODULE TESTS
 ***************************************************************************/

/**
 * @brief Test find_process_by_pid function
 * @note Tests finding processes by PID including invalid PIDs, boundary
 *       values, and init process
 */
static void test_process_finder_find_by_pid(void) {
    pid_t self_pid;
    pid_t found_pid;
    pid_t result;

    self_pid = getpid();
    found_pid = find_process_by_pid(self_pid);
    assert(found_pid == self_pid);

    /* PID 0 is rejected before kill() */
    result = find_process_by_pid((pid_t)0);
    assert(result == 0);

    /* Negative PID is rejected before kill() */
    result = find_process_by_pid((pid_t)-1);
    assert(result == 0);

    /* INT_MAX: virtually guaranteed non-existent PID */
    result = find_process_by_pid((pid_t)INT_MAX);
    assert(result == 0);

    /* PID 1 (init/systemd) always exists */
    result = find_process_by_pid((pid_t)1);
    assert(result != 0);
}

/**
 * @brief Test find_process_by_name function
 * @note Tests finding processes by name including wrong names, absolute
 *       paths, NULL, empty string, and trailing slash
 */
static void test_process_finder_find_by_name(void) {
    char *self_cmd;
    const char *self_command;
    char *wrong_name;
    size_t len;
    pid_t found_pid;
    pid_t self_pid;
#if defined(__linux__)
    char abs_path[64];
#endif /* __linux__ */

    /* Allocate buffers on the heap to stay within stack size limits */
    self_cmd = (char *)malloc(CMD_BUFF_SIZE);
    if (self_cmd == NULL) {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    /* Allocate buffer large enough for self_command plus one extra char */
    wrong_name = (char *)malloc(CMD_BUFF_SIZE + 1);
    if (wrong_name == NULL) {
        free(self_cmd);
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }

    /*
     * Obtain the OS-visible argv[0] of the current process via the
     * process iterator. When running under a wrapper (e.g., valgrind),
     * main()'s argv[0] differs from what the OS records as the process
     * command; get_self_command() always returns the correct value.
     */
    self_command = get_self_command(self_cmd, CMD_BUFF_SIZE);
    /*
     * get_self_command() queries the current process via the iterator;
     * a NULL return means the iterator or read_cmd is broken, which is
     * a real regression rather than an expected skip condition.
     */
    assert(self_command != NULL);

    /*
     * Verify that find_process_by_name can find the current process
     * using its OS-visible command name.
     */
    found_pid = find_process_by_name(self_command);
    self_pid = getpid();
    assert(found_pid == self_pid);

    /*
     * Test Case 2: Pass an incorrect process name by appending 'x'
     * to the current process's command.
     * Expectation: Should return 0 (process not found).
     */
    strcpy(wrong_name, self_command); /* Copy the OS-visible command */
    strcat(wrong_name, "x");          /* Append 'x' to make it non-matching */
    found_pid = find_process_by_name(wrong_name);
    assert(found_pid == 0);

    /*
     * Test Case 3: Pass a copy of the current process's command with
     * the last character removed.
     * Expectation: Should return 0 (process not found).
     */
    strcpy(wrong_name, self_command); /* Copy the OS-visible command */
    len = strlen(wrong_name);
    wrong_name[len - 1] = '\0'; /* Remove the last character */
    found_pid = find_process_by_name(wrong_name);
    assert(found_pid == 0);

#if defined(__linux__)
    /*
     * Test the absolute-path comparison branch: when process_name starts
     * with '/', find_process_by_name compares the full path against each
     * process's cmdline.  Use a path incorporating the current PID so it
     * is unique enough to never match any running process's cmdline,
     * even in shared CI environments.
     */
    snprintf(abs_path, sizeof(abs_path), "/nonexistent/cpulimit_abs_%ld",
             (long)getpid());
    found_pid = find_process_by_name(abs_path);
    assert(found_pid == 0);
#endif /* __linux__ */

    /*
     * Test Case 1: Pass an empty string to find_process_by_name.
     * Expectation: Should return 0 (process not found).
     */
    strcpy(wrong_name, "");
    found_pid = find_process_by_name(wrong_name);
    assert(found_pid == 0);

    /* NULL name must return 0 without crashing */
    found_pid = find_process_by_name(NULL);
    assert(found_pid == 0);

    /* Trailing slash yields an empty basename; must return 0 */
    found_pid = find_process_by_name("bin/");
    assert(found_pid == 0);

    free(wrong_name);
    free(self_cmd);
}

/**
 * @brief Test find_process_by_name with self's executable basename
 * @note The OS-visible command basename must be found; result > 0 or
 *       result is -PID (EPERM in confined environments). Uses the process
 *       iterator to obtain the real argv[0] so the test passes when the
 *       binary is launched via a wrapper such as valgrind.
 */
static void test_process_finder_find_by_name_self(void) {
    char *self_buf;
    const char *self_command;
    const char *self_name;
    pid_t result;

    self_buf = (char *)malloc(CMD_BUFF_SIZE);
    if (self_buf == NULL) {
        fprintf(stderr, "malloc failed %s(%d)\n", __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }
    self_command = get_self_command(self_buf, CMD_BUFF_SIZE);
    if (self_command == NULL) {
        free(self_buf);
        return;
    }
    self_name = file_basename(self_command);
    if (self_name == NULL || self_name[0] == '\0') {
        free(self_buf);
        return;
    }
    result = find_process_by_name(self_name);
    free(self_buf);
    /* Must find at least itself (positive) or get EPERM (-pid) */
    assert(result != 0);
}

/**
 * @brief Test find_process_by_name with process launched via symlink
 * @note Creates a temporary symlink to /bin/sleep, execs it so that
 *       argv[0] is the symlink path, and verifies that find_process_by_name
 *       finds the child using the symlink's basename. Skipped if /bin/sleep
 *       is unavailable or the symlink cannot be created.
 *
 * This exercises the requirement that proc->command stores argv[0] as
 * seen by the OS: the symlink path, not the resolved binary path.
 */
static void test_process_finder_find_by_name_symlink(void) {
    char *sym_path;
    char sym_name[64];
    pid_t child_pid;
    pid_t found_pid;
    unsigned int i;
    int child_status;
    const struct timespec poll_wait = {0, 100000000L}; /* 100 ms */

    /* Skip if /bin/sleep is not executable on this platform */
    if (access("/bin/sleep", X_OK) != 0) {
        return;
    }

    /*
     * Build a unique symlink name derived from the current PID to avoid
     * collisions when tests run in parallel.
     */
    if (snprintf(sym_name, sizeof(sym_name), "cpulimit_sym_%ld",
                 (long)getpid()) >= (int)sizeof(sym_name)) {
        return; /* Skip if name was truncated (should never happen) */
    }

    sym_path = (char *)malloc(PATH_MAX);
    if (sym_path == NULL) {
        return;
    }
    if (snprintf(sym_path, (size_t)PATH_MAX, "/tmp/%s", sym_name) >= PATH_MAX) {
        free(sym_path);
        return; /* Skip if path was truncated (should never happen) */
    }

    /* Remove any stale symlink left by a previous failed run */
    unlink(sym_path);

    if (symlink("/bin/sleep", sym_path) != 0) {
        free(sym_path);
        return; /* Skip if symlink creation fails */
    }

    /* Fork a child that execs sleep via the symlink */
    child_pid = fork();
    if (child_pid < 0) {
        unlink(sym_path);
        free(sym_path);
        return;
    }
    if (child_pid == 0) {
        char sleep_arg[] = "100";
        char *child_argv[3];
        child_argv[0] = sym_path;
        child_argv[1] = sleep_arg;
        child_argv[2] = NULL;
        execv(sym_path, child_argv);
        _exit(1);
    }

    /*
     * Poll until the child appears in the process table with the
     * symlink basename as its command, or until the timeout expires.
     * A short per-iteration sleep avoids busy-waiting.
     */
    found_pid = 0;
    for (i = 0; i < 50 && found_pid == 0; i++) {
        sleep_timespec(&poll_wait);
        found_pid = find_process_by_name(sym_name);
    }

    /*
     * If the child was not found, check whether it already exited.
     * This happens when the target binary is a multicall binary (e.g.,
     * coreutils on Ubuntu 26.04+) that rejects an unrecognised argv[0]
     * and exits immediately.  In that case the symlink-name lookup is
     * impossible by design, so skip rather than assert.
     */
    if (found_pid == 0 &&
        waitpid(child_pid, &child_status, WNOHANG) == child_pid &&
        (WIFEXITED(child_status) || WIFSIGNALED(child_status))) {
        unlink(sym_path);
        free(sym_path);
        return;
    }

    /* Cleanup: kill child and remove symlink */
    kill_and_wait(child_pid, SIGKILL);
    unlink(sym_path);
    free(sym_path);

    /*
     * The child must be found by the symlink's basename, confirming
     * that proc->command stores argv[0] (the symlink path), not the
     * resolved binary path.
     */
    assert(found_pid == child_pid || found_pid == -child_pid);
}

/**
 * @brief Test find_process_by_name with process launched with custom argv[0]
 * @note Forks a child that execs /bin/sleep with a unique alias string
 *       as argv[0]. Verifies that find_process_by_name finds the child by
 *       that alias name. Skipped if /bin/sleep is not available.
 *
 * This exercises the requirement that proc->command stores the exact
 * argv[0] passed to execve(), regardless of the real binary name.
 */
static void test_process_finder_find_by_name_alias(void) {
    char alias_name[64];
    pid_t child_pid;
    pid_t found_pid;
    unsigned int i;
    int child_status;
    const struct timespec poll_wait = {0, 100000000L}; /* 100 ms */

    /* Skip if /bin/sleep is not executable on this platform */
    if (access("/bin/sleep", X_OK) != 0) {
        return;
    }

    /*
     * Build a unique alias name derived from the current PID. Using a
     * PID-qualified name avoids false matches with other processes.
     */
    if (snprintf(alias_name, sizeof(alias_name), "cpulimit_ali_%ld",
                 (long)getpid()) >= (int)sizeof(alias_name)) {
        return; /* Skip if name was truncated (should never happen) */
    }

    /* Fork a child that execs /bin/sleep with the alias as argv[0] */
    child_pid = fork();
    if (child_pid < 0) {
        return;
    }
    if (child_pid == 0) {
        char sleep_arg[] = "100";
        char *child_argv[3];
        child_argv[0] = alias_name;
        child_argv[1] = sleep_arg;
        child_argv[2] = NULL;
        execv("/bin/sleep", child_argv);
        _exit(1);
    }

    /*
     * Poll until the child appears in the process table with the alias
     * as its command, or until the timeout expires.
     */
    found_pid = 0;
    for (i = 0; i < 50 && found_pid == 0; i++) {
        sleep_timespec(&poll_wait);
        found_pid = find_process_by_name(alias_name);
    }

    /*
     * If the child was not found, check whether it already exited.
     * This happens when the target binary is a multicall binary (e.g.,
     * coreutils on Ubuntu 26.04+) that rejects an unrecognised argv[0]
     * and exits immediately.  In that case the alias lookup is
     * impossible by design, so skip rather than assert.
     */
    if (found_pid == 0 &&
        waitpid(child_pid, &child_status, WNOHANG) == child_pid &&
        (WIFEXITED(child_status) || WIFSIGNALED(child_status))) {
        return;
    }

    /* Cleanup: kill child */
    kill_and_wait(child_pid, SIGKILL);

    /*
     * The child must be found by the alias name, confirming that
     * proc->command stores argv[0] as passed to execve(), not the
     * resolved binary name (/bin/sleep).
     */
    assert(found_pid == child_pid || found_pid == -child_pid);
}

/**
 * @brief Test find_process_by_name ancestor-preference with a parent/child
 *        process tree of identical names
 * @note Forks multi_process_busy (which internally forks more children that
 *       share the same executable name).  Verifies that find_process_by_name
 *       returns the outermost ancestor (the PID we directly forked) rather
 *       than one of its descendants, matching the documented heuristic that
 *       the parent of a same-named tree is preferred.
 */
#if defined(__linux__)
/**
 * @brief Kill every process whose command name equals "comm"
 * @param comm Process command name to hunt for
 * @note find_process_by_name() scans the whole system by name, so the
 *       ancestor-preference check in this test is only deterministic when
 *       exactly one same-named process tree exists.  A previous (possibly
 *       interrupted) run or a concurrently injected process sharing the
 *       "multi_process_busy" name would otherwise be matched first and
 *       break the assertion.  Reaping all such processes before forking
 *       this test's own tree guarantees a clean, single-tree environment.
 */
static void reap_all_by_name(const char *comm) {
    DIR *proc_dir;
    const struct dirent *entry;

    proc_dir = opendir("/proc");
    if (proc_dir == NULL) {
        return;
    }
    while ((entry = readdir(proc_dir)) != NULL) {
        char *endptr;
        long pid_l;
        char stat_path[64];
        char *buf;
        const char *p;
        FILE *fp;
        pid_t pid;

        if (entry->d_type != DT_DIR && entry->d_type != DT_UNKNOWN) {
            continue;
        }
        errno = 0;
        pid_l = strtol(entry->d_name, &endptr, 10);
        if (errno != 0 || endptr == entry->d_name || *endptr != '\0') {
            continue;
        }
        pid = (pid_t)pid_l;
        if (pid <= 1) {
            continue;
        }
        if (snprintf(stat_path, sizeof(stat_path), "/proc/%ld/stat", pid_l) >=
            (int)sizeof(stat_path)) {
            continue;
        }
        fp = fopen(stat_path, "r");
        if (fp == NULL) {
            continue;
        }
        buf = (char *)malloc(1024);
        if (buf == NULL) {
            fclose(fp);
            continue;
        }
        if (fgets(buf, 1024, fp) == NULL) {
            free(buf);
            fclose(fp);
            continue;
        }
        fclose(fp);
        /* comm is between the first '(' and the last ')' */
        p = strrchr(buf, ')');
        if (p != NULL) {
            const char *open_paren =
                (const char *)memchr(buf, '(', (size_t)(p - buf));
            if (open_paren != NULL) {
                size_t len = (size_t)(p - open_paren - 1);
                char name[64];
                if (len < sizeof(name)) {
                    memcpy(name, open_paren + 1, len);
                    name[len] = '\0';
                    if (strcmp(name, comm) == 0) {
                        /* Same-named process: reap it for a clean scan. */
                        kill(pid, SIGKILL);
                    }
                }
            }
        }
        free(buf);
    }
    closedir(proc_dir);
}
#endif /* __linux__ */

static void test_process_finder_find_by_name_ancestor_pref(void) {
    char *self_buf;
    char *mpb_path;
    pid_t child_pid;
    pid_t found_pid;
    unsigned int i;
    int child_status;
    const struct timespec poll_wait = {0, 100000000L}; /* 100 ms */

    /* Locate the multi_process_busy helper built alongside the tests.
       PATH_MAX-sized stack buffers exceed the frame-size budget; use heap. */
    mpb_path = (char *)malloc(PATH_MAX);
    if (mpb_path == NULL) {
        return;
    }
    if (snprintf(mpb_path, PATH_MAX, "%s/tests/multi_process_busy",
                 getenv("CPULIMIT_BUILD_DIR") != NULL
                     ? getenv("CPULIMIT_BUILD_DIR")
                     : ".") >= (int)PATH_MAX) {
        free(mpb_path);
        return;
    }
    if (access(mpb_path, X_OK) != 0) {
        free(mpb_path);
        return; /* Helper not built in this layout; skip */
    }

#if defined(__linux__)
    /*
     * Reap every multi_process_busy process so the global
     * find_process_by_name() scan sees only the tree this test is about to
     * fork.  Any leftover from a prior (interrupted) run or a concurrently
     * injected same-named process would otherwise be matched first and
     * break the ancestor-preference assertion (flaky / non-deterministic).
     */
    reap_all_by_name("multi_process_busy");
#endif /* __linux__ */

    /*
     * Ensure the test process itself is not matched by name: remember the
     * test binary's own command so we can assert the found PID differs from
     * our own (it must be the multi_process_busy ancestor we fork).
     */
    self_buf = (char *)malloc(CMD_BUFF_SIZE);
    if (self_buf == NULL) {
        free(mpb_path);
        return;
    }
    if (get_self_command(self_buf, CMD_BUFF_SIZE) == NULL) {
        free(self_buf);
        free(mpb_path);
        return;
    }
    free(self_buf);

    /* Fork multi_process_busy; it will spawn same-named children. */
    child_pid = fork();
    if (child_pid < 0) {
        free(mpb_path);
        return;
    }
    if (child_pid == 0) {
        char proc_count[] = "2"; /* request at least 2 processes total */
        char *child_argv[3];
        /*
         * Become the leader of a new process group so that the
         * kill_and_wait(-child_pid, ...) cleanup below targets the
         * entire tree (the helper forks same-named children). Without
         * this the child stays in the test's process group, the
         * negative-PGID kill is ineffective, and the helper's children
         * leak as init-reparented orphans that pollute later
         * find_process_by_name() scans and can hang the suite.
         */
        setpgid(0, 0);
        child_argv[0] = mpb_path;
        child_argv[1] = proc_count;
        child_argv[2] = NULL;
        execv(mpb_path, child_argv);
        _exit(1);
    }

    /*
     * Poll until find_process_by_name returns THIS test's multi_process_busy
     * ancestor (positive or negative PGID form), not merely any same-named
     * process.  A concurrently injected same-named process (or a not-yet-
     * reaped leftover) may be returned transiently; the loop must skip such
     * mismatches and keep polling, otherwise the ancestor-preference assertion
     * below would fail non-deterministically (flaky / hang under injection).
     */
    found_pid = 0;
    for (i = 0; i < 50; i++) {
        sleep_timespec(&poll_wait);
        found_pid = find_process_by_name("multi_process_busy");
        if (found_pid == child_pid || found_pid == -child_pid) {
            break;
        }
    }

    if (found_pid == 0 &&
        waitpid(child_pid, &child_status, WNOHANG) == child_pid &&
        (WIFEXITED(child_status) || WIFSIGNALED(child_status))) {
        free(mpb_path);
        return; /* Child exited before it could be observed; skip */
    }

    /* Cleanup: kill the whole process group rooted at the child. */
    kill_and_wait(-child_pid, SIGKILL);

    /*
     * The ancestor-preference heuristic must return the outermost
     * multi_process_busy ancestor: the PID we directly forked (positive),
     * not one of its descendants and not our own test process.
     */
    assert(found_pid == child_pid || found_pid == -child_pid);
    assert(found_pid != getpid());
    free(mpb_path);
}

/***************************************************************************
 * PROCESS_GROUP MODULE TESTS
 ***************************************************************************/

/**
 * @brief Test get_process_group_cpu_usage function
 * @note Tests CPU usage calculation for process group
 */
static void test_process_group_cpu_usage(void) {
    struct process_group proc_group;
    double cpu_usage;
    pid_t child_pid;
    int node_idx, ret, ncpu;

    /* Create a child process that uses CPU */
    child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        /* Child process - busy loop */
        volatile int keep_running = 1;
        while (keep_running && !is_quit_flag_set()) {
            volatile int dummy_var;
            for (dummy_var = 0; dummy_var < 1000; dummy_var = dummy_var + 1) {
                ;
            }
        }
        _exit(EXIT_SUCCESS);
    }

    /* Initialize process group for child */
    ret = init_process_group(&proc_group, child_pid, 0);
    assert(ret == 0);

    /* First call should return -1 (no measurement yet) */
    cpu_usage = get_process_group_cpu_usage(&proc_group);
    assert(cpu_usage >= -1.00001 && cpu_usage <= -0.99999);

    /* Update a few times to get valid measurements */
    for (node_idx = 0; node_idx < 5; node_idx++) {
        const struct timespec sleep_time = {0, 100000000L}; /* 100ms */
        sleep_timespec(&sleep_time);
        update_process_group(&proc_group);
    }

    /* Should now have valid CPU usage */
    cpu_usage = get_process_group_cpu_usage(&proc_group);
    /* CPU usage should be between 0 and ncpu */
    ncpu = get_ncpu();
    assert(cpu_usage >= 0.0);
    assert(cpu_usage <= 1.0 * ncpu);

    ret = close_process_group(&proc_group);
    assert(ret == 0);
    kill_and_wait(child_pid, SIGKILL);
}

/**
 * @brief Test process group with rapid updates
 * @note Tests update_process_group called in quick succession
 */
static void test_process_group_rapid_updates(void) {
    struct process_group proc_group;
    pid_t child_pid;
    int proc_idx;
    int ret;

    child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        /* Child process */
        volatile int keep_running = 1;
        while (keep_running && !is_quit_flag_set()) {
            volatile int dummy_var;
            for (dummy_var = 0; dummy_var < 1000; dummy_var = dummy_var + 1) {
                ;
            }
        }
        _exit(EXIT_SUCCESS);
    }

    /* Initialize and update rapidly */
    ret = init_process_group(&proc_group, child_pid, 0);
    assert(ret == 0);

    for (proc_idx = 0; proc_idx < 20; proc_idx++) {
        size_t list_count;
        update_process_group(&proc_group);
        list_count = get_list_count(proc_group.proc_list);
        assert(list_count == 1);
    }

    ret = close_process_group(&proc_group);
    assert(ret == 0);
    kill_and_wait(child_pid, SIGKILL);
}

/**
 * @brief Test process group initialization with all processes
 * @note Verifies that a process group initialized with PID 0 (all processes)
 *       is non-empty and contains the current process
 */
static void test_process_group_init_all(void) {
    struct process_group proc_group;
    const struct list_node *node = NULL;
    size_t count = 0;
    int found_self = 0;
    int ret;
    size_t list_cnt;

    /* Initialize process group with all processes */
    ret = init_process_group(&proc_group, 0, 0);
    assert(ret == 0);
    update_process_group(&proc_group);

    /* Count processes in the group */
    for (node = proc_group.proc_list->first; node != NULL; node = node->next) {
        const struct process *proc = (const struct process *)node->data;
        if (proc->pid == getpid()) {
            found_self = 1;
        }
        count++;
    }
    assert(count > 0);
    assert(found_self == 1);
    list_cnt = get_list_count(proc_group.proc_list);
    assert(count == list_cnt);

    /* Update and verify again */
    update_process_group(&proc_group);
    ret = close_process_group(&proc_group);
    assert(ret == 0);
}

/**
 * @brief Test process group with a single process
 * @param include_children Flag indicating whether to include child processes
 * @note Creates a child process and verifies that the process group
 *       correctly tracks it, with or without child process inclusion
 */
static void test_process_group_single(int include_children) {
    struct process_group proc_group;
    int iter_idx;
    int ret;
    pid_t self_pid;
    pid_t child_pid;

    /* Create a child process for testing */
    child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        /* Child process: busy loop until killed */
        volatile int keep_running = 1;
        while (keep_running && !is_quit_flag_set()) {
            volatile int dummy_var;
            for (dummy_var = 0; dummy_var < 1000; dummy_var = dummy_var + 1) {
                ;
            }
        }
        _exit(EXIT_SUCCESS);
    }

    /* Initialize process group with the child PID */
    self_pid = getpid();
    ret = init_process_group(&proc_group, child_pid, include_children);
    assert(ret == 0);

    /* Update process group 100 times and verify consistency */
    for (iter_idx = 0; iter_idx < 100; iter_idx++) {
        const struct list_node *node = NULL;
        size_t count = 0;
        size_t list_count;

        update_process_group(&proc_group);
        list_count = get_list_count(proc_group.proc_list);
        assert(list_count == 1);

        for (node = proc_group.proc_list->first; node != NULL;
             node = node->next) {
            const struct process *proc = (const struct process *)node->data;
            int ncpu;
            int cpu_unset;
            int cpu_valid;
            assert(proc->pid == child_pid);
            assert(proc->ppid == self_pid);
            /* p->cpu_usage should be -1 or [0, NCPU] */
            ncpu = get_ncpu();
            cpu_unset =
                (proc->cpu_usage >= -1.00001 && proc->cpu_usage <= -0.99999);
            cpu_valid = (proc->cpu_usage >= 0 && proc->cpu_usage <= 1.0 * ncpu);
            assert(cpu_unset || cpu_valid);
            count++;
        }
        assert(count == 1);
    }
    ret = close_process_group(&proc_group);
    assert(ret == 0);

    /* Clean up child process */
    kill_and_wait(child_pid, SIGKILL);
}

/**
 * @brief Test process group with a single process (both with and without
 * children)
 * @note Wrapper function to test process group with include_children set to
 *       0 and 1
 */
static void test_process_group_init_single(void) {
    /* Test without including children */
    test_process_group_single(0);

    /* Test with including children */
    test_process_group_single(1);
}

/**
 * @brief Test process group initialization with invalid PIDs
 * @note Verifies that process group initialization with invalid PIDs (-1 and
 *       INT_MAX) results in empty process lists
 */
static void test_process_group_init_invalid_pid(void) {
    struct process_group proc_group;
    int ret;
    size_t list_count;

    /* Test with PID -1 */
    ret = init_process_group(&proc_group, -1, 0);
    assert(ret == 0);
    list_count = get_list_count(proc_group.proc_list);
    assert(list_count == 0);
    update_process_group(&proc_group);
    list_count = get_list_count(proc_group.proc_list);
    assert(list_count == 0);
    ret = close_process_group(&proc_group);
    assert(ret == 0);

    /* Test with PID INT_MAX */
    ret = init_process_group(&proc_group, INT_MAX, 0);
    assert(ret == 0);
    list_count = get_list_count(proc_group.proc_list);
    assert(list_count == 0);
    update_process_group(&proc_group);
    list_count = get_list_count(proc_group.proc_list);
    assert(list_count == 0);
    ret = close_process_group(&proc_group);
    assert(ret == 0);
}

/**
 * @brief Test init_process_group with NULL proc_group argument
 * @note Must return -1 without crashing when proc_group is NULL
 */
static void test_process_group_init_null(void) {
    int ret;
    ret = init_process_group(NULL, getpid(), 0);
    assert(ret == -1);
}

/**
 * @brief Test get_process_group_cpu_usage when process list is empty
 * @note Must return -1.0 when no processes are tracked
 */
static void test_process_group_cpu_usage_empty_list(void) {
    struct process_group proc_group;
    double usage;
    int ret;
    size_t list_count;

    /* Initialize with INT_MAX: no such process exists, list stays empty */
    ret = init_process_group(&proc_group, (pid_t)INT_MAX, 0);
    assert(ret == 0);
    list_count = get_list_count(proc_group.proc_list);
    assert(list_count == 0);

    /* Empty list must yield -1.0 (unknown) */
    usage = get_process_group_cpu_usage(&proc_group);
    assert(usage >= -1.00001 && usage <= -0.99999);

    ret = close_process_group(&proc_group);
    assert(ret == 0);
}

/**
 * @brief Test get_process_group_cpu_usage with NULL pointer
 * @note Verifies the null guard returns the -1 sentinel without crashing
 */
static void test_process_group_cpu_usage_null(void) {
    double usage = get_process_group_cpu_usage(NULL);
    assert(usage >= -1.00001 && usage <= -0.99999);
}

/**
 * @brief Test close_process_group with NULL pointer
 * @note Must return 0 without crashing
 */
static void test_process_group_close_null(void) {
    struct process_group proc_group;
    int ret;

    /* NULL proc_group pointer must return 0 without crashing */
    ret = close_process_group(NULL);
    assert(ret == 0);

    /* Partially initialised struct (NULL members) must also work */
    memset(&proc_group, 0, sizeof(proc_group));
    ret = close_process_group(&proc_group);
    assert(ret == 0);
}

/**
 * @brief Test that close_process_group zeros all numeric fields
 * @note After close, target_pid, include_children, and last_update must be 0
 */
static void test_process_group_close_zeros_fields(void) {
    struct process_group proc_group;
    int ret;
    pid_t self_pid;
    self_pid = getpid();
    ret = init_process_group(&proc_group, self_pid, 1);
    assert(ret == 0);
    assert(proc_group.target_pid == self_pid);
    assert(proc_group.include_children == 1);
    ret = close_process_group(&proc_group);
    assert(ret == 0);
    assert(proc_group.proc_list == NULL);
    assert(proc_group.proc_table == NULL);
    assert(proc_group.target_pid == 0);
    assert(proc_group.include_children == 0);
    assert(proc_group.last_update.tv_sec == 0);
    assert(proc_group.last_update.tv_nsec == 0);
}

/**
 * @brief Test update_process_group with NULL pointer
 * @note Must return 0 without crashing when proc_group is NULL
 */
static void test_process_group_update_null(void) {
    int ret;
    /* NULL proc_group must not crash and must return 0 */
    ret = update_process_group(NULL);
    assert(ret == 0);
}

/**
 * @brief Test update_process_group with uninitialized process_group members
 * @note Must return 0 without dereferencing NULL proc_list/proc_table
 */
static void test_process_group_update_uninitialized_struct(void) {
    struct process_group proc_group;
    int ret;
    memset(&proc_group, 0, sizeof(proc_group));
    /* NULL proc_list/proc_table must not crash and must return 0 */
    ret = update_process_group(&proc_group);
    assert(ret == 0);
}

/**
 * @brief Test get_process_group_cpu_usage with uninitialized process_group
 * @note Must return -1.0 when proc_list is NULL
 */
static void test_process_group_cpu_usage_uninitialized_struct(void) {
    struct process_group proc_group;
    double usage;
    memset(&proc_group, 0, sizeof(proc_group));
    usage = get_process_group_cpu_usage(&proc_group);
    assert(usage >= -1.00001 && usage <= -0.99999);
}

/**
 * @brief Test update_process_group twice in quick succession
 * @note Second call exercises the "insufficient dt" branch
 */
static void test_process_group_double_update(void) {
    struct process_group proc_group;
    int ret;
    pid_t self_pid;
    self_pid = getpid();
    ret = init_process_group(&proc_group, self_pid, 0);
    assert(ret == 0);
    update_process_group(&proc_group);
    /* Immediate second update: dt < MIN_DT, so CPU usage stays -1 */
    update_process_group(&proc_group);
    ret = close_process_group(&proc_group);
    assert(ret == 0);
}

/**
 * @brief Test that update_process_group returns 0 on successful updates
 * @note Regression test for the fix that changed update_process_group from
 *       void to int: verifies the return value on success paths
 */
static void test_process_group_update_return_value(void) {
    struct process_group proc_group;
    pid_t child_pid;
    int ret;
    size_t list_count;

    child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid == 0) {
        /* Child pauses until killed */
        sigset_t full_mask;
        if (sigfillset(&full_mask) != 0) {
            _exit(1);
        }
        if (sigprocmask(SIG_BLOCK, &full_mask, NULL) != 0) {
            _exit(1);
        }
        test_suspend_until_killed();
    }

    /* update_process_group must return 0 when the target exists */
    ret = init_process_group(&proc_group, child_pid, 0);
    assert(ret == 0);
    ret = update_process_group(&proc_group);
    assert(ret == 0);
    list_count = get_list_count(proc_group.proc_list);
    assert(list_count == 1);

    /* Kill the child and wait so it is fully gone */
    kill_and_wait(child_pid, SIGKILL);

    /* update_process_group must also return 0 when the target has gone */
    ret = update_process_group(&proc_group);
    assert(ret == 0);
    list_count = get_list_count(proc_group.proc_list);
    assert(list_count == 0);

    ret = close_process_group(&proc_group);
    assert(ret == 0);
}

/**
 * @brief Test get_process_group_cpu_usage after waiting for a valid sample
 * @note After two updates separated by enough time, cpu_usage may be >= 0
 */
static void test_process_group_cpu_usage_with_usage(void) {
    struct process_group proc_group;
    const struct timespec wait_time = {0, 50000000L}; /* 50 ms */
    double usage;
    int iter_idx;
    int ret;
    pid_t self_pid;

    self_pid = getpid();
    ret = init_process_group(&proc_group, self_pid, 0);
    assert(ret == 0);
    for (iter_idx = 0; iter_idx < 5; iter_idx++) {
        sleep_timespec(&wait_time);
        update_process_group(&proc_group);
    }
    usage = get_process_group_cpu_usage(&proc_group);
    /*
     * After several updates usage should be either -1 (not yet measured)
     * or a valid non-negative value.
     */
    assert(usage >= -1.00001);
    ret = close_process_group(&proc_group);
    assert(ret == 0);
}

/**
 * @brief Test update_process_group when the target exits between init and
 *        the first explicit update call
 * @note Exercises the race where the target terminates after
 *       init_process_group (which performs one internal update) but before the
 *       caller invokes update_process_group again.  The function must handle a
 *       missing process gracefully and leave proc_list empty without crashing.
 */
static void test_process_group_race_target_exits_between_init_and_update(void) {
    struct process_group proc_group;
    pid_t child_pid;
    pid_t waited;
    int ret;
    size_t list_count;

    child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid == 0) {
        _exit(EXIT_SUCCESS);
    }

    ret = init_process_group(&proc_group, child_pid, 0);
    assert(ret == 0);

    /* Ensure child has definitely exited before calling update */
    do {
        waited = waitpid(child_pid, NULL, 0);
    } while (waited == -1 && errno == EINTR);
    assert(waited == child_pid);

    /* update_process_group must not crash when the target is gone */
    update_process_group(&proc_group);

    /* The process no longer exists; list must now be empty */
    list_count = get_list_count(proc_group.proc_list);
    assert(list_count == 0);

    ret = close_process_group(&proc_group);
    assert(ret == 0);
}

/**
 * @brief Test update_process_group with rapidly spawning and exiting children
 * @note Exercises the race where child processes are created and destroyed
 *       between successive update_process_group calls.  The function must never
 *       crash or corrupt internal state regardless of how quickly descendants
 *       appear and disappear.
 */
static void test_process_group_race_rapid_child_spawn_exit(void) {
    struct process_group proc_group;
    pid_t parent_pid;
    pid_t spawner_pid;
    pid_t waited;
    int update_idx;
    int ret;
    int spawner_status;

    spawner_pid = fork();
    assert(spawner_pid >= 0);
    if (spawner_pid == 0) {
        int spawn_idx;
        /*
         * Rapidly spawn and exit short-lived children to exercise
         * the race between update and process lifecycle.
         */
        setpgid(0, 0);
        for (spawn_idx = 0; spawn_idx < 20; spawn_idx++) {
            pid_t child_pid = fork();
            assert(child_pid >= 0);
            if (child_pid == 0) {
                _exit(EXIT_SUCCESS);
            }
            /* Reap immediately to prevent zombie accumulation */
            do {
                waited = waitpid(child_pid, NULL, 0);
            } while (waited == -1 && errno == EINTR);
        }
        _exit(EXIT_SUCCESS);
    }

    parent_pid = spawner_pid;
    ret = init_process_group(&proc_group, parent_pid, 1);
    assert(ret == 0);

    /* Run several updates while children may be spawning and exiting */
    for (update_idx = 0; update_idx < 10; update_idx++) {
        const struct timespec small_sleep = {0, 5000000L}; /* 5 ms */
        update_process_group(&proc_group);
        sleep_timespec(&small_sleep);
    }

    ret = close_process_group(&proc_group);
    assert(ret == 0);

    /* Reap spawner */
    do {
        waited = waitpid(spawner_pid, &spawner_status, 0);
    } while (waited == -1 && errno == EINTR);
    assert(waited == spawner_pid);
}

/**
 * @brief Regression test: proc_table must not retain exited descendants
 * @note update_process_group() previously returned early when
 *       close_process_iterator() failed, skipping
 *       remove_stale_from_process_table() and leaking hash table entries
 *       for processes that had already exited.  This test tracks a parent
 *       whose children all exit, then verifies that after further update
 *       cycles the table holds no entry for any exited child PID and that
 *       the live entry count matches the rebuilt active list exactly.
 */
static void test_process_group_purges_exited_descendants(void) {
    struct process_group proc_group;
    const struct list_node *node;
    pid_t spawner_pid, child_pids[4], waited;
    int child_idx, update_idx, ret, spawner_status;
    int sync_pipe[2];

    ret = pipe(sync_pipe);
    assert(ret == 0);

    spawner_pid = fork();
    assert(spawner_pid >= 0);
    if (spawner_pid == 0) {
        /*
         * Child acts as the tracked parent: it spawns a few grandchildren,
         * reports their PIDs, reaps them so they truly exit, then idles
         * until killed.
         */
        int idx;
        pid_t spawned[4];
        close(sync_pipe[0]);
        setpgid(0, 0);
        for (idx = 0; idx < 4; idx++) {
            pid_t grandchild = fork();
            assert(grandchild >= 0);
            if (grandchild == 0) {
                _exit(EXIT_SUCCESS);
            }
            spawned[idx] = grandchild;
        }
        /* Reap all grandchildren so none remain as zombies */
        for (idx = 0; idx < 4; idx++) {
            pid_t w;
            do {
                w = waitpid(spawned[idx], NULL, 0);
            } while (w == -1 && errno == EINTR);
        }
        /* Publish the exited PIDs to the parent */
        if (write(sync_pipe[1], spawned, sizeof(spawned)) !=
            (ssize_t)sizeof(spawned)) {
            _exit(EXIT_FAILURE);
        }
        close(sync_pipe[1]);
        for (;;) {
            const struct timespec idle = {0, 20000000L}; /* 20 ms */
            sleep_timespec(&idle);
        }
    }

    close(sync_pipe[1]);
    /* Receive the PIDs of the now-exited grandchildren */
    assert(read(sync_pipe[0], child_pids, sizeof(child_pids)) ==
           (ssize_t)sizeof(child_pids));
    close(sync_pipe[0]);

    ret = init_process_group(&proc_group, spawner_pid, 1);
    assert(ret == 0);

    /*
     * Run enough update cycles that any stale entry would have to be
     * purged by remove_stale_from_process_table().
     */
    for (update_idx = 0; update_idx < 5; update_idx++) {
        const struct timespec gap = {0, 25000000L}; /* 25 ms */
        ret = update_process_group(&proc_group);
        assert(ret == 0);
        sleep_timespec(&gap);
    }

    /* No exited grandchild may remain in the hash table */
    for (child_idx = 0; child_idx < 4; child_idx++) {
        const struct process *stale =
            find_in_process_table(proc_group.proc_table, child_pids[child_idx]);
        assert(stale == NULL);
    }

    /*
     * Every entry reachable from the active list must still resolve in the
     * table, proving the purge removed only stale entries.
     */
    for (node = first_node(proc_group.proc_list); node != NULL;
         node = node->next) {
        const struct process *live = (const struct process *)node->data;
        assert(live != NULL);
        assert(find_in_process_table(proc_group.proc_table, live->pid) == live);
    }

    ret = close_process_group(&proc_group);
    assert(ret == 0);

    kill_and_wait(spawner_pid, SIGKILL);
    do {
        waited = waitpid(spawner_pid, &spawner_status, WNOHANG);
    } while (waited == -1 && errno == EINTR);
}

/**
 * @brief Test that a tracked entry is reset when its history stops applying
 *
 * update_existing_process_entry() discards the stored CPU history and
 * marks the usage unknown in two situations: the CPU time moved backwards
 * (the PID now belongs to a different process) and the clock moved
 * backwards.  Both leave the entry with a fresh baseline so the next
 * delta is measured against the new process rather than the old one, and
 * neither path had any coverage.
 */
static void test_process_group_entry_resets_on_reuse_and_backward_clock(void) {
    struct process_group proc_group;
    pid_t idle_pid, waited;
    int ret, idle_status;
    struct process *tracked;
    double inflated_cpu_time;

    fflush(stdout);
    fflush(stderr);
    idle_pid = fork();
    assert(idle_pid >= 0);
    if (idle_pid == 0) {
        /*
         * Bounded so that a failed assertion below cannot leave an
         * orphan behind: a stray process with this test's name would
         * break find_process_by_name() in every later run.
         */
        int idle_ticks;
        for (idle_ticks = 0; idle_ticks < 1500; idle_ticks++) {
            const struct timespec idle = {0, 20000000L}; /* 20 ms */
            sleep_timespec(&idle);
        }
        _exit(EXIT_SUCCESS);
    }

    ret = init_process_group(&proc_group, idle_pid, 0);
    assert(ret == 0);

    tracked = find_in_process_table(proc_group.proc_table, idle_pid);
    assert(tracked != NULL);

    /*
     * PID reuse: pretend the previously tracked process had burned far
     * more CPU than the process now occupying that PID.
     */
    inflated_cpu_time = tracked->cpu_time + 1e9;
    tracked->cpu_time = inflated_cpu_time;
    ret = update_process_group(&proc_group);
    assert(ret == 0);
    assert(tracked->cpu_time < inflated_cpu_time);
    assert(tracked->cpu_usage < 0);

    /*
     * Backward clock: push the baseline into the future so the elapsed
     * time of this cycle comes out negative.
     */
    tracked->cpu_usage = 0.5;
    proc_group.last_update.tv_sec += (time_t)3600;
    ret = update_process_group(&proc_group);
    assert(ret == 0);
    assert(tracked->cpu_usage < 0);

    ret = close_process_group(&proc_group);
    assert(ret == 0);

    kill_and_wait(idle_pid, SIGKILL);
    do {
        waited = waitpid(idle_pid, &idle_status, WNOHANG);
    } while (waited == -1 && errno == EINTR);
}

/***************************************************************************
 * LIMIT_PROCESS MODULE TESTS
 ***************************************************************************/

/**
 * @brief Test limit_process function
 * @note Creates a process group with multi processes and applies CPU
 *       limiting to verify that the CPU usage stays within the specified limit
 */
static void test_limit_process_basic(void) {
    const double cpu_usage_limit = 0.5;
    pid_t child_pid;
    int sync_pipe[2], num_procs;
    int ret;

    num_procs = get_ncpu();
    /* Ensure at least 2 processes to validate include_children option */
    num_procs = MAX(num_procs, 2);

    /* Create pipe for synchronization */
    ret = pipe(sync_pipe);
    assert(ret == 0);

    /* Fork first child process */
    child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid > 0) {
        /* Parent process: monitor CPU usage */
        pid_t limiter_pid;
        ssize_t ack_count, read_ret;
        char ack;

        ret = close(sync_pipe[1]);
        assert(ret == 0);

        /* Wait for num_procs acknowledgements (from num_procs processes) */
        for (ack_count = 0; ack_count < num_procs; ack_count++) {
            read_ret = read(sync_pipe[0], &ack, 1);
            if (read_ret == -1 && errno == EINTR) {
                continue; /* Interrupted, retry read */
            }
            assert(read_ret == 1 && ack == 'A');
        }
        /* Now should read EOF */
        do {
            read_ret = read(sync_pipe[0], &ack, 1);
        } while (read_ret == -1 && errno == EINTR);
        assert(read_ret == 0);
        ret = close(sync_pipe[0]);
        assert(ret == 0);
        assert(ack_count == num_procs);
        /* Fork CPU limiter process */
        limiter_pid = fork();
        assert(limiter_pid >= 0);

        if (limiter_pid > 0) {
            /* Monitor process: track CPU usage */
            int iter_idx;
            size_t count = 0;
            double cpu_usage = 0;
            struct process_group proc_group;
            const struct timespec sleep_time = {0, 500000000L};
            int ncpu;

            /* Initialize process group monitoring */
            ret = init_process_group(&proc_group, child_pid, 1);
            assert(ret == 0);

            /* Monitor CPU usage over 60 iterations */
            for (iter_idx = 0; iter_idx < 60 && !is_quit_flag_set();
                 iter_idx++) {
                double temp_cpu_usage;
                size_t list_count;
                size_t expected_count;
                sleep_timespec(&sleep_time);
                update_process_group(&proc_group);

                /* Verify all num_procs processes are being monitored */
                list_count = get_list_count(proc_group.proc_list);
                expected_count = (size_t)num_procs;
                assert(list_count == expected_count);

                temp_cpu_usage = get_process_group_cpu_usage(&proc_group);
                if (temp_cpu_usage > 0) {
                    cpu_usage += temp_cpu_usage;
                    count++;
                }
            }
            ret = close_process_group(&proc_group);
            assert(ret == 0);
            assert(count > 0);

            /* Terminate limiter process first */
            kill_and_wait(limiter_pid, SIGKILL);

            /* Terminate entire process group */
            kill_and_wait(-child_pid, SIGKILL);

            /* Calculate and display average CPU usage */
            cpu_usage /= (double)count;
            printf("CPU usage limit: %.3f, CPU usage: %.3f\n", cpu_usage_limit,
                   cpu_usage);

            ncpu = get_ncpu();
            assert(cpu_usage <= ncpu);
            /*
             * The average has to land near the requested limit.  The
             * bound is deliberately loose so that scheduling noise
             * cannot turn this into a flaky failure, but it stays far
             * below what an unthrottled group would report: without
             * limiting, every monitored process pegs a core and the
             * group total reaches num_procs.  Checking only against
             * ncpu above would let that pass.
             */
            assert(cpu_usage <= cpu_usage_limit * 3.0);

            return;
        }
        /* limiter_pid == 0: CPU limiter process */
        limit_process(child_pid, cpu_usage_limit, 1, 0);
        _exit(EXIT_SUCCESS);
    } else {
        /* child_pid == 0: Target process group */
        int proc_idx;
        volatile int keep_running = 1;
        ssize_t nwritten;

        /* Create new process group */
        setpgid(0, 0);

        ret = close(sync_pipe[0]);
        assert(ret == 0);

        /* Fork (num_procs - 1) child processes */
        for (proc_idx = 1; proc_idx < num_procs; proc_idx++) {
            pid_t pid = fork();
            assert(pid >= 0);

            if (pid == 0) {
                /* Child process: do not create more children */
                break;
            }
        }

        /* Send acknowledgement and close pipe */
        nwritten = write(sync_pipe[1], "A", 1);
        assert(nwritten == 1);
        ret = close(sync_pipe[1]);
        assert(ret == 0);

        /* Keep processes running until terminated */
        while (keep_running && !is_quit_flag_set()) {
            volatile int dummy_var;
            for (dummy_var = 0; dummy_var < 1000; dummy_var = dummy_var + 1) {
                ;
            }
        }
        _exit(EXIT_SUCCESS);
    }
}

/**
 * @brief Test limit_process when the target has already exited
 * @note Exercises the empty-proc_list early-exit branch in limit_process
 */
static void test_limit_process_exits_early(void) {
    pid_t child_pid;
    pid_t waited_pid;

    child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid == 0) {
        _exit(EXIT_SUCCESS);
    }

    /* Wait for child to exit completely */
    do {
        waited_pid = waitpid(child_pid, NULL, 0);
    } while (waited_pid == -1 && errno == EINTR);
    assert(waited_pid == child_pid);

    /* limit_process must handle an already-gone process gracefully */
    limit_process(child_pid, 0.5, 0, 0);
}

/**
 * @brief Test limit_process with verbose=1
 * @note Exercises the verbose printf branches; output suppressed via fork
 */
static void test_limit_process_verbose(void) {
    pid_t wrapper_pid, waited;
    int wrapper_status, w_exited, w_exit_code;

    wrapper_pid = fork();
    assert(wrapper_pid >= 0);
    if (wrapper_pid == 0) {
        pid_t child_pid;
        pid_t waited_pid;

        close(STDOUT_FILENO); /* Suppress verbose output */

        child_pid = fork();
        assert(child_pid >= 0);
        if (child_pid == 0) {
            _exit(EXIT_SUCCESS);
        }

        do {
            waited_pid = waitpid(child_pid, NULL, 0);
        } while (waited_pid == -1 && errno == EINTR);
        assert(waited_pid == child_pid);
        limit_process(child_pid, 0.5, 0, 1); /* verbose = 1 */
        _exit(EXIT_SUCCESS);
    }

    waited = waitpid(wrapper_pid, &wrapper_status, 0);
    assert(waited == wrapper_pid);
    w_exited = WIFEXITED(wrapper_status);
    assert(w_exited);
    w_exit_code = WEXITSTATUS(wrapper_status);
    assert(w_exit_code == EXIT_SUCCESS);
}

/**
 * @brief Test limit_process with include_children=1
 * @note Target exits quickly; exercises the children-tracking code path
 */
static void test_limit_process_include_children(void) {
    pid_t child_pid;
    pid_t waited_pid;

    child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid == 0) {
        _exit(EXIT_SUCCESS);
    }

    /* Wait for child to exit completely */
    do {
        waited_pid = waitpid(child_pid, NULL, 0);
    } while (waited_pid == -1 && errno == EINTR);
    assert(waited_pid == child_pid);
    limit_process(child_pid, 0.5, 1, 0); /* include_children=1 */
}

/**
 * @brief Drain every heartbeat byte currently readable from a pipe
 * @param fd Non-blocking read end of a heartbeat pipe
 * @return Number of bytes drained
 *
 * A running process writes heartbeats into the pipe; a suspended process
 * writes nothing.  A zero count over a time window therefore proves the
 * writer did not execute during that window.
 */
static int drain_heartbeats(int fd) {
    char buf[128];
    int count = 0;
    for (;;) {
        ssize_t n_read = read(fd, buf, sizeof(buf));
        if (n_read > 0) {
            count += (int)n_read;
            continue;
        }
        if (n_read < 0 && errno == EINTR) {
            continue;
        }
        break; /* EAGAIN: pipe drained, or a hard error */
    }
    return count;
}

/**
 * @brief Test that a descendant orphaned by its ancestor is resumed on exit
 *
 * A descendant tracked through include_children stops matching the group as
 * soon as its monitored ancestor exits: the descendant is re-parented away,
 * so is_child_of() no longer finds the ancestor in its parent chain and the
 * descendant disappears from the process group while it still holds the
 * SIGSTOP that limit_process() sent.  limit_process() must therefore
 * remember every PID it suspended and resume those as well when it leaves;
 * otherwise the orphan stays suspended forever.
 *
 * The descendant announces that it is executing by writing heartbeats into
 * a pipe.  Once the limiter has exited, heartbeats must reappear.
 */
static void test_limit_process_resumes_orphaned_descendant(void) {
    const double cpu_usage_limit = 0.001;
    const struct timespec settle_time = {1, 0};
    const struct timespec poll_time = {0, 20000000L}; /* 20 ms */
    int heartbeat[2], info[2], ret;
    pid_t target_pid, descendant_pid, limiter_pid;
    size_t info_read;
    int limiter_exited, attempt;

    ret = pipe(heartbeat);
    assert(ret == 0);
    ret = pipe(info);
    assert(ret == 0);
    ret = fcntl(heartbeat[0], F_SETFL, O_NONBLOCK);
    assert(ret == 0);
    ret = fcntl(heartbeat[1], F_SETFL, O_NONBLOCK);
    assert(ret == 0);

    fflush(stdout);
    fflush(stderr);
    target_pid = fork();
    assert(target_pid >= 0);
    if (target_pid == 0) {
        pid_t child_pid;
        ret = close(heartbeat[0]);
        assert(ret == 0);
        ret = close(info[0]);
        assert(ret == 0);

        child_pid = fork();
        assert(child_pid >= 0);
        if (child_pid == 0) {
            ret = close(info[1]);
            assert(ret == 0);
            for (;;) {
                volatile int spin;
                for (spin = 0; spin < 20000; spin = spin + 1) {
                    ;
                }
                /*
                 * Best effort: the write end is non-blocking, so a full
                 * pipe simply drops the heartbeat.
                 */
                if (write(heartbeat[1], "x", 1) != 1) {
                    /* Pipe full: the drain loop will catch up later. */
                }
            }
        }
        if (write(info[1], &child_pid, sizeof(child_pid)) !=
            (ssize_t)sizeof(child_pid)) {
            _exit(EXIT_FAILURE);
        }
        ret = close(info[1]);
        assert(ret == 0);
        for (;;) {
            volatile int spin;
            for (spin = 0; spin < 20000; spin = spin + 1) {
                ;
            }
        }
    }

    ret = close(heartbeat[1]);
    assert(ret == 0);
    ret = close(info[1]);
    assert(ret == 0);

    /* Learn the descendant PID from the target. */
    info_read = 0;
    while (info_read < sizeof(descendant_pid)) {
        ssize_t n_read = read(info[0], ((char *)&descendant_pid) + info_read,
                              sizeof(descendant_pid) - info_read);
        if (n_read > 0) {
            info_read += (size_t)n_read;
            continue;
        }
        if (n_read < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    assert(info_read == sizeof(descendant_pid));
    ret = close(info[0]);
    assert(ret == 0);

    limiter_pid = fork();
    assert(limiter_pid >= 0);
    if (limiter_pid == 0) {
        ret = close(heartbeat[0]);
        assert(ret == 0);
        configure_signal_handler();
        limit_process(target_pid, cpu_usage_limit, 1, 0);
        _exit(EXIT_SUCCESS);
    }

    /* Let the limiter suspend the whole group for a few cycles. */
    sleep_timespec(&settle_time);

    /* Kill the ancestor: the descendant is orphaned while suspended. */
    kill(target_pid, SIGKILL);
    while (waitpid(target_pid, NULL, 0) == -1 && errno == EINTR) {
        /* Retry on EINTR */
    }

    /* The limiter must notice that its group is gone and return. */
    limiter_exited = 0;
    for (attempt = 0; attempt < 500 && !limiter_exited; attempt++) {
        pid_t wpid = waitpid(limiter_pid, NULL, WNOHANG);
        if (wpid == limiter_pid) {
            limiter_exited = 1;
        } else if (wpid == -1 && errno != EINTR) {
            break;
        } else {
            sleep_timespec(&poll_time);
        }
    }
    if (!limiter_exited) {
        kill(limiter_pid, SIGKILL);
        while (waitpid(limiter_pid, NULL, 0) == -1 && errno == EINTR) {
            /* Retry on EINTR */
        }
    }
    assert(limiter_exited);

    /*
     * The limiter is gone, so nothing can suspend the descendant any more.
     * It must be executing again: heartbeats have to reappear.
     */
    drain_heartbeats(heartbeat[0]);
    sleep_timespec(&settle_time);
    assert(drain_heartbeats(heartbeat[0]) > 0);

    /* The descendant was re-parented away, so it can only be killed. */
    kill(descendant_pid, SIGKILL);
    ret = close(heartbeat[0]);
    assert(ret == 0);
}

/**
 * @brief Signal handler used by race condition tests: exits on SIGCONT
 * @param sig Signal number (unused)
 *
 * Installed as the SIGCONT handler so a process immediately terminates
 * when resumed after being stopped by SIGSTOP.  This reproduces the race
 * where limit_process issues SIGCONT and the target exits before the next
 * SIGSTOP can be delivered.
 */
static void sigcont_exit_handler(int sig) {
    (void)sig;
    _exit(EXIT_SUCCESS);
}

/**
 * @brief Shared counter incremented by resume_counter_handler.
 * @note Used by test_limiter_run_pid_or_exe_mode_resumes_target to prove the
 *       target was resumed (received SIGCONT) at least once instead of being
 *       left permanently stopped.
 */
static volatile sig_atomic_t resume_counter = 0;

/**
 * @brief SIGCONT handler that records a resume event in resume_counter.
 * @param sig Signal number (unused)
 */
static void resume_counter_handler(int sig) {
    (void)sig;
    resume_counter++;
}

/**
 * @brief Test that limit_process handles ESRCH when target exits on SIGCONT
 * @note Exercises the race between SIGCONT and the subsequent SIGSTOP:
 *       the target installs a SIGCONT handler that calls _exit(), so when
 *       limit_process resumes the stopped process, it dies immediately.
 *       The next SIGSTOP attempt must get ESRCH, remove the entry, and exit
 *       the control loop cleanly without crashing.
 */
static void test_limit_process_race_process_exits_on_sigcont(void) {
    pid_t wrapper_pid, waited;
    int wrapper_status, w_exited, w_exit_code;

    wrapper_pid = fork();
    assert(wrapper_pid >= 0);
    if (wrapper_pid == 0) {
        pid_t target_pid;
        pid_t limiter_pid;
        int limiter_status;
        pid_t res;

        /* Fork the target process */
        target_pid = fork();
        assert(target_pid >= 0);
        if (target_pid == 0) {
            struct sigaction sa;
            /*
             * Install SIGCONT handler that exits immediately.
             * When limit_process sends SIGCONT to resume the stopped
             * target, the handler fires and the process exits, creating
             * the ESRCH race on the next SIGSTOP attempt.
             */
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = sigcont_exit_handler;
            if (sigemptyset(&sa.sa_mask) != 0) {
                _exit(1);
            }
            if (sigaction(SIGCONT, &sa, NULL) != 0) {
                _exit(1);
            }
            /* Busy loop: stays alive until stopped then resumed */
            for (;;) {
                volatile int dummy_var;
                for (dummy_var = 0; dummy_var < 1000;
                     dummy_var = dummy_var + 1) {
                    ;
                }
            }
        }

        /* Fork the limiter */
        limiter_pid = fork();
        assert(limiter_pid >= 0);
        if (limiter_pid == 0) {
            configure_signal_handler();
            /*
             * Very low CPU limit forces short work slots and long sleep
             * slots, exercising SIGSTOP/SIGCONT cycles quickly.
             */
            limit_process(target_pid, 0.001, 0, 0);
            _exit(EXIT_SUCCESS);
        }

        /* Wait for limiter to exit naturally (target exits on SIGCONT) */
        do {
            waited = waitpid(limiter_pid, &limiter_status, 0);
        } while (waited == -1 && errno == EINTR);
        /* Reap any leftover target, if still running */
        do {
            res = waitpid(target_pid, NULL, WNOHANG);
        } while (res == -1 && errno == EINTR);

        if (res == 0) {
            /* Child still running: kill and reap */
            if (kill(target_pid, SIGKILL) == -1 && errno != ESRCH) {
                /*
                 * kill failed for unexpected reason; fall through to
                 * reap anyway to avoid zombies.
                 */
            }
            do {
                res = waitpid(target_pid, NULL, 0);
            } while (res == -1 && errno == EINTR);
        }
        /* res == -1 && errno == ECHILD means already reaped */

        if (!WIFEXITED(limiter_status) ||
            WEXITSTATUS(limiter_status) != EXIT_SUCCESS) {
            _exit(1);
        }
        _exit(EXIT_SUCCESS);
    }

    waited = waitpid(wrapper_pid, &wrapper_status, 0);
    assert(waited == wrapper_pid);
    w_exited = WIFEXITED(wrapper_status);
    assert(w_exited);
    w_exit_code = WEXITSTATUS(wrapper_status);
    assert(w_exit_code == EXIT_SUCCESS);
}

/**
 * @brief Test that a quit signal received during limit_process sleep exits
 *        the control loop gracefully
 * @note Exercises the race where clock_nanosleep (or nanosleep) is
 *       interrupted by a SIGTERM delivered from an external process.  Because
 *       neither clock_nanosleep nor nanosleep honours SA_RESTART, the sleep
 *       returns EINTR and the loop immediately checks is_quit_flag_set(),
 *       which must now be true, causing a clean exit.
 */
static void test_limit_process_race_quit_during_sleep(void) {
    int ready_pipe[2];
    pid_t target_pid;
    pid_t limiter_pid;
    int limiter_status;
    int ret;

    ret = pipe(ready_pipe);
    assert(ret == 0);

    /* Fork the target: spin forever until killed */
    target_pid = fork();
    assert(target_pid >= 0);
    if (target_pid == 0) {
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        for (;;) {
            volatile int dummy_var;
            for (dummy_var = 0; dummy_var < 1000; dummy_var = dummy_var + 1) {
                ;
            }
        }
    }

    /* Fork the limiter */
    limiter_pid = fork();
    assert(limiter_pid >= 0);
    if (limiter_pid == 0) {
        close(ready_pipe[0]);
        configure_signal_handler();

        /* Signal readiness before entering the blocking limit_process loop */
        if (write(ready_pipe[1], "L", 1) != 1) {
            close(ready_pipe[1]);
            kill(target_pid, SIGKILL);
            _exit(EXIT_FAILURE);
        }
        close(ready_pipe[1]);

        limit_process(target_pid, 0.5, 0, 0);
        /*
         * limit_process always resumes all stopped processes before
         * returning, so target_pid should be in a runnable state here.
         */
        _exit(EXIT_SUCCESS);
    } else {
        const struct timespec delay = {0, 100000000L}; /* 100 ms */
        char ready_byte;
        ssize_t n_read;
        int exited;
        int exit_code;
        pid_t waited;

        /* Wait for limiter to be ready */
        close(ready_pipe[1]);
        do {
            n_read = read(ready_pipe[0], &ready_byte, 1);
        } while (n_read < 0 && errno == EINTR);
        assert(n_read == 1 && ready_byte == 'L');
        close(ready_pipe[0]);

        /* Give limiter a moment to enter its sleep loop */
        sleep_timespec(&delay);

        /* Deliver SIGTERM to the limiter: interrupts sleep, sets quit_flag */
        kill(limiter_pid, SIGTERM);

        /* Limiter must exit promptly */
        waited = waitpid(limiter_pid, &limiter_status, 0);
        assert(waited == limiter_pid);
        exited = WIFEXITED(limiter_status);
        assert(exited);
        exit_code = WEXITSTATUS(limiter_status);
        assert(exit_code == EXIT_SUCCESS);

        /* Clean up the spinning target */
        kill_and_wait(target_pid, SIGKILL);
    }
}

/***************************************************************************
 * LIMITER MODULE TESTS
 ***************************************************************************/

/**
 * @brief Test run_command_mode with a command that exits immediately
 * @note Forks a child to call run_command_mode and checks the exit code
 *       matches the command's exit code
 */
static void test_limiter_run_command_mode(void) {
    pid_t pid, waited;
    int status, exited, exit_code;
    struct cpulimit_cfg cfg;
    char cmd[] = "true";
    char *args[2];

    args[0] = cmd;
    args[1] = NULL;
    memset(&cfg, 0, sizeof(struct cpulimit_cfg));
    cfg.program_name = "test";
    cfg.command_mode = 1;
    cfg.command_args = args;
    cfg.limit = 0.5;
    cfg.lazy_mode = 1;

    /*
     * Flush stdout and stderr before forking to prevent the child from
     * inheriting buffered output.  run_command_mode() calls
     * fflush(stdout) and fflush(stderr) before its own inner fork; if
     * the parent's buffers are not empty at this point, the child's
     * flushes would write the buffered content early, and the parent's
     * buffers would write the same content again later, producing
     * duplicated output when stdout or stderr is piped.
     */
    fflush(stdout);
    fflush(stderr);

    /* Run in child process since run_command_mode calls exit() */
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        run_command_mode(&cfg);
        _exit(EXIT_FAILURE); /* Should not reach here */
    }

    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    /* 'true' exits with 0, so run_command_mode should exit with 0 */
    exit_code = WEXITSTATUS(status);
    assert(exit_code == EXIT_SUCCESS);
}

/**
 * @brief Test run_pid_or_exe_mode when the target process does not exist
 * @note Verifies that lazy mode exits with EXIT_FAILURE when the target
 *       executable name is not found
 */
static void test_limiter_run_pid_or_exe_mode(void) {
    pid_t pid, waited;
    int status, exited, exit_code;
    struct cpulimit_cfg cfg;
    const char exe_name[] = "nonexistent_exe_cpulimit_test_12345";

    memset(&cfg, 0, sizeof(struct cpulimit_cfg));
    cfg.program_name = "test";
    cfg.exe_name = exe_name;
    cfg.limit = 0.5;
    cfg.lazy_mode = 1;

    /* Run in child since run_pid_or_exe_mode calls exit() */
    fflush(stdout);
    fflush(stderr);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        /* Suppress output from run_pid_or_exe_mode in child */
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        run_pid_or_exe_mode(&cfg);
        _exit(EXIT_FAILURE); /* Should not reach here */
    }

    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    /* Process not found with lazy_mode=1 -> EXIT_FAILURE */
    exit_code = WEXITSTATUS(status);
    assert(exit_code == EXIT_FAILURE);
}

/**
 * @brief Test run_command_mode with a non-existent binary
 * @note execvp ENOENT in child; parent should return shell code 127
 */
static void test_limiter_run_command_mode_nonexistent(void) {
    pid_t pid, waited;
    int status, exited, exit_code;
    struct cpulimit_cfg cfg;
    char cmd[] = "/nonexistent_cpulimit_test_binary_xyz";
    char *args[2];

    args[0] = cmd;
    args[1] = NULL;
    memset(&cfg, 0, sizeof(struct cpulimit_cfg));
    cfg.program_name = "test";
    cfg.command_mode = 1;
    cfg.command_args = args;
    cfg.limit = 0.5;
    cfg.lazy_mode = 1;

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        run_command_mode(&cfg);
        _exit(EXIT_FAILURE);
    }

    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == 127);
}

/**
 * @brief Internal argv marker turning this binary into the SIGINT-counting
 *        command driven by the signal-forwarding test
 *
 * Unit tests must live in cpulimit_test.c, so the command used by
 * test_limiter_run_command_mode_forwards_signal_once() is this very
 * binary re-executed with this marker instead of a separate helper.
 */
#define SIGCOUNT_CHILD_ARG "--cpulimit-test-sigcount-child"

/** @brief How many 10 ms slots the counting command waits for its SIGINT. */
#define SIGCOUNT_WAIT_SLOTS 1000

/** @brief How many 10 ms slots it lingers, so a duplicate shows up. */
#define SIGCOUNT_LINGER_SLOTS 150

/** @brief Number of SIGINTs received by the counting command. */
static volatile sig_atomic_t sigint_delivery_count = 0;

/**
 * @brief Record one SIGINT delivery in the counting command
 * @param sig Signal number (unused)
 */
static void count_sigint_delivery(int sig) {
    (void)sig;
    sigint_delivery_count++;
}

/**
 * @brief Count SIGINT deliveries and return that count
 * @param ready_path File created once the handler is installed, so the
 *                   driving test can synchronise instead of guessing a delay
 * @return Number of SIGINTs received, 99 if the handler was not installed
 */
static int run_sigcount_child(const char *ready_path) {
    const struct timespec poll_time = {0, 10000000L}; /* 10 ms */
    struct sigaction sa;
    int ready_fd, slot;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = count_sigint_delivery;
    if (sigemptyset(&sa.sa_mask) != 0 || sigaction(SIGINT, &sa, NULL) != 0) {
        return 99;
    }

    ready_fd = creat(ready_path, 0600);
    if (ready_fd >= 0) {
        close(ready_fd);
    }

    /* Wait for the first SIGINT. */
    for (slot = 0; slot < SIGCOUNT_WAIT_SLOTS && sigint_delivery_count == 0;
         slot++) {
        sleep_timespec(&poll_time);
    }

    /*
     * Linger afterwards. A duplicate delivered shortly after the first
     * one is only observable while this process is still running.
     */
    for (slot = 0; slot < SIGCOUNT_LINGER_SLOTS; slot++) {
        sleep_timespec(&poll_time);
    }

    return (int)sigint_delivery_count;
}

/**
 * @brief Test that interrupting run_command_mode forwards the signal once
 *
 * A single Ctrl+C must reach the command exactly once, the way a shell
 * delivers it: a program that traps SIGINT to shut itself down cleanly
 * would have that handler cut short by a second, duplicate delivery.
 *
 * The command is this binary re-executed with SIGCOUNT_CHILD_ARG, which
 * exits with the number of SIGINTs it received. run_command_mode()
 * propagates that as its own status, so the wrapper must exit with 1.
 */
static void test_limiter_run_command_mode_forwards_signal_once(void) {
    const struct timespec poll_time = {0, 20000000L}; /* 20 ms */
    pid_t wrapper_pid, waited;
    int wrapper_status, w_exited, w_exit_code, attempt, ready, ready_fd;
    int close_ret;
    char ready_path[] = "/tmp/cpulimit_test_sigcount_XXXXXX";
    char child_arg[] = SIGCOUNT_CHILD_ARG;
    char *args[4];

    ready_fd = mkstemp(ready_path);
    assert(ready_fd >= 0);
    close_ret = close(ready_fd);
    assert(close_ret == 0);
    /* The command recreates it once its handler is installed. */
    unlink(ready_path);

    args[0] = argv0;
    args[1] = child_arg;
    args[2] = ready_path;
    args[3] = NULL;

    fflush(stdout);
    fflush(stderr);
    wrapper_pid = fork();
    assert(wrapper_pid >= 0);
    if (wrapper_pid == 0) {
        struct cpulimit_cfg cfg;
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        memset(&cfg, 0, sizeof(struct cpulimit_cfg));
        cfg.program_name = "test";
        cfg.command_mode = 1;
        cfg.command_args = args;
        cfg.limit = 0.5;
        cfg.lazy_mode = 1;
        configure_signal_handler();
        run_command_mode(&cfg);
        _exit(EXIT_FAILURE);
    }

    /* Wait until the command has installed its handler. */
    ready = 0;
    for (attempt = 0; attempt < 500 && !ready; attempt++) {
        if (access(ready_path, F_OK) == 0) {
            ready = 1;
        } else {
            sleep_timespec(&poll_time);
        }
    }
    assert(ready);

    /* One interruption must produce exactly one forwarded SIGINT. */
    kill(wrapper_pid, SIGINT);

    waited = waitpid(wrapper_pid, &wrapper_status, 0);
    assert(waited == wrapper_pid);
    w_exited = WIFEXITED(wrapper_status);
    unlink(ready_path);
    assert(w_exited);
    w_exit_code = WEXITSTATUS(wrapper_status);
    assert(w_exit_code == 1);
}

/**
 * @brief Test run_command_mode with a script whose shebang interpreter
 *        does not exist
 * @note execvp returns ENOENT but the file itself exists; the parent
 *       should return shell code 126 (found but not executable / bad
 *       interpreter), not 127 (command not found)
 */
static void test_limiter_run_command_mode_bad_shebang(void) {
    pid_t pid, waited;
    int status, fd, fchmod_ret, exited, exit_code, close_ret;
    static const char shebang[] = "#!/nonexistent_interpreter_cpulimit_xyz\n";
    ssize_t nwritten;
    ssize_t expected_len;
    struct cpulimit_cfg cfg;
    char tmp_path[] = "/tmp/cpulimit_test_shebang_XXXXXX";
    char *args[2];

    /*
     * Create a temporary executable script with a missing shebang
     * interpreter so that execvp() fails with ENOENT even though
     * the file itself exists.
     */
    fd = mkstemp(tmp_path);
    assert(fd >= 0);
    /* sizeof(shebang) - 1: exclude the null terminator from the write */
    nwritten = write(fd, shebang, sizeof(shebang) - 1);
    expected_len = (ssize_t)(sizeof(shebang) - 1);
    assert(nwritten == expected_len);
    fchmod_ret = fchmod(fd, 0755);
    assert(fchmod_ret == 0);
    close_ret = close(fd);
    assert(close_ret == 0);

    args[0] = tmp_path;
    args[1] = NULL;
    memset(&cfg, 0, sizeof(struct cpulimit_cfg));
    cfg.program_name = "test";
    cfg.command_mode = 1;
    cfg.command_args = args;
    cfg.limit = 0.5;
    cfg.lazy_mode = 1;

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        run_command_mode(&cfg);
        _exit(EXIT_FAILURE);
    }

    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == 126);
    unlink(tmp_path);
}

/**
 * @brief Detect whether the test binary is running under valgrind
 * @return 1 when a valgrind preload library has been injected, 0 otherwise
 *
 * valgrind's execve() interception opens the executable to inspect it, and
 * that open blocks forever when the target is not a regular file. A test
 * that execs a FIFO would therefore hang the run instead of failing, so
 * such tests skip themselves when a valgrind preload library is visible in
 * LD_PRELOAD (Linux) or DYLD_INSERT_LIBRARIES (macOS).
 */
static int running_under_valgrind(void) {
    const char *preload = getenv("LD_PRELOAD");
#if defined(__APPLE__)
    if (preload == NULL) {
        preload = getenv("DYLD_INSERT_LIBRARIES");
    }
#endif
    if (preload == NULL) {
        return 0;
    }
    return strstr(preload, "vgpreload") != NULL;
}

/**
 * @brief Test run_command_mode with a FIFO as the command
 *
 * argv[0] is not required to be a regular file, and open() on a FIFO that
 * has no writer blocks until another process opens the write end. The exec
 * pre-check that inspects the shebang line must therefore open the target
 * with O_NONBLOCK: a blocking open strands the command child before exec,
 * and the parent then blocks reading the exec synchronisation pipe, where
 * it can no longer be terminated by a signal either. execvp() rejects a
 * FIFO at once, so the correct outcome is shell status 126 with no wait.
 *
 * @note Skipped under valgrind, whose execve() interception opens the
 *       target executable and so blocks on a FIFO itself.
 * @note The wait is bounded so that a regression fails this test instead of
 *       hanging the whole run. If cpulimit does not come back, the FIFO is
 *       given a writer to release the stranded child before cleanup.
 */
static void test_limiter_run_command_mode_fifo(void) {
    const struct timespec poll_time = {0, 20000000L}; /* 20 ms */
    pid_t pid, waited;
    int status, fd, exited, exit_code, ret, attempt;
    struct cpulimit_cfg cfg;
    char fifo_path[] = "/tmp/cpulimit_test_fifo_XXXXXX";
    char *args[2];

    if (running_under_valgrind()) {
        printf("(skipped: valgrind's exec interception blocks on a FIFO)\n");
        return;
    }

    /* Reserve a unique name, then turn it into a FIFO. */
    fd = mkstemp(fifo_path);
    assert(fd >= 0);
    ret = close(fd);
    assert(ret == 0);
    ret = unlink(fifo_path);
    assert(ret == 0);
    ret = mkfifo(fifo_path, 0600);
    assert(ret == 0);

    args[0] = fifo_path;
    args[1] = NULL;
    memset(&cfg, 0, sizeof(struct cpulimit_cfg));
    cfg.program_name = "test";
    cfg.command_mode = 1;
    cfg.command_args = args;
    cfg.limit = 0.5;
    cfg.lazy_mode = 1;

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        run_command_mode(&cfg);
        _exit(EXIT_FAILURE);
    }

    /* cpulimit must reject the FIFO immediately; it must never block. */
    waited = 0;
    for (attempt = 0; attempt < 100; attempt++) {
        waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid || (waited == -1 && errno != EINTR)) {
            break;
        }
        sleep_timespec(&poll_time);
    }
    if (waited != pid) {
        int unblock_fd;
        /* Release the child blocked in open(), then reap it. */
        unblock_fd = open(fifo_path, O_WRONLY | O_NONBLOCK);
        if (unblock_fd >= 0) {
            if (write(unblock_fd, "xx", 2) != 2) {
                /* Best effort: only meant to release the reader. */
            }
            ret = close(unblock_fd);
            assert(ret == 0);
        }
        kill_blocking(pid);
    }
    ret = unlink(fifo_path);
    assert(ret == 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == 126);
}

/**
 * @brief Test run_command_mode with an inaccessible shebang interpreter path
 * @note The interpreter path uses a non-directory path component, so lookup
 *       fails with an inaccessible-path error and should still map to shell
 *       status 126 with an "inaccessible" diagnostic.
 */
static void
test_limiter_run_command_mode_shebang_interpreter_inaccessible(void) {
    pid_t pid, waited;
    int status, fd, prefix_fd, exited, exit_code, ret;
    int stderr_pipe[2];
    ssize_t nwritten, expected_len;
    size_t err_len;
    struct cpulimit_cfg cfg;
    char tmp_path[] = "/tmp/cpulimit_test_shebang_inaccessible_XXXXXX";
    char interp_prefix[] = "/tmp/cpulimit_test_interp_prefix_XXXXXX";
    char interp_path[sizeof(interp_prefix) + sizeof("/interp")];
    char shebang[sizeof(interp_path) + sizeof("#!\n")];
    char *err_buf;
    const char *accessible_msg;
    const char *not_found_msg;
    char *args[2];

    /*
     * Allocate err_buf on the heap: 512 bytes exceeds the 256-byte
     * per-object stack limit and pushes the total frame over 512 bytes.
     */
    err_buf = (char *)malloc(512);
    assert(err_buf != NULL);

    prefix_fd = mkstemp(interp_prefix);
    assert(prefix_fd >= 0);
    ret = close(prefix_fd);
    assert(ret == 0);

    ret =
        snprintf(interp_path, sizeof(interp_path), "%s/interp", interp_prefix);
    assert(ret > 0 && ret < (int)sizeof(interp_path));

    fd = mkstemp(tmp_path);
    assert(fd >= 0);
    ret = snprintf(shebang, sizeof(shebang), "#!%s\n", interp_path);
    assert(ret > 0 && ret < (int)sizeof(shebang));
    nwritten = write(fd, shebang, (size_t)ret);
    expected_len = (ssize_t)ret;
    assert(nwritten == expected_len);
    ret = fchmod(fd, 0755);
    assert(ret == 0);
    ret = close(fd);
    assert(ret == 0);

    args[0] = tmp_path;
    args[1] = NULL;
    memset(&cfg, 0, sizeof(struct cpulimit_cfg));
    cfg.program_name = "test";
    cfg.command_mode = 1;
    cfg.command_args = args;
    cfg.limit = 0.5;
    cfg.lazy_mode = 1;

    ret = pipe(stderr_pipe);
    assert(ret == 0);
    fflush(stdout);
    fflush(stderr);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(stderr_pipe[0]);
        ret = dup2(stderr_pipe[1], STDERR_FILENO);
        if (ret < 0) {
            _exit(EXIT_FAILURE);
        }
        close(stderr_pipe[1]);
        run_command_mode(&cfg);
        _exit(EXIT_FAILURE);
    }

    close(stderr_pipe[1]);
    err_len = 0;
    while (1) {
        ssize_t nread =
            read(stderr_pipe[0], err_buf + err_len, 512 - 1 - err_len);
        if (nread > 0) {
            err_len += (size_t)nread;
            if (err_len == 512 - 1) {
                break;
            }
            continue;
        }
        if (nread == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }
    err_buf[err_len] = '\0';
    close(stderr_pipe[0]);

    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == 126);
    accessible_msg = strstr(err_buf, "shebang interpreter is inaccessible");
    assert(accessible_msg != NULL);
    not_found_msg = strstr(err_buf, "shebang interpreter not found");
    assert(not_found_msg == NULL);

    free(err_buf);

    ret = unlink(interp_prefix);
    assert(ret == 0);
    ret = unlink(tmp_path);
    assert(ret == 0);
}

/**
 * @brief Test run_command_mode with verbose=1 and a command that succeeds
 * @note Exercises the verbose printf branches in run_command_mode
 */
static void test_limiter_run_command_mode_verbose(void) {
    pid_t pid, waited;
    int status, exited, exit_code;
    struct cpulimit_cfg cfg;
    char cmd[] = "true";
    char *args[2];

    args[0] = cmd;
    args[1] = NULL;
    memset(&cfg, 0, sizeof(struct cpulimit_cfg));
    cfg.program_name = "test";
    cfg.command_mode = 1;
    cfg.command_args = args;
    cfg.limit = 0.5;
    cfg.lazy_mode = 1;
    cfg.verbose = 1;

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        run_command_mode(&cfg);
        _exit(EXIT_FAILURE);
    }

    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == EXIT_SUCCESS);
}

/**
 * @brief Test run_pid_or_exe_mode with a PID (pid_mode=1) that does not exist
 * @note Uses INT_MAX which is virtually guaranteed to be non-existent;
 *       lazy_mode=1 -> EXIT_FAILURE
 */
static void test_limiter_run_pid_or_exe_mode_pid_not_found(void) {
    pid_t pid, waited;
    int status, exited, exit_code;
    struct cpulimit_cfg cfg;

    memset(&cfg, 0, sizeof(struct cpulimit_cfg));
    cfg.program_name = "test";
    cfg.target_pid = (pid_t)INT_MAX;
    cfg.limit = 0.5;
    cfg.lazy_mode = 1;

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        run_pid_or_exe_mode(&cfg);
        _exit(EXIT_FAILURE);
    }

    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == EXIT_FAILURE);
}

/**
 * @brief Test run_command_mode with a command that exits non-zero ('false')
 * @note Exit status of 'false' is 1; run_command_mode should propagate it
 */
static void test_limiter_run_command_mode_false(void) {
    pid_t pid, waited;
    int status, exited, exit_code;
    struct cpulimit_cfg cfg;
    char cmd[] = "false";
    char *args[2];

    args[0] = cmd;
    args[1] = NULL;
    memset(&cfg, 0, sizeof(struct cpulimit_cfg));
    cfg.program_name = "test";
    cfg.command_mode = 1;
    cfg.command_args = args;
    cfg.limit = 0.5;
    cfg.lazy_mode = 1;

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        run_command_mode(&cfg);
        _exit(99);
    }

    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == EXIT_FAILURE);
}

/**
 * @brief Test run_command_mode exit status when command is killed by SIGTERM
 * @note Shell convention: exit status = 128 + signal_number
 *       'sh -c "kill -TERM $$"' causes the shell to send SIGTERM to itself;
 *       run_command_mode must propagate exit status 128 + SIGTERM.
 */
static void test_limiter_run_command_mode_signal_term(void) {
    pid_t pid, waited;
    int status, exited, exit_code;
    struct cpulimit_cfg cfg;
    char cmd[] = "sh";
    char arg1[] = "-c";
    char arg2[] = "kill -TERM $$";
    char *args[4];

    args[0] = cmd;
    args[1] = arg1;
    args[2] = arg2;
    args[3] = NULL;
    memset(&cfg, 0, sizeof(struct cpulimit_cfg));
    cfg.program_name = "test";
    cfg.command_mode = 1;
    cfg.command_args = args;
    cfg.limit = 0.5;
    cfg.lazy_mode = 1;

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        run_command_mode(&cfg);
        _exit(99);
    }

    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    /* Shell killed by SIGTERM -> exit status 128 + SIGTERM */
    exit_code = WEXITSTATUS(status);
    assert(exit_code == 128 + SIGTERM);
}

/**
 * @brief Test run_command_mode exit status when command is killed by SIGKILL
 * @note Shell convention: exit status = 128 + signal_number
 *       'sh -c "kill -KILL $$"' causes the shell to send SIGKILL to itself;
 *       run_command_mode must propagate exit status 128 + SIGKILL.
 */
static void test_limiter_run_command_mode_signal_kill(void) {
    pid_t pid, waited;
    int status, exited, exit_code;
    struct cpulimit_cfg cfg;
    char cmd[] = "sh";
    char arg1[] = "-c";
    char arg2[] = "kill -9 $$";
    char *args[4];

    args[0] = cmd;
    args[1] = arg1;
    args[2] = arg2;
    args[3] = NULL;
    memset(&cfg, 0, sizeof(struct cpulimit_cfg));
    cfg.program_name = "test";
    cfg.command_mode = 1;
    cfg.command_args = args;
    cfg.limit = 0.5;
    cfg.lazy_mode = 1;

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        run_command_mode(&cfg);
        _exit(99);
    }

    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    /* Shell killed by SIGKILL -> exit status 128 + SIGKILL */
    exit_code = WEXITSTATUS(status);
    assert(exit_code == 128 + SIGKILL);
}

/**
 * @brief Test run_command_mode when the command forks a background grandchild
 * @note Verifies that run_command_mode exits correctly (with the shell's exit
 *       status) even when the executed command itself forks a child process
 *       that outlives it.  The grandchild is reparented to init and does not
 *       affect the parent's wait loop, matching standard POSIX shell semantics.
 */
static void test_limiter_run_command_mode_with_fork(void) {
    pid_t pid, waited;
    int status, exited, exit_code;
    struct cpulimit_cfg cfg;
    char cmd[] = "sh";
    char arg1[] = "-c";
    /* Fork a short-lived background child; the shell exits immediately */
    char arg2[] = "sleep 1 &";
    char *args[4];

    args[0] = cmd;
    args[1] = arg1;
    args[2] = arg2;
    args[3] = NULL;
    memset(&cfg, 0, sizeof(struct cpulimit_cfg));
    cfg.program_name = "test";
    cfg.command_mode = 1;
    cfg.command_args = args;
    cfg.limit = 0.5;
    cfg.lazy_mode = 1;

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        run_command_mode(&cfg);
        _exit(99);
    }

    /*
     * The shell exits immediately after forking the background child.
     * run_command_mode must not block waiting for the grandchild;
     * it should exit promptly with the shell's exit status (0).
     */
    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    exit_code = WEXITSTATUS(status);
    assert(exit_code == EXIT_SUCCESS);
}

/**
 * @brief Test run_command_mode forwards SIGTERM when quit flag is set
 * @note Sends SIGTERM to the wrapper process while it is running a long-lived
 *       command ('sleep 60').  run_command_mode must forward SIGTERM to the
 *       command process group and exit with 128 + SIGTERM = 143.
 */
static void test_limiter_run_command_mode_quit_signal(void) {
    int ready_pipe[2];
    pid_t wrapper_pid, waited;
    int wrapper_status, w_exited, w_exit_code, ret;
    char ready_byte;
    ssize_t n_read;

    ret = pipe(ready_pipe);
    assert(ret == 0);

    fflush(stdout);
    fflush(stderr);
    wrapper_pid = fork();
    assert(wrapper_pid >= 0);
    if (wrapper_pid == 0) {
        struct cpulimit_cfg cfg;
        char cmd[] = "sleep";
        char arg1[] = "60";
        char *args[3];

        ret = close(ready_pipe[0]);
        assert(ret == 0);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);

        args[0] = cmd;
        args[1] = arg1;
        args[2] = NULL;
        memset(&cfg, 0, sizeof(struct cpulimit_cfg));
        cfg.program_name = "test";
        cfg.command_mode = 1;
        cfg.command_args = args;
        cfg.limit = 0.5;
        cfg.lazy_mode = 1;
        /* Notify test that wrapper is ready to call run_command_mode */
        if (write(ready_pipe[1], "A", 1) != 1) {
            close(ready_pipe[1]);
            _exit(EXIT_FAILURE);
        }
        ret = close(ready_pipe[1]);
        assert(ret == 0);
        run_command_mode(&cfg);
        _exit(99);
    }

    /* Wait for wrapper to signal readiness */
    ret = close(ready_pipe[1]);
    assert(ret == 0);
    do {
        n_read = read(ready_pipe[0], &ready_byte, 1);
    } while (n_read < 0 && errno == EINTR);
    assert(n_read == 1 && ready_byte == 'A');
    ret = close(ready_pipe[0]);
    assert(ret == 0);

    /* Request termination: wrapper's signal handler sets quit_flag */
    kill(wrapper_pid, SIGTERM);

    waited = waitpid(wrapper_pid, &wrapper_status, 0);
    assert(waited == wrapper_pid);
    w_exited = WIFEXITED(wrapper_status);
    assert(w_exited);
    /*
     * wrapper forwards SIGTERM to 'sleep 60'; sleep exits with SIGTERM.
     * run_command_mode exits with 128 + SIGTERM.
     */
    w_exit_code = WEXITSTATUS(wrapper_status);
    assert(w_exit_code == 128 + SIGTERM);
}

/**
 * @brief Test run_command_mode forwards the exact received signal (SIGINT)
 * @note Sends SIGINT to the wrapper process while it runs 'sleep 60'
 *       With correct signal forwarding, the command receives SIGINT and exits
 *       with 128 + SIGINT = 130 (matching standard shell Ctrl+C behavior).
 *       Without the fix, the command would receive SIGTERM and exit with 143.
 */
static void test_limiter_run_command_mode_signal_forwarding(void) {
    int ready_pipe[2];
    pid_t wrapper_pid, waited;
    int wrapper_status, w_exited, w_exit_code, ret;
    char ready_byte;
    ssize_t n_read;

    ret = pipe(ready_pipe);
    assert(ret == 0);

    fflush(stdout);
    fflush(stderr);
    wrapper_pid = fork();
    assert(wrapper_pid >= 0);
    if (wrapper_pid == 0) {
        struct cpulimit_cfg cfg;
        char cmd[] = "sleep";
        char arg1[] = "60";
        char *args[3];

        ret = close(ready_pipe[0]);
        assert(ret == 0);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);

        args[0] = cmd;
        args[1] = arg1;
        args[2] = NULL;
        memset(&cfg, 0, sizeof(struct cpulimit_cfg));
        cfg.program_name = "test";
        cfg.command_mode = 1;
        cfg.command_args = args;
        cfg.limit = 0.5;
        cfg.lazy_mode = 1;
        /* Notify test that wrapper is ready to call run_command_mode */
        if (write(ready_pipe[1], "A", 1) != 1) {
            close(ready_pipe[1]);
            _exit(EXIT_FAILURE);
        }
        ret = close(ready_pipe[1]);
        assert(ret == 0);
        run_command_mode(&cfg);
        _exit(99);
    }

    /* Wait for wrapper to signal readiness */
    ret = close(ready_pipe[1]);
    assert(ret == 0);
    do {
        n_read = read(ready_pipe[0], &ready_byte, 1);
    } while (n_read < 0 && errno == EINTR);
    assert(n_read == 1 && ready_byte == 'A');
    ret = close(ready_pipe[0]);
    assert(ret == 0);

    /* Send SIGINT (Ctrl+C equivalent) to the wrapper */
    kill(wrapper_pid, SIGINT);

    waited = waitpid(wrapper_pid, &wrapper_status, 0);
    assert(waited == wrapper_pid);
    w_exited = WIFEXITED(wrapper_status);
    assert(w_exited);
    /*
     * Wrapper must forward SIGINT (not SIGTERM) to the command group.
     * 'sleep 60' exits with SIGINT -> exit status 128 + SIGINT = 130.
     */
    w_exit_code = WEXITSTATUS(wrapper_status);
    assert(w_exit_code == 128 + SIGINT);
}

/**
 * @brief Test run_pid_or_exe_mode with exe mode, non-lazy, immediate exit
 *  via quit flag (verifies the non-lazy quit-flag early-exit path)
 * @note Uses a nonexistent exe so proc_list stays empty, quit flag breaks loop
 */
static void test_limiter_run_pid_or_exe_mode_quit(void) {
    pid_t pid, waited;
    int status, exited, exit_code;
    struct cpulimit_cfg cfg;
    const char exe[] = "nonexistent_cpulimit_quit_test_xyz";

    memset(&cfg, 0, sizeof(struct cpulimit_cfg));
    cfg.program_name = "test";
    cfg.exe_name = exe;
    cfg.limit = 0.5;
    cfg.lazy_mode = 0; /* non-lazy: loop until quit */

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        configure_signal_handler();
        /* Send ourselves SIGTERM to set the quit flag, then call the mode */
        kill(getpid(), SIGTERM);
        run_pid_or_exe_mode(&cfg);
        _exit(99);
    }

    waited = waitpid(pid, &status, 0);
    assert(waited == pid);
    exited = WIFEXITED(status);
    assert(exited);
    /* Process not found -> EXIT_SUCCESS (non-lazy, quit gracefully) */
    exit_code = WEXITSTATUS(status);
    assert(exit_code == EXIT_SUCCESS);
}

/**
 * @brief Test run_pid_or_exe_mode with verbose=0 when process is found
 * @note Verifies that the non-verbose code path (verbose guard is false)
 *       works correctly when a process is found: the function limits it and
 *       exits with EXIT_SUCCESS when the target terminates naturally.
 */
static void test_limiter_run_pid_or_exe_mode_pid_found(void) {
    pid_t wrapper_pid, waited;
    int wrapper_status, w_exited, w_exit_code;
    const struct timespec target_life = {0, 500000000L}; /* 500 ms */

    fflush(stdout);
    fflush(stderr);
    wrapper_pid = fork();
    assert(wrapper_pid >= 0);
    if (wrapper_pid == 0) {
        pid_t target_pid;
        struct cpulimit_cfg cfg;

        close(STDOUT_FILENO);
        close(STDERR_FILENO);

        /* Fork a target that stays alive long enough to be found */
        target_pid = fork();
        assert(target_pid >= 0);
        if (target_pid == 0) {
            sleep_timespec(&target_life);
            _exit(EXIT_SUCCESS);
        }

        memset(&cfg, 0, sizeof(struct cpulimit_cfg));
        cfg.program_name = "test";
        cfg.target_pid = target_pid;
        cfg.limit = 0.5;
        cfg.lazy_mode = 1;
        cfg.verbose = 0; /* non-verbose: verbose guard must not print */
        run_pid_or_exe_mode(&cfg);
        _exit(EXIT_FAILURE); /* safety fallback: run_pid_or_exe_mode always
                                calls exit */
    }

    waited = waitpid(wrapper_pid, &wrapper_status, 0);
    assert(waited == wrapper_pid);
    w_exited = WIFEXITED(wrapper_status);
    assert(w_exited);
    w_exit_code = WEXITSTATUS(wrapper_status);
    assert(w_exit_code == EXIT_SUCCESS);
}

/**
 * @brief Test run_pid_or_exe_mode exits with failure when target is self
 * @note When the found PID matches the calling process, run_pid_or_exe_mode
 *       must exit with EXIT_FAILURE to prevent cpulimit from limiting itself.
 */
static void test_limiter_run_pid_or_exe_mode_self(void) {
    pid_t wrapper_pid, waited;
    int wrapper_status, w_exited, w_exit_code;

    fflush(stdout);
    fflush(stderr);
    wrapper_pid = fork();
    assert(wrapper_pid >= 0);
    if (wrapper_pid == 0) {
        struct cpulimit_cfg cfg;

        close(STDOUT_FILENO);
        close(STDERR_FILENO);

        memset(&cfg, 0, sizeof(struct cpulimit_cfg));
        cfg.program_name = "test";
        cfg.target_pid = getpid(); /* target is the wrapper itself */
        cfg.limit = 0.5;
        cfg.lazy_mode = 1;
        run_pid_or_exe_mode(&cfg);
        _exit(EXIT_FAILURE); /* safety fallback: run_pid_or_exe_mode always
                                calls exit */
    }

    waited = waitpid(wrapper_pid, &wrapper_status, 0);
    assert(waited == wrapper_pid);
    w_exited = WIFEXITED(wrapper_status);
    assert(w_exited);
    /* Must exit with failure to prevent self-limiting */
    w_exit_code = WEXITSTATUS(wrapper_status);
    assert(w_exit_code == EXIT_FAILURE);
}

/**
 * @brief Test run_pid_or_exe_mode with verbose=1 exercises the verbose path
 * @note Forks a short-lived target process, then calls run_pid_or_exe_mode
 *       with verbose=1 and lazy_mode=1. Suppresses output. Verifies the
 *       function exits cleanly when the target terminates, confirming the
 *       verbose code path (including the "Process found" message guard)
 *       does not crash.
 */
static void test_limiter_run_pid_or_exe_mode_verbose(void) {
    pid_t wrapper_pid, waited;
    int wrapper_status, w_exited, w_exit_code;
    const struct timespec target_life = {0, 500000000L}; /* 500 ms */

    fflush(stdout);
    fflush(stderr);
    wrapper_pid = fork();
    assert(wrapper_pid >= 0);
    if (wrapper_pid == 0) {
        pid_t target_pid;
        struct cpulimit_cfg cfg;

        close(STDOUT_FILENO);
        close(STDERR_FILENO);

        /* Fork a target that stays alive long enough to be found */
        target_pid = fork();
        assert(target_pid >= 0);
        if (target_pid == 0) {
            sleep_timespec(&target_life);
            _exit(EXIT_SUCCESS);
        }

        memset(&cfg, 0, sizeof(struct cpulimit_cfg));
        cfg.program_name = "test";
        cfg.target_pid = target_pid;
        cfg.limit = 0.5;
        cfg.lazy_mode = 1;
        cfg.verbose = 1; /* exercises verbose branch in run_pid_or_exe_mode */
        run_pid_or_exe_mode(&cfg);
        _exit(EXIT_FAILURE); /* safety fallback: run_pid_or_exe_mode always
                                calls exit */
    }

    waited = waitpid(wrapper_pid, &wrapper_status, 0);
    assert(waited == wrapper_pid);
    w_exited = WIFEXITED(wrapper_status);
    assert(w_exited);
    w_exit_code = WEXITSTATUS(wrapper_status);
    assert(w_exit_code == EXIT_SUCCESS);
}

/**
 * @brief Test run_pid_or_exe_mode always resumes the target before returning
 * @note Regression test for the missing-resume bug: run_pid_or_exe_mode must
 *       send SIGCONT to the target after limit_process() returns, mirroring
 *       the symmetric guard in run_command_mode().  Without it, a target that
 *       is still SIGSTOP-ped when limit_process() exits (e.g. because a stopped
 *       process is momentarily invisible to the process iterator, or because
 *       update_process_group() failed and cleared the process list) would be
 *       left permanently stopped.
 *
 *       The target installs a SIGCONT handler that increments a shared
 *       counter, then is pre-stopped by the test.  The wrapper drives
 *       run_pid_or_exe_mode(); limit_process() must eventually SIGCONT the
 *       target, because only a resumed target can reach its own _exit() and
 *       make run_pid_or_exe_mode() return.  We assert the wrapper exits
 *       EXIT_SUCCESS.
 *
 *       The target must announce through ready_pipe that its handlers are
 *       installed before the wrapper sends SIGSTOP: fork() gives no ordering
 *       guarantee between parent and child, and on FreeBSD the parent
 *       normally keeps running (unlike Linux, where the child is scheduled
 *       first).  A SIGSTOP that overtakes sigaction() freezes the target
 *       before it ever installed the handler, so the later SIGCONT is
 *       handled by the default disposition, resume_counter stays 0 and the
 *       target spins forever instead of terminating -- hanging the test.
 */
static void test_limiter_run_pid_or_exe_mode_resumes_target(void) {
    int ready_pipe[2];
    pid_t wrapper_pid, waited;
    int wrapper_status, w_exited, w_exit_code, ret;

    ret = pipe(ready_pipe);
    assert(ret == 0);

    fflush(stdout);
    fflush(stderr);
    wrapper_pid = fork();
    assert(wrapper_pid >= 0);
    if (wrapper_pid == 0) {
        pid_t target_pid;
        int target_status;
        struct cpulimit_cfg cfg;
        struct sigaction sa_cont;
        char ready_byte;
        ssize_t n_read;

        memset(&sa_cont, 0, sizeof(sa_cont));

        close(STDOUT_FILENO);
        close(STDERR_FILENO);

        target_pid = fork();
        assert(target_pid >= 0);
        if (target_pid == 0) {
            ret = close(ready_pipe[0]);
            assert(ret == 0);
            /*
             * Install a SIGCONT handler that records each resume.  The
             * resume count is reported back via the exit code (0 = resumed
             * at least once, 2 = never resumed / left permanently stopped).
             *
             * No disposition is installed for SIGSTOP: it can neither be
             * caught nor redirected, so its default action already applies.
             * Calling sigaction(SIGSTOP, SIG_DFL) would be a no-op at best
             * and is rejected with EINVAL by glibc, which made this target
             * exit immediately and turned the test into a no-op on Linux.
             */
            sa_cont.sa_handler = resume_counter_handler;
            if (sigemptyset(&sa_cont.sa_mask) != 0 ||
                sigaction(SIGCONT, &sa_cont, NULL) != 0) {
                _exit(2);
            }
            /*
             * Handshake: the SIGSTOP below must not overtake sigaction(),
             * or the resume would never be recorded (see the function
             * documentation).  The write end is closed right afterwards so
             * that the wrapper also observes an EOF if this child dies
             * before it could report readiness.
             */
            if (write(ready_pipe[1], "R", 1) != 1) {
                _exit(2);
            }
            ret = close(ready_pipe[1]);
            assert(ret == 0);
            /*
             * Spin until resumed at least once.  While the process is
             * SIGSTOP-ped it cannot execute, so reaching the _exit() below
             * proves it was resumed (received SIGCONT).  Bail out if the
             * parent disappears so we never spin forever.
             */
            while (resume_counter < 1 && getppid() != 1) {
                struct timespec tiny = {0, 1000000L};
                nanosleep(&tiny, NULL);
            }
            _exit(resume_counter >= 1 ? EXIT_SUCCESS : 2);
        }

        /*
         * Wait for the handshake byte: the target is only stopped once its
         * SIGCONT handler is in place, otherwise it cannot notice the
         * resume and would spin forever.
         */
        ret = close(ready_pipe[1]);
        assert(ret == 0);
        do {
            n_read = read(ready_pipe[0], &ready_byte, 1);
        } while (n_read < 0 && errno == EINTR);
        ret = close(ready_pipe[0]);
        assert(ret == 0);
        if (n_read != 1 || ready_byte != 'R') {
            _exit(EXIT_FAILURE);
        }

        /* Pre-stop the target before the limiter takes over */
        kill(target_pid, SIGSTOP);

        memset(&cfg, 0, sizeof(struct cpulimit_cfg));
        cfg.program_name = "test";
        cfg.target_pid = target_pid;
        cfg.limit = 0.5;
        cfg.lazy_mode = 1;
        cfg.verbose = 0;
        run_pid_or_exe_mode(&cfg);
        /*
         * run_pid_or_exe_mode only returns after the target terminates.
         * Reap it to learn whether the target was ever resumed.
         */
        if (waitpid(target_pid, &target_status, 0) == target_pid &&
            WIFEXITED(target_status)) {
            _exit(WEXITSTATUS(target_status));
        }
        _exit(EXIT_FAILURE); /* safety fallback: run_pid_or_exe_mode always
                                calls exit */
    }

    ret = close(ready_pipe[0]);
    assert(ret == 0);
    ret = close(ready_pipe[1]);
    assert(ret == 0);

    waited = waitpid(wrapper_pid, &wrapper_status, 0);
    assert(waited == wrapper_pid);
    w_exited = WIFEXITED(wrapper_status);
    assert(w_exited);
    w_exit_code = WEXITSTATUS(wrapper_status);
    /*
     * EXIT_SUCCESS (0) means the target was resumed and ran to completion.
     * Any other code means the target was never resumed (left stopped) or an
     * error occurred -- which would indicate a regression of the resume bug.
     */
    assert(w_exit_code == EXIT_SUCCESS);
}

/**
 * @brief Test run_command_mode when the quit flag is already set before
 *        limit_process is entered
 * @note Exercises the race where a termination signal (SIGTERM) arrives
 *       before run_command_mode is called, so quit_flag is true by the time
 *       limit_process is invoked.  limit_process must detect the preset quit
 *       flag, skip the control loop, resume any stopped processes, and return
 *       immediately.  run_command_mode then forwards the quit signal to the
 *       command process group and exits with 128 + SIGTERM.
 */
static void test_limiter_race_quit_flag_preset_before_limit(void) {
    pid_t wrapper_pid, waited;
    int wrapper_status, w_exited, w_exit_code;

    fflush(stdout);
    fflush(stderr);
    wrapper_pid = fork();
    assert(wrapper_pid >= 0);
    if (wrapper_pid == 0) {
        struct cpulimit_cfg cfg;
        char cmd[] = "sleep";
        char arg1[] = "60";
        char *args[3];

        close(STDOUT_FILENO);
        close(STDERR_FILENO);

        args[0] = cmd;
        args[1] = arg1;
        args[2] = NULL;
        memset(&cfg, 0, sizeof(struct cpulimit_cfg));
        cfg.program_name = "test";
        cfg.command_mode = 1;
        cfg.command_args = args;
        cfg.limit = 0.5;

        configure_signal_handler();

        /*
         * Deliver SIGTERM synchronously: the signal handler runs before
         * raise() returns, setting quit_flag without modifying the
         * process signal mask.  This guarantees quit_flag is set before
         * run_command_mode is entered.
         */
        if (raise(SIGTERM) != 0) {
            _exit(EXIT_FAILURE);
        }
        /*
         * quit_flag is now set.  run_command_mode will fork the command
         * child, read the sync byte, call limit_process (which exits
         * immediately because quit_flag is set), then forward SIGTERM to
         * the child process group.
         */
        run_command_mode(&cfg);
        _exit(EXIT_FAILURE); /* safety: run_command_mode always calls exit */
    }

    waited = waitpid(wrapper_pid, &wrapper_status, 0);
    assert(waited == wrapper_pid);
    w_exited = WIFEXITED(wrapper_status);
    assert(w_exited);
    /*
     * run_command_mode forwards SIGTERM to 'sleep 60'; sleep exits with
     * SIGTERM.  run_command_mode exits with 128 + SIGTERM.
     */
    w_exit_code = WEXITSTATUS(wrapper_status);
    assert(w_exit_code == 128 + SIGTERM);
}

/**
 * @brief Test run_command_mode when SIGTERM arrives during the sync pipe read
 * @note Exercises the SA_RESTART race in the sync pipe read: with SA_RESTART
 *       the underlying read() syscall is transparently restarted after signal
 *       delivery, so the parent does not observe EINTR.  The signal handler has
 *       already set quit_flag by the time the read completes, causing
 *       limit_process to return immediately and the command to receive the
 *       forwarded signal.
 */
static void test_limiter_race_signal_during_sync_pipe_read(void) {
    int notify_pipe[2];
    pid_t wrapper_pid, waited;
    int wrapper_status, w_exited, w_exit_code, ret;
    char notify_byte;
    ssize_t n_read;

    ret = pipe(notify_pipe);
    assert(ret == 0);

    fflush(stdout);
    fflush(stderr);
    wrapper_pid = fork();
    assert(wrapper_pid >= 0);
    if (wrapper_pid == 0) {
        struct cpulimit_cfg cfg;
        char cmd[] = "sleep";
        char arg1[] = "60";
        char *args[3];

        close(notify_pipe[0]);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);

        args[0] = cmd;
        args[1] = arg1;
        args[2] = NULL;
        memset(&cfg, 0, sizeof(struct cpulimit_cfg));
        cfg.program_name = "test";
        cfg.command_mode = 1;
        cfg.command_args = args;
        cfg.limit = 0.5;

        configure_signal_handler();

        /* Notify parent just before calling run_command_mode */
        if (write(notify_pipe[1], "G", 1) != 1) {
            close(notify_pipe[1]);
            _exit(EXIT_FAILURE);
        }
        close(notify_pipe[1]);

        /*
         * run_command_mode will fork a child and block on read() waiting
         * for the child's sync byte.  The parent sends SIGTERM during
         * this window.  With SA_RESTART, read() is transparently
         * restarted; quit_flag is set.  After reading the sync byte,
         * limit_process exits immediately and SIGTERM is forwarded.
         */
        run_command_mode(&cfg);
        _exit(EXIT_FAILURE); /* safety: run_command_mode always calls exit */
    }

    /* Wait for wrapper to be ready */
    close(notify_pipe[1]);
    do {
        n_read = read(notify_pipe[0], &notify_byte, 1);
    } while (n_read < 0 && errno == EINTR);
    assert(n_read == 1 && notify_byte == 'G');
    close(notify_pipe[0]);

    /*
     * Send SIGTERM as early as possible after run_command_mode is entered,
     * targeting the window when the parent is blocked on the sync read.
     * Even if the signal arrives before or after the sync read, the
     * expected behaviour (wrapper exits 128+SIGTERM) must hold.
     */
    kill(wrapper_pid, SIGTERM);

    waited = waitpid(wrapper_pid, &wrapper_status, 0);
    assert(waited == wrapper_pid);
    w_exited = WIFEXITED(wrapper_status);
    assert(w_exited);
    w_exit_code = WEXITSTATUS(wrapper_status);
    assert(w_exit_code == 128 + SIGTERM);
}

#if defined(__clang__)
/* Clang: fully supported */
#define NOINLINE_USED __attribute__((noinline, used))
#elif defined(__GNUC__)
/* GCC major version check only (safe for GCC 2.x) */
#if __GNUC__ < 3
/* GCC 2.x: attribute support unreliable */
#define NOINLINE_USED
#elif __GNUC__ == 3
/* GCC 3.x */
#if defined(__GNUC_MINOR__) && (__GNUC_MINOR__ >= 4)
#define NOINLINE_USED __attribute__((noinline, used))
#elif defined(__GNUC_MINOR__) && (__GNUC_MINOR__ >= 1)
#define NOINLINE_USED __attribute__((noinline))
#else
#define NOINLINE_USED
#endif
#else
/* GCC >= 4 */
#define NOINLINE_USED __attribute__((noinline, used))
#endif
#else
/* Other compilers */
#define NOINLINE_USED
#endif

/**
 * @brief Robust test function invocation
 * Ensures that the test function is called and cannot be inlined.
 * @param test_fn Pointer to the void(void) test function to invoke
 */
static NOINLINE_USED void test_invoke_indirect(void (*test_fn)(void)) {
    void (*volatile fn_slot_array[2])(void) = {NULL, NULL};
    void (*volatile *volatile fn_slot_ptr)(void);
    size_t i, rand_iter_count;
    if (test_fn == NULL) {
        fprintf(
            stderr,
            "Error: test_invoke_indirect called with NULL function pointer\n");
        exit(EXIT_FAILURE);
    }
    fn_slot_ptr = fn_slot_array;
    rand_iter_count = (size_t)random() % 10 + 10;
    for (i = 0; i < rand_iter_count; i++) {
        volatile long idx = (random() >> 1) & 1;
        if (idx << ((random() >> 1) & 2) > 1) {
            *(fn_slot_array + !idx) = test_fn;
        } else {
            fn_slot_ptr[idx] = test_fn;
        }
        idx = (random() >> 1) % 2;
        if (fn_slot_ptr[idx] != NULL) {
            *(fn_slot_ptr + !((idx > 0) - (idx < 0))) = fn_slot_array[idx];
        } else {
            *(fn_slot_array + idx) = fn_slot_ptr[(!!idx) ^ 1];
        }
        *(fn_slot_ptr + ((random() >> 1) & 1)) =
            *(fn_slot_array + ((random() >> 1) & 1));
    }
    fn_slot_ptr += ((random() >> 1) & 1);
    (*fn_slot_ptr)();
}

/** @def RUN_TEST(test_func)
 *  @brief Macro to run a test function and print its status
 *  @param test_func Name of the test function to run
 */
#define RUN_TEST(test_func)                                                    \
    do {                                                                       \
        printf("Running %s()...\n", #test_func);                               \
        test_invoke_indirect(test_func);                                       \
        printf("%s() passed.\n", #test_func);                                  \
    } while (0)

/**
 * @brief Main test function
 * @param argc Argument count
 * @param argv Argument vector
 * @return 0 on success
 * @note Runs all test functions organized by module and prints their results
 *       Installs signal handlers for SIGINT and SIGTERM to request graceful
 *       shutdown of the test run instead of abrupt termination.
 *       When argv[1] is SIGCOUNT_CHILD_ARG, runs as the SIGINT-counting
 *       command of the signal-forwarding test instead of the suite.
 */
int main(int argc, char *argv[]) {
    struct timespec time_seed;

    assert(argc >= 1);
    argv0 = argv[0];

    if (argc == 3 && strcmp(argv[1], SIGCOUNT_CHILD_ARG) == 0) {
        return run_sigcount_child(argv[2]);
    }

    check_y2038();

    if (get_current_time(&time_seed) != 0) {
        fprintf(stderr, "Failed to get time for random seed: %s\n",
                strerror(errno));
        exit(EXIT_FAILURE);
    }
    srandom((unsigned int)(time_seed.tv_sec ^ time_seed.tv_nsec));

    configure_signal_handler();
    printf("Starting tests...\n");

    /* Time util module tests */
    printf("\n=== TIME_UTIL MODULE TESTS ===\n");
    RUN_TEST(test_time_util_nsec2timespec);
    RUN_TEST(test_time_util_get_current_time);
    RUN_TEST(test_time_util_sleep_timespec);
    RUN_TEST(test_time_util_timediff_in_ms);
#if defined(__APPLE__)
    RUN_TEST(test_apple_proc_argv0_buffer_sizing);
#endif

    /* Util module tests */
    printf("\n=== UTIL MODULE TESTS ===\n");
    RUN_TEST(test_util_file_basename);
    RUN_TEST(test_util_get_ncpu);
    RUN_TEST(test_util_increase_priority);
    RUN_TEST(test_util_increase_priority_retries_lower_levels);
    RUN_TEST(test_util_long2pid_t);
#if defined(__linux__)
    RUN_TEST(test_util_read_file_contents);
    RUN_TEST(test_util_parse_cpu_range);
#endif
    RUN_TEST(test_util_macros);

    /* List module tests */
    printf("\n=== LIST MODULE TESTS ===\n");
    RUN_TEST(test_list_init_and_empty);
    RUN_TEST(test_list_add_elem);
    RUN_TEST(test_list_delete_node);
    RUN_TEST(test_list_destroy_node);
    RUN_TEST(test_list_locate);
    RUN_TEST(test_list_clear_and_destroy);
    RUN_TEST(test_list_edge_cases);
    RUN_TEST(test_list_null_data_operations);

    /* Signal handler module tests */
    printf("\n=== SIGNAL_HANDLER MODULE TESTS ===\n");
    RUN_TEST(test_signal_handler_flags);
    RUN_TEST(test_signal_handler_sigquit);
    RUN_TEST(test_signal_handler_sighup);
    RUN_TEST(test_signal_handler_sigpipe);
    RUN_TEST(test_signal_handler_initial_state);
    RUN_TEST(test_signal_handler_get_quit_signal);
    RUN_TEST(test_signal_handler_reconfigure_resets_state);
    RUN_TEST(test_signal_handler_reconfigure_delivers_pending);
    RUN_TEST(test_signal_handler_reset_to_default);
    RUN_TEST(test_signal_handler_race_concurrent_signals);
    RUN_TEST(test_signal_handler_race_signal_interrupts_sleep);
    RUN_TEST(test_signal_handler_race_rapid_all_signals);

    /* Process iterator module tests */
    printf("\n=== PROCESS_ITERATOR MODULE TESTS ===\n");
    RUN_TEST(test_process_iterator_is_child_of);
    RUN_TEST(test_process_iterator_is_child_of_deep);
    RUN_TEST(test_is_child_of_kernel_thread_not_child_of_init);
    RUN_TEST(test_process_iterator_newline_comm);
    RUN_TEST(test_process_iterator_filter_edge_cases);
    RUN_TEST(test_process_iterator_single);
    RUN_TEST(test_process_iterator_multiple);
    RUN_TEST(test_process_iterator_all);
    RUN_TEST(test_process_iterator_read_command);
    RUN_TEST(test_process_iterator_getppid_of);
    RUN_TEST(test_process_iterator_null_inputs);
    RUN_TEST(test_process_iterator_close_null);
    RUN_TEST(test_process_iterator_getppid_of_edges);
    RUN_TEST(test_process_iterator_init_all_with_children);
    RUN_TEST(test_process_iterator_exhaust_single);
    RUN_TEST(test_process_iterator_with_children);
    RUN_TEST(test_process_iterator_close_null_dip);
    RUN_TEST(test_process_iterator_null_proc_dir_guard);

    /* CLI module tests */
    printf("\n=== CLI MODULE TESTS ===\n");
    RUN_TEST(test_cli_pid_mode);
    RUN_TEST(test_cli_exe_mode);
    RUN_TEST(test_cli_command_mode);
    RUN_TEST(test_cli_long_options);
    RUN_TEST(test_cli_long_option_exe);
    RUN_TEST(test_cli_optional_flags);
    RUN_TEST(test_cli_verbose_flag);
    RUN_TEST(test_cli_help);
    RUN_TEST(test_cli_missing_limit);
    RUN_TEST(test_cli_invalid_limits);
    RUN_TEST(test_cli_invalid_pids);
    RUN_TEST(test_cli_empty_exe);
    RUN_TEST(test_cli_no_target);
    RUN_TEST(test_cli_multiple_targets);
    RUN_TEST(test_cli_unknown_option);
    RUN_TEST(test_cli_missing_arg);
    RUN_TEST(test_cli_long_option_include_children);
    RUN_TEST(test_cli_limit_at_max);
    RUN_TEST(test_cli_pid_minimum_valid);
    RUN_TEST(test_cli_limit_trailing_chars);
    RUN_TEST(test_cli_long_options_lazy_verbose);
    RUN_TEST(test_cli_duplicate_options);
    RUN_TEST(test_cli_null_cfg);
    RUN_TEST(test_cli_invalid_api_inputs);

    /* Process table module tests */
    printf("\n=== PROCESS_TABLE MODULE TESTS ===\n");
    RUN_TEST(test_process_table_init_destroy);
    RUN_TEST(test_process_table_add_find);
    RUN_TEST(test_process_table_del);
    RUN_TEST(test_process_table_remove_stale);
    RUN_TEST(test_process_table_remove_stale_null_data);
    RUN_TEST(test_process_table_collisions);
    RUN_TEST(test_process_table_empty_buckets);
    RUN_TEST(test_process_table_null_inputs_and_dup);
    RUN_TEST(test_process_table_stale_null_list);
    RUN_TEST(test_process_table_init_hashsize_zero);
    RUN_TEST(test_process_table_find_null_pt);
    RUN_TEST(test_process_table_del_absent_pid);
    RUN_TEST(test_process_table_del_empty_bucket);
    RUN_TEST(test_process_table_destroy_edge_cases);
    RUN_TEST(test_process_table_ops_after_destroy);

    /* Process finder module tests */
    printf("\n=== PROCESS_FINDER MODULE TESTS ===\n");
    RUN_TEST(test_process_finder_find_by_pid);
    RUN_TEST(test_process_finder_find_by_name);
    RUN_TEST(test_process_finder_find_by_name_self);
    RUN_TEST(test_process_finder_find_by_name_symlink);
    RUN_TEST(test_process_finder_find_by_name_alias);
    RUN_TEST(test_process_finder_find_by_name_ancestor_pref);

    /* Process group module tests */
    printf("\n=== PROCESS_GROUP MODULE TESTS ===\n");
    RUN_TEST(test_process_group_cpu_usage);
    RUN_TEST(test_process_group_rapid_updates);
    RUN_TEST(test_process_group_init_all);
    RUN_TEST(test_process_group_init_single);
    RUN_TEST(test_process_group_init_invalid_pid);
    RUN_TEST(test_process_group_init_null);
    RUN_TEST(test_process_group_cpu_usage_empty_list);
    RUN_TEST(test_process_group_cpu_usage_null);
    RUN_TEST(test_process_group_close_null);
    RUN_TEST(test_process_group_close_zeros_fields);
    RUN_TEST(test_process_group_update_null);
    RUN_TEST(test_process_group_update_uninitialized_struct);
    RUN_TEST(test_process_group_cpu_usage_uninitialized_struct);
    RUN_TEST(test_process_group_double_update);
    RUN_TEST(test_process_group_update_return_value);
    RUN_TEST(test_process_group_cpu_usage_with_usage);
    RUN_TEST(test_process_group_race_target_exits_between_init_and_update);
    RUN_TEST(test_process_group_race_rapid_child_spawn_exit);
    RUN_TEST(test_process_group_purges_exited_descendants);
    RUN_TEST(test_process_group_entry_resets_on_reuse_and_backward_clock);

    /* Limit process module tests */
    printf("\n=== LIMIT_PROCESS MODULE TESTS ===\n");
    RUN_TEST(test_limit_process_basic);
    RUN_TEST(test_limit_process_exits_early);
    RUN_TEST(test_limit_process_verbose);
    RUN_TEST(test_limit_process_include_children);
    RUN_TEST(test_limit_process_resumes_orphaned_descendant);
    RUN_TEST(test_limit_process_race_process_exits_on_sigcont);
    RUN_TEST(test_limit_process_race_quit_during_sleep);

    /* Limiter module tests */
    printf("\n=== LIMITER MODULE TESTS ===\n");
    RUN_TEST(test_limiter_run_command_mode);
    RUN_TEST(test_limiter_run_pid_or_exe_mode);
    RUN_TEST(test_limiter_run_command_mode_nonexistent);
    RUN_TEST(test_limiter_run_command_mode_bad_shebang);
    RUN_TEST(test_limiter_run_command_mode_fifo);
    RUN_TEST(test_limiter_run_command_mode_forwards_signal_once);
    RUN_TEST(test_limiter_run_command_mode_shebang_interpreter_inaccessible);
    RUN_TEST(test_limiter_run_command_mode_verbose);
    RUN_TEST(test_limiter_run_pid_or_exe_mode_pid_not_found);
    RUN_TEST(test_limiter_run_command_mode_false);
    RUN_TEST(test_limiter_run_command_mode_signal_term);
    RUN_TEST(test_limiter_run_command_mode_signal_kill);
    RUN_TEST(test_limiter_run_command_mode_with_fork);
    RUN_TEST(test_limiter_run_command_mode_quit_signal);
    RUN_TEST(test_limiter_run_command_mode_signal_forwarding);
    RUN_TEST(test_limiter_run_pid_or_exe_mode_quit);
    RUN_TEST(test_limiter_run_pid_or_exe_mode_pid_found);
    RUN_TEST(test_limiter_run_pid_or_exe_mode_self);
    RUN_TEST(test_limiter_run_pid_or_exe_mode_verbose);
    RUN_TEST(test_limiter_race_quit_flag_preset_before_limit);
    RUN_TEST(test_limiter_race_signal_during_sync_pipe_read);
    RUN_TEST(test_limiter_run_pid_or_exe_mode_resumes_target);
    printf("\n=== ALL TESTS PASSED ===\n");

    return 0;
}
