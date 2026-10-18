//! `ld_afw_*` / `ld_afr_*`: the recorders' audio file writers and readers
//! (WAV, FLAC, AIFF, MP3) for the C++ side: the DAW's export and bounce,
//! and reading takes the way the recorder plays them (MP3 gapless).
//! C++ view: `livedaw::AudioFileWriter` / `AudioFileReader` and the
//! `livedaw::daw` render functions.
//!
//! Formats are the `FileFormat` codes: 0 WAV, 1 FLAC, 2 AIFF, 3 MP3; `bits`
//! 16, 24 or 32 (32 = float, WAV and AIFF); `kbps` the MP3 bit rate.

use crate::guard;
use crate::session::{str_arg, LdString};
use livedaw_engine::recorder::codec::{self, AudioReader, AudioWriter, EncodeFormat, FileFormat};
use livedaw_engine::recorder::wav::SampleFormat;
use std::path::PathBuf;

/// An open file being written (opaque to C++).
pub struct LdAudioWriter(Box<dyn AudioWriter>);
/// An open file being read (opaque to C++).
pub struct LdAudioReader(Box<dyn AudioReader>);

/// The encoder settings from C's scalars.
pub fn encode_format(format: i32, bits: i32, kbps: i32) -> EncodeFormat {
    let sample = match bits {
        ..=16 => SampleFormat::Int16,
        17..=24 => SampleFormat::Int24,
        _ => SampleFormat::Float32,
    };
    EncodeFormat::new(FileFormat::from_i32(format), sample, kbps.max(0) as u32)
}

unsafe fn set_error(error: *mut *mut LdString, msg: String) {
    if let Some(out) = error.as_mut() {
        *out = LdString::new(msg);
    }
}

/// Creates (or truncates) `path`; null on failure (`*error` set, may be null).
#[no_mangle]
pub unsafe extern "C" fn ld_afw_create(
    path: *const u8,
    len: usize,
    channels: i32,
    rate: i32,
    format: i32,
    bits: i32,
    kbps: i32,
    error: *mut *mut LdString,
) -> *mut LdAudioWriter {
    let file = PathBuf::from(str_arg(path, len).into_owned());
    let fmt = encode_format(format, bits, kbps);
    let result = guard(Err("internal error".to_owned()), || {
        codec::create_writer(&file, channels.max(1) as usize, rate.max(1) as u32, fmt).map_err(|e| e.to_string())
    });
    match result {
        Ok(w) => Box::into_raw(Box::new(LdAudioWriter(w))),
        Err(msg) => {
            set_error(error, msg);
            std::ptr::null_mut()
        }
    }
}

/// Appends `frames` frames from `num_channels` planar buffers.
#[no_mangle]
pub unsafe extern "C" fn ld_afw_write(
    w: *mut LdAudioWriter,
    channels: *const *const f32,
    num_channels: i32,
    frames: i32,
) -> bool {
    let (Some(w), false) = (w.as_mut(), channels.is_null()) else { return false };
    let n = frames.max(0) as usize;
    let chans: Vec<&[f32]> = (0..num_channels.max(0) as usize).map(|c| std::slice::from_raw_parts(*channels.add(c), n)).collect();
    guard(false, || w.0.write_planar(&chans, n).is_ok())
}

/// Finalizes and closes the file (and frees `w`); the frames written, or -1.
#[no_mangle]
pub unsafe extern "C" fn ld_afw_finalize(w: *mut LdAudioWriter) -> i64 {
    if w.is_null() {
        return -1;
    }
    let w = Box::from_raw(w);
    guard(-1, || w.0.finalize().map_or(-1, |f| f as i64))
}

/// Gives up: closes and deletes the file (and frees `w`).
#[no_mangle]
pub unsafe extern "C" fn ld_afw_abort(w: *mut LdAudioWriter) {
    if w.is_null() {
        return;
    }
    let w = Box::from_raw(w);
    let path = w.0.path().to_owned();
    drop(w);
    let _ = std::fs::remove_file(path);
}

/// Opens a WAV, FLAC, AIFF or MP3 file; null if it can't be read.
#[no_mangle]
pub unsafe extern "C" fn ld_afr_open(path: *const u8, len: usize) -> *mut LdAudioReader {
    let file = PathBuf::from(str_arg(path, len).into_owned());
    match guard(None, || codec::open_reader(&file).ok()) {
        Some(r) => Box::into_raw(Box::new(LdAudioReader(r))),
        None => std::ptr::null_mut(),
    }
}

/// What the file holds (any pointer may be null).
#[no_mangle]
pub unsafe extern "C" fn ld_afr_info(
    r: *const LdAudioReader,
    format: *mut i32,
    channels: *mut i32,
    rate: *mut i32,
    bits: *mut i32,
    frames: *mut i64,
    kbps: *mut i32,
) -> bool {
    let Some(r) = r.as_ref() else { return false };
    let i = *r.0.info();
    write_info(&i, format, channels, rate, bits, frames, kbps);
    true
}

pub(crate) unsafe fn write_info(
    i: &codec::AudioInfo,
    format: *mut i32,
    channels: *mut i32,
    rate: *mut i32,
    bits: *mut i32,
    frames: *mut i64,
    kbps: *mut i32,
) {
    if let Some(x) = format.as_mut() {
        *x = i.format as i32;
    }
    if let Some(x) = channels.as_mut() {
        *x = i.channels as i32;
    }
    if let Some(x) = rate.as_mut() {
        *x = i.sample_rate as i32;
    }
    if let Some(x) = bits.as_mut() {
        *x = i32::from(i.bits);
    }
    if let Some(x) = frames.as_mut() {
        *x = i.frames as i64;
    }
    if let Some(x) = kbps.as_mut() {
        *x = i.kbps as i32;
    }
}

/// Reads `frames` frames from `start` into `num_channels` planar buffers
/// (extra buffers repeat the last channel; outside the file: zeros).
#[no_mangle]
pub unsafe extern "C" fn ld_afr_read(
    r: *mut LdAudioReader,
    start: i64,
    channels: *const *mut f32,
    num_channels: i32,
    frames: i32,
) -> bool {
    let (Some(r), false) = (r.as_mut(), channels.is_null()) else { return false };
    let n = frames.max(0) as usize;
    let mut chans: Vec<&mut [f32]> =
        (0..num_channels.max(0) as usize).map(|c| std::slice::from_raw_parts_mut(*channels.add(c), n)).collect();
    guard(false, || r.0.read(start, &mut chans).is_ok())
}

#[no_mangle]
pub unsafe extern "C" fn ld_afr_close(r: *mut LdAudioReader) {
    if !r.is_null() {
        drop(Box::from_raw(r));
    }
}

/// Why `channels` at `rate` can't be written in the format (empty: it can).
#[no_mangle]
pub unsafe extern "C" fn ld_audio_format_check(format: i32, bits: i32, kbps: i32, channels: i32, rate: i32) -> *mut LdString {
    let fmt = encode_format(format, bits, kbps);
    LdString::new(fmt.check(channels.max(0) as usize, rate.max(0) as u32).err().unwrap_or_default())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn writes_and_reads_through_the_c_functions() {
        let dir = std::env::temp_dir().join(format!("livedaw-ffi-codec-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        for (format, bits) in [(0, 32), (1, 24), (2, 16), (3, 16)] {
            let path = dir.join(format!("t.{}", FileFormat::from_i32(format).extension()));
            let p = path.to_string_lossy().into_owned();
            unsafe {
                let w = ld_afw_create(p.as_ptr(), p.len(), 2, 48000, format, bits, 192, std::ptr::null_mut());
                assert!(!w.is_null());
                let l: Vec<f32> = (0..10000).map(|i| ((i as f32) * 0.05).sin() * 0.5).collect();
                let r: Vec<f32> = l.iter().map(|x| -x).collect();
                let ptrs = [l.as_ptr(), r.as_ptr()];
                assert!(ld_afw_write(w, ptrs.as_ptr(), 2, 10000));
                assert_eq!(ld_afw_finalize(w), 10000);
                let rd = ld_afr_open(p.as_ptr(), p.len());
                assert!(!rd.is_null());
                let (mut f, mut ch, mut frames) = (-1, 0, 0i64);
                assert!(ld_afr_info(
                    rd,
                    &mut f,
                    &mut ch,
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                    &mut frames,
                    std::ptr::null_mut()
                ));
                assert_eq!((f, ch, frames), (format, 2, 10000));
                let mut a = vec![0f32; 100];
                let mut b = vec![0f32; 100];
                let outs = [a.as_mut_ptr(), b.as_mut_ptr()];
                assert!(ld_afr_read(rd, 5000, outs.as_ptr(), 2, 100));
                let tol = if format == 3 { 0.05 } else { 1e-4 };
                assert!((a[0] - l[5000]).abs() < tol && (b[0] + l[5000]).abs() < tol, "{format}: {} vs {}", a[0], l[5000]);
                ld_afr_close(rd);
            }
        }
        unsafe {
            let msg = ld_audio_format_check(3, 16, 320, 2, 96000);
            let text = std::slice::from_raw_parts(crate::session::ld_string_data(msg), crate::session::ld_string_len(msg));
            assert!(String::from_utf8_lossy(text).contains("MP3"));
            crate::session::ld_string_free(msg);
            let ok = ld_audio_format_check(1, 24, 0, 2, 96000);
            assert_eq!(crate::session::ld_string_len(ok), 0);
            crate::session::ld_string_free(ok);
        }
        let _ = std::fs::remove_dir_all(dir);
    }
}
