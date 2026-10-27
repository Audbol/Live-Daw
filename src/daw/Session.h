#pragma once

#include "daw/AudioFiles.h"
#include "daw/Project.h"

#include <juce_events/juce_events.h>

#include <map>
#include <memory>
#include <optional>

namespace livedaw
{
class MixerEngine;
}

namespace livedaw::daw
{

/** The console's recording session: the open Project tied to the engine's
    Recorder. It hands the recorder a new playlist whenever the project
    changes, follows the loop range, starts recordings of the armed tracks and
    turns their takes into clips, keeps the peaks of the takes in progress and
    the record levels for the views, imports and exports audio on a worker
    thread, autosaves, and remembers recent projects.

    The editor, the recorder screen and the surface's transport keys share one
    Session. Message thread only (but the import/export jobs). */
class Session final : private juce::Timer, private Project::Listener
{
public:
    /** `settings` (may be null) keeps the recent projects. */
    Session (Recorder&, juce::PropertiesFile* settings);
    ~Session() override;

    Project& getProject() noexcept { return project; }
    Recorder& getRecorder() noexcept { return recorder; }
    juce::AudioFormatManager& getFormats() noexcept { return formats; }

    //==============================================================================
    // Projects

    /** Where new projects go unless the user chose another folder:
        <documents>/Sidecar/Recordings. */
    static juce::File getDefaultFolder();
    /** Overrides the default folder (tests). */
    void setDefaultFolder (const juce::File& f) { defaultFolder = f; }
    /** Where new projects (and their recordings) go: the folder chosen in the
        editor's FILE menu (daw.projectsFolder; another drive, for instance),
        otherwise the default folder. */
    juce::File getProjectsFolder() const;
    /** FILE > Projects folder: an empty File goes back to the default. Kept in
        the app settings; existing projects stay where they are. */
    void setProjectsFolder (const juce::File&);
    /** Whether the user chose a projects folder (not the default). */
    bool hasCustomProjectsFolder() const;
    static constexpr const char* projectsFolderKey = "daw.projectsFolder";

    /** A new, untitled project at the engine's rate (it gets a folder when
        first saved, recorded into or imported into). */
    void newProject();
    /** A new project saved as `parent`/`name` (the default folder if `parent`
        is empty). */
    juce::Result createProject (const juce::String& name, const juce::File& parent = {});
    /** Opens a project file. With `restoreAutosave`, its newer autosave is
        loaded instead (and the project stays unsaved). */
    juce::Result openProject (const juce::File& file, bool restoreAutosave = false);
    juce::Result save();
    juce::Result saveAs (const juce::File& folder);
    /** Gives an untitled project a dated folder in the default folder. */
    juce::Result ensureFolder();

    /** The autosave beside a project file. */
    static juce::File autosaveFileFor (const juce::File& projectFile);
    /** Whether `projectFile` has an autosave newer than itself (unsaved changes). */
    static bool hasNewerAutosave (const juce::File& projectFile);
    /** Writes the autosave now if the project has unsaved changes. */
    void autosave();

    juce::StringArray getRecentProjects() const;
    /** Project files in the default folder, newest first. */
    std::vector<juce::File> getSessions() const;
    /** Moves a project's folder to the trash (not the open one). */
    juce::Result deleteSession (const juce::File& projectFile);
    /** Renames a project's folder and file. */
    juce::Result renameSession (const juce::File& projectFile, const juce::String& newName, juce::File* renamed = nullptr);

    //==============================================================================
    // Transport (positions on the project's timeline, in samples)

    Recorder::State getState() const { return recorder.getState(); }
    bool isRolling() const { return recorder.isRolling(); }
    /** Recording, or priming to record (the recorder fills its buffers first). */
    bool isRecording() const
    {
        const auto s = recorder.getState();
        return s == Recorder::State::Recording || (recordRequested && s == Recorder::State::Starting);
    }
    int64 getPosition() const { return recorder.getPosition(); }
    void play();
    void stop();
    void togglePlay();
    /** Records the armed tracks from the current position. */
    juce::Result record();
    void locate (int64 position);
    /** Jumps to the previous / next marker (or the start / end). */
    void jumpToMarker (bool forward);
    void setLoopEnabled (bool);
    bool isLoopEnabled() const { return project.isLoopOn(); }
    /** MODE: PLAY, A->B (from the loop range's start, stopping at its end) or
        LOOP, as the WING-LIVE (manual 8.1 fig. 75). Kept in the project; the
        app settings' recorder.playMode follows it for the WING remote
        (/cards/wlive/1/cfg/playmode). */
    using PlayMode = Recorder::PlayMode;
    PlayMode getPlayMode() const;
    void setPlayMode (PlayMode);
    static constexpr const char* playModeKey = "recorder.playMode";

    /** The transport as AUTO INPUT SELECT sees it (manual 8.1 p. 85-86). */
    enum class TransportMode
    {
        Stop,
        Play,
        Record
    };
    TransportMode getTransportMode() const;
    /** Called on the message thread when getTransportMode() changes. */
    std::function<void (TransportMode)> onTransportModeChanged;

    /** AUTO INPUT SELECT (the recorder's SETTINGS, manual 8.1 p. 85-86, WING
        /cards/wlive/autoin, auto_stop, auto_play, auto_rec): what the stop,
        play or record status asks of the channels in GLOBAL INPUT SELECT
        ENABLE routed to or from the recorder (isRoutedToRecorder), from the app
        settings: nothing (OFF, or KEEP), MAIN (false) or ALT (true). */
    static std::optional<bool> autoInputFor (const juce::PropertiesFile*, TransportMode);
    /** A channel AUTO INPUT SELECT switches (the WING: on WLIVE REC or WLIVE
        PLAY): its MAIN or ALT input is a playback channel, or a track
        records its strip or its MAIN input's port (`mainInputPort`). */
    bool isRoutedToRecorder (StripKind kind, int number, int mainInputPort, int altInputPort) const;
    /** AUTO INPUT SELECT on a change to `mode`: switches the GLOBAL input and
        aux channels routed to or from the recorder to what the settings ask.
        Returns how many channels it switched. */
    int applyAutoInput (MixerEngine&, TransportMode) const;
    static constexpr const char* autoInputKey = "recorder.autoInput";
    /** KEEP (0), MAIN (1) or ALT (2) per status. */
    static constexpr const char* autoInputModeKeys[3] = { "recorder.autoStop", "recorder.autoPlay", "recorder.autoRec" };
    /** SHOW METERS: the recorder screen shows the meters of the armed tracks. */
    static constexpr const char* showMetersKey = "recorder.showMeters";
    /** Adds a marker at the play position. */
    int64 addMarkerAtPosition();
    /** False when the project's rate differs from the engine's (playback would
        run at the wrong speed). */
    bool rateMatches() const;

    //==============================================================================
    // Recording state

    bool isRecordingTrack (int64 trackId) const;
    int64 getRecordStart() const noexcept { return recordStart; }
    int64 getRecordedLength() const { return isRecording() ? recorder.getRecordedLength() : 0; }
    /** Peaks of the take in progress on a track (min/max per channel,
        interleaved, Recorder::kSamplesPerPeak samples each), or nullptr. */
    const std::vector<float>* getRecordingPeaks (int64 trackId, int& numChannels) const;
    /** The record level of a track in dBFS (held and decaying), -100 if none. */
    float getTrackLevel (int64 trackId, int channel) const;
    int getNumArmed() const;
    int getNumArmedChannels() const;
    juce::int64 getFreeDiskBytes() const;
    /** How long the armed tracks can record on the project's disk, in seconds. */
    double getRecordSecondsLeft() const;

    //==============================================================================
    // Setups

    struct ChannelInfo
    {
        int number = 1;          // as shown: CH 1 = 1
        juce::String name;       // empty: "CH n"
        int colour = 1;
        bool stereo = false;
        int inputPort = -1;      // the channel's input port (-1: none patched)
    };

    /** RECORD ALL CHANNELS: a track per channel (the first tracks are reused,
        so their clips stay), named and coloured like it, recording its input
        port (or its pre-fader signal when nothing is patched), armed, playing
        back on consecutive playback channels. One undoable action. */
    void setupRecordAllChannels (const std::vector<ChannelInfo>&);
    void armAll (bool arm);

    /** VIRTUAL SOUNDCHECK: which channel plays back which track. A track
        recording a channel's strip (pre / post fader) or the input port the
        channel is patched to feeds that channel; each channel once. */
    struct SoundcheckRoute
    {
        int channelNumber = 1;
        int playChannel = 0;   // the track's first playback channel
        bool stereo = false;
    };
    std::vector<SoundcheckRoute> virtualSoundcheckRoutes (const std::vector<ChannelInfo>&) const;

    //==============================================================================
    // Import / export (one job at a time, on a worker thread)

    /** IMPORT (the editor's FILE menu, daw.importMode): COPY TO PROJECT
        converts every file into the project's Audio folder; KEEP IN PLACE
        references a file where it is when the project can play it as it is
        (see whyNotInPlace) and copies only the others; ASK EACH TIME lets the
        editor ask on every import. */
    enum class ImportMode
    {
        Copy = 0,
        KeepInPlace = 1,
        Ask = 2
    };
    ImportMode getImportMode() const;
    void setImportMode (ImportMode);
    static constexpr const char* importModeKey = "daw.importMode";

    /** Imports files as clips at `position`, the first on `trackId` (0: a new
        track) and each further one on the next track (added as needed).
        `keepInPlace` overrides the IMPORT setting (ASK EACH TIME without it
        copies). */
    void importFiles (const juce::Array<juce::File>&, int64 trackId, int64 position, std::optional<bool> keepInPlace = std::nullopt);
    /** COLLECT FILES INTO PROJECT: copies every file the clips play from
        outside the project's folder into its Audio folder, repoints the clips
        (one undoable action) and saves, so the folder can be moved or archived
        on its own. Offline files are left as they are (and reported). */
    void collectFiles();
    /** Clips playing files outside the project's folder (KEEP IN PLACE). */
    int getNumReferencedFiles() const;

    /** A clip's file is missing (a drive unplugged, a file moved): the clip
        shows OFFLINE and plays silence. Checked again about once a second;
        playback picks the file up when it comes back. */
    bool isOffline (const ClipData&) const;
    /** Distinct missing files of the project's clips. */
    juce::StringArray getOfflineFiles() const;
    /** Checks every clip's file now (normally on the timer). */
    void checkOfflineFiles();
    /** Renders every track to <folder>/<nn> <name><ext> in `format`. */
    void exportTracks (const juce::File& folder, AudioFileFormat format = AudioFileFormat::wav (32));

    /** The format takes are recorded in (32-bit float WAV by default; kept in
        the app settings as daw.recordFormat). */
    AudioFileFormat getRecordFormat() const noexcept { return recordFormat; }
    void setRecordFormat (AudioFileFormat);
    /** The format EXPORT last used (daw.exportFormat). */
    AudioFileFormat getExportFormat() const noexcept { return exportFormat; }
    bool isBusy() const noexcept { return busy; }
    juce::String getJobName() const { return jobName; }
    float getJobProgress() const noexcept { return jobProgress.load(); }
    void cancelJob() { cancel = true; }

    //==============================================================================
    // Shared editing state

    struct ClipboardItem
    {
        int trackOffset = 0;   // tracks below the first copied clip's track
        ClipData clip;         // start relative to the earliest copied clip
    };
    std::vector<ClipboardItem> clipboard;

    /** The track imports, pastes and the recorder screen work on. */
    int64 selectedTrack = 0;
    /** Overlapping clips get crossfades after moves, pastes and imports. */
    bool autoCrossfade = true;
    /** A take clears the range it covers on its track. */
    bool replaceOnRecord = true;

    /** A drag is changing the project: playlist updates are throttled. */
    void setGestureActive (bool active) { gesture = active; }

    /** Messages for the event log: (text, isError). */
    std::function<void (const juce::String&, bool)> onMessage;
    /** Called after a project was opened, created or cleared. */
    std::function<void()> onProjectReplaced;
    /** A clip's file went offline or came back (repaint the clips). */
    std::function<void()> onOfflineChanged;

    /** Pushes the playlist and loop to the recorder now (tests). */
    void pushPlaylistNow();
    /** Collects finished takes now (normally on the timer). */
    void collectTakes();

private:
    void timerCallback() override;
    void projectChanged() override;
    void projectReplaced() override;
    void message (const juce::String& text, bool error = false);
    void addRecent (const juce::File&);
    void pollLevels();
    void pollPeaks();
    void runJob (const juce::String& name, std::function<void (const std::atomic<bool>&, std::atomic<float>&)> work);

    Recorder& recorder;
    juce::PropertiesFile* settings;
    juce::AudioFormatManager formats;
    AudioFileFormat recordFormat, exportFormat;
    Project project;
    juce::File defaultFolder;
    // Files of the clips -> exists (refreshed on the timer).
    mutable std::map<juce::String, bool> fileExists;
    juce::uint32 lastOfflineCheck = 0;
    ImportMode importMode = ImportMode::Copy; // without settings
    void reportOffline();

    bool playlistDirty = true, gesture = false;
    juce::uint32 lastPush = 0, lastAutosave = 0;
    juce::uint32 lastLoopHash = 0;
    TransportMode shownTransport = TransportMode::Stop;
    void checkTransportMode();
    void publishPlayMode();

    // The recording in progress: record track index -> track id.
    std::map<int, int64> recordTracks;
    bool recordRequested = false;
    int64 recordStart = 0;
    struct Peaks
    {
        int numChannels = 1;
        std::vector<float> minMax;
    };
    std::map<int64, Peaks> peaks;
    struct Level
    {
        float db[2] { -100.0f, -100.0f };
    };
    std::map<int64, Level> levels;
    mutable juce::int64 freeBytes = 0;
    mutable juce::uint32 freeBytesTime = 0;

    // Jobs
    std::unique_ptr<juce::Thread> worker;
    std::atomic<bool> cancel { false };
    std::atomic<float> jobProgress { 0.0f };
    bool busy = false;
    juce::String jobName;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE (Session)
};

} // namespace livedaw::daw
