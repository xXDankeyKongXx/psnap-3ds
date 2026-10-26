// Crash log: when a thread of the app faults (data abort, prefetch abort, undefined instruction),
// write what's needed to find the cause to sdmc:/3ds/psnap/crash.txt - the registers, the core, the
// game's last 64 recompiled functions entered (trace.h) and where the game was - then stop with a
// panic (Luma shows its usual screen). The file is opened at start and written through the FS
// service directly, so the handler doesn't need the C library's locks or the heap, which the
// crashed thread may hold.

#include "crash_log.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trace.h"


static Handle crash_file;
static volatile int crashing;
static u64 start_tick;

#define HANDLER_STACK (16 * 1024)

static void handler(ERRF_ExceptionInfo* e, CpuRegisters* r) {
    if (__atomic_exchange_n(&crashing, 1, __ATOMIC_SEQ_CST)) {
        for (;;) svcSleepThread(1000000000LL); // another thread is already reporting
    }
    static char buf[6144];
    int n = 0;
#define OUT(...) n += snprintf(buf + n, n < (int)sizeof(buf) ? sizeof(buf) - n : 0, __VA_ARGS__)
    static const char* const kinds[] = { "prefetch abort", "data abort", "undefined instruction", "VFP", "?" };
    OUT("Pokemon Snap 3DS v" SNAP_VERSION " crashed after %.1f s\n", (double)(svcGetSystemTick() - start_tick) / SYSCLOCK_ARM11);
    OUT("%s on core %d, fsr %08lX, far %08lX\n", kinds[e->type < 4 ? e->type : 4], svcGetProcessorID(),
        (unsigned long)e->fsr, (unsigned long)e->far);
    for (int i = 0; i < 13; i++) OUT("r%-2d %08lX%s", i, (unsigned long)r->r[i], i % 4 == 3 ? "\n" : "  ");
    OUT("\nsp  %08lX  lr  %08lX  pc  %08lX  cpsr %08lX\n", (unsigned long)r->sp, (unsigned long)r->lr,
        (unsigned long)r->pc, (unsigned long)r->cpsr);
    unsigned pos = recomp_trace_pos;
    OUT("last game functions entered (%u calls, newest last):\n", pos);
    for (unsigned i = 0; i < RECOMP_TRACE_RING; i++) {
        const char* name = recomp_trace_ring[(pos + i) & (RECOMP_TRACE_RING - 1)];
        if (name) OUT(" %s", name);
    }
    OUT("\n");
#undef OUT
    if (n > (int)sizeof(buf)) n = sizeof(buf);
    if (crash_file) {
        u32 written = 0;
        FSFILE_Write(crash_file, &written, 0, buf, (u32)n, FS_WRITE_FLUSH);
        FSFILE_SetSize(crash_file, (u64)n);
        FSFILE_Close(crash_file);
    }
    svcBreak(USERBREAK_PANIC);
    for (;;) svcSleepThread(1000000000LL);
}

void crash_log_init(void) {
    start_tick = svcGetSystemTick();
    if (R_SUCCEEDED(FSUSER_OpenFileDirectly(&crash_file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
            fsMakePath(PATH_ASCII, "/3ds/psnap/crash.txt"), FS_OPEN_CREATE | FS_OPEN_WRITE, 0))) {
        FSFILE_SetSize(crash_file, 0); // nothing from an earlier run
    } else {
        crash_file = 0;
    }
}

void crash_log_attach(void) {
    void* stack = aligned_alloc(8, HANDLER_STACK);
    if (stack) threadOnException(handler, (u8*)stack + HANDLER_STACK, WRITE_DATA_TO_HANDLER_STACK);
}
