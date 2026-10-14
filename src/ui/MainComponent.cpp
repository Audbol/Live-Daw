#include "ui/MainComponent.h"
#include "engine/Session.h"

namespace livedaw::ui
{

//==============================================================================
class MainComponent::PluginWindow final : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioProcessor& p, const juce::String& title, MainComponent& o)
        : juce::DocumentWindow (title, Colours::panel, closeButton | minimiseButton), processor (p), owner (o)
    {
        setUsingNativeTitleBar (true);

        juce::AudioProcessorEditor* editor = processor.hasEditor() ? processor.createEditorIfNeeded() : nullptr;
        if (editor == nullptr)
            editor = new juce::GenericAudioProcessorEditor (processor);

        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    ~PluginWindow() override { clearContentComponent(); }

    void closeButtonPressed() override
    {
        juce::MessageManager::callAsync ([o = juce::Component::SafePointer<MainComponent> (&owner), this]
        {
            if (o != nullptr)
                o->pluginWindows.removeObject (this);
        });
    }

    juce::AudioProcessor& processor;

private:
    MainComponent& owner;
};

//==============================================================================
/** Lays out input strips, then buses, horizontally. */
class MainComponent::StripsContainer final : public juce::Component
{
public:
    explicit StripsContainer (juce::OwnedArray<StripComponent>& s) : strips (s) {}

    int getPreferredWidth() const
    {
        int w = 0;
        StripKind lastKind = StripKind::Input;
        for (auto* s : strips)
        {
            if (s->getStrip().isBus() && lastKind == StripKind::Input)
                w += kGap;
            lastKind = s->getStrip().getKind();
            w += StripComponent::kWidth;
        }
        return w;
    }

    void resized() override
    {
        int x = 0;
        StripKind lastKind = StripKind::Input;
        for (auto* s : strips)
        {
            if (s->getStrip().isBus() && lastKind == StripKind::Input)
                x += kGap;
            lastKind = s->getStrip().getKind();
            s->setBounds (x, 0, StripComponent::kWidth, getHeight());
            x += StripComponent::kWidth;
        }
    }

private:
    static constexpr int kGap = 12;
    juce::OwnedArray<StripComponent>& strips;
};

//==============================================================================
MainComponent::MainComponent (juce::AudioDeviceManager& dm, MixerEngine& e, PluginHost& h, juce::PropertiesFile& s)
    : deviceManager (dm), engine (e), host (h), settings (s), detailPanel (e, h)
{
    setLookAndFeel (&lookAndFeel);
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

    for (auto* b : { &audioButton, &pluginsButton, &newButton, &openButton, &saveButton,
                     &addAuxButton, &addGroupButton, &fewerButton, &moreButton })
        addAndMakeVisible (b);

    audioButton.onClick = [this] { showAudioSettings(); };
    pluginsButton.onClick = [this] { showPluginScanner(); };
    newButton.onClick = [this] { newSession(); };
    openButton.onClick = [this] { openSession(); };
    saveButton.onClick = [this] { saveSession(); };
    addAuxButton.onClick = [this] { addBus (StripKind::Aux); };
    addGroupButton.onClick = [this] { addBus (StripKind::Group); };
    fewerButton.onClick = [this] { setChannelCount (static_cast<int> (engine.getInputs().size()) - 1); };
    moreButton.onClick = [this] { setChannelCount (static_cast<int> (engine.getInputs().size()) + 1); };

    addAuxButton.setTooltip ("Add an aux bus (monitor mix / FX send)");
    addGroupButton.setTooltip ("Add a group (subgroup routed to master)");

    channelsCaption.setText ("Channels", juce::dontSendNotification);
    channelsCaption.setColour (juce::Label::textColourId, Colours::textDim);
    addAndMakeVisible (channelsCaption);

    channelCountLabel.setEditable (true);
    channelCountLabel.setJustificationType (juce::Justification::centred);
    channelCountLabel.setColour (juce::Label::backgroundColourId, Colours::panelLight);
    channelCountLabel.setTooltip ("Number of input channels (double-click to type). Limited only by CPU.");
    channelCountLabel.onTextChange = [this] { setChannelCount (channelCountLabel.getText().getIntValue()); };
    addAndMakeVisible (channelCountLabel);

    statusLabel.setJustificationType (juce::Justification::centredRight);
    statusLabel.setColour (juce::Label::textColourId, Colours::textDim);
    statusLabel.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (statusLabel);

    stripsContainer = std::make_unique<StripsContainer> (strips);
    stripViewport.setViewedComponent (stripsContainer.get(), false);
    stripViewport.setScrollBarsShown (false, true);
    addAndMakeVisible (stripViewport);

    detailPanel.onOpenPluginEditor = [this] (Strip& strip, int slot) { openPluginEditor (strip, slot); };
    detailPanel.onPluginRemoved = [this] (juce::AudioProcessor* p) { closePluginWindowsFor (p); };
    detailPanel.onShowPluginScanner = [this] { showPluginScanner(); };
    addAndMakeVisible (detailPanel);

    engine.onDeviceChanged = [this]
    {
        // Hardware channel names may have changed.
        detailPanel.refresh();
    };

    rebuildStrips();
    updateTitle();
    setSize (1400, 900);
    startTimerHz (2);
}

MainComponent::~MainComponent()
{
    engine.onDeviceChanged = nullptr;
    pluginWindows.clear();
    scannerWindow.reset();
    if (audioSettingsWindow != nullptr)
        delete audioSettingsWindow.getComponent();

    stripViewport.setViewedComponent (nullptr, false);
    detailPanel.setStrip (nullptr);
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

//==============================================================================
void MainComponent::rebuildStrips()
{
    auto* selected = detailPanel.getStrip();
    bool selectedStillExists = selected == &engine.getMaster();

    strips.clear();

    auto addStrip = [this, &selected, &selectedStillExists] (Strip& s)
    {
        auto* comp = strips.add (new StripComponent (s, engine));
        comp->onSelect = [this] (Strip& st) { selectStrip (&st); };
        comp->onRemove = [this] (Strip& st) { removeStrip (st); };
        stripsContainer->addAndMakeVisible (comp);
        selectedStillExists = selectedStillExists || &s == selected;
    };

    for (auto& s : engine.getInputs())
        addStrip (*s);
    for (auto& s : engine.getBuses())
        addStrip (*s);

    masterStrip = std::make_unique<StripComponent> (engine.getMaster(), engine);
    masterStrip->onSelect = [this] (Strip& st) { selectStrip (&st); };
    addAndMakeVisible (*masterStrip);

    channelCountLabel.setText (juce::String (engine.getInputs().size()), juce::dontSendNotification);

    selectStrip (selectedStillExists ? selected : nullptr);
    resized();
    stripsContainer->resized(); // its size may not have changed, but its children did
}

void MainComponent::selectStrip (Strip* strip)
{
    for (auto* s : strips)
        s->setSelected (&s->getStrip() == strip);
    if (masterStrip != nullptr)
        masterStrip->setSelected (strip == &engine.getMaster());

    detailPanel.setStrip (strip);
}

void MainComponent::removeStrip (Strip& strip)
{
    // Defer: this is called from inside the strip component's own menu callback.
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this), s = &strip]
    {
        if (safe == nullptr)
            return;

        if (safe->detailPanel.getStrip() == s)
            safe->detailPanel.setStrip (nullptr);

        safe->closePluginWindowsFor (*s);
        const bool wasBus = s->isBus();
        safe->engine.removeStrip (s);
        safe->rebuildStrips();

        if (wasBus)
            safe->detailPanel.refresh(); // sends list changed
    });
}

void MainComponent::setChannelCount (int count)
{
    count = juce::jlimit (0, 4096, count);

    // Close editors of plugins on channels that are about to disappear.
    for (size_t i = static_cast<size_t> (count); i < engine.getInputs().size(); ++i)
        closePluginWindowsFor (*engine.getInputs()[i]);

    if (auto* sel = detailPanel.getStrip())
        for (size_t i = static_cast<size_t> (count); i < engine.getInputs().size(); ++i)
            if (engine.getInputs()[i].get() == sel)
                detailPanel.setStrip (nullptr);

    engine.setNumInputs (count);
    rebuildStrips();
}

void MainComponent::addBus (StripKind kind)
{
    if (engine.addBus (kind) == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Live DAW",
                                                "All " + juce::String (kMaxBuses) + " bus slots are in use.");
        return;
    }

    rebuildStrips();
    detailPanel.refresh();
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);
    g.setColour (Colours::panel);
    g.fillRect (getLocalBounds().removeFromTop (44));
}

void MainComponent::resized()
{
    auto b = getLocalBounds();
    auto bar = b.removeFromTop (44).reduced (8, 8);

    for (auto* button : { &audioButton, &pluginsButton })
    {
        button->setBounds (bar.removeFromLeft (100));
        bar.removeFromLeft (6);
    }
    bar.removeFromLeft (12);
    for (auto* button : { &newButton, &openButton, &saveButton })
    {
        button->setBounds (bar.removeFromLeft (60));
        bar.removeFromLeft (6);
    }
    bar.removeFromLeft (12);
    channelsCaption.setBounds (bar.removeFromLeft (64));
    fewerButton.setBounds (bar.removeFromLeft (28));
    channelCountLabel.setBounds (bar.removeFromLeft (50).reduced (2, 0));
    moreButton.setBounds (bar.removeFromLeft (28));
    bar.removeFromLeft (12);
    addAuxButton.setBounds (bar.removeFromLeft (70));
    bar.removeFromLeft (6);
    addGroupButton.setBounds (bar.removeFromLeft (80));
    bar.removeFromLeft (12);
    statusLabel.setBounds (bar);

    detailPanel.setBounds (b.removeFromBottom (juce::jlimit (330, 380, getHeight() * 38 / 100)));

    auto console = b.reduced (4);
    if (masterStrip != nullptr)
        masterStrip->setBounds (console.removeFromRight (StripComponent::kWidth));
    console.removeFromRight (8);

    stripViewport.setBounds (console);
    stripsContainer->setSize (juce::jmax (console.getWidth(), stripsContainer->getPreferredWidth()),
                              console.getHeight() - stripViewport.getScrollBarThickness());
}

void MainComponent::timerCallback()
{
    juce::String status;

    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        status << device->getTypeName() << ": " << device->getName() << "  |  "
               << juce::String (device->getCurrentSampleRate() / 1000.0, 1) << " kHz, "
               << device->getCurrentBufferSizeSamples() << " smp ("
               << juce::String (1000.0 * device->getCurrentBufferSizeSamples() / device->getCurrentSampleRate(), 1)
               << " ms)  |  ";
    }
    else
    {
        status << "No audio device  |  ";
    }

    status << "DSP " << juce::roundToInt (engine.getDspLoad() * 100.0f) << "%  |  "
           << (engine.getNumWorkerThreads() + 1) << " threads";

    if (engine.anySoloActive.load())
        status << "  |  SOLO ACTIVE";

    statusLabel.setText (status, juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId,
                           engine.getDspLoad() > 0.85f ? Colours::mute
                                                       : (engine.anySoloActive.load() ? Colours::solo : Colours::textDim));
}

//==============================================================================
void MainComponent::showAudioSettings()
{
    if (audioSettingsWindow != nullptr)
    {
        audioSettingsWindow->toFront (true);
        return;
    }

    auto* selector = new juce::AudioDeviceSelectorComponent (deviceManager, 0, 256, 0, 256,
                                                             false, false, false, false);
    selector->setSize (560, 520);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (selector);
    options.dialogTitle = "Audio Setup (ASIO / CoreAudio / ALSA / JACK)";
    options.dialogBackgroundColour = Colours::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    audioSettingsWindow = options.launchAsync();
}

void MainComponent::showPluginScanner()
{
    if (scannerWindow != nullptr)
    {
        scannerWindow->toFront (true);
        return;
    }

    class ScannerWindow final : public juce::DocumentWindow
    {
    public:
        ScannerWindow (PluginHost& h, std::unique_ptr<juce::DocumentWindow>& o)
            : juce::DocumentWindow ("Plugins (VST3 / AU / LV2)", Colours::panel, closeButton), owner (o)
        {
            setUsingNativeTitleBar (true);
            auto* list = new juce::PluginListComponent (h.getFormatManager(), h.getKnownPlugins(),
                                                        h.getDeadMansPedalFile(), h.getSettings(), true);
            list->setSize (700, 500);
            setContentOwned (list, true);
            setResizable (true, false);
            centreWithSize (700, 500);
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            juce::MessageManager::callAsync ([&o = owner] { o.reset(); });
        }

    private:
        std::unique_ptr<juce::DocumentWindow>& owner;
    };

    scannerWindow = std::make_unique<ScannerWindow> (host, scannerWindow);
}

void MainComponent::openPluginEditor (Strip& strip, int slot)
{
    auto* plugin = strip.inserts.getPlugin (slot);
    if (plugin == nullptr)
        return;

    for (auto* w : pluginWindows)
    {
        if (&w->processor == plugin)
        {
            w->toFront (true);
            return;
        }
    }

    pluginWindows.add (new PluginWindow (*plugin, strip.name + " - " + plugin->getName(), *this));
}

void MainComponent::closePluginWindowsFor (juce::AudioProcessor* processor)
{
    for (int i = pluginWindows.size(); --i >= 0;)
        if (&pluginWindows[i]->processor == processor)
            pluginWindows.remove (i);
}

void MainComponent::closePluginWindowsFor (Strip& strip)
{
    for (int slot = 0; slot < kNumInsertSlots; ++slot)
        if (auto* p = strip.inserts.getPlugin (slot))
            closePluginWindowsFor (p);
}

//==============================================================================
void MainComponent::newSession()
{
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "New session",
                                        "Clear the console and start a new session?", "New", "Cancel", this,
                                        juce::ModalCallbackFunction::create ([this] (int result)
    {
        if (result == 0)
            return;

        pluginWindows.clear();
        detailPanel.setStrip (nullptr);
        engine.reset();
        engine.setNumInputs (16);
        sessionFile = juce::File();
        rebuildStrips();
        updateTitle();
    }));
}

void MainComponent::openSession()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Open session", sessionFile, "*.livedaw");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File())
            return;

        pluginWindows.clear();
        detailPanel.setStrip (nullptr);
        const auto result = session::loadFromFile (engine, file, host);
        sessionFile = file;
        rebuildStrips();
        updateTitle();

        if (result.failed())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Open session",
                                                    result.getErrorMessage());
    });
}

void MainComponent::saveSession()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Save session", sessionFile, "*.livedaw");
    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this] (const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file == juce::File())
            return;

        file = file.withFileExtension ("livedaw");
        const auto result = session::saveToFile (engine, file);

        if (result.failed())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save session",
                                                    result.getErrorMessage());
            return;
        }

        sessionFile = file;
        updateTitle();
    });
}

void MainComponent::updateTitle()
{
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
        window->setName ("Live DAW" + (sessionFile.existsAsFile() ? " - " + sessionFile.getFileNameWithoutExtension()
                                                                  : juce::String()));
}

} // namespace livedaw::ui
