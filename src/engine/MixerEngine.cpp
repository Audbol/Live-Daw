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
      monitorLevelDb (controls.monitorLevelDb),
      monitorOutputLeft (controls.monitorOutputLeft),
      monitorOutputRight (controls.monitorOutputRight),
      monitorPeak (controls.monitorPeak),
      anySoloActive (controls.anySoloActive),
      outputProtection (controls.outputProtection),
      outputCeilingDb (controls.outputCeilingDb),
      outputClipEvents (controls.outputClipEvents),
      garbage (engine)
{
    master = wrap (ld::ld_engine_master (engine));
    startWorkers (getSampleRate(), getBlockSize());
    startTimerHz (10);
}

MixerEngine::~MixerEngine()
{
    stopTimer();
    // The device manager must have removed this callback before destruction.
    ld::ld_engine_shutdown_workers (engine);
    for (auto& w : workers)
        w->stopThread (2000);
    workers.clear();

    inputs.clear();
    buses.clear();
    master.reset();
    ld::ld_engine_destroy (engine); // releases the plugins still loaded
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
std::unique_ptr<Strip> MixerEngine::wrap (ld::Strip* handle)
{
    jassert (handle != nullptr);
    auto strip = std::make_unique<Strip> (engine, handle);

    switch (strip->getKind())
    {
        case StripKind::Input:
            strip->name = "Ch " + juce::String (inputs.size() + 1);
            break;
        case StripKind::Aux:
        case StripKind::Group:
        {
            int count = 1;
            for (auto& b : buses)
                if (b->getKind() == strip->getKind())
                    ++count;
            strip->name = (strip->getKind() == StripKind::Aux ? "Aux " : "Group ") + juce::String (count);
            break;
        }
        case StripKind::Master:
            strip->name = "Master";
            break;
    }

    return strip;
}

void MixerEngine::syncInputs()
{
    const auto n = static_cast<size_t> (ld::ld_engine_num_inputs (engine));

    // Strips are only ever appended or removed from the end, except by removeStrip().
    if (inputs.size() > n)
        inputs.resize (n);

    for (size_t i = 0; i < n; ++i)
    {
        auto* handle = ld::ld_engine_input_at (engine, (int32_t) i);
        if (i < inputs.size())
        {
            jassert (inputs[i]->getHandle() == handle);
            continue;
        }
        inputs.push_back (wrap (handle));
    }
}

Strip* MixerEngine::addInput (int requestedId)
{
    JUCE_ASSERT_MESSAGE_THREAD
    auto* handle = ld::ld_engine_add_input (engine, requestedId);
    if (handle == nullptr)
        return nullptr;

    return inputs.emplace_back (wrap (handle)).get();
}

void MixerEngine::setNumInputs (int numInputs)
{
    JUCE_ASSERT_MESSAGE_THREAD
    ld::ld_engine_set_num_inputs (engine, juce::jmax (0, numInputs));
    syncInputs();
}

Strip* MixerEngine::addBus (StripKind kind, int requestedSlot)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (kind == StripKind::Aux || kind == StripKind::Group);

    auto* handle = ld::ld_engine_add_bus (engine, static_cast<int32_t> (kind), requestedSlot);
    if (handle == nullptr)
        return nullptr;

    return buses.emplace_back (wrap (handle)).get();
}

Strip* MixerEngine::getBusInSlot (int slot) const noexcept
{
    for (auto& b : buses)
        if (b->getId() == slot)
            return b.get();

    return nullptr;
}

void MixerEngine::removeStrip (Strip* strip)
{
    JUCE_ASSERT_MESSAGE_THREAD

    for (auto* list : { &inputs, &buses })
    {
        for (auto it = list->begin(); it != list->end(); ++it)
        {
            if (it->get() == strip)
            {
                ld::ld_engine_remove_strip (engine, strip->getHandle());
                list->erase (it);
                return;
            }
        }
    }
}

void MixerEngine::reset()
{
    JUCE_ASSERT_MESSAGE_THREAD
    ld::ld_engine_reset (engine);

    inputs.clear();
    buses.clear();
    master = wrap (ld::ld_engine_master (engine));
}

void MixerEngine::timerCallback()
{
    garbage.collect();
}

int MixerEngine::getTotalDspFaults() const
{
    return ld::ld_engine_total_dsp_faults (engine);
}

int MixerEngine::getNumFaultedPlugins() const
{
    return ld::ld_engine_num_faulted_plugins (engine);
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
    for (auto* list : { &inputs, &buses })
        for (auto& s : *list)
            s->inserts.prepare (sampleRate, blockSize);
    master->inserts.prepare (sampleRate, blockSize);
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
