#include <atomic>
#include <cstdio>
#include <thread>
#include <cassert>
#include <string>

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "blockingconcurrentqueue.h"

#include "ultramodern/threads.hpp"

// Native APIs only used to set thread names for easier debugging
#ifdef _WIN32
#include <Windows.h>
#endif

static ultramodern::threads::callbacks_t threads_callbacks;

void ultramodern::threads::set_callbacks(const callbacks_t& callbacks) {
    threads_callbacks = callbacks;
}

std::string ultramodern::threads::get_game_thread_name(const OSThread* t) {
    if (threads_callbacks.get_game_thread_name == nullptr) {
        return "Game Thread " + std::to_string(t->id);
    }
    return threads_callbacks.get_game_thread_name(t);
}

extern "C" void bootproc();

thread_local bool is_entrypoint_thread = false;
// Whether this thread is part of the game (i.e. the start thread or one spawned by osCreateThread)
thread_local bool is_game_thread = false;
thread_local PTR(OSThread) thread_self = NULLPTR;

void ultramodern::set_entrypoint_thread() {
    ::is_game_thread = true;
    ::is_entrypoint_thread = true;
}

bool ultramodern::is_entrypoint_thread() {
    return ::is_entrypoint_thread;
}

bool ultramodern::is_game_thread() {
    return ::is_game_thread;
}

#if 0
int main(int argc, char** argv) {
    ultramodern::set_entrypoint_thread();

    bootproc();
}
#endif

#if 1
void run_thread_function(uint8_t* rdram, uint64_t addr, uint64_t sp, uint64_t arg);
#else
#define run_thread_function(func, sp, arg) func(arg)
#endif

#if defined(_WIN32)
void ultramodern::set_native_thread_name(const std::string& name) {
    std::wstring wname{name.begin(), name.end()};

    HRESULT r;
    r = SetThreadDescription(
        GetCurrentThread(),
        wname.c_str()
    );
}

void ultramodern::set_native_thread_priority(ThreadPriority pri) {
    int nPriority = THREAD_PRIORITY_NORMAL;

    // Convert ThreadPriority to Win32 priority
    switch (pri) {
        case ThreadPriority::Low:
            nPriority = THREAD_PRIORITY_BELOW_NORMAL;
            break;
        case ThreadPriority::Normal:
            nPriority = THREAD_PRIORITY_NORMAL;
            break;
        case ThreadPriority::High:
            nPriority = THREAD_PRIORITY_ABOVE_NORMAL;
            break;
        case ThreadPriority::VeryHigh:
            nPriority = THREAD_PRIORITY_HIGHEST;
            break;
        case ThreadPriority::Critical:
            nPriority = THREAD_PRIORITY_TIME_CRITICAL;
            break;
        default:
            throw std::runtime_error("Invalid thread priority!");
            break;
    }
    // SetThreadPriority(GetCurrentThread(), nPriority);
}
#elif defined(__3DS__)
extern "C" {
#include <3ds/types.h>
#include <3ds/svc.h>
}

// 3DS port: kernel threads have no names.
void ultramodern::set_native_thread_name(const std::string& name) {}

// 3DS port: the kernel never time-slices threads of equal priority, so a game thread that spins
// would starve every thread at its level. Event threads (VI, timer, RSP/gfx tasks) therefore run
// above the front end's main thread (0x30) and game threads ("High", one runs at a time) below it,
// so neither the threads that wake the game nor the bottom-screen UI can be starved.
// Lower number = higher priority; applications may use 0x18-0x3F.
void ultramodern::set_native_thread_priority(ThreadPriority pri) {
    s32 prio = 0x30;
    switch (pri) {
        case ThreadPriority::Critical: prio = 0x19; break; // VI thread
        case ThreadPriority::VeryHigh: prio = 0x1A; break; // timer thread
        case ThreadPriority::Normal:   prio = 0x1B; break; // SP task + graphics threads
        case ThreadPriority::High:     prio = 0x31; break; // game threads
        case ThreadPriority::Low:      prio = 0x32; break;
    }
    svcSetThreadPriority(CUR_THREAD_HANDLE, prio);
}
#elif defined(__linux__)
#include <sys/prctl.h>

void ultramodern::set_native_thread_name(const std::string& name) {
    if (name.length() > 15) {
        // Linux only accepts up to 16 characters including the null terminator for a thread name.
        debug_printf("[Thread] The thread name '%s' will be truncated to 15 characters", name.c_str());
    }

    prctl(PR_SET_NAME, name.c_str());
}

void ultramodern::set_native_thread_priority(ThreadPriority pri) {
    // TODO linux thread priority
    // printf("set_native_thread_priority unimplemented\n");
    // int nPriority = THREAD_PRIORITY_NORMAL;

    // // Convert ThreadPriority to Win32 priority
    // switch (pri) {
    //     case ThreadPriority::Low:
    //         nPriority = THREAD_PRIORITY_BELOW_NORMAL;
    //         break;
    //     case ThreadPriority::Normal:
    //         nPriority = THREAD_PRIORITY_NORMAL;
    //         break;
    //     case ThreadPriority::High:
    //         nPriority = THREAD_PRIORITY_ABOVE_NORMAL;
    //         break;
    //     case ThreadPriority::VeryHigh:
    //         nPriority = THREAD_PRIORITY_HIGHEST;
    //         break;
    //     case ThreadPriority::Critical:
    //         nPriority = THREAD_PRIORITY_TIME_CRITICAL;
    //         break;
    //     default:
    //         throw std::runtime_error("Invalid thread priority!");
    //         break;
    // }
}
#elif defined(__APPLE__)
void ultramodern::set_native_thread_name(const std::string& name) {
    if (name.length() > 15) {
        // Macs seem to only accept up to 16 characters including the null terminator for a thread name.
        debug_printf("[Thread] The thread name '%s' will be truncated to 15 characters", name.c_str());
    }

    pthread_setname_np(name.c_str());
}

void ultramodern::set_native_thread_priority(ThreadPriority pri) {}
#endif

extern std::atomic<int> ultramodern_running_thread; // mesgqueue.cpp (3DS port diagnostics)

// 3DS port diagnostics: total latency from signalling a game thread to it running (ticks).
extern "C" { std::atomic<uint64_t> ultramodern_switch_ticks{0}; std::atomic<unsigned> ultramodern_switch_count{0}; }

void wait_for_resumed(RDRAM_ARG UltraThreadContext* thread_context) {
    thread_context->running.wait();
    if (thread_context->signal_tick) {
        ultramodern_switch_ticks += svcGetSystemTick() - thread_context->signal_tick;
        ultramodern_switch_count++;
        thread_context->signal_tick = 0;
    }
    if (thread_self) ultramodern_running_thread = TO_PTR(OSThread, thread_self)->id;
    // If this thread's context was replaced by another thread or deleted, destroy it again from its own context.
    // This will trigger thread cleanup instead.
    if (TO_PTR(OSThread, ultramodern::this_thread())->context != thread_context) {
        osDestroyThread(PASS_RDRAM NULLPTR);
    }
}

// 3DS port diagnostics: game thread switches (resumes), logged by the watchdog (main.c).
extern "C" { std::atomic<unsigned> ultramodern_thread_resumes{0}; }

void resume_thread(OSThread* t) {
    ultramodern_thread_resumes++;
    debug_printf("[Thread] Resuming execution of thread %d\n", t->id);
    t->context->signal_tick = svcGetSystemTick();
    t->context->running.signal();
}

void run_next_thread(RDRAM_ARG1) {
    if (ultramodern::thread_queue_empty(PASS_RDRAM ultramodern::running_queue)) {
        throw std::runtime_error("No threads left to run!\n");
    }

    OSThread* to_run = TO_PTR(OSThread, ultramodern::thread_queue_pop(PASS_RDRAM ultramodern::running_queue));
    ultramodern_thread_resumes++;
    debug_printf("[Scheduling] Resuming execution of thread %d\n", to_run->id);
    to_run->context->signal_tick = svcGetSystemTick();
    to_run->context->running.signal();
}

void ultramodern::run_next_thread_and_wait(RDRAM_ARG1) {
    UltraThreadContext* cur_context = TO_PTR(OSThread, thread_self)->context;
    run_next_thread(PASS_RDRAM1);
    wait_for_resumed(PASS_RDRAM cur_context);
}

void ultramodern::resume_thread_and_wait(RDRAM_ARG OSThread *t) {
    UltraThreadContext* cur_context = TO_PTR(OSThread, thread_self)->context;
    resume_thread(t);
    wait_for_resumed(PASS_RDRAM cur_context);
}

static void _thread_func(RDRAM_ARG PTR(OSThread) self_, PTR(thread_func_t) entrypoint, PTR(void) arg, UltraThreadContext* thread_context) {
    OSThread *self = TO_PTR(OSThread, self_);
    debug_printf("[Thread] Thread created: %d\n", self->id);
    thread_self = self_;
    is_game_thread = true;

    // Set the thread name
    ultramodern::set_native_thread_name(ultramodern::threads::get_game_thread_name(self));
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::High);

    // Signal the initialized semaphore to indicate that this thread can be started.
    thread_context->initialized.signal();

    debug_printf("[Thread] Thread waiting to be started: %d\n", self->id);

    // Wait until the thread is marked as running.
    try {
        wait_for_resumed(PASS_RDRAM thread_context);
    } catch (ultramodern::thread_terminated& terminated) {
    }

    // Make sure the thread wasn't replaced or destroyed before it was started.
    if (self->context == thread_context) {
        debug_printf("[Thread] Thread started: %d\n", self->id);
        try {
            // Run the thread's function with the provided argument.
            run_thread_function(PASS_RDRAM entrypoint, self->sp, arg);
        } catch (ultramodern::thread_terminated& terminated) {
        }
    }
    else {
        debug_printf("[Thread] Thread destroyed before being started: %d\n", self->id);
    }

    // Check if the thread hasn't been destroyed or replaced. If so, then the thread terminated or destroyed itself,
    // so mark this thread as destroyed and run the next queued thread.
    if (self->context == thread_context) {
        self->context = nullptr;
        run_next_thread(PASS_RDRAM1);
    }

    // Dispose of this thread now that it's completed or terminated.
    ultramodern::cleanup_thread(thread_context);
    // 3DS port: the system thread goes back to the pool (core_thread.hpp) and may run another game
    // thread next, so it mustn't keep this one's identity.
    thread_self = NULLPTR;
    is_game_thread = false;
}

extern "C" void osStartThread(RDRAM_ARG PTR(OSThread) t_) {
    OSThread* t = TO_PTR(OSThread, t_);
    debug_printf("[os] Start Thread %d\n", t->id);

    // If this is a game thread, insert the new thread into the running queue and then check the running queue.
    if (thread_self) {
        ultramodern::schedule_running_thread(PASS_RDRAM t_);
        ultramodern::check_running_queue(PASS_RDRAM1);
    }
    // Otherwise, immediately start the thread and terminate this one.
    else {
        t->state = OSThreadState::QUEUED;
        resume_thread(t);
        //throw ultramodern::thread_terminated{};
    }
}

extern "C" void osCreateThread(RDRAM_ARG PTR(OSThread) t_, OSId id, PTR(thread_func_t) entrypoint, PTR(void) arg, PTR(void) sp, OSPri pri) {
    debug_printf("[os] Create Thread %d\n", id);
    OSThread *t = TO_PTR(OSThread, t_);
    
    t->next = NULLPTR;
    t->queue = NULLPTR;
    t->priority = pri;
    t->id = id;
    t->state = OSThreadState::STOPPED;
    t->sp = sp - 0x10; // Set up the first stack frame

    // Spawn a new thread, which will immediately pause itself and wait until it's been started.
    // Pass the context as an argument to the thread function to ensure that it can't get cleared before the thread captures its value.
    UltraThreadContext* context = new UltraThreadContext{};
    t->context = context;
#ifdef __3DS__
    // 3DS port: a fiber (fibers.hpp). It first runs once it's resumed, so there's nothing to wait for.
    ultramodern::fiber_create([rdram, t_, entrypoint, arg, context]() { _thread_func(rdram, t_, entrypoint, arg, context); },
                              &context->running);
#else
    context->host_thread = ultramodern::CoreThread{ultramodern::core::game, 128 * 1024, _thread_func, PASS_RDRAM t_, entrypoint, arg, t->context};

    // Wait until the thread is initialized to indicate that it's ready to be started.
    context->initialized.wait();
#endif
    debug_printf("[os] Thread %d is ready to be started\n", t->id);
}

extern "C" void osStopThread(RDRAM_ARG PTR(OSThread) t_) {
    if (t_ == NULLPTR) {
        t_ = thread_self;
    }
    // Check if the thread is stopping itself (arg is null or thread_self).
    if (t_ == thread_self) {
        ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
    }
    else {
        assert(false);
    }
}

extern "C" void osDestroyThread(RDRAM_ARG PTR(OSThread) t_) {
    if (t_ == NULLPTR) {
        t_ = thread_self;
    }
    OSThread* t = TO_PTR(OSThread, t_);
    // Check if the thread is destroying itself (arg is null or thread_self)
    if (t_ == thread_self) {
        throw ultramodern::thread_terminated{};
    }
    // Otherwise if the thread isn't stopped, remove it from its currrent queue., 
    if (t->state != OSThreadState::STOPPED) {
        ultramodern::thread_queue_remove(PASS_RDRAM t->queue, t_);
    }
    // Check if the thread has already been destroyed to prevent destroying it again.
    UltraThreadContext* cur_context = t->context;
    if (cur_context != nullptr) {
        // Mark the target thread as destroyed and resume it. When it starts it'll check this and terminate itself instead of resuming.
        t->context = nullptr;
        cur_context->running.signal();
    }
}

extern "C" void osSetThreadPri(RDRAM_ARG PTR(OSThread) t_, OSPri pri) {
    if (t_ == NULLPTR) {
        t_ = thread_self;
    }
    OSThread* t = TO_PTR(OSThread, t_);

    if (t->priority != pri) {
        t->priority = pri;

        if (t_ != ultramodern::this_thread() && t->state != OSThreadState::STOPPED) {
            ultramodern::thread_queue_remove(PASS_RDRAM t->queue, t_);
            ultramodern::thread_queue_insert(PASS_RDRAM t->queue, t_);
        }

        ultramodern::check_running_queue(PASS_RDRAM1);
    }
}

extern "C" OSPri osGetThreadPri(RDRAM_ARG PTR(OSThread) t) {
    if (t == NULLPTR) {
        t = thread_self;
    }
    return TO_PTR(OSThread, t)->priority;
}

extern "C" OSId osGetThreadId(RDRAM_ARG PTR(OSThread) t) {
    if (t == NULLPTR) {
        t = thread_self;
    }
    return TO_PTR(OSThread, t)->id;
}

PTR(OSThread) ultramodern::this_thread() {
    return thread_self;
}

static std::thread thread_cleaner_thread;
static moodycamel::BlockingConcurrentQueue<UltraThreadContext*> deleted_threads{};
extern std::atomic_bool exited;

void thread_cleaner_func() {
    using namespace std::chrono_literals;
    while (!exited) {
        UltraThreadContext* to_delete;
        if (deleted_threads.wait_dequeue_timed(to_delete, 10ms)) {
            debug_printf("[Cleanup] Deleting thread context %p\n", to_delete);

            to_delete->host_thread.join();
            delete to_delete;
        }
    }
}

void ultramodern::init_thread_cleanup() {
    thread_cleaner_thread = std::thread{thread_cleaner_func};
}

void ultramodern::cleanup_thread(UltraThreadContext *cur_context) {
    deleted_threads.enqueue(cur_context);
}

void ultramodern::join_thread_cleaner_thread() {
    thread_cleaner_thread.join();
}

#ifdef __3DS__
// --- 3DS port: game threads as fibers (fibers.hpp) -------------------------------------------------

#include <cstdlib>
#include <mutex>
#include <vector>

extern "C" {
#include <3ds/synchronization.h>
}

namespace {

struct FiberRegs {
    uint32_t r[8];   // r4-r11
    uint32_t sp, lr;
    double d[8];     // d8-d15
};

struct Fiber {
    FiberRegs regs{};
    void* stack = nullptr;
    std::function<void()> body;
    ultramodern::FiberSemaphore* waiting_on = nullptr; // runnable once this has a count
    bool dead = false;
    // The thread-locals of the game thread it is (saved while switched out).
    PTR(OSThread) self = NULLPTR;
    bool is_game = false, is_entry = false;
};

constexpr size_t fiber_stack_size = 128 * 1024; // as the system threads had

std::mutex fibers_mutex;            // fibers may be created from outside the host (boot)
std::vector<Fiber*> fibers;
Fiber* current = nullptr;           // on the host: the running fiber, nullptr = the host loop
FiberRegs host_regs{};
LightSemaphore host_wake;           // signals from outside the host
thread_local bool is_fiber_host = false;
ultramodern::CoreThread host_thread;
bool host_started = false;

} // namespace

extern "C" void fiber_switch(FiberRegs* from, const FiberRegs* to); // fiber_switch.s
extern "C" void fiber_trampoline();

// Switches the host from whatever runs now (a fiber or the host loop) to `next` (nullptr: the loop).
static void run_fiber(Fiber* next) {
    Fiber* prev = current;
    FiberRegs* from = &host_regs;
    if (prev) {
        prev->self = thread_self;
        prev->is_game = is_game_thread;
        prev->is_entry = is_entrypoint_thread;
        from = &prev->regs;
    }
    current = next;
    thread_self = next ? next->self : NULLPTR;
    is_game_thread = next ? next->is_game : false;
    is_entrypoint_thread = next ? next->is_entry : false;
    fiber_switch(from, next ? &next->regs : &host_regs);
}

// A fiber other than `except` that has been signalled, or nullptr.
static Fiber* runnable_fiber(Fiber* except) {
    std::lock_guard lock{ fibers_mutex };
    for (Fiber* f : fibers) {
        if (f != except && !f->dead && f->waiting_on && f->waiting_on->count.load() > 0) return f;
    }
    return nullptr;
}

extern "C" void fiber_entry(Fiber* f) {
    f->body();
    // Finished: never runs again; the host loop frees the stack (not this fiber, which is on it).
    f->dead = true;
    f->waiting_on = nullptr;
    run_fiber(runnable_fiber(f));
    for (;;) {}
}

// Frees finished fibers' stacks. Only on the host (a finished fiber has switched off its stack by the
// time anything else runs there) and never the running one.
static void free_dead_fibers() {
    std::lock_guard lock{ fibers_mutex };
    for (auto it = fibers.begin(); it != fibers.end();) {
        if ((*it)->dead && *it != current) {
            free((*it)->stack);
            delete *it;
            it = fibers.erase(it);
        } else {
            ++it;
        }
    }
}

static void fiber_host_loop() {
    is_fiber_host = true;
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::High);
    for (;;) {
        free_dead_fibers();
        Fiber* next = runnable_fiber(nullptr);
        if (next) {
            run_fiber(next);
        } else {
            LightSemaphore_Acquire(&host_wake, 1);
        }
    }
}

bool ultramodern::on_fiber_host() { return is_fiber_host; }

void ultramodern::fiber_create(std::function<void()> body, FiberSemaphore* start) {
    if (is_fiber_host) free_dead_fibers(); // fibers mostly hand over to each other, rarely to the loop
    Fiber* f = new Fiber{};
    f->body = std::move(body);
    f->stack = aligned_alloc(16, fiber_stack_size);
    if (!f->stack) {
        fprintf(stderr, "[fibers] out of memory for a %u KB stack\n", (unsigned)(fiber_stack_size / 1024));
        abort();
    }
    f->regs.sp = ((uint32_t)f->stack + fiber_stack_size) & ~7u;
    f->regs.lr = (uint32_t)&fiber_trampoline;
    f->regs.r[0] = (uint32_t)f; // r4: the trampoline's argument
    f->waiting_on = start;
    bool start_host = false;
    {
        std::lock_guard lock{ fibers_mutex };
        fibers.push_back(f);
        static unsigned created;
        if (++created % 16 == 1) fprintf(stderr, "[fibers] %u created, %u alive\n", created, (unsigned)fibers.size());
        if (!host_started) {
            host_started = true;
            start_host = true;
        }
    }
    if (start_host) {
        LightSemaphore_Init(&host_wake, 0, 0x7FFF);
        host_thread = ultramodern::CoreThread{ ultramodern::core::game, 64 * 1024, fiber_host_loop };
    }
    if (!is_fiber_host) LightSemaphore_Release(&host_wake, 1);
}

bool ultramodern::FiberSemaphore::try_take() {
    int c = count.load();
    while (c > 0) {
        if (count.compare_exchange_weak(c, c - 1)) return true;
    }
    return false;
}

void ultramodern::FiberSemaphore::signal() {
    count.fetch_add(1);
    if (!is_fiber_host && host_started) LightSemaphore_Release(&host_wake, 1);
}

void ultramodern::FiberSemaphore::wait() {
    if (try_take()) return;
    if (!is_fiber_host || !current) {
        // Not a fiber (shouldn't happen): wait the slow way.
        while (!try_take()) svcSleepThread(100000);
        return;
    }
    Fiber* self = current;
    self->waiting_on = this;
    while (!try_take()) {
        run_fiber(runnable_fiber(self)); // back here once someone switches to us
    }
    self->waiting_on = nullptr;
}
#endif
