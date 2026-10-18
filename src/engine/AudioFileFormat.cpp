#include "engine/AudioFileFormat.h"
#include "engine/RustStrings.h"

namespace livedaw
{

static_assert ((int) AudioFileFormat::Type::Wav == ld::kFormatWav && (int) AudioFileFormat::Type::Flac == ld::kFormatFlac
               && (int) AudioFileFormat::Type::Aiff == ld::kFormatAiff && (int) AudioFileFormat::Type::Mp3 == ld::kFormatMp3);

AudioFileFormat AudioFileFormat::normalized() const
{
    AudioFileFormat f = *this;
    switch (type)
    {
        case Type::Flac: f.bits = bits <= 16 ? 16 : 24; break;
        case Type::Wav:
        case Type::Aiff: f.bits = bits <= 16 ? 16 : (bits <= 24 ? 24 : 32); break;
        case Type::Mp3: f.bits = 16; break;
    }
    int best = kMp3Bitrates[0];
    for (int b : kMp3Bitrates)
        if (std::abs (b - kbps) < std::abs (best - kbps))
            best = b;
    f.kbps = best;
    return f;
}

bool AudioFileFormat::operator== (const AudioFileFormat& o) const noexcept
{
    const auto a = normalized(), b = o.normalized();
    return a.type == b.type && (a.type == Type::Mp3 ? a.kbps == b.kbps : a.bits == b.bits);
}

juce::String AudioFileFormat::extension() const
{
    switch (type)
    {
        case Type::Wav: return ".wav";
        case Type::Flac: return ".flac";
        case Type::Aiff: return ".aif";
        case Type::Mp3: return ".mp3";
    }
    return ".wav";
}

juce::String AudioFileFormat::typeName (Type t)
{
    switch (t)
    {
        case Type::Wav: return "WAV";
        case Type::Flac: return "FLAC";
        case Type::Aiff: return "AIFF";
        case Type::Mp3: return "MP3";
    }
    return "WAV";
}

juce::String AudioFileFormat::describe() const
{
    const auto f = normalized();
    if (f.type == Type::Mp3)
        return "MP3 " + juce::String (f.kbps) + " KBPS";
    return typeName (f.type) + " " + (f.bits == 32 ? juce::String ("32-BIT FLOAT") : juce::String (f.bits) + "-BIT");
}

juce::String AudioFileFormat::shortName() const
{
    const auto f = normalized();
    if (f.type == Type::Mp3)
        return "MP3 " + juce::String (f.kbps);
    return typeName (f.type) + " " + (f.bits == 32 ? juce::String ("32F") : juce::String (f.bits));
}

juce::String AudioFileFormat::toString() const
{
    const auto f = normalized();
    return typeName (f.type).toLowerCase() + ":" + juce::String (f.type == Type::Mp3 ? f.kbps : f.bits);
}

AudioFileFormat AudioFileFormat::fromString (const juce::String& text, AudioFileFormat fallback)
{
    const auto name = text.upToFirstOccurrenceOf (":", false, false).trim().toUpperCase();
    const int value = text.fromFirstOccurrenceOf (":", false, false).getIntValue();
    for (auto t : { Type::Wav, Type::Flac, Type::Aiff, Type::Mp3 })
        if (name == typeName (t))
        {
            AudioFileFormat f { t, t == Type::Mp3 ? 16 : (value > 0 ? value : fallback.bits), t == Type::Mp3 && value > 0 ? value : fallback.kbps };
            return f.normalized();
        }
    return fallback.normalized();
}

AudioFileFormat AudioFileFormat::fromString (const juce::String& text)
{
    return fromString (text, AudioFileFormat {});
}

juce::String AudioFileFormat::check (int numChannels, double sampleRate) const
{
    const auto f = normalized();
    return takeString (ld::ld_audio_format_check ((int32_t) f.type, f.bits, f.kbps, numChannels, juce::roundToInt (sampleRate)));
}

std::vector<AudioFileFormat> AudioFileFormat::recordChoices()
{
    return { wav (32), wav (24), aiff (24), flac (24), mp3 (320), mp3 (256), mp3 (192), mp3 (128) };
}

std::vector<AudioFileFormat> AudioFileFormat::exportChoices()
{
    return { wav (32), wav (24), wav (16), aiff (24), aiff (16), flac (24), flac (16), mp3 (320), mp3 (256), mp3 (192), mp3 (128) };
}

bool AudioFileFormat::isAudioFile (const juce::File& f)
{
    return f.hasFileExtension ("wav;wave;flac;aif;aiff;aifc;mp3");
}

AudioFileFormat::Type AudioFileFormat::typeOfFile (const juce::File& f)
{
    if (f.hasFileExtension ("flac"))
        return Type::Flac;
    if (f.hasFileExtension ("aif;aiff;aifc"))
        return Type::Aiff;
    if (f.hasFileExtension ("mp3"))
        return Type::Mp3;
    return Type::Wav;
}

//==============================================================================
AudioFileWriter::~AudioFileWriter()
{
    if (handle != nullptr)
        ld::ld_afw_abort (handle);
}

std::unique_ptr<AudioFileWriter> AudioFileWriter::create (const juce::File& file, int numChannels, double sampleRate, AudioFileFormat format,
                                                          juce::String* error)
{
    const auto f = format.normalized();
    const Utf8 path (file.getFullPathName());
    ld::LdString* message = nullptr;
    auto* h = ld::ld_afw_create (path.data, path.size, numChannels, juce::roundToInt (sampleRate), (int32_t) f.type, f.bits, f.kbps, &message);
    const auto text = takeString (message);
    if (h == nullptr)
    {
        if (error != nullptr)
            *error = text.isNotEmpty() ? text : juce::String ("Can't create " + file.getFullPathName());
        return nullptr;
    }
    std::unique_ptr<AudioFileWriter> w (new AudioFileWriter());
    w->handle = h;
    return w;
}

bool AudioFileWriter::write (const float* const* channels, int numChannels, int numFrames)
{
    return handle != nullptr && ld::ld_afw_write (handle, channels, numChannels, numFrames);
}

int64_t AudioFileWriter::finalize()
{
    if (handle == nullptr)
        return -1;
    auto* h = std::exchange (handle, nullptr);
    return ld::ld_afw_finalize (h);
}

void AudioFileWriter::abort()
{
    if (handle != nullptr)
        ld::ld_afw_abort (std::exchange (handle, nullptr));
}

//==============================================================================
AudioFileReader::~AudioFileReader()
{
    if (handle != nullptr)
        ld::ld_afr_close (handle);
}

std::unique_ptr<AudioFileReader> AudioFileReader::open (const juce::File& file)
{
    const Utf8 path (file.getFullPathName());
    auto* h = ld::ld_afr_open (path.data, path.size);
    if (h == nullptr)
        return nullptr;
    std::unique_ptr<AudioFileReader> r (new AudioFileReader());
    r->handle = h;
    int32_t format = 0, channels = 0, rate = 0, bits = 0, kbps = 0;
    int64_t frames = 0;
    ld::ld_afr_info (h, &format, &channels, &rate, &bits, &frames, &kbps);
    r->type = static_cast<AudioFileFormat::Type> (juce::jlimit (0, 3, (int) format));
    r->numChannels = channels;
    r->sampleRate = rate;
    r->bits = bits;
    r->length = frames;
    r->kbps = kbps;
    return r;
}

bool AudioFileReader::read (int64_t start, float* const* channels, int numChans, int numFrames)
{
    return handle != nullptr && ld::ld_afr_read (handle, start, channels, numChans, numFrames);
}

//==============================================================================
namespace
{
    class EngineReader final : public juce::AudioFormatReader
    {
    public:
        EngineReader (juce::InputStream* stream, std::unique_ptr<AudioFileReader> r) : juce::AudioFormatReader (stream, "MP3"), reader (std::move (r))
        {
            sampleRate = reader->getSampleRate();
            bitsPerSample = 32;
            usesFloatingPointData = true;
            lengthInSamples = reader->getLength();
            numChannels = (unsigned int) reader->getNumChannels();
        }

        bool readSamples (int* const* destChannels, int numDestChannels, int startOffsetInDestBuffer, juce::int64 startSampleInFile,
                          int numSamples) override
        {
            if (numSamples <= 0)
                return true;
            scratch.resize ((size_t) juce::jmax (1, numDestChannels));
            std::vector<float*> ptrs;
            for (int c = 0; c < numDestChannels; ++c)
            {
                if (destChannels[c] != nullptr)
                    ptrs.push_back (reinterpret_cast<float*> (destChannels[c]) + startOffsetInDestBuffer);
                else
                {
                    scratch[(size_t) c].resize ((size_t) numSamples);
                    ptrs.push_back (scratch[(size_t) c].data());
                }
            }
            return reader->read (startSampleInFile, ptrs.data(), (int) ptrs.size(), numSamples);
        }

    private:
        std::unique_ptr<AudioFileReader> reader;
        std::vector<std::vector<float>> scratch;
    };
} // namespace

EngineMp3AudioFormat::EngineMp3AudioFormat() : juce::AudioFormat ("MP3 file (Live DAW)", ".mp3") {}

juce::Array<int> EngineMp3AudioFormat::getPossibleSampleRates()
{
    return { 32000, 44100, 48000 };
}

juce::Array<int> EngineMp3AudioFormat::getPossibleBitDepths()
{
    return { 16 };
}

juce::AudioFormatReader* EngineMp3AudioFormat::createReaderFor (juce::InputStream* stream, bool deleteStreamIfOpeningFails)
{
    std::unique_ptr<AudioFileReader> reader;
    if (auto* fin = dynamic_cast<juce::FileInputStream*> (stream))
        reader = AudioFileReader::open (fin->getFile());
    if (reader == nullptr || reader->getType() != AudioFileFormat::Type::Mp3)
    {
        if (deleteStreamIfOpeningFails)
            delete stream;
        return nullptr;
    }
    return new EngineReader (stream, std::move (reader));
}

} // namespace livedaw
