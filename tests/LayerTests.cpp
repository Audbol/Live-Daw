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

    static std::vector<Strip*> resolve (const MixerEngine& engine, const Layer& layer)
    {
        std::vector<Strip*> strips;
        for (auto& f : layer.faders)
            if (! f.dca)
                if (auto* s = state::findStrip (engine, f.strip))
                    strips.push_back (s);
        return strips;
    }

    void runTest() override
    {
        using S = LayerSet::Section;

        beginTest ("Layer keys follow the WING's three fader sections");
        {
            MixerEngine engine (0);
            LayerSet layers;
            engine.setNumChannels (40);

            auto labels = [&] (S section)
            {
                juce::StringArray l;
                for (auto& layer : layers.getDefaultLayers (engine, section))
                    l.add (LayerSet::keyLabel (layer).replace ("\n", " "));
                return l.joinIntoString ("|");
            };
            expectEquals (labels (S::Left), juce::String ("1-12|13-24|25-36|37-40 AUX IN|BUS MASTER|USER 1|USER 2"));
            expectEquals (labels (S::Center), juce::String ("DCA|MAIN MATRIX|AUX IN|INPUTS|BUS MASTER|USER 1|USER 2"));
            expectEquals (labels (S::Right), juce::String ("MAIN MATRIX|DCA|INPUTS|AUX IN|BUS MASTER|USER 1|USER 2"));
            expectEquals (names (layers.getDefaultLayers (engine, S::Left)).joinIntoString ("|"),
                          juce::String::fromUTF8 ("CH 1-12|CH 13-24|CH 25-36|CH 37-40 \xc2\xb7 AUX 1-8|BUS 1-16|USER 1|USER 2"));

            // The first key of each section is lit after a reset; long layers stay one key.
            expectEquals (layers.getSelectedLayer (engine, S::Center).name, juce::String ("DCA 1-16"));
            expectEquals (layers.getSelectedLayer (engine, S::Right).name, juce::String::fromUTF8 ("MAIN 1-4 \xc2\xb7 MTX 1-8"));
            expectEquals ((int) layers.findLayer (engine, S::Center, "inputs").faders.size(), 40);

            // Layer ids of older shows still resolve.
            const auto dcas = layers.findLayer (engine, S::Center, "dca-2");
            expectEquals (dcas.id, juce::String ("dca-1"));
            expect (dcas.faders[8].dca && dcas.faders[8].dcaIndex == 8);
            expectEquals (layers.findLayer (engine, S::Left, "bus-2").id, juce::String ("bus-1"));
            expectEquals (layers.findLayer (engine, S::Right, "in-2").id, juce::String ("inputs"));
            expectEquals (layers.findLayer (engine, S::Left, "all").id, juce::String ("in-1"));
        }

        beginTest ("Banks follow the console and the section width");
        {
            MixerEngine engine (0);
            LayerSet layers;
            engine.setNumChannels (8);
            expectEquals (layers.getDefaultLayers (engine, S::Left)[0].name, juce::String::fromUTF8 ("CH 1-8 \xc2\xb7 AUX 1-8"));
            expectEquals (LayerSet::keyLabel (layers.getDefaultLayers (engine, S::Left)[0]), juce::String ("1-8\nAUX IN"));

            engine.setNumChannels (100);
            layers.setWidth (S::Left, LayerSet::kAutoWidth);
            layers.setFittedWidth (S::Left, 48); // e.g. an ultrawide monitor
            const auto banks = layers.getDefaultLayers (engine, S::Left);
            expectEquals (banks[0].name, juce::String ("CH 1-48"));
            expectEquals (banks[2].name, juce::String::fromUTF8 ("CH 97-100 \xc2\xb7 AUX 1-8"));

            // After a resize, the bank holding channel 60 is found again.
            const auto ch60 = state::FaderRef::of (state::StripRef::of (*engine.getChannels()[59]));
            expectEquals (layers.findDefaultLayerContaining (engine, S::Left, ch60), juce::String ("in-2"));
            layers.setFittedWidth (S::Left, 20);
            expectEquals (layers.findDefaultLayerContaining (engine, S::Left, ch60), juce::String ("in-3"));

            layers.setWidth (S::Left, 12);
            expectEquals (layers.getEffectiveWidth (S::Left), 12);
        }

        beginTest ("User banks hold any faders in any order");
        {
            MixerEngine engine (0);
            LayerSet layers;
            engine.setNumChannels (4);
            auto& bus = *engine.getBusInSlot (3);
            auto& ch3 = *engine.getChannels()[2];
            auto& ch1 = *engine.getChannels()[0];

            const auto id = layers.addUserLayer ("Vocals + wedge");
            layers.addToUserLayer (id, state::FaderRef::of (state::StripRef::of (engine.getMain())));
            layers.addToUserLayer (id, state::FaderRef::of (state::StripRef::of (ch3)));
            layers.addToUserLayer (id, state::FaderRef::of (state::StripRef::of (bus)));
            layers.addToUserLayer (id, state::FaderRef::ofDca (5));
            layers.addToUserLayer (id, state::FaderRef::of (state::StripRef::of (ch1)));
            layers.addToUserLayer (id, state::FaderRef::of (state::StripRef::of (ch3))); // duplicate ignored

            auto layer = layers.findLayer (engine, S::Center, id);
            expectEquals ((int) layer.faders.size(), 5);
            expect (layer.faders[3].dca && layer.faders[3].dcaIndex == 5);
            auto strips = resolve (engine, layer);
            expect (strips[0] == &engine.getMain());
            expect (strips[1] == &ch3);
            expect (strips[2] == &bus);
            expect (strips[3] == &ch1);

            layers.moveWithinUserLayer (id, 4, -1);
            layers.moveWithinUserLayer (id, 3, -1);
            strips = resolve (engine, layers.findLayer (engine, S::Center, id));
            expect (strips[2] == &ch1);

            // The first user bank is on every section's USER 1 key.
            const auto user1 = layers.findLayer (engine, S::Left, "user-1");
            expectEquals (user1.name, juce::String ("Vocals + wedge"));
            expectEquals (LayerSet::keyLabel (user1), juce::String ("USER 1"));
            expectEquals ((int) user1.faders.size(), 5);
            expect (layers.findLayer (engine, S::Right, "user-2").faders.empty());

            // Removed strips simply disappear from the bank.
            engine.removeStrip (&ch3);
            strips = resolve (engine, layers.findLayer (engine, S::Center, id));
            expectEquals ((int) strips.size(), 3);
            engine.getGarbage().collectAllNow();
        }

        beginTest ("Unknown bank ids fall back to the section's first bank");
        {
            MixerEngine engine (0);
            LayerSet layers;
            engine.setNumChannels (2);
            expectEquals (layers.findLayer (engine, S::Right, "groups-3").id, juce::String ("out-1"));
            expectEquals (layers.findLayer (engine, S::Left, "groups-3").id, juce::String ("in-1"));
        }

        beginTest ("Banks are saved with the show");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumChannels (6);
            show.layers.setWidth (S::Left, LayerSet::kAutoWidth);
            const auto id = show.layers.addUserLayer ("Band", { state::FaderRef::of (state::StripRef::of (*engine.getChannels()[5])),
                                                                state::FaderRef::ofDca (0) });
            show.layers.setSelectedLayerId (S::Right, id);

            auto xml = show.toXml();
            MixerEngine e2 (0);
            Show loaded (e2, host);
            expect (loaded.loadFromXml (*xml).wasOk());

            expect (loaded.layers.isAutoWidth (S::Left));
            expectEquals (loaded.layers.getSelectedLayerId (S::Right), id);
            expectEquals (loaded.layers.getSelectedLayerId (S::Left), juce::String ("in-1"));
            expectEquals ((int) loaded.layers.getUserLayers().size(), 1);
            const auto layer = loaded.layers.findLayer (e2, S::Right, id);
            expectEquals ((int) layer.faders.size(), 2);
            expect (layer.faders[1].dca);

            loaded.clear (4);
            expect (loaded.layers.getUserLayers().empty());
            expectEquals ((int) e2.getChannels().size(), 4);
        }
    }
};

static LayerTests layerTests;
