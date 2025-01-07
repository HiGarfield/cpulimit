#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "../src/util.h"

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
