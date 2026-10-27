//! OILCAN: a Tel-Ray "Ad-n-echo" style oil can delay.

use crate::wing::dial_log;
use crate::wing::fx::blocks::{fast_tanh, shape, Allpass, DelayLine, Lfo, Smoothed, Wave};
use crate::wing::fx::modulation::shared::{advance, ms, CutFilter, Glide};
use crate::wing::fx::{FxAlgo, FxContext};
use crate::wing::Params;
use std::f32::consts::TAU;

/// TIME dial 0..10 (log).
const TIME_MS: (f32, f32) = (60.0, 480.0);
/// The second wiper reads a little later than the first.
const WIPER_SPACING: f32 = 1.03;
/// Disc wobble at WOBBLE 10: two rates (Hz) with their pitch deviation.
const WOBBLE: [(f32, f32); 2] = [(4.3, 0.03), (1.1, 0.012)];
/// Random wander: rate (Hz) and depth (ms) at WOBBLE 10.
const WANDER: (f32, f32) = (2.3, 0.5);
/// TONE dial 0..10: the loop's high cut (log).
const TONE_HZ: (f32, f32) = (1200.0, 7000.0);
const LOW_CUT_HZ: f32 = 150.0;
/// Allpasses in the loop smear every repeat a little more.
const SMEAR_MS: [[f32; 2]; 2] = [[11.3, 17.9], [12.1, 16.7]];
const SMEAR_GAIN: f32 = 0.5;
const HEADROOM: f32 = 1.2;

/// Oilcan Delay (Tel-Ray Ad-n-echo).
///
/// The original stores the echo electrostatically on a disc spinning in an
/// oil-filled can, read by wipers. Here each channel has a loop read by two
/// wipers whose delays warble with the disc (WOBBLE 0..10: up to 3 % pitch
/// at 4.3 Hz plus a slower 1.1 Hz swing and random wander, out of phase
/// between the wipers and the channels); their sum is murky and chorused.
/// Two allpasses in the loop smear each repeat a little more (the
/// slightly reverberant tail), TONE sets the loop's high cut (1.2-7 kHz)
/// and SUSTAIN the feedback (10 = unity, held by soft saturation). TIME
/// (60-480 ms) glides when changed.
pub(super) struct Oilcan {
    sr: f32,
    lines: [DelayLine; 2],
    time: Glide,
    fb: Smoothed,
    wobble: Smoothed,
    /// Phases and increments of the two wobble LFOs.
    phase: [f32; 2],
    inc: [f32; 2],
    wander: Lfo,
    /// Swing in samples of the two LFOs and the wander.
    depth: [f32; 3],
    cut: CutFilter,
    smear: [[Allpass; 2]; 2],
    primed: bool,
}

impl Oilcan {
    pub(super) fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let mut wander = Lfo::default();
        wander.set_rate(WANDER.0, sr);
        let swing = |(hz, dev): (f32, f32)| dev / (TAU * hz) * sr;
        let ap = |d: f32| Allpass::new(ms(d, sr) as usize + 2, ms(d, sr).round().max(1.0), SMEAR_GAIN);
        let len = ms(TIME_MS.1 * WIPER_SPACING + 10.0, sr) as usize;
        Self {
            sr,
            lines: [DelayLine::new(len), DelayLine::new(len)],
            time: Glide::new(200.0, 0.3, sr),
            fb: Smoothed::new(0.4, 30.0, sr),
            wobble: Smoothed::new(0.5, 30.0, sr),
            phase: [0.0; 2],
            inc: WOBBLE.map(|(hz, _)| hz / sr),
            wander,
            depth: [swing(WOBBLE[0]), swing(WOBBLE[1]), ms(WANDER.1, sr)],
            cut: CutFilter::new(sr),
            smear: SMEAR_MS.map(|ch| ch.map(ap)),
            primed: false,
        }
    }
}

impl FxAlgo for Oilcan {
    fn set_params(&mut self, p: &Params) {
        self.time.target = ms(dial_log(p.f("time"), TIME_MS.0, TIME_MS.1), self.sr);
        self.fb.target = p.f("sust") * 0.1;
        self.wobble.target = p.f("wb") * 0.1;
        self.cut.set(LOW_CUT_HZ, dial_log(p.f("tone"), TONE_HZ.0, TONE_HZ.1));
        if !self.primed {
            self.primed = true;
            self.time.snap();
            self.fb.snap(self.fb.target);
            self.wobble.snap(self.wobble.target);
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        self.cut.update(left.len());
        let [d0, d1, d2] = self.depth;
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            let d = self.time.tick();
            let w = self.wobble.tick();
            let fb = self.fb.tick();
            advance(&mut self.phase[0], self.inc[0]);
            advance(&mut self.phase[1], self.inc[1]);
            let wander = d2 * self.wander.next(Wave::Random, 0.0);
            let [p0, p1] = self.phase;
            for (ch, x) in [l, r].into_iter().enumerate() {
                // The second wiper sits half an LFO cycle away: the opposite swing.
                let off = 0.2 * ch as f32;
                let m = w * (d0 * shape(Wave::Sine, p0 + off) + d1 * shape(Wave::Sine, p1 + off) + wander);
                let line = &self.lines[ch];
                let y = line.tap_hermite(d + m) + line.tap_hermite(d * WIPER_SPACING - m);
                let y = self.cut.tick(ch, 0.5 * y);
                let [a, b] = &mut self.smear[ch];
                let s = b.tick(a.tick(y));
                self.lines[ch].push(HEADROOM * fast_tanh((*x + fb * s) / HEADROOM));
                *x = s;
            }
        }
    }

    fn reset(&mut self) {
        for l in &mut self.lines {
            l.clear();
        }
        for ap in self.smear.iter_mut().flatten() {
            ap.clear();
        }
        self.cut.reset();
        self.phase = [0.0; 2];
        self.wander.reset(0.0);
        self.time.snap();
        self.fb.snap(self.fb.target);
        self.wobble.snap(self.wobble.target);
    }
}

#[cfg(test)]
mod tests {
    use super::super::tests::{device, impulse, max_step, peak, SR};
    use super::*;

    #[test]
    fn echo_arrives_after_time() {
        let (mut dev, v) = device("OILCAN", &[("time", 5.0), ("wb", 5.0)]);
        let (l, r) = impulse(&mut dev, &v, 1.0);
        let t = (dial_log(5.0, TIME_MS.0, TIME_MS.1) * 0.001 * SR) as usize;
        assert!(peak(&l, 0, t - t / 20).1 < 1e-3, "nothing before the first repeat");
        // The repeat is smeared (two wipers, allpasses, a dark tone): check its energy.
        let energy = |x: &[f32]| x[t - t / 20..t + t / 5].iter().map(|v| v * v).sum::<f32>();
        assert!(energy(&l) > 0.02 && energy(&r) > 0.02, "{} {}", energy(&l), energy(&r));
    }

    #[test]
    fn time_changes_glide() {
        let step = max_step("OILCAN", &[("wb", 0.0), ("sust", 0.0)], &[("time", 8.0)]);
        assert!(step < 0.05, "{step}");
    }
}
