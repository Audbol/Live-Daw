#include "ui/ResponseGraph.h"
#include "engine/Models.h"
#include "ui/Knobs.h"

namespace livedaw::ui
{

namespace
{
    const juce::Colour eqCurveColour { 0xffe8a33a };
    const juce::Colour filterCurveColour { 0xff4aa3e0 };
    const juce::Colour selectedBandColour { 0xff4aa3e0 };

    /** "50", "100", "1k", "10k" */
    juce::String rulerText (float f)
    {
        return f >= 1000.0f ? juce::String (juce::roundToInt (f / 1000.0f)) + "k" : juce::String (juce::roundToInt (f));
    }
} // namespace

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
    // panel shows the bare grid like the console. The ruler runs along the bottom.
    auto r = getLocalBounds().toFloat().reduced (1.0f).withTrimmedBottom (juce::jlimit (5.0f, 9.0f, getHeight() * 0.035f));
    if (showLabels())
        r = r.withTrimmedLeft (24.0f).withTrimmedBottom (14.0f);
    if (block == Filter)
        r = r.withTrimmedTop (16.0f);
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
    const auto p = plot();
    return p.getCentreY() - juce::jlimit (-kRangeDb, kRangeDb, db) / kRangeDb * p.getHeight() * 0.5f;
}

float ResponseGraph::dbForY (float y) const
{
    const auto p = plot();
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
    g.fillAll (juce::Colours::black);

    // Grid: a line for every 10, 20 ... 90 Hz, 100, 200 ... (brighter at the
    // decades) and eight level divisions.
    for (float decade : { 10.0f, 100.0f, 1000.0f, 10000.0f })
        for (int m = 1; m <= 9; ++m)
        {
            const float f = decade * (float) m;
            if (f < 20.0f || f > 20000.0f)
                continue;
            g.setColour (m == 1 || m == 2 || m == 5 ? juce::Colour (0xff4a4a4a) : juce::Colour (0xff2c2c2c));
            g.drawVerticalLine (juce::roundToInt (xForFreq (f)), p.getY(), p.getBottom());
        }
    for (int i = 1; i < 8; ++i)
    {
        g.setColour (i == 4 ? juce::Colour (0xff5a5a5a) : juce::Colour (0xff2c2c2c));
        g.drawHorizontalLine (juce::roundToInt (p.getY() + p.getHeight() * (float) i / 8.0f), p.getX(), p.getRight());
    }

    if (showLabels())
    {
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.setColour (Colours::textDim);
        for (float f : { 20.0f, 50.0f, 100.0f, 200.0f, 300.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
            g.drawText (rulerText (f), juce::Rectangle<float> (xForFreq (f) - 18.0f, (float) getHeight() - 14.0f, 36.0f, 13.0f), juce::Justification::centred);
        if (block == Eq)
            for (int i = -2; i <= 2; ++i)
            {
                const float db = kRangeDb * (float) i / 2.0f;
                g.drawText (juce::String (juce::roundToInt (db)), juce::Rectangle<float> (0.0f, yForDb (db) - 7.0f, 20.0f, 14.0f),
                            juce::Justification::centredRight);
            }
    }

    // The frequency ruler along the bottom: a tick per semitone, taller at C.
    {
        const float top = p.getBottom() + 1.0f;
        const float h = getLocalBounds().toFloat().reduced (1.0f).getBottom() - top - (showLabels() ? 14.0f : 0.0f);
        for (int note = 16; note <= 135; ++note)
        {
            const float x = xForFreq (440.0f * std::pow (2.0f, ((float) note - 69.0f) / 12.0f));
            if (x < p.getX() || x > p.getRight())
                continue;
            const bool c = note % 12 == 0;
            g.setColour (c ? juce::Colour (0xff8a8a8a) : juce::Colour (0xff505050));
            g.fillRect (x, top, 1.0f, c ? h : h * 0.55f);
        }
    }

    if (strip == nullptr)
        return;

    g.saveState();
    g.reduceClipRegion (p.toNearestInt());
    auto& f = strip->params.filter;

    if (block == Filter)
    {
        // The channel EQ behind, the filter in blue.
        const auto eqCurve = curveOf (eqResponse);
        g.setColour (eqCurveColour.withAlpha (strip->params.eq.on.load() ? 0.55f : 0.25f));
        g.strokePath (eqCurve, juce::PathStrokeType (1.5f));
        g.setColour (filterCurveColour);
        g.strokePath (curveOf (response), juce::PathStrokeType (juce::jlimit (2.0f, 3.0f, getHeight() / 90.0f)));
    }
    else
    {
        const auto curve = curveOf (response);
        const bool on = strip->params.eq.on.load();
        const auto colour = on ? eqCurveColour : Colours::textDim;
        juce::Path fill (curve);
        fill.lineTo (p.getRight(), yForDb (0.0f));
        fill.lineTo (p.getX(), yForDb (0.0f));
        fill.closeSubPath();
        g.setColour (colour.withAlpha (on ? 0.35f : 0.15f));
        g.fillPath (fill);
        g.setColour (colour);
        g.strokePath (curve, juce::PathStrokeType (juce::jlimit (1.5f, 3.0f, getHeight() / 80.0f)));
    }
    g.restoreState();

    if (block == Filter)
    {
        // LC and HC: vertical lines labelled at the top, a round handle at the bottom.
        const float handle = juce::jlimit (7.0f, 11.0f, getHeight() / 30.0f);
        auto line = [&] (float freq, bool on, const char* name)
        {
            const float x = xForFreq (freq);
            const auto c = on ? juce::Colours::white : juce::Colour (0xff6a6a6a);
            g.setColour (c);
            g.fillRect (x - 1.0f, p.getY(), 2.0f, p.getHeight());
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.drawText (name, juce::Rectangle<float> (x - 16.0f, p.getY() - 16.0f, 32.0f, 15.0f), juce::Justification::centred);
            g.fillEllipse (x - handle, p.getBottom() - handle * 2.0f - 2.0f, handle * 2.0f, handle * 2.0f);
            g.setColour (juce::Colours::black);
            g.drawEllipse (x - handle * 0.5f, p.getBottom() - handle * 1.5f - 2.0f, handle, handle, 1.5f);
        };
        line (f.lowCutFreq.load(), f.lowCut.load(), "LC");
        line (f.highCutFreq.load(), f.highCut.load(), "HC");
        return;
    }

    // Band handles: white "+" crosses, the selected band in blue.
    auto& values = strip->params.eq.values;
    const float arm = juce::jlimit (4.0f, 10.0f, getHeight() / 24.0f);
    for (size_t i = 0; i < bands.size(); ++i)
    {
        const auto& band = bands[i];
        const auto c = juce::Point<float> (xForFreq (values[band.f].load()), yForDb (values[band.g].load()));
        const bool sel = (int) i == selectedBand;
        g.setColour (sel ? selectedBandColour : juce::Colours::white);
        const float t = sel ? 3.0f : 2.0f;
        g.fillRect (juce::Rectangle<float> (arm * 2.0f, t).withCentre (c));
        g.fillRect (juce::Rectangle<float> (t, arm * 2.0f).withCentre (c));
        if (showLabels())
        {
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.drawText (band.id.toUpperCase(), juce::Rectangle<float> (24.0f, 14.0f).withCentre (c.translated (arm + 8.0f, -arm - 2.0f)),
                        juce::Justification::centred);
        }
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
    if (juce::jmin (lc, hc) > 16.0f)
        return -1;
    return lc <= hc ? 0 : 1;
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
        else
            f.highCutFreq = juce::jlimit (200.0f, 20000.0f, freqForX (e.position.x));
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
