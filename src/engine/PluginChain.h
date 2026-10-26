#pragma once

#include "engine/MonoBuses.h"

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

    Audio: the members run in order on one buffer, skipping bypassed and
    faulted ones (a member's flags are atomics shared by the chains holding it).
    It never allocates or locks. In a chain of several, a member that throws,
    outputs NaN/inf or (sandboxed) misses its block or loses its helper is
    marked faulted and passed through: its input goes on to the next member, the
    rest of the chain keeps running and the slot doesn't fault. It stays out
    until cleared (clearFault, or bypassing it on or off); takeMemberFault()
    reports it once. A chain of one fails the block instead, like a plugin on
    its own: didLastBlockFail() makes FxSlots report a slot fault (the engine
    auto-bypasses the slot) and takeFault() names the member.

    The latency is the sum of the members' that are neither bypassed nor
    faulted (each may change it at any time, the chain follows; a fault is
    followed by the next updateLatency() on the message thread). Members may be
    SandboxedPlugins: a sandboxed member whose helper died restarts it itself.

    The state (getStateInformation, createXml) is every member's description,
    bypass, state and chain window view (zoom, collapsed) as a <PLUGINCHAIN>
    element, which shows store in place of a single <PLUGIN>. Faults aren't
    saved. */
class PluginChain final : public juce::AudioPluginInstance
{
public:
    static constexpr int kMaxMembers = 8;
    static constexpr const char* kXmlTag = "PLUGINCHAIN";

    enum class FaultKind
    {
        none,
        threw,
        nonFinite,
        sandbox // the sandboxed member missed its block or lost its helper
    };

    /** How the chain window shows a member (message thread; saved with the show). */
    struct View
    {
        int zoom = 100; // percent: one of kZoomSteps
        bool collapsed = false;

        bool isDefault() const noexcept { return zoom == 100 && ! collapsed; }
        bool operator== (const View& o) const noexcept { return zoom == o.zoom && collapsed == o.collapsed; }
        bool operator!= (const View& o) const noexcept { return ! operator== (o); }
    };
    static constexpr int kZoomSteps[] = { 50, 67, 75, 85, 100 };
    /** The nearest step to `percent`. */
    static int snapZoom (int percent) noexcept;

    /** A plugin in a chain, shared by the chains that hold it. */
    struct Member final : private juce::AudioProcessorListener
    {
        explicit Member (std::shared_ptr<juce::AudioPluginInstance>);
        ~Member() override;

        std::shared_ptr<juce::AudioPluginInstance> plugin;
        SandboxedPlugin* sandboxed = nullptr;
        int numChannels = 2; // the plugin's buffer: at least stereo, more for side-chain buses
        MonoBuses mono;      // a mono main input or output (see MonoBuses)
        std::atomic<bool> bypassed { false };
        /** Set by the audio thread when it failed in a chain of several (it's then
            passed through); cleared by the user. Not saved. */
        std::atomic<FaultKind> fault { FaultKind::none };
        std::atomic<bool> faultUnreported { false };
        View view; // message thread

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
    /** Any thread: a bypassed member is skipped and adds no latency. Clears its fault. */
    void setBypassed (int index, bool);

    bool isFaulted (int index) const noexcept { return members[(size_t) index]->fault.load() != FaultKind::none; }
    FaultKind getFaultKind (int index) const noexcept { return members[(size_t) index]->fault.load(); }
    /** Any thread: un-faults a member (it runs again from the next block). */
    void clearFault (int index);
    int getNumFaulted() const noexcept;

    const View& getView (int index) const noexcept { return members[(size_t) index]->view; }
    void setView (int index, View);
    /** Shows must store it as a chain: several members, or one bypassed or with a view of its own. */
    bool needsChainFormat() const noexcept;

    bool hasSandboxedMember() const noexcept;

    /** Any thread: works the latency out again (the members' latencies may have
        changed without telling their host) and reports a change. */
    void updateLatency();

    /** "TAL-Reverb + Pro-Q 3 + Limiter" (getName() shortens it for three or more). */
    juce::String describeMembers() const;

    /** Audio thread: the last processBlock() failed (a chain of one; see takeFault()). */
    bool didLastBlockFail() const noexcept { return lastBlockFailed; }

    struct Fault
    {
        int member = -1;
        FaultKind kind = FaultKind::none;
    };
    /** Message thread: the member of a chain of one that failed a block since the last call (then forgotten). */
    Fault takeFault() noexcept;
    /** Message thread: a member of a chain of several that faulted and hasn't been
        reported yet (member -1: none). Call until none to report them all. */
    Fault takeMemberFault() noexcept;
    /** "output NaN/Inf", "threw an exception"... */
    static juce::String describeFault (FaultKind);

    /** The chain as <PLUGINCHAIN name=.. views=1><MEMBER bypass=.. zoom=.. collapsed=..><PLUGIN ../><STATE>base64</STATE></MEMBER>..</PLUGINCHAIN>
        (zoom and collapsed only when not the default; `views` says every member's
        view is stored, so a missing one is the default). A member that throws
        while saving its state is stored without one. */
    std::unique_ptr<juce::XmlElement> createXml() const;
    /** The attribute a single plugin's <PLUGIN> description carries when it was
        saved with the default chain window view (a plugin with any other view is
        saved as a chain), so a recall can put the view back. */
    static constexpr const char* kDefaultViewAttribute = "sidecarView";
    /** A saved member (from createXml or a single <PLUGIN> description). */
    struct Saved
    {
        juce::PluginDescription description;
        bool bypassed = false;
        View view;
        /** Whether `view` was saved (scenes and shows from before it weren't:
            a recall then keeps the current view). */
        bool hasView = false;
        juce::MemoryBlock state;
    };
    /** Reads a <PLUGINCHAIN> element; false if it isn't one or a member's description is unreadable. */
    static bool parseXml (const juce::XmlElement&, std::vector<Saved>& members);

    /** Message thread: restores the members' states, bypass and (saved) views from `saved`
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
    juce::AudioBuffer<float> dry; // a member's input, put back if it fails
    int maxChannels = 2;
    bool lastBlockFailed = false;

    std::atomic<int> faultMember { -1 };
    std::atomic<FaultKind> faultKind { FaultKind::none };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginChain)
};

} // namespace livedaw
