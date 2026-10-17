#include "daw/AudioFiles.h"
#include "daw/Project.h"
#include "daw/Session.h"
#include "daw/TimeGrid.h"
#include "engine/MixerEngine.h"

#include <juce_core/juce_core.h>

using namespace livedaw;
using namespace livedaw::daw;

namespace
{
/** A temporary folder, deleted with its contents at the end of the test. */
struct TempFolder
{
    TempFolder() : folder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getChildFile ("LiveDAW-DawTests-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64())))
    {
        folder.createDirectory();
    }
    ~TempFolder() { folder.deleteRecursively(); }

    juce::File folder;
};

/** Writes `seconds` of a sine (stereo: the right channel at half level) as a float WAV. */
juce::File writeSine (const juce::File& file, double rate, double seconds, int channels, float level = 0.5f)
{
    auto writer = createWavWriter (file, rate, channels);
    const auto n = (int) (rate * seconds);
    juce::AudioBuffer<float> b (channels, n);
    for (int i = 0; i < n; ++i)
        for (int ch = 0; ch < channels; ++ch)
            b.setSample (ch, i, level * (ch == 0 ? 1.0f : 0.5f) * std::sin ((float) i * 440.0f * juce::MathConstants<float>::twoPi / (float) rate));
    writer->writeFromAudioSampleBuffer (b, 0, n);
    return file;
}

ClipData makeClip (int64 start, int64 length, int64 fileOffset = 0, int64 fileLength = 1000000)
{
    ClipData c;
    c.file = "Audio/a.wav";
    c.start = start;
    c.length = length;
    c.fileOffset = fileOffset;
    c.fileLength = fileLength;
    return c;
}
} // namespace

class DawTests final : public juce::UnitTest
{
public:
    DawTests() : juce::UnitTest ("DAW project", "LiveDAW") {}

    void runTest() override
    {
        testClipMath();
        testTimeGrid();
        testUndoRedo();
        testEdits();
        testPlaylist();
        testSaveLoad();
        testTakes();
        testImportAndRender();
        testSessionSetups();
    }

private:
    void testClipMath()
    {
        beginTest ("Split, trim and move keep the audio in place");
        {
            auto c = makeClip (1000, 5000, 200, 10000);
            c.fadeIn = 300;
            c.fadeOut = 400;

            expect (clipmath::canSplit (c, 3000));
            expect (! clipmath::canSplit (c, 1000 + 10));
            const auto [left, right] = clipmath::split (c, 3000);
            expectEquals (left.start, (int64) 1000);
            expectEquals (left.length, (int64) 2000);
            expectEquals (left.fileOffset, (int64) 200);
            expectEquals (left.fadeIn, (int64) 300);
            expectEquals (left.fadeOut, (int64) 0);
            expectEquals (right.start, (int64) 3000);
            expectEquals (right.length, (int64) 3000);
            expectEquals (right.fileOffset, (int64) 2200); // the sample that was at 3000 still plays at 3000
            expectEquals (right.fadeIn, (int64) 0);
            expectEquals (right.fadeOut, (int64) 400);

            // Trimming the start reveals the file only back to its first sample.
            const auto t1 = clipmath::trimStart (c, 500);
            expectEquals (t1.start, (int64) 800);
            expectEquals (t1.fileOffset, (int64) 0);
            expectEquals (t1.end(), c.end());
            const auto t2 = clipmath::trimStart (c, 2000);
            expectEquals (t2.start, (int64) 2000);
            expectEquals (t2.fileOffset, (int64) 1200);
            expectEquals (t2.length, (int64) 4000);
            // ... and never past the end.
            const auto t3 = clipmath::trimStart (c, 99999);
            expectEquals (t3.length, clipmath::kMinLength);
            expectEquals (t3.end(), c.end());

            // The end stops at the end of the file.
            expectEquals (clipmath::trimEnd (c, 2000000).length, (int64) 9800);
            expectEquals (clipmath::trimEnd (c, 4000).length, (int64) 3000);
            expectEquals (clipmath::trimEnd (c, 0).length, clipmath::kMinLength);
            // Fades shrink with the clip.
            const auto t4 = clipmath::trimEnd (c, 1500);
            expect (t4.fadeIn + t4.fadeOut <= t4.length);

            expectEquals (clipmath::moveTo (c, -50).start, (int64) 0);
            expectEquals (clipmath::moveTo (c, 7777).fileOffset, c.fileOffset);

            const auto middle = clipmath::removeRange (c, 2000, 3000);
            expectEquals ((int) middle.size(), 2);
            expectEquals (middle[0].end(), (int64) 2000);
            expectEquals (middle[1].start, (int64) 3000);
            expectEquals (middle[1].fileOffset, (int64) 2200);
            expectEquals ((int) clipmath::removeRange (c, 0, 99999).size(), 0);
            expectEquals ((int) clipmath::removeRange (c, 7000, 8000).size(), 1);

            expectWithinAbsoluteError (clipmath::fadeGain (FadeShape::EqualPower, 0.5f), 0.7071f, 0.001f);
            expectWithinAbsoluteError (clipmath::fadeGain (FadeShape::SCurve, 0.5f), 0.5f, 0.001f);
            expectEquals (clipmath::fadeGain (FadeShape::Linear, 2.0f), 1.0f);
        }
    }

    void testTimeGrid()
    {
        beginTest ("Time grid: snapping and formatting");
        {
            TimeGrid g;
            g.sampleRate = 48000.0;
            g.tempo = 120.0;
            expectEquals (g.samplesPerBeat(), 24000.0);
            expectEquals (g.samplesPerBar(), 96000.0);
            expectEquals (g.snap (50000, SnapMode::Beats, 100.0), (juce::int64) 48000);
            expectEquals (g.snap (50000, SnapMode::Bars, 100.0), (juce::int64) 96000);
            expectEquals (g.snap (50000, SnapMode::Seconds, 1000.0), (juce::int64) 48000);
            expectEquals (g.snap (50001, SnapMode::Off, 1.0), (juce::int64) 50001);
            // GRID follows the zoom: fine when zoomed in, bars when zoomed out.
            expect (g.step (SnapMode::Grid, 10.0) < g.samplesPerBeat());
            expect (g.step (SnapMode::Grid, 20000.0) >= g.samplesPerBar());
            expectEquals (g.formatTime (48000 * 83 + 24000), juce::String ("1:23.500"));
            expectEquals (g.formatBars (96000 * 2 + 24000 + 12000), juce::String ("3|2|240"));
            bool snapped = false;
            expectEquals (snapToEdges (1005, { 0, 1000, 2000 }, 10, &snapped), (juce::int64) 1000);
            expect (snapped);
            expectEquals (snapToEdges (1500, { 0, 1000, 2000 }, 10, &snapped), (juce::int64) 1500);
            expect (! snapped);
        }
    }

    void testUndoRedo()
    {
        beginTest ("Every edit undoes and redoes");
        {
            Project p;
            p.clear (48000.0);
            p.beginEdit ("Add track");
            const auto t1 = p.addTrack();
            p.beginEdit ("Add clip");
            const auto c1 = p.addClip (t1, makeClip (1000, 4000));
            p.beginEdit ("Move");
            p.setClip (clipmath::moveTo (*p.getClip (c1), 9000));
            p.beginEdit ("Rename track");
            p.setTrackProperty (t1, ids::name, "Kick");
            expectEquals (p.getTrackData (0).name, juce::String ("Kick"));
            expectEquals (p.getClip (c1)->start, (int64) 9000);

            expect (p.undoLast());
            expectEquals (p.getTrackData (0).name, juce::String ("Track 1"));
            expect (p.undoLast());
            expectEquals (p.getClip (c1)->start, (int64) 1000);
            expect (p.undoLast());
            expect (! p.getClip (c1).has_value());
            expect (p.redoNext());
            expectEquals (p.getClip (c1)->start, (int64) 1000);
            expect (p.redoNext());
            expectEquals (p.getClip (c1)->start, (int64) 9000);

            // A drag changes a clip many times in one action: one undo step.
            p.beginEdit ("Drag");
            for (int64 x = 9000; x <= 20000; x += 1000)
                p.setClip (clipmath::moveTo (*p.getClip (c1), x));
            expect (p.undoLast());
            expectEquals (p.getClip (c1)->start, (int64) 9000);

            // Arming and heights don't land on the undo stack.
            p.beginEdit ("Arm");
            p.setTrackProperty (t1, ids::arm, true, false);
            p.setTrackProperty (t1, ids::height, 150, false);
            expect (p.getUndoManager().getUndoDescription() == "Move");
            expect (p.getTrackData (0).arm);

            // Split: undo joins the parts again.
            p.beginEdit ("Split");
            const auto made = p.splitClips ({ c1 }, 10000);
            expectEquals ((int) made.size(), 1);
            expectEquals ((int) p.getClips (t1).size(), 2);
            expect (p.undoLast());
            expectEquals ((int) p.getClips (t1).size(), 1);
            expectEquals (p.getClip (c1)->length, (int64) 4000);

            p.beginEdit ("Delete track");
            p.removeTrack (t1);
            expectEquals (p.getNumTracks(), 0);
            expect (p.undoLast());
            expectEquals (p.getNumTracks(), 1);
            expectEquals ((int) p.getClips (t1).size(), 1);
        }
    }

    void testEdits()
    {
        beginTest ("Track edits, moves between tracks, crossfades and markers");
        {
            Project p;
            p.clear (48000.0);
            const auto a = p.addTrack();
            TrackData stereo;
            stereo.stereo = true;
            const auto b = p.addTrack (stereo);
            const auto c = p.addTrack();
            expectEquals (p.getTrackData (0).playChannel, 0);
            expectEquals (p.getTrackData (1).playChannel, 1); // 1 and 2
            expectEquals (p.getTrackData (2).playChannel, 3);
            expectEquals (p.getTrackData (1).name, juce::String ("Track 2"));

            p.moveTrack (c, 0);
            expectEquals (p.indexOfTrack (c), 0);
            expectEquals (p.indexOfTrack (a), 1);

            const auto clip = p.addClip (a, makeClip (0, 1000));
            p.moveClipToTrack (clip, b);
            expectEquals (p.trackOfClip (clip), b);
            expect (p.getClips (a).empty());

            // Partial overlap: a crossfade over the overlap.
            const auto x = p.addClip (a, makeClip (0, 10000));
            const auto y = p.addClip (a, makeClip (8000, 10000));
            p.crossfadeOverlaps (a);
            expectEquals (p.getClip (x)->fadeOut, (int64) 2000);
            expectEquals (p.getClip (y)->fadeIn, (int64) 2000);

            // Clearing a range trims and splits.
            p.clearRange (a, 4000, 12000);
            const auto clips = p.getClips (a);
            expectEquals ((int) clips.size(), 2);
            int64 total = 0;
            for (auto& cl : clips)
            {
                expect (cl.end() <= 4000 || cl.start >= 12000);
                total += cl.length;
            }
            expectEquals (total, (int64) 4000 + 6000);

            const auto m2 = p.addMarker (96000, "Chorus");
            p.addMarker (48000);
            const auto markers = p.getMarkers();
            expectEquals ((int) markers.size(), 2);
            expectEquals (markers[0].position, (int64) 48000);
            expectEquals (markers[1].name, juce::String ("Chorus"));
            p.setMarker (m2, 10, "Intro");
            expectEquals (p.getMarkers()[0].name, juce::String ("Intro"));

            p.setLoop (5000, 1000);
            expectEquals (p.getLoopStart(), (int64) 1000);
            expectEquals (p.getLoopEnd(), (int64) 5000);
            expectEquals (p.getLength(), (int64) 18000);
        }
    }

    void testPlaylist()
    {
        beginTest ("Playlist: gains, mute, solo, muted clips and missing files");
        {
            TempFolder temp;
            Project p;
            p.clear (48000.0);
            expect (p.saveAs (temp.folder.getChildFile ("Song")).wasOk());
            const auto file = writeSine (p.getAudioFolder().getChildFile ("a.wav"), 48000.0, 0.5, 1);

            TrackData td;
            td.gainDb = -6.0f;
            const auto t1 = p.addTrack (td);
            const auto t2 = p.addTrack();
            const auto t3 = p.addTrack();
            auto c = makeClip (100, 1000, 10, 24000);
            c.file = p.storedPath (file);
            expectEquals (c.file, juce::String ("Audio/a.wav"));
            c.gainDb = 6.0f;
            c.fadeIn = 50;
            c.fadeInShape = FadeShape::SCurve;
            p.addClip (t1, c);
            auto muted = c;
            muted.mute = true;
            p.addClip (t1, muted);
            p.addClip (t2, c);
            auto missing = c;
            missing.file = "Audio/missing.wav";
            p.addClip (t3, missing);

            auto list = p.buildPlaylist();
            expectEquals ((int) list.size(), 3);
            expectEquals ((int) list[0].clips.size(), 1); // the muted clip is left out
            expectEquals ((int) list[2].clips.size(), 0); // so is the missing file
            expectWithinAbsoluteError (list[0].gain, 0.501f, 0.002f);
            expectWithinAbsoluteError (list[0].clips[0].gain, 1.995f, 0.003f);
            expect (list[0].clips[0].file == file);
            expectEquals (list[0].clips[0].fileOffset, (int64_t) 10);
            expectEquals (list[0].clips[0].fadeIn, (int64_t) 50);
            expect (list[0].clips[0].fadeInShape == FadeShape::SCurve);
            expect (! list[0].mute && ! list[1].mute);
            expectEquals (list[1].channel, 1);

            p.setTrackProperty (t1, ids::mute, true);
            list = p.buildPlaylist();
            expect (list[0].mute && ! list[1].mute);

            // Solo: only soloed tracks play.
            p.setTrackProperty (t1, ids::mute, false);
            p.setTrackProperty (t2, ids::solo, true);
            list = p.buildPlaylist();
            expect (list[0].mute);
            expect (! list[1].mute);
            expect (list[2].mute);

            p.setTrackProperty (t2, ids::stereo, true);
            p.setTrackProperty (t2, ids::playChannel, 63);
            list = p.buildPlaylist();
            expectEquals (list[1].numChannels, 2);
            expectEquals (list[1].channel, 62); // kept inside the 64 playback channels
        }
    }

    void testSaveLoad()
    {
        beginTest ("Save and load round trip");
        {
            TempFolder temp;
            const auto folder = temp.folder.getChildFile ("Live 2026");
            juce::String xmlBefore;
            int64 clipId = 0;
            {
                Project p;
                p.clear (44100.0);
                p.setTempo (98.5);
                p.setTimeSignature (6, 8);
                p.setLoop (1000, 900000);
                p.setLoopOn (true);
                TrackData t;
                t.name = "Vox";
                t.colour = 7;
                t.stereo = true;
                t.source.kind = Recorder::Source::Kind::StripPostFader;
                t.source.stripKind = StripKind::Bus;
                t.source.stripNumber = 3;
                t.gainDb = -3.5f;
                t.mute = true;
                t.height = 120;
                const auto id = p.addTrack (t);
                auto c = makeClip (4410, 88200, 100, 200000);
                c.file = folder.getChildFile ("Audio/vox.wav").getFullPathName(); // stored relative once saved
                c.fadeOut = 1000;
                c.fadeOutShape = FadeShape::Linear;
                c.gainDb = -2.0f;
                c.name = "Verse";
                clipId = p.addClip (id, c);
                p.addMarker (44100, "Verse");
                expect (p.saveAs (folder).wasOk());
                expect (! p.isDirty());
                expect (p.getFile().existsAsFile());
                expectEquals (p.getFile().getFileName(), juce::String ("Live 2026.ldproj"));
                expect (p.getAudioFolder().isDirectory());
                expectEquals (p.getClip (clipId)->file, juce::String ("Audio/vox.wav"));
                xmlBefore = p.getState().toXmlString();
            }

            Project q;
            expect (q.load (folder.getChildFile ("Live 2026.ldproj")).wasOk());
            expectEquals (q.getName(), juce::String ("Live 2026"));
            expectEquals (q.getSampleRate(), 44100.0);
            expectEquals (q.getTempo(), 98.5);
            expectEquals (q.getSigNumerator(), 6);
            expectEquals (q.getSigDenominator(), 8);
            expect (q.isLoopOn());
            expectEquals (q.getLoopEnd(), (int64) 900000);
            const auto t = q.getTrackData (0);
            expectEquals (t.name, juce::String ("Vox"));
            expectEquals (t.colour, 7);
            expect (t.stereo && t.mute && ! t.solo);
            expect (t.source.kind == Recorder::Source::Kind::StripPostFader);
            expect (t.source.stripKind == StripKind::Bus);
            expectEquals (t.source.stripNumber, 3);
            expectEquals (t.gainDb, -3.5f);
            expectEquals (t.height, 120);
            const auto c = q.getClip (clipId);
            expect (c.has_value());
            expectEquals (c->start, (int64) 4410);
            expectEquals (c->fileOffset, (int64) 100);
            expectEquals (c->fileLength, (int64) 200000);
            expectEquals (c->fadeOut, (int64) 1000);
            expect (c->fadeOutShape == FadeShape::Linear);
            expect (c->fadeInShape == FadeShape::EqualPower);
            expectEquals (c->gainDb, -2.0f);
            expectEquals (c->name, juce::String ("Verse"));
            expect (q.resolve (c->file) == folder.getChildFile ("Audio/vox.wav"));
            expectEquals (q.getMarkers()[0].name, juce::String ("Verse"));
            expect (! q.canUndo());

            // New ids continue after the loaded ones.
            const auto newTrack = q.addTrack();
            expect (newTrack > clipId);

            // A damaged file falls back to the backup of the previous save.
            expect (q.save().wasOk());
            q.getFile().replaceWithText ("<LDPROJECT broken");
            Project r;
            expect (r.load (q.getFile()).wasOk());
            expectEquals (r.getNumTracks(), 1);
            expect (r.load (temp.folder.getChildFile ("nothing.ldproj")).failed());

            // A moved folder still finds its audio.
            const auto moved = temp.folder.getChildFile ("Moved");
            expect (folder.moveFileTo (moved));
            Project m;
            expect (m.load (moved.getChildFile ("Live 2026.ldproj")).wasOk());
            expect (m.resolve (m.getClip (clipId)->file) == moved.getChildFile ("Audio/vox.wav"));
        }
    }

    void testTakes()
    {
        beginTest ("Finished takes become clips");
        {
            TempFolder temp;
            Project p;
            p.clear (48000.0);
            expect (p.saveAs (temp.folder.getChildFile ("Gig")).wasOk());
            const auto kick = p.addTrack();
            const auto snare = p.addTrack();
            p.addClip (kick, makeClip (0, 48000 * 10));

            const auto take1 = writeSine (p.getAudioFolder().getChildFile ("Kick.wav"), 48000.0, 1.0, 1);
            const auto take2 = writeSine (p.getAudioFolder().getChildFile ("Snare.wav"), 48000.0, 1.0, 2);
            std::vector<Recorder::Take> takes {
                { 0, take1, 48000 * 4, 48000, 1 },
                { 1, take2, 48000 * 4, 48000, 2 },
                { 5, take2, 0, 100, 1 } // a track index that wasn't recording
            };
            p.beginEdit ("Record");
            const auto created = p.addTakes (takes, { { 0, kick }, { 1, snare } }, true);
            expectEquals ((int) created.size(), 2);

            const auto c = p.getClip (created[0]);
            expectEquals (c->start, (int64) 48000 * 4);
            expectEquals (c->length, (int64) 48000);
            expectEquals (c->fileLength, (int64) 48000);
            expectEquals (c->file, juce::String ("Audio/Kick.wav"));
            expectEquals (c->name, juce::String ("Kick"));
            expectEquals (p.getClip (created[1])->numChannels, 2);

            // Replace: the take's range was cut out of the clip under it.
            expectEquals ((int) p.getClips (kick).size(), 3);
            for (auto& clip : p.getClips (kick))
                if (clip.id != created[0])
                    expect (clip.end() <= 48000 * 4 || clip.start >= 48000 * 5);

            // One undo removes the whole recording.
            expect (p.undoLast());
            expectEquals ((int) p.getClips (kick).size(), 1);
            expect (p.getClips (snare).empty());
        }

        beginTest ("Session: record the armed tracks, the take becomes a clip and plays back through an ALT input");
        {
            TempFolder temp;
            MixerEngine engine (0);
            engine.setNumChannels (2);
            constexpr int kBlock = 128;
            engine.prepareOffline (48000.0, kBlock, 2, 2);
            auto& recorder = engine.getRecorder();
            Session session (recorder, nullptr);
            session.setDefaultFolder (temp.folder);
            auto& p = session.getProject();
            expectEquals (p.getSampleRate(), 48000.0);

            // Runs the engine offline, giving the disk threads time before each block.
            std::vector<float> in0 (kBlock), in1 (kBlock), out0 (kBlock), out1 (kBlock);
            auto run = [&] (int blocks, float level)
            {
                for (int b = 0; b < blocks; ++b)
                {
                    recorder.waitForDisk();
                    std::fill (in0.begin(), in0.end(), level);
                    std::fill (in1.begin(), in1.end(), 0.0f);
                    const float* ins[] = { in0.data(), in1.data() };
                    float* outs[] = { out0.data(), out1.data() };
                    engine.processBlock (ins, 2, outs, 2, kBlock);
                }
            };

            TrackData td;
            td.name = "Vox";
            td.source.kind = Recorder::Source::Kind::HardwareInput;
            td.source.port = 0;
            const auto t = p.addTrack (td);
            expect (session.record().failed()); // nothing armed
            expect (! p.hasFolder());

            p.setTrackProperty (t, ids::arm, true, false);
            const auto result = session.record();
            expect (result.wasOk(), result.getErrorMessage());
            // The take gets a dated project folder when there was none.
            expect (p.hasFolder());
            expect (p.getFolder().isAChildOf (temp.folder));
            expect (session.isRecordingTrack (t));
            run (375, 0.25f); // one second of DC on input 1
            session.stop();
            run (8, 0.0f);
            for (int i = 0; i < 500 && p.getClips (t).empty(); ++i)
            {
                recorder.waitForDisk();
                session.collectTakes();
                juce::Thread::sleep (2);
            }
            const auto clips = p.getClips (t);
            expectEquals ((int) clips.size(), 1);
            if (clips.size() == 1)
            {
                const auto file = p.resolve (clips[0].file);
                expect (file.isAChildOf (p.getAudioFolder()));
                expectEquals (file.getFileName(), juce::String ("Vox.wav"));
                expect (std::abs (clips[0].length - 48000) <= kBlock * 2, juce::String (clips[0].length));
                expect (! p.isDirty()); // saved with its takes
                std::unique_ptr<juce::AudioFormatReader> reader (session.getFormats().createReaderFor (file));
                expect (reader != nullptr);
                if (reader != nullptr)
                {
                    juce::AudioBuffer<float> b (1, 4800);
                    reader->read (&b, 0, 4800, 24000, true, false);
                    expectWithinAbsoluteError (b.getSample (0, 100), 0.25f, 0.001f);
                }

                // VIRTUAL SOUNDCHECK: channel 2 takes the track's playback channel as its ALT input.
                Session::ChannelInfo ch2;
                ch2.number = 2;
                ch2.inputPort = 0; // patched to the input the track recorded
                const auto routes = session.virtualSoundcheckRoutes ({ ch2 });
                expectEquals ((int) routes.size(), 1);
                expectEquals (routes[0].channelNumber, 2);
                expectEquals (routes[0].playChannel, 0);
                auto& strip = *engine.getChannels()[1];
                strip.setAltInput (Recorder::playbackSource (routes[0].playChannel));
                strip.params.inputLeft = -1;
                engine.setInputSelectGlobal (true);
                engine.setGlobalAlt (true);
                expect (strip.isUsingAltInput());

                // Play the take from the start: the channel's input meter shows it.
                p.setTrackProperty (t, ids::arm, false, false);
                session.locate (0);
                session.play();
                strip.peakMeter (MeterPoint::Input, 0).store (0.0f);
                run (250, 0.0f);
                expect (session.getState() == Recorder::State::Playing);
                expectWithinAbsoluteError (strip.peakMeter (MeterPoint::Input, 0).load(), 0.25f, 0.01f);
                session.stop();
            }
        }
    }

    void testImportAndRender()
    {
        beginTest ("Import converts to WAV at the project's rate; export renders gains and fades");
        {
            TempFolder temp;
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            const auto source = writeSine (temp.folder.getChildFile ("source.wav"), 44100.0, 1.0, 2);
            std::atomic<bool> cancel { false };
            const auto r = importAudioFile (formats, source, temp.folder.getChildFile ("Audio"), 48000.0, cancel);
            expect (r.result.wasOk(), r.result.getErrorMessage());
            expectEquals (r.length, (int64) 48000);
            expectEquals (r.numChannels, 2);
            expectEquals (r.name, juce::String ("source"));
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (r.file));
            expect (reader != nullptr);
            expectEquals (reader->sampleRate, 48000.0);
            expectEquals ((int64) reader->lengthInSamples, (int64) 48000);
            // The resampled sine keeps its level (and isn't shifted by the filter's delay).
            juce::AudioBuffer<float> b (2, 48000);
            reader->read (&b, 0, 48000, 0, true, true);
            expectWithinAbsoluteError (b.getMagnitude (0, 1000, 46000), 0.5f, 0.01f);
            expectWithinAbsoluteError (b.getMagnitude (1, 1000, 46000), 0.25f, 0.01f);
            expectWithinAbsoluteError (b.getSample (0, 0), 0.0f, 0.02f);
            expect (std::abs (b.getSample (0, 27)) > 0.3f); // 440 Hz: a quarter period is 27 samples

            // Same rate: copied as it is.
            const auto same = importAudioFile (formats, source, temp.folder.getChildFile ("Audio"), 44100.0, cancel);
            expectEquals (same.length, (int64) 44100);
            expect (same.file != r.file); // a fresh name

            expect (! canImport (formats, temp.folder.getChildFile ("x.txt")));
            expect (canImport (formats, temp.folder.getChildFile ("x.aiff")));
            expect (importWildcard (formats).contains ("*.wav"));
            expect (importAudioFile (formats, temp.folder.getChildFile ("none.wav"), temp.folder, 48000.0, cancel).result.failed());

            // Render: a clip at 0.25 s with -6 dB and a linear fade-in.
            ClipData c;
            c.start = 12000;
            c.length = 24000;
            c.fileOffset = 0;
            c.gainDb = -6.0206f;
            c.fadeIn = 12000;
            c.fadeInShape = FadeShape::Linear;
            const auto dest = temp.folder.getChildFile ("Export/01 Track.wav");
            const auto rendered = renderTrack (formats, { { r.file, c } }, 0.0f, 1, 48000.0, 48000, dest, cancel);
            expect (rendered.wasOk(), rendered.getErrorMessage());
            std::unique_ptr<juce::AudioFormatReader> out (formats.createReaderFor (dest));
            expect (out != nullptr);
            expectEquals ((int64) out->lengthInSamples, (int64) 48000);
            juce::AudioBuffer<float> o (1, 48000);
            out->read (&o, 0, 48000, 0, true, false);
            expectEquals (o.getMagnitude (0, 0, 12000), 0.0f);
            expectWithinAbsoluteError (o.getMagnitude (0, 24000, 12000), 0.25f, 0.01f);
            expect (o.getMagnitude (0, 12000, 2000) < 0.05f); // the fade starts at zero
            expectEquals (o.getMagnitude (0, 36000, 12000), 0.0f);
        }
    }

    void testSessionSetups()
    {
        beginTest ("RECORD ALL CHANNELS: a track per channel, armed, from its input");
        {
            MixerEngine engine (0);
            auto& recorder = engine.getRecorder();
            Session session (recorder, nullptr);
            auto& p = session.getProject();
            const auto existing = p.addTrack();
            p.addClip (existing, makeClip (0, 1000));
            std::vector<Session::ChannelInfo> channels;
            for (int i = 1; i <= 4; ++i)
            {
                Session::ChannelInfo ch;
                ch.number = i;
                ch.name = i == 2 ? "Snare" : "";
                ch.colour = i;
                ch.stereo = i == 3;
                ch.inputPort = i == 4 ? -1 : i - 1;
                channels.push_back (ch);
            }
            session.setupRecordAllChannels (channels);
            expectEquals (p.getNumTracks(), 4);
            expectEquals (p.getTrackData (0).id, existing); // reused, clips kept
            expectEquals ((int) p.getClips (existing).size(), 1);
            expectEquals (p.getTrackData (0).name, juce::String ("CH 1"));
            expectEquals (p.getTrackData (1).name, juce::String ("Snare"));
            expectEquals (session.getNumArmed(), 4);
            expectEquals (session.getNumArmedChannels(), 5);
            expect (p.getTrackData (0).source.kind == Recorder::Source::Kind::HardwareInput);
            expectEquals (p.getTrackData (1).source.port, 1);
            expect (p.getTrackData (3).source.kind == Recorder::Source::Kind::StripPreFader);
            expectEquals (p.getTrackData (3).source.stripNumber, 4);
            expectEquals (p.getTrackData (2).playChannel, 2);
            expectEquals (p.getTrackData (3).playChannel, 4); // after the stereo pair
            expect (p.undoLast());
            expectEquals (p.getNumTracks(), 1);

            session.armAll (false);
            expectEquals (session.getNumArmed(), 0);
        }
    }
};

static DawTests dawTests;
