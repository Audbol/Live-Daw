//! `ld_rec_*`: the multitrack recorder / player (message thread; the audio
//! thread side runs inside `ld_engine_process`). C++ view: `livedaw::Recorder`.

use crate::session::{str_arg, LdString};
use crate::{engine, guard};
use livedaw_engine::recorder::{
    Clip, FadeShape, PlaybackTrack, Playlist, RecordSource, RecordTrackSpec, SourceKind, TransportState,
};
use livedaw_engine::Engine;
use std::path::PathBuf;
use std::time::Duration;

/// A clip of the playlist (`Recorder::Clip`).
#[repr(C)]
pub struct LdRecClip {
    pub path: *const u8,
    pub path_len: usize,
    pub file_offset: i64,
    pub start: i64,
    pub length: i64,
    pub fade_in: i64,
    pub fade_out: i64,
    pub gain: f32,
    /// 0 Linear, 1 EqualPower, 2 SCurve.
    pub fade_in_shape: i32,
    pub fade_out_shape: i32,
}

/// A playback track: its clips are `clips[first_clip .. first_clip + num_clips]`.
#[repr(C)]
pub struct LdRecPlaybackTrack {
    pub channel: i32,
    pub num_channels: i32,
    pub gain: f32,
    pub mute: bool,
    pub first_clip: i32,
    pub num_clips: i32,
}

/// A track to record (`Recorder::RecordTrack`).
#[repr(C)]
pub struct LdRecRecordTrack {
    pub track: i32,
    /// 0 hardware input, 1 strip pre-fader, 2 strip post-fader.
    pub source_kind: i32,
    pub port: i32,
    pub strip_kind: i32,
    pub strip_number: i32,
    pub num_channels: i32,
    pub path: *const u8,
    pub path_len: usize,
}

/// A finished take; the caller frees `path` with `ld_string_free`.
#[repr(C)]
pub struct LdRecTake {
    pub track: i32,
    pub num_channels: i32,
    pub start: i64,
    pub length: i64,
    pub path: *mut LdString,
}

unsafe fn slice<'a, T>(p: *const T, n: i32) -> &'a [T] {
    if p.is_null() || n <= 0 {
        &[]
    } else {
        std::slice::from_raw_parts(p, n as usize)
    }
}

/// 0 Stopped, 1 Starting, 2 Playing, 3 Recording.
#[no_mangle]
pub unsafe extern "C" fn ld_rec_state(e: *const Engine) -> i32 {
    engine(e).map_or(0, |e| guard(TransportState::Stopped, || e.recorder().state()) as i32)
}

#[no_mangle]
pub unsafe extern "C" fn ld_rec_position(e: *const Engine) -> i64 {
    engine(e).map_or(0, |e| guard(0, || e.recorder().position()))
}

#[no_mangle]
pub unsafe extern "C" fn ld_rec_play(e: *const Engine) {
    if let Some(e) = engine(e) {
        guard((), || e.recorder().play());
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_rec_stop(e: *const Engine) {
    if let Some(e) = engine(e) {
        guard((), || e.recorder().stop());
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_rec_locate(e: *const Engine, position: i64) {
    if let Some(e) = engine(e) {
        guard((), || e.recorder().locate(position));
    }
}

#[no_mangle]
pub unsafe extern "C" fn ld_rec_set_loop(e: *const Engine, start: i64, end: i64, enabled: bool) {
    if let Some(e) = engine(e) {
        guard((), || e.recorder().set_loop(start, end, enabled));
    }
}

/// Writes the loop range (either pointer may be null); returns whether it is enabled.
#[no_mangle]
pub unsafe extern "C" fn ld_rec_loop(e: *const Engine, start: *mut i64, end: *mut i64) -> bool {
    let Some(e) = engine(e) else { return false };
    let l = guard(Default::default(), || e.recorder().loop_range());
    if let Some(s) = start.as_mut() {
        *s = l.start;
    }
    if let Some(x) = end.as_mut() {
        *x = l.end;
    }
    l.enabled
}

/// Replaces the playlist (copied; the arrays may be freed after the call).
#[no_mangle]
pub unsafe extern "C" fn ld_rec_set_playlist(
    e: *const Engine,
    tracks: *const LdRecPlaybackTrack,
    num_tracks: i32,
    clips: *const LdRecClip,
    num_clips: i32,
) {
    let Some(e) = engine(e) else { return };
    let tracks = slice(tracks, num_tracks);
    let clips = slice(clips, num_clips);
    guard((), || {
        let playlist = Playlist {
            tracks: tracks
                .iter()
                .map(|t| {
                    let first = t.first_clip.max(0) as usize;
                    let last = (first + t.num_clips.max(0) as usize).min(clips.len());
                    PlaybackTrack {
                        channel: t.channel.max(0) as usize,
                        num_channels: t.num_channels.clamp(1, 2) as usize,
                        gain: t.gain,
                        mute: t.mute,
                        clips: clips[first.min(last)..last]
                            .iter()
                            .map(|c| Clip {
                                file: PathBuf::from(str_arg(c.path, c.path_len).into_owned()),
                                file_offset: c.file_offset.max(0),
                                start: c.start,
                                length: c.length,
                                gain: c.gain,
                                fade_in: c.fade_in.max(0),
                                fade_out: c.fade_out.max(0),
                                fade_in_shape: FadeShape::from_i32(c.fade_in_shape),
                                fade_out_shape: FadeShape::from_i32(c.fade_out_shape),
                            })
                            .collect(),
                    }
                })
                .collect(),
        };
        e.recorder().set_playlist(&playlist);
    });
}

/// The format of the next recordings: 0 WAV, 1 FLAC, 2 AIFF, 3 MP3; `bits`
/// 16, 24 or 32 (32-bit float, WAV / AIFF); `kbps` the MP3 bit rate. Take
/// files get the format's extension (the caller names them).
#[no_mangle]
pub unsafe extern "C" fn ld_rec_set_record_format(e: *const Engine, format: i32, bits: i32, kbps: i32) {
    if let Some(e) = engine(e) {
        guard((), || e.recorder().set_record_format(crate::codec::encode_format(format, bits, kbps)));
    }
}

/// The format of the next recordings (pointers may be null).
#[no_mangle]
pub unsafe extern "C" fn ld_rec_record_format(e: *const Engine, format: *mut i32, bits: *mut i32, kbps: *mut i32) {
    let Some(e) = engine(e) else { return };
    let f = e.recorder().record_format();
    if let Some(x) = format.as_mut() {
        *x = f.format as i32;
    }
    if let Some(x) = bits.as_mut() {
        *x = i32::from(f.sample.bits());
    }
    if let Some(x) = kbps.as_mut() {
        *x = f.kbps as i32;
    }
}

/// Starts recording. On failure returns false and sets `*error` (free it
/// with `ld_string_free`; `error` may be null).
#[no_mangle]
pub unsafe extern "C" fn ld_rec_record(
    e: *const Engine,
    tracks: *const LdRecRecordTrack,
    num_tracks: i32,
    error: *mut *mut LdString,
) -> bool {
    let Some(e) = engine(e) else { return false };
    let tracks = slice(tracks, num_tracks);
    let result = guard(Err("internal error".to_owned()), || {
        let specs: Vec<RecordTrackSpec> = tracks
            .iter()
            .map(|t| RecordTrackSpec {
                track: t.track,
                source: RecordSource {
                    kind: match t.source_kind {
                        1 => SourceKind::StripPreFader,
                        2 => SourceKind::StripPostFader,
                        _ => SourceKind::HardwareInput,
                    },
                    port: t.port,
                    strip_kind: t.strip_kind,
                    strip_number: t.strip_number,
                },
                num_channels: t.num_channels.max(0) as usize,
                file: PathBuf::from(str_arg(t.path, t.path_len).into_owned()),
            })
            .collect();
        e.record(&specs)
    });
    match result {
        Ok(()) => true,
        Err(msg) => {
            if let Some(out) = error.as_mut() {
                *out = LdString::new(msg);
            }
            false
        }
    }
}

/// Takes the next finalized take; false when there is none.
#[no_mangle]
pub unsafe extern "C" fn ld_rec_pop_take(e: *const Engine, out: *mut LdRecTake) -> bool {
    let (Some(e), Some(out)) = (engine(e), out.as_mut()) else { return false };
    let Some(take) = guard(None, || e.recorder().pop_finished_take()) else { return false };
    *out = LdRecTake {
        track: take.track,
        num_channels: take.num_channels as i32,
        start: take.start,
        length: take.length,
        path: LdString::new(take.file.to_string_lossy().into_owned()),
    };
    true
}

/// Linear peak of a recording track's channel since the last call.
#[no_mangle]
pub unsafe extern "C" fn ld_rec_record_level(e: *const Engine, track: i32, channel: i32) -> f32 {
    engine(e).map_or(0.0, |e| guard(0.0, || e.recorder().record_level(track, channel.max(0) as usize)))
}

/// Peaks of the recording of `track` from peak `first_peak`: min/max pairs
/// per channel, interleaved. `min_max` holds `max_peaks * 2 * channels`
/// floats (channels: `ld_rec_record_track_channels`). Returns the peaks written.
#[no_mangle]
pub unsafe extern "C" fn ld_rec_recording_peaks(
    e: *const Engine,
    track: i32,
    first_peak: i64,
    max_peaks: i32,
    min_max: *mut f32,
) -> i32 {
    let Some(e) = engine(e) else { return 0 };
    if min_max.is_null() || max_peaks <= 0 {
        return 0;
    }
    guard(0, || {
        let channels = e.recorder().record_track_channels(track);
        let out = std::slice::from_raw_parts_mut(min_max, max_peaks as usize * 2 * channels);
        e.recorder().recording_peaks(track, first_peak, max_peaks as usize, out) as i32
    })
}

/// Channels of `track` in the current (or last) recording; 0 if not recording it.
#[no_mangle]
pub unsafe extern "C" fn ld_rec_record_track_channels(e: *const Engine, track: i32) -> i32 {
    engine(e).map_or(0, |e| guard(0, || e.recorder().record_track_channels(track) as i32))
}

#[no_mangle]
pub unsafe extern "C" fn ld_rec_recorded_length(e: *const Engine) -> i64 {
    engine(e).map_or(0, |e| guard(0, || e.recorder().recorded_length()))
}

#[no_mangle]
pub unsafe extern "C" fn ld_rec_dropouts(e: *const Engine) -> i32 {
    engine(e).map_or(0, |e| e.recorder().dropouts() as i32)
}

#[no_mangle]
pub unsafe extern "C" fn ld_rec_disk_load(e: *const Engine) -> f32 {
    engine(e).map_or(0.0, |e| e.recorder().disk_load())
}

/// Waits (up to `timeout_ms`) until the disk threads have caught up with what
/// they can do now (tests and offline rendering, faster than real time).
#[no_mangle]
pub unsafe extern "C" fn ld_rec_wait_for_disk(e: *const Engine, timeout_ms: i32) -> bool {
    engine(e).is_some_and(|e| guard(false, || e.recorder().wait_for_disk(Duration::from_millis(timeout_ms.max(0) as u64))))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::session::ld_string_free;
    use crate::*;

    #[test]
    fn records_and_plays_through_the_c_abi() {
        let dir = std::env::temp_dir().join(format!("livedaw-ffi-rec-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let path = dir.join("t.wav").to_string_lossy().into_owned();
        let clip_path = dir.join("clip.wav").to_string_lossy().into_owned();
        unsafe {
            let e = ld_engine_create(0);
            let in_map = [0, 1];
            let out_map = [0, 1];
            ld_engine_prepare(e, 48000.0, 64, in_map.as_ptr(), 2, out_map.as_ptr(), 2);
            ld_rec_locate(e, 480);
            assert_eq!(ld_rec_position(e), 480);
            ld_rec_set_loop(e, 100, 200, true);
            let (mut s, mut x) = (0, 0);
            assert!(ld_rec_loop(e, &mut s, &mut x) && (s, x) == (100, 200));

            let clip = LdRecClip {
                path: clip_path.as_ptr(),
                path_len: clip_path.len(),
                file_offset: 0,
                start: 0,
                length: 10,
                fade_in: 0,
                fade_out: 0,
                gain: 1.0,
                fade_in_shape: 0,
                fade_out_shape: 2,
            };
            let track = LdRecPlaybackTrack { channel: 0, num_channels: 1, gain: 1.0, mute: false, first_clip: 0, num_clips: 1 };
            ld_rec_set_playlist(e, &track, 1, &clip, 1);

            let rec = LdRecRecordTrack {
                track: 5,
                source_kind: 0,
                port: 1,
                strip_kind: 0,
                strip_number: 0,
                num_channels: 1,
                path: path.as_ptr(),
                path_len: path.len(),
            };
            let mut error = std::ptr::null_mut();
            let bad = LdRecRecordTrack { port: 7, ..rec };
            assert!(!ld_rec_record(e, &bad, 1, &mut error));
            assert!(!error.is_null());
            ld_string_free(error);
            assert!(ld_rec_record(e, &rec, 1, std::ptr::null_mut()));
            assert_eq!(ld_rec_state(e), 1, "starting");

            let input = [0.5f32; 64];
            let ins = [input.as_ptr(), input.as_ptr()];
            let mut o = [[0.0f32; 64]; 2];
            for _ in 0..200 {
                assert!(ld_rec_wait_for_disk(e, 10_000));
                let outs = [o[0].as_mut_ptr(), o[1].as_mut_ptr()];
                ld_engine_process(e, ins.as_ptr(), 2, outs.as_ptr(), 2, 64);
            }
            assert_eq!(ld_rec_state(e), 3, "recording");
            assert!(ld_rec_recorded_length(e) > 0);
            assert!(ld_rec_record_level(e, 5, 0) > 0.4);
            assert_eq!(ld_rec_record_track_channels(e, 5), 1);
            ld_rec_stop(e);
            let outs = [o[0].as_mut_ptr(), o[1].as_mut_ptr()];
            ld_engine_process(e, ins.as_ptr(), 2, outs.as_ptr(), 2, 64);
            let mut take = LdRecTake { track: 0, num_channels: 0, start: 0, length: 0, path: std::ptr::null_mut() };
            let deadline = std::time::Instant::now() + Duration::from_secs(10);
            while !ld_rec_pop_take(e, &mut take) {
                assert!(std::time::Instant::now() < deadline);
                std::thread::sleep(Duration::from_millis(2));
            }
            assert_eq!((take.track, take.num_channels, take.start), (5, 1, 480));
            assert!(take.length > 0);
            assert!(!take.path.is_null());
            ld_string_free(take.path);
            let mut peaks = [0.0f32; 8];
            assert!(ld_rec_recording_peaks(e, 5, 0, 4, peaks.as_mut_ptr()) == 4);
            assert_eq!(peaks[1], 0.5);
            assert_eq!(ld_rec_dropouts(e), 0);
            ld_engine_destroy(e);
        }
        let _ = std::fs::remove_dir_all(dir);
    }
}
