//! The server: a std::net listener thread, and per browser a thread reading
//! its requests (then its WebSocket messages) and one writing its frames.
//!
//! Frames never queue up: a browser acknowledges every frame it has drawn and
//! has at most MAX_IN_FLIGHT unacknowledged, and each frame carries the newest
//! image of every tile the browser lacks (see `screen`). So a slow browser
//! gets fewer frames, never stale ones, and the host never waits for the
//! network: it only puts tiles into the shared screen.

use crate::auth::{PinGuard, Verdict};
use crate::http::{self, Route};
use crate::input::{self, Decoded, Event, Queue};
use crate::screen::{Screen, View, MSG_PONG};
use crate::ws::{self, Message, Opcode};
use std::collections::VecDeque;
use std::io::{ErrorKind, Write};
use std::net::{IpAddr, Ipv4Addr, Shutdown, SocketAddr, TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

pub const DEFAULT_PORT: u16 = 8080;
/// Browsers connected at once (with or without the PIN yet).
pub const MAX_CLIENTS: usize = 8;
/// Frames a browser may have unacknowledged.
pub const MAX_IN_FLIGHT: u32 = 2;
/// A browser that acknowledges nothing for this long gets frames again (in
/// case an acknowledgement was lost).
pub const ACK_TIMEOUT: Duration = Duration::from_secs(3);
/// A browser that has not entered the PIN by then is dropped.
pub const AUTH_TIMEOUT: Duration = Duration::from_secs(120);
/// How long a browser may take to send its request.
const REQUEST_TIMEOUT: Duration = Duration::from_secs(10);
/// A browser that takes longer than this to accept a frame is dropped.
const WRITE_TIMEOUT: Duration = Duration::from_secs(10);
/// A wrong PIN is answered after this delay.
const WRONG_PIN_DELAY: Duration = Duration::from_millis(1000);

#[derive(Clone, Debug)]
pub struct Config {
    pub bind: IpAddr,
    /// 0 = any free port (tests).
    pub port: u16,
    /// 4-8 digits, or empty for none.
    pub pin: String,
    /// Browsers watch but their input is ignored.
    pub view_only: bool,
}

impl Default for Config {
    fn default() -> Self {
        Self { bind: IpAddr::V4(Ipv4Addr::UNSPECIFIED), port: DEFAULT_PORT, pin: String::new(), view_only: false }
    }
}

#[derive(Clone, Debug)]
pub struct ClientInfo {
    pub addr: SocketAddr,
    /// Has entered the PIN (or none is needed): gets frames.
    pub watching: bool,
    pub seconds: u64,
}

#[derive(Clone, Debug, Default)]
pub struct Status {
    pub addr: Option<SocketAddr>,
    pub error: Option<String>,
    pub clients: Vec<ClientInfo>,
    pub bytes_sent: u64,
    pub frames_sent: u64,
}

/// What a browser's writer thread waits for.
#[derive(Default)]
struct Outbox {
    /// Control and text messages, sent before any frame.
    messages: VecDeque<(Opcode, Vec<u8>)>,
    /// The screen has news.
    dirty: bool,
    in_flight: u32,
    last_ack: Option<Instant>,
    closed: bool,
}

struct Client {
    id: u32,
    addr: SocketAddr,
    since: Instant,
    watching: AtomicBool,
    stream: TcpStream,
    outbox: Mutex<Outbox>,
    wake: Condvar,
}

impl Client {
    fn send(&self, opcode: Opcode, payload: Vec<u8>) {
        lock(&self.outbox).messages.push_back((opcode, payload));
        self.wake.notify_one();
    }

    fn text(&self, json: &str) {
        self.send(Opcode::Text, json.as_bytes().to_vec());
    }

    fn mark_dirty(&self) {
        lock(&self.outbox).dirty = true;
        self.wake.notify_one();
    }

    fn close(&self) {
        lock(&self.outbox).closed = true;
        self.wake.notify_one();
        let _ = self.stream.shutdown(Shutdown::Both);
    }
}

struct Shared {
    view_only: bool,
    pin: Mutex<PinGuard>,
    screen: Mutex<Screen>,
    clients: Mutex<Vec<Arc<Client>>>,
    input: Mutex<Queue>,
    stop: AtomicBool,
    next_id: AtomicU32,
    bytes_sent: AtomicU64,
    frames_sent: AtomicU64,
}

fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

/// The running server; stops when dropped.
pub struct WebServer {
    shared: Arc<Shared>,
    addr: Option<SocketAddr>,
    error: Option<String>,
    listener: Option<JoinHandle<()>>,
}

impl WebServer {
    /// Starts listening. A port that cannot be bound leaves the server
    /// without a listener (see `status().error`), not failing the host.
    pub fn start(config: &Config) -> Self {
        let shared = Arc::new(Shared {
            view_only: config.view_only,
            pin: Mutex::new(PinGuard::new(config.pin.trim())),
            screen: Mutex::new(Screen::default()),
            clients: Mutex::new(Vec::new()),
            input: Mutex::new(Queue::default()),
            stop: AtomicBool::new(false),
            next_id: AtomicU32::new(1),
            bytes_sent: AtomicU64::new(0),
            frames_sent: AtomicU64::new(0),
        });
        let bound = TcpListener::bind(SocketAddr::new(config.bind, config.port)).and_then(|l| {
            l.set_nonblocking(true)?;
            Ok(l)
        });
        match bound {
            Ok(listener) => {
                let addr = listener.local_addr().ok();
                let s = shared.clone();
                let handle = std::thread::Builder::new().name("web-listen".into()).spawn(move || listen(&s, &listener)).ok();
                Self { shared, addr, error: None, listener: handle }
            }
            Err(e) => Self { shared, addr: None, error: Some(format!("port {}: {e}", config.port)), listener: None },
        }
    }

    pub fn stop(&mut self) {
        self.shared.stop.store(true, Ordering::Relaxed);
        if let Some(h) = self.listener.take() {
            let _ = h.join();
        }
        for c in lock(&self.shared.clients).drain(..) {
            c.close();
        }
    }

    pub fn addr(&self) -> Option<SocketAddr> {
        self.addr
    }

    pub fn status(&self) -> Status {
        let clients = lock(&self.shared.clients)
            .iter()
            .map(|c| ClientInfo {
                addr: c.addr,
                watching: c.watching.load(Ordering::Relaxed),
                seconds: c.since.elapsed().as_secs(),
            })
            .collect();
        Status {
            addr: self.addr,
            error: self.error.clone(),
            clients,
            bytes_sent: self.shared.bytes_sent.load(Ordering::Relaxed),
            frames_sent: self.shared.frames_sent.load(Ordering::Relaxed),
        }
    }

    /// Whether any browser watches (the host snapshots only then).
    pub fn watched(&self) -> bool {
        lock(&self.shared.clients).iter().any(|c| c.watching.load(Ordering::Relaxed))
    }

    /// A new screen size (or a restart after nobody watched): returns the
    /// generation `put_tile` must quote.
    pub fn reset_screen(&self, width: u32, height: u32, tile: u32) -> u32 {
        let generation = lock(&self.shared.screen).reset(width, height, tile);
        self.publish();
        generation
    }

    /// See `Screen::changed`.
    pub fn changed_tiles(&self, pixels: &[u8], stride: usize, bpp: usize, candidates: &[u32]) -> Vec<u32> {
        lock(&self.shared.screen).changed(pixels, stride, bpp, candidates)
    }

    pub fn put_tile(&self, generation: u32, index: u32, format: u8, data: &[u8]) -> bool {
        lock(&self.shared.screen).put(generation, index, format, data)
    }

    /// Tells the browsers' writers that tiles were put.
    pub fn publish(&self) {
        for c in lock(&self.shared.clients).iter() {
            c.mark_dirty();
        }
    }

    pub fn take_input(&self) -> Option<Event> {
        lock(&self.shared.input).pop()
    }
}

impl Drop for WebServer {
    fn drop(&mut self) {
        self.stop();
    }
}

fn listen(shared: &Arc<Shared>, listener: &TcpListener) {
    while !shared.stop.load(Ordering::Relaxed) {
        match listener.accept() {
            Ok((stream, peer)) => {
                let s = shared.clone();
                let _ = std::thread::Builder::new().name("web-client".into()).spawn(move || serve(&s, stream, peer));
            }
            Err(e) if e.kind() == ErrorKind::WouldBlock => std::thread::sleep(Duration::from_millis(20)),
            Err(_) => std::thread::sleep(Duration::from_millis(100)),
        }
        // Drop browsers that never entered the PIN.
        for c in lock(&shared.clients).iter() {
            if !c.watching.load(Ordering::Relaxed) && c.since.elapsed() > AUTH_TIMEOUT {
                c.close();
            }
        }
    }
}

/// One connection: an HTTP request, then maybe a WebSocket.
fn serve(shared: &Arc<Shared>, mut stream: TcpStream, peer: SocketAddr) {
    let _ = stream.set_nonblocking(false);
    let _ = stream.set_nodelay(true);
    let _ = stream.set_read_timeout(Some(REQUEST_TIMEOUT));
    let _ = stream.set_write_timeout(Some(WRITE_TIMEOUT));
    let Ok(Some(req)) = http::read_request(&mut stream) else { return };
    match http::route(&req) {
        Route::Asset(a) => {
            let _ = stream.write_all(&http::response(200, "OK", a.content_type, a.body, req.method == "HEAD"));
            let _ = stream.flush();
        }
        Route::Error(code, reason) => {
            let _ = stream.write_all(&http::error_response(code, reason));
            let _ = stream.flush();
        }
        Route::WebSocket => {
            if shared.stop.load(Ordering::Relaxed) || lock(&shared.clients).len() >= MAX_CLIENTS {
                let _ = stream.write_all(&http::error_response(503, "Service Unavailable"));
                return;
            }
            let key = req.header("Sec-WebSocket-Key").unwrap_or_default().to_string();
            if stream.write_all(&http::upgrade_response(&key)).is_ok() {
                let _ = stream.set_read_timeout(None);
                websocket(shared, stream, peer);
            }
        }
    }
}

fn websocket(shared: &Arc<Shared>, stream: TcpStream, peer: SocketAddr) {
    let (Ok(reader), Ok(writer)) = (stream.try_clone(), stream.try_clone()) else { return };
    let needs_pin = lock(&shared.pin).required();
    let client = Arc::new(Client {
        id: shared.next_id.fetch_add(1, Ordering::Relaxed),
        addr: peer,
        since: Instant::now(),
        watching: AtomicBool::new(!needs_pin),
        stream,
        outbox: Mutex::new(Outbox { dirty: true, ..Outbox::default() }),
        wake: Condvar::new(),
    });
    lock(&shared.clients).push(client.clone());
    client.text(&format!("{{\"t\":\"hello\",\"pin\":{needs_pin},\"viewOnly\":{}}}", shared.view_only));

    let writer_thread = {
        let (s, c) = (shared.clone(), client.clone());
        std::thread::Builder::new().name("web-send".into()).spawn(move || write_loop(&s, &c, writer))
    };
    read_loop(shared, &client, reader);

    client.close();
    lock(&shared.clients).retain(|c| !Arc::ptr_eq(c, &client));
    if let Ok(h) = writer_thread {
        let _ = h.join();
    }
}

fn read_loop(shared: &Shared, client: &Client, stream: TcpStream) {
    let mut reader = ws::Reader::new(stream);
    while !shared.stop.load(Ordering::Relaxed) {
        let Ok(message) = reader.read() else { return };
        match message {
            Message::Close => {
                client.send(Opcode::Close, Vec::new());
                return;
            }
            Message::Ping(data) => client.send(Opcode::Pong, data),
            Message::Pong => {}
            Message::Text(text) => {
                if let Some(pin) = text.strip_prefix("pin:") {
                    enter_pin(shared, client, pin);
                }
            }
            Message::Binary(data) => {
                if !client.watching.load(Ordering::Relaxed) {
                    continue;
                }
                match input::decode(&data, client.id) {
                    Some(Decoded::Input(e)) if !shared.view_only => lock(&shared.input).push(e),
                    Some(Decoded::Ack(_)) => {
                        let mut o = lock(&client.outbox);
                        o.in_flight = o.in_flight.saturating_sub(1);
                        o.last_ack = Some(Instant::now());
                        drop(o);
                        client.wake.notify_one();
                    }
                    Some(Decoded::Ping(stamp)) => {
                        let mut m = vec![MSG_PONG];
                        m.extend_from_slice(&stamp);
                        client.send(Opcode::Binary, m);
                    }
                    _ => {}
                }
            }
        }
    }
}

fn enter_pin(shared: &Shared, client: &Client, pin: &str) {
    if client.watching.load(Ordering::Relaxed) {
        return;
    }
    let verdict = lock(&shared.pin).check(client.addr.ip(), pin.trim(), Instant::now());
    match verdict {
        Verdict::Accepted => {
            client.watching.store(true, Ordering::Relaxed);
            client.text("{\"t\":\"ok\"}");
            client.mark_dirty();
        }
        Verdict::Wrong => {
            std::thread::sleep(WRONG_PIN_DELAY);
            client.text("{\"t\":\"wrong\"}");
        }
        Verdict::Wait(d) => client.text(&format!("{{\"t\":\"wait\",\"s\":{}}}", d.as_secs().max(1))),
    }
}

fn write_loop(shared: &Shared, client: &Client, mut stream: TcpStream) {
    let mut view = View::default();
    loop {
        let (messages, frame_due) = {
            let mut o = lock(&client.outbox);
            loop {
                if o.closed || shared.stop.load(Ordering::Relaxed) {
                    return;
                }
                if o.in_flight > 0 && o.last_ack.is_some_and(|t| t.elapsed() > ACK_TIMEOUT) {
                    o.in_flight = 0;
                }
                let frame_due = o.dirty && o.in_flight < MAX_IN_FLIGHT && client.watching.load(Ordering::Relaxed);
                if frame_due || !o.messages.is_empty() {
                    if frame_due {
                        o.dirty = false;
                    }
                    break (std::mem::take(&mut o.messages), frame_due);
                }
                o = client.wake.wait_timeout(o, Duration::from_millis(500)).map(|(g, _)| g).unwrap_or_else(|e| e.into_inner().0);
            }
        };
        for (opcode, payload) in &messages {
            if ws::write_frame(&mut stream, *opcode, payload).is_err() || *opcode == Opcode::Close {
                client.close();
                return;
            }
            shared.bytes_sent.fetch_add(payload.len() as u64, Ordering::Relaxed);
        }
        if frame_due {
            let frame = lock(&shared.screen).collect(&mut view);
            if let Some(frame) = frame {
                {
                    let mut o = lock(&client.outbox);
                    o.in_flight += 1;
                    if o.in_flight == 1 {
                        o.last_ack = Some(Instant::now());
                    }
                }
                if ws::write_frame(&mut stream, Opcode::Binary, &frame).is_err() {
                    client.close();
                    return;
                }
                shared.bytes_sent.fetch_add(frame.len() as u64, Ordering::Relaxed);
                shared.frames_sent.fetch_add(1, Ordering::Relaxed);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::screen::{FORMAT_PNG, MSG_FRAME};
    use crate::ws::tests::client_frame;
    use std::io::Read;

    fn start(pin: &str, view_only: bool) -> WebServer {
        let s = WebServer::start(&Config { bind: IpAddr::V4(Ipv4Addr::LOCALHOST), port: 0, pin: pin.into(), view_only });
        assert!(s.addr().is_some(), "{:?}", s.status().error);
        s
    }

    fn get(s: &WebServer, request: &str) -> String {
        let mut c = TcpStream::connect(s.addr().unwrap()).unwrap();
        c.write_all(request.as_bytes()).unwrap();
        let mut out = String::new();
        c.read_to_string(&mut out).unwrap();
        out
    }

    /// A WebSocket client for the tests.
    struct Browser {
        stream: TcpStream,
    }

    impl Browser {
        fn connect(s: &WebServer) -> Self {
            let mut stream = TcpStream::connect(s.addr().unwrap()).unwrap();
            stream.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
            let port = s.addr().unwrap().port();
            write!(
                stream,
                "GET /ws HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nOrigin: http://127.0.0.1:{port}\r\nUpgrade: websocket\r\n\
                 Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"
            )
            .unwrap();
            let mut head = Vec::new();
            let mut byte = [0u8; 1];
            while !head.ends_with(b"\r\n\r\n") {
                stream.read_exact(&mut byte).unwrap();
                head.push(byte[0]);
            }
            let head = String::from_utf8(head).unwrap();
            assert!(head.starts_with("HTTP/1.1 101"), "{head}");
            assert!(head.contains("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
            Self { stream }
        }

        /// The next server message: (opcode, payload).
        fn read(&mut self) -> (u8, Vec<u8>) {
            let mut h = [0u8; 2];
            self.stream.read_exact(&mut h).unwrap();
            assert_eq!(h[1] & 0x80, 0, "server frames are not masked");
            let len = match h[1] & 0x7F {
                126 => {
                    let mut b = [0u8; 2];
                    self.stream.read_exact(&mut b).unwrap();
                    u64::from(u16::from_be_bytes(b))
                }
                127 => {
                    let mut b = [0u8; 8];
                    self.stream.read_exact(&mut b).unwrap();
                    u64::from_be_bytes(b)
                }
                n => u64::from(n),
            };
            let mut payload = vec![0u8; len as usize];
            self.stream.read_exact(&mut payload).unwrap();
            (h[0] & 0x0F, payload)
        }

        fn text(&mut self) -> String {
            let (op, p) = self.read();
            assert_eq!(op, 1);
            String::from_utf8(p).unwrap()
        }

        fn send(&mut self, opcode: u8, payload: &[u8]) {
            self.stream.write_all(&client_frame(opcode, true, payload)).unwrap();
        }

        fn nothing_within(&mut self, ms: u64) -> bool {
            self.stream.set_read_timeout(Some(Duration::from_millis(ms))).unwrap();
            let mut b = [0u8; 1];
            let quiet = matches!(self.stream.peek(&mut b), Err(e) if e.kind() == ErrorKind::WouldBlock || e.kind() == ErrorKind::TimedOut);
            self.stream.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
            quiet
        }
    }

    fn wait_until(mut f: impl FnMut() -> bool) {
        let t = Instant::now();
        while !f() {
            assert!(t.elapsed() < Duration::from_secs(5), "timed out");
            std::thread::sleep(Duration::from_millis(5));
        }
    }

    #[test]
    fn serves_the_page_and_refuses_the_rest() {
        let s = start("", false);
        let port = s.addr().unwrap().port();
        let page = get(&s, &format!("GET / HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n\r\n"));
        assert!(page.starts_with("HTTP/1.1 200 OK"));
        assert!(page.contains("<title>Sidecar</title>"));
        assert!(get(&s, "GET /../etc/passwd HTTP/1.1\r\nHost: localhost\r\n\r\n").starts_with("HTTP/1.1 404"));
        assert!(get(&s, "GET / HTTP/1.1\r\nHost: evil.example\r\n\r\n").starts_with("HTTP/1.1 403"));
    }

    #[test]
    fn a_port_in_use_is_reported() {
        let a = start("", false);
        let b = WebServer::start(&Config {
            bind: IpAddr::V4(Ipv4Addr::LOCALHOST),
            port: a.addr().unwrap().port(),
            ..Config::default()
        });
        assert!(b.addr().is_none() && b.status().error.is_some());
    }

    #[test]
    fn frames_flow_with_acknowledgements() {
        let s = start("", false);
        let generation = s.reset_screen(128, 64, 64);
        let mut b = Browser::connect(&s);
        assert_eq!(b.text(), "{\"t\":\"hello\",\"pin\":false,\"viewOnly\":false}");
        wait_until(|| s.watched());
        // The size comes at once, the tiles as they are put.
        let (op, m) = b.read();
        assert_eq!((op, m[0]), (2, MSG_FRAME));
        assert!(s.put_tile(generation, 1, FORMAT_PNG, b"tile"));
        s.publish();
        let (_, m) = b.read();
        assert_eq!(&m[m.len() - 4..], b"tile");
        // Two frames unacknowledged: the third waits for an ack.
        s.put_tile(generation, 0, FORMAT_PNG, b"a");
        s.publish();
        assert!(b.nothing_within(200));
        b.send(2, &[input::MSG_ACK, 1, 0, 0, 0]);
        let (_, m) = b.read();
        assert_eq!(&m[m.len() - 1..], b"a");

        // Ping, input.
        b.send(2, &[input::MSG_PING, 1, 2, 3, 4, 5, 6, 7, 8]);
        assert_eq!(b.read().1, vec![MSG_PONG, 1, 2, 3, 4, 5, 6, 7, 8]);
        let mut down = vec![input::MSG_POINTER, 1, 1, 0];
        down.extend_from_slice(&0.5f32.to_le_bytes());
        down.extend_from_slice(&0.25f32.to_le_bytes());
        b.send(2, &down);
        let mut got = None;
        wait_until(|| {
            got = s.take_input();
            got.is_some()
        });
        let e = got.unwrap();
        assert_eq!((e.kind, e.x, e.y, e.buttons), (input::KIND_DOWN, 0.5, 0.25, 1));

        assert!(s.status().bytes_sent > 0 && s.status().frames_sent == 3);
        b.send(8, &[]);
        wait_until(|| s.status().clients.is_empty());
    }

    #[test]
    fn nothing_before_the_pin() {
        let s = start("2468", false);
        let generation = s.reset_screen(64, 64, 64);
        s.put_tile(generation, 0, FORMAT_PNG, b"secret");
        let mut b = Browser::connect(&s);
        assert_eq!(b.text(), "{\"t\":\"hello\",\"pin\":true,\"viewOnly\":false}");
        // No frame and no input before the PIN.
        b.send(2, &[input::MSG_KEY, 1, 0, 65, 0, 0, 0]);
        assert!(b.nothing_within(200));
        assert!(!s.watched() && s.take_input().is_none());
        b.send(1, b"pin:1357");
        assert_eq!(b.text(), "{\"t\":\"wrong\"}");
        b.send(1, b"pin:2468");
        assert_eq!(b.text(), "{\"t\":\"ok\"}");
        let (_, m) = b.read();
        assert_eq!(&m[m.len() - 6..], b"secret");
        assert!(s.watched());
    }

    #[test]
    fn a_screen_reset_while_watched_is_sent() {
        let s = start("", false);
        let mut b = Browser::connect(&s);
        b.text();
        wait_until(|| s.watched());
        let generation = s.reset_screen(100, 70, 64);
        let (_, m) = b.read();
        assert_eq!(u16::from_le_bytes([m[5], m[6]]), 100);
        s.put_tile(generation, 3, FORMAT_PNG, b"TILE");
        s.publish();
        let (_, m) = b.read();
        assert_eq!(&m[m.len() - 4..], b"TILE");
    }

    #[test]
    fn view_only_ignores_input() {
        let s = start("", true);
        let mut b = Browser::connect(&s);
        assert!(b.text().contains("\"viewOnly\":true"));
        b.send(2, &[input::MSG_KEY, 1, 0, 65, 0, 0, 0]);
        b.send(2, &[input::MSG_PING, 0, 0, 0, 0, 0, 0, 0, 9]);
        loop {
            let (op, m) = b.read();
            if op == 2 && m[0] == MSG_PONG {
                break;
            }
        }
        assert!(s.take_input().is_none());
    }

    #[test]
    fn stop_closes_the_browsers() {
        let mut s = start("", false);
        let mut b = Browser::connect(&s);
        b.text();
        s.stop();
        let mut rest = Vec::new();
        let _ = b.stream.read_to_end(&mut rest);
        assert!(s.status().clients.is_empty());
    }
}
