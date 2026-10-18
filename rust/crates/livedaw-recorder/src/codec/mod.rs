//! Audio file formats of both recorders: WAV / RF64 ([`crate::wav`]), AIFF
//! / AIFF-C ([`aiff`]), FLAC ([`flac`]) and MP3 ([`mp3`]), behind one writer
//! trait ([`AudioWriter`], made by [`create_writer`]) and one reader trait
//! ([`AudioReader`], opened by [`open_reader`], which recognizes the format
//! by its contents).
//!
//! Every encoder and decoder is written here from the format
//! specifications (no dependencies):
//! * WAV: RIFF / RF64 (EBU Tech 3306), 16/24-bit PCM or 32-bit float.
//! * AIFF: big-endian 16/24-bit PCM; AIFF-C `fl32` for 32-bit float.
//! * FLAC (RFC 9639): 16/24-bit lossless, fixed and LPC predictors with
//!   Rice-coded residuals, verbatim fallback, STREAMINFO (total samples, MD5)
//!   patched when the file is finalized.
//! * MP3 (ISO/IEC 11172-3, MPEG-1 Layer III): CBR 128-320 kbit/s at 32, 44.1
//!   or 48 kHz, mono or (joint) stereo, with an `Info` frame carrying the
//!   frame count and the encoder delay / padding for gapless reading.
//!
//! Writers are crash-safe as the WAV writer is: `update_header` (every few
//! seconds while recording) hands everything to the OS and leaves a file the
//! readers accept; `finalize` writes the final headers.

// The codecs follow the index notation of their specifications.
#![allow(clippy::needless_range_loop)]

pub mod aiff;
pub(crate) mod bits;
pub mod flac;
mod md5;
pub mod mp3;

use crate::wav::{self, SampleFormat};
use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom};
use std::path::Path;

/// The container / codec of a file.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Default)]
pub enum FileFormat {
    #[default]
    Wav = 0,
    Flac = 1,
    Aiff = 2,
    Mp3 = 3,
}

impl FileFormat {
    pub const ALL: [FileFormat; 4] = [Self::Wav, Self::Flac, Self::Aiff, Self::Mp3];

    pub fn from_i32(v: i32) -> Self {
        match v {
            1 => Self::Flac,
            2 => Self::Aiff,
            3 => Self::Mp3,
            _ => Self::Wav,
        }
    }

    /// The file name extension new files get (without the dot).
    pub fn extension(self) -> &'static str {
        match self {
            Self::Wav => "wav",
            Self::Flac => "flac",
            Self::Aiff => "aif",
            Self::Mp3 => "mp3",
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Self::Wav => "WAV",
            Self::Flac => "FLAC",
            Self::Aiff => "AIFF",
            Self::Mp3 => "MP3",
        }
    }

    /// The format a file name extension stands for (case-insensitive).
    pub fn from_extension(ext: &str) -> Option<Self> {
        match ext.to_ascii_lowercase().as_str() {
            "wav" | "wave" | "bwf" | "rf64" => Some(Self::Wav),
            "flac" => Some(Self::Flac),
            "aif" | "aiff" | "aifc" => Some(Self::Aiff),
            "mp3" => Some(Self::Mp3),
            _ => None,
        }
    }

    /// Whether `path` has an extension of one of the formats.
    pub fn is_audio_file(path: &Path) -> bool {
        path.extension().and_then(|e| e.to_str()).and_then(Self::from_extension).is_some()
    }
}

/// MP3 bit rates the encoder offers (kbit/s).
pub const MP3_BITRATES: [u32; 4] = [128, 192, 256, 320];

/// What a recorder writes: the format, the sample format of the PCM formats
/// (16/24-bit integer, or 32-bit float for WAV and AIFF) and the MP3 bit rate.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EncodeFormat {
    pub format: FileFormat,
    pub sample: SampleFormat,
    /// MP3 only (kbit/s; the nearest of [`MP3_BITRATES`]).
    pub kbps: u32,
}

impl Default for EncodeFormat {
    /// 32-bit float WAV (the multitrack recorder's default).
    fn default() -> Self {
        Self { format: FileFormat::Wav, sample: SampleFormat::Float32, kbps: 320 }
    }
}

impl EncodeFormat {
    pub fn new(format: FileFormat, sample: SampleFormat, kbps: u32) -> Self {
        Self { format, sample, kbps }.normalized()
    }

    /// The nearest format the encoders can write: FLAC is 16 or 24 bits
    /// (float and 32 bits become 24), WAV / AIFF 16, 24 or 32-bit float
    /// (32-bit integer becomes float), MP3 one of [`MP3_BITRATES`].
    pub fn normalized(self) -> Self {
        let sample = match (self.format, self.sample) {
            (FileFormat::Flac, SampleFormat::Int16) => SampleFormat::Int16,
            (FileFormat::Flac, _) => SampleFormat::Int24,
            (_, SampleFormat::Int32) => SampleFormat::Float32,
            (_, s) => s,
        };
        let kbps = *MP3_BITRATES.iter().min_by_key(|&&b| b.abs_diff(self.kbps)).unwrap_or(&320);
        Self { format: self.format, sample, kbps }
    }

    /// Bits per sample of the PCM formats (0 for MP3).
    pub fn bits(self) -> u16 {
        if self.format == FileFormat::Mp3 {
            0
        } else {
            self.sample.bits()
        }
    }

    pub fn extension(self) -> &'static str {
        self.format.extension()
    }

    /// Why `channels` at `sample_rate` can't be written in this format, if so.
    pub fn check(self, channels: usize, sample_rate: u32) -> Result<(), String> {
        match self.format {
            FileFormat::Mp3 if channels > 2 => Err("MP3 records 1 or 2 channels".into()),
            FileFormat::Mp3 if !mp3::SAMPLE_RATES.contains(&sample_rate) => {
                Err(format!("MP3 needs 32, 44.1 or 48 kHz (the engine runs at {sample_rate} Hz)"))
            }
            FileFormat::Flac if channels > 8 => Err("FLAC records at most 8 channels".into()),
            _ if channels == 0 => Err("No channels".into()),
            _ => Ok(()),
        }
    }
}

/// What a reader knows about a file.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AudioInfo {
    pub format: FileFormat,
    pub channels: usize,
    pub sample_rate: u32,
    pub frames: u64,
    /// Bits per sample (MP3: 16, what it decodes to).
    pub bits: u16,
    pub float: bool,
    /// MP3: the bit rate of the first frame (kbit/s); 0 otherwise.
    pub kbps: u32,
}

/// Writes an audio file block by block (a recorder's disk thread).
pub trait AudioWriter: Send {
    fn path(&self) -> &Path;
    fn channels(&self) -> usize;
    /// Frames written so far.
    fn frames(&self) -> u64;
    /// Appends `n` frames from planar channels (missing channels write
    /// silence; integer formats round and clip).
    fn write_planar(&mut self, channels: &[&[f32]], n: usize) -> io::Result<()>;
    /// Appends `n` silent frames.
    fn write_silence(&mut self, n: usize) -> io::Result<()>;
    /// Makes the file valid for what was written so far (as far as the
    /// format allows before the end) and hands the data to the OS.
    fn update_header(&mut self) -> io::Result<()>;
    /// Writes what is pending and the final headers, and closes the file.
    /// Returns the frames written.
    fn finalize(self: Box<Self>) -> io::Result<u64>;
    /// WAV only: the RIFF size past which the header becomes RF64 (tests).
    fn set_rf64_threshold(&mut self, _riff_bytes: u64) {}
}

/// Reads an audio file with random access, converted to `f32`.
pub trait AudioReader: Send {
    fn info(&self) -> &AudioInfo;
    /// Reads `out[0].len()` frames starting at `frame` into `out` (one slice
    /// per channel; extra slices repeat the last channel). Frames outside the
    /// file are zeros.
    fn read(&mut self, frame: i64, out: &mut [&mut [f32]]) -> io::Result<()>;
}

/// Creates (or truncates) `path` as a file in `format`.
pub fn create_writer(path: &Path, channels: usize, sample_rate: u32, format: EncodeFormat) -> io::Result<Box<dyn AudioWriter>> {
    let format = format.normalized();
    format.check(channels, sample_rate).map_err(|e| io::Error::new(io::ErrorKind::Unsupported, e))?;
    if let Some(dir) = path.parent().filter(|d| !d.as_os_str().is_empty()) {
        std::fs::create_dir_all(dir)?;
    }
    Ok(match format.format {
        FileFormat::Wav => Box::new(wav::WavWriter::create_with_format(path, channels, sample_rate, format.sample)?),
        FileFormat::Aiff => Box::new(aiff::AiffWriter::create(path, channels, sample_rate, format.sample)?),
        FileFormat::Flac => Box::new(flac::FlacWriter::create(path, channels, sample_rate, format.sample.bits() as u32)?),
        FileFormat::Mp3 => mp3::Mp3Writer::create(path, channels, sample_rate, format.kbps)?,
    })
}

/// The format of a file, from its first bytes.
pub fn sniff(file: &mut File) -> io::Result<FileFormat> {
    file.seek(SeekFrom::Start(0))?;
    let mut head = [0u8; 12];
    let mut got = 0;
    while got < head.len() {
        let n = file.read(&mut head[got..])?;
        if n == 0 {
            break;
        }
        got += n;
    }
    file.seek(SeekFrom::Start(0))?;
    let h = &head[..got];
    if h.len() >= 12 && (h.starts_with(b"RIFF") || h.starts_with(b"RF64")) && &h[8..12] == b"WAVE" {
        Ok(FileFormat::Wav)
    } else if h.len() >= 12 && h.starts_with(b"FORM") && (&h[8..12] == b"AIFF" || &h[8..12] == b"AIFC") {
        Ok(FileFormat::Aiff)
    } else if h.starts_with(b"fLaC") || (h.starts_with(b"ID3") && flac_after_id3(file)?) {
        Ok(FileFormat::Flac)
    } else if h.starts_with(b"ID3") || (h.len() >= 2 && h[0] == 0xff && h[1] & 0xe0 == 0xe0) {
        Ok(FileFormat::Mp3)
    } else {
        Err(io::Error::new(io::ErrorKind::InvalidData, "not a WAV, AIFF, FLAC or MP3 file"))
    }
}

/// Size of an ID3v2 tag starting at the current file position (0 if none).
pub(crate) fn id3v2_size(head: &[u8]) -> u64 {
    if head.len() >= 10 && head.starts_with(b"ID3") {
        let size = head[6..10].iter().fold(0u64, |a, &b| (a << 7) | u64::from(b & 0x7f));
        let footer = if head[5] & 0x10 != 0 { 10 } else { 0 };
        10 + size + footer
    } else {
        0
    }
}

fn flac_after_id3(file: &mut File) -> io::Result<bool> {
    let mut head = [0u8; 10];
    file.seek(SeekFrom::Start(0))?;
    file.read_exact(&mut head)?;
    let skip = id3v2_size(&head);
    file.seek(SeekFrom::Start(skip))?;
    let mut magic = [0u8; 4];
    let ok = file.read_exact(&mut magic).is_ok() && &magic == b"fLaC";
    file.seek(SeekFrom::Start(0))?;
    Ok(ok)
}

/// Opens any file of the four formats (recognized by its contents).
pub fn open_reader(path: &Path) -> io::Result<Box<dyn AudioReader>> {
    let mut file = File::open(path)?;
    Ok(match sniff(&mut file)? {
        FileFormat::Wav => Box::new(wav::WavReader::from_file(file)?),
        FileFormat::Aiff => Box::new(aiff::AiffReader::from_file(file)?),
        FileFormat::Flac => Box::new(flac::FlacReader::from_file(file)?),
        FileFormat::Mp3 => mp3::Mp3Reader::from_file(file)?,
    })
}

/// The header of any file of the four formats (for the browser).
pub fn file_info(path: &Path) -> io::Result<AudioInfo> {
    open_reader(path).map(|r| *r.info())
}

/// Random access to a file's bytes through a window (FLAC and MP3 readers).
pub(crate) struct FileWindow {
    file: File,
    pub len: u64,
    start: u64,
    buf: Vec<u8>,
}

impl FileWindow {
    pub fn new(file: File) -> io::Result<Self> {
        let len = file.metadata()?.len();
        Ok(Self { file, len, start: 0, buf: Vec::new() })
    }

    /// Up to `len` bytes from `offset` (fewer at the end of the file). Reads
    /// at least `min_read` bytes when it has to go to the file.
    pub fn get(&mut self, offset: u64, len: usize, min_read: usize) -> io::Result<&[u8]> {
        let end = (offset + len as u64).min(self.len);
        if offset >= self.len {
            return Ok(&[]);
        }
        let have = offset >= self.start && end <= self.start + self.buf.len() as u64;
        if !have {
            let want = (len.max(min_read) as u64).min(self.len - offset) as usize;
            self.buf.resize(want, 0);
            self.file.seek(SeekFrom::Start(offset))?;
            self.file.read_exact(&mut self.buf)?;
            self.start = offset;
        }
        let a = (offset - self.start) as usize;
        let b = (end - self.start) as usize;
        Ok(&self.buf[a..b])
    }
}

/// Converts `x` to an integer sample of `bits` bits (rounded to the nearest
/// step, clipped; NaN is 0), the inverse of [`from_int`].
#[inline]
pub(crate) fn to_int(x: f32, bits: u32) -> i32 {
    let scale = (1i64 << (bits - 1)) as f64;
    let v = (x as f64 * scale).round();
    if v.is_nan() {
        0
    } else {
        v.clamp(-scale, scale - 1.0) as i32
    }
}

#[inline]
pub(crate) fn from_int(v: i32, bits: u32) -> f32 {
    (v as f64 / (1i64 << (bits - 1)) as f64) as f32
}

/// Copies `count` frames of interleaved `decoded` (from frame `src`) into
/// `out` at `dst` (the shared tail of the PCM readers).
pub(crate) fn fill_out(out: &mut [&mut [f32]], dst: usize, channels: usize, count: usize, get: impl Fn(usize, usize) -> f32) {
    for (c, o) in out.iter_mut().enumerate() {
        let ch = c.min(channels - 1);
        for (i, x) in o[dst..dst + count].iter_mut().enumerate() {
            *x = get(i, ch);
        }
    }
}

#[cfg(test)]
pub(crate) mod test_util {
    use std::path::Path;
    use std::process::Command;

    /// Whether `ffmpeg` can be run (external decoder checks are skipped otherwise).
    pub fn have_ffmpeg() -> bool {
        Command::new("ffmpeg").arg("-version").output().map(|o| o.status.success()).unwrap_or(false)
    }

    /// Decodes `path` with ffmpeg to interleaved f32 (None without ffmpeg or on error).
    pub fn ffmpeg_decode(path: &Path, extra: &[&str]) -> Option<(Vec<f32>, String)> {
        let out = Command::new("ffmpeg")
            .args(["-v", "error", "-nostdin", "-i"])
            .arg(path)
            .args(extra)
            .args(["-f", "f32le", "-"])
            .output()
            .ok()?;
        if !out.status.success() {
            return None;
        }
        let samples = out.stdout.chunks_exact(4).map(|b| f32::from_le_bytes([b[0], b[1], b[2], b[3]])).collect();
        Some((samples, String::from_utf8_lossy(&out.stderr).into_owned()))
    }

    /// Raw integer PCM from ffmpeg (`fmt` "s16le" / "s24le" / "s32le").
    pub fn ffmpeg_decode_raw(path: &Path, fmt: &str) -> Option<(Vec<u8>, String)> {
        let out =
            Command::new("ffmpeg").args(["-v", "error", "-nostdin", "-i"]).arg(path).args(["-f", fmt, "-"]).output().ok()?;
        if !out.status.success() {
            return None;
        }
        Some((out.stdout, String::from_utf8_lossy(&out.stderr).into_owned()))
    }

    /// Encodes interleaved 16-bit `pcm` with ffmpeg (`args` select the codec) into `dest`.
    pub fn ffmpeg_encode(pcm: &[i16], channels: usize, rate: u32, args: &[&str], dest: &Path) -> bool {
        let raw = dest.with_extension("raw");
        let bytes: Vec<u8> = pcm.iter().flat_map(|s| s.to_le_bytes()).collect();
        if std::fs::write(&raw, bytes).is_err() {
            return false;
        }
        let ok = Command::new("ffmpeg")
            .args(["-v", "error", "-nostdin", "-y", "-f", "s16le", "-ar", &rate.to_string(), "-ac", &channels.to_string(), "-i"])
            .arg(&raw)
            .args(args)
            .arg(dest)
            .status()
            .map(|s| s.success())
            .unwrap_or(false);
        let _ = std::fs::remove_file(raw);
        ok
    }

    /// A deterministic pseudo-random generator for test signals.
    pub struct Lcg(pub u64);

    impl Lcg {
        pub fn next_u32(&mut self) -> u32 {
            self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
            (self.0 >> 33) as u32
        }

        /// Uniform in [-1, 1).
        pub fn next_f32(&mut self) -> f32 {
            (self.next_u32() as f64 / (1u64 << 31) as f64 * 2.0 - 1.0) as f32
        }
    }
}
