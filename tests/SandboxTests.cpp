#include "TestHelpers.h"
#include "engine/PluginHost.h"
#include "engine/PluginScanner.h"
#include "engine/SandboxedPlugin.h"
#include "sandbox/TestPlugins.h"

#include <cstdlib>

using namespace livedaw;
using livedaw::sandbox::TestPluginFormat;
using livedaw::test::OfflineRig;

/** The plugin sandbox against the real helper (SidecarPluginHost) and its test
    plugins: sample-accurate processing, parameters and state through the sandbox,
    and plugins that crash or hang on demand. */
class SandboxTests final : public juce::UnitTest
{
public:
    SandboxTests() : juce::UnitTest ("Plugin sandbox", "LiveDAW") {}

    void runTest() override
    {
#if JUCE_WINDOWS
        _putenv_s (TestPluginFormat::kEnvironmentVariable, "1");
#else
        setenv (TestPluginFormat::kEnvironmentVariable, "1", 1);
#endif

        beginTest ("A plugin that hangs the out-of-process scan fails after the timeout; the next scan works");
        {
            OutOfProcessScanner scanner (2000);
            TestPluginFormat format;
            juce::OwnedArray<juce::PluginDescription> found;

            const auto start = juce::Time::getMillisecondCounterHiRes();
            expect (! scanner.findPluginTypesFor (format, found, TestPluginFormat::kHangingScanId));
            const auto elapsed = juce::Time::getMillisecondCounterHiRes() - start;
            expect (elapsed >= 2000.0 && elapsed < 20000.0, "took " + juce::String (elapsed) + " ms");
            expect (found.isEmpty());

            // A fresh scanner process takes over.
            const auto gain = TestPluginFormat::describe (TestPluginFormat::Kind::gain);
            expect (scanner.findPluginTypesFor (format, found, gain.fileOrIdentifier));
            expectEquals (found.size(), 1);
            if (! found.isEmpty())
                expectEquals (found[0]->fileOrIdentifier, gain.fileOrIdentifier);
            scanner.scanFinished();
        }

        beginTest ("A missing helper is an error, not a crash");
        {
            SandboxOptions o;
            o.helper = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("no-such-sidecar-helper");
            juce::String error;
            auto p = SandboxedPlugin::create (TestPluginFormat::describe (TestPluginFormat::Kind::gain), 48000.0, 128,
                                              std::make_shared<const SandboxOptions> (o), error);
            expect (p == nullptr);
            expect (error.contains ("missing"), error);
        }

        beginTest ("Processing through the sandbox matches in-process, one block later");
        {
            juce::String error;
            auto sandboxed = SandboxedPlugin::create (TestPluginFormat::describe (TestPluginFormat::Kind::gain), 48000.0, kBlock,
                                                      options (2000.0), error);
            expect (sandboxed != nullptr, error);
            if (sandboxed == nullptr)
                return;
            auto local = TestPluginFormat::create (TestPluginFormat::Kind::gain);

            expectEquals (sandboxed->getName(), juce::String ("Sidecar Test gain"));
            expectEquals (sandboxed->getParameters().size(), 3);
            expectEquals (sandboxed->getLatencySamples(), kBlock);

            // Automation from Sidecar: the same values on both.
            for (juce::AudioPluginInstance* p : { static_cast<juce::AudioPluginInstance*> (sandboxed.get()), local.get() })
            {
                p->getParameters()[0]->setValue (0.35f); // gain 0.7
                p->getParameters()[1]->setValue (0.6f);  // tone
                p->prepareToPlay (48000.0, kBlock);
            }
            expectEquals (sandboxed->getLatencySamples(), kBlock);

            // Varying block sizes (<= the prepared size) keep the latency at one prepared block.
            juce::Random rng (42);
            std::vector<float> in[2], outLocal[2], outSandbox[2];
            const int sizes[] = { 128, 128, 64, 100, 128, 1, 127, 128 };
            juce::MidiBuffer midi;
            for (int b = 0; b < 80; ++b)
            {
                const int n = sizes[b % 8];
                juce::AudioBuffer<float> a (2, n), s (2, n);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < n; ++i)
                    {
                        const float x = rng.nextFloat() * 2.0f - 1.0f;
                        a.setSample (ch, i, x);
                        s.setSample (ch, i, x);
                        in[ch].push_back (x);
                    }
                local->processBlock (a, midi);
                sandboxed->processBlock (s, midi);
                expect (! sandboxed->didLastBlockFail(), "block " + juce::String (b) + " failed");
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < n; ++i)
                    {
                        outLocal[ch].push_back (a.getSample (ch, i));
                        outSandbox[ch].push_back (s.getSample (ch, i));
                    }
            }

            int mismatches = 0;
            float silence = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
            {
                for (size_t i = 0; i < (size_t) kBlock; ++i)
                    silence = juce::jmax (silence, std::abs (outSandbox[ch][i]));
                for (size_t i = 0; i + (size_t) kBlock < outSandbox[ch].size(); ++i)
                    if (outSandbox[ch][i + (size_t) kBlock] != outLocal[ch][i])
                        ++mismatches;
            }
            expectEquals (silence, 0.0f, "the first block is the sandbox's latency");
            expectEquals (mismatches, 0, "sandboxed output differs from in-process output");

            // State through the sandbox: same as the in-process plugin's.
            juce::MemoryBlock remoteState, localState;
            sandboxed->getStateInformation (remoteState);
            local->getStateInformation (localState);
            expect (remoteState == localState, "state differs");

            // Restoring state updates the plugin and, once mirrored, Sidecar's parameter values.
            juce::MemoryBlock other;
            {
                juce::MemoryOutputStream out (other, false);
                out.writeFloat (0.125f);
                out.writeFloat (0.0f);
                out.writeFloat (0.0f);
            }
            sandboxed->setStateInformation (other.getData(), (int) other.getSize());
            auto* gain = sandboxed->getParameters()[0];
            const auto start = juce::Time::getMillisecondCounter();
            while (gain->getValue() != 0.125f && juce::Time::getMillisecondCounter() - start < 3000)
            {
                juce::Thread::sleep (20);
                sandboxed->supervise();
            }
            expectEquals (gain->getValue(), 0.125f);
            expectEquals (gain->getText (0.5f, 32), juce::String ("0.500"));

            juce::MemoryBlock back;
            sandboxed->getStateInformation (back);
            expect (back == other, "state didn't round-trip");

            // The plugin's editor opens in the helper (only where there is a display).
#if JUCE_LINUX
            const bool haveDisplay = juce::SystemStats::getEnvironmentVariable ("DISPLAY", {}).isNotEmpty();
#else
            const bool haveDisplay = false; // not run from the console test runner
#endif
            if (haveDisplay)
            {
                // (The answer comes through the message loop, which this runner doesn't spin:
                // check that the helper survives showing and closing the window.)
                sandboxed->openEditorWindow ("Sandbox test", nullptr);
                juce::Thread::sleep (1500);
                sandboxed->closeEditorWindow();
                juce::Thread::sleep (300);
                sandboxed->supervise();
                expect (sandboxed->getStatus() == SandboxedPlugin::Status::running);
                juce::MemoryBlock afterEditor;
                sandboxed->getStateInformation (afterEditor);
                expect (afterEditor == other, "helper lost after showing its editor");
            }
        }

        beginTest ("A parameter set right after loading keeps its value (the helper's values from loading don't overwrite it)");
        {
            juce::String error;
            auto sandboxed = SandboxedPlugin::create (TestPluginFormat::describe (TestPluginFormat::Kind::gain), 48000.0, kBlock,
                                                      options (2000.0), error);
            expect (sandboxed != nullptr, error);
            if (sandboxed != nullptr)
            {
                auto* gain = sandboxed->getParameters()[0];
                gain->setValueNotifyingHost (0.35f); // as a scene recall or a remote does at once
                const auto start = juce::Time::getMillisecondCounter();
                while (juce::Time::getMillisecondCounter() - start < 800) // past the 300 ms a local change wins
                {
                    juce::Thread::sleep (20);
                    sandboxed->supervise();
                }
                expectEquals (gain->getValue(), 0.35f);
                juce::MemoryBlock state;
                sandboxed->getStateInformation (state);
                juce::MemoryInputStream in (state, false);
                expectEquals (in.readFloat(), 0.35f, "the helper doesn't have it");
            }
        }

        beginTest ("The sandbox's block of latency is delay-compensated, FX MIX included");
        compensationTest();

        beginTest ("A plugin that crashes is faulted; the console keeps running and the plugin comes back");
        faultTest (TestPluginFormat::Kind::crash);

        beginTest ("A plugin that hangs is faulted, its helper killed and restarted");
        faultTest (TestPluginFormat::Kind::hang);

        beginTest ("A sandboxed plugin that crashes in a chain of three bypasses only itself and comes back");
        chainFaultTest();
    }

private:
    static constexpr int kBlock = OfflineRig::kBlock;
    juce::StringArray events;

    std::shared_ptr<const SandboxOptions> options (double minWaitMs)
    {
        SandboxOptions o;
        o.helper = juce::File (LIVEDAW_TEST_PLUGIN_HOST);
        o.minWaitMs = minWaitMs; // the offline rig runs blocks back to back
        o.hangTimeoutMs = 300;
        o.maxRestarts = 3;
        o.onEvent = [this] (const juce::String& message, bool) { events.add (message); };
        return std::make_shared<const SandboxOptions> (o);
    }

    /** Main 1 left after a unit impulse on input 1 (settled on silence first), as
        (sample, value) for every non-silent sample. */
    static std::vector<std::pair<int, float>> impulse (MixerEngine& engine, int blocks)
    {
        engine.updateLatencyCompensation();
        std::vector<float> in ((size_t) kBlock, 0.0f), outL ((size_t) kBlock), outR ((size_t) kBlock);
        const float* ins[] = { in.data() };
        float* outs[] = { outL.data(), outR.data() };
        for (int b = 0; b < 2 * MixerEngine::kMaxCompensation / kBlock + 4; ++b)
            engine.processBlock (ins, 1, outs, 2, kBlock);
        std::vector<std::pair<int, float>> peaks;
        for (int b = 0; b < blocks; ++b)
        {
            in[0] = b == 0 ? 1.0f : 0.0f;
            engine.processBlock (ins, 1, outs, 2, kBlock);
            for (int i = 0; i < kBlock; ++i)
                if (std::abs (outL[(size_t) i]) > 1.0e-4f)
                    peaks.push_back ({ b * kBlock + i, outL[(size_t) i] });
        }
        return peaks;
    }

    void compensationTest()
    {
        using livedaw::test::kCentre;
        MixerEngine engine (0);
        OfflineRig rig (engine, 1, 2);
        engine.outputProtection = false; // two channels sum above 0 dBFS
        test::setChannels (engine, 2);
        for (auto& ch : engine.getChannels())
            ch->params.inputLeft = 0;

        juce::String error;
        auto plugin = SandboxedPlugin::create (TestPluginFormat::describe (TestPluginFormat::Kind::gain), 48000.0, kBlock,
                                               options (2000.0), error);
        expect (plugin != nullptr, error);
        if (plugin == nullptr)
            return;
        plugin->getParameters()[0]->setValue (0.5f); // gain 1
        plugin->getParameters()[1]->setValue (0.0f); // no filtering: a pure one-block delay

        auto& fx = engine.getFx();
        fx.setPlugin (2, std::move (plugin));
        auto& ch1 = *engine.getChannels()[0];
        ch1.params.preInsert.on = true;
        ch1.params.preInsert.fx = 2;
        expectEquals (fx.getLatency (2), kBlock);

        // Channel 2 is delayed to meet channel 1's sandboxed insert: one impulse at Main 1.
        auto peaks = impulse (engine, 4);
        expectEquals ((int) peaks.size(), 1);
        if (! peaks.empty())
        {
            expectEquals (peaks[0].first, kBlock);
            expectWithinAbsoluteError (peaks[0].second, 2.0f * kCentre, 1.0e-4f);
        }
        expectEquals (engine.getTotalCompensation(), kBlock);

        // FX MIX 50 %: the dry half is delayed as much as the wet half.
        fx.params (2).mix = 50.0f;
        peaks = impulse (engine, 4);
        expectEquals ((int) peaks.size(), 1);
        if (! peaks.empty())
        {
            expectEquals (peaks[0].first, kBlock);
            expectWithinAbsoluteError (peaks[0].second, 2.0f * kCentre, 1.0e-4f);
        }
        expect (! fx.isFaulted (2));

        // In a chain with an in-process plugin and a second sandboxed one: the
        // latencies add up, the running helper carries on.
        fx.params (2).mix = 100.0f;
        auto local = TestPluginFormat::create (TestPluginFormat::Kind::gain);
        local->getParameters()[1]->setValue (0.0f);
        expect (fx.addToChain (2, std::move (local), 0));
        auto second = SandboxedPlugin::create (TestPluginFormat::describe (TestPluginFormat::Kind::gain), 48000.0, kBlock,
                                               options (2000.0), error);
        expect (second != nullptr, error);
        if (second != nullptr)
        {
            second->getParameters()[0]->setValue (0.5f);
            second->getParameters()[1]->setValue (0.0f);
            expect (fx.addToChain (2, std::move (second)));
        }
        expect (fx.isSandboxed (2));
        expectEquals (fx.getNumChainMembers (2), 3);
        expectEquals (fx.getLatency (2), 2 * kBlock);
        peaks = impulse (engine, 6);
        expectEquals ((int) peaks.size(), 1);
        if (! peaks.empty())
        {
            expectEquals (peaks[0].first, 2 * kBlock);
            expectWithinAbsoluteError (peaks[0].second, 2.0f * kCentre, 1.0e-4f);
        }
        expect (! fx.isFaulted (2));

        fx.setPlugin (2, nullptr);
        engine.getGarbage().collectAllNow();
    }

    void faultTest (TestPluginFormat::Kind kind)
    {
        events.clear();
        PluginHost host (nullptr);
        host.setSandboxOptions (*options (200.0));
        host.setSandboxEnabled (true);

        MixerEngine engine (1);
        OfflineRig rig (engine, 2, 2);
        test::setChannels (engine, 1);
        auto& strip = *engine.getChannels()[0];
        auto& fx = engine.getFx();
        rig.setInput (0, 0.25f);

        // The dry level, with no insert.
        rig.run (40);
        const float dry = rig.out (0);
        expect (dry > 0.01f);

        juce::String error;
        auto instance = host.createInstance (TestPluginFormat::describe (kind), 48000.0, kBlock, error);
        expect (instance != nullptr, error);
        if (instance == nullptr)
            return;
        auto* sandboxed = dynamic_cast<SandboxedPlugin*> (instance.get());
        expect (sandboxed != nullptr, "not sandboxed");
        if (sandboxed == nullptr)
            return;

        fx.setPlugin (0, std::move (instance));
        fx.clearFlags (0);
        expect (fx.isSandboxed (0));
        expectEquals (fx.getLatency (0), kBlock); // reported to the delay compensation
        strip.params.preInsert.on = true;
        strip.params.preInsert.fx = 0;

        // Processed: gain 0.25 -> x0.5.
        sandboxed->getParameters()[0]->setValue (0.25f);
        juce::MemoryBlock state;
        sandboxed->getStateInformation (state); // also cached for a restart
        rig.run (40);
        expect (! fx.isFaulted (0), "faulted while healthy");
        expectWithinAbsoluteError (rig.out (0), 0.5f * dry, 1.0e-4f);
        const int firstPid = sandboxed->getHelperProcessId();
        expect (firstPid > 0);

        // Automation makes it misbehave.
        sandboxed->getParameters()[2]->setValue (1.0f);
        for (int i = 0; i < 40 && ! fx.isFaulted (0); ++i)
            rig.run (1);
        expect (fx.isFaulted (0), "not faulted");
        expectEquals (engine.getNumFaultedFx(), 1);

        // The console keeps running: the strip passes its dry signal.
        rig.run (20);
        expect (std::isfinite (rig.out (0)));
        expectWithinAbsoluteError (rig.out (0), dry, 1.0e-4f);

        // The supervisor restarts the helper with the cached state.
        const auto start = juce::Time::getMillisecondCounter();
        while ((sandboxed->getStatus() != SandboxedPlugin::Status::running || sandboxed->getHelperProcessId() == firstPid)
               && juce::Time::getMillisecondCounter() - start < 10000)
        {
            juce::Thread::sleep (20);
            sandboxed->supervise();
        }
        expect (sandboxed->getStatus() == SandboxedPlugin::Status::running, "not restarted");
        expect (sandboxed->getHelperProcessId() != firstPid, "same helper");
        // A crash can first show as a helper that stopped responding (a crash
        // handler, such as a sanitizer's report, keeps it alive for a moment).
        const auto log = events.joinIntoString ("\n");
        expect (kind == TestPluginFormat::Kind::crash ? (log.contains ("crashed") || log.contains ("stopped responding"))
                                                      : log.contains ("stopped responding"),
                log);

        // Still bypassed until the fault is cleared; then processed again with the restored state.
        rig.run (5);
        expectWithinAbsoluteError (rig.out (0), dry, 1.0e-4f);
        fx.clearFlags (0);
        rig.run (40);
        expect (! fx.isFaulted (0), "faulted after the restart");
        expectWithinAbsoluteError (rig.out (0), 0.5f * dry, 1.0e-4f);

        fx.setPlugin (0, nullptr);
        engine.getGarbage().collectAllNow();
    }

    void chainFaultTest()
    {
        events.clear();
        PluginHost host (nullptr);
        host.setSandboxOptions (*options (200.0));
        host.setSandboxEnabled (true);

        MixerEngine engine (1);
        OfflineRig rig (engine, 2, 2);
        test::setChannels (engine, 1);
        auto& strip = *engine.getChannels()[0];
        auto& fx = engine.getFx();
        rig.setInput (0, 0.25f);
        rig.run (40);
        const float dry = rig.out (0);
        expect (dry > 0.01f);

        // In-process x0.5, the sandboxed crash plugin at x1, in-process x0.5.
        auto halve = []
        {
            auto p = TestPluginFormat::create (TestPluginFormat::Kind::gain);
            p->getParameters()[0]->setValue (0.25f);
            return p;
        };
        juce::String error;
        auto instance = host.createInstance (TestPluginFormat::describe (TestPluginFormat::Kind::crash), 48000.0, kBlock, error);
        expect (instance != nullptr, error);
        auto* sandboxed = dynamic_cast<SandboxedPlugin*> (instance.get());
        expect (sandboxed != nullptr, "not sandboxed");
        if (sandboxed == nullptr)
            return;
        sandboxed->getParameters()[0]->setValue (0.5f);
        juce::MemoryBlock state;
        sandboxed->getStateInformation (state); // also cached for a restart

        fx.setPlugin (0, halve());
        expect (fx.addToChain (0, std::move (instance)));
        expect (fx.addToChain (0, halve()));
        fx.clearFlags (0);
        strip.params.preInsert.on = true;
        strip.params.preInsert.fx = 0;
        rig.run (40);
        expect (! fx.isFaulted (0) && fx.getNumFaultedChainMembers (0) == 0, "faulted while healthy");
        expectWithinAbsoluteError (rig.out (0), 0.25f * dry, 1.0e-4f);
        engine.pollPlugins();
        expectEquals (fx.getLatency (0), kBlock);
        const int firstPid = sandboxed->getHelperProcessId();

        // It crashes: only it is faulted, the other two keep processing.
        juce::StringArray warnings;
        engine.onPluginWarning = [&warnings] (const juce::String& text) { warnings.add (text); };
        sandboxed->getParameters()[2]->setValue (1.0f);
        for (int i = 0; i < 40 && ! fx.isChainMemberFaulted (0, 1); ++i)
            rig.run (1);
        expect (fx.isChainMemberFaulted (0, 1), "not faulted");
        expect (! fx.isFaulted (0), "the slot faulted");
        expectEquals (engine.getNumFaultedFx(), 0);
        rig.run (20);
        expect (std::isfinite (rig.out (0)));
        expectWithinAbsoluteError (rig.out (0), 0.25f * dry, 1.0e-4f);
        engine.pollPlugins();
        expectEquals (warnings.size(), 1);
        expect (warnings[0].startsWith ("FX 1: Sidecar Test crash (plugin 2 of 3) ")
                    && warnings[0].endsWith ("; bypassed, the rest of the chain keeps running"),
                warnings[0]);
        expectEquals (fx.getLatency (0), 0); // counted as bypassed

        // Its helper is restarted (as for a plugin on its own); CLEAR runs it again.
        const auto start = juce::Time::getMillisecondCounter();
        while ((sandboxed->getStatus() != SandboxedPlugin::Status::running || sandboxed->getHelperProcessId() == firstPid)
               && juce::Time::getMillisecondCounter() - start < 10000)
        {
            juce::Thread::sleep (20);
            sandboxed->supervise();
        }
        expect (sandboxed->getStatus() == SandboxedPlugin::Status::running, "not restarted");
        expect (sandboxed->getHelperProcessId() != firstPid, "same helper");
        sandboxed->getParameters()[2]->setValue (0.0f);
        rig.run (5);
        expect (fx.isChainMemberFaulted (0, 1), "un-faulted before CLEAR");
        fx.clearChainMemberFault (0, 1);
        rig.run (40);
        expect (! fx.isChainMemberFaulted (0, 1), "faulted after the restart");
        expectWithinAbsoluteError (rig.out (0), 0.25f * dry, 1.0e-4f);
        engine.pollPlugins();
        expectEquals (fx.getLatency (0), kBlock);

        engine.onPluginWarning = nullptr;
        fx.setPlugin (0, nullptr);
        engine.getGarbage().collectAllNow();
    }
};

static SandboxTests sandboxTests;
