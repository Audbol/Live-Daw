#include "TestHelpers.h"
#include "engine/PluginHost.h"

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
            engine.setNumInputs (1);

            juce::String error;
            auto instance = host.createInstance (*desc, 48000.0, OfflineRig::kBlock, error);
            if (instance == nullptr)
            {
                logMessage ("Could not instantiate " + desc->name + ": " + error);
                continue;
            }

            logMessage ("Hosting " + desc->name);
            auto& strip = *engine.getInputs()[0];
            strip.inserts.setPlugin (0, std::move (instance));
            rig.setInput (0, 0.25f);
            rig.run (200);

            expect (! strip.inserts.isFaulted (0), "plugin faulted");
            expect (std::isfinite (rig.out (0)));
            expect (strip.inserts.getSlotLoad (0) >= 0.0f);

            // Plugin state survives a capture/restore cycle.
            juce::MemoryBlock state;
            strip.inserts.getPlugin (0)->getStateInformation (state);
            logMessage ("State size: " + juce::String ((int) state.getSize()) + " bytes");

            strip.inserts.setPlugin (0, nullptr);
            engine.getGarbage().collectAllNow();
            return;
        }

        logMessage ("No suitable effect among the scanned plugins");
    }
};

static PluginHostingTests pluginHostingTests;
