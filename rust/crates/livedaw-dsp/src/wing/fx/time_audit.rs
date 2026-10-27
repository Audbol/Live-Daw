//! The functional audit of the reverbs, delays, modulation, pitch and amp
//! devices: each control driven with real signals and its effect measured
//! (decay times from the impulse response, delay and pre-delay times to the
//! sample, feedback, LFO rates, pitch intervals, modes, MIX, stereo, clicks on
//! jumps, tails), and the live readouts the panels' meters show.

use super::{model_index, FxContext, FxDevice, FxLive};
use crate::wing::{ParamKind, FX_MODELS, MAX_FX_PARAMS};

pub(crate) const SR: f32 = 48_000.0;

/// The devices this audit covers.
pub(crate) const AUDITED: &[&str] = &[
    "HALL", "ROOM", "CHAMBER", "PLATE", "CONCERT", "AMBI", "V-ROOM", "V-REV", "V-PLATE", "GATED", "REVERSE", "DEL/REV",
    "SHIMMER", "SPRING", "VSS3", "BPLATE", "DIMCRS", "CHORUS", "FLANGER", "PHASER", "PANNER", "ROTARY", "MOOD", "ST-DL",
    "TAP-DL", "TAPE-DL", "OILCAN", "BBD-DL", "PITCH", "D-PITCH", "DOUBLE", "PCORR", "SUB", "SUB-M", "P-BASS", "RACKAMP",
    "UKROCK", "ANGEL", "JAZZC", "DELUXE",
];

/// A device at 48 kHz and its parameter values (defaults with `edit`).
pub(crate) struct Rig {
    pub dev: FxDevice,
    pub values: [f32; MAX_FX_PARAMS],
    token: &'static str,
}

impl Rig {
    pub fn new(token: &str, edit: &[(&str, f32)]) -> Self {
        let model = model_index(token).unwrap_or_else(|| panic!("{token}: unknown"));
        let spec = &FX_MODELS[model];
        let mut values = [0.0; MAX_FX_PARAMS];
        spec.defaults_into(&mut values);
        let mut rig =
            Self { dev: FxDevice::new(model, FxContext { sample_rate: SR, max_block: 256 }).unwrap(), values, token: spec.token };
        rig.set(edit);
        rig
    }

    pub fn set(&mut self, edit: &[(&str, f32)]) {
        let spec = &FX_MODELS[self.dev.model()];
        for (id, v) in edit {
            let i = spec.param_index(id).unwrap_or_else(|| panic!("{}: no parameter {id}", self.token));
            self.values[i] = *v;
        }
    }

    /// Runs stereo `l`/`r` through in 256-sample blocks; returns the outputs.
    pub fn run(&mut self, l: &[f32], r: &[f32]) -> (Vec<f32>, Vec<f32>) {
        let n = self.dev.spec().params.len();
        let (mut ol, mut or) = (l.to_vec(), r.to_vec());
        for (a, b) in ol.chunks_mut(256).zip(or.chunks_mut(256)) {
            self.dev.process(&self.values[..n], a, b);
        }
        (ol, or)
    }

    /// Mono input on both sides; the mono sum out.
    pub fn mono(&mut self, x: &[f32]) -> Vec<f32> {
        let (l, r) = self.run(x, x);
        l.iter().zip(&r).map(|(a, b)| 0.5 * (a + b)).collect()
    }

    pub fn live(&self, what: FxLive) -> f32 {
        self.dev.live(what)
    }
}

pub(crate) fn secs(s: f32) -> usize {
    (s * SR) as usize
}

pub(crate) fn impulse(n: usize) -> Vec<f32> {
    let mut v = vec![0.0; n];
    v[0] = 1.0;
    v
}

pub(crate) fn sine(hz: f32, amp: f32, n: usize) -> Vec<f32> {
    (0..n).map(|i| amp * (std::f32::consts::TAU * hz * i as f32 / SR).sin()).collect()
}

pub(crate) fn noise(seed: u32, amp: f32, n: usize) -> Vec<f32> {
    let mut s = seed | 1;
    (0..n)
        .map(|_| {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            amp * ((s as f32 / u32::MAX as f32) * 2.0 - 1.0)
        })
        .collect()
}

pub(crate) fn energy(x: &[f32]) -> f32 {
    x.iter().map(|v| v * v).sum()
}

pub(crate) fn rms(x: &[f32]) -> f32 {
    (energy(x) / x.len().max(1) as f32).sqrt()
}

pub(crate) fn peak(x: &[f32]) -> f32 {
    x.iter().fold(0.0f32, |a, v| a.max(v.abs()))
}

/// Level of `hz` in `x` (Goertzel), as the amplitude of a sine.
pub(crate) fn tone(x: &[f32], hz: f32) -> f32 {
    let w = std::f32::consts::TAU * hz / SR;
    let c = 2.0 * w.cos();
    let (mut s1, mut s2) = (0.0f64, 0.0f64);
    for &v in x {
        let s = f64::from(v) + f64::from(c) * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    let p = s1 * s1 + s2 * s2 - f64::from(c) * s1 * s2;
    (2.0 * p.max(0.0).sqrt() / x.len() as f64) as f32
}

/// The strongest frequency between `lo` and `hi` Hz (1 Hz steps, then refined).
pub(crate) fn dominant(x: &[f32], lo: f32, hi: f32) -> f32 {
    let mut best = (lo, 0.0f32);
    let mut f = lo;
    while f <= hi {
        let t = tone(x, f);
        if t > best.1 {
            best = (f, t);
        }
        f += 1.0;
    }
    let mut f = best.0 - 1.0;
    while f <= best.0 + 1.0 {
        let t = tone(x, f);
        if t > best.1 {
            best = (f, t);
        }
        f += 0.05;
    }
    best.0
}

/// RT60 from the energy decay curve (Schroeder), fitted between -5 and -25 dB.
pub(crate) fn rt60(x: &[f32]) -> f32 {
    let mut edc = vec![0.0f64; x.len()];
    let mut acc = 0.0f64;
    for i in (0..x.len()).rev() {
        acc += f64::from(x[i]).powi(2);
        edc[i] = acc;
    }
    let db = |i: usize| 10.0 * (edc[i] / edc[0]).log10();
    let t5 = (0..x.len()).find(|&i| db(i) < -5.0).unwrap_or(0);
    let t25 = (0..x.len()).find(|&i| db(i) < -25.0).unwrap_or(x.len());
    3.0 * (t25 - t5) as f32 / SR
}

/// The first sample whose magnitude exceeds `frac` of the peak.
pub(crate) fn onset(x: &[f32], frac: f32) -> usize {
    let p = peak(x);
    x.iter().position(|v| v.abs() > frac * p).unwrap_or(x.len())
}

/// Index of the largest magnitude in `x[from..to]`.
pub(crate) fn argmax(x: &[f32], from: usize, to: usize) -> usize {
    (from..to.min(x.len())).max_by(|&a, &b| x[a].abs().total_cmp(&x[b].abs())).unwrap_or(from)
}

/// How different two renders are: the difference's energy against the larger one's.
fn difference(a: &(Vec<f32>, Vec<f32>), b: &(Vec<f32>, Vec<f32>)) -> f32 {
    let d: f32 = a.0.iter().zip(&b.0).chain(a.1.iter().zip(&b.1)).map(|(x, y)| (x - y) * (x - y)).sum();
    let e = (energy(&a.0) + energy(&a.1)).max(energy(&b.0) + energy(&b.1)).max(1e-12);
    d / e
}

/// The probe the parameter check plays: a slightly flat A2 sawtooth (pitched,
/// rich in harmonics) with a noise burst and an accent, left and right
/// different, then silence for the tails.
fn probe() -> (Vec<f32>, Vec<f32>) {
    let n = secs(2.0);
    let hum = secs(0.9);
    let saw = |i: usize, hz: f32| {
        let ph = (hz * i as f32 / SR).fract();
        0.35 * (2.0 * ph - 1.0)
    };
    let nl = noise(7, 0.25, n);
    let nr = noise(11, 0.25, n);
    // Notes of 150 ms with 150 ms gaps (envelopes release, tails show).
    let note = secs(0.15);
    let env = |i: usize| {
        let k = i % (2 * note);
        if i >= hum || k >= note {
            return 0.0;
        }
        (k as f32 / 240.0).min(1.0) * ((note - k) as f32 / 240.0).min(1.0)
    };
    let l: Vec<f32> = (0..n).map(|i| env(i) * (saw(i, 108.5) + if i < secs(0.15) { nl[i] } else { 0.0 })).collect();
    let r: Vec<f32> = (0..n).map(|i| env(i) * (0.8 * saw(i, 108.5) + if i < secs(0.15) { nr[i] } else { 0.0 })).collect();
    (l, r)
}

/// Settings a parameter only shows its effect in (FREEZE for ADD, the
/// envelope depth for the envelope times, the speed a rate belongs to...).
fn context(token: &str, id: &str) -> &'static [(&'static str, f32)] {
    match (token, id) {
        ("V-ROOM", "add") => &[("frz", 1.0)],
        ("PHASER", "att" | "hld" | "rel") => &[("emod", 100.0)],
        ("PANNER", "att" | "hld" | "rel") => &[("edep", 100.0)],
        ("PANNER", "espd") => &[("depth", 60.0)],
        ("MOOD", "att" | "hld" | "rel") => &[("env", 100.0), ("lfo", 0.0)],
        ("MOOD", "phase") => &[("reso", 6.0)],
        ("ROTARY", "hi") => &[("sw", 2.0)],
        ("CHAMBER", "ecl" | "ecr") => &[("ell", 0.0), ("elr", 0.0)],
        ("UKROCK" | "ANGEL" | "DELUXE", "sag") => &[("gain", 9.0), ("vol", 9.0), ("mstr", 9.0)],
        ("CHORUS", "sprd") => &[("phase", 0.0)],
        ("DIMCRS", "in") => &[],
        ("PITCH", "cent") => &[("semi", 0.0)],
        ("D-PITCH", "cent1" | "cent2") => &[],
        ("VSS3", "lxo" | "mxo") => &[("ldcy", 0.3), ("lmdcy", 2.5), ("hmdcy", 0.5)],
        ("VSS3", "lshv") => &[("lsdmp", -18.0)],
        ("P-BASS", "xf") => &[("bass", -30.0)],
        ("SUB-M", _) => &[("mix", 100.0)],
        _ => &[],
    }
}

/// Parameters with nothing to hear in a render: screen-only switches and
/// those checked by their own tests below.
fn exempt(token: &str, id: &str) -> bool {
    matches!(
        (token, id),
        // The console's preset list and LOAD set the other parameters (no preset data: see the audit log); VIEW is the screen's.
        ("VSS3", "view" | "preset" | "load")
            // The acceleration of a speed change (ROTARY's drum and horn): checked by `rotary_speeds_and_ramps`.
            | ("ROTARY", "dac" | "hac")
            // PITCH FIX's note keys: checked by `pitch_fix_corrects_to_the_scale`.
            | ("PCORR", "sw_c" | "sw_db" | "sw_d" | "sw_eb" | "sw_e" | "sw_f" | "sw_gb" | "sw_g" | "sw_ab" | "sw_bb" | "sw_b")
    )
}

#[test]
fn every_parameter_changes_the_sound() {
    let (l, r) = probe();
    let mut dead = Vec::new();
    for token in AUDITED {
        let spec = &FX_MODELS[model_index(token).unwrap()];
        for p in spec.params {
            if exempt(token, p.id) {
                continue;
            }
            let render = |pos: f32| {
                let ctx: Vec<_> = context(token, p.id).iter().copied().filter(|(id, _)| spec.param_index(id).is_some()).collect();
                let mut rig = Rig::new(token, &ctx);
                // A phase that wraps (-180..180) is the same at both ends: compare the middle.
                let wraps = p.unit.is_empty() && p.lo() == -180.0 && p.hi() == 180.0;
                let v = match p.kind {
                    ParamKind::Toggle => pos,
                    _ if wraps => p.from_normalised(0.5 + 0.5 * pos),
                    _ => p.from_normalised(pos),
                };
                rig.set(&[(p.id, v)]);
                rig.run(&l, &r)
            };
            let d = difference(&render(0.0), &render(1.0));
            if d < 1e-3 {
                dead.push(format!("{token} {} ({:.1e})", p.id, d));
            }
        }
    }
    assert!(dead.is_empty(), "parameters that change nothing: {dead:?}");
}

// ---- Reverbs ------------------------------------------------------------------------

/// The mid band (500 Hz - 2 kHz) of `x`.
fn mid_band(x: &[f32]) -> Vec<f32> {
    let mut bp = crate::biquad::Biquad::default();
    bp.set_coefficients(crate::biquad::BiquadCoeffs::band_pass(f64::from(SR), 1000.0, 0.7));
    x.iter().map(|&v| bp.tick(v, 0)).collect()
}

#[test]
fn reverb_decay_times_follow_decay() {
    // The mid-band RT60 of the impulse response against the DECAY control,
    // with the filters open (and SIZE at the default, where the 480L banks'
    // DECAY is the RT60).
    let open: &[(&str, f32)] = &[("hc", 20_000.0), ("damp", 20_000.0)];
    type Case<'a> = (&'a str, &'a [(&'a str, f32)], &'a [f32]);
    let cases: &[Case] = &[
        ("HALL", open, &[0.8, 1.6, 3.5]),
        // The pre-echo loops (FEED / ECHO level) off: they add their own repeats.
        ("ROOM", &[("hc", 20_000.0), ("damp", 20_000.0), ("efl", 0.0), ("efr", 0.0)], &[0.8, 2.5, 6.0]),
        ("CHAMBER", &[("hc", 20_000.0), ("damp", 20_000.0), ("ell", -60.0), ("elr", -60.0)], &[0.8, 2.0, 5.0]),
        ("PLATE", &[("hc", 20_000.0), ("damp", 20_000.0), ("efl", 0.0), ("efr", 0.0)], &[0.8, 2.0, 5.0]),
        ("CONCERT", open, &[1.0, 2.0, 5.0]),
        ("AMBI", &[("hc", 20_000.0), ("damp", 20_000.0), ("tail", 100.0)], &[0.6, 1.5, 4.0]),
        ("BPLATE", open, &[0.8, 1.6, 3.5]),
        ("V-ROOM", &[("hc", 20_000.0), ("hmult", 1.0), ("lmult", 1.0)], &[0.6, 1.5, 4.0]),
        ("V-REV", &[("hc", 20_000.0), ("hmult", 0.67)], &[0.6, 1.5, 4.0]),
        ("V-PLATE", &[], &[1.2, 2.5, 5.0]),
        ("DEL/REV", &[("dly", 0.0), ("d2r", 0.0), ("i2r", 100.0), ("damp", 20_000.0)], &[0.6, 1.5, 4.0]),
        ("SHIMMER", &[("shim", 0.0), ("hc", 7000.0), ("damp", 0.0)], &[1.5, 3.0, 8.0]),
        ("SPRING", &[], &[1.5, 3.0, 6.0]),
        ("VSS3", &[("hcut", 20_000.0), ("erlvl", -60.0)], &[0.6, 1.5, 4.0]),
    ];
    let mut bad = Vec::new();
    for (token, set, decays) in cases {
        for &dcy in *decays {
            let mut rig = Rig::new(token, set);
            rig.set(&[("dcy", dcy)]);
            let ir = rig.mono(&impulse(secs(2.5 * dcy + 2.0)));
            let t = rt60(&mid_band(&ir));
            if (t / dcy - 1.0).abs() > 0.3 {
                bad.push(format!("{token} DECAY {dcy} s: RT60 {t:.2} s"));
            }
        }
    }
    assert!(bad.is_empty(), "{bad:?}");
}

#[test]
fn pre_delays_are_exact() {
    // The first sound of the impulse response moves by exactly the change in
    // PRE DLY (within 1 ms).
    let cases: &[(&str, &str, f32, f32)] = &[
        ("HALL", "pdel", 0.0, 120.0),
        ("ROOM", "pdel", 0.0, 120.0),
        ("CHAMBER", "pdel", 0.0, 120.0),
        ("PLATE", "pdel", 0.0, 120.0),
        ("CONCERT", "pdel", 0.0, 120.0),
        ("AMBI", "pdel", 0.0, 120.0),
        ("BPLATE", "pdel", 0.0, 120.0),
        ("V-ROOM", "pdel", 0.0, 120.0),
        ("V-REV", "pdel", 0.0, 100.0),
        ("V-PLATE", "pdel", 0.0, 200.0),
        ("GATED", "pdel", 0.0, 120.0),
        ("REVERSE", "pdel", 0.0, 120.0),
        ("SHIMMER", "pdel", 0.0, 200.0),
        ("DEL/REV", "pdel", 0.0, 150.0),
        ("VSS3", "rvpdly", 0.0, 150.0),
        ("PITCH", "dly", 0.0, 250.0),
    ];
    let mut bad = Vec::new();
    for &(token, id, a, b) in cases {
        let first = |v: f32| {
            let mut rig = Rig::new(token, &[(id, v)]);
            match token {
                "VSS3" => rig.set(&[("erlvl", -60.0)]),
                "DEL/REV" => rig.set(&[("dly", 0.0), ("d2r", 0.0), ("i2r", 100.0)]),
                "V-ROOM" => rig.set(&[("erlvl", 0.0)]),
                // The 480L's pre-echoes come before the pre-delayed reverb (as their name says).
                "CHAMBER" => rig.set(&[("ell", -60.0), ("elr", -60.0)]),
                "ROOM" | "PLATE" => rig.set(&[("efl", 0.0), ("efr", 0.0)]),
                _ => {}
            }
            onset(&rig.mono(&impulse(secs(1.0))), 1e-3)
        };
        let shift = first(b) as f32 - first(a) as f32;
        let want = (b - a) * SR / 1000.0;
        if (shift - want).abs() > 0.001 * SR {
            bad.push(format!("{token} {id} {a} -> {b} ms moved the onset {:.1} ms", shift / SR * 1000.0));
        }
    }
    assert!(bad.is_empty(), "{bad:?}");
}

/// The level of `x` above `hz` against its level below `hz` / 4, dB.
fn tilt_db(x: &[f32], hz: f32) -> f32 {
    let mut hp = crate::biquad::Biquad::default();
    hp.set_coefficients(crate::biquad::BiquadCoeffs::high_pass(f64::from(SR), f64::from(hz), 0.707));
    let mut lp = crate::biquad::Biquad::default();
    lp.set_coefficients(crate::biquad::BiquadCoeffs::low_pass(f64::from(SR), f64::from(hz / 4.0), 0.707));
    let h: Vec<f32> = x.iter().map(|&v| hp.tick(v, 0)).collect();
    let l: Vec<f32> = x.iter().map(|&v| lp.tick(v, 0)).collect();
    10.0 * (energy(&h).max(1e-20) / energy(&l).max(1e-20)).log10()
}

#[test]
fn cut_filters_cut_the_right_end() {
    // Lowering a HI CUT / DAMPING darkens the output, raising a LO CUT
    // thins it, on every device that has one.
    let n = noise(5, 0.3, secs(1.2));
    let mut bad = Vec::new();
    for token in AUDITED {
        let spec = &FX_MODELS[model_index(token).unwrap()];
        for p in spec.params {
            let (lo_cut, hi_cut) = match p.label {
                "LO CUT" | "FEED LC" | "FEED LCUT" | "REV LCUT" | "ER LCUT" => (true, false),
                "HI CUT" | "FEED HC" | "FEED HCUT" | "DAMPING" => (false, true),
                _ => (false, false),
            };
            if !(lo_cut || hi_cut) || (*token == "SHIMMER" && p.id == "damp") {
                continue;
            }
            let at = |pos: f32| {
                let mut rig = Rig::new(token, &[]);
                match (*token, p.id) {
                    ("VSS3", "erlc") => rig.set(&[("rvlvl", -60.0), ("erlvl", 0.0)]),
                    // The wet signal alone, and feedback filters with plenty of feedback.
                    ("CHORUS", _) => rig.set(&[("mix", 100.0)]),
                    ("FLANGER", _) => rig.set(&[("mix", 100.0), ("feed", 90.0)]),
                    ("ST-DL", "flc" | "fhc") => rig.set(&[("feed", 90.0), ("time", 40.0)]),
                    ("DEL/REV", "fhc") => rig.set(&[("feed", 100.0), ("dly", 100.0), ("d2r", 0.0), ("time", 40.0)]),
                    _ => {}
                }
                // A feedback high cut at its lowest leaves only the first pass
                // in both bands: compare it a little higher up.
                let feedback_hc = hi_cut && p.label.starts_with("FEED");
                rig.set(&[(p.id, p.from_normalised(if feedback_hc { 0.4 + 0.6 * pos } else { pos }))]);
                let out = rig.mono(&n);
                tilt_db(&out[secs(0.3)..], if lo_cut { 400.0 } else { 3000.0 })
            };
            let (a, b) = (at(0.0), at(1.0));
            // Opening a HI CUT (max) leaves more highs, raising a LO CUT (max)
            // fewer lows: either way the tilt rises.
            let ok = b > a + 3.0;
            if !ok {
                bad.push(format!("{token} {} ({}): tilt {a:.1} dB at min, {b:.1} dB at max", p.id, p.label));
            }
        }
    }
    assert!(bad.is_empty(), "{bad:?}");
}

#[test]
fn reverbs_are_stereo_and_tails_ring_on() {
    // A left-only impulse comes out on both sides, the two sides differ, and
    // the tail keeps ringing after the input stops.
    for token in [
        "HALL", "ROOM", "CHAMBER", "PLATE", "CONCERT", "AMBI", "V-ROOM", "V-REV", "V-PLATE", "BPLATE", "VSS3", "SHIMMER",
        "DEL/REV",
    ] {
        let mut rig = Rig::new(token, &[]);
        let x = impulse(secs(2.0));
        let (l, r) = rig.run(&x, &vec![0.0; x.len()]);
        let (el, er) = (energy(&l), energy(&r));
        assert!(er > 0.05 * el, "{token}: left in, right out {er} vs {el}");
        let corr: f32 = l.iter().zip(&r).map(|(a, b)| a * b).sum::<f32>() / (el * er).sqrt();
        assert!(corr.abs() < 0.7, "{token}: left and right nearly the same (correlation {corr})");
        let late = energy(&l[secs(0.4)..secs(0.6)]) + energy(&r[secs(0.4)..secs(0.6)]);
        assert!(late > 1e-6 * (el + er), "{token}: no tail 0.4 s after the impulse");
    }
}

#[test]
fn reverse_rise_and_gated_attack_shape_the_envelope() {
    let ms = |t: f32| secs(t / 1000.0);
    // REVERSE: RISE sets how long the swell takes to reach its peak.
    let peak_at = |rise: f32| {
        let mut rig = Rig::new("REVERSE", &[("pdel", 0.0), ("dcy", 0.8), ("rise", rise)]);
        let out = rig.mono(&impulse(ms(1500.0)));
        let e: Vec<f32> = out.chunks(ms(20.0)).map(energy).collect();
        e.iter().enumerate().max_by(|a, b| a.1.total_cmp(b.1)).unwrap().0
    };
    assert!(peak_at(50.0) != peak_at(0.0), "RISE doesn't move the swell's peak");
    // GATED: ATTACK lets the reverb build fast (high) or slowly (low).
    let early = |att: f32| {
        let mut rig = Rig::new("GATED", &[("pdel", 0.0), ("dcy", 0.6), ("att", att)]);
        let out = rig.mono(&impulse(ms(800.0)));
        energy(&out[..ms(40.0)]) / energy(&out[..ms(500.0)])
    };
    let (slow, fast) = (early(0.0), early(30.0));
    assert!((fast / slow - 1.0).abs() > 0.2, "GATED ATTACK: early share {slow} (0) vs {fast} (30)");
}

#[test]
fn freeze_add_lets_new_sound_in() {
    // V-ROOM FREEZE holds the tail and ignores the input; with ADD the input
    // joins the frozen pattern.
    let tone_in = sine(440.0, 0.3, secs(1.0));
    let held = |add: f32| {
        let mut rig = Rig::new("V-ROOM", &[("dcy", 1.0), ("frz", 1.0), ("add", add)]);
        rig.mono(&tone_in);
        let after = rig.mono(&vec![0.0; secs(1.0)]);
        tone(&after[secs(0.5)..], 440.0)
    };
    let (without, with) = (held(0.0), held(1.0));
    assert!(with > 10.0 * without.max(1e-6), "FREEZE+ADD keeps the new tone ({with}), FREEZE alone ignores it ({without})");
}

// ---- Delays -------------------------------------------------------------------------

/// The positions of the first `n` echoes of an impulse (peaks at least 20 ms
/// apart above 5 % of the largest).
fn echoes(x: &[f32], n: usize) -> Vec<usize> {
    let p = peak(x);
    let mut out = Vec::new();
    let mut i = 1;
    while i < x.len() && out.len() < n {
        if x[i].abs() > 0.05 * p {
            let k = argmax(x, i, i + secs(0.01));
            out.push(k);
            i = k + secs(0.02);
        } else {
            i += 1;
        }
    }
    out
}

#[test]
fn delay_times_are_exact() {
    let mut bad = Vec::new();
    let mut check = |what: String, got: Option<usize>, want_ms: f32| {
        let want = want_ms * SR / 1000.0;
        match got {
            Some(got) if (got as f32 - want).abs() <= 0.0015 * SR + 2.0 => {}
            Some(got) => bad.push(format!("{what}: {:.1} ms, want {want_ms} ms", got as f32 / SR * 1000.0)),
            None => bad.push(format!("{what}: no echo")),
        }
    };
    let open = [("feed", 0.0), ("lc", 20.0), ("hc", 20_000.0)];
    // ST-DL: TIME x FACTOR, PATTERN per side, OFFSET on the right.
    for (time, fact, factor) in [(333.0, 5.0, 1.0), (500.0, 2.0, 0.5), (240.0, 10.0, 2.0), (400.0, 6.0, 0.375)] {
        let mut rig = Rig::new("ST-DL", &open);
        rig.set(&[("time", time), ("fact", fact)]);
        let (l, _) = rig.run(&impulse(secs(1.5)), &impulse(secs(1.5)));
        check(format!("ST-DL TIME {time} FACTOR {factor}"), echoes(&l, 1).first().copied(), time * factor);
    }
    let mut rig = Rig::new("ST-DL", &open);
    rig.set(&[("time", 400.0), ("fact", 5.0), ("pat", 0.0)]);
    let (l, r) = rig.run(&impulse(secs(1.0)), &impulse(secs(1.0)));
    check("ST-DL PATTERN 1/2:1 left".into(), echoes(&l, 1).first().copied(), 200.0);
    check("ST-DL PATTERN 1/2:1 right".into(), echoes(&r, 1).first().copied(), 400.0);
    let mut rig = Rig::new("ST-DL", &open);
    rig.set(&[("time", 300.0), ("fact", 5.0), ("offs", 40.0)]);
    let (_, r) = rig.run(&impulse(secs(1.0)), &impulse(secs(1.0)));
    check("ST-DL OFFSET +40".into(), echoes(&r, 1).first().copied(), 340.0);
    // TAPE-DL TIME (FLUTTER off), DEL/REV TIME.
    for time in [60.0, 300.0, 650.0] {
        let mut rig = Rig::new("TAPE-DL", &[("time", time), ("wf", 0.0), ("sust", 0.0)]);
        let out = rig.mono(&impulse(secs(1.0)));
        check(format!("TAPE-DL TIME {time}"), echoes(&out, 1).first().copied(), time);
    }
    for time in [50.0, 240.0, 1200.0] {
        let mut rig = Rig::new("DEL/REV", &[("time", time), ("feed", 0.0), ("dly", 100.0), ("d2r", 0.0), ("i2r", 0.0)]);
        let out = rig.mono(&impulse(secs(1.5)));
        check(format!("DEL/REV TIME {time}"), echoes(&out, 1).first().copied(), time);
    }
    // OILCAN 0..10 is 60-480 ms (log), BBD 0..100 is 20-600 ms (exponential).
    for (dial, want) in [(0.0, 60.0), (10.0, 480.0)] {
        let mut rig = Rig::new("OILCAN", &[("time", dial), ("wb", 0.0), ("sust", 0.0)]);
        let out = rig.mono(&impulse(secs(1.0)));
        check(format!("OILCAN TIME {dial}"), echoes(&out, 1).first().copied(), want);
    }
    for (dial, want) in [(0.0, 20.0), (100.0, 600.0)] {
        let mut rig = Rig::new("BBD-DL", &[("dly", dial), ("fb", 0.0)]);
        let out = rig.mono(&impulse(secs(1.0)));
        check(format!("BBD-DL DELAY {dial}"), echoes(&out, 1).first().copied(), want);
    }
    // TAP-DL: PRE DLY, then REPEATS taps spread over TIME x FACTOR.
    let mut rig =
        Rig::new("TAP-DL", &[("time", 400.0), ("rep", 4.0), ("pdel", 100.0), ("slp", 0.0), ("diff", 0.0), ("wid", 0.0)]);
    let out = rig.mono(&impulse(secs(1.0)));
    let taps = echoes(&out, 6);
    assert_eq!(taps.len(), 4, "TAP-DL REPEATS 4: {taps:?}");
    for (k, &t) in taps.iter().enumerate() {
        check(format!("TAP-DL tap {}", k + 1), Some(t), 100.0 + 400.0 * (k + 1) as f32 / 4.0);
    }
    assert!(bad.is_empty(), "{bad:?}");
}

#[test]
fn feedback_sets_the_repeats() {
    // Each repeat is FEED times the last (ST-DL, through open loop filters),
    // SUSTAIN / FEED lengthen the tape, oil can and BBD repeats, and TAP-DL's
    // SLOPE tilts its taps by SLOPE dB from first to last.
    let mut rig = Rig::new(
        "ST-DL",
        &[("time", 200.0), ("fact", 5.0), ("feed", 50.0), ("flc", 20.0), ("fhc", 20_000.0), ("lc", 20.0), ("hc", 20_000.0)],
    );
    let (l, _) = rig.run(&impulse(secs(1.0)), &impulse(secs(1.0)));
    let e = echoes(&l, 3);
    let ratio = l[e[1]].abs() / l[e[0]].abs();
    assert!((ratio - 0.5).abs() < 0.08, "ST-DL FEED 50 %: second echo {ratio} of the first");
    for (token, id, time, lo, hi) in [
        ("TAPE-DL", "sust", ("time", 200.0), 10.0, 70.0),
        ("OILCAN", "sust", ("time", 3.0), 1.0, 7.0),
        ("BBD-DL", "fb", ("dly", 40.0), 10.0, 70.0),
    ] {
        let tail = |v: f32| {
            let mut rig = Rig::new(token, &[time, (id, v)]);
            let out = rig.mono(&impulse(secs(3.0)));
            energy(&out[secs(1.0)..])
        };
        let (short, long) = (tail(lo), tail(hi));
        assert!(long > 20.0 * short.max(1e-12), "{token}: more {id} gives longer repeats ({short} -> {long})");
    }
    let mut rig =
        Rig::new("TAP-DL", &[("time", 400.0), ("rep", 4.0), ("pdel", 0.0), ("slp", -12.0), ("diff", 0.0), ("wid", 0.0)]);
    let out = rig.mono(&impulse(secs(1.0)));
    let e = echoes(&out, 4);
    let db = 20.0 * (out[e[3]].abs() / out[e[0]].abs()).log10();
    assert!((db + 12.0).abs() < 1.0, "TAP-DL SLOPE -12 dB: last tap {db:.1} dB against the first");
}

#[test]
fn delay_modes_route_the_feedback() {
    // ST-DL with a left-only impulse: ST keeps the repeats left, X (cross)
    // ping-pongs them to the right, M (mono) puts them on both.
    let run = |mode: f32| {
        let mut rig = Rig::new(
            "ST-DL",
            &[("mode", mode), ("time", 150.0), ("fact", 5.0), ("feed", 60.0), ("flc", 20.0), ("fhc", 20_000.0)],
        );
        let x = impulse(secs(1.0));
        rig.run(&x, &vec![0.0; x.len()])
    };
    let (_, r_st) = run(0.0);
    let (_, r_x) = run(1.0);
    let (l_m, r_m) = run(2.0);
    assert!(energy(&r_st) < 1e-9, "ST: the right side stays silent");
    let first_right = echoes(&r_x, 1);
    assert!(
        first_right.first().is_some_and(|&t| (t as f32 - 0.3 * SR).abs() < 0.002 * SR),
        "X: the second repeat comes back right ({first_right:?})"
    );
    assert!(
        energy(&r_m) > 0.03 * energy(&l_m) && echoes(&r_m, 1).first().is_some_and(|&t| (t as f32 - 0.3 * SR).abs() < 0.002 * SR),
        "M: the repeats come back on both sides"
    );
    // TAP-DL: JUMP alternates hard left and right, MOVE pans across.
    let pans = |mode: f32| {
        let mut rig =
            Rig::new("TAP-DL", &[("mode", mode), ("rep", 4.0), ("wid", 100.0), ("slp", 0.0), ("pdel", 0.0), ("diff", 0.0)]);
        let x = impulse(secs(1.0));
        let (l, r) = rig.run(&x, &x);
        let both: Vec<f32> = l.iter().zip(&r).map(|(a, b)| a.abs() + b.abs()).collect();
        echoes(&both, 4).iter().map(|&k| (r[k].abs() - l[k].abs()) / (r[k].abs() + l[k].abs())).collect::<Vec<f32>>()
    };
    let jump = pans(1.0);
    assert!(
        jump.len() == 4 && jump.windows(2).all(|w| w[0] * w[1] < 0.0) && jump.iter().all(|p| p.abs() > 0.9),
        "JUMP: {jump:?}"
    );
    let mv = pans(0.0);
    assert!(mv.len() == 4 && mv.windows(2).all(|w| w[1] > w[0]) && mv[0] < -0.9 && mv[3] > 0.9, "MOVE: {mv:?}");
}

// ---- Modulation ---------------------------------------------------------------------

/// The strongest rate (Hz, between `lo` and `hi`) at which `series` (sampled
/// at `rate` Hz) moves, after removing its mean.
fn series_rate(series: &[f32], rate: f32, lo: f32, hi: f32) -> f32 {
    let mean = series.iter().sum::<f32>() / series.len().max(1) as f32;
    let x: Vec<f64> = series.iter().map(|v| f64::from(v - mean)).collect();
    let power = |hz: f32| {
        let w = std::f64::consts::TAU * f64::from(hz) / f64::from(rate);
        let (mut re, mut im) = (0.0, 0.0);
        for (k, v) in x.iter().enumerate() {
            re += v * (w * k as f64).cos();
            im += v * (w * k as f64).sin();
        }
        re * re + im * im
    };
    let mut best = (lo, 0.0);
    let steps = 600;
    for i in 0..=steps {
        let hz = lo * (hi / lo).powf(i as f32 / steps as f32);
        let p = power(hz);
        if p > best.1 {
            best = (hz, p);
        }
    }
    best.0
}

/// The level of `x` in 5 ms steps (a series at 200 Hz).
fn envelope(x: &[f32]) -> Vec<f32> {
    x.chunks(secs(0.005)).map(rms).collect()
}

/// The instantaneous frequency of `x` (from its rising zero crossings),
/// averaged into a series at 200 Hz.
fn frequency_track(x: &[f32]) -> Vec<f32> {
    let step = secs(0.005);
    let mut out = vec![0.0f32; x.len() / step];
    let mut count = vec![0u32; out.len()];
    let mut last: Option<f32> = None;
    for i in 1..x.len() {
        if x[i - 1] < 0.0 && x[i] >= 0.0 {
            let t = (i - 1) as f32 + x[i - 1] / (x[i - 1] - x[i]);
            if let Some(prev) = last {
                let k = (t as usize / step).min(out.len().saturating_sub(1));
                out[k] += SR / (t - prev);
                count[k] += 1;
            }
            last = Some(t);
        }
    }
    let mut held = 0.0;
    out.iter()
        .zip(&count)
        .map(|(f, &c)| {
            if c > 0 {
                held = f / c as f32;
            }
            held
        })
        .collect()
}

#[test]
fn lfo_rates_are_their_speed() {
    // The modulation of a steady tone moves at SPEED: as pitch wobble for
    // the delay-line devices and the phaser, as level for the tremolo and the
    // filter, at the rotor speed for ROTARY.
    let input = sine(1000.0, 0.4, secs(8.0));
    let mut bad = Vec::new();
    let mut check = |what: String, got: f32, want: f32| {
        if (got / want - 1.0).abs() > 0.04 {
            bad.push(format!("{what}: moves at {got:.3} Hz, want {want} Hz"));
        }
    };
    for spd in [0.3, 1.0, 3.0] {
        let mut rig = Rig::new("CHORUS", &[("spd", spd), ("mix", 100.0), ("wave", 0.0), ("depl", 60.0)]);
        let (l, _) = rig.run(&input, &input);
        check(format!("CHORUS SPEED {spd}"), series_rate(&frequency_track(&l[secs(1.0)..]), 200.0, 0.1, 8.0), spd);
        let mut rig = Rig::new("FLANGER", &[("spd", spd), ("mix", 100.0), ("feed", 0.0)]);
        let (l, _) = rig.run(&input, &input);
        check(format!("FLANGER SPEED {spd}"), series_rate(&frequency_track(&l[secs(1.0)..]), 200.0, 0.1, 8.0), spd);
        let mut rig =
            Rig::new("PHASER", &[("spd", spd), ("mix", 100.0), ("reso", 0.0), ("wave", 0.0), ("depth", 80.0), ("range", 50.0)]);
        let (l, _) = rig.run(&input, &input);
        check(format!("PHASER SPEED {spd}"), series_rate(&frequency_track(&l[secs(1.0)..]), 200.0, 0.1, 8.0), spd);
        let mut rig = Rig::new("PANNER", &[("spd", spd), ("phase", 0.0), ("depth", 100.0), ("wave", 0.0)]);
        let (l, _) = rig.run(&input, &input);
        check(format!("PANNER SPEED {spd}"), series_rate(&envelope(&l[secs(1.0)..]), 200.0, 0.1, 8.0), spd);
        // A low pass below the tone: the level rises and falls once per cycle.
        let mut rig =
            Rig::new("MOOD", &[("spd", spd), ("wave", 1.0), ("lfo", 60.0), ("fbase", 600.0), ("filt", 0.0), ("env", 0.0)]);
        let (l, _) = rig.run(&input, &input);
        check(format!("MOOD SPEED {spd}"), series_rate(&envelope(&l[secs(1.0)..]), 200.0, 0.1, 8.0), spd);
    }
    // ROTARY: the horn turns at SLOW / FAST RATE once it has spun up.
    for (sw, id, hz) in [(1.0, "lo", 0.8), (2.0, "hi", 6.0)] {
        let mut rig = Rig::new("ROTARY", &[("sw", sw), (id, hz), ("bal", 100.0), ("mix", 100.0), ("hac", 100.0)]);
        let (l, _) = rig.run(&input, &input);
        check(format!("ROTARY {id} {hz}"), series_rate(&envelope(&l[secs(3.0)..]), 200.0, 0.2, 12.0), hz);
    }
    // DIMCRS: buttons 1 and 4 alone: 0.25 and 0.6 Hz.
    for (sw, hz) in [("sw1", 0.25), ("sw4", 0.6)] {
        let mut rig = Rig::new("DIMCRS", &[("sw1", 0.0), ("sw2", 0.0), ("sw3", 0.0), ("sw4", 0.0), ("drysw", 0.0)]);
        rig.set(&[(sw, 1.0)]);
        let (l, _) = rig.run(&input, &input);
        check(format!("DIMCRS {sw}"), series_rate(&frequency_track(&l[secs(1.0)..]), 200.0, 0.1, 4.0), hz);
    }
    assert!(bad.is_empty(), "{bad:?}");
}

#[test]
fn depth_zero_stops_the_modulation() {
    let input = sine(1000.0, 0.4, secs(3.0));
    let wobble = |token: &str, set: &[(&str, f32)]| {
        let mut rig = Rig::new(token, set);
        let (l, _) = rig.run(&input, &input);
        let f = frequency_track(&l[secs(0.5)..]);
        let e = envelope(&l[secs(0.5)..]);
        let spread = |v: &[f32]| {
            let (lo, hi) = v.iter().fold((f32::MAX, f32::MIN), |(a, b), &x| (a.min(x), b.max(x)));
            (hi - lo) / hi.abs().max(1e-9)
        };
        spread(&f) + spread(&e)
    };
    for (token, depth_ids, extra) in [
        ("CHORUS", &["depl", "depr"][..], &[("mix", 100.0)][..]),
        ("FLANGER", &["depl", "depr"][..], &[("mix", 100.0), ("feed", 0.0)][..]),
        ("PHASER", &["depth"][..], &[("mix", 100.0)][..]),
        ("PANNER", &["depth"][..], &[][..]),
        ("MOOD", &["lfo"][..], &[("fbase", 1000.0), ("filt", 2.0)][..]),
    ] {
        let mut on: Vec<(&str, f32)> = extra.to_vec();
        let mut off = on.clone();
        for id in depth_ids {
            on.push((id, 80.0));
            off.push((id, 0.0));
        }
        let (a, b) = (wobble(token, &on), wobble(token, &off));
        assert!(b < 0.01 && a > 5.0 * b.max(1e-4), "{token}: modulation at depth 80 {a}, at 0 {b}");
    }
}

#[test]
fn panner_pans_and_lfos_offset_the_sides() {
    // TREMOLO/PANNER PHASE 180: the sides move in opposite directions; the
    // needle readout follows; CHORUS PHASE offsets the right LFO.
    let input = sine(1000.0, 0.4, secs(2.0));
    let mut rig = Rig::new("PANNER", &[("phase", 180.0), ("depth", 100.0), ("spd", 2.0)]);
    let (l, r) = rig.run(&input, &input);
    let (el, er) = (envelope(&l), envelope(&r));
    let corr: f32 = {
        let ml = el.iter().sum::<f32>() / el.len() as f32;
        let mr = er.iter().sum::<f32>() / er.len() as f32;
        let num: f32 = el.iter().zip(&er).map(|(a, b)| (a - ml) * (b - mr)).sum();
        let den = (el.iter().map(|a| (a - ml).powi(2)).sum::<f32>() * er.iter().map(|b| (b - mr).powi(2)).sum::<f32>()).sqrt();
        num / den
    };
    assert!(corr < -0.9, "auto-pan: the sides' levels correlate {corr}");
    let mut needles = Vec::new();
    for _ in 0..50 {
        rig.run(&input[..secs(0.01)], &input[..secs(0.01)]);
        needles.push(rig.live(FxLive::Needle));
    }
    assert!(needles.iter().any(|&n| n < -0.8) && needles.iter().any(|&n| n > 0.8), "needle swing {needles:?}");
}

// ---- Pitch --------------------------------------------------------------------------

#[test]
fn pitch_shifts_land_on_the_interval() {
    let input = sine(440.0, 0.4, secs(1.5));
    let mut bad = Vec::new();
    for (semi, cent) in [(-12.0, 0.0), (-5.0, 0.0), (7.0, 0.0), (12.0, 0.0), (0.0, 50.0), (3.0, -25.0)] {
        let mut rig = Rig::new("PITCH", &[("semi", semi), ("cent", cent), ("mix", 100.0), ("dly", 0.0)]);
        let out = rig.mono(&input);
        let want = 440.0 * 2f32.powf((semi + cent / 100.0) / 12.0);
        let got = dominant(&out[secs(0.5)..], want * 0.9, want * 1.1);
        if (got / want - 1.0).abs() > 0.003 {
            bad.push(format!("PITCH {semi} st {cent} c: {got:.1} Hz, want {want:.1}"));
        }
    }
    // D-PITCH: voice 1 a fifth up hard left, voice 2 an octave down hard right.
    let mut rig = Rig::new(
        "D-PITCH",
        &[("semi1", 7.0), ("cent1", 0.0), ("pan1", -100.0), ("semi2", -12.0), ("cent2", 0.0), ("pan2", 100.0)],
    );
    let (l, r) = rig.run(&input, &input);
    let (fl, fr) = (dominant(&l[secs(0.5)..], 200.0, 800.0), dominant(&r[secs(0.5)..], 150.0, 800.0));
    if (fl / 659.26 - 1.0).abs() > 0.003 || (fr / 220.0 - 1.0).abs() > 0.003 {
        bad.push(format!("D-PITCH: left {fl:.1} Hz (want 659.3), right {fr:.1} Hz (want 220)"));
    }
    assert!(bad.is_empty(), "{bad:?}");
}

#[test]
fn pitch_fix_corrects_to_the_scale() {
    // A4 + 40 cents: pulled to 440 Hz; with the A key on (A disabled) it stays;
    // A4 PITCH moves the target.
    let hz = 440.0 * 2f32.powf(0.4 / 12.0);
    let input = sine(hz, 0.4, secs(2.0));
    let out_hz = |set: &[(&str, f32)]| {
        let mut rig = Rig::new("PCORR", &[("amnt", 100.0), ("spd", 100.0)]);
        rig.set(set);
        let out = rig.mono(&input);
        (dominant(&out[secs(1.0)..], 400.0, 500.0), rig.live(FxLive::Note), rig.live(FxLive::Needle))
    };
    let (fixed, note, cents) = out_hz(&[]);
    assert!((fixed / 440.0 - 1.0).abs() < 0.004, "corrected to {fixed} Hz");
    assert_eq!(note, 9.0, "the note heard is A");
    assert!((cents + 40.0).abs() < 6.0, "correction {cents} cents");
    let (kept, _, _) = out_hz(&[("sw_a", 1.0)]);
    assert!((kept / hz - 1.0).abs() < 0.004, "A disabled: {kept} Hz, want {hz}");
    let (moved, _, _) = out_hz(&[("a4", 445.0)]);
    assert!((moved / 445.0 - 1.0).abs() < 0.004, "A4 PITCH 445: {moved} Hz");
    // AMOUNT 50: half way.
    let (half, _, _) = out_hz(&[("amnt", 50.0)]);
    let want = 440.0 * 2f32.powf(0.2 / 12.0);
    assert!((half / want - 1.0).abs() < 0.004, "AMOUNT 50: {half} Hz, want {want}");
}

#[test]
fn octavers_add_the_octaves_below() {
    let input = sine(220.0, 0.4, secs(1.5));
    let level = |set: &[(&str, f32)], hz: f32| {
        let mut rig = Rig::new("SUB", set);
        let out = rig.mono(&input);
        tone(&out[secs(0.5)..], hz)
    };
    let one = [("oct1", 100.0), ("oct2", 0.0)];
    let two = [("oct1", 0.0), ("oct2", 100.0)];
    assert!(level(&one, 110.0) > 10.0 * level(&two, 110.0).max(1e-5), "OCTAVE 1 makes 110 Hz");
    assert!(level(&two, 55.0) > 10.0 * level(&one, 55.0).max(1e-5), "OCTAVE 2 makes 55 Hz");
    // SUB MONSTER: band 3 (FREQ) is made from the content an octave above.
    let input = sine(100.0, 0.4, secs(1.5));
    let band3 = |v: f32| {
        let mut rig = Rig::new(
            "SUB-M",
            &[("freq", 50.0), ("mix", 100.0), ("bd1", 0.0), ("bd2", 0.0), ("bd4", 0.0), ("bd5", 0.0), ("bd3", v)],
        );
        let out = rig.mono(&input);
        (tone(&out[secs(0.5)..], 50.0), rig.live(FxLive::Band3))
    };
    let ((on, live_on), (off, live_off)) = (band3(100.0), band3(0.0));
    assert!(on > 10.0 * off.max(1e-5), "BAND 3 at 100 % adds 50 Hz ({on} vs {off})");
    assert!(live_on > 0.01 && live_off < 1e-4, "band 3 readout {live_on} / {live_off}");
}

#[test]
fn psycho_bass_adds_harmonics_and_solo_keeps_the_lows() {
    let input = sine(50.0, 0.4, secs(1.5));
    let run = |set: &[(&str, f32)]| {
        let mut rig = Rig::new("P-BASS", set);
        rig.mono(&input)[secs(0.5)..].to_vec()
    };
    let quiet = run(&[("int", -24.0)]);
    let loud = run(&[("int", 6.0)]);
    let harm = |x: &[f32]| tone(x, 100.0) + tone(x, 150.0) + tone(x, 200.0);
    assert!(harm(&loud) > 4.0 * harm(&quiet), "INTENSITY adds harmonics");
    let solo = run(&[("int", 6.0), ("solo", 1.0), ("xf", 100.0)]);
    assert!(harm(&solo) < 0.1 * harm(&loud), "SOLO: only the band below X/O");
}

// ---- Amps ---------------------------------------------------------------------------

/// Harmonic distortion of a 220 Hz tone: harmonics 2-8 against the fundamental.
fn thd(x: &[f32]) -> f32 {
    let h: f32 = (2..=8).map(|k| tone(x, 220.0 * k as f32).powi(2)).sum::<f32>().sqrt();
    h / tone(x, 220.0).max(1e-9)
}

#[test]
fn amps_respond_like_amps() {
    let input = sine(220.0, 0.25, secs(1.0));
    for (token, gain) in [("RACKAMP", "pre"), ("UKROCK", "gain"), ("ANGEL", "gain"), ("JAZZC", "vol"), ("DELUXE", "vol")] {
        let spec = &FX_MODELS[model_index(token).unwrap()];
        let lo = spec.params[spec.param_index(gain).unwrap()].lo();
        let run = |set: &[(&str, f32)]| {
            let mut rig = Rig::new(token, set);
            rig.mono(&input)[secs(0.4)..].to_vec()
        };
        // More GAIN / VOLUME: more distortion.
        let (clean, dirty) = (thd(&run(&[(gain, lo + 1.0)])), thd(&run(&[(gain, 10.0)])));
        assert!(dirty > 1.5 * clean, "{token}: {gain} distortion {clean} -> {dirty}");
        // OUT GAIN: louder.
        let (soft, loud) = (rms(&run(&[("out", lo + 1.0)])), rms(&run(&[("out", 10.0)])));
        assert!(loud > 2.0 * soft, "{token}: OUT GAIN {soft} -> {loud}");
        // BASS and TREBLE tilt the tone (a quiet, clean signal).
        let tones: Vec<f32> =
            sine(100.0, 0.02, secs(1.0)).iter().zip(sine(3000.0, 0.02, secs(1.0))).map(|(a, b)| a + b).collect();
        let tilt = |set: &[(&str, f32)]| {
            let mut rig = Rig::new(token, &[(gain, lo + 1.0)]);
            rig.set(set);
            let out = rig.mono(&tones);
            (tone(&out[secs(0.4)..], 100.0), tone(&out[secs(0.4)..], 3000.0))
        };
        let bass_id = if spec.param_index("bass").is_some() { "bass" } else { "leq" };
        let treb_id = if spec.param_index("treb").is_some() { "treb" } else { "heq" };
        let (b_lo, b_hi) = (tilt(&[(bass_id, lo)]).0, tilt(&[(bass_id, 10.0)]).0);
        assert!(b_hi > 1.4 * b_lo, "{token}: {bass_id} {b_lo} -> {b_hi} at 100 Hz");
        let (t_lo, t_hi) = (tilt(&[(treb_id, lo)]).1, tilt(&[(treb_id, 10.0)]).1);
        assert!(t_hi > 1.4 * t_lo, "{token}: {treb_id} {t_lo} -> {t_hi} at 3 kHz");
        // CAB SIM: the speaker rolls the top off (8 kHz against 220 Hz, clean).
        let fizz = |cab: f32| {
            let x: Vec<f32> =
                sine(220.0, 0.02, secs(1.0)).iter().zip(sine(8000.0, 0.02, secs(1.0))).map(|(a, b)| a + b).collect();
            let mut rig = Rig::new(token, &[("cab", cab), (gain, lo + 1.0)]);
            let out = rig.mono(&x);
            20.0 * (tone(&out[secs(0.4)..], 8000.0) / tone(&out[secs(0.4)..], 220.0)).log10()
        };
        let (open, cab) = (fizz(0.0), fizz(1.0));
        assert!(open > cab + 6.0, "{token}: the cabinet doesn't roll off the top ({open:.1} dB -> {cab:.1} dB)");
    }
}

// ---- Every device -------------------------------------------------------------------

#[test]
fn mix_zero_is_the_dry_signal() {
    // Devices with their own MIX: at 0 the input comes through untouched.
    let input = noise(9, 0.3, secs(0.5));
    for token in ["CHORUS", "FLANGER", "PHASER", "ROTARY", "MOOD", "PITCH", "DOUBLE", "SUB-M"] {
        let mut rig = Rig::new(token, &[("mix", 0.0)]);
        let out = rig.mono(&input);
        let diff: f32 = out.iter().zip(&input).skip(secs(0.1)).map(|(a, b)| (a - b) * (a - b)).sum();
        assert!(diff < 1e-3 * energy(&input[secs(0.1)..]), "{token}: MIX 0 isn't the dry signal ({diff})");
    }
}

#[test]
fn no_clicks_when_controls_jump() {
    // A steady tone, then a control jumps from a quarter to near the end of
    // its travel: what appears above 6 kHz in the moment of the jump may not
    // exceed what either setting makes on its own (smoothing, crossfades).
    let input = sine(220.0, 0.3, secs(1.2));
    let hf_peak = |x: &[f32]| {
        let mut hp = crate::biquad::Biquad::default();
        hp.set_coefficients(crate::biquad::BiquadCoeffs::high_pass(f64::from(SR), 6000.0, 0.707));
        x.iter().map(|&v| hp.tick(v, 0).abs()).skip(64).fold(0.0f32, f32::max)
    };
    let mut bad = Vec::new();
    for token in AUDITED {
        let spec = &FX_MODELS[model_index(token).unwrap()];
        for p in spec.params.iter().filter(|p| p.is_continuous() || matches!(p.kind, ParamKind::Int)) {
            if exempt(token, p.id) {
                continue;
            }
            let (a, b) = (p.from_normalised(0.25), p.from_normalised(0.85));
            let steady = |v: f32| {
                let mut rig = Rig::new(token, &[(p.id, v)]);
                rig.mono(&input);
                let out = rig.mono(&input);
                (hf_peak(&out[secs(0.2)..]), peak(&out[secs(0.2)..]))
            };
            let (hf_a, lvl_a) = steady(a);
            let (hf_b, lvl_b) = steady(b);
            let mut rig = Rig::new(token, &[(p.id, a)]);
            rig.mono(&input);
            rig.set(&[(p.id, b)]);
            let jump = hf_peak(&rig.mono(&input[..secs(0.5)]));
            let allowed = 2.0 * hf_a.max(hf_b) + 0.005 * lvl_a.max(lvl_b).max(0.05);
            if jump > allowed {
                bad.push(format!("{token} {} {a:.2} -> {b:.2}: {jump:.4} above 6 kHz (steady {hf_a:.4} / {hf_b:.4})", p.id));
            }
        }
    }
    assert!(bad.is_empty(), "clicks: {bad:?}");
}

#[test]
fn tails_end_cleanly() {
    // After a burst and a long silence every device is silent without
    // denormal numbers in its output, and (release builds) silence costs no
    // more than sound does.
    let mut slow = Vec::new();
    for token in AUDITED {
        let mut rig = Rig::new(token, &[]);
        rig.mono(&noise(3, 0.5, secs(0.3)));
        let silence = vec![0.0; secs(30.0)];
        let (l, r) = rig.run(&silence, &silence);
        let out: Vec<f32> = l[secs(29.0)..].iter().chain(&r[secs(29.0)..]).copied().collect();
        let tail = &out[..];
        assert!(peak(tail) < 1e-5, "{token}: still {} after 30 s", peak(tail));
        if !tail.iter().all(|v| *v == 0.0 || v.abs() >= f32::MIN_POSITIVE) {
            slow.push(format!("{token}: denormals in the tail"));
        }
        if !cfg!(debug_assertions) {
            let t = std::time::Instant::now();
            rig.mono(&vec![0.0; secs(5.0)]);
            let quiet = t.elapsed();
            let busy_in = noise(4, 0.3, secs(5.0));
            let t = std::time::Instant::now();
            rig.mono(&busy_in);
            let busy = t.elapsed();
            if quiet > busy * 3 + std::time::Duration::from_millis(10) {
                slow.push(format!("{token}: silence costs {quiet:?}, sound {busy:?}"));
            }
        }
    }
    assert!(slow.is_empty(), "denormal slowdowns: {slow:?}");
}
