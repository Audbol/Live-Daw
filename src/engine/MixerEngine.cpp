#include "engine/MixerEngine.h"

namespace livedaw
{

/** A realtime thread lent to the engine's worker pool. ld_engine_run_worker
    returns only once ld_engine_shutdown_workers has been called. */
class MixerEngine::Worker final : public juce::Thread
{
public:
    Worker (ld::Engine* e, int i)
        : juce::Thread ("LiveDAW DSP worker " + juce::String (i)), engine (e), index (i)
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

    void run() override { ld::ld_engine_run_worker (engine, index); }

private:
    ld::Engine* engine;
    int index;
};

//==============================================================================
static ld::Engine* createEngine (int numWorkerThreads)
{
    if (const char* mismatch = ld::verifyLayout())
    {
        // The C++ header and the Rust library disagree: sharing parameter blocks
        // would corrupt memory, so refuse to run at all.
        juce::Logger::writeToLog (juce::String ("Rust engine layout mismatch: ") + mismatch);
        std::abort();
    }

    auto* e = ld::ld_engine_create (numWorkerThreads);
    if (e == nullptr)
        std::abort();
    return e;
}

MixerEngine::MixerEngine (int numWorkerThreads)
    : engine (createEngine (numWorkerThreads)),
      controls (*ld::ld_engine_controls (engine)),
      soloAfl (controls.soloAfl),
      busSoloAfl (controls.busSoloAfl),
      mainSoloAfl (controls.mainSoloAfl),
      matrixSoloAfl (controls.matrixSoloAfl),
      monitorLevelDb (controls.monitorLevelDb),
      monitorDim (controls.monitorDim),
      monitorDimDb (controls.monitorDimDb),
      monitorMute (controls.monitorMute),
      monitorMono (controls.monitorMono),
      monitorSwap (controls.monitorSwap),
      monitorOutputLeft (controls.monitorOutputLeft),
      monitorOutputRight (controls.monitorOutputRight),
      monitorPeak (controls.monitorPeak),
      anySoloActive (controls.anySoloActive),
      muteGroups (controls.muteGroups),
      automixEnable (controls.automixEnable),
      inputSelectGlobal (controls.inputSelectGlobal),
      globalAlt (controls.globalAlt),
      dcaMuteGroups (controls.dcaMuteGroups),
      mainLink (controls.mainLink),
      startupMainMute (controls.startupMainMute),
      soloMode (controls.soloMode),
      soloBus (controls.soloBus),
      muteOverride (controls.muteOverride),
      sipActive (controls.sipActive),
      talk (controls.talk),
      speakers (controls.speakers),
      phones (controls.phones),
      monitor (controls.monitor),
      outputProtection (controls.outputProtection),
      outputCeilingDb (controls.outputCeilingDb),
      outputClipEvents (controls.outputClipEvents),
      garbage (engine),
      fx (engine)
{
    sourceTable = ld::ld_engine_sources (engine, &numSources);
    syncStrips();
    startWorkers (getSampleRate(), getBlockSize());
    startTimerHz (10);
}

MixerEngine::~MixerEngine()
{
    wing.reset(); // stops the remote servers while the engine is intact
    stopTimer();
    // The device manager must have removed this callback before destruction.
    ld::ld_engine_shutdown_workers (engine);
    for (auto& w : workers)
        w->stopThread (2000);
    workers.clear();

    for (auto& list : lists)
        list.clear();
    recorder.reset();
    usbRecorder.reset();
    ld::ld_engine_destroy (engine); // releases the plugins still loaded; a recording is finalized
}

void MixerEngine::startWorkers (double sampleRate, int blockSize)
{
    const int n = ld::ld_engine_num_workers (engine);
    for (int i = 0; i < n; ++i)
    {
        auto& w = workers.emplace_back (std::make_unique<Worker> (engine, i));
        w->start (sampleRate, blockSize);
    }
}

//==============================================================================
void MixerEngine::syncStrips()
{
    JUCE_ASSERT_MESSAGE_THREAD

    // Keep the wrapper of every strip that still exists, so Strip* held by the UI
    // stay valid; strips removed from the engine lose theirs. (A removed strip is
    // only freed by a later collect, so its address can't be reused before this.)
    std::unordered_map<ld::Strip*, std::unique_ptr<Strip>> old;
    for (auto& list : lists)
        for (auto& s : list)
            old.emplace (s->getHandle(), std::move (s));

    for (int k = 0; k < kNumStripKinds; ++k)
    {
        auto& list = lists[(size_t) k];
        const int n = ld::ld_engine_num_strips (engine, k);
        list.clear();
        list.reserve ((size_t) n);

        for (int i = 0; i < n; ++i)
        {
            auto* handle = ld::ld_engine_strip_at (engine, k, i);
            jassert (handle != nullptr);
            auto it = old.find (handle);
            auto& s = list.emplace_back (it != old.end() ? std::move (it->second) : std::make_unique<Strip> (engine, handle));
            if (it != old.end())
                old.erase (it);
            // Buses keep their slot number; the others are numbered by position.
            s->setNumber (k == (int) StripKind::Bus ? s->getId() + 1 : i + 1);
        }
    }

    ++structureVersion;
}

Strip* MixerEngine::addChannel (int requestedId)
{
    JUCE_ASSERT_MESSAGE_THREAD
    auto* handle = ld::ld_engine_add_channel (engine, requestedId);
    if (handle == nullptr)
        return nullptr;

    syncStrips();
    return getChannels().back().get();
}

void MixerEngine::setNumChannels (int numChannels)
{
    JUCE_ASSERT_MESSAGE_THREAD
    ld::ld_engine_set_num_channels (engine, juce::jmax (0, numChannels));
    syncStrips();
}

Strip* MixerEngine::addBus (int requestedSlot)
{
    JUCE_ASSERT_MESSAGE_THREAD
    auto* handle = ld::ld_engine_add_bus (engine, requestedSlot);
    if (handle == nullptr)
        return nullptr;

    syncStrips();
    return getBusInSlot (ld::ld_strip_id (handle));
}

Strip* MixerEngine::getBusInSlot (int slot) const noexcept
{
    for (auto& b : getBuses())
        if (b->getId() == slot)
            return b.get();

    return nullptr;
}

Strip* MixerEngine::findChannel (int id) const noexcept
{
    for (auto& s : getChannels())
        if (s->getId() == id)
            return s.get();

    return nullptr;
}

void MixerEngine::removeStrip (Strip* strip)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (strip != nullptr && ld::ld_engine_remove_strip (engine, strip->getHandle()))
        syncStrips();
}

void MixerEngine::reset()
{
    JUCE_ASSERT_MESSAGE_THREAD
    ld::ld_engine_reset (engine);
    syncStrips();
}

void MixerEngine::setDcaName (int index, const juce::String& name)
{
    const Utf8 u (name);
    ld::ld_engine_set_group_name (engine, 0, index, u.data, u.size);
}

void MixerEngine::setMuteGroupName (int index, const juce::String& name)
{
    const Utf8 u (name);
    ld::ld_engine_set_group_name (engine, 1, index, u.data, u.size);
}

void MixerEngine::timerCallback()
{
    garbage.collect();
}

bool MixerEngine::isSoloAfl (StripKind kind) const noexcept
{
    switch (kind)
    {
        case StripKind::Bus:    return busSoloAfl.load();
        case StripKind::Main:   return mainSoloAfl.load();
        case StripKind::Matrix: return matrixSoloAfl.load();
        case StripKind::Channel:
        case StripKind::Aux:    break;
    }
    return soloAfl.load();
}

void MixerEngine::setSoloAfl (StripKind kind, bool afl) noexcept
{
    switch (kind)
    {
        case StripKind::Bus:    busSoloAfl = afl; return;
        case StripKind::Main:   mainSoloAfl = afl; return;
        case StripKind::Matrix: matrixSoloAfl = afl; return;
        case StripKind::Channel:
        case StripKind::Aux:    break;
    }
    soloAfl = afl;
}

bool MixerEngine::applyStartupMainMute()
{
    return ld::ld_engine_apply_startup_main_mute (engine);
}

//==============================================================================
void MixerEngine::setSoloBus (int choice) noexcept
{
    soloBus = juce::jlimit (0, 2, choice);
    if (soloMode.load() == ld::kSoloLive && anySoloActive.load())
        controls.soloBusOverride = true;
}

Strip* MixerEngine::getTalkStrip() const noexcept
{
    const int kind = talk.assignKind.load(), index = talk.assignIndex.load();
    if (kind != (int) StripKind::Channel && kind != (int) StripKind::Aux)
        return nullptr;
    const auto& list = getStrips (static_cast<StripKind> (kind));
    return juce::isPositiveAndBelow (index, (int) list.size()) ? list[(size_t) index].get() : nullptr;
}

void MixerEngine::setTalkStrip (const Strip* strip) noexcept
{
    if (strip == nullptr || ! strip->isInput())
    {
        talk.assignKind = ld::kTalkOff;
        return;
    }
    talk.assignIndex = strip->getNumber() - 1;
    talk.assignKind = (int32_t) strip->getKind();
}

void MixerEngine::talkKey (int path, bool down) noexcept
{
    if (! juce::isPositiveAndBelow (path, 2))
        return;
    auto& p = talk.paths[path];
    const auto now = juce::Time::getMillisecondCounter();
    const auto i = (size_t) path;
    switch (p.mode.load())
    {
        case ld::kTalkPush:
            p.on = down;
            break;
        case ld::kTalkLatch:
            if (down)
                p.on = ! p.on.load();
            break;
        default:
            // AUTO: a press turns talk on (or off when it's latched on); released
            // after the hold time it was a push, so it ends; a short press latches.
            if (down)
            {
                talkPressMs[i] = now;
                talkEndsOnRelease[i] = ! p.on.load();
                p.on = ! p.on.load();
            }
            else if (talkEndsOnRelease[i] && now - talkPressMs[i] >= kTalkHoldMs)
            {
                p.on = false;
            }
            break;
    }
}

juce::String MixerEngine::monitorSourceName (int index)
{
    struct Group { const char* name; int count; };
    const Group groups[] = { { "MAIN", kNumMains }, { "MTX", kNumMatrices }, { "BUS", kWingBuses }, { "AUX", kNumAux } };
    for (auto& g : groups)
    {
        if (juce::isPositiveAndBelow (index, g.count))
            return juce::String (g.name) + "." + juce::String (index + 1);
        index -= g.count;
    }
    return "OFF";
}

juce::String MixerEngine::directSourceName (int index)
{
    return takeString (ld::ld_monitor_direct_name (index));
}

Strip* MixerEngine::getSourceSoloHost() const noexcept
{
    const auto* host = ld::ld_engine_source_solo_host (engine);
    for (auto kind : { StripKind::Channel, StripKind::Aux })
        for (auto& s : getStrips (kind))
            if (s->getHandle() == host)
                return s.get();
    return nullptr;
}

void MixerEngine::setSourceSolo (int port, bool on) noexcept
{
    auto* host = getSourceSoloHost();
    if (on && sourceIndex (port) >= 0)
    {
        monitor.sourceSoloPort = port;
        if (host != nullptr)
            host->params.solo = true;
        else
            monitor.sourceSoloOn = true;
    }
    else if (! on && monitor.sourceSoloPort.load() == port)
        clearSourceSolo();
}

void MixerEngine::setSourceSoloPort (int port) noexcept
{
    if (sourceIndex (port) >= 0)
        monitor.sourceSoloPort = port;
}

bool MixerEngine::isSourceSoloActive() const noexcept
{
    const auto* host = getSourceSoloHost();
    return host != nullptr && (monitor.sourceSoloOn.load() || host->params.solo.load());
}

void MixerEngine::clearSourceSolo() noexcept
{
    monitor.sourceSoloOn = false;
    if (auto* host = getSourceSoloHost())
        host->params.solo = false;
}

bool MixerEngine::isSourceSoloed (int port) const noexcept
{
    return isSourceSoloActive() && monitor.sourceSoloPort.load() == port;
}

void MixerEngine::setSourceName (int index, const juce::String& name)
{
    const Utf8 u (name);
    ld::ld_source_set_name (engine, index, u.data, u.size);
}

int MixerEngine::getTotalDspFaults() const
{
    return ld::ld_engine_total_dsp_faults (engine);
}

int MixerEngine::getNumFaultedFx() const
{
    return ld::ld_engine_num_faulted_fx (engine);
}

//==============================================================================
void MixerEngine::prepare (double sampleRate, int blockSize, const juce::BigInteger& activeIns,
                           const juce::BigInteger& activeOuts)
{
    auto build = [] (const juce::BigInteger& active, int numHw)
    {
        std::vector<int32_t> map ((size_t) numHw, -1);
        int next = 0;
        for (int hw = 0; hw < numHw; ++hw)
            if (active[hw])
                map[(size_t) hw] = next++;
        return map;
    };

    const auto inMap = build (activeIns, inputNames.size());
    const auto outMap = build (activeOuts, outputNames.size());

    ld::ld_engine_prepare (engine, sampleRate, blockSize, inMap.data(), (int32_t) inMap.size(),
                           outMap.data(), (int32_t) outMap.size());

    // Plugins live on the C++ side and are prepared here, also while the device is stopped.
    fx.prepare (sampleRate, blockSize);
}

void MixerEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    inputNames = device->getInputChannelNames();
    outputNames = device->getOutputChannelNames();

    // JUCE calls this from the thread that (re)starts the device, which is the
    // message thread for AudioDeviceManager.
    prepare (device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples(),
             device->getActiveInputChannels(), device->getActiveOutputChannels());

    deviceRunning = true;
    garbage.setDeviceRunning (true);

    juce::MessageManager::callAsync ([this, alive = std::weak_ptr<bool> (aliveFlag)]
    {
        if (alive.lock() != nullptr && onDeviceChanged)
            onDeviceChanged();
    });
}

void MixerEngine::audioDeviceStopped()
{
    deviceRunning = false;
    garbage.setDeviceRunning (false);
}

void MixerEngine::audioDeviceError (const juce::String& errorMessage)
{
    juce::MessageManager::callAsync ([this, errorMessage, alive = std::weak_ptr<bool> (aliveFlag)]
    {
        if (alive.lock() != nullptr && onDeviceError)
            onDeviceError (errorMessage);
    });
}

void MixerEngine::prepareOffline (double sampleRate, int blockSize, int numHwIns, int numHwOuts)
{
    inputNames.clear();
    outputNames.clear();

    for (int i = 0; i < numHwIns; ++i)
        inputNames.add ("In " + juce::String (i + 1));
    for (int i = 0; i < numHwOuts; ++i)
        outputNames.add ("Out " + juce::String (i + 1));

    juce::BigInteger ins, outs;
    ins.setRange (0, numHwIns, true);
    outs.setRange (0, numHwOuts, true);
    prepare (sampleRate, blockSize, ins, outs);
}

void MixerEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                    int numInputChannels,
                                                    float* const* outputChannelData,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    processBlock (inputChannelData, numInputChannels, outputChannelData, numOutputChannels, numSamples);
}

void MixerEngine::processBlock (const float* const* ins, int numIns,
                                float* const* outs, int numOuts, int numSamples) noexcept
{
    lastCallbackMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
    ld::ld_engine_process (engine, ins, numIns, outs, numOuts, numSamples);
}

} // namespace livedaw
