// SidecarPluginHost: the plugin sandbox helper. Sidecar starts one per sandboxed
// plugin (see engine/SandboxedPlugin.h); it hosts the plugin, processes the blocks
// Sidecar hands it through shared memory and opens the plugin's editor in its own
// window. If the plugin crashes or hangs, only this process is lost.

#include "sandbox/SandboxShared.h"
#include "sandbox/TestPlugins.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>
#include <mutex>

namespace livedaw::sandbox
{

namespace
{
    void configureForStereo (juce::AudioPluginInstance& plugin)
    {
        auto layout = plugin.getBusesLayout();
        if (layout.inputBuses.size() > 0)
            layout.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();
        if (layout.outputBuses.size() > 0)
            layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
        if (plugin.checkBusesLayoutSupported (layout))
            plugin.setBusesLayout (layout);
    }

    void writeBlock (juce::MemoryOutputStream& out, const juce::MemoryBlock& block)
    {
        out.writeInt ((int) block.getSize());
        out.write (block.getData(), block.getSize());
    }

    juce::MemoryBlock readBlock (juce::MemoryInputStream& in)
    {
        juce::MemoryBlock block;
        const auto size = juce::jlimit (0, (int) in.getNumBytesRemaining(), in.readInt());
        in.readIntoMemoryBlock (block, size);
        return block;
    }
} // namespace

//==============================================================================
class SandboxHost final : private PingingConnection,
                          private juce::Timer
{
public:
    SandboxHost()
    {
        juce::addDefaultFormatsToManager (formats);
        if (TestPluginFormat::isEnabledByEnvironment())
            formats.addFormat (std::make_unique<TestPluginFormat>());
    }

    ~SandboxHost() override
    {
        shutdown();
        stopTimer();
        stopProcessing();
        editorWindow.reset();
        plugin.reset();
    }

    bool connect (const juce::String& pipeName)
    {
        if (! connectToPipe (pipeName, kPipeTimeoutMs))
            return false;
        startPinging();
        return true;
    }

private:
    //==============================================================================
    // Control messages arrive on the connection thread; the plugin is driven from
    // the message thread (as hosts do), apart from the audio.
    void handleLost() override
    {
        // Sidecar quit, died or gave up on us: leave at once, whatever the plugin is doing.
        juce::Process::terminate();
    }

    void handleMessage (const juce::MemoryBlock& message) override
    {
        juce::MessageManager::callAsync ([this, message] { handle (message); });
    }

    void reply (int id, bool ok, const std::function<void (juce::MemoryOutputStream&)>& payload = {})
    {
        juce::MemoryOutputStream out;
        out.writeInt ((int) Msg::reply);
        out.writeInt (id);
        out.writeBool (ok);
        if (payload)
            payload (out);
        sendMessage (out.getMemoryBlock());
    }

    void replyError (int id, const juce::String& error)
    {
        reply (id, false, [&] (auto& out) { out.writeString (error); });
    }

    void handle (const juce::MemoryBlock& message)
    {
        juce::MemoryInputStream in (message, false);
        const auto type = (Msg) in.readInt();
        const int id = in.readInt();

        if (type != Msg::load && type != Msg::quit && plugin == nullptr)
            return replyError (id, "no plugin loaded");

        switch (type)
        {
            case Msg::load:          return load (id, in);
            case Msg::prepare:       return prepare (id, in);
            case Msg::getState:      return getState (id);
            case Msg::setState:      return setState (id, in);
            case Msg::paramText:     return paramText (id, in);
            case Msg::paramFromText: return paramFromText (id, in);
            case Msg::setProgram:    plugin->setCurrentProgram (in.readInt()); return reply (id, true);
            case Msg::openEditor:    return openEditor (id, in.readString());
            case Msg::closeEditor:   editorWindow.reset(); return reply (id, true);
            case Msg::quit:          return quit (id);
            case Msg::reply:
            case Msg::editorClosed:
            case Msg::ping:          break;
        }
        replyError (id, "unknown request");
    }

    //==============================================================================
    void load (int id, juce::MemoryInputStream& in)
    {
        const auto shmName = in.readString();
        const auto semName = in.readString();
        const auto descXml = in.readString();
        const double sampleRate = in.readDouble();
        const int blockSize = juce::jlimit (1, kMaxBlock, in.readInt());

        if (plugin != nullptr)
            return replyError (id, "already loaded");
        if (! memory.open (shmName, sizeof (SharedBlock)) || ! semaphore.open (semName))
            return replyError (id, "could not open the shared audio buffer");

        shared = static_cast<SharedBlock*> (memory.getData());
        if (shared->magic != kMagic || shared->version != kVersion)
            return replyError (id, "Sidecar and its plugin host are different versions");

        juce::PluginDescription desc;
        const auto xml = juce::parseXML (descXml);
        if (xml == nullptr || ! desc.loadFromXml (*xml))
            return replyError (id, "invalid plugin description");

        juce::String error;
        plugin = formats.createPluginInstance (desc, sampleRate, blockSize, error);
        if (plugin == nullptr)
            return replyError (id, error.isNotEmpty() ? error : juce::String ("could not be created"));

        configureForStereo (*plugin);
        prepareLocked (sampleRate, blockSize);

        const auto& all = plugin->getParameters();
        params.clear();
        for (int i = 0; i < juce::jmin (all.size(), kMaxParams); ++i)
            params.push_back (all[i]);
        seen.assign (params.size(), 0);
        published.assign (params.size(), 0.0f);
        {
            const std::lock_guard<std::mutex> lock (paramLock);
            publishParams (true);
        }

        reply (id, true, [this] (juce::MemoryOutputStream& out)
        {
            out.writeInt (currentProcessId());
            out.writeString (plugin->getName());
            out.writeInt (plugin->getLatencySamples());
            out.writeDouble (plugin->getTailLengthSeconds());
            out.writeBool (plugin->hasEditor());
            const int numPrograms = juce::jmin (plugin->getNumPrograms(), 1024);
            out.writeInt (numPrograms);
            out.writeInt (plugin->getCurrentProgram());
            for (int i = 0; i < numPrograms; ++i)
                out.writeString (plugin->getProgramName (i));

            out.writeInt ((int) params.size());
            for (auto* p : params)
            {
                auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (p);
                out.writeString (hosted != nullptr ? hosted->getParameterID() : juce::String (p->getParameterIndex()));
                out.writeString (p->getName (256));
                out.writeString (p->getLabel());
                out.writeFloat (p->getDefaultValue());
                out.writeFloat (p->getValue());
                out.writeInt (p->getNumSteps());
                out.writeBool (p->isDiscrete());
                out.writeBool (p->isBoolean());
                out.writeBool (p->isAutomatable());
                out.writeBool (p->isMetaParameter());
                out.writeInt ((int) p->getCategory());
            }
        });

        startTimerHz (15);
        if (! audioThread.startRealtimeThread (juce::Thread::RealtimeOptions().withPriority (9)))
            audioThread.startThread (juce::Thread::Priority::highest);
    }

    void prepare (int id, juce::MemoryInputStream& in)
    {
        const double sampleRate = in.readDouble();
        const int blockSize = juce::jlimit (1, kMaxBlock, in.readInt());
        {
            const std::lock_guard<std::mutex> lock (processLock);
            plugin->releaseResources();
            prepareLocked (sampleRate, blockSize);
        }
        reply (id, true, [this] (auto& out) { out.writeInt (plugin->getLatencySamples()); });
    }

    void prepareLocked (double sampleRate, int blockSize)
    {
        plugin->setRateAndBufferSizeDetails (sampleRate, blockSize);
        plugin->prepareToPlay (sampleRate, blockSize);
        numChannels = juce::jmax (2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels());
        scratch.setSize (numChannels, kMaxBlock, false, true, false);
        midi.ensureSize (256);
        shared->latency.store (plugin->getLatencySamples(), std::memory_order_relaxed);
    }

    void getState (int id)
    {
        {
            const std::lock_guard<std::mutex> lock (paramLock);
            applyParams(); // values Sidecar set just before asking belong in the state
        }
        juce::MemoryBlock state;
        plugin->getStateInformation (state);
        reply (id, true, [&] (auto& out) { writeBlock (out, state); });
    }

    void setState (int id, juce::MemoryInputStream& in)
    {
        const auto state = readBlock (in);
        plugin->setStateInformation (state.getData(), (int) state.getSize());
        {
            const std::lock_guard<std::mutex> lock (paramLock);
            publishParams (false);
        }
        reply (id, true);
    }

    void paramText (int id, juce::MemoryInputStream& in)
    {
        const int index = in.readInt();
        const float value = in.readFloat();
        if (! juce::isPositiveAndBelow (index, (int) params.size()))
            return replyError (id, "bad parameter");
        const auto text = params[(size_t) index]->getText (value, 1024);
        reply (id, true, [&] (auto& out) { out.writeString (text); });
    }

    void paramFromText (int id, juce::MemoryInputStream& in)
    {
        const int index = in.readInt();
        const auto text = in.readString();
        if (! juce::isPositiveAndBelow (index, (int) params.size()))
            return replyError (id, "bad parameter");
        const float value = params[(size_t) index]->getValueForText (text);
        reply (id, true, [&] (auto& out) { out.writeFloat (value); });
    }

    void quit (int id)
    {
        reply (id, true);
        stopTimer();
        editorWindow.reset();
        if (! stopProcessing())
            juce::Process::terminate(); // the plugin is stuck in processBlock
        plugin.reset();
        juce::Process::terminate();
    }

    //==============================================================================
    // Parameters: Sidecar's changes come in through toHelper[], the plugin's own
    // (its editor, its state) go out through fromHelper[].
    void applyParams()
    {
        if (shared == nullptr)
            return;
        const auto serial = shared->toHelperSerial.load (std::memory_order_acquire);
        if (serial == seenSerial)
            return;
        seenSerial = serial;

        for (size_t i = 0; i < params.size(); ++i)
        {
            const auto s = shared->toHelper[i].serial.load (std::memory_order_acquire);
            if (s == seen[i])
                continue;
            seen[i] = s;
            const float v = shared->toHelper[i].value.load (std::memory_order_relaxed);
            params[i]->setValue (v);
            published[i] = v; // not echoed back
        }
    }

    void publishParams (bool all)
    {
        bool changed = false;
        for (size_t i = 0; i < params.size(); ++i)
        {
            const float v = params[i]->getValue();
            if (! all && v == published[i])
                continue;
            published[i] = v;
            shared->fromHelper[i].value.store (v, std::memory_order_relaxed);
            shared->fromHelper[i].serial.fetch_add (1, std::memory_order_release);
            changed = true;
        }
        if (changed)
            shared->fromHelperSerial.fetch_add (1, std::memory_order_release);
    }

    void timerCallback() override
    {
        // Sidecar's changes also apply while no audio flows (the slot isn't in use).
        const std::lock_guard<std::mutex> lock (paramLock);
        applyParams();
        publishParams (false);
        if (plugin != nullptr)
            shared->latency.store (plugin->getLatencySamples(), std::memory_order_relaxed);
    }

    //==============================================================================
    // Audio: one block per request.
    struct AudioThread final : public juce::Thread
    {
        explicit AudioThread (SandboxHost& o) : juce::Thread ("Sidecar sandbox audio"), owner (o) {}
        void run() override { owner.processRequests(); }
        SandboxHost& owner;
    };

    void processRequests()
    {
        juce::uint32 last = shared->doneSeq.load (std::memory_order_relaxed);

        while (! audioThread.threadShouldExit())
        {
            semaphore.wait();
            if (shared->quit.load (std::memory_order_acquire) != 0 || audioThread.threadShouldExit())
                break;

            const auto seq = shared->requestSeq.load (std::memory_order_acquire);
            if (seq == last)
                continue;
            last = seq;
            const int n = juce::jlimit (0, kMaxBlock, (int) shared->requestSamples.load (std::memory_order_relaxed));

            if (paramLock.try_lock())
            {
                applyParams();
                paramLock.unlock();
            }

            {
                const std::lock_guard<std::mutex> lock (processLock);
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    if (ch < 2)
                        scratch.copyFrom (ch, 0, shared->input[ch], n);
                    else
                        scratch.clear (ch, 0, n);
                }

                juce::AudioBuffer<float> view (scratch.getArrayOfWritePointers(), numChannels, n);
                midi.clear();
                try
                {
                    plugin->processBlock (view, midi);
                }
                catch (...)
                {
                    juce::Process::terminate(); // reported to Sidecar as a crash
                }

                for (int ch = 0; ch < 2; ++ch)
                    juce::FloatVectorOperations::copy (shared->output[ch], scratch.getReadPointer (ch), n);
            }

            shared->heartbeat.fetch_add (1, std::memory_order_relaxed);
            shared->doneSeq.store (seq, std::memory_order_release);
        }
    }

    /** False if the processing thread is stuck. */
    bool stopProcessing()
    {
        if (! audioThread.isThreadRunning())
            return true;
        audioThread.signalThreadShouldExit();
        if (shared != nullptr)
            shared->quit.store (1, std::memory_order_release);
        semaphore.post();
        return audioThread.stopThread (1000);
    }

    //==============================================================================
    class EditorWindow final : public juce::DocumentWindow
    {
    public:
        EditorWindow (juce::AudioProcessor& p, const juce::String& title, std::function<void()> closed)
            : juce::DocumentWindow (title, juce::Colour (0xff1c1c1c), closeButton | minimiseButton), onClosed (std::move (closed))
        {
            setUsingNativeTitleBar (true);
            auto* editor = p.hasEditor() ? p.createEditorAndMakeActive() : nullptr;
            if (editor == nullptr)
                editor = new juce::GenericAudioProcessorEditor (p);
            setContentOwned (editor, true);
            setResizable (editor->isResizable(), false);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        ~EditorWindow() override { clearContentComponent(); }

        void closeButtonPressed() override
        {
            juce::MessageManager::callAsync (onClosed);
        }

    private:
        std::function<void()> onClosed;
    };

    void openEditor (int id, const juce::String& title)
    {
        if (juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() == nullptr)
            return replyError (id, "no display is available to the plugin host");

        if (editorWindow == nullptr)
        {
            editorWindow = std::make_unique<EditorWindow> (*plugin, title, [this]
            {
                editorWindow.reset();
                juce::MemoryOutputStream out;
                out.writeInt ((int) Msg::editorClosed);
                out.writeInt (0);
                sendMessage (out.getMemoryBlock());
            });
        }
        juce::Process::makeForegroundProcess();
        editorWindow->setMinimised (false);
        editorWindow->toFront (true);
        reply (id, true);
    }

    //==============================================================================
    juce::AudioPluginFormatManager formats;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    SharedMemory memory;
    NamedSemaphore semaphore;
    SharedBlock* shared = nullptr;

    std::mutex processLock; // the audio thread vs. prepare (Sidecar's engine is stopped then)
    std::mutex paramLock;   // applyParams / publishParams: the audio thread only tries it
    std::vector<juce::AudioProcessorParameter*> params;
    std::vector<juce::uint32> seen;
    std::vector<float> published;
    juce::uint32 seenSerial = 0;

    juce::AudioBuffer<float> scratch;
    juce::MidiBuffer midi;
    int numChannels = 2;

    std::unique_ptr<EditorWindow> editorWindow;
    AudioThread audioThread { *this };
};

//==============================================================================
class HostApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "SidecarPluginHost"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String& commandLine) override
    {
        const auto pipeName = commandLine.fromFirstOccurrenceOf (kCommandLinePrefix, false, false)
                                  .upToFirstOccurrenceOf (" ", false, false)
                                  .trim();
        host = std::make_unique<SandboxHost>();
        if (pipeName.isEmpty() || ! host->connect (pipeName))
        {
            std::cerr << "SidecarPluginHost is started by Sidecar to run a plugin in a sandbox." << std::endl;
            host.reset();
            setApplicationReturnValue (2);
            quit();
        }
    }

    void shutdown() override { host.reset(); }
    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted (const juce::String&) override {}

private:
    std::unique_ptr<SandboxHost> host;
};

} // namespace livedaw::sandbox

START_JUCE_APPLICATION (livedaw::sandbox::HostApplication)
