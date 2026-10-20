//! The channel FILTER block: low cut (6/12/18/24 dB/oct), high cut (6/12 dB/oct)
//! and the tool filter (TILT, MAX, AP1, AP2 all-pass for phase alignment).

use super::{Params, FILTER_TOOLS, MAX_TOOL_PARAMS};
use crate::biquad::{Biquad, BiquadCoeffs};
use std::f64::consts::FRAC_1_SQRT_2;

pub const LOW_CUT_SLOPES: [&str; 4] = ["6", "12", "18", "24"];
pub const HIGH_CUT_SLOPES: [&str; 2] = ["6", "12"];
const SUB_BLOCK: usize = 32;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FilterSettings {
    pub low_cut: bool,
    pub low_cut_freq: f32,
    /// Index into [`LOW_CUT_SLOPES`].
    pub low_cut_slope: usize,
    pub high_cut: bool,
    pub high_cut_freq: f32,
    /// Index into [`HIGH_CUT_SLOPES`].
    pub high_cut_slope: usize,
    pub tool: bool,
    /// Index into [`FILTER_TOOLS`].
    pub tool_model: usize,
    pub tool_params: [f32; MAX_TOOL_PARAMS],
}

/// The cut frequencies of a factory-set console: 100 Hz and 10 kHz on the
/// parameters' steps, which it shows as 100.2 Hz and 10k02 Hz.
pub const LOW_CUT_DEFAULT_HZ: f32 = 100.2374;
pub const HIGH_CUT_DEFAULT_HZ: f32 = 10018.26;

impl Default for FilterSettings {
    /// As a factory-set console: both cuts off, 24 dB/oct low cut, 12 dB/oct high cut.
    fn default() -> Self {
        Self {
            low_cut: false,
            low_cut_freq: LOW_CUT_DEFAULT_HZ,
            low_cut_slope: 3,
            high_cut: false,
            high_cut_freq: HIGH_CUT_DEFAULT_HZ,
            high_cut_slope: 1,
            tool: false,
            tool_model: 0,
            tool_params: [0.0; MAX_TOOL_PARAMS],
        }
    }
}

impl FilterSettings {
    pub fn is_active(&self) -> bool {
        self.low_cut || self.high_cut || self.tool
    }

    /// The filters for these settings (also used to draw the response).
    pub fn design(&self, sr: f64) -> ([BiquadCoeffs; 6], usize) {
        let mut out = [BiquadCoeffs::identity(); 6];
        let mut n = 0;
        let mut push = |c: BiquadCoeffs| {
            if n < out.len() {
                out[n] = c;
                n += 1;
            }
        };
        if self.low_cut {
            let f = self.low_cut_freq.clamp(20.0, 2000.0) as f64;
            match self.low_cut_slope {
                0 => push(BiquadCoeffs::high_pass_1(sr, f)),
                1 => push(BiquadCoeffs::high_pass(sr, f, FRAC_1_SQRT_2)),
                2 => {
                    push(BiquadCoeffs::high_pass_1(sr, f));
                    push(BiquadCoeffs::high_pass(sr, f, 1.0));
                }
                _ => {
                    push(BiquadCoeffs::high_pass(sr, f, 0.5412));
                    push(BiquadCoeffs::high_pass(sr, f, 1.3066));
                }
            }
        }
        if self.high_cut {
            let f = self.high_cut_freq.clamp(50.0, 20000.0) as f64;
            if self.high_cut_slope == 0 {
                push(BiquadCoeffs::low_pass_1(sr, f));
            } else {
                push(BiquadCoeffs::low_pass(sr, f, FRAC_1_SQRT_2));
            }
        }
        if self.tool {
            if let Some(spec) = FILTER_TOOLS.get(self.tool_model) {
                let p = Params::new(spec, &self.tool_params);
                match spec.token {
                    "TILT" => {
                        let t = p.f("tilt") as f64;
                        push(BiquadCoeffs::low_shelf(sr, 1000.0, 0.5, -t * 0.5));
                        push(BiquadCoeffs::high_shelf(sr, 1000.0, 0.5, t * 0.5));
                    }
                    "MAX" => {
                        // Low contour: deep bass lift with a slight low-mid scoop.
                        // High processing: presence/air lift.
                        let low = p.f("low") as f64 / 100.0;
                        let high = p.f("proc") as f64 / 100.0;
                        push(BiquadCoeffs::low_shelf(sr, 110.0, 0.7, 6.0 * low));
                        push(BiquadCoeffs::peak(sr, 320.0, 0.9, -2.0 * low));
                        push(BiquadCoeffs::high_shelf(sr, 5000.0, 0.6, 6.0 * high));
                    }
                    "AP1" => push(BiquadCoeffs::all_pass_1(sr, p.f("f") as f64)),
                    "AP2" => push(BiquadCoeffs::all_pass_2(sr, p.f("f") as f64, p.f("q") as f64)),
                    _ => {}
                }
            }
        }
        (out, n)
    }

    pub fn response_db(&self, freq: f64, sr: f64) -> f64 {
        let (coeffs, n) = self.design(sr);
        let mag: f64 = coeffs[..n].iter().map(|c| c.magnitude_at(freq, sr)).product();
        20.0 * mag.max(1e-9).log10()
    }
}

pub struct WingFilter {
    sr: f64,
    sections: [Biquad; 6],
    len: usize,
    current: Option<FilterSettings>,
}

impl Default for WingFilter {
    fn default() -> Self {
        Self { sr: 48000.0, sections: Default::default(), len: 0, current: None }
    }
}

impl WingFilter {
    pub fn prepare(&mut self, sample_rate: f64) {
        self.sr = sample_rate;
        self.reset();
    }

    pub fn reset(&mut self) {
        for s in &mut self.sections {
            s.reset();
        }
        self.current = None;
    }

    pub fn process(&mut self, chans: &mut [&mut [f32]], target: &FilterSettings) {
        let n = chans.iter().map(|c| c.len()).min().unwrap_or(0);
        let mut start = 0;
        while start < n {
            let len = SUB_BLOCK.min(n - start);
            self.step(target);
            for (ch, data) in chans.iter_mut().enumerate() {
                for s in &mut self.sections[..self.len] {
                    s.process(&mut data[start..start + len], ch);
                }
            }
            start += len;
        }
    }

    /// Glides the corner frequencies; switches and slopes change immediately.
    fn step(&mut self, target: &FilterSettings) {
        let next = match self.current {
            None => *target,
            Some(cur) if cur == *target => return,
            Some(cur) => {
                let glide = |a: f32, b: f32| {
                    let n = a * (b / a.max(1.0)).powf(0.3);
                    if (n / b - 1.0).abs() < 1e-4 {
                        b
                    } else {
                        n
                    }
                };
                let mut n = *target;
                if cur.low_cut && target.low_cut {
                    n.low_cut_freq = glide(cur.low_cut_freq, target.low_cut_freq);
                }
                if cur.high_cut && target.high_cut {
                    n.high_cut_freq = glide(cur.high_cut_freq, target.high_cut_freq);
                }
                n
            }
        };
        let structure_changed = self.current.is_none_or(|c| {
            c.low_cut != next.low_cut
                || c.low_cut_slope != next.low_cut_slope
                || c.high_cut != next.high_cut
                || c.high_cut_slope != next.high_cut_slope
                || c.tool != next.tool
                || c.tool_model != next.tool_model
        });
        let (coeffs, len) = next.design(self.sr);
        if structure_changed {
            for s in &mut self.sections {
                s.reset();
            }
        }
        for (s, c) in self.sections.iter_mut().zip(&coeffs[..len]) {
            s.set_coefficients(*c);
        }
        self.len = len;
        self.current = Some(next);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wing::model_index;

    #[test]
    fn slopes() {
        let sr = 48000.0;
        for (slope, per_octave) in [(0usize, 6.0), (1, 12.0), (2, 18.0), (3, 24.0)] {
            let s = FilterSettings { low_cut: true, low_cut_freq: 1000.0, low_cut_slope: slope, ..Default::default() };
            let drop = s.response_db(125.0, sr) - s.response_db(250.0, sr);
            assert!((drop + per_octave).abs() < 1.5, "slope {slope}: {drop}");
        }
        let s = FilterSettings { high_cut: true, high_cut_freq: 1000.0, high_cut_slope: 1, ..Default::default() };
        assert!((s.response_db(8000.0, sr) - s.response_db(4000.0, sr) + 12.0).abs() < 1.5);
    }

    #[test]
    fn tools() {
        let sr = 48000.0;
        let tilt = model_index(FILTER_TOOLS, "TILT").unwrap();
        let s = FilterSettings { tool: true, tool_model: tilt, tool_params: [6.0, 0.0], ..Default::default() };
        assert!(s.response_db(10000.0, sr) > 2.0 && s.response_db(50.0, sr) < -2.0);
        let ap = model_index(FILTER_TOOLS, "AP2").unwrap();
        let s = FilterSettings { tool: true, tool_model: ap, tool_params: [500.0, 1.0], ..Default::default() };
        for f in [50.0, 500.0, 5000.0] {
            assert!(s.response_db(f, sr).abs() < 1e-6);
        }
    }

    #[test]
    fn starts_as_a_factory_set_console() {
        // Everything off; LC 100.2 Hz at 24 dB/oct, HC 10k02 Hz at 12 dB/oct.
        let s = FilterSettings::default();
        assert!(!s.is_active());
        assert_eq!((LOW_CUT_SLOPES[s.low_cut_slope], HIGH_CUT_SLOPES[s.high_cut_slope]), ("24", "12"));
        assert!((s.low_cut_freq - 100.24).abs() < 0.01 && (s.high_cut_freq - 10018.26).abs() < 0.01);
    }

    #[test]
    fn processes_without_blowing_up() {
        let mut flt = WingFilter::default();
        flt.prepare(48000.0);
        let mut a = vec![0.5f32; 1024];
        let mut s = FilterSettings { low_cut: true, low_cut_freq: 100.0, ..Default::default() };
        flt.process(&mut [&mut a[..]], &s);
        assert!(a[1023].abs() < 0.01, "DC must be removed");
        s.low_cut_freq = 1500.0;
        s.low_cut_slope = 3;
        flt.process(&mut [&mut a[..]], &s);
        assert!(a.iter().all(|x| x.is_finite()));
    }
}
