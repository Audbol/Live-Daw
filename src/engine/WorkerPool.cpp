#include "engine/WorkerPool.h"

#include <thread>

namespace livedaw
{

class WorkerPool::Worker final : public juce::Thread
{
public:
    Worker (WorkerPool& p, int index)
        : juce::Thread ("LiveDAW DSP worker " + juce::String (index)), pool (p)
    {
    }

    void start (double sampleRate, int blockSize)
    {
        const auto options = juce::Thread::RealtimeOptions {}
                                 .withPriority (9)
                                 .withApproximateAudioProcessingTime (blockSize, sampleRate);

        if (! startRealtimeThread (options))
            startThread (juce::Thread::Priority::highest);
    }

    void run() override
    {
        uint32_t seen = pool.wakeSignal.load (std::memory_order_acquire);

        while (! pool.shouldExit.load (std::memory_order_acquire))
        {
            // Spin briefly (the next block is usually only a few ms away), then sleep.
            const int spins = pool.spinIterations.load (std::memory_order_relaxed);
            int i = 0;
            while (pool.wakeSignal.load (std::memory_order_acquire) == seen && i++ < spins)
                std::this_thread::yield();

            if (pool.wakeSignal.load (std::memory_order_acquire) == seen)
                pool.wakeSignal.wait (seen, std::memory_order_acquire);

            seen = pool.wakeSignal.load (std::memory_order_acquire);

            if (pool.shouldExit.load (std::memory_order_acquire))
                break;

            pool.runTasks();
        }
    }

private:
    WorkerPool& pool;
};

WorkerPool::WorkerPool (int numWorkers)
{
    for (int i = 0; i < numWorkers; ++i)
        workers.push_back (std::make_unique<Worker> (*this, i + 1));

    for (auto& w : workers)
        w->start (48000.0, 256);
}

WorkerPool::~WorkerPool()
{
    shouldExit.store (true, std::memory_order_release);
    wakeSignal.fetch_add (1, std::memory_order_acq_rel);
    wakeSignal.notify_all();

    for (auto& w : workers)
        w->stopThread (2000);
}

int WorkerPool::defaultNumWorkers()
{
    // Leave one core for the audio thread itself (which also does work) and one
    // for the UI/OS.
    return juce::jmax (0, juce::SystemStats::getNumCpus() - 2);
}

void WorkerPool::setBlockDurationHint (double sampleRate, int blockSize)
{
    if (sampleRate <= 0.0 || blockSize <= 0)
        return;

    // Spin for roughly a quarter of a block before falling back to a futex wait.
    const double blockMicros = 1.0e6 * blockSize / sampleRate;
    spinIterations.store (juce::jlimit (100, 20000, static_cast<int> (blockMicros * 2.0)));
}

void WorkerPool::runTasks() noexcept
{
    for (;;)
    {
        uint64_t w = work.load (std::memory_order_acquire);
        const auto index = static_cast<int> (w & 0xffffu);
        const auto count = static_cast<int> ((w >> 16) & 0xffffu);

        if (index >= count)
            return;

        // If the claim below succeeds, this generation still had unclaimed work when
        // these were loaded, so the caller cannot have overwritten them yet.
        const auto task = jobTask.load (std::memory_order_acquire);
        auto* const ctx = jobContext.load (std::memory_order_acquire);

        if (task == nullptr)
            return;

        if (! work.compare_exchange_weak (w, w + 1, std::memory_order_acq_rel))
            continue;

        task (ctx, index);
        remaining.fetch_sub (1, std::memory_order_acq_rel);
    }
}

void WorkerPool::parallelFor (int count, TaskFn task, void* context) noexcept
{
    if (count <= 0)
        return;

    if (workers.empty() || count == 1)
    {
        for (int i = 0; i < count; ++i)
            task (context, i);
        return;
    }

    if (count > kMaxTasksPerJob)
    {
        // Split huge jobs; each chunk is a separate fork/join.
        struct Offset
        {
            TaskFn task;
            void* context;
            int base;
        };

        for (int base = 0; base < count; base += kMaxTasksPerJob)
        {
            Offset o { task, context, base };
            parallelFor (juce::jmin (kMaxTasksPerJob, count - base),
                         [] (void* c, int i) { auto* off = static_cast<Offset*> (c); off->task (off->context, off->base + i); },
                         &o);
        }
        return;
    }

    ++generation;
    jobTask.store (task, std::memory_order_relaxed);
    jobContext.store (context, std::memory_order_relaxed);
    remaining.store (count, std::memory_order_relaxed);
    work.store ((static_cast<uint64_t> (generation) << 32) | (static_cast<uint64_t> (count) << 16),
                std::memory_order_release);

    wakeSignal.fetch_add (1, std::memory_order_acq_rel);
    wakeSignal.notify_all();

    runTasks();

    while (remaining.load (std::memory_order_acquire) > 0)
        std::this_thread::yield();
}

} // namespace livedaw
