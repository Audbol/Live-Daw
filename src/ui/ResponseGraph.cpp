#include "ui/ResponseGraph.h"
#include "engine/Models.h"
#include "ui/Knobs.h"

namespace livedaw::ui
{

namespace
{
    // The 3.1 colours (sampled from the console's EQ and FILTER screens).
    const juce::Colour eqCurveColour { 0xfff0a830 };
    const juce::Colour eqFillColour { 0xff38280f };
    const juce::Colour filterCurveColour { 0xff4890e0 };
    const juce::Colour selectedBandColour { 0xff4890e0 };
    const juce::Colour gridColour { 0xff383838 };
    const juce::Colour bandLineColour { 0xff585858 };
    const juce::Colour labelColour { 0xffc0c0c0 };
    const juce::Colour dimLabelColour { 0xff707070 };
    const juce::Colour allPassColour { 0xffe0d400 };

    /** "20", "100", "1K", "10K" (the console writes an upper-case K). */
    juce::String rulerText (float f)
    {
        return f >= 1000.0f ? juce::String (juce::roundToInt (f / 1000.0f)) + "K" : juce::String (juce::roundToInt (f));
    }

    /** An open ring handle (filled black when selected). */
    void paintRing (juce::Graphics& g, juce::Point<float> c, float radius, juce::Colour colour, bool filled)
    {
        auto r = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c);
        if (filled)
        {
            g.setColour (juce::Colours::black);
            g.fillEllipse (r);
        }
        g.setColour (colour);
        g.drawEllipse (r, juce::jmax (1.5f, radius * 0.28f));
    }
} // namespace

void ResponseGraph::paintPianoRuler (juce::Graphics& g, juce::Rectangle<float> area, std::function<float (float)> xForHz)
{
    // White keys: grey blocks parted by black lines (full height between E-F
    // and B-C, the lower half elsewhere); black keys: dark notches on top.
    auto noteHz = [] (float note) { return 440.0f * std::pow (2.0f, (note - 69.0f) / 12.0f); };
    g.setColour (juce::Colour (0xff606060));
    g.fillRect (area);
    const float notchH = area.getHeight() * 0.55f;
    for (int note = 15; note <= 136; ++note)
    {
        const int pc = note % 12;
        const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
        const float x0 = xForHz (noteHz ((float) note - 0.5f)), x1 = xForHz (noteHz ((float) note + 0.5f));
        if (x1 < area.getX() || x0 > area.getRight())
            continue;
        if (black)
        {
            auto k = juce::Rectangle<float> (x0, area.getY(), x1 - x0, notchH).getIntersection (area);
            g.setColour (juce::Colours::black);
            g.fillRect (k);
            g.setColour (juce::Colour (0xff383838));
            g.fillRect (k.reduced (0.75f, 0.0f).withTrimmedBottom (0.75f));
            // The white keys' line under the black key.
            g.setColour (juce::Colours::black);
            g.fillRect ((x0 + x1) * 0.5f - 0.5f, area.getY() + notchH, 1.0f, area.getHeight() - notchH);
        }
        else if (pc == 4 || pc == 11)
        {
            g.setColour (juce::Colours::black);
            g.fillRect (x1 - 0.5f, area.getY(), 1.0f, area.getHeight());
        }
    }
}

ResponseGraph* ResponseGraph::rtaOwner = nullptr;
int ResponseGraph::lastShownView = ld::kRtaViewHome;

ResponseGraph::~ResponseGraph()
{
    // The analyser idles by itself once nobody reads it.
    if (rtaOwner == this)
        rtaOwner = nullptr;
}

juce::Colour ResponseGraph::rtaColour (int index)
{
    // RD, AM, BL at 25, 50, 75 % (the console's red, amber and blue).
    static const juce::Colour hues[] = { juce::Colour (0xffe8343c), juce::Colour (0xfff0a830), juce::Colour (0xff3c8cf0) };
    const int i = juce::jlimit (0, 8, index);
    return hues[i / 3].withAlpha (0.25f * (float) (i % 3 + 1));
}

int ResponseGraph::rtaViewId() const
{
    switch (rtaView)
    {
        case RtaView::None:   return -1;
        case RtaView::Home:   return ld::kRtaViewHome;
        case RtaView::Eq:     return ld::kRtaViewEq;
        case RtaView::Filter: return ld::kRtaViewFilter;
        case RtaView::Auto:   break;
    }
    return block == Filter ? ld::kRtaViewFilter : (ruler == Ruler::Top ? ld::kRtaViewEq : ld::kRtaViewHome);
}

ResponseGraph::RtaLayout ResponseGraph::rtaLayout() const
{
    RtaLayout l;
    if (strip == nullptr || (l.view = rtaViewId()) < 0)
        return l;
    const int display = Analysis (*strip).settings (l.view).display.load();
    if (l.view == ld::kRtaViewHome)
    {
        // OFF, 1/3, FULL: over the overview's curve.
        static const float fractions[] = { 0.0f, 1.0f / 3.0f, 1.0f };
        l.fraction = fractions[juce::jlimit (0, 2, display)];
    }
    else
    {
        // OFF, 1/4, 1/3, 1/2 (a strip under the curve), OVL/3, OVL (over it).
        static const float fractions[] = { 0.0f, 0.25f, 1.0f / 3.0f, 0.5f, 1.0f / 3.0f, 1.0f };
        const int d = juce::jlimit (0, 5, display);
        l.fraction = fractions[d];
        l.split = d >= 1 && d <= 3 && block == Eq;
    }
    if (l.fraction <= 0.0f)
        l.view = -1;
    return l;
}

void ResponseGraph::updateRta()
{
    if (isShowing() && rtaViewId() >= 0)
        lastShownView = rtaViewId();
    const auto layout = rtaLayout();
    if (layout.view >= 0 && isShowing())
    {
        Analysis analysis (*strip);
        if (rtaOwner != this || ! analysis.controls.rtaOn.load() || ! analysis.isAnalysed (*strip) || analysis.controls.view.load() != layout.view)
        {
            analysis.showRta (strip, layout.view);
            rtaOwner = this;
        }
        const auto before = rta;
        analysis.read (rta);
        // AUTOGAIN: the power average of the bands sits mid-scale (smoothed).
        double sum = 0.0;
        for (float level : rta.levels)
            sum += std::pow (10.0, (double) level / 10.0);
        const float average = (float) (10.0 * std::log10 (sum / ld::kRtaBands + 1.0e-15));
        if (average > -110.0f)
            autoGainDb += (average - autoGainDb) * 0.12f;
        // A new frame that changes nothing (silence) needs no repaint: the
        // graph can be big (a maximized screen).
        if (! rtaShown || (rta.frame != before.frame && (rta.levels != before.levels || rta.peaks != before.peaks)))
            repaint();
        rtaShown = true;
        if (getTimerInterval() > 40)
            startTimerHz (30);
    }
    else
    {
        if (rtaOwner == this)
        {
            if (strip != nullptr)
                Analysis (*strip).hideRta();
            rtaOwner = nullptr;
        }
        if (rtaShown)
        {
            rtaShown = false;
            repaint();
        }
        if (getTimerInterval() < 50)
            startTimerHz (15);
    }
}

void ResponseGraph::paintRta (juce::Graphics& g, juce::Rectangle<float> area, const RtaLayout& layout)
{
    const auto& s = Analysis (*strip).settings (layout.view);
    const float range = s.range.load() == 0 ? 30.0f : 60.0f;
    const float gain = juce::jlimit (-5.0f, 50.0f, s.gainDb.load());
    // The top of the scale: 0 dBFS (less FIXED GAIN), or with AUTOGAIN the
    // signal's average at mid-scale.
    const float top = (s.autogain.load() ? autoGainDb + range * 0.5f : 0.0f) - gain;
    const auto colour = rtaColour (s.colour.load());
    const auto hold = colour.withAlpha (juce::jmin (1.0f, colour.getFloatAlpha() + 0.35f));
    auto heightOf = [&] (float db) { return juce::jlimit (0.0f, 1.0f, (db - (top - range)) / range) * area.getHeight(); };

    g.saveState();
    g.reduceClipRegion (area.toNearestInt());
    for (int b = 0; b < Analysis::numBands; ++b)
    {
        const float x0 = xForFreq (Analysis::bandLowHz (b)), x1 = xForFreq (Analysis::bandHighHz (b));
        if (x1 < area.getX() || x0 > area.getRight())
            continue;
        const float w = juce::jmax (1.0f, x1 - x0 - (x1 - x0 > 3.0f ? 1.0f : 0.0f));
        const float h = heightOf (rta.levels[(size_t) b]);
        if (h > 0.5f)
        {
            g.setColour (colour);
            g.fillRect (x0, area.getBottom() - h, w, h);
        }
        const float p = heightOf (rta.peaks[(size_t) b]);
        if (p > 1.0f)
        {
            g.setColour (hold);
            g.fillRect (x0, area.getBottom() - p, w, juce::jmin (2.0f, p));
        }
    }
    g.restoreState();
}

juce::Rectangle<float> ResponseGraph::curvePlot() const
{
    auto p = plot();
    if (rtaShown)
        if (const auto l = rtaLayout(); l.view >= 0 && l.split)
            p.removeFromBottom (p.getHeight() * l.fraction);
    return p;
}

ResponseGraph::ResponseGraph()
{
    const int n = 240;
    for (int i = 0; i < n; ++i)
        freqs.push_back (20.0 * std::pow (1000.0, (double) i / (n - 1)));
    response.assign (freqs.size(), 0.0);
    eqResponse.assign (freqs.size(), 0.0);
    startTimerHz (15);
}

void ResponseGraph::setStrip (Strip* s, Block b)
{
    strip = s;
    block = b;
    builtModel = -2;
    selectedBand = -1;
    updateBands();
    timerCallback();
    repaint();
}

juce::StringArray ResponseGraph::stdBands (const Strip& s)
{
    const auto table = static_cast<models::Table> (s.getEqTable());
    const auto& spec = models::model (table, s.params.eq.model.load());
    juce::StringArray ids;
    if (spec.token != "STD")
        return ids;
    for (auto& p : spec.params)
        if (p.id.endsWith ("f") && p.id.length() == 2 && spec.find (p.id.dropLastCharacters (1) + "g") >= 0)
            ids.add (p.id.dropLastCharacters (1));
    return ids;
}

void ResponseGraph::updateBands()
{
    bands.clear();
    if (strip == nullptr || block != Eq)
        return;

    builtModel = strip->params.eq.model.load();
    const auto& spec = models::model (static_cast<models::Table> (strip->getEqTable()), builtModel);
    for (auto& id : stdBands (*strip))
        bands.push_back ({ id, spec.find (id + "f"), spec.find (id + "g"), spec.find (id + "q") });
}

void ResponseGraph::timerCallback()
{
    if (strip == nullptr)
        return;
    updateRta();

    if (block == Eq && strip->params.eq.model.load() != builtModel)
        updateBands();

    std::vector<double> next (freqs.size(), 0.0);
    ld::ld_strip_response (strip->getHandle(), block, freqs.data(), next.data(), (int32_t) freqs.size(), 48000.0);
    bool changed = next != response;
    if (changed)
        response = std::move (next);
    if (block == Filter)
    {
        // The channel EQ, drawn behind the filter.
        std::vector<double> eq (freqs.size(), 0.0);
        ld::ld_strip_response (strip->getHandle(), Eq, freqs.data(), eq.data(), (int32_t) freqs.size(), 48000.0);
        if (eq != eqResponse)
        {
            eqResponse = std::move (eq);
            changed = true;
        }
    }
    if (changed || block == Filter)
        repaint();
}

juce::Rectangle<float> ResponseGraph::plot() const
{
    // Labels only when there is room for them (the full screens); the overview
    // panel shows the bare grid like the console.
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const float rulerH = juce::jlimit (5.0f, 14.0f, getHeight() * 0.03f);
    if (ruler == Ruler::Top)
        r.removeFromTop (rulerH + juce::jmax (4.0f, getHeight() * 0.02f));
    else if (ruler == Ruler::Bottom)
        r.removeFromBottom (rulerH);
    if (showLabels())
    {
        r.removeFromBottom (15.0f);
        if (block == Filter || ! bands.empty())
            r.removeFromTop (18.0f);
    }
    return r;
}

bool ResponseGraph::showLabels() const
{
    return getHeight() > 200 && getWidth() > 300;
}

float ResponseGraph::xForFreq (float f) const
{
    const auto p = plot();
    return p.getX() + p.getWidth() * std::log (juce::jmax (20.0f, f) / 20.0f) / std::log (1000.0f);
}

float ResponseGraph::freqForX (float x) const
{
    const auto p = plot();
    return 20.0f * std::pow (1000.0f, juce::jlimit (0.0f, 1.0f, (x - p.getX()) / p.getWidth()));
}

float ResponseGraph::yForDb (float db) const
{
    const auto p = curvePlot();
    return p.getCentreY() - juce::jlimit (-kRangeDb, kRangeDb, db) / kRangeDb * p.getHeight() * 0.5f;
}

float ResponseGraph::dbForY (float y) const
{
    const auto p = curvePlot();
    return (p.getCentreY() - y) / (p.getHeight() * 0.5f) * kRangeDb;
}

juce::Path ResponseGraph::curveOf (const std::vector<double>& db) const
{
    juce::Path curve;
    for (size_t i = 0; i < freqs.size() && i < db.size(); ++i)
    {
        const auto pt = juce::Point<float> (xForFreq ((float) freqs[i]), yForDb ((float) db[i]));
        if (i == 0)
            curve.startNewSubPath (pt);
        else
            curve.lineTo (pt);
    }
    return curve;
}

void ResponseGraph::paint (juce::Graphics& g)
{
    const auto p = plot();
    const auto bounds = getLocalBounds().toFloat().reduced (1.0f);
    g.fillAll (juce::Colours::black);

    // Grid: a line for every 10, 20 ... 90 Hz, 100, 200 ... and four level
    // divisions, all in the same dark grey.
    g.setColour (gridColour);
    for (float decade : { 10.0f, 100.0f, 1000.0f, 10000.0f })
        for (int m = 1; m <= 9; ++m)
        {
            const float f = decade * (float) m;
            if (f < 20.0f || f > 20000.0f)
                continue;
            g.drawVerticalLine (juce::roundToInt (xForFreq (f)), p.getY(), p.getBottom());
        }
    const auto cp = curvePlot();
    for (int i = 0; i <= 4; ++i)
        g.drawHorizontalLine (juce::roundToInt (cp.getY() + (cp.getHeight() - 1.0f) * (float) i / 4.0f), cp.getX(), cp.getRight());

    // The piano-key ruler.
    const float rulerH = juce::jlimit (5.0f, 14.0f, getHeight() * 0.03f);
    auto xHz = [this] (float hz) { return xForFreq (hz); };
    if (ruler == Ruler::Top)
        paintPianoRuler (g, juce::Rectangle<float> (p.getX(), bounds.getY(), p.getWidth(), rulerH), xHz);
    else if (ruler == Ruler::Bottom)
        paintPianoRuler (g, juce::Rectangle<float> (p.getX(), p.getBottom() + 1.0f, p.getWidth(), rulerH - 1.0f), xHz);

    if (showLabels())
    {
        g.setFont (wingFont (TypeScale::meterScale * 1.25f));
        g.setColour (labelColour);
        for (float f : { 20.0f, 40.0f, 60.0f, 100.0f, 200.0f, 300.0f, 400.0f, 600.0f, 1000.0f, 2000.0f, 3000.0f, 4000.0f, 6000.0f, 10000.0f, 20000.0f })
        {
            auto r = juce::Rectangle<float> (xForFreq (f) - 18.0f, bounds.getBottom() - 14.0f, 36.0f, 13.0f);
            r = r.withX (juce::jlimit (0.0f, (float) getWidth() - r.getWidth(), r.getX()));
            const auto just = f <= 20.0f ? juce::Justification::centredLeft : (f >= 20000.0f ? juce::Justification::centredRight : juce::Justification::centred);
            g.drawText (rulerText (f), r, just);
        }
    }

    if (strip == nullptr)
        return;

    auto& f = strip->params.filter;

    // The RTA: over the curve's lower part (or all of it), or in its own strip.
    const auto layout = rtaLayout();
    juce::Rectangle<float> rtaArea;
    if (rtaShown && layout.view >= 0)
    {
        rtaArea = layout.split ? p.withTop (cp.getBottom() + 1.0f) : p.withTrimmedTop (p.getHeight() * (1.0f - layout.fraction));
        if (layout.split)
        {
            g.setColour (gridColour);
            g.drawHorizontalLine (juce::roundToInt (cp.getBottom()), p.getX(), p.getRight());
        }
    }

    if (block == Filter)
    {
        if (! rtaArea.isEmpty())
            paintRta (g, rtaArea, layout);
        // The channel EQ behind, the filter in blue.
        g.saveState();
        g.reduceClipRegion (p.toNearestInt());
        const auto eqCurve = curveOf (eqResponse);
        g.setColour (eqCurveColour.withAlpha (strip->params.eq.on.load() ? 0.55f : 0.25f));
        g.strokePath (eqCurve, juce::PathStrokeType (1.5f));
        g.setColour (filterCurveColour);
        g.strokePath (curveOf (response), juce::PathStrokeType (juce::jlimit (2.0f, 3.0f, getHeight() / 90.0f)));
        g.restoreState();

        // LC, HC (and the all-pass AP): vertical lines labelled at the top,
        // an open ring handle near the bottom.
        const float radius = juce::jlimit (5.0f, 10.0f, getHeight() / 34.0f);
        auto line = [&] (float freq, juce::Colour colour, const juce::String& name)
        {
            const float x = xForFreq (freq);
            g.setColour (colour);
            g.fillRect (x - 1.0f, p.getY(), 2.0f, p.getHeight());
            g.setFont (wingFont (TypeScale::body));
            g.drawText (name, juce::Rectangle<float> (x - 18.0f, p.getY() - 17.0f, 36.0f, 15.0f), juce::Justification::centred);
            paintRing (g, { x, p.getBottom() - radius * 2.5f }, radius, colour, true);
        };
        line (f.lowCutFreq.load(), f.lowCut.load() ? juce::Colours::white : dimLabelColour, "LC");
        line (f.highCutFreq.load(), f.highCut.load() ? juce::Colours::white : dimLabelColour, "HC");
        const auto& tool = models::model (models::FilterTools, f.toolModel.load());
        if (tool.token.startsWith ("AP"))
            if (const int i = tool.find ("f"); i >= 0)
                line (f.toolParams[i].load(), f.tool.load() ? allPassColour : allPassColour.withMultipliedBrightness (0.45f), "AP");
        return;
    }

    // The EQ curve, its boosts and cuts shaded against the 0 dB line (a flat
    // EQ shows no fill, as on the console).
    {
        g.saveState();
        g.reduceClipRegion (p.toNearestInt());
        const auto curve = curveOf (response);
        const bool on = strip->params.eq.on.load();
        const float zero = yForDb (0.0f);
        juce::Path fill (curve);
        fill.lineTo (cp.getRight() + 2.0f, zero);
        fill.lineTo (cp.getX() - 2.0f, zero);
        fill.closeSubPath();
        g.setColour (on ? eqFillColour : juce::Colour (0xff1c1c1c));
        g.fillPath (fill);
        if (! rtaArea.isEmpty())
            paintRta (g, rtaArea, layout);
        g.setColour (on ? eqCurveColour : dimLabelColour);
        g.strokePath (curve, juce::PathStrokeType (juce::jlimit (1.5f, 3.0f, getHeight() / 120.0f)));
        g.restoreState();
    }

    // The bands: a line from the label at the top, an open ring at the gain.
    auto& values = strip->params.eq.values;
    const bool labels = showLabels();
    const float radius = juce::jlimit (3.5f, 10.0f, getHeight() / 34.0f);
    for (size_t i = 0; i < bands.size(); ++i)
    {
        const auto& band = bands[i];
        const auto c = juce::Point<float> (xForFreq (values[band.f].load()), yForDb (values[band.g].load()));
        const bool sel = (int) i == selectedBand;
        if (labels)
        {
            g.setColour (sel ? selectedBandColour : bandLineColour);
            g.fillRect (juce::Rectangle<float> (sel ? 2.0f : 1.5f, p.getBottom() - p.getY() - 2.0f).withPosition (c.x - 0.75f, p.getY() + 2.0f));
            g.setColour (sel ? juce::Colours::white : dimLabelColour);
            g.setFont (wingFont (TypeScale::body));
            g.drawText (band.id.toUpperCase(), juce::Rectangle<float> (24.0f, 16.0f).withCentre ({ c.x, p.getY() - 9.0f }), juce::Justification::centred);
        }
        paintRing (g, c, radius, juce::Colours::white, sel);
    }
}

int ResponseGraph::bandAt (juce::Point<float> pos) const
{
    if (strip == nullptr)
        return -1;
    auto& values = strip->params.eq.values;
    int best = -1;
    float bestDist = 14.0f;
    for (size_t i = 0; i < bands.size(); ++i)
    {
        const auto c = juce::Point<float> (xForFreq (values[bands[i].f].load()), yForDb (values[bands[i].g].load()));
        const float d = c.getDistanceFrom (pos);
        if (d < bestDist)
        {
            bestDist = d;
            best = (int) i;
        }
    }
    return best;
}

int ResponseGraph::cutLineAt (juce::Point<float> pos) const
{
    if (strip == nullptr || block != Filter)
        return -1;
    auto& f = strip->params.filter;
    const float lc = std::abs (xForFreq (f.lowCutFreq.load()) - pos.x);
    const float hc = std::abs (xForFreq (f.highCutFreq.load()) - pos.x);
    float ap = 1.0e9f;
    const auto& tool = models::model (models::FilterTools, f.toolModel.load());
    if (const int i = tool.token.startsWith ("AP") ? tool.find ("f") : -1; i >= 0)
        ap = std::abs (xForFreq (f.toolParams[i].load()) - pos.x);
    if (juce::jmin (lc, hc, ap) > 16.0f)
        return -1;
    return ap < juce::jmin (lc, hc) ? 2 : (lc <= hc ? 0 : 1);
}

void ResponseGraph::mouseDown (const juce::MouseEvent& e)
{
    dragLine = cutLineAt (e.position);
    if (block == Filter)
        return;
    dragBand = bandAt (e.position);
    if (dragBand >= 0)
    {
        selectedBand = dragBand;
        if (onBandSelected)
            onBandSelected (selectedBand);
        repaint();
    }
}

void ResponseGraph::mouseDrag (const juce::MouseEvent& e)
{
    if (strip == nullptr)
        return;

    if (dragLine >= 0)
    {
        auto& f = strip->params.filter;
        if (dragLine == 0)
            f.lowCutFreq = juce::jlimit (20.0f, 2000.0f, freqForX (e.position.x));
        else if (dragLine == 1)
            f.highCutFreq = juce::jlimit (200.0f, 20000.0f, freqForX (e.position.x));
        else
        {
            const auto& tool = models::model (models::FilterTools, f.toolModel.load());
            if (const int i = tool.find ("f"); i >= 0)
                f.toolParams[i] = juce::jlimit (tool.params[(size_t) i].min, tool.params[(size_t) i].max, freqForX (e.position.x));
        }
        repaint();
        return;
    }
    if (dragBand < 0)
        return;

    const auto& spec = models::model (static_cast<models::Table> (strip->getEqTable()), strip->params.eq.model.load());
    const auto& band = bands[(size_t) dragBand];
    auto& values = strip->params.eq.values;
    const auto& fp = spec.params[(size_t) band.f];
    const auto& gp = spec.params[(size_t) band.g];
    values[band.f] = juce::jlimit (fp.min, fp.max, freqForX (e.position.x));
    values[band.g] = juce::jlimit (gp.min, gp.max, std::round (dbForY (e.position.y) * 10.0f) / 10.0f);
    repaint();
}

void ResponseGraph::mouseUp (const juce::MouseEvent&)
{
    dragLine = -1;
    dragBand = -1;
}

void ResponseGraph::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (block == Filter)
        return;
    const int i = bandAt (e.position) >= 0 ? bandAt (e.position) : selectedBand;
    if (strip == nullptr || i < 0 || bands[(size_t) i].q < 0)
        return;

    const auto& spec = models::model (static_cast<models::Table> (strip->getEqTable()), strip->params.eq.model.load());
    const auto& qp = spec.params[(size_t) bands[(size_t) i].q];
    auto& q = strip->params.eq.values[bands[(size_t) i].q];
    q = qp.fromNormalised (juce::jlimit (0.0f, 1.0f, qp.toNormalised (q.load()) + wheel.deltaY * 0.15f));
}

} // namespace livedaw::ui
