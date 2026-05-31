#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include "logger.h"

void log_event(LogLevel level, const char *module, const char *fmt, ...)
{
    /* timestamp */
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", t);

    /* level string */
    const char *level_str;
    switch (level) {
        case LOG_INFO:  level_str = "INFO "; break;
        case LOG_WARN:  level_str = "WARN "; break;
        case LOG_ERROR: level_str = "ERROR"; break;
        default:        level_str = "?????"; break;
    }

    printf("[%s] [%s] [%-10s] ", timestamp, level_str, module);

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    printf("\n");
    fflush(stdout);
}