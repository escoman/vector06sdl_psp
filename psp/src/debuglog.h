#pragma once

#include <string>

/* File-based debug logger for PSP.
 * Writes to ms0:/PSP/GAME/VECTOR06C/debug.log */
void dbglog_open();
void dbglog(const char *fmt, ...);
void dbglog_close();
