//! Reverbs: the console's 480L-style halls, rooms, chambers, plates, concert and
//! ambience, the vintage room / reverb / plate, gated, reverse, delay+reverb,
//! shimmer, spring, VSS3 and blue plate.
//!
//! Every device outputs the 100 % wet reverb. Most are built on one voice
//! ([`space::Space`]: damping filter, allpass input diffusion, pre-delay with
//! one build-up tap per tank line, a 16-line feedback delay network with
//! in-loop allpasses, random line modulation and per-band decay, then output
//! filters and width); the gated / reverse and spring reverbs have their own
//! structures. Control changes glide (delay times move a little every sample,
//! or crossfade for the delay / echo times), so nothing zips or clicks. See
//! each submodule for the devices' designs and how their controls map; the
//! mapping follows the WING manual (chapter 11.4).

use super::{FxAlgo, FxContext};

mod lexicon;
mod nonlin;
mod parts;
mod space;
mod special;
mod spring;
mod vintage;
mod vss3;

/// The devices this module implements.
pub(super) const TOKENS: &[&str] = &[
    "HALL", "ROOM", "CHAMBER", "PLATE", "CONCERT", "AMBI", "V-ROOM", "V-REV", "V-PLATE", "GATED", "REVERSE", "DEL/REV",
    "SHIMMER", "SPRING", "VSS3", "BPLATE",
];

pub(super) fn create(token: &str, ctx: FxContext) -> Option<Box<dyn FxAlgo>> {
    use lexicon::{Kind, Lexicon};
    if !TOKENS.contains(&token) {
        return None;
    }
    let lex = |k| Box::new(Lexicon::new(k, ctx)) as Box<dyn FxAlgo>;
    Some(match token {
        "HALL" => lex(Kind::Hall),
        "ROOM" => lex(Kind::Room),
        "CHAMBER" => lex(Kind::Chamber),
        "PLATE" => lex(Kind::Plate),
        "CONCERT" => lex(Kind::Concert),
        "AMBI" => lex(Kind::Ambience),
        "BPLATE" => lex(Kind::BluePlate),
        "V-ROOM" => Box::new(vintage::VintageRoom::new(ctx)),
        "V-REV" => Box::new(vintage::VintageReverb::new(ctx)),
        "V-PLATE" => Box::new(vintage::VintagePlate::new(ctx)),
        "GATED" => Box::new(nonlin::NonLin::new(false, ctx)),
        "REVERSE" => Box::new(nonlin::NonLin::new(true, ctx)),
        "DEL/REV" => Box::new(special::DelayReverb::new(ctx)),
        "SHIMMER" => Box::new(special::Shimmer::new(ctx)),
        "SPRING" => Box::new(spring::SpringReverb::new(ctx)),
        _ => Box::new(vss3::Vss3::new(ctx)),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wing::{Params, FX_MODELS, MAX_FX_PARAMS};

    #[test]
    fn devices() {
        for token in super::TOKENS {
            crate::wing::fx::tests::check_device(token);
        }
    }

    /// A device at 48 kHz with its defaults, `edit` applied to the values.
    fn device(token: &str, edit: &[(&str, f32)]) -> Box<dyn FxAlgo> {
        let spec = &FX_MODELS[crate::wing::fx::model_index(token).unwrap()];
        let mut values = [0.0; MAX_FX_PARAMS];
        spec.defaults_into(&mut values);
        for (id, v) in edit {
            values[spec.param_index(id).unwrap()] = *v;
        }
        let mut dev = create(token, FxContext { sample_rate: 48_000.0, max_block: 512 }).unwrap();
        dev.set_params(&Params::new(spec, &values));
        dev
    }

    /// Mono-summed output for `input` (left = right = input).
    fn render(dev: &mut dyn FxAlgo, input: &[f32]) -> Vec<f32> {
        let mut out = Vec::with_capacity(input.len());
        for chunk in input.chunks(256) {
            let mut l = chunk.to_vec();
            let mut r = chunk.to_vec();
            dev.process(&mut l, &mut r);
            out.extend(l.iter().zip(&r).map(|(a, b)| 0.5 * (a + b)));
        }
        out
    }

    fn impulse(n: usize) -> Vec<f32> {
        let mut v = vec![0.0; n];
        v[0] = 1.0;
        v
    }

    /// RT60 estimated from the energy decay curve (Schroeder integration),
    /// fitted between -5 and -35 dB.
    fn rt60(x: &[f32], sr: f32) -> f32 {
        let mut edc = vec![0.0f64; x.len()];
        let mut acc = 0.0f64;
        for i in (0..x.len()).rev() {
            acc += (x[i] as f64).powi(2);
            edc[i] = acc;
        }
        let db = |i: usize| 10.0 * (edc[i] / edc[0]).log10();
        let t5 = (0..x.len()).find(|&i| db(i) < -5.0).unwrap();
        let t35 = (0..x.len()).find(|&i| db(i) < -35.0).unwrap();
        2.0 * (t35 - t5) as f32 / sr
    }

    /// Applies new parameter values to a running device.
    fn retune(dev: &mut dyn FxAlgo, token: &str, edit: &[(&str, f32)]) {
        let spec = &FX_MODELS[crate::wing::fx::model_index(token).unwrap()];
        let mut values = [0.0; MAX_FX_PARAMS];
        spec.defaults_into(&mut values);
        for (id, v) in edit {
            values[spec.param_index(id).unwrap()] = *v;
        }
        dev.set_params(&Params::new(spec, &values));
    }

    /// Level of `hz` in `x` (Goertzel), linear.
    fn tone(x: &[f32], hz: f32) -> f32 {
        let w = std::f32::consts::TAU * hz / 48_000.0;
        let (mut s1, mut s2) = (0.0f32, 0.0f32);
        for &v in x {
            let s = v + 2.0 * w.cos() * s1 - s2;
            s2 = s1;
            s1 = s;
        }
        (s1 * s1 + s2 * s2 - 2.0 * w.cos() * s1 * s2).sqrt() / x.len() as f32
    }

    fn sine(hz: f32, n: usize) -> Vec<f32> {
        (0..n).map(|i| 0.5 * (std::f32::consts::TAU * hz * i as f32 / 48_000.0).sin()).collect()
    }

    fn energy(x: &[f32]) -> f32 {
        x.iter().map(|v| v * v).sum()
    }

    #[test]
    fn hall_rt60_follows_decay() {
        for dcy in [1.0, 2.0, 4.0] {
            let mut dev = device("HALL", &[("dcy", dcy), ("damp", 20000.0), ("hc", 20000.0)]);
            let out = render(dev.as_mut(), &impulse(48_000 * 12));
            let t = rt60(&out, 48_000.0);
            assert!((t / dcy - 1.0).abs() < 0.2, "DECAY {dcy} s measured {t} s");
        }
    }

    #[test]
    fn decays_follow_their_controls() {
        // The bass multiplier lengthens the lows only; SIZE scales the 480L's
        // DECAY; the vintage room's HI MULT shortens the highs.
        let band_rt = |token: &str, edit: &[(&str, f32)], lo: f32, hi: f32| {
            let mut dev = device(token, edit);
            let out = render(dev.as_mut(), &impulse(48_000 * 10));
            let mut bp = crate::biquad::Biquad::default();
            bp.set_coefficients(crate::biquad::BiquadCoeffs::band_pass(48_000.0, (lo * hi).sqrt() as f64, 1.0));
            let f: Vec<f32> = out.iter().map(|&x| bp.tick(x, 0)).collect();
            rt60(&f, 48_000.0)
        };
        // (The pre-echo loop, FEED, off: its repeats don't follow BASS MULT.)
        let flat = band_rt("ROOM", &[("dcy", 2.0), ("efl", 0.0), ("efr", 0.0)], 150.0, 300.0);
        let bass = band_rt("ROOM", &[("dcy", 2.0), ("mult", 2.0), ("efl", 0.0), ("efr", 0.0)], 150.0, 300.0);
        assert!(bass > 1.5 * flat, "BASS MULT 2: {flat} -> {bass} s");
        let small = band_rt("HALL", &[("size", 30.0)], 700.0, 1400.0);
        let big = band_rt("HALL", &[("size", 100.0)], 700.0, 1400.0);
        assert!(big > 1.3 * small, "SIZE 30 -> 100: {small} -> {big} s");
        let vroom = [("dcy", 2.0), ("hmult", 0.3), ("hc", 12_000.0), ("dens", 20.0)];
        let mid = band_rt("V-ROOM", &vroom, 700.0, 1400.0);
        let top = band_rt("V-ROOM", &vroom, 6000.0, 12000.0);
        assert!(top < 0.6 * mid, "HI MULT 0.3: mids {mid} s, highs {top} s");
    }

    #[test]
    fn reflection_times_apply_from_the_start() {
        // A freshly loaded device places its early reflections (V-ROOM ER LEFT /
        // RIGHT) and reflection clusters (CONCERT REFL L / R) at their times
        // right away, not only after the time is changed.
        for (token, ids, short, long) in [("V-ROOM", ["erl", "err"], 5.0, 150.0), ("CONCERT", ["rfl", "rfr"], 30.0, 600.0)] {
            let at = |t: f32| {
                let mut dev = device(token, &[(ids[0], t), (ids[1], t), ("pdel", 0.0)]);
                render(dev.as_mut(), &impulse(48_000))
            };
            let (a, b) = (at(short), at(long));
            let diff: f32 = a.iter().zip(&b).map(|(x, y)| (x - y) * (x - y)).sum();
            assert!(diff > 1e-3 * energy(&a), "{token}: the reflection time changes nothing on a new device");
        }
    }

    #[test]
    fn reverse_swells_and_gated_cuts() {
        let ms = |t: f32| (t * 48.0) as usize;
        // REVERSE, 600 ms window: quiet start, loud end, silence after.
        let mut dev = device("REVERSE", &[("dcy", 0.6), ("pdel", 0.0)]);
        let out = render(dev.as_mut(), &impulse(ms(1500.0)));
        let first = energy(&out[ms(20.0)..ms(200.0)]);
        let last = energy(&out[ms(400.0)..ms(590.0)]);
        let after = energy(&out[ms(700.0)..]);
        assert!(last > 4.0 * first, "reverse: first {first}, last {last}");
        assert!(after < 1e-4 * last, "reverse tail after the window: {after}");
        // GATED, 400 ms window: full until the gate, then gone.
        let mut dev = device("GATED", &[("dcy", 0.4), ("pdel", 0.0), ("sprd", 100.0)]);
        let out = render(dev.as_mut(), &impulse(ms(1500.0)));
        let open = energy(&out[ms(200.0)..ms(390.0)]);
        let closed = energy(&out[ms(480.0)..]);
        assert!(open > 0.3 * energy(&out[..ms(190.0)]), "gate sags before its end");
        assert!(closed < 1e-4 * open, "gated tail: open {open}, closed {closed}");
    }

    #[test]
    fn freeze_holds_the_tail() {
        let mut rng = 0x2468_ace1u32;
        let noise: Vec<f32> = (0..48_000)
            .map(|_| {
                rng ^= rng << 13;
                rng ^= rng >> 17;
                rng ^= rng << 5;
                (rng as f32 / u32::MAX as f32 - 0.5) * 0.5
            })
            .collect();
        let mut dev = device("V-ROOM", &[("dcy", 1.0)]);
        render(dev.as_mut(), &noise);
        retune(dev.as_mut(), "V-ROOM", &[("dcy", 1.0), ("frz", 1.0)]);
        // Frozen: the input is ignored and the tail holds its level.
        let held = render(dev.as_mut(), &noise[..48_000 / 2]);
        let later = render(dev.as_mut(), &vec![0.0; 48_000 * 4]);
        let a = energy(&held[12_000..]) / 12_000.0;
        let b = energy(&later[48_000 * 3..]) / 48_000.0;
        assert!(a > 1e-4 && (b / a) > 0.5 && (b / a) < 2.0, "frozen level {a} -> {b}");
        // Released: it decays again.
        retune(dev.as_mut(), "V-ROOM", &[("dcy", 1.0)]);
        let gone = render(dev.as_mut(), &vec![0.0; 48_000 * 4]);
        assert!(energy(&gone[48_000 * 3..]) / 48_000.0 < 1e-4 * b);
    }

    #[test]
    fn size_and_time_changes_are_smooth() {
        // A steady 220 Hz tone through the reverb; jumping SIZE (or the delay
        // TIME) glides the delays, which bends the pitch but must not click:
        // nothing appears above 5 kHz.
        let hf_peak = |x: &[f32]| {
            let mut hp = crate::biquad::Biquad::default();
            hp.set_coefficients(crate::biquad::BiquadCoeffs::high_pass(48_000.0, 5000.0, 0.707));
            x.iter().map(|&v| hp.tick(v, 0).abs()).skip(64).fold(0.0f32, f32::max)
        };
        for (token, id, from, to) in [
            ("HALL", "size", 20.0, 100.0),
            ("ROOM", "size", 70.0, 4.0),
            ("HALL", "pdel", 0.0, 200.0),
            ("DEL/REV", "time", 100.0, 900.0),
            ("GATED", "dcy", 0.2, 1.0),
            ("V-ROOM", "erl", 0.0, 200.0),
            ("VSS3", "ertype", 0.0, 12.0),
            ("CONCERT", "rfl", 30.0, 1000.0),
            ("SHIMMER", "size", 50.0, 1.0),
        ] {
            let mut dev = device(token, &[(id, from)]);
            let input = sine(220.0, 48_000);
            let before = render(dev.as_mut(), &input);
            retune(dev.as_mut(), token, &[(id, to)]);
            let after = render(dev.as_mut(), &input);
            let level = before[24_000..].iter().fold(0.0f32, |a, v| a.max(v.abs()));
            let click = hf_peak(&after);
            assert!(click < 0.01 * level, "{token} {id} {from} -> {to}: {click} above 5 kHz (level {level})");
        }
    }

    #[test]
    fn shimmer_adds_the_octave() {
        let octave = |shim: f32| {
            let mut dev = device("SHIMMER", &[("shim", shim), ("dcy", 4.0)]);
            let out = render(dev.as_mut(), &sine(330.0, 48_000 * 2));
            tone(&out[48_000..], 660.0) / tone(&out[48_000..], 330.0)
        };
        let (dry, wet) = (octave(0.0), octave(80.0));
        assert!(wet > 8.0 * dry, "octave / fundamental: {dry} -> {wet}");
    }

    #[test]
    fn spring_chirps() {
        // Dispersion: in the first arrival the highs come after the lows.
        let mut dev = device("SPRING", &[]);
        let out = render(dev.as_mut(), &impulse(48_000 / 20));
        let centroid = |hz: f32| {
            let mut bp = crate::biquad::Biquad::default();
            bp.set_coefficients(crate::biquad::BiquadCoeffs::band_pass(48_000.0, hz as f64, 2.0));
            let e: Vec<f32> = out.iter().map(|&x| bp.tick(x, 0).powi(2)).collect();
            let total: f32 = e.iter().sum();
            e.iter().enumerate().map(|(i, v)| i as f32 * v).sum::<f32>() / total.max(1e-20)
        };
        let (lo, hi) = (centroid(400.0), centroid(3000.0));
        assert!(hi > lo + 48.0 * 3.0, "low band at {lo}, high band at {hi} samples");
    }

    #[test]
    #[ignore = "prints levels and costs"]
    fn levels() {
        let mut rng = 0x1234_5678u32;
        let noise: Vec<f32> = (0..48_000 * 2)
            .map(|_| {
                rng ^= rng << 13;
                rng ^= rng >> 17;
                rng ^= rng << 5;
                (rng as f32 / u32::MAX as f32 - 0.5) * 0.5
            })
            .collect();
        let rms_in = (noise.iter().map(|v| v * v).sum::<f32>() / noise.len() as f32).sqrt();
        for token in TOKENS {
            let mut dev = device(token, &[]);
            let out = render(dev.as_mut(), &noise);
            let tail = &out[48_000..];
            let rms = (tail.iter().map(|v| v * v).sum::<f32>() / tail.len() as f32).sqrt();
            // Cost: the best of a few runs of a second of stereo in 256-sample blocks.
            let (mut l, mut r) = (vec![0.0f32; 256], vec![0.0f32; 256]);
            let mut best = f64::MAX;
            for _ in 0..5 {
                let t = std::time::Instant::now();
                for block in noise[..48_000].chunks(256) {
                    l[..block.len()].copy_from_slice(block);
                    r[..block.len()].copy_from_slice(block);
                    dev.process(&mut l[..block.len()], &mut r[..block.len()]);
                }
                best = best.min(t.elapsed().as_secs_f64() * 1000.0);
            }
            println!("{token:8} wet/dry {:6.2} dB   {best:5.2} ms per second", 20.0 * (rms / rms_in).log10());
        }
    }
}
