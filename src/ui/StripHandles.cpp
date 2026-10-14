#include "ui/StripHandles.h"
#include "dsp/DspUtils.h"

namespace livedaw::ui
{

namespace
{
    constexpr int kToggleWidth = 18;

    void drawToggle (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, bool on, juce::Colour onColour)
    {
        g.setColour (on ? onColour : juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (r.toFloat().reduced (0.5f), 2.0f);
        g.setColour (on ? juce::Colours::black : Colours::textDim);
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.drawText (text, r, juce::Justification::centred);
    }

    float proportionForDb (float db, float minDb) { return juce::jlimit (0.0f, 1.0f, (db - minDb) / -minDb); }
} // namespace

//==============================================================================
InsertSlotsHandle::InsertSlotsHandle (Strip& s, UiContext& c) : strip (s), ctx (c)
{
    startTimerHz (4);
}

void InsertSlotsHandle::setNumRows (int rows)
{
    numRows = juce::jlimit (1, kNumInsertSlots, rows);
    repaint();
}

int InsertSlotsHandle::rowAt (juce::Point<int> p) const
{
    const int row = p.y / kRowHeight;
    return juce::isPositiveAndBelow (row, numRows) ? row : -1;
}

void InsertSlotsHandle::paint (juce::Graphics& g)
{
    g.setFont (juce::FontOptions (10.0f));

    for (int i = 0; i < numRows; ++i)
    {
        auto r = juce::Rectangle<int> (0, i * kRowHeight, getWidth(), kRowHeight).reduced (0, 1);
        auto* plugin = strip.inserts.getPlugin (i);

        juce::Colour bg = juce::Colours::black.withAlpha (0.3f);
        juce::Colour fg = Colours::textDim;
        juce::String text = "-";

        if (plugin != nullptr)
        {
            text = plugin->getName();

            if (strip.inserts.isFaulted (i))
            {
                bg = Colours::mute.withAlpha (0.8f);
                fg = juce::Colours::white;
                text = "! " + text;
            }
            else if (strip.inserts.isBypassed (i))
            {
                bg = Colours::panelLight;
                fg = Colours::textDim;
            }
            else
            {
                bg = Colours::accent.withAlpha (0.55f);
                fg = juce::Colours::white;
            }
        }

        g.setColour (bg);
        g.fillRoundedRectangle (r.toFloat(), 2.0f);
        g.setColour (fg);
        g.drawFittedText (text, r.reduced (3, 0), juce::Justification::centredLeft, 1, 0.8f);
    }
}

void InsertSlotsHandle::mouseUp (const juce::MouseEvent& e)
{
    const int row = rowAt (e.getPosition());
    if (row < 0 || ! getLocalBounds().contains (e.getPosition()))
        return;

    if (e.mods.isPopupMenu())
        showInsertSlotMenu (ctx, strip, row, this);
    else
        clickInsertSlot (ctx, strip, row, this);
}

//==============================================================================
GateHandle::GateHandle (Strip& s) : strip (s)
{
    setTooltip ("Gate: click G to switch on/off, drag the bar to set the threshold, double-click to reset");
    startTimerHz (30);
}

void GateHandle::timerCallback()
{
    // This handle is the only reader of the input peak meter.
    const float peak = juce::jmax (strip.inputPeak[0].exchange (0.0f), strip.inputPeak[1].exchange (0.0f));
    const float db = dsp::gainToDb (peak);
    levelDb = db > levelDb ? db : juce::jmax (db, levelDb - 1.5f);
    repaint();
}

void GateHandle::paint (juce::Graphics& g)
{
    const auto& gp = strip.params.gate;
    const bool on = gp.enabled.load();
    const bool closed = on && strip.gateGain.load() < 0.5f;
    auto b = getLocalBounds();

    drawToggle (g, b.removeFromLeft (kToggleWidth), "G", on, closed ? Colours::mute : Colours::active);
    b.removeFromLeft (2);

    auto bar = b.toFloat();
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillRoundedRectangle (bar, 2.0f);

    constexpr float minDb = -80.0f;
    g.setColour ((on ? Colours::meterGreen : Colours::textDim).withAlpha (0.6f));
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * proportionForDb (levelDb, minDb)), 2.0f);

    const float tx = bar.getX() + bar.getWidth() * proportionForDb (gp.thresholdDb.load(), minDb);
    g.setColour (on ? Colours::solo : Colours::textDim);
    g.fillRect (tx - 1.0f, bar.getY(), 2.0f, bar.getHeight());

    g.setColour (Colours::text);
    g.setFont (juce::FontOptions (9.0f));
    g.drawText (juce::String (juce::roundToInt (gp.thresholdDb.load())), b.reduced (2, 0), juce::Justification::centredRight);
}

void GateHandle::mouseDown (const juce::MouseEvent& e)
{
    draggingBar = e.x > kToggleWidth;

    if (! draggingBar)
        strip.params.gate.enabled = ! strip.params.gate.enabled.load();

    dragStartThreshold = strip.params.gate.thresholdDb.load();
}

void GateHandle::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggingBar)
        return;

    const float barWidth = static_cast<float> (juce::jmax (1, getWidth() - kToggleWidth - 2));
    const float delta = (static_cast<float> (e.getDistanceFromDragStartX()) / barWidth) * 80.0f
                        - static_cast<float> (e.getDistanceFromDragStartY()) * 0.25f;
    strip.params.gate.thresholdDb = juce::jlimit (-80.0f, 0.0f, std::round ((dragStartThreshold + delta) * 2.0f) / 2.0f);
}

void GateHandle::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.x > kToggleWidth)
        strip.params.gate.thresholdDb = -50.0f;
}

//==============================================================================
CompHandle::CompHandle (Strip& s) : strip (s)
{
    setTooltip ("Compressor: click C to switch on/off, drag to set the threshold, double-click to reset");
    startTimerHz (30);
}

void CompHandle::timerCallback()
{
    const float r = strip.compReductionDb.load();
    reductionDb = r > reductionDb ? r : reductionDb * 0.85f + r * 0.15f;
    repaint();
}

void CompHandle::paint (juce::Graphics& g)
{
    const auto& cp = strip.params.comp;
    const bool on = cp.enabled.load();
    auto b = getLocalBounds();

    drawToggle (g, b.removeFromLeft (kToggleWidth), "C", on, reductionDb > 0.5f ? Colours::solo : Colours::active);
    b.removeFromLeft (2);

    auto bar = b.toFloat();
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillRoundedRectangle (bar, 2.0f);

    if (on)
    {
        const float w = bar.getWidth() * juce::jlimit (0.0f, 1.0f, reductionDb / 20.0f);
        g.setColour (Colours::solo.withAlpha (0.85f));
        g.fillRoundedRectangle (bar.removeFromRight (w), 2.0f);
    }

    g.setColour (Colours::text);
    g.setFont (juce::FontOptions (9.0f));
    g.drawText (juce::String (juce::roundToInt (cp.thresholdDb.load())) + " " + juce::String (cp.ratio.load(), 1) + ":1",
                b.reduced (2, 0), juce::Justification::centredLeft);
}

void CompHandle::mouseDown (const juce::MouseEvent& e)
{
    draggingBar = e.x > kToggleWidth;

    if (! draggingBar)
        strip.params.comp.enabled = ! strip.params.comp.enabled.load();

    dragStartThreshold = strip.params.comp.thresholdDb.load();
}

void CompHandle::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggingBar)
        return;

    const float delta = static_cast<float> (e.getDistanceFromDragStartX()) * 0.25f
                        - static_cast<float> (e.getDistanceFromDragStartY()) * 0.25f;
    strip.params.comp.thresholdDb = juce::jlimit (-60.0f, 0.0f, std::round ((dragStartThreshold + delta) * 2.0f) / 2.0f);
}

void CompHandle::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.x > kToggleWidth)
        strip.params.comp.thresholdDb = -20.0f;
}

//==============================================================================
MiniToggle::MiniToggle (std::atomic<bool>& p, const juce::String& text, juce::Colour onColour)
    : param (p), label (text), colour (onColour)
{
    shown = param.load();
    startTimerHz (10);
}

void MiniToggle::paint (juce::Graphics& g)
{
    drawToggle (g, getLocalBounds(), label, shown, colour);
}

void MiniToggle::mouseDown (const juce::MouseEvent&)
{
    param = ! param.load();
    timerCallback();
}

void MiniToggle::timerCallback()
{
    if (shown != param.load())
    {
        shown = param.load();
        repaint();
    }
}

//==============================================================================
EqHandle::EqHandle (Strip& s, UiContext& ctx)
    : editor (s.params.eq, [&ctx] { return ctx.engine.getSampleRate(); }),
      eqOn (s.params.eq.enabled, "EQ", Colours::accent),
      hpfOn (s.params.hpfEnabled, "HP", Colours::accent)
{
    editor.setCompact (true);
    editor.onBackgroundClick = [&ctx, &s] { if (ctx.selectStrip) ctx.selectStrip (&s); };
    editor.setTooltip ("EQ: drag the handles (wheel = Q), click to open the channel");
    addAndMakeVisible (editor);

    eqOn.setTooltip ("EQ on/off");
    hpfOn.setTooltip ("High-pass filter on/off");
    addAndMakeVisible (eqOn);
    addAndMakeVisible (hpfOn);
}

void EqHandle::resized()
{
    auto b = getLocalBounds();
    auto toggles = b.removeFromLeft (kToggleWidth);
    eqOn.setBounds (toggles.removeFromTop (toggles.getHeight() / 2).reduced (0, 1));
    hpfOn.setBounds (toggles.reduced (0, 1));
    b.removeFromLeft (2);
    editor.setBounds (b);
}

} // namespace livedaw::ui
