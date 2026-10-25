//! Writes the WING's factory values and descriptions from a parameter dump of
//! a factory-initialized console (`tools/wing/wing_compare.py dump`: its `wing.json`), as the
//! tables Sidecar answers from:
//!
//! cargo run -p livedaw-wing --example wing_factory -- wing.json values > data/wing_factory.tsv
//! cargo run -p livedaw-wing --example wing_factory -- wing.json desc > tests/data/wing_desc.tsv
//!
//! `values`: every static parameter of our tree the dump has, `path<TAB>value`
//! (floats as their exact value, everything else as the console's text).
//! `desc`: every parameter the dump has (the active models' too) with its
//! description. Paths with numbers fold into a pattern (`/ch/*/fdr`) with
//! the most common entry; the paths that differ from it are listed one by one.
//! Values that belong to the dumped console rather than to the factory (its
//! IP address, date and time) are left out.

use livedaw_wing::json::{self, Json};
use livedaw_wing::schema::{schema, NodeType};
use std::collections::BTreeMap;

fn pattern(path: &str) -> String {
    path.split('/')
        .map(|p| if !p.is_empty() && p.bytes().all(|c| c.is_ascii_digit()) { "*" } else { p })
        .collect::<Vec<_>>()
        .join("/")
}

fn console_specific(path: &str) -> bool {
    let tail = path.rsplit('/').next().unwrap_or("");
    let numbered = |prefix: &str| tail.strip_prefix(prefix).is_some_and(|n| n.len() == 1 && n.as_bytes()[0].is_ascii_digit());
    (path.starts_with("/$syscfg/") && (numbered("ip") || numbered("gw") || numbered("msk")))
        || matches!(path, "/$stat/date" | "/$stat/time" | "/$syscfg/$serial" | "/$syscfg/consolename")
}

/// Folds `entries` (path -> text): a pattern for the most common text of
/// each pattern, the paths that differ from it one by one.
fn fold(entries: &BTreeMap<String, String>) -> String {
    let mut by_pattern: BTreeMap<String, BTreeMap<&str, usize>> = BTreeMap::new();
    for (path, text) in entries {
        *by_pattern.entry(pattern(path)).or_default().entry(text.as_str()).or_default() += 1;
    }
    let mut common: BTreeMap<String, &str> = BTreeMap::new();
    let mut out = String::new();
    for (pat, texts) in &by_pattern {
        let (text, _) = texts.iter().max_by_key(|(t, n)| (**n, std::cmp::Reverse(**t))).unwrap();
        common.insert(pat.clone(), text);
        out.push_str(&format!("{pat}\t{text}\n"));
    }
    for (path, text) in entries {
        if common[&pattern(path)] != text.as_str() {
            out.push_str(&format!("{path}\t{text}\n"));
        }
    }
    out
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let [input, mode] = args.as_slice() else {
        eprintln!("usage: wing_factory <wing.json> values|desc");
        std::process::exit(2);
    };
    let text = std::fs::read_to_string(input).unwrap_or_else(|e| panic!("{input}: {e}"));
    let dump = json::parse(&text).unwrap_or_else(|e| panic!("{input}: {e}"));
    let leaves = dump.get("leaves").expect("no leaves in the dump");
    let mut entries = BTreeMap::new();
    match mode.as_str() {
        "values" => {
            let s = schema();
            for r in 0..s.num_rows() as u16 {
                let def = &s.defs[s.rows[r as usize].def as usize];
                if !def.is_leaf() {
                    continue;
                }
                let path = format!("/{}", s.row_parts(r).join("/"));
                if console_specific(&path) {
                    continue;
                }
                let Some(Json::Arr(v)) = leaves.get(&path).and_then(|l| l.get("value")) else { continue };
                let shown = v.first().and_then(Json::as_str).unwrap_or("").to_owned();
                let value = match (def.ty, v.get(2).and_then(Json::as_f64)) {
                    (NodeType::LinFloat | NodeType::LogFloat | NodeType::Fader, Some(x)) => json::fmt_num(x as f32 as f64),
                    _ => shown,
                };
                if value.contains(['\t', '\n']) {
                    continue;
                }
                entries.insert(path, value);
            }
        }
        "desc" => {
            for (path, leaf) in leaves.entries() {
                if let Some(d) = leaf.get("desc").and_then(Json::as_str) {
                    entries.insert(path.clone(), d.to_owned());
                }
            }
        }
        _ => {
            eprintln!("mode: values or desc");
            std::process::exit(2);
        }
    }
    print!("{}", fold(&entries));
}
