#include "daw/AudioFiles.h"
#include "daw/Project.h"
#include "daw/Session.h"
#include "daw/TimeGrid.h"
#include "engine/AppPaths.h"
#include "engine/MixerEngine.h"
#include "TestHelpers.h"

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
        testKeepInPlace();
        testFormats();
        testSessionSetups();
        testPlayModesAndAutoInput();
        testDefaultFolders();
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

            // Pulled apart, the crossfade's fades go; a fade changed since stays.
            {
                const auto crossfades = p.getCrossfades (a);
                expectEquals ((int) crossfades.size(), 1);
                expect (crossfades[0].first == x && crossfades[0].second == y && crossfades[0].length == 2000);
                p.setClip (clipmath::moveTo (*p.getClip (y), 9000)); // still overlapping: kept
                p.releaseCrossfades (crossfades);
                expectEquals (p.getClip (y)->fadeIn, (int64) 2000);
                p.setClip (clipmath::moveTo (*p.getClip (y), 20000));
                p.setClipProperty (x, ids::fadeOut, (int64) 500);
                p.releaseCrossfades (crossfades);
                expectEquals (p.getClip (x)->fadeOut, (int64) 500);
                expectEquals (p.getClip (y)->fadeIn, (int64) 0);
                p.setClipProperty (x, ids::fadeOut, (int64) 0);
                p.setClip (clipmath::moveTo (*p.getClip (y), 8000));
                p.crossfadeOverlaps (a);
            }

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
                strip.setSrcAuto (true);
                engine.setGlobalAlt (true);
                expect (strip.isUsingAltInput());
                expect (session.isRoutedToRecorder (StripKind::Channel, 2, -1, strip.getAltInputLeft()));
                expect (! session.isRoutedToRecorder (StripKind::Channel, 1, 5, -1));
                expect (session.isRoutedToRecorder (StripKind::Channel, 1, 0, -1), "a track records port 0");

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

        beginTest ("Session: a take never goes into a project at another rate than the console's");
        {
            TempFolder temp;
            MixerEngine engine (0);
            engine.prepareOffline (48000.0, 128, 2, 2);
            auto& recorder = engine.getRecorder();
            Session session (recorder, nullptr);
            session.setDefaultFolder (temp.folder);
            auto& p = session.getProject();
            TrackData td;
            td.arm = true;
            const auto t = p.addTrack (td);
            const auto m = p.addMarker (48000);
            p.setLoop (24000, 96000);

            // The clock rate changes: a project without audio follows it, markers keep their time.
            engine.prepareOffline (96000.0, 128, 2, 2);
            expect (! session.rateMatches());
            auto result = session.record();
            expect (result.wasOk(), result.getErrorMessage());
            session.stop();
            expectEquals (p.getSampleRate(), 96000.0);
            expect (session.rateMatches());
            expectEquals (p.getMarkers()[0].position, (int64) 96000);
            expect (p.getMarkers()[0].id == m);
            expectEquals (p.getLoopStart(), (int64) 48000);
            expectEquals (p.getLoopEnd(), (int64) 192000);

            // With audio it stays, and recording is refused.
            p.addClip (t, makeClip (0, 1000));
            engine.prepareOffline (48000.0, 128, 2, 2);
            result = session.record();
            expect (result.failed());
            expect (! session.isRecording());
            expectEquals (p.getSampleRate(), 96000.0);
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

    void testKeepInPlace()
    {
        beginTest ("IMPORT: KEEP IN PLACE references what plays as it is, copies the rest and says why");
        {
            TempFolder temp;
            juce::AudioFormatManager formats;
            formats.registerFormat (new EngineMp3AudioFormat(), false);
            formats.registerBasicFormats();
            const auto library = temp.folder.getChildFile ("Library"); // the samples drive
            auto writeIn = [&library] (const juce::String& fileName, AudioFileFormat format, double rate, int channels)
            {
                const auto f = library.getChildFile (fileName);
                library.createDirectory();
                auto w = AudioFileWriter::create (f, channels, rate, format);
                std::vector<std::vector<float>> data ((size_t) channels, std::vector<float> ((size_t) rate / 4));
                for (auto& ch : data)
                    for (size_t i = 0; i < ch.size(); ++i)
                        ch[i] = 0.3f * (float) std::sin ((double) i * 0.05);
                std::vector<const float*> ptrs;
                for (auto& ch : data)
                    ptrs.push_back (ch.data());
                w->write (ptrs.data(), channels, (int) rate / 4);
                w->finalize();
                return f;
            };
            expectEquals (khzText (44100.0), juce::String ("44.1"));
            expectEquals (khzText (48000.0), juce::String ("48"));
            expectEquals (khzText (88200.0), juce::String ("88.2"));

            int64 length = 0;
            int channels = 0;
            expectEquals (whyNotInPlace (formats, writeIn ("a.flac", AudioFileFormat::flac (24), 48000.0, 2), 48000.0, &length, &channels),
                          juce::String());
            expectEquals (length, (int64) 12000);
            expectEquals (channels, 2);
            expectEquals (whyNotInPlace (formats, writeIn ("a.aif", AudioFileFormat::aiff (24), 48000.0, 1), 48000.0), juce::String());
            expectEquals (whyNotInPlace (formats, writeIn ("b.wav", AudioFileFormat::wav (16), 44100.0, 2), 48000.0),
                          juce::String ("resampled copy: 44.1 kHz file in a 48 kHz project"));
            expectEquals (whyNotInPlace (formats, writeIn ("c.wav", AudioFileFormat::wav (24), 48000.0, 4), 48000.0),
                          juce::String ("copy of the first two channels: 4-channel file"));
            expect (whyNotInPlace (formats, writeIn ("d.mp3", AudioFileFormat::mp3 (192), 48000.0, 2), 48000.0).startsWith ("decoded copy"));
            library.getChildFile ("e.wav").replaceWithText ("not audio");
            expect (whyNotInPlace (formats, library.getChildFile ("e.wav"), 48000.0).startsWith ("converted copy"));
        }

        beginTest ("KEEP IN PLACE: referenced files survive save, Save As, going offline and COLLECT FILES; the settings persist");
        {
            TempFolder temp;
            MixerEngine engine (0);
            constexpr int kBlock = 128;
            engine.prepareOffline (48000.0, kBlock, 2, 2);
            auto& recorder = engine.getRecorder();
            const auto settingsFile = temp.folder.getChildFile ("test.settings");
            juce::PropertiesFile::Options options;
            juce::PropertiesFile settings (settingsFile, options);
            Session session (recorder, &settings);
            session.setDefaultFolder (temp.folder.getChildFile ("Projects"));
            auto& p = session.getProject();
            juce::StringArray messages;
            session.onMessage = [&messages] (const juce::String& m, bool) { messages.add (m); };
            auto wait = [&session]
            {
                for (int i = 0; i < 2000 && session.isBusy(); ++i)
                    test::pumpMessages (5);
                test::pumpMessages (5);
            };
            auto said = [&messages] (const juce::String& text) { return messages.joinIntoString ("\n").contains (text); };
            std::vector<float> in0 (kBlock), out0 (kBlock), out1 (kBlock);
            auto run = [&] (int blocks)
            {
                for (int b = 0; b < blocks; ++b)
                {
                    recorder.waitForDisk();
                    const float* ins[] = { in0.data(), in0.data() };
                    float* outs[] = { out0.data(), out1.data() };
                    engine.processBlock (ins, 2, outs, 2, kBlock);
                }
            };

            // The settings: COPY TO PROJECT by default; IMPORT and the projects folder persist.
            expect (session.getImportMode() == Session::ImportMode::Copy);
            session.setImportMode (Session::ImportMode::Ask);
            const auto elsewhere = temp.folder.getChildFile ("Other drive");
            elsewhere.createDirectory();
            session.setProjectsFolder (elsewhere);
            expect (session.getProjectsFolder() == elsewhere && session.hasCustomProjectsFolder());
            settings.saveIfNeeded();
            {
                juce::PropertiesFile reread (settingsFile, options);
                expectEquals (reread.getIntValue (Session::importModeKey), (int) Session::ImportMode::Ask);
                expectEquals (reread.getValue (Session::projectsFolderKey), elsewhere.getFullPathName());
            }
            // A chosen folder that isn't there (a drive unplugged) isn't created anew.
            session.setProjectsFolder (temp.folder.getChildFile ("Unplugged/Projects"));
            expect (session.ensureFolder().failed());
            expect (! temp.folder.getChildFile ("Unplugged").exists());
            session.setProjectsFolder (elsewhere);
            expect (session.ensureFolder().wasOk());
            expect (p.getFolder().isAChildOf (elsewhere));
            session.setProjectsFolder ({});
            expect (! session.hasCustomProjectsFolder() && session.getProjectsFolder() == temp.folder.getChildFile ("Projects"));
            session.setImportMode (Session::ImportMode::KeepInPlace);

            const auto library = temp.folder.getChildFile ("Library");
            const auto kick = writeSine (library.getChildFile ("kick.wav"), 48000.0, 0.5, 2);
            const auto pad = writeSine (library.getChildFile ("pad.wav"), 44100.0, 0.5, 1);
            session.importFiles ({ kick, pad }, 0, 0);
            wait();
            expectEquals (p.getNumTracks(), 2);
            const auto kickTrack = p.getTrackData (0).id, padTrack = p.getTrackData (1).id;
            auto kickClip = p.getClips (kickTrack)[0];
            const auto padClip = p.getClips (padTrack)[0];
            expect (p.resolve (kickClip.file) == kick, "the original is referenced");
            expect (juce::File::isAbsolutePath (kickClip.file));
            expectEquals (kickClip.length, (int64) 24000);
            expect (p.resolve (padClip.file).isAChildOf (p.getAudioFolder()), "a 44.1 kHz file gets a copy");
            expectEquals (padClip.length, (int64) 24000);
            expect (said ("pad: resampled copy: 44.1 kHz file in a 48 kHz project"));
            expect (said ("Imported 2 files (1 kept in place, 1 copied into the project)"));
            expectEquals (p.getAudioFolder().getNumberOfChildFiles (juce::File::findFiles), 1, "no duplicate of the referenced file");
            expectEquals (session.getNumReferencedFiles(), 1);
            expect (! session.isOffline (kickClip));
            expectEquals ((int) p.buildPlaylist()[0].clips.size(), 1);

            // ASK's COPY TO PROJECT copies a file that could stay.
            session.importFiles ({ kick }, 0, 48000, false);
            wait();
            expectEquals (p.getNumTracks(), 3);
            expect (p.resolve (p.getClips (p.getTrackData (2).id)[0].file).isAChildOf (p.getAudioFolder()));
            p.beginEdit ("remove");
            p.removeTrack (p.getTrackData (2).id);

            // Saving keeps the absolute path; Save As elsewhere keeps both clips playing.
            expect (session.save().wasOk());
            expect (p.getFile().loadFileAsString().contains (kick.getFullPathName()));
            const auto oldAudio = p.getAudioFolder();
            expect (session.saveAs (temp.folder.getChildFile ("Projects/Moved")).wasOk());
            expect (p.resolve (p.getClips (kickTrack)[0].file) == kick);
            expect (p.resolve (p.getClips (padTrack)[0].file).isAChildOf (oldAudio));
            expectEquals ((int) p.buildPlaylist()[0].clips.size() + (int) p.buildPlaylist()[1].clips.size(), 2);
            expectEquals (session.getNumReferencedFiles(), 2);

            // The drive goes away: the clip is offline, left out of playback, which keeps running.
            const auto unplugged = temp.folder.getChildFile ("unplugged.wav");
            expect (kick.moveFileTo (unplugged));
            session.checkOfflineFiles();
            expect (session.isOffline (p.getClips (kickTrack)[0]));
            expectEquals (session.getOfflineFiles().size(), 1);
            expect (said ("OFFLINE: " + kick.getFullPathName()));
            session.pushPlaylistNow();
            expect (p.buildPlaylist()[0].clips.empty());
            session.locate (0);
            session.play();
            run (300);
            expect (session.isRolling(), "playback goes on");
            expect (session.getPosition() > 0);
            session.stop();

            // A project with a missing file loads with the clip offline (not dropped).
            expect (session.save().wasOk());
            const auto projectFile = p.getFile();
            session.newProject();
            messages.clear();
            expect (session.openProject (projectFile).wasOk());
            expectEquals ((int) p.getClips (kickTrack).size(), 1);
            expect (p.resolve (p.getClips (kickTrack)[0].file) == kick);
            expect (session.isOffline (p.getClips (kickTrack)[0]));
            expect (said ("OFFLINE: 1 file is missing"));

            // It comes back.
            expect (unplugged.moveFileTo (kick));
            session.checkOfflineFiles();
            expect (! session.isOffline (p.getClips (kickTrack)[0]));
            expect (said ("Back online: " + kick.getFullPathName()));

            // COLLECT FILES copies both files from outside into the Audio folder and repoints the clips.
            session.collectFiles();
            wait();
            expectEquals (session.getNumReferencedFiles(), 0);
            const auto collected = p.resolve (p.getClips (kickTrack)[0].file);
            expect (collected.isAChildOf (p.getAudioFolder()) && ! juce::File::isAbsolutePath (p.getClips (kickTrack)[0].file));
            expectEquals (collected.getSize(), kick.getSize());
            expect (kick.existsAsFile(), "the original stays");
            expect (p.resolve (p.getClips (padTrack)[0].file).isAChildOf (p.getAudioFolder()));
            expect (! p.isDirty(), "saved");
            expect (said ("Collected 2 files"));
            // Undo points the clips back to the originals.
            p.undoLast();
            expect (p.resolve (p.getClips (kickTrack)[0].file) == kick);
        }
    }

    void testFormats()
    {
        beginTest ("Audio file formats: names, settings text, what MP3 can record");
        {
            expectEquals (AudioFileFormat::wav (32).describe(), juce::String ("WAV 32-BIT FLOAT"));
            expectEquals (AudioFileFormat::flac (20).normalized().bits, 24);
            expectEquals (AudioFileFormat::mp3 (300).normalized().kbps, 320);
            expectEquals (AudioFileFormat::aiff (24).extension(), juce::String (".aif"));
            expectEquals (AudioFileFormat::mp3 (192).shortName(), juce::String ("MP3 192"));
            for (const auto& f : AudioFileFormat::exportChoices())
                expect (AudioFileFormat::fromString (f.toString()) == f, f.toString());
            expect (AudioFileFormat::fromString ("nonsense", AudioFileFormat::flac (16)) == AudioFileFormat::flac (16));
            expect (AudioFileFormat::mp3().check (2, 96000.0).isNotEmpty());
            expect (AudioFileFormat::mp3().check (2, 44100.0).isEmpty());
            expect (AudioFileFormat::flac().check (2, 96000.0).isEmpty());
            expect (AudioFileFormat::isAudioFile (juce::File ("/a/b.AIFF")) && ! AudioFileFormat::isAudioFile (juce::File ("/a/b.ogg")));
            expect (AudioFileFormat::typeOfFile (juce::File ("/a/b.flac")) == AudioFileFormat::Type::Flac);
        }

        beginTest ("Takes record as FLAC, AIFF or MP3; export renders every format");
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
            int64 clock = 0;
            std::vector<float> in0 (kBlock), in1 (kBlock), out0 (kBlock), out1 (kBlock);
            auto tone = [] (int64 t) { return 0.5f * (float) std::sin ((double) t * juce::MathConstants<double>::twoPi * 441.0 / 48000.0); };
            auto run = [&] (int blocks, bool signal)
            {
                for (int b = 0; b < blocks; ++b)
                {
                    recorder.waitForDisk();
                    for (int i = 0; i < kBlock; ++i)
                        in0[(size_t) i] = signal ? tone (clock + i) : 0.0f;
                    std::fill (in1.begin(), in1.end(), 0.0f);
                    const float* ins[] = { in0.data(), in1.data() };
                    float* outs[] = { out0.data(), out1.data() };
                    engine.processBlock (ins, 2, outs, 2, kBlock);
                    clock += kBlock;
                }
            };
            TrackData td;
            td.name = "Gtr";
            td.source.kind = Recorder::Source::Kind::HardwareInput;
            td.source.port = 0;
            const auto t = p.addTrack (td);
            p.setTrackProperty (t, ids::arm, true, false);
            for (auto format : { AudioFileFormat::flac (24), AudioFileFormat::aiff (24), AudioFileFormat::mp3 (256) })
            {
                session.setRecordFormat (format);
                expect (recorder.getRecordFormat() == format);
                session.locate (p.getLength() + 48000);
                const auto before = p.getClips (t).size();
                const auto result = session.record();
                expect (result.wasOk(), result.getErrorMessage());
                run (375, true);
                session.stop();
                run (8, false);
                for (int i = 0; i < 500 && p.getClips (t).size() == before; ++i)
                {
                    recorder.waitForDisk();
                    session.collectTakes();
                    juce::Thread::sleep (2);
                }
                const auto clips = p.getClips (t);
                expectEquals ((int) clips.size(), (int) before + 1);
                if (clips.size() != before + 1)
                    continue;
                const auto& clip = clips.back();
                const auto file = p.resolve (clip.file);
                expectEquals (file.getFileExtension(), format.extension(), "takes carry the format's extension");
                // The project's readers (waveforms, export) read it at the right level, sample-aligned.
                std::unique_ptr<juce::AudioFormatReader> reader (session.getFormats().createReaderFor (file));
                expect (reader != nullptr);
                if (reader == nullptr)
                    continue;
                expectEquals ((int64) reader->lengthInSamples, clip.length);
                juce::AudioBuffer<float> b (1, 24000);
                reader->read (&b, 0, 24000, 12000, true, false);
                const bool lossy = format.type == AudioFileFormat::Type::Mp3;
                expectWithinAbsoluteError (b.getMagnitude (0, 0, 24000), 0.5f, lossy ? 0.03f : 0.001f);
            }
            expect (session.getRecordFormat() == AudioFileFormat::mp3 (256));

            // Export the track in each format; every file reads back with JUCE.
            juce::AudioFormatManager juceFormats;
            juceFormats.registerBasicFormats();
            const auto length = p.getLength();
            std::vector<RenderClip> clips;
            for (auto& c : p.getClips (t))
                clips.push_back ({ p.resolve (c.file), c });
            std::atomic<bool> cancel { false };
            for (const auto& format : AudioFileFormat::exportChoices())
            {
                const auto dest = temp.folder.getChildFile ("Export/01 Gtr" + format.extension());
                const auto r = renderTrack (session.getFormats(), clips, 0.0f, 1, 48000.0, length, dest, cancel, {}, format);
                expect (r.wasOk(), format.describe() + ": " + r.getErrorMessage());
                std::unique_ptr<juce::AudioFormatReader> out (juceFormats.createReaderFor (dest));
                expect (out != nullptr, format.describe());
                if (out == nullptr)
                    continue;
                const bool lossy = format.type == AudioFileFormat::Type::Mp3;
                if (! lossy)
                    expectEquals ((int64) out->lengthInSamples, length);
                juce::AudioBuffer<float> o (1, (int) out->lengthInSamples);
                out->read (&o, 0, o.getNumSamples(), 0, true, false);
                expectWithinAbsoluteError (o.getMagnitude (0, 0, o.getNumSamples()), 0.5f, lossy ? 0.05f : 0.002f, format.describe());
                dest.deleteFile();
            }
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

    void testDefaultFolders()
    {
        beginTest ("Default folders: an unknown Documents isn't the drive's root; a failed save says where to choose another (#12)");
        {
            const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
            expect (paths::firstKnown ({ juce::File(), home }) == home);
            expect (paths::firstKnown ({ juce::File() }) == juce::File::getSpecialLocation (juce::File::tempDirectory));
            for (const auto& f : { paths::documents(), paths::appData(), Session::getDefaultFolder() })
                expect (f.getParentDirectory() != f.getParentDirectory().getParentDirectory(), f.getFullPathName());

            TempFolder temp;
            MixerEngine engine (0);
            engine.prepareOffline (48000.0, 128, 2, 2);
            Session session (engine.getRecorder(), nullptr);
            // A projects folder that can't be created (here: under a file).
            const auto blocker = temp.folder.getChildFile ("Not a folder");
            blocker.replaceWithText ("x");
            session.setDefaultFolder (blocker.getChildFile ("Recordings"));
            const auto r = session.ensureFolder();
            expect (r.failed());
            expect (r.getErrorMessage().contains ("FILE > Projects folder"), r.getErrorMessage());
            expect (session.createProject ("Gig").getErrorMessage().contains ("FILE > Projects folder"));
        }
    }

    void testPlayModesAndAutoInput()
    {
        beginTest ("MODE A->B stops at the range's end; AUTO INPUT SELECT follows the transport");
        {
            TempFolder temp;
            MixerEngine engine (0);
            constexpr int kBlock = 128;
            engine.prepareOffline (48000.0, kBlock, 2, 2);
            auto& recorder = engine.getRecorder();
            juce::PropertiesFile::Options options;
            juce::PropertiesFile settings (temp.folder.getChildFile ("test.settings"), options);
            Session session (recorder, &settings);
            session.setDefaultFolder (temp.folder);
            auto& p = session.getProject();
            std::vector<Session::TransportMode> modes;
            session.onTransportModeChanged = [&modes] (Session::TransportMode m) { modes.push_back (m); };

            std::vector<float> in0 (kBlock), out0 (kBlock), out1 (kBlock);
            auto run = [&] (int blocks)
            {
                for (int b = 0; b < blocks; ++b)
                {
                    recorder.waitForDisk();
                    const float* ins[] = { in0.data(), in0.data() };
                    float* outs[] = { out0.data(), out1.data() };
                    engine.processBlock (ins, 2, outs, 2, kBlock);
                }
            };

            // A new project plays on (PLAY); the MODE follows into the settings for the WING remote.
            expect (session.getPlayMode() == Session::PlayMode::Play);
            p.setLoop (4800, 9600);
            session.setPlayMode (Session::PlayMode::AToB);
            expect (p.isAToBOn() && ! p.isLoopOn());
            expectEquals (settings.getIntValue (Session::playModeKey), 1);

            // A->B plays from A and stops exactly at B.
            session.locate (0);
            session.play();
            expect (modes.size() == 1 && modes[0] == Session::TransportMode::Play);
            for (int i = 0; i < 400 && session.getState() != Recorder::State::Stopped; ++i)
                run (1);
            expect (session.getState() == Recorder::State::Stopped);
            expectEquals ((int) session.getPosition(), 9600);

            // LOOP (the editor's loop key too) clears A->B.
            session.setLoopEnabled (true);
            expect (session.getPlayMode() == Session::PlayMode::Loop && ! p.isAToBOn());
            expectEquals (settings.getIntValue (Session::playModeKey), 2);
            session.setPlayMode (Session::PlayMode::Play);
            session.play();
            session.stop();
            expect (modes.back() == Session::TransportMode::Stop);

            // AUTO INPUT SELECT: OFF does nothing; then STOP -> MAIN, PLAY -> ALT, RECORD keeps.
            using TM = Session::TransportMode;
            expect (! Session::autoInputFor (&settings, TM::Play).has_value());
            settings.setValue (Session::autoInputModeKeys[0], 1);
            settings.setValue (Session::autoInputModeKeys[1], 2);
            settings.setValue (Session::autoInputModeKeys[2], juce::var (0));
            expect (! Session::autoInputFor (&settings, TM::Play).has_value(), "AUTO INPUT SELECT is OFF");
            settings.setValue (Session::autoInputKey, 1);
            expect (Session::autoInputFor (&settings, TM::Play) == std::optional<bool> (true));
            expect (Session::autoInputFor (&settings, TM::Stop) == std::optional<bool> (false));
            expect (! Session::autoInputFor (&settings, TM::Record).has_value(), "KEEP");
            expect (! Session::autoInputFor (nullptr, TM::Play).has_value());

            // It switches the GLOBAL channels routed to or from the recorder only:
            // CH 1 plays a track on ALT (GLOBAL), CH 2 too but INDIVIDUAL, CH 3
            // is GLOBAL but has nothing to do with the recorder.
            test::setChannels (engine, 3);
            auto& chans = engine.getChannels();
            for (int i = 0; i < 3; ++i)
            {
                chans[(size_t) i]->params.inputLeft = 10 + i;
                chans[(size_t) i]->setAltInput (i < 2 ? Recorder::playbackSource (i) : 20);
                chans[(size_t) i]->setSrcAuto (i != 1);
            }
            expectEquals (session.applyAutoInput (engine, TM::Play), 1);
            expect (chans[0]->isUsingAltInput() && ! chans[1]->isAltSelected() && ! chans[2]->isAltSelected());
            expectEquals (session.applyAutoInput (engine, TM::Record), 0, "KEEP");
            expectEquals (session.applyAutoInput (engine, TM::Stop), 1);
            expect (! chans[0]->isAltSelected());
            settings.setValue (Session::autoInputKey, juce::var (0));
            chans[0]->setAltSelected (true);
            expectEquals (session.applyAutoInput (engine, TM::Stop), 0, "AUTO INPUT SELECT OFF");
        }
    }
};

static DawTests dawTests;
