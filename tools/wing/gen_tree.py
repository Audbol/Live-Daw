#!/usr/bin/env python3
"""Generates rust/crates/livedaw-wing/data/wing_tree.bin, the WING parameter tree
Sidecar's remote servers and snapshot files speak, from a WING parameter sweep
(propmap.jsonl from the Apache-2.0 libwing project, firmware 3.1, a WING Rack).

Usage: gen_tree.py path/to/propmap.jsonl path/to/wing_tree.bin

Only metadata is taken from the sweep: node names, numeric ids, types, units,
ranges and choices. The sweep lists every model's parameters under a synthetic
model level (/ch/1/gate/GATE/thr); on the console they sit directly under the
slot (/ch/1/gate/thr) and share one id per position, so they are stored once per
slot family ("model templates") plus each slot's ids by position.

Format (little endian), read by livedaw-wing/src/schema.rs:

  "LDWT" u32 version(1)
  u32 n, strings      [u8 len, bytes]...
  u32 n, enums        [u8 kind 0=string 1=float][u16 count] then per item:
                      string: u16 item, u16 longitem; float: f32 item, u16 longitem
  u32 n, defs         [u8 type][u8 unit][u8 flags (1 = read-only, 2 = has range)]
                      [u16 longname] + lin/log/fader-with-range: f32 min, f32 max,
                      i32 steps; int: i32 min, i32 max; enums: u16 enum; string:
                      u16 maxlen
  u32 n, rows         [u16 parent (0xffff = root)][u16 name (0xffff = none)]
                      [u16 index][u16 def][u32 id]   (depth-first, parents first)
  u32 n, families     [u16 nmodels] then per model [u16 token][u16 nparams]
                      then per param [u16 name][u16 index][u16 def]
  u32 n, slots        [u16 row][u16 family][u8 nids] then u32 id per position 1..
"""

import json
import re
import struct
import sys

TYPES = {"node": 0, "linear float": 1, "log float": 2, "fader level": 3, "integer": 4,
         "string enum": 5, "float enum": 6, "string": 7}
UNITS = {None: 0, "dB": 1, "%": 2, "ms": 3, "Hz": 4, "meters": 5, "seconds": 6, "octaves": 7}

rows = [json.loads(line) for line in open(sys.argv[1])]

# Model slots: a node with a string-enum 'mdl' child.
slots = {}
for r in rows:
    fn = r["fullname"]
    if fn.endswith("/mdl") and r["type"] == "string enum" and fn.count("/") >= 2:
        slots[fn[:-4]] = [i["item"] for i in r["items"]]


def model_row(fn):
    """(slot, model, param) if `fn` is a model parameter of a slot, else None.
    Model tokens may contain '/' (DEL/REV), so match by prefix."""
    k = fn.find("/", 1)
    while k != -1:
        slot = fn[:k]
        if slot in slots:
            rest = fn[k + 1:]
            for m in sorted(slots[slot], key=len, reverse=True):
                if rest.startswith(m + "/") and "/" not in rest[len(m) + 1:]:
                    return slot, m, rest[len(m) + 1:]
        k = fn.find("/", k + 1)
    return None


strings, string_ix = [], {}


def s(text):
    if text is None:
        return 0xFFFF
    if text not in string_ix:
        b = text.encode()
        assert len(b) < 256, text
        string_ix[text] = len(strings)
        strings.append(b)
    return string_ix[text]


enums, enum_ix = [], {}


def enum_of(r):
    t = r["type"]
    items = r.get("items", [])
    if t == "string enum":
        key = ("s", tuple((str(i["item"]), i.get("longitem", "")) for i in items))
        data = struct.pack("<BH", 0, len(items)) + b"".join(
            struct.pack("<HH", s(str(i["item"])), s(i.get("longitem", ""))) for i in items)
    else:
        key = ("f", tuple((float(i["item"]), i.get("longitem", "")) for i in items))
        data = struct.pack("<BH", 1, len(items)) + b"".join(
            struct.pack("<fH", float(i["item"]), s(i.get("longitem", ""))) for i in items)
    if key not in enum_ix:
        enum_ix[key] = len(enums)
        enums.append(data)
    return enum_ix[key]


defs, def_ix = [], {}


# Parameters the sweep marks read-only only because of the console's state at
# the time (a dynamics model without MIX/GAIN, a linked send's pan, a bus's send
# to itself, a stereo-linked matrix): consoles save them in snapshots, so they
# are writable here.
STATE_READ_ONLY = [re.compile(p) for p in (
    r"^/(ch|aux|bus|main|mtx)/\d+/(gate|dyn)/(mix|gain)$",
    r"^/(ch|aux)/\d+/send/(\d+|MX\d)/pan$",
    r"^/bus/\d+/send/\d+/(on|lvl|pre)$",
    r"^/(bus|main|mtx)/\d+/(pan|wid)$",
    r"^/io/in/[A-Z]+/\d+/mode$",
)]


def sweep_read_only(r):
    if any(p.match(r["fullname"]) for p in STATE_READ_ONLY):
        return False
    return bool(r.get("read_only"))


def def_of(r):
    t = TYPES[r["type"]]
    unit = UNITS[r.get("unit")]
    flags = 1 if sweep_read_only(r) else 0
    payload = b""
    if t in (1, 2):
        flags |= 2
        payload = struct.pack("<ffi", r["minfloat"], r["maxfloat"], r.get("steps", 0))
    elif t == 3:
        # The sweep omits fader ranges; a live console sends -144..10 dB in 1024 steps.
        flags |= 2
        payload = struct.pack("<ffi", -144.0, 10.0, 1024)
    elif t == 4:
        payload = struct.pack("<ii", r["minint"], r["maxint"])
    elif t in (5, 6):
        payload = struct.pack("<H", enum_of(r))
    elif t == 7:
        payload = struct.pack("<H", r.get("maxstringlen", 0))
    data = struct.pack("<BBBH", t, unit, flags, s(r.get("longname", ""))) + payload
    if data not in def_ix:
        def_ix[data] = len(defs)
        defs.append(data)
    return def_ix[data]


def family_key(slot):
    return "/".join("N" if p.isdigit() else p for p in slot.split("/"))


# Static rows (everything that isn't a model parameter), parents first.
static, row_ix = [], {}
model_params = {}  # slot -> {model: [(name, index, def, id)]}
for r in rows:
    fn = r["fullname"]
    m = model_row(fn)
    if m:
        slot, model, name = m
        model_params.setdefault(slot, {}).setdefault(model, []).append((name, r.get("index", 0), def_of(r), r["id"]))
        continue
    parent = fn.rsplit("/", 1)[0]
    pix = 0xFFFF if parent == "" else row_ix[parent]
    row_ix[fn] = len(static)
    static.append(struct.pack("<HHHHI", pix, s(r.get("name")), r.get("index", 0), def_of(r), r["id"] & 0xFFFFFFFF))
assert len(static) < 0xFFFF

# Model templates per slot family: the first slot of the family defines them.
families, family_ix, slot_rec = [], {}, []
mismatch = 0
for slot, models in slots.items():
    key = family_key(slot)
    params = model_params.get(slot, {})
    if key not in family_ix:
        family_ix[key] = len(families)
        data = struct.pack("<H", len(models))
        for model in models:
            ps = params.get(model, [])
            data += struct.pack("<HH", s(model), len(ps))
            for name, index, d, _ in ps:
                data += struct.pack("<HHH", s(name), index, d)
        families.append((key, data, {mo: [(n, i, d) for n, i, d, _ in params.get(mo, [])] for mo in models}))
    else:
        ref = families[family_ix[key]][2]
        mine = {mo: [(n, i, d) for n, i, d, _ in params.get(mo, [])] for mo in models}
        if [(n, i) for mo in models for n, i, _ in mine[mo]] != [(n, i) for mo in models for n, i, _ in ref.get(mo, [])]:
            mismatch += 1
    ids = {}
    for model, ps in params.items():
        for _, index, _, pid in ps:
            ids.setdefault(index, pid & 0xFFFFFFFF)
            assert ids[index] == pid & 0xFFFFFFFF, (slot, index)
    n = max(ids) if ids else 0
    slot_rec.append(struct.pack("<HHB", row_ix[slot], family_ix[key], n) +
                    b"".join(struct.pack("<I", ids.get(i, 0)) for i in range(1, n + 1)))

out = bytearray(b"LDWT" + struct.pack("<I", 1))
out += struct.pack("<I", len(strings)) + b"".join(struct.pack("<B", len(b)) + b for b in strings)
out += struct.pack("<I", len(enums)) + b"".join(enums)
out += struct.pack("<I", len(defs)) + b"".join(defs)
out += struct.pack("<I", len(static)) + b"".join(static)
out += struct.pack("<I", len(families)) + b"".join(f[1] for f in families)
out += struct.pack("<I", len(slot_rec)) + b"".join(slot_rec)
open(sys.argv[2], "wb").write(out)
print(f"{len(static)} rows, {sum(len(p) for m in model_params.values() for p in m.values())} model rows in "
      f"{len(families)} families / {len(slot_rec)} slots, {len(defs)} defs, {len(enums)} enums, "
      f"{len(strings)} strings, {len(out)} bytes; {mismatch} slots differ from their family template",
      file=sys.stderr)
