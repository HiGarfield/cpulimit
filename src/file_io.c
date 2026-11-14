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

#include "file_io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

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
char *read_file_contents(const char *file_name) {
    int fd = -1;
    char *buf = NULL;
    size_t bufsize = 2048;
    size_t buflen = 0;
    ssize_t n_read;

    if (file_name == NULL) {
        errno = EINVAL;
        return NULL;
    }

    fd = open(file_name, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }

    buf = (char *)malloc(bufsize);
    if (buf == NULL) {
        goto error;
    }

    while (1) {
        if (buflen >= bufsize - 1) {
            char *temp;
            size_t size_max = 0;
            /* Use unsigned wraparound for SIZE_MAX, not a signed cast. */
            size_max--;
            if (bufsize > size_max / 2) {
                goto error;
            }
            bufsize *= 2;
            temp = (char *)realloc(buf, bufsize);
            if (temp == NULL) {
                goto error;
            }
            buf = temp;
        }

        do {
            n_read = read(fd, buf + buflen, bufsize - buflen - 1);
        } while (n_read < 0 && errno == EINTR);

        if (n_read <= 0) {
            break;
        }

        buflen += (size_t)n_read;
    }

    if (n_read < 0 || (n_read == 0 && buflen == 0)) {
        /* An empty file has no failing syscall, so set EINVAL explicitly. */
        if (n_read == 0) {
            errno = EINVAL;
        }
        goto error;
    }

    close(fd);
    buf[buflen] = '\0';
    return buf;

error:
    if (fd >= 0) {
        /* Preserve errno across close() on the failure path. */
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
    }
    free(buf);
    return NULL;
}
#endif
