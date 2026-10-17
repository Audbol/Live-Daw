#include "ui/MonitorsPage.h"
#include "dsp/DspUtils.h"
#include "ui/WingPopups.h"

namespace livedaw::ui
{

namespace
{
    // The toolbar's colours (fig. 72).
    const juce::Colour talkYellow = KnobColours::yellow, pathLime = Colours::lime, dimGreen = Colours::green, muteRed = Colours::mute,
                       monoOrange = KnobColours::orange;

    /** The console's meter scale: CLIP, -6, -12, -24, -36, -48, -60 evenly spaced. */
    float meterPos (float db)
    {
        static constexpr float marks[] = { -60.0f, -48.0f, -36.0f, -24.0f, -12.0f, -6.0f, 0.0f };
        if (db <= marks[0])
            return 0.0f;
        for (size_t i = 1; i < std::size (marks); ++i)
            if (db <= marks[i])
                return ((float) (i - 1) + (db - marks[i - 1]) / (marks[i] - marks[i - 1])) / 6.0f;
        return 1.0f;
    }

    void panel (juce::Graphics& g, juce::Rectangle<int> header, const juce::String& title)
    {
        g.setColour (Dark::header);
        g.fillRect (header);
        g.setColour (Dark::text);
        g.setFont (panelTitleFont (header.getHeight()));
        g.drawFittedText (title, header.reduced (6, 0), juce::Justification::centred, 1);
    }

    /** A tab with an icon: lit with a yellow top line and yellow text. */
    void tab (juce::Graphics& g, juce::Rectangle<int> r, Icon icon, const juce::String& text, bool on, bool enabled)
    {
        g.setColour (on ? Dark::tabOn : Dark::tabOff);
        g.fillRect (r);
        const auto ink = on ? talkYellow : (enabled ? Dark::textDim : Dark::textDim.withAlpha (0.4f));
        if (on)
        {
            g.setColour (talkYellow);
            g.fillRect (r.withHeight (3));
        }
        auto ib = r.removeFromLeft (r.getHeight()).reduced (r.getHeight() / 5);
        if (on)
        {
            g.setColour (talkYellow);
            g.fillRect (ib.expanded (2));
            drawIcon (g, icon, ib.toFloat(), juce::Colours::black);
        }
        else
            drawIcon (g, icon, ib.toFloat(), ink);
        g.setColour (ink);
        g.setFont (juce::FontOptions (juce::jmin (18.0f, (float) r.getHeight() * 0.5f), juce::Font::bold));
        g.drawFittedText (text, r.reduced (6, 0), juce::Justification::centredLeft, 1);
    }

    /** A grid of small tiles ("BUS" over "1"), all unselected and dim (talkback isn't here). */
    void tileGrid (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title, const juce::String& caption, int count, int columns)
    {
        auto head = area.removeFromTop (juce::jmin (16, area.getHeight() / 5));
        g.setColour (Dark::key.withAlpha (0.6f));
        g.fillRect (head);
        g.setColour (Dark::textDim);
        g.setFont (juce::FontOptions (juce::jmin (12.0f, (float) head.getHeight() * 0.8f), juce::Font::bold));
        g.drawText (title, head, juce::Justification::centred);
        area.removeFromTop (2);
        const int rows = (count + columns - 1) / columns;
        const int w = area.getWidth() / columns, h = area.getHeight() / juce::jmax (1, rows);
        for (int i = 0; i < count; ++i)
        {
            auto t = juce::Rectangle<int> (area.getX() + (i % columns) * w, area.getY() + (i / columns) * h, w, h).reduced (2);
            g.setColour (Dark::key.withAlpha (0.5f));
            g.fillRect (t);
            g.setColour (Dark::textDim.withAlpha (0.6f));
            g.setFont (juce::FontOptions (juce::jmin (9.0f, (float) t.getHeight() * 0.35f), juce::Font::bold));
            g.drawText (caption, t.removeFromTop (t.getHeight() * 2 / 5), juce::Justification::centred);
            g.setFont (juce::FontOptions (juce::jmin (15.0f, (float) t.getHeight() * 0.7f), juce::Font::bold));
            g.drawText (juce::String (i + 1), t, juce::Justification::centred);
        }
    }

    /** An icon key with its caption under it (INVERT, DELAY, EQ), dim when disabled. */
    void iconKey (juce::Graphics& g, juce::Rectangle<int> r, Icon icon, const juce::String& text, bool enabled)
    {
        const auto ink = enabled ? Dark::text : Dark::textDim.withAlpha (0.45f);
        auto f = r.toFloat();
        auto label = f.removeFromBottom (f.getHeight() * 0.32f);
        g.setColour (Dark::key.withAlpha (enabled ? 1.0f : 0.5f));
        g.fillRect (f);
        drawIcon (g, icon, f.reduced (f.getHeight() * 0.2f), ink);
        g.setColour (ink);
        g.setFont (juce::FontOptions (juce::jlimit (8.0f, 12.0f, label.getHeight() * 0.8f), juce::Font::bold));
        g.drawFittedText (text, label.toNearestInt(), juce::Justification::centred, 1);
    }

    /** A drop-down field (caption over value, dots key), dim when disabled. */
    void field (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& caption, const juce::String& value, bool enabled)
    {
        const float a = enabled ? 1.0f : 0.45f;
        auto key = r.removeFromRight (juce::jmin (r.getHeight() / 2, 22));
        r.removeFromRight (3);
        g.setColour (Dark::well);
        g.fillRect (r);
        g.setColour (Dark::key.withAlpha (a));
        g.fillRect (key);
        g.setColour (Dark::text.withAlpha (a));
        const auto kc = key.getCentre().toFloat();
        const float d = juce::jlimit (3.0f, 5.0f, (float) key.getHeight() * 0.1f);
        for (int i = -1; i <= 1; ++i)
            g.fillEllipse (juce::Rectangle<float> (d, d).withCentre (kc.translated (0.0f, (float) i * d * 2.2f)));
        auto t = r.reduced (6, 3);
        g.setColour (Dark::textDim.withAlpha (a));
        g.setFont (wingFontPx (juce::jmin (TypeScale::caption * getWingScreenHeight(), (float) t.getHeight() * 0.2f)));
        g.drawFittedText (caption, t.removeFromTop (t.getHeight() * 38 / 100), juce::Justification::bottomLeft, 1);
        g.setColour (Dark::text.withAlpha (a));
        g.setFont (wingFontPx (juce::jmin (TypeScale::value * getWingScreenHeight(), (float) t.getHeight() * 0.5f)));
        g.drawFittedText (value, t, juce::Justification::centredLeft, 1, 0.8f);
    }
} // namespace

//==============================================================================
KnobField::KnobField (KnobBinding b, juce::Colour ringColour) : binding (std::move (b)), ring (ringColour)
{
    startTimerHz (10);
}

void KnobField::setBinding (KnobBinding b)
{
    binding = std::move (b);
    repaint();
}

void KnobField::timerCallback()
{
    const auto text = binding.isValid() && binding.text ? binding.text() + juce::String (binding.get()) : juce::String();
    if (text != shown)
    {
        shown = text;
        repaint();
    }
}

void KnobField::paint (juce::Graphics& g)
{
    const bool on = binding.isValid();
    const float a = on ? 1.0f : 0.4f;
    auto r = getLocalBounds();
    g.setColour (Dark::well);
    g.fillRect (r);
    auto knob = r.removeFromLeft (r.getHeight()).toFloat().reduced ((float) r.getHeight() * 0.16f);
    const float a0 = juce::MathConstants<float>::pi * 1.25f, a1 = juce::MathConstants<float>::pi * 2.75f;
    const float rad = knob.getWidth() * 0.42f, thick = juce::jmax (2.5f, knob.getWidth() * 0.16f);
    juce::Path track, arc;
    track.addCentredArc (knob.getCentreX(), knob.getCentreY(), rad, rad, 0.0f, a0, a1, true);
    g.setColour (Dark::key.withAlpha (a));
    g.strokePath (track, juce::PathStrokeType (thick));
    if (on)
    {
        arc.addCentredArc (knob.getCentreX(), knob.getCentreY(), rad, rad, 0.0f, a0, a0 + (a1 - a0) * juce::jlimit (0.0f, 1.0f, binding.get()), true);
        g.setColour (ring);
        g.strokePath (arc, juce::PathStrokeType (thick));
    }
    auto t = r.reduced (4, 3);
    g.setColour (Dark::textDim.withAlpha (a));
    g.setFont (wingFontPx (juce::jmin (TypeScale::caption * getWingScreenHeight(), (float) t.getHeight() * 0.2f)));
    g.drawFittedText (on ? binding.label.toUpperCase() : placeholder, t.removeFromTop (t.getHeight() * 38 / 100), juce::Justification::bottomLeft, 1);
    g.setColour (Dark::text.withAlpha (a));
    g.setFont (wingFontPx (juce::jmin (TypeScale::value * getWingScreenHeight(), (float) t.getHeight() * 0.5f)));
    g.drawFittedText (on && binding.text ? binding.text() : placeholderValue, t, juce::Justification::centredLeft, 1, 0.8f);
}

void KnobField::mouseDown (const juce::MouseEvent&)
{
    if (binding.isValid())
        dragStart = binding.get();
}

void KnobField::mouseDrag (const juce::MouseEvent& e)
{
    if (binding.isValid() && binding.set)
    {
        binding.set (juce::jlimit (0.0f, 1.0f, dragStart - (float) e.getDistanceFromDragStartY() / 200.0f));
        repaint();
    }
}

void KnobField::mouseDoubleClick (const juce::MouseEvent&)
{
    if (binding.isValid() && binding.reset)
    {
        binding.reset();
        repaint();
    }
}

void KnobField::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    if (binding.isValid() && binding.set)
    {
        binding.set (juce::jlimit (0.0f, 1.0f, binding.get() + (w.deltaY > 0.0f ? 0.01f : -0.01f)));
        repaint();
    }
}

//==============================================================================
MonitorsPage::MonitorsPage (UiContext& c) : ctx (c)
{
    auto& e = ctx.engine;
    auto flip = [] (std::atomic<bool>& b) { b = ! b.load(); };
    tools = {
        { "TALK A", Icon::ChevronUp, talkYellow, [] { return false; }, nullptr, {} },
        { "TALK B", Icon::ChevronUp, talkYellow, [] { return false; }, nullptr, {} },
        { "PHONES", Icon::Headphones, pathLime, [] { return false; }, nullptr, {} },
        { "SPEAKERS", Icon::Speaker, pathLime, [] { return false; }, nullptr, {} },
        { "PH+SPK", Icon::Speakers, pathLime, [] { return true; }, nullptr, {} },
        { "DIM", Icon::ChevronDown, dimGreen, [&e] { return e.monitorDim.load(); }, [&e, flip] { flip (e.monitorDim); }, {} },
        { "MUTE", Icon::Cancel, muteRed, [&e] { return e.monitorMute.load(); }, [&e, flip] { flip (e.monitorMute); }, {} },
        { "MONO", Icon::Circle, monoOrange, [&e] { return e.monitorMono.load(); }, [&e, flip] { flip (e.monitorMono); }, {} },
        { "LR SWAP", Icon::WidthArrows, monoOrange, [&e] { return e.monitorSwap.load(); }, [&e, flip] { flip (e.monitorSwap); }, {} },
    };

    // What the engine has; the rest is drawn as the console's controls, disabled.
    dim.setBinding (bindLinear ("DIM", e.monitorDimDb, 0.0f, 40.0f, " dB", 0, 20.0f));
    for (auto [k, label, value] : { std::tuple { &srcLevel, "SRC LEVEL", "0.0 dB" }, std::tuple { &srcMix, "SRC MIX", "-oo dB" },
                                    std::tuple { &pflDim, "PFL DIM", "12 dB" }, std::tuple { &bandSolo, "BAND SOLO", "12 dB" },
                                    std::tuple { &busDim, "BUS DIM", "9 dB" }, std::tuple { &monDim, "MON DIM", "9 dB" },
                                    std::tuple { &limiter, "LIMITER", "0 dB" } })
    {
        k->placeholder = label;
        k->placeholderValue = value;
    }
    for (auto* k : { &srcLevel, &srcMix, &dim, &pflDim, &bandSolo, &busDim, &monDim, &limiter })
        addAndMakeVisible (k);

    soloKeys = std::make_unique<ChoiceKeys> (juce::StringArray { "PFL", "AFL" }, [&e] { return e.soloAfl.load() ? 1 : 0; },
                                             [&e] (int i) { e.soloAfl = i == 1; }, ChoiceKeys::prePost());
    addAndMakeVisible (*soloKeys);
    switchMode = std::make_unique<ChoiceKeys> (juce::StringArray { "AUTO", "PUSH", "LATCH" }, [] { return 0; }, nullptr);
    switchMode->setEnabled (false);
    addAndMakeVisible (*switchMode);

    anchor.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (anchor);
    startTimerHz (20);
}

MonitorsPage::~MonitorsPage() = default;

std::vector<KnobBinding> MonitorsPage::getKnobs()
{
    // The parameter bar: TALK LVL, SRC LEVEL, SRC MIX, PFL DIM, DIST on the
    // console; here the monitor level and the DIM amount.
    return { bindLinear ("LEVEL", ctx.engine.monitorLevelDb, -60.0f, 10.0f, " dB", 1, 0.0f), dim.getBinding() };
}

StatusTitle MonitorsPage::getStatusTitle()
{
    StatusTitle t;
    t.kind = StatusTitle::Kind::Text;
    t.text = "MONITORS";
    t.icon = Icon::Monitors;
    return t;
}

juce::String MonitorsPage::outputName (int port) const
{
    if (port < 0)
        return "OFF";
    const auto& names = ctx.engine.getHardwareOutputNames();
    return port < names.size() && names[port].isNotEmpty() ? names[port].toUpperCase() : "OUT " + juce::String (port + 1);
}

void MonitorsPage::chooseOutput (bool right)
{
    auto& target = right ? ctx.engine.monitorOutputRight : ctx.engine.monitorOutputLeft;
    juce::StringArray items { "OFF" };
    const int n = juce::jmax (64, ctx.engine.getHardwareOutputNames().size());
    for (int i = 0; i < n; ++i)
        items.add (outputName (i));
    popups::ListOptions options;
    options.columns = 2;
    options.caption = right ? "MONITOR R" : "MONITOR L";
    anchor.setBounds (right ? rightField : leftField);
    juce::Component::SafePointer<MonitorsPage> safe (this);
    popups::chooseFrom (anchor, items, target.load() + 1, [safe, &target] (int i)
    {
        target = i - 1;
        if (safe != nullptr)
            safe->repaint();
    }, options);
}

void MonitorsPage::timerCallback()
{
    std::array<float, 2> peak { ctx.meters.getMonitor (0), ctx.meters.getMonitor (1) };
    for (auto& p : peak)
        p = meterPos (dsp::gainToDb (p));
    if (std::abs (peak[0] - shownPeak[0]) > 0.005f || std::abs (peak[1] - shownPeak[1]) > 0.005f)
    {
        shownPeak = peak;
        repaint (meterArea.getUnion (faderArea));
    }
    repaint (toolbar);
}

void MonitorsPage::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
    paintToolbar (g);
    paintTalk (g);
    paintPath (g);
    paintLevel (g);
}

void MonitorsPage::paintToolbar (juce::Graphics& g)
{
    g.setColour (Dark::body);
    g.fillRect (toolbar);
    for (auto& t : tools)
    {
        const bool lit = t.lit && t.lit();
        const bool enabled = t.press != nullptr || lit;
        auto r = t.area.toFloat();
        g.setColour (lit ? t.colour : t.colour.withMultipliedBrightness (0.35f).withMultipliedAlpha (enabled ? 1.0f : 0.6f));
        g.fillRect (r);
        const auto ink = lit ? juce::Colours::black : (enabled ? t.colour : t.colour.withMultipliedAlpha (0.5f));
        auto label = r.removeFromBottom (r.getHeight() * 0.3f);
        drawIcon (g, t.icon, r.reduced (r.getHeight() * 0.18f), ink);
        g.setColour (ink);
        g.setFont (juce::FontOptions (juce::jlimit (7.0f, 11.0f, label.getHeight() * 0.85f), juce::Font::bold));
        g.drawFittedText (t.text, label.toNearestInt(), juce::Justification::centredTop, 1, 0.6f);
    }
}

void MonitorsPage::paintTalk (juce::Graphics& g)
{
    // TALK A / TALK B: talkback isn't in the engine yet, so all of it is dim.
    tab (g, talkTabs.withWidth (talkTabs.getWidth() / 2).reduced (1, 0), Icon::Edit, "TALK A", true, false);
    tab (g, talkTabs.withTrimmedLeft (talkTabs.getWidth() / 2).reduced (1, 0), Icon::Edit, "TALK B", false, false);

    g.setColour (Dark::body);
    g.fillRect (talkArea);
    auto left = switchBox;
    panel (g, left.removeFromTop (juce::jmin (kDarkHeaderHeight, left.getHeight() / 5)), "SWITCH MODE");
    auto dimArea = autoDimBox;
    panel (g, dimArea.removeFromTop (juce::jmin (kDarkHeaderHeight, dimArea.getHeight() / 4)), "AUTO DIM");

    auto sendLevels = busGrid.withHeight (juce::jmin (22, busGrid.getHeight() / 6)).translated (0, -juce::jmin (26, busGrid.getHeight() / 5));
    g.setColour (Dark::textDim.withAlpha (0.6f));
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText ("USE SEND LEVELS", sendLevels, juce::Justification::centredLeft);
    tileGrid (g, busGrid, "TALK TO BUS", "BUS", 16, 4);
    tileGrid (g, mtxGrid, "TALK TO MATRIX", "MTX", 8, 4);
    tileGrid (g, mainGrid, "TALK TO MAIN", "MAIN", 4, 4);
}

void MonitorsPage::paintPath (juce::Graphics& g)
{
    tab (g, pathTabs.withWidth (pathTabs.getWidth() / 2).reduced (1, 0), Icon::Headphones, "PHONES", true, true);
    tab (g, pathTabs.withTrimmedLeft (pathTabs.getWidth() / 2).reduced (1, 0), Icon::Speaker, "SPEAKERS", false, false);
    panel (g, pathHeader, "PHONES SETTINGS");
    g.setColour (Dark::body);
    g.fillRect (pathArea);

    // The monitor follows Main 1 without a solo; DIRECT IN, EQ, delay: not here yet.
    field (g, sourceField, "SOURCE", "MAIN.1", false);
    field (g, directField, "DIRECT IN", "OFF", false);
    iconKey (g, eqKey, Icon::Eq, "EQ", false);
    iconKey (g, invertKey, Icon::Invert, "INVERT", false);
    iconKey (g, delayKey, Icon::DeltaT, "DELAY", false);

    // The distance slider (DIST), disabled.
    g.setColour (Dark::well);
    g.fillRect (distSlider);
    g.setColour (Dark::textDim.withAlpha (0.5f));
    g.fillRect (distSlider.withSizeKeepingCentre (3, distSlider.getHeight() - 16));
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("0.1 m", distSlider.removeFromBottom (14), juce::Justification::centred);

    // The outputs and PFL / AFL (the engine's monitor path).
    auto o = outputBox;
    panel (g, o.removeFromTop (juce::jmin (22, o.getHeight() / 3)), "OUTPUT / SOLO");
    field (g, leftField, "MONITOR L", outputName (ctx.engine.monitorOutputLeft.load()), true);
    field (g, rightField, "MONITOR R", outputName (ctx.engine.monitorOutputRight.load()), true);
}

void MonitorsPage::paintLevel (juce::Graphics& g)
{
    panel (g, levelHeader, "LEVEL");
    g.setColour (Dark::body);
    g.fillRect (levelArea);

    // The meter (L R) with the CLIP .. -60 scale.
    auto m = meterArea.toFloat();
    g.setColour (Dark::well);
    g.fillRect (m);
    auto scale = m.removeFromLeft (m.getWidth() * 0.45f);
    g.setColour (Dark::textDim);
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    for (auto [text, db] : { std::pair { "CLIP", 0.0f }, std::pair { "-6", -6.0f }, std::pair { "-12", -12.0f }, std::pair { "-24", -24.0f },
                             std::pair { "-36", -36.0f }, std::pair { "-48", -48.0f }, std::pair { "-60", -60.0f } })
    {
        const float y = m.getBottom() - 14.0f - (m.getHeight() - 20.0f) * meterPos (db);
        g.drawText (text, juce::Rectangle<float> (scale.getX(), y - 6.0f, scale.getWidth() - 2.0f, 12.0f), juce::Justification::centredRight);
    }
    auto bars = m.reduced (2.0f, 6.0f).withTrimmedBottom (8.0f);
    const float w = bars.getWidth() / 2.0f;
    for (int ch = 0; ch < 2; ++ch)
    {
        auto bar = juce::Rectangle<float> (bars.getX() + w * (float) ch, bars.getY(), w, bars.getHeight()).reduced (2.0f, 0.0f);
        g.setColour (Dark::key);
        g.fillRect (bar);
        const float level = shownPeak[(size_t) ch];
        auto lit = bar.withTop (bar.getBottom() - bar.getHeight() * level);
        g.setColour (level > meterPos (-0.5f) ? Colours::meterRed : level > meterPos (-12.0f) ? Colours::meterYellow : Colours::meterGreen);
        g.fillRect (lit);
    }

    // The level fader: the monitor level, a lime fill and a value flag.
    auto f = faderArea.toFloat();
    g.setColour (Dark::well);
    g.fillRect (f);
    const float pos = fader::dbToPos (juce::jlimit (-144.0f, 10.0f, ctx.engine.monitorLevelDb.load()));
    auto slot = f.withSizeKeepingCentre (juce::jmin (10.0f, f.getWidth() * 0.3f), f.getHeight() - 16.0f);
    g.setColour (Dark::key);
    g.fillRect (slot);
    g.setColour (pathLime);
    g.fillRect (slot.withTop (slot.getBottom() - slot.getHeight() * pos));
    const float y = slot.getBottom() - slot.getHeight() * pos;
    auto flag = juce::Rectangle<float> (f.getWidth() * 0.9f, 16.0f).withCentre ({ f.getCentreX(), juce::jlimit (f.getY() + 8.0f, f.getBottom() - 8.0f, y) });
    g.setColour (juce::Colours::white);
    g.fillRect (flag);
    g.setColour (juce::Colours::black);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawFittedText (fader::text (ctx.engine.monitorLevelDb.load()) + " dB", flag.toNearestInt(), juce::Justification::centred, 1, 0.6f);
}

void MonitorsPage::resized()
{
    auto b = getLocalBounds();
    toolbar = b.removeFromLeft (juce::jlimit (48, 90, getWidth() * 8 / 100));
    b.removeFromLeft (4);
    const int tabH = juce::jlimit (26, 44, getHeight() / 12);
    const int gap = 4;

    // Toolbar keys, top to bottom.
    {
        auto t = toolbar.reduced (4, 4);
        const int h = t.getHeight() / (int) tools.size();
        for (auto& k : tools)
            k.area = t.removeFromTop (h).reduced (0, 2);
    }

    auto talk = b.removeFromLeft ((b.getWidth() - gap) * 45 / 100);
    b.removeFromLeft (gap);
    auto right = b;

    talkTabs = talk.removeFromTop (tabH);
    talk.removeFromTop (gap);
    talkArea = talk;
    {
        auto t = talkArea.reduced (4);
        auto col = t.removeFromLeft (t.getWidth() * 40 / 100);
        t.removeFromLeft (6);
        switchBox = col.removeFromTop (col.getHeight() * 46 / 100);
        col.removeFromTop (gap);
        autoDimBox = col;
        auto sw = switchBox.withTrimmedTop (juce::jmin (kDarkHeaderHeight, switchBox.getHeight() / 5) + 6).reduced (8, 0);
        switchMode->setBounds (sw.withHeight (juce::jmin (sw.getHeight() / 3, 34)));
        auto ad = autoDimBox.withTrimmedTop (juce::jmin (kDarkHeaderHeight, autoDimBox.getHeight() / 4) + 6).reduced (6, 0);
        const int kh = juce::jmin (48, ad.getHeight() / 2 - 4);
        busDim.setBounds (ad.removeFromTop (kh));
        ad.removeFromTop (6);
        monDim.setBounds (ad.removeFromTop (kh));

        t.removeFromTop (juce::jmin (28, t.getHeight() / 8));
        busGrid = t.removeFromTop (t.getHeight() * 50 / 100);
        t.removeFromTop (6);
        mtxGrid = t.removeFromTop (t.getHeight() * 55 / 100);
        t.removeFromTop (6);
        mainGrid = t;
    }

    auto level = right.removeFromRight (juce::jlimit (90, 200, right.getWidth() * 30 / 100));
    right.removeFromRight (gap);
    pathTabs = right.removeFromTop (tabH);
    right.removeFromTop (gap);
    pathHeader = right.removeFromTop (kDarkHeaderHeight);
    right.removeFromTop (2);
    pathArea = right;
    {
        auto p = pathArea.reduced (4);
        const int rowH = juce::jlimit (30, 52, p.getHeight() / 7);
        auto top = p.removeFromTop (rowH);
        sourceField = top.removeFromLeft (top.getWidth() / 2 - 3);
        top.removeFromLeft (6);
        directField = top;
        p.removeFromTop (6);
        auto knobs = p.removeFromLeft (p.getWidth() / 2 - 3);
        p.removeFromLeft (6);
        for (auto* k : { &srcLevel, &srcMix, &dim, &pflDim, &bandSolo })
        {
            k->setBounds (knobs.removeFromTop (rowH).reduced (0, 2));
            knobs.removeFromTop (2);
        }
        // Right column: EQ, the distance slider with INVERT and DELAY, then the outputs.
        eqKey = p.removeFromTop (rowH).reduced (0, 2);
        p.removeFromTop (4);
        outputBox = p.removeFromBottom (juce::jmin (p.getHeight() / 2, rowH * 3 + 26));
        p.removeFromBottom (4);
        distSlider = p.removeFromLeft (p.getWidth() / 2 - 3);
        p.removeFromLeft (6);
        invertKey = p.removeFromTop (p.getHeight() / 2).reduced (0, 2);
        delayKey = p.reduced (0, 2);
        auto o = outputBox.withTrimmedTop (juce::jmin (22, outputBox.getHeight() / 3) + 4);
        const int fh = juce::jmax (18, (o.getHeight() - 8) / 3);
        leftField = o.removeFromTop (fh);
        o.removeFromTop (4);
        rightField = o.removeFromTop (fh);
        o.removeFromTop (4);
        soloBox = o;
        soloKeys->setBounds (soloBox.reduced (0, 1));
    }

    levelHeader = level.removeFromTop (tabH + gap + kDarkHeaderHeight);
    levelHeader = levelHeader.withTrimmedTop (tabH + gap);
    level.removeFromTop (2);
    levelArea = level;
    {
        auto l = levelArea.reduced (4);
        limiter.setBounds (l.removeFromTop (juce::jlimit (30, 52, l.getHeight() / 7)).reduced (0, 2));
        l.removeFromTop (6);
        meterArea = l.removeFromLeft (l.getWidth() / 2);
        faderArea = l.withTrimmedLeft (4);
    }
}

void MonitorsPage::mouseUp (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();
    for (auto& t : tools)
        if (t.area.contains (pos) && t.press)
        {
            t.press();
            repaint (toolbar);
            return;
        }
    if (leftField.contains (pos))
        chooseOutput (false);
    else if (rightField.contains (pos))
        chooseOutput (true);
}

} // namespace livedaw::ui
