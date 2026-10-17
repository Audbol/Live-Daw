//! Transport tests: the recorder driven like the engine drives it, chunk by
//! chunk, with the real disk threads.

use livedaw_recorder::wav::{WavReader, WavWriter};
use livedaw_recorder::*;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

const BLOCK: usize = 512;
const WAIT: Duration = Duration::from_secs(10);

fn temp_dir(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("livedaw-transport-{}-{}", name, std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

/// A mono file whose sample k is k (exact in f32 up to 2^24).
fn ramp_file(dir: &Path, frames: usize) -> PathBuf {
    let path = dir.join("ramp.wav");
    let ramp: Vec<f32> = (0..frames).map(|k| k as f32).collect();
    let mut w = WavWriter::create(&path, 1, 48000).unwrap();
    w.write_planar(&[&ramp], frames).unwrap();
    w.finalize().unwrap();
    path
}

/// One rolling chunk as the audio thread saw it.
#[derive(Debug)]
struct Chunk {
    state: TransportState,
    pos: i64,
    samples: Vec<f32>,
}

struct Driver {
    rec: Recorder,
    chunks: Vec<Chunk>,
}

impl Driver {
    fn new() -> Self {
        let rec = Recorder::new();
        unsafe { rec.prepare(48000.0, BLOCK) };
        Self { rec, chunks: Vec::new() }
    }

    /// One audio callback of `n` samples; playback channel `ch` is captured,
    /// `record` gives the value of each recorded sample by callback offset.
    fn callback(&mut self, n: usize, ch: usize, record: &dyn Fn(usize) -> f32) {
        let mut done = 0;
        while done < n {
            let m = unsafe { self.rec.begin_chunk(n - done) };
            let (state, pos) = unsafe { self.rec.chunk_state() };
            let samples = unsafe { std::slice::from_raw_parts(self.rec.playback_ptrs()[ch], m) }.to_vec();
            let input: Vec<f32> = (0..m).map(|i| record(done + i)).collect();
            let streams = unsafe { self.rec.record_streams() }.len();
            for s in 0..streams {
                unsafe { self.rec.write_record_stream(s, Some(&input), m) };
            }
            unsafe { self.rec.end_chunk(m) };
            self.chunks.push(Chunk { state, pos, samples });
            done += m;
        }
    }

    /// Callbacks with the disk threads kept ahead (tests run faster than real time).
    fn run(&mut self, callbacks: usize, ch: usize) {
        for _ in 0..callbacks {
            assert!(self.rec.wait_for_disk(WAIT));
            self.callback(BLOCK, ch, &|_| 0.0);
        }
    }

    /// Runs until the transport rolls.
    fn roll(&mut self, ch: usize) {
        for _ in 0..1000 {
            self.run(1, ch);
            if self.rec.state().is_rolling() {
                return;
            }
        }
        panic!("the transport never rolled");
    }
}

fn rolling(chunks: &[Chunk]) -> impl Iterator<Item = &Chunk> {
    chunks.iter().filter(|c| c.state.is_rolling())
}

#[test]
fn play_and_locate_are_sample_accurate() {
    let dir = temp_dir("locate");
    let file = ramp_file(&dir, 200_000);
    let mut d = Driver::new();
    // The file's sample k plays at timeline 1000 + k on playback channel 5.
    let mut clip = Clip::new(&file, 1000, 150_000);
    clip.file_offset = 0;
    d.rec.set_playlist(&Playlist { tracks: vec![PlaybackTrack::new(5, 1, vec![clip])] });
    d.rec.locate(48_000);
    assert_eq!(d.rec.position(), 48_000, "a locate shows at once");
    d.rec.play();
    assert_eq!(d.rec.state(), TransportState::Starting);
    d.roll(5);
    assert_eq!(d.chunks.iter().filter(|c| c.state == TransportState::Starting).count(), 1);
    assert!(
        d.chunks.iter().filter(|c| !c.state.is_rolling()).all(|c| c.samples.iter().all(|&x| x == 0.0)),
        "silent while starting"
    );
    d.run(40, 5);

    let check = |chunks: &[Chunk], from: i64| {
        let mut expected_pos = from;
        for (n, c) in rolling(chunks).enumerate() {
            assert_eq!(c.pos, expected_pos, "chunks are contiguous");
            for (i, &x) in c.samples.iter().enumerate() {
                let t = c.pos + i as i64;
                let value = (t - 1000) as f32;
                if n == 0 && i < DECLICK_SAMPLES {
                    assert!(x.abs() <= value.abs() && x >= 0.0, "fade in at {t}");
                } else {
                    assert_eq!(x, value, "timeline {t}");
                }
            }
            expected_pos += c.samples.len() as i64;
        }
        expected_pos
    };
    let end = check(&d.chunks, 48_000);
    assert_eq!(d.rec.position(), end);

    // Locate while rolling: a fade out, a refill (Starting), then on from there.
    d.chunks.clear();
    d.rec.locate(100_000);
    d.run(1, 5);
    assert!(d.chunks[0].samples.last().copied() == Some(0.0), "faded out before moving");
    assert_eq!(d.chunks[0].state, TransportState::Playing);
    d.chunks.clear();
    d.roll(5);
    d.run(20, 5);
    check(&d.chunks, 100_000);
    assert_eq!(d.rec.dropouts(), 0);

    d.rec.stop();
    d.run(2, 5);
    assert_eq!(d.rec.state(), TransportState::Stopped);
    let stopped = d.rec.position();
    d.chunks.clear();
    d.run(3, 5);
    assert!(d.chunks.iter().all(|c| c.state == TransportState::Stopped && c.samples.iter().all(|&x| x == 0.0)));
    assert_eq!(d.rec.position(), stopped, "stopped: the position stays");
    drop(d);
    let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn the_loop_wraps_on_a_chunk_boundary() {
    let dir = temp_dir("loop");
    let file = ramp_file(&dir, 100_000);
    let mut d = Driver::new();
    d.rec.set_playlist(&Playlist { tracks: vec![PlaybackTrack::new(0, 1, vec![Clip::new(&file, 0, 100_000)])] });
    // A loop that doesn't fit whole blocks: 3000 samples from 10 000.
    d.rec.set_loop(10_000, 13_000, true);
    d.rec.locate(9_100);
    d.rec.play();
    d.roll(0);
    d.run(30, 0); // ~5 passes through the loop

    let mut expected = 9_100i64;
    let mut wraps = 0;
    for (n, c) in rolling(&d.chunks).enumerate() {
        assert_eq!(c.pos, expected);
        assert!(c.pos >= 13_000 || c.pos + c.samples.len() as i64 <= 13_000, "a chunk never crosses the loop end");
        for (i, &x) in c.samples.iter().enumerate() {
            if n > 0 || i >= DECLICK_SAMPLES {
                assert_eq!(x, (c.pos + i as i64) as f32);
            }
        }
        expected = c.pos + c.samples.len() as i64;
        if expected == 13_000 {
            expected = 10_000;
            wraps += 1;
        }
    }
    assert!(wraps >= 4, "{wraps} wraps");
    assert_eq!(d.rec.position(), expected);

    // Changing the loop while rolling keeps playing (a crossfade, no refill).
    d.chunks.clear();
    d.rec.set_loop(20_000, 30_000, true);
    d.run(30, 0);
    assert!(d.chunks.iter().all(|c| c.state.is_rolling()), "no stall");
    d.rec.set_loop(0, 0, false);
    d.run(10, 0);
    assert!(d.chunks.iter().all(|c| c.state.is_rolling()));
    assert_eq!(d.rec.dropouts(), 0);
    drop(d);
    let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn a_playlist_change_while_rolling_crossfades_in_place() {
    let dir = temp_dir("switch");
    let file = ramp_file(&dir, 400_000);
    let mut d = Driver::new();
    let track = |gain: f32| {
        let mut clip = Clip::new(&file, 0, 400_000);
        clip.gain = gain;
        Playlist { tracks: vec![PlaybackTrack::new(2, 1, vec![clip])] }
    };
    d.rec.set_playlist(&track(1.0));
    d.rec.play();
    d.roll(2);
    d.run(20, 2);
    let before = d.chunks.len();

    d.rec.set_playlist(&track(0.5));
    d.run(60, 2);

    // Every sample is the ramp at its timeline position times a gain that
    // goes from 1 to 0.5 once, smoothly, within one crossfade.
    let mut expected = rolling(&d.chunks).next().unwrap().pos;
    let mut gains = Vec::new();
    for (n, c) in rolling(&d.chunks).enumerate() {
        assert_eq!(c.pos, expected, "the transport never stopped");
        for (i, &x) in c.samples.iter().enumerate() {
            let t = c.pos + i as i64;
            if (n > 0 || i >= DECLICK_SAMPLES) && t > 0 {
                gains.push(x / t as f32);
            }
        }
        expected += c.samples.len() as i64;
    }
    assert!(d.chunks[before..].iter().all(|c| c.state == TransportState::Playing), "no refill, no stall");
    let first_change = gains.iter().position(|&g| (g - 1.0).abs() > 1e-6).expect("the new playlist plays");
    let settled = gains.iter().position(|&g| (g - 0.5).abs() < 1e-6).expect("fully switched");
    assert!(settled - first_change <= CROSSFADE_SAMPLES, "one crossfade: {first_change}..{settled}");
    for w in gains.windows(2) {
        assert!(w[1] <= w[0] + 1e-6 && (w[1] - w[0]).abs() < 2.0 / CROSSFADE_SAMPLES as f32, "no jump");
    }
    assert!(gains[settled..].iter().all(|&g| (g - 0.5).abs() < 1e-6));
    assert_eq!(d.rec.dropouts(), 0);
    drop(d);
    let _ = std::fs::remove_dir_all(dir);
}

fn wait_for_takes(rec: &Recorder, n: usize) -> Vec<Take> {
    let deadline = Instant::now() + WAIT;
    let mut takes = Vec::new();
    while takes.len() < n && Instant::now() < deadline {
        match rec.pop_finished_take() {
            Some(t) => takes.push(t),
            None => std::thread::sleep(Duration::from_millis(2)),
        }
    }
    takes
}

#[test]
fn recording_is_sample_accurate_and_survives_rf64() {
    let dir = temp_dir("record");
    let mut d = Driver::new();
    d.rec.set_rf64_threshold(4096);
    let file = dir.join("take.wav");
    let stereo = dir.join("stereo.wav");
    let source = RecordSource { kind: SourceKind::HardwareInput, port: 0, strip_kind: 0, strip_number: 0 };
    d.rec.locate(7_000);
    d.rec
        .record(&[
            RecordTrackSpec { track: 3, source, num_channels: 1, file: file.clone() },
            RecordTrackSpec { track: 4, source, num_channels: 2, file: stereo.clone() },
        ])
        .unwrap();
    assert!(d.rec.record(&[RecordTrackSpec { track: 1, source, num_channels: 3, file: dir.join("x.wav") }]).is_err());
    assert!(file.exists(), "files are created by record()");

    // Each recorded sample is a counter of the samples fed to the recorder.
    let mut counter = 0usize;
    let mut first_recorded = None;
    for _ in 0..200 {
        assert!(d.rec.wait_for_disk(WAIT));
        let base = counter;
        let before = d.chunks.len();
        d.callback(BLOCK, 0, &|i| (base + i) as f32);
        for (k, c) in d.chunks[before..].iter().enumerate() {
            if c.state == TransportState::Recording && first_recorded.is_none() {
                let offset: usize = d.chunks[before..before + k].iter().map(|c| c.samples.len()).sum();
                first_recorded = Some((c.pos, base + offset));
            }
        }
        counter += BLOCK;
        if d.rec.recorded_length() > 20_000 {
            break;
        }
    }
    let (start_pos, first_value) = first_recorded.expect("recording started");
    assert_eq!(start_pos, 7_000);
    assert_eq!(d.rec.state(), TransportState::Recording);
    assert!(d.rec.record_level(3, 0) > 0.0);
    let mut peaks = vec![0.0f32; 64];
    assert!(d.rec.wait_for_disk(WAIT));
    let n = d.rec.recording_peaks(4, 2, 16, &mut peaks);
    assert!(n == 16, "peaks while recording");
    let peak2 = (first_value + 2 * SAMPLES_PER_PEAK) as f32;
    assert_eq!(&peaks[..4], &[peak2, peak2 + 255.0, peak2, peak2 + 255.0], "min/max per channel");

    // The take ends with the chunk that sees the stop.
    d.rec.stop();
    let base = counter;
    d.callback(BLOCK, 0, &|i| (base + i) as f32);
    assert_eq!(d.rec.state(), TransportState::Stopped);
    let recorded = d.rec.position() - start_pos;
    let takes = wait_for_takes(&d.rec, 2);
    assert_eq!(takes.len(), 2);
    assert_eq!(d.rec.recorded_length(), 0);
    for t in &takes {
        assert_eq!(t.start, 7_000);
        assert_eq!(t.length, recorded);
        let mut r = WavReader::open(&t.file).unwrap();
        assert!(r.info().rf64, "past the (test) threshold: RF64");
        assert_eq!((r.info().channels, r.info().frames as i64), (t.num_channels, t.length));
        let mut l = vec![0.0f32; t.length as usize];
        let mut rr = vec![0.0f32; t.length as usize];
        r.read(0, &mut [&mut l[..], &mut rr[..]]).unwrap();
        for k in 0..t.length as usize {
            assert_eq!(l[k], (first_value + k) as f32, "take sample {k} = timeline {}", t.start + k as i64);
            assert_eq!(rr[k], l[k]);
        }
    }
    assert_eq!((takes[0].track, takes[1].track), (3, 4));
    assert_eq!(d.rec.dropouts(), 0);

    // A recording stopped before it rolls leaves no take and no file.
    let never = dir.join("never.wav");
    d.rec.record(&[RecordTrackSpec { track: 9, source, num_channels: 1, file: never.clone() }]).unwrap();
    d.rec.stop();
    d.run(3, 0);
    assert!(d.rec.wait_for_disk(WAIT));
    assert!(d.rec.pop_finished_take().is_none());
    assert!(!never.exists());
    drop(d);
    let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn dropping_the_recorder_finalizes_the_take() {
    let dir = temp_dir("drop");
    let file = dir.join("take.wav");
    let mut d = Driver::new();
    let source = RecordSource::default();
    d.rec.record(&[RecordTrackSpec { track: 0, source, num_channels: 1, file: file.clone() }]).unwrap();
    for _ in 0..50 {
        assert!(d.rec.wait_for_disk(WAIT));
        d.callback(BLOCK, 0, &|_| 0.25);
    }
    let length = d.rec.recorded_length();
    assert!(length > 0);
    drop(d); // like quitting the app while recording
    let r = WavReader::open(&file).unwrap();
    assert_eq!(r.info().frames as i64, length);
    let _ = std::fs::remove_dir_all(dir);
}
