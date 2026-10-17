#include "engine/Recorder.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace livedaw
{

// Placeholder until the engine's recorder lands: a wall-clock transport, no
// audio. A recording produces silent takes of the recorded length, so the UI's
// whole flow can be exercised.
struct Recorder::Impl
{
    double sampleRate = 48000.0;
    State state = State::Stopped;
    int64_t position = 0;          // at rollStartMs while rolling
    double rollStartMs = 0.0;
    int64_t loopStart = 0, loopEnd = 0;
    bool loop = false;
    std::vector<RecordTrack> recording;
    int64_t recordStart = 0;
    std::vector<Take> finished;

    int64_t now() const
    {
        if (state != State::Playing && state != State::Recording)
            return position;
        auto p = position + (int64_t) ((juce::Time::getMillisecondCounterHiRes() - rollStartMs) * sampleRate / 1000.0);
        if (loop && state == State::Playing && loopEnd > loopStart && p >= loopEnd)
            p = loopStart + (p - loopStart) % (loopEnd - loopStart);
        return p;
    }

    void roll (State s)
    {
        position = now();
        rollStartMs = juce::Time::getMillisecondCounterHiRes();
        state = s;
    }

    void finishRecording()
    {
        const auto length = juce::jmax<int64_t> (0, now() - recordStart);
        for (auto& r : recording)
        {
            juce::WavAudioFormat wav;
            r.file.getParentDirectory().createDirectory();
            r.file.deleteFile();
            if (auto out = std::unique_ptr<juce::OutputStream> (r.file.createOutputStream()))
            {
                const auto options = juce::AudioFormatWriterOptions().withSampleRate (sampleRate).withNumChannels (r.numChannels).withBitsPerSample (32).withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
                if (auto writer = wav.createWriterFor (out, options))
                {
                    juce::AudioBuffer<float> silence (r.numChannels, 4096);
                    silence.clear();
                    for (int64_t done = 0; done < length; done += 4096)
                        writer->writeFromAudioSampleBuffer (silence, 0, (int) juce::jmin<int64_t> (4096, length - done));
                }
            }
            finished.push_back ({ r.track, r.file, recordStart, length, r.numChannels });
        }
        recording.clear();
    }
};

Recorder::Recorder() : impl (std::make_unique<Impl>()) {}
Recorder::~Recorder() = default;

Recorder::State Recorder::getState() const { return impl->state; }
int64_t Recorder::getPosition() const { return impl->now(); }
double Recorder::getSampleRate() const { return impl->sampleRate; }

void Recorder::play()
{
    if (impl->state == State::Stopped)
        impl->roll (State::Playing);
}

void Recorder::stop()
{
    if (impl->state == State::Recording)
        impl->finishRecording();
    impl->position = impl->now();
    impl->state = State::Stopped;
}

void Recorder::locate (int64_t position)
{
    impl->position = juce::jmax<int64_t> (0, position);
    impl->rollStartMs = juce::Time::getMillisecondCounterHiRes();
}

void Recorder::setLoop (int64_t start, int64_t end, bool enabled)
{
    impl->position = impl->now();
    impl->rollStartMs = juce::Time::getMillisecondCounterHiRes();
    impl->loopStart = start;
    impl->loopEnd = end;
    impl->loop = enabled;
}

bool Recorder::isLoopEnabled() const { return impl->loop; }

void Recorder::setPlaylist (const Playlist&) {}

juce::Result Recorder::record (const std::vector<RecordTrack>& tracks)
{
    if (tracks.empty())
        return juce::Result::fail ("No tracks are armed");
    if (impl->state == State::Recording)
        return juce::Result::fail ("Already recording");
    impl->recording = tracks;
    impl->roll (State::Recording);
    impl->recordStart = impl->position;
    return juce::Result::ok();
}

std::vector<Recorder::Take> Recorder::takeFinishedTakes() { return std::exchange (impl->finished, {}); }

float Recorder::getRecordLevel (int, int) { return -100.0f; }

int Recorder::getRecordingPeaks (int track, int64_t firstPeak, int maxPeaks, float* minMax) const
{
    if (impl->state != State::Recording)
        return 0;
    for (const auto& r : impl->recording)
        if (r.track == track)
        {
            const auto available = getRecordedLength() / kSamplesPerPeak - firstPeak;
            const int n = (int) juce::jlimit<int64_t> (0, maxPeaks, available);
            std::fill (minMax, minMax + n * 2 * r.numChannels, 0.0f);
            return n;
        }
    return 0;
}

int64_t Recorder::getRecordedLength() const
{
    return impl->state == State::Recording ? impl->now() - impl->recordStart : 0;
}

int Recorder::getDropouts() const { return 0; }
float Recorder::getDiskLoad() const { return 0.0f; }

} // namespace livedaw
