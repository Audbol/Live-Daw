#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>
#include <vector>

namespace livedaw
{

class SandboxedPlugin;

/** Up to kMaxMembers plugins in series, seen by FxSlots (and so by the Rust
    engine, fault detection, auto-bypass and the per-slot CPU meter) as one
    plugin. An EXT slot holds a plugin or a PluginChain; a plugin on its own is
    a chain of one.

    A chain never changes: adding, removing or moving a member builds a new
    chain (FxSlots::addToChain...) that shares the members (and so their
    instances and running state) with the old one and is swapped in like any
    plugin. The engine runs one or the other, never both at once, and destroys
    the old one on the message thread; a member already prepared for the
    current sample rate and block size isn't prepared again.

    Audio: the members run in order on one buffer, skipping bypassed ones (a
    member's bypass is an atomic flag shared by the chains holding it). It never
    allocates or locks. A member that throws, outputs NaN/inf or (sandboxed)
    misses its block fails the block: didLastBlockFail() makes FxSlots report a
    fault like a single plugin's, and getFault() names the member.

    The latency is the sum of the non-bypassed members' (each may change it at
    any time, the chain follows). Members may be SandboxedPlugins.

    The state (getStateInformation, createXml) is every member's description,
    bypass and state as a <PLUGINCHAIN> element, which shows store in place of
    a single <PLUGIN>. */
class PluginChain final : public juce::AudioPluginInstance
{
public:
    static constexpr int kMaxMembers = 8;
    static constexpr const char* kXmlTag = "PLUGINCHAIN";

    /** A plugin in a chain, shared by the chains that hold it. */
    struct Member final : private juce::AudioProcessorListener
    {
        explicit Member (std::shared_ptr<juce::AudioPluginInstance>);
        ~Member() override;

        std::shared_ptr<juce::AudioPluginInstance> plugin;
        SandboxedPlugin* sandboxed = nullptr;
        int numChannels = 2; // the plugin's buffer: at least stereo, more for side-chain buses
        std::atomic<bool> bypassed { false };

        // Message thread: what it was prepared for (0: not prepared).
        double preparedRate = 0.0;
        int preparedBlock = 0;

        /** The chain built last with this member: told when the plugin's latency changes. */
        std::atomic<PluginChain*> owner { nullptr };

    private:
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override;
        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}

        JUCE_DECLARE_NON_COPYABLE (Member)
    };
    using MemberPtr = std::shared_ptr<Member>;

    /** Message thread: wraps a plugin for a chain (set up for stereo). Pass the
        sample rate and block size it's already prepared for, if it's running. */
    static MemberPtr makeMember (std::shared_ptr<juce::AudioPluginInstance>, bool bypassed = false,
                                 double preparedRate = 0.0, int preparedBlock = 0);

    /** Message thread: `members` (1 to kMaxMembers) in processing order. */
    explicit PluginChain (std::vector<MemberPtr> members);
    ~PluginChain() override;

    /** Asks for stereo main buses (what Sidecar's slots run). */
    static void configureForStereo (juce::AudioPluginInstance&);

    int size() const noexcept { return (int) members.size(); }
    juce::AudioPluginInstance& getMember (int index) const noexcept { return *members[(size_t) index]->plugin; }
    const std::vector<MemberPtr>& getMembers() const noexcept { return members; }

    bool isBypassed (int index) const noexcept { return members[(size_t) index]->bypassed.load(); }
    /** Any thread: a bypassed member is skipped and adds no latency. */
    void setBypassed (int index, bool);

    bool hasSandboxedMember() const noexcept;

    /** Any thread: works the latency out again (the members' latencies may have
        changed without telling their host) and reports a change. */
    void updateLatency();

    /** "TAL-Reverb + Pro-Q 3 + Limiter" (getName() shortens it for three or more). */
    juce::String describeMembers() const;

    /** Audio thread: the last processBlock() failed (see getFault()). */
    bool didLastBlockFail() const noexcept { return lastBlockFailed; }

    enum class FaultKind
    {
        none,
        threw,
        nonFinite,
        sandbox // the sandboxed member missed its block or lost its helper
    };
    struct Fault
    {
        int member = -1;
        FaultKind kind = FaultKind::none;
    };
    /** Message thread: the member that failed a block since the last call (then forgotten). */
    Fault takeFault() noexcept;

    /** The chain as <PLUGINCHAIN name=..><MEMBER bypass=..><PLUGIN ../><STATE>base64</STATE></MEMBER>..</PLUGINCHAIN>.
        A member that throws while saving its state is stored without one. */
    std::unique_ptr<juce::XmlElement> createXml() const;
    /** A saved member (from createXml or a single <PLUGIN> description). */
    struct Saved
    {
        juce::PluginDescription description;
        bool bypassed = false;
        juce::MemoryBlock state;
    };
    /** Reads a <PLUGINCHAIN> element; false if it isn't one or a member's description is unreadable. */
    static bool parseXml (const juce::XmlElement&, std::vector<Saved>& members);

    /** Message thread: restores the members' states and bypass from `saved`
        when it holds the same plugins in the same order (else false, unchanged). */
    bool restore (const std::vector<Saved>& saved);

    //==============================================================================
    const juce::String getName() const override;
    void fillInPluginDescription (juce::PluginDescription&) const override;
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    using juce::AudioPluginInstance::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    double getTailLengthSeconds() const override;
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    /** The editors are shown by the UI's chain window, side by side. */
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

private:
    void fail (int member, FaultKind) noexcept;

    const std::vector<MemberPtr> members;

    // Audio thread.
    juce::AudioBuffer<float> scratch;
    int maxChannels = 2;
    bool lastBlockFailed = false;

    std::atomic<int> faultMember { -1 };
    std::atomic<FaultKind> faultKind { FaultKind::none };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginChain)
};

} // namespace livedaw
