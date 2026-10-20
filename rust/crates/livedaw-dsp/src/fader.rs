//! The WING's fader law: where a level in dB sits on a fader's travel.

/// The fader's silence (-oo).
pub const FADER_MIN: f32 = -144.0;
pub const FADER_MAX: f32 = 10.0;

/// WING fader law (position 0..1 for a level in dB), as on the X32/WING: four
/// linear segments meeting at -60, -30 and -10 dB, 0 dB at 0.75 and +10 dB at 1.
/// The two upper segments match the protocol document's examples (0 dB =
/// 0.75, -3 dB = 0.675, +4 dB = 0.85); the lower ones follow the X32.
pub fn fader_to_pos(db: f32) -> f32 {
    let db = db.clamp(FADER_MIN, FADER_MAX);
    let pos = if db >= -10.0 {
        (db + 30.0) / 40.0
    } else if db >= -30.0 {
        (db + 50.0) / 80.0
    } else if db >= -60.0 {
        (db + 70.0) / 160.0
    } else {
        // -60 dB = 0.0625 down to -oo = 0.
        0.0625 * (db - FADER_MIN) / (-60.0 - FADER_MIN)
    };
    pos.clamp(0.0, 1.0)
}

pub fn pos_to_fader(pos: f32) -> f32 {
    let p = pos.clamp(0.0, 1.0);
    if p >= 0.5 {
        p * 40.0 - 30.0
    } else if p >= 0.25 {
        p * 80.0 - 50.0
    } else if p >= 0.0625 {
        p * 160.0 - 70.0
    } else {
        FADER_MIN + p / 0.0625 * (-60.0 - FADER_MIN)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn positions_round_trip() {
        for db in [FADER_MIN, -90.0, -60.0, -45.0, -30.0, -20.0, -10.0, -3.0, 0.0, 4.0, 10.0] {
            assert!((pos_to_fader(fader_to_pos(db)) - db).abs() < 1e-3, "{db} dB");
        }
        assert_eq!(fader_to_pos(0.0), 0.75);
    }
}
