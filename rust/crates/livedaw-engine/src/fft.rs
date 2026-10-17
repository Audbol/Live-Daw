//! In-place radix-2 complex FFT (iterative, decimation in time) for the RTA.
//! Tables are built once per size; `forward` doesn't allocate.

pub struct Fft {
    n: usize,
    /// e^(-2 pi i m / n) for m in 0..n/2.
    twiddles: Vec<(f32, f32)>,
    /// Bit-reversed index of every position.
    reversed: Vec<u32>,
}

impl Fft {
    /// `n` must be a power of two (at least 2).
    pub fn new(n: usize) -> Self {
        assert!(n >= 2 && n.is_power_of_two(), "FFT size must be a power of two");
        let bits = n.trailing_zeros();
        let twiddles = (0..n / 2)
            .map(|m| {
                let a = -2.0 * std::f64::consts::PI * m as f64 / n as f64;
                (a.cos() as f32, a.sin() as f32)
            })
            .collect();
        let reversed = (0..n as u32).map(|i| i.reverse_bits() >> (32 - bits)).collect();
        Self { n, twiddles, reversed }
    }

    pub fn len(&self) -> usize {
        self.n
    }

    pub fn is_empty(&self) -> bool {
        false
    }

    /// X[k] = sum x[j] e^(-2 pi i j k / n), unscaled, in place.
    pub fn forward(&self, re: &mut [f32], im: &mut [f32]) {
        let n = self.n;
        assert!(re.len() >= n && im.len() >= n);
        for i in 0..n {
            let j = self.reversed[i] as usize;
            if j > i {
                re.swap(i, j);
                im.swap(i, j);
            }
        }
        let mut len = 2;
        while len <= n {
            let half = len / 2;
            let step = n / len;
            for start in (0..n).step_by(len) {
                for k in 0..half {
                    let (wr, wi) = self.twiddles[k * step];
                    let (a, b) = (start + k, start + k + half);
                    let tr = re[b] * wr - im[b] * wi;
                    let ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr;
                    im[b] = im[a] - ti;
                    re[a] += tr;
                    im[a] += ti;
                }
            }
            len <<= 1;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn matches_a_direct_dft() {
        let n = 64;
        let fft = Fft::new(n);
        // A fixed pseudo-random signal.
        let mut seed = 12345u32;
        let mut next = || {
            seed = seed.wrapping_mul(1_103_515_245).wrapping_add(12345);
            (seed >> 8) as f32 / (1u32 << 24) as f32 - 0.5
        };
        let x: Vec<(f32, f32)> = (0..n).map(|_| (next(), next())).collect();
        let (mut re, mut im): (Vec<f32>, Vec<f32>) = x.iter().copied().unzip();
        fft.forward(&mut re, &mut im);
        for k in 0..n {
            let (mut sr, mut si) = (0.0f64, 0.0f64);
            for (j, &(xr, xi)) in x.iter().enumerate() {
                let a = -2.0 * std::f64::consts::PI * (j * k) as f64 / n as f64;
                sr += xr as f64 * a.cos() - xi as f64 * a.sin();
                si += xr as f64 * a.sin() + xi as f64 * a.cos();
            }
            assert!((re[k] as f64 - sr).abs() < 1e-4 && (im[k] as f64 - si).abs() < 1e-4, "bin {k}");
        }
    }

    #[test]
    fn a_sine_lands_in_its_bin() {
        let n = 1024;
        let fft = Fft::new(n);
        let mut re: Vec<f32> = (0..n).map(|i| (2.0 * std::f32::consts::PI * 37.0 * i as f32 / n as f32).sin()).collect();
        let mut im = vec![0.0; n];
        fft.forward(&mut re, &mut im);
        let mag = |k: usize| (re[k] * re[k] + im[k] * im[k]).sqrt();
        assert!((mag(37) - n as f32 / 2.0).abs() < 0.01 * n as f32);
        assert!(mag(36) < 1e-2 && mag(100) < 1e-2);
    }
}
