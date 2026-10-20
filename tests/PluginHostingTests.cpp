#include "TestHelpers.h"
#include "engine/PluginHost.h"
#include "engine/SandboxedPlugin.h"

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
