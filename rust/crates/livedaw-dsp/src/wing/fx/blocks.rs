//! Building blocks shared by the FX devices. Constructors allocate (message
//! thread); every per-sample method is allocation-free and branch-light.

pub use crate::biquad::{Biquad, BiquadCoeffs};
pub use crate::util::{db_to_gain, gain_to_db};

use std::f32::consts::{PI, TAU};

/// A circular delay line with integer, linear and Hermite (cubic) taps.
#[derive(Clone)]
pub struct DelayLine {
    buf: Vec<f32>,
    mask: usize,
    pos: usize,
}

impl DelayLine {
    /// Holds at least `max_delay` samples of history.
    pub fn new(max_delay: usize) -> Self {
        let len = (max_delay + 4).next_power_of_two();
        Self { buf: vec![0.0; len], mask: len - 1, pos: 0 }
    }

    /// Longest delay a tap can read.
    pub fn max_delay(&self) -> usize {
        self.buf.len() - 4
    }

    pub fn clear(&mut self) {
        self.buf.fill(0.0);
    }

    /// Writes the next sample.
    #[inline]
    pub fn push(&mut self, x: f32) {
        self.pos = (self.pos + 1) & self.mask;
        self.buf[self.pos] = x;
    }

    /// The sample written `d` pushes ago (0 = the latest).
    #[inline]
    pub fn tap(&self, d: usize) -> f32 {
        self.buf[(self.pos.wrapping_sub(d.min(self.mask))) & self.mask]
    }

    /// Fractional delay, linear interpolation.
    #[inline]
    pub fn tap_linear(&self, d: f32) -> f32 {
        let d = d.clamp(0.0, self.max_delay() as f32);
        let i = d as usize;
        let f = d - i as f32;
        let a = self.tap(i);
        let b = self.tap(i + 1);
        a + (b - a) * f
    }

    /// Fractional delay, 4-point Hermite interpolation (for modulated taps).
    #[inline]
    pub fn tap_hermite(&self, d: f32) -> f32 {
        let d = d.clamp(1.0, self.max_delay() as f32 - 2.0);
        let i = d as usize;
        let f = d - i as f32;
        let (xm1, x0, x1, x2) = (self.tap(i - 1), self.tap(i), self.tap(i + 1), self.tap(i + 2));
        let c1 = 0.5 * (x1 - xm1);
        let c2 = xm1 - 2.5 * x0 + 2.0 * x1 - 0.5 * x2;
        let c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
        ((c3 * f + c2) * f + c1) * f + x0
    }
}

/// Schroeder allpass diffuser, optionally with a modulated (fractional) delay.
#[derive(Clone)]
pub struct Allpass {
    line: DelayLine,
    pub delay: f32,
    pub gain: f32,
}

impl Allpass {
    pub fn new(max_delay: usize, delay: f32, gain: f32) -> Self {
        Self { line: DelayLine::new(max_delay + 2), delay, gain }
    }

    pub fn clear(&mut self) {
        self.line.clear();
    }

    #[inline]
    pub fn tick(&mut self, x: f32) -> f32 {
        self.tick_at(x, self.delay)
    }

    /// With the delay moved by `delay` samples (modulation).
    #[inline]
    pub fn tick_at(&mut self, x: f32, delay: f32) -> f32 {
        let delay = delay.max(1.0);
        let d = if delay.fract() == 0.0 { self.line.tap(delay as usize - 1) } else { self.line.tap_linear(delay - 1.0) };
        let v = x + self.gain * d;
        self.line.push(v);
        d - self.gain * v
    }
}

/// One-pole low-pass (and high-pass as `x - lp`).
#[derive(Clone, Copy, Default)]
pub struct OnePole {
    a: f32,
    z: f32,
}

impl OnePole {
    /// Cutoff in Hz.
    pub fn new(hz: f32, sr: f32) -> Self {
        let mut f = Self::default();
        f.set(hz, sr);
        f
    }

    pub fn set(&mut self, hz: f32, sr: f32) {
        let hz = hz.clamp(1.0, sr * 0.49);
        self.a = 1.0 - (-TAU * hz / sr).exp();
    }

    /// Sets the coefficient directly (0 = frozen, 1 = no filtering).
    pub fn set_coef(&mut self, a: f32) {
        self.a = a.clamp(0.0, 1.0);
    }

    pub fn reset(&mut self) {
        self.z = 0.0;
    }

    #[inline]
    pub fn lp(&mut self, x: f32) -> f32 {
        self.z += self.a * (x - self.z);
        self.z
    }

    #[inline]
    pub fn hp(&mut self, x: f32) -> f32 {
        x - self.lp(x)
    }
}

/// DC blocker (about 5 Hz).
#[derive(Clone, Copy, Default)]
pub struct DcBlock {
    x1: f32,
    y1: f32,
    r: f32,
}

impl DcBlock {
    pub fn new(sr: f32) -> Self {
        Self { x1: 0.0, y1: 0.0, r: (1.0 - TAU * 5.0 / sr).clamp(0.9, 0.99999) }
    }

    pub fn reset(&mut self) {
        self.x1 = 0.0;
        self.y1 = 0.0;
    }

    #[inline]
    pub fn tick(&mut self, x: f32) -> f32 {
        let y = x - self.x1 + self.r * self.y1;
        self.x1 = x;
        self.y1 = y;
        y
    }
}

/// Topology-preserving state variable filter (Zavalishin / Simper): low-pass,
/// band-pass and high-pass at once, stable under fast modulation.
#[derive(Clone, Copy, Default)]
pub struct Svf {
    g: f32,
    k: f32,
    a1: f32,
    a2: f32,
    a3: f32,
    ic1: f32,
    ic2: f32,
}

/// Outputs of one [`Svf`] step.
#[derive(Clone, Copy, Default)]
pub struct SvfOut {
    pub lp: f32,
    pub bp: f32,
    pub hp: f32,
}

impl SvfOut {
    pub fn notch(&self) -> f32 {
        self.lp + self.hp
    }
}

impl Svf {
    /// `q` 0.5 (soft) .. 20+ (resonant).
    pub fn set(&mut self, hz: f32, q: f32, sr: f32) {
        let hz = hz.clamp(5.0, sr * 0.45);
        self.g = (PI * hz / sr).tan();
        self.k = 1.0 / q.max(0.05);
        self.a1 = 1.0 / (1.0 + self.g * (self.g + self.k));
        self.a2 = self.g * self.a1;
        self.a3 = self.g * self.a2;
    }

    pub fn reset(&mut self) {
        self.ic1 = 0.0;
        self.ic2 = 0.0;
    }

    #[inline]
    pub fn tick(&mut self, x: f32) -> SvfOut {
        let v3 = x - self.ic2;
        let v1 = self.a1 * self.ic1 + self.a2 * v3;
        let v2 = self.ic2 + self.a2 * self.ic1 + self.a3 * v3;
        self.ic1 = 2.0 * v1 - self.ic1;
        self.ic2 = 2.0 * v2 - self.ic2;
        SvfOut { lp: v2, bp: v1, hp: x - self.k * v1 - v2 }
    }
}

/// LFO waveforms, `phase` in 0..1, output -1..1.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Wave {
    Sine,
    Triangle,
    SawUp,
    SawDown,
    Square,
    /// Smoothed random (sample & hold through a slew).
    Random,
}

/// A low-frequency oscillator.
#[derive(Clone, Copy)]
pub struct Lfo {
    pub phase: f32,
    inc: f32,
    rng: u32,
    held: f32,
    slewed: f32,
}

impl Default for Lfo {
    fn default() -> Self {
        Self { phase: 0.0, inc: 0.0, rng: 0x9E37_79B9, held: 0.0, slewed: 0.0 }
    }
}

impl Lfo {
    pub fn set_rate(&mut self, hz: f32, sr: f32) {
        self.inc = (hz / sr).clamp(0.0, 0.5);
    }

    pub fn reset(&mut self, phase: f32) {
        self.phase = phase.rem_euclid(1.0);
        self.held = 0.0;
        self.slewed = 0.0;
    }

    /// Advances one sample and returns the waveform at the new phase, offset by
    /// `offset` (0..1, e.g. 0.25 for the other channel).
    #[inline]
    pub fn next(&mut self, wave: Wave, offset: f32) -> f32 {
        self.phase += self.inc;
        if self.phase >= 1.0 {
            self.phase -= 1.0;
            self.rng ^= self.rng << 13;
            self.rng ^= self.rng >> 17;
            self.rng ^= self.rng << 5;
            self.held = (self.rng as f32 / u32::MAX as f32) * 2.0 - 1.0;
        }
        if wave == Wave::Random {
            self.slewed += (self.held - self.slewed) * (self.inc * 8.0).min(1.0);
            return self.slewed;
        }
        shape(wave, self.phase + offset)
    }

    /// The waveform at the current phase plus `offset`, without advancing.
    #[inline]
    pub fn at(&self, wave: Wave, offset: f32) -> f32 {
        if wave == Wave::Random {
            self.slewed
        } else {
            shape(wave, self.phase + offset)
        }
    }
}

/// A waveform at `phase` (any real; wrapped), -1..1.
#[inline]
pub fn shape(wave: Wave, phase: f32) -> f32 {
    let p = phase.rem_euclid(1.0);
    match wave {
        Wave::Sine => (TAU * p).sin(),
        Wave::Triangle => 1.0 - 4.0 * (p - 0.5).abs(),
        Wave::SawUp => 2.0 * p - 1.0,
        Wave::SawDown => 1.0 - 2.0 * p,
        Wave::Square => {
            if p < 0.5 {
                1.0
            } else {
                -1.0
            }
        }
        Wave::Random => 0.0,
    }
}

/// Peak envelope follower with attack, hold and release (ms).
#[derive(Clone, Copy, Default)]
pub struct Envelope {
    att: f32,
    rel: f32,
    hold: u32,
    held: u32,
    pub env: f32,
}

impl Envelope {
    pub fn set(&mut self, attack_ms: f32, hold_ms: f32, release_ms: f32, sr: f32) {
        self.att = coef(attack_ms, sr);
        self.rel = coef(release_ms, sr);
        self.hold = (hold_ms.max(0.0) * 0.001 * sr) as u32;
    }

    pub fn reset(&mut self) {
        self.env = 0.0;
        self.held = 0;
    }

    /// Feeds a (rectified) level, returns the envelope.
    #[inline]
    pub fn tick(&mut self, level: f32) -> f32 {
        if level > self.env {
            self.env += self.att * (level - self.env);
            self.held = self.hold;
        } else if self.held > 0 {
            self.held -= 1;
        } else {
            self.env += self.rel * (level - self.env);
        }
        self.env
    }
}

/// One-pole smoothing coefficient for a time constant in ms (1 = instant).
#[inline]
pub fn coef(ms: f32, sr: f32) -> f32 {
    if ms <= 0.0 {
        1.0
    } else {
        1.0 - (-1.0 / (ms * 0.001 * sr)).exp()
    }
}

/// A smoothed parameter (avoids zipper noise when a control moves).
#[derive(Clone, Copy, Default)]
pub struct Smoothed {
    pub value: f32,
    pub target: f32,
    a: f32,
}

impl Smoothed {
    pub fn new(value: f32, ms: f32, sr: f32) -> Self {
        Self { value, target: value, a: coef(ms, sr) }
    }

    /// Jumps straight to `v`.
    pub fn snap(&mut self, v: f32) {
        self.value = v;
        self.target = v;
    }

    #[inline]
    pub fn tick(&mut self) -> f32 {
        self.value += self.a * (self.target - self.value);
        self.value
    }
}

/// Xorshift noise, -1..1.
#[derive(Clone, Copy)]
pub struct Noise(pub u32);

impl Default for Noise {
    fn default() -> Self {
        Self(0x2545_F491)
    }
}

impl Noise {
    #[inline]
    pub fn sample(&mut self) -> f32 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 17;
        self.0 ^= self.0 << 5;
        (self.0 as f32 / u32::MAX as f32) * 2.0 - 1.0
    }
}

/// Fast tanh (Padé), within ~5e-3, saturating at +-1.
#[inline]
pub fn fast_tanh(x: f32) -> f32 {
    let x = x.clamp(-4.97, 4.97);
    let x2 = x * x;
    x * (27.0 + x2) / (27.0 + 9.0 * x2)
}

/// Cubic soft clip: linear around 0, flat at +-1 from |x| = 1.5.
#[inline]
pub fn soft_clip(x: f32) -> f32 {
    let x = x.clamp(-1.5, 1.5);
    x - x * x * x * (4.0 / 27.0)
}

/// Equal-power crossfade gains for `mix` 0..1: (dry, wet).
#[inline]
pub fn equal_power(mix: f32) -> (f32, f32) {
    let m = mix.clamp(0.0, 1.0) * PI * 0.5;
    (m.cos(), m.sin())
}

/// Delay-line pitch shifter: two read heads sweep a window and crossfade, so
/// the pitch moves by `ratio` with a little latency (half the window).
#[derive(Clone)]
pub struct PitchShifter {
    line: DelayLine,
    window: f32,
    phase: f32,
    step: f32,
}

impl PitchShifter {
    /// `window_ms` around 30-80 ms suits voices and instruments.
    pub fn new(window_ms: f32, sr: f32) -> Self {
        let window = (window_ms * 0.001 * sr).max(64.0);
        Self { line: DelayLine::new(window as usize + 8), window, phase: 0.0, step: 0.0 }
    }

    /// Pitch ratio (2 = an octave up, 0.5 = down).
    pub fn set_ratio(&mut self, ratio: f32) {
        self.step = (1.0 - ratio.clamp(0.25, 4.0)) / self.window;
    }

    /// Semitones (and cents) to a ratio.
    pub fn ratio_of(semitones: f32) -> f32 {
        2f32.powf(semitones / 12.0)
    }

    pub fn clear(&mut self) {
        self.line.clear();
        self.phase = 0.0;
    }

    #[inline]
    pub fn tick(&mut self, x: f32) -> f32 {
        self.line.push(x);
        self.phase = (self.phase + self.step).rem_euclid(1.0);
        let p2 = (self.phase + 0.5).rem_euclid(1.0);
        let a = self.line.tap_linear(1.0 + self.phase * self.window);
        let b = self.line.tap_linear(1.0 + p2 * self.window);
        let wa = (PI * self.phase).sin();
        let wb = (PI * p2).sin();
        a * wa * wa + b * wb * wb
    }
}

/// Samples for `ms` at `sr` (at least 1).
#[inline]
pub fn ms_to_samples(ms: f32, sr: f32) -> f32 {
    (ms * 0.001 * sr).max(1.0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn delay_taps() {
        let mut d = DelayLine::new(16);
        for i in 0..10 {
            d.push(i as f32);
        }
        assert_eq!(d.tap(0), 9.0);
        assert_eq!(d.tap(3), 6.0);
        assert!((d.tap_linear(2.5) - 6.5).abs() < 1e-6);
        assert!((d.tap_hermite(3.0) - 6.0).abs() < 1e-5);
        assert!((d.tap_hermite(4.5) - 4.5).abs() < 1e-4);
    }

    #[test]
    fn allpass_is_lossless() {
        let mut ap = Allpass::new(100, 37.0, 0.6);
        let mut energy = 0.0;
        for i in 0..20_000 {
            let y = ap.tick(if i == 0 { 1.0 } else { 0.0 });
            energy += y * y;
        }
        assert!((energy - 1.0).abs() < 1e-3, "{energy}");
    }

    #[test]
    fn svf_and_lfo() {
        let mut f = Svf::default();
        f.set(1000.0, 0.707, 48_000.0);
        let mut last = SvfOut::default();
        for _ in 0..2000 {
            last = f.tick(1.0);
        }
        assert!((last.lp - 1.0).abs() < 1e-3 && last.hp.abs() < 1e-3);
        let mut lfo = Lfo::default();
        lfo.set_rate(1.0, 100.0);
        let v: Vec<f32> = (0..100).map(|_| lfo.next(Wave::Triangle, 0.0)).collect();
        assert!(v.iter().all(|x| (-1.0..=1.0).contains(x)));
        assert!((fast_tanh(0.5) - 0.5f32.tanh()).abs() < 5e-3);
    }

    #[test]
    fn pitch_shifter_moves_pitch() {
        // An octave up doubles the zero-crossing rate of a sine.
        let sr = 48_000.0;
        let mut ps = PitchShifter::new(50.0, sr);
        ps.set_ratio(2.0);
        let mut crossings = 0;
        let mut last = 0.0;
        for i in 0..48_000 {
            let y = ps.tick((TAU * 200.0 * i as f32 / sr).sin());
            if i > 4800 && last < 0.0 && y >= 0.0 {
                crossings += 1;
            }
            last = y;
        }
        let hz = crossings as f32 / 0.9;
        assert!((hz - 400.0).abs() < 25.0, "{hz}");
    }
}
