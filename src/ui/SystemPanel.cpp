#include "ui/SystemPanel.h"

namespace livedaw::ui
{

SystemPanel::SystemPanel (UiContext& c, juce::AudioDeviceManager& dm)
    : ctx (c),
      deviceManager (dm),
      protection (c.engine.outputProtection, "Output protection", Colours::active),
      ceiling (c.engine.outputCeilingDb, -24.0, 6.0, 0.1, " dB", juce::Slider::LinearHorizontal)
{
    protection.setTooltip ("Brick-wall safety clip on every hardware output. Also silences NaN/inf.");
    addAndMakeVisible (protection);

    ceiling.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 20);
    addAndMakeVisible (ceiling);
    ceilingLabel.setText ("Output ceiling", juce::dontSendNotification);
    ceilingLabel.setColour (juce::Label::textColourId, Colours::textDim);
    ceilingLabel.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (ceilingLabel);

    stats.setJustificationType (juce::Justification::topLeft);
    stats.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
    addAndMakeVisible (stats);

    clearFaultsButton.setTooltip ("Clears the fault markers. Faulted plugins stay bypassed until you un-bypass them.");
    clearFaultsButton.onClick = [this]
    {
        auto clear = [] (Strip& s)
        {
            for (int i = 0; i < kNumInsertSlots; ++i)
                s.inserts.clearFault (i);
        };
        clear (ctx.engine.getMaster());
        for (auto& s : ctx.engine.getInputs())
            clear (*s);
        for (auto& s : ctx.engine.getBuses())
            clear (*s);
    };
    addAndMakeVisible (clearFaultsButton);

    logList.setModel (this);
    logList.setRowHeight (18);
    logList.setColour (juce::ListBox::backgroundColourId, juce::Colours::black.withAlpha (0.3f));
    addAndMakeVisible (logList);

    ctx.log.addChangeListener (this);
    startTimerHz (4);
    timerCallback();
}

SystemPanel::~SystemPanel()
{
    ctx.log.removeChangeListener (this);
}

void SystemPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    logList.updateContent();
    logList.scrollToEnsureRowIsOnscreen (getNumRows() - 1);
    logList.repaint();
}

int SystemPanel::getNumRows()
{
    return ctx.log.getEntries().size();
}

void SystemPanel::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
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

void SystemPanel::timerCallback()
{
    auto& engine = ctx.engine;
    juce::String text;

    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        const auto age = juce::Time::getMillisecondCounter() - engine.getLastCallbackTime();
        text << "Device         " << device->getTypeName() << " / " << device->getName() << "\n"
             << "Format         " << juce::String (device->getCurrentSampleRate(), 0) << " Hz, "
             << device->getCurrentBufferSizeSamples() << " samples\n"
             << "Latency        in " << device->getInputLatencyInSamples() << " / out "
             << device->getOutputLatencyInSamples() << " samples\n"
             << "Callbacks      " << (engine.isDeviceRunning() && age < 500 ? "running" : "STALLED") << "\n"
             << "Xruns          " << device->getXRunCount() << "\n";
    }
    else
    {
        text << "Device         none\n\n\n\n\n";
    }

    text << "DSP load       " << juce::roundToInt (engine.getDspLoad() * 100.0f) << "% ("
         << (engine.getNumWorkerThreads() + 1) << " threads)\n"
         << "Channels       " << (int) engine.getInputs().size() << " in, " << (int) engine.getBuses().size() << " buses\n"
         << "DSP faults     " << engine.getTotalDspFaults() << " blocks silenced\n"
         << "Plugin faults  " << engine.getNumFaultedPlugins() << " auto-bypassed\n"
         << "Output clips   " << engine.outputClipEvents.load() << " blocks limited";

    stats.setText (text, juce::dontSendNotification);
}

void SystemPanel::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background.darker (0.2f));

    for (auto [area, title] : { std::pair { leftArea, "PROTECTION & HEALTH" }, std::pair { rightArea, "EVENT LOG" } })
    {
        g.setColour (Colours::panel);
        g.fillRoundedRectangle (area.toFloat().reduced (2.0f), 5.0f);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (title, area.reduced (10, 6).removeFromTop (14), juce::Justification::centredLeft);
    }
}

void SystemPanel::resized()
{
    auto b = getLocalBounds().reduced (2);
    leftArea = b.removeFromLeft (juce::jmax (360, b.getWidth() / 3));
    rightArea = b;

    auto l = leftArea.reduced (10).withTrimmedTop (18);
    auto row = l.removeFromTop (24);
    protection.setBounds (row.removeFromLeft (150));
    row.removeFromLeft (8);
    clearFaultsButton.setBounds (row);
    l.removeFromTop (6);
    ceilingLabel.setBounds (l.removeFromTop (14));
    ceiling.setBounds (l.removeFromTop (24));
    l.removeFromTop (6);
    stats.setBounds (l);

    logList.setBounds (rightArea.reduced (10).withTrimmedTop (18));
}

} // namespace livedaw::ui
