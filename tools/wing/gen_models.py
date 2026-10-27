#!/usr/bin/env python3
"""Generates rust/crates/livedaw-dsp/src/wing/models_gen.rs from a WING parameter
sweep (propmap.jsonl from the Apache-2.0 libwing project, firmware 3.1).

Usage: gen_models.py path/to/propmap.jsonl > models_gen.rs

Only parameter names, ranges, units and choices are taken from the sweep. The
defaults are the console's: model_defaults.json next to this script holds the
values WING Edit 3.3.4 (which runs the console's firmware) gives every model when
a snapshot selects it without parameters (they match a factory-set WING and the
manual's figures)."""

import json
import math
import os
import struct
import sys

rows = [json.loads(l) for l in open(sys.argv[1])]
by_name = {r["fullname"]: r for r in rows}

DYN_NAMES = {  # as the console names them
    "GATE": "Wing Gate", "DUCK": "Wing Ducker", "E88": "Even 88 Gate", "9000G": "Soul 9000 Gate",
    "D241G": "Draw More 241", "DS902": "BDX902 Deesser", "DEQ": "Dynamic EQ", "DEQ2": "Dual Dynamic EQ",
    "WAVE": "Wave Designer", "PSE": "Source Extractor", "CMB": "PSE/LA Combo", "RIDE": "Auto Rider",
    "WARM": "Soul Warmth Pre", "COMP": "Wing Compressor", "EXP": "Wing Expander", "B160": "BDX 160 Comp",
    "B560": "BDX 560 Easy", "D241C": "Draw More Comp", "ECL33": "Even Comp/Lim", "9000C": "Soul 9000",
    "SBUS": "Soul Bus Comp", "RED3": "Red3 Compressor", "76LA": "76 Limiter Amp", "LA": "LA Leveler",
    "F670": "Fair Kid", "BLISS": "Eternal Bliss", "NSTR": "No-Stressor", "2250": "PIA2250 Rack",
    "L100": "LTA100 Leveler", "E88C": "Even 88 Comp", "LMT": "LMT Compressor", "ONEC": "One Knob Comp",
}
DYN_KIND = {  # broad category, used to group the model menu like the console
    "GATE": "gate", "DUCK": "gate", "E88": "gate", "9000G": "gate", "D241G": "gate", "PSE": "gate",
    "CMB": "special", "DS902": "special", "DEQ": "special", "DEQ2": "special", "WAVE": "special",
    "RIDE": "special", "WARM": "special",
}
EQ_NAMES = {
    "STD": "WING EQ", "SOUL": "Soul Analogue", "E88": "Even 88 Formant", "E84": "Even 84",
    "F110": "Fortissimo 110", "PULSAR": "Pulsar", "MACH4": "Mach EQ4", "PIA": "PIA 560 EQ",
}
TOOL_NAMES = {"TILT": "Tilt EQ", "MAX": "Maxer", "AP1": "All-Pass 1st", "AP2": "All-Pass 2nd"}

MODEL_DEFAULTS = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "model_defaults.json")))


class Exact(float):
    """A default as the console holds it, written as the shortest f32 literal."""


def items_of(r):
    return [i["item"] if isinstance(i, dict) else i for i in r.get("items", [])]


def default_for(key, pid, r):
    """The model's default (`key` is "DYN:GATE", "EQCH:SOUL", "FX:HALL"...)."""
    t = r["type"]
    items = items_of(r)
    d = MODEL_DEFAULTS.get(key, {}).get(pid)
    if d is not None:
        if isinstance(d, bool):
            return float(d)
        if isinstance(d, str):
            return float(items.index(d))
        if items:  # a float enum: the nearest item
            vals = [float(x) for x in items]
            return float(min(range(len(vals)), key=lambda i: abs(vals[i] - d)))
        return Exact(d)
    if key in MODEL_DEFAULTS and key != "FX:EXT":
        raise SystemExit(f"no default for {key}/{pid}")
    # EXT's RETURN group and input (WING Edit's are a console's I/O) and TRIM: the
    # start of each range; its FX MODE and LATENCY are the console's (model_defaults.json).
    if t == "fader level":
        return 0.0
    if items:
        return 0.0
    if t == "integer":
        return float(r.get("minint", 0))
    lo, hi = r.get("minfloat", 0.0), r.get("maxfloat", 1.0)
    a, b = min(lo, hi), max(lo, hi)
    if a <= 0 <= b:
        return 0.0
    if t == "log float" and a > 0:
        return math.sqrt(a * b)
    return (a + b) / 2



def kind_of(r):
    t = r["type"]
    items = items_of(r)
    if t == "fader level":
        return "Fader", None
    if t == "string enum":
        return "Choice", items
    if t == "float enum":
        return "Values", items
    if t == "integer":
        if r.get("minint") == 0 and r.get("maxint") == 1:
            return "Toggle", None
        return "Int", None
    if t == "log float":
        return "Log", None
    return "Linear", None


def rust_str(s):
    return json.dumps(s)


def fnum(x):
    if isinstance(x, Exact):
        f = struct.unpack("f", struct.pack("f", float(x)))[0]
        for n in range(1, 10):
            s = "%.*g" % (n, f)
            if struct.unpack("f", struct.pack("f", float(s)))[0] == f:
                break
        v = float(s)
        if v == round(v) and abs(v) < 1e7:
            return f"{round(v)}.0"
        return repr(v)
    # The sweep carries float32 artefacts (1000.0001220703125, 1.100000023841858).
    x = round(float(x), 4)
    if abs(x - round(x)) < 1e-3 and abs(x) < 1e7:
        return f"{round(x)}.0"
    return repr(x)


def emit_params(prefix, model, defaults_key):
    out = []
    for r in rows:
        fn = r["fullname"]
        if not fn.startswith(prefix + model + "/"):
            continue
        pid = fn[len(prefix + model + "/"):]
        if "/" in pid or pid.startswith("$"):
            continue
        kind, items = kind_of(r)
        if kind in ("Choice", "Values"):
            lo, hi, steps = 0, len(items) - 1, len(items)
        elif kind == "Fader":
            # A level control: -60 dB (off) .. +10 dB.
            lo, hi, steps = -60.0, 10.0, 141
        elif kind in ("Toggle", "Int"):
            lo, hi = r.get("minint", 0), r.get("maxint", 1)
            steps = hi - lo + 1
        else:
            lo, hi, steps = r["minfloat"], r["maxfloat"], r.get("steps", 101)
        d = default_for(defaults_key, pid, r)
        unit = r.get("unit", "")
        unit = {"meters": "m", "seconds": "s"}.get(unit, unit)
        if kind == "Fader":
            unit = "dB"
        label = r.get("longname") or pid.upper()
        if kind == "Choice":
            k = "ParamKind::Choice(&[" + ", ".join(rust_str(i) for i in items) + "])"
        elif kind == "Values":
            k = "ParamKind::Values(&[" + ", ".join(fnum(v) for v in items) + "])"
        elif kind == "Fader":
            k = "ParamKind::Linear"
        else:
            k = "ParamKind::" + kind
        out.append(
            f"        ParamSpec {{ id: {rust_str(pid)}, label: {rust_str(label)}, unit: {rust_str(unit)}, "
            f"kind: {k}, min: {fnum(lo)}, max: {fnum(hi)}, steps: {int(steps)}, default: {fnum(d)} }},"
        )
    return out


def model_items(path):
    return items_of(by_name[path])


print("// GENERATED by tools/wing/gen_models.py from a WING 3.1 parameter sweep. Do not edit by hand.")
print("// Parameter names, ranges and choices describe the console; defaults are WING Edit 3.3.4's")
print("// (tools/wing/model_defaults.json).")
print()
print("use super::{ModelCategory, ModelSpec, ParamKind, ParamSpec};")
print()


def table(name, prefix, models, names, cat_of, key):
    print(f"pub static {name}: &[ModelSpec] = &[")
    for m in models:
        params = emit_params(prefix, m, f"{key}:{m}")
        print(f"    ModelSpec {{ token: {rust_str(m)}, name: {rust_str(names.get(m, m))}, category: {cat_of(m)}, params: &[")
        for p in params:
            print(p)
        print("    ] },")
    print("];")
    print()


dyn_models = model_items("/ch/1/gate/mdl")
table("DYN_MODELS", "/ch/1/gate/", dyn_models, DYN_NAMES,
      lambda m: "ModelCategory::" + {"gate": "Gate", "special": "Special"}.get(DYN_KIND.get(m), "Compressor"), "DYN")
table("EQ_MODELS_CH", "/ch/1/eq/", model_items("/ch/1/eq/mdl"), EQ_NAMES, lambda m: "ModelCategory::Eq", "EQCH")
table("EQ_MODELS_BUS", "/bus/1/eq/", model_items("/bus/1/eq/mdl"), EQ_NAMES, lambda m: "ModelCategory::Eq", "EQBUS")
table("FILTER_TOOLS", "/ch/1/flt/", model_items("/ch/1/flt/mdl"), TOOL_NAMES, lambda m: "ModelCategory::Filter", "FLT")

# Models available on aux channels (a subset on the console)
print("/// Dynamics models available on aux channels (the console offers a subset there).")
print("pub static AUX_DYN_MODELS: &[&str] = &[" + ", ".join(rust_str(m) for m in model_items("/aux/1/dyn/mdl")) + "];")
print("/// Process orders (G = gate slot, E = EQ, D = dynamics slot, I = pre-fader insert), console order.")
print("pub static PROCESS_ORDERS: &[&str] = &[" + ", ".join(rust_str(m) for m in model_items("/ch/1/proc")) + "];")
print("/// Pre-fader tap points for bus sends, console order.")
print("pub static TAP_POINTS: &[&str] = &[" + ", ".join(rust_str(m) for m in model_items("/ch/1/ptap")) + "];")



# ---------------------------------------------------------------------------------
# FX rack (the 16 FX slots). Names as the console's documentation shows them.

FX_NAMES = {
    "NONE": "None", "EXT": "External",
    "HALL": "Hall Reverb", "ROOM": "Room Reverb", "CHAMBER": "Chamber Reverb", "PLATE": "Plate Reverb",
    "CONCERT": "Concert Reverb", "AMBI": "Ambience", "V-ROOM": "Vintage Room", "V-REV": "Vintage Reverb",
    "V-PLATE": "Vintage Plate", "GATED": "Gated Reverb", "REVERSE": "Reverse Reverb", "DEL/REV": "Delay/Reverb",
    "SHIMMER": "Shimmer Reverb", "SPRING": "Spring Reverb", "DIMCRS": "Dimension CRS", "CHORUS": "Stereo Chorus",
    "FLANGER": "Stereo Flanger", "ST-DL": "Stereo Delay", "TAP-DL": "Ultratap Delay", "TAPE-DL": "Tape Delay",
    "OILCAN": "Oilcan Delay", "BBD-DL": "BBD Delay", "PITCH": "Stereo Pitch", "D-PITCH": "Dual Pitch",
    "VSS3": "VSS3 Reverb", "BPLATE": "Blue Plate",
    "GEQ": "Graphic EQ", "PIA": "PIA 560 GEQ", "DOUBLE": "Double Vocal", "PCORR": "Pitch Fix",
    "LIMITER": "Precision Limiter", "DE-S2": "2-Band DeEsser", "ENHANCE": "Ultra Enhancer", "EXCITER": "Exciter",
    "P-BASS": "Psycho Bass", "ROTARY": "Rotary Speaker", "PHASER": "Phaser", "PANNER": "Tremolo/Panner",
    "TAPE": "Tape Machine", "MOOD": "Mood Filter", "SUB": "Sub Octaver", "RACKAMP": "Rack Amp",
    "UKROCK": "UK Rock Amp", "ANGEL": "Angel Amp", "JAZZC": "Jazz Clean Amp", "DELUXE": "Deluxe Amp",
    "BODY": "BodyRez", "SOUL": "Soul Analogue", "E88": "Even 88 Formant", "E84": "Even 84",
    "F110": "Fortissimo 110", "PULSAR": "Pulsar", "MACH4": "Mach EQ4", "C5-CMB": "Combinator",
    "SUB-M": "Sub Monster", "V-IMG": "Velvet Imager", "SPKMAN": "Speaker Manager", "DEQ3": "Triple DEQ",
    "*EVEN*": "Even Channel", "*SOUL*": "Soul Channel", "*VINTAGE*": "Vintage Channel", "*BUS*": "Bus Channel",
    "*MASTER*": "Mastering",
}
FX_PREMIUM = ["HALL", "ROOM", "CHAMBER", "PLATE", "CONCERT", "AMBI", "V-ROOM", "V-REV", "V-PLATE", "GATED",
              "REVERSE", "DEL/REV", "SHIMMER", "SPRING", "DIMCRS", "CHORUS", "FLANGER", "ST-DL", "TAP-DL",
              "TAPE-DL", "OILCAN", "BBD-DL", "PITCH", "D-PITCH", "VSS3", "BPLATE"]

def fx_category(m):
    if m == "NONE":
        return "ModelCategory::FxNone"
    if m == "EXT":
        return "ModelCategory::FxExternal"
    if m.startswith("*"):
        return "ModelCategory::FxChannel"
    return "ModelCategory::FxPremium" if m in FX_PREMIUM else "ModelCategory::FxStandard"


fx_models = model_items("/fx/1/mdl")
print("/// The FX rack's devices, console order: NONE, EXT (an outboard insert through a")
print("/// hardware output and input, or here a VST3/AU/LV2 plugin), the premium devices (FX")
print("/// slots 1-8 only), the standard devices and the channel strips.")
print("pub static FX_MODELS: &[ModelSpec] = &[")
# EXT's SOURCE (Sidecar's, not the console's): a plugin hosted in the slot, or the
# console's outboard insert (manual 11.11 p. 164: SEND, TRIM, FX MODE, RETURN, LATENCY).
EXT_SOURCE = (
    '        ParamSpec { id: "src", label: "SOURCE", unit: "", '
    'kind: ParamKind::Choice(&["PLUGIN", "OUTBOARD"]), min: 0.0, max: 1.0, steps: 2, default: 0.0 },'
)
for m in fx_models:
    params = [] if m == "NONE" else emit_params("/fx/1/", m, defaults_key="FX:" + m)
    if m == "EXT":
        params.append(EXT_SOURCE)
    print(f"    ModelSpec {{ token: {rust_str(m)}, name: {rust_str(FX_NAMES.get(m, m))}, category: {fx_category(m)}, params: &[")
    for p in params:
        print(p)
    print("    ] },")
print("];")
print()
