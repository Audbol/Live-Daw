//! An MPEG-1 Layer III decoder (ISO/IEC 11172-3 clause 2.4.3): side
//! information, the bit reservoir, scale factors (with scfsi), Huffman
//! decoding, requantization, mid/side and intensity stereo, short / mixed /
//! start / stop blocks (reordering, alias reduction, IMDCT, overlap-add),
//! frequency inversion and the polyphase synthesis filterbank.

use super::tables::*;
use crate::codec::bits::BitReader;

/// A parsed frame header (MPEG-1 Layer III only).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Header {
    pub crc: bool,
    pub bitrate_index: usize,
    pub sr_index: usize,
    pub padding: bool,
    /// 0 stereo, 1 joint stereo, 2 dual channel, 3 mono.
    pub mode: u32,
    pub mode_ext: u32,
}

impl Header {
    pub fn parse(b: &[u8]) -> Option<Self> {
        if b.len() < 4 || b[0] != 0xff || b[1] & 0xfe != 0xfa {
            return None; // sync, MPEG-1, Layer III
        }
        let bitrate_index = usize::from(b[2] >> 4);
        let sr_index = usize::from((b[2] >> 2) & 3);
        if bitrate_index == 0 || bitrate_index == 15 || sr_index == 3 || b[3] & 3 == 2 {
            return None; // free format, invalid, reserved emphasis
        }
        Some(Self {
            crc: b[1] & 1 == 0,
            bitrate_index,
            sr_index,
            padding: b[2] & 2 != 0,
            mode: u32::from(b[3] >> 6),
            mode_ext: u32::from((b[3] >> 4) & 3),
        })
    }

    pub fn channels(&self) -> usize {
        if self.mode == 3 {
            1
        } else {
            2
        }
    }

    pub fn sample_rate(&self) -> u32 {
        SAMPLE_RATES[self.sr_index]
    }

    pub fn kbps(&self) -> u32 {
        BITRATES[self.bitrate_index]
    }

    pub fn frame_len(&self) -> usize {
        (144_000 * self.kbps() / self.sample_rate()) as usize + usize::from(self.padding)
    }

    pub fn side_info_len(&self) -> usize {
        if self.channels() == 1 {
            17
        } else {
            32
        }
    }

    /// Whether `other` belongs to the same stream (for resynchronizing).
    pub fn compatible(&self, other: &Header) -> bool {
        self.sr_index == other.sr_index && (self.mode == 3) == (other.mode == 3)
    }
}

#[derive(Clone, Copy, Debug, Default)]
struct Granule {
    part2_3_length: usize,
    big_values: usize,
    global_gain: i32,
    scalefac_compress: usize,
    window_switching: bool,
    block_type: u32,
    mixed: bool,
    table_select: [usize; 3],
    subblock_gain: [i32; 3],
    region0_count: usize,
    region1_count: usize,
    preflag: bool,
    scalefac_scale: bool,
    count1_table: bool,
}

#[derive(Clone, Copy, Debug, Default)]
struct SideInfo {
    main_data_begin: usize,
    scfsi: [[bool; 4]; 2],
    gr: [[Granule; 2]; 2],
}

fn read_side_info(r: &mut BitReader, channels: usize) -> SideInfo {
    let mut si = SideInfo { main_data_begin: r.get(9) as usize, ..Default::default() };
    r.skip(if channels == 1 { 5 } else { 3 });
    for ch in 0..channels {
        for band in 0..4 {
            si.scfsi[ch][band] = r.get_bit();
        }
    }
    for gr in 0..2 {
        for ch in 0..channels {
            let g = &mut si.gr[gr][ch];
            g.part2_3_length = r.get(12) as usize;
            g.big_values = (r.get(9) as usize).min(288);
            g.global_gain = r.get(8) as i32;
            g.scalefac_compress = r.get(4) as usize;
            g.window_switching = r.get_bit();
            if g.window_switching {
                g.block_type = r.get(2);
                g.mixed = r.get_bit();
                g.table_select[0] = r.get(5) as usize;
                g.table_select[1] = r.get(5) as usize;
                for s in &mut g.subblock_gain {
                    *s = r.get(3) as i32;
                }
                g.region0_count = if g.block_type == 2 && !g.mixed { 8 } else { 7 };
                g.region1_count = 36;
            } else {
                for t in &mut g.table_select {
                    *t = r.get(5) as usize;
                }
                g.region0_count = r.get(4) as usize;
                g.region1_count = r.get(3) as usize;
            }
            g.preflag = r.get_bit();
            g.scalefac_scale = r.get_bit();
            g.count1_table = r.get_bit();
        }
    }
    si
}

/// Scale factors of one granule / channel.
#[derive(Clone, Copy, Debug, Default)]
struct ScaleFactors {
    long: [i32; 22],
    short: [[i32; 3]; 13],
}

/// The synthesis filterbank state of one channel.
pub(crate) struct Synth {
    v: [f32; 1024],
    offset: usize,
}

impl Synth {
    pub(crate) fn new() -> Self {
        Self { v: [0.0; 1024], offset: 0 }
    }

    /// 32 subband samples in, 32 PCM samples out.
    pub(crate) fn run(&mut self, s: &[f32; 32], out: &mut [f32]) {
        let n = numeric();
        self.offset = (self.offset + 1024 - 64) & 1023;
        for i in 0..64 {
            let row = &n.synth_matrix[i * 32..i * 32 + 32];
            let mut acc = 0.0f32;
            for k in 0..32 {
                acc += row[k] * s[k];
            }
            self.v[(self.offset + i) & 1023] = acc;
        }
        let d = &n.synth_window;
        for (j, o) in out.iter_mut().enumerate().take(32) {
            let mut acc = 0.0f32;
            for i in 0..8 {
                // U[64i + j] = V[128i + j], U[64i + 32 + j] = V[128i + 96 + j]
                acc += self.v[(self.offset + 128 * i + j) & 1023] * d[64 * i + j];
                acc += self.v[(self.offset + 128 * i + 96 + j) & 1023] * d[64 * i + 32 + j];
            }
            *o = acc;
        }
    }
}

/// Why a frame produced silence.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FrameError {
    /// Not a valid MPEG-1 Layer III frame.
    Header,
    /// The bit reservoir refers to data before what was decoded (after a seek).
    Reservoir,
    /// The Huffman data is inconsistent.
    Data,
}

/// Decodes frames one after the other into 1152 samples per channel.
pub struct Decoder {
    reservoir: Vec<u8>,
    overlap: Box<[[[f32; 18]; 32]; 2]>,
    synth: [Synth; 2],
    xr: Box<[[f32; 576]; 2]>,
    is: Box<[i32; 576]>,
    nonzero: [usize; 2],
    scalefac: [[ScaleFactors; 2]; 2],
    /// Sampling frequency index of the frame being decoded.
    sri: usize,
}

impl Default for Decoder {
    fn default() -> Self {
        Self::new()
    }
}

impl Decoder {
    pub fn new() -> Self {
        Self {
            reservoir: Vec::with_capacity(4096),
            overlap: Box::new([[[0.0; 18]; 32]; 2]),
            synth: [Synth::new(), Synth::new()],
            xr: Box::new([[0.0; 576]; 2]),
            is: Box::new([0; 576]),
            nonzero: [0; 2],
            scalefac: Default::default(),
            sri: 0,
        }
    }

    /// Forgets everything (before decoding from another position).
    pub fn reset(&mut self) {
        self.reservoir.clear();
        *self.overlap = [[[0.0; 18]; 32]; 2];
        self.synth = [Synth::new(), Synth::new()];
    }

    /// Decodes the frame at the start of `frame` (at least its whole length)
    /// into `out` (1152 samples per channel; channel 1 unused for mono).
    /// Errors still produce output (silence) and keep the state usable.
    pub fn decode(&mut self, frame: &[u8], out: &mut [[f32; 1152]; 2]) -> Result<Header, FrameError> {
        let h = Header::parse(frame).ok_or(FrameError::Header)?;
        let len = h.frame_len();
        if frame.len() < len {
            return Err(FrameError::Header);
        }
        let channels = h.channels();
        let si_start = 4 + if h.crc { 2 } else { 0 };
        let si_len = h.side_info_len();
        if si_start + si_len > len {
            return Err(FrameError::Header);
        }
        let si = read_side_info(&mut BitReader::new(&frame[si_start..si_start + si_len]), channels);
        let main = &frame[si_start + si_len..len];
        self.sri = h.sr_index;

        // Main data: the end of the reservoir, then this frame's.
        let begin = si.main_data_begin;
        let have = self.reservoir.len();
        let ok = begin <= have;
        let mut data = Vec::new();
        if ok {
            data.extend_from_slice(&self.reservoir[have - begin..]);
            data.extend_from_slice(main);
        }
        self.reservoir.extend_from_slice(main);
        if self.reservoir.len() > 2048 {
            let cut = self.reservoir.len() - 2048;
            self.reservoir.drain(..cut);
        }

        let mut result = Ok(h);
        if !ok {
            result = Err(FrameError::Reservoir);
        }
        let mut r = BitReader::new(&data);
        let mut pos = 0usize;
        for gr in 0..2 {
            for ch in 0..channels {
                let good = ok && self.decode_channel(&mut r, &mut pos, &h, &si, gr, ch);
                if !good {
                    self.xr[ch] = [0.0; 576];
                    self.nonzero[ch] = 0;
                    if ok {
                        result = Err(FrameError::Data);
                    }
                }
            }
            if channels == 2 && h.mode == 1 {
                self.stereo(&h, &si.gr[gr][1], gr);
            }
            for ch in 0..channels {
                let g = si.gr[gr][ch];
                self.hybrid(ch, &g, &mut out[ch][gr * 576..gr * 576 + 576]);
            }
        }
        result
    }

    /// Scale factors, Huffman data and requantization of one granule /
    /// channel into `xr[ch]`. Returns false on inconsistent data.
    fn decode_channel(&mut self, r: &mut BitReader, pos: &mut usize, h: &Header, si: &SideInfo, gr: usize, ch: usize) -> bool {
        let g = &si.gr[gr][ch];
        let start = *pos;
        let end = start + g.part2_3_length;
        *pos = end;
        if end > r.bits_left_total() {
            return false;
        }
        r.set_position(start);

        // Scale factors.
        let (slen1, slen2) = SLEN[g.scalefac_compress];
        let mut sf = ScaleFactors::default();
        if g.window_switching && g.block_type == 2 {
            if g.mixed {
                for v in &mut sf.long[..8] {
                    *v = r.get(slen1) as i32;
                }
                for sfb in 3..12 {
                    let slen = if sfb < 6 { slen1 } else { slen2 };
                    for w in 0..3 {
                        sf.short[sfb][w] = r.get(slen) as i32;
                    }
                }
            } else {
                for sfb in 0..12 {
                    let slen = if sfb < 6 { slen1 } else { slen2 };
                    for w in 0..3 {
                        sf.short[sfb][w] = r.get(slen) as i32;
                    }
                }
            }
        } else {
            let groups = [(0usize, 6usize, slen1), (6, 11, slen1), (11, 16, slen2), (16, 21, slen2)];
            for (gi, &(a, b, slen)) in groups.iter().enumerate() {
                if gr == 1 && si.scfsi[ch][gi] {
                    sf.long[a..b].copy_from_slice(&self.scalefac[0][ch].long[a..b]);
                } else {
                    for v in &mut sf.long[a..b] {
                        *v = r.get(slen) as i32;
                    }
                }
            }
        }
        self.scalefac[gr][ch] = sf;

        // Huffman data.
        let sri = h.sr_index;
        let is = &mut *self.is;
        is.fill(0);
        let big = g.big_values * 2;
        let (r1, r2) = if g.window_switching {
            (36usize, 576usize)
        } else {
            let a = SFB_LONG[sri][(g.region0_count + 1).min(22)];
            let b = SFB_LONG[sri][(g.region0_count + g.region1_count + 2).min(22)];
            (a, b)
        };
        let trees = trees();
        let mut i = 0;
        while i < big {
            let t = if i < r1 {
                g.table_select[0]
            } else if i < r2 {
                g.table_select[1]
            } else {
                g.table_select[2]
            };
            let region_end = if i < r1 {
                r1
            } else if i < r2 {
                r2
            } else {
                576
            }
            .min(big);
            if t == 0 {
                i = region_end;
                continue;
            }
            let Some(tree) = trees[t].as_ref() else { return false };
            let width = huff_source(t).map_or(16, |s| s.width) as i32;
            let linbits = LINBITS[t];
            while i < region_end {
                let Some(v) = huff_decode(r, tree) else { return false };
                let (mut x, mut y) = (v / width, v % width);
                if linbits > 0 && x == 15 {
                    x += r.get(linbits) as i32;
                }
                if x != 0 && r.get_bit() {
                    x = -x;
                }
                if linbits > 0 && y == 15 {
                    y += r.get(linbits) as i32;
                }
                if y != 0 && r.get_bit() {
                    y = -y;
                }
                is[i] = x;
                is[i + 1] = y;
                i += 2;
            }
        }
        if r.position() > end {
            return false;
        }
        // count1
        let a_tree = trees[32].as_ref();
        while i + 4 <= 576 && r.position() < end {
            let q = if g.count1_table {
                15 - r.get(4) as i32
            } else {
                match a_tree.and_then(|t| huff_decode(r, t)) {
                    Some(v) => v,
                    None => return false,
                }
            };
            let mut vals = [(q >> 3) & 1, (q >> 2) & 1, (q >> 1) & 1, q & 1];
            for v in &mut vals {
                if *v != 0 && r.get_bit() {
                    *v = -*v;
                }
            }
            if r.position() > end {
                break; // the last quadruple ran past the data: dropped
            }
            is[i..i + 4].copy_from_slice(&vals);
            i += 4;
        }
        let nonzero = is.iter().rposition(|&v| v != 0).map_or(0, |p| p + 1);
        self.nonzero[ch] = nonzero;

        // Requantization.
        let n = numeric();
        let xr = &mut self.xr[ch];
        let mult = if g.scalefac_scale { 1.0 } else { 0.5 };
        let base = 0.25 * (g.global_gain as f64 - 210.0);
        let pow2 = |e: f64| e.exp2() as f32;
        let sign_pow = |v: i32| -> f32 {
            let a = v.unsigned_abs() as usize;
            let m = if a < n.pow43.len() { n.pow43[a] } else { (a as f64).powf(4.0 / 3.0) as f32 };
            if v < 0 {
                -m
            } else {
                m
            }
        };
        xr.fill(0.0);
        let short = g.window_switching && g.block_type == 2;
        let long_end = if short {
            if g.mixed {
                36
            } else {
                0
            }
        } else {
            576
        };
        // Long part.
        let mut sfb = 0;
        let lim = nonzero.min(long_end);
        let mut k = 0;
        while k < lim {
            let b_end = SFB_LONG[sri][sfb + 1];
            let pre = if g.preflag { PRETAB[sfb] } else { 0 };
            let scale = pow2(base - mult * f64::from(sf.long[sfb.min(21)] + pre));
            while k < b_end.min(lim) {
                if is[k] != 0 {
                    xr[k] = sign_pow(is[k]) * scale;
                }
                k += 1;
            }
            sfb += 1;
        }
        // Short part (bitstream order: band, window, line).
        if short {
            let mut k = long_end;
            let first = if g.mixed { 3 } else { 0 };
            for sfb in first..13 {
                let width = SFB_SHORT[sri][sfb + 1] - SFB_SHORT[sri][sfb];
                for w in 0..3 {
                    let e = base - 2.0 * f64::from(g.subblock_gain[w]) - mult * f64::from(sf.short[sfb][w]);
                    let scale = pow2(e);
                    for _ in 0..width {
                        if k < nonzero && is[k] != 0 {
                            xr[k] = sign_pow(is[k]) * scale;
                        }
                        k += 1;
                    }
                }
            }
        }
        true
    }

    /// Mid/side and intensity stereo of a joint stereo granule.
    fn stereo(&mut self, h: &Header, right: &Granule, gr: usize) {
        let ms = h.mode_ext & 2 != 0;
        let intensity = h.mode_ext & 1 != 0;
        let sri = h.sr_index;
        // Lines from `is_start` on are intensity coded (per window for short blocks).
        let mut is_bound = [576usize; 3];
        let mut long_bound = 576usize;
        if intensity {
            let nz = self.nonzero[1];
            let short = right.window_switching && right.block_type == 2;
            if short {
                // Per window: the first band above the last nonzero value of the right channel.
                let first = if right.mixed { 3 } else { 0 };
                let base = if right.mixed { 36 } else { 0 };
                let mut k = base;
                let mut last_band = [-1i32; 3];
                for sfb in first..13 {
                    let width = SFB_SHORT[sri][sfb + 1] - SFB_SHORT[sri][sfb];
                    for lb in last_band.iter_mut() {
                        if (k..k + width).any(|i| self.xr[1][i] != 0.0) {
                            *lb = sfb as i32;
                        }
                        k += width;
                    }
                }
                for w in 0..3 {
                    is_bound[w] = (last_band[w] + 1).max(first as i32) as usize;
                    // as band index
                }
                long_bound = if right.mixed && last_band.iter().all(|&b| b < 0) {
                    // Long part of a mixed block: above its last nonzero band.
                    let mut b = 0;
                    while b < 8 && SFB_LONG[sri][b] < nz.min(36) {
                        b += 1;
                    }
                    b
                } else {
                    usize::MAX
                };
            } else {
                let mut b = 0;
                while b < 22 && SFB_LONG[sri][b] < nz {
                    b += 1;
                }
                long_bound = b;
            }
        }
        let sf = self.scalefac[gr][1];
        let short = right.window_switching && right.block_type == 2;
        let inv = std::f32::consts::FRAC_1_SQRT_2;
        let apply = |xr: &mut [[f32; 576]; 2], k: usize, is_pos: Option<i32>| match is_pos {
            Some(p) if intensity && p != 7 => {
                let (kl, kr) = if p == 6 {
                    (1.0, 0.0)
                } else {
                    let t = (f64::from(p) * std::f64::consts::PI / 12.0).tan();
                    ((t / (1.0 + t)) as f32, (1.0 / (1.0 + t)) as f32)
                };
                let m = xr[0][k];
                xr[0][k] = m * kl;
                xr[1][k] = m * kr;
            }
            _ if ms => {
                let (m, s) = (xr[0][k], xr[1][k]);
                xr[0][k] = (m + s) * inv;
                xr[1][k] = (m - s) * inv;
            }
            _ => {}
        };
        let xr = &mut *self.xr;
        if !short {
            for sfb in 0..22 {
                let pos = if intensity && sfb >= long_bound { Some(sf.long[sfb.min(20)]) } else { None };
                for k in SFB_LONG[sri][sfb]..SFB_LONG[sri][sfb + 1] {
                    apply(xr, k, pos);
                }
            }
        } else {
            let first = if right.mixed { 3 } else { 0 };
            if right.mixed {
                for sfb in 0..8 {
                    let pos = if intensity && sfb >= long_bound { Some(sf.long[sfb]) } else { None };
                    for k in SFB_LONG[sri][sfb]..SFB_LONG[sri][sfb + 1] {
                        apply(xr, k, pos);
                    }
                }
            }
            let mut k = if right.mixed { 36 } else { 0 };
            for sfb in first..13 {
                let width = SFB_SHORT[sri][sfb + 1] - SFB_SHORT[sri][sfb];
                for w in 0..3 {
                    let pos = if intensity && sfb >= is_bound[w] { Some(sf.short[sfb.min(11)][w]) } else { None };
                    for _ in 0..width {
                        apply(xr, k, pos);
                        k += 1;
                    }
                }
            }
        }
        let nz = self.nonzero[0].max(self.nonzero[1]);
        self.nonzero = [if intensity { 576 } else { nz }; 2];
    }

    /// Reordering, alias reduction, IMDCT, overlap-add, frequency inversion
    /// and synthesis of one granule / channel into 576 samples.
    fn hybrid(&mut self, ch: usize, g: &Granule, out: &mut [f32]) {
        let n = numeric();
        let xr = &mut self.xr[ch];
        let short = g.window_switching && g.block_type == 2;
        if short {
            // Bitstream order (band, window, line) -> subband order.
            let start = if g.mixed { 36 } else { 0 };
            let mut tmp = [0f32; 576];
            tmp[..start].copy_from_slice(&xr[..start]);
            let sri = self.sri;
            let first = if g.mixed { 3 } else { 0 };
            let mut k = start;
            for sfb in first..13 {
                let lo = SFB_SHORT[sri][sfb];
                let width = SFB_SHORT[sri][sfb + 1] - lo;
                for w in 0..3 {
                    for f in 0..width {
                        tmp[3 * (lo + f) + w] = xr[k];
                        k += 1;
                    }
                }
            }
            xr.copy_from_slice(&tmp);
        }
        // Alias reduction between long subbands.
        let alias_sbs = if short {
            if g.mixed {
                1
            } else {
                0
            }
        } else {
            31
        };
        for sb in 1..=alias_sbs {
            for i in 0..8 {
                let bu = xr[sb * 18 - 1 - i];
                let bd = xr[sb * 18 + i];
                xr[sb * 18 - 1 - i] = bu * n.cs[i] - bd * n.ca[i];
                xr[sb * 18 + i] = bd * n.cs[i] + bu * n.ca[i];
            }
        }
        // IMDCT and overlap-add.
        let mut sub = [[0f32; 18]; 32]; // [sb][time]
        let overlap = &mut self.overlap[ch];
        for sb in 0..32 {
            let x = &xr[sb * 18..sb * 18 + 18];
            let mut y = [0f32; 36];
            let long_block = !short || (g.mixed && sb < 2);
            if long_block {
                let wi = match (g.window_switching, g.block_type) {
                    (true, 1) => 1,
                    (true, 3) => 2,
                    _ => 0,
                };
                if x.iter().any(|&v| v != 0.0) {
                    let w = &n.windows[wi];
                    for i in 0..36 {
                        let row = &n.imdct_long[i * 18..i * 18 + 18];
                        let mut acc = 0.0f32;
                        for k in 0..18 {
                            acc += row[k] * x[k];
                        }
                        y[i] = acc * w[i];
                    }
                }
            } else if x.iter().any(|&v| v != 0.0) {
                for w in 0..3 {
                    for i in 0..12 {
                        let row = &n.imdct_short[i * 6..i * 6 + 6];
                        let mut acc = 0.0f32;
                        for k in 0..6 {
                            acc += row[k] * x[3 * k + w];
                        }
                        y[6 + 6 * w + i] += acc * n.short_window[i];
                    }
                }
            }
            for i in 0..18 {
                let mut v = y[i] + overlap[sb][i];
                if sb % 2 == 1 && i % 2 == 1 {
                    v = -v;
                }
                sub[sb][i] = v;
                overlap[sb][i] = y[i + 18];
            }
        }
        // Synthesis.
        let synth = &mut self.synth[ch];
        let mut s = [0f32; 32];
        for t in 0..18 {
            for sb in 0..32 {
                s[sb] = sub[sb][t];
            }
            synth.run(&s, &mut out[t * 32..t * 32 + 32]);
        }
    }
}

trait BitsTotal {
    fn bits_left_total(&self) -> usize;
}

impl BitsTotal for BitReader<'_> {
    fn bits_left_total(&self) -> usize {
        (self.bits_left() + self.position() as isize).max(0) as usize
    }
}

#[inline]
fn huff_decode(r: &mut BitReader, tree: &HuffTree) -> Option<i32> {
    let mut n = 0usize;
    loop {
        let child = tree.nodes[n][usize::from(r.get_bit())];
        if child < 0 {
            return Some(-child - 1);
        }
        if child == 0 || r.overrun {
            return None;
        }
        n = child as usize;
    }
}
