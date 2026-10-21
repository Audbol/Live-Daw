//! The mirrored screen as tiles: which tiles of a new snapshot changed (by
//! hash), the latest encoded image of every tile, and what each browser still
//! lacks.
//!
//! The host snapshots its window, asks `changed` which candidate tiles differ
//! from what was sent before, encodes those (PNG or JPEG) and `put`s them.
//! Every tile keeps only its newest image with a version number; a browser's
//! `View` remembers the versions it has, so a slow browser simply skips the
//! states it was too slow for and a new one gets the whole screen.

use std::sync::Arc;

/// Tile image formats (the frame message's format byte).
pub const FORMAT_PNG: u8 = 0;
pub const FORMAT_JPEG: u8 = 1;

/// Server -> browser binary message types (first byte).
pub const MSG_FRAME: u8 = 1;
pub const MSG_PONG: u8 = 2;

#[derive(Clone)]
struct Tile {
    version: u64,
    format: u8,
    data: Arc<[u8]>,
}

/// The screen's geometry and tiles.
pub struct Screen {
    generation: u32,
    width: u32,
    height: u32,
    tile: u32,
    cols: u32,
    rows: u32,
    hashes: Vec<Option<u64>>,
    tiles: Vec<Option<Tile>>,
    version: u64,
    seq: u32,
}

impl Default for Screen {
    fn default() -> Self {
        Self { generation: 0, width: 0, height: 0, tile: 64, cols: 0, rows: 0, hashes: vec![], tiles: vec![], version: 0, seq: 0 }
    }
}

/// What one browser has received.
#[derive(Default)]
pub struct View {
    generation: u32,
    versions: Vec<u64>,
}

impl Screen {
    /// Starts over with a new size (or after nobody watched): every tile is
    /// unknown. Returns the new generation, which `put` must quote.
    pub fn reset(&mut self, width: u32, height: u32, tile: u32) -> u32 {
        self.generation = self.generation.wrapping_add(1);
        self.width = width.min(u32::from(u16::MAX));
        self.height = height.min(u32::from(u16::MAX));
        self.tile = tile.clamp(16, 1024);
        self.cols = self.width.div_ceil(self.tile);
        self.rows = self.height.div_ceil(self.tile);
        let n = (self.cols * self.rows) as usize;
        self.hashes = vec![None; n];
        self.tiles = vec![None; n];
        self.generation
    }

    pub fn generation(&self) -> u32 {
        self.generation
    }

    pub fn size(&self) -> (u32, u32) {
        (self.width, self.height)
    }

    pub fn num_tiles(&self) -> usize {
        self.tiles.len()
    }

    /// The pixel rectangle (x, y, w, h) of tile `index`.
    pub fn tile_rect(&self, index: u32) -> (u32, u32, u32, u32) {
        let (col, row) = (index % self.cols.max(1), index / self.cols.max(1));
        let (x, y) = (col * self.tile, row * self.tile);
        (x, y, self.tile.min(self.width - x.min(self.width)), self.tile.min(self.height - y.min(self.height)))
    }

    /// Of the `candidates` (tile indices), those whose pixels differ from the
    /// last call. `pixels` is the whole snapshot (`stride` bytes per row,
    /// `bpp` bytes per pixel); the hashes are updated.
    pub fn changed(&mut self, pixels: &[u8], stride: usize, bpp: usize, candidates: &[u32]) -> Vec<u32> {
        let mut out = Vec::new();
        for &i in candidates {
            if i as usize >= self.hashes.len() {
                continue;
            }
            let (x, y, w, h) = self.tile_rect(i);
            let mut hash = Hasher::default();
            for row in y..y + h {
                let start = row as usize * stride + x as usize * bpp;
                let Some(line) = pixels.get(start..start + w as usize * bpp) else { return out };
                hash.write(line);
            }
            let hash = Some(hash.finish());
            if self.hashes[i as usize] != hash {
                self.hashes[i as usize] = hash;
                out.push(i);
            }
        }
        out
    }

    /// The encoded image of a tile (ignored if `generation` is not current).
    pub fn put(&mut self, generation: u32, index: u32, format: u8, data: &[u8]) -> bool {
        if generation != self.generation || index as usize >= self.tiles.len() {
            return false;
        }
        self.version += 1;
        self.tiles[index as usize] = Some(Tile { version: self.version, format, data: data.into() });
        true
    }

    /// Whether `view` lacks anything.
    pub fn has_news(&self, view: &View) -> bool {
        view.generation != self.generation
            || self.tiles.iter().zip(&view.versions).any(|(t, &v)| t.as_ref().is_some_and(|t| t.version > v))
    }

    /// The frame message with everything `view` lacks (None: nothing), and
    /// marks it as sent. A view of an older generation gets the new size and
    /// all tiles there are.
    ///
    /// Layout (little endian): u8 MSG_FRAME, u32 seq, u16 width, u16 height,
    /// u16 tile size, u16 count, then per tile u16 column, u16 row, u8 format,
    /// u32 length, the image.
    pub fn collect(&mut self, view: &mut View) -> Option<Vec<u8>> {
        let fresh = view.generation != self.generation;
        if fresh {
            view.generation = self.generation;
            view.versions = vec![0; self.tiles.len()];
        }
        let news: Vec<(usize, &Tile)> = self
            .tiles
            .iter()
            .enumerate()
            .filter_map(|(i, t)| t.as_ref().filter(|t| t.version > view.versions[i]).map(|t| (i, t)))
            .collect();
        if news.is_empty() && !fresh {
            return None;
        }
        self.seq = self.seq.wrapping_add(1);
        let size: usize = news.iter().map(|(_, t)| 9 + t.data.len()).sum();
        let mut m = Vec::with_capacity(13 + size);
        m.push(MSG_FRAME);
        m.extend_from_slice(&self.seq.to_le_bytes());
        for v in [self.width, self.height, self.tile, news.len() as u32] {
            m.extend_from_slice(&(v.min(0xFFFF) as u16).to_le_bytes());
        }
        for (i, t) in news.iter().take(0xFFFF) {
            let (col, row) = (*i as u32 % self.cols, *i as u32 / self.cols);
            m.extend_from_slice(&(col as u16).to_le_bytes());
            m.extend_from_slice(&(row as u16).to_le_bytes());
            m.push(t.format);
            m.extend_from_slice(&(t.data.len() as u32).to_le_bytes());
            m.extend_from_slice(&t.data);
            view.versions[*i] = t.version;
        }
        Some(m)
    }
}

/// A fast 64-bit hash for comparing tiles (not cryptographic; a collision
/// only leaves one tile stale until it changes again).
struct Hasher(u64);

impl Default for Hasher {
    fn default() -> Self {
        Self(0x9E37_79B9_7F4A_7C15)
    }
}

impl Hasher {
    fn write(&mut self, bytes: &[u8]) {
        let mut chunks = bytes.chunks_exact(8);
        for c in &mut chunks {
            let v = u64::from_le_bytes([c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]]);
            self.mix(v);
        }
        for &b in chunks.remainder() {
            self.mix(u64::from(b) | 0x100);
        }
    }

    fn mix(&mut self, v: u64) {
        self.0 = (self.0 ^ v).wrapping_mul(0x0100_0000_01B3_u64.wrapping_mul(0x9E37_79B9)).rotate_left(29);
    }

    fn finish(&self) -> u64 {
        let mut h = self.0;
        h ^= h >> 33;
        h = h.wrapping_mul(0xFF51_AFD7_ED55_8CCD);
        h ^ (h >> 33)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A tile of a frame message: column, row, format, image.
    type FrameTile = (u16, u16, u8, Vec<u8>);

    /// A frame message's size and tiles.
    fn parse_frame(m: &[u8]) -> (u32, u32, Vec<FrameTile>) {
        assert_eq!(m[0], MSG_FRAME);
        let u16_at = |o: usize| u16::from_le_bytes([m[o], m[o + 1]]);
        let (w, h, count) = (u32::from(u16_at(5)), u32::from(u16_at(7)), u16_at(11));
        let mut tiles = Vec::new();
        let mut o = 13;
        for _ in 0..count {
            let len = u32::from_le_bytes([m[o + 5], m[o + 6], m[o + 7], m[o + 8]]) as usize;
            tiles.push((u16_at(o), u16_at(o + 2), m[o + 4], m[o + 9..o + 9 + len].to_vec()));
            o += 9 + len;
        }
        assert_eq!(o, m.len());
        (w, h, tiles)
    }

    fn all(s: &Screen) -> Vec<u32> {
        (0..s.num_tiles() as u32).collect()
    }

    #[test]
    fn geometry_covers_partial_tiles() {
        let mut s = Screen::default();
        s.reset(150, 70, 64);
        assert_eq!(s.num_tiles(), 6);
        assert_eq!(s.tile_rect(0), (0, 0, 64, 64));
        assert_eq!(s.tile_rect(2), (128, 0, 22, 64));
        assert_eq!(s.tile_rect(5), (128, 64, 22, 6));
    }

    #[test]
    fn only_changed_tiles_are_reported() {
        let (w, h) = (130usize, 70usize);
        let mut px = vec![0u8; w * h * 4];
        let mut s = Screen::default();
        s.reset(w as u32, h as u32, 64);
        assert_eq!(s.changed(&px, w * 4, 4, &all(&s)), all(&s), "everything is new");
        assert!(s.changed(&px, w * 4, 4, &all(&s)).is_empty());
        // One pixel in the bottom right partial tile, one in tile 1.
        px[((h - 1) * w + (w - 1)) * 4] = 9;
        px[(10 * w + 70) * 4 + 2] = 9;
        assert_eq!(s.changed(&px, w * 4, 4, &all(&s)), vec![1, 5]);
        // Only candidates are looked at.
        px[0] = 1;
        assert!(s.changed(&px, w * 4, 4, &[1, 2]).is_empty());
        assert_eq!(s.changed(&px, w * 4, 4, &[0, 99]), vec![0]);
    }

    #[test]
    fn views_get_the_screen_then_only_news() {
        let mut s = Screen::default();
        let g = s.reset(100, 50, 64);
        let mut a = View::default();
        // A new view gets the size even before any tile.
        let (w, h, tiles) = parse_frame(&s.collect(&mut a).unwrap());
        assert_eq!((w, h, tiles.len()), (100, 50, 0));
        assert!(s.collect(&mut a).is_none());

        assert!(s.put(g, 0, FORMAT_PNG, b"t0"));
        assert!(s.put(g, 1, FORMAT_JPEG, b"t1"));
        assert!(!s.put(g + 1, 1, FORMAT_PNG, b"stale"));
        assert!(!s.put(g, 7, FORMAT_PNG, b"out of range"));
        assert!(s.has_news(&a));
        let (_, _, tiles) = parse_frame(&s.collect(&mut a).unwrap());
        assert_eq!(tiles, vec![(0, 0, FORMAT_PNG, b"t0".to_vec()), (1, 0, FORMAT_JPEG, b"t1".to_vec())]);
        assert!(!s.has_news(&a));

        // A slow view skips intermediate states; a late one gets everything.
        let mut b = View::default();
        s.put(g, 1, FORMAT_PNG, b"t1'");
        s.put(g, 1, FORMAT_PNG, b"t1''");
        let (_, _, tiles) = parse_frame(&s.collect(&mut a).unwrap());
        assert_eq!(tiles, vec![(1, 0, FORMAT_PNG, b"t1''".to_vec())]);
        let (_, _, tiles) = parse_frame(&s.collect(&mut b).unwrap());
        assert_eq!(tiles.len(), 2);

        // A new size: everybody starts over.
        let g2 = s.reset(200, 100, 64);
        assert!(s.has_news(&a));
        let (w, _, tiles) = parse_frame(&s.collect(&mut a).unwrap());
        assert_eq!((w, tiles.len()), (200, 0));
        s.put(g2, 5, FORMAT_PNG, b"x");
        let (_, _, tiles) = parse_frame(&s.collect(&mut a).unwrap());
        assert_eq!(tiles, vec![(1, 1, FORMAT_PNG, b"x".to_vec())]);
    }
}
