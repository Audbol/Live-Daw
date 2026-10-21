#!/usr/bin/env python3
"""Renders Sidecar for every WING recording in a folder, analyzes each pair and
writes one summary report.

  python3 run_all.py RECORDINGS_DIR [--out results] [--render-bin PATH] [--only NAME ...]
  python3 run_all.py --dry-run [--priority 1] [--modify TEST:PATH=VALUE ...]

Recordings are matched to tests by file name (`eq_peq_600_p6_q1.wav` or
`02_eq_peq_600_p6_q1.wav`) or, for any other name (`R_20261021-101500.wav`),
by the ID code in the recording. When RECORDINGS_DIR/wing_settings/NAME.json
exists (written by `wing_osc.py apply`), Sidecar is rendered with the values
the WING reported; otherwise with the plan's.

Sidecar's side is rendered with the engine itself (the `render` example of
livedaw-wing): `--render-bin` points at a built binary, else the script uses
rust/target/release/examples/render and builds it if needed (cargo, -j3).

--dry-run checks the kit without a WING: the "WING recordings" are Sidecar
renders with simulated capture artefacts (latency, drift, level offset,
noise), and `--modify` changes a setting on the simulated WING side only
(e.g. `--modify eq_peq_600_p6_q1:/ch/1/eq/2g=7`), which the report must catch.

Output: OUT/summary.md, OUT/reports/NAME.md (+ OUT/plots with matplotlib),
OUT/json/NAME.json, and the renders in OUT/sidecar.
"""

import argparse
import datetime
import glob
import json
import os
import re
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import analyze  # noqa: E402
import make_signals as ms  # noqa: E402
import simulate  # noqa: E402
from wavio import read_wav, write_wav  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
RUST = os.path.join(REPO, "rust")


def find_render(explicit=None, build=True, profile="release"):
    """The render binary: explicit, else built from the repository as it is now.

    cargo rebuilds only what changed, so the render always uses the current
    engine (a binary left from an older checkout would compare the WING with
    an old Sidecar)."""
    if explicit:
        return explicit
    exe = "render.exe" if os.name == "nt" else "render"
    path = os.path.join(RUST, "target", profile, "examples", exe)
    if not build:
        if os.path.exists(path):
            return path
        raise SystemExit("render binary not found: build it with "
                         "`cargo build --release -p livedaw-wing --example render` in rust/")
    cmd = ["cargo", "build", "-j3", "-p", "livedaw-wing", "--example", "render"]
    if profile == "release":
        cmd.insert(2, "--release")
    print("building Sidecar's render tool:", " ".join(cmd), file=sys.stderr)
    subprocess.run(cmd, cwd=RUST, check=True)
    return path


def settings_for(plan, test, overrides=None):
    s = dict(plan["common"])
    s.update(test["settings"])
    s.update(overrides or {})
    return s


def render(render_bin, settings, signal_path, out_path, work):
    sfile = os.path.join(work, os.path.splitext(os.path.basename(out_path))[0] + ".settings.json")
    with open(sfile, "w") as f:
        json.dump({"settings": settings}, f, indent=1)
    r = subprocess.run([render_bin, "--settings", sfile, "--in", signal_path, "--out", out_path],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"render failed: {r.stderr.strip()}")


def signal_file(test, folder):
    path = os.path.join(folder, ms.file_name(test))
    if not os.path.exists(path):
        x, _ = ms.test_signal(test)
        write_wav(path, x, ms.SR, 24)
    return path


def identify(path, plan):
    """The test a recording belongs to: by file name, else by its ID code."""
    stem = os.path.splitext(os.path.basename(path))[0]
    by_name = {t["name"]: t for t in plan["tests"]}
    if stem in by_name:
        return by_name[stem]
    m = re.match(r"^\d+_(.+)$", stem)
    if m and m.group(1) in by_name:
        return by_name[m.group(1)]
    data, sr = read_wav(path)
    if sr != ms.SR or len(data) < ms.SR:
        return None
    ch = data[:, 2] if data.shape[1] >= 3 and np.sqrt(np.mean(data[:, 2] ** 2)) > 1e-5 else data[:, 0]
    start, _ = analyze.find_chirp(ch, ms.sync_chirp(True))
    if start is None:
        return None
    code = ms.decode_id(ch, start + (ms.ID_AT_S - ms.LEAD_S) * ms.SR)
    for t in plan["tests"]:
        if t["code"] == code:
            return t
    return None


def parse_value(text):
    try:
        return float(text)
    except ValueError:
        return text


def summary_markdown(results, missing, out_dir):
    lines = ["# WING vs Sidecar: summary", "",
             f"{datetime.datetime.now():%Y-%m-%d %H:%M}, {len(results)} tests analyzed.", "",
             "| # | Test | Priority | Result | Within tolerance | Worst difference | Warnings |",
             "|---|---|---|---|---|---|---|"]
    for r in sorted(results, key=lambda r: r["code"]):
        verdict = "PASS" if r["failed"] == 0 else "**FAIL**"
        if r.get("error"):
            verdict = "**ERROR**"
        rel = os.path.join("reports", f"{r['test']}.md")
        lines.append(f"| {r['code']} | [{r['test']}]({rel}) | {r['priority']} | {verdict} | "
                     f"{r['checked'] - r['failed']}/{r['checked']} | {r.get('worst') or r.get('error', '')} | "
                     f"{len(r.get('warnings', []))} |")
    if missing:
        lines += ["", "Not recorded: " + ", ".join(missing)]
    lines.append("")
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recordings", nargs="?", help="folder with the WING recordings")
    ap.add_argument("--plan", default=os.path.join(HERE, "test_plan.json"))
    ap.add_argument("--out", help="output folder (default: results-YYYYMMDD-HHMMSS)")
    ap.add_argument("--render-bin", help="Sidecar's render binary")
    ap.add_argument("--debug-build", action="store_true", help="build/use the debug render binary")
    ap.add_argument("--only", nargs="*", help="only these tests")
    ap.add_argument("--priority", type=int, help="dry run: tests up to this priority")
    ap.add_argument("--dry-run", action="store_true", help="simulate the WING recordings (see above)")
    ap.add_argument("--modify", action="append", default=[], help="dry run: TEST:PATH=VALUE on the WING side")
    ap.add_argument("--no-plots", action="store_true")
    a = ap.parse_args(argv)
    if not a.recordings and not a.dry_run:
        ap.error("give the recordings folder, or --dry-run")
    plan = ms.load_plan(a.plan)
    out = a.out or f"results-{datetime.datetime.now():%Y%m%d-%H%M%S}"
    for sub in ("sidecar", "reports", "json", "signals", "work"):
        os.makedirs(os.path.join(out, sub), exist_ok=True)
    rbin = find_render(a.render_bin, profile="debug" if a.debug_build else "release")
    sigdir = os.path.join(out, "signals")
    work = os.path.join(out, "work")

    pairs = []
    if a.dry_run:
        mods = {}
        for m in a.modify:
            name, _, kv = m.partition(":")
            path, _, val = kv.partition("=")
            mods.setdefault(name, {})[path] = parse_value(val)
        rec_dir = os.path.join(out, "simulated_wing")
        os.makedirs(rec_dir, exist_ok=True)
        tests = [t for t in plan["tests"] if (not a.only or t["name"] in a.only)
                 and (a.priority is None or t["priority"] <= a.priority)]
        for i, t in enumerate(tests):
            sig = signal_file(t, sigdir)
            clean = os.path.join(work, f"{t['name']}.wing_clean.wav")
            render(rbin, settings_for(plan, t, mods.get(t["name"])), sig, clean, work)
            data, sr = read_wav(clean)
            cap = simulate.capture(data, sr, latency_samples=1234.37 + 101 * i, gain_db=-0.3, drift_ppm=25.0,
                                   noise_db=-100.0, lead_s=0.8, tail_s=0.5, seed=i + 1)
            path = os.path.join(rec_dir, f"R_{i:03d}.wav")
            write_wav(path, cap, sr, 24)
            pairs.append((path, t))
    else:
        files = sorted(set(glob.glob(os.path.join(a.recordings, "*.wav")) +
                           glob.glob(os.path.join(a.recordings, "*.WAV"))))
        latest = {}
        for path in files:
            t = identify(path, plan)
            if t is None:
                print(f"{os.path.basename(path)}: not a test recording (no ID code found), skipped", file=sys.stderr)
                continue
            if a.only and t["name"] not in a.only:
                continue
            if t["name"] in latest:
                print(f"{os.path.basename(latest[t['name']][0])}: {t['name']} again in {os.path.basename(path)}; "
                      "using the later file", file=sys.stderr)
            latest[t["name"]] = (path, t)
        pairs = sorted(latest.values(), key=lambda p: p[1]["code"])

    results = []
    done = set()
    for path, t in pairs:
        name = t["name"]
        done.add(name)
        print(f"{name}: {os.path.basename(path)}", file=sys.stderr)
        entry = {"test": name, "code": t["code"], "priority": t["priority"], "checked": 0, "failed": 0}
        try:
            settings = settings_for(plan, t)
            reported = os.path.join(a.recordings or "", "wing_settings", f"{name}.json") if a.recordings else ""
            if reported and os.path.exists(reported):
                with open(reported) as f:
                    settings.update(json.load(f).get("settings", {}))
            sig = signal_file(t, sigdir)
            side = os.path.join(out, "sidecar", f"{name}.wav")
            render(rbin, settings, sig, side, work)
            res = analyze.analyze(path, side, t, plan)
            plots = [] if a.no_plots else analyze.write_plots(res, os.path.join(out, "plots"),
                                                              os.path.join(out, "reports"))
            with open(os.path.join(out, "reports", f"{name}.md"), "w") as f:
                f.write(analyze.report_markdown(res, plots))
            j = analyze.result_json(res)
            with open(os.path.join(out, "json", f"{name}.json"), "w") as f:
                json.dump(j, f, indent=1)
            entry.update({k: j[k] for k in ("checked", "failed", "worst", "warnings")})
            print(f"  {'PASS' if j['failed'] == 0 else 'FAIL'} {j['checked'] - j['failed']}/{j['checked']} "
                  f"{j['worst']}", file=sys.stderr)
        except Exception as e:  # keep going: one bad recording shouldn't stop the rest
            entry["error"] = str(e)
            entry["failed"] = 1
            print(f"  ERROR {e}", file=sys.stderr)
        results.append(entry)
    missing = [t["name"] for t in plan["tests"] if t["name"] not in done
               and (not a.only or t["name"] in a.only) and (not a.dry_run or a.priority is None
                                                            or t["priority"] <= a.priority)]
    with open(os.path.join(out, "summary.md"), "w") as f:
        f.write(summary_markdown(results, missing if not a.dry_run else [], out))
    with open(os.path.join(out, "summary.json"), "w") as f:
        json.dump(results, f, indent=1)
    failed = sum(1 for r in results if r["failed"])
    print(f"{len(results)} tests, {failed} with differences; summary: {os.path.join(out, 'summary.md')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
