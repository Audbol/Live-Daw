#pragma once

#include "engine/FxSlots.h"
#include "engine/Strip.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace livedaw
{

/** The live mixing engine, laid out like a Behringer WING: a C++ facade over
    the Rust engine (rust/crates/livedaw-engine).

    Signal flow per audio block (all in Rust):
      1. channels and aux channels read their inputs and run their chains (parallel)
      2. automix groups X/Y share gain between their channels
      3. buses sum the sends feeding them (buses 9+ first, they can feed 1-8) (parallel)
      4. the 4 mains sum their sends, the 8 matrices their sends + direct inputs
      5. solo (PFL/AFL) builds the monitor bus, or the monitor follows Main 1
      6. mains, buses, matrices, direct outs and monitor go to the hardware outputs

    DCAs (16) and mute groups (8) act on any strip; the 16 FX slots hold the
    plugins the strips' inserts use. The number of channels is only limited by
    CPU: strips run in parallel on realtime worker threads (started here, run by
    the engine), and more buses than the WING's 16 can be added.

    Structure changes happen on the message thread; the engine publishes a new
    immutable topology and frees removed strips later (ld_engine_collect).
    This class keeps a (stateless) Strip view per engine strip for the UI. */
class MixerEngine final : public juce::AudioIODeviceCallback,
                          private juce::Timer
{
public:
    /** numWorkerThreads < 0 picks a default from the number of CPU cores. */
    explicit MixerEngine (int numWorkerThreads = -1);
    ~MixerEngine() override;

    //==============================================================================
    // Structure (message thread only)
    using StripList = std::vector<std::unique_ptr<Strip>>;

    const StripList& getStrips (StripKind kind) const noexcept { return lists[(size_t) kind]; }
    const StripList& getChannels() const noexcept { return getStrips (StripKind::Channel); }
    const StripList& getAux() const noexcept { return getStrips (StripKind::Aux); }
    const StripList& getBuses() const noexcept { return getStrips (StripKind::Bus); }
    const StripList& getMains() const noexcept { return getStrips (StripKind::Main); }
    const StripList& getMatrices() const noexcept { return getStrips (StripKind::Matrix); }
    /** Main 1 (the default monitor source). */
    Strip& getMain (int index = 0) const noexcept { return *getMains()[(size_t) juce::jlimit (0, kNumMains - 1, index)]; }

    /** Grows or shrinks the number of channels. New channel N is patched to
        hardware input N (when it exists). */
    void setNumChannels (int numChannels);

    /** Adds a channel. `id` restores a saved id (ignored if already taken). */
    Strip* addChannel (int id = -1);
    /** Adds a bus beyond the console's 16 in the first free slot (or `slot` if
        free). Returns nullptr only if all kMaxBuses slots are used. */
    Strip* addBus (int slot = -1);
    /** Removes a channel, or a bus beyond the console's 16. */
    void removeStrip (Strip* strip);

    Strip* getBusInSlot (int slot) const noexcept;
    Strip* findChannel (int id) const noexcept;

    /** Back to a fresh console: no channels, default aux/buses/mains/matrices,
        DCAs, mute groups and FX settings. */
    void reset();

    /** Rebuilds the strip lists from the engine after a change made directly in
        Rust (e.g. loading a show). Wrappers of strips that still exist are kept. */
    void syncStrips();

    /** Bumped by every structural change (for views that cache strip pointers). */
    int getStructureVersion() const noexcept { return structureVersion; }

    //==============================================================================
    // Groups and FX
    DcaParams& getDca (int index) const noexcept { return controls.dcas[juce::jlimit (0, kNumDcas - 1, index)]; }
    juce::String getDcaName (int index) const { return takeString (ld::ld_engine_group_name (engine, 0, index)); }
    void setDcaName (int index, const juce::String&);
    juce::String getMuteGroupName (int index) const { return takeString (ld::ld_engine_group_name (engine, 1, index)); }
    void setMuteGroupName (int index, const juce::String&);
    FxSlots& getFx() noexcept { return fx; }

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

    /** Bit n = mute group n+1 engaged. */
    std::atomic<uint32_t>& muteGroups;

    //==============================================================================
    // Stability / protection
    std::atomic<bool>& outputProtection;      // brick-wall safety clip on every hardware output
    std::atomic<float>& outputCeilingDb;
    std::atomic<int32_t>& outputClipEvents;   // blocks in which the safety clip engaged

    /** Millisecond counter (juce::Time::getMillisecondCounter) of the last audio callback. */
    juce::uint32 getLastCallbackTime() const noexcept { return lastCallbackMs.load (std::memory_order_relaxed); }
    bool isDeviceRunning() const noexcept { return deviceRunning.load(); }

    /** Sum of the NaN/inf faults caught on all strips and the number of auto-bypassed FX slots. */
    int getTotalDspFaults() const;
    int getNumFaultedFx() const;
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

    Garbage garbage;
    FxSlots fx;
    std::vector<std::unique_ptr<Worker>> workers;

    std::array<StripList, kNumStripKinds> lists;
    int structureVersion = 0;

    juce::StringArray inputNames, outputNames;
    std::atomic<juce::uint32> lastCallbackMs { 0 };
    std::atomic<bool> deviceRunning { false };
    std::shared_ptr<bool> aliveFlag = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerEngine)
};

} // namespace livedaw
