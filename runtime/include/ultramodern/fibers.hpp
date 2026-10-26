#pragma once
// 3DS port: game threads as fibers (threads.cpp, fiber_switch.s).
//
// ultramodern gave every N64 thread a system thread of its own. An application on the 3DS gets 32
// threads in all (its resource limit), and Pokemon Snap runs up to ~57 N64 threads in a course (its
// object processes are threads): on the console, creating the 23rd failed and the game froze (Azahar
// doesn't enforce the limit). Only one game thread runs at a time anyway, so they are fibers on one
// system thread on the game core: each has its own stack, and a thread waiting to be resumed switches
// straight to one that has been, in user mode.

#include <atomic>
#include <functional>

namespace ultramodern {

// Counting semaphore for a game thread's run/initialized signals. wait() is for fibers: it switches
// to another fiber (or the fiber host, which sleeps) until signalled. signal() may come from any
// thread; from outside the fiber host it also wakes the host.
struct FiberSemaphore {
    std::atomic<int> count{ 0 };
    void signal();
    void wait();
    bool try_take();
};

// Runs `body` as a new fiber on the fiber host (started on first use). The fiber first runs when
// `start` is signalled (the thread's `running`); its own code then waits on it as usual.
void fiber_create(std::function<void()> body, FiberSemaphore* start);

// Whether the caller is the fiber host thread (a fiber or the host loop).
bool on_fiber_host();

} // namespace ultramodern
