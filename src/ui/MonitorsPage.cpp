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

    /** A tab with an icon (an `Icon`, or a WING icon number when `wingIcon` >= 0):
        lit with a yellow top line and yellow text. */
    void tab (juce::Graphics& g, juce::Rectangle<int> r, Icon icon, const juce::String& text, bool on, bool enabled, int wingIcon = -1)
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
        auto draw = [&] (juce::Colour c)
        {
            if (wingIcon < 0 || ! drawWingIcon (g, wingIcon, ib.toFloat(), c))
                drawIcon (g, icon, ib.toFloat(), c);
        };
        if (on)
        {
            g.setColour (talkYellow);
            g.fillRect (ib.expanded (2));
            draw (juce::Colours::black);
        }
        else
            draw (ink);
        g.setColour (ink);
        g.setFont (juce::FontOptions (juce::jmin (18.0f, (float) r.getHeight() * 0.5f), juce::Font::bold));
        g.drawFittedText (text, r.reduced (6, 0), juce::Justification::centredLeft, 1);
    }

    /** The tiles of a grid (count tiles, `columns` wide) under its title row. */
    std::vector<juce::Rectangle<int>> gridTiles (juce::Rectangle<int> area, int count, int columns)
    {
        area.removeFromTop (juce::jmin (16, area.getHeight() / 5) + 2);
        const int rows = (count + columns - 1) / columns;
        const int w = area.getWidth() / columns, h = area.getHeight() / juce::jmax (1, rows);
        std::vector<juce::Rectangle<int>> tiles;
        for (int i = 0; i < count; ++i)
            tiles.push_back (juce::Rectangle<int> (area.getX() + (i % columns) * w, area.getY() + (i / columns) * h, w, h).reduced (2));
        return tiles;
    }

    /** A TALK TO grid: the title row, then small tiles ("BUS" over "1"), lit
        yellow for the destinations in `mask`. */
    void tileGrid (juce::Graphics& g, juce::Rectangle<int> area, const std::vector<juce::Rectangle<int>>& tiles, const juce::String& title,
                   const juce::String& caption, uint32_t mask)
    {
        auto head = area.removeFromTop (juce::jmin (16, area.getHeight() / 5));
        g.setColour (Dark::key);
        g.fillRect (head);
        g.setColour (Dark::textDim);
        g.setFont (juce::FontOptions (juce::jmin (12.0f, (float) head.getHeight() * 0.8f), juce::Font::bold));
        g.drawText (title, head, juce::Justification::centred);
        for (size_t i = 0; i < tiles.size(); ++i)
        {
            auto t = tiles[i];
            const bool on = (mask >> i) & 1u;
            g.setColour (on ? talkYellow : Dark::key);
            g.fillRect (t);
            g.setColour (on ? juce::Colours::black : Dark::textDim);
            g.setFont (juce::FontOptions (juce::jmin (9.0f, (float) t.getHeight() * 0.35f), juce::Font::bold));
            g.drawText (caption, t.removeFromTop (t.getHeight() * 2 / 5), juce::Justification::centred);
            g.setColour (on ? juce::Colours::black : Dark::text);
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
    // PHONES / SPEAKERS / PH+SPK pick the solo bus (the path the keys below act
    // on) and show that path's settings.
    auto pickPath = [this] (int choice)
    {
        ctx.engine.setSoloBus (choice);
        if (choice != 2)
            showPathTab (choice);
    };
    tools = {
        { "TALK A", Icon::ChevronUp, talkYellow, [&e] { return e.isTalkOn (0); }, nullptr, {}, 0 },
        { "TALK B", Icon::ChevronUp, talkYellow, [&e] { return e.isTalkOn (1); }, nullptr, {}, 1 },
        { "PHONES", Icon::Headphones, pathLime, [&e] { return e.soloBus.load() == 0; }, [pickPath] { pickPath (0); }, {} },
        { "SPEAKERS", Icon::Speaker, pathLime, [&e] { return e.soloBus.load() == 1; }, [pickPath] { pickPath (1); }, {} },
        { "PH+SPK", Icon::Speakers, pathLime, [&e] { return e.soloBus.load() == 2; }, [pickPath] { pickPath (2); }, {} },
        { "DIM", Icon::ChevronDown, dimGreen, [&e] { return e.monitorDim.load(); }, [&e, flip] { flip (e.monitorDim); }, {} },
        { "MUTE", Icon::Cancel, muteRed, [&e] { return e.monitorMute.load(); }, [&e, flip] { flip (e.monitorMute); }, {} },
        { "MONO", Icon::Circle, monoOrange, [&e] { return e.monitorMono.load(); }, [&e, flip] { flip (e.monitorMono); }, {} },
        { "LR SWAP", Icon::WidthArrows, monoOrange, [&e] { return e.monitorSwap.load(); }, [&e, flip] { flip (e.monitorSwap); }, {} },
    };

    // Not in the engine: drawn as the console's controls, disabled.
    for (auto [k, label, value] : { std::tuple { &bandSolo, "BAND SOLO", "12 dB" }, std::tuple { &limiter, "LIMITER", "0 dB" } })
    {
        k->placeholder = label;
        k->placeholderValue = value;
    }
    for (auto* k : { &srcLevel, &srcMix, &dim, &pflDim, &bandSolo, &busDim, &monDim, &limiter })
        addAndMakeVisible (k);

    soloKeys = std::make_unique<ChoiceKeys> (juce::StringArray { "PFL", "AFL" }, [&e] { return e.soloAfl.load() ? 1 : 0; },
                                             [&e] (int i) { e.soloAfl = i == 1; }, ChoiceKeys::prePost());
    addAndMakeVisible (*soloKeys);
    switchMode = std::make_unique<ChoiceKeys> (juce::StringArray { "AUTO", "PUSH", "LATCH" }, [this] { return (int) talkPath().mode.load(); },
                                               [this] (int i) { talkPath().mode = i; });
    addAndMakeVisible (*switchMode);
    sendLevels = std::make_unique<OnKey> ([this] { return talkPath().individual.load(); }, [this] (bool on) { talkPath().individual = on; });
    addAndMakeVisible (*sendLevels);

    showTalkTab (0);
    showPathTab (0);

    anchor.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (anchor);
    startTimerHz (20);
}

MonitorsPage::~MonitorsPage() = default;

ld::MonitorSourceParams& MonitorsPage::pathSource() const noexcept
{
    return showsPhones() ? ctx.engine.phones.source : ctx.engine.speakers;
}

std::atomic<float>& MonitorsPage::pathLevel() const noexcept
{
    return showsPhones() ? ctx.engine.phones.levelDb : ctx.engine.monitorLevelDb;
}

std::atomic<float>& MonitorsPage::pathDim() const noexcept
{
    return showsPhones() ? ctx.engine.phones.dimDb : ctx.engine.monitorDimDb;
}

std::atomic<int32_t>& MonitorsPage::pathOutput (bool right) const noexcept
{
    if (showsPhones())
        return right ? ctx.engine.phones.outputRight : ctx.engine.phones.outputLeft;
    return right ? ctx.engine.monitorOutputRight : ctx.engine.monitorOutputLeft;
}

ld::TalkPathParams& MonitorsPage::talkPath() const noexcept
{
    return ctx.engine.talk.paths[juce::jlimit (0, 1, talkTab)];
}

void MonitorsPage::showTalkTab (int t)
{
    talkTab = juce::jlimit (0, 1, t);
    auto& p = talkPath();
    busDim.setBinding (bindLinear ("BUS DIM", p.busDimDb, 0.0f, 40.0f, " dB", 0, 9.0f));
    monDim.setBinding (bindLinear ("MON DIM", p.monDimDb, 0.0f, 40.0f, " dB", 0, 9.0f));
    repaint();
}

void MonitorsPage::showPathTab (int t)
{
    pathTab = juce::jlimit (0, 1, t);
    auto& src = pathSource();
    srcLevel.setBinding (bindFader ("SRC LEVEL", src.sourceLevelDb));
    srcMix.setBinding (bindFader ("SRC MIX", src.sourceMixDb));
    dim.setBinding (bindLinear ("DIM", pathDim(), 0.0f, 40.0f, " dB", 0, 20.0f));
    pflDim.setBinding (bindLinear ("PFL DIM", src.pflDimDb, 0.0f, 40.0f, " dB", 0, 0.0f));
    repaint();
    notifyKnobsChanged(); // the parameter bar follows the path
}

std::vector<KnobBinding> MonitorsPage::getKnobs()
{
    // The parameter bar (fig. 72): TALK LVL, SRC LEVEL, SRC MIX, PFL DIM, then
    // the shown path's level and DIM (DIST: no delay here).
    return { bindFader ("TALK LVL", ctx.engine.talk.levelDb), srcLevel.getBinding(), srcMix.getBinding(), pflDim.getBinding(),
             bindLinear (showsPhones() ? "PHONES" : "SPEAKERS", pathLevel(), -60.0f, 10.0f, " dB", 1, 0.0f), dim.getBinding() };
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

juce::String MonitorsPage::talkChannelName() const
{
    auto* s = ctx.engine.getTalkStrip();
    if (s == nullptr)
        return ctx.engine.talk.assignKind.load() == ld::kTalkOff ? "OFF" : "NONE";
    // "CH40", "AUX8", plus a name of its own.
    const auto label = juce::String (livedaw::label (s->getKind())) + juce::String (s->getNumber());
    const auto name = stripName (*s);
    return name == s->getLabel() ? label : label + " " + name.toUpperCase();
}

void MonitorsPage::chooseOutput (bool right)
{
    auto& target = pathOutput (right);
    juce::StringArray items { "OFF" };
    const int n = juce::jmax (64, ctx.engine.getHardwareOutputNames().size());
    for (int i = 0; i < n; ++i)
        items.add (outputName (i));
    popups::ListOptions options;
    options.columns = 2;
    options.caption = (showsPhones() ? "PHONES " : "SPEAKERS ") + juce::String (right ? "R" : "L");
    anchor.setBounds (right ? rightField : leftField);
    juce::Component::SafePointer<MonitorsPage> safe (this);
    popups::chooseFrom (anchor, items, target.load() + 1, [safe, &target] (int i)
    {
        target = i - 1;
        if (safe != nullptr)
            safe->repaint();
    }, options);
}

void MonitorsPage::chooseSource()
{
    // What the path plays without a solo: MAIN.1-4, MTX.1-8, BUS.1-16, AUX.1-8.
    auto& target = pathSource().source;
    juce::StringArray items;
    for (int i = 0; i < ld::kNumMonitorSources; ++i)
        items.add (MixerEngine::monitorSourceName (i));
    popups::ListOptions options;
    options.columns = 4;
    options.caption = "SOURCE";
    anchor.setBounds (sourceField);
    juce::Component::SafePointer<MonitorsPage> safe (this);
    popups::chooseFrom (anchor, items, target.load(), [safe, &target] (int i)
    {
        target = i;
        if (safe != nullptr)
            safe->repaint();
    }, options);
}

void MonitorsPage::chooseTalkChannel()
{
    // OFF, then every input and aux channel (the WING offers CH40 and AUX8).
    std::vector<Strip*> strips;
    juce::StringArray items { "OFF" };
    for (auto kind : { StripKind::Channel, StripKind::Aux })
        for (auto& s : ctx.engine.getStrips (kind))
        {
            strips.push_back (s.get());
            items.add (juce::String (livedaw::label (kind)) + juce::String (s->getNumber()));
        }
    const auto* current = ctx.engine.getTalkStrip();
    int selected = 0;
    for (size_t i = 0; i < strips.size(); ++i)
        if (strips[i] == current)
            selected = (int) i + 1;
    popups::ListOptions options;
    options.columns = 6;
    options.caption = "TALKBACK CHANNEL";
    anchor.setBounds (talkChannelField);
    juce::Component::SafePointer<MonitorsPage> safe (this);
    popups::chooseFrom (anchor, items, selected, [safe, strips] (int i)
    {
        if (safe == nullptr)
            return;
        safe->ctx.engine.setTalkStrip (i > 0 ? strips[(size_t) i - 1] : nullptr);
        safe->repaint();
    }, options);
}

void MonitorsPage::timerCallback()
{
    std::array<float, 2> peak {};
    for (int ch = 0; ch < 2; ++ch)
    {
        // The phones meter has no other reader; the speakers' comes from the meter bank.
        phonesPeak[(size_t) ch] = juce::jmax (ctx.engine.phones.peak[ch].exchange (0.0f), phonesPeak[(size_t) ch] * 0.6f);
        peak[(size_t) ch] = meterPos (dsp::gainToDb (showsPhones() ? phonesPeak[(size_t) ch] : ctx.meters.getMonitor (ch)));
    }
    if (std::abs (peak[0] - shownPeak[0]) > 0.005f || std::abs (peak[1] - shownPeak[1]) > 0.005f)
    {
        shownPeak = peak;
        repaint (meterArea.getUnion (faderArea));
    }
    repaint (toolbar);
    repaint (talkArea);
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
        const bool enabled = t.press != nullptr || t.talkPath >= 0 || lit;
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
    constexpr int handheldMic = 102; // the WING's talkback icon
    tab (g, talkTabs.withWidth (talkTabs.getWidth() / 2).reduced (1, 0), Icon::Edit, "TALK A", talkTab == 0, true, handheldMic);
    tab (g, talkTabs.withTrimmedLeft (talkTabs.getWidth() / 2).reduced (1, 0), Icon::Edit, "TALK B", talkTab == 1, true, handheldMic);

    g.setColour (Dark::body);
    g.fillRect (talkArea);
    auto left = switchBox;
    panel (g, left.removeFromTop (juce::jmin (kDarkHeaderHeight, left.getHeight() / 5)), "SWITCH MODE");
    field (g, talkChannelField, "TALKBACK", talkChannelName(), true);
    auto dimArea = autoDimBox;
    panel (g, dimArea.removeFromTop (juce::jmin (kDarkHeaderHeight, dimArea.getHeight() / 4)), "AUTO DIM");

    g.setColour (Dark::textDim);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText ("USE SEND LEVELS", sendLevelsRow, juce::Justification::centredLeft);
    const auto& p = talkPath();
    tileGrid (g, busGrid, busTiles, "TALK TO BUS", "BUS", p.buses.load());
    tileGrid (g, mtxGrid, mtxTiles, "TALK TO MATRIX", "MTX", p.matrices.load());
    tileGrid (g, mainGrid, mainTiles, "TALK TO MAIN", "MAIN", p.mains.load());
}

void MonitorsPage::paintPath (juce::Graphics& g)
{
    tab (g, pathTabs.withWidth (pathTabs.getWidth() / 2).reduced (1, 0), Icon::Headphones, "PHONES", showsPhones(), true);
    tab (g, pathTabs.withTrimmedLeft (pathTabs.getWidth() / 2).reduced (1, 0), Icon::Speaker, "SPEAKERS", ! showsPhones(), true);
    panel (g, pathHeader, showsPhones() ? "PHONES SETTINGS" : "SPEAKERS SETTINGS");
    g.setColour (Dark::body);
    g.fillRect (pathArea);

    field (g, sourceField, "SOURCE", MixerEngine::monitorSourceName (pathSource().source.load()), true);
    // DIRECT IN, EQ, INVERT and DELAY: not in the engine.
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

    // The path's outputs and PFL / AFL.
    auto o = outputBox;
    panel (g, o.removeFromTop (juce::jmin (22, o.getHeight() / 3)), "OUTPUT / SOLO");
    const juce::String side = showsPhones() ? "PHONES " : "SPEAKERS ";
    field (g, leftField, side + "L", outputName (pathOutput (false).load()), true);
    field (g, rightField, side + "R", outputName (pathOutput (true).load()), true);
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

    // The level fader (set by PHONES LEVEL / MONITOR LEVEL): a lime fill and a value flag.
    const float levelDb = pathLevel().load();
    auto f = faderArea.toFloat();
    g.setColour (Dark::well);
    g.fillRect (f);
    const float pos = fader::dbToPos (juce::jlimit (-144.0f, 10.0f, levelDb));
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
    g.drawFittedText (fader::text (levelDb) + " dB", flag.toNearestInt(), juce::Justification::centred, 1, 0.6f);
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
        switchMode->setBounds (sw.removeFromTop (juce::jmin (sw.getHeight() / 3, 34)));
        sw.removeFromTop (8);
        talkChannelField = sw.removeFromTop (juce::jmin (sw.getHeight() - 4, 48));
        auto ad = autoDimBox.withTrimmedTop (juce::jmin (kDarkHeaderHeight, autoDimBox.getHeight() / 4) + 6).reduced (6, 0);
        const int kh = juce::jmin (48, ad.getHeight() / 2 - 4);
        busDim.setBounds (ad.removeFromTop (kh));
        ad.removeFromTop (6);
        monDim.setBounds (ad.removeFromTop (kh));

        sendLevelsRow = t.removeFromTop (juce::jmin (28, t.getHeight() / 8));
        sendLevels->setBounds (sendLevelsRow.removeFromRight (juce::jmin (48, sendLevelsRow.getWidth() / 4)).reduced (0, 3));
        busGrid = t.removeFromTop (t.getHeight() * 50 / 100);
        t.removeFromTop (6);
        mtxGrid = t.removeFromTop (t.getHeight() * 55 / 100);
        t.removeFromTop (6);
        mainGrid = t;
        busTiles = gridTiles (busGrid, kWingBuses, 4);
        mtxTiles = gridTiles (mtxGrid, kNumMatrices, 4);
        mainTiles = gridTiles (mainGrid, kNumMains, 4);
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

void MonitorsPage::mouseDown (const juce::MouseEvent& e)
{
    // The TALK keys go down and up like the surface's (SWITCH MODE).
    for (auto& t : tools)
        if (t.talkPath >= 0 && t.area.contains (e.getPosition()))
        {
            pressedTalk = t.talkPath;
            ctx.engine.talkKey (pressedTalk, true);
            repaint (toolbar);
            return;
        }
}

void MonitorsPage::mouseUp (const juce::MouseEvent& e)
{
    if (pressedTalk >= 0)
    {
        ctx.engine.talkKey (pressedTalk, false);
        pressedTalk = -1;
        repaint (toolbar);
        return;
    }
    const auto pos = e.getPosition();
    for (auto& t : tools)
        if (t.area.contains (pos) && t.press)
        {
            t.press();
            repaint (toolbar);
            return;
        }
    auto toggle = [this] (const std::vector<juce::Rectangle<int>>& tiles, std::atomic<uint32_t>& mask, juce::Point<int> p)
    {
        for (size_t i = 0; i < tiles.size(); ++i)
            if (tiles[i].contains (p))
            {
                mask = mask.load() ^ (1u << i);
                repaint (talkArea);
                return true;
            }
        return false;
    };
    auto& talkPathParams = talkPath();
    if (toggle (busTiles, talkPathParams.buses, pos) || toggle (mtxTiles, talkPathParams.matrices, pos) || toggle (mainTiles, talkPathParams.mains, pos))
        return;
    if (talkTabs.contains (pos))
        showTalkTab (pos.x < talkTabs.getCentreX() ? 0 : 1);
    else if (pathTabs.contains (pos))
        showPathTab (pos.x < pathTabs.getCentreX() ? 0 : 1);
    else if (talkChannelField.contains (pos))
        chooseTalkChannel();
    else if (sourceField.contains (pos))
        chooseSource();
    else if (leftField.contains (pos))
        chooseOutput (false);
    else if (rightField.contains (pos))
        chooseOutput (true);
}

} // namespace livedaw::ui