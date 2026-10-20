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
        levelPanel->setBounds (box (area.removeFromRight (area.getWidth() * 22 / 100), "LEVEL"));
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
constexpr float kSpeedOfSound = 343.0f;
constexpr float kMaxDelayMs = 500.0f;

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
        default: return ms * sr / 1000.0f;
    }
}

juce::String delayText (float value, int mode)
{
    static const char* units[] = { " m", " ft", " ms", "" };
    return juce::String (value, mode == 3 ? 0 : 1) + units[juce::jlimit (0, 3, mode)];
}

/** The delay as a knob: a square-law taper up to 500 ms in the current unit. */
KnobBinding delayBinding (UiContext& ctx, InputParams& in)
{
    KnobBinding b;
    const int mode = juce::jlimit (0, 3, (int) in.delayMode.load());
    b.label = mode <= 1 ? "DIST" : (mode == 2 ? "TIME" : "SAMPLES");
    auto maxFor = [&ctx, &in] { return msToDelay (kMaxDelayMs, juce::jlimit (0, 3, (int) in.delayMode.load()), sampleRateOf (ctx)); };
    b.get = [&in, maxFor] { return juce::jlimit (0.0f, 1.0f, std::sqrt (in.delay.load() / maxFor())); };
    b.set = [&in, maxFor] (float p) { in.delay = p * p * maxFor(); };
    b.text = [&in] { return delayText (in.delay.load(), juce::jlimit (0, 3, (int) in.delayMode.load())); };
    b.reset = [&in] { in.delay = 0.0f; };
    return b;
}

/** A vertical delay slider: a dark track with a scale and the value in a tag. */
class DelaySlider final : public juce::Component, private juce::Timer
{
public:
    DelaySlider (UiContext& c, InputParams& p) : ctx (c), in (p) { startTimerHz (12); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const auto b = delayBinding (ctx, in);
        const float tagH = juce::jlimit (16.0f, screenCap (22.0f), r.getHeight() * 0.08f);
        auto scale = r.removeFromLeft (r.getWidth() * 0.42f);
        auto track = r.withSizeKeepingCentre (juce::jlimit (8.0f, screenCap (22.0f), r.getWidth() * 0.4f), r.getHeight() - tagH);
        const int mode = juce::jlimit (0, 3, (int) in.delayMode.load());
        const float max = msToDelay (kMaxDelayMs, mode, sampleRateOf (ctx));
        g.setFont (homeFont (TypeScale::caption, juce::jlimit (9.0f, screenCap (12.0f), r.getHeight() * 0.045f)));
        g.setColour (Colours::textDim);
        for (float frac : { 1.0f, 0.5f, 0.25f, 0.1f, 0.02f, 0.0f })
        {
            const float y = track.getBottom() - track.getHeight() * std::sqrt (frac);
            g.drawText (juce::String (max * frac, mode == 3 || max * frac >= 10.0f ? 0 : 1),
                        scale.withY (y - 6.0f).withHeight (12.0f).toNearestInt(), juce::Justification::centredRight);
        }
        g.setColour (colours::well);
        g.fillRect (track);
        const float pos = b.get();
        const float y = track.getBottom() - track.getHeight() * pos;
        g.setColour (in.delayOn.load() ? colours::fader : juce::Colour (0xff3a4a5a));
        g.fillRect (track.withTop (y).reduced (track.getWidth() * 0.25f, 0.0f));
        auto tag = juce::Rectangle<float> (juce::jmin ((float) getWidth(), juce::jmax (44.0f, tagH * 3.0f)), tagH).withCentre ({ track.getCentreX(), juce::jmin (y, track.getBottom() - tagH * 0.5f) });
        tag.setX (juce::jlimit (0.0f, (float) getWidth() - tag.getWidth(), tag.getX()));
        g.setColour (Colours::text);
        g.fillRect (tag);
        g.setColour (juce::Colours::black);
        g.setFont (homeFont (TypeScale::body, tagH * 0.75f, WingWeight::SemiBold));
        g.drawFittedText (b.text(), tag.toNearestInt(), juce::Justification::centred, 1, 0.7f);
    }

    void mouseDown (const juce::MouseEvent&) override { start = delayBinding (ctx, in).get(); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        delayBinding (ctx, in).set (juce::jlimit (0.0f, 1.0f, start - (float) e.getDistanceFromDragStartY() / juce::jmax (60.0f, (float) getHeight()) * (e.mods.isShiftDown() ? 0.2f : 1.0f)));
        repaint();
    }
    void mouseDoubleClick (const juce::MouseEvent&) override { in.delay = 0.0f; }

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
            auto* k = units.add (new IconKey (names[m], std::nullopt, colours::fader));
            k->twoLines = true;
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
        bindKey (delayKey, in.delayOn);
        addAndMakeVisible (delayKey);
        startTimerHz (5);
        timerCallback();
    }

    std::function<void()> onUnitChanged;
    juce::Component* getSlider() noexcept { return &slider; }

    void resized() override
    {
        auto b = getLocalBounds();
        auto right = b.removeFromRight (b.getWidth() * 45 / 100);
        slider.setBounds (b.reduced (0, 2));
        delayKey.setBounds (right.removeFromBottom (juce::jlimit (24, screenCap (46), right.getHeight() / 5)).reduced (4, 0));
        right.removeFromBottom (6);
        const int h = juce::jmin (screenCap (46), right.getHeight() / 4);
        for (auto* k : units)
            k->setBounds (right.removeFromTop (h).reduced (4, 2));
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
        for (int db = 18; db >= -12; db -= 6)
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
        g.setColour (juce::Colour (0xff686868));
        g.fillRect (p.getRight(), y - lw * 0.5f, r.getRight() - p.getRight(), lw);
        g.setColour (juce::Colours::white);
        g.fillRect (p.getX() - 4.0f * k, y - lw * 0.5f, p.getWidth() + 4.0f * k, lw);

        // The handles: a white disc with three lines, a blue one with three bars.
        const float hd = 38.0f * k;
        auto handle = juce::Rectangle<float> (hd, hd).withCentre ({ x, y });
        g.setColour (juce::Colours::white);
        g.fillEllipse (handle);
        g.setColour (juce::Colours::black);
        for (int i = -1; i <= 1; ++i)
            g.fillRect (juce::Rectangle<float> (hd * 0.5f, juce::jmax (1.0f, 2.0f * k)).withCentre ({ x, y + (float) i * hd * 0.16f }));
        auto balHandle = juce::Rectangle<float> (hd, hd).withCentre ({ x, tickY });
        g.setColour (hasBalance() ? colours::fader : juce::Colour (0xff3a4a5a));
        g.fillEllipse (balHandle);
        g.setColour (juce::Colours::white);
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
    std::function<bool()> hasBalance; // a stereo strip (the ST key can change it while shown)
    bool balanceOnly = false;
    float startTrim = 0.0f, startBal = 0.0f;
    std::tuple<float, float, bool> shown { 1.0e9f, 0.0f, false };
};

/** "IN 3", "PLAY 5" or "NONE" for a channel input index (stereo: "IN 3/4"). */
juce::String inputSourceText (int left, int right, bool stereo)
{
    if (left < 0)
        return "NONE";
    const bool playback = Recorder::isPlaybackSource (left);
    const bool usbPlayer = UsbRecorder::isPlayerSource (left);
    const bool osc = MixerEngine::isOscSource (left);
    auto number = [playback, usbPlayer, osc] (int source)
    {
        return juce::String ((osc         ? MixerEngine::oscChannelOf (source)
                              : usbPlayer ? UsbRecorder::playerChannelOf (source)
                              : playback  ? Recorder::playbackChannelOf (source)
                                          : source) + 1);
    };
    const auto prefix = juce::String (osc ? "OSC " : usbPlayer ? "PLAYER " : playback ? "PLAY " : "IN ");
    if (stereo)
        return prefix + number (left) + "/" + number (right >= 0 ? right : left + 1);
    return prefix + number (left);
}

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
        items.add ("IN " + juce::String (i + 1) + (i < names.size() && names[i].isNotEmpty() ? "  " + names[i].toUpperCase() : juce::String()));
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
} // namespace

KnobBinding inputDelayBinding (UiContext& ctx, InputParams& in) { return delayBinding (ctx, in); }

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
    juce::String name = index >= 0 ? engine.getSourceName (index) : juce::String();
    if (name.isEmpty())
        name = left < 0 ? juce::String ("NONE") : port;
    const int colourIndex = src != nullptr ? src->colour.load() : 1;
    const auto colour = left < 0 ? colours::chipOff : (playback && colourIndex == 1 ? juce::Colour (0xff2f5a32) : wingColour (colourIndex));
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
    g.setColour (left < 0 ? colours::dim : (stereo ? colours::stereo : colours::mono));
    g.setFont (homeFont (TypeScale::body, label.getHeight() * 0.72f));
    g.drawFittedText (port, label.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.7f);

    auto body = r.reduced (3.0f, 2.0f);
    auto side = roomy ? body.removeFromRight (body.getWidth() * 0.34f) : juce::Rectangle<float>();
    const float iconSide = juce::jmin (body.getWidth(), body.getHeight()) * 0.86f;
    auto iconArea = juce::Rectangle<float> (iconSide, iconSide).withCentre (body.getCentre());
    const int icon = src != nullptr ? src->icon.load() : 0;
    const auto ink = colours::caption.withAlpha (left < 0 ? 0.4f : 1.0f);
    if (icon <= 0 || ! drawWingIcon (g, icon, iconArea, ink))
        drawIcon (g, MixerEngine::isOscSource (left) ? Icon::Sine : playback ? Icon::Waveform : Icon::Xlr, iconArea, ink);

    if (left < 0 || ! roomy)
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
            g.setColour (juce::Colour (0xff808080));
            g.drawRect (r, 2.0f);
        }
        if (alt)
            return;

        const auto* src = inputSourceParams (ctx.engine, left);
        const bool phantom = src != nullptr && src->phantom.load();
        const float alpha = src != nullptr ? 1.0f : 0.4f;
        auto k = phantomKey.toFloat();
        g.setColour (phantom ? colours::mute : colours::keyOff);
        g.fillRect (k);
        drawIcon (g, Icon::Power, k.reduced (k.getWidth() * 0.22f), (phantom ? colours::ink : colours::caption).withAlpha (alpha));
        g.setColour (colours::dim.withAlpha (alpha));
        g.setFont (homeFont (TypeScale::caption, (float) phantomCaption.getHeight()));
        g.drawText ("+48V", phantomCaption, juce::Justification::centred);

        // GAIN (-12 .. +60 dB): a dark slot with the white value tag [D16].
        auto f = gainSlider.toFloat();
        g.setColour (colours::well);
        g.fillRect (f);
        g.setColour (colours::outline);
        g.drawRect (f, 1.0f);
        const float gain = src != nullptr ? src->gainDb.load() : 0.0f;
        const float tagH = tagHeight();
        const float pos = juce::jlimit (0.0f, 1.0f, (gain + 12.0f) / 72.0f);
        const float y = f.getBottom() - tagH * 0.5f - 2.0f - (f.getHeight() - tagH - 4.0f) * pos;
        auto tag = juce::Rectangle<float> (juce::jmin (f.getWidth() + 18.0f, (float) getWidth() - 8.0f), tagH).withCentre ({ f.getCentreX(), y });
        g.setColour (juce::Colours::white.withAlpha (alpha));
        g.fillRect (tag);
        g.setColour (juce::Colours::black);
        g.setFont (homeFont (TypeScale::body, tagH * 0.8f, WingWeight::SemiBold));
        g.drawFittedText (juce::String (gain, 1) + " dB", tag.toNearestInt(), juce::Justification::centred, 1, 0.6f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        draggingGain = ! alt && gainSlider.expanded (10, 4).contains (e.getPosition());
        if (draggingGain)
            setGainFromY (e.y);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (draggingGain)
            setGainFromY (e.y);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (draggingGain || ! getLocalBounds().contains (e.getPosition()))
            return;
        if (! alt && phantomKey.contains (e.getPosition()))
        {
            const auto* src = inputSourceParams (ctx.engine, strip.getInputLeft());
            const bool on = src != nullptr && src->phantom.load();
            forInputSource (ctx.engine, strip.getInputLeft(), [on] (ld::SourceParams& s) { s.phantom = ! on; });
            repaint();
            return;
        }
        if (! tile.contains (e.getPosition()))
            return;
        if (! alt)
        {
            if (ctx.showScreen)
                ctx.showScreen (Screen::Routing);
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

private:
    static float tagHeight() { return juce::jlimit (12.0f, screenCap (20.0f), getWingScreenHeight() * 0.036f); }

    void setGainFromY (int y)
    {
        auto f = gainSlider.toFloat();
        const float tagH = tagHeight();
        const float span = juce::jmax (1.0f, f.getHeight() - tagH - 4.0f);
        const float pos = juce::jlimit (0.0f, 1.0f, (f.getBottom() - tagH * 0.5f - 2.0f - (float) y) / span);
        const float gain = std::round ((pos * 72.0f - 12.0f) * 2.0f) / 2.0f; // 0.5 dB steps
        forInputSource (ctx.engine, strip.getInputLeft(), [gain] (ld::SourceParams& s) { s.gainDb = gain; });
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
};

/** Draws a bracket over the MAIN / ALT keys (Fig. 62). */
class Bracket final : public juce::Component
{
public:
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        juce::Path p;
        p.startNewSubPath (r.getX(), r.getBottom());
        p.lineTo (r.getX(), r.getY());
        p.lineTo (r.getX() + r.getWidth() * 0.2f, r.getY());
        p.startNewSubPath (r.getRight() - r.getWidth() * 0.2f, r.getY());
        p.lineTo (r.getRight(), r.getY());
        p.lineTo (r.getRight(), r.getBottom());
        g.setColour (Colours::textDim);
        g.strokePath (p, juce::PathStrokeType (2.0f));
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
        addAndMakeVisible (trimMeter);
        invert = &key ("INVERT", Icon::Invert, colours::accent);
        invert->iconAbove = true;
        bindKey (*invert, p.input.invert);

        if (strip.isInput())
        {
            bracket = &make<Bracket>();
            // MAIN / ALT: the channel's own switch, or the console's global one
            // when INPUT SELECT is GLOBAL.
            auto& engine = ctx.engine;
            inputSel = &make<Segmented> (juce::StringArray { "MAIN", "ALT" },
                                         [this, &engine] { return (engine.isInputSelectGlobal() ? engine.isGlobalAlt() : strip.isAltSelected()) ? 1 : 0; },
                                         [this, &engine] (int i)
                                         {
                                             if (engine.isInputSelectGlobal())
                                                 engine.setGlobalAlt (i == 1);
                                             else
                                                 strip.setAltSelected (i == 1);
                                         });
            // 3.1: MAIN lights blue, ALT orange.
            inputSel->colourAt = [] (int i) { return i == 1 ? colours::alt : colours::fader; };
            mainTile = &make<InputSourcePanel> (ctx, strip, false);
            altTile = &make<InputSourcePanel> (ctx, strip, true);
            inputSelectLabel = &key ("INPUT SELECT");
            inputSelectLabel->setTooltip ("The console's INPUT SELECT (SETUP > AUDIO)");
            // A title bar, not a key [I7].
            inputSelectLabel->offColour = colours::panel;
            inputSelectLabel->offInk = colours::caption;
            inputSelectLabel->frame = colours::panel;
            inputSelectLabel->onClick = [this] { if (ctx.showScreen) ctx.showScreen (Screen::Setup); };
            selectMode = &make<Segmented> (juce::StringArray { "INDIVIDUAL", "GLOBAL" }, [&engine] { return engine.isInputSelectGlobal() ? 1 : 0; },
                                           [&engine] (int i) { engine.setInputSelectGlobal (i == 1); });
            // INDIVIDUAL lights blue, GLOBAL gold (1.11's MANUAL | AUTO were orange).
            selectMode->colourAt = [] (int i) { return i == 1 ? colours::accent : colours::fader; };
            selectMode->vertical = true;
            stereo = &key ("ST", std::nullopt, colours::stereo);
            stereo->setTooltip ("Stereo channel (two inputs)");
            bindKey (*stereo, p.stereo);
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
            loCut->iconAbove = true;
            bindKey (*loCut, p.filter.lowCut);
            hiCut = &key ("HICUT", Icon::HighCut, colours::eqOn);
            hiCut->iconAbove = true;
            bindKey (*hiCut, p.filter.highCut);
            tool = &key (toolFilterName (p.filter.toolModel.load()), toolFilterIcon (p.filter.toolModel.load()), toolFilterColour (p.filter.toolModel.load()));
            tool->iconAbove = true;
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
            // engine E11 would add AES/EBU, phones, speakers and solo bus sources.
            directOn = &key ("ON", std::nullopt, colours::accent);
            bindKey (*directOn, p.directOn);
            directSource = &field ("SOURCE", channelNames (ctx.engine), p.directSource, "NONE");
            directInvert = &key (juce::String::fromUTF8 ("\xc3\x98 INVERT"), std::nullopt, colours::accent);
            bindKey (*directInvert, p.directInvert);
            directPre = &key ("PRE", std::nullopt, colours::accent);
            bindKey (*directPre, p.directPre);
            directLevel = &make<KnobBox> (wingFader ("LEVEL", p.directLevelDb));
        }
    }

    std::vector<HomeKnob> knobs() override
    {
        auto& p = strip.params;
        auto bal = bindLinear ("BAL", p.input.balanceDb, -9.0f, 9.0f, " dB", 1, 0.0f);
        std::vector<HomeKnob> k { {}, { wingTrim ("TRIM", p.input.trimDb), thumbStyle (pad) }, {}, { bal, balStyle (pad) }, {} };
        if (delay != nullptr)
            k.push_back ({ delayBinding (ctx, p.input), distStyle (delay->getSlider()) });
        return k;
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        // Fig. 62: CHANNEL INPUT about 33 % of the width, TRIM & BALANCE 41 %,
        // FILTER over DELAY (POST) 26 %.
        auto right = b.removeFromRight (b.getWidth() * 26 / 100);
        auto left = b.removeFromLeft (b.getWidth() * 44 / 100);
        auto middle = b;

        if (strip.isInput())
        {
            // [3.1, D16] the MAIN / ALT keys under a bracket, then two columns:
            // the framed MAIN column (source tile, +48V, GAIN) and, at the right,
            // the ALT tile's panel, INPUT SELECT and INDIVIDUAL over GLOBAL.
            // WING pixels [I7]: the key row's box 80 tall, then the 192 px
            // columns with 8 px black gaps: the MAIN column; the ALT box (192),
            // the INPUT SELECT bar (64) and the INDIVIDUAL / GLOBAL box.
            box (left, "CHANNEL INPUT");
            auto ci = sectionBody (left, true);
            channelInputBody = ci;
            const int gap = juce::roundToInt (juce::jmax (3.0f, wingPx (8.0f)));
            const float v = (float) ci.getHeight() / 600.0f;
            bracketBox = ci.removeFromTop (juce::roundToInt (80.0f * v));
            ci.removeFromTop (gap);
            auto keys = bracketBox.reduced (juce::roundToInt (bracketBox.getWidth() * 0.04f), juce::roundToInt (12.0f * v));
            bracket->setBounds (keys.withTrimmedBottom (keys.getHeight() / 3));
            inputSel->setBounds (keys.withSizeKeepingCentre (juce::roundToInt (bracketBox.getWidth() * 0.61f), keys.getHeight()));
            auto mainCol = ci.removeFromLeft ((ci.getWidth() - gap) / 2);
            ci.removeFromLeft (gap);
            mainTile->setBounds (mainCol);
            altTile->setBounds (ci.removeFromTop (juce::roundToInt (192.0f * v)));
            ci.removeFromTop (gap);
            const int selTop = ci.getBottom() - juce::roundToInt (248.0f * v);
            stereo->setBounds (juce::Rectangle<int> (ci.getX(), ci.getY(), ci.getWidth(), juce::jmax (0, selTop - ci.getY() - gap))
                                   .withSizeKeepingCentre (juce::jmin (juce::roundToInt (wingPx (64.0f)), ci.getWidth() / 3), juce::roundToInt (wingPx (48.0f))));
            ci.setTop (selTop);
            inputSelectLabel->setBounds (ci.removeFromTop (juce::roundToInt (64.0f * v)));
            ci.removeFromTop (gap);
            modesBox = ci;
            const int kh = juce::roundToInt (56.0f * v);
            selectMode->setBounds (ci.withSizeKeepingCentre (juce::roundToInt (ci.getWidth() * 0.58f), kh * 2));
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
                row.removeFromLeft (6);
                directPre->setBounds (row);
                d.removeFromTop (6);
                directLevel->setBounds (d.removeFromTop (juce::jmin (screenCap (48), d.getHeight())).withWidth (juce::jmin (d.getWidth(), screenCap (150))));
            }
        }

        auto tb = box (middle, "TRIM & BALANCE");
        auto meterCol = tb.removeFromRight (juce::jmax (54, tb.getWidth() * 24 / 100));
        invert->setBounds (meterCol.removeFromBottom (juce::jlimit (30, screenCap (52), meterCol.getHeight() / 6)).reduced (2, 0));
        if (mono != nullptr)
        {
            meterCol.removeFromBottom (4);
            mono->setBounds (meterCol.removeFromBottom (juce::jlimit (22, screenCap (32), meterCol.getHeight() / 7)).reduced (2, 0));
        }
        trimMeter.setBounds (meterCol.reduced (2, 4));
        tb.removeFromRight (4);
        pad->setBounds (tb);

        if (loCut != nullptr)
        {
            auto f = box (right.removeFromTop (juce::jmax (80, right.getHeight() * 30 / 100)), "FILTER", true);
            filterHeader = f.withY (f.getY() - panelHeaderHeight() - 4).withHeight (panelHeaderHeight());
            layoutRow (f, { loCut, hiCut, tool }, 4);
        }
        if (delay != nullptr)
        {
            delay->setBounds (box (right, strip.hasPostFaderDelay() ? "DELAY (POST)" : "DELAY"));
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
        g.fillRect (modesBox);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (filterHeader.contains (e.getPosition()) && ctx.showHomeTab)
            ctx.showHomeTab (HomeTab::Filter);
    }

private:
    juce::Rectangle<int> channelInputBody, bracketBox, modesBox;
    TrimBalancePad* pad = nullptr;
    LedMeter trimMeter;
    IconKey *invert = nullptr, *stereo = nullptr, *mono = nullptr, *inputSelectLabel = nullptr, *feed = nullptr;
    FeedGrid* feedGrid = nullptr;
    IconKey *loCut = nullptr, *hiCut = nullptr, *tool = nullptr;
    IconKey *directOn = nullptr, *directInvert = nullptr, *directPre = nullptr;
    ChoiceField* directSource = nullptr;
    KnobBox* directLevel = nullptr;
    Segmented *inputSel = nullptr, *selectMode = nullptr;
    Bracket* bracket = nullptr;
    InputSourcePanel *mainTile = nullptr, *altTile = nullptr;
    DelayPanel* delay = nullptr;
    juce::Rectangle<int> filterHeader;
};

//==============================================================================
/** FILTER SETUP (Fig. 63): the header with FREEZE, LO CUT, HI CUT and the tool
    filter's key and CLOSE; the graph with the LC / HC lines; the TOOL FILTER
    list and the slopes. */
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
        // On the header bar without a key body [Fig. 63].
        for (auto* k : { loCut, hiCut, toolKey })
        {
            k->offColour = colours::panel;
            k->frame = colours::panel;
        }
        close = &key ("CLOSE", Icon::Check);
        close->offInk = colours::caption;
        close->onClick = [this] { if (ctx.showHomeTab) ctx.showHomeTab (HomeTab::Home); };

        graph = &make<ResponseGraph>();
        graph->setStrip (&strip, ResponseGraph::Filter);
        graph->setRuler (ResponseGraph::Ruler::Top);

        toolCaption = &label ("TOOL FILTER", 13.0f, colours::caption);
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
        lcCaption = &label ("LC", 13.0f, colours::caption);
        lcCaption->setJustificationType (juce::Justification::centred);
        lcSlopes = &make<Segmented> (models::list (models::LowCutSlopes), [&f] { return f.lowCutSlope.load(); }, [&f] (int i) { f.lowCutSlope = i; });
        lcSlopes->vertical = true;
        hcCaption = &label ("HC", 13.0f, colours::caption);
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
        std::vector<HomeKnob> k { { wingFreq ("LC FREQ", f.lowCutFreq, 20.0f, 2000.0f, 80.0f), freqStyle (graph) } };
        // The tool filter's parameters (TILT...) in the middle, HC FREQ always on the 6th knob.
        const auto& spec = models::model (models::FilterTools, f.toolModel.load());
        const auto colour = toolFilterColour (f.toolModel.load());
        for (int i = 0; i < (int) spec.params.size() && i < 4; ++i)
            k.push_back ({ bindConsoleParam (f.toolParams[i], spec.params[(size_t) i]), toolStyle (graph, colour) });
        while (k.size() < 5)
            k.push_back ({});
        k.push_back ({ wingFreq ("HC FREQ", f.highCutFreq, 50.0f, 20000.0f, 10000.0f), freqStyle (graph) });
        return k;
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        // Fig. 63: the title at the left, FREEZE, the LO CUT / HI CUT / tool
        // keys (icons over captions) and CLOSE at the right.
        header = b.removeFromTop (juce::jlimit (32, screenCap (60), b.getHeight() / 9));
        const float u = (float) header.getWidth() / 1160.0f;
        auto h = header.reduced (0, juce::jmax (3, header.getHeight() / 9));
        auto span = [&h, u, this] (float x0, float x1)
        { return juce::Rectangle<int> (header.getX() + juce::roundToInt (x0 * u), h.getY(), juce::roundToInt ((x1 - x0) * u), h.getHeight()); };
        freeze->setBounds (span (484.0f, 592.0f));
        loCut->setBounds (span (612.0f, 692.0f));
        hiCut->setBounds (span (700.0f, 780.0f));
        toolKey->setBounds (span (788.0f, 868.0f));
        close->setBounds (span (1004.0f, 1144.0f));
        b.removeFromTop (juce::jmax (4, getHeight() / 100));

        auto side = b.removeFromRight (juce::jmax (120, b.getWidth() * 22 / 100));
        b.removeFromRight (6);
        graph->setBounds (b);

        // [Fig. 63] TOOL FILTER over the four tool keys, then LC and HC side
        // by side, each a column of square slope keys.
        auto s = box (side, {});
        const int capH = juce::roundToInt (juce::jmax (14.0f, wingPx (26.0f)));
        const int gap = juce::roundToInt (juce::jmax (2.0f, wingPx (6.0f)));
        toolCaption->setBounds (s.removeFromTop (capH));
        const int kh = juce::jlimit (18, juce::jmax (18, s.getHeight() / 9), juce::roundToInt (wingPx (52.0f)));
        for (auto* k : tools)
        {
            k->setBounds (s.removeFromTop (kh));
            s.removeFromTop (gap);
        }
        s.removeFromTop (gap * 2);
        auto captions = s.removeFromTop (capH);
        const int colW = juce::jmin ((s.getWidth() - gap) / 2, kh);
        const int sq = juce::jmin (colW, (s.getHeight() - 3 * gap) / 4);
        auto lcCol = juce::Rectangle<int> (s.getCentreX() - gap / 2 - colW, s.getY(), colW, s.getHeight());
        auto hcCol = juce::Rectangle<int> (s.getCentreX() + gap / 2 + (gap % 2), s.getY(), colW, s.getHeight());
        lcCaption->setBounds (lcCol.withY (captions.getY()).withHeight (capH));
        hcCaption->setBounds (hcCol.withY (captions.getY()).withHeight (capH));
        lcSlopes->setBounds (lcCol.withHeight (sq * 4 + gap * 3).withSizeKeepingCentre (sq, sq * 4 + gap * 3));
        hcSlopes->setBounds (hcCol.withHeight (sq * 2 + gap).withSizeKeepingCentre (sq, sq * 2 + gap));
    }

    void paint (juce::Graphics& g) override
    {
        HomeScreen::paint (g);
        auto h = header.toFloat();
        g.setColour (colours::panel);
        g.fillRect (h);
        g.setColour (colours::caption);
        g.setFont (panelTitleFont (header.getHeight()));
        g.drawFittedText ("FILTER SETUP", h.withTrimmedLeft (h.getWidth() * 0.03f).withWidth (h.getWidth() * 0.36f).toNearestInt(), juce::Justification::centredLeft, 1);
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
            notifyKnobs();
        }
    }

    IconKey *freeze = nullptr, *loCut = nullptr, *hiCut = nullptr, *toolKey = nullptr, *close = nullptr;
    std::vector<IconKey*> tools;
    juce::Label *toolCaption = nullptr, *lcCaption = nullptr, *hcCaption = nullptr;
    Segmented *lcSlopes = nullptr, *hcSlopes = nullptr;
    ResponseGraph* graph = nullptr;
    juce::Rectangle<int> header;
    int shownTool = -1;
};

//==============================================================================
/** GATE or COMP slot: the model, its front panel, KEY (and XOVER) and the
    output gain and mix. The model's main parameters are on the six knobs. */
class DynTab final : public HomeScreen, private juce::Timer
{
public:
    /** GATE (manual Fig. 94) or COMP (Fig. 92, 93), firmware 3.1: two rows of
        controls over the model. Row 1: ON, the model field, MIX (COMP), KEY
        TAP and KEY SOURCE at the right. Row 2: GAIN (COMP) or ACCENT (the
        gate/expander), XOVER SOLO and XOVER MODE (COMP), KEY SOLO and KEY
        FILTER; their ">" keys open FREQ / Q (/ DEPTH) under the row. The
        WING's own models show the transfer graph with the ENVELOPE panel and
        the DETECTOR / ENVELOPE / AUTO ENV keys; the others their front panel. */
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
        for (auto [id, caption, names] : { std::tuple { "det", "DETECTOR", juce::StringArray { "PEAK", "RMS" } },
                                           std::tuple { "env", "ENVELOPE", juce::StringArray { "LIN", "LOG" } },
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
                kSegmentOn);
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

        keySource = &field ("KEY SOURCE", keySourceNames (ctx.engine), slot.scSource, "SELF");
        keySource->arrowKey = true;
        keyFilter = &field ("KEY FILTER", models::list (models::SidechainFilters), slot.scFilter);
        keyFilter->chevronKey = true;
        keyFilter->onKey = [this] { toggleDetail (Detail::Key); };
        keyFilter->keyLit = [this] { return detail == Detail::Key; };
        keyTap = &field ("KEY TAP", tapPointNames(), slot.scTap);
        // Not in the engine yet: another channel keys from its input (the setting is only stored).
        keyTap->setEnabled (false);
        keyFreq = &make<KnobBox> (wingFreq ("KEY FREQ", slot.scFreq, 20.0f, 20000.0f, 1000.0f));
        keyQ = &make<KnobBox> (bindLog ("KEY Q", slot.scQ, 0.44f, 10.0f, {}, 1.0f));
        // The key signal after the key filter on the solo bus.
        keySolo = &key ("KEY SOLO");
        keySolo->outlined = true;
        bindListen (*keySolo, strip, isGate ? ld::kListenGateKey : ld::kListenDynKey);

        if (! isGate)
        {
            xoMode = &field ("XOVER MODE", models::list (models::XoverTypes), slot.xoKind);
            xoMode->chevronKey = true;
            xoMode->onKey = [this] { toggleDetail (Detail::Xover); };
            xoMode->keyLit = [this] { return detail == Detail::Xover; };
            xoFreq = &make<KnobBox> (wingFreq ("XO FREQ", slot.xoFreq, 20.0f, 20000.0f, 1000.0f));
            xoDepth = &make<KnobBox> (bindLinear ("DEPTH", slot.xoDepth, -30.0f, 30.0f, " dB", 1, 0.0f));
            xoQ = &make<KnobBox> (bindLog ("XO Q", slot.xoQ, 0.4f, 4.0f, {}, 1.0f));
            xoSolo = &key ("XOVER SOLO");
            xoSolo->outlined = true;
            bindListen (*xoSolo, strip, ld::kListenXover); // the crossover band on the solo bus
        }
        bindModel();
        startTimerHz (8);
    }

    std::vector<HomeKnob> knobs() override
    {
        std::vector<HomeKnob> k;
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
        auto b = getLocalBounds();
        const float u = (float) b.getWidth() / 1160.0f;
        auto x = [u, &b] (float px) { return b.getX() + juce::roundToInt (px * u); };
        const int rowH = juce::jlimit (28, screenCap (64), b.getHeight() * 9 / 100);
        const int gap = juce::jmax (4, b.getHeight() / 90);
        auto span = [&x] (juce::Rectangle<int> row, float x0, float x1) { return juce::Rectangle<int> (x (x0), row.getY(), x (x1) - x (x0), row.getHeight()); };

        // Row 1 [Fig. 92, 94].
        b.removeFromTop (gap);
        row1 = b.removeFromTop (rowH);
        on->setBounds (span (row1, 16.0f, 110.0f).reduced (0, rowH / 8));
        model->setBounds (span (row1, 126.0f, 436.0f));
        mix->setBounds (span (row1, 448.0f, 616.0f));
        keyTap->setBounds (span (row1, 744.0f, 932.0f));
        keySource->setBounds (span (row1, 944.0f, 1144.0f));

        // Row 2.
        b.removeFromTop (gap);
        row2 = b.removeFromTop (rowH);
        gain->setBounds (span (row2, 16.0f, 216.0f));
        accent->setBounds (span (row2, 16.0f, 216.0f));
        if (xoSolo != nullptr)
        {
            xoSolo->setBounds (span (row2, 470.0f, 588.0f).reduced (0, rowH / 10));
            xoMode->setBounds (span (row2, 600.0f, 830.0f));
        }
        keySolo->setBounds (span (row2, 842.0f, 960.0f).reduced (0, rowH / 10));
        keyFilter->setBounds (span (row2, 972.0f, 1144.0f));

        // The ">" keys' settings under row 2.
        const bool peak = models::list (models::XoverTypes)[slot.xoKind.load()].containsIgnoreCase ("PEAK");
        const bool keyQShown = slot.scFilter.load() >= 3; // BP, NOTCH
        for (auto* c : { (juce::Component*) keyFreq, (juce::Component*) keyQ, (juce::Component*) xoFreq, (juce::Component*) xoDepth, (juce::Component*) xoQ })
            if (c != nullptr)
                c->setVisible (false);
        detailRow = {};
        if (detail != Detail::None)
        {
            b.removeFromTop (gap);
            detailRow = b.removeFromTop (rowH);
            std::vector<juce::Component*> shown;
            if (detail == Detail::Key)
                shown = keyQShown ? std::vector<juce::Component*> { keyFreq, keyQ } : std::vector<juce::Component*> { keyFreq };
            else if (xoFreq != nullptr)
                shown = peak ? std::vector<juce::Component*> { xoFreq, xoDepth, xoQ } : std::vector<juce::Component*> { xoFreq, xoDepth };
            float right = 1144.0f;
            for (auto it = shown.rbegin(); it != shown.rend(); ++it)
            {
                (*it)->setVisible (true);
                (*it)->setBounds (span (detailRow, right - 176.0f, right));
                right -= 188.0f;
            }
        }

        // The model.
        b.removeFromTop (gap);
        body = b.withTrimmedBottom (gap).withLeft (x (16.0f)).withRight (x (1144.0f));
        const bool native = hasTransferGraph (shownModel);
        graph->setVisible (native);
        envelope->setVisible (native);
        face->setVisible (! native);
        for (auto& t : toggles)
            t.keys->setVisible (false);
        togglePanel = {};
        if (! native)
        {
            face->setBounds (body);
            return;
        }
        auto left = body.withWidth (juce::roundToInt ((float) body.getWidth() * 0.485f));
        graph->setBounds (left);
        auto right = body.withTrimmedLeft (left.getWidth() + juce::jmax (4, juce::roundToInt (8.0f * u)));
        const auto& spec = models::model (models::Dynamics, shownModel);
        auto envArea = right.removeFromTop (right.getHeight() * 68 / 100);
        envelope->setBounds (envArea);
        right.removeFromTop (juce::jmax (4, juce::roundToInt (8.0f * u)));
        togglePanel = right;
        std::vector<Toggle*> present;
        for (auto& t : toggles)
            if (spec.find (t.paramId) >= 0)
                present.push_back (&t);
        if (present.empty())
            return;
        const int cellW = togglePanel.getWidth() / 3;
        const int keyH = juce::jlimit (20, screenCap (52), togglePanel.getHeight() * 36 / 100);
        const int keyW = juce::jmin (cellW * 3 / 4, keyH * 3);
        // DETECTOR left, AUTO ENV right, ENVELOPE in the middle [Fig. 92; 93 without ENVELOPE in the 3.x list].
        for (auto* t : present)
        {
            const int column = t->paramId == "det" ? 0 : (t->paramId == "env" ? 1 : 2);
            auto cell = juce::Rectangle<int> (togglePanel.getX() + column * cellW, togglePanel.getY(), cellW, togglePanel.getHeight());
            t->caption = cell.withTrimmedTop (cell.getHeight() * 22 / 100).withHeight (cell.getHeight() * 20 / 100);
            t->keys->setBounds (juce::Rectangle<int> (keyW, keyH).withCentre ({ cell.getCentreX(), cell.getY() + cell.getHeight() * 64 / 100 }));
            t->keys->setVisible (true);
        }
    }

    void paint (juce::Graphics& g) override
    {
        HomeScreen::paint (g);
        if (! togglePanel.isEmpty())
        {
            g.setColour (Colours::screenPanel);
            g.fillRect (togglePanel);
            g.setColour (Colours::text);
            g.setFont (wingFontPx (TypeScale::body * getWingScreenHeight()));
            for (auto& t : toggles)
                if (t.keys->isVisible())
                    g.drawText (t.label, t.caption, juce::Justification::centred, false);
        }
    }

private:
    enum class Detail
    {
        None,
        Key,
        Xover
    };

    struct Toggle
    {
        SegmentedKeys* keys;
        juce::String label, paramId;
        juce::Rectangle<int> caption {};
    };

    // The lit choice of DETECTOR / ENVELOPE / AUTO ENV (3.x: orange).
    static inline const juce::Colour kSegmentOn { 0xfff5a00a };

    bool isGate;
    DynSlotParams& slot;
    IconKey *on = nullptr, *keySolo = nullptr, *xoSolo = nullptr;
    ChoiceField* model = nullptr;
    juce::StringArray allowed;
    KnobBox *mix = nullptr, *gain = nullptr, *accent = nullptr, *keyFreq = nullptr, *keyQ = nullptr, *xoFreq = nullptr, *xoDepth = nullptr,
            *xoQ = nullptr;
    DynTransferGraph* graph = nullptr;
    DynEnvelopeGraph* envelope = nullptr;
    std::vector<Toggle> toggles;
    Faceplate* face = nullptr;
    ChoiceField *keySource = nullptr, *keyFilter = nullptr, *keyTap = nullptr, *xoMode = nullptr;
    juce::Rectangle<int> row1, row2, detailRow, body, togglePanel;
    int shownModel = -1, shownXo = -1, shownKeyFilter = -1, knobPage = 0;
    Detail detail = Detail::None;

    /** "SELF" is the field's none text; the channels as the console lists them (CH.1 ...). */
    static juce::StringArray keySourceNames (MixerEngine& engine)
    {
        juce::StringArray items;
        for (int i = 0; i < (int) engine.getChannels().size(); ++i)
            items.add ("CH." + juce::String (i + 1));
        return items;
    }

    void toggleDetail (Detail d)
    {
        detail = detail == d ? Detail::None : d;
        resized();
        repaint();
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
        face->bind (models::Dynamics, shownModel, slot.values,
                    [this] { return -(isGate ? strip.meters.gateReductionDb.load() : strip.meters.dynReductionDb.load()); });
        const auto& spec = models::model (models::Dynamics, shownModel);
        const int acc = spec.find ("acc");
        accent->setBinding (acc >= 0 ? bindConsoleParam (slot.values[acc], spec.params[(size_t) acc]) : KnobBinding {});
        accent->setVisible (isGate && acc >= 0);
        knobPage = 0;
        notifyKnobs();
        resized();
        repaint();
    }

    void timerCallback() override
    {
        if (slot.model.load() != shownModel)
            bindModel();
        if (slot.xoKind.load() != shownXo || slot.scFilter.load() != shownKeyFilter)
        {
            shownXo = slot.xoKind.load();
            shownKeyFilter = slot.scFilter.load();
            resized();
        }
    }
};

//==============================================================================
/** EQ (firmware 3.1): the header with ON, the EQ MODEL field, the MIX knob
    box, FREEZE, LO CUT / HI CUT / FILTER on input channels and the tool
    filter's knob box at the right; a piano-key ruler over the graph; the WING
    EQ's band list (the cut / shelf bands with their icon, the PEQ bands by
    number; hold a key to reset its band) with BAND SOLO under it. An analog
    model shows its front panel under the graph instead. */
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
                                        ModelSelector::choose (field, table, strip.params.eq.model.load(),
                                                               [this] (int m) { strip.setModel (ModelBlock::Eq, m); }, allowed);
                                    });
        model->arrowKey = true;
        mix = &make<KnobBox> (bindLinear ("MIX", eq.mix, 0.0f, 125.0f, " %", 0, 100.0f));
        if (strip.getKind() == StripKind::Channel)
        {
            loCut = &key ({}, Icon::LowCut, colours::eqOn);
            loCut->setTooltip ("LO CUT");
            bindKey (*loCut, strip.params.filter.lowCut);
            hiCut = &key ({}, Icon::HighCut, colours::eqOn);
            hiCut->setTooltip ("HI CUT");
            bindKey (*hiCut, strip.params.filter.highCut);
            filter = &key ("FILTER", std::nullopt);
            filter->onClick = [this] { if (ctx.showHomeTab) ctx.showHomeTab (HomeTab::Filter); };
            // The tool filter's main control ("filter controls on EQ page", 3.0).
            auto& f = strip.params.filter;
            const auto& tool = models::model (models::FilterTools, f.toolModel.load());
            if (! tool.params.empty())
            {
                tilt = &make<KnobBox> (bindConsoleParam (f.toolParams[0], tool.params[0]));
                tilt->ringColour = toolFilterColour (f.toolModel.load());
                shownTool = f.toolModel.load();
            }
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
        k.push_back ({ b[0], thumbStyle (graph) });
        k.push_back ({ b[2], widthStyle (graph) });
        k.push_back ({ b[1], styleOf (KnobStyle::Bar::Thumb, KnobColours::blue, graph) });
        return k;
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        // The 3.1 header (WING pixels from the content's left edge, 1160 wide):
        // ON 16-112, EQ MODEL 128-432, MIX 448-612, FREEZE 628-736, the tool
        // filter's box 976-1152; LO CUT / HI CUT / FILTER fill the gap.
        header = b.removeFromTop (juce::jlimit (32, screenCap (72), b.getHeight() * 12 / 100));
        auto h = header.withSizeKeepingCentre (header.getWidth(), header.getHeight() * 4 / 5);
        const float u = (float) header.getWidth() / 1160.0f;
        auto span = [&h, u, this] (float x0, float x1)
        { return juce::Rectangle<int> (header.getX() + juce::roundToInt (x0 * u), h.getY(), juce::roundToInt ((x1 - x0) * u), h.getHeight()); };
        on->setBounds (span (16.0f, 112.0f).withSizeKeepingCentre (juce::roundToInt (96.0f * u), header.getHeight() * 3 / 5));
        model->setBounds (span (128.0f, 432.0f));
        mix->setBounds (span (448.0f, 612.0f));
        freeze->setBounds (span (628.0f, 736.0f));
        if (filter != nullptr)
        {
            auto keys = span (752.0f, 960.0f);
            // Square keys, narrower in a small window so FILTER keeps a third.
            const int kw = juce::jmin (keys.getHeight(), (keys.getWidth() - 8) / 3);
            loCut->setBounds (keys.removeFromLeft (kw));
            keys.removeFromLeft (4);
            hiCut->setBounds (keys.removeFromLeft (kw));
            keys.removeFromLeft (4);
            filter->setBounds (keys);
        }
        if (tilt != nullptr)
            tilt->setBounds (span (976.0f, 1152.0f));
        b.removeFromTop (juce::jmax (4, getHeight() / 100));

        const bool wingEq = ! bands.isEmpty();
        face->setVisible (! wingEq);

        // The band list at the right (980-1152), BAND SOLO under it.
        auto side = b.removeFromRight (juce::roundToInt (172.0f * u));
        b.removeFromRight (juce::roundToInt (20.0f * u));
        const int kh = juce::jlimit (22, screenCap (46), side.getHeight() / 9);
        bandSolo->setBounds (side.removeFromBottom (kh));
        side.removeFromBottom (juce::jmax (6, kh / 2));
        bandKeyArea = side;
        // [D10] 52 px keys 60 px apart.
        bandKeyGap = juce::roundToInt (juce::jmax (2.0f, wingPx (8.0f)));
        bandKeyH = juce::jlimit (16, juce::roundToInt (juce::jmax (16.0f, wingPx (52.0f))),
                                 bandKeyArea.getHeight() / juce::jmax (6, bands.size()) - bandKeyGap);

        if (! wingEq)
        {
            graph->setBounds (b.removeFromTop (b.getHeight() * 38 / 100));
            b.removeFromTop (4);
            face->setBounds (b);
            freeze->setVisible (true);
            return;
        }
        graph->setBounds (b);
    }

    void paint (juce::Graphics& g) override
    {
        HomeScreen::paint (g);
        g.setColour (colours::panel);
        g.fillRect (header);
        g.fillRect (bandKeyArea.getUnion (bandSolo->getBounds()).expanded (4));

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
            auto lead = k.removeFromLeft (k.getWidth() * 0.4f);
            juce::String type = "PEQ";
            if (id == "l" || id == "h")
            {
                if (const int t = spec.find (id + "eq"); t >= 0)
                    type = consoleText (spec.params[(size_t) t], strip.params.eq.values[t].load()).toUpperCase();
                const bool cut = type.contains ("CUT");
                const bool shelf = type.contains ("SH");
                const auto icon = id == "l" ? (cut ? Icon::LowCut : (shelf ? Icon::LowShelf : Icon::Bell))
                                            : (cut ? Icon::HighCut : (shelf ? Icon::HighShelf : Icon::Bell));
                drawIcon (g, icon, lead.reduced (lead.getHeight() * 0.22f), ink);
                if (shelf)
                    type = "SHV";
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
            g.drawFittedText (type, k.toNearestInt().reduced (4, 0), juce::Justification::centred, 1, 0.7f);
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
        if (graph->getSelectedBand() < 0 && ! bands.isEmpty())
            graph->setSelectedBand (juce::jmin (bands.size() - 1, 1));
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
        for (auto suffix : { "g", "f", "q", "eq" })
        {
            const int i = spec.find (band + suffix);
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
        if (tilt != nullptr && strip.params.filter.toolModel.load() != shownTool)
        {
            // The tool filter changed: its knob box follows.
            auto& f = strip.params.filter;
            shownTool = f.toolModel.load();
            const auto& tool = models::model (models::FilterTools, shownTool);
            owned.removeObject (tilt);
            tilt = tool.params.empty() ? nullptr : &make<KnobBox> (bindConsoleParam (f.toolParams[0], tool.params[0]));
            if (tilt != nullptr)
                tilt->ringColour = toolFilterColour (shownTool);
            resized();
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

    IconKey *on = nullptr, *loCut = nullptr, *hiCut = nullptr, *filter = nullptr, *bandSolo = nullptr, *freeze = nullptr;
    ChoiceField* model = nullptr;
    KnobBox *mix = nullptr, *tilt = nullptr;
    ResponseGraph* graph = nullptr;
    Faceplate* face = nullptr;
    juce::StringArray bands, allowed;
    std::vector<juce::Rectangle<int>> bandKeys;
    juce::Rectangle<int> header, bandKeyArea;
    int bandKeyH = 30, bandKeyGap = 4, shownModel = -1, shownTool = -1, knobPage = 0, pressCount = 0;
    bool heldReset = false;
};

//==============================================================================
/** One channel of the automix screen: colour strip with the number, icon and
    name, the X / Y group, the attenuation meter and the weight slider. */
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
        drawChannelIcon (g, strip.params.icon.load(), strip.getKind(), iconArea.reduced (iconArea.getHeight() * 0.15f), colours::caption);
        auto name = r.removeFromTop (14.0f);
        g.setColour (Colours::text);
        g.setFont (wingFont (TypeScale::caption));
        g.drawFittedText (stripName (strip).toUpperCase(), name.toNearestInt(), juce::Justification::centred, 1, 0.6f);
        groupKey = r.removeFromTop (juce::jlimit (16.0f, screenCap (24.0f), r.getHeight() * 0.1f)).reduced (r.getWidth() * 0.2f, 1.0f);
        const int mode = strip.params.postInsert.mode.load();
        g.setColour (mode == 1 ? colours::fader : Colours::active);
        g.fillRect (groupKey);
        g.setColour (juce::Colours::black);
        g.setFont (homeFont (TypeScale::value, groupKey.getHeight() * 0.8f, WingWeight::SemiBold));
        g.drawText (mode == 1 ? "X" : "Y", groupKey.toNearestInt(), juce::Justification::centred);
        r.removeFromTop (4.0f);

        // Attenuation meter (yellow, from the top) and the weight slider.
        auto meter = r.removeFromRight (juce::jmax (4.0f, r.getWidth() * 0.18f)).reduced (0.0f, 2.0f);
        g.setColour (colours::well);
        g.fillRect (meter);
        const float att = juce::jlimit (0.0f, 1.0f, -strip.meters.automixDb.load() / 40.0f);
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
                                        [this] (ChoiceField& field)
                                        {
                                            // NONE and the 16 slots in two columns, all
                                            // in view: NONE and FX 1-8 (the premium
                                            // slots), FX 9-16 beside FX 1-8.
                                            juce::StringArray items { "NONE" };
                                            for (int i = 0; i < kNumFx; ++i)
                                                items.add (ctx.engine.getFx().describe (i));
                                            popups::ListOptions options;
                                            options.columns = 2;
                                            options.gapsBefore = { 1 + kNumFx / 2 };
                                            options.maxRows = 1 + kNumFx / 2;
                                            options.caption = "FX PROCESSOR";
                                            popups::chooseFrom (field, items, ins.fx.load() + 1, [this] (int i) { ins.fx = i - 1; }, options);
                                        });
        processor->keyHidden = true; // the > key beside it is the field's key [D8]
        openSlot = &key ({}, Icon::ChevronRight);
        openSlot->setTooltip ("Edit the slot on EFFECTS");
        openSlot->onClick = [this]
        {
            if (const int slot = ins.fx.load(); slot >= 0 && ctx.editFx)
                ctx.editFx (slot);
        };
        type = &make<ChoiceField> ("FX TYPE", [this] { return typeText; }, [this] (ChoiceField& field)
                                   {
                                       if (const int slot = ins.fx.load(); slot >= 0)
                                           showFxDeviceMenu (ctx, slot, &field, [this] { shownSlot = -2; });
                                   });
        type->arrowKey = true;
        reset = &key ("RESET", Icon::Reset);
        reset->iconAbove = true;
        reset->outlined = true;
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
        spill = &key ("FX SPILL", Icon::Spill);
        spill->outlined = true;
        spill->isOn = [this] { return ins.fx.load() >= 0 && isSpilling (ctx, { LayerSet::Spill::Kind::Fx, ins.fx.load(), {} }); };
        spill->onClick = [this]
        {
            if (const int slot = ins.fx.load(); slot >= 0)
                toggleSpill (ctx, std::nullopt, { LayerSet::Spill::Kind::Fx, slot, {} });
        };
        if (strip.isOutput())
        {
            setCust = &key ("SET CUST", Icon::KeyboardEdit);
            setCust->iconAbove = true;
            setCust->outlined = true;
            setCust->setTooltip ("Name the channel after the effect");
            setCust->onClick = [this]
            {
                const int slot = ins.fx.load();
                if (slot < 0 || ! ctx.engine.getFx().isLoaded (slot))
                    return;
                auto* plugin = ctx.engine.getFx().getPlugin (slot);
                strip.setName (plugin != nullptr ? plugin->getName() : models::model (models::Fx, ctx.engine.getFx().getModel (slot)).name.toUpperCase());
            };
        }
        openEditor = &key ("OPEN EDITOR", Icon::Edit);
        openEditor->onClick = [this]
        {
            const int slot = ins.fx.load();
            if (slot >= 0 && ctx.engine.getFx().hasPlugin (slot) && ctx.openPluginEditor)
                ctx.openPluginEditor (slot);
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

    void rebuildAutomix()
    {
        for (auto* col : columns)
            owned.removeObject (col);
        columns.clear();
        if (automix())
            for (auto& ch : ctx.engine.getChannels())
                if (ch->params.postInsert.mode.load() > 0)
                    columns.push_back (&make<AutomixColumn> (*ch, ch.get() == &strip));
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
        if (automix() != shownAutomix)
            rebuildAutomix();
        if (slot != shownSlot || model != shownModel || plugin != shownPlugin)
        {
            shownSlot = slot;
            shownModel = model;
            shownPlugin = plugin;
            knobPage = 0;
            typeText = "NONE";
            if (plugin)
                typeText = fx.getPlugin (slot)->getName().toUpperCase();
            else if (model > ld::kFxExt)
                typeText = models::model (models::Fx, model).name.toUpperCase();
            type->setEnabled (slot >= 0);
            openSlot->setEnabled (slot >= 0);
            spill->setEnabled (slot >= 0);
            reset->setEnabled (slot >= 0 && model > ld::kFxExt);
            if (setCust != nullptr)
                setCust->setEnabled (slot >= 0 && fx.isLoaded (slot));
            // FX MIX follows the slot.
            if (mixBox != nullptr)
                owned.removeObject (mixBox);
            mixBox = nullptr;
            if (slot >= 0 && fx.isLoaded (slot))
                mixBox = &make<KnobBox> (bindLinear ("FX MIX", fx.params (slot).mix, 0.0f, 100.0f, " %", 0, 100.0f));
            if (slot >= 0 && model > ld::kFxExt && ! plugin)
            {
                face->bind (models::Fx, model, fx.params (slot).values, [&fx, slot] { return fx.getMeter (slot); });
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
            hint->setText (slot < 0 ? "Choose an FX PROCESSOR (one of the 16 slots of the FX rack)."
                                    : (plugin ? "Plugin: open its editor." : (model <= ld::kFxExt ? "The slot is empty: choose its FX TYPE." : juce::String())),
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
        // 336-416, FX TYPE 840-1144; row 2 FX MIX 16-172, RESET 904-968,
        // FX SPILL 988-1144.
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

        for (auto* c : { (juce::Component*) processor, (juce::Component*) openSlot, (juce::Component*) type, (juce::Component*) reset,
                         (juce::Component*) spill, (juce::Component*) face, (juce::Component*) hint, (juce::Component*) openEditor })
            c->setVisible (! am);
        if (setCust != nullptr)
            setCust->setVisible (! am);
        if (mixBox != nullptr)
            mixBox->setVisible (! am);

        if (modeKeys != nullptr)
        {
            // Automix: the mode keys in the header, the group enables in row 2.
            modeKeys->setBounds (span (h, am ? 128.0f : 432.0f, am ? 560.0f : 824.0f));
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
        openSlot->setBounds (proc.removeFromRight (proc.getHeight() * 2 / 3));
        processor->setBounds (proc.withTrimmedRight (2));
        if (setCust != nullptr)
            setCust->setBounds (span (h, 336.0f, 416.0f));
        type->setBounds (span (h, 840.0f, 1144.0f));
        if (mixBox != nullptr)
            mixBox->setBounds (span (r2, 16.0f, 172.0f));
        reset->setBounds (span (r2, 904.0f, 968.0f));
        spill->setBounds (span (r2, 988.0f, 1144.0f));

        openEditor->setVisible (shownPlugin);
        if (shownPlugin)
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
    juce::Rectangle<int> header, row2;
    juce::String typeText { "NONE" };
    int shownSlot = -2, shownModel = -2, knobPage = 0;
    bool shownPlugin = false, shownAutomix = false;
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
        level->setBounds (box (right, "LEVEL"));

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
    only), LINK SEND PAN TO CHANNEL PAN and the send pan. */
class SendOptions final : public juce::Component, private juce::Timer
{
public:
    /** A bus send: TAP (PRE) / POST / GROUP. */
    SendOptions (SendParams& s, bool inputSource, bool canIgnoreMute)
        : mode (juce::StringArray { sendModeName (0, inputSource), "POST", "GROUP" }, [&s] { return s.mode.load(); }, [&s] (int m) { s.mode = m; }),
          panBox (wingPan ("PAN", s.pan)), canIgnore (canIgnoreMute)
    {
        bindKey (ignoreMute, s.ignoreMute);
        bindKey (link, s.panLink);
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
            bindKey (link, m.panLink);
            isLinked = [&m] { return m.panLink.load(); };
        }
        else
        {
            isLinked = [] { return false; };
        }
        setUp();
    }

    void setUp()
    {
        ignoreMute.twoLines = true;
        link.twoLines = true;
        for (juce::Component* c : { (juce::Component*) &mode, (juce::Component*) &ignoreMute, (juce::Component*) &panBox })
            addAndMakeVisible (c);
        addChildComponent (link);
        link.setVisible (isLinked != nullptr && link.onClick != nullptr);
        timerCallback();
        startTimerHz (6);
        setSize (300, 210);
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
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 6);
        r.removeFromTop (16);
        mode.setBounds (r.removeFromTop (32));
        r.removeFromTop (8);
        auto row = r.removeFromTop (46);
        ignoreMute.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (2, 0));
        link.setBounds (row.reduced (2, 0));
        r.removeFromTop (8);
        panBox.setBounds (r.removeFromTop (44));
    }

private:
    Segmented mode;
    IconKey ignoreMute { "IGNORE CHANNEL MUTE", std::nullopt, colours::accent }, link { "LINK SEND PAN TO CHANNEL PAN", std::nullopt, colours::accent };
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
                                  bindLog ("Q", b.q, 0.442f, 10.0f, {}, i == 1 ? 1.0f : 0.707f) })
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
    static constexpr float kDefaultFreqs[ld::kTapEqBands] = { 100.0f, 1000.0f, 10000.0f };

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
                // Buses 9+ can feed buses 1-8; the others are shown empty.
                const bool possible = &bus != &strip && ! (strip.isBus() && ! (strip.getId() >= kBusToBusTargets && bus.getId() < kBusToBusTargets));
                auto* send = &strip.sends[(size_t) bus.getId()];
                auto* col = &make<SendColumn> (strip, bus, send, nullptr, possible, [this, i] { select (i); });
                const bool self = &bus == &strip;
                col->onImpossible = [this, self, name = shortLabel (bus)]
                {
                    // WING 3.0: "Warning when a bus is sent to the same bus (feedback loop)".
                    popups::warn (*this, "FEEDBACK LOOP",
                                  self ? "A bus can't send to itself: the send would feed its own input."
                                       : "Only buses 9-16 can send to buses 1-8: a send from " + shortLabel (strip) + " to " + name
                                             + " could feed back into itself.");
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
            k.push_back ({ bindLog ("Q", b.q, 0.442f, 10.0f, {}, 1.0f), widthStyle (tapEqPanel) });
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
        dst.setName (src.getName());
        d.colour = s.colour.load();
        d.icon = s.icon.load();
        d.led = s.led.load();
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
            if (dst.isBus() && ! (dst.getId() >= kBusToBusTargets && id < kBusToBusTargets))
                continue;
            auto* from = src.sends.find (id);
            if (from == nullptr && dst.sends.find (id) == nullptr)
                continue;
            auto& to = dst.sends[(size_t) id];
            to.enabled = from != nullptr && from->enabled.load();
            to.panLink = from != nullptr ? from->panLink.load() : true;
            to.ignoreMute = from != nullptr && from->ignoreMute.load();
            to.panReversed = from != nullptr && from->panReversed.load();
            to.mode = from != nullptr ? from->mode.load() : 1;
            to.levelDb = from != nullptr ? from->levelDb.load() : -100.0f;
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
        updateTabs();
        startTimerHz (4);
    }

    void paint (juce::Graphics& g) override
    {
        OverlayScreen::paint (g);
        // SCOPE: the title at the left, NONE and ALL in the header.
        drawSectionBox (g, scopePanel, " ");
        {
            auto header = sectionHeader (scopePanel).withTrimmedLeft (12);
            g.setColour (colours::caption);
            g.setFont (wingFontPx (juce::jmin (TypeScale::header * getWingScreenHeight(), (float) header.getHeight() * 0.42f)));
            g.drawText ("SCOPE", header, juce::Justification::centredLeft, false);
        }
        drawSectionBox (g, destPanel, "DESTINATION");
        if (copy)
            if (auto* src = state::findStrip (ctx.engine, source))
            {
                paintStripTile (g, sourceTile.toFloat(), *src, false);
                g.setColour (colours::accent);
                g.drawRect (sourceTile.toFloat(), 2.0f);
            }

        // The destination tiles.
        tiles.clear();
        const auto& list = ctx.engine.getStrips (destKind);
        const int n = (int) list.size();
        if (n == 0)
            return;
        auto grid = gridArea;
        const int columns = 8;
        const int rows = juce::jmax (5, (n + columns - 1) / columns);
        const float w = (float) grid.getWidth() / (float) columns, h = juce::jmin ((float) grid.getHeight() / (float) rows, w * 0.8f);
        for (int i = 0; i < n; ++i)
        {
            auto r = juce::Rectangle<float> ((float) grid.getX() + (float) (i % columns) * w, (float) grid.getY() + (float) (i / columns) * h, w, h).reduced (2.0f);
            const auto ref = state::StripRef::of (*list[(size_t) i]);
            const bool sel = std::find (destinations.begin(), destinations.end(), ref) != destinations.end();
            const bool isSource = copy && ref == source;
            paintDestinationTile (g, r, *list[(size_t) i], sel, isSource);
            tiles.push_back ({ r.toNearestInt(), ref });
        }
    }

    void layoutContent (juce::Rectangle<int> b) override
    {
        scopePanel = b.removeFromLeft (b.getWidth() * 40 / 100);
        b.removeFromLeft (6);
        destPanel = b;

        auto header = sectionHeader (scopePanel);
        const int iconKeyW = juce::jmin (header.getHeight() + 8, header.getWidth() / 5);
        all.setBounds (header.removeFromRight (iconKeyW).reduced (2));
        none.setBounds (header.removeFromRight (iconKeyW).reduced (2));
        auto s = sectionContent (scopePanel, true);
        if (copy)
        {
            auto top = s.removeFromTop (juce::jlimit (30, screenCap (52), s.getHeight() / 7));
            sourceTile = top.removeFromLeft (juce::jmin (top.getWidth() / 3, top.getHeight() * 3 / 2));
            s.removeFromTop (8);
        }
        action.setBounds (s.removeFromBottom (juce::jlimit (28, screenCap (44), s.getHeight() / 7)).withSizeKeepingCentre (juce::jmin (screenCap (160), s.getWidth()), juce::jlimit (28, screenCap (44), s.getHeight() / 7)));
        s.removeFromBottom (8);
        // CONN CONFIG CUST TAGS / IN FILTER GATE DYN / EQ DELAY PRE POST / SEND MAIN / FDR MUTE PAN
        const int layout[5][4] = { { Conn, Config, Cust, Tags }, { In, FilterBit, GateBit, DynBit }, { EqBit, DelayBit, Pre, Post },
                                   { Send, MainBit, -1, -1 }, { Fdr, MuteBit, PanBit, -1 } };
        const int kh = s.getHeight() / 5, kw = s.getWidth() / 4;
        for (int r = 0; r < 5; ++r)
            for (int col = 0; col < 4; ++col)
                if (layout[r][col] >= 0)
                    scopeKeys[layout[r][col]]->setBounds (juce::Rectangle<int> (s.getX() + col * kw, s.getY() + r * kh, kw, kh).reduced (1));

        auto d = sectionContent (destPanel, true);
        auto tabRow = d.removeFromTop (juce::jlimit (24, screenCap (36), d.getHeight() / 9));
        const int tw = tabRow.getWidth() / kNumStripKinds;
        for (auto* t : tabs)
            t->setBounds (tabRow.removeFromLeft (tw).reduced (3, 0));
        d.removeFromTop (6);
        gridArea = d;
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
        large in the middle and the number under it; a gold frame when chosen. */
    static void paintDestinationTile (juce::Graphics& g, juce::Rectangle<float> r, const Strip& s, bool selected, bool dim)
    {
        const auto colour = dim ? stripDisplayColour (s).withMultipliedBrightness (0.5f) : stripDisplayColour (s);
        g.setColour (colours::keyOff);
        g.fillRect (r);
        auto body = r;
        auto bar = body.removeFromTop (juce::jmax (8.0f, r.getHeight() * 0.2f));
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
        if (! drawWingIcon (g, s.params.icon.load(), iconArea, colours::caption) && ! s.isInput())
            drawIcon (g, stripIcon (s.getKind()), iconArea.reduced (iconArea.getHeight() * 0.1f), colours::dim);
        if (selected)
        {
            g.setColour (colours::accent);
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
        action.setEnabled (scope != 0 && ! destinations.empty());
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
    juce::Rectangle<int> scopePanel, destPanel, sourceTile, gridArea;
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
            if (&src == &dst || (src.isBus() && ! (src.getId() >= kBusToBusTargets && dst.getId() < kBusToBusTargets)))
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
