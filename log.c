#include "pm.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

pm_identity process_identity;

char *get_identity_name (pm_identity id)
{
        switch (id) {
        case MAIN: return "main";
        case DAEMON: return "daemon";
        case MONITOR: return "monitor";
        default: return "unknown";
        }
}

void log_info (char *message, ...)
{
        va_list args;
        va_start (args, message);

        fprintf (stderr, "[INFO: %s] ", get_identity_name (process_identity));
        vfprintf (stderr, message, args);
        putchar ('\n');
        va_end (args);
}

void log_warn (char *message, ...)
{
        va_list args;
        va_start (args, message);

        fprintf (stderr, "[WARN: %s] ", get_identity_name (process_identity));
        vfprintf (stderr, message, args);
        putchar ('\n');

        va_end (args);
}

void log_error (char *message, ...)
{
        va_list args;
        va_start (args, message);

        fprintf (stderr, "[ERR: %s] ", get_identity_name (process_identity));
        vfprintf (stderr, message, args);
        putchar ('\n');

        va_end (args);
}
