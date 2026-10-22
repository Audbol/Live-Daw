#include "ui/DynamicsGraph.h"

#include "ui/LookAndFeel.h"

#include <cmath>

namespace livedaw::ui
{

namespace
{
    // Firmware 3.x (manual Fig. 92-94, the model list): a #202020 plot with
    // a dot grid, a grey threshold line with a white handle, the curve orange.
    const juce::Colour plotBg { 0xff202020 };
    const juce::Colour gridDot { 0xff3c3c3c };
    const juce::Colour axisText { 0xffb8b8b8 };
    const juce::Colour thresholdLine { 0xff484848 };
    const juce::Colour curveOn { 0xfff09a28 };
    const juce::Colour curveOff { 0xff707070 };
    const juce::Colour levelDot { 0xffffe23a };
    const juce::Colour grLit { 0xffe8302a };
    const juce::Colour segmentOff { 0xff101010 };
    const juce::Colour tick { 0xff484848 };
    // The COMP and GATE screens of a console: one-pixel #c0c0c0 dots, the
    // curve #707070 over a #484848 threshold line while off, blue over #707070
    // while on; the ENVELOPE green (gate, ducker) or grey (compressor,
    // expander), on or off, between #707070 guide lines.
    const juce::Colour screenDot { 0xffc0c0c0 };
    const juce::Colour curveScreenOn { 0xff4890e0 };
    const juce::Colour thresholdLineOn { 0xff707070 };
    const juce::Colour envLineOn { 0xff18e460 };
    const juce::Colour envFillOn { 0xff185030 };
    const juce::Colour envLineOff { 0xff484848 };
    const juce::Colour envFillOff { 0xff282828 };
    const juce::Colour envGuide { 0xff707070 };

    const juce::String& tokenOf (int model) { return models::model (models::Dynamics, model).token; }

    /** The gate and the ducker draw their ENVELOPE green whether the slot is on or not [3.1]. */
    bool greenEnvelope (int model)
    {
        const auto& t = tokenOf (model);
        return t == "GATE" || t == "DUCK";
    }

    /** Largest reduction the graph's GR meter shows. */
    float grMeterRange (int model, float range)
    {
        const auto& t = tokenOf (model);
        return t == "GATE" || t == "DUCK" ? range - 20.0f : 20.0f;
    }

    void paintDots (juce::Graphics& g, juce::Rectangle<float> r, float stepX, float stepY, float d, juce::Colour colour = gridDot)
    {
        g.setColour (colour);
        for (float y = r.getY(); y <= r.getBottom() + 0.5f; y += stepY)
            for (float x = r.getX(); x <= r.getRight() + 0.5f; x += stepX)
                g.fillRect (x - d * 0.5f, y - d * 0.5f, d, d);
    }

    /** The round threshold handle: white with three dark bars. */
    void paintHandle (juce::Graphics& g, juce::Rectangle<float> r)
    {
        g.setColour (juce::Colours::white);
        g.fillEllipse (r);
        g.setColour (juce::Colour (0xff202020));
        const float w = r.getWidth() * 0.42f, h = juce::jmax (1.0f, r.getHeight() * 0.06f);
        for (int i = -1; i <= 1; ++i)
            g.fillRect (juce::Rectangle<float> (w, h).withCentre (r.getCentre().translated (0.0f, (float) i * r.getHeight() * 0.14f)));
    }

    float valueOf (const DynSlotParams& slot, int index) { return index >= 0 ? slot.values[index].load() : 0.0f; }
} // namespace

bool hasTransferGraph (int model)
{
    const auto& t = tokenOf (model);
    return t == "GATE" || t == "DUCK" || t == "COMP" || t == "EXP";
}

float transferRangeDb (int model)
{
    const auto& t = tokenOf (model);
    return t == "GATE" || t == "DUCK" ? 80.0f : 60.0f;
}

std::vector<float> transferCurve (int model, const float* values, int count)
{
    const float range = transferRangeDb (model);
    std::vector<float> in ((size_t) count), out ((size_t) count);
    for (int i = 0; i < count; ++i)
        in[(size_t) i] = -range + range * (float) i / (float) juce::jmax (1, count - 1);
    if (! models::dynStaticCurve (model, values, ld::kMaxDynParams, in.data(), out.data(), count))
        return {};
    return out;
}

std::vector<float> slotValues (const DynSlotParams& slot)
{
    std::vector<float> v (ld::kMaxDynParams);
    for (int i = 0; i < ld::kMaxDynParams; ++i)
        v[(size_t) i] = slot.values[i].load();
    return v;
}

void paintTransferThumb (juce::Graphics& g, juce::Rectangle<float> r, int model, const float* values, bool on)
{
    g.setColour (juce::Colours::black);
    g.fillRect (r);
    const float range = transferRangeDb (model);
    auto plot = r.reduced (juce::jmax (2.0f, r.getHeight() * 0.08f));
    plot = plot.withSizeKeepingCentre (juce::jmin (plot.getWidth(), plot.getHeight() * 1.2f), plot.getHeight());
    const auto curve = transferCurve (model, values, 81);
    auto pointAt = [&] (float in, float out)
    {
        return juce::Point<float> (plot.getX() + plot.getWidth() * (in + range) / range,
                                   plot.getBottom() - plot.getHeight() * juce::jlimit (0.0f, 1.0f, (out + range) / range));
    };
    const auto& spec = models::model (models::Dynamics, model);
    const int ti = spec.find ("thr");
    const float thr = ti >= 0 ? values[ti] : -20.0f;
    const auto thrPoint = pointAt (thr, thr);
    g.setColour (thresholdLine);
    g.drawHorizontalLine (juce::roundToInt (thrPoint.y), plot.getX(), plot.getRight());
    if (! curve.empty())
    {
        juce::Path p;
        for (size_t i = 0; i < curve.size(); ++i)
        {
            const float in = -range + range * (float) i / (float) (curve.size() - 1);
            const auto pt = pointAt (in, curve[i]);
            if (i == 0)
                p.startNewSubPath (pt);
            else
                p.lineTo (pt);
        }
        g.saveState();
        g.reduceClipRegion (plot.expanded (1.0f).toNearestInt());
        g.setColour (on ? curveOn : curveOff);
        g.strokePath (p, juce::PathStrokeType (juce::jmax (1.2f, r.getHeight() * 0.03f)));
        g.restoreState();
    }
    const float d = juce::jmax (4.0f, r.getHeight() * 0.11f);
    g.setColour (juce::Colours::white);
    g.fillEllipse (juce::Rectangle<float> (d, d).withCentre ({ plot.getRight(), thrPoint.y }));
}

void paintDynamicsTile (juce::Graphics& g, juce::Rectangle<float> r, int model, const float* values, bool on)
{
    g.setColour (juce::Colours::black);
    g.fillRect (r);
    auto left = r.removeFromLeft (r.getWidth() * 0.48f);
    paintTransferThumb (g, left, model, values, on);
    r.removeFromLeft (2.0f);
    const auto& spec = models::model (models::Dynamics, model);
    auto norm = [&] (const char* id)
    {
        const int i = spec.find (id);
        if (i < 0)
            return 0.2f;
        const auto& p = spec.params[(size_t) i];
        return p.toNormalised (values[i]);
    };
    auto title = r.removeFromTop (r.getHeight() * 0.2f);
    g.setColour (axisText);
    g.setFont (wingFontPx (juce::jmax (4.0f, title.getHeight() * 0.5f)));
    g.drawText ("ENVELOPE", title, juce::Justification::centred, false);
    DynEnvelopeGraph::paintShape (g, r.reduced (1.0f, 0.0f), norm ("att"), norm ("hld"), norm ("rel"), greenEnvelope (model), false);
}

//==============================================================================
DynTransferGraph::DynTransferGraph (DynSlotParams& s, std::function<float()> grFn, std::function<float()> keyFn, std::function<float()> outFn)
    : slot (s), readGr (std::move (grFn)), readKey (std::move (keyFn)), readOut (std::move (outFn))
{
    setMouseCursor (juce::MouseCursor::NormalCursor);
    rebuild();
    startTimerHz (30);
}

void DynTransferGraph::rebuild()
{
    builtModel = slot.model.load();
    builtValues = slotValues (slot);
    builtOn = slot.on.load();
    range = transferRangeDb (builtModel);
    curve = transferCurve (builtModel, builtValues.data(), juce::roundToInt (range) * 4 + 1);
    repaint();
}

void DynTransferGraph::timerCallback()
{
    if (slot.model.load() != builtModel || slot.on.load() != builtOn || slotValues (slot) != builtValues)
        rebuild();
    const float newGr = slot.on.load() && readGr ? juce::jmax (0.0f, readGr()) : 0.0f;
    const float newKey = slot.on.load() && readKey ? readKey() : -120.0f;
    const float peak = readOut ? readOut() : 0.0f;
    // Meters: fast up, slower down (as the console's).
    gr = newGr > gr ? newGr : gr * 0.8f + newGr * 0.2f;
    key = newKey > key ? newKey : key * 0.6f + newKey * 0.4f;
    const float outDb = peak > 0.0f ? 20.0f * std::log10 (peak) : -120.0f;
    out = outDb > out ? outDb : juce::jmax (outDb, out - 1.5f);
    // Only when the meters moved: the graph is big and a pop-up may lie over it.
    if (std::abs (gr - shownGr) > 0.05f || std::abs (key - shownKey) > 0.1f || std::abs (out - shownOut) > 0.1f)
    {
        shownGr = gr;
        shownKey = key;
        shownOut = out;
        repaint();
    }
}

juce::Rectangle<float> DynTransferGraph::plotArea() const
{
    auto r = getLocalBounds().toFloat();
    const float top = juce::jmax (8.0f, r.getHeight() * 0.04f);
    const float h = r.getHeight() - top * 2.0f;
    const float left = r.getWidth() * 0.095f;
    const float w = juce::jmin (h, r.getWidth() - left - r.getWidth() * 0.13f);
    return { r.getX() + left, r.getY() + top, w, h };
}

float DynTransferGraph::yFor (float db) const
{
    const auto p = plotArea();
    return p.getBottom() - p.getHeight() * (db + range) / range;
}

float DynTransferGraph::xFor (float db) const
{
    const auto p = plotArea();
    return p.getX() + p.getWidth() * (db + range) / range;
}

float DynTransferGraph::dbForY (float y) const
{
    const auto p = plotArea();
    return (p.getBottom() - y) / juce::jmax (1.0f, p.getHeight()) * range - range;
}

int DynTransferGraph::thresholdIndex() const { return models::model (models::Dynamics, builtModel).find ("thr"); }

juce::Rectangle<float> DynTransferGraph::handleArea() const
{
    const int ti = thresholdIndex();
    const float thr = ti >= 0 ? slot.values[ti].load() : 0.0f;
    const float d = juce::jlimit (14.0f, 40.0f, getHeight() * 0.075f);
    return juce::Rectangle<float> (d, d).withCentre ({ (float) getWidth() - d * 0.5f - 2.0f, yFor (thr) });
}

void DynTransferGraph::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (plotBg);
    g.fillRect (bounds);
    const auto plot = plotArea();
    const float pxPerDb = plot.getHeight() / range;
    paintDots (g, plot, pxPerDb * 5.0f, pxPerDb * 5.0f, juce::jmax (1.0f, pxPerDb * 0.135f), screenDot);

    // The levels down the left, the ticks at the right (lit up to the output level).
    // The column fits "-60" (small windows shrink the labels rather than cut them).
    const float labelW = plot.getX() - bounds.getX() - bounds.getWidth() * 0.03f;
    g.setFont (fontFittingWidth (wingFontPx (juce::jlimit (6.0f, 11.0f, pxPerDb * 1.6f)), "-60", labelW - 3.0f, 1.0f));
    const float tickX = plot.getRight() + bounds.getWidth() * 0.03f;
    const float tickW = juce::jmax (4.0f, bounds.getWidth() * 0.018f);
    for (int db = 0; db >= -(int) range; db -= 10)
    {
        const float y = yFor ((float) db);
        g.setColour (axisText);
        g.drawText (juce::String (db), juce::Rectangle<float> (bounds.getX() + 3.0f, y - 8.0f, labelW, 16.0f),
                    juce::Justification::centredLeft, false);
        g.setColour (out >= (float) db && slot.on.load() ? juce::Colour (0xff35d05a) : tick);
        g.fillRect (tickX, y - 1.5f, tickW, juce::jmax (2.0f, pxPerDb * 0.4f));
    }

    // The gain reduction meter along the left edge of the plot.
    {
        const float meterRange = grMeterRange (builtModel, range);
        const float x = plot.getX() - bounds.getWidth() * 0.018f;
        const float w = juce::jmax (3.0f, bounds.getWidth() * 0.016f);
        const float top = yFor (0.0f), bottom = yFor (-meterRange);
        const float pitch = juce::jmax (2.5f, pxPerDb * 0.5f);
        const float lit = top + (bottom - top) * juce::jlimit (0.0f, 1.0f, gr / meterRange);
        for (float y = top; y + pitch * 0.5f <= bottom; y += pitch)
        {
            g.setColour (y + pitch * 0.5f <= lit ? grLit : segmentOff);
            g.fillRect (x, y, w, pitch * 0.6f);
        }
    }

    const int ti = thresholdIndex();
    const float thr = ti >= 0 ? slot.values[ti].load() : 0.0f;
    const auto handle = handleArea();
    g.setColour (slot.on.load() ? thresholdLineOn : thresholdLine);
    g.fillRect (juce::Rectangle<float> (plot.getX(), yFor (thr) - 1.0f, handle.getX() - plot.getX(), 2.0f));

    if (! curve.empty())
    {
        juce::Path p;
        for (size_t i = 0; i < curve.size(); ++i)
        {
            const float in = -range + range * (float) i / (float) (curve.size() - 1);
            const juce::Point<float> pt (xFor (in), yFor (juce::jmax (curve[i], -range - 2.0f)));
            if (i == 0)
                p.startNewSubPath (pt);
            else
                p.lineTo (pt);
        }
        g.saveState();
        g.reduceClipRegion (plot.toNearestInt());
        g.setColour (slot.on.load() ? curveScreenOn : curveOff);
        g.strokePath (p, juce::PathStrokeType (juce::jmax (1.5f, pxPerDb * 0.35f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        // The live level on the curve.
        if (slot.on.load() && key > -range)
        {
            const float pos = juce::jlimit (0.0f, 1.0f, (key + range) / range) * (float) (curve.size() - 1);
            const auto i0 = (size_t) pos;
            const auto i1 = juce::jmin (curve.size() - 1, i0 + 1);
            const float y = curve[i0] + (curve[i1] - curve[i0]) * (pos - (float) i0);
            const float d = juce::jmax (5.0f, pxPerDb * 1.6f);
            g.setColour (levelDot);
            g.fillEllipse (juce::Rectangle<float> (d, d).withCentre ({ xFor (juce::jmin (key, 0.0f)), yFor (y) }));
        }
        g.restoreState();
    }
    paintHandle (g, handle);
    if (dragging)
    {
        g.setColour (juce::Colours::white);
        g.setFont (wingFontPx (juce::jlimit (7.0f, 16.0f, pxPerDb * 2.6f)));
        g.drawText (juce::String (thr, 1) + " dB", juce::Rectangle<float> (plot.getX() + 6.0f, yFor (thr) - 22.0f, 120.0f, 18.0f),
                    juce::Justification::centredLeft, false);
    }
}

void DynTransferGraph::mouseDown (const juce::MouseEvent& e)
{
    const int ti = thresholdIndex();
    const float thr = ti >= 0 ? slot.values[ti].load() : 0.0f;
    dragging = ti >= 0 && (handleArea().expanded (6.0f).contains (e.position) || std::abs (e.position.y - yFor (thr)) < 8.0f);
    repaint();
}

void DynTransferGraph::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;
    const int ti = thresholdIndex();
    const auto& p = models::model (models::Dynamics, builtModel).params[(size_t) ti];
    const float lo = juce::jmin (p.min, p.max), hi = juce::jmax (p.min, p.max);
    slot.values[ti] = juce::jlimit (lo, hi, std::round (dbForY (e.position.y) * 2.0f) * 0.5f);
    rebuild();
}

void DynTransferGraph::mouseUp (const juce::MouseEvent&)
{
    dragging = false;
    repaint();
}

void DynTransferGraph::mouseDoubleClick (const juce::MouseEvent&)
{
    const int ti = thresholdIndex();
    if (ti >= 0)
        slot.values[ti] = models::model (models::Dynamics, builtModel).params[(size_t) ti].defaultValue;
}

//==============================================================================
DynEnvelopeGraph::DynEnvelopeGraph (DynSlotParams& s) : slot (s) { startTimerHz (12); }

int DynEnvelopeGraph::paramIndex (int which) const
{
    static const char* ids[] = { "att", "hld", "rel" };
    return models::model (models::Dynamics, slot.model.load()).find (ids[juce::jlimit (0, 2, which)]);
}

std::array<float, 3> DynEnvelopeGraph::positions() const
{
    std::array<float, 3> p {};
    const auto& spec = models::model (models::Dynamics, slot.model.load());
    for (int i = 0; i < 3; ++i)
    {
        const int idx = paramIndex (i);
        if (idx < 0)
        {
            p[(size_t) i] = -1.0f;
            continue;
        }
        const auto& param = spec.params[(size_t) idx];
        // Each as its knob is turned: the gate's 10 ms HOLD (a log knob)
        // a third of the way, the compressor's 20 ms a tenth [3.1].
        p[(size_t) i] = param.toNormalised (valueOf (slot, idx));
    }
    return p;
}

void DynEnvelopeGraph::timerCallback()
{
    const auto p = positions();
    if (p != shown || slot.on.load() != shownOn)
    {
        shown = p;
        shownOn = slot.on.load();
        repaint();
    }
}

juce::Rectangle<float> DynEnvelopeGraph::graphArea() const
{
    auto r = getLocalBounds().toFloat();
    r.removeFromTop (r.getHeight() * 0.2f + 6.0f);
    return r;
}

namespace
{
    /** Segment width per unit of knob travel, as a share of the graph's width. */
    constexpr float kSegment = 0.265f;

    struct Shape
    {
        float x0, x1, x2, x3, top, bottom;
    };

    Shape shapeIn (juce::Rectangle<float> area, float a, float h, float r)
    {
        const float w = area.getWidth() * kSegment;
        const float minW = juce::jmax (1.0f, area.getWidth() * 0.006f);
        const float wa = juce::jmax (minW, juce::jmax (0.0f, a) * w), wh = juce::jmax (0.0f, h) * w, wr = juce::jmax (minW, juce::jmax (0.0f, r) * w);
        const float x0 = area.getCentreX() - (wa + wh + wr) * 0.5f;
        return { x0, x0 + wa, x0 + wa + wh, x0 + wa + wh + wr, area.getY() + area.getHeight() * 0.2f, area.getBottom() };
    }
} // namespace

void DynEnvelopeGraph::paintShape (juce::Graphics& g, juce::Rectangle<float> area, float attack, float hold, float release, bool on, bool handles)
{
    g.setColour (plotBg);
    g.fillRect (area);
    paintDots (g, area.reduced (area.getWidth() * 0.02f, area.getHeight() * 0.05f), area.getWidth() / 24.0f, area.getHeight() / 8.0f,
               juce::jmax (1.0f, area.getHeight() * (handles ? 0.004f : 0.006f)), handles ? screenDot : gridDot);
    const auto s = shapeIn (area, attack, hold, release);
    juce::Path p;
    p.startNewSubPath (s.x0, s.bottom);
    p.lineTo (s.x1, s.top);
    p.lineTo (s.x2, s.top);
    p.lineTo (s.x3, s.bottom);
    juce::Path fill (p);
    fill.closeSubPath();
    g.setColour (on ? envFillOn : envFillOff);
    g.fillPath (fill);
    // The guides at the ends of the hold, dimmed where they cross the fill.
    const float guideW = juce::jmax (1.5f, area.getHeight() * 0.012f);
    g.setColour (envGuide.withAlpha (0.6f));
    g.fillRect (s.x1 - guideW * 0.5f, area.getY(), guideW, area.getHeight());
    g.fillRect (s.x2 - guideW * 0.5f, area.getY(), guideW, area.getHeight());
    g.setColour (envGuide);
    g.fillRect (s.x1 - guideW * 0.5f, area.getY(), guideW, s.top - area.getY());
    g.fillRect (s.x2 - guideW * 0.5f, area.getY(), guideW, s.top - area.getY());
    g.setColour (on ? envLineOn : envLineOff);
    g.strokePath (p, juce::PathStrokeType (juce::jmax (1.2f, area.getHeight() * 0.012f)));
    if (! handles)
        return;
    const float d = juce::jlimit (10.0f, 30.0f, area.getHeight() * 0.11f);
    const float low = s.bottom - area.getHeight() * 0.2f;
    for (auto pt : { juce::Point<float> (s.x0, low), juce::Point<float> (s.x2, s.top), juce::Point<float> (s.x3, low) })
    {
        g.setColour (envGuide);
        if (pt.y > s.top)
            g.fillRect (pt.x - 0.75f, pt.y, 1.5f, s.bottom - pt.y);
        g.setColour (plotBg);
        g.fillEllipse (juce::Rectangle<float> (d, d).withCentre (pt));
        g.setColour (juce::Colours::white);
        g.drawEllipse (juce::Rectangle<float> (d, d).withCentre (pt), juce::jmax (1.5f, d * 0.12f));
    }
}

std::array<juce::Point<float>, 3> DynEnvelopeGraph::handlePoints() const
{
    const auto area = graphArea();
    const auto p = positions();
    const auto s = shapeIn (area, p[0], p[1], p[2]);
    const float low = s.bottom - area.getHeight() * 0.2f;
    return { juce::Point<float> (s.x0, low), juce::Point<float> (s.x2, s.top), juce::Point<float> (s.x3, low) };
}

void DynEnvelopeGraph::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    auto title = r.removeFromTop (r.getHeight() * 0.2f);
    g.setColour (Colours::screenHeader);
    g.fillRect (title);
    g.setColour (Colours::text);
    g.setFont (wingFontPx (juce::jmin (TypeScale::header * getWingScreenHeight(), title.getHeight() * 0.42f)));
    g.drawText ("ENVELOPE", title, juce::Justification::centred, false);
    const auto p = positions();
    if (p[0] < 0.0f && p[2] < 0.0f)
        return;
    paintShape (g, graphArea(), p[0], juce::jmax (0.0f, p[1]), p[2], greenEnvelope (slot.model.load()), true);
}

void DynEnvelopeGraph::mouseDown (const juce::MouseEvent& e)
{
    const auto pts = handlePoints();
    dragHandle = -1;
    float best = juce::jmax (24.0f, getHeight() * 0.12f);
    for (int i = 0; i < 3; ++i)
    {
        const float d = pts[(size_t) i].getDistanceFrom (e.position);
        if (d < best && paramIndex (i) >= 0)
        {
            best = d;
            dragHandle = i;
        }
    }
    if (dragHandle >= 0)
        dragStart = positions()[(size_t) dragHandle];
}

void DynEnvelopeGraph::mouseDrag (const juce::MouseEvent& e)
{
    if (dragHandle < 0)
        return;
    const int idx = paramIndex (dragHandle);
    const auto& spec = models::model (models::Dynamics, slot.model.load());
    const float w = juce::jmax (1.0f, graphArea().getWidth() * kSegment);
    // The attack handle sits at the left end: dragging it left lengthens the attack.
    const float sign = dragHandle == 0 ? -1.0f : 1.0f;
    const float pos = juce::jlimit (0.0f, 1.0f, dragStart + sign * (float) e.getDistanceFromDragStartX() / w);
    const auto& param = spec.params[(size_t) idx];
    slot.values[idx] = param.fromNormalised (pos);
    timerCallback();
}

void DynEnvelopeGraph::mouseUp (const juce::MouseEvent&) { dragHandle = -1; }

} // namespace livedaw::ui
