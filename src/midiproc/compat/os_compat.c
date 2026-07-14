#if !defined(_WIN32) && !defined(WIN32) && !defined(__WIN32__)

#include "os_compat.h"

#ifdef __APPLE__
#include <assert.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/sysctl.h>
bool IsDebuggerPresent() {
    int mib[4];
    struct kinfo_proc info;
    size_t size;

    info.kp_proc.p_flag = 0;
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PID;
    mib[3] = getpid();

    size = sizeof(info);
    sysctl(mib, sizeof(mib) / sizeof(*mib), &info, &size, NULL, 0);

    return ((info.kp_proc.p_flag & P_TRACED) != 0);
}
#else
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>

#ifdef __GNUC__
#include <unistd.h>
#endif

#ifndef constexpr
#define constexpr const
#endif

// linux and other unix-like systems
bool IsDebuggerPresent() {
    char buf[4096];

    const int status_fd = open("/proc/self/status", O_RDONLY);
    if (status_fd == -1)
        return false;

    const ssize_t num_read = read(status_fd, buf, sizeof(buf) - 1);
    close(status_fd);

    if (num_read <= 0)
        return false;

    buf[num_read] = '\0';
    constexpr char tracerPidString[] = "TracerPid:";
    char *tracer_pid_ptr = strstr((const char *)buf, tracerPidString);
    if (tracer_pid_ptr == NULL)
        return false;

    for (const char* characterPtr = tracer_pid_ptr + sizeof(tracerPidString) - 1; characterPtr <= buf + num_read; ++characterPtr)
    {
        if (isspace(*characterPtr))
            continue;
        else
            return isdigit(*characterPtr) != 0 && *characterPtr != '0';
    }

    return false;
}
#endif


void OutputDebugStringW(const WCHAR * fmt, ...) {
#ifdef NDEBUG
    (void)fmt;

    return;
#else
    if( !IsDebuggerPresent() )
        return;
    va_list args;
    va_start(args, fmt);
    vwprintf(fmt, args);
    va_end(args);
#endif
}

#endif
