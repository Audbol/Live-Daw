//! WING dynamics models (the GATE and DYNAMICS slots): one realtime engine
//! with a handful of cores - gate/expander, ducker, compressor/limiter/
//! expander, de-esser, dynamic EQ, transient designer, level rider and
//! saturation - and a mapping from each of the 32 console models' controls
//! onto those cores.
//!
//! Model character comes from the gain computer (knee, ratios that grow with
//! level for the variable-mu and leveler models, "beyond infinity" ratios),
//! the detector (peak/RMS, feedback, the opto cell's HF sensitivity), the time
//! constants (fixed, stepped, program dependent, crest-factor driven,
//! multi-stage opto/auto release, constant-rate and anti-log release), gain
//! staging and a little analog colouration. Behaviour follows the model
//! descriptions in the WING operation manual (section 11.2; page numbers in
//! the comments below). Nothing here allocates.

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
    /// Opens at this level ...
    thr: f32,
    /// ... and closes below this one (hysteresis).
    close: f32,
    range: f32,
    /// Expansion ratio; 0 = gate.
    ratio: f32,
    /// Extra expansion per dB below the threshold (a ratio that grows with depth).
    growth: f32,
    att: f32,
    rel: f32,
    hold: u32,
    accent_db: f32,
    rms: bool,
}

/// Static gain computer of a compressor stage (gain change in dB, <= 0).
#[derive(Clone, Copy, Debug)]
enum Curve {
    /// Downward compressor; `slope` = 1/ratio - 1 (0 = none, -1 = limiting, < -1 beyond infinity).
    Comp { thr: f32, slope: f32, knee: f32 },
    /// Downward expander, limited to `range`.
    Expand { thr: f32, ratio: f32, knee: f32, range: f32 },
    /// Variable-mu: `r0`:1 at the threshold, the ratio growing with the level
    /// over it (`r0 * (1 + over / width)`), towards limiting.
    VarMu { thr: f32, r0: f32, width: f32, knee: f32 },
}

impl Curve {
    #[inline]
    fn gain(&self, x: f32) -> f32 {
        match *self {
            Curve::Comp { thr, slope, knee } => comp_curve(x, thr, slope, knee),
            Curve::Expand { thr, ratio, knee, range } => exp_curve(x, thr, ratio, knee, range),
            Curve::VarMu { thr, r0, width, knee } => {
                let e = knee_over(x - thr, knee);
                (width / r0) * (e / width).ln_1p() - e
            }
        }
    }
}

/// Attack time behaviour.
#[derive(Clone, Copy, Debug)]
enum Attack {
    /// The set time.
    Fixed,
    /// Program dependent: `slow_ms` for small changes, down to `fast_ms` for a
    /// step of `full_db` or more.
    Program { fast_ms: f32, slow_ms: f32, full_db: f32 },
    /// Shortened on transients (high crest factor), down to `min_scale` of the set time.
    Crest { min_scale: f32 },
}

/// Release (recovery) behaviour.
#[derive(Clone, Copy, Debug)]
enum Release {
    /// The set time.
    Fixed,
    /// The more reduction there has been, the longer: the set time after
    /// none, up to `max_ms` after `full_db` or more.
    Auto { max_ms: f32, full_db: f32 },
    /// Two stage: deep reduction recovers at `fast_ms`, the last few dB at the
    /// set time (`split_db` sets the depth where the fast stage takes over).
    TwoStage { fast_ms: f32, split_db: f32 },
    /// Slower stages that charge during sustained reduction and hold part of
    /// it back (opto cells, multi time-constant auto release circuits).
    Stages([Stage; 2]),
    /// Constant rate in dB/s (a straight line on a dB scale).
    Rate(f32),
    /// Slow at first, then accelerating back to unity; average rate in dB/s.
    AntiLog(f32),
    /// Follows the crest factor: shorter on transient material.
    Crest,
}

#[derive(Clone, Copy, Debug, Default)]
struct Stage {
    /// Share of the reduction the stage holds (0 = unused).
    frac: f32,
    /// Coefficients while charging (more reduction) and releasing.
    charge: f32,
    rel: f32,
}

#[derive(Clone, Copy, Debug)]
struct CompCfg {
    curve: Curve,
    att: f32,
    att_ms: f32,
    rel: f32,
    rel_ms: f32,
    attack: Attack,
    release: Release,
    hold: u32,
    rms: bool,
    /// LIN envelope: the gain moves on a linear scale instead of in dB.
    lin_env: bool,
    feedback: bool,
    /// HF emphasis of the detector (0 = flat).
    det_tilt: f32,
    pre_db: f32,
    post_db: f32,
    colour: f32,
    max_gr: f32,
    /// Makeup that follows the average gain reduction.
    auto_gain: bool,
    /// Peak limiter after the compressor: (threshold dB, release coef).
    limiter: Option<(f32, f32)>,
    enabled: bool,
}

impl CompCfg {
    fn uses_crest(&self) -> bool {
        matches!(self.attack, Attack::Crest { .. }) || matches!(self.release, Release::Crest)
    }
}

impl Default for CompCfg {
    fn default() -> Self {
        Self {
            curve: Curve::Comp { thr: 0.0, slope: 0.0, knee: 0.0 },
            att: 0.0,
            att_ms: 10.0,
            rel: 0.0,
            rel_ms: 100.0,
            attack: Attack::Fixed,
            release: Release::Fixed,
            hold: 0,
            rms: false,
            lin_env: false,
            feedback: false,
            det_tilt: 0.0,
            pre_db: 0.0,
            post_db: 0.0,
            colour: 0.0,
            max_gr: f32::INFINITY,
            auto_gain: false,
            limiter: None,
            enabled: true,
        }
    }
}

/// One band of the dynamic EQ (p.95): the band's gain moves from 0 dB toward
/// `gain_db` as the level passes the threshold.
#[derive(Clone, Copy, Debug)]
struct DeqBand {
    thr: f32,
    /// dB of gain change per dB past the threshold (1 - 1/ratio).
    amount: f32,
    att: f32,
    rel: f32,
    /// 0 = flat (full range), 1 = BP (bell), 2/3 = LP6/LP12 (low shelf), 4/5 = HP6/HP12 (high shelf).
    filter: usize,
    /// Largest gain change; its sign picks cut or boost.
    gain_db: f32,
    freq: f32,
    q: f32,
    /// true = acts above the threshold, false = below.
    above: bool,
}

impl DeqBand {
    /// Gain (dB) the band heads for at detector level `lvl`.
    #[inline]
    fn gain_for(&self, lvl: f32) -> f32 {
        let past = if self.above { lvl - self.thr } else { self.thr - lvl };
        let amount = knee_over(past, 3.0) * self.amount;
        amount.min(self.gain_db.abs()).copysign(self.gain_db)
    }
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

/// ratio -> slope for the gain computer (999 = infinity; negative = beyond
/// infinity, -4 meaning 1 dB down for every 4 dB over, as on the BDX 560, p.108).
fn slope_of(ratio: f32) -> f32 {
    if ratio >= 999.0 || ratio == 0.0 {
        -1.0
    } else {
        1.0 / ratio - 1.0
    }
}

/// Slope a feedback detector needs for `ratio` (it sees the reduced output).
/// Limited so the loop stays stable at the fastest attack.
fn feedback_slope(ratio: f32) -> f32 {
    if ratio >= 999.0 {
        -9.0
    } else {
        (1.0 - ratio).clamp(-9.0, 0.0)
    }
}

/// dBu-calibrated thresholds of analog models: +18 dBu = 0 dBFS.
fn dbu(x: f32) -> f32 {
    x - 18.0
}

fn configure(spec: &ModelSpec, v: &[f32], sr: f32) -> Config {
    let p = Params::new(spec, v);
    let comp = |thr: f32, ratio: f32, knee: f32, att_ms: f32, rel_ms: f32| CompCfg {
        curve: Curve::Comp { thr, slope: slope_of(ratio), knee },
        att: coef(att_ms, sr),
        att_ms,
        rel: coef(rel_ms, sr),
        rel_ms,
        ..Default::default()
    };
    let stage = |frac: f32, charge_ms: f32, rel_ms: f32| Stage { frac, charge: coef(charge_ms, sr), rel: coef(rel_ms, sr) };
    let gate = |thr: f32, range: f32, ratio: f32, att_ms: f32, hold_ms: f32, rel_ms: f32| GateCfg {
        thr,
        close: thr,
        range,
        ratio,
        growth: 0.0,
        att: coef(att_ms, sr),
        rel: coef(rel_ms, sr),
        hold: (hold_ms * 0.001 * sr) as u32,
        accent_db: 0.0,
        rms: false,
    };
    // LA-style opto leveler, shared by LA and CMB (p.113-114): the cell is
    // program dependent and frequency sensitive, and releases in stages - most
    // of the reduction quickly, the rest progressively slower. COMPRESS is
    // about 3:1, LIMIT an infinite ratio.
    let leveler = |gain: f32, peak: f32, limit: bool| CompCfg {
        release: Release::Stages([stage(0.4, 400.0, 1500.0), stage(0.2, 3000.0, 8000.0)]),
        rms: true,
        det_tilt: 0.5,
        colour: 0.12,
        post_db: (gain - 50.0) * 0.4,
        ..comp(-peak * 0.4, if limit { 999.0 } else { 3.0 }, if limit { 6.0 } else { 10.0 }, 10.0, 60.0)
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
            // Opens HYST dB above the threshold and closes at it; FAST is 50 us
            // instead of 500 us (p.123). Fully counter-clockwise, HYST reads
            // EXP: an expander (panel, p.122).
            let thr = p.f("thr") - if p.on("m40") { 40.0 } else { 0.0 };
            let hyst = p.f("hyst");
            let ratio = if hyst < 0.25 { 2.0 } else { 0.0 };
            let mut g = gate(thr + hyst, p.f("range"), ratio, if p.on("fast") { 0.05 } else { 0.5 }, 10.0, p.f("rel"));
            g.close = thr;
            Config::Gate(g)
        }
        "9000G" => {
            // 2:1 expander or gate; attack 0.1 ms (FAST) or 1.5 ms (p.122).
            let ratio = if p.text("mode") == "EXP" { 2.0 } else { 0.0 };
            Config::Gate(gate(p.f("thr"), p.f("range"), ratio, if p.on("fast") { 0.1 } else { 1.5 }, p.f("hld"), p.f("rel")))
        }
        "D241G" => {
            // A gentle expander near the threshold that turns into a gate further
            // below it; SLOW changes the release only (p.123).
            let mut g = gate(p.f("thr"), 80.0, 1.5, 0.2, 50.0, if p.on("slow") { 500.0 } else { 100.0 });
            g.growth = 0.1;
            Config::Gate(g)
        }
        "PSE" | "CMB" => {
            // A plain gate; FAST and PEAK pick the time constants of Table 11.33 (p.125).
            let peak = p.on("peak");
            let (att, rel) = match (peak, p.on("fast")) {
                (false, true) => (100.0, 100.0),
                (false, false) => (200.0, 200.0),
                (true, true) => (20.0, 200.0),
                (true, false) => (20.0, 1000.0),
            };
            let mut g = gate(p.f("thr"), p.f("depth"), 0.0, att, 10.0, rel);
            g.rms = !peak;
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
                    amount: 1.0 - 1.0 / get("ratio").max(1.0),
                    att: coef(get("att"), sr),
                    rel: coef(get("rel"), sr),
                    filter,
                    gain_db: get("g"),
                    freq: get("f"),
                    q: get("q"),
                    above: text("mode") != "low",
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
            let (thr, knee) = (p.f("thr"), p.f("knee") * 3.0);
            let mut c = comp(thr, ratio, knee, p.f("att"), p.f("rel"));
            c.hold = (p.f("hld") * 0.001 * sr) as u32;
            c.rms = p.text("det") == "RMS";
            // ENVELOPE shapes attack, hold and release alike (p.101).
            c.lin_env = p.text("env") == "LIN";
            if p.on("auto") {
                // AUTO ENV: faster attack on transients, longer release after more reduction.
                c.attack = Attack::Crest { min_scale: 0.1 };
                c.release = Release::Auto { max_ms: (p.f("rel") * 6.0).clamp(p.f("rel"), 4000.0), full_db: 12.0 };
            }
            if spec.token == "EXP" {
                c.curve = Curve::Expand { thr, ratio, knee, range: 60.0 };
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
            // The soft knee starts at the threshold and reaches the full ratio
            // 10 dB above it; AUTO sets attack and release from the signal (p.108).
            let auto = p.on("auto");
            let mut c = comp(
                p.f("thr") + 5.0,
                p.f("ratio"),
                10.0,
                if auto { 10.0 } else { p.f("att") },
                if auto { 150.0 } else { p.f("rel") },
            );
            if auto {
                c.attack = Attack::Crest { min_scale: 0.1 };
                c.release = Release::Auto { max_ms: 1500.0, full_db: 12.0 };
            }
            c.limiter = Some((p.f("lim"), coef(p.f("lrel"), sr)));
            Config::Comp(c)
        }
        "ECL33" => {
            // Recovery auto 1: 40-800 ms, auto 2: 150-1500 ms, longer the more
            // reduction there is. Attack: compressor 3/6 ms, limiter 2/4 ms (p.111).
            let rec = |text: &str, fallback: f32| -> (f32, Release) {
                match text {
                    "A1" => (40.0, Release::Auto { max_ms: 800.0, full_db: 20.0 }),
                    "A2" => (150.0, Release::Auto { max_ms: 1500.0, full_db: 20.0 }),
                    t => (ms_choice(t, fallback), Release::Fixed),
                }
            };
            let (lrel, lmode) = rec(p.text("lrec"), 100.0);
            let mut limiter = comp(p.f("lthr"), 999.0, 0.0, if p.on("lfast") { 2.0 } else { 4.0 }, lrel);
            limiter.release = lmode;
            limiter.enabled = p.on("lon");
            let (crel, cmode) = rec(p.text("crec"), 400.0);
            let mut compressor = comp(p.f("cthr"), p.f("ratio"), 3.0, if p.on("cfast") { 3.0 } else { 6.0 }, crel);
            compressor.release = cmode;
            compressor.enabled = p.on("con");
            compressor.post_db = p.f("cgain");
            compressor.colour = 0.08;
            Config::Dual(compressor, limiter)
        }
        "9000C" => {
            // Program-dependent attack of 3-30 ms, or 3 ms with FAST; soft knee,
            // hard with PEAK (p.109-110).
            let fast = p.on("fast");
            let knee = if p.on("peak") { 0.0 } else { 8.0 };
            let mut c = comp(dbu(p.f("thr")), p.f("ratio"), knee, if fast { 3.0 } else { 30.0 }, p.f("rel"));
            if !fast {
                c.attack = Attack::Program { fast_ms: 3.0, slow_ms: 30.0, full_db: 20.0 };
            }
            c.rms = true;
            c.colour = 0.05;
            Config::Comp(c)
        }
        "SBUS" => {
            // AUTO release is two stage: deep reduction recovers quickly, the
            // last few dB slowly (p.110).
            let rel_text = p.text("rel");
            let auto = rel_text == "AUTO";
            let rel_ms = if auto { 1200.0 } else { ms_choice(rel_text, 0.4) * 1000.0 };
            let mut c = comp(p.f("thr"), p.f("ratio"), 1.0, p.f("att"), rel_ms);
            if auto {
                c.release = Release::TwoStage { fast_ms: 100.0, split_db: 6.0 };
            }
            c.colour = 0.05;
            Config::Comp(c)
        }
        "RED3" => {
            // Known for a highly transparent sound (no colouration); AUTO as on
            // the WING compressor (p.109).
            let mut c = comp(p.f("thr"), p.f("ratio"), 4.0, p.f("att"), p.f("rel"));
            if p.on("auto") {
                c.attack = Attack::Crest { min_scale: 0.1 };
                c.release = Release::Auto { max_ms: (p.f("rel") * 6.0).min(4000.0), full_db: 12.0 };
            }
            Config::Comp(c)
        }
        "76LA" => {
            // FET limiter: the input knob drives a fixed threshold; knobs at 7 are
            // fastest. Release 1.1 s (1) .. 50 ms (7), attack 800 us .. 20 us.
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
            // Variable-mu: 2:1 at the threshold, the ratio rising towards limiting
            // as the level goes further over; lowering DC BIAS from 1.00 widens
            // the knee until it reads as a gentler initial ratio (p.114-115).
            let pos = p.f("time").round() as i32;
            let (att, rel) = match pos {
                1 => (0.2, 300.0),
                2 => (0.2, 800.0),
                3 => (0.4, 2000.0),
                4 => (0.8, 5000.0),
                5 => (0.2, 2000.0),
                _ => (0.4, 300.0),
            };
            let thr = -p.f("thr") * 4.0;
            let mut c = comp(thr, 2.0, 0.0, att, rel);
            c.curve = Curve::VarMu { thr, r0: 2.0, width: 10.0, knee: 3.0 + 27.0 * (1.0 - p.f("bias")) };
            // Positions 5 and 6 release in stages: individual transients fast,
            // repeated peaks after ~10 s (and, on 6, sustained level after ~25 s).
            match pos {
                5 => c.release = Release::Stages([stage(0.6, 1000.0, 10000.0), Stage::default()]),
                6 => c.release = Release::Stages([stage(0.6, 500.0, 10000.0), stage(0.3, 5000.0, 25000.0)]),
                _ => {}
            }
            // INPUT GAIN attenuates the signal before the tube stage (more drive,
            // more colour); unity gain at its default of -10 dB.
            c.pre_db = p.f("in") + 10.0;
            c.post_db = c.pre_db;
            c.rms = true;
            c.colour = 0.2;
            Config::Comp(c)
        }
        "BLISS" => {
            // Negative ratios set the output slope: -4 = 4 dB down for every dB
            // over (p.112). AUTO FAST shortens the attack on loud transients;
            // the release is linear (constant rate in dB), ANTI LOG makes it
            // slow at first, then accelerating.
            let r = p.f("ratio");
            let slope = if r < 0.0 { r - 1.0 } else { slope_of(r) };
            let thr = p.f("thr");
            let mut c = comp(thr, 2.0, 2.0, p.f("att"), p.f("rel"));
            c.curve = Curve::Comp { thr, slope, knee: 2.0 };
            if p.on("afast") {
                c.attack = Attack::Crest { min_scale: 0.1 };
            }
            let rate = 10.0 / (p.f("rel") * 0.001);
            c.release = if p.on("alog") { Release::AntiLog(rate) } else { Release::Rate(rate) };
            if p.on("glon") {
                c.max_gr = -p.f("glim");
            }
            Config::Comp(c)
        }
        "NSTR" => {
            // Soft knee except at 20:1 and NUKE; 10:1 behaves like an optical
            // compressor, with a release of up to 20 s (p.115-116).
            let text = p.text("ratio");
            let nuke = text == "NUKE";
            let ratio: f32 = if nuke { 999.0 } else { text.trim_end_matches(":1").parse().unwrap_or(4.0) };
            let opto = (ratio - 10.0).abs() < 0.01;
            let hard = nuke || ratio >= 20.0;
            let att = if nuke {
                0.05
            } else if opto {
                dial_log(p.f("att"), 5.0, 30.0)
            } else {
                dial_log(p.f("att"), 0.05, 30.0)
            };
            let rel = dial_log(p.f("rel"), 50.0, 3500.0);
            let mut c = comp(-20.0, ratio, if hard { 0.0 } else { 8.0 }, att, rel);
            if opto {
                c.release = Release::Stages([stage(0.4, 500.0, dial_log(p.f("rel"), 1000.0, 20000.0)), Stage::default()]);
            }
            c.pre_db = p.f("in") * 4.0;
            c.post_db = (p.f("out") - 5.0) * 4.0;
            c.colour = 0.1;
            Config::Comp(c)
        }
        "2250" => {
            // Threshold +10..-20 (dBu, panel), program-dependent attack, OLD =
            // feedback detection (smoother), NEW = feed-forward (p.116).
            let amount = p.f("ratio") / 10.0;
            let ratio = if amount >= 0.999 { 999.0 } else { 1.0 / (1.0 - amount) };
            let base = match p.text("att") {
                "FAST" => 1.0,
                "SLOW" => 30.0,
                _ => 10.0,
            };
            let thr = dbu(10.0 - 3.0 * p.f("thr"));
            let knee = if p.text("knee") == "SOFT" { 10.0 } else { 0.0 };
            let mut c = comp(thr, ratio, knee, base, p.f("rel"));
            c.attack = Attack::Program { fast_ms: base / 3.0, slow_ms: base, full_db: 15.0 };
            if p.text("type") == "OLD" {
                c.feedback = true;
                c.curve = Curve::Comp { thr, slope: feedback_slope(ratio), knee };
            }
            Config::Comp(c)
        }
        "L100" => {
            // GAIN is the output gain; GAIN REDUCTION lowers the threshold and the
            // ratio grows with the reduction; the release lengthens with more
            // reduction (p.117).
            let speed = |t: &str, f: f32, m: f32, s: f32| match t {
                "FAST" => f,
                "SLOW" => s,
                _ => m,
            };
            let thr = -p.f("gr") * 4.0;
            let rel = speed(p.text("rel"), 100.0, 400.0, 1500.0);
            let mut c = comp(thr, 2.0, 6.0, speed(p.text("att"), 2.0, 10.0, 40.0), rel);
            c.curve = Curve::VarMu { thr, r0: 2.0, width: 8.0, knee: 6.0 };
            c.release = Release::Auto { max_ms: rel * 4.0, full_db: 12.0 };
            c.post_db = (p.f("ingain") - 5.0) * 3.0;
            c.rms = true;
            c.colour = 0.1;
            Config::Comp(c)
        }
        "E88C" => {
            // Double time-constant auto attack (1.5-5 ms, FAST 0.1-5 ms); the
            // ratio knob ends at LIM and the release knob at AUTO, a triple
            // time-constant release (p.105 and panel).
            let thr = dbu(p.f("thr")) - if p.on("thrpull") { 20.0 } else { 0.0 };
            let ratio = if p.f("ratio") >= 9.95 { 999.0 } else { p.f("ratio") };
            let auto = p.f("rel") >= 2990.0;
            let fast = p.text("att") == "FAST";
            let knee = if p.text("knee") == "SOFT" { 6.0 } else { 0.0 };
            let mut c = comp(thr, ratio, knee, 5.0, if auto { 80.0 } else { p.f("rel") });
            c.attack = Attack::Program { fast_ms: if fast { 0.1 } else { 1.5 }, slow_ms: 5.0, full_db: 15.0 };
            if auto {
                c.release = Release::Stages([stage(0.5, 300.0, 700.0), stage(0.25, 2000.0, 3000.0)]);
            }
            c.colour = 0.05;
            Config::Comp(c)
        }
        "LMT" => {
            let mut c = comp(-p.f("comp") * 0.3, 4.0, 6.0, 5.0, 200.0);
            c.release = Release::Auto { max_ms: 1500.0, full_db: 12.0 };
            c.post_db = p.f("cgain");
            c.enabled = p.on("con");
            Config::Lmt { speed: p.f("tspd"), trans: p.f("trans"), trans_gain: p.f("tgain"), trans_on: p.on("ton"), comp: c }
        }
        "ONEC" => {
            // Attack and release follow the crest factor; AUTO GAIN makes up for
            // the reduction (p.106).
            let amount = p.f("gr");
            let mut c = comp(-amount * 3.5, 1.5 + amount * 0.35, 6.0, 10.0, 300.0);
            c.attack = Attack::Crest { min_scale: 0.05 };
            c.release = Release::Crest;
            c.auto_gain = p.on("dag");
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
    /// Recent amount of reduction (dB, >= 0) for program-dependent release.
    amount: f32,
    /// Slow release stages.
    stage: [f32; 2],
    lim_gr: f32,
    last_out: f32,
    /// Detector HF-emphasis filter.
    tilt_lp: f32,
    /// Crest factor envelopes: peak and mean square.
    peak: f32,
    power: f32,
    /// Reduction when the release began (anti-log release).
    rel_from: f32,
    /// Auto gain (dB).
    makeup: f32,
}

#[derive(Clone, Copy, Default)]
struct GateState {
    env: f32,
    gain: f32,
    hold: u32,
    open: bool,
    accent: f32,
}

/// Fixed per-sample-rate coefficients (no exp() per sample for these).
#[derive(Clone, Copy, Default)]
struct Coefs {
    rms: f32,
    gate_peak: f32,
    accent: f32,
    tilt: f32,
    crest: f32,
    amount_charge: f32,
    amount_rel: f32,
    makeup: f32,
    deq_env: f32,
    deess_att: f32,
    deess_rel: f32,
    ride: f32,
}

impl Coefs {
    fn new(sr: f32) -> Self {
        Self {
            rms: coef(10.0, sr),
            gate_peak: coef(5.0, sr),
            accent: coef(30.0, sr),
            // One-pole low pass at 1 kHz: its complement is the detector's HF emphasis.
            tilt: 1.0 - (-2.0 * std::f32::consts::PI * 1000.0 / sr).exp(),
            crest: coef(200.0, sr),
            amount_charge: coef(100.0, sr),
            amount_rel: coef(1000.0, sr),
            makeup: coef(1500.0, sr),
            deq_env: coef(15.0, sr),
            deess_att: coef(1.0, sr),
            deess_rel: coef(60.0, sr),
            ride: coef(300.0, sr),
        }
    }
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
    k: Coefs,
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
            k: Coefs::default(),
        };
        d.prepare(48000.0);
        d
    }
}

/// Level over the threshold through a soft knee `knee` dB wide (0 below it,
/// quadratic inside it).
#[inline]
fn knee_over(over: f32, knee: f32) -> f32 {
    if knee > 0.0 && over.abs() <= knee * 0.5 {
        (over + knee * 0.5).powi(2) / (2.0 * knee)
    } else {
        over.max(0.0)
    }
}

/// Static compressor curve: gain change in dB (<= 0) for a level in dB.
#[inline]
fn comp_curve(x: f32, thr: f32, slope: f32, knee: f32) -> f32 {
    slope * knee_over(x - thr, knee)
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

/// One-pole step from `cur` toward `target` in dB, or on a linear scale.
#[inline]
fn smooth(cur: f32, target: f32, c: f32, linear: bool) -> f32 {
    if linear {
        gain_to_db((c * db_to_gain(cur) + (1.0 - c) * db_to_gain(target)).max(1e-6))
    } else {
        c * cur + (1.0 - c) * target
    }
}

/// Moves `cur` toward `target` by at most `step`.
#[inline]
fn step_toward(cur: f32, target: f32, step: f32) -> f32 {
    cur + (target - cur).clamp(-step, step)
}

impl WingDynamics {
    pub fn prepare(&mut self, sample_rate: f64) {
        self.sr = sample_rate as f32;
        self.k = Coefs::new(self.sr);
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
                self.aux_env = [0.0; 4];
                self.aux_gain = [0.0; 2];
                self.wave = [0.0; 4];
                self.ride_gain = 0.0;
                self.ride_ms = 0.0;
                self.ride_hold = 0;
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
                        0 => BiquadCoeffs::identity(), // flat: full-range processing
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
                let y = self.comp_tick(&c, 0, key, x, 1.0, st);
                (None, y)
            }
            Config::Dual(a, b) => {
                let y = self.comp_tick(&a, 0, key, x, 1.0, st);
                let k2 = if stereo { y[0].abs().max(y[1].abs()) } else { y[0] };
                let y = self.comp_tick(&b, 1, k2, y, 1.0, st);
                (None, y)
            }
            Config::Combo(g, c) => {
                let gain = self.gate_tick(&g, key, false);
                let gl = db_to_gain(gain);
                let y = [x[0] * gl, x[1] * gl];
                let k2 = key * gl;
                st.gain_reduction_db = st.gain_reduction_db.max(-gain);
                (None, self.comp_tick(&c, 0, k2, y, 1.0, st))
            }
            Config::DeEss { range, full_band, .. } => {
                let hf = self.aux_filters[0].tick(key, 0);
                let (a, r) = (self.k.deess_att, self.k.deess_rel);
                let follow = |env: &mut f32, v: f32| {
                    let v = v.abs();
                    *env = if v > *env { a * *env + (1.0 - a) * v } else { r * *env + (1.0 - r) * v };
                };
                follow(&mut self.aux_env[0], hf);
                follow(&mut self.aux_env[1], key);
                // Reduce when sibilance dominates the signal (level independent, p.123).
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
                let rel_env = self.k.deq_env;
                let mut y = x;
                for (i, b) in bands.iter().take(count).enumerate() {
                    // Detector: peak follower on the band (flat: the whole key).
                    let det = if b.filter == 0 { key } else { self.aux_filters[i * 2].tick(key, 0) };
                    let a = det.abs();
                    let env = &mut self.aux_env[i];
                    *env = if a > *env { a } else { rel_env * *env + (1.0 - rel_env) * a };
                    let want = if b.gain_db == 0.0 { 0.0 } else { b.gain_for(gain_to_db(*env + 1e-9)) };
                    // ATTACK moves the gain away from 0 dB, RELEASE back toward it (p.95).
                    let g = &mut self.aux_gain[i];
                    let c = if want.abs() > g.abs() { b.att } else { b.rel };
                    *g = c * *g + (1.0 - c) * want;
                    st.gain_reduction_db = st.gain_reduction_db.max(-g.min(0.0));
                    st.active |= g.abs() > 0.5;
                    let k = db_to_gain(*g) - 1.0;
                    if b.filter == 0 {
                        y = [y[0] * (1.0 + k), y[1] * (1.0 + k)];
                    } else {
                        let f = &mut self.aux_filters[i * 2 + 1];
                        let bl = f.tick(y[0], 0);
                        y[0] += k * bl;
                        if stereo {
                            let br = f.tick(y[1], 1);
                            y[1] += k * br;
                        }
                    }
                }
                (None, y)
            }
            Config::Wave { attack, sustain, gain } => {
                let (t, s) = self.wave_env(key, 1.0);
                let g = attack * t + sustain * s + gain;
                st.active |= g.abs() > 0.5;
                st.gain_reduction_db = st.gain_reduction_db.max(-g.min(0.0));
                (Some(g), x)
            }
            Config::Ride { thr, target, speed_db_s, amount, hold, range } => {
                let k = key * key;
                let c = self.k.ride;
                self.ride_ms = c * self.ride_ms + (1.0 - c) * k;
                let lvl = 10.0 * (self.ride_ms + 1e-12).log10();
                if lvl > thr {
                    self.ride_hold = hold;
                    // Logarithmic curve: larger steps at first, flattening out
                    // toward RANGE (p.120).
                    let want = range * ((target - lvl) * amount / range).tanh();
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
                // T GAIN is the shaper's output level and works on its own; the
                // shaper itself (TRANSIENTS: more or less reduction during
                // attacks) works only with the compressor on (p.106).
                let tg = if trans_on { db_to_gain(trans_gain) } else { 1.0 };
                let scale = if trans_on && comp.enabled {
                    let (t, _) = self.wave_env(key * tg, 0.3 + speed * 1.7);
                    1.0 - trans / 10.0 * t
                } else {
                    1.0
                };
                (None, self.comp_tick(&comp, 0, key * tg, [x[0] * tg, x[1] * tg], scale, st))
            }
        }
    }

    /// Gate / expander / ducker gain in dB.
    #[inline]
    fn gate_tick(&mut self, g: &GateCfg, key: f32, duck: bool) -> f32 {
        let k = self.k;
        let s = &mut self.gate;
        let v = key.abs();
        let lvl = if g.rms {
            s.env = k.rms * s.env + (1.0 - k.rms) * v * v;
            10.0 * (s.env + 1e-12).log10()
        } else {
            // Fast peak follower: instant attack, 5 ms decay.
            s.env = v.max(s.env * k.gate_peak);
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
                let under = (g.close - lvl).max(0.0);
                (-(g.ratio - 1.0) * under - g.growth * under * under).max(-g.range)
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
        s.accent *= k.accent;
        next + accent
    }

    /// Compressor/limiter/expander stage on samples `x`, keyed by `key`.
    /// `gr_scale` scales the stage's reduction (the LMT transient shaper).
    #[inline]
    fn comp_tick(&mut self, c: &CompCfg, idx: usize, key: f32, x: [f32; 2], gr_scale: f32, st: &mut DynStatus) -> [f32; 2] {
        if !c.enabled {
            return x;
        }
        let sr = self.sr;
        let k = self.k;
        let s = &mut self.comp[idx];
        let pre = db_to_gain(c.pre_db);
        let mut det = if c.feedback { s.last_out } else { key * pre };
        if c.det_tilt > 0.0 {
            s.tilt_lp += k.tilt * (det - s.tilt_lp);
            det += c.det_tilt * (det - s.tilt_lp);
        }
        let lvl = if c.rms {
            s.ms = k.rms * s.ms + (1.0 - k.rms) * det * det;
            10.0 * (s.ms + 1e-12).log10()
        } else {
            gain_to_db(det.abs() + 1e-9)
        };
        // Crest factor (peak over RMS) as a time scale: 1 for a sine, smaller
        // the more transient the signal.
        let crest_scale = if c.uses_crest() {
            let a = det.abs();
            s.peak = if a > s.peak { a } else { s.peak * k.crest };
            s.power = k.crest * s.power + (1.0 - k.crest) * a * a;
            let crest_db = 10.0 * ((s.peak * s.peak + 1e-12) / (s.power + 1e-12)).log10();
            10f32.powf(-(crest_db - 3.0).clamp(0.0, 30.0) / 10.0)
        } else {
            1.0
        };

        let target = c.curve.gain(lvl).max(-c.max_gr);
        // Compressors attack when the gain falls; an expander's attack restores
        // the gain (p.103).
        let expander = matches!(c.curve, Curve::Expand { .. });
        let attacking = (target < s.gr) != expander;
        let amount = (-s.gr).max(0.0);
        let ka = if amount > s.amount { k.amount_charge } else { k.amount_rel };
        s.amount = ka * s.amount + (1.0 - ka) * amount;

        if attacking {
            let a = match c.attack {
                Attack::Fixed => c.att,
                Attack::Program { fast_ms, slow_ms, full_db } => {
                    let t = ((s.gr - target).abs() / full_db).min(1.0);
                    coef(slow_ms * (fast_ms / slow_ms).powf(t), sr)
                }
                Attack::Crest { min_scale } => coef(c.att_ms * crest_scale.max(min_scale), sr),
            };
            s.gr = smooth(s.gr, target, a, c.lin_env);
            s.hold = c.hold;
            s.rel_from = s.gr;
        } else if s.hold > 0 {
            s.hold -= 1;
            s.rel_from = s.gr;
        } else {
            match c.release {
                Release::Rate(db_s) => s.gr = step_toward(s.gr, target, db_s / sr),
                Release::AntiLog(db_s) => {
                    let done = if s.rel_from < -1e-3 { (1.0 - s.gr / s.rel_from).clamp(0.0, 1.0) } else { 1.0 };
                    s.gr = step_toward(s.gr, target, db_s / sr * (0.25 + 2.5 * done));
                }
                _ => {
                    let r = match c.release {
                        Release::Auto { max_ms, full_db } => {
                            coef(c.rel_ms * (max_ms / c.rel_ms).powf((s.amount / full_db).min(1.0)), sr)
                        }
                        Release::TwoStage { fast_ms, split_db } => {
                            coef(c.rel_ms * (fast_ms / c.rel_ms).powf((amount / split_db).min(1.0)), sr)
                        }
                        Release::Crest => coef(c.rel_ms * crest_scale.max(0.1), sr),
                        _ => c.rel,
                    };
                    s.gr = smooth(s.gr, target, r, c.lin_env);
                }
            }
        }

        let mut gr = s.gr;
        if let Release::Stages(stages) = c.release {
            let base = s.gr;
            for (v, stg) in s.stage.iter_mut().zip(stages.iter()) {
                if stg.frac <= 0.0 {
                    continue;
                }
                let t = base * stg.frac;
                let a = if t < *v { stg.charge } else { stg.rel };
                *v = a * *v + (1.0 - a) * t;
                gr = gr.min(*v);
            }
        }
        let gr = (gr * gr_scale).min(0.0);
        let mut post = c.post_db;
        if c.auto_gain {
            s.makeup = k.makeup * s.makeup + (1.0 - k.makeup) * -gr;
            post += s.makeup;
        }

        // The input gain only drives the detector and the colouration; the level
        // after the stage is set by `post_db` (models with input/output knobs).
        let g = db_to_gain(gr + post);
        let y_l = if c.colour > 0.0 { colour(x[0] * pre, c.colour) / pre } else { x[0] };
        let y_r = if c.colour > 0.0 { colour(x[1] * pre, c.colour) / pre } else { x[1] };
        let mut y = [y_l * g, y_r * g];

        let mut total = gr;
        if let Some((lthr, lrel)) = c.limiter {
            let peak = y[0].abs().max(y[1].abs());
            let over = (gain_to_db(peak + 1e-9) - lthr).max(0.0);
            let t = -over;
            s.lim_gr = if t < s.lim_gr { t } else { lrel * s.lim_gr + (1.0 - lrel) * t };
            let lg = db_to_gain(s.lim_gr);
            y = [y[0] * lg, y[1] * lg];
            total += s.lim_gr;
        }

        s.last_out = y[0].abs().max(y[1].abs());
        st.gain_reduction_db = st.gain_reduction_db.max(-total.min(0.0));
        st.active |= total < -0.5;
        y
    }

    /// Transient designer envelopes: (transient, sustain) amounts, 0..1 each.
    #[inline]
    fn wave_env(&mut self, key: f32, speed: f32) -> (f32, f32) {
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
        (t, s)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wing::{model_index, ParamKind};

    const SR: f32 = 48000.0;

    fn defaults(token: &str) -> (usize, [f32; MAX_DYN_PARAMS]) {
        let i = model_index(DYN_MODELS, token).unwrap();
        let mut v = [0.0; MAX_DYN_PARAMS];
        DYN_MODELS[i].defaults_into(&mut v);
        (i, v)
    }

    fn set(m: usize, v: &mut [f32], id: &str, x: f32) {
        v[DYN_MODELS[m].param_index(id).unwrap()] = x;
    }

    /// Sets a choice (or a Values item) by its text / number.
    fn pick(m: usize, v: &mut [f32], id: &str, item: &str) {
        let i = DYN_MODELS[m].param_index(id).unwrap();
        v[i] = match DYN_MODELS[m].params[i].kind {
            ParamKind::Choice(items) => items.iter().position(|t| *t == item).unwrap(),
            ParamKind::Values(items) => {
                let x: f32 = item.parse().unwrap();
                items.iter().position(|t| (*t - x).abs() < 1e-3).unwrap()
            }
            _ => panic!("{id} is not a choice"),
        } as f32;
    }

    /// A model's values with some parameters set (numbers as values, text as choices).
    fn model(token: &str, sets: &[(&str, &str)]) -> (usize, [f32; MAX_DYN_PARAMS]) {
        let (m, mut v) = defaults(token);
        for (id, x) in sets {
            let i = DYN_MODELS[m].param_index(id).unwrap();
            match DYN_MODELS[m].params[i].kind {
                ParamKind::Choice(_) | ParamKind::Values(_) => pick(m, &mut v, id, x),
                _ => v[i] = x.parse().unwrap(),
            }
        }
        (m, v)
    }

    fn config(token: &str, sets: &[(&str, &str)]) -> Config {
        let (m, v) = model(token, sets);
        configure(&DYN_MODELS[m], &v[..DYN_MODELS[m].params.len()], SR)
    }

    fn comp_cfg(token: &str, sets: &[(&str, &str)]) -> CompCfg {
        match config(token, sets) {
            Config::Comp(c) => c,
            other => panic!("{token}: not a compressor: {other:?}"),
        }
    }

    fn gate_cfg(token: &str, sets: &[(&str, &str)]) -> GateCfg {
        match config(token, sets) {
            Config::Gate(g) => g,
            other => panic!("{token}: not a gate: {other:?}"),
        }
    }

    fn sine(amp: f32, freq: f32, n: usize) -> Vec<f32> {
        (0..n).map(|i| (i as f32 * 2.0 * std::f32::consts::PI * freq / 48000.0).sin() * amp).collect()
    }

    fn run_on(d: &mut WingDynamics, m: usize, v: &[f32], input: &[f32]) -> (Vec<f32>, DynStatus) {
        let mut buf = input.to_vec();
        let mut status = DynStatus::default();
        for chunk in buf.chunks_mut(256) {
            status = d.process(&mut [chunk], None, m, v, &SidechainSettings::default(), &XoverSettings::default(), 1.0, 0.0);
        }
        (buf, status)
    }

    fn run(m: usize, v: &[f32], input: &[f32]) -> (Vec<f32>, DynStatus) {
        let mut d = WingDynamics::default();
        d.prepare(48000.0);
        run_on(&mut d, m, v, input)
    }

    fn rms_tail(x: &[f32]) -> f32 {
        let tail = &x[x.len() / 2..];
        (tail.iter().map(|v| v * v).sum::<f32>() / tail.len() as f32).sqrt()
    }

    /// Steady-state gain (dB) of a model on a sine.
    fn sine_gain(m: usize, v: &[f32], amp: f32, freq: f32, n: usize) -> f32 {
        let input = sine(amp, freq, n);
        let (out, _) = run(m, v, &input);
        gain_to_db(rms_tail(&out) / rms_tail(&input))
    }

    /// Samples until the first compressor stage's reduction is above `level` dB,
    /// starting from `from` dB, on silence.
    fn recovery_samples(token: &str, sets: &[(&str, &str)], from: f32, level: f32) -> usize {
        let (m, v) = model(token, sets);
        let mut d = WingDynamics::default();
        d.prepare(48000.0);
        run_on(&mut d, m, &v, &[0.0; 64]);
        d.comp[0].gr = from;
        d.comp[0].amount = -from;
        d.comp[0].rel_from = from;
        let mut buf = [0.0f32; 1];
        for n in 0..(48000 * 30) {
            run_on(&mut d, m, &v, &buf);
            buf[0] = 0.0;
            if d.comp[0].gr > level {
                return n;
            }
        }
        usize::MAX
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
        pick(m, &mut v, "ratio", "4");
        let (out, s) = run(m, &v, &sine(1.0, 200.0, 96000));
        let gr = gain_to_db(rms_tail(&out) / (1.0 / 2f32.sqrt()));
        assert!((gr + 12.75).abs() < 1.5, "gain change {gr}");
        assert!(s.gain_reduction_db > 10.0);
    }

    #[test]
    fn compressors_reduce_loud_signals() {
        for token in [
            "COMP", "B160", "B560", "D241C", "ECL33", "9000C", "SBUS", "RED3", "LA", "F670", "BLISS", "NSTR", "2250", "L100",
            "E88C", "ONEC",
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

    // ---- dynamic EQ (p.95, p.100) ---------------------------------------------------------

    #[test]
    fn dynamic_eq_flat_filter_is_full_range_and_gain_is_the_limit() {
        // FLAT: the whole signal, not a bell. GAIN -6 caps the reduction.
        let (m, v) = model("DEQ", &[("filt", "OFF"), ("mode", "high"), ("thr", "-40"), ("ratio", "10"), ("g", "-6")]);
        for f in [100.0, 3000.0, 10000.0] {
            let g = sine_gain(m, &v, 0.5, f, 24000);
            assert!((g + 6.0).abs() < 0.3, "{g} dB at {f} Hz");
        }
        // A bell only works around its frequency.
        let (m, v) =
            model("DEQ", &[("filt", "BP"), ("f", "3000"), ("mode", "high"), ("thr", "-40"), ("ratio", "10"), ("g", "-6")]);
        assert!((sine_gain(m, &v, 0.5, 3000.0, 24000) + 6.0).abs() < 0.3);
        assert!(sine_gain(m, &v, 0.5, 100.0, 24000).abs() < 0.3);
    }

    #[test]
    fn dynamic_eq_gain_and_mode_pick_the_four_processors() {
        let loud = 0.5; // about -6 dB, above a -30 dB threshold
        let quiet = 0.003; // about -50 dB, below it
        let cases = [
            ("high", "-6", loud, -6.0), // downward compressor
            ("high", "-6", quiet, 0.0), //   ... idle below the threshold
            ("low", "-6", quiet, -6.0), // downward expander
            ("low", "-6", loud, 0.0),   //   ... idle above
            ("low", "6", quiet, 6.0),   // upward compressor
            ("high", "6", loud, 6.0),   // upward expander
            ("high", "0", loud, 0.0),   // GAIN 0: nothing to do
        ];
        for (mode, gain, amp, want) in cases {
            let (m, v) = model("DEQ", &[("filt", "OFF"), ("mode", mode), ("thr", "-30"), ("ratio", "10"), ("g", gain)]);
            let g = sine_gain(m, &v, amp, 1000.0, 24000);
            assert!((g - want).abs() < 0.3, "{mode} {gain} at {amp}: {g} dB, want {want}");
        }
    }

    #[test]
    fn dynamic_eq_ratio_sets_the_change_per_db() {
        // 2:1, 10 dB past the threshold -> 5 dB of reduction (inside GAIN).
        let (m, v) = model("DEQ", &[("filt", "OFF"), ("mode", "high"), ("thr", "-30"), ("ratio", "2"), ("g", "-15")]);
        let g = sine_gain(m, &v, db_to_gain(-20.0), 1000.0, 24000);
        assert!((g + 5.0).abs() < 0.6, "{g}");
    }

    #[test]
    fn dual_dynamic_eq_bands_work_independently() {
        let (m, v) = model(
            "DEQ2",
            &[
                ("1-filt", "BP"),
                ("1-f", "200"),
                ("1-thr", "-40"),
                ("1-ratio", "10"),
                ("1-g", "-6"),
                ("1-mode", "high"),
                ("1-q", "2"),
                ("2-filt", "BP"),
                ("2-f", "5000"),
                ("2-thr", "-40"),
                ("2-ratio", "10"),
                ("2-g", "4"),
                ("2-mode", "high"),
                ("2-q", "2"),
            ],
        );
        assert!((sine_gain(m, &v, 0.5, 200.0, 24000) + 6.0).abs() < 0.5);
        assert!((sine_gain(m, &v, 0.5, 5000.0, 24000) - 4.0).abs() < 0.5);
        // Defaults (GAIN 0) are transparent.
        let (m, v) = defaults("DEQ2");
        let input = sine(0.5, 1000.0, 4800);
        assert_eq!(run(m, &v, &input).0, input);
    }

    // ---- gates (p.104, p.122-125) ---------------------------------------------------------

    #[test]
    fn even88_gate_opens_hyst_above_the_threshold() {
        // THR -30, HYST 10: opens at -20, closes at -30 (p.123).
        let (m, v) = model("E88", &[("thr", "-30"), ("hyst", "10"), ("range", "40"), ("rel", "100")]);
        let mut d = WingDynamics::default();
        d.prepare(48000.0);
        let between = sine(db_to_gain(-25.0), 1000.0, 9600);
        let (out, _) = run_on(&mut d, m, &v, &between);
        assert!(rms_tail(&out) < rms_tail(&between) * 0.1, "opened below THR + HYST");
        run_on(&mut d, m, &v, &sine(db_to_gain(-15.0), 1000.0, 9600));
        let (out, _) = run_on(&mut d, m, &v, &between);
        assert!((rms_tail(&out) / rms_tail(&between) - 1.0).abs() < 0.05, "closed above THR");
    }

    #[test]
    fn gate_attack_times_follow_the_manual() {
        let g = gate_cfg("E88", &[("fast", "1")]);
        assert_eq!(g.att, coef(0.05, SR));
        assert_eq!(gate_cfg("E88", &[("fast", "0")]).att, coef(0.5, SR));
        assert_eq!(gate_cfg("9000G", &[("fast", "0")]).att, coef(1.5, SR));
        assert_eq!(gate_cfg("9000G", &[("fast", "1")]).att, coef(0.1, SR));
        // EXP at the hysteresis knob's end.
        assert_eq!(gate_cfg("E88", &[("hyst", "0")]).ratio, 2.0);
        assert_eq!(gate_cfg("E88", &[("hyst", "5")]).ratio, 0.0);
        // Draw More 241: SLOW changes the release only.
        let (fast, slow) = (gate_cfg("D241G", &[("slow", "0")]), gate_cfg("D241G", &[("slow", "1")]));
        assert_eq!(fast.att, slow.att);
        assert_eq!(fast.hold, slow.hold);
        assert!(slow.rel > fast.rel);
    }

    #[test]
    fn source_extractor_time_constants_follow_table_11_33() {
        for (peak, fast, att, rel) in
            [("0", "1", 100.0, 100.0), ("0", "0", 200.0, 200.0), ("1", "1", 20.0, 200.0), ("1", "0", 20.0, 1000.0)]
        {
            let g = gate_cfg("PSE", &[("peak", peak), ("fast", fast)]);
            assert_eq!((g.att, g.rel), (coef(att, SR), coef(rel, SR)), "peak {peak} fast {fast}");
            assert_eq!(g.rms, peak == "0");
            assert_eq!(g.ratio, 0.0, "a plain gate");
        }
    }

    #[test]
    fn draw_more_gate_expands_gently_then_gates() {
        let (m, v) = model("D241G", &[("thr", "-40")]);
        let near = -sine_gain(m, &v, db_to_gain(-43.0), 1000.0, 48000);
        let far = -sine_gain(m, &v, db_to_gain(-60.0), 1000.0, 48000);
        assert!(near > 0.5 && near < 4.5, "3 dB below: {near} dB");
        assert!(far > 40.0, "20 dB below: {far} dB");
        assert!(near / 3.0 < far / 20.0 * 0.6, "the ratio grows with depth: {near} / {far}");
    }

    // ---- compressors and expander ----------------------------------------------------------

    #[test]
    fn expander_attack_restores_and_release_reduces() {
        let (m, v) = model("EXP", &[("thr", "-40"), ("ratio", "2"), ("knee", "0"), ("att", "1"), ("hld", "1"), ("rel", "1000")]);
        let mut d = WingDynamics::default();
        d.prepare(48000.0);
        run_on(&mut d, m, &v, &sine(0.5, 1000.0, 4800));
        // Quiet: the reduction builds up at the RELEASE time (1 s, slow) ...
        run_on(&mut d, m, &v, &sine(0.001, 1000.0, 2400));
        let after_50ms = d.comp[0].gr;
        assert!(after_50ms > -3.0, "expander closed too fast: {after_50ms}");
        run_on(&mut d, m, &v, &sine(0.001, 1000.0, 48000 * 4));
        assert!(d.comp[0].gr < -15.0, "{}", d.comp[0].gr);
        // ... and goes away at the ATTACK time (1 ms, fast).
        run_on(&mut d, m, &v, &sine(0.5, 1000.0, 480));
        assert!(d.comp[0].gr > -1.0, "{}", d.comp[0].gr);
    }

    #[test]
    fn lin_envelope_shapes_the_attack_too() {
        let gr_after = |env: &str| {
            let (m, v) =
                model("COMP", &[("thr", "-40"), ("ratio", "100"), ("knee", "0"), ("att", "10"), ("det", "PEAK"), ("env", env)]);
            let mut d = WingDynamics::default();
            d.prepare(48000.0);
            run_on(&mut d, m, &v, &vec![0.5; 240]);
            d.comp[0].gr
        };
        let (lin, log) = (gr_after("LIN"), gr_after("LOG"));
        assert!(lin > log + 2.0, "LIN {lin} vs LOG {log}");
    }

    #[test]
    fn soul9000_peak_switch_selects_the_hard_knee() {
        // THR 0 dBu = -18 dBFS; a sine 1 dB below it (RMS).
        let amp = db_to_gain(-19.0 + 3.0);
        let (m, soft) = model("9000C", &[("thr", "0"), ("ratio", "4"), ("peak", "0")]);
        let (_, hard) = model("9000C", &[("thr", "0"), ("ratio", "4"), ("peak", "1")]);
        let input = sine(amp, 1000.0, 24000);
        assert!(run(m, &soft, &input).1.gain_reduction_db > 0.25);
        assert!(run(m, &hard, &input).1.gain_reduction_db < 0.05);
        let slow = comp_cfg("9000C", &[("fast", "0")]).attack;
        assert!(matches!(slow, Attack::Program { fast_ms, slow_ms, .. } if fast_ms == 3.0 && slow_ms == 30.0));
        assert_eq!(comp_cfg("9000C", &[("fast", "1")]).att, coef(3.0, SR));
    }

    #[test]
    fn soul_bus_auto_release_recovers_deep_reduction_first() {
        let sets = [("rel", "AUTO")];
        let deep = recovery_samples("SBUS", &sets, -12.0, -6.0);
        let shallow = recovery_samples("SBUS", &sets, -2.0, -1.0);
        assert!(deep < shallow / 2, "12->6 dB in {deep}, 2->1 dB in {shallow}");
    }

    #[test]
    fn even_comp_auto_recovery_grows_with_reduction() {
        for (rec, lo, hi) in [("A1", 40.0, 800.0), ("A2", 150.0, 1500.0)] {
            let little = recovery_samples("ECL33", &[("crec", rec)], -1.0, -0.37) as f32 / 48.0;
            let lots = recovery_samples("ECL33", &[("crec", rec)], -20.0, -7.4) as f32 / 48.0;
            assert!(little < lots, "{rec}: {little} ms vs {lots} ms");
            assert!(little >= lo * 0.8 && lots <= hi * 1.2, "{rec}: {little} ms .. {lots} ms");
        }
        assert!(matches!(config("ECL33", &[("cfast", "0")]), Config::Dual(a, _) if a.att == coef(6.0, SR)));
        assert!(matches!(config("ECL33", &[("cfast", "1")]), Config::Dual(a, _) if a.att == coef(3.0, SR)));
    }

    #[test]
    fn leveler_limit_is_an_infinite_ratio() {
        let swing = |mode: &str| {
            let (m, v) = model("LA", &[("peak", "60"), ("mode", mode)]);
            let a = sine_gain(m, &v, 0.5, 200.0, 48000);
            let b = sine_gain(m, &v, 0.5 * db_to_gain(10.0), 200.0, 48000);
            10.0 + b - a // output change for +10 dB in
        };
        let (lim, cmp) = (swing("LIM"), swing("COMP"));
        assert!(lim < 1.0, "LIMIT: +{lim} dB out for +10 dB in");
        assert!((2.5..4.5).contains(&cmp), "COMPRESS: +{cmp} dB out");
    }

    #[test]
    fn leveler_releases_in_stages() {
        // Most of the reduction comes back quickly, the rest slowly.
        let (m, v) = model("LA", &[("peak", "80")]);
        let mut d = WingDynamics::default();
        d.prepare(48000.0);
        run_on(&mut d, m, &v, &sine(0.8, 200.0, 48000 * 4));
        let full = -(d.comp[0].gr.min(d.comp[0].stage[0]).min(d.comp[0].stage[1]));
        let held = |d: &WingDynamics| -(d.comp[0].gr.min(d.comp[0].stage[0]).min(d.comp[0].stage[1]));
        run_on(&mut d, m, &v, &vec![0.0; 9600]); // 200 ms
        let fast = held(&d);
        assert!(fast < full * 0.5 && fast > 0.5, "after 200 ms: {fast} of {full} dB");
        run_on(&mut d, m, &v, &vec![0.0; 48000]);
        let later = held(&d);
        assert!(later < fast && later > 0.05, "after 1.2 s: {later} dB");
    }

    #[test]
    fn fair_kid_follows_its_manual_page() {
        // INPUT GAIN: unity at -10 dB, otherwise it changes the level too.
        let (m, unity) = model("F670", &[("thr", "0"), ("in", "-10")]);
        let (_, hot) = model("F670", &[("thr", "0"), ("in", "-4")]);
        assert!(sine_gain(m, &unity, 0.01, 1000.0, 24000).abs() < 0.1);
        assert!((sine_gain(m, &hot, 0.01, 1000.0, 24000) - 6.0).abs() < 0.2);
        // Variable mu: 2:1 at the threshold, the ratio rising with the level.
        let c = comp_cfg("F670", &[("thr", "5"), ("bias", "1")]);
        let thr = -20.0;
        let slope = |x: f32| c.curve.gain(x + 0.5) - c.curve.gain(x - 0.5);
        assert!((slope(thr + 2.0) + 0.5).abs() < 0.1, "{}", slope(thr + 2.0));
        assert!(slope(thr + 30.0) < -0.8, "{}", slope(thr + 30.0));
        // Lower DC BIAS: a wider knee (more reduction just below the threshold).
        let wide = comp_cfg("F670", &[("thr", "5"), ("bias", "0")]);
        assert!(wide.curve.gain(thr - 4.0) < c.curve.gain(thr - 4.0) - 0.5);
        // TIME CONSTANT positions.
        for (pos, att, rel) in
            [(1, 0.2, 300.0), (2, 0.2, 800.0), (3, 0.4, 2000.0), (4, 0.8, 5000.0), (5, 0.2, 2000.0), (6, 0.4, 300.0)]
        {
            let c = comp_cfg("F670", &[("time", &pos.to_string())]);
            assert_eq!((c.att_ms, c.rel_ms), (att, rel), "position {pos}");
            assert_eq!(matches!(c.release, Release::Stages(_)), pos >= 5, "position {pos}");
        }
        // DC BIAS defaults to 1.00 (Factory Cal).
        let (m, v) = defaults("F670");
        assert_eq!(v[DYN_MODELS[m].param_index("bias").unwrap()], 1.0);
    }

    #[test]
    fn eternal_bliss_ratios_release_and_auto_fast() {
        let slope = |r: &str| match comp_cfg("BLISS", &[("ratio", r)]).curve {
            Curve::Comp { slope, .. } => slope,
            _ => unreachable!(),
        };
        // -4: 4 dB below the threshold for every dB above it (p.112).
        assert!((slope("-4") + 5.0).abs() < 1e-4);
        assert!(slope("-0.3") > slope("-1") && slope("-1") > slope("-4"), "beyond infinity grows clockwise");
        assert!((slope("10") + 0.9).abs() < 1e-4);
        assert!(matches!(comp_cfg("BLISS", &[("afast", "1")]).attack, Attack::Crest { .. }));
        // Linear release vs anti-log (slow first, then accelerating).
        let lin = recovery_samples("BLISS", &[("rel", "150"), ("alog", "0")], -20.0, -18.0);
        let alog = recovery_samples("BLISS", &[("rel", "150"), ("alog", "1")], -20.0, -18.0);
        assert!(alog > lin * 2, "first 2 dB: linear {lin}, anti-log {alog}");
        let alog_all = recovery_samples("BLISS", &[("rel", "150"), ("alog", "1")], -20.0, -0.5);
        let lin_all = recovery_samples("BLISS", &[("rel", "150"), ("alog", "0")], -20.0, -0.5);
        assert!(alog_all < lin_all * 2, "anti-log finishes: {alog_all} vs {lin_all}");
    }

    #[test]
    fn no_stressor_knee_and_opto_mode() {
        let knee = |r: &str| match comp_cfg("NSTR", &[("ratio", r)]).curve {
            Curve::Comp { knee, .. } => knee,
            _ => unreachable!(),
        };
        for r in ["1.5:1", "2:1", "3:1", "4:1", "6:1", "10:1"] {
            assert!(knee(r) > 0.0, "{r} should be soft");
        }
        assert_eq!(knee("20:1"), 0.0);
        assert_eq!(knee("NUKE"), 0.0);
        assert!(matches!(comp_cfg("NSTR", &[("ratio", "10:1")]).release, Release::Stages(_)));
        assert!(matches!(comp_cfg("NSTR", &[("ratio", "4:1")]).release, Release::Fixed));
    }

    #[test]
    fn pia2250_threshold_scale_and_detection_types() {
        let thr = |k: &str| match comp_cfg("2250", &[("thr", k), ("type", "NEW")]).curve {
            Curve::Comp { thr, .. } => thr,
            _ => unreachable!(),
        };
        assert_eq!(thr("0"), dbu(10.0));
        assert_eq!(thr("10"), dbu(-20.0));
        // OLD (feedback) and NEW (feed-forward) give about the same static ratio.
        let (m, old) = model("2250", &[("thr", "8"), ("ratio", "7.5"), ("type", "OLD")]);
        let (_, new) = model("2250", &[("thr", "8"), ("ratio", "7.5"), ("type", "NEW")]);
        let (a, b) = (sine_gain(m, &old, 0.5, 200.0, 96000), sine_gain(m, &new, 0.5, 200.0, 96000));
        assert!(a < -6.0 && b < -6.0 && (a - b).abs() < 3.0, "OLD {a} dB, NEW {b} dB");
        assert!(matches!(comp_cfg("2250", &[]).attack, Attack::Program { .. }));
    }

    #[test]
    fn lta100_gain_is_its_output_gain() {
        let (m, a) = model("L100", &[("ingain", "5"), ("gr", "6")]);
        let (_, b) = model("L100", &[("ingain", "8"), ("gr", "6")]);
        let loud = sine(0.8, 200.0, 48000);
        let (sa, sb) = (run(m, &a, &loud).1, run(m, &b, &loud).1);
        assert!((sa.gain_reduction_db - sb.gain_reduction_db).abs() < 0.05, "GAIN changed the reduction");
        assert!((sine_gain(m, &b, 0.001, 1000.0, 24000) - 9.0).abs() < 0.2);
        // The ratio grows with the reduction.
        let c = comp_cfg("L100", &[("gr", "5")]);
        let slope = |x: f32| c.curve.gain(x + 0.5) - c.curve.gain(x - 0.5);
        assert!(slope(-16.0) > slope(5.0) + 0.15, "{} vs {}", slope(-16.0), slope(5.0));
        assert!(matches!(c.release, Release::Auto { .. }));
    }

    #[test]
    fn even88_comp_lim_auto_and_attack_ranges() {
        let c = comp_cfg("E88C", &[("ratio", "10"), ("rel", "3000"), ("att", "FAST")]);
        assert!(matches!(c.curve, Curve::Comp { slope, .. } if slope == -1.0));
        assert!(matches!(c.release, Release::Stages(_)));
        assert!(matches!(c.attack, Attack::Program { fast_ms, slow_ms, .. } if fast_ms == 0.1 && slow_ms == 5.0));
        let c = comp_cfg("E88C", &[("ratio", "4"), ("rel", "300"), ("att", "SLOW")]);
        assert!(matches!(c.curve, Curve::Comp { slope, .. } if (slope + 0.75).abs() < 1e-4));
        assert!(matches!(c.release, Release::Fixed));
        assert!(matches!(c.attack, Attack::Program { fast_ms, slow_ms, .. } if fast_ms == 1.5 && slow_ms == 5.0));
    }

    #[test]
    fn draw_more_comp_knee_starts_at_the_threshold() {
        let c = comp_cfg("D241C", &[("thr", "-20"), ("ratio", "4")]);
        assert_eq!(c.curve.gain(-20.0), 0.0);
        assert!(c.curve.gain(-19.0) < 0.0);
        // Full ratio from 10 dB above the threshold.
        let d = c.curve.gain(-5.0) - c.curve.gain(-10.0);
        assert!((d + 3.75).abs() < 1e-3, "{d}");
    }

    #[test]
    fn listen_mic_shaper_needs_the_compressor() {
        // Compressor off: TRANSIENTS does nothing, T GAIN still works (p.106).
        let (m, v) = model("LMT", &[("con", "0"), ("ton", "1"), ("trans", "10"), ("tgain", "0")]);
        let input = sine(0.5, 200.0, 4800);
        assert_eq!(run(m, &v, &input).0, input);
        let (m, v) = model("LMT", &[("con", "0"), ("ton", "1"), ("tgain", "6")]);
        assert!((sine_gain(m, &v, 0.5, 200.0, 4800) - 6.0).abs() < 0.01);
        // With the compressor on, TRANSIENTS changes the reduction on attacks.
        let burst: Vec<f32> = (0..48000).map(|i| if i % 12000 < 2400 { 0.8 } else { 0.0 } * ((i as f32) * 0.03).sin()).collect();
        let peak_out = |trans: &str| {
            let (m, v) = model("LMT", &[("con", "1"), ("ton", "1"), ("trans", trans), ("comp", "80")]);
            let (out, _) = run(m, &v, &burst);
            out[24000..24480].iter().fold(0.0f32, |a, x| a.max(x.abs()))
        };
        assert!(peak_out("10") > peak_out("-10") * 1.2);
    }

    #[test]
    fn one_knob_auto_gain_makes_up_the_reduction() {
        let (m, off) = model("ONEC", &[("gr", "8"), ("dag", "0")]);
        let (_, on) = model("ONEC", &[("gr", "8"), ("dag", "1")]);
        let without = sine_gain(m, &off, 0.8, 200.0, 48000 * 6);
        let with = sine_gain(m, &on, 0.8, 200.0, 48000 * 6);
        assert!(without < -6.0, "{without}");
        assert!(with > without + 5.0 && with.abs() < 2.0, "{with}");
        assert!(matches!(comp_cfg("ONEC", &[]).release, Release::Crest));
    }

    #[test]
    fn auto_rider_curve_flattens_toward_range() {
        // 10 dB under the target at 2:1: half of it linearly, less on the log curve.
        let (m, v) = model("RIDE", &[("thr", "-54"), ("tgt", "-18"), ("ratio", "2"), ("range", "6"), ("spd", "50")]);
        let g = sine_gain(m, &v, db_to_gain(-28.0 + 3.0), 1000.0, 48000 * 4);
        let want = 6.0 * (5.0f32 / 6.0).tanh();
        assert!((g - want).abs() < 0.4, "{g} dB, want {want}");
    }

    #[test]
    fn crest_factor_speeds_up_the_attack_on_transients() {
        let c = comp_cfg("ONEC", &[]);
        let mut d = WingDynamics::default();
        d.prepare(48000.0);
        let mut st = DynStatus::default();
        // Steady sine: the crest-driven scale stays near 1.
        for i in 0..48000 {
            let x = (i as f32 * 0.05).sin() * 0.3;
            d.comp_tick(&c, 0, x, [x, x], 1.0, &mut st);
        }
        let s = d.comp[0];
        let crest = 10.0 * ((s.peak * s.peak) / s.power).log10();
        assert!((crest - 3.0).abs() < 0.5, "sine crest {crest}");
        // A click after silence reads as a large crest factor.
        for _ in 0..48000 {
            d.comp_tick(&c, 0, 0.0, [0.0; 2], 1.0, &mut st);
        }
        d.comp_tick(&c, 0, 0.9, [0.9; 2], 1.0, &mut st);
        let s = d.comp[0];
        assert!(10.0 * ((s.peak * s.peak) / s.power).log10() > 20.0);
    }
}
