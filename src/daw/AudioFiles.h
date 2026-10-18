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
    juce::File file;        // the WAV written in the project's Audio folder
    int64 length = 0;       // samples at the project's rate
    int numChannels = 1;
    juce::String name;      // the source's name
};

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
