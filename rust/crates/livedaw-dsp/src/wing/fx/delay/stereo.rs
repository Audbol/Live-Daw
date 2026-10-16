//! ST-DL: the stereo delay.

use super::XfadeTap;
use crate::wing::fx::blocks::{DelayLine, Smoothed};
use crate::wing::fx::modulation::shared::{loop_limit, ms, CutFilter, Morph};
use crate::wing::fx::{FxAlgo, FxContext};
use crate::wing::Params;

/// FACTOR choices: 1/4, 1/3, 1/2, 2/3, 3/4, 1, 3/8, 5/4, 4/3, 3/2, 2.
const FACTORS: [f32; 11] = [0.25, 1.0 / 3.0, 0.5, 2.0 / 3.0, 0.75, 1.0, 0.375, 1.25, 4.0 / 3.0, 1.5, 2.0];
/// PATTERN choices as (left, right) multiples of the delay time.
const PATTERNS: [(f32, f32); 9] = [
    (0.5, 1.0),
    (2.0 / 3.0, 1.0),
    (0.75, 1.0),
    (0.875, 1.0),
    (1.0, 1.0),
    (1.0, 1.125),
    (1.0, 1.25),
    (1.0, 4.0 / 3.0),
    (1.0, 1.5),
];
/// TIME x FACTOR x PATTERN + OFFSET at their largest.
const MAX_MS: f32 = 3000.0 * 2.0 * 1.5 + 50.0;
const FADE_MS: f32 = 50.0;

const MODE_ST: usize = 0;
const MODE_X: usize = 1;

/// Stereo Delay.
///
/// The delay time is TIME x FACTOR (a note fraction); PATTERN sets the L:R
/// ratio of the two delays and OFFSET adds up to +-50 ms to the right one.
/// MODE ST runs two independent delays; X feeds the mono sum into the left
/// delay and crosses the feedback over (left repeats feed the right delay
/// and vice versa), so repeats ping-pong; M runs one mono delay (left time)
/// on both outputs. FEED is the level of each repeat, through FEED LCUT /
/// HCUT and a loop limiter (100 % holds the repeats without running away);
/// LO / HI CUT filter the output. Time changes crossfade to the new
/// position; mode changes switch under a short fade.
pub(super) struct StereoDelay {
    sr: f32,
    lines: [DelayLine; 2],
    taps: [XfadeTap; 2],
    fb: Smoothed,
    fcut: CutFilter,
    cut: CutFilter,
    mode: usize,
    next_mode: usize,
    morph: Morph,
    primed: bool,
}

impl StereoDelay {
    pub(super) fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let len = ms(MAX_MS, sr) as usize + 4;
        Self {
            sr,
            lines: [DelayLine::new(len), DelayLine::new(len)],
            taps: [XfadeTap::new(FADE_MS, sr); 2],
            fb: Smoothed::new(0.3, 30.0, sr),
            fcut: CutFilter::new(sr),
            cut: CutFilter::new(sr),
            mode: MODE_ST,
            next_mode: MODE_ST,
            morph: Morph::new(10.0, sr),
            primed: false,
        }
    }
}

impl FxAlgo for StereoDelay {
    fn set_params(&mut self, p: &Params) {
        let sr = self.sr;
        let base = p.f("time") * FACTORS.get(p.choice("fact")).copied().unwrap_or(1.0);
        let (pl, pr) = PATTERNS.get(p.choice("pat")).copied().unwrap_or((1.0, 1.0));
        let max = self.lines[0].max_delay() - 1;
        // Taps are read before the new sample is written: index = delay - 1.
        let tap = |t_ms: f32| (ms(t_ms, sr).round() as usize).saturating_sub(1).min(max);
        self.taps[0].set(tap(base * pl));
        self.taps[1].set(tap(base * pr + p.f("offs")));
        self.fb.target = p.f("feed") * 0.01;
        self.fcut.set(p.f("flc"), p.f("fhc"));
        self.cut.set(p.f("lc"), p.f("hc"));
        self.next_mode = p.choice("mode").min(2);
        if !self.primed {
            self.primed = true;
            self.mode = self.next_mode;
            for t in &mut self.taps {
                t.snap();
            }
            self.fb.snap(self.fb.target);
        } else if self.next_mode != self.mode {
            self.morph.request();
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        let n = left.len();
        self.fcut.update(n);
        self.cut.update(n);
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            if self.morph.tick() {
                self.mode = self.next_mode;
            }
            let fb = self.fb.tick();
            let yl = self.taps[0].read(&self.lines[0]);
            let yr = self.taps[1].read(&self.lines[1]);
            let fl = self.fcut.tick(0, yl);
            let fr = self.fcut.tick(1, yr);
            let (il, ir, ol, or) = match self.mode {
                MODE_ST => (*l + fb * fl, *r + fb * fr, yl, yr),
                MODE_X => (0.5 * (*l + *r) + fb * fr, fb * fl, yl, yr),
                _ => {
                    // Mono: the right line mirrors the left so a mode change finds it full.
                    let m = 0.5 * (*l + *r) + fb * fl;
                    (m, m, yl, yl)
                }
            };
            self.lines[0].push(loop_limit(il));
            self.lines[1].push(loop_limit(ir));
            let g = self.morph.gain();
            *l = self.cut.tick(0, ol) * g;
            *r = self.cut.tick(1, or) * g;
        }
    }

    fn reset(&mut self) {
        for l in &mut self.lines {
            l.clear();
        }
        for t in &mut self.taps {
            t.snap();
        }
        self.fcut.reset();
        self.cut.reset();
        self.mode = self.next_mode;
        self.morph.reset();
        self.fb.snap(self.fb.target);
    }
}

#[cfg(test)]
mod tests {
    use super::super::tests::{device, impulse, max_step, peak, SR};

    #[test]
    fn echo_lands_at_time() {
        let open = [("feed", 0.0), ("lc", 20.0), ("hc", 20_000.0)];
        let (mut dev, v) = device("ST-DL", &open);
        let (l, r) = impulse(&mut dev, &v, 1.0);
        let at = (0.375 * SR) as usize;
        let (i, p) = peak(&l, 100, l.len());
        assert!(i.abs_diff(at) <= 3 && p > 0.4, "echo at {i} ({p})");
        assert!(peak(&r, 100, l.len()).0.abs_diff(at) <= 3);
        assert!(peak(&l, 0, at - 200).1 < 1e-3, "nothing before the echo");

        // FACTOR 1/2 and PATTERN 1/2:1 with a 10 ms OFFSET on the right.
        let (mut dev, v) = device("ST-DL", &[open[0], open[1], open[2], ("fact", 2.0), ("pat", 0.0), ("offs", 10.0)]);
        let (l, r) = impulse(&mut dev, &v, 1.0);
        assert!(peak(&l, 100, l.len()).0.abs_diff((0.09375 * SR) as usize) <= 3);
        assert!(peak(&r, 100, r.len()).0.abs_diff((0.1975 * SR) as usize) <= 3);
    }

    #[test]
    fn cross_mode_ping_pongs() {
        let settings =
            [("lc", 20.0), ("hc", 20_000.0), ("flc", 20.0), ("fhc", 20_000.0), ("mode", 1.0), ("time", 100.0), ("feed", 50.0)];
        let (mut dev, v) = device("ST-DL", &settings);
        let (l, r) = impulse(&mut dev, &v, 0.5);
        let t = (0.1 * SR) as usize;
        let at = |x: &[f32], k: usize| peak(x, k * t - 50, k * t + 50).1;
        assert!(at(&l, 1) > 0.3 && at(&r, 1) < 1e-3);
        assert!(at(&r, 2) > 0.1 && at(&l, 2) < 1e-3);
        assert!(at(&l, 3) > 0.03 && at(&r, 3) < 1e-3);
    }

    #[test]
    fn time_changes_crossfade() {
        let step = max_step("ST-DL", &[("feed", 0.0)], &[("time", 500.0)]);
        assert!(step < 0.04, "{step}");
    }
}
