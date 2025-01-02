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
    volatile int unused_value = 0;
    while (1)
        (void)unused_value;
    return NULL;
}

int main(int argc, char *argv[])
{

    int i = 0;
    int num_threads = get_ncpu();
    increase_priority();
    if (argc == 2)
        num_threads = atoi(argv[1]);
    for (i = 0; i < num_threads - 1; i++)
    {
        pthread_t thread;
        int ret;
        if ((ret = pthread_create(&thread, NULL, &loop, NULL)) != 0)
        {
            printf("pthread_create() failed. Error code %d\n", ret);
            exit(EXIT_FAILURE);
        }
    }
    loop(NULL);
    return 0;
}
