#include "engine/ConsoleState.h"

namespace livedaw::state
{

namespace
{
    template <typename T>
    void setAttr (juce::XmlElement& e, const char* name, const std::atomic<T>& value)
    {
        if constexpr (std::is_same_v<T, bool>)
            e.setAttribute (name, value.load() ? 1 : 0);
        else if constexpr (std::is_same_v<T, int>)
            e.setAttribute (name, value.load());
        else
            e.setAttribute (name, static_cast<double> (value.load()));
    }

    template <typename T>
    void getAttr (const juce::XmlElement& e, const char* name, std::atomic<T>& value)
    {
        if (! e.hasAttribute (name))
            return;

        if constexpr (std::is_same_v<T, bool>)
            value = e.getBoolAttribute (name);
        else if constexpr (std::is_same_v<T, int>)
            value = e.getIntAttribute (name);
        else
            value = static_cast<float> (e.getDoubleAttribute (name));
    }

    /** Sets a continuous value, or queues it for a fade. */
    void getFadable (const juce::XmlElement& e, const char* name, std::atomic<float>& value,
                     std::vector<FadeTarget>* fades)
    {
        if (! e.hasAttribute (name))
            return;

        const auto target = static_cast<float> (e.getDoubleAttribute (name));

        if (fades != nullptr)
            fades->push_back ({ &value, target });
        else
            value = target;
    }

    enum class PluginMode
    {
        Instantiate, // show load: create the plugins
        StateOnly,   // scene/snippet recall: only touch plugins that are already there
        Skip         // safe mode
    };

    std::unique_ptr<juce::XmlElement> captureStrip (const Strip& s, juce::uint32 sections)
    {
        auto e = std::make_unique<juce::XmlElement> ("Strip");
        const auto& p = s.params;

        e->setAttribute ("ref", StripRef::of (s).toString());
        e->setAttribute ("sections", static_cast<int> (sections));

        if (sections & Name)
        {
            e->setAttribute ("name", s.name);
            e->setAttribute ("colour", s.colour.toString());
        }

        if (sections & Routing)
        {
            setAttr (*e, "stereo", p.stereo);
            setAttr (*e, "inL", p.inputLeft);
            setAttr (*e, "inR", p.inputRight);
            setAttr (*e, "outL", p.outputLeft);
            setAttr (*e, "outR", p.outputRight);
            setAttr (*e, "mainAssign", p.mainAssign);
        }

        if (sections & Preamp)
        {
            setAttr (*e, "trimDb", p.trimDb);
            setAttr (*e, "polarity", p.polarityInvert);
            setAttr (*e, "hpf", p.hpfEnabled);
            setAttr (*e, "hpfFreq", p.hpfFrequency);
        }

        if (sections & Fader)
        {
            setAttr (*e, "faderDb", p.faderDb);
            setAttr (*e, "pan", p.pan);
            setAttr (*e, "mute", p.mute);
        }

        if (sections & Gate)
        {
            auto* gate = e->createNewChildElement ("Gate");
            setAttr (*gate, "on", p.gate.enabled);
            setAttr (*gate, "threshold", p.gate.thresholdDb);
            setAttr (*gate, "range", p.gate.rangeDb);
            setAttr (*gate, "attack", p.gate.attackMs);
            setAttr (*gate, "hold", p.gate.holdMs);
            setAttr (*gate, "release", p.gate.releaseMs);
        }

        if (sections & Eq)
        {
            auto* eq = e->createNewChildElement ("Eq");
            setAttr (*eq, "on", p.eq.enabled);
            for (size_t i = 0; i < p.eq.bands.size(); ++i)
            {
                auto* band = eq->createNewChildElement ("Band");
                band->setAttribute ("index", static_cast<int> (i));
                setAttr (*band, "type", p.eq.bands[i].type);
                setAttr (*band, "freq", p.eq.bands[i].frequency);
                setAttr (*band, "gain", p.eq.bands[i].gainDb);
                setAttr (*band, "q", p.eq.bands[i].q);
                setAttr (*band, "on", p.eq.bands[i].enabled);
            }
        }

        if (sections & Comp)
        {
            auto* comp = e->createNewChildElement ("Comp");
            setAttr (*comp, "on", p.comp.enabled);
            setAttr (*comp, "threshold", p.comp.thresholdDb);
            setAttr (*comp, "ratio", p.comp.ratio);
            setAttr (*comp, "attack", p.comp.attackMs);
            setAttr (*comp, "release", p.comp.releaseMs);
            setAttr (*comp, "knee", p.comp.kneeDb);
            setAttr (*comp, "makeup", p.comp.makeupDb);
        }

        if ((sections & Sends) && s.getKind() == StripKind::Input)
        {
            auto* sends = e->createNewChildElement ("Sends");
            for (size_t slot = 0; slot < p.sends.size(); ++slot)
            {
                const auto& send = p.sends[slot];
                auto* se = sends->createNewChildElement ("Send");
                se->setAttribute ("slot", static_cast<int> (slot));
                setAttr (*se, "on", send.enabled);
                setAttr (*se, "level", send.levelDb);
                setAttr (*se, "pre", send.preFader);
            }
        }

        if (sections & Inserts)
        {
            auto* inserts = e->createNewChildElement ("Inserts");
            for (int slot = 0; slot < kNumInsertSlots; ++slot)
            {
                auto* plugin = s.inserts.getPlugin (slot);
                auto* ie = inserts->createNewChildElement ("Insert");
                ie->setAttribute ("slot", slot);

                if (plugin == nullptr)
                    continue;

                ie->setAttribute ("bypass", s.inserts.isBypassed (slot) ? 1 : 0);
                ie->addChildElement (plugin->getPluginDescription().createXml().release());

                juce::MemoryBlock data;
                plugin->getStateInformation (data);
                ie->createNewChildElement ("State")->addTextElement (data.toBase64Encoding());
            }
        }

        return e;
    }

    void restorePluginState (juce::AudioPluginInstance& plugin, const juce::XmlElement& insert)
    {
        if (auto* data = insert.getChildByName ("State"))
        {
            juce::MemoryBlock block;
            if (block.fromBase64Encoding (data->getAllSubText()))
                plugin.setStateInformation (block.getData(), static_cast<int> (block.getSize()));
        }
    }

    void applyStrip (Strip& s, const juce::XmlElement& e, juce::uint32 requested, PluginMode pluginMode,
                     MixerEngine& engine, PluginHost* host, std::vector<FadeTarget>* fades, ApplyResult& result)
    {
        // Only touch what was both captured and requested.
        const auto sections = requested & static_cast<juce::uint32> (e.getIntAttribute ("sections", static_cast<int> (AllSections)));
        auto& p = s.params;

        if (sections & Name)
        {
            s.name = e.getStringAttribute ("name", s.name);
            if (e.hasAttribute ("colour"))
                s.colour = juce::Colour::fromString (e.getStringAttribute ("colour"));
        }

        if (sections & Routing)
        {
            if (s.getKind() != StripKind::Master)
                getAttr (e, "stereo", p.stereo);
            getAttr (e, "inL", p.inputLeft);
            getAttr (e, "inR", p.inputRight);
            getAttr (e, "outL", p.outputLeft);
            getAttr (e, "outR", p.outputRight);
            getAttr (e, "mainAssign", p.mainAssign);
        }

        if (sections & Preamp)
        {
            getAttr (e, "trimDb", p.trimDb);
            getAttr (e, "polarity", p.polarityInvert);
            getAttr (e, "hpf", p.hpfEnabled);
            getAttr (e, "hpfFreq", p.hpfFrequency);
        }

        if (sections & Fader)
        {
            getFadable (e, "faderDb", p.faderDb, fades);
            getFadable (e, "pan", p.pan, fades);
            getAttr (e, "mute", p.mute);
        }

        if (auto* gate = e.getChildByName ("Gate"); gate != nullptr && (sections & Gate))
        {
            getAttr (*gate, "on", p.gate.enabled);
            getAttr (*gate, "threshold", p.gate.thresholdDb);
            getAttr (*gate, "range", p.gate.rangeDb);
            getAttr (*gate, "attack", p.gate.attackMs);
            getAttr (*gate, "hold", p.gate.holdMs);
            getAttr (*gate, "release", p.gate.releaseMs);
        }

        if (auto* eq = e.getChildByName ("Eq"); eq != nullptr && (sections & Eq))
        {
            getAttr (*eq, "on", p.eq.enabled);
            for (auto* band : eq->getChildWithTagNameIterator ("Band"))
            {
                const int i = band->getIntAttribute ("index", -1);
                if (! juce::isPositiveAndBelow (i, static_cast<int> (p.eq.bands.size())))
                    continue;

                auto& b = p.eq.bands[(size_t) i];
                getAttr (*band, "type", b.type);
                getAttr (*band, "freq", b.frequency);
                getAttr (*band, "gain", b.gainDb);
                getAttr (*band, "q", b.q);
                getAttr (*band, "on", b.enabled);
            }
        }

        if (auto* comp = e.getChildByName ("Comp"); comp != nullptr && (sections & Comp))
        {
            getAttr (*comp, "on", p.comp.enabled);
            getAttr (*comp, "threshold", p.comp.thresholdDb);
            getAttr (*comp, "ratio", p.comp.ratio);
            getAttr (*comp, "attack", p.comp.attackMs);
            getAttr (*comp, "release", p.comp.releaseMs);
            getAttr (*comp, "knee", p.comp.kneeDb);
            getAttr (*comp, "makeup", p.comp.makeupDb);
        }

        if (auto* sends = e.getChildByName ("Sends"); sends != nullptr && (sections & Sends))
        {
            for (auto* se : sends->getChildWithTagNameIterator ("Send"))
            {
                const int slot = se->getIntAttribute ("slot", -1);
                if (! juce::isPositiveAndBelow (slot, kMaxBuses))
                    continue;

                auto& send = p.sends[(size_t) slot];
                getAttr (*se, "on", send.enabled);
                getFadable (*se, "level", send.levelDb, fades);
                getAttr (*se, "pre", send.preFader);
            }
        }

        if (auto* inserts = e.getChildByName ("Inserts"); inserts != nullptr && (sections & Inserts)
                                                         && pluginMode != PluginMode::Skip)
        {
            for (auto* ie : inserts->getChildWithTagNameIterator ("Insert"))
            {
                const int slot = ie->getIntAttribute ("slot", -1);
                if (! juce::isPositiveAndBelow (slot, kNumInsertSlots))
                    continue;

                auto* descXml = ie->getChildByName ("PLUGIN");
                juce::PluginDescription desc;
                const bool hasPlugin = descXml != nullptr && desc.loadFromXml (*descXml);

                if (pluginMode == PluginMode::StateOnly)
                {
                    auto* current = s.inserts.getPlugin (slot);

                    if (! hasPlugin)
                        continue; // the slot was empty when captured: leave whatever is there

                    if (current == nullptr
                        || current->getPluginDescription().createIdentifierString() != desc.createIdentifierString())
                    {
                        result.warnings.add (s.name + " insert " + juce::String (slot + 1) + ": expected "
                                             + desc.name + ", not recalled");
                        continue;
                    }

                    restorePluginState (*current, *ie);
                    s.inserts.setBypassed (slot, ie->getBoolAttribute ("bypass"));
                    continue;
                }

                // Instantiate (show load)
                if (! hasPlugin || host == nullptr)
                    continue;

                juce::String error;
                auto instance = host->createInstance (desc, engine.getSampleRate(), engine.getBlockSize(), error);

                if (instance == nullptr)
                {
                    result.warnings.add (s.name + ": " + desc.name + " (" + error + ")");
                    continue;
                }

                restorePluginState (*instance, *ie);
                s.inserts.setPlugin (slot, std::move (instance));
                s.inserts.setBypassed (slot, ie->getBoolAttribute ("bypass"));
            }
        }

        ++result.stripsApplied;
    }

    bool contains (const std::vector<StripRef>* only, const StripRef& ref)
    {
        return only == nullptr || std::find (only->begin(), only->end(), ref) != only->end();
    }

    void captureMonitor (const MixerEngine& engine, juce::XmlElement& parent)
    {
        auto* monitor = parent.createNewChildElement ("Monitor");
        setAttr (*monitor, "afl", engine.soloAfl);
        setAttr (*monitor, "levelDb", engine.monitorLevelDb);
        setAttr (*monitor, "outL", engine.monitorOutputLeft);
        setAttr (*monitor, "outR", engine.monitorOutputRight);
    }

    void applyMonitor (MixerEngine& engine, const juce::XmlElement& parent)
    {
        if (auto* monitor = parent.getChildByName ("Monitor"))
        {
            getAttr (*monitor, "afl", engine.soloAfl);
            getAttr (*monitor, "levelDb", engine.monitorLevelDb);
            getAttr (*monitor, "outL", engine.monitorOutputLeft);
            getAttr (*monitor, "outR", engine.monitorOutputRight);
        }
    }
} // namespace

//==============================================================================
Section sectionAt (int index) noexcept
{
    return static_cast<Section> (1u << juce::jlimit (0, kNumSections - 1, index));
}

const char* sectionName (Section s) noexcept
{
    switch (s)
    {
        case Name:        return "Name";
        case Routing:     return "Routing";
        case Preamp:      return "Preamp";
        case Gate:        return "Gate";
        case Eq:          return "EQ";
        case Comp:        return "Comp";
        case Inserts:     return "Inserts";
        case Sends:       return "Sends";
        case Fader:       return "Fader";
        case AllSections: return "All";
    }
    return "";
}

juce::String StripRef::toString() const
{
    return juce::String (livedaw::toString (kind)) + ":" + juce::String (id);
}

StripRef StripRef::fromString (const juce::String& s)
{
    return { stripKindFromString (s.upToFirstOccurrenceOf (":", false, false)),
             s.fromFirstOccurrenceOf (":", false, false).getIntValue() };
}

StripRef StripRef::of (const Strip& s) noexcept
{
    return { s.getKind(), s.getId() };
}

Strip* findStrip (MixerEngine& engine, const StripRef& ref) noexcept
{
    switch (ref.kind)
    {
        case StripKind::Master:
            return &engine.getMaster();

        case StripKind::Aux:
        case StripKind::Group:
        {
            auto* bus = engine.getBusInSlot (ref.id);
            return bus != nullptr && bus->getKind() == ref.kind ? bus : nullptr;
        }

        case StripKind::Input:
            for (auto& s : engine.getInputs())
                if (s->getId() == ref.id)
                    return s.get();
            return nullptr;
    }

    return nullptr;
}

//==============================================================================
std::unique_ptr<juce::XmlElement> capture (const MixerEngine& engine, juce::uint32 sections,
                                           const std::vector<StripRef>* only)
{
    auto state = std::make_unique<juce::XmlElement> ("State");
    state->setAttribute ("sections", static_cast<int> (sections));

    auto add = [&] (const Strip& s)
    {
        if (contains (only, StripRef::of (s)))
            state->addChildElement (captureStrip (s, sections).release());
    };

    add (engine.getMaster());
    for (auto& b : engine.getBuses())
        add (*b);
    for (auto& in : engine.getInputs())
        add (*in);

    if ((sections & Routing) && contains (only, StripRef::of (engine.getMaster())))
        captureMonitor (engine, *state);

    return state;
}

ApplyResult apply (MixerEngine& engine, const juce::XmlElement& state, juce::uint32 sections,
                   const std::vector<StripRef>* only, bool respectRecallSafe,
                   std::vector<FadeTarget>* fades)
{
    ApplyResult result;

    for (auto* e : state.getChildWithTagNameIterator ("Strip"))
    {
        const auto ref = StripRef::fromString (e->getStringAttribute ("ref"));

        if (! contains (only, ref))
            continue;

        auto* strip = findStrip (engine, ref);

        if (strip == nullptr)
        {
            ++result.stripsMissing;
            continue;
        }

        if (respectRecallSafe && strip->params.recallSafe.load())
        {
            ++result.stripsSkippedSafe;
            continue;
        }

        applyStrip (*strip, *e, sections, PluginMode::StateOnly, engine, nullptr, fades, result);
    }

    if ((sections & Routing) && contains (only, StripRef::of (engine.getMaster()))
        && ! (respectRecallSafe && engine.getMaster().params.recallSafe.load()))
        applyMonitor (engine, state);

    return result;
}

//==============================================================================
std::unique_ptr<juce::XmlElement> saveConsole (const MixerEngine& engine)
{
    auto console = std::make_unique<juce::XmlElement> ("Console");

    auto save = [] (const Strip& s)
    {
        auto e = captureStrip (s, AllSections);
        e->setAttribute ("kind", livedaw::toString (s.getKind()));
        e->setAttribute ("id", s.getId());
        e->setAttribute ("recallSafe", s.params.recallSafe.load() ? 1 : 0);
        return e;
    };

    captureMonitor (engine, *console);
    setAttr (*console, "outputProtection", engine.outputProtection);
    setAttr (*console, "outputCeilingDb", engine.outputCeilingDb);

    console->createNewChildElement ("Master")->addChildElement (save (engine.getMaster()).release());

    auto* buses = console->createNewChildElement ("Buses");
    for (auto& b : engine.getBuses())
        buses->addChildElement (save (*b).release());

    auto* inputs = console->createNewChildElement ("Inputs");
    for (auto& in : engine.getInputs())
        inputs->addChildElement (save (*in).release());

    return console;
}

juce::Result loadConsole (MixerEngine& engine, const juce::XmlElement& console, PluginHost& host, bool loadPlugins)
{
    engine.reset();
    ApplyResult result;
    const auto mode = loadPlugins ? PluginMode::Instantiate : PluginMode::Skip;

    applyMonitor (engine, console);
    getAttr (console, "outputProtection", engine.outputProtection);
    getAttr (console, "outputCeilingDb", engine.outputCeilingDb);

    auto load = [&] (Strip& s, const juce::XmlElement& e)
    {
        applyStrip (s, e, AllSections, mode, engine, &host, nullptr, result);
        s.params.recallSafe = e.getBoolAttribute ("recallSafe");

        // Sessions from before per-channel width: a right input meant stereo.
        if (s.getKind() == StripKind::Input && ! e.hasAttribute ("stereo"))
            s.params.stereo = e.getIntAttribute ("inR", -1) >= 0;
    };

    if (auto* master = console.getChildByName ("Master"))
        if (auto* e = master->getChildByName ("Strip"))
            load (engine.getMaster(), *e);

    if (auto* buses = console.getChildByName ("Buses"))
    {
        for (auto* e : buses->getChildWithTagNameIterator ("Strip"))
        {
            const auto kind = stripKindFromString (e->getStringAttribute ("kind"));
            if (kind != StripKind::Aux && kind != StripKind::Group)
                continue;

            if (auto* bus = engine.addBus (kind, e->getIntAttribute ("id", -1)))
                load (*bus, *e);
            else
                result.warnings.add ("Bus could not be created (all slots in use)");
        }
    }

    if (auto* inputs = console.getChildByName ("Inputs"))
        for (auto* e : inputs->getChildWithTagNameIterator ("Strip"))
            load (*engine.addInput (e->getIntAttribute ("id", -1)), *e);

    if (! result.warnings.isEmpty())
        return juce::Result::fail ("Some plugins could not be loaded:\n" + result.warnings.joinIntoString ("\n"));

    return juce::Result::ok();
}

} // namespace livedaw::state
