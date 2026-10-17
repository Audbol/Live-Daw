//! The remote servers: discovery (UDP 2222), OSC (UDP 2223) and the Native
//! protocol (TCP 2222, meters over UDP), each on its own std::net thread, plus
//! a poller that pushes changes (from any source: the UI, scene recalls, other
//! clients) to the OSC subscriber and the Native clients, and sends meters.
//!
//! Nothing here runs on the audio thread: the servers read and write the
//! engine's shared atomics like the UI does.

use crate::map::{bindings, Binding, Console, Identity, SetError, WingState};
use crate::meters::{self, Meter};
use crate::native::{self, MeterRequest, Session};
use crate::osc::{self, Arg, Effect, Message, SubFormat};
use crate::schema::{schema, Node};
use crate::value::Value;
use livedaw_engine::Engine;
use std::collections::HashMap;
use std::io::{ErrorKind, Read, Write};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, TcpListener, TcpStream, UdpSocket};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, MutexGuard, OnceLock};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

/// Access to an engine owned elsewhere.
pub trait EngineHandle: Send + Sync + 'static {
    fn engine(&self) -> &Engine;
}

impl EngineHandle for Arc<Engine> {
    fn engine(&self) -> &Engine {
        self
    }
}

/// An engine owned by the host (the C ABI's handle).
pub struct EnginePtr(pub *const Engine);

// SAFETY: the engine is Sync; the host keeps it alive while the server runs
// (the C ABI stops the server before the engine is destroyed).
unsafe impl Send for EnginePtr {}
unsafe impl Sync for EnginePtr {}

impl EngineHandle for EnginePtr {
    fn engine(&self) -> &Engine {
        // SAFETY: see above.
        unsafe { &*self.0 }
    }
}

/// The WING's ports.
pub const DISCOVERY_PORT: u16 = 2222;
pub const OSC_PORT: u16 = 2223;
pub const NATIVE_PORT: u16 = 2222;

/// Maximum Native clients (the console's limit).
pub const MAX_NATIVE_CLIENTS: usize = 16;
/// Subscriptions die without a renewal (OSC: 10 s, meters: 5 s).
pub const OSC_SUBSCRIPTION_LIFE: Duration = Duration::from_secs(10);
pub const METER_LIFE: Duration = Duration::from_secs(5);
/// A Native client that sends nothing for this long is dropped (the console
/// uses 10 s; clients send a keep-alive every 7 s).
pub const NATIVE_IDLE: Duration = Duration::from_secs(12);

#[derive(Clone, Debug)]
pub struct Config {
    pub identity: Identity,
    pub bind: IpAddr,
    /// 0 = any free port (tests).
    pub discovery_port: u16,
    pub osc_port: u16,
    pub native_port: u16,
    pub discovery: bool,
    pub osc: bool,
    pub native: bool,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            identity: Identity::default(),
            bind: IpAddr::V4(Ipv4Addr::UNSPECIFIED),
            discovery_port: DISCOVERY_PORT,
            osc_port: OSC_PORT,
            native_port: NATIVE_PORT,
            discovery: true,
            osc: true,
            native: true,
        }
    }
}

/// Who caused a change (it isn't echoed back to them).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Origin {
    None,
    Osc(SocketAddr),
    Native(u64),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ClientKind {
    Native,
    Osc,
}

#[derive(Clone, Debug)]
pub struct ClientInfo {
    pub kind: ClientKind,
    pub addr: SocketAddr,
    /// Native: has a meter subscription. OSC: holds the event subscription.
    pub subscribed: bool,
    pub seconds: u64,
}

#[derive(Clone, Debug, Default)]
pub struct Status {
    pub discovery: Option<SocketAddr>,
    pub osc: Option<SocketAddr>,
    pub native: Option<SocketAddr>,
    pub clients: Vec<ClientInfo>,
    pub errors: Vec<String>,
    pub osc_messages: u64,
    pub native_bytes: u64,
}

struct OscSubscription {
    addr: SocketAddr,
    format: SubFormat,
    expires: Instant,
}

struct MeterSub {
    port: Option<u16>,
    report_id: u32,
    meters: Vec<Meter>,
    expires: Instant,
}

struct NativeClient {
    id: u64,
    peer: SocketAddr,
    writer: Mutex<TcpStream>,
    meters: Mutex<MeterSub>,
    alive: AtomicBool,
    since: Instant,
}

impl NativeClient {
    fn send(&self, bytes: &[u8]) {
        if !self.alive.load(Ordering::Relaxed) {
            return;
        }
        let mut w = self.writer.lock().unwrap_or_else(|e| e.into_inner());
        if w.write_all(bytes).is_err() {
            self.alive.store(false, Ordering::Relaxed);
            let _ = w.shutdown(std::net::Shutdown::Both);
        }
    }
}

/// State shared by the server threads.
pub struct Shared {
    engine: Arc<dyn EngineHandle>,
    pub state: Arc<WingState>,
    /// Last values pushed to subscribers (None while nobody listens).
    notify: Mutex<Option<HashMap<Node, Value>>>,
    clients: Mutex<Vec<Arc<NativeClient>>>,
    osc_sub: Mutex<Option<OscSubscription>>,
    osc_peers: Mutex<HashMap<SocketAddr, Instant>>,
    osc_tx: Mutex<Option<UdpSocket>>,
    stop: AtomicBool,
    next_id: AtomicU64,
    osc_messages: AtomicU64,
    native_bytes: AtomicU64,
}

fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

/// Static rows bound to the engine (polled for changes).
fn mapped_rows() -> &'static [u16] {
    static R: OnceLock<Vec<u16>> = OnceLock::new();
    R.get_or_init(|| {
        let s = schema();
        bindings()
            .iter()
            .enumerate()
            .filter(|(r, b)| **b != Binding::None && s.defs[s.rows[*r].def as usize].is_leaf())
            .map(|(r, _)| r as u16)
            .collect()
    })
}

impl Shared {
    pub fn new(engine: Arc<dyn EngineHandle>, state: Arc<WingState>) -> Self {
        Self {
            engine,
            state,
            notify: Mutex::new(None),
            clients: Mutex::default(),
            osc_sub: Mutex::new(None),
            osc_peers: Mutex::default(),
            osc_tx: Mutex::new(None),
            stop: AtomicBool::new(false),
            next_id: AtomicU64::new(1),
            osc_messages: AtomicU64::new(0),
            native_bytes: AtomicU64::new(0),
        }
    }

    pub fn engine(&self) -> &Engine {
        self.engine.engine()
    }

    pub fn console(&self) -> Console<'_> {
        Console::new(self.engine.engine(), &self.state)
    }

    /// "WING,<ip>,<name>,<model>,<serial>,<firmware>" (discovery and `/?`).
    pub fn info_string(&self, ip: Option<IpAddr>) -> String {
        let id = self.state.identity();
        let ip = ip.map_or_else(|| "0.0.0.0".to_owned(), |i| i.to_string());
        let clean = |s: &str| s.replace(',', " ");
        format!("WING,{ip},{},{},{},{}", clean(&id.name), clean(&id.model), clean(&id.serial), clean(&id.firmware))
    }

    fn has_listeners(&self) -> bool {
        let osc = lock(&self.osc_sub).as_ref().is_some_and(|s| s.expires > Instant::now());
        osc || !lock(&self.clients).is_empty()
    }

    /// Sets a parameter for a client and tells everyone else.
    pub fn apply_set(&self, node: Node, v: &Value, origin: Origin) -> Result<Value, SetError> {
        self.apply(node, origin, |c| c.set(node, v))
    }

    pub fn apply_toggle(&self, node: Node, origin: Origin) -> Result<Value, SetError> {
        self.apply(node, origin, |c| c.toggle(node))
    }

    pub fn apply_step(&self, node: Node, amount: i32, origin: Origin) -> Result<Value, SetError> {
        self.apply(node, origin, |c| c.step(node, amount))
    }

    fn apply(&self, node: Node, origin: Origin, f: impl FnOnce(&Console) -> Result<Value, SetError>) -> Result<Value, SetError> {
        let mut cache = lock(&self.notify);
        let c = self.console();
        let v = f(&c)?;
        if let Some(cache) = cache.as_mut() {
            if c.is_mapped(node) {
                cache.insert(node, v.clone());
            }
        }
        self.broadcast(&c, &[(node, v.clone())], origin);
        Ok(v)
    }

    /// Pushes changes to the OSC subscriber and the Native clients (not to `origin`).
    fn broadcast(&self, c: &Console, changes: &[(Node, Value)], origin: Origin) {
        if changes.is_empty() {
            return;
        }
        let s = schema();
        // OSC
        let sub = lock(&self.osc_sub).as_ref().filter(|s| s.expires > Instant::now()).map(|s| (s.addr, s.format));
        if let Some((addr, format)) = sub {
            if origin != Origin::Osc(addr) {
                if let Some(sock) = lock(&self.osc_tx).as_ref() {
                    match format {
                        SubFormat::Binary => {
                            let mut blob = Vec::new();
                            for (n, v) in changes {
                                native::encode_data(&mut blob, s.id(*n), v);
                                if blob.len() > osc::MAX_PACKET - 512 {
                                    let _ = sock.send_to(
                                        &osc::encode(&Message::new("/", vec![Arg::Blob(std::mem::take(&mut blob))])),
                                        addr,
                                    );
                                }
                            }
                            if !blob.is_empty() {
                                let _ = sock.send_to(&osc::encode(&Message::new("/", vec![Arg::Blob(blob)])), addr);
                            }
                        }
                        _ => {
                            for (n, v) in changes {
                                let Some(def) = s.def(*n, c) else { continue };
                                let path = s.path(*n, c);
                                let msg = if format == SubFormat::Native {
                                    osc::native_event(&path, def, v)
                                } else {
                                    osc::param_reply(&path, def, v)
                                };
                                let _ = sock.send_to(&osc::encode(&msg), addr);
                            }
                        }
                    }
                }
            }
        }
        // Native
        let clients: Vec<Arc<NativeClient>> = lock(&self.clients).clone();
        if clients.iter().any(|cl| origin != Origin::Native(cl.id)) {
            let mut payload = Vec::with_capacity(changes.len() * 10);
            for (n, v) in changes {
                native::encode_data(&mut payload, s.id(*n), v);
            }
            let wire = native::frame(native::CH_AUDIO, &payload);
            for cl in clients.iter().filter(|cl| origin != Origin::Native(cl.id)) {
                cl.send(&wire);
            }
        }
    }

    /// Every engine-backed leaf with its value.
    fn snapshot(&self, c: &Console) -> Vec<(Node, Value)> {
        let s = schema();
        let mut out = Vec::with_capacity(mapped_rows().len() + 4096);
        for &r in mapped_rows() {
            let n = Node::Row(r);
            if c.is_mapped(n) {
                if let Some(v) = c.get(n) {
                    out.push((n, v));
                }
            }
        }
        for (slot, sl) in s.slots.iter().enumerate() {
            if !c.is_mapped(Node::Row(sl.row)) {
                continue;
            }
            for ch in s.children(Node::Row(sl.row), c) {
                if let Node::Param { .. } = ch {
                    if c.is_mapped(ch) {
                        if let Some(v) = c.get(ch) {
                            out.push((ch, v));
                        }
                    }
                }
            }
            let _ = slot;
        }
        out
    }

    /// Finds what changed since the last poll and pushes it.
    pub fn poll_changes(&self) {
        let mut cache = lock(&self.notify);
        if !self.has_listeners() {
            *cache = None;
            return;
        }
        let c = self.console();
        let now = self.snapshot(&c);
        match cache.as_mut() {
            None => *cache = Some(now.into_iter().collect()),
            Some(map) => {
                let mut changes = Vec::new();
                for (n, v) in now {
                    if map.get(&n) != Some(&v) {
                        map.insert(n, v.clone());
                        changes.push((n, v));
                    }
                }
                self.broadcast(&c, &changes, Origin::None);
            }
        }
    }

    fn send_meters(&self, sock: &UdpSocket) {
        let clients: Vec<Arc<NativeClient>> = lock(&self.clients).clone();
        let now = Instant::now();
        let mut c = None;
        for cl in clients {
            let sub = lock(&cl.meters);
            let Some(port) = sub.port else { continue };
            if sub.meters.is_empty() || sub.expires < now {
                continue;
            }
            let c = c.get_or_insert_with(|| self.console());
            let words = meters::frame_words(c, &sub.meters);
            let _ = sock.send_to(&meters::datagram(sub.report_id, &words), SocketAddr::new(cl.peer.ip(), port));
        }
    }

    fn subscribe_osc(&self, addr: SocketAddr, format: SubFormat) {
        *lock(&self.osc_sub) = Some(OscSubscription { addr, format, expires: Instant::now() + OSC_SUBSCRIPTION_LIFE });
    }

    pub fn status(&self) -> (Vec<ClientInfo>, u64, u64) {
        let now = Instant::now();
        let mut out = Vec::new();
        for cl in lock(&self.clients).iter() {
            let m = lock(&cl.meters);
            out.push(ClientInfo {
                kind: ClientKind::Native,
                addr: cl.peer,
                subscribed: !m.meters.is_empty() && m.expires > now,
                seconds: now.duration_since(cl.since).as_secs(),
            });
        }
        let sub = lock(&self.osc_sub).as_ref().filter(|s| s.expires > now).map(|s| s.addr);
        let mut peers = lock(&self.osc_peers);
        peers.retain(|_, seen| now.duration_since(*seen) < Duration::from_secs(15));
        for (addr, seen) in peers.iter() {
            out.push(ClientInfo {
                kind: ClientKind::Osc,
                addr: *addr,
                subscribed: sub == Some(*addr),
                seconds: now.duration_since(*seen).as_secs(),
            });
        }
        (out, self.osc_messages.load(Ordering::Relaxed), self.native_bytes.load(Ordering::Relaxed))
    }

    /// Handles one OSC datagram; replies through `sock`.
    pub fn handle_osc(&self, sock: &UdpSocket, data: &[u8], from: SocketAddr) {
        let Some(msg) = osc::decode(data) else { return };
        self.osc_messages.fetch_add(1, Ordering::Relaxed);
        lock(&self.osc_peers).insert(from, Instant::now());
        let (mut replies, port, effect) = osc::handle(self, &msg, Origin::Osc(from));
        let dest = port.map_or(from, |p| SocketAddr::new(from.ip(), p));
        if let Some(Effect::Subscribe(format)) = effect {
            self.subscribe_osc(dest, format);
        }
        // `/?` reports the address the client reached us on.
        for r in replies.iter_mut() {
            if r.addr == "/?" {
                r.args = vec![Arg::Str(self.info_string(Some(local_ip_for(from))))];
            }
            let bytes = osc::encode(r);
            if bytes.len() <= osc::MAX_PACKET {
                let _ = sock.send_to(&bytes, dest);
            }
        }
    }
}

/// The local address a reply to `peer` leaves from.
pub fn local_ip_for(peer: SocketAddr) -> IpAddr {
    let unspecified = if peer.is_ipv4() { "0.0.0.0:0" } else { "[::]:0" };
    UdpSocket::bind(unspecified)
        .and_then(|s| {
            s.connect(peer)?;
            s.local_addr()
        })
        .map(|a| a.ip())
        .ok()
        .filter(|ip| !ip.is_unspecified())
        .unwrap_or(IpAddr::V4(Ipv4Addr::LOCALHOST))
}

/// The running servers. Stops them when dropped.
pub struct WingServer {
    shared: Arc<Shared>,
    threads: Vec<JoinHandle<()>>,
    status: Status,
}

impl WingServer {
    /// Starts the servers `config` enables. A port that can't be bound is
    /// reported in [`Status::errors`]; the others still start.
    pub fn start(engine: Arc<dyn EngineHandle>, state: Arc<WingState>, config: &Config) -> WingServer {
        *lock(&state.identity) = config.identity.clone();
        let shared = Arc::new(Shared::new(engine, state));
        let mut status = Status::default();
        let mut threads = Vec::new();

        if config.discovery {
            match UdpSocket::bind((config.bind, config.discovery_port)) {
                Ok(sock) => {
                    status.discovery = sock.local_addr().ok();
                    let sh = shared.clone();
                    threads.push(spawn("wing-discovery", move || discovery_loop(&sh, sock)));
                }
                Err(e) => status.errors.push(format!("Discovery (UDP {}): {e}", config.discovery_port)),
            }
        }
        if config.osc {
            match UdpSocket::bind((config.bind, config.osc_port)) {
                Ok(sock) => {
                    status.osc = sock.local_addr().ok();
                    *lock(&shared.osc_tx) = sock.try_clone().ok();
                    let sh = shared.clone();
                    threads.push(spawn("wing-osc", move || osc_loop(&sh, sock)));
                }
                Err(e) => status.errors.push(format!("OSC (UDP {}): {e}", config.osc_port)),
            }
        }
        if config.native {
            match TcpListener::bind((config.bind, config.native_port)) {
                Ok(listener) => {
                    status.native = listener.local_addr().ok();
                    let sh = shared.clone();
                    threads.push(spawn("wing-native", move || native_loop(&sh, listener)));
                }
                Err(e) => status.errors.push(format!("Native (TCP {}): {e}", config.native_port)),
            }
        }
        let sh = shared.clone();
        threads.push(spawn("wing-poll", move || poll_loop(&sh)));
        WingServer { shared, threads, status }
    }

    pub fn shared(&self) -> &Arc<Shared> {
        &self.shared
    }

    pub fn status(&self) -> Status {
        let mut s = self.status.clone();
        let (clients, osc, native) = self.shared.status();
        s.clients = clients;
        s.osc_messages = osc;
        s.native_bytes = native;
        s
    }

    pub fn stop(&mut self) {
        self.shared.stop.store(true, Ordering::Relaxed);
        for cl in lock(&self.shared.clients).iter() {
            let _ = lock(&cl.writer).shutdown(std::net::Shutdown::Both);
        }
        for t in self.threads.drain(..) {
            let _ = t.join();
        }
        lock(&self.shared.clients).clear();
    }
}

impl Drop for WingServer {
    fn drop(&mut self) {
        self.stop();
    }
}

fn spawn(name: &str, f: impl FnOnce() + Send + 'static) -> JoinHandle<()> {
    std::thread::Builder::new()
        .name(name.into())
        .spawn(move || {
            // A bug must not take the app down.
            let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(f));
        })
        .expect("spawning a thread")
}

const TICK: Duration = Duration::from_millis(100);

fn discovery_loop(sh: &Shared, sock: UdpSocket) {
    let _ = sock.set_read_timeout(Some(TICK));
    let mut buf = [0u8; 1500];
    while !sh.stop.load(Ordering::Relaxed) {
        match sock.recv_from(&mut buf) {
            Ok((n, from)) => {
                if buf[..n].trim_ascii_end().starts_with(b"WING?") {
                    let _ = sock.send_to(sh.info_string(Some(local_ip_for(from))).as_bytes(), from);
                }
            }
            Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut | ErrorKind::Interrupted) => {}
            Err(_) => std::thread::sleep(TICK),
        }
    }
}

fn osc_loop(sh: &Shared, sock: UdpSocket) {
    let _ = sock.set_read_timeout(Some(TICK));
    let mut buf = vec![0u8; 65536];
    while !sh.stop.load(Ordering::Relaxed) {
        match sock.recv_from(&mut buf) {
            Ok((n, from)) => sh.handle_osc(&sock, &buf[..n], from),
            Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut | ErrorKind::Interrupted) => {}
            // e.g. an ICMP "port unreachable" from a client that went away (Windows)
            Err(_) => std::thread::sleep(Duration::from_millis(5)),
        }
    }
}

fn native_loop(sh: &Arc<Shared>, listener: TcpListener) {
    let _ = listener.set_nonblocking(true);
    let mut readers: Vec<JoinHandle<()>> = Vec::new();
    while !sh.stop.load(Ordering::Relaxed) {
        match listener.accept() {
            Ok((stream, peer)) => {
                let _ = stream.set_nonblocking(false);
                if lock(&sh.clients).len() >= MAX_NATIVE_CLIENTS {
                    drop(stream);
                    continue;
                }
                let Ok(writer) = stream.try_clone() else { continue };
                let _ = stream.set_nodelay(true);
                let _ = writer.set_write_timeout(Some(Duration::from_secs(2)));
                let client = Arc::new(NativeClient {
                    id: sh.next_id.fetch_add(1, Ordering::Relaxed),
                    peer,
                    writer: Mutex::new(writer),
                    meters: Mutex::new(MeterSub { port: None, report_id: 0, meters: Vec::new(), expires: Instant::now() }),
                    alive: AtomicBool::new(true),
                    since: Instant::now(),
                });
                lock(&sh.clients).push(client.clone());
                let sh2 = sh.clone();
                readers.push(spawn("wing-native-client", move || client_loop(&sh2, client, stream)));
            }
            Err(e) if e.kind() == ErrorKind::WouldBlock => std::thread::sleep(Duration::from_millis(20)),
            Err(_) => std::thread::sleep(TICK),
        }
        readers.retain(|t| !t.is_finished());
    }
    for t in readers {
        let _ = t.join();
    }
}

fn client_loop(sh: &Shared, client: Arc<NativeClient>, mut stream: TcpStream) {
    let _ = stream.set_read_timeout(Some(TICK));
    let mut session = Session::new(Origin::Native(client.id));
    let mut buf = vec![0u8; 16384];
    let mut last = Instant::now();
    while !sh.stop.load(Ordering::Relaxed) && client.alive.load(Ordering::Relaxed) {
        match stream.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => {
                last = Instant::now();
                sh.native_bytes.fetch_add(n as u64, Ordering::Relaxed);
                session.feed(sh, &buf[..n]);
                if !session.out.is_empty() {
                    let payload = std::mem::take(&mut session.out);
                    client.send(&native::frame(native::CH_AUDIO, &payload));
                }
                for req in session.meter_requests.drain(..) {
                    let mut m = lock(&client.meters);
                    match req {
                        MeterRequest::Port(p) => m.port = Some(p),
                        MeterRequest::Subscribe { report_id, meters } => {
                            m.report_id = report_id;
                            m.meters = meters;
                            m.expires = Instant::now() + METER_LIFE;
                        }
                        MeterRequest::Renew { report_id } => {
                            if m.report_id == report_id {
                                m.expires = Instant::now() + METER_LIFE;
                            }
                        }
                    }
                }
            }
            Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut | ErrorKind::Interrupted) => {
                if last.elapsed() > NATIVE_IDLE {
                    break;
                }
            }
            Err(_) => break,
        }
    }
    client.alive.store(false, Ordering::Relaxed);
    let _ = stream.shutdown(std::net::Shutdown::Both);
    lock(&sh.clients).retain(|c| c.id != client.id);
}

fn poll_loop(sh: &Shared) {
    let meter_sock = UdpSocket::bind("0.0.0.0:0").ok();
    let mut tick = 0u64;
    while !sh.stop.load(Ordering::Relaxed) {
        std::thread::sleep(Duration::from_millis(50));
        tick += 1;
        if let Some(sock) = &meter_sock {
            sh.send_meters(sock);
        }
        if tick % 2 == 0 {
            sh.poll_changes();
        }
        lock(&sh.clients).retain(|c| c.alive.load(Ordering::Relaxed));
    }
}
