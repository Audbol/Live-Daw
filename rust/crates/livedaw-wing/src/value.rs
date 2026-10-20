//! Parameter values as the WING represents them, and their conversions:
//! display text (`"-oo"`, `"10k02"`, `"1:3"`), the 0..1 "raw" position the
//! OSC replies carry, and coercion of whatever a client sends (a float to an
//! integer parameter, an enum index, a string number) to the parameter's type.

use crate::schema::{unit, Def, NodeType};

/// A WING value: floats in the parameter's unit (faders in dB, -144 = -oo),
/// integers (switches are 0/1), strings (string enums carry the item).
#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Int(i32),
    Float(f32),
    Str(String),
}

impl Value {
    pub fn str(s: impl Into<String>) -> Self {
        Value::Str(s.into())
    }

    pub fn bool(b: bool) -> Self {
        Value::Int(b as i32)
    }

    pub fn as_f32(&self) -> Option<f32> {
        match self {
            Value::Int(i) => Some(*i as f32),
            Value::Float(f) => Some(*f),
            Value::Str(s) => parse_number(s),
        }
    }

    pub fn as_i32(&self) -> Option<i32> {
        match self {
            Value::Int(i) => Some(*i),
            Value::Float(f) if f.is_finite() => Some(f.round() as i32),
            Value::Float(_) => None,
            Value::Str(s) => parse_number(s).map(|f| f.round() as i32),
        }
    }

    pub fn as_bool(&self) -> bool {
        self.as_i32().unwrap_or(0) != 0
    }
}

pub use livedaw_dsp::fader::{fader_to_pos, pos_to_fader, FADER_MAX, FADER_MIN};

/// Parses console number text: "-3.0", "-oo", "1k50", "10k02", "1:3" isn't one.
pub fn parse_number(s: &str) -> Option<f32> {
    let t = s.trim().trim_matches('\'');
    if t.eq_ignore_ascii_case("-oo") || t.eq_ignore_ascii_case("-inf") {
        return Some(FADER_MIN);
    }
    if let Ok(v) = t.parse::<f32>() {
        return v.is_finite().then_some(v);
    }
    let (a, b) = t.split_once(['k', 'K'])?;
    let whole: f32 = if a.is_empty() { 0.0 } else { a.parse().ok()? };
    if b.is_empty() {
        return Some(whole * 1000.0);
    }
    if !b.bytes().all(|c| c.is_ascii_digit()) {
        return None;
    }
    let frac = b.parse::<u32>().ok()? as f32 / 10f32.powi(b.len() as i32);
    Some((whole + frac) * 1000.0)
}

fn lo_hi(def: &Def) -> (f32, f32) {
    (def.min.min(def.max), def.min.max(def.max))
}

/// The 0..1 position of a value (OSC's raw facet, Native's 0xd6 token).
pub fn to_raw(def: &Def, v: &Value) -> f32 {
    let r = match def.ty {
        NodeType::Fader => fader_to_pos(v.as_f32().unwrap_or(FADER_MIN)),
        NodeType::LinFloat => {
            let x = v.as_f32().unwrap_or(def.min);
            if def.max != def.min {
                (x - def.min) / (def.max - def.min)
            } else {
                0.0
            }
        }
        NodeType::LogFloat => {
            let x = v.as_f32().unwrap_or(def.min);
            if def.min > 0.0 && def.max > 0.0 && x > 0.0 && def.max != def.min {
                (x / def.min).ln() / (def.max / def.min).ln()
            } else {
                0.0
            }
        }
        NodeType::Int => {
            let x = v.as_i32().unwrap_or(def.min_int);
            if def.max_int != def.min_int {
                (x - def.min_int) as f32 / (def.max_int - def.min_int) as f32
            } else {
                0.0
            }
        }
        NodeType::StrEnum | NodeType::FloatEnum => {
            let n = def.items().len();
            match enum_index(def, v) {
                Some(i) if n > 1 => i as f32 / (n - 1) as f32,
                _ => 0.0,
            }
        }
        NodeType::Str | NodeType::Node => 0.0,
    };
    if !r.is_finite() {
        return 0.0;
    }
    let r = r.clamp(0.0, 1.0);
    // A stepped float sits on one of its steps: k / (steps - 1), as the console sends it.
    match def.ty {
        NodeType::LinFloat | NodeType::LogFloat if def.steps > 1 => {
            let n = (def.steps - 1) as f64;
            ((r as f64 * n).round() / n) as f32
        }
        _ => r,
    }
}

/// The value at 0..1 position `raw`.
pub fn from_raw(def: &Def, raw: f32) -> Option<Value> {
    let p = if raw.is_finite() { raw.clamp(0.0, 1.0) } else { return None };
    Some(match def.ty {
        NodeType::Fader => Value::Float(pos_to_fader(p)),
        NodeType::LinFloat => Value::Float(def.min + (def.max - def.min) * p),
        NodeType::LogFloat if def.min > 0.0 && def.max > 0.0 => Value::Float(def.min * (def.max / def.min).powf(p)),
        NodeType::LogFloat => Value::Float(def.min + (def.max - def.min) * p),
        NodeType::Int => Value::Int(def.min_int + ((def.max_int - def.min_int) as f32 * p).round() as i32),
        NodeType::StrEnum | NodeType::FloatEnum => {
            let n = def.items().len();
            if n == 0 {
                return None;
            }
            item_value(def, ((n - 1) as f32 * p).round() as usize)?
        }
        NodeType::Str | NodeType::Node => return None,
    })
}

/// Index of an enum value (an item, a long item, an index, or a float item value).
pub fn enum_index(def: &Def, v: &Value) -> Option<usize> {
    let items = def.items();
    if def.ty == NodeType::FloatEnum {
        let x = match v {
            Value::Str(s) => {
                if let Some(i) = items.iter().position(|it| it.long.eq_ignore_ascii_case(s.trim())) {
                    return Some(i);
                }
                parse_number(s)?
            }
            _ => v.as_f32()?,
        };
        // Exact value first, then the nearest item.
        return items.iter().enumerate().min_by(|a, b| (a.1.value - x).abs().total_cmp(&(b.1.value - x).abs())).map(|(i, _)| i);
    }
    match v {
        Value::Str(s) => {
            let s = s.trim().trim_matches('\'');
            items
                .iter()
                .position(|it| it.item == s)
                .or_else(|| items.iter().position(|it| it.item.eq_ignore_ascii_case(s)))
                .or_else(|| items.iter().position(|it| !it.long.is_empty() && it.long.eq_ignore_ascii_case(s)))
                .or_else(|| s.parse::<usize>().ok().filter(|&i| i < items.len()))
        }
        _ => usize::try_from(v.as_i32()?).ok().filter(|&i| i < items.len()),
    }
}

/// The canonical value of enum item `i`.
pub fn item_value(def: &Def, i: usize) -> Option<Value> {
    let it = def.items().get(i)?;
    Some(if def.ty == NodeType::FloatEnum { Value::Float(it.value) } else { Value::str(it.item) })
}

/// Coerces a value a client sent to the parameter's canonical type and range.
/// None if it can't be (an unknown enum item, text for a number...).
pub fn coerce(def: &Def, v: &Value) -> Option<Value> {
    Some(match def.ty {
        NodeType::Node => return None,
        NodeType::Fader => Value::Float(v.as_f32()?.clamp(FADER_MIN, FADER_MAX)),
        NodeType::LinFloat | NodeType::LogFloat => {
            let (lo, hi) = lo_hi(def);
            let x = v.as_f32()?;
            Value::Float(if hi > lo { x.clamp(lo, hi) } else { x })
        }
        NodeType::Int => {
            let x = match v {
                Value::Str(s) if s.eq_ignore_ascii_case("on") => 1,
                Value::Str(s) if s.eq_ignore_ascii_case("off") => 0,
                _ => v.as_i32()?,
            };
            Value::Int(if def.max_int >= def.min_int { x.clamp(def.min_int, def.max_int) } else { x })
        }
        NodeType::StrEnum | NodeType::FloatEnum => item_value(def, enum_index(def, v)?)?,
        NodeType::Str => {
            let s = match v {
                Value::Str(s) => s.clone(),
                Value::Int(i) => i.to_string(),
                Value::Float(f) => format!("{f:.2}"),
            };
            let max = if def.max_len > 0 { def.max_len as usize } else { 256 };
            Value::Str(truncate(&s, max))
        }
    })
}

fn truncate(s: &str, max: usize) -> String {
    if s.len() <= max {
        return s.to_owned();
    }
    let mut end = max;
    while !s.is_char_boundary(end) {
        end -= 1;
    }
    s[..end].to_owned()
}

/// A neutral value for a parameter nothing has set yet.
pub fn default_value(def: &Def) -> Value {
    match def.ty {
        NodeType::Node | NodeType::Str => Value::str(""),
        NodeType::Fader => Value::Float(0.0),
        NodeType::LinFloat | NodeType::LogFloat => {
            let (lo, hi) = lo_hi(def);
            Value::Float(if lo <= 0.0 && 0.0 <= hi { 0.0 } else { lo })
        }
        NodeType::Int => Value::Int(if def.min_int <= 0 && 0 <= def.max_int { 0 } else { def.min_int }),
        NodeType::StrEnum | NodeType::FloatEnum => item_value(def, 0).unwrap_or(Value::Int(0)),
    }
}

/// Hz as the console writes it: below 1 kHz with `decimals` decimals ("80.2",
/// "100"), from 1 kHz up in kHz with one digit more after the "k" ("1k50",
/// "10k0").
pub fn format_hz(v: f32, decimals: usize) -> String {
    let text = format!("{v:.decimals$}");
    if text.parse::<f32>().is_ok_and(|r| r.abs() < 1000.0) {
        return text;
    }
    let digits = decimals + 1;
    let scale = 10i64.pow(digits as u32);
    let n = (v as f64 / 1000.0 * scale as f64).round() as i64;
    format!("{}k{:0digits$}", n / scale, n % scale)
}

/// Positions the console shows as a running time: the player's seconds
/// ("9:59:59") and the WING-LIVE card's milliseconds, to the hundredth
/// ("2:35:20").
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Clock {
    Seconds,
    Millis,
}

fn clock(def: &Def) -> Option<Clock> {
    if def.ty != NodeType::LinFloat || def.unit != unit::NONE || def.min != 0.0 {
        return None;
    }
    match def.steps {
        36_000 => Some(Clock::Seconds),
        36_000_000 => Some(Clock::Millis),
        _ => None,
    }
}

fn format_clock(c: Clock, v: f32) -> String {
    let v = v.max(0.0) as f64;
    match c {
        Clock::Seconds => {
            let t = v.round() as i64;
            let (h, m, s) = (t / 3600, t / 60 % 60, t % 60);
            if h > 0 {
                format!("{h}:{m:02}:{s:02}")
            } else {
                format!("{m}:{s:02}")
            }
        }
        Clock::Millis => {
            // Hours wrap at 100: the console's range reads "0:00:00 .. 0:00:00".
            let t = (v / 10.0).round() as i64;
            let (h, m, s, cs) = (t / 360_000 % 100, t / 6000 % 60, t / 100 % 60, t % 100);
            if h > 0 {
                format!("{h}:{m:02}:{s:02}:{cs:02}")
            } else {
                format!("{m}:{s:02}:{cs:02}")
            }
        }
    }
}

fn parse_clock(c: Clock, text: &str) -> Option<f32> {
    let parts: Vec<f64> = text.split(':').map(|p| p.trim().parse::<f64>().ok()).collect::<Option<_>>()?;
    let (scale, fields): (f64, &[f64]) = match c {
        Clock::Seconds => (1.0, &[3600.0, 60.0, 1.0]),
        Clock::Millis => (10.0, &[360_000.0, 6000.0, 100.0, 1.0]),
    };
    if parts.len() > fields.len() {
        return None;
    }
    let units = &fields[fields.len() - parts.len()..];
    Some((parts.iter().zip(units).map(|(p, u)| p * u).sum::<f64>() * scale) as f32)
}

/// Decimals of a float parameter's text, fixed per parameter as on the
/// console (its value and the bounds of its range alike). None for floats
/// without a range.
pub fn decimals(def: &Def) -> Option<usize> {
    if def.steps < 2 || def.max == def.min {
        return None;
    }
    let lo = def.min.min(def.max);
    Some(match def.ty {
        // As many as the step needs: 0.1 -> 1, 2.5 -> 1, 0.25 -> 2, 5 -> 0.
        NodeType::LinFloat => {
            let step = (def.max - def.min).abs() as f64 / (def.steps - 1) as f64;
            (0..3)
                .find(|&d| {
                    let x = step * 10f64.powi(d);
                    (x - x.round()).abs() < 1e-3 * x.max(1.0)
                })
                .unwrap_or(3) as usize
        }
        // The EQs, filters and the oscillator (an odd count of 320 or more
        // steps per decade) show tenths of a hertz, the sidechain filters
        // (300 per decade) and the channel PEQ (960 steps) whole hertz.
        NodeType::LogFloat if def.unit == unit::HZ && lo >= 10.0 => {
            let per_decade = (def.steps - 1) as f32 / (def.max / def.min).abs().log10();
            usize::from(def.steps % 2 == 1 && per_decade >= 319.0)
        }
        NodeType::LogFloat if lo < 1.0 => 2,
        NodeType::LogFloat if def.unit == unit::MS => 0,
        NodeType::LogFloat if lo < 10.0 => 1,
        NodeType::LogFloat => 0,
        _ => return None,
    })
}

/// Signed ranges whose positive bound the console writes with a "+" ("lin
/// [-15.0 .. +15.0 dB]"): gains, trims, balances and pans, but not widths
/// (%) or the dynamic EQ's +/-30 dB depth.
pub fn signed(def: &Def) -> bool {
    def.ty == NodeType::LinFloat
        && def.min < 0.0
        && def.max == -def.min
        && def.unit != unit::PERCENT
        && !(def.unit == unit::DB && def.max >= 30.0)
}

/// A float parameter's number as the console writes it (its value, or a bound
/// of its range).
pub fn format_float(def: &Def, x: f32) -> String {
    if let Some(c) = clock(def) {
        return format_clock(c, x);
    }
    let d = decimals(def).unwrap_or(if x.abs() >= 100.0 || (def.unit == unit::MS && x.abs() >= 10.0) {
        0
    } else if x.abs() >= 10.0 {
        1
    } else {
        2
    });
    if def.unit == unit::HZ {
        format_hz(x, d)
    } else {
        format!("{x:.d$}")
    }
}

/// A float enum's item as the console lists it: one decimal ("48000.0",
/// "3.0"), whole dB ("30"), and in lists that also hold items below 10 (the
/// ratios) the items from 10 up whole and padded to three characters (" 10").
pub fn format_float_item(def: &Def, x: f32) -> String {
    let items = def.items();
    let mixed = items.iter().any(|i| i.value.abs() < 10.0) && items.iter().any(|i| i.value.abs() >= 10.0);
    if def.unit == unit::DB {
        format!("{x:.0}")
    } else if mixed && x.abs() >= 10.0 {
        format!("{x:3.0}")
    } else {
        format!("{x:.1}")
    }
}

/// The value at the nearest of the parameter's steps: the console keeps a
/// float at one of its `steps` positions (a Q of 1.00 reads back 0.9979702,
/// 100 Hz 99.68543, 150 ms 150.3349).
pub fn quantise(def: &Def, v: &Value) -> Value {
    let Value::Float(x) = *v else { return v.clone() };
    if def.steps < 2 || def.max == def.min || !x.is_finite() {
        return v.clone();
    }
    let (min, max, n) = (def.min as f64, def.max as f64, (def.steps - 1) as f64);
    let q = match def.ty {
        NodeType::LinFloat => {
            let k = ((x as f64 - min) / (max - min) * n).round();
            min + (max - min) * k / n
        }
        NodeType::LogFloat if min > 0.0 && max > 0.0 && x > 0.0 => {
            let k = ((x as f64 / min).ln() / (max / min).ln() * n).round();
            min * (max / min).powf(k / n)
        }
        _ => return v.clone(),
    };
    Value::Float(q as f32)
}

/// The value as the console displays it (the first argument of an OSC reply).
pub fn display(def: &Def, v: &Value) -> String {
    match def.ty {
        NodeType::Node => String::new(),
        NodeType::Str => match v {
            Value::Str(s) => s.clone(),
            other => other.as_i32().map(|i| i.to_string()).unwrap_or_default(),
        },
        NodeType::StrEnum => match enum_index(def, v) {
            Some(i) => def.items()[i].item.to_owned(),
            None => match v {
                Value::Str(s) => s.clone(),
                other => other.as_i32().map(|i| i.to_string()).unwrap_or_default(),
            },
        },
        NodeType::FloatEnum => format_float_item(def, v.as_f32().unwrap_or(0.0)).trim_start().to_owned(),
        NodeType::Int => v.as_i32().unwrap_or(0).to_string(),
        NodeType::Fader => {
            let x = v.as_f32().unwrap_or(FADER_MIN);
            if x <= FADER_MIN + 0.01 {
                "-oo".into()
            } else {
                format!("{x:.1}")
            }
        }
        NodeType::LinFloat | NodeType::LogFloat => format_float(def, v.as_f32().unwrap_or(0.0)),
    }
}

/// Parses display text back into a value ("-oo", "1k50", "2:35:20", an enum item...).
pub fn parse_display(def: &Def, text: &str) -> Option<Value> {
    let t = text.trim();
    let t = t.strip_prefix('\'').and_then(|x| x.strip_suffix('\'')).unwrap_or(t);
    match def.ty {
        NodeType::Str => Some(Value::str(t)),
        NodeType::StrEnum | NodeType::FloatEnum => coerce(def, &Value::str(t)),
        _ => {
            let x = match clock(def) {
                Some(c) if t.contains(':') => parse_clock(c, t)?,
                _ => parse_number(t)?,
            };
            coerce(def, &Value::Float(x))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::schema::{schema, NoModels};

    fn def(path: &str) -> &'static Def {
        let s = schema();
        s.def(s.resolve(path, &NoModels).unwrap(), &NoModels).unwrap()
    }

    #[test]
    fn fader_law_matches_the_documented_points() {
        for (db, pos) in [(0.0, 0.75), (-1.0, 0.725), (-2.0, 0.70), (-3.0, 0.675), (3.0, 0.825), (4.0, 0.85), (10.0, 1.0)] {
            assert!((fader_to_pos(db) - pos).abs() < 1e-5, "{db} dB");
            assert!((pos_to_fader(pos) - db).abs() < 1e-3);
        }
        assert_eq!(fader_to_pos(-144.0), 0.0);
        assert_eq!(pos_to_fader(0.0), -144.0);
        for i in 0..=100 {
            let p = i as f32 / 100.0;
            assert!((fader_to_pos(pos_to_fader(p)) - p).abs() < 1e-4);
        }
    }

    #[test]
    fn display_and_parse() {
        assert_eq!(display(def("/ch/1/fdr"), &Value::Float(-144.0)), "-oo");
        assert_eq!(display(def("/ch/1/fdr"), &Value::Float(-3.0)), "-3.0");
        assert_eq!(display(def("/ch/1/flt/hcf"), &Value::Float(10018.26)), "10k02");
        assert_eq!(display(def("/ch/1/flt/lcf"), &Value::Float(100.2374)), "100.2");
        assert_eq!(display(def("/ch/1/pan"), &Value::Float(0.0)), "0");
        assert_eq!(display(def("/ch/1/in/set/trim"), &Value::Float(0.0)), "0.0");
        assert_eq!(display(def("/ch/1/mon"), &Value::Int(2)), "A+B");
        assert_eq!(parse_number("1k50"), Some(1500.0));
        assert_eq!(parse_number("-oo"), Some(-144.0));
        assert_eq!(parse_display(def("/ch/1/fdr"), "-oo"), Some(Value::Float(-144.0)));
        assert_eq!(parse_display(def("/ch/1/mon"), "a+b"), Some(Value::str("A+B")));
    }

    #[test]
    fn values_read_as_on_the_console() {
        // A factory-set WING's replies (text, value): decimals fixed per
        // parameter, kHz with one digit more than below 1 kHz.
        for (path, text, x) in [
            ("/ch/1/flt/lcf", "60.1", 60.13884),
            ("/ch/1/flt/hcf", "10k02", 10018.26),
            ("/io/in/OSC/1/osc/f", "1k00", 999.992),
            ("/cfg/mon/1/eq/hsf", "12k00", 11999.27),
            ("/ch/1/dynsc/f", "1k0", 1002.374),
            ("/ch/1/peq/1f", "100", 99.68543),
            ("/ch/1/peq/1f", "999", 999.2505),
            ("/ch/1/peq/1f", "10k0", 10016.53),
            ("/ch/1/peq/1q", "1.00", 0.9979702),
            ("/ch/1/dynxo/q", "0.40", 0.4),
            ("/ch/1/dynxo/depth", "6.0", 6.0),
            ("/io/in/LCL/1/g", "0.0", 0.0),
            ("/$ctl/$globals/mousespd", "1.77", 1.77),
            ("/ch/1/in/set/trim", "0.0", 0.0),
            ("/ch/1/pan", "0", 0.0),
            ("/ch/1/wid", "100", 100.0),
            ("/play/$pos", "0:00", 0.0),
            ("/cards/wlive/1/$stat/sdfree", "2:35:20", 155200.0),
            ("/cards/wlive/2/$stat/sdfree", "1:12:52:85", 4372850.0),
            ("/$globals/clkrate", "48000.0", 48000.0),
            ("/cfg/rta/eqrange", "30", 30.0),
        ] {
            let d = def(path);
            assert_eq!(display(d, &Value::Float(x)), text, "{path}");
            let back = parse_display(d, text).and_then(|v| v.as_f32()).unwrap();
            assert_eq!(quantise(d, &Value::Float(back)).as_f32(), quantise(d, &Value::Float(x)).as_f32(), "{path} {text}");
        }
        assert_eq!(display(def("/play/$pos"), &Value::Float(35999.0)), "9:59:59");
        assert_eq!(parse_display(def("/play/$pos"), "1:00:05"), Some(Value::Float(3605.0)));
    }

    #[test]
    fn floats_are_kept_on_their_steps() {
        // What a WING reads back after a client sets a round value.
        for (path, set, read) in [
            ("/ch/1/peq/1q", 1.0, 0.9979702),
            ("/ch/1/peq/1f", 100.0, 99.68543),
            ("/ch/1/dynsc/f", 1000.0, 1002.374),
            ("/io/in/OSC/1/osc/f", 1000.0, 999.992),
            ("/cfg/mon/1/eq/hsf", 12000.0, 11999.27),
            ("/ch/1/in/set/trim", 4.53, 4.5),
            ("/io/in/LCL/1/g", 3.0, 2.5),
        ] {
            let Some(Value::Float(got)) = Some(quantise(def(path), &Value::Float(set))) else { panic!() };
            assert!((got - read).abs() <= 2e-6 * read.max(1.0), "{path}: {got} for {set}, the console reads {read}");
        }
        // Integers, enums and faders as they are.
        assert_eq!(quantise(def("/ch/1/fdr"), &Value::Float(-3.3)), Value::Float(-3.3));
        assert_eq!(quantise(def("/ch/1/col"), &Value::Int(3)), Value::Int(3));
    }

    #[test]
    fn coercion() {
        let mute = def("/ch/1/mute");
        assert_eq!(coerce(mute, &Value::Float(1.0)), Some(Value::Int(1)));
        assert_eq!(coerce(mute, &Value::Int(5)), Some(Value::Int(1)));
        assert_eq!(coerce(mute, &Value::str("1")), Some(Value::Int(1)));
        let mode = def("/ch/1/send/1/mode");
        assert_eq!(coerce(mode, &Value::Int(1)), Some(Value::str("POST")));
        assert_eq!(coerce(mode, &Value::str("grp")), Some(Value::str("GRP")));
        assert_eq!(coerce(mode, &Value::str("bogus")), None);
        let trim = def("/ch/1/in/set/trim");
        assert_eq!(coerce(trim, &Value::Float(40.0)), Some(Value::Float(18.0)));
        let name = def("/ch/1/name");
        assert_eq!(coerce(name, &Value::str("A very long channel name")), Some(Value::str("A very long chan")));
        assert!((to_raw(trim, &Value::Float(0.0)) - 0.5).abs() < 1e-6);
        assert_eq!(from_raw(trim, 1.0), Some(Value::Float(18.0)));
    }
}
