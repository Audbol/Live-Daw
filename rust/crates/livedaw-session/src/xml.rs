//! A small, dependency-free XML DOM: enough for show files and plugin
//! descriptions, compatible with files written by JUCE's XmlElement.
//!
//! The parser never panics on malformed input; it returns an [`XmlError`] with
//! the byte offset. Nesting depth is limited so a hostile file can't overflow
//! the stack.

use std::fmt::{self, Write as _};

const MAX_DEPTH: usize = 512;

#[derive(Debug, Clone, PartialEq)]
pub enum Node {
    Element(Element),
    Text(String),
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct Element {
    pub name: String,
    pub attrs: Vec<(String, String)>,
    pub children: Vec<Node>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct XmlError {
    pub offset: usize,
    pub message: String,
}

impl fmt::Display for XmlError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "XML error at byte {}: {}", self.offset, self.message)
    }
}

impl std::error::Error for XmlError {}

// ---- element API ------------------------------------------------------------------

impl Element {
    pub fn new(name: impl Into<String>) -> Self {
        Self { name: name.into(), attrs: Vec::new(), children: Vec::new() }
    }

    pub fn has_tag(&self, name: &str) -> bool {
        self.name == name
    }

    pub fn attr(&self, name: &str) -> Option<&str> {
        self.attrs.iter().find(|(k, _)| k == name).map(|(_, v)| v.as_str())
    }

    pub fn has_attr(&self, name: &str) -> bool {
        self.attr(name).is_some()
    }

    /// Sets (or replaces) an attribute.
    pub fn set(&mut self, name: &str, value: impl fmt::Display) -> &mut Self {
        let value = value.to_string();
        match self.attrs.iter_mut().find(|(k, _)| k == name) {
            Some((_, v)) => *v = value,
            None => self.attrs.push((name.to_owned(), value)),
        }
        self
    }

    pub fn set_bool(&mut self, name: &str, value: bool) -> &mut Self {
        self.set(name, if value { 1 } else { 0 })
    }

    pub fn str_or<'a>(&'a self, name: &str, default: &'a str) -> &'a str {
        self.attr(name).unwrap_or(default)
    }

    /// Like JUCE's getIntAttribute: a leading integer, 0 if none, `default` if absent.
    pub fn int_or(&self, name: &str, default: i32) -> i32 {
        self.attr(name).map_or(default, parse_int_prefix)
    }

    /// Like JUCE's getDoubleAttribute.
    pub fn double_or(&self, name: &str, default: f64) -> f64 {
        self.attr(name).map_or(default, parse_double_prefix)
    }

    /// Like JUCE's getBoolAttribute: true if the value starts with 1, t or y.
    pub fn bool_or(&self, name: &str, default: bool) -> bool {
        self.attr(name).map_or(default, parse_bool)
    }

    pub fn elements(&self) -> impl Iterator<Item = &Element> {
        self.children.iter().filter_map(|c| match c {
            Node::Element(e) => Some(e),
            Node::Text(_) => None,
        })
    }

    pub fn children_named<'a>(&'a self, name: &'a str) -> impl Iterator<Item = &'a Element> + 'a {
        self.elements().filter(move |e| e.name == name)
    }

    pub fn child(&self, name: &str) -> Option<&Element> {
        self.elements().find(|e| e.name == name)
    }

    pub fn add(&mut self, child: Element) -> &mut Element {
        self.children.push(Node::Element(child));
        match self.children.last_mut() {
            Some(Node::Element(e)) => e,
            _ => unreachable!(),
        }
    }

    pub fn add_new(&mut self, name: &str) -> &mut Element {
        self.add(Element::new(name))
    }

    pub fn add_text(&mut self, text: impl Into<String>) {
        self.children.push(Node::Text(text.into()));
    }

    /// All text inside this element, recursively (JUCE's getAllSubText).
    pub fn text(&self) -> String {
        let mut out = String::new();
        self.collect_text(&mut out);
        out
    }

    fn collect_text(&self, out: &mut String) {
        for c in &self.children {
            match c {
                Node::Text(t) => out.push_str(t),
                Node::Element(e) => e.collect_text(out),
            }
        }
    }

    /// Serialises with an XML declaration, in JUCE's indented style.
    pub fn to_document(&self) -> String {
        let mut out = String::from("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\n");
        self.write(&mut out, 0);
        out.push('\n');
        out
    }

    /// Serialises this element alone, on one line (for embedding/exchange).
    pub fn to_compact(&self) -> String {
        let mut out = String::new();
        self.write_compact(&mut out);
        out
    }

    fn write_open(&self, out: &mut String) {
        out.push('<');
        out.push_str(&self.name);
        for (k, v) in &self.attrs {
            let _ = write!(out, " {k}=\"");
            escape_into(out, v, true);
            out.push('"');
        }
    }

    fn write(&self, out: &mut String, indent: usize) {
        for _ in 0..indent {
            out.push(' ');
        }
        self.write_open(out);
        if self.children.is_empty() {
            out.push_str("/>");
            return;
        }
        out.push('>');
        if self.children.iter().all(|c| matches!(c, Node::Text(_))) {
            for c in &self.children {
                if let Node::Text(t) = c {
                    escape_into(out, t, false);
                }
            }
        } else {
            out.push('\n');
            for c in &self.children {
                match c {
                    Node::Element(e) => e.write(out, indent + 2),
                    Node::Text(t) => {
                        for _ in 0..indent + 2 {
                            out.push(' ');
                        }
                        escape_into(out, t, false);
                    }
                }
                out.push('\n');
            }
            for _ in 0..indent {
                out.push(' ');
            }
        }
        let _ = write!(out, "</{}>", self.name);
    }

    fn write_compact(&self, out: &mut String) {
        self.write_open(out);
        if self.children.is_empty() {
            out.push_str("/>");
            return;
        }
        out.push('>');
        for c in &self.children {
            match c {
                Node::Element(e) => e.write_compact(out),
                Node::Text(t) => escape_into(out, t, false),
            }
        }
        let _ = write!(out, "</{}>", self.name);
    }
}

fn escape_into(out: &mut String, s: &str, attribute: bool) {
    for c in s.chars() {
        match c {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' if attribute => out.push_str("&quot;"),
            '\'' if attribute => out.push_str("&apos;"),
            c if (c as u32) < 32 && (attribute || !matches!(c, '\n' | '\t')) => {
                let _ = write!(out, "&#{};", c as u32);
            }
            c => out.push(c),
        }
    }
}

// ---- lenient number parsing (JUCE String::getIntValue / getDoubleValue) --------------

pub fn parse_int_prefix(s: &str) -> i32 {
    let s = s.trim_start();
    let (neg, digits) = match s.as_bytes().first() {
        Some(b'-') => (true, &s[1..]),
        Some(b'+') => (false, &s[1..]),
        _ => (false, s),
    };
    let mut v: i64 = 0;
    for b in digits.bytes() {
        if !b.is_ascii_digit() {
            break;
        }
        v = (v * 10 + i64::from(b - b'0')).min(i64::from(i32::MAX) + 1);
    }
    let v = if neg { -v } else { v };
    v.clamp(i64::from(i32::MIN), i64::from(i32::MAX)) as i32
}

pub fn parse_double_prefix(s: &str) -> f64 {
    let s = s.trim_start();
    let b = s.as_bytes();
    let mut i = 0;
    if i < b.len() && (b[i] == b'-' || b[i] == b'+') {
        i += 1;
    }
    let digits_start = i;
    while i < b.len() && b[i].is_ascii_digit() {
        i += 1;
    }
    if i < b.len() && b[i] == b'.' {
        i += 1;
        while i < b.len() && b[i].is_ascii_digit() {
            i += 1;
        }
    }
    if i == digits_start || (i == digits_start + 1 && b[digits_start] == b'.') {
        return 0.0;
    }
    let mantissa_end = i;
    if i < b.len() && (b[i] == b'e' || b[i] == b'E') {
        let mut j = i + 1;
        if j < b.len() && (b[j] == b'-' || b[j] == b'+') {
            j += 1;
        }
        let exp_digits = j;
        while j < b.len() && b[j].is_ascii_digit() {
            j += 1;
        }
        if j > exp_digits {
            i = j;
        }
    }
    let v = s[..i].parse::<f64>().or_else(|_| s[..mantissa_end].parse::<f64>()).unwrap_or(0.0);
    if v.is_finite() {
        v
    } else {
        0.0
    }
}

pub fn parse_bool(s: &str) -> bool {
    matches!(s.trim_start().as_bytes().first(), Some(b'1' | b't' | b'T' | b'y' | b'Y'))
}

// ---- parser -----------------------------------------------------------------------

/// Parses a document and returns its root element.
pub fn parse(text: &str) -> Result<Element, XmlError> {
    let mut p = Parser { s: text.as_bytes(), text, i: 0 };
    p.skip_bom();
    p.skip_misc()?;
    if p.i >= p.s.len() || p.s[p.i] != b'<' {
        return Err(p.err("no root element"));
    }
    let root = p.element(0)?;
    p.skip_misc()?;
    if p.i < p.s.len() {
        return Err(p.err("content after the root element"));
    }
    Ok(root)
}

struct Parser<'a> {
    s: &'a [u8],
    text: &'a str,
    i: usize,
}

impl Parser<'_> {
    fn err(&self, message: &str) -> XmlError {
        XmlError { offset: self.i, message: message.to_owned() }
    }

    fn starts(&self, pat: &str) -> bool {
        self.s[self.i..].starts_with(pat.as_bytes())
    }

    fn skip_bom(&mut self) {
        if self.starts("\u{feff}") {
            self.i += 3;
        }
    }

    fn skip_ws(&mut self) {
        while self.i < self.s.len() && self.s[self.i].is_ascii_whitespace() {
            self.i += 1;
        }
    }

    fn skip_past(&mut self, end: &str) -> Result<(), XmlError> {
        match find(&self.s[self.i..], end.as_bytes()) {
            Some(pos) => {
                self.i += pos + end.len();
                Ok(())
            }
            None => Err(self.err(&format!("missing '{end}'"))),
        }
    }

    /// Whitespace, comments, processing instructions and DOCTYPE outside the root.
    fn skip_misc(&mut self) -> Result<(), XmlError> {
        loop {
            self.skip_ws();
            if self.starts("<?") {
                self.skip_past("?>")?;
            } else if self.starts("<!--") {
                self.skip_past("-->")?;
            } else if self.starts("<!DOCTYPE") || self.starts("<!doctype") {
                self.skip_doctype()?;
            } else {
                return Ok(());
            }
        }
    }

    fn skip_doctype(&mut self) -> Result<(), XmlError> {
        let mut depth = 0usize;
        while self.i < self.s.len() {
            match self.s[self.i] {
                b'[' => depth += 1,
                b']' => depth = depth.saturating_sub(1),
                b'>' if depth == 0 => {
                    self.i += 1;
                    return Ok(());
                }
                _ => {}
            }
            self.i += 1;
        }
        Err(self.err("unterminated DOCTYPE"))
    }

    fn name(&mut self) -> Result<String, XmlError> {
        let start = self.i;
        while self.i < self.s.len() {
            let c = self.s[self.i];
            if c.is_ascii_whitespace() || matches!(c, b'=' | b'>' | b'/' | b'<' | b'"' | b'\'') {
                break;
            }
            self.i += 1;
        }
        if self.i == start {
            return Err(self.err("expected a name"));
        }
        Ok(self.text[start..self.i].to_owned())
    }

    fn element(&mut self, depth: usize) -> Result<Element, XmlError> {
        if depth > MAX_DEPTH {
            return Err(self.err("elements nested too deeply"));
        }
        self.i += 1; // '<'
        let mut el = Element::new(self.name()?);

        loop {
            self.skip_ws();
            if self.i >= self.s.len() {
                return Err(self.err("unterminated start tag"));
            }
            match self.s[self.i] {
                b'/' => {
                    if !self.starts("/>") {
                        return Err(self.err("expected '/>'"));
                    }
                    self.i += 2;
                    return Ok(el);
                }
                b'>' => {
                    self.i += 1;
                    break;
                }
                _ => {
                    let key = self.name()?;
                    self.skip_ws();
                    if self.i >= self.s.len() || self.s[self.i] != b'=' {
                        return Err(self.err("expected '=' after attribute name"));
                    }
                    self.i += 1;
                    self.skip_ws();
                    let value = self.quoted()?;
                    if !el.has_attr(&key) {
                        el.attrs.push((key, value));
                    }
                }
            }
        }

        // Content
        let mut text = String::new();
        loop {
            if self.i >= self.s.len() {
                return Err(self.err(&format!("missing </{}>", el.name)));
            }
            if self.starts("</") {
                self.i += 2;
                let close = self.name()?;
                if close != el.name {
                    return Err(self.err(&format!("</{close}> does not close <{}>", el.name)));
                }
                self.skip_ws();
                if self.i >= self.s.len() || self.s[self.i] != b'>' {
                    return Err(self.err("expected '>'"));
                }
                self.i += 1;
                flush_text(&mut el, &mut text);
                return Ok(el);
            } else if self.starts("<!--") {
                self.skip_past("-->")?;
            } else if self.starts("<![CDATA[") {
                self.i += 9;
                let start = self.i;
                self.skip_past("]]>")?;
                text.push_str(&self.text[start..self.i - 3]);
            } else if self.starts("<?") {
                self.skip_past("?>")?;
            } else if self.s[self.i] == b'<' {
                flush_text(&mut el, &mut text);
                let child = self.element(depth + 1)?;
                el.children.push(Node::Element(child));
            } else {
                let start = self.i;
                while self.i < self.s.len() && self.s[self.i] != b'<' {
                    self.i += 1;
                }
                self.decode_into(&mut text, start, self.i)?;
            }
        }
    }

    fn quoted(&mut self) -> Result<String, XmlError> {
        let quote = match self.s.get(self.i) {
            Some(&q @ (b'"' | b'\'')) => q,
            _ => return Err(self.err("expected a quoted value")),
        };
        self.i += 1;
        let start = self.i;
        while self.i < self.s.len() && self.s[self.i] != quote {
            self.i += 1;
        }
        if self.i >= self.s.len() {
            return Err(self.err("unterminated attribute value"));
        }
        let mut out = String::new();
        self.decode_into(&mut out, start, self.i)?;
        self.i += 1;
        Ok(out)
    }

    /// Decodes entity references in text[start..end].
    fn decode_into(&self, out: &mut String, start: usize, end: usize) -> Result<(), XmlError> {
        let raw = &self.text[start..end];
        let mut rest = raw;
        while let Some(amp) = rest.find('&') {
            out.push_str(&rest[..amp]);
            rest = &rest[amp..];
            let Some(semi) = rest.find(';').filter(|&p| p <= 12) else {
                // A bare '&': keep it, as lenient parsers do.
                out.push('&');
                rest = &rest[1..];
                continue;
            };
            let entity = &rest[1..semi];
            let decoded = match entity {
                "lt" => Some('<'),
                "gt" => Some('>'),
                "amp" => Some('&'),
                "quot" => Some('"'),
                "apos" => Some('\''),
                _ => {
                    let code = if let Some(hex) = entity.strip_prefix("#x").or_else(|| entity.strip_prefix("#X")) {
                        u32::from_str_radix(hex, 16).ok()
                    } else if let Some(dec) = entity.strip_prefix('#') {
                        dec.parse::<u32>().ok()
                    } else {
                        None
                    };
                    code.and_then(char::from_u32)
                }
            };
            match decoded {
                Some(c) => {
                    out.push(c);
                    rest = &rest[semi + 1..];
                }
                None => {
                    out.push('&');
                    rest = &rest[1..];
                }
            }
        }
        out.push_str(rest);
        Ok(())
    }
}

fn flush_text(el: &mut Element, text: &mut String) {
    // Whitespace between elements is formatting, not content.
    if !text.trim().is_empty() {
        el.children.push(Node::Text(std::mem::take(text)));
    }
    text.clear();
}

fn find(hay: &[u8], needle: &[u8]) -> Option<usize> {
    hay.windows(needle.len()).position(|w| w == needle)
}

/// Shortest decimal text that reads back as the same f32.
pub fn fmt_f32(v: f32) -> String {
    if v.is_finite() {
        format!("{v}")
    } else {
        "0".to_owned()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip() {
        let mut root = Element::new("Root");
        root.set("a", "x \"q\" & <y>\n").set("n", 3);
        root.add_new("Child").set("v", 1.5);
        root.add_new("T").add_text("hello & bye");
        let text = root.to_document();
        let back = parse(&text).unwrap();
        assert_eq!(back, root);
        assert_eq!(parse(&root.to_compact()).unwrap(), root);
    }

    #[test]
    fn juce_style_document() {
        let doc = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\n<LiveDAWShow version=\"2\" name=\"Gig &amp; Co\">\n  \
                   <!-- comment -->\n  <Console outputProtection=\"1\">\n    <Monitor afl=\"0\"/>\n  </Console>\n  \
                   <State>QUJD&#10;REVG</State>\n</LiveDAWShow>\n";
        let root = parse(doc).unwrap();
        assert_eq!(root.name, "LiveDAWShow");
        assert_eq!(root.attr("name"), Some("Gig & Co"));
        assert_eq!(root.int_or("version", 0), 2);
        assert!(root.child("Console").unwrap().bool_or("outputProtection", false));
        assert_eq!(root.child("State").unwrap().text(), "QUJD\nREVG");
    }

    #[test]
    fn lenient_numbers() {
        assert_eq!(parse_int_prefix(" 12abc"), 12);
        assert_eq!(parse_int_prefix("-7"), -7);
        assert_eq!(parse_int_prefix("x"), 0);
        assert_eq!(parse_int_prefix("99999999999"), i32::MAX);
        assert_eq!(parse_double_prefix("-3.5dB"), -3.5);
        assert_eq!(parse_double_prefix("1e3"), 1000.0);
        assert_eq!(parse_double_prefix("2e"), 2.0);
        assert_eq!(parse_double_prefix("."), 0.0);
        assert_eq!(parse_double_prefix("nan"), 0.0);
        assert!(parse_bool("true") && parse_bool("1") && parse_bool("Yes"));
        assert!(!parse_bool("0") && !parse_bool("false") && !parse_bool(""));
        assert_eq!(fmt_f32(0.1), "0.1");
        assert_eq!(fmt_f32(-3.0), "-3");
    }

    #[test]
    fn malformed_input_is_an_error_not_a_panic() {
        for bad in ["", "<", "<a", "<a>", "<a></b>", "<a b></a>", "<a b=\"1></a>", "<a/><b/>", "text", "<a>&#xFFFFFFFF;</a"] {
            assert!(parse(bad).is_err(), "{bad:?} should fail");
        }
        let deep = "<a>".repeat(MAX_DEPTH + 10);
        assert!(parse(&deep).is_err());
        // Every prefix of a valid document fails cleanly.
        let doc = parse("<r x=\"1\"><c>t&amp;</c><![CDATA[<raw>]]></r>").unwrap().to_document();
        for i in 0..doc.len() {
            if doc.is_char_boundary(i) {
                let _ = parse(&doc[..i]);
            }
        }
    }

    #[test]
    fn cdata_and_entities() {
        let r = parse("<r><![CDATA[a<b]]>&#65;&#x42;&bogus;&amp</r>").unwrap();
        assert_eq!(r.text(), "a<bAB&bogus;&amp");
    }
}
