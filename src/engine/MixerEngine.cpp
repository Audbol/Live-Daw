#include "engine/MixerEngine.h"
#include "dsp/DspUtils.h"

namespace livedaw
{

MixerEngine::MixerEngine (int numWorkerThreads)
    : pool (numWorkerThreads)
{
    for (auto& p : monitorPeak)
        p.store (0.0f);

    master = std::make_unique<Strip> (StripKind::Master, 0, garbage);
    master->name = "Master";
    monitorBuffer.setSize (2, blockSize);

    publishTopology();
    startTimerHz (10);
}

MixerEngine::~MixerEngine()
{
    stopTimer();
    // The device manager must have removed this callback before destruction.
    delete topology.exchange (nullptr);
    garbage.collectAllNow();
}

//==============================================================================
void MixerEngine::publishTopology()
{
    JUCE_ASSERT_MESSAGE_THREAD

    auto next = std::make_unique<Topology>();
    next->master = master.get();

    for (auto& s : inputs)
        next->inputs.push_back (s.get());
    for (auto& s : buses)
        next->buses.push_back (s.get());

    std::unique_ptr<Topology> old (topology.exchange (next.release(), std::memory_order_acq_rel));
    garbage.retire (std::move (old));
}

void MixerEngine::prepareStrip (Strip& s)
{
    s.prepare (sampleRate, blockSize);
}

void MixerEngine::prepareAll()
{
    for (auto& s : inputs)
        prepareStrip (*s);
    for (auto& s : buses)
        prepareStrip (*s);
    prepareStrip (*master);

    monitorBuffer.setSize (2, blockSize, false, true, false);
    pool.setBlockDurationHint (sampleRate, blockSize);
}

std::unique_ptr<Strip> MixerEngine::createInputStrip (int requestedId)
{
    const int index = static_cast<int> (inputs.size());

    bool idFree = requestedId > 0;
    for (auto& s : inputs)
        idFree = idFree && s->getId() != requestedId;

    const int id = idFree ? requestedId : nextInputId;
    nextInputId = juce::jmax (nextInputId, id + 1);

    auto strip = std::make_unique<Strip> (StripKind::Input, id, garbage);
    strip->name = "Ch " + juce::String (index + 1);
    strip->params.inputLeft = (inputNames.isEmpty() || index < inputNames.size()) ? index : -1;
    prepareStrip (*strip);
    return strip;
}

Strip* MixerEngine::addInput (int requestedId)
{
    JUCE_ASSERT_MESSAGE_THREAD

    auto* raw = inputs.emplace_back (createInputStrip (requestedId)).get();
    publishTopology();
    return raw;
}

void MixerEngine::setNumInputs (int numInputs)
{
    JUCE_ASSERT_MESSAGE_THREAD
    numInputs = juce::jmax (0, numInputs);
    const auto target = static_cast<size_t> (numInputs);

    if (target == inputs.size())
        return;

    std::vector<std::unique_ptr<Strip>> removed;

    while (inputs.size() < target)
        inputs.push_back (createInputStrip());

    while (inputs.size() > target)
    {
        removed.push_back (std::move (inputs.back()));
        inputs.pop_back();
    }

    publishTopology();

    for (auto& s : removed)
        garbage.retire (std::move (s));
}

Strip* MixerEngine::addBus (StripKind kind, int requestedSlot)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (kind == StripKind::Aux || kind == StripKind::Group);

    std::vector<bool> used ((size_t) kMaxBuses, false);
    for (auto& b : buses)
        used[(size_t) b->getId()] = true;

    int slot = -1;
    if (juce::isPositiveAndBelow (requestedSlot, kMaxBuses) && ! used[(size_t) requestedSlot])
        slot = requestedSlot;
    else
        for (int i = 0; i < kMaxBuses && slot < 0; ++i)
            if (! used[(size_t) i])
                slot = i;

    if (slot < 0)
        return nullptr;

    // Make sure no channel still carries a send from an old bus in this slot.
    // (The ramp state is left alone: an audio callback may still be finishing
    // with a bus just removed from this slot. It settles to the new target.)
    for (auto& in : inputs)
    {
        auto& send = in->params.sends[(size_t) slot];
        send.enabled = false;
        send.levelDb = 0.0f;
        send.preFader = (kind == StripKind::Aux);
    }

    auto strip = std::make_unique<Strip> (kind, slot, garbage);
    int count = 1;
    for (auto& b : buses)
        if (b->getKind() == kind)
            ++count;
    strip->name = (kind == StripKind::Aux ? "Aux " : "Group ") + juce::String (count);
    prepareStrip (*strip);

    auto* raw = strip.get();
    buses.push_back (std::move (strip));
    publishTopology();
    return raw;
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
                auto owned = std::move (*it);
                list->erase (it);
                publishTopology();

                if (owned->isBus())
                    for (auto& in : inputs)
                        if (auto* send = in->params.sends.find (owned->getId()))
                send->enabled = false;

                garbage.retire (std::move (owned));
                return;
            }
        }
    }
}

void MixerEngine::reset()
{
    JUCE_ASSERT_MESSAGE_THREAD

    std::vector<std::unique_ptr<Strip>> removed;
    for (auto& s : inputs)
        removed.push_back (std::move (s));
    for (auto& s : buses)
        removed.push_back (std::move (s));
    removed.push_back (std::move (master));

    inputs.clear();
    buses.clear();
    nextInputId = 1;

    master = std::make_unique<Strip> (StripKind::Master, 0, garbage);
    master->name = "Master";
    prepareStrip (*master);

    publishTopology();

    for (auto& s : removed)
        garbage.retire (std::move (s));

    soloAfl = false;
    monitorLevelDb = 0.0f;
}

void MixerEngine::timerCallback()
{
    garbage.collect();
}

//==============================================================================
void MixerEngine::setupChannelMaps (const juce::BigInteger& activeIns, const juce::BigInteger& activeOuts,
                                    int numHwIns, int numHwOuts)
{
    auto build = [] (const juce::BigInteger& active, int numHw, std::vector<int>& map)
    {
        map.assign ((size_t) numHw, -1);
        int next = 0;
        for (int hw = 0; hw < numHw; ++hw)
            if (active[hw])
                map[(size_t) hw] = next++;
    };

    build (activeIns, numHwIns, hwInToActive);
    build (activeOuts, numHwOuts, hwOutToActive);
    hwInPtrs.assign ((size_t) numHwIns, nullptr);
    hwOutPtrs.assign ((size_t) numHwOuts, nullptr);
}

void MixerEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    sampleRate = device->getCurrentSampleRate();
    blockSize = device->getCurrentBufferSizeSamples();
    inputNames = device->getInputChannelNames();
    outputNames = device->getOutputChannelNames();

    setupChannelMaps (device->getActiveInputChannels(), device->getActiveOutputChannels(),
                      inputNames.size(), outputNames.size());

    // Strips are owned by the message thread; JUCE calls this from the thread that
    // (re)starts the device, which is the message thread for AudioDeviceManager.
    prepareAll();
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

int MixerEngine::getTotalDspFaults() const
{
    int total = master->dspFaults.load();
    for (auto* list : { &inputs, &buses })
        for (auto& s : *list)
            total += s->dspFaults.load();
    return total;
}

int MixerEngine::getNumFaultedPlugins() const
{
    int total = 0;
    auto count = [&total] (const Strip& s)
    {
        for (int slot = 0; slot < kNumInsertSlots; ++slot)
            if (s.inserts.isFaulted (slot))
                ++total;
    };

    count (*master);
    for (auto* list : { &inputs, &buses })
        for (auto& s : *list)
            count (*s);
    return total;
}

void MixerEngine::prepareOffline (double newSampleRate, int newBlockSize, int numHwIns, int numHwOuts)
{
    sampleRate = newSampleRate;
    blockSize = newBlockSize;

    inputNames.clear();
    outputNames.clear();
    for (int i = 0; i < numHwIns; ++i)
        inputNames.add ("In " + juce::String (i + 1));
    for (int i = 0; i < numHwOuts; ++i)
        outputNames.add ("Out " + juce::String (i + 1));

    juce::BigInteger ins, outs;
    ins.setRange (0, numHwIns, true);
    outs.setRange (0, numHwOuts, true);
    setupChannelMaps (ins, outs, numHwIns, numHwOuts);
    prepareAll();
}

void MixerEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                    int numInputChannels,
                                                    float* const* outputChannelData,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    const auto startTicks = juce::Time::getHighResolutionTicks();

    processBlock (inputChannelData, numInputChannels, outputChannelData, numOutputChannels, numSamples);

    const auto elapsed = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - startTicks);
    const auto load = static_cast<float> (elapsed * sampleRate / juce::jmax (1, numSamples));
    dspLoad.store (0.9f * dspLoad.load (std::memory_order_relaxed) + 0.1f * load, std::memory_order_relaxed);
}

//==============================================================================
void MixerEngine::processBlock (const float* const* ins, int numIns,
                                float* const* outs, int numOuts, int numSamples) noexcept
{
    const juce::ScopedNoDenormals noDenormals;
    garbage.audioCallbackStarted();
    lastCallbackMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);

    for (int i = 0; i < numOuts; ++i)
        if (outs[i] != nullptr)
            juce::FloatVectorOperations::clear (outs[i], numSamples);

    if (auto* topo = topology.load (std::memory_order_acquire))
    {
        // Devices may deliver more samples than announced; process in prepared-size chunks.
        for (int start = 0; start < numSamples; start += blockSize)
        {
            const int n = juce::jmin (blockSize, numSamples - start);

            for (size_t hw = 0; hw < hwInPtrs.size(); ++hw)
            {
                const int a = hwInToActive[hw];
                hwInPtrs[hw] = (a >= 0 && a < numIns && ins[a] != nullptr) ? ins[a] + start : nullptr;
            }

            for (size_t hw = 0; hw < hwOutPtrs.size(); ++hw)
            {
                const int a = hwOutToActive[hw];
                hwOutPtrs[hw] = (a >= 0 && a < numOuts && outs[a] != nullptr) ? outs[a] + start : nullptr;
            }

            processChunk (*topo, n);
        }
    }

    garbage.audioCallbackFinished();
}

void MixerEngine::processChunk (const Topology& topo, int n) noexcept
{
    const auto* const* hwIns = hwInPtrs.data();
    const int numHwIns = static_cast<int> (hwInPtrs.size());

    // 1. Input channels (parallel)
    pool.parallelFor (static_cast<int> (topo.inputs.size()), [&] (int i)
    {
        auto& s = *topo.inputs[(size_t) i];
        s.readHardwareInputs (hwIns, numHwIns, n);
        s.process (n);
    });

    // 2. Buses (parallel; each bus owns its own send ramp state on every input)
    pool.parallelFor (static_cast<int> (topo.buses.size()), [&] (int i)
    {
        auto& bus = *topo.buses[(size_t) i];
        sumSendsIntoBus (bus, topo.inputs, n);
        bus.process (n);
    });

    // 3. Master
    sumMaster (topo, n);

    // 4. Solo / monitor
    buildMonitor (topo, n);

    // 5. Hardware outputs
    writeOutputs (topo, n);

    // 6. Safety clip: nothing above the ceiling ever reaches the amplifiers.
    protectOutputs (n);
}

void MixerEngine::protectOutputs (int n) noexcept
{
    if (! outputProtection.load (std::memory_order_relaxed))
        return;

    const float ceiling = juce::jlimit (0.0f, 4.0f, dsp::dbToGain (outputCeilingDb.load (std::memory_order_relaxed)));
    bool engaged = false;

    for (auto* out : hwOutPtrs)
    {
        if (out == nullptr)
            continue;

        if (! dsp::allFinite (out, n))
        {
            juce::FloatVectorOperations::clear (out, n);
            engaged = true;
            continue;
        }

        if (dsp::peakLevel (out, n) > ceiling)
        {
            juce::FloatVectorOperations::clip (out, out, -ceiling, ceiling, n);
            engaged = true;
        }
    }

    if (engaged)
        outputClipEvents.fetch_add (1, std::memory_order_relaxed);
}

void MixerEngine::sumSendsIntoBus (Strip& bus, const std::vector<Strip*>& ins, int n) noexcept
{
    bus.beginSummedBlock (n);

    const bool busStereo = bus.getNumChannels() == 2;
    auto* const* dst = bus.getWorkBuffers();

    for (auto* in : ins)
    {
        auto* sendPtr = in->params.sends.find (bus.getId());
        if (sendPtr == nullptr)
            continue; // never configured: no send

        auto& send = *sendPtr;
        const float target = send.enabled.load (std::memory_order_relaxed)
                                 ? dsp::dbToGain (send.levelDb.load (std::memory_order_relaxed))
                                 : 0.0f;
        float& last = send.lastGain;

        if (target == 0.0f && last == 0.0f)
            continue;

        const auto* const* src = send.preFader.load (std::memory_order_relaxed) ? in->getPreFader()
                                                                                 : in->getPostFader();
        const bool srcStereo = in->getNumChannels() == 2;

        if (busStereo)
        {
            // Sends follow the channel pan/balance into stereo buses.
            const float l = in->getPanLeft(), r = in->getPanRight();
            dsp::addWithGainRamp (dst[0], src[0], n, last * l, target * l);
            dsp::addWithGainRamp (dst[1], src[srcStereo ? 1 : 0], n, last * r, target * r);
        }
        else if (srcStereo)
        {
            dsp::addWithGainRamp (dst[0], src[0], n, last * 0.5f, target * 0.5f);
            dsp::addWithGainRamp (dst[0], src[1], n, last * 0.5f, target * 0.5f);
        }
        else
        {
            dsp::addWithGainRamp (dst[0], src[0], n, last, target);
        }

        last = target;
    }
}

void MixerEngine::sumMaster (const Topology& topo, int n) noexcept
{
    auto& m = *topo.master;
    m.beginSummedBlock (n);
    auto* const* dst = m.getWorkBuffers();

    auto addStrip = [&] (Strip& s)
    {
        const float target = s.params.mainAssign.load (std::memory_order_relaxed) ? 1.0f : 0.0f;

        if (target != 0.0f || s.lastMainAssignGain != 0.0f)
        {
            const auto* const* src = s.getMainOut();
            dsp::addWithGainRamp (dst[0], src[0], n, s.lastMainAssignGain, target);
            dsp::addWithGainRamp (dst[1], src[1], n, s.lastMainAssignGain, target);
        }

        s.lastMainAssignGain = target;
    };

    for (auto* s : topo.inputs)
        addStrip (*s);
    for (auto* s : topo.buses)
        addStrip (*s);

    m.process (n);
}

void MixerEngine::buildMonitor (const Topology& topo, int n) noexcept
{
    bool solo = false;
    for (auto* s : topo.inputs)
        solo = solo || s->params.solo.load (std::memory_order_relaxed);
    for (auto* s : topo.buses)
        solo = solo || s->params.solo.load (std::memory_order_relaxed);

    anySoloActive.store (solo, std::memory_order_relaxed);

    monitorBuffer.clear (0, n);
    auto* const* mon = monitorBuffer.getArrayOfWritePointers();
    const bool afl = soloAfl.load (std::memory_order_relaxed);

    auto addSolo = [&] (Strip& s)
    {
        const float target = (solo && s.params.solo.load (std::memory_order_relaxed)) ? 1.0f : 0.0f;

        if (target != 0.0f || s.lastSoloGain != 0.0f)
        {
            if (afl)
            {
                const auto* const* src = s.getMainOut();
                dsp::addWithGainRamp (mon[0], src[0], n, s.lastSoloGain, target);
                dsp::addWithGainRamp (mon[1], src[1], n, s.lastSoloGain, target);
            }
            else
            {
                const auto* const* src = s.getPostChain();
                const bool stereo = s.getNumChannels() == 2;
                dsp::addWithGainRamp (mon[0], src[0], n, s.lastSoloGain, target);
                dsp::addWithGainRamp (mon[1], src[stereo ? 1 : 0], n, s.lastSoloGain, target);
            }
        }

        s.lastSoloGain = target;
    };

    for (auto* s : topo.inputs)
        addSolo (*s);
    for (auto* s : topo.buses)
        addSolo (*s);

    // With nothing soloed the monitor follows the master.
    const float masterTarget = solo ? 0.0f : 1.0f;
    const auto* const* m = topo.master->getMainOut();
    dsp::addWithGainRamp (mon[0], m[0], n, lastMasterToMonitor, masterTarget);
    dsp::addWithGainRamp (mon[1], m[1], n, lastMasterToMonitor, masterTarget);
    lastMasterToMonitor = masterTarget;

    const float level = dsp::dbToGain (monitorLevelDb.load (std::memory_order_relaxed));
    dsp::applyGainRamp (mon[0], n, lastMonitorGain, level);
    dsp::applyGainRamp (mon[1], n, lastMonitorGain, level);
    lastMonitorGain = level;

    for (int c = 0; c < 2; ++c)
        dsp::atomicMax (monitorPeak[(size_t) c], dsp::peakLevel (mon[c], n));
}

void MixerEngine::addToOutput (int hw, const float* src, int n) noexcept
{
    if (hw >= 0 && hw < static_cast<int> (hwOutPtrs.size()))
        if (auto* dst = hwOutPtrs[(size_t) hw])
            juce::FloatVectorOperations::add (dst, src, n);
}

void MixerEngine::writeOutputs (const Topology& topo, int n) noexcept
{
    auto writeStrip = [&] (const Strip& s, bool directOut)
    {
        const int l = s.params.outputLeft.load (std::memory_order_relaxed);
        const int r = s.params.outputRight.load (std::memory_order_relaxed);

        if (l < 0 && r < 0)
            return;

        if (directOut)
        {
            // Input direct outs carry the processed signal before the fader (for recording).
            const auto* const* src = s.getPostChain();
            addToOutput (l, src[0], n);
            addToOutput (r, src[s.getNumChannels() > 1 ? 1 : 0], n);
        }
        else if (s.getNumChannels() == 1)
        {
            // Mono buses feed the same signal to both assigned outputs.
            addToOutput (l, s.getPostFader()[0], n);
            addToOutput (r, s.getPostFader()[0], n);
        }
        else
        {
            addToOutput (l, s.getMainOut()[0], n);
            addToOutput (r, s.getMainOut()[1], n);
        }
    };

    writeStrip (*topo.master, false);

    for (auto* s : topo.buses)
        writeStrip (*s, false);

    for (auto* s : topo.inputs)
        writeStrip (*s, true);

    addToOutput (monitorOutputLeft.load (std::memory_order_relaxed), monitorBuffer.getReadPointer (0), n);
    addToOutput (monitorOutputRight.load (std::memory_order_relaxed), monitorBuffer.getReadPointer (1), n);
}

} // namespace livedaw
