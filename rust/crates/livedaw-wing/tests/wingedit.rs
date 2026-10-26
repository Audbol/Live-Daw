//! What Behringer's WING Edit (3.3.4, traffic recorded against Sidecar) does
//! on the Native protocol, replayed: MIXER -> PC (`ROOT DATA?`), PC -> MIXER
//! (every value written back in tree order, model before its parameters),
//! the model request after connecting, live model switches, FX and channel
//! processing models, inserts and plugin (EXT) slots.

use livedaw_engine::fx::InsertCallbacks;
use livedaw_engine::params::FX_EXT;
use livedaw_engine::{Engine, StripKind};
use livedaw_session::ConsoleModel;
use livedaw_wing::schema::NodeType;
use livedaw_wing::server::{Config, EngineHandle, WingServer};
use livedaw_wing::{schema, Console, Identity, Node, Value, WingState};
use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::{IpAddr, Ipv4Addr, TcpStream};
use std::sync::atomic::Ordering::Relaxed;
use std::sync::Arc;
use std::time::{Duration, Instant};

fn engine() -> Arc<Engine> {
    let e = Arc::new(Engine::new(0));
    e.set_num_channels(40);
    e
}

fn start(model: ConsoleModel) -> (Arc<Engine>, Arc<WingState>, WingServer) {
    let e = engine();
    let config = Config {
        identity: Identity { name: "EDIT-TEST".into(), ..Identity::of_model(model) },
        bind: IpAddr::V4(Ipv4Addr::LOCALHOST),
        discovery_port: 0,
        osc_port: 0,
        native_port: 0,
        ..Config::default()
    };
    let state = Arc::new(WingState::default());
    let handle: Arc<dyn EngineHandle> = Arc::new(e.clone());
    let server = WingServer::start(handle, state.clone(), &config);
    assert!(server.status().errors.is_empty(), "{:?}", server.status().errors);
    (e, state, server)
}

fn id(path: &str) -> u32 {
    let s = schema();
    s.id(s.resolve(path, &livedaw_wing::schema::NoModels).unwrap_or_else(|| panic!("{path}")))
}

/// A Native client speaking as WING Edit does.
struct Client {
    tcp: TcpStream,
    de: livedaw_wing::native::Deframer,
    data: Vec<u8>,
}

/// Generous deadlines: CI runs these under ThreadSanitizer.
const PATIENCE: Duration = Duration::from_secs(30);

impl Client {
    fn connect(server: &WingServer) -> Self {
        let tcp = TcpStream::connect(server.status().native.unwrap()).unwrap();
        tcp.set_read_timeout(Some(Duration::from_millis(100))).unwrap();
        let mut c = Self { tcp, de: Default::default(), data: Vec::new() };
        c.send(&[]);
        c
    }

    /// Sends Audio Engine tokens (framed on channel 2).
    fn send(&mut self, tokens: &[u8]) {
        self.tcp.write_all(&livedaw_wing::native::frame(livedaw_wing::native::CH_AUDIO, tokens)).unwrap();
    }

    fn set(&mut self, id: u32, v: &Value) {
        let mut t = Vec::new();
        livedaw_wing::native::encode_data(&mut t, id, v);
        self.send(&t);
    }

    fn pump(&mut self) {
        let mut buf = [0u8; 65536];
        if let Ok(n) = self.tcp.read(&mut buf) {
            let mut ev = Vec::new();
            for &b in &buf[..n] {
                self.de.push(b, &mut ev);
            }
            for e in ev {
                if let livedaw_wing::native::Event::Data(livedaw_wing::native::CH_AUDIO, b) = e {
                    self.data.push(b);
                }
            }
        }
    }

    /// Reads (id, value) pairs until `done` says so (or the deadline passes).
    fn read_until(&mut self, mut done: impl FnMut(&[(u32, Value)], usize) -> bool) -> (Vec<(u32, Value)>, usize) {
        let deadline = Instant::now() + PATIENCE;
        loop {
            let (vals, ends) = decode(&self.data);
            if done(&vals, ends) || Instant::now() > deadline {
                self.data.clear();
                return (vals, ends);
            }
            self.pump();
        }
    }

    /// `ROOT DATA?`: the whole console, as WING Edit's MIXER -> PC asks for it.
    fn dump(&mut self) -> Vec<(u32, Value)> {
        self.read_for(Duration::from_millis(300));
        self.send(&[0xda, 0xdc]);
        self.read_until(|_, ends| ends >= 1).0
    }

    fn read_for(&mut self, d: Duration) -> Vec<(u32, Value)> {
        let until = Instant::now() + d;
        while Instant::now() < until {
            self.pump();
        }
        let (vals, _) = decode(&self.data);
        self.data.clear();
        vals
    }
}

/// Decodes the server's value tokens: (id, value) pairs and the number of ends.
fn decode(d: &[u8]) -> (Vec<(u32, Value)>, usize) {
    let (mut out, mut ends, mut cur, mut i) = (Vec::new(), 0, 0u32, 0);
    let be32 = |i: usize| u32::from_be_bytes([d[i], d[i + 1], d[i + 2], d[i + 3]]);
    while i < d.len() {
        let op = d[i];
        let need = match op {
            0x80..=0xbf => 1 + (op - 0x7f) as usize,
            0xd1 if i + 1 < d.len() => 3 + d[i + 1] as usize,
            0xd1 => usize::MAX,
            0xd3 => 3,
            0xd4..=0xd7 => 5,
            0xdf if i + 2 < d.len() => 3 + u16::from_be_bytes([d[i + 1], d[i + 2]]) as usize,
            0xdf => usize::MAX,
            _ => 1,
        };
        if need == usize::MAX || i + need > d.len() {
            break;
        }
        let s = |a: usize, b: usize| Value::Str(String::from_utf8_lossy(&d[a..b]).into_owned());
        match op {
            0x00..=0x3f => out.push((cur, Value::Int(op as i32))),
            0x80..=0xbf => out.push((cur, s(i + 1, i + need))),
            0xd0 => out.push((cur, Value::str(""))),
            0xd1 => out.push((cur, s(i + 2, i + need))),
            0xd3 => out.push((cur, Value::Int(i16::from_be_bytes([d[i + 1], d[i + 2]]) as i32))),
            0xd4 => out.push((cur, Value::Int(be32(i + 1) as i32))),
            0xd5 => out.push((cur, Value::Float(f32::from_bits(be32(i + 1))))),
            0xd7 => cur = be32(i + 1),
            0xde => ends += 1,
            _ => {}
        }
        i += need;
    }
    (out, ends)
}

fn close(a: &Value, b: &Value) -> bool {
    match (a, b) {
        (Value::Float(x), Value::Float(y)) => (x - y).abs() <= x.abs().max(1.0) * 1e-4,
        _ => a == b,
    }
}

fn path_of(id: u32, c: &Console) -> String {
    let s = schema();
    s.by_id(id, c).map_or_else(|| format!("#{id:08x}"), |n| s.path(n, c))
}

/// WING Edit's PC -> MIXER: every writable value of `dump`, in its order.
fn write_back(client: &mut Client, dump: &[(u32, Value)], c: &Console) {
    let s = schema();
    let mut tokens = Vec::new();
    for (id, v) in dump {
        // WING Edit skips what it can't change (the console's read-only nodes).
        if s.by_id(*id, c).and_then(|n| c.def(n)).is_some_and(|d| d.read_only) {
            continue;
        }
        livedaw_wing::native::encode_data(&mut tokens, *id, v);
    }
    client.send(&tokens);
    // A get after the last set returns once every set before it was applied.
    client.send(&[0xd7]);
    client.send(&id("/ch/1/fdr").to_be_bytes());
    client.send(&[0xdc]);
    client.read_until(|_, ends| ends >= 1);
}

/// Values that move on their own or follow the client's view, not the show.
fn volatile(path: &str) -> bool {
    path == "/cfg/solo/$srcsgrp" || path.contains("/$stat/") || path.starts_with("/$stat") || path.contains("time")
}

fn diffs(before: &[(u32, Value)], after: &[(u32, Value)], c: &Console) -> Vec<String> {
    let after: HashMap<u32, &Value> = after.iter().map(|(i, v)| (*i, v)).collect();
    before
        .iter()
        .filter_map(|(i, v)| {
            let p = path_of(*i, c);
            match after.get(i) {
                Some(w) if close(v, w) => None,
                _ if volatile(&p) => None,
                w => Some(format!("{p}: {v:?} -> {w:?}")),
            }
        })
        .collect()
}

/// MIXER -> PC then PC -> MIXER of the same values changes nothing, on either
/// console model. (Sidecar's LOCAL OUT n and AES50 A n are one port: an
/// output nobody patched answered the factory WING's patch, LCL 3 BUS 5 but
/// A 3 OFF, and the write-back of A 3 unpatched LCL 3.)
#[test]
fn a_synchronization_round_trip_changes_nothing() {
    for model in [ConsoleModel::Full, ConsoleModel::Compact] {
        let (e, st, server) = start(model);
        let c = Console::new(&e, &st);
        let mut edit = Client::connect(&server);
        let before = edit.dump();
        assert!(before.len() > 25_000, "{}", before.len());
        let get = |p: &str| before.iter().find(|(i, _)| *i == id(p)).map(|(_, v)| v.clone());
        for n in 1..=8 {
            assert_eq!(get(&format!("/io/out/LCL/{n}/grp")), get(&format!("/io/out/A/{n}/grp")), "LCL {n} / A {n}");
        }
        write_back(&mut edit, &before, &c);
        let after = edit.dump();
        let d = diffs(&before, &after, &c);
        assert!(d.is_empty(), "{model:?}: {} differ:\n{}", d.len(), d[..d.len().min(40)].join("\n"));
    }
}

/// Moves every settable number under `prefix` elsewhere in its range.
fn scramble(c: &Console, prefix: &str, k: f32) {
    let s = schema();
    let mut leaves = Vec::new();
    c.leaves(s.resolve(prefix, c).unwrap(), &mut leaves);
    for n in leaves {
        let p = s.path(n, c);
        let Some(d) = c.def(n) else { continue };
        if d.read_only || p.contains("/$") || p.ends_with("/mdl") {
            continue;
        }
        let v = match d.ty {
            NodeType::LinFloat | NodeType::LogFloat | NodeType::Fader => Value::Float(d.min + (d.max - d.min) * k),
            NodeType::Int => Value::Int(d.min_int + ((d.max_int - d.min_int) as f32 * k) as i32),
            _ => continue,
        };
        let _ = c.set(n, &v);
    }
}

/// PC -> MIXER from a WING Edit that holds another show: FX models in premium
/// and standard slots, channel GATE / EQ / DYNAMICS models, inserts, and the
/// new models' parameters (addressed by position, so they only land if the
/// model was switched first) all arrive.
#[test]
fn pc_to_mixer_carries_models_parameters_and_inserts() {
    // The show WING Edit holds, made on another console.
    let (ea, sa) = (engine(), WingState::default());
    let a = Console::new(&ea, &sa);
    let set = |c: &Console, p: &str, v: Value| c.set(schema().resolve(p, c).unwrap(), &v).unwrap();
    for (p, m) in [
        ("/fx/1/mdl", "HALL"),
        ("/fx/2/mdl", "ST-DL"),
        ("/fx/9/mdl", "GEQ"),
        ("/fx/10/mdl", "*EVEN*"),
        ("/ch/1/gate/mdl", "DUCK"),
        ("/ch/1/eq/mdl", "SOUL"),
        ("/ch/1/dyn/mdl", "LA"),
        ("/ch/2/dyn/mdl", "9000C"),
        ("/bus/1/dyn/mdl", "SBUS"),
        ("/ch/1/preins/ins", "FX9"),
        ("/ch/1/postins/ins", "FX2"),
        ("/bus/1/preins/ins", "FX1"),
    ] {
        set(&a, p, Value::str(m));
    }
    set(&a, "/ch/1/preins/on", Value::Int(1));
    set(&a, "/fx/1/fxmix", Value::Float(40.0));
    ea.collect();
    for p in ["/fx/1", "/fx/2", "/fx/9", "/fx/10", "/ch/1/gate", "/ch/1/eq", "/ch/1/dyn", "/ch/2/dyn", "/bus/1/dyn"] {
        scramble(&a, p, 0.37);
    }
    let s = schema();
    let mut leaves = Vec::new();
    a.leaves(Node::Root, &mut leaves);
    let show: Vec<(u32, Value)> = leaves.iter().filter_map(|&n| Some((s.id(n), a.get(n)?))).collect();

    let (e, st, server) = start(ConsoleModel::Full);
    let c = Console::new(&e, &st);
    let mut edit = Client::connect(&server);
    write_back(&mut edit, &show, &a);
    let after = edit.dump();
    let d = diffs(&show, &after, &a);
    assert!(d.is_empty(), "{} differ:\n{}", d.len(), d[..d.len().min(40)].join("\n"));
    // The engine runs it: the insert is routed to the slot.
    let ch1 = e.channel_at(0).unwrap();
    assert!(ch1.params.pre_insert.on.load(Relaxed));
    assert_eq!(ch1.params.pre_insert.fx.load(Relaxed), 8);
    assert_eq!(c.get(s.resolve("/fx/9/$a_chn", &c).unwrap()), Some(Value::Int(1)));
}

/// WING Edit sets `/$syscfg/$cnsmdl` to "ModelRequest" after connecting; the
/// read-only node answers the model, as any refused set answers the value
/// that stands.
#[test]
fn the_model_request_is_answered() {
    for (model, name) in [(ConsoleModel::Full, "ngc-full"), (ConsoleModel::Compact, "wing-compact")] {
        let (_e, _st, server) = start(model);
        let mut edit = Client::connect(&server);
        let cnsmdl = id("/$syscfg/$cnsmdl");
        edit.set(cnsmdl, &Value::str("ModelRequest"));
        let want = (cnsmdl, Value::str(name));
        let (vals, _) = edit.read_until(|v, _| v.contains(&want));
        assert!(vals.contains(&want), "{vals:?}");
    }
}

/// Switching CONSOLE MODEL reaches a connected WING Edit at once (it follows
/// `$cnscfg`, its ACTIVE WING MODEL).
#[test]
fn a_model_switch_reaches_connected_clients() {
    let (_e, st, server) = start(ConsoleModel::Full);
    let mut edit = Client::connect(&server);
    edit.read_for(Duration::from_millis(300));
    st.set_console_model(ConsoleModel::Compact);
    let want = (id("/$syscfg/$cnscfg"), Value::str("wing-compact"));
    let (vals, _) = edit.read_until(|v, _| v.contains(&want));
    assert!(vals.contains(&want), "{vals:?}");
}

/// A model chosen on Sidecar (or another client) sends all of the new model's
/// parameters, also those equal to the old model's at the same position:
/// WING Edit starts a model from its own defaults.
#[test]
fn a_new_model_sends_all_its_parameters() {
    let (e, st, server) = start(ConsoleModel::Full);
    let c = Console::new(&e, &st);
    let mut edit = Client::connect(&server);
    edit.read_for(Duration::from_millis(300));
    let s = schema();
    // A model whose parameters include one equal to the default model's at
    // the same position (same id, same value): a plain change list skips it.
    let params_of = |c: &Console| -> Vec<(u32, Option<Value>)> {
        let mut leaves = Vec::new();
        c.leaves(s.resolve("/ch/3/gate", c).unwrap(), &mut leaves);
        leaves.iter().filter(|n| matches!(n, Node::Param { .. })).map(|&n| (s.id(n), c.get(n))).collect()
    };
    let probe_engine = engine();
    let probe_state = WingState::default();
    let probe = Console::new(&probe_engine, &probe_state);
    let old = params_of(&probe);
    let mdl = s.resolve("/ch/3/gate/mdl", &probe).unwrap();
    let model = probe
        .def(mdl)
        .unwrap()
        .items()
        .iter()
        .map(|i| i.item)
        .find(|m| {
            probe.set(mdl, &Value::str(*m)).unwrap();
            let new = params_of(&probe);
            probe.set(mdl, &Value::str("GATE")).unwrap();
            *m != "GATE" && new.iter().any(|p| old.contains(p))
        })
        .expect("a model sharing a parameter value at a position");
    let gate = s.resolve("/ch/3/gate/mdl", &c).unwrap();
    c.set(gate, &Value::str(model)).unwrap(); // as the UI would (no client origin)
    let params: Vec<u32> = params_of(&c).into_iter().map(|(i, _)| i).collect();
    assert!(!params.is_empty());
    let mut got = Vec::new();
    let (vals, _) = edit.read_until(|v, _| params.iter().all(|p| v.iter().any(|(i, _)| i == p)));
    got.extend(vals);
    let missing: Vec<String> = params.iter().filter(|p| !got.iter().any(|(i, _)| i == *p)).map(|p| path_of(*p, &c)).collect();
    assert!(missing.is_empty(), "not sent: {missing:?}");
    assert!(got.contains(&(id("/ch/3/gate/mdl"), Value::str(model))));
}

unsafe extern "C" fn pass(_: *mut std::ffi::c_void, _: *const *mut f32, _: i32, _: i32) -> i32 {
    0
}

/// An FX slot hosting a plugin shows as EXT (WING Edit's EXTERNAL effect);
/// WING Edit choosing another model there is refused and answered with EXT,
/// and a sync round trip keeps the plugin.
#[test]
fn a_plugin_slot_is_an_external_effect() {
    let (e, st, server) = start(ConsoleModel::Full);
    let c = Console::new(&e, &st);
    e.controls.fx[2].set_model(FX_EXT, true);
    e.fx(2).unwrap().set(Some(InsertCallbacks { process: pass, release: None, ctx: std::ptr::null_mut() }), e.garbage());
    e.collect();
    let mut edit = Client::connect(&server);
    let before = edit.dump();
    let mdl = id("/fx/3/mdl");
    assert!(before.contains(&(mdl, Value::str("EXT"))));
    edit.set(mdl, &Value::str("HALL"));
    let want = (mdl, Value::str("EXT"));
    let (vals, _) = edit.read_until(|v, _| v.contains(&want));
    assert!(vals.contains(&want), "{vals:?}");
    write_back(&mut edit, &before, &c);
    assert!(e.fx(2).unwrap().is_loaded());
    let d = diffs(&before, &edit.dump(), &c);
    assert!(d.is_empty(), "{d:?}");
}

/// Engine defaults between two steps read on the step, as the console (and
/// WING Edit, which snaps to the steps) shows them: HALL's decay is 2.03 s.
#[test]
fn defaults_read_on_their_step() {
    let e = engine();
    let st = WingState::default();
    let c = Console::new(&e, &st);
    let s = schema();
    c.set(s.resolve("/fx/1/mdl", &c).unwrap(), &Value::str("HALL")).unwrap();
    let dcy = c.get(s.resolve("/fx/1/dcy", &c).unwrap()).and_then(|v| v.as_f32()).unwrap();
    // 0.2 s * 25^(72/100): step 72 of 101.
    assert!((dcy - 0.2 * 25f32.powf(0.72)).abs() < 1e-3, "{dcy}");
    let _ = StripKind::Channel;
}
