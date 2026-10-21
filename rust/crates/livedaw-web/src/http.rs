//! The HTTP side: just enough of HTTP/1.1 to serve the embedded pages and
//! upgrade `/ws` to a WebSocket. One request per connection.

use std::io::{self, Read};
use std::net::IpAddr;

/// Longest request head accepted (browsers send well under 2 KB).
pub const MAX_HEAD: usize = 8 * 1024;

/// A page or file built into the binary.
pub struct Asset {
    pub path: &'static str,
    pub content_type: &'static str,
    pub body: &'static [u8],
}

/// Everything the server serves besides the WebSocket. More pages (a phone
/// layout...) go here.
pub const ASSETS: &[Asset] = &[Asset { path: "/", content_type: "text/html; charset=utf-8", body: MIRROR_PAGE.as_bytes() }];

/// The screen mirror: one self-contained page (HTML, CSS and JS inline).
pub const MIRROR_PAGE: &str = include_str!("../web/mirror.html");

/// The WebSocket endpoint.
pub const WS_PATH: &str = "/ws";

#[derive(Debug, Default, PartialEq, Eq)]
pub struct Request {
    pub method: String,
    /// Without the query string.
    pub path: String,
    pub headers: Vec<(String, String)>,
}

impl Request {
    /// A header's value (names compare case-insensitively).
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(n, _)| n.eq_ignore_ascii_case(name)).map(|(_, v)| v.as_str())
    }

    /// Whether a comma-separated header lists `token` (case-insensitive).
    fn has_token(&self, name: &str, token: &str) -> bool {
        self.header(name).is_some_and(|v| v.split(',').any(|t| t.trim().eq_ignore_ascii_case(token)))
    }

    pub fn is_websocket(&self) -> bool {
        self.has_token("Connection", "upgrade") && self.has_token("Upgrade", "websocket")
    }
}

/// Parses a request head (up to and including the blank line).
pub fn parse_request(head: &str) -> Option<Request> {
    let mut lines = head.split("\r\n");
    let mut first = lines.next()?.split(' ');
    let (method, target, version) = (first.next()?, first.next()?, first.next()?);
    if !version.starts_with("HTTP/1.") || method.is_empty() {
        return None;
    }
    let path = target.split(['?', '#']).next().unwrap_or("/").to_string();
    let mut headers = Vec::new();
    for line in lines.take_while(|l| !l.is_empty()) {
        let (name, value) = line.split_once(':')?;
        headers.push((name.trim().to_string(), value.trim().to_string()));
    }
    Some(Request { method: method.to_string(), path, headers })
}

/// Reads a request head byte by byte (so nothing after it is consumed: a
/// WebSocket's first frame may follow at once).
pub fn read_request(r: &mut impl Read) -> io::Result<Option<Request>> {
    let mut head = Vec::with_capacity(1024);
    let mut byte = [0u8; 1];
    while !head.ends_with(b"\r\n\r\n") {
        if head.len() >= MAX_HEAD || r.read(&mut byte)? == 0 {
            return Ok(None);
        }
        head.push(byte[0]);
    }
    Ok(std::str::from_utf8(&head).ok().and_then(parse_request))
}

/// What to do with a request.
#[derive(Debug, PartialEq, Eq)]
pub enum Route {
    Asset(&'static Asset),
    WebSocket,
    /// Status code and reason.
    Error(u16, &'static str),
}

impl std::fmt::Debug for Asset {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.path)
    }
}

impl PartialEq for Asset {
    fn eq(&self, other: &Self) -> bool {
        self.path == other.path
    }
}

impl Eq for Asset {}

pub fn route(req: &Request) -> Route {
    if !req.header("Host").is_some_and(host_allowed) {
        return Route::Error(403, "Forbidden");
    }
    if req.path == WS_PATH {
        // A page from another site may not open the socket (browsers let any
        // page connect to any WebSocket; they always send Origin).
        let same_origin = req.header("Origin").is_none_or(|o| origin_host(o) == req.header("Host"));
        return match (req.method.as_str(), req.is_websocket(), same_origin) {
            (_, _, false) => Route::Error(403, "Forbidden"),
            ("GET", true, true) if req.header("Sec-WebSocket-Key").is_some() => Route::WebSocket,
            _ => Route::Error(400, "Bad Request"),
        };
    }
    match ASSETS.iter().find(|a| a.path == req.path) {
        Some(a) if req.method == "GET" || req.method == "HEAD" => Route::Asset(a),
        Some(_) => Route::Error(405, "Method Not Allowed"),
        None => Route::Error(404, "Not Found"),
    }
}

/// `host[:port]` of an Origin (`http://host:port`).
fn origin_host(origin: &str) -> Option<&str> {
    origin.split_once("://").map(|(_, rest)| rest.trim_end_matches('/'))
}

/// Hosts the pages answer to: IP addresses, localhost and single-label or
/// `.local` names. Anything else is a site's domain pointed at this machine
/// (DNS rebinding), which must not reach the console.
pub fn host_allowed(host: &str) -> bool {
    let name = if let Some(rest) = host.strip_prefix('[') {
        // [IPv6]:port
        return rest.split(']').next().is_some_and(|ip| ip.parse::<IpAddr>().is_ok());
    } else {
        host.rsplit_once(':').map_or(host, |(name, port)| if port.parse::<u16>().is_ok() { name } else { host })
    };
    let name = name.trim_end_matches('.').to_ascii_lowercase();
    !name.is_empty() && (name.parse::<IpAddr>().is_ok() || name == "localhost" || !name.contains('.') || name.ends_with(".local"))
}

/// A complete response with a body (the body is left out for HEAD).
pub fn response(status: u16, reason: &str, content_type: &str, body: &[u8], head_only: bool) -> Vec<u8> {
    let mut out = format!(
        "HTTP/1.1 {status} {reason}\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nCache-Control: no-store\r\n\
         X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\nConnection: close\r\n\r\n",
        body.len()
    )
    .into_bytes();
    if !head_only {
        out.extend_from_slice(body);
    }
    out
}

pub fn error_response(status: u16, reason: &str) -> Vec<u8> {
    response(status, reason, "text/plain; charset=utf-8", format!("{status} {reason}\n").as_bytes(), false)
}

/// The 101 that switches to the WebSocket protocol.
pub fn upgrade_response(key: &str) -> Vec<u8> {
    format!(
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {}\r\n\r\n",
        crate::ws::accept_key(key)
    )
    .into_bytes()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn req(text: &str) -> Request {
        parse_request(text).expect("parses")
    }

    #[test]
    fn parses_the_request_line_and_headers() {
        let r =
            req("GET /ws?x=1 HTTP/1.1\r\nHost: 10.0.0.2:8080\r\nUpgrade: WebSocket\r\nConnection: keep-alive, Upgrade\r\n\r\n");
        assert_eq!(r.method, "GET");
        assert_eq!(r.path, "/ws");
        assert_eq!(r.header("host"), Some("10.0.0.2:8080"));
        assert!(r.is_websocket());
        assert!(parse_request("GARBAGE\r\n\r\n").is_none());
        assert!(parse_request("GET / SPDY/3\r\n\r\n").is_none());
    }

    #[test]
    fn reads_only_the_head() {
        let bytes = b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\nNEXT";
        let mut r = &bytes[..];
        assert_eq!(read_request(&mut r).unwrap().unwrap().path, "/");
        assert_eq!(r, b"NEXT");
        let mut endless = std::io::repeat(b'a');
        assert!(read_request(&mut endless).unwrap().is_none());
    }

    #[test]
    fn routes_pages_and_the_socket() {
        let page = req("GET / HTTP/1.1\r\nHost: 192.168.1.20:8080\r\n\r\n");
        assert!(matches!(route(&page), Route::Asset(a) if a.content_type.starts_with("text/html")));
        let head = req("HEAD /?pin HTTP/1.1\r\nHost: 192.168.1.20\r\n\r\n");
        assert!(matches!(route(&head), Route::Asset(_)));
        assert_eq!(route(&req("GET /etc/passwd HTTP/1.1\r\nHost: localhost\r\n\r\n")), Route::Error(404, "Not Found"));
        assert_eq!(route(&req("POST / HTTP/1.1\r\nHost: localhost\r\n\r\n")), Route::Error(405, "Method Not Allowed"));
        assert_eq!(route(&req("GET / HTTP/1.1\r\n\r\n")), Route::Error(403, "Forbidden"));

        let ws = "GET /ws HTTP/1.1\r\nHost: 192.168.1.20:8080\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\
                  Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n";
        assert_eq!(route(&req(&format!("{ws}Origin: http://192.168.1.20:8080\r\n\r\n"))), Route::WebSocket);
        assert_eq!(route(&req(&format!("{ws}\r\n"))), Route::WebSocket);
        assert_eq!(route(&req(&format!("{ws}Origin: https://evil.example\r\n\r\n"))), Route::Error(403, "Forbidden"));
        assert_eq!(route(&req("GET /ws HTTP/1.1\r\nHost: localhost\r\n\r\n")), Route::Error(400, "Bad Request"));
    }

    #[test]
    fn refuses_foreign_host_names() {
        for ok in
            ["192.168.1.20:8080", "127.0.0.1", "localhost:8080", "[::1]:8080", "[fe80::1]", "studio-pc", "studio-pc.local:80"]
        {
            assert!(host_allowed(ok), "{ok}");
        }
        for bad in ["", "evil.example", "evil.example:8080", "192.168.1.20.nip.io", "[evil]:80", ":8080"] {
            assert!(!host_allowed(bad), "{bad}");
        }
    }

    #[test]
    fn upgrade_answers_with_the_accept_key() {
        let r = String::from_utf8(upgrade_response("dGhlIHNhbXBsZSBub25jZQ==")).unwrap();
        assert!(r.starts_with("HTTP/1.1 101 "));
        assert!(r.contains("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"));
        let head = String::from_utf8(response(200, "OK", "text/html", b"abc", true)).unwrap();
        assert!(head.contains("Content-Length: 3\r\n") && head.ends_with("\r\n\r\n"));
    }
}
