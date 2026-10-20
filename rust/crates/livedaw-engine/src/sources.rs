//! Sources (ROUTING > SOURCES, manual pp. 39-40): every hardware input port,
//! every recorder playback channel, the four USB PLAYER channels (the
//! USB deck's player, input `USB_PLAYER_SOURCE_BASE + n`) and the two
//! oscillators (the OSC group, input `OSC_SOURCE_BASE + n`) are sources with a name, colour and
//! icon, a gain, mute, polarity, +48 V (stored only: there is no preamp to
//! switch) and a pairing: MONO, or STEREO / MID-SIDE with its neighbour
//! (ports 1-2, 3-4, ...; the pair shares the first port's settings).
//!
//! The oscillators (`/io/in/OSC/1..2/osc`): each generates a sine, pink or
//! white noise ([`livedaw_dsp::generator`]) at its LEVEL and FREQ; its source
//! MUTE switches it off, as on the WING (the OSC sources have no gain,
//! polarity or +48 V there). A STEREO / MID-SIDE pair runs on OSC 1's
//! settings: the sine in phase on both sides, the noises uncorrelated.
//!
//! Channels patched to a source get its processing first: once per block,
//! before the channels run, the engine processes each source whose settings
//! change its signal into a buffer of its own and hands that buffer to the
//! channels (and to the recorder's input streams) instead of the port's.
//! Sources at their defaults cost nothing: their port is passed through.
//!
//! A MID/SIDE pair decodes to left = M + S, right = M - S (M on the first port).
//!
//! Source tags (SOURCES > TAGS): a source can belong to mute groups; it is
//! muted while one of them is engaged (the WING's `/io/in/../tags` `#M1`...).

use crate::atomic::AtomicF32;
use livedaw_dsp::generator::{generator_mode, SignalGenerator, GENERATOR_MODES};
use livedaw_dsp::generator::{GENERATOR_FREQ_MAX_HZ, GENERATOR_FREQ_MIN_HZ, GENERATOR_LEVEL_MAX_DB, GENERATOR_LEVEL_MIN_DB};
use livedaw_dsp::util::{apply_gain_ramp, db_to_gain, peak_level};
use livedaw_recorder::{playback_channel, usb_player_channel, NUM_PLAYBACK_CHANNELS, PLAYBACK_SOURCE_BASE};
use livedaw_recorder::{USB_CHANNELS, USB_PLAYER_SOURCE_BASE};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering::Relaxed};
use std::sync::Mutex;

/// Hardware input ports with a source (12 groups of 48); ports beyond pass through.
pub const NUM_HW_SOURCES: usize = 576;
/// The oscillators (the OSC source group): OSC n is input `OSC_SOURCE_BASE + n - 1`.
pub const NUM_OSCILLATORS: usize = 2;
pub const OSC_SOURCE_BASE: i32 = 0x30000;
/// Sources: the hardware ports, the playback channels, the USB PLAYER, then the oscillators.
pub const NUM_SOURCES: usize = NUM_HW_SOURCES + NUM_PLAYBACK_CHANNELS + USB_CHANNELS + NUM_OSCILLATORS;
/// Table index of USB PLAYER 1.
pub const USB_PLAYER_SOURCE_INDEX: usize = NUM_HW_SOURCES + NUM_PLAYBACK_CHANNELS;
/// Table index of OSC 1.
pub const OSC_SOURCE_INDEX: usize = USB_PLAYER_SOURCE_INDEX + USB_CHANNELS;

/// The oscillator an input index refers to, if any.
pub fn osc_channel(input: i32) -> Option<usize> {
    let n = input.checked_sub(OSC_SOURCE_BASE)?;
    usize::try_from(n).ok().filter(|&n| n < NUM_OSCILLATORS)
}

/// The oscillators' defaults: a 1 kHz sine at -20 dBFS.
pub const OSC_DEFAULT_LEVEL_DB: f32 = -20.0;
pub const OSC_DEFAULT_FREQ_HZ: f32 = 1000.0;

/// One oscillator's settings, shared with the UI (`ld_engine_oscillators`).
/// Its on / off is its source's MUTE.
#[repr(C)]
#[derive(Debug)]
pub struct OscParams {
    /// Index into [`GENERATOR_MODES`] (SINE, PINK, WHITE).
    pub mode: AtomicI32,
    /// LEVEL, dBFS (the sine's peak; the noises have the sine's RMS).
    pub level_db: AtomicF32,
    pub freq_hz: AtomicF32,
}

impl Default for OscParams {
    fn default() -> Self {
        Self {
            mode: AtomicI32::new(generator_mode::SINE),
            level_db: AtomicF32::new(OSC_DEFAULT_LEVEL_DB),
            freq_hz: AtomicF32::new(OSC_DEFAULT_FREQ_HZ),
        }
    }
}

impl OscParams {
    pub fn reset(&self) {
        self.mode.store(generator_mode::SINE, Relaxed);
        self.level_db.store(OSC_DEFAULT_LEVEL_DB);
        self.freq_hz.store(OSC_DEFAULT_FREQ_HZ);
    }

    pub fn is_default(&self) -> bool {
        self.mode.load(Relaxed) == generator_mode::SINE
            && self.level_db.load() == OSC_DEFAULT_LEVEL_DB
            && self.freq_hz.load() == OSC_DEFAULT_FREQ_HZ
    }

    /// Mode, level and frequency, in range.
    fn get(&self) -> (i32, f32, f32) {
        (
            self.mode.load(Relaxed).clamp(0, GENERATOR_MODES.len() as i32 - 1),
            self.level_db.load().clamp(GENERATOR_LEVEL_MIN_DB, GENERATOR_LEVEL_MAX_DB),
            self.freq_hz.load().clamp(GENERATOR_FREQ_MIN_HZ, GENERATOR_FREQ_MAX_HZ),
        )
    }
}

/// Source pairing.
pub const SOURCE_MODES: [&str; 3] = ["MONO", "STEREO", "MID/SIDE"];

pub mod source_mode {
    pub const MONO: i32 = 0;
    pub const STEREO: i32 = 1;
    pub const MID_SIDE: i32 = 2;
}

/// GAIN range in dB.
pub const SOURCE_GAIN_MIN_DB: f32 = -12.0;
pub const SOURCE_GAIN_MAX_DB: f32 = 60.0;

/// Default source colour (WING colour 1, the console's source blue).
pub const DEFAULT_SOURCE_COLOUR: i32 = 1;

/// One source's settings, shared with the UI (`ld_engine_sources`).
#[repr(C)]
#[derive(Debug)]
pub struct SourceParams {
    pub gain_db: AtomicF32,
    pub mute: AtomicBool,
    pub invert: AtomicBool,
    /// +48 V phantom power (stored with the show; nothing to switch here).
    pub phantom: AtomicBool,
    /// Index into [`SOURCE_MODES`]; both sources of a pair hold the same mode.
    pub mode: AtomicI32,
    /// WING colour 1..18 and icon number (0 = none).
    pub colour: AtomicI32,
    pub icon: AtomicI32,
    /// Peak since the last read (after the source's processing).
    pub peak: AtomicF32,
    /// ABI 8, SOURCES > TAGS: bit n = mute group n+1 mutes the source.
    pub mute_groups: AtomicU32,
}

impl Default for SourceParams {
    fn default() -> Self {
        Self {
            gain_db: AtomicF32::new(0.0),
            mute: AtomicBool::new(false),
            invert: AtomicBool::new(false),
            phantom: AtomicBool::new(false),
            mode: AtomicI32::new(source_mode::MONO),
            colour: AtomicI32::new(DEFAULT_SOURCE_COLOUR),
            icon: AtomicI32::new(0),
            peak: AtomicF32::new(0.0),
            mute_groups: AtomicU32::new(0),
        }
    }
}

impl SourceParams {
    /// INITIALIZE: back to the defaults (the pairing is left to the table).
    fn reset(&self) {
        self.gain_db.store(0.0);
        self.mute.store(false, Relaxed);
        self.invert.store(false, Relaxed);
        self.phantom.store(false, Relaxed);
        self.colour.store(DEFAULT_SOURCE_COLOUR, Relaxed);
        self.icon.store(0, Relaxed);
        self.mute_groups.store(0, Relaxed);
    }

    /// True if the settings differ from the defaults (what a show stores).
    pub fn is_default(&self) -> bool {
        self.gain_db.load() == 0.0
            && !self.mute.load(Relaxed)
            && !self.invert.load(Relaxed)
            && !self.phantom.load(Relaxed)
            && self.mode.load(Relaxed) == source_mode::MONO
            && self.colour.load(Relaxed) == DEFAULT_SOURCE_COLOUR
            && self.icon.load(Relaxed) == 0
            && self.mute_groups.load(Relaxed) == 0
    }

    /// Muted: its own MUTE, or one of its mute groups is engaged.
    pub fn is_muted(&self, mute_groups: u32) -> bool {
        self.mute.load(Relaxed) || self.mute_groups.load(Relaxed) & mute_groups != 0
    }

    /// The gain the source applies (mute, mute groups, gain, polarity).
    fn gain(&self, mute_groups: u32) -> f32 {
        if self.is_muted(mute_groups) {
            return 0.0;
        }
        let g = db_to_gain(self.gain_db.load().clamp(SOURCE_GAIN_MIN_DB, SOURCE_GAIN_MAX_DB));
        if self.invert.load(Relaxed) {
            -g
        } else {
            g
        }
    }
}

/// The table index of input port `port` (a hardware input or a playback
/// channel), None for ports without a source.
pub fn source_index(port: i32) -> Option<usize> {
    if let Some(n) = osc_channel(port) {
        return Some(OSC_SOURCE_INDEX + n);
    }
    if let Some(n) = usb_player_channel(port) {
        return Some(USB_PLAYER_SOURCE_INDEX + n);
    }
    match playback_channel(port) {
        Some(n) => Some(NUM_HW_SOURCES + n),
        None => usize::try_from(port).ok().filter(|&p| p < NUM_HW_SOURCES),
    }
}

/// The input port of table index `index` (inverse of [`source_index`]).
pub fn source_port(index: usize) -> Option<i32> {
    if index < NUM_HW_SOURCES {
        Some(index as i32)
    } else if index < USB_PLAYER_SOURCE_INDEX {
        Some(PLAYBACK_SOURCE_BASE + (index - NUM_HW_SOURCES) as i32)
    } else if index < OSC_SOURCE_INDEX {
        Some(USB_PLAYER_SOURCE_BASE + (index - USB_PLAYER_SOURCE_INDEX) as i32)
    } else if index < NUM_SOURCES {
        Some(OSC_SOURCE_BASE + (index - OSC_SOURCE_INDEX) as i32)
    } else {
        None
    }
}

/// The first source of `index`'s pair.
pub fn pair_first(index: usize) -> usize {
    index & !1
}

/// Every source's settings and names.
pub struct SourceTable {
    params: Box<[SourceParams]>,
    /// Names (message thread; empty = the port's name).
    names: Mutex<Vec<String>>,
    osc: [OscParams; NUM_OSCILLATORS],
}

impl Default for SourceTable {
    fn default() -> Self {
        Self {
            params: (0..NUM_SOURCES).map(|_| SourceParams::default()).collect(),
            names: Mutex::new(vec![String::new(); NUM_SOURCES]),
            osc: Default::default(),
        }
    }
}

impl SourceTable {
    pub fn params(&self) -> &[SourceParams] {
        &self.params
    }

    pub fn get(&self, index: usize) -> Option<&SourceParams> {
        self.params.get(index)
    }

    /// Oscillator `n` (0-based).
    pub fn osc(&self, n: usize) -> Option<&OscParams> {
        self.osc.get(n)
    }

    pub fn oscillators(&self) -> &[OscParams] {
        &self.osc
    }

    pub fn name(&self, index: usize) -> String {
        self.names.lock().unwrap_or_else(|e| e.into_inner()).get(index).cloned().unwrap_or_default()
    }

    pub fn set_name(&self, index: usize, name: &str) {
        if let Some(n) = self.names.lock().unwrap_or_else(|e| e.into_inner()).get_mut(index) {
            *n = name.to_owned();
        }
    }

    /// Pairs `index` with its neighbour (STEREO, MID/SIDE) or splits the pair
    /// (MONO): both sources of the pair get the mode, and a new pair takes the
    /// first source's settings.
    pub fn set_mode(&self, index: usize, mode: i32) {
        let first = pair_first(index);
        let (Some(a), Some(b)) = (self.params.get(first), self.params.get(first + 1)) else { return };
        let mode = mode.clamp(0, SOURCE_MODES.len() as i32 - 1);
        if mode != source_mode::MONO {
            b.gain_db.store(a.gain_db.load());
            b.mute.store(a.mute.load(Relaxed), Relaxed);
            b.invert.store(a.invert.load(Relaxed), Relaxed);
            b.phantom.store(a.phantom.load(Relaxed), Relaxed);
            b.mute_groups.store(a.mute_groups.load(Relaxed), Relaxed);
        }
        a.mode.store(mode, Relaxed);
        b.mode.store(mode, Relaxed);
    }

    /// INITIALIZE: the source's settings (its pair's, if paired) and name back
    /// to the defaults; the pairing stays.
    pub fn initialize(&self, index: usize) {
        let Some(p) = self.params.get(index) else { return };
        let paired = p.mode.load(Relaxed) != source_mode::MONO;
        let first = pair_first(index);
        let indices = if paired { first..first + 2 } else { index..index + 1 };
        for i in indices {
            self.params[i].reset();
            self.set_name(i, "");
            if let Some(o) = i.checked_sub(OSC_SOURCE_INDEX).and_then(|n| self.osc.get(n)) {
                o.reset();
            }
        }
    }

    /// Everything back to the defaults (a new console).
    pub fn reset(&self) {
        for p in self.params.iter() {
            p.reset();
            p.mode.store(source_mode::MONO, Relaxed);
        }
        self.osc.iter().for_each(OscParams::reset);
        self.names.lock().unwrap_or_else(|e| e.into_inner()).iter_mut().for_each(String::clear);
    }
}

const IDENTITY: [f32; 4] = [1.0, 0.0, 0.0, 1.0];
const MID_SIDE: [f32; 4] = [1.0, 1.0, 1.0, -1.0];

/// Audio-thread state of the sources: processed buffers, ramps and the
/// pointers the channels read.
#[derive(Default)]
pub(crate) struct SourceDsp {
    /// One buffer per source the device has (hardware ports, then playback).
    buffers: Vec<Vec<f32>>,
    last_gain: Vec<f32>,
    /// Per pair (indexed by the pair's first source / 2).
    last_matrix: Vec<[f32; 4]>,
    num_hw: usize,
    /// What the channels read: hardware inputs by port, playback channels
    /// (then the USB PLAYER's four and the oscillators, see `source_ptr`).
    hw_out: Vec<*const f32>,
    playback_out: Vec<*const f32>,
    /// The oscillators and the signal they generate this block.
    generators: Vec<SignalGenerator>,
    generated: Vec<Vec<f32>>,
}

impl SourceDsp {
    /// Allocates for a device (message thread, device stopped).
    pub fn prepare(&mut self, num_hw_inputs: usize, block_size: usize, sample_rate: f64) {
        let num_hw = num_hw_inputs.min(NUM_HW_SOURCES);
        let count = num_hw.next_multiple_of(2) + NUM_PLAYBACK_CHANNELS + USB_CHANNELS + NUM_OSCILLATORS;
        self.num_hw = num_hw;
        self.buffers = vec![vec![0.0; block_size]; count];
        self.last_gain = vec![1.0; count];
        self.last_matrix = vec![IDENTITY; count / 2];
        self.hw_out = vec![std::ptr::null(); num_hw_inputs];
        self.playback_out = vec![std::ptr::null(); NUM_PLAYBACK_CHANNELS + USB_CHANNELS + NUM_OSCILLATORS];
        self.generators = (0..NUM_OSCILLATORS as u32).map(|n| SignalGenerator::new(sample_rate, 0x9e37_79b9 ^ (n + 1))).collect();
        self.generated = vec![vec![0.0; block_size]; NUM_OSCILLATORS];
    }

    pub fn hw_ptrs(&self) -> &[*const f32] {
        &self.hw_out
    }

    pub fn playback_ptrs(&self) -> &[*const f32] {
        &self.playback_out
    }

    /// Processes this block's sources from the raw port buffers (`usb`: the
    /// USB deck's player channels; `mute_groups`: the engaged mute groups).
    ///
    /// # Safety
    /// Audio thread. Non-null pointers must be valid for `n` samples.
    pub unsafe fn process(
        &mut self,
        table: &SourceTable,
        hw: &[*const f32],
        playback: &[*const f32],
        usb: &[*const f32; USB_CHANNELS],
        n: usize,
        mute_groups: u32,
    ) {
        if self.hw_out.len() != hw.len() || n > self.buffers.first().map_or(0, Vec::len) {
            // Not prepared for this layout: pass everything through.
            self.hw_out.clear();
            self.playback_out.clear();
            self.hw_out.extend_from_slice(&hw[..hw.len().min(self.hw_out.capacity())]);
            let room = self.playback_out.capacity();
            self.playback_out.extend_from_slice(&playback[..playback.len().min(room)]);
            let room = self.playback_out.capacity() - self.playback_out.len();
            self.playback_out.extend_from_slice(&usb[..USB_CHANNELS.min(room)]);
            let room = self.playback_out.capacity() - self.playback_out.len();
            self.playback_out.extend(std::iter::repeat_n(std::ptr::null(), NUM_OSCILLATORS.min(room)));
            return;
        }
        // Ports beyond the source table pass through.
        for (out, &raw) in self.hw_out.iter_mut().zip(hw).skip(self.num_hw) {
            *out = raw;
        }
        let hw_pairs = self.num_hw.div_ceil(2);
        for pair in 0..hw_pairs {
            let first = pair * 2;
            let raw =
                [hw.get(first).copied().unwrap_or(std::ptr::null()), hw.get(first + 1).copied().unwrap_or(std::ptr::null())];
            let [l, r] = self.process_pair(table, first, first, raw, n, mute_groups);
            self.hw_out[first] = l;
            if first + 1 < self.num_hw {
                self.hw_out[first + 1] = r;
            }
        }
        let base = self.num_hw.next_multiple_of(2);
        for pair in 0..NUM_PLAYBACK_CHANNELS / 2 {
            let ch = pair * 2;
            let raw = [
                playback.get(ch).copied().unwrap_or(std::ptr::null()),
                playback.get(ch + 1).copied().unwrap_or(std::ptr::null()),
            ];
            let [l, r] = self.process_pair(table, NUM_HW_SOURCES + ch, base + ch, raw, n, mute_groups);
            self.playback_out[ch] = l;
            self.playback_out[ch + 1] = r;
        }
        // The USB PLAYER, after the playback channels.
        let base = base + NUM_PLAYBACK_CHANNELS;
        for pair in 0..USB_CHANNELS / 2 {
            let ch = pair * 2;
            let [l, r] =
                self.process_pair(table, USB_PLAYER_SOURCE_INDEX + ch, base + ch, [usb[ch], usb[ch + 1]], n, mute_groups);
            self.playback_out[NUM_PLAYBACK_CHANNELS + ch] = l;
            self.playback_out[NUM_PLAYBACK_CHANNELS + ch + 1] = r;
        }
        // The oscillators, after the USB PLAYER.
        let raw = self.generate(table, n);
        let [l, r] = self.process_pair(table, OSC_SOURCE_INDEX, base + USB_CHANNELS, raw, n, mute_groups);
        self.playback_out[NUM_PLAYBACK_CHANNELS + USB_CHANNELS] = l;
        self.playback_out[NUM_PLAYBACK_CHANNELS + USB_CHANNELS + 1] = r;
    }

    /// Runs the oscillators into `generated` (a pair on OSC 1's settings, its
    /// sine in phase) and returns their buffers.
    fn generate(&mut self, table: &SourceTable, n: usize) -> [*const f32; 2] {
        let ([g1, g2, ..], [b1, b2, ..]) = (&mut self.generators[..], &mut self.generated[..]) else {
            return [std::ptr::null(); 2];
        };
        let paired = table.params[OSC_SOURCE_INDEX].mode.load(Relaxed) != source_mode::MONO;
        let (mode, level, freq) = table.osc[0].get();
        if paired {
            g2.set_phase(g1.phase());
        }
        g1.process(&mut b1[..n], mode, freq, level);
        let (mode, level, freq) = if paired { (mode, level, freq) } else { table.osc[1].get() };
        g2.process(&mut b2[..n], mode, freq, level);
        [b1.as_ptr(), b2.as_ptr()]
    }

    /// One pair: `index` is the table index of its first source, `slot` the
    /// index of its first buffer. Returns the pointers the channels read.
    unsafe fn process_pair(
        &mut self,
        table: &SourceTable,
        index: usize,
        slot: usize,
        raw: [*const f32; 2],
        n: usize,
        mute_groups: u32,
    ) -> [*const f32; 2] {
        let p = &table.params[index..index + 2];
        let mode = p[0].mode.load(Relaxed);
        // A pair shares the first source's settings.
        let gains = if mode == source_mode::MONO {
            [p[0].gain(mute_groups), p[1].gain(mute_groups)]
        } else {
            [p[0].gain(mute_groups); 2]
        };
        let matrix = if mode == source_mode::MID_SIDE { MID_SIDE } else { IDENTITY };
        let last = [self.last_gain[slot], self.last_gain[slot + 1]];
        let last_matrix = self.last_matrix[slot / 2];
        let plain = gains == [1.0, 1.0] && last == [1.0, 1.0] && matrix == IDENTITY && last_matrix == IDENTITY;

        let mut out = raw;
        if !plain && (!raw[0].is_null() || !raw[1].is_null()) {
            let (a, b) = self.buffers.split_at_mut(slot + 1);
            let bufs = [&mut a[slot][..n], &mut b[0][..n]];
            for (ch, buf) in bufs.into_iter().enumerate() {
                if raw[ch].is_null() {
                    buf.fill(0.0);
                } else {
                    buf.copy_from_slice(std::slice::from_raw_parts(raw[ch], n));
                    apply_gain_ramp(buf, last[ch], gains[ch]);
                }
            }
            if matrix != IDENTITY || last_matrix != IDENTITY {
                let (a, b) = self.buffers.split_at_mut(slot + 1);
                crossfade_matrix(&mut a[slot][..n], &mut b[0][..n], last_matrix, matrix);
            }
            // A decoded pair has signal on both sides; otherwise a missing port stays missing.
            let decoded = matrix != IDENTITY || last_matrix != IDENTITY;
            for ch in 0..2 {
                out[ch] = if raw[ch].is_null() && !decoded { std::ptr::null() } else { self.buffers[slot + ch].as_ptr() };
            }
        }
        self.last_gain[slot] = gains[0];
        self.last_gain[slot + 1] = gains[1];
        self.last_matrix[slot / 2] = matrix;
        for ch in 0..2 {
            if !out[ch].is_null() {
                p[ch].peak.fetch_max(peak_level(std::slice::from_raw_parts(out[ch], n)));
            }
        }
        out
    }
}

/// `(l, r) = m * (l, r)` with the matrix crossfading from `last` to `target`.
fn crossfade_matrix(l: &mut [f32], r: &mut [f32], last: [f32; 4], target: [f32; 4]) {
    let step = 1.0 / l.len().max(1) as f32;
    let mut t = 0.0;
    for (x, y) in l.iter_mut().zip(r.iter_mut()) {
        t += step;
        let m: [f32; 4] = std::array::from_fn(|i| last[i] + (target[i] - last[i]) * t);
        let (a, b) = (*x, *y);
        *x = m[0] * a + m[1] * b;
        *y = m[2] * a + m[3] * b;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn indices_and_ports() {
        assert_eq!(source_index(0), Some(0));
        assert_eq!(source_index(NUM_HW_SOURCES as i32), None);
        assert_eq!(source_index(PLAYBACK_SOURCE_BASE + 3), Some(NUM_HW_SOURCES + 3));
        assert_eq!(source_index(-1), None);
        assert_eq!(source_index(USB_PLAYER_SOURCE_BASE + 1), Some(USB_PLAYER_SOURCE_INDEX + 1));
        assert_eq!(source_index(USB_PLAYER_SOURCE_BASE + USB_CHANNELS as i32), None);
        for i in [0, 5, NUM_HW_SOURCES - 1, NUM_HW_SOURCES, USB_PLAYER_SOURCE_INDEX - 1, USB_PLAYER_SOURCE_INDEX, NUM_SOURCES - 1]
        {
            assert_eq!(source_index(source_port(i).unwrap()), Some(i));
        }
        assert_eq!(source_port(NUM_SOURCES), None);
        assert_eq!(pair_first(5), 4);
    }

    #[test]
    fn pairs_share_settings_and_initialize_keeps_the_pairing() {
        let t = SourceTable::default();
        let p = t.params();
        p[2].gain_db.store(12.0);
        p[2].invert.store(true, Relaxed);
        t.set_mode(3, source_mode::STEREO);
        assert_eq!(p[2].mode.load(Relaxed), source_mode::STEREO);
        assert_eq!(p[3].mode.load(Relaxed), source_mode::STEREO);
        assert_eq!(p[3].gain_db.load(), 12.0);
        assert!(p[3].invert.load(Relaxed));
        t.set_name(2, "OH");
        t.initialize(3);
        assert_eq!(p[2].gain_db.load(), 0.0);
        assert!(!p[2].invert.load(Relaxed));
        assert_eq!(t.name(2), "");
        assert_eq!(p[3].mode.load(Relaxed), source_mode::STEREO);
        t.set_mode(2, source_mode::MONO);
        assert!(p[2].is_default() && p[3].is_default());
    }
}
