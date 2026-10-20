//! The WING's OSC protocol (UDP 2223), as the protocol document describes it:
//! get/set by path (or `/#<id>`), node listings and dumps (`*`, `?`, `#`),
//! multi-parameter node sets (`/ch/1 ,s "fdr=3,mute=1"`), toggles (`,i -1`),
//! the `/%<port>` reply-port prefix, Native commands in blobs, console info
//! (`/?`) and the three event subscriptions (`/*s`, `/*S`, `/*b`).

use crate::map::{Console, SetError};
use crate::native;
use crate::schema::{schema, unit, Def, Node, NodeType};
use crate::server::{Origin, Shared};
use crate::value::{self, Value};

/// OSC over UDP is limited to 32 KB on the WING.
pub const MAX_PACKET: usize = 32 * 1024;

#[derive(Clone, Debug, PartialEq)]
pub enum Arg {
    Int(i32),
    Float(f32),
    Str(String),
    Blob(Vec<u8>),
}

#[derive(Clone, Debug, PartialEq)]
pub struct Message {
    pub addr: String,
    pub args: Vec<Arg>,
}

impl Message {
    pub fn new(addr: impl Into<String>, args: Vec<Arg>) -> Self {
        Self { addr: addr.into(), args }
    }
}

fn push_str(buf: &mut Vec<u8>, s: &str) {
    buf.extend_from_slice(s.as_bytes());
    let total = (s.len() + 1).div_ceil(4) * 4;
    buf.resize(buf.len() + total - s.len(), 0);
}

/// Encodes a message (no arguments: a bare address, as the WING's own examples).
pub fn encode(msg: &Message) -> Vec<u8> {
    let mut buf = Vec::with_capacity(64);
    push_str(&mut buf, &msg.addr);
    if msg.args.is_empty() {
        return buf;
    }
    let mut tags = String::from(",");
    for a in &msg.args {
        tags.push(match a {
            Arg::Int(_) => 'i',
            Arg::Float(_) => 'f',
            Arg::Str(_) => 's',
            Arg::Blob(_) => 'b',
        });
    }
    push_str(&mut buf, &tags);
    for a in &msg.args {
        match a {
            Arg::Int(v) => buf.extend_from_slice(&v.to_be_bytes()),
            Arg::Float(v) => buf.extend_from_slice(&v.to_be_bytes()),
            Arg::Str(s) => push_str(&mut buf, &s.replace('\0', "")),
            Arg::Blob(b) => {
                buf.extend_from_slice(&(b.len() as i32).to_be_bytes());
                buf.extend_from_slice(b);
                buf.resize(buf.len() + (4 - b.len() % 4) % 4, 0);
            }
        }
    }
    buf
}

fn read_str(b: &[u8], i: &mut usize) -> Option<String> {
    let start = *i;
    let nul = b.get(start..)?.iter().position(|&c| c == 0)?;
    let s = String::from_utf8_lossy(&b[start..start + nul]).into_owned();
    *i = (start + (nul + 1).div_ceil(4) * 4).min(b.len());
    Some(s)
}

fn read_u32(b: &[u8], i: &mut usize) -> Option<[u8; 4]> {
    let v: [u8; 4] = b.get(*i..*i + 4)?.try_into().ok()?;
    *i += 4;
    Some(v)
}

/// Decodes a message; the type tag string may be missing (older clients).
pub fn decode(b: &[u8]) -> Option<Message> {
    let mut i = 0;
    let addr = read_str(b, &mut i)?;
    if !addr.starts_with('/') {
        return None; // bundles aren't supported
    }
    if i >= b.len() {
        return Some(Message { addr, args: vec![] });
    }
    let tags = read_str(b, &mut i)?;
    let mut args = Vec::new();
    for t in tags.strip_prefix(',')?.chars() {
        args.push(match t {
            'i' => Arg::Int(i32::from_be_bytes(read_u32(b, &mut i)?)),
            'f' => Arg::Float(f32::from_be_bytes(read_u32(b, &mut i)?)),
            's' => Arg::Str(read_str(b, &mut i)?),
            'b' => {
                let n = usize::try_from(i32::from_be_bytes(read_u32(b, &mut i)?)).ok()?;
                let data = b.get(i..i + n)?.to_vec();
                i += n + (4 - n % 4) % 4;
                Arg::Blob(data)
            }
            'T' => Arg::Int(1),
            'F' => Arg::Int(0),
            _ => return None,
        });
    }
    Some(Message { addr, args })
}

/// The event subscription formats.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SubFormat {
    /// `/*s`: triplets like a get reply.
    Triplet,
    /// `/*S`: the value alone, in its native type.
    Native,
    /// `/*b`: Native protocol tokens in a blob addressed to `/`.
    Binary,
}

/// The reply to a parameter get: `,s` (strings, enums), `,sff` (floats: display,
/// 0..1 position, value) or `,sfi` (integers: display, 0..1 position, and the
/// step from the bottom of the range, as the console counts it: "1" of 1..12
/// is 0, "0" of -200..200 is 200).
pub fn param_reply(addr: &str, def: &Def, v: &Value) -> Message {
    let text = value::display(def, v);
    let args = match def.ty {
        NodeType::Str | NodeType::StrEnum | NodeType::Node => vec![Arg::Str(text)],
        NodeType::Int => {
            let step = v.as_i32().unwrap_or(0).saturating_sub(def.min_int);
            vec![Arg::Str(text), Arg::Float(value::to_raw(def, v)), Arg::Int(step)]
        }
        _ => vec![Arg::Str(text), Arg::Float(value::to_raw(def, v)), Arg::Float(v.as_f32().unwrap_or(0.0))],
    };
    Message::new(addr, args)
}

/// A `/*S` event: the value in the parameter's own type.
pub fn native_event(addr: &str, def: &Def, v: &Value) -> Message {
    let arg = match (def.ty, v) {
        (NodeType::Str | NodeType::StrEnum, _) => Arg::Str(value::display(def, v)),
        (NodeType::Int, v) => Arg::Int(v.as_i32().unwrap_or(0)),
        (_, v) => Arg::Float(v.as_f32().unwrap_or(0.0)),
    };
    Message::new(addr, vec![arg])
}

fn ack(addr: &str, status: &str) -> Message {
    Message::new(addr, vec![Arg::Str(status.into())])
}

fn status_text(e: &SetError) -> &'static str {
    match e {
        SetError::NotFound => "NODE NOT FOUND",
        SetError::IsNode => "NODE IS NOT PAR",
        SetError::ReadOnly | SetError::BadValue | SetError::Unsupported(_) => "VALUE ERROR",
    }
}

/// Text of a value inside node strings: quoted when it holds the grammar's characters.
fn node_text(def: &Def, v: &Value) -> String {
    let t = value::display(def, v);
    if t.contains([',', '=', ':', ' ', '\'']) {
        format!("'{}'", t.replace('\'', ""))
    } else {
        t
    }
}

/// The node data dump (`,s *`): what a snapshot would save (no read-only or
/// `$` nodes), `a.b=v` paths with leading dots to go up.
pub(crate) fn dump(c: &Console, node: Node) -> String {
    let mut out = String::new();
    let mut stack: Vec<String> = Vec::new(); // path of the current position, relative to `node`
    fn walk(c: &Console, node: Node, prefix: &mut Vec<String>, stack: &mut Vec<String>, out: &mut String) {
        let s = schema();
        for ch in s.children(node, c) {
            let name = s.name(ch, c);
            if name.starts_with('$') {
                continue;
            }
            let Some(def) = s.def(ch, c) else { continue };
            if def.is_leaf() {
                if def.read_only {
                    continue;
                }
                let Some(v) = c.get(ch) else { continue };
                // Navigate from `stack` to `prefix`.
                let common = stack.iter().zip(prefix.iter()).take_while(|(a, b)| a == b).count();
                let ups = stack.len() - common;
                for _ in 0..ups {
                    out.push('.');
                }
                for p in &prefix[common..] {
                    out.push_str(p);
                    out.push('.');
                }
                *stack = prefix.clone();
                out.push_str(&name);
                out.push('=');
                out.push_str(&node_text(def, &v));
                out.push(',');
            } else {
                prefix.push(name.into_owned());
                walk(c, ch, prefix, stack, out);
                prefix.pop();
            }
        }
    }
    walk(c, node, &mut Vec::new(), &mut stack, &mut out);
    out
}

/// One line of a node description (`?`, or `#` with values).
fn describe_line(c: &Console, node: Node, with_value: bool) -> String {
    let s = schema();
    let name = s.name(node, c);
    let Some(def) = s.def(node, c) else { return format!(" {name}") };
    let mut line = format!(" {name}");
    if with_value && def.is_leaf() {
        if let Some(v) = c.get(node) {
            line.push(' ');
            line.push_str(&value::display(def, &v));
        }
    }
    // The console doesn't mark read-only parameters.
    let u = unit::name(def.unit);
    let us = if u.is_empty() { String::new() } else { format!(" {u}") };
    // Bounds as the console writes the values ("20.0 .. 20k00", "-15.0 .. +15.0").
    let bound = |x: f32| {
        let t = value::format_float(def, x);
        if x > 0.0 && value::signed(def) {
            format!("+{t}")
        } else {
            t
        }
    };
    match def.ty {
        NodeType::Node => line.push_str(" (node)"),
        NodeType::LinFloat => line.push_str(&format!(" lin [{} .. {}{us}], {} steps", bound(def.min), bound(def.max), def.steps)),
        NodeType::LogFloat => line.push_str(&format!(" log [{} .. {}{us}], {} steps", bound(def.min), bound(def.max), def.steps)),
        NodeType::Fader => line.push_str(" fader [-oo .. 10.0 dB], 1024 steps"),
        NodeType::Int => line.push_str(&format!(" int [{} .. {}{us}]", def.min_int, def.max_int)),
        NodeType::StrEnum => {
            let items: Vec<&str> = def.items().iter().map(|i| i.item).collect();
            line.push_str(&format!(" list [{}]", items.join(", ")));
        }
        NodeType::FloatEnum => {
            let items: Vec<String> = def.items().iter().map(|i| value::format_float_item(def, i.value)).collect();
            line.push_str(&format!(" list [{}]", items.join(", ")));
        }
        NodeType::Str => line.push_str(&format!(" string [{}]", def.max_len)),
    }
    line
}

pub(crate) fn describe(c: &Console, node: Node, with_values: bool) -> String {
    let s = schema();
    let nodes = if s.is_leaf(node, c) { vec![node] } else { s.children(node, c) };
    nodes.into_iter().map(|n| describe_line(c, n, with_values)).collect::<Vec<_>>().join("\n")
}

/// Applies a node-string set ("fdr=3,mute=1" from `base`, or
/// "/ch.1.fdr=-1,mute=0,.2.fdr=0" from the root).
fn node_set(shared: &Shared, base: Node, text: &str, origin: Origin) -> Result<(), &'static str> {
    let s = schema();
    let mut cur = base;
    let mut result = Ok(());
    for raw in text.split(',') {
        let mut el = raw.trim();
        if el.is_empty() {
            continue;
        }
        if let Some(rest) = el.strip_prefix('/') {
            cur = Node::Root;
            el = rest;
        }
        while let Some(rest) = el.strip_prefix('.') {
            cur = s.parent(cur);
            el = rest;
        }
        let Some((path, val)) = el.split_once('=') else {
            result = Err("INCOMPLETE DATA");
            continue;
        };
        let mut parts: Vec<&str> = path.split('.').filter(|p| !p.is_empty()).collect();
        let Some(leaf) = parts.pop() else {
            result = Err("NODE NOT FOUND");
            continue;
        };
        let c = shared.console();
        for p in parts {
            match s.child(cur, p, &c) {
                Some(n) => cur = n,
                None => {
                    result = Err("NODE NOT FOUND");
                    continue;
                }
            }
        }
        let Some(node) = s.child(cur, leaf, &c) else {
            result = Err("NODE NOT FOUND");
            continue;
        };
        let Some(def) = s.def(node, &c) else {
            result = Err("NODE NOT FOUND");
            continue;
        };
        if !def.is_leaf() {
            result = Err("NODE IS NOT PAR");
            continue;
        }
        let v = value::parse_display(def, val).unwrap_or_else(|| Value::str(val.trim().trim_matches('\'')));
        drop(c);
        if let Err(e) = shared.apply_set(node, &v, origin) {
            result = Err(status_text(&e));
        }
    }
    result
}

/// Splits `/%<port>` off an address.
fn reply_port(addr: &str) -> (Option<u16>, &str) {
    if let Some(rest) = addr.strip_prefix("/%") {
        let digits = rest.bytes().take_while(u8::is_ascii_digit).count();
        if digits > 0 {
            if let Ok(p) = rest[..digits].parse::<u16>() {
                let tail = &rest[digits..];
                return (Some(p), if tail.is_empty() { "/" } else { tail });
            }
        }
    }
    (None, addr)
}

/// What a request asks of the server besides its replies.
pub enum Effect {
    Subscribe(SubFormat),
}

/// Handles one request; returns the replies (and where to send them: `None`
/// = back to the sender, `Some(port)` = the sender's address on that port).
pub fn handle(shared: &Shared, msg: &Message, origin: Origin) -> (Vec<Message>, Option<u16>, Option<Effect>) {
    let (port, addr) = reply_port(&msg.addr);
    let mut out = Vec::new();
    let mut effect = None;
    match addr {
        "/?" => out.push(Message::new("/?", vec![Arg::Str(shared.info_string(None))])),
        "/*s" => effect = Some(Effect::Subscribe(SubFormat::Triplet)),
        "/*S" => effect = Some(Effect::Subscribe(SubFormat::Native)),
        "/*b" => effect = Some(Effect::Subscribe(SubFormat::Binary)),
        _ => {
            let s = schema();
            let c = shared.console();
            let node = if let Some(hex) = addr.strip_prefix("/#") {
                u32::from_str_radix(hex.trim_end_matches('/'), 16).ok().and_then(|id| s.by_id(id, &c))
            } else {
                s.resolve(addr, &c)
            };
            let Some(node) = node else { return (out, port, None) };
            let path = s.path(node, &c);
            let leaf = s.is_leaf(node, &c);
            match msg.args.as_slice() {
                [] if leaf => {
                    if let (Some(def), Some(v)) = (s.def(node, &c), c.get(node)) {
                        out.push(param_reply(&path, def, &v));
                    }
                }
                [] => {
                    let names = s.children(node, &c).into_iter().map(|n| Arg::Str(s.name(n, &c).into_owned())).collect();
                    out.push(Message::new(path, names));
                }
                [Arg::Str(q)] if !leaf && q == "*" => out.push(Message::new(path, vec![Arg::Str(dump(&c, node))])),
                [Arg::Str(q)] if q == "?" => out.push(Message::new(path, vec![Arg::Str(describe(&c, node, false))])),
                [Arg::Str(q)] if q == "#" => out.push(Message::new(path, vec![Arg::Str(describe(&c, node, true))])),
                [Arg::Blob(data)] => {
                    drop(c);
                    let reply = native::run_blob(shared, node, data, origin);
                    if !reply.is_empty() {
                        out.push(Message::new(path, vec![Arg::Blob(reply)]));
                    } else {
                        // A set: acknowledged like the document's blob examples.
                        out.push(ack("/*", "OK"));
                    }
                }
                [Arg::Str(text)] if !leaf => {
                    drop(c);
                    let status = node_set(shared, node, text, origin);
                    let ack_addr = if node == Node::Root { "/*".to_owned() } else { format!("{path}*") };
                    out.push(ack(&ack_addr, status.err().unwrap_or("OK")));
                }
                [arg] if leaf => {
                    let def = s.def(node, &c);
                    drop(c);
                    let Some(def) = def else { return (out, port, None) };
                    let result = match arg {
                        // -1 toggles a 0/1 integer.
                        Arg::Int(-1) if def.ty == NodeType::Int && def.min_int == 0 && def.max_int == 1 => {
                            shared.apply_toggle(node, origin)
                        }
                        Arg::Int(i) => shared.apply_set(node, &Value::Int(*i), origin),
                        Arg::Float(f) => shared.apply_set(node, &Value::Float(*f), origin),
                        Arg::Str(t) => {
                            let v = value::parse_display(def, t).unwrap_or_else(|| Value::str(t.as_str()));
                            shared.apply_set(node, &v, origin)
                        }
                        Arg::Blob(_) => Err(SetError::BadValue),
                    };
                    // Sets aren't echoed; errors are reported.
                    if let Err(e) = result {
                        out.push(ack(&format!("{path}*"), status_text(&e)));
                    }
                }
                _ => {}
            }
        }
    }
    (out, port, effect)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn codec_matches_the_documented_bytes() {
        // "/?" -> 2f3f0000
        assert_eq!(encode(&Message::new("/?", vec![])), [0x2f, 0x3f, 0, 0]);
        // /ch/1/fdr ,sff "-oo" 0.0 -144.0 (protocol document p. 21)
        let reply = Message::new("/ch/1/fdr", vec![Arg::Str("-oo".into()), Arg::Float(0.0), Arg::Float(-144.0)]);
        let hex: String = encode(&reply).iter().map(|b| format!("{b:02x}")).collect();
        assert_eq!(hex, "2f63682f312f6664720000002c736666000000002d6f6f0000000000c3100000");
        // /ch/1/mute ,sfi "1" 1.0 1
        let mute = Message::new("/ch/1/mute", vec![Arg::Str("1".into()), Arg::Float(1.0), Arg::Int(1)]);
        let hex: String = encode(&mute).iter().map(|b| format!("{b:02x}")).collect();
        assert_eq!(hex, "2f63682f312f6d75746500002c73666900000000310000003f80000000000001");
        for m in [reply, mute, Message::new("/x", vec![Arg::Blob(vec![1, 2, 3, 4, 5])])] {
            assert_eq!(decode(&encode(&m)), Some(m));
        }
        // The compliant empty-tag form.
        assert_eq!(decode(b"/?\0\0,\0\0\0"), Some(Message::new("/?", vec![])));
        assert_eq!(reply_port("/%10027/ch/1/mute"), (Some(10027), "/ch/1/mute"));
    }

    #[test]
    fn describes_and_replies_like_the_console() {
        use crate::map::WingState;
        let e = livedaw_engine::Engine::new(0);
        e.set_num_channels(40);
        let st = WingState::default();
        let c = Console::new(&e, &st);
        let s = schema();
        let node = |path: &str| s.resolve(path, &c).unwrap_or_else(|| panic!("{path}"));
        c.set(node("/ch/1/dyn/mdl"), &Value::str("COMP")).unwrap();
        // Lines of a factory-set WING's `,s ?` descriptions (firmware 3.1):
        // no "r/o", decimals and "+" as in the values, kHz, units on ints,
        // float enums as lists.
        for (path, want) in [
            ("/ch/1", " 1 (node)"),
            ("/ch/1/fdr", " fdr fader [-oo .. 10.0 dB], 1024 steps"),
            ("/ch/1/$fdr", " $fdr fader [-oo .. 10.0 dB], 1024 steps"),
            ("/ch/1/in/set/trim", " trim lin [-18.0 .. +18.0 dB], 361 steps"),
            ("/ch/1/pan", " pan lin [-100 .. +100], 201 steps"),
            ("/ch/1/wid", " wid lin [-150 .. 150 %], 61 steps"),
            ("/ch/1/dynxo/depth", " depth lin [-30.0 .. 30.0 dB], 121 steps"),
            ("/io/in/LCL/1/g", " g lin [-2.5 .. 45.0 dB], 20 steps"),
            ("/$ctl/$globals/mousespd", " mousespd lin [0.10 .. 2.00], 191 steps"),
            ("/play/$pos", " $pos lin [0:00 .. 9:59:59], 36000 steps"),
            ("/cards/wlive/1/$stat/sdfree", " sdfree lin [0:00:00 .. 0:00:00], 36000000 steps"),
            ("/ch/1/flt/lcf", " lcf log [20.0 .. 2k00 Hz], 641 steps"),
            ("/ch/1/flt/hcf", " hcf log [50.0 .. 20k00 Hz], 833 steps"),
            ("/ch/1/dynsc/f", " f log [20 .. 20k0 Hz], 901 steps"),
            ("/ch/1/peq/1f", " 1f log [20 .. 20k0 Hz], 960 steps"),
            ("/ch/1/peq/1q", " 1q log [0.44 .. 10.00], 181 steps"),
            ("/ch/1/dyn/rel", " rel log [4 .. 4000 ms], 121 steps"),
            ("/$ctl/cfg/lights/chedit", " chedit int [5 .. 100 %]"),
            ("/$globals/clkrate", " clkrate list [48000.0, 44100.0]"),
            ("/cfg/rta/eqrange", " eqrange list [30, 60]"),
            (
                "/ch/1/dyn/ratio",
                " ratio list [1.1, 1.2, 1.3, 1.5, 1.7, 2.0, 2.5, 3.0, 3.5, 4.0, 5.0, 6.0, 8.0,  10,  20,  50, 100]",
            ),
        ] {
            let n = node(path);
            let line = if s.is_leaf(n, &c) { describe(&c, n, false) } else { describe_line(&c, n, false) };
            assert_eq!(line, want, "{path}");
        }
        // Integers: the reply's number counts from the bottom of the range.
        let reply = |path: &str, v: Value| param_reply(path, s.def(node(path), &c).unwrap(), &v).args;
        assert_eq!(reply("/ch/1/col", Value::Int(1)), [Arg::Str("1".into()), Arg::Float(0.0), Arg::Int(0)]);
        assert_eq!(reply("/$stat/ppm", Value::Int(0)), [Arg::Str("0".into()), Arg::Float(0.5), Arg::Int(200)]);
        assert_eq!(reply("/$ctl/$stat/sof", Value::Int(0)).last(), Some(&Arg::Int(1)));
        assert_eq!(reply("/ch/1/mute", Value::Int(1)), [Arg::Str("1".into()), Arg::Float(1.0), Arg::Int(1)]);
    }
}
