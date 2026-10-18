//! The USB recorder / player driven like the engine drives it: chunks on the
//! test thread (the "audio thread"), the deck's disk thread caught up
//! between chunks. A counting allocator checks that the audio-thread calls
//! never allocate.

use livedaw_recorder::usb::{PlayerState, RecFormat, RecState, RecTap, UsbDeck, USB_CHANNELS};
use livedaw_recorder::wav::{SampleFormat, WavReader, WavWriter};
use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use std::path::{Path, PathBuf};
use std::time::Duration;

struct CountingAlloc;

thread_local! {
    static ARMED: Cell<bool> = const { Cell::new(false) };
    static ALLOCS: Cell<usize> = const { Cell::new(0) };
}

fn note() {
    if ARMED.with(|a| a.get()) {
        ALLOCS.with(|c| c.set(c.get() + 1));
    }
}

unsafe impl GlobalAlloc for CountingAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        note();
        System.alloc(layout)
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        note();
        System.dealloc(ptr, layout)
    }
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        note();
        System.realloc(ptr, layout, new_size)
    }
}

#[global_allocator]
static GLOBAL: CountingAlloc = CountingAlloc;

const RATE: f64 = 48000.0;
const BLOCK: usize = 256;

fn temp_dir(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("livedaw-usb-{}-{}", name, std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

fn deck() -> UsbDeck {
    let d = UsbDeck::new();
    // SAFETY: no audio thread yet.
    unsafe { d.prepare(RATE, BLOCK) };
    d
}

fn settle(d: &UsbDeck) {
    assert!(d.wait_for_disk(Duration::from_secs(10)), "disk thread stuck");
}

/// One chunk: plays (returns the four player channels) and records `taps`
/// (one slice per port, `None` = silence). Asserts it doesn't allocate.
fn chunk(d: &UsbDeck, n: usize, taps: &[Option<&[f32]>; USB_CHANNELS]) -> [Vec<f32>; USB_CHANNELS] {
    let mut out: [Vec<f32>; USB_CHANNELS] = std::array::from_fn(|_| Vec::with_capacity(n));
    ARMED.with(|a| a.set(true));
    unsafe {
        d.begin_chunk(n);
        let ptrs = *d.player_ptrs();
        for (o, p) in out.iter_mut().zip(ptrs) {
            // Vec::extend_from_slice within capacity doesn't allocate.
            o.extend_from_slice(std::slice::from_raw_parts(p, n));
        }
        for (ch, t) in taps.iter().enumerate() {
            d.write_rec(ch, t.map(|x| &x[..n]), n);
        }
        let _ = d.rec_tap(0);
        d.end_chunk(n);
    }
    ARMED.with(|a| a.set(false));
    assert_eq!(ALLOCS.with(|c| c.replace(0)), 0, "the audio-thread side allocated");
    out
}

const SILENT: [Option<&[f32]>; USB_CHANNELS] = [None, None, None, None];

/// A distinct signal per channel on the integer grid of `bits`.
fn grid_signal(bits: u16, channel: usize, start: usize, n: usize) -> Vec<f32> {
    let scale = if bits == 16 { 32768.0f64 } else { 8_388_608.0 };
    (start..start + n)
        .map(|i| {
            let v = ((i as f64 * 0.0137 * (channel + 1) as f64).sin() * 0.8 * scale).round();
            (v / scale) as f32
        })
        .collect()
}

fn read_wav(path: &Path) -> (livedaw_recorder::wav::WavInfo, Vec<Vec<f32>>) {
    let mut r = WavReader::open(path).unwrap();
    let info = *r.info();
    let mut chans = vec![vec![0.0; info.frames as usize]; info.channels];
    let mut refs: Vec<&mut [f32]> = chans.iter_mut().map(|c| &mut c[..]).collect();
    r.read(0, &mut refs).unwrap();
    (info, chans)
}

/// Records `chunks` chunks of grid signals from frame `start` of the signal.
fn record_chunks(d: &UsbDeck, bits: u16, start: usize, chunks: usize) {
    for k in 0..chunks {
        let sig: Vec<Vec<f32>> = (0..USB_CHANNELS).map(|c| grid_signal(bits, c, start + k * BLOCK, BLOCK)).collect();
        chunk(d, BLOCK, &[Some(&sig[0]), Some(&sig[1]), Some(&sig[2]), Some(&sig[3])]);
        if k % 16 == 15 {
            settle(d);
        }
    }
}

#[test]
fn records_2_and_4_channels_at_16_and_24_bit_bit_exact() {
    let dir = temp_dir("rec");
    for (channels, bits) in [(2usize, 16u16), (4, 24), (2, 24), (4, 16)] {
        let d = deck();
        let path = dir.join(format!("R_{channels}_{bits}.wav"));
        d.record(&path, RecFormat { channels, bits }).unwrap();
        assert_eq!(d.rec_state(), RecState::Recording, "shows at once");
        record_chunks(&d, bits, 0, 40);
        assert_eq!(d.rec_time(), 40 * BLOCK as i64);
        d.stop_recording();
        chunk(&d, BLOCK, &SILENT);
        settle(&d);
        assert_eq!(d.rec_state(), RecState::Stopped);

        let (info, chans) = read_wav(&path);
        let format = if bits == 16 { SampleFormat::Int16 } else { SampleFormat::Int24 };
        assert_eq!((info.channels, info.sample_rate, info.format), (channels, 48000, format));
        assert_eq!(info.frames, 40 * BLOCK as u64);
        for (c, got) in chans.iter().enumerate() {
            assert_eq!(*got, grid_signal(bits, c, 0, 40 * BLOCK), "{channels} ch {bits} bit: channel {c}");
        }
        assert_eq!(d.dropouts(), 0);
    }
    let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn pause_resume_and_new_file_lose_nothing() {
    let dir = temp_dir("newfile");
    let d = deck();
    let a = dir.join("a.wav");
    let b = dir.join("b.wav");
    d.record(&a, RecFormat { channels: 2, bits: 24 }).unwrap();
    record_chunks(&d, 24, 0, 10);
    d.pause_recording();
    assert_eq!(d.rec_state(), RecState::Paused);
    record_chunks(&d, 24, 10 * BLOCK, 5); // not recorded
    assert_eq!(d.rec_time(), 10 * BLOCK as i64, "the record time stops while paused");
    // REC resumes the same file.
    d.record(&dir.join("unused.wav"), RecFormat::default()).unwrap();
    assert!(!dir.join("unused.wav").exists());
    record_chunks(&d, 24, 15 * BLOCK, 10);
    d.new_file(&b).unwrap();
    assert_eq!(d.rec_file().as_deref(), Some(b.as_path()));
    record_chunks(&d, 24, 25 * BLOCK, 7);
    assert_eq!(d.rec_time(), 7 * BLOCK as i64, "the time restarts with the new file");
    assert_eq!(d.rec_total(), 27 * BLOCK as i64);
    d.stop_recording();
    chunk(&d, BLOCK, &SILENT);
    settle(&d);

    let (_, first) = read_wav(&a);
    let mut expect = grid_signal(24, 0, 0, 10 * BLOCK);
    expect.extend(grid_signal(24, 0, 15 * BLOCK, 10 * BLOCK));
    assert_eq!(first[0], expect, "before the pause, then after it");
    let (_, second) = read_wav(&b);
    assert_eq!(second[0], grid_signal(24, 0, 25 * BLOCK, 7 * BLOCK), "NEW FILE starts on the next sample");
    assert_eq!(second[1], grid_signal(24, 1, 25 * BLOCK, 7 * BLOCK));
    assert!(d.new_file(&dir.join("c.wav")).is_err(), "NEW FILE needs a recording");

    // STOP before the audio thread took the recording: no file is left.
    let c = dir.join("c.wav");
    d.record(&c, RecFormat::default()).unwrap();
    d.stop_recording();
    chunk(&d, BLOCK, &SILENT);
    settle(&d);
    assert!(!c.exists());
    let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn rec_meters_and_taps() {
    let d = deck();
    assert_eq!(d.tap(0), RecTap::Strip { kind: 3, number: 1, channel: 0 }, "REC 1/2 = MAIN 1 L/R");
    assert_eq!(d.tap(1), RecTap::Strip { kind: 3, number: 1, channel: 1 });
    assert_eq!(d.tap(2), RecTap::Off);
    d.set_tap(2, RecTap::Source(5));
    d.set_tap(3, RecTap::Strip { kind: 2, number: 12, channel: 1 });
    assert_eq!(d.tap(2), RecTap::Source(5));
    assert_eq!(d.tap(3), RecTap::Strip { kind: 2, number: 12, channel: 1 });
    for t in [RecTap::Off, RecTap::Source(0), RecTap::Source(0x20003), RecTap::Strip { kind: 0, number: 40, channel: 1 }] {
        assert_eq!(RecTap::from_code(t.code()), t);
    }
    // Meters work without recording.
    let x = vec![0.5f32; BLOCK];
    chunk(&d, BLOCK, &[None, Some(&x), None, None]);
    assert_eq!(d.rec_level(1), 0.5);
    assert_eq!(d.rec_level(1), 0.0, "peak since the last read");
}

// ---- player -------------------------------------------------------------------------------

/// Writes a 16-bit clip of `frames` whose samples encode (clip, channel, frame).
fn clip(dir: &Path, name: &str, tag: usize, channels: usize, frames: usize, rate: u32) -> PathBuf {
    let path = dir.join(name);
    let chans: Vec<Vec<f32>> = (0..channels).map(|c| clip_signal(tag, c, 0, frames)).collect();
    let refs: Vec<&[f32]> = chans.iter().map(|c| &c[..]).collect();
    let mut w = WavWriter::create_with_format(&path, channels, rate, SampleFormat::Int16).unwrap();
    w.write_planar(&refs, frames).unwrap();
    w.finalize().unwrap();
    path
}

fn clip_signal(tag: usize, channel: usize, start: usize, n: usize) -> Vec<f32> {
    (start..start + n).map(|i| ((tag * 4000 + channel * 1000 + i % 997) as f32 - 8000.0) / 32768.0).collect()
}

/// Plays `chunks` chunks (catching the disk up after each); returns channel
/// `ch` of the output.
fn play_chunks(d: &UsbDeck, chunks: usize, ch: usize) -> Vec<f32> {
    let mut out = Vec::new();
    for _ in 0..chunks {
        let o = chunk(d, BLOCK, &SILENT);
        out.extend_from_slice(&o[ch]);
        settle(d);
    }
    out
}

/// The index where `needle` first occurs in `hay`.
fn find(hay: &[f32], needle: &[f32]) -> Option<usize> {
    hay.windows(needle.len()).position(|w| w == needle)
}

#[test]
fn plays_a_playlist_gapless_and_stops_at_its_end() {
    let dir = temp_dir("playlist");
    let a = clip(&dir, "a.wav", 1, 2, 3000, 48000);
    let b = clip(&dir, "b.wav", 2, 2, 2000, 48000);
    let d = deck();
    let (ida, idb) = {
        let mut q = d.queue();
        q.new_playlist();
        (q.add(&a), q.add(&b))
    };
    d.play();
    assert_eq!(d.player_state(), PlayerState::Playing, "shows at once");
    let out = play_chunks(&d, 30, 1);
    // A starts after priming with a fade-in, then plays exactly.
    let start = find(&out, &clip_signal(1, 1, 300, 200)).expect("a plays") - 300;
    assert_eq!(&out[start + 256..start + 3000], &clip_signal(1, 1, 256, 2744)[..]);
    assert!(out[..start].iter().all(|&x| x == 0.0));
    // B follows on the next sample, then the playlist ends.
    assert_eq!(&out[start + 3000..start + 5000], &clip_signal(2, 1, 0, 2000)[..]);
    assert!(out[start + 5000..].iter().all(|&x| x == 0.0));
    assert_eq!(d.player_state(), PlayerState::Stopped);
    assert_eq!(d.current_entry(), ida, "the end cues the first clip");
    assert_eq!(d.position(), 0);
    let info = d.file_info(idb).unwrap();
    assert_eq!((info.channels, info.sample_rate, info.bits, info.length, info.rate_mismatch), (2, 48000, 16, 2000, false));
    let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn repeat_skip_and_stop_after() {
    let dir = temp_dir("repeat");
    let a = clip(&dir, "a.wav", 1, 1, 1500, 48000);
    let b = clip(&dir, "b.wav", 2, 1, 1500, 48000);
    let c = clip(&dir, "c.wav", 3, 1, 1500, 48000);
    let d = deck();
    let ids: Vec<u64> = {
        let mut q = d.queue();
        q.new_playlist();
        let ids = vec![q.add(&a), q.add(&b), q.add(&c)];
        q.set_skip(1, true);
        q.set_repeat(true);
        ids
    };
    d.play();
    let out = play_chunks(&d, 40, 1); // mono plays on both USB PLAYER 1 and 2
    let start = find(&out, &clip_signal(1, 0, 300, 200)).unwrap() - 300;
    assert_eq!(&out[start + 1500..start + 3000], &clip_signal(3, 0, 0, 1500)[..], "SKIP passes b over");
    assert_eq!(&out[start + 3000..start + 4500], &clip_signal(1, 0, 0, 1500)[..], "REPEAT starts over");
    assert_eq!(d.player_state(), PlayerState::Playing);

    // STOP after the clip playing now (set well before its end).
    d.stop();
    play_chunks(&d, 2, 0);
    d.queue().set_stop(0, true);
    d.load(ids[0], 0, true);
    let out = play_chunks(&d, 20, 0);
    let start = find(&out, &clip_signal(1, 0, 300, 200)).unwrap() - 300;
    assert!(out[start + 1500..].iter().all(|&x| x == 0.0), "stops after a");
    assert_eq!(d.player_state(), PlayerState::Stopped);
    assert_eq!(d.current_entry(), ids[2], "and cues the next clip (b is skipped)");
    d.play();
    let out = play_chunks(&d, 10, 0);
    assert!(find(&out, &clip_signal(3, 0, 300, 200)).is_some(), "PLAY goes on with c");
    let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn pause_seek_next_and_prev() {
    let dir = temp_dir("seek");
    let a = clip(&dir, "a.wav", 1, 4, 48000, 48000);
    let b = clip(&dir, "b.wav", 2, 4, 48000, 48000);
    let d = deck();
    d.queue().set_folder(&dir);
    let ids: Vec<u64> = d.queue().entries().iter().map(|e| e.id).collect();
    assert_eq!(d.queue().entries()[0].path, a);
    assert_eq!(d.queue().entries()[1].path, b);

    // Cue a at 10000 while stopped, play: it starts there.
    d.load(ids[0], 10_000, false);
    play_chunks(&d, 2, 0);
    assert_eq!(d.player_state(), PlayerState::Paused);
    assert_eq!(d.position(), 10_000);
    d.play();
    let out = play_chunks(&d, 8, 3);
    let at = find(&out, &clip_signal(1, 3, 10_300, 100)).expect("plays from the cue") as i64 - 300;
    assert!(at >= 0);
    assert_eq!(d.position(), 10_000 + 8 * BLOCK as i64, "PLAY from PAUSE goes on at once");

    // PAUSE keeps the position; a seek while paused moves it; PLAY goes on there.
    d.pause();
    play_chunks(&d, 2, 0);
    assert_eq!(d.player_state(), PlayerState::Paused);
    let paused_at = d.position();
    assert_eq!(play_chunks(&d, 2, 0), vec![0.0; 2 * BLOCK]);
    assert_eq!(d.position(), paused_at);
    d.seek(30_000);
    assert_eq!(d.position(), 30_000, "shows at once");
    play_chunks(&d, 2, 0);
    assert_eq!(d.position(), 30_000);
    d.play();
    let out = play_chunks(&d, 4, 2);
    assert!(find(&out, &clip_signal(1, 2, 30_300, 100)).is_some());

    // > goes to b (playing on), < in its first seconds back to a.
    assert!(d.next());
    play_chunks(&d, 3, 0);
    assert_eq!(d.current_entry(), ids[1]);
    assert_eq!(d.player_state(), PlayerState::Playing);
    assert!(!d.next(), "the last file without REPEAT");
    assert!(d.prev());
    play_chunks(&d, 3, 0);
    assert_eq!(d.current_entry(), ids[0]);
    // STOP rewinds.
    d.stop();
    play_chunks(&d, 2, 0);
    assert_eq!((d.player_state(), d.position()), (PlayerState::Stopped, 0));
    // A folder plays one file at a time: a ends, b is cued.
    d.load(ids[0], 47_000, true);
    play_chunks(&d, 12, 0);
    assert_eq!((d.player_state(), d.current_entry()), (PlayerState::Stopped, ids[1]));
    let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn a_file_at_another_rate_is_resampled_and_flagged() {
    let dir = temp_dir("rate");
    let path = dir.join("tone.wav");
    let x: Vec<f32> = (0..44100).map(|i| (2.0 * std::f64::consts::PI * 441.0 * i as f64 / 44100.0).sin() as f32 * 0.5).collect();
    let mut w = WavWriter::create_with_format(&path, 2, 44100, SampleFormat::Int24).unwrap();
    w.write_planar(&[&x, &x], x.len()).unwrap();
    w.finalize().unwrap();
    let d = deck();
    let id = {
        let mut q = d.queue();
        q.new_playlist();
        q.add(&path)
    };
    d.play();
    let out = play_chunks(&d, 60, 0);
    let info = d.file_info(id).unwrap();
    assert!(info.rate_mismatch);
    assert_eq!((info.sample_rate, info.bits, info.length), (44100, 24, 48000));
    // 441 Hz at 48 kHz: a period of 108.84 samples. Count zero crossings over 0.25 s.
    let first = out.iter().position(|&v| v.abs() > 0.1).unwrap();
    let seg = &out[first + 2000..first + 14000];
    let crossings = seg.windows(2).filter(|w| w[0] < 0.0 && w[1] >= 0.0).count();
    assert!((109..=111).contains(&crossings), "{crossings} periods in 12000 samples");
    let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn missing_files_and_empty_queues() {
    let dir = temp_dir("missing");
    let d = deck();
    d.play();
    play_chunks(&d, 3, 0);
    assert_eq!(d.player_state(), PlayerState::Stopped, "nothing to play");
    let id = {
        let mut q = d.queue();
        q.new_playlist();
        q.add(&dir.join("gone.wav"))
    };
    d.play();
    play_chunks(&d, 3, 0);
    assert_eq!(d.player_state(), PlayerState::Error);
    assert_eq!(d.current_entry(), id);
    let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn the_clip_cued_keeps_its_info_when_the_queue_changes() {
    let dir = temp_dir("info");
    let a = clip(&dir, "a.wav", 1, 2, 40000, 48000);
    let d = deck();
    d.queue().set_folder(&dir);
    let id = d.queue().entries()[0].id;
    d.load(id, 0, true);
    play_chunks(&d, 4, 0);
    d.stop();
    play_chunks(&d, 2, 0);
    d.queue().new_playlist();
    d.queue().add(&a);
    assert_eq!(d.current_entry(), id);
    assert!(d.file_info(id).is_some(), "still known");
    assert_eq!(d.length(), 40000);
    let _ = std::fs::remove_dir_all(dir);
}
