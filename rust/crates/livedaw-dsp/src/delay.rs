//! Stereo delay line for channel and output time alignment (up to 500 ms).

/// Delay units of the console: meters, feet, milliseconds, samples.
pub const DELAY_MODES: [&str; 4] = ["M", "FT", "MS", "SMP"];

/// Speed of sound used for distance-based delays (m/s, ~20 degrees C).
pub const SPEED_OF_SOUND: f32 = 343.0;

/// Converts a delay `value` in `DELAY_MODES[mode]` units to milliseconds.
pub fn delay_to_ms(value: f32, mode: usize, sample_rate: f64) -> f32 {
    match mode {
        0 => value / SPEED_OF_SOUND * 1000.0,
        1 => value * 0.3048 / SPEED_OF_SOUND * 1000.0,
        2 => value,
        _ => value / sample_rate as f32 * 1000.0,
    }
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
        assert!((delay_to_ms(3.43, 0, 48000.0) - 10.0).abs() < 1e-3);
        assert!((delay_to_ms(480.0, 3, 48000.0) - 10.0).abs() < 1e-3);
    }
}
