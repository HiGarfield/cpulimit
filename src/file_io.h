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

#ifndef CPULIMIT_FILE_IO_H
#define CPULIMIT_FILE_IO_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__linux__)
/**
 * @brief Read the entire contents of a text file.
 *
 * Opens the specified text file and reads all of its bytes into a
 * heap-allocated, NUL-terminated buffer. Unlike a line reader, this reads
 * past any newline, which is required for files such as /proc/[pid]/stat
 * whose only string field (comm) may legitimately embed a newline.
 *
 * The returned buffer is heap-allocated and must be freed by the caller.
 *
 * @param file_name Path to the file.
 * @return Heap-allocated NUL-terminated string, or NULL on error or empty
 *         file.
 */
char *read_file_contents(const char *file_name);
#endif

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_FILE_IO_H */
