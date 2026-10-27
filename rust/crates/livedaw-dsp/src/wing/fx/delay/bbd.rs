//! BBD-DL: an analog bucket-brigade delay pedal.

use crate::wing::fx::blocks::{fast_tanh, DelayLine, Smoothed, Svf};
use crate::wing::fx::modulation::shared::{ms, Glide};
use crate::wing::fx::{FxAlgo, FxContext};
use crate::wing::Params;

/// DELAY 0..100 maps exponentially onto this range.
const TIME_MS: (f32, f32) = (20.0, 600.0);
/// Bandwidth at the shortest delay; it falls with longer delays (a slower
/// clock) as `(20 ms / delay) ^ BW_EXP`: 9 kHz at 20 ms, ~2.2 kHz at 600 ms.
const BW_HZ: f32 = 9000.0;
const BW_EXP: f32 = 0.42;
/// Compander detector (ms) and how hard it squeezes loud signals.
const COMP_ATTACK_MS: f32 = 5.0;
const COMP_RELEASE_MS: f32 = 80.0;
const COMP_AMOUNT: f32 = 0.5;
/// Drive of the bucket-brigade chip itself.
const CLIP: f32 = 1.2;

/// BBD Delay (analog bucket-brigade pedal).
///
/// DELAY 0..100 sets 20-600 ms (exponential). The signal passes an
/// anti-alias filter, a compander-style stage (the compressor's level
/// detector lags behind transients, which then clip softly in the "chip";
/// sustained loud signals stay clean) and the line, and comes back through
/// a reconstruction filter. Both filters follow the clock: longer delays
/// are darker, and every repeat (FEED) loses more highs. Changing DELAY
/// glides, bending the pitch of the repeats as turning the knob of the
/// pedal does. FEED 100 % holds the repeats while the filters slowly eat
/// them; nothing here adds noise.
pub(super) struct Bbd {
    sr: f32,
    lines: [DelayLine; 2],
    time: Glide,
    fb: Smoothed,
    aa: [Svf; 2],
    rc: [Svf; 2],
    /// Current filter cutoff.
    bw: f32,
    env: [f32; 2],
    att: f32,
    rel: f32,
    primed: bool,
}

impl Bbd {
    pub(super) fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let len = ms(TIME_MS.1 + 10.0, sr) as usize;
        let mut dev = Self {
            sr,
            lines: [DelayLine::new(len), DelayLine::new(len)],
            time: Glide::new(150.0, 0.5, sr),
            fb: Smoothed::new(0.3, 30.0, sr),
            aa: [Svf::default(); 2],
            rc: [Svf::default(); 2],
            bw: 0.0,
            env: [0.0; 2],
            att: crate::wing::fx::blocks::coef(COMP_ATTACK_MS, sr),
            rel: crate::wing::fx::blocks::coef(COMP_RELEASE_MS, sr),
            primed: false,
        };
        dev.time.target = ms(TIME_MS.0, sr);
        dev.time.snap();
        dev.track_clock();
        dev
    }

    /// Sets the filters for the current delay (called once per block).
    fn track_clock(&mut self) {
        let delay_ms = (self.time.value * 1000.0 / self.sr).max(1.0);
        let hz = (BW_HZ * (TIME_MS.0 / delay_ms).powf(BW_EXP)).min(self.sr * 0.45);
        if (hz / self.bw - 1.0).abs() > 0.005 {
            self.bw = hz;
            for f in self.aa.iter_mut().chain(self.rc.iter_mut()) {
                f.set(hz, 0.7, self.sr);
            }
        }
    }
}

impl FxAlgo for Bbd {
    fn set_params(&mut self, p: &Params) {
        let (lo, hi) = TIME_MS;
        self.time.target = ms(lo * (hi / lo).powf(p.f("dly") * 0.01), self.sr);
        self.fb.target = p.f("fb") * 0.01;
        if !self.primed {
            self.primed = true;
            self.time.snap();
            self.fb.snap(self.fb.target);
            self.track_clock();
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        self.track_clock();
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            let d = self.time.tick();
            let fb = self.fb.tick();
            for (ch, x) in [l, r].into_iter().enumerate() {
                let y = self.rc[ch].tick(self.lines[ch].tap_hermite(d)).lp;
                let v = *x + fb * y;
                let a = v.abs();
                let e = &mut self.env[ch];
                *e += (a - *e) * if a > *e { self.att } else { self.rel };
                // Compress, clip in the chip, expand again (never above unity gain).
                let g = 1.0 + COMP_AMOUNT * *e;
                let s = g * fast_tanh(CLIP * v / g) / CLIP;
                self.lines[ch].push(self.aa[ch].tick(s).lp);
                *x = y;
            }
        }
    }

    fn reset(&mut self) {
        for l in &mut self.lines {
            l.clear();
        }
        for f in self.aa.iter_mut().chain(self.rc.iter_mut()) {
            f.reset();
        }
        self.env = [0.0; 2];
        self.time.snap();
        self.fb.snap(self.fb.target);
        self.track_clock();
    }
}

#[cfg(test)]
mod tests {
    use super::super::tests::{device, impulse, max_step, peak, SR};
    use std::f32::consts::TAU;

    #[test]
    fn echo_at_delay_and_band_limited() {
        let (mut dev, v) = device("BBD-DL", &[("fb", 0.0), ("dly", 40.0)]);
        let (l, _) = impulse(&mut dev, &v, 0.5);
        let t = 20.0 * 30f32.powf(0.4) * 0.001 * SR;
        let (i, p) = peak(&l, 10, l.len());
        assert!((i as f32 - t).abs() < 15.0 && p > 0.05, "echo at {i} ({p}), expected {t}");

        let level = |hz: f32| {
            let (mut dev, v) = device("BBD-DL", &[("fb", 0.0)]);
            let mut l: Vec<f32> = (0..24_000).map(|i| 0.3 * (TAU * hz * i as f32 / SR).sin()).collect();
            let mut r = l.clone();
            for (cl, cr) in l.chunks_mut(256).zip(r.chunks_mut(256)) {
                dev.process(&v, cl, cr);
            }
            l[12_000..].iter().fold(0.0f32, |a, x| a.max(x.abs()))
        };
        assert!(level(10_000.0) < 0.1 * level(300.0));
    }

    #[test]
    fn delay_changes_glide() {
        let step = max_step("BBD-DL", &[("fb", 0.0)], &[("dly", 70.0)]);
        assert!(step < 0.05, "{step}");
    }
}
