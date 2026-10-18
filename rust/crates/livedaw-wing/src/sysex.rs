//! The WING's MIDI SYSEX CONTROL (`F0 00 20 32 57 <cmd> <data> F7`, "WING
//! Remote Protocols", chapter WING MIDI SYSEX) on the parameter tree: cmd 02
//! executes a node string, 03 dumps a node, 05 describes one. The MIDI side
//! (framing, the identity reply) is the application's.
//!
//! Node strings use the snapshot grammar: `/` starts at the root, `.` steps
//! down a level (`/ch.1.fdr=-1,mute=0,.2.fdr=0,mute=1`), a leading `.` goes
//! up one.

use crate::map::{Console, SetError};
use crate::osc;
use crate::schema::{schema, Node};
use crate::value::{self, Value};

/// The console's error texts.
pub const NODE_NOT_FOUND: &str = "NODE NOT FOUND";
pub const VALUE_ERROR: &str = "VALUE ERROR";
pub const NODE_IS_NOT_PAR: &str = "NODE IS NOT PAR";
pub const INCOMPLETE_DATA: &str = "INCOMPLETE DATA";

fn error_text(e: &SetError) -> &'static str {
    match e {
        SetError::NotFound => NODE_NOT_FOUND,
        SetError::IsNode => NODE_IS_NOT_PAR,
        SetError::ReadOnly | SetError::BadValue | SetError::Unsupported(_) => VALUE_ERROR,
    }
}

/// cmd 02: applies a node string. Every element is tried; the last error wins.
pub fn execute(c: &Console, text: &str) -> Result<(), &'static str> {
    let s = schema();
    let mut cur = Node::Root;
    let mut result = Ok(());
    for raw in text.split(',') {
        let mut el = raw.trim();
        if el.is_empty() {
            continue;
        }
        if let Some(rest) = el.strip_prefix('/') {
            cur = Node::Root;
            el = rest;
        }
        while let Some(rest) = el.strip_prefix('.') {
            cur = s.parent(cur);
            el = rest;
        }
        let Some((path, val)) = el.split_once('=') else {
            result = Err(INCOMPLETE_DATA);
            continue;
        };
        let mut parts: Vec<&str> = path.split('.').filter(|p| !p.is_empty()).collect();
        let Some(leaf) = parts.pop() else {
            result = Err(NODE_NOT_FOUND);
            continue;
        };
        let mut found = true;
        for p in parts {
            match s.child(cur, p, c) {
                Some(n) => cur = n,
                None => {
                    found = false;
                    break;
                }
            }
        }
        let Some(node) = found.then(|| s.child(cur, leaf, c)).flatten() else {
            result = Err(NODE_NOT_FOUND);
            continue;
        };
        let Some(def) = s.def(node, c) else {
            result = Err(NODE_NOT_FOUND);
            continue;
        };
        if !def.is_leaf() {
            result = Err(NODE_IS_NOT_PAR);
            continue;
        }
        let v = value::parse_display(def, val).unwrap_or_else(|| Value::str(val.trim().trim_matches('\'')));
        if let Err(e) = c.set(node, &v) {
            result = Err(error_text(&e));
        }
    }
    result
}

/// A node from a sysex path: `/ch.1.fdr`, `/ch/1/fdr` or `/aux`.
fn resolve(c: &Console, text: &str) -> Option<Node> {
    let path = text.trim().trim_end_matches('\0').trim().replace('.', "/");
    let path = if path.starts_with('/') { path } else { format!("/{path}") };
    if path == "/" {
        return Some(Node::Root);
    }
    schema().resolve(path.trim_end_matches('/'), c)
}

/// cmd 03: a leaf's value, or a node's values as a node string.
pub fn dump(c: &Console, text: &str) -> Result<String, &'static str> {
    let s = schema();
    let node = resolve(c, text).ok_or(NODE_NOT_FOUND)?;
    let def = s.def(node, c).ok_or(NODE_NOT_FOUND)?;
    if def.is_leaf() {
        let v = c.get(node).ok_or(NODE_NOT_FOUND)?;
        return Ok(value::display(def, &v));
    }
    Ok(osc::dump(c, node))
}

/// cmd 05: the node's description (its children, or the leaf's range).
pub fn describe(c: &Console, text: &str) -> Result<String, &'static str> {
    let node = resolve(c, text).ok_or(NODE_NOT_FOUND)?;
    let mut out = osc::describe(c, node, false);
    out.push('\n');
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::map::WingState;
    use livedaw_engine::Engine;

    #[test]
    fn execute_dump_and_describe() {
        let engine = Engine::new(0);
        engine.set_num_channels(4);
        let state = WingState::default();
        let c = Console::new(&engine, &state);
        assert_eq!(execute(&c, "/ch.1.fdr=-1,mute=1,.2.fdr=0,mute=0"), Ok(()));
        assert_eq!(dump(&c, "/ch.1.fdr").as_deref(), Ok("-1.0"));
        assert_eq!(dump(&c, "/ch/1/mute").as_deref(), Ok("1"));
        assert_eq!(dump(&c, "/ch.2.fdr").as_deref(), Ok("0.0"));
        assert_eq!(execute(&c, "/ch.1.fd=-1"), Err(NODE_NOT_FOUND));
        assert_eq!(execute(&c, "/ch.1.fdr"), Err(INCOMPLETE_DATA));
        assert_eq!(execute(&c, "/ch.1=3"), Err(NODE_IS_NOT_PAR));
        assert_eq!(dump(&c, "/nothing"), Err(NODE_NOT_FOUND));
        let aux = dump(&c, "/aux.1").unwrap();
        assert!(aux.contains("fdr=") && aux.contains("mute="), "{aux}");
        let d = describe(&c, "/ch.1.fdr").unwrap();
        assert!(d.contains("fader"), "{d}");
        assert!(describe(&c, "/aux").unwrap().contains(" 1 "));
    }
}
