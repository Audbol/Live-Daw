//! Tape Machine (WING manual 11.8): record/playback colour of an analog tape
//! recorder, without the hiss.
//!
//! DRIVE (dB) sets the level onto the tape. The record path pre-emphasises the
//! highs (corner set by the tape speed, like the NAB/IEC curves), saturates
//! with a hysteresis-flavoured curve and de-emphasises again, so the highs
//! saturate first, as on tape. The curve's input shifts against the direction
//! of the signal by an amount that grows with level, which opens a loop like a
//! tape's magnetisation curve (bias keeps quiet signals linear). Small signals
//! pass at unity gain whatever the drive; loud ones compress and pick up odd
//! harmonics. The direction is tracked over the distance the signal travels,
//! not over time, so the loop is continuous and independent of frequency.
//!
//! SPEED (7.5..30 ips) moves the playback response: lower speeds sound darker
//! (high-frequency loss from about 8 kHz at 7.5 ips, 14 kHz at 15 ips, 24 kHz
//! at 30 ips) and move the head bump down (about 26, 48 and 90 Hz). LOW BUMP
//! emulates the low-frequency irregularities of tape playback (the head bump:
//! a lift, a dip an octave above and the low roll-off below it). HIGH SHELV
//! enables the playback high shelf that restores the highs the slower speeds
//! lose (+4 dB at 7.5 ips, +2.5 dB at 15 ips, +1 dB at 30 ips). OUT GAIN is
//! the output level.

use super::{sub_blocks, Cascade, Design, Glide};
use crate::wing::fx::blocks::{db_to_gain, fast_tanh, Biquad, BiquadCoeffs, Smoothed};
use crate::wing::fx::FxAlgo;
use crate::wing::Params;

const EMPHASIS_DB: f64 = 6.0;
/// Width of the hysteresis loop (shift of the curve's input at full level).
const LOOP: f32 = 0.3;
/// Input travel after a reversal over which the loop switches branches.
const TRAVEL: f32 = 0.25;

pub(crate) struct Tape {
    sr: f32,
    pre: Biquad,
    de: Biquad,
    playback: Cascade<5>,
    /// ln(ips), head bump 0..1, high shelf 0..1.
    glide: Glide<3>,
    last: [f32; 2],
    /// Direction of travel per channel, -1..1.
    dir: [f32; 2],
    drive: Smoothed,
    out: Smoothed,
    primed: bool,
}

impl Tape {
    pub(crate) fn new(sr: f32) -> Self {
        let mut t = Self {
            sr,
            pre: Biquad::default(),
            de: Biquad::default(),
            playback: Cascade::new(),
            glide: Glide::new(0.0),
            last: [0.0; 2],
            dir: [0.0; 2],
            drive: Smoothed::new(1.0, 30.0, sr),
            out: Smoothed::new(1.0, 30.0, sr),
            primed: false,
        };
        t.glide.tgt = [15f32.ln(), 1.0, 1.0];
        t.glide.snap();
        t.redesign();
        t
    }

    fn redesign(&mut self) {
        let sr = self.sr as f64;
        let nyq = sr * 0.45;
        let [lips, bump, shelf] = self.glide.cur;
        let k = (lips.exp() / 15.0) as f64;
        let emph = (3183.0 * k).min(nyq);
        self.pre.set_coefficients(BiquadCoeffs::high_shelf(sr, emph, 0.7, EMPHASIS_DB));
        self.de.set_coefficients(BiquadCoeffs::high_shelf(sr, emph, 0.7, -EMPHASIS_DB));

        let fb = 48.0 * k.powf(0.9);
        let (bump, shelf) = (bump as f64, shelf as f64);
        let mut d = Design::<5>::new();
        d.push(BiquadCoeffs::peak(sr, fb, 1.2, 3.0 * bump));
        d.push(BiquadCoeffs::peak(sr, fb * 2.4, 1.0, -1.5 * bump));
        // Low roll-off under the bump; with the bump off it moves down out of the way.
        d.push(BiquadCoeffs::high_pass(sr, (fb / 3.0).powf(bump) * 8f64.powf(1.0 - bump), 0.6));
        let shelf_db = 2.5 - 1.5 * k.log2();
        d.push(BiquadCoeffs::high_shelf(sr, (5000.0 * k.sqrt()).min(nyq), 0.7, shelf_db * shelf));
        d.push(BiquadCoeffs::low_pass(sr, (14_000.0 * k.powf(0.8)).min(nyq), 0.6));
        self.playback.set(d.sections());
    }
}

impl FxAlgo for Tape {
    fn set_params(&mut self, p: &Params) {
        let on = |id: &str| if p.on(id) { 1.0 } else { 0.0 };
        self.glide.tgt = [p.f("spd").ln(), on("low"), on("hi")];
        self.drive.target = db_to_gain(p.f("drv"));
        self.out.target = db_to_gain(p.f("out"));
        if !self.primed {
            self.glide.snap();
            self.redesign();
            self.drive.snap(self.drive.target);
            self.out.snap(self.out.target);
            self.primed = true;
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        let n = left.len().min(right.len());
        sub_blocks(n, |a, b| {
            if self.glide.step() != 0 {
                self.redesign();
            }
            let (l, r) = (&mut left[a..b], &mut right[a..b]);
            self.pre.process(l, 0);
            self.pre.process(r, 1);
            for (x, y) in l.iter_mut().zip(r.iter_mut()) {
                let g = self.drive.tick();
                for (c, v) in [x, y].into_iter().enumerate() {
                    let u = *v * g;
                    // Direction of travel and how far into saturation.
                    let d = u - self.last[c];
                    self.last[c] = u;
                    let dir = &mut self.dir[c];
                    *dir += (d.signum() - *dir) * (d.abs() * (1.0 / TRAVEL)).min(1.0);
                    let t = fast_tanh(u);
                    *v = fast_tanh(u - LOOP * *dir * t * t) / g;
                }
            }
            self.de.process(l, 0);
            self.de.process(r, 1);
            self.playback.process(l, r);
            for (x, y) in l.iter_mut().zip(r.iter_mut()) {
                let o = self.out.tick();
                *x *= o;
                *y *= o;
            }
        });
    }

    fn reset(&mut self) {
        self.pre.reset();
        self.de.reset();
        self.playback.reset();
        self.last = [0.0; 2];
        self.dir = [0.0; 2];
        self.glide.snap();
        self.redesign();
        self.drive.snap(self.drive.target);
        self.out.snap(self.out.target);
    }
}

#[cfg(test)]
mod tests {
    use super::super::tests::Rig;

    #[test]
    fn quiet_mids_pass_loud_signals_compress() {
        let mut rig = Rig::new("TAPE");
        let quiet = rig.sine_gain_db(1000.0, 0.01);
        assert!(quiet.abs() < 1.0, "{quiet}");
        let mut rig = Rig::new("TAPE");
        rig.set("drv", 20.0);
        let loud = rig.sine_gain_db(1000.0, 0.5);
        assert!(loud < -6.0, "{loud}");
    }

    #[test]
    fn high_shelf_restores_the_top() {
        for spd in [7.5, 15.0] {
            let mut off = Rig::new("TAPE");
            off.set("spd", spd).set("hi", 0.0);
            let mut on = Rig::new("TAPE");
            on.set("spd", spd).set("hi", 1.0);
            assert!(on.sine_gain_db(8000.0, 0.01) > off.sine_gain_db(8000.0, 0.01) + 1.5, "{spd} ips");
        }
    }

    #[test]
    fn speed_moves_the_top() {
        let mut slow = Rig::new("TAPE");
        slow.set("spd", 7.5).set("hi", 0.0);
        let mut fast = Rig::new("TAPE");
        fast.set("spd", 30.0).set("hi", 0.0);
        assert!(slow.sine_gain_db(12_000.0, 0.01) < fast.sine_gain_db(12_000.0, 0.01) - 3.0);
    }
}
