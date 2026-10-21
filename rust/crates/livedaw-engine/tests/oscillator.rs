//! The oscillator (the OSC source group) patched to channels.

use livedaw_dsp::generator::generator_mode;
mod common;
use common::{set_channels, unity_console};
use livedaw_engine::sources::{source_index, source_mode, OSC_SOURCE_BASE, OSC_SOURCE_INDEX};
use livedaw_engine::Engine;
use std::sync::atomic::Ordering::Relaxed;
use std::sync::Arc;

const BLOCK: usize = 64;
const SR: f64 = 48000.0;

/// An engine with silent inputs; `run` returns what the two outputs played.
struct Rig {
    engine: Arc<Engine>,
    inputs: Vec<Vec<f32>>,
    outputs: Vec<Vec<f32>>,
}

impl Rig {
    fn new() -> Self {
        let engine = Arc::new(Engine::new(0));
        unity_console(&engine);
        unsafe { engine.prepare(SR, BLOCK, &[0, 1], &[0, 1]) };
        set_channels(&engine, 2);
        Self { engine, inputs: vec![vec![0.0; BLOCK]; 2], outputs: vec![vec![0.0; BLOCK]; 2] }
    }

    fn run(&mut self, samples: usize) -> [Vec<f32>; 2] {
        let ins: Vec<*const f32> = self.inputs.iter().map(|v| v.as_ptr()).collect();
        let outs: Vec<*mut f32> = self.outputs.iter_mut().map(|v| v.as_mut_ptr()).collect();
        let mut played = [Vec::new(), Vec::new()];
        for _ in 0..samples / BLOCK {
            unsafe { self.engine.process(&ins, &outs, BLOCK) };
            for (p, o) in played.iter_mut().zip(&self.outputs) {
                p.extend_from_slice(o);
            }
        }
        played
    }
}

fn peak(x: &[f32]) -> f32 {
    x.iter().fold(0.0, |m, v| m.max(v.abs()))
}

fn rms_db(x: &[f32]) -> f64 {
    10.0 * (x.iter().map(|&v| v as f64 * v as f64).sum::<f64>() / x.len() as f64).log10()
}

#[test]
fn a_channel_patched_to_osc_plays_its_tone() {
    let mut rig = Rig::new();
    let engine = rig.engine.clone();
    let ch = engine.channel_at(0).unwrap();
    ch.params.input_left.store(OSC_SOURCE_BASE, Relaxed);
    let osc = engine.sources().osc(0).unwrap();
    osc.level_db.store(-12.0);
    osc.freq_hz.store(440.0);

    let [l, _] = rig.run(SR as usize);
    // Past the channel's ramps: a -12 dBFS sine (unity channel, centre pan and main).
    let tail = &l[4800..];
    assert!((20.0 * peak(tail).log10() - -12.0).abs() < 0.1, "peak {}", peak(tail));
    let crossings = l.windows(2).filter(|w| w[0] < 0.0 && w[1] >= 0.0).count();
    assert!((crossings as i32 - 440).abs() <= 1, "{crossings} cycles in a second");
    assert!(ch.meters.input_peak[0].load() > 0.2, "the channel meters it");

    // Pink noise at the sine's RMS.
    osc.mode.store(generator_mode::PINK, Relaxed);
    let [l, _] = rig.run(SR as usize * 4);
    let level = rms_db(&l[4800..]);
    assert!((level - (-15.01)).abs() < 0.5, "pink at {level} dB RMS");

    // MUTE switches the oscillator off; OSC 2 isn't patched anywhere.
    engine.sources().get(source_index(OSC_SOURCE_BASE).unwrap()).unwrap().mute.store(true, Relaxed);
    let [l, _] = rig.run(BLOCK * 64);
    assert_eq!(peak(&l[BLOCK * 32..]), 0.0);
}

#[test]
fn a_stereo_pair_plays_osc_1_on_both_sides() {
    let mut rig = Rig::new();
    let engine = rig.engine.clone();
    let ch = engine.channel_at(0).unwrap();
    ch.params.input_left.store(OSC_SOURCE_BASE, Relaxed);
    // STEREO: the channel patched to OSC 1 goes stereo on OSC 1 / 2.
    engine.set_source_mode(OSC_SOURCE_INDEX, source_mode::STEREO);
    assert!(ch.params.stereo.load(Relaxed));
    let src = engine.sources();
    src.osc(1).unwrap().freq_hz.store(5000.0); // ignored while paired
    src.osc(1).unwrap().level_db.store(-40.0);

    let [l, r] = rig.run(SR as usize / 2);
    let (l, r) = (&l[4800..], &r[4800..]);
    assert!(peak(l) > 0.05);
    let diff = l.iter().zip(r).fold(0.0f32, |m, (a, b)| m.max((a - b).abs()));
    assert!(diff < 1e-6, "the sine is in phase on both sides ({diff})");

    // Noise: the same level on both sides, uncorrelated.
    src.osc(0).unwrap().mode.store(generator_mode::WHITE, Relaxed);
    let [l, r] = rig.run(SR as usize);
    let (l, r) = (&l[4800..], &r[4800..]);
    assert!((rms_db(l) - rms_db(r)).abs() < 0.2);
    let dot: f64 = l.iter().zip(r).map(|(&a, &b)| a as f64 * b as f64).sum();
    let norm = (l.iter().map(|&a| a as f64 * a as f64).sum::<f64>() * r.iter().map(|&b| b as f64 * b as f64).sum::<f64>()).sqrt();
    assert!((dot / norm).abs() < 0.05, "correlation {}", dot / norm);
}

#[test]
fn osc_sources_have_source_settings_and_initialize_resets_the_oscillator() {
    let engine = Engine::new(0);
    let index = source_index(OSC_SOURCE_BASE + 1).unwrap();
    assert_eq!(index, OSC_SOURCE_INDEX + 1);
    let src = engine.sources();
    let osc = src.osc(1).unwrap();
    osc.mode.store(generator_mode::WHITE, Relaxed);
    osc.level_db.store(-6.0);
    src.set_name(index, "PINK");
    src.initialize(index);
    assert!(osc.is_default());
    assert_eq!(src.name(index), "");
    assert!(source_index(OSC_SOURCE_BASE + 2).is_none());
}
