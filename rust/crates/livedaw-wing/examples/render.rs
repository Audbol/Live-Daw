//! Renders a WAV file through Sidecar's engine, offline and deterministically,
//! with the console set up from WING parameter paths: the Sidecar side of
//! `tools/wing/audio_compare`.
//!
//! The file's first channel plays into channel 1 (as the interface's input 1);
//! the output is a 32-bit float WAV of Main 1 L/R and, as channel 3, the
//! input itself (the reference the analysis measures against, as the WING's
//! recorder takes the USB player's signal on its input 3). Settings are WING paths
//! with values as the console shows or takes them, applied through the same
//! mapping as the WING remote (`Console::set`, so a value lands on the
//! parameter's step like on the console):
//!
//! ```json
//! { "settings": { "/ch/1/fdr": 0, "/main/1/fdr": 0, "/ch/1/eq/on": 1, "/ch/1/eq/mdl": "STD", "/ch/1/eq/2g": 6 } }
//! ```
//!
//! (a flat object of paths works too; keys starting with `_` are comments).
//! Model choices (`.../mdl`) are applied first, so a model's parameters
//! resolve against it.
//!
//! cargo run --release -p livedaw-wing --example render -- --settings test.json --in signal.wav --out sidecar.wav
//!     [--settings more.json ...] [--set PATH=VALUE ...] [--block N] [--preroll SECONDS] [--tail SECONDS] [--print]
//! cargo run -p livedaw-wing --example render -- --list /ch/1/eq [--settings test.json]

use livedaw_engine::recorder::wav::{SampleFormat, WavReader, WavWriter};
use livedaw_engine::{Engine, StripKind};
use livedaw_wing::json::{self, Json};
use livedaw_wing::schema::NodeType;
use livedaw_wing::{schema, value, Console, Node, Value, WingState};
use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering::Relaxed;

/// One setting: a WING path and its value.
#[derive(Clone, Debug, PartialEq)]
struct Setting {
    path: String,
    value: Value,
}

/// Reads settings from JSON text: `{"settings": {...}}` or a flat object of
/// paths. Keys starting with `_` are comments.
fn parse_settings(text: &str) -> Result<Vec<Setting>, String> {
    let doc = json::parse(text)?;
    let obj = match doc.get("settings") {
        Some(inner @ Json::Obj(_)) => inner,
        Some(_) => return Err("\"settings\" must be an object of WING paths".into()),
        None if matches!(doc, Json::Obj(_)) => &doc,
        None => return Err("expected a JSON object of WING paths".into()),
    };
    let mut out = Vec::new();
    for (key, v) in obj.entries() {
        if key.starts_with('_') {
            continue;
        }
        if !key.starts_with('/') {
            return Err(format!("{key}: paths start with '/' (e.g. /ch/1/eq/1g)"));
        }
        let value = match v {
            Json::Num(n) => Value::Float(*n as f32),
            Json::Bool(b) => Value::bool(*b),
            Json::Str(s) => Value::Str(s.clone()),
            _ => return Err(format!("{key}: the value must be a number, a string or true/false")),
        };
        out.push(Setting { path: key.clone(), value });
    }
    Ok(out)
}

/// `PATH=VALUE` from the command line (numbers become floats, the rest text).
fn parse_set(arg: &str) -> Result<Setting, String> {
    let (path, v) = arg.split_once('=').ok_or_else(|| format!("{arg}: expected PATH=VALUE"))?;
    let path = path.trim();
    if !path.starts_with('/') {
        return Err(format!("{path}: paths start with '/'"));
    }
    let v = v.trim();
    let value = match v.parse::<f32>() {
        Ok(x) if x.is_finite() => Value::Float(x),
        _ => Value::Str(v.to_owned()),
    };
    Ok(Setting { path: path.to_owned(), value })
}

/// The order to apply settings in: model choices first (a model's
/// parameters only exist once it is active), later duplicates win.
fn apply_order(settings: &[Setting]) -> Vec<Setting> {
    let mut out: Vec<Setting> = Vec::new();
    for s in settings {
        out.retain(|o| o.path != s.path);
        out.push(s.clone());
    }
    let is_model = |s: &Setting| s.path.trim_end_matches('/').ends_with("/mdl");
    let (mut first, rest): (Vec<_>, Vec<_>) = out.into_iter().partition(is_model);
    first.extend(rest);
    first
}

/// Applies the settings; returns a line per setting (path, value in effect,
/// and whether it reaches the audio).
fn apply(c: &Console, settings: &[Setting]) -> Result<Vec<String>, String> {
    let s = schema();
    let mut report = Vec::new();
    for st in apply_order(settings) {
        let node = s.resolve(&st.path, c).ok_or_else(|| format!("{}: no such WING parameter", st.path))?;
        let applied = c.set(node, &st.value).map_err(|e| format!("{} = {:?}: {e}", st.path, st.value))?;
        let def = c.def(node).ok_or_else(|| format!("{}: no definition", st.path))?;
        let note = if c.is_mapped(node) { "" } else { "   (stored only: no effect on Sidecar's audio)" };
        report.push(format!("{} = {}{note}", st.path, value::display(def, &applied)));
    }
    Ok(report)
}

/// The parameters under a path with their values and ranges.
fn list(c: &Console, path: &str) -> Result<Vec<String>, String> {
    let s = schema();
    let node = s.resolve(path, c).ok_or_else(|| format!("{path}: no such WING node"))?;
    let nodes = if s.is_leaf(node, c) { vec![node] } else { s.children(node, c) };
    Ok(nodes.into_iter().map(|n| describe(c, n)).collect())
}

fn describe(c: &Console, node: Node) -> String {
    let s = schema();
    let path = s.path(node, c);
    let Some(def) = c.def(node) else { return path };
    let current = c.get(node).map(|v| value::display(def, &v)).unwrap_or_default();
    let unit = livedaw_wing::schema::unit::name(def.unit);
    let range = match def.ty {
        NodeType::Node => "(node)".to_owned(),
        NodeType::LinFloat | NodeType::LogFloat => format!("{} .. {} {unit}, {} steps", def.min, def.max, def.steps),
        NodeType::Fader => "fader -oo .. 10 dB".to_owned(),
        NodeType::Int => format!("int {} .. {} {unit}", def.min_int, def.max_int),
        NodeType::StrEnum => format!("[{}]", def.items().iter().map(|i| i.item).collect::<Vec<_>>().join(", ")),
        NodeType::FloatEnum => {
            let items: Vec<String> = def.items().iter().map(|i| value::format_float_item(def, i.value)).collect();
            format!("[{}]", items.join(", ").replace("  ", " "))
        }
        NodeType::Str => "string".to_owned(),
    };
    let mapped = if !def.is_leaf() || c.is_mapped(node) { "" } else { "   (stored only)" };
    format!("{path} = {current}   {range}{mapped}")
}

#[derive(Debug)]
struct Options {
    settings: Vec<Setting>,
    input: Option<PathBuf>,
    output: Option<PathBuf>,
    list: Option<String>,
    block: usize,
    preroll: f64,
    tail: f64,
    print: bool,
}

/// A number in `lo..=hi` for option `what`.
fn number<T: std::str::FromStr + PartialOrd + std::fmt::Display>(text: &str, lo: T, hi: T, what: &str) -> Result<T, String> {
    text.trim().parse().ok().filter(|v| *v >= lo && *v <= hi).ok_or_else(|| format!("{what}: a number from {lo} to {hi}"))
}

fn parse_args(args: &[String]) -> Result<Options, String> {
    let mut o =
        Options { settings: Vec::new(), input: None, output: None, list: None, block: 64, preroll: 1.0, tail: 0.0, print: false };
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut next = || it.next().cloned().ok_or_else(|| format!("{a} needs a value"));
        match a.as_str() {
            "--settings" => {
                let file = next()?;
                let text = std::fs::read_to_string(&file).map_err(|e| format!("{file}: {e}"))?;
                o.settings.extend(parse_settings(&text).map_err(|e| format!("{file}: {e}"))?);
            }
            "--set" => o.settings.push(parse_set(&next()?)?),
            "--in" => o.input = Some(next()?.into()),
            "--out" => o.output = Some(next()?.into()),
            "--list" => o.list = Some(next()?),
            "--block" => o.block = number(&next()?, 1, 8192, a)?,
            "--preroll" => o.preroll = number(&next()?, 0.0, 60.0, a)?,
            "--tail" => o.tail = number(&next()?, 0.0, 600.0, a)?,
            "--print" => o.print = true,
            other => return Err(format!("unknown argument {other}")),
        }
    }
    Ok(o)
}

/// A console as a factory-initialized WING with channel 1 on input 1 and
/// Main 1 on outputs 1-2.
fn console_engine(sample_rate: f64, block: usize) -> Engine {
    let engine = Engine::new(0);
    engine.set_num_channels(40);
    // SAFETY: nothing is processing yet.
    unsafe { engine.prepare(sample_rate, block, &[0], &[0, 1]) };
    let ch = engine.channel_at(0).expect("channel 1");
    ch.params.input_left.store(0, Relaxed);
    ch.params.input_right.store(-1, Relaxed);
    let main = engine.strip_at(StripKind::Main, 0).expect("main 1");
    main.params.output_left.store(0, Relaxed);
    main.params.output_right.store(1, Relaxed);
    engine
}

/// Runs `input` (mono) through the engine; `preroll` samples of silence first
/// (discarded) let parameter ramps and detectors settle.
fn render(engine: &Engine, input: &[f32], block: usize, preroll: usize, tail: usize) -> [Vec<f32>; 2] {
    let total = preroll + input.len() + tail;
    let mut out = [Vec::with_capacity(total), Vec::with_capacity(total)];
    let mut inb = vec![0.0f32; block];
    let mut outb = [vec![0.0f32; block], vec![0.0f32; block]];
    let mut pos = 0;
    while pos < total {
        let n = block.min(total - pos);
        for (i, x) in inb[..n].iter_mut().enumerate() {
            *x = (pos + i).checked_sub(preroll).and_then(|k| input.get(k)).copied().unwrap_or(0.0);
        }
        let ins = [inb.as_ptr()];
        let outs = [outb[0].as_mut_ptr(), outb[1].as_mut_ptr()];
        // SAFETY: the buffers hold `block` >= n samples; this is the only audio thread.
        unsafe { engine.process(&ins, &outs, n) };
        for (o, b) in out.iter_mut().zip(&outb) {
            o.extend_from_slice(&b[..n]);
        }
        // The message thread's duties (FX devices chosen by the settings, garbage).
        if pos < preroll && (pos / block) % 64 == 0 {
            engine.collect();
        }
        pos += n;
    }
    for o in &mut out {
        o.drain(..preroll.min(o.len()));
    }
    out
}

fn read_mono(path: &Path) -> Result<(Vec<f32>, u32), String> {
    let err = |e: std::io::Error| format!("{}: {e}", path.display());
    let mut r = WavReader::open(path).map_err(err)?;
    let info = *r.info();
    let mut x = vec![0.0f32; info.frames as usize];
    r.read(0, &mut [&mut x[..]]).map_err(err)?;
    Ok((x, info.sample_rate))
}

/// Main 1 L/R and the input (silence past its end).
fn write_capture(path: &Path, out: &[Vec<f32>; 2], input: &[f32], sample_rate: u32) -> Result<(), String> {
    let err = |e: std::io::Error| format!("{}: {e}", path.display());
    let mut w = WavWriter::create_with_format(path, 3, sample_rate, SampleFormat::Float32).map_err(err)?;
    w.write_planar(&[&out[0], &out[1], input], out[0].len()).map_err(err)?;
    w.finalize().map_err(err)?;
    Ok(())
}

fn run(args: &[String]) -> Result<(), String> {
    let o = parse_args(args)?;
    let state = WingState::default();
    if let Some(path) = &o.list {
        let engine = console_engine(48_000.0, o.block);
        let c = Console::new(&engine, &state);
        apply(&c, &o.settings)?;
        for line in list(&c, path)? {
            println!("{line}");
        }
        return Ok(());
    }
    let (input, output) = match (&o.input, &o.output) {
        (Some(i), Some(out)) => (i, out),
        _ => return Err("usage: render --settings FILE.json --in IN.wav --out OUT.wav (or --list PATH)".into()),
    };
    let (signal, sample_rate) = read_mono(input)?;
    let engine = console_engine(sample_rate as f64, o.block);
    let report = apply(&Console::new(&engine, &state), &o.settings)?;
    if o.print {
        for line in &report {
            println!("{line}");
        }
    }
    engine.collect();
    let sr = sample_rate as f64;
    let out = render(&engine, &signal, o.block, (o.preroll * sr) as usize, (o.tail * sr) as usize);
    if let Some(i) = (0..engine.num_fx()).find(|&i| engine.fx(i).is_some_and(|f| f.is_faulted())) {
        return Err(format!("FX {} faulted while rendering", i + 1));
    }
    write_capture(output, &out, &signal, sample_rate)?;
    println!("wrote {} ({:.2} s, {} settings)", output.display(), out[0].len() as f64 / sr, report.len());
    Ok(())
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if let Err(e) = run(&args) {
        eprintln!("render: {e}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn set(path: &str, value: Value) -> Setting {
        Setting { path: path.into(), value }
    }

    #[test]
    fn parses_settings_objects() {
        let flat = r#"{"_comment": "EQ band 2", "/ch/1/eq/2g": 6, "/ch/1/eq/mdl": "STD", "/ch/1/eq/on": true}"#;
        let wrapped = format!(r#"{{"name": "peq", "settings": {flat}}}"#);
        for text in [flat, wrapped.as_str()] {
            assert_eq!(
                parse_settings(text).unwrap(),
                vec![
                    set("/ch/1/eq/2g", Value::Float(6.0)),
                    set("/ch/1/eq/mdl", Value::str("STD")),
                    set("/ch/1/eq/on", Value::Int(1)),
                ]
            );
        }
        assert!(parse_settings(r#"{"eq/2g": 6}"#).unwrap_err().contains("start with '/'"));
        assert!(parse_settings(r#"{"/ch/1/eq/2g": [6]}"#).is_err());
        assert!(parse_settings(r#"{"settings": 3}"#).is_err());
        assert!(parse_settings("[1]").is_err());
        assert!(parse_settings("{").is_err());
    }

    #[test]
    fn parses_command_line_settings() {
        assert_eq!(parse_set("/ch/1/dyn/thr=-20").unwrap(), set("/ch/1/dyn/thr", Value::Float(-20.0)));
        assert_eq!(parse_set("/ch/1/dyn/mdl = COMP").unwrap(), set("/ch/1/dyn/mdl", Value::str("COMP")));
        assert_eq!(parse_set("/ch/1/gate/ratio=1:3").unwrap(), set("/ch/1/gate/ratio", Value::str("1:3")));
        assert!(parse_set("/ch/1/fdr").is_err());
        assert!(parse_set("fdr=0").is_err());
        let args = |a: &[&str]| parse_args(&a.iter().map(|s| s.to_string()).collect::<Vec<_>>());
        let o =
            args(&["--set", "/ch/1/fdr=-3", "--block", "128", "--preroll", "0.5", "--in", "a.wav", "--out", "b.wav"]).unwrap();
        assert_eq!((o.block, o.preroll, o.settings.len()), (128, 0.5, 1));
        assert_eq!(o.input, Some(PathBuf::from("a.wav")));
        assert!(args(&["--block", "0"]).unwrap_err().contains("--block"));
        assert!(args(&["--tail"]).unwrap_err().contains("needs a value"));
        assert!(args(&["--settings", "/nonexistent/settings.json"]).is_err());
        assert!(args(&["--bogus"]).is_err());
    }

    #[test]
    fn models_first_and_later_values_win() {
        let order = apply_order(&[
            set("/ch/1/dyn/thr", Value::Float(-10.0)),
            set("/ch/1/dyn/mdl", Value::str("COMP")),
            set("/ch/1/dyn/thr", Value::Float(-20.0)),
        ]);
        assert_eq!(order, vec![set("/ch/1/dyn/mdl", Value::str("COMP")), set("/ch/1/dyn/thr", Value::Float(-20.0))]);
    }

    #[test]
    fn settings_reach_the_engine_and_unknown_paths_fail() {
        let engine = console_engine(48_000.0, 64);
        let state = WingState::default();
        let c = Console::new(&engine, &state);
        let report = apply(
            &c,
            &[
                set("/ch/1/eq/2g", Value::Float(6.0)),
                set("/ch/1/eq/mdl", Value::str("STD")),
                set("/ch/1/dyn/mdl", Value::str("COMP")),
                set("/ch/1/dyn/ratio", Value::str("4.0")),
            ],
        )
        .unwrap();
        assert!(report.iter().any(|l| l.starts_with("/ch/1/eq/2g = 6.0")), "{report:?}");
        let ch = engine.channel_at(0).unwrap();
        assert_eq!(
            ch.params.dynamics.model_index(),
            livedaw_dsp::wing::model_index(livedaw_dsp::wing::DYN_MODELS, "COMP").unwrap()
        );
        assert!(apply(&c, &[set("/ch/1/eq/nothing", Value::Float(1.0))]).unwrap_err().contains("no such WING parameter"));
        assert!(list(&c, "/ch/1/eq").unwrap().iter().any(|l| l.starts_with("/ch/1/eq/2g = 6.0")));
    }

    #[test]
    fn renders_unity_and_is_deterministic() {
        let unity =
            [set("/ch/1/fdr", Value::Float(0.0)), set("/main/1/fdr", Value::Float(0.0)), set("/ch/1/pan", Value::Float(0.0))];
        let input: Vec<f32> = (0..4800).map(|i| (i as f32 * 0.05).sin() * 0.5).collect();
        let once = || {
            let engine = console_engine(48_000.0, 64);
            let state = WingState::default();
            apply(&Console::new(&engine, &state), &unity).unwrap();
            engine.collect();
            render(&engine, &input, 64, 4800, 480)
        };
        let a = once();
        assert_eq!(a[0].len(), input.len() + 480);
        assert_eq!(a, once());
        // A centred mono channel reaches both sides of Main 1 at the same level.
        let rms = |x: &[f32]| (x.iter().map(|v| v * v).sum::<f32>() / x.len() as f32).sqrt();
        assert!(rms(&a[0]) > 0.1 && (rms(&a[0]) - rms(&a[1])).abs() < 1e-6);
    }
}
