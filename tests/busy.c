/**
 *
 * cpulimit - a CPU usage limiter for Linux, macOS, and FreeBSD
 *
 * Copyright (C) 2005-2012, by: Angelo Marletta <angelo dot marletta at gmail dot com>
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "../src/util.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static void *busy_loop(void *arg)
{
    volatile int keep_loop = 1;
    (void)arg;
    pthread_detach(pthread_self());
    while (keep_loop)
        ;
    return NULL;
}

int main(int argc, char *argv[])
{
    int i, num_threads;
    num_threads = argc == 2 ? atoi(argv[1]) : get_ncpu();
    num_threads = MAX(num_threads, 1);

    for (i = 1; i < num_threads; i++)
    {
        pthread_t tid;
        if (pthread_create(&tid, NULL, busy_loop, NULL) != 0)
        {
            fprintf(stderr, "Failed to create thread %d\n", i);
            exit(EXIT_FAILURE);
        }
    }

    busy_loop(NULL);

    return 0;
}
