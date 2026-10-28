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
#include <unistd.h>

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
int is_script_inaccessible_interpreter(const char *path) {
    int fd;
    char buf[256];
    ssize_t n;
    char *p;
    char *end;

    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return 0;
    }
    /*
     * Retry only on EINTR (which transfers no data, so the buffer
     * position need not advance). Partial reads are acceptable: only
     * the shebang prefix and the interpreter path are needed, and both
     * fit comfortably within a single read of a regular file. A
     * non-regular file (a FIFO with no data, for instance) simply
     * reports EAGAIN here and is treated as "not a script".
     */
    do {
        n = read(fd, buf, sizeof(buf) - 1);
    } while (n < 0 && errno == EINTR);
    /*
     * Preserve errno across close() only when the read failed: close() may
     * replace it, and on the success path there is nothing to preserve --
     * writing back whatever errno happened to hold hands the caller a stale
     * value under this function's name, and hides a close() that failed.
     */
    if (n < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
    } else {
        close(fd);
    }

    if (n < 2 || buf[0] != '#' || buf[1] != '!') {
        return 0;
    }
    buf[n] = '\0';

    p = buf + 2;
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    end = p;
    while (*end != '\0' && *end != '\n' && *end != '\r' && *end != ' ' &&
           *end != '\t') {
        end++;
    }
    /*
     * A full buffer holding neither an end of line nor a blank means the
     * single read stopped in the middle of the interpreter's path, so what
     * sits in buf is a prefix of it. access() on that prefix reports a path
     * that is not there, and the 126 this function is for would refuse a
     * script the kernel itself can run. Say nothing either way and let the
     * exec carry the real verdict.
     */
    if (*end == '\0' && n == (ssize_t)(sizeof(buf) - 1)) {
        return 0;
    }
    *end = '\0';

    if (*p == '\0') {
        return 0;
    }

    /* Interpreter path is inaccessible -> report 126 */
    return access(p, F_OK) != 0;
}
