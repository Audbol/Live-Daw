#pragma once

#include "engine/RtGarbage.h"
#include "engine/Strip.h"
#include "engine/WorkerPool.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace livedaw
{

/** The live mixing engine.

    Signal flow per audio block:
      1. every input strip reads its hardware input(s) and runs its chain   (parallel)
      2. every bus sums the channel sends feeding it and runs its chain     (parallel)
      3. the master sums assigned channels and groups and runs its chain
      4. solo (PFL/AFL) builds the monitor bus, or the monitor follows master
      5. master, buses, direct outs and monitor are written to the hardware outputs

    The number of input channels is only limited by CPU: strips are processed in
    parallel on a realtime worker pool.

    Structure (adding/removing strips) is changed on the message thread by
    building a new immutable Topology and swapping it in atomically. Removed
    strips are destroyed later on the message thread (see RtGarbage).
*/
class MixerEngine final : public juce::AudioIODeviceCallback,
                          private juce::Timer
{
public:
    explicit MixerEngine (int numWorkerThreads = WorkerPool::defaultNumWorkers());
    ~MixerEngine() override;

    //==============================================================================
    // Structure (message thread only)
    const std::vector<std::unique_ptr<Strip>>& getInputs() const noexcept { return inputs; }
    const std::vector<std::unique_ptr<Strip>>& getBuses() const noexcept { return buses; }
    Strip& getMaster() noexcept { return *master; }
    const Strip& getMaster() const noexcept { return *master; }

    /** Grows or shrinks the number of input channels. New channel N is patched to
        hardware input N (when it exists). */
    void setNumInputs (int numInputs);

    Strip* addInput();
    /** Adds an aux or group bus in the first free slot (or `slot` if it's free).
        Returns nullptr when all kMaxBuses slots are used. */
    Strip* addBus (StripKind kind, int slot = -1);
    void removeStrip (Strip* strip);

    /** Finds a bus by its slot id. */
    Strip* getBusInSlot (int slot) const noexcept;

    /** Removes all inputs and buses and resets the master (used when loading sessions). */
    void reset();

    //==============================================================================
    // Monitor / solo
    std::atomic<bool> soloAfl { false };  // false = PFL, true = AFL
    std::atomic<float> monitorLevelDb { 0.0f };
    std::atomic<int> monitorOutputLeft { -1 }, monitorOutputRight { -1 };
    std::array<std::atomic<float>, 2> monitorPeak {};
    std::atomic<bool> anySoloActive { false };

    //==============================================================================
    // Device information (message thread)
    double getSampleRate() const noexcept { return sampleRate; }
    int getBlockSize() const noexcept { return blockSize; }
    const juce::StringArray& getHardwareInputNames() const noexcept { return inputNames; }
    const juce::StringArray& getHardwareOutputNames() const noexcept { return outputNames; }
    int getNumWorkerThreads() const noexcept { return pool.getNumWorkers(); }

    /** Called (on the message thread) whenever the audio device or its channel layout changes. */
    std::function<void()> onDeviceChanged;

    /** Average time spent in the audio callback relative to the block duration (0..1+). */
    float getDspLoad() const noexcept { return dspLoad.load (std::memory_order_relaxed); }

    RtGarbage& getGarbage() noexcept { return garbage; }

    //==============================================================================
    // AudioIODeviceCallback
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;

    /** Prepares the engine without a real device (tests / offline rendering).
        Hardware channel numbers map 1:1 to the buffers passed to processBlock(). */
    void prepareOffline (double sampleRate, int blockSize, int numHardwareInputs, int numHardwareOutputs);

    /** Runs one block. Buffers are the *active* device channels, as JUCE delivers them. */
    void processBlock (const float* const* inputs, int numInputs,
                       float* const* outputs, int numOutputs, int numSamples) noexcept;

private:
    struct Topology
    {
        std::vector<Strip*> inputs, buses;
        Strip* master = nullptr;
    };

    void timerCallback() override;
    void publishTopology();
    void prepareAll();
    std::unique_ptr<Strip> createInputStrip();
    void prepareStrip (Strip&);
    void setupChannelMaps (const juce::BigInteger& activeIns, const juce::BigInteger& activeOuts,
                           int numHwIns, int numHwOuts);

    void processChunk (const Topology&, int numSamples) noexcept;
    static void sumSendsIntoBus (Strip& bus, const std::vector<Strip*>& inputs, int numSamples) noexcept;
    void sumMaster (const Topology&, int numSamples) noexcept;
    void buildMonitor (const Topology&, int numSamples) noexcept;
    void writeOutputs (const Topology&, int numSamples) noexcept;
    void addToOutput (int hwChannel, const float* src, int numSamples) noexcept;

    RtGarbage garbage;
    WorkerPool pool;

    std::vector<std::unique_ptr<Strip>> inputs, buses;
    std::unique_ptr<Strip> master;
    int nextInputId = 1;

    std::atomic<Topology*> topology { nullptr };

    // Device state: only modified while the device is stopped.
    double sampleRate = 48000.0;
    int blockSize = 512;
    juce::StringArray inputNames, outputNames;
    std::vector<int> hwInToActive, hwOutToActive;
    std::vector<const float*> hwInPtrs;   // indexed by hardware channel, per chunk
    std::vector<float*> hwOutPtrs;        // indexed by hardware channel, per chunk

    juce::AudioBuffer<float> monitorBuffer;
    float lastMonitorGain = 0.0f, lastMasterToMonitor = 1.0f;
    std::atomic<float> dspLoad { 0.0f };
    std::shared_ptr<bool> aliveFlag = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerEngine)
};

} // namespace livedaw
