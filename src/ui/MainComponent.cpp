#include "ui/MainComponent.h"
#include "ui/MidiRemoteUi.h"
#include "ui/WingPopups.h"
#include "ui/daw/DawEditor.h"

#include <set>

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
/** Surface lock: covers the surface (everything but the main display), ignores
    its input and shows a lock. */
class MainComponent::LockOverlay final : public juce::Component
{
public:
    explicit LockOverlay (MainComponent& o) : owner (o) {}

    bool hitTest (int x, int y) override { return ! owner.display->getBounds().contains (x, y); }

    void paint (juce::Graphics& g) override
    {
        g.excludeClipRegion (owner.display->getBounds());
        g.fillAll (juce::Colours::black.withAlpha (0.45f));
        const auto faders = owner.sections[0]->getBounds().getUnion (owner.sections[LayerSet::kNumSections - 1]->getBounds());
        auto badge = juce::Rectangle<int> (300, 64).withCentre (faders.getCentre());
        g.setColour (Colours::screenPanel.withAlpha (0.95f));
        g.fillRoundedRectangle (badge.toFloat(), 6.0f);
        drawIcon (g, Icon::LockClosed, badge.removeFromLeft (64).reduced (14).toFloat(), Colours::yellow);
        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        g.drawText ("SURFACE LOCKED  (Ctrl+L)", badge, juce::Justification::centredLeft);
    }

private:
    MainComponent& owner;
};

//==============================================================================
/** SOLO SYSTEM (SETUP > AUDIO): EXCLUSIVE SOLO and SELECT FOLLOWS SOLO act on
    every new solo, wherever it was switched on (surface, screens, custom
    controls), so no control needs to know about them. */
class MainComponent::SoloWatcher final : private juce::Timer
{
public:
    explicit SoloWatcher (MainComponent& o) : owner (o)
    {
        snapshot();
        startTimerHz (30);
    }

    /** Takes the solos as they are now as known (after the owner changed them). */
    void snapshot() { known = current(); }

    /** Clears every solo but `keep` (a strip handle or DCA, nullptr for none). */
    void clearAllBut (const void* keep)
    {
        auto& engine = owner.engine;
        for (int k = 0; k < kNumStripKinds; ++k)
            for (auto& s : engine.getStrips (static_cast<StripKind> (k)))
                if (s->getHandle() != keep)
                    s->params.solo = false;
        for (int i = 0; i < kNumDcas; ++i)
            if (&engine.getDca (i) != keep)
                engine.getDca (i).solo = false;
    }

private:
    std::set<const void*> current() const
    {
        std::set<const void*> soloed;
        auto& engine = owner.engine;
        for (int k = 0; k < kNumStripKinds; ++k)
            for (auto& s : engine.getStrips (static_cast<StripKind> (k)))
                if (s->params.solo.load())
                    soloed.insert (s->getHandle());
        for (int i = 0; i < kNumDcas; ++i)
            if (engine.getDca (i).solo.load())
                soloed.insert (&engine.getDca (i));
        return soloed;
    }

    Strip* stripOf (const void* key) const
    {
        for (int k = 0; k < kNumStripKinds; ++k)
            for (auto& s : owner.engine.getStrips (static_cast<StripKind> (k)))
                if (s->getHandle() == key)
                    return s.get();
        return nullptr;
    }

    void timerCallback() override
    {
        const auto now = current();
        const void* added = nullptr;
        for (auto* k : now)
            if (known.count (k) == 0)
                added = k;
        known = now;
        if (added == nullptr)
            return;
        if (owner.settings.getBoolValue (uiprefs::exclusiveSolo, false))
        {
            clearAllBut (added);
            known = { added };
        }
        if (owner.settings.getBoolValue (uiprefs::selectFollowsSolo, false))
            if (auto* s = stripOf (added); s != nullptr && s != owner.getSelectedStrip())
                owner.selectStrip (s);
    }

    MainComponent& owner;
    std::set<const void*> known;
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
    ctx.sofChanged = [this]
    {
        // BUS SoF ACTIVATES SOLO: entering Sends on Fader solos the destination.
        if (auto* sel = getSelectedStrip(); sel != nullptr && ctx.sendsOnFader && sel->isOutput() && settings.getBoolValue (uiprefs::sofActivatesSolo, false))
            sel->params.solo = true;
        for (auto& sec : sections)
            sec->refreshFaders();
    };
    ctx.getScreen = [this] { return display != nullptr ? display->getScreen() : Screen::Home; };
    ctx.revealFader = [this] (LayerSet::Section section, const state::FaderRef& f) { sections[section]->reveal (f); };
    ctx.takeScreenshot = [this] { saveScreenshot(); };
    ctx.setSurfaceLocked = [this] (bool locked) { setSurfaceLocked (locked); };
    ctx.isSurfaceLocked = [this] { return lockOverlay != nullptr; };
    ctx.newShow = [this] { newShow(); };
    ctx.openShow = [this] { openShow(); };
    ctx.saveShow = [this] (bool saveAs) { saveShow (saveAs); };
    ctx.openShowFile = [this] (const juce::File& file) { openShowFile (file); };
    ctx.saveShowTo = [this] (const juce::File& file) { saveShowTo (file); };

    // The recording session (the recorder screen, the editor and MULTI TRANSP share it).
    recording = std::make_unique<daw::Session> (engine.getRecorder(), &settings);
    recording->onMessage = [this] (const juce::String& text, bool error)
    {
        if (error)
            log.warning (text);
        else
            log.info (text);
    };
    ctx.recording = recording.get();
    ctx.showEditor = [this] (bool shown) { showEditor (shown); };
    ctx.isEditorShown = [this] { return editorShown; };

    // The custom control assignments live in the show (CC EDIT edits them).
    customControlAssignments = std::make_unique<cc::Assignments> (show);
    ctx.customControls = customControlAssignments.get();
    ctx.customControlsChanged = [this]
    {
        if (customControls != nullptr)
            customControls->refresh();
    };

    show.onChanged = [this]
    {
        if (customControls != nullptr)
            customControls->refresh();
        if (display != nullptr)
            display->refreshLibrary();
    };
    show.onFxCleared = [this] (int slot)
    {
        if (auto* p = engine.getFx().getPlugin (slot))
            closePluginWindowsFor (p);
    };

    // MIDI remote control and DAW remote control (SETUP > REMOTE and DAW).
    midiRemote = std::make_unique<MidiRemote> (engine, show);
    ctx.midi = midiRemote.get();
    midiui::connect (ctx);

    channelStrip = std::make_unique<ChannelStripSection> (ctx);
    display = std::make_unique<MainDisplay> (ctx);
    monitor = std::make_unique<MonitorSection> (ctx);
    for (int i = 0; i < LayerSet::kNumSections; ++i)
        sections[i] = std::make_unique<FaderSection> (ctx, static_cast<LayerSet::Section> (i));
    customControls = std::make_unique<CustomControlSection> (ctx);
    // The 4-Channel section works on the right fader section's four strips.
    fourChannel = std::make_unique<FourChannelSection> (ctx, [this] (int i) { return sections[LayerSet::Right] != nullptr ? sections[LayerSet::Right]->getStrip (i) : nullptr; });
    soloWatcher = std::make_unique<SoloWatcher> (*this);
    for (juce::Component* c : { (juce::Component*) channelStrip.get(), (juce::Component*) display.get(), (juce::Component*) monitor.get(),
                                (juce::Component*) customControls.get(), (juce::Component*) fourChannel.get() })
        addAndMakeVisible (c);
    for (auto& sec : sections)
        addAndMakeVisible (*sec);
    // DAW mode: the DAW's strips over the fader sections (they show themselves).
    for (int i = 0; i < LayerSet::kNumSections; ++i)
    {
        dawSections[i] = std::make_unique<DawFaderSection> (ctx, static_cast<LayerSet::Section> (i));
        addChildComponent (*dawSections[i]);
    }

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
    openLastProject();
}

MainComponent::~MainComponent()
{
    // MIDI goes first: its callbacks reach the views.
    ctx.midi = nullptr;
    midiRemote.reset();

    // Unsaved project changes survive the quit in the autosave (offered on the next open).
    recording->autosave();
    soloWatcher.reset();
    editor.reset();
    show.onChanged = nullptr;
    show.onFxCleared = nullptr;
    lockOverlay.reset();
    closeAllPluginWindows();
    scannerWindow.reset();
    fourChannel.reset();
    for (auto& sec : dawSections)
        sec.reset();
    for (auto& sec : sections)
        sec.reset();
    customControls.reset();
    monitor.reset();
    display.reset();
    channelStrip.reset();
    ctx.recording = nullptr;
    recording.reset();
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

    // SOLO FOLLOWS SELECT: while anything is soloed, the solo moves to the selection.
    if (strip != nullptr && soloWatcher != nullptr && settings.getBoolValue (uiprefs::soloFollowsSelect, false) && engine.anySoloActive.load()
        && ! strip->params.solo.load())
    {
        soloWatcher->clearAllBut (nullptr);
        strip->params.solo = true;
        soloWatcher->snapshot();
    }

    channelStrip->refresh();
    display->refresh();
    if (ctx.sendsOnFader)
        for (auto& sec : sections)
            sec->refreshFaders();
}

void MainComponent::refreshAll()
{
    // A loaded or new show brings its own custom controls.
    customControlAssignments->reload();

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
    customControls->refresh();
}

void MainComponent::refreshSections()
{
    layoutSections();
    for (auto& sec : sections)
        sec->refresh();
    for (int s = 0; s < LayerSet::kNumSections; ++s)
        if (dawSections[s] != nullptr)
            dawSections[s]->setNumStrips (sections[s]->getNumStrips());
    fourChannel->refresh();
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

    // The custom controls section (between center and right) is about four
    // strips wide plus a key column. When the window is too narrow for usable
    // strips, its lower part moves under the monitoring panel instead.
    const int total = b.getWidth();
    const int banks = LayerSet::kNumSections * FaderSection::kBankColumnWidth;
    const int strips = juce::jmax (1, fixedStrips + autoCount * 12);
    constexpr int ccStrips = 4, minStripWithCc = 50;
    int stripW = (total - banks - FaderSection::kBankColumnWidth) / (strips + ccStrips);
    const bool withCc = stripW >= minStripWithCc;
    if (! withCc)
        stripW = (total - banks) / strips;
    stripW = juce::jlimit (40, 120, stripW);
    const int ccW = withCc ? FaderSection::kBankColumnWidth + ccStrips * stripW : 0;
    if (withCc != ccInBottomRow)
    {
        ccInBottomRow = withCc;
        customControls->setCompact (! withCc);
        layoutTopRow();
    }

    int x = 0;
    int remaining = total - ccW;
    for (int s = 0; s < LayerSet::kNumSections; ++s)
        if (widths[s] != LayerSet::kAutoWidth)
            remaining -= FaderSection::widthFor (widths[s], stripW);

    for (int s = 0; s < LayerSet::kNumSections; ++s)
    {
        if (s == LayerSet::Right && withCc)
        {
            customControls->setBounds (x, b.getY(), ccW, b.getHeight());
            x += ccW;
        }

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
    for (int s = 0; s < LayerSet::kNumSections; ++s)
        if (dawSections[s] != nullptr)
        {
            dawSections[s]->setBounds (sections[s]->getBounds());
            dawSections[s]->setNumStrips (sections[s]->getNumStrips());
        }
}

void MainComponent::layoutTopRow()
{
    auto b = getLocalBounds();
    const int topH = juce::jlimit (300, juce::jmax (300, b.getHeight() - 260), juce::roundToInt ((float) b.getHeight() * topFraction));
    auto top = b.removeFromTop (topH);
    splitter.setBounds (b.removeFromTop (6));

    // Channel strip section about 29%, the right column about 22%, the main
    // display (keys, screen and its right-hand column) in between. The right
    // column holds the monitoring panel over the 4-Channel section (which works
    // on the right fader section below it); narrow windows put the lower part
    // of the custom controls there instead of the 4-Channel section.
    const int stripW = juce::jlimit (330, 620, top.getWidth() * 29 / 100);
    const int monitorW = juce::jlimit (260, 460, top.getWidth() * 22 / 100);
    channelStrip->setBounds (top.removeFromLeft (stripW));
    auto right = top.removeFromRight (monitorW);
    fourChannel->setVisible (ccInBottomRow && ! editorShown);
    if (! ccInBottomRow)
        customControls->setBounds (right.removeFromBottom (right.getHeight() * 45 / 100));
    else
        fourChannel->setBounds (right.removeFromBottom (right.getHeight() * 54 / 100));
    monitor->setBounds (right);
    display->setBounds (top);
}

void MainComponent::resized()
{
    if (editor != nullptr)
        editor->setBounds (getLocalBounds());
    layoutTopRow();

    // Auto-width sections may now hold a different number of faders.
    int before[LayerSet::kNumSections];
    for (int s = 0; s < LayerSet::kNumSections; ++s)
        before[s] = show.layers.getEffectiveWidth (static_cast<LayerSet::Section> (s));
    layoutSections();
    for (int s = 0; s < LayerSet::kNumSections; ++s)
        if (show.layers.getEffectiveWidth (static_cast<LayerSet::Section> (s)) != before[s])
            sections[s]->refresh();

    if (lockOverlay != nullptr)
        lockOverlay->setBounds (getLocalBounds());
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    if (key.getModifiers().isCommandDown())
    {
        // With Ctrl held, X11 gives a control code as the text and the lowercase key as the code.
        const auto c = juce::CharacterFunctions::toLowerCase (key.getTextCharacter());
        const auto k = juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) key.getKeyCode());
        if (c == 'e' || k == 'e')
        {
            showEditor (! editorShown);
            return true;
        }
        if (c == 's' || k == 's')
        {
            saveShow (key.getModifiers().isShiftDown());
            return true;
        }
        if (c == 'o' || k == 'o')
        {
            openShow();
            return true;
        }
        if (c == 'g' || k == 'g')
        {
            logRecall (ctx, "next scene", show.recallNext());
            return true;
        }
        if (c == 'p' || k == 'p')
        {
            saveScreenshot();
            return true;
        }
        if (c == 'l' || k == 'l')
        {
            setSurfaceLocked (lockOverlay == nullptr);
            return true;
        }
    }
    return false;
}

void MainComponent::saveScreenshot()
{
    const auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Live DAW").getChildFile ("screens");
    const auto file = dir.getChildFile ("screen-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".png");
    const auto image = display->createComponentSnapshot (display->getLocalBounds());

    juce::PNGImageFormat png;
    bool ok = dir.createDirectory().wasOk();
    if (ok)
    {
        file.deleteFile();
        juce::FileOutputStream out (file);
        ok = out.openedOk() && png.writeImageToStream (image, out);
    }
    if (ok)
        log.info ("Screenshot saved to " + file.getFullPathName());
    else
        log.warning ("Couldn't save the screenshot to " + file.getFullPathName());
}

void MainComponent::setSurfaceLocked (bool locked)
{
    if (locked == (lockOverlay != nullptr))
        return;
    if (locked)
    {
        lockOverlay = std::make_unique<LockOverlay> (*this);
        addAndMakeVisible (*lockOverlay);
        lockOverlay->setBounds (getLocalBounds());
    }
    else
    {
        lockOverlay.reset();
    }
    log.info (locked ? "Surface locked" : "Surface unlocked");
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
void MainComponent::showEditor (bool shown)
{
    if (shown && editor == nullptr)
    {
        editor = std::make_unique<DawEditor> (ctx, *recording);
        editor->onClose = [this] { juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)] { if (safe != nullptr) safe->showEditor (false); }); };
        addChildComponent (*editor);
        editor->setBounds (getLocalBounds());
    }
    editorShown = shown;
    // The console's parts stay alive (and keep their state) under the editor.
    for (juce::Component* c : { (juce::Component*) channelStrip.get(), (juce::Component*) display.get(), (juce::Component*) monitor.get(),
                                (juce::Component*) customControls.get(), (juce::Component*) &splitter })
        c->setVisible (! shown);
    fourChannel->setVisible (! shown && ccInBottomRow);
    for (auto& sec : sections)
        sec->setVisible (! shown);
    if (lockOverlay != nullptr)
        lockOverlay->setVisible (! shown);
    if (editor != nullptr)
    {
        editor->setVisible (shown);
        if (shown)
        {
            editor->toFront (true);
            editor->grabKeyboardFocus();
        }
    }
    if (! shown)
        grabKeyboardFocus();
}

void MainComponent::openLastProject()
{
    const auto recent = recording->getRecentProjects();
    const juce::File last = recent.isEmpty() ? juce::File() : juce::File (recent[0]);
    if (! last.existsAsFile())
        return;
    if (! daw::Session::hasNewerAutosave (last))
    {
        if (auto r = recording->openProject (last); r.failed())
            log.warning ("Recording project: " + r.getErrorMessage());
        return;
    }
    // The saved version opens now; RESTORE reopens it with the autosave.
    if (auto r = recording->openProject (last); r.failed())
        log.warning ("Recording project: " + r.getErrorMessage());
    popups::confirm (*display, "RECORDING PROJECT",
                     "\"" + last.getFileNameWithoutExtension() + "\" has unsaved changes from the last session. Restore them?", "RESTORE",
                     [this, last]
                     {
                         if (auto result = recording->openProject (last, true); result.failed())
                             log.warning ("Recording project: " + result.getErrorMessage());
                     });
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
    popups::confirm (*display, "NEW SHOW", "Clear the console and all scenes and start a new show (a 40 channel WING)?", "NEW", [this]
    {
        closeAllPluginWindows();
        for (int i = 0; i < kNumFx; ++i)
            engine.getFx().setPlugin (i, nullptr);
        show.clear (kWingChannels);
        showFile = juce::File();
        hasSelection = false;
        refreshAll();
        updateTitle();
        log.info ("New show");
    }, true);
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
        if (const auto file = fc.getResult(); file != juce::File())
            openShowFile (file);
    });
}

void MainComponent::openShowFile (const juce::File& file)
{
    const auto result = reloadShow (file, true);
    showFile = file.hasFileExtension (Show::fileExtension) ? file : juce::File();
    updateTitle();
    log.info ("Opened show " + file.getFileName());

    if (result.failed())
    {
        log.warning (result.getErrorMessage());
        popups::warn (*display, "OPEN SHOW", result.getErrorMessage());
    }
}

void MainComponent::saveShowTo (juce::File file)
{
    file = file.withFileExtension (Show::fileExtension);
    if (show.getName() == "Untitled show" || show.getName() == "Imported session")
        show.setName (file.getFileNameWithoutExtension());

    const auto result = show.saveToFile (file);

    if (result.failed())
    {
        log.error ("Save failed: " + result.getErrorMessage());
        popups::warn (*display, "SAVE SHOW", result.getErrorMessage());
        return;
    }

    showFile = file;
    log.info ("Saved show " + file.getFileName());
    updateTitle();
    display->refreshLibrary();
}

void MainComponent::saveShow (bool saveAs)
{
    auto writeTo = [this] (const juce::File& file) { saveShowTo (file); };

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
