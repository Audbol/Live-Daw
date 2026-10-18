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

/** Amplitude and SNR (dB) of the best fit of a sine at `hz` to `x`. */
std::pair<double, double> fitSine (const float* x, int n, double hz, double rate)
{
    double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
    const double w = juce::MathConstants<double>::twoPi * hz / rate;
    for (int i = 0; i < n; ++i)
    {
        const double s = std::sin (w * i), c = std::cos (w * i);
        ss += s * s;
        sc += s * c;
        cc += c * c;
        ys += x[i] * s;
        yc += x[i] * c;
    }
    const double det = ss * cc - sc * sc, a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
    double err = 0, sig = 0;
    for (int i = 0; i < n; ++i)
    {
        const double m = a * std::sin (w * i) + b * std::cos (w * i);
        err += (x[i] - m) * (x[i] - m);
        sig += m * m;
    }
    return { std::sqrt (a * a + b * b), 10.0 * std::log10 (sig / juce::jmax (1.0e-30, err)) };
}
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

        beginTest ("FORMAT: FLAC and AIFF recordings read back bit-exact with JUCE (libFLAC), MP3 with JUCE's MP3 decoder");
        {
            UsbTempDir tmp ("formats");
            MixerEngine engine (0);
            UsbDriver driver (engine, 2, 2);
            auto& usb = engine.getUsbRecorder();
            usb.setStorageFolder (tmp.dir);
            UsbRecorder::Tap in1;
            in1.kind = UsbRecorder::Tap::Kind::Source;
            in1.source = 0;
            usb.setTap (0, in1);
            usb.setTap (1, in1);
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            const juce::Time when (2024, 0, 2, 3, 4, 5);
            for (auto type : { AudioFileFormat::Type::Flac, AudioFileFormat::Type::Aiff, AudioFileFormat::Type::Mp3 })
            {
                const bool mp3 = type == AudioFileFormat::Type::Mp3;
                usb.setRecFileType (type);
                usb.setRecChannels (4);
                usb.setRecBits (16);
                usb.setRecKbps (192);
                expectEquals (usb.getEffectiveRecChannels(), mp3 ? 2 : 4, "MP3 records two channels");
                expectEquals (usb.nextRecordingFile (when).getFileExtension(), AudioFileFormat { type, 16, 192 }.extension());
                expect (usb.record().wasOk());
                const auto file = usb.getRecFile();
                const auto start = driver.clock;
                auto input = [mp3] (int ch, int64_t t)
                {
                    if (ch != 0)
                        return 0.0f;
                    return mp3 ? 0.5f * (float) std::sin ((double) t * juce::MathConstants<double>::twoPi * 1000.0 / 48000.0) : ramp16 (t);
                };
                for (int b = 0; b < 750; ++b) // two seconds
                    driver.block (input);
                usb.stopRecording();
                driver.run (2);
                expect (usb.waitForDisk());
                expectEquals (file.getFileExtension(), AudioFileFormat { type, 16, 192 }.extension());

                UsbRecorder::FileInfo header;
                expect (UsbRecorder::readFileInfo (file, header));
                expect (header.type == type);
                expectEquals (header.frames, (int64_t) 750 * UsbDriver::kBlock, "every sample, nothing more");
                expectEquals (header.kbps, mp3 ? 192 : 0);

                std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
                expect (reader != nullptr, file.getFileName());
                if (reader == nullptr)
                    continue;
                expectEquals ((int) reader->numChannels, mp3 ? 2 : 4);
                juce::AudioBuffer<float> buf ((int) reader->numChannels, (int) reader->lengthInSamples);
                reader->read (&buf, 0, buf.getNumSamples(), 0, true, true);
                if (! mp3)
                {
                    bool exact = buf.getNumSamples() == 750 * UsbDriver::kBlock;
                    for (int i = 0; exact && i < buf.getNumSamples(); ++i)
                        exact = buf.getSample (0, i) == ramp16 (start + i) && buf.getSample (1, i) == ramp16 (start + i) && buf.getSample (3, i) == 0.0f;
                    expect (exact, AudioFileFormat::typeName (type) + " reads back bit-exact");
                }
                else
                {
                    // JUCE's own decoder: 1 kHz at -6 dB (whatever its delay).
                    const int n = buf.getNumSamples() - 8000;
                    const auto [amp, snr] = fitSine (buf.getReadPointer (0, 4000), n, 1000.0, 48000.0);
                    expectWithinAbsoluteError (20.0 * std::log10 (amp / 0.5), 0.0, 1.0);
                    expect (snr > 30.0, "SNR " + juce::String (snr));
                    // The engine's reader: sample-aligned with the input.
                    auto own = AudioFileReader::open (file);
                    expect (own != nullptr && own->getLength() == 750 * UsbDriver::kBlock);
                    if (own != nullptr)
                    {
                        std::vector<float> l (48000), r (48000);
                        float* chans[] = { l.data(), r.data() };
                        expect (own->read (24000, chans, 2, 48000));
                        double err = 0, sig = 0;
                        for (int i = 0; i < 48000; ++i)
                        {
                            const float want = input (0, start + 24000 + i);
                            err += (l[(size_t) i] - want) * (l[(size_t) i] - want);
                            sig += want * want;
                        }
                        expect (10.0 * std::log10 (sig / err) > 30.0, "aligned with what was recorded");
                    }
                }
            }
            // The format is kept in the settings.
            juce::PropertiesFile::Options options;
            options.applicationName = "usbformat";
            options.folderName = tmp.dir.getFullPathName();
            juce::PropertiesFile settings (tmp.dir.getChildFile ("settings.xml"), options);
            usb.setRecFileType (AudioFileFormat::Type::Mp3);
            usb.setRecKbps (256);
            usb.saveSettings (settings);
            usb.setRecFileType (AudioFileFormat::Type::Wav);
            usb.setRecKbps (128);
            usb.loadSettings (settings);
            expect (usb.getRecFileType() == AudioFileFormat::Type::Mp3 && usb.getRecKbps() == 256);
            // The browser lists every format.
            for (auto fileName : { "x.flac", "y.mp3", "z.aif", "w.ogg" })
                tmp.dir.getChildFile (fileName).replaceWithText ("x");
            int audio = 0;
            for (auto& item : UsbRecorder::listFolder (tmp.dir))
                if (item.kind == UsbRecorder::ItemKind::Wav)
                    ++audio;
            expectEquals (audio, 6, "the three recordings and x.flac, y.mp3, z.aif");
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
            expect (UsbRecorder::readFileInfo (tmp.dir.getChildFile ("a.wav"), header) && header.frames == 22050);
            expect (! UsbRecorder::readFileInfo (tmp.dir.getChildFile ("SET.m3u"), header));
        }
    }
};

static UsbRecorderTests usbRecorderTests;
