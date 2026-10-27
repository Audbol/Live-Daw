//! WING EQ models: the 6-band (channels) / 8-band (buses) WING EQ and the
//! analog-style models (SOUL, E88, E84, F110, PULSAR, MACH4, PIA).
//!
//! A model's controls are turned into up to [`MAX_SECTIONS`] biquads plus an
//! output gain. The console's MIX control (0..125 %) scales every band gain,
//! so 100 % is the EQ as set, lower values soften it and higher values
//! intensify it. Curves, ranges and switch meanings follow the WING
//! operation manual (section 11.1; page numbers in the comments below).

use super::{dial_log, ModelSpec, ParamKind, Params, MAX_EQ_PARAMS};
use crate::biquad::{Biquad, BiquadCoeffs};
use crate::util::db_to_gain;
use std::f64::consts::FRAC_1_SQRT_2;

pub const MAX_SECTIONS: usize = 16;
const SUB_BLOCK: usize = 32;

#[derive(Clone, Copy, Debug)]
pub struct EqDesign {
    pub sections: [BiquadCoeffs; MAX_SECTIONS],
    pub len: usize,
    pub out_gain_db: f32,
}

impl Default for EqDesign {
    fn default() -> Self {
        Self { sections: [BiquadCoeffs::identity(); MAX_SECTIONS], len: 0, out_gain_db: 0.0 }
    }
}

impl EqDesign {
    fn push(&mut self, c: BiquadCoeffs) {
        if self.len < MAX_SECTIONS {
            self.sections[self.len] = c;
            self.len += 1;
        }
    }

    /// Combined response in dB at `freq`.
    pub fn response_db(&self, freq: f64, sample_rate: f64) -> f64 {
        let mut mag = 1.0;
        for c in &self.sections[..self.len] {
            mag *= c.magnitude_at(freq, sample_rate);
        }
        20.0 * mag.max(1e-9).log10() + self.out_gain_db as f64
    }
}

/// Designs the filters for `model` with parameter `values`. `gain_scale` is MIX / 100.
pub fn design(spec: &ModelSpec, values: &[f32], sr: f64, gain_scale: f32) -> EqDesign {
    let p = Params::new(spec, values);
    let k = gain_scale.clamp(0.0, 1.25);
    let mut d = EqDesign::default();
    match spec.token {
        "STD" => design_std(&p, sr, k, &mut d),
        "SOUL" => design_soul(&p, sr, k, &mut d),
        "E88" => design_e88(&p, sr, k, &mut d),
        "E84" => design_e84(&p, sr, k, &mut d),
        "F110" => design_f110(&p, sr, k, &mut d),
        "PULSAR" => design_pulsar(&p, sr, k, &mut d),
        "MACH4" => design_mach4(&p, sr, k, &mut d),
        "PIA" => design_pia(&p, sr, k, &mut d),
        _ => {}
    }
    d
}

fn peak(sr: f64, f: f32, q: f32, g: f32) -> BiquadCoeffs {
    BiquadCoeffs::peak(sr, f as f64, q as f64, g as f64)
}
fn low_shelf(sr: f64, f: f32, q: f32, g: f32) -> BiquadCoeffs {
    BiquadCoeffs::low_shelf(sr, f as f64, q as f64, g as f64)
}
fn high_shelf(sr: f64, f: f32, q: f32, g: f32) -> BiquadCoeffs {
    BiquadCoeffs::high_shelf(sr, f as f64, q as f64, g as f64)
}

/// Adds a band only when it does something (keeps the section count low).
fn push_gain(d: &mut EqDesign, g: f32, c: impl FnOnce() -> BiquadCoeffs) {
    if g.abs() > 1e-3 {
        d.push(c());
    }
}

/// High/low-pass filter families of the bus EQ outer bands.
fn push_cut(d: &mut EqDesign, sr: f64, kind: &str, f: f32, q: f32, high_pass: bool) {
    let f = f as f64;
    // Bessel sections are frequency-scaled so the -3 dB point lands on `f`.
    let sec = |d: &mut EqDesign, q: f64, scale: f64| {
        let fs = if high_pass { f / scale } else { f * scale };
        d.push(if high_pass { BiquadCoeffs::high_pass(sr, fs, q) } else { BiquadCoeffs::low_pass(sr, fs, q) });
    };
    let first = |d: &mut EqDesign| {
        d.push(if high_pass { BiquadCoeffs::high_pass_1(sr, f) } else { BiquadCoeffs::low_pass_1(sr, f) });
    };
    match kind {
        "CUT" => sec(d, q as f64, 1.0),
        "BW6" => first(d),
        "BW12" => sec(d, FRAC_1_SQRT_2, 1.0),
        "BS12" => sec(d, 0.5773, 1.2736),
        "LR12" => sec(d, 0.5, 1.0),
        "BW18" => {
            first(d);
            sec(d, 1.0, 1.0);
        }
        "BW24" => {
            sec(d, 0.5412, 1.0);
            sec(d, 1.3066, 1.0);
        }
        "BS24" => {
            sec(d, 0.5219, 1.4192);
            sec(d, 0.8055, 1.5912);
        }
        "LR24" => {
            sec(d, FRAC_1_SQRT_2, 1.0);
            sec(d, FRAC_1_SQRT_2, 1.0);
        }
        "BW48" => {
            for q in [0.5098, 0.6013, 0.9000, 2.5629] {
                sec(d, q, 1.0);
            }
        }
        "LR48" => {
            for q in [0.5412, 1.3066, 0.5412, 1.3066] {
                sec(d, q, 1.0);
            }
        }
        _ => {}
    }
}

/// Band parameter ids of the WING EQ (no formatting on the audio thread).
const STD_BANDS: [[&str; 3]; 8] = [
    ["lg", "lf", "lq"],
    ["1g", "1f", "1q"],
    ["2g", "2f", "2q"],
    ["3g", "3f", "3q"],
    ["4g", "4f", "4q"],
    ["5g", "5f", "5q"],
    ["6g", "6f", "6q"],
    ["hg", "hf", "hq"],
];

/// A WING EQ bell. Its Q is the resonance of the boosting section (the cut's
/// for a cut), so the bell is wider than a peaking filter of that Q: the
/// filter's Q is the set Q over 10^(|gain| / 40). Measured on a WING: Q 1.00
/// and 2.00 at +6 dB come out as Q 0.707 and 1.413, Q 4.00 at -9 dB as 2.38.
/// High bells keep their analog shape up to Nyquist (a 12 kHz bell measured
/// within 0.03 dB of that), so the design is the decramped one.
fn wing_peak(sr: f64, f: f32, q: f32, g: f32) -> BiquadCoeffs {
    BiquadCoeffs::peak_decramped(sr, f as f64, (q / 10f32.powf(g.abs() / 40.0)) as f64, g as f64)
}

/// A WING EQ shelf. Its frequency is where the boosting section's poles (the
/// cutting one's zeros) sit, not the half-gain point: the shelf's midpoint is
/// 10^(|gain| / 80) above it for a low shelf, below it for a high one. Its
/// slope is always Q 0.707; measured on a WING, the band's Q (0.5 to 2) left
/// the shelves unchanged.
fn wing_shelf(sr: f64, f: f32, g: f32, low: bool) -> BiquadCoeffs {
    let shift = 10f32.powf(g.abs() / 80.0);
    if low {
        low_shelf(sr, f * shift, std::f32::consts::FRAC_1_SQRT_2, g)
    } else {
        high_shelf(sr, f / shift, std::f32::consts::FRAC_1_SQRT_2, g)
    }
}

fn design_std(p: &Params, sr: f64, k: f32, d: &mut EqDesign) {
    let band = |ids: &[&str; 3]| (p.f(ids[0]) * k, p.f(ids[1]), p.f(ids[2]));

    let (g, f, q) = band(&STD_BANDS[0]);
    match p.text("leq") {
        "PEQ" => push_gain(d, g, || wing_peak(sr, f, q, g)),
        "SHV" => push_gain(d, g, || wing_shelf(sr, f, g, true)),
        kind => push_cut(d, sr, kind, f, q, true),
    }
    for ids in &STD_BANDS[1..7] {
        if p.spec.param_index(ids[0]).is_none() {
            break;
        }
        let (g, f, q) = band(ids);
        push_gain(d, g, || wing_peak(sr, f, q, g));
    }
    let (g, f, q) = band(&STD_BANDS[7]);
    match p.text("heq") {
        "PEQ" => push_gain(d, g, || wing_peak(sr, f, q, g)),
        "SHV" => push_gain(d, g, || wing_shelf(sr, f, g, false)),
        kind => push_cut(d, sr, kind, f, q, false),
    }
    let tilt = p.f("tilt") * k;
    if tilt.abs() > 1e-3 {
        d.push(low_shelf(sr, 1000.0, 0.5, -tilt * 0.5));
        d.push(high_shelf(sr, 1000.0, 0.5, tilt * 0.5));
    }
}

/// E-series style: bells get narrower as they're boosted or cut further.
fn proportional_q(q: f32, g: f32) -> f32 {
    q * (0.6 + 0.4 * (g.abs() / 15.0).min(1.0))
}

fn design_soul(p: &Params, sr: f64, k: f32, d: &mut EqDesign) {
    let g = |id: &str| p.f(id) * 3.0 * k; // dial +-5 -> +-15 dB
    let lg = g("lg");
    push_gain(d, lg, || low_shelf(sr, dial_log(p.f("lf"), 30.0, 450.0), 0.7, lg));
    // The Q knobs run from narrow (2.5) to wide (0.3) clockwise, and the mid
    // bands narrow further as their gain goes up (p.96 and its panel).
    let lmg = g("lmg");
    let lmf = dial_log(p.f("lmf"), 200.0, 2500.0) / if p.on("lmf3") { 3.0 } else { 1.0 };
    let lmq = proportional_q(dial_log(p.f("lmq"), 2.5, 0.3), lmg);
    push_gain(d, lmg, || peak(sr, lmf, lmq, lmg));
    let hmg = g("hmg");
    let hmf = (dial_log(p.f("hmf"), 600.0, 7000.0) * if p.on("hmf3") { 3.0 } else { 1.0 }).min(20000.0);
    let hmq = proportional_q(dial_log(p.f("hmq"), 2.5, 0.3), hmg);
    push_gain(d, hmg, || peak(sr, hmf, hmq, hmg));
    let hg = g("hg");
    push_gain(d, hg, || high_shelf(sr, dial_log(p.f("hf"), 1500.0, 16000.0), 0.7, hg));
}

fn design_e88(p: &Params, sr: f64, k: f32, d: &mut EqDesign) {
    let g = |id: &str| p.f(id) * 3.0 * k;
    // LOW and HIGH: bell or shelf; HI-Q switches their Q from 0.7 to 2.0 (p.97).
    let outer = |d: &mut EqDesign, f: f32, gain: f32, shelf: bool, high_q: bool, high: bool| {
        let q = if high_q { 2.0 } else { 0.7 };
        push_gain(d, gain, || match (shelf, high) {
            (true, false) => low_shelf(sr, f, q, gain),
            (true, true) => high_shelf(sr, f, q, gain),
            _ => peak(sr, f, q, gain),
        });
    };
    outer(d, dial_log(p.f("lf"), 33.0, 440.0), g("lg"), p.text("lt") == "SHELV", p.text("lq") == "HIGH", false);
    let lmg = g("lmg");
    let (lmf, lmq) = (dial_log(p.f("lmf"), 100.0, 2000.0), dial_log(p.f("lmq"), 0.4, 4.0));
    push_gain(d, lmg, || peak(sr, lmf, lmq, lmg));
    let hmg = g("hmg");
    let (hmf, hmq) = (dial_log(p.f("hmf"), 600.0, 9000.0), dial_log(p.f("hmq"), 0.4, 4.0));
    push_gain(d, hmg, || peak(sr, hmf, hmq, hmg));
    outer(d, dial_log(p.f("hf"), 1500.0, 18000.0), g("hg"), p.text("ht") == "SHELV", p.text("hq") == "HIGH", true);
}

/// How strongly the Even 84's shelves pull on each other (see [`shared_shelves`]).
const E84_COUPLING: f64 = 0.25;

fn design_e84(p: &Params, sr: f64, k: f32, d: &mut EqDesign) {
    // LOW +-15 dB, MID and HIGH +-20 dB (p.97).
    let lg = p.f("lg") * 3.0 * k;
    let mg = p.f("mg") * 4.0 * k;
    let hg = p.f("hg") * 4.0 * k;
    let (lf, hf) = (p.freq_choice("lf"), p.freq_choice("hf"));
    if let Some(f) = lf {
        // Inductor shelves have a little resonance at the corner.
        push_gain(d, lg, || low_shelf(sr, f, 0.9, lg));
    }
    if let Some(f) = hf {
        push_gain(d, hg, || high_shelf(sr, f, 0.8, hg));
    }
    if let (Some(fl), Some(fh)) = (lf, hf) {
        if lg.abs() > 1e-3 && hg.abs() > 1e-3 {
            d.push(shared_shelves(sr, fl, lg, fh, hg));
        }
    }
    if let Some(f) = p.freq_choice("mf") {
        let q = if p.text("mq") == "HIGH" { 1.8 } else { 0.7 };
        push_gain(d, mg, || peak(sr, f, q, mg));
    }
    d.out_gain_db = p.f("g");
}

/// The Even 84's two shelves are one shared filter circuit, so setting one
/// changes the other's response (p.97). Modelled on first-order prototypes as
/// the ratio between the two shelves summed in one network,
/// `1 + a LP(fl) + b HP(fh)`, and the same shelves in series: a biquad that is
/// 0 dB at DC and at the top, flat when either shelf is at 0 dB, and in
/// between takes some of the mid lift away when both shelves boost (scaled by
/// [`E84_COUPLING`] for the steeper second-order shelves).
fn shared_shelves(sr: f64, fl: f32, lg_db: f32, fh: f32, hg_db: f32) -> BiquadCoeffs {
    let warp = |f: f32| 2.0 * sr * (std::f64::consts::PI * (f as f64).clamp(10.0, sr * 0.45) / sr).tan();
    let (wl, wh) = (warp(fl), warp(fh));
    let a = 10f64.powf(lg_db as f64 / 20.0) - 1.0;
    let b = 10f64.powf(hg_db as f64 / 20.0) - 1.0;
    let (c0, c2) = (1.0 + a, (1.0 + b) / (wl * wh));
    let n1 = 1.0 / wl + (1.0 + a + b) / wh;
    let d1 = (1.0 / wl + (1.0 + a + b + E84_COUPLING * a * b) / wh).max(0.1 / wl);
    bilinear([c0, n1, c2], [c0, d1, c2], 2.0 * sr)
}

/// Bilinear transform of `(n0 + n1 s + n2 s^2) / (d0 + d1 s + d2 s^2)`, with
/// `k` = 2 * sample rate (frequencies pre-warped by the caller).
fn bilinear(n: [f64; 3], d: [f64; 3], k: f64) -> BiquadCoeffs {
    let z = |p: [f64; 3]| [p[0] + p[1] * k + p[2] * k * k, 2.0 * (p[0] - p[2] * k * k), p[0] - p[1] * k + p[2] * k * k];
    let (b, a) = (z(n), z(d));
    BiquadCoeffs { b0: b[0] / a[0], b1: b[1] / a[0], b2: b[2] / a[0], a1: a[1] / a[0], a2: a[2] / a[0] }
}

const F110_SHELF_Q: f32 = 1.1;

fn design_f110(p: &Params, sr: f64, k: f32, d: &mut EqDesign) {
    if p.on("peq") {
        let g = |id: &str| p.f(id) * 3.6 * k; // +-18 dB
        let lmg = g("lmg");
        let lmf = dial_log(p.f("lmf"), 40.0, 400.0) * if p.on("lmf3") { 3.0 } else { 1.0 };
        let lmq = dial_log(p.f("lmq"), 0.5, 5.0);
        push_gain(d, lmg, || peak(sr, lmf, lmq, lmg));
        let hmg = g("hmg");
        let hmf = (dial_log(p.f("hmf"), 600.0, 6000.0) * if p.on("hmf3") { 3.0 } else { 1.0 }).min(20000.0);
        let hmq = dial_log(p.f("hmq"), 0.5, 5.0);
        push_gain(d, hmg, || peak(sr, hmf, hmq, hmg));
    }
    if p.on("shv") {
        // Higher-order shelves (p.97-98): two resonant sections each, steeper
        // than a plain shelf, with the slight dip before the shelf that gives
        // the enhancer-like character.
        let g = |id: &str| p.f(id) * 3.2 * k;
        if let Some(f) = p.freq_choice("lf") {
            let lg = g("lg");
            push_gain(d, lg, || low_shelf(sr, f, F110_SHELF_Q, lg * 0.5));
            push_gain(d, lg, || low_shelf(sr, f, F110_SHELF_Q, lg * 0.5));
        }
        if let Some(f) = p.freq_choice("hf") {
            let hg = g("hg");
            push_gain(d, hg, || high_shelf(sr, f, F110_SHELF_Q, hg * 0.5));
            push_gain(d, hg, || high_shelf(sr, f, F110_SHELF_Q, hg * 0.5));
        }
    }
    d.out_gain_db = p.f("g");
}

fn design_pulsar(p: &Params, sr: f64, k: f32, d: &mut EqDesign) {
    if p.on("eq1") {
        // HI-LO EQ1: LO ATT low shelf, LO BOOST, HI BOOST parametric, HI ATT
        // high shelf (p.98-99). LO BOOST is a resonant shelf - a slight peak
        // around the selected frequency with the boost held below it - so
        // that boosting and cutting together gives the dip just above the
        // low-end boost the manual describes (a plain bell would leave only
        // the cut below the boost); the cut shelf sits higher than the boost.
        let lf = p.freq_choice("1lf").unwrap_or(60.0);
        let boost = p.f("1lb") * 1.35 * k; // 0..13.5 dB
        let att = -p.f("1latt") * 1.75 * k; // 0..-17.5 dB
        push_gain(d, boost, || low_shelf(sr, lf, 1.0, boost));
        push_gain(d, att, || low_shelf(sr, lf * 1.8, 0.5, att));
        let hb = p.f("1hb") * 1.6 * k; // 0..16 dB
        let hq = dial_log(10.0 - p.f("1hw"), 0.4, 2.5); // WIDTH 10 = broad
        let hf = p.freq_choice("1hf").unwrap_or(8000.0);
        push_gain(d, hb, || peak(sr, hf, hq, hb));
        let hatt = -p.f("1hatt") * 1.6 * k;
        let hattf = p.freq_choice("1hattf").unwrap_or(10000.0);
        push_gain(d, hatt, || high_shelf(sr, hattf, 0.7, hatt));
    }
    if p.on("eq5") {
        let lb = p.f("5lb") * k;
        let lf = p.freq_choice("5lf").unwrap_or(500.0);
        push_gain(d, lb, || peak(sr, lf, 1.0, lb));
        let md = -p.f("5md") * k;
        let mf = p.freq_choice("5mf").unwrap_or(1500.0);
        push_gain(d, md, || peak(sr, mf, 1.2, md));
        let hb = p.f("5hb") * k;
        let hf = p.freq_choice("5hf").unwrap_or(3000.0);
        push_gain(d, hb, || peak(sr, hf, 1.0, hb));
    }
}

fn design_mach4(p: &Params, sr: f64, k: f32, d: &mut EqDesign) {
    // Fixed bands as on the panel (p.100): SUB, 40, 160 and 650 Hz bells, a
    // 2.5 kHz shelf and the AIR shelf.
    let g = |id: &str| p.f(id) * 2.5 * k; // dial +-5 -> +-12.5 dB
    let sub = g("sub");
    push_gain(d, sub, || peak(sr, 10.0, 0.5, sub));
    for (id, f) in [("40", 40.0), ("160", 160.0), ("650", 650.0)] {
        let gain = g(id);
        push_gain(d, gain, || peak(sr, f, 0.6, gain));
    }
    let hi = g("2k5");
    push_gain(d, hi, || high_shelf(sr, 2500.0, 0.5, hi));
    if let Some(f) = p.freq_choice("airm") {
        let air = p.f("air") * 1.5 * k; // 0..15 dB
                                        // Air bands above Nyquist still lift the top octave a little.
        let fc = f.min((sr * 0.42) as f32);
        let air = if f > fc { air * fc / f } else { air };
        push_gain(d, air, || high_shelf(sr, fc, 0.5, air));
    }
    if p.on("again") {
        // AUTO: undo the level change the EQ makes - the mean of its response
        // over the audio band, one point per octave (31 Hz - 16 kHz).
        let mean = (0..10).map(|i| d.response_db(31.25 * f64::from(1u32 << i), sr)).sum::<f64>() / 10.0;
        d.out_gain_db = -mean as f32;
    }
}

/// PIA 560: each band narrows as it is boosted or cut further (p.94).
fn pia_q(g: f32) -> f32 {
    0.7 + 1.1 * (g.abs() / 12.0).min(1.0)
}

fn design_pia(p: &Params, sr: f64, k: f32, d: &mut EqDesign) {
    for (id, f) in [
        ("31", 31.0),
        ("63", 63.0),
        ("125", 125.0),
        ("250", 250.0),
        ("500", 500.0),
        ("1k", 1000.0),
        ("2k", 2000.0),
        ("4k", 4000.0),
        ("8k", 8000.0),
        ("16k", 16000.0),
    ] {
        let gain = p.f(id) * k;
        push_gain(d, gain, || peak(sr, f, pia_q(gain), gain));
    }
    d.out_gain_db = p.f("g");
}

// ---- realtime processor ---------------------------------------------------------------

/// A WING EQ block: model + parameters in, smoothed filtering out.
pub struct WingEq {
    sr: f64,
    sections: [Biquad; MAX_SECTIONS],
    len: usize,
    model: Option<(usize, bool)>,
    smoothed: [f32; MAX_EQ_PARAMS],
    scale: f32,
    out_gain: f32,
    target_out_gain: f32,
}

impl Default for WingEq {
    fn default() -> Self {
        Self {
            sr: 48000.0,
            sections: Default::default(),
            len: 0,
            model: None,
            smoothed: [0.0; MAX_EQ_PARAMS],
            scale: 0.0,
            out_gain: 1.0,
            target_out_gain: 1.0,
        }
    }
}

impl WingEq {
    pub fn prepare(&mut self, sample_rate: f64) {
        self.sr = sample_rate;
        self.reset();
    }

    pub fn reset(&mut self) {
        for s in &mut self.sections {
            s.reset();
        }
        self.model = None;
    }

    /// Processes `chans` in place. `table` is the channel or bus model table.
    pub fn process(
        &mut self,
        chans: &mut [&mut [f32]],
        table: &[ModelSpec],
        model: usize,
        on: bool,
        mix_pct: f32,
        values: &[f32],
    ) {
        let Some(spec) = table.get(model) else { return };
        let key = (model, std::ptr::eq(table.as_ptr(), super::EQ_MODELS_BUS.as_ptr()));
        let target_scale = if on { mix_pct.clamp(0.0, 125.0) / 100.0 } else { 0.0 };

        if self.model != Some(key) {
            // New model: jump to its parameters (no glide between unrelated controls).
            for s in &mut self.sections {
                s.reset();
            }
            for (i, p) in spec.params.iter().enumerate() {
                self.smoothed[i] = p.sanitise(values.get(i).copied().unwrap_or(p.default));
            }
            self.model = Some(key);
            self.scale = target_scale;
            self.redesign(spec);
            self.out_gain = self.target_out_gain;
        }

        if !on && self.scale < 1e-3 {
            return;
        }

        let n = chans.iter().map(|c| c.len()).min().unwrap_or(0);
        let mut start = 0;
        while start < n {
            let len = SUB_BLOCK.min(n - start);
            if self.step_smoothing(spec, values, target_scale) {
                self.redesign(spec);
            }
            let g0 = self.out_gain;
            let g1 = self.target_out_gain;
            for (ch, data) in chans.iter_mut().enumerate() {
                let block = &mut data[start..start + len];
                for s in &mut self.sections[..self.len] {
                    s.process(block, ch);
                }
                if (g0 - 1.0).abs() > 1e-6 || (g1 - 1.0).abs() > 1e-6 {
                    crate::util::apply_gain_ramp(block, g0, g1);
                }
            }
            self.out_gain = g1;
            start += len;
        }
    }

    /// Moves smoothed values toward their targets; true if the design changed.
    fn step_smoothing(&mut self, spec: &ModelSpec, values: &[f32], target_scale: f32) -> bool {
        const K: f32 = 0.3;
        let mut changed = false;
        for (i, p) in spec.params.iter().enumerate() {
            let target = p.sanitise(values.get(i).copied().unwrap_or(p.default));
            let cur = self.smoothed[i];
            if cur == target {
                continue;
            }
            changed = true;
            self.smoothed[i] = match p.kind {
                ParamKind::Linear => {
                    let next = cur + (target - cur) * K;
                    if (next - target).abs() < 1e-3 {
                        target
                    } else {
                        next
                    }
                }
                ParamKind::Log if cur > 0.0 && target > 0.0 => {
                    let next = cur * (target / cur).powf(K);
                    if (next / target - 1.0).abs() < 1e-4 {
                        target
                    } else {
                        next
                    }
                }
                _ => target,
            };
        }
        if self.scale != target_scale {
            changed = true;
            let next = self.scale + (target_scale - self.scale) * K;
            self.scale = if (next - target_scale).abs() < 1e-3 { target_scale } else { next };
        }
        changed
    }

    fn redesign(&mut self, spec: &ModelSpec) {
        let d = design(spec, &self.smoothed[..spec.params.len()], self.sr, self.scale);
        if d.len != self.len {
            // The structure changed (a band turned on/off): new sections start clean.
            for s in &mut self.sections[self.len.min(d.len)..] {
                s.reset();
            }
        }
        for (s, c) in self.sections.iter_mut().zip(&d.sections[..d.len]) {
            s.set_coefficients(*c);
        }
        self.len = d.len;
        let out_db = if self.scale > 0.0 { d.out_gain_db } else { 0.0 };
        self.target_out_gain = db_to_gain(out_db);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wing::{model_index, EQ_MODELS_BUS, EQ_MODELS_CH};

    fn defaults(table: &[ModelSpec], token: &str) -> (usize, [f32; MAX_EQ_PARAMS]) {
        let i = model_index(table, token).unwrap();
        let mut v = [0.0; MAX_EQ_PARAMS];
        table[i].defaults_into(&mut v);
        (i, v)
    }

    fn set(table: &[ModelSpec], m: usize, v: &mut [f32], id: &str, x: f32) {
        v[table[m].param_index(id).unwrap()] = x;
    }

    #[test]
    fn defaults_are_flat() {
        for table in [EQ_MODELS_CH, EQ_MODELS_BUS] {
            for m in table {
                let mut v = [0.0; MAX_EQ_PARAMS];
                m.defaults_into(&mut v);
                let d = design(m, &v, 48000.0, 1.0);
                for f in [30.0, 100.0, 1000.0, 5000.0, 15000.0] {
                    assert!(d.response_db(f, 48000.0).abs() < 0.2, "{} not flat at {f}", m.token);
                }
            }
        }
    }

    #[test]
    fn std_bands_and_mix() {
        let (m, mut v) = defaults(EQ_MODELS_CH, "STD");
        set(EQ_MODELS_CH, m, &mut v, "2g", 6.0);
        set(EQ_MODELS_CH, m, &mut v, "2f", 1000.0);
        let full = design(&EQ_MODELS_CH[m], &v, 48000.0, 1.0);
        assert!((full.response_db(1000.0, 48000.0) - 6.0).abs() < 0.1);
        let half = design(&EQ_MODELS_CH[m], &v, 48000.0, 0.5);
        assert!((half.response_db(1000.0, 48000.0) - 3.0).abs() < 0.1);
        let more = design(&EQ_MODELS_CH[m], &v, 48000.0, 1.25);
        assert!((more.response_db(1000.0, 48000.0) - 7.5).abs() < 0.1);
    }

    #[test]
    fn bus_cut_filters() {
        let (m, mut v) = defaults(EQ_MODELS_BUS, "STD");
        let spec = &EQ_MODELS_BUS[m];
        let leq = spec.param_index("leq").unwrap();
        let ParamKind::Choice(items) = spec.params[leq].kind else { panic!() };
        set(EQ_MODELS_BUS, m, &mut v, "lf", 100.0);
        for (i, kind) in items.iter().enumerate().skip(2) {
            v[leq] = i as f32;
            let d = design(spec, &v, 48000.0, 1.0);
            assert!(d.response_db(10.0, 48000.0) < -6.0, "{kind} doesn't cut");
            assert!(d.response_db(5000.0, 48000.0).abs() < 0.5, "{kind} affects the pass band");
            if kind.starts_with("BW") || kind.starts_with("BS") {
                assert!((d.response_db(100.0, 48000.0) + 3.0).abs() < 0.6, "{kind} corner");
            }
        }
    }

    type Case<'a> = (&'a [ModelSpec], &'a str, &'a [(&'a str, f32)], f64, f64);

    #[test]
    fn analog_models_respond() {
        let cases: &[Case] = &[
            (EQ_MODELS_CH, "SOUL", &[("lmg", 5.0), ("lmf", 5.0), ("lmf3", 0.0)], 700.0, 10.0),
            (EQ_MODELS_CH, "E84", &[("mg", 5.0)], 1600.0, 12.0),
            (EQ_MODELS_CH, "PULSAR", &[("1lb", 10.0), ("1lf", 2.0)], 30.0, 10.0),
            (EQ_MODELS_CH, "MACH4", &[("650", 5.0), ("again", 0.0)], 650.0, 10.0),
            (EQ_MODELS_BUS, "PIA", &[("1k", 6.0)], 1000.0, 5.0),
            (EQ_MODELS_CH, "F110", &[("hmg", -5.0), ("hmf", 5.0)], 1897.0, -14.0),
            (EQ_MODELS_CH, "E88", &[("hg", 5.0), ("hf", 5.0)], 16000.0, 10.0),
        ];
        for (table, token, sets, freq, at_least) in cases {
            let (m, mut v) = defaults(table, token);
            for (id, x) in *sets {
                set(table, m, &mut v, id, *x);
            }
            let r = design(&table[m], &v, 48000.0, 1.0).response_db(*freq, 48000.0);
            if *at_least >= 0.0 {
                assert!(r >= *at_least, "{token}: {r} dB at {freq}");
            } else {
                assert!(r <= *at_least, "{token}: {r} dB at {freq}");
            }
        }
    }

    fn designed(table: &[ModelSpec], token: &str, sets: &[(&str, f32)]) -> EqDesign {
        let (m, mut v) = defaults(table, token);
        for (id, x) in sets {
            set(table, m, &mut v, id, *x);
        }
        design(&table[m], &v, 48000.0, 1.0)
    }

    fn choice(table: &[ModelSpec], token: &str, id: &str, item: &str) -> f32 {
        let m = model_index(table, token).unwrap();
        let ParamKind::Choice(items) = table[m].params[table[m].param_index(id).unwrap()].kind else { panic!() };
        items.iter().position(|t| *t == item).unwrap() as f32
    }

    const SR: f64 = 48000.0;

    #[test]
    fn soul_q_knobs_run_from_narrow_to_wide() {
        // Full gain: the proportional-Q factor is 1, so the Q is the knob's.
        let f = dial_log(5.0, 200.0, 2500.0);
        for (dial, q) in [(0.0, 2.5), (10.0, 0.3)] {
            let d = designed(EQ_MODELS_CH, "SOUL", &[("lmg", 5.0), ("lmq", dial), ("lmf", 5.0), ("lmf3", 0.0)]);
            let want = BiquadCoeffs::peak(SR, f as f64, q, 15.0);
            let (a, b) = (d.response_db(2.0 * f as f64, SR), 20.0 * want.magnitude_at(2.0 * f as f64, SR).log10());
            assert!((a - b).abs() < 0.01, "dial {dial}: {a} vs {b}");
        }
        // ... and narrower again as the gain goes up.
        let soft = designed(EQ_MODELS_CH, "SOUL", &[("lmg", 1.0), ("lmf", 5.0), ("lmf3", 0.0)]);
        let hard = designed(EQ_MODELS_CH, "SOUL", &[("lmg", 5.0), ("lmf", 5.0), ("lmf3", 0.0)]);
        let at = |d: &EqDesign, g: f64| d.response_db(2.0 * f as f64, SR) / g;
        assert!(at(&hard, 15.0) < at(&soft, 3.0));
    }

    #[test]
    fn even88_hi_q_switches_between_q_0_7_and_2() {
        let f = dial_log(5.0, 33.0, 440.0) as f64;
        let bell = choice(EQ_MODELS_CH, "E88", "lt", "BELL");
        let shelf = choice(EQ_MODELS_CH, "E88", "lt", "SHELV");
        let high = choice(EQ_MODELS_CH, "E88", "lq", "HIGH");
        let low = choice(EQ_MODELS_CH, "E88", "lq", "LOW");
        for (lt, lq, want) in [
            (bell, low, BiquadCoeffs::peak(SR, f, 0.7, 15.0)),
            (bell, high, BiquadCoeffs::peak(SR, f, 2.0, 15.0)),
            (shelf, low, BiquadCoeffs::low_shelf(SR, f, 0.7, 15.0)),
            (shelf, high, BiquadCoeffs::low_shelf(SR, f, 2.0, 15.0)),
        ] {
            let d = designed(EQ_MODELS_CH, "E88", &[("lf", 5.0), ("lg", 5.0), ("lt", lt), ("lq", lq)]);
            for x in [f / 3.0, f * 1.5, f * 3.0] {
                let b = 20.0 * want.magnitude_at(x, SR).log10();
                assert!((d.response_db(x, SR) - b).abs() < 0.01, "lt {lt} lq {lq} at {x}");
            }
        }
    }

    #[test]
    fn even84_band_ranges() {
        // LOW +-15 dB, MID +-20 dB, HIGH +-20 dB (p.97).
        let (lf, hf) = (choice(EQ_MODELS_CH, "E84", "lf", "60"), choice(EQ_MODELS_CH, "E84", "hf", "12k"));
        let low = designed(EQ_MODELS_CH, "E84", &[("lf", lf), ("lg", 5.0)]);
        assert!((low.response_db(10.0, SR) - 15.0).abs() < 0.3);
        let mid = designed(EQ_MODELS_CH, "E84", &[("mg", -5.0)]);
        assert!((mid.response_db(1600.0, SR) + 20.0).abs() < 0.05);
        let high = designed(EQ_MODELS_CH, "E84", &[("hf", hf), ("hg", 5.0)]);
        assert!((high.response_db(22000.0, SR) - 20.0).abs() < 0.5);
    }

    #[test]
    fn even84_shelves_interact() {
        let (lf, hf) = (choice(EQ_MODELS_CH, "E84", "lf", "220"), choice(EQ_MODELS_CH, "E84", "hf", "10k"));
        let one = |id: &str, g: f32| designed(EQ_MODELS_CH, "E84", &[("lf", lf), ("hf", hf), (id, g)]);
        let both = |lg: f32, hg: f32| designed(EQ_MODELS_CH, "E84", &[("lf", lf), ("hf", hf), ("lg", lg), ("hg", hg)]);
        // Boosting both shelves takes some of the mid lift away ...
        let x = 1000.0;
        let apart = one("lg", 5.0).response_db(x, SR) + one("hg", 5.0).response_db(x, SR);
        let together = both(5.0, 5.0).response_db(x, SR);
        assert!(together < apart - 0.5, "{together} vs {apart}");
        // ... but leaves the shelves' own ranges (and DC/top) alone.
        assert!((both(5.0, 5.0).response_db(10.0, SR) - 15.0).abs() < 0.3);
        assert!((both(5.0, 5.0).response_db(22000.0, SR) - 20.0).abs() < 0.5);
        // One shelf alone: no coupling section at all.
        assert_eq!(one("lg", 5.0).len, 1);
        // Every combination is stable (poles inside the unit circle).
        for lg in [-5.0, -2.0, 2.0, 5.0] {
            for hg in [-5.0, -2.0, 2.0, 5.0] {
                for c in &both(lg, hg).sections[..both(lg, hg).len] {
                    assert!(c.a2.abs() < 1.0 && c.a1.abs() < 1.0 + c.a2, "lg {lg} hg {hg}: {c:?}");
                }
            }
        }
    }

    #[test]
    fn fortissimo_shelves_are_higher_order() {
        // Steeper than one second-order shelf, with a dip below the boost.
        let d = designed(EQ_MODELS_CH, "F110", &[("hg", 5.0)]); // 10 kHz, +16 dB
        let plain = BiquadCoeffs::high_shelf(SR, 10000.0, 0.7, 16.0);
        let plain_db = |f: f64| 20.0 * plain.magnitude_at(f, SR).log10();
        assert!(d.response_db(5000.0, SR) < -0.5, "no dip: {}", d.response_db(5000.0, SR));
        assert!(plain_db(5000.0) > 0.5);
        assert!(d.response_db(15000.0, SR) > plain_db(15000.0));
        assert!((d.response_db(10000.0, SR) - 8.0).abs() < 0.5);
    }

    #[test]
    fn pulsar_low_boost_and_cut_dip() {
        // Boost and cut together: a low-end boost with a dip just above it (p.98).
        let d = designed(EQ_MODELS_CH, "PULSAR", &[("1lb", 10.0), ("1latt", 5.0), ("1lf", 2.0)]);
        assert!(d.response_db(30.0, SR) > 3.0);
        assert!(d.response_db(100.0, SR) < -3.0);
        // HI WIDTH: low values narrower.
        let narrow = designed(EQ_MODELS_CH, "PULSAR", &[("1hb", 10.0), ("1hw", 0.0)]);
        let broad = designed(EQ_MODELS_CH, "PULSAR", &[("1hb", 10.0), ("1hw", 10.0)]);
        assert!(narrow.response_db(4000.0, SR) < broad.response_db(4000.0, SR));
    }

    #[test]
    fn mach4_bands_and_auto_gain() {
        // SUB is a bell (it falls back below its frequency), 2.5 kHz a shelf (p.100).
        let sub = designed(EQ_MODELS_CH, "MACH4", &[("sub", 5.0), ("again", 0.0)]);
        assert!((sub.response_db(10.0, SR) - 12.5).abs() < 0.1);
        assert!(sub.response_db(1.0, SR) < 3.0);
        let hi = designed(EQ_MODELS_CH, "MACH4", &[("2k5", 5.0), ("again", 0.0)]);
        assert!((hi.response_db(20000.0, SR) - 12.5).abs() < 0.5);
        assert!(hi.response_db(100.0, SR).abs() < 0.1);
        // AUTO cancels the EQ's overall level change.
        let sets = [("2k5", 5.0), ("40", 3.0), ("air", 6.0), ("airm", 3.0)];
        let plain = designed(EQ_MODELS_CH, "MACH4", &[sets[0], sets[1], sets[2], sets[3], ("again", 0.0)]);
        let auto = designed(EQ_MODELS_CH, "MACH4", &[sets[0], sets[1], sets[2], sets[3], ("again", 1.0)]);
        let mean = |d: &EqDesign| (0..10).map(|i| d.response_db(31.25 * f64::from(1u32 << i), SR)).sum::<f64>() / 10.0;
        assert!(mean(&plain) > 3.0);
        assert!(mean(&auto).abs() < 1e-3, "{}", mean(&auto));
    }

    #[test]
    fn pia_bands_narrow_as_the_gain_grows() {
        let small = designed(EQ_MODELS_BUS, "PIA", &[("1k", 3.0)]);
        let large = designed(EQ_MODELS_BUS, "PIA", &[("1k", 12.0)]);
        assert!((large.response_db(1000.0, SR) - 12.0).abs() < 0.05);
        let skirt = |d: &EqDesign, g: f64| d.response_db(1500.0, SR) / g;
        assert!(skirt(&large, 12.0) < skirt(&small, 3.0) * 0.7);
    }

    #[test]
    fn processor_matches_design_and_bypasses() {
        let (m, mut v) = defaults(EQ_MODELS_CH, "STD");
        set(EQ_MODELS_CH, m, &mut v, "3g", 12.0);
        set(EQ_MODELS_CH, m, &mut v, "3f", 1000.0);
        let mut eq = WingEq::default();
        eq.prepare(48000.0);
        let tone: Vec<f32> = (0..48000).map(|i| (i as f32 * 2.0 * std::f32::consts::PI * 1000.0 / 48000.0).sin() * 0.1).collect();
        let mut buf = tone.clone();
        eq.process(&mut [&mut buf[..]], EQ_MODELS_CH, m, true, 100.0, &v);
        let peak = buf[24000..].iter().fold(0.0f32, |a, x| a.max(x.abs()));
        assert!((crate::util::gain_to_db(peak / 0.1) - 12.0).abs() < 0.3);

        let mut off = tone.clone();
        let mut eq2 = WingEq::default();
        eq2.prepare(48000.0);
        eq2.process(&mut [&mut off[..]], EQ_MODELS_CH, m, false, 100.0, &v);
        assert_eq!(off, tone);
    }
}
