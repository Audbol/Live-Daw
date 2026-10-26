//! Stereo delay line for channel and output time alignment (up to 500 ms).

/// Delay units of the console: meters, feet, milliseconds, samples.
pub const DELAY_MODES: [&str; 4] = ["M", "FT", "MS", "SMP"];

/// Speed of sound used for distance-based delays (m/s, ~20 degrees C).
pub const SPEED_OF_SOUND: f32 = 343.0;

/// The speed the WING turns a strip delay's distance into time with: 0.3 m
/// per ms (its DIST range, 0.1 .. 150 m, is 1/3 .. 500 ms, and 0.1 m shows
/// as 0.3 ft, 0.3 ms and 16 samples at 48 kHz).
pub const DELAY_SPEED_OF_SOUND: f32 = 300.0;

/// Meters per foot.
pub const METERS_PER_FOOT: f32 = 0.3048;

/// The strip delay's range in meters (`/ch/n/in/set/dly`: DIST 0.1 .. 150 m).
pub const MIN_DELAY_M: f32 = 0.1;
pub const MAX_DELAY_M: f32 = 150.0;

/// The shortest strip delay, in samples: what the WING shows (and SAMPLES
/// starts at) for its 0.1 m minimum.
pub const MIN_DELAY_SAMPLES: f32 = 16.0;

/// Converts a strip delay `value` in `DELAY_MODES[mode]` units to milliseconds.
pub fn delay_to_ms(value: f32, mode: usize, sample_rate: f64) -> f32 {
    match mode {
        0 => value / DELAY_SPEED_OF_SOUND * 1000.0,
        1 => value * METERS_PER_FOOT / DELAY_SPEED_OF_SOUND * 1000.0,
        2 => value,
        _ => value / sample_rate as f32 * 1000.0,
    }
}

/// Converts `ms` to a strip delay in `DELAY_MODES[mode]` units (samples:
/// whole, from [`MIN_DELAY_SAMPLES`]).
pub fn ms_to_delay(ms: f32, mode: usize, sample_rate: f64) -> f32 {
    match mode {
        0 => ms * DELAY_SPEED_OF_SOUND / 1000.0,
        1 => ms * DELAY_SPEED_OF_SOUND / 1000.0 / METERS_PER_FOOT,
        2 => ms,
        _ => (ms * 0.001 * sample_rate as f32).round().max(MIN_DELAY_SAMPLES),
    }
}

/// The strip delay's range in `DELAY_MODES[mode]` units: 0.1 .. 150 m in
/// any unit; SAMPLES from 16 at every rate.
pub fn delay_range(mode: usize, sample_rate: f64) -> (f32, f32) {
    let unit = |m: f32| ms_to_delay(delay_to_ms(m, 0, sample_rate), mode, sample_rate);
    (unit(MIN_DELAY_M), unit(MAX_DELAY_M))
}

/// The samples a strip delay of `value` (`DELAY_MODES[mode]` units) delays
/// by: the nearest whole sample, at least [`MIN_DELAY_SAMPLES`].
pub fn delay_samples(value: f32, mode: usize, sample_rate: f64) -> usize {
    let ms = delay_to_ms(value, mode, sample_rate);
    let samples = if ms.is_finite() { (ms.max(0.0) * 0.001 * sample_rate as f32).round() } else { 0.0 };
    samples.max(MIN_DELAY_SAMPLES) as usize
}

pub const MAX_DELAY_MS: f32 = 500.0;

#[derive(Default)]
pub struct DelayLine {
    buf: [Vec<f32>; 2],
    pos: usize,
    delay: usize,
}

impl DelayLine {
    /// Allocates (message thread / device stopped).
    pub fn prepare(&mut self, sample_rate: f64) {
        let len = (MAX_DELAY_MS * 0.001 * sample_rate as f32) as usize + 1;
        for b in &mut self.buf {
            b.clear();
            b.resize(len, 0.0);
        }
        self.pos = 0;
        self.delay = 0;
    }

    pub fn reset(&mut self) {
        for b in &mut self.buf {
            b.fill(0.0);
        }
    }

    /// Delays `chans` in place by `samples` (clamped to the maximum).
    pub fn process(&mut self, chans: &mut [&mut [f32]], samples: usize) {
        let len = self.buf[0].len();
        if len < 2 {
            return;
        }
        let samples = samples.min(len - 1);
        if samples != self.delay {
            self.delay = samples;
        }
        let n = chans.iter().map(|c| c.len()).min().unwrap_or(0);
        let start = self.pos;
        for (ch, data) in chans.iter_mut().enumerate().take(2) {
            let buf = &mut self.buf[ch];
            let mut w = start;
            for x in data[..n].iter_mut() {
                buf[w] = *x;
                let r = (w + len - samples) % len;
                *x = buf[r];
                w = (w + 1) % len;
            }
        }
        self.pos = (start + n) % len;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn delays_by_whole_samples() {
        let mut d = DelayLine::default();
        d.prepare(48000.0);
        let mut a: Vec<f32> = (0..100).map(|i| i as f32).collect();
        d.process(&mut [&mut a[..]], 10);
        assert_eq!(a[10], 0.0);
        assert_eq!(a[11], 1.0);
        assert_eq!(a[..10], [0.0; 10]);
        assert!((delay_to_ms(3.0, 0, 48000.0) - 10.0).abs() < 1e-3);
        assert!((delay_to_ms(480.0, 3, 48000.0) - 10.0).abs() < 1e-3);
    }

    /// As the WING's DELAY panel: 0.1 m reads 0.3 ft, 0.3 ms and 16 samples
    /// at 48 kHz, and delays by 16 samples; 150 m is 500 ms.
    #[test]
    fn units_convert_as_on_the_wing() {
        let sr = 48000.0;
        let ms = delay_to_ms(0.1, 0, sr);
        assert!((ms_to_delay(ms, 1, sr) - 0.328).abs() < 1e-3, "feet");
        assert!((ms - 0.3333).abs() < 1e-3, "ms");
        assert_eq!(ms_to_delay(ms, 3, sr), 16.0, "samples");
        assert_eq!(delay_samples(0.1, 0, sr), 16);
        assert!((delay_to_ms(150.0, 0, sr) - 500.0).abs() < 1e-3);
        assert_eq!(delay_samples(150.0, 0, sr), 24000);
        assert_eq!(delay_samples(10.0, 2, sr), 480);
        assert_eq!(delay_samples(1.0, 1, sr), 49); // 1 ft = 1.016 ms
                                                   // Twice the rate, twice the samples; the 16-sample minimum at every rate.
        assert_eq!(delay_samples(0.1, 0, 96000.0), 32);
        assert_eq!(delay_samples(0.1, 0, 44100.0), 16);
        assert_eq!(ms_to_delay(delay_to_ms(0.1, 0, 44100.0), 3, 44100.0), 16.0);
        assert_eq!(delay_samples(3.0, 3, sr), 16, "SAMPLES starts at 16");
        assert_eq!(delay_samples(f32::NAN, 0, sr), 16);
        // The range in every unit.
        let (lo, hi) = delay_range(3, sr);
        assert_eq!((lo, hi), (16.0, 24000.0));
        let (lo, hi) = delay_range(2, sr);
        assert!((lo - 0.3333).abs() < 1e-3 && (hi - 500.0).abs() < 1e-3);
        let (lo, hi) = delay_range(0, sr);
        assert!((lo - 0.1).abs() < 1e-5 && (hi - 150.0).abs() < 1e-3);
    }
}
