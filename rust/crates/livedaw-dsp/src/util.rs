//! Level conversions, pan laws and block helpers.

/// Anything at or below this level is treated as silence (-inf dB).
pub const MINUS_INF_DB: f32 = -100.0;

#[inline]
pub fn db_to_gain(db: f32) -> f32 {
    if db <= MINUS_INF_DB {
        0.0
    } else {
        10.0f32.powf(db * 0.05)
    }
}

#[inline]
pub fn gain_to_db(gain: f32) -> f32 {
    if gain > 0.0 {
        (20.0 * gain.log10()).max(MINUS_INF_DB)
    } else {
        MINUS_INF_DB
    }
}

/// One-pole smoothing coefficient for a time constant in milliseconds.
#[inline]
pub fn time_constant_coeff(ms: f32, sample_rate: f64) -> f32 {
    if ms <= 0.0 || sample_rate <= 0.0 {
        return 0.0;
    }
    (-1.0 / (0.001 * ms as f64 * sample_rate)).exp() as f32
}

/// Multiplies a block by a gain moving linearly from `start` to `end`.
#[inline]
pub fn apply_gain_ramp(data: &mut [f32], start: f32, end: f32) {
    if start == end {
        if start != 1.0 {
            data.iter_mut().for_each(|x| *x *= start);
        }
        return;
    }
    let step = (end - start) / data.len().max(1) as f32;
    let mut g = start;
    for x in data.iter_mut() {
        g += step;
        *x *= g;
    }
}

/// `dst += src * gain`, with the gain ramping from `start` to `end`.
#[inline]
pub fn add_with_gain_ramp(dst: &mut [f32], src: &[f32], start: f32, end: f32) {
    if start == 0.0 && end == 0.0 {
        return;
    }
    let n = dst.len().min(src.len());
    if start == end {
        for i in 0..n {
            dst[i] += src[i] * start;
        }
        return;
    }
    let step = (end - start) / n.max(1) as f32;
    let mut g = start;
    for i in 0..n {
        g += step;
        dst[i] += src[i] * g;
    }
}

#[inline]
pub fn peak_level(data: &[f32]) -> f32 {
    data.iter().fold(0.0f32, |p, x| p.max(x.abs()))
}

/// True if the block contains no NaN or infinity.
#[inline]
pub fn all_finite(data: &[f32]) -> bool {
    // inf * 0 and NaN * 0 are NaN, and NaN poisons the sum: one branch per block.
    let acc = data.iter().fold(0.0f32, |acc, x| acc + x * 0.0);
    acc == 0.0
}

/// The WING's pan law for a mono source; `pan` is -1 (L) .. +1 (R). Unity on
/// both sides at centre; panning rises the near side linearly to +3 dB and
/// lowers the far side linearly to silence (measured on a WING: centre 0 / 0 dB,
/// L50 +1.63 / -6.02 dB, L100 +3.01 dB / off).
#[inline]
pub fn wing_mono_pan(pan: f32) -> (f32, f32) {
    let pan = pan.clamp(-1.0, 1.0);
    let near = 1.0 + pan.abs() * (std::f32::consts::SQRT_2 - 1.0);
    let far = 1.0 - pan.abs();
    if pan < 0.0 {
        (near, far)
    } else {
        (far, near)
    }
}

/// Balance law for a stereo source: the opposite side is attenuated, centre is unity.
#[inline]
pub fn stereo_balance(pan: f32) -> (f32, f32) {
    let pan = pan.clamp(-1.0, 1.0);
    ((1.0 - pan).min(1.0), (1.0 + pan).min(1.0))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn db_conversions() {
        assert!((db_to_gain(0.0) - 1.0).abs() < 1e-6);
        assert!((db_to_gain(-6.0206) - 0.5).abs() < 1e-4);
        assert_eq!(db_to_gain(MINUS_INF_DB), 0.0);
        assert!((gain_to_db(0.5) + 6.0206).abs() < 1e-3);
    }

    #[test]
    fn pan_laws() {
        // The levels a WING gives at centre, L50 and L100.
        assert_eq!(wing_mono_pan(0.0), (1.0, 1.0));
        let (l, r) = wing_mono_pan(-0.5);
        assert!((gain_to_db(l) - 1.63).abs() < 0.01 && (gain_to_db(r) + 6.02).abs() < 0.01);
        let (l, r) = wing_mono_pan(-1.0);
        assert!((gain_to_db(l) - 3.01).abs() < 0.01 && r == 0.0);
        assert_eq!(wing_mono_pan(0.5), (wing_mono_pan(-0.5).1, wing_mono_pan(-0.5).0));
        assert_eq!(stereo_balance(0.0), (1.0, 1.0));
        let (l, r) = stereo_balance(0.5);
        assert!((l - 0.5).abs() < 1e-6 && r == 1.0);
    }

    #[test]
    fn finite_check() {
        assert!(all_finite(&[0.0, 1.0, -1.0]));
        assert!(!all_finite(&[0.0, f32::NAN]));
        assert!(!all_finite(&[f32::INFINITY]));
    }

    #[test]
    fn ramps() {
        let mut d = [1.0f32; 4];
        apply_gain_ramp(&mut d, 0.0, 1.0);
        assert!((d[3] - 1.0).abs() < 1e-6 && d[0] > 0.0 && d[0] < 1.0);
        let mut dst = [0.0f32; 4];
        add_with_gain_ramp(&mut dst, &[2.0; 4], 0.5, 0.5);
        assert_eq!(dst, [1.0; 4]);
    }
}
