//! The common reverb voice most devices are built on: a 6 dB/oct damping
//! filter, input diffusion (optionally with the 480L's pre-echo loop around
//! it), the pre-delay with its build-up taps (one per tank line), the FDN
//! tank and the output stage. A device fills a [`Settings`] from its controls;
//! everything glides to new settings at the control rate.

use super::parts::{pole, rate, BuildUp, Decay, Diffuser, Echo, Fdn, Output, Param, CTL};

/// What a device asks of its voice (targets; the voice smooths them).
#[derive(Clone, Copy, Debug)]
pub(super) struct Settings {
    pub pre_ms: f32,
    /// Mean tank line length (SIZE).
    pub size_ms: f32,
    pub decay: Decay,
    /// Input (and in-loop) diffusion 0..1.
    pub diffusion: f32,
    /// Build-up contour: 0 = front-loaded, 0.5 = even, 1 = slow swell.
    pub shape: f32,
    /// Duration of the build-up after the pre-delay.
    pub build_ms: f32,
    /// Fade-in of the build-up (0 = none).
    pub ramp_ms: f32,
    /// The 6 dB/oct high cut ahead of everything (DAMPING; >= 20 kHz = off).
    pub damp_hz: f32,
    pub mod_hz: f32,
    pub mod_ms: f32,
    pub lc: f32,
    pub hc: f32,
    pub width: f32,
    pub gain: f32,
    /// Tank input gain (0 holds a frozen tail).
    pub input: f32,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            pre_ms: 0.0,
            size_ms: 50.0,
            decay: Decay::lexicon(2.0, 1.0, 500.0, 8000.0),
            diffusion: 0.7,
            shape: 0.5,
            build_ms: 40.0,
            ramp_ms: 0.0,
            damp_hz: 20000.0,
            mod_hz: 0.5,
            mod_ms: 0.3,
            lc: 20.0,
            hc: 20000.0,
            width: 1.0,
            gain: 1.0,
            input: 1.0,
        }
    }
}

/// Fixed properties of a voice.
pub(super) struct Config<const N: usize> {
    /// Relative line lengths (see `parts::spread`).
    pub rel: [f32; N],
    pub max_size_ms: f32,
    pub max_pre_ms: f32,
    /// In-loop allpasses, gain `[a, b]`: `a + b * diffusion`.
    pub loop_ap: Option<[f32; 2]>,
    /// Input diffuser stage delays (ms) at `diff_ref_ms` of size.
    pub diff_ms: [f32; 4],
    pub diff_ref_ms: f32,
    pub seed: u32,
}

/// The build-up taps' share in the output (see [`Space::early_mix`]): enough
/// to fill the gap before the tank answers without rising above it.
const EARLY_MIX: f32 = 0.3;

pub(super) struct Space<const N: usize> {
    sr: f32,
    pub build: BuildUp<N>,
    diff_l: Diffuser,
    diff_r: Diffuser,
    pub fdn: Fdn<N>,
    pub out: Output,
    /// Pre-echo loop around the diffusion (ROOM, CHAMBER, PLATE).
    pub echo: Option<Echo>,
    /// The pre-echoes due in the output with the current sample.
    pub echo_out: (f32, f32),
    /// Tank output gain (level calibration).
    pub cal: f32,
    /// How much of the build-up taps (the early sound right after the
    /// pre-delay) [`Self::tank`] adds to the tank's output, so the reverb
    /// starts at PRE DLY instead of a line length later.
    pub early_mix: f32,
    damp: Param,
    a_damp: f32,
    zd: [f32; 2],
    size: Param,
    bands: [Param; 4],
    xo: [Param; 3],
    diffusion: Param,
    mod_hz: Param,
    mod_depth: Param,
    input: Param,
    diff_ref: f32,
    loop_ap: Option<[f32; 2]>,
    a_fast: f32,
    a_slow: f32,
    force: bool,
}

impl<const N: usize> Space<N> {
    pub fn new(sr: f32, c: &Config<N>) -> Self {
        let max_scale = (c.max_size_ms / c.diff_ref_ms).sqrt().max(1.0);
        Self {
            sr,
            build: BuildUp::new(sr, c.max_pre_ms + 2.9 * c.max_size_ms + 20.0, c.seed ^ 0xB1D),
            diff_l: Diffuser::new(sr, c.diff_ms, max_scale),
            diff_r: Diffuser::new(sr, c.diff_ms.map(|m| m * 1.09), max_scale),
            fdn: Fdn::new(sr, c.rel, c.max_size_ms, c.loop_ap.is_some(), c.seed),
            out: Output::new(sr),
            echo: None,
            echo_out: (0.0, 0.0),
            cal: 1.0,
            early_mix: EARLY_MIX,
            damp: Param::default(),
            a_damp: 1.0,
            zd: [0.0; 2],
            size: Param::default(),
            bands: [Param::default(); 4],
            xo: [Param::default(); 3],
            diffusion: Param::default(),
            mod_hz: Param::default(),
            mod_depth: Param::default(),
            input: Param::default(),
            diff_ref: c.diff_ref_ms * 0.001 * sr,
            loop_ap: c.loop_ap,
            a_fast: rate(40.0, sr),
            a_slow: rate(250.0, sr),
            force: true,
        }
    }

    /// New targets.
    pub fn set(&mut self, s: &Settings) {
        let ms = 0.001 * self.sr;
        self.size.set(s.size_ms * ms);
        for (p, &v) in self.bands.iter_mut().zip(&s.decay.bands) {
            p.set(v);
        }
        for (p, &v) in self.xo.iter_mut().zip(&s.decay.xo) {
            p.set(v);
        }
        self.diffusion.set(s.diffusion);
        self.mod_hz.set(s.mod_hz);
        self.mod_depth.set(s.mod_ms * ms);
        self.input.set(s.input);
        self.build.pre.set(s.pre_ms * ms);
        self.build.span.set(s.build_ms * ms);
        self.build.shape.set(s.shape);
        self.build.ramp.set(s.ramp_ms * ms);
        self.damp.set(s.damp_hz);
        self.out.lc.set(s.lc);
        self.out.hc.set(s.hc);
        self.out.width.set(s.width);
        self.out.gain.set(s.gain);
    }

    /// Jumps to the targets (first parameters, reset).
    pub fn snap(&mut self) {
        for p in [&mut self.size, &mut self.diffusion, &mut self.mod_hz, &mut self.mod_depth, &mut self.input, &mut self.damp] {
            p.snap();
        }
        if let Some(e) = &mut self.echo {
            e.snap();
        }
        for p in self.bands.iter_mut().chain(&mut self.xo) {
            p.snap();
        }
        self.build.snap();
        self.out.snap();
        self.force = true;
    }

    pub fn clear(&mut self) {
        self.build.clear();
        self.diff_l.clear();
        self.diff_r.clear();
        self.fdn.clear();
        self.out.clear();
        if let Some(e) = &mut self.echo {
            e.clear();
        }
        self.zd = [0.0; 2];
        self.echo_out = (0.0, 0.0);
    }

    /// Once per control step.
    pub fn control(&mut self) {
        let force = std::mem::take(&mut self.force);
        let a = self.a_fast;
        // SIZE glides at most ~5 % of a sample per sample: a line's pitch shifts
        // compound on every pass round the loop, so a faster glide swoops.
        let sized = self.size.step_limited(self.a_slow, 0.055 * CTL as f32) || force;
        let mut decayed = false;
        for p in self.bands.iter_mut().chain(&mut self.xo) {
            decayed |= p.step(a);
        }
        let diffused = self.diffusion.step(a);
        let modded = self.mod_hz.step(a) | self.mod_depth.step(a);
        self.input.step(a);
        if sized {
            self.fdn.set_size(self.size.cur, self.size.moving());
        }
        if sized || diffused {
            let moving = self.size.moving();
            let scale = (self.size.cur / self.diff_ref).sqrt().clamp(0.3, 4.0);
            self.diff_l.set(scale, self.diffusion.cur, moving);
            self.diff_r.set(scale, self.diffusion.cur, moving);
            if let Some([a0, a1]) = self.loop_ap {
                self.fdn.set_ap_gain(a0 + a1 * self.diffusion.cur);
            }
        }
        if sized || decayed {
            let d = Decay { bands: self.bands.map(|p| p.cur), xo: self.xo.map(|p| p.cur) };
            self.fdn.set_decay(&d);
        }
        if sized || modded {
            self.fdn.set_mod(self.mod_hz.cur, self.mod_depth.cur);
        }
        if self.damp.step(a) || force {
            self.a_damp = if self.damp.cur >= 19_999.0 { 1.0 } else { pole(self.damp.cur, self.sr) };
        }
        if let Some(e) = &mut self.echo {
            e.control(a);
        }
        self.fdn.control();
        self.build.control(self.a_slow, force);
        self.out.control(a, force);
    }

    /// DAMPING: the 6 dB/oct high cut, applied to the input so it shapes
    /// the early reflections and the tail alike.
    #[inline]
    pub fn damp(&mut self, xl: f32, xr: f32) -> (f32, f32) {
        let a = self.a_damp;
        self.zd[0] += a * (xl - self.zd[0]);
        self.zd[1] += a * (xr - self.zd[1]);
        (self.zd[0], self.zd[1])
    }

    /// Diffusion, pre-delay, build-up and tank: the raw stereo reverb (scaled
    /// by `cal`).
    #[inline]
    pub fn tank(&mut self, xl: f32, xr: f32) -> (f32, f32) {
        let ((el, er), (tl, tr)) = self.tank_split(xl, xr);
        let g = self.early_mix;
        (tl + g * el, tr + g * er)
    }

    /// As [`Self::tank`], also returning the summed build-up taps that feed
    /// the tank (an ambience's early sound).
    #[inline]
    pub fn tank_split(&mut self, mut xl: f32, mut xr: f32) -> ((f32, f32), (f32, f32)) {
        if let Some(e) = &mut self.echo {
            let [el, er] = e.read();
            xl += e.feed[0].cur * el;
            xr += e.feed[1].cur * er;
            self.echo_out = (e.level[0].cur * el, e.level[1].cur * er);
        }
        let dl = self.diff_l.tick(xl);
        let dr = self.diff_r.tick(xr);
        if let Some(e) = &mut self.echo {
            e.write(dl, dr);
        }
        let (lines, early) = self.build.tick(dl, dr);
        let (l, r) = self.fdn.tick(&lines, self.input.cur);
        (early, (l * self.cal, r * self.cal))
    }
}
