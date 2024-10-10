#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/resource.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include "util.h"

#ifdef __IMPL_BASENAME
const char *__basename(const char *path)
{
    const char *p = strrchr(path, '/');
    return p != NULL ? p + 1 : path;
}
#endif

#ifdef __IMPL_GET_TIME
int __get_time(struct timespec *ts)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL))
    {
        return -1;
    }
    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = tv.tv_usec * 1000L;
    return 0;
}
#endif

void increase_priority(void)
{
    static const int MAX_PRIORITY = -20;
    int old_priority, priority;
    old_priority = getpriority(PRIO_PROCESS, 0);
    for (priority = MAX_PRIORITY; priority < old_priority; priority++)
    {
        if (setpriority(PRIO_PROCESS, 0, priority) == 0 &&
            getpriority(PRIO_PROCESS, 0) == priority)
            break;
    }
}

/* Get the number of CPUs */
int get_ncpu(void)
{
    int ncpu;
#if defined(_SC_NPROCESSORS_ONLN)
    ncpu = sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(__APPLE__)
    int mib[2] = {CTL_HW, HW_NCPU};
    size_t len = sizeof(ncpu);
    sysctl(mib, 2, &ncpu, &len, NULL, 0);
#elif defined(_GNU_SOURCE)
    ncpu = get_nprocs();
#else
    ncpu = -1;
#endif
    return ncpu;
}

pid_t get_pid_max(void)
{
#if defined(__linux__)
    /* read /proc/sys/kernel/pid_max */
    long pid_max = -1;
    FILE *fd;
    if ((fd = fopen("/proc/sys/kernel/pid_max", "r")) != NULL)
    {
        if (fscanf(fd, "%ld", &pid_max) != 1)
            pid_max = -1;
        fclose(fd);
    }
    return (pid_t)pid_max;
#elif defined(__FreeBSD__)
    return (pid_t)99999;
#elif defined(__APPLE__)
    return (pid_t)99998;
#else
    return (pid_t)-1;
#endif
}
