#include "ui/CustomControlsPage.h"
#include "engine/Models.h"
#include "ui/MidiRemoteUi.h"
#include "ui/UsbRecorderPage.h"
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
    // USB PLAY/REC: the USB recorder / player's keys (the option).
    const char* const usbNames[] = { "PLAY", "PAUSE", "STOP", "PREV", "NEXT", "REC", "REC PAUSE", "REC STOP", "NEW FILE", "REPEAT" };
    constexpr int kNumUsbFunctions = 10;

    Strip* stripOf (UiContext& ctx, int target) { return state::findStrip (ctx.engine, decodeStrip (target)); }

    /** FADER SPILL's kinds, in the order of their codes (kind * 100 + index). */
    constexpr int kNumSpillKinds = 6;

    /** FADER SPILL: the spill of a target, kind * 100 + index (custom.rs);
        SEL SENDS spills the selected strip's sends (none without one). */
    std::optional<LayerSet::Spill> spillOf (UiContext& ctx, int target)
    {
        const int kind = target / 100, index = target % 100;
        if (! juce::isPositiveAndBelow (kind, kNumSpillKinds) || index < 0)
            return std::nullopt;
        LayerSet::Spill spill { static_cast<LayerSet::Spill::Kind> (kind), index, {} };
        if (spill.kind == LayerSet::Spill::Kind::Sends)
        {
            auto* sel = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr;
            if (sel == nullptr)
                return std::nullopt;
            spill.index = 0;
            spill.strip = state::StripRef::of (*sel);
        }
        return spill;
    }
    const char* const spillKindNames[] = { "DCA", "BUS", "FX", "AUTOMIX", "SEL SENDS", "MAIN" };

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
                                           "MIDI CC/NOTE/PGM", "USB PLAY/REC", "SD RECORDER", "GPIO", "CC BANK" };
    if (s == Slot::Encoder)
        return juce::isPositiveAndBelow (category, (int) std::size (encoders)) ? encoders[category] : "";
    return juce::isPositiveAndBelow (category, (int) std::size (buttons)) ? buttons[category] : "";
}

bool categoryAvailable (Slot s, int category)
{
    if (s == Slot::Encoder)
        return juce::isPositiveAndBelow (category, (int) kNumEncoderCategories);
    // SD RECORDER and GPIO need the console's hardware.
    return category != BtnSdRecorder && category != BtnGpio && juce::isPositiveAndBelow (category, (int) kNumButtonCategories);
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
        case EncDawVpot:
            if (juce::isPositiveAndBelow (f.target, DawRemote::kNumVpotFunctions))
                return midiui::bindDawVpot (ctx, f.target);
            break;
        case EncMidiCc:
            return midiui::bindMidiCc (ctx, juce::jlimit (0, 15, f.target), juce::jlimit (0, 127, f.option));
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
        case BtnFaderSpill:
            // On the left section (as the screens' SPILL keys); again: back.
            if (const auto spill = spillOf (ctx, f.target))
                toggleSpill (ctx, std::nullopt, *spill);
            break;
        case BtnDawButtons:
            midiui::pressDawCommand (ctx, f.option, true);
            break;
        case BtnMidi:
            midiui::pressMidiKey (ctx, f.target, f.option, true);
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
        case BtnUsbPlayer:
        {
            auto& usb = ctx.engine.getUsbRecorder();
            switch (f.option)
            {
                case 0: UsbRecorderPage::pressPlay (ctx); break;
                case 1: usb.togglePlayPause(); break;
                case 2: usb.stop(); break;
                case 3: usb.previous(); break;
                case 4: usb.next(); break;
                case 5: UsbRecorderPage::pressRec (ctx); break;
                case 6: usb.toggleRecPause(); break;
                case 7: usb.stopRecording(); break;
                case 8:
                    if (auto r = usb.newFile(); r.failed())
                        ctx.log.warning ("USB recorder: " + r.getErrorMessage());
                    break;
                case 9: usb.setRepeat (! usb.getRepeat()); break;
                default: break;
            }
            break;
        }
        case BtnCcBank:
            // CC BANK n (3.0.6): the custom controls show bank n.
            if (ctx.customControls != nullptr && juce::isPositiveAndBelow (f.option, kNumLayers))
            {
                ctx.customControls->currentLayer = f.option;
                if (ctx.customControlsChanged)
                    ctx.customControlsChanged();
            }
            break;
        default:
            break;
    }
}

void release (UiContext& ctx, const Function& f)
{
    if (f.category == BtnDawButtons)
        midiui::pressDawCommand (ctx, f.option, false);
    else if (f.category == BtnMidi)
        midiui::pressMidiKey (ctx, f.target, f.option, false);
}

int ledState (UiContext& ctx, const Function& f)
{
    if (f.category == BtnDawButtons)
        return midiui::dawCommandLed (ctx, f.option);
    return isLit (ctx, f) ? 1 : 0;
}

juce::String layerName (int layer)
{
    if (layer == kUserLayer)
        return "USER";
    if (layer == kCompactMainMatrixLayer)
        return "MAIN/MTX 5-8";
    if (isCompactLayer (layer))
    {
        const int first = (layer < kCompactMainMatrixLayer ? layer - kFirstCompactUserLayer : layer - kFirstCompactDcaLayer) * 4 + 1;
        return juce::String (layer < kCompactMainMatrixLayer ? "USER " : "DCA ") + juce::String (first) + "-" + juce::String (first + 3);
    }
    if (layer >= kFirstDawLayer)
        return "DAW " + juce::String (layer - kFirstDawLayer + 1);
    if (layer >= kFirstFourChannelLayer)
        return "U" + juce::String (layer - kFirstFourChannelLayer + 1);
    return "LAYER " + juce::String (layer + 1);
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
        case BtnFaderSpill:
            if (const auto spill = spillOf (ctx, f.target))
                return isSpilling (ctx, *spill);
            break;
        case BtnDawButtons: return midiui::dawCommandLed (ctx, f.option) != 0;
        case BtnMidi: return midiui::midiKeyLit (ctx, f.target, f.option);
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
        case BtnUsbPlayer:
        {
            auto& usb = ctx.engine.getUsbRecorder();
            switch (f.option)
            {
                case 0: return usb.getPlayState() == UsbRecorder::PlayState::Playing;
                case 1: return usb.getPlayState() == UsbRecorder::PlayState::Paused;
                case 2: return usb.getPlayState() == UsbRecorder::PlayState::Stopped;
                case 5: return usb.getRecState() == UsbRecorder::RecState::Recording;
                case 6: return usb.getRecState() == UsbRecorder::RecState::Paused;
                case 9: return usb.getRepeat();
                default: return false;
            }
        }
        case BtnCcBank: return ctx.customControls != nullptr && ctx.customControls->currentLayer == f.option;
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
            case EncDawVpot:      return DawRemote::vpotFunctionName (f.target).replace ("V-POT ", "VPOT ").replace ("JOG WHEEL", "JOG");
            case EncMidiCc:       return midiui::describeMidiCc (f.target, f.option);
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
        case BtnFaderSpill:
        {
            const int kind = juce::jlimit (0, kNumSpillKinds - 1, f.target / 100), index = f.target % 100;
            if (kind == 4)
                return "SPILL SENDS";
            if (kind == 3)
                return juce::String ("SPILL AM ") + (index == 0 ? "X" : "Y");
            return juce::String ("SPILL ") + spillKindNames[kind] + juce::String (index + 1);
        }
        case BtnDawButtons:   return midiui::describeDawCommand (ctx, f.option);
        case BtnMidi:         return midiui::describeMidiKey (f.target, f.option);
        case BtnUsbPlayer:    return juce::String ("USB ") + usbNames[juce::jlimit (0, kNumUsbFunctions - 1, f.option)];
        case BtnCcBank:       return "CC BANK " + juce::String (f.option + 1);
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
        // [3.1] a #303030 key in a 1 px #c0c0c0 frame, the icon in its left
        // 52 px (34 px tall) and the caption centred in the rest.
        const auto ink = enabled ? Dark::caption : Dark::textDim.withAlpha (0.5f);
        g.setColour (Dark::key);
        g.fillRect (r);
        g.setColour (ink);
        g.drawRect (r, 1);
        auto inner = r;
        auto iconArea = inner.removeFromLeft (juce::roundToInt (wingPx (52.0f)));
        drawIcon (g, icon, iconArea.withSizeKeepingCentre (juce::roundToInt (wingPx (34.0f)), juce::roundToInt (wingPx (34.0f))).toFloat(), ink);
        g.setColour (ink);
        g.setFont (wingFontPx (wingPx (14.0f)));
        g.drawFittedText (text, inner, juce::Justification::centred, 1);
    }
} // namespace

CustomControlsPage::CustomControlsPage (UiContext& c, cc::Assignments& a, int startLayer) : ctx (c), assignments (a)
{
    layer = startLayer >= 0 ? juce::jlimit (0, cc::kNumStoredLayers - 1, startLayer) : assignments.currentLayer;
    if (cc::isLowerLayer (layer))
        slot = cc::Slot::Upper; // the lower part has keys only
    lists[0].title = "FUNCTION";
    anchor.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (anchor);
    rebuildLists();
    revealSelection();
    startTimerHz (5);
}

StatusTitle CustomControlsPage::getStatusTitle()
{
    // A dark tile with a knob icon and the layer number [D1].
    StatusTitle t;
    t.kind = StatusTitle::Kind::Layer;
    t.icon = Icon::CcEdit;
    t.text = cc::isLowerLayer (layer) ? cc::layerName (layer) : juce::String (layer + 1);
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
    if (cc::isLowerLayer (layer))
    {
        // ‹ › step through DAW 1-4 or the COMPACT's nine sets; USER stays.
        if (cc::isCompactLayer (layer))
            layer = cc::kFirstCompactUserLayer + (l - cc::kFirstCompactUserLayer + cc::kNumCompactLayers) % cc::kNumCompactLayers;
        else if (layer >= cc::kFirstDawLayer)
            layer = cc::kFirstDawLayer + (l - cc::kFirstDawLayer + cc::kNumDawLayers) % cc::kNumDawLayers;
        resized(); // the sets have one row of keys, the DAW layers two
        rebuildLists();
        notifyStatusKeysChanged();
        repaint();
        return;
    }
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
            case EncDawVpot:
                targets.title = "V-POT";
                for (int i = 0; i < DawRemote::kNumVpotFunctions; ++i)
                {
                    targets.rows.add (DawRemote::vpotFunctionName (i));
                    targetCodes.push_back (i);
                }
                break;
            case EncMidiCc:
                addNumbered (targets, targetCodes, "MIDI CHANNEL", "CH ", 16);
                options.title = "CC";
                for (int i = 0; i < 128; ++i)
                {
                    options.rows.add ("CC " + juce::String (i));
                    optionCodes.push_back (i);
                }
                break;
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
            case BtnFaderSpill:
            {
                // SPILL (kind), then which one: both set the target (the
                // option codes are whole targets, kind * 100 + index).
                targets.title = "SPILL";
                const int kind = juce::jlimit (0, kNumSpillKinds - 1, f.target / 100);
                for (int k = 0; k < kNumSpillKinds; ++k)
                {
                    targets.rows.add (spillKindNames[k]);
                    targetCodes.push_back (k * 100 + (k == kind ? f.target % 100 : 0));
                }
                auto add = [this, &options, kind] (const juce::String& row, int item)
                {
                    options.rows.add (row);
                    optionCodes.push_back (kind * 100 + item);
                };
                switch (kind)
                {
                    case 0:
                        options.title = "DCA";
                        for (int i = 0; i < kNumDcas; ++i)
                            add (juce::String (i + 1) + "   " + ctx.engine.getDcaName (i).toUpperCase(), i);
                        break;
                    case 1:
                        options.title = "BUS";
                        for (auto& b : ctx.engine.getBuses())
                            add (shortStripLabel (*b) + "\t" + (stripName (*b) != b->getLabel() ? stripName (*b).toUpperCase() : juce::String()), b->getId());
                        break;
                    case 2:
                        options.title = "EFFECT PROCESSOR";
                        for (int i = 0; i < kNumFx; ++i)
                            add ("FX" + juce::String (i + 1) + "   " + models::model (models::Fx, ctx.engine.getFx().getModel (i)).name.toUpperCase(), i);
                        break;
                    case 3:
                        options.title = "AUTOMIX";
                        add ("GROUP X", 0);
                        add ("GROUP Y", 1);
                        break;
                    case 5:
                        // A main's spill: the channels, aux and buses feeding it (spidx 57-60).
                        options.title = "MAIN";
                        for (auto& m : ctx.engine.getStrips (StripKind::Main))
                            add (shortStripLabel (*m) + "\t" + (stripName (*m) != m->getLabel() ? stripName (*m).toUpperCase() : juce::String()), m->getId());
                        break;
                    default:
                        options.title = "SPILL";
                        add ("SELECTED CHANNEL'S SENDS", 0);
                        break;
                }
                break;
            }
            case BtnDawButtons:
            {
                // GROUP, then the group's MCU / HUI commands (the other
                // protocol's greyed out, as on the console).
                const auto groups = DawRemote::commandGroups();
                targets.title = "GROUP";
                for (int i = 0; i < groups.size(); ++i)
                {
                    targets.rows.add (groups[i]);
                    targetCodes.push_back (i);
                }
                auto* d = midiui::daw (ctx);
                options.title = d != nullptr && d->isHui() ? "HUI COMMAND" : "MCU COMMAND";
                const auto& table = DawRemote::commands();
                const auto group = groups[juce::jlimit (0, groups.size() - 1, f.target)];
                for (size_t i = 0; i < table.size(); ++i)
                    if (group == table[i].group)
                    {
                        options.rows.add (juce::String (table[i].code) + "   " + table[i].name);
                        optionCodes.push_back ((int) i);
                        options.enabled.push_back (d == nullptr || d->isAvailable ((int) i));
                    }
                break;
            }
            case BtnMidi:
            {
                // MESSAGE (type and MIDI channel), then the CC, note or program.
                targets.title = "MESSAGE";
                for (int type = 0; type < midiui::kNumMidiKeyTypes; ++type)
                    for (int ch = 0; ch < 16; ++ch)
                    {
                        targets.rows.add (midiui::midiKeyTypeName (type) + "  CH " + juce::String (ch + 1));
                        targetCodes.push_back (type * 16 + ch);
                    }
                const int type = f.target / 16;
                options.title = type == midiui::ProgramChange ? "PROGRAM" : type >= midiui::NoteToggle ? "NOTE" : "CC";
                for (int i = 0; i < 128; ++i)
                {
                    options.rows.add (options.title + " " + juce::String (type == midiui::ProgramChange ? i + 1 : i));
                    optionCodes.push_back (i);
                }
                break;
            }
            case BtnUsbPlayer: addNames (options, optionCodes, "USB PLAY/REC", usbNames, kNumUsbFunctions); break;
            case BtnCcBank:
                options.title = "CC BANK";
                for (int i = 0; i < kNumLayers; ++i)
                {
                    options.rows.add ("BANK " + juce::String (i + 1));
                    optionCodes.push_back (i);
                }
                break;
            default: break;
        }
    }
    for (size_t i = 0; i < targetCodes.size(); ++i)
        if (targetCodes[i] == f.target)
            targets.selected = (int) i;
    const int optionValue = slot != Slot::Encoder && f.category == BtnFaderSpill ? f.target : f.option;
    for (size_t i = 0; i < optionCodes.size(); ++i)
        if (optionCodes[i] == optionValue)
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

void CustomControlsPage::revealSelection()
{
    bool moved = false;
    for (auto& l : lists)
    {
        const int visible = juce::jmax (1, (int) l.rowAreas.size());
        if (l.selected >= 0 && (l.selected < l.scroll || l.selected >= l.scroll + visible))
        {
            l.scroll = juce::jmax (0, l.selected - visible / 2);
            moved = true;
        }
    }
    if (moved)
    {
        resized();
        repaint();
    }
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
    if (const auto& enabled = lists[list].enabled; juce::isPositiveAndBelow (row, (int) enabled.size()) && ! enabled[(size_t) row])
        return;
    (isTarget || (slot != Slot::Encoder && f.category == BtnFaderSpill) ? f.target : f.option) = codes[(size_t) row];
    if (slot != Slot::Encoder && f.category == BtnDawButtons && isTarget)
    {
        // A new group: its first command.
        const auto group = DawRemote::commandGroups()[f.target];
        const auto& table = DawRemote::commands();
        for (size_t i = 0; i < table.size(); ++i)
            if (group == table[i].group)
            {
                f.option = (int) i;
                break;
            }
    }
    store (f);
    rebuildLists();
}

void CustomControlsPage::timerCallback()
{
    if (assignments.getRevision() != shownRevision)
    {
        shownRevision = assignments.getRevision();
        if (assignments.currentLayer != layer && ! cc::isLowerLayer (layer))
            setLayer (assignments.currentLayer);
        else
            rebuildLists();
    }
}

void CustomControlsPage::layoutList (ListColumn& l, juce::Rectangle<int> area)
{
    // As on the console: a 64 px header, the list (64 px rows on a 72 px pitch, the
    // scroll bar at the right) and the ▲ ▼ page keys under it.
    l.header = area.removeFromTop (juce::roundToInt (wingPx (64.0f)));
    area.removeFromTop (juce::roundToInt (wingPx (8.0f)));
    auto keys = area.removeFromBottom (juce::roundToInt (wingPx (40.0f)));
    // [3.1] ▲ 39 to 9 px left of the column's centre, ▼ 1 to 31 px right of it, 16 px from the list.
    const int kh = juce::roundToInt (wingPx (16.0f)), kw = juce::roundToInt (wingPx (30.0f));
    l.upKey = juce::Rectangle<int> (area.getCentreX() - juce::roundToInt (wingPx (39.0f)), keys.getY() + juce::roundToInt (wingPx (16.0f)), kw, kh);
    l.downKey = l.upKey.withX (area.getCentreX() + juce::roundToInt (wingPx (1.0f)));
    l.body = area;
    l.rowAreas.clear();
    const int pad = juce::roundToInt (wingPx (8.0f));
    auto rows = area.reduced (pad, pad);
    rows.removeFromRight (juce::roundToInt (wingPx (40.0f))); // the scroll bar
    const int rowH = juce::jmax (18, juce::roundToInt (wingPx (64.0f))), rowGap = juce::roundToInt (wingPx (8.0f));
    const int visible = juce::jmax (1, (rows.getHeight() + rowGap) / (rowH + rowGap));
    l.scroll = juce::jlimit (0, juce::jmax (0, l.rows.size() - visible), l.scroll);
    for (int i = 0; i < visible && l.scroll + i < l.rows.size(); ++i)
        l.rowAreas.push_back (rows.removeFromTop (rowH + rowGap).withTrimmedBottom (rowGap));
}

void CustomControlsPage::resized()
{
    // In px of the 1280 x 800 screen (this page starts 80 px down):
    // the 80 px header, the LAYER column 376 px wide and three 288 px lists.
    const float sx = (float) getWidth() / 1280.0f;
    auto rect = [&] (float x0, float y0, float x1, float y1)
    {
        return juce::Rectangle<int>::leftTopRightBottom (juce::roundToInt (x0 * sx), juce::roundToInt (wingPx (y0 - 80.0f)), juce::roundToInt (x1 * sx),
                                                         juce::roundToInt (wingPx (y1 - 80.0f)));
    };
    header = rect (8.0f, 80.0f, 1272.0f, 160.0f);
    prevKey = rect (20.0f, 92.0f, 76.0f, 148.0f);
    nextKey = rect (84.0f, 92.0f, 140.0f, 148.0f);
    userKey = rect (148.0f, 92.0f, 228.0f, 148.0f);
    gpioKey = rect (236.0f, 92.0f, 316.0f, 148.0f);
    title = header;

    const float listTop = 168.0f, listBottom = 792.0f;
    layoutList (lists[0], rect (392.0f, listTop, 680.0f, listBottom));
    layoutList (lists[1], rect (688.0f, listTop, 976.0f, listBottom));
    layoutList (lists[2], rect (984.0f, listTop, 1272.0f, listBottom));

    // LAYER panel, then SETTINGS | CONTROL.
    layerHeader = rect (8.0f, 168.0f, 384.0f, 232.0f);
    layerBody = rect (8.0f, 240.0f, 384.0f, 496.0f);
    settingsHeader = rect (8.0f, 504.0f, 192.0f, 560.0f);
    controlHeader = rect (200.0f, 504.0f, 384.0f, 560.0f);
    colourBox = rect (8.0f, 568.0f, 192.0f, 656.0f);
    clearBox = rect (8.0f, 664.0f, 192.0f, 752.0f);
    nameBox = rect (200.0f, 568.0f, 384.0f, 752.0f);
    colourKey = rect (24.0f, 584.0f, 176.0f, 640.0f);
    clearKey = rect (24.0f, 680.0f, 176.0f, 736.0f);
    nameKey = rect (216.0f, 584.0f, 368.0f, 640.0f);

    // The layer's map: four columns of colour bar, encoder and two keys.
    elements.clear();
    colourBars.clear();
    auto m = layerBody.reduced (juce::roundToInt (wingPx (12.0f)), juce::roundToInt (wingPx (12.0f)));
    const int cw = m.getWidth() / cc::kNumControls;
    const bool keysOnly = cc::isLowerLayer (layer);
    const bool lowerKeys = cc::hasLowerKeys (layer);
    const int rowH = m.getHeight() / (! keysOnly ? 3 : lowerKeys ? 2 : 1);
    if (! keysOnly)
    {
        // [3.1] columns 82 px wide on a 90 px pitch from x 20: the 8 px colour
        // bar at y 252, the encoder (its tile at the bottom) at 265-336, the
        // keys at 340-408 and 408-476 (each key 18 px down).
        for (int i = 0; i < cc::kNumControls; ++i)
        {
            const float x = 20.0f + 90.0f * (float) i;
            colourBars.push_back (rect (x, 252.0f, x + 82.0f, 260.0f));
            elements.push_back ({ rect (x, 265.0f, x + 82.0f, 336.0f), i, cc::Slot::Encoder });
            elements.push_back ({ rect (x, 340.0f, x + 82.0f, 408.0f), i, cc::Slot::Upper });
            elements.push_back ({ rect (x, 408.0f, x + 82.0f, 476.0f), i, cc::Slot::Lower });
        }
        return;
    }
    for (int i = 0; i < cc::kNumControls; ++i)
    {
        auto col = juce::Rectangle<int> (m.getX() + i * cw, m.getY(), cw, m.getHeight()).reduced (4, 0);
        colourBars.push_back (col.removeFromTop (juce::jmax (4, rowH / 10)));
        auto rows = col;
        for (auto s : { cc::Slot::Encoder, cc::Slot::Upper, cc::Slot::Lower })
            if (! (keysOnly && s == cc::Slot::Encoder) && ! (! lowerKeys && s == cc::Slot::Lower))
                elements.push_back ({ rows.removeFromTop (rowH - (s == cc::Slot::Encoder || (keysOnly && s == cc::Slot::Upper) ? juce::jmax (4, rowH / 10) : 0)), i, s });
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
    g.setColour (Dark::caption);
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
        g.setColour (enabled ? Dark::caption : Dark::textDim.withAlpha (0.45f));
        g.setFont (wingFontPx (juce::jmin (TypeScale::body * getWingScreenHeight(), (float) r.getHeight() * 0.3f))); // [D1]
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
    // A ▲ over the scroll well and a ▼ under it.
    auto bar = l.body.reduced (juce::roundToInt (wingPx (8.0f))).removeFromRight (juce::roundToInt (wingPx (32.0f)));
    for (bool up : { true, false })
    {
        auto end = up ? bar.removeFromTop (juce::roundToInt (wingPx (28.0f))) : bar.removeFromBottom (juce::roundToInt (wingPx (28.0f)));
        const auto c = end.getCentre().toFloat();
        const float a = (float) end.getWidth() * 0.45f, h = a * 0.55f;
        juce::Path tri;
        if (up)
            tri.addTriangle (c.x - a, c.y + h * 0.5f, c.x + a, c.y + h * 0.5f, c.x, c.y - h * 0.5f);
        else
            tri.addTriangle (c.x - a, c.y - h * 0.5f, c.x + a, c.y - h * 0.5f, c.x, c.y + h * 0.5f);
        g.setColour (Dark::textDim);
        g.fillPath (tri);
    }
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
        // [3.1] a 30 x 16 triangle filling the key.
        juce::Path tri;
        const auto k = r.toFloat();
        if (up)
            tri.addTriangle (k.getX(), k.getBottom(), k.getRight(), k.getBottom(), k.getCentreX(), k.getY());
        else
            tri.addTriangle (k.getX(), k.getY(), k.getRight(), k.getY(), k.getCentreX(), k.getBottom());
        g.setColour (focused ? Dark::textDim : Dark::textDim.withAlpha (0.5f));
        g.fillPath (tri);
    }
}

void CustomControlsPage::paintLayer (juce::Graphics& g)
{
    g.setColour (Dark::header);
    g.fillRect (layerHeader);
    g.setColour (Dark::caption);
    g.setFont (panelTitleFont (layerHeader.getHeight()));
    g.drawText (cc::layerName (layer), layerHeader, juce::Justification::centred);
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
        const bool keysOnly = cc::isLowerLayer (layer);
        // [3.1] the encoder a 52 px tile 2 px above the element's bottom, a key
        // 40 px (a #707070 cap in a 4 px #303030 frame) 18 px from its top.
        const int s = keysOnly ? juce::jmin (r.getHeight() - 4, r.getWidth() * 2 / 3)
                               : juce::roundToInt (wingPx (e.slot == cc::Slot::Encoder ? 52.0f : 40.0f));
        auto square = keysOnly ? r.withSizeKeepingCentre (s, s)
                     : (e.slot == cc::Slot::Encoder ? juce::Rectangle<int> (r.getCentreX() - s / 2, r.getBottom() - juce::roundToInt (wingPx (2.0f)) - s, s, s)
                                                    : juce::Rectangle<int> (r.getCentreX() - s / 2, r.getY() + juce::roundToInt (wingPx (18.0f)), s, s));
        auto caption = r.withBottom (square.getY());
        juce::String text = e.slot == cc::Slot::Encoder && c.name.isNotEmpty() ? c.name : cc::describe (ctx, e.slot, f);
        g.setColour (Dark::caption);
        g.setFont (wingFontPx (juce::jlimit (6.0f, wingPx (11.0f), (float) caption.getHeight() * 0.6f)));
        g.drawFittedText (text.removeCharacters ("|*"), caption, juce::Justification::centredBottom, 1, 0.6f);
        if (e.slot == cc::Slot::Encoder)
        {
            // A panel-coloured tile in a 2 px black frame.
            const int frame = juce::jmax (1, juce::roundToInt (wingPx (2.0f)));
            g.setColour (juce::Colours::black);
            g.fillRect (square);
            g.setColour (Dark::body);
            g.fillRect (square.reduced (frame));
            // A knob: a thick ring open at the bottom (8 of 52 px, 34 px across).
            const auto k = square.toFloat();
            const float radius = (float) s * 13.0f / 52.0f, stroke = (float) s * 8.0f / 52.0f;
            juce::Path ring;
            ring.addCentredArc (k.getCentreX(), k.getCentreY(), radius, radius, 0.0f, juce::MathConstants<float>::pi * 1.2f,
                                juce::MathConstants<float>::pi * 2.8f, true);
            g.setColour (f.isAssigned() ? keyGrey : Colours::screenLine);
            g.strokePath (ring, juce::PathStrokeType (stroke));
        }
        else
        {
            g.setColour (Dark::key);
            g.fillRect (square);
            g.setColour (f.isAssigned() ? keyGrey : unsetGrey);
            g.fillRect (square.reduced (juce::jmax (1, s / 10)));
        }
        if (e.index == index && e.slot == slot)
        {
            g.setColour (Colours::yellow);
            g.drawRect (e.area.expanded (2).toFloat(), juce::jmax (2.0f, wingPx (3.0f)));
        }
    }
}

void CustomControlsPage::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);

    // Header: ‹ ›, USER (these controls) and GPIO (not here yet), the title.
    g.setColour (Dark::header);
    g.fillRect (header);
    arrowKey (g, prevKey, false);
    arrowKey (g, nextKey, true);
    for (auto [r, text, enabled] : { std::tuple { userKey, "USER", true }, std::tuple { gpioKey, "GPIO", true } })
    {
        g.setColour (juce::Colours::black);
        g.fillRect (r);
        g.setColour (enabled ? Dark::key : Dark::key.withAlpha (0.5f));
        g.fillRect (r.reduced (1));
        g.setColour (enabled ? Dark::caption : Dark::textDim.withAlpha (0.5f));
        g.setFont (wingFontPx (juce::jmin (TypeScale::body * getWingScreenHeight(), (float) r.getHeight() * 0.3f)));
        g.drawText (text, r, juce::Justification::centred);
    }
    g.setColour (Dark::caption);
    g.setFont (wingFontPx (juce::jmin (wingPx (19.0f), (float) title.getHeight() * 0.36f)));
    g.drawText ("CUSTOM CONTROLS", title, juce::Justification::centred);

    paintLayer (g);
    for (int i = 0; i < 3; ++i)
        paintList (g, lists[i], focusedList == i);

    for (auto [r, text] : { std::pair { settingsHeader, "SETTINGS" }, std::pair { controlHeader, "CONTROL" } })
    {
        g.setColour (Dark::header);
        g.fillRect (r);
        g.setColour (Dark::caption);
        g.setFont (wingFontPx (juce::jmin (TypeScale::body * getWingScreenHeight(), (float) r.getHeight() * 0.3f)));
        g.drawText (text, r, juce::Justification::centred);
    }
    g.setColour (Dark::body);
    for (auto r : { colourBox, clearBox, nameBox })
        g.fillRect (r);
    outlinedKey (g, colourKey, Icon::PaintBucket, "COLOR");
    outlinedKey (g, clearKey, Icon::Warning, "CLR ALL");
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
            for (auto& l : lists)
                l.scroll = 0;
            rebuildLists();
            revealSelection();
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
