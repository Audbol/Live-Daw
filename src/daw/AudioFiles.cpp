#include "daw/AudioFiles.h"

namespace livedaw::daw
{

juce::String importWildcard (juce::AudioFormatManager& formats)
{
    juce::StringArray patterns;
    for (auto* f : formats)
        for (auto ext : f->getFileExtensions())
            patterns.addIfNotAlreadyThere ("*" + ext);
    return patterns.joinIntoString (";");
}

bool canImport (juce::AudioFormatManager& formats, const juce::File& f)
{
    return formats.findFormatForFileExtension (f.getFileExtension()) != nullptr;
}

juce::File uniqueAudioFile (const juce::File& folder, const juce::String& base)
{
    auto clean = juce::File::createLegalFileName (base.trim()).replaceCharacters (" ", "_");
    if (clean.isEmpty())
        clean = "audio";
    auto f = folder.getChildFile (clean + ".wav");
    for (int i = 2; f.exists(); ++i)
        f = folder.getChildFile (clean + "-" + juce::String (i) + ".wav");
    return f;
}

std::unique_ptr<juce::AudioFormatWriter> createWavWriter (const juce::File& file, double sampleRate, int numChannels)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> out (file.createOutputStream());
    if (out == nullptr)
        return nullptr;
    juce::WavAudioFormat wav;
    const auto options = juce::AudioFormatWriterOptions()
                             .withSampleRate (sampleRate)
                             .withNumChannels (numChannels)
                             .withBitsPerSample (32)
                             .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    return wav.createWriterFor (out, options);
}

//==============================================================================
ImportResult importAudioFile (juce::AudioFormatManager& formats, const juce::File& source, const juce::File& folder, double sampleRate,
                              const std::atomic<bool>& cancel, std::function<void (float)> progress)
{
    ImportResult r;
    r.name = source.getFileNameWithoutExtension();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (source));
    if (reader == nullptr)
    {
        r.result = juce::Result::fail ("Can't read " + source.getFileName() + " (unknown or damaged format)");
        return r;
    }
    if (reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
    {
        r.result = juce::Result::fail (source.getFileName() + " holds no audio");
        return r;
    }

    const int channels = juce::jlimit (1, 2, (int) reader->numChannels);
    r.numChannels = channels;
    r.file = uniqueAudioFile (folder, r.name);
    auto writer = createWavWriter (r.file, sampleRate, channels);
    if (writer == nullptr)
    {
        r.result = juce::Result::fail ("Can't write " + r.file.getFullPathName());
        return r;
    }

    const double ratio = reader->sampleRate / sampleRate; // input samples per output sample
    const int64 inLength = reader->lengthInSamples;
    const int64 outLength = (int64) std::ceil ((double) inLength / ratio);
    constexpr int kBlock = 8192;
    juce::AudioBuffer<float> in (channels, kBlock), out (channels, kBlock);
    auto fail = [&] (const juce::String& message)
    {
        writer.reset();
        r.file.deleteFile();
        r.result = juce::Result::fail (message);
        return r;
    };

    if (std::abs (ratio - 1.0) < 1.0e-9)
    {
        for (int64 pos = 0; pos < inLength; pos += kBlock)
        {
            if (cancel.load())
                return fail ("Cancelled");
            const int n = (int) juce::jmin<int64> (kBlock, inLength - pos);
            if (! reader->read (&in, 0, n, pos, true, channels > 1))
                return fail ("Read error in " + source.getFileName());
            if (! writer->writeFromAudioSampleBuffer (in, 0, n))
                return fail ("Write error (disk full?)");
            if (progress)
                progress ((float) pos / (float) inLength);
        }
        r.length = inLength;
    }
    else
    {
        // Stream through windowed-sinc interpolators, one per channel. Input is
        // kept in a FIFO; after the end of the file zeros flush the filter.
        std::vector<juce::WindowedSincInterpolator> interpolators ((size_t) channels);
        std::vector<std::vector<float>> fifo ((size_t) channels);
        size_t fifoStart = 0;
        int64 readPos = 0, written = 0;
        const auto latency = (double) juce::WindowedSincInterpolator::getBaseLatency();
        int64 toSkip = (int64) std::round (latency / ratio);
        while (written < outLength)
        {
            if (cancel.load())
                return fail ("Cancelled");
            const int numOut = (int) juce::jmin<int64> (kBlock, outLength - written + toSkip);
            const auto needed = (size_t) std::ceil ((double) numOut * ratio) + 64;
            while (fifo[0].size() - fifoStart < needed)
            {
                const int n = (int) juce::jmin<int64> (kBlock, inLength - readPos);
                if (n > 0)
                {
                    if (! reader->read (&in, 0, n, readPos, true, channels > 1))
                        return fail ("Read error in " + source.getFileName());
                    readPos += n;
                    for (int ch = 0; ch < channels; ++ch)
                        fifo[(size_t) ch].insert (fifo[(size_t) ch].end(), in.getReadPointer (ch), in.getReadPointer (ch) + n);
                }
                else
                {
                    for (auto& f : fifo)
                        f.insert (f.end(), (size_t) kBlock, 0.0f);
                }
            }
            int used = 0;
            for (int ch = 0; ch < channels; ++ch)
                used = interpolators[(size_t) ch].process (ratio, fifo[(size_t) ch].data() + fifoStart, out.getWritePointer (ch), numOut);
            fifoStart += (size_t) used;
            if (fifoStart > (size_t) kBlock * 4)
            {
                for (auto& f : fifo)
                    f.erase (f.begin(), f.begin() + (std::ptrdiff_t) fifoStart);
                fifoStart = 0;
            }
            // Drop the interpolator's latency from the start.
            const int skip = (int) juce::jmin<int64> (toSkip, numOut);
            toSkip -= skip;
            const int keep = (int) juce::jmin<int64> (numOut - skip, outLength - written);
            if (keep > 0)
            {
                juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), channels, skip, keep);
                if (! writer->writeFromAudioSampleBuffer (view, 0, keep))
                    return fail ("Write error (disk full?)");
                written += keep;
            }
            if (progress)
                progress ((float) written / (float) outLength);
        }
        r.length = outLength;
    }

    if (! writer->flush())
        return fail ("Write error (disk full?)");
    writer.reset();
    if (progress)
        progress (1.0f);
    return r;
}

//==============================================================================
juce::Result renderTrack (juce::AudioFormatManager& formats, const std::vector<RenderClip>& clips, float trackGainDb, int numChannels,
                          double sampleRate, int64 length, const juce::File& dest, const std::atomic<bool>& cancel,
                          std::function<void (float)> progress)
{
    numChannels = juce::jlimit (1, 2, numChannels);
    auto writer = createWavWriter (dest, sampleRate, numChannels);
    if (writer == nullptr)
        return juce::Result::fail ("Can't write " + dest.getFullPathName());

    std::vector<std::unique_ptr<juce::AudioFormatReader>> readers;
    for (auto& c : clips)
        readers.emplace_back (formats.createReaderFor (c.file));

    const float trackGain = trackGainDb <= -100.0f ? 0.0f : std::pow (10.0f, trackGainDb / 20.0f);
    constexpr int kBlock = 8192;
    juce::AudioBuffer<float> mix (numChannels, kBlock), part (2, kBlock);
    for (int64 pos = 0; pos < length; pos += kBlock)
    {
        if (cancel.load())
        {
            writer.reset();
            dest.deleteFile();
            return juce::Result::fail ("Cancelled");
        }
        const int n = (int) juce::jmin<int64> (kBlock, length - pos);
        mix.clear();
        for (size_t i = 0; i < clips.size(); ++i)
        {
            const auto& c = clips[i].clip;
            auto* reader = readers[i].get();
            if (reader == nullptr || c.mute)
                continue;
            const int64 from = juce::jmax (pos, c.start), to = juce::jmin (pos + n, c.end());
            if (to <= from)
                continue;
            const int count = (int) (to - from);
            const int offset = (int) (from - pos);
            part.clear();
            reader->read (&part, 0, count, c.fileOffset + (from - c.start), true, reader->numChannels > 1);
            if (reader->numChannels < 2)
                part.copyFrom (1, 0, part, 0, 0, count);
            const float clipGain = (c.gainDb <= -100.0f ? 0.0f : std::pow (10.0f, c.gainDb / 20.0f)) * trackGain;
            for (int s = 0; s < count; ++s)
            {
                const int64 t = from + s - c.start; // position inside the clip
                float g = clipGain;
                if (t < c.fadeIn)
                    g *= clipmath::fadeGain (c.fadeInShape, (float) ((double) t / (double) c.fadeIn));
                if (t >= c.length - c.fadeOut && c.fadeOut > 0)
                    g *= clipmath::fadeGain (c.fadeOutShape, (float) ((double) (c.length - t) / (double) c.fadeOut));
                for (int ch = 0; ch < numChannels; ++ch)
                    mix.addSample (ch, offset + s, part.getSample (numChannels == 1 ? 0 : ch, s) * g);
            }
        }
        if (! writer->writeFromAudioSampleBuffer (mix, 0, n))
        {
            writer.reset();
            dest.deleteFile();
            return juce::Result::fail ("Write error (disk full?)");
        }
        if (progress)
            progress ((float) ((double) pos / (double) juce::jmax<int64> (1, length)));
    }
    writer->flush();
    return juce::Result::ok();
}

} // namespace livedaw::daw
