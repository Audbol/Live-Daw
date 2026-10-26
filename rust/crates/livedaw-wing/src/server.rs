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
use crate::schema::{schema, ModelSource, Node};
use crate::value::Value;
use livedaw_engine::Engine;
use std::collections::{HashMap, VecDeque};
use std::io::{ErrorKind, Read, Write};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, TcpListener, TcpStream, UdpSocket};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, OnceLock};
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
/// Subscriptions die without a renewal (the console: OSC 10 s, meters 5 s).
/// Sidecar gives a renewal that arrives a little late (Wi-Fi) 20% more, so a
/// client renewing just inside the console's limit never sees a gap.
pub const OSC_SUBSCRIPTION_LIFE: Duration = Duration::from_secs(12);
pub const METER_LIFE: Duration = Duration::from_secs(6);
/// A Native client that sends nothing for this long is dropped (the console
/// uses 10 s; clients send a keep-alive every 7 s).
pub const NATIVE_IDLE: Duration = Duration::from_secs(12);
/// A Native client whose connection takes nothing for this long is dropped.
/// Its data waits in its own queue meanwhile: a slow client (a tablet on
/// Wi-Fi) never holds up the other clients, the OSC server or the meters.
pub const NATIVE_WRITE_STALL: Duration = Duration::from_secs(10);
/// A Native client with this much data waiting is dropped.
pub const NATIVE_MAX_QUEUED: usize = 32 << 20;
/// A Native client's requests are read only while less than this waits for it
/// (TCP then holds the client back, as a console would).
const NATIVE_READ_PAUSE: usize = 1 << 20;
/// OSC subscribers served at once. The console serves one (the last to ask);
/// Sidecar serves each client that subscribes, so two remotes on OSC don't
/// take the event stream from each other every few seconds.
pub const MAX_OSC_SUBSCRIBERS: usize = 16;
/// OSC events leave at most this many datagrams per millisecond. A scene
/// recall changes hundreds of parameters at once, one datagram each: sent in
/// one go they overflow the receive buffer of the client (a phone or tablet,
/// often on Wi-Fi) and it misses changes.
pub const OSC_EVENTS_PER_MS: usize = 8;
/// At most this many OSC events leave at once: where a 1 ms sleep takes
/// longer (Windows' timer ticks at 15.6 ms by default, a busy machine), the
/// sender catches up with bigger batches, but never more than this.
const OSC_EVENTS_BURST: usize = 8 * OSC_EVENTS_PER_MS;

/// How many OSC events may leave now, `since` the last batch.
fn paced_batch(since: Duration) -> usize {
    let due = since.as_micros() as usize * OSC_EVENTS_PER_MS / 1000;
    due.clamp(OSC_EVENTS_PER_MS, OSC_EVENTS_BURST)
}

/// OSC events waiting beyond this are dropped (oldest first).
const OSC_MAX_QUEUED: usize = 1 << 18;

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
    /// Where events go (the requester, or its `/%<port>`).
    addr: SocketAddr,
    /// Who asked (its own changes aren't echoed to it).
    requester: SocketAddr,
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
    /// For shutting the connection down from any thread.
    stream: TcpStream,
    /// Data waiting for the client's writer thread.
    out: Mutex<Vec<u8>>,
    ready: Condvar,
    meters: Mutex<MeterSub>,
    alive: AtomicBool,
    since: Instant,
}

impl NativeClient {
    /// Queues `bytes` for the client; never blocks on the network.
    fn send(&self, bytes: &[u8]) {
        if !self.alive.load(Ordering::Relaxed) {
            return;
        }
        let mut out = lock(&self.out);
        if out.len() + bytes.len() > NATIVE_MAX_QUEUED {
            drop(out);
            self.close();
            return;
        }
        out.extend_from_slice(bytes);
        self.ready.notify_one();
    }

    fn queued(&self) -> usize {
        lock(&self.out).len()
    }

    fn close(&self) {
        self.alive.store(false, Ordering::Relaxed);
        let _ = self.stream.shutdown(std::net::Shutdown::Both);
        self.ready.notify_all();
    }
}

/// State shared by the server threads.
pub struct Shared {
    engine: Arc<dyn EngineHandle>,
    pub state: Arc<WingState>,
    /// Last values pushed to subscribers (None while nobody listens).
    notify: Mutex<Option<HashMap<Node, Value>>>,
    /// The model of each processing slot at the last poll (see `poll_changes`).
    slot_models: Mutex<Vec<Option<usize>>>,
    /// Bumped (under `notify`) by every client change; see `poll_changes`.
    generation: AtomicU64,
    clients: Mutex<Vec<Arc<NativeClient>>>,
    osc_subs: Mutex<Vec<OscSubscription>>,
    osc_peers: Mutex<HashMap<SocketAddr, Instant>>,
    /// The paced OSC event sender runs (`osc_send_loop`).
    osc_events_on: AtomicBool,
    /// OSC events for the paced sender (`osc_send_loop`).
    osc_events: Mutex<VecDeque<(SocketAddr, Arc<[u8]>)>>,
    osc_ready: Condvar,
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
            .filter(|(r, b)| !matches!(**b, Binding::None | Binding::Stored) && s.defs[s.rows[*r].def as usize].is_leaf())
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
            slot_models: Mutex::default(),
            generation: AtomicU64::new(0),
            clients: Mutex::default(),
            osc_subs: Mutex::default(),
            osc_peers: Mutex::default(),
            osc_events_on: AtomicBool::new(false),
            osc_events: Mutex::default(),
            osc_ready: Condvar::new(),
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
        let now = Instant::now();
        let osc = lock(&self.osc_subs).iter().any(|s| s.expires > now);
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
        self.generation.fetch_add(1, Ordering::AcqRel);
        if let Some(cache) = cache.as_mut() {
            if c.is_mapped(node) {
                cache.insert(node, v.clone());
            }
        }
        self.broadcast(&c, &[(node, v.clone())], origin);
        Ok(v)
    }

    /// Pushes changes to the OSC subscribers and the Native clients (not to `origin`).
    fn broadcast(&self, c: &Console, changes: &[(Node, Value)], origin: Origin) {
        if changes.is_empty() {
            return;
        }
        let s = schema();
        self.queue_osc_events(c, changes, origin);
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

    /// Queues the events of `changes` for the OSC subscribers (not `origin`)
    /// for the paced sender.
    fn queue_osc_events(&self, c: &Console, changes: &[(Node, Value)], origin: Origin) {
        let now = Instant::now();
        let subs: Vec<(SocketAddr, SubFormat)> = lock(&self.osc_subs)
            .iter()
            .filter(|s| s.expires > now && origin != Origin::Osc(s.requester))
            .map(|s| (s.addr, s.format))
            .collect();
        if subs.is_empty() || !self.osc_events_on.load(Ordering::Relaxed) {
            return;
        }
        let s = schema();
        // Each format's datagrams are built once for all its subscribers.
        // (subscriber, its datagrams)
        type Datagrams = Arc<Vec<Arc<[u8]>>>;
        let mut streams: Vec<(SocketAddr, Datagrams)> = Vec::with_capacity(subs.len());
        for format in [SubFormat::Binary, SubFormat::Triplet, SubFormat::Native] {
            let to: Vec<SocketAddr> = subs.iter().filter(|s| s.1 == format).map(|s| s.0).collect();
            if to.is_empty() {
                continue;
            }
            let mut packets: Vec<Arc<[u8]>> = Vec::new();
            if format == SubFormat::Binary {
                let mut blob = Vec::new();
                for (n, v) in changes {
                    native::encode_data(&mut blob, s.id(*n), v);
                    if blob.len() > osc::MAX_PACKET - 512 {
                        packets.push(osc::encode(&Message::new("/", vec![Arg::Blob(std::mem::take(&mut blob))])).into());
                    }
                }
                if !blob.is_empty() {
                    packets.push(osc::encode(&Message::new("/", vec![Arg::Blob(blob)])).into());
                }
            } else {
                for (n, v) in changes {
                    let Some(def) = s.def(*n, c) else { continue };
                    let path = s.path(*n, c);
                    let msg = if format == SubFormat::Native {
                        osc::native_event(&path, def, v)
                    } else {
                        osc::param_reply(&path, def, v)
                    };
                    packets.push(osc::encode(&msg).into());
                }
            }
            let packets = Arc::new(packets);
            streams.extend(to.into_iter().map(|addr| (addr, packets.clone())));
        }
        // Interleaved: each subscriber gets its next event in turn, so with
        // several subscribers none waits for another's whole burst.
        let longest = streams.iter().map(|(_, p)| p.len()).max().unwrap_or(0);
        let mut q = lock(&self.osc_events);
        for i in 0..longest {
            q.extend(streams.iter().filter_map(|(addr, p)| p.get(i).map(|p| (*addr, p.clone()))));
        }
        let excess = q.len().saturating_sub(OSC_MAX_QUEUED);
        q.drain(..excess);
        self.osc_ready.notify_one();
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

    /// Takes the values changes are found against, if nobody listened
    /// before: right when a client arrives, so that a change made before the
    /// next poll (after the client read the old value) still reaches it.
    fn ensure_baseline(&self) {
        if lock(&self.notify).is_some() {
            return;
        }
        // A value changed meanwhile is the old one here: the next poll sends it.
        let now = self.snapshot(&self.console());
        let mut cache = lock(&self.notify);
        if cache.is_none() {
            *cache = Some(now.into_iter().collect());
        }
    }

    /// Finds what changed since the last poll and pushes it.
    pub fn poll_changes(&self) {
        {
            // Under the lock: a client arriving now (ensure_baseline) keeps its baseline.
            let mut cache = lock(&self.notify);
            if !self.has_listeners() {
                *cache = None;
                return;
            }
        }
        // The snapshot is taken without the lock so that it never holds up a
        // client's set. One that raced a client change may hold the old value,
        // so it is dropped and the next poll tries again.
        let generation = self.generation.load(Ordering::Acquire);
        let c = self.console();
        // Taken first: a model that changes meanwhile is pushed in full next time.
        let models: Vec<Option<usize>> = (0..schema().slots.len()).map(|i| c.active_model(i as u16)).collect();
        let now = self.snapshot(&c);
        let mut cache = lock(&self.notify);
        if self.generation.load(Ordering::Acquire) != generation {
            return;
        }
        // A slot whose model changed sends all of the new model's parameters,
        // also those that happen to equal the old model's at the same
        // position: a client (WING Edit) starts the new model from its own
        // defaults, which needn't be Sidecar's.
        let renewed: Vec<bool> = {
            let mut last = lock(&self.slot_models);
            let renewed = models.iter().enumerate().map(|(i, m)| last.get(i).is_some_and(|old| old != m)).collect();
            *last = models;
            renewed
        };
        match cache.as_mut() {
            None => *cache = Some(now.into_iter().collect()),
            Some(map) => {
                let mut changes = Vec::new();
                for (n, v) in now {
                    let forced = matches!(n, Node::Param { slot, .. } if renewed.get(slot as usize) == Some(&true));
                    if forced || map.get(&n) != Some(&v) {
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

    fn subscribe_osc(&self, addr: SocketAddr, requester: SocketAddr, format: SubFormat) {
        let now = Instant::now();
        let mut subs = lock(&self.osc_subs);
        subs.retain(|s| s.expires > now && s.addr != addr);
        if subs.len() >= MAX_OSC_SUBSCRIBERS {
            // The longest without a renewal makes way.
            if let Some(i) = subs.iter().enumerate().min_by_key(|(_, s)| s.expires).map(|(i, _)| i) {
                subs.remove(i);
            }
        }
        subs.push(OscSubscription { addr, requester, format, expires: now + OSC_SUBSCRIPTION_LIFE });
        drop(subs);
        self.ensure_baseline();
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
        let subs: Vec<(SocketAddr, SocketAddr)> =
            lock(&self.osc_subs).iter().filter(|s| s.expires > now).map(|s| (s.addr, s.requester)).collect();
        let mut peers = lock(&self.osc_peers);
        peers.retain(|_, seen| now.duration_since(*seen) < Duration::from_secs(15));
        for (addr, seen) in peers.iter() {
            out.push(ClientInfo {
                kind: ClientKind::Osc,
                addr: *addr,
                subscribed: subs.iter().any(|(to, from)| to == addr || from == addr),
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
            self.subscribe_osc(dest, from, format);
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
                    if let Ok(tx) = sock.try_clone() {
                        shared.osc_events_on.store(true, Ordering::Relaxed);
                        let sh = shared.clone();
                        threads.push(spawn("wing-osc-events", move || osc_send_loop(&sh, tx)));
                    }
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
            cl.close();
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
            Err(e) if quiet_udp_error(&e) => {}
            Err(_) => std::thread::sleep(TICK),
        }
    }
}

/// Errors a UDP server just reads past: timeouts, and the "port unreachable"
/// a client that went away leaves behind (Windows reports it on the next
/// receive). Pausing on those would hold up every other client's replies.
fn quiet_udp_error(e: &std::io::Error) -> bool {
    matches!(
        e.kind(),
        ErrorKind::WouldBlock
            | ErrorKind::TimedOut
            | ErrorKind::Interrupted
            | ErrorKind::ConnectionReset
            | ErrorKind::ConnectionRefused
            | ErrorKind::ConnectionAborted
    )
}

fn osc_loop(sh: &Shared, sock: UdpSocket) {
    let _ = sock.set_read_timeout(Some(TICK));
    let mut buf = vec![0u8; 65536];
    while !sh.stop.load(Ordering::Relaxed) {
        match sock.recv_from(&mut buf) {
            Ok((n, from)) => sh.handle_osc(&sock, &buf[..n], from),
            Err(e) if quiet_udp_error(&e) => {}
            Err(_) => std::thread::sleep(Duration::from_millis(5)),
        }
    }
}

/// Sends the OSC events, paced (OSC_EVENTS_PER_MS).
fn osc_send_loop(sh: &Shared, sock: UdpSocket) {
    let mut batch = Vec::with_capacity(OSC_EVENTS_BURST);
    let mut last = Instant::now();
    while !sh.stop.load(Ordering::Relaxed) {
        let more = {
            let mut q = lock(&sh.osc_events);
            while q.is_empty() && !sh.stop.load(Ordering::Relaxed) {
                q = sh.osc_ready.wait_timeout(q, TICK).unwrap_or_else(|e| e.into_inner()).0;
                last = Instant::now();
            }
            let n = q.len().min(paced_batch(last.elapsed()));
            last = Instant::now();
            batch.extend(q.drain(..n));
            !q.is_empty()
        };
        for (addr, p) in batch.drain(..) {
            let _ = sock.send_to(&p, addr);
        }
        if more {
            std::thread::sleep(Duration::from_millis(1));
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
                let (Ok(writer), Ok(control)) = (stream.try_clone(), stream.try_clone()) else { continue };
                let _ = stream.set_nodelay(true);
                let _ = writer.set_write_timeout(Some(NATIVE_WRITE_STALL));
                let client = Arc::new(NativeClient {
                    id: sh.next_id.fetch_add(1, Ordering::Relaxed),
                    peer,
                    stream: control,
                    out: Mutex::default(),
                    ready: Condvar::new(),
                    meters: Mutex::new(MeterSub { port: None, report_id: 0, meters: Vec::new(), expires: Instant::now() }),
                    alive: AtomicBool::new(true),
                    since: Instant::now(),
                });
                lock(&sh.clients).push(client.clone());
                sh.ensure_baseline();
                let (sh2, cl2) = (sh.clone(), client.clone());
                readers.push(spawn("wing-native-write", move || writer_loop(&sh2, &cl2, writer)));
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

/// Sends a Native client what was queued for it.
fn writer_loop(sh: &Shared, client: &NativeClient, mut stream: TcpStream) {
    let mut buf = Vec::new();
    loop {
        {
            let mut out = lock(&client.out);
            while out.is_empty() && client.alive.load(Ordering::Relaxed) && !sh.stop.load(Ordering::Relaxed) {
                out = client.ready.wait_timeout(out, TICK).unwrap_or_else(|e| e.into_inner()).0;
            }
            if !client.alive.load(Ordering::Relaxed) || sh.stop.load(Ordering::Relaxed) {
                return;
            }
            std::mem::swap(&mut *out, &mut buf);
        }
        // write_all finishes partial sends; one that stalls for
        // NATIVE_WRITE_STALL means the client is gone.
        if stream.write_all(&buf).is_err() {
            client.close();
            return;
        }
        buf.clear();
        if buf.capacity() > 1 << 20 {
            buf = Vec::new();
        }
    }
}

fn client_loop(sh: &Shared, client: Arc<NativeClient>, mut stream: TcpStream) {
    let _ = stream.set_read_timeout(Some(TICK));
    let mut session = Session::new(Origin::Native(client.id));
    let mut buf = vec![0u8; 16384];
    let mut last = Instant::now();
    while !sh.stop.load(Ordering::Relaxed) && client.alive.load(Ordering::Relaxed) {
        if client.queued() > NATIVE_READ_PAUSE {
            // Let the client catch up before answering more; it isn't idle meanwhile.
            std::thread::sleep(Duration::from_millis(10));
            last = Instant::now();
            continue;
        }
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
    client.close();
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn osc_events_are_paced_by_the_time_that_passed() {
        assert_eq!(paced_batch(Duration::ZERO), OSC_EVENTS_PER_MS);
        assert_eq!(paced_batch(Duration::from_micros(1100)), OSC_EVENTS_PER_MS);
        assert_eq!(paced_batch(Duration::from_millis(3)), 3 * OSC_EVENTS_PER_MS);
        // A coarse timer (Windows: 15.6 ms) catches up, in bursts no bigger than this.
        assert_eq!(paced_batch(Duration::from_micros(15600)), OSC_EVENTS_BURST);
        assert_eq!(paced_batch(Duration::from_secs(1)), OSC_EVENTS_BURST);
    }

    /// A burst for several OSC subscribers (of different formats) is queued
    /// in turns, so the last subscriber doesn't wait for everyone else's.
    #[test]
    fn subscribers_take_turns_in_a_burst() {
        let engine: Arc<dyn EngineHandle> = Arc::new(Arc::new(Engine::new(0)));
        let sh = Shared::new(engine, Arc::new(WingState::default()));
        sh.osc_events_on.store(true, Ordering::Relaxed);
        let addrs: Vec<SocketAddr> = (0..3).map(|i| SocketAddr::from(([127, 0, 0, 1], 9000 + i))).collect();
        for (addr, format) in addrs.iter().zip([SubFormat::Triplet, SubFormat::Native, SubFormat::Triplet]) {
            sh.subscribe_osc(*addr, *addr, format);
        }
        let c = sh.console();
        let changes: Vec<(Node, Value)> = (1..=10)
            .map(|ch| {
                let n = schema().resolve(&format!("/ch/{ch}/fdr"), &c).unwrap();
                (n, c.get(n).unwrap())
            })
            .collect();
        sh.queue_osc_events(&c, &changes, Origin::None);
        let q = lock(&sh.osc_events);
        assert_eq!(q.len(), addrs.len() * changes.len());
        // Each round holds one event for each subscriber.
        let order: Vec<SocketAddr> = q.iter().map(|e| e.0).collect();
        for (k, round) in order.chunks(addrs.len()).enumerate() {
            assert!(addrs.iter().all(|a| round.contains(a)), "round {k}: {round:?}");
        }
    }
}
