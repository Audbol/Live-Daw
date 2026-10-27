#include "ui/MonitorsPage.h"
#include "dsp/DspUtils.h"
#include "ui/HomeWidgets.h"
#include "ui/MeterBallistics.h"
#include "ui/ResponseGraph.h"
#include "ui/WingPopups.h"

namespace livedaw::ui
{

namespace
{
    // The toolbar's colours (fig. 72; measured off the console's screen,
    // firmware 3.1): unlit keys are their colour at a fifth on a dark tint.
    const juce::Colour talkYellow { 0xfff8c430 }, pathOlive = Dark::olive, dimGreen = Colours::green, muteRed { 0xffd84848 },
                       monoOrange = Colours::alt, levelLime { 0xffa0c000 }, gridHeader { 0xff404840 };

    /** `v` px of the WING's 1280 x 800 screen in px of this screen. */
    int px (float v) { return juce::roundToInt (wingPx (v)); }

    /** A rectangle given in WING px from the page's top left (the page starts
        under the status bar, at the console's y = 80). */
    juce::Rectangle<int> wingRect (float x, float y, float w, float h)
    {
        return { px (x), px (y), px (x + w) - px (x), px (y + h) - px (y) };
    }

    /** The console's fonts by the height of their capitals in WING px. */
    juce::Font capFont (float capWingPx, WingWeight weight = WingWeight::Medium)
    {
        return wingFontPx (juce::jmax (5.0f, wingPx (capWingPx)), weight);
    }

    /** The talkback side is drawn at half brightness while there is no
        talkback channel (the console's TALKBACK DISABLED). */
    juce::Colour dimmed (juce::Colour c, bool dim) { return dim ? c.withMultipliedBrightness (0.5f) : c; }

    /** The console's meter scale: CLIP, -6, -12, -24, -36, -48, -60 evenly spaced. */
    float meterPos (float db) { return meters::scalePos (db); }

    /** A panel header: a #202020 box with its title in light grey. */
    void panel (juce::Graphics& g, juce::Rectangle<int> header, const juce::String& title, bool dim = false)
    {
        g.setColour (dimmed (Dark::header, dim));
        g.fillRect (header);
        g.setColour (dimmed (Colours::caption, dim));
        g.setFont (capFont (20.0f));
        g.drawFittedText (title, header.reduced (6, 0), juce::Justification::centred, 1);
    }

    /** A tab with an icon (an `Icon`, or a WING icon number when `wingIcon` >= 0):
        lit #303030 with a gold top line and gold print, unlit grey on #202020. */
    void tab (juce::Graphics& g, juce::Rectangle<int> r, Icon icon, const juce::String& text, bool on, bool dim, int wingIcon = -1)
    {
        g.setColour (dimmed (on ? Dark::tabOn : Dark::tabOff, dim));
        g.fillRect (r);
        const auto ink = dimmed (on ? Colours::gold : Colours::caption, dim);
        if (on)
        {
            g.setColour (ink);
            g.fillRect (r.withHeight (px (4.0f)));
        }
        auto ib = r.removeFromLeft (px (56.0f)).toFloat().withSizeKeepingCentre (wingPx (40.0f), wingPx (40.0f));
        if (wingIcon < 0 || ! drawWingIcon (g, wingIcon, ib, ink))
            drawIcon (g, icon, ib, ink);
        g.setColour (ink);
        g.setFont (fontFittingWidth (capFont (on ? 20.0f : 15.0f), text, (float) r.getWidth() - 8.0f, 0.7f));
        g.drawFittedText (text, r.reduced (px (8.0f), 0), on ? juce::Justification::centredLeft : juce::Justification::centred, 1);
    }

    /** The tiles of a TALK TO grid: `count` tiles 52 px square 56 apart,
        `columns` wide, under the grid's 24 px title bar. */
    std::vector<juce::Rectangle<int>> gridTiles (juce::Rectangle<int> area, int count, int columns)
    {
        std::vector<juce::Rectangle<int>> tiles;
        const int x0 = area.getX(), y0 = area.getY();
        for (int i = 0; i < count; ++i)
        {
            const float x = 6.0f + 56.0f * (float) (i % columns), y = 28.0f + 56.0f * (float) (i / columns);
            tiles.push_back ({ x0 + px (x), y0 + px (y), px (x + 52.0f) - px (x), px (y + 52.0f) - px (y) });
        }
        return tiles;
    }

    /** A TALK TO grid [WING]: a title bar over black, framed in its grey, and
        tiles ("BUS" over "1"), lit yellow for the destinations in `mask`. */
    void tileGrid (juce::Graphics& g, juce::Rectangle<int> area, const std::vector<juce::Rectangle<int>>& tiles, const juce::String& title,
                   const juce::String& caption, uint32_t mask, bool dim)
    {
        g.setColour (dimmed (gridHeader, dim));
        g.fillRect (area);
        g.setColour (juce::Colours::black);
        g.fillRect (area.reduced (px (2.0f)).withTrimmedTop (px (22.0f)));
        auto head = area.withHeight (px (24.0f));
        g.setColour (dimmed (Colours::caption, dim));
        g.setFont (capFont (12.0f));
        g.drawText (title, head, juce::Justification::centred);
        for (size_t i = 0; i < tiles.size(); ++i)
        {
            auto t = tiles[i];
            const bool on = (mask >> i) & 1u;
            g.setColour (dimmed (on ? talkYellow : Dark::body, dim));
            g.fillRect (t);
            g.setColour (dimmed (on ? juce::Colours::black : Colours::screenLine, dim));
            g.setFont (capFont (9.0f));
            g.drawText (caption, t.removeFromTop (t.getHeight() * 2 / 5), juce::Justification::centred);
            g.setColour (dimmed (on ? juce::Colours::black : Colours::textDim, dim));
            g.setFont (capFont (14.0f));
            g.drawText (juce::String (i + 1), t, juce::Justification::centredTop);
        }
    }

    /** A drop-down field [WING]: caption over value on #101010, the dots key
        framed black at the right. */
    void field (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& caption, const juce::String& value, bool enabled)
    {
        const float a = enabled ? 1.0f : 0.45f;
        g.setColour (juce::Colours::black);
        g.fillRect (r);
        auto key = r.removeFromRight (px (32.0f));
        r = r.reduced (px (2.0f));
        g.setColour (Dark::well);
        g.fillRect (r);
        g.setColour (Dark::key.withAlpha (a));
        g.fillRect (key.reduced (px (2.0f)));
        g.setColour (Colours::caption.withAlpha (a));
        const auto kc = key.getCentre().toFloat();
        const float d = wingPx (7.0f);
        for (int i = -1; i <= 1; ++i)
            g.fillEllipse (juce::Rectangle<float> (d, d).withCentre (kc.translated (0.0f, (float) i * wingPx (12.0f))));
        auto t = r.reduced (px (8.0f), px (4.0f));
        g.setColour (Colours::caption.withAlpha (a));
        g.setFont (fontFittingWidth (capFont (14.0f), caption, (float) t.getWidth(), 0.7f));
        g.drawFittedText (caption, t.removeFromTop (t.getHeight() * 45 / 100), juce::Justification::centredLeft, 1);
        g.setColour (Dark::text.withAlpha (a));
        g.setFont (fontFittingWidth (capFont (18.0f), value, (float) t.getWidth(), 0.8f));
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
    // [WING] #101010 in a 2 px black frame.
    g.setColour (juce::Colours::black);
    g.fillRect (r);
    g.setColour (Dark::well);
    g.fillRect (r.reduced (juce::roundToInt (wingPx (2.0f))));
    // A narrow field (AUTO DIM at 1280x800) keeps half its width for the text.
    const int knobW = juce::jmin (r.getHeight(), r.getWidth() / 2);
    auto knob = r.removeFromLeft (knobW).toFloat().withSizeKeepingCentre ((float) knobW, (float) knobW).reduced ((float) knobW * 0.12f);
    const float a0 = juce::MathConstants<float>::pi * 1.25f, a1 = juce::MathConstants<float>::pi * 2.75f;
    const float rad = knob.getWidth() * 0.42f, thick = juce::jmax (2.5f, knob.getWidth() * 0.2f);
    juce::Path track, arc;
    track.addCentredArc (knob.getCentreX(), knob.getCentreY(), rad, rad, 0.0f, a0, a1, true);
    g.setColour (Dark::key.withAlpha (a));
    g.strokePath (track, juce::PathStrokeType (thick));
    if (on)
    {
        arc.addCentredArc (knob.getCentreX(), knob.getCentreY(), rad, rad, 0.0f, a0, a0 + (a1 - a0) * juce::jlimit (0.0f, 1.0f, ringFromTop ? 1.0f - binding.get() : binding.get()), true);
        g.setColour (ring);
        g.strokePath (arc, juce::PathStrokeType (thick));
    }
    auto t = r.reduced (juce::jlimit (1, screenCap (4), r.getWidth() / 12), 3);
    g.setColour (Colours::caption.withAlpha (a));
    g.setFont (wingFontPx (juce::jmin (wingPx (14.0f), (float) t.getHeight() * 0.26f)));
    g.drawFittedText (on ? binding.label.toUpperCase() : placeholder, t.removeFromTop (t.getHeight() * 45 / 100), juce::Justification::bottomLeft, 1, 0.7f);
    g.setColour (Dark::text.withAlpha (a));
    g.setFont (wingFontPx (juce::jmin (wingPx (21.0f), (float) t.getHeight() * 0.8f)));
    g.drawFittedText (on && binding.text ? binding.text() : placeholderValue, t, juce::Justification::centredLeft, 1, 0.7f);
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
    1-6 (PEQ), H (high shelf) - with PAN and WIDTH. The top row: ON, RTA, the
    path, PAN, WIDTH, CLOSE; the graph with a line and ring per band (drag:
    frequency and gain, wheel: Q of a PEQ band) over the path's RTA; the band
    keys at the right. The knobs under the display edit the selected band's
    GAIN, FREQ and Q. The RTA reads the path PRE or POST its EQ and draws as
    EQ/RTA SETTINGS say (RTA SOURCE, COLOR, RANGE, FIXED GAIN, AUTOGAIN). */
class MonitorEqView final : public juce::Component, private juce::Timer
{
public:
    explicit MonitorEqView (MixerEngine& e) : engine (e)
    {
        on.isOn = [this] { return dsp().eqOn.load(); };
        on.onClick = [this] { dsp().eqOn = ! dsp().eqOn.load(); };
        addAndMakeVisible (on);
        rtaKey.isOn = [] { return rtaWanted; };
        rtaKey.onClick = [this]
        {
            rtaWanted = ! rtaWanted;
            updateRta();
            repaint();
        };
        addAndMakeVisible (rtaKey);
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
        startTimerHz (30);
    }

    ~MonitorEqView() override { stopRta(); }

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
            k.push_back (bindLog ("Q " + tag, b.q, 0.442f, 10.0f, {}, 2.0f));
        return k;
    }

    void resized() override
    {
        auto r = getLocalBounds();
        bar = r.removeFromTop (juce::jlimit (30, screenCap (50), getHeight() / 9));
        auto b = bar.reduced (4, 4);
        on.setBounds (b.removeFromLeft (juce::jmin (screenCap (90), b.getWidth() / 9)));
        b.removeFromLeft (4);
        rtaKey.setBounds (b.removeFromLeft (juce::jmin (screenCap (90), b.getWidth() / 8)));
        close.setBounds (b.removeFromRight (juce::jmin (screenCap (110), b.getWidth() / 8)));
        b.removeFromRight (16);
        width.setBounds (b.removeFromRight (juce::jmin (screenCap (150), b.getWidth() / 6)));
        b.removeFromRight (8);
        pan.setBounds (b.removeFromRight (juce::jmin (screenCap (150), b.getWidth() / 5)));
        title = b;
        r.removeFromTop (6);
        auto side = r.removeFromRight (juce::jlimit (70, screenCap (130), getWidth() * 12 / 100)).reduced (4, 0);
        r.removeFromRight (4);
        graph = r.reduced (2, 0).toFloat();
        const int kh = juce::jmin (screenCap (40), side.getHeight() / ld::kMonitorEqBands);
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
        const float rulerH = juce::jlimit (5.0f, screenCap (12.0f), area.getHeight() * 0.04f);
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
        g.setFont (wingFont (TypeScale::statusCaption));
        for (float f : { 20.0f, 40.0f, 60.0f, 100.0f, 200.0f, 400.0f, 600.0f, 1000.0f, 2000.0f, 4000.0f, 6000.0f, 10000.0f, 20000.0f })
            g.drawText (f >= 1000.0f ? juce::String (juce::roundToInt (f / 1000.0f)) + "k" : juce::String (juce::roundToInt (f)),
                        juce::Rectangle<float> (xForFreq (f) - 16.0f, labels.getY() + 2.0f, 32.0f, labels.getHeight() - 2.0f).toNearestInt(),
                        juce::Justification::centred);

        if (rtaShown)
            paintRta (g);

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
        g.setColour ((active ? home::colours::eqFill : juce::Colour (0xff1c1c1c)).withMultipliedAlpha (rtaShown ? 0.6f : 1.0f));
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
            g.setFont (wingFont (TypeScale::statusCaption * 1.2f));
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
    // A WING after INITIALIZE (livedaw-engine MONITOR_EQ_DEFAULTS).
    static constexpr float kDefaultFreqs[ld::kMonitorEqBands] = { 60.14f, 129.88f, 299.25f, 699.49f, 1499.8f, 2992.5f, 6013.9f, 12000.0f };

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

    /** Drives the analyser while the RTA is wanted and the view shows. */
    void updateRta()
    {
        Analysis analysis (engine.getHandle());
        const int path = phones ? 1 : 0;
        if (rtaWanted && isShowing())
        {
            if (! analysis.controls.rtaOn.load() || ! analysis.isAnalysingMonitor (path) || analysis.controls.view.load() != ld::kRtaViewMonitor)
                analysis.showMonitorRta (path);
            const auto before = rta;
            analysis.read (rta);
            // AUTOGAIN: the power average of the bands sits mid-scale (smoothed).
            double sum = 0.0;
            for (float level : rta.levels)
                sum += std::pow (10.0, (double) level / 10.0);
            const float average = (float) (10.0 * std::log10 (sum / ld::kRtaBands + 1.0e-15));
            if (average > -110.0f)
                autoGainDb += (average - autoGainDb) * 0.12f;
            if (! rtaShown || (rta.frame != before.frame && (rta.levels != before.levels || rta.peaks != before.peaks)))
                repaint();
            rtaShown = true;
        }
        else
        {
            stopRta();
            if (std::exchange (rtaShown, false))
                repaint();
        }
    }

    void stopRta()
    {
        Analysis analysis (engine.getHandle());
        if (analysis.controls.view.load() == ld::kRtaViewMonitor && analysis.controls.stripKind.load() == ld::kAnalyseMonitor)
            analysis.hideRta();
    }

    void paintRta (juce::Graphics& g)
    {
        const auto& s = Analysis (engine.getHandle()).settings (ld::kRtaViewMonitor);
        const float range = s.range.load() == 0 ? 30.0f : 60.0f;
        const float gain = juce::jlimit (-5.0f, 50.0f, s.gainDb.load());
        // The top of the scale: 0 dBFS (less FIXED GAIN), or with AUTOGAIN the
        // signal's average at mid-scale (as on the channel EQ).
        const float top = (s.autogain.load() ? autoGainDb + range * 0.5f : 0.0f) - gain;
        const auto colour = ResponseGraph::rtaColour (s.colour.load());
        const auto hold = colour.withAlpha (juce::jmin (1.0f, colour.getFloatAlpha() + 0.35f));
        auto heightOf = [&] (float db) { return juce::jlimit (0.0f, 1.0f, (db - (top - range)) / range) * plot.getHeight(); };
        g.saveState();
        g.reduceClipRegion (plot.toNearestInt());
        for (int b = 0; b < Analysis::numBands; ++b)
        {
            const float x0 = xForFreq (Analysis::bandLowHz (b)), x1 = xForFreq (Analysis::bandHighHz (b));
            if (x1 < plot.getX() || x0 > plot.getRight())
                continue;
            const float w = juce::jmax (1.0f, x1 - x0 - (x1 - x0 > 3.0f ? 1.0f : 0.0f));
            if (const float h = heightOf (rta.levels[(size_t) b]); h > 0.5f)
            {
                g.setColour (colour);
                g.fillRect (x0, plot.getBottom() - h, w, h);
            }
            if (const float p = heightOf (rta.peaks[(size_t) b]); p > 1.0f)
            {
                g.setColour (hold);
                g.fillRect (x0, plot.getBottom() - p, w, juce::jmin (2.0f, p));
            }
        }
        g.restoreState();
    }

    void timerCallback() override
    {
        updateRta();
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
    /** RTA on (kept while the app runs, for both paths). */
    static inline bool rtaWanted = true;
    Analysis::Bands rta;
    bool rtaShown = false;
    float autoGainDb = 0.0f;
    home::IconKey on { "ON", std::nullopt, Colours::gold }, rtaKey { "RTA", std::nullopt, Colours::gold }, close { "CLOSE", Icon::Check };
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
        { "PHONES", Icon::Headphones, pathOlive, [&e] { return e.soloBus.load() == 0; }, [pickPath] { pickPath (0); }, {} },
        { "SPEAKERS", Icon::Speaker, pathOlive, [&e] { return e.soloBus.load() == 1; }, [pickPath] { pickPath (1); }, {} },
        { "PH+SPK", Icon::PhonesSpeaker, pathOlive, [&e] { return e.soloBus.load() == 2; }, [pickPath] { pickPath (2); }, {} },
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
    busDim.setBinding (bindLinear ("BUS DIM", p.busDimDb, 0.0f, 40.0f, " dB", 0, 0.0f));
    monDim.setBinding (bindLinear ("MON DIM", p.monDimDb, 0.0f, 40.0f, " dB", 0, 0.0f));
    repaint();
}

void MonitorsPage::showPathTab (int t)
{
    pathTab = juce::jlimit (0, 1, t);
    auto& src = pathSource();
    auto& d = pathDsp();
    srcLevel.setBinding (home::wingFader ("SRC LEVEL", src.sourceLevelDb));
    srcMix.setBinding (home::wingFader ("SRC MIX", src.sourceMixDb));
    dim.setBinding (bindLinear ("DIM", pathDim(), 0.0f, 40.0f, " dB", 0, 20.0f));
    pflDim.setBinding (bindLinear ("PFL DIM", src.pflDimDb, 0.0f, 40.0f, " dB", 0, 12.0f));
    bandSolo.setBinding (bindLinear ("BAND SOLO", d.bandSoloDb, 0.0f, 24.0f, " dB", 0, 0.0f));
    limiter.setBinding (bindLinear ("LIMITER", d.limiterDb, -40.0f, 0.0f, " dB", 0, 0.0f));
    limiter.ringFromTop = true;
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
    // The parameter bar (fig. 72, firmware 3.1): SRC LEVEL, SRC MIX, PFL DIM,
    // DIST, then the DIRECT IN level (the console has no control for it),
    // only while a DIRECT IN source is chosen: with DIRECT IN OFF the console's
    // bar ends at DIST.
    auto& d = pathDsp();
    auto dist = bindLog ("DIST", d.delayM, 0.1f, 100.0f, " m", 0.1f);
    dist.text = [&m = d.delayM] { return juce::String (m.load(), m.load() < 10.0f ? 1 : 0) + " m"; };
    return { {}, srcLevel.getBinding(), srcMix.getBinding(), pflDim.getBinding(), dist,
             d.directSource.load() >= 0 ? home::wingFader ("DIR LVL", d.directLevelDb) : KnobBinding {} };
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
    return { {}, style (KnobStyle::Bar::Fill, KnobColours::white),
             style (KnobStyle::Bar::Fill, KnobColours::white), style (KnobStyle::Bar::FillRight, KnobColours::blue),
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
    // What the path plays without a solo: OFF (only the solo bus), MAIN.1-4,
    // MTX.1-8, BUS.1-16, AUX.1-8 (the WING's /cfg/mon/n/src).
    auto& target = pathSource().source;
    juce::StringArray items { "OFF" };
    for (int i = 0; i < ld::kNumMonitorSources; ++i)
        items.add (MixerEngine::monitorSourceName (i));
    popups::ListOptions options;
    options.columns = 4;
    options.caption = "SOURCE";
    anchor.setBounds (sourceField);
    juce::Component::SafePointer<MonitorsPage> safe (this);
    popups::chooseFrom (anchor, items, juce::jmax (-1, target.load()) + 1, [safe, &target] (int i)
    {
        target = i - 1;
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
        {
            safe->repaint();
            if (safe->knobsChanged)
                safe->knobsChanged();
        }
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
        // Both paths from the meter bank (the main meter can show them too).
        peak[(size_t) ch] = meterPos (dsp::gainToDb (showsPhones() ? ctx.meters.getPhones (ch) : ctx.meters.getMonitor (ch)));
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
    // The talk side's controls dim with it (no talkback channel).
    const float talkAlpha = ctx.engine.getTalkStrip() == nullptr ? 0.5f : 1.0f;
    for (juce::Component* c : { (juce::Component*) switchMode.get(), (juce::Component*) sendLevels.get(), (juce::Component*) &busDim, (juce::Component*) &monDim })
        if (c->getAlpha() != talkAlpha)
        {
            c->setAlpha (talkAlpha);
            repaint (talkTabs.getUnion (autoDimBox).getUnion (talkArea));
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
        // [WING] a 2 px black frame; unlit: the colour at a fifth, its print in the colour.
        auto r = t.area.toFloat();
        g.setColour (juce::Colours::black);
        g.fillRect (r);
        r = r.reduced (wingPx (2.0f));
        g.setColour (lit ? t.colour : t.colour.withMultipliedBrightness (0.22f));
        g.fillRect (r);
        const auto ink = lit ? juce::Colours::black : t.colour;
        auto label = r.removeFromBottom (wingPx (24.0f));
        auto icon = r.withSizeKeepingCentre (wingPx (32.0f), wingPx (32.0f)).translated (0.0f, wingPx (4.0f));
        g.setColour (ink);
        juce::Path p;
        if (t.icon == Icon::ChevronUp)
        {
            // TALK: a filled triangle up.
            p.addTriangle (icon.getX(), icon.getCentreY() + icon.getHeight() * 0.22f, icon.getRight(), icon.getCentreY() + icon.getHeight() * 0.22f,
                           icon.getCentreX(), icon.getCentreY() - icon.getHeight() * 0.25f);
            g.fillPath (p);
        }
        else if (t.icon == Icon::ChevronDown)
        {
            // DIM: a small filled triangle down.
            auto s = icon.reduced (icon.getWidth() * 0.18f);
            p.addTriangle (s.getX(), s.getCentreY() - s.getHeight() * 0.2f, s.getRight(), s.getCentreY() - s.getHeight() * 0.2f, s.getCentreX(),
                           s.getCentreY() + s.getHeight() * 0.25f);
            g.fillPath (p);
        }
        else if (t.icon == Icon::Cancel)
        {
            // MUTE: a cross.
            auto s = icon.reduced (icon.getWidth() * 0.3f);
            g.drawLine ({ s.getTopLeft(), s.getBottomRight() }, wingPx (3.0f));
            g.drawLine ({ s.getTopRight(), s.getBottomLeft() }, wingPx (3.0f));
        }
        else if (t.icon == Icon::WidthArrows)
        {
            // LR SWAP: two rings side by side, the left one filled.
            auto s = icon.withSizeKeepingCentre (icon.getWidth() * 0.8f, icon.getWidth() * 0.45f);
            const float d = s.getHeight();
            g.fillEllipse (s.getX(), s.getY(), d, d);
            g.drawEllipse (s.getRight() - d, s.getY(), d, d, wingPx (2.0f));
        }
        else if (t.icon == Icon::Circle)
            g.drawEllipse (icon.reduced (icon.getWidth() * 0.25f), wingPx (2.0f)); // MONO: a ring
        else
            drawIcon (g, t.icon, icon, ink);
        g.setColour (ink);
        g.setFont (capFont (10.0f, WingWeight::SemiBold));
        g.drawFittedText (t.text, label.toNearestInt(), juce::Justification::centred, 1, 0.6f);
    }
}

void MonitorsPage::paintTalk (juce::Graphics& g)
{
    // Without a talkback channel the talk side is dim (TALKBACK DISABLED).
    const bool talkDim = ctx.engine.getTalkStrip() == nullptr;
    g.setColour (dimmed (Dark::header, talkDim));
    g.fillRect (talkTabs);
    for (int i = 0; i < 2; ++i)
        tab (g, talkTabKeys[i], i == 0 ? Icon::TalkHead : Icon::TalkHeadRight, i == 0 ? "TALK A" : "TALK B", talkTab == i, talkDim); // [3.1] a speaking head
    g.setColour (juce::Colours::black);
    g.fillRect (talkTabKeys[0].getUnion (talkTabKeys[1]).withTop (talkTabKeys[0].getBottom()).withHeight (px (3.0f)));

    panel (g, switchHeader, "SWITCH MODE", talkDim);
    panel (g, autoDimHeader, "AUTO DIM", talkDim);
    g.setColour (dimmed (Dark::body, talkDim));
    for (auto r : { switchBox, autoDimBox, talkArea })
        g.fillRect (r);

    if (talkDim)
    {
        // TALKBACK DISABLED (touch it to choose the channel).
        auto r = talkChannelField.reduced (px (20.0f), 0);
        const auto ink = Colours::gold.withMultipliedBrightness (0.75f);
        drawIcon (g, Icon::Info, r.removeFromLeft (px (28.0f)).toFloat().withSizeKeepingCentre (wingPx (24.0f), wingPx (24.0f)), ink);
        g.setColour (ink);
        g.setFont (capFont (15.0f));
        g.drawFittedText ("TALKBACK\nDISABLED", r, juce::Justification::centred, 2, 0.8f);
    }
    else
        field (g, talkChannelField.reduced (px (8.0f), px (7.0f)), "TALKBACK", talkChannelName(), true);

    g.setColour (dimmed (Colours::caption, talkDim));
    g.setFont (capFont (15.0f));
    g.drawText ("USE SEND LEVELS", sendLevelsRow, juce::Justification::centredLeft);
    const auto& p = talkPath();
    tileGrid (g, busGrid, busTiles, "TALK TO BUS", "BUS", p.buses.load(), talkDim);
    tileGrid (g, mtxGrid, mtxTiles, "TALK TO MATRIX", "MTX", p.matrices.load(), talkDim);
    tileGrid (g, mainGrid, mainTiles, "TALK TO MAIN", "MAIN", p.mains.load(), talkDim);
}

void MonitorsPage::paintPath (juce::Graphics& g)
{
    g.setColour (Dark::header);
    g.fillRect (pathTabs);
    tab (g, pathTabKeys[0], Icon::Headphones, "PHONES", showsPhones(), false);
    tab (g, pathTabKeys[1], Icon::Speaker, "SPEAKERS", ! showsPhones(), false);
    g.setColour (juce::Colours::black);
    g.fillRect (pathTabKeys[0].getUnion (pathTabKeys[1]).withTop (pathTabKeys[0].getBottom()).withHeight (px (3.0f)));
    panel (g, pathHeader, showsPhones() ? "PHONES SETTINGS" : "SPEAKERS SETTINGS");
    g.setColour (Dark::body);
    for (auto r : { sourceBox, dimBox, directBox, distBox })
        g.fillRect (r);

    auto& d = pathDsp();
    field (g, sourceField, "SOURCE", MixerEngine::monitorSourceName (pathSource().source.load()), true);
    field (g, directField, "DIRECT IN", MixerEngine::directSourceName (d.directSource.load()), true);
    paintEqKey (g);
    paintDistSlider (g);
    auto key = [&g] (juce::Rectangle<int> r, Icon icon, const juce::String& text, bool on, juce::Colour onColour)
    {
        // An outlined icon key, its caption under it.
        auto f = r.toFloat();
        g.setColour (on ? onColour : Dark::key);
        g.fillRect (f);
        g.setColour (Colours::screenLine);
        g.drawRect (f, 1.0f);
        drawIcon (g, icon, f.reduced (f.getWidth() * 0.15f), on ? juce::Colours::black : Colours::screenLine.brighter (0.4f));
        g.setColour (on ? onColour : Colours::screenLine.brighter (0.4f));
        g.setFont (capFont (15.0f));
        g.drawText (text, r.withY (r.getBottom() + px (10.0f)).withHeight (px (24.0f)).expanded (px (30.0f), 0), juce::Justification::centred, false);
    };
    key (invertKey, Icon::Invert, "INVERT", d.invert.load(), KnobColours::yellow);
    key (delayKey, Icon::DeltaT, "DELAY", d.delayOn.load(), Colours::lime);
}

void MonitorsPage::paintEqKey (juce::Graphics& g)
{
    // "EQ" and the path's curve (a flat line while the EQ does nothing):
    // outlined on black, gold while on.
    auto& d = pathDsp();
    const bool on = d.eqOn.load();
    auto r = eqKey.toFloat();
    g.setColour (on ? Colours::gold : juce::Colours::black);
    g.fillRect (r);
    g.setColour (Colours::caption);
    g.drawRect (r, wingPx (1.5f));
    g.setColour (on ? juce::Colours::black : Colours::caption);
    auto text = r.removeFromLeft (wingPx (40.0f));
    g.setFont (capFont (15.0f));
    g.drawText ("EQ", text.withTrimmedLeft (wingPx (8.0f)), juce::Justification::centredLeft, false);
    auto c = r.reduced (wingPx (6.0f), r.getHeight() * 0.25f);
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
    g.strokePath (p, juce::PathStrokeType (wingPx (1.5f)));
}

namespace
{
    // The DIST slider [WING]: 0.1 .. 100 m, linear (100, 75, 50, 25 on its scale).
    float distToPos (float m) { return juce::jlimit (0.0f, 1.0f, (m - 0.1f) / 99.9f); }
    float posToDist (float p) { return 0.1f + 99.9f * juce::jlimit (0.0f, 1.0f, p); }

    /** The travel of the slider's cap inside its well. */
    juce::Rectangle<float> distTrack (juce::Rectangle<int> slider)
    {
        return slider.toFloat().withTrimmedTop (wingPx (14.0f)).withTrimmedBottom (wingPx (14.0f));
    }

    /** A fader's well [WING]: a 2 px black frame round #101010, a black slot. */
    juce::Rectangle<float> paintWell (juce::Graphics& g, juce::Rectangle<float> r, float slotTop, float slotBottom)
    {
        g.setColour (juce::Colours::black);
        g.fillRect (r);
        g.setColour (Dark::well);
        g.fillRect (r.reduced (wingPx (2.0f)));
        const float w = wingPx (12.0f);
        const juce::Rectangle<float> slot (r.getCentreX() - w / 2.0f, slotTop, w, slotBottom - slotTop);
        g.setColour (juce::Colours::black);
        g.fillRoundedRectangle (slot, w * 0.3f);
        return slot;
    }
} // namespace

void MonitorsPage::paintDistSlider (juce::Graphics& g)
{
    auto& d = pathDsp();
    const bool on = d.delayOn.load();
    const float m = d.delayM.load();
    const auto track = distTrack (distSlider);
    const auto slot = paintWell (g, distSlider.toFloat(), (float) distSlider.getY() + wingPx (8.0f), track.getBottom());
    const float y = track.getBottom() - track.getHeight() * distToPos (m);
    if (on)
    {
        g.setColour (Colours::lime);
        g.fillRect (slot.withTop (y));
    }
    // The scale left of the well.
    g.setColour (Colours::caption);
    g.setFont (capFont (10.0f));
    for (float mark : { 25.0f, 50.0f, 75.0f, 100.0f })
    {
        const float my = track.getBottom() - track.getHeight() * distToPos (mark);
        g.drawText (juce::String (juce::roundToInt (mark)), juce::Rectangle<float> ((float) distSlider.getX() - wingPx (40.0f), my - wingPx (8.0f), wingPx (34.0f), wingPx (16.0f)),
                    juce::Justification::centredRight);
    }
    g.drawText ("0.1 m", juce::Rectangle<float> ((float) distSlider.getX() - wingPx (46.0f), track.getBottom() - wingPx (8.0f), wingPx (42.0f), wingPx (16.0f)),
                juce::Justification::centredRight);
    // The cap: the distance in a grey box.
    auto box = juce::Rectangle<float> (wingPx (60.0f), wingPx (24.0f)).withCentre ({ slot.getCentreX(), y });
    g.setColour (on ? Colours::lime : Colours::caption.darker (0.15f));
    g.fillRect (box);
    g.setColour (juce::Colours::black);
    g.setFont (capFont (12.0f));
    g.drawFittedText (juce::String (m, m < 10.0f ? 1 : 0) + " m", box.toNearestInt(), juce::Justification::centred, 1, 0.7f);
}

void MonitorsPage::paintLevel (juce::Graphics& g)
{
    panel (g, levelHeader, "LEVEL");
    g.setColour (Dark::body);
    g.fillRect (levelArea);

    // The meter (L R) with the CLIP .. -60 scale, segments lit green, yellow, red.
    auto m = meterArea.toFloat();
    g.setColour (Colours::caption);
    g.setFont (capFont (9.0f));
    for (auto [text, db] : { std::pair { "CLIP", 0.0f }, std::pair { "-6", -6.0f }, std::pair { "-12", -12.0f }, std::pair { "-24", -24.0f },
                             std::pair { "-36", -36.0f }, std::pair { "-48", -48.0f }, std::pair { "-60", -60.0f } })
    {
        const float y = m.getBottom() - m.getHeight() * meterPos (db);
        g.drawText (text, juce::Rectangle<float> (m.getX() - wingPx (40.0f), y - wingPx (8.0f), wingPx (34.0f), wingPx (16.0f)), juce::Justification::centredRight);
    }
    const float segment = wingPx (3.0f);
    for (int ch = 0; ch < 2; ++ch)
    {
        auto bar = juce::Rectangle<float> (m.getX() + wingPx (19.0f) * (float) ch, m.getY(), wingPx (12.0f), m.getHeight());
        const float level = shownPeak[(size_t) ch];
        for (float y = bar.getBottom() - segment; y >= bar.getY(); y -= segment)
        {
            const float pos = (bar.getBottom() - y) / bar.getHeight();
            const bool lit = pos <= level;
            g.setColour (! lit ? Colours::screenTile
                               : (pos > meterPos (-0.5f) ? Colours::meterRed : pos > meterPos (-12.0f) ? Colours::meterYellow : Colours::meterGreen));
            g.fillRect (bar.getX(), y, bar.getWidth(), segment - wingPx (1.0f));
        }
        g.setColour (Colours::caption);
        g.drawText (ch == 0 ? "L" : "R", juce::Rectangle<float> (bar.getX(), bar.getBottom() + wingPx (2.0f), bar.getWidth(), wingPx (16.0f)), juce::Justification::centred);
    }
    // The limiter's gain reduction (firmware 1.11) down from the top, beside the meter.
    if (shownGr > 0.1f)
    {
        auto gr = juce::Rectangle<float> (m.getX() + wingPx (36.0f), m.getY(), wingPx (4.0f), m.getHeight());
        g.setColour (Colours::amber);
        g.fillRect (gr.withHeight (gr.getHeight() * juce::jlimit (0.0f, 1.0f, shownGr / 20.0f)));
    }

    // The level fader (set by PHONES LEVEL / MONITOR LEVEL): its scale, a lime
    // fill and the value in a white box at the cap.
    const float levelDb = pathLevel().load();
    auto f = faderArea.toFloat();
    const auto lane = f.withTrimmedTop (wingPx (15.0f)).withTrimmedBottom (wingPx (15.0f));
    const auto slot = paintWell (g, f, f.getY() + wingPx (10.0f), lane.getBottom());
    g.setColour (Colours::caption);
    g.setFont (capFont (9.0f));
    for (float db : { 10.0f, 5.0f, 0.0f, -5.0f, -10.0f, -20.0f, -30.0f, -50.0f, -144.0f })
    {
        const float y = lane.getBottom() - lane.getHeight() * fader::dbToPos (db);
        const auto text = db < -100.0f ? juce::String ("-oo") : (db > 0.0f ? "+" : "") + juce::String (juce::roundToInt (db));
        g.drawText (text, juce::Rectangle<float> (f.getX() - wingPx (40.0f), y - wingPx (8.0f), wingPx (34.0f), wingPx (16.0f)), juce::Justification::centredRight);
    }
    const float y = lane.getBottom() - lane.getHeight() * fader::dbToPos (juce::jlimit (-144.0f, 10.0f, levelDb));
    g.setColour (levelLime);
    g.fillRect (slot.withTop (y));
    auto flag = juce::Rectangle<float> (wingPx (64.0f), wingPx (24.0f)).withCentre ({ f.getCentreX(), y });
    g.fillRect (juce::Rectangle<float> (f.getX(), y - wingPx (2.0f), f.getWidth(), wingPx (4.0f)));
    g.setColour (juce::Colours::white);
    g.fillRoundedRectangle (flag, wingPx (2.0f));
    g.setColour (juce::Colours::black);
    g.setFont (capFont (12.0f));
    g.drawFittedText (home::wingDb (levelDb) + " dB", flag.toNearestInt(), juce::Justification::centred, 1, 0.6f);
}

void MonitorsPage::resized()
{
    // [WING] firmware 3.1, in the console's pixels from the page's top
    // (the status bar's bottom, y = 80).
    if (eqView != nullptr)
        eqView->setBounds (getLocalBounds());

    // The toolbar: TALK A / B, PHONES / SPEAKERS / PH+SPK, DIM / MUTE / MONO / LR SWAP.
    toolbar = wingRect (8.0f, 0.0f, 80.0f, 672.0f);
    {
        static constexpr float tops[] = { 4.0f, 74.0f, 160.0f, 230.0f, 300.0f, 386.0f, 456.0f, 526.0f, 596.0f };
        for (size_t i = 0; i < tools.size() && i < std::size (tops); ++i)
            tools[i].area = wingRect (12.0f, tops[i], 72.0f, 70.0f);
    }

    // TALK A / TALK B.
    talkTabs = wingRect (128.0f, 0.0f, 464.0f, 64.0f);
    talkTabKeys[0] = wingRect (128.0f, 0.0f, 190.0f, 55.0f);
    talkTabKeys[1] = wingRect (320.0f, 0.0f, 190.0f, 55.0f);
    switchHeader = wingRect (128.0f, 72.0f, 192.0f, 64.0f);
    switchBox = wingRect (128.0f, 144.0f, 192.0f, 202.0f);
    switchMode->setBounds (wingRect (165.0f, 160.0f, 118.0f, 170.0f));
    talkChannelField = wingRect (128.0f, 346.0f, 192.0f, 78.0f);
    autoDimHeader = wingRect (128.0f, 424.0f, 192.0f, 64.0f);
    autoDimBox = wingRect (128.0f, 496.0f, 192.0f, 176.0f);
    busDim.setBounds (wingRect (144.0f, 512.0f, 160.0f, 64.0f));
    monDim.setBounds (wingRect (144.0f, 592.0f, 160.0f, 64.0f));
    talkArea = wingRect (328.0f, 72.0f, 264.0f, 600.0f);
    sendLevelsRow = wingRect (348.0f, 72.0f, 170.0f, 64.0f);
    sendLevels->setBounds (wingRect (528.0f, 84.0f, 48.0f, 40.0f));
    busGrid = wingRect (344.0f, 140.0f, 232.0f, 254.0f);
    mtxGrid = wingRect (344.0f, 411.0f, 232.0f, 142.0f);
    mainGrid = wingRect (344.0f, 570.0f, 232.0f, 86.0f);
    busTiles = gridTiles (busGrid, kWingBuses, 4);
    mtxTiles = gridTiles (mtxGrid, kNumMatrices, 4);
    mainTiles = gridTiles (mainGrid, kNumMains, 4);

    // PHONES / SPEAKERS SETTINGS: SOURCE, SRC LEVEL, SRC MIX; DIM, PFL DIM,
    // BAND SOLO; DIRECT IN; the EQ key, the DIST slider, INVERT and DELAY.
    pathTabs = wingRect (632.0f, 0.0f, 640.0f, 64.0f);
    pathTabKeys[0] = wingRect (632.0f, 0.0f, 189.0f, 55.0f);
    pathTabKeys[1] = wingRect (824.0f, 0.0f, 189.0f, 55.0f);
    pathHeader = wingRect (632.0f, 72.0f, 424.0f, 64.0f);
    sourceBox = wingRect (632.0f, 144.0f, 208.0f, 264.0f);
    sourceField = wingRect (648.0f, 160.0f, 176.0f, 64.0f);
    srcLevel.setBounds (wingRect (648.0f, 248.0f, 176.0f, 64.0f));
    srcMix.setBounds (wingRect (648.0f, 328.0f, 176.0f, 64.0f));
    dimBox = wingRect (632.0f, 416.0f, 208.0f, 256.0f);
    dim.setBounds (wingRect (648.0f, 432.0f, 176.0f, 64.0f));
    pflDim.setBounds (wingRect (648.0f, 512.0f, 176.0f, 64.0f));
    bandSolo.setBounds (wingRect (648.0f, 592.0f, 176.0f, 64.0f));
    directBox = wingRect (848.0f, 144.0f, 208.0f, 96.0f);
    directField = wingRect (864.0f, 160.0f, 176.0f, 64.0f);
    distBox = wingRect (848.0f, 248.0f, 208.0f, 424.0f);
    eqKey = wingRect (864.0f, 260.0f, 176.0f, 64.0f);
    distSlider = wingRect (894.0f, 336.0f, 64.0f, 320.0f);
    invertKey = wingRect (983.0f, 457.0f, 48.0f, 48.0f);
    delayKey = wingRect (983.0f, 577.0f, 48.0f, 48.0f);
    pathArea = wingRect (632.0f, 144.0f, 424.0f, 528.0f);

    // LEVEL: LIMITER, the meter, the level fader.
    levelHeader = wingRect (1064.0f, 72.0f, 208.0f, 64.0f);
    levelArea = wingRect (1064.0f, 144.0f, 208.0f, 528.0f);
    limiter.setBounds (wingRect (1080.0f, 160.0f, 176.0f, 64.0f));
    meterArea = wingRect (1104.0f, 254.0f, 31.0f, 326.0f);
    faderArea = wingRect (1176.0f, 240.0f, 80.0f, 416.0f);
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
    if (talkTabKeys[0].contains (pos) || talkTabKeys[1].contains (pos))
        showTalkTab (talkTabKeys[0].contains (pos) ? 0 : 1);
    else if (pathTabKeys[0].contains (pos) || pathTabKeys[1].contains (pos))
        showPathTab (pathTabKeys[0].contains (pos) ? 0 : 1);
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
