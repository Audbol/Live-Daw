#include "ui/FourChannelSection.h"
#include "engine/Models.h"
#include "ui/CustomControlsPage.h"
#include "ui/FaderStrip.h"
#include "ui/WingPopups.h"

namespace livedaw::ui
{

namespace
{
    const char* const modeLabels[FourChannelSection::kNumModes] = { "GAIN\n48 V", "GATE", "COMP", "FILTER", "F1", "F2", "F3", "PAN\nMAIN" };

    /** The threshold of a dynamics slot's model (its first parameter if it has none). */
    KnobBinding thresholdOf (DynSlotParams& slot, const juce::String& label)
    {
        const auto& spec = models::model (models::Dynamics, slot.model.load());
        int i = spec.find ("thr");
        if (i < 0)
            i = spec.find ("1-thr");
        if (i < 0 && ! spec.params.empty())
            i = 0;
        if (i < 0)
            return {};
        auto b = bindModelParam (slot.values[i], spec.params[(size_t) i]);
        b.label = label;
        return b;
    }
} // namespace

FourChannelSection::FourChannelSection (UiContext& c, std::function<Strip* (int)> at) : ctx (c), stripAt (std::move (at))
{
    for (int i = 0; i < kNumModes; ++i)
    {
        auto* k = modeKeys.add (new SurfaceKey (modeLabels[i]));
        k->ledColour = Colours::keyLayer;
        k->onPress = [this, i] { setMode (i); };
        k->ledState = [this, i] { return mode == i ? SurfaceKey::Lit : SurfaceKey::Off; };
        if (i >= F1 && i <= F3)
        {
            k->onHold = [this, i] { chooseBus (i - F1); };
            k->setTooltip ("Sends to a bus on the four right strips (hold: choose the bus)");
        }
        addAndMakeVisible (k);
    }
    modeKeys[Gain]->setTooltip ("GAIN: input trim; the key below switches phantom power (no preamps here)");
    modeKeys[Filter]->setTooltip ("FILTER: low cut (press again: high cut); the key below switches the low cut");
    modeKeys[Pan]->setTooltip ("PAN: pan (press again: width); the key below switches the MAIN 1 send");

    for (int i = 0; i < 4; ++i)
    {
        knobs[i].setHardware (true);
        addAndMakeVisible (knobs[i]);
        auto* k = channelKeys.add (new SurfaceKey());
        k->shape = SurfaceKey::Shape::Square;
        k->onPress = [this, i]
        {
            if (const auto* control = customControl (i))
            {
                cc::press (ctx, control->upper);
                return;
            }
            auto* s = strips[(size_t) i];
            if (s == nullptr)
                return;
            auto& p = s->params;
            auto flip = [] (std::atomic<bool>& b) { b = ! b.load(); };
            switch (mode)
            {
                case Gate:   if (s->getKind() == StripKind::Channel) flip (p.gate.on); break;
                case Comp:   flip (p.dynamics.on); break;
                case Filter: if (s->getKind() == StripKind::Channel) flip (p.filter.lowCut); break;
                case Pan:    flip (p.mains[0].on); break;
                case F1:
                case F2:
                case F3:
                    if (s->isInput() || s->isBus())
                        flip (s->sends[(size_t) busOf (mode - F1)].enabled);
                    break;
                default: break;
            }
        };
        k->ledState = [this, i]
        {
            if (const auto* control = customControl (i))
                return cc::isLit (ctx, control->upper) ? SurfaceKey::Lit : SurfaceKey::Off;
            auto* s = strips[(size_t) i];
            if (s == nullptr)
                return SurfaceKey::Off;
            auto& p = s->params;
            bool on = false;
            switch (mode)
            {
                case Gate:   on = p.gate.on.load(); break;
                case Comp:   on = p.dynamics.on.load(); break;
                case Filter: on = p.filter.lowCut.load(); break;
                case Pan:    on = p.mains[0].on.load(); break;
                case F1:
                case F2:
                case F3:
                    if (auto* send = s->sends.find (busOf (mode - F1)))
                        on = send->enabled.load();
                    break;
                default: break;
            }
            return on ? SurfaceKey::Lit : SurfaceKey::Off;
        };
        addAndMakeVisible (k);
    }
    startTimerHz (8);
    refresh();
}

FourChannelSection::~FourChannelSection() = default;

int FourChannelSection::busOf (int fKey) const
{
    const int buses = juce::jmax (1, (int) ctx.engine.getBuses().size());
    return juce::jlimit (0, buses - 1, ctx.settings.getIntValue ("ui.fourChannelBus." + juce::String (fKey), fKey));
}

void FourChannelSection::chooseBus (int fKey)
{
    if (ctx.settings.getBoolValue (uiprefs::f1f3AsCustom, false))
        return; // U1-U3: CC EDIT assigns them
    juce::StringArray items;
    for (auto& b : ctx.engine.getBuses())
        items.add (shortStripLabel (*b) + "  " + stripName (*b).toUpperCase());
    popups::ListOptions options;
    options.columns = items.size() > 12 ? 2 : 1;
    options.caption = "F" + juce::String (fKey + 1) + " SENDS TO";
    juce::Component::SafePointer<FourChannelSection> safe (this);
    popups::chooseFrom (*modeKeys[F1 + fKey], items, busOf (fKey), [safe, fKey] (int i)
    {
        if (safe == nullptr)
            return;
        safe->ctx.settings.setValue ("ui.fourChannelBus." + juce::String (fKey), i);
        safe->setMode (F1 + fKey);
        safe->bind();
    }, options);
}

void FourChannelSection::setMode (int m)
{
    // FILTER and PAN pressed again switch to their second parameter.
    alternate = m == mode && (m == Filter || m == Pan) ? ! alternate : false;
    mode = m;
    bind();
    repaint();
}

void FourChannelSection::refresh()
{
    for (int i = 0; i < 4; ++i)
        strips[(size_t) i] = stripAt ? stripAt (i) : nullptr;
    bind();
    repaint (lcd);
}

const cc::Control* FourChannelSection::customControl (int i) const
{
    // USE F1-F3 AS CUSTOM CONTROLS: F1-F3 are the custom control layers U1-U3.
    if (mode < F1 || mode > F3 || ctx.customControls == nullptr || ! ctx.settings.getBoolValue (uiprefs::f1f3AsCustom, false))
        return nullptr;
    return &ctx.customControls->get (cc::kFirstFourChannelLayer + mode - F1, i);
}

void FourChannelSection::bind()
{
    for (int i = 0; i < 4; ++i)
    {
        auto* s = strips[(size_t) i];
        KnobBinding b;
        if (const auto* c = customControl (i))
        {
            knobs[i].setBinding (cc::bindEncoder (ctx, c->encoder));
            channelKeys[i]->setEnabled (c->upper.isAssigned());
            continue;
        }
        if (s != nullptr)
        {
            auto& p = s->params;
            const bool channel = s->getKind() == StripKind::Channel;
            switch (mode)
            {
                case Gain:   b = bindLinear ("TRIM", p.input.trimDb, -18.0f, 18.0f, " dB", 1, 0.0f); break;
                case Gate:   if (channel) b = thresholdOf (p.gate, "GATE THR"); break;
                case Comp:   b = thresholdOf (p.dynamics, "COMP THR"); break;
                case Filter:
                    if (channel)
                        b = alternate ? bindFreq ("HI CUT", p.filter.highCutFreq, 50.0f, 20000.0f, 10000.0f)
                                      : bindFreq ("LO CUT", p.filter.lowCutFreq, 20.0f, 2000.0f, 80.0f);
                    break;
                case Pan:
                    b = alternate ? bindLinear ("WIDTH", p.width, -150.0f, 150.0f, "%", 0, 100.0f) : bindPan ("PAN", p.pan);
                    break;
                case F1:
                case F2:
                case F3:
                    if (s->isInput() || s->isBus())
                        b = bindFader ("BUS " + juce::String (busOf (mode - F1) + 1), s->sends[(size_t) busOf (mode - F1)].levelDb);
                    break;
                default: break;
            }
        }
        knobs[i].setBinding (b);
        channelKeys[i]->setEnabled (s != nullptr && mode != Gain);
    }
    shownState = {};
}

juce::String FourChannelSection::modeCaption() const
{
    switch (mode)
    {
        case Gain:   return "TRIM";
        case Gate:   return "GATE";
        case Comp:   return "COMP";
        case Filter: return alternate ? "HI CUT" : "LO CUT";
        case Pan:    return alternate ? "WIDTH" : "PAN";
        case F1:
        case F2:
        case F3:
            if (ctx.settings.getBoolValue (uiprefs::f1f3AsCustom, false))
                return "U" + juce::String (mode - F1 + 1);
            return "B" + juce::String (busOf (mode - F1) + 1);
        default:     return {};
    }
}

void FourChannelSection::timerCallback()
{
    // Custom control layers U1-U3 follow CC EDIT.
    if (ctx.customControls != nullptr && ctx.customControls->getRevision() != shownCcRevision)
    {
        shownCcRevision = ctx.customControls->getRevision();
        bind();
    }
    // Follow the right section's faders (layer changes, shift keys) and the values.
    juce::String state;
    bool changed = false;
    for (int i = 0; i < 4; ++i)
    {
        auto* s = stripAt ? stripAt (i) : nullptr;
        if (s != strips[(size_t) i])
            changed = true;
        if (s != nullptr)
            state << stripName (*s) << s->getColourIndex() << scribbleSourceTag (*s, ctx.settings.getBoolValue (uiprefs::sourceOnScribble, true))
                  << s->params.icon.load() << '|';
        if (channelKeys[i]->ledState)
            state << (int) channelKeys[i]->ledState();
    }
    if (changed)
        refresh();
    for (auto& k : knobs)
        if (const auto& b = k.getBinding(); b.isValid() && b.text)
            state << b.text();
    if (state != shownState)
    {
        shownState = state;
        repaint (lcd);
    }
}

void FourChannelSection::paint (juce::Graphics& g)
{
    g.fillAll (Colours::panel);
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.fillRect (getWidth() - 1, 0, 1, getHeight());

    // The key panel and the knob panel are printed frames, joined at the left
    // by a bracket; the LCD sits in a black window between them.
    g.setColour (Colours::sectionLine.withAlpha (0.8f));
    g.drawRoundedRectangle (keyPanel.toFloat().reduced (0.5f), 4.0f, 1.2f);
    g.drawRoundedRectangle (knobPanel.toFloat().reduced (0.5f), 4.0f, 1.2f);
    {
        const float x = (float) panel.getX() + 1.0f, y0 = (float) keyPanel.getCentreY(), y1 = (float) knobPanel.getCentreY();
        juce::Path bracket;
        bracket.startNewSubPath ((float) keyPanel.getX() - 3.0f, y0);
        bracket.lineTo (x + 4.0f, y0);
        bracket.quadraticTo (x, y0, x, y0 + 4.0f);
        bracket.lineTo (x, y1 - 4.0f);
        bracket.quadraticTo (x, y1, x + 4.0f, y1);
        bracket.lineTo ((float) knobPanel.getX() - 3.0f, y1);
        g.strokePath (bracket, juce::PathStrokeType (1.2f));
    }

    // The LCD: the four strips with the mode's value under the name.
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (lcd.toFloat().expanded (3.0f), 4.0f);
    auto r = lcd.reduced (3, 4);
    const int w = r.getWidth() / 4;
    for (int i = 0; i < 4; ++i)
    {
        auto cell = juce::Rectangle<int> (r.getX() + i * w, r.getY(), w, r.getHeight());
        auto* s = strips[(size_t) i];
        if (s != nullptr)
            drawLightBar (g, cell.removeFromTop (juce::jmax (4, cell.getHeight() / 9)).toFloat().reduced ((float) w * 0.15f, 0.5f), stripDisplayColour (*s), s->params.led.load());
        else
            cell.removeFromTop (juce::jmax (4, cell.getHeight() / 9));
        cell.removeFromTop (3);
        ScribbleInfo info;
        if (const auto* c = customControl (i))
        {
            // A custom control layer: the layer, the encoder's value large
            // with the control's name, and its key.
            info.tag = "U" + juce::String (mode - F1 + 1);
            info.tagFirst = true;
            info.number = juce::String (i + 1);
            const auto& b = knobs[i].getBinding();
            info.value = b.isValid() && b.text ? b.text() : juce::String ("-");
            info.caption = c->name.isNotEmpty() ? c->name.removeCharacters ("*|") : cc::describe (ctx, cc::Slot::Encoder, c->encoder);
            info.name = cc::describe (ctx, cc::Slot::Upper, c->upper);
        }
        else if (s != nullptr)
        {
            // As the console's: the knob's value large with what it sets
            // under it, and the key's state under a rule ("-10.0" / "THR" /
            // "OFF"); the strip itself shows on the fader's scribble below.
            const auto& b = knobs[i].getBinding();
            info.value = b.isValid() && b.text ? b.text() : juce::String ("-");
            if (info.value.endsWith (" dB"))
                info.value = info.value.dropLastCharacters (3);
            info.caption = b.label.endsWith (" THR") ? juce::String ("THR") : modeCaption();
            if (channelKeys[i]->isEnabled() && channelKeys[i]->ledState)
                info.footer = channelKeys[i]->ledState() == SurfaceKey::Lit ? "ON" : "OFF";
        }
        info.backlight = uiprefs::brightness (ctx.settings, "scribble", 60);
        info.contrast = uiprefs::brightness (ctx.settings, "scrCont", 50);
        drawScribble (g, cell.toFloat(), info);
        // One LCD, a fine rule between the strips.
        drawScribbleFrame (g, cell.toFloat(), i == 0, i == 3, info.backlight);
    }

    // Rules between the encoder columns.
    g.setColour (Colours::silk.withAlpha (0.35f));
    for (int i = 1; i < 4; ++i)
    {
        const int x = knobPanel.getX() + knobPanel.getWidth() * i / 4;
        g.fillRect (x, knobPanel.getY() + 12, 1, knobPanel.getHeight() - 24);
    }
}

void FourChannelSection::resized()
{
    panel = getLocalBounds().reduced (6, 6);
    auto b = panel.withTrimmedLeft (12).reduced (4, 4);
    heading = {};

    const int h = b.getHeight();
    const int keyRowH = juce::jlimit (18, 32, h / 8);
    keyPanel = b.removeFromTop (keyRowH * 2 + 26);
    b.removeFromTop (6);
    b.removeFromTop (6);
    lcd = b.removeFromTop (juce::jlimit (40, 96, h / 5)).reduced (4, 0);
    b.removeFromTop (10);
    knobPanel = b;

    auto keys = keyPanel.reduced (6, 9);
    const int kw = keys.getWidth() / 4;
    for (int i = 0; i < kNumModes; ++i)
    {
        const int row = i / 4, col = i % 4;
        modeKeys[i]->setBounds (juce::Rectangle<int> (keys.getX() + col * kw, keys.getY() + row * (keyRowH + 8), kw, keyRowH).reduced (3, 0));
    }

    // Each column: the encoder in the upper part, its square key below.
    auto k = knobPanel.reduced (4, 8);
    const int cw = k.getWidth() / 4;
    const int keyH = juce::jlimit (16, 30, juce::jmin (k.getHeight() / 5, cw * 45 / 100));
    const int knob = juce::jlimit (24, 60, juce::jmin (cw * 70 / 100, k.getHeight() - keyH - 14));
    for (int i = 0; i < 4; ++i)
    {
        auto col = juce::Rectangle<int> (k.getX() + i * cw, k.getY(), cw, k.getHeight());
        knobs[i].setBounds (juce::Rectangle<int> (knob, knob).withCentre ({ col.getCentreX(), col.getY() + juce::jmax (knob / 2 + 2, col.getHeight() * 36 / 100) }));
        channelKeys[i]->setBounds (juce::Rectangle<int> (keyH + 4, keyH + 4).withCentre ({ col.getCentreX(), col.getY() + juce::jmin (col.getHeight() - keyH / 2 - 4, col.getHeight() * 78 / 100) }));
    }
}

} // namespace livedaw::ui
