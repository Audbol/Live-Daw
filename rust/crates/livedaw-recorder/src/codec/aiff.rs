//! AIFF files (Audio IFF 1.3, and AIFF-C for 32-bit float): a writer for
//! big-endian 16/24-bit PCM (`AIFF`) or 32-bit float (`AIFC` / `fl32`), and a
//! reader for 8-32-bit integer (`NONE`, `twos`, little-endian `sowt`) and
//! 32/64-bit float (`fl32`, `fl64`) files.
//!
//! As with WAV, the header (`FORM` size, `COMM` frames, `SSND` size) is
//! rewritten every few seconds while recording, and the reader takes a size
//! of 0 or one past the end of the file as "until the end", so a file cut
//! short by a crash still reads. AIFF has 32-bit sizes: past 4 GB the sizes
//! stay at their maximum and the reader reads to the end of the file.

use super::{fill_out, from_int, to_int, AudioInfo, AudioReader, AudioWriter, FileFormat};
use crate::wav::SampleFormat;
use std::fs::File;
use std::io::{self, BufWriter, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

const FLOAT_NAME: &[u8] = b"32-bit floating point";

/// An 80-bit IEEE 754 extended value (the `COMM` sample rate).
fn extended(v: f64) -> [u8; 10] {
    let mut out = [0u8; 10];
    if v <= 0.0 || !v.is_finite() {
        return out;
    }
    let exp = v.log2().floor() as i32;
    let mantissa = (v / 2f64.powi(exp - 63)).round() as u64;
    out[..2].copy_from_slice(&((exp + 16383) as u16).to_be_bytes());
    out[2..].copy_from_slice(&mantissa.to_be_bytes());
    out
}

fn from_extended(b: &[u8]) -> f64 {
    let exp = i32::from(u16::from_be_bytes([b[0], b[1]]) & 0x7fff);
    let mantissa = u64::from_be_bytes(b[2..10].try_into().unwrap_or([0; 8]));
    if exp == 0 && mantissa == 0 {
        return 0.0;
    }
    let v = mantissa as f64 * 2f64.powi(exp - 16383 - 63);
    if b[0] & 0x80 != 0 {
        -v
    } else {
        v
    }
}

/// Writes AIFF (integer) or AIFF-C (float) files.
pub struct AiffWriter {
    out: BufWriter<File>,
    path: PathBuf,
    format: SampleFormat,
    channels: usize,
    sample_rate: u32,
    frames: u64,
    padded: bool,
    bytes: Vec<u8>,
}

impl AiffWriter {
    /// Creates (or truncates) `path`: `Int16` / `Int24` as AIFF, `Float32` as
    /// AIFF-C (`Int32` is written as float too).
    pub fn create(path: &Path, channels: usize, sample_rate: u32, format: SampleFormat) -> io::Result<Self> {
        let format = if format == SampleFormat::Int32 { SampleFormat::Float32 } else { format };
        let file = File::create(path)?;
        let mut w = Self {
            out: BufWriter::with_capacity(1 << 18, file),
            path: path.to_owned(),
            format,
            channels: channels.max(1),
            sample_rate,
            frames: 0,
            padded: false,
            bytes: Vec::new(),
        };
        let header = w.header();
        w.out.write_all(&header)?;
        w.out.flush()?;
        Ok(w)
    }

    fn float(&self) -> bool {
        self.format == SampleFormat::Float32
    }

    fn frame_bytes(&self) -> usize {
        self.channels * self.format.bytes()
    }

    fn data_bytes(&self) -> u64 {
        self.frames * self.frame_bytes() as u64
    }

    fn header(&self) -> Vec<u8> {
        let clip = |v: u64| v.min(u32::MAX as u64) as u32;
        let mut comm = Vec::new();
        comm.extend_from_slice(&(self.channels as u16).to_be_bytes());
        comm.extend_from_slice(&clip(self.frames).to_be_bytes());
        comm.extend_from_slice(&self.format.bits().to_be_bytes());
        comm.extend_from_slice(&extended(f64::from(self.sample_rate)));
        if self.float() {
            comm.extend_from_slice(b"fl32");
            comm.push(FLOAT_NAME.len() as u8);
            comm.extend_from_slice(FLOAT_NAME);
            if comm.len() % 2 == 1 {
                comm.push(0);
            }
        }
        let mut h = Vec::with_capacity(96);
        h.extend_from_slice(b"FORM");
        h.extend_from_slice(&[0; 4]); // patched below
        h.extend_from_slice(if self.float() { b"AIFC" } else { b"AIFF" });
        if self.float() {
            h.extend_from_slice(b"FVER");
            h.extend_from_slice(&4u32.to_be_bytes());
            h.extend_from_slice(&0xA280_5140u32.to_be_bytes()); // AIFF-C version 1
        }
        h.extend_from_slice(b"COMM");
        h.extend_from_slice(&(comm.len() as u32).to_be_bytes());
        h.extend_from_slice(&comm);
        h.extend_from_slice(b"SSND");
        h.extend_from_slice(&clip(8 + self.data_bytes()).to_be_bytes());
        h.extend_from_slice(&[0; 8]); // offset, block size
        let form = h.len() as u64 - 8 + self.data_bytes() + u64::from(self.padded);
        h[4..8].copy_from_slice(&clip(form).to_be_bytes());
        h
    }
}

impl AudioWriter for AiffWriter {
    fn path(&self) -> &Path {
        &self.path
    }

    fn channels(&self) -> usize {
        self.channels
    }

    fn frames(&self) -> u64 {
        self.frames
    }

    fn write_planar(&mut self, channels: &[&[f32]], n: usize) -> io::Result<()> {
        let nc = self.channels;
        self.bytes.clear();
        self.bytes.reserve(n * self.frame_bytes());
        for i in 0..n {
            for c in 0..nc {
                let x = channels.get(c).and_then(|ch| ch.get(i)).copied().unwrap_or(0.0);
                match self.format {
                    SampleFormat::Int16 => self.bytes.extend_from_slice(&(to_int(x, 16) as i16).to_be_bytes()),
                    SampleFormat::Int24 => self.bytes.extend_from_slice(&to_int(x, 24).to_be_bytes()[1..]),
                    _ => self.bytes.extend_from_slice(&x.to_be_bytes()),
                }
            }
        }
        self.out.write_all(&self.bytes)?;
        self.frames += n as u64;
        Ok(())
    }

    fn write_silence(&mut self, n: usize) -> io::Result<()> {
        let zeros = [0u8; 4096];
        let mut left = n * self.frame_bytes();
        while left > 0 {
            let k = left.min(zeros.len());
            self.out.write_all(&zeros[..k])?;
            left -= k;
        }
        self.frames += n as u64;
        Ok(())
    }

    fn update_header(&mut self) -> io::Result<()> {
        self.out.flush()?;
        let header = self.header();
        let file = self.out.get_mut();
        file.seek(SeekFrom::Start(0))?;
        file.write_all(&header)?;
        file.seek(SeekFrom::End(0))?;
        Ok(())
    }

    fn finalize(mut self: Box<Self>) -> io::Result<u64> {
        if self.data_bytes() % 2 == 1 {
            self.out.write_all(&[0])?;
            self.padded = true;
        }
        self.update_header()?;
        self.out.get_mut().sync_data()?;
        Ok(self.frames)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Encoding {
    /// Big-endian signed integer of `bytes` bytes.
    IntBe(usize),
    /// Little-endian (`sowt`).
    IntLe(usize),
    F32,
    F64,
}

impl Encoding {
    fn bytes(self) -> usize {
        match self {
            Self::IntBe(b) | Self::IntLe(b) => b,
            Self::F32 => 4,
            Self::F64 => 8,
        }
    }
}

/// Random access to the frames of an AIFF / AIFF-C file.
pub struct AiffReader {
    file: File,
    info: AudioInfo,
    encoding: Encoding,
    data_offset: u64,
    bytes: Vec<u8>,
}

impl AiffReader {
    pub fn open(path: &Path) -> io::Result<Self> {
        Self::from_file(File::open(path)?)
    }

    pub fn from_file(mut file: File) -> io::Result<Self> {
        let invalid = |m: &str| io::Error::new(io::ErrorKind::InvalidData, m.to_owned());
        let file_len = file.metadata()?.len();
        file.seek(SeekFrom::Start(0))?;
        let mut head = [0u8; 12];
        file.read_exact(&mut head)?;
        if &head[..4] != b"FORM" || !(&head[8..12] == b"AIFF" || &head[8..12] == b"AIFC") {
            return Err(invalid("not an AIFF file"));
        }
        let aifc = &head[8..12] == b"AIFC";
        let mut comm: Option<(usize, u64, u16, f64, [u8; 4])> = None;
        let mut pos = 12u64;
        while pos + 8 <= file_len {
            file.seek(SeekFrom::Start(pos))?;
            let mut ch = [0u8; 8];
            file.read_exact(&mut ch)?;
            let size = u64::from(u32::from_be_bytes([ch[4], ch[5], ch[6], ch[7]]));
            let body = pos + 8;
            match &ch[..4] {
                b"COMM" => {
                    let mut b = [0u8; 22];
                    let n = size.min(22) as usize;
                    file.read_exact(&mut b[..n])?;
                    let channels = u16::from_be_bytes([b[0], b[1]]) as usize;
                    let frames = u64::from(u32::from_be_bytes([b[2], b[3], b[4], b[5]]));
                    let bits = u16::from_be_bytes([b[6], b[7]]);
                    let rate = from_extended(&b[8..18]);
                    let compression = if aifc && n >= 22 { [b[18], b[19], b[20], b[21]] } else { *b"NONE" };
                    comm = Some((channels, frames, bits, rate, compression));
                }
                b"SSND" => {
                    let (channels, comm_frames, bits, rate, compression) = comm.ok_or_else(|| invalid("SSND before COMM"))?;
                    if channels == 0 {
                        return Err(invalid("no channels"));
                    }
                    let bytes = usize::from(bits.div_ceil(8));
                    let encoding = match &compression {
                        b"NONE" | b"twos" if (1..=4).contains(&bytes) => Encoding::IntBe(bytes),
                        b"sowt" if (1..=4).contains(&bytes) => Encoding::IntLe(bytes),
                        b"fl32" | b"FL32" => Encoding::F32,
                        b"fl64" | b"FL64" => Encoding::F64,
                        _ => {
                            return Err(io::Error::new(
                                io::ErrorKind::Unsupported,
                                format!("AIFF compression {}", String::from_utf8_lossy(&compression)),
                            ))
                        }
                    };
                    let mut off = [0u8; 4];
                    file.read_exact(&mut off)?;
                    let data_offset = body + 8 + u64::from(u32::from_be_bytes(off));
                    let available = file_len.saturating_sub(data_offset);
                    let declared = size.saturating_sub(8 + u64::from(u32::from_be_bytes(off)));
                    let data =
                        if declared == 0 || size >= u32::MAX as u64 || declared > available { available } else { declared };
                    let frame_bytes = (channels * encoding.bytes()) as u64;
                    let data_frames = data / frame_bytes;
                    let frames = if comm_frames == 0 || comm_frames == u32::MAX as u64 {
                        data_frames
                    } else {
                        comm_frames.min(data_frames)
                    };
                    let float = matches!(encoding, Encoding::F32 | Encoding::F64);
                    let info = AudioInfo {
                        format: FileFormat::Aiff,
                        channels,
                        sample_rate: rate.round() as u32,
                        frames,
                        bits: if float { encoding.bytes() as u16 * 8 } else { bits },
                        float,
                        kbps: 0,
                    };
                    return Ok(Self { file, info, encoding, data_offset, bytes: Vec::new() });
                }
                _ => {}
            }
            pos = body + size + (size & 1);
        }
        Err(invalid("no SSND chunk"))
    }
}

impl AudioReader for AiffReader {
    fn info(&self) -> &AudioInfo {
        &self.info
    }

    fn read(&mut self, frame: i64, out: &mut [&mut [f32]]) -> io::Result<()> {
        let Some(n) = out.first().map(|o| o.len()) else { return Ok(()) };
        for o in out.iter_mut() {
            o.fill(0.0);
        }
        let first = frame.max(0);
        let last = (frame + n as i64).min(self.info.frames as i64);
        if last <= first {
            return Ok(());
        }
        let count = (last - first) as usize;
        let skip = (first - frame) as usize;
        let channels = self.info.channels;
        let width = self.encoding.bytes();
        let frame_bytes = channels * width;
        self.bytes.resize(count * frame_bytes, 0);
        self.file.seek(SeekFrom::Start(self.data_offset + first as u64 * frame_bytes as u64))?;
        self.file.read_exact(&mut self.bytes)?;
        let enc = self.encoding;
        let bytes = &self.bytes;
        fill_out(out, skip, channels, count, |i, ch| {
            let at = i * frame_bytes + ch * width;
            let b = &bytes[at..at + width];
            match enc {
                Encoding::IntBe(w) => {
                    let mut v = [0u8; 4];
                    v[..w].copy_from_slice(b);
                    from_int(i32::from_be_bytes(v) >> (32 - 8 * w), 8 * w as u32)
                }
                Encoding::IntLe(w) => {
                    let mut v = [0u8; 4];
                    v[4 - w..].copy_from_slice(b);
                    from_int(i32::from_le_bytes(v) >> (32 - 8 * w), 8 * w as u32)
                }
                Encoding::F32 => f32::from_be_bytes([b[0], b[1], b[2], b[3]]),
                Encoding::F64 => f64::from_be_bytes(b.try_into().unwrap_or([0; 8])) as f32,
            }
        });
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::codec::test_util::*;
    use crate::codec::{create_writer, open_reader, EncodeFormat};
    use crate::wav::tests::temp_dir;

    fn grid(channels: usize, frames: usize, bits: u32) -> Vec<Vec<f32>> {
        let mut rng = Lcg(bits as u64 * 7 + channels as u64);
        let scale = (1i64 << (bits - 1)) as f64;
        (0..channels)
            .map(|c| {
                (0..frames)
                    .map(|i| {
                        let v = ((i as f64 * 0.01 * (c + 1) as f64).sin() * 0.7 + rng.next_f32() as f64 * 0.2) * scale;
                        (v.round() / scale) as f32
                    })
                    .collect()
            })
            .collect()
    }

    #[test]
    fn extended_round_trips() {
        for rate in [8000.0, 44100.0, 48000.0, 96000.0, 192000.0, 22050.0] {
            assert_eq!(from_extended(&extended(rate)), rate);
        }
        assert_eq!(extended(44100.0), [0x40, 0x0e, 0xac, 0x44, 0, 0, 0, 0, 0, 0]);
    }

    #[test]
    fn pcm_and_float_round_trip_bit_exact() {
        let dir = temp_dir("aiff-rt");
        for (sample, bits) in [(SampleFormat::Int16, 16), (SampleFormat::Int24, 24), (SampleFormat::Float32, 32)] {
            for channels in [1usize, 2, 4] {
                let frames = 3001; // odd: the pad byte for mono 24-bit
                let mut chans = grid(channels, frames, bits.min(24));
                chans[0][0] = -1.0;
                if sample == SampleFormat::Float32 {
                    chans[0][1] = 1.5; // float keeps overs
                }
                let path = dir.join(format!("t-{bits}-{channels}.aif"));
                let mut w = create_writer(&path, channels, 44100, EncodeFormat::new(FileFormat::Aiff, sample, 0)).unwrap();
                let refs: Vec<&[f32]> = chans.iter().map(|c| &c[..1000]).collect();
                w.write_planar(&refs, 1000).unwrap();
                w.update_header().unwrap();
                w.write_silence(1).unwrap();
                let refs: Vec<&[f32]> = chans.iter().map(|c| &c[1001..]).collect();
                w.write_planar(&refs, frames - 1001).unwrap();
                assert_eq!(w.finalize().unwrap(), frames as u64);
                for c in &mut chans {
                    c[1000] = 0.0;
                }
                let bytes = std::fs::read(&path).unwrap();
                assert_eq!(u32::from_be_bytes(bytes[4..8].try_into().unwrap()) as usize, bytes.len() - 8, "FORM size");
                let mut r = open_reader(&path).unwrap();
                let info = *r.info();
                assert_eq!(
                    (info.format, info.channels, info.sample_rate, info.frames),
                    (FileFormat::Aiff, channels, 44100, frames as u64)
                );
                assert_eq!(info.float, sample == SampleFormat::Float32);
                let mut back = vec![vec![0.0f32; frames]; channels];
                let mut refs: Vec<&mut [f32]> = back.iter_mut().map(|c| &mut c[..]).collect();
                r.read(0, &mut refs).unwrap();
                assert_eq!(back, chans, "{sample:?} x{channels}");
                // Random access across the start.
                let mut a = [9.0f32; 4];
                r.read(-2, &mut [&mut a[..]]).unwrap();
                assert_eq!(a, [0.0, 0.0, chans[0][0], chans[0][1]]);

                if have_ffmpeg() && sample != SampleFormat::Float32 {
                    let (pcm, _) = ffmpeg_decode(&path, &[]).expect("ffmpeg reads the AIFF");
                    assert_eq!(pcm.len(), frames * channels);
                    for (i, x) in pcm.iter().enumerate() {
                        assert_eq!(*x, chans[i % channels][i / channels], "ffmpeg sample {i}");
                    }
                }
            }
        }
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn a_crash_before_the_header_rewrite_still_reads() {
        let dir = temp_dir("aiff-crash");
        let path = dir.join("crash.aif");
        let mut w = AiffWriter::create(&path, 2, 48000, SampleFormat::Int24).unwrap();
        w.write_planar(&[&[0.5; 64], &[-0.25; 64]], 64).unwrap();
        w.out.flush().unwrap();
        std::mem::forget(w);
        let mut r = AiffReader::open(&path).unwrap();
        assert_eq!(r.info().frames, 64);
        let mut a = [0.0f32; 64];
        let mut b = [0.0f32; 64];
        r.read(0, &mut [&mut a[..], &mut b[..]]).unwrap();
        assert!(a.iter().all(|&x| x == 0.5) && b.iter().all(|&x| x == -0.25));
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn reads_files_other_software_writes() {
        if !have_ffmpeg() {
            return;
        }
        let dir = temp_dir("aiff-ext");
        let pcm: Vec<i16> = (0..2000).map(|i| ((i as f64 * 0.05).sin() * 20000.0) as i16).collect();
        for (args, name) in [(&["-c:a", "pcm_s16be"][..], "be.aiff"), (&["-c:a", "pcm_s24be"][..], "be24.aiff")] {
            let path = dir.join(name);
            assert!(ffmpeg_encode(&pcm, 2, 44100, args, &path));
            let mut r = open_reader(&path).unwrap();
            assert_eq!((r.info().channels, r.info().frames), (2, 1000));
            let mut l = vec![0.0f32; 1000];
            let mut rr = vec![0.0f32; 1000];
            r.read(0, &mut [&mut l[..], &mut rr[..]]).unwrap();
            for i in 0..1000 {
                assert_eq!(l[i], pcm[2 * i] as f32 / 32768.0);
                assert_eq!(rr[i], pcm[2 * i + 1] as f32 / 32768.0);
            }
        }
        let _ = std::fs::remove_dir_all(dir);
    }
}
