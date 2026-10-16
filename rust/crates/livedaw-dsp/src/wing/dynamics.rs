//! WING dynamics models (the GATE and DYNAMICS slots): one realtime engine
//! with a handful of cores - gate/expander, ducker, compressor/limiter,
//! de-esser, dynamic EQ, transient designer, level rider and saturation - and
//! a mapping from each of the 32 console models' controls onto those cores.
//!
//! Model character comes from the detector (peak/RMS, feedback), knee, time
//! constants (fixed, stepped, program dependent, opto, release rate), gain
//! staging and a little analog colouration. Nothing here allocates.

use super::{colour, dial_log, ModelSpec, Params, DYN_MODELS, MAX_DYN_PARAMS};
use crate::biquad::{Biquad, BiquadCoeffs};
use crate::util::{db_to_gain, gain_to_db};

pub const SC_FILTERS: [&str; 5] = ["OFF", "LP12", "HP12", "BP", "NOTCH"];
pub const XOVER_TYPES: [&str; 6] = ["OFF", "LO", "HI", "S6", "S12", "PEAK"];

/// Key (sidechain) filter of a dynamics slot.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SidechainSettings {
    /// Index into [`SC_FILTERS`].
    pub filter: usize,
    pub freq: f32,
    pub q: f32,
}

impl Default for SidechainSettings {
    fn default() -> Self {
        Self { filter: 0, freq: 1000.0, q: 1.0 }
    }
}

/// Restricts the dynamics to a frequency band (DYN XOVER).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct XoverSettings {
    /// Index into [`XOVER_TYPES`].
    pub kind: usize,
    pub freq: f32,
    pub q: f32,
    /// Largest gain change of the band in dB (0 = unlimited).
    pub depth: f32,
}

impl Default for XoverSettings {
    fn default() -> Self {
        Self { kind: 0, freq: 1000.0, q: 1.0, depth: 0.0 }
    }
}

/// What the slot did in the last block (meters and LEDs).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct DynStatus {
    /// Largest gain reduction in the block, in dB (>= 0).
    pub gain_reduction_db: f32,
    /// Gate closed / compressor working (the console's GATE and DYN LEDs).
    pub active: bool,
}

// ---- configuration --------------------------------------------------------------------

fn coef(ms: f32, sr: f32) -> f32 {
    (-1.0 / (ms.max(0.005) * 0.001 * sr)).exp()
}

#[derive(Clone, Copy, Debug)]
struct GateCfg {
    thr: f32,
    close: f32,
    range: f32,
    /// Expansion ratio; 0 = gate.
    ratio: f32,
    att: f32,
    rel: f32,
    hold: u32,
    accent_db: f32,
    rms: bool,
}

#[derive(Clone, Copy, Debug)]
enum Release {
    Fixed,
    /// Release time grows with sustained gain reduction (up to `max_ms`).
    Auto {
        max_ms: f32,
    },
    /// Opto cell: fast first stage, slow program-dependent tail.
    Opto,
    /// Constant recovery rate in dB/s (VCA "release rate" designs).
    Rate(f32),
}

#[derive(Clone, Copy, Debug)]
struct CompCfg {
    thr: f32,
    /// 1/ratio - 1 (0 = no compression, -1 = limiting, < -1 beyond infinity).
    slope: f32,
    knee: f32,
    /// Downward expander instead of compressor (uses `ratio`).
    expander: Option<(f32, f32)>,
    att: f32,
    rel: f32,
    rel_ms: f32,
    hold: u32,
    rms: bool,
    linear_release: bool,
    release: Release,
    feedback: bool,
    pre_db: f32,
    post_db: f32,
    colour: f32,
    max_gr: f32,
    /// Peak limiter after the compressor: (threshold dB, release coef).
    limiter: Option<(f32, f32)>,
    enabled: bool,
}

impl Default for CompCfg {
    fn default() -> Self {
        Self {
            thr: 0.0,
            slope: 0.0,
            knee: 0.0,
            expander: None,
            att: 0.0,
            rel: 0.0,
            rel_ms: 100.0,
            hold: 0,
            rms: false,
            linear_release: false,
            release: Release::Fixed,
            feedback: false,
            pre_db: 0.0,
            post_db: 0.0,
            colour: 0.0,
            max_gr: f32::INFINITY,
            limiter: None,
            enabled: true,
        }
    }
}

#[derive(Clone, Copy, Debug)]
struct DeqBand {
    thr: f32,
    slope: f32,
    att: f32,
    rel: f32,
    /// Detection/processing band (0 = broadband).
    filter: usize,
    gain_db: f32,
    freq: f32,
    q: f32,
    /// true = act above threshold, false = below.
    high: bool,
}

#[derive(Clone, Copy, Debug)]
enum Config {
    Gate(GateCfg),
    Duck(GateCfg),
    Comp(CompCfg),
    /// Two compressors in series (e.g. limiter + compressor).
    Dual(CompCfg, CompCfg),
    Combo(GateCfg, CompCfg),
    DeEss {
        freq: f32,
        range: f32,
        full_band: bool,
    },
    DynEq([DeqBand; 2], usize),
    Wave {
        attack: f32,
        sustain: f32,
        gain: f32,
    },
    Ride {
        thr: f32,
        target: f32,
        speed_db_s: f32,
        amount: f32,
        hold: u32,
        range: f32,
    },
    Warm {
        drive: f32,
        harm: f32,
        colour: f32,
        trim: f32,
        mix: f32,
    },
    Lmt {
        speed: f32,
        trans: f32,
        trans_gain: f32,
        trans_on: bool,
        comp: CompCfg,
    },
}

fn ms_choice(text: &str, default: f32) -> f32 {
    text.parse::<f32>().unwrap_or(default)
}

/// ratio -> slope for the gain computer (999 = infinity).
fn slope_of(ratio: f32) -> f32 {
    if ratio >= 999.0 || ratio == 0.0 {
        -1.0
    } else {
        1.0 / ratio - 1.0
    }
}

/// dBu-calibrated thresholds of analog models: +18 dBu = 0 dBFS.
fn dbu(x: f32) -> f32 {
    x - 18.0
}

fn configure(spec: &ModelSpec, v: &[f32], sr: f32) -> Config {
    let p = Params::new(spec, v);
    let comp = |thr: f32, ratio: f32, knee: f32, att_ms: f32, rel_ms: f32| CompCfg {
        thr,
        slope: slope_of(ratio),
        knee,
        att: coef(att_ms, sr),
        rel: coef(rel_ms, sr),
        rel_ms,
        ..Default::default()
    };
    let gate = |thr: f32, range: f32, ratio: f32, att_ms: f32, hold_ms: f32, rel_ms: f32| GateCfg {
        thr,
        close: thr,
        range,
        ratio,
        att: coef(att_ms, sr),
        rel: coef(rel_ms, sr),
        hold: (hold_ms * 0.001 * sr) as u32,
        accent_db: 0.0,
        rms: false,
    };
    // LA-style opto leveler, shared by LA and CMB.
    let leveler = |gain: f32, peak: f32, limit: bool| CompCfg {
        release: Release::Opto,
        rms: true,
        colour: 0.12,
        post_db: (gain - 50.0) * 0.4,
        ..comp(-peak * 0.4, if limit { 10.0 } else { 3.0 }, if limit { 6.0 } else { 10.0 }, 10.0, 60.0)
    };

    match spec.token {
        // ---- gates and expanders -------------------------------------------------------
        "GATE" => {
            let ratio = [1.5, 2.0, 3.0, 4.0, 0.0][p.choice("ratio").min(4)];
            let mut g = gate(p.f("thr"), p.f("range"), ratio, p.f("att"), p.f("hld"), p.f("rel"));
            g.accent_db = p.f("acc") / 100.0 * 6.0;
            Config::Gate(g)
        }
        "DUCK" => Config::Duck(gate(p.f("thr"), p.f("range"), 0.0, p.f("att"), p.f("hld"), p.f("rel"))),
        "E88" => {
            let thr = p.f("thr") - if p.on("m40") { 40.0 } else { 0.0 };
            let mut g = gate(thr, p.f("range"), 0.0, if p.on("fast") { 0.1 } else { 1.5 }, 10.0, p.f("rel"));
            g.close = thr - p.f("hyst");
            Config::Gate(g)
        }
        "9000G" => {
            let ratio = if p.text("mode") == "EXP" { 2.0 } else { 0.0 };
            Config::Gate(gate(p.f("thr"), p.f("range"), ratio, if p.on("fast") { 0.1 } else { 1.0 }, p.f("hld"), p.f("rel")))
        }
        "D241G" => {
            let slow = p.on("slow");
            let mut g = gate(
                p.f("thr"),
                80.0,
                0.0,
                if slow { 2.0 } else { 0.2 },
                if slow { 150.0 } else { 50.0 },
                if slow { 500.0 } else { 100.0 },
            );
            g.close = g.thr - 4.0;
            Config::Gate(g)
        }
        "PSE" | "CMB" => {
            let fast = p.on("fast");
            let mut g = gate(p.f("thr"), p.f("depth"), 2.5, if fast { 0.5 } else { 5.0 }, 20.0, if fast { 60.0 } else { 250.0 });
            g.rms = !p.on("peak");
            if spec.token == "PSE" {
                Config::Gate(g)
            } else {
                Config::Combo(g, leveler(p.f("ingain"), p.f("cpeak"), p.text("cmode") == "LIM"))
            }
        }
        "DS902" => Config::DeEss { freq: p.f("f"), range: p.f("range"), full_band: p.text("mode") == "FULL" },
        "DEQ" | "DEQ2" => {
            let band = |prefix: &str| {
                let get = |id: &str| -> f32 {
                    let mut buf = [0u8; 16];
                    p.f(join(&mut buf, prefix, id))
                };
                let text = |id: &str| -> &'static str {
                    let mut buf = [0u8; 16];
                    p.text(join(&mut buf, prefix, id))
                };
                let filter = match text("filt") {
                    "BP" => 1,
                    "LP6" => 2,
                    "LP12" => 3,
                    "HP6" => 4,
                    "HP12" => 5,
                    _ => 0,
                };
                DeqBand {
                    thr: get("thr"),
                    slope: slope_of(get("ratio")),
                    att: coef(get("att"), sr),
                    rel: coef(get("rel"), sr),
                    filter,
                    gain_db: get("g"),
                    freq: get("f"),
                    q: get("q"),
                    high: text("mode") != "low",
                }
            };
            if spec.token == "DEQ" {
                let b = band("");
                Config::DynEq([b, b], 1)
            } else {
                Config::DynEq([band("1-"), band("2-")], 2)
            }
        }
        "WAVE" => Config::Wave { attack: p.f("att"), sustain: p.f("sust"), gain: p.f("g") },
        "RIDE" => Config::Ride {
            thr: p.f("thr"),
            target: p.f("tgt"),
            speed_db_s: p.f("spd"),
            amount: 1.0 - 1.0 / p.f("ratio").max(1.0),
            hold: (p.f("hld") * sr) as u32,
            range: p.f("range"),
        },
        "WARM" => Config::Warm {
            drive: p.f("drv") / 100.0,
            harm: p.f("hrm") / 100.0,
            colour: p.f("col"),
            trim: p.f("trim"),
            mix: p.f("wmix") / 100.0,
        },

        // ---- compressors ---------------------------------------------------------------
        "COMP" | "EXP" => {
            let ratio = p.f("ratio");
            let mut c = comp(p.f("thr"), ratio, p.f("knee") * 3.0, p.f("att"), p.f("rel"));
            c.hold = (p.f("hld") * 0.001 * sr) as u32;
            c.rms = p.text("det") == "RMS";
            c.linear_release = p.text("env") == "LIN";
            if p.on("auto") {
                c.release = Release::Auto { max_ms: (p.f("rel") * 6.0).min(4000.0) };
            }
            if spec.token == "EXP" {
                c.expander = Some((ratio, 60.0));
                c.slope = 0.0;
            }
            Config::Comp(c)
        }
        "B160" => {
            let thr = 20.0 * (p.f("thr") / 0.775).log10();
            let c = CompCfg { rms: true, release: Release::Rate(120.0), ..comp(dbu(thr), p.f("comp"), 0.0, 15.0, 100.0) };
            Config::Comp(c)
        }
        "B560" => {
            let knee = if p.on("easy") { 12.0 } else { 0.0 };
            let c =
                CompCfg { rms: true, release: Release::Rate(120.0), ..comp(dbu(p.f("thr")), p.f("ratio"), knee, 10.0, 100.0) };
            Config::Comp(c)
        }
        "D241C" => {
            let auto = p.on("auto");
            let mut c = comp(p.f("thr"), p.f("ratio"), 2.0, if auto { 10.0 } else { p.f("att") }, p.f("rel"));
            if auto {
                c.release = Release::Auto { max_ms: 1500.0 };
            }
            c.limiter = Some((p.f("lim"), coef(p.f("lrel"), sr)));
            Config::Comp(c)
        }
        "ECL33" => {
            let rec = |text: &str, fallback: f32| -> (f32, Release) {
                match text {
                    "A1" => (100.0, Release::Auto { max_ms: 2000.0 }),
                    "A2" => (50.0, Release::Auto { max_ms: 5000.0 }),
                    t => (ms_choice(t, fallback), Release::Fixed),
                }
            };
            let (lrel, lmode) = rec(p.text("lrec"), 100.0);
            let mut limiter = comp(p.f("lthr"), 999.0, 0.0, if p.on("lfast") { 2.0 } else { 4.0 }, lrel);
            limiter.release = lmode;
            limiter.enabled = p.on("lon");
            let (crel, cmode) = rec(p.text("crec"), 400.0);
            let mut compressor = comp(p.f("cthr"), p.f("ratio"), 3.0, if p.on("cfast") { 3.0 } else { 20.0 }, crel);
            compressor.release = cmode;
            compressor.enabled = p.on("con");
            compressor.post_db = p.f("cgain");
            compressor.colour = 0.08;
            Config::Dual(compressor, limiter)
        }
        "9000C" => {
            let mut c = comp(dbu(p.f("thr")), p.f("ratio"), 2.0, if p.on("fast") { 3.0 } else { 30.0 }, p.f("rel"));
            c.rms = !p.on("peak");
            c.colour = 0.05;
            Config::Comp(c)
        }
        "SBUS" => {
            let rel_text = p.text("rel");
            let auto = rel_text == "AUTO";
            let rel_ms = if auto { 100.0 } else { ms_choice(rel_text, 0.4) * 1000.0 };
            let mut c = comp(p.f("thr"), p.f("ratio"), 1.0, p.f("att"), rel_ms);
            if auto {
                c.release = Release::Auto { max_ms: 1200.0 };
            }
            c.colour = 0.05;
            Config::Comp(c)
        }
        "RED3" => {
            let auto = p.on("auto");
            let mut c = comp(p.f("thr"), p.f("ratio"), 4.0, if auto { 8.0 } else { p.f("att") }, p.f("rel"));
            if auto {
                c.release = Release::Auto { max_ms: 2000.0 };
            }
            c.colour = 0.05;
            Config::Comp(c)
        }
        "76LA" => {
            // FET limiter: the input knob drives a fixed threshold; knobs at 7 are fastest.
            let all = p.text("ratio") == "ALL";
            let ratio = if all { 20.0 } else { ms_choice(p.text("ratio"), 4.0) };
            let att = 0.8 * (0.02f32 / 0.8).powf((p.f("att") - 1.0) / 6.0);
            let rel = 1100.0 * (50.0f32 / 1100.0).powf((p.f("rel") - 1.0) / 6.0);
            let mut c = comp(-18.0, ratio, if all { 6.0 } else { 2.0 }, att, rel);
            c.pre_db = p.f("in") + 24.0;
            c.post_db = p.f("out") + 24.0;
            c.colour = if all { 0.3 } else { 0.15 };
            Config::Comp(c)
        }
        "LA" => Config::Comp(leveler(p.f("ingain"), p.f("peak"), p.text("mode") == "LIM")),
        "F670" => {
            let (att, rel, auto) = match p.f("time") as i32 {
                1 => (0.2, 300.0, false),
                2 => (0.2, 800.0, false),
                3 => (0.4, 2000.0, false),
                4 => (0.4, 5000.0, false),
                5 => (0.4, 2000.0, true),
                _ => (0.2, 300.0, true),
            };
            let bias = p.f("bias");
            let mut c = comp(-p.f("thr") * 4.0, 2.0 + 6.0 * (1.0 - bias), 6.0 + 18.0 * bias, att, rel);
            if auto {
                c.release = Release::Auto { max_ms: if p.f("time") as i32 == 5 { 10000.0 } else { 25000.0 } };
            }
            c.pre_db = p.f("in") + 10.0;
            c.post_db = 0.0;
            c.rms = true;
            c.colour = 0.2;
            Config::Comp(c)
        }
        "BLISS" => {
            let mut c = comp(p.f("thr"), p.f("ratio"), 2.0, p.f("att"), p.f("rel"));
            if p.on("afast") {
                c.release = Release::Auto { max_ms: (p.f("rel") * 4.0).min(4000.0) };
            }
            c.linear_release = p.on("alog");
            if p.on("glon") {
                c.max_gr = -p.f("glim");
            }
            Config::Comp(c)
        }
        "NSTR" => {
            let text = p.text("ratio");
            let nuke = text == "NUKE";
            let ratio = if nuke { 999.0 } else { text.trim_end_matches(":1").parse().unwrap_or(4.0) };
            let att = dial_log(p.f("att"), 0.05, 30.0);
            let rel = dial_log(p.f("rel"), 50.0, 3500.0);
            let mut c = comp(-20.0, ratio, if ratio <= 4.0 { 6.0 } else { 1.0 }, if nuke { 0.05 } else { att }, rel);
            c.pre_db = p.f("in") * 4.0;
            c.post_db = (p.f("out") - 5.0) * 4.0;
            c.colour = 0.1;
            Config::Comp(c)
        }
        "2250" => {
            let amount = p.f("ratio") / 10.0;
            let ratio = if amount >= 0.999 { 999.0 } else { 1.0 / (1.0 - amount) };
            let att = match p.text("att") {
                "FAST" => 1.0,
                "SLOW" => 30.0,
                _ => 10.0,
            };
            let mut c = comp(-p.f("thr") * 4.0, ratio, if p.text("knee") == "SOFT" { 10.0 } else { 0.0 }, att, p.f("rel"));
            c.feedback = p.text("type") == "OLD";
            Config::Comp(c)
        }
        "L100" => {
            let speed = |t: &str, f: f32, m: f32, s: f32| match t {
                "FAST" => f,
                "SLOW" => s,
                _ => m,
            };
            let mut c = comp(
                -p.f("gr") * 4.0,
                4.0,
                8.0,
                speed(p.text("att"), 2.0, 10.0, 40.0),
                speed(p.text("rel"), 100.0, 400.0, 1500.0),
            );
            c.pre_db = (p.f("ingain") - 5.0) * 3.0;
            c.rms = true;
            c.colour = 0.1;
            Config::Comp(c)
        }
        "E88C" => {
            let thr = dbu(p.f("thr")) - if p.on("thrpull") { 20.0 } else { 0.0 };
            let mut c = comp(
                thr,
                p.f("ratio"),
                if p.text("knee") == "SOFT" { 6.0 } else { 0.0 },
                if p.text("att") == "FAST" { 3.0 } else { 30.0 },
                p.f("rel"),
            );
            c.colour = 0.05;
            Config::Comp(c)
        }
        "LMT" => {
            let mut c = comp(-p.f("comp") * 0.3, 4.0, 6.0, 5.0, 200.0);
            c.release = Release::Auto { max_ms: 1500.0 };
            c.post_db = p.f("cgain");
            c.enabled = p.on("con");
            Config::Lmt { speed: p.f("tspd"), trans: p.f("trans"), trans_gain: p.f("tgain"), trans_on: p.on("ton"), comp: c }
        }
        "ONEC" => {
            let amount = p.f("gr");
            let mut c = comp(-amount * 3.5, 1.5 + amount * 0.35, 6.0, if p.on("dag") { 1.0 } else { 10.0 }, 150.0);
            c.release = Release::Auto { max_ms: 1200.0 };
            c.colour = if p.on("dag") { 0.08 } else { 0.0 };
            Config::Comp(c)
        }
        _ => Config::Comp(CompCfg { enabled: false, ..Default::default() }),
    }
}

/// "1-" + "thr" without allocating.
fn join<'a>(buf: &'a mut [u8; 16], a: &str, b: &str) -> &'a str {
    let n = (a.len() + b.len()).min(16);
    let (x, y) = buf.split_at_mut(a.len().min(16));
    x.copy_from_slice(&a.as_bytes()[..x.len()]);
    let m = n - x.len();
    y[..m].copy_from_slice(&b.as_bytes()[..m]);
    std::str::from_utf8(&buf[..n]).unwrap_or("")
}

// ---- state ----------------------------------------------------------------------------

#[derive(Clone, Copy, Default)]
struct CompState {
    gr: f32,
    ms: f32,
    hold: u32,
    slow_gr: f32,
    opto_slow: f32,
    lim_gr: f32,
    last_out: f32,
}

#[derive(Clone, Copy, Default)]
struct GateState {
    env: f32,
    gain: f32,
    hold: u32,
    open: bool,
    accent: f32,
}

pub struct WingDynamics {
    sr: f32,
    model: Option<usize>,
    values: [f32; MAX_DYN_PARAMS],
    cfg: Option<Config>,
    comp: [CompState; 2],
    gate: GateState,
    // Key filter (mono key)
    sc: Biquad,
    sc_settings: Option<SidechainSettings>,
    // Crossover band filter (per channel) and its settings
    xo: Biquad,
    xo_settings: Option<XoverSettings>,
    // De-esser / dynamic EQ helpers
    aux_filters: [Biquad; 4],
    aux_env: [f32; 4],
    aux_gain: [f32; 2],
    // Transient designer envelopes: fast, slow, sustain-fast, sustain-slow
    wave: [f32; 4],
    // Rider
    ride_gain: f32,
    ride_ms: f32,
    ride_hold: u32,
    // Saturation tone filter state per channel
    warm_lp: [f32; 2],
    rms_coef: f32,
}

impl Default for WingDynamics {
    fn default() -> Self {
        let mut d = Self {
            sr: 48000.0,
            model: None,
            values: [0.0; MAX_DYN_PARAMS],
            cfg: None,
            comp: [CompState::default(); 2],
            gate: GateState::default(),
            sc: Biquad::default(),
            sc_settings: None,
            xo: Biquad::default(),
            xo_settings: None,
            aux_filters: Default::default(),
            aux_env: [0.0; 4],
            aux_gain: [0.0; 2],
            wave: [0.0; 4],
            ride_gain: 0.0,
            ride_ms: 0.0,
            ride_hold: 0,
            warm_lp: [0.0; 2],
            rms_coef: 0.0,
        };
        d.prepare(48000.0);
        d
    }
}

/// Static compressor curve: gain change in dB (<= 0) for a level in dB.
#[inline]
fn comp_curve(x: f32, thr: f32, slope: f32, knee: f32) -> f32 {
    let over = x - thr;
    if knee > 0.0 && over.abs() <= knee * 0.5 {
        slope * (over + knee * 0.5).powi(2) / (2.0 * knee)
    } else if over > 0.0 {
        slope * over
    } else {
        0.0
    }
}

/// Downward expander: gain change (<= 0) below the threshold, limited to `range`.
#[inline]
fn exp_curve(x: f32, thr: f32, ratio: f32, knee: f32, range: f32) -> f32 {
    let under = x - thr;
    let g = if knee > 0.0 && under.abs() <= knee * 0.5 {
        (ratio - 1.0) * -(under - knee * 0.5).powi(2) / (2.0 * knee)
    } else if under < 0.0 {
        (ratio - 1.0) * under
    } else {
        0.0
    };
    g.max(-range)
}

impl WingDynamics {
    pub fn prepare(&mut self, sample_rate: f64) {
        self.sr = sample_rate as f32;
        self.rms_coef = coef(10.0, self.sr);
        self.reset();
    }

    pub fn reset(&mut self) {
        self.model = None;
        self.cfg = None;
        self.comp = [CompState::default(); 2];
        self.gate = GateState { gain: 1.0, ..Default::default() };
        self.sc.reset();
        self.sc_settings = None;
        self.xo.reset();
        self.xo_settings = None;
        for f in &mut self.aux_filters {
            f.reset();
        }
        self.aux_env = [0.0; 4];
        self.aux_gain = [0.0; 2];
        self.wave = [0.0; 4];
        self.ride_gain = 0.0;
        self.ride_ms = 0.0;
        self.ride_hold = 0;
        self.warm_lp = [0.0; 2];
    }

    fn update(&mut self, model: usize, values: &[f32], sc: &SidechainSettings, xo: &XoverSettings) {
        let Some(spec) = DYN_MODELS.get(model) else { return };
        let n = spec.params.len().min(MAX_DYN_PARAMS);
        let changed = self.model != Some(model) || self.values[..n] != values[..n.min(values.len())];
        if changed {
            if self.model != Some(model) {
                self.comp = [CompState::default(); 2];
                self.gate = GateState { gain: 1.0, ..Default::default() };
                for f in &mut self.aux_filters {
                    f.reset();
                }
            }
            self.values[..n].copy_from_slice(&values[..n.min(values.len())]);
            self.model = Some(model);
            let cfg = configure(spec, &self.values[..n], self.sr);
            self.configure_aux(&cfg);
            self.cfg = Some(cfg);
        }
        if self.sc_settings != Some(*sc) {
            let sr = self.sr as f64;
            let (f, q) = (sc.freq.clamp(20.0, 20000.0) as f64, sc.q.clamp(0.3, 10.0) as f64);
            self.sc.set_coefficients(match sc.filter {
                1 => BiquadCoeffs::low_pass(sr, f, std::f64::consts::FRAC_1_SQRT_2),
                2 => BiquadCoeffs::high_pass(sr, f, std::f64::consts::FRAC_1_SQRT_2),
                3 => BiquadCoeffs::band_pass(sr, f, q),
                4 => BiquadCoeffs::notch(sr, f, q),
                _ => BiquadCoeffs::identity(),
            });
            self.sc_settings = Some(*sc);
        }
        if self.xo_settings != Some(*xo) {
            let sr = self.sr as f64;
            let (f, q) = (xo.freq.clamp(20.0, 20000.0) as f64, xo.q.clamp(0.3, 10.0) as f64);
            self.xo.set_coefficients(match xo.kind {
                1 => BiquadCoeffs::low_pass(sr, f, std::f64::consts::FRAC_1_SQRT_2),
                2 => BiquadCoeffs::high_pass(sr, f, std::f64::consts::FRAC_1_SQRT_2),
                3 => BiquadCoeffs::high_pass_1(sr, f),
                4 => BiquadCoeffs::high_pass(sr, f, 0.5),
                5 => BiquadCoeffs::band_pass(sr, f, q),
                _ => BiquadCoeffs::identity(),
            });
            self.xo_settings = Some(*xo);
        }
    }

    /// Filters used by the de-esser and dynamic EQ.
    fn configure_aux(&mut self, cfg: &Config) {
        let sr = self.sr as f64;
        match *cfg {
            Config::DeEss { freq, .. } => {
                let f = freq as f64;
                self.aux_filters[0].set_coefficients(BiquadCoeffs::high_pass(sr, f, std::f64::consts::FRAC_1_SQRT_2));
                self.aux_filters[1].set_coefficients(BiquadCoeffs::high_pass(sr, f, std::f64::consts::FRAC_1_SQRT_2));
            }
            Config::DynEq(bands, n) => {
                for (i, b) in bands.iter().take(n).enumerate() {
                    let (f, q) = (b.freq as f64, b.q as f64);
                    let c = match b.filter {
                        2 => BiquadCoeffs::low_pass_1(sr, f),
                        3 => BiquadCoeffs::low_pass(sr, f, std::f64::consts::FRAC_1_SQRT_2),
                        4 => BiquadCoeffs::high_pass_1(sr, f),
                        5 => BiquadCoeffs::high_pass(sr, f, std::f64::consts::FRAC_1_SQRT_2),
                        _ => BiquadCoeffs::band_pass(sr, f, q),
                    };
                    self.aux_filters[i * 2].set_coefficients(c); // detection (mono)
                    self.aux_filters[i * 2 + 1].set_coefficients(c); // processing (per channel)
                }
            }
            _ => {}
        }
    }

    /// Processes `chans` (1 or 2 channels) in place.
    #[allow(clippy::needless_range_loop)] // both channels are indexed per sample
    ///
    /// * `key` - external key signal (another channel), or None for the input itself;
    /// * `mix` - 0..1 wet/dry, `makeup_db` - gain after the processor.
    #[allow(clippy::too_many_arguments)]
    pub fn process(
        &mut self,
        chans: &mut [&mut [f32]],
        key: Option<&[f32]>,
        model: usize,
        values: &[f32],
        sc: &SidechainSettings,
        xo: &XoverSettings,
        mix: f32,
        makeup_db: f32,
    ) -> DynStatus {
        self.update(model, values, sc, xo);
        let Some(cfg) = self.cfg else { return DynStatus::default() };
        let n = chans.iter().map(|c| c.len()).min().unwrap_or(0);
        let stereo = chans.len() > 1;
        let mix = mix.clamp(0.0, 1.0);
        let makeup = db_to_gain(makeup_db);
        let use_xo = xo.kind != 0;
        let depth = if xo.depth.abs() > 0.0 { xo.depth.abs() } else { f32::INFINITY };
        let mut status = DynStatus::default();

        for i in 0..n {
            let l = chans[0][i];
            let r = if stereo { chans[1][i] } else { l };
            let key_raw = match key {
                Some(k) => k.get(i).copied().unwrap_or(0.0),
                None => {
                    if stereo {
                        if l.abs() > r.abs() {
                            l
                        } else {
                            r
                        }
                    } else {
                        l
                    }
                }
            };
            let key_sample = self.sc.tick(key_raw, 0);

            let (gain_db, outs) = self.tick(&cfg, key_sample, [l, r], stereo, &mut status);
            let (mut yl, mut yr) = (outs[0], outs[1]);
            if let Some(g) = gain_db {
                let g = if use_xo { g.clamp(-depth, depth) } else { g };
                let gl = db_to_gain(g);
                if use_xo {
                    let bl = self.xo.tick(yl, 0);
                    yl += (gl - 1.0) * bl;
                    if stereo {
                        let br = self.xo.tick(yr, 1);
                        yr += (gl - 1.0) * br;
                    }
                } else {
                    yl *= gl;
                    yr *= gl;
                }
            }
            chans[0][i] = (l + mix * (yl - l)) * makeup;
            if stereo {
                chans[1][i] = (r + mix * (yr - r)) * makeup;
            }
        }
        status
    }

    /// One sample: returns the gain to apply (dB) and/or already processed samples.
    #[inline]
    fn tick(&mut self, cfg: &Config, key: f32, x: [f32; 2], stereo: bool, st: &mut DynStatus) -> (Option<f32>, [f32; 2]) {
        match *cfg {
            Config::Gate(g) => {
                let gain = self.gate_tick(&g, key, false);
                st.active |= gain < -1.0;
                st.gain_reduction_db = st.gain_reduction_db.max(-gain.min(0.0));
                (Some(gain), x)
            }
            Config::Duck(g) => {
                let gain = self.gate_tick(&g, key, true);
                st.active |= gain < -1.0;
                st.gain_reduction_db = st.gain_reduction_db.max(-gain);
                (Some(gain), x)
            }
            Config::Comp(c) => {
                let y = self.comp_tick(&c, 0, key, x, st);
                (None, y)
            }
            Config::Dual(a, b) => {
                let y = self.comp_tick(&a, 0, key, x, st);
                let k2 = if stereo { y[0].abs().max(y[1].abs()) } else { y[0] };
                let y = self.comp_tick(&b, 1, k2, y, st);
                (None, y)
            }
            Config::Combo(g, c) => {
                let gain = self.gate_tick(&g, key, false);
                let gl = db_to_gain(gain);
                let y = [x[0] * gl, x[1] * gl];
                let k2 = key * gl;
                st.gain_reduction_db = st.gain_reduction_db.max(-gain);
                (None, self.comp_tick(&c, 0, k2, y, st))
            }
            Config::DeEss { range, full_band, .. } => {
                let hf = self.aux_filters[0].tick(key, 0);
                let a = coef(1.0, self.sr);
                let r = coef(60.0, self.sr);
                let follow = |env: &mut f32, v: f32| {
                    let v = v.abs();
                    *env = if v > *env { a * *env + (1.0 - a) * v } else { r * *env + (1.0 - r) * v };
                };
                follow(&mut self.aux_env[0], hf);
                follow(&mut self.aux_env[1], key);
                // Reduce when sibilance dominates the signal (level independent).
                let ratio_db = gain_to_db(self.aux_env[0] + 1e-9) - gain_to_db(self.aux_env[1] + 1e-9);
                let reduction = ((ratio_db + 8.0) * 1.5).clamp(0.0, range);
                st.active |= reduction > 0.5;
                st.gain_reduction_db = st.gain_reduction_db.max(reduction);
                if full_band {
                    (Some(-reduction), x)
                } else {
                    let g = db_to_gain(-reduction);
                    let hl = self.aux_filters[1].tick(x[0], 0);
                    let hr = if stereo { self.aux_filters[1].tick(x[1], 1) } else { hl };
                    (None, [x[0] + (g - 1.0) * hl, x[1] + (g - 1.0) * hr])
                }
            }
            Config::DynEq(bands, count) => {
                let mut y = x;
                for (i, b) in bands.iter().take(count).enumerate() {
                    let det = if b.filter == 0 { key } else { self.aux_filters[i * 2].tick(key, 0) };
                    let lvl = gain_to_db(det.abs() + 1e-9);
                    let target =
                        if b.high { comp_curve(lvl, b.thr, b.slope, 3.0) } else { -comp_curve(-lvl, -b.thr, b.slope, 3.0) }
                            .clamp(-24.0, 24.0);
                    let g = &mut self.aux_gain[i];
                    let c = if target < *g { b.att } else { b.rel };
                    *g = c * *g + (1.0 - c) * target;
                    st.gain_reduction_db = st.gain_reduction_db.max(-g.min(0.0));
                    st.active |= g.abs() > 0.5;
                    let gl = db_to_gain(b.gain_db + *g);
                    let f = &mut self.aux_filters[i * 2 + 1];
                    let bl = f.tick(y[0], 0);
                    y[0] += (gl - 1.0) * bl;
                    if stereo {
                        let br = f.tick(y[1], 1);
                        y[1] += (gl - 1.0) * br;
                    }
                }
                (None, y)
            }
            Config::Wave { attack, sustain, gain } => {
                let g = self.wave_tick(key, attack, sustain, 1.0) + gain;
                st.active |= g.abs() > 0.5;
                st.gain_reduction_db = st.gain_reduction_db.max(-g.min(0.0));
                (Some(g), x)
            }
            Config::Ride { thr, target, speed_db_s, amount, hold, range } => {
                let k = key * key;
                let c = coef(300.0, self.sr);
                self.ride_ms = c * self.ride_ms + (1.0 - c) * k;
                let lvl = 10.0 * (self.ride_ms + 1e-12).log10();
                if lvl > thr {
                    self.ride_hold = hold;
                    let want = ((target - lvl) * amount).clamp(-range, range);
                    let step = speed_db_s / self.sr;
                    self.ride_gain += (want - self.ride_gain).clamp(-step, step);
                } else if self.ride_hold > 0 {
                    self.ride_hold -= 1;
                } else {
                    // Silence: drift back to unity slowly.
                    let step = speed_db_s * 0.25 / self.sr;
                    self.ride_gain -= self.ride_gain.clamp(-step, step);
                }
                st.active |= self.ride_gain.abs() > 0.5;
                st.gain_reduction_db = st.gain_reduction_db.max(-self.ride_gain.min(0.0));
                (Some(self.ride_gain), x)
            }
            Config::Warm { drive, harm, colour: tone, trim, mix } => {
                let pre = db_to_gain(drive * 18.0);
                let post = db_to_gain(trim) / pre.sqrt();
                let bias = harm * 0.3;
                let lp = (1.0 - (-2.0 * std::f32::consts::PI * 3000.0 / self.sr).exp()).clamp(0.0, 1.0);
                let mut y = x;
                for (ch, v) in y.iter_mut().enumerate().take(if stereo { 2 } else { 1 }) {
                    let s = ((*v * pre + bias).tanh() - bias.tanh()) * post;
                    // COLOR: negative darkens, positive brightens.
                    let state = &mut self.warm_lp[ch];
                    *state += lp * (s - *state);
                    let toned = s + tone * (s - *state);
                    *v += mix * (toned - *v);
                }
                if !stereo {
                    y[1] = y[0];
                }
                st.active |= drive > 0.2;
                (None, y)
            }
            Config::Lmt { speed, trans, trans_gain, trans_on, comp } => {
                let mut y = x;
                if trans_on {
                    let g = db_to_gain(self.wave_tick(key, trans * 1.2, -trans * 0.6, 0.3 + speed * 1.7) + trans_gain);
                    y = [y[0] * g, y[1] * g];
                }
                (None, self.comp_tick(&comp, 0, key, y, st))
            }
        }
    }

    /// Gate / expander / ducker gain in dB.
    #[inline]
    fn gate_tick(&mut self, g: &GateCfg, key: f32, duck: bool) -> f32 {
        let s = &mut self.gate;
        let v = key.abs();
        let lvl = if g.rms {
            let c = self.rms_coef;
            s.env = c * s.env + (1.0 - c) * v * v;
            10.0 * (s.env + 1e-12).log10()
        } else {
            // Fast peak follower: instant attack, 5 ms decay.
            let d = (-1.0 / (0.005 * self.sr)).exp();
            s.env = v.max(s.env * d);
            gain_to_db(s.env + 1e-9)
        };

        let target = if duck {
            if lvl >= g.thr {
                s.hold = g.hold;
                -g.range
            } else if s.hold > 0 {
                s.hold -= 1;
                -g.range
            } else {
                0.0
            }
        } else {
            let was_open = s.open;
            if lvl >= g.thr {
                s.open = true;
                s.hold = g.hold;
            } else if lvl < g.close {
                if s.hold > 0 {
                    s.hold -= 1;
                } else {
                    s.open = false;
                }
            }
            if s.open && !was_open {
                s.accent = g.accent_db;
            }
            if s.open {
                0.0
            } else if g.ratio > 0.0 {
                exp_curve(lvl, g.thr, g.ratio, 0.0, g.range)
            } else {
                -g.range
            }
        };

        // Gate: opening (gain rising) uses ATTACK, closing uses RELEASE.
        // Ducker: ducking (gain falling) uses ATTACK, recovering uses RELEASE.
        let current = gain_to_db(s.gain.max(1e-6));
        let rising = target > current;
        let c = if rising != duck { g.att } else { g.rel };
        let next = c * current + (1.0 - c) * target;
        s.gain = db_to_gain(next);
        let accent = s.accent;
        s.accent *= coef(30.0, self.sr);
        next + accent
    }

    /// Compressor/limiter/expander stage on samples `x`, keyed by `key`.
    #[inline]
    fn comp_tick(&mut self, c: &CompCfg, idx: usize, key: f32, x: [f32; 2], st: &mut DynStatus) -> [f32; 2] {
        if !c.enabled {
            return x;
        }
        let sr = self.sr;
        let rms_coef = self.rms_coef;
        let s = &mut self.comp[idx];
        let pre = db_to_gain(c.pre_db);
        let det_in = if c.feedback { s.last_out } else { key * pre };
        let lvl = if c.rms {
            s.ms = rms_coef * s.ms + (1.0 - rms_coef) * det_in * det_in;
            10.0 * (s.ms + 1e-12).log10()
        } else {
            gain_to_db(det_in.abs() + 1e-9)
        };

        let mut target = match c.expander {
            Some((ratio, range)) => exp_curve(lvl, c.thr, ratio, c.knee, range),
            None => comp_curve(lvl, c.thr, c.slope, c.knee),
        };
        target = target.max(-c.max_gr);

        // Release time (or rate) for this sample.
        let (rel_coef, rate) = match c.release {
            Release::Fixed => (c.rel, None),
            Release::Auto { max_ms } => {
                let k = coef(1000.0, sr);
                s.slow_gr = k * s.slow_gr + (1.0 - k) * (-s.gr).max(0.0);
                let ms = (c.rel_ms * (1.0 + s.slow_gr / 3.0)).min(max_ms);
                (coef(ms, sr), None)
            }
            Release::Opto => (c.rel, None),
            Release::Rate(db_s) => (c.rel, Some(db_s / sr)),
        };

        if target < s.gr {
            s.gr = c.att * s.gr + (1.0 - c.att) * target;
            s.hold = c.hold;
        } else if s.hold > 0 {
            s.hold -= 1;
        } else if let Some(step) = rate {
            s.gr = (s.gr + step).min(target);
        } else if c.linear_release {
            let lin = db_to_gain(s.gr);
            let next = rel_coef * lin + (1.0 - rel_coef) * db_to_gain(target);
            s.gr = gain_to_db(next.max(1e-6));
        } else {
            s.gr = rel_coef * s.gr + (1.0 - rel_coef) * target;
        }

        let mut gr = s.gr;
        if matches!(c.release, Release::Opto) {
            // Slow tail: the cell keeps part of the reduction after long compression.
            let (a, r) = (coef(500.0, sr), coef(2500.0, sr));
            let t = s.gr * 0.5;
            s.opto_slow = if t < s.opto_slow { a * s.opto_slow + (1.0 - a) * t } else { r * s.opto_slow + (1.0 - r) * t };
            gr = gr.min(s.opto_slow);
        }

        // The input gain only drives the detector and the colouration; the level
        // after the stage is set by `post_db` (models with input/output knobs).
        let g = db_to_gain(gr + c.post_db);
        let y_l = if c.colour > 0.0 { colour(x[0] * pre, c.colour) / pre } else { x[0] };
        let y_r = if c.colour > 0.0 { colour(x[1] * pre, c.colour) / pre } else { x[1] };
        let mut y = [y_l * g, y_r * g];

        if let Some((lthr, lrel)) = c.limiter {
            let peak = y[0].abs().max(y[1].abs());
            let over = (gain_to_db(peak + 1e-9) - lthr).max(0.0);
            let t = -over;
            s.lim_gr = if t < s.lim_gr { t } else { lrel * s.lim_gr + (1.0 - lrel) * t };
            let lg = db_to_gain(s.lim_gr);
            y = [y[0] * lg, y[1] * lg];
            gr += s.lim_gr;
        }

        s.last_out = y[0].abs().max(y[1].abs());
        st.gain_reduction_db = st.gain_reduction_db.max(-gr.min(0.0));
        st.active |= gr < -0.5;
        y
    }

    /// Transient designer gain (dB) from attack/sustain amounts.
    #[inline]
    fn wave_tick(&mut self, key: f32, attack: f32, sustain: f32, speed: f32) -> f32 {
        let v = key.abs();
        let sr = self.sr;
        let follow = |env: &mut f32, att_ms: f32, rel_ms: f32| {
            let c = if v > *env { coef(att_ms, sr) } else { coef(rel_ms, sr) };
            *env = c * *env + (1.0 - c) * v;
        };
        follow(&mut self.wave[0], 0.5 / speed, 30.0 / speed);
        follow(&mut self.wave[1], 15.0 / speed, 30.0 / speed);
        follow(&mut self.wave[2], 0.5 / speed, 40.0 / speed);
        follow(&mut self.wave[3], 0.5 / speed, 250.0 / speed);
        // Transients: fast envelope ahead of slow one. Sustain: slow-release tail above fast-release tail.
        let t = (gain_to_db(self.wave[0] + 1e-9) - gain_to_db(self.wave[1] + 1e-9)).clamp(0.0, 12.0) / 12.0;
        let s = (gain_to_db(self.wave[3] + 1e-9) - gain_to_db(self.wave[2] + 1e-9)).clamp(0.0, 12.0) / 12.0;
        attack * t + sustain * s
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wing::model_index;

    fn defaults(token: &str) -> (usize, [f32; MAX_DYN_PARAMS]) {
        let i = model_index(DYN_MODELS, token).unwrap();
        let mut v = [0.0; MAX_DYN_PARAMS];
        DYN_MODELS[i].defaults_into(&mut v);
        (i, v)
    }

    fn set(m: usize, v: &mut [f32], id: &str, x: f32) {
        v[DYN_MODELS[m].param_index(id).unwrap()] = x;
    }

    fn sine(amp: f32, freq: f32, n: usize) -> Vec<f32> {
        (0..n).map(|i| (i as f32 * 2.0 * std::f32::consts::PI * freq / 48000.0).sin() * amp).collect()
    }

    fn run(m: usize, v: &[f32], input: &[f32]) -> (Vec<f32>, DynStatus) {
        let mut d = WingDynamics::default();
        d.prepare(48000.0);
        let mut buf = input.to_vec();
        let mut status = DynStatus::default();
        for chunk in buf.chunks_mut(256) {
            status = d.process(&mut [chunk], None, m, v, &SidechainSettings::default(), &XoverSettings::default(), 1.0, 0.0);
        }
        (buf, status)
    }

    fn rms_tail(x: &[f32]) -> f32 {
        let tail = &x[x.len() / 2..];
        (tail.iter().map(|v| v * v).sum::<f32>() / tail.len() as f32).sqrt()
    }

    #[test]
    fn every_model_is_stable_on_loud_and_quiet_signals() {
        for (m, spec) in DYN_MODELS.iter().enumerate() {
            let mut v = [0.0; MAX_DYN_PARAMS];
            spec.defaults_into(&mut v);
            for amp in [0.0, 0.001, 0.5, 2.0] {
                let (out, _) = run(m, &v, &sine(amp, 200.0, 24000));
                assert!(out.iter().all(|x| x.is_finite()), "{} produced non-finite output", spec.token);
                assert!(out.iter().all(|x| x.abs() < 40.0), "{} blew up", spec.token);
            }
        }
    }

    #[test]
    fn gate_closes_on_quiet_input_and_opens_on_loud() {
        let (m, v) = defaults("GATE");
        let (quiet, s) = run(m, &v, &sine(0.001, 200.0, 48000)); // -60 dB < -40 dB threshold
        assert!(rms_tail(&quiet) < 0.001 * 0.01, "gate didn't close");
        assert!(s.active);
        let (loud, _) = run(m, &v, &sine(0.3, 200.0, 48000));
        assert!((rms_tail(&loud) / (0.3 / 2f32.sqrt()) - 1.0).abs() < 0.05, "gate didn't open");
    }

    #[test]
    fn compressor_follows_ratio() {
        let (m, mut v) = defaults("COMP");
        set(m, &mut v, "knee", 0.0);
        // 0 dBFS-ish sine RMS -3 dB; threshold -20 dB, 4:1 -> about 12.75 dB reduction.
        let ratio_index = [1.1, 1.2, 1.3, 1.5, 1.7, 2.0, 2.5, 3.0, 3.5, 4.0].iter().position(|r| *r == 4.0).unwrap();
        set(m, &mut v, "ratio", ratio_index as f32);
        let (out, s) = run(m, &v, &sine(1.0, 200.0, 96000));
        let gr = gain_to_db(rms_tail(&out) / (1.0 / 2f32.sqrt()));
        assert!((gr + 12.75).abs() < 1.5, "gain change {gr}");
        assert!(s.gain_reduction_db > 10.0);
    }

    #[test]
    fn compressors_reduce_loud_signals() {
        for token in [
            "COMP", "B160", "B560", "D241C", "ECL33", "9000C", "SBUS", "RED3", "LA", "F670", "BLISS", "NSTR", "2250", "E88C",
            "ONEC",
        ] {
            let (m, v) = defaults(token);
            let (_, s) = run(m, &v, &sine(1.0, 200.0, 48000));
            assert!(s.gain_reduction_db > 1.0, "{token} didn't compress ({} dB)", s.gain_reduction_db);
        }
        // The FET model compresses once driven.
        let (m, mut v) = defaults("76LA");
        set(m, &mut v, "in", 0.0);
        let (_, s) = run(m, &v, &sine(0.5, 200.0, 48000));
        assert!(s.gain_reduction_db > 3.0);
    }

    #[test]
    fn ducker_ducks_on_key() {
        let (m, v) = defaults("DUCK");
        let mut d = WingDynamics::default();
        d.prepare(48000.0);
        let key = sine(0.5, 300.0, 4800);
        let mut sig = vec![0.25f32; 4800];
        for _ in 0..10 {
            d.process(&mut [&mut sig[..]], Some(&key), m, &v, &SidechainSettings::default(), &XoverSettings::default(), 1.0, 0.0);
            sig.fill(0.25);
        }
        d.process(&mut [&mut sig[..]], Some(&key), m, &v, &SidechainSettings::default(), &XoverSettings::default(), 1.0, 0.0);
        assert!(sig[4000] < 0.25 * 0.5, "not ducked: {}", sig[4000]);
    }

    #[test]
    fn deesser_reduces_sibilance_only() {
        let (m, v) = defaults("DS902");
        let (low, s1) = run(m, &v, &sine(0.5, 200.0, 48000));
        assert!(s1.gain_reduction_db < 0.5);
        assert!((rms_tail(&low) / (0.5 / 2f32.sqrt()) - 1.0).abs() < 0.05);
        let (_, s2) = run(m, &v, &sine(0.5, 8000.0, 48000));
        assert!(s2.gain_reduction_db > 2.0);
    }

    #[test]
    fn mix_and_makeup() {
        let (m, v) = defaults("COMP");
        let mut d = WingDynamics::default();
        d.prepare(48000.0);
        let mut buf = sine(0.01, 200.0, 4800); // below threshold: unchanged
        let orig = buf.clone();
        d.process(&mut [&mut buf[..]], None, m, &v, &SidechainSettings::default(), &XoverSettings::default(), 0.5, 6.0);
        let g = buf[1000] / orig[1000];
        assert!((g - db_to_gain(6.0)).abs() < 0.01);
    }

    #[test]
    fn join_without_allocation() {
        let mut b = [0u8; 16];
        assert_eq!(join(&mut b, "1-", "thr"), "1-thr");
    }
}
