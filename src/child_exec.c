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

#include "child_exec.h"

#include "script_check.h"
#include "signal_handler.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

void exec_child_process(const struct cpulimit_cfg *cfg, int sync_read_fd,
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
     * whose shebang interpreter is inaccessible before calling execvp().
     * Under normal execution execvp() would fail in this case,
     * but under valgrind the exec interception is unrecoverable, so the
     * check must happen before exec.  _exit() closes all fds (including
     * sync_write_fd), which also signals exec completion to the parent.
     */
    if (strchr(cfg->command_args[0], '/') != NULL &&
        is_script_inaccessible_interpreter(cfg->command_args[0])) {
        fprintf(stderr,
                "%s: cannot execute: shebang interpreter is inaccessible\n",
                cfg->command_args[0]);
        _exit(EXIT_CMD_NOT_EXECUTABLE);
    }
    execvp(cfg->command_args[0], cfg->command_args);

    /*
     * Execution reaches here only if execvp() failed.
     * Use shell-compatible exit codes:
     * - EXIT_CMD_NOT_FOUND (127): command not found
     * - EXIT_CMD_NOT_EXECUTABLE (126): found but not executable
     *
     * For explicit paths (containing '/') the correct classification is
     * determined by access(F_OK): if the file does not exist, the
     * command was not found (127); if it does exist, it was found but
     * could not be executed (126, e.g. missing dynamic linker, bad
     * permissions, or a shebang interpreter that exec cannot locate).
     * Using access(F_OK) rather than relying on errno is necessary
     * because some kernel/libc combinations (e.g. 32-bit musl on Linux
     * with seccomp-restricted execve) return EACCES instead of ENOENT
     * for non-existent paths, which would otherwise be misclassified as
     * "not executable" (126) rather than "not found" (127).
     *
     * For PATH-resolved names (no '/' in argument) the errno from
     * execvp is the only signal available.
     *
     * Close sync_write_fd explicitly to signal exec failure to the
     * parent (the FD_CLOEXEC path only applies on success).
     */
    saved_errno = errno;
    perror("execvp");
    close(sync_write_fd);
    if (strchr(cfg->command_args[0], '/') != NULL) {
        _exit(access(cfg->command_args[0], F_OK) == 0 ? EXIT_CMD_NOT_EXECUTABLE
                                                      : EXIT_CMD_NOT_FOUND);
    }
    _exit(saved_errno == ENOENT ? EXIT_CMD_NOT_FOUND : EXIT_CMD_NOT_EXECUTABLE);

error_out:
    _exit(EXIT_FAILURE);
}
