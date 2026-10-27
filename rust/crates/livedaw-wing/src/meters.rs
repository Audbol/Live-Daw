//! Native protocol meters: the families a client can request and the words
//! each returns (protocol document "Meter Data"; layouts as libwing decodes
//! them). Levels are signed 16-bit 1/256 dB.
//!
//! The console sends a frame about every 50 ms ("every approximately 50ms",
//! protocol document, Meters) and its clients draw the words as they come:
//! WING Edit 3.3.4 shows a step down, a 40 ms burst or a 0.2 s over on the
//! very next frame, with no decay, hold or clip latch of its own. So the
//! levels carry the meter ballistics: [`MeterClock`] takes the engine's peaks
//! since its last tick (its own copy, [`livedaw_engine::strip::RemotePeaks`];
//! the UI takes the others) and keeps a level per meter that rises at once and
//! falls [`RELEASE_DB_PER_S`], the release of Sidecar's own meters
//! (`src/ui/MeterBallistics.h`), so a remote reads what the console's screens
//! and surface show.

use crate::map::Console;
use livedaw_dsp::wing::DYN_MODELS;
use livedaw_engine::{Engine, Strip, StripKind};
use std::collections::HashMap;
use std::sync::atomic::Ordering::Relaxed;
use std::sync::Arc;
use std::time::{Duration, Instant};

/// The console's meter frame period.
pub const METER_PERIOD: Duration = Duration::from_millis(50);
/// How fast a level falls, dB per second (keep in step with
/// `meters::kReleaseDbPerSecond` in `src/ui/MeterBallistics.h`).
pub const RELEASE_DB_PER_S: f32 = 24.0;
/// The bottom of a level: the lowest word (-128 dB), "-oo".
pub const FLOOR_DB: f32 = -128.0;
/// A tick this long after the last one starts the levels over (nobody was
/// listening; the peaks gathered meanwhile are stale).
const IDLE: Duration = Duration::from_secs(1);

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

/// A level meter: instant attack, a linear fall in dB.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Ballistic {
    pub db: f32,
}

impl Default for Ballistic {
    fn default() -> Self {
        Self { db: FLOOR_DB }
    }
}

impl Ballistic {
    /// Moves on by `dt` seconds with `peak` (linear, the peak in that time);
    /// returns the level in dB.
    pub fn advance(&mut self, peak: f32, dt: f32) -> f32 {
        let fallen = (self.db - RELEASE_DB_PER_S * dt.max(0.0)).max(FLOOR_DB);
        self.db = gain_to_db(peak).max(fallen);
        self.db
    }
}

fn gain_to_db(peak: f32) -> f32 {
    if peak > 0.0 && peak.is_finite() {
        (20.0 * peak.log10()).max(FLOOR_DB)
    } else {
        FLOOR_DB
    }
}

/// The input, pre-fader and output levels of a strip, L/R.
type StripLevels = [[Ballistic; 2]; 3];
const INPUT: usize = 0;
const PRE: usize = 1;
const OUTPUT: usize = 2;

/// The meter levels the stream sends, moved on once per frame tick.
#[derive(Default)]
pub struct MeterClock {
    last: Option<Instant>,
    strips: HashMap<usize, StripLevels>,
    /// SPEAKERS, PHONES; L/R.
    monitor: [[Ballistic; 2]; 2],
}

fn key(s: &Arc<Strip>) -> usize {
    Arc::as_ptr(s) as usize
}

impl MeterClock {
    /// Takes the engine's peaks since the last tick and moves every level on
    /// to `now`.
    pub fn tick(&mut self, engine: &Engine, now: Instant) {
        let since = self.last.map(|t| now.saturating_duration_since(t));
        self.last = Some(now);
        let fresh = since.is_none_or(|d| d >= IDLE);
        let dt = since.map_or(0.0, |d| d.as_secs_f32());
        let mut seen = HashMap::with_capacity(self.strips.len());
        for kind in StripKind::ALL {
            for s in engine.strips(kind) {
                let peaks = s.remote_peaks.take();
                let mut levels =
                    if fresh { StripLevels::default() } else { self.strips.get(&key(&s)).copied().unwrap_or_default() };
                if !fresh {
                    for (point, pair) in levels.iter_mut().enumerate() {
                        for (ch, b) in pair.iter_mut().enumerate() {
                            b.advance(peaks[point][ch], dt);
                        }
                    }
                }
                seen.insert(key(&s), levels);
            }
        }
        self.strips = seen;
        for (path, pair) in self.monitor.iter_mut().enumerate() {
            for (ch, b) in pair.iter_mut().enumerate() {
                let peak = engine.monitor_remote_peaks[path][ch].exchange(0.0);
                if fresh {
                    *b = Ballistic::default();
                } else {
                    b.advance(peak, dt);
                }
            }
        }
    }

    /// A strip's level in dB at `point` (INPUT, PRE, OUTPUT), channel 0/1.
    fn level(&self, s: &Arc<Strip>, point: usize, ch: usize) -> f32 {
        self.strips.get(&key(s)).map_or(FLOOR_DB, |l| l[point][ch].db)
    }
}

/// A level in dB as 1/256 dB (the floor and below: the lowest word).
pub fn db_word(db: f32) -> i16 {
    if db <= FLOOR_DB || db.is_nan() {
        return i16::MIN;
    }
    (db * 256.0).round().clamp(i16::MIN as f32, i16::MAX as f32) as i16
}

/// A level (linear peak) as 1/256 dB.
pub fn level_word(peak: f32) -> i16 {
    db_word(gain_to_db(peak))
}

/// Gain reduction in dB as the document's scale: 256 = `full_scale` dB
/// ("for most of them 1.0 (256) maps to 20 dB gain reduction. Standard Wing
/// gate is 60 dB").
fn reduction_word(db: f32, full_scale: f32) -> i16 {
    (db.abs() / full_scale * 256.0).round().clamp(0.0, i16::MAX as f32) as i16
}

/// The full scale of a strip's gate gain word: 60 dB for the WING gate, else 20.
fn gate_scale(s: &Strip) -> f32 {
    let model = s.params.gate.model.load(Relaxed).max(0) as usize;
    if DYN_MODELS.get(model).is_some_and(|m| m.token == "GATE") {
        60.0
    } else {
        20.0
    }
}

fn strip_words(clock: &MeterClock, s: Option<&Arc<Strip>>, v2: bool, out: &mut Vec<i16>) {
    let Some(s) = s else {
        out.extend(std::iter::repeat_n(i16::MIN, if v2 { 11 } else { 8 }));
        return;
    };
    let m = &s.meters;
    // A mono strip shows its one side on both.
    let right = usize::from(s.num_channels() > 1);
    let level = |point, ch| clock.level(s, point, ch);
    let (il, ir) = (level(INPUT, 0), level(INPUT, right));
    // Keys: the slot's detector while it works, else the strip's input.
    let key = |active: bool, key_db: f32| if active && key_db > -119.0 { key_db } else { il.max(ir) };
    let gate_on = s.params.gate.on.load(Relaxed);
    let dyn_on = s.params.dynamics.on.load(Relaxed);
    out.push(db_word(il));
    out.push(db_word(ir));
    out.push(db_word(level(OUTPUT, 0)));
    out.push(db_word(level(OUTPUT, right)));
    out.push(db_word(key(gate_on, m.gate_key_db.load())));
    out.push(reduction_word(m.gate_reduction_db.load(), gate_scale(s)));
    if v2 {
        out.push(m.gate_active.load(Relaxed) as i16);
    }
    out.push(db_word(key(dyn_on, m.dyn_key_db.load())));
    out.push(reduction_word(m.dyn_reduction_db.load(), 20.0));
    if v2 {
        out.push(m.dyn_active.load(Relaxed) as i16);
        out.push((m.automix_db.load() * 256.0).round().clamp(i16::MIN as f32, i16::MAX as f32) as i16);
    }
}

/// The words of one meter frame for `meters`, in request order, from the
/// levels of `clock`'s latest tick.
pub fn frame_words(c: &Console, clock: &MeterClock, meters: &[Meter]) -> Vec<i16> {
    let mut out = Vec::with_capacity(meters.iter().map(|m| m.words()).sum());
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
                    strip_words(clock, c.strip(k, n), v2, &mut out);
                    continue;
                }
                match op {
                    0xa5 => {
                        // Pre fader L/R, post fader L/R: DCAs carry no signal
                        // here, so the loudest member's, as the METERS screen.
                        let bit = 1u32.checked_shl(i as u32).unwrap_or(0);
                        let mut words = [FLOOR_DB; 4];
                        for kind in StripKind::ALL {
                            for s in c.engine.strips(kind) {
                                if s.params.dca_mask.load(Relaxed) & bit != 0 {
                                    let right = usize::from(s.num_channels() > 1);
                                    for (w, (point, ch)) in
                                        words.iter_mut().zip([(PRE, 0), (PRE, right), (OUTPUT, 0), (OUTPUT, right)])
                                    {
                                        *w = w.max(clock.level(&s, point, ch));
                                    }
                                }
                            }
                        }
                        out.extend(words.map(db_word));
                    }
                    0xa6 => {
                        // No FX input/output levels here; the state meters'
                        // first five are the dynamic bands' gains (dB * 2048 / 6).
                        out.extend([i16::MIN; 4]);
                        let fx = c.engine.fx(i as usize);
                        for band in 0..6 {
                            let db = if band < 5 { fx.map_or(0.0, |r| r.band_gain(band)) } else { 0.0 };
                            out.push((db * 2048.0 / 6.0).round().clamp(i16::MIN as f32, i16::MAX as f32) as i16);
                        }
                    }
                    _ => out.extend(std::iter::repeat_n(i16::MIN, m.words())),
                }
            }
            Meter::Monitor => {
                // Solo bus, MON 1 (PHONES, "MON A"), MON 2 (SPEAKERS, "MON B");
                // the solo bus as the speakers hear it.
                let spk = clock.monitor[0].map(|b| db_word(b.db));
                let ph = clock.monitor[1].map(|b| db_word(b.db));
                out.extend([spk[0], spk[1], ph[0], ph[1], spk[0], spk[1]]);
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

/// The tick after `next` on a fixed `period` grid, never earlier than `now`
/// (a late tick skips ahead rather than bunching up frames).
pub fn next_tick(next: Instant, now: Instant, period: Duration) -> Instant {
    let due = next + period;
    if due > now {
        return due;
    }
    let behind = now.duration_since(due).as_nanos() / period.as_nanos().max(1);
    due + period * (behind as u32 + 1)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn words_and_levels() {
        assert_eq!(level_word(1.0), 0);
        assert_eq!(level_word(0.5), (-6.0206f32 * 256.0).round() as i16);
        assert_eq!(level_word(0.0), i16::MIN);
        assert_eq!(db_word(FLOOR_DB), i16::MIN);
        assert_eq!(Meter::from_wire(0xa0, Some(0)).unwrap().words(), 8);
        assert_eq!(Meter::from_wire(0xab, Some(3)).unwrap().words(), 11);
        assert_eq!(Meter::from_wire(0xa9, None), Some(Meter::Monitor));
        assert_eq!(datagram(0x0001_2345, &[1, -1]), [0, 1, 0x23, 0x45, 0, 1, 0xff, 0xff]);
        assert_eq!(reduction_word(-20.0, 20.0), 256);
        assert_eq!(reduction_word(30.0, 60.0), 128);
    }

    #[test]
    fn ballistics_rise_at_once_and_fall_at_the_release_rate() {
        let mut b = Ballistic::default();
        // Attack: the first frame with the peak shows it in full.
        assert!((b.advance(0.5, 0.05) + 6.02).abs() < 0.01);
        // Release: 24 dB/s, frame by frame, whatever the frame rate.
        let mut t = 0.0;
        while t < 0.999 {
            b.advance(0.0, 0.05);
            t += 0.05;
        }
        assert!((b.db - (-6.02 - RELEASE_DB_PER_S)).abs() < 0.05, "{}", b.db);
        // A louder peak meanwhile takes over at once; a quieter one doesn't.
        b.advance(0.1, 0.05);
        assert!((b.db + 20.0).abs() < 0.05, "{}", b.db);
        b.advance(0.01, 0.05);
        assert!((b.db - (-20.0 - RELEASE_DB_PER_S * 0.05)).abs() < 0.05, "{}", b.db);
        // Silence falls to the floor and stays there.
        b.advance(0.0, 10.0);
        assert_eq!(b.db, FLOOR_DB);
        assert_eq!(db_word(b.db), i16::MIN);
    }

    #[test]
    fn ticks_stay_on_their_grid() {
        let t0 = Instant::now();
        let p = METER_PERIOD;
        assert_eq!(next_tick(t0, t0, p), t0 + p);
        // Work that took 10 ms doesn't push the grid back.
        assert_eq!(next_tick(t0 + p, t0 + p + Duration::from_millis(10), p), t0 + 2 * p);
        // A stall of 3.2 periods skips to the next tick in the future.
        assert_eq!(next_tick(t0, t0 + p * 4 + Duration::from_millis(10), p), t0 + 5 * p);
    }

    #[test]
    fn the_clock_takes_the_peaks_and_decays() {
        let engine = Engine::new(0);
        engine.set_num_channels(2);
        let ch = engine.strips(StripKind::Channel)[0].clone();
        let mut clock = MeterClock::default();
        let t0 = Instant::now();
        clock.tick(&engine, t0); // starts the clock (levels at the floor)
        ch.remote_peaks.input[0].store(0.5);
        ch.remote_peaks.output[1].store(0.25);
        clock.tick(&engine, t0 + METER_PERIOD);
        assert!((clock.level(&ch, INPUT, 0) + 6.02).abs() < 0.01);
        assert!((clock.level(&ch, OUTPUT, 1) + 12.04).abs() < 0.01);
        // The clock took them: the next tick only decays.
        assert_eq!(ch.remote_peaks.input[0].load(), 0.0);
        clock.tick(&engine, t0 + METER_PERIOD * 11);
        assert!((clock.level(&ch, INPUT, 0) - (-6.02 - RELEASE_DB_PER_S * 0.5)).abs() < 0.02);
        // After an idle second the levels start over, the stale peaks dropped.
        ch.remote_peaks.input[0].store(1.0);
        clock.tick(&engine, t0 + Duration::from_secs(5));
        assert_eq!(clock.level(&ch, INPUT, 0), FLOOR_DB);
    }
}
