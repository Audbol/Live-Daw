#include "ui/HomeScreens.h"
#include "ui/Controls.h"
#include "ui/Faceplate.h"
#include "ui/MeterComponent.h"
#include "ui/ModelParams.h"
#include "ui/ResponseGraph.h"
#include "ui/WingPopups.h"
#include "ui/WingWidgets.h"

namespace livedaw::ui
{

using namespace home;

//==============================================================================
void HomeScreen::paint (juce::Graphics& g)
{
    for (auto& p : boxes)
        drawSectionBox (g, p.area, p.title, p.chevron);
}

juce::Label& HomeScreen::label (const juce::String& text, float size, juce::Colour colour)
{
    auto& l = make<juce::Label>();
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
        const float tagH = juce::jlimit (16.0f, 22.0f, r.getHeight() * 0.08f);
        auto scale = r.removeFromLeft (r.getWidth() * 0.42f);
        auto track = r.withSizeKeepingCentre (juce::jlimit (8.0f, 22.0f, r.getWidth() * 0.4f), r.getHeight() - tagH);
        const int mode = juce::jlimit (0, 3, (int) in.delayMode.load());
        const float max = msToDelay (kMaxDelayMs, mode, sampleRateOf (ctx));
        g.setFont (juce::FontOptions (juce::jlimit (9.0f, 12.0f, r.getHeight() * 0.045f), juce::Font::bold));
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
        g.setFont (juce::FontOptions (tagH * 0.7f, juce::Font::bold));
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
        delayKey.setBounds (right.removeFromBottom (juce::jlimit (24, 46, right.getHeight() / 5)).reduced (4, 0));
        right.removeFromBottom (6);
        const int h = juce::jmin (46, right.getHeight() / 4);
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
    TrimBalancePad (std::atomic<float>& t, std::atomic<float>& b, bool stereo) : trim (t), bal (b), hasBalance (stereo) { startTimerHz (12); }

    juce::Rectangle<float> pad() const { return getLocalBounds().toFloat().reduced (2.0f).withTrimmedTop (16.0f).withTrimmedBottom (16.0f).withTrimmedLeft (30.0f); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colours::black);
        g.fillRect (r);
        const auto p = pad();
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText ("TRIM", r.reduced (4.0f, 2.0f).toNearestInt(), juce::Justification::topLeft);
        g.drawText ("BAL", r.reduced (4.0f, 2.0f).toNearestInt(), juce::Justification::topRight);
        for (int db = 18; db >= -18; db -= 6)
        {
            const float y = p.getCentreY() - (float) db / 18.0f * p.getHeight() * 0.5f;
            g.setColour (Colours::textDim);
            g.drawText (db == 0 ? juce::String ("0 dB") : juce::String (std::abs (db)), juce::Rectangle<float> (r.getX(), y - 6.0f, 28.0f, 12.0f).toNearestInt(),
                        juce::Justification::centredRight);
            g.setColour (juce::Colour (0xff3a3a3a));
            g.fillRect (p.getX(), y, 6.0f, 1.0f);
            g.fillRect (p.getRight() - 6.0f, y, 6.0f, 1.0f);
        }
        // Ticks and labels along the bottom (balance).
        for (int i = 0; i <= 18; ++i)
        {
            const float x = p.getX() + p.getWidth() * (float) i / 18.0f;
            g.setColour (juce::Colour (0xff6a6a6a));
            g.fillRect (x, p.getBottom() - (i % 9 == 0 ? 10.0f : 5.0f), 1.0f, i % 9 == 0 ? 10.0f : 5.0f);
        }
        g.setColour (Colours::textDim);
        g.drawText ("-9 dB", juce::Rectangle<float> (p.getX() - 10.0f, p.getBottom() + 2.0f, 40.0f, 12.0f).toNearestInt(), juce::Justification::centredLeft);
        g.drawText ("0 dB", juce::Rectangle<float> (p.getCentreX() - 20.0f, p.getBottom() + 2.0f, 40.0f, 12.0f).toNearestInt(), juce::Justification::centred);
        g.drawText ("+9 dB", juce::Rectangle<float> (p.getRight() - 30.0f, p.getBottom() + 2.0f, 40.0f, 12.0f).toNearestInt(), juce::Justification::centredRight);

        const float x = p.getCentreX() + bal.load() / 9.0f * p.getWidth() * 0.5f;
        const float y = p.getCentreY() - trim.load() / 18.0f * p.getHeight() * 0.5f;
        g.setColour (colours::fader);
        g.fillRect (x - 1.0f, p.getY(), 2.0f, p.getHeight());
        g.setColour (juce::Colour (0xffbdbdbd));
        g.fillRect (p.getX(), y - 1.0f, p.getWidth(), 2.0f);
        // The crosshair handle and the balance handle on the bottom axis.
        const float hw = juce::jlimit (14.0f, 24.0f, p.getWidth() * 0.08f);
        g.setColour (juce::Colours::white);
        g.fillRoundedRectangle (juce::Rectangle<float> (hw, hw * 0.6f).withCentre ({ x, y }), 3.0f);
        g.setColour (juce::Colours::black);
        g.fillRect (juce::Rectangle<float> (hw * 0.6f, 1.5f).withCentre ({ x, y }));
        const float bh = hw * 0.7f;
        g.setColour (hasBalance ? colours::fader : juce::Colour (0xff3a4a5a));
        g.fillEllipse (juce::Rectangle<float> (bh, bh).withCentre ({ x, p.getBottom() - bh * 0.6f }));
        g.setColour (juce::Colours::white);
        g.fillRect (juce::Rectangle<float> (1.5f, bh * 0.5f).withCentre ({ x - 2.0f, p.getBottom() - bh * 0.6f }));
        g.fillRect (juce::Rectangle<float> (1.5f, bh * 0.5f).withCentre ({ x + 2.0f, p.getBottom() - bh * 0.6f }));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto p = pad();
        balanceOnly = hasBalance && e.position.y > p.getBottom() - 22.0f;
        startTrim = trim.load();
        startBal = bal.load();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto p = pad();
        const float fine = e.mods.isShiftDown() ? 0.2f : 1.0f;
        if (hasBalance)
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
        const auto now = std::make_pair (trim.load(), bal.load());
        if (now != shown)
        {
            shown = now;
            repaint();
        }
    }

    std::atomic<float>& trim;
    std::atomic<float>& bal;
    bool hasBalance, balanceOnly = false;
    float startTrim = 0.0f, startBal = 0.0f;
    std::pair<float, float> shown { 1.0e9f, 0.0f };
};

/** "IN 3", "PLAY 5" or "NONE" for a channel input index (stereo: "IN 3/4"). */
juce::String inputSourceText (int left, int right, bool stereo)
{
    if (left < 0)
        return "NONE";
    const bool playback = Recorder::isPlaybackSource (left);
    auto number = [playback] (int source) { return juce::String ((playback ? Recorder::playbackChannelOf (source) : source) + 1); };
    const auto prefix = juce::String (playback ? "PLAY " : "IN ");
    if (stereo)
        return prefix + number (left) + "/" + number (right >= 0 ? right : left + 1);
    return prefix + number (left);
}

/** The ALT input menu: NONE, the hardware inputs and the recorder's playback channels. */
void showAltSourceMenu (UiContext& ctx, int current, juce::Component* target, std::function<void (int)> chosen)
{
    juce::PopupMenu menu;
    menu.addSectionHeader ("ALT SOURCE");
    menu.addItem ("NONE", true, current < 0, [chosen] { chosen (-1); });
    const auto& names = ctx.engine.getHardwareInputNames();
    const int numInputs = names.isEmpty() ? 64 : names.size();
    for (int g = 0; g < numInputs; g += 16)
    {
        juce::PopupMenu sub;
        for (int i = g; i < juce::jmin (numInputs, g + 16); ++i)
            sub.addItem ("IN " + juce::String (i + 1) + (i < names.size() && names[i].isNotEmpty() ? "  (" + names[i] + ")" : juce::String()), true,
                         current == i, [chosen, i] { chosen (i); });
        menu.addSubMenu ("AUDIO IN " + juce::String (g + 1) + "-" + juce::String (juce::jmin (numInputs, g + 16)), sub, true, nullptr,
                         current >= g && current < g + 16);
    }
    for (int g = 0; g < Recorder::kNumPlaybackChannels; g += 16)
    {
        juce::PopupMenu sub;
        for (int ch = g; ch < g + 16; ++ch)
            sub.addItem (Recorder::playbackSourceName (ch), true, current == Recorder::playbackSource (ch),
                         [chosen, ch] { chosen (Recorder::playbackSource (ch)); });
        menu.addSubMenu ("RECORDER PLAY " + juce::String (g + 1) + "-" + juce::String (g + 16), sub, true, nullptr,
                         Recorder::playbackChannelOf (current) >= g && Recorder::playbackChannelOf (current) < g + 16);
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target));
}

/** The ALT source tile (WING INPUT SELECT): the channel's ALT input, e.g. a
    recorder playback channel for virtual soundcheck. Click to choose it. */
class AltSourceTile final : public juce::Component, private juce::Timer
{
public:
    AltSourceTile (UiContext& c, Strip& s) : ctx (c), strip (s)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        startTimerHz (4);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colours::black);
        g.fillRect (r);
        const bool has = strip.hasAltInput();
        const bool playback = has && Recorder::isPlaybackSource (strip.getAltInputLeft());
        auto inner = r.reduced (2.0f);
        auto header = inner.removeFromTop (juce::jlimit (12.0f, 22.0f, inner.getHeight() * 0.2f));
        g.setColour (playback ? juce::Colour (0xff2f5a32) : juce::Colour (0xff2a4a70));
        g.fillRect (header);
        g.setColour (Colours::text.withAlpha (has ? 1.0f : 0.5f));
        g.setFont (juce::FontOptions (header.getHeight() * 0.8f, juce::Font::bold));
        g.drawText (playback ? "RECORDER" : "ALT", header.toNearestInt(), juce::Justification::centred);
        g.setColour (has ? colours::keyOff : colours::panel);
        g.fillRect (inner);
        auto label = inner.removeFromBottom (juce::jlimit (12.0f, 22.0f, inner.getHeight() * 0.26f));
        drawIcon (g, playback ? Icon::Waveform : Icon::Xlr, inner.reduced (inner.getHeight() * 0.2f), colours::caption.withAlpha (has ? 1.0f : 0.4f));
        g.setColour (colours::well);
        g.fillRect (label);
        const bool st = strip.params.stereo.load();
        g.setColour ((st ? colours::stereo : colours::mono).withAlpha (has ? 1.0f : 0.5f));
        g.setFont (juce::FontOptions (label.getHeight() * 0.8f, juce::Font::bold));
        g.drawFittedText (inputSourceText (strip.getAltInputLeft(), strip.getAltInputRight(), st), label.toNearestInt(), juce::Justification::centred, 1, 0.7f);
        if (strip.isUsingAltInput())
        {
            // The source in use is framed (the ALT key lights orange).
            g.setColour (juce::Colour (0xffa0a0a0));
            g.drawRect (r, 2.0f);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! getLocalBounds().contains (e.getPosition()))
            return;
        juce::Component::SafePointer<AltSourceTile> safe (this);
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
    void timerCallback() override
    {
        const auto now = juce::String (strip.getAltInputLeft()) + "|" + juce::String (strip.getAltInputRight()) + (strip.isUsingAltInput() ? "A" : "M")
                         + (strip.params.stereo.load() ? "S" : "");
        if (now != shown)
        {
            shown = now;
            repaint();
        }
    }

    UiContext& ctx;
    Strip& strip;
    juce::String shown;
};

/** The source tile of the INPUT screen: name strip, icon, port. Click to
    patch the channel on ROUTING. */
class PatchTile final : public juce::Component, private juce::Timer
{
public:
    PatchTile (UiContext& c, Strip& s) : ctx (c), strip (s)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTooltip ("Patch on ROUTING");
        startTimerHz (4);
    }

    void setTooltip (const juce::String&) {}

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colours::black);
        g.fillRect (r);
        auto inner = r.reduced (2.0f);
        auto header = inner.removeFromTop (juce::jlimit (12.0f, 22.0f, inner.getHeight() * 0.2f));
        const auto colour = strip.getColour();
        g.setColour (colour);
        g.fillRect (header);
        g.setColour (colour.getPerceivedBrightness() > 0.45f ? juce::Colours::black : juce::Colours::white);
        g.setFont (juce::FontOptions (header.getHeight() * 0.85f, juce::Font::bold));
        g.drawFittedText (stripName (strip).toUpperCase(), header.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.7f);
        g.setColour (colours::keyOff);
        g.fillRect (inner);
        auto label = inner.removeFromBottom (juce::jlimit (12.0f, 22.0f, inner.getHeight() * 0.26f));
        drawChannelIcon (g, strip.params.icon.load(), strip.getKind(), inner.reduced (inner.getHeight() * 0.2f), colours::ink);
        g.setColour (colours::well);
        g.fillRect (label);
        const bool st = strip.params.stereo.load();
        g.setColour (st ? colours::stereo : colours::mono);
        g.setFont (juce::FontOptions (label.getHeight() * 0.8f, juce::Font::bold));
        g.drawFittedText (port(), label.toNearestInt(), juce::Justification::centred, 1, 0.7f);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (getLocalBounds().contains (e.getPosition()) && ctx.showScreen)
            ctx.showScreen (Screen::Routing);
    }

private:
    juce::String port() const
    {
        return inputSourceText (strip.getInputLeft(), strip.getInputRight(), strip.params.stereo.load());
    }

    void timerCallback() override
    {
        const auto now = port() + stripName (strip) + juce::String (strip.getColourIndex()) + "|" + juce::String (strip.params.icon.load());
        if (now != shown)
        {
            shown = now;
            repaint();
        }
    }

    UiContext& ctx;
    Strip& strip;
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
                  const float gainDb = strip.params.input.trimDb.load() + (ch == 0 ? -juce::jmax (0.0f, bal) : juce::jmin (0.0f, bal) * -1.0f);
                  return ctx.meters.get (&strip, true, ch) * juce::Decibels::decibelsToGain (gainDb);
              },
              [this] { return strip.getNumChannels(); })
    {
        auto& p = strip.params;
        pad = &make<TrimBalancePad> (p.input.trimDb, p.input.balanceDb, strip.getNumChannels() == 2 || strip.isOutput() || p.stereo.load());
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
            mainTile = &make<PatchTile> (ctx, strip);
            altTile = &make<AltSourceTile> (ctx, strip);
            inputSelectLabel = &key ("INPUT SELECT");
            inputSelectLabel->setTooltip ("The console's INPUT SELECT (SETUP > AUDIO)");
            inputSelectLabel->onClick = [this] { if (ctx.showScreen) ctx.showScreen (Screen::Setup); };
            selectMode = &make<Segmented> (juce::StringArray { "INDIVIDUAL", "GLOBAL" }, [&engine] { return engine.isInputSelectGlobal() ? 1 : 0; },
                                           [&engine] (int i) { engine.setInputSelectGlobal (i == 1); });
            // INDIVIDUAL lights blue, GLOBAL gold (1.11's MANUAL | AUTO were orange).
            selectMode->colourAt = [] (int i) { return i == 1 ? colours::accent : colours::fader; };
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
            auto ci = box (left, "CHANNEL INPUT");
            auto top = ci.removeFromTop (juce::jlimit (24, 40, ci.getHeight() / 8));
            bracket->setBounds (top.reduced (0, 2).withTrimmedBottom (top.getHeight() / 2));
            inputSel->setBounds (top.reduced (ci.getWidth() / 5, 0));
            ci.removeFromTop (6);
            auto tiles = ci.removeFromTop (ci.getHeight() * 48 / 100);
            mainTile->setBounds (tiles.removeFromLeft (tiles.getWidth() / 2).reduced (3, 0).withSizeKeepingCentre (juce::jmin (tiles.getWidth() - 6, tiles.getHeight() * 5 / 4), juce::jmin (tiles.getHeight(), tiles.getWidth() * 4 / 5)));
            altTile->setBounds (tiles.reduced (3, 0).withSizeKeepingCentre (juce::jmin (tiles.getWidth() - 6, tiles.getHeight() * 5 / 4), juce::jmin (tiles.getHeight(), tiles.getWidth() * 4 / 5)));
            ci.removeFromTop (8);
            const int kh = juce::jlimit (22, 34, ci.getHeight() / 4);
            auto row = ci.removeFromTop (kh);
            stereo->setBounds (row.removeFromLeft (juce::jmin (50, row.getWidth() / 4)));
            row.removeFromLeft (6);
            inputSelectLabel->setBounds (row);
            ci.removeFromTop (6);
            selectMode->setBounds (ci.removeFromTop (kh).withTrimmedLeft (juce::jmin (56, ci.getWidth() / 4)));
        }
        else
        {
            auto feedArea = directOn != nullptr ? left.removeFromTop (left.getHeight() * 55 / 100) : left;
            auto f = box (feedArea, "FEED");
            feed->setBounds (f.removeFromBottom (juce::jlimit (24, 36, f.getHeight() / 6)));
            f.removeFromBottom (6);
            feedGrid->setBounds (f);
            if (directOn != nullptr)
            {
                auto d = box (left, "DIRECT INPUT");
                const int kh = juce::jlimit (22, 32, d.getHeight() / 5);
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
                directLevel->setBounds (d.removeFromTop (juce::jmin (48, d.getHeight())).withWidth (juce::jmin (d.getWidth(), 150)));
            }
        }

        auto tb = box (middle, "TRIM & BALANCE");
        auto meterCol = tb.removeFromRight (juce::jmax (54, tb.getWidth() * 24 / 100));
        invert->setBounds (meterCol.removeFromBottom (juce::jlimit (30, 52, meterCol.getHeight() / 6)).reduced (2, 0));
        if (mono != nullptr)
        {
            meterCol.removeFromBottom (4);
            mono->setBounds (meterCol.removeFromBottom (juce::jlimit (22, 32, meterCol.getHeight() / 7)).reduced (2, 0));
        }
        trimMeter.setBounds (meterCol.reduced (2, 4));
        tb.removeFromRight (4);
        pad->setBounds (tb);

        if (loCut != nullptr)
        {
            auto f = box (right.removeFromTop (juce::jmax (80, right.getHeight() * 30 / 100)), "FILTER", true);
            filterHeader = f.withY (f.getY() - kPanelHeaderHeight - 4).withHeight (kPanelHeaderHeight);
            layoutRow (f, { loCut, hiCut, tool }, 4);
        }
        if (delay != nullptr)
        {
            delay->setBounds (box (right, strip.hasPostFaderDelay() ? "DELAY (POST)" : "DELAY"));
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (filterHeader.contains (e.getPosition()) && ctx.showHomeTab)
            ctx.showHomeTab (HomeTab::Filter);
    }

private:
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
    PatchTile* mainTile = nullptr;
    AltSourceTile* altTile = nullptr;
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
        freeze->setEnabled (false); // needs the RTA (not modelled)
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
        lcCaption = &label ("LC  dB/oct", 13.0f, colours::caption);
        lcSlopes = &make<Segmented> (models::list (models::LowCutSlopes), [&f] { return f.lowCutSlope.load(); }, [&f] (int i) { f.lowCutSlope = i; });
        hcCaption = &label ("HC  dB/oct", 13.0f, colours::caption);
        hcSlopes = &make<Segmented> (models::list (models::HighCutSlopes), [&f] { return f.highCutSlope.load(); }, [&f] (int i) { f.highCutSlope = i; });
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
        k.push_back ({ wingFreq ("HC FREQ", f.highCutFreq, 200.0f, 20000.0f, 10000.0f), freqStyle (graph) });
        return k;
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        // Fig. 63: the title at the left, FREEZE, the LO CUT / HI CUT / tool
        // keys (icons over captions) and CLOSE at the right.
        header = b.removeFromTop (juce::jlimit (32, 60, b.getHeight() / 9));
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

        auto s = box (side, {});
        toolCaption->setBounds (s.removeFromTop (18));
        const int kh = juce::jlimit (22, 34, s.getHeight() / 9);
        for (auto* k : tools)
        {
            k->setBounds (s.removeFromTop (kh));
            s.removeFromTop (3);
        }
        s.removeFromTop (8);
        lcCaption->setBounds (s.removeFromTop (18));
        lcSlopes->setBounds (s.removeFromTop (kh));
        s.removeFromTop (8);
        hcCaption->setBounds (s.removeFromTop (18));
        hcSlopes->setBounds (s.removeFromTop (kh).withWidth (s.getWidth() / 2));
    }

    void paint (juce::Graphics& g) override
    {
        HomeScreen::paint (g);
        auto h = header.toFloat();
        g.setColour (colours::panel);
        g.fillRect (h);
        g.setColour (colours::caption);
        g.setFont (juce::FontOptions (juce::jlimit (14.0f, 26.0f, h.getHeight() * 0.42f), juce::Font::plain));
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
    DynTab (UiContext& c, Strip& s, bool gateSlot) : HomeScreen (c, s), isGate (gateSlot), slot (gateSlot ? s.params.gate : s.params.dynamics)
    {
        on = &key ("ON", std::nullopt, colours::accent);
        bindKey (*on, slot.on);
        allowed = strip.getKind() == StripKind::Aux && ! isGate ? models::list (models::AuxDynModels) : juce::StringArray();
        model = &make<ChoiceField> (isGate ? "GATE MODEL" : "COMP MODEL",
                                    [this] { return models::model (models::Dynamics, slot.model.load()).name.toUpperCase(); },
                                    [this] (ChoiceField& f)
                                    {
                                        ModelSelector::choose (f, models::Dynamics, slot.model.load(),
                                                               [this] (int m) { strip.setModel (isGate ? ModelBlock::Gate : ModelBlock::Dynamics, m); },
                                                               allowed, isGate ? ModelSelector::gateSlotGroups() : ModelSelector::dynamicsSlotGroups());
                                    });
        mix = &make<KnobBox> (bindLinear ("MIX", slot.mix, 0.0f, 100.0f, " %", 0, 100.0f));
        gain = &make<KnobBox> (bindLinear ("GAIN", slot.gain, -6.0f, 12.0f, " dB", 1, 0.0f));
        gr = &make<GainReductionMeter> ([this] { return isGate ? strip.meters.gateReductionDb.load() : strip.meters.dynReductionDb.load(); },
                                        isGate ? 40.0f : 24.0f);
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
        bindFace();
        startTimerHz (8);

        keySource = &field ("KEY SOURCE", channelNames (ctx.engine), slot.scSource, "SELF");
        keyFilter = &field ("KEY FILTER", models::list (models::SidechainFilters), slot.scFilter);
        keyTap = &field ("KEY TAP", tapPointNames(), slot.scTap);
        keyFreq = &make<KnobBox> (wingFreq ("FREQ", slot.scFreq, 20.0f, 20000.0f, 1000.0f));
        keyQ = &make<KnobBox> (bindLog ("Q", slot.scQ, 0.5f, 10.0f, {}, 1.0f));
        // Needs the engine's solo tap for the key signal (report E9).
        keySolo = &key ("KEY SOLO");
        keySolo->setEnabled (false);

        if (! isGate)
        {
            xoMode = &field ("XOVER MODE", models::list (models::XoverTypes), slot.xoKind);
            xoFreq = &make<KnobBox> (wingFreq ("FREQ", slot.xoFreq, 20.0f, 20000.0f, 1000.0f));
            xoDepth = &make<KnobBox> (bindLinear ("DEPTH", slot.xoDepth, -40.0f, 0.0f, " dB", 1, -40.0f));
            xoQ = &make<KnobBox> (bindLog ("Q", slot.xoQ, 0.5f, 10.0f, {}, 1.0f));
            xoSolo = &key ("XOVER SOLO");
            xoSolo->setEnabled (false); // report E9
        }
    }

    std::vector<HomeKnob> knobs() override
    {
        std::vector<HomeKnob> k;
        const auto& spec = models::model (models::Dynamics, slot.model.load());
        for (int i = knobPage * 6; i < juce::jmin ((int) spec.params.size(), knobPage * 6 + 6); ++i)
            k.push_back ({ bindConsoleParam (slot.values[i], spec.params[(size_t) i]), styleOf (KnobStyle::Bar::Auto, {}, face) });
        return k;
    }

    void resized() override
    {
        clearBoxes();
        auto b = getLocalBounds();
        // The header as on the 3.1 EQ screen: ON, the model field, the gain
        // reduction at the right.
        header = b.removeFromTop (juce::jlimit (32, 60, b.getHeight() / 9));
        const float u = (float) header.getWidth() / 1160.0f;
        auto h = header.reduced (0, juce::jmax (3, header.getHeight() / 9));
        auto span = [&h, u, this] (float x0, float x1)
        { return juce::Rectangle<int> (header.getX() + juce::roundToInt (x0 * u), h.getY(), juce::roundToInt ((x1 - x0) * u), h.getHeight()); };
        on->setBounds (span (16.0f, 112.0f));
        model->setBounds (span (128.0f, 520.0f));
        gr->setBounds (span (900.0f, 1144.0f).reduced (0, h.getHeight() / 5));
        b.removeFromTop (juce::jmax (4, getHeight() / 100));

        // Side column: KEY, XOVER (COMP slot) and the output.
        auto side = b.removeFromRight (juce::jmin (320, b.getWidth() * 40 / 100));
        const int boxH = juce::jlimit (26, 40, side.getHeight() / 11);
        auto out = box (side.removeFromBottom (boxH + kPanelHeaderHeight + 20), "OUTPUT");
        layoutRow (out.withSizeKeepingCentre (out.getWidth(), boxH), { mix, gain }, 6);
        auto keyArea = box (side.removeFromTop (isGate ? side.getHeight() : side.getHeight() * 52 / 100), "KEY");
        const int kh = juce::jlimit (28, 48, keyArea.getHeight() / 4);
        layoutRow (keyArea.removeFromTop (kh), { keySource, keyFilter, keyTap });
        keyArea.removeFromTop (6);
        auto knobRow = keyArea.removeFromTop (juce::jmin (boxH, keyArea.getHeight()));
        keySolo->setBounds (knobRow.removeFromRight (knobRow.getWidth() / 3).reduced (3, 0));
        layoutRow (knobRow, { keyFreq, keyQ }, 6);
        if (! isGate)
        {
            auto xo = box (side, "XOVER");
            auto row = xo.removeFromTop (kh);
            xoSolo->setBounds (row.removeFromRight (row.getWidth() / 3).reduced (3, 0));
            xoMode->setBounds (row.withTrimmedRight (3));
            xo.removeFromTop (6);
            const bool peak = models::list (models::XoverTypes)[slot.xoKind.load()].containsIgnoreCase ("PEAK");
            xoQ->setVisible (peak);
            auto boxRow = xo.removeFromTop (juce::jmin (boxH, xo.getHeight()));
            if (peak)
                layoutRow (boxRow, { xoFreq, xoDepth, xoQ }, 6);
            else
                layoutRow (boxRow.withWidth (boxRow.getWidth() * 2 / 3), { xoFreq, xoDepth }, 6);
        }

        b.removeFromRight (6);
        face->setBounds (b.reduced (0, 4));
    }

    void paint (juce::Graphics& g) override
    {
        HomeScreen::paint (g);
        g.setColour (colours::panel);
        g.fillRect (header);
    }

private:
    bool isGate;
    DynSlotParams& slot;
    IconKey *on = nullptr, *keySolo = nullptr, *xoSolo = nullptr;
    ChoiceField* model = nullptr;
    juce::StringArray allowed;
    KnobBox *mix = nullptr, *gain = nullptr, *keyFreq = nullptr, *keyQ = nullptr, *xoFreq = nullptr, *xoDepth = nullptr, *xoQ = nullptr;
    GainReductionMeter* gr = nullptr;
    Faceplate* face = nullptr;
    ChoiceField *keySource = nullptr, *keyFilter = nullptr, *keyTap = nullptr, *xoMode = nullptr;
    juce::Rectangle<int> header;
    int shownModel = -1, shownXo = -1, knobPage = 0;

    void bindFace()
    {
        shownModel = slot.model.load();
        face->bind (models::Dynamics, shownModel, slot.values,
                    [this] { return -(isGate ? strip.meters.gateReductionDb.load() : strip.meters.dynReductionDb.load()); });
        knobPage = 0;
        notifyKnobs();
    }

    void timerCallback() override
    {
        if (slot.model.load() != shownModel)
            bindFace();
        if (slot.xoKind.load() != shownXo)
        {
            shownXo = slot.xoKind.load();
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
        bandSolo = &key ("BAND SOLO");
        bandSolo->outlined = true;
        bandSolo->setEnabled (false); // needs the engine's band solo (report E9)
        freeze = &key ("FREEZE");
        freeze->outlined = true;
        freeze->setEnabled (false); // needs the RTA (not modelled)

        graph = &make<ResponseGraph>();
        graph->setStrip (&strip, ResponseGraph::Eq);
        graph->setRuler (ResponseGraph::Ruler::Top);
        graph->onBandSelected = [this] (int) { bindBand(); };
        // Analog models show their front panel.
        face = &make<Faceplate>();
        face->onTouched = [this] (int param)
        {
            if (param / 6 != knobPage)
            {
                knobPage = param / 6;
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
            for (int i = knobPage * 6; i < juce::jmin ((int) spec.params.size(), knobPage * 6 + 5); ++i)
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
        header = b.removeFromTop (juce::jlimit (32, 60, b.getHeight() / 9));
        auto h = header.reduced (0, juce::jmax (3, header.getHeight() / 9));
        const float u = (float) header.getWidth() / 1160.0f;
        auto span = [&h, u, this] (float x0, float x1)
        { return juce::Rectangle<int> (header.getX() + juce::roundToInt (x0 * u), h.getY(), juce::roundToInt ((x1 - x0) * u), h.getHeight()); };
        on->setBounds (span (16.0f, 112.0f));
        model->setBounds (span (128.0f, 432.0f));
        mix->setBounds (span (448.0f, 612.0f));
        freeze->setBounds (span (628.0f, 736.0f));
        if (filter != nullptr)
        {
            auto keys = span (752.0f, 960.0f);
            const int kw = keys.getHeight();
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
        const int kh = juce::jlimit (22, 46, side.getHeight() / 9);
        bandSolo->setBounds (side.removeFromBottom (kh));
        side.removeFromBottom (juce::jmax (6, kh / 2));
        bandKeyArea = side;
        bandKeyH = juce::jlimit (20, 48, bandKeyArea.getHeight() / juce::jmax (6, bands.size()));

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
                g.setColour (ink);
                g.setFont (juce::FontOptions (k.getHeight() * 0.6f, juce::Font::plain));
                g.drawText (id.toUpperCase(), lead.toNearestInt(), juce::Justification::centred);
            }
            g.setColour (ink);
            g.setFont (juce::FontOptions (juce::jlimit (11.0f, 20.0f, k.getHeight() * 0.42f), juce::Font::plain));
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
    void updateBands()
    {
        bands = ResponseGraph::stdBands (strip);
        if (graph->getSelectedBand() < 0 && ! bands.isEmpty())
            graph->setSelectedBand (juce::jmin (bands.size() - 1, 1));
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
    int bandKeyH = 30, shownModel = -1, shownTool = -1, knobPage = 0, pressCount = 0;
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
        auto top = r.removeFromTop (juce::jlimit (12.0f, 18.0f, r.getHeight() * 0.06f));
        g.setColour (strip.getColour());
        g.fillRect (top);
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (top.getHeight() * 0.85f, juce::Font::bold));
        g.drawText (juce::String (strip.getNumber()), top.toNearestInt(), juce::Justification::centred);
        auto iconArea = r.removeFromTop (r.getHeight() * 0.1f);
        drawChannelIcon (g, strip.params.icon.load(), strip.getKind(), iconArea.reduced (iconArea.getHeight() * 0.15f), colours::caption);
        auto name = r.removeFromTop (14.0f);
        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawFittedText (stripName (strip).toUpperCase(), name.toNearestInt(), juce::Justification::centred, 1, 0.6f);
        groupKey = r.removeFromTop (juce::jlimit (16.0f, 24.0f, r.getHeight() * 0.1f)).reduced (r.getWidth() * 0.2f, 1.0f);
        const int mode = strip.params.postInsert.mode.load();
        g.setColour (mode == 1 ? colours::fader : Colours::active);
        g.fillRect (groupKey);
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (groupKey.getHeight() * 0.8f, juce::Font::bold));
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
        auto tag = juce::Rectangle<float> (juce::jmin (r.getWidth(), 40.0f), 18.0f).withCentre ({ slider.getCentreX(), y });
        g.setColour (Colours::text);
        g.fillRect (tag);
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
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
                                            // NONE and the 16 slots, in two columns.
                                            juce::StringArray items { "NONE" };
                                            for (int i = 0; i < kNumFx; ++i)
                                                items.add (ctx.engine.getFx().describe (i));
                                            popups::ListOptions options;
                                            options.columns = 2;
                                            options.caption = "FX PROCESSOR";
                                            popups::chooseFrom (field, items, ins.fx.load() + 1, [this] (int i) { ins.fx = i - 1; }, options);
                                        });
        processor->dotsKey = true;
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
        // FX SPILL puts the effect on the left fader section (the spill layers, W40).
        spill = &key ("FX SPILL", Icon::Faders);
        spill->outlined = true;
        spill->setEnabled (false);
        if (strip.isOutput())
        {
            setCust = &key ("SET CUST", Icon::Edit);
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
        header = b.removeFromTop (juce::jlimit (32, 60, b.getHeight() / 9));
        const float u = (float) header.getWidth() / 1160.0f;
        auto span = [u, this] (juce::Rectangle<int> row, float x0, float x1)
        { return juce::Rectangle<int> (header.getX() + juce::roundToInt (x0 * u), row.getY(), juce::roundToInt ((x1 - x0) * u), row.getHeight()); };
        auto h = header.reduced (0, juce::jmax (3, header.getHeight() / 9));
        on->setBounds (span (h, 16.0f, 112.0f));
        const bool am = automix();
        b.removeFromTop (juce::jmax (4, getHeight() / 100));
        row2 = b.removeFromTop (juce::jlimit (36, 76, b.getHeight() / 6));
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
            openEditor->setBounds (b.withSizeKeepingCentre (juce::jmin (200, b.getWidth()), 40));
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
        g.setFont (juce::FontOptions (stripH * 0.82f, juce::Font::plain));
        g.drawText (shortLabel (main), label.toNearestInt(), juce::Justification::centred);
        auto name = r.removeFromTop (nameH);
        g.setColour (colours::caption);
        g.setFont (juce::FontOptions (juce::jmin (13.0f, nameH * 0.7f), juce::Font::plain));
        g.drawFittedText (hasDefaultName (main) ? juce::String() : stripName (main), name.toNearestInt(), juce::Justification::centred, 1, 0.6f);
        g.setColour (colours::panel);
        g.fillRect (boxArea);
        g.setColour (colours::caption);
        g.setFont (juce::FontOptions (juce::jmin (14.0f, boxArea.getHeight() * 0.22f), juce::Font::plain));
        g.drawText ("MAIN", boxArea.withHeight (boxArea.getHeight() * 0.3f).toNearestInt(), juce::Justification::centred);
    }

    void resized() override
    {
        stripH = (float) juce::jlimit (11, 18, getHeight() / 33);
        nameH = (float) juce::jlimit (12, 22, getHeight() / 26);
        auto r = getLocalBounds().reduced (juce::jmax (2, getWidth() * 6 / 100), 0);
        r.removeFromTop ((int) (stripH + nameH) + 2);
        boxArea = r.removeFromTop (juce::jlimit (40, 96, getHeight() * 17 / 100)).toFloat();
        pre.setBounds (boxArea.toNearestInt().withTrimmedTop ((int) (boxArea.getHeight() * 0.3f)).reduced (4, 4));
        r.removeFromTop (6);
        r.removeFromBottom (6);
        on.setBounds (r.removeFromBottom (juce::jlimit (18, 34, getHeight() / 11)));
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
        const int headerH = juce::jlimit (32, 60, getHeight() / 9);
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
    }

    void paint (juce::Graphics& g) override
    {
        HomeScreen::paint (g);
        // A grid of small grey dots behind the pan graphic.
        g.setColour (juce::Colour (0xff303030));
        const float step = juce::jmax (6.0f, (float) getHeight() / 48.0f);
        for (float y = (float) field.getY() + step * 0.5f; y < (float) field.getBottom(); y += step)
            for (float x = (float) field.getX() + step * 0.5f; x < (float) field.getRight(); x += step)
                g.fillRect (x, y, 1.0f, 1.0f);

        for (auto [area, text] : { std::make_pair (channelHeader, strip.getLabel().replace ("CH ", "CHANNEL ").replace ("MTX ", "MATRIX ")),
                                   std::make_pair (sendsHeader, juce::String ("MAIN SENDS")) })
        {
            if (area.isEmpty())
                continue;
            g.setColour (colours::panel);
            g.fillRect (area);
            g.setColour (colours::caption);
            g.setFont (juce::FontOptions (juce::jlimit (14.0f, 26.0f, (float) area.getHeight() * 0.42f), juce::Font::plain));
            g.drawText (text, area, juce::Justification::centred);
        }

        // LR VECTOR SCOPE: needs the engine's L/R sample ring (W38): the empty
        // scope, a dark green dotted square over its caption.
        auto s = scopeArea.toFloat();
        auto caption = s.removeFromBottom (juce::jlimit (11.0f, 18.0f, s.getHeight() * 0.12f));
        g.setColour (colours::outline);
        g.fillRect (s.expanded (1.5f).withBottom (caption.getBottom()));
        g.setColour (juce::Colour (0xff101810));
        g.fillRect (s);
        g.setColour (juce::Colour (0xff1c3020));
        const float d = s.getWidth() / 16.0f;
        for (int i = 1; i < 16; ++i)
            for (int j = 1; j < 16; ++j)
                g.fillRect (s.getX() + d * (float) i, s.getY() + s.getHeight() / 16.0f * (float) j, 1.0f, 1.0f);
        g.setColour (juce::Colour (0xff202020));
        g.fillRect (caption);
        g.setColour (colours::caption);
        g.setFont (juce::FontOptions (caption.getHeight() * 0.8f, juce::Font::plain));
        g.drawText ("LR VECTOR SCOPE", caption.toNearestInt(), juce::Justification::centred);
    }

private:
    bool hasMainSends = false;
    std::vector<MainSendColumn*> sends;
    DelayPanel* delay = nullptr;
    PanWidthDisplay* pan = nullptr;
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

    /** A matrix send: TAP (PRE) / POST, no pan link. */
    SendOptions (MatrixSendParams& m, bool inputSource, bool canIgnoreMute)
        : mode (juce::StringArray { sendModeName (0, inputSource), "POST" }, [&m] { return m.pre.load() ? 0 : 1; }, [&m] (int i) { m.pre = i == 0; }),
          panBox (wingPan ("PAN", m.pan)), canIgnore (canIgnoreMute)
    {
        bindKey (ignoreMute, m.ignoreMute);
        link.setVisible (false);
        isTap = [&m] { return m.pre.load(); };
        isLinked = [] { return false; };
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
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
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
        g.setFont (juce::FontOptions (stripH * 0.82f, juce::Font::plain));
        g.drawText (shortLabel (dst), label.toNearestInt(), juce::Justification::centred);
        auto name = r.removeFromTop (nameH);
        g.setColour (colours::caption);
        g.setFont (juce::FontOptions (juce::jmin (13.0f, nameH * 0.7f), juce::Font::plain));
        g.drawFittedText (hasDefaultName (dst) ? juce::String() : stripName (dst), name.toNearestInt(), juce::Justification::centred, 1, 0.6f);

        // The mode and pan box.
        g.setColour (colours::panel);
        g.fillRect (boxArea);
        auto box = boxArea;
        auto modeRow = box.removeFromTop (juce::jlimit (12.0f, 20.0f, boxArea.getHeight() * 0.24f));
        modeArea = modeRow;
        g.setFont (juce::FontOptions (juce::jmin (14.0f, modeRow.getHeight() * 0.85f), juce::Font::plain));
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
            g.drawText (sendModeName (0, inputSource || mtx != nullptr ? inputSource : false), modeRow.withTrimmedRight (modeRow.getWidth() * 0.5f).toNearestInt(),
                        juce::Justification::centred);
            g.setColour (mode == 1 ? colours::post : colours::dim);
            g.drawText ("POST", modeRow.withTrimmedLeft (modeRow.getWidth() * 0.5f).toNearestInt(), juce::Justification::centred);
        }

        // The pan ring, PAN LINK under it.
        auto linkArea = box.removeFromBottom (juce::jlimit (10.0f, 18.0f, boxArea.getHeight() * 0.22f));
        panArea = box.reduced (2.0f);
        const bool linked = send != nullptr && send->panLink.load();
        const float chPan = send != nullptr && send->panReversed.load() ? -src.params.pan.load() : src.params.pan.load();
        const float panValue = linked ? chPan : (send != nullptr ? send->pan.load() : mtx->pan.load());
        paintSendPan (g, panArea, panValue, linked ? src.params.width.load() : 100.0f, isOnNow());
        if (send != nullptr)
        {
            g.setColour (linked ? colours::link : colours::dim);
            g.setFont (juce::FontOptions (juce::jmin (13.0f, linkArea.getHeight() * 0.85f), juce::Font::plain));
            g.drawFittedText (linked && send->panReversed.load() ? "LINK REV" : "PAN LINK", linkArea.toNearestInt(), juce::Justification::centred, 1, 0.6f);
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
        stripH = (float) juce::jlimit (11, 18, getHeight() / 33);
        nameH = (float) juce::jlimit (12, 22, getHeight() / 26);
        auto r = getLocalBounds().reduced (juce::jmax (2, getWidth() * 6 / 100), 0);
        r.removeFromTop ((int) (stripH + nameH) + 2);
        boxArea = r.removeFromTop (juce::jlimit (40, 96, getHeight() * 17 / 100)).toFloat();
        r.removeFromTop (6);
        if (busSource())
            pre.setBounds (boxArea.toNearestInt().withTrimmedTop (juce::jlimit (12, 20, (int) (boxArea.getHeight() * 0.24f))).reduced (3, 4));
        r.removeFromBottom (6);
        on.setBounds (r.removeFromBottom (juce::jlimit (18, 34, getHeight() / 11)));
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
        const float nodeD = juce::jlimit (12.0f, 26.0f, chain.getHeight() * 0.22f);
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
                g.setFont (juce::FontOptions (blockH * 0.6f, juce::Font::plain));
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
            g.setFont (juce::FontOptions (nodeD * 0.7f, juce::Font::bold));
            g.drawText (juce::String (it.node + 1), disc.toNearestInt(), juce::Justification::centred);
            auto caption = juce::Rectangle<float> (unit * 2.6f, chain.getHeight() * 0.2f).withCentre ({ c.x, lineY + chain.getHeight() * 0.42f });
            g.setColour (sel ? colours::ink : colours::caption);
            g.setFont (juce::FontOptions (juce::jlimit (9.0f, 16.0f, caption.getHeight() * 0.8f), juce::Font::plain));
            g.drawFittedText (it.text, caption.toNearestInt(), juce::Justification::centred, 1, 0.6f);
            hits.push_back ({ disc.expanded (nodeD * 0.4f).getUnion (caption), it.node });
            x += unit * 1.6f;
        }
        if (strip.hasSelectableTap())
        {
            // The end of the chain: post-processing sends.
            g.setColour (colours::caption);
            g.setFont (juce::FontOptions (juce::jlimit (9.0f, 16.0f, chain.getHeight() * 0.16f), juce::Font::plain));
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

/** BUS SENDS / MATRIX SENDS (Figs. 66-67): the TAP POINT diagram, TAP WIDTH and
    TAP EQ in the header; a column per bus (16) or matrix (8). */
class SendsTab final : public HomeScreen
{
public:
    SendsTab (UiContext& c, Strip& s, bool matrixPage) : HomeScreen (c, s), matrix (matrixPage || s.getKind() == StripKind::Main)
    {
        // engine (report E7): TAP WIDTH and TAP EQ of the TAP-mode sends.
        KnobBinding width;
        width.label = "TAP WIDTH";
        width.get = [] { return 0.75f; };
        width.text = [] { return juce::String ("100 %"); };
        tapWidth = &make<KnobBox> (width);
        tapWidth->ringColour = colours::panArc;
        tapWidth->setEnabled (false);
        tapEq = &key ("TAP EQ", std::nullopt);
        tapEq->setEnabled (false);
        tapEqMore = &key ({}, Icon::ChevronRight);
        tapEqMore->setEnabled (false);
        // SPILL puts the strip's sends on a fader section (the spill layers, W40).
        spill = &key ("SPILL", Icon::Faders);
        spill->setEnabled (false);
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

    std::vector<HomeKnob> knobs() override
    {
        std::vector<HomeKnob> k;
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
        header = b.removeFromTop (juce::jlimit (32, 60, b.getHeight() / 9));
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
            chain->setBounds (b.removeFromTop (0).withHeight (juce::jlimit (70, 150, getHeight() / 5)));

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
        g.setFont (juce::FontOptions (juce::jlimit (14.0f, 24.0f, h.getHeight() * 0.42f), juce::Font::plain));
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
        g.setFont (juce::FontOptions (juce::jlimit (10.0f, 18.0f, r.getHeight() * 0.36f), juce::Font::plain));
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
            g.setFont (juce::FontOptions (juce::jlimit (9.0f, 16.0f, box.getHeight() * 0.62f), juce::Font::plain));
            g.drawFittedText (text, box.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.6f);
        }
        g.setColour (colours::tap);
        g.fillEllipse (circle);
        g.setColour (colours::ink);
        g.setFont (juce::FontOptions (circle.getHeight() * 0.72f, juce::Font::bold));
        g.drawText (juce::String (tap + 1), circle.toNearestInt(), juce::Justification::centred);
    }

    bool matrix;
    std::vector<SendColumn*> columns;
    KnobBox* tapWidth = nullptr;
    IconKey *tapEq = nullptr, *tapEqMore = nullptr, *tapMenu = nullptr, *spill = nullptr;
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
        d.inputLeft = s.inputLeft.load();
        d.inputRight = s.inputRight.load();
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
    if (has (EqBit) && src.getEqTable() == dst.getEqTable())
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
        if (dst.getKind() != StripKind::Matrix && src.getKind() != StripKind::Matrix)
            for (int i = 0; i < kNumMatrices; ++i)
            {
                d.matrix[i].on = s.matrix[i].on.load();
                d.matrix[i].pre = s.matrix[i].pre.load();
                d.matrix[i].ignoreMute = s.matrix[i].ignoreMute.load();
                d.matrix[i].levelDb = s.matrix[i].levelDb.load();
                d.matrix[i].pan = s.matrix[i].pan.load();
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
        for (int i = 0; i < kNumScopeBits; ++i)
        {
            auto* k = scopeKeys.add (new IconKey (scopeName (i) + juce::String (i == Send || i == MainBit ? juce::String::fromUTF8 (" \xe2\x96\xbe") : juce::String()),
                                                  std::nullopt, colours::accent));
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
            auto* t = tabs.add (new WingTab (tabNames[k]));
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
        drawSectionBox (g, scopePanel, "SCOPE");
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
            paintStripTile (g, r, *list[(size_t) i], sel, isSource);
            tiles.push_back ({ r.toNearestInt(), ref });
        }
    }

    void layoutContent (juce::Rectangle<int> b) override
    {
        scopePanel = b.removeFromLeft (b.getWidth() * 40 / 100);
        b.removeFromLeft (6);
        destPanel = b;

        auto s = sectionContent (scopePanel, true);
        auto top = s.removeFromTop (juce::jlimit (30, 52, s.getHeight() / 7));
        if (copy)
            sourceTile = top.removeFromLeft (juce::jmin (top.getWidth() / 3, top.getHeight() * 3 / 2));
        top.removeFromLeft (8);
        none.setBounds (top.removeFromLeft (top.getWidth() / 2).reduced (2, 2));
        all.setBounds (top.reduced (2, 2));
        s.removeFromTop (8);
        action.setBounds (s.removeFromBottom (juce::jlimit (28, 44, s.getHeight() / 7)).withSizeKeepingCentre (juce::jmin (160, s.getWidth()), juce::jlimit (28, 44, s.getHeight() / 7)));
        s.removeFromBottom (8);
        // CONN CONFIG CUST TAGS / IN FILTER GATE DYN / EQ DELAY PRE POST / SEND MAIN / FDR MUTE PAN
        const int layout[5][4] = { { Conn, Config, Cust, Tags }, { In, FilterBit, GateBit, DynBit }, { EqBit, DelayBit, Pre, Post },
                                   { Send, MainBit, -1, -1 }, { Fdr, MuteBit, PanBit, -1 } };
        const int kh = s.getHeight() / 5, kw = s.getWidth() / 4;
        for (int r = 0; r < 5; ++r)
            for (int col = 0; col < 4; ++col)
                if (layout[r][col] >= 0)
                    scopeKeys[layout[r][col]]->setBounds (juce::Rectangle<int> (s.getX() + col * kw, s.getY() + r * kh, kw, kh).reduced (3, 4));

        auto d = sectionContent (destPanel, true);
        auto tabRow = d.removeFromTop (juce::jlimit (24, 36, d.getHeight() / 9));
        const int tw = tabRow.getWidth() / kNumStripKinds;
        for (auto* t : tabs)
            t->setBounds (tabRow.removeFromLeft (tw).reduced (1, 0));
        d.removeFromTop (6);
        gridArea = d;
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const auto pos = e.getPosition();
        if (copy && sourceTile.contains (pos))
        {
            // Choose another source.
            juce::PopupMenu m;
            for (int k = 0; k < kNumStripKinds; ++k)
            {
                juce::PopupMenu sub;
                for (auto& strip : ctx.engine.getStrips (static_cast<StripKind> (k)))
                {
                    const auto ref = state::StripRef::of (*strip);
                    sub.addItem (describeStrip (*strip), true, ref == source, [this, ref] { source = ref; repaint(); });
                }
                static const char* names[] = { "CHANNEL", "AUX", "BUS", "MAIN", "MATRIX" };
                m.addSubMenu (names[k], sub);
            }
            m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (sourceTile)));
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
    IconKey none { "NONE", std::nullopt }, all { "ALL", std::nullopt }, action { "INIT", Icon::Reset, colours::accent };
    juce::OwnedArray<IconKey> scopeKeys;
    juce::OwnedArray<juce::TextButton> tabs;
    juce::Rectangle<int> scopePanel, destPanel, sourceTile, gridArea;
    std::vector<std::pair<juce::Rectangle<int>, state::StripRef>> tiles;
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
        noneKey.onClick = [this] { selection.clear(); repaint(); };
        allChannels.onClick = [this]
        {
            selection.clear();
            for (auto& ch : ctx.engine.getChannels())
                selection.push_back (state::StripRef::of (*ch));
            repaint();
        };
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
        // Mains have no send pan, matrix sends no pan link.
        panKeys->isEnabledAt = [mainTarget, busTarget] (int i) { return i == 0 || (! mainTarget && (busTarget || i < 3)); };
        levelKeys = std::make_unique<Segmented> (juce::StringArray { "KEEP", "SET TO\n-oo dB", "SET TO\n-10 dB", "SET TO\n0 dB" },
                                                 [this] { return levelChoice; }, [this] (int i) { levelChoice = i; }, colours::accent);
        levelKeys->twoLines = true;
        enableKeys = std::make_unique<Segmented> (juce::StringArray { "KEEP", "SET OFF", "SET ON" }, [this] { return enableChoice; }, [this] (int i) { enableChoice = i; }, colours::accent);
        applyKey.onClick = [this] { apply(); };
        for (juce::Component* comp : { (juce::Component*) &noneKey, (juce::Component*) &allChannels, (juce::Component*) modeKeys.get(),
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
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.drawText (text, r, juce::Justification::centredLeft);
        };
        caption (captions[0], "SET SEND MODE", {});
        caption (captions[1], "IGNORE CHANNEL MUTE (TAP ONLY)", {});
        caption (captions[2], "SEND PANNING", {});
        caption (captions[3], "SEND LEVEL", juce::Colour (0xff1e2e44));
        caption (captions[4], "SEND ENABLE", juce::Colour (0xff33331c));

        // The source cells: colour strip, number, mode badge, PAN LINK, pan line.
        cells.clear();
        struct Group
        {
            const MixerEngine::StripList* list;
        };
        std::vector<const Strip*> sources;
        for (auto* list : { &ctx.engine.getChannels(), &ctx.engine.getAux(), &ctx.engine.getBuses() })
            for (auto& st : *list)
                sources.push_back (st.get());
        if (dst->getKind() == StripKind::Matrix)
            for (auto& st : ctx.engine.getMains())
                sources.push_back (st.get());
        const int columns = 8;
        const int rows = ((int) sources.size() + columns - 1) / columns;
        const float w = (float) gridArea.getWidth() / (float) columns;
        const float h = juce::jmin ((float) gridArea.getHeight() / (float) juce::jmax (1, rows), w * 0.75f);
        for (int i = 0; i < (int) sources.size(); ++i)
        {
            const auto& src = *sources[(size_t) i];
            auto r = juce::Rectangle<float> ((float) gridArea.getX() + (float) (i % columns) * w, (float) gridArea.getY() + (float) (i / columns) * h, w, h).reduced (1.5f);
            const auto ref = state::StripRef::of (src);
            bool possible = true;
            int mode = 1;
            bool on = false, link = false;
            float pan = 0.0f;
            describe (*dst, src, possible, on, mode, link, pan);
            const bool sel = std::find (selection.begin(), selection.end(), ref) != selection.end();
            g.setColour (possible ? colours::keyOff : colours::well);
            g.fillRect (r);
            if (possible)
            {
                auto strip = r.removeFromTop (juce::jmax (6.0f, r.getHeight() * 0.2f));
                g.setColour (src.getColour().withMultipliedBrightness (on ? 1.0f : 0.45f));
                g.fillRect (strip);
                g.setColour (on ? Colours::text : Colours::textDim);
                g.setFont (juce::FontOptions (juce::jlimit (8.0f, 13.0f, r.getHeight() * 0.3f), juce::Font::bold));
                g.drawText (shortLabel (src), r.withHeight (r.getHeight() * 0.4f).toNearestInt(), juce::Justification::centred);
                auto badge = r.withTrimmedTop (r.getHeight() * 0.42f).withHeight (r.getHeight() * 0.26f).reduced (r.getWidth() * 0.18f, 0.0f);
                drawChip (g, badge, sendModeName (mode, src.isInput(), true), on ? sendModeColour (mode) : colours::chipOff, juce::Colours::black);
                if (link)
                {
                    g.setColour (colours::stereo);
                    g.setFont (juce::FontOptions (juce::jlimit (7.0f, 10.0f, r.getHeight() * 0.18f), juce::Font::bold));
                    g.drawFittedText ("PAN LINK", r.withTrimmedTop (r.getHeight() * 0.7f).toNearestInt(), juce::Justification::centred, 1, 0.6f);
                }
                else
                {
                    auto line = r.withTrimmedTop (r.getHeight() * 0.8f).reduced (r.getWidth() * 0.12f, 0.0f);
                    g.setColour (Colours::textDim);
                    g.fillRect (line.getX(), line.getCentreY(), line.getWidth(), 1.0f);
                    const float x = line.getCentreX() + pan / 100.0f * line.getWidth() * 0.5f;
                    g.setColour (Colours::text);
                    g.fillRect (x - 1.0f, line.getY(), 2.0f, line.getHeight());
                }
            }
            if (sel)
            {
                g.setColour (juce::Colours::white);
                g.drawRect (juce::Rectangle<float> ((float) gridArea.getX() + (float) (i % columns) * w, (float) gridArea.getY() + (float) (i / columns) * h, w, h).reduced (1.0f), 2.0f);
            }
            if (possible)
                cells.push_back ({ juce::Rectangle<float> ((float) gridArea.getX() + (float) (i % columns) * w, (float) gridArea.getY() + (float) (i / columns) * h, w, h).toNearestInt(), ref });
        }
    }

    void layoutContent (juce::Rectangle<int> b) override
    {
        auto left = b.removeFromLeft (b.getWidth() * 58 / 100);
        auto top = left.removeFromTop (juce::jlimit (26, 40, left.getHeight() / 10));
        noneKey.setBounds (top.removeFromLeft (juce::jmin (90, top.getWidth() / 4)).reduced (2));
        allChannels.setBounds (top.removeFromLeft (juce::jmin (90, top.getWidth() / 3)).reduced (2));
        left.removeFromTop (4);
        gridArea = left;
        b.removeFromLeft (10);

        auto applyRow = b.removeFromBottom (juce::jlimit (28, 40, b.getHeight() / 9));
        applyKey.setBounds (applyRow.removeFromRight (juce::jmin (120, applyRow.getWidth() / 3)));
        b.removeFromBottom (4);
        const int sectionH = b.getHeight() / 5;
        juce::Component* rows[] = { modeKeys.get(), ignoreKeys.get(), panKeys.get(), levelKeys.get(), enableKeys.get() };
        for (int i = 0; i < 5; ++i)
        {
            auto sec = b.removeFromTop (sectionH);
            captions[i] = sec.removeFromTop (16);
            rows[i]->setBounds (sec.removeFromTop (juce::jmin (sec.getHeight() - 4, 40)));
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
        pan = m.pan.load();
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
                if (panChoice == 1)
                    m.pan = 0.0f;
                else if (panChoice == 2)
                    m.pan = src->params.pan.load();
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
    IconKey noneKey { "NONE", Icon::Cancel }, allChannels { "ALL CH", std::nullopt }, applyKey { "APPLY", Icon::Check, colours::accent };
    std::unique_ptr<Segmented> modeKeys, ignoreKeys, panKeys, levelKeys, enableKeys;
    juce::Rectangle<int> gridArea, captions[5];
    int modeChoice = 0, ignoreChoice = 0, panChoice = 0, levelChoice = 0, enableChoice = 0;
    bool dragAdds = true;
};

//==============================================================================
/** RTA SETTINGS / EQ/RTA SETTINGS (Figs. 24-25, D10), docked at the right:
    LO BAND / HI BAND fields (the WING EQ's low and high band types), then the
    RTA rows with the console's choices (`/cfg/rta`, `/$ctl/cfg/rta`). The RTA
    itself is not modelled (W35): its rows show the defaults, disabled. */
class SettingsScreen final : public OverlayScreen
{
public:
    SettingsScreen (Strip& s, bool eqSettings) : OverlayScreen (eqSettings ? "EQ/RTA SETTINGS" : "RTA SETTINGS"), strip (s), eq (eqSettings)
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
                field->dotsKey = true;
                addAndMakeVisible (field);
            }
        }
        // Label, the console's choices, the default shown.
        struct Row
        {
            const char* label;
            juce::StringArray choices;
            int shown;
        };
        const Row rows[] = { { "RTA SOURCE", eq ? juce::StringArray { "PRE", "POST" } : juce::StringArray { "IN", "EQ", "POST" }, eq ? 1 : 2 },
                             { "RTA RANGE", { "30 dB", "60 dB" }, 1 },
                             { "RTA DECAY", { "SLOW", "MED", "FAST" }, 1 },
                             { "RTA DETECTOR", { "PEAK", "RMS", "AVG" }, 1 } };
        for (auto& row : rows)
        {
            auto* field = rta.add (new ChoiceField ({}, [text = row.choices[row.shown]] { return text; }, {}));
            field->dotsKey = true;
            field->setEnabled (false);
            rtaLabels.add (row.label);
            addAndMakeVisible (field);
        }
        autoGain.setEnabled (false);
        addAndMakeVisible (autoGain);
        sizeMode = std::make_unique<ChoiceField> ("SIZE/MODE", [this] { return juce::String (eq ? "1/4" : "1/3"); }, nullptr);
        colour = std::make_unique<ChoiceField> ("COLOR", [] { return juce::String ("BL75"); }, nullptr);
        KnobBinding gain;
        gain.label = "FIXED GAIN";
        gain.get = [] { return 0.1f; };
        gain.text = [] { return juce::String ("0 dB"); };
        fixedGain = std::make_unique<KnobBox> (gain);
        for (juce::Component* c : { (juce::Component*) sizeMode.get(), (juce::Component*) colour.get(), (juce::Component*) fixedGain.get() })
        {
            c->setEnabled (false);
            addAndMakeVisible (c);
        }
        sizeMode->dotsKey = colour->dotsKey = true;
    }

    void paint (juce::Graphics& g) override
    {
        OverlayScreen::paint (g);
        g.setColour (colours::panel);
        g.fillRect (body);
        g.setFont (juce::FontOptions (juce::jlimit (12.0f, 18.0f, (float) rowH * 0.4f), juce::Font::plain));
        for (int i = 0; i < rta.size(); ++i)
        {
            g.setColour (colours::dim);
            g.drawText (rtaLabels[i], rta[i]->getBounds().withX (labelX).withWidth (rta[i]->getX() - labelX - 8), juce::Justification::centredLeft);
        }
        g.drawText ("RTA AUTOGAIN", autoGain.getBounds().withX (labelX).withWidth (autoGain.getX() - labelX - 8), juce::Justification::centredLeft);
        g.setFont (juce::FontOptions (12.0f, juce::Font::plain));
        g.drawText ("The RTA (analyser) is not available.", note, juce::Justification::centredLeft);
    }

    void layoutContent (juce::Rectangle<int> b) override
    {
        body = b;
        b = b.reduced (juce::jmax (8, b.getWidth() / 40), 6);
        labelX = b.getX();
        rowH = juce::jlimit (26, 52, b.getHeight() / 9);
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
        for (auto* f : rta)
            control (*f, fieldW);
        control (autoGain, rowH * 3 / 2);
        auto row = b.removeFromTop (rowH);
        b.removeFromTop (rowH / 6);
        sizeMode->setBounds (row.removeFromLeft (fieldW));
        fixedGain->setBounds (row.removeFromRight (fieldW));
        colour->setBounds (b.removeFromTop (rowH).withWidth (fieldW));
        b.removeFromTop (rowH / 6);
        note = b.removeFromTop (20);
    }

private:
    Strip& strip;
    bool eq;
    juce::OwnedArray<ChoiceField> bands, rta;
    juce::StringArray rtaLabels;
    IconKey autoGain { "ON", std::nullopt, colours::accent };
    std::unique_ptr<ChoiceField> sizeMode, colour;
    std::unique_ptr<KnobBox> fixedGain;
    juce::Rectangle<int> body, note;
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
    screen->setPlacement (OverlayScreen::Placement::DockRight, 0.46f);
    return screen;
}

} // namespace livedaw::ui
