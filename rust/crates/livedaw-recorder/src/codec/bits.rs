//! MSB-first bit packing for the FLAC and MP3 bitstreams.

/// Appends bits to a byte vector, most significant bit first.
#[derive(Default)]
pub struct BitWriter {
    pub bytes: Vec<u8>,
    acc: u64,
    /// Bits held in `acc` (0..8 between calls).
    n: u32,
}

impl BitWriter {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn clear(&mut self) {
        self.bytes.clear();
        self.acc = 0;
        self.n = 0;
    }

    /// Bits written so far.
    pub fn bit_len(&self) -> usize {
        self.bytes.len() * 8 + self.n as usize
    }

    /// Writes the low `bits` (0..=32) bits of `v`.
    #[inline]
    pub fn put(&mut self, v: u32, bits: u32) {
        debug_assert!(bits <= 32);
        if bits == 0 {
            return;
        }
        let v = u64::from(v) & ((1u64 << bits) - 1);
        self.acc = (self.acc << bits) | v;
        self.n += bits;
        while self.n >= 8 {
            self.n -= 8;
            self.bytes.push((self.acc >> self.n) as u8);
        }
        self.acc &= (1u64 << self.n) - 1;
    }

    /// Writes a signed value in `bits` two's complement bits.
    #[inline]
    pub fn put_signed(&mut self, v: i32, bits: u32) {
        self.put(v as u32, bits);
    }

    pub fn put_u64(&mut self, v: u64, bits: u32) {
        if bits > 32 {
            self.put((v >> 32) as u32, bits - 32);
            self.put(v as u32, 32);
        } else {
            self.put(v as u32, bits);
        }
    }

    /// `q` zero bits then a one (FLAC's unary code).
    #[inline]
    pub fn put_unary(&mut self, mut q: u32) {
        while q >= 32 {
            self.put(0, 32);
            q -= 32;
        }
        self.put(1, q + 1);
    }

    /// Pads with zero bits to a byte boundary.
    pub fn align(&mut self) {
        if self.n > 0 {
            let pad = 8 - self.n;
            self.put(0, pad);
        }
    }
}

/// Reads bits from a byte slice, most significant bit first. Reading past
/// the end yields zero bits and sets `overrun`.
pub struct BitReader<'a> {
    data: &'a [u8],
    /// Next bit to read.
    pos: usize,
    pub overrun: bool,
}

impl<'a> BitReader<'a> {
    pub fn new(data: &'a [u8]) -> Self {
        Self { data, pos: 0, overrun: false }
    }

    pub fn at(data: &'a [u8], bit: usize) -> Self {
        Self { data, pos: bit, overrun: false }
    }

    pub fn position(&self) -> usize {
        self.pos
    }

    pub fn set_position(&mut self, bit: usize) {
        self.pos = bit;
    }

    pub fn bits_left(&self) -> isize {
        (self.data.len() * 8) as isize - self.pos as isize
    }

    pub fn align(&mut self) {
        self.pos = self.pos.div_ceil(8) * 8;
    }

    /// The next `bits` (0..=32) bits without consuming them.
    #[inline]
    pub fn peek(&self, bits: u32) -> u32 {
        if bits == 0 {
            return 0;
        }
        let byte = self.pos >> 3;
        let shift = (self.pos & 7) as u32;
        // Gather 8 bytes (zeros past the end).
        let mut w = 0u64;
        if byte + 8 <= self.data.len() {
            w = u64::from_be_bytes(self.data[byte..byte + 8].try_into().unwrap_or([0; 8]));
        } else {
            for i in 0..8 {
                w = (w << 8) | u64::from(self.data.get(byte + i).copied().unwrap_or(0));
            }
        }
        ((w << shift) >> (64 - bits)) as u32
    }

    #[inline]
    pub fn get(&mut self, bits: u32) -> u32 {
        let v = self.peek(bits);
        self.skip(bits as usize);
        v
    }

    #[inline]
    pub fn skip(&mut self, bits: usize) {
        self.pos += bits;
        if self.pos > self.data.len() * 8 {
            self.overrun = true;
        }
    }

    #[inline]
    pub fn get_bit(&mut self) -> bool {
        self.get(1) != 0
    }

    pub fn get_u64(&mut self, bits: u32) -> u64 {
        if bits > 32 {
            let hi = u64::from(self.get(bits - 32));
            (hi << 32) | u64::from(self.get(32))
        } else {
            u64::from(self.get(bits))
        }
    }

    /// A `bits`-wide two's complement value.
    #[inline]
    pub fn get_signed(&mut self, bits: u32) -> i32 {
        if bits == 0 {
            return 0;
        }
        let v = self.get(bits);
        ((v << (32 - bits)) as i32) >> (32 - bits)
    }

    /// Counts zero bits up to a one (consumed). None past the end.
    #[inline]
    pub fn get_unary(&mut self) -> Option<u32> {
        let mut q = 0;
        loop {
            let w = self.peek(32);
            if w != 0 {
                let z = w.leading_zeros();
                self.skip(z as usize + 1);
                return if self.overrun { None } else { Some(q + z) };
            }
            q += 32;
            self.skip(32);
            if self.overrun {
                return None;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip() {
        let mut w = BitWriter::new();
        w.put(0b101, 3);
        w.put(0xdead_beef, 32);
        w.put_signed(-5, 7);
        w.put_unary(40);
        w.put_u64(0x1_2345_6789, 36);
        w.align();
        let mut r = BitReader::new(&w.bytes);
        assert_eq!(r.get(3), 0b101);
        assert_eq!(r.get(32), 0xdead_beef);
        assert_eq!(r.get_signed(7), -5);
        assert_eq!(r.get_unary(), Some(40));
        assert_eq!(r.get_u64(36), 0x1_2345_6789);
        assert!(!r.overrun);
        r.align();
        assert_eq!(r.position(), w.bytes.len() * 8);
        r.get(1);
        assert!(r.overrun);
    }
}
