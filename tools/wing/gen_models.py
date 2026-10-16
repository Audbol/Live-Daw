#!/usr/bin/env python3
"""Generates rust/crates/livedaw-dsp/src/wing/models_gen.rs from a WING parameter
sweep (propmap.jsonl from the Apache-2.0 libwing project, firmware 3.1).

Usage: gen_models.py path/to/propmap.jsonl > models_gen.rs

Only parameter names, ranges, units and choices are taken from the sweep; the
defaults below are ours (the sweep doesn't carry them)."""

import json
import math
import sys

rows = [json.loads(l) for l in open(sys.argv[1])]
by_name = {r["fullname"]: r for r in rows}

DYN_NAMES = {  # as the console names them
    "GATE": "Gate/Expander", "DUCK": "Ducker", "E88": "Even 88 Gate", "9000G": "Soul 9000 Gate",
    "D241G": "Draw More 241", "DS902": "BDX 902 De-Esser", "DEQ": "Dynamic EQ", "DEQ2": "Dual Dynamic EQ",
    "WAVE": "Wave Designer", "PSE": "Source Extractor", "CMB": "Extractor + Leveler", "RIDE": "Auto Rider",
    "WARM": "Soul Warmth Pre", "COMP": "Wing Compressor", "EXP": "Wing Expander", "B160": "BDX 160 Comp",
    "B560": "BDX 560 Easy", "D241C": "Draw More Comp", "ECL33": "Even Comp/Lim", "9000C": "Soul 9000",
    "SBUS": "Soul Bus Comp", "RED3": "Red3 Compressor", "76LA": "76 Limiter Amp", "LA": "LA Leveler",
    "F670": "Fair Kid", "BLISS": "Eternal Bliss", "NSTR": "No-Stressor", "2250": "PIA2250 Rack",
    "L100": "LTA100 Leveler", "E88C": "Even 88 Comp", "LMT": "Transient Limiter", "ONEC": "One-Knob Comp",
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

# (model, param) -> default value (choices: the item text)
DEFAULTS = {
    ("GATE", "thr"): -40, ("GATE", "range"): 60, ("GATE", "att"): 0, ("GATE", "hld"): 50, ("GATE", "rel"): 200,
    ("GATE", "acc"): 0, ("GATE", "ratio"): "gate",
    ("DUCK", "thr"): -30, ("DUCK", "range"): 12, ("DUCK", "att"): 5, ("DUCK", "hld"): 100, ("DUCK", "rel"): 400,
    ("E88", "thr"): -30, ("E88", "hyst"): 5, ("E88", "range"): 40, ("E88", "rel"): 300,
    ("9000G", "thr"): -30, ("9000G", "range"): 30, ("9000G", "hld"): 50, ("9000G", "rel"): 300,
    ("D241G", "thr"): -40,
    ("DS902", "f"): 5000, ("DS902", "range"): 6,
    ("DEQ", "thr"): -20, ("DEQ", "ratio"): 2, ("DEQ", "att"): 10, ("DEQ", "rel"): 150, ("DEQ", "filt"): "BP",
    ("DEQ", "g"): 0, ("DEQ", "f"): 3000, ("DEQ", "q"): 2, ("DEQ", "mode"): "high",
    # Dual DEQ: the panel of manual Fig. 91 (GAIN 0 dB = the band does nothing).
    **{("DEQ2", f"{b}-{k}"): v for b, f in (("1", 200), ("2", 2000)) for k, v in
       (("thr", -20), ("ratio", 2), ("att", 50), ("rel", 300), ("filt", "BP"), ("g", 0), ("f", f), ("q", 1),
        ("mode", "low"))},
    ("WAVE", "att"): 0, ("WAVE", "sust"): 0, ("WAVE", "g"): 0,
    ("PSE", "thr"): -20, ("PSE", "depth"): 10, ("CMB", "thr"): -20, ("CMB", "depth"): 10,
    ("CMB", "ingain"): 50, ("CMB", "cpeak"): 30,
    ("RIDE", "thr"): -40, ("RIDE", "tgt"): -18, ("RIDE", "spd"): 10, ("RIDE", "ratio"): 4, ("RIDE", "hld"): 1,
    ("RIDE", "range"): 6,
    ("WARM", "drv"): 50, ("WARM", "hrm"): 0, ("WARM", "col"): 0, ("WARM", "trim"): 0, ("WARM", "wmix"): 100,
    ("COMP", "thr"): -20, ("COMP", "ratio"): 3, ("COMP", "knee"): 2, ("COMP", "det"): "RMS", ("COMP", "att"): 10,
    ("COMP", "hld"): 1, ("COMP", "rel"): 150, ("COMP", "env"): "LOG",
    ("EXP", "thr"): -40, ("EXP", "ratio"): 2, ("EXP", "knee"): 2, ("EXP", "att"): 5, ("EXP", "hld"): 20,
    ("EXP", "rel"): 200,
    ("B160", "thr"): 0.5, ("B160", "comp"): 3, ("B560", "thr"): 0, ("B560", "ratio"): 3,
    ("D241C", "thr"): -20, ("D241C", "ratio"): 3, ("D241C", "att"): 5, ("D241C", "rel"): 300, ("D241C", "lim"): 0,
    ("D241C", "lrel"): 300,
    ("ECL33", "lthr"): -1, ("ECL33", "lrec"): "100", ("ECL33", "con"): 1, ("ECL33", "cthr"): -20,
    ("ECL33", "ratio"): 2, ("ECL33", "crec"): "400", ("ECL33", "cgain"): 0,
    ("9000C", "thr"): 0, ("9000C", "ratio"): 4, ("9000C", "rel"): 300,
    ("SBUS", "thr"): -16, ("SBUS", "ratio"): 4, ("SBUS", "att"): 10, ("SBUS", "rel"): "AUTO",
    ("RED3", "thr"): -20, ("RED3", "ratio"): 3, ("RED3", "att"): 10, ("RED3", "rel"): 300,
    ("76LA", "in"): -24, ("76LA", "out"): -24, ("76LA", "att"): 4, ("76LA", "rel"): 4, ("76LA", "ratio"): "4",
    ("LA", "ingain"): 50, ("LA", "peak"): 30,
    # DC BIAS: 1.00 is the factory calibration (manual p.115).
    ("F670", "in"): -10, ("F670", "thr"): 5, ("F670", "time"): 2, ("F670", "bias"): 1.0,
    ("BLISS", "thr"): -20, ("BLISS", "ratio"): 2, ("BLISS", "att"): 10, ("BLISS", "rel"): 150, ("BLISS", "glim"): -12,
    ("NSTR", "in"): 5, ("NSTR", "out"): 5, ("NSTR", "att"): 5, ("NSTR", "rel"): 5, ("NSTR", "ratio"): "4:1",
    ("2250", "thr"): 5, ("2250", "ratio"): 3, ("2250", "att"): "MED", ("2250", "rel"): 300,
    ("L100", "ingain"): 5, ("L100", "gr"): 3, ("L100", "att"): "MED", ("L100", "rel"): "MED",
    ("E88C", "thr"): 0, ("E88C", "ratio"): 3, ("E88C", "rel"): 300,
    ("LMT", "tspd"): 0.5, ("LMT", "comp"): 30,
    ("ONEC", "gr"): 3,
    # EQ
    ("SOUL", "lf"): 5, ("SOUL", "lmf"): 5, ("SOUL", "lmq"): 5, ("SOUL", "hmf"): 5, ("SOUL", "hmq"): 5, ("SOUL", "hf"): 5,
    ("E88", "lf"): 5, ("E88", "lmf"): 5, ("E88", "lmq"): 5, ("E88", "hmf"): 5, ("E88", "hmq"): 5, ("E88", "hf"): 5,
    ("E88", "lt"): "SHELV", ("E88", "ht"): "SHELV",
    ("E84", "lf"): "60", ("E84", "mf"): "1k6", ("E84", "hf"): "12k",
    ("F110", "peq"): 1, ("F110", "shv"): 1, ("F110", "lmf"): 5, ("F110", "lmq"): 5, ("F110", "hmf"): 5,
    ("F110", "hmq"): 5, ("F110", "lf"): "95", ("F110", "hf"): "10k",
    ("PULSAR", "eq1"): 1, ("PULSAR", "1lf"): "60", ("PULSAR", "1hw"): 5, ("PULSAR", "1hf"): "8k",
    ("PULSAR", "1hattf"): "10k", ("PULSAR", "eq5"): 1, ("PULSAR", "5lf"): "500", ("PULSAR", "5mf"): "1k5",
    ("PULSAR", "5hf"): "3k",
    ("MACH4", "airm"): "10k",
    ("TILT", "tilt"): 0, ("MAX", "low"): 0, ("MAX", "proc"): 0, ("AP1", "f"): 1000, ("AP2", "f"): 1000, ("AP2", "q"): 1,
}
STD_CH_FREQ = {"l": 100, "1": 250, "2": 630, "3": 1600, "4": 4000, "h": 10000}
STD_BUS_FREQ = {"l": 80, "1": 160, "2": 315, "3": 630, "4": 1250, "5": 2500, "6": 5000, "h": 12000}


def items_of(r):
    return [i["item"] if isinstance(i, dict) else i for i in r.get("items", [])]


def default_for(model, pid, r, std_freqs):
    t = r["type"]
    key = (model, pid)
    items = items_of(r)
    if model == "STD":
        band = pid[0] if pid[0] in "lh" else pid[0]
        if pid.endswith("g") or pid == "tilt":
            return 0.0
        if pid.endswith("f"):
            return float(std_freqs.get(band, 1000))
        if pid.endswith("eq"):
            return float(items.index("SHV"))
        if pid.endswith("q"):
            return 0.7 if band in "lh" else 1.0
    if model.startswith("FX:"):
        key = (model[3:], pid)
        d = fx_default(model[3:], pid)
        if d is None and key in DEFAULTS:
            d = DEFAULTS[key]
        if d is not None:
            if items:
                if isinstance(d, str):
                    return float(items.index(d))
                try:
                    vals = [float(x) for x in items]
                except ValueError:
                    return float(int(d))  # an index into named items
                return float(min(range(len(vals)), key=lambda i: abs(vals[i] - d)))
            return float(d)
        if t == "fader level":
            return 0.0
    if key in DEFAULTS:
        d = DEFAULTS[key]
        if items:
            if isinstance(d, str):
                return float(items.index(d))
            # numeric default for a float enum: nearest item
            vals = [float(x) for x in items]
            return float(min(range(len(vals)), key=lambda i: abs(vals[i] - d)))
        return float(d)
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
    # The sweep carries float32 artefacts (1000.0001220703125, 1.100000023841858).
    x = round(float(x), 4)
    if abs(x - round(x)) < 1e-3 and abs(x) < 1e7:
        return f"{round(x)}.0"
    return repr(x)


def emit_params(prefix, model, std_freqs=None, defaults_key=None):
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
        d = default_for(defaults_key or model, pid, r, std_freqs or {})
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
print("// Parameter names, ranges and choices describe the console; defaults are ours.")
print()
print("use super::{ModelCategory, ModelSpec, ParamKind, ParamSpec};")
print()


def table(name, prefix, models, names, cat_of, std_freqs=None):
    print(f"pub static {name}: &[ModelSpec] = &[")
    for m in models:
        params = emit_params(prefix, m, std_freqs)
        print(f"    ModelSpec {{ token: {rust_str(m)}, name: {rust_str(names.get(m, m))}, category: {cat_of(m)}, params: &[")
        for p in params:
            print(p)
        print("    ] },")
    print("];")
    print()


dyn_models = model_items("/ch/1/gate/mdl")
table("DYN_MODELS", "/ch/1/gate/", dyn_models, DYN_NAMES,
      lambda m: "ModelCategory::" + {"gate": "Gate", "special": "Special"}.get(DYN_KIND.get(m), "Compressor"))
table("EQ_MODELS_CH", "/ch/1/eq/", model_items("/ch/1/eq/mdl"), EQ_NAMES, lambda m: "ModelCategory::Eq", STD_CH_FREQ)
table("EQ_MODELS_BUS", "/bus/1/eq/", model_items("/bus/1/eq/mdl"), EQ_NAMES, lambda m: "ModelCategory::Eq", STD_BUS_FREQ)
table("FILTER_TOOLS", "/ch/1/flt/", model_items("/ch/1/flt/mdl"), TOOL_NAMES, lambda m: "ModelCategory::Filter")

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
    "NONE": "None", "EXT": "External (Plugin)",
    "HALL": "Hall Reverb", "ROOM": "Room Reverb", "CHAMBER": "Chamber Reverb", "PLATE": "Plate Reverb",
    "CONCERT": "Concert Reverb", "AMBI": "Ambience", "V-ROOM": "Vintage Room", "V-REV": "Vintage Reverb",
    "V-PLATE": "Vintage Plate", "GATED": "Gated Reverb", "REVERSE": "Reverse Reverb", "DEL/REV": "Delay Reverb",
    "SHIMMER": "Shimmer Reverb", "SPRING": "Spring Reverb", "DIMCRS": "Dimension Chorus", "CHORUS": "Stereo Chorus",
    "FLANGER": "Stereo Flanger", "ST-DL": "Stereo Delay", "TAP-DL": "Ultratap Delay", "TAPE-DL": "Tape Delay",
    "OILCAN": "Oilcan Delay", "BBD-DL": "BBD Delay", "PITCH": "Stereo Pitch", "D-PITCH": "Dual Pitch",
    "VSS3": "VSS3 Reverb", "BPLATE": "Blue Plate",
    "GEQ": "Graphic EQ", "PIA": "PIA 560 GEQ", "DOUBLE": "Double Vocal", "PCORR": "Pitch Fix",
    "LIMITER": "Precision Limiter", "DE-S2": "2 Band De-Esser", "ENHANCE": "Ultra Enhancer", "EXCITER": "Exciter",
    "P-BASS": "Psycho Bass", "ROTARY": "Rotary Speaker", "PHASER": "Phaser", "PANNER": "Tremolo Panner",
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

_FX_DEFAULTS = {
    "HALL": dict(pdel=20, size=60, dcy=2.0, mult=1.0, damp=8000, lc=80, hc=12000, shp=120, sprd=25, diff=20, mspd=30),
    "ROOM": dict(pdel=10, size=20, dcy=1.0, mult=1.0, damp=9000, lc=80, hc=12000, shp=100, sprd=25, diff=60, spin=40,
                 ecl=0, ecr=0, efl=0, efr=0),
    "CHAMBER": dict(pdel=15, size=30, dcy=1.5, mult=1.0, damp=8000, lc=80, hc=12000, shp=100, sprd=25, diff=70, spin=40,
                    ecl=0, ecr=0, ell=-60, elr=-60),
    "PLATE": dict(pdel=0, size=30, dcy=1.8, mult=1.0, damp=10000, lc=100, hc=14000, att=50, sprd=25, diff=80, spin=40,
                  ecl=0, ecr=0, efl=0, efr=0),
    "CONCERT": dict(pdel=25, size=45, dcy=2.5, mult=1.1, damp=7000, lc=60, hc=11000, shp=25, sprd=30, diff=70, depth=8,
                    rfl=30, rfr=45, rfll=-12, rflr=-12, spin=40, crs=30),
    "AMBI": dict(pdel=0, size=30, dcy=0.6, tail=50, damp=9000, diff=20, mod=30, lc=100, hc=14000),
    "V-ROOM": dict(pdel=10, size=20, dcy=1.2, dens=20, erlvl=50, lmult=1.0, hmult=0.7, lc=60, hc=12000, frz=0, erl=20,
                   err=25, add=0, lvl=0),
    "V-REV": dict(pdel=20, dcy=2.0, lmult=1.0, hmult=0.5, mod=30, lc=60, hc=12000, out="FRONT", trans=0),
    "V-PLATE": dict(pdel=10, dcy=2.5, lc=100, col=0),
    "GATED": dict(pdel=0, att=5, dcy=0.4, dens=30, diff=20, sprd=50, lc=100, hsf=8000, hsg=-6),
    "REVERSE": dict(pdel=0, rise=30, dcy=0.6, diff=20, sprd=50, lc=100, hsf=8000, hsg=-6),
    "DEL/REV": dict(time=375, feed=30, fhc=6000, dly=50, d2r=30, pdel=10, size=50, dcy=1.8, damp=8000, rlc=100, i2r=80),
    "SHIMMER": dict(pdel=20, size=40, dcy=4.0, lc=80, hc=5000, damp=50, shim=50, shine=50),
    "SPRING": dict(dcy=2.5, dens=15, low=25, high=25),
    "DIMCRS": dict(sw1=0, sw2=1, sw3=0, sw4=0, drysw=1, **{"in": "STEREO"}),
    "CHORUS": dict(lc=80, hc=12000, wave=50, phase=90, mix=50, dlyl=15, dlyr=20, depl=40, depr=40, sprd=50, spd=0.6),
    "FLANGER": dict(lc=20, hc=20000, flc=20, fhc=20000, mix=50, dlyl=2, dlyr=3, depl=60, depr=60, phase=90, spd=0.3,
                    feed=50),
    "ST-DL": dict(time=375, mode="ST", fact="1", pat="1:1", offs=0, feed=30, flc=100, fhc=8000, lc=80, hc=12000),
    "TAP-DL": dict(time=300, rep=6, slp=-6, fact="1", pdel=0, mode="SPREAD", wid=50, diff=20, lc=80, hc=10000),
    "TAPE-DL": dict(time=300, sust=40, drv=30, wf=30),
    "OILCAN": dict(time=5, sust=4, wb=5, tone=5),
    "BBD-DL": dict(dly=40, fb=30),
    "PITCH": dict(semi=0, cent=10, dly=10, lc=80, hc=12000, mix=100),
    "D-PITCH": dict(semi1=0, cent1=-8, dly1=15, pan1=-60, lvl1=0, semi2=0, cent2=8, dly2=25, pan2=60, lvl2=0, lc=80,
                    hc=12000),
    "VSS3": dict(preset=0, load=0, erpdly=10, ertype="ROYAL", ersize="MED", erpos="NEAR", erbal=0, erlc=80, ercol=0,
                 erlvl=-6, rvtype="SMOOTH", rvwid="NORMAL", rvpdly=20, dcy=2.0, diff=0, rvbal=0, rvlvl=0, ldcy=1.2,
                 lmdcy=1.0, hmdcy=0.8, hdcy=0.6, hsoft=0, lxo=200, mxo=1000, hxo=5000, lshv=80, lsdmp=0, hcut=16000,
                 mtype="A", mrate=0, mwid=50, view=0),
    "BPLATE": dict(pdel=0, size=40, dcy=1.6, mult=1.0, damp=9000, lc=100, hc=14000, xover=200, mdep=10, mspd=30, diff=25),
    "GEQ": dict(type="STD"),
    "PIA": dict(mix=100, g=0),
    "DOUBLE": dict(mode="TIGHT", mix=50, sprd=50),
    "PCORR": dict(spd=20, amnt=100, a4=440),
    "LIMITER": dict(gin=0, gout=0, sqz=0, knee=2, again=0, att=0.3, rel=200),
    "DE-S2": dict(lo=10, hi=20, los=10, his=20, gdr="f", mode="st"),
    "ENHANCE": dict(stlvl=0, lmf=0, mlvl=0, st=0, m=0, bass=30, mid=30, high=30, g=0, solo=0, bassf=25, midq=25, highf=25),
    "EXCITER": dict(tune=4000, peak=30, zero=30, tmb=0, hrm=40, mix=30, solo=0),
    "P-BASS": dict(int=-6, bass=-20, xf=80, solo=0),
    "ROTARY": dict(sw="SLOW", lo=0.7, hi=6.5, bal=0, mix=100, dist=50, dac=50, hac=50),
    "PHASER": dict(spd=0.5, phase=90, wave=0, range=50, depth=70, emod=0, att=50, hld=200, rel=200, mix=50, stg=6, reso=30),
    "PANNER": dict(att=50, hld=200, rel=200, espd=0, edep=0, spd=2.0, phase=180, wave=0, depth=70),
    "TAPE": dict(drv=6, spd=15, low=1, hi=1, out=0),
    "MOOD": dict(fbase=800, filt="LP", slope="24", reso=4, drv=2, env=50, att=20, hld=50, rel=100, mix=100, lfo=0, spd=1.0,
                 phase=0, wave="TRI"),
    "SUB": dict(rng="MID", oct1=50, oct2=0),
    "RACKAMP": dict(pre=5, buzz=5, punch=5, crunch=5, drive=5, out=5, leq=5, heq=5, cab=1),
    "UKROCK": dict(gain=5, bass=5, mid=5, treb=5, pres=5, mstr=5, out=5, sag=3, cab=1),
    "ANGEL": dict(gain=5, bass=5, mid=5, treb=5, pres=5, mstr=5, out=5, sag=3, cab=1, midb=0, bri=0, bot=0),
    "JAZZC": dict(vol=5, bass=5, mid=5, treb=5, out=5, bri=0, cab=1),
    "DELUXE": dict(vol=5, bass=5, treb=5, out=5, sag=3, cab=1),
    "BODY": dict(body=50),
    "SOUL": dict(mix=100), "E88": dict(mix=100), "E84": dict(mix=100, g=0), "F110": dict(mix=100, g=0),
    "PULSAR": dict(mix=100), "MACH4": dict(mix=100, again=0),
    "C5-CMB": dict(thr=-20, gain=0, ratio=1.5, slope="24", bandsel=1, att=5, rel=200, arel=1, sbc=5, sbcon=0, mix=100),
    "SUB-M": dict(mix=50, freq=55, bd1=50, bd2=0, bd3=0, bd4=0, bd5=0),
    "V-IMG": dict(wid=0, st=50, g=0, mode="VELVET", deep=0),
    "SPKMAN": dict(hpf=20, hptype="FLAT", lpf=20000, lptype="FLAT", tiltf=1000, tiltg=0, phase=0, invert=0, dist=0, pos=0,
                   dyneq=0, deqthr=-20, deqratio=2, deqatt=10, deqrel=200, deqfilt="BP", deqg=0, deqf=1000, deqq=1,
                   deqmode="high", lim=0, limthr=-6, limrms=0, limrel=200),
    "*EVEN*": dict(g_thr=-30, g_hyst=5, g_range=40, g_rel=300, g_fast=0, g_m40=0, g_on=0, eq_on=1, lf=5, lg=0, lq="LOW",
                   lt="SHELV", lmf=5, lmg=0, lmq=5, hmf=5, hmg=0, hmq=5, hf=5, hg=0, hq="LOW", ht="SHELV", mix=100,
                   d_lon=0, d_lthr=-2, d_lrec="100", d_lfast=0, d_con=1, d_cthr=-20, d_ratio=3, d_crec="400", d_cfast=0,
                   d_gain=0),
    "*SOUL*": dict(g_thr=-30, g_range=30, g_hld=50, g_rel=300, g_fast=0, g_mode="GATE", g_on=0, eq_on=1, lf=5, lg=0, lmf=5,
                   lmf3=0, lmq=5, lmg=0, hmf=5, hmf3=0, hmq=5, hmg=0, hf=5, hg=0, mix=100, d_on=1, d_thr=0, d_ratio=2,
                   d_fast=0, d_rel=300, d_peak=0),
    "*VINTAGE*": {"d_in": -24, "d_out": -24, "d_att": 4, "d_rel": 4, "d_ratio": "4", "d_on": 1, "eq1": 1, "1lb": 0,
                  "1latt": 0, "1lf": "60", "1hw": 5, "1hb": 0, "1hf": "8k", "1hatt": 0, "1hattf": "10k", "eq5": 1,
                  "5lb": 0, "5lf": "500", "5md": 0, "5mf": "1k5", "5hb": 0, "5hf": "3k", "l_ingain": 40, "l_peak": 30,
                  "l_mode": "COMP", "l_on": 0},
    "*BUS*": dict(w_drv=50, w_hrm=0, w_col=0, w_trim=0, w_mix=100, w_on=0, eq_on=1, g=0, lf="60", lg=0, mf="1k6", mg=0,
                  mq="LOW", hf="12k", hg=0, mix=100, d_thr=-16, d_ratio=4, d_att=10, d_rel="AUTO", d_gain=0, d_on=1),
    "*MASTER*": {"t_drv": 0, "t_spd": 15, "t_low": 0, "t_hi": 0, "t_on": 0, "airm": "10k", "eq_on": 1, "e_bassf": 25,
                 "e_midq": 25, "e_highf": 25, "e_on": 0, "l_gin": 0, "l_gout": 0, "l_sqz": 0, "l_knee": 2, "l_again": 0,
                 "l_att": 0.3, "l_rel": 200, "l_on": 1},
}
_DEQ3_FREQ = {"1": 200, "2": 2000, "3": 8000}


def fx_default(model, pid):
    d = _FX_DEFAULTS.get(model, {})
    if pid in d:
        return d[pid]
    if model == "DEQ3":
        band, _, what = pid.partition("-")
        return {"thr": -20, "ratio": 2, "att": 10, "rel": 200, "filt": "BP", "g": -6, "f": _DEQ3_FREQ.get(band, 1000),
                "q": 1.5, "mode": "high"}.get(what)
    if model == "C5-CMB":
        what = pid.split("_")[0]
        return {"thr": 0, "gain": 0, "byp": 0, "width": 0}.get(what)
    if model == "PCORR" and pid.startswith("sw_"):
        return 1
    return None


def fx_category(m):
    if m == "NONE":
        return "ModelCategory::FxNone"
    if m == "EXT":
        return "ModelCategory::FxExternal"
    if m.startswith("*"):
        return "ModelCategory::FxChannel"
    return "ModelCategory::FxPremium" if m in FX_PREMIUM else "ModelCategory::FxStandard"


fx_models = model_items("/fx/1/mdl")
print("/// The FX rack's devices, console order: NONE, EXT (here: a VST3/AU/LV2 plugin), the")
print("/// premium devices (FX slots 1-8 only), the standard devices and the channel strips.")
print("pub static FX_MODELS: &[ModelSpec] = &[")
for m in fx_models:
    # EXT's own parameters route a hardware insert; here EXT is a plugin and has none.
    params = [] if m in ("NONE", "EXT") else emit_params("/fx/1/", m, defaults_key="FX:" + m)
    print(f"    ModelSpec {{ token: {rust_str(m)}, name: {rust_str(FX_NAMES.get(m, m))}, category: {fx_category(m)}, params: &[")
    for p in params:
        print(p)
    print("    ] },")
print("];")
print()
