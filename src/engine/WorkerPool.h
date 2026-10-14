#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace livedaw
{

/** A tiny fork/join pool for the audio callback.

    The audio thread calls parallelFor(); it publishes the job, wakes the workers,
    takes part in the work itself and then waits until every index has been
    processed. Workers run at realtime priority. Nothing here allocates or locks
    on the audio thread, so mixer channel count scales with available CPU cores.
*/
class WorkerPool
{
public:
    using TaskFn = void (*) (void* context, int index);

    /** numWorkers excludes the calling (audio) thread. 0 = run everything inline. */
    explicit WorkerPool (int numWorkers);
    ~WorkerPool();

    static int defaultNumWorkers();

    int getNumWorkers() const noexcept { return static_cast<int> (workers.size()); }

    /** Runs task(context, i) for i in [0, count). Audio thread only; not re-entrant. */
    void parallelFor (int count, TaskFn task, void* context) noexcept;

    /** Convenience wrapper for lambdas (no allocation: the lambda lives on the caller's stack). */
    template <typename Fn>
    void parallelFor (int count, Fn&& fn) noexcept
    {
        using F = std::remove_reference_t<Fn>;
        parallelFor (count, [] (void* ctx, int i) { (*static_cast<F*> (ctx)) (i); }, &fn);
    }

    /** Lets the pool size its spin budget to the audio block duration. */
    void setBlockDurationHint (double sampleRate, int blockSize);

private:
    class Worker;

    void runTasks() noexcept;

    std::vector<std::unique_ptr<Worker>> workers;

    static constexpr int kMaxTasksPerJob = 0xffff;

    // Job function, published before `work` is stored with release semantics.
    std::atomic<TaskFn> jobTask { nullptr };
    std::atomic<void*> jobContext { nullptr };

    // Bits 32-63: generation, 16-31: task count, 0-15: next index to claim.
    // Keeping the count in the same word as the claim index means a worker can
    // never claim an index of one job against the count of another.
    std::atomic<uint64_t> work { 0 };
    std::atomic<int> remaining { 0 };
    std::atomic<uint32_t> wakeSignal { 0 };
    uint32_t generation = 0;

    std::atomic<bool> shouldExit { false };
    std::atomic<int> spinIterations { 2000 };

    JUCE_DECLARE_NON_COPYABLE (WorkerPool)
};

} // namespace livedaw
