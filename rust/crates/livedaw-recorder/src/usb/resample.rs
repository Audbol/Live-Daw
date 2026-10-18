//! Reads an audio file at the engine's sample rate: a windowed-sinc resampler
//! (64 taps, 256 interpolated phases, Blackman window) for files whose rate
//! differs, a plain copy otherwise.
//!
//! Output frame `k` of a file sits at source position `k * in_rate /
//! out_rate`, computed exactly in integers, so seeking anywhere and reading
//! block by block give the same samples and the position never drifts.

use crate::codec::{AudioInfo, AudioReader};
use std::io;

const HALF_TAPS: usize = 32;
const TAPS: usize = 2 * HALF_TAPS;
const PHASES: usize = 256;
/// Source frames read per refill (plus the kernel's reach).
const SOURCE_BLOCK: usize = 8192;

fn gcd(a: u64, b: u64) -> u64 {
    if b == 0 {
        a
    } else {
        gcd(b, a % b)
    }
}

/// `ceil(a * b / c)` without overflow for realistic file lengths.
pub(crate) fn scale_ceil(a: u64, b: u64, c: u64) -> u64 {
    ((a as u128 * b as u128).div_ceil(c.max(1) as u128)) as u64
}

pub(crate) struct FileStream {
    reader: Box<dyn AudioReader>,
    info: AudioInfo,
    /// Reduced rates: output frame k is source position k * num / den.
    num: u64,
    den: u64,
    channels: usize,
    /// Length in output frames.
    length: u64,
    /// `(PHASES + 1) * TAPS` coefficients; row p is the kernel at fraction p / PHASES.
    kernel: Vec<f32>,
    /// Cached source frames `[src_start, src_start + src_len)` per channel.
    src: Vec<Vec<f32>>,
    src_start: i64,
    src_len: usize,
}

impl FileStream {
    pub fn new(reader: Box<dyn AudioReader>, out_rate: u32) -> Self {
        let info = *reader.info();
        let in_rate = u64::from(info.sample_rate.max(1));
        let out_rate = u64::from(out_rate.max(1));
        let g = gcd(in_rate, out_rate);
        let (num, den) = (in_rate / g, out_rate / g);
        let channels = info.channels.max(1);
        let kernel = if num == den { Vec::new() } else { make_kernel(num, den) };
        Self {
            reader,
            info,
            num,
            den,
            channels,
            length: scale_ceil(info.frames, den, num),
            kernel,
            src: vec![Vec::new(); channels],
            src_start: 0,
            src_len: 0,
        }
    }

    pub fn channels(&self) -> usize {
        self.channels
    }

    /// The file's header.
    pub fn info(&self) -> &AudioInfo {
        &self.info
    }

    /// The file's length in output frames.
    pub fn length(&self) -> u64 {
        self.length
    }

    pub fn resampling(&self) -> bool {
        self.num != self.den
    }

    /// Makes `[first, first + len)` of the source available in `src`.
    fn ensure(&mut self, first: i64, len: usize) -> io::Result<()> {
        let have = first >= self.src_start && first + len as i64 <= self.src_start + self.src_len as i64;
        if have {
            return Ok(());
        }
        let n = len.max(SOURCE_BLOCK + TAPS);
        for c in &mut self.src {
            c.resize(n, 0.0);
        }
        let mut refs: Vec<&mut [f32]> = self.src.iter_mut().map(|c| &mut c[..n]).collect();
        self.reader.read(first, &mut refs)?;
        self.src_start = first;
        self.src_len = n;
        Ok(())
    }

    /// Writes output frames `[start, start + n)` into `out` (one slice per
    /// file channel, each at least `n` long). Frames past the end are zeros.
    pub fn render(&mut self, start: u64, n: usize, out: &mut [Vec<f32>]) -> io::Result<()> {
        if !self.resampling() {
            self.ensure(start as i64, n)?;
            let skip = (start as i64 - self.src_start) as usize;
            for (o, s) in out.iter_mut().zip(&self.src) {
                o[..n].copy_from_slice(&s[skip..skip + n]);
            }
            return Ok(());
        }
        let mut k = start;
        let mut done = 0;
        while done < n {
            // The source span this run of output frames needs.
            let last = k + (n - done) as u64 - 1;
            let first_src = (k as u128 * self.num as u128 / self.den as u128) as i64 - HALF_TAPS as i64 + 1;
            let last_src = (last as u128 * self.num as u128 / self.den as u128) as i64 + HALF_TAPS as i64;
            let span = (last_src - first_src + 1) as usize;
            // Render at most what one refill covers.
            let run = if span <= SOURCE_BLOCK + TAPS {
                n - done
            } else {
                ((SOURCE_BLOCK as u64 * self.den / self.num) as usize).clamp(1, n - done)
            };
            let run_last = k + run as u64 - 1;
            let run_last_src = (run_last as u128 * self.num as u128 / self.den as u128) as i64 + HALF_TAPS as i64;
            self.ensure(first_src, (run_last_src - first_src + 1) as usize)?;
            for i in 0..run {
                let kk = k + i as u64;
                let t = kk as u128 * self.num as u128;
                let ipart = (t / self.den as u128) as i64;
                let frac = (t % self.den as u128) as f64 / self.den as f64;
                let pf = frac * PHASES as f64;
                let p = (pf as usize).min(PHASES - 1);
                let w = (pf - p as f64) as f32;
                let row0 = &self.kernel[p * TAPS..(p + 1) * TAPS];
                let row1 = &self.kernel[(p + 1) * TAPS..(p + 2) * TAPS];
                let base = (ipart - HALF_TAPS as i64 + 1 - self.src_start) as usize;
                for (c, o) in out.iter_mut().enumerate().take(self.channels) {
                    let s = &self.src[c][base..base + TAPS];
                    let mut acc = 0.0f32;
                    for m in 0..TAPS {
                        acc += s[m] * (row0[m] + w * (row1[m] - row0[m]));
                    }
                    o[done + i] = acc;
                }
            }
            k += run as u64;
            done += run;
        }
        // Past the end: zeros (the kernel's tail rings out inside the length).
        let valid = self.length.saturating_sub(start).min(n as u64) as usize;
        for o in out.iter_mut().take(self.channels) {
            o[valid..n].fill(0.0);
        }
        Ok(())
    }
}

/// The interpolation kernel for a `num / den` rate ratio (source frames per
/// output frame), low-passed below the lower Nyquist frequency.
fn make_kernel(num: u64, den: u64) -> Vec<f32> {
    let cutoff = (den as f64 / num as f64).min(1.0) * 0.95;
    let mut k = vec![0.0f32; (PHASES + 1) * TAPS];
    for p in 0..=PHASES {
        let frac = p as f64 / PHASES as f64;
        let row = &mut k[p * TAPS..(p + 1) * TAPS];
        let mut sum = 0.0;
        for (m, c) in row.iter_mut().enumerate() {
            // Distance from the output position to source tap m.
            let x = frac + HALF_TAPS as f64 - 1.0 - m as f64;
            let u = x / HALF_TAPS as f64;
            let window = if u.abs() >= 1.0 {
                0.0
            } else {
                0.42 + 0.5 * (std::f64::consts::PI * u).cos() + 0.08 * (2.0 * std::f64::consts::PI * u).cos()
            };
            let arg = std::f64::consts::PI * cutoff * x;
            let sinc = if arg.abs() < 1e-12 { 1.0 } else { arg.sin() / arg };
            let v = cutoff * sinc * window;
            *c = v as f32;
            sum += v;
        }
        // Unity gain at DC for every phase.
        for c in row.iter_mut() {
            *c = (*c as f64 / sum) as f32;
        }
    }
    k
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wav::WavWriter;
    use std::path::{Path, PathBuf};

    fn temp_dir(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("livedaw-usbrs-{}-{}", name, std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn sine_file(path: &Path, rate: u32, freq: f64, frames: usize) {
        let x: Vec<f32> =
            (0..frames).map(|i| (2.0 * std::f64::consts::PI * freq * i as f64 / rate as f64).sin() as f32 * 0.5).collect();
        let mut w = WavWriter::create(path, 1, rate).unwrap();
        w.write_planar(&[&x], frames).unwrap();
        w.finalize().unwrap();
    }

    #[test]
    fn same_rate_copies_and_converted_rate_keeps_the_tone() {
        let dir = temp_dir("rs");
        let path = dir.join("a.wav");
        sine_file(&path, 48000, 1000.0, 48000);
        let mut s = FileStream::new(crate::codec::open_reader(&path).unwrap(), 48000);
        assert!(!s.resampling());
        assert_eq!(s.length(), 48000);
        let mut out = vec![vec![0.0; 100]];
        s.render(1000, 100, &mut out).unwrap();
        let expect = |i: usize| (2.0 * std::f64::consts::PI * 1000.0 * i as f64 / 48000.0).sin() as f32 * 0.5;
        assert!(out[0].iter().enumerate().all(|(i, &v)| v == expect(1000 + i)));

        // 44.1 kHz file, 48 kHz engine: same tone at the output rate.
        let path = dir.join("b.wav");
        sine_file(&path, 44100, 1000.0, 44100);
        let mut s = FileStream::new(crate::codec::open_reader(&path).unwrap(), 48000);
        assert!(s.resampling());
        assert_eq!(s.length(), 48000);
        // Read in odd blocks, then all at once: identical.
        let mut whole = vec![vec![0.0; 48000]];
        s.render(0, 48000, &mut whole).unwrap();
        let mut pieces = vec![0.0f32; 48000];
        let mut at = 0;
        for len in [1usize, 777, 5000, 12345].iter().cycle() {
            if at >= 48000 {
                break;
            }
            let n = (*len).min(48000 - at);
            let mut buf = vec![vec![0.0; n]];
            s.render(at as u64, n, &mut buf).unwrap();
            pieces[at..at + n].copy_from_slice(&buf[0]);
            at += n;
        }
        assert_eq!(whole[0], pieces);
        let err = (100..47900).map(|i| (whole[0][i] - expect(i)).abs()).fold(0.0f32, f32::max);
        assert!(err < 2e-3, "44.1 -> 48 kHz error {err}");

        // 48 kHz file, 44.1 kHz engine: a tone above the new Nyquist is removed.
        let path = dir.join("c.wav");
        sine_file(&path, 48000, 23500.0, 48000);
        let mut s = FileStream::new(crate::codec::open_reader(&path).unwrap(), 44100);
        assert_eq!(s.length(), 44100);
        let mut out = vec![vec![0.0; 44100]];
        s.render(0, 44100, &mut out).unwrap();
        let peak = out[0][200..44000].iter().fold(0.0f32, |p, v| p.max(v.abs()));
        assert!(peak < 0.02, "aliasing {peak}");
        let _ = std::fs::remove_dir_all(dir);
    }
}
