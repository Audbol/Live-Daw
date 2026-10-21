"""Self-tests of the audio comparison kit, without a WING.

  cd tools/wing/audio_compare && python3 -m unittest -v

The end-to-end tests render Sidecar with the engine (the livedaw-wing
`render` example): set SIDECAR_RENDER to a built binary, or they use
rust/target/{release,debug}/examples/render and build it with cargo if
neither exists (skipped without cargo). Each takes Sidecar's render as the
"WING" too, after simulated capture artefacts (latency, clock drift, level
offset, noise), and checks that the report passes; then changes one setting
on the "WING" side only and checks that the report catches exactly that.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

import numpy as np
from scipy import signal as sps

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.dirname(HERE)
sys.path.insert(0, KIT)
import analyze  # noqa: E402
import make_signals as ms  # noqa: E402
import plan as plan_mod  # noqa: E402
import run_all  # noqa: E402
import simulate  # noqa: E402
import wavio  # noqa: E402
import wing_osc  # noqa: E402

PLAN = ms.load_plan()
ARTEFACTS = dict(latency_samples=4321.77, gain_db=-0.4, drift_ppm=40.0, noise_db=-110.0, lead_s=0.6, tail_s=0.4)


def failed(res):
    return [r for r in res.rows if r["ok"] is False]


class WavTests(unittest.TestCase):
    def test_round_trip(self):
        x = np.stack([np.linspace(-0.9, 0.9, 1000), np.sin(np.arange(1000) * 0.1) * 0.5], axis=1)
        with tempfile.TemporaryDirectory() as d:
            for bits, tol in ((16, 1 / 32768), (24, 1 / 8388608), (32, 1e-9), ("float", 1e-7)):
                p = os.path.join(d, f"x{bits}.wav")
                wavio.write_wav(p, x, 48000, bits)
                y, sr = wavio.read_wav(p, use_soundfile=False)
                self.assertEqual((sr, y.shape), (48000, x.shape))
                self.assertLess(np.max(np.abs(y - x)), tol, bits)

    def test_extensible_float(self):
        # WAVE_FORMAT_EXTENSIBLE with a float subformat, as Sidecar's recorder writes.
        import struct
        x = np.array([0.25, -0.5, 0.75], dtype="<f4")
        sub = struct.pack("<H", 3) + b"\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71"
        fmt = struct.pack("<HHIIHHHHI", 0xFFFE, 1, 48000, 192000, 4, 32, 22, 32, 4) + sub
        body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", 12) + x.tobytes()
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "ext.wav")
            with open(p, "wb") as f:
                f.write(b"RIFF" + struct.pack("<I", len(body)) + body)
            y, sr = wavio.read_wav(p, use_soundfile=False)
        self.assertEqual(sr, 48000)
        np.testing.assert_allclose(y[:, 0], x)


class SignalTests(unittest.TestCase):
    def test_id_code_round_trip(self):
        for code in (1, 37, 65, 200):
            x, tl = ms.build("key", code)
            self.assertEqual(ms.decode_id(x, tl["id_at"]), code)
        x, tl = ms.build("key", 37)
        x[tl["id_at"]:tl["id_at"] + 2400] = 0.0
        self.assertNotEqual(ms.decode_id(x, tl["id_at"]), 37)

    def test_signals_are_deterministic_and_laid_out(self):
        for kind in ms.KINDS:
            a, tl = ms.build(kind, 5)
            b, _ = ms.build(kind, 5)
            np.testing.assert_array_equal(a, b)
            self.assertEqual(len(a), tl["length"])
            self.assertLessEqual(np.max(np.abs(a)), 1.0)
            for s in tl["segments"]:
                self.assertLessEqual(s["start"] + s["length"] + s["after"], tl["end_chirp"])
        x, tl = ms.build("dyn", 1)
        seg = ms.segment(tl, "levels")
        n = int(seg["params"]["step_s"] * ms.SR)
        top = x[seg["start"] + 30 * n:seg["start"] + 31 * n]
        self.assertAlmostEqual(np.max(np.abs(top)), 1.0, places=3)  # the 0 dBFS step

    def test_plan_files_are_current(self):
        built = plan_mod.build_plan()
        self.assertEqual(json.loads(json.dumps(built)), PLAN, "run `python3 plan.py`")
        with open(os.path.join(KIT, "test_plan.md")) as f:
            self.assertEqual(f.read(), plan_mod.markdown(built), "run `python3 plan.py`")
        codes = [t["code"] for t in PLAN["tests"]]
        self.assertEqual(len(set(codes)), len(codes))
        for t in PLAN["tests"]:
            self.assertIn(t["analysis"], analyze.ANALYSES)
            self.assertIn(t["signal"], ms.KINDS)
            self.assertLess(t["code"], 200)
        self.assertEqual([t["priority"] for t in PLAN["tests"]], sorted(t["priority"] for t in PLAN["tests"]))

    def test_osc_codec(self):
        msg = wing_osc.encode("/ch/1/eq/2g", [6.0])
        self.assertEqual(wing_osc.decode(msg), ("/ch/1/eq/2g", [6.0]))
        self.assertEqual(wing_osc.decode(wing_osc.encode("/a", ["x", 3]))[1], ["x", 3])
        self.assertEqual(wing_osc.reported_value(["601.4", 0.4, 601.388]), ("601.4", 601.388))
        self.assertEqual(wing_osc.reported_value(["1", 1.0, 1]), ("1", 1))
        self.assertEqual(wing_osc.reported_value(["RMS"]), ("RMS", "RMS"))
        self.assertTrue(wing_osc.matches(600.0, "601.4"))
        self.assertTrue(wing_osc.matches(20000.0, "20k0"))
        self.assertTrue(wing_osc.matches("1:3", "1:3"))
        self.assertTrue(wing_osc.matches(4.0, "4.0"))
        self.assertFalse(wing_osc.matches(6.0, "7.0"))


def write_capture(path, data):
    wavio.write_wav(path, data, ms.SR, "float")


class AlignmentTests(unittest.TestCase):
    """Capture artefacts on a known linear filter, without the engine."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.test = analyze.find_test(PLAN, "eq_peq_600_p6_q1")
        self.signal, self.tl = ms.test_signal(self.test)
        b, a = sps.iirpeak(600.0, 1.0, fs=ms.SR)
        out = self.signal + 0.5 * sps.lfilter(b, a, self.signal)
        self.clean = np.stack([out * 0.7, out * 0.7, self.signal], axis=1)
        self.sidecar = os.path.join(self.dir, "sidecar.wav")
        write_capture(self.sidecar, self.clean)

    def tearDown(self):
        shutil.rmtree(self.dir)

    def run_capture(self, data):
        wing = os.path.join(self.dir, "wing.wav")
        write_capture(wing, data)
        return analyze.analyze(wing, self.sidecar, self.test, PLAN)

    def test_recovers_latency_drift_and_level(self):
        res = self.run_capture(simulate.capture(self.clean, **ARTEFACTS))
        info = res.captures["WING"]
        self.assertAlmostEqual(info["offset_s"] * ms.SR, ARTEFACTS["lead_s"] * ms.SR + ARTEFACTS["latency_samples"],
                               delta=0.05)
        self.assertAlmostEqual(info["drift_ppm"], ARTEFACTS["drift_ppm"], delta=0.5)
        self.assertAlmostEqual(info["gain_cal_db"], ARTEFACTS["gain_db"], delta=0.005)
        self.assertEqual(info["code"], self.test["code"])
        self.assertEqual(failed(res), [], analyze.report_markdown(res))

    def test_without_a_reference_channel(self):
        art = dict(ARTEFACTS, gain_db=0.0)
        res = self.run_capture(simulate.capture(self.clean[:, :2], **art))
        self.assertEqual(failed(res), [], analyze.report_markdown(res))
        self.assertTrue(any("no reference channel" in w for w in res.warnings))

    def test_catches_a_level_offset_without_reference(self):
        res = self.run_capture(simulate.capture(self.clean[:, :2], **dict(ARTEFACTS, gain_db=-1.0)))
        self.assertTrue(any("1 kHz gain" in r["metric"] for r in failed(res)))

    def test_identifies_recordings_by_their_code(self):
        wing = os.path.join(self.dir, "R_20261021-101500.wav")
        write_capture(wing, simulate.capture(self.clean, **ARTEFACTS))
        self.assertEqual(run_all.identify(wing, PLAN)["name"], self.test["name"])


def render_binary():
    explicit = os.environ.get("SIDECAR_RENDER")
    if explicit:
        return explicit
    try:
        return run_all.find_render(build=False)
    except SystemExit:
        pass
    if shutil.which("cargo") is None:
        return None
    try:
        return run_all.find_render(build=True, profile="debug")
    except (subprocess.CalledProcessError, OSError):
        return None


class EndToEndTests(unittest.TestCase):
    """Sidecar's render as both sides; then a deliberate change on the "WING" side."""

    @classmethod
    def setUpClass(cls):
        cls.render = render_binary()
        if cls.render is None:
            raise unittest.SkipTest("Sidecar's render tool is not available (no cargo)")
        cls.dir = tempfile.mkdtemp()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir)

    def compare(self, name, wing_changes=None, artefacts=ARTEFACTS):
        test = analyze.find_test(PLAN, name)
        sig = run_all.signal_file(test, self.dir)
        side = os.path.join(self.dir, f"{name}.sidecar.wav")
        run_all.render(self.render, run_all.settings_for(PLAN, test), sig, side, self.dir)
        clean = os.path.join(self.dir, f"{name}.wing_clean.wav")
        run_all.render(self.render, run_all.settings_for(PLAN, test, wing_changes), sig, clean, self.dir)
        data, sr = wavio.read_wav(clean)
        wing = os.path.join(self.dir, f"{name}.wing.wav")
        wavio.write_wav(wing, simulate.capture(data, sr, **artefacts), sr, 24)
        return analyze.analyze(wing, side, test, PLAN)

    def test_eq_band_gain(self):
        res = self.compare("eq_peq_600_p6_q1")
        self.assertEqual(failed(res), [], analyze.report_markdown(res))
        res = self.compare("eq_peq_600_p6_q1", {"/ch/1/eq/2g": 7.0})
        bad = failed(res)
        self.assertTrue(bad)
        self.assertTrue(all(r["section"] in ("sweep magnitude", "1 kHz tone") for r in bad), bad)
        mag = next(r for r in bad if r["metric"].startswith("largest difference (within"))
        self.assertAlmostEqual(abs(mag["diff"]), 1.0, delta=0.1)
        self.assertTrue(any("at 60" in n for n in res.notes), res.notes)  # largest difference at ~600 Hz

    def test_compressor_ratio(self):
        res = self.compare("comp_r4_k0_peak")
        self.assertEqual(failed(res), [], analyze.report_markdown(res))
        res = self.compare("comp_r4_k0_peak", {"/ch/1/dyn/ratio": 5.0})
        static = [r for r in failed(res) if r["section"] == "static curve"]
        self.assertTrue(static)
        # Only steps above the threshold (-30 dBFS, pan law -3 dB) differ.
        steps = [int(r["metric"].split()[1]) for r in static]
        self.assertGreater(min(steps), -30, steps)
        ok_below = [r for r in res.rows if r["section"] == "static curve" and r["ok"]
                    and int(r["metric"].split()[1]) < -32]
        self.assertTrue(ok_below)

    def test_gate_release(self):
        res = self.compare("gate_r12_m35_r20", {"/ch/1/gate/rel": 250.0})
        bad = failed(res)
        fall = [r for r in bad if r["section"].startswith("timing: fall") and r["unit"] == "ms"]
        self.assertTrue(fall, bad)
        self.assertTrue(all(r["diff"] > 0 for r in fall), fall)  # Sidecar (500 ms) releases later
        self.assertFalse(any(r["section"] == "static curve" for r in bad), bad)

    def test_delay_time(self):
        res = self.compare("fx_stdl")
        self.assertEqual(failed(res), [], analyze.report_markdown(res))
        res = self.compare("fx_stdl", {"/fx/1/time": 310.0})
        times = [r for r in failed(res) if r["metric"].endswith("time")]
        self.assertTrue(times)
        self.assertAlmostEqual(times[0]["diff"], -10.0, delta=0.5)


if __name__ == "__main__":
    unittest.main()
