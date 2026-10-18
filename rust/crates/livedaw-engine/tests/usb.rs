//! The USB recorder / player inside the engine: the RECORDER output group
//! taps sources and strips, the USB PLAYER channels are sources channels
//! patch like any input.

use livedaw_engine::params::StripKind;
use livedaw_engine::recorder::usb::{PlayerState, RecFormat, RecState, RecTap};
use livedaw_engine::recorder::wav::WavReader;
use livedaw_engine::{Engine, USB_PLAYER_SOURCE_BASE};
use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering::Relaxed;
use std::time::Duration;

const BLOCK: usize = 128;
const WAIT: Duration = Duration::from_secs(10);

fn temp_dir(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("livedaw-engine-usb-{}-{}", name, std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

fn read_wav(path: &Path) -> Vec<Vec<f32>> {
    let mut r = WavReader::open(path).unwrap();
    let info = *r.info();
    let mut chans = vec![vec![0.0; info.frames as usize]; info.channels];
    let mut refs: Vec<&mut [f32]> = chans.iter_mut().map(|c| &mut c[..]).collect();
    r.read(0, &mut refs).unwrap();
    chans
}

/// Input 1's signal: distinct values on the 24-bit grid.
fn signal(t: usize) -> f32 {
    ((((t * 7919) % 200_001) as f64 - 100_000.0) / 8_388_608.0) as f32
}

struct Rig {
    engine: Engine,
    inputs: Vec<Vec<f32>>,
    outputs: Vec<Vec<f32>>,
    clock: usize,
    /// Hardware output 1 as it was produced.
    out1: Vec<f32>,
}

impl Rig {
    fn new() -> Self {
        let engine = Engine::new(0);
        unsafe { engine.prepare(48000.0, BLOCK, &[0, 1], &[0, 1]) };
        engine.set_num_channels(2);
        Self { engine, inputs: vec![vec![0.0; BLOCK]; 2], outputs: vec![vec![0.0; BLOCK]; 2], clock: 0, out1: Vec::new() }
    }

    fn block(&mut self) {
        assert!(self.engine.usb().wait_for_disk(WAIT));
        for (i, x) in self.inputs[0].iter_mut().enumerate() {
            *x = signal(self.clock + i);
        }
        let ins: Vec<*const f32> = self.inputs.iter().map(|v| v.as_ptr()).collect();
        let outs: Vec<*mut f32> = self.outputs.iter_mut().map(|v| v.as_mut_ptr()).collect();
        unsafe { self.engine.process(&ins, &outs, BLOCK) };
        self.out1.extend_from_slice(&self.outputs[0]);
        self.clock += BLOCK;
    }
}

#[test]
fn records_the_recorder_group_and_plays_it_back_as_usb_player_sources() {
    let dir = temp_dir("loop");
    let mut rig = Rig::new();
    let usb = rig.engine.usb();
    // REC 1 = input 1 (a source), REC 2 = MAIN 1 L, whose hardware output 1
    // the rig captures (by default REC 1/2 = MAIN 1 L/R).
    assert_eq!(usb.tap(1), RecTap::Strip { kind: StripKind::Main as i32, number: 1, channel: 1 });
    usb.set_tap(0, RecTap::Source(0));
    usb.set_tap(1, RecTap::Strip { kind: StripKind::Main as i32, number: 1, channel: 0 });
    let ch1 = rig.engine.channel_at(0).unwrap();
    ch1.params.input_left.store(0, Relaxed);
    ch1.params.fader_db.store(0.0);
    rig.engine.main(0).unwrap().params.fader_db.store(0.0);

    let take = dir.join("R_take.wav");
    rig.engine.usb().record(&take, RecFormat { channels: 2, bits: 24 }).unwrap();
    let start = rig.clock; // the next block starts the recording
    while rig.clock < start + 48_000 {
        rig.block();
    }
    assert_eq!(rig.engine.usb().rec_state(), RecState::Recording);
    rig.engine.usb().stop_recording();
    rig.block();
    assert!(rig.engine.usb().wait_for_disk(WAIT));

    let rec = read_wav(&take);
    assert_eq!(rec.len(), 2);
    assert_eq!(rec[0].len(), 48_000);
    let expect: Vec<f32> = (start..start + 48_000).map(signal).collect();
    assert_eq!(rec[0], expect, "a source tap records bit-exact");
    let main_l = &rig.out1[start..start + 48_000];
    let worst = rec[1].iter().zip(main_l).map(|(a, b)| (a - b).abs()).fold(0.0f32, f32::max);
    assert!(worst <= 1.0 / 8_388_608.0, "MAIN 1 L within a 24-bit step ({worst})");
    assert!(main_l.iter().any(|&x| x != 0.0), "the main carries the input");

    // Play the take into USB PLAYER 1/2; CH 2 listens to USB PLAYER 1 and
    // REC 3 records it again (4-channel file).
    let ch2 = rig.engine.channel_at(1).unwrap();
    ch2.params.input_left.store(USB_PLAYER_SOURCE_BASE, Relaxed);
    let id = {
        let mut q = rig.engine.usb().queue();
        q.new_playlist();
        q.add(&take)
    };
    let again = dir.join("R_again.wav");
    rig.engine.usb().set_tap(2, RecTap::Source(USB_PLAYER_SOURCE_BASE));
    rig.engine.usb().record(&again, RecFormat { channels: 4, bits: 24 }).unwrap();
    rig.engine.usb().play();
    for _ in 0..(60_000 / BLOCK) {
        rig.block();
    }
    assert_eq!(rig.engine.usb().player_state(), PlayerState::Stopped, "the clip ended");
    assert_eq!(rig.engine.usb().current_entry(), id);
    assert!(ch2.meters.input_peak[0].load() > 0.0, "CH 2 hears USB PLAYER 1");
    rig.engine.usb().stop_recording();
    rig.block();
    assert!(rig.engine.usb().wait_for_disk(WAIT));

    let rec2 = read_wav(&again);
    assert_eq!(rec2.len(), 4);
    // After the player's fade-in, the take plays back sample for sample.
    let played = &rec2[2];
    let at = played.windows(64).position(|w| w == &expect[1000..1064]).expect("the take plays") - 1000;
    assert_eq!(&played[at + 256..at + 48_000], &expect[256..]);
    let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn usb_player_sources_have_source_settings() {
    use livedaw_engine::sources::{source_index, source_port, USB_PLAYER_SOURCE_INDEX};
    let engine = Engine::new(0);
    let index = source_index(USB_PLAYER_SOURCE_BASE + 2).unwrap();
    assert_eq!(index, USB_PLAYER_SOURCE_INDEX + 2);
    assert_eq!(source_port(index), Some(USB_PLAYER_SOURCE_BASE + 2));
    assert!(engine.sources().get(index).is_some());
}
