#include <atomic>
#include <algorithm>
#include <bitset>
#include <deque>
#include <mutex>
#include <cstdio>
#include <thread>

#include "blockingconcurrentqueue.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

struct QueuedMessage {
    PTR(OSMesgQueue) mq;
    OSMesg mesg;
    bool jam;
    bool requeue_if_blocked;
};

static moodycamel::BlockingConcurrentQueue<QueuedMessage> external_messages {};
std::bitset<32> requeue_enabled;

void ultramodern::set_message_queue_control(const ultramodern::MessageQueueControl& mqc) {
    requeue_enabled.reset();
    requeue_enabled.set(static_cast<int>(EventMessageSource::Timer), mqc.requeue_timer);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Sp), mqc.requeue_sp);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Si), mqc.requeue_si);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Ai), mqc.requeue_ai);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Vi), mqc.requeue_vi);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Pi), mqc.requeue_pi);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Dp), mqc.requeue_dp);
}

// 3DS port: set whenever an external message is queued; recompiled functions check it on entry
// (see source/port/trace.h) so a game thread busy in a long loop is preempted like an interrupt would.
extern "C" { volatile int ultramodern_external_pending = 0; }

void ultramodern::enqueue_external_message_src(PTR(OSMesgQueue) mq, OSMesg msg, bool jam, EventMessageSource src) {
    external_messages.enqueue({mq, msg, jam, requeue_enabled[static_cast<int>(src)]});
    __atomic_store_n(&ultramodern_external_pending, 1, __ATOMIC_RELEASE);
}

void ultramodern::enqueue_external_message(PTR(OSMesgQueue) mq, OSMesg msg, bool jam, bool requeue_if_blocked) {
    external_messages.enqueue({mq, msg, jam, requeue_if_blocked});
    __atomic_store_n(&ultramodern_external_pending, 1, __ATOMIC_RELEASE);
}

bool do_send(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, bool jam, bool block);

// 3DS port: external (interrupt-like) messages that could not be delivered yet, oldest first.
// Upstream re-enqueued a blocked message at the back of the shared queue, which let a later
// message for the same game queue overtake it (e.g. an RDP-done before its SP-done), and NuSystem's
// scheduler then never saw a graphics task finish - the game stalled for tens of seconds. Messages
// are now held in order, and once one message for a queue is held, later ones for that queue wait
// behind it.
static std::deque<QueuedMessage> held_messages;
static std::mutex held_messages_mutex;

static void deliver_held_messages(RDRAM_ARG1) {
    std::lock_guard lock{ held_messages_mutex };
    QueuedMessage incoming;
    while (external_messages.try_dequeue(incoming)) {
        held_messages.push_back(incoming);
    }
    PTR(OSMesgQueue) blocked_queues[16];
    int num_blocked = 0;
    for (auto it = held_messages.begin(); it != held_messages.end();) {
        bool queue_blocked = std::find(blocked_queues, blocked_queues + num_blocked, it->mq) != blocked_queues + num_blocked;
        if (!queue_blocked && do_send(PASS_RDRAM it->mq, it->mesg, it->jam, false)) {
            it = held_messages.erase(it);
            continue;
        }
        if (!queue_blocked && num_blocked < 16) {
            blocked_queues[num_blocked++] = it->mq;
        }
        if (!it->requeue_if_blocked) {
            // Sources that don't requeue (e.g. VI, AI) are dropped when they can't be delivered.
            it = held_messages.erase(it);
            continue;
        }
        ++it;
    }
}

void dequeue_external_messages(RDRAM_ARG1) {
    deliver_held_messages(PASS_RDRAM1);
}

// 3DS port: interrupt state for __osDisableInt/__osRestoreInt (librecomp/ultra_translation.cpp).
extern "C" { volatile int ultramodern_interrupts_enabled = 1; }

// 3DS port: preemption point, called from every recompiled function's entry when an external
// message is pending. Upstream only switches threads inside OS calls, so a low-priority thread in a
// long compute loop (Mega Man 64 sorts its object list with an O(n^2) quicksort in a priority-10
// thread while loading) kept the frame thread from running for ~40 s. On the N64 the VI/SP/DP
// interrupt would preempt it; this does the same.
extern "C" void ultramodern_preempt_point(RDRAM_ARG1) {
    if (!ultramodern_interrupts_enabled || !ultramodern::is_game_thread()) {
        return;
    }
    __atomic_store_n(&ultramodern_external_pending, 0, __ATOMIC_RELAXED);
    dequeue_external_messages(PASS_RDRAM1);
    ultramodern::check_running_queue(PASS_RDRAM1);
}

void ultramodern::wait_for_external_message(RDRAM_ARG1) {
    QueuedMessage to_send;
    external_messages.wait_dequeue(to_send);
    {
        std::lock_guard lock{ held_messages_mutex };
        held_messages.push_back(to_send);
    }
    deliver_held_messages(PASS_RDRAM1);
}

void ultramodern::wait_for_external_message_timed(RDRAM_ARG u32 millis) {
    QueuedMessage to_send;
    if (external_messages.wait_dequeue_timed(to_send, std::chrono::milliseconds{millis})) {
        std::lock_guard lock{ held_messages_mutex };
        held_messages.push_back(to_send);
    }
    deliver_held_messages(PASS_RDRAM1);
}

extern "C" void osCreateMesgQueue(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg, s32 count) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    mq->blocked_on_recv = NULLPTR;
    mq->blocked_on_send = NULLPTR;
    mq->msgCount = count;
    mq->msg = msg;
    mq->validCount = 0;
    mq->first = 0;
}

s32 MQ_GET_COUNT(OSMesgQueue *mq) {
    return mq->validCount;
}

s32 MQ_IS_EMPTY(OSMesgQueue *mq) {
    return mq->validCount == 0;
}

s32 MQ_IS_FULL(OSMesgQueue* mq) {
    return MQ_GET_COUNT(mq) >= mq->msgCount;
}

bool do_send(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, bool jam, bool block) {
    OSMesgQueue* mq = TO_PTR(OSMesgQueue, mq_);
    if (!block) {
        // If non-blocking, fail if the queue is full.
        if (MQ_IS_FULL(mq)) {
            return false;
        }
    }
    else {
        // Otherwise, yield this thread until the queue has room.
        while (MQ_IS_FULL(mq)) {
            debug_printf("[Message Queue] Thread %d is blocked on send\n", TO_PTR(OSThread, ultramodern::this_thread())->id);
            ultramodern::thread_queue_insert(PASS_RDRAM GET_MEMBER(OSMesgQueue, mq_, blocked_on_send), ultramodern::this_thread());
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
        }
    }
    
    if (jam) {
        // Jams insert at the head of the message queue's buffer.
        mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
        TO_PTR(OSMesg, mq->msg)[mq->first] = msg;
        mq->validCount++;
    }
    else {
        // Sends insert at the tail of the message queue's buffer.
        s32 last = (mq->first + mq->validCount) % mq->msgCount;
        TO_PTR(OSMesg, mq->msg)[last] = msg;
        mq->validCount++;
    }

    // If any threads were blocked on receiving from this message queue, pop the first one and schedule it.
    PTR(PTR(OSThread)) blocked_queue = GET_MEMBER(OSMesgQueue, mq_, blocked_on_recv);
    if (!ultramodern::thread_queue_empty(PASS_RDRAM blocked_queue)) {
        ultramodern::schedule_running_thread(PASS_RDRAM ultramodern::thread_queue_pop(PASS_RDRAM blocked_queue));
    }
    
    return true;
}

bool do_recv(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg_, bool block) {
    OSMesgQueue* mq = TO_PTR(OSMesgQueue, mq_);
    if (!block) {
        // If non-blocking, fail if the queue is empty
        if (MQ_IS_EMPTY(mq)) {
            return false;
        }
    } else {
        // Otherwise, yield this thread in a loop until the queue is no longer full
        while (MQ_IS_EMPTY(mq)) {
            debug_printf("[Message Queue] Thread %d is blocked on receive\n", TO_PTR(OSThread, ultramodern::this_thread())->id);
            ultramodern::thread_queue_insert(PASS_RDRAM GET_MEMBER(OSMesgQueue, mq_, blocked_on_recv), ultramodern::this_thread());
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
        }
    }

    if (msg_ != NULLPTR) {
        *TO_PTR(OSMesg, msg_) = TO_PTR(OSMesg, mq->msg)[mq->first];
    }
    
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;

    // If any threads were blocked on sending to this message queue, pop the first one and schedule it.
    PTR(PTR(OSThread)) blocked_queue = GET_MEMBER(OSMesgQueue, mq_, blocked_on_send);
    if (!ultramodern::thread_queue_empty(PASS_RDRAM blocked_queue)) {
        ultramodern::schedule_running_thread(PASS_RDRAM ultramodern::thread_queue_pop(PASS_RDRAM blocked_queue));
    }

    return true;
}

// 3DS port (diagnostics): what each game thread is blocked on, by thread id, plus the last
// recompiled function entered before it blocked (needs trace_mode, see source/port/trace.h).
#include "trace.h"
std::atomic<int> ultramodern_running_thread{ -1 };
static std::atomic<int32_t> blocked_queue[32];
static std::atomic<char> blocked_kind[32];
constexpr int caller_history = 12;
static const char* volatile blocked_caller[32][caller_history]; // newest first

struct BlockedScope {
    int id = -1;
    BlockedScope(RDRAM_ARG char kind, PTR(OSMesgQueue) mq, bool blocking) {
        if (!blocking || !ultramodern::this_thread()) return;
        id = TO_PTR(OSThread, ultramodern::this_thread())->id & 31;
        blocked_queue[id] = mq;
        blocked_kind[id] = kind;
        // Only one game thread runs at a time, so the newest trace entries are this thread's.
        unsigned pos = recomp_trace_pos;
        for (int i = 0; i < caller_history; i++) {
            blocked_caller[id][i] = recomp_trace_ring[(pos - 1 - i) & (RECOMP_TRACE_RING - 1)];
        }
    }
    ~BlockedScope() {
        if (id >= 0) blocked_queue[id] = 0;
    }
};

extern "C" void ultramodern_debug_dump_waits(void) {
    {
        std::lock_guard lock{ held_messages_mutex };
        fprintf(stderr, "[held] %u undelivered:", (unsigned)held_messages.size());
        for (size_t i = 0; i < held_messages.size() && i < 8; i++) {
            fprintf(stderr, " mq%08X=%08X", (unsigned)held_messages[i].mq, (unsigned)held_messages[i].mesg);
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "[waits] running=thread%d", ultramodern_running_thread.load());
    for (int i = 0; i < 32; i++) {
        int32_t mq = blocked_queue[i].load();
        if (mq) fprintf(stderr, " thread%d:%c%08X@%s", i, blocked_kind[i].load(), (unsigned)mq, blocked_caller[i][0] ? blocked_caller[i][0] : "?");
    }
    fprintf(stderr, "\n");
    // Recent call path of each blocked game thread (ids 3 and up; lower ids are NuSystem internals).
    for (int t = 3; t < 32; t++) {
        if (!blocked_queue[t].load()) continue;
        fprintf(stderr, "[waits] thread%d path (newest first):", t);
        for (int i = 0; i < caller_history; i++) {
            if (blocked_caller[t][i]) fprintf(stderr, " %s", blocked_caller[t][i]);
        }
        fprintf(stderr, "\n");
    }
}

extern "C" s32 osSendMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    bool jam = false;
    
    // Don't directly send to the message queue if this isn't a game thread to avoid contention.
    if (!ultramodern::is_game_thread()) {
        ultramodern::enqueue_external_message(mq_, msg, jam, false);
        return 0;
    }
    
    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // Try to send the message.
    BlockedScope blocked(PASS_RDRAM 'S', mq_, flags == OS_MESG_BLOCK);
    bool sent = do_send(PASS_RDRAM mq_, msg, jam, flags == OS_MESG_BLOCK);
    
    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return sent ? 0 : -1;
}

extern "C" s32 osJamMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    bool jam = true;
    
    // Don't directly send to the message queue if this isn't a game thread to avoid contention.
    if (!ultramodern::is_game_thread()) {
        ultramodern::enqueue_external_message(mq_, msg, jam, false);
        return 0;
    }
    
    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // Try to send the message.
    BlockedScope blocked(PASS_RDRAM 'S', mq_, flags == OS_MESG_BLOCK);
    bool sent = do_send(PASS_RDRAM mq_, msg, jam, flags == OS_MESG_BLOCK);
    
    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return sent ? 0 : -1;
}

extern "C" s32 osRecvMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg_, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);

    assert(ultramodern::is_game_thread() && "RecvMesg not allowed outside of game threads.");

    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // Try to receive a message.
    BlockedScope blocked(PASS_RDRAM 'R', mq_, flags == OS_MESG_BLOCK);
    bool received = do_recv(PASS_RDRAM mq_, msg_, flags == OS_MESG_BLOCK);
    
    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return received ? 0 : -1;
}
