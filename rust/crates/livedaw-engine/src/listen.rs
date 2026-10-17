//! What BAND SOLO plays: the selected WING EQ band as a filter (a band pass
//! for the bells, a low or high pass for the outer bands' shelves and cuts:
//! the range they act on).

use livedaw_dsp::biquad::BiquadCoeffs;
use livedaw_dsp::wing::{ModelSpec, Params};
use std::f64::consts::FRAC_1_SQRT_2;

/// The parameter index of `<band><suffix>` ("2" + "q"), without allocating.
fn find(spec: &ModelSpec, band: u8, suffix: &str) -> Option<usize> {
    spec.params.iter().position(|p| {
        let id = p.id.as_bytes();
        id.len() == 1 + suffix.len() && id[0] == band && &p.id[1..] == suffix
    })
}

/// The WING EQ's bands in the order the EQ screen lists them (low band,
/// 1..n, high band): every "<b>f" parameter with a matching "<b>g". Yields
/// the band letters (b'l', b'1', ... b'h').
pub fn std_bands(spec: &ModelSpec) -> impl Iterator<Item = u8> + '_ {
    spec.params.iter().filter_map(move |p| {
        let id = p.id.as_bytes();
        (id.len() == 2 && id[1] == b'f' && find(spec, id[0], "g").is_some()).then_some(id[0])
    })
}

/// The filter BAND SOLO plays for band `band` (an index into [`std_bands`])
/// of `spec` with parameter `values`. None for models without the WING EQ's
/// bands (analog models) or an unknown band.
pub fn band_solo_coeffs(spec: &ModelSpec, values: &[f32], band: usize, sr: f64) -> Option<BiquadCoeffs> {
    if spec.token != "STD" {
        return None;
    }
    let id = std_bands(spec).nth(band)?;
    let p = Params::new(spec, values);
    let value = |suffix: &str| find(spec, id, suffix).map(|i| p.f(spec.params[i].id));
    let f = value("f")? as f64;
    let q = value("q").map_or(FRAC_1_SQRT_2, |q| q as f64).clamp(0.3, 20.0);
    let kind = find(spec, id, "eq").map_or("PEQ", |i| p.text(spec.params[i].id));
    Some(match id {
        b'l' if kind != "PEQ" => BiquadCoeffs::low_pass(sr, f, FRAC_1_SQRT_2),
        b'h' if kind != "PEQ" => BiquadCoeffs::high_pass(sr, f, FRAC_1_SQRT_2),
        _ => BiquadCoeffs::band_pass(sr, f, q),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use livedaw_dsp::wing::{model_index, EQ_MODELS_BUS, EQ_MODELS_CH, MAX_EQ_PARAMS};

    #[test]
    fn the_wing_eq_bands_and_their_filters() {
        let spec = &EQ_MODELS_CH[model_index(EQ_MODELS_CH, "STD").unwrap()];
        let bands: Vec<char> = std_bands(spec).map(char::from).collect();
        assert_eq!(bands.first(), Some(&'l'));
        assert_eq!(bands.last(), Some(&'h'));
        assert!(bands.contains(&'1') && bands.contains(&'4'));
        let bus = &EQ_MODELS_BUS[model_index(EQ_MODELS_BUS, "STD").unwrap()];
        assert!(std_bands(bus).count() > bands.len() - 1);

        let mut values = [0.0; MAX_EQ_PARAMS];
        spec.defaults_into(&mut values);
        let sr = 48000.0;
        // Band "1": a band pass at its frequency.
        let f1 = values[spec.param_index("1f").unwrap()] as f64;
        let c = band_solo_coeffs(spec, &values, 1, sr).unwrap();
        assert!((c.magnitude_at(f1, sr) - 1.0).abs() < 1e-3);
        assert!(c.magnitude_at(f1 * 8.0, sr) < 0.2 && c.magnitude_at(f1 / 8.0, sr) < 0.2);
        // The low band (a shelf or cut): what lies below it.
        let fl = values[spec.param_index("lf").unwrap()] as f64;
        let low = band_solo_coeffs(spec, &values, 0, sr).unwrap();
        assert!(low.magnitude_at(fl / 4.0, sr) > 0.9 && low.magnitude_at(fl * 8.0, sr) < 0.05);
        // The high band: what lies above it.
        let fh = values[spec.param_index("hf").unwrap()] as f64;
        let high = band_solo_coeffs(spec, &values, bands.len() - 1, sr).unwrap();
        assert!(high.magnitude_at((fh * 2.0).min(20000.0), sr) > 0.8 && high.magnitude_at(fh / 8.0, sr) < 0.05);
        // Out of range, or an analog model: nothing.
        assert!(band_solo_coeffs(spec, &values, 99, sr).is_none());
        let soul = &EQ_MODELS_CH[model_index(EQ_MODELS_CH, "SOUL").unwrap()];
        assert!(band_solo_coeffs(soul, &values, 0, sr).is_none());
    }
}
