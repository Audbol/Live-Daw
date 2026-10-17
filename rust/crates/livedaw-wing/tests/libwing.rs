//! Runs libwing's own clients against the remote servers. They are not a
//! dependency of this repository: build them with
//! `tools/wing/libwing_tools.sh DIR` and point `LIBWING_TOOLS` at the
//! directory holding the binaries. Without it these tests pass trivially
//! (and say so on stderr). LIBWING_CAPTURE_MODELS=1 adds wingdrive's
//! model capture (several minutes).

use livedaw_engine::Engine;
use livedaw_wing::server::{Config, EngineHandle, WingServer};
use livedaw_wing::WingState;
use std::net::{IpAddr, Ipv4Addr};
use std::path::PathBuf;
use std::process::Command;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;

struct Desk {
    server: WingServer,
    stop: Arc<AtomicBool>,
    collector: Option<std::thread::JoinHandle<()>>,
}

impl Desk {
    fn start() -> Self {
        let engine = Arc::new(Engine::new(0));
        engine.set_num_channels(40);
        let config =
            Config { bind: IpAddr::V4(Ipv4Addr::LOCALHOST), discovery_port: 0, osc_port: 0, native_port: 0, ..Config::default() };
        let handle: Arc<dyn EngineHandle> = Arc::new(engine.clone());
        let server = WingServer::start(handle, Arc::new(WingState::default()), &config);
        assert!(server.status().errors.is_empty(), "{:?}", server.status().errors);
        // The app's message thread: builds FX devices picked remotely.
        let stop = Arc::new(AtomicBool::new(false));
        let flag = stop.clone();
        let collector = std::thread::spawn(move || {
            while !flag.load(Ordering::Relaxed) {
                engine.collect();
                std::thread::sleep(Duration::from_millis(20));
            }
        });
        Self { server, stop, collector: Some(collector) }
    }
}

impl Drop for Desk {
    fn drop(&mut self) {
        self.server.stop();
        self.stop.store(true, Ordering::Relaxed);
        if let Some(t) = self.collector.take() {
            let _ = t.join();
        }
    }
}

fn tool(name: &str) -> Option<PathBuf> {
    let dir = std::env::var_os("LIBWING_TOOLS")?;
    let path = PathBuf::from(dir).join(name);
    assert!(path.exists(), "LIBWING_TOOLS has no {name}");
    Some(path)
}

fn run(cmd: &mut Command) -> String {
    let out = cmd.output().expect("run libwing tool");
    let text = format!("{}{}", String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr));
    assert!(out.status.success(), "{cmd:?} failed:\n{text}");
    text
}

#[test]
fn wingdrive_battery_and_model_capture() {
    let Some(wingdrive) = tool("wingdrive") else {
        eprintln!("LIBWING_TOOLS not set; skipping wingdrive");
        return;
    };
    let desk = Desk::start();
    let target = desk.server.status().native.unwrap().to_string();
    let text = run(Command::new(&wingdrive).args(["--target", &target, "--allow-state-changing", "--gets", "256"]));
    assert!(text.contains("battery complete"), "{text}");
    assert!(text.contains("defs: 9/9 resolved"), "{text}");
    assert!(text.contains("writes: 3 target(s) applied and restored"), "{text}");
    assert!(text.contains("meters: 2/2 frame(s) received"), "{text}");

    // Every model of every processing slot, selected and read back, then the
    // original models restored. wingdrive waits 400 ms after every selection,
    // so this takes minutes: opt in with LIBWING_CAPTURE_MODELS=1.
    if std::env::var_os("LIBWING_CAPTURE_MODELS").is_none() {
        return;
    }
    let out = std::env::temp_dir().join(format!("livedaw-wing-models-{}.json", std::process::id()));
    let _ = std::fs::remove_file(&out);
    let text = run(Command::new(&wingdrive).args(["--target", &target, "--allow-state-changing", "--capture-models"]).arg(&out));
    assert!(text.contains("models captured for /fx/1"), "{text}");
    assert!(text.contains("restored 13"), "{text}");
    let _ = std::fs::remove_file(&out);
}

#[test]
fn libwing_osc_client() {
    let Some(oscprobe) = tool("oscprobe") else {
        eprintln!("LIBWING_TOOLS not set; skipping oscprobe");
        return;
    };
    let desk = Desk::start();
    let target = desk.server.status().osc.unwrap().to_string();
    let text = run(Command::new(oscprobe).arg(&target));
    assert!(text.contains("all checks passed"), "{text}");
}
