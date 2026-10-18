use super::*;
use crate::codec::test_util::*;
use crate::codec::{create_writer, open_reader, EncodeFormat};
use crate::wav::tests::temp_dir;
use crate::wav::SampleFormat;

fn sine(freq: f64, amp: f64, rate: u32, n: usize, phase: f64) -> Vec<f32> {
    (0..n).map(|i| (amp * (std::f64::consts::TAU * freq * i as f64 / f64::from(rate) + phase).sin()) as f32).collect()
}

/// Splits a raw stream into frames.
fn frames_of(bytes: &[u8]) -> Vec<&[u8]> {
    let mut out = Vec::new();
    let mut at = 0;
    while let Some(h) = bytes.get(at..).and_then(Header::parse) {
        let len = h.frame_len();
        if at + len > bytes.len() {
            break;
        }
        out.push(&bytes[at..at + len]);
        at += len;
    }
    out
}

/// Fits `a sin + b cos` at `freq` to `y`: (amplitude, SNR in dB of the fit).
fn fit_sine(y: &[f32], freq: f64, rate: u32) -> (f64, f64) {
    let w = std::f64::consts::TAU * freq / f64::from(rate);
    let (mut ss, mut sc, mut cc, mut ys, mut yc) = (0.0, 0.0, 0.0, 0.0, 0.0);
    for (i, &v) in y.iter().enumerate() {
        let (s, c) = ((w * i as f64).sin(), (w * i as f64).cos());
        ss += s * s;
        sc += s * c;
        cc += c * c;
        ys += f64::from(v) * s;
        yc += f64::from(v) * c;
    }
    let det = ss * cc - sc * sc;
    let a = (ys * cc - yc * sc) / det;
    let b = (yc * ss - ys * sc) / det;
    let mut err = 0.0;
    let mut sig = 0.0;
    for (i, &v) in y.iter().enumerate() {
        let m = a * (w * i as f64).sin() + b * (w * i as f64).cos();
        err += (f64::from(v) - m).powi(2);
        sig += m * m;
    }
    ((a * a + b * b).sqrt(), 10.0 * (sig / err.max(1e-30)).log10())
}

/// SNR of `y` against the reference `x` (same alignment), in dB.
fn snr(x: &[f32], y: &[f32]) -> f64 {
    let sig: f64 = x.iter().map(|&v| f64::from(v).powi(2)).sum();
    let err: f64 = x.iter().zip(y).map(|(&a, &b)| f64::from(a - b).powi(2)).sum();
    10.0 * (sig / err.max(1e-30)).log10()
}

fn read_all(path: &Path) -> (AudioInfo, Vec<Vec<f32>>) {
    let mut r = open_reader(path).unwrap();
    let info = *r.info();
    let mut chans = vec![vec![0.0; info.frames as usize]; info.channels];
    let mut refs: Vec<&mut [f32]> = chans.iter_mut().map(|c| &mut c[..]).collect();
    r.read(0, &mut refs).unwrap();
    (info, chans)
}

#[test]
fn filterbanks_reconstruct() {
    let enc = Encoder::new(1, 48000, 320).unwrap();
    let mut a = enc::Analysis::new();
    let mut s = dec::Synth::new();
    let mut rng = Lcg(3);
    let x: Vec<f32> = (0..32 * 400).map(|_| rng.next_f32() * 0.5).collect();
    let mut out = vec![0f32; x.len()];
    let mut sb = [0f32; 32];
    for (i, c) in x.chunks(32).enumerate() {
        a.run(c, &enc.window, &enc.matrix, &mut sb);
        s.run(&sb, &mut out[i * 32..i * 32 + 32]);
    }
    // The analysis / synthesis pair delays by 481 samples and is nearly transparent.
    assert!(snr(&x[2000 - 481..10000 - 481], &out[2000..10000]) > 80.0);
}

#[test]
fn an_impulse_comes_out_after_the_codec_delay() {
    let mut enc = Encoder::new(1, 48000, 320).unwrap();
    let mut x = vec![0f32; 1152 * 12];
    x[5000] = 0.5;
    let mut stream = Vec::new();
    for f in x.chunks(1152) {
        stream.extend_from_slice(enc.encode(&[f]));
    }
    let out = decode_mine(&stream);
    let (pos, peak) = out.iter().enumerate().fold((0, 0f32), |(p, m), (i, &v)| if v.abs() > m.abs() { (i, v) } else { (p, m) });
    assert_eq!(pos, 5000 + enc::TOTAL_DELAY);
    assert!((peak - 0.5).abs() < 0.01, "peak {peak}");
}

#[test]
fn sines_across_the_band_keep_level_and_snr() {
    let rate = 48000;
    for fi in 0..24 {
        let freq = fi as f64 * 1000.0 + 123.0;
        let mut enc = Encoder::new(1, rate, 320).unwrap();
        let x = sine(freq, 0.25, rate, 1152 * 12, 0.0);
        let mut stream = Vec::new();
        for f in x.chunks(1152) {
            stream.extend_from_slice(enc.encode(&[f]));
        }
        let out = decode_mine(&stream);
        let (amp, s) = fit_sine(&out[3000..13000], freq, rate);
        assert!((amp / 0.25 - 1.0).abs() < 0.02, "{freq} Hz: amplitude {amp}");
        assert!(s > 60.0, "{freq} Hz: SNR {s}");
    }
}

/// One granule of a hand-made test frame (mono, 48 kHz, 320 kbit/s): one
/// pair of big values coded with `table`, then `bits`.
struct TestGranule {
    gg: u32,
    table: usize,
    big_values: usize,
    bits: Vec<(u32, u32)>,
}

fn test_frame(grs: [&TestGranule; 2]) -> Vec<u8> {
    use crate::codec::bits::BitWriter;
    let mut w = BitWriter::new();
    w.put(0xfffb_e4c4, 32); // MPEG-1 L3, 320 kbit/s, 48 kHz, mono
    w.put(0, 9);
    w.put(0, 5);
    w.put(0, 4);
    for g in grs {
        let len: u32 = g.bits.iter().map(|b| b.1).sum();
        w.put(len, 12);
        w.put(g.big_values as u32, 9);
        w.put(g.gg, 8);
        w.put(0, 4);
        w.put(0, 1);
        w.put(g.table as u32, 5);
        w.put(0, 5);
        w.put(0, 5);
        w.put(0, 4); // region0_count: lines 0..4
        w.put(0, 3);
        w.put(0, 1);
        w.put(0, 1);
        w.put(1, 1); // count1 table B
    }
    for g in grs {
        for &(v, n) in &g.bits {
            w.put(v, n);
        }
    }
    w.align();
    w.bytes.resize(960, 0);
    w.bytes
}

fn silent_granule() -> TestGranule {
    TestGranule { gg: 0, table: 0, big_values: 0, bits: Vec::new() }
}

/// The bits of pair (x, y) (non-negative) in table `t`, signs positive.
fn pair_code(t: usize, x: usize, y: usize) -> Vec<(u32, u32)> {
    let s = tables::huff_source(t).unwrap();
    let lb = tables::LINBITS[t];
    let (xx, yy) = (x.min(15), y.min(15));
    let i = xx * s.width + yy;
    let mut bits = vec![(u32::from(s.codes[i]), u32::from(s.lens[i]))];
    if lb > 0 && x >= 15 {
        bits.push(((x - 15) as u32, lb));
    }
    if x != 0 {
        bits.push((0, 1));
    }
    if lb > 0 && y >= 15 {
        bits.push(((y - 15) as u32, lb));
    }
    if y != 0 {
        bits.push((0, 1));
    }
    bits
}

fn decode_mine(stream: &[u8]) -> Vec<f32> {
    let mut dec = Decoder::new();
    let mut out = Vec::new();
    let mut pcm = [[0f32; 1152]; 2];
    for f in frames_of(stream) {
        let _ = dec.decode(f, &mut pcm);
        out.extend_from_slice(&pcm[0]);
    }
    out
}

/// Every Huffman code of every table, in hand-made frames, decodes the same
/// with ffmpeg as with this decoder.
#[test]
fn huffman_codes_match_an_external_decoder() {
    if !have_ffmpeg() {
        return;
    }
    let mut cases = Vec::new();
    for t in [1usize, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 24] {
        let w = tables::huff_source(t).unwrap().width;
        for x in 0..w {
            for y in 0..w {
                cases.push((t, x, y));
            }
        }
    }
    // Escape values of the linbits tables.
    for t in [17usize, 23, 25, 31] {
        cases.push((t, 15 + (1 << tables::LINBITS[t]) - 1, 3));
        cases.push((t, 2, 15 + (1 << tables::LINBITS[t]) / 2));
    }
    let mut stream = Vec::new();
    for pair in cases.chunks(2) {
        // Escapes are large: a smaller gain keeps them in range.
        let gain = |x: usize, y: usize| if x.max(y) > 15 { 110 } else { 180 };
        let g: Vec<TestGranule> = pair
            .iter()
            .map(|&(t, x, y)| TestGranule { gg: gain(x, y), table: t, big_values: 1, bits: pair_code(t, x, y) })
            .collect();
        let second = g.get(1).unwrap_or(&g[0]);
        stream.extend(test_frame([&g[0], second]));
        stream.extend(test_frame([&silent_granule(), &silent_granule()]));
    }
    let dir = temp_dir("mp3-huff");
    let path = dir.join("codes.mp3");
    std::fs::write(&path, &stream).unwrap();
    let (theirs, err) = ffmpeg_decode(&path, &[]).expect("ffmpeg decodes");
    let mine = decode_mine(&stream);
    assert_eq!(theirs.len(), mine.len(), "{err}");
    let mut bad = Vec::new();
    for (i, pair) in cases.chunks(2).enumerate() {
        for (gi, &(t, x, y)) in pair.iter().enumerate() {
            // The granule and the next one (its overlap) of frame 2i.
            let a = 2 * i * 1152 + gi * 576;
            let d = (a..a + 1152).map(|k| (theirs[k] - mine[k]).abs()).fold(0.0f32, f32::max);
            if d > 1e-4 {
                bad.push((t, x, y));
            }
        }
    }
    let _ = std::fs::remove_dir_all(dir);
    assert!(bad.is_empty(), "codes decoding differently: {bad:?} {err}");
}

/// Encodes stereo sines through the writer, reads them back through the
/// reader (gapless: sample-aligned with the input) and through ffmpeg.
#[test]
fn writer_and_reader_round_trip_at_every_rate_and_bitrate() {
    let dir = temp_dir("mp3-rt");
    let ffmpeg = have_ffmpeg();
    for rate in SAMPLE_RATES {
        for kbps in crate::codec::MP3_BITRATES {
            for channels in [1usize, 2] {
                if (kbps == 192 || kbps == 256) && rate != 44100 {
                    continue; // keep the matrix small: every bit rate at 44.1 kHz
                }
                let n = rate as usize * 2 + 777;
                let left = sine(1000.0, 0.5, rate, n, 0.3);
                let right = sine(1500.0, 0.25, rate, n, 1.0);
                let path = dir.join(format!("s-{rate}-{kbps}-{channels}.mp3"));
                let mut w =
                    create_writer(&path, channels, rate, EncodeFormat::new(FileFormat::Mp3, SampleFormat::Int16, kbps)).unwrap();
                let mut at = 0;
                while at < n {
                    let k = (n - at).min(1000);
                    let refs = [&left[at..at + k], &right[at..at + k]];
                    w.write_planar(&refs[..channels], k).unwrap();
                    w.update_header().unwrap();
                    at += k;
                }
                assert_eq!(w.finalize().unwrap(), n as u64);
                let (info, back) = read_all(&path);
                assert_eq!(
                    (info.format, info.channels, info.sample_rate, info.frames, info.kbps),
                    (FileFormat::Mp3, channels, rate, n as u64, kbps)
                );
                // Aligned with the input: compare directly (the edges aside).
                let s = snr(&left[2000..n - 2000], &back[0][2000..n - 2000]);
                let min_snr = if kbps >= 192 { 40.0 } else { 30.0 };
                assert!(s > min_snr, "{rate} Hz {kbps} kbit/s x{channels}: left SNR {s:.1} dB");
                let (amp, _) = fit_sine(&back[0][2000..n - 2000], 1000.0, rate);
                assert!((20.0 * (amp / 0.5).log10()).abs() < 0.2, "level {amp}");
                if channels == 2 {
                    let s = snr(&right[2000..n - 2000], &back[1][2000..n - 2000]);
                    assert!(s > min_snr - 6.0, "{rate} Hz {kbps} kbit/s: right SNR {s:.1} dB");
                }
                let bytes = std::fs::metadata(&path).unwrap().len() as f64;
                let expected = f64::from(kbps) * 125.0 * n as f64 / f64::from(rate);
                assert!((bytes / expected - 1.0).abs() < 0.1, "CBR size {bytes} vs {expected}");

                if ffmpeg {
                    let (pcm, err) = ffmpeg_decode(&path, &[]).expect("ffmpeg decodes");
                    assert!(err.trim().is_empty(), "ffmpeg reports: {err}");
                    let l: Vec<f32> = pcm.iter().step_by(channels).copied().collect();
                    let (amp, s) = fit_sine(&l[3000..n - 3000], 1000.0, rate);
                    assert!((20.0 * (amp / 0.5).log10()).abs() < 1.0, "ffmpeg level {amp}");
                    assert!(s > min_snr, "ffmpeg SNR {s:.1}");
                    if channels == 2 {
                        let r: Vec<f32> = pcm.iter().skip(1).step_by(2).copied().collect();
                        let (amp, _) = fit_sine(&r[3000..n - 3000], 1500.0, rate);
                        assert!((20.0 * (amp / 0.25).log10()).abs() < 1.0, "ffmpeg right level {amp}");
                    }
                }
            }
        }
    }
    let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn random_access_matches_sequential_reading() {
    let dir = temp_dir("mp3-seek");
    let path = dir.join("seek.mp3");
    let rate = 44100;
    let n = 300_000;
    let mut rng = Lcg(11);
    let left: Vec<f32> = (0..n).map(|i| (i as f32 * 0.03).sin() * 0.3 + rng.next_f32() * 0.05).collect();
    let right: Vec<f32> = (0..n).map(|i| (i as f32 * 0.011).cos() * 0.3).collect();
    let mut w = create_writer(&path, 2, rate, EncodeFormat::new(FileFormat::Mp3, SampleFormat::Int16, 192)).unwrap();
    w.write_planar(&[&left, &right], n).unwrap();
    w.finalize().unwrap();
    let (_, all) = read_all(&path);
    let mut r = open_reader(&path).unwrap();
    for &start in &[150_000i64, 3, 100_001, 1151, 1152, 290_000, 0, n as i64 - 10, -5, 77_777] {
        let mut a = vec![0.0f32; 3000];
        let mut b = vec![0.0f32; 3000];
        r.read(start, &mut [&mut a[..], &mut b[..]]).unwrap();
        for i in 0..3000 {
            let k = start + i as i64;
            let want = if k < 0 || k >= n as i64 { (0.0, 0.0) } else { (all[0][k as usize], all[1][k as usize]) };
            assert!((a[i] - want.0).abs() < 1e-6 && (b[i] - want.1).abs() < 1e-6, "frame {k}: {} {} vs {want:?}", a[i], b[i]);
        }
    }
    let _ = std::fs::remove_dir_all(dir);
}

#[test]
fn a_file_cut_short_plays_to_its_last_frame() {
    let dir = temp_dir("mp3-crash");
    let path = dir.join("crash.mp3");
    let rate = 48000;
    let n = 100_000;
    let x = sine(440.0, 0.5, rate, n, 0.0);
    let mut w = Mp3Writer::new(&path, 1, rate, 128).unwrap();
    w.write_planar(&[&x], n).unwrap();
    w.update_header().unwrap(); // flushed, never finalized
    let frames = n / FRAME_SAMPLES;
    drop(w);
    let (info, back) = read_all(&path);
    assert_eq!(info.frames, (frames * FRAME_SAMPLES - enc::TOTAL_DELAY) as u64);
    let len = info.frames as usize;
    assert!(snr(&x[1000..len - 1000], &back[0][1000..len - 1000]) > 30.0, "aligned with the input");
    // Torn last frame: left out.
    let size = std::fs::metadata(&path).unwrap().len();
    std::fs::OpenOptions::new().write(true).open(&path).unwrap().set_len(size - 10).unwrap();
    let (info2, _) = read_all(&path);
    assert_eq!(info2.frames, info.frames - FRAME_SAMPLES as u64);
    let _ = std::fs::remove_dir_all(dir);
}

/// Files of other encoders (LAME: CBR, VBR, short blocks, mono; shine)
/// decode as ffmpeg decodes them.
#[test]
fn decodes_other_encoders_like_an_external_decoder() {
    if !have_ffmpeg() {
        return;
    }
    let dir = temp_dir("mp3-ext");
    let rate = 44100;
    let n = 44100 * 3;
    let mut rng = Lcg(21);
    let mut pcm = Vec::with_capacity(n * 2);
    for i in 0..n {
        let t = i as f64 / f64::from(rate);
        // Tones, noise and clicks (short blocks), different in the two channels.
        let click = if i % 9000 < 40 { 0.8 } else { 0.0 };
        let l = (t * 440.0 * std::f64::consts::TAU).sin() * 0.3 + f64::from(rng.next_f32()) * 0.05 + click;
        let r = (t * 660.0 * std::f64::consts::TAU).sin() * 0.3 + (t * 5000.0 * std::f64::consts::TAU).sin() * 0.05;
        pcm.push((l.clamp(-1.0, 1.0) * 32000.0) as i16);
        pcm.push((r * 32000.0) as i16);
    }
    let cases: [(&str, &[&str], usize); 6] = [
        ("lame-cbr128.mp3", &["-c:a", "libmp3lame", "-b:a", "128k"], 2),
        ("lame-vbr.mp3", &["-c:a", "libmp3lame", "-q:a", "2"], 2),
        ("lame-js-320.mp3", &["-c:a", "libmp3lame", "-b:a", "320k", "-joint_stereo", "1"], 2),
        ("lame-stereo.mp3", &["-c:a", "libmp3lame", "-b:a", "192k", "-joint_stereo", "0"], 2),
        ("lame-mono.mp3", &["-c:a", "libmp3lame", "-b:a", "96k", "-ac", "1"], 1),
        ("shine.mp3", &["-c:a", "libshine", "-b:a", "128k"], 2),
    ];
    for (name, args, channels) in cases {
        let path = dir.join(name);
        if !ffmpeg_encode(&pcm, 2, rate, args, &path) {
            continue; // that encoder isn't built into this ffmpeg
        }
        let (theirs, _) = ffmpeg_decode(&path, &[]).expect("ffmpeg decodes");
        let (info, mine) = read_all(&path);
        assert_eq!(info.channels, channels, "{name}");
        let frames = theirs.len() / channels;
        assert_eq!(info.frames as usize, frames, "{name}: length");
        let mut worst = 0.0f32;
        for i in 0..frames {
            for c in 0..channels {
                worst = worst.max((theirs[i * channels + c] - mine[c][i]).abs());
            }
        }
        assert!(worst < 1e-4, "{name}: differs from ffmpeg by {worst}");
    }
    let _ = std::fs::remove_dir_all(dir);
}

/// Encoding and decoding speed (run with --ignored --nocapture, in release).
#[test]
#[ignore]
fn bench_codecs() {
    let rate = 48000;
    let n = rate as usize * 20;
    let mut rng = Lcg(1);
    let l: Vec<f32> = (0..n).map(|i| (i as f32 * 0.05).sin() * 0.3 + rng.next_f32() * 0.1).collect();
    let r: Vec<f32> = (0..n).map(|i| (i as f32 * 0.031).sin() * 0.3 + rng.next_f32() * 0.1).collect();
    let dir = temp_dir("bench");
    for (fmt, sample, kbps) in [
        (FileFormat::Mp3, SampleFormat::Int16, 320),
        (FileFormat::Mp3, SampleFormat::Int16, 128),
        (FileFormat::Flac, SampleFormat::Int24, 0),
        (FileFormat::Flac, SampleFormat::Int16, 0),
    ] {
        let path = dir.join(format!("b.{}", fmt.extension()));
        let t = std::time::Instant::now();
        let mut w = create_writer(&path, 2, rate, EncodeFormat::new(fmt, sample, kbps)).unwrap();
        for c in 0..n / 4096 {
            w.write_planar(&[&l[c * 4096..(c + 1) * 4096], &r[c * 4096..(c + 1) * 4096]], 4096).unwrap();
        }
        w.finalize().unwrap();
        let enc = t.elapsed().as_secs_f64();
        let t = std::time::Instant::now();
        let (_, back) = read_all(&path);
        let dec = t.elapsed().as_secs_f64();
        eprintln!(
            "{fmt:?} {kbps}: encode {:.1}x realtime, decode {:.1}x realtime ({} frames)",
            20.0 / enc,
            20.0 / dec,
            back[0].len()
        );
    }
    let _ = std::fs::remove_dir_all(dir);
}
