#include "ui/Pages.h"

namespace livedaw::ui
{

UtilityPage::UtilityPage (UiContext& c) : ctx (c)
{
    stats.setJustificationType (juce::Justification::topLeft);
    stats.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.5f, juce::Font::plain));
    addAndMakeVisible (stats);

    clearFaultsButton.setTooltip ("Clears the FX fault and double-assignment markers (a faulted plugin is bypassed until cleared)");
    clearFaultsButton.onClick = [this]
    {
        for (int i = 0; i < kNumFx; ++i)
            ctx.engine.getFx().clearFlags (i);
    };
    clearClipsButton.onClick = [this] { ctx.engine.outputClipEvents = 0; };
    addAndMakeVisible (clearFaultsButton);
    addAndMakeVisible (clearClipsButton);

    logList.setModel (this);
    logList.setRowHeight (18);
    logList.setColour (juce::ListBox::backgroundColourId, juce::Colours::black);
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
        g.fillAll (Colours::accent.withAlpha (0.3f));

    auto r = juce::Rectangle<int> (width, height).reduced (6, 0);
    g.setFont (juce::FontOptions (12.0f));
    g.setColour (Colours::textDim);
    g.drawText (e.time.formatted ("%H:%M:%S"), r.removeFromLeft (64), juce::Justification::centredLeft);
    g.setColour (e.level == EventLog::Level::Error     ? Colours::mute
                 : e.level == EventLog::Level::Warning ? Colours::solo
                                                       : Colours::text);
    g.drawText (e.message, r, juce::Justification::centredLeft);
}

void UtilityPage::timerCallback()
{
    auto& engine = ctx.engine;
    juce::String text;

    if (auto* device = ctx.deviceManager.getCurrentAudioDevice())
    {
        const auto age = juce::Time::getMillisecondCounter() - engine.getLastCallbackTime();
        text << "Device       " << device->getTypeName() << " / " << device->getName() << "\n"
             << "Format       " << juce::String (device->getCurrentSampleRate(), 0) << " Hz, "
             << device->getCurrentBufferSizeSamples() << " samples\n"
             << "Latency      in " << device->getInputLatencyInSamples() << " / out "
             << device->getOutputLatencyInSamples() << " samples\n"
             << "Callbacks    " << (engine.isDeviceRunning() && age < 500 ? "running" : "STALLED") << "\n"
             << "Xruns        " << device->getXRunCount() << "\n";
    }
    else
    {
        text << "Device       none\n\n\n\n\n";
    }

    text << "DSP load     " << juce::roundToInt (engine.getDspLoad() * 100.0f) << "% ("
         << (engine.getNumWorkerThreads() + 1) << " threads)\n"
         << "Console      " << (int) engine.getChannels().size() << " ch, " << (int) engine.getAux().size() << " aux, "
         << (int) engine.getBuses().size() << " bus, " << (int) engine.getMains().size() << " main, "
         << (int) engine.getMatrices().size() << " mtx\n"
         << "DSP faults   " << engine.getTotalDspFaults() << " blocks silenced\n"
         << "FX faults    " << engine.getNumFaultedFx() << " slots auto-bypassed\n"
         << "Engine       " << engine.getEngineFaults() << " caught faults\n"
         << "Output clips " << engine.outputClipEvents.load() << " blocks limited";

    stats.setText (text, juce::dontSendNotification);
}

void UtilityPage::paint (juce::Graphics& g)
{
    g.fillAll (Colours::screen);
    drawSectionBox (g, leftArea, "SYSTEM HEALTH");
    drawSectionBox (g, rightArea, "EVENT LOG");
}

void UtilityPage::resized()
{
    auto b = getLocalBounds().reduced (2);
    leftArea = b.removeFromLeft (juce::jmax (340, b.getWidth() * 2 / 5));
    rightArea = b;

    auto l = leftArea.reduced (10).withTrimmedTop (18);
    auto row = l.removeFromBottom (28);
    clearFaultsButton.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (2, 0));
    clearClipsButton.setBounds (row.reduced (2, 0));
    stats.setBounds (l);
    logList.setBounds (rightArea.reduced (10).withTrimmedTop (18));
}

} // namespace livedaw::ui
