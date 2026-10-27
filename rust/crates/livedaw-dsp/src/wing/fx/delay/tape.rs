//! TAPE-DL: a Roland RE-201 Space Echo style tape delay.

use crate::wing::fx::blocks::{fast_tanh, DelayLine, Lfo, OnePole, Smoothed, Svf, Wave};
use crate::wing::fx::meters::VuMeter;
use crate::wing::fx::modulation::shared::{ms, Glide};
use crate::wing::fx::{FxAlgo, FxContext, FX_READOUTS};
use crate::wing::Params;
use std::f32::consts::TAU;

const MAX_MS: f32 = 650.0;
/// Wow and flutter: rate (Hz) and pitch deviation at FLUTTER 100 %.
const WOW: (f32, f32) = (0.7, 0.004);
const FLUTTER: (f32, f32) = (6.3, 0.0015);
/// Slow random drift of the tape speed: rate (Hz) and depth (ms).
const DRIFT: (f32, f32) = (0.37, 0.6);
/// Playback head and tape response, applied on every pass.
const HEAD_LP_HZ: f32 = 4200.0;
const HEAD_HP_HZ: f32 = 90.0;
/// SUSTAIN 100 % slightly over unity: the echo runs away into the saturation.
const SUSTAIN_MAX: f32 = 1.08;
/// DRIVE 100 % = +18 dB into the tape.
const DRIVE_DB: f32 = 18.0;
const HEADROOM: f32 = 1.3;
/// The VUs' 0 VU.
const VU_ZERO_DBFS: f32 = -18.0;

/// Tape Delay (Roland RE-201 Space Echo).
///
/// A tape loop per channel: the input and the feedback (SUSTAIN) are
/// recorded through a saturating tape stage (DRIVE adds gain into it, so the
/// repeats compress and distort; the loop gain stays SUSTAIN at low levels)
/// and played back through a head response (90 Hz .. 4.2 kHz) that darkens
/// every repeat. FLUTTER sets wow (0.7 Hz), flutter (6.3 Hz) and a random
/// speed drift, shared by both channels as on one tape. TIME (60-650 ms)
/// changes glide like the motor speed changing, bending the pitch of the
/// repeats. Above ~93 % SUSTAIN the echo self-oscillates, held in check by
/// the tape saturation.
pub(super) struct TapeDelay {
    sr: f32,
    lines: [DelayLine; 2],
    time: Glide,
    fb: Smoothed,
    /// Gain into the tape, the make-up after it and the loop compensation.
    pre: Smoothed,
    post: Smoothed,
    fb_comp: Smoothed,
    flutter: Smoothed,
    wow: Lfo,
    flut: Lfo,
    drift: Lfo,
    /// Modulation depths in samples: wow, flutter, drift.
    depth: [f32; 3],
    lp: [Svf; 2],
    hp: [OnePole; 2],
    /// The record level onto each channel's tape (its VU).
    vu: [VuMeter; 2],
    primed: bool,
}

impl TapeDelay {
    pub(super) fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let lfo = |hz: f32, phase: f32| {
            let mut l = Lfo::default();
            l.set_rate(hz, sr);
            l.reset(phase);
            l
        };
        let (wow, flut, drift) = (lfo(WOW.0, 0.0), lfo(FLUTTER.0, 0.3), lfo(DRIFT.0, 0.0));
        let mut lp = [Svf::default(); 2];
        for f in &mut lp {
            f.set(HEAD_LP_HZ, 0.6, sr);
        }
        let s = |v: f32| Smoothed::new(v, 30.0, sr);
        // Pitch deviation d at f Hz needs a delay swing of d / (2 pi f) seconds.
        let swing = |(hz, dev): (f32, f32)| dev / (TAU * hz) * sr;
        Self {
            sr,
            lines: [DelayLine::new(ms(MAX_MS + 10.0, sr) as usize), DelayLine::new(ms(MAX_MS + 10.0, sr) as usize)],
            time: Glide::new(250.0, 0.3, sr),
            fb: s(0.4),
            pre: s(1.0),
            post: s(1.0),
            fb_comp: s(1.0),
            flutter: s(0.3),
            wow,
            flut,
            drift,
            depth: [swing(WOW), swing(FLUTTER), ms(DRIFT.1, sr)],
            lp,
            hp: [OnePole::new(HEAD_HP_HZ, sr); 2],
            vu: std::array::from_fn(|_| VuMeter::new(sr, VU_ZERO_DBFS)),
            primed: false,
        }
    }

    fn smoothers(&mut self) -> [&mut Smoothed; 5] {
        [&mut self.fb, &mut self.pre, &mut self.post, &mut self.fb_comp, &mut self.flutter]
    }
}

impl FxAlgo for TapeDelay {
    fn set_params(&mut self, p: &Params) {
        self.time.target = ms(p.f("time").clamp(60.0, MAX_MS), self.sr);
        self.fb.target = p.f("sust") * 0.01 * SUSTAIN_MAX;
        let pre = 10f32.powf(p.f("drv") * 0.01 * DRIVE_DB / 20.0);
        self.pre.target = pre;
        self.post.target = pre.powf(-0.75);
        self.fb_comp.target = pre.powf(-0.25);
        self.flutter.target = p.f("wf") * 0.01;
        if !self.primed {
            self.primed = true;
            self.time.snap();
            for s in self.smoothers() {
                s.snap(s.target);
            }
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        let [dw, df, dd] = self.depth;
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            let fl = self.flutter.tick();
            let wobble = dw * self.wow.next(Wave::Sine, 0.0)
                + df * self.flut.next(Wave::Sine, 0.0)
                + dd * self.drift.next(Wave::Random, 0.0);
            let d = self.time.tick() + fl * wobble;
            let fb = self.fb.tick() * self.fb_comp.tick();
            let pre = self.pre.tick();
            let post = self.post.tick();
            for (ch, x) in [l, r].into_iter().enumerate() {
                let y = self.lines[ch].tap_hermite(d);
                let y = self.hp[ch].hp(self.lp[ch].tick(y).lp);
                let into = pre * (*x + fb * y);
                self.vu[ch].tick(into);
                let rec = HEADROOM * fast_tanh(into / HEADROOM) * post;
                self.lines[ch].push(rec);
                *x = y;
            }
        }
    }

    /// Readouts 0 / 1: the record level onto the left / right tape (the two
    /// VUs; VU levels, 0 VU at -18 dBFS).
    fn readouts(&self, out: &mut [f32; FX_READOUTS]) {
        out[0] = self.vu[0].level();
        out[1] = self.vu[1].level();
    }

    fn reset(&mut self) {
        for v in &mut self.vu {
            v.reset();
        }
        for l in &mut self.lines {
            l.clear();
        }
        for f in &mut self.lp {
            f.reset();
        }
        for f in &mut self.hp {
            f.reset();
        }
        self.wow.reset(0.0);
        self.flut.reset(0.3);
        self.drift.reset(0.0);
        self.time.snap();
        for s in self.smoothers() {
            s.snap(s.target);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::tests::{device, impulse, max_step, peak, SR};

    #[test]
    fn repeats_land_and_darken() {
        let (mut dev, v) = device("TAPE-DL", &[("wf", 0.0)]);
        let (l, _) = impulse(&mut dev, &v, 1.0);
        let t = (0.3 * SR) as usize;
        assert!(peak(&l, 0, t - 100).1 < 1e-4, "nothing before the first repeat");
        let (i1, p1) = peak(&l, t - 100, t + 100);
        let (i2, p2) = peak(&l, 2 * t - 100, 2 * t + 100);
        assert!(i1.abs_diff(t) < 20 && i2.abs_diff(2 * t) < 40, "{i1} {i2}");
        assert!(p2 < 0.6 * p1, "repeats fade: {p1} {p2}");
        // Darker: the second repeat is more spread out than the first.
        let width = |i: usize, p: f32| l[i - 50..i + 50].iter().filter(|x| x.abs() > 0.3 * p).count();
        assert!(width(i2, p2) > width(i1, p1), "{} {}", width(i1, p1), width(i2, p2));
    }

    #[test]
    fn time_changes_glide() {
        let step = max_step("TAPE-DL", &[("wf", 0.0), ("sust", 0.0), ("drv", 0.0)], &[("time", 500.0)]);
        assert!(step < 0.05, "{step}");
    }
}
