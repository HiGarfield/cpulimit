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
 *         accessed; 0 otherwise, including when the path cannot be read in
 *         full and no conclusion is possible.
 *
 * Opens with O_NONBLOCK so argv[0] being a FIFO or device (which would block
 * until a peer appears) cannot strand this child before exec and freeze the
 * parent on the sync pipe; regular files are unaffected. Reads until the
 * buffer fills or the file ends: a path that never ended -- a full buffer,
 * or a non-blocking read that stopped on EAGAIN with a writer possibly
 * still appending -- yields no verdict, so a truncated interpreter path is
 * never judged. Done before exec because under tools like valgrind the
 * execve interception is unrecoverable.
 */
int is_script_inaccessible_interpreter(const char *path) {
    int fd, read_failed = 0, at_eof = 0;
    char buf[256];
    ssize_t n = 0;
    char *p;
    char *end;

    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return 0;
    }
    /*
     * Read until the buffer is full or the file has no more data. One read
     * can stop in the middle of a shebang on a non-blocking FIFO whose
     * writer has only sent a prefix; buf[n] would then terminate a
     * truncated interpreter path and access() would judge a path that is
     * not there. EINTR transfers no data and only retries, EOF proves the
     * path is complete, and EAGAIN means a writer may still be appending,
     * so nothing can be concluded about the bytes already read.
     */
    while ((size_t)n < sizeof(buf) - 1) {
        ssize_t got;
        got = read(fd, buf + n, sizeof(buf) - 1 - (size_t)n);
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
    /*
     * Preserve errno across close() only when the read failed: close() may
     * replace it, and on the success path there is nothing to preserve --
     * writing back whatever errno happened to hold hands the caller a stale
     * value under this function's name, and hides a close() that failed.
     * A read that failed says nothing about a shebang.
     */
    if (read_failed) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return 0;
    }
    close(fd);

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
     * No terminator and no end of path: either the buffer filled while the
     * interpreter's path was still going, or a non-blocking read stopped on
     * EAGAIN with a writer possibly still appending. Both leave a prefix
     * rather than a path. access() on that prefix reports something that is
     * not there, and the 126 this function is for would refuse a script the
     * kernel itself can run. Say nothing either way and let the exec carry
     * the real verdict.
     */
    if (*end == '\0' && (n == (ssize_t)(sizeof(buf) - 1) || !at_eof)) {
        return 0;
    }
    *end = '\0';

    if (*p == '\0') {
        return 0;
    }

    /*
     * X_OK, not F_OK: execve() needs the interpreter to be executable, so
     * a path that exists but has no execute bit is as inaccessible as a
     * missing one. Testing existence alone lets that case fall through to
     * execvp(), whose failed execve() is exactly the unrecoverable outcome
     * this pre-check exists to avoid. Either way the script reports 126.
     */
    return access(p, X_OK) != 0;
}
