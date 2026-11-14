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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/**
 * @def EXIT_CMD_NOT_EXECUTABLE
 * @brief Shell exit status 126: the command exists but cannot be executed.
 */
#define EXIT_CMD_NOT_EXECUTABLE 126

/**
 * @def EXIT_CMD_NOT_FOUND
 * @brief Shell exit status 127: the command could not be found.
 */
#define EXIT_CMD_NOT_FOUND 127

/**
 * @brief Resolve a bare command name through PATH for the shebang pre-check.
 *
 * @param name Command name without a '/'.
 * @param out Buffer receiving the resolved path.
 * @param out_size Size of out in bytes.
 * @param skipped_bad Set when a candidate with an inaccessible interpreter
 *        was passed over.
 * @return 1 when out holds a usable candidate, or the first passed-over
 *         script when none is usable; 0 when resolution fails.
 */
static int resolve_command_path(const char *name, char *out, size_t out_size,
                                int *skipped_bad) {
    const char *path_env;
    const char *bad_dir = NULL;
    size_t bad_dir_len = 0;
    int found = 0;

    *skipped_bad = 0;
    if (strchr(name, '/') != NULL) {
        return 0;
    }
    path_env = getenv("PATH");
    if (path_env == NULL) {
        path_env = "/usr/bin:/bin";
    }
    /* An empty PATH component denotes the current directory. */
    while (1) {
        const char *sep = strchr(path_env, ':');
        size_t dir_len =
            sep != NULL ? (size_t)(sep - path_env) : strlen(path_env);
        int len;
        struct stat candidate_stat;

        if (dir_len == 0) {
            len = snprintf(out, out_size, "./%s", name);
        } else if (dir_len <= (size_t)INT_MAX && dir_len < out_size) {
            len = snprintf(out, out_size, "%.*s/%s", (int)dir_len, path_env,
                           name);
        } else {
            len = -1;
        }
        /* X_OK also passes for directories and FIFOs, which execvp() skips. */
        if (len > 0 && (size_t)len < out_size && access(out, X_OK) == 0 &&
            stat(out, &candidate_stat) == 0 &&
            S_ISREG(candidate_stat.st_mode)) {
            if (is_script_inaccessible_interpreter(out)) {
                if (bad_dir == NULL) {
                    bad_dir = path_env;
                    bad_dir_len = dir_len;
                }
                *skipped_bad = 1;
            } else {
                found = 1;
                break;
            }
        }
        if (sep == NULL) {
            break;
        }
        path_env = sep + 1;
    }
    if (!found && bad_dir != NULL) {
        int len;
        if (bad_dir_len == 0) {
            len = snprintf(out, out_size, "./%s", name);
        } else if (bad_dir_len <= (size_t)INT_MAX && bad_dir_len < out_size) {
            len = snprintf(out, out_size, "%.*s/%s", (int)bad_dir_len, bad_dir,
                           name);
        } else {
            len = -1;
        }
        /* Only a complete path may be handed to the pre-check. */
        if (len > 0 && (size_t)len < out_size) {
            found = 1;
        }
    }
    return found;
}

/**
 * @brief Ignore SIGPIPE while reporting an exec failure.
 *
 * @note No command is exec'd afterwards, so the disposition cannot leak.
 */
static void ignore_sigpipe_for_diagnostics(void) {
    (void)signal(SIGPIPE, SIG_IGN);
}

/**
 * @brief Set up and execute the command in the forked child.
 *
 * @param cfg Command configuration.
 * @param sync_read_fd Read end of the exec synchronization pipe.
 * @param sync_write_fd Write end of the exec synchronization pipe.
 *
 * @note Never returns: the child either execs or calls _exit().
 */

void exec_child_process(const struct cpulimit_cfg *cfg, int sync_read_fd,
                        int sync_write_fd) {
    int saved_errno;
    int skipped_bad = 0;
    const char *check_path;
    const char *script_path;
    const char *exec_path;
    char *resolved;

    /* Lead a new process group so signals reach the whole command tree. */
    if (setpgid(0, 0) < 0) {
        perror("setpgid");
        close(sync_read_fd);
        close(sync_write_fd);
        goto error_out;
    }

    /* Reset inherited handlers before notifying the parent. */
    /* A signal arriving mid-exec must not hit an inherited handler. */
    if (reset_signal_handlers_to_default() != 0) {
        close(sync_read_fd);
        close(sync_write_fd);
        goto error_out;
    }

    close(sync_read_fd);
    /* Keep sync_write_fd open until exec: FD_CLOEXEC closes it on success. */
    if (write(sync_write_fd, "A", 1) != 1) {
        perror("write sync");
        close(sync_write_fd);
        goto error_out;
    }

    /* Detect an inaccessible shebang interpreter before exec. */
    /* Valgrind cannot recover from a failed intercepted execve(). */
    if (cfg->command_args == NULL || cfg->command_args[0] == NULL) {
        goto error_out;
    }
    check_path = cfg->command_args[0];
    resolved = (char *)malloc(PATH_MAX);
    /* Heap-allocate the PATH buffer: a stack array exceeds 512 bytes. */
    /* Check explicit paths as given, bare names after PATH resolution. */
    if (strchr(check_path, '/') != NULL) {
        script_path = check_path;
    } else if (resolved != NULL &&
               resolve_command_path(check_path, resolved, PATH_MAX,
                                    &skipped_bad)) {
        script_path = resolved;
    } else {
        script_path = NULL;
    }
    if (script_path != NULL &&
        is_script_inaccessible_interpreter(script_path)) {
        ignore_sigpipe_for_diagnostics();
        fprintf(stderr,
                "%s: cannot execute: shebang interpreter is inaccessible\n",
                check_path);
        free(resolved);
        _exit(EXIT_CMD_NOT_EXECUTABLE);
    }
    /* Execute the selected candidate directly: */
    /* execvp() would restart the PATH search under an exec interceptor. */
    exec_path = (skipped_bad && script_path != NULL) ? script_path : check_path;
    /* NOLINTNEXTLINE(clang-analyzer-optin.taint.GenericTaint) */
    execvp(exec_path, cfg->command_args);

    /* Map an exec failure to the shell-compatible 126/127 status. */
    /* Explicit paths use access(F_OK), not errno, for classification. */
    saved_errno = errno;
    ignore_sigpipe_for_diagnostics();
    /* perror() reads errno, which signal() may have changed. */
    errno = saved_errno;
    perror("execvp");
    free(resolved);
    close(sync_write_fd);
    if (strchr(cfg->command_args[0], '/') != NULL) {
        _exit(access(cfg->command_args[0], F_OK) == 0 ? EXIT_CMD_NOT_EXECUTABLE
                                                      : EXIT_CMD_NOT_FOUND);
    }
    _exit(saved_errno == ENOENT ? EXIT_CMD_NOT_FOUND : EXIT_CMD_NOT_EXECUTABLE);

error_out:
    _exit(EXIT_FAILURE);
}
