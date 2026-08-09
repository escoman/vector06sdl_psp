#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <pspkernel.h>

#include "debuglog.h"

static int log_fd = -1;
static const char LOG_PATH[] = "ms0:/PSP/GAME/VECTOR06C/debug.log";

void dbglog_open()
{
    if (log_fd >= 0) {
        return;
    }
    /* Open (create/truncate) the log file */
    log_fd = sceIoOpen(LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (log_fd < 0) {
        /* Fallback: leave closed */
        log_fd = -1;
    }
}

void dbglog(const char *fmt, ...)
{
    if (log_fd < 0) {
        return;
    }
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    /* Add timestamp: [seconds.milliseconds] since boot */
    SceUInt64 us = sceKernelGetSystemTimeWide();
    unsigned int ms = (unsigned int)(us / 1000);
    char tsbuf[640];
    int n = snprintf(tsbuf, sizeof(tsbuf), "[%u.%03u] %s",
                     ms / 1000, ms % 1000, buf);
    if (n > 0) {
        sceIoWrite(log_fd, tsbuf, n);
    }
}

void dbglog_close()
{
    if (log_fd >= 0) {
        sceIoClose(log_fd);
        log_fd = -1;
    }
}
