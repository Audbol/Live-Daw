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
    if r.is_finite() {
        r.clamp(0.0, 1.0)
    } else {
        0.0
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

/// Hz as the console writes it: "80.2", "601.4", "1k50", "10k02".
pub fn format_hz(v: f32) -> String {
    if v < 1000.0 {
        return format!("{v:.1}");
    }
    let k = v / 1000.0;
    let mut whole = k.trunc() as i64;
    let mut frac = ((k - k.trunc()) * 100.0).round() as i64;
    if frac >= 100 {
        whole += 1;
        frac -= 100;
    }
    format!("{whole}k{frac:02}")
}

fn decimals_for_step(step: f32) -> usize {
    if step >= 0.999 {
        0
    } else if step >= 0.0999 {
        1
    } else {
        2
    }
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
        NodeType::FloatEnum => {
            let x = v.as_f32().unwrap_or(0.0);
            if x.fract() == 0.0 && x.abs() < 1e6 {
                format!("{x:.0}")
            } else {
                format!("{x:.1}")
            }
        }
        NodeType::Int => v.as_i32().unwrap_or(0).to_string(),
        NodeType::Fader => {
            let x = v.as_f32().unwrap_or(FADER_MIN);
            if x <= FADER_MIN + 0.01 {
                "-oo".into()
            } else {
                format!("{x:.1}")
            }
        }
        NodeType::LinFloat | NodeType::LogFloat => {
            let x = v.as_f32().unwrap_or(0.0);
            if def.unit == unit::HZ {
                return format_hz(x);
            }
            let d = if def.ty == NodeType::LinFloat && def.steps > 1 {
                decimals_for_step((def.max - def.min).abs() / (def.steps - 1) as f32)
            } else if x.abs() >= 100.0 || (def.unit == unit::MS && x.abs() >= 10.0) {
                0
            } else if x.abs() >= 10.0 {
                1
            } else {
                2
            };
            format!("{x:.d$}")
        }
    }
}

/// Parses display text back into a value ("-oo", "1k50", an enum item...).
pub fn parse_display(def: &Def, text: &str) -> Option<Value> {
    let t = text.trim();
    let t = t.strip_prefix('\'').and_then(|x| x.strip_suffix('\'')).unwrap_or(t);
    match def.ty {
        NodeType::Str => Some(Value::str(t)),
        NodeType::StrEnum | NodeType::FloatEnum => coerce(def, &Value::str(t)),
        _ => coerce(def, &Value::Float(parse_number(t)?)),
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
