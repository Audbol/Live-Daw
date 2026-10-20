#pragma once

#include "engine/Recorder.h"

#include <juce_data_structures/juce_data_structures.h>

#include <map>
#include <optional>
#include <vector>

namespace livedaw::daw
{

using int64 = juce::int64;
using FadeShape = Recorder::FadeShape;

/** The XML element and attribute names of a project file. */
namespace ids
{
#define LIVEDAW_DAW_ID(name) inline const juce::Identifier name (#name);
    LIVEDAW_DAW_ID (LDPROJECT)
    LIVEDAW_DAW_ID (TRACKS)
    LIVEDAW_DAW_ID (TRACK)
    LIVEDAW_DAW_ID (CLIP)
    LIVEDAW_DAW_ID (MARKERS)
    LIVEDAW_DAW_ID (MARKER)

    LIVEDAW_DAW_ID (version)
    LIVEDAW_DAW_ID (nextId)
    LIVEDAW_DAW_ID (id)
    LIVEDAW_DAW_ID (name)
    LIVEDAW_DAW_ID (sampleRate)
    LIVEDAW_DAW_ID (tempo)
    LIVEDAW_DAW_ID (sigNum)
    LIVEDAW_DAW_ID (sigDen)
    LIVEDAW_DAW_ID (loopStart)
    LIVEDAW_DAW_ID (loopEnd)
    LIVEDAW_DAW_ID (loopOn)

    LIVEDAW_DAW_ID (colour)
    LIVEDAW_DAW_ID (stereo)
    LIVEDAW_DAW_ID (srcKind)
    LIVEDAW_DAW_ID (srcPort)
    LIVEDAW_DAW_ID (srcStrip)
    LIVEDAW_DAW_ID (srcNumber)
    LIVEDAW_DAW_ID (arm)
    LIVEDAW_DAW_ID (mute)
    LIVEDAW_DAW_ID (solo)
    LIVEDAW_DAW_ID (gain)
    LIVEDAW_DAW_ID (playChannel)
    LIVEDAW_DAW_ID (height)

    LIVEDAW_DAW_ID (file)
    LIVEDAW_DAW_ID (fileOffset)
    LIVEDAW_DAW_ID (fileLength)
    LIVEDAW_DAW_ID (channels)
    LIVEDAW_DAW_ID (start)
    LIVEDAW_DAW_ID (length)
    LIVEDAW_DAW_ID (fadeIn)
    LIVEDAW_DAW_ID (fadeOut)
    LIVEDAW_DAW_ID (fadeInShape)
    LIVEDAW_DAW_ID (fadeOutShape)

    LIVEDAW_DAW_ID (position)
#undef LIVEDAW_DAW_ID
} // namespace ids

/** A region of an audio file placed on a track (a plain copy of a CLIP). */
struct ClipData
{
    int64 id = 0;
    juce::String file;      // as stored: relative to the project folder, or absolute
    int64 fileOffset = 0;   // first sample of the file that plays
    int64 fileLength = 0;   // samples in the file (limits trimming)
    int numChannels = 1;
    int64 start = 0;        // timeline position of fileOffset
    int64 length = 0;
    float gainDb = 0.0f;
    int64 fadeIn = 0, fadeOut = 0;
    FadeShape fadeInShape = FadeShape::EqualPower, fadeOutShape = FadeShape::EqualPower;
    bool mute = false;
    juce::String name;

    int64 end() const noexcept { return start + length; }
};

/** Edit arithmetic on clips (all positions in samples). */
namespace clipmath
{
    /** The shortest clip an edit leaves. */
    constexpr int64 kMinLength = 64;

    /** Keeps the fades inside the clip (they may meet, not cross). */
    void clampFades (ClipData&);
    /** Moves the clip's start to `newStart` (>= 0), keeping its content. */
    ClipData moveTo (ClipData, int64 newStart);
    /** Moves the left edge, revealing or hiding the start of the file. */
    ClipData trimStart (ClipData, int64 newStart);
    /** Moves the right edge, within the file. */
    ClipData trimEnd (ClipData, int64 newEnd);
    /** Splits at `at` (strictly inside the clip): the left and the right part.
        The left part keeps the fade-in, the right part the fade-out. */
    std::pair<ClipData, ClipData> split (const ClipData&, int64 at);
    /** True when `at` is far enough inside the clip to split it there. */
    bool canSplit (const ClipData&, int64 at);
    /** What remains of the clip outside [from, to): none, one or two parts. */
    std::vector<ClipData> removeRange (const ClipData&, int64 from, int64 to);

    /** The gain of a fade shape at x = 0..1 (fade-in; a fade-out uses 1 - x). */
    float fadeGain (FadeShape, float x) noexcept;
} // namespace clipmath

/** A track's settings (a plain copy of a TRACK). */
struct TrackData
{
    int64 id = 0;
    juce::String name;
    int colour = 1;              // WING colour 1..18
    bool stereo = false;
    Recorder::Source source;     // what it records
    bool arm = false, mute = false, solo = false;
    float gainDb = 0.0f;
    int playChannel = 0;         // first playback channel
    int height = 0;              // px in the editor (0 = default)
};

struct MarkerData
{
    int64 id = 0;
    int64 position = 0;
    juce::String name;
};

/** "IN 3", "CH 4 PRE", "BUS 2 POST" */
juce::String describeSource (const Recorder::Source&);

/** A multitrack project: tracks of clips, markers, a loop range, the tempo and
    time signature of the grid, and the sample rate of its timeline.

    The state is a juce::ValueTree and every edit goes through the project's
    UndoManager: call beginEdit() at the start of a user action, then make its
    changes. View settings (track heights) and record arming are not undoable.

    A saved project is a folder: <name>/<name>.ldproj (XML) and an Audio
    folder for its recordings and imported files. Clip files inside the folder
    are stored relative to it, so the folder can be moved. Message thread only. */
class Project final : private juce::ValueTree::Listener
{
public:
    static constexpr const char* fileExtension = ".ldproj";
    static constexpr int kFormatVersion = 1;
    static constexpr int kDefaultTrackHeight = 72;

    Project();
    ~Project() override;

    //==============================================================================
    // Files

    /** Starts a new, empty project, unsaved (no folder yet). */
    void clear (double sampleRate);

    /** Makes `folder` the project's folder (created with its Audio folder) and
        saves the project there as <folder name>.ldproj. */
    juce::Result saveAs (const juce::File& folder);
    /** Saves to the project's file (fails while it has no folder). */
    juce::Result save();
    /** Writes the project to `file` without making it the project's file (autosave). */
    juce::Result writeTo (const juce::File& file) const;
    /** Loads a project file. On failure the project is left unchanged. `asFile`
        overrides the file the project then belongs to (an autosave restored
        into its project). */
    juce::Result load (const juce::File& file, const juce::File& asFile = {});

    bool hasFolder() const noexcept { return folder != juce::File(); }
    juce::File getFolder() const { return folder; }
    juce::File getFile() const;
    juce::File getAudioFolder() const;
    juce::String getName() const;

    /** A clip's file on disk. */
    juce::File resolve (const juce::String& storedPath) const;
    /** How a file is stored in a clip (relative inside the project's folder). */
    juce::String storedPath (const juce::File&) const;

    /** The project's folder was moved or renamed to `newFolder` (clip paths
        inside it are relative, so they follow). */
    void relocate (const juce::File& newFolder) { folder = newFolder; }

    /** Changed since the last save or load. */
    bool isDirty() const noexcept { return changeCount != savedChangeCount; }
    void markDirty() noexcept { ++changeCount; }
    /** Increments with every change (for autosave and caches). */
    juce::uint32 getChangeCount() const noexcept { return changeCount; }

    //==============================================================================
    // Undo

    juce::UndoManager& getUndoManager() noexcept { return undo; }
    /** Starts a new undoable action; the changes until the next call undo together. */
    void beginEdit (const juce::String& actionName);
    bool canUndo() const { return undo.canUndo(); }
    bool canRedo() const { return undo.canRedo(); }
    bool undoLast() { return undo.undo(); }
    bool redoNext() { return undo.redo(); }

    juce::ValueTree& getState() noexcept { return state; }

    //==============================================================================
    // Timeline settings

    double getSampleRate() const;
    double getTempo() const;
    int getSigNumerator() const;
    int getSigDenominator() const;
    void setTempo (double bpm);
    void setTimeSignature (int numerator, int denominator);

    int64 getLoopStart() const;
    int64 getLoopEnd() const;
    bool isLoopOn() const;
    void setLoop (int64 start, int64 end);
    /** Not undoable (a transport switch, saved with the project). */
    void setLoopOn (bool);

    /** Samples per beat / bar at the project's tempo and rate. */
    double samplesPerBeat() const;
    double samplesPerBar() const;

    /** The end of the last clip. */
    int64 getLength() const;

    //==============================================================================
    // Tracks

    int getNumTracks() const;
    juce::ValueTree getTrack (int index) const;
    juce::ValueTree findTrack (int64 trackId) const;
    int indexOfTrack (int64 trackId) const;
    TrackData getTrackData (int index) const;
    std::vector<TrackData> getTracks() const;

    /** Adds a track at `index` (-1: at the end). Ids, names and channels are
        filled in where `t` leaves them 0 / empty. Returns its id. */
    int64 addTrack (TrackData t = {}, int index = -1);
    void removeTrack (int64 trackId);
    void moveTrack (int64 trackId, int newIndex);
    /** Undoable unless `undoable` is false (heights, arming). */
    void setTrackProperty (int64 trackId, const juce::Identifier&, const juce::var&, bool undoable = true);
    void setTrackSource (int64 trackId, const Recorder::Source&);

    /** The lowest playback channel no track uses (with room for `numChannels`). */
    int freePlaybackChannel (int numChannels) const;
    bool anySolo() const;

    //==============================================================================
    // Clips

    std::vector<ClipData> getClips (int64 trackId) const;
    std::optional<ClipData> getClip (int64 clipId) const;
    juce::ValueTree findClip (int64 clipId) const;
    /** The id of the track holding a clip, 0 if none. */
    int64 trackOfClip (int64 clipId) const;

    /** Adds a clip to a track (a new id is assigned). Returns its id. */
    int64 addClip (int64 trackId, ClipData);
    void removeClip (int64 clipId);
    /** Writes every field of `c` (but its id) to the clip with c.id. */
    void setClip (const ClipData& c);
    void setClipProperty (int64 clipId, const juce::Identifier&, const juce::var&);
    void moveClipToTrack (int64 clipId, int64 trackId);

    /** Splits the clips that contain `at`. Returns the ids of the right-hand parts. */
    std::vector<int64> splitClips (const std::vector<int64>& clipIds, int64 at);
    /** Removes [from, to) from every clip of the track (trimming and splitting
        them), except the clip `keep`. */
    void clearRange (int64 trackId, int64 from, int64 to, int64 keep = 0);
    /** Gives every partial overlap on the track a crossfade: the earlier clip
        fades out and the later one fades in across the overlap. */
    void crossfadeOverlaps (int64 trackId);

    /** A crossfade: `first` fades out and `second` fades in over `length`
        samples where they overlap. */
    struct Crossfade
    {
        int64 first = 0, second = 0, length = 0;
    };
    /** The crossfades on a track (overlaps the fades exactly span). */
    std::vector<Crossfade> getCrossfades (int64 trackId) const;
    /** Takes back the fades of crossfades whose clips no longer overlap (one
        moved away, to another track or was removed): a fade still as long as
        the crossfade goes back to none. */
    void releaseCrossfades (const std::vector<Crossfade>&);

    //==============================================================================
    // Markers

    std::vector<MarkerData> getMarkers() const; // sorted by position
    int64 addMarker (int64 position, const juce::String& name = {});
    void removeMarker (int64 markerId);
    void setMarker (int64 markerId, int64 position, const juce::String& name);

    //==============================================================================
    // Playback and recording

    /** The flattened playlist for the recorder: muted tracks (and, while any
        track is soloed, the tracks that aren't) are muted, muted clips and
        clips of missing files left out, gains made linear. */
    Recorder::Playlist buildPlaylist() const;

    /** Adds finished takes as clips: `trackOfRecordIndex` maps each take's
        Take::track to a track id. With `replace`, the take's range is cleared
        from the clips already on the track. Returns the new clip ids. */
    std::vector<int64> addTakes (const std::vector<Recorder::Take>&, const std::map<int, int64>& trackOfRecordIndex, bool replace);

    //==============================================================================
    // Conversions

    static ClipData clipFromTree (const juce::ValueTree&);
    static TrackData trackFromTree (const juce::ValueTree&);
    static void writeClip (juce::ValueTree&, const ClipData&, juce::UndoManager*);

    struct Listener
    {
        virtual ~Listener() = default;
        /** Anything in the project changed. */
        virtual void projectChanged() {}
        /** Another project was loaded or a new one started. */
        virtual void projectReplaced() {}
    };

    void addListener (Listener* l) { listeners.add (l); }
    void removeListener (Listener* l) { listeners.remove (l); }

private:
    int64 newId();
    juce::ValueTree tracksTree() const;
    juce::ValueTree markersTree() const;
    void changed();
    void replaced();
    static juce::ValueTree makeEmpty (double sampleRate);

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { changed(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { changed(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { changed(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override { changed(); }

    juce::ValueTree state;
    juce::UndoManager undo { 1000000, 200 };
    juce::File folder;
    juce::uint32 changeCount = 0, savedChangeCount = 0;
    bool loading = false;
    juce::ListenerList<Listener> listeners;

    JUCE_DECLARE_NON_COPYABLE (Project)
};

} // namespace livedaw::daw
