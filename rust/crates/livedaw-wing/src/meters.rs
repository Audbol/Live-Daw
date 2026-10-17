//! Native protocol meters: the families a client can request and the words
//! each returns (protocol document "Meter Data"; layouts as libwing decodes
//! them). Levels are signed 16-bit 1/256 dB.
//!
//! Live DAW's meters are "peak since the UI last read" atomics; they are
//! loaded here (never reset), so the values follow the UI's metering rate.

use crate::map::Console;
use livedaw_engine::{Strip, StripKind};
use std::sync::atomic::Ordering::Relaxed;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Meter {
    /// Family opcode (0xa0..0xaf) and 0-based index.
    Indexed(u8, u8),
    Monitor,
    Rta,
}

impl Meter {
    /// A meter from its family opcode and (for indexed families) its 0-based index byte.
    pub fn from_wire(op: u8, index: Option<u8>) -> Option<Meter> {
        match (op, index) {
            (0xa9, None) => Some(Meter::Monitor),
            (0xaa, None) => Some(Meter::Rta),
            (0xa9 | 0xaa, Some(_)) | (_, None) => None,
            (0xa0..=0xaf, Some(i)) => Some(Meter::Indexed(op, i)),
            _ => None,
        }
    }

    /// Words this meter returns.
    pub fn words(self) -> usize {
        match self {
            Meter::Monitor => 6,
            Meter::Rta => 120,
            Meter::Indexed(op, _) => match op {
                0xa0..=0xa4 => 8,
                0xa5 => 4,
                0xa6 => 10,
                0xa7 | 0xa8 => 8,
                _ => 11, // V2 families
            },
        }
    }
}

/// A level (linear peak) as 1/256 dB.
pub fn level_word(peak: f32) -> i16 {
    if peak <= 0.0 || !peak.is_finite() {
        return i16::MIN;
    }
    let db = 20.0 * peak.log10();
    (db * 256.0).round().clamp(i16::MIN as f32, i16::MAX as f32) as i16
}

/// Gain reduction in dB as the document's scale (256 = 20 dB).
fn reduction_word(db: f32) -> i16 {
    (db.abs() / 20.0 * 256.0).round().clamp(0.0, i16::MAX as f32) as i16
}

fn strip_words(s: Option<&std::sync::Arc<Strip>>, v2: bool, out: &mut Vec<i16>) {
    let Some(s) = s else {
        out.extend(std::iter::repeat_n(i16::MIN, if v2 { 11 } else { 8 }));
        return;
    };
    let m = &s.meters;
    let (il, ir) = (m.input_peak[0].load(), m.input_peak[1].load());
    let (ol, or) = (m.output_peak[0].load(), m.output_peak[1].load());
    let stereo = s.num_channels() > 1;
    out.push(level_word(il));
    out.push(level_word(if stereo { ir } else { il }));
    out.push(level_word(ol));
    out.push(level_word(if stereo { or } else { ol }));
    out.push(level_word(il.max(ir))); // gate key: the input
    out.push(reduction_word(m.gate_reduction_db.load()));
    if v2 {
        out.push(m.gate_active.load(Relaxed) as i16);
    }
    out.push(level_word(il.max(ir))); // dynamics key
    out.push(reduction_word(m.dyn_reduction_db.load()));
    if v2 {
        out.push(m.dyn_active.load(Relaxed) as i16);
        out.push((m.automix_db.load() * 256.0).round().clamp(i16::MIN as f32, i16::MAX as f32) as i16);
    }
}

/// The words of one meter frame for `meters`, in request order.
pub fn frame_words(c: &Console, meters: &[Meter]) -> Vec<i16> {
    let mut out = Vec::with_capacity(meters.iter().map(|m| m.words()).sum());
    let ctl = &c.engine.controls;
    for &m in meters {
        match m {
            Meter::Indexed(op, i) => {
                let n = i as u16 + 1;
                let kind = |k| (k, op >= 0xab);
                let strip = match op {
                    0xa0 | 0xab => Some(kind(StripKind::Channel)),
                    0xa1 | 0xac => Some(kind(StripKind::Aux)),
                    0xa2 | 0xad => Some(kind(StripKind::Bus)),
                    0xa3 | 0xae => Some(kind(StripKind::Main)),
                    0xa4 | 0xaf => Some(kind(StripKind::Matrix)),
                    _ => None,
                };
                if let Some((k, v2)) = strip {
                    strip_words(c.strip(k, n), v2, &mut out);
                    continue;
                }
                match op {
                    0xa5 => {
                        // DCAs carry no signal here: the loudest member's post-fader level.
                        let bit = 1u32.checked_shl(i as u32).unwrap_or(0);
                        let mut peak = 0.0f32;
                        for kind in StripKind::ALL {
                            for s in c.engine.strips(kind) {
                                if s.params.dca_mask.load(Relaxed) & bit != 0 {
                                    peak = peak.max(s.meters.output_peak[0].load()).max(s.meters.output_peak[1].load());
                                }
                            }
                        }
                        let w = level_word(peak);
                        out.extend([w, w, w, w]);
                    }
                    0xa6 => {
                        let level = c.engine.fx(i as usize).map_or(0.0, |r| r.meter());
                        let w = level_word(level);
                        out.extend([w, w, w, w]);
                        out.extend([0i16; 6]);
                    }
                    _ => out.extend(std::iter::repeat_n(i16::MIN, m.words())),
                }
            }
            Meter::Monitor => {
                let (l, r) = (level_word(ctl.monitor_peak[0].load()), level_word(ctl.monitor_peak[1].load()));
                out.extend([l, r, l, r, l, r]);
            }
            Meter::Rta => out.extend(std::iter::repeat_n(i16::MIN, 120)),
        }
    }
    out
}

/// A meter datagram: the 4-byte report id, then the words big-endian.
pub fn datagram(report_id: u32, words: &[i16]) -> Vec<u8> {
    let mut out = Vec::with_capacity(4 + words.len() * 2);
    out.extend_from_slice(&report_id.to_be_bytes());
    for w in words {
        out.extend_from_slice(&w.to_be_bytes());
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn words_and_levels() {
        assert_eq!(level_word(1.0), 0);
        assert_eq!(level_word(0.5), (-6.0206f32 * 256.0).round() as i16);
        assert_eq!(level_word(0.0), i16::MIN);
        assert_eq!(Meter::from_wire(0xa0, Some(0)).unwrap().words(), 8);
        assert_eq!(Meter::from_wire(0xab, Some(3)).unwrap().words(), 11);
        assert_eq!(Meter::from_wire(0xa9, None), Some(Meter::Monitor));
        assert_eq!(datagram(0x0001_2345, &[1, -1]), [0, 1, 0x23, 0x45, 0, 1, 0xff, 0xff]);
    }
}
