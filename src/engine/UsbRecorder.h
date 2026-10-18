#pragma once

#include "engine/Strip.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <vector>

namespace livedaw
{

/** The WING's USB recorder / player (manual 8.2 "4-Track USB Recorder"; WING
    tree /rec and /play), over the engine's ld_usb_* functions.

    - "USB" storage is a folder on disk (default <documents>/Live DAW/USB,
      changeable): the browser lists its folders, WAV files and playlists,
      creates folders, renames and deletes.
    - Recorder: records the RECORDER output group (four ports, each a strip's
      output or a source, routed on ROUTING > OUTPUTS) to a 2- or 4-channel,
      16- or 24-bit WAV file "R_YYYYMMDD-HHMMSS.wav" in the REC PATH folder,
      at the engine's rate. REC, PAUSE, STOP, NEW FILE.
    - Player: plays WAV clips (any rate: resampled and flagged) into the USB
      PLAYER source group (kNumChannels sources at kPlayerSourceBase), which
      channels patch like any input. What it plays is the queue: the files of
      the browser's folder (one at a time) or a playlist (SKIP, STOP after,
      REPEAT; M3U files).

    Message thread only. */
class UsbRecorder
{
public:
    static constexpr int kNumChannels = 4;
    static constexpr int kPlayerSourceBase = 0x20000;
    static constexpr const char* kPlaylistExtension = ".m3u";

    explicit UsbRecorder (ld::Engine* engine);
    ~UsbRecorder();

    /** Input index of USB PLAYER channel n, and back (-1 if `input` isn't one). */
    static constexpr int playerSource (int channel) noexcept { return kPlayerSourceBase + channel; }
    static constexpr int playerChannelOf (int input) noexcept
    {
        return input >= kPlayerSourceBase && input < kPlayerSourceBase + kNumChannels ? input - kPlayerSourceBase : -1;
    }
    static constexpr bool isPlayerSource (int input) noexcept { return playerChannelOf (input) >= 0; }
    /** "PLAYER 1" .. "PLAYER 4" (the USB PLAYER source group). */
    static juce::String playerSourceName (int channel) { return "PLAYER " + juce::String (channel + 1); }

    //==============================================================================
    // Storage

    /** The default "USB stick": <documents>/Live DAW/USB. */
    static juce::File defaultStorageFolder();
    juce::File getStorageFolder() const { return storage; }
    /** Changes the storage folder (created if missing); the browser and REC PATH
        move inside it if they were outside. */
    void setStorageFolder (const juce::File&);

    enum class ItemKind
    {
        Folder,
        Wav,
        Playlist
    };

    struct Item
    {
        juce::File file;
        ItemKind kind = ItemKind::Wav;
        juce::Time modified;
    };

    /** The folders, WAV files and playlists in `folder`: folders first, then by name. */
    static std::vector<Item> listFolder (const juce::File& folder);
    /** FOLDER: a new folder in `parent`. */
    juce::Result createFolder (const juce::File& parent, const juce::String& name);
    /** RENAME (the extension of a file stays). */
    juce::Result rename (const juce::File& item, const juce::String& newName, juce::File* renamed = nullptr);
    /** DELETE (a folder with its contents). */
    juce::Result deleteItem (const juce::File& item);
    /** Whether `f` is the storage folder or inside it. */
    bool isInsideStorage (const juce::File& f) const;

    //==============================================================================
    // Recorder

    enum class RecState
    {
        Stopped,
        Recording,
        Paused,
        Error
    };

    RecState getRecState() const;
    bool isRecording() const { return getRecState() == RecState::Recording || getRecState() == RecState::Paused; }

    /** 2-CH / 4-CH and 16 bit / 24 bit (the next recording's format). */
    int getRecChannels() const noexcept { return recChannels; }
    int getRecBits() const noexcept { return recBits; }
    void setRecChannels (int channels) noexcept { recChannels = channels <= 2 ? 2 : 4; }
    void setRecBits (int bits) noexcept { recBits = bits <= 16 ? 16 : 24; }
    /** USB HOST SPEED FS: 2-channel 16-bit recording only (HS: 4-track 24-bit). */
    void setFullSpeedOnly (bool fs) noexcept { fullSpeed = fs; }
    bool isFullSpeedOnly() const noexcept { return fullSpeed; }

    /** REC PATH: the folder new recordings go to. */
    juce::File getRecPath() const { return recPath; }
    void setRecPath (const juce::File& folder) { recPath = folder; }

    /** A new file name in the REC PATH: R_YYYYMMDD-HHMMSS.wav (-2, -3 ... if taken). */
    juce::File nextRecordingFile (juce::Time when = juce::Time::getCurrentTime()) const;

    /** REC: starts a new recording, or resumes a paused one. */
    juce::Result record();
    /** PAUSE: pauses, or resumes when paused. */
    void toggleRecPause();
    void stopRecording();
    /** NEW FILE: the recording goes on in a new file from the next sample. */
    juce::Result newFile();

    /** The file being (or last) recorded. */
    juce::File getRecFile() const;
    /** Record time of the current file and of the whole recording, in samples. */
    int64_t getRecTime() const;
    int64_t getRecTotal() const;
    /** RECORDER port `port`'s level in dBFS (peak since the last call). */
    float getRecLevel (int port);

    /** What a RECORDER port records (ROUTING > OUTPUTS, group RECORDER). */
    struct Tap
    {
        enum class Kind
        {
            Off,
            Source, // an input index (hardware input, playback channel, USB player)
            Strip   // a strip's output: input / aux channels at their direct out (pre-fader)
        };

        Kind kind = Kind::Off;
        int source = -1;
        StripKind stripKind = StripKind::Main;
        int stripNumber = 1; // as shown: CH 3 = 3, BUS 12 = 12
        int channel = 0;     // 0 left, 1 right

        int code() const noexcept;
        static Tap fromCode (int code) noexcept;
        bool operator== (const Tap& o) const noexcept { return code() == o.code(); }
    };

    Tap getTap (int port) const;
    void setTap (int port, const Tap&);

    //==============================================================================
    // Player

    enum class PlayState
    {
        Stopped,
        Playing,
        Paused,
        Error
    };

    PlayState getPlayState() const;
    void play();
    void pause();
    /** PAUSE key: pauses, or goes on when paused. */
    void togglePlayPause();
    void stop();
    /** Plays (or cues) a queue entry from the start. */
    void playEntry (uint64_t entry, bool play = true);
    /** PLAY on a file of the browser or the playlist: plays it, or goes on if it is the one paused. */
    void playFile (const juce::File&);
    /** Moves the position in the current clip (samples). */
    void seek (int64_t position);
    bool next();
    bool previous();

    uint64_t getCurrentEntry() const;
    /** The clip playing or cued (empty if none). */
    juce::File getCurrentFile() const;
    int64_t getPosition() const;
    /** The current clip's length in samples (0 until it has been opened). */
    int64_t getLength() const;
    double getSampleRate() const;
    /** USB PLAYER channel `ch`'s level in dBFS (peak since the last call). */
    float getPlayLevel (int ch);

    struct FileInfo
    {
        int channels = 0, sampleRate = 0, bits = 0;
        int64_t frames = 0; // in the file's own rate
    };

    /** A WAV file's header (false if it can't be read). */
    static bool readWavInfo (const juce::File&, FileInfo&);

    /** The current clip as the player opened it: length in engine samples and
        whether its rate differs from the engine's (it is resampled). */
    bool getCurrentInfo (FileInfo&, int64_t& length, bool& rateMismatch) const;

    //==============================================================================
    // The queue: the browser folder's files, or a playlist

    struct Entry
    {
        uint64_t id = 0;
        juce::File file;
        bool skip = false, stop = false;
    };

    std::vector<Entry> getQueue() const;
    /** Bumped on every change of the queue. */
    uint64_t getQueueVersion() const;
    int indexOf (uint64_t entry) const;
    /** The browser's folder: the queue plays its files (unless a playlist is open). */
    void setBrowserFolder (const juce::File&);
    juce::File getBrowserFolder() const { return browserFolder; }
    /** Re-reads the folder (after files changed). */
    void refreshQueue();

    bool isPlaylistOpen() const;
    /** The playlist's file (empty while unsaved). */
    juce::File getPlaylistFile() const;
    bool isPlaylistDirty() const;
    /** CREATE PLAYLIST: an empty one. */
    void createPlaylist();
    juce::Result openPlaylist (const juce::File&);
    /** SAVE PLAYLIST as `file` (".m3u" is added if missing). */
    juce::Result savePlaylist (const juce::File& file);
    void closePlaylist();
    /** ADD: a clip (a playlist is created if none is open). Returns its id. */
    uint64_t add (const juce::File&);
    /** ADD ALL: the WAV files of a folder. */
    int addAll (const juce::File& folder);
    void remove (int index);
    /** UP (-1) / DOWN (+1): the entry's new index or -1. */
    int move (int index, int delta);
    void setSkip (int index, bool);
    void setStop (int index, bool);
    void setStopAll (bool);
    bool getRepeat() const;
    void setRepeat (bool);

    //==============================================================================
    /** The settings kept between runs (storage, REC PATH, browser folder,
        format, RECORDER taps). */
    void loadSettings (juce::PropertiesFile&);
    void saveSettings (juce::PropertiesFile&) const;

    /** Disk under/overruns and write errors since the app started. */
    int getDropouts() const;
    /** Offline use (tests): waits until the disk thread caught up. */
    bool waitForDisk (int timeoutMs = 10000);

private:
    ld::Engine* engine;
    juce::File storage, recPath, browserFolder;
    int recChannels = 2, recBits = 24;
    bool fullSpeed = false;

    JUCE_DECLARE_NON_COPYABLE (UsbRecorder)
};

} // namespace livedaw
