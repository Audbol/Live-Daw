//! Custom controls: the assignments of the WING's CUSTOM CONTROLS section
//! (CC EDIT), saved with the show.
//!
//! Each layer has four controls; a control is an encoder with two keys under
//! it (`enc`, `bu`, `bd` in the console's parameter tree), a name and a colour.
//! Layers 0-15 are the upper part's 16 layers; the console's other layers
//! (the lower part's USER layer, three legacy layers, the lower part's DAW
//! 1-4 layers of DAW mode, then the WING COMPACT's key sets) follow. The
//! 4-Channel section's F1-F3 play layers 1-3 (WING manual 5.5 p. 48: "USE
//! F1-F3 AS CUSTOM CONTROLS mirrors the Custom Control layers 1-3"); shows
//! saved before had them on layers 17-19 of their own, which loading moves to
//! layers 1-3 (see [`CustomControls::from_xml`]). What a function does is the
//! application's business: here it is a category, a target and an option,
//! stored as `category,target,option`.

use crate::xml::{parse_int_prefix, Element};

/// The upper part's layers (UP / DOWN), then USER, the three legacy F1-F3
/// layers, DAW 1-4 and the COMPACT's nine key sets.
pub const NUM_LAYERS: usize = 33;
/// The lower part's USER layer (`/$ctl/user/user`).
pub const USER_LAYER: usize = 16;
/// Where shows saved before layout 2 kept the 4-Channel section's F1-F3
/// layers (an older guess; nothing plays them now).
pub const LEGACY_FOUR_CHANNEL_LAYER: usize = 17;
/// The first of the four DAW layers (the lower part in DAW mode: `daw1`-`daw4`).
pub const FIRST_DAW_LAYER: usize = 20;
/// The WING COMPACT's key sets (`/$ctl/user/U1`-`U4`, `MM`, `D1`-`D4`; WING
/// Edit's USER 1-4 .. USER 13-16, MAIN/MTX 5-8, DCA 1-4 .. DCA 13-16): four
/// keys each (an upper key, a name and a colour per control, no encoder),
/// which its 16 keys play. USER 1-4 .. 13-16 are the keys in USER mode, four
/// sets for 16 keys; MAIN/MTX 5-8 the four keys MAIN MATRIX mode leaves free
/// between MAIN 1-4 and MATRIX 1-8; DCA 1-4 .. 13-16 the keys in DCA mode
/// with DCA AS CC (`/$ctl/cfg/dcacc`) on.
pub const FIRST_COMPACT_USER_LAYER: usize = 24;
pub const COMPACT_MAIN_MATRIX_LAYER: usize = 28;
pub const FIRST_COMPACT_DCA_LAYER: usize = 29;
/// How many layers the COMPACT's USER and DCA sets have each.
pub const NUM_COMPACT_SETS: usize = 4;
pub const NUM_CONTROLS: usize = 4;

/// What a knob or key does (category 0 = not assigned).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CcFunction {
    pub category: i32,
    pub target: i32,
    pub option: i32,
}

impl CcFunction {
    pub fn is_assigned(&self) -> bool {
        self.category != 0
    }

    fn to_attr(self) -> String {
        format!("{},{},{}", self.category, self.target, self.option)
    }

    fn parse(s: &str) -> Self {
        let mut parts = s.split(',').map(|p| parse_int_prefix(p.trim()));
        CcFunction { category: parts.next().unwrap_or(0), target: parts.next().unwrap_or(0), option: parts.next().unwrap_or(0) }
    }
}

/// One encoder with its upper and lower key.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CcControl {
    pub encoder: CcFunction,
    pub upper: CcFunction,
    pub lower: CcFunction,
    pub name: String,
    /// The WING colour (1-18), 0 = none.
    pub colour: i32,
}

impl CcControl {
    pub fn is_default(&self) -> bool {
        *self == CcControl::default()
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct CustomControls {
    layers: Vec<[CcControl; NUM_CONTROLS]>,
}

/// Key category 13, DAW BUTTONS: the target is the command's group and the
/// option the command, both in the order of the WING's DAW BUTTONS list
/// (the application's `DawRemote::commands()`).
pub const DAW_BUTTONS_CATEGORY: i32 = 13;

/// The DAW BUTTONS command list's groups: (code prefix, number of commands),
/// in the list's order (TRANSPORT T1-T20, NAVIGATION N1-N9, ...).
const DAW_COMMAND_GROUPS: [(&str, i32); 10] =
    [("T", 20), ("N", 9), ("A", 16), ("F", 8), ("V", 15), ("AU", 12), ("SY", 10), ("OT", 12), ("E", 10), ("SP", 6)];

/// The group (TRANSPORT 0, NAVIGATION 1, ...) of DAW BUTTONS command `option`.
pub fn daw_command_group(option: i32) -> i32 {
    let mut first = 0;
    for (group, (_, count)) in DAW_COMMAND_GROUPS.iter().enumerate() {
        first += count;
        if option < first {
            return group as i32;
        }
    }
    DAW_COMMAND_GROUPS.len() as i32 - 1
}

/// The DAW BUTTONS key of command `code` ("T1", "AU4", ...).
fn daw_key(code: &str) -> CcFunction {
    let mut first = 0;
    for (group, (prefix, count)) in DAW_COMMAND_GROUPS.iter().enumerate() {
        if let Some(n) = code.strip_prefix(prefix).and_then(|n| n.parse::<i32>().ok()).filter(|n| (1..=*count).contains(n)) {
            return CcFunction { category: DAW_BUTTONS_CATEGORY, target: group as i32, option: first + n - 1 };
        }
        first += count;
    }
    CcFunction::default()
}

/// The DAW layers DAW 1-4 of a factory-initialized WING (`/$ctl/user/daw1-4`):
/// per control the upper key's command, the lower key's and the name.
const FACTORY_DAW_LAYERS: [[(&str, &str, &str); NUM_CONTROLS]; 4] = [
    [("T1", "T4", "STOP"), ("T2", "T5", "PLAY"), ("T8", "OT4", "CYCLE"), ("T3", "SY6", "RECORD")],
    [("T1", "T4", "STOP"), ("T2", "T5", "PLAY"), ("", "", ""), ("T3", "T6", "RECORD")],
    [("T1", "T4", "STOP"), ("T2", "T5", "PLAY"), ("AU4", "AU1", " |TOUCH"), ("AU5", "AU2", " |LATCH")],
    [("OT3", "OT1", "NAME/VALUE"), ("", "SP2", "[V-POTS]"), ("", "SP3", "[RECRDY]"), ("N5", "T11", "ZOOM")],
];

/// The factory assignment of control `index` of `layer`: the DAW layers'
/// keys; every other control is not assigned.
pub fn factory_control(layer: usize, index: usize) -> CcControl {
    let Some(daw) = layer.checked_sub(FIRST_DAW_LAYER).and_then(|l| FACTORY_DAW_LAYERS.get(l)) else {
        return CcControl::default();
    };
    let (upper, lower, name) = daw[index.min(NUM_CONTROLS - 1)];
    CcControl { upper: daw_key(upper), lower: daw_key(lower), name: name.to_owned(), ..CcControl::default() }
}

/// `<CustomControls factory="...">`: the defaults the controls left out had.
/// Shows without it were saved when every control, the DAW layers' too,
/// started not assigned.
const FACTORY_ATTR: &str = "factory";
const FACTORY_WING: &str = "wing";
/// `<CustomControls layout="2">`: F1-F3 play layers 1-3. Without it the
/// legacy layers 17-19 are F1-F3's and move to layers 1-3 on loading.
const LAYOUT_ATTR: &str = "layout";
const LAYOUT: i32 = 2;

impl Default for CustomControls {
    /// A factory-initialized WING's: the DAW layers assigned, nothing else.
    fn default() -> Self {
        Self { layers: (0..NUM_LAYERS).map(|l| std::array::from_fn(|i| factory_control(l, i))).collect() }
    }
}

impl CustomControls {
    pub fn control(&self, layer: usize, index: usize) -> Option<&CcControl> {
        self.layers.get(layer).and_then(|l| l.get(index))
    }

    pub fn control_mut(&mut self, layer: usize, index: usize) -> Option<&mut CcControl> {
        self.layers.get_mut(layer).and_then(|l| l.get_mut(index))
    }

    /// CLR ALL: every control of a layer back to not assigned.
    pub fn clear_layer(&mut self, layer: usize) {
        if let Some(l) = self.layers.get_mut(layer) {
            *l = Default::default();
        }
    }

    /// Back to a factory-initialized WING's controls.
    pub fn reset(&mut self) {
        *self = Self::default();
    }

    /// What a show saved before the factory DAW layers starts with: nothing assigned.
    pub fn legacy() -> Self {
        Self { layers: vec![Default::default(); NUM_LAYERS] }
    }

    /// `<CustomControls>` with a `<Control>` per control that differs from its
    /// factory assignment.
    pub fn to_xml(&self) -> Element {
        let mut xml = Element::new("CustomControls");
        xml.set(FACTORY_ATTR, FACTORY_WING).set(LAYOUT_ATTR, LAYOUT);
        for (l, layer) in self.layers.iter().enumerate() {
            for (i, c) in layer.iter().enumerate() {
                if *c == factory_control(l, i) {
                    continue;
                }
                xml.add_new("Control")
                    .set("layer", l)
                    .set("index", i)
                    .set("name", &c.name)
                    .set("colour", c.colour)
                    .set("enc", c.encoder.to_attr())
                    .set("bu", c.upper.to_attr())
                    .set("bd", c.lower.to_attr());
            }
        }
        xml
    }

    /// `<CustomControls layout="2">` with a `<Control>` per control that has
    /// anything assigned or set, the factory DAW layers' too (no `factory`
    /// attribute: what is left out is not assigned). The application's cache
    /// reads this, and writes it back the same way.
    pub fn to_full_xml(&self) -> Element {
        let mut xml = Element::new("CustomControls");
        xml.set(LAYOUT_ATTR, LAYOUT);
        for (l, layer) in self.layers.iter().enumerate() {
            for (i, c) in layer.iter().enumerate() {
                if c.is_default() {
                    continue;
                }
                xml.add_new("Control")
                    .set("layer", l)
                    .set("index", i)
                    .set("name", &c.name)
                    .set("colour", c.colour)
                    .set("enc", c.encoder.to_attr())
                    .set("bu", c.upper.to_attr())
                    .set("bd", c.lower.to_attr());
            }
        }
        xml
    }

    /// Shows saved before layout 2: the 4-Channel section's F1-F3 layers
    /// (legacy layers 17-19: their encoders and upper keys, which F1-F3
    /// played) move to layers 1-3, control by control, where those controls
    /// are not assigned; a control of layers 1-3 already in use keeps its
    /// assignment and the F1-F3 control stays where it was.
    fn move_legacy_four_channel_layers(&mut self) {
        for f in 0..3 {
            for i in 0..NUM_CONTROLS {
                let old = self.layers[LEGACY_FOUR_CHANNEL_LAYER + f][i].clone();
                if old.is_default() || !self.layers[f][i].is_default() {
                    continue;
                }
                self.layers[f][i] = old;
                self.layers[LEGACY_FOUR_CHANNEL_LAYER + f][i] = CcControl::default();
            }
        }
    }

    pub fn from_xml(&mut self, xml: &Element) {
        *self = if xml.attr(FACTORY_ATTR) == Some(FACTORY_WING) { Self::default() } else { Self::legacy() };
        for e in xml.children_named("Control") {
            let (Ok(layer), Ok(index)) = (usize::try_from(e.int_or("layer", -1)), usize::try_from(e.int_or("index", -1))) else {
                continue;
            };
            if let Some(c) = self.control_mut(layer, index) {
                *c = CcControl {
                    encoder: CcFunction::parse(e.str_or("enc", "")),
                    upper: CcFunction::parse(e.str_or("bu", "")),
                    lower: CcFunction::parse(e.str_or("bd", "")),
                    name: e.str_or("name", "").to_owned(),
                    colour: e.int_or("colour", 0).clamp(0, 18),
                };
            }
        }
        if xml.int_or(LAYOUT_ATTR, 1) < LAYOUT {
            self.move_legacy_four_channel_layers();
        }
    }
}

/// Button category 12, FADER SPILL: the target is the spill, kind * 100 +
/// index (kinds as the layer set's spills: 0 DCA, 1 the sources of bus slot
/// `index`, 2 FX slot, 3 automix group X / Y, 4 the selected strip's sends);
/// lit while a fader section spills it.
pub const FADER_SPILL_CATEGORY: i32 = 12;

/// Button category 15, USB PLAY/REC (the USB recorder / player): the option
/// is the key, in this order.
pub const USB_PLAY_REC_CATEGORY: i32 = 15;
pub const USB_PLAY_REC_FUNCTIONS: [&str; 10] =
    ["PLAY", "PAUSE", "STOP", "PREV", "NEXT", "REC", "REC PAUSE", "REC STOP", "NEW FILE", "REPEAT"];

/// Button category 18, CC BANK (WING 3.0.6: a CC key switches the custom
/// controls bank): the option is the upper part's layer to show, 0-15 (lit
/// while it is shown). Categories are append-only: 16 SD RECORDER and 17
/// GPIO come before it.
pub const CC_BANK_CATEGORY: i32 = 18;

/// Button category 11, OTHER: the option is the function, in these codes
/// (append-only; CC EDIT lists them in WING Edit's order).
pub const OTHER_CATEGORY: i32 = 11;
pub mod other {
    pub const MONITOR_DIM: i32 = 0;
    pub const MONITOR_MONO: i32 = 1;
    pub const MONITOR_SWAP: i32 = 2;
    pub const MONITOR_MUTE: i32 = 3;
    pub const FADER_OFF: i32 = 4;
    pub const FADER_MINUS_10: i32 = 5;
    pub const FADER_0: i32 = 6;
    pub const AUTOMIX_X: i32 = 7;
    pub const AUTOMIX_Y: i32 = 8;
    pub const CHANNEL_PREV: i32 = 9;
    pub const CHANNEL_NEXT: i32 = 10;
    pub const CC_BANK_UP: i32 = 11;
    pub const CC_BANK_DOWN: i32 = 12;
    pub const SOF_OFF: i32 = 13;
    pub const TALKBACK_A: i32 = 14;
    pub const TALKBACK_B: i32 = 15;
    pub const GLOBAL_MAIN_ALT: i32 = 16;
    pub const DAW_LAYER: i32 = 17;
    pub const MONITOR_PHONES: i32 = 18;
    pub const MONITOR_SPEAKERS: i32 = 19;
    pub const MONITOR_PH_SPK: i32 = 20;
    /// CH, BUS, MAIN, MTX SOLO TAP: the strip type's solo PFL / AFL.
    pub const CH_SOLO_TAP: i32 = 21;
    pub const MTX_SOLO_TAP: i32 = 24;
    /// CH, BUS, MAIN, MTX, DCA METER TAP: the strip type's meters PRE / POST.
    pub const CH_METER_TAP: i32 = 25;
    pub const DCA_METER_TAP: i32 = 29;
    pub const COUNT: i32 = 30;
}

/// Button categories 19, 20: CH INSERT PRE / POST ON/OFF (WING `INS1`,
/// `INS2`): the target is the strip (as MUTE's).
pub const INSERT_PRE_CATEGORY: i32 = 19;
pub const INSERT_POST_CATEGORY: i32 = 20;
/// Button category 21, FX PAR (`FXPAR`): the target is the FX slot (0-15),
/// the option the parameter (-1: the slot's MIX). A press toggles or steps
/// the parameter, or taps the tempo of a delay's TIME; held, the encoder
/// above the key sets it (WING manual 4.8 p. 25).
pub const FX_PARAMETER_CATEGORY: i32 = 21;
/// Button category 22, OVERVIEW PAGE (`VIEWPAGE`): the target is the area
/// (0-6: L, C, R, CMPCT, RCK, EXT, VRT), the option the bank (0-20, the
/// WING's 1-21: the section's layer keys 1-7, then its fixed banks).
pub const OVERVIEW_PAGE_CATEGORY: i32 = 22;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn assignments_round_trip() {
        let mut cc = CustomControls::default();
        {
            let c = cc.control_mut(3, 2).unwrap();
            c.name = "*MainPA Fader".into();
            c.colour = 5;
            c.encoder = CcFunction { category: 1, target: 300_000, option: 0 };
            c.lower = CcFunction { category: 11, target: -1, option: 2 };
        }
        cc.control_mut(16, 0).unwrap().upper = CcFunction { category: 2, target: 100_001, option: 0 };
        assert!(cc.control(NUM_LAYERS, 0).is_none());
        assert!(cc.control_mut(0, NUM_CONTROLS).is_none());

        let mut loaded = CustomControls::default();
        loaded.from_xml(&cc.to_xml());
        assert_eq!(loaded, cc);
        assert_eq!(loaded.control(3, 2).unwrap().lower.option, 2);
        assert!(!loaded.control(3, 1).unwrap().encoder.is_assigned());

        loaded.clear_layer(3);
        assert!(loaded.control(3, 2).unwrap().is_default());
        assert_eq!(cc.to_xml().children_named("Control").count(), 2);
    }

    #[test]
    fn the_daw_layers_start_as_a_factory_set_wing() {
        // DAW 1: STOP / REWIND, PLAY / FFWD, CYCLE / TIME/BEATS, RECORD / UNDO.
        let cc = CustomControls::default();
        let stop = cc.control(FIRST_DAW_LAYER, 0).unwrap();
        assert_eq!(stop.name, "STOP");
        assert_eq!(stop.upper, CcFunction { category: DAW_BUTTONS_CATEGORY, target: 0, option: 0 });
        assert_eq!(stop.lower, CcFunction { category: DAW_BUTTONS_CATEGORY, target: 0, option: 3 });
        let record = cc.control(FIRST_DAW_LAYER, 3).unwrap();
        assert_eq!(record.lower, CcFunction { category: DAW_BUTTONS_CATEGORY, target: 6, option: 85 }); // SY6 UNDO
        let vpots = cc.control(FIRST_DAW_LAYER + 3, 1).unwrap();
        assert!(!vpots.upper.is_assigned());
        assert_eq!(vpots.lower, CcFunction { category: DAW_BUTTONS_CATEGORY, target: 9, option: 113 }); // SP2
        assert!(cc.control(FIRST_DAW_LAYER + 1, 2).unwrap().is_default());
        assert!(cc.control(0, 0).unwrap().is_default() && cc.control(16, 0).unwrap().is_default());

        // A factory set saves nothing; a cleared DAW key is saved.
        assert_eq!(cc.to_xml().children_named("Control").count(), 0);
        let mut changed = cc.clone();
        changed.clear_layer(FIRST_DAW_LAYER);
        let mut loaded = CustomControls::legacy();
        loaded.from_xml(&changed.to_xml());
        assert_eq!(loaded, changed);
        // Shows saved before the factory DAW layers keep theirs empty.
        loaded.from_xml(&Element::new("CustomControls"));
        assert_eq!(loaded, CustomControls::legacy());
    }

    #[test]
    fn daw_layers_round_trip() {
        // A DAW BUTTONS key (category 13, command 1) and a MIDI key on DAW 4.
        let mut cc = CustomControls::default();
        cc.control_mut(FIRST_DAW_LAYER, 0).unwrap().upper = CcFunction { category: 13, target: 0, option: 1 };
        {
            let c = cc.control_mut(FIRST_DAW_LAYER + 3, 3).unwrap();
            c.lower = CcFunction { category: 14, target: 2 * 16 + 9, option: 60 * 128 + 127 };
            c.name = "PLAY".into();
        }
        assert!(cc.control(NUM_LAYERS - 1, 3).is_some());
        let mut loaded = CustomControls::default();
        loaded.from_xml(&cc.to_xml());
        assert_eq!(loaded, cc);
        assert_eq!(loaded.control(FIRST_DAW_LAYER + 3, 3).unwrap().lower.option, 60 * 128 + 127);

        // A show from before the DAW layers loads, and keeps its own layers
        // (its F3 layer's key moves to layer 3: see the F1-F3 test).
        let mut old = Element::new("CustomControls");
        old.add_new("Control").set("layer", 19).set("index", 1).set("bu", "2,100001,0");
        loaded.from_xml(&old);
        assert_eq!(loaded.control(2, 1).unwrap().upper.category, 2);
        assert!(loaded.control(19, 1).unwrap().is_default());
        assert!(loaded.control(FIRST_DAW_LAYER, 0).unwrap().is_default());
    }

    #[test]
    fn bad_input_is_ignored() {
        let mut xml = Element::new("CustomControls");
        xml.add_new("Control").set("layer", 99).set("index", 0).set("name", "x");
        xml.add_new("Control").set("layer", 1).set("index", 1).set("enc", "4").set("colour", 40);
        let mut cc = CustomControls::default();
        cc.from_xml(&xml);
        let c = cc.control(1, 1).unwrap();
        assert_eq!(c.encoder, CcFunction { category: 4, target: 0, option: 0 });
        assert_eq!(c.colour, 18);
    }

    #[test]
    fn usb_play_rec_keys_round_trip() {
        let mut cc = CustomControls::default();
        let rec = USB_PLAY_REC_FUNCTIONS.iter().position(|&f| f == "REC").unwrap() as i32;
        cc.control_mut(16, 3).unwrap().lower = CcFunction { category: USB_PLAY_REC_CATEGORY, target: 0, option: rec };
        let mut loaded = CustomControls::default();
        loaded.from_xml(&cc.to_xml());
        assert_eq!(loaded.control(16, 3).unwrap().lower, CcFunction { category: 15, target: 0, option: 5 });
    }

    #[test]
    fn cc_bank_keys_round_trip() {
        let mut cc = CustomControls::default();
        cc.control_mut(2, 1).unwrap().upper = CcFunction { category: CC_BANK_CATEGORY, target: 0, option: 7 };
        let mut loaded = CustomControls::default();
        loaded.from_xml(&cc.to_xml());
        assert_eq!(loaded.control(2, 1).unwrap().upper, CcFunction { category: 18, target: 0, option: 7 });
    }

    #[test]
    fn fader_spill_keys_round_trip() {
        let mut cc = CustomControls::default();
        // DCA 3 and FX slot 12.
        cc.control_mut(0, 0).unwrap().upper = CcFunction { category: FADER_SPILL_CATEGORY, target: 2, option: 0 };
        cc.control_mut(16, 2).unwrap().lower = CcFunction { category: FADER_SPILL_CATEGORY, target: 211, option: 0 };
        let mut loaded = CustomControls::default();
        loaded.from_xml(&cc.to_xml());
        assert_eq!(loaded.control(0, 0).unwrap().upper, CcFunction { category: 12, target: 2, option: 0 });
        assert_eq!(loaded.control(16, 2).unwrap().lower, CcFunction { category: 12, target: 211, option: 0 });
    }

    #[test]
    fn compact_key_sets_round_trip() {
        // USER 13-16's last key, MAIN/MTX 5-8's first and DCA 1-4's.
        let mut cc = CustomControls::default();
        let last_user = FIRST_COMPACT_USER_LAYER + NUM_COMPACT_SETS - 1;
        assert_eq!((last_user + 1, COMPACT_MAIN_MATRIX_LAYER + 1), (COMPACT_MAIN_MATRIX_LAYER, FIRST_COMPACT_DCA_LAYER));
        assert_eq!(FIRST_COMPACT_DCA_LAYER + NUM_COMPACT_SETS, NUM_LAYERS);
        {
            let c = cc.control_mut(last_user, 3).unwrap();
            c.upper = CcFunction { category: 10, target: 4, option: 0 };
            c.name = "SCENE 4".into();
            c.colour = 7;
        }
        cc.control_mut(COMPACT_MAIN_MATRIX_LAYER, 0).unwrap().upper = CcFunction { category: 1, target: 100_000, option: 0 };
        cc.control_mut(FIRST_COMPACT_DCA_LAYER, 0).unwrap().upper = CcFunction { category: 3, target: 2, option: 0 };
        assert!(cc.control(FIRST_COMPACT_DCA_LAYER + NUM_COMPACT_SETS - 1, 3).unwrap().is_default());
        let mut loaded = CustomControls::default();
        loaded.from_xml(&cc.to_xml());
        assert_eq!(loaded, cc);
        assert_eq!(loaded.control(last_user, 3).unwrap().name, "SCENE 4");
        assert_eq!(cc.to_xml().children_named("Control").count(), 3);
    }

    #[test]
    fn f1_f3_of_older_shows_move_to_layers_1_to_3() {
        // An older show: F1 and F3 assigned on the legacy layers 17 and 19;
        // layer 1's first control is in use, its second free.
        let mut old = Element::new("CustomControls");
        old.set(FACTORY_ATTR, FACTORY_WING);
        old.add_new("Control").set("layer", 0).set("index", 0).set("name", "Mine").set("bu", "1,100000,0");
        old.add_new("Control").set("layer", 17).set("index", 0).set("name", "F1 A").set("enc", "1,100003,0");
        old.add_new("Control").set("layer", 17).set("index", 1).set("name", "F1 B").set("bu", "2,100001,0").set("colour", 4);
        old.add_new("Control").set("layer", 19).set("index", 3).set("enc", "7,0,1");
        let mut cc = CustomControls::default();
        cc.from_xml(&old);
        // Layer 1's own control stays; F1's first control stays on layer 17.
        assert_eq!(cc.control(0, 0).unwrap().name, "Mine");
        assert_eq!(cc.control(LEGACY_FOUR_CHANNEL_LAYER, 0).unwrap().name, "F1 A");
        let moved = cc.control(0, 1).unwrap();
        assert_eq!((moved.name.as_str(), moved.colour, moved.upper.category), ("F1 B", 4, 2));
        assert!(cc.control(LEGACY_FOUR_CHANNEL_LAYER, 1).unwrap().is_default());
        assert_eq!(cc.control(2, 3).unwrap().encoder, CcFunction { category: 7, target: 0, option: 1 });

        // Saved again (layout 2) it loads as it is: nothing moves twice.
        let mut again = CustomControls::default();
        again.from_xml(&cc.to_xml());
        assert_eq!(again, cc);
        let mut cache = CustomControls::default();
        cache.from_xml(&cc.to_full_xml());
        assert_eq!(cache, cc);
    }

    #[test]
    fn the_full_listing_keeps_the_factory_daw_layers() {
        // What the application's cache reads: every assigned control, the
        // factory DAW layers' too, and nothing that isn't.
        let mut cc = CustomControls::default();
        cc.control_mut(0, 2).unwrap().upper = CcFunction { category: OTHER_CATEGORY, target: 0, option: other::DAW_LAYER };
        cc.control_mut(1, 0).unwrap().upper = CcFunction { category: FX_PARAMETER_CATEGORY, target: 3, option: -1 };
        let full = cc.to_full_xml();
        assert_eq!(full.attr(FACTORY_ATTR), None);
        let daw = (0..4).map(|l| (0..NUM_CONTROLS).filter(|&i| !factory_control(FIRST_DAW_LAYER + l, i).is_default()).count());
        assert_eq!(full.children_named("Control").count(), 2 + daw.sum::<usize>());
        let mut loaded = CustomControls::legacy();
        loaded.from_xml(&full);
        assert_eq!(loaded, cc);
        assert_eq!(loaded.control(1, 0).unwrap().upper.option, -1);
        assert_eq!(loaded.control(FIRST_DAW_LAYER, 0).unwrap().name, "STOP");
    }
}
