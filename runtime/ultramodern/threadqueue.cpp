#include <cassert>
#include <cstdio>

#include "ultramodern/ultramodern.hpp"

static PTR(OSThread) running_queue_impl = NULLPTR;

static PTR(OSThread)* queue_to_ptr(RDRAM_ARG PTR(PTR(OSThread)) queue) {
    if (queue == ultramodern::running_queue) {
        return &running_queue_impl;
    }
    return TO_PTR(PTR(OSThread), queue);
}

void ultramodern::thread_queue_insert(RDRAM_ARG PTR(PTR(OSThread)) queue_, PTR(OSThread) toadd_) {
    PTR(OSThread)* cur = queue_to_ptr(PASS_RDRAM queue_);
    OSThread* toadd = TO_PTR(OSThread, toadd_); 
    debug_printf("[Thread Queue] Inserting thread %d into queue 0x%08X\n", toadd->id, (uintptr_t)queue_);
    while (*cur && TO_PTR(OSThread, *cur)->priority > toadd->priority) {
        cur = &TO_PTR(OSThread, *cur)->next;
    }
    toadd->next = (*cur);
    toadd->queue = queue_;
    *cur = toadd_;

    debug_printf("  Contains:");
    cur = queue_to_ptr(PASS_RDRAM queue_);
    while (*cur) {
        debug_printf("%d (%d) ", TO_PTR(OSThread, *cur)->id, TO_PTR(OSThread, *cur)->priority);
        cur = &TO_PTR(OSThread, *cur)->next;
    }
    debug_printf("\n");
}

PTR(OSThread) ultramodern::thread_queue_pop(RDRAM_ARG PTR(PTR(OSThread)) queue_) {
    PTR(OSThread)* queue = queue_to_ptr(PASS_RDRAM queue_);
    PTR(OSThread) ret = *queue;
    *queue = TO_PTR(OSThread, ret)->next;
    TO_PTR(OSThread, ret)->queue = NULLPTR;
    debug_printf("[Thread Queue] Popped thread %d from queue 0x%08X\n", TO_PTR(OSThread, ret)->id, (uintptr_t)queue_);
    return ret;
}

bool ultramodern::thread_queue_remove(RDRAM_ARG PTR(PTR(OSThread)) queue_, PTR(OSThread) t_) {
    debug_printf("[Thread Queue] Removing thread %d from queue 0x%08X\n", TO_PTR(OSThread, t_)->id, (uintptr_t)queue_);

    // 3DS port: walk the list. Upstream re-read the head every iteration, so it only ever checked
    // the first entry and could loop forever when the thread wasn't at the head.
    // A thread that isn't in any queue (a running one, or one whose queue field is stale) can come here
    // with a null queue: nothing to remove. Links outside RDRAM end the walk; on the console reading
    // through them was a data abort when Pokemon Snap destroyed its first scene's threads (Azahar read
    // zeros there and stopped).
    if (queue_ == NULLPTR) return false;
    auto valid = [](PTR(OSThread) p) { return (uint32_t)p >= 0x80000000u && (uint32_t)p < 0x80800000u; };
    PTR(OSThread)* link = queue_to_ptr(PASS_RDRAM queue_);
    if (queue_ != ultramodern::running_queue && !valid((PTR(OSThread))queue_)) return false;
    while (*link != NULLPTR) {
        if (!valid(*link)) {
            static bool logged;
            if (!logged) {
                logged = true;
                fprintf(stderr, "[threads] queue 0x%08X: link 0x%08X outside RDRAM, walk stopped\n", (unsigned)queue_, (unsigned)*link);
            }
            return false;
        }
        if (*link == t_) {
            *link = TO_PTR(OSThread, *link)->next;
            return true;
        }
        link = &TO_PTR(OSThread, *link)->next;
    }

    return false;
}

// 3DS port (diagnostics): the scheduler's run queue, highest priority first.
extern "C" void ultramodern_debug_dump_run_queue(uint8_t* rdram) {
    fprintf(stderr, "[runq]");
    PTR(OSThread) cur = running_queue_impl;
    for (int i = 0; cur != NULLPTR && i < 16; i++) {
        OSThread* t = TO_PTR(OSThread, cur);
        fprintf(stderr, " thread%d(pri %d)", (int)t->id, (int)t->priority);
        cur = t->next;
    }
    fprintf(stderr, "\n");
}

bool ultramodern::thread_queue_empty(RDRAM_ARG PTR(PTR(OSThread)) queue_) {
    PTR(OSThread)* queue = queue_to_ptr(PASS_RDRAM queue_);
    return *queue == NULLPTR;
}

PTR(OSThread) ultramodern::thread_queue_peek(RDRAM_ARG PTR(PTR(OSThread)) queue_) {
    PTR(OSThread)* queue = queue_to_ptr(PASS_RDRAM queue_);
    return *queue;
}
