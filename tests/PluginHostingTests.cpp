#include "TestHelpers.h"
#include "engine/PluginHost.h"
#include "engine/SandboxedPlugin.h"
#include "sandbox/TestPlugins.h"

using namespace livedaw;
using livedaw::test::OfflineRig;

/** Integration test against real installed plugins (LV2/VST3). Skipped when none
    are installed, so it's safe to run anywhere. */
class PluginHostingTests final : public juce::UnitTest
{
public:
    PluginHostingTests() : juce::UnitTest ("Plugin hosting", "LiveDAW") {}

    void runTest() override
    {
        PluginHost host (nullptr);
        auto& fm = host.getFormatManager();

        juce::StringArray candidates;
        juce::AudioPluginFormat* format = nullptr;

        for (auto* f : fm.getFormats())
        {
            // AudioUnits are skipped: scanning them from this console test runner
            // (no NSApplication) returns nothing. Out-of-process AU scanning still
            // needs verifying in the real app on a Mac.
            if (f->getName() == "AudioUnit")
                continue;

            const auto found = f->searchPathsForPlugins (f->getDefaultLocationsToSearch(), true, false);
            if (! found.isEmpty())
            {
                candidates = found;
                format = f;
                break;
            }
        }

        beginTest ("Out-of-process scan and live hosting of a real plugin");

        if (format == nullptr)
        {
            logMessage ("No plugins installed; skipping");
            expect (true);
            return;
        }

        logMessage ("Found " + juce::String (candidates.size()) + " " + format->getName() + " plugins");

        // Scan a handful through the out-of-process scanner (the custom scanner on the list).
        juce::OwnedArray<juce::PluginDescription> types;
        for (int i = 0; i < juce::jmin (6, candidates.size()); ++i)
            host.getKnownPlugins().scanAndAddFile (candidates[i], false, types, *format);

        logMessage ("Scanned " + juce::String (types.size()) + " plugin types out of process");
        expect (types.size() > 0);

        // Pick an effect with stereo-capable I/O and run audio through it on a strip.
        for (auto* desc : types)
        {
            if (desc->isInstrument || desc->numInputChannels == 0)
                continue;

            MixerEngine engine (1);
            OfflineRig rig (engine, 2, 2);
            test::setChannels (engine, 1);

            juce::String error;
            auto instance = host.createInstance (*desc, 48000.0, OfflineRig::kBlock, error);
            if (instance == nullptr)
            {
                logMessage ("Could not instantiate " + desc->name + ": " + error);
                continue;
            }

            logMessage ("Hosting " + desc->name);
            auto& strip = *engine.getChannels()[0];
            auto& fx = engine.getFx();
            fx.setPlugin (0, std::move (instance));
            strip.params.preInsert.on = true;
            strip.params.preInsert.fx = 0;
            rig.setInput (0, 0.25f);
            rig.run (200);

            expect (! fx.isFaulted (0), "plugin faulted");
            expect (std::isfinite (rig.out (0)));
            expect (fx.getLoad (0) >= 0.0f);

            // Plugin state survives a capture/restore cycle.
            juce::MemoryBlock state;
            fx.getPlugin (0)->getStateInformation (state);
            logMessage ("State size: " + juce::String ((int) state.getSize()) + " bytes");

            const float inProcessOut = rig.out (0);
            fx.setPlugin (0, nullptr);
            engine.getGarbage().collectAllNow();

            // The same plugin in the plugin sandbox (the real helper): same steady-state
            // output, state through the sandbox.
            beginTest ("A real plugin in the plugin sandbox");
            auto options = host.getSandboxOptions();
            options.helper = juce::File (LIVEDAW_TEST_PLUGIN_HOST);
            options.minWaitMs = 2000.0; // the offline rig runs blocks back to back
            host.setSandboxOptions (options);
            host.setSandboxEnabled (true);
            auto sandboxed = host.createInstance (*desc, 48000.0, OfflineRig::kBlock, error);
            host.setSandboxEnabled (false);
            expect (dynamic_cast<SandboxedPlugin*> (sandboxed.get()) != nullptr, error);
            if (sandboxed == nullptr)
                return;

            fx.setPlugin (0, std::move (sandboxed));
            fx.clearFlags (0);
            rig.run (200);
            expect (! fx.isFaulted (0), "sandboxed plugin faulted");
            expectWithinAbsoluteError (rig.out (0), inProcessOut, 1.0e-4f);

            juce::MemoryBlock sandboxState;
            fx.getPlugin (0)->getStateInformation (sandboxState);
            expectEquals ((int) sandboxState.getSize(), (int) state.getSize());
            fx.getPlugin (0)->setStateInformation (state.getData(), (int) state.getSize());
            rig.run (20);
            expect (! fx.isFaulted (0), "faulted after a state restore");

            fx.setPlugin (0, nullptr);
            engine.getGarbage().collectAllNow();
            return;
        }

        logMessage ("No suitable effect among the scanned plugins");
    }
};

static PluginHostingTests pluginHostingTests;

//==============================================================================
namespace
{
    /** A plugin with latency, as a lookahead limiter or a linear-phase EQ has:
        it delays its input by its reported latency, which can change while it
        runs (setLatencySamples notifies the host, as JUCE plugin wrappers do). */
    class LatencyPlugin final : public juce::AudioPluginInstance
    {
    public:
        explicit LatencyPlugin (int latency)
            : juce::AudioPluginInstance (BusesProperties()
                                             .withInput ("In", juce::AudioChannelSet::stereo())
                                             .withOutput ("Out", juce::AudioChannelSet::stereo()))
        {
            history.setSize (2, kHistory);
            history.clear();
            setLatencySamples (latency);
        }

        void fillInPluginDescription (juce::PluginDescription& d) const override
        {
            d.name = getName();
            d.pluginFormatName = "Fake";
            d.fileOrIdentifier = "fake-latency";
            d.numInputChannels = d.numOutputChannels = 2;
        }

        const juce::String getName() const override { return "Latency"; }
        void prepareToPlay (double, int) override {}
        void releaseResources() override {}

        using juce::AudioPluginInstance::processBlock;

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            const int delay = juce::jlimit (0, kHistory - 1, getLatencySamples());
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                for (int ch = 0; ch < 2; ++ch)
                {
                    history.setSample (ch, pos, buffer.getSample (ch, i));
                    buffer.setSample (ch, i, history.getSample (ch, (pos + kHistory - delay) % kHistory));
                }
                pos = (pos + 1) % kHistory;
            }
        }

        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return {}; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}

    private:
        static constexpr int kHistory = 16384;
        juce::AudioBuffer<float> history;
        int pos = 0;
    };
} // namespace

/** Plugin delay compensation with hosted plugins: their reported latency
    (and its changes) lines up parallel paths at the buses and mains. */
class LatencyCompensationTests final : public juce::UnitTest
{
public:
    LatencyCompensationTests() : juce::UnitTest ("Plugin delay compensation", "LiveDAW") {}

    /** Main 1 left after a unit impulse on input 1 (settled on silence first),
        as (sample, value) for every non-silent sample. */
    static std::vector<std::pair<int, float>> impulse (MixerEngine& engine, int blocks)
    {
        constexpr int n = OfflineRig::kBlock;
        engine.updateLatencyCompensation();
        std::vector<float> in ((size_t) n, 0.0f), outL ((size_t) n), outR ((size_t) n);
        const float* ins[] = { in.data() };
        float* outs[] = { outL.data(), outR.data() };
        for (int b = 0; b < 2 * MixerEngine::kMaxCompensation / n + 4; ++b)
            engine.processBlock (ins, 1, outs, 2, n);
        std::vector<std::pair<int, float>> peaks;
        for (int b = 0; b < blocks; ++b)
        {
            in[0] = b == 0 ? 1.0f : 0.0f;
            engine.processBlock (ins, 1, outs, 2, n);
            for (int i = 0; i < n; ++i)
                if (std::abs (outL[(size_t) i]) > 1.0e-4f)
                    peaks.push_back ({ b * n + i, outL[(size_t) i] });
        }
        return peaks;
    }

    void runTest() override
    {
        using livedaw::test::kCentre;
        MixerEngine engine (0);
        OfflineRig rig (engine, 1, 2);
        engine.outputProtection = false; // two channels sum above 0 dBFS
        test::setChannels (engine, 2);
        for (auto& ch : engine.getChannels())
            ch->params.inputLeft = 0;
        auto& fx = engine.getFx();
        auto* plugin = new LatencyPlugin (100);
        fx.setPlugin (2, std::unique_ptr<juce::AudioPluginInstance> (plugin));
        auto& ch1 = *engine.getChannels()[0];
        ch1.params.preInsert.on = true;
        ch1.params.preInsert.fx = 2;

        beginTest ("Compensation is on by default and lines up channels at Main 1");
        expect (engine.isLatencyCompensationOn());
        expectEquals (fx.getLatency (2), 100);
        auto peaks = impulse (engine, 4);
        expectEquals ((int) peaks.size(), 1);
        if (! peaks.empty())
        {
            expectEquals (peaks[0].first, 100);
            expectWithinAbsoluteError (peaks[0].second, 2.0f * kCentre, 1.0e-4f);
        }
        expectEquals (engine.getTotalCompensation(), 100);
        expectEquals (ch1.getLatency(), 100);
        expectEquals (engine.getMain().getLatency(), 100);

        beginTest ("A latency change reported by the plugin is followed");
        plugin->setLatencySamples (300); // the listener passes it on at once
        expectEquals (fx.getLatency (2), 300);
        peaks = impulse (engine, 5);
        expectEquals ((int) peaks.size(), 1);
        if (! peaks.empty())
            expectEquals (peaks[0].first, 300);

        beginTest ("A bus with a plugin lines up with the direct path");
        auto& ch2 = *engine.getChannels()[1];
        ch2.params.mains[0].on = false;
        ch1.params.preInsert.on = false;
        auto& bus = *engine.getBusInSlot (0);
        auto& send = ch2.sends[0];
        send.enabled = true;
        send.mode = 1; // POST
        bus.params.mains[0].on = true;
        bus.params.preInsert.on = true;
        bus.params.preInsert.fx = 2;
        plugin->setLatencySamples (64);
        peaks = impulse (engine, 3);
        expectEquals ((int) peaks.size(), 1);
        if (! peaks.empty())
            expectEquals (peaks[0].first, 64);
        expectEquals (engine.getMain().getLatency(), 64);

        beginTest ("DELAY COMPENSATION off leaves the paths apart");
        engine.setLatencyCompensation (false);
        peaks = impulse (engine, 3);
        expectEquals ((int) peaks.size(), 2);
        if (peaks.size() == 2)
        {
            expectEquals (peaks[0].first, 0);
            expectEquals (peaks[1].first, 64);
        }
        expectEquals (engine.getTotalCompensation(), 0);
        engine.setLatencyCompensation (true);

        beginTest ("Compensation beyond the maximum is capped and reported");
        plugin->setLatencySamples (MixerEngine::kMaxCompensation + 100);
        engine.updateLatencyCompensation();
        expect (engine.isCompensationLimited());
        plugin->setLatencySamples (10);
        engine.updateLatencyCompensation();
        expect (! engine.isCompensationLimited());

        beginTest ("A removed plugin no longer counts");
        fx.setPlugin (2, nullptr);
        engine.getGarbage().collectAllNow();
        expectEquals (fx.getLatency (2), 0);
        engine.updateLatencyCompensation();
        expectEquals (engine.getTotalCompensation(), 0);
    }
};

static LatencyCompensationTests latencyCompensationTests;

//==============================================================================
/** The EXT device's outboard insert (WING manual 11.11 p. 164) beside plugin
    hosting: FX SEND out, RETURN back, compensated like a plugin's latency. */
class OutboardInsertTests final : public juce::UnitTest
{
public:
    OutboardInsertTests() : juce::UnitTest ("Outboard inserts", "LiveDAW") {}

    void runTest() override
    {
        constexpr int n = OfflineRig::kBlock;
        MixerEngine engine (0);
        OfflineRig rig (engine, 3, 4); // in 1: the source, ins 2-3: the gear's return; outs 3-4: FX SEND
        engine.outputProtection = false;
        test::setChannels (engine, 2);
        for (auto& ch : engine.getChannels())
            ch->params.inputLeft = 0;
        for (auto& aux : engine.getStrips (StripKind::Aux))
            aux->params.inputLeft = aux->params.inputRight = -1;
        auto& fx = engine.getFx();

        beginTest ("SOURCE OUTBOARD makes the EXT device an outboard insert");
        fx.setOutboard (0);
        expect (fx.isOutboard (0) && fx.isLoaded (0) && ! fx.hasPlugin (0));
        expect (! fx.hasReturn (0));
        auto& p = fx.params (0);
        p.values[ld::ext::kGroup] = 3.0f; // A
        p.values[ld::ext::kInput] = 2.0f;
        p.values[ld::ext::kMode] = 1.0f; // STEREO
        p.sendLeft = 2;
        p.sendRight = 3;
        expect (fx.hasReturn (0));
        expectEquals (fx.returnLabel (0), juce::String ("A 2/3"));
        expectEquals (fx.getLatency (0), n); // one block: the offline rig reports no I/O latency
        expect (fx.describe (0).contains ("outboard"));
        auto& ch1 = *engine.getChannels()[0];
        ch1.params.preInsert.on = true;
        ch1.params.preInsert.fx = 0;

        beginTest ("The send goes out, the return comes back a block later, in time with the direct path");
        engine.updateLatencyCompensation();
        std::vector<std::vector<float>> in (3, std::vector<float> ((size_t) n, 0.0f)), out (4, std::vector<float> ((size_t) n, 0.0f));
        auto block = [&]
        {
            // The gear: what left on outputs 3-4 comes back on inputs 2-3 in the next block.
            in[1] = out[2];
            in[2] = out[3];
            const float* ins[] = { in[0].data(), in[1].data(), in[2].data() };
            float* outs[] = { out[0].data(), out[1].data(), out[2].data(), out[3].data() };
            engine.processBlock (ins, 3, outs, 4, n);
        };
        for (int b = 0; b < 2 * MixerEngine::kMaxCompensation / n + 4; ++b)
            block();
        std::vector<std::pair<int, float>> main, send;
        for (int b = 0; b < 4; ++b)
        {
            in[0][0] = b == 0 ? 1.0f : 0.0f;
            block();
            for (int i = 0; i < n; ++i)
            {
                if (std::abs (out[0][(size_t) i]) > 1.0e-4f)
                    main.push_back ({ b * n + i, out[0][(size_t) i] });
                if (std::abs (out[2][(size_t) i]) > 1.0e-4f)
                    send.push_back ({ b * n + i, out[2][(size_t) i] });
            }
        }
        expectEquals ((int) send.size(), 1);
        expectEquals ((int) main.size(), 1);
        if (main.size() == 1)
        {
            expectEquals (main[0].first, n);
            expectWithinAbsoluteError (main[0].second, 2.0f * livedaw::test::kCentre, 1.0e-4f);
        }
        expectEquals (engine.getTotalCompensation(), n);

        beginTest ("Loading a plugin makes it a plugin slot again; OUTBOARD removes the plugin");
        fx.setPlugin (0, std::make_unique<LatencyPlugin> (10));
        expect (fx.hasPlugin (0) && ! fx.isOutboard (0));
        expectEquals (p.values[ld::ext::kSource].load(), 0.0f);
        expectEquals (p.values[ld::ext::kGroup].load(), 3.0f, "the outboard settings stay");
        expectEquals (fx.getLatency (0), 10);
        fx.setOutboard (0);
        engine.getGarbage().collectAllNow();
        expect (fx.isOutboard (0) && ! fx.hasPlugin (0));
    }
};

static OutboardInsertTests outboardInsertTests;

//==============================================================================
namespace
{
    /** A plugin with mono main buses (no stereo layout), as LSP's "Mono" plugins:
        it scales its one channel and leaves any other alone, as a hosted plugin
        does with channels that aren't its own. */
    class MonoGainPlugin final : public juce::AudioPluginInstance
    {
    public:
        explicit MonoGainPlugin (float g)
            : juce::AudioPluginInstance (BusesProperties()
                                             .withInput ("In", juce::AudioChannelSet::mono())
                                             .withOutput ("Out", juce::AudioChannelSet::mono())),
              gain (g)
        {
        }

        bool isBusesLayoutSupported (const BusesLayout& l) const override
        {
            return l.getMainInputChannelSet() == juce::AudioChannelSet::mono() && l.getMainOutputChannelSet() == juce::AudioChannelSet::mono();
        }

        void fillInPluginDescription (juce::PluginDescription& d) const override
        {
            d.name = getName();
            d.pluginFormatName = "Fake";
            d.fileOrIdentifier = "fake-mono";
            d.numInputChannels = d.numOutputChannels = 1;
        }

        const juce::String getName() const override { return "Mono gain"; }
        void prepareToPlay (double, int) override {}
        void releaseResources() override {}

        using juce::AudioPluginInstance::processBlock;

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override { buffer.applyGain (0, 0, buffer.getNumSamples(), gain); }

        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return {}; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}

    private:
        float gain;
    };
} // namespace

/** Hosting fixes found with real plugins (see RealPluginTests). */
class PluginHostingFixTests final : public juce::UnitTest
{
public:
    PluginHostingFixTests() : juce::UnitTest ("Plugin hosting fixes", "LiveDAW") {}

    void runTest() override
    {
        beginTest ("A mono plugin on a stereo channel takes both sides and feeds both (alone and in a chain)");
        for (const bool chained : { false, true })
        {
            MixerEngine engine (0);
            OfflineRig rig (engine, 2, 2);
            test::setChannels (engine, 1);
            auto& strip = *engine.getChannels()[0];
            strip.params.stereo = true;
            strip.params.inputLeft = 0;
            strip.params.inputRight = 1;
            rig.setInput (0, 0.4f);
            rig.setInput (1, 0.2f);
            rig.run (4);
            const float dryL = rig.out (0), dryR = rig.out (1);
            expect (dryL > 0.0f && dryR > 0.0f);

            auto& fx = engine.getFx();
            fx.setPlugin (0, std::make_unique<MonoGainPlugin> (0.5f));
            if (chained)
                fx.addToChain (0, std::make_unique<test::FakePlugin> (1.0f));
            strip.params.preInsert.on = true;
            strip.params.preInsert.fx = 0;
            rig.run (4);
            // Both sides get half the mono sum (0.3), relative to their dry level.
            expectWithinAbsoluteError (rig.out (0), 0.5f * 0.3f / 0.4f * dryL, 1.0e-5f);
            expectWithinAbsoluteError (rig.out (1), 0.5f * 0.3f / 0.2f * dryR, 1.0e-5f);
            fx.setPlugin (0, nullptr);
            engine.getGarbage().collectAllNow();
        }

        beginTest ("A VST3 described by the library inside its bundle (Linux instances, shows) loads from the bundle");
        {
            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sidecar-vst3-test");
            const auto bundle = dir.getChildFile ("Some Plugin.vst3");
            const auto library = bundle.getChildFile ("Contents").getChildFile ("x86_64-linux").getChildFile ("Some Plugin.so");

            juce::PluginDescription d;
            d.pluginFormatName = "VST3";
            d.name = "Some Plugin";
            d.fileOrIdentifier = library.getFullPathName();
            expectEquals (PluginHost::resolveDescription (d).fileOrIdentifier, bundle.getFullPathName());
            expectEquals (PluginHost::resolveDescription (d).name, d.name);

            d.fileOrIdentifier = bundle.getFullPathName(); // already the bundle (a scan's)
            expectEquals (PluginHost::resolveDescription (d).fileOrIdentifier, bundle.getFullPathName());
            d.fileOrIdentifier = dir.getChildFile ("plain.so").getFullPathName(); // no bundle around it
            expectEquals (PluginHost::resolveDescription (d).fileOrIdentifier, d.fileOrIdentifier);
            d.pluginFormatName = "LV2";
            d.fileOrIdentifier = library.getFullPathName();
            expectEquals (PluginHost::resolveDescription (d).fileOrIdentifier, d.fileOrIdentifier);
            d.fileOrIdentifier = "http://lsp-plug.in/plugins/lv2/comp_delay_stereo"; // not a path
            expectEquals (PluginHost::resolveDescription (d).fileOrIdentifier, d.fileOrIdentifier);
        }

        beginTest ("A plugin Sidecar made is described by what it was made from, without asking it");
        {
            using sandbox::TestPluginFormat;
            PluginHost host (nullptr);
            host.getFormatManager().addFormat (std::make_unique<TestPluginFormat>());
            auto requested = TestPluginFormat::describe (TestPluginFormat::Kind::gain);
            requested.version = "9.9 (from the scan)"; // the instance itself doesn't say this
            juce::String error;
            auto plugin = host.createInstance (requested, 48000.0, 128, error);
            expect (plugin != nullptr, error);
            if (plugin != nullptr)
            {
                expect (plugin->getPluginDescription().version != requested.version);
                expectEquals (PluginHost::describe (*plugin).version, requested.version);

                // A plugin Sidecar didn't make (or one at a reused address) describes itself.
                test::FakePlugin fake;
                expectEquals (PluginHost::describe (fake).name, fake.getPluginDescription().name);
            }
        }
    }
};

static PluginHostingFixTests pluginHostingFixTests;
