#ifndef __ULTRAMODERN_CORE_THREAD_HPP__
#define __ULTRAMODERN_CORE_THREAD_HPP__

// 3DS port: a std::thread-like thread created on a chosen CPU core. std::thread goes through
// libctru's pthread backend, which always uses the default core, so every runtime thread shared
// core 0 with the UI while the New 3DS's second application core (core 2) sat idle.
//
// The system threads are pooled: when a function finishes, its thread waits for the next one instead
// of exiting. The game destroys and recreates 4-6 of its threads at every scene change; on a real
// console, creating and ending system threads that often (on core 2) ended in a crash inside the
// system kernel at scene changes, which the pool avoids - after the first few scenes no system
// thread is ever created or ended again.

#include <3ds.h>

#include <cstdio>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

extern "C" void crash_log_attach(void); // source/port/crash_log.c: report this thread's crashes

namespace ultramodern {
    namespace core {
        constexpr int ui = -2;   // default core (core 0): UI, rendering, timing
        constexpr int game = 2;  // New 3DS extra application core: game threads and audio
        constexpr int gfx = 2;   // display-list interpretation (with the game: core 0 can't also fit it next to the UI and audio)
        constexpr int audio = -2; // audio microcode: core 0, fitted around the UI's frames (events.cpp)
    }

    namespace thread_pool {
        // One function run on a pooled thread; join() waits for `done`.
        struct Job {
            std::function<void()> fn;
            LightEvent done;
        };

        struct Worker {
            int core;
            size_t stack_size;
            LightSemaphore start; // signalled when `job` is set
            std::shared_ptr<Job> job;
            Thread handle = nullptr;
        };

        inline LightLock& lock() {
            static LightLock l = [] { LightLock ll; LightLock_Init(&ll); return ll; }();
            return l;
        }
        inline std::vector<Worker*>& idle() {
            static std::vector<Worker*> v;
            return v;
        }
        inline unsigned& created() {
            static unsigned n;
            return n;
        }

        inline void worker_main(void* arg) {
            crash_log_attach();
            Worker* w = static_cast<Worker*>(arg);
            for (;;) {
                LightSemaphore_Acquire(&w->start, 1);
                std::shared_ptr<Job> job = std::move(w->job);
                job->fn();
                job->fn = nullptr; // release what it captured before reporting done
                LightLock_Lock(&lock());
                idle().push_back(w); // available again before anyone is told this job ended
                LightLock_Unlock(&lock());
                LightEvent_Signal(&job->done);
            }
        }

        // Runs `job` on an idle thread of this core and stack size, or on a new one. False if no
        // thread could be created.
        inline bool run(int core, size_t stack_size, std::shared_ptr<Job> job) {
            Worker* w = nullptr;
            LightLock_Lock(&lock());
            auto& v = idle();
            for (size_t i = 0; i < v.size(); i++) {
                if (v[i]->core == core && v[i]->stack_size >= stack_size) {
                    w = v[i];
                    v.erase(v.begin() + i);
                    break;
                }
            }
            LightLock_Unlock(&lock());
            if (!w) {
                w = new Worker{ core, stack_size, {}, nullptr, nullptr };
                LightSemaphore_Init(&w->start, 0, 1);
                s32 prio = 0x30;
                svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
                w->handle = threadCreate(worker_main, w, stack_size, prio, core, true);
                if (!w->handle && core != -2) {
                    w->core = -2; // e.g. Old 3DS: no core 2
                    w->handle = threadCreate(worker_main, w, stack_size, prio, -2, true);
                }
                unsigned n = ++created();
                fprintf(stderr, "[threads] pool thread #%u on core %d%s\n", n, w->core, w->handle ? "" : " FAILED");
                if (!w->handle) {
                    delete w;
                    return false;
                }
            }
            w->job = std::move(job);
            LightSemaphore_Release(&w->start, 1);
            return true;
        }
    }

    class CoreThread {
    public:
        CoreThread() = default;

        template <class F, class... Args>
        CoreThread(int core, size_t stack_size, F&& f, Args&&... args) {
            auto job = std::make_shared<thread_pool::Job>();
            job->fn = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
            LightEvent_Init(&job->done, RESET_STICKY);
            if (thread_pool::run(core, stack_size, job)) this->job = std::move(job);
        }

        CoreThread(CoreThread&& other) noexcept : job(std::move(other.job)) {}
        CoreThread& operator=(CoreThread&& other) noexcept {
            std::swap(job, other.job);
            return *this;
        }
        CoreThread(const CoreThread&) = delete;
        CoreThread& operator=(const CoreThread&) = delete;
        ~CoreThread() = default; // like detach: the function runs on

        bool joinable() const { return job != nullptr; }
        void join() {
            if (!job) return;
            LightEvent_Wait(&job->done);
            job.reset();
        }
        void detach() { job.reset(); }

    private:
        std::shared_ptr<thread_pool::Job> job;
    };
}

#endif
