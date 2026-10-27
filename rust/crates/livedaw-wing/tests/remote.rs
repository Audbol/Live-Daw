//! The remote servers over real sockets (ephemeral ports on localhost):
//! discovery, OSC get/set/nodes/subscriptions, the Native protocol (gets,
//! sets, definitions, change notifications, meters). Requests are encoded the
//! way libwing and the protocol document encode them.

use livedaw_engine::{Engine, StripKind};
use livedaw_session::custom::CustomControls;
use livedaw_session::layers::{FaderRef, LayerSet, LEFT};
use livedaw_session::ConsoleModel;
use livedaw_wing::map::{LibAction, Library};
use livedaw_wing::osc::{self, Arg, Message};
use livedaw_wing::server::{Config, EngineHandle, WingServer};
use livedaw_wing::{schema, surface, Identity, WingState};
use std::io::{Read, Write};
use std::net::{IpAddr, Ipv4Addr, SocketAddr, TcpStream, UdpSocket};
use std::sync::atomic::Ordering::Relaxed;
use std::sync::Arc;
use std::time::{Duration, Instant};

fn start() -> (Arc<Engine>, WingServer) {
    let engine = Arc::new(Engine::new(0));
    engine.set_num_channels(40);
    let config = Config {
        identity: Identity { name: "TEST-DESK".into(), ..Identity::default() },
        bind: IpAddr::V4(Ipv4Addr::LOCALHOST),
        discovery_port: 0,
        osc_port: 0,
        native_port: 0,
        ..Config::default()
    };
    let handle: Arc<dyn EngineHandle> = Arc::new(engine.clone());
    let server = WingServer::start(handle, Arc::new(WingState::default()), &config);
    assert!(server.status().errors.is_empty(), "{:?}", server.status().errors);
    (engine, server)
}

fn id(path: &str) -> u32 {
    let s = schema();
    s.id(s.resolve(path, &livedaw_wing::schema::NoModels).unwrap())
}

struct Osc {
    sock: UdpSocket,
    to: SocketAddr,
}

impl Osc {
    fn new(server: &WingServer) -> Self {
        let sock = UdpSocket::bind("127.0.0.1:0").unwrap();
        sock.set_read_timeout(Some(Duration::from_millis(500))).unwrap();
        Self { sock, to: server.status().osc.unwrap() }
    }
    fn send(&self, m: &Message) {
        self.sock.send_to(&osc::encode(m), self.to).unwrap();
    }
    fn recv(&self) -> Option<Message> {
        let mut buf = [0u8; 65536];
        let (n, _) = self.sock.recv_from(&mut buf).ok()?;
        osc::decode(&buf[..n])
    }
    fn get(&self, addr: &str) -> Message {
        self.send(&Message::new(addr, vec![]));
        self.recv().unwrap_or_else(|| panic!("no reply for {addr}"))
    }
}

#[test]
fn discovery_replies_like_a_wing() {
    let (_e, server) = start();
    let sock = UdpSocket::bind("127.0.0.1:0").unwrap();
    sock.set_read_timeout(Some(Duration::from_secs(10))).unwrap();
    sock.send_to(b"WING?", server.status().discovery.unwrap()).unwrap();
    let mut buf = [0u8; 512];
    let (n, _) = sock.recv_from(&mut buf).unwrap();
    let reply = std::str::from_utf8(&buf[..n]).unwrap();
    let f: Vec<&str> = reply.split(',').collect();
    assert_eq!(f.len(), 6, "{reply}");
    assert_eq!((f[0], f[1], f[2], f[3]), ("WING", "127.0.0.1", "TEST-DESK", "ngc-full"));
    assert!(f[5].starts_with("3.1"));
}

#[test]
fn a_compact_reports_its_model() {
    let engine: Arc<dyn EngineHandle> = Arc::new(Arc::new(Engine::new(0)));
    let config = Config {
        identity: Identity { name: "SMALL-DESK".into(), ..Identity::of_model(ConsoleModel::Compact) },
        bind: IpAddr::V4(Ipv4Addr::LOCALHOST),
        discovery_port: 0,
        osc_port: 0,
        native_port: 0,
        ..Config::default()
    };
    let state = Arc::new(WingState::default());
    let server = WingServer::start(engine, state.clone(), &config);
    assert!(server.status().errors.is_empty(), "{:?}", server.status().errors);
    let c = Osc::new(&server);
    let info = c.get("/?");
    assert!(matches!(&info.args[..], [Arg::Str(s)] if s.starts_with("WING,127.0.0.1,SMALL-DESK,wing-compact,")), "{info:?}");
    assert_eq!(c.get("/$syscfg/$cnsmdl").args, vec![Arg::Str("wing-compact".into())]);
    assert_eq!(c.get("/$syscfg/$cnscfg").args, vec![Arg::Str("wing-compact".into())]);
    // The COMPACT's surface settings are the host's (factory values until it publishes).
    assert_eq!(c.get("/$ctl/cfg/mfdr").args[0], Arg::Str("MAIN.1".into()));
    assert_eq!(c.get("/$ctl/cfg/cscmode").args[0], Arg::Str("BUS".into()));
    state.set_host_setting("ui.compactMainFader", "DCA.3");
    assert_eq!(c.get("/$ctl/cfg/mfdr").args[0], Arg::Str("DCA.3".into()));
    c.send(&Message::new("/$ctl/cfg/cscmode", vec![Arg::Str("USER".into())]));
    assert_eq!(c.get("/$ctl/cfg/cscmode").args[0], Arg::Str("USER".into()));
    assert_eq!(state.take_host_setting(), Some(("ui.compactKeyMode", "USER".to_owned())));
    c.send(&Message::new("/$ctl/cfg/autosel_CMPCT", vec![Arg::Int(1)]));
    assert_eq!(c.get("/$ctl/cfg/autosel_CMPCT").args[0], Arg::Str("1".into()));
    assert_eq!(state.take_host_setting(), Some(("ui.autoSelectCompact", "1".to_owned())));
    // Switching the model reaches clients at once.
    state.set_console_model(ConsoleModel::Full);
    assert!(matches!(&c.get("/?").args[..], [Arg::Str(s)] if s.starts_with("WING,127.0.0.1,SMALL-DESK,ngc-full,")));
}

#[test]
fn osc_get_set_nodes_and_errors() {
    let (engine, server) = start();
    let c = Osc::new(&server);
    // /? (protocol document p. 20)
    let info = c.get("/?");
    assert!(matches!(&info.args[..], [Arg::Str(s)] if s.starts_with("WING,127.0.0.1,TEST-DESK,")));
    // Set as float and as string; read back as ,sff (p. 22).
    c.send(&Message::new("/ch/2/fdr", vec![Arg::Float(-3.0)]));
    let r = c.get("/ch/2/fdr");
    assert_eq!(r.args, vec![Arg::Str("-3.0".into()), Arg::Float(0.675), Arg::Float(-3.0)]);
    c.send(&Message::new("/ch/2/fdr", vec![Arg::Str("-2".into())]));
    assert_eq!(c.get("/ch/2/fdr").args[2], Arg::Float(-2.0));
    assert_eq!(engine.channel_at(1).unwrap().params.fader_db.load(), -2.0);
    // Toggle with -1 (p. 22), ,sfi replies.
    c.send(&Message::new("/ch/1/mute", vec![Arg::Int(-1)]));
    assert_eq!(c.get("/ch/1/mute").args, vec![Arg::Str("1".into()), Arg::Float(1.0), Arg::Int(1)]);
    // Enums by name and by index (p. 22-23).
    c.send(&Message::new("/ch/1/mon", vec![Arg::Str("B".into())]));
    assert_eq!(c.get("/ch/1/mon").args, vec![Arg::Str("B".into())]);
    c.send(&Message::new("/ch/1/mon", vec![Arg::Int(2)]));
    assert_eq!(c.get("/ch/1/mon").args, vec![Arg::Str("A+B".into())]);
    // Node listing, data dump, node sets with acks (p. 23-25).
    let root = c.get("/");
    assert_eq!(root.args.len(), 17);
    let fx = c.get("/fx/1");
    assert_eq!(fx.args[..2], [Arg::Str("mdl".into()), Arg::Str("fxmix".into())]);
    c.send(&Message::new("/fx/1", vec![Arg::Str("*".into())]));
    assert_eq!(c.recv().unwrap().args, vec![Arg::Str("mdl=NONE,fxmix=100,".into())]);
    c.send(&Message::new("/ch/1", vec![Arg::Str("fdr=4,mute=1".into())]));
    assert_eq!(c.recv().unwrap(), Message::new("/ch/1*", vec![Arg::Str("OK".into())]));
    c.send(&Message::new("/", vec![Arg::Str("/ch.1.fdr=-1,mute=0,.2.fdr=0,mute=1".into())]));
    assert_eq!(c.recv().unwrap(), Message::new("/*", vec![Arg::Str("OK".into())]));
    assert_eq!(c.get("/ch/1/fdr").args[2], Arg::Float(-1.0));
    assert_eq!(c.get("/ch/2/mute").args[2], Arg::Int(1));
    c.send(&Message::new("/ch/1", vec![Arg::Str("nothere=1".into())]));
    assert_eq!(c.recv().unwrap().args, vec![Arg::Str("NODE NOT FOUND".into())]);
    // Model parameters follow the model (p. 26).
    c.send(&Message::new("/fx/1/mdl", vec![Arg::Str("pia".into())]));
    assert_eq!(c.get("/fx/1/mdl").args, vec![Arg::Str("PIA".into())]);
    c.send(&Message::new("/fx/1/125", vec![Arg::Float(10.0)]));
    assert_eq!(c.get("/fx/1/125").args[2], Arg::Float(10.0));
    // Hash addressing (p. 21) answers with the path.
    let mute = c.get("/#f50f69f8");
    assert_eq!(mute.addr, "/ch/1/mute");
    // Reply to another port (p. 21).
    let other = UdpSocket::bind("127.0.0.1:0").unwrap();
    other.set_read_timeout(Some(Duration::from_secs(1))).unwrap();
    let port = other.local_addr().unwrap().port();
    c.send(&Message::new(format!("/%{port}/ch/1/mute"), vec![]));
    let mut buf = [0u8; 256];
    let (n, _) = other.recv_from(&mut buf).unwrap();
    assert_eq!(osc::decode(&buf[..n]).unwrap().addr, "/ch/1/mute");
    // Native tokens in a blob: data request on a parameter (p. 29).
    c.send(&Message::new("/ch/1/mute", vec![Arg::Blob(vec![0xdc])]));
    let r = c.recv().unwrap();
    let mut want = vec![0xd7];
    want.extend_from_slice(&0xf50f69f8u32.to_be_bytes());
    want.extend_from_slice(&[0x00, 0xde]);
    assert_eq!(r.args, vec![Arg::Blob(want)]);
}

#[test]
fn osc_subscriptions_push_changes_from_anywhere() {
    let (engine, server) = start();
    let sub = Osc::new(&server);
    let other = Osc::new(&server);
    sub.send(&Message::new("/*S", vec![]));
    std::thread::sleep(Duration::from_millis(300)); // the poller takes its first snapshot
                                                    // A change from another client is pushed at once, the setter isn't echoed.
    other.send(&Message::new("/ch/5/fdr", vec![Arg::Float(-7.5)]));
    let ev = sub.recv().expect("event");
    assert_eq!(ev, Message::new("/ch/5/fdr", vec![Arg::Float(-7.5)]));
    assert!(other.recv().is_none());
    // A change made in the engine (the UI, a scene) is found by the poller.
    engine.channel_at(5).unwrap().params.mute.store(true, Relaxed);
    let deadline = Instant::now() + Duration::from_secs(2);
    let mut seen = false;
    while Instant::now() < deadline && !seen {
        if let Some(m) = sub.recv() {
            seen = m == Message::new("/ch/6/mute", vec![Arg::Int(1)]);
        }
    }
    assert!(seen, "engine change not pushed");
    // Triplet format.
    sub.send(&Message::new("/*s", vec![]));
    std::thread::sleep(Duration::from_millis(50));
    other.send(&Message::new("/ch/5/name", vec![Arg::Str("Vox".into())]));
    let deadline = Instant::now() + Duration::from_secs(2);
    let mut got = None;
    while Instant::now() < deadline && got.is_none() {
        got = sub.recv().filter(|m| m.addr == "/ch/5/name");
    }
    assert_eq!(got.unwrap().args, vec![Arg::Str("Vox".into())]);
}

#[test]
fn osc_scene_control_and_listen() {
    let engine = Arc::new(Engine::new(0));
    engine.set_num_channels(40);
    let state = Arc::new(WingState::default());
    let config =
        Config { bind: IpAddr::V4(Ipv4Addr::LOCALHOST), discovery_port: 0, osc_port: 0, native_port: 0, ..Config::default() };
    let handle: Arc<dyn EngineHandle> = Arc::new(engine.clone());
    let server = WingServer::start(handle, state.clone(), &config);
    let c = Osc::new(&server);
    // The host publishes its show; GO queues a recall it runs.
    state.set_library(Library { attached: true, show: "Gig".into(), num_scenes: 4, current: 0, scene: "Intro".into(), tag: 0 });
    assert_eq!(c.get("/$ctl/lib/$active").args, vec![Arg::Str("Intro".into())]);
    c.send(&Message::new("/$ctl/lib/$actionidx", vec![Arg::Int(2)]));
    c.send(&Message::new("/$ctl/lib/$action", vec![Arg::Str("GO".into())]));
    let deadline = Instant::now() + Duration::from_secs(2);
    let mut action = None;
    while Instant::now() < deadline && action.is_none() {
        action = state.take_lib_action();
        std::thread::sleep(Duration::from_millis(10));
    }
    assert_eq!(action, Some(LibAction::Go(2)));
    // The recall shows to subscribers.
    c.send(&Message::new("/*S", vec![]));
    std::thread::sleep(Duration::from_millis(300));
    state.set_library(Library { attached: true, show: "Gig".into(), num_scenes: 4, current: 2, scene: "Chorus".into(), tag: 0 });
    let deadline = Instant::now() + Duration::from_secs(2);
    let mut seen = false;
    while Instant::now() < deadline && !seen {
        seen = c.recv().is_some_and(|m| m == Message::new("/$ctl/lib/$active", vec![Arg::Str("Chorus".into())]));
    }
    assert!(seen, "scene change not pushed");
    // LISTEN: KEY SOLO of CH 4's DYNAMICS.
    c.send(&Message::new("/ch/4/dynsc/$solo", vec![Arg::Int(1)]));
    let a = engine.analysis().controls();
    let deadline = Instant::now() + Duration::from_secs(2);
    while Instant::now() < deadline && a.listen_mode.load(Relaxed) == 0 {
        std::thread::sleep(Duration::from_millis(10));
    }
    assert_eq!(a.listen_mode.load(Relaxed), livedaw_engine::analyser::listen_mode::DYN_KEY);
    assert_eq!(a.listen_id.load(Relaxed), engine.channel_at(3).unwrap().id());
}

#[test]
fn osc_surface_layers_and_custom_controls() {
    let engine = Arc::new(Engine::new(0));
    engine.set_num_channels(40);
    let state = Arc::new(WingState::default());
    let config =
        Config { bind: IpAddr::V4(Ipv4Addr::LOCALHOST), discovery_port: 0, osc_port: 0, native_port: 0, ..Config::default() };
    let handle: Arc<dyn EngineHandle> = Arc::new(engine.clone());
    let server = WingServer::start(handle, state.clone(), &config);
    let c = Osc::new(&server);
    // The host's show, published as its message thread would.
    let (mut layers, mut custom) = (LayerSet::default(), CustomControls::default());
    let sync = |layers: &mut LayerSet, custom: &mut CustomControls| surface::sync(&state, &engine, layers, custom, 0);
    sync(&mut layers, &mut custom);
    // "1" of 1..22: the console's ,sfi reply counts from the bottom of the range.
    assert_eq!(c.get("/$ctl/layer/L/sel").args, vec![Arg::Str("1".into()), Arg::Float(0.0), Arg::Int(0)]);

    // A client picks the left section's 25-36 key and assigns USER 1's first fader.
    c.send(&Message::new("/$ctl/layer/L/sel", vec![Arg::Int(3)]));
    c.send(&Message::new("/$ctl/layer/L/6/1/type", vec![Arg::Str("DCA".into())]));
    c.send(&Message::new("/$ctl/layer/L/6/1/i", vec![Arg::Int(4)]));
    c.send(&Message::new("/$ctl/user/2/3/enc/mode", vec![Arg::Str("PAN".into())]));
    // The reply to a get shows the change before the host has applied it.
    assert_eq!(c.get("/$ctl/layer/L/6/1/i").args.last(), Some(&Arg::Int(4)));
    let deadline = Instant::now() + Duration::from_secs(2);
    while Instant::now() < deadline && layers.user_layers().first().map(|l| l.faders.clone()) != Some(vec![FaderRef::Dca(3)]) {
        sync(&mut layers, &mut custom);
        std::thread::sleep(Duration::from_millis(10));
    }
    assert_eq!(layers.selected(LEFT), "in-3");
    assert_eq!(layers.user_layers()[0].faders, vec![FaderRef::Dca(3)]);
    assert_eq!(custom.control(1, 2).unwrap().encoder.category, 2);

    // Changes made on the console reach subscribers.
    c.send(&Message::new("/*S", vec![]));
    std::thread::sleep(Duration::from_millis(300));
    let id = layers.user_layers()[0].id.clone();
    layers.rename_user_layer(&id, "Drums");
    custom.control_mut(0, 0).unwrap().name = "Lead".into();
    sync(&mut layers, &mut custom);
    let want = [
        Message::new("/$ctl/layer/L/6/name", vec![Arg::Str("Drums".into())]),
        Message::new("/$ctl/user/1/1/enc/name", vec![Arg::Str("Lead".into())]),
    ];
    let mut seen = [false; 2];
    let deadline = Instant::now() + Duration::from_secs(2);
    while Instant::now() < deadline && !seen.iter().all(|s| *s) {
        if let Some(m) = c.recv() {
            for (s, w) in seen.iter_mut().zip(&want) {
                *s |= m == *w;
            }
        }
    }
    assert_eq!(seen, [true; 2], "console changes not pushed");
}

// ---- Native ------------------------------------------------------------------------

struct Native {
    tcp: TcpStream,
    rx: Vec<u8>,
}

/// Decoded server tokens: (id, value) pairs and request ends.
#[derive(Debug, PartialEq)]
enum Tok {
    Data(u32, livedaw_wing::Value),
    Def(Vec<u8>),
    End,
}

impl Native {
    fn connect(server: &WingServer) -> Self {
        let mut tcp = TcpStream::connect(server.status().native.unwrap()).unwrap();
        tcp.set_read_timeout(Some(Duration::from_millis(300))).unwrap();
        tcp.write_all(&[0xdf, 0xd1]).unwrap();
        Self { tcp, rx: Vec::new() }
    }

    fn send(&mut self, payload: &[u8]) {
        let mut wire = Vec::new();
        for &b in payload {
            wire.push(b);
            if b == 0xdf {
                wire.push(0xde);
            }
        }
        self.tcp.write_all(&wire).unwrap();
    }

    fn select(id: u32) -> Vec<u8> {
        let mut v = vec![0xd7];
        v.extend_from_slice(&id.to_be_bytes());
        v
    }

    /// Reads tokens until `n` request ends (or a timeout).
    fn read_until_ends(&mut self, n: usize) -> Vec<Tok> {
        // Generous: under ThreadSanitizer on a shared CI runner a reply can take seconds.
        let deadline = Instant::now() + Duration::from_secs(20);
        loop {
            let toks = decode(&self.rx);
            if toks.iter().filter(|t| **t == Tok::End).count() >= n || Instant::now() > deadline {
                self.rx.clear();
                return toks;
            }
            let mut buf = [0u8; 65536];
            if let Ok(k) = self.tcp.read(&mut buf) {
                if k == 0 {
                    return toks;
                }
                self.rx.extend_from_slice(&buf[..k]);
            }
        }
    }

    /// Reads until `want` arrives (or 20 s pass, as for `read_until_ends`).
    fn read_until_has(&mut self, want: &Tok) -> Vec<Tok> {
        let deadline = Instant::now() + Duration::from_secs(20);
        loop {
            let toks = decode(&self.rx);
            if toks.contains(want) || Instant::now() > deadline {
                self.rx.clear();
                return toks;
            }
            let mut buf = [0u8; 65536];
            if let Ok(k) = self.tcp.read(&mut buf) {
                if k == 0 {
                    return toks;
                }
                self.rx.extend_from_slice(&buf[..k]);
            }
        }
    }

    fn read_for(&mut self, d: Duration) -> Vec<Tok> {
        let deadline = Instant::now() + d;
        while Instant::now() < deadline {
            let mut buf = [0u8; 65536];
            if let Ok(k) = self.tcp.read(&mut buf) {
                self.rx.extend_from_slice(&buf[..k]);
            }
        }
        let t = decode(&self.rx);
        self.rx.clear();
        t
    }
}

/// De-frames (channel 1 only) and decodes server tokens.
fn decode(wire: &[u8]) -> Vec<Tok> {
    let mut data = Vec::new();
    let mut esc = false;
    let mut ch = 1;
    for &b in wire {
        if esc {
            esc = false;
            match b {
                0xde | 0xdf => {
                    if ch == 1 {
                        data.push(0xdf)
                    }
                }
                0xd0..=0xdd => ch = b - 0xd0,
                other => {
                    if ch == 1 {
                        data.extend_from_slice(&[0xdf, other])
                    }
                }
            }
        } else if b == 0xdf {
            esc = true;
        } else if ch == 1 {
            data.push(b);
        }
    }
    let mut out = Vec::new();
    let mut i = 0;
    let mut cur = 0u32;
    let be32 = |d: &[u8], i: usize| u32::from_be_bytes([d[i], d[i + 1], d[i + 2], d[i + 3]]);
    use livedaw_wing::Value;
    while i < data.len() {
        let op = data[i];
        i += 1;
        match op {
            0x00..=0x3f => out.push(Tok::Data(cur, Value::Int(op as i32))),
            0x80..=0xbf => {
                let n = (op - 0x7f) as usize;
                out.push(Tok::Data(cur, Value::Str(String::from_utf8_lossy(&data[i..i + n]).into())));
                i += n;
            }
            0xd0 => out.push(Tok::Data(cur, Value::str(""))),
            0xd1 => {
                let n = data[i] as usize + 1;
                out.push(Tok::Data(cur, Value::Str(String::from_utf8_lossy(&data[i + 1..i + 1 + n]).into())));
                i += 1 + n;
            }
            0xd3 => {
                out.push(Tok::Data(cur, Value::Int(i16::from_be_bytes([data[i], data[i + 1]]) as i32)));
                i += 2;
            }
            0xd4 => {
                out.push(Tok::Data(cur, Value::Int(be32(&data, i) as i32)));
                i += 4;
            }
            0xd5 | 0xd6 => {
                out.push(Tok::Data(cur, Value::Float(f32::from_bits(be32(&data, i)))));
                i += 4;
            }
            0xd7 => {
                cur = be32(&data, i);
                i += 4;
            }
            0xde => out.push(Tok::End),
            0xdf => {
                let mut n = u16::from_be_bytes([data[i], data[i + 1]]) as usize;
                i += 2;
                if n == 0 {
                    n = be32(&data, i) as usize;
                    i += 4;
                }
                out.push(Tok::Def(data[i..i + n].to_vec()));
                i += n;
            }
            _ => {}
        }
    }
    out
}

/// Without the `$` mirrors (/ch/1/$fdr...) the poller pushes after a set.
fn plain(toks: Vec<Tok>) -> Vec<Tok> {
    let s = schema();
    toks.into_iter()
        .filter(|t| match t {
            Tok::Data(id, _) => !s
                .by_id(*id, &livedaw_wing::schema::NoModels)
                .is_some_and(|n| s.path(n, &livedaw_wing::schema::NoModels).contains("/$")),
            _ => true,
        })
        .collect()
}

#[test]
fn native_get_set_definitions_and_notifications() {
    let (engine, server) = start();
    let mut a = Native::connect(&server);
    let mut b = Native::connect(&server);
    let fdr = id("/ch/1/fdr");
    // Both sessions are up once each has answered a get (a fixed pause was too
    // short under ThreadSanitizer on CI: B missed A's set and only saw the
    // poller's $fdr mirror).
    for c in [&mut a, &mut b] {
        let mut get = Native::select(fdr);
        get.push(0xdc);
        c.send(&get);
        c.read_until_ends(1);
    }
    // Set (float token), then get: d7 <id> <value> de.
    let mut req = Native::select(fdr);
    req.push(0xd5);
    req.extend_from_slice(&(-6.5f32).to_be_bytes());
    a.send(&req);
    let mut get = Native::select(fdr);
    get.push(0xdc);
    a.send(&get);
    let toks = a.read_until_ends(1);
    assert_eq!(plain(toks), vec![Tok::Data(fdr, livedaw_wing::Value::Float(-6.5)), Tok::End]);
    assert_eq!(engine.channel_at(0).unwrap().params.fader_db.load(), -6.5);
    // The other client was told.
    let told = Tok::Data(fdr, livedaw_wing::Value::Float(-6.5));
    let toks = b.read_until_has(&told);
    assert!(toks.contains(&told), "{toks:?}");
    // Strings and enums.
    let name = id("/ch/1/name");
    let mut req = Native::select(name);
    req.push(0x7f + 4);
    req.extend_from_slice(b"Kick");
    req.push(0xdc);
    a.send(&req);
    assert!(a.read_until_ends(1).contains(&Tok::Data(name, livedaw_wing::Value::str("Kick"))));
    // Definition of a leaf (fader: -144..10, 1024 steps).
    let mut req = Native::select(fdr);
    req.push(0xdd);
    a.send(&req);
    let toks = a.read_until_ends(1);
    let Some(Tok::Def(def)) = toks.iter().find(|t| matches!(t, Tok::Def(_))) else { panic!("{toks:?}") };
    assert_eq!(u32::from_be_bytes(def[4..8].try_into().unwrap()), fdr);
    assert_eq!(&def[def.len() - 12..def.len() - 8], &(-144.0f32).to_be_bytes());
    // Definitions of a node's children: /ch/1/gate has on, mdl, mix, gain + the model's.
    let mut req = Native::select(id("/ch/1/gate"));
    req.push(0xdd);
    a.send(&req);
    let toks = a.read_until_ends(1);
    let defs = toks.iter().filter(|t| matches!(t, Tok::Def(_))).count();
    assert!(defs > 8, "{defs}");
    // Root children (parent id 0).
    a.send(&[0xda, 0xdd]);
    let toks = a.read_until_ends(1);
    assert_eq!(toks.iter().filter(|t| matches!(t, Tok::Def(_))).count(), 17);
    // Navigation by name and index: ch / index 3 / name "mute" -> data.
    let mut req = vec![0xda, 0xc0 + 1];
    req.extend_from_slice(b"ch");
    req.push(0x40 + 2); // child 3
    req.push(0xc0 + 3);
    req.extend_from_slice(b"mute");
    req.push(0x01); // set 1
    req.push(0xdc);
    a.send(&req);
    let toks = a.read_until_ends(1);
    assert!(toks.contains(&Tok::Data(id("/ch/3/mute"), livedaw_wing::Value::Int(1))), "{toks:?}");
    assert!(engine.channel_at(2).unwrap().params.mute.load(Relaxed));
    // Toggle (d8).
    let mut req = Native::select(id("/ch/3/mute"));
    req.extend_from_slice(&[0xd8, 0xdc]);
    a.send(&req);
    assert!(a.read_until_ends(1).contains(&Tok::Data(id("/ch/3/mute"), livedaw_wing::Value::Int(0))));
    // A node's data: every leaf under it, then de.
    let mut req = Native::select(id("/ch/1/main"));
    req.push(0xdc);
    a.send(&req);
    let toks = a.read_until_ends(1);
    assert_eq!(plain(toks).iter().filter(|t| matches!(t, Tok::Data(..))).count(), 12);
    // An unknown id: just the end.
    let mut req = Native::select(0x1234_5678);
    req.push(0xdc);
    a.send(&req);
    assert_eq!(plain(a.read_until_ends(1)), vec![Tok::End]);
    // Engine changes reach the clients through the poller.
    b.read_for(Duration::from_millis(100));
    engine.strip_by_number(StripKind::Bus, 3).unwrap().params.fader_db.store(-20.0);
    let pushed = Tok::Data(id("/bus/3/fdr"), livedaw_wing::Value::Float(-20.0));
    let toks = b.read_until_has(&pushed);
    assert!(toks.contains(&pushed), "{toks:?}");
}

#[test]
fn native_meters_over_udp() {
    let (engine, server) = start();
    let mut a = Native::connect(&server);
    engine.channel_at(0).unwrap().meters.input_peak[0].store(0.5);
    let udp = UdpSocket::bind("127.0.0.1:0").unwrap();
    udp.set_read_timeout(Some(Duration::from_secs(10))).unwrap();
    let port = udp.local_addr().unwrap().port();
    // Data bytes equal to the channel marker 0xdf are escaped (0xdf 0xde), as
    // libwing does: an ephemeral port such as 0xdf.. would otherwise read as a
    // channel switch and the subscription would be lost.
    let esc =
        |bytes: [u8; 2]| bytes.iter().flat_map(|&b| if b == 0xdf { vec![0xdf, 0xde] } else { vec![b] }).collect::<Vec<u8>>();
    // As libwing sends it: channel 3, port, report id (id 7 + port), collection, back to channel 1.
    let mut req = vec![0xdf, 0xd3, 0xd3];
    req.extend(esc(port.to_be_bytes()));
    req.push(0xd4);
    req.extend(esc(7u16.to_be_bytes()));
    req.extend(esc(port.to_be_bytes()));
    req.extend_from_slice(&[0xdc, 0xa0, 0x00, 0xa3, 0x00, 0xaa, 0xde, 0xdf, 0xd1]);
    a.tcp.write_all(&req).unwrap();
    let mut buf = [0u8; 8192];
    let (n, from) = udp.recv_from(&mut buf).unwrap();
    assert_eq!(from.ip(), IpAddr::V4(Ipv4Addr::LOCALHOST));
    assert_eq!(u16::from_be_bytes([buf[0], buf[1]]), 7);
    // channel (8) + main (8) + RTA (120) words.
    assert_eq!((n - 4) / 2, 8 + 8 + 120);
    let input_l = i16::from_be_bytes([buf[4], buf[5]]);
    assert!((input_l as f32 / 256.0 + 6.02).abs() < 0.1, "{input_l}");
    // Renewal keeps it going.
    let mut renew = vec![0xdf, 0xd3, 0xd4];
    renew.extend(esc(7u16.to_be_bytes()));
    renew.extend(esc(port.to_be_bytes()));
    renew.extend_from_slice(&[0xdf, 0xd1]);
    a.tcp.write_all(&renew).unwrap();
    assert!(udp.recv_from(&mut buf).is_ok());
}

/// The parameters mapped since the first coverage report, set and read
/// through both protocols: the engine, the host's settings or the stored
/// value change.
#[test]
fn osc_and_native_reach_sources_outputs_safes_and_settings() {
    use livedaw_engine::recorder::usb::RecTap;
    use livedaw_engine::sources::USB_PLAYER_SOURCE_INDEX;
    let engine = Arc::new(Engine::new(0));
    engine.set_num_channels(40);
    let state = Arc::new(WingState::default());
    let config =
        Config { bind: IpAddr::V4(Ipv4Addr::LOCALHOST), discovery_port: 0, osc_port: 0, native_port: 0, ..Config::default() };
    let handle: Arc<dyn EngineHandle> = Arc::new(engine.clone());
    let server = WingServer::start(handle, state.clone(), &config);
    let c = Osc::new(&server);
    let text = |m: Message| match m.args.first() {
        Some(Arg::Str(s)) => s.clone(),
        other => panic!("{other:?}"),
    };

    // OSC: LOCAL IN is A (Sidecar's ports 1-48).
    c.send(&Message::new("/io/in/LCL/4/name", vec![Arg::Str("Gtr".into())]));
    assert_eq!(text(c.get("/io/in/A/4/name")), "Gtr");
    assert_eq!(engine.sources().name(3), "Gtr");
    // The USB recorder's RECORDER ports.
    c.send(&Message::new("/io/out/REC/1/grp", vec![Arg::Str("BUS".into())]));
    c.send(&Message::new("/io/out/REC/1/in", vec![Arg::Int(3)]));
    assert_eq!(text(c.get("/io/out/REC/1/grp")), "BUS");
    assert_eq!(engine.usb().tap(0), RecTap::Strip { kind: StripKind::Bus as i32, number: 2, channel: 0 });
    // Safes in the console's format.
    c.send(&Message::new("/$ctl/safes/main", vec![Arg::Str(" +".into())]));
    assert_eq!(text(c.get("/$ctl/safes/main")), " +  ");
    assert!(engine.strip_by_number(StripKind::Main, 2).unwrap().params.recall_safe.load(Relaxed));
    // A host setting: METERS > bus meters PRE.
    c.send(&Message::new("/cfg/mtr/mtrpage/bus", vec![Arg::Str("PRE".into())]));
    assert_eq!(text(c.get("/cfg/mtr/mtrpage/bus")), "PRE");
    assert_eq!(state.take_host_setting(), Some(("setup.meterTapPre.1", "1".to_owned())));
    // A stored factory value, kept as set.
    assert_eq!(text(c.get("/cards/wmadi/mode")), "SFP1");
    c.send(&Message::new("/cards/wmadi/mode", vec![Arg::Str("BNC".into())]));
    assert_eq!(text(c.get("/cards/wmadi/mode")), "BNC");

    // Native: the USB player's source name, a DAW setting, a stored value.
    let mut a = Native::connect(&server);
    std::thread::sleep(Duration::from_millis(150));
    let name = id("/io/in/PLAY/1/name");
    let mut req = Native::select(name);
    req.push(0x7f + 3);
    req.extend_from_slice(b"BGM");
    req.push(0xdc);
    a.send(&req);
    assert!(a.read_until_ends(1).contains(&Tok::Data(name, livedaw_wing::Value::str("BGM"))));
    assert_eq!(engine.sources().name(USB_PLAYER_SOURCE_INDEX), "BGM");
    let conn = id("/$ctl/daw/conn");
    let mut req = Native::select(conn);
    req.push(0x00); // DIN
    req.push(0xdc);
    a.send(&req);
    assert!(a.read_until_ends(1).contains(&Tok::Data(conn, livedaw_wing::Value::str("DIN"))));
    assert_eq!(state.take_host_setting(), Some(("daw.conn", "0".to_owned())));
    let vol = id("/$stat/usbvolname");
    let mut req = Native::select(vol);
    req.push(0xdc);
    a.send(&req);
    assert!(a.read_until_ends(1).contains(&Tok::Data(vol, livedaw_wing::Value::str("NO DRIVE"))));
}
