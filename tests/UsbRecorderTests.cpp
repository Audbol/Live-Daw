#include "TestHelpers.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <functional>

using namespace livedaw;

namespace
{
struct UsbTempDir
{
    explicit UsbTempDir (const juce::String& name)
        : dir (juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("livedaw-usb-tests-" + name + "-" + juce::String (juce::Time::currentTimeMillis())))
    {
        dir.deleteRecursively();
        dir.createDirectory();
    }

    ~UsbTempDir() { dir.deleteRecursively(); }

    juce::File dir;
};

/** Runs a MixerEngine offline; the USB deck's disk thread catches up before each block. */
struct UsbDriver
{
    static constexpr int kBlock = 128;

    UsbDriver (MixerEngine& e, int numIns, int numOuts) : engine (e)
    {
        engine.prepareOffline (48000.0, kBlock, numIns, numOuts);
        ins.assign ((size_t) numIns, std::vector<float> (kBlock, 0.0f));
        outs.assign ((size_t) numOuts, std::vector<float> (kBlock, 0.0f));
    }

    void block (const std::function<float (int, int64_t)>& input)
    {
        engine.getUsbRecorder().waitForDisk();
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

    void run (int blocks)
    {
        for (int b = 0; b < blocks; ++b)
            block ([] (int, int64_t) { return 0.0f; });
    }

    MixerEngine& engine;
    std::vector<std::vector<float>> ins, outs;
    int64_t clock = 0;
};

/** Input 1: a ramp on the 16-bit grid. */
float ramp16 (int64_t t) { return (float) ((t % 20000) - 10000) / 32768.0f; }
} // namespace

class UsbRecorderTests final : public juce::UnitTest
{
public:
    UsbRecorderTests() : juce::UnitTest ("USB recorder / player", "LiveDAW") {}

    void runTest() override
    {
        beginTest ("RECORDER taps encode like the engine's; MAIN 1 L/R by default");
        {
            MixerEngine engine (0);
            auto& usb = engine.getUsbRecorder();
            auto tap = usb.getTap (0);
            expect (tap.kind == UsbRecorder::Tap::Kind::Strip && tap.stripKind == StripKind::Main && tap.stripNumber == 1 && tap.channel == 0);
            expectEquals (usb.getTap (1).channel, 1);
            expect (usb.getTap (3).kind == UsbRecorder::Tap::Kind::Off);
            UsbRecorder::Tap bus;
            bus.kind = UsbRecorder::Tap::Kind::Strip;
            bus.stripKind = StripKind::Bus;
            bus.stripNumber = 12;
            bus.channel = 1;
            usb.setTap (2, bus);
            expect (usb.getTap (2) == bus);
            UsbRecorder::Tap source;
            source.kind = UsbRecorder::Tap::Kind::Source;
            source.source = UsbRecorder::playerSource (3);
            usb.setTap (3, source);
            expectEquals (usb.getTap (3).source, UsbRecorder::kPlayerSourceBase + 3);
            expectEquals (UsbRecorder::playerChannelOf (0x20002), 2);
            expect (! UsbRecorder::isPlayerSource (Recorder::playbackSource (0)));
            expectEquals (MixerEngine::sourceIndex (UsbRecorder::playerSource (0)), ld::kNumHwSources + ld::kNumPlaybackChannels);
            expect (engine.getSource (MixerEngine::sourceIndex (UsbRecorder::playerSource (3))) != nullptr, "USB PLAYER sources have settings");
        }

        beginTest ("The browser lists folders, WAV files and playlists; FOLDER, RENAME, DELETE");
        {
            UsbTempDir tmp ("browser");
            MixerEngine engine (0);
            auto& usb = engine.getUsbRecorder();
            usb.setStorageFolder (tmp.dir);
            expect (usb.getRecPath() == tmp.dir && usb.getBrowserFolder() == tmp.dir);
            for (auto fileName : { "song 10.wav", "song 2.WAV", "notes.txt", "set.m3u" })
                tmp.dir.getChildFile (fileName).replaceWithText ("x");
            expect (usb.createFolder (tmp.dir, "LIVE").wasOk());
            expect (usb.createFolder (tmp.dir, "LIVE").failed(), "exists already");
            expect (usb.createFolder (tmp.dir, "  ").failed());
            auto items = UsbRecorder::listFolder (tmp.dir);
            juce::StringArray names;
            for (auto& i : items)
                names.add (i.file.getFileName());
            expectEquals (names.joinIntoString ("|"), juce::String ("LIVE|set.m3u|song 2.WAV|song 10.wav"));
            expect (items[0].kind == UsbRecorder::ItemKind::Folder && items[1].kind == UsbRecorder::ItemKind::Playlist);

            juce::File renamed;
            expect (usb.rename (tmp.dir.getChildFile ("song 2.WAV"), "Intro", &renamed).wasOk());
            expectEquals (renamed.getFileName(), juce::String ("Intro.WAV"), "the extension stays");
            expect (usb.rename (renamed, "song 10.wav").failed(), "taken");
            expect (usb.deleteItem (tmp.dir.getChildFile ("LIVE")).wasOk());
            expect (! tmp.dir.getChildFile ("LIVE").exists());
            expect (usb.deleteItem (tmp.dir).failed(), "not the USB folder itself");
            // The queue follows the browser folder (its WAV files).
            expectEquals ((int) usb.getQueue().size(), 2);
        }

        beginTest ("Recording names its files R_YYYYMMDD-HHMMSS in the REC PATH");
        {
            UsbTempDir tmp ("names");
            MixerEngine engine (0);
            auto& usb = engine.getUsbRecorder();
            usb.setStorageFolder (tmp.dir);
            auto sub = tmp.dir.getChildFile ("REC");
            sub.createDirectory();
            usb.setRecPath (sub);
            const juce::Time when (2024, 11, 16, 14, 5, 57); // months count from 0
            auto f = usb.nextRecordingFile (when);
            expect (f == sub.getChildFile ("R_20241216-140557.wav"));
            f.replaceWithText ("x");
            expectEquals (usb.nextRecordingFile (when).getFileName(), juce::String ("R_20241216-140557-2.wav"));
        }

        beginTest ("A 4-channel 16-bit recording reads back with JUCE; NEW FILE splits it");
        {
            UsbTempDir tmp ("record");
            MixerEngine engine (0);
            UsbDriver driver (engine, 2, 2);
            auto& usb = engine.getUsbRecorder();
            usb.setStorageFolder (tmp.dir);
            UsbRecorder::Tap in1;
            in1.kind = UsbRecorder::Tap::Kind::Source;
            in1.source = 0;
            for (int port = 0; port < UsbRecorder::kNumChannels; ++port)
                usb.setTap (port, port == 2 ? in1 : UsbRecorder::Tap {});
            usb.setRecChannels (4);
            usb.setRecBits (16);
            expect (usb.record().wasOk());
            expect (usb.getRecState() == UsbRecorder::RecState::Recording);
            const auto first = usb.getRecFile();
            const auto start = driver.clock;
            auto input = [] (int ch, int64_t t) { return ch == 0 ? ramp16 (t) : 0.0f; };
            for (int b = 0; b < 100; ++b)
                driver.block (input);
            juce::Thread::sleep (1100); // the next file gets a new name (one per second)
            expect (usb.newFile().wasOk());
            const auto second = usb.getRecFile();
            expect (second != first);
            const auto split = driver.clock;
            for (int b = 0; b < 50; ++b)
                driver.block (input);
            usb.stopRecording();
            driver.run (2);
            expect (usb.waitForDisk());
            expect (usb.getRecState() == UsbRecorder::RecState::Stopped);

            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> a (formats.createReaderFor (first)), b (formats.createReaderFor (second));
            expect (a != nullptr && b != nullptr);
            if (a != nullptr && b != nullptr)
            {
                expectEquals ((int) a->numChannels, 4);
                expectEquals ((int) a->bitsPerSample, 16);
                expectEquals (a->sampleRate, 48000.0);
                expectEquals ((int) a->lengthInSamples, (int) (split - start));
                expectEquals ((int) b->lengthInSamples, 50 * UsbDriver::kBlock);
                juce::AudioBuffer<float> buf (4, (int) a->lengthInSamples);
                a->read (&buf, 0, buf.getNumSamples(), 0, true, true);
                bool exact = true;
                for (int i = 0; i < buf.getNumSamples(); ++i)
                    exact = exact && buf.getSample (2, i) == ramp16 (start + i) && buf.getSample (0, i) == 0.0f;
                expect (exact, "REC 3 = input 1, bit-exact");
                juce::AudioBuffer<float> rest (4, (int) b->lengthInSamples);
                b->read (&rest, 0, rest.getNumSamples(), 0, true, true);
                expectEquals (rest.getSample (2, 0), ramp16 (split), "NEW FILE goes on with the next sample");
            }
        }

        beginTest ("Playlists: save, open, play through, REPEAT, SKIP");
        {
            UsbTempDir tmp ("playlist");
            MixerEngine engine (0);
            UsbDriver driver (engine, 2, 2);
            auto& usb = engine.getUsbRecorder();
            usb.setStorageFolder (tmp.dir);
            juce::WavAudioFormat wav;
            for (auto fileName : { "a.wav", "b.wav" })
            {
                auto f = tmp.dir.getChildFile (fileName);
                std::unique_ptr<juce::OutputStream> stream (f.createOutputStream().release());
                auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (24));
                expect (writer != nullptr);
                if (writer == nullptr)
                    continue;
                juce::AudioBuffer<float> buf (2, 22050);
                for (int i = 0; i < buf.getNumSamples(); ++i)
                {
                    buf.setSample (0, i, 0.25f);
                    buf.setSample (1, i, -0.25f);
                }
                writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
            }
            usb.setBrowserFolder (tmp.dir);
            expectEquals ((int) usb.getQueue().size(), 2, "the folder's files");
            usb.createPlaylist();
            expect (usb.isPlaylistOpen() && usb.getQueue().empty());
            usb.add (tmp.dir.getChildFile ("a.wav"));
            usb.add (tmp.dir.getChildFile ("b.wav"));
            expectEquals (usb.move (1, -1), 0);
            usb.setSkip (1, true);
            usb.setRepeat (true);
            expect (usb.isPlaylistDirty());
            expect (usb.savePlaylist (tmp.dir.getChildFile ("SET")).wasOk());
            expect (usb.getPlaylistFile() == tmp.dir.getChildFile ("SET.m3u") && ! usb.isPlaylistDirty());
            usb.closePlaylist();
            expect (! usb.isPlaylistOpen());
            expectEquals ((int) usb.getQueue().size(), 2);
            expect (usb.openPlaylist (tmp.dir.getChildFile ("SET.m3u")).wasOk());
            auto queue = usb.getQueue();
            expect (queue.size() == 2 && queue[0].file.getFileName() == "b.wav" && queue[1].skip && usb.getRepeat());

            // b plays (a is skipped), then again (REPEAT): still playing after 1.5 s.
            usb.play();
            expect (usb.getPlayState() == UsbRecorder::PlayState::Playing);
            driver.run (600);
            expect (usb.getPlayState() == UsbRecorder::PlayState::Playing);
            expect (usb.getCurrentFile() == tmp.dir.getChildFile ("b.wav"));
            UsbRecorder::FileInfo info;
            int64_t length = 0;
            bool mismatch = false;
            expect (usb.getCurrentInfo (info, length, mismatch));
            expect (mismatch && info.sampleRate == 44100 && info.bits == 24 && info.channels == 2);
            expectEquals ((int) length, 24000, "half a second at 48 kHz");
            expect (std::abs (driver.outs[0][0]) < 1.0f);
            usb.setRepeat (false); // counts from the next decision (at most a clip and half a second ahead)
            driver.run (600);
            expect (usb.getPlayState() == UsbRecorder::PlayState::Stopped, "without REPEAT it ends");
            UsbRecorder::FileInfo header;
            expect (UsbRecorder::readWavInfo (tmp.dir.getChildFile ("a.wav"), header) && header.frames == 22050);
            expect (! UsbRecorder::readWavInfo (tmp.dir.getChildFile ("SET.m3u"), header));
        }
    }
};

static UsbRecorderTests usbRecorderTests;
