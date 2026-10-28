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

#include "cli.h"
#include "script_check.h"
#include "signal_handler.h"

#include <errno.h>
#include <limits.h>
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
 * interpreter). Mirrors the POSIX shell convention for exit status 126.
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
 * @brief Resolve a bare command name to the path execvp() would execute
 *
 * @param name Command name without a '/'
 * @param out  Buffer receiving the resolved path (must hold PATH_MAX bytes)
 * @param out_size Size of out in bytes
 * @return 1 if a component of PATH yields a file that can be executed, 0
 *         otherwise
 *
 * Mirrors execvp()'s PATH search so the shebang pre-check below can inspect a
 * PATH-resolved script the same way it inspects an explicit path. Returns 0
 * (no resolution) for names that already contain a '/', leaving the existing
 * explicit-path path untouched.
 *
 * The question asked of each component is the one execvp() asks: can this
 * candidate be executed? A file that merely exists is not the one the exec
 * will use, because execvp() skips a component it cannot execute and keeps
 * searching, so a probe by existence would judge the command from a file the
 * exec never reaches -- and the pre-check refuses on that judgement. An empty
 * component denotes the current directory (POSIX), which is what "." spells
 * here; strtok() dropped those along with the file execvp() would have run.
 */
static int resolve_command_path(const char *name, char *out, size_t out_size) {
    const char *path_env;
    int found = 0;

    if (strchr(name, '/') != NULL) {
        return 0;
    }
    path_env = getenv("PATH");
    if (path_env == NULL) {
        path_env = "/usr/bin:/bin";
    }
    /*
     * Walk PATH in place, never copying it: the components are only read, and
     * a component that does not fit in out is skipped like any other
     * candidate that cannot be used. An empty component means the current
     * directory (POSIX), which is what "." spells here.
     */
    while (1) {
        const char *sep = strchr(path_env, ':');
        size_t dir_len =
            sep != NULL ? (size_t)(sep - path_env) : strlen(path_env);
        int len;

        if (dir_len == 0) {
            len = snprintf(out, out_size, "./%s", name);
        } else if (dir_len <= (size_t)INT_MAX) {
            len = snprintf(out, out_size, "%.*s/%s", (int)dir_len, path_env,
                           name);
        } else {
            len = -1;
        }
        if (len > 0 && (size_t)len < out_size && access(out, X_OK) == 0) {
            found = 1;
            break;
        }
        if (sep == NULL) {
            break;
        }
        path_env = sep + 1;
    }
    return found;
}

/**
 * @brief Execute a child process for command mode
 *
 * @param cfg Pointer to configuration structure containing command and options
 * @param sync_read_fd Read end of the synchronization pipe
 * @param sync_write_fd Write end of the synchronization pipe
 *
 * @note This function never returns; it calls _exit() on any failure
 */

void exec_child_process(const struct cpulimit_cfg *cfg, int sync_read_fd,
                        int sync_write_fd) {
    int saved_errno;
    const char *check_path;
    const char *script_path;
    char *resolved;

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
     * the parent. After fork(), this child inherits the parent's
     * configured handlers. execvp() resets them, but on systems
     * where exec takes measurable time (e.g., macOS 10.15+ with
     * library validation), a signal forwarded by the parent between
     * reading the sync byte and exec completing would be silently
     * caught by the inherited handler instead of terminating this
     * process. Resetting here closes that race window.
     */
    if (reset_signal_handlers_to_default() != 0) {
        close(sync_read_fd);
        close(sync_write_fd);
        goto error_out;
    }

    close(sync_read_fd);
    /*
     * Signal parent that child initialization is complete.
     * Parent blocks until receiving this byte.
     * Do NOT close sync_write_fd here: it must remain open until exec
     * so that the close-on-exec flag (FD_CLOEXEC) causes it to be
     * closed automatically on a successful execvp(), signalling exec
     * completion to the parent. On exec failure the code below closes
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
     * Pre-check: detect a script whose shebang interpreter is inaccessible
     * before calling execvp(). Under normal execution execvp() would fail in
     * this case, but under valgrind the exec interception is unrecoverable, so
     * the check must happen before exec. _exit() closes all fds (including
     * sync_write_fd), which also signals exec completion to the parent.
     *
     * The check runs for an explicit path AND for a bare name resolved through
     * PATH, so both spellings of the same script report the same code: an
     * inaccessible interpreter always yields 126, never a misleading 127.
     */
    /*
     * cli.c assigns command_args only when a command was given, and rejects
     * an empty one, so command_args[0] is never NULL here. State that
     * explicitly: the analyser cannot carry the guarantee across translation
     * units and would otherwise read execvp() as being handed a NULL file
     * name.
     */
    if (cfg->command_args == NULL || cfg->command_args[0] == NULL) {
        goto error_out;
    }
    check_path = cfg->command_args[0];
    resolved = (char *)malloc(PATH_MAX);
    /*
     * Heap-allocate the PATH resolution buffer instead of putting a
     * PATH_MAX-sized array on the stack, which would blow the project's
     * -Wstack-usage=512 limit. Only the PATH-resolved branch needs it:
     * the explicit-path branch works on check_path itself and therefore
     * runs whether or not the allocation succeeded. A failed
     * allocation costs just the PATH pre-check -- a bare name then falls
     * through to execvp() and reports the usual code -- never the
     * explicit-path check whose 126 classification must not depend on a
     * 4 KiB allocation succeeding.
     */
    /*
     * Decide which path to pre-check, then check it once. An explicit path
     * is checked as given so that its 126 classification never depends on
     * whether the allocation above succeeded; a bare name has to be resolved
     * through PATH first, and one that resolves nowhere has nothing to
     * check and falls through to execvp(). The message names what the user
     * typed either way.
     */
    if (strchr(check_path, '/') != NULL) {
        script_path = check_path;
    } else if (resolved != NULL &&
               resolve_command_path(check_path, resolved, PATH_MAX)) {
        script_path = resolved;
    } else {
        script_path = NULL;
    }
    if (script_path != NULL &&
        is_script_inaccessible_interpreter(script_path)) {
        fprintf(stderr,
                "%s: cannot execute: shebang interpreter is inaccessible\n",
                check_path);
        free(resolved);
        _exit(EXIT_CMD_NOT_EXECUTABLE);
    }
    free(resolved);
    execvp(check_path, cfg->command_args);

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
