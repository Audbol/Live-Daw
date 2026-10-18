//! A straightforward MPEG-1 Layer III encoder (CBR, long blocks):
//!
//! 1. the polyphase analysis filterbank (ISO/IEC 11172-3 C.1.3; the
//!    analysis window is the synthesis window / 32),
//! 2. an 18-line MDCT per subband (sine window, 50 % overlap) and the
//!    encoder side of the alias reduction butterflies,
//! 3. per granule and channel a simple masking model: band energies spread
//!    over neighbouring bands (-15 dB per band upwards, -25 dB downwards)
//!    lowered by a fixed signal-to-mask ratio, floored by the threshold of
//!    hearing - the noise each scale factor band may carry,
//! 4. the two nested loops: the inner one finds the finest global gain whose
//!    Huffman coding fits the bits of the granule, the outer one amplifies
//!    (scale factors) the bands whose noise exceeds what they may carry, and
//!    keeps the best result,
//! 5. Huffman coding with the standard tables: big values in three regions
//!    (boundaries and tables chosen by exhaustive search), count1
//!    quadruples with the cheaper of tables A and B,
//! 6. mid/side stereo when the channels are similar (joint stereo frames).
//!
//! Frames don't use the bit reservoir (each frame's data is in the frame);
//! within a frame, bits the first granule doesn't need go to the second.

use super::tables::*;
use crate::codec::bits::BitWriter;

/// Samples of encoder plus decoder delay: output sample `k` of a standard
/// decoder is input sample `k - TOTAL_DELAY` (the polyphase filterbank
/// pair, 481, plus one granule of MDCT overlap).
pub const TOTAL_DELAY: usize = 481 + 576;
/// The part of the delay a standard decoder adds (the LAME tag convention).
pub const DECODER_DELAY: usize = 529;

const MAX_IX: i32 = 8191 + 15;
/// Signal-to-mask ratio of the masking model (dB).
const SMR_DB: f64 = 16.0;

/// The polyphase analysis filterbank of one channel.
pub(crate) struct Analysis {
    x: [f32; 512],
    pos: usize,
}

impl Analysis {
    pub(crate) fn new() -> Self {
        Self { x: [0.0; 512], pos: 0 }
    }

    /// 32 new samples in (oldest first), 32 subband samples out.
    pub(crate) fn run(&mut self, input: &[f32], window: &[f32; 512], matrix: &[f32], out: &mut [f32; 32]) {
        // X[0] is the newest sample: X[i] = X[i - 32], X[31 - j] = input[j].
        self.pos = (self.pos + 512 - 32) & 511;
        for (j, &s) in input.iter().enumerate().take(32) {
            self.x[(self.pos + 31 - j) & 511] = s;
        }
        let mut y = [0f32; 64];
        for (i, yi) in y.iter_mut().enumerate() {
            let mut acc = 0.0;
            for j in 0..8 {
                let n = i + 64 * j;
                acc += window[n] * self.x[(self.pos + n) & 511];
            }
            *yi = acc;
        }
        for (k, o) in out.iter_mut().enumerate() {
            let row = &matrix[k * 64..k * 64 + 64];
            let mut acc = 0.0;
            for i in 0..64 {
                acc += row[i] * y[i];
            }
            *o = acc;
        }
    }
}

/// The coding of one granule / channel.
#[derive(Clone)]
struct Coded {
    ix: Box<[i32; 576]>,
    sf: [i32; 21],
    global_gain: i32,
    scalefac_compress: usize,
    layout: Layout,
    part2_bits: usize,
}

#[derive(Clone, Copy, Debug, Default)]
struct Layout {
    bits: usize,
    big_values: usize,
    /// Lines up to here are coded (count1 region end).
    count1_end: usize,
    count1_table: bool,
    table_select: [usize; 3],
    region0_count: usize,
    region1_count: usize,
}

/// Big-value tables the encoder considers (fixed-size ones), with their largest value.
const SMALL_TABLES: [(usize, i32); 13] =
    [(1, 1), (2, 2), (3, 2), (5, 3), (6, 3), (7, 5), (8, 5), (9, 5), (10, 7), (11, 7), (12, 7), (13, 15), (15, 15)];

pub struct Encoder {
    channels: usize,
    sr_index: usize,
    br_index: usize,
    sample_rate: u32,
    kbps: u32,
    analysis: [Analysis; 2],
    /// Subband samples of the previous granule per channel ([sb][t]).
    prev: Box<[[[f32; 18]; 32]; 2]>,
    pub(crate) window: [f32; 512],
    pub(crate) matrix: Vec<f32>,
    /// Allowed noise floor per long band (threshold of hearing).
    ath: [f64; 22],
    frac: u32,
    bw: BitWriter,
    xr: Box<[[[f32; 576]; 2]; 2]>,
    cum: Vec<Vec<u32>>,
}

/// The MDCT gain that makes the standard decoder reconstruct the input.
const MDCT_SCALE: f32 = 1.0 / 9.0;
/// Energy of the coefficients of a full-scale sine per granule (calibration
/// of the threshold of hearing).
const FULL_SCALE_ENERGY: f64 = 576.0 / 2.0 / 9.0;

impl Encoder {
    /// `sample_rate` one of 32, 44.1, 48 kHz; `kbps` one of the Layer III rates.
    pub fn new(channels: usize, sample_rate: u32, kbps: u32) -> Option<Self> {
        let sr_index = SAMPLE_RATES.iter().position(|&r| r == sample_rate)?;
        let br_index = BITRATES.iter().position(|&b| b == kbps).filter(|&i| i > 0)?;
        if !(1..=2).contains(&channels) {
            return None;
        }
        let n = numeric();
        let window = std::array::from_fn(|i| n.synth_window[i] / 32.0);
        let matrix = (0..32)
            .flat_map(|k| {
                (0..64).map(move |i| ((2.0 * k as f64 + 1.0) * (i as f64 - 16.0) * std::f64::consts::PI / 64.0).cos() as f32)
            })
            .collect();
        let sfb = &SFB_LONG[sr_index];
        let ath = std::array::from_fn(|b| {
            let lo = sfb[b];
            let hi = sfb[b + 1];
            // The quietest point of the band (centre frequencies of its lines).
            let mut min_db = f64::MAX;
            for line in lo..hi {
                let f = (line as f64 + 0.5) * f64::from(sample_rate) / 2.0 / 576.0;
                min_db = min_db.min(ath_dbfs(f));
            }
            FULL_SCALE_ENERGY * 10f64.powf(min_db / 10.0) * (hi - lo) as f64 / 576.0 * 2.0
        });
        Some(Self {
            channels,
            sr_index,
            br_index,
            sample_rate,
            kbps,
            analysis: [Analysis::new(), Analysis::new()],
            prev: Box::new([[[0.0; 18]; 32]; 2]),
            window,
            matrix,
            ath,
            frac: 0,
            bw: BitWriter::new(),
            xr: Box::new([[[0.0; 576]; 2]; 2]),
            cum: Vec::new(),
        })
    }

    pub fn channels(&self) -> usize {
        self.channels
    }

    fn side_info_len(&self) -> usize {
        if self.channels == 1 {
            17
        } else {
            32
        }
    }

    fn header(&self, padding: bool, mode: u32, mode_ext: u32) -> u32 {
        0xfffb_0000
            | (self.br_index as u32) << 12
            | (self.sr_index as u32) << 10
            | u32::from(padding) << 9
            | mode << 6
            | mode_ext << 4
            | 1 << 2
    }

    fn mode(&self) -> u32 {
        if self.channels == 1 {
            3
        } else {
            1
        }
    }

    /// Frame length without padding.
    pub fn frame_len(&self) -> usize {
        (144_000 * self.kbps / self.sample_rate) as usize
    }

    /// The `Info` frame (a silent frame carrying the frame count, the byte
    /// count and the encoder delay / padding in the LAME tag layout), with
    /// the frame count still 0 when written at the start.
    pub fn info_frame(&self, frames: u32, bytes: u32, delay: u32, padding: u32) -> Vec<u8> {
        let len = self.frame_len();
        let mut f = vec![0u8; len];
        f[..4].copy_from_slice(&self.header(false, self.mode(), 0).to_be_bytes());
        let mut at = 4 + self.side_info_len();
        f[at..at + 4].copy_from_slice(b"Info");
        f[at + 4..at + 8].copy_from_slice(&0x0fu32.to_be_bytes()); // frames, bytes, TOC, quality
        f[at + 8..at + 12].copy_from_slice(&frames.to_be_bytes());
        f[at + 12..at + 16].copy_from_slice(&bytes.to_be_bytes());
        for i in 0..100 {
            f[at + 16 + i] = (i * 256 / 100) as u8;
        }
        // quality 0 (4 bytes), then the extension.
        at += 120;
        f[at..at + 9].copy_from_slice(b"LiveDAW  ");
        f[at + 9] = 0x01; // revision 0, CBR
        f[at + 10] = (self.sample_rate / 2 / 100).min(255) as u8;
        f[at + 20] = self.kbps.min(255) as u8;
        let dp = (delay.min(4095) << 12) | padding.min(4095);
        f[at + 21..at + 24].copy_from_slice(&dp.to_be_bytes()[1..]);
        f[at + 28..at + 32].copy_from_slice(&bytes.to_be_bytes());
        // CRC-16 (reflected 0x8005) of the first 190 bytes.
        let crc = crc16_arc(&f[..190.min(at + 34)]);
        f[at + 34..at + 36].copy_from_slice(&crc.to_be_bytes());
        f
    }

    /// Encodes 1152 samples per channel (`pcm[c]`, full scale = 1.0) into
    /// one frame.
    pub fn encode(&mut self, pcm: &[&[f32]]) -> &[u8] {
        let n = numeric();
        let channels = self.channels;
        // Filterbank and MDCT.
        for ch in 0..channels {
            let mut sub = [[[0f32; 18]; 32]; 2];
            let mut s = [0f32; 32];
            for slot in 0..36 {
                let input = &pcm[ch][slot * 32..slot * 32 + 32];
                self.analysis[ch].run(input, &self.window, &self.matrix, &mut s);
                for sb in 0..32 {
                    sub[slot / 18][sb][slot % 18] = s[sb];
                }
            }
            for gr in 0..2 {
                let xr = &mut self.xr[gr][ch];
                for sb in 0..32 {
                    let mut z = [0f32; 36];
                    z[..18].copy_from_slice(&self.prev[ch][sb]);
                    z[18..].copy_from_slice(&sub[gr][sb]);
                    if sb % 2 == 1 {
                        for i in (1..36).step_by(2) {
                            z[i] = -z[i];
                        }
                    }
                    let w = &n.windows[0];
                    for k in 0..18 {
                        let mut acc = 0.0f32;
                        for i in 0..36 {
                            acc += z[i] * w[i] * n.imdct_long[i * 18 + k];
                        }
                        xr[sb * 18 + k] = acc * MDCT_SCALE;
                    }
                    self.prev[ch][sb] = sub[gr][sb];
                }
                // Alias reduction (encoder side: the transposed butterflies).
                for sb in 1..32 {
                    for i in 0..8 {
                        let bu = xr[sb * 18 - 1 - i];
                        let bd = xr[sb * 18 + i];
                        xr[sb * 18 - 1 - i] = bu * n.cs[i] + bd * n.ca[i];
                        xr[sb * 18 + i] = bd * n.cs[i] - bu * n.ca[i];
                    }
                }
            }
        }
        // Mid/side when the channels are similar.
        let mut ms = false;
        if channels == 2 {
            let (mut em, mut es) = (0.0f64, 0.0f64);
            for gr in 0..2 {
                for k in 0..576 {
                    let (l, r) = (f64::from(self.xr[gr][0][k]), f64::from(self.xr[gr][1][k]));
                    em += (l + r) * (l + r);
                    es += (l - r) * (l - r);
                }
            }
            ms = es < 0.3 * em;
            if ms {
                let inv = std::f32::consts::FRAC_1_SQRT_2;
                for gr in 0..2 {
                    for k in 0..576 {
                        let (l, r) = (self.xr[gr][0][k], self.xr[gr][1][k]);
                        self.xr[gr][0][k] = (l + r) * inv;
                        self.xr[gr][1][k] = (l - r) * inv;
                    }
                }
            }
        }

        // Bits.
        let rest = 144_000 * self.kbps % self.sample_rate;
        self.frac += rest;
        let padding = self.frac >= self.sample_rate;
        if padding {
            self.frac -= self.sample_rate;
        }
        let len = self.frame_len() + usize::from(padding);
        let main_bits = (len - 4 - self.side_info_len()) * 8;
        let mut coded: Vec<Coded> = Vec::with_capacity(4);
        let mut used = 0;
        for gr in 0..2 {
            for ch in 0..channels {
                let left = 2 * channels - coded.len();
                let budget = ((main_bits - used) / left).min(4095);
                let xr = self.xr[gr][ch];
                let c = self.code_granule(&xr, budget);
                used += c.part2_bits + c.layout.bits;
                coded.push(c);
            }
        }

        // The frame.
        let mode_ext = if ms { 2 } else { 0 };
        let header = self.header(padding, self.mode(), mode_ext);
        let w = &mut self.bw;
        w.clear();
        w.put(header, 32);
        w.put(0, 9); // main_data_begin
        w.put(0, if channels == 1 { 5 } else { 3 });
        w.put(0, 4 * channels as u32); // scfsi
        for c in &coded {
            let l = &c.layout;
            w.put((c.part2_bits + l.bits) as u32, 12);
            w.put(l.big_values as u32, 9);
            w.put(c.global_gain as u32, 8);
            w.put(c.scalefac_compress as u32, 4);
            w.put(0, 1); // no window switching
            for &t in &l.table_select {
                w.put(t as u32, 5);
            }
            w.put(l.region0_count as u32, 4);
            w.put(l.region1_count as u32, 3);
            w.put(0, 1); // preflag
            w.put(0, 1); // scalefac_scale
            w.put(u32::from(l.count1_table), 1);
        }
        let sfb = &SFB_LONG[self.sr_index];
        for c in &coded {
            let (slen1, slen2) = SLEN[c.scalefac_compress];
            for b in 0..21 {
                w.put(c.sf[b] as u32, if b < 11 { slen1 } else { slen2 });
            }
            write_huffman(w, &c.ix, &c.layout, sfb);
        }
        debug_assert!(w.bit_len() <= len * 8);
        w.align();
        w.bytes.resize(len, 0);
        &self.bw.bytes
    }

    /// The outer loop for one granule / channel.
    fn code_granule(&mut self, xr: &[f32; 576], budget: usize) -> Coded {
        let sfb = SFB_LONG[self.sr_index];
        let mut energy = [0f64; 22];
        for b in 0..22 {
            energy[b] = xr[sfb[b]..sfb[b + 1]].iter().map(|&v| f64::from(v) * f64::from(v)).sum();
        }
        // Allowed noise per band.
        let mut xmin = [0f64; 22];
        let smr = 10f64.powf(-SMR_DB / 10.0);
        for b in 0..22 {
            let mut spread = 0.0;
            for (j, &e) in energy.iter().enumerate() {
                spread += e * spreading()[b + 21 - j];
            }
            // Spread energy is relative to the band's share of lines.
            xmin[b] = (spread * smr).min(energy[b].max(1e-30) * 1.0).max(self.ath[b]);
        }
        let xr34: Vec<f32> = xr.iter().map(|&v| v.abs().powf(0.75)).collect();
        let max34 = xr34.iter().fold(0f32, |m, &v| m.max(v));

        let mut sf = [0i32; 21];
        let mut best: Option<(f64, Coded)> = None;
        let mut gg_hint = 210;
        let mut stale = 0;
        for _iter in 0..8 {
            let Some((compress, part2)) = scalefac_compress_for(&sf) else { break };
            if part2 >= budget {
                break;
            }
            let (gg, ix, layout) = self.inner_loop(&xr34, max34, &sf, budget - part2, gg_hint);
            gg_hint = gg;
            // Noise per band.
            let n = numeric();
            let mut over = 0usize;
            let mut over_db = 0.0f64;
            let mut amplify = [false; 21];
            for b in 0..22 {
                let s = if b < 21 { sf[b] } else { 0 };
                let step = (0.25 * (f64::from(gg) - 210.0) - 0.5 * f64::from(s)).exp2();
                let mut noise = 0.0f64;
                for k in sfb[b]..sfb[b + 1] {
                    let q = f64::from(n.pow43[ix[k].unsigned_abs() as usize]) * step;
                    let d = f64::from(xr[k].abs()) - q;
                    noise += d * d;
                }
                if noise > xmin[b] {
                    over += 1;
                    over_db += 10.0 * (noise / xmin[b]).log10();
                    if b < 21 {
                        amplify[b] = true;
                    }
                }
            }
            let score = over_db;
            let candidate = Coded { ix, sf, global_gain: gg, scalefac_compress: compress, layout, part2_bits: part2 };
            if best.as_ref().is_none_or(|(s, _)| score < *s) {
                best = Some((score, candidate));
                stale = 0;
            } else {
                stale += 1;
                if stale >= 2 {
                    break; // amplifying stopped helping
                }
            }
            if over == 0 || amplify.iter().all(|&a| a) {
                break;
            }
            let mut next = sf;
            let mut ok = true;
            for b in 0..21 {
                if amplify[b] {
                    next[b] += 1;
                    if next[b] > if b < 11 { 15 } else { 7 } {
                        ok = false;
                    }
                }
            }
            if !ok {
                break;
            }
            sf = next;
        }
        match best {
            Some((_, mut c)) => {
                // Quantization worked on magnitudes: the signs.
                for (v, &x) in c.ix.iter_mut().zip(xr.iter()) {
                    if x < 0.0 {
                        *v = -*v;
                    }
                }
                c
            }
            None => {
                // Not even the scale factors fit: silence.
                Coded {
                    ix: Box::new([0; 576]),
                    sf: [0; 21],
                    global_gain: 210,
                    scalefac_compress: 0,
                    layout: Layout::default(),
                    part2_bits: 0,
                }
            }
        }
    }

    /// The finest global gain whose coding fits `bits`.
    fn inner_loop(&mut self, xr34: &[f32], max34: f32, sf: &[i32; 21], bits: usize, hint: i32) -> (i32, Box<[i32; 576]>, Layout) {
        let sfb = SFB_LONG[self.sr_index];
        let mut ix = Box::new([0i32; 576]);
        let quantize = |gg: i32, ix: &mut [i32; 576]| {
            for b in 0..22 {
                let s = if b < 21 { sf[b] } else { 0 };
                // (|xr| / step)^(3/4) with step = 2^((gg - 210) / 4 - s / 2)
                let k = (-0.1875 * (f64::from(gg) - 210.0) + 0.375 * f64::from(s)).exp2() as f32;
                for i in sfb[b]..sfb[b + 1] {
                    ix[i] = (xr34[i] * k + 0.4054) as i32;
                }
            }
        };
        // The smallest gain keeping every value codable.
        let max_sf = sf.iter().copied().max().unwrap_or(0);
        let mut lo = 0i32;
        while lo < 255
            && f64::from(max34) * (-0.1875 * (f64::from(lo) - 210.0) + 0.375 * f64::from(max_sf)).exp2() + 0.4054
                >= f64::from(MAX_IX)
        {
            lo += 1;
        }
        let mut hi = 255i32;
        // Find the smallest gain in [lo, hi] that fits (bits fall as the gain rises).
        let fits = |gg: i32, ix: &mut [i32; 576], cum: &mut Vec<Vec<u32>>| -> Option<Layout> {
            quantize(gg, ix);
            let l = layout(ix, &sfb, cum);
            (l.bits <= bits).then_some(l)
        };
        let mut cum = std::mem::take(&mut self.cum);
        let mut best: Option<(i32, Layout)> = None;
        // Gallop from the hint (the previous answer) to bracket the gain, then bisect.
        let start = hint.clamp(lo, hi);
        if let Some(l) = fits(start, &mut ix, &mut cum) {
            best = Some((start, l));
            let (mut upper, mut step) = (start, 1);
            loop {
                let g = upper - step;
                if g < lo {
                    hi = upper - 1;
                    break;
                }
                match fits(g, &mut ix, &mut cum) {
                    Some(l) => {
                        best = Some((g, l));
                        upper = g;
                        step *= 2;
                    }
                    None => {
                        lo = g + 1;
                        hi = upper - 1;
                        break;
                    }
                }
            }
        } else {
            let (mut lower, mut step) = (start, 1);
            loop {
                let g = lower + step;
                if g > 255 {
                    lo = lower + 1;
                    break;
                }
                match fits(g, &mut ix, &mut cum) {
                    Some(l) => {
                        best = Some((g, l));
                        lo = lower + 1;
                        hi = g - 1;
                        break;
                    }
                    None => {
                        lower = g;
                        step *= 2;
                    }
                }
            }
        }
        while lo <= hi {
            let mid = (lo + hi) / 2;
            if let Some(l) = fits(mid, &mut ix, &mut cum) {
                best = Some((mid, l));
                hi = mid - 1;
            } else {
                lo = mid + 1;
            }
        }
        let (gg, l) = match best {
            Some(b) => b,
            None => {
                ix.fill(0);
                (255, layout(&ix, &sfb, &mut cum))
            }
        };
        quantize(gg, &mut ix);
        self.cum = cum;
        (gg, ix, l)
    }
}

/// Masking spread by band distance (index 21 + d for a masker d bands
/// below): -15 dB per band upwards, -25 dB per band downwards.
fn spreading() -> &'static [f64; 43] {
    static S: std::sync::OnceLock<[f64; 43]> = std::sync::OnceLock::new();
    S.get_or_init(|| {
        std::array::from_fn(|i| {
            let d = i as f64 - 21.0;
            if d >= 0.0 {
                10f64.powf(-1.5 * d)
            } else {
                10f64.powf(2.5 * d)
            }
        })
    })
}

/// The threshold of hearing in dBFS (Terhardt's approximation, with 0 dB SPL
/// placed at -110 dBFS and capped at -10 dBFS).
fn ath_dbfs(f: f64) -> f64 {
    let khz = (f / 1000.0).max(0.02);
    let spl = 3.64 * khz.powf(-0.8) - 6.5 * (-0.6 * (khz - 3.3) * (khz - 3.3)).exp() + 1e-3 * khz.powi(4);
    (spl - 110.0).min(-10.0)
}

fn crc16_arc(data: &[u8]) -> u16 {
    let mut crc = 0u16;
    for &b in data {
        crc ^= u16::from(b);
        for _ in 0..8 {
            crc = if crc & 1 != 0 { (crc >> 1) ^ 0xa001 } else { crc >> 1 };
        }
    }
    crc
}

/// The cheapest `scalefac_compress` for long-block scale factors `sf` and its bits.
fn scalefac_compress_for(sf: &[i32; 21]) -> Option<(usize, usize)> {
    let max1 = sf[..11].iter().copied().max().unwrap_or(0);
    let max2 = sf[11..].iter().copied().max().unwrap_or(0);
    let mut best: Option<(usize, usize)> = None;
    for (i, &(s1, s2)) in SLEN.iter().enumerate() {
        if max1 < (1 << s1) && max2 < (1 << s2) {
            let bits = 11 * s1 as usize + 10 * s2 as usize;
            if best.is_none_or(|(_, b)| bits < b) {
                best = Some((i, bits));
            }
        }
    }
    best
}

/// What coding a pair costs in a big-value table.
struct TableCost {
    table: usize,
    /// The largest value it codes.
    limit: i32,
    linbits: u32,
    /// Code length plus sign bits for (min(x, 15), min(y, 15)), 16 x 16.
    cost: [u8; 256],
}

fn table_costs() -> &'static [TableCost] {
    static T: std::sync::OnceLock<Vec<TableCost>> = std::sync::OnceLock::new();
    T.get_or_init(|| {
        let mut out = Vec::new();
        let mut add = |t: usize| {
            let Some(src) = huff_source(t) else { return };
            let linbits = LINBITS[t];
            let limit = if linbits > 0 { 15 + (1 << linbits) - 1 } else { src.width as i32 - 1 };
            let mut cost = [0u8; 256];
            for x in 0..16usize {
                for y in 0..16usize {
                    if x < src.width && y < src.width {
                        cost[x * 16 + y] = src.lens[x * src.width + y] + u8::from(x != 0) + u8::from(y != 0);
                    }
                }
            }
            out.push(TableCost { table: t, limit, linbits, cost });
        };
        for &(t, _) in &SMALL_TABLES {
            add(t);
        }
        for t in 16..32 {
            add(t);
        }
        out
    })
}

/// Region boundaries, tables and the bit count of a quantized granule.
fn layout(ix: &[i32; 576], sfb: &[usize; 23], cum: &mut Vec<Vec<u32>>) -> Layout {
    let mut rz = 576;
    while rz >= 2 && ix[rz - 1] == 0 && ix[rz - 2] == 0 {
        rz -= 2;
    }
    let mut c1 = rz;
    while c1 >= 4 && ix[c1 - 4..c1].iter().all(|v| v.abs() <= 1) {
        c1 -= 4;
    }
    // count1 bits with tables A and B.
    let (_, lens_a) = count1_a();
    let (mut bits_a, mut bits_b) = (0usize, 0usize);
    for q in ix[c1..rz].chunks_exact(4) {
        let idx = (q[0].abs() << 3 | q[1].abs() << 2 | q[2].abs() << 1 | q[3].abs()) as usize;
        let signs = q.iter().filter(|&&v| v != 0).count();
        bits_a += usize::from(lens_a[idx]) + signs;
        bits_b += 4 + signs;
    }
    let count1_table = bits_b < bits_a;
    let count1_bits = bits_a.min(bits_b);

    let pairs = c1 / 2;
    let mut out = Layout { big_values: pairs, count1_end: rz, count1_table, ..Default::default() };
    if pairs == 0 {
        out.bits = count1_bits;
        return out;
    }
    // Band boundaries in pairs (clipped to the big values) and band maxima.
    let mut bp = [0usize; 23];
    for (j, b) in bp.iter_mut().enumerate() {
        *b = (sfb[j] / 2).min(pairs);
    }
    let mut bmax = [0i32; 22];
    for j in 0..22 {
        bmax[j] = ix[2 * bp[j]..2 * bp[j + 1]].iter().map(|v| v.abs()).max().unwrap_or(0);
    }
    let gmax = bmax.iter().copied().max().unwrap_or(0);
    // Candidate tables: the fixed-size ones, and per escape family the one
    // with the fewest extra bits reaching the largest value.
    let costs = table_costs();
    // Fixed-size tables of the two smallest sizes reaching each band's maximum.
    let mut wanted = [false; 16];
    for &m in bmax.iter().filter(|&&m| m > 0 && m <= 15) {
        let mut classes = 0;
        for limit in [1, 2, 3, 5, 7, 15] {
            if limit >= m && classes < 2 {
                wanted[limit as usize] = true;
                classes += 1;
            }
        }
    }
    let mut cands: Vec<&TableCost> = costs.iter().filter(|c| c.linbits == 0 && wanted[c.limit as usize]).collect();
    for family in [16usize, 24] {
        if let Some(c) = costs.iter().find(|c| c.table >= family && c.table < family + 8 && c.limit >= gmax) {
            cands.push(c);
        }
    }
    // Per pair: its index in the 16 x 16 grid and how many escapes it has.
    let mut grid = [0u8; 288];
    let mut escapes = [0u8; 288];
    for p in 0..pairs {
        let (x, y) = (ix[2 * p].unsigned_abs(), ix[2 * p + 1].unsigned_abs());
        grid[p] = (x.min(15) * 16 + y.min(15)) as u8;
        escapes[p] = u8::from(x >= 15) + u8::from(y >= 15);
    }
    // Cumulative bits per candidate (pairs a table can't code count anything;
    // the region maxima rule those out).
    cum.resize(cands.len(), Vec::new());
    for (ci, c) in cands.iter().enumerate() {
        let v = &mut cum[ci];
        v.resize(pairs + 1, 0);
        let mut acc = 0u32;
        if c.linbits == 0 {
            for p in 0..pairs {
                acc += u32::from(c.cost[usize::from(grid[p])]);
                v[p + 1] = acc;
            }
        } else {
            for p in 0..pairs {
                acc += u32::from(c.cost[usize::from(grid[p])]) + c.linbits * u32::from(escapes[p]);
                v[p + 1] = acc;
            }
        }
    }
    // Cost of the region between band boundaries i and j (memoized).
    let mut memo = [[(u32::MAX, 0usize); 23]; 23];
    let mut region = |i: usize, j: usize| -> (u32, usize) {
        if i >= j || bp[i] >= bp[j] {
            return (0, 0);
        }
        if memo[i][j].0 != u32::MAX {
            return memo[i][j];
        }
        let max = bmax[i..j].iter().copied().max().unwrap_or(0);
        let mut best = (0, 0);
        if max > 0 {
            best = (u32::MAX - 1, 0);
            for (ci, c) in cands.iter().enumerate() {
                if c.limit >= max {
                    let bits = cum[ci][bp[j]] - cum[ci][bp[i]];
                    if bits < best.0 {
                        best = (bits, c.table);
                    }
                }
            }
        }
        memo[i][j] = best;
        best
    };
    let mut best = (u32::MAX, [0usize; 3], 0usize, 0usize);
    for r0 in 0..16usize {
        let (c0, t0) = region(0, r0 + 1);
        for r1 in 0..8usize {
            let k = r0 + r1 + 2;
            if k > 22 {
                break;
            }
            let (cr1, t1) = region(r0 + 1, k);
            let (cr2, t2) = region(k, 22);
            let total = c0 + cr1 + cr2;
            if total < best.0 {
                best = (total, [t0, t1, t2], r0, r1);
            }
            if bp[k] >= pairs {
                break; // larger r1 change nothing
            }
        }
        if bp[r0 + 1] >= pairs {
            break;
        }
    }
    out.bits = best.0 as usize + count1_bits;
    out.table_select = best.1;
    out.region0_count = best.2;
    out.region1_count = best.3;
    out
}

fn write_huffman(w: &mut BitWriter, ix: &[i32; 576], l: &Layout, sfb: &[usize; 23]) {
    let big = l.big_values * 2;
    let r1 = sfb[(l.region0_count + 1).min(22)];
    let r2 = sfb[(l.region0_count + l.region1_count + 2).min(22)];
    let mut i = 0;
    while i < big {
        let t = if i < r1 {
            l.table_select[0]
        } else if i < r2 {
            l.table_select[1]
        } else {
            l.table_select[2]
        };
        if t == 0 {
            i += 2;
            continue;
        }
        let Some(src) = huff_source(t) else { return };
        let linbits = LINBITS[t];
        let (x, y) = (ix[i], ix[i + 1]);
        let (ax, ay) = (x.abs(), y.abs());
        let (xx, yy) = (ax.min(15) as usize, ay.min(15) as usize);
        let idx = xx * src.width + yy;
        w.put(u32::from(src.codes[idx]), u32::from(src.lens[idx]));
        if linbits > 0 && ax >= 15 {
            w.put((ax - 15) as u32, linbits);
        }
        if ax != 0 {
            w.put(u32::from(x < 0), 1);
        }
        if linbits > 0 && ay >= 15 {
            w.put((ay - 15) as u32, linbits);
        }
        if ay != 0 {
            w.put(u32::from(y < 0), 1);
        }
        i += 2;
    }
    let (codes_a, lens_a) = count1_a();
    for q in ix[big..l.count1_end].chunks_exact(4) {
        let idx = (q[0].abs() << 3 | q[1].abs() << 2 | q[2].abs() << 1 | q[3].abs()) as usize;
        if l.count1_table {
            w.put(15 - idx as u32, 4);
        } else {
            w.put(u32::from(codes_a[idx]), u32::from(lens_a[idx]));
        }
        for &v in q {
            if v != 0 {
                w.put(u32::from(v < 0), 1);
            }
        }
    }
}
