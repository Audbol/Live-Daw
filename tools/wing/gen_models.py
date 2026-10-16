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

DYN_NAMES = {
    "GATE": "Gate/Expander", "DUCK": "Ducker", "E88": "Even 88 Gate", "9000G": "Soul 9000 Gate",
    "D241G": "Draw 241 Gate", "DS902": "De-Esser 902", "DEQ": "Dynamic EQ", "DEQ2": "Dual Dynamic EQ",
    "WAVE": "Wave Designer", "PSE": "Source Extractor", "CMB": "Extractor + Leveler", "RIDE": "Auto Rider",
    "WARM": "Warm Harmonics", "COMP": "Compressor", "EXP": "Expander", "B160": "160 VCA Comp",
    "B560": "560 Easy Comp", "D241C": "Draw 241 Comp/Lim", "ECL33": "ECL 33 Comp/Lim", "9000C": "Soul 9000 Comp",
    "SBUS": "Soul Bus Comp", "RED3": "Red 3 Comp", "76LA": "76 Limiting Amp", "LA": "LA Leveler",
    "F670": "Fair Kid 670", "BLISS": "Eternal Bliss", "NSTR": "No-Stressor", "2250": "2250 Comp",
    "L100": "L100 Leveler", "E88C": "Even 88 Comp", "LMT": "LMT Transient Comp", "ONEC": "One-Knob Comp",
}
DYN_KIND = {  # broad category, used to group the model menu like the console
    "GATE": "gate", "DUCK": "gate", "E88": "gate", "9000G": "gate", "D241G": "gate", "PSE": "gate",
    "CMB": "special", "DS902": "special", "DEQ": "special", "DEQ2": "special", "WAVE": "special",
    "RIDE": "special", "WARM": "special",
}
EQ_NAMES = {
    "STD": "WING EQ", "SOUL": "Soul Analogue EQ", "E88": "Even 88 EQ", "E84": "Even 84 EQ",
    "F110": "Fortissimo 110 EQ", "PULSAR": "Pulsar EQ", "MACH4": "Mach EQ4", "PIA": "PIA Graphic EQ",
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
    ("F670", "in"): -10, ("F670", "thr"): 5, ("F670", "time"): 2, ("F670", "bias"): 0.5,
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


def emit_params(prefix, model, std_freqs=None):
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
        elif kind in ("Toggle", "Int"):
            lo, hi = r.get("minint", 0), r.get("maxint", 1)
            steps = hi - lo + 1
        else:
            lo, hi, steps = r["minfloat"], r["maxfloat"], r.get("steps", 101)
        d = default_for(model, pid, r, std_freqs or {})
        unit = r.get("unit", "")
        unit = {"meters": "m", "seconds": "s"}.get(unit, unit)
        if kind == "Choice":
            k = "ParamKind::Choice(&[" + ", ".join(rust_str(i) for i in items) + "])"
        elif kind == "Values":
            k = "ParamKind::Values(&[" + ", ".join(fnum(v) for v in items) + "])"
        else:
            k = "ParamKind::" + kind
        out.append(
            f"        ParamSpec {{ id: {rust_str(pid)}, label: {rust_str(r.get('longname', pid))}, unit: {rust_str(unit)}, "
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
