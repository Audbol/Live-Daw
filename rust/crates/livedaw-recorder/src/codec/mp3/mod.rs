//! MP3 (MPEG-1 Layer III) files: [`Mp3Writer`] (CBR 128-320 kbit/s, 32 /
//! 44.1 / 48 kHz, mono or joint stereo) and [`Mp3Reader`] (any MPEG-1
//! Layer III stream, with random access).
//!
//! The writer starts the file with an `Info` frame (a silent frame in the
//! Xing / LAME tag layout) and patches it when the file is finalized: the
//! frame count, the byte count and the encoder delay and padding, so readers
//! that honour it (this one) return exactly the samples that were written.
//! Every other frame is self-contained (no bit reservoir), so a file cut
//! short by a crash plays up to its last complete frame.
//!
//! The reader indexes frame headers lazily (an `Info` / `Xing` frame gives
//! the length up front; otherwise the headers are scanned once). A jump
//! restarts the decoder a few frames before the target, which rebuilds the
//! bit reservoir and the filterbank state, so random access returns the
//! same samples as decoding from the start.

mod dec;
mod enc;
mod huffman_data;
pub(crate) mod tables;

pub use dec::{Decoder, FrameError, Header};
pub use enc::Encoder;

use super::{AudioInfo, AudioReader, AudioWriter, FileFormat, FileWindow};
use std::fs::File;
use std::io::{self, BufWriter, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

/// Sample rates the encoder supports.
pub const SAMPLE_RATES: [u32; 3] = [32000, 44100, 48000];
/// Samples per frame.
pub const FRAME_SAMPLES: usize = 1152;
/// Frames decoded (and discarded) before a jump target.
const PRIME_FRAMES: u64 = 10;

/// Writes an MP3 file.
pub struct Mp3Writer {
    out: BufWriter<File>,
    path: PathBuf,
    enc: Encoder,
    channels: usize,
    pending: Vec<Vec<f32>>,
    /// Input frames (samples per channel) accepted.
    frames: u64,
    /// Audio frames written (the Info frame not counted).
    mp3_frames: u64,
    bytes: u64,
    info_len: usize,
}

impl Mp3Writer {
    pub fn create(path: &Path, channels: usize, sample_rate: u32, kbps: u32) -> io::Result<Box<dyn AudioWriter>> {
        Ok(Box::new(Self::new(path, channels, sample_rate, kbps)?))
    }

    pub fn new(path: &Path, channels: usize, sample_rate: u32, kbps: u32) -> io::Result<Self> {
        let enc = Encoder::new(channels, sample_rate, kbps).ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::Unsupported,
                format!("MP3 can't encode {channels} ch at {sample_rate} Hz, {kbps} kbit/s"),
            )
        })?;
        let file = File::create(path)?;
        let info = enc.info_frame(0, 0, Self::enc_delay(), 0);
        let mut w = Self {
            out: BufWriter::with_capacity(1 << 16, file),
            path: path.to_owned(),
            channels,
            pending: vec![Vec::with_capacity(FRAME_SAMPLES); channels],
            frames: 0,
            mp3_frames: 0,
            bytes: info.len() as u64,
            info_len: info.len(),
            enc,
        };
        w.out.write_all(&info)?;
        w.out.flush()?;
        Ok(w)
    }

    /// The encoder delay as the LAME tag counts it (without the decoder's 529).
    fn enc_delay() -> u32 {
        (enc::TOTAL_DELAY - enc::DECODER_DELAY) as u32
    }

    fn encode_pending(&mut self) -> io::Result<()> {
        for p in &mut self.pending {
            p.resize(FRAME_SAMPLES, 0.0);
        }
        let refs: Vec<&[f32]> = self.pending.iter().map(|p| &p[..]).collect();
        let frame = self.enc.encode(&refs);
        self.out.write_all(frame)?;
        self.bytes += frame.len() as u64;
        self.mp3_frames += 1;
        for p in &mut self.pending {
            p.clear();
        }
        Ok(())
    }
}

impl AudioWriter for Mp3Writer {
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
        let mut done = 0;
        while done < n {
            let k = (FRAME_SAMPLES - self.pending[0].len()).min(n - done);
            for (c, p) in self.pending.iter_mut().enumerate() {
                match channels.get(c) {
                    Some(src) => p.extend((done..done + k).map(|i| src.get(i).copied().unwrap_or(0.0))),
                    None => p.resize(p.len() + k, 0.0),
                }
            }
            done += k;
            self.frames += k as u64;
            if self.pending[0].len() == FRAME_SAMPLES {
                self.encode_pending()?;
            }
        }
        Ok(())
    }

    fn write_silence(&mut self, n: usize) -> io::Result<()> {
        let zeros = [0.0f32; FRAME_SAMPLES];
        let mut left = n;
        while left > 0 {
            let k = left.min(FRAME_SAMPLES);
            let refs: Vec<&[f32]> = (0..self.channels).map(|_| &zeros[..k]).collect();
            self.write_planar(&refs, k)?;
            left -= k;
        }
        Ok(())
    }

    fn update_header(&mut self) -> io::Result<()> {
        // Frames are complete as written; the Info frame is patched at the end.
        self.out.flush()
    }

    fn finalize(mut self: Box<Self>) -> io::Result<u64> {
        // Flush the filterbanks: every input sample reaches the output.
        let needed = self.frames + enc::TOTAL_DELAY as u64;
        while self.mp3_frames * (FRAME_SAMPLES as u64) < needed {
            self.encode_pending()?;
        }
        self.out.flush()?;
        let delay = Self::enc_delay();
        let padding = (self.mp3_frames * FRAME_SAMPLES as u64 - self.frames - u64::from(delay)) as u32;
        let info = self.enc.info_frame(self.mp3_frames as u32, self.bytes.min(u32::MAX as u64) as u32, delay, padding);
        debug_assert_eq!(info.len(), self.info_len);
        let file = self.out.get_mut();
        file.seek(SeekFrom::Start(0))?;
        file.write_all(&info)?;
        file.sync_data()?;
        Ok(self.frames)
    }
}

/// What an `Info` / `Xing` frame says.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct XingInfo {
    frames: Option<u32>,
    /// Encoder delay and padding (LAME tag), if present.
    gapless: Option<(u32, u32)>,
}

fn parse_xing(frame: &[u8], h: &Header) -> Option<XingInfo> {
    let at = 4 + if h.crc { 2 } else { 0 } + h.side_info_len();
    let tag = frame.get(at..at + 8)?;
    if &tag[..4] != b"Xing" && &tag[..4] != b"Info" {
        return None;
    }
    let flags = u32::from_be_bytes([tag[4], tag[5], tag[6], tag[7]]);
    let mut p = at + 8;
    let mut info = XingInfo::default();
    if flags & 1 != 0 {
        info.frames = frame.get(p..p + 4).map(|b| u32::from_be_bytes([b[0], b[1], b[2], b[3]]));
        p += 4;
    }
    if flags & 2 != 0 {
        p += 4;
    }
    if flags & 4 != 0 {
        p += 100;
    }
    if flags & 8 != 0 {
        p += 4;
    }
    // The LAME extension: a 9-character encoder name, then delay / padding at +21.
    if let Some(ext) = frame.get(p..p + 24) {
        if ext[..4].iter().all(|c| c.is_ascii_alphanumeric()) {
            let v = u32::from_be_bytes([0, ext[21], ext[22], ext[23]]);
            info.gapless = Some((v >> 12, v & 0xfff));
        }
    }
    Some(info)
}

/// Random access to an MP3 file.
pub struct Mp3Reader {
    win: FileWindow,
    info: AudioInfo,
    first: Header,
    /// Offsets of the audio frames found so far.
    offsets: Vec<u64>,
    scanned_all: bool,
    /// Decoded samples skipped at the start (gapless).
    skip: u64,
    dec: Decoder,
    pcm: Box<[[f32; 1152]; 2]>,
    pcm_frame: Option<u64>,
    /// The frame the decoder state continues with.
    next_frame: u64,
}

impl Mp3Reader {
    pub fn open(path: &Path) -> io::Result<Self> {
        Self::new(File::open(path)?)
    }

    pub fn from_file(file: File) -> io::Result<Box<dyn AudioReader>> {
        Ok(Box::new(Self::new(file)?))
    }

    pub fn new(file: File) -> io::Result<Self> {
        let invalid = |m: &str| io::Error::new(io::ErrorKind::InvalidData, m.to_owned());
        let mut win = FileWindow::new(file)?;
        let head = win.get(0, 10, 1 << 16)?.to_vec();
        let start = super::id3v2_size(&head);
        // The first header followed by another one (or the end of the file).
        let mut at = start;
        let limit = (start + (1 << 20)).min(win.len);
        let (first_off, first) = loop {
            if at + 4 > limit {
                return Err(invalid("no MPEG-1 Layer III frame"));
            }
            let b = win.get(at, 4, 1 << 16)?;
            if let Some(h) = Header::parse(b) {
                let next = at + h.frame_len() as u64;
                let ok = next + 4 > win.len || Header::parse(win.get(next, 4, 1 << 16)?).is_some_and(|n| h.compatible(&n));
                if ok {
                    break (at, h);
                }
            }
            at += 1;
        };
        let frame = win.get(first_off, first.frame_len(), 1 << 16)?.to_vec();
        let xing = parse_xing(&frame, &first);
        let mut r = Self {
            win,
            info: AudioInfo {
                format: FileFormat::Mp3,
                channels: first.channels(),
                sample_rate: first.sample_rate(),
                frames: 0,
                bits: 16,
                float: false,
                kbps: first.kbps(),
            },
            first,
            offsets: Vec::new(),
            scanned_all: false,
            skip: 0,
            dec: Decoder::new(),
            pcm: Box::new([[0.0; 1152]; 2]),
            pcm_frame: None,
            next_frame: 0,
        };
        let audio_start = if xing.is_some() { first_off + first.frame_len() as u64 } else { first_off };
        r.offsets.push(audio_start);
        if r.header_at(audio_start)?.is_none() {
            r.offsets.clear();
            r.scanned_all = true;
        }
        let frames = match xing.and_then(|x| x.frames).filter(|&f| f > 0) {
            Some(f) => u64::from(f),
            None => {
                r.index_to(u64::MAX)?;
                r.offsets.len() as u64
            }
        };
        let total = frames * FRAME_SAMPLES as u64;
        // The decoder's own delay is skipped in any case (as ffmpeg does);
        // the LAME tag adds the encoder delay and the padding at the end.
        let (skip, trim) = match xing.and_then(|x| x.gapless) {
            Some((delay, padding)) => {
                let skip = u64::from(delay) + enc::DECODER_DELAY as u64;
                let trim = u64::from(padding).saturating_sub(enc::DECODER_DELAY as u64);
                (skip, trim)
            }
            None => (enc::DECODER_DELAY as u64, 0),
        };
        r.skip = skip;
        r.info.frames = total.saturating_sub(skip + trim);
        Ok(r)
    }

    fn header_at(&mut self, off: u64) -> io::Result<Option<Header>> {
        let b = self.win.get(off, 4, 1 << 16)?;
        Ok(Header::parse(b).filter(|h| h.compatible(&self.first) && off + h.frame_len() as u64 <= self.win.len))
    }

    /// Extends the frame index to hold frame `f` (or the end of the file).
    fn index_to(&mut self, f: u64) -> io::Result<()> {
        while !self.scanned_all && (self.offsets.len() as u64) <= f {
            let Some(&last) = self.offsets.last() else {
                self.scanned_all = true;
                break;
            };
            let Some(h) = self.header_at(last)? else {
                self.offsets.pop();
                self.scanned_all = true;
                break;
            };
            let mut next = last + h.frame_len() as u64;
            if self.header_at(next)?.is_none() {
                // Junk between frames (or a tag at the end): look a little further.
                let mut found = None;
                let end = (next + 4096).min(self.win.len);
                let mut at = next;
                while at + 4 <= end {
                    if let Some(h2) = self.header_at(at)? {
                        let after = at + h2.frame_len() as u64;
                        if after + 4 > self.win.len || self.header_at(after)?.is_some() {
                            found = Some(at);
                            break;
                        }
                    }
                    at += 1;
                }
                match found {
                    Some(a) => next = a,
                    None => {
                        self.scanned_all = true;
                        break;
                    }
                }
            }
            self.offsets.push(next);
        }
        Ok(())
    }

    fn decode_frame(&mut self, f: u64) -> io::Result<bool> {
        self.index_to(f)?;
        let Some(&off) = self.offsets.get(f as usize) else { return Ok(false) };
        let bytes = self.win.get(off, 1441 + 4, 1 << 18)?;
        let _ = self.dec.decode(bytes, &mut self.pcm);
        self.pcm_frame = Some(f);
        self.next_frame = f + 1;
        Ok(true)
    }

    /// Makes `pcm` hold frame `f`. False past the end.
    fn ensure(&mut self, f: u64) -> io::Result<bool> {
        if self.pcm_frame == Some(f) {
            return Ok(true);
        }
        if self.next_frame != f || self.pcm_frame.is_none() && f > 0 {
            self.dec.reset();
            let start = f.saturating_sub(PRIME_FRAMES);
            for g in start..f {
                if !self.decode_frame(g)? {
                    return Ok(false);
                }
            }
        }
        self.decode_frame(f)
    }
}

impl AudioReader for Mp3Reader {
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
        let channels = self.info.channels;
        let mut pos = first;
        while pos < last {
            let d = pos as u64 + self.skip;
            let f = d / FRAME_SAMPLES as u64;
            let off = (d % FRAME_SAMPLES as u64) as usize;
            if !self.ensure(f)? {
                break;
            }
            let count = (FRAME_SAMPLES - off).min((last - pos) as usize);
            let pcm = &self.pcm;
            super::fill_out(out, (pos - frame) as usize, channels, count, |i, ch| pcm[ch][off + i]);
            pos += count as i64;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests;
