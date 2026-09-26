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

#ifndef CPULIMIT_SCRIPT_CHECK_H
#define CPULIMIT_SCRIPT_CHECK_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check whether a script shebang references an inaccessible interpreter
 *
 * @param path Path to the file to inspect
 * @return 1 if the file begins with "#!" and the interpreter path cannot be
 *         accessed; 0 otherwise.
 *
 * Opens with O_NONBLOCK so argv[0] being a FIFO or device (which would block
 * until a peer appears) cannot strand this child before exec and freeze the
 * parent on the sync pipe; regular files are unaffected. Done before exec
 * because under tools like valgrind the execve interception is unrecoverable.
 */
int is_script_inaccessible_interpreter(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_SCRIPT_CHECK_H */
