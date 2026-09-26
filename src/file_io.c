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
 * @brief Read the entire contents of a text file.
 *
 * Opens the file and reads all bytes into a heap-allocated, NUL-terminated
 * buffer, past any newline -- needed for /proc/[pid]/stat whose comm field may
 * embed a newline. The caller must free the result.
 *
 * @param file_name Path to the file.
 * @return Heap-allocated NUL-terminated string, or NULL on error or empty file.
 */
char *read_file_contents(const char *file_name) {
    int fd = -1;
    char *buf = NULL;
    size_t bufsize = 2048;
    size_t buflen = 0;
    ssize_t n_read;

    if (file_name == NULL) {
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
            if (bufsize > (size_t)-1 / 2) {
                goto error; /* Prevent overflow */
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
        goto error;
    }

    close(fd);
    buf[buflen] = '\0';
    return buf;

error:
    if (fd >= 0) {
        close(fd);
    }
    free(buf);
    return NULL;
}
#endif /* __linux__ */
