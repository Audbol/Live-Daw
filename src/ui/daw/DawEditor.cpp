#include "ui/daw/DawEditor.h"

namespace livedaw::ui
{

using daw::int64;

//==============================================================================
namespace dawfiles
{
    void confirmDiscard (juce::Component* parent, daw::Session& session, std::function<void()> then)
    {
        auto& p = session.getProject();
        const bool empty = ! p.hasFolder() && p.getNumTracks() == 0;
        if (! p.isDirty() || empty)
        {
            then();
            return;
        }
        juce::AlertWindow::showYesNoCancelBox (juce::MessageBoxIconType::QuestionIcon, "Recording project",
                                               "Save the changes to \"" + p.getName() + "\" first?", "SAVE", "DISCARD", "CANCEL", parent,
                                               juce::ModalCallbackFunction::create ([&session, then] (int r)
        {
            if (r == 0)
                return;
            if (r == 1)
            {
                auto result = session.save();
                if (result.failed())
                {
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save", result.getErrorMessage());
                    return;
                }
            }
            then();
        }));
    }

    void newProject (juce::Component* parent, daw::Session& session)
    {
        confirmDiscard (parent, session, [parent, &session]
        {
            const auto name = "Session " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M");
            askForText (parent, "NEW PROJECT", "Name (saved in " + session.getProjectsFolder().getFullPathName() + "):", name,
                        [&session] (const juce::String& n)
            {
                if (n.isEmpty())
                    return;
                auto result = session.createProject (n);
                if (result.failed())
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "New project", result.getErrorMessage());
            });
        });
    }

    void open (juce::Component* parent, daw::Session& session, const juce::File& file)
    {
        if (session.getProject().hasFolder() && file == session.getProject().getFile())
            return;
        confirmDiscard (parent, session, [parent, &session, file]
        {
            auto load = [&session, file] (bool restore)
            {
                auto result = session.openProject (file, restore);
                if (result.failed())
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Open project", result.getErrorMessage());
            };
            if (! daw::Session::hasNewerAutosave (file))
            {
                load (false);
                return;
            }
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Open project",
                                                "\"" + file.getFileNameWithoutExtension() + "\" has unsaved changes from an autosave. Restore them?",
                                                "RESTORE", "OPEN SAVED", parent,
                                                juce::ModalCallbackFunction::create ([load] (int r) { load (r != 0); }));
        });
    }

    void openWithChooser (juce::Component* parent, daw::Session& session, std::unique_ptr<juce::FileChooser>& chooser)
    {
        chooser = std::make_unique<juce::FileChooser> ("Open recording project", session.getProjectsFolder(), juce::String ("*") + daw::Project::fileExtension);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [parent, &session] (const juce::FileChooser& fc)
        {
            if (fc.getResult() != juce::File())
                open (parent, session, fc.getResult());
        });
    }

    void save (juce::Component*, daw::Session& session)
    {
        auto result = session.save();
        if (result.failed())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save project", result.getErrorMessage());
    }

    void saveAs (juce::Component* parent, daw::Session& session)
    {
        askForText (parent, "SAVE AS", "New project name (a folder in " + session.getProjectsFolder().getFullPathName() + "):",
                    session.getProject().getName() + " copy", [&session] (const juce::String& n)
        {
            const auto clean = juce::File::createLegalFileName (n);
            if (clean.isEmpty())
                return;
            const auto folder = session.getProjectsFolder().getChildFile (clean);
            if (folder.exists())
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save as", "\"" + clean + "\" already exists.");
                return;
            }
            auto result = session.saveAs (folder);
            if (result.failed())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save as", result.getErrorMessage());
        });
    }

    void importWithChooser (juce::Component*, daw::Session& session, std::unique_ptr<juce::FileChooser>& chooser)
    {
        chooser = std::make_unique<juce::FileChooser> ("Import audio", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                       daw::importWildcard (session.getFormats()));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectMultipleItems,
                              [&session] (const juce::FileChooser& fc)
        {
            if (! fc.getResults().isEmpty())
                session.importFiles (fc.getResults(), session.selectedTrack, session.getPosition());
        });
    }

    void exportTracks (juce::Component* parent, daw::Session& session, AudioFileFormat format)
    {
        auto result = session.ensureFolder();
        if (result.failed())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Export", result.getErrorMessage());
            return;
        }
        const auto folder = session.getProject().getFolder().getChildFile ("Export");
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Export tracks",
                                            "Render every track (clip gains, fades and track gain, from the start of the timeline) to "
                                                + format.describe() + " files in\n" + folder.getFullPathName() + "?",
                                            "EXPORT", "CANCEL", parent,
                                            juce::ModalCallbackFunction::create ([&session, folder, format] (int r)
        {
            if (r != 0)
                session.exportTracks (folder, format);
        }));
    }

    void showMenu (juce::Component* target, daw::Session& session, std::unique_ptr<juce::FileChooser>& chooser)
    {
        auto* parent = target->getTopLevelComponent();
        juce::PopupMenu m;
        m.addSectionHeader ("PROJECT: " + session.getProject().getName().toUpperCase());
        m.addItem ("New project...", [parent, &session] { newProject (parent, session); });
        m.addItem ("Open...   (Ctrl+O)", [parent, &session, &chooser] { openWithChooser (parent, session, chooser); });
        juce::PopupMenu recent;
        for (auto& path : session.getRecentProjects())
        {
            const juce::File f (path);
            recent.addItem (f.getFileNameWithoutExtension() + "   " + f.getParentDirectory().getParentDirectory().getFullPathName(), f.existsAsFile(),
                            session.getProject().hasFolder() && f == session.getProject().getFile(), [parent, &session, f] { open (parent, session, f); });
        }
        m.addSubMenu ("Open recent", recent, recent.getNumItems() > 0);
        m.addItem ("Save   (Ctrl+S)", [parent, &session] { save (parent, session); });
        m.addItem ("Save as...   (Ctrl+Shift+S)", [parent, &session] { saveAs (parent, session); });
        m.addItem ("Show the project folder", session.getProject().hasFolder(), false, [&session] { session.getProject().getFolder().revealToUser(); });
        m.addSeparator();
        m.addItem ("Import audio...   (Ctrl+I)", [parent, &session, &chooser] { importWithChooser (parent, session, chooser); });
        juce::PopupMenu exports;
        for (const auto& f : AudioFileFormat::exportChoices())
            exports.addItem (f.describe() + "...", true, f == session.getExportFormat(), [parent, &session, f] { exportTracks (parent, session, f); });
        m.addSubMenu ("Export tracks", exports, session.getProject().getLength() > 0);
        juce::PopupMenu formats;
        for (const auto& f : AudioFileFormat::recordChoices())
        {
            const auto why = f.check (2, session.getProject().getSampleRate());
            formats.addItem (f.describe() + (why.isNotEmpty() ? "   (needs 32 / 44.1 / 48 kHz)" : juce::String()), why.isEmpty() && ! session.isRecording(),
                             f == session.getRecordFormat(), [&session, f] { session.setRecordFormat (f); });
        }
        m.addSubMenu ("Record format: " + session.getRecordFormat().describe(), formats);
        m.addSeparator();
        m.addItem ("Crossfade overlaps automatically", true, session.autoCrossfade, [&session] { session.autoCrossfade = ! session.autoCrossfade; });
        m.addItem ("Recording replaces what it covers", true, session.replaceOnRecord, [&session] { session.replaceOnRecord = ! session.replaceOnRecord; });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target));
    }
} // namespace dawfiles

//==============================================================================
/** A field of the top bar: a caption over a value; drag up/down or the wheel
    changes it, a double-click types it, a click can open a menu. */
class DawValueField final : public juce::Component, public juce::SettableTooltipClient
{
public:
    DawValueField (juce::String captionText) : caption (std::move (captionText)) {}

    std::function<juce::String()> getText;
    std::function<void (float steps)> nudge;
    std::function<void()> onDoubleClick, onClick;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (Dark::well);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (isMouseOver() ? DawColours::keyOutline.brighter (0.3f) : DawColours::line);
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
        auto t = r.reduced (6.0f, 2.0f).toNearestInt();
        g.setColour (DawColours::textDim);
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.drawText (caption, t.removeFromTop (12), juce::Justification::centredLeft);
        g.setColour (DawColours::text);
        g.setFont (juce::FontOptions (juce::jmin (20.0f, (float) t.getHeight()), juce::Font::bold));
        g.drawFittedText (getText ? getText() : juce::String(), t, juce::Justification::centredLeft, 1, 0.7f);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseDown (const juce::MouseEvent&) override { lastY = 0; dragged = false; }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (nudge == nullptr)
            return;
        const int dy = e.getDistanceFromDragStartY();
        const int step = e.mods.isShiftDown() ? 2 : 6;
        if (std::abs (dy - lastY) >= step)
        {
            nudge ((float) ((lastY - dy) / step) * (e.mods.isShiftDown() ? 0.1f : 1.0f));
            lastY = dy - (dy - lastY) % step;
            dragged = true;
            repaint();
        }
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! dragged && e.getNumberOfClicks() == 1 && onClick && getLocalBounds().contains (e.getPosition()))
            onClick();
    }
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (onDoubleClick)
            onDoubleClick();
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (nudge)
            nudge ((w.deltaY > 0 ? 1.0f : -1.0f) * (e.mods.isShiftDown() ? 0.1f : 1.0f));
        repaint();
    }

private:
    juce::String caption;
    int lastY = 0;
    bool dragged = false;
};

//==============================================================================
class DawEditor::TopBar final : public juce::Component
{
public:
    explicit TopBar (DawEditor& e) : editor (e), state (e.state), session (e.session)
    {
        auto& s = session;
        consoleKey.iconAndText = true;
        consoleKey.onClick = [this] { if (editor.onClose) editor.onClose(); };
        consoleKey.setTooltip ("Back to the console (Esc, Ctrl+E)");
        fileKey.onClick = [this] { dawfiles::showMenu (&fileKey, session, editor.chooser); };
        startKey.onClick = [this] { session.locate (0); state.scrollToShow (0); };
        startKey.setTooltip ("Return to the start (Home)");
        rewKey.onClick = [this] { session.jumpToMarker (false); };
        rewKey.setTooltip ("Previous marker (Ctrl+Left)");
        ffKey.onClick = [this] { session.jumpToMarker (true); };
        ffKey.setTooltip ("Next marker (Ctrl+Right)");
        playKey.onClick = [&s] { if (! s.isRolling()) s.play(); };
        playKey.isLit = [&s] { return s.getState() == Recorder::State::Playing || s.getState() == Recorder::State::Starting; };
        playKey.setTooltip ("Play (space)");
        stopKey.onClick = [&s] { s.stop(); };
        stopKey.setTooltip ("Stop (space)");
        recKey.iconColour = DawColours::record;
        recKey.isLit = [&s] { return s.isRecording(); };
        recKey.onClick = [this] { editor.keyPressed (juce::KeyPress ('r')); };
        recKey.setTooltip ("Record the armed tracks (R)");
        loopKey.isLit = [&s] { return s.isLoopEnabled(); };
        loopKey.onClick = [&s] { s.setLoopEnabled (! s.isLoopEnabled()); };
        loopKey.setTooltip ("Loop (L): drag in the ruler to set the range");
        markerKey.onClick = [&s] { s.addMarkerAtPosition(); };
        markerKey.setTooltip ("Add a marker at the playhead (M)");
        snapKey.onClick = [this] { snapMenu(); };
        snapKey.setTooltip ("What edits snap to (clip edges and markers always attract; hold shift to place freely)");
        zoomOutKey.onClick = [this] { state.zoomAround (state.samplesPerPixel * 1.5, (float) state.viewWidth * 0.5f); };
        zoomInKey.onClick = [this] { state.zoomAround (state.samplesPerPixel / 1.5, (float) state.viewWidth * 0.5f); };
        fitKey.onClick = [this] { state.zoomToFit(); };
        fitKey.setTooltip ("Zoom to fit (F); Ctrl+wheel zooms at the mouse");
        undoKey.onClick = [this] { state.project.undoLast(); };
        redoKey.onClick = [this] { state.project.redoNext(); };
        undoKey.setTooltip ("Undo (Ctrl+Z)");
        redoKey.setTooltip ("Redo (Ctrl+Shift+Z)");
        followKey.isLit = [this] { return state.follow; };
        followKey.onClick = [this] { state.follow = ! state.follow; };
        followKey.setTooltip ("The view pages with the playhead while playing");

        tempo.getText = [this] { return juce::String (state.project.getTempo(), 1); };
        tempo.nudge = [this] (float steps)
        {
            state.project.beginEdit ("Tempo");
            state.project.setTempo (std::round ((state.project.getTempo() + steps) * 10.0) / 10.0);
        };
        tempo.onDoubleClick = [this]
        {
            askForText (getTopLevelComponent(), "TEMPO", "Beats per minute (20-400):", juce::String (state.project.getTempo(), 2), [this] (const juce::String& t)
            {
                if (t.getDoubleValue() > 0.0)
                {
                    state.project.beginEdit ("Tempo");
                    state.project.setTempo (t.getDoubleValue());
                }
            });
        };
        sig.getText = [this] { return juce::String (state.project.getSigNumerator()) + "/" + juce::String (state.project.getSigDenominator()); };
        sig.onClick = [this] { sigMenu(); };
        position.getText = [] { return juce::String(); };

        for (juce::Component* c : { (juce::Component*) &consoleKey, (juce::Component*) &fileKey, (juce::Component*) &startKey, (juce::Component*) &rewKey,
                                    (juce::Component*) &playKey, (juce::Component*) &stopKey, (juce::Component*) &recKey, (juce::Component*) &ffKey,
                                    (juce::Component*) &loopKey, (juce::Component*) &markerKey, (juce::Component*) &tempo, (juce::Component*) &sig,
                                    (juce::Component*) &snapKey, (juce::Component*) &zoomOutKey, (juce::Component*) &zoomInKey,
                                    (juce::Component*) &fitKey, (juce::Component*) &followKey, (juce::Component*) &undoKey, (juce::Component*) &redoKey })
            addAndMakeVisible (c);
        tempo.setTooltip ("Tempo of the grid: drag, wheel or double-click");
        sig.setTooltip ("Time signature of the grid");
    }

    void snapMenu()
    {
        juce::PopupMenu m;
        m.addSectionHeader ("SNAP");
        for (auto mode : { daw::SnapMode::Off, daw::SnapMode::Grid, daw::SnapMode::Bars, daw::SnapMode::Beats, daw::SnapMode::Seconds })
            m.addItem (daw::snapModeName (mode), true, state.snap == mode, [this, mode]
            {
                state.snap = mode;
                state.ctx.settings.setValue ("daw.snap", (int) mode);
                repaint();
            });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&snapKey));
    }

    void sigMenu()
    {
        juce::PopupMenu m;
        m.addSectionHeader ("TIME SIGNATURE");
        const std::pair<int, int> sigs[] = { { 2, 4 }, { 3, 4 }, { 4, 4 }, { 5, 4 }, { 6, 4 }, { 7, 4 }, { 3, 8 }, { 5, 8 }, { 6, 8 }, { 7, 8 }, { 9, 8 }, { 12, 8 }, { 2, 2 } };
        for (auto [n, d] : sigs)
            m.addItem (juce::String (n) + "/" + juce::String (d), true,
                       state.project.getSigNumerator() == n && state.project.getSigDenominator() == d, [this, n = n, d = d]
            {
                state.project.beginEdit ("Time signature");
                state.project.setTimeSignature (n, d);
            });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&sig));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (DawColours::bar);
        g.setColour (DawColours::line);
        g.fillRect (0, getHeight() - 1, getWidth(), 1);

        // The project.
        auto n = nameArea;
        g.setColour (DawColours::textDim);
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.drawText ("PROJECT", n.removeFromTop (14), juce::Justification::bottomLeft);
        g.setColour (DawColours::text);
        g.setFont (juce::FontOptions (19.0f, juce::Font::bold));
        g.drawFittedText (state.project.getName().toUpperCase() + (state.project.isDirty() ? " *" : ""), n, juce::Justification::centredLeft, 1, 0.7f);

        // Position.
        auto p = positionArea.toFloat();
        g.setColour (juce::Colours::black);
        g.fillRoundedRectangle (p, 4.0f);
        g.setColour (session.isRecording() ? DawColours::record : DawColours::line);
        g.drawRoundedRectangle (p.reduced (0.5f), 4.0f, 1.0f);
        const auto grid = state.grid();
        const auto pos = session.getPosition();
        auto t = positionArea.reduced (10, 4);
        g.setColour (session.isRecording() ? DawColours::record : DawColours::text);
        g.setFont (juce::FontOptions ((float) t.getHeight() * 0.62f, juce::Font::bold));
        g.drawFittedText (state.showBars ? grid.formatBars (pos) : grid.formatTime (pos), t.removeFromTop (t.getHeight() * 64 / 100),
                          juce::Justification::centredLeft, 1, 0.8f);
        g.setColour (DawColours::textDim);
        g.setFont (juce::FontOptions ((float) t.getHeight() * 0.9f, juce::Font::bold));
        g.drawFittedText ((state.showBars ? grid.formatTime (pos) : grid.formatBars (pos)) + "   " + stateText(), t, juce::Justification::centredLeft, 1, 0.8f);

        // Status.
        auto s = statusArea;
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        auto line = [&] (const juce::String& text, juce::Colour c)
        {
            g.setColour (c);
            g.drawFittedText (text, s.removeFromTop (s.getHeight() / 2 + 1), juce::Justification::centredLeft, 1, 0.7f);
        };
        auto& rec = session.getRecorder();
        const int dropouts = rec.getDropouts();
        const float load = rec.getDiskLoad();
        line ("DISK " + juce::String (juce::roundToInt (load * 100.0f)) + "%   DROPOUTS " + juce::String (dropouts) + "   "
                  + rateText (state.project.getSampleRate()) + "   REC LEFT " + durationText (session.getRecordSecondsLeft()),
              dropouts > 0 || load > 0.8f ? DawColours::record : DawColours::textDim);
        if (session.isBusy())
            line (session.getJobName() + " " + juce::String (juce::roundToInt (session.getJobProgress() * 100.0f)) + "%", DawColours::selection);
        else if (! session.rateMatches())
            line ("PROJECT " + rateText (state.project.getSampleRate()) + " BUT CONSOLE " + rateText (rec.getSampleRate()) + ": WRONG SPEED", DawColours::record);
        else if (message.isNotEmpty() && juce::Time::getMillisecondCounter() - messageTime < 6000)
            line (message, DawColours::text);
        else
            line (juce::String (session.getNumArmed()) + " ARMED   " + juce::String ((int) state.rows.size()) + " TRACKS   SNAP " + daw::snapModeName (state.snap),
                  DawColours::textDim);
    }

    juce::String stateText() const
    {
        switch (session.getState())
        {
            case Recorder::State::Stopped:   return "STOP";
            case Recorder::State::Starting:  return "STARTING";
            case Recorder::State::Playing:   return session.isLoopEnabled() ? "PLAY LOOP" : "PLAY";
            case Recorder::State::Recording: return "RECORD";
        }
        return {};
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (8, 7);
        const int h = b.getHeight();
        const int t = juce::jmin (h, 44);
        const int nameW = juce::jlimit (110, 240, getWidth() / 9), positionW = juce::jlimit (150, 230, getWidth() / 9);
        // Everything left of the status needs about this much; a narrower
        // window (down to the 1152 px minimum) scales the keys down to fit.
        const int needed = 118 + 68 + nameW + 8 + 8 * (t + 3) + 8 + positionW + 8 + 76 + 60 + 100 + 32 + 32 + 44 + 72 + 54 + 62;
        const float scale = juce::jmin (1.0f, (float) b.getWidth() / (float) needed);
        auto sc = [scale] (int w) { return juce::roundToInt ((float) w * scale); };
        auto key = [&] (juce::Component& c, int w, int gap = 4)
        {
            c.setBounds (b.removeFromLeft (sc (w)));
            b.removeFromLeft (sc (gap));
        };
        key (consoleKey, 112, 6);
        key (fileKey, 58, 10);
        nameArea = b.removeFromLeft (sc (nameW));
        b.removeFromLeft (sc (8));
        for (auto* k : { &startKey, &rewKey, &playKey, &stopKey, &recKey, &ffKey, &loopKey, &markerKey })
        {
            k->setBounds (b.removeFromLeft (sc (t)).withSizeKeepingCentre (sc (t), juce::jmin (h, sc (t) + 8)));
            b.removeFromLeft (sc (3));
        }
        b.removeFromLeft (sc (8));
        positionArea = b.removeFromLeft (sc (positionW));
        b.removeFromLeft (sc (8));
        key (tempo, 72);
        key (sig, 52, 8);
        key (snapKey, 92, 8);
        key (zoomOutKey, 30, 2);
        key (zoomInKey, 30, 2);
        key (fitKey, 40, 4);
        key (followKey, 64, 8);
        key (undoKey, 52, 2);
        key (redoKey, 52, 10);
        statusArea = b;
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (positionArea.contains (e.getPosition()))
        {
            state.showBars = ! state.showBars;
            state.ctx.settings.setValue ("daw.showBars", state.showBars);
            if (state.viewChanged)
                state.viewChanged();
        }
    }

    /** Every frame: repaint the position (and the status a few times a second). */
    void tick()
    {
        const auto pos = session.getPosition();
        if (pos != shownPosition || session.getState() != shownState)
        {
            shownPosition = pos;
            shownState = session.getState();
            repaint (positionArea);
        }
        snapKey.setButtonText ("SNAP " + juce::String (daw::snapModeName (state.snap)));
        undoKey.setEnabled (state.project.canUndo());
        redoKey.setEnabled (state.project.canRedo());
        const auto now = juce::Time::getMillisecondCounter();
        if (now - lastStatus > 250)
        {
            lastStatus = now;
            repaint (statusArea);
            repaint (nameArea);
        }
    }

    void showMessage (const juce::String& m)
    {
        message = m;
        messageTime = juce::Time::getMillisecondCounter();
        repaint (statusArea);
    }

private:
    DawEditor& editor;
    DawEditorState& state;
    daw::Session& session;

    DawKey consoleKey { "CONSOLE", Icon::Faders }, fileKey { "FILE" };
    DawKey startKey { {}, Icon::ReturnToStart }, rewKey { {}, Icon::MarkerPrev }, playKey { {}, Icon::Play, Colours::meterGreen },
        stopKey { {}, Icon::Stop }, recKey { {}, Icon::Record, DawColours::record }, ffKey { {}, Icon::MarkerNext },
        loopKey { {}, Icon::Loop, DawColours::loop }, markerKey { {}, Icon::MarkerAdd };
    DawKey snapKey { "SNAP GRID" }, zoomOutKey { "-" }, zoomInKey { "+" }, fitKey { "FIT" }, followKey { "FOLLOW" }, undoKey { "UNDO" }, redoKey { "REDO" };
    DawValueField tempo { "TEMPO" }, sig { "SIG" }, position { "POSITION" };
    juce::Rectangle<int> nameArea, positionArea, statusArea;
    int64 shownPosition = -1;
    Recorder::State shownState = Recorder::State::Stopped;
    juce::uint32 lastStatus = 0, messageTime = 0;
    juce::String message;
};

//==============================================================================
DawEditor::DawEditor (UiContext& c, daw::Session& s)
    : ctx (c), session (s), state (c, s), waves (s.getFormats()), trackList (state), ruler (state), lanes (state, waves)
{
    state.snap = static_cast<daw::SnapMode> (juce::jlimit (0, 4, ctx.settings.getIntValue ("daw.snap", (int) daw::SnapMode::Grid)));
    state.showBars = ctx.settings.getBoolValue ("daw.showBars", false);
    state.viewChanged = [this] { viewChanged(); };
    state.selectionChanged = [this]
    {
        lanes.repaint();
        for (int i = 0; i < trackList.getNumChildComponents(); ++i)
            trackList.getChildComponent (i)->repaint();
    };
    state.status = [this] (const juce::String& m) { status (m); };
    waves.onChange = [this] { lanes.repaint(); };

    topBar = std::make_unique<TopBar> (*this);
    addAndMakeVisible (*topBar);
    addAndMakeVisible (trackList);
    addAndMakeVisible (ruler);
    addAndMakeVisible (lanes);
    addAndMakeVisible (playhead);
    for (auto* sb : { &hScroll, &vScroll })
    {
        sb->addListener (this);
        sb->setAutoHide (false);
        sb->setColour (juce::ScrollBar::thumbColourId, DawColours::keyOutline);
        sb->setColour (juce::ScrollBar::backgroundColourId, DawColours::bar);
        addAndMakeVisible (*sb);
    }
    lanes.onLocate = [this] (int64 pos) { session.locate (pos); };

    session.getProject().addListener (this);
    setWantsKeyboardFocus (true);
    projectReplaced();
    vblank = std::make_unique<juce::VBlankAttachment> (this, [this] { frame(); });
}

DawEditor::~DawEditor()
{
    session.getProject().removeListener (this);
    vblank.reset();
}

void DawEditor::paint (juce::Graphics& g)
{
    g.fillAll (DawColours::background);
    // The corner above the track headers.
    g.setColour (DawColours::bar);
    g.fillRect (corner);
    g.setColour (DawColours::line);
    g.fillRect (corner.getX(), corner.getBottom() - 1, corner.getWidth(), 1);
    g.fillRect (corner.getRight() - 1, corner.getY(), 1, corner.getHeight());
    g.setColour (DawColours::textDim);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    auto c = corner.reduced (10, 4);
    g.drawText ("TRACKS", c.removeFromTop (c.getHeight() / 2), juce::Justification::centredLeft);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("R ARM   M MUTE   S SOLO", c, juce::Justification::centredLeft);
}

void DawEditor::resized()
{
    auto b = getLocalBounds();
    topBar->setBounds (b.removeFromTop (58));
    constexpr int headerW = 270, scroll = 12;
    const int rulerH = DawRuler::kTimeHeight + DawRuler::kMarkerHeight;
    auto left = b.removeFromLeft (headerW);
    corner = left.removeFromTop (rulerH);
    left.removeFromBottom (scroll);
    trackList.setBounds (left);
    vScroll.setBounds (b.removeFromRight (scroll).withTrimmedTop (rulerH).withTrimmedBottom (scroll));
    ruler.setBounds (b.removeFromTop (rulerH));
    hScroll.setBounds (b.removeFromBottom (scroll));
    lanes.setBounds (b);
    state.viewWidth = lanes.getWidth();
    state.viewHeight = lanes.getHeight();
    if (! fitted && state.viewWidth > 100)
    {
        // The first layout: fit the project now that the lanes' width is known.
        fitted = true;
        state.zoomToFit();
    }
    state.clampScroll();
    trackList.resized();
    updateScrollBars();
    layoutPlayhead();
}

void DawEditor::visibilityChanged()
{
    if (isShowing())
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<DawEditor> (this)]
        {
            if (safe != nullptr && safe->isShowing())
                safe->grabKeyboardFocus();
        });
}

void DawEditor::status (const juce::String& m)
{
    topBar->showMessage (m);
}

//==============================================================================
void DawEditor::projectChanged()
{
    triggerAsyncUpdate();
}

void DawEditor::projectReplaced()
{
    waves.clear();
    auto& p = session.getProject();
    waves.setPeaksFolder (p.hasFolder() ? p.getFolder().getChildFile ("Peaks") : juce::File());
    state.selection.clear();
    state.scrollY = 0;
    state.refresh();
    state.zoomToFit();
    handleAsyncUpdate();
}

void DawEditor::handleAsyncUpdate()
{
    auto& p = session.getProject();
    waves.setPeaksFolder (p.hasFolder() ? p.getFolder().getChildFile ("Peaks") : juce::File());
    state.refresh();
    trackList.update();
    lanes.repaint();
    ruler.repaint();
    topBar->repaint();
    updateScrollBars();
}

void DawEditor::viewChanged()
{
    state.clampScroll();
    trackList.resized();
    lanes.repaint();
    ruler.repaint();
    updateScrollBars();
    layoutPlayhead();
}

void DawEditor::updateScrollBars()
{
    updatingScrollBars = true;
    const double visible = (double) juce::jmax (1, state.viewWidth) * state.samplesPerPixel;
    const double length = std::max ({ (double) session.getProject().getLength() * 1.2, state.viewStart + visible, session.getProject().getSampleRate() * 60.0 });
    hScroll.setRangeLimits (0.0, length);
    hScroll.setCurrentRange (state.viewStart, visible);
    vScroll.setRangeLimits (0.0, (double) juce::jmax (state.totalHeight + 40, state.viewHeight));
    vScroll.setCurrentRange ((double) state.scrollY, (double) state.viewHeight);
    updatingScrollBars = false;
}

void DawEditor::scrollBarMoved (juce::ScrollBar* bar, double start)
{
    if (updatingScrollBars)
        return;
    if (bar == &hScroll)
        state.viewStart = start;
    else
        state.scrollY = juce::roundToInt (start);
    viewChanged();
}

void DawEditor::layoutPlayhead()
{
    const int x = lanes.getX() + juce::roundToInt (state.xOf (session.getPosition()));
    const bool visible = x >= lanes.getX() - 1 && x <= lanes.getRight();
    playhead.setVisible (visible);
    if (visible)
    {
        playhead.recording = session.isRecording();
        playhead.setBounds (x - 7, ruler.getY(), 15, lanes.getBottom() - ruler.getY());
    }
    lastPlayheadX = x;
}

void DawEditor::frame()
{
    if (! isShowing())
        return;
    // Keys come back to the editor after a dialog or menu closed.
    if (juce::Component::getCurrentlyFocusedComponent() == nullptr && juce::Component::getCurrentlyModalComponent() == nullptr
        && getPeer() != nullptr && getPeer()->isFocused())
        grabKeyboardFocus();
    // Page-scroll with the playhead.
    const auto pos = session.getPosition();
    if (state.follow && session.isRolling())
    {
        const float x = state.xOf (pos);
        if (x > (float) state.viewWidth - 24.0f || x < 0.0f)
        {
            state.viewStart = (double) pos - (double) state.viewWidth * 0.04 * state.samplesPerPixel;
            viewChanged();
        }
    }
    const int x = lanes.getX() + juce::roundToInt (state.xOf (pos));
    if (x != lastPlayheadX || playhead.recording != session.isRecording())
        layoutPlayhead();
    lanes.tick();
    topBar->tick();
}

//==============================================================================
bool DawEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    return lanes.isInterestedInFileDrag (files);
}

void DawEditor::filesDropped (const juce::StringArray& files, int x, int y)
{
    // Dropped outside the lanes (headers, top bar): new tracks at the playhead.
    juce::ignoreUnused (x, y);
    juce::Array<juce::File> list;
    for (auto& f : files)
        if (daw::canImport (session.getFormats(), juce::File (f)))
            list.add (juce::File (f));
    session.importFiles (list, 0, session.getPosition());
}

bool DawEditor::keyPressed (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    const int code = key.getKeyCode();
    const auto ch = juce::CharacterFunctions::toLowerCase (key.getTextCharacter());
    const bool cmd = mods.isCommandDown();
    // With Ctrl the text character can be a control code: compare the key code too.
    auto is = [&] (char c) { return ch == c || juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) code) == (juce::juce_wchar) c; };
    auto& p = session.getProject();

    if (code == juce::KeyPress::spaceKey)
    {
        session.togglePlay();
        return true;
    }
    if (code == juce::KeyPress::escapeKey)
    {
        if (! state.selection.empty())
            state.clearSelection();
        else if (onClose)
            onClose();
        return true;
    }
    if (cmd && is ('e'))
    {
        if (onClose)
            onClose();
        return true;
    }
    if (cmd && is ('z'))
    {
        if (mods.isShiftDown())
            p.redoNext();
        else
            p.undoLast();
        return true;
    }
    if (cmd && is ('y'))
    {
        p.redoNext();
        return true;
    }
    if (cmd && is ('s'))
    {
        if (mods.isShiftDown())
            dawfiles::saveAs (this, session);
        else
            dawfiles::save (this, session);
        return true;
    }
    if (cmd && is ('o'))
    {
        dawfiles::openWithChooser (this, session, chooser);
        return true;
    }
    if (cmd && is ('i'))
    {
        dawfiles::importWithChooser (this, session, chooser);
        return true;
    }
    if (cmd && is ('d'))
    {
        state.duplicateSelection();
        return true;
    }
    if (cmd && is ('c'))
    {
        state.copySelection();
        return true;
    }
    if (cmd && is ('x'))
    {
        state.cutSelection();
        return true;
    }
    if (cmd && is ('v'))
    {
        state.paste (session.getPosition());
        return true;
    }
    if (cmd && is ('a'))
    {
        state.selectAll();
        return true;
    }
    if (cmd && is ('m'))
    {
        state.toggleMuteSelection();
        return true;
    }
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
    {
        state.deleteSelection();
        return true;
    }
    if (code == juce::KeyPress::homeKey)
    {
        session.locate (0);
        state.scrollToShow (0);
        return true;
    }
    if (code == juce::KeyPress::endKey)
    {
        session.locate (p.getLength());
        state.scrollToShow (p.getLength());
        return true;
    }
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
    {
        const int dir = code == juce::KeyPress::leftKey ? -1 : 1;
        if (cmd)
            session.jumpToMarker (dir > 0);
        else if (! state.selection.empty())
            state.nudge (dir, mods.isAltDown());
        else
        {
            double step = state.grid().step (state.snap, state.samplesPerPixel);
            if (step <= 0.0)
                step = state.samplesPerPixel * 10.0;
            session.locate (state.grid().snap (session.getPosition() + (int64) (step * dir), state.snap, state.samplesPerPixel));
        }
        state.scrollToShow (session.getPosition());
        return true;
    }
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
    {
        const int row = juce::jlimit (0, (int) state.rows.size() - 1, state.rowOf (session.selectedTrack) + (code == juce::KeyPress::upKey ? -1 : 1));
        if (row >= 0 && row < (int) state.rows.size())
            state.selectTrack (state.rows[(size_t) row].id);
        return true;
    }
    if (mods.isAnyModifierKeyDown() && ! mods.isShiftDown())
        return false;

    if (is ('r'))
    {
        if (session.isRecording())
            session.stop();
        else if (auto r = session.record(); r.failed())
            status (r.getErrorMessage());
        return true;
    }
    if (is ('l'))
    {
        if (p.getLoopEnd() <= p.getLoopStart())
            status ("Drag in the ruler to set the loop range");
        session.setLoopEnabled (! session.isLoopEnabled());
        ruler.repaint();
        lanes.repaint();
        return true;
    }
    if (is ('m'))
    {
        session.addMarkerAtPosition();
        return true;
    }
    if (is ('s'))
    {
        const auto at = mods.isShiftDown() ? lanes.positionUnderMouse() : std::optional<int64> (session.getPosition());
        if (at.has_value())
            state.splitAt (mods.isShiftDown() ? state.snapToGrid (*at) : *at);
        return true;
    }
    if (is ('f'))
    {
        state.zoomToFit();
        return true;
    }
    if (ch == '+' || ch == '=')
    {
        state.zoomAround (state.samplesPerPixel / 1.5, state.xOf (session.getPosition()));
        return true;
    }
    if (ch == '-')
    {
        state.zoomAround (state.samplesPerPixel * 1.5, state.xOf (session.getPosition()));
        return true;
    }
    return false;
}

} // namespace livedaw::ui
