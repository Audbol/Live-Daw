#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace livedaw
{

/** Settings shared by the sandboxed plugins of a PluginHost. */
struct SandboxOptions
{
    /** The helper executable (SidecarPluginHost); see SandboxedPlugin::findHelper(). */
    juce::File helper;

    /** How long the audio thread may wait for a block's output once it is due,
        as a fraction of the block's duration (plus `minWaitMs`). The output of a
        block is collected one block later, so the helper has (1 + fraction) block
        periods to process it. */
    double waitFraction = 0.5;
    double minWaitMs = 0.0;

    int loadTimeoutMs = 30000;    // starting the helper and creating the plugin
    int requestTimeoutMs = 5000;  // state, prepare
    int hangTimeoutMs = 2000;     // a helper that hasn't finished a block by then is killed
    int maxRestarts = 3;          // per minute; then the plugin stays off

    /** Message thread: sandbox events for the event log (crashes, restarts). */
    std::function<void (const juce::String& message, bool isError)> onEvent;
};

/** A plugin running in a separate helper process (SidecarPluginHost), seen by
    Sidecar as an ordinary AudioPluginInstance: FxSlots, shows (state), the editor
    window and parameter automation use it like an in-process plugin.

    Audio: processBlock() hands the block to the helper through shared memory and
    takes back the output of the previous block, so the sandbox adds one block of
    latency (reported through getLatencySamples(), on top of the plugin's own). It
    never locks, allocates or waits without a bound: if the previous block's output
    isn't there within SandboxOptions::waitFraction of a block, or the helper is
    gone, the block fails (didLastBlockFail(); FxSlots then reports a fault, so the
    engine restores the dry signal and bypasses the slot).

    Message thread: a timer (supervise()) mirrors parameter changes made in the
    helper, restarts a helper that crashed or hung (with the plugin's last known
    state) and logs what happened. The slot stays bypassed until the fault is
    cleared, as for any faulted plugin.

    The plugin's own editor opens in a window of the helper process; the editor
    Sidecar shows (createEditor()) has a button for it and generic controls. */
class SandboxedPlugin final : public juce::AudioPluginInstance, private juce::Timer
{
public:
    /** Message thread: starts a helper and creates the plugin in it (nullptr and
        `error` on failure). */
    static std::unique_ptr<SandboxedPlugin> create (const juce::PluginDescription&, double sampleRate, int blockSize,
                                                    std::shared_ptr<const SandboxOptions>, juce::String& error);

    /** The helper next to the running executable (inside the bundle on macOS), or
        a non-existent file. */
    static juce::File findHelper();

    ~SandboxedPlugin() override;

    enum class Status
    {
        running,
        faulted,    // a block timed out or the helper died: being recovered
        restarting,
        stopped     // crashed too often, or couldn't be restarted
    };

    Status getStatus() const noexcept { return status.load(); }
    /** Samples the sandbox adds (one block). */
    int getSandboxLatency() const noexcept { return blockSize; }

    /** Audio thread: the last processBlock() didn't get the plugin's output. */
    bool didLastBlockFail() const noexcept { return lastBlockFailed; }

    /** Message thread (also called by a timer): mirrors the helper's parameter
        changes, recovers a crashed or hung helper. */
    void supervise();

    /** Message thread: asks the helper to show the plugin's editor window. `done`
        gets an error message (empty if shown), on the message thread. */
    void openEditorWindow (const juce::String& title, std::function<void (const juce::String& error)> done);
    void closeEditorWindow();

    /** Test hook: the helper's process id (0 if not running). */
    int getHelperProcessId() const noexcept { return helperPid; }

    //==============================================================================
    void fillInPluginDescription (juce::PluginDescription& d) const override { d = description; }
    const juce::String getName() const override { return pluginName; }
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override {}
    using juce::AudioPluginInstance::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    double getTailLengthSeconds() const override { return tailSeconds; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    int getNumPrograms() override { return juce::jmax (1, programNames.size()); }
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int index) override { return programNames[index]; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

private:
    SandboxedPlugin (const juce::PluginDescription&, std::shared_ptr<const SandboxOptions>);

    class Connection;
    class Param;
    class Editor;
    struct Reply;

    bool launch (juce::String& error);
    bool loadInHelper (double sampleRate, int blockSize, bool firstLoad, juce::String& error);
    void killHelper();
    bool isHelperRunning() const;
    void quiesce();
    void resetPipeline();
    void restart (const juce::String& reason);
    void log (const juce::String& message, bool isError) const;
    void timerCallback() override { supervise(); }

    // Parameter plumbing (Param calls these).
    void sendParam (int index, float value) noexcept;
    juce::String paramText (int index, float value);
    float paramFromText (int index, const juce::String& text, float fallback);
    void syncParamsFromHelper();

    bool processChunk (float* const* channels, int numChannels, int start, int n) noexcept;

    juce::PluginDescription description;
    std::shared_ptr<const SandboxOptions> options;
    juce::String pluginName;
    double tailSeconds = 0.0;
    juce::StringArray programNames;
    int currentProgram = 0;
    juce::Array<Param*> params;

    // Helper process and shared memory (message thread, apart from the audio exchange).
    std::unique_ptr<juce::ChildProcess> process;
    std::unique_ptr<Connection> connection;
    struct Shared;
    std::unique_ptr<Shared> shared;
    std::atomic<int> helperPid { 0 };
    double currentSampleRate = 48000.0;
    juce::Array<juce::int64> restartTimes;
    juce::int64 faultSinceMs = 0;
    juce::int64 lastSnapshotMs = 0;

    // Last known state, restored into a restarted helper (any thread, under stateLock).
    std::mutex stateLock;
    juce::MemoryBlock cachedState;

    // Audio-thread handshake: the message thread only touches the pipeline after
    // setting `status` away from running and seeing `inProcess` false.
    std::atomic<Status> status { Status::stopped };
    std::atomic<bool> inProcess { false };
    std::atomic<bool> helperAlive { false };
    std::atomic<int> faultReason { 0 }; // 1 = timed out, 2 = helper gone (set by the audio thread)

    // Audio-thread pipeline state.
    int blockSize = 512;              // the added latency, and the largest exchanged block
    juce::AudioBuffer<float> fifo;    // plugin output waiting to be played (blockSize samples after a collect)
    int fifoCount = 0;
    int pending = 0;                  // samples sent but not collected yet
    juce::uint32 seq = 0;
    juce::int64 pendingDeadline = 0, lastCallTicks = 0, gapTicks = 0;
    double ticksPerSecond = 1.0;
    bool lastBlockFailed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SandboxedPlugin)
};

} // namespace livedaw
