#pragma once

#include "ui/daw/DawTimeline.h"
#include "ui/daw/DawTracks.h"

namespace livedaw::ui
{

/** Project file actions shared by the editor, the recorder screen and the
    surface: they ask before discarding unsaved changes and report failures. */
namespace dawfiles
{
    /** Runs `then` once the open project's unsaved changes are saved or
        discarded (asks first; nothing to ask for an empty untitled project). */
    void confirmDiscard (juce::Component* parent, daw::Session&, std::function<void()> then);
    /** NEW: asks for a name and creates the project in the recordings folder. */
    void newProject (juce::Component* parent, daw::Session&);
    /** Opens a project file (offering its autosave when that's newer). */
    void open (juce::Component* parent, daw::Session&, const juce::File&);
    /** OPEN...: a file chooser for .ldproj files. */
    void openWithChooser (juce::Component* parent, daw::Session&, std::unique_ptr<juce::FileChooser>&);
    void save (juce::Component* parent, daw::Session&);
    void saveAs (juce::Component* parent, daw::Session&);
    /** IMPORT...: audio files at the playhead on the selected track. */
    void importWithChooser (juce::Component* parent, daw::Session&, std::unique_ptr<juce::FileChooser>&);
    /** EXPORT: every track consolidated to a WAV in the project's Export folder. */
    void exportTracks (juce::Component* parent, daw::Session&);
    /** The FILE menu (new, open, recent, save, save as, import, export). */
    void showMenu (juce::Component* target, daw::Session&, std::unique_ptr<juce::FileChooser>&);
} // namespace dawfiles

/** The multitrack editor: a full-window view in place of the console. The
    top bar (CONSOLE, FILE, the project, transport, position, tempo and time
    signature, snap, zoom, undo, disk status), the track headers, the ruler
    with markers and loop range, the lanes with the clips, and the playhead,
    which follows while playing.

    Keys: space play/stop, R record, Home/End, L loop, M marker, S split at the
    playhead (Shift+S at the mouse), Delete, Ctrl+Z / Ctrl+Shift+Z, Ctrl+D,
    Ctrl+X/C/V, Ctrl+A, arrows nudge, Ctrl+arrows jump between markers, F zoom
    to fit, +/- zoom, Esc or Ctrl+E back to the console. */
class DawEditor final : public juce::Component,
                        public juce::FileDragAndDropTarget,
                        private daw::Project::Listener,
                        private juce::ScrollBar::Listener,
                        private juce::AsyncUpdater
{
public:
    DawEditor (UiContext&, daw::Session&);
    ~DawEditor() override;

    /** CONSOLE (or Esc / Ctrl+E): back to the console view. */
    std::function<void()> onClose;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void visibilityChanged() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

    class TopBar;

private:
    void projectChanged() override;
    void projectReplaced() override;
    void handleAsyncUpdate() override;
    void scrollBarMoved (juce::ScrollBar*, double) override;

    void frame();
    void viewChanged();
    void updateScrollBars();
    void layoutPlayhead();
    void status (const juce::String&);

    UiContext& ctx;
    daw::Session& session;
    DawEditorState state;
    DawWaveforms waves;
    std::unique_ptr<TopBar> topBar;
    DawTrackList trackList;
    DawRuler ruler;
    DawLanes lanes;
    DawPlayhead playhead;
    juce::ScrollBar hScroll { false }, vScroll { true };
    std::unique_ptr<juce::VBlankAttachment> vblank;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> corner;
    bool updatingScrollBars = false;
    int lastPlayheadX = -1000;

    JUCE_DECLARE_NON_COPYABLE (DawEditor)
};

} // namespace livedaw::ui
