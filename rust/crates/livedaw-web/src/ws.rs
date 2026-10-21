//! WebSocket (RFC 6455), server side: the handshake's accept key and the
//! framing. Small enough to keep here instead of pulling in a crate: SHA-1
//! and base64 only serve the handshake.

use std::io::{self, Read, Write};

/// The GUID the handshake appends to the client's key.
const GUID: &str = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/// Biggest message a client may send (input events are a few bytes).
pub const MAX_MESSAGE: usize = 64 * 1024;

/// SHA-1 (FIPS 180-4). Only for the handshake, where the protocol fixes it.
pub fn sha1(data: &[u8]) -> [u8; 20] {
    let mut h: [u32; 5] = [0x6745_2301, 0xEFCD_AB89, 0x98BA_DCFE, 0x1032_5476, 0xC3D2_E1F0];
    let mut msg = data.to_vec();
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&((data.len() as u64) * 8).to_be_bytes());

    for block in msg.chunks_exact(64) {
        let mut w = [0u32; 80];
        for (i, word) in block.chunks_exact(4).enumerate() {
            w[i] = u32::from_be_bytes([word[0], word[1], word[2], word[3]]);
        }
        for i in 16..80 {
            w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]).rotate_left(1);
        }
        let [mut a, mut b, mut c, mut d, mut e] = h;
        for (i, wi) in w.iter().enumerate() {
            let (f, k) = match i {
                0..=19 => ((b & c) | (!b & d), 0x5A82_7999),
                20..=39 => (b ^ c ^ d, 0x6ED9_EBA1),
                40..=59 => ((b & c) | (b & d) | (c & d), 0x8F1B_BCDC),
                _ => (b ^ c ^ d, 0xCA62_C1D6),
            };
            let t = a.rotate_left(5).wrapping_add(f).wrapping_add(e).wrapping_add(k).wrapping_add(*wi);
            e = d;
            d = c;
            c = b.rotate_left(30);
            b = a;
            a = t;
        }
        for (hi, v) in h.iter_mut().zip([a, b, c, d, e]) {
            *hi = hi.wrapping_add(v);
        }
    }

    let mut out = [0u8; 20];
    for (chunk, v) in out.chunks_exact_mut(4).zip(h) {
        chunk.copy_from_slice(&v.to_be_bytes());
    }
    out
}

/// Standard base64 with padding.
pub fn base64(data: &[u8]) -> String {
    const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(data.len().div_ceil(3) * 4);
    for chunk in data.chunks(3) {
        let b = [chunk[0], chunk.get(1).copied().unwrap_or(0), chunk.get(2).copied().unwrap_or(0)];
        let n = (u32::from(b[0]) << 16) | (u32::from(b[1]) << 8) | u32::from(b[2]);
        for i in 0..4 {
            if i <= chunk.len() {
                out.push(ALPHABET[((n >> (18 - 6 * i)) & 63) as usize] as char);
            } else {
                out.push('=');
            }
        }
    }
    out
}

/// `Sec-WebSocket-Accept` for a client's `Sec-WebSocket-Key`.
pub fn accept_key(key: &str) -> String {
    base64(&sha1(format!("{}{GUID}", key.trim()).as_bytes()))
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Opcode {
    Continuation = 0,
    Text = 1,
    Binary = 2,
    Close = 8,
    Ping = 9,
    Pong = 10,
}

impl Opcode {
    fn from_u8(v: u8) -> Option<Self> {
        Some(match v {
            0 => Self::Continuation,
            1 => Self::Text,
            2 => Self::Binary,
            8 => Self::Close,
            9 => Self::Ping,
            10 => Self::Pong,
            _ => return None,
        })
    }
}

/// A whole message from the client (fragments joined, control frames apart).
#[derive(Debug, PartialEq, Eq)]
pub enum Message {
    Text(String),
    Binary(Vec<u8>),
    Ping(Vec<u8>),
    Pong,
    Close,
}

fn invalid(what: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, what.to_string())
}

/// The header of a server frame (never masked) carrying `len` bytes.
pub fn frame_header(opcode: Opcode, len: usize) -> Vec<u8> {
    let mut h = Vec::with_capacity(10);
    h.push(0x80 | opcode as u8);
    if len < 126 {
        h.push(len as u8);
    } else if len <= 0xFFFF {
        h.push(126);
        h.extend_from_slice(&(len as u16).to_be_bytes());
    } else {
        h.push(127);
        h.extend_from_slice(&(len as u64).to_be_bytes());
    }
    h
}

/// Writes one unfragmented server frame.
pub fn write_frame(w: &mut impl Write, opcode: Opcode, payload: &[u8]) -> io::Result<()> {
    w.write_all(&frame_header(opcode, payload.len()))?;
    w.write_all(payload)?;
    w.flush()
}

/// Reads client frames into messages. Client frames must be masked; messages
/// over MAX_MESSAGE close the connection.
pub struct Reader<R> {
    inner: R,
    partial: Option<(Opcode, Vec<u8>)>,
}

impl<R: Read> Reader<R> {
    pub fn new(inner: R) -> Self {
        Self { inner, partial: None }
    }

    /// The next message; an error (or `Close`) ends the connection.
    pub fn read(&mut self) -> io::Result<Message> {
        loop {
            let mut head = [0u8; 2];
            self.inner.read_exact(&mut head)?;
            let fin = head[0] & 0x80 != 0;
            if head[0] & 0x70 != 0 {
                return Err(invalid("reserved bits set"));
            }
            let opcode = Opcode::from_u8(head[0] & 0x0F).ok_or_else(|| invalid("unknown opcode"))?;
            if head[1] & 0x80 == 0 {
                return Err(invalid("client frame not masked"));
            }
            let len = match head[1] & 0x7F {
                126 => {
                    let mut b = [0u8; 2];
                    self.inner.read_exact(&mut b)?;
                    u64::from(u16::from_be_bytes(b))
                }
                127 => {
                    let mut b = [0u8; 8];
                    self.inner.read_exact(&mut b)?;
                    u64::from_be_bytes(b)
                }
                n => u64::from(n),
            };
            let control = matches!(opcode, Opcode::Close | Opcode::Ping | Opcode::Pong);
            if control && (len > 125 || !fin) {
                return Err(invalid("bad control frame"));
            }
            let buffered = self.partial.as_ref().map_or(0, |p| p.1.len()) as u64;
            if len + buffered > MAX_MESSAGE as u64 {
                return Err(invalid("message too big"));
            }
            let mut mask = [0u8; 4];
            self.inner.read_exact(&mut mask)?;
            let mut payload = vec![0u8; len as usize];
            self.inner.read_exact(&mut payload)?;
            for (i, b) in payload.iter_mut().enumerate() {
                *b ^= mask[i % 4];
            }

            match opcode {
                Opcode::Close => return Ok(Message::Close),
                Opcode::Ping => return Ok(Message::Ping(payload)),
                Opcode::Pong => return Ok(Message::Pong),
                Opcode::Continuation => {
                    let Some((_, buf)) = self.partial.as_mut() else { return Err(invalid("continuation without a start")) };
                    buf.extend_from_slice(&payload);
                }
                Opcode::Text | Opcode::Binary => {
                    if self.partial.is_some() {
                        return Err(invalid("new message inside a fragmented one"));
                    }
                    self.partial = Some((opcode, payload));
                }
            }
            if fin {
                let (op, data) = self.partial.take().unwrap_or((Opcode::Binary, Vec::new()));
                return Ok(if op == Opcode::Text {
                    Message::Text(String::from_utf8(data).map_err(|_| invalid("text is not UTF-8"))?)
                } else {
                    Message::Binary(data)
                });
            }
        }
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    fn hex(b: &[u8]) -> String {
        b.iter().map(|x| format!("{x:02x}")).collect()
    }

    /// A client frame as browsers send it (masked).
    pub fn client_frame(opcode: u8, fin: bool, payload: &[u8]) -> Vec<u8> {
        let mask = [0x37, 0xFA, 0x21, 0x3D];
        let mut f = vec![if fin { 0x80 } else { 0 } | opcode];
        if payload.len() < 126 {
            f.push(0x80 | payload.len() as u8);
        } else {
            f.push(0x80 | 126);
            f.extend_from_slice(&(payload.len() as u16).to_be_bytes());
        }
        f.extend_from_slice(&mask);
        f.extend(payload.iter().enumerate().map(|(i, b)| b ^ mask[i % 4]));
        f
    }

    #[test]
    fn sha1_known_answers() {
        assert_eq!(hex(&sha1(b"")), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
        assert_eq!(hex(&sha1(b"abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
        let long = b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        assert_eq!(hex(&sha1(long)), "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
        assert_eq!(hex(&sha1(&[b'a'; 1000])), "291e9a6c66994949b57ba5e650361e98fc36b1ba");
    }

    #[test]
    fn base64_pads() {
        assert_eq!(base64(b""), "");
        assert_eq!(base64(b"f"), "Zg==");
        assert_eq!(base64(b"fo"), "Zm8=");
        assert_eq!(base64(b"foo"), "Zm9v");
        assert_eq!(base64(b"foobar"), "Zm9vYmFy");
    }

    #[test]
    fn accept_key_matches_the_rfc_example() {
        assert_eq!(accept_key("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    }

    #[test]
    fn headers_use_the_short_and_long_lengths() {
        assert_eq!(frame_header(Opcode::Binary, 5), vec![0x82, 5]);
        assert_eq!(frame_header(Opcode::Text, 300), vec![0x81, 126, 1, 44]);
        assert_eq!(frame_header(Opcode::Binary, 70_000), vec![0x82, 127, 0, 0, 0, 0, 0, 1, 0x11, 0x70]);
    }

    #[test]
    fn reads_masked_and_fragmented_messages() {
        let mut bytes = client_frame(1, true, b"pin:1234");
        bytes.extend(client_frame(2, false, &[1, 2]));
        bytes.extend(client_frame(9, true, b"hi")); // a ping between fragments
        bytes.extend(client_frame(0, true, &[3]));
        bytes.extend(client_frame(2, true, &[7; 200]));
        bytes.extend(client_frame(8, true, &[]));
        let mut r = Reader::new(&bytes[..]);
        assert_eq!(r.read().unwrap(), Message::Text("pin:1234".into()));
        assert_eq!(r.read().unwrap(), Message::Ping(b"hi".to_vec()));
        assert_eq!(r.read().unwrap(), Message::Binary(vec![1, 2, 3]));
        assert_eq!(r.read().unwrap(), Message::Binary(vec![7; 200]));
        assert_eq!(r.read().unwrap(), Message::Close);
        assert!(r.read().is_err());
    }

    #[test]
    fn refuses_unmasked_and_oversized_frames() {
        let unmasked = [0x82u8, 1, 0];
        assert!(Reader::new(&unmasked[..]).read().is_err());
        let mut big = vec![0x82u8, 0x80 | 127];
        big.extend_from_slice(&(MAX_MESSAGE as u64 + 1).to_be_bytes());
        big.extend_from_slice(&[0; 4]);
        assert!(Reader::new(&big[..]).read().is_err());
        let long_ping = client_frame(9, true, &[0; 126]);
        assert!(Reader::new(&long_ping[..]).read().is_err());
    }
}
