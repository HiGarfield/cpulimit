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

#include <unistd.h>

/**
 * @brief CPU load generator using fork
 * @return Always returns 0
 * @note This program creates 4 processes (original + 3 children) that
 *       each run an infinite busy loop, consuming CPU cycles for
 *       testing purposes.
 */
int main(void)
{
    /* Create two child processes, resulting in 4 total processes */
    fork();
    fork();
    while (1)
    {
        /* Infinite busy loop to consume CPU cycles */
        ;
    }
    return 0;
}
