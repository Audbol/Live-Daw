//! Custom controls: the assignments of the WING's CUSTOM CONTROLS section
//! (CC EDIT), saved with the show.
//!
//! Each layer has four controls; a control is an encoder with two keys under
//! it (`enc`, `bu`, `bd` in the console's parameter tree), a name and a colour.
//! Layers 0-15 are the upper part's 16 layers; the console's other layers
//! (the lower part's USER layer, the 4-Channel section's U1-U3) follow. What a
//! function does is the application's business: here it is a category, a
//! target and an option, stored as `category,target,option`.

use crate::xml::{parse_int_prefix, Element};

/// The upper part's layers (UP / DOWN), then USER and U1-U3.
pub const NUM_LAYERS: usize = 20;
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

impl Default for CustomControls {
    fn default() -> Self {
        Self { layers: vec![Default::default(); NUM_LAYERS] }
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

    pub fn reset(&mut self) {
        *self = Self::default();
    }

    /// `<CustomControls>` with a `<Control>` per assigned or named control.
    pub fn to_xml(&self) -> Element {
        let mut xml = Element::new("CustomControls");
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

    pub fn from_xml(&mut self, xml: &Element) {
        self.reset();
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
    }
}

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
}
