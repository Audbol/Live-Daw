#include "engine/Session.h"

namespace livedaw::session
{

namespace
{
    constexpr int kSessionVersion = 1;

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

    std::unique_ptr<juce::XmlElement> saveStrip (const Strip& s)
    {
        auto e = std::make_unique<juce::XmlElement> ("Strip");
        const auto& p = s.params;

        e->setAttribute ("kind", toString (s.getKind()));
        e->setAttribute ("id", s.getId());
        e->setAttribute ("name", s.name);
        e->setAttribute ("colour", s.colour.toString());
        setAttr (*e, "trimDb", p.trimDb);
        setAttr (*e, "polarity", p.polarityInvert);
        setAttr (*e, "hpf", p.hpfEnabled);
        setAttr (*e, "hpfFreq", p.hpfFrequency);
        setAttr (*e, "faderDb", p.faderDb);
        setAttr (*e, "pan", p.pan);
        setAttr (*e, "mute", p.mute);
        setAttr (*e, "mainAssign", p.mainAssign);
        setAttr (*e, "inL", p.inputLeft);
        setAttr (*e, "inR", p.inputRight);
        setAttr (*e, "stereo", p.stereo);
        setAttr (*e, "outL", p.outputLeft);
        setAttr (*e, "outR", p.outputRight);

        auto* gate = e->createNewChildElement ("Gate");
        setAttr (*gate, "on", p.gate.enabled);
        setAttr (*gate, "threshold", p.gate.thresholdDb);
        setAttr (*gate, "range", p.gate.rangeDb);
        setAttr (*gate, "attack", p.gate.attackMs);
        setAttr (*gate, "hold", p.gate.holdMs);
        setAttr (*gate, "release", p.gate.releaseMs);

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

        auto* comp = e->createNewChildElement ("Comp");
        setAttr (*comp, "on", p.comp.enabled);
        setAttr (*comp, "threshold", p.comp.thresholdDb);
        setAttr (*comp, "ratio", p.comp.ratio);
        setAttr (*comp, "attack", p.comp.attackMs);
        setAttr (*comp, "release", p.comp.releaseMs);
        setAttr (*comp, "knee", p.comp.kneeDb);
        setAttr (*comp, "makeup", p.comp.makeupDb);

        if (s.getKind() == StripKind::Input)
        {
            auto* sends = e->createNewChildElement ("Sends");
            for (size_t slot = 0; slot < p.sends.size(); ++slot)
            {
                const auto& send = p.sends[slot];
                if (! send.enabled && send.levelDb == 0.0f)
                    continue;

                auto* se = sends->createNewChildElement ("Send");
                se->setAttribute ("slot", static_cast<int> (slot));
                setAttr (*se, "on", send.enabled);
                setAttr (*se, "level", send.levelDb);
                setAttr (*se, "pre", send.preFader);
            }
        }

        auto* inserts = e->createNewChildElement ("Inserts");
        for (int slot = 0; slot < kNumInsertSlots; ++slot)
        {
            auto* plugin = s.inserts.getPlugin (slot);
            if (plugin == nullptr)
                continue;

            auto* ie = inserts->createNewChildElement ("Insert");
            ie->setAttribute ("slot", slot);
            ie->setAttribute ("bypass", s.inserts.isBypassed (slot) ? 1 : 0);
            ie->addChildElement (plugin->getPluginDescription().createXml().release());

            juce::MemoryBlock state;
            plugin->getStateInformation (state);
            ie->createNewChildElement ("State")->addTextElement (state.toBase64Encoding());
        }

        return e;
    }

    void loadStrip (Strip& s, const juce::XmlElement& e, MixerEngine& engine, PluginHost& host,
                    juce::StringArray& errors)
    {
        auto& p = s.params;

        s.name = e.getStringAttribute ("name", s.name);
        if (e.hasAttribute ("colour"))
            s.colour = juce::Colour::fromString (e.getStringAttribute ("colour"));

        getAttr (e, "trimDb", p.trimDb);
        getAttr (e, "polarity", p.polarityInvert);
        getAttr (e, "hpf", p.hpfEnabled);
        getAttr (e, "hpfFreq", p.hpfFrequency);
        getAttr (e, "faderDb", p.faderDb);
        getAttr (e, "pan", p.pan);
        getAttr (e, "mute", p.mute);
        getAttr (e, "mainAssign", p.mainAssign);
        getAttr (e, "inL", p.inputLeft);
        getAttr (e, "inR", p.inputRight);
        getAttr (e, "stereo", p.stereo);
        getAttr (e, "outL", p.outputLeft);
        getAttr (e, "outR", p.outputRight);

        if (auto* gate = e.getChildByName ("Gate"))
        {
            getAttr (*gate, "on", p.gate.enabled);
            getAttr (*gate, "threshold", p.gate.thresholdDb);
            getAttr (*gate, "range", p.gate.rangeDb);
            getAttr (*gate, "attack", p.gate.attackMs);
            getAttr (*gate, "hold", p.gate.holdMs);
            getAttr (*gate, "release", p.gate.releaseMs);
        }

        if (auto* eq = e.getChildByName ("Eq"))
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

        if (auto* comp = e.getChildByName ("Comp"))
        {
            getAttr (*comp, "on", p.comp.enabled);
            getAttr (*comp, "threshold", p.comp.thresholdDb);
            getAttr (*comp, "ratio", p.comp.ratio);
            getAttr (*comp, "attack", p.comp.attackMs);
            getAttr (*comp, "release", p.comp.releaseMs);
            getAttr (*comp, "knee", p.comp.kneeDb);
            getAttr (*comp, "makeup", p.comp.makeupDb);
        }

        if (auto* sends = e.getChildByName ("Sends"))
        {
            for (auto* se : sends->getChildWithTagNameIterator ("Send"))
            {
                const int slot = se->getIntAttribute ("slot", -1);
                if (! juce::isPositiveAndBelow (slot, kMaxBuses))
                    continue;

                auto& send = p.sends[(size_t) slot];
                getAttr (*se, "on", send.enabled);
                getAttr (*se, "level", send.levelDb);
                getAttr (*se, "pre", send.preFader);
            }
        }

        if (auto* inserts = e.getChildByName ("Inserts"))
        {
            for (auto* ie : inserts->getChildWithTagNameIterator ("Insert"))
            {
                const int slot = ie->getIntAttribute ("slot", -1);
                auto* descXml = ie->getChildByName ("PLUGIN");

                if (! juce::isPositiveAndBelow (slot, kNumInsertSlots) || descXml == nullptr)
                    continue;

                juce::PluginDescription desc;
                if (! desc.loadFromXml (*descXml))
                    continue;

                juce::String error;
                auto instance = host.createInstance (desc, engine.getSampleRate(), engine.getBlockSize(), error);

                if (instance == nullptr)
                {
                    errors.add (s.name + ": " + desc.name + " (" + error + ")");
                    continue;
                }

                if (auto* state = ie->getChildByName ("State"))
                {
                    juce::MemoryBlock block;
                    if (block.fromBase64Encoding (state->getAllSubText()))
                        instance->setStateInformation (block.getData(), static_cast<int> (block.getSize()));
                }

                s.inserts.setPlugin (slot, std::move (instance));
                s.inserts.setBypassed (slot, ie->getBoolAttribute ("bypass"));
            }
        }
    }
} // namespace

std::unique_ptr<juce::XmlElement> save (const MixerEngine& engine)
{
    auto root = std::make_unique<juce::XmlElement> ("LiveDAWSession");
    root->setAttribute ("version", kSessionVersion);

    auto* monitor = root->createNewChildElement ("Monitor");
    setAttr (*monitor, "afl", engine.soloAfl);
    setAttr (*monitor, "levelDb", engine.monitorLevelDb);
    setAttr (*monitor, "outL", engine.monitorOutputLeft);
    setAttr (*monitor, "outR", engine.monitorOutputRight);

    root->createNewChildElement ("Master")->addChildElement (saveStrip (engine.getMaster()).release());

    auto* buses = root->createNewChildElement ("Buses");
    for (auto& b : engine.getBuses())
        buses->addChildElement (saveStrip (*b).release());

    auto* inputs = root->createNewChildElement ("Inputs");
    for (auto& in : engine.getInputs())
        inputs->addChildElement (saveStrip (*in).release());

    return root;
}

juce::Result load (MixerEngine& engine, const juce::XmlElement& xml, PluginHost& host)
{
    if (! xml.hasTagName ("LiveDAWSession"))
        return juce::Result::fail ("Not a Live DAW session file");

    if (xml.getIntAttribute ("version") > kSessionVersion)
        return juce::Result::fail ("This session was saved by a newer version of Live DAW");

    engine.reset();
    juce::StringArray errors;

    if (auto* monitor = xml.getChildByName ("Monitor"))
    {
        getAttr (*monitor, "afl", engine.soloAfl);
        getAttr (*monitor, "levelDb", engine.monitorLevelDb);
        getAttr (*monitor, "outL", engine.monitorOutputLeft);
        getAttr (*monitor, "outR", engine.monitorOutputRight);
    }

    if (auto* master = xml.getChildByName ("Master"))
        if (auto* e = master->getChildByName ("Strip"))
            loadStrip (engine.getMaster(), *e, engine, host, errors);

    if (auto* buses = xml.getChildByName ("Buses"))
    {
        for (auto* e : buses->getChildWithTagNameIterator ("Strip"))
        {
            const auto kind = stripKindFromString (e->getStringAttribute ("kind"));
            if (kind != StripKind::Aux && kind != StripKind::Group)
                continue;

            if (auto* bus = engine.addBus (kind, e->getIntAttribute ("id", -1)))
                loadStrip (*bus, *e, engine, host, errors);
        }
    }

    if (auto* inputs = xml.getChildByName ("Inputs"))
        for (auto* e : inputs->getChildWithTagNameIterator ("Strip"))
            loadStrip (*engine.addInput(), *e, engine, host, errors);

    if (! errors.isEmpty())
        return juce::Result::fail ("Some plugins could not be loaded:\n" + errors.joinIntoString ("\n"));

    return juce::Result::ok();
}

juce::Result saveToFile (const MixerEngine& engine, const juce::File& file)
{
    auto xml = save (engine);

    if (! xml->writeTo (file))
        return juce::Result::fail ("Could not write " + file.getFullPathName());

    return juce::Result::ok();
}

juce::Result loadFromFile (MixerEngine& engine, const juce::File& file, PluginHost& host)
{
    auto xml = juce::parseXML (file);

    if (xml == nullptr)
        return juce::Result::fail ("Could not read " + file.getFullPathName());

    return load (engine, *xml, host);
}

} // namespace livedaw::session
