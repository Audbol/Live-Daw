//! Tables of MPEG-1 Layer III shared by the encoder and the decoder.

use super::huffman_data::*;
use std::f64::consts::PI;
use std::sync::OnceLock;

/// Sampling frequencies of MPEG-1 (index 0, 1, 2 of the header).
pub const SAMPLE_RATES: [u32; 3] = [44100, 48000, 32000];
/// Bit rates of MPEG-1 Layer III (kbit/s) by header index (0: free format).
pub const BITRATES: [u32; 15] = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320];

/// Long-block scale factor band boundaries (23 per rate).
#[rustfmt::skip]
pub const SFB_LONG: [[usize; 23]; 3] = [
    [0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 52, 62, 74, 90, 110, 134, 162, 196, 238, 288, 342, 418, 576],
    [0, 4, 8, 12, 16, 20, 24, 30, 36, 42, 50, 60, 72, 88, 106, 128, 156, 190, 230, 276, 330, 384, 576],
    [0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 54, 66, 82, 102, 126, 156, 194, 240, 296, 364, 448, 550, 576],
];

/// Short-block scale factor band boundaries (per window, 14 per rate).
#[rustfmt::skip]
pub const SFB_SHORT: [[usize; 14]; 3] = [
    [0, 4, 8, 12, 16, 22, 30, 40, 52, 66, 84, 106, 136, 192],
    [0, 4, 8, 12, 16, 22, 28, 38, 50, 64, 80, 100, 126, 192],
    [0, 4, 8, 12, 16, 22, 30, 42, 58, 78, 104, 138, 180, 192],
];

/// Pre-emphasis added to the long-block scale factors when `preflag` is set.
pub const PRETAB: [i32; 22] = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 3, 2, 0];

/// Scale factor lengths (slen1, slen2) by `scalefac_compress`.
pub const SLEN: [(u32, u32); 16] = [
    (0, 0),
    (0, 1),
    (0, 2),
    (0, 3),
    (3, 0),
    (1, 1),
    (1, 2),
    (1, 3),
    (2, 1),
    (2, 2),
    (2, 3),
    (3, 1),
    (3, 2),
    (3, 3),
    (4, 2),
    (4, 3),
];

/// Extra bits of each big-value table (`table_select`).
pub const LINBITS: [u32; 32] =
    [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 6, 8, 10, 13, 4, 5, 6, 7, 8, 9, 11, 13];

/// Alias reduction coefficients c_i.
pub const ALIAS_C: [f64; 8] = [-0.6, -0.535, -0.33, -0.185, -0.095, -0.041, -0.0142, -0.0037];

/// A Huffman table's source: width of the (x, y) grid, codes and lengths.
pub struct HuffSource {
    pub width: usize,
    pub codes: &'static [u16],
    pub lens: &'static [u8],
}

/// The big-value table `t` (0..31; None for 0 and the unused 4 and 14).
pub fn huff_source(t: usize) -> Option<HuffSource> {
    let s = |width, codes: &'static [u16], lens: &'static [u8]| Some(HuffSource { width, codes, lens });
    match t {
        1 => s(2, &HCODE_1, &HLEN_1),
        2 => s(3, &HCODE_2, &HLEN_2),
        3 => s(3, &HCODE_3, &HLEN_3),
        5 => s(4, &HCODE_5, &HLEN_5),
        6 => s(4, &HCODE_6, &HLEN_6),
        7 => s(6, &HCODE_7, &HLEN_7),
        8 => s(6, &HCODE_8, &HLEN_8),
        9 => s(6, &HCODE_9, &HLEN_9),
        10 => s(8, &HCODE_10, &HLEN_10),
        11 => s(8, &HCODE_11, &HLEN_11),
        12 => s(8, &HCODE_12, &HLEN_12),
        13 => s(16, &HCODE_13, &HLEN_13),
        15 => s(16, &HCODE_15, &HLEN_15),
        16..=23 => s(16, &HCODE_16, &HLEN_16),
        24..=31 => s(16, &HCODE_24, &HLEN_24),
        _ => None,
    }
}

/// count1 table A (`count1table_select` 0) for the quadruple index 8v+4w+2x+y.
pub fn count1_a() -> (&'static [u16], &'static [u8]) {
    (&HCODE_A, &HLEN_A)
}

/// A decoding tree: node `n` has children `nodes[n]`; a child >= 0 is a
/// node, < 0 a leaf holding value `-child - 1`.
pub struct HuffTree {
    pub nodes: Vec<[i32; 2]>,
}

impl HuffTree {
    pub fn build(codes: &[u16], lens: &[u8]) -> Self {
        let mut nodes: Vec<[i32; 2]> = vec![[0, 0]];
        for (value, (&code, &len)) in codes.iter().zip(lens).enumerate() {
            let mut n = 0usize;
            for b in (0..len).rev() {
                let bit = ((u32::from(code) >> b) & 1) as usize;
                if b == 0 {
                    if nodes[n][bit] == 0 {
                        nodes[n][bit] = -(value as i32) - 1;
                    }
                } else {
                    let next = nodes[n][bit];
                    if next > 0 {
                        n = next as usize;
                    } else if next == 0 {
                        nodes.push([0, 0]);
                        let id = nodes.len() - 1;
                        nodes[n][bit] = id as i32;
                        n = id;
                    } else {
                        break; // a leaf on the way: an inconsistent table entry
                    }
                }
            }
        }
        Self { nodes }
    }
}

/// Decoding trees of tables 0..=31 (by code family) and count1 table A (32).
pub fn trees() -> &'static [Option<HuffTree>; 33] {
    static T: OnceLock<[Option<HuffTree>; 33]> = OnceLock::new();
    T.get_or_init(|| {
        std::array::from_fn(|t| {
            if t == 32 {
                let (c, l) = count1_a();
                Some(HuffTree::build(c, l))
            } else {
                huff_source(t).map(|s| HuffTree::build(s.codes, s.lens))
            }
        })
    })
}

/// Derived numeric tables.
pub struct Numeric {
    /// The synthesis window D[0..512].
    pub synth_window: [f32; 512],
    /// Synthesis matrixing N[i][k] = cos((16 + i)(2k + 1) pi / 64), 64 x 32.
    pub synth_matrix: Vec<f32>,
    /// Long IMDCT: cos(pi / 72 (2i + 1 + 18)(2k + 1)), 36 x 18.
    pub imdct_long: Vec<f32>,
    /// Short IMDCT: cos(pi / 24 (2i + 1 + 6)(2k + 1)), 12 x 6.
    pub imdct_short: Vec<f32>,
    /// Windows of block types 0, 1, 3 (index 0, 1, 2) and the short window.
    pub windows: [[f32; 36]; 3],
    pub short_window: [f32; 12],
    /// Alias reduction cs / ca.
    pub cs: [f32; 8],
    pub ca: [f32; 8],
    /// |i|^(4/3) for i in 0..8207.
    pub pow43: Vec<f32>,
}

pub fn numeric() -> &'static Numeric {
    static N: OnceLock<Numeric> = OnceLock::new();
    N.get_or_init(|| {
        let mut synth_window = [0f32; 512];
        for (n, w) in synth_window.iter_mut().enumerate() {
            let v = if n <= 256 {
                SYNTH_WINDOW_HALF[n]
            } else if n % 64 == 0 {
                SYNTH_WINDOW_HALF[512 - n]
            } else {
                -SYNTH_WINDOW_HALF[512 - n]
            };
            *w = (f64::from(v) / 65536.0) as f32;
        }
        let synth_matrix = (0..64)
            .flat_map(|i| (0..32).map(move |k| ((16.0 + i as f64) * (2.0 * k as f64 + 1.0) * PI / 64.0).cos() as f32))
            .collect();
        let imdct_long = (0..36)
            .flat_map(|i| (0..18).map(move |k| (PI / 72.0 * (2.0 * i as f64 + 1.0 + 18.0) * (2.0 * k as f64 + 1.0)).cos() as f32))
            .collect();
        let imdct_short = (0..12)
            .flat_map(|i| (0..6).map(move |k| (PI / 24.0 * (2.0 * i as f64 + 1.0 + 6.0) * (2.0 * k as f64 + 1.0)).cos() as f32))
            .collect();
        let mut windows = [[0f32; 36]; 3];
        for i in 0..36 {
            let long = (PI / 36.0 * (i as f64 + 0.5)).sin();
            windows[0][i] = long as f32;
            windows[1][i] = match i {
                0..=17 => long,
                18..=23 => 1.0,
                24..=29 => (PI / 12.0 * (i as f64 - 18.0 + 0.5)).sin(),
                _ => 0.0,
            } as f32;
            windows[2][i] = match i {
                0..=5 => 0.0,
                6..=11 => (PI / 12.0 * (i as f64 - 6.0 + 0.5)).sin(),
                12..=17 => 1.0,
                _ => long,
            } as f32;
        }
        let short_window = std::array::from_fn(|i| (PI / 12.0 * (i as f64 + 0.5)).sin() as f32);
        let cs = std::array::from_fn(|i| (1.0 / (1.0 + ALIAS_C[i] * ALIAS_C[i]).sqrt()) as f32);
        let ca = std::array::from_fn(|i| (ALIAS_C[i] / (1.0 + ALIAS_C[i] * ALIAS_C[i]).sqrt()) as f32);
        let pow43 = (0..8207).map(|i| (i as f64).powf(4.0 / 3.0) as f32).collect();
        Numeric { synth_window, synth_matrix, imdct_long, imdct_short, windows, short_window, cs, ca, pow43 }
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn huffman_tables_are_complete_prefix_codes() {
        let check = |codes: &[u16], lens: &[u8], name: &str| {
            let kraft: f64 = lens.iter().map(|&l| 0.5f64.powi(i32::from(l))).sum();
            assert!((kraft - 1.0).abs() < 1e-12, "table {name}: Kraft sum {kraft}");
            let mut words: Vec<String> =
                codes.iter().zip(lens).map(|(&c, &l)| format!("{:0width$b}", c, width = l as usize)).collect();
            words.sort();
            for w in words.windows(2) {
                assert!(!w[1].starts_with(&w[0]), "table {name}: {} is a prefix of {}", w[0], w[1]);
            }
        };
        for t in [1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 24] {
            let s = huff_source(t).unwrap();
            assert_eq!(s.codes.len(), s.width * s.width);
            check(s.codes, s.lens, &t.to_string());
        }
        let (c, l) = count1_a();
        check(c, l, "A");
    }

    #[test]
    fn synthesis_window_is_smooth_and_symmetric() {
        let n = numeric();
        // The prototype filter h[n] = D[n] * (-1)^(n / 64) is symmetric and smooth.
        let h: Vec<f64> = (0..512).map(|i| f64::from(n.synth_window[i]) * if (i / 64) % 2 == 0 { 1.0 } else { -1.0 }).collect();
        for i in 1..256 {
            assert!((h[i] - h[512 - i]).abs() < 1e-9, "symmetric at {i}");
        }
        for i in 2..509 {
            let d3 = h[i + 1] - 3.0 * h[i] + 3.0 * h[i - 1] - h[i - 2];
            assert!(d3.abs() < 8.0 / 65536.0, "smooth at {i}: {}", d3 * 65536.0);
        }
        assert!((h[256] - 1.144989014).abs() < 1e-6);
    }
}
