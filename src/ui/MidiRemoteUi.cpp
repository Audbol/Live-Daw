#include "ui/MidiRemoteUi.h"

#include "engine/WingRemote.h"
#include "ui/CustomControlsPage.h"
#include "ui/FaderStrip.h"

namespace livedaw::ui::midiui
{

void connect (UiContext& ctx)
{
    if (ctx.midi == nullptr)
        return;
    auto& midi = *ctx.midi;
    midi.consoleName = ctx.settings.getValue (WingRemote::Settings::nameKey, "SIDECAR");

    // CUSTOM CONTROLS (RX ONLY): MIDI turns the encoders and presses the keys
    // of the upper part's layers, whatever layer the surface shows.
    midi.onCustomEncoder = [&ctx] (int layer, int control, int value)
    {
        if (ctx.customControls == nullptr)
            return;
        const auto binding = cc::bindEncoder (ctx, ctx.customControls->get (layer, control).encoder);
        if (binding.isValid() && binding.set)
            binding.set ((float) value / 127.0f);
    };
    midi.onCustomKey = [&ctx] (int layer, int key, bool down)
    {
        if (ctx.customControls == nullptr)
            return;
        const auto& c = ctx.customControls->get (layer, key % cc::kNumControls);
        const auto f = key < cc::kNumControls ? c.upper : c.lower;
        if (down)
            cc::press (ctx, f);
        else
            cc::release (ctx, f);
    };
    midi.onRecalled = [&ctx] (const juce::String& what, const state::ApplyResult& r) { logRecall (ctx, what, r); };
    midi.getDaw().onModeChanged = [&ctx]
    {
        if (ctx.customControlsChanged)
            ctx.customControlsChanged();
    };
    midi.apply (MidiRemote::Settings::load (ctx.settings));
}

void applySettings (UiContext& ctx, const MidiRemote::Settings& s)
{
    s.save (ctx.settings);
    if (ctx.midi != nullptr)
        ctx.midi->apply (s);
    if (ctx.customControlsChanged)
        ctx.customControlsChanged();
}

DawRemote* daw (UiContext& ctx)
{
    return ctx.midi != nullptr ? &ctx.midi->getDaw() : nullptr;
}

bool dawModeOn (UiContext& ctx)
{
    return ctx.midi != nullptr && ctx.midi->getDaw().isOn();
}

//==============================================================================
juce::String midiKeyTypeName (int type)
{
    static const char* names[] = { "CC TOGGLE", "CC PUSH", "NOTE TOGGLE", "NOTE PUSH", "PGM CHANGE" };
    return names[juce::jlimit (0, (int) kNumMidiKeyTypes - 1, type)];
}

KnobBinding bindDawVpot (UiContext& ctx, int function)
{
    // An endless knob: whole steps turn the V-pot; it centres itself again
    // once it rests.
    struct State
    {
        float position = 0.5f, pending = 0.0f;
        juce::uint32 lastMove = 0;
    };
    auto state = std::make_shared<State>();
    KnobBinding b;
    b.label = DawRemote::vpotFunctionName (function);
    b.get = [state]
    {
        if (juce::Time::getMillisecondCounter() - state->lastMove > 300)
            state->position = 0.5f;
        return state->position;
    };
    b.set = [state, &ctx, function] (float v)
    {
        state->pending += (v - state->position) * 48.0f;
        state->position = v;
        state->lastMove = juce::Time::getMillisecondCounter();
        if (const int steps = (int) state->pending; steps != 0)
        {
            state->pending -= (float) steps;
            if (auto* d = daw (ctx))
                d->turnVpotFunction (function, steps);
        }
    };
    b.text = [] { return juce::String(); };
    return b;
}

KnobBinding bindMidiCc (UiContext& ctx, int channel, int controller)
{
    KnobBinding b;
    b.label = "CC " + juce::String (controller);
    auto value = [&ctx, channel, controller] { return ctx.midi != nullptr ? ctx.midi->getExternalCc (channel + 1, controller) : 0; };
    b.get = [value] { return (float) value() / 127.0f; };
    b.set = [&ctx, channel, controller, value] (float v)
    {
        const int next = juce::jlimit (0, 127, juce::roundToInt (v * 127.0f));
        if (ctx.midi == nullptr || next == value())
            return;
        ctx.midi->setExternalCc (channel + 1, controller, next);
        ctx.midi->sendExternal (juce::MidiMessage::controllerEvent (channel + 1, controller, next));
    };
    b.text = [value] { return juce::String (value()); };
    b.steps = 127.0f;
    return b;
}

void pressDawCommand (UiContext& ctx, int command, bool down)
{
    if (auto* d = daw (ctx))
        d->pressCommand (command, down);
}

void pressMidiKey (UiContext& ctx, int target, int option, bool down)
{
    if (ctx.midi == nullptr)
        return;
    auto& midi = *ctx.midi;
    const int type = target / 16, channel = target % 16 + 1, number = juce::jlimit (0, 127, option);
    switch (type)
    {
        case CcToggle:
            if (down)
            {
                const bool on = ! midi.getExternalToggle (channel, number, false);
                midi.setExternalToggle (channel, number, false, on);
                midi.setExternalCc (channel, number, on ? 127 : 0);
                midi.sendExternal (juce::MidiMessage::controllerEvent (channel, number, on ? 127 : 0));
            }
            break;
        case CcPush:
            midi.setExternalToggle (channel, number, false, down);
            midi.sendExternal (juce::MidiMessage::controllerEvent (channel, number, down ? 127 : 0));
            break;
        case NoteToggle:
            if (down)
            {
                const bool on = ! midi.getExternalToggle (channel, number, true);
                midi.setExternalToggle (channel, number, true, on);
                midi.sendExternal (on ? juce::MidiMessage::noteOn (channel, number, (juce::uint8) 127) : juce::MidiMessage::noteOff (channel, number));
            }
            break;
        case NotePush:
            midi.setExternalToggle (channel, number, true, down);
            midi.sendExternal (down ? juce::MidiMessage::noteOn (channel, number, (juce::uint8) 127) : juce::MidiMessage::noteOff (channel, number));
            break;
        case ProgramChange:
            if (down)
                midi.sendExternal (juce::MidiMessage::programChange (channel, number));
            break;
        default: break;
    }
}

int dawCommandLed (UiContext& ctx, int command)
{
    auto* d = daw (ctx);
    if (d == nullptr)
        return 0;
    switch (d->getCommandLed (command))
    {
        case DawRemote::On:    return 1;
        case DawRemote::Blink: return 2;
        default:               return 0;
    }
}

bool midiKeyLit (UiContext& ctx, int target, int option)
{
    if (ctx.midi == nullptr)
        return false;
    const int type = target / 16;
    if (type == ProgramChange)
        return false;
    return ctx.midi->getExternalToggle (target % 16 + 1, juce::jlimit (0, 127, option), type == NoteToggle || type == NotePush);
}

juce::String describeDawCommand (UiContext& ctx, int command)
{
    const int preset = ctx.midi != nullptr ? ctx.midi->getSettings().daw.preset : 0;
    return DawRemote::commandLabel (command, preset);
}

juce::String describeMidiKey (int target, int option)
{
    const int type = target / 16;
    static const char* prefixes[] = { "CC", "CC", "NOTE", "NOTE", "PGM" };
    const int number = type == ProgramChange ? option + 1 : option;
    return juce::String (prefixes[juce::jlimit (0, (int) kNumMidiKeyTypes - 1, type)]) + juce::String (number) + "/" + juce::String (target % 16 + 1);
}

juce::String describeMidiCc (int channel, int controller)
{
    return "CC" + juce::String (controller) + "/" + juce::String (channel + 1);
}

//==============================================================================
bool upperIsDaw (UiContext& ctx, bool dawLayerSelected)
{
    auto* d = daw (ctx);
    return d != nullptr && d->isOn() && d->getSettings().upperCc && dawLayerSelected;
}

int upperChannel (UiContext& ctx, int column)
{
    auto* d = daw (ctx);
    return d != nullptr ? d->getVpotBlock() * 4 + juce::jlimit (0, 3, column) : column;
}

KnobBinding bindUpperVpot (UiContext& ctx, int column)
{
    // An endless knob; the channel follows the block (UP / DOWN) at the time of the turn.
    struct State
    {
        float position = 0.5f, pending = 0.0f;
        juce::uint32 lastMove = 0;
    };
    auto state = std::make_shared<State>();
    KnobBinding b;
    b.label = "V-POT";
    b.text = [] { return juce::String(); };
    b.get = [state]
    {
        if (juce::Time::getMillisecondCounter() - state->lastMove > 300)
            state->position = 0.5f;
        return state->position;
    };
    b.set = [state, &ctx, column] (float v)
    {
        state->pending += (v - state->position) * 48.0f;
        state->position = v;
        state->lastMove = juce::Time::getMillisecondCounter();
        if (const int steps = (int) state->pending; steps != 0)
        {
            state->pending -= (float) steps;
            if (auto* d = daw (ctx))
                d->turnVpot (upperChannel (ctx, column), steps);
        }
    };
    return b;
}

void pressUpperKey (UiContext& ctx, int key, bool down)
{
    auto* d = daw (ctx);
    if (d == nullptr)
        return;
    const int channel = upperChannel (ctx, key % 4);
    if (key < 4)
        d->pressVpot (channel, down);
    else
        d->pressStripButton (channel, DawRemote::StripButton::Rec, down);
}

int upperKeyLed (UiContext& ctx, int key)
{
    auto* d = daw (ctx);
    if (d == nullptr)
        return 0;
    const auto& s = d->getStrip (upperChannel (ctx, key % 4));
    // The upper key shows the LED under the V-pot, the lower one REC.
    const int led = key < 4 ? ((s.ring & 0x40) != 0 ? DawRemote::On : DawRemote::Off) : s.rec;
    return led == DawRemote::On ? 1 : led == DawRemote::Blink ? 2 : 0;
}

void stepUpperBlock (UiContext& ctx, int delta)
{
    if (auto* d = daw (ctx))
        d->setVpotBlock (d->getVpotBlock() + delta);
}

void paintUpperLcd (juce::Graphics& g, juce::Rectangle<int> area, UiContext& ctx)
{
    auto* d = daw (ctx);
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (area.toFloat(), 4.0f);
    if (d == nullptr)
        return;
    auto r = area.reduced (3, 4);
    const int w = r.getWidth() / 4;
    for (int i = 0; i < 4; ++i)
    {
        auto cell = juce::Rectangle<int> (r.getX() + i * w, r.getY(), w, r.getHeight()).reduced (2, 0);
        const int channel = upperChannel (ctx, i);
        const bool exists = channel < d->getNumChannels();
        const auto& s = d->getStrip (channel);
        // A white bar over the channels the controller has.
        drawLightBar (g, cell.removeFromTop (juce::jmax (4, cell.getHeight() / 9)).toFloat(), Colours::silk, exists);
        cell.removeFromTop (juce::jmax (3, cell.getHeight() / 8));
        // The V-pot ring under the text: eleven LEDs and the centre dot.
        auto ringArea = cell.removeFromBottom (juce::jmax (5, cell.getHeight() / 6));
        ScribbleInfo info;
        info.backlight = uiprefs::brightness (ctx.settings, "scribble", 60);
        info.contrast = uiprefs::brightness (ctx.settings, "scrCont", 50);
        info.tag = d->isHui() ? "HUI" : "MCU";
        info.number = juce::String (channel + 1);
        info.line2 = exists ? s.lower : juce::String();
        info.name = exists ? (s.upper.isNotEmpty() ? s.upper : "CH " + juce::String (channel + 1)) : juce::String ("-");
        drawScribble (g, cell.toFloat(), info);
        drawScribbleFrame (g, cell.toFloat(), true, true, info.backlight);
        const int leds = DawRemote::ringLeds (s.ring);
        const float lw = (float) ringArea.getWidth() / 12.0f;
        for (int l = 0; l < 11; ++l)
        {
            auto led = juce::Rectangle<float> ((float) ringArea.getX() + lw * ((float) l + 0.5f), (float) ringArea.getY() + 1.0f, lw * 0.75f,
                                               (float) ringArea.getHeight() - 2.0f);
            g.setColour ((leds >> l) & 1 ? Colours::ledYellow : Colours::silk.withAlpha (0.12f));
            g.fillRect (led);
        }
    }
}

juce::String assignmentDisplay (UiContext& ctx)
{
    auto* d = daw (ctx);
    return d != nullptr && d->isOn() ? d->getAssignment().trim() : juce::String();
}

void turnWheel (UiContext& ctx, int steps)
{
    auto* d = daw (ctx);
    if (d == nullptr)
        return;
    // DISABLE WHEEL DURING PLAY: nothing while the DAW's PLAY lights.
    if (d->getSettings().disableWheel && d->getCommandLed (DawRemote::findCommand ("T2")) != DawRemote::Off)
        return;
    d->turnJog (steps);
}

void pressArrow (UiContext& ctx, int dx, int dy)
{
    auto* d = daw (ctx);
    if (d == nullptr)
        return;
    const char* code = dy < 0 ? "N1" : dy > 0 ? "N2" : dx < 0 ? "N3" : "N4";
    const int command = DawRemote::findCommand (code);
    d->pressCommand (command, true);
    d->pressCommand (command, false);
}

bool pressDawKey (UiContext& ctx)
{
    auto* d = daw (ctx);
    if (d == nullptr || ! d->getSettings().enabled)
        return false;
    d->setOn (! d->isOn());
    ctx.log.info (d->isOn() ? "DAW remote control on" : "DAW remote control off");
    return true;
}

} // namespace livedaw::ui::midiui
