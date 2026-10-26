#include "TestHelpers.h"
#include "engine/PluginChain.h"
#include "engine/Show.h"
#include "sandbox/TestPlugins.h"
#include "ui/LookAndFeel.h"
#include "ui/PluginChainView.h"

using namespace livedaw;
using livedaw::sandbox::TestPluginFormat;
using livedaw::test::FakePlugin;
using livedaw::test::OfflineRig;
using livedaw::test::kCentre;

namespace
{
    /** y = x * mul + add (so the order of two matters), with a settable latency;
        counts its prepareToPlay calls. Its state is mul and add. */
    class AffinePlugin final : public juce::AudioPluginInstance
    {
    public:
        AffinePlugin (juce::String n, float m, float a)
            : juce::AudioPluginInstance (BusesProperties()
                                             .withInput ("In", juce::AudioChannelSet::stereo())
                                             .withOutput ("Out", juce::AudioChannelSet::stereo())),
              mul (m), add (a), name (std::move (n))
        {
        }

        std::atomic<float> mul, add;
        int prepareCalls = 0;

        void fillInPluginDescription (juce::PluginDescription& d) const override
        {
            d.name = name;
            d.pluginFormatName = "Fake";
            d.fileOrIdentifier = "affine-" + name;
            d.uniqueId = d.deprecatedUid = d.fileOrIdentifier.hashCode();
            d.numInputChannels = d.numOutputChannels = 2;
        }

        const juce::String getName() const override { return name; }
        void prepareToPlay (double, int) override { ++prepareCalls; }
        void releaseResources() override {}

        using juce::AudioPluginInstance::processBlock;

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                    buffer.setSample (ch, i, buffer.getSample (ch, i) * mul.load() + add.load());
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

        void getStateInformation (juce::MemoryBlock& data) override
        {
            const float v[] = { mul.load(), add.load() };
            data.replaceAll (v, sizeof (v));
        }

        void setStateInformation (const void* data, int size) override
        {
            if (size == 2 * (int) sizeof (float))
            {
                mul = static_cast<const float*> (data)[0];
                add = static_cast<const float*> (data)[1];
            }
        }

    private:
        juce::String name;
    };

    float processConstant (juce::AudioProcessor& p, float in)
    {
        juce::AudioBuffer<float> buffer (2, OfflineRig::kBlock);
        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), in, buffer.getNumSamples());
        juce::MidiBuffer midi;
        p.processBlock (buffer, midi);
        return buffer.getSample (0, buffer.getNumSamples() - 1);
    }

    float param (juce::AudioProcessor& p, int index) { return p.getParameters()[index]->getValue(); }
} // namespace

/** FX slot plugin chains (PluginChain, FxSlots::addToChain...): processing order,
    latency and bypass, swaps that keep the running plugins, faults and shows. */
class PluginChainTests final : public juce::UnitTest
{
public:
    PluginChainTests() : juce::UnitTest ("Plugin chains", "LiveDAW") {}

    void runTest() override
    {
        beginTest ("A chain runs its plugins in order");
        {
            std::vector<PluginChain::MemberPtr> members;
            members.push_back (PluginChain::makeMember (std::make_shared<AffinePlugin> ("Double", 2.0f, 0.0f)));
            members.push_back (PluginChain::makeMember (std::make_shared<AffinePlugin> ("Offset", 1.0f, 0.5f)));
            PluginChain chain (members);
            chain.prepareToPlay (48000.0, OfflineRig::kBlock);
            expectWithinAbsoluteError (processConstant (chain, 0.25f), 1.0f, 1.0e-6f); // 0.25 * 2 + 0.5
            expectEquals (chain.getName(), juce::String ("Double + Offset"));

            PluginChain reversed ({ members[1], members[0] });
            reversed.prepareToPlay (48000.0, OfflineRig::kBlock);
            expectWithinAbsoluteError (processConstant (reversed, 0.25f), 1.5f, 1.0e-6f); // (0.25 + 0.5) * 2

            chain.setBypassed (0, true);
            expectWithinAbsoluteError (processConstant (chain, 0.25f), 0.75f, 1.0e-6f);
        }

        beginTest ("An FX slot runs its chain in order; edits keep the running plugins");
        {
            MixerEngine engine (0);
            OfflineRig rig (engine, 1, 2);
            engine.outputProtection = false; // levels above 0 dBFS
            test::setChannels (engine, 1);
            auto& strip = *engine.getChannels()[0];
            strip.params.preInsert.on = true;
            strip.params.preInsert.fx = 4;
            auto& fx = engine.getFx();
            rig.setInput (0, 0.25f);

            auto* a = new AffinePlugin ("Double", 2.0f, 0.0f);
            fx.setPlugin (4, std::unique_ptr<juce::AudioPluginInstance> (a));
            expect (fx.getChain (4) == nullptr); // a plugin on its own
            expectEquals (fx.getNumChainMembers (4), 1);
            rig.run (4);
            expectWithinAbsoluteError (rig.out (0), 0.5f * kCentre, 1.0e-5f);

            auto* b = new AffinePlugin ("Offset", 1.0f, 0.5f);
            expect (fx.addToChain (4, std::unique_ptr<juce::AudioPluginInstance> (b)));
            expect (fx.getChain (4) != nullptr);
            expect (fx.getChainMember (4, 0) == a && fx.getChainMember (4, 1) == b);
            expectEquals (fx.describe (4), juce::String ("FX 5: Double + Offset"));
            rig.run (4);
            expectWithinAbsoluteError (rig.out (0), 1.0f * kCentre, 1.0e-5f);

            expect (fx.moveInChain (4, 1, 0));
            expect (fx.getChainMember (4, 0) == b && fx.getChainMember (4, 1) == a);
            rig.run (4);
            expectWithinAbsoluteError (rig.out (0), 1.5f * kCentre, 1.0e-5f);
            engine.getGarbage().collectAllNow(); // the replaced chains go; their plugins stay
            expectEquals (a->prepareCalls, 1);    // never prepared again while running
            expectEquals (b->prepareCalls, 1);

            auto* c = new AffinePlugin ("Third", 1.0f, 0.0f);
            expect (fx.addToChain (4, std::unique_ptr<juce::AudioPluginInstance> (c), 1));
            expectEquals (fx.getPlugin (4)->getName(), juce::String ("Offset +2"));
            expect (fx.removeFromChain (4, 0));
            engine.getGarbage().collectAllNow();
            expectEquals (fx.getNumChainMembers (4), 2);
            expect (fx.getChainMember (4, 0) == c && fx.getChainMember (4, 1) == a);
            rig.run (4);
            expectWithinAbsoluteError (rig.out (0), 0.5f * kCentre, 1.0e-5f);

            // Full at eight.
            for (int i = fx.getNumChainMembers (4); i < PluginChain::kMaxMembers; ++i)
                expect (fx.addToChain (4, std::make_unique<AffinePlugin> ("Unity", 1.0f, 0.0f)));
            expect (! fx.addToChain (4, std::make_unique<AffinePlugin> ("Ninth", 1.0f, 0.0f)));
            expect (! fx.addToChain (5, std::make_unique<AffinePlugin> ("Nothing to add to", 1.0f, 0.0f)));

            while (fx.getNumChainMembers (4) > 0)
                fx.removeFromChain (4, 0);
            expect (! fx.hasPlugin (4)); // the last one empties the slot
            engine.getGarbage().collectAllNow();
        }

        beginTest ("The latency is the sum of the members that aren't bypassed");
        {
            MixerEngine engine (0);
            OfflineRig rig (engine, 1, 2);
            auto& fx = engine.getFx();
            auto* a = new AffinePlugin ("A", 1.0f, 0.0f);
            auto* b = new AffinePlugin ("B", 1.0f, 0.0f);
            auto* c = new AffinePlugin ("C", 1.0f, 0.0f);
            a->setLatencySamples (10);
            b->setLatencySamples (20);
            c->setLatencySamples (30);
            fx.setPlugin (1, std::unique_ptr<juce::AudioPluginInstance> (a));
            expectEquals (fx.getLatency (1), 10);
            fx.addToChain (1, std::unique_ptr<juce::AudioPluginInstance> (b));
            fx.addToChain (1, std::unique_ptr<juce::AudioPluginInstance> (c));
            expectEquals (fx.getLatency (1), 60);

            fx.setChainMemberBypassed (1, 1, true);
            expect (fx.isChainMemberBypassed (1, 1));
            expectEquals (fx.getLatency (1), 40);
            a->setLatencySamples (25); // a member's change is followed at once
            expectEquals (fx.getLatency (1), 55);
            b->setLatencySamples (5); // bypassed: no effect
            expectEquals (fx.getLatency (1), 55);
            fx.setChainMemberBypassed (1, 1, false);
            expectEquals (fx.getLatency (1), 60);

            // Bypassing a plugin on its own makes it a chain of one.
            fx.setPlugin (2, std::make_unique<AffinePlugin> ("Solo", 1.0f, 0.0f));
            fx.getPlugin (2)->setLatencySamples (64);
            expectEquals (fx.getLatency (2), 64);
            fx.setChainMemberBypassed (2, 0, true);
            expect (fx.getChain (2) != nullptr);
            expectEquals (fx.getLatency (2), 0);
            fx.setPlugin (1, nullptr);
            fx.setPlugin (2, nullptr);
            engine.getGarbage().collectAllNow();
        }

        beginTest ("A plugin that fails in a chain bypasses only itself; the rest keeps running");
        {
            for (auto mode : { FakePlugin::Mode::Throw, FakePlugin::Mode::EmitNaN })
            {
                MixerEngine engine (0);
                OfflineRig rig (engine, 1, 2);
                engine.outputProtection = false;
                test::setChannels (engine, 1);
                auto& strip = *engine.getChannels()[0];
                strip.params.preInsert.on = true;
                strip.params.preInsert.fx = 0;
                auto& fx = engine.getFx();
                juce::StringArray warnings;
                engine.onPluginWarning = [&warnings] (const juce::String& text) { warnings.add (text); };

                fx.setPlugin (0, std::make_unique<FakePlugin> (0.5f));
                auto* bad = new FakePlugin (1.0f, FakePlugin::Mode::Gain, "bad");
                bad->setLatencySamples (100);
                fx.addToChain (0, std::unique_ptr<juce::AudioPluginInstance> (bad));
                fx.addToChain (0, std::make_unique<FakePlugin> (2.0f));
                rig.setInput (0, 0.25f);
                rig.run();
                expectEquals (fx.getLatency (0), 100);

                bad->mode = mode;
                rig.run (4);
                expect (! fx.isFaulted (0), "the slot faulted");
                expect (fx.isChainMemberFaulted (0, 1));
                expect (! fx.isChainMemberFaulted (0, 0) && ! fx.isChainMemberFaulted (0, 2));
                expectEquals (fx.getNumFaultedChainMembers (0), 1);
                expectWithinAbsoluteError (rig.out (0), 0.25f * 0.5f * 2.0f * kCentre, 1.0e-5f); // through the other two

                // Named once in the log; a faulted plugin counts as bypassed for the latency.
                engine.pollPlugins();
                engine.pollPlugins();
                expectEquals (warnings.size(), 1);
                const auto what = mode == FakePlugin::Mode::Throw ? juce::String ("threw an exception") : juce::String ("output NaN/Inf");
                expectEquals (warnings[0], "FX 1: Fake bad (plugin 2 of 3) " + what + "; bypassed, the rest of the chain keeps running");
                expectEquals (fx.getLatency (0), 0);
                expect (! fx.getChain (0)->createXml()->toString().containsIgnoreCase ("fault")); // not saved

                // CLEAR tries it again: still failing, it faults again (and is reported again).
                fx.clearChainMemberFault (0, 1);
                expect (! fx.isChainMemberFaulted (0, 1));
                rig.run (2);
                expect (fx.isChainMemberFaulted (0, 1));
                engine.pollPlugins();
                expectEquals (warnings.size(), 2);

                // Fixed: CLEAR brings it back.
                bad->mode = FakePlugin::Mode::Gain;
                bad->gain = 3.0f;
                fx.clearChainMemberFault (0, 1);
                rig.run (2);
                expect (! fx.isChainMemberFaulted (0, 1));
                expectWithinAbsoluteError (rig.out (0), 0.25f * 0.5f * 3.0f * 2.0f * kCentre, 1.0e-5f);
                engine.pollPlugins();
                expectEquals (fx.getLatency (0), 100);

                // BYP on or off clears a fault too, and the slot's CLEAR clears its plugins'.
                bad->mode = mode;
                rig.run (2);
                expect (fx.isChainMemberFaulted (0, 1));
                fx.setChainMemberBypassed (0, 1, true);
                expect (! fx.isChainMemberFaulted (0, 1) && fx.isChainMemberBypassed (0, 1));
                fx.setChainMemberBypassed (0, 1, false);
                rig.run (2);
                expect (fx.isChainMemberFaulted (0, 1));
                fx.clearFlags (0);
                expect (! fx.isChainMemberFaulted (0, 1));

                engine.onPluginWarning = nullptr;
                fx.setPlugin (0, nullptr);
                engine.getGarbage().collectAllNow();
            }
        }

        beginTest ("A chain of one that fails auto-bypasses the slot, as a plugin on its own");
        {
            MixerEngine engine (0);
            OfflineRig rig (engine, 1, 2);
            test::setChannels (engine, 1);
            auto& strip = *engine.getChannels()[0];
            strip.params.preInsert.on = true;
            strip.params.preInsert.fx = 0;
            auto& fx = engine.getFx();
            auto* bad = new FakePlugin (1.0f, FakePlugin::Mode::Gain, "bad");
            fx.setPlugin (0, std::unique_ptr<juce::AudioPluginInstance> (bad));
            fx.setChainMemberBypassed (0, 0, true); // a chain of one
            fx.setChainMemberBypassed (0, 0, false);
            expect (fx.getChain (0) != nullptr);
            bad->mode = FakePlugin::Mode::EmitNaN;
            rig.setInput (0, 0.5f);
            rig.run();

            expect (fx.isFaulted (0));
            expect (! fx.isChainMemberFaulted (0, 0));
            expectWithinAbsoluteError (rig.out (0), 0.5f * kCentre, 1.0e-5f); // dry signal passes
            const auto fault = fx.getChain (0)->takeFault();
            expectEquals (fault.member, 0);
            expect (fault.kind == PluginChain::FaultKind::nonFinite);
            expectEquals (fx.getChain (0)->takeFault().member, -1); // reported once
            fx.setPlugin (0, nullptr);
            engine.getGarbage().collectAllNow();
        }

        beginTest ("Editing a faulted slot's chain (zoom, collapse, add, move) keeps it bypassed until cleared");
        {
            MixerEngine engine (0);
            OfflineRig rig (engine, 1, 2);
            test::setChannels (engine, 1);
            auto& strip = *engine.getChannels()[0];
            strip.params.preInsert.on = true;
            strip.params.preInsert.fx = 0;
            auto& fx = engine.getFx();
            auto* bad = new FakePlugin (1.0f, FakePlugin::Mode::Throw, "bad");
            fx.setPlugin (0, std::unique_ptr<juce::AudioPluginInstance> (bad)); // a plugin on its own
            rig.setInput (0, 0.5f);
            rig.run();
            expect (fx.isFaulted (0));

            // Collapsing its card in the plugin window makes it a chain of one.
            fx.setChainMemberView (0, 0, { 100, true });
            expect (fx.getChain (0) != nullptr);
            expect (fx.isFaulted (0), "a view change cleared the slot's fault");
            fx.setChainMemberView (0, 0, { 67, true });
            expect (fx.isFaulted (0));

            expect (fx.addToChain (0, std::make_unique<FakePlugin> (2.0f)));
            expect (fx.isFaulted (0), "adding a plugin cleared the slot's fault");
            expect (fx.moveInChain (0, 1, 0));
            expect (fx.isFaulted (0), "moving a plugin cleared the slot's fault");
            bad->mode = FakePlugin::Mode::Gain;
            rig.run (2);
            expectWithinAbsoluteError (rig.out (0), 0.5f * kCentre, 1.0e-5f); // still bypassed: dry

            // Only clearing the fault runs the slot again.
            fx.clearFlags (0);
            rig.run (2);
            expect (! fx.isFaulted (0));
            expectWithinAbsoluteError (rig.out (0), 0.5f * 2.0f * kCentre, 1.0e-5f);

            // A new plugin (not a rearrangement) starts unfaulted, as before.
            fx.setPlugin (0, std::make_unique<FakePlugin> (1.0f, FakePlugin::Mode::Throw, "bad"));
            rig.run();
            expect (fx.isFaulted (0));
            fx.setPlugin (0, std::make_unique<FakePlugin> (1.0f));
            expect (! fx.isFaulted (0));
            fx.setPlugin (0, nullptr);
            engine.getGarbage().collectAllNow();
        }

        beginTest ("Zoom steps and FIT");
        {
            using V = ui::PluginChainView;
            expectEquals (V::nextZoom (100, -1), 85);
            expectEquals (V::nextZoom (85, -1), 75);
            expectEquals (V::nextZoom (67, -1), 50);
            expectEquals (V::nextZoom (50, -1), 50);
            expectEquals (V::nextZoom (67, 1), 75);
            expectEquals (V::nextZoom (100, 1), 100); // no growing past 100 %
            expectEquals (PluginChain::snapZoom (70), 67);
            expectEquals (PluginChain::snapZoom (130), 100);
            expectEquals (V::fitZoom (400, 500), 100);
            expectEquals (V::fitZoom (400, 340), 85);
            expectEquals (V::fitZoom (400, 300), 75);
            expectEquals (V::fitZoom (400, 299), 67);
            expectEquals (V::fitZoom (400, 100), 50); // the smallest step if none fits
        }

        beginTest ("A 3-plugin chain survives a show save and load, and scenes recall it");
        {
            PluginHost host (nullptr);
            host.getFormatManager().addFormat (std::make_unique<TestPluginFormat>());
            MixerEngine engine (0);
            OfflineRig rig (engine, 1, 2);
            Show show (engine, host);
            auto& fx = engine.getFx();

            const float gains[] = { 0.2f, 0.4f, 0.6f };
            auto withGain = [] (float g)
            {
                auto p = TestPluginFormat::create (TestPluginFormat::Kind::gain);
                p->getParameters()[0]->setValue (g);
                return p;
            };
            fx.setPlugin (6, withGain (gains[0]));
            fx.addToChain (6, withGain (gains[1]));
            fx.addToChain (6, withGain (gains[2]));
            fx.setChainMemberBypassed (6, 1, true);
            fx.setChainMemberView (6, 0, { 67, false });
            fx.setChainMemberView (6, 2, { 100, true });
            const auto xml = show.toXml();
            expect (xml->toString().contains (PluginChain::kXmlTag));

            show.addScene ("Chain");
            fx.getChainMember (6, 2)->getParameters()[0]->setValue (0.9f);
            fx.setChainMemberBypassed (6, 1, false);
            expectEquals ((int) show.recallScene (0).warnings.size(), 0);
            expectWithinAbsoluteError (param (*fx.getChainMember (6, 2), 0), gains[2], 1.0e-6f);
            expect (fx.isChainMemberBypassed (6, 1));

            MixerEngine engine2 (0);
            OfflineRig rig2 (engine2, 1, 2);
            Show loaded (engine2, host);
            const auto result = loaded.loadFromXml (*xml);
            expect (result.wasOk(), result.getErrorMessage());
            auto& fx2 = engine2.getFx();
            expect (fx2.getChain (6) != nullptr);
            expectEquals (fx2.getNumChainMembers (6), 3);
            for (int i = 0; i < fx2.getNumChainMembers (6); ++i)
            {
                expectEquals (fx2.getChainMember (6, i)->getName(), juce::String ("Sidecar Test gain"));
                expectWithinAbsoluteError (param (*fx2.getChainMember (6, i), 0), gains[i], 1.0e-6f);
                expect (fx2.isChainMemberBypassed (6, i) == (i == 1));
            }
            expectEquals (fx2.getChainMemberView (6, 0).zoom, 67);
            expect (! fx2.getChainMemberView (6, 0).collapsed);
            expect (fx2.getChainMemberView (6, 1).isDefault());
            expect (fx2.getChainMemberView (6, 2).collapsed && fx2.getChainMemberView (6, 2).zoom == 100);

            // Chains saved before zoom and collapse read with the defaults.
            auto saved = fx.getChain (6)->createXml();
            expect (saved->getChildElement (0)->getIntAttribute ("zoom") == 67 && saved->getChildElement (2)->getBoolAttribute ("collapsed"));
            for (auto* member : saved->getChildIterator())
            {
                member->removeAttribute ("zoom");
                member->removeAttribute ("collapsed");
            }
            std::vector<PluginChain::Saved> members;
            expect (PluginChain::parseXml (*saved, members));
            expectEquals ((int) members.size(), 3);
            for (auto& m : members)
                expect (m.view.isDefault());
            expectEquals (fx2.getPlugin (6)->getName(), juce::String ("Sidecar Test gain +2"));

            // A different chain isn't recalled into.
            fx2.moveInChain (6, 0, 2);
            loaded.addScene ("Moved");
            fx2.removeFromChain (6, 0);
            expectEquals ((int) loaded.recallScene (0).warnings.size(), 1);

            // Safe mode: no plugins.
            MixerEngine engine3 (0);
            Show safe (engine3, host);
            expect (safe.loadFromXml (*xml, false).wasOk());
            expect (! engine3.getFx().hasPlugin (6));

            fx.setPlugin (6, nullptr);
            fx2.setPlugin (6, nullptr);
            engine.getGarbage().collectAllNow();
            engine2.getGarbage().collectAllNow();
        }

        beginTest ("A plugin on its own (and a chain of one) is saved as before; such shows load unchanged");
        {
            PluginHost host (nullptr);
            host.getFormatManager().addFormat (std::make_unique<TestPluginFormat>());
            MixerEngine engine (0);
            OfflineRig rig (engine, 1, 2);
            Show show (engine, host);
            auto& fx = engine.getFx();

            auto plugin = TestPluginFormat::create (TestPluginFormat::Kind::gain);
            plugin->getParameters()[0]->setValue (0.3f);
            fx.setPlugin (3, std::move (plugin));
            const auto single = show.toXml();
            const auto text = single->toString();
            expect (text.contains ("<PLUGIN") && ! text.contains (PluginChain::kXmlTag));

            // Grown to two and back to one: still the single-plugin format.
            fx.addToChain (3, TestPluginFormat::create (TestPluginFormat::Kind::gain));
            fx.removeFromChain (3, 1);
            expect (fx.getChain (3) != nullptr);
            expect (! show.toXmlString().contains (PluginChain::kXmlTag));
            // ... unless its window view isn't the default.
            fx.setChainMemberView (3, 0, { 75, false });
            expect (show.toXmlString().contains (PluginChain::kXmlTag));
            fx.setChainMemberView (3, 0, {});
            expect (! show.toXmlString().contains (PluginChain::kXmlTag));

            MixerEngine engine2 (0);
            OfflineRig rig2 (engine2, 1, 2);
            Show loaded (engine2, host);
            expect (loaded.loadFromXml (*single).wasOk());
            auto& fx2 = engine2.getFx();
            expect (fx2.hasPlugin (3));
            expect (fx2.getChain (3) == nullptr); // a plugin on its own, as before
            expectWithinAbsoluteError (param (*fx2.getPlugin (3), 0), 0.3f, 1.0e-6f);

            fx.setPlugin (3, nullptr);
            fx2.setPlugin (3, nullptr);
            engine.getGarbage().collectAllNow();
            engine2.getGarbage().collectAllNow();
        }

        beginTest ("Scenes and snippets recall the plugin window's zoom and collapse; older scenes keep the current view");
        {
            PluginHost host (nullptr);
            host.getFormatManager().addFormat (std::make_unique<TestPluginFormat>());
            MixerEngine engine (0);
            OfflineRig rig (engine, 1, 2);
            Show show (engine, host);
            auto& fx = engine.getFx();

            // FX 5 a plugin on its own (the single-plugin format while its view is the default), FX 6 a chain of two.
            fx.setPlugin (4, TestPluginFormat::create (TestPluginFormat::Kind::gain));
            fx.setPlugin (5, TestPluginFormat::create (TestPluginFormat::Kind::gain));
            fx.addToChain (5, TestPluginFormat::create (TestPluginFormat::Kind::gain));
            fx.setChainMemberView (5, 0, { 67, false });
            fx.setChainMemberView (5, 1, { 100, true });
            show.addScene ("Zoomed");
            expect (show.toXmlString().contains (PluginChain::kDefaultViewAttribute));
            fx.setChainMemberView (5, 0, {});
            fx.setChainMemberView (5, 1, { 50, false });
            fx.setChainMemberView (4, 0, { 75, true });
            show.addScene ("Default and 50");

            expectEquals ((int) show.recallScene (0).warnings.size(), 0);
            expectEquals (fx.getChainMemberView (5, 0).zoom, 67);
            expect (! fx.getChainMemberView (5, 0).collapsed);
            expect (fx.getChainMemberView (5, 1).collapsed && fx.getChainMemberView (5, 1).zoom == 100);
            expect (fx.getChainMemberView (4, 0).isDefault(), "the plugin on its own kept its view");
            expectEquals ((int) show.recallScene (1).warnings.size(), 0);
            expect (fx.getChainMemberView (5, 0).isDefault(), "a default view stored in the scene isn't recalled");
            expectEquals (fx.getChainMemberView (5, 1).zoom, 50);
            expect (! fx.getChainMemberView (5, 1).collapsed);
            expect (fx.getChainMemberView (4, 0).collapsed && fx.getChainMemberView (4, 0).zoom == 75);

            // A snippet that recalls the slot's plugin state recalls its view as well.
            fx.setChainMemberView (5, 0, { 85, false });
            auto scope = state::Scope::none();
            scope.sections = state::Inserts;
            scope.fx = 1u << 5;
            const int snippet = show.addSnippet (0, "FX 6", scope);
            expect (snippet >= 0);
            fx.setChainMemberView (5, 0, { 50, true });
            expectEquals ((int) show.recallSnippet (0, snippet).warnings.size(), 0);
            expectEquals (fx.getChainMemberView (5, 0).zoom, 85);
            expect (! fx.getChainMemberView (5, 0).collapsed);

            // Scenes from before views were stored: the current view stays, except
            // where the chain stored a view other than the default (as it always did).
            auto old = show.toXml();
            std::function<void (juce::XmlElement&)> forget = [&forget] (juce::XmlElement& e)
            {
                e.removeAttribute ("views");
                e.removeAttribute (PluginChain::kDefaultViewAttribute);
                for (auto* child : e.getChildIterator())
                    forget (*child);
            };
            forget (*old);
            expect (! old->toString().contains (PluginChain::kDefaultViewAttribute) && ! old->toString().contains ("views="));

            MixerEngine engine2 (0);
            OfflineRig rig2 (engine2, 1, 2);
            Show loaded (engine2, host);
            expect (loaded.loadFromXml (*old).wasOk());
            auto& fx2 = engine2.getFx();
            expectEquals (fx2.getNumChainMembers (5), 2);
            fx2.setChainMemberView (5, 0, { 75, true });
            fx2.setChainMemberView (4, 0, { 67, false });
            expectEquals ((int) loaded.recallScene (0).warnings.size(), 0);
            expectEquals (fx2.getChainMemberView (5, 0).zoom, 67);
            expectEquals (fx2.getChainMemberView (4, 0).zoom, 67); // the plugin on its own: no view stored
            expectEquals ((int) loaded.recallScene (1).warnings.size(), 0);
            expectEquals (fx2.getChainMemberView (5, 1).zoom, 50);
            fx2.setChainMemberView (5, 0, { 75, true });
            expectEquals ((int) loaded.recallScene (1).warnings.size(), 0);
            expect (fx2.getChainMemberView (5, 0).zoom == 75 && fx2.getChainMemberView (5, 0).collapsed); // stored at the default: unknown then

            for (auto* f : { &fx, &fx2 })
                for (int slot : { 4, 5 })
                    f->setPlugin (slot, nullptr);
            engine.getGarbage().collectAllNow();
            engine2.getGarbage().collectAllNow();
        }

        beginTest ("The plugin window: chain bar, cards, zoom, collapse, faults, ROW and FOCUS");
        {
            using V = ui::PluginChainView;
            const auto snapshots = juce::SystemStats::getEnvironmentVariable ("SIDECAR_CHAIN_SNAPSHOT_DIR", {});
            std::unique_ptr<ui::ConsoleLookAndFeel> look;
            if (snapshots.isNotEmpty())
            {
                look = std::make_unique<ui::ConsoleLookAndFeel>();
                juce::LookAndFeel::setDefaultLookAndFeel (look.get());
            }

            MixerEngine engine (0);
            OfflineRig rig (engine, 1, 2);
            engine.outputProtection = false;
            test::setChannels (engine, 1);
            auto& strip = *engine.getChannels()[0];
            strip.params.preInsert.on = true;
            strip.params.preInsert.fx = 0;
            auto& fx = engine.getFx();
            fx.setPlugin (0, TestPluginFormat::create (TestPluginFormat::Kind::gain));
            int adds = 0;
            auto layoutSaved = V::Layout::row;
            {
                V view (fx, 0, { [&adds] (juce::Component&) { ++adds; }, {}, {}, [&layoutSaved] (V::Layout l) { layoutSaved = l; } });
                view.setMaxSize ({ 4000, 2000 }); // not the screen's: CI displays may be small
                expectEquals (view.getNumCards(), 1);
                expect (view.shows (fx.getPlugin (0)));
                const int single = view.getWidth();
                expect (single >= V::kMinCardWidth);
                expect (view.getZoomMethod (0) == V::ZoomMethod::transform); // generic controls

                fx.addToChain (0, TestPluginFormat::create (TestPluginFormat::Kind::crash));
                auto* nan = new FakePlugin (1.0f, FakePlugin::Mode::Gain, "nan"); // no editor: generic controls
                fx.addToChain (0, std::unique_ptr<juce::AudioPluginInstance> (nan));
                view.refresh();
                expectEquals (view.getNumCards(), 3);
                expect (view.shows (fx.getPlugin (0)) && view.shows (fx.getChainMember (0, 2)));
                expect (view.getWidth() > single); // grows to fit
                const int full0 = view.getEditorBounds (0).getHeight(), full2 = view.getEditorBounds (2).getHeight();

                // Zoom: a generic editor is scaled with a transform.
                const auto full = view.getEditorBounds (0);
                view.setZoom (0, 67);
                expectEquals (fx.getChainMemberView (0, 0).zoom, 67);
                const auto small = view.getEditorBounds (0);
                expectWithinAbsoluteError ((float) small.getWidth(), (float) full.getWidth() * 0.67f, 1.5f);
                expectWithinAbsoluteError ((float) small.getHeight(), (float) full.getHeight() * 0.67f, 1.5f);
                expectEquals (small.getY(), V::kTitleHeight);
                view.stepZoom (0, 1);
                expectEquals (fx.getChainMemberView (0, 0).zoom, 75);
                view.stepZoom (0, -1);
                expectEquals (fx.getChainMemberView (0, 0).zoom, 67);

                // Collapse: a thin column in ROW, the others move up close.
                const int before = view.getCardBounds (2).getX();
                view.setCollapsed (1, true);
                expect (fx.getChainMemberView (0, 1).collapsed);
                expectEquals (view.getCardBounds (1).getWidth(), V::kCollapsedWidth);
                expect (view.getEditorBounds (1).isEmpty());
                expect (view.getCardBounds (2).getX() < before);

                // A faulted plugin (the rest of the chain runs).
                rig.setInput (0, 0.25f);
                nan->mode = FakePlugin::Mode::EmitNaN;
                rig.run (2);
                expect (fx.isChainMemberFaulted (0, 2) && ! fx.isFaulted (0));
                view.refresh();

                auto snapshot = [&] (const juce::String& fileName)
                {
                    if (snapshots.isEmpty())
                        return;
                    juce::File file (juce::File (snapshots).getChildFile (fileName));
                    file.deleteFile();
                    juce::FileOutputStream out (file);
                    juce::PNGImageFormat().writeImageToStream (view.createComponentSnapshot (view.getLocalBounds(), true, 1.0f), out);
                    logMessage ("Saved " + file.getFullPathName());
                };
                snapshot ("chain_row.png");

                // FOCUS: one card at a time, the chips as tabs.
                view.setLayout (V::Layout::focus);
                expect (layoutSaved == V::Layout::focus);
                view.select (2);
                expectEquals (view.getSelected(), 2);
                expect (view.isCardVisible (2) && ! view.isCardVisible (0) && ! view.isCardVisible (1));
                snapshot ("chain_focus.png");
                view.select (0);
                expect (view.isCardVisible (0) && ! view.isCardVisible (2));
                view.setLayout (V::Layout::row);
                expect (view.isCardVisible (0) && view.isCardVisible (2));

                // FIT: the largest steps that fit the window's height (collapsed ones stay).
                view.setSize (view.getWidth(), V::kBarHeight + V::kTitleHeight + 100);
                view.fitToHeight();
                expectEquals (fx.getChainMemberView (0, 0).zoom, V::fitZoom (full0, 100));
                expectEquals (fx.getChainMemberView (0, 2).zoom, V::fitZoom (full2, 100));
                expect (fx.getChainMemberView (0, 1).collapsed && fx.getChainMemberView (0, 1).zoom == 100);

                // Beyond the size limit it scrolls.
                view.setMaxSize ({ 4000, 2000 });
                const auto natural = view.getBounds();
                view.setMaxSize ({ 500, 2000 });
                expectEquals (view.getWidth(), 500);
                view.setMaxSize ({ 4000, 2000 });
                expect (view.getBounds() == natural);

                // A removed plugin's editor goes before the plugin.
                view.dropEditorFor (fx.getChainMember (0, 2));
                fx.removeFromChain (0, 2);
                expectEquals (view.getNumCards(), 2);
                view.refresh();
                expectEquals (view.getNumCards(), 2);
                fx.setPlugin (0, nullptr);
                view.refresh();
                expectEquals (view.getNumCards(), 0);
            }
            engine.getGarbage().collectAllNow();
            if (look != nullptr)
                juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        }
    }
};

static PluginChainTests pluginChainTests;

//==============================================================================
namespace
{
    /** A plugin whose editor behaves like a real plugin's native one: it starts
        tiny and takes its real size later (a VST3 view on Linux, once attached),
        follows the host scale factor, and (`snapsBack`) goes back to its size
        after the host resizes it smaller (an LV2 UI with a minimum size). */
    class NativeEditorPlugin final : public juce::AudioPluginInstance
    {
    public:
        static constexpr int kWidth = 400, kHeight = 300;

        class Editor final : public juce::AudioProcessorEditor
        {
        public:
            Editor (NativeEditorPlugin& p, bool resizable, bool snaps) : juce::AudioProcessorEditor (p), snapsBack (snaps)
            {
                setResizable (resizable, false);
                setSize (32, 32);
            }

            void attach() { setSize (kWidth, kHeight); }

            void setScaleFactor (float s) override
            {
                scale = s;
                setSize (juce::roundToInt (kWidth * s), juce::roundToInt (kHeight * s));
            }

            void resized() override
            {
                if (snapsBack && scale == 1.0f && getWidth() > 32 && getWidth() < kWidth)
                    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<Editor> (this)]
                    {
                        if (safe != nullptr)
                            safe->setSize (kWidth, kHeight);
                    });
            }

            void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::darkgrey); }

            float scale = 1.0f;
            bool snapsBack;
        };

        NativeEditorPlugin (juce::String f, bool r, bool s)
            : juce::AudioPluginInstance (BusesProperties()
                                             .withInput ("In", juce::AudioChannelSet::stereo())
                                             .withOutput ("Out", juce::AudioChannelSet::stereo())),
              format (std::move (f)), resizable (r), snaps (s)
        {
        }

        Editor* editor = nullptr;

        void fillInPluginDescription (juce::PluginDescription& d) const override
        {
            d.name = getName();
            d.pluginFormatName = format;
            d.fileOrIdentifier = "native-editor-" + format;
            d.numInputChannels = d.numOutputChannels = 2;
        }

        const juce::String getName() const override { return "Native editor " + format; }
        void prepareToPlay (double, int) override {}
        void releaseResources() override {}
        using juce::AudioPluginInstance::processBlock;
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return editor = new Editor (*this, resizable, snaps); }
        bool hasEditor() const override { return true; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return {}; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}

    private:
        juce::String format;
        bool resizable, snaps;
    };
} // namespace

/** The plugin window's zoom with editors that act as real plugins' native ones. */
class PluginChainZoomTests final : public juce::UnitTest
{
public:
    PluginChainZoomTests() : juce::UnitTest ("Plugin window zoom with native editors", "LiveDAW") {}

    void runTest() override
    {
        using V = ui::PluginChainView;
        MixerEngine engine (0);
        OfflineRig rig (engine, 1, 2);
        auto& fx = engine.getFx();

        beginTest ("A native editor that takes its real size once shown zooms by the host scale");
        {
            auto* plugin = new NativeEditorPlugin ("VST3", false, false);
            fx.setPlugin (0, std::unique_ptr<juce::AudioPluginInstance> (plugin));
            V view (fx, 0, {});
            view.setMaxSize ({ 4000, 2000 });
            expect (plugin->editor != nullptr);
            if (plugin->editor != nullptr)
            {
                plugin->editor->attach(); // its real size, as a VST3 view gets once attached
                test::pumpMessages (50);
                expect (view.getZoomMethod (0) == V::ZoomMethod::hostScale);
                view.setZoom (0, 67);
                test::pumpMessages (1000); // the view's timer drops a host scale the editor doesn't follow
                expect (view.getZoomMethod (0) == V::ZoomMethod::hostScale, "the host scale was dropped");
                expectEquals (view.getEditorBounds (0).getWidth(), juce::roundToInt (NativeEditorPlugin::kWidth * 0.67f));
                view.setZoom (0, 100);
                expectEquals (view.getEditorBounds (0).getWidth(), NativeEditorPlugin::kWidth);
            }
        }

        beginTest ("An editor that goes back to its size after a resize falls back to the host scale");
        {
            auto* plugin = new NativeEditorPlugin ("LV2", true, true);
            fx.setPlugin (1, std::unique_ptr<juce::AudioPluginInstance> (plugin));
            V view (fx, 1, {});
            view.setMaxSize ({ 4000, 2000 });
            if (plugin->editor != nullptr)
            {
                plugin->editor->attach();
                test::pumpMessages (50);
                expect (view.getZoomMethod (0) == V::ZoomMethod::resize);
                view.setZoom (0, 67);
                test::pumpMessages (1000);
                expect (view.getZoomMethod (0) == V::ZoomMethod::hostScale, "still shown as resized");
                expectEquals (view.getEditorBounds (0).getWidth(), juce::roundToInt (NativeEditorPlugin::kWidth * 0.67f));
            }
        }

        fx.setPlugin (0, nullptr);
        fx.setPlugin (1, nullptr);
        engine.getGarbage().collectAllNow();
    }
};

static PluginChainZoomTests pluginChainZoomTests;
