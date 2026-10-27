#include "ui/MainComponent.h"
#include "engine/AppPaths.h"
#include "ui/MidiRemoteUi.h"
#include "ui/PluginChainWindow.h"
#include "ui/StartupNotice.h"
#include "ui/WebMirror.h"
#include "ui/WingPopups.h"
#include "ui/daw/DawEditor.h"

#include <set>

namespace livedaw::ui
{

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
        const auto faders = owner.sections[0]->getBounds().getUnion (owner.isCompact() ? owner.compactKeys->getBounds()
                                                                                       : owner.sections[LayerSet::kNumSections - 1]->getBounds());
        auto badge = juce::Rectangle<int> (300, 64).withCentre (faders.getCentre());
        g.setColour (Colours::screenPanel.withAlpha (0.95f));
        g.fillRoundedRectangle (badge.toFloat(), 6.0f);
        drawIcon (g, Icon::LockClosed, badge.removeFromLeft (64).reduced (14).toFloat(), Colours::yellow);
        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        g.drawText ("LOCKED: HOLD HOME", badge, juce::Justification::centredLeft);
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
        if (owner.settings.getBoolValue (uiprefs::exclusiveSolo, uiprefs::exclusiveSoloDefault))
        {
            clearAllBut (added);
            known = { added };
        }
        if (owner.settings.getBoolValue (uiprefs::selectFollowsSolo, uiprefs::selectFollowsSoloDefault))
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
    // The surface's finish and the console model (SETUP > GENERAL > CONSOLE)
    // are app settings; the COMPACT is black.
    const auto look = ConsoleLook::load (settings);
    setSurfaceFinish (look.shownFinish());
    ctx.consoleModel = look.model;

    // Plugin delay compensation (SETUP > PLUGINS) is an app setting, on by default.
    engine.setLatencyCompensation (settings.getBoolValue (uiprefs::latencyCompensation, true));
    engine.onPluginWarning = [this] (const juce::String& text) { log.warning (text); };

    ctx.selectStrip = [this] (Strip* strip) { selectStrip (strip); };
    ctx.getSelectedStrip = [this] { return getSelectedStrip(); };
    ctx.showScreen = [this] (Screen screen) { display->showScreen (screen); };
    ctx.showOverviewPage = [this] (int area, int bank) { display->showOverviewPage (cc::overviewSection (area), bank); };
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
        if (compactKeys != nullptr)
            compactKeys->refresh();
    };
    ctx.getScreen = [this] { return display != nullptr ? display->getScreen() : Screen::Home; };
    // The COMPACT's faders are the left section's.
    ctx.revealFader = [this] (LayerSet::Section section, const state::FaderRef& f) { sections[isCompact() ? LayerSet::Left : section]->reveal (f); };
    ctx.setConsoleModel = [this] (ConsoleModel m) { applyConsoleModel (m, true); };
    ctx.takeScreenshot = [this] { saveScreenshot(); };
    ctx.holdHomeLock = [this] (const juce::String& held) { holdHomeLock (held); };
    settings.setValue (uiprefs::surfaceLock, juce::String()); // a restart unlocks, as on the console
    ctx.isSurfaceLocked = [this] { return surfaceLock.isLocked(); };
    ctx.newShow = [this] { newShow(); };
    ctx.openShow = [this] { openShow(); };
    ctx.saveShow = [this] (bool saveAs) { saveShow (saveAs); };
    ctx.openShowFile = [this] (const juce::File& file) { openShowFile (file); };
    ctx.saveShowTo = [this] (const juce::File& file) { saveShowTo (file); };
    ctx.showFileRenamed = [this] (const juce::File& from, const juce::File& to)
    {
        // The open show's file: SAVE writes the new one, and the show takes its name.
        if (showFile == juce::File() || from != showFile)
            return;
        showFile = to;
        show.setName (to.getFileNameWithoutExtension());
        updateTitle();
        display->refreshLibrary();
    };

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
    ctx.setScreenMaximized = [this] (bool maximized) { setScreenMaximized (maximized); };
    ctx.isScreenMaximized = [this] { return screenMaximized; };

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

    // The web remote (SETUP > REMOTE): starts now if it is enabled.
    webMirror = std::make_unique<WebMirror> (*this, settings, log);
    ctx.web = webMirror.get();

    channelStrip = std::make_unique<ChannelStripSection> (ctx);
    display = std::make_unique<MainDisplay> (ctx);
    monitor = std::make_unique<MonitorSection> (ctx);
    for (int i = 0; i < LayerSet::kNumSections; ++i)
        sections[i] = std::make_unique<FaderSection> (ctx, static_cast<LayerSet::Section> (i));
    customControls = std::make_unique<CustomControlSection> (ctx);
    compactKeys = std::make_unique<CompactSection> (ctx);
    addChildComponent (*compactKeys);
    // The 4-Channel section works on the right fader section's four strips.
    fourChannel = std::make_unique<FourChannelSection> (ctx, [this] (int i) { return sections[LayerSet::Right] != nullptr ? sections[LayerSet::Right]->getStrip (i) : nullptr; });
    soloWatcher = std::make_unique<SoloWatcher> (*this);

    // WING remote clients recall scenes (/$ctl/lib) like MIDI does, and change
    // the layers and custom controls (/$ctl/layer, /$ctl/user) like the surface.
    engine.getWing().setSurfaceCallbacks ([this] { return customControlAssignments->currentLayer; },
                                          [this] (int changes, int bank)
                                          {
                                              if ((changes & WingRemote::surfaceControls) != 0)
                                                  customControlAssignments->reload();
                                              if (bank >= 0)
                                                  customControlAssignments->currentLayer = bank;
                                              customControls->refresh();
                                              if ((changes & WingRemote::surfaceLayers) != 0)
                                                  refreshSections();
                                          });
    engine.getWing().attachShow (&show, [this] (const juce::String& what, const state::ApplyResult& r)
                                 { logRecall (ctx, what, r); });
    // ... and SETUP's options that are WING parameters (metering, surface, solo, MIDI, DAW).
    engine.getWing().setHostSettings (&settings,
                                      [this] (const juce::StringArray& keys)
                                      {
                                          for (const auto& k : keys)
                                              if (k.startsWith ("midi.") || k.startsWith ("daw."))
                                              {
                                                  if (midiRemote != nullptr)
                                                      midiRemote->apply (MidiRemote::Settings::load (settings));
                                                  break;
                                              }
                                          if (keys.contains ("ui.selDblClick") || keys.contains ("setup.faderScreenLayerLink"))
                                              notifyLayersChangedAsync (ctx);
                                          // The COMPACT's fader 13 and 16 keys (/$ctl/cfg/mfdr, cscmode, mfdrkeep, dcacc...).
                                          if (compactKeys != nullptr)
                                              for (const auto& k : keys)
                                                  if (k.startsWith ("ui.compact"))
                                                  {
                                                      compactKeys->refresh();
                                                      break;
                                                  }
                                          repaint();
                                      });
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

    applyConsoleModel (look.model, false);
    setWantsKeyboardFocus (true);
    setSize (1680, 1000);
    refreshAll();
    updateTitle();
    openLastProject();
    // Screen only, as the last session left it.
    if (settings.getBoolValue (uiprefs::screenMaximized, false))
        setScreenMaximized (true);
}

MainComponent::~MainComponent()
{
    // The web remote stops watching the window and closes its browsers.
    ctx.web = nullptr;
    webMirror.reset();
    // MIDI and the WING remote's scene and surface control go first: their callbacks reach the views.
    engine.getWing().attachShow (nullptr);
    engine.getWing().setSurfaceCallbacks ({}, {});
    engine.getWing().setHostSettings (nullptr);
    ctx.midi = nullptr;
    midiRemote.reset();
    engine.onPluginWarning = nullptr;

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
    compactKeys.reset();
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
    {
        for (auto& sec : sections)
            sec->refreshFaders();
        if (compactKeys != nullptr)
            compactKeys->refresh();
    }
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
    if (compactKeys != nullptr)
        compactKeys->refresh();
}

void MainComponent::applyConsoleModel (ConsoleModel model, bool save)
{
    if (save)
    {
        auto look = ConsoleLook::load (settings);
        look.model = model;
        look.save (settings);
        log.info ("Console model: " + consoleModelName (model));
    }
    const bool changed = model != ctx.consoleModel;
    ctx.consoleModel = model;
    // The layers (the COMPACT's section and keys), the WING remote's identity,
    // the colours and the parts of the surface.
    show.layers.setModel (model);
    engine.getWing().setConsoleModel (model);
    engine.getWing().setBlackFinish (ConsoleLook::load (settings).finish == SurfaceFinish::Black);
    setSurfaceFinish (ConsoleLook::load (settings).shownFinish());
    monitor->setConsoleModel (model);
    if (changed && isCompact())
        ctx.sendsOnFader = false; // its SOF is the BUS and MAIN MATRIX keys'
    updateVisibility();
    refreshSections();
    resized();
    repaint();
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (screenMaximized ? juce::Colours::black : surfaceColours().surface);
}

int MainComponent::topRowHeight() const
{
    const int h = getHeight();
    // Wide windows are short for their width: past 2:1 the top row (and with
    // it the screen, the most important part) takes up to 8 % more.
    const float aspect = (float) getWidth() / (float) juce::jmax (1, h);
    const float wideBoost = juce::jlimit (0.0f, 0.08f, (aspect - 2.0f) * 0.06f);
    // Short windows (a 1366 x 768 laptop) keep the screen legible, as tall as
    // the fader sections' compact layout (meters folded away) allows.
    const int legible = juce::jlimit (300, juce::jmax (300, h - 6 - FaderStrip::kMinCompactHeight),
                                      MainDisplay::heightForScreen (MainDisplay::kLegibleScreenHeight));
    return juce::jlimit (legible, juce::jmax (legible, h - 260), juce::roundToInt ((float) h * (topFraction + wideBoost)));
}

MainComponent::BottomRowPlan MainComponent::planBottomRow (int total, int height) const
{
    int fixedStrips = 0, autoCount = 0;
    for (int s = 0; s < LayerSet::kNumSections; ++s)
    {
        const int w = show.layers.getWidth (static_cast<LayerSet::Section> (s));
        if (w == LayerSet::kAutoWidth)
            ++autoCount;
        else
            fixedStrips += w;
    }

    // The custom controls section (between center and right) is about four
    // strips wide plus a key column. When the window is too narrow for usable
    // strips, its lower part moves under the monitoring panel instead. Strips
    // are at most about 0.15 of the row's height wide.
    const int banks = LayerSet::kNumSections * FaderSection::kBankColumnWidth;
    const int strips = juce::jmax (1, fixedStrips + autoCount * 12);
    constexpr int ccStrips = 4, minStripWithCc = 40;
    BottomRowPlan plan;
    plan.stripW = (total - banks - FaderSection::kBankColumnWidth) / (strips + ccStrips);
    plan.withCc = plan.stripW >= minStripWithCc;
    if (! plan.withCc)
        plan.stripW = (total - banks) / strips;
    plan.stripW = juce::jlimit (40, juce::jlimit (70, 120, height * 15 / 100), plan.stripW);
    return plan;
}

MainComponent::TopRowSides MainComponent::topRowSideWidths (int width)
{
    // About 18 %, 26 % and 12 % of the row (the console: 19, 28 and 13).
    return { juce::jlimit (220, 400, width * 18 / 100), juce::jlimit (320, 580, width * 26 / 100), juce::jlimit (190, 320, width * 12 / 100) };
}

juce::Rectangle<int> MainComponent::consoleArea() const
{
    const auto all = getLocalBounds();
    const int width = all.getWidth();
    const int topH = topRowHeight();

    // The bottom row at its widest strips (Auto width sections fill any width).
    int bottomW = 0;
    const auto plan = planBottomRow (width, all.getHeight() - topH - 6);
    for (int s = 0; s < LayerSet::kNumSections; ++s)
    {
        const int n = show.layers.getWidth (static_cast<LayerSet::Section> (s));
        if (n == LayerSet::kAutoWidth)
            return all;
        bottomW += FaderSection::widthFor (n, plan.stripW);
    }
    if (plan.withCc)
        bottomW += FaderSection::kBankColumnWidth + 4 * plan.stripW;

    // The top row: the monitoring panel, the channel strip and the 4-Channel
    // column at their usual size, the display unit as wide as its height wants.
    const int topW = topRowSideWidths (width).total() + MainDisplay::preferredWidth (topH);

    return all.withSizeKeepingCentre (juce::jmin (width, juce::jmax (bottomW, topW)), all.getHeight());
}

void MainComponent::placeDisplay (juce::Rectangle<int> bounds)
{
    // Screen only: the display takes the window (the rest keeps its layout,
    // hidden, so restoring brings it back unchanged). Past about 1100 px of
    // height it is laid out at that size and scaled up as a whole, so the
    // screens keep the proportions they were drawn for (all vector drawing,
    // so it stays sharp at 4K).
    if (screenMaximized)
    {
        const auto all = getLocalBounds();
        const float scale = juce::jmax (1.0f, (float) all.getHeight() / kMaxScreenOnlyLayoutHeight);
        display->setTransform (juce::AffineTransform::scale (scale));
        display->setBounds (0, 0, (int) std::ceil ((float) all.getWidth() / scale), (int) std::ceil ((float) all.getHeight() / scale));
    }
    else
    {
        display->setTransform ({});
        display->setBounds (bounds);
    }
}

void MainComponent::layoutCompact()
{
    const int topH = topRowHeight();
    const auto plan = layout::compact::plan (getLocalBounds(), topH, MainDisplay::preferredWidth (topH));
    monitor->setBounds (plan.panel);
    splitter.setBounds (plan.splitter);
    placeDisplay (plan.display);
    sections[LayerSet::Left]->setBounds (plan.faders);
    compactKeys->setStripWidth (plan.stripW);
    compactKeys->setBounds (plan.keys);
    // What the COMPACT doesn't have keeps no place on the surface.
    for (juce::Component* c : { (juce::Component*) channelStrip.get(), (juce::Component*) customControls.get(), (juce::Component*) fourChannel.get(),
                                (juce::Component*) sections[LayerSet::Center].get(), (juce::Component*) sections[LayerSet::Right].get() })
        c->setBounds ({});
    for (int s = 0; s < LayerSet::kNumSections; ++s)
        if (dawSections[s] != nullptr)
        {
            dawSections[s]->setBounds (sections[s]->getBounds());
            dawSections[s]->setNumStrips (sections[s]->getNumStrips());
        }
}

void MainComponent::layoutSections()
{
    if (isCompact())
    {
        layoutCompact();
        return;
    }
    auto b = consoleArea();
    b.removeFromTop (topRowHeight() + 6);

    auto& layers = show.layers;
    int widths[LayerSet::kNumSections];
    int autoCount = 0;
    for (int s = 0; s < LayerSet::kNumSections; ++s)
    {
        widths[s] = layers.getWidth (static_cast<LayerSet::Section> (s));
        if (widths[s] == LayerSet::kAutoWidth)
            ++autoCount;
    }

    const int total = b.getWidth();
    const auto plan = planBottomRow (total, b.getHeight());
    const int stripW = plan.stripW;
    const bool withCc = plan.withCc;
    const int ccW = withCc ? FaderSection::kBankColumnWidth + 4 * stripW : 0;
    if (withCc != ccInBottomRow)
    {
        ccInBottomRow = withCc;
        customControls->setCompact (! withCc);
        layoutTopRow();
    }

    int remaining = total - ccW;
    for (int s = 0; s < LayerSet::kNumSections; ++s)
        if (widths[s] != LayerSet::kAutoWidth)
            remaining -= FaderSection::widthFor (widths[s], stripW);

    // What the strips leave: a few pixels go to the last section; more (the
    // strips at their widest) is shared either side, the row centred.
    int x = b.getX();
    const bool centred = autoCount == 0 && remaining >= stripW;
    if (centred)
        x += remaining / 2;

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
        if (s == LayerSet::kNumSections - 1 && ! centred)
            w = juce::jmax (w, b.getRight() - x);
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
    if (isCompact())
    {
        layoutCompact();
        return;
    }
    auto b = consoleArea();
    auto top = b.removeFromTop (topRowHeight());
    splitter.setBounds (b.removeFromTop (6));

    // As on the WING: the WING panel with USB AUDIO/DATA and MONITORING/
    // TALKBACK at the left, the main display unit (keys, screen and its right
    // hand column), the channel strip section, and the 4-Channel section at
    // the right (it works on the right fader section below it). Narrow windows
    // put the lower part of the custom controls there instead. Width the
    // display unit can't use is shared either side of it.
    // Windows too narrow for that keep the display wide: the channel strip at
    // the left, the monitoring panel over the 4-Channel section at the right,
    // both narrowed (to a point) until the screen is legible.
    const auto sides = topRowSideWidths (top.getWidth());
    const int displayW = MainDisplay::preferredWidth (top.getHeight());
    const int legibleW = MainDisplay::preferredWidth (MainDisplay::heightForScreen (MainDisplay::kLegibleScreenHeight));
    juce::Rectangle<int> right;
    if (top.getWidth() - sides.total() >= juce::jmin (displayW, juce::jmax (legibleW, top.getWidth() * 40 / 100)))
    {
        monitor->setBounds (top.removeFromLeft (sides.monitor));
        right = top.removeFromRight (sides.fourChannel);
        channelStrip->setBounds (top.removeFromRight (sides.strip));
    }
    else
    {
        int stripW = juce::jlimit (330, 620, top.getWidth() * 29 / 100);
        int rightW = juce::jlimit (260, 460, top.getWidth() * 22 / 100);
        if (const int over = stripW + rightW + displayW - top.getWidth(); over > 0)
        {
            const int stripSpare = stripW - kMinCompactStripWidth, rightSpare = rightW - kMinCompactRightWidth;
            const int cut = juce::jmin (over, stripSpare + rightSpare);
            const int stripCut = cut * stripSpare / juce::jmax (1, stripSpare + rightSpare);
            stripW -= stripCut;
            rightW -= cut - stripCut;
        }
        channelStrip->setBounds (top.removeFromLeft (stripW));
        right = top.removeFromRight (rightW);
        monitor->setBounds (right.removeFromTop (right.getHeight() * (ccInBottomRow ? 46 : 55) / 100));
    }
    top = top.withSizeKeepingCentre (juce::jmin (top.getWidth(), MainDisplay::preferredWidth (top.getHeight())), top.getHeight());
    fourChannel->setVisible (ccInBottomRow && ! editorShown && ! screenMaximized);
    if (! ccInBottomRow)
        customControls->setBounds (right);
    else
        fourChannel->setBounds (right);
    placeDisplay (top);
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

void MainComponent::focusGained (FocusChangeType)
{
    // Something over the whole console that takes the keyboard (a pop-up such
    // as the start-up notice, the editor) keeps it when the window gets the focus.
    for (int i = getNumChildComponents(); --i >= 0;)
        if (auto* c = getChildComponent (i); c->isVisible() && c->getWantsKeyboardFocus() && c->getBounds() == getLocalBounds())
        {
            c->grabKeyboardFocus();
            return;
        }
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    // F10 (or Ctrl+Shift+F): only the screen, or back; Esc leaves it (then full screen).
    if (key.getKeyCode() == juce::KeyPress::F10Key && ! mods.isAnyModifierKeyDown())
    {
        setScreenMaximized (! screenMaximized);
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::F11Key && ! mods.isAnyModifierKeyDown())
    {
        // Switching recreates the window on Linux; a press that reaches the new
        // window as well mustn't switch back.
        if (const auto now = juce::Time::getMillisecondCounter(); now - lastFullScreenSwitch > 500)
        {
            lastFullScreenSwitch = now;
            setFullScreen (! fullScreen);
        }
        return true;
    }
    if (key == juce::KeyPress::escapeKey && ! editorShown && (screenMaximized || fullScreen))
    {
        if (screenMaximized)
            setScreenMaximized (false);
        else
            setFullScreen (false);
        return true;
    }
    if (key.getModifiers().isCommandDown())
    {
        // With Ctrl held, X11 gives a control code as the text and the lowercase key as the code.
        const auto c = juce::CharacterFunctions::toLowerCase (key.getTextCharacter());
        const auto k = juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) key.getKeyCode());
        if ((c == 'f' || k == 'f') && key.getModifiers().isShiftDown())
        {
            setScreenMaximized (! screenMaximized);
            return true;
        }
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
            holdHomeLock (SurfaceLock::homeOnly()); // as holding HOME alone
            return true;
        }
    }
    return false;
}

void MainComponent::saveScreenshot()
{
    const auto dir = paths::documents().getChildFile ("screens");
    const auto file = dir.getChildFile ("screen-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".png");
    // At the size it's shown (scaled up when only the screen shows).
    const auto image = display->createComponentSnapshot (display->getLocalBounds(), true, juce::jmax (1.0f, display->getTransform().mat00));

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

void MainComponent::holdHomeLock (const juce::String& held)
{
    const auto result = surfaceLock.holdHome (held);
    if (result == SurfaceLock::Result::WrongCode)
    {
        log.info ("Surface stays locked: hold HOME with the keys it was locked with");
        return;
    }
    const bool locked = result == SurfaceLock::Result::Locked;
    if (locked)
    {
        lockOverlay = std::make_unique<LockOverlay> (*this);
        addAndMakeVisible (*lockOverlay);
        lockOverlay->setBounds (getLocalBounds());
        lockOverlay->setVisible (! editorShown);
    }
    else
    {
        lockOverlay.reset();
    }
    // The touchscreen and the display's knobs too (SHOW METER PAGE WHEN LOCKED there).
    display->setLocked (locked);
    // The WING remote's /$ctl/$stat/cnslock.
    settings.setValue (uiprefs::surfaceLock, surfaceLock.getCode());
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
    if (! engine.getFx().hasPlugin (slot))
        return;

    for (auto* w : pluginWindows)
    {
        if (w->getSlot() == slot)
        {
            w->toFront (true);
            return;
        }
    }

    // One window per slot: its plugins' editors side by side.
    pluginWindows.add (new PluginChainWindow (ctx, slot, [this] (PluginChainWindow& w) { pluginWindows.removeObject (&w); }));
}

void MainComponent::closePluginWindowsFor (juce::AudioProcessor* p)
{
    // The slot's plugin (or chain) closes its window; a plugin of a chain only its editor.
    for (int i = pluginWindows.size(); --i >= 0;)
    {
        // The slot's current plugin counts too: the window notices a chain
        // swapped elsewhere only on its next refresh.
        if (pluginWindows[i]->getShownProcessor() == p || engine.getFx().getPlugin (pluginWindows[i]->getSlot()) == p)
            pluginWindows.remove (i);
        else
            pluginWindows[i]->dropEditorFor (p);
    }
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
    updateVisibility();
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

void MainComponent::updateVisibility()
{
    // The console's parts stay alive (and keep their state) under the editor
    // and while the screen is maximized.
    const bool surface = ! editorShown && ! screenMaximized;
    // The COMPACT has no channel strip, custom controls, 4-Channel, center
    // or right section; it has its key section instead.
    const bool full = ! isCompact();
    for (juce::Component* c : { (juce::Component*) monitor.get(), (juce::Component*) &splitter })
        c->setVisible (surface);
    for (juce::Component* c : { (juce::Component*) channelStrip.get(), (juce::Component*) customControls.get() })
        c->setVisible (surface && full);
    display->setVisible (! editorShown);
    fourChannel->setVisible (surface && ccInBottomRow && full);
    for (int s = 0; s < LayerSet::kNumSections; ++s)
        sections[s]->setVisible (surface && (full || s == LayerSet::Left));
    compactKeys->setVisible (surface && ! full);
    // DAW mode's strips show themselves while the surface does (their timer).
    for (auto& sec : dawSections)
        if (sec != nullptr && ! surface)
            sec->setVisible (false);
    if (lockOverlay != nullptr)
        lockOverlay->setVisible (surface);
}

void MainComponent::setScreenMaximized (bool maximized)
{
    if (maximized == screenMaximized)
        return;
    screenMaximized = maximized;
    settings.setValue (uiprefs::screenMaximized, maximized);
    display->setMaximized (maximized);
    updateVisibility();
    layoutTopRow();
    repaint();
    if (! editorShown && isShowing())
        grabKeyboardFocus();
}

void MainComponent::setFullScreen (bool shouldBe)
{
    auto* window = dynamic_cast<juce::DocumentWindow*> (getTopLevelComponent());
    if (window == nullptr || window == static_cast<juce::Component*> (this) || shouldBe == fullScreen)
        return;
    fullScreen = shouldBe;
    settings.setValue (uiprefs::fullScreen, shouldBe);
    if (shouldBe)
    {
        windowStateBeforeFullScreen = window->getWindowStateAsString();
#if JUCE_LINUX || JUCE_BSD
        // X11: JUCE's kiosk mode only resizes the window, so drop the native
        // frame first (an undecorated window over the whole screen).
        window->setUsingNativeTitleBar (false);
        window->setTitleBarHeight (0);
        window->setResizable (false, false);
#endif
        juce::Desktop::getInstance().setKioskModeComponent (window, false);
    }
    else
    {
        juce::Desktop::getInstance().setKioskModeComponent (nullptr);
#if JUCE_LINUX || JUCE_BSD
        window->setUsingNativeTitleBar (true);
        window->setResizable (true, true);
#endif
        if (windowStateBeforeFullScreen.isNotEmpty())
            window->restoreWindowStateFromString (windowStateBeforeFullScreen);
        windowStateBeforeFullScreen.clear();
    }
    log.info (shouldBe ? "Full screen (F11 or Esc to leave)" : "Left full screen");
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (safe != nullptr && safe->isShowing() && ! safe->editorShown)
            safe->grabKeyboardFocus();
    });
}

void MainComponent::showStartupNotice()
{
    startupNotice::show (settings, *this, true);
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
    juce::String title = juce::String (kAppName) + " - " + show.getName();
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
    // As on the WING, the show is called what its file is called (also after
    // SAVE AS under a new name).
    const auto previousName = show.getName();
    show.setName (file.getFileNameWithoutExtension());

    const auto result = show.saveToFile (file);

    if (result.failed())
    {
        show.setName (previousName);
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
