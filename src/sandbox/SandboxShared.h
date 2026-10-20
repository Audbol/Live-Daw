#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <cstdint>

namespace livedaw::sandbox
{

/** What Sidecar and the plugin sandbox helper (SidecarPluginHost) share.

    Control (loading, state, editors) goes through a named pipe (a JUCE
    InterprocessConnection). Audio goes through a shared memory block and a named
    semaphore: the mixer writes a block of input, bumps `requestSeq` and posts the
    semaphore; the helper processes it and publishes `doneSeq`. The mixer never
    blocks on the helper: it collects the output one block later, waiting at most a
    bounded time for it (see SandboxedPlugin). */

inline constexpr std::uint32_t kMagic = 0x53444378; // "SDCx"
inline constexpr std::uint32_t kVersion = 1;
inline constexpr int kMaxBlock = 8192;  // samples per exchanged block
inline constexpr int kMaxParams = 4096; // parameters mirrored through shared memory
inline constexpr const char* kCommandLinePrefix = "--sidecar-plugin-sandbox:";
inline constexpr juce::uint32 kPipeMagic = 0x5344436b;
inline constexpr int kPipeTimeoutMs = 10000; // also: silence after which the other side counts as gone

static_assert (std::atomic<std::uint32_t>::is_always_lock_free && std::atomic<float>::is_always_lock_free,
               "shared-memory signalling needs address-free atomics");

/** One mirrored parameter: the value and a serial bumped after each write. */
struct ParamSlot
{
    std::atomic<float> value;
    std::atomic<std::uint32_t> serial;
};

/** The shared memory block (created by Sidecar, opened by the helper). */
struct SharedBlock
{
    std::uint32_t magic, version;

    // Mixer -> helper.
    alignas (64) std::atomic<std::uint32_t> requestSeq;
    std::atomic<std::int32_t> requestSamples;
    std::atomic<std::int32_t> quit;
    std::atomic<std::uint32_t> toHelperSerial; // bumped after any toHelper[] write

    // Helper -> mixer.
    alignas (64) std::atomic<std::uint32_t> doneSeq;
    std::atomic<std::int32_t> latency;           // the plugin's own latency in samples
    std::atomic<std::uint32_t> fromHelperSerial; // bumped after any fromHelper[] write
    std::atomic<std::uint32_t> heartbeat;        // bumped by the helper's processing thread

    alignas (64) float input[2][kMaxBlock];
    float output[2][kMaxBlock];

    ParamSlot toHelper[kMaxParams];   // values set from Sidecar (automation, generic editor)
    ParamSlot fromHelper[kMaxParams]; // values changed in the plugin (its editor, its state)
};

/** Control messages (first int32 of every pipe message, then an int32 request id). */
enum class Msg : std::int32_t
{
    // Requests from Sidecar (each answered with a Reply carrying the same id).
    load = 1,     // shm name, semaphore name, description XML, sample rate, block size
    prepare,      // sample rate, block size
    getState,     //
    setState,     // state bytes
    paramText,    // index, value          -> text
    paramFromText, // index, text          -> value
    setProgram,   // index
    openEditor,   // window title
    closeEditor,  //
    quit,         //

    reply = 100,  // ok flag, then per-request payload
    editorClosed, // the user closed the helper's editor window
    ping          // every second, both ways
};

/** The control pipe: an InterprocessConnection that pings the other side every
    second from its own thread (so a busy message thread doesn't look like a dead
    process) and reports the connection lost after kPipeTimeoutMs of silence. */
class PingingConnection : public juce::InterprocessConnection,
                          private juce::Thread
{
public:
    PingingConnection() : juce::InterprocessConnection (false, kPipeMagic), juce::Thread ("Sidecar sandbox ping") {}

    /** Derived classes call this first in their destructor. */
    void shutdown()
    {
        stopThread (2000);
        disconnect (2000, Notify::no);
    }

    void startPinging()
    {
        lastReceived = juce::Time::getMillisecondCounter();
        startThread (juce::Thread::Priority::low);
    }

    bool isLost() const noexcept { return lost.load(); }

protected:
    /** A message other than a ping (connection thread). */
    virtual void handleMessage (const juce::MemoryBlock&) = 0;
    /** Called once, from the connection or ping thread. */
    virtual void handleLost() = 0;

private:
    void connectionMade() override {}
    void connectionLost() override { markLost(); }

    void messageReceived (const juce::MemoryBlock& message) override
    {
        lastReceived = juce::Time::getMillisecondCounter();
        if (message.getSize() >= 4 && juce::ByteOrder::littleEndianInt (message.getData()) == (juce::uint32) Msg::ping)
            return;
        handleMessage (message);
    }

    void run() override
    {
        juce::MemoryOutputStream ping;
        ping.writeInt ((int) Msg::ping);
        ping.writeInt (0);

        while (! threadShouldExit())
        {
            if (juce::Time::getMillisecondCounter() - lastReceived.load() > (juce::uint32) kPipeTimeoutMs
                || ! sendMessage (ping.getMemoryBlock()))
            {
                markLost();
                return;
            }
            wait (1000);
        }
    }

    void markLost()
    {
        if (! lost.exchange (true))
            handleLost();
    }

    std::atomic<juce::uint32> lastReceived { 0 };
    std::atomic<bool> lost { false };
};

//==============================================================================
/** A named shared memory block (POSIX shm_open / Windows file mapping). */
class SharedMemory
{
public:
    SharedMemory() = default;
    ~SharedMemory();

    /** Creates (owner) or opens a block of `size` bytes, zero-filled on creation. */
    bool create (const juce::String& name, size_t size);
    bool open (const juce::String& name, size_t size);
    void close();

    void* getData() const noexcept { return data; }

private:
    void* data = nullptr;
    size_t size = 0;
    juce::String name;
    bool owner = false;
#if JUCE_WINDOWS
    void* handle = nullptr;
#endif

    JUCE_DECLARE_NON_COPYABLE (SharedMemory)
};

/** A named counting semaphore. post() never blocks (safe on the audio thread). */
class NamedSemaphore
{
public:
    NamedSemaphore() = default;
    ~NamedSemaphore();

    bool create (const juce::String& name);
    bool open (const juce::String& name);
    void close();

    void post() noexcept;
    /** Blocks until posted (helper side only). */
    void wait() noexcept;

private:
    void* handle = nullptr;
    juce::String name;
    bool owner = false;

    JUCE_DECLARE_NON_COPYABLE (NamedSemaphore)
};

/** A short unique name for the shared objects ("/sdc..." on POSIX: macOS allows 31 chars). */
juce::String makeUniqueName();

/** This process's id (sent to Sidecar so it can let the helper's windows take focus). */
int currentProcessId();

/** Windows: lets process `pid` bring its windows to the front. No-op elsewhere. */
void allowProcessToTakeFocus (int pid);

} // namespace livedaw::sandbox
