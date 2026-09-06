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

#ifndef CPULIMIT_PATH_UTIL_H
#define CPULIMIT_PATH_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Extract filename from a full path
 * @param path Full file path (may contain directory separators), or NULL
 * @return Pointer to the filename portion within the path string
 *
 * Returns a pointer to the substring after the last '/' character, or the
 * original string if no '/' is found. Does not allocate memory; the returned
 * pointer references part of the input string.
 *
 * If path is NULL, returns an empty string.
 */
const char *get_file_basename(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* CPULIMIT_PATH_UTIL_H */
