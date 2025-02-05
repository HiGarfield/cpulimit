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

#ifndef __GNUC__
#define __attribute__(attr)
#endif

static void *loop(void *param __attribute__((unused)))
{
    while (1)
        ;
    return NULL;
}

int main(int argc, char *argv[])
{
    int i, num_threads;
    pthread_t *threads;
    num_threads = argc == 2 ? atoi(argv[1]) : get_ncpu();
    num_threads = MAX(num_threads, 1);

    threads = (pthread_t *)malloc((size_t)num_threads * sizeof(pthread_t));
    if (threads == NULL)
    {
        fprintf(stderr, "malloc() failed.\n");
        exit(EXIT_FAILURE);
    }

    increase_priority();

    for (i = 0; i < num_threads; i++)
    {
        int ret;
        if ((ret = pthread_create(&threads[i], NULL, loop, NULL)) != 0)
        {
            fprintf(stderr, "pthread_create() failed. Error code %d\n", ret);
            free(threads);
            exit(EXIT_FAILURE);
        }
    }

    for (i = 0; i < num_threads; i++)
    {
        pthread_join(threads[i], NULL);
    }

    free(threads);
    return 0;
}
