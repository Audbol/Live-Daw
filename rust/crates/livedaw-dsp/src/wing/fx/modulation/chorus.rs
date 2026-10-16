//! DIMCRS, CHORUS and FLANGER: short delay lines swept by LFOs and read with
//! Hermite interpolation, so the modulation is smooth and alias-free.

use super::shared::{advance, loop_limit, ms, CutFilter};
use crate::wing::fx::blocks::{shape, DelayLine, Smoothed, Svf, Wave};
use crate::wing::fx::{FxAlgo, FxContext};
use crate::wing::Params;

/// Triangle with rounded turning points (no sudden pitch flip at the ends).
#[inline]
fn rounded_tri(phase: f32) -> f32 {
    let t = shape(Wave::Triangle, phase);
    1.5 * t - 0.5 * t * t * t
}

// ---- DIMCRS ---------------------------------------------------------------------------

/// Rate (Hz) and sweep depth (ms) of mode switches 1-4.
const DIM_MODES: [(f32, f32); 4] = [(0.25, 0.4), (0.35, 0.7), (0.45, 1.0), (0.6, 1.5)];
/// Centre delay of both bucket-brigade lines.
const DIM_BASE_MS: f32 = 5.0;
/// Inverted cross-feed between the two lines.
const DIM_CROSS: f32 = 0.3;
/// Keeps the wet level close to the dry level after the cross-feed.
const DIM_NORM: f32 = 1.15;

/// Dimension Chorus (Roland SDD-320 "Dimension D" style).
///
/// Two bucket-brigade lines around 5 ms are swept by one rounded-triangle LFO
/// in opposite phase; each output carries its own line plus an inverted
/// share of the other, which gives width without an obvious wobble (the
/// modulation partly cancels in mono). SW 1-4 select rate/depth presets and
/// combine: the fastest pressed rate and the sum of the pressed depths
/// (capped at 2.5 ms). With no switch pressed the effect is bypassed (dry
/// only when DRY is on). IN MODE MONO feeds both lines the mono sum, STEREO
/// feeds each line its own channel. DRY on mixes dry and wet 1:1 inside the
/// device, as the original does; off outputs the wet signal only.
pub(super) struct Dimension {
    sr: f32,
    lines: [DelayLine; 2],
    /// Bucket-brigade bandwidth.
    lp: [Svf; 2],
    phase: f32,
    rate: Smoothed,
    /// Sweep depth in samples.
    depth: Smoothed,
    /// 0 when no mode switch is pressed.
    active: Smoothed,
    mono: Smoothed,
    dry: Smoothed,
    base: f32,
    primed: bool,
}

impl Dimension {
    pub(super) fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let base = ms(DIM_BASE_MS, sr);
        let len = (base + ms(3.0, sr)) as usize + 8;
        let mut lp = [Svf::default(); 2];
        for f in &mut lp {
            f.set(8000.0, 0.6, sr);
        }
        Self {
            sr,
            lines: [DelayLine::new(len), DelayLine::new(len)],
            lp,
            phase: 0.0,
            rate: Smoothed::new(0.35, 300.0, sr),
            depth: Smoothed::new(ms(0.7, sr), 300.0, sr),
            active: Smoothed::new(1.0, 30.0, sr),
            mono: Smoothed::new(0.0, 30.0, sr),
            dry: Smoothed::new(1.0, 30.0, sr),
            base,
            primed: false,
        }
    }
}

impl FxAlgo for Dimension {
    fn set_params(&mut self, p: &Params) {
        let pressed = [p.on("sw1"), p.on("sw2"), p.on("sw3"), p.on("sw4")];
        let (mut rate, mut depth, mut n) = (0.0f32, 0.0f32, 0);
        for (&on, (r, d)) in pressed.iter().zip(DIM_MODES) {
            if on {
                rate = rate.max(r);
                depth += d;
                n += 1;
            }
        }
        // With nothing pressed the modulation keeps running while the wet fades out.
        if n > 0 {
            self.rate.target = rate;
            self.depth.target = ms(depth.min(2.5), self.sr);
        }
        self.active.target = if n > 0 { 1.0 } else { 0.0 };
        self.mono.target = if p.choice("in") == 0 { 1.0 } else { 0.0 };
        self.dry.target = if p.on("drysw") { 1.0 } else { 0.0 };
        if !self.primed {
            self.primed = true;
            for s in [&mut self.rate, &mut self.depth, &mut self.active, &mut self.mono, &mut self.dry] {
                s.snap(s.target);
            }
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        let inv_sr = 1.0 / self.sr;
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            let (dl, dr) = (*l, *r);
            let mono = self.mono.tick();
            let m = 0.5 * (dl + dr);
            self.lines[0].push(dl + (m - dl) * mono);
            self.lines[1].push(dr + (m - dr) * mono);

            advance(&mut self.phase, self.rate.tick() * inv_sr);
            let sweep = self.depth.tick() * rounded_tri(self.phase);
            let w1 = self.lp[0].tick(self.lines[0].tap_hermite(self.base + sweep)).lp;
            let w2 = self.lp[1].tick(self.lines[1].tap_hermite(self.base - sweep)).lp;
            let wl = (w1 - DIM_CROSS * w2) * DIM_NORM;
            let wr = (w2 - DIM_CROSS * w1) * DIM_NORM;

            let dry = self.dry.tick();
            let act = self.active.tick();
            let wet_g = act * (1.0 - 0.5 * dry);
            let dry_g = dry * (1.0 - 0.5 * act);
            *l = wl * wet_g + dl * dry_g;
            *r = wr * wet_g + dr * dry_g;
        }
    }

    fn reset(&mut self) {
        for l in &mut self.lines {
            l.clear();
        }
        for f in &mut self.lp {
            f.reset();
        }
        self.phase = 0.0;
        for s in [&mut self.rate, &mut self.depth, &mut self.active, &mut self.mono, &mut self.dry] {
            s.snap(s.target);
        }
    }
}

// ---- CHORUS ---------------------------------------------------------------------------

/// Longest sweep around the centre delay, ms.
const CHORUS_MAX_SWEEP_MS: f32 = 8.0;

/// Stereo Chorus (Boss CE-2 flavoured).
///
/// Each channel has its own delay line (DELAY L/R, 5-50 ms) swept by a shared
/// LFO whose WAVEFORM blends triangle (0) into sine (100); the right sweep is
/// PHASE degrees behind the left. DEPTH L/R sets the sweep as a share of the
/// channel's delay (100 % = +-40 %, at most +-8 ms). SPREAD narrows the wet
/// signal from full L/R separation (100 %) to mono (0 %). LO/HI CUT filter
/// the wet signal; MIX is the device's own dry/wet balance.
pub(super) struct Chorus {
    sr: f32,
    lines: [DelayLine; 2],
    phase: f32,
    inc: f32,
    /// Centre delays in samples.
    base: [Smoothed; 2],
    /// Sweep amplitudes in samples.
    depth: [Smoothed; 2],
    /// 0 = triangle .. 1 = sine.
    blend: Smoothed,
    /// Right LFO phase offset, cycles.
    offset: Smoothed,
    spread: Smoothed,
    mix: Smoothed,
    cut: CutFilter,
    primed: bool,
}

impl Chorus {
    pub(super) fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let len = ms(50.0 + CHORUS_MAX_SWEEP_MS + 2.0, sr) as usize + 8;
        let s = |v: f32, t: f32| Smoothed::new(v, t, sr);
        Self {
            sr,
            lines: [DelayLine::new(len), DelayLine::new(len)],
            phase: 0.0,
            inc: 0.0,
            base: [s(ms(15.0, sr), 120.0), s(ms(20.0, sr), 120.0)],
            depth: [s(0.0, 60.0), s(0.0, 60.0)],
            blend: s(0.5, 30.0),
            offset: s(0.25, 120.0),
            spread: s(0.5, 30.0),
            mix: s(0.5, 20.0),
            cut: CutFilter::new(sr),
            primed: false,
        }
    }

    fn smoothers(&mut self) -> [&mut Smoothed; 8] {
        let [b0, b1] = &mut self.base;
        let [d0, d1] = &mut self.depth;
        [b0, b1, d0, d1, &mut self.blend, &mut self.offset, &mut self.spread, &mut self.mix]
    }
}

/// Triangle blended into sine (`blend` 0..1).
#[inline]
fn tri_sine(phase: f32, blend: f32) -> f32 {
    let t = shape(Wave::Triangle, phase);
    t + (shape(Wave::Sine, phase) - t) * blend
}

impl FxAlgo for Chorus {
    fn set_params(&mut self, p: &Params) {
        let sr = self.sr;
        for (ch, (dly, dep)) in [("dlyl", "depl"), ("dlyr", "depr")].into_iter().enumerate() {
            let d = p.f(dly);
            self.base[ch].target = ms(d, sr);
            self.depth[ch].target = ms((p.f(dep) * 0.01 * 0.4 * d).min(CHORUS_MAX_SWEEP_MS), sr);
        }
        self.blend.target = p.f("wave") * 0.01;
        self.offset.target = p.f("phase") / 360.0;
        self.spread.target = p.f("sprd") * 0.01;
        self.mix.target = p.f("mix") * 0.01;
        self.inc = (p.f("spd") / sr).min(0.5);
        self.cut.set(p.f("lc"), p.f("hc"));
        if !self.primed {
            self.primed = true;
            for s in self.smoothers() {
                s.snap(s.target);
            }
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        self.cut.update(left.len());
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            let (dl, dr) = (*l, *r);
            self.lines[0].push(dl);
            self.lines[1].push(dr);
            advance(&mut self.phase, self.inc);
            let blend = self.blend.tick();
            let off = self.offset.tick();
            let ml = tri_sine(self.phase, blend);
            let mr = tri_sine(self.phase + off, blend);
            let yl = self.lines[0].tap_hermite(self.base[0].tick() + self.depth[0].tick() * ml);
            let yr = self.lines[1].tap_hermite(self.base[1].tick() + self.depth[1].tick() * mr);

            let mid = 0.5 * (yl + yr);
            let side = 0.5 * (yl - yr) * self.spread.tick();
            let wl = self.cut.tick(0, mid + side);
            let wr = self.cut.tick(1, mid - side);
            let mix = self.mix.tick();
            *l = dl + (wl - dl) * mix;
            *r = dr + (wr - dr) * mix;
        }
    }

    fn reset(&mut self) {
        for l in &mut self.lines {
            l.clear();
        }
        self.cut.reset();
        self.phase = 0.0;
        for s in self.smoothers() {
            s.snap(s.target);
        }
    }
}

// ---- FLANGER --------------------------------------------------------------------------

/// Sweep range at 100 % depth: +-2 octaves of delay around DELAY.
const FLANGE_OCTAVES: f32 = 2.0;

/// Stereo Flanger.
///
/// A triangle LFO sweeps each channel's delay exponentially around DELAY L/R
/// (0.5-20 ms): DEPTH 100 % covers a quarter to four times the delay, an even
/// sweep in pitch. PHASE offsets the right LFO. FEED recirculates the delayed
/// signal (negative = inverted, for the hollow flanging sound) through FEED
/// LC/HC and a loop limiter; LO/HI CUT filter the wet signal and MIX is the
/// device's own dry/wet balance (50 % gives the deepest notches).
pub(super) struct Flanger {
    sr: f32,
    lines: [DelayLine; 2],
    phase: f32,
    inc: f32,
    /// Centre delays in samples.
    base: [Smoothed; 2],
    /// Sweep depth 0..1.
    depth: [Smoothed; 2],
    offset: Smoothed,
    fb: Smoothed,
    mix: Smoothed,
    cut: CutFilter,
    fcut: CutFilter,
    primed: bool,
}

impl Flanger {
    pub(super) fn new(ctx: FxContext) -> Self {
        let sr = ctx.sample_rate;
        let len = ms(20.0 * FLANGE_OCTAVES.exp2() + 1.0, sr) as usize + 8;
        let s = |v: f32, t: f32| Smoothed::new(v, t, sr);
        Self {
            sr,
            lines: [DelayLine::new(len), DelayLine::new(len)],
            phase: 0.0,
            inc: 0.0,
            base: [s(ms(2.0, sr), 120.0), s(ms(3.0, sr), 120.0)],
            depth: [s(0.6, 60.0), s(0.6, 60.0)],
            offset: s(0.25, 120.0),
            fb: s(0.5, 30.0),
            mix: s(0.5, 20.0),
            cut: CutFilter::new(sr),
            fcut: CutFilter::new(sr),
            primed: false,
        }
    }

    fn smoothers(&mut self) -> [&mut Smoothed; 7] {
        let [b0, b1] = &mut self.base;
        let [d0, d1] = &mut self.depth;
        [b0, b1, d0, d1, &mut self.offset, &mut self.fb, &mut self.mix]
    }
}

impl FxAlgo for Flanger {
    fn set_params(&mut self, p: &Params) {
        let sr = self.sr;
        self.base[0].target = ms(p.f("dlyl"), sr);
        self.base[1].target = ms(p.f("dlyr"), sr);
        self.depth[0].target = p.f("depl") * 0.01;
        self.depth[1].target = p.f("depr") * 0.01;
        self.offset.target = p.f("phase") / 360.0;
        self.fb.target = p.f("feed") * 0.01;
        self.mix.target = p.f("mix") * 0.01;
        self.inc = (p.f("spd") / sr).min(0.5);
        self.cut.set(p.f("lc"), p.f("hc"));
        self.fcut.set(p.f("flc"), p.f("fhc"));
        if !self.primed {
            self.primed = true;
            for s in self.smoothers() {
                s.snap(s.target);
            }
        }
    }

    fn process(&mut self, left: &mut [f32], right: &mut [f32]) {
        let n = left.len();
        self.cut.update(n);
        self.fcut.update(n);
        for (l, r) in left.iter_mut().zip(right.iter_mut()) {
            advance(&mut self.phase, self.inc);
            let off = self.offset.tick();
            let fb = self.fb.tick();
            let mix = self.mix.tick();
            for (ch, x) in [l, r].into_iter().enumerate() {
                let lfo = shape(Wave::Triangle, self.phase + off * ch as f32);
                let d = self.base[ch].tick() * (FLANGE_OCTAVES * self.depth[ch].tick() * lfo).exp2();
                let y = self.lines[ch].tap_hermite(d);
                let f = self.fcut.tick(ch, y);
                self.lines[ch].push(loop_limit(*x + fb * f));
                let wet = self.cut.tick(ch, y);
                *x += (wet - *x) * mix;
            }
        }
    }

    fn reset(&mut self) {
        for l in &mut self.lines {
            l.clear();
        }
        self.cut.reset();
        self.fcut.reset();
        self.phase = 0.0;
        for s in self.smoothers() {
            s.snap(s.target);
        }
    }
}
