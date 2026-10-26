#include "ui/DawSurface.h"

#include "ui/MidiRemoteUi.h"
#include "ui/FaderSection.h"

namespace livedaw::ui
{

namespace
{
    // The layer column's keys in DAW mode (WING DAW Documentation, "Layer
    // Buttons"): the seven layer keys of each section, then ◄4, 4►, VIEW and
    // SOF FLIP.
    const char* const mcuKeys[LayerSet::kNumSections][11] = {
        { "A1", "A3", "A2", "A5", "A4", "N6", "N7", "N8", "N9", "OT3", "" },     // left: 1-12 TRACK, 13-24 PAN, 25-36 SEND, AUX EQ, BUS PLUG-IN
        { "A3", "A2", "A5", "A1", "A4", "N6", "N7", "N8", "N9", "OT3", "OT1" },  // center: DCA PAN, MAIN SEND, AUX EQ, INPUTS TRACK, BUS PLUG-IN
        { "A1", "A3", "A2", "A5", "A4", "N6", "N7", "N8", "N9", "OT3", "" },     // right: MAIN TRACK, DCA PAN, INPUTS SEND, AUX EQ, BUS PLUG-IN
    };
    const char* const huiKeys[LayerSet::kNumSections][11] = {
        { "A7", "A8", "A9", "A10", "A11", "N6", "N7", "N8", "N9", "A3", "" },
        { "A8", "A9", "A10", "A12", "A11", "N6", "N7", "N8", "N9", "A3", "A7" },
        { "A7", "A8", "A9", "A10", "A11", "N6", "N7", "N8", "N9", "A3", "" },
    };

    /** The MCU meter (0..13) in dB, for the strip's six LEDs. */
    float meterDb (int level)
    {
        static const float db[] = { -100.0f, -60.0f, -50.0f, -40.0f, -30.0f, -20.0f, -14.0f, -10.0f, -8.0f, -6.0f, -4.0f, -2.0f, 0.0f, 0.0f };
        return db[juce::jlimit (0, 13, level)];
    }

    int ledOf (int led)
    {
        return led == DawRemote::On ? SurfaceKey::Lit : led == DawRemote::Blink ? SurfaceKey::Blink : SurfaceKey::Off;
    }
} // namespace

//==============================================================================
/** One DAW channel strip: colour bar and scribble strip, SELECT, SOLO, the
    meter block, MUTE and the motor fader (the DAW's fader position unless it
    is touched). */
class DawFaderSection::DawStrip final : public juce::Component, private juce::Timer
{
public:
    explicit DawStrip (UiContext& c) : ctx (c)
    {
        fader.setSliderStyle (juce::Slider::LinearVertical);
        fader.getProperties().set ("surfaceFader", true);
        fader.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        fader.setNormalisableRange (fader::range());
        fader.setMouseDragSensitivity (400);
        fader.onDragStart = [this]
        {
            if (auto* d = midiui::daw (ctx); d != nullptr && channel >= 0)
                d->touchFader (channel, true);
        };
        fader.onValueChange = [this]
        {
            if (auto* d = midiui::daw (ctx); d != nullptr && channel >= 0)
                d->moveFader (channel, fader::range().convertTo0to1 (fader.getValue()));
        };
        fader.onDragEnd = [this]
        {
            if (auto* d = midiui::daw (ctx); d != nullptr && channel >= 0)
                d->touchFader (channel, false);
        };
        addAndMakeVisible (fader);

        selectKey.ledColour = Colours::keySelect;
        soloKey.ledColour = Colours::keySolo;
        muteKey.ledColour = Colours::keyMute;
        const std::pair<TalkKey*, DawRemote::StripButton> keys[] = { { &selectKey, DawRemote::StripButton::Select },
                                                                     { &soloKey, DawRemote::StripButton::Solo },
                                                                     { &muteKey, DawRemote::StripButton::Mute } };
        for (auto [k, button] : keys)
        {
            k->backlit = true;
            const auto b = button;
            k->onTalk = [this, b] (bool down)
            {
                auto* d = midiui::daw (ctx);
                if (d == nullptr || channel < 0)
                    return;
                d->pressChannelKey (channel, b, down);
                // SELECT also brings the channel's block of four to the upper CC section.
                if (b == DawRemote::StripButton::Select && down)
                    d->setVpotBlock (channel / 4);
            };
            k->ledState = [this, b]
            {
                auto* d = midiui::daw (ctx);
                if (d == nullptr || channel < 0)
                    return (int) SurfaceKey::Off;
                const auto& s = d->getStrip (channel);
                return ledOf (b == DawRemote::StripButton::Select ? s.select : b == DawRemote::StripButton::Solo ? s.solo : s.mute);
            };
            addAndMakeVisible (k);
        }
        startTimerHz (25);
    }

    void setChannel (int c)
    {
        channel = c;
        for (juce::Component* k : { (juce::Component*) &selectKey, (juce::Component*) &soloKey, (juce::Component*) &muteKey, (juce::Component*) &fader })
            k->setEnabled (channel >= 0);
        repaint();
    }

    void setModuleEdges (bool first, bool last)
    {
        moduleFirst = first;
        moduleLast = last;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (surfaceColours().surface);
        auto band = [this, &g] (juce::Rectangle<int> rows, int margin, juce::Colour colour)
        {
            if (rows.isEmpty())
                return;
            const float left = moduleFirst ? 3.0f : 0.0f, right = (float) getWidth() - (moduleLast ? 3.0f : 0.0f);
            juce::Path p;
            p.addRoundedRectangle (left, (float) (rows.getY() - margin), right - left, (float) (rows.getHeight() + 2 * margin), 4.0f, 4.0f,
                                   moduleFirst, moduleLast, moduleFirst, moduleLast);
            g.setColour (colour);
            g.fillPath (p);
        };
        band (scribbleArea, 4, juce::Colours::black);
        band (meterArea, 4, juce::Colour (0xff141517));

        auto* d = midiui::daw (ctx);
        const bool active = d != nullptr && channel >= 0;
        drawLightBar (g, barArea.toFloat(), surfaceColours().silk.withMultipliedBrightness (uiprefs::brightness (ctx.settings, "colorLed", 25)),
                      active && d->isConnected (channel / DawRemote::kStrips));
        // A strip without a DAW channel shows nothing, as a blank layer slot.
        auto info = blankScribble();
        if (active)
        {
            info.backlight = uiprefs::brightness (ctx.settings, "scribble", 60);
            info.contrast = uiprefs::brightness (ctx.settings, "scrCont", 50);
            // The DAW's two lines: the value line in the middle, the track name at the bottom.
            const auto& s = d->getStrip (channel);
            info.number = juce::String (channel + 1);
            info.tag = channel < DawRemote::kStrips ? juce::String (d->isHui() ? "HUI" : "MCU") : "EXT" + juce::String (channel / DawRemote::kStrips);
            info.name = s.upper.isNotEmpty() ? s.upper : (d->isHui() ? "HUI " : "MCU ") + juce::String (channel % DawRemote::kStrips + 1);
            info.line2 = s.lower;
        }
        drawScribble (g, lcdArea.toFloat(), info);
        drawScribbleFrame (g, lcdArea.toFloat(), true, true, info.backlight);

        if (active && ! meterArea.isEmpty())
        {
            // The DAW's meter on the six LEDs (red at clip / overload).
            const auto& s = d->getStrip (channel);
            auto m = meterArea.toFloat();
            const float rowH = m.getHeight() / (float) FaderStrip::kMeterRows;
            const float ledW = juce::jlimit (5.0f, 10.0f, m.getWidth() * 0.14f);
            const float bright = uiprefs::brightness (ctx.settings, "meters", 40);
            const int lit = s.overload ? FaderStrip::kMeterSegments
                                       : leds::countLit (FaderStrip::kMeterThresholds, FaderStrip::kMeterSegments, meterDb (s.meter));
            auto columns = juce::Rectangle<float> (m.getCentreX() - ledW * 0.5f, m.getY() + rowH, ledW, rowH * FaderStrip::kMeterSegments);
            leds::drawColumn (g, columns, FaderStrip::kMeterSegments, lit,
                              [bright] (int i) { return leds::meterColour (FaderStrip::kMeterThresholds[i]).withMultipliedBrightness (bright); });
            // The V-pot ring as a row of LEDs under the meter.
            const int ring = DawRemote::ringLeds (s.ring);
            auto ringRow = juce::Rectangle<float> (m.getX() + 4.0f, m.getBottom() - rowH * 0.8f, m.getWidth() - 8.0f, rowH * 0.5f);
            const float lw = ringRow.getWidth() / 11.0f;
            for (int i = 0; i < 11; ++i)
            {
                g.setColour (((ring >> i) & 1) != 0 ? Colours::ledYellow.withMultipliedBrightness (bright) : surfaceColours().ledOff);
                g.fillRect (ringRow.getX() + lw * (float) i + 0.5f, ringRow.getY(), lw - 1.0f, ringRow.getHeight());
            }
        }
    }

    void resized() override
    {
        // As FaderStrip lays out a strip.
        auto b = getLocalBounds().reduced (2, 3);
        const int w = b.getWidth();
        const int innerW = juce::roundToInt ((float) w * 0.88f);
        const int lcdH = juce::jlimit (42, 78, juce::roundToInt ((float) w * 0.9f));
        const int barH = juce::jlimit (4, 8, lcdH / 7);
        scribbleArea = b.removeFromTop (barH + 4 + lcdH).withSizeKeepingCentre (innerW, barH + 4 + lcdH);
        barArea = scribbleArea.withHeight (barH);
        lcdArea = scribbleArea.withTrimmedTop (barH + 4);
        b.removeFromTop (8);
        const int keyH = juce::jlimit (18, 30, juce::roundToInt ((float) w * 0.46f));
        const int keyW = juce::roundToInt ((float) w * 0.76f);
        auto key = [&] (SurfaceKey& k) { k.setBounds (b.removeFromTop (keyH).withSizeKeepingCentre (keyW, keyH)); };
        key (selectKey);
        b.removeFromTop (5);
        key (soloKey);
        b.removeFromTop (8);
        // Short windows fold the meter away, as FaderStrip does.
        int meterH = juce::jmin (juce::jlimit (56, 100, juce::roundToInt ((float) keyH * 3.3f)),
                                 b.getHeight() - 8 - keyH - 6 - FaderStrip::kMinFaderTravel);
        if (meterH < 40)
            meterH = 0;
        meterArea = b.removeFromTop (meterH);
        if (meterH > 0)
            b.removeFromTop (8);
        key (muteKey);
        b.removeFromTop (6);
        fader.setBounds (b);
    }

    juce::Rectangle<int> getScribbleArea() const noexcept { return scribbleArea; }

private:
    void timerCallback() override
    {
        auto* d = midiui::daw (ctx);
        if (d == nullptr || channel < 0)
            return;
        // The motor fader follows the DAW unless it is touched.
        const auto& s = d->getStrip (channel);
        if (! fader.isMouseButtonDown())
        {
            const double v = fader::range().convertFrom0to1 ((double) s.fader / 16383.0);
            if (std::abs (v - fader.getValue()) > 1.0e-3)
                fader.setValue (v, juce::dontSendNotification);
        }
        const auto state = s.upper + "|" + s.lower + "|" + juce::String (s.meter) + "|" + juce::String ((int) s.overload) + "|" + juce::String (s.ring)
                           + "|" + juce::String ((int) d->isConnected (channel / DawRemote::kStrips));
        if (state != shownState)
        {
            shownState = state;
            repaint();
        }
    }

    UiContext& ctx;
    int channel = -1;
    juce::Slider fader;
    TalkKey selectKey { "SELECT" }, soloKey { "SOLO" }, muteKey { "MUTE" };
    juce::Rectangle<int> scribbleArea, barArea, lcdArea, meterArea;
    bool moduleFirst = false, moduleLast = false;
    juce::String shownState;
};

//==============================================================================
DawFaderSection::DawFaderSection (UiContext& c, LayerSet::Section s) : ctx (c), section (s)
{
    viewKey.isActive = [this]
    {
        auto* d = midiui::daw (ctx);
        return d != nullptr && d->getCommandLed (commandOf (9)) != DawRemote::Off;
    };
    viewKey.onPress = [this]
    {
        midiui::pressDawCommand (ctx, commandOf (9), true);
        midiui::pressDawCommand (ctx, commandOf (9), false);
    };
    viewKey.setTooltip ("DAW mode: NAME/VALUE (HUI: PAN)");
    addAndMakeVisible (viewKey);
    rebuildKeys();
    setNumStrips (section == LayerSet::Left ? 12 : section == LayerSet::Center ? 8 : 4);
    setVisible (false);
    startTimerHz (8);
}

DawFaderSection::~DawFaderSection() = default;

int DawFaderSection::commandOf (int key) const
{
    auto* d = midiui::daw (ctx);
    const auto& table = d != nullptr && d->isHui() ? huiKeys : mcuKeys;
    return DawRemote::findCommand (table[(size_t) section][juce::jlimit (0, 10, key)]);
}

int DawFaderSection::channelOf (int index) const noexcept
{
    static const int first[] = { 0, 12, 20 }, count[] = { 12, 8, 4 };
    auto* d = midiui::daw (ctx);
    if (d == nullptr || index >= count[(size_t) section])
        return -1;
    const int channel = first[(size_t) section] + index;
    return channel < d->getNumChannels() ? channel : -1;
}

void DawFaderSection::rebuildKeys()
{
    layerKeys.clear();
    // The section's own layer key labels (the print stays; the function changes).
    const auto layers = ctx.show.layers.getDefaultLayers (ctx.engine, section);
    const int numKeys = section == LayerSet::Center ? 10 : 9;
    for (int i = 0; i < numKeys; ++i)
    {
        juce::String label;
        if (i < 7)
            label = juce::isPositiveAndBelow (i, (int) layers.size()) ? LayerSet::keyLabel (layers[(size_t) i]) : juce::String();
        else if (i == 9)
            label = "SOF\nFLIP";
        auto* k = layerKeys.add (new TalkKey (label));
        if (i == 7 || i == 8)
        {
            k->setLabel ("4");
            k->arrow = i == 7 ? -1 : 1;
        }
        const int key = i == 9 ? 10 : i;
        k->onTalk = [this, key] (bool down) { midiui::pressDawCommand (ctx, commandOf (key), down); };
        k->ledState = [this, key] { return midiui::dawCommandLed (ctx, commandOf (key)); };
        k->ledColour = Colours::keyWhite;
        if (const auto* cmd = juce::isPositiveAndBelow (commandOf (key), (int) DawRemote::commands().size()) ? &DawRemote::commands()[(size_t) commandOf (key)] : nullptr)
            k->setTooltip (juce::String ("DAW mode: ") + cmd->name);
        addAndMakeVisible (k);
    }
    resized();
}

void DawFaderSection::setNumStrips (int n)
{
    n = juce::jmax (1, n);
    if (n == strips.size())
        return;
    strips.clear();
    for (int i = 0; i < n; ++i)
    {
        auto* s = strips.add (new DawStrip (ctx));
        s->setChannel (channelOf (i));
        addAndMakeVisible (s);
    }
    resized();
}

void DawFaderSection::timerCallback()
{
    auto* d = midiui::daw (ctx);
    const bool show = d != nullptr && d->isOn() && d->getSettings().configuration != DawRemote::CustomControlsOnly
                      && ! (ctx.isEditorShown && ctx.isEditorShown()) && ! (ctx.isScreenMaximized && ctx.isScreenMaximized());
    // Added after the fader sections, it lies over them (and under the
    // surface lock and the editor, which come later).
    if (show != isVisible())
        setVisible (show);
    if (! show)
        return;
    if (d->isHui() != wasHui)
    {
        wasHui = d->isHui();
        rebuildKeys();
    }
    for (int i = 0; i < strips.size(); ++i)
        strips[i]->setChannel (channelOf (i));
}

void DawFaderSection::paint (juce::Graphics& g)
{
    g.fillAll (surfaceColours().surface);
    g.setColour (surfaceColours().surfaceLine);
    g.fillRect (getWidth() - 1, 0, 1, getHeight());
    // DAW over the column: the section controls the DAW.
    g.setColour (surfaceColours().surfacePrint);
    g.setFont (silkFont (9.0f));
    g.drawText ("DAW", column.withHeight (14).translated (0, 2), juce::Justification::centred);
}

void DawFaderSection::resized()
{
    auto b = getLocalBounds();
    column = b.removeFromLeft (FaderSection::kBankColumnWidth);
    const int n = strips.size();
    if (n > 0)
    {
        const int w = b.getWidth() / n;
        for (int i = 0; i < n; ++i)
        {
            strips[i]->setBounds (b.getX() + i * w, b.getY(), w, b.getHeight());
            strips[i]->setModuleEdges (i % 4 == 0, i % 4 == 3 || i == n - 1);
        }
    }

    // As FaderSection lays out its column: VIEW level with the scribble strips,
    // the keys from the SELECT row down, SOF FLIP at the bottom of the center.
    auto c = column.reduced (5, 0);
    const auto scribble = n > 0 ? strips[0]->getScribbleArea() : juce::Rectangle<int> (0, 4, 10, 56);
    const int viewSize = juce::jmin (c.getWidth() - 6, 42);
    viewKey.setBounds (juce::Rectangle<int> (viewSize, viewSize).withCentre ({ c.getCentreX(), scribble.getCentreY() }));
    c.setTop (scribble.getBottom() + 8);
    if (layerKeys.size() > 9)
        layerKeys[9]->setBounds (c.removeFromBottom (36).withSizeKeepingCentre (c.getWidth(), 30));
    c.removeFromBottom (6);
    const int numKeys = 9;
    const int pitch = juce::jlimit (16, 38, c.getHeight() / numKeys);
    const int keyH = juce::jmin (30, pitch - juce::jmax (3, pitch / 6));
    for (int i = 0; i < juce::jmin (9, layerKeys.size()); ++i)
        layerKeys[i]->setBounds (c.removeFromTop (pitch).withSizeKeepingCentre (c.getWidth(), keyH));
}

} // namespace livedaw::ui
