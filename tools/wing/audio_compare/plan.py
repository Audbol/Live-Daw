#!/usr/bin/env python3
"""The test plan: which settings to compare, with which signal and analysis.

This file is the source; `python3 plan.py` writes test_plan.json (read by the
other scripts) and test_plan.md (for the person at the console). Settings are
WING parameter paths with values as the console shows them (numbers in the
parameter's unit, choices as text), the same paths Sidecar's render tool and
the WING's OSC remote take.
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_signals  # noqa: E402

# Applied on both sides before every test: channel 1 flat into Main 1, Main 1 flat.
COMMON = {
    "/ch/1/in/set/trim": 0.0, "/ch/1/in/set/bal": 0.0, "/ch/1/in/set/inv": 0, "/ch/1/in/set/dlyon": 0,
    "/ch/1/flt/lc": 0, "/ch/1/flt/hc": 0, "/ch/1/flt/tf": 0,
    "/ch/1/proc": "GEDI",
    "/ch/1/gate/on": 0, "/ch/1/gate/mix": 100.0, "/ch/1/gate/gain": 0.0,
    "/ch/1/gatesc/type": "OFF", "/ch/1/gatesc/src": "SELF",
    "/ch/1/eq/on": 0, "/ch/1/eq/mix": 100.0,
    # Every WING EQ band flat and the outer bands shelves again, so no band
    # of an earlier test is left in (a test sets only its own bands).
    "/ch/1/eq/mdl": "STD", "/ch/1/eq/leq": "SHV", "/ch/1/eq/heq": "SHV",
    "/ch/1/eq/lg": 0.0, "/ch/1/eq/1g": 0.0, "/ch/1/eq/2g": 0.0, "/ch/1/eq/3g": 0.0, "/ch/1/eq/4g": 0.0,
    "/ch/1/eq/hg": 0.0,
    "/ch/1/dyn/on": 0, "/ch/1/dyn/mix": 100.0, "/ch/1/dyn/gain": 0.0,
    "/ch/1/dynsc/type": "OFF", "/ch/1/dynsc/src": "SELF", "/ch/1/dynxo/type": "OFF",
    "/ch/1/preins/on": 0, "/ch/1/postins/on": 0,
    "/ch/1/mute": 0, "/ch/1/fdr": 0.0, "/ch/1/pan": 0.0, "/ch/1/wid": 100.0,
    "/ch/1/main/1/on": 1, "/ch/1/main/1/lvl": 0.0, "/ch/1/main/1/pre": 0,
    "/main/1/mute": 0, "/main/1/fdr": 0.0, "/main/1/pan": 0.0, "/main/1/wid": 100.0, "/main/1/busmono": 0,
    "/main/1/eq/on": 0, "/main/1/dyn/on": 0, "/main/1/preins/on": 0, "/main/1/postins/on": 0,
    "/main/1/dly/on": 0,
}

# The WING's patch for the session (Sidecar's render patches the same itself):
# the USB player into channel 1, Main 1 L/R and the raw player signal to the
# 4-track USB recorder.
WING_ONLY = {
    "/ch/1/in/conn/grp": "PLAY", "/ch/1/in/conn/in": 1,
    "/io/in/PLAY/1/mode": "M", "/io/in/PLAY/1/mute": 0, "/io/in/PLAY/1/pol": 0,
    "/io/out/REC/1/grp": "MAIN", "/io/out/REC/1/in": 1,
    "/io/out/REC/2/grp": "MAIN", "/io/out/REC/2/in": 2,
    "/io/out/REC/3/grp": "PLAY", "/io/out/REC/3/in": 1,
    "/io/out/REC/4/grp": "OFF",
    "/rec/channels": "4", "/rec/resolution": "24",
}

EQ = {"/ch/1/eq/on": 1, "/ch/1/eq/mdl": "STD"}
COMP = {"/ch/1/dyn/on": 1, "/ch/1/dyn/mdl": "COMP"}
GATE = {"/ch/1/gate/on": 1, "/ch/1/gate/mdl": "GATE"}


def fx(model, **params):
    s = {"/ch/1/preins/on": 1, "/ch/1/preins/ins": "FX1", "/fx/1/mdl": model, "/fx/1/fxmix": 100.0}
    s.update({f"/fx/1/{k}": v for k, v in params.items()})
    return s


def dyn(model, **params):
    s = {"/ch/1/dyn/on": 1, "/ch/1/dyn/mdl": model}
    s.update({f"/ch/1/dyn/{k}": v for k, v in params.items()})
    return s


def eqm(model, **params):
    s = {"/ch/1/eq/on": 1, "/ch/1/eq/mdl": model}
    s.update({f"/ch/1/eq/{k}": v for k, v in params.items()})
    return s


def comp(thr, ratio, knee, det, att, hld, rel, env, auto, **extra):
    s = dict(COMP)
    s.update({"/ch/1/dyn/thr": float(thr), "/ch/1/dyn/ratio": float(ratio), "/ch/1/dyn/knee": int(knee),
              "/ch/1/dyn/det": det, "/ch/1/dyn/att": float(att), "/ch/1/dyn/hld": float(hld),
              "/ch/1/dyn/rel": float(rel), "/ch/1/dyn/env": env, "/ch/1/dyn/auto": int(auto)})
    s.update(extra)
    return s


def gate(thr, ratio, rng, att, hld, rel, acc=0.0, **extra):
    s = dict(GATE)
    s.update({"/ch/1/gate/thr": float(thr), "/ch/1/gate/ratio": ratio, "/ch/1/gate/range": float(rng),
              "/ch/1/gate/att": float(att), "/ch/1/gate/hld": float(hld), "/ch/1/gate/rel": float(rel),
              "/ch/1/gate/acc": float(acc)})
    s.update(extra)
    return s


def peq(band, f, g, q):
    return {f"/ch/1/eq/{band}f": float(f), f"/ch/1/eq/{band}g": float(g), f"/ch/1/eq/{band}q": float(q)}


def merge(*ds):
    out = {}
    for d in ds:
        out.update(d)
    return out


KEY_COMP = comp(-30, 4.0, 0, "PEAK", 5, 10, 100, "LIN", 0)

# name, priority, signal kind, analysis, title, settings, notes
TESTS = [
    ("base_unity", 1, "lin", "lin", "Channel at unity (baseline: level, pan law, flatness, latency)", {}, ""),
    # ---- WING EQ (STD) ----------------------------------------------------------
    ("eq_peq_600_p6_q1", 1, "lin", "lin", "WING EQ band 2 PEQ 600 Hz +6 dB Q 1",
     merge(EQ, peq(2, 600, 6, 1.0)), ""),
    ("eq_peq_1k5_m9_q4", 1, "lin", "lin", "WING EQ band 3 PEQ 1.5 kHz -9 dB Q 4",
     merge(EQ, peq(3, 1500, -9, 4.0)), ""),
    ("eq_peq_100_p6_q2", 1, "lin", "lin", "WING EQ band 1 PEQ 100 Hz +6 dB Q 2",
     merge(EQ, peq(1, 100, 6, 2.0)), ""),
    ("eq_peq_4k_p12_q05", 1, "lin", "lin", "WING EQ band 4 PEQ 4 kHz +12 dB Q 0.5 (wide, high gain)",
     merge(EQ, peq(4, 4000, 12, 0.5)), ""),
    ("eq_peq_12k_p6_q2", 1, "lin", "lin", "WING EQ band 4 PEQ 12 kHz +6 dB Q 2 (near Nyquist: bell shape)",
     merge(EQ, peq(4, 12000, 6, 2.0)), "Shows whether a bell keeps its shape near 24 kHz (cramping)."),
    ("eq_lshv_80_p6", 1, "lin", "lin", "WING EQ LOW shelf 80 Hz +6 dB Q 1",
     merge(EQ, {"/ch/1/eq/leq": "SHV"}, peq("l", 80, 6, 1.0)), ""),
    ("eq_lshv_250_m6_q2", 1, "lin", "lin", "WING EQ LOW shelf 250 Hz -6 dB Q 2 (Q on a shelf)",
     merge(EQ, {"/ch/1/eq/leq": "SHV"}, peq("l", 250, -6, 2.0)), ""),
    ("eq_hshv_8k_p6", 1, "lin", "lin", "WING EQ HIGH shelf 8 kHz +6 dB Q 1",
     merge(EQ, {"/ch/1/eq/heq": "SHV"}, peq("h", 8000, 6, 1.0)), ""),
    ("eq_hshv_4k_m9_q05", 1, "lin", "lin", "WING EQ HIGH shelf 4 kHz -9 dB Q 0.5",
     merge(EQ, {"/ch/1/eq/heq": "SHV"}, peq("h", 4000, -9, 0.5)), ""),
    ("eq_lpeq_hpeq", 1, "lin", "lin", "WING EQ LOW and HIGH bands as PEQ (60 Hz +6 Q 2, 15 kHz -6 Q 1)",
     merge(EQ, {"/ch/1/eq/leq": "PEQ", "/ch/1/eq/heq": "PEQ"}, peq("l", 60, 6, 2.0), peq("h", 15000, -6, 1.0)), ""),
    ("eq_combined", 1, "lin", "lin", "WING EQ, all six bands (a vocal curve)",
     merge(EQ, {"/ch/1/eq/leq": "SHV", "/ch/1/eq/heq": "SHV"}, peq("l", 100, -3, 1.0), peq(1, 250, -4, 1.5),
           peq(2, 800, 2, 1.0), peq(3, 3000, 3, 2.0), peq(4, 6000, -2, 3.0), peq("h", 12000, 4, 1.0)), ""),
    # ---- FILTER --------------------------------------------------------------------
    ("flt_lc6_100", 1, "lin", "lin", "LOW CUT 100 Hz 6 dB/oct",
     {"/ch/1/flt/lc": 1, "/ch/1/flt/lcf": 100.0, "/ch/1/flt/lcs": "6"}, ""),
    ("flt_lc12_100", 1, "lin", "lin", "LOW CUT 100 Hz 12 dB/oct",
     {"/ch/1/flt/lc": 1, "/ch/1/flt/lcf": 100.0, "/ch/1/flt/lcs": "12"}, ""),
    ("flt_lc18_100", 1, "lin", "lin", "LOW CUT 100 Hz 18 dB/oct",
     {"/ch/1/flt/lc": 1, "/ch/1/flt/lcf": 100.0, "/ch/1/flt/lcs": "18"}, ""),
    ("flt_lc24_100", 1, "lin", "lin", "LOW CUT 100 Hz 24 dB/oct",
     {"/ch/1/flt/lc": 1, "/ch/1/flt/lcf": 100.0, "/ch/1/flt/lcs": "24"}, ""),
    ("flt_hc6_5k", 1, "lin", "lin", "HIGH CUT 5 kHz 6 dB/oct",
     {"/ch/1/flt/hc": 1, "/ch/1/flt/hcf": 5000.0, "/ch/1/flt/hcs": "6"}, ""),
    ("flt_hc12_5k", 1, "lin", "lin", "HIGH CUT 5 kHz 12 dB/oct",
     {"/ch/1/flt/hc": 1, "/ch/1/flt/hcf": 5000.0, "/ch/1/flt/hcs": "12"}, ""),
    ("flt_tilt_p3", 1, "lin", "lin", "FILTER tool TILT +3 dB",
     {"/ch/1/flt/tf": 1, "/ch/1/flt/mdl": "TILT", "/ch/1/flt/tilt": 3.0}, ""),
    # ---- WING COMP -----------------------------------------------------------------
    ("comp_default", 1, "dyn", "dyn", "WING COMP as factory set but THR -30 (3:1, KNEE 3, RMS, AUTO on)",
     comp(-30, 3.0, 3, "RMS", 50, 20, 150, "LOG", 1), ""),
    ("comp_r4_k0_peak", 1, "dyn", "dyn", "WING COMP -30 dB 4:1 KNEE 0 PEAK, 5 / 10 / 100 ms, LIN, AUTO off",
     comp(-30, 4.0, 0, "PEAK", 5, 10, 100, "LIN", 0), ""),
    ("comp_r4_k0_peak_auto", 1, "dyn", "dyn", "As comp_r4_k0_peak with AUTO on",
     comp(-30, 4.0, 0, "PEAK", 5, 10, 100, "LIN", 1), "AUTO ENV on/off: compare with comp_r4_k0_peak."),
    ("comp_r10_k5_rms", 1, "dyn", "dyn", "WING COMP -20 dB 10:1 KNEE 5 RMS, 20 / 20 / 300 ms, LOG, AUTO off",
     comp(-20, 10.0, 5, "RMS", 20, 20, 300, "LOG", 0), ""),
    ("comp_r2_k2_rms", 1, "dyn", "dyn", "WING COMP -40 dB 2:1 KNEE 2 RMS, 10 / 20 / 200 ms, LOG, AUTO off",
     comp(-40, 2.0, 2, "RMS", 10, 20, 200, "LOG", 0), ""),
    ("comp_lim_r100", 1, "dyn", "dyn", "WING COMP as a limiter: -20 dB 100:1 KNEE 0 PEAK, 0 / 1 / 50 ms",
     comp(-20, 100.0, 0, "PEAK", 0, 1, 50, "LIN", 0), ""),
    # ---- WING GATE -----------------------------------------------------------------
    ("gate_default_m40", 1, "dyn", "dyn", "WING GATE factory set: -40 dB 1:3 RANGE 40, 10 / 10 / 200 ms",
     gate(-40, "1:3", 40, 10, 10, 200), ""),
    ("gate_gate_m30_r60", 1, "dyn", "dyn", "WING GATE -30 dB ratio GATE RANGE 60, 0 / 50 / 100 ms",
     gate(-30, "gate", 60, 0, 50, 100), ""),
    ("gate_r12_m35_r20", 1, "dyn", "dyn", "WING GATE -35 dB 1:2 RANGE 20, 5 / 20 / 500 ms",
     gate(-35, "1:2", 20, 5, 20, 500), ""),
    # ---- priority 2: more EQ and filter settings ------------------------------------
    ("eq_mix_50", 2, "lin", "lin", "WING EQ band 2 1 kHz +12 dB Q 1 at EQ MIX 50 %",
     merge(EQ, peq(2, 1000, 12, 1.0), {"/ch/1/eq/mix": 50.0}), ""),
    ("flt_lc24_400", 2, "lin", "lin", "LOW CUT 400 Hz 24 dB/oct",
     {"/ch/1/flt/lc": 1, "/ch/1/flt/lcf": 400.0, "/ch/1/flt/lcs": "24"}, ""),
    ("flt_tilt_m6", 2, "lin", "lin", "FILTER tool TILT -6 dB",
     {"/ch/1/flt/tf": 1, "/ch/1/flt/mdl": "TILT", "/ch/1/flt/tilt": -6.0}, ""),
    ("flt_ap1_1k", 2, "lin", "lin", "FILTER tool AP1 1 kHz (phase only)",
     {"/ch/1/flt/tf": 1, "/ch/1/flt/mdl": "AP1", "/ch/1/flt/f": 1000.0}, ""),
    ("flt_ap2_1k_q2", 2, "lin", "lin", "FILTER tool AP2 1 kHz Q 2 (phase only)",
     {"/ch/1/flt/tf": 1, "/ch/1/flt/mdl": "AP2", "/ch/1/flt/f": 1000.0, "/ch/1/flt/q": 2.0}, ""),
    # ---- priority 2: dynamics extras, key filter, crossover --------------------------
    ("comp_mix_gain", 2, "dyn", "dyn", "WING COMP 4:1 with MIX 50 % and GAIN +6 dB (parallel)",
     comp(-30, 4.0, 2, "PEAK", 5, 10, 100, "LIN", 0, **{"/ch/1/dyn/mix": 50.0, "/ch/1/dyn/gain": 6.0}), ""),
    ("gate_r14_acc50", 2, "dyn", "dyn", "WING GATE -45 dB 1:4 RANGE 30 ACCENT 50 %, 20 / 100 / 1000 ms",
     gate(-45, "1:4", 30, 20, 100, 1000, 50), ""),
    ("comp_key_hp1k", 2, "key", "key", "WING COMP with KEY FILTER HP12 1 kHz (tone steps at -10 dBFS)",
     merge(KEY_COMP, {"/ch/1/dynsc/type": "HP12", "/ch/1/dynsc/f": 1000.0}), ""),
    ("comp_key_bp2k", 2, "key", "key", "WING COMP with KEY FILTER BP 2 kHz Q 2",
     merge(KEY_COMP, {"/ch/1/dynsc/type": "BP", "/ch/1/dynsc/f": 2000.0, "/ch/1/dynsc/q": 2.0}), ""),
    ("gate_key_lp500", 2, "key", "key", "WING GATE -30 dB with KEY FILTER LP12 500 Hz",
     merge(gate(-30, "gate", 40, 0, 10, 100), {"/ch/1/gatesc/type": "LP12", "/ch/1/gatesc/f": 500.0}), ""),
    ("comp_xo_hi2k", 2, "key", "key", "WING COMP in XOVER mode HI 2 kHz, DEPTH 12 dB",
     merge(KEY_COMP, {"/ch/1/dynxo/type": "HI", "/ch/1/dynxo/f": 2000.0, "/ch/1/dynxo/depth": 12.0}), ""),
    ("comp_xo_lo200", 2, "key", "key", "WING COMP in XOVER mode LO 200 Hz, DEPTH 30 dB",
     merge(KEY_COMP, {"/ch/1/dynxo/type": "LO", "/ch/1/dynxo/f": 200.0, "/ch/1/dynxo/depth": 30.0}), ""),
    # ---- priority 2: other dynamics models (most used first) -------------------------
    ("dyn_exp", 2, "dyn", "dyn", "WING EXPANDER -40 dB 2:1 KNEE 2 PEAK, 5 / 20 / 200 ms",
     dyn("EXP", thr=-40.0, ratio=2.0, knee=2, det="PEAK", att=5.0, hld=20.0, rel=200.0, env="LIN", auto=0), ""),
    ("dyn_76la", 2, "dyn", "dyn", "76 LIMITER AMP: INPUT -24, OUTPUT -24, ATTACK 4, RELEASE 4, RATIO 4",
     dyn("76LA", **{"in": -24.0, "out": -24.0, "att": 4.0, "rel": 4.0, "ratio": "4"}), ""),
    ("dyn_la", 2, "dyn", "dyn", "LA LEVELER: GAIN 50, PEAK 50, COMP",
     dyn("LA", ingain=50.0, peak=50.0, mode="COMP"), ""),
    ("dyn_sbus", 2, "dyn", "dyn", "SOUL BUS COMP: -20 dB 4:1, ATTACK 10 ms, RELEASE 0.4 s",
     dyn("SBUS", thr=-20.0, ratio=4.0, att=10.0, rel="0.4"), ""),
    ("dyn_9000c", 2, "dyn", "dyn", "SOUL 9000 comp: THR -10, 4:1, RELEASE 300 ms, PEAK off, FAST off",
     dyn("9000C", thr=-10.0, ratio=4.0, fast=0, rel=300.0, peak=0), ""),
    ("dyn_b160", 2, "dyn", "dyn", "BDX 160: THR 0.1, COMP 4.0",
     dyn("B160", thr=0.1, comp=4.0), ""),
    ("dyn_deq_1k", 2, "dyn", "dyn", "DYNAMIC EQ: BP 1 kHz Q 2, -30 dB 3:1, 10 / 150 ms, GAIN -12, MODE high",
     dyn("DEQ", thr=-30.0, ratio=3.0, att=10.0, rel=150.0, filt="BP", g=-12.0, f=1000.0, q=2.0, mode="high"),
     "GAIN is the largest change: at 0 the band does nothing."),
    # ---- priority 2: the other channel EQ models -------------------------------------
    ("eqm_soul", 2, "lin", "lin", "SOUL ANALOGUE EQ: LO +3, LM -3, HM +2, HF +3 (frequencies at 5)",
     eqm("SOUL", lf=5.0, lg=3.0, lmf=5.0, lmq=5.0, lmg=-3.0, hmf=5.0, hmq=5.0, hmg=2.0, hf=5.0, hg=3.0), ""),
    ("eqm_e88", 2, "lin", "lin", "EVEN 88 FORMANT EQ: LF shelf +3, LM -3, HM +3, HF shelf +2",
     eqm("E88", lf=5.0, lg=3.0, lq="LOW", lt="SHELV", lmf=5.0, lmg=-3.0, lmq=5.0, hmf=5.0, hmg=3.0, hmq=5.0,
         hf=5.0, hg=2.0, hq="LOW", ht="SHELV"), ""),
    ("eqm_e84", 2, "lin", "lin", "EVEN 84 EQ: LF 110 +3, MID 1k6 +3 LOW Q, HF 12k +3",
     eqm("E84", g=0.0, lf="110", lg=3.0, mf="1k6", mg=3.0, mq="LOW", hf="12k", hg=3.0), ""),
    ("eqm_f110", 2, "lin", "lin", "FORTISSIMO 110 EQ: LF 95 +3, LM -3, HM +3, HF 10k +3",
     eqm("F110", peq=1, lmf=5.0, lmg=-3.0, lmq=5.0, lmf3=0, hmf=5.0, hmg=3.0, hmq=5.0, hmf3=0, shv=1,
         lf="95", lg=3.0, hf="10k", hg=3.0, g=0.0), ""),
    ("eqm_pulsar", 2, "lin", "lin", "PULSAR EQ: LF 60 BOOST 5, HF 8k BOOST 4 WID 5",
     eqm("PULSAR", eq1=1, **{"1lb": 5.0, "1latt": 0.0, "1lf": "60", "1hw": 5.0, "1hb": 4.0, "1hf": "8k",
                            "1hatt": 0.0, "eq5": 0}), ""),
    ("eqm_mach4", 2, "lin", "lin", "MACH EQ4: 40 +2, 160 -2, 2k5 +2, AIR 5 at 10k",
     eqm("MACH4", sub=0.0, **{"40": 2.0, "160": -2.0, "650": 0.0, "2k5": 2.0}, air=5.0, airm="10k", again=0), ""),
    # ---- priority 2: FX on INS 1 (FX slot 1, MIX 100 %) -------------------------------
    ("fx_plate", 2, "fx", "reverb", "PLATE REVERB: PRE DLY 20, SIZE 30, DECAY 1.8 s (others factory)",
     fx("PLATE", pdel=20.0, size=30.0, dcy=1.8, mult=1.0, damp=9700.0, lc=100.0, hc=13800.0, att=50.0,
        sprd=25.0, diff=80.0, spin=40.0, ecl=0.0, ecr=0.0, efl=0.0, efr=0.0), ""),
    ("fx_stdl", 2, "fx", "delay", "STEREO DELAY: 300 ms, ST, FACTOR 1, FEED 40 %, filters open",
     fx("ST-DL", time=300.0, mode="ST", fact="1", pat="1:1", offs=0.0, feed=40.0, flc=20.0, fhc=20000.0,
        lc=20.0, hc=20000.0), ""),
    ("fx_chorus", 2, "mod", "mod", "STEREO CHORUS: MIX 100, 15 / 20 ms, DEPTH 40 %, SPEED 0.6 Hz",
     fx("CHORUS", lc=20.0, hc=20000.0, wave=50.0, phase=90.0, mix=100.0, dlyl=15.0, dlyr=20.0, depl=40.0,
        depr=40.0, sprd=50.0, spd=0.6), ""),
    ("fx_rackamp", 2, "drive", "drive", "RACK AMP: all controls at 5, CAB SIM on",
     fx("RACKAMP", pre=5.0, buzz=5.0, punch=5.0, crunch=5.0, drive=5.0, out=5.0, leq=5.0, heq=5.0, cab=1), ""),
    ("music_strip", 2, "music", "music", "A full channel on music: LC 80, gate, EQ, compressor",
     merge({"/ch/1/flt/lc": 1, "/ch/1/flt/lcf": 80.0, "/ch/1/flt/lcs": "24"},
           gate(-50, "1:3", 20, 5, 50, 200),
           EQ, {"/ch/1/eq/leq": "SHV", "/ch/1/eq/heq": "SHV"}, peq("l", 100, -2, 1.0), peq(2, 400, -3, 1.5),
           peq(3, 3000, 3, 1.0), peq("h", 10000, 3, 1.0),
           comp(-20, 3.0, 3, "RMS", 20, 20, 150, "LOG", 1)), "A null test on program material."),
    # ---- priority 3 ---------------------------------------------------------------------
    ("flt_max", 3, "lin", "lin", "FILTER tool MAXER: LOW CONT 50 %, HIGH PROC 50 %",
     {"/ch/1/flt/tf": 1, "/ch/1/flt/mdl": "MAX", "/ch/1/flt/low": 50.0, "/ch/1/flt/proc": 50.0}, ""),
    ("main_eq_lr24_100", 3, "lin", "lin", "MAIN 1 EQ LOW band LR24 cut at 100 Hz",
     {"/main/1/eq/on": 1, "/main/1/eq/mdl": "STD", "/main/1/eq/leq": "LR24", "/main/1/eq/lf": 100.0}, ""),
    ("main_eq_bw48_8k", 3, "lin", "lin", "MAIN 1 EQ HIGH band BW48 cut at 8 kHz",
     {"/main/1/eq/on": 1, "/main/1/eq/mdl": "STD", "/main/1/eq/heq": "BW48", "/main/1/eq/hf": 8000.0}, ""),
    ("fx_hall", 3, "fx", "reverb", "HALL REVERB: factory values (PRE DLY 20, SIZE 60, DECAY 2.0 s)",
     fx("HALL", pdel=20.0, size=60.0, dcy=2.0, mult=1.0, damp=8100.0, lc=79.0, hc=11500.0, shp=120.0, sprd=25.0,
        diff=20.0, mspd=30.0), ""),
    ("fx_ukrock", 3, "drive", "drive", "UK ROCK AMP: all at 5, SAG 3, CAB SIM on",
     fx("UKROCK", gain=5.0, bass=5.0, mid=5.0, treb=5.0, pres=5.0, mstr=5.0, out=5.0, sag=3.0, cab=1), ""),
    ("dyn_e88_gate", 3, "dyn", "dyn", "EVEN 88 GATE: THR -30, HYST 5, RANGE 40, RELEASE 300 ms",
     dyn("E88", thr=-30.0, hyst=5.0, range=40.0, rel=300.0, fast=0, m40=0), "In the DYNAMICS slot."),
    ("dyn_ds902", 3, "key", "key", "BDX902 DE-ESSER: 5 kHz, RANGE 6, HF",
     dyn("DS902", f=5000.0, range=6.0, mode="HF"), ""),
    ("dyn_ds902_full", 3, "key", "key", "BDX902 DE-ESSER: 3 kHz, RANGE 12, FULL",
     dyn("DS902", f=3000.0, range=12.0, mode="FULL"), "Each tone is all signal: the cut per tone shows the detector's band."),
    ("dyn_deq2_shelves", 3, "key", "key",
     "DUAL DYNAMIC EQ: 1 = LP12 200 Hz -9 dB above -40 dB 10:1; 2 = HP12 5 kHz -6 dB above -40 dB 10:1",
     dyn("DEQ2", **{"1-thr": -40.0, "1-ratio": 10.0, "1-att": 10.0, "1-rel": 150.0, "1-filt": "LP12", "1-g": -9.0,
                    "1-f": 200.0, "1-q": 1.0, "1-mode": "high",
                    "2-thr": -40.0, "2-ratio": 10.0, "2-att": 10.0, "2-rel": 150.0, "2-filt": "HP12", "2-g": -6.0,
                    "2-f": 5000.0, "2-q": 1.0, "2-mode": "high"}),
     "The shelves' shape near FREQ (no rise above 0 dB) and the two bands apart."),
    ("dyn_deq_flat", 3, "dyn", "dyn", "DYNAMIC EQ: FILTER OFF (whole signal), -30 dB 3:1, 10 / 150 ms, GAIN -12, MODE high",
     dyn("DEQ", thr=-30.0, ratio=3.0, att=10.0, rel=150.0, filt="OFF", g=-12.0, f=1000.0, q=2.0, mode="high"),
     "ATTACK / RELEASE without a band filter's ringing (with BP the first ms after a level drop are the filter's)."),
]

# Tolerances for the pass/fail columns (analyze.py).
TOLERANCES = {
    "mag_db": 0.25,            # magnitude where both are within 20 dB of their peak
    "mag_db_skirt": 1.0,       # magnitude 20-40 dB below the peak
    "phase_deg": 10.0,         # phase after removing a pure delay, where the magnitude is within 20 dB
    "level_db": 0.25,          # 1 kHz level, pan law
    "static_db": 0.5,          # compressor / gate static curve per step
    "static_db_closed": 3.0,   # steps where the output is more than 40 dB down (a closed gate)
    "time_rel": 0.2,           # attack / hold / release times (relative)
    "time_abs_ms": 2.0,        # ... or absolute, whichever is larger
    "key_db": 0.5,             # key filter / crossover: gain per frequency
    "rt_rel": 0.1,             # reverb EDT / T30 per band
    "c80_db": 1.0,             # reverb C80 per band
    "band_db": 1.0,            # reverb, modulation, drive: level per band
    "delay_ms": 0.5,           # delay echo times
    "echo_db": 0.5,            # delay echo levels
    "lfo_rel": 0.05,           # modulation rate
    "depth_rel": 0.2,          # modulation depth
    "thd_db": 3.0,             # harmonics (where above -80 dB)
    "spectrum_db": 0.5,        # music / noise 1/3-octave spectrum
}

# ---- where each control is on the console ----------------------------------------

MODEL_LABELS = {
    "GATE": {"thr": "THR", "range": "RANGE", "att": "ATTACK", "hld": "HOLD", "rel": "RELEASE", "acc": "ACCENT",
             "ratio": "RATIO"},
    "COMP": {"thr": "THR", "ratio": "RATIO", "knee": "KNEE", "det": "DETECTOR", "att": "ATTACK", "hld": "HOLD",
             "rel": "RELEASE", "env": "ENVELOPE", "auto": "AUTO"},
    "EXP": {"thr": "THR", "ratio": "RATIO", "knee": "KNEE", "det": "DETECTOR", "att": "ATTACK", "hld": "HOLD",
            "rel": "RELEASE", "env": "ENVELOPE", "auto": "AUTO"},
    "76LA": {"in": "INPUT", "out": "OUTPUT", "att": "ATTACK", "rel": "RELEASE", "ratio": "RATIO"},
    "LA": {"ingain": "GAIN", "peak": "PEAK", "mode": "MODE"},
    "SBUS": {"thr": "THR", "ratio": "RATIO", "att": "ATTACK", "rel": "RELEASE"},
    "9000C": {"thr": "THR", "ratio": "RATIO", "fast": "FAST ATT", "rel": "RELEASE", "peak": "PEAK"},
    "B160": {"thr": "THR", "comp": "COMP"},
    "DEQ": {"thr": "THR", "ratio": "RATIO", "att": "ATTACK", "rel": "RELEASE", "filt": "FILTER", "g": "GAIN",
            "f": "FREQ", "q": "Q", "mode": "MODE"},
    "E88": {"thr": "THR", "hyst": "HYST", "range": "RANGE", "rel": "RELEASE", "fast": "FAST", "m40": "THR -40",
            "lf": "LF FREQ", "lg": "LF GAIN", "lq": "LF Q", "lt": "LF TYPE", "lmf": "LM FREQ", "lmg": "LM GAIN",
            "lmq": "LM Q", "hmf": "HM FREQ", "hmg": "HM GAIN", "hmq": "HM Q", "hf": "HF FREQ", "hg": "HF GAIN",
            "hq": "HF Q", "ht": "HF TYPE"},
    "DS902": {"f": "FREQ", "range": "RANGE", "mode": "MODE"},
    "DEQ2": {f"{b}-{k}": f"{b}-{v}" for b in (1, 2) for k, v in (("thr", "THR"), ("ratio", "RATIO"), ("att", "ATT"),
                                                               ("rel", "REL"), ("filt", "FILTER"), ("g", "GAIN"),
                                                               ("f", "FREQ"), ("q", "Q"), ("mode", "MODE"))},
    "SOUL": {"lf": "LO FREQ", "lg": "LO GAIN", "lmf": "LM FREQ", "lmq": "LM Q", "lmg": "LM GAIN", "hmf": "HM FREQ",
             "hmq": "HM Q", "hmg": "HM GAIN", "hf": "HF FREQ", "hg": "HF GAIN"},
    "E84": {"g": "GAIN", "lf": "LF FREQ", "lg": "LF GAIN", "mf": "MID FREQ", "mg": "MID GAIN", "mq": "MID Q",
            "hf": "HF FREQ", "hg": "HF GAIN"},
    "F110": {"peq": "PEQ ON", "lmf": "LM FREQ", "lmg": "LM GAIN", "lmq": "LM Q", "lmf3": "LM x3", "hmf": "HM FREQ",
             "hmg": "HM GAIN", "hmq": "HM Q", "hmf3": "HM x3", "shv": "SHV ON", "lf": "LF FREQ", "lg": "LF GAIN",
             "hf": "HF FREQ", "hg": "HF GAIN", "g": "GAIN"},
    "PULSAR": {"eq1": "EQ1 ON", "1lb": "LF BOOST", "1latt": "LF ATT", "1lf": "LF FREQ", "1hw": "HF WID",
               "1hb": "HF BOOST", "1hf": "HF FREQ", "1hatt": "HF ATT", "eq5": "EQ5 ON"},
    "MACH4": {"sub": "SUB", "40": "40", "160": "160", "650": "650", "2k5": "2k5", "air": "AIR", "airm": "AIR MODE",
              "again": "AUTO"},
    "PLATE": {"pdel": "PRE DLY", "size": "SIZE", "dcy": "DECAY", "mult": "BASS MULT", "damp": "DAMPING",
              "lc": "LO CUT", "hc": "HI CUT", "att": "ATTACK", "sprd": "SPREAD", "diff": "DIFFUSION", "spin": "SPIN",
              "ecl": "ECHO L", "ecr": "ECHO R", "efl": "FEED L", "efr": "FEED R"},
    "HALL": {"pdel": "PRE DLY", "size": "SIZE", "dcy": "DECAY", "mult": "BASS MULT", "damp": "DAMPING",
             "lc": "LO CUT", "hc": "HI CUT", "shp": "SHAPE", "sprd": "SPREAD", "diff": "DIFFUSION",
             "mspd": "MOD SPD"},
    "ST-DL": {"time": "TIME", "mode": "MODE", "fact": "FACTOR", "pat": "PATTERN", "offs": "OFFSET", "feed": "FEED",
              "flc": "FEED LCUT", "fhc": "FEED HCUT", "lc": "LO CUT", "hc": "HI CUT"},
    "CHORUS": {"lc": "LO CUT", "hc": "HI CUT", "wave": "WAVEFORM", "phase": "PHASE", "mix": "MIX",
               "dlyl": "DELAY L", "dlyr": "DELAY R", "depl": "DEPTH L", "depr": "DEPTH R", "sprd": "SPREAD",
               "spd": "SPEED"},
    "RACKAMP": {"pre": "PREAMP", "buzz": "BUZZ", "punch": "PUNCH", "crunch": "CRUNCH", "drive": "DRIVE",
                "out": "OUT GAIN", "leq": "LOW EQ", "heq": "HIGH EQ", "cab": "CAB SIM"},
    "UKROCK": {"gain": "GAIN", "bass": "BASS", "mid": "MIDDLE", "treb": "TREBLE", "pres": "PRESENCE",
               "mstr": "MASTER", "out": "OUT GAIN", "sag": "SAG", "cab": "CAB SIM"},
    "TILT": {"tilt": "TILT"}, "MAX": {"low": "LOW CONT", "proc": "HIGH PROC"}, "AP1": {"f": "FREQ"},
    "AP2": {"f": "FREQ", "q": "Q"},
}

STD_BANDS = {"l": "L", "1": "1", "2": "2", "3": "3", "4": "4", "5": "5", "6": "6", "h": "H"}

FIXED = {
    "in/set/trim": "INPUT > TRIM", "in/set/bal": "INPUT > BALANCE", "in/set/inv": "INPUT > polarity (Ø)",
    "in/set/dlyon": "INPUT > DELAY on", "in/conn/grp": "INPUT > CHANNEL INPUT MAIN: source group",
    "in/conn/in": "INPUT > CHANNEL INPUT MAIN: input number",
    "flt/lc": "FILTER > LOW CUT on", "flt/lcf": "FILTER > LOW CUT FREQ", "flt/lcs": "FILTER > LOW CUT SLOPE",
    "flt/hc": "FILTER > HIGH CUT on", "flt/hcf": "FILTER > HIGH CUT FREQ", "flt/hcs": "FILTER > HIGH CUT SLOPE",
    "flt/tf": "FILTER > TOOL on", "flt/mdl": "FILTER > TOOL type",
    "proc": "HOME > process order (wrench: drag the slots) = GATE, EQ, DYN, INS 1",
    "gate/on": "GATE on", "gate/mdl": "GATE > model", "gate/mix": "GATE > MIX", "gate/gain": "GATE > GAIN",
    "gatesc/type": "GATE > KEY FILTER > TYPE", "gatesc/f": "GATE > KEY FILTER > FREQ",
    "gatesc/q": "GATE > KEY FILTER > Q", "gatesc/src": "GATE > KEY SOURCE",
    "eq/on": "EQ on", "eq/mdl": "EQ > model", "eq/mix": "EQ > MIX",
    "dyn/on": "DYNAMICS (COMP) on", "dyn/mdl": "DYNAMICS > model", "dyn/mix": "DYNAMICS > MIX",
    "dyn/gain": "DYNAMICS > GAIN",
    "dynsc/type": "DYNAMICS > KEY FILTER > TYPE", "dynsc/f": "DYNAMICS > KEY FILTER > FREQ",
    "dynsc/q": "DYNAMICS > KEY FILTER > Q", "dynsc/src": "DYNAMICS > KEY SOURCE",
    "dynxo/type": "DYNAMICS > XOVER MODE > TYPE", "dynxo/f": "DYNAMICS > XOVER MODE > FREQ",
    "dynxo/q": "DYNAMICS > XOVER MODE > Q", "dynxo/depth": "DYNAMICS > XOVER MODE > DEPTH",
    "preins/on": "INS 1 on", "preins/ins": "INS 1 > FX PROCESSOR", "postins/on": "INS 2 on",
    "mute": "MUTE", "fdr": "fader", "pan": "PAN", "wid": "WIDTH", "busmono": "BUS MONO",
    "main/1/on": "MAIN SENDS > MAIN 1 on", "main/1/lvl": "MAIN SENDS > MAIN 1 level",
    "main/1/pre": "MAIN SENDS > MAIN 1 PRE off", "dly/on": "DELAY on",
}


def where(path, settings):
    """Where a control is on the console ("CH 1 > EQ > GAIN 2")."""
    m = re.match(r"^/(ch|main|fx|io|rec)/(.*)$", path)
    if not m:
        return path
    top, rest = m.groups()
    if top == "fx":
        n, _, pid = rest.partition("/")
        if pid == "mdl":
            return f"EFFECTS > FX {n} > FX TYPE"
        if pid == "fxmix":
            return f"EFFECTS > FX {n} > MIX"
        model = settings.get(f"/fx/{n}/mdl", "")
        return f"EFFECTS > FX {n} ({model}) > {MODEL_LABELS.get(model, {}).get(pid, pid.upper())}"
    if top == "io":
        mm = re.match(r"out/REC/(\d)/(grp|in)", rest)
        if mm:
            what = "source group" if mm.group(2) == "grp" else "source number"
            return f"ROUTING > OUTPUTS > output group USB REC (RECORDER) > {mm.group(1)}: {what}"
        mm = re.match(r"in/PLAY/(\d)/(\w+)", rest)
        if mm:
            return f"ROUTING > SOURCES > group USB PLAYER > {mm.group(1)} > {mm.group(2).upper()}"
        return path
    if top == "rec":
        return {"channels": "USB RECORDER > 2-CH / 4-CH", "resolution": "USB RECORDER > 16 / 24 bit"}.get(rest, path)
    strip, _, field = rest.partition("/")
    name = "CH" if top == "ch" else "MAIN"
    prefix = f"{name} {strip}"
    if field in FIXED:
        return f"{prefix} > {FIXED[field]}"
    mm = re.match(r"^(gate|dyn|eq|flt)/(.+)$", field)
    if mm:
        slot, pid = mm.groups()
        model = settings.get(f"/{top}/{strip}/{slot}/mdl", {"gate": "GATE", "dyn": "COMP", "eq": "STD",
                                                             "flt": "TILT"}[slot])
        screen = {"gate": "GATE", "dyn": "DYNAMICS", "eq": "EQ", "flt": "FILTER > TOOL"}[slot]
        if slot == "eq" and model == "STD":
            b = re.match(r"^([lh1-6])(g|f|q|eq)$", pid)
            if b:
                what = {"g": "GAIN", "f": "FREQ", "q": "Q", "eq": "EQ"}[b.group(2)]
                return f"{prefix} > EQ > {what} {STD_BANDS[b.group(1)]}" + (" (band type)" if b.group(2) == "eq" else "")
        label = MODEL_LABELS.get(model, {}).get(pid, pid.upper())
        return f"{prefix} > {screen} ({model}) > {label}"
    return f"{prefix} > {field}"


def build_plan():
    tests = []
    for code, (name, prio, kind, analysis, title, settings, notes) in enumerate(TESTS, start=1):
        assert kind in make_signals.KINDS, name
        x, tl = make_signals.build(kind, code)
        tests.append({
            "name": name, "code": code, "priority": prio, "title": title, "signal": kind,
            "signal_file": f"signals/tests/{code:02d}_{name}.wav", "seconds": round(len(x) / make_signals.SR, 2),
            "analysis": analysis, "settings": settings, "notes": notes,
            "where": {p: where(p, settings) for p in settings},
        })
    names = [t["name"] for t in tests]
    assert len(set(names)) == len(names), "duplicate test names"
    return {
        "version": 1, "sample_rate": make_signals.SR, "strip": "/ch/1",
        "common": COMMON, "wing_only": WING_ONLY, "tolerances": TOLERANCES, "tests": tests,
    }


def session_minutes(plan, priority=None, per_test_s=150.0):
    tests = [t for t in plan["tests"] if priority is None or t["priority"] <= priority]
    return sum(t["seconds"] + per_test_s for t in tests) / 60.0, len(tests)


def fmt(v):
    if isinstance(v, float):
        return f"{v:g}"
    return str(v)


def markdown(plan):
    out = []
    w = out.append
    by_prio = {}
    for t in plan["tests"]:
        by_prio.setdefault(t["priority"], []).append(t)
    totals = []
    for p in sorted(by_prio):
        manual, n = session_minutes(plan, p, 150.0)
        auto, _ = session_minutes(plan, p, 25.0)
        totals.append((p, n, manual, auto))
    w("# WING vs Sidecar: audio test plan")
    w("")
    w("Generated by `plan.py` (edit that file, then run `python3 plan.py`); the same plan is in")
    w("`test_plan.json` for the scripts. Every test sets **channel 1** (and for FX tests FX slot 1),")
    w("plays its signal file from the WING's USB player into channel 1, and records Main 1 L/R plus")
    w("the raw player signal with the WING's USB recorder. Sidecar's side is rendered offline from")
    w("the same settings (`run_all.py`), so at the console you only record the WING.")
    w("")
    w("## Time")
    w("")
    w("| Stop after | Tests | Dialling by hand | With `wing_osc.py apply` |")
    w("|---|---|---|---|")
    for p, n, manual, auto in totals:
        w(f"| priority {p} | {n} | about {manual:.0f} min | about {auto:.0f} min |")
    w("")
    w("Priority 1 is the baseline, the WING EQ, the filters, the WING COMP and the WING GATE; stop")
    w("whenever you like, every finished test is useful on its own. By hand counts about 2.5 minutes per")
    w("test for setting up and starting/stopping the recorder; with the OSC helper about 25 seconds.")
    w("")
    w("## Before the first test (once)")
    w("")
    w("1. **Save your show** on the WING (LIBRARY), then start from a clean console: SETUP > INITIALIZE")
    w("   CONSOLE (or at least: every other channel, aux and bus off Main 1, Main 1 processing off).")
    w("2. Copy the folder `signals/tests` from `make_signals.py` to a USB stick (FAT32 or exFAT) and plug")
    w("   it into the WING's USB port (the one for the USB recorder / player).")
    w("3. Patch (ROUTING), or let `python3 wing_osc.py WING_IP setup` do it:")
    w("")
    w("| Path | Value | On the console |")
    w("|---|---|---|")
    for p, v in plan["wing_only"].items():
        w(f"| `{p}` | {fmt(v)} | {where(p, {})} |")
    w("")
    w("That is: channel 1's MAIN input = USB PLAYER 1, a mono source (MODE M: if PLAYER 1/2 were a")
    w("stereo pair, channel 1 would become a stereo channel); recorder inputs 1-2 = MAIN 1 L / R, 3 = USB")
    w("PLAYER 1 (the unprocessed signal: it lets the analysis measure latency and correct any level")
    w("offset of the player or recorder), 4 off. USB RECORDER set to 4-CH, 24 bit, WAV. The files are")
    w("mono; should the player not take mono files, write stereo ones (`make_signals.py --stereo`, the")
    w("same signal on both sides).")
    w("4. **Levels:** nothing to adjust. The player has no gain, the channel's TRIM is 0 dB, fader and Main")
    w("   1 at 0 dB; the files peak at -6 dBFS (the level steps of the dynamics tests reach 0 dBFS), so")
    w("   nothing clips. Do not touch the input gain, fader or Main fader during the session.")
    w("5. Settings every test starts from (the helper sets them with each test; by hand: set them once,")
    w("   then only change what a test lists and put it back afterwards):")
    w("")
    w("| Path | Value | On the console |")
    w("|---|---|---|")
    for p, v in plan["common"].items():
        w(f"| `{p}` | {fmt(v)} | {where(p, {})} |")
    w("")
    w("## For each test")
    w("")
    w("1. Set the test's settings (by hand from the table, or `python3 wing_osc.py WING_IP apply NAME`, which")
    w("   also reads every value back and saves what the console reports).")
    w("2. USB RECORDER: REC. USB PLAYER: play the test's file (`signals/tests/NN_name.wav`).")
    w("3. When the player stops (a falling chirp ends every file), STOP the recorder.")
    w("4. Note nothing: the recording carries the test's ID code. Name it after the test if you like")
    w("   (`eq_peq_600_p6_q1.wav`); `run_all.py` finds the test either way.")
    w("")
    w("Values are as the console shows them; the console puts a value on its own steps (600 Hz reads")
    w("601.4), and Sidecar's render does the same, so set the closest value the knob gives.")
    w("Sidecar reads the very same paths: the render tool (`render --settings`) applies them through")
    w("Sidecar's WING mapping, so `/ch/1/eq/2g` is EQ band 2's gain on both.")
    w("")
    for p in sorted(by_prio):
        w(f"## Priority {p}")
        w("")
        for t in by_prio[p]:
            w(f"### {t['code']:02d} `{t['name']}`: {t['title']}")
            w("")
            w(f"Signal `{t['signal_file']}` ({t['signal']}, {t['seconds']:.0f} s), analysis: {t['analysis']}.")
            if t["notes"]:
                w(t["notes"])
            w("")
            if t["settings"]:
                w("| Path | Value | On the console |")
                w("|---|---|---|")
                for path, v in t["settings"].items():
                    w(f"| `{path}` | {fmt(v)} | {t['where'][path]} |")
            else:
                w("Nothing beyond the common settings.")
            w("")
    w("## Tolerances")
    w("")
    w("| Measure | Tolerance |")
    w("|---|---|")
    for k, v in plan["tolerances"].items():
        w(f"| {k} | {v:g} |")
    w("")
    return "\n".join(out)


def main():
    plan = build_plan()
    with open(os.path.join(HERE, "test_plan.json"), "w") as f:
        json.dump(plan, f, indent=1)
        f.write("\n")
    with open(os.path.join(HERE, "test_plan.md"), "w") as f:
        f.write(markdown(plan))
    for p in (1, 2, 3):
        manual, n = session_minutes(plan, p)
        auto, _ = session_minutes(plan, p, 25.0)
        print(f"priority <= {p}: {n} tests, about {manual:.0f} min by hand, {auto:.0f} min with wing_osc.py")


if __name__ == "__main__":
    main()
