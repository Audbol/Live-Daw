//! Oversampling for the amps' non-linear stages: polyphase IIR half-band
//! filters (pairs of allpass chains, the structure of Laurent de Soras'
//! HIIR), designed at creation from an elliptic prototype.
//!
//! The first stage (1x <-> 2x) is flat to ~0.42 fs and rejects ~95 dB from
//! ~0.58 fs; the second (2x <-> 4x) only has to reject the images above the
//! first stage's passband, so it is short.

/// A two-path polyphase IIR half-band filter with `N` allpass sections, for
/// 2x up- or downsampling.
#[derive(Clone, Copy)]
pub struct HalfBand<const N: usize> {
    c: [f32; N],
    x: [f32; N],
    y: [f32; N],
}

impl<const N: usize> HalfBand<N> {
    /// `transition` is the transition band width relative to the high sample
    /// rate (centred on a quarter of it).
    pub fn new(transition: f64) -> Self {
        let d = design(N, transition);
        Self { c: std::array::from_fn(|i| d[i] as f32), x: [0.0; N], y: [0.0; N] }
    }

    pub fn reset(&mut self) {
        self.x = [0.0; N];
        self.y = [0.0; N];
    }

    /// Runs the two paths (even coefficients on `s0`, odd on `s1`).
    #[inline(always)]
    fn paths(&mut self, mut s0: f32, mut s1: f32) -> (f32, f32) {
        let mut i = 0;
        while i + 1 < N {
            let t0 = (s0 - self.y[i]) * self.c[i] + self.x[i];
            let t1 = (s1 - self.y[i + 1]) * self.c[i + 1] + self.x[i + 1];
            self.x[i] = s0;
            self.x[i + 1] = s1;
            self.y[i] = t0;
            self.y[i + 1] = t1;
            s0 = t0;
            s1 = t1;
            i += 2;
        }
        if i < N {
            let t0 = (s0 - self.y[i]) * self.c[i] + self.x[i];
            self.x[i] = s0;
            self.y[i] = t0;
            s0 = t0;
        }
        (s0, s1)
    }

    /// One input sample to two output samples at twice the rate.
    #[inline(always)]
    pub fn up(&mut self, x: f32) -> (f32, f32) {
        self.paths(x, x)
    }

    /// Two input samples (in time order) to one at half the rate.
    #[inline(always)]
    pub fn down(&mut self, a: f32, b: f32) -> f32 {
        let (s0, s1) = self.paths(b, a);
        0.5 * (s0 + s1)
    }
}

/// Most sections [`design`] computes.
const MAX_COEFS: usize = 12;

/// Allpass coefficients of an elliptic half-band filter with `n` sections
/// (filter order 2n + 1) and the given transition band (HIIR's designer).
fn design(n: usize, transition: f64) -> [f64; MAX_COEFS] {
    use std::f64::consts::PI;
    let transition = transition.clamp(1e-4, 0.49);
    let mut k = ((1.0 - transition * 2.0) * PI / 4.0).tan();
    k *= k;
    let kksqrt = (1.0 - k * k).powf(0.25);
    let e = 0.5 * (1.0 - kksqrt) / (1.0 + kksqrt);
    let e4 = e * e * e * e;
    let q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));
    let order = (2 * n + 1) as f64;
    let mut out = [0.0; MAX_COEFS];
    for (idx, o) in out.iter_mut().enumerate().take(n) {
        let c = (idx + 1) as f64;
        let mut num = 0.0;
        let mut sign = 1.0;
        for i in 0..64 {
            let i = i as f64;
            let term = q.powf(i * (i + 1.0)) * ((i * 2.0 + 1.0) * c * PI / order).sin() * sign;
            num += term;
            sign = -sign;
            if term.abs() < 1e-30 {
                break;
            }
        }
        let mut den = 0.0;
        let mut sign = -1.0;
        for i in 1..64 {
            let i = i as f64;
            let term = q.powf(i * i) * (i * 2.0 * c * PI / order).cos() * sign;
            den += term;
            sign = -sign;
            if term.abs() < 1e-30 {
                break;
            }
        }
        let ww = num * q.powf(0.25) / (den + 0.5);
        let wwsq = ww * ww;
        let x = ((1.0 - wwsq * k) * (1.0 - wwsq / k)).sqrt() / (1.0 + wwsq);
        *o = (1.0 - x) / (1.0 + x);
    }
    out
}

type First = HalfBand<6>;
type Second = HalfBand<3>;

fn first() -> First {
    HalfBand::new(0.08)
}

/// 4x up- and downsampler (two half-band stages each way).
#[derive(Clone, Copy)]
pub struct Oversampler4 {
    up1: First,
    up2: Second,
    down2: Second,
    down1: First,
}

impl Default for Oversampler4 {
    fn default() -> Self {
        let second = HalfBand::new(0.25);
        Self { up1: first(), up2: second, down2: second, down1: first() }
    }
}

impl Oversampler4 {
    pub fn reset(&mut self) {
        self.up1.reset();
        self.up2.reset();
        self.down2.reset();
        self.down1.reset();
    }

    #[inline]
    pub fn up(&mut self, x: f32) -> [f32; 4] {
        let (a, b) = self.up1.up(x);
        let (a0, a1) = self.up2.up(a);
        let (b0, b1) = self.up2.up(b);
        [a0, a1, b0, b1]
    }

    #[inline]
    pub fn down(&mut self, s: [f32; 4]) -> f32 {
        let a = self.down2.down(s[0], s[1]);
        let b = self.down2.down(s[2], s[3]);
        self.down1.down(a, b)
    }
}

/// 2x up- and downsampler.
#[derive(Clone, Copy)]
pub struct Oversampler2 {
    up: First,
    down: First,
}

impl Default for Oversampler2 {
    fn default() -> Self {
        Self { up: first(), down: first() }
    }
}

impl Oversampler2 {
    pub fn reset(&mut self) {
        self.up.reset();
        self.down.reset();
    }

    #[inline]
    pub fn up(&mut self, x: f32) -> [f32; 2] {
        let (a, b) = self.up.up(x);
        [a, b]
    }

    #[inline]
    pub fn down(&mut self, s: [f32; 2]) -> f32 {
        self.down.down(s[0], s[1])
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::f32::consts::TAU;

    /// Amplitude of a sine at frequency `f` (relative to the base rate,
    /// generated at the 4x rate), after downsampling.
    fn tone_through(f: f32, os: &mut Oversampler4) -> f32 {
        let mut sum = 0.0f64;
        for i in 0..8000 {
            let s: [f32; 4] = std::array::from_fn(|k| (std::f64::consts::TAU * f as f64 * (4 * i + k) as f64 / 4.0).sin() as f32);
            let y = os.down(s);
            if i >= 4000 {
                sum += (y * y) as f64;
            }
        }
        (2.0 * sum / 4000.0).sqrt() as f32
    }

    #[test]
    fn rejects_what_would_alias() {
        let mut os = Oversampler4::default();
        // Passband (relative to the base rate).
        assert!((tone_through(0.1, &mut os) - 1.0).abs() < 0.01);
        assert!((tone_through(0.4, &mut os) - 1.0).abs() < 0.02);
        // Above the base Nyquist: would fold back without the filters.
        for f in [0.6, 0.9, 1.3, 1.7] {
            let p = tone_through(f, &mut os);
            assert!(p < 1e-3, "{f}: {p}");
        }
    }

    #[test]
    fn up_then_down_is_transparent() {
        let mut os = Oversampler4::default();
        let mut out = vec![];
        let input: Vec<f32> = (0..4000).map(|i| (TAU * 0.05 * i as f32).sin()).collect();
        for &x in &input {
            let u = os.up(x);
            out.push(os.down(u));
        }
        // Same amplitude, small delay.
        let amp = (2.0 * out[2000..].iter().map(|x| x * x).sum::<f32>() / 2000.0).sqrt();
        assert!((amp - 1.0).abs() < 0.01, "{amp}");
        // And the upsampled signal has no images: a 0.3 fs tone stays clean.
        let mut os = Oversampler4::default();
        let mut image = 0.0f64;
        let (mut c, mut s) = (0.0f64, 0.0f64);
        for i in 0..8000 {
            let u = os.up((TAU * 0.3 * i as f32).sin());
            for (k, v) in u.iter().enumerate() {
                let n = (4 * i + k) as f64;
                // Image of 0.3 fs at 0.7 fs (relative to the base rate).
                let w = std::f64::consts::TAU * 0.7 * n / 4.0;
                if i > 2000 {
                    c += *v as f64 * w.cos();
                    s += *v as f64 * w.sin();
                }
            }
        }
        image += (c * c + s * s).sqrt() * 2.0 / (6000.0 * 4.0);
        assert!(image < 1e-3, "{image}");
    }
}
