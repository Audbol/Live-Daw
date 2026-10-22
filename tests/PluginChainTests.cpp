#include "TestHelpers.h"
#include "engine/PluginChain.h"
#include "engine/Show.h"
#include "sandbox/TestPlugins.h"
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

        beginTest ("A member that fails auto-bypasses the slot and is named");
        {
            for (auto mode : { FakePlugin::Mode::Throw, FakePlugin::Mode::EmitNaN })
            {
                MixerEngine engine (0);
                OfflineRig rig (engine, 1, 2);
                test::setChannels (engine, 1);
                auto& strip = *engine.getChannels()[0];
                strip.params.preInsert.on = true;
                strip.params.preInsert.fx = 0;
                auto& fx = engine.getFx();
                fx.setPlugin (0, std::make_unique<FakePlugin> (0.5f));
                fx.addToChain (0, std::make_unique<FakePlugin> (1.0f, mode, "bad"));
                rig.setInput (0, 0.5f);
                rig.run();

                expect (fx.isFaulted (0));
                expectWithinAbsoluteError (rig.out (0), 0.5f * kCentre, 1.0e-5f); // dry signal passes
                const auto fault = fx.getChain (0)->takeFault();
                expectEquals (fault.member, 1);
                expect (fault.kind == (mode == FakePlugin::Mode::Throw ? PluginChain::FaultKind::threw : PluginChain::FaultKind::nonFinite));
                expectEquals (fx.getChain (0)->takeFault().member, -1); // reported once
                engine.getGarbage().collectAllNow();
            }
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

        beginTest ("The plugin window shows the chain's editors side by side");
        {
            MixerEngine engine (0);
            OfflineRig rig (engine, 1, 2);
            auto& fx = engine.getFx();
            fx.setPlugin (0, TestPluginFormat::create (TestPluginFormat::Kind::gain));
            int adds = 0;
            ui::PluginChainView view (fx, 0, { [&adds] (juce::Component&) { ++adds; }, {}, {} });
            expectEquals (view.getNumColumns(), 1);
            expect (view.shows (fx.getPlugin (0)));
            const int single = view.getWidth();
            expect (single >= ui::PluginChainView::kMinColumnWidth + ui::PluginChainView::kPlusWidth);

            fx.addToChain (0, TestPluginFormat::create (TestPluginFormat::Kind::crash));
            fx.addToChain (0, std::make_unique<FakePlugin> (1.0f)); // no editor: generic controls
            fx.setChainMemberBypassed (0, 1, true);
            view.refresh();
            expectEquals (view.getNumColumns(), 3);
            expect (view.shows (fx.getPlugin (0)) && view.shows (fx.getChainMember (0, 2)));
            expect (view.getWidth() > single); // grows to fit
            expect (view.wantsResizableWindow());

            // Beyond the screen it scrolls.
            const auto natural = view.getBounds();
            view.setMaxSize ({ 500, 2000 });
            expectEquals (view.getWidth(), 500);
            view.setMaxSize ({ 4000, 2000 });
            expect (view.getBounds() == natural);

            if (const auto path = juce::SystemStats::getEnvironmentVariable ("SIDECAR_CHAIN_SNAPSHOT", {}); path.isNotEmpty())
            {
                juce::File file (path);
                file.deleteFile();
                juce::FileOutputStream out (file);
                juce::PNGImageFormat().writeImageToStream (view.createComponentSnapshot (view.getLocalBounds(), true, 1.0f), out);
                logMessage ("Saved " + path);
            }

            // A removed plugin's editor goes before the plugin.
            view.dropEditorFor (fx.getChainMember (0, 2));
            fx.removeFromChain (0, 2);
            expectEquals (view.getNumColumns(), 2);
            view.refresh();
            expectEquals (view.getNumColumns(), 2);
            fx.setPlugin (0, nullptr);
            view.refresh();
            expectEquals (view.getNumColumns(), 0);
            engine.getGarbage().collectAllNow();
        }
    }
};

static PluginChainTests pluginChainTests;
