#include "engine/PluginChain.h"
#include "engine/SandboxedPlugin.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace livedaw
{

namespace
{
    bool allFinite (const float* data, int n) noexcept
    {
        // As the engine checks: inf * 0 and NaN * 0 are NaN, and NaN poisons the
        // sum, so one branch per block.
        float acc = 0.0f;
        for (int i = 0; i < n; ++i)
            acc += data[i] * 0.0f;
        return acc == 0.0f;
    }
} // namespace

int PluginChain::snapZoom (int percent) noexcept
{
    int best = 100;
    for (const int step : kZoomSteps)
        if (std::abs (step - percent) < std::abs (best - percent))
            best = step;
    return best;
}

juce::String PluginChain::describeFault (FaultKind kind)
{
    switch (kind)
    {
        case FaultKind::threw:     return "threw an exception";
        case FaultKind::nonFinite: return "output NaN/Inf";
        case FaultKind::sandbox:   return "missed its block or lost its helper in the sandbox";
        case FaultKind::none:      break;
    }
    return {};
}

//==============================================================================
PluginChain::Member::Member (std::shared_ptr<juce::AudioPluginInstance> p) : plugin (std::move (p))
{
    sandboxed = dynamic_cast<SandboxedPlugin*> (plugin.get());
    numChannels = juce::jmax (2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels());
    plugin->addListener (this);
}

PluginChain::Member::~Member()
{
    plugin->removeListener (this);
}

void PluginChain::Member::audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& details)
{
    // Any thread (a plugin may change its latency inside processBlock).
    if (details.latencyChanged)
        if (auto* chain = owner.load())
            chain->updateLatency();
}

PluginChain::MemberPtr PluginChain::makeMember (std::shared_ptr<juce::AudioPluginInstance> plugin, bool bypassed,
                                                double preparedRate, int preparedBlock)
{
    jassert (plugin != nullptr);
    if (preparedRate <= 0.0)
        configureForStereo (*plugin); // not while it runs
    auto m = std::make_shared<Member> (std::move (plugin));
    m->bypassed = bypassed;
    m->preparedRate = preparedRate;
    m->preparedBlock = preparedBlock;
    return m;
}

void PluginChain::configureForStereo (juce::AudioPluginInstance& plugin)
{
    auto layout = plugin.getBusesLayout();
    if (layout.inputBuses.size() > 0)
        layout.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();
    if (layout.outputBuses.size() > 0)
        layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
    if (plugin.checkBusesLayoutSupported (layout))
        plugin.setBusesLayout (layout);
}

//==============================================================================
PluginChain::PluginChain (std::vector<MemberPtr> m)
    : juce::AudioPluginInstance (BusesProperties()
                                     .withInput ("In", juce::AudioChannelSet::stereo())
                                     .withOutput ("Out", juce::AudioChannelSet::stereo())),
      members (std::move (m))
{
    jassert (! members.empty() && (int) members.size() <= kMaxMembers);
    for (auto& member : members)
    {
        member->owner = this;
        maxChannels = juce::jmax (maxChannels, member->numChannels);
    }
    updateLatency();
}

PluginChain::~PluginChain()
{
    for (auto& member : members)
    {
        auto* self = this;
        member->owner.compare_exchange_strong (self, nullptr);
    }
}

void PluginChain::setBypassed (int index, bool bypassed)
{
    if (! juce::isPositiveAndBelow (index, size()))
        return;
    auto& m = *members[(size_t) index];
    m.bypassed = bypassed;
    m.fault = FaultKind::none; // BYP on or off clears a fault
    m.faultUnreported = false;
    updateLatency();
}

void PluginChain::clearFault (int index)
{
    if (! juce::isPositiveAndBelow (index, size()))
        return;
    auto& m = *members[(size_t) index];
    m.fault = FaultKind::none;
    m.faultUnreported = false;
    updateLatency();
}

int PluginChain::getNumFaulted() const noexcept
{
    return (int) std::count_if (members.begin(), members.end(), [] (auto& m) { return m->fault.load() != FaultKind::none; });
}

void PluginChain::setView (int index, View view)
{
    if (juce::isPositiveAndBelow (index, size()))
        members[(size_t) index]->view = { snapZoom (view.zoom), view.collapsed };
}

bool PluginChain::needsChainFormat() const noexcept
{
    return size() > 1 || members.front()->bypassed.load() || ! members.front()->view.isDefault();
}

bool PluginChain::hasSandboxedMember() const noexcept
{
    return std::any_of (members.begin(), members.end(), [] (auto& m) { return m->sandboxed != nullptr; });
}

void PluginChain::updateLatency()
{
    int total = 0;
    for (auto& m : members)
        if (! m->bypassed.load() && m->fault.load() == FaultKind::none)
            total += m->plugin->getLatencySamples();
    setLatencySamples (total); // tells FxSlots when it changes
}

juce::String PluginChain::describeMembers() const
{
    juce::StringArray names;
    for (auto& m : members)
        names.add (m->plugin->getName());
    return names.joinIntoString (" + ");
}

const juce::String PluginChain::getName() const
{
    // The EFFECTS tiles and the slot lists show this: "A", "A + B", "A +2".
    if (size() <= 2)
        return describeMembers();
    return members.front()->plugin->getName() + " +" + juce::String (size() - 1);
}

void PluginChain::fillInPluginDescription (juce::PluginDescription& d) const
{
    if (size() == 1)
    {
        d = members.front()->plugin->getPluginDescription();
        return;
    }
    d = {};
    d.name = getName();
    d.descriptiveName = describeMembers();
    d.pluginFormatName = "Chain";
    d.category = "Effect";
    d.manufacturerName = juce::String (size()) + " plugins";
    d.fileOrIdentifier = "sidecar-chain";
    d.numInputChannels = d.numOutputChannels = 2;
}

//==============================================================================
void PluginChain::prepareToPlay (double sampleRate, int maxBlock)
{
    // Members shared with the chain this one replaces may be running: those
    // already prepared for these settings are left alone.
    for (auto& m : members)
    {
        if (m->preparedRate == sampleRate && m->preparedBlock == maxBlock)
            continue;
        m->plugin->setRateAndBufferSizeDetails (sampleRate, maxBlock);
        m->plugin->prepareToPlay (sampleRate, maxBlock);
        m->preparedRate = sampleRate;
        m->preparedBlock = maxBlock;
    }
    scratch.setSize (maxChannels, juce::jmax (1, maxBlock), false, true, false);
    dry.setSize (2, juce::jmax (1, maxBlock), false, true, false);
    updateLatency(); // plugins may change it in prepareToPlay
}

void PluginChain::releaseResources()
{
    for (auto& m : members)
    {
        m->plugin->releaseResources();
        m->preparedRate = 0.0;
        m->preparedBlock = 0;
    }
}

bool PluginChain::isBusesLayoutSupported (const BusesLayout& layout) const
{
    return layout.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

double PluginChain::getTailLengthSeconds() const
{
    double tail = 0.0;
    for (auto& m : members)
        if (! m->bypassed.load() && m->fault.load() == FaultKind::none)
            tail += m->plugin->getTailLengthSeconds();
    return tail;
}

void PluginChain::fail (int member, FaultKind kind) noexcept
{
    lastBlockFailed = true;
    int none = -1;
    if (faultMember.compare_exchange_strong (none, member))
        faultKind = kind;
}

PluginChain::Fault PluginChain::takeFault() noexcept
{
    Fault f;
    f.kind = faultKind.load();
    f.member = faultMember.exchange (-1);
    return f;
}

PluginChain::Fault PluginChain::takeMemberFault() noexcept
{
    for (int i = 0; i < size(); ++i)
    {
        auto& m = *members[(size_t) i];
        if (m.faultUnreported.exchange (false))
            if (const auto kind = m.fault.load(); kind != FaultKind::none)
                return { i, kind };
    }
    return {};
}

void PluginChain::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    lastBlockFailed = false;
    const int n = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    if (n == 0 || numChannels == 0)
        return;
    if (n > scratch.getNumSamples())
    {
        jassertfalse; // not prepared for this block size
        return;
    }

    // The members run on the chain's own buffer, wide enough for the widest.
    scratch.copyFrom (0, 0, buffer, 0, 0, n);
    scratch.copyFrom (1, 0, buffer, juce::jmin (1, numChannels - 1), 0, n);

    // In a chain of several a failing member bypasses only itself.
    const bool isolate = size() > 1;
    for (int i = 0; i < size(); ++i)
    {
        auto& m = *members[(size_t) i];
        if (m.bypassed.load (std::memory_order_relaxed) || m.fault.load (std::memory_order_relaxed) != FaultKind::none)
            continue;

        if (isolate)
            for (int ch = 0; ch < 2; ++ch)
                dry.copyFrom (ch, 0, scratch, ch, 0, n);
        for (int ch = 2; ch < m.numChannels; ++ch)
            scratch.clear (ch, 0, n); // side-chain inputs are silent
        juce::AudioBuffer<float> view (scratch.getArrayOfWritePointers(), m.numChannels, n);
        midi.clear();

        auto kind = FaultKind::none;
        try
        {
            m.plugin->processBlock (view, midi);
        }
        catch (...)
        {
            kind = FaultKind::threw;
        }
        if (kind == FaultKind::none && m.sandboxed != nullptr && m.sandboxed->didLastBlockFail())
            kind = FaultKind::sandbox;
        if (kind == FaultKind::none && (! allFinite (scratch.getReadPointer (0), n) || ! allFinite (scratch.getReadPointer (1), n)))
            kind = FaultKind::nonFinite;
        if (kind == FaultKind::none)
            continue;

        if (! isolate)
        {
            fail (i, kind);
            return;
        }
        // Faulted: its input goes on to the next member (the latency follows on
        // the message thread, see updateLatency()).
        m.fault.store (kind);
        m.faultUnreported.store (true);
        for (int ch = 0; ch < 2; ++ch)
            scratch.copyFrom (ch, 0, dry, ch, 0, n);
    }

    for (int ch = 0; ch < juce::jmin (2, numChannels); ++ch)
        buffer.copyFrom (ch, 0, scratch, ch, 0, n);
}

//==============================================================================
std::unique_ptr<juce::XmlElement> PluginChain::createXml() const
{
    auto xml = std::make_unique<juce::XmlElement> (kXmlTag);
    xml->setAttribute ("name", describeMembers());
    for (auto& m : members)
    {
        auto* e = xml->createNewChildElement ("MEMBER");
        e->setAttribute ("bypass", m->bypassed.load() ? 1 : 0);
        if (m->view.zoom != 100)
            e->setAttribute ("zoom", m->view.zoom);
        if (m->view.collapsed)
            e->setAttribute ("collapsed", 1);
        e->addChildElement (m->plugin->getPluginDescription().createXml().release());

        juce::MemoryBlock data;
        try
        {
            m->plugin->getStateInformation (data);
        }
        catch (...)
        {
            data.reset();
        }
        e->createNewChildElement ("STATE")->addTextElement (data.toBase64Encoding());
    }
    return xml;
}

bool PluginChain::parseXml (const juce::XmlElement& xml, std::vector<Saved>& saved)
{
    if (! xml.hasTagName (kXmlTag))
        return false;
    saved.clear();
    for (auto* e : xml.getChildWithTagNameIterator ("MEMBER"))
    {
        Saved s;
        auto* desc = e->getChildByName ("PLUGIN");
        if (desc == nullptr || ! s.description.loadFromXml (*desc))
            return false;
        s.bypassed = e->getBoolAttribute ("bypass");
        s.view.zoom = snapZoom (e->getIntAttribute ("zoom", 100)); // older shows: neither
        s.view.collapsed = e->getBoolAttribute ("collapsed");
        if (auto* state = e->getChildByName ("STATE"))
            if (const auto text = state->getAllSubText().trim(); text.isNotEmpty())
                s.state.fromBase64Encoding (text);
        saved.push_back (std::move (s));
    }
    return ! saved.empty() && (int) saved.size() <= kMaxMembers;
}

bool PluginChain::restore (const std::vector<Saved>& saved)
{
    if ((int) saved.size() != size())
        return false;
    for (int i = 0; i < size(); ++i)
        if (getMember (i).getPluginDescription().createIdentifierString() != saved[(size_t) i].description.createIdentifierString())
            return false;

    for (int i = 0; i < size(); ++i)
    {
        const auto& s = saved[(size_t) i];
        if (s.state.getSize() > 0)
            getMember (i).setStateInformation (s.state.getData(), (int) s.state.getSize());
        members[(size_t) i]->bypassed = s.bypassed;
        members[(size_t) i]->view = s.view;
    }
    updateLatency();
    return true;
}

void PluginChain::getStateInformation (juce::MemoryBlock& data)
{
    copyXmlToBinary (*createXml(), data);
}

void PluginChain::setStateInformation (const void* data, int sizeInBytes)
{
    std::vector<Saved> saved;
    if (auto xml = getXmlFromBinary (data, sizeInBytes); xml != nullptr && parseXml (*xml, saved))
        restore (saved);
}

} // namespace livedaw
