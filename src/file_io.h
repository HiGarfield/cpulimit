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

#ifndef CPULIMIT_FILE_IO_H
#define CPULIMIT_FILE_IO_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__linux__)
/**
 * @brief Read a whole text file into a NUL-terminated heap buffer.
 *
 * @param file_name Path to the file.
 * @return The allocated string, or NULL on error or an empty file; the caller
 *         must free it.
 *
 * Reads past any newline, because /proc/[pid]/stat may embed one in the comm
 * field. A NULL path or an empty file sets errno to EINVAL, and a real
 * syscall failure keeps its own errno.
 */
char *read_file_contents(const char *file_name);
#endif

#ifdef __cplusplus
}
#endif

#endif
