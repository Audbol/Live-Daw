#include "TestHelpers.h"
#include "engine/PluginHost.h"
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

        beginTest ("A plugin that crashes is faulted; the console keeps running and the plugin comes back");
        faultTest (TestPluginFormat::Kind::crash);

        beginTest ("A plugin that hangs is faulted, its helper killed and restarted");
        faultTest (TestPluginFormat::Kind::hang);
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

    void faultTest (TestPluginFormat::Kind kind)
    {
        events.clear();
        PluginHost host (nullptr);
        host.setSandboxOptions (*options (200.0));
        host.setSandboxEnabled (true);

        MixerEngine engine (1);
        OfflineRig rig (engine, 2, 2);
        engine.setNumChannels (1);
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
        expectEquals (fx.getLatencySamples (0), kBlock);
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
        expect (events.joinIntoString ("\n").contains (kind == TestPluginFormat::Kind::crash ? "crashed" : "stopped responding"),
                events.joinIntoString ("\n"));

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
};

static SandboxTests sandboxTests;
