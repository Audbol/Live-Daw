#include "ui/ResponseGraph.h"
#include "engine/Models.h"
#include "ui/Knobs.h"

namespace livedaw::ui
{

ResponseGraph::ResponseGraph()
{
    const int n = 240;
    for (int i = 0; i < n; ++i)
        freqs.push_back (20.0 * std::pow (1000.0, (double) i / (n - 1)));
    response.assign (freqs.size(), 0.0);
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
    if (next != response)
    {
        response = std::move (next);
        repaint();
    }
}

juce::Rectangle<float> ResponseGraph::plot() const
{
    // Labels only when there is room for them (the full EQ screen); the overview
    // panel shows the bare grid like the console. The keyboard runs along the bottom.
    auto r = getLocalBounds().toFloat().reduced (2.0f).withTrimmedBottom (juce::jlimit (8.0f, 14.0f, getHeight() * 0.05f));
    if (showLabels())
        r = r.withTrimmedLeft (24.0f).withTrimmedBottom (14.0f);
    return r;
}

bool ResponseGraph::showLabels() const
{
    return getHeight() > 260;
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

void ResponseGraph::paint (juce::Graphics& g)
{
    const auto p = plot();
    g.setColour (juce::Colour (0xff3b3b3b));
    g.fillRect (getLocalBounds());
    g.setColour (juce::Colour (0xff5c5c5c));
    g.drawRect (p.expanded (1.0f), 1.0f);

    // Grid: every 10, 100, 1k ... like the console's EQ view, and eight level divisions.
    const auto gridColour = juce::Colour (0xff9a9a9a).withAlpha (0.55f);
    g.setColour (gridColour);
    for (float decade : { 10.0f, 100.0f, 1000.0f, 10000.0f })
        for (int m = 1; m <= 9; ++m)
        {
            const float f = decade * (float) m;
            if (f < 20.0f || f > 20000.0f)
                continue;
            g.drawVerticalLine (juce::roundToInt (xForFreq (f)), p.getY(), p.getBottom());
        }
    for (int i = 1; i < 8; ++i)
        g.drawHorizontalLine (juce::roundToInt (p.getY() + p.getHeight() * (float) i / 8.0f), p.getX(), p.getRight());

    if (showLabels())
    {
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.setColour (Colours::textDim);
        for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
            g.drawText (formatHz (f), juce::Rectangle<float> (xForFreq (f) - 24.0f, getHeight() - 15.0f, 48.0f, 13.0f), juce::Justification::centred);
        for (int i = -2; i <= 2; ++i)
        {
            const float db = kRangeDb * (float) i / 2.0f;
            g.drawText (juce::String (juce::roundToInt (db)), juce::Rectangle<float> (2.0f, yForDb (db) - 7.0f, 20.0f, 14.0f),
                        juce::Justification::centredRight);
        }
    }

    // The keyboard strip along the bottom, one key per semitone from 20 Hz to 20 kHz.
    {
        auto kb = juce::Rectangle<float> (p.getX(), p.getBottom() + 2.0f, p.getWidth(),
                                          getLocalBounds().toFloat().reduced (2.0f).getBottom() - p.getBottom() - 2.0f - (showLabels() ? 14.0f : 0.0f));
        g.setColour (juce::Colour (0xffb4b4b4));
        g.fillRect (kb);
        for (int note = 16; note <= 135; ++note)
        {
            const float f0 = 440.0f * std::pow (2.0f, ((float) note - 0.5f - 69.0f) / 12.0f);
            const float f1 = 440.0f * std::pow (2.0f, ((float) note + 0.5f - 69.0f) / 12.0f);
            const float x0 = juce::jmax (kb.getX(), xForFreq (f0)), x1 = juce::jmin (kb.getRight(), xForFreq (f1));
            if (x1 <= x0)
                continue;
            const int pc = note % 12;
            if (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10)
            {
                g.setColour (juce::Colour (0xff3b3b3b));
                g.fillRect (juce::Rectangle<float> (x0, kb.getY(), x1 - x0, kb.getHeight() * 0.55f));
            }
            else if (pc == 0 || pc == 5)
            {
                g.setColour (juce::Colour (0xff3b3b3b));
                g.drawVerticalLine (juce::roundToInt (x0), kb.getY(), kb.getBottom());
            }
        }
    }

    if (strip == nullptr)
        return;

    juce::Path curve;
    for (size_t i = 0; i < freqs.size(); ++i)
    {
        const auto pt = juce::Point<float> (xForFreq ((float) freqs[i]), yForDb ((float) response[i]));
        if (i == 0)
            curve.startNewSubPath (pt);
        else
            curve.lineTo (pt);
    }

    const bool on = block == Eq ? strip->params.eq.on.load() : true;
    const auto curveColour = on ? juce::Colour (0xfff5d36a) : Colours::textDim;
    juce::Path fill (curve);
    fill.lineTo (p.getRight(), yForDb (0.0f));
    fill.lineTo (p.getX(), yForDb (0.0f));
    fill.closeSubPath();
    g.saveState();
    g.reduceClipRegion (p.toNearestInt());
    g.setColour (curveColour.withAlpha (on ? 0.42f : 0.18f));
    g.fillPath (fill);
    g.setColour (curveColour);
    g.strokePath (curve, juce::PathStrokeType (juce::jlimit (2.0f, 3.5f, getHeight() / 70.0f)));
    g.restoreState();

    // Band handles: crosshairs, the selected one in cyan.
    auto& values = strip->params.eq.values;
    const float arm = juce::jlimit (6.0f, 11.0f, getHeight() / 22.0f);
    for (size_t i = 0; i < bands.size(); ++i)
    {
        const auto& band = bands[i];
        const auto c = juce::Point<float> (xForFreq (values[band.f].load()), yForDb (values[band.g].load()));
        const bool sel = (int) i == selectedBand;
        g.setColour (sel ? Colours::cyan : juce::Colours::white);
        const float t = sel ? 3.0f : 2.5f;
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

void ResponseGraph::mouseDown (const juce::MouseEvent& e)
{
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
    if (strip == nullptr || dragBand < 0)
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

void ResponseGraph::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const int i = bandAt (e.position) >= 0 ? bandAt (e.position) : selectedBand;
    if (strip == nullptr || i < 0 || bands[(size_t) i].q < 0)
        return;

    const auto& spec = models::model (static_cast<models::Table> (strip->getEqTable()), strip->params.eq.model.load());
    const auto& qp = spec.params[(size_t) bands[(size_t) i].q];
    auto& q = strip->params.eq.values[bands[(size_t) i].q];
    q = qp.fromNormalised (juce::jlimit (0.0f, 1.0f, qp.toNormalised (q.load()) + wheel.deltaY * 0.15f));
}

} // namespace livedaw::ui
