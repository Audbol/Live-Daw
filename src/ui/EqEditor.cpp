#include "ui/EqEditor.h"
#include "ui/LookAndFeel.h"

namespace livedaw::ui
{

namespace
{
    const juce::Colour bandColours[] = { juce::Colour (0xffe8743a), juce::Colour (0xffe8d23a),
                                         juce::Colour (0xff45c46b), juce::Colour (0xff4fa3ff) };
} // namespace

EqEditor::EqEditor (EqParams& p, std::function<double()> sr)
    : params (p), getSampleRate (std::move (sr))
{
    startTimerHz (20);
}

void EqEditor::setCompact (bool shouldBeCompact)
{
    compact = shouldBeCompact;
    repaint();
}

void EqEditor::timerCallback()
{
    if (! (snapshotEq (params) == lastDrawn))
        repaint();
}

float EqEditor::freqToX (float freq) const
{
    return static_cast<float> (getWidth()) * std::log (freq / kMinFreq) / std::log (kMaxFreq / kMinFreq);
}

float EqEditor::xToFreq (float x) const
{
    return kMinFreq * std::pow (kMaxFreq / kMinFreq, juce::jlimit (0.0f, 1.0f, x / static_cast<float> (getWidth())));
}

float EqEditor::dbToY (float db) const
{
    return juce::jmap (db, kRangeDb, -kRangeDb, 4.0f, static_cast<float> (getHeight()) - 4.0f);
}

float EqEditor::yToDb (float y) const
{
    return juce::jlimit (-kRangeDb, kRangeDb, juce::jmap (y, 4.0f, static_cast<float> (getHeight()) - 4.0f, kRangeDb, -kRangeDb));
}

juce::Point<float> EqEditor::handlePosition (int band) const
{
    const auto& b = params.bands[(size_t) band];
    const bool cut = isCut (static_cast<EqBandType> (b.type.load()));
    return { freqToX (b.frequency.load()), dbToY (cut ? 0.0f : b.gainDb.load()) };
}

int EqEditor::hitTestBand (juce::Point<float> pos) const
{
    const float radius = compact ? 7.0f : 10.0f;
    for (int b = kNumEqBands; --b >= 0;)
        if (handlePosition (b).getDistanceFrom (pos) < radius)
            return b;

    return -1;
}

void EqEditor::paint (juce::Graphics& g)
{
    lastDrawn = snapshotEq (params);
    const double sr = juce::jmax (8000.0, getSampleRate());
    auto bounds = getLocalBounds().toFloat();

    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillRoundedRectangle (bounds, 4.0f);

    // Grid
    g.setFont (juce::FontOptions (9.0f));
    for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float x = freqToX (f);
        g.setColour (Colours::outline.withAlpha (compact ? 0.3f : 0.6f));
        g.drawVerticalLine (juce::roundToInt (x), bounds.getY(), bounds.getBottom());
        if (compact)
            continue;
        g.setColour (Colours::textDim);
        g.drawText (f >= 1000.0f ? juce::String (f / 1000.0f, 0) + "k" : juce::String (f, 0),
                    juce::Rectangle<float> (x + 2.0f, bounds.getBottom() - 12.0f, 30.0f, 10.0f),
                    juce::Justification::left);
    }

    for (float db : { -12.0f, -6.0f, 0.0f, 6.0f, 12.0f })
    {
        if (compact && db != 0.0f)
            continue;

        g.setColour (Colours::outline.withAlpha (db == 0.0f ? 1.0f : 0.5f));
        g.drawHorizontalLine (juce::roundToInt (dbToY (db)), bounds.getX(), bounds.getRight());
    }

    // Response curve
    juce::Path curve;
    const int w = getWidth();
    for (int x = 0; x <= w; x += 2)
    {
        const auto db = static_cast<float> (eqResponseDb (lastDrawn, xToFreq (static_cast<float> (x)), sr));
        const float y = dbToY (juce::jlimit (-kRangeDb - 6.0f, kRangeDb + 6.0f, db));
        if (x == 0)
            curve.startNewSubPath (0.0f, y);
        else
            curve.lineTo (static_cast<float> (x), y);
    }

    juce::Path fill (curve);
    fill.lineTo (static_cast<float> (w), dbToY (0.0f));
    fill.lineTo (0.0f, dbToY (0.0f));
    fill.closeSubPath();

    const auto curveColour = lastDrawn.enabled != 0 ? Colours::accent : Colours::textDim;
    g.setColour (curveColour.withAlpha (0.15f));
    g.fillPath (fill);
    g.setColour (curveColour);
    g.strokePath (curve, juce::PathStrokeType (compact ? 1.5f : 2.0f));

    // Band handles
    for (int b = 0; b < kNumEqBands; ++b)
    {
        const auto pos = handlePosition (b);
        const bool on = lastDrawn.bands[b].enabled != 0 && lastDrawn.enabled != 0;
        g.setColour (bandColours[b].withAlpha (on ? 1.0f : 0.35f));

        if (compact)
        {
            g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (pos));
            continue;
        }

        g.fillEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre (pos));
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
        g.drawText (juce::String (b + 1), juce::Rectangle<float> (12.0f, 12.0f).withCentre (pos), juce::Justification::centred);
    }
}

void EqEditor::mouseDown (const juce::MouseEvent& e)
{
    dragBand = hitTestBand (e.position);

    if (dragBand < 0 && onBackgroundClick)
        onBackgroundClick();
}

void EqEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (dragBand < 0)
        return;

    auto& b = params.bands[(size_t) dragBand];
    b.frequency = juce::jlimit (kMinFreq, kMaxFreq, xToFreq (e.position.x));

    if (! isCut (static_cast<EqBandType> (b.type.load())))
        b.gainDb = std::round (yToDb (e.position.y) * 10.0f) / 10.0f;

    repaint();
}

void EqEditor::mouseUp (const juce::MouseEvent&)
{
    dragBand = -1;
}

void EqEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const int band = hitTestBand (e.position);
    if (band < 0)
        return;

    auto& q = params.bands[(size_t) band].q;
    q = juce::jlimit (0.1f, 16.0f, q.load() * std::pow (1.15f, wheel.deltaY * 4.0f));
    repaint();
}

void EqEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Double-click a handle to flatten that band.
    const int band = hitTestBand (e.position);
    if (band >= 0)
        params.bands[(size_t) band].gainDb = 0.0f;
}

} // namespace livedaw::ui
