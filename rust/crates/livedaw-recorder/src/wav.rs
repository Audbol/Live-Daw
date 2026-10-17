//! WAV files: a 32-bit float writer that survives crashes and grows past 4 GB
//! (RIFF upgraded to RF64, EBU Tech 3306), and a reader for 16/24/32-bit
//! integer and 32-bit float files (plain, WAVE_FORMAT_EXTENSIBLE or RF64).
//!
//! The writer reserves a `JUNK` chunk the size of a `ds64` chunk right after
//! the RIFF header. While the file is below the threshold it is a plain RIFF
//! file; past it the header is rewritten in place as RF64 (`JUNK` becomes
//! `ds64`, the 32-bit sizes become 0xFFFFFFFF). The header is rewritten every
//! few seconds while recording, so a crash loses at most that much audio.

use std::fs::File;
use std::io::{self, BufWriter, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

const WAVE_FORMAT_PCM: u16 = 1;
const WAVE_FORMAT_IEEE_FLOAT: u16 = 3;
const WAVE_FORMAT_EXTENSIBLE: u16 = 0xfffe;

/// The largest RIFF size a plain WAV header can describe.
pub const RIFF_LIMIT: u64 = u32::MAX as u64;

/// Byte offsets in the header the writer produces.
const HEADER_BYTES: u64 = 94;
const FACT_FRAMES_AT: u64 = 82;
const DATA_SIZE_AT: u64 = 90;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SampleFormat {
    Int16,
    Int24,
    Int32,
    Float32,
}

impl SampleFormat {
    pub fn bytes(self) -> usize {
        match self {
            Self::Int16 => 2,
            Self::Int24 => 3,
            Self::Int32 | Self::Float32 => 4,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct WavInfo {
    pub channels: usize,
    pub sample_rate: u32,
    pub format: SampleFormat,
    pub frames: u64,
    pub data_offset: u64,
    pub rf64: bool,
}

fn invalid(msg: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, msg.to_owned())
}

fn u16_at(b: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([b[at], b[at + 1]])
}
fn u32_at(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}
fn u64_at(b: &[u8], at: usize) -> u64 {
    u64::from_le_bytes(b[at..at + 8].try_into().unwrap_or([0; 8]))
}

/// Reads the header of a WAV file.
pub fn read_info(file: &mut File) -> io::Result<WavInfo> {
    let file_len = file.metadata()?.len();
    file.seek(SeekFrom::Start(0))?;
    let mut riff = [0u8; 12];
    file.read_exact(&mut riff)?;
    let rf64 = match &riff[0..4] {
        b"RIFF" => false,
        b"RF64" => true,
        _ => return Err(invalid("not a RIFF/RF64 file")),
    };
    if &riff[8..12] != b"WAVE" {
        return Err(invalid("not a WAVE file"));
    }

    let mut ds64_data: Option<u64> = None;
    let mut format: Option<(u16, usize, u32, u16)> = None; // tag, channels, rate, bits
    let mut pos = 12u64;
    while pos + 8 <= file_len {
        file.seek(SeekFrom::Start(pos))?;
        let mut head = [0u8; 8];
        file.read_exact(&mut head)?;
        let id = [head[0], head[1], head[2], head[3]];
        let size = u32_at(&head, 4) as u64;
        let body = pos + 8;
        match &id {
            b"ds64" if rf64 => {
                let mut b = [0u8; 24];
                file.read_exact(&mut b)?;
                ds64_data = Some(u64_at(&b, 8));
            }
            b"fmt " => {
                let mut b = vec![0u8; size.clamp(16, 40) as usize];
                file.read_exact(&mut b[..size.min(40) as usize])?;
                let mut tag = u16_at(&b, 0);
                let channels = u16_at(&b, 2) as usize;
                let rate = u32_at(&b, 4);
                let bits = u16_at(&b, 14);
                if tag == WAVE_FORMAT_EXTENSIBLE {
                    if size < 40 {
                        return Err(invalid("short WAVE_FORMAT_EXTENSIBLE header"));
                    }
                    tag = u16_at(&b, 24); // first two bytes of the sub-format GUID
                }
                format = Some((tag, channels, rate, bits));
            }
            b"data" => {
                let (tag, channels, sample_rate, bits) = format.ok_or_else(|| invalid("data before fmt"))?;
                let format = match (tag, bits) {
                    (WAVE_FORMAT_PCM, 16) => SampleFormat::Int16,
                    (WAVE_FORMAT_PCM, 24) => SampleFormat::Int24,
                    (WAVE_FORMAT_PCM, 32) => SampleFormat::Int32,
                    (WAVE_FORMAT_IEEE_FLOAT, 32) => SampleFormat::Float32,
                    _ => return Err(io::Error::new(io::ErrorKind::Unsupported, format!("WAV format {tag} / {bits} bits"))),
                };
                if channels == 0 {
                    return Err(invalid("no channels"));
                }
                let available = file_len - body;
                let declared = if rf64 && size == RIFF_LIMIT { ds64_data.unwrap_or(available) } else { size };
                // A crash can leave the header behind the audio: 0 means "until the end".
                let bytes = if declared == 0 { available } else { declared.min(available) };
                let frame_bytes = (channels * format.bytes()) as u64;
                return Ok(WavInfo { channels, sample_rate, format, frames: bytes / frame_bytes, data_offset: body, rf64 });
            }
            _ => {}
        }
        pos = body + size + (size & 1);
    }
    Err(invalid("no data chunk"))
}

/// Random access to the frames of a WAV file, converted to `f32`.
pub struct WavReader {
    file: File,
    info: WavInfo,
    bytes: Vec<u8>,
}

impl WavReader {
    pub fn open(path: &Path) -> io::Result<Self> {
        let mut file = File::open(path)?;
        let info = read_info(&mut file)?;
        Ok(Self { file, info, bytes: Vec::new() })
    }

    pub fn info(&self) -> &WavInfo {
        &self.info
    }

    /// Reads `out[0].len()` frames starting at `frame` into `out` (one slice
    /// per channel; extra slices repeat the last channel). Frames outside the
    /// file are zeros.
    pub fn read(&mut self, frame: i64, out: &mut [&mut [f32]]) -> io::Result<()> {
        let Some(n) = out.first().map(|o| o.len()) else { return Ok(()) };
        for o in out.iter_mut() {
            o.fill(0.0);
        }
        // The part of [frame, frame + n) inside the file.
        let first = frame.max(0);
        let last = (frame + n as i64).min(self.info.frames as i64);
        if last <= first {
            return Ok(());
        }
        let count = (last - first) as usize;
        let skip = (first - frame) as usize;
        let channels = self.info.channels;
        let width = self.info.format.bytes();
        let frame_bytes = channels * width;
        self.bytes.resize(count * frame_bytes, 0);
        self.file.seek(SeekFrom::Start(self.info.data_offset + first as u64 * frame_bytes as u64))?;
        self.file.read_exact(&mut self.bytes)?;

        let format = self.info.format;
        let sample = |b: &[u8]| -> f32 {
            match format {
                SampleFormat::Int16 => i16::from_le_bytes([b[0], b[1]]) as f32 / 32768.0,
                SampleFormat::Int24 => (i32::from_le_bytes([0, b[0], b[1], b[2]]) >> 8) as f32 / 8_388_608.0,
                SampleFormat::Int32 => i32::from_le_bytes([b[0], b[1], b[2], b[3]]) as f32 / 2_147_483_648.0,
                SampleFormat::Float32 => f32::from_le_bytes([b[0], b[1], b[2], b[3]]),
            }
        };
        for (c, o) in out.iter_mut().enumerate() {
            let ch = c.min(channels - 1);
            for (i, x) in o[skip..skip + count].iter_mut().enumerate() {
                let at = i * frame_bytes + ch * width;
                *x = sample(&self.bytes[at..at + width]);
            }
        }
        Ok(())
    }
}

/// Writes 32-bit float WAV files (mono or stereo, any channel count).
pub struct WavWriter {
    out: BufWriter<File>,
    path: PathBuf,
    channels: usize,
    sample_rate: u32,
    frames: u64,
    rf64_threshold: u64,
    rf64: bool,
    bytes: Vec<u8>,
}

impl WavWriter {
    /// Creates (or truncates) `path` and writes an empty header.
    pub fn create(path: &Path, channels: usize, sample_rate: u32) -> io::Result<Self> {
        if let Some(dir) = path.parent().filter(|d| !d.as_os_str().is_empty()) {
            std::fs::create_dir_all(dir)?;
        }
        let file = File::create(path)?;
        let mut w = Self {
            out: BufWriter::with_capacity(1 << 18, file),
            path: path.to_owned(),
            channels: channels.max(1),
            sample_rate,
            frames: 0,
            rf64_threshold: RIFF_LIMIT,
            rf64: false,
            bytes: Vec::new(),
        };
        let header = w.header();
        w.out.write_all(&header)?;
        w.out.flush()?;
        Ok(w)
    }

    /// Sizes above which the header becomes RF64 (tests use a small one).
    pub fn set_rf64_threshold(&mut self, riff_bytes: u64) {
        self.rf64_threshold = riff_bytes.min(RIFF_LIMIT);
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn channels(&self) -> usize {
        self.channels
    }

    pub fn frames(&self) -> u64 {
        self.frames
    }

    pub fn is_rf64(&self) -> bool {
        self.rf64
    }

    fn data_bytes(&self) -> u64 {
        self.frames * self.channels as u64 * 4
    }

    fn header(&self) -> Vec<u8> {
        let data = self.data_bytes();
        let riff = HEADER_BYTES - 8 + data;
        let rf64 = self.rf64;
        let u32_or_max = |v: u64| if rf64 { u32::MAX } else { v.min(RIFF_LIMIT) as u32 };
        let block_align = (self.channels * 4) as u16;
        let mut h = Vec::with_capacity(HEADER_BYTES as usize);
        h.extend_from_slice(if rf64 { b"RF64" } else { b"RIFF" });
        h.extend_from_slice(&u32_or_max(riff).to_le_bytes());
        h.extend_from_slice(b"WAVE");
        // ds64 (RF64) or the JUNK chunk reserving its space.
        h.extend_from_slice(if rf64 { b"ds64" } else { b"JUNK" });
        h.extend_from_slice(&28u32.to_le_bytes());
        if rf64 {
            h.extend_from_slice(&riff.to_le_bytes());
            h.extend_from_slice(&data.to_le_bytes());
            h.extend_from_slice(&self.frames.to_le_bytes());
            h.extend_from_slice(&0u32.to_le_bytes()); // no table
        } else {
            h.extend_from_slice(&[0u8; 28]);
        }
        h.extend_from_slice(b"fmt ");
        h.extend_from_slice(&18u32.to_le_bytes());
        h.extend_from_slice(&WAVE_FORMAT_IEEE_FLOAT.to_le_bytes());
        h.extend_from_slice(&(self.channels as u16).to_le_bytes());
        h.extend_from_slice(&self.sample_rate.to_le_bytes());
        h.extend_from_slice(&(self.sample_rate * block_align as u32).to_le_bytes());
        h.extend_from_slice(&block_align.to_le_bytes());
        h.extend_from_slice(&32u16.to_le_bytes());
        h.extend_from_slice(&0u16.to_le_bytes()); // cbSize
        h.extend_from_slice(b"fact");
        h.extend_from_slice(&4u32.to_le_bytes());
        h.extend_from_slice(&u32_or_max(self.frames).to_le_bytes());
        h.extend_from_slice(b"data");
        h.extend_from_slice(&u32_or_max(data).to_le_bytes());
        debug_assert_eq!(h.len() as u64, HEADER_BYTES);
        debug_assert_eq!(&h[FACT_FRAMES_AT as usize - 8..FACT_FRAMES_AT as usize - 4], b"fact");
        debug_assert_eq!(&h[DATA_SIZE_AT as usize - 4..DATA_SIZE_AT as usize], b"data");
        h
    }

    /// Appends `n` frames from planar channels (missing channels write silence).
    pub fn write_planar(&mut self, channels: &[&[f32]], n: usize) -> io::Result<()> {
        let nc = self.channels;
        self.bytes.clear();
        self.bytes.reserve(n * nc * 4);
        for i in 0..n {
            for c in 0..nc {
                let x = channels.get(c).and_then(|ch| ch.get(i)).copied().unwrap_or(0.0);
                self.bytes.extend_from_slice(&x.to_le_bytes());
            }
        }
        self.out.write_all(&self.bytes)?;
        self.frames += n as u64;
        Ok(())
    }

    /// Appends `n` silent frames.
    pub fn write_silence(&mut self, n: usize) -> io::Result<()> {
        let zeros = [0u8; 4096];
        let mut left = n * self.channels * 4;
        while left > 0 {
            let k = left.min(zeros.len());
            self.out.write_all(&zeros[..k])?;
            left -= k;
        }
        self.frames += n as u64;
        Ok(())
    }

    /// Rewrites the header for the frames written so far (RF64 once past the
    /// threshold) and hands everything to the OS, so the file is valid if the
    /// application dies now.
    pub fn update_header(&mut self) -> io::Result<()> {
        if !self.rf64 && HEADER_BYTES - 8 + self.data_bytes() > self.rf64_threshold {
            self.rf64 = true;
        }
        self.out.flush()?;
        let header = self.header();
        let file = self.out.get_mut();
        file.seek(SeekFrom::Start(0))?;
        file.write_all(&header)?;
        file.seek(SeekFrom::End(0))?;
        Ok(())
    }

    /// Writes the final header and closes the file. Returns the frames written.
    pub fn finalize(mut self) -> io::Result<u64> {
        self.update_header()?;
        self.out.get_mut().sync_data()?;
        Ok(self.frames)
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    pub fn temp_dir(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("livedaw-rec-{}-{}", name, std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    /// Writes an integer PCM file the way other software does (16/24/32 bits,
    /// optionally WAVE_FORMAT_EXTENSIBLE).
    pub fn write_pcm(path: &Path, channels: usize, bits: u16, extensible: bool, frames: &[Vec<f32>]) {
        let n = frames[0].len();
        let width = bits as usize / 8;
        let mut data = Vec::new();
        for i in 0..n {
            for ch in frames.iter().take(channels) {
                let x = ch[i].clamp(-1.0, 1.0);
                match bits {
                    16 => data.extend_from_slice(&((x * 32767.0).round() as i16).to_le_bytes()),
                    24 => data.extend_from_slice(&((x * 8_388_607.0).round() as i32).to_le_bytes()[..3]),
                    _ => data.extend_from_slice(&((x as f64 * 2_147_483_647.0).round() as i32).to_le_bytes()),
                }
            }
        }
        let mut fmt = Vec::new();
        fmt.extend_from_slice(&(if extensible { WAVE_FORMAT_EXTENSIBLE } else { WAVE_FORMAT_PCM }).to_le_bytes());
        fmt.extend_from_slice(&(channels as u16).to_le_bytes());
        fmt.extend_from_slice(&48000u32.to_le_bytes());
        fmt.extend_from_slice(&(48000 * (channels * width) as u32).to_le_bytes());
        fmt.extend_from_slice(&((channels * width) as u16).to_le_bytes());
        fmt.extend_from_slice(&bits.to_le_bytes());
        if extensible {
            fmt.extend_from_slice(&22u16.to_le_bytes());
            fmt.extend_from_slice(&bits.to_le_bytes());
            fmt.extend_from_slice(&3u32.to_le_bytes());
            fmt.extend_from_slice(&[1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71]);
        }
        let mut file = Vec::new();
        file.extend_from_slice(b"RIFF");
        file.extend_from_slice(&((4 + 8 + fmt.len() + 8 + 10 + 8 + data.len()) as u32).to_le_bytes());
        file.extend_from_slice(b"WAVE");
        file.extend_from_slice(b"fmt ");
        file.extend_from_slice(&(fmt.len() as u32).to_le_bytes());
        file.extend_from_slice(&fmt);
        file.extend_from_slice(b"LIST"); // an odd-sized chunk to skip (padded)
        file.extend_from_slice(&9u32.to_le_bytes());
        file.extend_from_slice(b"INFOabcde\0");
        file.extend_from_slice(b"data");
        file.extend_from_slice(&(data.len() as u32).to_le_bytes());
        file.extend_from_slice(&data);
        std::fs::write(path, file).unwrap();
    }

    fn read_all(path: &Path) -> (WavInfo, Vec<Vec<f32>>) {
        let mut r = WavReader::open(path).unwrap();
        let info = *r.info();
        let mut chans = vec![vec![0.0; info.frames as usize]; info.channels];
        let mut refs: Vec<&mut [f32]> = chans.iter_mut().map(|c| &mut c[..]).collect();
        r.read(0, &mut refs).unwrap();
        (info, chans)
    }

    #[test]
    fn float_round_trip_is_exact() {
        let dir = temp_dir("wav-float");
        let path = dir.join("take.wav");
        let left: Vec<f32> = (0..1000).map(|i| (i as f32 * 0.01).sin()).collect();
        let right: Vec<f32> = (0..1000).map(|i| -(i as f32) / 1000.0).collect();
        let mut w = WavWriter::create(&path, 2, 44100).unwrap();
        w.write_planar(&[&left[..600], &right[..600]], 600).unwrap();
        w.update_header().unwrap();
        w.write_planar(&[&left[600..], &right[600..]], 400).unwrap();
        w.write_silence(10).unwrap();
        assert_eq!(w.finalize().unwrap(), 1010);

        let (info, chans) = read_all(&path);
        assert_eq!(
            (info.channels, info.sample_rate, info.format, info.frames, info.rf64),
            (2, 44100, SampleFormat::Float32, 1010, false)
        );
        assert_eq!(&chans[0][..1000], &left[..]);
        assert_eq!(&chans[1][..1000], &right[..]);
        assert!(chans[0][1000..].iter().all(|&x| x == 0.0));

        // Random access, partly outside the file.
        let mut r = WavReader::open(&path).unwrap();
        let mut a = [9.0f32; 6];
        let mut b = [9.0f32; 6];
        r.read(-2, &mut [&mut a[..], &mut b[..]]).unwrap();
        assert_eq!(&a[..2], &[0.0, 0.0]);
        assert_eq!(&a[2..], &left[..4]);
        assert_eq!(&b[2..], &right[..4]);
        let mut tail = [9.0f32; 4];
        r.read(1008, &mut [&mut tail[..]]).unwrap();
        assert_eq!(tail, [0.0; 4]);
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn rf64_past_the_threshold() {
        let dir = temp_dir("wav-rf64");
        let path = dir.join("big.wav");
        let mut w = WavWriter::create(&path, 1, 48000).unwrap();
        w.set_rf64_threshold(1000);
        let ramp: Vec<f32> = (0..200).map(|i| i as f32).collect();
        w.write_planar(&[&ramp], 200).unwrap(); // 800 bytes: still RIFF
        w.update_header().unwrap();
        assert!(!w.is_rf64());
        assert_eq!(&std::fs::read(&path).unwrap()[..4], b"RIFF");
        w.write_planar(&[&ramp], 200).unwrap(); // 1600 bytes: RF64
        w.finalize().unwrap();

        let bytes = std::fs::read(&path).unwrap();
        assert_eq!(&bytes[..4], b"RF64");
        assert_eq!(u32_at(&bytes, 4), u32::MAX);
        assert_eq!(&bytes[12..16], b"ds64");
        assert_eq!(u64_at(&bytes, 20), bytes.len() as u64 - 8, "ds64 RIFF size");
        assert_eq!(u64_at(&bytes, 28), 1600, "ds64 data size");
        assert_eq!(u64_at(&bytes, 36), 400, "ds64 sample count");
        assert_eq!(u32_at(&bytes, DATA_SIZE_AT as usize), u32::MAX);

        let (info, chans) = read_all(&path);
        assert!(info.rf64);
        assert_eq!(info.frames, 400);
        assert_eq!(&chans[0][..200], &ramp[..]);
        assert_eq!(&chans[0][200..], &ramp[..]);
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn a_crash_before_the_header_rewrite_still_reads() {
        let dir = temp_dir("wav-crash");
        let path = dir.join("crash.wav");
        let mut w = WavWriter::create(&path, 1, 48000).unwrap();
        w.write_planar(&[&[0.5; 64]], 64).unwrap();
        w.out.flush().unwrap();
        std::mem::forget(w); // no header update, no finalize
        let (info, chans) = read_all(&path);
        assert_eq!(info.frames, 64);
        assert!(chans[0].iter().all(|&x| x == 0.5));
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn integer_formats_mono_and_stereo() {
        let dir = temp_dir("wav-int");
        let left: Vec<f32> = (0..300).map(|i| ((i as f32) * 0.05).sin() * 0.9).collect();
        let right: Vec<f32> = left.iter().map(|x| -x * 0.5).collect();
        for (bits, ext, tol) in
            [(16u16, false, 1.0 / 16000.0), (24, true, 1e-6), (32, false, 1e-8), (24, false, 1e-6), (16, true, 1.0 / 16000.0)]
        {
            for channels in [1usize, 2] {
                let path = dir.join(format!("pcm{bits}-{ext}-{channels}.wav"));
                write_pcm(&path, channels, bits, ext, &[left.clone(), right.clone()]);
                let (info, chans) = read_all(&path);
                assert_eq!((info.channels, info.frames), (channels, 300));
                assert_eq!(info.format.bytes(), bits as usize / 8);
                for (i, x) in chans[0].iter().enumerate() {
                    assert!((x - left[i]).abs() <= tol, "{bits} bits: {x} vs {}", left[i]);
                }
                if channels == 2 {
                    assert!(chans[1].iter().zip(&right).all(|(a, b)| (a - b).abs() <= tol));
                }
            }
        }
        // Full scale and negative full scale.
        let path = dir.join("full.wav");
        write_pcm(&path, 1, 24, false, &[vec![1.0, -1.0, 0.0]]);
        let (_, chans) = read_all(&path);
        assert!((chans[0][0] - 1.0).abs() < 1e-6 && (chans[0][1] + 1.0).abs() < 1e-6 && chans[0][2] == 0.0);
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn rejects_what_it_cannot_read() {
        let dir = temp_dir("wav-bad");
        let path = dir.join("bad.wav");
        std::fs::write(&path, b"RIFF\x04\0\0\0AIFF").unwrap();
        assert!(WavReader::open(&path).is_err());
        std::fs::write(&path, b"not a wav").unwrap();
        assert!(WavReader::open(&path).is_err());
        let _ = std::fs::remove_dir_all(dir);
    }
}
