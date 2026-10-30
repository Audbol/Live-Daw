//! FLAC (RFC 9639): a 16/24-bit encoder and a decoder with random access.
//!
//! **Encoder** ([`FlacWriter`]): fixed 4096-sample blocks. Per block and
//! channel it tries a constant subframe, the fixed predictors of order 0-4
//! and LPC predictors (autocorrelation of a Tukey-windowed block,
//! Levinson-Durbin, the two most promising orders up to 12 quantized to 14
//! or 15 bits), each with Rice-coded residuals in the best partition order
//! (0-8; Rice2 parameters for 24-bit material), and falls back to verbatim.
//! Stereo blocks pick left/right, left/side, side/right or mid/side, whichever
//! is smallest. The STREAMINFO block is written with "unknown" length and MD5
//! and patched when the file is finalized; frames are written as they are
//! encoded, so a file cut short by a crash decodes up to its last frame.
//!
//! **Decoder** ([`FlacReader`]): any FLAC stream (fixed or variable block
//! size, 4-32 bits, up to 8 channels, all subframe types, wasted bits,
//! Rice / Rice2 with escaped partitions), verifying each frame's CRC-16.
//! Sequential reads decode frame after frame; a jump uses the SEEKTABLE if
//! there is one, else bisects the file on frame headers (sync code, CRC-8,
//! then a full decode). A stream without a total length (a crash) is
//! measured by decoding its last frames.

use super::bits::{BitReader, BitWriter};
use super::md5::Md5;
use super::{fill_out, from_int, to_int, AudioInfo, AudioReader, AudioWriter, FileFormat, FileWindow};
use std::fs::File;
use std::io::{self, BufWriter, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

/// Samples per block the encoder writes.
pub const BLOCK_SIZE: usize = 4096;
const MAX_LPC_ORDER: usize = 12;
const MAX_PARTITION_ORDER: u32 = 8;
/// Offset of the STREAMINFO body in the files the writer produces.
const STREAMINFO_AT: u64 = 8;

// ---- checksums ------------------------------------------------------------------------

fn crc8(data: &[u8]) -> u8 {
    let mut crc = 0u8;
    for &b in data {
        crc ^= b;
        for _ in 0..8 {
            crc = if crc & 0x80 != 0 { (crc << 1) ^ 0x07 } else { crc << 1 };
        }
    }
    crc
}

fn crc16_table() -> &'static [u16; 256] {
    static TABLE: std::sync::OnceLock<[u16; 256]> = std::sync::OnceLock::new();
    TABLE.get_or_init(|| {
        let mut t = [0u16; 256];
        for (i, e) in t.iter_mut().enumerate() {
            let mut crc = (i as u16) << 8;
            for _ in 0..8 {
                crc = if crc & 0x8000 != 0 { (crc << 1) ^ 0x8005 } else { crc << 1 };
            }
            *e = crc;
        }
        t
    })
}

fn crc16(data: &[u8]) -> u16 {
    let t = crc16_table();
    data.iter().fold(0u16, |crc, &b| (crc << 8) ^ t[usize::from((crc >> 8) as u8 ^ b)])
}

// ---- encoder ------------------------------------------------------------------------------

fn sample_rate_code(rate: u32) -> (u32, Option<(u32, u32)>) {
    match rate {
        88200 => (1, None),
        176400 => (2, None),
        192000 => (3, None),
        8000 => (4, None),
        16000 => (5, None),
        22050 => (6, None),
        24000 => (7, None),
        32000 => (8, None),
        44100 => (9, None),
        48000 => (10, None),
        96000 => (11, None),
        r if r % 1000 == 0 && r / 1000 < 256 => (12, Some((r / 1000, 8))),
        r if r < 65536 => (13, Some((r, 16))),
        r if r % 10 == 0 && r / 10 < 65536 => (14, Some((r / 10, 16))),
        _ => (0, None),
    }
}

fn sample_size_code(bits: u32) -> u32 {
    match bits {
        8 => 1,
        12 => 2,
        16 => 4,
        20 => 5,
        24 => 6,
        32 => 7,
        _ => 0,
    }
}

fn put_utf8(w: &mut BitWriter, v: u64) {
    if v < 0x80 {
        w.put(v as u32, 8);
        return;
    }
    let bytes = match v {
        _ if v < 0x800 => 2,
        _ if v < 0x1_0000 => 3,
        _ if v < 0x20_0000 => 4,
        _ if v < 0x400_0000 => 5,
        _ if v < 0x8000_0000 => 6,
        _ => 7,
    };
    let lead: u32 = if bytes == 7 { 0xfe } else { (0xff00u32 >> bytes) & 0xff };
    let first_bits = if bytes == 7 { 0 } else { 7 - bytes };
    w.put(lead | ((v >> (6 * (bytes - 1))) as u32 & ((1 << first_bits) - 1)), 8);
    for i in (0..bytes - 1).rev() {
        w.put(0x80 | ((v >> (6 * i)) as u32 & 0x3f), 8);
    }
}

#[inline]
fn fold(e: i64) -> u64 {
    ((e << 1) ^ (e >> 63)) as u64
}

/// How a subframe is coded.
#[derive(Clone, Debug)]
enum Predictor {
    Constant,
    Verbatim,
    Fixed(usize),
    Lpc { coefs: Vec<i32>, precision: u32, shift: u32 },
}

/// The Rice coding chosen for a residual.
#[derive(Clone, Debug, Default)]
struct RiceChoice {
    order: u32,
    params: Vec<u32>,
    rice2: bool,
    bits: u64,
}

/// Picks the partition order and parameters for `res` (whose first `pred_order`
/// samples are warm-up, not coded).
fn choose_rice(res: &[i64], n: usize, pred_order: usize, sums: &mut Vec<u64>) -> RiceChoice {
    // The finest partition order allowed: n divisible by 2^o and the first
    // partition longer than the predictor order.
    let mut max_order = 0;
    while max_order < MAX_PARTITION_ORDER && n % (1 << (max_order + 1)) == 0 && (n >> (max_order + 1)) > pred_order {
        max_order += 1;
    }
    let parts = 1usize << max_order;
    let psize = n >> max_order;
    sums.clear();
    sums.resize(parts, 0);
    for (p, s) in sums.iter_mut().enumerate() {
        let start = if p == 0 { pred_order } else { p * psize };
        *s = res[start..(p + 1) * psize].iter().map(|&e| fold(e)).sum();
    }
    let mut best = RiceChoice { bits: u64::MAX, ..Default::default() };
    let mut level: Vec<u64> = sums.clone();
    let mut order = max_order;
    loop {
        let parts = 1usize << order;
        let psize = n >> order;
        let mut params = Vec::with_capacity(parts);
        let mut bits = 0u64;
        let mut max_k = 0;
        for (p, &sum) in level.iter().enumerate() {
            let count = (if p == 0 { psize - pred_order } else { psize }) as u64;
            // Estimated bits for parameter k: count * (k + 1) + sum >> k.
            let k = match sum.checked_div(count) {
                Some(mean) if mean > 0 => (63 - mean.leading_zeros()).min(30),
                _ => 0,
            };
            let cost = |k: u32| count * u64::from(k + 1) + (sum >> k);
            let mut kb = k;
            for cand in [k.saturating_sub(1), k + 1] {
                if cand <= 30 && cost(cand) < cost(kb) {
                    kb = cand;
                }
            }
            max_k = max_k.max(kb);
            bits += cost(kb);
            params.push(kb);
        }
        let rice2 = max_k > 14;
        bits += parts as u64 * if rice2 { 5 } else { 4 } + 6;
        if bits < best.bits {
            best = RiceChoice { order, params, rice2, bits };
        }
        if order == 0 {
            break;
        }
        level = level.chunks(2).map(|c| c[0] + c[1]).collect();
        order -= 1;
    }
    best
}

fn write_residual(w: &mut BitWriter, res: &[i64], n: usize, pred_order: usize, rice: &RiceChoice) {
    w.put(u32::from(rice.rice2), 2);
    w.put(rice.order, 4);
    let psize = n >> rice.order;
    let pbits = if rice.rice2 { 5 } else { 4 };
    for (p, &k) in rice.params.iter().enumerate() {
        w.put(k, pbits);
        let start = if p == 0 { pred_order } else { p * psize };
        let mask = if k == 0 { 0 } else { (1u64 << k) - 1 };
        for &e in &res[start..(p + 1) * psize] {
            let u = fold(e);
            w.put_unary((u >> k) as u32);
            if k > 0 {
                w.put_u64(u & mask, k);
            }
        }
    }
}

/// Fixed predictor residuals of order `order`.
fn fixed_residual(x: &[i64], order: usize, res: &mut [i64]) {
    let n = x.len();
    res[..order.min(n)].copy_from_slice(&x[..order.min(n)]);
    for i in order..n {
        res[i] = match order {
            0 => x[i],
            1 => x[i] - x[i - 1],
            2 => x[i] - 2 * x[i - 1] + x[i - 2],
            3 => x[i] - 3 * x[i - 1] + 3 * x[i - 2] - x[i - 3],
            _ => x[i] - 4 * x[i - 1] + 6 * x[i - 2] - 4 * x[i - 3] + x[i - 4],
        };
    }
}

fn lpc_residual(x: &[i64], coefs: &[i32], shift: u32, res: &mut [i64]) -> bool {
    let order = coefs.len();
    let n = x.len();
    res[..order].copy_from_slice(&x[..order]);
    let limit = i64::from(i32::MAX);
    for i in order..n {
        let mut acc = 0i64;
        for (j, &c) in coefs.iter().enumerate() {
            acc += i64::from(c) * x[i - 1 - j];
        }
        let e = x[i] - (acc >> shift);
        if e > limit || e < -limit {
            return false;
        }
        res[i] = e;
    }
    true
}

/// Levinson-Durbin: predictor coefficients of every order up to `max` (row
/// k-1 = order k) and the prediction errors.
fn levinson(r: &[f64], max: usize) -> (Vec<Vec<f64>>, Vec<f64>) {
    let mut out = Vec::with_capacity(max);
    let mut errs = Vec::with_capacity(max);
    let mut a = vec![0.0f64; max];
    let mut err = r[0];
    for i in 0..max {
        if err <= 0.0 {
            break;
        }
        let mut acc = r[i + 1];
        for j in 0..i {
            acc -= a[j] * r[i - j];
        }
        let k = acc / err;
        let prev = a.clone();
        a[i] = k;
        for j in 0..i {
            a[j] = prev[j] - k * prev[i - 1 - j];
        }
        err *= 1.0 - k * k;
        out.push(a[..=i].to_vec());
        errs.push(err.max(0.0));
    }
    (out, errs)
}

fn quantize(coefs: &[f64], precision: u32) -> Option<(Vec<i32>, u32)> {
    let cmax = coefs.iter().fold(0.0f64, |m, c| m.max(c.abs()));
    if cmax <= 0.0 || !cmax.is_finite() {
        return None;
    }
    let max_q = (1i32 << (precision - 1)) - 1;
    // Largest shift with cmax * 2^shift <= max_q.
    let e = cmax.log2().floor() as i32 + 1;
    let shift = (precision as i32 - 1 - e).clamp(0, 15) as u32;
    let mut q = Vec::with_capacity(coefs.len());
    let mut error = 0.0f64;
    for &c in coefs {
        error += c * f64::from(1u32 << shift);
        let v = error.round().clamp(-f64::from(max_q) - 1.0, f64::from(max_q));
        q.push(v as i32);
        error -= v;
    }
    Some((q, shift))
}

/// Encodes one channel's block: analysis, then the cheapest coding.
struct SubframeEncoder {
    res: Vec<i64>,
    best_res: Vec<i64>,
    window: Vec<f64>,
    windowed: Vec<f64>,
    sums: Vec<u64>,
}

struct Subframe {
    predictor: Predictor,
    rice: RiceChoice,
    /// Estimated size in bits.
    bits: u64,
    /// The residual (for fixed / LPC).
    res: Vec<i64>,
}

impl SubframeEncoder {
    fn new() -> Self {
        Self { res: Vec::new(), best_res: Vec::new(), window: Vec::new(), windowed: Vec::new(), sums: Vec::new() }
    }

    fn tukey(&mut self, n: usize) {
        if self.window.len() == n {
            return;
        }
        let p = 0.5;
        let taper = ((p * n as f64) / 2.0) as usize;
        self.window = (0..n)
            .map(|i| {
                if taper == 0 || (i >= taper && i < n - taper) {
                    1.0
                } else {
                    let k = if i < taper { i } else { n - 1 - i };
                    0.5 - 0.5 * (std::f64::consts::PI * k as f64 / taper as f64).cos()
                }
            })
            .collect();
    }

    fn analyze(&mut self, x: &[i64], bps: u32) -> Subframe {
        let n = x.len();
        let verbatim_bits = n as u64 * u64::from(bps) + 8;
        if x.iter().all(|&v| v == x[0]) {
            return Subframe {
                predictor: Predictor::Constant,
                rice: RiceChoice::default(),
                bits: u64::from(bps) + 8,
                res: Vec::new(),
            };
        }
        let mut best =
            Subframe { predictor: Predictor::Verbatim, rice: RiceChoice::default(), bits: verbatim_bits, res: Vec::new() };
        self.res.resize(n, 0);
        self.best_res.resize(n, 0);
        let mut found = false;
        for order in 0..=4usize.min(n.saturating_sub(1)) {
            fixed_residual(x, order, &mut self.res);
            if self.res[order..].iter().any(|&e| e.abs() > i64::from(i32::MAX)) {
                continue;
            }
            let rice = choose_rice(&self.res, n, order, &mut self.sums);
            let bits = rice.bits + 8 + order as u64 * u64::from(bps);
            if bits < best.bits {
                best = Subframe { predictor: Predictor::Fixed(order), rice, bits, res: Vec::new() };
                std::mem::swap(&mut self.res, &mut self.best_res);
                found = true;
            }
        }
        // LPC
        let max_order = MAX_LPC_ORDER.min(n / 4);
        if max_order >= 1 && n >= 32 {
            self.tukey(n);
            self.windowed.clear();
            self.windowed.extend(x.iter().zip(&self.window).map(|(&v, &w)| v as f64 * w));
            let mut r = vec![0.0f64; max_order + 1];
            for (lag, rl) in r.iter_mut().enumerate() {
                *rl = self.windowed[lag..].iter().zip(&self.windowed).map(|(a, b)| a * b).sum();
            }
            if r[0] > 0.0 {
                r[0] *= 1.0 + 1e-9; // a touch of white noise keeps it well conditioned
                let (sets, errs) = levinson(&r, max_order);
                let precision = if bps <= 16 { 14 } else { 15 };
                // Estimated bits per order: residual entropy plus the coefficients.
                let mut cands: Vec<(f64, usize)> = errs
                    .iter()
                    .enumerate()
                    .map(|(i, &e)| {
                        let order = i + 1;
                        let per_sample = 0.5 * (e / n as f64).max(1e-12).log2();
                        (per_sample.max(0.0) * n as f64 + (order as u32 * (precision + bps)) as f64, order)
                    })
                    .collect();
                cands.sort_by(|a, b| a.0.total_cmp(&b.0));
                for &(_, order) in cands.iter().take(2) {
                    let Some((coefs, shift)) = quantize(&sets[order - 1], precision) else { continue };
                    if !lpc_residual(x, &coefs, shift, &mut self.res) {
                        continue;
                    }
                    let rice = choose_rice(&self.res, n, order, &mut self.sums);
                    let bits = rice.bits + 8 + order as u64 * u64::from(bps + precision) + 9;
                    if bits < best.bits {
                        best = Subframe { predictor: Predictor::Lpc { coefs, precision, shift }, rice, bits, res: Vec::new() };
                        std::mem::swap(&mut self.res, &mut self.best_res);
                        found = true;
                    }
                }
            }
        }
        if found && !matches!(best.predictor, Predictor::Verbatim) {
            best.res = self.best_res.clone();
        }
        best
    }
}

fn write_subframe(w: &mut BitWriter, x: &[i64], bps: u32, sf: &Subframe) {
    let n = x.len();
    w.put(0, 1);
    match &sf.predictor {
        Predictor::Constant => {
            w.put(0, 6);
            w.put(0, 1);
            w.put_signed(x[0] as i32, bps);
        }
        Predictor::Verbatim => {
            w.put(1, 6);
            w.put(0, 1);
            for &v in x {
                w.put_signed(v as i32, bps);
            }
        }
        Predictor::Fixed(order) => {
            w.put(8 | *order as u32, 6);
            w.put(0, 1);
            for &v in &x[..*order] {
                w.put_signed(v as i32, bps);
            }
            write_residual(w, &sf.res, n, *order, &sf.rice);
        }
        Predictor::Lpc { coefs, precision, shift } => {
            let order = coefs.len();
            w.put(0x20 | (order as u32 - 1), 6);
            w.put(0, 1);
            for &v in &x[..order] {
                w.put_signed(v as i32, bps);
            }
            w.put(precision - 1, 4);
            w.put(*shift, 5);
            for &c in coefs {
                w.put_signed(c, *precision);
            }
            write_residual(w, &sf.res, n, order, &sf.rice);
        }
    }
}

/// Writes 16- or 24-bit FLAC files.
pub struct FlacWriter {
    out: BufWriter<File>,
    path: PathBuf,
    channels: usize,
    sample_rate: u32,
    bits: u32,
    /// Pending samples per channel (less than a block).
    pending: Vec<Vec<i32>>,
    /// Frames (samples per channel) accepted, pending ones included.
    frames: u64,
    frame_number: u64,
    md5: Md5,
    md5_bytes: Vec<u8>,
    min_frame: u32,
    max_frame: u32,
    bw: BitWriter,
    enc: SubframeEncoder,
    work: Vec<Vec<i64>>,
}

impl FlacWriter {
    pub fn create(path: &Path, channels: usize, sample_rate: u32, bits: u32) -> io::Result<Self> {
        let bits = if bits <= 16 { 16 } else { 24 };
        if channels == 0 || channels > 8 {
            return Err(io::Error::new(io::ErrorKind::Unsupported, "FLAC has 1 to 8 channels"));
        }
        let file = File::create(path)?;
        let mut w = Self {
            out: BufWriter::with_capacity(1 << 18, file),
            path: path.to_owned(),
            channels,
            sample_rate,
            bits,
            pending: vec![Vec::with_capacity(BLOCK_SIZE); channels],
            frames: 0,
            frame_number: 0,
            md5: Md5::new(),
            md5_bytes: Vec::new(),
            min_frame: 0,
            max_frame: 0,
            bw: BitWriter::new(),
            enc: SubframeEncoder::new(),
            work: vec![Vec::new(); 4],
        };
        let mut head = Vec::with_capacity(42);
        head.extend_from_slice(b"fLaC");
        head.push(0x80); // last metadata block, STREAMINFO
        head.extend_from_slice(&[0, 0, 34]);
        head.extend_from_slice(&w.streaminfo(0, [0; 16]));
        w.out.write_all(&head)?;
        w.out.flush()?;
        Ok(w)
    }

    fn streaminfo(&self, total: u64, md5: [u8; 16]) -> [u8; 34] {
        let mut b = BitWriter::new();
        b.put(BLOCK_SIZE as u32, 16);
        b.put(BLOCK_SIZE as u32, 16);
        b.put(self.min_frame, 24);
        b.put(self.max_frame, 24);
        b.put(self.sample_rate, 20);
        b.put(self.channels as u32 - 1, 3);
        b.put(self.bits - 1, 5);
        b.put_u64(total, 36);
        for x in md5 {
            b.put(u32::from(x), 8);
        }
        let mut out = [0u8; 34];
        out.copy_from_slice(&b.bytes);
        out
    }

    fn push_frame(&mut self, channels: &[&[f32]], start: usize, k: usize) {
        let bits = self.bits;
        let width = bits as usize / 8;
        self.md5_bytes.clear();
        let base = self.pending[0].len();
        for c in 0..self.channels {
            let src = channels.get(c).copied();
            for i in 0..k {
                let x = src.and_then(|ch| ch.get(start + i)).copied().unwrap_or(0.0);
                self.pending[c].push(to_int(x, bits));
            }
        }
        for i in 0..k {
            for c in 0..self.channels {
                let v = self.pending[c][base + i];
                self.md5_bytes.extend_from_slice(&v.to_le_bytes()[..width]);
            }
        }
        self.md5.update(&self.md5_bytes);
        self.frames += k as u64;
    }

    fn encode_pending(&mut self) -> io::Result<()> {
        let n = self.pending[0].len();
        if n == 0 {
            return Ok(());
        }
        let bps = self.bits;
        let mut chans: Vec<Vec<i64>> = std::mem::take(&mut self.work);
        chans.resize(4, Vec::new());
        // Channel signals: 0..channels, plus mid (2) / side (3) for stereo.
        let mut subframes: Vec<Subframe> = Vec::new();
        let assignment;
        if self.channels == 2 {
            let (l, r) = (&self.pending[0], &self.pending[1]);
            for (j, c) in chans.iter_mut().enumerate() {
                c.clear();
                c.extend((0..n).map(|i| {
                    let (a, b) = (i64::from(l[i]), i64::from(r[i]));
                    match j {
                        0 => a,
                        1 => b,
                        2 => (a + b) >> 1,
                        _ => a - b,
                    }
                }));
            }
            let sf: Vec<Subframe> = (0..4).map(|j| self.enc.analyze(&chans[j], if j == 3 { bps + 1 } else { bps })).collect();
            let options = [(0usize, 1usize, 1u32), (0, 3, 8), (3, 1, 9), (2, 3, 10)];
            let &(a, b, code) = options.iter().min_by_key(|&&(a, b, _)| sf[a].bits + sf[b].bits).unwrap_or(&options[0]);
            assignment = code;
            let mut sf = sf.into_iter().map(Some).collect::<Vec<_>>();
            subframes.push(sf[a].take().unwrap_or_else(unreachable_subframe));
            subframes.push(sf[b].take().unwrap_or_else(unreachable_subframe));
            let (x0, x1) = (chans[a].clone(), chans[b].clone());
            chans[0] = x0;
            chans[1] = x1;
        } else {
            chans.resize(self.channels.max(4), Vec::new());
            for c in 0..self.channels {
                chans[c].clear();
                chans[c].extend(self.pending[c].iter().map(|&v| i64::from(v)));
                subframes.push(self.enc.analyze(&chans[c], bps));
            }
            assignment = self.channels as u32 - 1;
        }

        let w = &mut self.bw;
        w.clear();
        w.put(0xfff8, 16);
        let (bs_code, bs_extra) = if n == BLOCK_SIZE {
            (12, None)
        } else if n <= 256 {
            (6, Some((n as u32 - 1, 8)))
        } else {
            (7, Some((n as u32 - 1, 16)))
        };
        let (sr_code, sr_extra) = sample_rate_code(self.sample_rate);
        w.put(bs_code, 4);
        w.put(sr_code, 4);
        w.put(assignment, 4);
        w.put(sample_size_code(bps), 3);
        w.put(0, 1);
        put_utf8(w, self.frame_number);
        if let Some((v, b)) = bs_extra {
            w.put(v, b);
        }
        if let Some((v, b)) = sr_extra {
            w.put(v, b);
        }
        let crc = crc8(&w.bytes);
        w.put(u32::from(crc), 8);
        for (c, sf) in subframes.iter().enumerate() {
            let side = (assignment == 8 && c == 1) || (assignment == 9 && c == 0) || (assignment == 10 && c == 1);
            write_subframe(w, &chans[c], if side { bps + 1 } else { bps }, sf);
        }
        w.align();
        let crc = crc16(&w.bytes);
        w.put(u32::from(crc), 16);
        let size = w.bytes.len() as u32;
        self.min_frame = if self.min_frame == 0 { size } else { self.min_frame.min(size) };
        self.max_frame = self.max_frame.max(size);
        self.out.write_all(&self.bw.bytes)?;
        self.frame_number += 1;
        for p in &mut self.pending {
            p.clear();
        }
        self.work = chans;
        Ok(())
    }
}

fn unreachable_subframe() -> Subframe {
    Subframe { predictor: Predictor::Verbatim, rice: RiceChoice::default(), bits: 0, res: Vec::new() }
}

impl AudioWriter for FlacWriter {
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
            let k = (BLOCK_SIZE - self.pending[0].len()).min(n - done);
            self.push_frame(channels, done, k);
            done += k;
            if self.pending[0].len() == BLOCK_SIZE {
                self.encode_pending()?;
            }
        }
        Ok(())
    }

    fn write_silence(&mut self, n: usize) -> io::Result<()> {
        let zeros = [0.0f32; 1024];
        let mut left = n;
        while left > 0 {
            let k = left.min(zeros.len());
            let refs: Vec<&[f32]> = (0..self.channels).map(|_| &zeros[..k]).collect();
            self.write_planar(&refs, k)?;
            left -= k;
        }
        Ok(())
    }

    fn update_header(&mut self) -> io::Result<()> {
        // Complete frames are valid as they are; STREAMINFO keeps "unknown
        // length" until the end.
        self.out.flush()
    }

    fn finalize(mut self: Box<Self>) -> io::Result<u64> {
        self.encode_pending()?;
        self.out.flush()?;
        let md5 = std::mem::take(&mut self.md5).finish();
        let info = self.streaminfo(self.frames, md5);
        let file = self.out.get_mut();
        file.seek(SeekFrom::Start(STREAMINFO_AT))?;
        file.write_all(&info)?;
        file.sync_data()?;
        Ok(self.frames)
    }
}

// ---- decoder ----------------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, Default)]
struct StreamInfo {
    min_block: u32,
    max_block: u32,
    max_frame: u32,
    sample_rate: u32,
    channels: usize,
    bits: u32,
    total: u64,
}

/// A decoded frame's header.
#[derive(Clone, Copy, Debug)]
struct FrameHeader {
    block_size: usize,
    channels: usize,
    assignment: u32,
    bits: u32,
    first_sample: u64,
    /// Header length in bytes (CRC-8 included).
    len: usize,
}

fn parse_header(b: &[u8], si: &StreamInfo) -> Option<FrameHeader> {
    if b.len() < 6 || b[0] != 0xff || b[1] & 0xfe != 0xf8 {
        return None;
    }
    let variable = b[1] & 1 == 1;
    let bs_code = b[2] >> 4;
    let sr_code = b[2] & 15;
    let ch_code = u32::from(b[3] >> 4);
    let ss_code = (b[3] >> 1) & 7;
    if b[3] & 1 != 0 || sr_code == 15 || ss_code == 3 || ch_code > 10 || bs_code == 0 {
        return None;
    }
    // The UTF-8 coded number.
    let lead = b[4];
    let extra = match lead {
        0x00..=0x7f => 0,
        0xc0..=0xdf => 1,
        0xe0..=0xef => 2,
        0xf0..=0xf7 => 3,
        0xf8..=0xfb => 4,
        0xfc..=0xfd => 5,
        0xfe => 6,
        _ => return None,
    };
    let mut v: u64 = if extra == 0 {
        u64::from(lead)
    } else if extra == 6 {
        0
    } else {
        u64::from(lead & (0x3f >> extra))
    };
    let mut at = 5;
    for _ in 0..extra {
        let c = *b.get(at)?;
        if c & 0xc0 != 0x80 {
            return None;
        }
        v = (v << 6) | u64::from(c & 0x3f);
        at += 1;
    }
    let block_size = match bs_code {
        1 => 192,
        2..=5 => 576 << (bs_code - 2),
        6 => {
            at += 1;
            usize::from(*b.get(at - 1)?) + 1
        }
        7 => {
            at += 2;
            (usize::from(*b.get(at - 2)?) << 8 | usize::from(*b.get(at - 1)?)) + 1
        }
        _ => 256 << (bs_code - 8),
    };
    match sr_code {
        12 => at += 1,
        13 | 14 => at += 2,
        _ => {}
    }
    let crc = *b.get(at)?;
    if crc8(&b[..at]) != crc {
        return None;
    }
    let channels = if ch_code < 8 { ch_code as usize + 1 } else { 2 };
    let bits = match ss_code {
        0 => si.bits,
        1 => 8,
        2 => 12,
        4 => 16,
        5 => 20,
        6 => 24,
        _ => 32,
    };
    if bits == 0 || (si.channels != 0 && channels != si.channels) {
        return None;
    }
    let first_sample = if variable { v } else { v * u64::from(si.max_block.max(si.min_block).max(1)) };
    Some(FrameHeader { block_size, channels, assignment: ch_code, bits, first_sample, len: at + 1 })
}

fn decode_residual(r: &mut BitReader, out: &mut [i64], order: usize) -> Option<()> {
    let n = out.len();
    let method = r.get(2);
    if method > 1 {
        return None;
    }
    let pbits = if method == 0 { 4 } else { 5 };
    let escape = (1 << pbits) - 1;
    let porder = r.get(4);
    let parts = 1usize << porder;
    if n % parts != 0 || (n >> porder) < order {
        return None;
    }
    let psize = n >> porder;
    let mut i = order;
    for p in 0..parts {
        let k = r.get(pbits);
        let end = (p + 1) * psize;
        if k == escape {
            let raw = r.get(5);
            for e in &mut out[i..end] {
                *e = i64::from(r.get_signed(raw));
            }
        } else {
            for e in &mut out[i..end] {
                let q = u64::from(r.get_unary()?);
                let u = (q << k) | u64::from(r.get(k));
                *e = ((u >> 1) as i64) ^ -((u & 1) as i64);
            }
        }
        if r.overrun {
            return None;
        }
        i = end;
    }
    Some(())
}

fn decode_subframe(r: &mut BitReader, out: &mut [i64], bps: u32, qlp: &mut Vec<i64>) -> Option<()> {
    let n = out.len();
    if r.get(1) != 0 {
        return None;
    }
    let kind = r.get(6);
    let mut wasted = 0;
    if r.get_bit() {
        wasted = r.get_unary()? + 1;
    }
    if wasted >= bps {
        return None;
    }
    let bps = bps - wasted;
    let get = |r: &mut BitReader| -> i64 {
        if bps > 32 {
            let v = r.get_u64(bps);
            ((v << (64 - bps)) as i64) >> (64 - bps)
        } else {
            i64::from(r.get_signed(bps))
        }
    };
    match kind {
        0 => {
            let v = get(r);
            out.fill(v);
        }
        1 => {
            for o in out.iter_mut() {
                *o = get(r);
            }
        }
        8..=12 => {
            let order = (kind - 8) as usize;
            if order > n {
                return None;
            }
            for o in &mut out[..order] {
                *o = get(r);
            }
            decode_residual(r, out, order)?;
            for i in order..n {
                out[i] += match order {
                    0 => 0,
                    1 => out[i - 1],
                    2 => 2 * out[i - 1] - out[i - 2],
                    3 => 3 * out[i - 1] - 3 * out[i - 2] + out[i - 3],
                    _ => 4 * out[i - 1] - 6 * out[i - 2] + 4 * out[i - 3] - out[i - 4],
                };
            }
        }
        32..=63 => {
            let order = (kind - 31) as usize;
            if order > n {
                return None;
            }
            for o in &mut out[..order] {
                *o = get(r);
            }
            let precision = r.get(4) + 1;
            if precision == 16 {
                return None;
            }
            let shift = r.get_signed(5);
            if shift < 0 {
                return None;
            }
            qlp.clear();
            for _ in 0..order {
                qlp.push(i64::from(r.get_signed(precision)));
            }
            decode_residual(r, out, order)?;
            for i in order..n {
                let mut acc = 0i64;
                for (j, &c) in qlp.iter().enumerate() {
                    acc = acc.wrapping_add(c.wrapping_mul(out[i - 1 - j]));
                }
                out[i] = out[i].wrapping_add(acc >> shift);
            }
        }
        _ => return None,
    }
    if wasted > 0 {
        for o in out.iter_mut() {
            *o <<= wasted;
        }
    }
    if r.overrun {
        None
    } else {
        Some(())
    }
}

/// Decodes the frame at the start of `b` into `out` (one buffer per channel).
/// Returns the header and the frame's length in bytes.
fn decode_frame(b: &[u8], si: &StreamInfo, out: &mut [Vec<i64>], qlp: &mut Vec<i64>) -> Option<(FrameHeader, usize)> {
    let h = parse_header(b, si)?;
    // Every frame has the stream's channels (fewer would leave a channel of
    // the block from an earlier frame).
    if h.channels != out.len() {
        return None;
    }
    let mut r = BitReader::at(b, h.len * 8);
    for (c, o) in out.iter_mut().enumerate().take(h.channels) {
        o.resize(h.block_size, 0);
        let side = matches!((h.assignment, c), (8, 1) | (9, 0) | (10, 1));
        decode_subframe(&mut r, o, if side { h.bits + 1 } else { h.bits }, qlp)?;
    }
    r.align();
    let end = r.position() / 8;
    if end + 2 > b.len() {
        return None;
    }
    let crc = u16::from_be_bytes([b[end], b[end + 1]]);
    if crc16(&b[..end]) != crc {
        return None;
    }
    let n = h.block_size;
    match h.assignment {
        8 => {
            let (l, s) = out.split_at_mut(1);
            for i in 0..n {
                s[0][i] = l[0][i] - s[0][i];
            }
        }
        9 => {
            let (s, r) = out.split_at_mut(1);
            for i in 0..n {
                s[0][i] += r[0][i];
            }
        }
        10 => {
            let (m, s) = out.split_at_mut(1);
            for i in 0..n {
                let side = s[0][i];
                let mid = (m[0][i] << 1) | (side & 1);
                m[0][i] = (mid + side) >> 1;
                s[0][i] = (mid - side) >> 1;
            }
        }
        _ => {}
    }
    Some((h, end + 2))
}

/// Random access to a FLAC file.
pub struct FlacReader {
    win: FileWindow,
    si: StreamInfo,
    info: AudioInfo,
    first_frame: u64,
    /// SEEKTABLE points (sample, byte offset from the first frame).
    seek_points: Vec<(u64, u64)>,
    block: Vec<Vec<i64>>,
    block_start: u64,
    block_len: usize,
    /// Byte offset of the frame after the decoded one (0: none decoded).
    next_offset: u64,
    qlp: Vec<i64>,
    max_frame_bytes: usize,
}

const WINDOW_BYTES: usize = 1 << 19;

impl FlacReader {
    pub fn open(path: &Path) -> io::Result<Self> {
        Self::from_file(File::open(path)?)
    }

    pub fn from_file(file: File) -> io::Result<Self> {
        let invalid = |m: &str| io::Error::new(io::ErrorKind::InvalidData, m.to_owned());
        let mut win = FileWindow::new(file)?;
        let head = win.get(0, 10, 4096)?.to_vec();
        let mut pos = super::id3v2_size(&head);
        if win.get(pos, 4, 4096)? != b"fLaC" {
            return Err(invalid("not a FLAC file"));
        }
        pos += 4;
        let mut si: Option<StreamInfo> = None;
        let mut seek_points = Vec::new();
        loop {
            let h = win.get(pos, 4, 4096)?.to_vec();
            if h.len() < 4 {
                return Err(invalid("truncated FLAC metadata"));
            }
            let last = h[0] & 0x80 != 0;
            let kind = h[0] & 0x7f;
            let len = (usize::from(h[1]) << 16) | (usize::from(h[2]) << 8) | usize::from(h[3]);
            let body = win.get(pos + 4, len, 4096)?.to_vec();
            if body.len() < len {
                return Err(invalid("truncated FLAC metadata"));
            }
            match kind {
                0 if len >= 34 => {
                    let mut r = BitReader::new(&body);
                    si = Some(StreamInfo {
                        min_block: r.get(16),
                        max_block: r.get(16),
                        max_frame: {
                            r.get(24);
                            r.get(24)
                        },
                        sample_rate: r.get(20),
                        channels: r.get(3) as usize + 1,
                        bits: r.get(5) + 1,
                        total: r.get_u64(36),
                    });
                }
                3 => {
                    for p in body.chunks_exact(18) {
                        let sample = u64::from_be_bytes(p[..8].try_into().unwrap_or([0xff; 8]));
                        let offset = u64::from_be_bytes(p[8..16].try_into().unwrap_or([0; 8]));
                        if sample != u64::MAX {
                            seek_points.push((sample, offset));
                        }
                    }
                }
                127 => return Err(invalid("invalid FLAC metadata block")),
                _ => {}
            }
            pos += 4 + len as u64;
            if last {
                break;
            }
        }
        let si = si.ok_or_else(|| invalid("no STREAMINFO"))?;
        if si.sample_rate == 0 || si.bits < 4 {
            return Err(invalid("invalid STREAMINFO"));
        }
        let max_block = if si.max_block >= 16 { si.max_block as usize } else { 65535 };
        let max_frame_bytes = if si.max_frame > 0 {
            si.max_frame as usize + 64
        } else {
            max_block * si.channels * (si.bits as usize + 1) / 8 + 1024
        };
        let mut r = Self {
            win,
            si,
            info: AudioInfo {
                format: FileFormat::Flac,
                channels: si.channels,
                sample_rate: si.sample_rate,
                frames: si.total,
                bits: si.bits as u16,
                float: false,
                kbps: 0,
            },
            first_frame: pos,
            seek_points,
            block: vec![Vec::new(); si.channels],
            block_start: 0,
            block_len: 0,
            next_offset: 0,
            qlp: Vec::new(),
            max_frame_bytes,
        };
        if si.total == 0 {
            r.info.frames = r.measure()?;
            r.next_offset = 0;
            r.block_len = 0;
        }
        Ok(r)
    }

    /// Decodes the frame at `offset` into the block. Returns false if there
    /// is no valid frame there.
    fn decode_at(&mut self, offset: u64) -> io::Result<bool> {
        let bytes = self.win.get(offset, self.max_frame_bytes, WINDOW_BYTES)?;
        match decode_frame(bytes, &self.si, &mut self.block, &mut self.qlp) {
            Some((h, len)) => {
                self.block_start = h.first_sample;
                self.block_len = h.block_size;
                self.next_offset = offset + len as u64;
                Ok(true)
            }
            None => {
                // The failed decode may have resized the block: it holds no frame now.
                self.block_len = 0;
                Ok(false)
            }
        }
    }

    /// The first valid frame at or after `from` (decoded into the block):
    /// its offset.
    fn find_frame(&mut self, from: u64) -> io::Result<Option<u64>> {
        let mut at = from.max(self.first_frame);
        while at + 2 < self.win.len {
            let chunk = self.win.get(at, 65536, WINDOW_BYTES)?.to_vec();
            if chunk.len() < 2 {
                break;
            }
            let mut i = 0;
            while i + 1 < chunk.len() {
                if chunk[i] == 0xff && chunk[i + 1] & 0xfe == 0xf8 && self.decode_at(at + i as u64)? {
                    return Ok(Some(at + i as u64));
                }
                i += 1;
            }
            at += (chunk.len() - 1) as u64;
        }
        Ok(None)
    }

    /// The length of a stream whose STREAMINFO doesn't say (a file cut short):
    /// decodes the last frames.
    fn measure(&mut self) -> io::Result<u64> {
        let mut back = 1u64 << 18;
        loop {
            let from = self.win.len.saturating_sub(back).max(self.first_frame);
            if self.find_frame(from)?.is_some() {
                let mut end = self.block_start + self.block_len as u64;
                while self.decode_at(self.next_offset)? {
                    end = self.block_start + self.block_len as u64;
                }
                return Ok(end);
            }
            if from == self.first_frame {
                return Ok(0);
            }
            back *= 4;
        }
    }

    /// Makes the block hold sample `target` (< frames). False if it can't.
    fn locate(&mut self, target: u64) -> io::Result<bool> {
        let in_block = |s: &Self| s.block_len > 0 && target >= s.block_start && target < s.block_start + s.block_len as u64;
        if in_block(self) {
            return Ok(true);
        }
        let max_block = self.si.max_block.max(4096) as u64;
        let ahead =
            self.block_len > 0 && target >= self.block_start && target < self.block_start + self.block_len as u64 + 4 * max_block;
        if !ahead {
            // Jump: the best known point at or before the target.
            let mut lo = (self.first_frame, 0u64);
            if let Some(&(s, o)) = self.seek_points.iter().filter(|p| p.0 <= target).max_by_key(|p| p.0) {
                lo = (self.first_frame + o, s);
            }
            let mut hi = (self.win.len, self.info.frames.max(target + 1));
            // Bisect on frame headers while the range is large.
            while hi.0 - lo.0 > 1 << 16 {
                let span = (hi.1 - lo.1).max(1) as f64;
                let frac = ((target - lo.1) as f64 / span).clamp(0.0, 1.0);
                let guess = lo.0 + ((hi.0 - lo.0) as f64 * frac) as u64;
                let guess = guess.clamp(lo.0 + 1, hi.0 - (1 << 15)).saturating_sub(self.max_frame_bytes as u64 / 2).max(lo.0 + 1);
                match self.find_frame(guess)? {
                    Some(off) if off < hi.0 => {
                        if self.block_start <= target {
                            if in_block(self) {
                                return Ok(true);
                            }
                            lo = (off, self.block_start);
                        } else {
                            hi = (off, self.block_start);
                        }
                    }
                    _ => {
                        hi = (guess, hi.1);
                    }
                }
            }
            if !self.decode_at(lo.0)? && self.find_frame(lo.0)?.is_none() {
                return Ok(false);
            }
        }
        while !in_block(self) {
            if self.block_start > target || !self.decode_at(self.next_offset)? {
                return Ok(false);
            }
        }
        Ok(true)
    }
}

impl AudioReader for FlacReader {
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
        let mut pos = first;
        let bits = self.si.bits;
        let channels = self.info.channels;
        while pos < last {
            if !self.locate(pos as u64)? {
                break; // a damaged stretch reads as silence
            }
            let off = (pos as u64 - self.block_start) as usize;
            let count = (self.block_len - off).min((last - pos) as usize);
            let block = &self.block;
            fill_out(out, (pos - frame) as usize, channels, count, |i, ch| from_int(block[ch][off + i] as i32, bits));
            pos += count as i64;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::codec::test_util::*;
    use crate::codec::{create_writer, open_reader, EncodeFormat};
    use crate::wav::tests::temp_dir;
    use crate::wav::SampleFormat;

    /// Test material on the integer grid: tones, noise, silence, a constant
    /// stretch, full-scale steps.
    fn material(channels: usize, frames: usize, bits: u32, seed: u64) -> Vec<Vec<f32>> {
        let mut rng = Lcg(seed);
        let scale = (1i64 << (bits - 1)) as f64;
        (0..channels)
            .map(|c| {
                (0..frames)
                    .map(|i| {
                        let t = i as f64 / 48000.0;
                        let v = match (i / 5000) % 5 {
                            0 => (t * 440.0 * (c + 1) as f64 * std::f64::consts::TAU).sin() * 0.8,
                            1 => rng.next_f32() as f64 * 0.9,
                            2 => 0.0,
                            3 => 0.25,
                            _ => {
                                if (i / 37) % 2 == 0 {
                                    1.0
                                } else {
                                    -1.0
                                }
                            }
                        };
                        ((v * scale).round().clamp(-scale, scale - 1.0) / scale) as f32
                    })
                    .collect()
            })
            .collect()
    }

    fn read_all(path: &Path) -> (AudioInfo, Vec<Vec<f32>>) {
        let mut r = open_reader(path).unwrap();
        let info = *r.info();
        let mut chans = vec![vec![0.0; info.frames as usize]; info.channels];
        let mut refs: Vec<&mut [f32]> = chans.iter_mut().map(|c| &mut c[..]).collect();
        r.read(0, &mut refs).unwrap();
        (info, chans)
    }

    fn streaminfo_md5(path: &Path) -> [u8; 16] {
        let b = std::fs::read(path).unwrap();
        b[8 + 18..8 + 34].try_into().unwrap()
    }

    fn expected_md5(chans: &[Vec<f32>], bits: u32) -> [u8; 16] {
        let mut m = Md5::new();
        for i in 0..chans[0].len() {
            for c in chans {
                m.update(&to_int(c[i], bits).to_le_bytes()[..bits as usize / 8]);
            }
        }
        m.finish()
    }

    #[test]
    fn utf8_numbers() {
        for v in [
            0u64,
            0x7f,
            0x80,
            0x7ff,
            0x800,
            0xffff,
            0x10000,
            0x1f_ffff,
            0x20_0000,
            0x3ff_ffff,
            0x400_0000,
            0x7fff_ffff,
            0x8000_0000,
            0xf_ffff_ffff,
        ] {
            let mut w = BitWriter::new();
            w.put(0xfff8, 16);
            w.put(0xc, 4);
            w.put(10, 4);
            w.put(1, 4);
            w.put(4, 3);
            w.put(0, 1);
            put_utf8(&mut w, v);
            let crc = crc8(&w.bytes);
            w.put(u32::from(crc), 8);
            let si = StreamInfo { min_block: 4096, max_block: 4096, bits: 16, channels: 2, ..Default::default() };
            let h = parse_header(&w.bytes, &si).expect("header parses");
            assert_eq!(h.first_sample, v * 4096, "{v:#x}");
            assert_eq!(h.len, w.bytes.len());
        }
    }

    #[test]
    fn round_trips_bit_exact_and_external_decoder_agrees() {
        let dir = temp_dir("flac-rt");
        let ffmpeg = have_ffmpeg();
        for bits in [16u32, 24] {
            for channels in [1usize, 2, 4] {
                let frames = 3 * BLOCK_SIZE * 2 + 1234 + 25000; // several blocks and a short last one
                let mut chans = material(channels, frames, bits, bits as u64 + channels as u64);
                let path = dir.join(format!("t-{bits}-{channels}.flac"));
                let sample = if bits == 16 { SampleFormat::Int16 } else { SampleFormat::Int24 };
                let mut w = create_writer(&path, channels, 48000, EncodeFormat::new(FileFormat::Flac, sample, 0)).unwrap();
                // Odd write sizes cross the block boundaries.
                let mut at = 0;
                let mut step = 1000;
                while at < frames {
                    let k = step.min(frames - at);
                    if at > 20000 && at < 21500 {
                        w.write_silence(k).unwrap();
                        for c in &mut chans {
                            c[at..at + k].fill(0.0);
                        }
                    } else {
                        let refs: Vec<&[f32]> = chans.iter().map(|c| &c[at..at + k]).collect();
                        w.write_planar(&refs, k).unwrap();
                    }
                    w.update_header().unwrap();
                    at += k;
                    step = step * 3 / 2 + 7;
                }
                assert_eq!(w.finalize().unwrap(), frames as u64);
                let (info, back) = read_all(&path);
                assert_eq!(
                    (info.format, info.channels, info.sample_rate, info.frames, info.bits),
                    (FileFormat::Flac, channels, 48000, frames as u64, bits as u16)
                );
                assert!(back == chans, "FLAC {bits} bits x{channels} decodes bit-exact");
                assert_eq!(streaminfo_md5(&path), expected_md5(&chans, bits), "STREAMINFO MD5");
                let size = std::fs::metadata(&path).unwrap().len() as f64;
                let raw = (frames * channels * bits as usize / 8) as f64;
                assert!(size < raw * 0.9, "compresses: {size} of {raw}");

                if ffmpeg {
                    let fmt = if bits == 16 { "s16le" } else { "s32le" };
                    let (pcm, err) = ffmpeg_decode_raw(&path, fmt).expect("ffmpeg decodes");
                    assert!(err.trim().is_empty(), "ffmpeg reports: {err}");
                    let width = if bits == 16 { 2 } else { 4 };
                    assert_eq!(pcm.len(), frames * channels * width);
                    for (i, s) in pcm.chunks_exact(width).enumerate() {
                        let v = if bits == 16 {
                            i32::from(i16::from_le_bytes([s[0], s[1]]))
                        } else {
                            i32::from_le_bytes([s[0], s[1], s[2], s[3]]) >> 8
                        };
                        let want = to_int(chans[i % channels][i / channels], bits);
                        assert_eq!(v, want, "ffmpeg sample {i} ({bits} bits x{channels})");
                    }
                }
            }
        }
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn random_access_and_a_file_cut_short() {
        let dir = temp_dir("flac-seek");
        let path = dir.join("seek.flac");
        let frames = 200_000;
        let chans = material(2, frames, 24, 99);
        let mut w = FlacWriter::create(&path, 2, 44100, 24).unwrap();
        let refs: Vec<&[f32]> = chans.iter().map(|c| &c[..]).collect();
        AudioWriter::write_planar(&mut w, &refs, frames).unwrap();
        AudioWriter::update_header(&mut w).unwrap(); // flushed, never finalized
        let complete = frames / BLOCK_SIZE * BLOCK_SIZE;
        // Truncate in the middle of the last complete frame's successor: decodes up to the frame before.
        drop(w);
        {
            let mut r = FlacReader::open(&path).unwrap();
            assert_eq!(r.info().frames, complete as u64, "length measured from the frames");
            for &start in &[150_000i64, 3, 100_001, 4095, 4096, 190_000, 0, complete as i64 - 10, -5] {
                let mut a = vec![0.0f32; 700];
                let mut b = vec![0.0f32; 700];
                r.read(start, &mut [&mut a[..], &mut b[..]]).unwrap();
                for i in 0..700 {
                    let k = start + i as i64;
                    let want =
                        if k < 0 || k >= complete as i64 { (0.0, 0.0) } else { (chans[0][k as usize], chans[1][k as usize]) };
                    assert_eq!((a[i], b[i]), want, "frame {k}");
                }
            }
        }
        let len = std::fs::metadata(&path).unwrap().len();
        let f = std::fs::OpenOptions::new().write(true).open(&path).unwrap();
        f.set_len(len - 100).unwrap();
        drop(f);
        let r = FlacReader::open(&path).unwrap();
        assert_eq!(r.info().frames, complete as u64 - BLOCK_SIZE as u64, "a torn last frame is left out");
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn decodes_files_other_encoders_write() {
        if !have_ffmpeg() {
            return;
        }
        let dir = temp_dir("flac-ext");
        let mut rng = Lcg(5);
        let frames = 30000;
        let pcm: Vec<i16> = (0..frames * 2)
            .map(|i| {
                let t = (i / 2) as f64 / 44100.0;
                ((t * 330.0 * std::f64::consts::TAU).sin() * 12000.0 + rng.next_f32() as f64 * 3000.0 * (i % 2) as f64) as i16
            })
            .collect();
        for (k, args) in [
            &["-compression_level", "0"][..],
            &["-compression_level", "8"],
            &["-compression_level", "12", "-lpc_type", "cholesky"],
            &["-compression_level", "5", "-ch_mode", "mid_side"],
            &["-compression_level", "5", "-frame_size", "1152"],
        ]
        .iter()
        .enumerate()
        {
            let path = dir.join(format!("ext{k}.flac"));
            assert!(ffmpeg_encode(&pcm, 2, 44100, args, &path), "ffmpeg encodes");
            let (info, back) = read_all(&path);
            assert_eq!((info.channels, info.frames, info.bits), (2, frames as u64, 16));
            for i in 0..frames {
                assert_eq!(back[0][i], f32::from(pcm[2 * i]) / 32768.0, "{args:?} L {i}");
                assert_eq!(back[1][i], f32::from(pcm[2 * i + 1]) / 32768.0, "{args:?} R {i}");
            }
            // Seeking in someone else's stream.
            let mut r = open_reader(&path).unwrap();
            let mut a = vec![0.0f32; 300];
            r.read(20000, &mut [&mut a[..]]).unwrap();
            assert_eq!(a[0], f32::from(pcm[40000]) / 32768.0);
        }
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn a_damaged_frame_after_a_good_one_reads_as_silence() {
        // A file whose second frame header claims 192 samples (with a valid
        // header CRC) but whose body is the original 4096-sample frame: the
        // frame fails to decode. Reading the first frame again after trying
        // the second must still work, not index past the decoded block.
        let dir = temp_dir("flac-damaged-frame");
        let path = dir.join("a.flac");
        let frames = 3 * BLOCK_SIZE;
        let ch: Vec<Vec<f32>> =
            (0..2).map(|c| (0..frames).map(|k| ((k * (c + 1)) as f32 * 0.01).sin() * 0.5).collect()).collect();
        let mut w = create_writer(&path, 2, 48000, EncodeFormat::new(FileFormat::Flac, SampleFormat::Int16, 0)).unwrap();
        w.write_planar(&[&ch[0], &ch[1]], frames).unwrap();
        w.finalize().unwrap();

        let mut r = FlacReader::open(&path).unwrap();
        let mut a = [0.0f32; 16];
        r.read(0, &mut [&mut a[..]]).unwrap();
        let second = r.next_offset as usize;
        drop(r);
        let mut bytes = std::fs::read(&path).unwrap();
        assert_eq!(&bytes[second..second + 2], &[0xff, 0xf8]);
        bytes[second + 2] = (1 << 4) | (bytes[second + 2] & 15); // 192 samples
        bytes[second + 5] = crc8(&bytes[second..second + 5]);
        std::fs::write(&path, &bytes).unwrap();

        let mut r = FlacReader::open(&path).unwrap();
        let (mut a, mut b) = (vec![0.0f32; BLOCK_SIZE], vec![0.0f32; BLOCK_SIZE]);
        r.read(0, &mut [&mut a[..16], &mut b[..16]]).unwrap();
        r.read(BLOCK_SIZE as i64, &mut [&mut a[..16], &mut b[..16]]).unwrap();
        assert!(a[..16].iter().all(|&x| x == 0.0), "the damaged frame is silent");
        r.read(0, &mut [&mut a[..], &mut b[..]]).unwrap();
        assert_eq!(a[BLOCK_SIZE - 1], (ch[0][BLOCK_SIZE - 1] * 32768.0).round() / 32768.0);
        let _ = std::fs::remove_dir_all(dir);
    }
}
