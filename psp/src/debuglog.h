#pragma once

#ifndef DEBUG_ENABLED
#define DEBUG_ENABLED 0
#endif

#if DEBUG_ENABLED

void dbglog_open();
void dbglog(const char *fmt, ...);
void dbglog_close();

#else

#define dbglog_open()  ((void)0)
#define dbglog(...)    ((void)0)
#define dbglog_close() ((void)0)

#endif