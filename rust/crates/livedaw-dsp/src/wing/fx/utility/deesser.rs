//! 2 Band De-Esser, after the SPL De-Esser: no threshold to set. Each band
//! compares its sibilance band with the whole signal and reduces the band when
//! sibilance dominates, so the result doesn't depend on the input level.
//!
//! LOW and HIGH (0..50) set the largest reduction of the two bands (up to
//! 20 dB). GENDER moves the bands: f = 6 kHz and 10 kHz, m = 4.5 kHz and
//! 7.5 kHz. MODE st works on left and right with linked detection; ms works on
//! mid and side, the side using LOW (S) and HIGH (S). The reduction is a
//! dynamic cut of the band only (signal plus a scaled band-pass of itself).

use super::{db_gain, level_db};
use crate::wing::fx::blocks::{coef, Biquad, BiquadCoeffs};
use crate::wing::fx::FxAlgo;
use crate::wing::Params;

const FREQS: [[f64; 2]; 2] = [[6000.0, 10_000.0], [4500.0, 7500.0]];
const BAND_Q: f64 = 1.6;
/// Sibilance this far below the whole signal starts the reduction ...
const ONSET_DB: f32 = -16.0;
/// ... and this much above the onset gets the full reduction.
const SPAN_DB: f32 = 10.0;

pub(crate) struct DeEsser {
    sr: f32,
    /// One band-pass per band; its two channel states are the two paths (L/R or M/S).
    bands: [Biquad; 2],
    gender: usize,
    ms: bool,
    /// Largest reduction in dB, [band][path].
    depth: [[f32; 2]; 2],
    band_env: [[f32; 2]; 2],
    full_env: [f32; 2],
    /// Current reduction in dB (>= 0), [band][path].
    red: [[f32; 2]; 2],
    env_att: f32,
    env_rel: f32,
    red_att: f32,
    red_rel: f32,
    meter: f32,
}

impl DeEsser {
    pub(crate) fn new(sr: f32) -> Self {
        let mut s = Self {
            sr,
            bands: Default::default(),
            gender: usize::MAX,
            ms: false,
            depth: [[0.0; 2]; 2],
            band_env: [[0.0; 2]; 2],
            full_env: [0.0; 2],
            red: [[0.0; 2]; 2],
            env_att: coef(0.5, sr),
            env_rel: coef(40.0, sr),
            red_att: coef(1.0, sr),
            red_rel: coef(60.0, sr),
            meter: 0.0,
        };
        s.set_gender(0);
        s
    }

    fn set_gender(&mut self, g: usize) {
        if g != self.gender {
            self.gender = g;
            for (b, f) in self.bands.iter_mut().zip(FREQS[g.min(1)]) {
                b.set_coefficients(BiquadCoeffs::band_pass(self.sr as f64, f, BAND_Q));
            }
        }
    }
}

#[inline]
fn follow(env: &mut f32, x: f32, att: f32, rel: f32) {
    let v = x.abs();
    *env += (v - *env) * if v > *env { att } else { rel };
}

impl FxAlgo for DeEsser {
    fn set_params(&mut self, p: &Params) {
        self.set_gender(p.choice("gdr"));
        self.ms = p.choice("mode") == 1;
        let db = |id: &str| p.f(id) * 0.4;
        let (lo, hi) = (db("lo"), db("hi"));
        let (lo_s, hi_s) = if self.ms { (db("los"), db("his")) } else { (lo, hi) };
        self.depth = [[lo, lo_s], [hi, hi_s]];
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        let mut most = 0.0f32;
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            let x = if self.ms { [(*l + *r) * 0.5, (*l - *r) * 0.5] } else { [*l, *r] };
            // [band][path]
            let mut band = [[0.0f32; 2]; 2];
            for (env, &v) in self.full_env.iter_mut().zip(&x) {
                follow(env, v, self.env_att, self.env_rel);
            }
            for ((filter, out), envs) in self.bands.iter_mut().zip(band.iter_mut()).zip(self.band_env.iter_mut()) {
                for (p, ((o, &v), env)) in out.iter_mut().zip(&x).zip(envs.iter_mut()).enumerate() {
                    *o = filter.tick(v, p);
                    follow(env, *o, self.env_att, self.env_rel);
                }
            }
            for ((reds, envs), depths) in self.red.iter_mut().zip(&self.band_env).zip(&self.depth) {
                // How much the band dominates, 0..1, per path.
                let mut amount = [0.0f32; 2];
                for ((a, &env), &full) in amount.iter_mut().zip(envs).zip(&self.full_env) {
                    if full > 1e-6 {
                        *a = ((level_db(env / full) - ONSET_DB) / SPAN_DB).clamp(0.0, 1.0);
                    }
                }
                if !self.ms {
                    amount = [amount[0].max(amount[1]); 2];
                }
                for ((red, a), depth) in reds.iter_mut().zip(amount).zip(depths) {
                    let target = depth * a;
                    *red += (target - *red) * if target > *red { self.red_att } else { self.red_rel };
                    most = most.max(*red);
                }
            }
            let mut y = x;
            for (band, reds) in band.iter().zip(&self.red) {
                for ((yp, &bp), &red) in y.iter_mut().zip(band).zip(reds) {
                    *yp += (db_gain(-red) - 1.0) * bp;
                }
            }
            if self.ms {
                *l = y[0] + y[1];
                *r = y[0] - y[1];
            } else {
                *l = y[0];
                *r = y[1];
            }
        }
        self.meter = -most;
    }

    fn reset(&mut self) {
        for b in &mut self.bands {
            b.reset();
        }
        self.band_env = [[0.0; 2]; 2];
        self.full_env = [0.0; 2];
        self.red = [[0.0; 2]; 2];
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
    fn reduces_sibilance_not_the_body() {
        let mut rig = Rig::new("DE-S2");
        rig.set("hi", 50.0).set("lo", 50.0);
        let low = rig.sine_gain_db(300.0, 0.3);
        assert!(low.abs() < 0.2, "{low} dB at 300 Hz");
        let mut rig = Rig::new("DE-S2");
        rig.set("hi", 50.0).set("lo", 50.0);
        let ess = rig.sine_gain_db(7000.0, 0.3);
        assert!(ess < -6.0, "{ess} dB at 7 kHz");
        assert!(rig.dev.meter() < -6.0);
    }

    #[test]
    fn mid_side_mode_keeps_mono_mono() {
        let mut rig = Rig::new("DE-S2");
        rig.set("mode", 1.0);
        let n = 9600;
        let x: Vec<f32> = (0..n).map(|i| ((i * 7919) % 997) as f32 / 997.0 - 0.5).collect();
        let (mut l, mut r) = (x.clone(), x.clone());
        rig.run(&mut l, &mut r);
        assert!(l.iter().zip(&r).all(|(a, b)| (a - b).abs() < 1e-6));
    }
}
