#include "TestHelpers.h"
#include "engine/PluginHost.h"
#include "engine/SandboxedPlugin.h"
#include "engine/Show.h"
#include "ui/PluginChainView.h"

#include <cmath>

using namespace livedaw;

using livedaw::test::pumpMessages;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 128;

bool hasDisplay()
{
#if JUCE_LINUX || JUCE_BSD
    // LSP's VST3 build (and many other Linux plugins) crash in createView /
    // hasEditor without an X display: their editors need one.
    return juce::SystemStats::getEnvironmentVariable ("DISPLAY", {}).isNotEmpty();
#else
    return true;
#endif
}

float toDb (float gain) { return 20.0f * std::log10 (juce::jmax (gain, 1.0e-9f)); }

juce::AudioProcessorParameter* findParam (juce::AudioProcessor& p, const juce::String& name)
{
    for (auto* prm : p.getParameters())
        if (prm->getName (256).trim() == name)
            return prm;
    return nullptr;
}

/** As Sidecar's editors set a parameter: a gesture and setValueNotifyingHost. */
void setNormalised (juce::AudioProcessorParameter& prm, float value)
{
    prm.beginChangeGesture();
    prm.setValueNotifyingHost (value);
    prm.endChangeGesture();
}

float textNumber (const juce::String& text)
{
    const auto t = text.trim();
    if (t.containsIgnoreCase ("inf"))
        return t.startsWithChar ('-') ? -1.0e9f : 1.0e9f;
    return t.getFloatValue();
}

/** Sets a continuous parameter to the value it displays as `target` (in its own
    units), by bisection on its text. */
bool setDisplayed (juce::AudioProcessor& p, const juce::String& name, float target)
{
    auto* prm = findParam (p, name);
    if (prm == nullptr)
        return false;
    auto at = [prm] (float v) { return textNumber (prm->getText (v, 64)); };
    const bool rising = at (1.0f) > at (0.0f);
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 32; ++i)
    {
        const float mid = 0.5f * (lo + hi);
        if ((at (mid) < target) == rising)
            lo = mid;
        else
            hi = mid;
    }
    setNormalised (*prm, 0.5f * (lo + hi));
    return true;
}

/** Sets a gain parameter in dB, whether the plugin shows it in dB (LSP VST3) or
    as a linear gain (LSP LV2). */
bool setGainDb (juce::AudioProcessor& p, const juce::String& name, float db)
{
    auto* prm = findParam (p, name);
    if (prm == nullptr)
        return false;
    const bool inDb = prm->getLabel().containsIgnoreCase ("dB") || prm->getCurrentValueAsText().containsIgnoreCase ("dB");
    return setDisplayed (p, name, inDb ? db : std::pow (10.0f, db / 20.0f));
}

/** Selects the choice of a stepped parameter shown as `label`. */
bool setChoice (juce::AudioProcessor& p, const juce::String& name, const juce::String& label)
{
    auto* prm = findParam (p, name);
    if (prm == nullptr)
        return false;
    const int steps = juce::jlimit (2, 1000, prm->getNumSteps());
    for (int i = 0; i < steps; ++i)
    {
        const float v = (float) i / (float) (steps - 1);
        if (prm->getText (v, 64).trim().equalsIgnoreCase (label))
        {
            setNormalised (*prm, v);
            return true;
        }
    }
    return false;
}

juce::String paramText (juce::AudioProcessor& p, const juce::String& name)
{
    auto* prm = findParam (p, name);
    return prm != nullptr ? prm->getCurrentValueAsText().trim() : juce::String ("<missing>");
}

/** A console with one input channel (or two) fed with tones, the way the
    offline tests drive the engine, capturing Main 1. */
struct ToneRig
{
    explicit ToneRig (int channels = 1)
    {
        test::unityConsole (engine);
        engine.outputProtection = false;
        engine.prepareOffline (kRate, kBlock, 2, 2);
        test::setChannels (engine, channels);
        for (auto& ch : engine.getChannels())
        {
            ch->params.inputLeft = 0;
            ch->params.inputRight = 1;
        }
    }

    ~ToneRig()
    {
        for (int slot = 0; slot < kNumFx; ++slot)
            engine.getFx().setPlugin (slot, nullptr);
        engine.getGarbage().collectAllNow();
    }

    Strip& channel (int i) { return *engine.getChannels()[(size_t) i]; }

    void insert (int channelIndex, int slot)
    {
        channel (channelIndex).params.preInsert.fx = slot;
        channel (channelIndex).params.preInsert.on = true;
    }

    /** Runs `blocks` blocks of the tones (pumping messages now and then), and
        keeps the last `capture` samples of Main 1. */
    void run (int blocks, int capture = 4800)
    {
        std::vector<float> in0 ((size_t) kBlock), in1 ((size_t) kBlock), outL ((size_t) kBlock), outR ((size_t) kBlock);
        const float* ins[] = { in0.data(), in1.data() };
        float* outs[] = { outL.data(), outR.data() };
        left.clear();
        right.clear();
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < kBlock; ++i, ++t)
            {
                float x = 0.0f;
                for (auto& [f, a] : tones)
                    x += a * (float) std::sin (juce::MathConstants<double>::twoPi * f * (double) t / kRate);
                in0[(size_t) i] = x;
                in1[(size_t) i] = x;
            }
            engine.processBlock (ins, 2, outs, 2, kBlock);
            if ((blocks - b) * kBlock <= capture + kBlock)
            {
                left.insert (left.end(), outL.begin(), outL.end());
                right.insert (right.end(), outR.begin(), outR.end());
            }
            if (b % 16 == 15)
                pumpMessages (1);
        }
        const auto keep = (size_t) juce::jmin (capture, (int) left.size());
        left.erase (left.begin(), left.end() - (std::ptrdiff_t) keep);
        right.erase (right.begin(), right.end() - (std::ptrdiff_t) keep);
    }

    /** The level of the `freq` component of Main 1 left (or right), in dBFS
        amplitude (a DFT bin over a whole number of periods). */
    float toneDb (double freq, bool rightSide = false) const
    {
        const auto& x = rightSide ? right : left;
        double re = 0.0, im = 0.0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double w = juce::MathConstants<double>::twoPi * freq * (double) i / kRate;
            re += x[i] * std::cos (w);
            im += x[i] * std::sin (w);
        }
        return toDb ((float) (2.0 * std::sqrt (re * re + im * im) / (double) x.size()));
    }

    float peakDb() const
    {
        float peak = 0.0f;
        for (auto v : left)
            peak = juce::jmax (peak, std::abs (v));
        return toDb (peak);
    }

    bool allFinite() const
    {
        for (auto v : left)
            if (! std::isfinite (v))
                return false;
        return true;
    }

    MixerEngine engine { 0 };
    std::vector<std::pair<double, float>> tones { { 1000.0, 0.25f } };
    std::vector<float> left, right;
    juce::int64 t = 0;
};

/** The impulse response at Main 1 left after a unit-ish impulse (`amp`) on the
    input, after settling on silence: (sample, value) of each non-silent sample. */
std::vector<std::pair<int, float>> impulseAtMain (MixerEngine& engine, float amp, int blocks)
{
    engine.updateLatencyCompensation();
    std::vector<float> in ((size_t) kBlock, 0.0f), outL ((size_t) kBlock), outR ((size_t) kBlock);
    const float* ins[] = { in.data(), in.data() };
    float* outs[] = { outL.data(), outR.data() };
    for (int b = 0; b < 2 * MixerEngine::kMaxCompensation / kBlock + 40; ++b)
        engine.processBlock (ins, 2, outs, 2, kBlock);
    std::vector<std::pair<int, float>> peaks;
    for (int b = 0; b < blocks; ++b)
    {
        in[0] = b == 0 ? amp : 0.0f;
        engine.processBlock (ins, 2, outs, 2, kBlock);
        for (int i = 0; i < kBlock; ++i)
            if (std::abs (outL[(size_t) i]) > 0.02f * amp)
                peaks.push_back ({ b * kBlock + i, outL[(size_t) i] });
    }
    return peaks;
}

/** The LSP plugins this test uses, found by name in a scan. */
struct LspSet
{
    juce::String format;
    const juce::PluginDescription* eq = nullptr;          // Parametric Equalizer x8 Stereo
    const juce::PluginDescription* eqMono = nullptr;      // Parametric Equalizer x8 Mono
    const juce::PluginDescription* compressor = nullptr;  // Compressor Stereo
    const juce::PluginDescription* limiter = nullptr;     // Limiter Stereo (lookahead: latency)
    const juce::PluginDescription* delay = nullptr;       // Delay Compensator Stereo
    const juce::PluginDescription* sidechain = nullptr;   // Sidechain Compressor Stereo (a second input bus)

    bool usable() const { return eq != nullptr && compressor != nullptr && limiter != nullptr && delay != nullptr; }
};
} // namespace

/** Real plugins (the LSP plugins, VST3 and LV2) through the whole path: the
    out-of-process scanner, an FX slot on a channel insert, in-process and in the
    plugin sandbox, with measured effects, bypass, delay compensation, a chain of
    three saved in a show, parameter changes and the editors in the plugin
    window. Skipped when the LSP plugins aren't installed (CI); the editor checks
    need a display (Xvfb will do). */
class RealPluginTests final : public juce::UnitTest
{
public:
    RealPluginTests() : juce::UnitTest ("Real plugins (LSP VST3 / LV2)", "LiveDAW") {}

    void runTest() override
    {
        PluginHost host (nullptr);
        juce::OwnedArray<juce::PluginDescription> vst3Types, lv2Types;
        const auto vst3 = scan (host, "VST3", vst3Types);
        const auto lv2 = scan (host, "LV2", lv2Types);

        if (! vst3.usable() && ! lv2.usable())
        {
            beginTest ("Real plugins");
            logMessage ("The LSP plugins (VST3 or LV2) aren't installed; skipping");
            expect (true);
            return;
        }

        for (const auto* set : { &vst3, &lv2 })
        {
            if (! set->usable())
            {
                logMessage ("No LSP " + set->format + " plugins; skipping " + set->format);
                continue;
            }
            for (const bool sandboxed : { false, true })
            {
                if (sandboxed && ! juce::File (LIVEDAW_TEST_PLUGIN_HOST).existsAsFile())
                    continue;
                configure (host, sandboxed);
                runSuite (host, *set, sandboxed);
            }
        }
        host.setSandboxEnabled (false);
    }

private:
    juce::String tag;

    LspSet scan (PluginHost& host, const juce::String& formatName, juce::OwnedArray<juce::PluginDescription>& types)
    {
        LspSet set;
        set.format = formatName;
        juce::AudioPluginFormat* format = nullptr;
        for (auto* f : host.getFormatManager().getFormats())
            if (f->getName() == formatName)
                format = f;
        if (format == nullptr)
            return set;

        // Only the LSP bundles (VST3 and LV2: one bundle holding all of them).
        juce::StringArray files;
        for (auto& f : format->searchPathsForPlugins (format->getDefaultLocationsToSearch(), true, false))
            if (f.containsIgnoreCase ("lsp-plugins"))
                files.add (f);
        if (files.isEmpty())
            return set;

        beginTest (formatName + ": the out-of-process scanner finds the LSP plugins");
        const auto start = juce::Time::getMillisecondCounter();
        int scanned = 0;
        for (auto& f : files)
        {
            ++scanned;
            host.getKnownPlugins().scanAndAddFile (f, false, types, *format);
            if (formatName == "LV2" && types.size() > 1)
                break; // each LSP LV2 identifier finds the whole bundle
        }
        logMessage ("Scanned " + juce::String (scanned) + " " + formatName + " file(s) out of process: "
                    + juce::String (types.size()) + " plugins in " + juce::String (juce::Time::getMillisecondCounter() - start) + " ms");
        expect (types.size() > 0);

        auto find = [&types] (const juce::String& wanted) -> const juce::PluginDescription*
        {
            for (auto* d : types)
                if (d->name == wanted || d->name == "LSP " + wanted)
                    return d;
            return nullptr;
        };
        set.eq = find ("Parametric Equalizer x8 Stereo");
        set.eqMono = find ("Parametric Equalizer x8 Mono");
        set.compressor = find ("Compressor Stereo");
        set.limiter = find ("Limiter Stereo");
        set.delay = find ("Delay Compensator Stereo");
        set.sidechain = find ("Sidechain Compressor Stereo");
        expect (set.usable(), "the LSP plugins this test uses weren't all found");
        if (! set.usable())
            for (int i = 0; i < juce::jmin (8, types.size()); ++i)
                logMessage ("  found " + types[i]->name);
        return set;
    }

    void configure (PluginHost& host, bool sandboxed)
    {
        auto options = host.getSandboxOptions();
        options.helper = juce::File (LIVEDAW_TEST_PLUGIN_HOST);
        options.minWaitMs = 2000.0; // the offline rig runs blocks back to back
        host.setSandboxOptions (options);
        host.setSandboxEnabled (sandboxed);
    }

    std::unique_ptr<juce::AudioPluginInstance> create (PluginHost& host, const juce::PluginDescription& d)
    {
        juce::String error;
        auto p = host.createInstance (d, kRate, kBlock, error);
        expect (p != nullptr, tag + "could not create " + d.name + ": " + error);
        if (p != nullptr && host.isSandboxEnabled())
            expect (dynamic_cast<SandboxedPlugin*> (p.get()) != nullptr, "not sandboxed");
        return p;
    }

    /** Runs the rig and the message loop until a latency change has gone round
        (VST3: restartComponent on the message thread; the sandbox: the helper's
        and SandboxedPlugin's timers). */
    static void settle (ToneRig& rig)
    {
        for (int i = 0; i < 8; ++i)
        {
            rig.run (20);
            pumpMessages (100);
        }
        rig.engine.updateLatencyCompensation();
    }

    void runSuite (PluginHost& host, const LspSet& lsp, bool sandboxed)
    {
        tag = lsp.format + (sandboxed ? " (sandbox): " : " (in-process): ");


        //======================================================================
        beginTest (tag + "an LSP EQ on a channel insert: output gain and a bell cut measure as set");
        {
            ToneRig rig;
            rig.tones = { { 1000.0, 0.1f }, { 100.0, 0.1f } };
            rig.run (60);
            const float dry1k = rig.toneDb (1000.0), dry100 = rig.toneDb (100.0);

            auto& fx = rig.engine.getFx();
            auto plugin = create (host, *lsp.eq);
            if (plugin == nullptr)
                return;
            auto* eq = plugin.get();
            fx.setPlugin (0, std::move (plugin));
            rig.insert (0, 0);
            expect (fx.isSandboxed (0) == sandboxed);

            rig.run (200);
            expect (rig.allFinite() && ! fx.isFaulted (0));
            logMessage (tag + "flat EQ: 1 kHz " + juce::String (rig.toneDb (1000.0) - dry1k, 2) + " dB, 100 Hz "
                        + juce::String (rig.toneDb (100.0) - dry100, 2) + " dB");
            expectWithinAbsoluteError (rig.toneDb (1000.0), dry1k, 0.05f);

            // A parameter change from Sidecar: Output gain -12 dB.
            expect (setGainDb (*eq, "Output gain", -12.0f), "no Output gain");
            pumpMessages (50);
            rig.run (200);
            const float gain = rig.toneDb (1000.0) - dry1k;
            logMessage (tag + "Output gain set to " + paramText (*eq, "Output gain") + ": measured " + juce::String (gain, 2)
                        + " dB at 1 kHz, " + juce::String (rig.toneDb (100.0) - dry100, 2) + " dB at 100 Hz");
            expectWithinAbsoluteError (gain, -12.0f, 0.1f);
            expectWithinAbsoluteError (rig.toneDb (100.0) - dry100, -12.0f, 0.1f);
            expect (setGainDb (*eq, "Output gain", 0.0f));

            // Band 0: a -12 dB bell at 1 kHz.
            expect (setChoice (*eq, "Filter type 0", "Bell"), "no Bell filter");
            expect (setDisplayed (*eq, "Frequency 0", 1000.0f));
            expect (setGainDb (*eq, "Gain 0", -12.0f));
            expect (setDisplayed (*eq, "Quality factor 0", 4.0f));
            pumpMessages (50);
            rig.run (300);
            const float cut1k = rig.toneDb (1000.0) - dry1k, cut100 = rig.toneDb (100.0) - dry100;
            logMessage (tag + "bell " + paramText (*eq, "Frequency 0") + " Hz, " + paramText (*eq, "Gain 0") + ", Q "
                        + paramText (*eq, "Quality factor 0") + ": measured " + juce::String (cut1k, 2) + " dB at 1 kHz, "
                        + juce::String (cut100, 2) + " dB at 100 Hz");
            expectWithinAbsoluteError (cut1k, -12.0f, 0.3f);
            expectWithinAbsoluteError (cut100, 0.0f, 0.3f);

            //==================================================================
            beginTest (tag + "bypass gives back the dry signal");
            {
                ToneRig ref;
                ref.tones = rig.tones;
                ref.run (60);
                fx.params (0).bypass = true;
                rig.engine.updateLatencyCompensation();
                rig.t = ref.t - 60 * kBlock; // the same tone phase as the reference
                rig.run (60);
                float maxDiff = 0.0f;
                for (size_t i = 0; i < rig.left.size(); ++i)
                    maxDiff = juce::jmax (maxDiff, std::abs (rig.left[i] - ref.left[i]));
                logMessage (tag + "slot BYPASS: max difference from the dry signal " + juce::String (maxDiff, 7)
                            + ", 1 kHz " + juce::String (rig.toneDb (1000.0) - dry1k, 3) + " dB");
                expectLessThan (maxDiff, 1.0e-5f);
                fx.params (0).bypass = false;
                rig.engine.updateLatencyCompensation();
                rig.run (100);
                expectWithinAbsoluteError (rig.toneDb (1000.0) - dry1k, -12.0f, 0.3f);

                // The plugin window's BYP (a chain member bypassed).
                fx.setChainMemberBypassed (0, 0, true);
                rig.run (100);
                logMessage (tag + "plugin BYP: 1 kHz " + juce::String (rig.toneDb (1000.0) - dry1k, 3) + " dB");
                expectWithinAbsoluteError (rig.toneDb (1000.0), dry1k, 0.02f);
                fx.setChainMemberBypassed (0, 0, false);
                rig.run (100);
                expectWithinAbsoluteError (rig.toneDb (1000.0) - dry1k, -12.0f, 0.3f);

                // The plugin's own bypass parameter (VST3: "Bypass"; LV2: "Enabled").
                if (findParam (*fx.getChainMember (0, 0), "Bypass") != nullptr)
                {
                    auto* member = fx.getChainMember (0, 0);
                    expect (setChoice (*member, "Bypass", "on"));
                    pumpMessages (50);
                    rig.run (200);
                    logMessage (tag + "the plugin's Bypass parameter: 1 kHz " + juce::String (rig.toneDb (1000.0) - dry1k, 3) + " dB");
                    expectWithinAbsoluteError (rig.toneDb (1000.0), dry1k, 0.1f);
                    expect (setChoice (*member, "Bypass", "off"));
                }
            }
        }

        //======================================================================
        beginTest (tag + "a mono plugin on a stereo channel processes both sides");
        if (lsp.eqMono != nullptr)
        {
            ToneRig rig;
            auto& ch = rig.channel (0);
            ch.params.stereo = true;
            rig.run (60);
            const float dryL = rig.toneDb (1000.0), dryR = rig.toneDb (1000.0, true);
            auto plugin = create (host, *lsp.eqMono);
            if (plugin != nullptr)
            {
                auto* eq = plugin.get();
                rig.engine.getFx().setPlugin (1, std::move (plugin));
                rig.insert (0, 1);
                expect (setGainDb (*eq, "Output gain", -12.0f));
                pumpMessages (50);
                rig.run (200);
                const float l = rig.toneDb (1000.0) - dryL, r = rig.toneDb (1000.0, true) - dryR;
                logMessage (tag + "mono EQ, Output gain -12 dB on a stereo channel: L " + juce::String (l, 2) + " dB, R "
                            + juce::String (r, 2) + " dB");
                expectWithinAbsoluteError (l, -12.0f, 0.2f);
                expectWithinAbsoluteError (r, -12.0f, 0.2f);
            }
        }

        //======================================================================
        beginTest (tag + "an LSP compressor reduces the level");
        {
            ToneRig rig;
            rig.tones = { { 1000.0, 0.5f } };
            rig.run (60);
            const float dry = rig.toneDb (1000.0);
            auto plugin = create (host, *lsp.compressor);
            if (plugin != nullptr)
            {
                auto* comp = plugin.get();
                rig.engine.getFx().setPlugin (2, std::move (plugin));
                rig.insert (0, 2);
                rig.run (200);
                const float defaults = rig.toneDb (1000.0) - dry;
                expect (setGainDb (*comp, "Attack threshold", -30.0f));
                expect (setDisplayed (*comp, "Ratio", 10.0f));
                pumpMessages (50);
                rig.run (400);
                const float reduced = rig.toneDb (1000.0) - dry;
                logMessage (tag + "compressor (defaults: threshold -12 dB, ratio 4) " + juce::String (defaults, 2)
                            + " dB; threshold " + paramText (*comp, "Attack threshold") + ", ratio " + paramText (*comp, "Ratio") + ": "
                            + juce::String (reduced, 2) + " dB on a -6 dBFS 1 kHz tone");
                expectLessThan (defaults, -1.0f);
                expectLessThan (reduced, defaults - 3.0f);
                expect (rig.allFinite() && ! rig.engine.getFx().isFaulted (2));
            }
        }

        //======================================================================
        beginTest (tag + "delay compensation follows the latency an LSP limiter reports");
        {
            ToneRig rig (2);
            auto& fx = rig.engine.getFx();
            auto plugin = create (host, *lsp.limiter);
            if (plugin != nullptr)
            {
                auto* limiter = plugin.get();
                fx.setPlugin (3, std::move (plugin));
                rig.insert (0, 3);
                rig.tones = { { 1000.0, 0.05f } };
                settle (rig); // the plugin reports its lookahead once it runs
                const int base = sandboxed ? kBlock : 0;
                const int reported = limiter->getLatencySamples();
                logMessage (tag + "limiter lookahead " + paramText (*limiter, "Lookahead") + ": reports " + juce::String (reported)
                            + " samples (slot " + juce::String (fx.getLatency (3)) + ")");
                expectEquals (reported - base, 240); // 5 ms at 48 kHz
                expectEquals (fx.getLatency (3), reported);

                auto peaks = impulseAtMain (rig.engine, 0.1f, 2 + reported / kBlock + 2);
                logMessage (tag + "total compensation " + juce::String (rig.engine.getTotalCompensation()) + ", "
                            + juce::String ((int) peaks.size()) + " peak(s), first at "
                            + (peaks.empty() ? juce::String ("-") : juce::String (peaks[0].first)));
                expectEquals (rig.engine.getTotalCompensation(), reported);
                expectEquals ((int) peaks.size(), 1);
                if (! peaks.empty())
                {
                    expectEquals (peaks[0].first, reported);
                    expectWithinAbsoluteError (peaks[0].second, 0.2f * test::kCentre, 0.01f);
                }

                // A latency change (Lookahead 10 ms) is followed.
                expect (setDisplayed (*limiter, "Lookahead", 10.0f));
                settle (rig);
                logMessage (tag + "lookahead " + paramText (*limiter, "Lookahead") + ": reports " + juce::String (limiter->getLatencySamples())
                            + ", slot " + juce::String (fx.getLatency (3)));
                expectWithinAbsoluteError (limiter->getLatencySamples() - base, 480, 1); // the knob lands within a sample
                expectEquals (fx.getLatency (3), limiter->getLatencySamples());
                peaks = impulseAtMain (rig.engine, 0.1f, 2 + limiter->getLatencySamples() / kBlock + 2);
                expectEquals ((int) peaks.size(), 1);
                if (! peaks.empty())
                    expectEquals (peaks[0].first, limiter->getLatencySamples());

                // Bypassed, it adds none.
                fx.params (3).bypass = true;
                rig.engine.updateLatencyCompensation();
                expectEquals (rig.engine.getTotalCompensation(), 0);
                fx.params (3).bypass = false;
            }
        }

        //======================================================================
        beginTest (tag + "a plugin with a side-chain input runs (the side chain silent)");
        if (lsp.sidechain != nullptr)
        {
            ToneRig rig;
            rig.tones = { { 1000.0, 0.5f } };
            rig.run (60);
            const float dry = rig.toneDb (1000.0);
            auto plugin = create (host, *lsp.sidechain);
            if (plugin != nullptr)
            {
                logMessage (tag + lsp.sidechain->name + ": " + juce::String (plugin->getTotalNumInputChannels()) + " inputs");
                rig.engine.getFx().setPlugin (4, std::move (plugin));
                rig.insert (0, 4);
                rig.run (200);
                logMessage (tag + "side-chain compressor (internal side chain by default): " + juce::String (rig.toneDb (1000.0) - dry, 2) + " dB");
                expect (rig.allFinite() && ! rig.engine.getFx().isFaulted (4));
            }
        }

        //======================================================================
        beginTest (tag + "a chain of three real plugins runs in order, and a show keeps it");
        {
            ToneRig rig;
            auto& fx = rig.engine.getFx();
            rig.run (60);
            const float dry = rig.toneDb (1000.0);
            auto eq = create (host, *lsp.eq), limiter = create (host, *lsp.limiter), delay = create (host, *lsp.delay);
            if (eq == nullptr || limiter == nullptr || delay == nullptr)
                return;
            fx.setPlugin (5, std::move (eq));
            expect (fx.addToChain (5, std::move (limiter)));
            expect (fx.addToChain (5, std::move (delay)));
            rig.insert (0, 5);
            expectEquals (fx.getNumChainMembers (5), 3);
            if (fx.getNumChainMembers (5) != 3)
                return;
            // EQ +12 dB, then a limiter at -6 dB, then -3 dB (set as the plugin window does, while they run).
            expect (setGainDb (*fx.getChainMember (5, 0), "Output gain", 12.0f));
            expect (setGainDb (*fx.getChainMember (5, 1), "Threshold", -6.0f));
            expect (setChoice (*fx.getChainMember (5, 1), "Gain boost", "off"));
            setChoice (*fx.getChainMember (5, 1), "Automatic level regulation", "off");
            expect (setGainDb (*fx.getChainMember (5, 2), "Wet amount", -3.0f));
            pumpMessages (50);
            rig.run (400);
            const float eqFirst = rig.peakDb();
            logMessage (tag + "EQ +12 > limiter -6 > -3 dB on a -12 dBFS tone: peak " + juce::String (eqFirst, 2) + " dBFS ("
                        + fx.getPlugin (5)->getName() + ")");
            expectWithinAbsoluteError (eqFirst, -9.0f, 1.5f);

            // The limiter first: the tone passes it, then +12 and -3.
            expect (fx.moveInChain (5, 1, 0));
            rig.run (400);
            const float limiterFirst = rig.peakDb();
            logMessage (tag + "limiter > EQ > -3 dB: peak " + juce::String (limiterFirst, 2) + " dBFS, 1 kHz "
                        + juce::String (rig.toneDb (1000.0) - dry, 2) + " dB");
            expectWithinAbsoluteError (rig.toneDb (1000.0) - dry, 9.0f, 0.3f);
            expectGreaterThan (limiterFirst, eqFirst + 5.0f);
            expect (! fx.isFaulted (5) && fx.getNumFaultedChainMembers (5) == 0);
            fx.setChainMemberView (5, 1, { 75, false });
            const auto order = fx.getChain (5)->describeMembers();

            Show show (rig.engine, host);
            const auto xml = show.toXml();
            expect (xml->toString().contains (PluginChain::kXmlTag));
            // SIDECAR_REAL_PLUGIN_SHOW_DIR: keep the show (to open in the app).
            if (const auto dir = juce::SystemStats::getEnvironmentVariable ("SIDECAR_REAL_PLUGIN_SHOW_DIR", {}); dir.isNotEmpty())
                juce::File (dir).getChildFile (lsp.format + (sandboxed ? "-sandbox" : "") + ".lshow").replaceWithText (show.toXmlString());

            ToneRig rig2;
            Show loaded (rig2.engine, host);
            const auto result = loaded.loadFromXml (*xml);
            expect (result.wasOk(), result.getErrorMessage());
            auto& fx2 = rig2.engine.getFx();
            expectEquals (fx2.getNumChainMembers (5), 3);
            if (fx2.getNumChainMembers (5) == 3)
            {
                rig2.tones = rig.tones;
                rig2.run (400); // a sandboxed plugin's values are mirrored from its helper meanwhile
                pumpMessages (300);
                expectEquals (fx2.getChain (5)->describeMembers(), order);
                expectEquals (fx2.getChainMemberView (5, 1).zoom, 75);
                expect (fx2.isSandboxed (5) == sandboxed);
                for (int i = 0; i < 3; ++i)
                    for (auto paramName : { "Output gain", "Threshold", "Wet amount", "Gain boost" })
                        if (findParam (*fx.getChainMember (5, i), paramName) != nullptr)
                            expectEquals (paramText (*fx2.getChainMember (5, i), paramName), paramText (*fx.getChainMember (5, i), paramName),
                                          juce::String ("member ") + juce::String (i) + " " + paramName);
                rig2.run (100);
                logMessage (tag + "after a show save and load (" + order + "): 1 kHz " + juce::String (rig2.toneDb (1000.0) - dry, 2)
                            + " dB (before " + juce::String (rig.toneDb (1000.0) - dry, 2) + " dB)");
                expectWithinAbsoluteError (rig2.toneDb (1000.0), rig.toneDb (1000.0), 0.1f);
            }

            //==================================================================
            beginTest (tag + "editors of real plugins, and the plugin window with zoom");
            if (! hasDisplay())
            {
                logMessage ("No display: skipping the editors");
            }
            else
            {
                checkEditors (fx, 5, sandboxed);
            }
        }
    }

    void checkEditors (FxSlots& fx, int slot, bool sandboxed)
    {
        using V = ui::PluginChainView;
        for (int i = 0; i < fx.getNumChainMembers (slot); ++i)
        {
            auto* p = fx.getChainMember (slot, i);
            expect (p->hasEditor());
            std::unique_ptr<juce::AudioProcessorEditor> editor (p->createEditorAndMakeActive());
            expect (editor != nullptr, tag + p->getName() + ": no editor");
            if (editor == nullptr)
                continue;
            logMessage (tag + p->getName() + " editor " + juce::String (editor->getWidth()) + " x " + juce::String (editor->getHeight())
                        + (editor->isResizable() ? ", resizable" : ", fixed size"));
            expect (editor->getWidth() > 0 && editor->getHeight() > 0);
            pumpMessages (200);
            p->editorBeingDeleted (editor.get());
            editor.reset();
            pumpMessages (50);
        }

        fx.setChainMemberView (slot, 1, {}); // all at 100 %
        auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
        const auto area = display != nullptr ? display->userBounds.toNearestInt() : juce::Rectangle<int> (0, 0, 1024, 600);
        {
            // As the app's plugin window (PluginChainWindow) holds it.
            V view (fx, slot, {});
            view.setMaxSize ({ area.getWidth() - 40, area.getHeight() - 80 });
            juce::DocumentWindow window ("FX " + juce::String (slot + 1), juce::Colours::black, juce::DocumentWindow::closeButton);
            window.setUsingNativeTitleBar (true);
            window.setContentNonOwned (&view, true);
            window.setTopLeftPosition (area.getTopLeft() + juce::Point<int> (10, 10));
            window.setVisible (true);
            pumpMessages (500);
            expectEquals (view.getNumCards(), fx.getNumChainMembers (slot));
            for (int i = 0; i < view.getNumCards(); ++i)
            {
                const auto method = view.getZoomMethod (i);
                const juce::String names[] = { "resize", "host scale (setScaleFactor)", "transform", "fixed" };
                const auto before = view.getEditorBounds (i);
                view.setZoom (i, 67);
                pumpMessages (1000); // a host scale the editor doesn't follow is dropped by the view's timer
                view.refresh();
                const auto after = view.getEditorBounds (i);
                const auto finalMethod = view.getZoomMethod (i);
                logMessage (tag + fx.getChainMember (slot, i)->getName() + ": zoom method " + names[(int) method]
                            + (finalMethod != method ? " -> " + names[(int) finalMethod] : juce::String()) + "; editor "
                            + juce::String (before.getWidth()) + " x " + juce::String (before.getHeight()) + " at 100 %, "
                            + juce::String (after.getWidth()) + " x " + juce::String (after.getHeight()) + " at 67 %");
                if (finalMethod == V::ZoomMethod::fixed)
                    expect (after.getWidth() == before.getWidth() && after.getHeight() == before.getHeight());
                else
                    expect (after.getWidth() < before.getWidth() && after.getHeight() < before.getHeight(),
                            "the editor didn't get smaller");
                if (sandboxed)
                    expect (method == V::ZoomMethod::transform); // the sandbox's own panel
                view.setZoom (i, 100);
                pumpMessages (100);
            }
            window.clearContentComponent(); // as ~PluginChainWindow
        }
        pumpMessages (100);
    }
};

static RealPluginTests realPluginTests;
