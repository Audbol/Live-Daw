//! A headless Sidecar console answering the WING protocols, for testing
//! remote clients (libwing's wingprop / wingmon / wingdrive, Companion...)
//! without the app.
//!
//! cargo run -p livedaw-wing --example wing_emulator -- [--name NAME] [--osc PORT]
//!     [--native PORT] [--discovery PORT] [--bind IP] [--snap FILE]

use livedaw_engine::Engine;
use livedaw_wing::server::{Config, EngineHandle, WingServer};
use livedaw_wing::{snap, WingState};
use std::sync::Arc;
use std::time::Duration;

fn main() {
    let mut config = Config::default();
    let mut snap_file = None;
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        let mut next = || args.next().unwrap_or_default();
        match a.as_str() {
            "--name" => config.identity.name = next(),
            "--osc" => config.osc_port = next().parse().expect("port"),
            "--native" => config.native_port = next().parse().expect("port"),
            "--discovery" => config.discovery_port = next().parse().expect("port"),
            "--bind" => config.bind = next().parse().expect("IP address"),
            "--snap" => snap_file = Some(next()),
            other => {
                eprintln!("unknown argument {other}");
                std::process::exit(2);
            }
        }
    }

    let engine = Arc::new(Engine::new(0));
    engine.set_num_channels(40);
    let state = Arc::new(WingState::default());
    if let Some(path) = snap_file {
        match snap::import_file(&engine, &state, std::path::Path::new(&path), &snap::ImportOptions::default()) {
            Ok(report) => println!("{}", report.summary()),
            Err(e) => eprintln!("{path}: {e}"),
        }
    }
    let handle: Arc<dyn EngineHandle> = Arc::new(engine.clone());
    let server = WingServer::start(handle, state, &config);
    let st = server.status();
    println!("discovery {:?}, OSC {:?}, native {:?}", st.discovery, st.osc, st.native);
    for e in &st.errors {
        eprintln!("{e}");
    }
    loop {
        // The message thread's duties: build FX devices chosen remotely, free garbage.
        engine.collect();
        std::thread::sleep(Duration::from_millis(100));
    }
}
