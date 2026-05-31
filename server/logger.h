#ifndef LOGGER_H
#define LOGGER_H

typedef enum {
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} LogLevel;

/**
 * @brief Log a timestamped event to stdout
 *
 * @param level  : severity level (LOG_INFO, LOG_WARN, LOG_ERROR)
 * @param module : name of the module logging the event e.g. "ServerMng"
 * @param fmt    : printf-style format string
 */
void log_event(LogLevel level, const char *module, const char *fmt, ...);

#endif /* LOGGER_H */