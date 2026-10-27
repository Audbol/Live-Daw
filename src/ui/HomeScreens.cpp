#include "ui/HomeScreens.h"
#include "ui/Controls.h"
#include "ui/DynamicsGraph.h"
#include "ui/Faceplate.h"
#include "ui/MeterComponent.h"
#include "ui/ModelParams.h"
#include "ui/ResponseGraph.h"
#include "ui/WingPopups.h"
#include "ui/WingWidgets.h"

namespace livedaw::ui
{

using namespace home;

namespace
{
    /** FREEZE: holds the RTA (the EQ and FILTER screens share it). */
    void bindFreeze (IconKey& k, const Strip& s)
    {
        k.isOn = [a = Analysis (s)] { return a.isFrozen(); };
        k.onClick = [a = Analysis (s)]() mutable { a.setFrozen (! a.isFrozen()); };
    }

    /** A LISTEN key of `s` (BAND SOLO, KEY SOLO, XOVER SOLO, ld::ListenMode):
        it plays on the solo bus instead of the soloed strips, lit while it
        does; press again to stop. */
    void bindListen (IconKey& k, const Strip& s, int mode, std::function<int()> band = {})
    {
        k.isOn = [a = Analysis (s), &s, mode] { return a.isListening (s, mode); };
        k.onClick = [a = Analysis (s), &s, mode, band]() mutable { a.toggleListen (s, mode, band ? band() : 0); };
    }

    /** A dynamics parameter on the parameter bar as the console colours it
        (Fig. 92-94): thresholds white, ratio and knee yellow, times green
        (each as a thumb on a grey line), ranges and gains as a blue fill. */
    KnobStyle dynamicsStyle (const models::Param& p, juce::Component* target)
    {
        const auto id = p.id.toLowerCase();
        const auto label = p.label.toUpperCase();
        if (id == "thr" || id.endsWith ("thr") || label.startsWith ("THR"))
            return styleOf (KnobStyle::Bar::Thumb, KnobColours::white, target);
        if (id.contains ("ratio") || id == "knee" || label == "RATIO" || label == "KNEE")
            return styleOf (KnobStyle::Bar::Thumb, KnobColours::yellow, target);
        if (p.unit == "ms" || p.unit == "seconds" || label.contains ("ATTACK") || label.contains ("RELEASE") || label.contains ("HOLD")
            || label.contains ("REC"))
            return styleOf (KnobStyle::Bar::Thumb, KnobColours::timeGreen, target);
        if (id == "range" || id == "depth" || (p.unit == "dB" && p.isContinuous()))
            return styleOf (KnobStyle::Bar::Fill, KnobColours::blue, target);
        return styleOf (KnobStyle::Bar::Auto, {}, target);
    }
} // namespace

//==============================================================================
void HomeScreen::paint (juce::Graphics& g)
{
    for (auto& p : boxes)
        drawSectionBox (g, p.area, p.title, p.chevron);
}

namespace
{
    /** A caption on a HOME screen: `size` px tall on the 500 px screen of a
        1920 x 1080 window and in proportion on other screens, no taller than
        the label. */
    class ScreenLabel final : public juce::Label
    {
    public:
        explicit ScreenLabel (float sizePx) : size (sizePx) {}

        void resized() override
        {
            const float px = size * getWingScreenHeight() / 500.0f;
            setFont (wingFontPx (juce::jmin (px * 0.7f, (float) getHeight() * 0.62f)));
            juce::Label::resized();
        }

    private:
        float size;
    };
} // namespace

juce::Label& HomeScreen::label (const juce::String& text, float size, juce::Colour colour)
{
    auto& l = make<ScreenLabel> (size);
    l.setText (text, juce::dontSendNotification);
    l.setFont (juce::FontOptions (size, juce::Font::bold));
    l.setColour (juce::Label::textColourId, colour);
    l.setMinimumHorizontalScale (0.7f);
    return l;
}

DisplayKnob& HomeScreen::knob (KnobBinding b)
{
    auto& k = make<DisplayKnob>();
    k.setCompact (true);
    k.setBinding (std::move (b));
    return k;
}

BoolButton& HomeScreen::toggle (const juce::String& text, std::atomic<bool>& p, juce::Colour on)
{
    auto& b = make<BoolButton> (text, on);
    b.bind (&p);
    return b;
}

IntCombo& HomeScreen::combo (const juce::StringArray& items, std::atomic<int32_t>* p, const juce::String& noneText)
{
    auto& c = make<IntCombo>();
    c.setItems (items, noneText);
    c.bind (p);
    return c;
}

IconKey& HomeScreen::key (const juce::String& text, std::optional<Icon> icon, juce::Colour on)
{
    return make<IconKey> (text, icon, on);
}

ChoiceField& HomeScreen::field (const juce::String& caption, const juce::StringArray& items, std::atomic<int32_t>& value, const juce::String& noneText)
{
    return make<ChoiceField> (caption,
                              [items, &value, noneText]
                              {
                                  const int v = value.load();
                                  return v < 0 ? noneText : items[v];
                              },
                              [items, &value, noneText, caption] (ChoiceField& f)
                              {
                                  juce::StringArray list;
                                  if (noneText.isNotEmpty())
                                      list.add (noneText);
                                  list.addArray (items);
                                  const int offset = noneText.isNotEmpty() ? 1 : 0;
                                  popups::ListOptions options;
                                  options.columns = list.size() > 30 ? 3 : (list.size() > 12 ? 2 : 1);
                                  options.caption = caption;
                                  popups::chooseFrom (f, list, value.load() + offset, [&value, offset] (int i) { value = i - offset; }, options);
                              });
}

juce::Rectangle<int> HomeScreen::box (juce::Rectangle<int> area, const juce::String& title, bool chevron)
{
    boxes.push_back ({ area, title, chevron });
    return sectionContent (area, title.isNotEmpty());
}

LevelPanel& HomeScreen::addLevelPanel()
{
    levelPanel = &make<LevelPanel> (ctx, strip);
    return *levelPanel;
}

juce::Rectangle<int> HomeScreen::layoutLevelPanel (juce::Rectangle<int> area)
{
    if (levelPanel != nullptr)
    {
        const auto levelArea = area.removeFromRight (area.getWidth() * 22 / 100);
        box (levelArea, "LEVEL");
        levelPanel->setBounds (sectionBody (levelArea, true));
        area.removeFromRight (juce::jmax (4, getParentWidth() / 160));
    }
    return area;
}

void layoutRow (juce::Rectangle<int> r, std::initializer_list<juce::Component*> comps, int gap)
{
    const int n = (int) comps.size();
    if (n == 0)
        return;
    const int w = (r.getWidth() - gap * (n - 1)) / n;
    for (auto* c : comps)
    {
        if (c != nullptr)
            c->setBounds (r.removeFromLeft (w));
        r.removeFromLeft (gap);
    }
}

juce::StringArray channelNames (MixerEngine& engine)
{
    juce::StringArray items;
    for (auto& ch : engine.getChannels())
        items.add (describeStrip (*ch));
    return items;
}

KnobBinding thresholdBinding (DynSlotParams& slot, const juce::String& label)
{
    const auto& spec = models::model (models::Dynamics, slot.model.load());
    if (spec.params.empty())
        return {};
    const int i = juce::jmax (0, spec.find ("thr"));
    auto b = bindConsoleParam (slot.values[i], spec.params[(size_t) i]);
    b.label = label;
    return b;
}

MaskButtons::MaskButtons (std::atomic<uint32_t>& m, int count, const juce::String& prefix, juce::Colour on, int columns) : mask (m), numColumns (columns)
{
    for (int i = 0; i < count; ++i)
    {
        auto* b = buttons.add (new juce::TextButton (prefix + juce::String (i + 1)));
        b->setClickingTogglesState (true);
        b->setColour (juce::TextButton::buttonOnColourId, on);
        b->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        b->onClick = [this, i, b]
        {
            const auto bit = 1u << i;
            mask = b->getToggleState() ? (mask.load() | bit) : (mask.load() & ~bit);
        };
        addAndMakeVisible (b);
    }
    startTimerHz (8);
    timerCallback();
}

void MaskButtons::resized()
{
    const int n = buttons.size();
    const int columns = juce::jmin (n, numColumns);
    const int rows = (n + columns - 1) / juce::jmax (1, columns);
    const int w = getWidth() / juce::jmax (1, columns), h = getHeight() / juce::jmax (1, rows);
    for (int i = 0; i < n; ++i)
        buttons[i]->setBounds ((i % columns) * w, (i / columns) * h, w - 3, h - 3);
}

void MaskButtons::timerCallback()
{
    for (int i = 0; i < buttons.size(); ++i)
        buttons[i]->setToggleState ((mask.load() >> i) & 1u, juce::dontSendNotification);
}

namespace
{

//==============================================================================
// Delay units: the engine stores the delay in the unit selected (M, FT, MS, SMP).
// The WING turns distance into time at 0.3 m per ms (livedaw_dsp::delay):
// 0.1 m reads 0.3 ft, 0.3 ms and 16 samples at 48 kHz.
constexpr float kSpeedOfSound = 300.0f;
// The WING's range (`/ch/n/in/set/dly`: DIST 0.1 .. 150 m) in any unit; SAMPLES
// starts at 16 at every rate.
constexpr float kMinDelayM = 0.1f, kMaxDelayM = 150.0f, kMinDelaySamples = 16.0f;

float sampleRateOf (UiContext& ctx)
{
    const double sr = ctx.engine.getSampleRate();
    return sr > 0.0 ? (float) sr : 48000.0f;
}

float delayToMs (float value, int mode, float sr)
{
    switch (mode)
    {
        case 0:  return value / kSpeedOfSound * 1000.0f;
        case 1:  return value * 0.3048f / kSpeedOfSound * 1000.0f;
        case 2:  return value;
        default: return value / sr * 1000.0f;
    }
}

float msToDelay (float ms, int mode, float sr)
{
    switch (mode)
    {
        case 0:  return ms * kSpeedOfSound / 1000.0f;
        case 1:  return ms * kSpeedOfSound / 1000.0f / 0.3048f;
        case 2:  return ms;
        default: return juce::jmax (kMinDelaySamples, std::round (ms * sr / 1000.0f));
    }
}

/** The delay range in `mode`'s unit. */
juce::NormalisableRange<float> delayRange (int mode, float sr)
{
    auto inUnit = [mode, sr] (float metres) { return msToDelay (delayToMs (metres, 0, sr), mode, sr); };
    return { inUnit (kMinDelayM), inUnit (kMaxDelayM) };
}

juce::String delayText (float value, int mode)
{
    static const char* units[] = { " m", " ft", " ms", "" };
    return (mode == 3 ? juce::String (juce::roundToInt (value)) : juce::String (value, 1)) + units[juce::jlimit (0, 3, mode)];
}

/** The delay as a knob: linear over the WING's range in the current unit. */
KnobBinding delayBinding (UiContext& ctx, InputParams& in)
{
    KnobBinding b;
    const int mode = juce::jlimit (0, 3, (int) in.delayMode.load());
    b.label = mode <= 1 ? "DIST" : (mode == 2 ? "TIME" : "SAMPLES");
    auto range = [&ctx, &in] { return delayRange (juce::jlimit (0, 3, (int) in.delayMode.load()), sampleRateOf (ctx)); };
    b.get = [&in, range] { return juce::jlimit (0.0f, 1.0f, range().convertTo0to1 (in.delay.load())); };
    b.set = [&in, range] (float p)
    {
        const float v = range().convertFrom0to1 (juce::jlimit (0.0f, 1.0f, p));
        in.delay = in.delayMode.load() == 3 ? std::round (v) : v; // whole samples
    };
    b.text = [&in] { return delayText (in.delay.load(), juce::jlimit (0, 3, (int) in.delayMode.load())); };
    b.reset = [&in, range] { in.delay = range().start; };
    b.steps = 1500.0f;
    return b;
}

/** A vertical delay slider: a dark track with a scale and the value in a tag. */
class DelaySlider final : public juce::Component, private juce::Timer
{
public:
    DelaySlider (UiContext& c, InputParams& p) : ctx (c), in (p) { startTimerHz (12); }

    void paint (juce::Graphics& g) override
    {
        // WING 3.1 (a 116 x 360 slider): the scale right-aligned in the left
        // 36 px; the 76 px well (2 px black rim, #101010) with a 12 px black
        // slot from 10 px under its top; the value in a tag across the well's
        // inside, 25 tall, at the value (grey while the delay is off).
        auto r = getLocalBounds().toFloat();
        const auto b = delayBinding (ctx, in);
        const float u = r.getWidth() / 116.0f;
        const float tagH = juce::jmin (25.0f * u, r.getHeight() * 0.1f);
        auto scale = r.removeFromLeft (36.0f * u);
        r.removeFromLeft (4.0f * u);
        const auto well = r;
        const float rim = juce::jmax (1.0f, 2.0f * u);
        auto track = well.withSizeKeepingCentre (12.0f * u, well.getHeight()).withTrimmedTop (10.0f * u).withTrimmedBottom (rim + tagH * 0.5f);
        const int mode = juce::jlimit (0, 3, (int) in.delayMode.load());
        const auto range = delayRange (mode, sampleRateOf (ctx));
        g.setFont (homeFont (TypeScale::caption, juce::jlimit (9.0f, screenCap (12.0f), r.getHeight() * 0.045f)));
        g.setColour (Colours::textDim);
        // [I7] the minimum with its unit (0.1 m), then five equal steps to the top.
        for (int i = 0; i <= 5; ++i)
        {
            const float frac = (float) i / 5.0f;
            const float y = track.getBottom() - track.getHeight() * frac;
            const float v = i == 0 ? range.start : range.end * frac;
            const auto text = i == 0 ? delayText (v, mode) : juce::String (v, mode == 3 || v >= 10.0f ? 0 : 1);
            g.drawText (text, scale.withY (y - 6.0f).withHeight (12.0f).toNearestInt(), juce::Justification::centredRight);
        }
        g.setColour (juce::Colours::black);
        g.fillRect (well);
        g.setColour (colours::well);
        g.fillRect (well.reduced (rim));
        g.setColour (juce::Colours::black);
        g.fillRect (track);
        const float pos = b.get();
        const float y = track.getBottom() - track.getHeight() * pos;
        if (in.delayOn.load())
        {
            g.setColour (colours::fader);
            g.fillRect (track.withTop (y).reduced (track.getWidth() * 0.25f, 0.0f));
        }
        const auto tag = well.reduced (rim, 0.0f).withHeight (tagH).withCentre ({ well.getCentreX(), juce::jmin (y, well.getBottom() - rim - tagH * 0.5f) });
        g.setColour (in.delayOn.load() ? Colours::text : Colours::textDim);
        g.fillRect (tag);
        g.setColour (juce::Colours::black);
        g.setFont (homeFont (TypeScale::body, tagH * 0.75f));
        g.drawFittedText (b.text(), tag.toNearestInt(), juce::Justification::centred, 1, 0.7f);
    }

    void mouseDown (const juce::MouseEvent&) override { start = delayBinding (ctx, in).get(); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        delayBinding (ctx, in).set (juce::jlimit (0.0f, 1.0f, start - (float) e.getDistanceFromDragStartY() / juce::jmax (60.0f, (float) getHeight()) * (e.mods.isShiftDown() ? 0.2f : 1.0f)));
        repaint();
    }
    void mouseDoubleClick (const juce::MouseEvent&) override { delayBinding (ctx, in).reset(); }

private:
    void timerCallback() override
    {
        const auto now = std::make_tuple (in.delay.load(), in.delayMode.load(), in.delayOn.load());
        if (now != shown)
        {
            shown = now;
            repaint();
        }
    }

    UiContext& ctx;
    InputParams& in;
    float start = 0.0f;
    std::tuple<float, int, bool> shown { -1.0f, -1, false };
};

/** The DELAY panel: the slider, the unit keys showing the delay in every unit
    (choosing one converts the value so the delay stays the same), and DELAY. */
class DelayPanel final : public juce::Component, private juce::Timer
{
public:
    DelayPanel (UiContext& c, InputParams& p) : ctx (c), in (p), slider (c, p)
    {
        addAndMakeVisible (slider);
        static const char* names[] = { "METERS", "FEET", "MS", "SAMPLES" };
        for (int m = 0; m < 4; ++m)
        {
            // [I7] the chosen unit lit in the choice blue, the others light grey.
            auto* k = units.add (new IconKey (names[m], std::nullopt, colours::choice));
            k->nameOverValue = true;
            k->offInk = colours::caption;
            k->isOn = [this, m] { return in.delayMode.load() == m; };
            k->onClick = [this, m]
            {
                const float sr = sampleRateOf (ctx);
                const int from = juce::jlimit (0, 3, (int) in.delayMode.load());
                const float ms = delayToMs (in.delay.load(), from, sr);
                in.delayMode = m;
                in.delay = msToDelay (ms, m, sr);
                if (onUnitChanged)
                    onUnitChanged();
            };
            addAndMakeVisible (k);
        }
        delayKey.setTooltip ("Delay on");
        delayKey.captionBelow = true;
        delayKey.keySide = 48.0f;
        bindKey (delayKey, in.delayOn);
        addAndMakeVisible (delayKey);
        startTimerHz (5);
        timerCallback();
    }

    std::function<void()> onUnitChanged;
    juce::Component* getSlider() noexcept { return &slider; }

    void resized() override
    {
        // WING 3.1 (in the 280 x 408 body): the slider (its well at x 52-128,
        // y 24-384, the scale left of it), the four unit keys 96 x 56 stacked
        // at (160, 24), the 48 px DELAY key at (184, 308), its caption under it.
        const float kx = (float) getWidth() / 280.0f, ky = (float) getHeight() / 408.0f;
        auto at = [kx, ky] (float x, float y, float w, float h)
        {
            const int x0 = juce::roundToInt (x * kx), y0 = juce::roundToInt (y * ky);
            return juce::Rectangle<int> (x0, y0, juce::roundToInt ((x + w) * kx) - x0, juce::roundToInt ((y + h) * ky) - y0);
        };
        slider.setBounds (at (12.0f, 24.0f, 116.0f, 360.0f));
        for (int i = 0; i < units.size(); ++i)
            units[i]->setBounds (at (160.0f, 24.0f + 56.0f * (float) i, 96.0f, 56.0f));
        delayKey.setBounds (at (172.0f, 308.0f, 72.0f, 88.0f));
    }

private:
    void timerCallback() override
    {
        static const char* names[] = { "METERS", "FEET", "MS", "SAMPLES" };
        const float sr = sampleRateOf (ctx);
        const float ms = delayToMs (in.delay.load(), juce::jlimit (0, 3, (int) in.delayMode.load()), sr);
        for (int m = 0; m < 4; ++m)
        {
            const auto text = juce::String (names[m]) + "\n" + delayText (msToDelay (ms, m, sr), m);
            if (units[m]->getButtonText() != text)
                units[m]->setButtonText (text);
        }
    }

    UiContext& ctx;
    InputParams& in;
    DelaySlider slider;
    juce::OwnedArray<IconKey> units;
    IconKey delayKey { "DELAY", Icon::DeltaT, colours::accent };
};

/** TRIM & BALANCE: an XY pad, trim (+-18 dB) up and down, balance (+-9 dB)
    across, with the balance handle on the bottom axis. */
class TrimBalancePad final : public juce::Component, private juce::Timer
{
public:
    TrimBalancePad (std::atomic<float>& t, std::atomic<float>& b, std::function<bool()> stereo) : trim (t), bal (b), hasBalance (std::move (stereo))
    {
        startTimerHz (12);
    }

    /** The trim field (+-18 dB, the balance across it) in the black box
        [I7]: 276 x 552 WING px, the field 31-246 across and 30-462 down. */
    juce::Rectangle<float> pad() const
    {
        const auto r = getLocalBounds().toFloat();
        return { r.getX() + r.getWidth() * 0.112f, r.getY() + r.getHeight() * 0.054f, r.getWidth() * 0.78f, r.getHeight() * 0.78f };
    }

    /** The balance row under the field: ticks, the handle, -9 dB / 0 dB / +9 dB. */
    juce::Rectangle<float> balanceRow() const
    {
        const auto r = getLocalBounds().toFloat();
        const auto p = pad();
        return { p.getX(), p.getBottom(), p.getWidth(), r.getBottom() - p.getBottom() };
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colours::black);
        g.fillRect (r);
        const auto p = pad();
        const float k = r.getHeight() / 552.0f; // WING px of the box
        const auto small = wingFont (TypeScale::meterScale * 1.1f);
        const float lh = small.getHeight();

        // The dotted grid.
        g.setColour (juce::Colour (0xff404040));
        const float dot = juce::jmax (1.0f, 2.0f * k);
        for (float yy = p.getY(); yy <= p.getBottom() + 0.5f; yy += p.getHeight() / 24.0f)
            for (float xx = p.getX(); xx <= p.getRight() + 0.5f; xx += p.getWidth() / 9.0f)
                g.fillRect (xx - dot * 0.5f, yy - dot * 0.5f, dot, dot);

        g.setColour (Colours::textDim);
        g.setFont (small);
        g.drawText ("TRIM", juce::Rectangle<float> (p.getX(), r.getY(), p.getWidth() * 0.5f, p.getY() - r.getY()).toNearestInt(), juce::Justification::centredLeft);
        g.drawText ("BAL", juce::Rectangle<float> (p.getCentreX(), r.getY(), p.getWidth() * 0.5f, p.getY() - r.getY()).toNearestInt(), juce::Justification::centredRight);
        for (int db = 18; db >= -18; db -= 6)
        {
            const float y = p.getCentreY() - (float) db / 18.0f * p.getHeight() * 0.5f;
            g.setColour (Colours::caption);
            g.drawText (db == 0 ? juce::String ("0 dB") : juce::String (db), juce::Rectangle<float> (r.getX(), y - lh * 0.5f, p.getX() - r.getX() - 3.0f * k, lh).toNearestInt(),
                        juce::Justification::centredRight);
            g.setColour (juce::Colour (0xff585858));
            g.fillRect (r.getRight() - 20.0f * k, y - k, 6.0f * k, juce::jmax (1.0f, 2.0f * k));
        }

        // The balance ticks along the bottom: long at -9, 0 and +9 dB.
        const auto row = balanceRow();
        const float tickY = row.getY() + row.getHeight() * 0.42f;
        for (int i = 0; i <= 12; ++i)
        {
            if (i == 6)
                continue;
            const bool major = i == 0 || i == 12;
            const float x = p.getX() + p.getWidth() * (float) i / 12.0f;
            const float th = (major ? 22.0f : 12.0f) * k;
            g.setColour (juce::Colour (0xff686868));
            g.fillRoundedRectangle (juce::Rectangle<float> (juce::jmax (1.5f, 4.0f * k), th).withCentre ({ x, tickY }), 1.5f * k);
        }
        g.setColour (Colours::caption);
        const float by = row.getBottom() - lh * 1.3f;
        g.drawText ("-9 dB", juce::Rectangle<float> (p.getX() - lh * 2.0f, by, lh * 4.0f, lh).toNearestInt(), juce::Justification::centred);
        g.drawText ("0 dB", juce::Rectangle<float> (p.getCentreX() - lh * 2.0f, by, lh * 4.0f, lh).toNearestInt(), juce::Justification::centred);
        g.drawText ("+9 dB", juce::Rectangle<float> (p.getRight() - lh * 2.0f, by, lh * 4.0f, lh).toNearestInt(), juce::Justification::centred);

        // The blue balance line, the white trim line (grey past the field).
        const float x = p.getCentreX() + bal.load() / 9.0f * p.getWidth() * 0.5f;
        const float y = p.getCentreY() - trim.load() / 18.0f * p.getHeight() * 0.5f;
        const float lw = juce::jmax (1.5f, 2.0f * k);
        g.setColour (colours::fader);
        g.fillRect (x - lw * 0.5f, p.getY() - 10.0f * k, lw, tickY - p.getY());
        // WING 3.1: the trim line light grey across the field, dark grey past it.
        g.setColour (Colours::screenLine);
        g.fillRect (p.getRight(), y - lw * 0.5f, r.getRight() - p.getRight(), lw);
        g.setColour (Colours::caption);
        g.fillRect (p.getX(), y - lw * 0.5f, p.getWidth(), lw);

        // The handles: a light grey disc with three lines, a blue one with three bars.
        const float hd = 38.0f * k;
        auto handle = juce::Rectangle<float> (hd, hd).withCentre ({ x, y });
        g.setColour (Colours::caption);
        g.fillEllipse (handle);
        g.setColour (juce::Colours::black);
        for (int i = -1; i <= 1; ++i)
            g.fillRect (juce::Rectangle<float> (hd * 0.5f, juce::jmax (1.0f, 2.0f * k)).withCentre ({ x, y + (float) i * hd * 0.16f }));
        auto balHandle = juce::Rectangle<float> (hd, hd).withCentre ({ x, tickY });
        // (Blue for a mono channel too, as the console shows it.)
        g.setColour (colours::fader);
        g.fillEllipse (balHandle);
        g.setColour (juce::Colours::black);
        for (int i = -1; i <= 1; ++i)
            g.fillRect (juce::Rectangle<float> (juce::jmax (1.0f, 2.0f * k), hd * 0.44f).withCentre ({ x + (float) i * hd * 0.16f, tickY }));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        balanceOnly = hasBalance() && e.position.y > balanceRow().getY();
        startTrim = trim.load();
        startBal = bal.load();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto p = pad();
        const float fine = e.mods.isShiftDown() ? 0.2f : 1.0f;
        if (hasBalance())
            bal = juce::jlimit (-9.0f, 9.0f, startBal + fine * (float) e.getDistanceFromDragStartX() / (p.getWidth() * 0.5f) * 9.0f);
        if (! balanceOnly)
            trim = juce::jlimit (-18.0f, 18.0f, startTrim - fine * (float) e.getDistanceFromDragStartY() / (p.getHeight() * 0.5f) * 18.0f);
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        trim = 0.0f;
        bal = 0.0f;
    }

private:
    void timerCallback() override
    {
        const auto now = std::make_tuple (trim.load(), bal.load(), hasBalance());
        if (now != shown)
        {
            shown = now;
            repaint();
        }
    }

    std::atomic<float>& trim;
    std::atomic<float>& bal;
    std::function<bool()> hasBalance; // a stereo strip (SELECT MAIN INPUT can change it while shown)
    bool balanceOnly = false;
    float startTrim = 0.0f, startBal = 0.0f;
    std::tuple<float, float, bool> shown { 1.0e9f, 0.0f, false };
};

/** The ALT input list: NONE, the hardware inputs and the recorder's playback channels. */
void showAltSourceMenu (UiContext& ctx, int current, juce::Component* target, std::function<void (int)> chosen)
{
    if (target == nullptr)
        return;
    juce::StringArray items { "NONE" };
    std::vector<int> sources { -1 };
    const auto& names = ctx.engine.getHardwareInputNames();
    const int numInputs = names.isEmpty() ? 64 : names.size();
    for (int i = 0; i < numInputs; ++i)
    {
        items.add (inputPortName (i) + (i < names.size() && names[i].isNotEmpty() ? "  " + names[i].toUpperCase() : juce::String()));
        sources.push_back (i);
    }
    for (int ch = 0; ch < Recorder::kNumPlaybackChannels; ++ch)
    {
        items.add (Recorder::playbackSourceName (ch).toUpperCase());
        sources.push_back (Recorder::playbackSource (ch));
    }
    const auto it = std::find (sources.begin(), sources.end(), current);
    popups::ListOptions options;
    options.columns = 4;
    options.caption = "ALT SOURCE";
    popups::chooseFrom (*target, items, it != sources.end() ? (int) (it - sources.begin()) : 0, [chosen, sources] (int i)
    {
        if (juce::isPositiveAndBelow (i, (int) sources.size()))
            chosen (sources[(size_t) i]);
    }, options);
}

/** Applies `set` to the source of input `port` and, when it is a stereo or
    mid/side pair, to its partner (as ROUTING > SOURCES does). */
void forInputSource (MixerEngine& engine, int port, const std::function<void (ld::SourceParams&)>& set)
{
    const int index = port >= 0 ? MixerEngine::sourceIndex (port) : -1;
    auto* src = engine.getSource (index);
    if (src == nullptr)
        return;
    const bool paired = src->mode.load() != ld::kSourceMono;
    for (int i : { paired ? (index & ~1) : index, paired ? (index | 1) : -1 })
        if (auto* s = engine.getSource (i))
            set (*s);
}

/** The settings of input `port` (a pair runs on its first source's). */
ld::SourceParams* inputSourceParams (MixerEngine& engine, int port)
{
    const int index = port >= 0 ? MixerEngine::sourceIndex (port) : -1;
    auto* src = engine.getSource (index);
    if (src != nullptr && src->mode.load() != ld::kSourceMono)
        return engine.getSource (index & ~1);
    return src;
}

/** The GAIN range of input `port`'s source group (none without a source). */
MixerEngine::GainRange inputGainRange (int port)
{
    return port >= 0 ? MixerEngine::sourceGainRange (MixerEngine::sourceIndex (port)) : MixerEngine::GainRange();
}
} // namespace

KnobBinding inputDelayBinding (UiContext& ctx, InputParams& in) { return delayBinding (ctx, in); }

KnobBinding sourceGainBinding (UiContext& ctx, Strip& strip)
{
    // The source group's range and steps, as the WING's (LOCAL IN -2.5 .. +45 dB
    // in 2.5 dB steps, AES50 -3 .. +45.5 dB in 0.5 dB steps); a source the
    // WING gives no gain reads "-".
    auto& engine = ctx.engine;
    KnobBinding b;
    b.label = "GAIN";
    b.get = [&engine, &strip]
    {
        const auto* src = inputSourceParams (engine, strip.getInputLeft());
        return src != nullptr ? inputGainRange (strip.getInputLeft()).toProportion (src->gainDb.load()) : 0.0f;
    };
    b.set = [&engine, &strip] (float p)
    {
        const auto range = inputGainRange (strip.getInputLeft());
        if (! range.hasGain())
            return;
        const float gain = range.fromProportion (p);
        forInputSource (engine, strip.getInputLeft(), [gain] (ld::SourceParams& s) { s.gainDb = gain; });
    };
    b.text = [&engine, &strip]
    {
        const auto* src = inputSourceParams (engine, strip.getInputLeft());
        const auto range = inputGainRange (strip.getInputLeft());
        return src != nullptr && range.hasGain() ? juce::String (range.clamp (src->gainDb.load()), 1) + " dB" : juce::String ("-");
    };
    b.reset = [&engine, &strip] { forInputSource (engine, strip.getInputLeft(), [] (ld::SourceParams& s) { s.gainDb = 0.0f; }); };
    // The steps of the source patched when the binding is made (set() puts
    // the gain on the current source's steps in any case).
    const auto range = inputGainRange (strip.getInputLeft());
    b.steps = range.hasGain() ? (float) range.steps() : 0.0f;
    return b;
}

/** A routing-style source tile [3.1, D16]: the source's name strip in its
    colour over a #303030 body with the icon, the signal ring and the 48V /
    MUTE chips at the right and the port in blue (olive for stereo). */
void paintInputSourceTile (juce::Graphics& g, juce::Rectangle<float> r, MixerEngine& engine, int left, int right, bool stereo, bool signal)
{
    const bool usbPlayer = left >= 0 && UsbRecorder::isPlayerSource (left);
    const bool playback = left >= 0 && (Recorder::isPlaybackSource (left) || usbPlayer);
    const int index = left >= 0 ? MixerEngine::sourceIndex (left) : -1;
    const auto* src = inputSourceParams (engine, left);
    const auto port = inputSourceText (left, right, stereo);
    // [I7] no source: a grey strip over a black tile with a circled plus.
    if (left < 0)
    {
        g.setColour (colours::chipOff);
        g.fillRect (r.removeFromTop (juce::jmax (12.0f, r.getHeight() * 0.18f)));
        g.setColour (juce::Colours::black);
        g.fillRect (r);
        const float d = juce::jmin (r.getWidth(), r.getHeight()) * 0.42f;
        const auto plus = juce::Rectangle<float> (d, d).withCentre (r.getCentre());
        g.setColour (juce::Colour (0xff303430));
        g.drawEllipse (plus, juce::jmax (1.5f, d * 0.06f));
        const auto c = plus.getCentre();
        g.drawLine (c.x - d * 0.25f, c.y, c.x + d * 0.25f, c.y, juce::jmax (1.5f, d * 0.06f));
        g.drawLine (c.x, c.y - d * 0.25f, c.x, c.y + d * 0.25f, juce::jmax (1.5f, d * 0.06f));
        return;
    }
    // The strip carries the source's name once it has one [I1].
    const juce::String name = index >= 0 ? engine.getSourceName (index) : juce::String();
    const int colourIndex = src != nullptr ? src->colour.load() : ld::kDefaultColour;
    const auto colour = playback && colourIndex == ld::kDefaultColour ? juce::Colour (0xff2f5a32) : wingColour (colourIndex);
    const float screenH = getWingScreenHeight();
    // Small tiles (small windows) keep the icon and the port only.
    const bool roomy = r.getHeight() >= 44.0f && r.getWidth() >= 64.0f;

    auto bar = r.removeFromTop (juce::jmax (12.0f, r.getHeight() * 0.2f));
    g.setColour (colour);
    g.fillRect (bar);
    g.setColour (colour.getPerceivedBrightness() > 0.45f ? juce::Colours::black : juce::Colours::white);
    g.setFont (homeFont (TypeScale::body * 1.05f, bar.getHeight() * 0.7f, WingWeight::SemiBold));
    g.drawFittedText (name, bar.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.7f);

    g.setColour (colours::keyOff);
    g.fillRect (r);
    auto label = r.removeFromBottom (juce::jmax (11.0f, r.getHeight() * 0.26f));
    g.setColour (colours::chipOff);
    g.fillRect (label.getX() + 3.0f, label.getY(), label.getWidth() - 6.0f, juce::jmax (1.0f, label.getHeight() * 0.06f));
    g.setColour (stereo ? colours::stereo : colours::mono);
    g.setFont (homeFont (TypeScale::body * 1.25f, label.getHeight() * 0.8f));
    g.drawFittedText (port, label.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.7f);

    auto body = r.reduced (3.0f, 2.0f);
    auto side = roomy ? body.removeFromRight (body.getWidth() * 0.34f) : juce::Rectangle<float>();
    const float iconSide = juce::jmin (body.getWidth(), body.getHeight()) * 0.86f;
    auto iconArea = juce::Rectangle<float> (iconSide, iconSide).withCentre (body.getCentre());
    const int icon = src != nullptr ? src->icon.load() : 0;
    const auto ink = colours::caption;
    if (icon <= 0 || ! drawWingIcon (g, icon, iconArea, ink))
        drawIcon (g, sourceIcon (left), iconArea, ink);

    if (! roomy)
        return;
    // The signal ring over the 48V and MUTE chips (grey when off, red when on).
    const float chipH = juce::jlimit (6.0f, screenH * 0.024f, side.getHeight() * 0.27f);
    const float ringD = juce::jmin (chipH * 1.25f, side.getWidth() * 0.5f);
    auto ring = juce::Rectangle<float> (ringD, ringD).withPosition (side.getRight() - ringD - 1.0f, side.getY());
    g.setColour (signal ? Colours::meterGreen : colours::caption);
    if (signal)
        g.fillEllipse (ring);
    else
        g.drawEllipse (ring, 1.2f);
    auto chips = side.withTrimmedTop (ringD + 2.0f);
    chips = chips.withHeight (chipH * 2.0f + 2.0f).withTrimmedLeft (juce::jmax (0.0f, chips.getWidth() - chipH * 2.6f));
    const bool phantom = src != nullptr && src->phantom.load();
    const bool mute = src != nullptr && src->mute.load();
    drawChip (g, chips.removeFromTop (chipH), "48V", phantom ? colours::mute : colours::chipOff, phantom ? colours::ink : juce::Colours::black);
    chips.removeFromTop (2.0f);
    drawChip (g, chips.removeFromTop (chipH), "MUTE", mute ? colours::mute : colours::chipOff, mute ? colours::ink : juce::Colours::black);
}

namespace
{

/** One source column of CHANNEL INPUT [3.1, D16]: a #202020 panel with the
    source tile, framed in grey while the channel listens to it. The MAIN
    column adds the source's +48V key and its preamp GAIN slider (the WING
    only controls the MAIN input's preamp here). Click the MAIN tile to patch
    on ROUTING, the ALT tile to choose the ALT input. */
class InputSourcePanel final : public juce::Component, private juce::Timer
{
public:
    InputSourcePanel (UiContext& c, Strip& s, bool altSource) : ctx (c), strip (s), alt (altSource)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        startTimerHz (8);
    }

    void resized() override
    {
        // [I7, D16] WING pixels in a 192 px wide box (the MAIN column 512 tall,
        // the ALT box 192): the 112 px tile 40 px in, the 48 px +48V key at
        // y 192 with its caption under it, the 64 px GAIN slider from y 276.
        const float kx = (float) getWidth() / 192.0f;
        const float ky = (float) getHeight() / (alt ? 192.0f : 512.0f);
        const float k = juce::jmin (kx, ky * (alt ? 1.0f : 1.6f));
        auto px = [] (float v) { return juce::roundToInt (v); };
        const int side = juce::jmin (px (112.0f * k), getHeight() - 8);
        tile = juce::Rectangle<int> (side, side).withCentre ({ getWidth() / 2, alt ? getHeight() / 2 : px (40.0f * ky) + side / 2 });
        if (alt)
            return;
        auto r = getLocalBounds().withTop (tile.getBottom() + px (40.0f * ky));
        const int keySide = juce::jmin (px (48.0f * k), r.getHeight() / 5);
        phantomKey = r.removeFromTop (keySide).withSizeKeepingCentre (keySide, keySide);
        phantomCaption = r.removeFromTop (juce::jmax (12, px (28.0f * ky)));
        r.removeFromTop (px (8.0f * ky));
        r.removeFromBottom (px (28.0f * ky));
        gainSlider = r.withSizeKeepingCentre (juce::jmax (20, px (64.0f * kx)), r.getHeight());
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (colours::panel);
        g.fillRect (r);
        const int left = alt ? strip.getAltInputLeft() : strip.getInputLeft();
        const int right = alt ? strip.getAltInputRight() : strip.getInputRight();
        const bool st = strip.params.stereo.load();
        const bool active = alt == strip.isUsingAltInput();
        const bool signal = active && ctx.meters.get (&strip, true, 0) > juce::Decibels::decibelsToGain (-50.0f);
        paintInputSourceTile (g, tile.toFloat(), ctx.engine, left, right, st, signal);
        if (active)
        {
            // WING 3.1: a 4 px grey frame round the column in use.
            g.setColour (Colours::textDim);
            g.drawRect (r, juce::jmax (2.0f, wingPx (4.0f)));
        }
        if (alt)
            return;

        const auto* src = inputSourceParams (ctx.engine, left);
        const bool phantom = src != nullptr && src->phantom.load();
        const float alpha = src != nullptr ? 1.0f : 0.4f;
        auto k = phantomKey.toFloat();
        g.setColour (phantom ? colours::mute : colours::keyOff);
        g.fillRect (k);
        paintHoldProgress (g, k, phantomHold.progress(), phantom ? colours::keyOff : colours::mute);
        drawIcon (g, Icon::Power, k.reduced (k.getWidth() * 0.22f), (phantom ? colours::ink : colours::caption).withAlpha (alpha));
        g.setColour (colours::dim.withAlpha (alpha));
        g.setFont (homeFont (TypeScale::caption, (float) phantomCaption.getHeight()));
        g.drawText ("+48V", phantomCaption, juce::Justification::centred);

        // GAIN (the source group's range: 0 dB sits low, as on the WING),
        // WING 3.1: a 64 px well in a 2 px black rim, the 12 px black slot
        // from 10 px under its top, the white value tag across the well's
        // inside (25 tall).
        auto f = gainSlider.toFloat();
        const float rim = juce::jmax (1.0f, wingPx (2.0f));
        g.setColour (juce::Colours::black);
        g.fillRect (f);
        g.setColour (colours::well);
        g.fillRect (f.reduced (rim));
        g.setColour (juce::Colours::black);
        g.fillRect (f.withSizeKeepingCentre (wingPx (12.0f), f.getHeight()).withTrimmedTop (wingPx (10.0f)).withTrimmedBottom (rim));
        const auto range = inputGainRange (strip.getInputLeft());
        const float gain = src != nullptr ? range.clamp (src->gainDb.load()) : 0.0f;
        const float tagH = tagHeight();
        const float pos = range.toProportion (gain);
        const float y = f.getBottom() - tagH * 0.5f - 2.0f - (f.getHeight() - tagH - 4.0f) * pos;
        auto tag = juce::Rectangle<float> (f.getWidth() - 2.0f * rim, tagH).withCentre ({ f.getCentreX(), y });
        g.setColour (juce::Colours::white.withAlpha (alpha));
        g.fillRect (tag);
        g.setColour (juce::Colours::black);
        g.setFont (homeFont (TypeScale::body, tagH * 0.8f));
        g.drawFittedText (juce::String (gain, 1) + " dB", tag.toNearestInt(), juce::Justification::centred, 1, 0.6f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        draggingGain = ! alt && gainSlider.expanded (10, 4).contains (e.getPosition());
        if (draggingGain)
            setGainFromY (e.y);
        // +48V switches when held (kPhantomHoldSeconds), as on the console.
        if (! alt && phantomKey.contains (e.getPosition()) && inputSourceParams (ctx.engine, strip.getInputLeft()) != nullptr)
            phantomHold.press();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (draggingGain)
            setGainFromY (e.y);
        if (! phantomKey.contains (e.getPosition()))
            phantomHold.release();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        phantomHold.release();
        if (draggingGain || ! getLocalBounds().contains (e.getPosition()))
            return;
        if (! alt && phantomKey.contains (e.getPosition()))
            return;
        if (! tile.contains (e.getPosition()))
            return;
        if (! alt)
        {
            if (onSelect)
                onSelect();
            return;
        }
        juce::Component::SafePointer<InputSourcePanel> safe (this);
        showAltSourceMenu (ctx, strip.getAltInputLeft(), this, [safe] (int source)
        {
            if (safe != nullptr)
            {
                safe->strip.setAltInput (source);
                safe->repaint();
            }
        });
    }

    /** The MAIN tile was touched (SELECT MAIN INPUT). */
    std::function<void()> onSelect;

private:
    static float tagHeight() { return juce::jmax (12.0f, wingPx (25.0f)); }

    void setGainFromY (int y)
    {
        auto f = gainSlider.toFloat();
        const float tagH = tagHeight();
        const float span = juce::jmax (1.0f, f.getHeight() - tagH - 4.0f);
        const float pos = juce::jlimit (0.0f, 1.0f, (f.getBottom() - tagH * 0.5f - 2.0f - (float) y) / span);
        const auto range = inputGainRange (strip.getInputLeft());
        if (! range.hasGain())
            return;
        const float gain = range.fromProportion (pos); // on the group's steps
        forInputSource (ctx.engine, strip.getInputLeft(), [gain] (ld::SourceParams& s) { s.gainDb = gain; });
        repaint();
    }

    void switchPhantom()
    {
        const auto* src = inputSourceParams (ctx.engine, strip.getInputLeft());
        const bool on = src != nullptr && src->phantom.load();
        forInputSource (ctx.engine, strip.getInputLeft(), [on] (ld::SourceParams& s) { s.phantom = ! on; });
        repaint();
    }

    void timerCallback() override
    {
        const int left = alt ? strip.getAltInputLeft() : strip.getInputLeft();
        const auto* src = inputSourceParams (ctx.engine, left);
        const int index = left >= 0 ? MixerEngine::sourceIndex (left) : -1;
        const bool signal = ctx.meters.get (&strip, true, 0) > juce::Decibels::decibelsToGain (-50.0f);
        auto now = juce::String (left) + "|" + juce::String (alt ? strip.getAltInputRight() : strip.getInputRight()) + (strip.isUsingAltInput() ? "A" : "M")
                   + (strip.params.stereo.load() ? "S" : "") + (signal ? "s" : "") + (index >= 0 ? ctx.engine.getSourceName (index) : juce::String());
        if (src != nullptr)
            now << "|" << src->colour.load() << "|" << src->icon.load() << (src->phantom.load() ? "P" : "") << (src->mute.load() ? "M" : "")
                << "|" << src->gainDb.load();
        if (now != shown)
        {
            shown = now;
            repaint();
        }
    }

    UiContext& ctx;
    Strip& strip;
    const bool alt;
    bool draggingGain = false;
    juce::Rectangle<int> tile, phantomKey, phantomCaption, gainSlider;
    juce::String shown;
    HoldKey phantomHold { [this] { switchPhantom(); }, [this] { repaint (phantomKey); } };
};

/** SELECT MAIN INPUT [I7, firmware 3.1], opened from the MAIN source tile: the
    SOURCE GROUP list, NONE, the source's M / ST / M/S, CLOSE, then the
    group's sources (eight to a row, the MAIN input framed gold, ports the
    audio interface doesn't have "n/a"). A touch patches the source. */
class SourceSelectPanel final : public juce::Component, private juce::Timer
{
public:
    SourceSelectPanel (UiContext& c, Strip& s) : ctx (c), strip (s)
    {
        group = groupOf (strip.getInputLeft());
        groupField = std::make_unique<DropdownField> ("SOURCE GROUP", [this] { return groupName (group); }, [this] (DropdownField& f)
        {
            juce::StringArray names;
            for (int i = 0; i < numGroups(); ++i)
                names.add (groupName (i));
            popups::chooseFrom (f, names, group, [safe = juce::Component::SafePointer<SourceSelectPanel> (this)] (int i)
            {
                if (safe != nullptr)
                {
                    safe->group = i;
                    safe->repaint();
                }
            });
        });
        groupField->icon = Icon::Xlr;
        groupField->arrowKey = true;
        groupField->consoleMetrics = true;
        addAndMakeVisible (*groupField);
        none.offInk = colours::caption;
        none.captionBelow = true;
        none.offColour = colours::panel;
        none.onClick = [this]
        {
            strip.params.inputLeft = -1;
            strip.params.inputRight = -1;
        };
        addAndMakeVisible (none);
        // The selected source's pairing (ROUTING > SOURCES MODE).
        mode = std::make_unique<Segmented> (juce::StringArray { "M", "ST", "M/S" }, [this]
        {
            const auto* src = ctx.engine.getSource (sourceIndexOf (strip.getInputLeft()));
            return src != nullptr ? (int) src->mode.load() : -1;
        }, [this] (int m)
        {
            if (const int index = sourceIndexOf (strip.getInputLeft()); index >= 0)
                ctx.engine.setSourceMode (index, m);
        });
        mode->colourAt = [] (int) { return colours::fader; };
        addAndMakeVisible (*mode);
        close.offInk = colours::caption;
        close.offColour = colours::panel;
        close.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (close);
        startTimerHz (5);
    }

    std::function<void()> onClose;

    void resized() override
    {
        // WING pixels (712 x 324 from x 560): the header 80 tall with the
        // group field (264), NONE, M | ST | M/S (182) and CLOSE; then the
        // tiles, 80 x 72 on an 88 x 76 pitch.
        auto b = getLocalBounds();
        const float u = (float) getWidth() / 712.0f;
        header = b.removeFromTop (juce::roundToInt (80.0f * u));
        b.removeFromTop (juce::roundToInt (8.0f * u));
        gridArea = b.withHeight (juce::jmin (b.getHeight(), juce::roundToInt (236.0f * u)));
        auto h = header.reduced (juce::roundToInt (8.0f * u));
        groupField->setBounds (h.removeFromLeft (juce::roundToInt (264.0f * u)));
        h.removeFromLeft (juce::roundToInt (16.0f * u));
        none.setBounds (h.removeFromLeft (juce::roundToInt (48.0f * u)));
        h.removeFromLeft (juce::roundToInt (32.0f * u));
        mode->setBounds (h.removeFromLeft (juce::roundToInt (182.0f * u)).reduced (0, juce::roundToInt (4.0f * u)));
        close.setBounds (h.removeFromRight (juce::roundToInt (120.0f * u)).reduced (0, juce::roundToInt (4.0f * u)));
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (colours::panel);
        g.fillRect (header);
        g.fillRect (gridArea);
        auto inner = gridArea.toFloat().reduced (juce::jmax (2.0f, (float) getWidth() / 356.0f));
        g.setColour (juce::Colours::black);
        g.fillRect (inner);
        tiles.clear();
        const float pw = inner.getWidth() / 8.0f, ph = inner.getHeight() / 3.0f;
        const int numPorts = ctx.engine.getHardwareInputNames().size();
        for (int i = 0; i < 24; ++i)
        {
            const int port = sourceAt (group, i);
            if (port == kNoSource)
                continue;
            auto cell = juce::Rectangle<float> (inner.getX() + pw * (float) (i % 8), inner.getY() + ph * (float) (i / 8), pw, ph).reduced (pw * 0.04f, ph * 0.05f);
            tiles.push_back ({ cell.toNearestInt(), port });
            const bool missing = port >= 0 && port < ld::kPlaybackSourceBase && port >= numPorts;
            if (missing)
            {
                // A port the audio interface doesn't have: "n/a" on a dark tile.
                g.setColour (juce::Colour (0xff10103a));
                g.fillRect (cell.removeFromTop (cell.getHeight() * 0.2f));
                g.setColour (colours::panel);
                g.fillRect (cell);
                g.setColour (colours::dim);
                g.setFont (homeFont (TypeScale::body, cell.getHeight() * 0.3f));
                g.drawText ("n/a", cell.withTrimmedBottom (cell.getHeight() * 0.35f), juce::Justification::centred);
                g.setColour (colours::mono.withAlpha (0.6f));
                g.setFont (homeFont (TypeScale::caption, cell.getHeight() * 0.24f));
                g.drawText (inputPortName (port), cell.withTrimmedTop (cell.getHeight() * 0.65f), juce::Justification::centred);
                continue;
            }
            paintInputSourceTile (g, cell, ctx.engine, port, port + 1, false, false);
            if (port == strip.getInputLeft())
            {
                g.setColour (colours::accent);
                g.drawRect (cell.expanded (2.0f), 2.5f);
            }
        }
        g.setColour (colours::caption);
        g.setFont (homeFont (TypeScale::body, close.getHeight() * 0.4f));
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        for (auto& [area, port] : tiles)
            if (area.contains (e.getPosition()))
            {
                patch (port);
                repaint();
                return;
            }
    }

private:
    static constexpr int kNoSource = -2;
    static constexpr int kGroupSize = 24;

    static int sourceIndexOf (int port) { return port >= 0 ? MixerEngine::sourceIndex (port) : -1; }

    int numHardwareGroups() const
    {
        const int ports = ctx.engine.getHardwareInputNames().size();
        return juce::jmax (1, (ports + kGroupSize - 1) / kGroupSize);
    }
    int numPlaybackGroups() const { return (Recorder::kNumPlaybackChannels + kGroupSize - 1) / kGroupSize; }
    /** After the ports and playback channels: USB PLAYER, OSCILLATOR, then the
        internal sources (manual 2.6) in pages of 24: BUS 1-12, BUS 13-16, MAIN,
        MATRIX and the USER SIGNAL group's USR 1-24, 25-48, 49-56. */
    struct InternalGroup
    {
        const char* name;
        int first, count; // input indices first .. first + count - 1
    };
    static const std::vector<InternalGroup>& internalGroups()
    {
        static const std::vector<InternalGroup> groups {
            { "USB PLAYER", UsbRecorder::playerSource (0), UsbRecorder::kNumChannels },
            { "OSCILLATOR", MixerEngine::oscSource (0), ld::kNumOscillators },
            { "BUS 1-12", ld::kStripSourceBase, 24 },
            { "BUS 13-16", ld::kStripSourceBase + 24, 2 * kWingBuses - 24 },
            { "MAIN", ld::kStripSourceBase + 2 * kWingBuses, 2 * kNumMains },
            { "MATRIX", ld::kStripSourceBase + 2 * (kWingBuses + kNumMains), 2 * kNumMatrices },
            { "USR 1-24", MixerEngine::usrSource (0), 24 },
            { "USR 25-48", MixerEngine::usrSource (24), 24 },
            { "USR 49-56", MixerEngine::usrSource (48), ld::kNumUsrSources - 48 },
        };
        return groups;
    }
    int numGroups() const { return numHardwareGroups() + numPlaybackGroups() + (int) internalGroups().size(); }

    juce::String groupName (int g) const
    {
        const int hw = numHardwareGroups();
        auto range = [] (const char* prefix, int first, int last) { return juce::String (prefix) + juce::String (first) + "-" + juce::String (last); };
        if (g < hw)
            return range ("AUDIO IN ", g * kGroupSize + 1, g * kGroupSize + kGroupSize);
        if (g < hw + numPlaybackGroups())
            return range ("PLAY ", (g - hw) * kGroupSize + 1, juce::jmin (Recorder::kNumPlaybackChannels, (g - hw + 1) * kGroupSize));
        const int k = g - hw - numPlaybackGroups();
        return juce::isPositiveAndBelow (k, (int) internalGroups().size()) ? internalGroups()[(size_t) k].name : "";
    }

    /** The source `i` of group `g`, or kNoSource. */
    int sourceAt (int g, int i) const
    {
        const int hw = numHardwareGroups();
        if (g < hw)
            return g * kGroupSize + i;
        if (g < hw + numPlaybackGroups())
        {
            const int channel = (g - hw) * kGroupSize + i;
            return channel < Recorder::kNumPlaybackChannels ? Recorder::playbackSource (channel) : kNoSource;
        }
        const int k = g - hw - numPlaybackGroups();
        if (! juce::isPositiveAndBelow (k, (int) internalGroups().size()))
            return kNoSource;
        const auto& internal = internalGroups()[(size_t) k];
        return i < internal.count ? internal.first + i : kNoSource;
    }

    int groupOf (int port) const
    {
        for (int g = 0; g < numGroups(); ++g)
            for (int i = 0; i < kGroupSize; ++i)
                if (port >= 0 && sourceAt (g, i) == port)
                    return g;
        return 0;
    }

    /** Patches the MAIN input as ROUTING does: a paired source in stereo. */
    void patch (int port)
    {
        auto& p = strip.params;
        if (MixerEngine::isStripSource (port))
        {
            // A bus, main or matrix: stereo, L and R (the WING's $BUS... are ST).
            const int first = port - (port - ld::kStripSourceBase) % 2;
            p.inputLeft = first;
            p.inputRight = first + 1;
            p.stereo = true;
        }
        else if (auto* src = ctx.engine.getSource (sourceIndexOf (port)); src != nullptr && src->mode.load() != ld::kSourceMono)
        {
            const int first = MixerEngine::sourcePort (sourceIndexOf (port) & ~1);
            p.inputLeft = first;
            p.inputRight = first + 1;
            p.stereo = true;
        }
        else
        {
            p.inputLeft = port;
            p.inputRight = -1;
        }
    }

    void timerCallback() override
    {
        const auto now = juce::String (strip.getInputLeft()) + "|" + juce::String (group);
        if (now != shown)
        {
            shown = now;
            repaint();
        }
    }

    UiContext& ctx;
    Strip& strip;
    int group = 0;
    std::unique_ptr<DropdownField> groupField;
    IconKey none { "NONE", Icon::ClearCircle };
    std::unique_ptr<Segmented> mode;
    IconKey close { "CLOSE", Icon::Check };
    juce::Rectangle<int> header, gridArea;
    std::vector<std::pair<juce::Rectangle<int>, int>> tiles;
    juce::String shown;
};

/** COPY CUSTOMIZATION under SELECT MAIN INPUT [I7]: the channel's tile and
    the source's, with << (the source's name, colour and icon to the channel)
    and >> (the channel's to the source) between them. */
class CopyCustomizationPanel final : public juce::Component, private juce::Timer
{
public:
    CopyCustomizationPanel (UiContext& c, Strip& s) : ctx (c), strip (s) { startTimerHz (4); }

    void resized() override
    {
        auto b = getLocalBounds();
        const float u = (float) getWidth() / 392.0f;
        auto row = b.withTrimmedTop (juce::roundToInt (16.0f * u));
        auto caption = row.removeFromTop (juce::roundToInt (32.0f * u));
        juce::ignoreUnused (caption);
        const int tile = juce::jmin (juce::roundToInt (112.0f * u), row.getHeight());
        channelTile = juce::Rectangle<int> (tile, tile).withPosition (b.getX() + juce::roundToInt (32.0f * u), row.getY());
        sourceTile = channelTile.withX (b.getRight() - juce::roundToInt (32.0f * u) - tile);
        toChannel = juce::Rectangle<int> (channelTile.getRight(), row.getY(), (sourceTile.getX() - channelTile.getRight()) / 2, tile);
        toSource = toChannel.withX (toChannel.getRight());
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (colours::panel);
        g.fillRect (getLocalBounds());
        g.setColour (colours::caption);
        g.setFont (homeFont (TypeScale::body * 1.1f, (float) channelTile.getHeight() * 0.18f));
        for (auto [tile, text] : { std::pair { channelTile, "CHANNEL" }, { sourceTile, "SOURCE" } })
            g.drawText (text, tile.withY (tile.getY() - tile.getHeight() * 30 / 100).withHeight (tile.getHeight() * 26 / 100), juce::Justification::centred);

        // The channel: its own colour over a grey body with the number.
        auto c = channelTile.toFloat();
        g.setColour (juce::Colours::black);
        g.fillRect (c);
        c = c.reduced (c.getWidth() * 0.07f, c.getHeight() * 0.18f);
        g.setColour (wingColour (strip.params.colour.load()));
        g.fillRect (c.removeFromTop (c.getHeight() * 0.28f));
        g.setColour (colours::keyOff);
        g.fillRect (c);
        g.setColour (juce::Colours::black);
        g.fillRect (c.withWidth (c.getHeight() * 0.5f).reduced (2.0f));
        g.setColour (colours::caption);
        g.setFont (homeFont (TypeScale::value, c.getHeight() * 0.8f));
        g.drawText (juce::String (strip.getNumber()), c.withWidth (c.getHeight() * 0.5f), juce::Justification::centred);

        g.setColour (juce::Colours::black);
        g.fillRect (sourceTile);
        paintInputSourceTile (g, sourceTile.toFloat().reduced (3.0f), ctx.engine, strip.getInputLeft(), strip.getInputRight(), strip.params.stereo.load(), false);

        // << and >>, dim without a source.
        const bool enabled = ctx.engine.getSource (sourceIndex()) != nullptr;
        g.setColour (enabled ? colours::caption : colours::dim);
        for (auto [area, left] : { std::pair { toChannel, true }, { toSource, false } })
        {
            auto a = area.toFloat().withSizeKeepingCentre ((float) area.getWidth() * 0.6f, (float) area.getHeight() * 0.36f);
            juce::Path chevrons;
            for (float dx : { 0.0f, 0.45f })
            {
                const float x0 = a.getX() + a.getWidth() * (left ? dx + 0.4f : dx), x1 = a.getX() + a.getWidth() * (left ? dx : dx + 0.4f);
                chevrons.startNewSubPath (x0, a.getY());
                chevrons.lineTo (x1, a.getCentreY());
                chevrons.lineTo (x0, a.getBottom());
            }
            g.strokePath (chevrons, juce::PathStrokeType (juce::jmax (1.5f, a.getHeight() * 0.1f)));
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const int index = sourceIndex();
        auto* src = ctx.engine.getSource (index);
        if (src == nullptr)
            return;
        if (toChannel.contains (e.getPosition()))
        {
            strip.setName (ctx.engine.getSourceName (index));
            strip.params.colour = src->colour.load();
            strip.params.icon = src->icon.load();
        }
        else if (toSource.contains (e.getPosition()))
        {
            ctx.engine.setSourceName (index, strip.getOwnName());
            src->colour = strip.params.colour.load();
            src->icon = strip.params.icon.load();
        }
        repaint();
    }

private:
    int sourceIndex() const { return strip.getInputLeft() >= 0 ? MixerEngine::sourceIndex (strip.getInputLeft()) : -1; }

    void timerCallback() override { repaint(); }

    UiContext& ctx;
    Strip& strip;
    juce::Rectangle<int> channelTile, sourceTile, toChannel, toSource;
};

/** Draws a bracket over the MAIN / ALT keys (Fig. 62). */
class Bracket final : public juce::Component
{
public:
    void paint (juce::Graphics& g) override
    {
        // WING 3.1: 4 px grey lines, 51 px in from each side at the top, then
        // down to the bottom.
        const auto r = getLocalBounds().toFloat();
        const float t = juce::jmax (2.0f, wingPx (4.0f)), arm = r.getWidth() * 51.0f / 360.0f;
        g.setColour (Colours::textDim);
        g.fillRect (r.getX(), r.getY(), arm, t);
        g.fillRect (r.getX(), r.getY(), t, r.getHeight());
        g.fillRect (r.getRight() - arm, r.getY(), arm, t);
        g.fillRect (r.getRight() - t, r.getY(), t, r.getHeight());
    }
};

//==============================================================================
/** INPUT: CHANNEL INPUT, TRIM & BALANCE, FILTER and DELAY (POST) for input
    channels and aux (Fig. 62); FEED, TRIM & BALANCE and the delay (and the
    matrices' DIRECT INPUT) for buses, mains and matrices. */
class InputTab final : public HomeScreen
{
public:
    InputTab (UiContext& c, Strip& s)
        : HomeScreen (c, s),
          trimMeter (
              [this] (int ch)
              {
                  // Post TRIM: the meter bank only reads the input peak (the engine's
                  // trimPeak isn't in it yet), so scale it by the trim and balance.
                  const float bal = strip.getNumChannels() == 2 ? strip.params.input.balanceDb.load() : 0.0f;
                  // As the engine: a positive balance turns the left side down, a negative one the right.
                  const float gainDb = strip.params.input.trimDb.load() + (ch == 0 ? -juce::jmax (0.0f, bal) : juce::jmin (0.0f, bal));
                  return ctx.meters.get (&strip, true, ch) * juce::Decibels::decibelsToGain (gainDb);
              },
              [this] { return strip.getNumChannels(); })
    {
        auto& p = strip.params;
        pad = &make<TrimBalancePad> (p.input.trimDb, p.input.balanceDb, [this] { return strip.getNumChannels() == 2 || strip.isOutput() || strip.params.stereo.load(); });
        trimMeter.showScale = true;
        trimMeter.monoLabel = "IN";
        addAndMakeVisible (trimMeter);
        invert = &key ("INVERT", Icon::Invert, colours::accent);
        invert->captionBelow = true;
        invert->keySide = 48.0f;
        bindKey (*invert, p.input.invert);

        if (strip.isInput())
        {
            bracket = &make<Bracket>();
            // MAIN / ALT: the channel's own switch. A GLOBAL channel follows
            // the global MAIN / ALT switch and is locked here unless GLOBAL
            // INPUT SELECT OVERRIDE is on (manual 5.5 p. 45).
            inputSel = &make<Segmented> (juce::StringArray { "MAIN", "ALT" }, [this] { return strip.isAltSelected() ? 1 : 0; },
                                         [this] (int i) { strip.switchInput (i == 1); });
            inputSel->isEnabledAt = [this] (int) { return strip.canSwitchInput(); };
            // 3.1: MAIN lights blue, ALT orange.
            inputSel->colourAt = [] (int i) { return i == 1 ? colours::alt : colours::fader; };
            mainTile = &make<InputSourcePanel> (ctx, strip, false);
            mainTile->onSelect = [this] { showSourceSelect (true); };
            altTile = &make<InputSourcePanel> (ctx, strip, true);
            sourceSelect = &make<SourceSelectPanel> (ctx, strip);
            sourceSelect->onClose = [this] { showSourceSelect (false); };
            copyCustomization = &make<CopyCustomizationPanel> (ctx, strip);
            inputSelectLabel = &key ("INPUT SELECT");
            inputSelectLabel->setTooltip ("The console's INPUT SELECT (SETUP > AUDIO)");
            // A title bar, not a key [I7].
            inputSelectLabel->offColour = colours::panel;
            inputSelectLabel->offInk = colours::caption;
            inputSelectLabel->frame = colours::panel;
            inputSelectLabel->onClick = [this] { if (ctx.showScreen) ctx.showScreen (Screen::Setup); };
            // The channel in SETUP > AUDIO's GLOBAL INPUT SELECT ENABLE (manual 6.1 p. 68-69).
            selectMode = &make<Segmented> (juce::StringArray { "INDIVIDUAL", "GLOBAL" }, [this] { return strip.isSrcAuto() ? 1 : 0; },
                                           [this] (int i) { strip.setSrcAuto (i == 1); });
            // INDIVIDUAL lights blue, GLOBAL gold (1.11's MANUAL | AUTO were orange).
            selectMode->colourAt = [] (int i) { return i == 1 ? colours::accent : colours::choice; };
            selectMode->vertical = true;
        }
        else
        {
            feed = &key ("FEED CONFIGURATION", std::nullopt);
            feed->onClick = [this] { if (openFeedConfig) openFeedConfig(); };
            feedGrid = &make<FeedGrid> (ctx, strip, [this] { if (openFeedConfig) openFeedConfig(); });
            mono = &key ("MONO", std::nullopt, colours::accent);
            mono->setTooltip ("Sum to mono");
            bindKey (*mono, p.mono);
        }

        if (strip.getKind() == StripKind::Channel)
        {
            loCut = &key ("LOCUT", Icon::LowCut, colours::eqOn);
            loCut->captionBelow = true;
            bindKey (*loCut, p.filter.lowCut);
            hiCut = &key ("HICUT", Icon::HighCut, colours::eqOn);
            hiCut->captionBelow = true;
            bindKey (*hiCut, p.filter.highCut);
            tool = &key (toolFilterName (p.filter.toolModel.load()), toolFilterIcon (p.filter.toolModel.load()), toolFilterColour (p.filter.toolModel.load()));
            tool->captionBelow = true;
            bindKey (*tool, p.filter.tool);
        }

        // Mains and matrices have their delay in the output slot (MAIN screen).
        if (strip.getKind() != StripKind::Main && strip.getKind() != StripKind::Matrix)
        {
            delay = &make<DelayPanel> (ctx, p.input);
            delay->onUnitChanged = [this] { notifyKnobs(); };
        }

        if (strip.getKind() == StripKind::Matrix)
        {
            // Manual 6.4: "Direct Input: an additional signal (AES/EBU, Phones,
            // Speakers or Solo bus)" (/mtx/n/dir/in; AES is silent here).
            directOn = &key ("ON", std::nullopt, colours::accent);
            bindKey (*directOn, p.directOn);
            juce::StringArray directInputs;
            for (int i = 0; i < ld::kNumMatrixDirectInputs; ++i)
                directInputs.add (MixerEngine::matrixDirectName (i));
            directSource = &field ("SOURCE", directInputs, p.directSource, {});
            directInvert = &key (juce::String::fromUTF8 ("\xc3\x98 INVERT"), std::nullopt, colours::accent);
            bindKey (*directInvert, p.directInvert);
            directLevel = &make<KnobBox> (wingFader ("LEVEL", p.directLevelDb));
        }
    }

    /** SELECT MAIN INPUT in place of TRIM & BALANCE, FILTER and DELAY, COPY
        CUSTOMIZATION under the MAIN column [I7]. */
    void showSourceSelect (bool show)
    {
        selecting = show;
        resized();
        repaint();
        notifyKnobs();
    }

    std::vector<HomeKnob> knobs() override
    {
        if (selecting)
            return {};
        auto& p = strip.params;
        auto bal = bindLinear ("BAL", p.input.balanceDb, -9.0f, 9.0f, " dB", 1, 0.0f);
        // [I7] GAIN (the MAIN source's), TRIM, BAL and the delay.
        HomeKnob gain;
        if (strip.isInput())
            gain = { sourceGainBinding (ctx, strip), gainStyle (mainTile) };
        std::vector<HomeKnob> k { gain, { wingTrim ("TRIM", p.input.trimDb), centreStyle (pad) }, {}, { bal, balStyle (pad) }, {} };
        if (delay != nullptr)
            k.push_back ({ delayBinding (ctx, p.input), distStyle (delay->getSlider()) });
        return k;
    }

    void resized() override
    {
        clearBoxes();
        // WING 3.1 pixels: CHANNEL INPUT 112-504, TRIM & BALANCE 512-984,
        // FILTER (y 80-264) over DELAY (POST) 992-1272, down to 752.
        auto left = panelAt (112.0f, 80.0f, 392.0f, 672.0f);
        auto middle = panelAt (512.0f, 80.0f, 472.0f, 672.0f);
        auto right = panelAt (992.0f, 80.0f, 280.0f, 672.0f);

        if (strip.isInput())
        {
            // [3.1, D16] the MAIN / ALT keys under a bracket, then two columns:
            // the framed MAIN column (source tile, +48V, GAIN) and, at the right,
            // the ALT tile's panel, INPUT SELECT and INDIVIDUAL over GLOBAL.
            // WING pixels [I7]: the key row's box 80 tall, then the 192 px
            // columns with 8 px black gaps: the MAIN column; the ALT box (192),
            // the INPUT SELECT bar (64) and the INDIVIDUAL / GLOBAL box.
            box (left, selecting ? "SELECT MAIN INPUT" : "CHANNEL INPUT");
            auto ci = sectionBody (left, true);
            channelInputBody = ci;
            const int gap = juce::roundToInt (juce::jmax (3.0f, wingPx (8.0f)));
            const float v = (float) ci.getHeight() / 600.0f;
            bracketBox = ci.removeFromTop (juce::roundToInt (80.0f * v));
            ci.removeFromTop (gap);
            // WING 3.1: MAIN | ALT 240 x 56 at (188, 164), the bracket from y 186
            // down to 224, x 128-488.
            bracket->setBounds (wingRect (128.0f, 186.0f, 360.0f, 38.0f));
            inputSel->setBounds (wingRect (188.0f, 164.0f, 240.0f, 56.0f));
            auto mainCol = ci.removeFromLeft ((ci.getWidth() - gap) / 2);
            ci.removeFromLeft (gap);
            mainTile->setBounds (mainCol);
            altTile->setBounds (ci.removeFromTop (juce::roundToInt (192.0f * v)));
            ci.removeFromTop (gap);
            const int selTop = ci.getBottom() - juce::roundToInt (248.0f * v);
            ci.setTop (selTop);
            inputSelectLabel->setBounds (ci.removeFromTop (juce::roundToInt (64.0f * v)));
            ci.removeFromTop (gap);
            modesBox = ci;
            const int kh = juce::roundToInt (56.0f * v);
            selectMode->setBounds (ci.withSizeKeepingCentre (juce::roundToInt (ci.getWidth() * 0.58f), kh * 2));

            // SELECT MAIN INPUT: COPY CUSTOMIZATION over the lower half of the
            // MAIN column and the INPUT SELECT boxes; the picker at the right.
            copyArea = {};
            if (selecting)
            {
                copyArea = channelInputBody.withTop (selTop);
                copyCustomization->setBounds (sectionBody (copyArea, true));
                // WING 3.1: the picker from x 560, y 80 down to 404.
                sourceSelect->setBounds (wingRect (560.0f, 80.0f, 712.0f, 324.0f));
                mainTile->setBounds (mainTile->getBounds().withBottom (selTop - gap));
            }
            // [I7] the rest of CHANNEL INPUT dims while the picker is open.
            for (juce::Component* c : { (juce::Component*) bracket, (juce::Component*) inputSel, (juce::Component*) altTile })
                c->setAlpha (selecting ? 0.35f : 1.0f);
            for (juce::Component* c : { (juce::Component*) inputSelectLabel, (juce::Component*) selectMode })
                c->setVisible (! selecting);
            copyCustomization->setVisible (selecting);
            sourceSelect->setVisible (selecting);
        }
        else
        {
            auto feedArea = directOn != nullptr ? left.removeFromTop (left.getHeight() * 55 / 100) : left;
            auto f = box (feedArea, strip.isBus() ? "BUS FEED" : "FEED"); // 3.0.5: opens the FEED CONFIGURATION
            feed->setBounds (f.removeFromBottom (juce::jlimit (24, screenCap (36), f.getHeight() / 6)));
            f.removeFromBottom (6);
            feedGrid->setBounds (f);
            if (directOn != nullptr)
            {
                auto d = box (left, "DIRECT INPUT");
                const int kh = juce::jlimit (22, screenCap (32), d.getHeight() / 5);
                auto row = d.removeFromTop (kh);
                directOn->setBounds (row.removeFromLeft (row.getWidth() / 4));
                row.removeFromLeft (6);
                directSource->setBounds (row);
                d.removeFromTop (6);
                row = d.removeFromTop (kh);
                directInvert->setBounds (row.removeFromLeft (row.getWidth() * 2 / 3).reduced (0, 0));
                d.removeFromTop (6);
                directLevel->setBounds (d.removeFromTop (juce::jmin (screenCap (48), d.getHeight())).withWidth (juce::jmin (d.getWidth(), screenCap (150))));
            }
        }

        for (juce::Component* c : { (juce::Component*) pad, (juce::Component*) &trimMeter, (juce::Component*) invert, (juce::Component*) delay,
                                    (juce::Component*) loCut, (juce::Component*) hiCut, (juce::Component*) tool })
            if (c != nullptr)
                c->setVisible (! selecting);
        if (selecting)
            return;

        // WING 3.1 pixels: the pad's black box 276 x 552 at (544, 172); the
        // meter (one 13 px bar at x 902, CLIP at y 176 to -60 at 500, IN under
        // it); INVERT (48 px, its caption under it) at (884, 652).
        box (middle, "TRIM & BALANCE");
        pad->setBounds (wingRect (544.0f, 172.0f, 276.0f, 552.0f));
        trimMeter.setBounds (wingRect (864.0f, 166.0f, 72.0f, 360.0f));
        invert->setBounds (wingRect (872.0f, 652.0f, 72.0f, 84.0f));
        if (mono != nullptr)
            mono->setBounds (wingRect (872.0f, 560.0f, 72.0f, 64.0f));

        if (loCut != nullptr)
        {
            auto f = box (right.removeFromTop (panelAt (992.0f, 80.0f, 280.0f, 184.0f).getHeight()), "FILTER", true);
            filterHeader = f.withY (f.getY() - panelHeaderHeight() - 4).withHeight (panelHeaderHeight());
            // WING 3.1: 56 px keys 84 apart from x 1020, y 168, captions under them.
            float x = 1006.0f;
            for (auto* k : { loCut, hiCut, tool })
            {
                k->keySide = 56.0f;
                k->setBounds (wingRect (x, 168.0f, 84.0f, 82.0f));
                x += 84.0f;
            }
        }
        if (delay != nullptr)
        {
            box (right, strip.hasPostFaderDelay() ? "DELAY (POST)" : "DELAY");
            delay->setBounds (sectionBody (right, true));
        }
    }

    void paint (juce::Graphics& g) override
    {
        HomeScreen::paint (g);
        if (channelInputBody.isEmpty())
            return;
        // CHANNEL INPUT is several boxes on black [I7, D16].
        g.setColour (Colours::screen);
        g.fillRect (channelInputBody);
        g.setColour (colours::panel);
        g.fillRect (bracketBox);
        if (! selecting)
            g.fillRect (modesBox);
        else
            drawSectionBox (g, copyArea, "COPY CUSTOMIZATION");
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (filterHeader.contains (e.getPosition()) && ctx.showHomeTab)
            ctx.showHomeTab (HomeTab::Filter);
    }

private:
    juce::Rectangle<int> channelInputBody, bracketBox, modesBox, copyArea;
    SourceSelectPanel* sourceSelect = nullptr;
    CopyCustomizationPanel* copyCustomization = nullptr;
    bool selecting = false;
    TrimBalancePad* pad = nullptr;
    LedMeter trimMeter;
    IconKey *invert = nullptr, *mono = nullptr, *inputSelectLabel = nullptr, *feed = nullptr;
    FeedGrid* feedGrid = nullptr;
    IconKey *loCut = nullptr, *hiCut = nullptr, *tool = nullptr;
    IconKey *directOn = nullptr, *directInvert = nullptr;
    ChoiceField* directSource = nullptr;
    KnobBox* directLevel = nullptr;
    Segmented *inputSel = nullptr, *selectMode = nullptr;
    Bracket* bracket = nullptr;
    InputSourcePanel *mainTile = nullptr, *altTile = nullptr;
    DelayPanel* delay = nullptr;
    juce::Rectangle<int> filterHeader;
};

//==============================================================================
/** FILTER SETUP (Fig. 63, as a 3.1 console shows it): the header with the
    title, FREEZE, the LO CUT / HI CUT / tool filter keys and CLOSE; the graph
    with the LC / HC lines; the TOOL FILTER list and the slopes in a panel at
    the right. */
class FilterTab final : public HomeScreen, private juce::Timer
{
public:
    FilterTab (UiContext& c, Strip& s) : HomeScreen (c, s)
    {
        auto& f = strip.params.filter;
        freeze = &key ("FREEZE");
        freeze->outlined = true;
        bindFreeze (*freeze, strip);
        // The header keys: icons with a small caption under them (Fig. 63).
        loCut = &key ("LO CUT", Icon::LowCut, colours::eqOn);
        loCut->iconAbove = true;
        bindKey (*loCut, f.lowCut);
        hiCut = &key ("HI CUT", Icon::HighCut, colours::eqOn);
        hiCut->iconAbove = true;
        bindKey (*hiCut, f.highCut);
        toolKey = &key (toolFilterName (f.toolModel.load()), toolFilterIcon (f.toolModel.load()), toolFilterColour (f.toolModel.load()));
        toolKey->iconAbove = true;
        bindKey (*toolKey, f.tool);
        // On the header bar without a key body, drawn in their colour until lit [3.1].
        for (auto* k : { loCut, hiCut, toolKey })
        {
            k->offColour = colours::panel;
            k->frame = colours::panel;
            k->offInk = colours::eqOn;
        }
        toolKey->offInk = toolFilterColour (f.toolModel.load());
        close = &key ("CLOSE", Icon::Check);
        close->iconAfter = true;
        close->offColour = colours::panel;
        close->frame = colours::panel;
        close->offInk = colours::caption;
        close->onClick = [this] { if (ctx.showHomeTab) ctx.showHomeTab (HomeTab::Home); };

        graph = &make<ResponseGraph>();
        graph->setStrip (&strip, ResponseGraph::Filter);
        graph->setRuler (ResponseGraph::Ruler::Top);

        toolCaption = &label ("TOOL FILTER", 15.0f, colours::caption);
        toolCaption->setJustificationType (juce::Justification::centred);
        for (int m = 0; m < (int) models::table (models::FilterTools).size(); ++m)
        {
            // Each tool lights in its colour: TILT cyan, MAXER orange, AP yellow.
            auto& k = key (toolFilterName (m), toolFilterIcon (m), toolFilterColour (m));
            k.offInk = colours::caption;
            k.isOn = [&f, m] { return f.toolModel.load() == m; };
            k.onClick = [this, &f, m]
            {
                if (f.toolModel.load() == m)
                    f.tool = ! f.tool.load();
                else
                {
                    strip.setModel (ModelBlock::FilterTool, m);
                    f.tool = true;
                }
                notifyKnobs();
            };
            tools.push_back (&k);
        }
        lcCaption = &label ("LC", 15.0f, colours::caption);
        lcCaption->setJustificationType (juce::Justification::centred);
        for (auto** unit : { &lcUnit, &hcUnit })
        {
            *unit = &label ("dB/Oct", 10.0f, colours::dim);
            (*unit)->setJustificationType (juce::Justification::centred);
        }
        lcSlopes = &make<Segmented> (models::list (models::LowCutSlopes), [&f] { return f.lowCutSlope.load(); }, [&f] (int i) { f.lowCutSlope = i; });
        lcSlopes->vertical = true;
        hcCaption = &label ("HC", 15.0f, colours::caption);
        hcCaption->setJustificationType (juce::Justification::centred);
        hcSlopes = &make<Segmented> (models::list (models::HighCutSlopes), [&f] { return f.highCutSlope.load(); }, [&f] (int i) { f.highCutSlope = i; });
        hcSlopes->vertical = true;
        shownTool = f.toolModel.load();
        startTimerHz (4);
    }

    bool hasSettings() const override { return true; }

    std::vector<HomeKnob> knobs() override
    {
        auto& f = strip.params.filter;
        std::vector<HomeKnob> k { { wingFreq ("LC FREQ", f.lowCutFreq, 20.0f, 2000.0f, kLowCutDefaultHz), freqStyle (graph) } };
        // The tool filter's parameters (TILT...) in the middle, HC FREQ always on the 6th knob.
        const auto& spec = models::model (models::FilterTools, f.toolModel.load());
        const auto colour = toolFilterColour (f.toolModel.load());
        for (int i = 0; i < (int) spec.params.size() && i < 4; ++i)
            k.push_back ({ bindConsoleParam (f.toolParams[i], spec.params[(size_t) i]), toolStyle (graph, colour) });
        while (k.size() < 5)
            k.push_back ({});
        k.push_back ({ wingFreq ("HC FREQ", f.highCutFreq, 50.0f, 20000.0f, kHighCutDefaultHz), freqStyle (graph) });
        return k;
    }

    void resized() override
    {
        clearBoxes();
        // In WING pixels from the content's top left (1160 wide) [3.1]: the
        // header 80 high with FREEZE, the LO CUT / HI CUT / tool keys (icons
        // over captions) and CLOSE; the graph under it; at the right a panel
        // with TOOL FILTER over the four tool keys, then the LC and HC slope
        // columns.
        const auto b = getLocalBounds();
        const float u = (float) b.getWidth() / 1160.0f;
        auto at = [u, &b] (float x0, float y0, float x1, float y1)
        {
            return juce::Rectangle<int> (b.getX() + juce::roundToInt (x0 * u), b.getY() + juce::roundToInt (y0 * u), juce::roundToInt ((x1 - x0) * u),
                                         juce::roundToInt ((y1 - y0) * u));
        };
        header = at (0.0f, 0.0f, 1160.0f, 80.0f);
        freeze->setBounds (at (628.0f, 8.0f, 736.0f, 72.0f));
        loCut->setBounds (at (752.0f, 12.0f, 808.0f, 68.0f));
        hiCut->setBounds (at (816.0f, 12.0f, 872.0f, 68.0f));
        toolKey->setBounds (at (880.0f, 12.0f, 936.0f, 68.0f));
        close->setBounds (at (1024.0f, 16.0f, 1144.0f, 64.0f));
        graph->setBounds (at (0.0f, 88.0f, 960.0f, 88.0f).withBottom (b.getBottom()));

        side = at (972.0f, 88.0f, 1160.0f, 88.0f).withBottom (b.getBottom() - juce::roundToInt (6.0f * u));
        toolCaption->setBounds (at (972.0f, 100.0f, 1160.0f, 128.0f));
        for (size_t i = 0; i < tools.size(); ++i)
            tools[i]->setBounds (at (980.0f, 136.0f + 63.0f * (float) i, 1152.0f, 200.0f + 63.0f * (float) i));
        lcCaption->setBounds (at (990.0f, 408.0f, 1062.0f, 432.0f));
        hcCaption->setBounds (at (1069.0f, 408.0f, 1141.0f, 432.0f));
        lcUnit->setBounds (at (990.0f, 430.0f, 1062.0f, 448.0f));
        hcUnit->setBounds (at (1069.0f, 430.0f, 1141.0f, 448.0f));
        lcSlopes->setBounds (at (1000.0f, 450.0f, 1052.0f, 650.0f));
        hcSlopes->setBounds (at (1079.0f, 450.0f, 1131.0f, 550.0f));
    }

    void paint (juce::Graphics& g) override
    {
        HomeScreen::paint (g);
        g.setColour (colours::panel);
        g.fillRect (header);
        g.fillRect (side);
        g.setColour (colours::caption);
        g.setFont (homeFont (TypeScale::header, (float) header.getHeight() * 0.4f));
        const int titleX = juce::roundToInt ((float) header.getWidth() * 33.0f / 1160.0f);
        g.drawText ("FILTER SETUP", header.withTrimmedLeft (titleX).withWidth (header.getWidth() * 36 / 100), juce::Justification::centredLeft, false);
    }

private:
    void timerCallback() override
    {
        auto& f = strip.params.filter;
        if (f.toolModel.load() != shownTool)
        {
            shownTool = f.toolModel.load();
            toolKey->setButtonText (toolFilterName (shownTool));
            toolKey->setIcon (toolFilterIcon (shownTool));
            toolKey->setOnColour (toolFilterColour (shownTool));
            toolKey->offInk = toolFilterColour (shownTool);
            notifyKnobs();
        }
    }

    IconKey *freeze = nullptr, *loCut = nullptr, *hiCut = nullptr, *toolKey = nullptr, *close = nullptr;
    std::vector<IconKey*> tools;
    juce::Label *toolCaption = nullptr, *lcCaption = nullptr, *hcCaption = nullptr, *lcUnit = nullptr, *hcUnit = nullptr;
    Segmented *lcSlopes = nullptr, *hcSlopes = nullptr;
    ResponseGraph* graph = nullptr;
    juce::Rectangle<int> header, side;
    int shownTool = -1;
};

//==============================================================================
/** GATE or COMP slot: the model, its front panel, KEY (and XOVER) and the
    output gain and mix. The model's main parameters are on the six knobs. */
class DynTab final : public HomeScreen, private juce::Timer
{
public:
    /** GATE or COMP as a factory-set 3.1 console shows them, in WING pixels
        from the content's top left (1160 wide): two panel rows over the
        model. Row 1: ON, the model field, MIX and XOVER MODE / XOVER SOLO
        (COMP). Row 2: ACCENT (the gate) or GAIN (COMP), KEY TAP (only while a
        channel keys the slot), KEY SOURCE, KEY FILTER and KEY SOLO. The
        WING's own models show the transfer graph with the ENVELOPE panel and
        the DETECTOR / AUTO ENV keys; the others their front panel. The ">"
        keys of KEY FILTER and XOVER MODE open the GATE KEY FILTER / COMP
        XOVER page over the slot: the filter's response, its type and the
        solo key, FREQ on the knobs. */
    DynTab (UiContext& c, Strip& s, bool gateSlot) : HomeScreen (c, s), isGate (gateSlot), slot (gateSlot ? s.params.gate : s.params.dynamics)
    {
        on = &key ("ON", std::nullopt, colours::accent);
        bindKey (*on, slot.on);
        allowed = strip.getKind() == StripKind::Aux && ! isGate ? models::list (models::AuxDynModels) : juce::StringArray();
        model = &make<ChoiceField> (isGate ? "GATE MODEL" : "DYNAMICS MODEL",
                                    [this] { return models::model (models::Dynamics, slot.model.load()).name.toUpperCase(); },
                                    [this] (ChoiceField& f)
                                    {
                                        ModelSelector::chooseDynamics (f, slot, [this] (int m) { strip.setModel (isGate ? ModelBlock::Gate : ModelBlock::Dynamics, m); },
                                                                       allowed);
                                    });
        model->arrowKey = true;
        mix = &make<KnobBox> (bindLinear ("MIX", slot.mix, 0.0f, 100.0f, " %", 0, 100.0f));
        gain = &make<KnobBox> (bindLinear ("GAIN", slot.gain, -6.0f, 12.0f, " dB", 1, 0.0f));
        accent = &make<KnobBox> (KnobBinding {});
        mix->setVisible (! isGate);
        gain->setVisible (! isGate);

        auto readGr = [this] { return isGate ? strip.meters.gateReductionDb.load() : strip.meters.dynReductionDb.load(); };
        graph = &make<DynTransferGraph> (
            slot, readGr, [this] { return isGate ? strip.meters.gateKeyDb.load() : strip.meters.dynKeyDb.load(); },
            [this] { return isGate ? strip.meters.gateOutPeak.exchange (0.0f) : strip.meters.dynOutPeak.exchange (0.0f); });
        envelope = &make<DynEnvelopeGraph> (slot);
        // DETECTOR left, AUTO ENV right; the 3.1 screen has no ENVELOPE (LIN / LOG) key.
        for (auto [id, caption, names] : { std::tuple { "det", "DETECTOR", juce::StringArray { "PEAK", "RMS" } },
                                           std::tuple { "auto", "AUTO ENV", juce::StringArray { "OFF", "ON" } } })
        {
            const juce::String paramId (id);
            auto& k = make<SegmentedKeys> (
                names,
                [this, paramId]
                {
                    const int i = models::model (models::Dynamics, slot.model.load()).find (paramId);
                    return i < 0 ? -1 : juce::roundToInt (slot.values[i].load());
                },
                [this, paramId] (int v)
                {
                    const int i = models::model (models::Dynamics, slot.model.load()).find (paramId);
                    if (i >= 0)
                        slot.values[i] = (float) v;
                },
                colours::choice);
            toggles.push_back ({ &k, caption, paramId });
        }
        // The model's front panel.
        face = &make<Faceplate>();
        face->onTouched = [this] (int param)
        {
            if (param / 6 != knobPage)
            {
                knobPage = param / 6;
                notifyKnobs();
            }
        };

        keySource = &make<ChoiceField> ("KEY SOURCE",
                                        [this] { return MixerEngine::keySourceName (slot.scSource.load()); },
                                        [this] (ChoiceField& f) { chooseKeySource (f); });
        keySource->arrowKey = true;
        keyFilter = &field ("KEY FILTER", keyFilterNames(), slot.scFilter);
        keyFilter->chevronKey = true;
        keyFilter->onKey = [this] { showPage (Page::Key); };
        // Where the key is taken from the KEY SOURCE (shown while another strip
        // keys the slot): an input channel's tap points, or for aux, buses,
        // mains and matrices BUS, DYN, PFL, AFL, EQ, INS2 (`dynsc/tap`).
        keyTap = &field ("KEY TAP", strip.getKind() == StripKind::Channel ? tapPointNames() : MixerEngine::outputKeyTapNames(), slot.scTap);
        // The key signal after the key filter on the solo bus.
        keySolo = &key ("KEY SOLO");
        keySolo->outlined = true;
        bindListen (*keySolo, strip, isGate ? ld::kListenGateKey : ld::kListenDynKey);

        if (! isGate)
        {
            xoMode = &field ("XOVER MODE", xoverNames(), slot.xoKind);
            xoMode->chevronKey = true;
            xoMode->onKey = [this] { showPage (Page::Xover); };
            xoSolo = &key ("XOVER SOLO");
            xoSolo->outlined = true;
            bindListen (*xoSolo, strip, ld::kListenXover); // the crossover band on the solo bus
        }

        // The GATE KEY FILTER / COMP XOVER page.
        pageClose = &key ("CLOSE", Icon::Check);
        pageClose->iconAfter = true;
        pageClose->offColour = colours::panel;
        pageClose->frame = colours::panel;
        pageClose->offInk = colours::caption;
        pageClose->onClick = [this] { showPage (Page::Slot); };
        pageGraph = &make<ResponseGraph>();
        pageGraph->setRuler (ResponseGraph::Ruler::Top);
        pageType = &typeField ("FILTER TYPE", keyFilterNames(), slot.scFilter);
        if (! isGate)
            pageXoType = &typeField ("XOVER MODE", xoverNames(), slot.xoKind);
        pageKeySolo = &key ("KEY SOLO");
        pageKeySolo->outlined = true;
        bindListen (*pageKeySolo, strip, isGate ? ld::kListenGateKey : ld::kListenDynKey);
        if (! isGate)
        {
            pageXoSolo = &key ("XOVER SOLO");
            pageXoSolo->outlined = true;
            bindListen (*pageXoSolo, strip, ld::kListenXover);
        }
        // The page's DEPTH (the crossover) and Q (band pass, notch, PEAK) boxes.
        pageDepth = &make<KnobBox> (depthBinding());
        pageDepth->ringFill = KnobBox::Ring::Centre;
        pageQ = &make<KnobBox> (KnobBinding {});
        pageQ->ringFill = KnobBox::Ring::Width;
        pageQ->ringColour = pageQ->valueColour = KnobColours::qGreen;
        bindModel();
        startTimerHz (8);
    }

    std::vector<HomeKnob> knobs() override
    {
        std::vector<HomeKnob> k;
        if (page != Page::Slot)
        {
            // [3.1] the crossover's DEPTH on the third knob, Q (band pass,
            // notch, the crossover's PEAK) on the fourth, FREQ on the fifth.
            k.resize (6);
            if (showsDepth())
                k[2] = { depthBinding(), styleOf (KnobStyle::Bar::Centre, KnobColours::white, pageDepth) };
            if (showsQ())
                k[3] = { qBinding(), qStyle (pageQ) };
            k[4] = { wingFreq ("FREQ", page == Page::Xover ? slot.xoFreq : slot.scFreq, 20.0f, 20000.0f, kKeyFilterDefaultHz),
                     styleOf (KnobStyle::Bar::Thumb, KnobColours::blue, pageGraph) };
            return k;
        }
        const auto& spec = models::model (models::Dynamics, slot.model.load());
        const auto order = encoderParams (spec);
        for (int i = knobPage * 6; i < juce::jmin ((int) order.size(), knobPage * 6 + 6); ++i)
        {
            const int p = order[(size_t) i];
            if (p < 0)
            {
                k.push_back ({});
                continue;
            }
            const auto& param = spec.params[(size_t) p];
            juce::Component* target = hasTransferGraph (shownModel) ? (param.id == "thr" ? (juce::Component*) graph : (juce::Component*) envelope) : face;
            k.push_back ({ bindConsoleParam (slot.values[p], param), dynamicsStyle (param, target) });
        }
        return k;
    }

    void resized() override
    {
        clearBoxes();
        const auto b = getLocalBounds();
        const float u = (float) b.getWidth() / 1160.0f;
        auto at = [u, &b] (float x0, float y0, float x1, float y1)
        {
            return juce::Rectangle<int> (b.getX() + juce::roundToInt (x0 * u), b.getY() + juce::roundToInt (y0 * u), juce::roundToInt ((x1 - x0) * u),
                                         juce::roundToInt ((y1 - y0) * u));
        };
        const bool slotPage = page == Page::Slot;
        for (auto* c : std::initializer_list<juce::Component*> { on, model, mix, gain, accent, keyTap, keySource, keyFilter, keySolo, xoMode, xoSolo, graph, envelope, face })
            if (c != nullptr)
                c->setVisible (slotPage);
        for (auto* c : std::initializer_list<juce::Component*> { pageClose, pageGraph, pageType, pageXoType, pageKeySolo, pageXoSolo, pageDepth, pageQ })
            if (c != nullptr)
                c->setVisible (false);
        for (auto& t : toggles)
            t.keys->setVisible (false);
        bands.clear();
        togglePanel = {};
        if (! slotPage)
        {
            layoutPage (at);
            return;
        }

        // Row 1 [3.1].
        bands.push_back (at (0.0f, 0.0f, 1160.0f, 80.0f));
        on->setBounds (at (16.0f, 16.0f, 112.0f, 64.0f));
        model->setBounds (at (128.0f, 8.0f, 432.0f, 72.0f));
        mix->setBounds (at (448.0f, 8.0f, 612.0f, 72.0f));
        mix->setVisible (! isGate);
        if (xoMode != nullptr)
        {
            xoMode->setBounds (at (808.0f, 8.0f, 1000.0f, 72.0f));
            xoSolo->setBounds (at (1016.0f, 8.0f, 1144.0f, 72.0f));
        }

        // Row 2.
        bands.push_back (at (0.0f, 88.0f, 1160.0f, 184.0f));
        gain->setBounds (at (16.0f, 104.0f, 180.0f, 168.0f));
        gain->setVisible (! isGate);
        accent->setBounds (at (16.0f, 104.0f, 180.0f, 168.0f));
        accent->setVisible (isGate && accent->getBinding().isValid());
        keyTap->setBounds (at (392.0f, 104.0f, 584.0f, 168.0f));
        keyTap->setVisible (slot.scSource.load() >= 0);
        keySource->setBounds (at (600.0f, 104.0f, 792.0f, 168.0f));
        keyFilter->setBounds (at (808.0f, 104.0f, 1000.0f, 168.0f));
        keySolo->setBounds (at (1016.0f, 104.0f, 1144.0f, 168.0f));

        // The model: the transfer graph left, ENVELOPE (title, shape, keys) right.
        body = at (0.0f, 192.0f, 1160.0f, 192.0f).withBottom (b.getBottom());
        const bool native = hasTransferGraph (shownModel);
        graph->setVisible (native);
        envelope->setVisible (native);
        face->setVisible (! native);
        if (! native)
        {
            face->setBounds (body);
            return;
        }
        graph->setBounds (body.withRight (at (0.0f, 0.0f, 552.0f, 0.0f).getRight()));
        auto right = body.withLeft (at (560.0f, 0.0f, 560.0f, 0.0f).getX());
        envelope->setBounds (right.withBottom (at (0.0f, 0.0f, 0.0f, 512.0f).getBottom()));
        togglePanel = right.withTop (at (0.0f, 520.0f, 0.0f, 520.0f).getY());
        const auto& spec = models::model (models::Dynamics, shownModel);
        // DETECTOR around x 680, AUTO ENV around x 1040: 128 x 56 keys under their captions.
        for (auto& t : toggles)
        {
            if (spec.find (t.paramId) < 0)
                continue;
            const float cx = t.paramId == "det" ? 680.0f : 1040.0f;
            t.caption = at (cx - 90.0f, 0.0f, cx + 90.0f, 0.0f).withY (togglePanel.getY() + juce::roundToInt (28.0f * u)).withHeight (juce::roundToInt (32.0f * u));
            t.keys->setBounds (at (cx - 64.0f, 0.0f, cx + 64.0f, 0.0f).withY (togglePanel.getY() + juce::roundToInt (68.0f * u)).withHeight (juce::roundToInt (56.0f * u)));
            t.keys->setVisible (true);
        }
    }

    void paint (juce::Graphics& g) override
    {
        HomeScreen::paint (g);
        g.setColour (colours::panel);
        for (auto& r : bands)
            g.fillRect (r);
        if (page != Page::Slot)
        {
            // The page's title over the middle of the graph [3.1].
            const auto header = bands.front();
            g.setColour (colours::caption);
            g.setFont (homeFont (TypeScale::header, (float) header.getHeight() * 0.4f));
            g.drawText (pageTitle(), header.withWidth (header.getWidth() * 960 / 1160), juce::Justification::centred, false);
            return;
        }
        if (! togglePanel.isEmpty())
        {
            g.fillRect (togglePanel);
            g.setColour (Colours::caption);
            g.setFont (wingFontPx (juce::jmin (wingPx (16.0f), (float) togglePanel.getHeight() * 0.11f)) /* 16 px capitals, light grey (3.1) */);
            for (auto& t : toggles)
                if (t.keys->isVisible())
                    g.drawText (t.label, t.caption, juce::Justification::centred, false);
        }
    }

private:
    enum class Page
    {
        Slot,
        Key,
        Xover
    };

    struct Toggle
    {
        SegmentedKeys* keys;
        juce::String label, paramId;
        juce::Rectangle<int> caption {};
    };

    bool isGate;
    DynSlotParams& slot;
    IconKey *on = nullptr, *keySolo = nullptr, *xoSolo = nullptr;
    ChoiceField* model = nullptr;
    juce::StringArray allowed;
    KnobBox *mix = nullptr, *gain = nullptr, *accent = nullptr;
    DynTransferGraph* graph = nullptr;
    DynEnvelopeGraph* envelope = nullptr;
    std::vector<Toggle> toggles;
    Faceplate* face = nullptr;
    ChoiceField *keySource = nullptr, *keyFilter = nullptr, *keyTap = nullptr, *xoMode = nullptr;
    IconKey *pageClose = nullptr, *pageKeySolo = nullptr, *pageXoSolo = nullptr;
    ResponseGraph* pageGraph = nullptr;
    KnobBox *pageDepth = nullptr, *pageQ = nullptr;
    ChoiceField *pageType = nullptr, *pageXoType = nullptr;
    std::vector<juce::Rectangle<int>> bands;
    juce::Rectangle<int> body, togglePanel;
    int shownModel = -1, shownXo = -1, shownKeyFilter = -1, shownSource = -2, knobPage = 0;
    Page page = Page::Slot;

    /** SELECT CHANNEL: SELF, or one of the channels by its colour, number and
        icon. Aux, buses, mains and matrices key from the buses, mains,
        matrices and aux instead, in the WING's order (`dynsc/src`). */
    void chooseKeySource (ChoiceField& f)
    {
        std::vector<popups::ChannelTile> tiles;
        if (strip.getKind() != StripKind::Channel)
        {
            static const char* const prefixes[] = { "", "A", "B", "M", "MX" };
            for (int n = 0; n < MixerEngine::kNumOutputKeySources; ++n)
            {
                const int code = MixerEngine::outputKeySource (n);
                const auto kind = static_cast<StripKind> (code / ld::kKeyStripBase);
                const int index = code % ld::kKeyStripBase;
                const auto& list = ctx.engine.getStrips (kind);
                const Strip* s = juce::isPositiveAndBelow (index, (int) list.size()) ? list[(size_t) index].get() : nullptr;
                popups::ChannelTile t;
                t.number = prefixes[(int) kind] + juce::String (index + 1);
                t.colour = s != nullptr ? stripDisplayColour (*s) : Colours::textDim;
                tiles.push_back (std::move (t));
            }
            popups::chooseChannel (f, "SELECT KEY SOURCE", "SELF", std::move (tiles), MixerEngine::outputKeySourceIndex (slot.scSource.load()),
                                   [this] (int i) { slot.scSource = i < 0 ? -1 : MixerEngine::outputKeySource (i); });
            return;
        }
        for (auto& channel : ctx.engine.getChannels())
        {
            const Strip* ch = channel.get();
            popups::ChannelTile t;
            t.number = juce::String (ch->getNumber());
            t.colour = stripDisplayColour (*ch);
            if (stripHasSource (*ch))
                t.paintIcon = [ch] (juce::Graphics& g, juce::Rectangle<float> r) { drawStripIcon (g, *ch, r, Colours::caption); };
            tiles.push_back (std::move (t));
        }
        popups::chooseChannel (f, "SELECT CHANNEL", "SELF", std::move (tiles), slot.scSource.load(), [this] (int i) { slot.scSource = i; });
    }

    /** The key filter types and crossover modes by the console's long names. */
    static juce::StringArray keyFilterNames() { return { "FLAT", "LOWPASS", "HIGHPASS", "BANDPASS", "NOTCH" }; }
    static juce::StringArray xoverNames() { return { "FLAT", "LOW", "HIGH", "6 dB/Oct", "12 dB/Oct", "PEAK" }; }

    /** The page's type field: its list opens right under it, without a caption row [3.1]. */
    ChoiceField& typeField (const juce::String& caption, const juce::StringArray& items, std::atomic<int32_t>& value)
    {
        return make<ChoiceField> (caption, [items, &value] { return items[value.load()]; },
                                  [items, &value] (ChoiceField& f) { popups::chooseFrom (f, items, value.load(), [&value] (int i) { value = i; }); });
    }

    bool showsDepth() const { return page == Page::Xover && slot.xoKind.load() > 0; }
    bool showsQ() const { return page == Page::Xover ? slot.xoKind.load() == 5 : slot.scFilter.load() >= 3; }
    KnobBinding depthBinding() { return bindLinear ("DEPTH", slot.xoDepth, -30.0f, 30.0f, " dB", 1, 6.0f); }
    KnobBinding qBinding()
    {
        return page == Page::Xover ? bindLog ("Q", slot.xoQ, 0.4f, 4.0f, {}, 0.4f) : bindLog ("Q", slot.scQ, 0.44f, 10.0f, {}, 1.995882f);
    }

    juce::String pageTitle() const
    {
        if (page == Page::Xover)
            return "COMP XOVER";
        return isGate ? "GATE KEY FILTER" : "COMP KEY FILTER";
    }

    bool hidesStatusKeys() const override { return page != Page::Slot; }

    void showPage (Page p)
    {
        page = p;
        if (statusKeysChanged)
            statusKeysChanged();
        if (p != Page::Slot)
            pageGraph->setStrip (&strip, p == Page::Xover ? ResponseGraph::Xover : (isGate ? ResponseGraph::GateKey : ResponseGraph::DynamicsKey));
        notifyKnobs();
        resized();
        repaint();
    }

    /** The key filter / crossover page: the header with the title and CLOSE,
        the graph with the ruler at the top, FILTER TYPE (XOVER MODE) in a
        panel at the right and the solo key at the bottom of the panel under it. */
    template <typename At>
    void layoutPage (At at)
    {
        bands.push_back (at (0.0f, 0.0f, 1160.0f, 80.0f));
        pageClose->setBounds (at (1024.0f, 16.0f, 1144.0f, 64.0f));
        pageClose->setVisible (true);
        const int bottom = getLocalBounds().getBottom();
        pageGraph->setBounds (at (0.0f, 88.0f, 960.0f, 88.0f).withBottom (bottom));
        pageGraph->setVisible (true);
        const bool xover = page == Page::Xover;
        bands.push_back (at (972.0f, 88.0f, 1160.0f, 168.0f));
        auto* type = xover ? pageXoType : pageType;
        type->setBounds (at (980.0f, 96.0f, 1152.0f, 160.0f));
        type->setVisible (true);
        auto lower = at (972.0f, 176.0f, 1160.0f, 176.0f).withBottom (bottom - juce::roundToInt (6.0f * (float) getWidth() / 1160.0f));
        bands.push_back (lower);
        auto* solo = xover ? pageXoSolo : pageKeySolo;
        solo->setBounds (at (980.0f, 0.0f, 1152.0f, 64.0f).withY (lower.getBottom() - juce::roundToInt (72.0f * (float) getWidth() / 1160.0f)));
        solo->setVisible (true);
        // DEPTH, then Q, in the panel's upper part [3.1].
        pageDepth->setBounds (at (980.0f, 256.0f, 1152.0f, 320.0f));
        pageDepth->setVisible (showsDepth());
        pageQ->setBinding (qBinding());
        pageQ->setBounds (at (980.0f, 328.0f, 1152.0f, 392.0f));
        pageQ->setVisible (showsQ());
    }

    /** The parameters on the six knobs, in the console's order: the WING
        models put THR, RATIO, KNEE / RANGE, ATTACK, HOLD, RELEASE there (the
        DETECTOR, ENVELOPE and AUTO keys and ACCENT are on the screen); the
        others their parameters in order. */
    static std::vector<int> encoderParams (const models::Model& spec)
    {
        std::vector<int> order;
        auto add = [&] (std::initializer_list<const char*> ids)
        {
            for (auto* id : ids)
                order.push_back (spec.find (id));
        };
        if (spec.token == "COMP" || spec.token == "EXP")
            add ({ "thr", "ratio", "knee", "att", "hld", "rel" });
        else if (spec.token == "GATE")
            add ({ "thr", "ratio", "range", "att", "hld", "rel" });
        else if (spec.token == "DUCK")
            add ({ "thr", "range", "att", "hld", "rel" });
        else
            for (int i = 0; i < (int) spec.params.size(); ++i)
                order.push_back (i);
        return order;
    }

    void bindModel()
    {
        shownModel = slot.model.load();
        face->bind (
            models::Dynamics, shownModel, slot.values,
            [this] { return -(isGate ? strip.meters.gateReductionDb.load() : strip.meters.dynReductionDb.load()); },
            [this] (int band) { return strip.getDynBandGain (isGate, band); });
        const auto& spec = models::model (models::Dynamics, shownModel);
        const int acc = spec.find ("acc");
        accent->setBinding (acc >= 0 ? bindConsoleParam (slot.values[acc], spec.params[(size_t) acc]) : KnobBinding {});
        knobPage = 0;
        notifyKnobs();
        resized();
        repaint();
    }

    void timerCallback() override
    {
        if (slot.model.load() != shownModel)
            bindModel();
        if (slot.xoKind.load() != shownXo || slot.scFilter.load() != shownKeyFilter || slot.scSource.load() != shownSource)
        {
            // The knobs (Q, DEPTH) and KEY TAP follow the filter types and the key source.
            shownXo = slot.xoKind.load();
            shownKeyFilter = slot.scFilter.load();
            shownSource = slot.scSource.load();
            notifyKnobs();
            resized();
        }
    }
};

//==============================================================================
/** EQ (firmware 3.1): the header with ON, the EQ MODEL field, the MIX knob
    box, FREEZE and, on input channels, the LO CUT / HI CUT / tool filter keys
    and the FILTER field (its ">" opens FILTER SETUP); a piano-key ruler over
    the graph; the WING EQ's band list (the cut / shelf bands with their icon,
    the PEQ bands by number; hold a key to reset its band) with BAND SOLO at
    the bottom of its panel. An analog model shows its front panel under the
    graph instead. */
class EqTab final : public HomeScreen, private juce::Timer
{
public:
    EqTab (UiContext& c, Strip& s) : HomeScreen (c, s)
    {
        auto& eq = strip.params.eq;
        const auto table = static_cast<models::Table> (strip.getEqTable());
        on = &key ("ON", std::nullopt, colours::accent);
        bindKey (*on, eq.on);
        // The console doesn't offer the MACH EQ4 on aux channels.
        if (strip.getKind() == StripKind::Aux)
            for (auto& m : models::table (table))
                if (m.token != "MACH4")
                    allowed.add (m.token);
        model = &make<ChoiceField> ("EQ MODEL", [this, table] { return models::model (table, strip.params.eq.model.load()).name.toUpperCase(); },
                                    [this, table] (ChoiceField& field)
                                    {
                                        ModelSelector::chooseEq (field, table, strip.params.eq.model.load(),
                                                                 [this] (int m) { strip.setModel (ModelBlock::Eq, m); }, allowed);
                                    });
        model->arrowKey = true;
        mix = &make<KnobBox> (bindLinear ("MIX", eq.mix, 0.0f, 125.0f, " %", 0, 100.0f));
        if (strip.getKind() == StripKind::Channel)
        {
            // The FILTER block's keys as on FILTER SETUP: icons over captions in
            // their colour on the header, filled while on [3.1].
            auto& f = strip.params.filter;
            shownTool = f.toolModel.load();
            loCut = &key ("LO CUT", Icon::LowCut, colours::eqOn);
            bindKey (*loCut, f.lowCut);
            hiCut = &key ("HI CUT", Icon::HighCut, colours::eqOn);
            bindKey (*hiCut, f.highCut);
            toolKey = &key (toolFilterName (shownTool), toolFilterIcon (shownTool), toolFilterColour (shownTool));
            bindKey (*toolKey, f.tool);
            for (auto* k : { loCut, hiCut, toolKey })
            {
                k->iconAbove = true;
                k->offColour = colours::panel;
                k->frame = colours::panel;
                k->offInk = colours::eqOn;
            }
            toolKey->offInk = toolFilterColour (shownTool);
            // FILTER: the blocks in use ("LC/HC/TILT"); the field picks the
            // tool filter, its ">" opens FILTER SETUP.
            filter = &make<ChoiceField> ("FILTER", [&f] { return "LC/HC/" + toolFilterToken (f.toolModel.load()); },
                                         [this] (ChoiceField& field)
                                         {
                                             juce::StringArray names;
                                             for (int m = 0; m < (int) models::table (models::FilterTools).size(); ++m)
                                                 names.add (toolFilterName (m));
                                             popups::ListOptions options;
                                             options.caption = "TOOL FILTER";
                                             popups::chooseFrom (field, names, strip.params.filter.toolModel.load(),
                                                                 [this] (int m) { strip.setModel (ModelBlock::FilterTool, m); }, options);
                                         });
            filter->chevronKey = true;
            filter->onKey = [this] { if (ctx.showHomeTab) ctx.showHomeTab (HomeTab::Filter); };
        }
        // BAND SOLO: the selected band's range on the solo bus (it follows
        // the band selection while it plays).
        bandSolo = &key ("BAND SOLO");
        bandSolo->outlined = true;
        bindListen (*bandSolo, strip, ld::kListenBand, [this] { return juce::jmax (0, graph->getSelectedBand()); });
        freeze = &key ("FREEZE");
        freeze->outlined = true;
        bindFreeze (*freeze, strip);

        graph = &make<ResponseGraph>();
        graph->setStrip (&strip, ResponseGraph::Eq);
        graph->setRuler (ResponseGraph::Ruler::Top);
        graph->onBandSelected = [this] (int) { bindBand(); };
        // Analog models show their front panel.
        face = &make<Faceplate>();
        face->onTouched = [this] (int param)
        {
            if (param / kModelKnobs != knobPage)
            {
                knobPage = param / kModelKnobs;
                notifyKnobs();
            }
        };
        shownModel = eq.model.load();
        face->bind (table, shownModel, eq.values);
        startTimerHz (8);
        updateBands();
    }

    bool hasSettings() const override { return true; }

    std::vector<HomeKnob> knobs() override
    {
        auto& eq = strip.params.eq;
        std::vector<HomeKnob> k;
        if (bands.isEmpty())
        {
            const auto& spec = models::model (static_cast<models::Table> (strip.getEqTable()), eq.model.load());
            for (int i = knobPage * kModelKnobs; i < juce::jmin ((int) spec.params.size(), (knobPage + 1) * kModelKnobs); ++i)
                k.push_back ({ bindConsoleParam (eq.values[i], spec.params[(size_t) i]), styleOf (KnobStyle::Bar::Auto, {}, face) });
            while (k.size() < 5)
                k.push_back ({});
            k.push_back ({ bindLinear ("EQ MIX", eq.mix, 0.0f, 125.0f, " %", 0, 100.0f), styleOf (KnobStyle::Bar::Fill, KnobColours::white, mix) });
            return k;
        }
        // The WING EQ: the band's type (low and high bands), then GAIN n
        // (white), Q n (green from the centre), FREQ n (blue) as on the console.
        const auto b = bandBindings();
        k.push_back (b[3].isValid() ? HomeKnob { b[3], styleOf (KnobStyle::Bar::Segments, colours::accent, graph) } : HomeKnob {});
        k.push_back ({});
        k.push_back ({ b[0], centreStyle (graph) });
        k.push_back ({ b[2], qStyle (graph) });
        k.push_back ({ b[1], styleOf (KnobStyle::Bar::Thumb, KnobColours::blue, graph) });
        return k;
    }

    void resized() override
    {
        clearBoxes();
        // The 3.1 screen in WING pixels from the content's top left (1160
        // wide): the header 80 high with ON 16-112, EQ MODEL 128-432, MIX
        // 448-612, FREEZE 628-736, the LO CUT / HI CUT / tool keys 752-936 and
        // FILTER 952-1144; the graph 0-960 under it; the band panel 972-1160.
        const auto b = getLocalBounds();
        const float u = (float) b.getWidth() / 1160.0f;
        auto at = [u, &b] (float x0, float y0, float x1, float y1)
        {
            return juce::Rectangle<int> (b.getX() + juce::roundToInt (x0 * u), b.getY() + juce::roundToInt (y0 * u), juce::roundToInt ((x1 - x0) * u),
                                         juce::roundToInt ((y1 - y0) * u));
        };
        header = at (0.0f, 0.0f, 1160.0f, 80.0f);
        on->setBounds (at (16.0f, 16.0f, 112.0f, 64.0f));
        model->setBounds (at (128.0f, 8.0f, 432.0f, 72.0f));
        mix->setBounds (at (448.0f, 8.0f, 612.0f, 72.0f));
        freeze->setBounds (at (628.0f, 8.0f, 736.0f, 72.0f));
        if (filter != nullptr)
        {
            loCut->setBounds (at (752.0f, 12.0f, 808.0f, 68.0f));
            hiCut->setBounds (at (816.0f, 12.0f, 872.0f, 68.0f));
            toolKey->setBounds (at (880.0f, 12.0f, 936.0f, 68.0f));
            filter->setBounds (at (952.0f, 8.0f, 1144.0f, 72.0f));
        }

        const bool wingEq = ! bands.isEmpty();
        face->setVisible (! wingEq);

        // The band keys 56 high 60 apart from the panel's top, BAND SOLO at its bottom.
        sidePanel = at (972.0f, 88.0f, 1160.0f, 88.0f).withBottom (b.getBottom() - juce::roundToInt (6.0f * u));
        bandSolo->setBounds (at (980.0f, 0.0f, 1152.0f, 64.0f).withY (sidePanel.getBottom() - juce::roundToInt (72.0f * u)));
        bandKeyArea = at (980.0f, 96.0f, 1152.0f, 96.0f).withBottom (bandSolo->getY() - juce::roundToInt (8.0f * u));
        bandKeyGap = juce::roundToInt (4.0f * u);
        bandKeyH = juce::jmin (juce::roundToInt (56.0f * u), bandKeyArea.getHeight() / juce::jmax (6, bands.size()) - bandKeyGap);
        auto rest = at (0.0f, 88.0f, 960.0f, 88.0f).withBottom (b.getBottom());

        if (! wingEq)
        {
            graph->setBounds (rest.removeFromTop (rest.getHeight() * 38 / 100));
            rest.removeFromTop (4);
            face->setBounds (rest);
            freeze->setVisible (true);
            return;
        }
        graph->setBounds (rest);
    }

    void paint (juce::Graphics& g) override
    {
        HomeScreen::paint (g);
        g.setColour (colours::panel);
        g.fillRect (header);
        g.fillRect (sidePanel);

        // The band list.
        bandKeys.clear();
        if (bands.isEmpty())
            return;
        auto area = bandKeyArea;
        const auto& spec = models::model (static_cast<models::Table> (strip.getEqTable()), strip.params.eq.model.load());
        for (int i = 0; i < bands.size(); ++i)
        {
            auto k = area.removeFromTop (bandKeyH).toFloat();
            area.removeFromTop (bandKeyGap);
            bandKeys.push_back (k.toNearestInt());
            const bool sel = i == juce::jmax (0, graph->getSelectedBand());
            paintKeyBody (g, k, sel ? colours::choice : colours::keyOff);
            const auto ink = sel ? juce::Colours::black : colours::caption;
            const auto& id = bands[i];
            // WING 3.1 (a 172 px key): the number or icon centred 43 px in,
            // the type centred 108 px in.
            auto typeArea = k.withTrimmedLeft (k.getWidth() * 0.45f).withWidth (k.getWidth() * 0.36f);
            auto lead = k.removeFromLeft (k.getWidth() * 0.5f);
            juce::String type = "PEQ";
            if (id == "l" || id == "h")
            {
                if (const int t = spec.find (id + "eq"); t >= 0)
                    type = consoleText (spec.params[(size_t) t], strip.params.eq.values[t].load()).toUpperCase();
                const bool cut = type.contains ("CUT");
                const bool shelf = type.contains ("SH");
                const auto icon = id == "l" ? (cut ? Icon::LowCut : (shelf ? Icon::LowShelf : Icon::Bell))
                                            : (cut ? Icon::HighCut : (shelf ? Icon::HighShelf : Icon::Bell));
                if (shelf)
                {
                    // The console's shelf mark: two strokes joining into one,
                    // the join towards the band's side (low: >-, high: -<).
                    const auto a = lead.withSizeKeepingCentre (lead.getHeight() * 0.72f, lead.getHeight() * 0.38f);
                    const bool low = id == "l";
                    const float x0 = low ? a.getX() : a.getRight(), x1 = low ? a.getRight() : a.getX();
                    const float xj = x0 + (x1 - x0) * 0.5f, xs = x0 + (x1 - x0) * 0.22f;
                    juce::Path mark;
                    mark.startNewSubPath (x0, a.getY());
                    mark.lineTo (xs, a.getY());
                    mark.lineTo (xj, a.getCentreY());
                    mark.lineTo (x1, a.getCentreY());
                    mark.startNewSubPath (x0, a.getBottom());
                    mark.lineTo (xs, a.getBottom());
                    mark.lineTo (xj, a.getCentreY());
                    g.setColour (ink);
                    g.strokePath (mark, juce::PathStrokeType (juce::jmax (1.5f, wingPx (2.5f))));
                    type = "SHV";
                }
                else
                    drawIcon (g, icon, lead.reduced (lead.getHeight() * 0.22f), ink);
            }
            else
            {
                // The band number large, the type smaller [D10].
                g.setColour (ink);
                g.setFont (homeFont (TypeScale::header, k.getHeight() * 0.6f, WingWeight::SemiBold));
                g.drawText (id.toUpperCase(), lead.toNearestInt(), juce::Justification::centred);
            }
            g.setColour (ink);
            g.setFont (homeFont (TypeScale::body * 1.1f, k.getHeight() * 0.46f));
            g.drawFittedText (type, typeArea.toNearestInt(), juce::Justification::centred, 1, 0.7f);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        // Hold a band key to reset its band (3.0.6).
        for (size_t i = 0; i < bandKeys.size(); ++i)
            if (bandKeys[i].contains (e.getPosition()))
            {
                const int press = ++pressCount;
                juce::Timer::callAfterDelay (700, [safe = juce::Component::SafePointer<EqTab> (this), press, band = (int) i]
                {
                    if (safe != nullptr && safe->pressCount == press && safe->isMouseButtonDown())
                    {
                        safe->resetBand (band);
                        safe->heldReset = true;
                    }
                });
            }
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        // A double-click on a band key resets the band too (mouse only: taps
        // on a touch screen hold the key instead).
        if (e.source.isMouse())
            for (size_t i = 0; i < bandKeys.size(); ++i)
                if (bandKeys[i].contains (e.getPosition()))
                    resetBand ((int) i);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        ++pressCount;
        if (std::exchange (heldReset, false))
            return;
        for (size_t i = 0; i < bandKeys.size(); ++i)
            if (bandKeys[i].contains (e.getPosition()))
            {
                graph->setSelectedBand ((int) i);
                bindBand();
                repaint();
                return;
            }
    }

private:
    /** An analog model's parameters on the first five knobs (EQ MIX is the sixth), a page at a time. */
    static constexpr int kModelKnobs = 5;

    void updateBands()
    {
        bands = ResponseGraph::stdBands (strip);
        // Band 4 is selected at first, as on a factory-set console.
        if (graph->getSelectedBand() < 0 && ! bands.isEmpty())
            graph->setSelectedBand (bands.contains ("4") ? bands.indexOf ("4") : juce::jmin (bands.size() - 1, 1));
        // Analog models have no bands to solo.
        bandSolo->setEnabled (! bands.isEmpty());
        bindBand();
    }

    /** The band's defaults: no gain, the model's standard frequency and Q. */
    void resetBand (int index)
    {
        if (index < 0 || index >= bands.size())
            return;
        auto& eq = strip.params.eq;
        const auto& spec = models::model (static_cast<models::Table> (strip.getEqTable()), eq.model.load());
        for (auto suffix : { "g", "f", "q", "eq" })
            if (const int i = spec.find (bands[index] + suffix); i >= 0)
                eq.values[i] = spec.params[(size_t) i].defaultValue;
        graph->setSelectedBand (index);
        bindBand();
    }

    /** GAIN, FREQ, Q and the band type of the selected band. */
    std::vector<KnobBinding> bandBindings() const
    {
        auto& eq = strip.params.eq;
        std::vector<KnobBinding> k;
        if (bands.isEmpty())
            return k;
        const auto& spec = models::model (static_cast<models::Table> (strip.getEqTable()), eq.model.load());
        const auto band = bands[juce::jlimit (0, bands.size() - 1, graph->getSelectedBand())];
        const auto suffixName = band.toUpperCase();
        const bool shelf = isShelf (spec, band);
        for (auto suffix : { "g", "f", "q", "eq" })
        {
            // A shelf has no Q on the console (its slope is fixed).
            const int i = shelf && juce::String (suffix) == "q" ? -1 : spec.find (band + suffix);
            auto b = i >= 0 ? bindConsoleParam (eq.values[i], spec.params[(size_t) i]) : KnobBinding {};
            if (i >= 0)
            {
                const juce::String s (suffix);
                b.label = (s == "g" ? "GAIN " : (s == "f" ? "FREQ " : (s == "q" ? "Q " : "BAND "))) + suffixName;
                if (s == "f")
                    b.text = [&v = eq.values[i]] { return wingHz (v.load()); };
            }
            k.push_back (b);
        }
        return k;
    }

    /** The band is a shelf (the outer bands' SHV type). */
    bool isShelf (const models::Model& spec, const juce::String& band) const
    {
        const int i = spec.find (band + "eq");
        return i >= 0 && spec.params[(size_t) i].display (strip.params.eq.values[i].load()) == "SHV";
    }

    void bindBand()
    {
        if (! bands.isEmpty())
            Analysis (strip).followBand (strip, juce::jmax (0, graph->getSelectedBand()));
        repaint();
        notifyKnobs();
    }

    void timerCallback() override
    {
        auto& eq = strip.params.eq;
        if (toolKey != nullptr && strip.params.filter.toolModel.load() != shownTool)
        {
            // The tool filter changed: its key follows.
            shownTool = strip.params.filter.toolModel.load();
            toolKey->setButtonText (toolFilterName (shownTool));
            toolKey->setIcon (toolFilterIcon (shownTool));
            toolKey->setOnColour (toolFilterColour (shownTool));
            toolKey->offInk = toolFilterColour (shownTool);
        }
        if (! bands.isEmpty())
        {
            // The selected band switched between shelf and bell: its Q comes or goes.
            const auto& spec = models::model (static_cast<models::Table> (strip.getEqTable()), eq.model.load());
            if (const bool shelf = isShelf (spec, bands[juce::jlimit (0, bands.size() - 1, graph->getSelectedBand())]); shelf != shownShelf)
            {
                shownShelf = shelf;
                notifyKnobs();
            }
        }
        if (eq.model.load() == shownModel)
            return;
        shownModel = eq.model.load();
        face->bind (static_cast<models::Table> (strip.getEqTable()), shownModel, eq.values);
        knobPage = 0;
        updateBands();
        resized();
        notifyKnobs();
    }

    IconKey *on = nullptr, *loCut = nullptr, *hiCut = nullptr, *toolKey = nullptr, *bandSolo = nullptr, *freeze = nullptr;
    ChoiceField *model = nullptr, *filter = nullptr;
    KnobBox* mix = nullptr;
    ResponseGraph* graph = nullptr;
    Faceplate* face = nullptr;
    juce::StringArray bands, allowed;
    std::vector<juce::Rectangle<int>> bandKeys;
    juce::Rectangle<int> header, sidePanel, bandKeyArea;
    int bandKeyH = 30, bandKeyGap = 4, shownModel = -1, shownTool = -1, knobPage = 0, pressCount = 0;
    bool heldReset = false, shownShelf = false;
};

//==============================================================================
/** One channel of the automix screen: colour strip with the number, icon and
    name, the X / Y group, the attenuation meter and the weight slider. The
    selected channel set to AUTO with INS 2 off is not a member: its group key
    is drawn dim and its meter stays empty. */
class AutomixColumn final : public juce::Component, private juce::Timer
{
public:
    AutomixColumn (Strip& s, bool isSelected) : strip (s), selected (isSelected) { startTimerHz (15); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (colours::panel);
        g.fillRect (r);
        auto top = r.removeFromTop (juce::jlimit (12.0f, screenCap (18.0f), r.getHeight() * 0.06f));
        g.setColour (stripDisplayColour (strip));
        g.fillRect (top);
        g.setColour (juce::Colours::black);
        g.setFont (homeFont (TypeScale::value, top.getHeight() * 0.85f, WingWeight::SemiBold));
        g.drawText (juce::String (strip.getNumber()), top.toNearestInt(), juce::Justification::centred);
        auto iconArea = r.removeFromTop (r.getHeight() * 0.1f);
        drawChannelIcon (g, strip.getIcon(), strip.getKind(), iconArea.reduced (iconArea.getHeight() * 0.15f), colours::caption);
        auto name = r.removeFromTop (14.0f);
        g.setColour (Colours::text);
        g.setFont (wingFont (TypeScale::caption));
        g.drawFittedText (stripName (strip).toUpperCase(), name.toNearestInt(), juce::Justification::centred, 1, 0.6f);
        groupKey = r.removeFromTop (juce::jlimit (16.0f, screenCap (24.0f), r.getHeight() * 0.1f)).reduced (r.getWidth() * 0.2f, 1.0f);
        const int mode = strip.params.postInsert.mode.load();
        const bool member = strip.automixGroup() >= 0;
        g.setColour (! member ? colours::dim : (mode == 1 ? colours::fader : Colours::active));
        g.fillRect (groupKey);
        g.setColour (juce::Colours::black);
        g.setFont (homeFont (TypeScale::value, groupKey.getHeight() * 0.8f, WingWeight::SemiBold));
        g.drawText (mode == 1 ? "X" : "Y", groupKey.toNearestInt(), juce::Justification::centred);
        r.removeFromTop (4.0f);

        // Attenuation meter (yellow, from the top) and the weight slider.
        auto meter = r.removeFromRight (juce::jmax (4.0f, r.getWidth() * 0.18f)).reduced (0.0f, 2.0f);
        g.setColour (colours::well);
        g.fillRect (meter);
        const float att = member ? juce::jlimit (0.0f, 1.0f, -strip.meters.automixDb.load() / 40.0f) : 0.0f;
        const int segs = 20;
        for (int i = 0; i < segs; ++i)
        {
            const float t = (float) i / (float) segs;
            if (t >= att)
                break;
            g.setColour (colours::accent);
            g.fillRect (meter.getX(), meter.getY() + meter.getHeight() * t + 0.5f, meter.getWidth(), meter.getHeight() / (float) segs - 1.0f);
        }
        slider = r.reduced (r.getWidth() * 0.2f, 2.0f);
        g.setColour (colours::well);
        g.fillRect (slider);
        const float pos = (strip.params.postInsert.weight.load() + 12.0f) / 24.0f;
        const float y = slider.getBottom() - slider.getHeight() * pos;
        auto tag = juce::Rectangle<float> (juce::jmin (r.getWidth(), screenCap (40.0f)), 18.0f).withCentre ({ slider.getCentreX(), y });
        g.setColour (Colours::text);
        g.fillRect (tag);
        g.setColour (juce::Colours::black);
        g.setFont (wingFont (TypeScale::body, WingWeight::SemiBold));
        g.drawText (juce::String (strip.params.postInsert.weight.load(), 1), tag.toNearestInt(), juce::Justification::centred);

        if (selected)
        {
            g.setColour (colours::accent);
            g.drawRect (getLocalBounds().toFloat().reduced (1.0f), 2.0f);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        start = strip.params.postInsert.weight.load();
        if (groupKey.contains (e.position))
        {
            strip.params.postInsert.mode = strip.params.postInsert.mode.load() == 1 ? 2 : 1;
            repaint();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (groupKey.contains (e.mouseDownPosition))
            return;
        strip.params.postInsert.weight = juce::jlimit (-12.0f, 12.0f, start - (float) e.getDistanceFromDragStartY() / juce::jmax (40.0f, slider.getHeight()) * 24.0f);
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override { strip.params.postInsert.weight = 0.0f; }

private:
    void timerCallback() override { repaint(); }

    Strip& strip;
    bool selected;
    juce::Rectangle<float> groupKey, slider;
    float start = 0.0f;
};

/** INS 1 / INS 2 (firmware 3.1): ON, the FX PROCESSOR field with its `›` key
    (the slot on EFFECTS), SET CUST (buses, mains, matrices) and the FX TYPE
    field; then the FX MIX knob box, RESET and FX SPILL; the effect's front
    panel below. INS 2 of an input channel offers FX | AUTO - X | AUTO - Y: in
    automix mode it shows the channels of the automix groups. */
class InsertTab final : public HomeScreen, private juce::Timer
{
public:
    InsertTab (UiContext& c, Strip& s, bool postInsert) : HomeScreen (c, s), post (postInsert), ins (post ? s.params.postInsert : s.params.preInsert)
    {
        on = &key ("ON", std::nullopt, colours::accent);
        bindKey (*on, ins.on);
        processor = &make<ChoiceField> ("FX PROCESSOR", [this]
                                        {
                                            const int slot = ins.fx.load();
                                            return slot < 0 ? juce::String ("NONE") : "FX" + juce::String (slot + 1);
                                        },
                                        [this] (ChoiceField&) { chooseProcessor(); });
        processor->keyHidden = true; // the > key beside it is the field's key [D8]
        openSlot = &key ({}, Icon::ChevronRight);
        openSlot->offInk = colours::ink; // [3.1] a white chevron
        openSlot->setTooltip ("Edit the slot on EFFECTS");
        openSlot->onClick = [this]
        {
            if (const int slot = ins.fx.load(); slot < 0)
                chooseProcessor(); // no slot yet: choose one
            else if (ctx.editFx)
                ctx.editFx (slot);
        };
        type = &make<ChoiceField> ("FX TYPE", [this] { return typeText; }, [this] (ChoiceField& field)
                                   {
                                       if (const int slot = ins.fx.load(); slot >= 0)
                                           showFxDeviceMenu (ctx, slot, &field, [this] { shownSlot = -2; }, this);
                                   });
        type->arrowKey = true;
        reset = &key ("RESET", Icon::Reset);
        reset->iconAbove = true;
        reset->outlined = true;
        reset->frame = juce::Colours::transparentBlack; // [3.1] an icon over its label, no frame
        reset->onClick = [this]
        {
            const int slot = ins.fx.load();
            if (slot < 0)
                return;
            auto& fx = ctx.engine.getFx();
            const auto& spec = models::model (models::Fx, fx.getModel (slot));
            for (size_t i = 0; i < spec.params.size(); ++i)
                fx.params (slot).values[i] = spec.params[i].defaultValue;
        };
        // FX SPILL puts the effect's parameters on the left fader section; again ends it.
        spill = &key ("FX SPILL", Icon::Faders); // [3.1] a row of faders
        spill->outlined = true;
        spill->isOn = [this] { return ins.fx.load() >= 0 && isSpilling (ctx, { LayerSet::Spill::Kind::Fx, ins.fx.load(), {} }); };
        spill->onClick = [this]
        {
            if (const int slot = ins.fx.load(); slot >= 0)
                toggleSpill (ctx, std::nullopt, { LayerSet::Spill::Kind::Fx, slot, {} });
        };
        // SET CUST (every strip's INS, 3.1).
        setCust = &key ("SET CUST", Icon::KeyboardEdit);
        setCust->iconAbove = true;
        setCust->outlined = true;
        setCust->frame = juce::Colours::transparentBlack;
        setCust->setTooltip ("Name the channel after the effect and give it the FX icon");
        setCust->onClick = [this]
        {
            const int slot = ins.fx.load();
            if (slot < 0 || ! ctx.engine.getFx().isLoaded (slot))
                return;
            auto* plugin = ctx.engine.getFx().getPlugin (slot);
            // [3.1] The device's short name and the FX icon; the colour stays.
            strip.setName (plugin != nullptr ? plugin->getName().toUpperCase() : juce::String (models::model (models::Fx, ctx.engine.getFx().getModel (slot)).token));
            strip.params.icon = 6;
        };
        openEditor = &key ("OPEN EDITOR", Icon::Edit);
        openEditor->onClick = [this]
        {
            const int slot = ins.fx.load();
            if (slot >= 0 && ctx.engine.getFx().hasPlugin (slot) && ctx.openPluginEditor)
                ctx.openPluginEditor (slot);
            else if (slot >= 0 && ctx.engine.getFx().isOutboard (slot) && ctx.editFx)
                ctx.editFx (slot); // the EXTERNAL EFFECT page
        };
        face = &make<Faceplate>();
        hint = &label ({}, 15.0f, colours::dim);
        hint->setJustificationType (juce::Justification::centred);

        if (post && strip.hasAutomix())
        {
            // FX | AUTO - X | AUTO - Y (automix only on the input channels).
            modeKeys = &make<Segmented> (juce::StringArray { "FX", "AUTO - X", "AUTO - Y" }, [this] { return ins.mode.load(); },
                                         [this] (int m)
                                         {
                                             ins.mode = m;
                                             rebuildAutomix();
                                         });
            // The automix groups' global enables.
            xEnable = &key ("X ENABLE", std::nullopt, colours::fader);
            bindKey (*xEnable, ctx.engine.automixEnable[0]);
            yEnable = &key ("Y ENABLE", std::nullopt, Colours::active);
            bindKey (*yEnable, ctx.engine.automixEnable[1]);
        }
        startTimerHz (4);
        rebuildAutomix();
        timerCallback();
    }

    bool automix() const { return post && strip.hasAutomix() && ins.mode.load() > 0; }

    /** SELECT FX PROCESSOR: NONE and the 16 slots of the rack. */
    void chooseProcessor()
    {
        showFxProcessorWindow (ctx, *this, ins.fx.load(), [this] (int slot) { ins.fx = slot; });
    }

    /** The channels the automix screen lists: the members of groups X and Y
        (INS 2 AUTO X / AUTO Y and ON, as the engine shares gain), and the
        selected channel itself (drawn as not a member while its INS 2 is off),
        with the structure they belong to. */
    std::vector<std::pair<Strip*, int>> automixMembers() const
    {
        std::vector<std::pair<Strip*, int>> list;
        if (! automix())
            return list;
        list.emplace_back (nullptr, ctx.engine.getStructureVersion());
        for (auto& ch : ctx.engine.getChannels())
            if (const int group = ch->automixGroup(); group >= 0 || ch.get() == &strip)
                list.emplace_back (ch.get(), group);
        return list;
    }

    void rebuildAutomix()
    {
        for (auto* col : columns)
            owned.removeObject (col);
        columns.clear();
        shownMembers = automixMembers();
        for (auto& [ch, group] : shownMembers)
            if (ch != nullptr)
                columns.push_back (&make<AutomixColumn> (*ch, ch == &strip));
        shownAutomix = automix();
        resized();
        notifyKnobs();
    }

    std::vector<HomeKnob> knobs() override
    {
        std::vector<HomeKnob> k;
        if (automix())
            return k;
        const int slot = ins.fx.load();
        auto& fx = ctx.engine.getFx();
        if (slot < 0 || ! fx.isLoaded (slot))
            return k;
        if (fx.hasPlugin (slot))
            return { { bindLinear ("FX MIX", fx.params (slot).mix, 0.0f, 100.0f, " %", 0, 100.0f), styleOf (KnobStyle::Bar::Fill, KnobColours::blue, mixBox) } };
        if (fx.isOutboard (slot))
            return { { bindLinear ("TRIM", fx.params (slot).values[ld::ext::kTrim], -18.0f, 18.0f, " dB", 1, 0.0f), styleOf (KnobStyle::Bar::Auto, {}, nullptr) },
                     { bindLinear ("FX MIX", fx.params (slot).mix, 0.0f, 100.0f, " %", 0, 100.0f), styleOf (KnobStyle::Bar::Fill, KnobColours::blue, mixBox) } };
        const auto& spec = models::model (models::Fx, fx.getModel (slot));
        for (int i = knobPage * 6; i < juce::jmin ((int) spec.params.size(), knobPage * 6 + 6); ++i)
            k.push_back ({ bindConsoleParam (fx.params (slot).values[i], spec.params[(size_t) i]), styleOf (KnobStyle::Bar::Auto, {}, face) });
        return k;
    }

    void timerCallback() override
    {
        const int slot = ins.fx.load();
        auto& fx = ctx.engine.getFx();
        const int model = slot >= 0 ? fx.getModel (slot) : -1;
        const bool plugin = slot >= 0 && fx.hasPlugin (slot);
        const bool outboard = slot >= 0 && fx.isOutboard (slot);
        // Channels joining or leaving a group (here, on their own INS 2, from a
        // scene or a remote) and added or removed channels rebuild the list.
        if (automix() != shownAutomix || automixMembers() != shownMembers)
            rebuildAutomix();
        if (slot != shownSlot || model != shownModel || plugin != shownPlugin || outboard != shownOutboard)
        {
            shownSlot = slot;
            shownModel = model;
            shownPlugin = plugin;
            shownOutboard = outboard;
            knobPage = 0;
            typeText = "NONE";
            if (plugin)
                typeText = fx.getPlugin (slot)->getName().toUpperCase();
            else if (outboard)
                typeText = "EXTERNAL (OUTBOARD)";
            else if (model > ld::kFxExt)
                typeText = models::model (models::Fx, model).name.toUpperCase();
            // [3.1] the keys stay lit as long as they show (RESET of an empty slot does nothing).
            type->setEnabled (slot >= 0);
            spill->setEnabled (slot >= 0);
            // FX MIX follows the slot.
            if (mixBox != nullptr)
                owned.removeObject (mixBox);
            mixBox = nullptr;
            if (slot >= 0 && fx.isLoaded (slot))
                mixBox = &make<KnobBox> (bindLinear ("FX MIX", fx.params (slot).mix, 0.0f, 100.0f, " %", 0, 100.0f));
            if (slot >= 0 && model > ld::kFxExt && ! plugin)
            {
                face->bind (
                    models::Fx, model, fx.params (slot).values, [&fx, slot] { return fx.getMeter (slot); },
                    [&fx, slot] (int band) { return fx.getBandGain (slot, band); });
                face->onTouched = [this] (int param)
                {
                    if (param / 6 != knobPage)
                    {
                        knobPage = param / 6;
                        notifyKnobs();
                    }
                };
            }
            else
                face->unbind();
            // [3.1] without a slot or a device the page below the rows is empty.
            hint->setText (plugin     ? "Plugin: open its editor."
                           : outboard ? "Outboard insert, return " + fx.returnLabel (slot) + ": open it on EFFECTS."
                                      : juce::String(),
                           juce::dontSendNotification);
            resized();
            repaint();
            notifyKnobs();
        }
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        // The 3.1 layout (WING pixels from the content's left edge, 1160 wide):
        // row 1 ON 16-112, FX PROCESSOR 128-288 + its key to 320, SET CUST
        // 336-416, FX | AUTO - X | AUTO - Y 512-798, FX TYPE 840-1144; row 2
        // FX MIX 16-172, RESET 904-968, FX SPILL 988-1144. Without a slot
        // only ON, FX PROCESSOR, SET CUST and the mode keys show.
        // [D8] row 1 is 80 of the 670 px page: 64 px fields, the 48 px ON key.
        header = b.removeFromTop (juce::jlimit (32, screenCap (72), b.getHeight() * 12 / 100));
        const float u = (float) header.getWidth() / 1160.0f;
        auto span = [u, this] (juce::Rectangle<int> row, float x0, float x1)
        { return juce::Rectangle<int> (header.getX() + juce::roundToInt (x0 * u), row.getY(), juce::roundToInt ((x1 - x0) * u), row.getHeight()); };
        auto h = header.withSizeKeepingCentre (header.getWidth(), header.getHeight() * 4 / 5);
        on->setBounds (span (h, 16.0f, 112.0f).withSizeKeepingCentre (juce::roundToInt (96.0f * u), header.getHeight() * 3 / 5));
        const bool am = automix();
        b.removeFromTop (juce::jmax (4, getHeight() / 100));
        row2 = b.removeFromTop (juce::jlimit (36, screenCap (76), b.getHeight() / 6));
        auto r2 = row2.reduced (0, juce::jmax (4, row2.getHeight() / 8));
        b.removeFromTop (juce::jmax (4, getHeight() / 100));

        for (auto* c : { (juce::Component*) processor, (juce::Component*) openSlot, (juce::Component*) setCust, (juce::Component*) face,
                         (juce::Component*) hint, (juce::Component*) openEditor })
            c->setVisible (! am);
        for (auto* c : { (juce::Component*) type, (juce::Component*) reset, (juce::Component*) spill })
            c->setVisible (! am && shownSlot >= 0);
        if (mixBox != nullptr)
            mixBox->setVisible (! am);

        if (modeKeys != nullptr)
        {
            // Automix: the mode keys in the header, the group enables in row 2.
            modeKeys->setBounds (span (h, am ? 128.0f : 512.0f, am ? 560.0f : 798.0f).withSizeKeepingCentre (juce::roundToInt ((am ? 432.0f : 286.0f) * u),
                                                                                                          header.getHeight() * 7 / 10));
            xEnable->setBounds (span (r2, 600.0f, 760.0f));
            yEnable->setBounds (span (r2, 776.0f, 936.0f));
            xEnable->setVisible (am);
            yEnable->setVisible (am);
        }
        if (am)
        {
            auto area = b.reduced (0, 2);
            const int w = juce::jmin (90, area.getWidth() / juce::jmax (1, (int) columns.size()));
            for (auto* col : columns)
                col->setBounds (area.removeFromLeft (w));
            return;
        }

        auto proc = span (h, 128.0f, 320.0f);
        openSlot->setBounds (proc.removeFromRight (proc.getHeight() / 2));
        processor->setBounds (proc);
        setCust->setBounds (span (h, 336.0f, 416.0f));
        type->setBounds (span (h, 840.0f, 1144.0f));
        if (mixBox != nullptr)
            mixBox->setBounds (span (r2, 16.0f, 172.0f));
        reset->setBounds (span (r2, 904.0f, 968.0f));
        spill->setBounds (span (r2, 988.0f, 1144.0f));

        openEditor->setVisible (shownPlugin || shownOutboard);
        if (shownPlugin || shownOutboard)
            openEditor->setBounds (b.withSizeKeepingCentre (juce::jmin (screenCap (200), b.getWidth()), 40));
        hint->setBounds (b.withTrimmedTop (b.getHeight() / 2 + 24).withHeight (24));
        face->setBounds (b);
    }

    void paint (juce::Graphics& g) override
    {
        HomeScreen::paint (g);
        g.setColour (colours::panel);
        g.fillRect (header);
        g.fillRect (row2);
    }

private:
    bool post;
    InsertParams& ins;
    IconKey *on = nullptr, *openSlot = nullptr, *reset = nullptr, *spill = nullptr, *setCust = nullptr, *openEditor = nullptr;
    IconKey *xEnable = nullptr, *yEnable = nullptr;
    ChoiceField *processor = nullptr, *type = nullptr;
    KnobBox* mixBox = nullptr;
    juce::Label* hint = nullptr;
    Segmented* modeKeys = nullptr;
    Faceplate* face = nullptr;
    std::vector<AutomixColumn*> columns;
    std::vector<std::pair<Strip*, int>> shownMembers;
    juce::Rectangle<int> header, row2;
    juce::String typeText { "NONE" };
    int shownSlot = -2, shownModel = -2, knobPage = 0;
    bool shownPlugin = false, shownOutboard = false, shownAutomix = false;
};

//==============================================================================
/** One main send (D7): the main's label on its colour and its name, a box with
    MAIN and the PRE key, the send fader (blue, grey while off) and ON. */
class MainSendColumn final : public juce::Component
{
public:
    MainSendColumn (Strip& mainStrip, MainSendParams& p) : main (mainStrip), send (p), fader (p.levelDb, false)
    {
        bindKey (pre, send.pre);
        bindKey (on, send.on);
        fader.isOn = [this] { return send.on.load(); };
        fader.fillColour = [this] { return send.on.load() ? colours::fader : colours::dim; };
        fader.wellColour = colours::panel;
        for (juce::Component* c : { (juce::Component*) &pre, (juce::Component*) &on, (juce::Component*) &fader })
            addAndMakeVisible (c);
    }

    juce::Component* getFader() noexcept { return &fader; }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (juce::jmax (2.0f, getWidth() * 0.06f), 0.0f);
        auto label = r.removeFromTop (stripH);
        g.setColour (main.getColour());
        g.fillRect (label);
        g.setColour (juce::Colours::black);
        g.setFont (homeFont (TypeScale::body, stripH * 0.82f));
        g.drawText (shortLabel (main), label.toNearestInt(), juce::Justification::centred);
        auto name = r.removeFromTop (nameH);
        g.setColour (colours::caption);
        g.setFont (homeFont (TypeScale::body, nameH * 0.7f));
        g.drawFittedText (hasDefaultName (main) ? juce::String() : stripName (main), name.toNearestInt(), juce::Justification::centred, 1, 0.6f);
        g.setColour (colours::panel);
        g.fillRect (boxArea);
        g.setColour (colours::caption);
        g.setFont (homeFont (TypeScale::body, boxArea.getHeight() * 0.22f));
        g.drawText ("MAIN", boxArea.withHeight (boxArea.getHeight() * 0.3f).toNearestInt(), juce::Justification::centred);
    }

    void resized() override
    {
        stripH = (float) juce::jlimit (11, screenCap (18), getHeight() / 33);
        nameH = (float) juce::jlimit (12, screenCap (22), getHeight() / 26);
        auto r = getLocalBounds().reduced (juce::jmax (2, getWidth() * 6 / 100), 0);
        r.removeFromTop ((int) (stripH + nameH) + 2);
        boxArea = r.removeFromTop (juce::jlimit (40, screenCap (96), getHeight() * 17 / 100)).toFloat();
        pre.setBounds (boxArea.toNearestInt().withTrimmedTop ((int) (boxArea.getHeight() * 0.3f)).reduced (juce::jlimit (1, screenCap (4), getWidth() / 10), 4));
        r.removeFromTop (6);
        r.removeFromBottom (6);
        on.setBounds (r.removeFromBottom (juce::jlimit (18, screenCap (34), getHeight() / 11)));
        r.removeFromBottom (6);
        fader.setBounds (r);
    }

private:
    Strip& main;
    MainSendParams& send;
    IconKey pre { "PRE", std::nullopt, colours::accent }, on { "ON", std::nullopt, colours::accent };
    ScreenFader fader;
    juce::Rectangle<float> boxArea;
    float stripH = 14.0f, nameH = 14.0f;
};

/** LR VECTOR SCOPE (D7, Fig. 65): the strip's left and right after its INPUT
    block, before pan, as a goniometer - a mono signal is a vertical line, L
    or R alone leans 45 degrees, a signal out of phase lies flat. A green
    trace on a dark green dotted square, scaled to the signal, over its
    caption. While it shows, the engine keeps a decimated L/R ring of the
    strip (ld_scope_read). */
class VectorScope final : public juce::Component, private juce::Timer
{
public:
    explicit VectorScope (Strip& s) : strip (s) { startTimerHz (30); }

    /** The square, without the caption under it. */
    juce::Rectangle<float> scopeArea() const
    {
        auto r = getLocalBounds().toFloat().reduced (1.5f);
        r.removeFromBottom (juce::jlimit (11.0f, screenCap (18.0f), r.getHeight() * 0.12f));
        return r;
    }

    void paint (juce::Graphics& g) override
    {
        auto all = getLocalBounds().toFloat();
        g.setColour (colours::outline);
        g.fillRect (all);
        auto inner = all.reduced (1.5f);
        const auto sq = scopeArea();
        const auto caption = inner.withTop (sq.getBottom());
        g.setColour (juce::Colour (0xff101810));
        g.fillRect (sq);
        g.setColour (juce::Colour (0xff1c3020));
        const float d = sq.getWidth() / 16.0f, e = sq.getHeight() / 16.0f;
        for (int i = 1; i < 16; ++i)
            for (int j = 1; j < 16; ++j)
                g.fillRect (sq.getX() + d * (float) i, sq.getY() + e * (float) j, 1.0f, 1.0f);
        g.setColour (juce::Colour (0xff202020));
        g.fillRect (caption);
        g.setColour (colours::caption);
        g.setFont (homeFont (TypeScale::caption, caption.getHeight() * 0.8f));
        g.drawFittedText ("LR VECTOR SCOPE", caption.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.6f);

        if (count <= 0)
            return;
        // M up, S across, scaled so the loudest recent excursion fills ~90 %.
        const auto c = sq.getCentre();
        const float half = juce::jmin (sq.getWidth(), sq.getHeight()) * 0.46f;
        const float k = half / juce::jmax (0.02f, scale) * 0.70710678f;
        juce::Path trace;
        for (int i = 0; i < count; ++i)
        {
            const float l = pairs[(size_t) (2 * i)], r = pairs[(size_t) (2 * i + 1)];
            const juce::Point<float> pt (c.x + juce::jlimit (-half, half, (r - l) * k), c.y - juce::jlimit (-half, half, (l + r) * k));
            if (i == 0)
                trace.startNewSubPath (pt);
            else
                trace.lineTo (pt);
        }
        g.saveState();
        g.reduceClipRegion (sq.toNearestInt());
        g.setColour (juce::Colour (0xff40e050).withAlpha (0.8f));
        g.strokePath (trace, juce::PathStrokeType (1.0f));
        g.restoreState();
    }

private:
    void timerCallback() override
    {
        Analysis analysis (strip);
        if (! isShowing())
        {
            if (std::exchange (shown, false) && analysis.isAnalysed (strip))
                analysis.hideScope();
            return;
        }
        if (! shown || ! analysis.controls.scopeOn.load() || ! analysis.isAnalysed (strip))
            analysis.showScope (&strip);
        shown = true;
        count = analysis.readScope (pairs.data(), kPairs);
        float peak = 0.0f;
        for (int i = 0; i < count; ++i)
        {
            const float l = pairs[(size_t) (2 * i)], r = pairs[(size_t) (2 * i + 1)];
            peak = juce::jmax (peak, std::abs (l + r), std::abs (r - l));
        }
        scale = juce::jmax (peak * 0.70710678f, scale * 0.92f);
        repaint();
    }

    static constexpr int kPairs = 1024;
    Strip& strip;
    std::array<float, 2 * kPairs> pairs {};
    int count = 0;
    float scale = 0.02f;
    bool shown = false;
};

/** MAIN: MAIN SENDS, the pan/width graphic with the LR VECTOR SCOPE, and the
    channel's LEVEL (Fig. 65). Mains and matrices: the output slot with the
    post-fader delay. */
class MainTab final : public HomeScreen
{
public:
    MainTab (UiContext& c, Strip& s) : HomeScreen (c, s)
    {
        auto& p = strip.params;
        hasMainSends = strip.getKind() != StripKind::Main && strip.getKind() != StripKind::Matrix;
        if (hasMainSends)
            for (int i = 0; i < kNumMains && i < (int) ctx.engine.getMains().size(); ++i)
                sends.push_back (&make<MainSendColumn> (*ctx.engine.getMains()[(size_t) i], p.mains[i]));
        else
        {
            delay = &make<DelayPanel> (ctx, p.input);
            delay->onUnitChanged = [this] { notifyKnobs(); };
        }
        pan = &make<PanWidthDisplay> (p.pan, p.width, true);
        scope = &make<VectorScope> (strip);
        level = &make<LevelPanel> (ctx, strip);
    }

    std::vector<HomeKnob> knobs() override
    {
        auto& p = strip.params;
        std::vector<HomeKnob> k;
        if (hasMainSends)
        {
            for (int i = 0; i < kNumMains; ++i)
                k.push_back ({ wingFader ("MAIN " + juce::String (i + 1), p.mains[i].levelDb), levelStyle (i < (int) sends.size() ? sends[(size_t) i]->getFader() : nullptr) });
            k.push_back ({ wingWidth ("WIDTH", p.width), widthStyle (pan) });
        }
        else
        {
            k = { {}, {}, {}, { delayBinding (ctx, p.input), distStyle (delay->getSlider()) }, { wingFader ("FADER", p.faderDb), levelStyle (level->getFader()) } };
        }
        k.push_back ({ wingPan ("PAN", p.pan), thumbStyle (pan) });
        return k;
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        // D7 (WING pixels of the 1160 content): MAIN SENDS 0-296, the pan
        // graphic in the middle, the strip's header and LEVEL 944-1160.
        const float u = (float) b.getWidth() / 1160.0f;
        auto right = b.removeFromRight (juce::roundToInt (216.0f * u));
        auto left = b.removeFromLeft (juce::roundToInt (296.0f * u));
        b.removeFromRight (juce::roundToInt (8.0f * u));
        const int headerH = juce::jlimit (32, screenCap (60), getHeight() / 9);
        channelHeader = right.removeFromTop (headerH);
        right.removeFromTop (juce::jmax (4, getHeight() / 100));
        box (right, "LEVEL");
        level->setBounds (sectionBody (right, true));

        sendsHeader = {};
        if (hasMainSends)
        {
            // The header bar, then the columns on the black page.
            sendsHeader = left.removeFromTop (headerH);
            left.removeFromTop (juce::jmax (4, getHeight() / 100));
            const int w = left.getWidth() / juce::jmax (1, (int) sends.size());
            for (auto* col : sends)
                col->setBounds (left.removeFromLeft (w));
        }
        else
            delay->setBounds (box (left, strip.getKind() == StripKind::Main || strip.getKind() == StripKind::Matrix ? "DELAY (POST)" : "DELAY"));

        // The pan graphic on a dotted field, the vector scope at its top right.
        field = b;
        scopeArea = b.withTrimmedTop (juce::roundToInt (16.0f * u)).removeFromTop (b.getHeight() * 24 / 100)
                        .removeFromRight (juce::roundToInt (148.0f * u)).withTrimmedRight (juce::roundToInt (12.0f * u));
        pan->setBounds (b.withTrimmedTop (b.getHeight() / 10));
        scope->setBounds (scopeArea.expanded (2));
        scope->toFront (false);
    }

    void paint (juce::Graphics& g) override
    {
        HomeScreen::paint (g);
        // A grid of small grey dots behind the pan graphic.
        g.setColour (juce::Colour (0xff383838));
        const float dot = juce::jmax (1.0f, wingPx (2.0f));
        const float step = juce::jmax (6.0f, (float) getHeight() / 48.0f);
        for (float y = (float) field.getY() + step * 0.5f; y < (float) field.getBottom(); y += step)
            for (float x = (float) field.getX() + step * 0.5f; x < (float) field.getRight(); x += step)
                g.fillRect (x, y, dot, dot);

        for (auto [area, text] : { std::make_pair (channelHeader, strip.getLabel().replace ("CH ", "CHANNEL ").replace ("MTX ", "MATRIX ")),
                                   std::make_pair (sendsHeader, juce::String ("MAIN SENDS")) })
        {
            if (area.isEmpty())
                continue;
            g.setColour (colours::panel);
            g.fillRect (area);
            g.setColour (colours::caption);
            g.setFont (panelTitleFont (area.getHeight()));
            g.drawText (text, area, juce::Justification::centred);
        }

    }

private:
    bool hasMainSends = false;
    std::vector<MainSendColumn*> sends;
    DelayPanel* delay = nullptr;
    PanWidthDisplay* pan = nullptr;
    VectorScope* scope = nullptr;
    LevelPanel* level = nullptr;
    juce::Rectangle<int> channelHeader, sendsHeader, field, scopeArea;
};

//==============================================================================
/** A send's pan knob, drawn like the channel's: a black ring whose green arc,
    centred on the pan position, spans the stereo width, and a grey needle. */
void paintSendPan (juce::Graphics& g, juce::Rectangle<float> r, float pan, float width, bool enabled)
{
    const float size = juce::jmin (r.getWidth(), r.getHeight());
    const auto c = r.getCentre();
    const float rad = size * 0.46f, t = rad * 0.34f, mid = rad - t * 0.5f;
    juce::Path ring;
    ring.addCentredArc (c.x, c.y, mid, mid, 0.0f, 0.0f, juce::MathConstants<float>::twoPi, true);
    g.setColour (juce::Colours::black);
    g.strokePath (ring, juce::PathStrokeType (t));
    const float a = PanWidthDisplay::panAngle (pan), hs = PanWidthDisplay::halfSpan (width);
    if (hs > 0.01f)
    {
        juce::Path arc;
        arc.addCentredArc (c.x, c.y, mid, mid, 0.0f, a - hs, a + hs, true);
        g.setColour (enabled ? colours::panArc : colours::panArc.withMultipliedBrightness (0.4f));
        g.strokePath (arc, juce::PathStrokeType (t, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
    }
    g.setColour (enabled ? juce::Colour (0xff909090) : colours::dim);
    g.drawLine (c.x, c.y, c.x + std::sin (a) * rad, c.y - std::cos (a) * rad, juce::jmax (1.2f, size * 0.05f));
}

/** The pop-up of a send's pan knob: MODE, IGNORE CHANNEL MUTE (TAP mode
    only), PAN LINK (OFF, LINK to the channel pan or LINK REVERSED, mirrored, as
    FEED CONFIGURATION sets it) and the send pan. */
class SendOptions final : public juce::Component, private juce::Timer
{
public:
    /** A bus send: TAP (PRE) / POST / GROUP. */
    SendOptions (SendParams& s, bool inputSource, bool canIgnoreMute)
        : mode (juce::StringArray { sendModeName (0, inputSource), "POST", "GROUP" }, [&s] { return s.mode.load(); }, [&s] (int m) { s.mode = m; }),
          panBox (wingPan ("PAN", s.pan)), canIgnore (canIgnoreMute)
    {
        bindKey (ignoreMute, s.ignoreMute);
        link = makeLink (s.panLink, s.panReversed);
        isTap = [&s] { return s.mode.load() == 0; };
        isLinked = [&s] { return s.panLink.load(); };
        setUp();
    }

    /** A matrix send: TAP (PRE) / POST; PAN LINK for input and aux channels. */
    SendOptions (MatrixSendParams& m, bool inputSource, bool canIgnoreMute)
        : mode (juce::StringArray { sendModeName (0, inputSource), "POST" }, [&m] { return m.pre.load() ? 0 : 1; }, [&m] (int i) { m.pre = i == 0; }),
          panBox (wingPan ("PAN", m.pan)), canIgnore (canIgnoreMute)
    {
        bindKey (ignoreMute, m.ignoreMute);
        isTap = [&m] { return m.pre.load(); };
        if (inputSource)
        {
            link = makeLink (m.panLink, m.panReversed);
            isLinked = [&m] { return m.panLink.load(); };
        }
        else
        {
            isLinked = [] { return false; };
        }
        setUp();
    }

    static std::unique_ptr<Segmented> makeLink (std::atomic<bool>& linked, std::atomic<bool>& reversed)
    {
        auto keys = std::make_unique<Segmented> (juce::StringArray { "OFF", "LINK", "LINK\nREVERSED" },
                                                 [&] { return linked.load() ? (reversed.load() ? 2 : 1) : 0; },
                                                 [&] (int i)
                                                 {
                                                     reversed = i == 2;
                                                     linked = i > 0;
                                                 });
        keys->twoLines = true;
        return keys;
    }

    void setUp()
    {
        ignoreMute.twoLines = true;
        for (juce::Component* c : { (juce::Component*) &mode, (juce::Component*) &ignoreMute, (juce::Component*) &panBox })
            addAndMakeVisible (c);
        if (link != nullptr)
            addAndMakeVisible (*link);
        timerCallback();
        startTimerHz (6);
        setSize (300, link != nullptr ? 276 : 210);
    }

    void timerCallback() override
    {
        ignoreMute.setEnabled (canIgnore && isTap());
        panBox.setEnabled (! isLinked());
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (colours::panel);
        g.setColour (Colours::textDim);
        g.setFont (wingFont (TypeScale::caption));
        g.drawText ("MODE", getLocalBounds().reduced (10, 6).removeFromTop (14), juce::Justification::centredLeft);
        if (link != nullptr)
            g.drawText ("PAN LINK", link->getBounds().withY (link->getY() - 16).withHeight (14), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 6);
        r.removeFromTop (16);
        mode.setBounds (r.removeFromTop (32));
        r.removeFromTop (8);
        ignoreMute.setBounds (r.removeFromTop (46).withTrimmedRight (r.getWidth() / 2).reduced (2, 0));
        if (link != nullptr)
        {
            r.removeFromTop (24);
            link->setBounds (r.removeFromTop (42));
        }
        r.removeFromTop (8);
        panBox.setBounds (r.removeFromTop (44));
    }

private:
    Segmented mode;
    IconKey ignoreMute { "IGNORE CHANNEL MUTE", std::nullopt, colours::accent };
    std::unique_ptr<Segmented> link; // PAN LINK, where the send has it
    KnobBox panBox;
    bool canIgnore;
    std::function<bool()> isTap, isLinked;
};

/** One column of BUS SENDS / MATRIX SENDS (3.1): the target's label on its
    colour and its name; a box with TAP | POST (lit red / green; GROUP blue),
    the pan ring and PAN LINK; the send fader filled in the mode's colour; ON.
    A bus as the source shows BUS and a PRE key instead, and no pan. Sends that
    can't exist (the bus itself) are an empty column. */
class SendColumn final : public juce::Component, private juce::Timer
{
public:
    /** A bus send (`send`) or a matrix send (`mtx`). */
    SendColumn (Strip& source, Strip& target, SendParams* s, MatrixSendParams* m, bool possible, std::function<void()> select)
        : src (source), dst (target), send (s), mtx (m), enabled (possible), onSelect (std::move (select)),
          fader (s != nullptr ? s->levelDb : m->levelDb, false)
    {
        fader.fillColour = [this]
        {
            const auto c = src.isBus() && modeOf() != 0 ? colours::post : sendModeColour (modeOf());
            return isOnNow() ? c : c.withMultipliedBrightness (0.45f);
        };
        fader.isOn = [this] { return isOnNow(); };
        fader.wellColour = colours::panel;
        on.isOn = [this] { return isOnNow(); };
        on.onClick = [this]
        {
            if (send != nullptr)
                send->enabled = ! send->enabled.load();
            else
                mtx->on = ! mtx->on.load();
            if (onSelect)
                onSelect();
        };
        pre.isOn = [this] { return modeOf() == 0; };
        pre.onClick = [this]
        {
            if (send != nullptr)
                send->mode = send->mode.load() == 0 ? 1 : 0;
            if (onSelect)
                onSelect();
        };
        addChildComponent (fader);
        addChildComponent (on);
        addChildComponent (pre);
        fader.setVisible (enabled);
        on.setVisible (enabled);
        pre.setVisible (enabled && busSource());
        startTimerHz (8);
    }

    bool selected = false;
    /** A touch on a send that can't exist (WING 3.0: the feedback-loop warning). */
    std::function<void()> onImpossible;
    juce::Component* getFader() noexcept { return &fader; }

    bool busSource() const { return src.isBus() && send != nullptr; }
    int modeOf() const { return send != nullptr ? send->mode.load() : (mtx->pre.load() ? 0 : 1); }
    bool isOnNow() const { return send != nullptr ? send->enabled.load() : mtx->on.load(); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (juce::jmax (2.0f, getWidth() * 0.06f), 0.0f);
        auto label = r.removeFromTop (stripH);
        g.setColour (dst.getColour());
        g.fillRect (label);
        g.setColour (juce::Colours::black);
        g.setFont (homeFont (TypeScale::body, stripH * 0.82f));
        g.drawText (shortLabel (dst), label.toNearestInt(), juce::Justification::centred);
        auto name = r.removeFromTop (nameH);
        g.setColour (colours::caption);
        g.setFont (homeFont (TypeScale::body, nameH * 0.7f));
        g.drawFittedText (hasDefaultName (dst) ? juce::String() : stripName (dst), name.toNearestInt(), juce::Justification::centred, 1, 0.6f);

        // The mode and pan box.
        g.setColour (colours::panel);
        g.fillRect (boxArea);
        auto box = boxArea;
        auto modeRow = box.removeFromTop (juce::jlimit (12.0f, screenCap (20.0f), boxArea.getHeight() * 0.24f));
        modeArea = modeRow;
        // TAP | POST side by side: small enough for both words in the column.
        g.setFont (fontFittingWidth (homeFont (TypeScale::body, juce::jmin (modeRow.getHeight() * 0.85f, modeRow.getWidth() * 0.3f)), "POST",
                                     modeRow.getWidth() * 0.5f - 1.0f, 0.85f));
        if (busSource())
        {
            g.setColour (colours::caption);
            g.drawText ("BUS", modeRow.toNearestInt(), juce::Justification::centred);
            return;
        }
        if (! enabled)
            return;
        const int mode = modeOf();
        if (mode == 2)
        {
            g.setColour (colours::group);
            g.drawText ("GROUP", modeRow.toNearestInt(), juce::Justification::centred);
        }
        else
        {
            const bool inputSource = src.isInput();
            g.setColour (mode == 0 ? colours::tap : colours::dim);
            g.drawFittedText (sendModeName (0, inputSource || mtx != nullptr ? inputSource : false),
                              modeRow.withTrimmedRight (modeRow.getWidth() * 0.5f).toNearestInt(), juce::Justification::centred, 1, 0.7f);
            g.setColour (mode == 1 ? colours::post : colours::dim);
            g.drawFittedText ("POST", modeRow.withTrimmedLeft (modeRow.getWidth() * 0.5f).toNearestInt(), juce::Justification::centred, 1, 0.7f);
        }

        // The pan ring, PAN LINK under it.
        auto linkArea = box.removeFromBottom (juce::jlimit (10.0f, screenCap (18.0f), boxArea.getHeight() * 0.22f));
        panArea = box.reduced (2.0f);
        // Matrix sends of input and aux channels have PAN LINK too.
        const bool hasLink = send != nullptr || src.isInput();
        const bool reversed = send != nullptr ? send->panReversed.load() : mtx->panReversed.load();
        const bool linked = hasLink && (send != nullptr ? send->panLink.load() : mtx->panLink.load());
        const float chPan = reversed ? -src.params.pan.load() : src.params.pan.load();
        const float panValue = linked ? chPan : (send != nullptr ? send->pan.load() : mtx->pan.load());
        paintSendPan (g, panArea, panValue, linked ? src.params.width.load() : 100.0f, isOnNow());
        if (hasLink)
        {
            g.setColour (linked ? colours::link : colours::dim);
            const auto linkText = linked && reversed ? "LINK REV" : "PAN LINK";
            g.setFont (fontFittingWidth (homeFont (TypeScale::body, linkArea.getHeight() * 0.85f), linkText, linkArea.getWidth() - 2.0f, 0.85f));
            g.drawFittedText (linkText, linkArea.toNearestInt(), juce::Justification::centred, 1, 0.6f);
        }
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        if (selected && enabled)
        {
            g.setColour (colours::accent);
            g.fillRect (on.getBounds().toFloat().withY ((float) on.getBottom() + 2.0f).withHeight (3.0f));
        }
    }

    void resized() override
    {
        stripH = (float) juce::jlimit (11, screenCap (18), getHeight() / 33);
        nameH = (float) juce::jlimit (12, screenCap (22), getHeight() / 26);
        auto r = getLocalBounds().reduced (juce::jmax (2, getWidth() * 6 / 100), 0);
        r.removeFromTop ((int) (stripH + nameH) + 2);
        boxArea = r.removeFromTop (juce::jlimit (40, screenCap (96), getHeight() * 17 / 100)).toFloat();
        r.removeFromTop (6);
        if (busSource())
            pre.setBounds (boxArea.toNearestInt().withTrimmedTop (juce::jlimit (12, screenCap (20), (int) (boxArea.getHeight() * 0.24f))).reduced (3, 4));
        r.removeFromBottom (6);
        on.setBounds (r.removeFromBottom (juce::jlimit (18, screenCap (34), getHeight() / 11)));
        r.removeFromBottom (6);
        fader.setBounds (r);
        fader.setVisible (enabled && modeOf() != 2); // GROUP sends have no level
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! enabled)
        {
            if (onImpossible && getLocalBounds().contains (e.getPosition()))
                onImpossible();
            return;
        }
        if (onSelect)
            onSelect();
        if (busSource())
            return;
        if (panArea.contains (e.position))
        {
            auto options = send != nullptr ? std::make_unique<SendOptions> (*send, src.isInput(), src.canIgnoreMuteOnSends())
                                           : std::make_unique<SendOptions> (*mtx, src.isInput(), src.canIgnoreMuteOnSends());
            juce::CallOutBox::launchAsynchronously (std::move (options), localAreaToGlobal (panArea.toNearestInt()), nullptr);
        }
        else if (modeArea.contains (e.position))
        {
            if (send != nullptr)
                send->mode = (send->mode.load() + 1) % 3;
            else
                mtx->pre = ! mtx->pre.load();
            if (onModeChanged)
                onModeChanged (modeOf());
        }
    }

    std::function<void (int mode)> onModeChanged;

private:
    void timerCallback() override
    {
        const int mode = modeOf();
        if (mode != shownMode)
        {
            shownMode = mode;
            resized();
        }
        repaint();
    }

    Strip& src;
    Strip& dst;
    SendParams* send;
    MatrixSendParams* mtx;
    bool enabled;
    std::function<void()> onSelect;
    ScreenFader fader;
    IconKey on { "ON", std::nullopt, colours::accent }, pre { "PRE", std::nullopt, colours::accent };
    juce::Rectangle<float> boxArea, modeArea, panArea;
    float stripH = 14.0f, nameH = 14.0f;
    int shownMode = -1;
};

/** The TAP POINT pop-up of BUS SENDS (3.1): the channel's processing chain
    with a numbered node between the blocks,
    `-(1)-[FILT]-(2)-[GATE]-(3)-[EQ]-(4)-[COMP]-(5)-[INS 1]-(6)-[FADER]-(7)-[INS 2]-(8)-[WIDTH]-|`,
    the blocks in the channel's processing order, FADER and WIDTH framed green,
    the current node a red disc. Touch a node to choose it. Aux and buses tap
    at a fixed point: their chain is only shown. */
class TapChain final : public juce::Component, private juce::Timer
{
public:
    explicit TapChain (Strip& s) : strip (s) { startTimerHz (6); }

    std::function<void (int tap)> onChosen;

    struct Item
    {
        juce::String text;  // a block's name, or a node's caption
        int node = -1;      // the tap index of a node, -1 for a block
        bool green = false; // FADER, WIDTH
    };

    std::vector<Item> items() const
    {
        std::vector<Item> list;
        auto block = [&list] (const juce::String& t, bool green = false) { list.push_back ({ t, -1, green }); };
        auto node = [&list] (const juce::String& caption, int tap) { list.push_back ({ caption, tap, false }); };
        const auto captions = tapPointNames();
        if (strip.getKind() == StripKind::Channel)
        {
            node (captions[0], 0);
            block ("FILT");
            node (captions[1], 1);
            const juce::String order = models::list (models::ProcessOrders)[juce::jlimit (0, 23, (int) strip.params.processOrder.load())];
            for (int i = 0; i < order.length(); ++i)
            {
                const auto letter = order[i];
                block (letter == 'G' ? "GATE" : letter == 'E' ? "EQ" : letter == 'D' ? "COMP" : "INS 1");
                node (captions[2 + i], 2 + i);
            }
            block ("FADER", true);
            node (captions[6], 6);
            block ("INS 2");
            node (captions[7], 7);
            block ("WIDTH", true);
            return list;
        }
        // Aux (DYN, EQ, INS 1) and buses (COMP, INS 1, then EQ, INS 2 after
        // the fader): fixed after INS 1.
        if (strip.getKind() == StripKind::Aux)
            for (auto* b : { "DYN", "EQ", "INS 1" })
                block (b);
        else
            for (auto* b : { "COMP", "INS 1" })
                block (b);
        node (captions[ld::kTapPfl], ld::kTapPfl);
        block ("FADER", true);
        if (strip.isBus())
        {
            block ("EQ");
            block ("INS 2");
        }
        block ("WIDTH", true);
        return list;
    }

    int current() const { return strip.hasSelectableTap() ? juce::jlimit (0, 7, (int) strip.params.tap.load()) : ld::kTapPfl; }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colours::black);
        g.fillRect (r);
        g.setColour (juce::Colour (0xff282828));
        g.fillRect (r.reduced (1.5f));

        const auto list = items();
        auto chain = r.reduced (r.getWidth() * 0.02f, 0.0f);
        const float lineY = chain.getY() + chain.getHeight() * 0.36f;
        const float nodeD = juce::jlimit (12.0f, screenCap (26.0f), chain.getHeight() * 0.22f);
        const float blockH = nodeD * 1.25f;
        int nodes = 0, blocks = 0;
        for (auto& it : list)
            (it.node >= 0 ? nodes : blocks) += 1;
        // Blocks twice as wide as nodes, the rest spread evenly.
        const float unit = chain.getWidth() / ((float) blocks * 2.6f + (float) nodes * 1.6f + 1.0f);
        g.setColour (colours::ink);
        g.fillRect (chain.getX(), lineY - 1.0f, chain.getWidth(), 2.0f);
        g.fillEllipse (chain.getX() - 3.0f, lineY - 3.0f, 6.0f, 6.0f);
        g.fillRect (chain.getRight() - 2.0f, lineY - blockH * 0.35f, 2.0f, blockH * 0.7f);

        hits.clear();
        float x = chain.getX() + unit * 0.5f;
        const int tap = current();
        for (auto& it : list)
        {
            if (it.node < 0)
            {
                auto b = juce::Rectangle<float> (x + unit * 0.2f, lineY - blockH * 0.5f, unit * 2.2f, blockH);
                g.setColour (it.green ? juce::Colour (0xff206024) : juce::Colours::black);
                g.fillRect (b);
                g.setColour (colours::ink);
                g.drawRect (b, 1.5f);
                g.setFont (homeFont (TypeScale::body, blockH * 0.6f));
                g.drawFittedText (it.text, b.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.6f);
                x += unit * 2.6f;
                continue;
            }
            const auto c = juce::Point<float> (x + unit * 0.8f, lineY);
            const bool sel = it.node == tap;
            // The stem down to the caption.
            g.setColour (sel ? colours::ink : colours::dim);
            g.fillRect (c.x - 1.0f, lineY, 2.0f, chain.getHeight() * 0.22f);
            g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ c.x, lineY + chain.getHeight() * 0.22f }));
            auto disc = juce::Rectangle<float> (nodeD, nodeD).withCentre (c);
            g.setColour (sel ? colours::tap : juce::Colours::black);
            g.fillEllipse (disc);
            if (! sel)
            {
                g.setColour (colours::ink);
                g.drawEllipse (disc.reduced (0.75f), 1.5f);
            }
            g.setColour (colours::ink);
            g.setFont (homeFont (TypeScale::body, nodeD * 0.7f, WingWeight::SemiBold));
            g.drawText (juce::String (it.node + 1), disc.toNearestInt(), juce::Justification::centred);
            auto caption = juce::Rectangle<float> (unit * 2.6f, chain.getHeight() * 0.2f).withCentre ({ c.x, lineY + chain.getHeight() * 0.42f });
            g.setColour (sel ? colours::ink : colours::caption);
            g.setFont (homeFont (TypeScale::body, caption.getHeight() * 0.8f));
            g.drawFittedText (it.text, caption.toNearestInt(), juce::Justification::centred, 1, 0.6f);
            hits.push_back ({ disc.expanded (nodeD * 0.4f).getUnion (caption), it.node });
            x += unit * 1.6f;
        }
        if (strip.hasSelectableTap())
        {
            // The end of the chain: post-processing sends.
            g.setColour (colours::caption);
            g.setFont (homeFont (TypeScale::body, chain.getHeight() * 0.16f));
            g.drawText ("[POST]", juce::Rectangle<float> (unit * 2.0f, chain.getHeight() * 0.2f).withCentre ({ chain.getRight() - unit, lineY + chain.getHeight() * 0.42f }).toNearestInt(),
                        juce::Justification::centred);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! strip.hasSelectableTap())
            return;
        for (auto& [area, tap] : hits)
            if (area.contains (e.position))
            {
                if (onChosen)
                    onChosen (tap);
                return;
            }
    }

private:
    void timerCallback() override
    {
        const auto now = std::make_pair ((int) strip.params.tap.load(), (int) strip.params.processOrder.load());
        if (now != shown)
        {
            shown = now;
            repaint();
        }
    }

    Strip& strip;
    std::vector<std::pair<juce::Rectangle<float>, int>> hits;
    std::pair<int, int> shown { -1, -1 };
};

/** The TAP EQ's response (ld_strip_response block 4) at `freqs`. */
std::vector<double> tapEqResponse (const Strip& s, const std::vector<double>& freqs)
{
    std::vector<double> db (freqs.size(), 0.0);
    ld::ld_strip_response (s.getHandle(), 4, freqs.data(), db.data(), (int32_t) freqs.size(), 48000.0);
    return db;
}

/** The header's TAP EQ key (D5): the caption and the TAP EQ's curve (a flat
    line while it does nothing), lit while the TAP EQ is on. Click: on / off. */
class TapEqKey final : public juce::Component, private juce::Timer
{
public:
    explicit TapEqKey (Strip& s) : strip (s)
    {
        for (int i = 0; i < 48; ++i)
            freqs.push_back (20.0 * std::pow (1000.0, (double) i / 47.0));
        startTimerHz (8);
    }

    std::function<void()> onClick;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const bool enabled = isEnabled();
        const bool on = enabled && strip.params.tapEq.on.load();
        paintKeyBody (g, r, on ? colours::accent : colours::keyOff);
        auto text = r.removeFromLeft (r.getWidth() * 0.42f);
        g.setColour (on ? juce::Colours::black : (enabled ? colours::caption : colours::dim));
        g.setFont (homeFont (TypeScale::body, r.getHeight() * 0.46f));
        g.drawFittedText ("TAP EQ", text.toNearestInt().reduced (4, 0), juce::Justification::centredLeft, 1, 0.8f);
        // The curve, +-15 dB over the key's height.
        auto c = r.reduced (r.getHeight() * 0.18f, r.getHeight() * 0.22f);
        const auto db = tapEqResponse (strip, freqs);
        juce::Path p;
        for (size_t i = 0; i < db.size(); ++i)
        {
            const juce::Point<float> pt (c.getX() + c.getWidth() * (float) i / (float) (db.size() - 1),
                                         c.getCentreY() - juce::jlimit (-1.0f, 1.0f, (float) db[i] / 15.0f) * c.getHeight() * 0.5f);
            if (i == 0)
                p.startNewSubPath (pt);
            else
                p.lineTo (pt);
        }
        g.setColour (on ? juce::Colours::black : (enabled ? colours::caption : colours::dim));
        g.strokePath (p, juce::PathStrokeType (1.5f));
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (isEnabled() && getLocalBounds().contains (e.getPosition()) && onClick)
            onClick();
    }

private:
    void timerCallback() override
    {
        auto& eq = strip.params.tapEq;
        juce::String state;
        state << (int) eq.on.load();
        for (auto& b : eq.bands)
            state << ':' << b.gainDb.load() << ',' << b.freq.load() << ',' << b.q.load();
        if (state != shown)
        {
            shown = state;
            repaint();
        }
    }

    Strip& strip;
    std::vector<double> freqs;
    juce::String shown;
};

/** TAP EQ (the `>` key of the BUS SENDS header): the 3-band EQ on the signal
    the channel's TAP-mode sends take - a low shelf, a peak and a high shelf.
    The curve with a ring per band (drag: frequency and gain, wheel: Q), and
    a column per band with GAIN, FREQ and Q. Opens over the send columns. */
class TapEqPanel final : public juce::Component, private juce::Timer
{
public:
    explicit TapEqPanel (Strip& s) : strip (s)
    {
        auto& eq = strip.params.tapEq;
        bindKey (on, eq.on);
        addAndMakeVisible (on);
        close.offInk = colours::caption;
        close.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (close);
        static const char* names[] = { "LOW SHELF", "PEAK", "HIGH SHELF" };
        for (int i = 0; i < ld::kTapEqBands; ++i)
        {
            auto& b = eq.bands[i];
            auto& k = *bandKeys.add (new IconKey (names[i], std::nullopt, colours::choice));
            k.isOn = [this, i] { return selected == i; };
            k.onClick = [this, i] { select (i); };
            addAndMakeVisible (k);
            // FREQ without its unit, so the boxes stay narrow.
            auto freq = wingFreq ("FREQ", b.freq, 20.0f, 20000.0f, kDefaultFreqs[i]);
            freq.text = [&f = b.freq] { return wingHz (f.load(), false); };
            for (auto binding : { bindLinear ("GAIN", b.gainDb, -15.0f, 15.0f, " dB", 1, 0.0f), freq,
                                  bindLog ("Q", b.q, 0.442f, 10.0f, {}, kDefaultQ) })
                addAndMakeVisible (boxes.add (new KnobBox (binding)));
        }
        for (int i = 0; i < 200; ++i)
            freqs.push_back (20.0 * std::pow (1000.0, (double) i / 199.0));
        startTimerHz (15);
    }

    std::function<void()> onClose;
    /** The band the knobs under the display edit changed. */
    std::function<void()> onSelect;
    int selected = 1;

    void select (int band)
    {
        selected = juce::jlimit (0, ld::kTapEqBands - 1, band);
        repaint();
        if (onSelect)
            onSelect();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (4);
        bar = r.removeFromTop (juce::jlimit (26, screenCap (44), getHeight() / 9));
        auto b = bar;
        close.setBounds (b.removeFromRight (b.getHeight() * 3).reduced (0, b.getHeight() / 10));
        b.removeFromRight (8);
        on.setBounds (b.removeFromRight (b.getHeight() * 2).reduced (0, b.getHeight() / 10));
        r.removeFromTop (6);
        auto side = r.removeFromRight (r.getWidth() * 46 / 100);
        r.removeFromRight (8);
        graph = r.toFloat();
        const int rowH = side.getHeight() / ld::kTapEqBands;
        for (int i = 0; i < ld::kTapEqBands; ++i)
        {
            auto row = side.removeFromTop (rowH).reduced (0, 3);
            bandKeys[i]->setBounds (row.removeFromTop (juce::jmin (screenCap (32), row.getHeight() / 3)));
            row.removeFromTop (4);
            const int w = (row.getWidth() - 8) / 3;
            for (int j = 0; j < 3; ++j)
            {
                boxes[i * 3 + j]->setBounds (row.removeFromLeft (w).withHeight (juce::jmin (row.getHeight(), screenCap (38))));
                row.removeFromLeft (4);
            }
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black);
        g.setColour (colours::panel);
        g.fillRect (bar);
        g.setColour (colours::caption);
        g.setFont (panelTitleFont (bar.getHeight()));
        g.drawText ("TAP EQ", bar.withTrimmedLeft (12), juce::Justification::centredLeft);

        // The graph as on the EQ screen: the piano ruler, a log grid, the
        // curve over its dark amber fill, a ring per band.
        auto area = graph;
        const float rulerH = juce::jlimit (5.0f, screenCap (12.0f), area.getHeight() * 0.04f);
        auto ruler = area.removeFromTop (rulerH);
        area.removeFromTop (16.0f);
        auto labels = area.removeFromBottom (14.0f);
        plot = area;
        auto xOf = [this] (float hz) { return xForFreq (hz); };
        ResponseGraph::paintPianoRuler (g, ruler, xOf);
        g.setColour (juce::Colour (0xff383838));
        for (float decade : { 10.0f, 100.0f, 1000.0f, 10000.0f })
            for (int m = 1; m <= 9; ++m)
                if (const float f = decade * (float) m; f >= 20.0f && f <= 20000.0f)
                    g.drawVerticalLine (juce::roundToInt (xForFreq (f)), plot.getY(), plot.getBottom());
        for (int i = 0; i <= 4; ++i)
            g.drawHorizontalLine (juce::roundToInt (plot.getY() + (plot.getHeight() - 1.0f) * (float) i / 4.0f), plot.getX(), plot.getRight());
        g.setColour (colours::caption);
        g.setFont (wingFont (TypeScale::caption));
        for (float f : { 20.0f, 100.0f, 1000.0f, 10000.0f, 20000.0f })
            g.drawText (f >= 1000.0f ? juce::String (juce::roundToInt (f / 1000.0f)) + "K" : juce::String (juce::roundToInt (f)),
                        juce::Rectangle<float> (xForFreq (f) - 18.0f, labels.getY(), 36.0f, labels.getHeight()).toNearestInt(),
                        juce::Justification::centred);

        const bool active = strip.params.tapEq.on.load();
        const auto db = tapEqResponse (strip, freqs);
        juce::Path curve;
        for (size_t i = 0; i < db.size(); ++i)
        {
            const juce::Point<float> pt (xForFreq ((float) freqs[i]), yForDb ((float) db[i]));
            if (i == 0)
                curve.startNewSubPath (pt);
            else
                curve.lineTo (pt);
        }
        g.saveState();
        g.reduceClipRegion (plot.toNearestInt());
        juce::Path fill (curve); // shaded against the 0 dB line
        fill.lineTo (plot.getRight() + 2.0f, yForDb (0.0f));
        fill.lineTo (plot.getX() - 2.0f, yForDb (0.0f));
        fill.closeSubPath();
        g.setColour (active ? colours::eqFill : juce::Colour (0xff1c1c1c));
        g.fillPath (fill);
        g.setColour (active ? colours::eqCurve : colours::dim);
        g.strokePath (curve, juce::PathStrokeType (2.0f));
        g.restoreState();

        static const char* tags[] = { "L", "P", "H" };
        for (int i = 0; i < ld::kTapEqBands; ++i)
        {
            const auto c = handle (i);
            const bool sel = i == selected;
            g.setColour (sel ? Colours::choice : juce::Colour (0xff585858));
            g.fillRect (c.x - 0.75f, plot.getY() + 2.0f, sel ? 2.0f : 1.5f, plot.getHeight() - 2.0f);
            g.setColour (sel ? colours::ink : colours::dim);
            g.setFont (wingFont (TypeScale::caption));
            g.drawText (tags[i], juce::Rectangle<float> (24.0f, 14.0f).withCentre ({ c.x, plot.getY() - 8.0f }).toNearestInt(), juce::Justification::centred);
            auto ring = juce::Rectangle<float> (16.0f, 16.0f).withCentre (c);
            if (sel)
            {
                g.setColour (juce::Colours::black);
                g.fillEllipse (ring);
            }
            g.setColour (colours::ink);
            g.drawEllipse (ring, 2.0f);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = -1;
        float best = 16.0f;
        for (int i = 0; i < ld::kTapEqBands; ++i)
            if (const float d = handle (i).getDistanceFrom (e.position); d < best)
            {
                best = d;
                dragging = i;
            }
        if (dragging >= 0)
            select (dragging);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0)
            return;
        auto& b = strip.params.tapEq.bands[dragging];
        b.freq = juce::jlimit (20.0f, 20000.0f, freqForX (e.position.x));
        b.gainDb = juce::jlimit (-15.0f, 15.0f, std::round ((plot.getCentreY() - e.position.y) / (plot.getHeight() * 0.5f) * kRangeDb * 10.0f) / 10.0f);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = -1; }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        auto& q = strip.params.tapEq.bands[selected].q;
        q = juce::jlimit (0.442f, 10.0f, q.load() * std::pow (1.15f, w.deltaY > 0 ? 1.0f : -1.0f));
        repaint();
    }

private:
    static constexpr float kRangeDb = 18.0f;
    // The engine's defaults (100 Hz, 1 kHz, 10 kHz at Q 1.00 on the console's steps).
    static constexpr float kDefaultFreqs[ld::kTapEqBands] = { 99.68543f, 999.2505f, 10016.53f };
    static constexpr float kDefaultQ = 0.9979702f;

    float xForFreq (float f) const { return plot.getX() + plot.getWidth() * std::log (juce::jmax (20.0f, f) / 20.0f) / std::log (1000.0f); }
    float freqForX (float x) const { return 20.0f * std::pow (1000.0f, juce::jlimit (0.0f, 1.0f, (x - plot.getX()) / plot.getWidth())); }
    float yForDb (float db) const { return plot.getCentreY() - juce::jlimit (-kRangeDb, kRangeDb, db) / kRangeDb * plot.getHeight() * 0.5f; }
    juce::Point<float> handle (int band) const
    {
        auto& b = strip.params.tapEq.bands[band];
        return { xForFreq (b.freq.load()), yForDb (b.gainDb.load()) };
    }

    void timerCallback() override
    {
        auto& eq = strip.params.tapEq;
        juce::String state;
        state << (int) eq.on.load() << selected;
        for (auto& b : eq.bands)
            state << ':' << b.gainDb.load() << ',' << b.freq.load() << ',' << b.q.load();
        if (state != shown)
        {
            shown = state;
            repaint();
        }
    }

    Strip& strip;
    IconKey on { "ON", std::nullopt, colours::accent }, close { "CLOSE", Icon::Check };
    juce::OwnedArray<IconKey> bandKeys;
    juce::OwnedArray<KnobBox> boxes;
    std::vector<double> freqs;
    juce::Rectangle<int> bar;
    juce::Rectangle<float> graph, plot;
    juce::String shown;
    int dragging = -1;
};

/** BUS SENDS / MATRIX SENDS (Figs. 66-67): the TAP POINT diagram, TAP WIDTH and
    TAP EQ in the header; a column per bus (16) or matrix (8). */
class SendsTab final : public HomeScreen
{
public:
    SendsTab (UiContext& c, Strip& s, bool matrixPage) : HomeScreen (c, s), matrix (matrixPage || s.getKind() == StripKind::Main)
    {
        // TAP WIDTH and TAP EQ shape what the channel's TAP-mode sends take
        // (input channels: /ch/n/tapwid, /ch/n/peq).
        const bool tapShaping = strip.getKind() == StripKind::Channel;
        tapWidth = &make<KnobBox> (wingWidth ("TAP WIDTH", strip.params.tapWidth));
        tapWidth->centredRange = true;
        tapWidth->ringColour = colours::panArc;
        tapWidth->setEnabled (tapShaping);
        tapEq = &make<TapEqKey> (strip);
        tapEq->setEnabled (tapShaping);
        tapEq->onClick = [this] { strip.params.tapEq.on = ! strip.params.tapEq.on.load(); };
        tapEqMore = &key ({}, Icon::ChevronRight);
        tapEqMore->setEnabled (tapShaping);
        tapEqMore->isOn = [this] { return tapEqPanel != nullptr && tapEqPanel->isVisible(); };
        tapEqMore->onClick = [this] { showTapEq (tapEqPanel == nullptr || ! tapEqPanel->isVisible()); };
        // SPILL puts the strip's sends on the left fader section (SELECTED CH SENDS); again ends it.
        spill = &key ("SPILL", Icon::Spill);
        spill->isOn = [this] { return isSpilling (ctx, { LayerSet::Spill::Kind::Sends, 0, state::StripRef::of (strip) }); };
        spill->onClick = [this] { toggleSpill (ctx, std::nullopt, { LayerSet::Spill::Kind::Sends, 0, state::StripRef::of (strip) }); };
        if (strip.getKind() != StripKind::Main && strip.getKind() != StripKind::Matrix)
        {
            tapMenu = &key ({}, Icon::ChevronDown);
            tapMenu->onClick = [this] { toggleChain(); };
            chain = &make<TapChain> (strip);
            chain->setVisible (false);
            chain->onChosen = [this] (int tap)
            {
                const int before = strip.params.tap.load();
                strip.params.tap = tap;
                chain->setVisible (false);
                repaint();
                // 1.12: "Additional info message when using post fader send tap".
                if (tap != before && (tap == ld::kTapAfl || tap == ld::kTapPost))
                    popups::warn (*this, "POST FADER TAP",
                                  "The TAP sends of this channel now follow its fader" + juce::String (tap == ld::kTapPost ? ", INS 2 and width." : ".")
                                      + " Choose a tap before the fader for monitor mixes that should not follow it.");
            };
        }
        build();
    }

    bool busToBus() const { return strip.isBus() && ! matrix; }

    void toggleChain()
    {
        if (chain == nullptr)
            return;
        chain->setVisible (! chain->isVisible());
        chain->toFront (false);
    }

    void build()
    {
        for (auto* col : columns)
            owned.removeObject (col);
        columns.clear();
        if (matrix)
        {
            for (int i = 0; i < kNumMatrices && i < (int) ctx.engine.getMatrices().size(); ++i)
                columns.push_back (&make<SendColumn> (strip, *ctx.engine.getMatrices()[(size_t) i], nullptr, &strip.params.matrix[i], true,
                                                      [this, i] { select (i); }));
        }
        else
        {
            const auto& buses = ctx.engine.getBuses();
            for (int i = 0; i < (int) buses.size() && i < 16; ++i)
            {
                auto& bus = *buses[(size_t) i];
                // A bus feeds every other bus (manual 5.10); its own column is shown empty.
                const bool possible = &bus != &strip && ! (strip.isBus() && ! busFeedsBus (strip.getId(), bus.getId()));
                auto* send = &strip.sends[(size_t) bus.getId()];
                auto* col = &make<SendColumn> (strip, bus, send, nullptr, possible, [this, i] { select (i); });
                const bool self = &bus == &strip;
                col->onImpossible = [this, self, name = shortLabel (bus)]
                {
                    // WING 3.0: "Warning when a bus is sent to the same bus (feedback loop)".
                    popups::warn (*this, "FEEDBACK LOOP",
                                  self ? "A bus can't send to itself: the send would feed its own input."
                                       : "A send from " + shortLabel (strip) + " to " + name + " isn't possible.");
                };
                col->onModeChanged = [this] (int mode)
                {
                    juce::ignoreUnused (mode);
                    repaint();
                };
                columns.push_back (col);
            }
        }
        selected = juce::jlimit (0, juce::jmax (0, (int) columns.size() - 1), selected);
        for (int i = 0; i < (int) columns.size(); ++i)
            columns[(size_t) i]->selected = i == selected;
        if (chain != nullptr)
            chain->toFront (false);
        if (tapEqPanel != nullptr && tapEqPanel->isVisible())
            tapEqPanel->toFront (false);
        resized();
        repaint();
    }

    void select (int i)
    {
        if (i == selected)
            return;
        selected = i;
        for (int c = 0; c < (int) columns.size(); ++c)
            columns[(size_t) c]->selected = c == selected;
        repaint();
        notifyKnobs();
    }

    void showTapEq (bool show)
    {
        if (show && tapEqPanel == nullptr)
        {
            tapEqPanel = &make<TapEqPanel> (strip);
            tapEqPanel->onClose = [this] { showTapEq (false); };
            tapEqPanel->onSelect = [this] { notifyKnobs(); };
        }
        if (tapEqPanel != nullptr)
        {
            tapEqPanel->setVisible (show);
            if (show)
                tapEqPanel->toFront (false);
        }
        resized();
        notifyKnobs();
    }

    std::vector<HomeKnob> knobs() override
    {
        std::vector<HomeKnob> k;
        if (tapEqPanel != nullptr && tapEqPanel->isVisible())
        {
            // TAP EQ: the band, then GAIN (white), Q (green), FREQ (blue) as on
            // the EQ screen, and TAP WIDTH.
            auto& b = strip.params.tapEq.bands[tapEqPanel->selected];
            KnobBinding band;
            band.label = "BAND";
            band.get = [this] { return (float) tapEqPanel->selected / (float) (ld::kTapEqBands - 1); };
            band.set = [this] (float p)
            {
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<SendsTab> (this), i = juce::roundToInt (p * (float) (ld::kTapEqBands - 1))]
                                                 { if (safe != nullptr && safe->tapEqPanel != nullptr && safe->tapEqPanel->selected != i) safe->tapEqPanel->select (i); });
            };
            band.text = [this] { static const char* n[] = { "LO SHV", "PEAK", "HI SHV" }; return juce::String (n[tapEqPanel->selected]); };
            band.steps = (float) ld::kTapEqBands;
            k.push_back ({ band, styleOf (KnobStyle::Bar::Segments, colours::accent, tapEqPanel, ld::kTapEqBands) });
            k.push_back ({});
            k.push_back ({ bindLinear ("GAIN", b.gainDb, -15.0f, 15.0f, " dB", 1, 0.0f), thumbStyle (tapEqPanel) });
            k.push_back ({ bindLog ("Q", b.q, 0.442f, 10.0f, {}, 1.0f), qStyle (tapEqPanel) });
            k.push_back ({ wingFreq ("FREQ", b.freq, 20.0f, 20000.0f, 1000.0f), styleOf (KnobStyle::Bar::Thumb, KnobColours::blue, tapEqPanel) });
            k.push_back ({ wingWidth ("TAP WIDTH", strip.params.tapWidth), widthStyle (tapWidth) });
            return k;
        }
        const int n = (int) columns.size();
        if (n == 0)
            return k;
        // BUS n / MATRIX n: which send the SEND LVL and PAN knobs change.
        KnobBinding which;
        which.label = matrix ? "MATRIX" : "BUS";
        which.get = [this, n] { return n > 1 ? (float) selected / (float) (n - 1) : 0.0f; };
        which.set = [this, n] (float p) { juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<SendsTab> (this), i = juce::roundToInt (p * (float) (n - 1))] { if (safe) safe->select (i); }); };
        which.text = [this] { return juce::String (selected + 1); };
        which.steps = (float) n;
        k.push_back ({ which, styleOf (KnobStyle::Bar::Segments, colours::accent, columns[(size_t) selected], n) });

        KnobBinding page;
        page.label = "PAGE";
        const bool mainStrip = strip.getKind() == StripKind::Main;
        page.get = [this] { return matrix ? 1.0f : 0.0f; };
        page.set = [this, mainStrip] (float p)
        {
            const bool m = mainStrip || p > 0.5f;
            if (m != matrix)
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<SendsTab> (this), m] { if (safe) safe->setPage (m); });
        };
        page.text = [this] { return juce::String (matrix ? "MATRIX" : "BUS"); };
        page.steps = 2.0f;
        k.push_back ({ page, styleOf (KnobStyle::Bar::Thumb, KnobColours::blue) });
        k.push_back ({});
        k.push_back ({});

        auto* col = columns[(size_t) selected];
        if (matrix)
        {
            auto& m = strip.params.matrix[selected];
            k.push_back ({ wingFader ("SEND LVL", m.levelDb), levelStyle (col->getFader()) });
            if (! (strip.isInput() && m.panLink.load()))
                k.push_back ({ wingPan ("PAN", m.pan), thumbStyle (col) });
        }
        else
        {
            auto& bus = *ctx.engine.getBuses()[(size_t) selected];
            auto& send = strip.sends[(size_t) bus.getId()];
            if (send.mode.load() != 2)
                k.push_back ({ wingFader ("SEND LVL", send.levelDb), levelStyle (col->getFader()) });
            else
                k.push_back ({});
            if (! send.panLink.load())
                k.push_back ({ wingPan ("PAN", send.pan), thumbStyle (col) });
        }
        return k;
    }

    void setPage (bool m)
    {
        matrix = m;
        build();
        notifyKnobs();
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        // The 3.1 header (WING pixels from the content's left edge, 1160
        // wide): TAP POINT 8-376, the title to 592, TAP WIDTH 592-764, TAP EQ
        // 780-1008, SPILL 1024-1156.
        header = b.removeFromTop (juce::jlimit (32, screenCap (60), b.getHeight() / 9));
        auto h = header.reduced (0, juce::jmax (3, header.getHeight() / 9));
        const float u = (float) header.getWidth() / 1160.0f;
        auto span = [&h, u, this] (float x0, float x1)
        { return juce::Rectangle<int> (header.getX() + juce::roundToInt (x0 * u), h.getY(), juce::roundToInt ((x1 - x0) * u), h.getHeight()); };
        const bool tapped = tapMenu != nullptr && ! busToBus();
        tapField = tapped ? span (8.0f, 376.0f) : juce::Rectangle<int>();
        if (tapMenu != nullptr)
        {
            tapMenu->setVisible (tapped);
            tapMenu->setBounds (tapField.withLeft (tapField.getRight() - tapField.getHeight() * 2 / 3));
        }
        titleArea = busToBus() ? span (8.0f, 1008.0f) : span (376.0f, 592.0f);
        tapWidth->setVisible (! busToBus());
        tapEq->setVisible (! busToBus());
        tapEqMore->setVisible (! busToBus());
        tapWidth->setBounds (span (592.0f, 764.0f));
        auto eq = span (780.0f, 1008.0f);
        tapEqMore->setBounds (eq.removeFromRight (eq.getHeight() * 2 / 3));
        tapEq->setBounds (eq);
        spill->setBounds (span (1024.0f, 1156.0f));
        b.removeFromTop (juce::jmax (4, getHeight() / 100));

        if (chain != nullptr)
            chain->setBounds (b.removeFromTop (0).withHeight (juce::jlimit (70, screenCap (150), getHeight() / 5)));

        if (tapEqPanel != nullptr)
            tapEqPanel->setBounds (b);

        // 16 columns; the matrix page right-aligns its 8.
        const int slots = matrix ? 16 : juce::jmax (16, (int) columns.size());
        const int w = b.getWidth() / slots;
        if (matrix)
            b.removeFromLeft (b.getWidth() - w * (int) columns.size());
        for (auto* col : columns)
            col->setBounds (b.removeFromLeft (w));
    }

    void paint (juce::Graphics& g) override
    {
        HomeScreen::paint (g);
        auto h = header.toFloat();
        g.setColour (colours::panel);
        g.fillRect (h);
        g.setColour (colours::caption);
        g.setFont (panelTitleFont (header.getHeight()));
        g.drawFittedText (matrix ? "MATRIX SENDS" : (busToBus() ? "BUS -> BUS SENDS" : "BUS SENDS"), titleArea, juce::Justification::centred, 1, 0.7f);
        if (! tapField.isEmpty())
            paintTapDiagram (g, tapField.toFloat().withTrimmedRight ((float) tapMenu->getWidth()));
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (tapField.contains (e.getPosition()))
            toggleChain();
        else if (chain != nullptr && chain->isVisible())
            chain->setVisible (false);
    }

private:
    /** TAP POINT -[EQ]-(4)-[COMP]-: the blocks around the tap point and its
        number, in a dark field (its key opens the whole chain). */
    void paintTapDiagram (juce::Graphics& g, juce::Rectangle<float> r)
    {
        paintKeyBody (g, r, colours::well);
        r = r.reduced (r.getHeight() * 0.12f, 0.0f);
        g.setColour (colours::caption);
        g.setFont (homeFont (TypeScale::body, r.getHeight() * 0.36f));
        auto caption = r.removeFromLeft (r.getWidth() * 0.34f);
        g.drawFittedText ("TAP POINT", caption.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);

        const auto items = chain->items();
        const int tap = chain->current();
        juce::String before, after;
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].node == tap)
            {
                before = i > 0 ? items[i - 1].text : juce::String ("IN");
                after = i + 1 < items.size() ? items[i + 1].text : juce::String ("OUT");
            }
        const float d = r.getHeight() * 0.42f;
        const float bw = (r.getWidth() - d) * 0.5f - r.getHeight() * 0.3f;
        g.setColour (colours::ink);
        g.fillRect (r.getX(), r.getCentreY() - 0.75f, r.getWidth(), 1.5f);
        auto b1 = juce::Rectangle<float> (r.getX() + r.getHeight() * 0.15f, r.getY(), bw, r.getHeight()).reduced (0.0f, r.getHeight() * 0.22f);
        auto circle = juce::Rectangle<float> (d, d).withCentre ({ r.getCentreX(), r.getCentreY() });
        auto b2 = b1.withX (r.getRight() - r.getHeight() * 0.15f - bw);
        for (auto [box, text] : { std::make_pair (b1, before), std::make_pair (b2, after) })
        {
            g.setColour (juce::Colours::black);
            g.fillRect (box);
            g.setColour (colours::ink);
            g.drawRect (box, 1.5f);
            g.setFont (homeFont (TypeScale::body, box.getHeight() * 0.62f));
            g.drawFittedText (text, box.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.6f);
        }
        g.setColour (colours::tap);
        g.fillEllipse (circle);
        g.setColour (colours::ink);
        g.setFont (homeFont (TypeScale::body, circle.getHeight() * 0.72f, WingWeight::SemiBold));
        g.drawText (juce::String (tap + 1), circle.toNearestInt(), juce::Justification::centred);
    }

    bool matrix;
    std::vector<SendColumn*> columns;
    KnobBox* tapWidth = nullptr;
    TapEqKey* tapEq = nullptr;
    TapEqPanel* tapEqPanel = nullptr;
    IconKey *tapEqMore = nullptr, *tapMenu = nullptr, *spill = nullptr;
    TapChain* chain = nullptr;
    juce::Rectangle<int> header, tapField, titleArea;
    int selected = 0;
};

//==============================================================================
// CHANNEL INIT / CHANNEL COPY (Figs. 22-23).

enum ScopeBit
{
    Conn,
    Config,
    Cust,
    Tags,
    In,
    FilterBit,
    GateBit,
    DynBit,
    EqBit,
    DelayBit,
    Pre,
    Post,
    Send,
    MainBit,
    Fdr,
    MuteBit,
    PanBit,
    kNumScopeBits
};

const char* scopeName (int bit)
{
    static const char* names[] = { "CONN", "CONFIG", "CUST", "TAGS", "IN", "FILTER", "GATE", "DYN", "EQ",
                                   "DELAY", "PRE", "POST", "SEND", "MAIN", "FDR", "MUTE", "PAN" };
    return names[juce::jlimit (0, kNumScopeBits - 1, bit)];
}

/** The scope preselected when INIT/COPY opens from a slot. */
juce::uint32 scopeFor (HomeTab from)
{
    auto bit = [] (int b) { return 1u << b; };
    switch (from)
    {
        case HomeTab::Input:      return bit (In) | bit (DelayBit);
        case HomeTab::Filter:     return bit (FilterBit);
        case HomeTab::Gate:       return bit (GateBit);
        case HomeTab::Eq:         return bit (EqBit);
        case HomeTab::Dynamics:   return bit (DynBit);
        case HomeTab::PreInsert:  return bit (Pre);
        case HomeTab::PostInsert: return bit (Post);
        case HomeTab::Main:       return bit (MainBit);
        case HomeTab::Sends:      return bit (Send);
        case HomeTab::Home:
        case HomeTab::Icon:
        case HomeTab::Tags:
        case HomeTab::Name:
            break;
    }
    return ((1u << kNumScopeBits) - 1u) & ~bit (Conn); // Channel Home: all but CONN
}

void copyDyn (const Strip& src, const Strip& dst, ModelBlock block)
{
    auto& s = block == ModelBlock::Gate ? src.params.gate : src.params.dynamics;
    auto& d = block == ModelBlock::Gate ? dst.params.gate : dst.params.dynamics;
    const int model = s.model.load();
    if (dst.getKind() == StripKind::Aux && block == ModelBlock::Dynamics
        && ! models::list (models::AuxDynModels).contains (models::model (models::Dynamics, model).token))
        return; // aux channels offer fewer dynamics models
    dst.setModel (block, model);
    for (int i = 0; i < ld::kMaxDynParams; ++i)
        d.values[i] = s.values[i].load();
    d.on = s.on.load();
    d.mix = s.mix.load();
    d.gain = s.gain.load();
    d.scFilter = s.scFilter.load();
    d.scFreq = s.scFreq.load();
    d.scQ = s.scQ.load();
    d.scSource = s.scSource.load();
    d.scTap = s.scTap.load();
    d.xoKind = s.xoKind.load();
    d.xoFreq = s.xoFreq.load();
    d.xoQ = s.xoQ.load();
    d.xoDepth = s.xoDepth.load();
}

void copyInsert (const InsertParams& s, InsertParams& d, bool automixAllowed)
{
    d.on = s.on.load();
    d.fx = s.fx.load();
    d.mode = automixAllowed ? s.mode.load() : 0;
    d.weight = s.weight.load();
}

/** Copies the parts of `src` in `scope` to `dst` ("only the common parameters
    are copied" between different channel types). */
void copyStripParams (MixerEngine& engine, const Strip& src, const Strip& dst, juce::uint32 scope)
{
    if (&src == &dst)
        return;
    auto has = [scope] (int bit) { return (scope >> bit) & 1u; };
    auto& s = src.params;
    auto& d = dst.params;
    const bool bothChannels = src.getKind() == StripKind::Channel && dst.getKind() == StripKind::Channel;

    if (has (Conn) && src.isInput() && dst.isInput())
    {
        // Both inputs and the MAIN / ALT switch (as a scene's input connection).
        d.inputLeft = s.inputLeft.load();
        d.inputRight = s.inputRight.load();
        d.altInputLeft = s.altInputLeft.load();
        d.altInputRight = s.altInputRight.load();
        d.altSelected = s.altSelected.load();
        d.srcAuto = s.srcAuto.load();
    }
    if (has (Config))
    {
        if (src.isInput() && dst.isInput())
            d.stereo = s.stereo.load();
        d.mono = s.mono.load();
        if (bothChannels)
        {
            d.processOrder = s.processOrder.load();
            d.tap = s.tap.load();
        }
        d.monitorBus = s.monitorBus.load();
    }
    if (has (Cust))
    {
        dst.setName (src.getOwnName());
        d.colour = s.colour.load();
        d.icon = s.icon.load();
        d.led = s.led.load();
        if (src.isInput() && dst.isInput())
            d.custLink = s.custLink.load();
    }
    if (has (Tags))
    {
        d.dcaMask = s.dcaMask.load();
        d.muteGroupMask = s.muteGroupMask.load();
        setStripTags (dst, stripTags (src));
    }
    if (has (In))
    {
        d.input.trimDb = s.input.trimDb.load();
        d.input.balanceDb = s.input.balanceDb.load();
        d.input.invert = s.input.invert.load();
    }
    if (has (FilterBit) && bothChannels)
    {
        auto& sf = s.filter;
        auto& df = d.filter;
        dst.setModel (ModelBlock::FilterTool, sf.toolModel.load());
        df.lowCut = sf.lowCut.load();
        df.highCut = sf.highCut.load();
        df.tool = sf.tool.load();
        df.lowCutFreq = sf.lowCutFreq.load();
        df.lowCutSlope = sf.lowCutSlope.load();
        df.highCutFreq = sf.highCutFreq.load();
        df.highCutSlope = sf.highCutSlope.load();
        for (int i = 0; i < ld::kMaxToolParams; ++i)
            df.toolParams[i] = sf.toolParams[i].load();
    }
    if (has (GateBit) && bothChannels)
        copyDyn (src, dst, ModelBlock::Gate);
    if (has (DynBit))
        copyDyn (src, dst, ModelBlock::Dynamics);
    // (Aux channels don't offer the MACH EQ4.)
    if (has (EqBit) && src.getEqTable() == dst.getEqTable()
        && ! (dst.getKind() == StripKind::Aux && models::model (models::ChannelEq, s.eq.model.load()).token == "MACH4"))
    {
        dst.setModel (ModelBlock::Eq, s.eq.model.load());
        for (int i = 0; i < ld::kMaxEqParams; ++i)
            d.eq.values[i] = s.eq.values[i].load();
        d.eq.on = s.eq.on.load();
        d.eq.mix = s.eq.mix.load();
    }
    if (has (DelayBit))
    {
        d.input.delayOn = s.input.delayOn.load();
        d.input.delayMode = s.input.delayMode.load();
        d.input.delay = s.input.delay.load();
    }
    if (has (Pre))
        copyInsert (s.preInsert, d.preInsert, false);
    if (has (Post) && dst.getKind() != StripKind::Aux && src.getKind() != StripKind::Aux)
        copyInsert (s.postInsert, d.postInsert, bothChannels);
    if (has (Send))
    {
        for (auto& bus : engine.getBuses())
        {
            const int id = bus->getId();
            if (bus.get() == &dst || dst.getKind() == StripKind::Main || dst.getKind() == StripKind::Matrix)
                continue;
            if (dst.isBus() && ! busFeedsBus (dst.getId(), id))
                continue;
            auto* from = src.sends.find (id);
            if (from == nullptr && dst.sends.find (id) == nullptr)
                continue;
            // A send never set copies as the factory send (off, -oo, its mode and pan link kept).
            auto& to = dst.sends[(size_t) id];
            to.enabled = from != nullptr && from->enabled.load();
            if (from != nullptr)
                to.panLink = from->panLink.load();
            to.ignoreMute = from != nullptr && from->ignoreMute.load();
            to.panReversed = from != nullptr && from->panReversed.load();
            if (from != nullptr)
                to.mode = from->mode.load();
            to.levelDb = from != nullptr ? from->levelDb.load() : -144.0f;
            to.pan = from != nullptr ? from->pan.load() : 0.0f;
        }
        // TAP WIDTH and TAP EQ shape what the channel's TAP-mode sends take.
        if (bothChannels)
        {
            d.tapWidth = s.tapWidth.load();
            d.tapEq.on = s.tapEq.on.load();
            for (int i = 0; i < ld::kTapEqBands; ++i)
            {
                d.tapEq.bands[i].gainDb = s.tapEq.bands[i].gainDb.load();
                d.tapEq.bands[i].freq = s.tapEq.bands[i].freq.load();
                d.tapEq.bands[i].q = s.tapEq.bands[i].q.load();
            }
        }
        if (dst.getKind() != StripKind::Matrix && src.getKind() != StripKind::Matrix)
            for (int i = 0; i < kNumMatrices; ++i)
            {
                d.matrix[i].on = s.matrix[i].on.load();
                d.matrix[i].pre = s.matrix[i].pre.load();
                d.matrix[i].ignoreMute = s.matrix[i].ignoreMute.load();
                d.matrix[i].levelDb = s.matrix[i].levelDb.load();
                d.matrix[i].pan = s.matrix[i].pan.load();
                d.matrix[i].panLink = s.matrix[i].panLink.load();
                d.matrix[i].panReversed = s.matrix[i].panReversed.load();
            }
    }
    if (has (MainBit) && dst.getKind() != StripKind::Main && dst.getKind() != StripKind::Matrix
        && src.getKind() != StripKind::Main && src.getKind() != StripKind::Matrix)
        for (int i = 0; i < kNumMains; ++i)
        {
            d.mains[i].on = s.mains[i].on.load();
            d.mains[i].pre = s.mains[i].pre.load();
            d.mains[i].levelDb = s.mains[i].levelDb.load();
        }
    if (has (Fdr))
        d.faderDb = s.faderDb.load();
    if (has (MuteBit))
        d.mute = s.mute.load();
    if (has (PanBit))
    {
        d.pan = s.pan.load();
        d.width = s.width.load();
    }
}

/** Default strips of every kind, in a scratch engine (INIT copies from them). */
class DefaultStrips
{
public:
    DefaultStrips() : engine (ld::ld_engine_create (0))
    {
        if (engine == nullptr)
            return;
        if (auto* ch = ld::ld_engine_add_channel (engine, -1))
            strips.push_back (std::make_unique<Strip> (engine, ch));
        for (int k = 1; k < kNumStripKinds; ++k)
            if (auto* s = ld::ld_engine_strip_at (engine, k, 0))
                strips.push_back (std::make_unique<Strip> (engine, s));
        // The factory sends to the console's buses (each bus slot has its own mode).
        for (auto& s : strips)
            for (int bus = 0; bus < kWingBuses; ++bus)
                (void) s->sends[(size_t) bus];
    }

    ~DefaultStrips()
    {
        strips.clear();
        if (engine != nullptr)
        {
            ld::ld_engine_shutdown_workers (engine);
            ld::ld_engine_destroy (engine);
        }
    }

    const Strip* of (StripKind k) const
    {
        for (auto& s : strips)
            if (s->getKind() == k)
                return s.get();
        return nullptr;
    }

    /** A fresh console's DCA (all sixteen start the same). */
    const ld::DcaParams* dca() const { return engine != nullptr ? &ld::ld_engine_controls (engine)->dcas[0] : nullptr; }

private:
    ld::Engine* engine;
    std::vector<std::unique_ptr<Strip>> strips;
    JUCE_DECLARE_NON_COPYABLE (DefaultStrips)
};

class InitCopyScreen final : public OverlayScreen, private juce::Timer
{
public:
    InitCopyScreen (UiContext& c, Strip& s, bool isCopy, HomeTab from)
        : OverlayScreen (isCopy ? "CHANNEL COPY" : "CHANNEL INIT"), ctx (c), copy (isCopy), scope (scopeFor (from))
    {
        source = state::StripRef::of (s);
        destKind = s.getKind();
        if (! copy)
            destinations.push_back (state::StripRef::of (s));

        none.onClick = [this] { scope = 0; repaint(); };
        all.onClick = [this] { scope = (1u << kNumScopeBits) - 1u; repaint(); };
        // [Fig. 22] square keys, an icon over the caption.
        static const Icon scopeIcons[] = { Icon::Link, Icon::Wrench, Icon::KeyboardEdit, Icon::Tag, Icon::Xlr, Icon::LowCut, Icon::Gate, Icon::Compressor, Icon::Eq,
                                           Icon::DeltaT, Icon::Insert, Icon::Insert, Icon::Sends, Icon::Faders, Icon::GainArrows, Icon::MuteGroup, Icon::Circle };
        static_assert (std::size (scopeIcons) == kNumScopeBits);
        none.iconAbove = all.iconAbove = true;
        none.offColour = all.offColour = colours::tab;
        none.offInk = all.offInk = colours::caption;
        none.frame = all.frame = colours::tab; // plain icons on the header, no key frame (3.1)
        for (int i = 0; i < kNumScopeBits; ++i)
        {
            auto* k = scopeKeys.add (new IconKey (scopeName (i) + juce::String (i == Send || i == MainBit ? juce::String::fromUTF8 (" \xe2\x96\xbe") : juce::String()),
                                                  scopeIcons[i], colours::accent));
            k->iconAbove = true;
            k->isOn = [this, i] { return (scope >> i) & 1u; };
            k->onClick = [this, i] { scope ^= 1u << i; };
            addAndMakeVisible (k);
        }
        action.setButtonText (copy ? "COPY" : "INIT");
        action.setIcon (copy ? Icon::Copy : Icon::Reset);
        action.onClick = [this] { apply(); };
        static const char* tabNames[] = { "CHAN", "AUX", "BUS", "MAIN", "MATRIX" };
        for (int k = 0; k < kNumStripKinds; ++k)
        {
            auto* t = tabs.add (new IconKey (tabNames[k], std::nullopt, colours::accent));
            t->offInk = colours::caption;
            t->onClick = [this, k]
            {
                destKind = static_cast<StripKind> (k);
                updateTabs();
                repaint();
            };
            addAndMakeVisible (t);
        }
        for (juce::Component* comp : { (juce::Component*) &none, (juce::Component*) &all, (juce::Component*) &action })
            addAndMakeVisible (comp);
        action.outlined = true;
        action.offInk = colours::caption;
        updateTabs();
        // [Fig. 22] a window over the dimmed page, 788 x 654 of the 1280 x 720 page.
        setPlacement (OverlayPlacement::centred (788.0f / 1280.0f, 654.0f / 720.0f));
        wingWindow = { 232.0f, 104.0f, 787.0f, 654.0f }; // as the 3.1 console places it
        startTimerHz (4);
    }

    void paint (juce::Graphics& g) override
    {
        OverlayScreen::paint (g);
        // SCOPE: the title at the left, NONE and ALL in the header.
        const int headerH = scopeHeader.getHeight(), gap = scopeBody.getY() - scopeHeader.getBottom();
        drawDarkPanel (g, scopePanel, " ", headerH, gap);
        g.setColour (colours::caption);
        g.setFont (panelTitleFont (headerH));
        g.drawText ("SCOPE", scopeHeader.withTrimmedLeft (juce::roundToInt ((float) headerH * 0.33f)), juce::Justification::centredLeft, false);
        // The rule under the scope keys.
        g.setColour (juce::Colours::black);
        g.fillRect (scopeRule);
        drawDarkPanel (g, destPanel, "DESTINATION", headerH, gap);
        g.setColour (juce::Colours::black);
        g.fillRect (gridArea.expanded (juce::roundToInt (wingPx (4.0f))));
        if (copy)
            if (auto* src = state::findStrip (ctx.engine, source))
            {
                paintStripTile (g, sourceTile.toFloat(), *src, false);
                g.setColour (colours::accent);
                g.drawRect (sourceTile.toFloat(), 2.0f);
            }

        // The destination tiles: 64 x 78 on a 66 x 83 pitch.
        tiles.clear();
        const auto& list = ctx.engine.getStrips (destKind);
        const int n = (int) list.size();
        if (n == 0)
            return;
        auto grid = gridArea;
        const int columns = 8;
        const int rows = juce::jmax (5, (n + columns - 1) / columns);
        const float w = (float) grid.getWidth() / (float) columns, h = juce::jmin ((float) grid.getHeight() / (float) rows, w * 83.0f / 66.0f);
        for (int i = 0; i < n; ++i)
        {
            auto r = juce::Rectangle<float> ((float) grid.getX() + (float) (i % columns) * w, (float) grid.getY() + (float) (i / columns) * h, w, h)
                         .reduced (w / 66.0f, h * 2.5f / 83.0f);
            const auto ref = state::StripRef::of (*list[(size_t) i]);
            const bool sel = std::find (destinations.begin(), destinations.end(), ref) != destinations.end();
            const bool isSource = copy && ref == source;
            paintDestinationTile (g, r, *list[(size_t) i], sel, isSource);
            tiles.push_back ({ r.toNearestInt(), ref });
        }
    }

    void layoutContent (juce::Rectangle<int>) override
    {
        // [Fig. 22] in px of the console's window (788 x 654 at 232, 104 of the
        // 1280 x 800 screen): SCOPE 236 px wide with 52 px keys on a 56 px
        // pitch, DESTINATION 544 px with its tabs and the tile grid.
        const auto window = windowArea();
        const float sx = (float) window.getWidth() / 788.0f, sy = (float) window.getHeight() / 654.0f;
        auto rect = [&] (float x0, float y0, float x1, float y1)
        {
            return juce::Rectangle<int>::leftTopRightBottom (window.getX() + juce::roundToInt ((x0 - 232.0f) * sx), window.getY() + juce::roundToInt ((y0 - 104.0f) * sy),
                                                             window.getX() + juce::roundToInt ((x1 - 232.0f) * sx), window.getY() + juce::roundToInt ((y1 - 104.0f) * sy));
        };
        scopePanel = rect (232.0f, 192.0f, 468.0f, 758.0f);
        destPanel = rect (476.0f, 192.0f, 1020.0f, 758.0f);
        scopeHeader = rect (232.0f, 192.0f, 468.0f, 256.0f);
        scopeBody = rect (232.0f, 264.0f, 468.0f, 758.0f);
        all.setBounds (rect (412.0f, 196.0f, 456.0f, 252.0f));
        none.setBounds (rect (356.0f, 196.0f, 400.0f, 252.0f));
        // The source (COPY): a small tile at the left of the title bar.
        sourceTile = copy ? rect (240.0f, 108.0f, 360.0f, 180.0f) : juce::Rectangle<int>();
        scopeRule = rect (248.0f, 566.0f, 452.0f, 570.0f);
        action.setBounds (rect (278.0f, 674.0f, 422.0f, 738.0f));
        // CONN CONFIG CUST TAGS / IN FILTER GATE DYN / EQ DELAY PRE POST / SEND MAIN / FDR MUTE PAN
        const int layout[5][4] = { { Conn, Config, Cust, Tags }, { In, FilterBit, GateBit, DynBit }, { EqBit, DelayBit, Pre, Post },
                                   { Send, MainBit, -1, -1 }, { Fdr, MuteBit, PanBit, -1 } };
        for (int r = 0; r < 5; ++r)
            for (int col = 0; col < 4; ++col)
                if (layout[r][col] >= 0)
                    scopeKeys[layout[r][col]]->setBounds (rect (240.0f + 56.0f * (float) col, 272.0f + 56.0f * (float) r, 292.0f + 56.0f * (float) col,
                                                                324.0f + 56.0f * (float) r));

        for (int k = 0; k < tabs.size(); ++k)
            tabs[k]->setBounds (rect (483.0f + 106.75f * (float) k, 270.0f, 585.0f + 106.75f * (float) k, 326.0f));
        gridArea = rect (484.0f, 336.0f, 1012.0f, 751.0f);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const auto pos = e.getPosition();
        if (copy && sourceTile.contains (pos))
        {
            // Choose another source.
            juce::StringArray items;
            std::vector<state::StripRef> refs;
            for (int k = 0; k < kNumStripKinds; ++k)
                for (auto& strip : ctx.engine.getStrips (static_cast<StripKind> (k)))
                {
                    refs.push_back (state::StripRef::of (*strip));
                    items.add (describeStrip (*strip).toUpperCase());
                }
            const auto it = std::find (refs.begin(), refs.end(), source);
            popups::ListOptions options;
            options.columns = 4;
            options.caption = "COPY SOURCE";
            if (sourceAnchor.getParentComponent() == nullptr)
            {
                sourceAnchor.setInterceptsMouseClicks (false, false);
                addChildComponent (sourceAnchor);
            }
            sourceAnchor.setBounds (sourceTile);
            juce::Component::SafePointer<InitCopyScreen> safe (this);
            popups::chooseFrom (sourceAnchor, items, it != refs.end() ? (int) (it - refs.begin()) : -1, [safe, refs] (int i)
            {
                if (safe != nullptr && juce::isPositiveAndBelow (i, (int) refs.size()))
                {
                    safe->source = refs[(size_t) i];
                    safe->repaint();
                }
            }, options);
            return;
        }
        for (auto& [area, ref] : tiles)
            if (area.contains (pos))
            {
                if (copy && ref == source)
                    return;
                auto it = std::find (destinations.begin(), destinations.end(), ref);
                if (it != destinations.end())
                    destinations.erase (it);
                else
                    destinations.push_back (ref);
                repaint();
                return;
            }
    }

private:
    /** A destination tile [Fig. 22]: the colour strip with the name, the icon
        large in the middle and the number under it; a light frame when chosen. */
    static void paintDestinationTile (juce::Graphics& g, juce::Rectangle<float> r, const Strip& s, bool selected, bool isSource)
    {
        const auto colour = stripDisplayColour (s);
        g.setColour (juce::Colour (0xff202420));
        g.fillRect (r);
        auto body = r;
        auto bar = body.removeFromTop (juce::jmax (6.0f, r.getHeight() * 16.0f / 78.0f));
        g.setColour (colour);
        g.fillRect (bar);
        if (! hasDefaultName (s))
        {
            g.setColour (colour.getPerceivedBrightness() > 0.3f ? juce::Colours::black : colours::ink);
            g.setFont (wingFontPx (bar.getHeight() * 0.6f));
            g.drawFittedText (stripName (s), bar.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.6f);
        }
        auto number = body.removeFromBottom (body.getHeight() * 0.3f);
        g.setColour (colours::caption);
        g.setFont (wingFontPx (number.getHeight() * 0.62f));
        g.drawText (s.isInput() ? juce::String (s.getNumber()) : shortLabel (s), number, juce::Justification::centred, false);
        auto iconArea = body.reduced (2.0f).withSizeKeepingCentre (juce::jmin (body.getWidth(), body.getHeight()), juce::jmin (body.getWidth(), body.getHeight()) - 2.0f);
        if (! drawWingIcon (g, s.getIcon(), iconArea, colours::caption) && ! s.isInput())
            drawIcon (g, stripIcon (s.getKind()), iconArea.reduced (iconArea.getHeight() * 0.1f), colours::dim);
        if (selected || isSource)
        {
            // [Fig. 22] a light grey frame round a chosen tile and the source (COPY).
            g.setColour (colours::caption);
            g.drawRect (r.expanded (1.0f), 2.0f);
        }
    }

    void updateTabs()
    {
        for (int k = 0; k < tabs.size(); ++k)
            tabs[k]->setToggleState (k == (int) destKind, juce::dontSendNotification);
    }

    void timerCallback() override
    {
        // (3.1: the key stays lit with no destination chosen; it then does nothing.)
        action.setEnabled (scope != 0);
    }

    void apply()
    {
        if (scope == 0 || destinations.empty())
            return;
        std::unique_ptr<DefaultStrips> defaults;
        if (! copy)
            defaults = std::make_unique<DefaultStrips>();
        int done = 0;
        for (auto& ref : destinations)
        {
            auto* dst = state::findStrip (ctx.engine, ref);
            if (dst == nullptr)
                continue;
            const Strip* src = copy ? state::findStrip (ctx.engine, source) : defaults->of (dst->getKind());
            if (src == nullptr)
                continue;
            copyStripParams (ctx.engine, *src, *dst, scope);
            ++done;
        }
        ctx.log.add (EventLog::Level::Info, juce::String (copy ? "Channel copy: " : "Channel init: ") + juce::String (done) + " strip(s)");
        if (onClose)
            onClose();
    }

    UiContext& ctx;
    bool copy;
    juce::uint32 scope;
    state::StripRef source;
    StripKind destKind = StripKind::Channel;
    std::vector<state::StripRef> destinations;
    IconKey none { "NONE", Icon::ListNone }, all { "ALL", Icon::ListAll }, action { "INIT", Icon::Reset, colours::accent };
    juce::OwnedArray<IconKey> scopeKeys;
    juce::OwnedArray<IconKey> tabs;
    juce::Rectangle<int> scopePanel, destPanel, scopeHeader, scopeBody, scopeRule, sourceTile, gridArea;
    std::vector<std::pair<juce::Rectangle<int>, state::StripRef>> tiles;
    juce::Component sourceAnchor; // where the source list opens (not shown)
};

//==============================================================================
/** BUS / MAIN / MATRIX FEED CONFIGURATION (Figs. 26-27): the sources feeding
    the strip, and actions that set the send mode, panning, level and enable
    of the selected sources at once. */
class FeedConfigScreen final : public OverlayScreen, private juce::Timer
{
public:
    FeedConfigScreen (UiContext& c, Strip& s)
        : OverlayScreen (feedTitle (s)), ctx (c), target (state::StripRef::of (s))
    {
        // Select none, all eligible sources or one group of them.
        noneKey.onClick = [this] { selection.clear(); repaint(); };
        allKey.onClick = [this] { selectGroup (-1); };
        chKey.onClick = [this] { selectGroup (0); };
        auxKey.onClick = [this] { selectGroup (1); };
        busKey.onClick = [this] { selectGroup (2); };
        const bool busTarget = s.isBus();
        const bool mainTarget = s.getKind() == StripKind::Main;
        modeKeys = std::make_unique<Segmented> (mainTarget || ! busTarget ? juce::StringArray { "KEEP", "TAP/PRE", "POST" } : juce::StringArray { "KEEP", "TAP/PRE", "POST", "GROUP" },
                                                [this] { return modeChoice; }, [this] (int i) { modeChoice = i; }, colours::accent);
        ignoreKeys = std::make_unique<Segmented> (juce::StringArray { "KEEP", "SET OFF", "SET ON" }, [this] { return ignoreChoice; },
                                                  [this] (int i) { ignoreChoice = i; }, colours::accent);
        ignoreKeys->setEnabled (! mainTarget); // main sends have no IGNORE CHANNEL MUTE
        panKeys = std::make_unique<Segmented> (juce::StringArray { "KEEP", "SET TO\nCENTER", "SET TO\nCH PAN", "LINK\nTO CH PAN", "LINK\nREVERSED" },
                                               [this] { return panChoice; }, [this] (int i) { panChoice = i; }, colours::accent);
        panKeys->twoLines = true;
        // Mains have no send pan; matrix sends link to the pan of input and aux channels only.
        panKeys->isEnabledAt = [mainTarget] (int i) { return i == 0 || ! mainTarget; };
        levelKeys = std::make_unique<Segmented> (juce::StringArray { "KEEP", "SET TO\n-oo dB", "SET TO\n-10 dB", "SET TO\n0 dB" },
                                                 [this] { return levelChoice; }, [this] (int i) { levelChoice = i; }, colours::accent);
        levelKeys->twoLines = true;
        enableKeys = std::make_unique<Segmented> (juce::StringArray { "KEEP", "SET OFF", "SET ON" }, [this] { return enableChoice; }, [this] (int i) { enableChoice = i; }, colours::accent);
        applyKey.onClick = [this] { apply(); };
        for (juce::Component* comp : { (juce::Component*) &noneKey, (juce::Component*) &allKey, (juce::Component*) &chKey, (juce::Component*) &auxKey,
                                       (juce::Component*) &busKey, (juce::Component*) modeKeys.get(),
                                       (juce::Component*) ignoreKeys.get(), (juce::Component*) panKeys.get(), (juce::Component*) levelKeys.get(),
                                       (juce::Component*) enableKeys.get(), (juce::Component*) &applyKey })
            addAndMakeVisible (comp);
        startTimerHz (4);
    }

    static juce::String feedTitle (const Strip& s)
    {
        const char* kind = s.isBus() ? "BUS" : (s.getKind() == StripKind::Main ? "MAIN" : "MATRIX");
        return juce::String (kind) + " " + juce::String (s.getNumber()) + " FEED CONFIGURATION";
    }

    void paint (juce::Graphics& g) override
    {
        OverlayScreen::paint (g);
        auto* dst = state::findStrip (ctx.engine, target);
        if (dst == nullptr)
            return;

        // Section captions and tints of the action panel.
        auto caption = [&g] (juce::Rectangle<int> r, const juce::String& text, juce::Colour tint)
        {
            if (! tint.isTransparent())
            {
                g.setColour (tint);
                g.fillRect (r.expanded (4, 2).withHeight (r.getHeight() * 3 + 8));
            }
            g.setColour (Colours::text);
            g.setFont (wingFont (TypeScale::body));
            g.drawText (text, r, juce::Justification::centredLeft);
        };
        caption (captions[0], "SET SEND MODE", {});
        caption (captions[1], "IGNORE CHANNEL MUTE (TAP ONLY)", {});
        caption (captions[2], "SEND PANNING", {});
        caption (captions[3], "SEND LEVEL", juce::Colour (0xff1e2e44));
        caption (captions[4], "SEND ENABLE", juce::Colour (0xff33331c));

        // The source groups (CHANNEL, AUX, BUS; MAIN for a matrix), each cell:
        // colour strip, number, mode badge, PAN LINK or pan line. A disabled
        // send is greyed out; a send that can't exist is an empty well.
        cells.clear();
        const auto groups = sourceGroups (*dst);
        const int columns = 8;
        int totalRows = 0;
        for (auto& grp : groups)
            totalRows += ((int) grp.second.size() + columns - 1) / columns;
        const float captionH = juce::jlimit (12.0f, screenCap (16.0f), (float) gridArea.getHeight() * 0.035f);
        const float w = (float) gridArea.getWidth() / (float) columns;
        const float h = juce::jmin (((float) gridArea.getHeight() - captionH * (float) groups.size() - 4.0f * (float) groups.size()) / (float) juce::jmax (1, totalRows),
                                    w * 0.62f);
        float y = (float) gridArea.getY();
        for (auto& [groupCaption, sources] : groups)
        {
            g.setColour (colours::caption);
            g.setFont (homeFont (TypeScale::caption, captionH));
            g.drawText (groupCaption, juce::Rectangle<float> ((float) gridArea.getX(), y, (float) gridArea.getWidth(), captionH).toNearestInt(), juce::Justification::centredLeft);
            y += captionH;
            for (int i = 0; i < (int) sources.size(); ++i)
            {
                const auto& src = *sources[(size_t) i];
                const auto cellArea = juce::Rectangle<float> ((float) gridArea.getX() + (float) (i % columns) * w, y + (float) (i / columns) * h, w, h);
                auto r = cellArea.reduced (1.5f);
                const auto ref = state::StripRef::of (src);
                bool possible = true;
                int mode = 1;
                bool on = false, link = false;
                float pan = 0.0f;
                describe (*dst, src, possible, on, mode, link, pan);
                const bool sel = std::find (selection.begin(), selection.end(), ref) != selection.end();
                if (! possible)
                {
                    g.setColour (colours::well);
                    g.fillRect (r);
                    continue;
                }
                g.setColour (on ? colours::keyOff : colours::keyOff.withMultipliedBrightness (0.55f));
                g.fillRect (r);
                auto band = r.removeFromTop (juce::jmax (r.getHeight() < 24.0f ? 3.0f : 5.0f, r.getHeight() * 0.18f));
                g.setColour (on ? stripDisplayColour (src) : stripDisplayColour (src).withSaturation (0.0f).withMultipliedBrightness (0.5f));
                g.fillRect (band);
                g.setColour (on ? Colours::text : Colours::textDim.withAlpha (0.6f));
                if (r.getHeight() < 20.0f)
                {
                    // Short cells (1280x800): the number and the mode badge side
                    // by side; no room for the pan line.
                    auto left = r.removeFromLeft (r.getWidth() * 0.45f);
                    g.setFont (homeFont (TypeScale::body, r.getHeight() * 0.75f, WingWeight::SemiBold));
                    g.drawFittedText (shortLabel (src), left.toNearestInt(), juce::Justification::centred, 1, 0.7f);
                    drawChip (g, r.reduced (1.0f, juce::jmax (1.0f, r.getHeight() * 0.15f)), sendModeName (mode, src.isInput(), true),
                              on ? sendModeColour (mode) : colours::chipOff, on ? juce::Colours::black : Colours::textDim);
                    if (sel)
                    {
                        g.setColour (juce::Colours::white);
                        g.drawRect (cellArea.reduced (1.0f), 2.0f);
                    }
                    cells.push_back ({ cellArea.toNearestInt(), ref });
                    continue;
                }
                g.setFont (homeFont (TypeScale::body, r.getHeight() * 0.3f, WingWeight::SemiBold));
                g.drawText (shortLabel (src), r.withHeight (r.getHeight() * 0.4f).toNearestInt(), juce::Justification::centred);
                auto badge = r.withTrimmedTop (r.getHeight() * 0.42f).withHeight (r.getHeight() * 0.26f).reduced (r.getWidth() * 0.18f, 0.0f);
                drawChip (g, badge, sendModeName (mode, src.isInput(), true), on ? sendModeColour (mode) : colours::chipOff, on ? juce::Colours::black : Colours::textDim);
                if (link)
                {
                    g.setColour (on ? colours::stereo : Colours::textDim.withAlpha (0.6f));
                    g.setFont (homeFont (TypeScale::caption, r.getHeight() * 0.18f));
                    g.drawFittedText ("PAN LINK", r.withTrimmedTop (r.getHeight() * 0.7f).toNearestInt(), juce::Justification::centred, 1, 0.6f);
                }
                else
                {
                    auto line = r.withTrimmedTop (r.getHeight() * 0.8f).reduced (r.getWidth() * 0.12f, 0.0f);
                    g.setColour (Colours::textDim);
                    g.fillRect (line.getX(), line.getCentreY(), line.getWidth(), 1.0f);
                    const float x = line.getCentreX() + pan / 100.0f * line.getWidth() * 0.5f;
                    g.setColour (on ? Colours::text : Colours::textDim);
                    g.fillRect (x - 1.0f, line.getY(), 2.0f, line.getHeight());
                }
                if (sel)
                {
                    g.setColour (juce::Colours::white);
                    g.drawRect (cellArea.reduced (1.0f), 2.0f);
                }
                cells.push_back ({ cellArea.toNearestInt(), ref });
            }
            y += (float) (((int) sources.size() + columns - 1) / columns) * h + 4.0f;
        }
    }

    void layoutContent (juce::Rectangle<int> b) override
    {
        auto left = b.removeFromLeft (b.getWidth() * 58 / 100);
        auto top = left.removeFromTop (juce::jlimit (26, screenCap (40), left.getHeight() / 10));
        const int keyW = juce::jmin (screenCap (100), top.getWidth() / 5);
        for (auto* k : { &noneKey, &allKey, &chKey, &auxKey, &busKey })
            k->setBounds (top.removeFromLeft (keyW).reduced (2));
        left.removeFromTop (4);
        gridArea = left;
        b.removeFromLeft (10);

        auto applyRow = b.removeFromBottom (juce::jlimit (28, screenCap (40), b.getHeight() / 9));
        applyKey.setBounds (applyRow.removeFromRight (juce::jmin (screenCap (120), applyRow.getWidth() / 3)));
        b.removeFromBottom (4);
        const int sectionH = b.getHeight() / 5;
        juce::Component* rows[] = { modeKeys.get(), ignoreKeys.get(), panKeys.get(), levelKeys.get(), enableKeys.get() };
        for (int i = 0; i < 5; ++i)
        {
            auto sec = b.removeFromTop (sectionH);
            captions[i] = sec.removeFromTop (16);
            rows[i]->setBounds (sec.removeFromTop (juce::jmin (sec.getHeight() - 4, screenCap (40))));
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragAdds = true;
        for (auto& [area, ref] : cells)
            if (area.contains (e.getPosition()))
            {
                dragAdds = std::find (selection.begin(), selection.end(), ref) == selection.end();
                touch (ref);
            }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        // Touch and drag selects several sources.
        for (auto& [area, ref] : cells)
            if (area.contains (e.getPosition()))
                touch (ref);
    }

private:
    /** The sources shown, by group: CHANNEL, AUX, BUS (and MAIN for a matrix). */
    std::vector<std::pair<juce::String, std::vector<const Strip*>>> sourceGroups (const Strip& dst) const
    {
        std::vector<std::pair<juce::String, std::vector<const Strip*>>> groups;
        auto add = [&groups] (const char* caption, const MixerEngine::StripList& list)
        {
            groups.push_back ({ caption, {} });
            for (auto& st : list)
                groups.back().second.push_back (st.get());
        };
        add ("CHANNEL", ctx.engine.getChannels());
        add ("AUX", ctx.engine.getAux());
        add ("BUS", ctx.engine.getBuses());
        if (dst.getKind() == StripKind::Matrix)
            add ("MAIN", ctx.engine.getMains());
        return groups;
    }

    /** Selects every eligible source of a group (-1: of all groups). */
    void selectGroup (int group)
    {
        selection.clear();
        if (auto* dst = state::findStrip (ctx.engine, target))
        {
            const auto groups = sourceGroups (*dst);
            for (int i = 0; i < (int) groups.size(); ++i)
                if (group < 0 || group == i)
                    for (auto* src : groups[(size_t) i].second)
                    {
                        bool possible = true, on = false, link = false;
                        int mode = 1;
                        float pan = 0.0f;
                        describe (*dst, *src, possible, on, mode, link, pan);
                        if (possible)
                            selection.push_back (state::StripRef::of (*src));
                    }
        }
        repaint();
    }

    void touch (const state::StripRef& ref)
    {
        auto it = std::find (selection.begin(), selection.end(), ref);
        if (dragAdds && it == selection.end())
            selection.push_back (ref);
        else if (! dragAdds && it != selection.end())
            selection.erase (it);
        repaint();
    }

    static void describe (const Strip& dst, const Strip& src, bool& possible, bool& on, int& mode, bool& link, float& pan)
    {
        possible = true;
        on = false;
        mode = 1;
        link = false;
        pan = 0.0f;
        if (dst.isBus())
        {
            if (&src == &dst || (src.isBus() && ! busFeedsBus (src.getId(), dst.getId())))
            {
                possible = false;
                return;
            }
            if (auto* send = src.sends.find (dst.getId()))
            {
                on = send->enabled.load();
                mode = send->mode.load();
                link = send->panLink.load();
                pan = link ? (send->panReversed.load() ? -src.params.pan.load() : src.params.pan.load()) : send->pan.load();
            }
            return;
        }
        if (dst.getKind() == StripKind::Main)
        {
            if (src.getKind() == StripKind::Main || src.getKind() == StripKind::Matrix)
            {
                possible = false;
                return;
            }
            auto& m = src.params.mains[juce::jlimit (0, kNumMains - 1, dst.getId())];
            on = m.on.load();
            mode = m.pre.load() ? 0 : 1;
            link = true;
            pan = src.params.pan.load();
            return;
        }
        if (src.getKind() == StripKind::Matrix)
        {
            possible = false;
            return;
        }
        auto& m = src.params.matrix[juce::jlimit (0, kNumMatrices - 1, dst.getId())];
        on = m.on.load();
        mode = m.pre.load() ? 0 : 1;
        link = src.isInput() && m.panLink.load();
        pan = link ? (m.panReversed.load() ? -src.params.pan.load() : src.params.pan.load()) : m.pan.load();
    }

    void timerCallback() override
    {
        applyKey.setEnabled (! selection.empty() && (modeChoice != 0 || ignoreChoice != 0 || panChoice != 0 || levelChoice != 0 || enableChoice != 0));
    }

    void apply()
    {
        auto* dst = state::findStrip (ctx.engine, target);
        if (dst == nullptr)
            return;
        static const float levels[] = { 0.0f, -100.0f, -10.0f, 0.0f };
        for (auto& ref : selection)
        {
            auto* src = state::findStrip (ctx.engine, ref);
            if (src == nullptr)
                continue;
            bool possible = true, on = false, link = false;
            int mode = 1;
            float pan = 0.0f;
            describe (*dst, *src, possible, on, mode, link, pan);
            if (! possible)
                continue;
            if (dst->isBus())
            {
                auto& send = src->sends[(size_t) dst->getId()];
                if (modeChoice > 0)
                    send.mode = modeChoice - 1; // TAP/PRE, POST, GROUP
                if (ignoreChoice > 0 && src->canIgnoreMuteOnSends())
                    send.ignoreMute = ignoreChoice == 2;
                if (panChoice == 1 || panChoice == 2)
                {
                    send.panLink = false;
                    send.panReversed = false;
                    send.pan = panChoice == 1 ? 0.0f : src->params.pan.load();
                }
                else if (panChoice == 3 || panChoice == 4)
                {
                    send.panLink = true;
                    send.panReversed = panChoice == 4;
                }
                if (levelChoice > 0)
                    send.levelDb = levels[levelChoice];
                if (enableChoice > 0)
                    send.enabled = enableChoice == 2;
            }
            else if (dst->getKind() == StripKind::Main)
            {
                auto& m = src->params.mains[juce::jlimit (0, kNumMains - 1, dst->getId())];
                if (modeChoice > 0)
                    m.pre = modeChoice == 1;
                if (levelChoice > 0)
                    m.levelDb = levels[levelChoice];
                if (enableChoice > 0)
                    m.on = enableChoice == 2;
            }
            else
            {
                auto& m = src->params.matrix[juce::jlimit (0, kNumMatrices - 1, dst->getId())];
                if (modeChoice > 0)
                    m.pre = modeChoice == 1;
                if (ignoreChoice > 0 && src->canIgnoreMuteOnSends())
                    m.ignoreMute = ignoreChoice == 2;
                if (panChoice == 1 || panChoice == 2)
                {
                    m.panLink = false;
                    m.panReversed = false;
                    m.pan = panChoice == 1 ? 0.0f : src->params.pan.load();
                }
                else if ((panChoice == 3 || panChoice == 4) && src->isInput())
                {
                    m.panLink = true;
                    m.panReversed = panChoice == 4;
                }
                if (levelChoice > 0)
                    m.levelDb = levels[levelChoice];
                if (enableChoice > 0)
                    m.on = enableChoice == 2;
            }
        }
        repaint();
    }

    UiContext& ctx;
    state::StripRef target;
    std::vector<state::StripRef> selection;
    std::vector<std::pair<juce::Rectangle<int>, state::StripRef>> cells;
    IconKey noneKey { "NONE", Icon::Cancel }, allKey { "ALL", std::nullopt }, chKey { "ALL CH", std::nullopt }, auxKey { "ALL AUX", std::nullopt },
        busKey { "ALL BUS", std::nullopt }, applyKey { "APPLY", Icon::Check, colours::accent };
    std::unique_ptr<Segmented> modeKeys, ignoreKeys, panKeys, levelKeys, enableKeys;
    juce::Rectangle<int> gridArea, captions[5];
    int modeChoice = 0, ignoreChoice = 0, panChoice = 0, levelChoice = 0, enableChoice = 0;
    bool dragAdds = true;
};

//==============================================================================
/** RTA SETTINGS / EQ/RTA SETTINGS (Figs. 24-25, D10), docked at the right:
    LO BAND / HI BAND fields (the WING EQ's low and high band types), then the
    RTA rows with the console's choices (`/cfg/rta`, `/$ctl/cfg/rta`): RTA
    SOURCE (IN | EQ | POST on HOME, PRE | POST the EQ or the filter), RANGE,
    DECAY, DETECTOR, AUTOGAIN, SIZE/MODE, FIXED GAIN and COLOR. HOME has its
    own settings; the EQ and FILTER screens share theirs (but not the source). */
class SettingsScreen final : public OverlayScreen
{
public:
    SettingsScreen (Strip& s, bool eqSettings)
        : OverlayScreen (eqSettings ? "EQ/RTA SETTINGS" : "RTA SETTINGS"),
          strip (s),
          eq (eqSettings),
          analysis (s),
          view (eqSettings ? ld::kRtaViewEq : (ResponseGraph::shownRtaView() == ld::kRtaViewFilter ? ld::kRtaViewFilter : ld::kRtaViewHome)),
          settings (analysis.settings (view))
    {
        if (eq)
        {
            const auto& spec = models::model (static_cast<models::Table> (strip.getEqTable()), strip.params.eq.model.load());
            for (auto [id, caption] : { std::make_pair ("leq", "LO BAND"), std::make_pair ("heq", "HI BAND") })
            {
                const int i = spec.find (id);
                if (i < 0)
                    continue;
                const auto& param = spec.params[(size_t) i];
                auto& value = strip.params.eq.values[i];
                juce::StringArray names;
                for (auto& c : param.choices)
                    names.add (c.toUpperCase());
                if (names.isEmpty())
                    names = { "PARAMETRIC", "SHELVING" };
                auto* field = bands.add (new ChoiceField (caption, [names, &value] { return names[juce::roundToInt (value.load())]; },
                                                          [names, &value] (ChoiceField& f)
                                                          {
                                                              popups::chooseFrom (f, names, juce::roundToInt (value.load()), [&value] (int v) { value = (float) v; });
                                                          }));
                addAndMakeVisible (field);
            }
        }
        // The rows as keys (Figs. 24-25): the source lit green, the others blue.
        auto& v = settings;
        auto& c = analysis.controls;
        std::atomic<int32_t>& source = view == ld::kRtaViewFilter ? c.filterTap : v.tap;
        auto row = [this] (const char* label, juce::StringArray names, std::atomic<int32_t>& value, juce::Colour on)
        {
            auto* keys = rta.add (new Segmented (std::move (names), [&value] { return value.load(); }, [&value] (int i) { value = i; }, on));
            rtaLabels.add (label);
            addAndMakeVisible (keys);
        };
        row ("RTA SOURCE", view == ld::kRtaViewHome ? juce::StringArray { "IN", "EQ", "POST" } : juce::StringArray { "PRE", "POST" }, source,
             colours::post);
        row ("RTA RANGE", { "30 dB", "60 dB" }, v.range, colours::choice);
        row ("RTA DECAY", { "SLOW", "MED", "FAST" }, v.decay, colours::choice);
        row ("RTA DETECTOR", { "PEAK", "RMS", "AVG" }, v.detector, colours::choice);
        bindKey (autoGain, v.autogain);
        addAndMakeVisible (autoGain);

        const auto sizes = view == ld::kRtaViewHome ? juce::StringArray { "OFF", "1/3", "FULL" } : juce::StringArray { "OFF", "1/4", "1/3", "1/2", "OVL/3", "OVL" };
        sizeMode = std::make_unique<ChoiceField> ("SIZE/MODE", [sizes, &v] { return sizes[juce::jlimit (0, sizes.size() - 1, (int) v.display.load())]; },
                                                  [sizes, &v] (ChoiceField& f)
                                                  { popups::chooseFrom (f, sizes, (int) v.display.load(), [&v] (int i) { v.display = i; }); });
        static const juce::StringArray colourNames { "RD25", "RD50", "RD75", "AM25", "AM50", "AM75", "BL25", "BL50", "BL75" };
        colour = std::make_unique<ChoiceField> ("COLOR", [&v] { return colourNames[juce::jlimit (0, 8, (int) v.colour.load())]; },
                                                [&v] (ChoiceField& f)
                                                { popups::chooseFrom (f, colourNames, (int) v.colour.load(), [&v] (int i) { v.colour = i; }); });
        fixedGain = std::make_unique<KnobBox> (bindLinear ("FIXED GAIN", v.gainDb, -5.0f, 50.0f, " dB", 0, 0.0f));
        for (juce::Component* comp : { (juce::Component*) sizeMode.get(), (juce::Component*) colour.get(), (juce::Component*) fixedGain.get() })
            addAndMakeVisible (comp);
    }

    void paint (juce::Graphics& g) override
    {
        OverlayScreen::paint (g);
        g.setColour (colours::panel);
        g.fillRect (body);
        g.setFont (homeFont (TypeScale::body, (float) rowH * 0.4f));
        for (int i = 0; i < rta.size(); ++i)
        {
            g.setColour (colours::dim);
            g.drawText (rtaLabels[i], rta[i]->getBounds().withX (labelX).withWidth (rta[i]->getX() - labelX - 8), juce::Justification::centredLeft);
        }
        g.drawText ("RTA AUTOGAIN", autoGain.getBounds().withX (labelX).withWidth (autoGain.getX() - labelX - 8), juce::Justification::centredLeft);
        // The colour COLOR picks, as a swatch beside its field.
        if (! swatch.isEmpty())
        {
            g.setColour (juce::Colours::black);
            g.fillRect (swatch);
            g.setColour (ResponseGraph::rtaColour (settings.colour.load()));
            g.fillRect (swatch.reduced (2));
        }
    }

    void layoutContent (juce::Rectangle<int> b) override
    {
        body = b;
        b = b.reduced (juce::jmax (8, b.getWidth() / 40), 6);
        labelX = b.getX();
        rowH = juce::jlimit (26, screenCap (52), b.getHeight() / 9);
        if (! bands.isEmpty())
        {
            auto row = b.removeFromTop (rowH);
            const int w = (row.getWidth() - 8) / 2;
            for (auto* f : bands)
            {
                f->setBounds (row.removeFromLeft (w));
                row.removeFromLeft (8);
            }
            b.removeFromTop (rowH / 3);
        }
        auto control = [&b, this] (juce::Component& c, int width)
        {
            auto row = b.removeFromTop (rowH);
            b.removeFromTop (rowH / 6);
            c.setBounds (row.removeFromRight (width).reduced (0, rowH / 10));
        };
        const int fieldW = b.getWidth() * 44 / 100;
        // A third of the field per key, right-aligned (Figs. 24-25).
        const int keyCounts[] = { view == ld::kRtaViewHome ? 3 : 2, 2, 3, 3 };
        for (int i = 0; i < rta.size(); ++i)
            control (*rta[i], fieldW * keyCounts[juce::jmin (i, 3)] / 3);
        control (autoGain, rowH * 3 / 2);
        auto row = b.removeFromTop (rowH);
        b.removeFromTop (rowH / 6);
        sizeMode->setBounds (row.removeFromLeft (fieldW));
        fixedGain->setBounds (row.removeFromRight (fieldW));
        auto colourRow = b.removeFromTop (rowH);
        colour->setBounds (colourRow.removeFromLeft (fieldW));
        swatch = colourRow.removeFromLeft (rowH).translated (8, 0).reduced (rowH / 6);
    }

private:
    Strip& strip;
    bool eq;
    Analysis analysis;
    int view;
    ld::RtaViewSettings& settings;
    juce::OwnedArray<ChoiceField> bands;
    juce::OwnedArray<Segmented> rta;
    juce::StringArray rtaLabels;
    IconKey autoGain { "ON", std::nullopt, colours::accent };
    std::unique_ptr<ChoiceField> sizeMode, colour;
    std::unique_ptr<KnobBox> fixedGain;
    juce::Rectangle<int> body, swatch;
    int labelX = 0, rowH = 30;
};

} // namespace

std::unique_ptr<HomeScreen> makeInputScreen (UiContext& c, Strip& s) { return std::make_unique<InputTab> (c, s); }
std::unique_ptr<HomeScreen> makeFilterScreen (UiContext& c, Strip& s) { return std::make_unique<FilterTab> (c, s); }
std::unique_ptr<HomeScreen> makeDynamicsScreen (UiContext& c, Strip& s, bool gate) { return std::make_unique<DynTab> (c, s, gate); }
std::unique_ptr<HomeScreen> makeEqScreen (UiContext& c, Strip& s) { return std::make_unique<EqTab> (c, s); }
std::unique_ptr<HomeScreen> makeInsertScreen (UiContext& c, Strip& s, bool post) { return std::make_unique<InsertTab> (c, s, post); }
std::unique_ptr<HomeScreen> makeMainScreen (UiContext& c, Strip& s) { return std::make_unique<MainTab> (c, s); }
std::unique_ptr<HomeScreen> makeSendsScreen (UiContext& c, Strip& s, bool matrixPage) { return std::make_unique<SendsTab> (c, s, matrixPage); }

int initializeParts (UiContext& ctx, const state::Scope& scope)
{
    // The CHANNEL INIT parts of each of our sections.
    auto bit = [] (int b) { return 1u << b; };
    const std::pair<juce::uint32, juce::uint32> parts[] = {
        { state::Name, bit (Cust) },
        { state::Routing, bit (Conn) | bit (Config) },
        { state::Preamp, bit (In) | bit (FilterBit) | bit (DelayBit) },
        { state::Gate, bit (GateBit) },
        { state::Eq, bit (EqBit) },
        { state::Dynamics, bit (DynBit) },
        { state::Inserts, bit (Pre) | bit (Post) },
        { state::Sends, bit (Send) | bit (MainBit) },
        { state::Fader, bit (Fdr) | bit (MuteBit) | bit (PanBit) },
        { state::Groups, bit (Tags) },
    };
    juce::uint32 initScope = 0;
    for (auto [section, b] : parts)
        if ((scope.sections & section) != 0)
            initScope |= b;

    DefaultStrips defaults;
    int done = 0;
    if (initScope != 0)
        for (int k = 0; k < kNumStripKinds; ++k)
            for (auto& strip : ctx.engine.getStrips (static_cast<StripKind> (k)))
                if (scope.includes (state::StripRef::of (*strip)))
                    if (const auto* src = defaults.of (strip->getKind()))
                    {
                        copyStripParams (ctx.engine, *src, *strip, initScope);
                        ++done;
                    }

    if (const auto* fresh = defaults.dca())
        for (int i = 0; i < kNumDcas; ++i)
            if ((scope.dcas >> i) & 1u)
            {
                auto& d = ctx.engine.getDca (i);
                d.faderDb = fresh->faderDb.load();
                d.mute = fresh->mute.load();
                d.solo = fresh->solo.load();
                d.led = fresh->led.load();
                d.colour = fresh->colour.load();
                d.icon = fresh->icon.load();
                ctx.engine.setDcaName (i, {});
            }
    for (int i = 0; i < kNumMuteGroups; ++i)
        if ((scope.muteGroups >> i) & 1u)
        {
            ctx.engine.muteGroups &= ~(1u << i);
            ctx.engine.setMuteGroupName (i, {});
        }
    for (int i = 0; i < kNumFx; ++i)
        if ((scope.fx >> i) & 1u)
            clearFx (ctx, i);
    return done;
}

std::unique_ptr<OverlayScreen> makeInitCopyScreen (UiContext& c, Strip& s, bool copy, HomeTab from)
{
    return std::make_unique<InitCopyScreen> (c, s, copy, from);
}

std::unique_ptr<OverlayScreen> makeFeedConfigScreen (UiContext& c, Strip& s)
{
    if (s.isInput())
        return {};
    return std::make_unique<FeedConfigScreen> (c, s);
}

std::unique_ptr<OverlayScreen> makeSettingsScreen (UiContext&, Strip& s, bool eqSettings)
{
    // Docked at the right over the dimmed page, as EQ/RTA SETTINGS on the 3.1 screens.
    auto screen = std::make_unique<SettingsScreen> (s, eqSettings);
    screen->setPlacement (OverlayPlacement::dockRight (0.46f));
    return screen;
}

} // namespace livedaw::ui
