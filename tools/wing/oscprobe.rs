//! Drives a WING OSC server with libwing's own OSC client and fails (exit 1)
//! if any answer is missing or wrong. Built by tools/wing/libwing_tools.sh
//! next to libwing's wingdrive/wingprop/wingmon; it is our code, libwing is
//! only a dependency of the throwaway wrapper crate.
//!
//! oscprobe [host:port]   (default 127.0.0.1:2223)

use libwing::osc::{self, OscEvent, SubscriptionFormat, WingOscClient};
use std::time::Duration;

fn check(ok: bool, what: &str, failures: &mut u32) {
    println!("{} {what}", if ok { "ok  " } else { "FAIL" });
    if !ok {
        *failures += 1;
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let target = std::env::args().nth(1).unwrap_or_else(|| "127.0.0.1:2223".into());
    let c = WingOscClient::connect_addr(target.parse()?)?;
    let t = Duration::from_millis(500);
    let mut failures = 0;

    let info = osc::parse_console_info(&c.request(&osc::console_info(), t)?)?;
    println!("console: {} model {} fw {}", info.name, info.model, info.firmware);

    c.send(&osc::set_float("/ch/3/fdr", -9.5)?)?;
    let r = osc::parse_param_reply(&c.request(&osc::get_param("/ch/3/fdr")?, t)?)?;
    check(format!("{:?}", r.facets).contains("value: -9.5"), &format!("set/get /ch/3/fdr {:?}", r.facets), &mut failures);

    c.send(&osc::toggle("/ch/3/mute")?)?;
    let r = osc::parse_param_reply(&c.request(&osc::get_param("/ch/3/mute")?, t)?)?;
    check(format!("{:?}", r.facets).contains("value: 1"), &format!("toggle /ch/3/mute {:?}", r.facets), &mut failures);

    c.send(&osc::set_enum_by_name("/ch/3/eq/mdl", "SOUL")?)?;
    let r = osc::parse_param_reply(&c.request(&osc::get_param("/ch/3/eq/mdl")?, t)?)?;
    check(format!("{:?}", r.facets).contains("SOUL"), &format!("enum /ch/3/eq/mdl {:?}", r.facets), &mut failures);

    let r = osc::parse_param_reply(&c.request(&osc::get_param_by_id(0xf50f69f8u32 as i32), t)?)?;
    check(r.path == "/ch/1/mute", &format!("get by id 0xf50f69f8 -> {}", r.path), &mut failures);

    let ack = c.request(&osc::node_set_local("/ch/3", &[("fdr", "-4"), ("mute", "0")])?, t)?;
    check(osc::parse_node_ack(&ack).is_ok(), "node set (local)", &mut failures);
    let ack = c.request(&osc::node_set_root("/ch.4.fdr=-1,mute=1,.5.fdr=0"), t)?;
    check(osc::parse_node_ack(&ack).is_ok(), "node set (root)", &mut failures);
    let r = osc::parse_param_reply(&c.request(&osc::get_param("/ch/4/fdr")?, t)?)?;
    check(format!("{:?}", r.facets).contains("value: -1"), "node set applied", &mut failures);

    let dump = osc::parse_node_dump(&c.request(&osc::node_dump("/ch/3/main")?, t)?)?;
    check(!dump.is_empty(), &format!("node dump /ch/3/main: {} pairs", dump.len()), &mut failures);
    let desc = c.request(&osc::node_describe("/fx/1")?, t)?;
    check(!desc.args.is_empty(), "node describe /fx/1", &mut failures);

    // Subscription on its own socket; the change comes from the other client.
    let sub = WingOscClient::connect_addr(target.parse()?)?;
    sub.subscribe(SubscriptionFormat::OscNative)?;
    std::thread::sleep(Duration::from_millis(300));
    c.send(&osc::set_float("/bus/2/fdr", -11.0)?)?;
    let mut seen = false;
    for _ in 0..5 {
        match sub.poll_event(Duration::from_millis(400)) {
            Ok(OscEvent::Param(p)) => {
                println!("event {} {:?}", p.path, p.facets);
                seen |= p.path == "/bus/2/fdr";
            }
            Ok(other) => println!("event {other:?}"),
            Err(_) => break,
        }
    }
    check(seen, "subscription event for /bus/2/fdr", &mut failures);

    if failures > 0 {
        std::process::exit(1);
    }
    println!("oscprobe: all checks passed");
    Ok(())
}
