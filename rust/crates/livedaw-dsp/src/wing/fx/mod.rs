//! The FX rack's built-in devices: the console's premium effects (reverbs,
//! modulation, delays, pitch; FX slots 1-8 only), its standard effects (EQs,
//! dynamics, enhancers, amps...) and its channel strips.
//!
//! Every device is described by its [`ModelSpec`] in [`FX_MODELS`] (generated
//! from the console's parameter tree) and implemented by an [`FxAlgo`]. A device
//! is created on the message thread with [`create`] (it allocates its buffers
//! there, sized for the sample rate and the largest block) and then runs on the
//! audio thread without allocating, locking or panicking.
//!
//! A device processes stereo in place and returns the *effect* signal; the FX
//! slot mixes it with the dry signal (FX MIX). Devices that have their own MIX
//! control (chorus, pitch...) apply it themselves.
//!
//! Like the processing models, the devices recreate each unit's controls and
//! characteristic sound; they are not circuit-level emulations.

pub mod blocks;
pub mod meters;

mod amp;
mod delay;
mod eqfx;
mod modulation;
mod pitch;
mod reverb;
mod utility;

use super::{ModelCategory, ModelSpec, Params, FX_MODELS, MAX_FX_PARAMS};

/// Fixed for an instance's lifetime.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct FxContext {
    pub sample_rate: f32,
    /// No `process` call is longer than this.
    pub max_block: usize,
}

/// A built-in FX device.
pub trait FxAlgo: Send {
    /// New parameter values. Called on the audio thread whenever they change and
    /// once before the first block; must not allocate.
    fn set_params(&mut self, p: &Params);

    /// Processes one block in place. `left` and `right` have the same length,
    /// at most `FxContext::max_block`.
    fn process(&mut self, left: &mut [f32], right: &mut [f32]);

    /// Clears all state (delay lines, envelopes...) as if newly created.
    fn reset(&mut self);

    /// A level for the screen: gain reduction in dB (<= 0) for dynamics
    /// devices, 0 otherwise.
    fn meter(&self) -> f32 {
        0.0
    }

    /// A band's live gain in dB, signed (cut < 0, boost > 0), for devices
    /// with dynamic bands (TRIPLE DEQ); 0 otherwise.
    fn band_gain(&self, _band: usize) -> f32 {
        0.0
    }

    /// The live values the device's screen shows (meters, gain-reduction
    /// bars, band levels, a scope...): the device writes the ones it has into
    /// `out` (each device documents its layout; the rest stay 0). Called on
    /// the audio thread after each block; must not allocate.
    ///
    /// Conventions: 0 is every readout's idle value. Levels are linear
    /// (peaks that rise at once and fall at [`meters::FALL_DB_PER_S`], or VU
    /// levels relative to 0 VU, see [`meters`]); gain reductions are dB
    /// (<= 0) following the processor's own envelope.
    fn readouts(&self, _out: &mut [f32; FX_READOUTS]) {}

    /// The delay the device adds to the signal, in samples (a limiter's
    /// look-ahead), fixed for the instance's lifetime. The slot delays FX
    /// MIX's dry signal by it and reports it to the delay compensation.
    fn latency(&self) -> u32 {
        0
    }
}

/// Bands [`FxAlgo::band_gain`] can report (TRIPLE DEQ has the most).
pub const FX_BANDS: usize = 3;

/// Readouts [`FxAlgo::readouts`] can report.
pub const FX_READOUTS: usize = 48;

/// Index of a device in [`FX_MODELS`].
pub fn model_index(token: &str) -> Option<usize> {
    super::model_index(FX_MODELS, token)
}

/// Whether a device may run in FX slot `slot` (0-based): premium devices need
/// slots 1-8, everything else runs anywhere.
pub fn allowed_in_slot(model: usize, slot: usize) -> bool {
    match FX_MODELS.get(model) {
        Some(m) => m.category != ModelCategory::FxPremium || slot < 8,
        None => false,
    }
}

/// Creates a device (message thread: allocates). `None` for NONE, EXT and
/// unknown tokens.
pub fn create(token: &str, ctx: FxContext) -> Option<Box<dyn FxAlgo>> {
    let ctx = FxContext { sample_rate: ctx.sample_rate.clamp(8000.0, 384_000.0), max_block: ctx.max_block.max(1) };
    reverb::create(token, ctx)
        .or_else(|| modulation::create(token, ctx))
        .or_else(|| delay::create(token, ctx))
        .or_else(|| pitch::create(token, ctx))
        .or_else(|| utility::create(token, ctx))
        .or_else(|| eqfx::create(token, ctx))
        .or_else(|| amp::create(token, ctx))
}

/// The devices whose screen graph is their measured frequency response.
const MEASURED: &[&str] = &["GEQ", "PIA", "SPKMAN"];

/// The frequency response in dB at `freqs` of a device as set, for its
/// screen's graph: the EQ section of the EQ devices and channel strips, and
/// the measured response of the graphic EQs and the speaker manager (a small
/// impulse through the device once its controls have settled). False for
/// devices without one. Message thread (allocates).
pub fn response(token: &str, values: &[f32], freqs: &[f32], out: &mut [f32]) -> bool {
    if eqfx::eq_response(token, values, freqs, out) {
        return true;
    }
    if !MEASURED.contains(&token) {
        return false;
    }
    const SR: f32 = 48_000.0;
    const LEN: usize = 16_384;
    const AMP: f32 = 0.01;
    let Some(mut dev) = model_index(token).and_then(|m| FxDevice::new(m, FxContext { sample_rate: SR, max_block: 256 })) else {
        return false;
    };
    let spec = dev.spec();
    let n = spec.params.len().min(MAX_FX_PARAMS);
    let mut v = [0.0; MAX_FX_PARAMS];
    spec.defaults_into(&mut v);
    for (dst, src) in v.iter_mut().zip(values).take(n) {
        *dst = *src;
    }
    // Let the parameter glides settle, then clear the state and measure.
    let (mut l, mut r) = (vec![0.0f32; LEN], vec![0.0f32; LEN]);
    dev.process(&v[..n], &mut l, &mut r);
    dev.reset();
    l.fill(0.0);
    r.fill(0.0);
    l[0] = AMP;
    r[0] = AMP;
    dev.process(&v[..n], &mut l, &mut r);
    for (f, o) in freqs.iter().zip(out.iter_mut()) {
        let w = std::f64::consts::TAU * f64::from(*f) / f64::from(SR);
        let (step_re, step_im) = (w.cos(), -w.sin());
        let (mut ph_re, mut ph_im, mut re, mut im) = (1.0f64, 0.0f64, 0.0f64, 0.0f64);
        for x in &l {
            re += f64::from(*x) * ph_re;
            im += f64::from(*x) * ph_im;
            (ph_re, ph_im) = (ph_re * step_re - ph_im * step_im, ph_re * step_im + ph_im * step_re);
        }
        *o = (20.0 * ((re * re + im * im).sqrt() / f64::from(AMP)).max(1e-9).log10()) as f32;
    }
    true
}

/// A running device: the instance plus the parameter values it last saw.
/// The FX slot owns one of these and feeds it the slot's parameter atomics.
pub struct FxDevice {
    model: usize,
    ctx: FxContext,
    algo: Box<dyn FxAlgo>,
    values: [f32; MAX_FX_PARAMS],
    primed: bool,
}

impl FxDevice {
    /// Message thread (allocates). `None` if the model isn't a built-in device.
    pub fn new(model: usize, ctx: FxContext) -> Option<Self> {
        let spec = FX_MODELS.get(model)?;
        let algo = create(spec.token, ctx)?;
        let mut values = [0.0; MAX_FX_PARAMS];
        spec.defaults_into(&mut values);
        Some(Self { model, ctx, algo, values, primed: false })
    }

    pub fn model(&self) -> usize {
        self.model
    }

    pub fn spec(&self) -> &'static ModelSpec {
        &FX_MODELS[self.model]
    }

    pub fn context(&self) -> FxContext {
        self.ctx
    }

    pub fn meter(&self) -> f32 {
        self.algo.meter()
    }

    /// See [`FxAlgo::band_gain`].
    pub fn band_gain(&self, band: usize) -> f32 {
        self.algo.band_gain(band)
    }

    /// See [`FxAlgo::readouts`].
    pub fn readouts(&self, out: &mut [f32; FX_READOUTS]) {
        self.algo.readouts(out);
    }

    /// See [`FxAlgo::latency`].
    pub fn latency(&self) -> u32 {
        self.algo.latency()
    }

    pub fn reset(&mut self) {
        self.algo.reset();
    }

    /// Audio thread: applies `values` if they changed, then processes in blocks
    /// of at most `max_block`.
    pub fn process(&mut self, values: &[f32], left: &mut [f32], right: &mut [f32]) {
        let spec = &FX_MODELS[self.model];
        let n = spec.params.len().min(MAX_FX_PARAMS).min(values.len());
        if !self.primed || self.values[..n] != values[..n] {
            for (i, p) in spec.params.iter().enumerate().take(n) {
                self.values[i] = p.sanitise(values[i]);
            }
            self.algo.set_params(&Params::new(spec, &self.values));
            self.primed = true;
        }
        let len = left.len().min(right.len());
        let mut start = 0;
        while start < len {
            let end = (start + self.ctx.max_block).min(len);
            self.algo.process(&mut left[start..end], &mut right[start..end]);
            start = end;
        }
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    fn response_at(token: &str, set: &[(&str, f32)], freqs: &[f32]) -> Option<Vec<f32>> {
        let spec = &FX_MODELS[model_index(token)?];
        let mut v = [0.0; MAX_FX_PARAMS];
        spec.defaults_into(&mut v);
        for (id, x) in set {
            v[spec.param_index(id).unwrap()] = *x;
        }
        let mut out = vec![0.0; freqs.len()];
        response(token, &v[..spec.params.len()], freqs, &mut out).then_some(out)
    }

    #[test]
    fn graphs_show_the_devices_response() {
        // A flat graphic EQ, then one band up.
        let flat = response_at("GEQ", &[], &[100.0, 1000.0, 10_000.0]).unwrap();
        assert!(flat.iter().all(|db| db.abs() < 0.5), "{flat:?}");
        let boost = response_at("GEQ", &[("1k", 12.0)], &[100.0, 1000.0]).unwrap();
        assert!(boost[1] > 8.0 && boost[0].abs() < 1.5, "{boost:?}");
        // The EQ section of a channel strip; off is flat.
        let strip = response_at("*SOUL*", &[("eq_on", 1.0), ("lg", 5.0)], &[60.0, 5000.0]).unwrap();
        assert!(strip[0] > 2.0 && strip[1].abs() < 1.5, "{strip:?}");
        let off = response_at("*SOUL*", &[("eq_on", 0.0), ("lg", 5.0)], &[60.0]).unwrap();
        assert!(off[0].abs() < 0.01, "{off:?}");
        // Devices without one.
        assert!(response_at("HALL", &[], &[1000.0]).is_none());
    }

    struct Rng(u32);

    impl Rng {
        fn sample(&mut self) -> f32 {
            self.0 ^= self.0 << 13;
            self.0 ^= self.0 >> 17;
            self.0 ^= self.0 << 5;
            (self.0 as f32 / u32::MAX as f32) * 2.0 - 1.0
        }
    }

    fn run(dev: &mut FxDevice, values: &[f32], input: &[f32], rng: &mut Rng) -> (Vec<f32>, Vec<f32>) {
        let mut l = input.to_vec();
        let mut r: Vec<f32> = input.iter().map(|x| x * 0.8).collect();
        // Uneven block sizes, as the engine's chunking may produce.
        let mut start = 0;
        while start < l.len() {
            let n = (1 + ((rng.sample() * 0.5 + 0.5) * 700.0) as usize).min(l.len() - start);
            dev.process(values, &mut l[start..start + n], &mut r[start..start + n]);
            start += n;
        }
        (l, r)
    }

    fn assert_sane(token: &str, what: &str, (l, r): &(Vec<f32>, Vec<f32>)) {
        let bad = l.iter().chain(r).position(|x| !x.is_finite() || x.abs() > 64.0);
        assert!(bad.is_none(), "{token}: {what}: non-finite or runaway output");
    }

    /// The screen's readouts are always finite (and the meter's a reduction).
    fn assert_readouts(token: &str, what: &str, dev: &FxDevice) {
        let mut r = [0.0; FX_READOUTS];
        dev.readouts(&mut r);
        assert!(r.iter().all(|x| x.is_finite() && x.abs() < 1e6), "{token}: {what}: readouts {r:?}");
        assert!(dev.meter() <= 0.0 && dev.meter().is_finite(), "{token}: {what}: meter {}", dev.meter());
    }

    /// The checks every device must pass (used by each family's tests).
    pub(crate) fn check_device(token: &str) {
        let model = model_index(token).unwrap_or_else(|| panic!("{token}: not in FX_MODELS"));
        let spec = &FX_MODELS[model];
        for sr in [44_100.0, 96_000.0] {
            let ctx = FxContext { sample_rate: sr, max_block: 512 };
            let mut dev = FxDevice::new(model, ctx).unwrap_or_else(|| panic!("{token}: not implemented"));
            let mut rng = Rng(0x1234_5678 ^ model as u32);
            let mut defaults = [0.0; MAX_FX_PARAMS];
            spec.defaults_into(&mut defaults);
            let secs = |s: f32| (s * sr) as usize;

            // Silence in, silence out: no noise, DC or self-oscillation at defaults.
            let silent = run(&mut dev, &defaults, &vec![0.0; secs(0.5)], &mut rng);
            let peak = silent.0.iter().chain(&silent.1).fold(0.0f32, |a, x| a.max(x.abs()));
            assert!(peak < 1e-4, "{token}: produces {peak} from silence");

            // Noise at defaults: sane and audible.
            let noise: Vec<f32> = (0..secs(1.0)).map(|_| rng.sample() * 0.5).collect();
            let out = run(&mut dev, &defaults, &noise, &mut rng);
            assert_sane(token, "defaults", &out);
            let energy: f32 = out.0.iter().chain(&out.1).map(|x| x * x).sum();
            assert!(energy > 1e-3, "{token}: no output for noise at its defaults");

            // ... and it dies away again (no endless tail at defaults).
            let tail = run(&mut dev, &defaults, &vec![0.0; secs(12.0)], &mut rng);
            let n = tail.0.len();
            let late = tail.0[n - secs(0.5)..].iter().chain(&tail.1[n - secs(0.5)..]).fold(0.0f32, |a, x| a.max(x.abs()));
            assert!(late < 1e-3, "{token}: still {late} after 12 s of silence");

            // Random settings, then the extremes: never NaN, inf or runaway.
            for _ in 0..24 {
                let mut values = defaults;
                for (v, p) in values.iter_mut().zip(spec.params) {
                    *v = p.from_normalised(rng.sample() * 0.5 + 0.5);
                }
                let input: Vec<f32> = (0..secs(0.15)).map(|_| rng.sample() * 0.9).collect();
                assert_sane(token, "random settings", &run(&mut dev, &values, &input, &mut rng));
                assert_readouts(token, "random settings", &dev);
            }
            for pos in [0.0, 1.0] {
                let mut values = defaults;
                for (v, p) in values.iter_mut().zip(spec.params) {
                    *v = p.from_normalised(pos);
                }
                let input: Vec<f32> = (0..secs(0.5)).map(|i| if i % 4410 == 0 { 1.0 } else { rng.sample() * 0.9 }).collect();
                assert_sane(
                    token,
                    if pos == 0.0 { "minimum settings" } else { "maximum settings" },
                    &run(&mut dev, &values, &input, &mut rng),
                );
            }

            dev.reset();
            let after = run(&mut dev, &defaults, &vec![0.0; secs(0.1)], &mut rng);
            let peak = after.0.iter().chain(&after.1).fold(0.0f32, |a, x| a.max(x.abs()));
            assert!(peak < 1e-4, "{token}: reset left {peak} behind");
        }

        // Cost: a second of 48 kHz stereo well under a core (release builds only).
        if !cfg!(debug_assertions) {
            let ctx = FxContext { sample_rate: 48_000.0, max_block: 256 };
            let mut dev = FxDevice::new(model, ctx).unwrap();
            let mut defaults = [0.0; MAX_FX_PARAMS];
            spec.defaults_into(&mut defaults);
            let mut l = vec![0.1f32; 256];
            let mut r = vec![0.1f32; 256];
            let t = std::time::Instant::now();
            for _ in 0..(48_000 / 256) {
                dev.process(&defaults, &mut l, &mut r);
            }
            let ms = t.elapsed().as_secs_f64() * 1000.0;
            assert!(ms < 150.0, "{token}: {ms:.1} ms per second of audio");
        }
    }

    #[test]
    fn slots_and_lookup() {
        let hall = model_index("HALL").unwrap();
        let geq = model_index("GEQ").unwrap();
        assert!(allowed_in_slot(hall, 0) && allowed_in_slot(hall, 7) && !allowed_in_slot(hall, 8));
        assert!(allowed_in_slot(geq, 15));
        assert!(create("NONE", FxContext { sample_rate: 48_000.0, max_block: 64 }).is_none());
        assert!(create("EXT", FxContext { sample_rate: 48_000.0, max_block: 64 }).is_none());
    }

    #[test]
    fn every_device_is_implemented() {
        // The families' own tests run the full checks; here, every table entry has a device.
        let ctx = FxContext { sample_rate: 48_000.0, max_block: 64 };
        for m in FX_MODELS.iter().filter(|m| !matches!(m.category, ModelCategory::FxNone | ModelCategory::FxExternal)) {
            assert!(create(m.token, ctx).is_some(), "{}: not implemented", m.token);
        }
    }
}
