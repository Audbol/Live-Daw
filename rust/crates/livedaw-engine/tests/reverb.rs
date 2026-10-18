//! Reverbs in the console, the way a WING uses them: channels send to a bus
//! with a reverb on its insert, the bus feeds Main 1; or a reverb straight on
//! a channel insert with a dry/wet FX MIX. Every reverb device must ring on
//! after the input stops.

use livedaw_dsp::wing::fx::model_index;
use livedaw_engine::params::StripKind;
use livedaw_engine::{Engine, Strip};
use std::sync::atomic::Ordering::Relaxed;
use std::sync::Arc;

/// The reverb devices of the FX table.
const REVERBS: &[&str] = &[
    "HALL", "ROOM", "CHAMBER", "PLATE", "CONCERT", "AMBI", "V-ROOM", "V-REV", "V-PLATE", "GATED", "REVERSE", "DEL/REV",
    "SHIMMER", "SPRING", "VSS3", "BPLATE",
];

/// Drives an engine with a noise burst on input 1 and records Main 1 (outputs 1-2).
struct Rig {
    engine: Arc<Engine>,
    sample_rate: f64,
    block: usize,
    inputs: Vec<Vec<f32>>,
    outputs: Vec<Vec<f32>>,
    rng: u32,
}

impl Rig {
    fn new(sample_rate: f64, block: usize) -> Self {
        let engine = Arc::new(Engine::new(0));
        unsafe { engine.prepare(sample_rate, block, &[0, 1], &[0, 1]) };
        engine.set_num_channels(2);
        let main = engine.strip_at(StripKind::Main, 0).unwrap();
        main.params.output_left.store(0, Relaxed);
        main.params.output_right.store(1, Relaxed);
        let ch = engine.channel_at(0).unwrap();
        ch.params.input_left.store(0, Relaxed);
        Self {
            engine,
            sample_rate,
            block,
            inputs: vec![vec![0.0; block]; 2],
            outputs: vec![vec![0.0; block]; 2],
            rng: 0x1357_9bdf,
        }
    }

    fn ch(&self) -> Arc<Strip> {
        self.engine.channel_at(0).unwrap()
    }

    fn bus(&self) -> Arc<Strip> {
        self.engine.strip_at(StripKind::Bus, 0).unwrap()
    }

    /// Runs `secs` of noise (or silence) and returns the energy per sample on Main 1.
    fn run(&mut self, secs: f64, noise: bool) -> f32 {
        let blocks = ((secs * self.sample_rate) as usize).div_ceil(self.block);
        let mut energy = 0.0f64;
        for _ in 0..blocks {
            for x in self.inputs[0].iter_mut() {
                *x = if noise {
                    self.rng ^= self.rng << 13;
                    self.rng ^= self.rng >> 17;
                    self.rng ^= self.rng << 5;
                    (self.rng as f32 / u32::MAX as f32 - 0.5) * 0.5
                } else {
                    0.0
                };
            }
            let ins: Vec<*const f32> = self.inputs.iter().map(|v| v.as_ptr()).collect();
            let outs: Vec<*mut f32> = self.outputs.iter_mut().map(|v| v.as_mut_ptr()).collect();
            unsafe { self.engine.process(&ins, &outs, self.block) };
            for o in &self.outputs {
                assert!(o.iter().all(|x| x.is_finite()));
                energy += o.iter().map(|&x| (x as f64).powi(2)).sum::<f64>();
            }
        }
        (energy / (blocks * self.block) as f64) as f32
    }

    /// Noise for a second, then the tail: (level while playing, tail level
    /// 100-400 ms after the input stopped).
    fn burst(&mut self) -> (f32, f32) {
        let playing = self.run(1.0, true);
        self.run(0.1, false);
        let tail = self.run(0.3, false);
        (playing, tail)
    }

    fn set_model(&self, slot: usize, token: &str) {
        assert!(self.engine.set_fx_model(slot, model_index(token).unwrap(), true), "{token}");
    }
}

fn db(x: f32) -> f32 {
    10.0 * x.max(1e-30).log10()
}

#[test]
fn bus_send_reverb_rings_on_into_main() {
    for &(sr, block) in &[(44_100.0, 441), (48_000.0, 32), (48_000.0, 2048), (96_000.0, 480)] {
        for (slot, token) in REVERBS.iter().enumerate() {
            let mut rig = Rig::new(sr, block);
            let (ch, bus) = (rig.ch(), rig.bus());
            // Only the reverb reaches Main 1: the channel feeds bus 1 (PRE).
            ch.params.mains[0].on.store(false, Relaxed);
            ch.sends.get_or_create(0).unwrap().enabled.store(true, Relaxed);
            bus.params.mains[0].on.store(true, Relaxed);
            let slot = slot % 8;
            rig.set_model(slot, token);
            bus.params.pre_insert.fx.store(slot as i32, Relaxed);
            bus.params.pre_insert.on.store(true, Relaxed);
            let (playing, tail) = rig.burst();
            assert!(db(playing) > -40.0, "{token} @ {sr}: wet level {:.1} dB", db(playing));
            assert!(db(tail) > db(playing) - 45.0, "{token} @ {sr}: tail {:.1} dB vs {:.1} dB", db(tail), db(playing));
            assert!(!rig.engine.fx(slot).unwrap().is_faulted(), "{token}: faulted");
        }
    }
}

#[test]
fn channel_insert_reverb_mixes_dry_and_wet() {
    for token in REVERBS {
        for stereo in [false, true] {
            for post in [false, true] {
                let mut rig = Rig::new(48_000.0, 128);
                let ch = rig.ch();
                ch.params.stereo.store(stereo, Relaxed);
                ch.params.input_right.store(0, Relaxed);
                rig.set_model(3, token);
                rig.engine.controls.fx[3].mix.store(30.0);
                let ins = if post { &ch.params.post_insert } else { &ch.params.pre_insert };
                ins.fx.store(3, Relaxed);
                ins.on.store(true, Relaxed);
                let (playing, tail) = rig.burst();
                assert!(db(playing) > -30.0, "{token}: level {:.1} dB", db(playing));
                assert!(
                    db(tail) > db(playing) - 50.0,
                    "{token} (stereo {stereo}, post {post}): tail {:.1} dB vs {:.1} dB",
                    db(tail),
                    db(playing)
                );
            }
        }
    }
}

#[test]
fn reverb_survives_model_changes_and_slot_moves() {
    let mut rig = Rig::new(48_000.0, 256);
    let (ch, bus) = (rig.ch(), rig.bus());
    ch.params.mains[0].on.store(false, Relaxed);
    ch.sends.get_or_create(0).unwrap().enabled.store(true, Relaxed);
    bus.params.mains[0].on.store(true, Relaxed);
    bus.params.pre_insert.on.store(true, Relaxed);
    for (slot, token) in [(0, "HALL"), (0, "PLATE"), (5, "SPRING"), (2, "V-ROOM"), (2, "ROOM")] {
        rig.set_model(slot, token);
        bus.params.pre_insert.fx.store(slot as i32, Relaxed);
        let (playing, tail) = rig.burst();
        assert!(db(tail) > db(playing) - 45.0, "{token} in FX {}: tail {:.1} dB", slot + 1, db(tail));
    }
    // A model written to the shared parameters (a scene recall) gets its
    // device on the next collect.
    rig.engine.controls.fx[2].set_model(model_index("CHAMBER").unwrap(), true);
    rig.engine.collect();
    let (playing, tail) = rig.burst();
    assert!(db(tail) > db(playing) - 45.0, "recalled model: tail {:.1} dB", db(tail));
}
