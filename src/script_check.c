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

int is_script_inaccessible_interpreter(const char *path) {
    int fd;
    int saved_errno;
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
    saved_errno = errno;
    close(fd);
    errno = saved_errno;

    /* Not a script if too short or no shebang prefix */
    if (n < 2 || buf[0] != '#' || buf[1] != '!') {
        return 0;
    }
    buf[n] = '\0';

    /* Skip optional whitespace after "#!" */
    p = buf + 2;
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    /* Find end of interpreter path (terminated by whitespace, newline, NUL) */
    end = p;
    while (*end != '\0' && *end != '\n' && *end != '\r' && *end != ' ' &&
           *end != '\t') {
        end++;
    }
    *end = '\0';

    if (*p == '\0') {
        return 0; /* Empty shebang line */
    }

    /* Interpreter path is inaccessible -> report 126 */
    return access(p, F_OK) != 0;
}
