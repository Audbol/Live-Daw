//! Soak test: the remote servers under the load real clients put on them,
//! with every gap, timeout and missed reply logged.
//!
//! * An OSC client like Mixing Station: `/?`, a burst of parameter gets,
//!   the `/*S` subscription renewed every 8 s, faders moved at 50 Hz, a poll
//!   every 100 ms and `/?` every 2 s.
//! * A Native client like WING Edit: the whole tree walked by definition
//!   requests, all values read (`da dc`), then a meter subscription renewed
//!   every 3 s (the protocol document's bare `df d3 d4 <id>`), a keep-alive
//!   every 7 s and a fader set at 10 Hz.
//! * A second OSC remote doing the same (two tablets).
//! * A Native client that only shows meters: the document's subscription and
//!   renewal, libwing's bare keep-alive, nothing else.
//! * An OSC client setting random values on every parameter of a few strips
//!   and FX slots (EQ, dynamics, models, sends...), 200 a second.
//! * A Native client on a slow link (reads 128 KB/s while syncing) and one
//!   that stops reading altogether (a stalled tablet on Wi-Fi).
//!
//! Each client checks what it gets: replies within their timeout, the other
//! clients' changes arriving as events, meters arriving without gaps. The
//! audio callback runs alongside; nothing may allocate on it.
//!
//! `soak_short` runs in CI (a little over the 10 s subscription life). The
//! long run measures the audio callback without clients and with them, for
//! as long each:
//!
//!   SOAK_SECS=300 cargo test --release -p livedaw-wing --test soak -- --ignored --nocapture
//!
//! SOAK_RT=1 gives the audio threads realtime priority, as the app does
//! (Linux, when allowed), SOAK_WORKERS the number of audio workers (2),
//! SOAK_NO_STALL=1 leaves out the client that stops reading, and
//! SOAK_ALLOC_TRACES=1 prints where audio-thread allocations come from.

use livedaw_engine::Engine;
use livedaw_wing::osc::{self, Arg, Message};
use livedaw_wing::server::{Config, EngineHandle, WingServer};
use livedaw_wing::{schema, Identity, WingState};
use std::collections::VecDeque;
use std::io::{Read, Write};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, TcpStream, UdpSocket};
use std::sync::atomic::{AtomicBool, Ordering::Relaxed};
use std::sync::mpsc::{channel, Receiver, RecvTimeoutError};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

/// Timeouts and gap limits are this many times longer in debug builds (and
/// under the sanitizers). The faults this test is for show as gaps of a
/// second or more either way.
const SLACK: u64 = if cfg!(debug_assertions) { 2 } else { 1 };

/// Counts allocations made on the audio threads (the callback and its workers).
struct Counting;

static AUDIO_ALLOCS: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

thread_local! {
    static IN_AUDIO: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

fn audio_thread(on: bool) {
    IN_AUDIO.with(|c| c.set(on));
}

/// Where the first few came from (set SOAK_ALLOC_TRACES=1).
static AUDIO_ALLOC_TRACES: Mutex<Vec<String>> = Mutex::new(Vec::new());
static TRACE_ALLOCS: AtomicBool = AtomicBool::new(false);

fn note_alloc() {
    if IN_AUDIO.try_with(|c| c.get()).unwrap_or(false) {
        let n = AUDIO_ALLOCS.fetch_add(1, Relaxed);
        if n < 4 && TRACE_ALLOCS.load(Relaxed) {
            audio_thread(false);
            let trace = std::backtrace::Backtrace::force_capture().to_string();
            AUDIO_ALLOC_TRACES.lock().unwrap_or_else(|e| e.into_inner()).push(trace);
            audio_thread(true);
        }
    }
}

unsafe impl std::alloc::GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: std::alloc::Layout) -> *mut u8 {
        note_alloc();
        unsafe { std::alloc::System.alloc(l) }
    }
    unsafe fn dealloc(&self, p: *mut u8, l: std::alloc::Layout) {
        note_alloc();
        unsafe { std::alloc::System.dealloc(p, l) }
    }
    unsafe fn alloc_zeroed(&self, l: std::alloc::Layout) -> *mut u8 {
        note_alloc();
        unsafe { std::alloc::System.alloc_zeroed(l) }
    }
    unsafe fn realloc(&self, p: *mut u8, l: std::alloc::Layout, n: usize) -> *mut u8 {
        note_alloc();
        unsafe { std::alloc::System.realloc(p, l, n) }
    }
}

#[global_allocator]
static ALLOC: Counting = Counting;

fn id(path: &str) -> u32 {
    let s = schema();
    s.id(s.resolve(path, &livedaw_wing::schema::NoModels).unwrap_or_else(|| panic!("{path}")))
}

/// What a client saw; problems are listed with the time they happened.
#[derive(Default)]
struct Report {
    name: String,
    problems: Vec<String>,
    notes: Vec<String>,
}

impl Report {
    fn new(name: &str) -> Self {
        Self { name: name.into(), ..Self::default() }
    }
    fn problem(&mut self, t0: Instant, what: String) {
        self.problems.push(format!("{:7.3}s {what}", t0.elapsed().as_secs_f64()));
    }
}

/// Watches a stream of arrivals for gaps longer than `limit`.
struct GapWatch {
    limit: Duration,
    last: Option<Instant>,
    count: u64,
    worst: Duration,
    gaps: Vec<(Instant, Duration)>,
}

impl GapWatch {
    fn new(limit: Duration) -> Self {
        Self { limit, last: None, count: 0, worst: Duration::ZERO, gaps: Vec::new() }
    }
    fn arrive(&mut self, at: Instant) {
        if let Some(last) = self.last {
            let gap = at.saturating_duration_since(last);
            self.worst = self.worst.max(gap);
            if gap > self.limit {
                self.gaps.push((last, gap));
            }
        }
        self.last = Some(at);
        self.count += 1;
    }
    /// Arrivals stopped before `end`.
    fn finish(&mut self, end: Instant) {
        if let Some(last) = self.last {
            let gap = end.saturating_duration_since(last);
            if gap > self.limit {
                self.worst = self.worst.max(gap);
                self.gaps.push((last, gap));
            }
        }
    }
    fn report(&self, r: &mut Report, t0: Instant, what: &str) {
        r.notes.push(format!("{what}: {} received, worst gap {} ms", self.count, self.worst.as_millis()));
        for (at, gap) in &self.gaps {
            r.problems.push(format!(
                "{:7.3}s {what}: nothing for {} ms",
                at.saturating_duration_since(t0).as_secs_f64(),
                gap.as_millis()
            ));
        }
    }
}

/// Latencies of request/reply pairs.
#[derive(Default)]
struct Latency {
    all: Vec<Duration>,
}

impl Latency {
    fn summary(&mut self, what: &str) -> String {
        if self.all.is_empty() {
            return format!("{what}: none");
        }
        self.all.sort();
        let p = |q: f64| self.all[((self.all.len() - 1) as f64 * q) as usize].as_secs_f64() * 1000.0;
        format!("{what}: {} replies, median {:.2} ms, p99 {:.2} ms, worst {:.2} ms", self.all.len(), p(0.5), p(0.99), p(1.0))
    }
}

// ============================================================================
// Mixing Station (OSC)

const OSC_REPLY_TIMEOUT: Duration = Duration::from_millis(1000 * SLACK);

fn osc_client(name: &str, to: SocketAddr, secs: f64, stop: Arc<AtomicBool>, t0: Instant) -> Report {
    let mut r = Report::new(name);
    let sock = UdpSocket::bind("127.0.0.1:0").unwrap();
    let rx_sock = sock.try_clone().unwrap();
    rx_sock.set_read_timeout(Some(Duration::from_millis(50))).unwrap();
    let (tx, rx) = channel::<(Instant, Message)>();
    let rx_stop = stop.clone();
    let reader = std::thread::spawn(move || {
        let mut buf = vec![0u8; 65536];
        while !rx_stop.load(Relaxed) {
            if let Ok((n, _)) = rx_sock.recv_from(&mut buf) {
                if let Some(m) = osc::decode(&buf[..n]) {
                    let _ = tx.send((Instant::now(), m));
                }
            }
        }
    });
    let send = |m: Message| {
        let _ = sock.send_to(&osc::encode(&m), to);
    };

    // Waits for the reply to `addr` (a get: 3 arguments, or `/?`), passing events to `events`.
    let mut events = GapWatch::new(Duration::from_millis(1000 * SLACK));
    let mut event_values = 0u64;
    let mut on_event = |at: Instant, m: &Message, events: &mut GapWatch| {
        if m.addr == "/ch/20/fdr" && m.args.len() == 1 {
            events.arrive(at);
            event_values += 1;
        }
    };
    let mut wait_reply = |addrs: &mut VecDeque<(String, Instant)>,
                          rx: &Receiver<(Instant, Message)>,
                          events: &mut GapWatch,
                          r: &mut Report,
                          lat: &mut Latency,
                          until_empty: bool| {
        // Receives until the oldest pending request is answered (or every one, `until_empty`).
        while let Some((want, sent)) = addrs.front().cloned() {
            let left = OSC_REPLY_TIMEOUT.saturating_sub(sent.elapsed());
            match rx.recv_timeout(left) {
                Ok((at, m)) => {
                    // Replies to gets are matched by address; the rest are events.
                    if let Some(pos) = addrs.iter().position(|(a, _)| *a == m.addr) {
                        let (_, s) = addrs.remove(pos).unwrap();
                        lat.all.push(at.saturating_duration_since(s));
                    } else {
                        on_event(at, &m, events);
                        continue;
                    }
                }
                Err(RecvTimeoutError::Timeout) => {
                    r.problem(t0, format!("no reply to {want} within {} ms", OSC_REPLY_TIMEOUT.as_millis()));
                    addrs.pop_front();
                }
                Err(RecvTimeoutError::Disconnected) => return,
            }
            if !until_empty {
                return;
            }
        }
    };

    let mut pending: VecDeque<(String, Instant)> = VecDeque::new();
    let mut lat_sync = Latency::default();
    let mut lat_poll = Latency::default();

    // Connect: who's there, then read the strips (16 requests in flight).
    send(Message::new("/?", vec![]));
    pending.push_back(("/?".into(), Instant::now()));
    wait_reply(&mut pending, &rx, &mut events, &mut r, &mut lat_sync, true);
    let started = Instant::now();
    let mut paths = Vec::new();
    for ch in 1..=40 {
        for p in ["fdr", "mute", "pan", "name", "col", "in/set/trim", "flt/lc", "eq/on", "dyn/on", "gate/on"] {
            paths.push(format!("/ch/{ch}/{p}"));
        }
        for b in 1..=16 {
            paths.push(format!("/ch/{ch}/send/{b}/lvl"));
        }
    }
    for b in 1..=16 {
        for p in ["fdr", "mute", "pan", "name"] {
            paths.push(format!("/bus/{b}/{p}"));
        }
    }
    for path in paths {
        if pending.len() >= 16 {
            wait_reply(&mut pending, &rx, &mut events, &mut r, &mut lat_sync, false);
        }
        send(Message::new(path.clone(), vec![]));
        pending.push_back((path, Instant::now()));
    }
    wait_reply(&mut pending, &rx, &mut events, &mut r, &mut lat_sync, true);
    r.notes.push(format!("initial sync {:.2} s", started.elapsed().as_secs_f64()));

    // Steady state.
    send(Message::new("/*S", vec![]));
    let mut last_sub = Instant::now();
    let mut last_poll = Instant::now();
    let mut last_info = Instant::now();
    let end = t0 + Duration::from_secs_f64(secs);
    let mut tick = 0u64;
    let mut poll_pending: VecDeque<(String, Instant)> = VecDeque::new();
    let events_from = Instant::now() + Duration::from_millis(500);
    while Instant::now() < end && !stop.load(Relaxed) {
        tick += 1;
        // Faders 1-8 at 50 Hz.
        for ch in 1..=8 {
            let v = -40.0 + 30.0 * (((tick + ch) % 100) as f32 / 100.0);
            send(Message::new(format!("/ch/{ch}/fdr"), vec![Arg::Float(v)]));
        }
        if last_sub.elapsed() >= Duration::from_secs(8) {
            send(Message::new("/*S", vec![]));
            last_sub = Instant::now();
        }
        if last_poll.elapsed() >= Duration::from_millis(100) {
            let path = format!("/ch/{}/mute", 9 + tick % 30);
            send(Message::new(path.clone(), vec![]));
            poll_pending.push_back((path, Instant::now()));
            last_poll = Instant::now();
        }
        if last_info.elapsed() >= Duration::from_secs(2) {
            send(Message::new("/?", vec![]));
            poll_pending.push_back(("/?".into(), Instant::now()));
            last_info = Instant::now();
        }
        // Collect what arrived for 20 ms.
        let until = Instant::now() + Duration::from_millis(20);
        loop {
            let left = until.saturating_duration_since(Instant::now());
            if left.is_zero() {
                break;
            }
            match rx.recv_timeout(left) {
                Ok((at, m)) => {
                    if m.args.len() != 1 || m.addr == "/?" {
                        if let Some(pos) = poll_pending.iter().position(|(a, _)| *a == m.addr) {
                            let (_, s) = poll_pending.remove(pos).unwrap();
                            lat_poll.all.push(at.saturating_duration_since(s));
                        }
                    } else if at >= events_from {
                        on_event(at, &m, &mut events);
                    }
                }
                Err(_) => break,
            }
        }
        while let Some((a, s)) = poll_pending.front() {
            if s.elapsed() > OSC_REPLY_TIMEOUT {
                r.problem(t0, format!("no reply to poll {a} within {} ms", OSC_REPLY_TIMEOUT.as_millis()));
                poll_pending.pop_front();
            } else {
                break;
            }
        }
    }
    events.finish(Instant::now());
    events.report(&mut r, t0, "events for /ch/20/fdr (Native client's 10 Hz moves)");
    r.notes.push(lat_sync.summary("initial gets"));
    r.notes.push(lat_poll.summary("polls"));
    r.notes.push(format!("{event_values} events"));
    stop.store(true, Relaxed);
    let _ = reader.join();
    r
}

/// A remote working through the console: random values for every parameter
/// of a few channels, buses, mains and FX slots (EQ, dynamics, models, sends,
/// FX models...), 200 a second, as Native tokens in OSC blobs (`d6`: a
/// position 0..1, which fits every type).
/// The sweeper's sets, to tell which ones slow the audio callback down.
static SWEEP_LOG: Mutex<Vec<(Instant, String, f32)>> = Mutex::new(Vec::new());

fn sweeper(to: SocketAddr, secs: f64, stop: Arc<AtomicBool>, t0: Instant) -> Report {
    let mut r = Report::new("OSC (parameter sweep)");
    let sock = UdpSocket::bind("127.0.0.1:0").unwrap();
    sock.set_read_timeout(Some(Duration::from_millis(500))).unwrap();
    let mut buf = vec![0u8; 65536];
    let mut ask = |path: &str| -> Option<String> {
        sock.send_to(&osc::encode(&Message::new(path, vec![Arg::Str("?".into())])), to).ok()?;
        loop {
            let (n, _) = sock.recv_from(&mut buf).ok()?;
            match osc::decode(&buf[..n]) {
                Some(Message { addr, args }) if addr == path => {
                    return args.into_iter().find_map(|a| if let Arg::Str(s) = a { Some(s) } else { None });
                }
                _ => {}
            }
        }
    };
    let mut leaves = Vec::new();
    let mut nodes: Vec<String> =
        ["/ch/1", "/ch/2", "/ch/3", "/aux/1", "/bus/1", "/bus/2", "/main/1", "/mtx/1", "/fx/1", "/fx/2", "/fx/9"]
            .iter()
            .map(|s| s.to_string())
            .collect();
    while let Some(node) = nodes.pop() {
        let Some(text) = ask(&node) else {
            r.problem(t0, format!("no description of {node}"));
            continue;
        };
        for line in text.lines() {
            let mut f = line.split_whitespace();
            let Some(name) = f.next() else { continue };
            if name.starts_with('$') {
                continue;
            }
            let path = format!("{node}/{name}");
            if line.contains("(node)") {
                nodes.push(path);
            } else if !line.contains(" string [") {
                leaves.push(path);
            }
        }
    }
    r.notes.push(format!("{} parameters swept", leaves.len()));
    let sock = UdpSocket::bind("127.0.0.1:0").unwrap();
    let end = t0 + Duration::from_secs_f64(secs);
    let mut seed = 0x2545_f491_4f6c_dd1du64;
    let mut sent = 0u64;
    while Instant::now() < end && !stop.load(Relaxed) && !leaves.is_empty() {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        let path = &leaves[(seed % leaves.len() as u64) as usize];
        let pos = ((seed >> 20) % 10_001) as f32 / 10_000.0;
        let mut tok = vec![0xd6];
        tok.extend_from_slice(&pos.to_be_bytes());
        let _ = sock.send_to(&osc::encode(&Message::new(path.clone(), vec![Arg::Blob(tok)])), to);
        SWEEP_LOG.lock().unwrap().push((Instant::now(), path.clone(), pos));
        sent += 1;
        std::thread::sleep(Duration::from_millis(5));
    }
    r.notes.push(format!("{sent} random sets"));
    r
}

// ============================================================================
// WING Edit (Native)

#[derive(Debug)]
enum Tok {
    Data(u32),
    End,
    /// (id, is a node)
    Def(u32, bool),
}

/// Incremental decoder of the server's channel-2 token stream.
#[derive(Default)]
struct Decoder {
    esc: bool,
    ch: u8,
    data: Vec<u8>,
    cur: u32,
}

impl Decoder {
    fn feed(&mut self, wire: &[u8], out: &mut Vec<Tok>) {
        for &b in wire {
            if self.esc {
                self.esc = false;
                match b {
                    0xde | 0xdf => {
                        if self.ch == 1 {
                            self.data.push(0xdf)
                        }
                    }
                    0xd0..=0xdd => self.ch = b - 0xd0,
                    other => {
                        if self.ch == 1 {
                            self.data.extend_from_slice(&[0xdf, other])
                        }
                    }
                }
            } else if b == 0xdf {
                self.esc = true;
            } else if self.ch == 1 {
                self.data.push(b);
            }
        }
        let d = &self.data;
        let mut i = 0;
        let be32 = |i: usize| u32::from_be_bytes([d[i], d[i + 1], d[i + 2], d[i + 3]]);
        while let Some(&op) = d.get(i) {
            let need = match op {
                0x80..=0xbf => 1 + (op - 0x7f) as usize,
                0xd1 => match d.get(i + 1) {
                    Some(&n) => 2 + n as usize + 1,
                    None => break,
                },
                0xd3 => 3,
                0xd4..=0xd7 => 5,
                0xdf => {
                    if d.len() < i + 3 {
                        break;
                    }
                    let n = u16::from_be_bytes([d[i + 1], d[i + 2]]) as usize;
                    if n == 0 {
                        if d.len() < i + 7 {
                            break;
                        }
                        7 + be32(i + 3) as usize
                    } else {
                        3 + n
                    }
                }
                _ => 1,
            };
            if d.len() < i + need {
                break;
            }
            match op {
                0x00..=0x3f | 0x80..=0xbf | 0xd0 | 0xd1 | 0xd3..=0xd6 => out.push(Tok::Data(self.cur)),
                0xd7 => self.cur = be32(i + 1),
                0xde => out.push(Tok::End),
                0xdf => {
                    let body = if u16::from_be_bytes([d[i + 1], d[i + 2]]) == 0 { i + 7 } else { i + 3 };
                    let id = be32(body + 4);
                    let mut j = body + 10;
                    j += 1 + d[j] as usize; // name
                    j += 1 + d[j] as usize; // long name
                    let flags = u16::from_be_bytes([d[j], d[j + 1]]);
                    out.push(Tok::Def(id, (flags >> 4) & 0x0f == 0));
                }
                _ => {}
            }
            i += need;
        }
        self.data.drain(..i);
    }
}

fn escaped(payload: &[u8]) -> Vec<u8> {
    let mut v = Vec::with_capacity(payload.len() + 4);
    for &b in payload {
        v.push(b);
        if b == 0xdf {
            v.push(0xde);
        }
    }
    v
}

const NATIVE_REPLY_TIMEOUT: Duration = Duration::from_secs(2 * SLACK);

fn native_client(
    to: SocketAddr,
    secs: f64,
    stop: Arc<AtomicBool>,
    t0: Instant,
    moves_from: Arc<Mutex<Option<Instant>>>,
) -> Report {
    let mut r = Report::new("Native (WING Edit)");
    let tcp = TcpStream::connect(to).unwrap();
    tcp.set_nodelay(true).unwrap();
    let mut rd = tcp.try_clone().unwrap();
    rd.set_read_timeout(Some(Duration::from_millis(50))).unwrap();
    let mut w = tcp;
    let (tx, rx) = channel::<(Instant, Tok)>();
    let closed = Arc::new(AtomicBool::new(false));
    let rx_stop = stop.clone();
    let rx_closed = closed.clone();
    let reader = std::thread::spawn(move || {
        let mut dec = Decoder::default();
        let mut buf = vec![0u8; 65536];
        let mut toks = Vec::new();
        while !rx_stop.load(Relaxed) {
            match rd.read(&mut buf) {
                Ok(0) => {
                    rx_closed.store(true, Relaxed);
                    break;
                }
                Ok(n) => {
                    let now = Instant::now();
                    dec.feed(&buf[..n], &mut toks);
                    for t in toks.drain(..) {
                        let _ = tx.send((now, t));
                    }
                }
                Err(e) if matches!(e.kind(), std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut) => {}
                Err(_) => {
                    rx_closed.store(true, Relaxed);
                    break;
                }
            }
        }
    });

    let fdr1 = id("/ch/1/fdr");
    let mut notes = GapWatch::new(Duration::from_millis(1000 * SLACK));
    let mut pending: VecDeque<Instant> = VecDeque::new();
    let mut lat_walk = Latency::default();
    let mut lat_poll = Latency::default();
    let mut defs: Vec<(u32, bool)> = Vec::new();
    let watch_from: Arc<Mutex<Option<Instant>>> = moves_from;
    // Handles one token; returns true for a request's end.
    let take = |at: Instant, t: Tok, defs: &mut Vec<(u32, bool)>, notes: &mut GapWatch| -> bool {
        match t {
            Tok::End => return true,
            Tok::Def(id, node) => defs.push((id, node)),
            Tok::Data(id) => {
                if id == fdr1 && watch_from.lock().unwrap().is_some_and(|f| at >= f) {
                    notes.arrive(at);
                }
            }
        }
        false
    };
    let drain = |pending: &mut VecDeque<Instant>,
                 lat: &mut Latency,
                 r: &mut Report,
                 defs: &mut Vec<(u32, bool)>,
                 notes: &mut GapWatch,
                 keep: usize,
                 timeout: Duration| {
        while pending.len() > keep {
            let sent = pending[0];
            let left = timeout.saturating_sub(sent.elapsed());
            match rx.recv_timeout(left) {
                Ok((at, t)) => {
                    if take(at, t, defs, notes) {
                        pending.pop_front();
                        lat.all.push(at.saturating_duration_since(sent));
                    }
                }
                Err(RecvTimeoutError::Timeout) => {
                    r.problem(t0, format!("no end of a request within {} ms", timeout.as_millis()));
                    pending.clear();
                }
                Err(RecvTimeoutError::Disconnected) => {
                    pending.clear();
                }
            }
        }
    };

    // Tree walk: definitions of every node's children (8 requests in flight).
    let started = Instant::now();
    let _ = w.write_all(&[0xdf, 0xd1, 0xda, 0xdd]);
    pending.push_back(Instant::now());
    drain(&mut pending, &mut lat_walk, &mut r, &mut defs, &mut notes, 0, NATIVE_REPLY_TIMEOUT);
    let mut queue: VecDeque<u32> = defs.drain(..).filter(|(_, n)| *n).map(|(i, _)| i).collect();
    let mut nodes = 0usize;
    let mut leaves = 0usize;
    while let Some(node) = queue.pop_front() {
        let mut req = vec![0xd7];
        req.extend_from_slice(&node.to_be_bytes());
        req.push(0xdd);
        let _ = w.write_all(&escaped(&req));
        pending.push_back(Instant::now());
        nodes += 1;
        drain(&mut pending, &mut lat_walk, &mut r, &mut defs, &mut notes, 7, NATIVE_REPLY_TIMEOUT);
        for (i, n) in defs.drain(..) {
            if n {
                queue.push_back(i);
            } else {
                leaves += 1;
            }
        }
        if queue.is_empty() {
            drain(&mut pending, &mut lat_walk, &mut r, &mut defs, &mut notes, 0, NATIVE_REPLY_TIMEOUT);
            for (i, n) in defs.drain(..) {
                if n {
                    queue.push_back(i);
                } else {
                    leaves += 1;
                }
            }
        }
        if stop.load(Relaxed) || closed.load(Relaxed) {
            break;
        }
    }
    r.notes.push(format!("tree walk: {nodes} nodes, {leaves} parameters in {:.2} s", started.elapsed().as_secs_f64()));
    // All values.
    let s2 = Instant::now();
    let _ = w.write_all(&[0xda, 0xdc]);
    pending.push_back(Instant::now());
    let mut dump = Latency::default();
    drain(&mut pending, &mut dump, &mut r, &mut defs, &mut notes, 0, Duration::from_secs(5));
    r.notes.push(format!("all values (da dc) in {:.0} ms", s2.elapsed().as_secs_f64() * 1000.0));

    // Meters: port, then the subscription, as the protocol document sends them.
    let udp = UdpSocket::bind("127.0.0.1:0").unwrap();
    udp.set_read_timeout(Some(Duration::from_millis(50))).unwrap();
    let port = udp.local_addr().unwrap().port();
    let meters = meter_receiver(udp, stop.clone(), t0, 0x11, 60, "Native meters");
    let mut req = vec![0xdf, 0xd3, 0xd3];
    req.extend(escaped(&port.to_be_bytes()));
    req.extend_from_slice(&[0xdf, 0xd3, 0xd4]);
    req.extend(escaped(&0x11u32.to_be_bytes()));
    req.push(0xdc);
    req.push(0xa0);
    req.extend(0u8..40);
    req.push(0xa2);
    req.extend(0u8..16);
    req.push(0xa3);
    req.extend(0u8..4);
    req.push(0xde);
    let _ = w.write_all(&req);

    // Steady state.
    let end = t0 + Duration::from_secs_f64(secs);
    let mut last_renew = Instant::now();
    let mut last_keep = Instant::now();
    let mut last_set = Instant::now();
    let mut v = 0u32;
    let fdr20 = id("/ch/20/fdr");
    let mute = id("/ch/30/mute");
    *watch_from.lock().unwrap() = Some(Instant::now() + Duration::from_millis(500));
    while Instant::now() < end && !stop.load(Relaxed) {
        if closed.load(Relaxed) {
            r.problem(t0, "the server closed the connection".into());
            break;
        }
        if last_renew.elapsed() >= Duration::from_secs(3) {
            // The document's renewal: nothing follows it.
            let mut renew = vec![0xdf, 0xd3, 0xd4];
            renew.extend(escaped(&0x11u32.to_be_bytes()));
            let _ = w.write_all(&renew);
            last_renew = Instant::now();
        }
        if last_set.elapsed() >= Duration::from_millis(100) {
            v += 1;
            let mut req = vec![0xdf, 0xd1, 0xd7];
            req.extend_from_slice(&fdr20.to_be_bytes());
            req.push(0xd5);
            req.extend_from_slice(&(-50.0 + (v % 40) as f32).to_be_bytes());
            let _ = w.write_all(&escaped_tail(&req, 2));
            last_set = Instant::now();
        }
        if last_keep.elapsed() >= Duration::from_secs(7) {
            // Keep-alive: a small get.
            let mut req = vec![0xdf, 0xd1, 0xd7];
            req.extend_from_slice(&mute.to_be_bytes());
            req.push(0xdc);
            let _ = w.write_all(&escaped_tail(&req, 2));
            pending.push_back(Instant::now());
            last_keep = Instant::now();
        }
        // Read for 20 ms.
        let until = Instant::now() + Duration::from_millis(20);
        loop {
            let left = until.saturating_duration_since(Instant::now());
            if left.is_zero() {
                break;
            }
            match rx.recv_timeout(left) {
                Ok((at, t)) => {
                    if take(at, t, &mut defs, &mut notes) {
                        if let Some(s) = pending.pop_front() {
                            lat_poll.all.push(at.saturating_duration_since(s));
                        }
                    }
                }
                Err(_) => break,
            }
        }
        if pending.front().is_some_and(|s| s.elapsed() > NATIVE_REPLY_TIMEOUT) {
            r.problem(t0, "no reply to a keep-alive get".into());
            pending.pop_front();
        }
    }
    notes.finish(Instant::now());
    notes.report(&mut r, t0, "notifications for /ch/1/fdr (OSC client's 50 Hz moves)");
    r.notes.push(lat_walk.summary("tree walk requests"));
    r.notes.push(lat_poll.summary("keep-alive gets"));
    stop.store(true, Relaxed);
    let _ = reader.join();
    let m = meters.join().unwrap();
    r.problems.extend(m.problems.into_iter().map(|p| format!("{p} (meters)")));
    r.notes.extend(m.notes);
    r
}

/// Receives meter datagrams of `strips` strips (8 words each) for `report_id`.
fn meter_receiver(
    udp: UdpSocket,
    stop: Arc<AtomicBool>,
    t0: Instant,
    report_id: u32,
    strips: usize,
    name: &'static str,
) -> JoinHandle<Report> {
    std::thread::spawn(move || {
        let mut gw = GapWatch::new(Duration::from_millis(250 * SLACK));
        let mut buf = [0u8; 4096];
        let mut sources = std::collections::HashSet::new();
        let mut bad = 0u64;
        while !stop.load(Relaxed) {
            if let Ok((n, from)) = udp.recv_from(&mut buf) {
                sources.insert(from);
                if n != 4 + 2 * 8 * strips || buf[..4] != report_id.to_be_bytes() {
                    bad += 1;
                }
                gw.arrive(Instant::now());
            }
        }
        gw.finish(Instant::now());
        let mut r = Report::new(name);
        gw.report(&mut r, t0, "meter datagrams (every 50 ms)");
        if bad > 0 {
            r.problem(t0, format!("{bad} meter datagrams of the wrong size or id"));
        }
        r.notes.push(format!("meters came from {sources:?}"));
        r
    })
}

/// A client that only shows meters (a meter bridge, or a remote's meter view
/// left open): the protocol document's subscription and renewal every 3 s,
/// libwing's bare keep-alive (`df d1`) every 7 s, nothing else.
fn meter_client(to: SocketAddr, secs: f64, stop: Arc<AtomicBool>, t0: Instant) -> Report {
    let mut r = Report::new("Native (meters only)");
    let mut tcp = TcpStream::connect(to).unwrap();
    tcp.set_nodelay(true).unwrap();
    let udp = UdpSocket::bind("127.0.0.1:0").unwrap();
    udp.set_read_timeout(Some(Duration::from_millis(50))).unwrap();
    let port = udp.local_addr().unwrap().port();
    let meters = meter_receiver(udp, stop.clone(), t0, 0x22, 8, "Native meters (meters only)");
    let mut req = vec![0xdf, 0xd3, 0xd3];
    req.extend(escaped(&port.to_be_bytes()));
    req.extend_from_slice(&[0xdf, 0xd3, 0xd4]);
    req.extend(escaped(&0x22u32.to_be_bytes()));
    req.push(0xdc);
    req.push(0xa0);
    req.extend(0u8..8);
    req.push(0xde);
    let _ = tcp.write_all(&req);
    let end = t0 + Duration::from_secs_f64(secs);
    let (mut last_renew, mut last_keep) = (Instant::now(), Instant::now());
    let mut sink = [0u8; 4096];
    tcp.set_read_timeout(Some(Duration::from_millis(20))).unwrap();
    while Instant::now() < end && !stop.load(Relaxed) {
        if last_renew.elapsed() >= Duration::from_secs(3) {
            let mut renew = vec![0xdf, 0xd3, 0xd4];
            renew.extend(escaped(&0x22u32.to_be_bytes()));
            if tcp.write_all(&renew).is_err() {
                r.problem(t0, "the connection was closed".into());
                break;
            }
            last_renew = Instant::now();
        }
        if last_keep.elapsed() >= Duration::from_secs(7) {
            let _ = tcp.write_all(&[0xdf, 0xd1]);
            last_keep = Instant::now();
        }
        if let Ok(0) = tcp.read(&mut sink) {
            r.problem(t0, "the server closed the connection".into());
            break;
        }
    }
    stop.store(true, Relaxed);
    let m = meters.join().unwrap();
    r.problems.extend(m.problems);
    r.notes.extend(m.notes);
    r
}

/// Escapes all but the first `skip` bytes (a channel selection).
fn escaped_tail(v: &[u8], skip: usize) -> Vec<u8> {
    let mut out = v[..skip].to_vec();
    out.extend(escaped(&v[skip..]));
    out
}

/// A client on a slow link (a tablet on busy Wi-Fi) syncing: it asks for all
/// values four times (about 1 MB) and reads 128 KB a second. It must get it
/// all without being dropped, and without holding up anyone else.
fn slow_client(to: SocketAddr, secs: f64, t0: Instant) -> Report {
    let mut r = Report::new("Native (slow link)");
    let mut tcp = TcpStream::connect(to).unwrap();
    tcp.set_read_timeout(Some(Duration::from_millis(100))).unwrap();
    for _ in 0..4 {
        let _ = tcp.write_all(&[0xdf, 0xd1, 0xda, 0xdc]);
    }
    let end = t0 + Duration::from_secs_f64(secs);
    let mut dec = Decoder::default();
    let mut toks = Vec::new();
    let mut ends = 0;
    let mut bytes = 0usize;
    let mut buf = vec![0u8; 16384];
    let mut last_keep = Instant::now();
    while Instant::now() < end && ends < 4 {
        match tcp.read(&mut buf[..12800]) {
            Ok(0) => {
                r.problem(t0, format!("the server closed the connection after {bytes} bytes"));
                return r;
            }
            Ok(n) => {
                bytes += n;
                dec.feed(&buf[..n], &mut toks);
                ends += toks.drain(..).filter(|t| matches!(t, Tok::End)).count();
            }
            Err(e) if matches!(e.kind(), std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut) => {}
            Err(e) => {
                r.problem(t0, format!("{e} after {bytes} bytes"));
                return r;
            }
        }
        if last_keep.elapsed() > Duration::from_secs(5) {
            let _ = tcp.write_all(&[0xdf, 0xd1]);
            last_keep = Instant::now();
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    if ends < 4 {
        r.problem(t0, format!("only {ends} of 4 replies ({bytes} bytes) in {secs} s"));
    }
    r.notes.push(format!("{bytes} bytes at 128 KB/s, {ends} of 4 replies in {:.1} s", t0.elapsed().as_secs_f64()));
    r
}

/// A client that asks for everything and never reads.
fn stalled_client(to: SocketAddr, stop: Arc<AtomicBool>) -> Report {
    let mut r = Report::new("Native (stalled)");
    let mut tcp = TcpStream::connect(to).unwrap();
    tcp.set_write_timeout(Some(Duration::from_millis(200))).unwrap();
    let mut asked = 0;
    while !stop.load(Relaxed) {
        if asked < 20 && tcp.write_all(&[0xdf, 0xd1, 0xda, 0xdc, 0xda, 0xdd]).is_ok() {
            asked += 1;
        }
        // Keep-alive so the server has no reason to drop it for idling.
        let _ = tcp.write_all(&[0xdf, 0xd1]);
        std::thread::sleep(Duration::from_millis(500));
    }
    r.notes.push(format!("asked for the whole tree {asked} times, read nothing"));
    r
}

// ============================================================================
// The audio callback

/// Runs the engine like an audio device (128 samples at 48 kHz) and records
/// how long each callback takes.
struct AudioRig {
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<Vec<(Instant, Duration)>>>,
    workers: Vec<JoinHandle<()>>,
    engine: Arc<Engine>,
}

const BLOCK: usize = 128;

/// Realtime priority for the audio threads, as the app gives them (Linux,
/// when allowed: SOAK_RT=1).
fn realtime() {
    #[cfg(target_os = "linux")]
    if std::env::var_os("SOAK_RT").is_some() {
        #[repr(C)]
        struct SchedParam {
            priority: i32,
        }
        extern "C" {
            fn pthread_self() -> usize;
            fn pthread_setschedparam(thread: usize, policy: i32, param: *const SchedParam) -> i32;
        }
        const SCHED_FIFO: i32 = 1;
        // SAFETY: plain libc calls on the current thread.
        let r = unsafe { pthread_setschedparam(pthread_self(), SCHED_FIFO, &SchedParam { priority: 50 }) };
        if r != 0 {
            eprintln!("no realtime priority (error {r})");
        }
    }
}

impl AudioRig {
    fn start(engine: Arc<Engine>, num_workers: usize) -> Self {
        let workers = (0..num_workers)
            .map(|i| {
                let e = engine.clone();
                std::thread::spawn(move || {
                    realtime();
                    audio_thread(true);
                    e.pool().run_worker(i);
                    audio_thread(false);
                })
            })
            .collect();
        while engine.pool().num_running_workers() < num_workers {
            std::thread::yield_now();
        }
        let in_map: Vec<i32> = (0..32).collect();
        let out_map: Vec<i32> = (0..32).collect();
        unsafe { engine.prepare(48000.0, BLOCK, &in_map, &out_map) };
        engine.set_device_running(true);
        let stop = Arc::new(AtomicBool::new(false));
        let st = stop.clone();
        let e = engine.clone();
        let thread = std::thread::spawn(move || {
            let mut inputs: Vec<Vec<f32>> =
                (0..32).map(|c| (0..BLOCK).map(|i| ((i * 7 + c * 13) % 64) as f32 / 64.0 - 0.5).collect()).collect();
            let mut outputs = vec![vec![0.0f32; BLOCK]; 32];
            let period = Duration::from_secs_f64(BLOCK as f64 / 48000.0);
            let mut times = Vec::with_capacity(1 << 20);
            let mut next = Instant::now();
            realtime();
            while !st.load(Relaxed) {
                let ins: Vec<*const f32> = inputs.iter_mut().map(|v| v.as_ptr()).collect();
                let outs: Vec<*mut f32> = outputs.iter_mut().map(|v| v.as_mut_ptr()).collect();
                let s = Instant::now();
                audio_thread(true);
                unsafe { e.process(&ins, &outs, BLOCK) };
                audio_thread(false);
                times.push((s, s.elapsed()));
                next += period;
                let now = Instant::now();
                if next > now {
                    std::thread::sleep(next - now);
                } else {
                    next = now;
                }
            }
            times
        });
        Self { stop, thread: Some(thread), workers, engine }
    }

    fn take(&mut self) -> Vec<(Instant, Duration)> {
        self.stop.store(true, Relaxed);
        let t = self.thread.take().unwrap().join().unwrap();
        self.engine.pool().shutdown();
        for w in self.workers.drain(..) {
            let _ = w.join();
        }
        t
    }
}

fn callback_summary(what: &str, mut t: Vec<Duration>) -> String {
    if t.is_empty() {
        return format!("{what}: no callbacks");
    }
    let budget = Duration::from_secs_f64(BLOCK as f64 / 48000.0);
    let over = t.iter().filter(|d| **d > budget).count();
    let over_half = t.iter().filter(|d| **d > budget / 2).count();
    t.sort();
    let us = |q: f64| t[((t.len() - 1) as f64 * q) as usize].as_secs_f64() * 1e6;
    format!(
        "{what}: {} callbacks, median {:.0} us, p99.9 {:.0} us, worst {:.0} us (budget {:.0} us), {} over half the budget, {} over budget",
        t.len(),
        us(0.5),
        us(0.999),
        us(1.0),
        budget.as_secs_f64() * 1e6,
        over_half,
        over
    )
}

// ============================================================================

/// CPU seconds used so far by the server's threads (Linux; None elsewhere).
fn server_cpu() -> Option<f64> {
    let mut total = 0u64;
    for t in std::fs::read_dir("/proc/self/task").ok()?.flatten() {
        let comm = std::fs::read_to_string(t.path().join("comm")).unwrap_or_default();
        if !comm.starts_with("wing-") {
            continue;
        }
        let stat = std::fs::read_to_string(t.path().join("stat")).unwrap_or_default();
        // Fields after the ")" closing the name: state is field 3; utime, stime are 14 and 15.
        let rest = stat.rsplit_once(')').map(|r| r.1).unwrap_or("");
        let f: Vec<&str> = rest.split_whitespace().collect();
        total += f.get(11).and_then(|v| v.parse::<u64>().ok()).unwrap_or(0);
        total += f.get(12).and_then(|v| v.parse::<u64>().ok()).unwrap_or(0);
    }
    Some(total as f64 / 100.0)
}

fn start_server(engine: &Arc<Engine>) -> WingServer {
    let config = Config {
        identity: Identity { name: "SOAK".into(), ..Identity::default() },
        bind: IpAddr::V4(Ipv4Addr::LOCALHOST),
        discovery_port: 0,
        osc_port: 0,
        native_port: 0,
        ..Config::default()
    };
    let handle: Arc<dyn EngineHandle> = Arc::new(engine.clone());
    let server = WingServer::start(handle, Arc::new(WingState::default()), &config);
    assert!(server.status().errors.is_empty(), "{:?}", server.status().errors);
    server
}

/// Runs the clients for `secs` seconds against a fresh server; returns the
/// reports, with the audio callback's times (`workers`: run the audio with
/// that many workers).
fn soak(secs: f64, workers: Option<usize>, stalled: bool) -> Vec<Report> {
    TRACE_ALLOCS.store(std::env::var_os("SOAK_ALLOC_TRACES").is_some(), Relaxed);
    let engine = Arc::new(Engine::new(workers.unwrap_or(0)));
    engine.set_num_channels(40);
    let mut reports = Vec::new();
    let mut rig = workers.map(|w| AudioRig::start(engine.clone(), w));
    if workers.is_some() {
        // Without clients (and without the servers) first: as long as with
        // them in the long run, to compare like with like.
        let quiet = if secs > 30.0 { secs.min(300.0) } else { secs / 4.0 };
        std::thread::sleep(Duration::from_secs_f64(quiet));
    }
    let server_started = Instant::now();
    let server = start_server(&engine);
    // The message thread's duties (FX devices chosen remotely, garbage).
    let stop = Arc::new(AtomicBool::new(false));
    let collector = {
        let (e, st) = (engine.clone(), stop.clone());
        std::thread::spawn(move || {
            while !st.load(Relaxed) {
                e.collect();
                std::thread::sleep(Duration::from_millis(100));
            }
        })
    };
    let st = server.status();
    let t0 = Instant::now();
    let cpu0 = server_cpu();
    let moves_from = Arc::new(Mutex::new(None));
    // Two OSC remotes at once (Mixing Station on two tablets, say): each keeps its events.
    let osc: Vec<JoinHandle<Report>> = ["OSC (Mixing Station)", "OSC (second remote)"]
        .into_iter()
        .map(|name| {
            let to = st.osc.unwrap();
            std::thread::spawn(move || osc_client(name, to, secs, Arc::new(AtomicBool::new(false)), t0))
        })
        .collect();
    let native = {
        let (to, stop) = (st.native.unwrap(), Arc::new(AtomicBool::new(false)));
        let s2 = stop.clone();
        let mf = moves_from.clone();
        (std::thread::spawn(move || native_client(to, secs, s2, t0, mf)), stop)
    };
    let sweep = {
        let to = st.osc.unwrap();
        std::thread::spawn(move || sweeper(to, secs, Arc::new(AtomicBool::new(false)), t0))
    };
    let meter_only = {
        let to = st.native.unwrap();
        std::thread::spawn(move || meter_client(to, secs, Arc::new(AtomicBool::new(false)), t0))
    };
    let slow = {
        let to = st.native.unwrap();
        std::thread::spawn(move || slow_client(to, secs, t0))
    };
    let stall_stop = Arc::new(AtomicBool::new(false));
    let stall = stalled.then(|| {
        let (to, s2) = (st.native.unwrap(), stall_stop.clone());
        std::thread::spawn(move || stalled_client(to, s2))
    });
    for o in osc {
        reports.push(o.join().unwrap());
    }
    reports.push(native.0.join().unwrap());
    reports.push(meter_only.join().unwrap());
    reports.push(sweep.join().unwrap());
    reports.push(slow.join().unwrap());
    stall_stop.store(true, Relaxed);
    if let Some(s) = stall {
        reports.push(s.join().unwrap());
    }
    let _ = native.1;
    if let (Some(a), Some(b)) = (cpu0, server_cpu()) {
        let mut r = Report::new("server threads");
        r.notes.push(format!(
            "{:.2} s of CPU in {:.0} s ({:.1}% of one core)",
            b - a,
            t0.elapsed().as_secs_f64(),
            100.0 * (b - a) / t0.elapsed().as_secs_f64()
        ));
        reports.push(r);
    }
    if let Some(rig) = rig.as_mut() {
        let (quiet, busy): (Vec<_>, Vec<_>) = rig.take().into_iter().partition(|(at, _)| *at < server_started);
        let mut r = Report::new("audio callback");
        let allocs = AUDIO_ALLOCS.load(Relaxed);
        if allocs > 0 {
            r.problem(t0, format!("{allocs} allocations or frees on the audio threads"));
            for t in AUDIO_ALLOC_TRACES.lock().unwrap().iter() {
                r.notes.push(format!("allocated on the audio thread at:\n{t}"));
            }
        }
        r.notes.push(callback_summary("without clients", quiet.into_iter().map(|t| t.1).collect()));
        // The sets just before each slow callback.
        let budget = Duration::from_secs_f64(BLOCK as f64 / 48000.0);
        let log = SWEEP_LOG.lock().unwrap();
        for (at, d) in busy.iter().filter(|(_, d)| *d > budget) {
            let before: Vec<String> = log
                .iter()
                .filter(|(t, _, _)| *t <= *at + *d && *t + Duration::from_millis(30) >= *at)
                .map(|(t, p, v)| {
                    format!(
                        "{p}={v:.3} ({:+.1} ms)",
                        (t.saturating_duration_since(*at).as_secs_f64() - at.saturating_duration_since(*t).as_secs_f64())
                            * 1000.0
                    )
                })
                .collect();
            r.notes.push(format!(
                "slow callback {:.1} ms at {:.3}s after: {}",
                d.as_secs_f64() * 1000.0,
                at.saturating_duration_since(t0).as_secs_f64(),
                before.join(", ")
            ));
        }
        r.notes.push(callback_summary("with clients", busy.into_iter().map(|t| t.1).collect()));
        reports.push(r);
    }
    stop.store(true, Relaxed);
    let _ = collector.join();
    drop(server);
    reports
}

fn print(reports: &[Report]) -> usize {
    let mut problems = 0;
    for r in reports {
        eprintln!("== {}", r.name);
        for n in &r.notes {
            eprintln!("   {n}");
        }
        for p in &r.problems {
            eprintln!("   PROBLEM {p}");
        }
        problems += r.problems.len();
    }
    problems
}

#[test]
fn soak_short() {
    // Long enough for the 10 s subscription life and the 5 s meter life to need renewals.
    // The audio runs too (no workers): nothing a client does may allocate on it.
    let reports = soak(12.0, Some(0), true);
    assert_eq!(print(&reports), 0, "see the problems above");
}

#[test]
#[ignore = "minutes long: SOAK_SECS=300 cargo test --release -p livedaw-wing --test soak -- --ignored --nocapture"]
fn soak_long() {
    let secs: f64 = std::env::var("SOAK_SECS").ok().and_then(|s| s.parse().ok()).unwrap_or(120.0);
    let stalled = std::env::var_os("SOAK_NO_STALL").is_none();
    let workers = std::env::var("SOAK_WORKERS").ok().and_then(|s| s.parse().ok()).unwrap_or(2);
    let reports = soak(secs, Some(workers), stalled);
    assert_eq!(print(&reports), 0, "see the problems above");
}

/// Sidecar restarted (or its network came back): the same ports again, OSC
/// clients answered on the socket they kept, Native clients told their old
/// connection is gone and able to connect again.
#[test]
fn clients_come_back_after_a_restart() {
    let engine = Arc::new(Engine::new(0));
    engine.set_num_channels(40);
    let server = start_server(&engine);
    let st = server.status();
    let (osc_at, native_at, disc_at) = (st.osc.unwrap(), st.native.unwrap(), st.discovery.unwrap());
    let osc = UdpSocket::bind("127.0.0.1:0").unwrap();
    osc.set_read_timeout(Some(Duration::from_millis(500))).unwrap();
    let ask = |sock: &UdpSocket, to: SocketAddr, what: &[u8]| -> Option<Vec<u8>> {
        sock.send_to(what, to).unwrap();
        let mut buf = [0u8; 2048];
        sock.recv_from(&mut buf).ok().map(|(n, _)| buf[..n].to_vec())
    };
    let info = osc::encode(&Message::new("/?", vec![]));
    assert!(ask(&osc, osc_at, &info).is_some());
    let mut tcp = TcpStream::connect(native_at).unwrap();
    tcp.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
    tcp.write_all(&[0xdf, 0xd1, 0xda, 0xdd]).unwrap();
    let mut buf = [0u8; 65536];
    assert!(tcp.read(&mut buf).unwrap() > 0);

    drop(server);
    assert!(ask(&osc, osc_at, &info).is_none(), "answered while stopped");
    loop {
        match tcp.read(&mut buf) {
            Ok(0) | Err(_) => break,
            Ok(_) => {}
        }
    }

    let config = Config {
        identity: Identity { name: "SOAK".into(), ..Identity::default() },
        bind: IpAddr::V4(Ipv4Addr::LOCALHOST),
        discovery_port: disc_at.port(),
        osc_port: osc_at.port(),
        native_port: native_at.port(),
        ..Config::default()
    };
    let handle: Arc<dyn EngineHandle> = Arc::new(engine.clone());
    let server = WingServer::start(handle, Arc::new(WingState::default()), &config);
    assert!(server.status().errors.is_empty(), "{:?}", server.status().errors);
    let reply = ask(&osc, osc_at, &info).expect("no OSC reply after the restart");
    assert!(String::from_utf8_lossy(&reply).contains("SOAK"));
    let disc = UdpSocket::bind("127.0.0.1:0").unwrap();
    disc.set_read_timeout(Some(Duration::from_millis(500))).unwrap();
    let reply = ask(&disc, disc_at, b"WING?").expect("no discovery reply after the restart");
    assert!(reply.starts_with(b"WING,127.0.0.1,SOAK,"), "{}", String::from_utf8_lossy(&reply));
    let mut tcp = TcpStream::connect(native_at).unwrap();
    tcp.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
    tcp.write_all(&[0xdf, 0xd1, 0xda, 0xdd]).unwrap();
    assert!(tcp.read(&mut buf).unwrap() > 0);
}

/// A scene recall changes hundreds of parameters at once: every change
/// reaches each OSC subscriber and each Native client.
#[test]
fn a_burst_of_changes_reaches_every_client() {
    use livedaw_engine::StripKind;
    let engine = Arc::new(Engine::new(0));
    engine.set_num_channels(40);
    let server = start_server(&engine);
    let st = server.status();
    let subs: Vec<UdpSocket> = (0..2)
        .map(|_| {
            let s = UdpSocket::bind("127.0.0.1:0").unwrap();
            s.set_read_timeout(Some(Duration::from_millis(300))).unwrap();
            s.send_to(&osc::encode(&Message::new("/*S", vec![])), st.osc.unwrap()).unwrap();
            s
        })
        .collect();
    let mut tcp = TcpStream::connect(st.native.unwrap()).unwrap();
    tcp.set_read_timeout(Some(Duration::from_millis(300))).unwrap();
    std::thread::sleep(Duration::from_millis(400));
    // Each client reads all the time, as an app does.
    let readers: Vec<JoinHandle<std::collections::HashSet<String>>> = subs
        .into_iter()
        .map(|s| {
            std::thread::spawn(move || {
                let mut got = std::collections::HashSet::new();
                let mut buf = vec![0u8; 65536];
                let mut quiet_since = Instant::now();
                while quiet_since.elapsed() < Duration::from_millis(600) {
                    if let Ok((n, _)) = s.recv_from(&mut buf) {
                        if let Some(m) = osc::decode(&buf[..n]) {
                            got.insert(m.addr);
                        }
                        quiet_since = Instant::now();
                    }
                }
                got
            })
        })
        .collect();
    let mut paths = std::collections::HashSet::new();
    for (kind, prefix, count) in [
        (StripKind::Channel, "ch", 40),
        (StripKind::Aux, "aux", 8),
        (StripKind::Bus, "bus", 16),
        (StripKind::Main, "main", 4),
        (StripKind::Matrix, "mtx", 8),
    ] {
        for i in 0..count {
            let s = engine.strip_at(kind, i).unwrap();
            s.params.fader_db.store(-3.0 - (i % 7) as f32);
            s.params.pan.store(10.0 + i as f32);
            s.params.mute.store(true, Relaxed);
            for p in ["fdr", "pan", "mute"] {
                paths.insert(format!("/{prefix}/{}/{p}", i + 1));
            }
        }
    }
    let ids: std::collections::HashSet<u32> = paths.iter().map(|p| id(p)).collect();
    for (k, r) in readers.into_iter().enumerate() {
        let got = r.join().unwrap();
        let missing: Vec<_> = paths.iter().filter(|p| !got.contains(*p)).collect();
        assert!(missing.is_empty(), "subscriber {k} missed {} of {}: {missing:?}", missing.len(), paths.len());
    }
    let mut dec = Decoder::default();
    let mut toks = Vec::new();
    let mut buf = vec![0u8; 65536];
    while let Ok(n) = tcp.read(&mut buf) {
        if n == 0 {
            break;
        }
        dec.feed(&buf[..n], &mut toks);
    }
    let got: std::collections::HashSet<u32> =
        toks.iter().filter_map(|t| if let Tok::Data(i) = t { Some(*i) } else { None }).collect();
    assert_eq!(ids.difference(&got).count(), 0, "the Native client missed changes");
}
