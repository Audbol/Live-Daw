#include "TestHelpers.h"
#include "engine/Show.h"

using namespace livedaw;

class LayerTests final : public juce::UnitTest
{
public:
    LayerTests() : juce::UnitTest ("Layers", "LiveDAW") {}

    static juce::StringArray names (const std::vector<Layer>& layers)
    {
        juce::StringArray n;
        for (auto& l : layers)
            n.add (l.name);
        return n;
    }

    void runTest() override
    {
        beginTest ("Default layers bank every strip of the console");
        {
            MixerEngine engine (0);
            LayerSet layers;
            engine.setNumInputs (40);
            engine.addBus (StripKind::Aux);
            engine.addBus (StripKind::Aux);
            engine.addBus (StripKind::Group);

            const auto defaults = layers.getDefaultLayers (engine);
            expectEquals (names (defaults).joinIntoString ("|"),
                          juce::String ("Ch 1-16|Ch 17-32|Ch 33-40|Aux|Groups|All"));
            expectEquals ((int) defaults[0].strips.size(), 16);
            expectEquals ((int) defaults[2].strips.size(), 8);
            expectEquals ((int) defaults[3].strips.size(), 2);
            expectEquals ((int) defaults.back().strips.size(), 43);

            // Every strip except the (always visible) master is on some bank layer.
            int covered = 0;
            for (size_t i = 0; i + 1 < defaults.size(); ++i)
                covered += (int) defaults[i].strips.size();
            expectEquals (covered, 43);
        }

        beginTest ("Defaults follow the console and the bank size");
        {
            MixerEngine engine (0);
            LayerSet layers;
            engine.setNumInputs (8);
            expectEquals (names (layers.getDefaultLayers (engine)).joinIntoString ("|"), juce::String ("Ch|All"));

            engine.setNumInputs (20);
            layers.setBankSize (8);
            expectEquals (names (layers.getDefaultLayers (engine)).joinIntoString ("|"),
                          juce::String ("Ch 1-8|Ch 9-16|Ch 17-20|All"));

            // Bank ids are stable so the selection survives topology changes.
            expectEquals (layers.getDefaultLayers (engine)[1].id, juce::String ("in-2"));
        }

        beginTest ("User layers hold any strips in any order");
        {
            MixerEngine engine (0);
            LayerSet layers;
            engine.setNumInputs (4);
            auto* aux = engine.addBus (StripKind::Aux);
            auto& ch3 = *engine.getInputs()[2];
            auto& ch1 = *engine.getInputs()[0];

            const auto id = layers.addUserLayer ("Vocals + wedge");
            layers.addToUserLayer (id, state::StripRef::of (engine.getMaster()));
            layers.addToUserLayer (id, state::StripRef::of (ch3));
            layers.addToUserLayer (id, state::StripRef::of (*aux));
            layers.addToUserLayer (id, state::StripRef::of (ch1));
            layers.addToUserLayer (id, state::StripRef::of (ch3)); // duplicate ignored

            auto strips = LayerSet::resolve (engine, layers.findLayer (engine, id));
            expectEquals ((int) strips.size(), 4);
            expect (strips[0] == &engine.getMaster());
            expect (strips[1] == &ch3);
            expect (strips[2] == aux);
            expect (strips[3] == &ch1);

            layers.moveWithinUserLayer (id, 3, -2); // ch1 before the aux... then before ch3
            strips = LayerSet::resolve (engine, layers.findLayer (engine, id));
            expect (strips[1] == &ch1);

            // Removed strips simply disappear from the layer.
            engine.removeStrip (&ch3);
            strips = LayerSet::resolve (engine, layers.findLayer (engine, id));
            expectEquals ((int) strips.size(), 3);
            engine.getGarbage().collectAllNow();
        }

        beginTest ("Unknown layer ids fall back to the first layer");
        {
            MixerEngine engine (0);
            LayerSet layers;
            engine.setNumInputs (2);
            expectEquals (layers.findLayer (engine, "groups-3").id, juce::String ("in-1"));
        }

        beginTest ("Layers are saved with the show");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumInputs (6);
            auto* group = engine.addBus (StripKind::Group);
            show.layers.setBankSize (12);
            const auto id = show.layers.addUserLayer ("Band", { state::StripRef::of (*engine.getInputs()[5]),
                                                                state::StripRef::of (*group) });
            show.layers.selectedLayerId = id;

            auto xml = show.toXml();
            MixerEngine e2 (0);
            Show loaded (e2, host);
            expect (loaded.loadFromXml (*xml).wasOk());

            expectEquals (loaded.layers.getBankSize(), 12);
            expectEquals (loaded.layers.selectedLayerId, id);
            expectEquals ((int) loaded.layers.getUserLayers().size(), 1);
            const auto strips = LayerSet::resolve (e2, loaded.layers.findLayer (e2, id));
            expectEquals ((int) strips.size(), 2);
            expect (strips[1]->getKind() == StripKind::Group);

            loaded.clear (4);
            expect (loaded.layers.getUserLayers().empty());
        }
    }
};

static LayerTests layerTests;
