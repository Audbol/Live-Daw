//! Strip parameters and meters.
//!
//! These blocks are `#[repr(C)]` and shared with the C++ UI, which mirrors them
//! field for field (rust/crates/livedaw-ffi/include/livedaw_engine.h). The UI
//! writes parameters; the audio thread snapshots them once per block. Both sides
//! verify the layout at startup (`ld_layout_info`).

use crate::atomic::AtomicF32;
use livedaw_dsp::{BandType, CompressorSettings, EqBand, EqSettings, GateSettings, NUM_BANDS};
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering::Relaxed};

#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StripKind {
    Input = 0,
    Aux = 1,
    Group = 2,
    Master = 3,
}

impl StripKind {
    pub fn from_i32(v: i32) -> Option<Self> {
        match v {
            0 => Some(Self::Input),
            1 => Some(Self::Aux),
            2 => Some(Self::Group),
            3 => Some(Self::Master),
            _ => None,
        }
    }

    pub fn is_bus(self) -> bool {
        matches!(self, Self::Aux | Self::Group)
    }
}

#[repr(C)]
#[derive(Debug)]
pub struct GateParams {
    pub enabled: AtomicBool,
    pub threshold_db: AtomicF32,
    pub range_db: AtomicF32,
    pub attack_ms: AtomicF32,
    pub hold_ms: AtomicF32,
    pub release_ms: AtomicF32,
}

impl Default for GateParams {
    fn default() -> Self {
        let d = GateSettings::default();
        Self {
            enabled: AtomicBool::new(d.enabled),
            threshold_db: AtomicF32::new(d.threshold_db),
            range_db: AtomicF32::new(d.range_db),
            attack_ms: AtomicF32::new(d.attack_ms),
            hold_ms: AtomicF32::new(d.hold_ms),
            release_ms: AtomicF32::new(d.release_ms),
        }
    }
}

impl GateParams {
    pub fn snapshot(&self) -> GateSettings {
        GateSettings {
            enabled: self.enabled.load(Relaxed),
            threshold_db: self.threshold_db.load(),
            range_db: self.range_db.load(),
            attack_ms: self.attack_ms.load(),
            hold_ms: self.hold_ms.load(),
            release_ms: self.release_ms.load(),
            ..GateSettings::default()
        }
    }
}

#[repr(C)]
#[derive(Debug)]
pub struct CompParams {
    pub enabled: AtomicBool,
    pub threshold_db: AtomicF32,
    pub ratio: AtomicF32,
    pub attack_ms: AtomicF32,
    pub release_ms: AtomicF32,
    pub knee_db: AtomicF32,
    pub makeup_db: AtomicF32,
}

impl Default for CompParams {
    fn default() -> Self {
        let d = CompressorSettings::default();
        Self {
            enabled: AtomicBool::new(d.enabled),
            threshold_db: AtomicF32::new(d.threshold_db),
            ratio: AtomicF32::new(d.ratio),
            attack_ms: AtomicF32::new(d.attack_ms),
            release_ms: AtomicF32::new(d.release_ms),
            knee_db: AtomicF32::new(d.knee_db),
            makeup_db: AtomicF32::new(d.makeup_db),
        }
    }
}

impl CompParams {
    pub fn snapshot(&self) -> CompressorSettings {
        CompressorSettings {
            enabled: self.enabled.load(Relaxed),
            threshold_db: self.threshold_db.load(),
            ratio: self.ratio.load(),
            attack_ms: self.attack_ms.load(),
            release_ms: self.release_ms.load(),
            knee_db: self.knee_db.load(),
            makeup_db: self.makeup_db.load(),
        }
    }
}

#[repr(C)]
#[derive(Debug)]
pub struct EqBandParams {
    pub band_type: AtomicI32,
    pub frequency: AtomicF32,
    pub gain_db: AtomicF32,
    pub q: AtomicF32,
    pub enabled: AtomicBool,
}

#[repr(C)]
#[derive(Debug)]
pub struct EqParams {
    pub enabled: AtomicBool,
    pub bands: [EqBandParams; NUM_BANDS],
}

impl Default for EqParams {
    fn default() -> Self {
        let d = EqSettings::default_bands();
        let band = |b: &EqBand| EqBandParams {
            band_type: AtomicI32::new(b.band_type as i32),
            frequency: AtomicF32::new(b.frequency),
            gain_db: AtomicF32::new(b.gain_db),
            q: AtomicF32::new(b.q),
            enabled: AtomicBool::new(b.enabled),
        };
        Self { enabled: AtomicBool::new(true), bands: [band(&d[0]), band(&d[1]), band(&d[2]), band(&d[3])] }
    }
}

impl EqParams {
    pub fn snapshot(&self) -> EqSettings {
        let band = |b: &EqBandParams| EqBand {
            band_type: BandType::from_i32(b.band_type.load(Relaxed)),
            frequency: b.frequency.load(),
            gain_db: b.gain_db.load(),
            q: b.q.load(),
            enabled: b.enabled.load(Relaxed),
        };
        EqSettings {
            enabled: self.enabled.load(Relaxed),
            bands: [band(&self.bands[0]), band(&self.bands[1]), band(&self.bands[2]), band(&self.bands[3])],
        }
    }
}

/// Everything the UI can change on a strip. Sends live in the strip's `SendBank`.
#[repr(C)]
#[derive(Debug)]
pub struct StripParams {
    // Input stage
    pub trim_db: AtomicF32,
    pub polarity_invert: AtomicBool,
    pub hpf_enabled: AtomicBool,
    pub hpf_frequency: AtomicF32,

    // Processing
    pub gate: GateParams,
    pub eq: EqParams,
    pub comp: CompParams,

    // Output stage
    pub fader_db: AtomicF32,
    pub pan: AtomicF32,
    pub mute: AtomicBool,
    pub solo: AtomicBool,
    pub main_assign: AtomicBool,

    /// Mono or stereo (inputs and buses; the master is always stereo).
    pub stereo: AtomicBool,
    /// Hardware input channels (-1 = none); the right one is used when stereo.
    pub input_left: AtomicI32,
    pub input_right: AtomicI32,
    /// Direct hardware outputs (-1 = none).
    pub output_left: AtomicI32,
    pub output_right: AtomicI32,

    /// Scene recalls skip this strip (snippets still apply).
    pub recall_safe: AtomicBool,
}

impl StripParams {
    pub fn new(kind: StripKind) -> Self {
        let (main_assign, stereo, out) = match kind {
            StripKind::Input => (true, false, (-1, -1)),
            StripKind::Aux => (false, false, (-1, -1)),
            StripKind::Group => (true, true, (-1, -1)),
            StripKind::Master => (false, true, (0, 1)),
        };
        Self {
            trim_db: AtomicF32::new(0.0),
            polarity_invert: AtomicBool::new(false),
            hpf_enabled: AtomicBool::new(false),
            hpf_frequency: AtomicF32::new(80.0),
            gate: GateParams::default(),
            eq: EqParams::default(),
            comp: CompParams::default(),
            fader_db: AtomicF32::new(0.0),
            pan: AtomicF32::new(0.0),
            mute: AtomicBool::new(false),
            solo: AtomicBool::new(false),
            main_assign: AtomicBool::new(main_assign),
            stereo: AtomicBool::new(stereo),
            input_left: AtomicI32::new(-1),
            input_right: AtomicI32::new(-1),
            output_left: AtomicI32::new(out.0),
            output_right: AtomicI32::new(out.1),
            recall_safe: AtomicBool::new(false),
        }
    }
}

/// Meters written by the audio thread. Peaks are "max since last read" (the UI
/// exchanges them with 0); dynamics meters hold the latest block's value.
#[repr(C)]
#[derive(Debug, Default)]
pub struct StripMeters {
    pub input_peak: [AtomicF32; 2],
    pub output_peak: [AtomicF32; 2],
    pub gate_gain: AtomicF32,
    pub comp_reduction_db: AtomicF32,
    pub current_width: AtomicI32,
    /// Blocks that produced NaN/inf and were silenced.
    pub dsp_faults: AtomicI32,
}

/// Engine-wide controls and status, shared with the UI.
#[repr(C)]
#[derive(Debug)]
pub struct EngineControls {
    pub solo_afl: AtomicBool,
    pub any_solo_active: AtomicBool,
    pub output_protection: AtomicBool,
    pub monitor_level_db: AtomicF32,
    pub monitor_output_left: AtomicI32,
    pub monitor_output_right: AtomicI32,
    pub output_ceiling_db: AtomicF32,
    /// Blocks in which the output safety clip engaged.
    pub output_clip_events: AtomicI32,
    pub monitor_peak: [AtomicF32; 2],
    /// Smoothed callback time relative to the block duration.
    pub dsp_load: AtomicF32,
    /// Engine-level faults (a caught panic silences the block).
    pub engine_faults: AtomicI32,
}

impl Default for EngineControls {
    fn default() -> Self {
        Self {
            solo_afl: AtomicBool::new(false),
            any_solo_active: AtomicBool::new(false),
            output_protection: AtomicBool::new(true),
            monitor_level_db: AtomicF32::new(0.0),
            monitor_output_left: AtomicI32::new(-1),
            monitor_output_right: AtomicI32::new(-1),
            output_ceiling_db: AtomicF32::new(0.0),
            output_clip_events: AtomicI32::new(0),
            monitor_peak: Default::default(),
            dsp_load: AtomicF32::new(0.0),
            engine_faults: AtomicI32::new(0),
        }
    }
}
