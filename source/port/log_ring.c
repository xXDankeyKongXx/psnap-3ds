// In-memory log device. After startup, stderr is reopened onto "log:" (a devoptab device whose
// writes append to a RAM buffer), and the watchdog thread copies the buffer to the SD card file with
// log_ring_drain(). Before this, a flush to the SD card held stderr's lock for milliseconds, and any
// thread that logged meanwhile (the UI thread, the renderer, audio) waited: the top screen hitched
// every 2 s when the watchdog wrote its report (Oak's text box showed it as uneven text).

#include "log_ring.h"

#include <3ds.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/iosupport.h>

#define RING_SIZE (64 * 1024)

static char ring[RING_SIZE];
static size_t ring_len;
static size_t dropped;
static LightLock ring_lock;
static FILE* sd_file;

static int log_open(struct _reent* r, void* fileStruct, const char* path, int flags, int mode) {
    (void)r; (void)fileStruct; (void)path; (void)flags; (void)mode;
    return 0;
}

static int log_close(struct _reent* r, void* fd) {
    (void)r; (void)fd;
    return 0;
}

static ssize_t log_write(struct _reent* r, void* fd, const char* ptr, size_t len) {
    (void)r; (void)fd;
    LightLock_Lock(&ring_lock);
    size_t room = RING_SIZE - ring_len;
    size_t n = len < room ? len : room;
    memcpy(ring + ring_len, ptr, n);
    ring_len += n;
    dropped += len - n;
    LightLock_Unlock(&ring_lock);
    return (ssize_t)len; // never block or fail: what doesn't fit is counted as dropped
}

static const devoptab_t log_device = {
    .name = "log",
    .structSize = 0,
    .open_r = log_open,
    .close_r = log_close,
    .write_r = log_write,
};

void log_ring_start(const char* sd_path) {
    LightLock_Init(&ring_lock);
    if (AddDevice(&log_device) < 0) return;
    fflush(stderr);
    sd_file = fopen(sd_path, "a"); // the startup log so far stays; this continues it
    if (!sd_file) return;
    // Line buffered: each line reaches the ring at once (a memcpy), so nothing waits on the SD card.
    if (freopen("log:/", "w", stderr)) setvbuf(stderr, NULL, _IOLBF, 1024);
}

void log_ring_drain(void) {
    static char out[RING_SIZE];
    if (!sd_file) return;
    LightLock_Lock(&ring_lock);
    size_t n = ring_len, lost = dropped;
    memcpy(out, ring, n);
    ring_len = 0;
    dropped = 0;
    LightLock_Unlock(&ring_lock);
    if (n) fwrite(out, 1, n, sd_file);
    if (lost) fprintf(sd_file, "[log] %u bytes dropped (log buffer full)\n", (unsigned)lost);
    fflush(sd_file);
}
