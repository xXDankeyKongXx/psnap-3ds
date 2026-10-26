#ifndef CRASH_LOG_H
#define CRASH_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

// Crash log (crash_log.c): init once at start, then attach in every thread that should report.
void crash_log_init(void);
void crash_log_attach(void);

#ifdef __cplusplus
}
#endif

#endif
