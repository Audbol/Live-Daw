#include "engine/SandboxedPlugin.h"
#include "sandbox/SandboxShared.h"

#include <condition_variable>
#include <map>
#include <optional>
#include <thread>

namespace livedaw
{

using sandbox::Msg;

//==============================================================================
struct SandboxedPlugin::Reply
{
    bool ok = false;
    juce::MemoryBlock data; // the payload after the ok flag
    juce::String error;
};

/** The control pipe to the helper: synchronous requests (message thread) and
    asynchronous ones (callback on the connection thread). */
class SandboxedPlugin::Connection final : public sandbox::PingingConnection
{
public:
    using Payload = std::function<void (juce::MemoryOutputStream&)>;
    using Callback = std::function<void (const Reply&)>;

    explicit Connection (SandboxedPlugin& o) : owner (o) {}
    ~Connection() override { shutdown(); }

    /** Waits for the reply. Fails on timeout, or as soon as `alive` says the helper is gone. */
    Reply request (Msg type, const Payload& payload, int timeoutMs, const std::function<bool()>& alive)
    {
        const int id = nextId++;
        {
            const std::lock_guard<std::mutex> l (lock);
            waiting[id] = std::nullopt;
        }

        if (! send (type, id, payload))
            return abandon (id, "the plugin host isn't responding");

        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        std::unique_lock<std::mutex> l (lock);
        for (;;)
        {
            if (auto it = waiting.find (id); it != waiting.end() && it->second.has_value())
            {
                auto reply = std::move (*it->second);
                waiting.erase (it);
                return reply;
            }
            if (isLost())
                return abandonLocked (id, "the plugin host closed");
            if (juce::Time::getMillisecondCounter() > deadline)
                return abandonLocked (id, "the plugin host didn't answer in time");

            l.unlock();
            const bool stillAlive = alive == nullptr || alive();
            l.lock();
            if (! stillAlive)
                return abandonLocked (id, "the plugin host died");
            condition.wait_for (l, std::chrono::milliseconds (20));
        }
    }

    void requestAsync (Msg type, const Payload& payload, Callback callback)
    {
        const int id = nextId++;
        {
            const std::lock_guard<std::mutex> l (lock);
            callbacks[id] = std::move (callback);
        }
        if (! send (type, id, payload))
            failCallbacks();
    }

private:
    bool send (Msg type, int id, const Payload& payload)
    {
        juce::MemoryOutputStream out;
        out.writeInt ((int) type);
        out.writeInt (id);
        if (payload)
            payload (out);
        return sendMessage (out.getMemoryBlock());
    }

    Reply abandon (int id, const juce::String& error)
    {
        const std::lock_guard<std::mutex> l (lock);
        return abandonLocked (id, error);
    }

    Reply abandonLocked (int id, const juce::String& error)
    {
        waiting.erase (id); // a late reply is dropped
        Reply r;
        r.error = error;
        return r;
    }

    void handleMessage (const juce::MemoryBlock& message) override
    {
        juce::MemoryInputStream in (message, false);
        const auto type = (Msg) in.readInt();
        const int id = in.readInt();
        if (type != Msg::reply)
            return; // editorClosed: nothing to do

        Reply reply;
        reply.ok = in.readBool();
        in.readIntoMemoryBlock (reply.data);
        if (! reply.ok)
            reply.error = juce::MemoryInputStream (reply.data, false).readString();

        Callback callback;
        {
            const std::lock_guard<std::mutex> l (lock);
            if (auto it = waiting.find (id); it != waiting.end())
                it->second = std::move (reply);
            else if (auto cb = callbacks.find (id); cb != callbacks.end())
            {
                callback = std::move (cb->second);
                callbacks.erase (cb);
            }
        }
        condition.notify_all();
        if (callback)
            callback (reply);
    }

    void handleLost() override
    {
        owner.helperAlive = false;
        condition.notify_all();
        failCallbacks();
    }

    void failCallbacks()
    {
        std::map<int, Callback> failed;
        {
            const std::lock_guard<std::mutex> l (lock);
            failed.swap (callbacks);
        }
        Reply r;
        r.error = "the plugin host closed";
        for (auto& c : failed)
            c.second (r);
    }

    SandboxedPlugin& owner;
    std::atomic<int> nextId { 1 };
    std::mutex lock;
    std::condition_variable condition;
    std::map<int, std::optional<Reply>> waiting;
    std::map<int, Callback> callbacks;
};

//==============================================================================
struct SandboxedPlugin::Shared
{
    sandbox::SharedMemory memory;
    sandbox::NamedSemaphore semaphore;
    sandbox::SharedBlock* block = nullptr;
    juce::String name;
    std::vector<juce::uint32> seenFrom; // message thread: fromHelper serials already mirrored
    juce::uint32 seenFromSerial = 0;
};

//==============================================================================
/** A parameter of the sandboxed plugin. Values are cached here; changes go to the
    helper through shared memory (lock-free, any thread). */
class SandboxedPlugin::Param final : public juce::AudioPluginInstance::HostedParameter
{
public:
    Param (SandboxedPlugin& o, int i) : owner (o), index (i) {}

    void read (juce::MemoryInputStream& in)
    {
        id = in.readString();
        name = in.readString();
        label = in.readString();
        defaultValue = in.readFloat();
        value = in.readFloat();
        numSteps = in.readInt();
        discrete = in.readBool();
        boolean = in.readBool();
        automatable = in.readBool();
        meta = in.readBool();
        category = (Category) in.readInt();
    }

    juce::String getParameterID() const override { return id; }
    float getValue() const override { return value.load(); }

    void setValue (float newValue) override
    {
        value = newValue;
        lastLocalChangeMs = juce::Time::getMillisecondCounter();
        owner.sendParam (index, newValue);
    }

    float getDefaultValue() const override { return defaultValue; }
    juce::String getName (int maximumStringLength) const override { return name.substring (0, maximumStringLength); }
    juce::String getLabel() const override { return label; }
    int getNumSteps() const override { return numSteps; }
    bool isDiscrete() const override { return discrete; }
    bool isBoolean() const override { return boolean; }
    bool isAutomatable() const override { return automatable; }
    bool isMetaParameter() const override { return meta; }
    Category getCategory() const override { return category; }
    juce::String getText (float v, int maximumStringLength) const override { return owner.paramText (index, v).substring (0, maximumStringLength); }
    float getValueForText (const juce::String& text) const override { return owner.paramFromText (index, text, value.load()); }

    /** Message thread: a value changed in the helper. False if it must wait (a value
        set here a moment ago wins for now; the helper's latest value is taken later). */
    bool changedInHelper (float v)
    {
        if (juce::Time::getMillisecondCounter() - lastLocalChangeMs.load() < 300)
            return false;
        if (v != value.load())
        {
            value = v;
            sendValueChangedMessageToListeners (v);
        }
        return true;
    }

    // Message thread: getText() cache.
    float cachedTextValue = -1.0f;
    juce::String cachedText;

private:
    SandboxedPlugin& owner;
    const int index;
    juce::String id, name, label;
    float defaultValue = 0.0f;
    std::atomic<float> value { 0.0f };
    std::atomic<juce::uint32> lastLocalChangeMs { 0 };
    int numSteps = 0x7fffffff;
    bool discrete = false, boolean = false, automatable = true, meta = false;
    Category category = genericParameter;
};

//==============================================================================
/** The editor Sidecar shows for a sandboxed plugin: a bar with the sandbox state
    and a button for the plugin's own editor (a window of the helper process), and
    generic controls for the parameters. */
class SandboxedPlugin::Editor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit Editor (SandboxedPlugin& p) : juce::AudioProcessorEditor (p), owner (p), generic (p)
    {
        showWindow.onClick = [this] { open(); };
        addAndMakeVisible (showWindow);
        viewport.setViewedComponent (&generic, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);
        setSize (juce::jmax (520, generic.getWidth() + viewport.getScrollBarThickness()), kBar + juce::jmin (generic.getHeight(), 520));
        startTimerHz (4);
        open();
    }

    ~Editor() override { owner.closeEditorWindow(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1c1c1c));
        auto bar = getLocalBounds().removeFromTop (kBar).reduced (10, 6);
        bar.removeFromRight (showWindow.getWidth() + 10);
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.drawText ("RUNNING IN THE PLUGIN SANDBOX (EXPERIMENTAL)", bar.removeFromTop (20), juce::Justification::centredLeft);

        juce::String info;
        switch (owner.getStatus())
        {
            case Status::running:    info = editorError.isNotEmpty() ? "The plugin's editor couldn't open: " + editorError
                                                                     : juce::String ("The plugin's own editor opens in a separate window."); break;
            case Status::faulted:    info = "The plugin stopped responding; recovering it."; break;
            case Status::restarting: info = "Restarting the plugin's sandbox..."; break;
            case Status::stopped:    info = "The plugin's sandbox has stopped. Reload the plugin to try again."; break;
        }
        g.setColour (owner.getStatus() == Status::running && editorError.isEmpty() ? juce::Colour (0xffb0b0b0) : juce::Colour (0xffffb040));
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText (info, bar, juce::Justification::topLeft, 2);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto bar = r.removeFromTop (kBar).reduced (10, 12);
        showWindow.setBounds (bar.removeFromRight (170));
        viewport.setBounds (r);
        generic.setSize (r.getWidth() - viewport.getScrollBarThickness(), generic.getHeight());
    }

private:
    static constexpr int kBar = 64;

    void open()
    {
        owner.openEditorWindow (owner.getName() + " - Sidecar plugin sandbox",
                                [safe = juce::Component::SafePointer<Editor> (this)] (const juce::String& error)
                                {
                                    if (safe != nullptr)
                                    {
                                        safe->editorError = error;
                                        safe->repaint();
                                    }
                                });
    }

    void timerCallback() override { repaint (getLocalBounds().removeFromTop (kBar)); }

    SandboxedPlugin& owner;
    juce::GenericAudioProcessorEditor generic;
    juce::Viewport viewport;
    juce::TextButton showWindow { "SHOW PLUGIN WINDOW" };
    juce::String editorError;
};

//==============================================================================
namespace
{
    void writeBlock (juce::MemoryOutputStream& out, const void* data, size_t size)
    {
        out.writeInt ((int) size);
        out.write (data, size);
    }

    juce::MemoryBlock readBlock (juce::MemoryInputStream& in)
    {
        juce::MemoryBlock block;
        const auto size = juce::jlimit (0, (int) in.getNumBytesRemaining(), in.readInt());
        in.readIntoMemoryBlock (block, size);
        return block;
    }

    juce::int64 nowMs() { return juce::Time::currentTimeMillis(); }
} // namespace

juce::File SandboxedPlugin::findHelper()
{
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
#if JUCE_MAC
    // Sidecar.app/Contents/MacOS/Sidecar -> Sidecar.app/Contents/Helpers/SidecarPluginHost.app
    const auto bundled = exe.getParentDirectory().getSiblingFile ("Helpers").getChildFile ("SidecarPluginHost.app/Contents/MacOS/SidecarPluginHost");
    if (bundled.existsAsFile())
        return bundled;
    return exe.getParentDirectory().getChildFile ("SidecarPluginHost");
#elif JUCE_WINDOWS
    return exe.getSiblingFile ("SidecarPluginHost.exe");
#else
    return exe.getSiblingFile ("SidecarPluginHost");
#endif
}

SandboxedPlugin::SandboxedPlugin (const juce::PluginDescription& desc, std::shared_ptr<const SandboxOptions> opts)
    : juce::AudioPluginInstance (BusesProperties()
                                     .withInput ("In", juce::AudioChannelSet::stereo())
                                     .withOutput ("Out", juce::AudioChannelSet::stereo())),
      description (desc),
      options (std::move (opts)),
      pluginName (desc.name),
      shared (std::make_unique<Shared>())
{
    ticksPerSecond = (double) juce::Time::getHighResolutionTicksPerSecond();
}

std::unique_ptr<SandboxedPlugin> SandboxedPlugin::create (const juce::PluginDescription& desc, double sampleRate, int blockSize,
                                                          std::shared_ptr<const SandboxOptions> options, juce::String& error)
{
    jassert (options != nullptr);
    std::unique_ptr<SandboxedPlugin> p (new SandboxedPlugin (desc, std::move (options)));

    // The shared audio block and its semaphore live as long as the plugin, across helper restarts.
    auto& s = *p->shared;
    for (int attempt = 0; attempt < 4 && s.block == nullptr; ++attempt)
    {
        s.name = sandbox::makeUniqueName();
        if (s.memory.create (s.name, sizeof (sandbox::SharedBlock)) && s.semaphore.create (s.name))
            s.block = new (s.memory.getData()) sandbox::SharedBlock();
        else
            s.memory.close();
    }
    if (s.block == nullptr)
    {
        error = "couldn't create the plugin sandbox's shared memory";
        return nullptr;
    }
    s.block->magic = sandbox::kMagic;
    s.block->version = sandbox::kVersion;

    if (! p->launch (error) || ! p->loadInHelper (sampleRate, blockSize, true, error))
        return nullptr;

    p->status = Status::running;
    p->startTimerHz (20);
    return p;
}

SandboxedPlugin::~SandboxedPlugin()
{
    stopTimer();
    quiesce();

    if (process != nullptr && process->isRunning() && connection != nullptr && ! connection->isLost())
    {
        // Let the plugin shut down cleanly (it may save files), then make sure.
        shared->block->quit.store (1, std::memory_order_release);
        shared->semaphore.post();
        connection->request (Msg::quit, nullptr, 1000, nullptr);
        process->waitForProcessToFinish (1000);
    }
    killHelper();
}

//==============================================================================
// Helper process

bool SandboxedPlugin::launch (juce::String& error)
{
    killHelper();

    if (! options->helper.existsAsFile())
    {
        error = "the plugin sandbox helper is missing (" + options->helper.getFullPathName() + ")";
        return false;
    }

    const auto pipeName = "sdcp" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64());
    connection = std::make_unique<Connection> (*this);
    if (! connection->createPipe (pipeName, sandbox::kPipeTimeoutMs, true))
    {
        connection.reset();
        error = "couldn't create the plugin sandbox's control pipe";
        return false;
    }

    process = std::make_unique<juce::ChildProcess>();
    if (! process->start (juce::StringArray { options->helper.getFullPathName(), sandbox::kCommandLinePrefix + pipeName }, 0))
    {
        killHelper();
        error = "couldn't start the plugin sandbox helper";
        return false;
    }

    helperAlive = true;
    connection->startPinging();
    return true;
}

bool SandboxedPlugin::loadInHelper (double sampleRate, int newBlockSize, bool firstLoad, juce::String& error)
{
    const auto descXml = description.createXml()->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader());
    const int exchanged = juce::jlimit (1, sandbox::kMaxBlock, newBlockSize);

    const auto reply = connection->request (Msg::load, [&] (juce::MemoryOutputStream& out)
    {
        out.writeString (shared->name);
        out.writeString (shared->name);
        out.writeString (descXml);
        out.writeDouble (sampleRate);
        out.writeInt (exchanged);
    }, options->loadTimeoutMs, [this] { return isHelperRunning(); });

    if (! reply.ok)
    {
        error = reply.error;
        killHelper();
        return false;
    }

    juce::MemoryInputStream in (reply.data, false);
    helperPid = in.readInt();
    const auto name = in.readString();
    const int pluginLatency = in.readInt();
    const double tail = in.readDouble();
    in.readBool(); // hasEditor: the sandbox's editor is always there
    const int numPrograms = in.readInt();
    const int program = in.readInt();
    juce::StringArray programs;
    for (int i = 0; i < numPrograms; ++i)
        programs.add (in.readString());
    const int numParams = in.readInt();

    if (firstLoad)
    {
        pluginName = name;
        tailSeconds = tail;
        programNames = programs;
        currentProgram = program;
        for (int i = 0; i < numParams; ++i)
        {
            auto param = std::make_unique<Param> (*this, i);
            param->read (in);
            params.add (param.get());
            addHostedParameter (std::move (param));
        }
        // The values just read are the ones the helper published while loading:
        // mirrored already. Left unseen, they would overwrite a change made here
        // before the first sync (300 ms later), though the helper has it.
        shared->seenFromSerial = shared->block->fromHelperSerial.load (std::memory_order_acquire);
        shared->seenFrom.assign ((size_t) numParams, 0);
        for (int i = 0; i < numParams && i < sandbox::kMaxParams; ++i)
            shared->seenFrom[(size_t) i] = shared->block->fromHelper[i].serial.load (std::memory_order_acquire);
    }
    else if (numParams != params.size())
    {
        log ("\"" + pluginName + "\" came back with a different set of parameters", true);
    }

    currentSampleRate = sampleRate;
    blockSize = exchanged;
    fifo.setSize (2, 2 * blockSize, false, true, false);
    gapTicks = (juce::int64) (ticksPerSecond * juce::jmax (0.25, 8.0 * blockSize / sampleRate));
    setRateAndBufferSizeDetails (sampleRate, newBlockSize);
    setLatencySamples (pluginLatency + blockSize);
    resetPipeline();
    return true;
}

bool SandboxedPlugin::isHelperRunning() const
{
    return process != nullptr && helperAlive.load() && process->isRunning();
}

void SandboxedPlugin::killHelper()
{
    helperAlive = false;
    if (process != nullptr)
    {
        if (process->isRunning())
            process->kill();
        process->waitForProcessToFinish (2000);
        process.reset();
    }
    connection.reset();
    helperPid = 0;
}

void SandboxedPlugin::quiesce()
{
    // After this the audio thread can't be inside processChunk (it checks status
    // after raising inProcess; both seq_cst), and won't enter it until running again.
    if (status.load() == Status::running)
        status = Status::faulted;
    while (inProcess.load())
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
}

void SandboxedPlugin::resetPipeline()
{
    // Message thread, audio quiesced (or not started).
    auto& block = *shared->block;
    block.quit.store (0);
    seq = block.requestSeq.load();
    pending = 0;
    fifo.clear();
    fifoCount = blockSize; // the added block of latency
    lastCallTicks = 0;
    faultReason = 0;
}

void SandboxedPlugin::log (const juce::String& message, bool isError) const
{
    if (options->onEvent)
        options->onEvent ("Plugin sandbox: " + message, isError);
}

void SandboxedPlugin::restart (const juce::String& reason)
{
    status = Status::restarting;
    quiesce();
    killHelper();

    const auto now = nowMs();
    restartTimes.removeIf ([now] (juce::int64 t) { return now - t > 60000; });
    if (restartTimes.size() >= options->maxRestarts)
    {
        status = Status::stopped;
        log ("\"" + pluginName + "\" " + reason + " again; it failed too often and stays off. Reload it to try again.", true);
        return;
    }
    restartTimes.add (now);

    juce::MemoryBlock state;
    {
        const std::lock_guard<std::mutex> l (stateLock);
        state = cachedState;
    }

    // A fresh start: Sidecar's parameter writes are carried by the restored state.
    auto& block = *shared->block;
    for (auto& slot : block.toHelper)
        slot.serial.store (0, std::memory_order_relaxed);
    block.toHelperSerial.store (0, std::memory_order_release);
    block.doneSeq.store (0);
    block.requestSeq.store (0);

    juce::String error;
    if (! launch (error) || ! loadInHelper (currentSampleRate, getBlockSize(), false, error))
    {
        status = Status::stopped;
        log ("\"" + pluginName + "\" " + reason + " and couldn't be restarted: " + error, true);
        return;
    }

    bool restored = true;
    if (! state.isEmpty())
        restored = connection->request (Msg::setState, [&] (auto& out) { writeBlock (out, state.getData(), state.getSize()); },
                                        options->requestTimeoutMs, [this] { return isHelperRunning(); }).ok;
    if (! restored)
        faultReason = 1; // supervise() retries
    status = restored ? Status::running : Status::faulted;

    log ("\"" + pluginName + "\" " + reason + "; its sandbox was restarted with the last saved state. "
         "The FX slot stays bypassed until you clear the fault.", true);
}

void SandboxedPlugin::supervise()
{
    syncParamsFromHelper();

    const auto current = status.load();
    if (current == Status::stopped || current == Status::restarting)
        return;

    if (! isHelperRunning())
        return restart ("crashed");

    if (const int reason = faultReason.load(); current == Status::faulted && reason != 0)
    {
        // A block timed out. If the helper catches up, it was only late: carry on.
        // If it doesn't finish within hangTimeoutMs, it is stuck: kill it.
        auto& block = *shared->block;
        if (faultSinceMs == 0)
            faultSinceMs = nowMs();
        if (block.doneSeq.load (std::memory_order_acquire) == block.requestSeq.load())
        {
            faultSinceMs = 0;
            quiesce();
            resetPipeline();
            status = Status::running;
            log ("\"" + pluginName + "\" was too slow for a block; the FX slot is bypassed until you clear the fault.", false);
        }
        else if (nowMs() - faultSinceMs > options->hangTimeoutMs)
        {
            faultSinceMs = 0;
            restart ("stopped responding");
        }
        return;
    }

    // Keep a recent copy of the state for a restart.
    if (current == Status::running && nowMs() - lastSnapshotMs > 10000 && connection != nullptr)
    {
        lastSnapshotMs = nowMs();
        connection->requestAsync (Msg::getState, nullptr, [this] (const Reply& r)
        {
            if (! r.ok)
                return;
            juce::MemoryInputStream in (r.data, false);
            auto state = readBlock (in);
            const std::lock_guard<std::mutex> l (stateLock);
            cachedState = std::move (state);
        });
    }

    if (const int latency = shared->block->latency.load (std::memory_order_relaxed) + blockSize; latency != getLatencySamples())
        setLatencySamples (latency);
}

//==============================================================================
// AudioProcessor

void SandboxedPlugin::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    // Message thread, with the device stopped or before the slot is installed.
    const auto previous = status.load();
    if (previous == Status::stopped || previous == Status::restarting || connection == nullptr)
        return;

    quiesce();
    status = Status::restarting;
    currentSampleRate = sampleRate;
    const int exchanged = juce::jlimit (1, sandbox::kMaxBlock, maximumExpectedSamplesPerBlock);

    // A block may still be in the helper: give it a moment.
    auto& block = *shared->block;
    for (int i = 0; i < 200 && block.doneSeq.load() != block.requestSeq.load(); ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));

    const auto r = connection->request (Msg::prepare, [&] (auto& out)
    {
        out.writeDouble (sampleRate);
        out.writeInt (exchanged);
    }, options->requestTimeoutMs, [this] { return isHelperRunning(); });

    blockSize = exchanged;
    fifo.setSize (2, 2 * blockSize, false, true, false);
    gapTicks = (juce::int64) (ticksPerSecond * juce::jmax (0.25, 8.0 * blockSize / sampleRate));
    if (r.ok)
        setLatencySamples (juce::MemoryInputStream (r.data, false).readInt() + blockSize);
    const bool caughtUp = block.doneSeq.load() == block.requestSeq.load();
    resetPipeline();
    status = r.ok && caughtUp && previous == Status::running ? Status::running : Status::faulted;
    if (! r.ok || ! caughtUp)
        faultReason = 1; // supervise() recovers it
}

bool SandboxedPlugin::isBusesLayoutSupported (const BusesLayout& layout) const
{
    return layout.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
           && layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void SandboxedPlugin::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // Audio thread. Lock-free and allocation-free: waits only for a bounded time.
    // The message thread leaves the pipeline alone while `inProcess` is up (see quiesce()).
    struct Scope
    {
        explicit Scope (std::atomic<bool>& f) : flag (f) { flag.store (true); }
        ~Scope() { flag.store (false); }
        std::atomic<bool>& flag;
    } scope (inProcess);

    lastBlockFailed = status.load() != Status::running;
    const int numChannels = juce::jmin (2, buffer.getNumChannels());
    if (lastBlockFailed || numChannels == 0)
        return;

    auto* const* channels = buffer.getArrayOfWritePointers();
    for (int start = 0; start < buffer.getNumSamples() && ! lastBlockFailed; start += blockSize)
        lastBlockFailed = ! processChunk (channels, numChannels, start, juce::jmin (blockSize, buffer.getNumSamples() - start));
}

bool SandboxedPlugin::processChunk (float* const* channels, int numChannels, int start, int n) noexcept
{
    auto& block = *shared->block;
    const auto now = juce::Time::getHighResolutionTicks();

    if (pending > 0)
    {
        // Collect the previous block's output.
        while (block.doneSeq.load (std::memory_order_acquire) != seq)
        {
            const bool alive = helperAlive.load (std::memory_order_relaxed);
            if (! alive || juce::Time::getHighResolutionTicks() > pendingDeadline)
            {
                faultReason = alive ? 1 : 2;
                auto expected = Status::running; // the message thread may have moved on already
                status.compare_exchange_strong (expected, Status::faulted);
                return false;
            }
            std::this_thread::yield();
        }

        if (lastCallTicks != 0 && now - lastCallTicks > gapTicks)
        {
            // The slot sat idle (bypassed, unassigned): start over instead of
            // playing stale audio.
            fifo.clear();
            fifoCount = blockSize;
        }
        else
        {
            for (int ch = 0; ch < 2; ++ch)
                fifo.copyFrom (ch, fifoCount, block.output[ch], pending);
            fifoCount += pending;
        }
        pending = 0;
    }

    // Hand this block over.
    for (int ch = 0; ch < 2; ++ch)
        juce::FloatVectorOperations::copy (block.input[ch], channels[juce::jmin (ch, numChannels - 1)] + start, n);
    block.requestSamples.store (n, std::memory_order_relaxed);
    block.requestSeq.store (++seq, std::memory_order_release);
    shared->semaphore.post();
    pending = n;
    const double budget = (1.0 + options->waitFraction) * n / currentSampleRate + options->minWaitMs * 0.001;
    pendingDeadline = now + (juce::int64) (budget * ticksPerSecond);
    lastCallTicks = now;

    // Play the oldest output.
    for (int ch = 0; ch < numChannels; ++ch)
        juce::FloatVectorOperations::copy (channels[ch] + start, fifo.getReadPointer (ch), n);
    fifoCount -= n;
    for (int ch = 0; ch < 2; ++ch)
        std::memmove (fifo.getWritePointer (ch), fifo.getReadPointer (ch) + n, sizeof (float) * (size_t) fifoCount);
    return true;
}

//==============================================================================
// Parameters, programs, state

void SandboxedPlugin::sendParam (int index, float value) noexcept
{
    if (! juce::isPositiveAndBelow (index, sandbox::kMaxParams))
        return;
    auto& block = *shared->block;
    block.toHelper[index].value.store (value, std::memory_order_relaxed);
    block.toHelper[index].serial.fetch_add (1, std::memory_order_release);
    block.toHelperSerial.fetch_add (1, std::memory_order_release);
}

juce::String SandboxedPlugin::paramText (int index, float value)
{
    auto* p = params[index];
    const auto fallback = juce::String (value, 3);
    if (p == nullptr || status.load() != Status::running || connection == nullptr
        || ! juce::MessageManager::existsAndIsCurrentThread())
        return fallback;
    if (value == p->cachedTextValue)
        return p->cachedText;

    const auto r = connection->request (Msg::paramText, [&] (auto& out)
    {
        out.writeInt (index);
        out.writeFloat (value);
    }, 250, nullptr);
    if (! r.ok)
        return fallback;
    p->cachedTextValue = value;
    p->cachedText = juce::MemoryInputStream (r.data, false).readString();
    return p->cachedText;
}

float SandboxedPlugin::paramFromText (int index, const juce::String& text, float fallback)
{
    if (status.load() != Status::running || connection == nullptr || ! juce::MessageManager::existsAndIsCurrentThread())
        return fallback;
    const auto r = connection->request (Msg::paramFromText, [&] (auto& out)
    {
        out.writeInt (index);
        out.writeString (text);
    }, 250, nullptr);
    return r.ok ? juce::MemoryInputStream (r.data, false).readFloat() : fallback;
}

void SandboxedPlugin::syncParamsFromHelper()
{
    auto& s = *shared;
    const auto serial = s.block->fromHelperSerial.load (std::memory_order_acquire);
    if (serial == s.seenFromSerial)
        return;
    bool deferred = false;
    for (int i = 0; i < params.size() && i < (int) s.seenFrom.size(); ++i)
    {
        auto& slot = s.block->fromHelper[i];
        const auto ser = slot.serial.load (std::memory_order_acquire);
        if (ser == s.seenFrom[(size_t) i])
            continue;
        if (params[i]->changedInHelper (slot.value.load (std::memory_order_relaxed)))
            s.seenFrom[(size_t) i] = ser;
        else
            deferred = true; // looked at again on the next tick
    }
    if (! deferred)
        s.seenFromSerial = serial;
}

void SandboxedPlugin::setCurrentProgram (int index)
{
    currentProgram = index;
    if (connection != nullptr && status.load() == Status::running)
        connection->requestAsync (Msg::setProgram, [index] (auto& out) { out.writeInt (index); }, [] (const Reply&) {});
}

void SandboxedPlugin::getStateInformation (juce::MemoryBlock& dest)
{
    if (connection != nullptr && status.load() == Status::running)
    {
        const auto r = connection->request (Msg::getState, nullptr, options->requestTimeoutMs, [this] { return isHelperRunning(); });
        if (r.ok)
        {
            juce::MemoryInputStream in (r.data, false);
            auto state = readBlock (in);
            const std::lock_guard<std::mutex> l (stateLock);
            cachedState = state;
            dest = std::move (state);
            return;
        }
    }

    // The helper is gone or busy: the last state it gave us.
    const std::lock_guard<std::mutex> l (stateLock);
    dest = cachedState;
}

void SandboxedPlugin::setStateInformation (const void* data, int sizeInBytes)
{
    {
        const std::lock_guard<std::mutex> l (stateLock);
        cachedState.replaceAll (data, (size_t) juce::jmax (0, sizeInBytes));
    }
    if (connection != nullptr && status.load() == Status::running)
        connection->request (Msg::setState, [&] (auto& out) { writeBlock (out, data, (size_t) juce::jmax (0, sizeInBytes)); },
                             options->requestTimeoutMs, [this] { return isHelperRunning(); });
}

//==============================================================================
// Editor

juce::AudioProcessorEditor* SandboxedPlugin::createEditor()
{
    return new Editor (*this);
}

void SandboxedPlugin::openEditorWindow (const juce::String& title, std::function<void (const juce::String&)> done)
{
    if (connection == nullptr || status.load() != Status::running)
    {
        if (done)
            done ("the plugin's sandbox isn't running");
        return;
    }

    sandbox::allowProcessToTakeFocus (helperPid);
    connection->requestAsync (Msg::openEditor, [&] (auto& out) { out.writeString (title); },
                              [done = std::move (done)] (const Reply& r)
                              {
                                  juce::MessageManager::callAsync ([done, error = r.ok ? juce::String() : r.error]
                                  {
                                      if (done)
                                          done (error);
                                  });
                              });
}

void SandboxedPlugin::closeEditorWindow()
{
    if (connection != nullptr && status.load() == Status::running)
        connection->requestAsync (Msg::closeEditor, nullptr, [] (const Reply&) {});
}

} // namespace livedaw
