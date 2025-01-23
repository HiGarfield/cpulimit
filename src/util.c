#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#if defined(__linux__) && defined(__UCLIBC__)
#include <stdlib.h>
#include <errno.h>
#endif
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif
#ifdef HAVE_SYS_SYSINFO_H
#include <sys/sysinfo.h>
#endif
#ifdef __IMPL_GET_TIME
#include <sys/time.h>
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
    static int cached_ncpu = -1;

    if (cached_ncpu > 0)
    {
        return cached_ncpu;
    }

#if defined(_SC_NPROCESSORS_ONLN)
    {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        cached_ncpu = ncpu > 0 ? (int)ncpu : 1;
    }
#elif defined(__APPLE__) || defined(__FreeBSD__)
    {
        int ncpu = 0, mib[2];
        mib[0] = CTL_HW;
#if defined(HW_AVAILCPU)
        mib[1] = HW_AVAILCPU;
#elif defined(HW_NCPU)
        mib[1] = HW_NCPU;
#else
#error "Unsupported platform"
#endif
        size_t len = sizeof(ncpu);
        if (sysctl(mib, 2, &ncpu, &len, NULL, 0) != 0 || ncpu < 1)
        {
            mib[1] = HW_NCPU;
            if (sysctl(mib, 2, &ncpu, &len, NULL, 0) != 0 || ncpu < 1)
            {
                cached_ncpu = 1;
                return 1;
            }
        }
        cached_ncpu = ncpu;
    }
#elif defined(__linux__)
    {
        int ncpu = get_nprocs();
        cached_ncpu = ncpu > 0 ? ncpu : 1;
    }
#else
#error "Unsupported platform"
#endif

    return cached_ncpu;
}

pid_t get_pid_max(void)
{
#if defined(__linux__)

#ifndef PID_T_MAX
#define MAX_SIGNED_INT(type) \
    ((type)((((type)1 << (sizeof(type) * 8 - 2)) - 1) * 2 + 1))
#define PID_T_MAX (MAX_SIGNED_INT(pid_t))
#endif /* #ifndef PID_T_MAX */

    long pid_max;
    FILE *fp;
    if ((fp = fopen("/proc/sys/kernel/pid_max", "r")) == NULL)
    {
        fprintf(stderr, "Fail to open /proc/sys/kernel/pid_max\n");
        return PID_T_MAX;
    }
    if (fscanf(fp, "%ld", &pid_max) != 1)
    {
        fprintf(stderr, "Fail to read /proc/sys/kernel/pid_max\n");
        fclose(fp);
        return PID_T_MAX;
    }
    fclose(fp);
    return (pid_t)pid_max;
#elif defined(__FreeBSD__)
    return (pid_t)99998;
#elif defined(__APPLE__)
    return (pid_t)99998;
#else
#error "Unsupported platform"
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
