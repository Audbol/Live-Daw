#include "ui/Pages.h"

namespace livedaw::ui
{

UtilityPage::UtilityPage (UiContext& c, bool setup) : ctx (c), onSetup (setup)
{
    clearFaultsButton.setTooltip ("Clears the FX fault and double-assignment markers (a faulted plugin is bypassed until cleared)");
    clearFaultsButton.onClick = [this]
    {
        for (int i = 0; i < kNumFx; ++i)
            ctx.engine.getFx().clearFlags (i);
    };
    clearClipsButton.onClick = [this] { ctx.engine.outputClipEvents = 0; };
    styleIconKey (clearFaultsButton);
    styleIconKey (clearClipsButton);
    addAndMakeVisible (clearFaultsButton);
    addAndMakeVisible (clearClipsButton);

    logList.setModel (this);
    logList.setRowHeight (26);
    logList.setColour (juce::ListBox::backgroundColourId, Dark::body);
    addAndMakeVisible (logList);

    ctx.log.addChangeListener (this);
    startTimerHz (4);
    timerCallback();
}

UtilityPage::~UtilityPage()
{
    ctx.log.removeChangeListener (this);
    logList.setModel (nullptr);
}

void UtilityPage::changeListenerCallback (juce::ChangeBroadcaster*)
{
    logList.updateContent();
    logList.scrollToEnsureRowIsOnscreen (getNumRows() - 1);
    logList.repaint();
}

int UtilityPage::getNumRows()
{
    return ctx.log.getEntries().size();
}

void UtilityPage::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    const auto& entries = ctx.log.getEntries();
    if (! juce::isPositiveAndBelow (row, entries.size()))
        return;

    const auto& e = entries.getReference (row);
    if (selected)
        g.fillAll (Dark::selectedRow);

    auto r = juce::Rectangle<int> (width, height).reduced (8, 0);
    g.setColour (juce::Colours::black);
    g.fillRect (0, height - 1, width, 1);
    const auto font = wingFontFit (TypeScale::body, (float) height * 0.62f);
    g.setFont (font);
    g.setColour (Dark::textDim);
    const auto time = e.time.formatted ("%H:%M:%S");
    g.drawText (time, r.removeFromLeft (juce::roundToInt (juce::GlyphArrangement::getStringWidth (font, time + "  "))), juce::Justification::centredLeft);
    g.setColour (e.level == EventLog::Level::Error     ? Colours::mute
                 : e.level == EventLog::Level::Warning ? Colours::solo
                                                       : Colours::text);
    g.drawText (e.message, r, juce::Justification::centredLeft);
}

void UtilityPage::timerCallback()
{
    auto& engine = ctx.engine;
    health.clear();

    if (auto* device = ctx.deviceManager.getCurrentAudioDevice())
    {
        const auto age = juce::Time::getMillisecondCounter() - engine.getLastCallbackTime();
        const bool running = engine.isDeviceRunning() && age < 500;
        health.push_back ({ "DEVICE", device->getTypeName() + " / " + device->getName(), 0 });
        health.push_back ({ "FORMAT", juce::String (device->getCurrentSampleRate(), 0) + " Hz, " + juce::String (device->getCurrentBufferSizeSamples()) + " samples", 0 });
        health.push_back ({ "LATENCY", "IN " + juce::String (device->getInputLatencyInSamples()) + " / OUT " + juce::String (device->getOutputLatencyInSamples()), 0 });
        health.push_back ({ "CALLBACKS", running ? "RUNNING" : "STALLED", running ? 0 : 2 });
        health.push_back ({ "XRUNS", juce::String (device->getXRunCount()), device->getXRunCount() > 0 ? 1 : 0 });
    }
    else
    {
        health.push_back ({ "DEVICE", "NONE", 2 });
    }

    const int load = juce::roundToInt (engine.getDspLoad() * 100.0f);
    health.push_back ({ "DSP LOAD", juce::String (load) + " %  (" + juce::String (engine.getNumWorkerThreads() + 1) + " THREADS)", load > 80 ? 2 : (load > 60 ? 1 : 0) });
    health.push_back ({ "CONSOLE", juce::String ((int) engine.getChannels().size()) + " CH, " + juce::String ((int) engine.getAux().size()) + " AUX, "
                                       + juce::String ((int) engine.getBuses().size()) + " BUS, " + juce::String ((int) engine.getMains().size()) + " MAIN, "
                                       + juce::String ((int) engine.getMatrices().size()) + " MTX", 0 });
    health.push_back ({ "DSP FAULTS", juce::String (engine.getTotalDspFaults()) + " BLOCKS SILENCED", engine.getTotalDspFaults() > 0 ? 2 : 0 });
    health.push_back ({ "FX FAULTS", juce::String (engine.getNumFaultedFx()) + " SLOTS BYPASSED", engine.getNumFaultedFx() > 0 ? 2 : 0 });
    health.push_back ({ "ENGINE", juce::String (engine.getEngineFaults()) + " CAUGHT FAULTS", engine.getEngineFaults() > 0 ? 2 : 0 });
    health.push_back ({ "OUTPUT CLIPS", juce::String (engine.outputClipEvents.load()) + " BLOCKS LIMITED", engine.outputClipEvents.load() > 0 ? 1 : 0 });
    repaint (statsArea);
}

void UtilityPage::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
    drawDarkPanel (g, leftArea, "SYSTEM HEALTH");
    drawDarkPanel (g, rightArea, "EVENT LOG");

    auto r = statsArea;
    const int h = juce::jmin (34, r.getHeight() / juce::jmax (1, (int) health.size()));
    for (auto& row : health)
    {
        auto line = r.removeFromTop (h);
        g.setColour (juce::Colours::black);
        g.fillRect (line.withHeight (1));
        g.setColour (Dark::textDim);
        g.setFont (wingFontFit (TypeScale::caption, (float) h * 0.5f));
        g.drawText (row.label, line.removeFromLeft (line.getWidth() * 34 / 100), juce::Justification::centredLeft);
        if (row.state != 0)
        {
            auto chip = line.removeFromRight (16).withSizeKeepingCentre (12, 12).toFloat();
            g.setColour (row.state == 2 ? Colours::mute : Colours::amber);
            g.fillEllipse (chip);
            line.removeFromRight (6);
        }
        g.setColour (row.state == 2 ? Colours::mute : Colours::text);
        g.setFont (wingFontFit (TypeScale::body, (float) h * 0.56f));
        g.drawFittedText (row.value, line, juce::Justification::centredLeft, 1);
    }
}

void UtilityPage::resized()
{
    auto b = getLocalBounds();
    logList.setRowHeight (juce::jlimit (14, 26, juce::roundToInt (getWingScreenHeight() * 0.052f)));
    leftArea = b.removeFromLeft (juce::jmax (300, b.getWidth() * 42 / 100));
    b.removeFromLeft (4);
    rightArea = b;

    auto l = darkPanelBody (leftArea, true).reduced (8, 4);
    auto row = l.removeFromBottom (40);
    clearFaultsButton.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (2, 0));
    clearClipsButton.setBounds (row.reduced (2, 0));
    l.removeFromBottom (6);
    statsArea = l;
    logList.setBounds (darkPanelBody (rightArea, true));
}

} // namespace livedaw::ui
