//! Converts show files from before the WING layout (show version 2, version 1
//! sessions) to the current format, so old shows keep loading.
//!
//! * input channels become channels, aux and group buses become buses (groups
//!   keep feeding Main 1), the master becomes Main 1;
//! * the gate becomes the GATE model, the compressor the COMP model and the
//!   4-band EQ the WING EQ (shelves on the L/H bands, bells on the mid bands,
//!   cuts on the channel filter);
//! * each strip's plugin inserts move to FX slots: the first to its pre insert,
//!   the second to its post insert;
//! * ARGB colours become the nearest WING colour.

use crate::refs::StripRef;
use crate::xml::{fmt_f32, Element};
use livedaw_dsp::wing::{model_index, ParamKind, DYN_MODELS, EQ_MODELS_BUS, EQ_MODELS_CH};
use livedaw_engine::params::{nearest_colour, NUM_FX};
use livedaw_engine::StripKind;
use std::collections::HashMap;

/// Consoles and states without a version (or below 3) are legacy.
pub fn is_legacy(e: &Element) -> bool {
    e.int_or("version", 0) < crate::console::CONSOLE_VERSION
}

/// "input:3" -> "ch:3", "aux:2"/"group:2" -> "bus:2", "master:0" -> "main:0".
pub fn convert_ref(old: &str) -> StripRef {
    let (kind, id) = old.split_once(':').unwrap_or((old, ""));
    let id = crate::xml::parse_int_prefix(id);
    match kind {
        "aux" | "group" => StripRef::new(StripKind::Bus, id),
        "master" => StripRef::new(StripKind::Main, 0),
        _ => StripRef::new(StripKind::Channel, id),
    }
}

/// A comma separated list of old references.
pub fn convert_refs(old: &str) -> String {
    let refs: Vec<StripRef> = old.split(',').map(str::trim).filter(|p| !p.is_empty()).map(convert_ref).collect();
    crate::refs::refs_to_string(&refs)
}

/// Old `<Layers>`: only the user layers survive (their strips converted).
pub fn convert_layers(old: &Element) -> Element {
    let mut layers = Element::new("Layers");
    for l in old.children_named("Layer") {
        layers
            .add_new("Layer")
            .set("id", l.str_or("id", ""))
            .set("name", l.str_or("name", "User"))
            .set("faders", convert_refs(l.str_or("strips", "")));
    }
    layers
}

fn copy_attr(from: &Element, name: &str, to: &mut Element, as_name: &str) {
    if let Some(v) = from.attr(name) {
        to.set(as_name, v);
    }
}

fn param(e: &mut Element, id: &str, v: f64) {
    e.add_new("P").set("n", id).set("v", fmt_f32(v as f32));
}

fn choice_index(table: &[livedaw_dsp::wing::ModelSpec], model: &str, id: &str, choice: &str) -> Option<usize> {
    let spec = &table[model_index(table, model)?];
    match spec.params[spec.param_index(id)?].kind {
        ParamKind::Choice(items) => items.iter().position(|c| *c == choice),
        _ => None,
    }
}

/// Index of the value closest to `x` in a values parameter.
fn nearest_value(table: &[livedaw_dsp::wing::ModelSpec], model: &str, id: &str, x: f64) -> f64 {
    let Some(spec) = model_index(table, model).map(|m| &table[m]) else { return 0.0 };
    let Some(ParamKind::Values(items)) = spec.param_index(id).map(|i| spec.params[i].kind) else { return 0.0 };
    let best = items.iter().enumerate().min_by(|a, b| {
        let da = (f64::from(*a.1) - x).abs();
        let db = (f64::from(*b.1) - x).abs();
        da.total_cmp(&db)
    });
    best.map_or(0.0, |(i, _)| i as f64)
}

#[derive(Default)]
pub struct Converter {
    /// (old strip ref, old insert slot) -> FX slot.
    fx_slots: HashMap<(String, i32), usize>,
    next_fx: usize,
    pub warnings: Vec<String>,
}

/// The parts of an old strip that its own kind can't carry.
struct Dropped {
    gate_on: bool,
    hpf_on: bool,
}

impl Converter {
    /// Converts an old `<Console>` (or v1 `<LiveDAWSession>`) to a version 3 console.
    pub fn convert_console(&mut self, old: &Element) -> Element {
        let mut console = Element::new("Console");
        console.set("version", crate::console::CONSOLE_VERSION);
        copy_attr(old, "outputProtection", &mut console, "outputProtection");
        copy_attr(old, "outputCeilingDb", &mut console, "outputCeilingDb");
        if let Some(m) = old.child("Monitor") {
            console.add(m.clone());
        }

        let mut fx = Element::new("Fx");
        let master: Vec<&Element> = old.child("Master").and_then(|m| m.child("Strip")).into_iter().collect();
        let buses: Vec<&Element> = old.child("Buses").into_iter().flat_map(|b| b.children_named("Strip")).collect();
        let inputs: Vec<&Element> = old.child("Inputs").into_iter().flat_map(|b| b.children_named("Strip")).collect();

        // In the old file's order, so FX slots are handed out the same way every time.
        let mut lists = Vec::new();
        for (tag, family, strips) in [("Mains", "master", master), ("Buses", "bus", buses), ("Channels", "input", inputs)] {
            let mut list = Element::new(tag);
            for e in strips {
                let old_kind = match (family, e.str_or("kind", "")) {
                    ("bus", "group") => "group",
                    ("bus", _) => "aux",
                    (f, _) => f,
                };
                let id = if family == "master" { 0 } else { e.int_or("id", -1) };
                let old_ref = format!("{old_kind}:{id}");
                let mut strip = self.convert_strip(e, &old_ref, true);
                strip.set("id", id);
                copy_attr(e, "recallSafe", &mut strip, "recallSafe");
                self.move_inserts(e, &old_ref, &mut strip, &mut fx);
                list.add(strip);
            }
            lists.push(list);
        }

        console.add(fx);
        for list in lists {
            console.add(list);
        }
        console
    }

    /// Converts an old scene or snippet `<State>` (call after `convert_console`
    /// of the same show, so plugin states find their FX slots).
    pub fn convert_state(&mut self, old: &Element) -> Element {
        let mut state = Element::new("State");
        state.set("version", crate::console::CONSOLE_VERSION);
        copy_attr(old, "sections", &mut state, "sections");
        let mut fx = Element::new("Fx");
        for e in old.children_named("Strip") {
            let old_ref = e.str_or("ref", "").to_owned();
            state.add(self.convert_strip(e, &old_ref, false));
            for ins in e.child("Inserts").into_iter().flat_map(|i| i.children_named("Insert")) {
                let (Some(desc), Some(&slot)) =
                    (ins.child("PLUGIN"), self.fx_slots.get(&(old_ref.clone(), ins.int_or("slot", -1))))
                else {
                    continue;
                };
                let x = fx.add_new("Slot");
                x.set("i", slot);
                copy_attr(ins, "bypass", x, "bypass");
                x.add(desc.clone());
                x.add_new("State").add_text(ins.child("State").map(Element::text).unwrap_or_default());
            }
        }
        if let Some(m) = old.child("Monitor") {
            state.add(m.clone());
        }
        if fx.elements().next().is_some() {
            state.add(fx);
        }
        state
    }

    /// Moves a console strip's plugins to FX slots and assigns its inserts.
    fn move_inserts(&mut self, old: &Element, old_ref: &str, strip: &mut Element, fx: &mut Element) {
        let mut used = 0;
        let mut ins = Element::new("Ins");
        for i in old.child("Inserts").into_iter().flat_map(|i| i.children_named("Insert")) {
            let Some(desc) = i.child("PLUGIN") else { continue };
            let name = desc.str_or("name", "plugin");
            if used == 2 || self.next_fx == NUM_FX {
                let strip_name = old.str_or("name", old_ref);
                self.warnings.push(format!("{strip_name}: {name} could not be moved to an FX slot (removed)"));
                continue;
            }
            let slot = self.next_fx;
            self.next_fx += 1;
            self.fx_slots.insert((old_ref.to_owned(), i.int_or("slot", -1)), slot);

            let x = fx.add_new("Slot");
            x.set("i", slot).set_bool("bypass", i.bool_or("bypass", false)).set("mix", 100);
            x.add(desc.clone());
            x.add_new("State").add_text(i.child("State").map(Element::text).unwrap_or_default());

            let point = ins.add_new(if used == 0 { "Pre" } else { "Post" });
            point.set("on", 1).set("fx", slot).set("mode", 0);
            used += 1;
        }
        if used > 0 {
            strip.add(ins);
        }
    }

    fn convert_strip(&mut self, old: &Element, old_ref: &str, in_console: bool) -> Element {
        let r = convert_ref(old_ref);
        let kind = r.kind;
        let bus = kind.is_output();
        let mut e = Element::new("Strip");
        e.set("ref", r);
        copy_attr(old, "sections", &mut e, "sections");
        let mut dropped = Dropped { gate_on: false, hpf_on: false };

        // NAME
        copy_attr(old, "name", &mut e, "name");
        if let Some(c) = old.attr("colour") {
            let argb = c.chars().filter_map(|c| c.to_digit(16)).fold(0u32, |acc, d| acc.wrapping_shl(4) | d);
            e.set("colour", nearest_colour(argb & 0xff_ffff));
        }

        // ROUTING
        if ["stereo", "inL", "inR", "outL", "outR"].iter().any(|a| old.has_attr(a)) {
            let route = e.add_new("Route");
            for a in ["inL", "inR", "outL", "outR"] {
                copy_attr(old, a, route, a);
            }
            if kind == StripKind::Channel {
                match old.attr("stereo") {
                    Some(s) => {
                        route.set("stereo", s);
                    }
                    // Sessions from before per-channel width: a right input meant stereo.
                    None if in_console => {
                        route.set_bool("stereo", old.int_or("inR", -1) >= 0);
                    }
                    None => {}
                }
            }
        }

        // PREAMP
        if old.has_attr("trimDb") || old.has_attr("polarity") {
            let i = e.add_new("In");
            copy_attr(old, "trimDb", i, "trim");
            copy_attr(old, "polarity", i, "inv");
        }
        let mut filter = Element::new("Flt");
        if old.has_attr("hpf") || old.has_attr("hpfFreq") {
            if kind == StripKind::Channel {
                copy_attr(old, "hpf", &mut filter, "lc");
                copy_attr(old, "hpfFreq", &mut filter, "lcf");
                filter.set("lcs", 1); // 12 dB/oct
            } else {
                dropped.hpf_on = old.bool_or("hpf", false);
            }
        }

        // GATE
        if let Some(g) = old.child("Gate") {
            if kind == StripKind::Channel {
                let gate = e.add_new("Gate");
                copy_attr(g, "on", gate, "on");
                gate.set("mdl", "GATE");
                for (from, to) in
                    [("threshold", "thr"), ("range", "range"), ("attack", "att"), ("hold", "hld"), ("release", "rel")]
                {
                    if g.has_attr(from) {
                        let v = g.double_or(from, 0.0);
                        // The old gate stored its range as a negative gain.
                        param(gate, to, if to == "range" { v.abs() } else { v });
                    }
                }
            } else {
                dropped.gate_on = g.bool_or("on", false);
            }
        }

        // EQ
        if let Some(q) = old.child("Eq") {
            let eq = self.convert_eq(q, bus, &mut filter);
            e.add(eq);
        }
        if !filter.attrs.is_empty() {
            e.add(filter);
        }

        // DYNAMICS
        if let Some(c) = old.child("Comp") {
            let d = e.add_new("Dyn");
            copy_attr(c, "on", d, "on");
            d.set("mdl", "COMP");
            copy_attr(c, "makeup", d, "gain");
            if c.has_attr("threshold") {
                param(d, "thr", c.double_or("threshold", -20.0));
            }
            if c.has_attr("ratio") {
                param(d, "ratio", nearest_value(DYN_MODELS, "COMP", "ratio", c.double_or("ratio", 4.0)));
            }
            if c.has_attr("knee") {
                param(d, "knee", (c.double_or("knee", 6.0) / 3.0).round().clamp(0.0, 5.0));
            }
            if c.has_attr("attack") {
                param(d, "att", c.double_or("attack", 10.0));
            }
            if c.has_attr("release") {
                param(d, "rel", c.double_or("release", 150.0));
            }
        }

        // SENDS (inputs only had bus sends; the main assign moves here from ROUTING)
        if let Some(sends) = old.child("Sends") {
            let s = e.add_new("Sends");
            for x in sends.children_named("Send") {
                let n = s.add_new("S");
                copy_attr(x, "slot", n, "slot");
                copy_attr(x, "on", n, "on");
                copy_attr(x, "level", n, "lvl");
                n.set("mode", if x.bool_or("pre", true) { 0 } else { 1 }).set("pan", 0).set("plink", 1);
            }
        }
        if kind != StripKind::Main && old.has_attr("mainAssign") {
            e.add_new("Mains").add_new("S").set("i", 0).set_bool("on", old.bool_or("mainAssign", false));
        }

        // FADER
        if ["faderDb", "pan", "mute"].iter().any(|a| old.has_attr(a)) {
            let f = e.add_new("Fdr");
            copy_attr(old, "faderDb", f, "lvl");
            if old.has_attr("pan") {
                f.set("pan", fmt_f32((old.double_or("pan", 0.0) * 100.0) as f32));
            }
            copy_attr(old, "mute", f, "mute");
        }

        let what = old.str_or("name", old_ref);
        if dropped.gate_on {
            self.warnings.push(format!("{what}: gate removed (buses and mains have no gate)"));
        }
        if dropped.hpf_on {
            self.warnings.push(format!("{what}: low cut removed (buses and mains have no filter)"));
        }
        e
    }

    /// The old 4-band EQ as the WING EQ. Cut bands go to the channel filter
    /// (`filter`) or, on buses, to the L/H bands' cut types.
    fn convert_eq(&mut self, old: &Element, bus: bool, filter: &mut Element) -> Element {
        let table = if bus { EQ_MODELS_BUS } else { EQ_MODELS_CH };
        let mut eq = Element::new("Eq");
        copy_attr(old, "on", &mut eq, "on");
        eq.set("mdl", "STD");

        let mids: &[&str] = if bus { &["2", "3", "4", "5", "1", "6"] } else { &["2", "3", "1", "4"] };
        let mut free_mids: Vec<&str> = mids.to_vec();
        let (mut low_free, mut high_free) = (true, true);

        let mut bands: Vec<&Element> = old.children_named("Band").collect();
        bands.sort_by_key(|b| b.int_or("index", 0));
        for b in bands {
            if !b.bool_or("on", true) {
                continue;
            }
            let (f, g, q) = (b.double_or("freq", 1000.0), b.double_or("gain", 0.0), b.double_or("q", 0.707));
            let band_type = b.int_or("type", 0); // 0 bell, 1 low shelf, 2 high shelf, 3 low cut, 4 high cut
            match band_type {
                3 | 4 if !bus => {
                    let (on, freq, slope) = if band_type == 3 { ("lc", "lcf", "lcs") } else { ("hc", "hcf", "hcs") };
                    filter.set(on, 1).set(freq, fmt_f32(f as f32)).set(slope, 1);
                }
                1 | 3 if low_free => {
                    low_free = false;
                    let shape = if band_type == 1 { "SHV" } else { "CUT" };
                    param(&mut eq, "lf", f);
                    param(&mut eq, "lq", q);
                    param(&mut eq, "lg", g);
                    param(&mut eq, "leq", choice_index(table, "STD", "leq", shape).unwrap_or(1) as f64);
                }
                2 | 4 if high_free => {
                    high_free = false;
                    let shape = if band_type == 2 { "SHV" } else { "CUT" };
                    param(&mut eq, "hf", f);
                    param(&mut eq, "hq", q);
                    param(&mut eq, "hg", g);
                    param(&mut eq, "heq", choice_index(table, "STD", "heq", shape).unwrap_or(1) as f64);
                }
                3 | 4 => self.warnings.push("EQ: a second cut band could not be converted (removed)".into()),
                _ => {
                    if !free_mids.is_empty() {
                        let m = free_mids.remove(0);
                        param(&mut eq, &format!("{m}f"), f);
                        param(&mut eq, &format!("{m}q"), q);
                        param(&mut eq, &format!("{m}g"), g);
                    } else if low_free || high_free {
                        let p = if low_free { "l" } else { "h" };
                        if low_free {
                            low_free = false;
                        } else {
                            high_free = false;
                        }
                        param(&mut eq, &format!("{p}f"), f);
                        param(&mut eq, &format!("{p}q"), q);
                        param(&mut eq, &format!("{p}g"), g);
                        param(&mut eq, &format!("{p}eq"), 0.0);
                    }
                }
            }
        }
        // Bands not used by the old EQ stay flat; L/H default to shelves.
        if low_free {
            param(&mut eq, "lg", 0.0);
        }
        if high_free {
            param(&mut eq, "hg", 0.0);
        }
        for m in free_mids {
            param(&mut eq, &format!("{m}g"), 0.0);
        }
        eq
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::xml::parse;

    fn p(e: &Element, id: &str) -> Option<f64> {
        e.children_named("P").find(|p| p.str_or("n", "") == id).map(|p| p.double_or("v", f64::NAN))
    }

    #[test]
    fn refs() {
        assert_eq!(convert_ref("input:3"), StripRef::new(StripKind::Channel, 3));
        assert_eq!(convert_ref("aux:79"), StripRef::new(StripKind::Bus, 79));
        assert_eq!(convert_ref("group:2"), StripRef::new(StripKind::Bus, 2));
        assert_eq!(convert_ref("master:0"), StripRef::new(StripKind::Main, 0));
        assert_eq!(convert_refs("input:1, master:0"), "ch:1,main:0");
    }

    #[test]
    fn strip_blocks() {
        let old = parse(
            r#"<Strip ref="input:4" sections="511" name="Kick" colour="ffe83a3a" stereo="0" inL="3" inR="-1"
                 outL="-1" outR="-1" mainAssign="1" trimDb="6" polarity="1" hpf="1" hpfFreq="90"
                 faderDb="-5" pan="-0.5" mute="1">
               <Gate on="1" threshold="-33" range="-40" attack="1" hold="20" release="150"/>
               <Eq on="1">
                 <Band index="0" type="1" freq="120" gain="3" q="0.7" on="1"/>
                 <Band index="1" type="0" freq="400" gain="-4" q="2" on="1"/>
                 <Band index="2" type="0" freq="2500" gain="0" q="1" on="0"/>
                 <Band index="3" type="4" freq="12000" gain="0" q="0.7" on="1"/>
               </Eq>
               <Comp on="1" threshold="-18" ratio="4.2" attack="5" release="80" knee="6" makeup="3"/>
               <Sends><Send slot="2" on="1" level="-3" pre="0"/></Sends>
             </Strip>"#,
        )
        .unwrap();
        let mut c = Converter::default();
        let s = c.convert_strip(&old, "input:4", true);
        assert_eq!(s.str_or("ref", ""), "ch:4");
        assert_eq!(s.int_or("colour", 0), 9);
        assert_eq!(s.child("Route").unwrap().int_or("inL", 0), 3);
        assert_eq!(s.child("In").unwrap().double_or("trim", 0.0), 6.0);
        let flt = s.child("Flt").unwrap();
        assert_eq!((flt.int_or("lc", 0), flt.double_or("lcf", 0.0)), (1, 90.0));
        assert_eq!((flt.int_or("hc", 0), flt.double_or("hcf", 0.0)), (1, 12000.0));
        let gate = s.child("Gate").unwrap();
        assert_eq!(p(gate, "thr"), Some(-33.0));
        assert_eq!(p(gate, "hld"), Some(20.0));
        assert_eq!(p(gate, "range"), Some(40.0));
        let eq = s.child("Eq").unwrap();
        assert_eq!((p(eq, "lf"), p(eq, "lg"), p(eq, "leq")), (Some(120.0), Some(3.0), Some(1.0)));
        assert_eq!((p(eq, "2f"), p(eq, "2g"), p(eq, "2q")), (Some(400.0), Some(-4.0), Some(2.0)));
        assert_eq!(p(eq, "3g"), Some(0.0));
        let dyn_ = s.child("Dyn").unwrap();
        assert_eq!(dyn_.str_or("mdl", ""), "COMP");
        assert_eq!(dyn_.double_or("gain", 0.0), 3.0);
        assert_eq!(p(dyn_, "ratio"), Some(9.0)); // 4.0
        assert_eq!(p(dyn_, "knee"), Some(2.0));
        let send = s.child("Sends").unwrap().child("S").unwrap();
        assert_eq!((send.int_or("slot", 0), send.double_or("lvl", 0.0), send.int_or("mode", -1)), (2, -3.0, 1));
        assert_eq!(s.child("Mains").unwrap().child("S").unwrap().int_or("on", 0), 1);
        let f = s.child("Fdr").unwrap();
        assert_eq!((f.double_or("lvl", 0.0), f.double_or("pan", 0.0), f.int_or("mute", 0)), (-5.0, -50.0, 1));
        assert!(c.warnings.is_empty());
    }

    #[test]
    fn console_moves_plugins_to_fx_slots() {
        let old = parse(
            r#"<Console outputCeilingDb="-1">
                 <Master><Strip kind="master" id="0" faderDb="-2"><Gate on="1"/></Strip></Master>
                 <Buses><Strip kind="group" id="5" mainAssign="1"/></Buses>
                 <Inputs>
                   <Strip kind="input" id="7" name="Vox" inL="0" inR="1"><Inserts>
                     <Insert slot="0"><PLUGIN name="A"/><State>a</State></Insert>
                     <Insert slot="3" bypass="1"><PLUGIN name="B"/><State>b</State></Insert>
                     <Insert slot="4"><PLUGIN name="C"/><State>c</State></Insert>
                   </Inserts></Strip>
                 </Inputs>
               </Console>"#,
        )
        .unwrap();
        let mut c = Converter::default();
        let console = c.convert_console(&old);
        assert_eq!(console.int_or("version", 0), 3);
        assert_eq!(console.double_or("outputCeilingDb", 0.0), -1.0);

        let vox = console.child("Channels").unwrap().child("Strip").unwrap();
        assert_eq!(vox.int_or("id", 0), 7);
        assert_eq!(vox.child("Route").unwrap().int_or("stereo", 0), 1);
        let ins = vox.child("Ins").unwrap();
        assert_eq!(ins.child("Pre").unwrap().int_or("fx", -1), 0);
        assert_eq!(ins.child("Post").unwrap().int_or("fx", -1), 1);
        let slots: Vec<_> = console.child("Fx").unwrap().children_named("Slot").collect();
        assert_eq!(slots.len(), 2);
        assert_eq!(slots[1].child("PLUGIN").unwrap().str_or("name", ""), "B");
        assert_eq!(slots[1].int_or("bypass", 0), 1);
        assert_eq!(slots[1].child("State").unwrap().text(), "b");
        assert_eq!(
            c.warnings,
            ["master:0: gate removed (buses and mains have no gate)", "Vox: C could not be moved to an FX slot (removed)"]
        );

        let bus = console.child("Buses").unwrap().child("Strip").unwrap();
        assert_eq!(bus.str_or("ref", ""), "bus:5");
        assert_eq!(bus.child("Mains").unwrap().child("S").unwrap().int_or("on", 0), 1);
        assert_eq!(console.child("Mains").unwrap().child("Strip").unwrap().str_or("ref", ""), "main:0");

        // A scene state finds the plugin's FX slot.
        let state = parse(
            r#"<State sections="64"><Strip ref="input:7" sections="64"><Inserts>
                 <Insert slot="3"><PLUGIN name="B"/><State>b2</State></Insert></Inserts></Strip></State>"#,
        )
        .unwrap();
        let st = c.convert_state(&state);
        assert_eq!(st.int_or("version", 0), 3);
        let slot = st.child("Fx").unwrap().child("Slot").unwrap();
        assert_eq!((slot.int_or("i", -1), slot.child("State").unwrap().text()), (1, "b2".to_string()));
    }
}
