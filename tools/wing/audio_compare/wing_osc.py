#!/usr/bin/env python3
"""Sets a WING up for a test of the plan over OSC, and reads the values back.

Optional: everything here can be dialled in by hand from test_plan.md. Using
it avoids typos and records the values the console actually took, which
run_all.py then renders Sidecar with. It talks the WING's documented OSC
(UDP 2223, Patrick-Gilles Maillot's "WING Remote Protocols"); it works
against Sidecar's WING REMOTE too. It CHANGES the console: save your show
first.

  python3 wing_osc.py HOST setup               patch for the session (USB player -> CH 1,
                                               recorder 1-3) and the common settings
  python3 wing_osc.py HOST apply TEST          common settings + the test's, read back,
                                               saved to wing_settings/TEST.json
  python3 wing_osc.py HOST read TEST           read only: what the console has now
  python3 wing_osc.py HOST record TEST --file PATH_ON_STICK
                                               EXPERIMENTAL: apply, then start the USB
                                               recorder, play the file, stop both
  python3 wing_osc.py HOST list                the tests

Options: --plan FILE, --port N (2223), --save DIR (wing_settings).
"""

import argparse
import json
import os
import re
import socket
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
OSC_PORT = 2223


def _pad(b):
    return b + b"\0" * ((4 - len(b) % 4) % 4)


def _s(text):
    return _pad(text.encode("utf-8") + b"\0")


def encode(addr, args=()):
    out = _s(addr)
    if not args:
        return out
    tags, body = ",", b""
    for a in args:
        if isinstance(a, bool) or isinstance(a, int):
            tags += "i"
            body += struct.pack(">i", int(a))
        elif isinstance(a, float):
            tags += "f"
            body += struct.pack(">f", a)
        else:
            tags += "s"
            body += _s(str(a))
    return out + _s(tags) + body


def _read_str(b, i):
    end = b.index(b"\0", i)
    return b[i:end].decode("utf-8", "replace"), i + ((end - i + 4) // 4) * 4


def decode(b):
    try:
        addr, i = _read_str(b, 0)
        if i >= len(b):
            return addr, []
        tags, i = _read_str(b, i)
        args = []
        for t in tags[1:]:
            if t == "i":
                args.append(struct.unpack(">i", b[i:i + 4])[0])
                i += 4
            elif t == "f":
                args.append(struct.unpack(">f", b[i:i + 4])[0])
                i += 4
            elif t == "s":
                s, i = _read_str(b, i)
                args.append(s)
            else:
                break
        return addr, args
    except (ValueError, struct.error, IndexError):
        return None


class Osc:
    def __init__(self, host, port=OSC_PORT, timeout=0.5):
        self.addr = (host, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(timeout)

    def send(self, path, *args):
        self.sock.sendto(encode(path, args), self.addr)

    def get(self, path, retries=3):
        for _ in range(retries):
            self.send(path)
            deadline = time.time() + self.sock.gettimeout()
            while time.time() < deadline:
                try:
                    data, _ = self.sock.recvfrom(65536)
                except socket.timeout:
                    break
                msg = decode(data)
                if msg and msg[0].rstrip("/") == path.rstrip("/"):
                    return msg[1]
        return None


def parse_number(text):
    t = str(text).strip().strip("'")
    if t.lower() in ("-oo", "-inf"):
        return -144.0
    try:
        return float(t)
    except ValueError:
        pass
    m = re.match(r"^(-?\d*)[kK](\d*)$", t)
    if m:
        whole = float(m.group(1) or 0)
        frac = float("0." + m.group(2)) if m.group(2) else 0.0
        return (whole + frac) * 1000.0
    return None


def reported_value(args):
    """(display text, value for a settings file) from a get reply."""
    if not args:
        return None, None
    text = str(args[0])
    if len(args) >= 3 and isinstance(args[2], float):
        return text, float(args[2])
    num = parse_number(text)
    if len(args) >= 3 and isinstance(args[2], int) and num is not None:
        return text, int(round(num))
    return text, text


def matches(wanted, text):
    if text is None:
        return False
    if isinstance(wanted, str):
        a, b = wanted.strip().lower(), text.strip().lower()
        if a == b:
            return True
        x, y = parse_number(a), parse_number(b)
        return x is not None and y is not None and abs(x - y) <= 1e-6 + 1e-3 * abs(x)
    got = parse_number(text)
    if got is None:
        return False
    # The console puts values on its steps (600 Hz reads 601.4): allow 1 % or 0.051.
    return abs(got - wanted) <= max(0.051, 0.012 * abs(wanted))


def ordered(settings):
    """Model choices first: a model's parameters exist once it is selected."""
    items = list(settings.items())
    return [kv for kv in items if kv[0].endswith("/mdl")] + [kv for kv in items if not kv[0].endswith("/mdl")]


def apply(osc, settings, write=True, pause=0.01):
    rows = []
    for path, value in ordered(settings):
        if write:
            osc.send(path, value)
            time.sleep(pause)
            if path.endswith("/mdl"):
                time.sleep(0.2)  # a new FX / dynamics model takes a moment
    time.sleep(0.1)
    for path, value in settings.items():
        text, val = reported_value(osc.get(path))
        rows.append((path, value, text, val, matches(value, text)))
    return rows


def load_plan(path):
    with open(path) as f:
        return json.load(f)


def find_test(plan, name):
    for t in plan["tests"]:
        if t["name"] == name or f"{t['code']:02d}" == name:
            return t
    raise SystemExit(f"no test {name!r} (python3 wing_osc.py HOST list)")


def print_rows(rows):
    bad = 0
    for path, wanted, text, _, ok in rows:
        mark = "ok" if ok else ("NO REPLY" if text is None else "DIFFERS")
        bad += 0 if ok else 1
        print(f"  {mark:8} {path:28} wanted {wanted!s:10} console: {text}")
    return bad


def save(rows, folder, name):
    os.makedirs(folder, exist_ok=True)
    data = {"settings": {p: v for p, _, _, v, _ in rows if v is not None},
            "displayed": {p: t for p, _, t, _, _ in rows}, "time": time.strftime("%Y-%m-%d %H:%M:%S")}
    path = os.path.join(folder, f"{name}.json")
    with open(path, "w") as f:
        json.dump(data, f, indent=1)
    return path


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("command", choices=["setup", "apply", "read", "record", "list"])
    ap.add_argument("test", nargs="?")
    ap.add_argument("--plan", default=os.path.join(HERE, "test_plan.json"))
    ap.add_argument("--port", type=int, default=OSC_PORT)
    ap.add_argument("--save", default="wing_settings", help="folder for the values read back")
    ap.add_argument("--file", help="record: the test file's path on the USB stick, as the WING shows it")
    a = ap.parse_args(argv)
    plan = load_plan(a.plan)
    if a.command == "list":
        for t in plan["tests"]:
            print(f"{t['code']:02d} {t['name']:24} P{t['priority']} {t['signal']:6} {t['title']}")
        return 0
    osc = Osc(a.host, a.port)
    if osc.get("/ch/1/fdr") is None:
        raise SystemExit(f"no OSC reply from {a.host}:{a.port} (network? WING OSC enabled?)")
    if a.command == "setup":
        settings = dict(plan["wing_only"])
        settings.update(plan["common"])
        bad = print_rows(apply(osc, settings))
        print("setup done" if not bad else f"{bad} values differ: check them on the console")
        return 1 if bad else 0
    if not a.test:
        ap.error("which test?")
    test = find_test(plan, a.test)
    settings = dict(plan["common"])
    settings.update(test["settings"])
    rows = apply(osc, settings, write=a.command != "read")
    print(f"{test['code']:02d} {test['name']}: {test['title']}")
    bad = print_rows(rows)
    path = save(rows, a.save, test["name"])
    print(f"values as the console reports them: {path}" + (f" ({bad} differ)" if bad else ""))
    if a.command == "record":
        if not a.file:
            ap.error("record needs --file (the test file on the USB stick)")
        print("EXPERIMENTAL: recorder REC, player PLAYFILE ...")
        osc.send("/rec/$action", "REC")
        time.sleep(1.0)
        osc.send("/play/$playfile", a.file)
        osc.send("/play/$action", "PLAYFILE")
        time.sleep(test["seconds"] + 2.0)
        osc.send("/rec/$action", "STOP")
        osc.send("/play/$action", "STOP")
        time.sleep(0.5)
        print(f"recorded: {reported_value(osc.get('/rec/$actfile'))[0]} (check it on the console)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
