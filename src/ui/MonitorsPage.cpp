#include "ui/MonitorsPage.h"
#include "dsp/DspUtils.h"
#include "ui/HomeWidgets.h"
#include "ui/ResponseGraph.h"
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
/** MONITORS EQ (fig. 73): the 8-band EQ of a monitor path - L (low shelf),
    1-6 (PEQ), H (high shelf) - with PAN and WIDTH. The top row: ON, the path,
    PAN, WIDTH, CLOSE; the graph with a line and ring per band (drag: frequency
    and gain, wheel: Q of a PEQ band); the band keys at the right. The knobs
    under the display edit the selected band's GAIN, FREQ and Q. */
class MonitorEqView final : public juce::Component, private juce::Timer
{
public:
    explicit MonitorEqView (MixerEngine& e) : engine (e)
    {
        on.isOn = [this] { return dsp().eqOn.load(); };
        on.onClick = [this] { dsp().eqOn = ! dsp().eqOn.load(); };
        addAndMakeVisible (on);
        close.offInk = home::colours::caption;
        close.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (close);
        addAndMakeVisible (pan);
        addAndMakeVisible (width);
        for (int i = 0; i < ld::kMonitorEqBands; ++i)
        {
            const bool shelf = i == 0 || i == ld::kMonitorEqBands - 1;
            auto& k = *bandKeys.add (new home::IconKey (shelf ? juce::String ("SHV") : juce::String (i) + "  PEQ",
                                                        shelf ? std::optional<Icon> (i == 0 ? Icon::LowShelf : Icon::HighShelf) : std::nullopt,
                                                        Colours::choice));
            k.isOn = [this, i] { return selected == i; };
            k.onClick = [this, i] { select (i); };
            addAndMakeVisible (k);
        }
        for (int i = 0; i < 200; ++i)
            freqs.push_back (20.0f * std::pow (1000.0f, (float) i / 199.0f));
        setPath (true);
        startTimerHz (15);
    }

    std::function<void()> onClose;
    /** The selected band changed (the parameter bar follows). */
    std::function<void()> onSelect;

    void setPath (bool phonesPath)
    {
        phones = phonesPath;
        pan.setBinding (home::wingPan ("PAN", dsp().pan));
        width.setBinding (home::wingWidth ("WIDTH", dsp().width));
        repaint();
    }

    void select (int band)
    {
        selected = juce::jlimit (0, ld::kMonitorEqBands - 1, band);
        repaint();
        if (onSelect)
            onSelect();
    }

    /** The parameter bar (fig. 73): the selected band's GAIN and FREQ (and Q). */
    std::vector<KnobBinding> knobs()
    {
        auto& b = dsp().eq[selected];
        const auto tag = bandTag (selected);
        const auto [lo, hi] = freqRange (selected);
        auto freq = home::wingFreq ("FREQ " + tag, b.freq, lo, hi, kDefaultFreqs[selected]);
        std::vector<KnobBinding> k { {}, bindLinear ("GAIN " + tag, b.gainDb, -15.0f, 15.0f, " dB", 1, 0.0f), {}, freq };
        if (! isShelf (selected))
            k.push_back (bindLog ("Q " + tag, b.q, 0.442f, 10.0f, {}, 1.0f));
        return k;
    }

    void resized() override
    {
        auto r = getLocalBounds();
        bar = r.removeFromTop (juce::jlimit (30, 50, getHeight() / 9));
        auto b = bar.reduced (4, 4);
        on.setBounds (b.removeFromLeft (juce::jmin (90, b.getWidth() / 9)));
        close.setBounds (b.removeFromRight (juce::jmin (110, b.getWidth() / 8)));
        b.removeFromRight (16);
        width.setBounds (b.removeFromRight (juce::jmin (150, b.getWidth() / 6)));
        b.removeFromRight (8);
        pan.setBounds (b.removeFromRight (juce::jmin (150, b.getWidth() / 5)));
        title = b;
        r.removeFromTop (6);
        auto side = r.removeFromRight (juce::jlimit (70, 130, getWidth() * 12 / 100)).reduced (4, 0);
        r.removeFromRight (4);
        graph = r.reduced (2, 0).toFloat();
        const int kh = juce::jmin (40, side.getHeight() / ld::kMonitorEqBands);
        for (auto* k : bandKeys)
        {
            k->setBounds (side.removeFromTop (kh).reduced (0, 2));
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black);
        g.setColour (Dark::header);
        g.fillRect (bar);
        // The path: headphones / speaker icon and its name.
        auto t = title.reduced (8, 0);
        auto icon = t.removeFromLeft (t.getHeight()).toFloat().reduced ((float) t.getHeight() * 0.18f);
        t.removeFromLeft (4);
        drawIcon (g, phones ? Icon::Headphones : Icon::Speaker, icon, Dark::text);
        g.setColour (Dark::text);
        g.setFont (panelTitleFont (bar.getHeight()));
        g.drawText (phones ? "PHONES" : "SPEAKERS", t, juce::Justification::centredLeft);

        g.setColour (Colours::screenPanel);
        g.fillRect (graph);
        auto area = graph.reduced (6.0f, 4.0f);
        const float rulerH = juce::jlimit (5.0f, 12.0f, area.getHeight() * 0.04f);
        auto ruler = area.removeFromTop (rulerH);
        area.removeFromTop (22.0f);
        auto labels = area.removeFromBottom (16.0f);
        plot = area;
        auto xOf = [this] (float hz) { return xForFreq (hz); };
        g.setColour (juce::Colours::black);
        g.fillRect (plot);
        ResponseGraph::paintPianoRuler (g, ruler, xOf);
        g.setColour (juce::Colour (0xff383838));
        for (float decade : { 10.0f, 100.0f, 1000.0f, 10000.0f })
            for (int m = 1; m <= 9; ++m)
                if (const float f = decade * (float) m; f >= 20.0f && f <= 20000.0f)
                    g.drawVerticalLine (juce::roundToInt (xForFreq (f)), plot.getY(), plot.getBottom());
        for (int i = 0; i <= 6; ++i)
            g.drawHorizontalLine (juce::roundToInt (plot.getY() + (plot.getHeight() - 1.0f) * (float) i / 6.0f), plot.getX(), plot.getRight());
        g.setColour (Dark::caption);
        g.setFont (juce::FontOptions (11.0f));
        for (float f : { 20.0f, 40.0f, 60.0f, 100.0f, 200.0f, 400.0f, 600.0f, 1000.0f, 2000.0f, 4000.0f, 6000.0f, 10000.0f, 20000.0f })
            g.drawText (f >= 1000.0f ? juce::String (juce::roundToInt (f / 1000.0f)) + "k" : juce::String (juce::roundToInt (f)),
                        juce::Rectangle<float> (xForFreq (f) - 16.0f, labels.getY() + 2.0f, 32.0f, labels.getHeight() - 2.0f).toNearestInt(),
                        juce::Justification::centred);

        // The curve over its fill (flat and grey while the EQ is off).
        const bool active = dsp().eqOn.load();
        std::vector<float> db (freqs.size(), 0.0f);
        engine.getMonitorEqResponse (phones, -1, freqs.data(), db.data(), (int) freqs.size());
        juce::Path curve;
        for (size_t i = 0; i < db.size(); ++i)
        {
            const juce::Point<float> pt (xForFreq (freqs[i]), yForDb (db[i]));
            if (i == 0)
                curve.startNewSubPath (pt);
            else
                curve.lineTo (pt);
        }
        g.saveState();
        g.reduceClipRegion (plot.toNearestInt());
        juce::Path fill (curve);
        fill.lineTo (plot.getRight(), plot.getCentreY());
        fill.lineTo (plot.getX(), plot.getCentreY());
        fill.closeSubPath();
        g.setColour (active ? home::colours::eqFill : juce::Colour (0xff1c1c1c));
        g.fillPath (fill);
        g.setColour (active ? home::colours::eqCurve : Dark::caption);
        g.strokePath (curve, juce::PathStrokeType (2.0f));
        g.restoreState();

        // A line from its label down and a ring at its gain, per band.
        for (int i = 0; i < ld::kMonitorEqBands; ++i)
        {
            const auto c = handle (i);
            const bool sel = i == selected;
            g.setColour (sel ? Colours::choice : juce::Colour (0xff6c6c6c));
            g.fillRect (c.x - 0.75f, plot.getY() + 2.0f, sel ? 2.5f : 1.5f, plot.getHeight() - 4.0f);
            g.setColour (sel ? Dark::text : Dark::caption);
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.drawText (bandTag (i), juce::Rectangle<float> (24.0f, 16.0f).withCentre ({ c.x, plot.getY() - 10.0f }).toNearestInt(),
                        juce::Justification::centred);
            auto ring = juce::Rectangle<float> (16.0f, 16.0f).withCentre (c);
            g.setColour (juce::Colours::black);
            g.fillEllipse (ring);
            g.setColour (sel ? Colours::choice.brighter (0.4f) : Dark::text);
            g.drawEllipse (ring, 2.0f);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = -1;
        float best = 18.0f;
        for (int i = 0; i < ld::kMonitorEqBands; ++i)
            if (const float d = handle (i).getDistanceFrom (e.position); d < best)
            {
                best = d;
                dragging = i;
            }
        if (dragging < 0 && plot.contains (e.position))
        {
            // Touching near a band's line picks it.
            for (int i = 0; i < ld::kMonitorEqBands; ++i)
                if (std::abs (handle (i).x - e.position.x) < 8.0f)
                    dragging = i;
        }
        if (dragging >= 0)
            select (dragging);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0)
            return;
        auto& b = dsp().eq[dragging];
        const auto [lo, hi] = freqRange (dragging);
        b.freq = juce::jlimit (lo, hi, freqForX (e.position.x));
        b.gainDb = juce::jlimit (-15.0f, 15.0f, std::round ((plot.getCentreY() - e.position.y) / (plot.getHeight() * 0.5f) * kRangeDb * 10.0f) / 10.0f);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = -1; }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        if (isShelf (selected))
            return;
        auto& q = dsp().eq[selected].q;
        q = juce::jlimit (0.442f, 10.0f, q.load() * std::pow (1.15f, w.deltaY > 0 ? 1.0f : -1.0f));
        repaint();
    }

private:
    static constexpr float kRangeDb = 18.0f;
    static constexpr float kDefaultFreqs[ld::kMonitorEqBands] = { 80.0f, 160.0f, 315.0f, 630.0f, 1250.0f, 2500.0f, 5000.0f, 12000.0f };

    static bool isShelf (int band) { return band == 0 || band == ld::kMonitorEqBands - 1; }
    static juce::String bandTag (int band) { return band == 0 ? "L" : band == ld::kMonitorEqBands - 1 ? "H" : juce::String (band); }
    static std::pair<float, float> freqRange (int band)
    {
        return band == 0 ? std::pair { 20.0f, 2000.0f } : band == ld::kMonitorEqBands - 1 ? std::pair { 200.0f, 20000.0f } : std::pair { 20.0f, 20000.0f };
    }

    ld::MonitorDspParams& dsp() const noexcept { return engine.monitorPath (phones); }
    float xForFreq (float f) const { return plot.getX() + plot.getWidth() * std::log (juce::jmax (20.0f, f) / 20.0f) / std::log (1000.0f); }
    float freqForX (float x) const { return 20.0f * std::pow (1000.0f, juce::jlimit (0.0f, 1.0f, (x - plot.getX()) / plot.getWidth())); }
    float yForDb (float db) const { return plot.getCentreY() - juce::jlimit (-kRangeDb, kRangeDb, db) / kRangeDb * plot.getHeight() * 0.5f; }
    juce::Point<float> handle (int band) const
    {
        auto& b = dsp().eq[band];
        return { xForFreq (b.freq.load()), yForDb (b.gainDb.load()) };
    }

    void timerCallback() override
    {
        auto& d = dsp();
        juce::String state;
        state << (int) d.eqOn.load() << selected << (int) phones;
        for (auto& b : d.eq)
            state << ':' << b.gainDb.load() << ',' << b.freq.load() << ',' << b.q.load();
        if (state != shown)
        {
            shown = state;
            repaint();
        }
    }

    MixerEngine& engine;
    bool phones = true;
    int selected = 0, dragging = -1;
    home::IconKey on { "ON", std::nullopt, Colours::gold }, close { "CLOSE", Icon::Check };
    KnobField pan, width { {}, Colours::width };
    juce::OwnedArray<home::IconKey> bandKeys;
    std::vector<float> freqs;
    juce::Rectangle<int> bar, title;
    juce::Rectangle<float> graph, plot;
    juce::String shown;
};

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

    for (auto* k : { &srcLevel, &srcMix, &dim, &pflDim, &bandSolo, &busDim, &monDim, &limiter })
        addAndMakeVisible (k);

    switchMode = std::make_unique<ChoiceKeys> (juce::StringArray { "AUTO", "PUSH", "LATCH" }, [this] { return (int) talkPath().mode.load(); },
                                               [this] (int i) { talkPath().mode = i; });
    switchMode->vertical = true;
    addAndMakeVisible (*switchMode);
    sendLevels = std::make_unique<OnKey> ([this] { return talkPath().individual.load(); }, [this] (bool on) { talkPath().individual = on; });
    addAndMakeVisible (*sendLevels);

    for (int i = 0; i < 32; ++i)
        eqKeyFreqs.push_back (20.0 * std::pow (1000.0, (double) i / 31.0));

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

ld::MonitorDspParams& MonitorsPage::pathDsp() const noexcept
{
    return ctx.engine.monitorPath (showsPhones());
}

std::atomic<float>& MonitorsPage::pathLevel() const noexcept
{
    return showsPhones() ? ctx.engine.phones.levelDb : ctx.engine.monitorLevelDb;
}

std::atomic<float>& MonitorsPage::pathDim() const noexcept
{
    return showsPhones() ? ctx.engine.phones.dimDb : ctx.engine.monitorDimDb;
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
    auto& d = pathDsp();
    srcLevel.setBinding (bindFader ("SRC LEVEL", src.sourceLevelDb));
    srcMix.setBinding (bindFader ("SRC MIX", src.sourceMixDb));
    dim.setBinding (bindLinear ("DIM", pathDim(), 0.0f, 40.0f, " dB", 0, 20.0f));
    pflDim.setBinding (bindLinear ("PFL DIM", src.pflDimDb, 0.0f, 40.0f, " dB", 0, 0.0f));
    bandSolo.setBinding (bindLinear ("BAND SOLO", d.bandSoloDb, 0.0f, 24.0f, " dB", 0, 12.0f));
    limiter.setBinding (bindLinear ("LIMITER", d.limiterDb, -40.0f, 0.0f, " dB", 0, 0.0f));
    if (eqView != nullptr)
        eqView->setPath (showsPhones());
    repaint();
    notifyKnobsChanged(); // the parameter bar follows the path
}

void MonitorsPage::showEq (bool show)
{
    if (show && eqView == nullptr)
    {
        eqView = std::make_unique<MonitorEqView> (ctx.engine);
        eqView->onClose = [this] { showEq (false); };
        eqView->onSelect = [this] { notifyKnobsChanged(); };
        addChildComponent (*eqView);
    }
    if (eqView == nullptr)
        return;
    eqView->setPath (showsPhones());
    eqView->setBounds (getLocalBounds());
    eqView->setVisible (show);
    if (show)
        eqView->toFront (false);
    notifyKnobsChanged();
}

bool MonitorsPage::isEqShown() const noexcept
{
    return eqView != nullptr && eqView->isVisible();
}

std::vector<KnobBinding> MonitorsPage::getKnobs()
{
    if (isEqShown())
        return eqView->knobs();
    // The parameter bar (fig. 72): TALK LVL, SRC LEVEL, SRC MIX, PFL DIM, DIST,
    // then the DIRECT IN level (the console has no control for it).
    auto& d = pathDsp();
    auto dist = bindLog ("DIST", d.delayM, 0.1f, 100.0f, " m", 0.1f);
    dist.text = [&m = d.delayM] { return juce::String (m.load(), m.load() < 10.0f ? 1 : 0) + " m"; };
    return { bindFader ("TALK LVL", ctx.engine.talk.levelDb), srcLevel.getBinding(), srcMix.getBinding(), pflDim.getBinding(), dist,
             bindFader ("DIR LVL", d.directLevelDb) };
}

std::vector<KnobStyle> MonitorsPage::getKnobStyles()
{
    auto style = [] (KnobStyle::Bar bar, juce::Colour colour)
    {
        KnobStyle s;
        s.bar = bar;
        s.colour = colour;
        return s;
    };
    if (isEqShown())
        return { {}, style (KnobStyle::Bar::Centre, KnobColours::white), {}, style (KnobStyle::Bar::Fill, KnobColours::blue),
                 style (KnobStyle::Bar::Fill, KnobColours::green) };
    return { style (KnobStyle::Bar::Fill, KnobColours::yellow), style (KnobStyle::Bar::Fill, KnobColours::white),
             style (KnobStyle::Bar::Fill, KnobColours::white), style (KnobStyle::Bar::Fill, KnobColours::blue),
             style (KnobStyle::Bar::Fill, Colours::lime), style (KnobStyle::Bar::Fill, KnobColours::blue) };
}

StatusTitle MonitorsPage::getStatusTitle()
{
    return {}; // the selected channel, as on the console (figs. 72-73)
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

void MonitorsPage::chooseDirectIn()
{
    // An extra signal mixed into the path: OFF, CH.1-40, AUX.1-8, BUS.1-16, MAIN.1-4, MTX.1-8.
    auto& target = pathDsp().directSource;
    juce::StringArray items { "OFF" };
    for (int i = 0; i < ld::kNumDirectSources; ++i)
        items.add (MixerEngine::directSourceName (i));
    popups::ListOptions options;
    options.columns = 8;
    options.caption = "DIRECT IN";
    anchor.setBounds (directField);
    juce::Component::SafePointer<MonitorsPage> safe (this);
    popups::chooseFrom (anchor, items, target.load() + 1, [safe, &target] (int i)
    {
        target = i - 1;
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

juce::String MonitorsPage::eqStateKey() const
{
    auto& d = pathDsp();
    juce::String state;
    state << (int) d.eqOn.load() << (int) d.invert.load() << (int) d.delayOn.load() << ':' << d.delayM.load() << ':' << d.directSource.load()
          << ':' << pathSource().source.load();
    for (auto& b : d.eq)
        state << ':' << b.gainDb.load() << ',' << b.freq.load() << ',' << b.q.load();
    return state;
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
    // The limiter's gain reduction (this page is its only reader), held and released.
    for (int path = 0; path < 2; ++path)
    {
        const float gr = ctx.engine.monitor.paths[path].limiterGrDb.exchange (0.0f);
        if ((path == 1) == showsPhones())
            limiterGr = juce::jmax (gr, limiterGr * 0.8f);
    }
    if (std::abs (peak[0] - shownPeak[0]) > 0.005f || std::abs (peak[1] - shownPeak[1]) > 0.005f || std::abs (limiterGr - shownGr) > 0.1f)
    {
        shownPeak = peak;
        shownGr = limiterGr;
        repaint (meterArea.getUnion (faderArea));
    }
    if (const auto state = eqStateKey(); state != shownEq)
    {
        shownEq = state;
        repaint (pathArea);
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

    auto& d = pathDsp();
    field (g, sourceField, "SOURCE", MixerEngine::monitorSourceName (pathSource().source.load()), true);
    field (g, directField, "DIRECT IN", MixerEngine::directSourceName (d.directSource.load()), true);
    paintEqKey (g);

    // The boxes: DIM / PFL DIM / BAND SOLO, and the DIST slider with INVERT and DELAY.
    for (auto box : { dimBox, distBox })
    {
        g.setColour (Dark::header);
        g.fillRect (box);
        g.setColour (juce::Colours::black);
        g.drawRect (box, 1);
    }
    paintDistSlider (g);
    auto key = [&g] (juce::Rectangle<int> r, Icon icon, const juce::String& text, bool on, juce::Colour onColour)
    {
        auto f = r.toFloat();
        auto label = f.removeFromBottom (juce::jmin (16.0f, f.getHeight() * 0.32f));
        g.setColour (on ? onColour : Dark::key);
        g.fillRoundedRectangle (f.reduced (1.0f), 2.0f);
        const auto ink = on ? juce::Colours::black : Dark::text;
        drawIcon (g, icon, f.withSizeKeepingCentre (juce::jmin (f.getWidth(), f.getHeight()), juce::jmin (f.getWidth(), f.getHeight())).reduced (f.getHeight() * 0.2f), ink);
        g.setColour (on ? onColour : Dark::textDim);
        g.setFont (juce::FontOptions (juce::jlimit (8.0f, 12.0f, label.getHeight() * 0.85f), juce::Font::bold));
        g.drawFittedText (text, label.toNearestInt(), juce::Justification::centred, 1, 0.7f);
    };
    key (invertKey, Icon::Invert, "INVERT", d.invert.load(), KnobColours::yellow);
    key (delayKey, Icon::DeltaT, "DELAY", d.delayOn.load(), Colours::lime);
}

void MonitorsPage::paintEqKey (juce::Graphics& g)
{
    // "EQ" and the path's curve (a flat line while the EQ does nothing); gold while on.
    auto& d = pathDsp();
    const bool on = d.eqOn.load();
    auto r = eqKey.toFloat();
    g.setColour (on ? Colours::gold : Dark::well);
    g.fillRect (r);
    g.setColour (on ? juce::Colours::black : Dark::text);
    auto text = r.removeFromLeft (juce::jmin (r.getWidth() * 0.3f, 40.0f));
    g.setFont (wingFontPx (juce::jmin (TypeScale::value * getWingScreenHeight(), r.getHeight() * 0.36f)));
    g.drawText ("EQ", text.reduced (6.0f, 0.0f), juce::Justification::centredLeft);
    auto c = r.reduced (6.0f, r.getHeight() * 0.25f);
    std::vector<float> freqs (eqKeyFreqs.begin(), eqKeyFreqs.end()), db (eqKeyFreqs.size(), 0.0f);
    ctx.engine.getMonitorEqResponse (showsPhones(), -1, freqs.data(), db.data(), (int) freqs.size());
    juce::Path p;
    for (size_t i = 0; i < db.size(); ++i)
    {
        const juce::Point<float> pt (c.getX() + c.getWidth() * (float) i / (float) (db.size() - 1),
                                     c.getCentreY() - juce::jlimit (-1.0f, 1.0f, db[i] / 15.0f) * c.getHeight() * 0.5f);
        if (i == 0)
            p.startNewSubPath (pt);
        else
            p.lineTo (pt);
    }
    g.strokePath (p, juce::PathStrokeType (1.5f));
}

namespace
{
    // The DIST slider's scale: 0.1 .. 100 m, logarithmic.
    float distToPos (float m) { return juce::jlimit (0.0f, 1.0f, std::log (juce::jmax (0.1f, m) / 0.1f) / std::log (1000.0f)); }
    float posToDist (float p) { return 0.1f * std::pow (1000.0f, juce::jlimit (0.0f, 1.0f, p)); }

    /** The slider's track inside its area (above the value box). */
    juce::Rectangle<float> distTrack (juce::Rectangle<int> slider)
    {
        auto r = slider.toFloat().reduced (2.0f, 8.0f);
        r.removeFromBottom (juce::jmin (36.0f, r.getHeight() * 0.28f));
        return r.withTrimmedLeft (r.getWidth() * 0.45f).withWidth (juce::jmin (10.0f, r.getWidth() * 0.25f));
    }
} // namespace

void MonitorsPage::paintDistSlider (juce::Graphics& g)
{
    auto& d = pathDsp();
    const bool on = d.delayOn.load();
    const float m = d.delayM.load();
    auto track = distTrack (distSlider);
    g.setColour (Dark::well);
    g.fillRect (track);
    const float y = track.getBottom() - track.getHeight() * distToPos (m);
    g.setColour (on ? Colours::lime : Dark::textDim);
    g.fillRect (track.withTop (y));
    // The thumb and the scale.
    g.setColour (Dark::text);
    g.fillRect (juce::Rectangle<float> (track.getWidth() + 8.0f, 4.0f).withCentre ({ track.getCentreX(), y }));
    g.setColour (Dark::textDim);
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    for (float mark : { 0.1f, 1.0f, 10.0f, 100.0f })
    {
        const float my = track.getBottom() - track.getHeight() * distToPos (mark);
        g.drawText (mark < 1.0f ? juce::String (mark, 1) : juce::String (juce::roundToInt (mark)),
                    juce::Rectangle<float> (distSlider.getX() + 2.0f, my - 6.0f, track.getX() - distSlider.getX() - 5.0f, 12.0f),
                    juce::Justification::centredRight);
    }
    // The value: the distance, and the delay it makes.
    auto value = distSlider.toFloat().withTop (track.getBottom() + 6.0f).reduced (2.0f, 0.0f);
    auto box = value.removeFromTop (juce::jmin (18.0f, value.getHeight() * 0.5f));
    g.setColour (Dark::key);
    g.fillRect (box);
    g.setColour (Dark::text);
    g.setFont (juce::FontOptions (juce::jmin (13.0f, box.getHeight() * 0.8f), juce::Font::bold));
    g.drawFittedText (juce::String (m, m < 10.0f ? 1 : 0) + " m", box.toNearestInt(), juce::Justification::centred, 1, 0.7f);
    g.setColour (on ? Colours::lime : Dark::textDim);
    g.setFont (juce::FontOptions (juce::jmin (11.0f, value.getHeight() * 0.8f), juce::Font::bold));
    g.drawFittedText (juce::String (MixerEngine::monitorDelayMs (m), 1) + " ms", value.toNearestInt(), juce::Justification::centred, 1, 0.7f);
}

void MonitorsPage::paintLevel (juce::Graphics& g)
{
    panel (g, levelHeader, "LEVEL");
    g.setColour (Dark::body);
    g.fillRect (levelArea);

    // The meter (L R) with the CLIP .. -60 scale, and the limiter's gain reduction.
    auto m = meterArea.toFloat();
    g.setColour (Dark::well);
    g.fillRect (m);
    auto scale = m.removeFromLeft (m.getWidth() * 0.38f);
    auto grBar = m.removeFromRight (juce::jmax (5.0f, m.getWidth() * 0.22f));
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
        g.setColour (Dark::textDim);
        g.drawText (ch == 0 ? "L" : "R", juce::Rectangle<float> (bar.getX(), bars.getBottom() + 1.0f, bar.getWidth(), 10.0f), juce::Justification::centred);
    }
    // GR (firmware 1.11): down from the top, 0 .. 20 dB.
    auto gr = grBar.reduced (1.0f, 6.0f).withTrimmedBottom (8.0f);
    g.setColour (Dark::key);
    g.fillRect (gr);
    g.setColour (Colours::amber);
    g.fillRect (gr.withHeight (gr.getHeight() * juce::jlimit (0.0f, 1.0f, shownGr / 20.0f)));
    g.setColour (Dark::textDim);
    g.setFont (juce::FontOptions (8.0f, juce::Font::bold));
    g.drawText ("GR", juce::Rectangle<float> (gr.getX() - 4.0f, gr.getBottom() + 1.0f, gr.getWidth() + 8.0f, 10.0f), juce::Justification::centred);

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
    if (eqView != nullptr)
        eqView->setBounds (b);
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
        // [Fig. 80] AUTO / PUSH / LATCH stacked, the TALKBACK channel field under them.
        talkChannelField = sw.removeFromBottom (juce::jmin (sw.getHeight() / 3, 48));
        sw.removeFromBottom (8);
        switchMode->setBounds (sw.withSizeKeepingCentre (juce::jmin (sw.getWidth(), 140), juce::jmin (sw.getHeight(), 3 * 34)));
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
        // Two columns (fig. 72). Left: SOURCE, SRC LEVEL, SRC MIX, then a box
        // with DIM, PFL DIM and BAND SOLO. Right: DIRECT IN, the EQ key, then a
        // box with the DIST slider, INVERT and DELAY.
        auto p = pathArea.reduced (4);
        const int rowH = juce::jlimit (30, 52, (p.getHeight() - 20) / 6);
        auto leftCol = p.removeFromLeft (p.getWidth() / 2 - 3);
        p.removeFromLeft (6);
        auto rightCol = p;

        sourceField = leftCol.removeFromTop (rowH).reduced (0, 2);
        leftCol.removeFromTop (4);
        for (auto* k : { &srcLevel, &srcMix })
        {
            k->setBounds (leftCol.removeFromTop (rowH).reduced (0, 2));
            leftCol.removeFromTop (2);
        }
        leftCol.removeFromTop (6);
        dimBox = leftCol;
        auto box = dimBox.reduced (3);
        const int kh = juce::jmin (rowH, box.getHeight() / 3);
        for (auto* k : { &dim, &pflDim, &bandSolo })
            k->setBounds (box.removeFromTop (kh).reduced (0, 2));

        directField = rightCol.removeFromTop (rowH).reduced (0, 2);
        rightCol.removeFromTop (4);
        eqKey = rightCol.removeFromTop (rowH).reduced (0, 2);
        rightCol.removeFromTop (8);
        distBox = rightCol;
        auto db = distBox.reduced (3);
        distSlider = db.removeFromLeft (db.getWidth() * 52 / 100);
        db.removeFromLeft (4);
        const int keyH = juce::jmin (db.getWidth() + 14, db.getHeight() / 2 - 6);
        invertKey = db.withHeight (keyH).withY (db.getY() + db.getHeight() / 4 - keyH / 2).reduced (2, 0);
        delayKey = db.withHeight (keyH).withY (db.getY() + db.getHeight() * 3 / 4 - keyH / 2).reduced (2, 0);
    }

    levelHeader = level.removeFromTop (tabH + gap + kDarkHeaderHeight);
    levelHeader = levelHeader.withTrimmedTop (tabH + gap);
    level.removeFromTop (2);
    levelArea = level;
    {
        auto l = levelArea.reduced (4);
        limiter.setBounds (l.removeFromTop (juce::jlimit (30, 52, (pathArea.getHeight() - 28) / 6)).reduced (0, 2));
        l.removeFromTop (6);
        meterArea = l.removeFromLeft (l.getWidth() * 55 / 100);
        faderArea = l.withTrimmedLeft (4);
    }
}

void MonitorsPage::setDistanceFromY (int y)
{
    auto track = distTrack (distSlider);
    const float pos = (track.getBottom() - (float) y) / juce::jmax (1.0f, track.getHeight());
    const float m = posToDist (pos);
    pathDsp().delayM = m < 10.0f ? std::round (m * 10.0f) / 10.0f : std::round (m);
    repaint (pathArea);
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
    draggingDist = distSlider.contains (e.getPosition());
    if (draggingDist)
        setDistanceFromY (e.y);
}

void MonitorsPage::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingDist)
        setDistanceFromY (e.y);
}

void MonitorsPage::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (distSlider.contains (e.getPosition()))
    {
        auto& m = pathDsp().delayM;
        const float pos = distToPos (m.load()) + (w.deltaY > 0.0f ? 0.01f : -0.01f);
        m = juce::jlimit (0.1f, 100.0f, posToDist (pos));
        repaint (pathArea);
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
    if (draggingDist)
    {
        draggingDist = false;
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
    auto& d = pathDsp();
    if (talkTabs.contains (pos))
        showTalkTab (pos.x < talkTabs.getCentreX() ? 0 : 1);
    else if (pathTabs.contains (pos))
        showPathTab (pos.x < pathTabs.getCentreX() ? 0 : 1);
    else if (talkChannelField.contains (pos))
        chooseTalkChannel();
    else if (sourceField.contains (pos))
        chooseSource();
    else if (directField.contains (pos))
        chooseDirectIn();
    else if (eqKey.contains (pos))
        showEq (true);
    else if (invertKey.contains (pos))
        d.invert = ! d.invert.load();
    else if (delayKey.contains (pos))
        d.delayOn = ! d.delayOn.load();
    repaint (pathArea);
}

} // namespace livedaw::ui
