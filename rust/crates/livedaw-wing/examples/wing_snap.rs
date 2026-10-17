//! Imports a WING snapshot into a fresh (headless) console and reports what
//! was applied; optionally writes the console back out as a snapshot.
//!
//! cargo run -p livedaw-wing --example wing_snap -- <in.snap> [out.snap]
//! cargo run -p livedaw-wing --example wing_snap -- --export <out.snap>

use livedaw_engine::Engine;
use livedaw_wing::snap::{self, ImportOptions};
use livedaw_wing::WingState;
use std::path::Path;

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let engine = Engine::new(0);
    engine.set_num_channels(40);
    let state = WingState::default();
    let out = match args.as_slice() {
        [flag, out] if flag == "--export" => Some(out.clone()),
        [input, rest @ ..] => {
            match snap::import_file(&engine, &state, Path::new(input), &ImportOptions::default()) {
                Ok(report) => println!("{}", report.details()),
                Err(e) => {
                    eprintln!("{input}: {e}");
                    std::process::exit(1);
                }
            }
            rest.first().cloned()
        }
        [] => {
            eprintln!("usage: wing_snap <in.snap> [out.snap] | --export <out.snap>");
            std::process::exit(2);
        }
    };
    if let Some(out) = out {
        if let Err(e) = snap::export_file(&engine, &state, Path::new(&out)) {
            eprintln!("{e}");
            std::process::exit(1);
        }
        println!("wrote {out}");
    }
}
