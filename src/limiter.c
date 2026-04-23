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

#include "limiter.h"

#include "cli.h"
#include "limit_process.h"
#include "process_finder.h"
#include "signal_handler.h"
#include "time_util.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/**
 * @def EXIT_CMD_NOT_EXECUTABLE
 * @brief Shell-compatible exit code: command found but not executable
 *
 * Used when the target file exists but cannot be executed (e.g., missing
 * execute permission, unsupported binary format, or missing shebang
 * interpreter).  Mirrors the POSIX shell convention for exit status 126.
 */
#define EXIT_CMD_NOT_EXECUTABLE 126

/**
 * @def EXIT_CMD_NOT_FOUND
 * @brief Shell-compatible exit code for execvp() ENOENT failures
 *
 * Used when execvp() fails with ENOENT and the limiter cannot verify that
 * the target file exists (for example, for a non-absolute argv[0] looked
 * up via PATH). This includes the classic "command not found" case as well
 * as other ENOENT-at-exec scenarios. Mirrors the POSIX shell convention
 * for exit status 127.
 */
#define EXIT_CMD_NOT_FOUND 127

/**
 * @def CHILD_KILL_TIMEOUT_MS
 * @brief Timeout in milliseconds before escalating to SIGKILL
 *
 * After the limiter decides to terminate the child process group, it
 * polls waitpid() in a loop.  If the child has not exited within this
 * many milliseconds, SIGKILL is sent to the entire process group.
 */
#define CHILD_KILL_TIMEOUT_MS 5000

/**
 * @def CHILD_POLL_INTERVAL_NS
 * @brief Nanoseconds between waitpid() polls during child cleanup
 *
 * While waiting for the child to exit (in collect_child_exit_status()),
 * the parent sleeps for this interval between non-blocking waitpid()
 * calls to avoid busy-waiting.
 */
#define CHILD_POLL_INTERVAL_NS 50000000L /* 50 ms */

/**
 * @brief Check whether a file is a script with a missing shebang interpreter.
 * @param path Path to the file to inspect.
 * @return 1 if the file begins with "#!" and the interpreter named on the
 *         shebang line cannot be accessed (access(F_OK) fails); 0 otherwise.
 *
 * This pre-exec check avoids calling execvp() on a script whose interpreter
 * is absent.  Under normal execution the kernel returns ENOENT in that case,
 * but under debugging tools such as valgrind the execve() interception is
 * unrecoverable, so the check must be made before exec.
 */
static int is_script_missing_interpreter(const char *path) {
    int fd;
    char buf[256];
    ssize_t n;
    char *p;
    char *end;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    /* Not a script if too short or no shebang prefix */
    if (n < 2 || buf[0] != '#' || buf[1] != '!') {
        return 0;
    }
    buf[n] = '\0';

    /* Skip optional whitespace after "#!" */
    p = buf + 2;
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    /* Find end of interpreter path (terminated by whitespace, newline, NUL) */
    end = p;
    while (*end != '\0' && *end != '\n' && *end != '\r' && *end != ' ' &&
           *end != '\t') {
        end++;
    }
    *end = '\0';

    if (*p == '\0') {
        return 0; /* Empty shebang line */
    }

    /* Interpreter path does not exist -> report 126 */
    return access(p, F_OK) != 0;
}

/**
 * @brief Execute a child process for command mode.
 * @param cfg Pointer to configuration structure containing command and options.
 * @param sync_read_fd Read end of the synchronization pipe.
 * @param sync_write_fd Write end of the synchronization pipe.
 *
 * This function executes in the child process after fork(). It sets up the
 * process group, resets signal handlers, signals readiness to the parent,
 * and replaces the process image with the user command via execvp().
 *
 * @note This function never returns; it calls _exit() on any failure.
 */
static void exec_child_process(const struct cpulimit_cfg *cfg, int sync_read_fd,
                               int sync_write_fd) {
    /*
     * This block executes in the child process.
     * The child will become the command specified by the user.
     */
    int saved_errno;

    /*
     * Create new process group with child as leader.
     * This allows limiting the entire process tree (child + descendants)
     * and enables sending signals to all related processes via -PGID.
     */
    if (setpgid(0, 0) < 0) {
        perror("setpgid");
        close(sync_read_fd);
        close(sync_write_fd);
        goto error_out;
    }

    /*
     * Reset inherited signal handlers to SIG_DFL before notifying
     * the parent.  After fork(), this child inherits the parent's
     * configured handlers.  execvp() resets them, but on systems
     * where exec takes measurable time (e.g., macOS 10.15+ with
     * library validation), a signal forwarded by the parent between
     * reading the sync byte and exec completing would be silently
     * caught by the inherited handler instead of terminating this
     * process.  Resetting here closes that race window.
     */
    if (reset_signal_handlers_to_default() != 0) {
        close(sync_read_fd);
        close(sync_write_fd);
        goto error_out;
    }

    /* Close unused read end of pipe */
    close(sync_read_fd);
    /*
     * Signal parent that child initialization is complete.
     * Parent blocks until receiving this byte.
     * Do NOT close sync_write_fd here: it must remain open until exec
     * so that the close-on-exec flag (FD_CLOEXEC) causes it to be
     * closed automatically on a successful execvp(), signalling exec
     * completion to the parent.  On exec failure the code below closes
     * it explicitly.
     */
    if (write(sync_write_fd, "A", 1) != 1) {
        perror("write sync");
        close(sync_write_fd);
        goto error_out;
    }

    /*
     * Replace child process image with the user command.
     * execvp() searches PATH for the executable and transfers control.
     * If successful, this function never returns and the close-on-exec
     * flag (FD_CLOEXEC) on sync_write_fd causes the kernel to close it
     * automatically, signalling exec completion to the parent.
     *
     * Pre-check: if the target is an explicit path, detect a script
     * whose shebang interpreter is missing before calling execvp().
     * Under normal execution execvp() would return ENOENT in this case,
     * but under valgrind the exec interception is unrecoverable, so the
     * check must happen before exec.  _exit() closes all fds (including
     * sync_write_fd), which also signals exec completion to the parent.
     */
    if (strchr(cfg->command_args[0], '/') != NULL &&
        is_script_missing_interpreter(cfg->command_args[0])) {
        fprintf(stderr, "%s: cannot execute: shebang interpreter not found\n",
                cfg->command_args[0]);
        _exit(EXIT_CMD_NOT_EXECUTABLE);
    }
    execvp(cfg->command_args[0], cfg->command_args);

    /*
     * Execution reaches here only if execvp() failed.
     * Use shell-compatible exit codes:
     * - EXIT_CMD_NOT_FOUND (127): command not found (ENOENT with no
     *   existing file)
     * - EXIT_CMD_NOT_EXECUTABLE (126): found but not executable
     *   (permission denied, or ENOENT when file exists but its
     *   interpreter is absent)
     *
     * Note: ENOENT can occur for two distinct reasons:
     * 1. The command (or a PATH-resolved name) does not exist -> 127.
     * 2. The file exists but exec fails (e.g., missing dynamic linker
     *    on an explicit-path binary) -> 126.
     * Case 2 is detectable only when the argument contains a '/'
     * (an explicit path), because only then can access(F_OK) confirm
     * that the file itself is present.
     *
     * Close sync_write_fd explicitly to signal exec failure to the
     * parent (the FD_CLOEXEC path only applies on success).
     */
    saved_errno = errno;
    perror("execvp");
    close(sync_write_fd);
    if (saved_errno == ENOENT && strchr(cfg->command_args[0], '/') != NULL &&
        access(cfg->command_args[0], F_OK) == 0) {
        _exit(EXIT_CMD_NOT_EXECUTABLE);
    }
    _exit(saved_errno == ENOENT ? EXIT_CMD_NOT_FOUND : EXIT_CMD_NOT_EXECUTABLE);

error_out:
    _exit(EXIT_FAILURE);
}

/**
 * @brief Wait for the child process to complete exec setup.
 * @param child_pid PID of the forked child process.
 * @param sync_read_fd Read end of the synchronization pipe.
 *
 * Reads the readiness byte written by the child after setpgid() and signal
 * handler reset, then blocks until the pipe EOF that indicates exec has
 * completed (or the child has exited on exec failure). Closes sync_read_fd
 * on return.
 *
 * On any error, kills and reaps the child, then calls exit(EXIT_FAILURE).
 */
static void wait_for_child_exec(pid_t child_pid, int sync_read_fd) {
    char sync_byte; /* Synchronization byte from child */
    ssize_t n_read; /* Bytes read from pipe */

    /*
     * Block until child signals readiness by writing to pipe.
     * This ensures child has completed setpgid() before parent continues.
     */
    do {
        n_read = read(sync_read_fd, &sync_byte, 1);
    } while (n_read < 0 && errno == EINTR);
    if (n_read != 1 || sync_byte != 'A') {
        pid_t wait_result; /* Return value of waitpid in error path */
        if (n_read < 0) {
            perror("read sync");
        } else if (n_read == 0) {
            fprintf(stderr, "Synchronization pipe closed before child setup\n");
        } else {
            fprintf(stderr, "Unexpected synchronization value from child\n");
        }
        close(sync_read_fd);
        /*
         * Kill child to prevent it from becoming an orphan, then reap
         * it to prevent zombie. SIGKILL is used because the child may
         * have failed in an unknown state and cannot be trusted to
         * respond to SIGTERM. SIGKILL cannot be caught or ignored, so
         * the subsequent blocking waitpid() will complete quickly.
         */
        kill(child_pid, SIGKILL);
        /*
         * Robustly reap the child: retry waitpid() on EINTR so that
         * a signal delivered to the parent during the wait does not
         * leave the child as a zombie. For any other error, log it and
         * proceed with the fatal exit.
         */
        do {
            wait_result = waitpid(child_pid, NULL, 0);
        } while (wait_result == -1 && errno == EINTR);
        if (wait_result == -1) {
            perror("waitpid");
        }
        exit(EXIT_FAILURE);
    }
    /*
     * Wait for exec to complete (or the child to exit on exec failure).
     * The write end of sync_pipe has FD_CLOEXEC set, so a successful exec
     * closes it automatically; on exec failure the child closes it
     * explicitly before _exit().  Either way, this read returns EOF
     * (n_read == 0), confirming that the child's process image is ready
     * to receive signals.  n_read > 0 is not expected but also signals
     * completion.  This eliminates the race where a signal (e.g. SIGTERM)
     * is sent while the child is still in the middle of exec setup, which
     * is critical under tools such as valgrind that intercept execve and
     * may not handle signals safely during their exec interception phase.
     */
    do {
        n_read = read(sync_read_fd, &sync_byte, 1);
    } while (n_read < 0 && errno == EINTR);
    close(sync_read_fd);
    if (n_read < 0) {
        pid_t wait_result;
        perror("read exec-sync");
        kill(child_pid, SIGKILL);
        do {
            wait_result = waitpid(child_pid, NULL, 0);
        } while (wait_result == -1 && errno == EINTR);
        if (wait_result == -1) {
            perror("waitpid");
        }
        exit(EXIT_FAILURE);
    }
}

/**
 * @brief Wait for the child process to exit and collect its exit status.
 * @param child_pid PID of the child process to wait for.
 * @param cfg Pointer to configuration structure (used for verbose output).
 * @return The child's exit status if successfully reaped, EXIT_FAILURE
 *         otherwise.
 *
 * Polls for the child's termination, applying a 5-second SIGKILL timeout
 * if the child does not exit in time. Translates signal termination to
 * shell-compatible exit codes (128 + signal number).
 *
 * Handles the race where a quit signal is delivered after
 * run_command_mode()'s is_quit_flag_set() check: if quit_flag becomes set
 * during polling, the exact received signal is forwarded to the child
 * process group so it exits with the correct status (128 + signal number)
 * rather than waiting for the SIGKILL timeout.  A SIGCONT is always sent
 * first so that a stopped child (e.g. on macOS 10.7 where stopped processes
 * are invisible to the process iterator) is resumed before the forwarded
 * signal is delivered.
 *
 * A two-phase forwarding check (sig_forwarded: 0 -> -1 -> 1) prevents
 * double-delivery when run_command_mode() has already forwarded the signal
 * before this function is called: the first poll that observes quit_flag
 * only marks the observation; the actual forward happens on the next poll.
 * This one-poll grace period allows a signal sent by run_command_mode() to
 * be delivered and acted upon before this function sends a duplicate.
 *
 * start_time is reset to the moment the signal is forwarded from inside
 * this function, giving the child the full CHILD_KILL_TIMEOUT_MS from the
 * point it first receives the forwarded signal (not from function entry).
 */
static int collect_child_exit_status(pid_t child_pid,
                                     const struct cpulimit_cfg *cfg) {
    int child_exit_status =
        EXIT_FAILURE;     /* Default if child not properly reaped */
    int child_reaped = 0; /* 1 if successfully reaped child PID */
    /*
     * Three-state forwarding flag:
     *   0  => no quit observed yet in this function
     *  -1  => quit observed once; will forward on the next poll
     *   1  => signal forwarded from this function
     */
    int sig_forwarded = 0;
    struct timespec start_time; /* Timeout anchor; reset when forwarding */

    /* Record time for timeout monitoring during cleanup */
    if (get_current_time(&start_time) != 0) {
        perror("get_current_time");
        exit(EXIT_FAILURE);
    }

    /*
     * Cleanup loop: wait for the command child process to exit.
     * waitpid() can only reap direct children of this process.
     * Any grandchildren (forked by child_pid) are not direct
     * children of this process; they are reparented to init when
     * child_pid exits, matching standard shell semantics.
     * Use a positive PID — the negative-PGID form waitpid(-pgid)
     * would also only return child_pid since it is our only
     * direct child, but using the positive form is clearer and
     * eliminates a dead code branch.
     */
    while (1) {
        int status;
        /*
         * Poll for child state change without blocking (WNOHANG).
         * Returns 0 if no state change has occurred yet, child's
         * PID if it has changed state, or -1 on error.
         */
        pid_t wpid = waitpid(child_pid, &status, WNOHANG);

        if (wpid == child_pid) {
            /* Child process has terminated; record exit status */
            child_reaped = 1;

            if (WIFEXITED(status)) {
                /* Child exited normally via exit() or return from main() */
                child_exit_status = WEXITSTATUS(status);
                if (cfg->verbose) {
                    printf("Process %ld exited with status %d\n",
                           (long)child_pid, child_exit_status);
                }
            } else if (WIFSIGNALED(status)) {
                /* Child was terminated by a signal (SIGTERM, SIGKILL, etc) */
                int signal_number = WTERMSIG(status);
                /*
                 * Shell convention: exit status = 128 + signal number
                 * Example: SIGTERM (15) -> exit status 143
                 */
                child_exit_status = 128 + signal_number;
                if (cfg->verbose) {
                    printf("Process %ld terminated by signal %d\n",
                           (long)child_pid, signal_number);
                }
            } else {
                /* Abnormal termination (neither exit nor signal) */
                if (cfg->verbose) {
                    printf("Process %ld terminated abnormally\n",
                           (long)child_pid);
                }
                child_exit_status = EXIT_FAILURE;
            }
            break;
        }
        if (wpid == 0) {
            /*
             * No state changes yet (WNOHANG returned immediately).
             * Check if we've exceeded timeout for graceful termination.
             */
            const struct timespec poll_sleep = {0, CHILD_POLL_INTERVAL_NS};
            struct timespec current_time;
            if (get_current_time(&current_time) != 0) {
                perror("get_current_time");
                exit(EXIT_FAILURE);
            }

            /*
             * Two-phase quit-signal forwarding.
             *
             * If the quit signal was already forwarded by run_command_mode()
             * before this function was called, is_quit_flag_set() will be
             * true on the very first poll.  Forwarding immediately would
             * duplicate the signal and shorten the grace period.  Instead,
             * the first observation only advances the state (0 -> -1);
             * the actual forward (and start_time reset) happens on the
             * following poll (-1 -> 1), giving the earlier signal one
             * poll-interval (CHILD_POLL_INTERVAL_NS) to be delivered.
             *
             * If the quit signal has not been forwarded yet (the late-arrival
             * race on macOS 10.7 where limit_process() exits before the
             * signal arrives), the same two polls occur; the child receives
             * SIGCONT + the forwarded signal one poll-interval after the
             * quit flag is first observed.
             *
             * sig_forwarded states:
             *   0  => no quit observed yet in this function
             *  -1  => quit observed; forward on next poll if still needed
             *   1  => already forwarded from this function
             */
            if (is_quit_flag_set()) {
                if (sig_forwarded < 0) {
                    int fwd_sig = get_quit_signal();
                    if (fwd_sig == SIGPIPE || fwd_sig == 0) {
                        fwd_sig = SIGTERM;
                    }
                    if (kill(-child_pid, SIGCONT) != 0 && errno != ESRCH) {
                        int err = errno;
                        fprintf(stderr, "kill(-%ld, SIGCONT) failed: %s\n",
                                (long)child_pid, strerror(err));
                    }
                    if (kill(-child_pid, fwd_sig) != 0 && errno != ESRCH) {
                        int err = errno;
                        fprintf(stderr, "kill(-%ld, %d) failed: %s\n",
                                (long)child_pid, fwd_sig, strerror(err));
                    }
                    sig_forwarded = 1;
                    /*
                     * Reset the timeout anchor to the moment the signal is
                     * forwarded so the child gets the full
                     * CHILD_KILL_TIMEOUT_MS grace period from this point,
                     * not from function entry.
                     */
                    if (get_current_time(&start_time) != 0) {
                        perror("get_current_time");
                        exit(EXIT_FAILURE);
                    }
                } else if (sig_forwarded == 0) {
                    /* First observation: note it, forward on the next poll */
                    sig_forwarded = -1;
                }
            }

            /*
             * After CHILD_KILL_TIMEOUT_MS, forcefully kill any remaining
             * processes.  This handles cases where processes ignore
             * SIGTERM.  Send SIGKILL to the entire process group (-pgid)
             * to also terminate any descendants that are still running,
             * even though we cannot wait() on them directly.
             */
            if (timediff_in_ms(&current_time, &start_time) >
                (double)CHILD_KILL_TIMEOUT_MS) {
                if (cfg->verbose) {
                    printf("Process %ld timed out, sending SIGKILL\n",
                           (long)child_pid);
                }
                /* SIGKILL cannot be caught or ignored */
                kill(-child_pid, SIGKILL);
            }
            /* Brief sleep to avoid busy-waiting */
            sleep_timespec(&poll_sleep);

        } else {
            /* wpid < 0: waitpid() encountered an error */
            if (errno == EINTR) {
                /* Interrupted by signal, retry immediately */
                continue;
            }
            if (errno != ECHILD) {
                /* Real error (not just "no children") */
                perror("waitpid");
            }
            /* ECHILD means child already reaped or no children */
            break;
        }
    }

    /*
     * Return child's exit status if we successfully reaped it,
     * otherwise return failure status.
     */
    return child_reaped ? child_exit_status : EXIT_FAILURE;
}

/**
 * @brief Execute and monitor a user-specified command with CPU limiting
 * @param cfg Pointer to configuration structure containing command and options
 *
 * This function implements command execution mode (COMMAND [ARG]...):
 * 1. Forks a child process to execute the specified command
 * 2. Creates a new process group for the child
 * 3. Applies CPU limiting to the command and optionally its descendants
 * 4. Waits for command completion and returns its exit status
 *
 * The parent process monitors the child and handles:
 * - Normal exit (returns child's exit code)
 * - Signal termination (returns 128 + signal number)
 * - Timeout after termination request (sends SIGKILL)
 *
 * @note This function calls exit() and does not return
 */
void run_command_mode(const struct cpulimit_cfg *cfg) {
    pid_t child_pid;  /* PID of forked child that will execute the command */
    int sync_pipe[2]; /* Pipe for parent-child synchronization */
    int fd_flags;     /* Current file descriptor flags for sync_pipe[1] */

    /*
     * Create pipe for synchronization.
     * The write end (sync_pipe[1]) has its close-on-exec flag set (FD_CLOEXEC)
     * so that a successful execvp() in the child closes it automatically,
     * signalling the parent that exec has completed.  On exec failure the
     * child closes it explicitly before _exit().  This lets the parent
     * perform a second read that blocks until exec is done (or the child
     * has exited), which ensures we never send signals to the child while
     * it is in the middle of exec setup (critical for correct behaviour
     * under tools such as valgrind that intercept execve).
     */
    if (pipe(sync_pipe) < 0) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }
    fd_flags = fcntl(sync_pipe[1], F_GETFD);
    if (fd_flags < 0 ||
        fcntl(sync_pipe[1], F_SETFD, fd_flags | FD_CLOEXEC) < 0) {
        perror("fcntl");
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        exit(EXIT_FAILURE);
    }

    /*
     * Flush stdout before forking.
     * This is a defensive measure to avoid duplicated buffered output if
     * future child code paths use stdio and exit()/flush inherited streams.
     */
    fflush(stdout);

    /* Fork to create child process that will execute user command */
    child_pid = fork();
    if (child_pid < 0) {
        perror("fork");
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        exit(EXIT_FAILURE);
    }

    if (child_pid == 0) {
        exec_child_process(cfg, sync_pipe[0], sync_pipe[1]);
        /* NOT REACHED */
    }

    /* Parent: close unused write end before waiting for child */
    close(sync_pipe[1]);
    wait_for_child_exec(child_pid, sync_pipe[0]);

    /*
     * Apply CPU limiting to child process.
     * If include_children is set, limit_process() also tracks and limits
     * all descendant processes. This call blocks until child terminates
     * or a quit signal is received.
     */
    if (cfg->verbose) {
        printf("Limiting process %ld\n", (long)child_pid);
    }
    limit_process(child_pid, cfg->limit, cfg->include_children, cfg->verbose);

    /*
     * Always resume the process group after limit_process() returns.
     * limit_process() sends SIGCONT via its process list before returning,
     * but on some platforms (e.g. macOS 10.7) a stopped process may not be
     * visible to the process iterator, leaving it stopped even though
     * limit_process() has exited.  Sending SIGCONT unconditionally here
     * ensures the child is running and able to receive any subsequent signal.
     * SIGCONT to an already-running process group is harmless.
     * The call may fail with ESRCH if the child has already exited; that
     * case is handled by collect_child_exit_status() below.
     */
    if (kill(-child_pid, SIGCONT) != 0 && errno != ESRCH) {
        int err = errno;
        fprintf(stderr, "kill(-%ld, SIGCONT) failed: %s\n", (long)child_pid,
                strerror(err));
    }

    /*
     * Check if user requested termination via signal (Ctrl+C, SIGTERM,
     * etc). If so, gracefully terminate the entire process group by
     * forwarding the exact received signal. This ensures behavior is
     * consistent with a standard shell: for example, Ctrl+C (SIGINT)
     * is forwarded as SIGINT so the child exits with status 130
     * (128+SIGINT), not 143 (128+SIGTERM).
     * SIGPIPE is an internal pipe-break signal; map it to SIGTERM to
     * avoid unexpected behavior in child processes that do not handle it.
     *
     * Note: if the quit signal arrives after this check (a race on
     * platforms where limit_process() exits early because a stopped
     * process is invisible to the iterator), collect_child_exit_status()
     * will detect and forward it from inside its polling loop.
     */
    if (is_quit_flag_set()) {
        int fwd_sig = get_quit_signal();
        /*
         * Forward the actual received signal. Two special cases:
         * - fwd_sig == 0: theoretically unreachable here because
         *   is_quit_flag_set() is true, meaning a signal was already
         *   delivered and recorded; however, guard defensively.
         * - fwd_sig == SIGPIPE: SIGPIPE is an internal broken-pipe
         *   signal relevant only to the writing process; forwarding it
         *   to the child group could cause unintended termination of
         *   children that write to unrelated pipes. Map it to SIGTERM
         *   so the child group is asked to exit gracefully.
         * Negative PID targets the process group: -PGID.
         */
        if (fwd_sig == SIGPIPE || fwd_sig == 0) {
            fwd_sig = SIGTERM;
        }
        if (kill(-child_pid, fwd_sig) != 0 && errno != ESRCH) {
            int err = errno;
            fprintf(stderr, "kill(-%ld, %d) failed: %s\n", (long)child_pid,
                    fwd_sig, strerror(err));
        }
    }

    exit(collect_child_exit_status(child_pid, cfg));
}

/**
 * @brief Search for and limit an existing process by PID or executable name
 * @param cfg Pointer to configuration structure containing target specification
 *
 * This function implements PID/exe search mode (-p PID or -e EXE):
 * 1. Continuously searches for the target process
 * 2. When found, applies CPU limiting
 * 3. Behavior depends on lazy_mode flag:
 *    - lazy_mode=1: Exit when target terminates or cannot be found
 *    - lazy_mode=0: Keep searching and re-attach if target restarts
 *
 * @note This function calls exit() and does not return
 */
void run_pid_or_exe_mode(const struct cpulimit_cfg *cfg) {
    /* Wait interval between search attempts when target not found */
    const struct timespec wait_time = {2, 0}; /* 2 seconds */
    int pid_mode = cfg->target_pid > 0, exit_status = EXIT_SUCCESS;

    while (!is_quit_flag_set()) {
        pid_t found_pid = pid_mode ? find_process_by_pid(cfg->target_pid)
                                   : find_process_by_name(cfg->exe_name);

        if (found_pid == 0) {
            /* Process does not exist */
            if (pid_mode) {
                fprintf(stderr, "Process with PID %ld cannot be found%s\n",
                        (long)cfg->target_pid,
                        cfg->lazy_mode ? "" : ", retrying...");
            } else {
                fprintf(stderr, "Process '%s' cannot be found%s\n",
                        cfg->exe_name, cfg->lazy_mode ? "" : ", retrying...");
            }
            if (cfg->lazy_mode) {
                /* In lazy mode, missing target is an error condition. */
                exit_status = EXIT_FAILURE;
            }
        } else if (found_pid < 0) {
            /*
             * Process exists but cannot be controlled (permission denied).
             * Negative PID indicates EPERM error. No point retrying.
             */
            fprintf(stderr, "No permission to control process %ld\n",
                    -(long)found_pid);
            exit_status = EXIT_FAILURE;
            break;
        } else {
            /*
             * Sanity check: prevent cpulimit from limiting itself.
             * This could cause system instability or deadlock.
             */
            if (found_pid == getpid()) {
                fprintf(stderr,
                        "Error: target process %ld is cpulimit itself\n",
                        (long)found_pid);
                exit(EXIT_FAILURE);
            }
            if (cfg->verbose) {
                printf("Process %ld found\n", (long)found_pid);
            }
            /*
             * Apply CPU limiting to the target process.
             * This call blocks until the process terminates or quit flag is
             * set.
             */
            limit_process(found_pid, cfg->limit, cfg->include_children,
                          cfg->verbose);
        }

        /*
         * Exit conditions:
         * - lazy_mode: Exit after first attempt (regardless of success)
         * - quit_flag: User requested termination via signal
         */
        if (cfg->lazy_mode || is_quit_flag_set()) {
            break;
        }

        /*
         * In non-lazy mode, wait before retrying.
         * This prevents excessive CPU usage when target is not running.
         */
        sleep_timespec(&wait_time);
    }
    exit(exit_status);
}
