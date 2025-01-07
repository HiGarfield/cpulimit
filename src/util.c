#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/resource.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif
#ifdef HAVE_SYS_SYSINFO_H
#include <sys/sysinfo.h>
#endif
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
#if defined(_SC_NPROCESSORS_ONLN)
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    return ncpu > 0 ? (int)ncpu : 1;
#elif defined(__APPLE__) || defined(__FreeBSD__)
    int ncpu = 0;
    int mib[2] = {CTL_HW, HW_AVAILCPU};
    size_t len = sizeof(ncpu);
    if (sysctl(mib, 2, &ncpu, &len, NULL, 0) != 0 || ncpu < 1)
    {
        mib[1] = HW_NCPU;
        if (sysctl(mib, 2, &ncpu, &len, NULL, 0) != 0 || ncpu < 1)
        {
            return 1;
        }
    }
    return ncpu;
#elif defined(__linux__)
    int ncpu = get_nprocs();
    return ncpu > 0 ? ncpu : 1;
#else
#error "Unsupported platform"
#endif
}

pid_t get_pid_max(void)
{
#if defined(__linux__)
    long pid_max = -1;
    FILE *fd;
    if ((fd = fopen("/proc/sys/kernel/pid_max", "r")) != NULL)
    {
        if (fscanf(fd, "%ld", &pid_max) != 1)
        {
            perror("fscanf");
            pid_max = -1;
        }
        fclose(fd);
    }
    return (pid_t)pid_max;
#elif defined(__APPLE__) || defined(__FreeBSD__)
    int max_proc;
    size_t size = sizeof(max_proc);
    if (sysctlbyname("kern.maxproc", &max_proc, &size, NULL, 0) == -1)
    {
        perror("sysctl");
        return (pid_t)-1;
    }
    return (pid_t)max_proc;
#else
#error "Platform not supported"
#endif
}

#if defined(__linux__) && defined(__UCLIBC__)
int getloadavg(double *loadavg, int nelem)
{
    FILE *fp;
    char buffer[65], *ptr;
    int i;

    if (nelem < 0)
    {
        return -1;
    }
    else if (nelem == 0)
    {
        return 0;
    }
    else if (nelem > 3)
    {
        nelem = 3;
    }

    if ((fp = fopen("/proc/loadavg", "r")) == NULL)
    {
        return -1;
    }

    if (fgets(buffer, sizeof(buffer), fp) == NULL)
    {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    ptr = buffer;

    for (i = 0; i < nelem; i++)
    {
        char *endptr;
        errno = 0;
        loadavg[i] = strtod(ptr, &endptr);
        if (errno != 0 || ptr == endptr)
        {
            return -1;
        }
        ptr = endptr;
    }

    return nelem;
}
#endif
