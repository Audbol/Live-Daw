//! The recorder inside the engine: playback channels as input sources, ALT
//! inputs, record taps, sample accuracy end to end.

use livedaw_engine::params::StripKind;
use livedaw_engine::recorder::wav::{WavReader, WavWriter};
use livedaw_engine::recorder::{
    Clip, PlaybackTrack, Playlist, RecordSource, RecordTrackSpec, SourceKind, Take, TransportState, DECLICK_SAMPLES,
};
use livedaw_engine::sources::OSC_SOURCE_BASE;
use livedaw_engine::{Engine, PLAYBACK_SOURCE_BASE, USB_PLAYER_SOURCE_BASE};
use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering::Relaxed;
use std::sync::Arc;
use std::time::{Duration, Instant};

const BLOCK: usize = 128;
const WAIT: Duration = Duration::from_secs(10);

fn temp_dir(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("livedaw-engine-rec-{}-{}", name, std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

fn write_wav(path: &Path, samples: &[f32]) {
    let mut w = WavWriter::create(path, 1, 48000).unwrap();
    w.write_planar(&[samples], samples.len()).unwrap();
    w.finalize().unwrap();
}

fn read_wav(path: &Path) -> Vec<Vec<f32>> {
    let mut r = WavReader::open(path).unwrap();
    let info = *r.info();
    let mut chans = vec![vec![0.0; info.frames as usize]; info.channels];
    let mut refs: Vec<&mut [f32]> = chans.iter_mut().map(|c| &mut c[..]).collect();
    r.read(0, &mut refs).unwrap();
    chans
}

/// An engine driven block by block (no workers), with the disk threads kept
/// ahead of it (the test runs faster than real time).
struct Rig {
    engine: Arc<Engine>,
    inputs: Vec<Vec<f32>>,
    outputs: Vec<Vec<f32>>,
    /// Samples fed so far.
    clock: usize,
}

impl Rig {
    fn new(num_ins: usize, num_outs: usize) -> Self {
        let engine = Arc::new(Engine::new(0));
        let in_map: Vec<i32> = (0..num_ins as i32).collect();
        let out_map: Vec<i32> = (0..num_outs as i32).collect();
        unsafe { engine.prepare(48000.0, BLOCK, &in_map, &out_map) };
        Self { engine, inputs: vec![vec![0.0; BLOCK]; num_ins], outputs: vec![vec![0.0; BLOCK]; num_outs], clock: 0 }
    }

    /// One block; `input(ch, t)` gives input `ch` at sample `t` of the clock.
    fn block(&mut self, input: &dyn Fn(usize, usize) -> f32) {
        assert!(self.engine.recorder().wait_for_disk(WAIT));
        for (ch, buf) in self.inputs.iter_mut().enumerate() {
            for (i, x) in buf.iter_mut().enumerate() {
                *x = input(ch, self.clock + i);
            }
        }
        let ins: Vec<*const f32> = self.inputs.iter().map(|v| v.as_ptr()).collect();
        let outs: Vec<*mut f32> = self.outputs.iter_mut().map(|v| v.as_mut_ptr()).collect();
        unsafe { self.engine.process(&ins, &outs, BLOCK) };
        self.clock += BLOCK;
    }

    fn run(&mut self, blocks: usize) {
        for _ in 0..blocks {
            self.block(&|_, _| 0.0);
        }
    }

    fn state(&self) -> TransportState {
        self.engine.recorder().state()
    }
}

fn wait_for_takes(rig: &Rig, n: usize) -> Vec<Take> {
    let deadline = Instant::now() + WAIT;
    let mut takes = Vec::new();
    while takes.len() < n && Instant::now() < deadline {
        match rig.engine.recorder().pop_finished_take() {
            Some(t) => takes.push(t),
            None => std::thread::sleep(Duration::from_millis(2)),
        }
    }
    takes
}

fn input_source(port: i32) -> RecordSource {
    RecordSource { kind: SourceKind::HardwareInput, port, ..Default::default() }
}

fn strip_source(kind: SourceKind, strip: StripKind, number: i32) -> RecordSource {
    RecordSource { kind, port: 0, strip_kind: strip as i32, strip_number: number }
}

#[test]
fn a_recorded_hardware_input_matches_at_the_reported_start() {
    let dir = temp_dir("hw");
    let mut rig = Rig::new(4, 2);
    let file = dir.join("in2.wav");
    rig.engine.recorder().locate(12_345);
    rig.engine.record(&[RecordTrackSpec { track: 0, source: input_source(1), num_channels: 1, file: file.clone() }]).unwrap();

    // Input 2 carries the sample clock; every sample is distinct.
    let clock = |ch: usize, t: usize| if ch == 1 { t as f32 } else { -1.0 };
    let mut first = None;
    for _ in 0..1000 {
        let before = rig.clock;
        let was = rig.state();
        rig.block(&clock);
        if was != TransportState::Recording && rig.state() == TransportState::Recording {
            // This block started the take: its first sample is clock `before`.
            first = Some((rig.engine.recorder().position() - BLOCK as i64, before));
        }
        if first.is_some() && rig.clock - first.unwrap().1 > 30_000 {
            break;
        }
    }
    let (start, first_clock) = first.expect("recording started");
    assert_eq!(start, 12_345, "recording starts at the position");
    rig.engine.recorder().stop();
    rig.block(&clock); // the chunk that sees the stop still records
    let end_clock = rig.clock;
    let takes = wait_for_takes(&rig, 1);
    assert_eq!(takes.len(), 1);
    let take = &takes[0];
    assert_eq!((take.start, take.length), (12_345, (end_clock - first_clock) as i64));
    let samples = &read_wav(&take.file)[0];
    assert_eq!(samples.len() as i64, take.length);
    for (k, &x) in samples.iter().enumerate() {
        assert_eq!(x, (first_clock + k) as f32, "sample {k} (timeline {})", take.start + k as i64);
    }
    assert_eq!(rig.engine.recorder().dropouts(), 0);

    // Missing sources are refused before any file is made.
    let bad = dir.join("bad.wav");
    assert!(rig
        .engine
        .record(&[RecordTrackSpec { track: 0, source: input_source(9), num_channels: 1, file: bad.clone() }])
        .is_err());
    let missing_strip = strip_source(SourceKind::StripPreFader, StripKind::Channel, 3);
    assert!(rig
        .engine
        .record(&[RecordTrackSpec { track: 0, source: missing_strip, num_channels: 1, file: bad.clone() }])
        .is_err());
    assert!(!bad.exists());
    let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn tracks_record_usb_player_and_oscillator_sources() {
    // RECORD ALL CHANNELS gives a channel patched to the USB PLAYER or an
    // oscillator a track on its source port: they always exist, whatever the
    // device has.
    let dir = temp_dir("usb-player");
    let rig = Rig::new(2, 2);
    for (i, port) in [USB_PLAYER_SOURCE_BASE, OSC_SOURCE_BASE].into_iter().enumerate() {
        let file = dir.join(format!("source{i}.wav"));
        let source = input_source(port);
        rig.engine.record(&[RecordTrackSpec { track: 0, source, num_channels: 2, file }]).unwrap();
        rig.engine.recorder().stop();
    }
    let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn a_playback_impulse_through_an_alt_input_records_at_its_timeline_position() {
    let dir = temp_dir("impulse");
    let mut rig = Rig::new(2, 4);
    rig.engine.set_num_channels(2);
    const T: usize = 20_000 + 37;
    const N: usize = 7; // playback channel
    let mut impulse = vec![0.0f32; 40_000];
    impulse[T - 1000] = 1.0;
    let clip_file = dir.join("impulse.wav");
    write_wav(&clip_file, &impulse);
    let mut clip = Clip::new(&clip_file, 1000, impulse.len() as i64); // file sample k plays at 1000 + k
    clip.file_offset = 0;
    rig.engine.recorder().set_playlist(&Playlist { tracks: vec![PlaybackTrack::new(N, 1, vec![clip])] });

    // CH 1 listens to playback channel N on its ALT input (virtual soundcheck);
    // CH 2 takes it as its main input.
    let ch1 = rig.engine.channel_at(0).unwrap();
    ch1.params.alt_input_left.store(PLAYBACK_SOURCE_BASE + N as i32, Relaxed);
    ch1.params.alt_selected.store(true, Relaxed);
    let ch2 = rig.engine.channel_at(1).unwrap();
    ch2.params.input_left.store(PLAYBACK_SOURCE_BASE + N as i32, Relaxed);

    let post = dir.join("ch1-post.wav");
    let pre = dir.join("ch2-pre.wav");
    rig.engine
        .record(&[
            RecordTrackSpec {
                track: 1,
                source: strip_source(SourceKind::StripPostFader, StripKind::Channel, 1),
                num_channels: 1,
                file: post.clone(),
            },
            RecordTrackSpec {
                track: 2,
                source: strip_source(SourceKind::StripPreFader, StripKind::Channel, 2),
                num_channels: 2,
                file: pre.clone(),
            },
        ])
        .unwrap();
    while rig.clock < 48_000 {
        rig.block(&|_, _| 0.0);
    }
    rig.engine.recorder().stop();
    rig.run(2);
    let takes = wait_for_takes(&rig, 2);
    assert_eq!(takes.len(), 2);
    assert!(takes.iter().all(|t| t.start == 0));

    let argmax = |x: &[f32]| {
        x.iter().enumerate().fold((0, 0.0f32), |(bi, bv), (i, &v)| if v.abs() > bv.abs() { (i, v) } else { (bi, bv) })
    };
    let post_samples = &read_wav(&post)[0];
    let (k, v) = argmax(post_samples);
    assert_eq!(takes[0].start + k as i64, T as i64, "the impulse lands at T");
    assert!((v - std::f32::consts::FRAC_1_SQRT_2).abs() < 1e-3, "post-fader: pan law ({v})");
    assert!(post_samples.iter().enumerate().all(|(i, &x)| i == k || x.abs() < 1e-3));

    let pre_samples = read_wav(&pre);
    assert_eq!(pre_samples.len(), 2);
    let (k, v) = argmax(&pre_samples[0]);
    assert_eq!(k as i64, T as i64);
    assert!((v - 1.0).abs() < 1e-3, "pre-fader: unity ({v})");
    assert_eq!(pre_samples[1][k], pre_samples[0][k], "a mono strip's tap feeds both channels");

    assert!(ch1.uses_alt_input(false, false) && !ch2.uses_alt_input(false, false));
    let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn alt_select_individual_or_global() {
    let mut rig = Rig::new(3, 6);
    rig.engine.set_num_channels(2);
    let levels = [0.1f32, 0.2, 0.4];
    let (a, b) = (rig.engine.channel_at(0).unwrap(), rig.engine.channel_at(1).unwrap());
    let aux = rig.engine.strip_at(StripKind::Aux, 0).unwrap();
    // Direct outs (pre-fader) on outputs 3, 4, 5; A: main 1 / ALT 3, B: main 2
    // and no ALT, AUX 1: main 2 / ALT 1.
    for (s, out) in [(&a, 3), (&b, 4), (&aux, 5)] {
        s.params.output_left.store(out, Relaxed);
    }
    a.params.alt_input_left.store(2, Relaxed);
    aux.params.input_left.store(1, Relaxed);
    aux.params.alt_input_left.store(0, Relaxed);

    let engine = rig.engine.clone();
    let c = &engine.controls;
    let check = |rig: &mut Rig, expected: [f32; 3], what: &str| {
        for _ in 0..4 {
            rig.block(&|ch, _| levels[ch]);
        }
        let got = [3, 4, 5].map(|o| *rig.outputs[o].last().unwrap());
        for (g, e) in got.iter().zip(expected) {
            assert!((g - e).abs() < 1e-4, "{what}: {got:?} != {expected:?}");
        }
    };
    check(&mut rig, [0.1, 0.2, 0.2], "MAIN everywhere");
    a.params.alt_selected.store(true, Relaxed);
    b.params.alt_selected.store(true, Relaxed);
    check(&mut rig, [0.4, 0.2, 0.2], "individual: A on ALT; B has no ALT source");
    c.input_select_global.store(true, Relaxed);
    check(&mut rig, [0.1, 0.2, 0.2], "global MAIN overrides the strips' switches");
    c.global_alt.store(true, Relaxed);
    check(&mut rig, [0.4, 0.2, 0.1], "global ALT: every strip with an ALT source");
    a.params.alt_selected.store(false, Relaxed);
    check(&mut rig, [0.4, 0.2, 0.1], "still global");
    c.input_select_global.store(false, Relaxed);
    check(&mut rig, [0.1, 0.2, 0.2], "back to individual");

    assert!(!a.uses_alt_input(false, true) && a.uses_alt_input(true, true) && !a.uses_alt_input(true, false));
    let bus = rig.engine.strip_at(StripKind::Bus, 0).unwrap();
    bus.params.alt_input_left.store(1, Relaxed);
    bus.params.alt_selected.store(true, Relaxed);
    assert!(!bus.uses_alt_input(false, false), "only input and aux channels have ALT inputs");
    assert_eq!(a.active_inputs(true, true), (2, -1));
}

#[test]
fn playback_follows_the_loop_through_the_engine() {
    let dir = temp_dir("loop");
    let mut rig = Rig::new(1, 4);
    rig.engine.set_num_channels(1);
    let ramp: Vec<f32> = (0..20_000).map(|k| k as f32 / 20_000.0).collect();
    let file = dir.join("ramp.wav");
    write_wav(&file, &ramp);
    rig.engine.recorder().set_playlist(&Playlist { tracks: vec![PlaybackTrack::new(0, 1, vec![Clip::new(&file, 0, 20_000)])] });
    let ch = rig.engine.channel_at(0).unwrap();
    ch.params.input_left.store(PLAYBACK_SOURCE_BASE, Relaxed);
    ch.params.eq.on.store(false, Relaxed);
    ch.params.output_left.store(2, Relaxed); // direct out
                                             // 300 samples from 1000: not a multiple of the block.
    rig.engine.recorder().set_loop(1000, 1300, true);
    rig.engine.recorder().locate(900);
    rig.engine.recorder().play();
    let mut out = Vec::new();
    while rig.clock < 48_000 {
        let was_rolling = rig.state().is_rolling();
        rig.block(&|_, _| 0.0);
        if was_rolling || rig.state().is_rolling() {
            out.extend_from_slice(&rig.outputs[2]);
        }
    }
    // The first rolling block starts at 900; then 900..1300, 1000..1300, ...
    let mut pos = 900usize;
    for (i, &x) in out.iter().enumerate() {
        if i >= DECLICK_SAMPLES {
            assert!((x - ramp[pos]).abs() < 1e-6, "sample {i}: {x} != ramp[{pos}]");
        }
        pos += 1;
        if pos == 1300 {
            pos = 1000;
        }
    }
    assert!(out.len() > 10 * 300, "several passes");
    let _ = std::fs::remove_dir_all(dir);
}
