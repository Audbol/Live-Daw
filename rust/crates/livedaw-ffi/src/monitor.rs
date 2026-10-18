//! Monitor path processing and SOURCE SOLO (ABI 8). The settings are fields
//! of `EngineControls::monitor` (shared memory); these are the helpers the UI
//! needs beyond them: names, the EQ curves and the SOURCE SOLO host.

use crate::session::LdString;
use crate::{engine, guard};
use livedaw_engine::monitor::{self, direct_source_name, source_solo_host};
use livedaw_engine::{Engine, Strip};
use std::sync::atomic::Ordering::Relaxed;

/// The display name of a DIRECT IN index ("CH.12", "AUX.6", "MTX.3"; "OFF").
#[no_mangle]
pub extern "C" fn ld_monitor_direct_name(index: i32) -> *mut LdString {
    LdString::new(guard(String::new(), || direct_source_name(index)))
}

/// Milliseconds of delay for a distance in meters (the DIST slider).
#[no_mangle]
pub extern "C" fn ld_monitor_delay_ms(meters: f32) -> f32 {
    monitor::delay_ms(meters)
}

/// Response in dB of monitor path `path`'s EQ (0 = SPEAKERS, 1 = PHONES) at
/// `freqs`: the whole EQ (`band` < 0; flat while off) or one band's curve.
#[no_mangle]
pub unsafe extern "C" fn ld_monitor_eq_response(
    e: *const Engine,
    path: i32,
    band: i32,
    freqs: *const f32,
    out_db: *mut f32,
    n: i32,
) {
    let Some(e) = engine(e) else { return };
    let Some(p) = usize::try_from(path).ok().and_then(|i| e.controls.monitor.paths.get(i)) else { return };
    if freqs.is_null() || out_db.is_null() || n <= 0 {
        return;
    }
    let freqs = std::slice::from_raw_parts(freqs, n as usize);
    let out = std::slice::from_raw_parts_mut(out_db, n as usize);
    guard((), || {
        let sr = e.sample_rate();
        if band < 0 {
            let d = p.eq_design(sr);
            for (o, &f) in out.iter_mut().zip(freqs) {
                *o = d.response_db(f as f64, sr) as f32;
            }
        } else {
            let c = p.band_design(band as usize, sr);
            for (o, &f) in out.iter_mut().zip(freqs) {
                *o = c.map_or(0.0, |c| (20.0 * c.magnitude_at(f as f64, sr).max(1e-9).log10()) as f32);
            }
        }
    });
}

/// The strip SOURCE SOLO plays on (CH39 or AUX7), or null when SOURCE SOLO is OFF.
#[no_mangle]
pub unsafe extern "C" fn ld_engine_source_solo_host(e: *const Engine) -> *const Strip {
    let Some(e) = engine(e) else { return std::ptr::null() };
    guard(std::ptr::null(), || {
        source_solo_host(e.controls.monitor.source_solo.load(Relaxed))
            .and_then(|(kind, i)| e.strip_by_number(kind, i as i32 + 1))
            .map_or(std::ptr::null(), |s| std::sync::Arc::as_ptr(&s))
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{ld_engine_create, ld_engine_destroy, ld_engine_set_num_channels};

    #[test]
    fn monitor_helpers_through_the_c_abi() {
        unsafe {
            let s = ld_monitor_direct_name(45);
            let text = std::slice::from_raw_parts(crate::session::ld_string_data(s), crate::session::ld_string_len(s));
            assert_eq!(text, b"AUX.6");
            crate::session::ld_string_free(s);
            assert!((ld_monitor_delay_ms(3.43) - 10.0).abs() < 1e-3);

            let e = ld_engine_create(0);
            ld_engine_set_num_channels(e, 40);
            assert!(ld_engine_source_solo_host(e).is_null());
            (*e).controls.monitor.source_solo.store(1, Relaxed);
            let host = ld_engine_source_solo_host(e);
            assert_eq!((*host).kind(), livedaw_engine::StripKind::Channel);
            assert_eq!((*e).channel_at(38).map(|s| std::sync::Arc::as_ptr(&s)), Some(host));

            let p = &(*e).controls.monitor.paths[1];
            p.eq_on.store(true, Relaxed);
            p.eq[3].gain_db.store(6.0);
            p.eq[3].freq.store(1000.0);
            let freqs = [100.0f32, 1000.0];
            let mut out = [0.0f32; 2];
            ld_monitor_eq_response(e, 1, -1, freqs.as_ptr(), out.as_mut_ptr(), 2);
            assert!((out[1] - 6.0).abs() < 0.1 && out[0].abs() < 1.0, "{out:?}");
            ld_monitor_eq_response(e, 1, 3, freqs.as_ptr(), out.as_mut_ptr(), 2);
            assert!((out[1] - 6.0).abs() < 0.1);
            ld_monitor_eq_response(e, 0, -1, freqs.as_ptr(), out.as_mut_ptr(), 2);
            assert_eq!(out, [0.0, 0.0], "SPEAKERS: flat");
            ld_engine_destroy(e);
        }
    }
}
