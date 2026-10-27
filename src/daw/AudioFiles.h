#pragma once

#include "daw/Project.h"
#include "engine/AudioFileFormat.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <functional>

namespace livedaw::daw
{

/** "*.wav;*.aif;..." for file choosers (`formats` holds the formats a project
    can import: registerBasicFormats() gives WAV, AIFF, FLAC, MP3 and the
    platform's codecs). */
juce::String importWildcard (juce::AudioFormatManager& formats);
bool canImport (juce::AudioFormatManager& formats, const juce::File&);

/** A file name inside `folder` that doesn't exist yet: "<base><ext>",
    "<base>-2<ext>", ... (characters that aren't allowed in file names removed;
    `extension` with its dot). */
juce::File uniqueAudioFile (const juce::File& folder, const juce::String& base, const juce::String& extension = ".wav");

/** A 32-bit float WAV writer for `file` (replacing it), or nullptr. */
std::unique_ptr<juce::AudioFormatWriter> createWavWriter (const juce::File& file, double sampleRate, int numChannels);

struct ImportResult
{
    juce::Result result = juce::Result::ok();
    juce::File file;        // the WAV written in the project's Audio folder (or the source, referenced)
    int64 length = 0;       // samples at the project's rate
    int numChannels = 1;
    juce::String name;      // the source's name
    bool referenced = false;   // the clip plays the source where it is (KEEP IN PLACE)
    juce::String copyReason;   // KEEP IN PLACE fell back to a copy: why ("resampled copy: ...")
};

/** "44.1", "48", "88.2" (kHz, for messages). */
juce::String khzText (double sampleRate);

/** Whether a project at `sampleRate` can play `source` where it is (IMPORT:
    KEEP IN PLACE): the recorder's player reads it as it is (WAV, AIFF or FLAC,
    seeking sample-accurately; MP3 and other compressed formats are decoded
    into a copy), at the project's rate, with one or two channels, and the
    editor's waveforms read the same length. Returns empty if it can (filling
    `length` and `numChannels`), otherwise why it gets a converted copy:
    "resampled copy: 44.1 kHz file in a 48 kHz project". */
juce::String whyNotInPlace (juce::AudioFormatManager& formats, const juce::File& source, double sampleRate,
                            int64* length = nullptr, int* numChannels = nullptr);

/** IMPORT with KEEP IN PLACE (`keepInPlace`): references `source` when
    whyNotInPlace() allows it, otherwise (and always without `keepInPlace`)
    converts it into `folder` with importAudioFile() (copyReason says why). */
ImportResult importOrReference (juce::AudioFormatManager& formats, const juce::File& source, const juce::File& folder, double sampleRate,
                                bool keepInPlace, const std::atomic<bool>& cancel, std::function<void (float)> progress = {});

/** Copies a file byte for byte (COLLECT FILES), replacing `dest`; a failed or
    cancelled copy leaves no `dest`. Any thread. */
juce::Result copyFile (const juce::File& source, const juce::File& dest, const std::atomic<bool>& cancel,
                       std::function<void (float)> progress = {});

/** Converts an audio file to a 32-bit float WAV at `sampleRate` in `folder`
    (resampled with a windowed sinc; more than two channels keep the first
    two). Runs on any thread (`formats` is only read); `cancel` stops it,
    `progress` gets 0..1. */
ImportResult importAudioFile (juce::AudioFormatManager& formats, const juce::File& source, const juce::File& folder, double sampleRate,
                              const std::atomic<bool>& cancel, std::function<void (float)> progress = {});

/** A clip to render, with its file resolved. */
struct RenderClip
{
    juce::File file;
    ClipData clip;
};

/** Renders a track's clips (clip gain, fades and the track gain; overlaps
    summed as in playback) from the timeline's start to `length` into a file
    in `format` (32-bit float WAV by default; WAV / AIFF / FLAC / MP3), so
    exported tracks line up. Runs on any thread. */
juce::Result renderTrack (juce::AudioFormatManager& formats, const std::vector<RenderClip>& clips, float trackGainDb, int numChannels, double sampleRate, int64 length,
                          const juce::File& dest, const std::atomic<bool>& cancel, std::function<void (float)> progress = {},
                          AudioFileFormat format = AudioFileFormat::wav (32));

} // namespace livedaw::daw
