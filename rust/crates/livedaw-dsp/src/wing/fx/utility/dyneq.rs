//! Dynamic EQ bands and the Triple DEQ.
//!
//! A band is a filter (FILTER: BP bell, LP6/LP12 low shelf, HP6/HP12 high
//! shelf; OFF = bell with full-band detection) whose gain moves from 0 dB
//! toward GAIN as the level in the band passes THR: in MODE high it acts above
//! the threshold, in MODE low below it. RATIO sets how fast the gain follows
//! the level (1 - 1/ratio dB per dB) and ATT/REL how quickly it moves. So a
//! negative GAIN in high mode is a band compressor limited to GAIN, a positive
//! GAIN in low mode lifts the band when it gets quiet, and so on.
//!
//! The band is applied as `x + (g - 1) * F(x)`, where F is the band filter, so
//! the filter is never redesigned per sample and 0 dB is exactly transparent.
//! The detector runs on the same filter output (linked across both channels).
//!
//! Triple DEQ: three such bands in series. The speaker manager uses one too.

use super::{db_gain, level_db, rail, sub_blocks, Glide};
use crate::wing::fx::blocks::{coef, Biquad, BiquadCoeffs};
use crate::wing::fx::FxAlgo;
use crate::wing::Params;
use std::f64::consts::FRAC_1_SQRT_2;

/// One band's controls.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(super) struct DynEqSettings {
    pub thr: f32,
    pub ratio: f32,
    pub att_ms: f32,
    pub rel_ms: f32,
    /// 0 OFF, 1 BP, 2 LP6, 3 LP12, 4 HP6, 5 HP12.
    pub filter: usize,
    pub gain_db: f32,
    pub freq: f32,
    pub q: f32,
    /// true = act above the threshold.
    pub high: bool,
}

impl DynEqSettings {
    /// Reads a band whose ids are `ids` = [thr, ratio, att, rel, filter, gain, freq, q, mode].
    pub fn read(p: &Params, ids: &[&str; 9]) -> Self {
        Self {
            thr: p.f(ids[0]),
            ratio: p.f(ids[1]),
            att_ms: p.f(ids[2]),
            rel_ms: p.f(ids[3]),
            filter: p.choice(ids[4]),
            gain_db: p.f(ids[5]),
            freq: p.f(ids[6]),
            q: p.f(ids[7]),
            high: p.choice(ids[8]) == 1,
        }
    }
}

pub(super) struct DynEqBand {
    sr: f32,
    filter: Biquad,
    kind: usize,
    /// ln(freq), ln(Q).
    glide: Glide<2>,
    thr: f32,
    slope: f32,
    att: f32,
    rel: f32,
    target_db: f32,
    high: bool,
    env: f32,
    env_rel: f32,
    /// Current band gain in dB.
    gain_db: f32,
}

impl DynEqBand {
    pub fn new(sr: f32) -> Self {
        let mut b = Self {
            sr,
            filter: Biquad::default(),
            kind: 1,
            glide: Glide::new(1000f32.ln()),
            thr: 0.0,
            slope: 0.0,
            att: 1.0,
            rel: 1.0,
            target_db: 0.0,
            high: true,
            env: 0.0,
            env_rel: coef(15.0, sr),
            gain_db: 0.0,
        };
        b.glide.tgt[1] = 0.0;
        b.glide.snap();
        b.redesign();
        b
    }

    pub fn set(&mut self, s: &DynEqSettings) {
        if s.filter != self.kind {
            self.kind = s.filter;
            self.filter.reset();
        }
        self.glide.tgt = [s.freq.clamp(20.0, 20_000.0).ln(), s.q.max(0.1).ln()];
        self.thr = s.thr;
        self.slope = 1.0 - 1.0 / s.ratio.max(1.0);
        self.att = coef(s.att_ms.max(0.1), self.sr);
        self.rel = coef(s.rel_ms.max(1.0), self.sr);
        self.target_db = s.gain_db;
        self.high = s.high;
    }

    /// Jumps to the set filter (first parameters, reset).
    pub fn snap(&mut self) {
        self.glide.snap();
        self.redesign();
    }

    fn redesign(&mut self) {
        let sr = self.sr as f64;
        let [f, q] = self.glide.cur.map(|v| v.exp() as f64);
        self.filter.set_coefficients(match self.kind {
            2 => BiquadCoeffs::low_pass_1(sr, f),
            3 => BiquadCoeffs::low_pass(sr, f, FRAC_1_SQRT_2),
            4 => BiquadCoeffs::high_pass_1(sr, f),
            5 => BiquadCoeffs::high_pass(sr, f, FRAC_1_SQRT_2),
            _ => BiquadCoeffs::band_pass(sr, f, q),
        });
    }

    /// Gain (dB) the band heads for at detector level `lvl_db`.
    #[inline]
    fn gain_for(&self, lvl_db: f32) -> f32 {
        const KNEE: f32 = 3.0;
        let over = if self.high { lvl_db - self.thr } else { self.thr - lvl_db };
        let amount = if over >= 0.5 * KNEE {
            over
        } else if over > -0.5 * KNEE {
            (over + 0.5 * KNEE).powi(2) / (2.0 * KNEE)
        } else {
            0.0
        } * self.slope;
        amount.min(self.target_db.abs()).copysign(self.target_db)
    }

    /// Processes in place; returns the band's lowest gain in the block (dB).
    pub fn process(&mut self, left: &mut [f32], right: &mut [f32]) -> f32 {
        let n = left.len().min(right.len());
        let mut lowest = 0.0f32;
        sub_blocks(n, |a, b| {
            if self.glide.step() != 0 {
                self.redesign();
            }
            for (l, r) in left[a..b].iter_mut().zip(right[a..b].iter_mut()) {
                let fl = self.filter.tick(*l, 0);
                let fr = self.filter.tick(*r, 1);
                let det = if self.kind == 0 { l.abs().max(r.abs()) } else { fl.abs().max(fr.abs()) };
                self.env = if det > self.env { det } else { self.env + (det - self.env) * self.env_rel };
                let want = if self.target_db == 0.0 { 0.0 } else { self.gain_for(level_db(self.env)) };
                let c = if want.abs() > self.gain_db.abs() { self.att } else { self.rel };
                self.gain_db += (want - self.gain_db) * c;
                lowest = lowest.min(self.gain_db);
                let k = db_gain(self.gain_db) - 1.0;
                *l += k * fl;
                *r += k * fr;
            }
        });
        lowest
    }

    pub fn reset(&mut self) {
        self.filter.reset();
        self.env = 0.0;
        self.gain_db = 0.0;
        self.snap();
    }
}

const DEQ3_IDS: [[&str; 9]; 3] = [
    ["1-thr", "1-ratio", "1-att", "1-rel", "1-filt", "1-g", "1-f", "1-q", "1-mode"],
    ["2-thr", "2-ratio", "2-att", "2-rel", "2-filt", "2-g", "2-f", "2-q", "2-mode"],
    ["3-thr", "3-ratio", "3-att", "3-rel", "3-filt", "3-g", "3-f", "3-q", "3-mode"],
];

/// Triple DEQ: three dynamic EQ bands in series.
pub(crate) struct TripleDeq {
    bands: [DynEqBand; 3],
    meter: f32,
    primed: bool,
}

impl TripleDeq {
    pub(crate) fn new(sr: f32) -> Self {
        Self { bands: std::array::from_fn(|_| DynEqBand::new(sr)), meter: 0.0, primed: false }
    }
}

impl FxAlgo for TripleDeq {
    fn set_params(&mut self, p: &Params) {
        for (band, ids) in self.bands.iter_mut().zip(&DEQ3_IDS) {
            band.set(&DynEqSettings::read(p, ids));
            if !self.primed {
                band.snap();
            }
        }
        self.primed = true;
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        let mut lowest = 0.0f32;
        for band in &mut self.bands {
            lowest = lowest.min(band.process(left, right));
        }
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            *l = rail(*l);
            *r = rail(*r);
        }
        self.meter = lowest;
    }

    fn reset(&mut self) {
        for band in &mut self.bands {
            band.reset();
        }
        self.meter = 0.0;
    }

    fn meter(&self) -> f32 {
        self.meter
    }
}

#[cfg(test)]
mod tests {
    use super::super::tests::Rig;

    #[test]
    fn cuts_loud_bands_only() {
        // Band 2 (2 kHz, -6 dB above -20 dB).
        let mut rig = Rig::new("DEQ3");
        let loud = rig.sine_gain_db(2000.0, 0.5);
        assert!(loud < -4.0 && loud > -6.5, "{loud}");
        assert!(rig.dev.meter() < -4.0);
        let mut rig = Rig::new("DEQ3");
        let quiet = rig.sine_gain_db(2000.0, 0.01);
        assert!(quiet.abs() < 0.1, "{quiet}");
    }

    #[test]
    fn low_mode_lifts_quiet_bands() {
        let mut rig = Rig::new("DEQ3");
        rig.set("2-mode", 0.0).set("2-g", 6.0).set("2-ratio", 6.0).set("2-thr", -10.0);
        let quiet = rig.sine_gain_db(2000.0, 0.01);
        assert!(quiet > 5.0, "{quiet}");
    }
}
