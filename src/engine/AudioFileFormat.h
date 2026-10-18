#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace ld
{
struct LdAudioWriter;
struct LdAudioReader;
} // namespace ld

namespace livedaw
{

/** The file formats both recorders write and play, and the DAW exports:
    WAV (16 / 24-bit, 32-bit float), FLAC (16 / 24-bit), AIFF (16 / 24-bit,
    32-bit float as AIFF-C) and MP3 (128 - 320 kbit/s). The encoders and
    decoders are the engine's (rust/crates/livedaw-recorder/src/codec). */
struct AudioFileFormat
{
    enum class Type
    {
        Wav = 0,
        Flac = 1,
        Aiff = 2,
        Mp3 = 3
    };

    Type type = Type::Wav;
    int bits = 32;   // 16, 24, 32 (32-bit float: WAV and AIFF); unused for MP3
    int kbps = 320;  // MP3

    static constexpr int kMp3Bitrates[] = { 128, 192, 256, 320 };

    static AudioFileFormat wav (int bits = 32) { return { Type::Wav, bits, 320 }; }
    static AudioFileFormat flac (int bits = 24) { return { Type::Flac, bits, 320 }; }
    static AudioFileFormat aiff (int bits = 24) { return { Type::Aiff, bits, 320 }; }
    static AudioFileFormat mp3 (int kbps = 320) { return { Type::Mp3, 16, kbps }; }

    /** The nearest format the encoders write (FLAC 16 / 24 bit, MP3 bit rates...). */
    AudioFileFormat normalized() const;
    bool operator== (const AudioFileFormat& o) const noexcept;
    bool operator!= (const AudioFileFormat& o) const noexcept { return ! (*this == o); }

    /** ".wav", ".flac", ".aif", ".mp3". */
    juce::String extension() const;
    /** "WAV", "FLAC", "AIFF", "MP3". */
    static juce::String typeName (Type);
    juce::String typeName() const { return typeName (type); }
    /** "WAV 32-BIT FLOAT", "FLAC 24-BIT", "MP3 320 KBPS". */
    juce::String describe() const;
    /** "WAV 32F", "FLAC 24", "MP3 320" (keys and chips). */
    juce::String shortName() const;
    /** Settings text: "wav:32", "flac:24", "mp3:320"; fromString falls back on errors. */
    juce::String toString() const;
    static AudioFileFormat fromString (const juce::String&, AudioFileFormat fallback);
    static AudioFileFormat fromString (const juce::String&);

    /** Why `channels` at `sampleRate` can't be recorded in this format (MP3 needs 32,
        44.1 or 48 kHz and at most two channels); empty if they can. */
    juce::String check (int channels, double sampleRate) const;

    /** The record format choices of the multitrack recorder (its settings). */
    static std::vector<AudioFileFormat> recordChoices();
    /** The export choices (the same, 16-bit ones included). */
    static std::vector<AudioFileFormat> exportChoices();

    /** Whether the file has the extension of one of the formats (.wav .flac .aif .aiff .aifc .mp3). */
    static bool isAudioFile (const juce::File&);
    /** The type a file's extension stands for (Wav for unknown ones). */
    static Type typeOfFile (const juce::File&);
};

/** Writes a file in an AudioFileFormat (the engine's encoders). Any thread. */
class AudioFileWriter
{
public:
    ~AudioFileWriter();

    /** Creates (or replaces) `file`; nullptr on failure (`error` says why). */
    static std::unique_ptr<AudioFileWriter> create (const juce::File& file, int numChannels, double sampleRate, AudioFileFormat,
                                                    juce::String* error = nullptr);

    bool write (const float* const* channels, int numChannels, int numFrames);
    /** Writes the final headers and closes the file; the frames written or -1. */
    int64_t finalize();
    /** Closes and deletes the file. */
    void abort();

private:
    AudioFileWriter() = default;
    ld::LdAudioWriter* handle = nullptr;
    JUCE_DECLARE_NON_COPYABLE (AudioFileWriter)
};

/** Reads WAV, FLAC, AIFF and MP3 files the way the recorders play them (MP3
    sample-aligned with what was recorded). Any thread. */
class AudioFileReader
{
public:
    ~AudioFileReader();

    static std::unique_ptr<AudioFileReader> open (const juce::File&);

    AudioFileFormat::Type getType() const noexcept { return type; }
    int getNumChannels() const noexcept { return numChannels; }
    double getSampleRate() const noexcept { return sampleRate; }
    int getBits() const noexcept { return bits; }
    int64_t getLength() const noexcept { return length; }
    int getKbps() const noexcept { return kbps; }

    /** Frames from `start` into `numChannels` buffers (extra ones repeat the last
        channel; zeros outside the file). */
    bool read (int64_t start, float* const* channels, int numChannels, int numFrames);

private:
    AudioFileReader() = default;
    ld::LdAudioReader* handle = nullptr;
    AudioFileFormat::Type type = AudioFileFormat::Type::Wav;
    int numChannels = 0, bits = 0, kbps = 0;
    double sampleRate = 0.0;
    int64_t length = 0;
    JUCE_DECLARE_NON_COPYABLE (AudioFileReader)
};

/** A juce::AudioFormat over AudioFileReader for MP3, registered ahead of JUCE's
    own in the project's AudioFormatManager so waveforms, import and export see
    MP3 takes sample-aligned with what the recorder plays. Reading only. */
class EngineMp3AudioFormat final : public juce::AudioFormat
{
public:
    EngineMp3AudioFormat();

    juce::Array<int> getPossibleSampleRates() override;
    juce::Array<int> getPossibleBitDepths() override;
    bool canDoStereo() override { return true; }
    bool canDoMono() override { return true; }
    bool isCompressed() override { return true; }
    juce::AudioFormatReader* createReaderFor (juce::InputStream*, bool deleteStreamIfOpeningFails) override;
    using juce::AudioFormat::createWriterFor;
    std::unique_ptr<juce::AudioFormatWriter> createWriterFor (std::unique_ptr<juce::OutputStream>&, const juce::AudioFormatWriterOptions&) override
    {
        return nullptr;
    }
};

} // namespace livedaw
