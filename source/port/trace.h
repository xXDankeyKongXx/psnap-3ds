#ifndef TRACE_H
#define TRACE_H

// Hooks for N64Recomp's trace_mode (recomp/us.rev1.3ds.toml): every recompiled function records its
// name on entry in a small ring buffer, so a hang can be located from the watchdog's log
// (snap_trace_dump). Only one game thread runs at a time, so the newest entries show where the
// running thread is.

#ifdef __cplusplus
extern "C" {
#endif

#define RECOMP_TRACE_RING 64
extern const char* volatile recomp_trace_ring[RECOMP_TRACE_RING];
extern volatile unsigned recomp_trace_pos;

// Defined in runtime/ultramodern/mesgqueue.cpp: function entry doubles as the interrupt point at
// which a pending VI/SP/DP message can preempt the running game thread.
extern volatile int ultramodern_external_pending;
void ultramodern_preempt_point(unsigned char* rdram);

#define TRACE_ENTRY() \
    recomp_trace_ring[recomp_trace_pos++ & (RECOMP_TRACE_RING - 1)] = __func__; \
    if (__builtin_expect(ultramodern_external_pending, 0)) ultramodern_preempt_point(rdram);
#define TRACE_RETURN()

#ifdef __cplusplus
}
#endif

#endif
