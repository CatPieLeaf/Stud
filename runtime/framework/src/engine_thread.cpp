#include "stud/engine_thread.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace stud::jni_bridge {

namespace {

struct Task {
    std::function<bool()> work;
    std::shared_ptr<std::atomic<bool>> done;
    std::shared_ptr<std::atomic<bool>> result;
};

class EngineThread {
public:
    static EngineThread& instance() {
        // Function-local static: constructed on first real use, never
        // destroyed (deliberate -- the thread must outlive everything
        // that could still submit to it, and the engine holds a raw
        // pthread_t reference to it for the whole process lifetime).
        static EngineThread* self = new EngineThread();
        return *self;
    }

    void submit(Task task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(task));
        }
        cv_.notify_one();
    }

private:
    EngineThread() {
        // Detached, never joined -- see instance()'s own comment. The
        // existing pthread_create interpose attaches every thread this
        // process spawns to the JVM, so work submitted here can make
        // real JNI calls exactly like the old per-call threads could.
        std::thread([this] { run(); }).detach();
    }

    void run() {
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return !queue_.empty(); });
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            bool ok = false;
            if (task.work) {
                ok = task.work();
            }
            task.result->store(ok, std::memory_order_relaxed);
            task.done->store(true, std::memory_order_relaxed);
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> queue_;
};

}  // namespace

EngineThreadOutcome run_on_engine_thread(std::function<bool()> work, int timeout_ms) {
    Task task;
    task.work = std::move(work);
    task.done = std::make_shared<std::atomic<bool>>(false);
    task.result = std::make_shared<std::atomic<bool>>(false);

    auto done = task.done;
    auto result = task.result;
    EngineThread::instance().submit(std::move(task));

    // Real, bounded, condition-based wait -- polls the real completion
    // flag rather than assuming an elapsed time, same discipline as the
    // per-call bounded wait this replaces.
    constexpr int kPollIntervalMs = 50;
    const int max_polls = timeout_ms / kPollIntervalMs;
    int polls = 0;
    while (!done->load(std::memory_order_relaxed) && polls < max_polls) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
        ++polls;
    }

    EngineThreadOutcome outcome;
    outcome.completed = done->load(std::memory_order_relaxed);
    outcome.result = result->load(std::memory_order_relaxed);
    return outcome;
}

}  // namespace stud::jni_bridge
