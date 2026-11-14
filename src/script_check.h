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
 * @brief Check whether a script shebang names an inaccessible interpreter.
 *
 * @param path Path to the file to inspect.
 * @return 1 when the file starts with "#!" and its interpreter is not
 *         executable; 0 otherwise, including when no conclusion is possible.
 *
 * Opens with O_NONBLOCK so a FIFO or device cannot block the child before
 * exec, and judges the interpreter only after a complete path has been read.
 * The check runs before exec because exec interception under valgrind is
 * unrecoverable.
 */
int is_script_inaccessible_interpreter(const char *path);

#ifdef __cplusplus
}
#endif

#endif
