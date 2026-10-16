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
    return getLocalBounds().toFloat().reduced (4.0f).withTrimmedLeft (22.0f).withTrimmedBottom (12.0f);
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
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 3.0f);

    g.setFont (juce::FontOptions (9.0f));
    for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float x = xForFreq (f);
        g.setColour (Colours::screenLine);
        g.drawVerticalLine (juce::roundToInt (x), p.getY(), p.getBottom());
        g.setColour (Colours::textDim);
        g.drawText (formatHz (f), juce::Rectangle<float> (x - 20.0f, p.getBottom(), 40.0f, 12.0f), juce::Justification::centred);
    }
    for (float db : { -12.0f, -6.0f, 0.0f, 6.0f, 12.0f })
    {
        const float y = yForDb (db);
        g.setColour (db == 0.0f ? Colours::outline : Colours::screenLine);
        g.drawHorizontalLine (juce::roundToInt (y), p.getX(), p.getRight());
        g.setColour (Colours::textDim);
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (getX() + 2.0f, y - 6.0f, 20.0f, 12.0f).withX (4.0f),
                    juce::Justification::centredRight);
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
    juce::Path fill (curve);
    fill.lineTo (p.getRight(), yForDb (0.0f));
    fill.lineTo (p.getX(), yForDb (0.0f));
    fill.closeSubPath();
    g.setColour ((on ? Colours::block : Colours::textDim).withAlpha (0.15f));
    g.fillPath (fill);
    g.setColour (on ? Colours::block : Colours::textDim);
    g.strokePath (curve, juce::PathStrokeType (2.0f));

    auto& values = strip->params.eq.values;
    for (size_t i = 0; i < bands.size(); ++i)
    {
        const auto& band = bands[i];
        const auto centre = juce::Point<float> (xForFreq (values[band.f].load()), yForDb (values[band.g].load()));
        const bool sel = (int) i == selectedBand;
        g.setColour (sel ? Colours::accent : Colours::text.withAlpha (0.85f));
        g.fillEllipse (juce::Rectangle<float> (sel ? 16.0f : 13.0f, sel ? 16.0f : 13.0f).withCentre (centre));
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
        g.drawText (band.id.toUpperCase(), juce::Rectangle<float> (16.0f, 16.0f).withCentre (centre), juce::Justification::centred);
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
