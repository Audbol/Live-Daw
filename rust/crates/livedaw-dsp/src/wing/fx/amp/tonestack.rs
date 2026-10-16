//! The passive treble/middle/bass tone stack of Fender and Marshall style
//! amps, from its circuit: the third-order transfer function of Yeh & Smith
//! ("Discretization of the '59 Fender Bassman tone stack", DAFx 2006) in
//! terms of the parts and the pot positions, discretised with the bilinear
//! transform. The knobs therefore interact and sweep exactly like the real
//! controls (the mid scoop, bass that mostly works in the lower half...).

/// Parts of a tone stack (ohms, farads). R1 treble pot, R2 bass pot, R3 mid
/// pot, R4 slope resistor, C1 treble cap, C2 bass cap, C3 mid cap.
#[derive(Debug, Clone, Copy)]
pub struct StackParts {
    pub r1: f64,
    pub r2: f64,
    pub r3: f64,
    pub r4: f64,
    pub c1: f64,
    pub c2: f64,
    pub c3: f64,
}

/// Marshall JCM800 2203.
pub const MARSHALL: StackParts = StackParts { r1: 220e3, r2: 1e6, r3: 25e3, r4: 33e3, c1: 470e-12, c2: 22e-9, c3: 22e-9 };
/// A high-gain (ENGL style) stack: tighter bass, more upper mids.
pub const HIGH_GAIN: StackParts = StackParts { r1: 250e3, r2: 1e6, r3: 25e3, r4: 39e3, c1: 680e-12, c2: 22e-9, c3: 47e-9 };
/// Fender AB763 (Deluxe Reverb): the mid pot is a fixed 6.8k, used with m = 1.
pub const FENDER_AB763: StackParts =
    StackParts { r1: 250e3, r2: 250e3, r3: 6.8e3, r4: 100e3, c1: 250e-12, c2: 100e-9, c3: 47e-9 };

/// The tone stack as a third-order IIR (transposed direct form II, f64).
#[derive(Debug, Clone, Copy, Default)]
pub struct ToneStack {
    b: [f64; 4],
    a: [f64; 4],
    z: [f64; 3],
}

impl ToneStack {
    /// Sets the response for treble `t`, middle `m` and bass `l` (pot
    /// positions 0..1, after any taper) at sample rate `sr`.
    pub fn design(&mut self, p: &StackParts, t: f64, m: f64, l: f64, sr: f64) {
        let (t, m, l) = (t.clamp(0.0, 1.0), m.clamp(0.0, 1.0), l.clamp(0.0, 1.0));
        let StackParts { r1, r2, r3, r4, c1, c2, c3 } = *p;
        let b1 = t * c1 * r1 + m * c3 * r3 + l * (c1 * r2 + c2 * r2) + (c1 * r3 + c2 * r3);
        let b2 = t * (c1 * c2 * r1 * r4 + c1 * c3 * r1 * r4) - m * m * (c1 * c3 * r3 * r3 + c2 * c3 * r3 * r3)
            + m * (c1 * c3 * r1 * r3 + c1 * c3 * r3 * r3 + c2 * c3 * r3 * r3)
            + l * (c1 * c2 * r1 * r2 + c1 * c2 * r2 * r4 + c1 * c3 * r2 * r4)
            + l * m * (c1 * c3 * r2 * r3 + c2 * c3 * r2 * r3)
            + (c1 * c2 * r1 * r3 + c1 * c2 * r3 * r4 + c1 * c3 * r3 * r4);
        let c123 = c1 * c2 * c3;
        let b3 = l * m * c123 * (r1 * r2 * r3 + r2 * r3 * r4) - m * m * c123 * (r1 * r3 * r3 + r3 * r3 * r4)
            + m * c123 * (r1 * r3 * r3 + r3 * r3 * r4)
            + t * c123 * r1 * r3 * r4
            - t * m * c123 * r1 * r3 * r4
            + t * l * c123 * r1 * r2 * r4;
        let a0 = 1.0;
        let a1 = (c1 * r1 + c1 * r3 + c2 * r3 + c2 * r4 + c3 * r4) + m * c3 * r3 + l * (c1 * r2 + c2 * r2);
        let a2 = m * (c1 * c3 * r1 * r3 - c2 * c3 * r3 * r4 + c1 * c3 * r3 * r3 + c2 * c3 * r3 * r3)
            + l * m * (c1 * c3 * r2 * r3 + c2 * c3 * r2 * r3)
            - m * m * (c1 * c3 * r3 * r3 + c2 * c3 * r3 * r3)
            + l * (c1 * c2 * r2 * r4 + c1 * c2 * r1 * r2 + c1 * c3 * r2 * r4 + c2 * c3 * r2 * r4)
            + (c1 * c2 * r1 * r4
                + c1 * c3 * r1 * r4
                + c1 * c2 * r3 * r4
                + c1 * c2 * r1 * r3
                + c1 * c3 * r3 * r4
                + c2 * c3 * r3 * r4);
        let a3 = l * m * c123 * (r1 * r2 * r3 + r2 * r3 * r4) - m * m * c123 * (r1 * r3 * r3 + r3 * r3 * r4)
            + m * c123 * (r3 * r3 * r4 + r1 * r3 * r3 - r1 * r3 * r4)
            + l * c123 * r1 * r2 * r4
            + c123 * r1 * r3 * r4;
        self.set_analog([0.0, b1, b2, b3], [a0, a1, a2, a3], sr);
    }

    /// Bilinear transform of H(s) = sum(b_k s^k) / sum(a_k s^k), k = 0..3.
    fn set_analog(&mut self, b: [f64; 4], a: [f64; 4], sr: f64) {
        // (1 - z^-1)^k (1 + z^-1)^(3-k) for k = 0..3.
        const BASIS: [[f64; 4]; 4] =
            [[1.0, 3.0, 3.0, 1.0], [1.0, 1.0, -1.0, -1.0], [1.0, -1.0, -1.0, 1.0], [1.0, -3.0, 3.0, -1.0]];
        let c = 2.0 * sr;
        let mut bd = [0.0; 4];
        let mut ad = [0.0; 4];
        let mut ck = 1.0;
        for k in 0..4 {
            for j in 0..4 {
                bd[j] += b[k] * ck * BASIS[k][j];
                ad[j] += a[k] * ck * BASIS[k][j];
            }
            ck *= c;
        }
        let norm = 1.0 / ad[0];
        for j in 0..4 {
            self.b[j] = bd[j] * norm;
            self.a[j] = ad[j] * norm;
        }
    }

    pub fn reset(&mut self) {
        self.z = [0.0; 3];
    }

    #[cfg(test)]
    pub fn tick(&mut self, x: f32) -> f32 {
        let mut b = [x];
        self.process(&mut b);
        b[0]
    }

    /// Filters a block in place.
    pub fn process(&mut self, data: &mut [f32]) {
        let (b, a) = (self.b, self.a);
        let [mut z0, mut z1, mut z2] = self.z;
        for v in data.iter_mut() {
            let x = *v as f64;
            let y = b[0] * x + z0;
            z0 = b[1] * x - a[1] * y + z1;
            z1 = b[2] * x - a[2] * y + z2;
            z2 = b[3] * x - a[3] * y;
            *v = y as f32;
        }
        self.z = [z0, z1, z2];
    }

    /// Magnitude at `hz` (tests and plots).
    #[cfg(test)]
    pub fn magnitude_at(&self, hz: f64, sr: f64) -> f64 {
        let w = std::f64::consts::TAU * hz / sr;
        let (mut nr, mut ni, mut dr, mut di) = (0.0, 0.0, 0.0, 0.0);
        for k in 0..4 {
            let (s, c) = (-(w * k as f64)).sin_cos();
            nr += self.b[k] * c;
            ni += self.b[k] * s;
            dr += self.a[k] * c;
            di += self.a[k] * s;
        }
        ((nr * nr + ni * ni) / (dr * dr + di * di)).sqrt()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn db(ts: &ToneStack, hz: f64) -> f64 {
        20.0 * ts.magnitude_at(hz, 48_000.0).log10()
    }

    #[test]
    fn knobs_work_like_the_real_ones() {
        let sr = 48_000.0;
        let mut ts = ToneStack::default();
        let resp = |ts: &mut ToneStack, t, m, l| {
            ts.design(&MARSHALL, t, m, l, sr);
            [db(ts, 80.0), db(ts, 700.0), db(ts, 5000.0)]
        };
        let mid = resp(&mut ts, 0.5, 0.5, 0.1);
        // The classic mid scoop at noon.
        assert!(mid[1] < mid[0] - 2.0 && mid[1] < mid[2] - 2.0, "{mid:?}");
        // Each knob raises its own region most.
        let bass = resp(&mut ts, 0.5, 0.5, 1.0);
        let treble = resp(&mut ts, 1.0, 0.5, 0.1);
        let middle = resp(&mut ts, 0.5, 1.0, 0.1);
        assert!(bass[0] - mid[0] > 2.0 && (bass[2] - mid[2]).abs() < 1.0);
        assert!(treble[2] - mid[2] > 3.0 && (treble[0] - mid[0]).abs() < 1.0);
        assert!(middle[1] - mid[1] > 2.0);
        // Bass and treble at zero cut hard; passive: never above 0 dB.
        let dark = resp(&mut ts, 0.0, 0.5, 0.0);
        assert!(dark[0] < mid[0] - 5.0 && dark[2] < mid[2] - 5.0);
        for (t, m, l) in [(1.0, 1.0, 1.0), (0.0, 0.0, 0.0), (1.0, 0.0, 1.0)] {
            ts.design(&FENDER_AB763, t, m, l, sr);
            for hz in [30.0, 100.0, 1000.0, 5000.0, 15000.0] {
                assert!(db(&ts, hz) < 0.5, "{t} {m} {l} {hz}");
            }
        }
    }

    #[test]
    fn filter_matches_its_response() {
        let sr = 48_000.0;
        let mut ts = ToneStack::default();
        ts.design(&MARSHALL, 0.7, 0.3, 0.2, sr);
        let hz = 1000.0;
        let mut peak = 0.0f32;
        for i in 0..48_000 {
            let y = ts.tick((std::f64::consts::TAU * hz * i as f64 / sr).sin() as f32);
            if i > 24_000 {
                peak = peak.max(y.abs());
            }
        }
        assert!((peak as f64 - ts.magnitude_at(hz, sr)).abs() < 1e-3);
    }
}
