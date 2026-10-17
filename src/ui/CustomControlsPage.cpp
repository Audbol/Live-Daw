#include "ui/CustomControlsPage.h"
#include "engine/Models.h"
#include "ui/WingPopups.h"

namespace livedaw::ui::cc
{

namespace
{
    Function parseFunction (const juce::String& s)
    {
        juce::StringArray parts;
        parts.addTokens (s, ",", {});
        return { parts[0].getIntValue(), parts[1].getIntValue(), parts[2].getIntValue() };
    }

    juce::String functionText (const Function& f)
    {
        return juce::String (f.category) + "," + juce::String (f.target) + "," + juce::String (f.option);
    }

    // CH PAGE: the HOME tabs; GLOBAL PAGE: the screens; OTHER: single actions.
    const HomeTab pageTabs[] = { HomeTab::Home, HomeTab::Input, HomeTab::Filter, HomeTab::Gate, HomeTab::Eq, HomeTab::Dynamics,
                                 HomeTab::PreInsert, HomeTab::PostInsert, HomeTab::Main, HomeTab::Sends };
    const char* const pageNames[] = { "HOME", "INPUT", "FILTER", "GATE", "EQ", "DYNAMICS", "INSERT PRE", "INSERT POST", "MAIN", "SENDS" };
    const Screen globalScreens[] = { Screen::Home, Screen::Effects, Screen::Meters, Screen::Routing, Screen::Setup, Screen::Library,
                                     Screen::Overview, Screen::Faders, Screen::Sends, Screen::Config, Screen::MuteGroups, Screen::Recorder };
    const char* const globalNames[] = { "HOME", "EFFECTS", "METERS", "ROUTING", "SETUP", "LIBRARY", "MIXER VIEW", "FADER VIEW", "SENDS VIEW",
                                        "FADER LAYER", "MUTE GROUPS", "WING LIVE", "CUSTOM CONTROLS", "MONITOR" };
    constexpr int kNumGlobalPages = 14;
    const char* const showControls[] = { "SCENE GO", "PREV", "NEXT", "GO PREV", "GO NEXT" };
    const char* const otherNames[] = { "MONITOR DIM", "MONITOR MONO", "MONITOR SWAP", "MONITOR MUTE", "FADER -oo", "FADER -10 dB", "FADER 0 dB",
                                       "AUTOMIX X", "AUTOMIX Y", "CHANNEL PREV", "CHANNEL NEXT", "CC BANK UP", "CC BANK DOWN", "SOF OFF" };
    constexpr int kNumOthers = 14;
    const char* const monitorNames[] = { "MONITOR LEVEL", "DIM AMOUNT" };

    Strip* stripOf (UiContext& ctx, int target) { return state::findStrip (ctx.engine, decodeStrip (target)); }

    /** Every strip in CHANNEL SELECT order. */
    std::vector<Strip*> allStrips (UiContext& ctx)
    {
        std::vector<Strip*> list;
        for (auto k : { StripKind::Channel, StripKind::Aux, StripKind::Bus, StripKind::Main, StripKind::Matrix })
            for (auto& s : ctx.engine.getStrips (k))
                list.push_back (s.get());
        return list;
    }

    /** The strip after / before the selected one (CHANNEL PREV / NEXT). */
    void stepSelection (UiContext& ctx, int delta)
    {
        const auto list = allStrips (ctx);
        if (list.empty() || ! ctx.selectStrip)
            return;
        auto* sel = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr;
        auto it = std::find (list.begin(), list.end(), sel);
        const int i = it == list.end() ? 0 : (int) (it - list.begin());
        ctx.selectStrip (list[(size_t) juce::jlimit (0, (int) list.size() - 1, i + delta)]);
    }
} // namespace

//==============================================================================
Assignments::Assignments (Show& s) : show (s)
{
    reload();
}

void Assignments::reload()
{
    for (auto& l : layers)
        l = {};
    if (auto xml = juce::parseXML (show.getCustomControlsXml()))
        for (auto* e : xml->getChildWithTagNameIterator ("Control"))
        {
            const int l = e->getIntAttribute ("layer", -1), i = e->getIntAttribute ("index", -1);
            if (! juce::isPositiveAndBelow (l, kNumStoredLayers) || ! juce::isPositiveAndBelow (i, kNumControls))
                continue;
            auto& c = layers[(size_t) l][(size_t) i];
            c.encoder = parseFunction (e->getStringAttribute ("enc"));
            c.upper = parseFunction (e->getStringAttribute ("bu"));
            c.lower = parseFunction (e->getStringAttribute ("bd"));
            c.name = e->getStringAttribute ("name");
            c.colour = juce::jlimit (0, 18, e->getIntAttribute ("colour"));
        }
    ++revision;
}

const Control& Assignments::get (int layer, int index) const
{
    return layers[(size_t) juce::jlimit (0, kNumStoredLayers - 1, layer)][(size_t) juce::jlimit (0, kNumControls - 1, index)];
}

void Assignments::set (int layer, int index, const Control& c)
{
    if (! juce::isPositiveAndBelow (layer, kNumStoredLayers) || ! juce::isPositiveAndBelow (index, kNumControls))
        return;
    layers[(size_t) layer][(size_t) index] = c;
    save();
}

void Assignments::clearLayer (int layer)
{
    if (! juce::isPositiveAndBelow (layer, kNumStoredLayers))
        return;
    layers[(size_t) layer] = {};
    save();
}

void Assignments::save()
{
    juce::XmlElement xml ("CustomControls");
    for (int l = 0; l < kNumStoredLayers; ++l)
        for (int i = 0; i < kNumControls; ++i)
        {
            const auto& c = layers[(size_t) l][(size_t) i];
            if (! c.encoder.isAssigned() && ! c.upper.isAssigned() && ! c.lower.isAssigned() && c.name.isEmpty() && c.colour == 0)
                continue;
            auto* e = xml.createNewChildElement ("Control");
            e->setAttribute ("layer", l);
            e->setAttribute ("index", i);
            e->setAttribute ("name", c.name);
            e->setAttribute ("colour", c.colour);
            e->setAttribute ("enc", functionText (c.encoder));
            e->setAttribute ("bu", functionText (c.upper));
            e->setAttribute ("bd", functionText (c.lower));
        }
    show.setCustomControlsXml (xml.toString (juce::XmlElement::TextFormat().singleLine().withoutHeader()));
    ++revision;
}

//==============================================================================
int numCategories (Slot s)
{
    return s == Slot::Encoder ? (int) kNumEncoderCategories : (int) kNumButtonCategories;
}

juce::String categoryName (Slot s, int category)
{
    static const char* const encoders[] = { "NOT ASSIGNED", "FADER", "PAN", "DCA FADER", "SEL BUS SEND", "FIXED BUS SEND",
                                            "FX PARAMETER", "MONITORING", "DAW V-POTS", "MIDI CC" };
    static const char* const buttons[] = { "NOT ASSIGNED", "MUTE", "SOLO", "DCA MUTE", "MGRP", "SENDS ON FDR", "CH PAGE", "GLOBAL PAGE",
                                           "FADER PAGE", "SHOW CONTROL", "SCENE RECALL", "OTHER", "FADER SPILL", "DAW BUTTONS",
                                           "MIDI CC/NOTE", "USB PLAY/REC", "SD RECORDER", "GPIO" };
    if (s == Slot::Encoder)
        return juce::isPositiveAndBelow (category, (int) std::size (encoders)) ? encoders[category] : "";
    return juce::isPositiveAndBelow (category, (int) std::size (buttons)) ? buttons[category] : "";
}

bool categoryAvailable (Slot s, int category)
{
    if (s == Slot::Encoder)
        return category < EncDawVpot;
    return category < BtnFaderSpill;
}

juce::Colour categoryColour (Slot s, int category)
{
    const juce::Colour busRed (0xff782828), fxMagenta (0xff780048), salmon (0xffa04050), spillRed (0xffa01818), dawGold (0xff7a6a20);
    if (s == Slot::Encoder)
    {
        switch (category)
        {
            case EncSelBusSend:
            case EncFixedBusSend: return busRed;
            case EncFxParameter:  return fxMagenta;
            case EncDawVpot:      return dawGold;
            default:              return juce::Colours::black;
        }
    }
    switch (category)
    {
        case BtnSendsOnFader: return salmon;
        case BtnFaderSpill:   return spillRed;
        case BtnDawButtons:   return dawGold;
        default:              return juce::Colours::black;
    }
}

int encodeStrip (const state::StripRef& r) noexcept
{
    return (int) r.kind * 100000 + r.id;
}

state::StripRef decodeStrip (int target) noexcept
{
    state::StripRef r;
    r.kind = static_cast<StripKind> (juce::jlimit (0, kNumStripKinds - 1, target / 100000));
    r.id = target % 100000;
    return r;
}

//==============================================================================
KnobBinding bindEncoder (UiContext& ctx, const Function& f)
{
    switch (f.category)
    {
        case EncFader:
            if (auto* s = stripOf (ctx, f.target))
                return bindFader (s->getLabel(), s->params.faderDb);
            break;
        case EncPan:
            if (auto* s = stripOf (ctx, f.target))
                return bindPan ("PAN", s->params.pan);
            break;
        case EncDcaFader:
            if (juce::isPositiveAndBelow (f.target, kNumDcas))
                return bindFader ("DCA " + juce::String (f.target + 1), ctx.engine.getDca (f.target).faderDb);
            break;
        case EncSelBusSend:
        {
            // The selected strip's send to the bus (rebound when the selection changes).
            auto* s = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr;
            if (s != nullptr && (s->isInput() || s->isBus()) && juce::isPositiveAndBelow (f.target, (int) ctx.engine.getBuses().size()))
                return bindFader ("BUS " + juce::String (f.target + 1), s->sends[(size_t) f.target].levelDb);
            break;
        }
        case EncFixedBusSend:
            if (auto* s = stripOf (ctx, f.target); s != nullptr && (s->isInput() || s->isBus()) && f.option >= 0)
                return bindFader ("BUS " + juce::String (f.option + 1), s->sends[(size_t) f.option].levelDb);
            break;
        case EncFxParameter:
            if (juce::isPositiveAndBelow (f.target, kNumFx))
            {
                auto& fx = ctx.engine.getFx();
                if (f.option < 0)
                    return bindLinear ("FX MIX", fx.params (f.target).mix, 0.0f, 100.0f, "%", 0, 100.0f);
                const auto& spec = models::model (models::Fx, fx.getModel (f.target));
                if (juce::isPositiveAndBelow (f.option, (int) spec.params.size()))
                    return bindModelParam (fx.params (f.target).values[f.option], spec.params[(size_t) f.option]);
            }
            break;
        case EncMonitoring:
            return f.option == 1 ? bindLinear ("DIM", ctx.engine.monitorDimDb, 0.0f, 40.0f, " dB", 0, 20.0f)
                                 : bindLinear ("MONITOR", ctx.engine.monitorLevelDb, -60.0f, 10.0f, " dB", 1, 0.0f);
        default:
            break;
    }
    return {};
}

void press (UiContext& ctx, const Function& f)
{
    auto flip = [] (std::atomic<bool>& b) { b = ! b.load(); };
    switch (f.category)
    {
        case BtnMute:
            if (auto* s = stripOf (ctx, f.target))
                flip (s->params.mute);
            break;
        case BtnSolo:
            if (auto* s = stripOf (ctx, f.target))
                flip (s->params.solo);
            break;
        case BtnDcaMute:
            if (juce::isPositiveAndBelow (f.target, kNumDcas))
                flip (ctx.engine.getDca (f.target).mute);
            break;
        case BtnMuteGroup:
            if (juce::isPositiveAndBelow (f.target, kNumMuteGroups))
                ctx.engine.muteGroups = ctx.engine.muteGroups.load() ^ (1u << (uint32_t) f.target);
            break;
        case BtnSendsOnFader:
            // Sends on Fader with this destination fixed; again: off.
            if (auto* s = stripOf (ctx, f.target))
            {
                const auto ref = state::StripRef::of (*s);
                const bool on = ctx.sendsOnFader && ctx.sofDestination.has_value() && *ctx.sofDestination == ref;
                ctx.sendsOnFader = ! on;
                ctx.sofDestination = on ? std::nullopt : std::optional<state::StripRef> (ref);
                if (ctx.sofChanged)
                    ctx.sofChanged();
            }
            break;
        case BtnChannelPage:
            if (auto* s = stripOf (ctx, f.target))
            {
                if (ctx.selectStrip)
                    ctx.selectStrip (s);
                if (ctx.showHomeTab)
                    ctx.showHomeTab (pageTabs[juce::jlimit (0, (int) std::size (pageTabs) - 1, f.option)]);
            }
            break;
        case BtnGlobalPage:
            if (f.option == 12 && ctx.showCustomControls)
                ctx.showCustomControls (-1);
            else if (f.option == 13 && ctx.showMonitors)
                ctx.showMonitors();
            else if (ctx.showScreen && juce::isPositiveAndBelow (f.option, (int) std::size (globalScreens)))
                ctx.showScreen (globalScreens[f.option]);
            break;
        case BtnFaderPage:
        {
            const auto section = static_cast<LayerSet::Section> (juce::jlimit (0, LayerSet::kNumSections - 1, f.target));
            const auto keys = ctx.show.layers.getDefaultLayers (ctx.engine, section);
            if (juce::isPositiveAndBelow (f.option, (int) keys.size()))
            {
                ctx.show.layers.setSelectedLayerId (section, keys[(size_t) f.option].id);
                notifyLayersChangedAsync (ctx);
            }
            break;
        }
        case BtnShowControl:
            switch (f.option)
            {
                case 0: logRecall (ctx, "scene", ctx.show.recallScene (juce::jmax (0, ctx.show.getCurrentSceneIndex()))); break;
                case 1:
                case 3: logRecall (ctx, "previous scene", ctx.show.recallPrevious()); break;
                default: logRecall (ctx, "next scene", ctx.show.recallNext()); break;
            }
            break;
        case BtnSceneRecall:
            logRecall (ctx, "scene #" + juce::String (f.target), ctx.show.recallSceneByTag (f.target));
            break;
        case BtnOther:
        {
            auto* sel = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr;
            switch (f.option)
            {
                case 0: flip (ctx.engine.monitorDim); break;
                case 1: flip (ctx.engine.monitorMono); break;
                case 2: flip (ctx.engine.monitorSwap); break;
                case 3: flip (ctx.engine.monitorMute); break;
                case 4: if (sel != nullptr) sel->params.faderDb = -144.0f; break;
                case 5: if (sel != nullptr) sel->params.faderDb = -10.0f; break;
                case 6: if (sel != nullptr) sel->params.faderDb = 0.0f; break;
                case 7: flip (ctx.engine.automixEnable[0]); break;
                case 8: flip (ctx.engine.automixEnable[1]); break;
                case 9: stepSelection (ctx, -1); break;
                case 10: stepSelection (ctx, 1); break;
                case 11:
                case 12:
                    if (ctx.customControls != nullptr)
                    {
                        auto& layer = ctx.customControls->currentLayer;
                        layer = (layer + (f.option == 11 ? 1 : kNumLayers - 1)) % kNumLayers;
                        if (ctx.customControlsChanged)
                            ctx.customControlsChanged();
                    }
                    break;
                case 13:
                    ctx.sendsOnFader = false;
                    ctx.sofDestination.reset();
                    if (ctx.sofChanged)
                        ctx.sofChanged();
                    break;
                default: break;
            }
            break;
        }
        default:
            break;
    }
}

bool isLit (UiContext& ctx, const Function& f)
{
    switch (f.category)
    {
        case BtnMute:
            if (auto* s = stripOf (ctx, f.target))
                return s->params.mute.load();
            break;
        case BtnSolo:
            if (auto* s = stripOf (ctx, f.target))
                return s->params.solo.load();
            break;
        case BtnDcaMute: return juce::isPositiveAndBelow (f.target, kNumDcas) && ctx.engine.getDca (f.target).mute.load();
        case BtnMuteGroup: return juce::isPositiveAndBelow (f.target, kNumMuteGroups) && ((ctx.engine.muteGroups.load() >> f.target) & 1u) != 0;
        case BtnSendsOnFader:
            if (auto* s = stripOf (ctx, f.target))
                return ctx.sendsOnFader && ctx.sofDestination.has_value() && *ctx.sofDestination == state::StripRef::of (*s);
            break;
        case BtnGlobalPage:
            return juce::isPositiveAndBelow (f.option, (int) std::size (globalScreens)) && ctx.getScreen && ctx.getScreen() == globalScreens[f.option];
        case BtnOther:
            switch (f.option)
            {
                case 0: return ctx.engine.monitorDim.load();
                case 1: return ctx.engine.monitorMono.load();
                case 2: return ctx.engine.monitorSwap.load();
                case 3: return ctx.engine.monitorMute.load();
                case 7: return ctx.engine.automixEnable[0].load();
                case 8: return ctx.engine.automixEnable[1].load();
                default: return false;
            }
        default:
            break;
    }
    return false;
}

juce::String describe (UiContext& ctx, Slot s, const Function& f)
{
    if (! f.isAssigned())
        return {};
    auto strip = [&ctx, &f]
    {
        auto* st = stripOf (ctx, f.target);
        return st != nullptr ? st->getLabel().removeCharacters (" ") : juce::String ("--");
    };
    if (s == Slot::Encoder)
    {
        switch (f.category)
        {
            case EncFader:        return strip() + " FADER";
            case EncPan:          return strip() + " PAN";
            case EncDcaFader:     return "DCA" + juce::String (f.target + 1);
            case EncSelBusSend:   return "SEL>B" + juce::String (f.target + 1);
            case EncFixedBusSend: return strip() + ">B" + juce::String (f.option + 1);
            case EncFxParameter:  return "FX" + juce::String (f.target + 1) + (f.option < 0 ? " MIX" : " P" + juce::String (f.option + 1));
            case EncMonitoring:   return monitorNames[juce::jlimit (0, 1, f.option)];
            default:              return categoryName (s, f.category);
        }
    }
    switch (f.category)
    {
        case BtnMute:         return strip() + " MUTE";
        case BtnSolo:         return strip() + " SOLO";
        case BtnDcaMute:      return "DCA" + juce::String (f.target + 1) + " MUTE";
        case BtnMuteGroup:    return ctx.engine.getMuteGroupName (juce::jlimit (0, kNumMuteGroups - 1, f.target));
        case BtnSendsOnFader: return "SOF " + strip();
        case BtnChannelPage:  return strip() + " " + pageNames[juce::jlimit (0, (int) std::size (pageNames) - 1, f.option)];
        case BtnGlobalPage:   return globalNames[juce::jlimit (0, kNumGlobalPages - 1, f.option)];
        case BtnFaderPage:    return juce::String ("LCR")[juce::jlimit (0, 2, f.target)] + juce::String (" LAYER ") + juce::String (f.option + 1);
        case BtnShowControl:  return juce::String ("SCN ") + showControls[juce::jlimit (0, 4, f.option)];
        case BtnSceneRecall:  return "SCENE #" + juce::String (f.target);
        case BtnOther:        return otherNames[juce::jlimit (0, kNumOthers - 1, f.option)];
        default:              return categoryName (s, f.category);
    }
}

} // namespace livedaw::ui::cc

//==============================================================================
namespace livedaw::ui
{

namespace
{
    const juce::Colour unsetGrey = Colours::textDim, keyGrey = Colours::caption;

    void arrowKey (juce::Graphics& g, juce::Rectangle<int> r, bool right)
    {
        g.setColour (unsetGrey);
        g.fillRect (r);
        g.setColour (juce::Colours::black);
        juce::Path p;
        const auto c = r.getCentre().toFloat();
        const float s = (float) juce::jmin (r.getWidth(), r.getHeight()) * 0.18f;
        p.startNewSubPath (c.x + (right ? -s * 0.6f : s * 0.6f), c.y - s * 1.4f);
        p.lineTo (c.x + (right ? s * 0.8f : -s * 0.8f), c.y);
        p.lineTo (c.x + (right ? -s * 0.6f : s * 0.6f), c.y + s * 1.4f);
        g.strokePath (p, juce::PathStrokeType (2.5f));
    }

    void outlinedKey (juce::Graphics& g, juce::Rectangle<int> r, Icon icon, const juce::String& text, bool enabled = true)
    {
        const auto ink = enabled ? Dark::text : Dark::textDim.withAlpha (0.5f);
        g.setColour (ink.withAlpha (0.8f));
        g.drawRect (r.toFloat(), 1.5f);
        auto inner = r.reduced (6, 4);
        drawIcon (g, icon, inner.removeFromLeft (inner.getHeight()).toFloat().reduced (2.0f), ink);
        g.setColour (ink);
        g.setFont (juce::FontOptions (juce::jlimit (10.0f, 18.0f, (float) inner.getHeight() * 0.5f), juce::Font::bold));
        g.drawFittedText (text, inner, juce::Justification::centred, 1);
    }
} // namespace

CustomControlsPage::CustomControlsPage (UiContext& c, cc::Assignments& a, int startLayer) : ctx (c), assignments (a)
{
    layer = startLayer >= 0 ? juce::jlimit (0, cc::kNumLayers - 1, startLayer) : assignments.currentLayer;
    lists[0].title = "FUNCTION";
    anchor.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (anchor);
    rebuildLists();
    startTimerHz (5);
}

StatusTitle CustomControlsPage::getStatusTitle()
{
    // A dark tile with a knob icon and the layer number [D1].
    StatusTitle t;
    t.kind = StatusTitle::Kind::Layer;
    t.icon = Icon::CcEdit;
    t.text = juce::String (layer + 1);
    return t;
}

cc::Control CustomControlsPage::control() const
{
    return assignments.get (layer, index);
}

cc::Function CustomControlsPage::function() const
{
    return control().slot (slot);
}

void CustomControlsPage::store (const cc::Function& f)
{
    auto c = control();
    c.slot (slot) = f;
    assignments.set (layer, index, c);
    if (ctx.customControlsChanged)
        ctx.customControlsChanged();
}

void CustomControlsPage::setLayer (int l)
{
    layer = (l + cc::kNumLayers) % cc::kNumLayers;
    assignments.currentLayer = layer;
    if (ctx.customControlsChanged)
        ctx.customControlsChanged();
    rebuildLists();
    notifyStatusKeysChanged(); // the title tile shows the layer
    repaint();
}

void CustomControlsPage::rebuildLists()
{
    using namespace cc;
    const auto f = function();

    // FUNCTION: the categories of the selected slot.
    auto& fn = lists[0];
    fn.rows.clear();
    fn.fills.clear();
    fn.enabled.clear();
    fn.bars.clear();
    for (int i = 0; i < numCategories (slot); ++i)
    {
        fn.rows.add (categoryName (slot, i));
        fn.fills.push_back (categoryColour (slot, i));
        fn.enabled.push_back (categoryAvailable (slot, i));
    }
    fn.selected = f.category;

    // The target and option lists.
    for (int i = 1; i < 3; ++i)
    {
        lists[i].title = {};
        lists[i].rows.clear();
        lists[i].bars.clear();
        lists[i].fills.clear();
        lists[i].enabled.clear();
        lists[i].selected = -1;
    }
    targetCodes.clear();
    optionCodes.clear();
    auto& targets = lists[1];
    auto& options = lists[2];

    auto addStrips = [this, &targets] (bool inputsAndBuses)
    {
        targets.title = "CHANNEL";
        for (auto k : { StripKind::Channel, StripKind::Aux, StripKind::Bus, StripKind::Main, StripKind::Matrix })
            for (auto& s : ctx.engine.getStrips (k))
            {
                if (inputsAndBuses && ! (s->isInput() || s->isBus()))
                    continue;
                targets.rows.add (shortStripLabel (*s) + "\t" + (stripName (*s) != s->getLabel() ? stripName (*s).toUpperCase() : juce::String()));
                targets.bars.push_back (stripDisplayColour (*s));
                targetCodes.push_back (encodeStrip (state::StripRef::of (*s)));
            }
    };
    auto addNumbered = [] (ListColumn& list, std::vector<int>& codes, const juce::String& heading, const juce::String& prefix, int count, int first = 0)
    {
        list.title = heading;
        for (int i = 0; i < count; ++i)
        {
            list.rows.add (prefix + juce::String (first + i + 1));
            codes.push_back (first + i);
        }
    };
    auto addNames = [] (ListColumn& list, std::vector<int>& codes, const juce::String& heading, const char* const* names, int count)
    {
        list.title = heading;
        for (int i = 0; i < count; ++i)
        {
            list.rows.add (names[i]);
            codes.push_back (i);
        }
    };
    const int buses = (int) ctx.engine.getBuses().size();

    if (slot == Slot::Encoder)
    {
        switch (f.category)
        {
            case EncFader:
            case EncPan: addStrips (false); break;
            case EncDcaFader: addNumbered (targets, targetCodes, "DCA", "DCA ", kNumDcas); break;
            case EncSelBusSend: addNumbered (targets, targetCodes, "BUS", "BUS ", buses); break;
            case EncFixedBusSend:
                addStrips (true);
                addNumbered (options, optionCodes, "BUS", "BUS ", buses);
                break;
            case EncFxParameter:
            {
                targets.title = "EFFECT PROCESSOR";
                for (int i = 0; i < kNumFx; ++i)
                {
                    const auto& spec = models::model (models::Fx, ctx.engine.getFx().getModel (i));
                    targets.rows.add ("FX" + juce::String (i + 1) + "   " + spec.name.toUpperCase());
                    targetCodes.push_back (i);
                }
                options.title = "PARAMETER";
                options.rows.add ("FX MIX");
                optionCodes.push_back (-1);
                if (juce::isPositiveAndBelow (f.target, kNumFx))
                {
                    const auto& spec = models::model (models::Fx, ctx.engine.getFx().getModel (f.target));
                    for (size_t i = 0; i < spec.params.size(); ++i)
                    {
                        options.rows.add (spec.params[i].label.toUpperCase());
                        optionCodes.push_back ((int) i);
                    }
                }
                break;
            }
            case EncMonitoring: addNames (options, optionCodes, "OTHER", monitorNames, 2); break;
            default: break;
        }
    }
    else
    {
        switch (f.category)
        {
            case BtnMute:
            case BtnSolo: addStrips (false); break;
            case BtnDcaMute: addNumbered (targets, targetCodes, "DCA", "DCA ", kNumDcas); break;
            case BtnMuteGroup:
                targets.title = "MUTE GROUP";
                for (int i = 0; i < kNumMuteGroups; ++i)
                {
                    targets.rows.add (juce::String (i + 1) + "   " + ctx.engine.getMuteGroupName (i).toUpperCase());
                    targetCodes.push_back (i);
                }
                break;
            case BtnSendsOnFader:
                targets.title = "CHANNEL";
                for (auto k : { StripKind::Bus, StripKind::Main, StripKind::Matrix })
                    for (auto& s : ctx.engine.getStrips (k))
                    {
                        targets.rows.add (shortStripLabel (*s) + "\t" + (stripName (*s) != s->getLabel() ? stripName (*s).toUpperCase() : juce::String()));
                        targets.bars.push_back (stripDisplayColour (*s));
                        targetCodes.push_back (encodeStrip (state::StripRef::of (*s)));
                    }
                break;
            case BtnChannelPage:
                addStrips (false);
                addNames (options, optionCodes, "PAGE", pageNames, (int) std::size (pageNames));
                break;
            case BtnGlobalPage: addNames (options, optionCodes, "PAGE", globalNames, kNumGlobalPages); break;
            case BtnFaderPage:
            {
                static const char* const sections[] = { "LEFT", "CENTER", "RIGHT" };
                addNames (targets, targetCodes, "SECTION", sections, 3);
                options.title = "LAYER";
                const auto keys = ctx.show.layers.getDefaultLayers (ctx.engine, static_cast<LayerSet::Section> (juce::jlimit (0, 2, f.target)));
                for (size_t i = 0; i < keys.size(); ++i)
                {
                    options.rows.add (LayerSet::keyLabel (keys[i]).replace ("\n", " "));
                    optionCodes.push_back ((int) i);
                }
                break;
            }
            case BtnShowControl: addNames (options, optionCodes, "SHOW CONTROL", showControls, 5); break;
            case BtnSceneRecall:
                targets.title = "SCENE TAG";
                for (int tag = 1; tag <= Show::maxSceneTag; ++tag)
                {
                    const int scene = ctx.show.findSceneByTag (tag);
                    const auto s = ctx.show.getScene (scene);
                    targets.rows.add ("#" + juce::String (tag) + (s ? "   " + s->name.toUpperCase() : juce::String()));
                    targetCodes.push_back (tag);
                }
                break;
            case BtnOther: addNames (options, optionCodes, "OTHER", otherNames, kNumOthers); break;
            default: break;
        }
    }
    for (size_t i = 0; i < targetCodes.size(); ++i)
        if (targetCodes[i] == f.target)
            targets.selected = (int) i;
    for (size_t i = 0; i < optionCodes.size(); ++i)
        if (optionCodes[i] == f.option)
            options.selected = (int) i;
    // An option-only function shows its list in the first sub-list column.
    if (targets.rows.isEmpty() && ! options.rows.isEmpty())
    {
        std::swap (lists[1].title, lists[2].title);
        std::swap (lists[1].rows, lists[2].rows);
        std::swap (lists[1].selected, lists[2].selected);
        std::swap (targetCodes, optionCodes);
        targetIsOption = true;
    }
    else
        targetIsOption = false;
    for (auto& l : lists)
        l.scroll = juce::jlimit (0, juce::jmax (0, l.rows.size() - 1), l.scroll);
    resized();
    repaint();
}

void CustomControlsPage::choose (int list, int row)
{
    using namespace cc;
    auto f = function();
    if (list == 0)
    {
        if (! categoryAvailable (slot, row) || row == f.category)
            return;
        // A new function: its first target and option.
        f = { row, 0, 0 };
        if (slot == Slot::Encoder && row == EncFixedBusSend)
            f.option = 0;
        if (slot == Slot::Encoder && row == EncFxParameter)
            f.option = -1;
        store (f);
        rebuildLists();
        if (! targetCodes.empty())
        {
            auto g = function();
            (targetIsOption ? g.option : g.target) = targetCodes.front();
            store (g);
            rebuildLists();
        }
        return;
    }
    const bool isTarget = (list == 1) != targetIsOption;
    const auto& codes = list == 1 ? targetCodes : optionCodes;
    if (! juce::isPositiveAndBelow (row, (int) codes.size()))
        return;
    (isTarget ? f.target : f.option) = codes[(size_t) row];
    store (f);
    rebuildLists();
}

void CustomControlsPage::timerCallback()
{
    if (assignments.getRevision() != shownRevision)
    {
        shownRevision = assignments.getRevision();
        if (assignments.currentLayer != layer)
            setLayer (assignments.currentLayer);
        else
            rebuildLists();
    }
}

void CustomControlsPage::layoutList (ListColumn& l, juce::Rectangle<int> area)
{
    l.header = area.removeFromTop (juce::jlimit (30, 64, getHeight() / 12));
    area.removeFromTop (6);
    auto keys = area.removeFromBottom (juce::jlimit (18, 30, getHeight() / 22));
    area.removeFromBottom (4);
    l.upKey = keys.withWidth (keys.getHeight() + 12).withX (area.getCentreX() - keys.getHeight() - 14);
    l.downKey = l.upKey.withX (area.getCentreX() + 2);
    l.body = area;
    l.rowAreas.clear();
    auto rows = area.reduced (6, 6);
    rows.removeFromRight (juce::jmin (26, rows.getWidth() / 8)); // the scroll bar
    const int rowH = juce::jlimit (22, 64, getHeight() / 12);
    const int visible = juce::jmax (1, (rows.getHeight() + 6) / (rowH + 6));
    l.scroll = juce::jlimit (0, juce::jmax (0, l.rows.size() - visible), l.scroll);
    for (int i = 0; i < visible && l.scroll + i < l.rows.size(); ++i)
        l.rowAreas.push_back (rows.removeFromTop (rowH + 6).withTrimmedBottom (6));
}

void CustomControlsPage::resized()
{
    auto b = getLocalBounds();
    header = b.removeFromTop (juce::jlimit (36, 80, getHeight() / 9));
    b.removeFromTop (8);
    auto h = header.reduced (10, 8);
    prevKey = h.removeFromLeft (h.getHeight() + 6);
    h.removeFromLeft (8);
    nextKey = h.removeFromLeft (h.getHeight() + 6);
    h.removeFromLeft (h.getHeight());
    gpioKey = h.removeFromLeft (h.getHeight() * 2);
    title = header;

    const int gap = 8;
    auto left = b.removeFromLeft (b.getWidth() * 30 / 100);
    b.removeFromLeft (gap);
    const int colW = (b.getWidth() - 2 * gap) / 3;
    layoutList (lists[0], b.removeFromLeft (colW));
    b.removeFromLeft (gap);
    layoutList (lists[1], b.removeFromLeft (colW));
    b.removeFromLeft (gap);
    layoutList (lists[2], b);

    // LAYER panel, then SETTINGS | CONTROL.
    layerHeader = left.removeFromTop (lists[0].header.getHeight());
    left.removeFromTop (6);
    layerBody = left.removeFromTop (left.getHeight() * 52 / 100);
    left.removeFromTop (6);
    auto settings = left.removeFromLeft ((left.getWidth() - gap) / 2);
    left.removeFromLeft (gap);
    auto controlArea = left;
    const int hh = juce::jlimit (26, 56, getHeight() / 14);
    settingsHeader = settings.removeFromTop (hh);
    controlHeader = controlArea.removeFromTop (hh);
    settings.removeFromTop (6);
    controlArea.removeFromTop (6);
    const int kh = juce::jmin (settings.getHeight() / 2 - 6, hh + 10);
    colourKey = settings.removeFromTop (settings.getHeight() / 2).withSizeKeepingCentre (settings.getWidth() - 20, kh);
    clearKey = settings.withSizeKeepingCentre (settings.getWidth() - 20, kh);
    nameKey = controlArea.removeFromTop (controlArea.getHeight() / 2).withSizeKeepingCentre (controlArea.getWidth() - 20, kh);

    // The layer's map: four columns of colour bar, encoder and two keys.
    elements.clear();
    colourBars.clear();
    auto m = layerBody.reduced (10, 10);
    const int cw = m.getWidth() / cc::kNumControls;
    const int rowH = m.getHeight() / 3;
    for (int i = 0; i < cc::kNumControls; ++i)
    {
        auto col = juce::Rectangle<int> (m.getX() + i * cw, m.getY(), cw, m.getHeight()).reduced (4, 0);
        colourBars.push_back (col.removeFromTop (juce::jmax (4, rowH / 10)));
        auto rows = col;
        for (auto s : { cc::Slot::Encoder, cc::Slot::Upper, cc::Slot::Lower })
            elements.push_back ({ rows.removeFromTop (rowH - (s == cc::Slot::Encoder ? juce::jmax (4, rowH / 10) : 0)), i, s });
    }
}

void CustomControlsPage::paintList (juce::Graphics& g, ListColumn& l, bool focused)
{
    if (l.title.isEmpty() && l.rows.isEmpty())
    {
        g.setColour (Dark::header);
        g.fillRect (l.header);
        return;
    }
    g.setColour (Dark::header);
    g.fillRect (l.header);
    g.setColour (Dark::text);
    g.setFont (panelTitleFont (l.header.getHeight()));
    g.drawFittedText (l.title, l.header.reduced (6, 0), juce::Justification::centred, 1);
    g.setColour (Dark::body);
    g.fillRect (l.body);

    for (size_t i = 0; i < l.rowAreas.size(); ++i)
    {
        const int row = l.scroll + (int) i;
        auto r = l.rowAreas[i];
        const bool enabled = row >= (int) l.enabled.size() || l.enabled[(size_t) row];
        g.setColour (row < (int) l.fills.size() ? l.fills[(size_t) row] : juce::Colours::black);
        g.fillRect (r);
        auto t = r;
        if (row < (int) l.bars.size())
        {
            g.setColour (l.bars[(size_t) row]);
            g.fillRect (t.removeFromLeft (6));
            // Channel rows: the strip's icon between its label and name.
            if (&l == &lists[1] && row < (int) targetCodes.size())
                if (auto* s = state::findStrip (ctx.engine, cc::decodeStrip (targetCodes[(size_t) row])))
                {
                    auto icon = t.reduced (8, 0);
                    icon.removeFromLeft (icon.getWidth() * 28 / 100);
                    drawStripIcon (g, *s, icon.withWidth (icon.getHeight()).toFloat().reduced ((float) icon.getHeight() * 0.2f), Dark::text);
                }
        }
        g.setColour (enabled ? Dark::text : Dark::textDim.withAlpha (0.45f));
        g.setFont (juce::FontOptions (juce::jlimit (11.0f, 21.0f, (float) r.getHeight() * 0.38f)));
        if (l.rows[row].containsChar ('\t'))
        {
            // A channel row: the label, the icon (drawn above), then the name.
            auto text = t.reduced (8, 0);
            g.drawFittedText (l.rows[row].upToFirstOccurrenceOf ("\t", false, false), text.removeFromLeft (text.getWidth() * 28 / 100), juce::Justification::centredLeft, 1, 0.8f);
            text.removeFromLeft (text.getHeight() + 6);
            g.drawFittedText (l.rows[row].fromFirstOccurrenceOf ("\t", false, false), text, juce::Justification::centredLeft, 1, 0.8f);
        }
        else
            g.drawFittedText (l.rows[row], t.reduced (8, 0), juce::Justification::centredLeft, 1, 0.8f);
        if (row == l.selected)
        {
            g.setColour (Colours::yellow);
            g.drawRect (r.toFloat(), 2.0f);
        }
    }

    // The scroll bar and the ▲ ▼ page keys.
    auto bar = l.body.reduced (6, 6).removeFromRight (juce::jmin (26, l.body.getWidth() / 8)).withTrimmedLeft (6);
    g.setColour (juce::Colours::black);
    g.fillRect (bar);
    if (l.rows.size() > (int) l.rowAreas.size() && ! l.rows.isEmpty())
    {
        const float total = (float) l.rows.size();
        auto thumb = bar.toFloat().withTrimmedTop (bar.getHeight() * (float) l.scroll / total)
                         .withHeight (bar.getHeight() * (float) l.rowAreas.size() / total);
        g.setColour (Dark::textDim);
        g.fillRect (thumb.reduced (3.0f, 1.0f));
    }
    for (auto [r, up] : { std::pair { l.upKey, true }, std::pair { l.downKey, false } })
    {
        juce::Path tri;
        const auto c = r.getCentre().toFloat();
        const float s = (float) r.getHeight() * 0.32f;
        if (up)
            tri.addTriangle (c.x - s, c.y + s * 0.6f, c.x + s, c.y + s * 0.6f, c.x, c.y - s * 0.6f);
        else
            tri.addTriangle (c.x - s, c.y - s * 0.6f, c.x + s, c.y - s * 0.6f, c.x, c.y + s * 0.6f);
        g.setColour (focused ? Dark::text : Dark::textDim);
        g.fillPath (tri);
    }
}

void CustomControlsPage::paintLayer (juce::Graphics& g)
{
    g.setColour (Dark::header);
    g.fillRect (layerHeader);
    g.setColour (Dark::text);
    g.setFont (panelTitleFont (layerHeader.getHeight()));
    g.drawText ("LAYER " + juce::String (layer + 1), layerHeader, juce::Justification::centred);
    g.setColour (Dark::body);
    g.fillRect (layerBody);

    for (int i = 0; i < cc::kNumControls; ++i)
    {
        const auto c = assignments.get (layer, i);
        g.setColour (c.colour > 0 ? wingColour (c.colour) : (c.encoder.isAssigned() || c.upper.isAssigned() || c.lower.isAssigned() ? keyGrey : juce::Colours::black));
        g.fillRect (colourBars[(size_t) i]);
    }
    for (auto& e : elements)
    {
        const auto c = assignments.get (layer, e.index);
        const auto& f = c.slot (e.slot);
        auto r = e.area;
        auto caption = r.removeFromTop (juce::jmax (12, r.getHeight() * 28 / 100));
        juce::String text = e.slot == cc::Slot::Encoder && c.name.isNotEmpty() ? c.name : cc::describe (ctx, e.slot, f);
        g.setColour (Dark::text);
        g.setFont (juce::FontOptions (juce::jlimit (8.0f, 14.0f, (float) caption.getHeight() * 0.8f)));
        g.drawFittedText (text.removeCharacters ("|*"), caption, juce::Justification::centredBottom, 1, 0.6f);
        const int s = juce::jmin (r.getHeight() - 4, r.getWidth() * 2 / 3);
        auto square = r.withSizeKeepingCentre (s, s);
        if (e.slot == cc::Slot::Encoder)
        {
            g.setColour (juce::Colours::black);
            g.fillRect (square);
            g.setColour (Dark::keyOutline.withAlpha (0.5f));
            g.drawRect (square, 1);
            // A knob: a ring open at the bottom.
            auto k = square.toFloat().reduced ((float) s * 0.18f);
            juce::Path ring;
            ring.addCentredArc (k.getCentreX(), k.getCentreY(), k.getWidth() * 0.4f, k.getWidth() * 0.4f, 0.0f, juce::MathConstants<float>::pi * 1.2f,
                                juce::MathConstants<float>::pi * 2.8f, true);
            g.setColour (f.isAssigned() ? keyGrey : unsetGrey);
            g.strokePath (ring, juce::PathStrokeType (juce::jmax (3.0f, (float) s * 0.12f)));
        }
        else
        {
            auto key = square.reduced (s / 6);
            g.setColour (Dark::body.darker (0.4f));
            g.fillRect (square);
            g.setColour (f.isAssigned() ? keyGrey : unsetGrey);
            g.fillRect (key);
        }
        if (e.index == index && e.slot == slot)
        {
            g.setColour (Colours::yellow);
            g.drawRect (e.area.expanded (2).toFloat(), 2.0f);
        }
    }
}

void CustomControlsPage::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);

    // Header: ‹ ›, GPIO (not here yet) and the title.
    g.setColour (Dark::header);
    g.fillRect (header);
    arrowKey (g, prevKey, false);
    arrowKey (g, nextKey, true);
    g.setColour (Dark::key.withAlpha (0.5f));
    g.fillRect (gpioKey);
    g.setColour (Dark::textDim.withAlpha (0.5f));
    g.setFont (juce::FontOptions (juce::jmin (20.0f, (float) gpioKey.getHeight() * 0.45f)));
    g.drawText ("GPIO", gpioKey, juce::Justification::centred);
    g.setColour (Dark::text);
    g.setFont (wingFontPx (juce::jmin (TypeScale::statusTitle * getWingScreenHeight() * 0.8f, (float) title.getHeight() * 0.36f), WingWeight::SemiBold));
    g.drawText ("CUSTOM CONTROLS", title, juce::Justification::centred);

    paintLayer (g);
    for (int i = 0; i < 3; ++i)
        paintList (g, lists[i], focusedList == i);

    for (auto [r, text] : { std::pair { settingsHeader, "SETTINGS" }, std::pair { controlHeader, "CONTROL" } })
    {
        g.setColour (Dark::header);
        g.fillRect (r);
        g.setColour (Dark::text);
        g.setFont (juce::FontOptions (juce::jmin (20.0f, (float) r.getHeight() * 0.45f)));
        g.drawText (text, r, juce::Justification::centred);
    }
    g.setColour (Dark::body);
    g.fillRect (colourKey.getUnion (clearKey).expanded (10, 8));
    g.fillRect (nameKey.expanded (10, 8).withBottom (clearKey.getBottom() + 8));
    outlinedKey (g, colourKey, Icon::Edit, "COLOR");
    outlinedKey (g, clearKey, Icon::Info, "CLR ALL");
    outlinedKey (g, nameKey, Icon::Edit, "NAME");
}

void CustomControlsPage::mouseUp (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();
    if (prevKey.contains (pos) || nextKey.contains (pos))
    {
        setLayer (layer + (nextKey.contains (pos) ? 1 : -1));
        return;
    }
    for (auto& el : elements)
        if (el.area.contains (pos))
        {
            index = el.index;
            slot = el.slot;
            lists[0].scroll = 0;
            rebuildLists();
            return;
        }
    for (int i = 0; i < 3; ++i)
    {
        auto& l = lists[i];
        if (l.upKey.contains (pos) || l.downKey.contains (pos))
        {
            const int page = juce::jmax (1, (int) l.rowAreas.size());
            l.scroll = juce::jmax (0, l.scroll + (l.downKey.contains (pos) ? page : -page));
            focusedList = i;
            resized();
            repaint();
            return;
        }
        for (size_t r = 0; r < l.rowAreas.size(); ++r)
            if (l.rowAreas[r].contains (pos))
            {
                focusedList = i;
                choose (i, l.scroll + (int) r);
                return;
            }
    }
    if (colourKey.contains (pos))
    {
        juce::StringArray items { "NONE" };
        for (int k = 1; k <= 18; ++k)
            items.add (wingColourName (k).toUpperCase());
        anchor.setBounds (colourKey);
        juce::Component::SafePointer<CustomControlsPage> safe (this);
        const int l = layer, i = index;
        popups::chooseFrom (anchor, items, control().colour, [safe, l, i] (int colour)
        {
            if (safe == nullptr)
                return;
            auto c = safe->assignments.get (l, i);
            c.colour = colour;
            safe->assignments.set (l, i, c);
            if (safe->ctx.customControlsChanged)
                safe->ctx.customControlsChanged();
            safe->repaint();
        });
    }
    else if (clearKey.contains (pos))
    {
        const int l = layer;
        popups::confirm (*this, "CLR ALL", "Clear every control of layer " + juce::String (l + 1) + "?", "CLEAR", [this, l]
        {
            assignments.clearLayer (l);
            if (ctx.customControlsChanged)
                ctx.customControlsChanged();
            rebuildLists();
        }, true);
    }
    else if (nameKey.contains (pos))
    {
        popups::TextOptions options;
        options.icon = Icon::CcEdit;
        options.maxLength = 16;
        const int l = layer, i = index;
        juce::Component::SafePointer<CustomControlsPage> safe (this);
        popups::editText (*this, "CONTROL NAME", control().name, [safe, l, i] (const juce::String& name)
        {
            if (safe == nullptr)
                return;
            auto c = safe->assignments.get (l, i);
            c.name = name.trim();
            safe->assignments.set (l, i, c);
            if (safe->ctx.customControlsChanged)
                safe->ctx.customControlsChanged();
            safe->repaint();
        }, options);
    }
}

void CustomControlsPage::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    for (int i = 0; i < 3; ++i)
        if (lists[i].body.contains (e.getPosition()))
        {
            lists[i].scroll = juce::jmax (0, lists[i].scroll + (w.deltaY > 0.0f ? -1 : 1));
            focusedList = i;
            resized();
            repaint();
            return;
        }
}

} // namespace livedaw::ui
