#ifndef LOG_RING_H
#define LOG_RING_H

// In-memory stderr (see log_ring.c): logging never waits for the SD card.

#ifdef __cplusplus
extern "C" {
#endif

// Reopens stderr onto the in-memory log; sd_path is the log file the drain appends to.
void log_ring_start(const char* sd_path);

// Copies what was logged since the last call to the SD card file (watchdog thread).
void log_ring_drain(void);

#ifdef __cplusplus
}
#endif

#endif
