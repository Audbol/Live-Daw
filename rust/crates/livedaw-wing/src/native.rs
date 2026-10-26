//! The WING's Native protocol (TCP 2222): channel framing with the 0xdf
//! escape, the token stream of channel 2 (ids, navigation, values, data and
//! definition requests) and the meter requests of channel 4.
//!
//! Channel numbers on the wire are 0-based (`df d1` selects the "Audio
//! Engine" channel, `df d3` the meters), as in the protocol document's tables
//! and libwing.

use crate::map::Console;
use crate::meters::Meter;
use crate::schema::{schema, Def, Node, NodeType};
use crate::server::{Origin, Shared};
use crate::value::{self, Value};

pub const ESCAPE: u8 = 0xdf;
pub const CH_AUDIO: u8 = 1;
pub const CH_METERS: u8 = 3;

/// Appends `payload` escaped: every 0xdf becomes `df de`, which both the
/// document's receive routine and libwing decode back to one 0xdf.
pub fn escape_into(out: &mut Vec<u8>, payload: &[u8]) {
    for &b in payload {
        out.push(b);
        if b == ESCAPE {
            out.push(0xde);
        }
    }
}

/// A complete frame on `channel`.
pub fn frame(channel: u8, payload: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(payload.len() + payload.len() / 16 + 2);
    out.extend_from_slice(&[ESCAPE, 0xd0 + channel]);
    escape_into(&mut out, payload);
    out
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Event {
    Select(u8),
    Data(u8, u8),
}

/// Incremental de-framer.
#[derive(Default)]
pub struct Deframer {
    escaped: bool,
    channel: Option<u8>,
}

impl Deframer {
    pub fn push(&mut self, byte: u8, out: &mut Vec<Event>) {
        // Data before any channel selection goes to the Audio Engine channel.
        let ch = self.channel.unwrap_or(CH_AUDIO);
        if !self.escaped {
            if byte == ESCAPE {
                self.escaped = true;
            } else {
                out.push(Event::Data(ch, byte));
            }
            return;
        }
        self.escaped = false;
        match byte {
            0xde | ESCAPE => out.push(Event::Data(ch, ESCAPE)),
            0xd0..=0xdd => {
                self.channel = Some(byte - 0xd0);
                out.push(Event::Select(byte - 0xd0));
            }
            other => {
                out.push(Event::Data(ch, ESCAPE));
                out.push(Event::Data(ch, other));
            }
        }
    }
}

/// Appends a value token.
pub fn encode_value(out: &mut Vec<u8>, v: &Value) {
    match v {
        Value::Int(i) if (0..=0x3f).contains(i) => out.push(*i as u8),
        Value::Int(i) if i16::try_from(*i).is_ok() => {
            out.push(0xd3);
            out.extend_from_slice(&(*i as i16).to_be_bytes());
        }
        Value::Int(i) => {
            out.push(0xd4);
            out.extend_from_slice(&i.to_be_bytes());
        }
        Value::Float(f) => {
            out.push(0xd5);
            out.extend_from_slice(&f.to_be_bytes());
        }
        Value::Str(s) => {
            let b = s.as_bytes();
            let b = &b[..b.len().min(256)];
            if b.is_empty() {
                out.push(0xd0);
            } else if b.len() <= 64 {
                out.push(0x7f + b.len() as u8);
                out.extend_from_slice(b);
            } else {
                out.extend_from_slice(&[0xd1, (b.len() - 1) as u8]);
                out.extend_from_slice(b);
            }
        }
    }
}

/// `d7 <id> <value>`.
pub fn encode_data(out: &mut Vec<u8>, id: u32, v: &Value) {
    out.push(0xd7);
    out.extend_from_slice(&id.to_be_bytes());
    encode_value(out, v);
}

fn push_short(out: &mut Vec<u8>, s: &str) {
    let b = &s.as_bytes()[..s.len().min(255)];
    out.push(b.len() as u8);
    out.extend_from_slice(b);
}

/// A node definition response: `df <len> parent id index name longname flags payload`.
pub fn encode_def(out: &mut Vec<u8>, c: &Console, node: Node) {
    let s = schema();
    let Some(def) = s.def(node, c) else { return };
    let mut body = Vec::with_capacity(48);
    body.extend_from_slice(&s.id(s.parent(node)).to_be_bytes());
    body.extend_from_slice(&s.id(node).to_be_bytes());
    body.extend_from_slice(&s.index(node).to_be_bytes());
    push_short(&mut body, s.wire_name(node, c));
    push_short(&mut body, def.long_name);
    let flags = ((def.ty.wire() as u16) << 4) | (def.unit as u16 & 0x0f) | if def.read_only { 1 << 9 } else { 0 };
    body.extend_from_slice(&flags.to_be_bytes());
    def_payload(&mut body, def);
    out.push(0xdf);
    match u16::try_from(body.len()) {
        Ok(n) if n > 0 => out.extend_from_slice(&n.to_be_bytes()),
        _ => {
            out.extend_from_slice(&0u16.to_be_bytes());
            out.extend_from_slice(&(body.len() as u32).to_be_bytes());
        }
    }
    out.extend_from_slice(&body);
}

fn def_payload(body: &mut Vec<u8>, def: &Def) {
    match def.ty {
        NodeType::Node => {}
        NodeType::LinFloat | NodeType::LogFloat | NodeType::Fader => {
            body.extend_from_slice(&def.min.to_be_bytes());
            body.extend_from_slice(&def.max.to_be_bytes());
            body.extend_from_slice(&def.steps.to_be_bytes());
        }
        NodeType::Int => {
            body.extend_from_slice(&def.min_int.to_be_bytes());
            body.extend_from_slice(&def.max_int.to_be_bytes());
        }
        NodeType::StrEnum => {
            let items = def.items();
            body.extend_from_slice(&(items.len() as u16).to_be_bytes());
            for i in items {
                push_short(body, i.item);
                push_short(body, i.long);
            }
        }
        NodeType::FloatEnum => {
            let items = def.items();
            body.extend_from_slice(&(items.len() as u16).to_be_bytes());
            for i in items {
                body.extend_from_slice(&i.value.to_be_bytes());
                push_short(body, i.long);
            }
        }
        NodeType::Str => body.extend_from_slice(&def.max_len.to_be_bytes()),
    }
}

/// Where the token stream points.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Cursor {
    Node(Node),
    /// An id this console doesn't have.
    Unknown,
}

/// A meter request decoded from channel 4.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MeterRequest {
    Port(u16),
    Subscribe { report_id: u32, meters: Vec<Meter> },
    Renew { report_id: u32 },
}

/// The protocol state of one connection (or one OSC blob).
pub struct Session {
    deframer: Deframer,
    cursor: Cursor,
    audio: Vec<u8>,
    meter: Vec<u8>,
    /// The report id of the last `d4` (a collection that arrives after a
    /// renewal was resolved on its own belongs to it).
    meter_id: Option<u32>,
    origin: Origin,
    /// Responses for the client (channel 2 payload, unframed).
    pub out: Vec<u8>,
    pub meter_requests: Vec<MeterRequest>,
}

impl Session {
    pub fn new(origin: Origin) -> Self {
        Self {
            deframer: Deframer::default(),
            cursor: Cursor::Node(Node::Root),
            audio: Vec::new(),
            meter: Vec::new(),
            meter_id: None,
            origin,
            out: Vec::new(),
            meter_requests: Vec::new(),
        }
    }

    /// Starts at `node` (OSC blobs address a node).
    pub fn at(origin: Origin, node: Node) -> Self {
        let mut s = Self::new(origin);
        s.cursor = Cursor::Node(node);
        s
    }

    /// Feeds raw (framed) bytes from the client.
    pub fn feed(&mut self, shared: &Shared, bytes: &[u8]) {
        let mut events = Vec::with_capacity(bytes.len());
        for &b in bytes {
            self.deframer.push(b, &mut events);
        }
        for e in events {
            match e {
                Event::Select(_) => self.flush_meter(),
                Event::Data(CH_AUDIO, b) => {
                    self.audio.push(b);
                    while self.parse_audio(shared) {}
                    if self.audio.len() > 64 * 1024 {
                        self.audio.clear(); // garbage: resync
                    }
                }
                Event::Data(CH_METERS, b) => {
                    self.meter.push(b);
                    while self.parse_meter() {}
                    if self.meter.len() > 16 * 1024 {
                        self.meter.clear();
                    }
                }
                Event::Data(..) => {}
            }
        }
        // A renewal as the protocol document sends it (`df d3 d4 <id>`) has
        // nothing after it to show where it ends: take it as soon as it is in,
        // not at the client's next channel switch, which may come after the
        // meters have timed out.
        self.flush_meter();
    }

    /// Feeds unframed Audio Engine tokens (an OSC blob).
    pub fn feed_tokens(&mut self, shared: &Shared, tokens: &[u8]) {
        for &b in tokens {
            self.audio.push(b);
            while self.parse_audio(shared) {}
        }
    }

    fn set_cursor_child(&mut self, c: &Console, by: Result<u32, &str>) {
        let s = schema();
        let Cursor::Node(node) = self.cursor else { return };
        let next = match by {
            Ok(index) => {
                let children = s.children(node, c);
                children.get((index as usize).wrapping_sub(1)).copied()
            }
            Err(name) => s.child(node, name, c),
        };
        self.cursor = next.map_or(Cursor::Unknown, Cursor::Node);
    }

    /// Parses one token from `audio`; false if it needs more bytes.
    fn parse_audio(&mut self, shared: &Shared) -> bool {
        let Some(&op) = self.audio.first() else { return false };
        let need = match op {
            0x80..=0xbf => 1 + (op - 0x7f) as usize,
            0xc0..=0xcf => 1 + (op - 0xbf) as usize,
            0xd1 => match self.audio.get(1) {
                Some(&n) => 2 + n as usize + 1,
                None => return false,
            },
            0xd2 | 0xd3 => 3,
            0xd4..=0xd7 => 5,
            0xd9 => 2,
            _ => 1,
        };
        if self.audio.len() < need {
            return false;
        }
        let tok: Vec<u8> = self.audio.drain(..need).collect();
        let be32 = |b: &[u8]| u32::from_be_bytes([b[1], b[2], b[3], b[4]]);
        match op {
            0x00..=0x3f => self.set(shared, Value::Int(op as i32)),
            0x40..=0x7f => self.set_cursor_child(&shared.console(), Ok((op - 0x3f) as u32)),
            0x80..=0xbf => self.set(shared, Value::Str(String::from_utf8_lossy(&tok[1..]).into_owned())),
            0xc0..=0xcf => {
                let name = String::from_utf8_lossy(&tok[1..]).into_owned();
                self.set_cursor_child(&shared.console(), Err(&name));
            }
            0xd0 => self.set(shared, Value::str("")),
            0xd1 => self.set(shared, Value::Str(String::from_utf8_lossy(&tok[2..]).into_owned())),
            0xd2 => {
                let i = u16::from_be_bytes([tok[1], tok[2]]) as u32 + 1;
                self.set_cursor_child(&shared.console(), Ok(i));
            }
            0xd3 => self.set(shared, Value::Int(i16::from_be_bytes([tok[1], tok[2]]) as i32)),
            0xd4 => self.set(shared, Value::Int(be32(&tok) as i32)),
            0xd5 => self.set(shared, Value::Float(f32::from_bits(be32(&tok)))),
            0xd6 => {
                let raw = f32::from_bits(be32(&tok));
                if let Cursor::Node(n) = self.cursor {
                    let c = shared.console();
                    if let Some(v) = schema().def(n, &c).and_then(|d| value::from_raw(d, raw)) {
                        drop(c);
                        self.set(shared, v);
                    }
                }
            }
            0xd7 => {
                let id = be32(&tok);
                let c = shared.console();
                self.cursor = schema().by_id(id, &c).map_or(Cursor::Unknown, Cursor::Node);
            }
            0xd8 => {
                if let Cursor::Node(n) = self.cursor {
                    let _ = shared.apply_toggle(n, self.origin);
                }
            }
            0xd9 => {
                if let Cursor::Node(n) = self.cursor {
                    let _ = shared.apply_step(n, tok[1] as i8 as i32, self.origin);
                }
            }
            0xda => self.cursor = Cursor::Node(Node::Root),
            0xdb => {
                if let Cursor::Node(n) = self.cursor {
                    self.cursor = Cursor::Node(schema().parent(n));
                }
            }
            0xdc => self.data_request(shared),
            0xdd => self.def_request(shared),
            _ => {}
        }
        true
    }

    fn set(&mut self, shared: &Shared, v: Value) {
        if let Cursor::Node(n) = self.cursor {
            let c = shared.console();
            let v = match (schema().def(n, &c), &v) {
                // Text for a number ("-oo", "1k5").
                (Some(def), Value::Str(t)) if def.ty != NodeType::Str => value::parse_display(def, t).unwrap_or(v),
                _ => v,
            };
            drop(c);
            if shared.apply_set(n, &v, self.origin).is_err() {
                // A refused set (read-only, out of the list...) answers the
                // value that stands, so the client shows it instead of what it
                // asked for. WING Edit asks for the model this way: it sets
                // `/$syscfg/$cnsmdl` to "ModelRequest" right after connecting
                // and takes the model from the reply.
                if let Some(v) = shared.console().get(n) {
                    encode_data(&mut self.out, schema().id(n), &v);
                }
            }
        }
    }

    fn data_request(&mut self, shared: &Shared) {
        if let Cursor::Node(n) = self.cursor {
            let c = shared.console();
            let s = schema();
            if s.is_leaf(n, &c) {
                if let Some(v) = c.get(n) {
                    encode_data(&mut self.out, s.id(n), &v);
                }
            } else {
                let mut leaves = Vec::new();
                c.leaves(n, &mut leaves);
                for l in leaves {
                    if let Some(v) = c.get(l) {
                        encode_data(&mut self.out, s.id(l), &v);
                    }
                }
            }
        }
        self.out.push(0xde);
    }

    fn def_request(&mut self, shared: &Shared) {
        if let Cursor::Node(n) = self.cursor {
            let c = shared.console();
            let s = schema();
            if s.is_leaf(n, &c) {
                encode_def(&mut self.out, &c, n);
            } else {
                for ch in s.children(n, &c) {
                    encode_def(&mut self.out, &c, ch);
                }
            }
        }
        self.out.push(0xde);
    }

    /// Parses one meter request; false if it needs more bytes.
    fn parse_meter(&mut self) -> bool {
        let Some(&op) = self.meter.first() else { return false };
        match op {
            0xd3 => {
                if self.meter.len() < 3 {
                    return false;
                }
                let port = u16::from_be_bytes([self.meter[1], self.meter[2]]);
                self.meter.drain(..3);
                self.meter_requests.push(MeterRequest::Port(port));
                true
            }
            0xd4 => {
                if self.meter.len() < 6 {
                    return false; // a lone renewal: see flush_meter
                }
                let report_id = u32::from_be_bytes([self.meter[1], self.meter[2], self.meter[3], self.meter[4]]);
                self.meter_id = Some(report_id);
                if self.meter[5] != 0xdc {
                    self.meter.drain(..5);
                    self.meter_requests.push(MeterRequest::Renew { report_id });
                    return true;
                }
                let Some(end) = self.meter.iter().skip(6).position(|&b| b == 0xde).map(|p| p + 6) else { return false };
                let meters = decode_meters(&self.meter[6..end]);
                self.meter.drain(..=end);
                self.meter_requests.push(MeterRequest::Subscribe { report_id, meters });
                true
            }
            // A collection whose report id came in an earlier read.
            0xdc if self.meter_id.is_some() => {
                let Some(end) = self.meter.iter().position(|&b| b == 0xde) else { return false };
                let meters = decode_meters(&self.meter[1..end]);
                self.meter.drain(..=end);
                self.meter_requests.push(MeterRequest::Subscribe { report_id: self.meter_id.unwrap_or(0), meters });
                true
            }
            _ => {
                self.meter.remove(0);
                true
            }
        }
    }

    fn flush_meter(&mut self) {
        if self.meter.len() == 5 && self.meter[0] == 0xd4 {
            let report_id = u32::from_be_bytes([self.meter[1], self.meter[2], self.meter[3], self.meter[4]]);
            self.meter_id = Some(report_id);
            self.meter_requests.push(MeterRequest::Renew { report_id });
            self.meter.clear();
        }
    }
}

fn decode_meters(raw: &[u8]) -> Vec<Meter> {
    let mut out = Vec::new();
    let mut family = None;
    for &b in raw {
        match b {
            0xa0..=0xaf => {
                family = Some(b);
                if let Some(m) = Meter::from_wire(b, None) {
                    out.push(m); // monitor and RTA take no index
                }
            }
            0x00..=0x7f => {
                if let Some(m) = family.and_then(|f| Meter::from_wire(f, Some(b))) {
                    out.push(m);
                }
            }
            _ => {}
        }
        if out.len() >= 512 {
            break;
        }
    }
    out
}

/// Runs Native tokens sent in an OSC blob from `node`; returns the response tokens.
pub fn run_blob(shared: &Shared, node: Node, data: &[u8], origin: Origin) -> Vec<u8> {
    let mut s = Session::at(origin, node);
    s.feed_tokens(shared, data);
    s.out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn framing_round_trips_escapes() {
        let payload = [0xd7, 0xdf, 0xd4, 0xab, 0xcf, 0x08, 0xdf];
        let wire = frame(1, &payload);
        assert_eq!(&wire[..2], &[0xdf, 0xd1]);
        let mut d = Deframer::default();
        let mut ev = Vec::new();
        for b in wire {
            d.push(b, &mut ev);
        }
        let data: Vec<u8> = ev.iter().filter_map(|e| if let Event::Data(1, b) = e { Some(*b) } else { None }).collect();
        assert_eq!(data, payload);
        // The document's example: D7DFDED4ABCF08 is D7DFD4ABCF08; DF + a non-channel byte passes.
        let mut d = Deframer::default();
        let mut ev = Vec::new();
        for b in [0xdf, 0xd1, 0xd7, 0xdf, 0xde, 0xd4, 0xab, 0xdf, 0xe0, 0x66] {
            d.push(b, &mut ev);
        }
        let data: Vec<u8> = ev.iter().filter_map(|e| if let Event::Data(1, b) = e { Some(*b) } else { None }).collect();
        assert_eq!(data, [0xd7, 0xdf, 0xd4, 0xab, 0xdf, 0xe0, 0x66]);
    }

    #[test]
    fn value_tokens() {
        let mut v = Vec::new();
        encode_value(&mut v, &Value::Int(1));
        encode_value(&mut v, &Value::Int(300));
        encode_value(&mut v, &Value::Int(100_000));
        encode_value(&mut v, &Value::Float(-144.0));
        encode_value(&mut v, &Value::str("HALL"));
        encode_value(&mut v, &Value::str(""));
        assert_eq!(
            v,
            [
                0x01, 0xd3, 0x01, 0x2c, 0xd4, 0x00, 0x01, 0x86, 0xa0, 0xd5, 0xc3, 0x10, 0x00, 0x00, 0x83, b'H', b'A', b'L', b'L',
                0xd0
            ]
        );
    }
}
