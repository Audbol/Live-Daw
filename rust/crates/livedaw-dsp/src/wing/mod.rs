//! WING-style processing models: the parameter tables of every gate/dynamics,
//! EQ and filter-tool model, and their DSP.
//!
//! Every model is a list of [`ParamSpec`]s. A model's state is a flat `[f32]`
//! in that order: real values for continuous parameters, 0/1 for toggles and
//! the item index for choices. The DSP reads values by parameter id through
//! [`Params`], so the tables (generated from the console's parameter tree) are
//! the single source of truth for names, ranges and defaults.
//!
//! The models reproduce each processor's controls and characteristic behaviour
//! (detector, knee, time constants, curve shapes, colouration). They are not
//! circuit-level emulations.

pub mod dynamics;
pub mod eq;
pub mod filter;
pub mod fx;
mod models_gen;

pub use models_gen::{
    AUX_DYN_MODELS, DYN_MODELS, EQ_MODELS_BUS, EQ_MODELS_CH, FILTER_TOOLS, FX_MODELS, PROCESS_ORDERS, TAP_POINTS,
};

/// Parameter slots per dynamics model (the largest, DEQ2, uses 18).
pub const MAX_DYN_PARAMS: usize = 20;
/// Parameter slots per EQ model (the bus STD EQ uses 27).
pub const MAX_EQ_PARAMS: usize = 32;
/// Parameter slots per filter tool.
pub const MAX_TOOL_PARAMS: usize = 2;
/// Parameter slots per FX device (the graphic EQ and the channel strips use 33).
pub const MAX_FX_PARAMS: usize = 36;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ModelCategory {
    Gate,
    Compressor,
    Special,
    Eq,
    Filter,
    /// FX rack: an empty slot.
    FxNone,
    /// FX rack: a VST3/AU/LV2 plugin (the console's EXT device).
    FxExternal,
    /// FX rack: devices for slots 1-8 only.
    FxPremium,
    /// FX rack: devices for any slot.
    FxStandard,
    /// FX rack: channel strips (any slot).
    FxChannel,
}

impl ModelCategory {
    pub fn is_fx(self) -> bool {
        matches!(self, Self::FxNone | Self::FxExternal | Self::FxPremium | Self::FxStandard | Self::FxChannel)
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum ParamKind {
    Linear,
    Log,
    /// Value is the index of the choice.
    Choice(&'static [&'static str]),
    /// Value is the index into a list of numbers.
    Values(&'static [f32]),
    Toggle,
    Int,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ParamSpec {
    pub id: &'static str,
    pub label: &'static str,
    pub unit: &'static str,
    pub kind: ParamKind,
    /// May be greater than `max` (the knob runs the other way).
    pub min: f32,
    pub max: f32,
    pub steps: u32,
    pub default: f32,
}

impl ParamSpec {
    pub fn lo(&self) -> f32 {
        self.min.min(self.max)
    }

    pub fn hi(&self) -> f32 {
        self.min.max(self.max)
    }

    /// Clamps (and for discrete kinds rounds) a value into range.
    pub fn sanitise(&self, v: f32) -> f32 {
        let v = if v.is_finite() { v } else { self.default };
        let v = v.clamp(self.lo(), self.hi());
        match self.kind {
            ParamKind::Linear | ParamKind::Log => v,
            _ => v.round(),
        }
    }

    pub fn is_continuous(&self) -> bool {
        matches!(self.kind, ParamKind::Linear | ParamKind::Log)
    }

    /// Value at `pos` 0..1 along the control (log taper for log parameters).
    pub fn from_normalised(&self, pos: f32) -> f32 {
        let pos = pos.clamp(0.0, 1.0);
        let v = match self.kind {
            ParamKind::Log if self.min > 0.0 && self.max > 0.0 => self.min * (self.max / self.min).powf(pos),
            _ => self.min + (self.max - self.min) * pos,
        };
        self.sanitise(v)
    }

    pub fn to_normalised(&self, v: f32) -> f32 {
        let v = self.sanitise(v);
        let pos = match self.kind {
            ParamKind::Log if self.min > 0.0 && self.max > 0.0 => (v / self.min).ln() / (self.max / self.min).ln(),
            _ if self.max != self.min => (v - self.min) / (self.max - self.min),
            _ => 0.0,
        };
        pos.clamp(0.0, 1.0)
    }

    /// Text for a value, as the console shows it ("-20.0 dB", "1:2", "8k",
    /// "5k7 Hz", "1503 ms"; see [`ParamSpec::console_text`]).
    pub fn display(&self, v: f32) -> String {
        let v = self.sanitise(v);
        match self.kind {
            ParamKind::Choice(items) => items.get(v as usize).copied().unwrap_or("?").to_string(),
            ParamKind::Values(items) => {
                let x = items.get(v as usize).copied().unwrap_or(0.0);
                if x >= 999.0 {
                    "INF".into()
                } else {
                    format_number(x, self.unit)
                }
            }
            ParamKind::Toggle => if v >= 0.5 { "ON" } else { "OFF" }.into(),
            ParamKind::Int | ParamKind::Linear | ParamKind::Log => self.console_text(v),
        }
    }
}

impl ParamSpec {
    /// A number's text as the console's screens show it (the WING's own text
    /// for the parameter, as its remote also writes it, with the unit): fixed
    /// decimals per parameter, kHz with the k as the decimal point and times
    /// in ms however long ("5k7 Hz", "1.57 s", "0.95", "50 ms", "1503 ms";
    /// manual Fig. 124 and 132, WING Edit's DUCKER and DYNAMIC EQ).
    pub fn console_text(&self, v: f32) -> String {
        let v = self.sanitise(v);
        let number = match self.kind {
            ParamKind::Int => format!("{}", v as i64),
            ParamKind::Linear | ParamKind::Log => {
                let d = self.console_decimals().unwrap_or(if v.abs() >= 100.0 {
                    0
                } else if v.abs() >= 10.0 {
                    1
                } else {
                    2
                });
                if self.unit == "Hz" {
                    console_hz(v, d)
                } else {
                    format!("{v:.d$}")
                }
            }
            _ => return self.display(v),
        };
        if self.unit.is_empty() {
            number
        } else {
            format!("{number} {}", self.unit)
        }
    }

    /// The decimals the console gives a float parameter: as many as a linear
    /// step needs (0.1 -> 1, 0.25 -> 2, 5 -> 0); for a log one by its range
    /// (two below 1, one below 10, none from 10 up and for ms; tenths of a
    /// hertz for the fine EQ frequencies).
    fn console_decimals(&self) -> Option<usize> {
        if self.steps < 2 || self.max == self.min {
            return None;
        }
        let lo = self.lo();
        Some(match self.kind {
            ParamKind::Linear => {
                let step = (self.max - self.min).abs() as f64 / (self.steps - 1) as f64;
                (0..3)
                    .find(|&d| {
                        let x = step * 10f64.powi(d);
                        (x - x.round()).abs() < 1e-3 * x.max(1.0)
                    })
                    .unwrap_or(3) as usize
            }
            ParamKind::Log if self.unit == "Hz" && lo >= 10.0 => {
                let per_decade = (self.steps - 1) as f32 / (self.max / self.min).abs().log10();
                usize::from(self.steps % 2 == 1 && per_decade >= 319.0)
            }
            ParamKind::Log if lo < 1.0 => 2,
            ParamKind::Log if self.unit == "ms" => 0,
            ParamKind::Log if lo < 10.0 => 1,
            ParamKind::Log => 0,
            _ => return None,
        })
    }
}

/// Hertz as the console writes them: "84", "998.6", from 1 kHz up with the k
/// as the decimal point and one digit more than the parameter's decimals
/// ("5k7", "1k00", "20k00").
fn console_hz(v: f32, decimals: usize) -> String {
    let text = format!("{v:.decimals$}");
    if text.parse::<f32>().is_ok_and(|r| r.abs() < 1000.0) {
        return text;
    }
    let digits = decimals + 1;
    let scale = 10i64.pow(digits as u32);
    let n = (v as f64 / 1000.0 * scale as f64).round() as i64;
    format!("{}k{:0digits$}", n / scale, n % scale)
}

fn format_number(v: f32, unit: &str) -> String {
    match unit {
        "Hz" if v >= 1000.0 => format!("{:.2} kHz", v / 1000.0),
        // LFO rates below 10 Hz ("0.50 Hz"), filter frequencies whole.
        "Hz" if v < 10.0 => format!("{v:.2} Hz"),
        "Hz" => format!("{v:.0} Hz"),
        "ms" if v >= 1000.0 => format!("{:.2} s", v / 1000.0),
        "ms" if v < 10.0 => format!("{v:.1} ms"),
        "ms" => format!("{v:.0} ms"),
        // Reverb decays as the console writes them: "1.35 s", "1.50 s".
        "s" => format!("{v:.2} s"),
        "" => {
            if v.abs() >= 100.0 {
                format!("{v:.0}")
            } else {
                format!("{v:.2}")
            }
        }
        u => format!("{v:.1} {u}"),
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ModelSpec {
    pub token: &'static str,
    pub name: &'static str,
    pub category: ModelCategory,
    pub params: &'static [ParamSpec],
}

impl ModelSpec {
    pub fn param_index(&self, id: &str) -> Option<usize> {
        self.params.iter().position(|p| p.id == id)
    }

    /// Writes the model's defaults into `out` (the rest is zeroed).
    pub fn defaults_into(&self, out: &mut [f32]) {
        out.fill(0.0);
        for (o, p) in out.iter_mut().zip(self.params) {
            *o = p.default;
        }
    }
}

/// Finds a model by token in a table.
pub fn model_index(table: &[ModelSpec], token: &str) -> Option<usize> {
    table.iter().position(|m| m.token == token)
}

/// Read access to a model's parameter values by id.
#[derive(Clone, Copy)]
pub struct Params<'a> {
    pub spec: &'a ModelSpec,
    pub values: &'a [f32],
}

impl<'a> Params<'a> {
    pub fn new(spec: &'a ModelSpec, values: &'a [f32]) -> Self {
        Self { spec, values }
    }

    fn raw(&self, id: &str) -> Option<(f32, &ParamSpec)> {
        let i = self.spec.param_index(id)?;
        let p = &self.spec.params[i];
        Some((p.sanitise(self.values.get(i).copied().unwrap_or(p.default)), p))
    }

    /// Continuous value (or the number a Values item stands for).
    pub fn f(&self, id: &str) -> f32 {
        match self.raw(id) {
            Some((v, p)) => match p.kind {
                ParamKind::Values(items) => items.get(v as usize).copied().unwrap_or(0.0),
                _ => v,
            },
            None => 0.0,
        }
    }

    pub fn on(&self, id: &str) -> bool {
        self.raw(id).is_some_and(|(v, _)| v >= 0.5)
    }

    /// Choice index.
    pub fn choice(&self, id: &str) -> usize {
        self.raw(id).map_or(0, |(v, _)| v.max(0.0) as usize)
    }

    /// Choice text ("" if missing).
    pub fn text(&self, id: &str) -> &'static str {
        match self.raw(id) {
            Some((v, p)) => match p.kind {
                ParamKind::Choice(items) => items.get(v as usize).copied().unwrap_or(""),
                _ => "",
            },
            None => "",
        }
    }

    /// A choice like "1k6", "35", "OFF" parsed as Hz (None for OFF/unparsable).
    pub fn freq_choice(&self, id: &str) -> Option<f32> {
        parse_freq(self.text(id))
    }
}

/// Parses console frequency labels: "35", "1k6", "3k", "10k", "1k5".
pub fn parse_freq(s: &str) -> Option<f32> {
    if s.is_empty() || s.eq_ignore_ascii_case("off") {
        return None;
    }
    if let Some((a, b)) = s.split_once('k') {
        // "1k6" = 1.6 kHz; parsed without allocating (called on the audio thread).
        let whole: f32 = if a.is_empty() { 0.0 } else { a.parse().ok()? };
        let frac: f32 = if b.is_empty() { 0.0 } else { b.parse::<u32>().ok()? as f32 / 10f32.powi(b.len() as i32) };
        return Some((whole + frac) * 1000.0);
    }
    s.parse().ok()
}

/// Maps a 0..10 dial to `lo..hi` logarithmically (analog-style frequency knobs).
pub fn dial_log(dial: f32, lo: f32, hi: f32) -> f32 {
    lo * (hi / lo).powf((dial / 10.0).clamp(0.0, 1.0))
}

/// Maps a 0..10 dial to `lo..hi` linearly.
pub fn dial_lin(dial: f32, lo: f32, hi: f32) -> f32 {
    lo + (hi - lo) * (dial / 10.0).clamp(0.0, 1.0)
}

/// Gentle, level-preserving saturation used for analog colouration.
#[inline]
pub fn colour(x: f32, amount: f32) -> f32 {
    if amount <= 0.0 {
        x
    } else {
        (1.0 - amount) * x + amount * x.tanh()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tables_are_consistent() {
        assert_eq!(DYN_MODELS.len(), 32);
        assert_eq!(EQ_MODELS_CH.len(), 7);
        assert_eq!(EQ_MODELS_BUS.len(), 7);
        assert_eq!(PROCESS_ORDERS.len(), 24);
        assert_eq!(FX_MODELS.len(), 65);
        assert_eq!(FX_MODELS.iter().filter(|m| m.category == ModelCategory::FxStandard).count(), 32);
        assert_eq!(FX_MODELS.iter().filter(|m| m.category == ModelCategory::FxPremium).count(), 26);
        assert_eq!(FX_MODELS.iter().filter(|m| m.category == ModelCategory::FxChannel).count(), 5);
        for table in [DYN_MODELS, EQ_MODELS_CH, EQ_MODELS_BUS, FILTER_TOOLS, FX_MODELS] {
            for m in table {
                let max = match m.category {
                    ModelCategory::Eq => MAX_EQ_PARAMS,
                    ModelCategory::Filter => MAX_TOOL_PARAMS,
                    c if c.is_fx() => MAX_FX_PARAMS,
                    _ => MAX_DYN_PARAMS,
                };
                assert!(m.params.len() <= max, "{} has {} params", m.token, m.params.len());
                for p in m.params {
                    assert_eq!(p.sanitise(p.default), p.default, "{}/{} default out of range", m.token, p.id);
                }
            }
        }
        for t in AUX_DYN_MODELS {
            assert!(model_index(DYN_MODELS, t).is_some());
        }
    }

    #[test]
    fn values_and_display() {
        let comp = &DYN_MODELS[model_index(DYN_MODELS, "COMP").unwrap()];
        let mut v = [0.0; MAX_DYN_PARAMS];
        comp.defaults_into(&mut v);
        let p = Params::new(comp, &v);
        // As a factory-set console: THR -10 dB, 3:1, KNEE 3, RMS, AUTO ENV on.
        assert_eq!(p.f("thr"), -10.0);
        assert_eq!(p.f("ratio"), 3.0);
        assert_eq!(p.f("knee"), 3.0);
        assert_eq!(p.text("det"), "RMS");
        assert!(p.on("auto"));
        let rel = &comp.params[comp.param_index("rel").unwrap()];
        assert!((rel.from_normalised(rel.to_normalised(150.0)) - 150.0).abs() < 0.01);
        assert_eq!(rel.display(1500.0), "1500 ms");
        assert_eq!(rel.display(rel.default), "150 ms");
        // A reverb's DECAY keeps two decimals, as the rack shows it.
        let decay =
            ParamSpec { id: "dcy", label: "DECAY", unit: "s", kind: ParamKind::Log, min: 0.1, max: 5.0, steps: 51, default: 1.8 };
        assert_eq!(decay.display(1.5), "1.50 s");
        assert_eq!(parse_freq("1k6"), Some(1600.0));
        assert_eq!(parse_freq("35"), Some(35.0));
        assert_eq!(parse_freq("OFF"), None);
        assert!((dial_log(10.0, 30.0, 450.0) - 450.0).abs() < 0.01);
    }

    #[test]
    fn fx_values_read_as_the_console_writes_them() {
        // The HALL at its defaults, as manual Fig. 124 shows it: PRE DLY 50 ms,
        // SIZE 50, DECAY 1.57 s, BASS MULT 0.95, DAMPING 5k7 Hz, DIFFUSION 25.
        let hall = &FX_MODELS[model_index(FX_MODELS, "HALL").unwrap()];
        let text: Vec<String> = ["pdel", "size", "dcy", "mult", "damp", "diff"]
            .iter()
            .map(|id| {
                let p = &hall.params[hall.param_index(id).unwrap()];
                p.console_text(p.default)
            })
            .collect();
        assert_eq!(text, ["50 ms", "50", "1.57 s", "0.95", "5k7 Hz", "25"]);
        // DELAY/REVERB, Fig. 132: TIME 240 ms, PRE DLY 24 ms; times stay in ms.
        let dr = &FX_MODELS[model_index(FX_MODELS, "DEL/REV").unwrap()];
        let time = &dr.params[dr.param_index("time").unwrap()];
        assert_eq!(time.console_text(time.default), "240 ms");
        assert_eq!(time.console_text(1500.0), "1500 ms");
        let pdel = &dr.params[dr.param_index("pdel").unwrap()];
        assert_eq!(pdel.console_text(pdel.default), "24 ms");
        // Rates below 1 Hz with two decimals, percentages whole, fine EQ frequencies in tenths.
        let chorus = &FX_MODELS[model_index(FX_MODELS, "CHORUS").unwrap()];
        let spd = &chorus.params[chorus.param_index("spd").unwrap()];
        assert_eq!(spd.console_text(0.5), "0.50 Hz");
        let mix = &chorus.params[chorus.param_index("mix").unwrap()];
        assert_eq!(mix.console_text(50.0), "50 %");
        let deq = &FX_MODELS[model_index(FX_MODELS, "DEQ3").unwrap()];
        let f = &deq.params[deq.param_index("1-f").unwrap()];
        assert_eq!((f.console_text(149.9788), f.console_text(1002.374)), ("150.0 Hz".into(), "1k00 Hz".into()));
    }

    #[test]
    fn wing_models_start_as_a_factory_set_console() {
        let values = |table: &'static [ModelSpec], token| {
            let spec = &table[model_index(table, token).unwrap()];
            let mut v = [0.0; MAX_EQ_PARAMS];
            spec.defaults_into(&mut v);
            (spec, v)
        };
        // GATE: THR -40.0 dB, RATIO 1:3, RANGE 40.0 dB, ATTACK 10 ms, HOLD 10 ms, RELEASE 200 ms, ACCENT 0 %.
        let (gate, v) = values(DYN_MODELS, "GATE");
        let p = Params::new(gate, &v);
        assert_eq!(gate.name, "Wing Gate");
        assert_eq!((p.f("thr"), p.text("ratio"), p.f("range")), (-40.0, "1:3", 40.0));
        assert_eq!((p.f("att"), p.f("hld"), p.f("rel"), p.f("acc")), (10.0, 10.0, 200.47488, 0.0));
        // COMP: ATTACK 50 ms, HOLD 20 ms, RELEASE 150 ms.
        let (comp, v) = values(DYN_MODELS, "COMP");
        let p = Params::new(comp, &v);
        assert_eq!((p.f("att"), p.f("hld"), p.f("rel")), (50.0, 20.0, 150.33495));
        // The channel EQ's bands on the frequency steps: band 4 reads 3k99 Hz.
        let (eq, v) = values(EQ_MODELS_CH, "STD");
        let p = Params::new(eq, &v);
        let freqs = ["lf", "1f", "2f", "3f", "4f", "hf"].map(|id| p.f(id));
        for (f, want) in freqs.iter().zip([80.0, 200.0, 600.0, 1500.0, 4000.0, 12000.0]) {
            assert!((f / want - 1.0).abs() < 0.005, "{f} Hz for {want} Hz");
        }
        assert!((freqs[4] - 3990.5).abs() < 0.1);
        // Q 1.00 on its steps, the shelves too.
        assert_eq!((p.f("4q"), p.f("lq"), p.f("4g")), (0.9979702, 0.9979702, 0.0));
    }
}
