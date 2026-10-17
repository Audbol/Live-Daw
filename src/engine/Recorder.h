#pragma once

#include "engine/Strip.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <vector>

namespace livedaw
{

/** The engine's multitrack recorder and player.

    - A transport with a position on the timeline (in samples at the engine's
      sample rate), play / record / stop / locate and a loop range.
    - Recording: up to kMaxRecordTracks sources (hardware inputs or strip taps)
      are written to WAV files on a disk thread. The audio thread only copies
      into lock-free buffers.
    - Playback: the UI hands over a flattened Playlist (tracks of clips with
      gain and fades). A disk thread renders it ahead of the play position into
      kNumPlaybackChannels playback channels, which are sources like hardware
      inputs. A channel takes one as its input, directly or as its ALT input
      (virtual soundcheck): input index kPlaybackSourceBase + n is playback
      channel n.

    The project itself (tracks, clips, edits, undo, files) belongs to the UI.
    The recorder only knows the playlist it was last given and the takes it
    recorded. Every method is for the message thread.

    The engine side is rust/crates/livedaw-recorder (ld_rec_*): the audio
    thread owns the transport and only copies to / from lock-free rings; a
    reader thread renders the playlist ahead of the play position, a writer
    thread writes the takes. */
class Recorder
{
public:
    static constexpr int kNumPlaybackChannels = 64;
    static constexpr int kMaxRecordTracks = 64;
    /** Input index of playback channel 0 (StripParams input and ALT input fields). */
    static constexpr int kPlaybackSourceBase = 0x10000;
    /** Resolution of the peaks returned by getRecordingPeaks(). */
    static constexpr int kSamplesPerPeak = 256;

    explicit Recorder (ld::Engine* engine);
    ~Recorder();

    /** Input index of playback channel n, and back (-1 if `input` isn't one). */
    static constexpr int playbackSource (int channel) noexcept { return kPlaybackSourceBase + channel; }
    static constexpr int playbackChannelOf (int input) noexcept
    {
        return input >= kPlaybackSourceBase && input < kPlaybackSourceBase + kNumPlaybackChannels ? input - kPlaybackSourceBase : -1;
    }
    static constexpr bool isPlaybackSource (int input) noexcept { return playbackChannelOf (input) >= 0; }
    /** "PLAY 1" .. "PLAY 64". */
    static juce::String playbackSourceName (int channel) { return "PLAY " + juce::String (channel + 1); }

    //==============================================================================
    // Transport

    enum class State
    {
        Stopped,
        Starting,  // filling the playback buffers before rolling
        Playing,
        Recording
    };

    State getState() const;
    bool isRolling() const { return getState() == State::Playing || getState() == State::Recording; }

    /** The play position on the timeline, in samples. */
    int64_t getPosition() const;
    /** The engine's sample rate (the timeline's unit is one sample at this rate). */
    double getSampleRate() const;

    /** Plays from the current position. */
    void play();
    /** Stops playback; a recording is finished and its takes are finalized. */
    void stop();
    /** Moves the play position (also while rolling: playback continues from there). */
    void locate (int64_t position);

    /** Playback wraps from `end` to `start` while enabled (not while recording). */
    void setLoop (int64_t start, int64_t end, bool enabled);
    bool isLoopEnabled() const;
    int64_t getLoopStart() const;
    int64_t getLoopEnd() const;

    //==============================================================================
    // Playback

    enum class FadeShape
    {
        Linear,
        EqualPower,
        SCurve
    };

    /** A region of an audio file placed on the timeline. Overlapping clips on a
        track are summed (fades make crossfades). */
    struct Clip
    {
        juce::File file;            // a WAV file (16/24/32-bit int or 32-bit float)
        int64_t fileOffset = 0;     // first sample of the file that plays
        int64_t start = 0;          // timeline position of that sample
        int64_t length = 0;         // samples
        float gain = 1.0f;          // linear
        int64_t fadeIn = 0, fadeOut = 0; // samples, inside the clip
        FadeShape fadeInShape = FadeShape::EqualPower, fadeOutShape = FadeShape::EqualPower;
    };

    struct PlaybackTrack
    {
        int channel = 0;        // first playback channel (0 .. kNumPlaybackChannels-1)
        int numChannels = 1;    // 1 or 2 (stereo uses channel and channel + 1)
        float gain = 1.0f;      // linear
        bool mute = false;
        std::vector<Clip> clips;
    };

    using Playlist = std::vector<PlaybackTrack>;

    /** Replaces what plays. While rolling, playback continues from the current
        position with the new content (after a short refill). */
    void setPlaylist (const Playlist&);

    //==============================================================================
    // Recording

    struct Source
    {
        enum class Kind
        {
            HardwareInput,  // a hardware input port, before any processing
            StripPreFader,  // a strip's signal at its pre-fader tap
            StripPostFader  // a strip's output after fader and pan
        };

        Kind kind = Kind::HardwareInput;
        int port = 0;                         // HardwareInput: first port (0-based)
        StripKind stripKind = StripKind::Channel;
        int stripNumber = 1;                  // Strip*: as shown ("CH 3" = 3, "MAIN 1" = 1)
    };

    struct RecordTrack
    {
        int track = 0;          // the UI's track index (reported back in its Take)
        Source source;
        int numChannels = 1;    // 1 or 2 (HardwareInput stereo: port and port + 1)
        juce::File file;        // the WAV file to create (32-bit float)
    };

    /** Starts recording the tracks from the current position. Playback of the
        playlist runs at the same time. Fails if the files can't be created, a
        source doesn't exist or nothing is to be recorded. */
    juce::Result record (const std::vector<RecordTrack>&);

    /** A finished recording of one track. `start` is the timeline position of its
        first sample as captured. The UI shifts it by the device's round-trip
        latency when overdubbing, if it wants to. */
    struct Take
    {
        int track = 0;
        juce::File file;
        int64_t start = 0;
        int64_t length = 0;
        int numChannels = 1;
    };

    /** The takes finalized since the last call (their files are complete). */
    std::vector<Take> takeFinishedTakes();

    /** The level of a recording track's input in dBFS, peak since the last call
        (for its meter). Works while armed tracks are recording. */
    float getRecordLevel (int track, int channel);

    /** Peaks of the recording in progress for a record track, for drawing it
        while it grows: min/max pairs (per channel interleaved: min0, max0,
        min1, max1, ...) of kSamplesPerPeak samples each, starting at peak
        `firstPeak`. Returns the number of peaks written (at most maxPeaks). */
    int getRecordingPeaks (int track, int64_t firstPeak, int maxPeaks, float* minMax) const;

    /** Samples recorded so far in the current recording. */
    int64_t getRecordedLength() const;

    //==============================================================================
    // Health

    /** Disk buffer under/overruns since the app started. */
    int getDropouts() const;
    /** The fullest record buffer / emptiest playback buffer, 0..1 (1 = in trouble). */
    float getDiskLoad() const;

    /** Offline use (tests, rendering faster than real time): waits until the
        disk threads have caught up with what they can do now. */
    bool waitForDisk (int timeoutMs = 10000);

private:
    ld::Engine* engine;

    JUCE_DECLARE_NON_COPYABLE (Recorder)
};

} // namespace livedaw
