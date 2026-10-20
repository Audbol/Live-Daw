//! Model metadata for the host UI: the UI builds its controls (knobs, choices,
//! switches, labels, value text) from the same tables the DSP uses.
//!
//! Tables: 0 = dynamics models (GATE and DYNAMICS slots), 1 = channel EQ
//! models, 2 = bus/main/matrix EQ models, 3 = filter tools, 4 = FX rack devices.
//! Lists: 0 = process
//! orders, 1 = tap points, 2 = sidechain filters, 3 = crossover types,
//! 4 = bus send modes, 5 = delay units, 6 = post insert modes, 7 = monitor
//! buses, 8 = low-cut slopes, 9 = high-cut slopes, 10 = aux dynamics models,
//! 11 = strip colours (index 0 = WING colour 1), 12 = MAIN LINK modes.

use crate::session::LdString;
use livedaw_dsp::delay::DELAY_MODES;
use livedaw_dsp::wing::dynamics::{SC_FILTERS, XOVER_TYPES};
use livedaw_dsp::wing::eq::design;
use livedaw_dsp::wing::filter::{HIGH_CUT_SLOPES, LOW_CUT_SLOPES};
use livedaw_dsp::wing::{
    ModelCategory, ModelSpec, ParamKind, ParamSpec, AUX_DYN_MODELS, DYN_MODELS, EQ_MODELS_BUS, EQ_MODELS_CH, FILTER_TOOLS,
    FX_MODELS, MAX_EQ_PARAMS, PROCESS_ORDERS, TAP_POINTS,
};
use livedaw_engine::params::{colour_rgb, MAIN_LINK_MODES, MONITOR_BUSES, POST_INSERT_MODES, WING_COLOURS};
use livedaw_engine::{Strip, SEND_MODES};

fn table(t: i32) -> &'static [ModelSpec] {
    match t {
        1 => EQ_MODELS_CH,
        2 => EQ_MODELS_BUS,
        3 => FILTER_TOOLS,
        4 => FX_MODELS,
        _ => DYN_MODELS,
    }
}

fn model(t: i32, m: i32) -> Option<&'static ModelSpec> {
    table(t).get(usize::try_from(m).ok()?)
}

fn param(t: i32, m: i32, p: i32) -> Option<&'static ParamSpec> {
    model(t, m)?.params.get(usize::try_from(p).ok()?)
}

fn list(l: i32) -> &'static [&'static str] {
    match l {
        0 => PROCESS_ORDERS,
        1 => TAP_POINTS,
        2 => &SC_FILTERS,
        3 => &XOVER_TYPES,
        4 => &SEND_MODES,
        5 => &DELAY_MODES,
        6 => &POST_INSERT_MODES,
        7 => &MONITOR_BUSES,
        8 => &LOW_CUT_SLOPES,
        9 => &HIGH_CUT_SLOPES,
        10 => AUX_DYN_MODELS,
        11 => &COLOUR_NAMES,
        12 => &MAIN_LINK_MODES,
        _ => &[],
    }
}

static COLOUR_NAMES: [&str; WING_COLOURS.len()] = {
    let mut names = [""; WING_COLOURS.len()];
    let mut i = 0;
    while i < names.len() {
        names[i] = WING_COLOURS[i].0;
        i += 1;
    }
    names
};

/// RGB (0xRRGGBB) of WING colour `index` (1..18).
#[no_mangle]
pub extern "C" fn ld_colour_rgb(index: i32) -> u32 {
    colour_rgb(index)
}

#[no_mangle]
pub extern "C" fn ld_model_count(t: i32) -> i32 {
    table(t).len() as i32
}

/// what: 0 = token ("COMP"), 1 = display name.
#[no_mangle]
pub extern "C" fn ld_model_text(t: i32, m: i32, what: i32) -> *mut LdString {
    let s = model(t, m).map_or("", |m| if what == 0 { m.token } else { m.name });
    LdString::new(s.to_owned())
}

/// 0 = gate, 1 = compressor, 2 = special, 3 = EQ, 4 = filter; FX devices:
/// 5 = none, 6 = external (plugin), 7 = premium, 8 = standard, 9 = channel strip.
#[no_mangle]
pub extern "C" fn ld_model_category(t: i32, m: i32) -> i32 {
    model(t, m).map_or(0, |m| match m.category {
        ModelCategory::Gate => 0,
        ModelCategory::Compressor => 1,
        ModelCategory::Special => 2,
        ModelCategory::Eq => 3,
        ModelCategory::Filter => 4,
        ModelCategory::FxNone => 5,
        ModelCategory::FxExternal => 6,
        ModelCategory::FxPremium => 7,
        ModelCategory::FxStandard => 8,
        ModelCategory::FxChannel => 9,
    })
}

#[no_mangle]
pub unsafe extern "C" fn ld_model_find(t: i32, utf8: *const u8, len: usize) -> i32 {
    let token = crate::session::str_arg(utf8, len);
    table(t).iter().position(|m| m.token == token).map_or(-1, |i| i as i32)
}

#[no_mangle]
pub extern "C" fn ld_model_num_params(t: i32, m: i32) -> i32 {
    model(t, m).map_or(0, |m| m.params.len() as i32)
}

#[repr(C)]
#[derive(Default)]
pub struct LdParamInfo {
    /// 0 = linear, 1 = log, 2 = choice, 3 = values, 4 = toggle, 5 = integer.
    pub kind: i32,
    pub min: f32,
    pub max: f32,
    pub steps: i32,
    pub default_value: f32,
    /// Number of choices (choice/values kinds).
    pub num_choices: i32,
}

#[no_mangle]
pub unsafe extern "C" fn ld_model_param_info(t: i32, m: i32, p: i32, out: *mut LdParamInfo) -> bool {
    let (Some(spec), Some(out)) = (param(t, m, p), out.as_mut()) else { return false };
    let (kind, num) = match spec.kind {
        ParamKind::Linear => (0, 0),
        ParamKind::Log => (1, 0),
        ParamKind::Choice(items) => (2, items.len()),
        ParamKind::Values(items) => (3, items.len()),
        ParamKind::Toggle => (4, 0),
        ParamKind::Int => (5, 0),
    };
    *out = LdParamInfo {
        kind,
        min: spec.min,
        max: spec.max,
        steps: spec.steps as i32,
        default_value: spec.default,
        num_choices: num as i32,
    };
    true
}

/// what: 0 = id ("thr"), 1 = label ("THR"), 2 = unit ("dB").
#[no_mangle]
pub extern "C" fn ld_model_param_text(t: i32, m: i32, p: i32, what: i32) -> *mut LdString {
    let s = param(t, m, p).map_or("", |p| match what {
        0 => p.id,
        1 => p.label,
        _ => p.unit,
    });
    LdString::new(s.to_owned())
}

/// Text of choice `i` of a choice/values parameter.
#[no_mangle]
pub extern "C" fn ld_model_param_choice(t: i32, m: i32, p: i32, i: i32) -> *mut LdString {
    let s = param(t, m, p).map(|p| p.display(i as f32)).unwrap_or_default();
    LdString::new(s)
}

/// Console-style text for a value ("-20.0 dB", "1:2", "8k").
#[no_mangle]
pub extern "C" fn ld_model_param_display(t: i32, m: i32, p: i32, value: f32) -> *mut LdString {
    LdString::new(param(t, m, p).map(|p| p.display(value)).unwrap_or_default())
}

/// Knob position 0..1 for a value (log taper for log parameters).
#[no_mangle]
pub extern "C" fn ld_model_param_to_normalised(t: i32, m: i32, p: i32, value: f32) -> f32 {
    param(t, m, p).map_or(0.0, |p| p.to_normalised(value))
}

#[no_mangle]
pub extern "C" fn ld_model_param_from_normalised(t: i32, m: i32, p: i32, pos: f32) -> f32 {
    param(t, m, p).map_or(0.0, |p| p.from_normalised(pos))
}

/// Static curve of a dynamics model (the transfer graph): for each input level
/// in `in_db`, the output level, computed from the same configuration the
/// audio thread runs. Returns false (outputs untouched) for models without a
/// level curve.
///
/// # Safety
/// `values` must hold `num_values` floats, `in_db` and `out_db` `count` each.
#[no_mangle]
pub unsafe extern "C" fn ld_dyn_static_curve(
    model: i32,
    values: *const f32,
    num_values: i32,
    in_db: *const f32,
    out_db: *mut f32,
    count: i32,
) -> bool {
    let (Ok(model), Ok(nv), Ok(n)) = (usize::try_from(model), usize::try_from(num_values), usize::try_from(count)) else {
        return false;
    };
    if values.is_null() || in_db.is_null() || out_db.is_null() {
        return false;
    }
    let values = std::slice::from_raw_parts(values, nv);
    let ins = std::slice::from_raw_parts(in_db, n);
    let outs = std::slice::from_raw_parts_mut(out_db, n);
    livedaw_dsp::wing::dynamics::static_curve(model, values, ins, outs)
}

#[no_mangle]
pub extern "C" fn ld_list_count(l: i32) -> i32 {
    list(l).len() as i32
}

#[no_mangle]
pub extern "C" fn ld_list_item(l: i32, i: i32) -> *mut LdString {
    LdString::new(usize::try_from(i).ok().and_then(|i| list(l).get(i)).copied().unwrap_or("").to_owned())
}

/// See `ld_strip_response`.
pub(crate) fn strip_response(s: &Strip, block: i32, freqs: &[f64], out: &mut [f64], sr: f64) {
    if block == 4 {
        // TAP EQ
        let d = livedaw_engine::tapsend::tap_eq_design(&s.params.tap_eq, sr);
        for (f, o) in freqs.iter().zip(out.iter_mut()) {
            let mag: f64 = d.iter().flatten().map(|c| c.magnitude_at(*f, sr)).product();
            *o = 20.0 * mag.max(1e-9).log10();
        }
        return;
    }
    if (5..=7).contains(&block) {
        // The GATE's or the DYNAMICS' key filter, the DYNAMICS' crossover.
        let c = match block {
            5 => s.params.gate.sidechain().coefficients(sr),
            6 => s.params.dynamics.sidechain().coefficients(sr),
            _ => s.params.dynamics.xover().coefficients(sr),
        };
        for (f, o) in freqs.iter().zip(out.iter_mut()) {
            *o = 20.0 * c.magnitude_at(*f, sr).max(1e-9).log10();
        }
        return;
    }
    if block == 3 {
        let settings = s.params.filter.snapshot();
        let (coeffs, n) = settings.design(sr);
        for (f, o) in freqs.iter().zip(out.iter_mut()) {
            let mag: f64 = coeffs[..n].iter().map(|c| c.magnitude_at(*f, sr)).product();
            *o = 20.0 * mag.max(1e-9).log10();
        }
        return;
    }
    let eq = &s.params.eq;
    let table = if s.kind().is_output() { EQ_MODELS_BUS } else { EQ_MODELS_CH };
    let spec = &table[(eq.model.load(std::sync::atomic::Ordering::Relaxed).max(0) as usize).min(table.len() - 1)];
    let mut values = [0.0; MAX_EQ_PARAMS];
    eq.values_snapshot(&mut values);
    let on = eq.on.load(std::sync::atomic::Ordering::Relaxed);
    let d = design(spec, &values[..spec.params.len()], sr, if on { eq.mix.load() / 100.0 } else { 0.0 });
    for (f, o) in freqs.iter().zip(out.iter_mut()) {
        *o = d.response_db(*f, sr);
    }
}
