#include "TestHelpers.h"
#include "engine/VirtualAudioDevice.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <functional>

using namespace livedaw;

namespace
{
struct TempDir
{
    explicit TempDir (const juce::String& name)
        : dir (juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("livedaw-recorder-tests-" + name + "-" + juce::String (juce::Time::currentTimeMillis())))
    {
        dir.deleteRecursively();
        dir.createDirectory();
    }

    ~TempDir() { dir.deleteRecursively(); }

    juce::File dir;
};

/** Drives a MixerEngine offline, block by block. The test runs faster than
    real time, so before each block the disk threads are given time to catch up. */
struct Driver
{
    static constexpr int kBlock = 128;

    Driver (MixerEngine& e, int numIns, int numOuts) : engine (e)
    {
        engine.prepareOffline (48000.0, kBlock, numIns, numOuts);
        ins.assign ((size_t) numIns, std::vector<float> (kBlock, 0.0f));
        outs.assign ((size_t) numOuts, std::vector<float> (kBlock, 0.0f));
    }

    void block (const std::function<float (int, int64_t)>& input)
    {
        engine.getRecorder().waitForDisk();
        std::vector<const float*> in;
        std::vector<float*> out;
        for (size_t ch = 0; ch < ins.size(); ++ch)
        {
            for (int i = 0; i < kBlock; ++i)
                ins[ch][(size_t) i] = input ((int) ch, clock + i);
            in.push_back (ins[ch].data());
        }
        for (auto& o : outs)
            out.push_back (o.data());
        engine.processBlock (in.data(), (int) in.size(), out.data(), (int) out.size(), kBlock);
        clock += kBlock;
    }

    MixerEngine& engine;
    std::vector<std::vector<float>> ins, outs;
    int64_t clock = 0; // samples fed so far
};

std::vector<Recorder::Take> waitForTakes (Recorder& recorder, size_t count)
{
    std::vector<Recorder::Take> takes;
    const auto deadline = juce::Time::getMillisecondCounter() + 10000;
    while (takes.size() < count && juce::Time::getMillisecondCounter() < deadline)
    {
        for (auto& t : recorder.takeFinishedTakes())
            takes.push_back (t);
        juce::Thread::sleep (2);
    }
    return takes;
}

std::unique_ptr<juce::AudioFormatReader> openReader (const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    return std::unique_ptr<juce::AudioFormatReader> (formats.createReaderFor (file));
}

juce::AudioBuffer<float> readAll (juce::AudioFormatReader& reader)
{
    juce::AudioBuffer<float> buffer ((int) reader.numChannels, (int) reader.lengthInSamples);
    reader.read (&buffer, 0, (int) reader.lengthInSamples, 0, true, true);
    return buffer;
}

Recorder::RecordTrack inputTrack (int track, int port, int numChannels, const juce::File& file)
{
    Recorder::RecordTrack t;
    t.track = track;
    t.source.kind = Recorder::Source::Kind::HardwareInput;
    t.source.port = port;
    t.numChannels = numChannels;
    t.file = file;
    return t;
}
} // namespace

class RecorderTests final : public juce::UnitTest
{
public:
    RecorderTests() : juce::UnitTest ("Recorder", "LiveDAW") {}

    void runTest() override
    {
        beginTest ("The C ABI is version 19 and both sides agree on the layout");
        {
            expectEquals (ld::kAbiVersion, 20);
            ld::LayoutInfo info {};
            ld::ld_layout_info (&info);
            expectEquals ((int) info.abiVersion, 20);
            const char* mismatch = ld::verifyLayout();
            expect (mismatch == nullptr, mismatch != nullptr ? juce::String (mismatch) : juce::String());
        }

        beginTest ("A recorded hardware input reads back exactly with JUCE");
        {
            TempDir tmp ("hw");
            MixerEngine engine (0);
            Driver d (engine, 4, 2);
            auto& rec = engine.getRecorder();
            const auto file = tmp.dir.getChildFile ("take.wav");

            rec.locate (4800);
            expectEquals (rec.getPosition(), (int64_t) 4800);
            expect (rec.record ({ inputTrack (7, 2, 1, file) }).wasOk());
            expect (rec.getState() == Recorder::State::Starting);
            expect (file.existsAsFile(), "record() creates the file");

            // Input 3 carries the sample clock: every recorded sample is distinct.
            const auto clock = [] (int ch, int64_t t) { return ch == 2 ? (float) t : 0.5f; };
            int64_t firstClock = -1, firstPos = -1;
            for (int i = 0; i < 2000 && (firstClock < 0 || d.clock - firstClock < 24000); ++i)
            {
                const auto was = rec.getState();
                const auto before = d.clock;
                d.block (clock);
                if (was != Recorder::State::Recording && rec.getState() == Recorder::State::Recording)
                {
                    firstClock = before;
                    firstPos = rec.getPosition() - Driver::kBlock;
                }
            }
            expectEquals (firstPos, (int64_t) 4800);
            expect (rec.isRolling());
            expect (rec.getRecordedLength() >= 24000);
            expect (rec.getRecordLevel (7, 0) > 0.0f, "the clock is louder than 0 dBFS by now");

            rec.waitForDisk();
            std::vector<float> peaks (20);
            expectEquals (rec.getRecordingPeaks (7, 1, 10, peaks.data()), 10);
            expectEquals (peaks[0], (float) (firstClock + Recorder::kSamplesPerPeak));
            expectEquals (peaks[1], (float) (firstClock + 2 * Recorder::kSamplesPerPeak - 1));

            rec.stop();
            d.block (clock); // the chunk that sees the stop is still recorded
            expect (rec.getState() == Recorder::State::Stopped);
            const auto takes = waitForTakes (rec, 1);
            expectEquals ((int) takes.size(), 1);
            if (takes.size() == 1)
            {
                const auto& take = takes[0];
                expectEquals (take.track, 7);
                expect (take.file == file);
                expectEquals (take.start, (int64_t) 4800);
                expectEquals (take.length, d.clock - firstClock);
                expectEquals (take.numChannels, 1);

                auto reader = openReader (take.file);
                expect (reader != nullptr);
                if (reader != nullptr)
                {
                    expectEquals ((int) reader->numChannels, 1);
                    expectEquals (reader->sampleRate, 48000.0);
                    expect (reader->usesFloatingPointData);
                    expectEquals ((int) reader->bitsPerSample, 32);
                    expectEquals ((int64_t) reader->lengthInSamples, take.length);
                    const auto samples = readAll (*reader);
                    int mismatches = 0;
                    for (int k = 0; k < samples.getNumSamples(); ++k)
                        if (samples.getSample (0, k) != (float) (firstClock + k))
                            ++mismatches;
                    expectEquals (mismatches, 0);
                }
            }
            expectEquals (rec.getDropouts(), 0);
            expectEquals (rec.getRecordedLength(), (int64_t) 0);

            expect (rec.record ({}).failed());
            expect (rec.record ({ inputTrack (1, 99, 1, tmp.dir.getChildFile ("x.wav")) }).failed(), "no input 100");
            auto strip = inputTrack (1, 0, 2, tmp.dir.getChildFile ("y.wav"));
            strip.source.kind = Recorder::Source::Kind::StripPreFader;
            strip.source.stripKind = StripKind::Channel;
            strip.source.stripNumber = 3;
            expect (rec.record ({ strip }).failed(), "there is no CH 3");
            expect (! tmp.dir.getChildFile ("y.wav").exists());
        }

        beginTest ("Playback through an ALT input records at its timeline position");
        {
            TempDir tmp ("alt");
            constexpr int64_t T = 12000 + 51;
            constexpr int64_t clipStart = 2000, fileOffset = 500;
            const auto clipFile = tmp.dir.getChildFile ("impulse.wav");
            {
                // A 24-bit file written by JUCE: file sample k plays at clipStart + k - fileOffset.
                juce::AudioBuffer<float> impulse (1, 20000);
                impulse.clear();
                impulse.setSample (0, (int) (T - clipStart + fileOffset), 1.0f);
                std::unique_ptr<juce::OutputStream> out (clipFile.createOutputStream());
                const auto options = juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (24);
                auto writer = juce::WavAudioFormat().createWriterFor (out, options);
                expect (writer != nullptr);
                if (writer != nullptr)
                    writer->writeFromAudioSampleBuffer (impulse, 0, impulse.getNumSamples());
            }

            MixerEngine engine (0);
            test::setChannels (engine, 2);
            engine.setInputSelectGlobal (false); // per channel (a new console's is GLOBAL)
            Driver d (engine, 2, 2);
            auto& rec = engine.getRecorder();
            auto& ch2 = *engine.getChannels()[1];
            ch2.setAltInput (Recorder::playbackSource (3));
            ch2.setAltSelected (true);
            expect (ch2.hasAltInput() && ch2.isUsingAltInput() && engine.usesAltInput (ch2));
            expectEquals (ch2.getActiveInputLeft(), Recorder::playbackSource (3));
            expectEquals (Recorder::playbackChannelOf (ch2.getAltInputLeft()), 3);
            expectEquals (Recorder::playbackSourceName (3), juce::String ("PLAY 4"));

            Recorder::Clip clip;
            clip.file = clipFile;
            clip.fileOffset = fileOffset;
            clip.start = clipStart;
            clip.length = 19000;
            Recorder::PlaybackTrack track;
            track.channel = 3;
            track.clips = { clip };
            rec.setPlaylist ({ track });

            auto post = inputTrack (4, 0, 1, tmp.dir.getChildFile ("ch2.wav"));
            post.source.kind = Recorder::Source::Kind::StripPostFader;
            post.source.stripKind = StripKind::Channel;
            post.source.stripNumber = 2;
            expect (rec.record ({ post }).wasOk());
            while (d.clock < 30000)
                d.block ([] (int, int64_t) { return 0.0f; });
            rec.stop();
            d.block ([] (int, int64_t) { return 0.0f; });
            const auto takes = waitForTakes (rec, 1);
            expectEquals ((int) takes.size(), 1);
            if (takes.size() == 1)
            {
                expectEquals (takes[0].start, (int64_t) 0);
                auto reader = openReader (takes[0].file);
                expect (reader != nullptr);
                if (reader != nullptr)
                {
                    const auto samples = readAll (*reader);
                    int peakAt = -1;
                    float peak = 0.0f;
                    for (int k = 0; k < samples.getNumSamples(); ++k)
                        if (std::abs (samples.getSample (0, k)) > std::abs (peak))
                        {
                            peak = samples.getSample (0, k);
                            peakAt = k;
                        }
                    expectEquals (takes[0].start + peakAt, T, "the impulse lands at T");
                    expectWithinAbsoluteError (peak, test::kCentre, 1.0e-3f);
                }
            }

            // INPUT SELECT global: the global switch decides.
            engine.setInputSelectGlobal (true);
            expect (! ch2.isUsingAltInput());
            engine.setGlobalAlt (true);
            expect (ch2.isUsingAltInput());
            expect (! engine.getChannels()[0]->isUsingAltInput(), "CH 1 has no ALT source");
            engine.setInputSelectGlobal (false);
            ch2.setAltInput (-1);
            expect (! ch2.isUsingAltInput());
        }

        beginTest ("The transport loops, locates and changes playlists while rolling");
        {
            MixerEngine engine (0);
            Driver d (engine, 0, 2);
            auto& rec = engine.getRecorder();
            rec.setLoop (1000, 5000, true);
            expect (rec.isLoopEnabled());
            expectEquals (rec.getLoopStart(), (int64_t) 1000);
            expectEquals (rec.getLoopEnd(), (int64_t) 5000);
            rec.locate (4000);
            rec.play();
            expect (rec.getState() == Recorder::State::Starting);
            for (int i = 0; i < 100; ++i)
                d.block ([] (int, int64_t) { return 0.0f; });
            expect (rec.getState() == Recorder::State::Playing);
            const auto pos = rec.getPosition();
            expect (pos >= 1000 && pos < 5000, "inside the loop: " + juce::String (pos));
            rec.setPlaylist ({});
            for (int i = 0; i < 20; ++i)
                d.block ([] (int, int64_t) { return 0.0f; });
            expect (rec.getState() == Recorder::State::Playing, "no refill for a playlist change");
            rec.locate (100000);
            d.block ([] (int, int64_t) { return 0.0f; });
            d.block ([] (int, int64_t) { return 0.0f; });
            expect (rec.isRolling());
            expect (rec.getPosition() > 100000);
            rec.stop();
            d.block ([] (int, int64_t) { return 0.0f; });
            expect (rec.getState() == Recorder::State::Stopped);
            expectEquals (rec.getDropouts(), 0);
            expectEquals (rec.getDiskLoad(), 0.0f);
        }

        beginTest ("The virtual device runs the engine in real time");
        {
            TempDir tmp ("virtual");
            juce::AudioDeviceManager manager;
            manager.addAudioDeviceType (std::make_unique<VirtualAudioIODeviceType>());
            manager.setCurrentAudioDeviceType (VirtualAudioIODeviceType::kTypeName, false);
            auto setup = manager.getAudioDeviceSetup();
            setup.outputDeviceName = setup.inputDeviceName = VirtualAudioIODeviceType::kDeviceName;
            setup.sampleRate = 48000.0;
            setup.bufferSize = 256;
            setup.useDefaultInputChannels = setup.useDefaultOutputChannels = false;
            setup.inputChannels.setRange (0, 8, true);
            setup.outputChannels.setRange (0, 8, true);
            expectEquals (manager.setAudioDeviceSetup (setup, true), juce::String());

            auto* device = manager.getCurrentAudioDevice();
            expect (device != nullptr);
            if (device != nullptr)
            {
                expectEquals (device->getTypeName(), juce::String (VirtualAudioIODeviceType::kTypeName));
                expectEquals (device->getInputChannelNames().size(), VirtualAudioIODeviceType::kNumChannels);
                expectEquals (device->getActiveInputChannels().countNumberOfSetBits(), 8);

                MixerEngine engine (0);
                test::setChannels (engine, 2);
                manager.addAudioCallback (&engine);
                auto& rec = engine.getRecorder();
                expect (rec.record ({ inputTrack (0, 0, 2, tmp.dir.getChildFile ("virtual.wav")) }).wasOk());
                const auto deadline = juce::Time::getMillisecondCounter() + 5000;
                while (rec.getState() != Recorder::State::Recording && juce::Time::getMillisecondCounter() < deadline)
                    juce::Thread::sleep (1);
                expect (rec.getState() == Recorder::State::Recording, "the device rolls the transport");
                const auto startLength = rec.getRecordedLength();
                const auto startMs = juce::Time::getMillisecondCounterHiRes();
                juce::Thread::sleep (400);
                const auto seconds = (juce::Time::getMillisecondCounterHiRes() - startMs) / 1000.0;
                const auto rate = (double) (rec.getRecordedLength() - startLength) / seconds;
                expect (rate > 48000.0 * 0.6 && rate < 48000.0 * 1.5, "real time: " + juce::String (rate, 0) + " samples/s");
                expect (engine.isDeviceRunning());
                rec.stop();
                const auto takes = waitForTakes (rec, 1);
                expectEquals ((int) takes.size(), 1);
                if (takes.size() == 1)
                {
                    expectEquals (takes[0].numChannels, 2);
                    auto reader = openReader (takes[0].file);
                    expect (reader != nullptr && reader->lengthInSamples == takes[0].length && takes[0].length > 0);
                    if (reader != nullptr)
                    {
                        const auto samples = readAll (*reader);
                        expectEquals (samples.getMagnitude (0, samples.getNumSamples()), 0.0f, "silent inputs");
                    }
                }
                manager.removeAudioCallback (&engine);
            }
            manager.closeAudioDevice();
        }
    }
};

static RecorderTests recorderTests;
