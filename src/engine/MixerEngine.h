#pragma once

#include "engine/Strip.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace livedaw
{

/** The live mixing engine: a C++ facade over the Rust engine (rust/crates/livedaw-engine).

    Signal flow per audio block (all in Rust):
      1. every input strip reads its hardware input(s) and runs its chain   (parallel)
      2. every bus sums the channel sends feeding it and runs its chain     (parallel)
      3. the master sums assigned channels and groups and runs its chain
      4. solo (PFL/AFL) builds the monitor bus, or the monitor follows master
      5. master, buses, direct outs and monitor are written to the hardware outputs

    The number of input channels is only limited by CPU: strips are processed in
    parallel on realtime worker threads (started here, run by the engine).

    Structure changes happen on the message thread; the engine publishes a new
    immutable topology and frees removed strips later (ld_engine_collect).
    This class keeps a Strip wrapper per engine strip for the UI metadata. */
class MixerEngine final : public juce::AudioIODeviceCallback,
                          private juce::Timer
{
public:
    /** numWorkerThreads < 0 picks a default from the number of CPU cores. */
    explicit MixerEngine (int numWorkerThreads = -1);
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

    /** Adds an input channel. `id` restores a saved id (ignored if already taken). */
    Strip* addInput (int id = -1);
    /** Adds an aux or group bus in the first free slot (or `slot` if it's free).
        Returns nullptr only if all kMaxBuses (16384) slots are used. */
    Strip* addBus (StripKind kind, int slot = -1);
    void removeStrip (Strip* strip);

    /** Finds a bus by its slot id. */
    Strip* getBusInSlot (int slot) const noexcept;

    /** Removes all inputs and buses and resets the master (used when loading sessions). */
    void reset();

private:
    ld::Engine* engine;
    ld::EngineControls& controls;

public:
    //==============================================================================
    // Monitor / solo (the engine's own atomics)
    std::atomic<bool>& soloAfl;               // false = PFL, true = AFL
    std::atomic<float>& monitorLevelDb;
    std::atomic<int32_t>& monitorOutputLeft;
    std::atomic<int32_t>& monitorOutputRight;
    std::atomic<float> (&monitorPeak)[2];
    std::atomic<bool>& anySoloActive;

    //==============================================================================
    // Stability / protection
    std::atomic<bool>& outputProtection;      // brick-wall safety clip on every hardware output
    std::atomic<float>& outputCeilingDb;
    std::atomic<int32_t>& outputClipEvents;   // blocks in which the safety clip engaged

    /** Millisecond counter (juce::Time::getMillisecondCounter) of the last audio callback. */
    juce::uint32 getLastCallbackTime() const noexcept { return lastCallbackMs.load (std::memory_order_relaxed); }
    bool isDeviceRunning() const noexcept { return deviceRunning.load(); }

    /** Sum of the NaN/inf faults caught on all strips and the number of auto-bypassed plugins. */
    int getTotalDspFaults() const;
    int getNumFaultedPlugins() const;
    /** Panics caught inside the engine (should stay 0). */
    int getEngineFaults() const noexcept { return controls.engineFaults.load(); }

    /** Called on the message thread when the device reports an error. */
    std::function<void (const juce::String&)> onDeviceError;

    //==============================================================================
    // Device information (message thread)
    double getSampleRate() const noexcept { return ld::ld_engine_sample_rate (engine); }
    int getBlockSize() const noexcept { return ld::ld_engine_block_size (engine); }
    const juce::StringArray& getHardwareInputNames() const noexcept { return inputNames; }
    const juce::StringArray& getHardwareOutputNames() const noexcept { return outputNames; }
    int getNumWorkerThreads() const noexcept { return ld::ld_engine_num_workers (engine); }

    /** Called (on the message thread) whenever the audio device or its channel layout changes. */
    std::function<void()> onDeviceChanged;

    /** Average time spent in the audio callback relative to the block duration (0..1+). */
    float getDspLoad() const noexcept { return controls.dspLoad.load (std::memory_order_relaxed); }

    /** Deferred destruction of removed strips/plugins (collected by a timer). */
    class Garbage
    {
    public:
        explicit Garbage (ld::Engine* e) : engine (e) {}
        void collect() { ld::ld_engine_collect (engine); }
        /** Frees everything now; only valid while no audio callback can run. */
        void collectAllNow() { ld::ld_engine_collect_all_now (engine); }
        void setDeviceRunning (bool running) { ld::ld_engine_set_device_running (engine, running); }
        int numPending() const { return ld::ld_engine_pending_garbage (engine); }

    private:
        ld::Engine* engine;
    };

    Garbage& getGarbage() noexcept { return garbage; }
    ld::Engine* getHandle() const noexcept { return engine; }

    //==============================================================================
    // AudioIODeviceCallback
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String& errorMessage) override;
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
    class Worker;

    void timerCallback() override;
    void prepare (double sampleRate, int blockSize, const juce::BigInteger& activeIns,
                  const juce::BigInteger& activeOuts);
    void startWorkers (double sampleRate, int blockSize);
    std::unique_ptr<Strip> wrap (ld::Strip* handle);
    void syncInputs();

    Garbage garbage;
    std::vector<std::unique_ptr<Worker>> workers;

    std::vector<std::unique_ptr<Strip>> inputs, buses;
    std::unique_ptr<Strip> master;

    juce::StringArray inputNames, outputNames;
    std::atomic<juce::uint32> lastCallbackMs { 0 };
    std::atomic<bool> deviceRunning { false };
    std::shared_ptr<bool> aliveFlag = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerEngine)
};

} // namespace livedaw
