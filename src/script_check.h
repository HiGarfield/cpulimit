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

#ifndef CPULIMIT_SCRIPT_CHECK_H
#define CPULIMIT_SCRIPT_CHECK_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check whether a script shebang references an inaccessible interpreter
 * @param path Path to the file to inspect
 * @return 1 if the file begins with "#!" and the interpreter path in the
 *         shebang cannot be accessed; 0 otherwise.
 *
 * This pre-exec check avoids calling execvp() on a script whose shebang
 * interpreter path cannot be accessed. Under normal execution execvp() would
 * fail, but under debugging tools such as valgrind the execve() interception
 * is unrecoverable on this path, so the check must be made before exec.
 *
 * The file is opened with O_NONBLOCK. argv[0] is not necessarily a regular
 * file: opening a FIFO that has no writer, or a character device that waits
 * for carrier, blocks until another process opens the other end. Blocking
 * here would strand this child before exec and, through the exec
 * synchronisation pipe, freeze the parent in a read that it cannot leave
 * even when asked to terminate. O_NONBLOCK is defined to have no effect on
 * regular files, so the check itself behaves exactly as before.
 */
int is_script_inaccessible_interpreter(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_SCRIPT_CHECK_H */
