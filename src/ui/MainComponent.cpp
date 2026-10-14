#include "ui/MainComponent.h"

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

        juce::AudioProcessorEditor* editor = processor.hasEditor() ? processor.createEditorAndMakeActive() : nullptr;
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
        juce::MessageManager::callAsync ([o = juce::Component::SafePointer<MainComponent> (&owner),
                                          w = juce::Component::SafePointer<PluginWindow> (this)]
        {
            if (o != nullptr && w != nullptr)
                o->pluginWindows.removeObject (w.getComponent());
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
        int x = 0;
        layout ([&x] (StripComponent*, int left) { x = left + StripComponent::kWidth; });
        return x;
    }

    void resized() override
    {
        layout ([this] (StripComponent* s, int left) { s->setBounds (left, 0, StripComponent::kWidth, getHeight()); });
    }

private:
    template <typename Fn>
    void layout (Fn&& place) const
    {
        // A gap separates runs of input channels from runs of buses/master.
        int x = 0;
        const Strip* previous = nullptr;
        for (auto* s : strips)
        {
            const bool isInput = s->getStrip().getKind() == StripKind::Input;
            if (previous != nullptr && isInput != (previous->getKind() == StripKind::Input))
                x += kGap;
            previous = &s->getStrip();
            place (s, x);
            x += StripComponent::kWidth;
        }
    }

    static constexpr int kGap = 12;
    juce::OwnedArray<StripComponent>& strips;
};

//==============================================================================
MainComponent::MainComponent (juce::AudioDeviceManager& dm, MixerEngine& e, PluginHost& h, Show& s,
                              EventLog& l, juce::PropertiesFile& props)
    : deviceManager (dm),
      engine (e),
      show (s),
      log (l),
      settings (props),
      ctx { e, h, s, l, {}, {}, {}, {}, {}, {} },
      layerBar (ctx),
      detailPanel (ctx),
      showPanel (ctx),
      layersPanel (ctx),
      systemPanel (ctx, dm)
{
    ctx.openPluginEditor = [this] (Strip& strip, int slot) { openPluginEditor (strip, slot); };
    ctx.pluginRemoved = [this] (juce::AudioProcessor* p) { closePluginWindowsFor (p); };
    ctx.showPluginScanner = [this] { showPluginScanner(); };
    ctx.selectStrip = [this] (Strip* strip) { selectStrip (strip); };
    ctx.getSelectedStrip = [this] { return detailPanel.getStrip(); };
    ctx.layersChanged = [this] { rebuildStrips(); };

    layerBar.onEditLayers = [this]
    {
        tabs.setCurrentTabIndex (2);
        setPanelVisible (true);
    };
    addAndMakeVisible (layerBar);
    setWantsKeyboardFocus (true);

    setLookAndFeel (&lookAndFeel);
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

    for (auto* b : { &audioButton, &pluginsButton, &showButton, &addAuxButton, &addGroupButton,
                     &fewerButton, &moreButton, &prevSceneButton, &goButton, &panelButton })
        addAndMakeVisible (b);

    audioButton.onClick = [this] { showAudioSettings(); };
    pluginsButton.onClick = [this] { showPluginScanner(); };
    showButton.onClick = [this] { showShowMenu(); };
    addAuxButton.onClick = [this] { addBus (StripKind::Aux); };
    addGroupButton.onClick = [this] { addBus (StripKind::Group); };
    fewerButton.onClick = [this] { setChannelCount (static_cast<int> (engine.getInputs().size()) - 1); };
    moreButton.onClick = [this] { setChannelCount (static_cast<int> (engine.getInputs().size()) + 1); };
    prevSceneButton.onClick = [this] { recall (&Show::recallPrevious); };
    goButton.onClick = [this] { recall (&Show::recallNext); };
    panelButton.onClick = [this] { setPanelVisible (! panelVisible); };

    showButton.setTooltip ("New / open / save show");
    addAuxButton.setTooltip ("Add an aux bus (monitor mix / FX send)");
    addGroupButton.setTooltip ("Add a group (subgroup routed to master)");
    prevSceneButton.setTooltip ("Recall the previous scene");
    goButton.setTooltip ("Recall the next scene");
    goButton.setColour (juce::TextButton::buttonColourId, Colours::active.darker (0.3f));
    panelButton.setTooltip ("Show / hide the bottom panel");
    panelButton.setClickingTogglesState (false);

    channelsCaption.setText ("Channels", juce::dontSendNotification);
    channelsCaption.setColour (juce::Label::textColourId, Colours::textDim);
    addAndMakeVisible (channelsCaption);

    channelCountLabel.setEditable (true);
    channelCountLabel.setJustificationType (juce::Justification::centred);
    channelCountLabel.setColour (juce::Label::backgroundColourId, Colours::panelLight);
    channelCountLabel.setTooltip ("Number of input channels (double-click to type). Limited only by CPU.");
    channelCountLabel.onTextChange = [this] { setChannelCount (channelCountLabel.getText().getIntValue()); };
    addAndMakeVisible (channelCountLabel);

    sceneLabel.setJustificationType (juce::Justification::centred);
    sceneLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    sceneLabel.setColour (juce::Label::backgroundColourId, juce::Colours::black.withAlpha (0.35f));
    sceneLabel.setColour (juce::Label::textColourId, Colours::active);
    addAndMakeVisible (sceneLabel);

    statusLabel.setJustificationType (juce::Justification::centredRight);
    statusLabel.setColour (juce::Label::textColourId, Colours::textDim);
    statusLabel.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (statusLabel);

    stripsContainer = std::make_unique<StripsContainer> (strips);
    stripViewport.setViewedComponent (stripsContainer.get(), false);
    stripViewport.setScrollBarsShown (false, true);
    addAndMakeVisible (stripViewport);

    const auto tabColour = Colours::background.darker (0.2f);
    tabs.addTab ("Channel", tabColour, &detailPanel, false);
    tabs.addTab ("Show", tabColour, &showPanel, false);
    tabs.addTab ("Layers", tabColour, &layersPanel, false);
    tabs.addTab ("System", tabColour, &systemPanel, false);
    tabs.setTabBarDepth (26);
    tabs.setOutline (0);
    addAndMakeVisible (tabs);

    engine.onDeviceChanged = [this] { detailPanel.refresh(); };

    show.onChanged = [this]
    {
        showPanel.refresh();
        updateSceneControls();
    };

    rebuildStrips();
    updateSceneControls();
    setSize (1500, 950);
    startTimerHz (2);
}

MainComponent::~MainComponent()
{
    show.onChanged = nullptr;
    engine.onDeviceChanged = nullptr;
    closeAllPluginWindows();
    scannerWindow.reset();
    if (audioSettingsWindow != nullptr)
        delete audioSettingsWindow.getComponent();

    tabs.clearTabs();
    stripViewport.setViewedComponent (nullptr, false);
    detailPanel.setStrip (nullptr);
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

//==============================================================================
void MainComponent::rebuildStrips()
{
    // The selection survives layer changes as long as the strip still exists
    // (pointer comparison only: a removed strip must not be dereferenced).
    auto* selected = detailPanel.getStrip();
    bool selectedStillExists = selected == &engine.getMaster();
    for (auto* list : { &engine.getInputs(), &engine.getBuses() })
        for (auto& s : *list)
            selectedStillExists = selectedStillExists || s.get() == selected;

    strips.clear();

    // Only the current layer's strips get components: big consoles stay light.
    auto& layers = show.layers;
    const auto layer = layers.findLayer (engine, layers.selectedLayerId);
    layers.selectedLayerId = layer.id;

    for (auto* s : LayerSet::resolve (engine, layer))
    {
        auto* comp = strips.add (new StripComponent (*s, ctx));
        comp->onRemove = [this] (Strip& st) { removeStrip (st); };
        comp->setNumInsertRows (insertRows);
        comp->setSelected (s == selected);
        stripsContainer->addAndMakeVisible (comp);
    }

    stripViewport.setViewPosition (0, 0);
    layerBar.refresh();
    layersPanel.refresh();

    masterStrip = std::make_unique<StripComponent> (engine.getMaster(), ctx);
    masterStrip->setNumInsertRows (insertRows);
    addAndMakeVisible (*masterStrip);

    channelCountLabel.setText (juce::String (engine.getInputs().size()), juce::dontSendNotification);

    selectStrip (selectedStillExists ? selected : nullptr, false);
    resized();
    stripsContainer->resized(); // its size may not have changed, but its children did
    showPanel.refresh();
}

void MainComponent::updateInsertRows()
{
    // Every strip shows the same number of slots (so the handles line up), enough
    // for the highest slot in use anywhere plus one free.
    int highest = -1;
    auto scan = [&highest] (const Strip& s)
    {
        for (int i = kNumInsertSlots; --i > highest;)
            if (s.inserts.getPlugin (i) != nullptr)
                highest = i;
    };

    scan (engine.getMaster());
    for (auto& s : engine.getInputs())
        scan (*s);
    for (auto& s : engine.getBuses())
        scan (*s);

    const int rows = juce::jlimit (3, kNumInsertSlots, highest + 2);
    if (rows == insertRows)
        return;

    insertRows = rows;
    for (auto* s : strips)
        s->setNumInsertRows (rows);
    if (masterStrip != nullptr)
        masterStrip->setNumInsertRows (rows);
}

void MainComponent::selectStrip (Strip* strip, bool reveal)
{
    for (auto* s : strips)
        s->setSelected (&s->getStrip() == strip);
    if (masterStrip != nullptr)
        masterStrip->setSelected (strip == &engine.getMaster());

    if (detailPanel.getStrip() != strip)
        detailPanel.setStrip (strip);

    if (strip != nullptr && reveal)
    {
        tabs.setCurrentTabIndex (0);
        setPanelVisible (true);
    }
}

void MainComponent::removeStrip (Strip& strip)
{
    // Defer: this is called from inside the strip component's own menu callback.
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this), ref = state::StripRef::of (strip)]
    {
        if (safe == nullptr)
            return;

        auto* s = state::findStrip (safe->engine, ref);
        if (s == nullptr)
            return;

        if (safe->detailPanel.getStrip() == s)
            safe->detailPanel.setStrip (nullptr);

        safe->closePluginWindowsFor (*s);
        const bool wasBus = s->isBus();
        safe->log.info ("Removed " + s->name);
        safe->engine.removeStrip (s);
        safe->rebuildStrips();

        if (wasBus)
            safe->detailPanel.refresh(); // sends list changed
    });
}

void MainComponent::setChannelCount (int count)
{
    count = juce::jlimit (0, 4096, count);

    for (size_t i = static_cast<size_t> (count); i < engine.getInputs().size(); ++i)
    {
        closePluginWindowsFor (*engine.getInputs()[i]);
        if (engine.getInputs()[i].get() == detailPanel.getStrip())
            detailPanel.setStrip (nullptr);
    }

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

void MainComponent::setPanelVisible (bool shouldBeVisible)
{
    if (panelVisible == shouldBeVisible)
        return;

    panelVisible = shouldBeVisible;
    tabs.setVisible (panelVisible);
    resized();
}

//==============================================================================
void MainComponent::recall (state::ApplyResult (Show::*fn)())
{
    if (show.getNumScenes() == 0)
        return;

    const auto result = (show.*fn)();
    const int index = show.getCurrentSceneIndex();
    if (auto* scene = show.getScene (index))
        logRecall (ctx, "scene " + juce::String (index + 1) + " \"" + scene->name + "\"", result);
}

void MainComponent::updateSceneControls()
{
    const int index = show.getCurrentSceneIndex();
    auto* scene = show.getScene (index);

    sceneLabel.setText (scene != nullptr ? juce::String (index + 1).paddedLeft ('0', 2) + "  " + scene->name
                                         : (show.getNumScenes() > 0 ? juce::String ("No scene recalled") : juce::String ("No scenes")),
                        juce::dontSendNotification);

    if (auto* next = show.getScene (index + 1))
        goButton.setTooltip ("Recall next scene: " + next->name);
    else
        goButton.setTooltip ("No next scene");

    prevSceneButton.setEnabled (index > 0);
    goButton.setEnabled (index + 1 < show.getNumScenes());
    updateTitle();
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

    for (auto* button : { &audioButton, &pluginsButton, &showButton })
    {
        button->setBounds (bar.removeFromLeft (button == &audioButton ? 96 : 70));
        bar.removeFromLeft (6);
    }
    bar.removeFromLeft (10);
    channelsCaption.setBounds (bar.removeFromLeft (62));
    fewerButton.setBounds (bar.removeFromLeft (26));
    channelCountLabel.setBounds (bar.removeFromLeft (46).reduced (2, 0));
    moreButton.setBounds (bar.removeFromLeft (26));
    bar.removeFromLeft (10);
    addAuxButton.setBounds (bar.removeFromLeft (62));
    bar.removeFromLeft (6);
    addGroupButton.setBounds (bar.removeFromLeft (72));
    bar.removeFromLeft (16);
    prevSceneButton.setBounds (bar.removeFromLeft (30));
    bar.removeFromLeft (4);
    sceneLabel.setBounds (bar.removeFromLeft (220));
    bar.removeFromLeft (4);
    goButton.setBounds (bar.removeFromLeft (64));
    bar.removeFromLeft (10);
    panelButton.setBounds (bar.removeFromRight (60));
    bar.removeFromRight (8);
    statusLabel.setBounds (bar);

    if (panelVisible)
        tabs.setBounds (b.removeFromBottom (juce::jlimit (330, 400, getHeight() * 38 / 100)));

    auto console = b.reduced (4);
    layerBar.setBounds (console.removeFromLeft (LayerBar::kWidth));
    console.removeFromLeft (6);
    if (masterStrip != nullptr)
        masterStrip->setBounds (console.removeFromRight (StripComponent::kWidth));
    console.removeFromRight (8);

    stripViewport.setBounds (console);
    stripsContainer->setSize (juce::jmax (console.getWidth(), stripsContainer->getPreferredWidth()),
                              console.getHeight() - stripViewport.getScrollBarThickness());
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    // Ctrl/Cmd + 1..9 selects a layer, like the layer keys on a console.
    if (key.getModifiers().isCommandDown())
    {
        const auto c = key.getTextCharacter();
        const int index = (c >= '1' && c <= '9') ? c - '1' : (key.getKeyCode() >= '1' && key.getKeyCode() <= '9' ? key.getKeyCode() - '1' : -1);

        if (index >= 0)
        {
            const auto layers = show.layers.getAllLayers (engine);
            if (index < static_cast<int> (layers.size()))
            {
                show.layers.selectedLayerId = layers[(size_t) index].id;
                rebuildStrips();
            }
            return true;
        }
    }

    return false;
}

void MainComponent::timerCallback()
{
    updateInsertRows();

    juce::String status;

    if (auto* device = deviceManager.getCurrentAudioDevice())
        status << device->getTypeName() << "  " << juce::String (device->getCurrentSampleRate() / 1000.0, 1) << " kHz / "
               << device->getCurrentBufferSizeSamples() << "  |  ";
    else
        status << "NO AUDIO DEVICE  |  ";

    status << "DSP " << juce::roundToInt (engine.getDspLoad() * 100.0f) << "%";

    const int faults = engine.getNumFaultedPlugins();
    if (faults > 0)
        status << "  |  " << faults << " PLUGIN FAULT" << (faults > 1 ? "S" : "");

    if (engine.anySoloActive.load())
        status << "  |  SOLO";

    statusLabel.setText (status, juce::dontSendNotification);

    const bool alarm = engine.getDspLoad() > 0.85f || faults > 0 || deviceManager.getCurrentAudioDevice() == nullptr;
    statusLabel.setColour (juce::Label::textColourId,
                           alarm ? Colours::mute : (engine.anySoloActive.load() ? Colours::solo : Colours::textDim));
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
        ScannerWindow (PluginHost& h, MainComponent& o)
            : juce::DocumentWindow ("Plugins (VST3 / AU / LV2) - scanned in a separate process", Colours::panel, closeButton),
              owner (o)
        {
            setUsingNativeTitleBar (true);
            auto* list = new juce::PluginListComponent (h.getFormatManager(), h.getKnownPlugins(),
                                                        h.getDeadMansPedalFile(), h.getSettings(), true);
            list->setSize (720, 500);
            setContentOwned (list, true);
            setResizable (true, false);
            centreWithSize (720, 500);
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            juce::MessageManager::callAsync ([o = juce::Component::SafePointer<MainComponent> (&owner)]
            {
                if (o != nullptr)
                    o->scannerWindow.reset();
            });
        }

    private:
        MainComponent& owner;
    };

    scannerWindow = std::make_unique<ScannerWindow> (ctx.host, *this);
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

void MainComponent::closeAllPluginWindows()
{
    pluginWindows.clear();
}

//==============================================================================
void MainComponent::showShowMenu()
{
    juce::PopupMenu menu;
    menu.addItem ("New show", [this] { newShow(); });
    menu.addItem ("Open show...", [this] { openShow(); });
    menu.addItem ("Save show", [this] { saveShow (false); });
    menu.addItem ("Save show as...", [this] { saveShow (true); });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (showButton));
}

void MainComponent::newShow()
{
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "New show",
                                        "Clear the console and all scenes and start a new show?", "New", "Cancel", this,
                                        juce::ModalCallbackFunction::create ([this] (int result)
    {
        if (result == 0)
            return;

        closeAllPluginWindows();
        detailPanel.setStrip (nullptr);
        show.clear (16);
        showFile = juce::File();
        rebuildStrips();
        updateTitle();
        log.info ("New show");
    }));
}

juce::Result MainComponent::reloadShow (const juce::File& file, bool loadPlugins)
{
    closeAllPluginWindows();
    detailPanel.setStrip (nullptr);
    const auto result = show.loadFromFile (file, loadPlugins);
    rebuildStrips();
    updateSceneControls();
    return result;
}

void MainComponent::openShow()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Open show", showFile, "*.lshow;*.livedaw");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File())
            return;

        const auto result = reloadShow (file, true);
        showFile = file.hasFileExtension (Show::fileExtension) ? file : juce::File();
        updateTitle();
        log.info ("Opened show " + file.getFileName());

        if (result.failed())
        {
            log.warning (result.getErrorMessage());
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Open show",
                                                    result.getErrorMessage());
        }
    });
}

void MainComponent::saveShow (bool saveAs)
{
    auto writeTo = [this] (juce::File file)
    {
        file = file.withFileExtension (Show::fileExtension);
        if (show.name == "Untitled show" || show.name == "Imported session")
            show.name = file.getFileNameWithoutExtension();

        const auto result = show.saveToFile (file);

        if (result.failed())
        {
            log.error ("Save failed: " + result.getErrorMessage());
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save show",
                                                    result.getErrorMessage());
            return;
        }

        showFile = file;
        log.info ("Saved show " + file.getFileName());
        updateTitle();
        showPanel.refresh();
    };

    if (! saveAs && showFile != juce::File())
    {
        writeTo (showFile);
        return;
    }

    fileChooser = std::make_unique<juce::FileChooser> ("Save show", showFile, "*.lshow");
    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [writeTo] (const juce::FileChooser& fc)
    {
        if (fc.getResult() != juce::File())
            writeTo (fc.getResult());
    });
}

void MainComponent::updateTitle()
{
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
        window->setName ("Live DAW - " + show.name + (showFile != juce::File() ? " (" + showFile.getFileName() + ")" : juce::String()));
}

} // namespace livedaw::ui
