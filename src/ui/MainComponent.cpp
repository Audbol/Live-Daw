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
MainComponent::MainComponent (juce::AudioDeviceManager& dm, MixerEngine& e, PluginHost& h, Show& s,
                              EventLog& l, juce::PropertiesFile& props)
    : deviceManager (dm),
      engine (e),
      show (s),
      log (l),
      settings (props),
      meterBank (e),
      ctx { e, h, s, l, dm, props, meterBank }
{
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    setLookAndFeel (&lookAndFeel);

    ctx.selectStrip = [this] (Strip* strip) { selectStrip (strip); };
    ctx.getSelectedStrip = [this] { return getSelectedStrip(); };
    ctx.showScreen = [this] (Screen screen) { display->showScreen (screen); };
    ctx.showHomeTab = [this] (HomeTab tab) { display->showHomeTab (tab); };
    ctx.openPluginEditor = [this] (int slot) { openPluginEditor (slot); };
    ctx.editFx = [this] (int slot) { display->editFx (slot); };
    ctx.pluginRemoved = [this] (juce::AudioProcessor* p) { closePluginWindowsFor (p); };
    ctx.showPluginScanner = [this] { showPluginScanner(); };
    ctx.structureChanged = [this] { refreshAll(); };
    ctx.layersChanged = [this] { refreshSections(); };
    ctx.sofChanged = [this] { for (auto& sec : sections) sec->refreshFaders(); };
    ctx.newShow = [this] { newShow(); };
    ctx.openShow = [this] { openShow(); };
    ctx.saveShow = [this] (bool saveAs) { saveShow (saveAs); };

    show.onChanged = [this]
    {
        if (monitor != nullptr)
            monitor->refresh();
        if (display != nullptr)
            display->refreshLibrary();
    };
    show.onFxCleared = [this] (int slot)
    {
        if (auto* p = engine.getFx().getPlugin (slot))
            closePluginWindowsFor (p);
    };

    channelStrip = std::make_unique<ChannelStripSection> (ctx);
    display = std::make_unique<MainDisplay> (ctx);
    monitor = std::make_unique<MonitorSection> (ctx);
    sections[0] = std::make_unique<FaderSection> (ctx, LayerSet::Left, "INPUTS");
    sections[1] = std::make_unique<FaderSection> (ctx, LayerSet::Center, "BUS / DCA");
    sections[2] = std::make_unique<FaderSection> (ctx, LayerSet::Right, "MAIN / MTX");
    for (juce::Component* c : { (juce::Component*) channelStrip.get(), (juce::Component*) display.get(), (juce::Component*) monitor.get() })
        addAndMakeVisible (c);
    for (auto& sec : sections)
        addAndMakeVisible (*sec);

    splitter.setTooltip ("Drag to resize the screen and the fader sections");
    splitter.onDrag = [this] (int delta)
    {
        topFraction = juce::jlimit (0.3f, 0.7f, topFraction + (float) delta / (float) juce::jmax (1, getHeight()));
        resized();
    };
    splitter.onDragEnd = [this] { settings.setValue ("ui.topFraction", topFraction); };
    splitter.onDoubleClick = [this] { topFraction = 0.52f; resized(); };
    addAndMakeVisible (splitter);
    topFraction = (float) settings.getDoubleValue ("ui.topFraction", 0.52);

    if (! engine.getChannels().empty())
        selectStrip (engine.getChannels()[0].get());

    setWantsKeyboardFocus (true);
    setSize (1680, 1000);
    refreshAll();
    updateTitle();
}

MainComponent::~MainComponent()
{
    show.onChanged = nullptr;
    show.onFxCleared = nullptr;
    closeAllPluginWindows();
    scannerWindow.reset();
    for (auto& sec : sections)
        sec.reset();
    monitor.reset();
    display.reset();
    channelStrip.reset();
    setLookAndFeel (nullptr);
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}

//==============================================================================
Strip* MainComponent::getSelectedStrip() const
{
    return hasSelection ? state::findStrip (engine, selected) : nullptr;
}

void MainComponent::selectStrip (Strip* strip)
{
    hasSelection = strip != nullptr;
    if (strip != nullptr)
        selected = state::StripRef::of (*strip);

    channelStrip->refresh();
    display->refresh();
    if (ctx.sendsOnFader)
        for (auto& sec : sections)
            sec->refreshFaders();
}

void MainComponent::refreshAll()
{
    // Keep a valid selection after structure changes and show loads.
    if (getSelectedStrip() == nullptr)
    {
        hasSelection = false;
        if (! engine.getChannels().empty())
        {
            selected = state::StripRef::of (*engine.getChannels()[0]);
            hasSelection = true;
        }
    }

    refreshSections();
    channelStrip->refresh();
    display->refresh();
    display->refreshLibrary();
    monitor->refresh();
}

void MainComponent::refreshSections()
{
    layoutSections();
    for (auto& sec : sections)
        sec->refresh();
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);
}

void MainComponent::layoutSections()
{
    auto b = getLocalBounds();
    const int topH = juce::jlimit (300, juce::jmax (300, b.getHeight() - 260), juce::roundToInt ((float) b.getHeight() * topFraction));
    b.removeFromTop (topH + 6);

    auto& layers = show.layers;
    int widths[LayerSet::kNumSections];
    int fixedStrips = 0, autoCount = 0;
    for (int s = 0; s < LayerSet::kNumSections; ++s)
    {
        widths[s] = layers.getWidth (static_cast<LayerSet::Section> (s));
        if (widths[s] == LayerSet::kAutoWidth)
            ++autoCount;
        else
            fixedStrips += widths[s];
    }

    const int total = b.getWidth();
    const int banks = LayerSet::kNumSections * FaderSection::kBankColumnWidth;
    const int stripW = juce::jlimit (40, 120, (total - banks) / juce::jmax (1, fixedStrips + autoCount * 12));

    int x = 0;
    int remaining = total;
    for (int s = 0; s < LayerSet::kNumSections; ++s)
        if (widths[s] != LayerSet::kAutoWidth)
            remaining -= FaderSection::widthFor (widths[s], stripW);

    for (int s = 0; s < LayerSet::kNumSections; ++s)
    {
        int w;
        if (widths[s] == LayerSet::kAutoWidth)
        {
            w = juce::jmax (FaderSection::kBankColumnWidth + FaderSection::kMinStripWidth, remaining / juce::jmax (1, autoCount));
            layers.setFittedWidth (static_cast<LayerSet::Section> (s), FaderSection::fitStrips (w));
        }
        else
        {
            w = FaderSection::widthFor (widths[s], stripW);
        }
        if (s == LayerSet::kNumSections - 1)
            w = juce::jmax (w, total - x);
        sections[s]->setBounds (x, b.getY(), w, b.getHeight());
        x += w;
    }
}

void MainComponent::resized()
{
    auto b = getLocalBounds();
    const int topH = juce::jlimit (300, juce::jmax (300, b.getHeight() - 260), juce::roundToInt ((float) b.getHeight() * topFraction));
    auto top = b.removeFromTop (topH);
    splitter.setBounds (b.removeFromTop (6));

    const int stripW = juce::jlimit (300, 520, top.getWidth() * 25 / 100);
    const int monitorW = juce::jlimit (240, 400, top.getWidth() * 18 / 100);
    channelStrip->setBounds (top.removeFromLeft (stripW));
    monitor->setBounds (top.removeFromRight (monitorW));
    display->setBounds (top);

    // Auto-width sections may now hold a different number of faders.
    int before[LayerSet::kNumSections];
    for (int s = 0; s < LayerSet::kNumSections; ++s)
        before[s] = show.layers.getEffectiveWidth (static_cast<LayerSet::Section> (s));
    layoutSections();
    for (int s = 0; s < LayerSet::kNumSections; ++s)
        if (show.layers.getEffectiveWidth (static_cast<LayerSet::Section> (s)) != before[s])
            sections[s]->refresh();
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    if (key.getModifiers().isCommandDown())
    {
        const auto c = juce::CharacterFunctions::toLowerCase (key.getTextCharacter());
        if (c == 's' || key.getKeyCode() == 'S')
        {
            saveShow (key.getModifiers().isShiftDown());
            return true;
        }
        if (c == 'o' || key.getKeyCode() == 'O')
        {
            openShow();
            return true;
        }
        if (c == 'g' || key.getKeyCode() == 'G')
        {
            logRecall (ctx, "next scene", show.recallNext());
            return true;
        }
    }
    return false;
}

//==============================================================================
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

void MainComponent::openPluginEditor (int slot)
{
    auto* plugin = engine.getFx().getPlugin (slot);
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

    pluginWindows.add (new PluginWindow (*plugin, "FX " + juce::String (slot + 1) + ": " + plugin->getName(), *this));
}

void MainComponent::closePluginWindowsFor (juce::AudioProcessor* p)
{
    for (int i = pluginWindows.size(); --i >= 0;)
        if (&pluginWindows[i]->processor == p)
            pluginWindows.remove (i);
}

void MainComponent::closeAllPluginWindows()
{
    pluginWindows.clear();
}

//==============================================================================
void MainComponent::updateTitle()
{
    juce::String title = "Live DAW - " + show.getName();
    if (showFile != juce::File())
        title << "  [" << showFile.getFileName() << "]";
    if (auto* top = getTopLevelComponent(); top != nullptr && top != this)
        top->setName (title);
    else
        setName (title);
}

void MainComponent::newShow()
{
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "New show",
                                        "Clear the console and all scenes and start a new show (a 40 channel WING)?",
                                        "New", "Cancel", this,
                                        juce::ModalCallbackFunction::create ([this] (int result)
    {
        if (result == 0)
            return;

        closeAllPluginWindows();
        for (int i = 0; i < kNumFx; ++i)
            engine.getFx().setPlugin (i, nullptr);
        show.clear (kWingChannels);
        showFile = juce::File();
        hasSelection = false;
        refreshAll();
        updateTitle();
        log.info ("New show");
    }));
}

juce::Result MainComponent::reloadShow (const juce::File& file, bool loadPlugins)
{
    closeAllPluginWindows();
    const auto result = show.loadFromFile (file, loadPlugins);
    refreshAll();
    updateTitle();
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
        if (show.getName() == "Untitled show" || show.getName() == "Imported session")
            show.setName (file.getFileNameWithoutExtension());

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
        display->refreshLibrary();
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

} // namespace livedaw::ui
