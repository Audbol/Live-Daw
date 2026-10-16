//! TAP-DL: the Ultratap multitap delay.

use super::smoothstep;
use crate::wing::fx::blocks::{db_to_gain, Allpass, DelayLine, Smoothed};
use crate::wing::fx::modulation::shared::{ms, CutFilter};
use crate::wing::fx::{FxAlgo, FxContext};
use crate::wing::Params;

const MAX_TAPS: usize = 16;
/// FACTOR choices: 1/3, 1/2, 2/3, 3/4, 1, 5/4, 4/3, 3/2, 2.
const FACTORS: [f32; 9] = [1.0 / 3.0, 0.5, 2.0 / 3.0, 0.75, 1.0, 1.25, 4.0 / 3.0, 1.5, 2.0];
/// PRE DLY + TIME x FACTOR at their largest.
const MAX_MS: f32 = 500.0 + 2000.0 * 2.0;
const FADE_MS: f32 = 60.0;
/// Diffusion allpasses per channel.
const DIFF_MS: [[f32; 3]; 2] = [[7.1, 11.3, 17.9], [7.9, 12.7, 19.3]];
/// FOCUS / SPREAD tap spacing curve.
const CURVE: f32 = 1.7;

/// One layout of the taps: delay (samples) and left/right gain of each.
#[derive(Clone, Copy, PartialEq)]
struct TapSet {
    n: usize,
    d: [usize; MAX_TAPS],
    gl: [f32; MAX_TAPS],
    gr: [f32; MAX_TAPS],
}

impl TapSet {
    const EMPTY: Self = Self { n: 0, d: [0; MAX_TAPS], gl: [0.0; MAX_TAPS], gr: [0.0; MAX_TAPS] };

    /// The taps for the current controls (see [`Ultratap`]).
    fn layout(p: &Params, sr: f32, max: usize) -> Self {
        let n = (p.f("rep").round() as usize).clamp(1, MAX_TAPS);
        let span = p.f("time") * FACTORS.get(p.choice("fact")).copied().unwrap_or(1.0);
        let pre = p.f("pdel");
        let slope = p.f("slp");
        let width = p.f("wid") * 0.01;
        let mode = p.choice("mode");
        let top = slope.max(0.0);
        let mut set = Self::EMPTY;
        set.n = n;
        let mut power = 0.0;
        let taps = set.d.iter_mut().zip(set.gl.iter_mut()).zip(set.gr.iter_mut()).enumerate().take(n);
        for (i, ((d, gl), gr)) in taps {
            let t = (i + 1) as f32 / n as f32;
            // 0 at the first tap, 1 at the last.
            let u = if n > 1 { i as f32 / (n - 1) as f32 } else { 0.0 };
            let side = if i % 2 == 0 { -1.0 } else { 1.0 };
            let (pos, pan) = match mode {
                0 => (t, 2.0 * u - 1.0),
                1 => (t, side),
                2 => (1.0 - (1.0 - t).powf(CURVE), side * (1.0 - u)),
                _ => (t.powf(CURVE), side * u),
            };
            let pan = if n > 1 { pan * width } else { 0.0 };
            let level = db_to_gain(slope * u - top);
            *d = (ms(pre + span * pos, sr).round() as usize).clamp(1, max);
            *gl = level * (1.0 - pan).min(1.0);
            *gr = level * (1.0 + pan).min(1.0);
            power += level * level;
        }
        // Many taps add up: keep the total power near that of two full taps.
        let norm = (2.0 / power.max(1e-6)).sqrt().min(1.0);
        for g in set.gl.iter_mut().take(n).chain(set.gr.iter_mut().take(n)) {
            *g *= norm;
        }
        set
    }

    #[inline]
    fn read(&self, line: &DelayLine) -> (f32, f32) {
        let (mut l, mut r) = (0.0, 0.0);
        for ((&d, &gl), &gr) in self.d.iter().zip(&self.gl).zip(&self.gr).take(self.n) {
            let x = line.tap(d);
            l += x * gl;
            r += x * gr;
        }
        (l, r)
    }
}

/// Ultratap Delay (Korg SDD-3000 / Eventide Ultratap style multitap).
///
/// REPEATS (1-16) taps of the mono input spread over TIME x FACTOR after PRE
/// DLY, without feedback. SLOPE tilts their levels from the first to the
/// last tap (negative = fading echoes, positive = swelling); the total is
/// normalised so more taps don't get louder. MODE sets spacing and panning:
/// MOVE evenly spaced taps sweeping left to right, JUMP evenly spaced taps
/// alternating left/right, FOCUS taps that crowd together towards the end
/// and close in from the sides to the centre, SPREAD taps that spread out
/// and fan out from the centre. WIDTH scales the panning (negative mirrors
/// it). DIFFUSION smears every tap through allpasses into a short cloud; LO
/// / HI CUT filter the output. Any change of the tap layout crossfades to
/// the new one.
pub(super) struct Ultratap {
    line: DelayLine,
    sr: f32,
    cur: TapSet,
    next: TapSet,
    target: TapSet,
    dirty: bool,
    fade: f32,
    step: f32,
    diff: [[Allpass; 3]; 2],
    amount: Smoothed,
    cut: CutFilter,
    primed: bool,
}

impl Ultratap {
    pub(super) fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let ap = |d: f32| Allpass::new(ms(d, sr) as usize + 2, ms(d, sr).round().max(1.0), 0.5);
        Self {
            line: DelayLine::new(ms(MAX_MS, sr) as usize + 4),
            sr,
            cur: TapSet::EMPTY,
            next: TapSet::EMPTY,
            target: TapSet::EMPTY,
            dirty: false,
            fade: 0.0,
            step: 1.0 / ms(FADE_MS, sr).max(1.0),
            diff: DIFF_MS.map(|ch| ch.map(ap)),
            amount: Smoothed::new(0.2, 50.0, sr),
            cut: CutFilter::new(sr),
            primed: false,
        }
    }
}

impl FxAlgo for Ultratap {
    fn set_params(&mut self, p: &Params) {
        self.target = TapSet::layout(p, self.sr, self.line.max_delay());
        self.dirty = true;
        self.amount.target = p.f("diff") * 0.01;
        self.cut.set(p.f("lc"), p.f("hc"));
        if !self.primed {
            self.primed = true;
            self.cur = self.target;
            self.next = self.target;
            self.amount.snap(self.amount.target);
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        self.cut.update(left.len());
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            if self.dirty && self.fade == 0.0 {
                self.dirty = false;
                if self.target != self.cur {
                    self.next = self.target;
                    self.fade = self.step;
                }
            }
            self.line.push(0.5 * (*l + *r));
            let (mut yl, mut yr) = self.cur.read(&self.line);
            if self.fade > 0.0 {
                let (bl, br) = self.next.read(&self.line);
                let w = smoothstep(self.fade.min(1.0));
                yl += (bl - yl) * w;
                yr += (br - yr) * w;
                self.fade += self.step;
                if self.fade >= 1.0 {
                    self.cur = self.next;
                    self.fade = 0.0;
                }
            }

            let amt = self.amount.tick();
            let g = 0.45 + 0.25 * amt;
            for (ch, (x, y)) in [(l, yl), (r, yr)].into_iter().enumerate() {
                let mut z = y;
                for ap in &mut self.diff[ch] {
                    ap.gain = g;
                    z = ap.tick(z);
                }
                *x = self.cut.tick(ch, y + (z - y) * amt);
            }
        }
    }

    fn reset(&mut self) {
        self.line.clear();
        for ap in self.diff.iter_mut().flatten() {
            ap.clear();
        }
        self.cut.reset();
        self.cur = self.target;
        self.next = self.target;
        self.fade = 0.0;
        self.dirty = false;
        self.amount.snap(self.amount.target);
    }
}

#[cfg(test)]
mod tests {
    use super::super::tests::{device, impulse, max_step, peak, SR};

    #[test]
    fn taps_land_on_the_grid() {
        // Four evenly spaced taps over 400 ms, all at the same level.
        let (mut dev, v) = device(
            "TAP-DL",
            &[("rep", 4.0), ("mode", 0.0), ("time", 400.0), ("slp", 0.0), ("wid", 0.0), ("diff", 0.0), ("hc", 20_000.0)],
        );
        let (l, _) = impulse(&mut dev, &v, 0.6);
        let ms = |t: f32| (t * 0.001 * SR) as usize;
        for k in 1..=4 {
            let (i, p) = peak(&l, ms(100.0 * k as f32 - 20.0), ms(100.0 * k as f32 + 20.0));
            assert!(i.abs_diff(ms(100.0 * k as f32)) <= 3 && p > 0.3, "tap {k} at {i} ({p})");
        }
        assert!(peak(&l, ms(130.0), ms(170.0)).1 < 0.01);
        assert!(peak(&l, ms(430.0), l.len()).1 < 0.01);
    }

    #[test]
    fn jump_alternates_sides_and_slope_tilts() {
        let (mut dev, v) = device(
            "TAP-DL",
            &[("rep", 2.0), ("mode", 1.0), ("time", 200.0), ("slp", -12.0), ("wid", 100.0), ("diff", 0.0), ("hc", 20_000.0)],
        );
        let (l, r) = impulse(&mut dev, &v, 0.3);
        let ms = |t: f32| (t * 0.001 * SR) as usize;
        let (first_l, first_r) = (peak(&l, ms(90.0), ms(110.0)).1, peak(&r, ms(90.0), ms(110.0)).1);
        let (second_l, second_r) = (peak(&l, ms(190.0), ms(210.0)).1, peak(&r, ms(190.0), ms(210.0)).1);
        assert!(first_l > 0.3 && first_r < 1e-3, "{first_l} {first_r}");
        assert!(second_r > 0.05 && second_l < 1e-3, "{second_l} {second_r}");
        // -12 dB from the first tap to the last.
        assert!((second_r / first_l - 0.25).abs() < 0.03, "{}", second_r / first_l);
    }

    #[test]
    fn layout_changes_crossfade() {
        let step = max_step("TAP-DL", &[], &[("time", 450.0), ("rep", 9.0)]);
        assert!(step < 0.04, "{step}");
    }
}
