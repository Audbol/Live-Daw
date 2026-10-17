#include "engine/Recorder.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace livedaw
{

static_assert (Recorder::kNumPlaybackChannels == ld::kNumPlaybackChannels);
static_assert (Recorder::kPlaybackSourceBase == ld::kPlaybackSourceBase);
static_assert (Recorder::kMaxRecordTracks == ld::kMaxRecordTracks);
static_assert (Recorder::kSamplesPerPeak == ld::kSamplesPerPeak);

static_assert ((int) Recorder::State::Stopped == 0 && (int) Recorder::State::Starting == 1
               && (int) Recorder::State::Playing == 2 && (int) Recorder::State::Recording == 3);
static_assert ((int) Recorder::FadeShape::Linear == 0 && (int) Recorder::FadeShape::EqualPower == 1
               && (int) Recorder::FadeShape::SCurve == 2);
static_assert ((int) Recorder::Source::Kind::HardwareInput == 0 && (int) Recorder::Source::Kind::StripPreFader == 1
               && (int) Recorder::Source::Kind::StripPostFader == 2);

Recorder::Recorder (ld::Engine* e) : engine (e) {}
Recorder::~Recorder() = default;

Recorder::State Recorder::getState() const { return static_cast<State> (ld::ld_rec_state (engine)); }
int64_t Recorder::getPosition() const { return ld::ld_rec_position (engine); }
double Recorder::getSampleRate() const { return ld::ld_engine_sample_rate (engine); }

void Recorder::play() { ld::ld_rec_play (engine); }
void Recorder::stop() { ld::ld_rec_stop (engine); }
void Recorder::locate (int64_t position) { ld::ld_rec_locate (engine, juce::jmax<int64_t> (0, position)); }

void Recorder::setLoop (int64_t start, int64_t end, bool enabled) { ld::ld_rec_set_loop (engine, start, end, enabled); }
bool Recorder::isLoopEnabled() const { return ld::ld_rec_loop (engine, nullptr, nullptr); }

int64_t Recorder::getLoopStart() const
{
    int64_t start = 0;
    ld::ld_rec_loop (engine, &start, nullptr);
    return start;
}

int64_t Recorder::getLoopEnd() const
{
    int64_t end = 0;
    ld::ld_rec_loop (engine, nullptr, &end);
    return end;
}

void Recorder::setPlaylist (const Playlist& playlist)
{
    // The paths must outlive the call: keep their UTF-8 here.
    std::vector<std::string> paths;
    std::vector<ld::RecClipC> clips;
    std::vector<ld::RecPlaybackTrackC> tracks;
    size_t numClips = 0;
    for (const auto& t : playlist)
        numClips += t.clips.size();
    paths.reserve (numClips);
    clips.reserve (numClips);
    tracks.reserve (playlist.size());

    for (const auto& t : playlist)
    {
        if (t.channel < 0 || t.channel >= kNumPlaybackChannels)
            continue; // plays nowhere
        tracks.push_back ({ t.channel, juce::jlimit (1, 2, t.numChannels), t.gain, t.mute, (int32_t) clips.size(),
                            (int32_t) t.clips.size() });
        for (const auto& c : t.clips)
        {
            const auto& path = paths.emplace_back (c.file.getFullPathName().toStdString());
            clips.push_back ({ path.data(), path.size(), c.fileOffset, c.start, c.length, c.fadeIn, c.fadeOut, c.gain,
                               (int32_t) c.fadeInShape, (int32_t) c.fadeOutShape });
        }
    }

    ld::ld_rec_set_playlist (engine, tracks.data(), (int32_t) tracks.size(), clips.data(), (int32_t) clips.size());
}

juce::Result Recorder::record (const std::vector<RecordTrack>& tracks)
{
    if (tracks.empty())
        return juce::Result::fail ("No tracks are armed");

    std::vector<std::string> paths;
    std::vector<ld::RecRecordTrackC> specs;
    paths.reserve (tracks.size());
    specs.reserve (tracks.size());
    for (const auto& t : tracks)
    {
        const auto& path = paths.emplace_back (t.file.getFullPathName().toStdString());
        specs.push_back ({ t.track, (int32_t) t.source.kind, t.source.port, (int32_t) t.source.stripKind,
                           t.source.stripNumber, t.numChannels, path.data(), path.size() });
    }

    ld::LdString* error = nullptr;
    if (ld::ld_rec_record (engine, specs.data(), (int32_t) specs.size(), &error))
        return juce::Result::ok();
    return juce::Result::fail (error != nullptr ? takeString (error) : juce::String ("Recording failed"));
}

std::vector<Recorder::Take> Recorder::takeFinishedTakes()
{
    std::vector<Take> takes;
    ld::RecTakeC t {};
    while (ld::ld_rec_pop_take (engine, &t))
        takes.push_back ({ t.track, juce::File (takeString (t.path)), t.start, t.length, t.numChannels });
    return takes;
}

float Recorder::getRecordLevel (int track, int channel)
{
    return juce::Decibels::gainToDecibels (ld::ld_rec_record_level (engine, track, channel), -100.0f);
}

int Recorder::getRecordingPeaks (int track, int64_t firstPeak, int maxPeaks, float* minMax) const
{
    if (minMax == nullptr || maxPeaks <= 0)
        return 0;
    return ld::ld_rec_recording_peaks (engine, track, firstPeak, maxPeaks, minMax);
}

int64_t Recorder::getRecordedLength() const { return ld::ld_rec_recorded_length (engine); }

int Recorder::getDropouts() const { return ld::ld_rec_dropouts (engine); }
float Recorder::getDiskLoad() const { return ld::ld_rec_disk_load (engine); }

bool Recorder::waitForDisk (int timeoutMs) { return ld::ld_rec_wait_for_disk (engine, timeoutMs); }

} // namespace livedaw
