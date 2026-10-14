#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace livedaw
{

/** Deferred deletion for objects that the audio thread might still be using.

    The message thread "retires" an object after unpublishing it. The object is
    destroyed (on the message thread) only once the audio callback has completed
    at least one full cycle since the retirement, which guarantees that no
    in-flight callback still holds a pointer to it. The audio thread never
    allocates or frees anything.
*/
class RtGarbage
{
public:
    /** Called by the audio thread at the start/end of every callback. */
    void audioCallbackStarted() noexcept { inCallback.store (true, std::memory_order_seq_cst); }
    void audioCallbackFinished() noexcept
    {
        callbackCount.fetch_add (1, std::memory_order_seq_cst);
        inCallback.store (false, std::memory_order_seq_cst);
    }

    /** Tells the collector whether the device is running (so garbage can be freed
        immediately when no callbacks are happening). */
    void setDeviceRunning (bool running) noexcept { deviceRunning.store (running); }

    template <typename T>
    void retire (std::unique_ptr<T> object)
    {
        if (object == nullptr)
            return;

        std::lock_guard<std::mutex> lock (mutex);
        pending.push_back ({ callbackCount.load(), std::make_unique<Holder<T>> (std::move (object)) });
    }

    /** Message thread: frees everything that is now provably unused. */
    void collect()
    {
        std::vector<std::unique_ptr<Base>> toFree;

        {
            std::lock_guard<std::mutex> lock (mutex);
            const auto count = callbackCount.load();
            const bool idle = ! deviceRunning.load() && ! inCallback.load();

            for (auto it = pending.begin(); it != pending.end();)
            {
                if (idle || count > it->epoch)
                {
                    toFree.push_back (std::move (it->object));
                    it = pending.erase (it);
                }
                else
                {
                    ++it;
                }
            }
        }
        // toFree is destroyed here, outside the lock.
    }

    void collectAllNow()
    {
        std::lock_guard<std::mutex> lock (mutex);
        pending.clear();
    }

    size_t numPending() const
    {
        std::lock_guard<std::mutex> lock (mutex);
        return pending.size();
    }

private:
    struct Base
    {
        virtual ~Base() = default;
    };

    template <typename T>
    struct Holder final : Base
    {
        explicit Holder (std::unique_ptr<T> o) : object (std::move (o)) {}
        std::unique_ptr<T> object;
    };

    struct Entry
    {
        uint64_t epoch;
        std::unique_ptr<Base> object;
    };

    mutable std::mutex mutex;
    std::vector<Entry> pending;
    std::atomic<uint64_t> callbackCount { 0 };
    std::atomic<bool> inCallback { false };
    std::atomic<bool> deviceRunning { false };
};

} // namespace livedaw
