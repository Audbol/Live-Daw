//! Recall scopes beyond console sections, global safes beyond strips, partial
//! updates and snippet recording (the WING's LIBRARY: SCOPE, GLOBAL, PARTIAL
//! LOAD / UPDATE, RESTORE, snippet REC / STOP).
//!
//! A [`Scope`] says what a recall touches: the strip sections (the CONTENTS
//! keys), which strips, which DCAs, mute groups and FX slots, and whether the
//! console-wide settings (monitor setup, INPUT SELECT, automix enables) come
//! along. DCAs, mute groups and FX slots still follow their section (DCA
//! levels with FADER, mute group states with GROUPS, FX slots with INSERTS);
//! the masks choose which of them.
//!
//! Scopes work on captured `<State>` elements: [`filter_state`] keeps what a
//! scope (minus the [`GlobalSafe`]s) lets through, and the regular
//! `console::apply` does the rest. [`merge_state`] puts a partial capture into
//! a stored one (PARTIAL UPDATE). [`Recording`] compares the console with the
//! one it saw at REC to find the parameters that changed.

use crate::console::{self, section, ApplyResult, FadeTarget, NoPlugins, PluginBridge};
use crate::refs::{refs_from_string, refs_to_string, StripRef};
use crate::xml::{parse_int_prefix, Element, Node};
use livedaw_engine::params::{NUM_DCAS, NUM_FX, NUM_MUTE_GROUPS};
use livedaw_engine::Engine;
use std::collections::HashMap;
use std::sync::atomic::Ordering::Relaxed;

/// The number of changed values per strip and section (one bit).
pub type ChangeCounts = HashMap<(StripRef, u32), usize>;

pub const ALL_DCAS: u32 = (1 << NUM_DCAS) - 1;
pub const ALL_MUTE_GROUPS: u32 = (1 << NUM_MUTE_GROUPS) - 1;
pub const ALL_FX: u32 = (1 << NUM_FX) - 1;

/// Console-wide parts of a state that belong to no strip, DCA, mute group or
/// FX slot (the monitor setup, INPUT SELECT, the automix enables and anything
/// added later).
fn is_console_part(name: &str) -> bool {
    !matches!(name, "Strip" | "Dcas" | "MuteGroups" | "Fx")
}

/// What a recall, a partial update or a snippet touches.
#[derive(Clone, Debug, PartialEq)]
pub struct Scope {
    /// Strip sections (`console::section`).
    pub sections: u32,
    /// The strips; `None` = every strip.
    pub strips: Option<Vec<StripRef>>,
    /// DCAs (bit n = DCA n+1), recalled with FADER (levels, mutes) and NAME.
    pub dcas: u32,
    /// Mute groups (bit n = group n+1), recalled with GROUPS.
    pub mute_groups: u32,
    /// FX slots (bit n = FX n+1), recalled with INSERTS.
    pub fx: u32,
    /// The console-wide settings: monitor setup and INPUT SELECT (with
    /// ROUTING), automix enables (with INSERTS).
    pub console: bool,
}

impl Default for Scope {
    fn default() -> Self {
        Self::console(section::ALL)
    }
}

impl Scope {
    /// The whole console within `sections`.
    pub fn console(sections: u32) -> Self {
        Self {
            sections: sections & section::ALL,
            strips: None,
            dcas: ALL_DCAS,
            mute_groups: ALL_MUTE_GROUPS,
            fx: ALL_FX,
            console: true,
        }
    }

    /// Only `sections` of `strips` (a snippet's classic scope).
    pub fn strips(sections: u32, strips: Vec<StripRef>) -> Self {
        Self { sections: sections & section::ALL, strips: Some(strips), dcas: 0, mute_groups: 0, fx: 0, console: false }
    }

    pub fn includes_strip(&self, r: &StripRef) -> bool {
        self.strips.as_ref().is_none_or(|s| s.contains(r))
    }

    /// True when the scope reaches beyond its strips.
    pub fn has_globals(&self) -> bool {
        self.dcas != 0 || self.mute_groups != 0 || self.fx != 0 || self.console
    }

    /// Nothing at all.
    pub fn is_empty(&self) -> bool {
        (self.sections == 0 || self.strips.as_ref().is_some_and(Vec::is_empty)) && !self.has_globals()
    }

    /// Clamps the masks to the console's items.
    pub fn sanitised(mut self) -> Self {
        self.sections &= section::ALL;
        self.dcas &= ALL_DCAS;
        self.mute_groups &= ALL_MUTE_GROUPS;
        self.fx &= ALL_FX;
        self
    }

    /// Writes the scope as attributes (`<prefix>Strips` only when limited).
    /// `sections_attr` names the sections attribute (scenes keep "scope").
    pub fn write_attrs(&self, e: &mut Element, sections_attr: &str) {
        e.set(sections_attr, self.sections as i32);
        if let Some(strips) = &self.strips {
            e.set("scopeStrips", refs_to_string(strips));
        }
        e.set("scopeDca", self.dcas)
            .set("scopeMgrp", self.mute_groups)
            .set("scopeFx", self.fx)
            .set_bool("scopeConsole", self.console);
    }

    /// Reads attributes written by [`Scope::write_attrs`]; anything missing
    /// (older shows) comes from `defaults`.
    pub fn read_attrs(e: &Element, sections_attr: &str, defaults: &Scope) -> Scope {
        let mask = |name: &str, default: u32| e.attr(name).map_or(default, |v| parse_int_prefix(v) as u32);
        Scope {
            sections: mask(sections_attr, defaults.sections),
            strips: match e.attr("scopeStrips") {
                Some(s) => Some(refs_from_string(s)),
                None => defaults.strips.clone(),
            },
            dcas: mask("scopeDca", defaults.dcas),
            mute_groups: mask("scopeMgrp", defaults.mute_groups),
            fx: mask("scopeFx", defaults.fx),
            console: e.bool_or("scopeConsole", defaults.console),
        }
        .sanitised()
    }
}

/// GLOBAL safes beyond the per-strip recall safe: DCAs, mute groups, FX slots,
/// content sections and the console-wide settings that scene recalls leave
/// alone. RESTORE and PARTIAL LOAD with NO GLOB ignore them.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct GlobalSafe {
    pub sections: u32,
    pub dcas: u32,
    pub mute_groups: u32,
    pub fx: u32,
    pub console: bool,
}

impl GlobalSafe {
    pub fn is_empty(&self) -> bool {
        *self == GlobalSafe::default()
    }

    pub fn sanitised(mut self) -> Self {
        self.sections &= section::ALL;
        self.dcas &= ALL_DCAS;
        self.mute_groups &= ALL_MUTE_GROUPS;
        self.fx &= ALL_FX;
        self
    }

    pub fn to_xml(&self) -> Element {
        let mut e = Element::new("GlobalSafe");
        e.set("sections", self.sections)
            .set("dcas", self.dcas)
            .set("mgrps", self.mute_groups)
            .set("fx", self.fx)
            .set_bool("console", self.console);
        e
    }

    pub fn from_xml(e: &Element) -> Self {
        let mask = |name: &str| e.attr(name).map_or(0, |v| parse_int_prefix(v) as u32);
        GlobalSafe {
            sections: mask("sections"),
            dcas: mask("dcas"),
            mute_groups: mask("mgrps"),
            fx: mask("fx"),
            console: e.bool_or("console", false),
        }
        .sanitised()
    }
}

fn index_bit(i: i32) -> u32 {
    if (0..32).contains(&i) {
        1 << i
    } else {
        0
    }
}

fn index_of(e: &Element) -> i32 {
    e.int_or("i", -1)
}

/// Keeps the children of `e` named `name` whose "i" is in `mask`.
fn keep_indexed(e: &Element, name: &str, mask: u32) -> Element {
    let mut out = Element { name: e.name.clone(), attrs: e.attrs.clone(), children: Vec::new() };
    for c in e.elements() {
        if c.name != name || mask & index_bit(index_of(c)) != 0 {
            out.add(c.clone());
        }
    }
    out
}

/// What of `state` a recall within `scope` touches, minus the global safes.
/// The mute group states outside the scope are set to the console's current
/// ones, so applying the result leaves them alone.
pub fn filter_state(engine: &Engine, state: &Element, scope: &Scope, safe: Option<&GlobalSafe>) -> Element {
    let none = GlobalSafe::default();
    let safe = safe.unwrap_or(&none);
    let dcas = scope.dcas & !safe.dcas;
    let groups = scope.mute_groups & !safe.mute_groups;
    let fx = scope.fx & !safe.fx;
    let console = scope.console && !safe.console;

    let mut out = Element { name: state.name.clone(), attrs: state.attrs.clone(), children: Vec::new() };
    for c in state.elements() {
        match c.name.as_str() {
            "Strip" => {
                if scope.includes_strip(&StripRef::parse(c.str_or("ref", ""))) {
                    out.add(c.clone());
                }
            }
            "Dcas" if dcas != 0 => {
                out.add(keep_indexed(c, "Dca", dcas));
            }
            "MuteGroups" if groups != 0 => {
                let mut m = keep_indexed(c, "G", groups);
                if let Some(saved) = c.attr("on").map(|v| v.trim().parse::<u32>().unwrap_or(0)) {
                    let current = engine.controls.mute_groups.load(Relaxed);
                    m.set("on", (current & !groups) | (saved & groups));
                }
                out.add(m);
            }
            "Fx" if fx != 0 => {
                out.add(keep_indexed(c, "Slot", fx));
            }
            name if is_console_part(name) && console => {
                out.add(c.clone());
            }
            _ => {}
        }
    }
    out
}

/// Applies `state` within `scope`, leaving out the global safes (and with
/// `respect_recall_safe`, the recall-safe strips).
pub fn apply_scoped(
    engine: &Engine,
    state: &Element,
    scope: &Scope,
    safe: Option<&GlobalSafe>,
    respect_recall_safe: bool,
    fades: Option<&mut Vec<FadeTarget>>,
    bridge: &mut dyn PluginBridge,
) -> ApplyResult {
    let filtered = filter_state(engine, state, scope, safe);
    let sections = scope.sections & !safe.map_or(0, |s| s.sections);
    console::apply(engine, &filtered, sections, None, respect_recall_safe, fades, bridge)
}

/// The sections that carry the scope's DCAs (FADER, NAME), mute groups
/// (GROUPS, NAME), FX slots (INSERTS) and console-wide settings (ROUTING,
/// INSERTS) in a capture.
pub fn global_sections(scope: &Scope) -> u32 {
    let mut s = 0;
    if scope.dcas != 0 {
        s |= section::FADER | section::NAME;
    }
    if scope.mute_groups != 0 {
        s |= section::GROUPS | section::NAME;
    }
    if scope.fx != 0 {
        s |= section::INSERTS;
    }
    if scope.console {
        s |= section::ROUTING | section::INSERTS;
    }
    s
}

/// Captures what `scope` covers: its strips with its sections, and its
/// DCAs, mute groups, FX slots and console-wide settings whatever its
/// sections (a recall still applies them only with their section).
pub fn capture_scoped(engine: &Engine, scope: &Scope, bridge: &mut dyn PluginBridge) -> Element {
    let strips = console::capture(engine, scope.sections, scope.strips.as_deref(), bridge);
    let globals = global_sections(scope);
    let mut out = Element::new("State");
    out.attrs.clone_from(&strips.attrs);
    out.set("sections", scope.sections | globals);
    for c in strips.children_named("Strip") {
        out.add(c.clone());
    }
    if globals == 0 {
        return out;
    }
    // A capture of every strip, for its console-wide parts.
    let full = console::capture(engine, globals, None, bridge);
    for c in full.elements() {
        match c.name.as_str() {
            "Strip" => {}
            "Dcas" if scope.dcas != 0 => {
                out.add(keep_indexed(c, "Dca", scope.dcas));
            }
            "MuteGroups" if scope.mute_groups != 0 => {
                out.add(keep_indexed(c, "G", scope.mute_groups));
            }
            "Fx" if scope.fx != 0 => {
                out.add(keep_indexed(c, "Slot", scope.fx));
            }
            name if is_console_part(name) && scope.console => {
                out.add(c.clone());
            }
            _ => {}
        }
    }
    out
}

fn child_index_mut<'a>(e: &'a mut Element, name: &str, key: &str, value: &str) -> Option<&'a mut Element> {
    e.children.iter_mut().find_map(|c| match c {
        Node::Element(x) if x.name == name && x.attr(key) == Some(value) => Some(x),
        _ => None,
    })
}

fn merge_attrs(into: &mut Element, from: &Element) {
    for (k, v) in &from.attrs {
        into.set(k, v);
    }
}

/// Replaces the children named like `from`'s, and its attributes.
fn merge_strip(into: &mut Element, from: &Element) {
    let sections = into.int_or("sections", 0) as u32 | from.int_or("sections", 0) as u32;
    for (k, v) in &from.attrs {
        if k != "ref" && k != "sections" {
            into.set(k, v);
        }
    }
    into.set("sections", sections);
    for c in from.elements() {
        into.children.retain(|x| !matches!(x, Node::Element(e) if e.name == c.name));
        into.add(c.clone());
    }
}

/// PARTIAL UPDATE: puts `partial` (a scoped capture) into `stored`: the
/// strips' captured sections, the DCAs, mute groups and FX slots it holds and
/// its console-wide parts replace the stored ones; the rest stays.
pub fn merge_state(stored: &mut Element, partial: &Element) {
    for c in partial.elements() {
        match c.name.as_str() {
            "Strip" => {
                let r = c.str_or("ref", "");
                match child_index_mut(stored, "Strip", "ref", r) {
                    Some(s) => merge_strip(s, c),
                    None => {
                        stored.add(c.clone());
                    }
                }
            }
            "Dcas" | "MuteGroups" => {
                let item = if c.name == "Dcas" { "Dca" } else { "G" };
                if stored.child(&c.name).is_none() {
                    stored.add_new(&c.name);
                }
                let Some(target) = stored.children.iter_mut().find_map(|x| match x {
                    Node::Element(e) if e.name == c.name => Some(e),
                    _ => None,
                }) else {
                    continue;
                };
                merge_attrs(target, c);
                for x in c.children_named(item) {
                    let i = x.str_or("i", "");
                    match child_index_mut(target, item, "i", i) {
                        Some(t) => merge_attrs(t, x),
                        None => {
                            target.add(x.clone());
                        }
                    }
                }
            }
            "Fx" => {
                if stored.child("Fx").is_none() {
                    stored.add_new("Fx");
                }
                let Some(target) = stored.children.iter_mut().find_map(|x| match x {
                    Node::Element(e) if e.name == "Fx" => Some(e),
                    _ => None,
                }) else {
                    continue;
                };
                for x in c.children_named("Slot") {
                    let i = x.str_or("i", "").to_owned();
                    target
                        .children
                        .retain(|n| !matches!(n, Node::Element(e) if e.name == "Slot" && e.attr("i") == Some(i.as_str())));
                    target.add(x.clone());
                }
            }
            _ => {
                stored.children.retain(|x| !matches!(x, Node::Element(e) if e.name == c.name));
                stored.add(c.clone());
            }
        }
    }
}

// ---- snippet recording -------------------------------------------------------------------

/// The key of a child for pairing it between two captures.
fn pair_key(e: &Element) -> String {
    for key in ["ref", "i", "slot", "n"] {
        if let Some(v) = e.attr(key) {
            return format!("{}:{key}={v}", e.name);
        }
    }
    e.name.clone()
}

/// How many values differ between two captures of the same thing: changed,
/// added or removed attributes (except the bookkeeping `ref` and `sections`),
/// recursively through children paired by their index.
pub fn count_differences(a: &Element, b: &Element) -> usize {
    let skip = |k: &str| k == "ref" || k == "sections";
    let mut n = 0;
    for (k, v) in &a.attrs {
        if !skip(k) && b.attr(k) != Some(v.as_str()) {
            n += 1;
        }
    }
    for (k, _) in &b.attrs {
        if !skip(k) && !a.has_attr(k) {
            n += 1;
        }
    }
    let mut theirs: HashMap<String, &Element> = b.elements().map(|e| (pair_key(e), e)).collect();
    for c in a.elements() {
        match theirs.remove(&pair_key(c)) {
            Some(other) => n += count_differences(c, other),
            None => n += c.attrs.len().max(1),
        }
    }
    n += theirs.values().map(|e| e.attrs.len().max(1)).sum::<usize>();
    let (ta, tb) = (direct_text(a), direct_text(b));
    if ta != tb {
        n += 1;
    }
    n
}

fn direct_text(e: &Element) -> String {
    e.children.iter().filter_map(|c| if let Node::Text(t) = c { Some(t.as_str()) } else { None }).collect()
}

/// What changed since REC: per strip the changed sections, the changed DCAs,
/// mute groups and FX slots, and the number of changed parameters.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct RecordedChanges {
    pub strips: Vec<(StripRef, u32)>,
    pub dcas: u32,
    pub mute_groups: u32,
    pub fx: u32,
    pub console: bool,
    /// Every section with a change (the console-wide ones included).
    pub sections: u32,
    pub count: usize,
}

impl RecordedChanges {
    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    /// Only what `focus` (REC FOCUS) lets through. The count goes down by an
    /// estimate: changes of dropped items are no longer counted.
    pub fn limited_to(&self, focus: &Scope, counts: &ChangeCounts) -> RecordedChanges {
        let mut out = RecordedChanges {
            dcas: self.dcas & focus.dcas,
            mute_groups: self.mute_groups & focus.mute_groups,
            fx: self.fx & focus.fx,
            console: self.console && focus.console,
            ..Default::default()
        };
        for (r, mask) in &self.strips {
            let m = mask & focus.sections;
            if m != 0 && focus.includes_strip(r) {
                out.strips.push((*r, m));
                out.sections |= m;
            }
        }
        out.count = out
            .strips
            .iter()
            .map(|(r, m)| {
                section_bits().filter(|b| m & b != 0).map(|b| counts.get(&(*r, b)).copied().unwrap_or(1)).sum::<usize>()
            })
            .sum();
        out.count +=
            (out.dcas.count_ones() + out.mute_groups.count_ones() + out.fx.count_ones()) as usize + usize::from(out.console);
        if out.dcas != 0 {
            out.sections |= section::FADER;
        }
        if out.mute_groups != 0 {
            out.sections |= section::GROUPS;
        }
        if out.fx != 0 {
            out.sections |= section::INSERTS;
        }
        out
    }

    /// The scope a snippet of these changes recalls.
    pub fn scope(&self) -> Scope {
        Scope {
            sections: self.sections,
            strips: Some(self.strips.iter().map(|(r, _)| *r).collect()),
            dcas: self.dcas,
            mute_groups: self.mute_groups,
            fx: self.fx,
            console: self.console,
        }
    }

    /// Captures the changed values: each strip with only its changed sections,
    /// and the changed DCAs, mute groups, FX slots and console-wide settings.
    pub fn capture(&self, engine: &Engine, bridge: &mut dyn PluginBridge) -> Element {
        let mut state = Element::new("State");
        state.set("version", console::CONSOLE_VERSION).set("sections", self.sections);
        for (r, mask) in &self.strips {
            let one = console::capture(engine, *mask, Some(std::slice::from_ref(r)), bridge);
            for s in one.children_named("Strip") {
                state.add(s.clone());
            }
        }
        if self.dcas != 0 || self.mute_groups != 0 || self.fx != 0 || self.console {
            let globals = Scope { strips: Some(Vec::new()), ..self.scope() };
            let captured = capture_scoped(engine, &globals, bridge);
            for c in captured.elements().filter(|c| c.name != "Strip") {
                state.add(c.clone());
            }
        }
        state
    }
}

/// Snippet REC: the console as it was at REC, section by section, to compare
/// with later. Plugins are left out (their state is not recorded).
pub struct Recording {
    base: Vec<Element>,
}

/// The ten strip sections, one bit each.
fn section_bits() -> impl Iterator<Item = u32> {
    (0..32).map(|b| 1u32 << b).filter(|b| section::ALL & b != 0)
}

fn capture_sections(engine: &Engine) -> Vec<Element> {
    section_bits().map(|b| console::capture(engine, b, None, &mut NoPlugins)).collect()
}

impl Recording {
    pub fn start(engine: &Engine) -> Self {
        Self { base: capture_sections(engine) }
    }

    /// What changed since REC, and the number of changed values per strip and
    /// section (for REC FOCUS).
    pub fn changes(&self, engine: &Engine) -> (RecordedChanges, ChangeCounts) {
        let now = capture_sections(engine);
        let mut out = RecordedChanges::default();
        let mut per_strip: HashMap<StripRef, u32> = HashMap::new();
        let mut counts: ChangeCounts = HashMap::new();
        let mut order: Vec<StripRef> = Vec::new();

        for (bit, (base, now)) in section_bits().zip(self.base.iter().zip(&now)) {
            let before: HashMap<&str, &Element> = base.children_named("Strip").map(|s| (s.str_or("ref", ""), s)).collect();
            for s in now.children_named("Strip") {
                let Some(old) = before.get(s.str_or("ref", "")) else { continue }; // strips added since REC
                let n = count_differences(old, s);
                if n > 0 {
                    let r = StripRef::parse(s.str_or("ref", ""));
                    if !per_strip.contains_key(&r) {
                        order.push(r);
                    }
                    *per_strip.entry(r).or_default() |= bit;
                    counts.insert((r, bit), n);
                    out.sections |= bit;
                    out.count += n;
                }
            }
            for c in now.elements().filter(|c| c.name != "Strip") {
                let old = base.child(&c.name);
                match c.name.as_str() {
                    "Dcas" | "MuteGroups" | "Fx" => {
                        let item = match c.name.as_str() {
                            "Dcas" => "Dca",
                            "MuteGroups" => "G",
                            _ => "Slot",
                        };
                        let mut mask = 0;
                        for x in c.children_named(item) {
                            let i = x.str_or("i", "");
                            let n = old
                                .and_then(|o| o.children_named(item).find(|y| y.str_or("i", "") == i))
                                .map_or(1, |y| count_differences(y, x));
                            if n > 0 {
                                mask |= index_bit(parse_int_prefix(i));
                                out.count += n;
                            }
                        }
                        if c.name == "MuteGroups" {
                            let on = |e: Option<&Element>| {
                                e.and_then(|e| e.attr("on")).map_or(0, |v| v.trim().parse::<u32>().unwrap_or(0))
                            };
                            let changed = on(Some(c)) ^ on(old);
                            out.count += changed.count_ones() as usize;
                            mask |= changed;
                        }
                        if mask != 0 {
                            out.sections |= bit;
                            match c.name.as_str() {
                                "Dcas" => out.dcas |= mask,
                                "MuteGroups" => out.mute_groups |= mask,
                                _ => out.fx |= mask,
                            }
                        }
                    }
                    _ => {
                        let n = old.map_or(1, |o| count_differences(o, c));
                        if n > 0 {
                            out.console = true;
                            out.sections |= bit;
                            out.count += n;
                        }
                    }
                }
            }
        }
        out.dcas &= ALL_DCAS;
        out.mute_groups &= ALL_MUTE_GROUPS;
        out.fx &= ALL_FX;
        out.strips = order.into_iter().map(|r| (r, per_strip[&r])).collect();
        (out, counts)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use livedaw_engine::StripKind;

    fn el(xml: &str) -> Element {
        crate::xml::parse(xml).unwrap()
    }

    #[test]
    fn scope_attributes_round_trip_with_defaults() {
        let mut e = Element::new("Scene");
        let s = Scope {
            sections: section::FADER | section::EQ,
            strips: Some(vec![StripRef::new(StripKind::Channel, 3), StripRef::new(StripKind::Bus, 0)]),
            dcas: 0b101,
            mute_groups: 0,
            fx: 1 << 15,
            console: false,
        };
        s.write_attrs(&mut e, "scope");
        assert_eq!(Scope::read_attrs(&e, "scope", &Scope::default()), s);

        // Older scenes: only "scope" (sections), everything else defaults.
        let old = el(r#"<Scene scope="256"/>"#);
        let read = Scope::read_attrs(&old, "scope", &Scope::default());
        assert_eq!(read, Scope::console(section::FADER));
        let snippet = Scope::read_attrs(&el(r#"<Snippet sections="99999"/>"#), "sections", &Scope::strips(0, vec![]));
        assert_eq!(snippet.sections, 99999 & section::ALL);
        assert!(!snippet.has_globals());
    }

    #[test]
    fn global_safe_round_trip() {
        let g = GlobalSafe { sections: section::EQ, dcas: 3, mute_groups: 1 << 7, fx: 1 << 20, console: true }.sanitised();
        assert_eq!(g.fx, 0);
        assert_eq!(GlobalSafe::from_xml(&g.to_xml()), g);
        assert!(GlobalSafe::from_xml(&el("<GlobalSafe/>")).is_empty());
    }

    #[test]
    fn merge_replaces_only_the_partial_parts() {
        let mut stored =
            el(r#"<State sections="1023"><Strip ref="ch:1" sections="1023" name="Kick"><Fdr lvl="-5"/><Eq on="1"/></Strip>
               <Strip ref="ch:2" sections="1023" name="Snare"><Fdr lvl="0"/></Strip>
               <Dcas><Dca i="0" name="Drums" lvl="0"/><Dca i="1" name="Vox" lvl="-3"/></Dcas>
               <Fx><Slot i="0" mdl="A"><P n="x" v="1"/></Slot><Slot i="1" mdl="B"/></Fx>
               <Monitor levelDb="0"/></State>"#);
        let partial = el(r#"<State sections="256"><Strip ref="ch:1" sections="256"><Fdr lvl="3"/></Strip>
               <Strip ref="ch:9" sections="256"><Fdr lvl="-1"/></Strip>
               <Dcas><Dca i="1" lvl="5"/></Dcas><Fx><Slot i="0" mdl="C"/></Fx><Monitor levelDb="-10"/></State>"#);
        merge_state(&mut stored, &partial);
        let strip = |r: &str| stored.children_named("Strip").find(|s| s.str_or("ref", "") == r).cloned().unwrap();
        let kick = strip("ch:1");
        assert_eq!(kick.str_or("name", ""), "Kick");
        assert_eq!(kick.child("Fdr").unwrap().str_or("lvl", ""), "3");
        assert_eq!(kick.child("Eq").unwrap().str_or("on", ""), "1");
        assert_eq!(strip("ch:2").child("Fdr").unwrap().str_or("lvl", ""), "0");
        assert_eq!(strip("ch:9").child("Fdr").unwrap().str_or("lvl", ""), "-1");
        let dcas = stored.child("Dcas").unwrap();
        let vox = dcas.children_named("Dca").find(|d| d.str_or("i", "") == "1").unwrap();
        assert_eq!((vox.str_or("name", ""), vox.str_or("lvl", "")), ("Vox", "5"));
        let fx = stored.child("Fx").unwrap();
        let slot0 = fx.children_named("Slot").find(|d| d.str_or("i", "") == "0").unwrap();
        assert_eq!(slot0.str_or("mdl", ""), "C");
        assert!(slot0.child("P").is_none());
        assert_eq!(fx.children_named("Slot").count(), 2);
        assert_eq!(stored.child("Monitor").unwrap().str_or("levelDb", ""), "-10");
    }

    #[test]
    fn differences_are_counted_per_value() {
        let a = el(
            r#"<Strip ref="ch:1" sections="8"><Gate on="0" mdl="GATE"><P n="thr" v="-40"/><P n="rng" v="60"/></Gate></Strip>"#,
        );
        let b = el(
            r#"<Strip ref="ch:1" sections="16"><Gate on="1" mdl="GATE"><P n="thr" v="-30"/><P n="rng" v="60"/></Gate></Strip>"#,
        );
        assert_eq!(count_differences(&a, &a), 0);
        assert_eq!(count_differences(&a, &b), 2);
        let c = el(r#"<Strip ref="ch:1"><Gate on="0" mdl="GATE"><P n="thr" v="-40"/></Gate><Extra x="1"/></Strip>"#);
        assert_eq!(count_differences(&a, &c), 2 + 1); // "rng" missing (2 attrs), Extra added
    }
}
