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

#include "script_check.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/**
 * @def SHEBANG_BUFFER_SIZE
 * @brief Maximum bytes read from a shebang line.
 */
#define SHEBANG_BUFFER_SIZE 256

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
int is_script_inaccessible_interpreter(const char *path) {
    int fd, read_failed = 0, at_eof = 0, inaccessible;
    char *buf;
    ssize_t n = 0;
    char *p;
    char *end;
    struct stat interpreter_stat;

    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return 0;
    }
    buf = (char *)malloc(SHEBANG_BUFFER_SIZE);
    if (buf == NULL) {
        close(fd);
        return 0;
    }
    /* Read to EOF or a full buffer; never judge a truncated path. */
    while ((size_t)n < SHEBANG_BUFFER_SIZE - 1) {
        ssize_t got;
        got = read(fd, buf + n, SHEBANG_BUFFER_SIZE - 1 - (size_t)n);
        if (got > 0) {
            n += got;
            continue;
        }
        if (got == 0) {
            at_eof = 1;
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN) {
            read_failed = 1;
        }
        break;
    }
    /* Preserve the read error's errno across close() and free(). */
    if (read_failed) {
        int saved_errno = errno;
        close(fd);
        free(buf);
        errno = saved_errno;
        return 0;
    }
    close(fd);

    if (n < 2 || buf[0] != '#' || buf[1] != '!') {
        free(buf);
        return 0;
    }
    buf[n] = '\0';

    p = buf + 2;
    p += strspn(p, " \t");

#if defined(__linux__)
    /* Linux keeps carriage returns in interpreter filenames; other kernels
     * retain their existing delimiter rules. */
    end = p + strcspn(p, "\n \t");
#else
    end = p + strcspn(p, "\n\r \t");
#endif
    if (*end == '\0' && (n == (ssize_t)(SHEBANG_BUFFER_SIZE - 1) || !at_eof)) {
        free(buf);
        return 0;
    }
    *end = '\0';

    if (*p == '\0') {
        free(buf);
        return 0;
    }

    /* X_OK also accepts directories, which execve() cannot interpret. */
    if (access(p, X_OK) != 0) {
        free(buf);
        return 1;
    }
    if (stat(p, &interpreter_stat) != 0) {
        free(buf);
        return 0;
    }
    inaccessible = !S_ISREG(interpreter_stat.st_mode);
    free(buf);
    return inaccessible;
}
