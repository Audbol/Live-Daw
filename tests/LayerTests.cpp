#include "TestHelpers.h"
#include "engine/Show.h"
#include "ui/LayerDrop.h"

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
            if (! f.dca && ! f.isEmpty())
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
            expectEquals (labels (S::Center), juce::String ("DCA|MAIN MATRIX|AUX IN|BUS MASTER|USER 1|USER 2")); // no INPUTS in the center
            expectEquals (labels (S::Right), juce::String ("MAIN MATRIX|DCA|INPUTS|AUX IN|BUS MASTER|USER 1|USER 2"));
            expectEquals (names (layers.getDefaultLayers (engine, S::Left)).joinIntoString ("|"),
                          juce::String::fromUTF8 ("CH 1-12|CH 13-24|CH 25-36|CH 37-40 \xc2\xb7 AUX 1-8|BUS 1-16|USER 1|USER 2"));

            // The first key of each section is lit after a reset; long layers stay one key.
            expectEquals (layers.getSelectedLayer (engine, S::Center).name, juce::String ("DCA 1-16"));
            expectEquals (layers.getSelectedLayer (engine, S::Right).name, juce::String::fromUTF8 ("MAIN 1-4 \xc2\xb7 MTX 1-8"));
            expectEquals ((int) layers.findLayer (engine, S::Right, "inputs").faders.size(), 40);

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
            expect (layers.addToUserLayer (id, state::FaderRef::of (state::StripRef::of (ch3)))); // a second copy, as on the WING

            auto layer = layers.findLayer (engine, S::Center, id);
            expectEquals ((int) layer.faders.size(), 6);
            expect (layer.faders[5] == layer.faders[1]);
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
            expectEquals ((int) user1.faders.size(), 6);
            expect (layers.findLayer (engine, S::Right, "user-2").faders.empty());

            // Removing one copy leaves the other.
            const auto ch3ref = state::FaderRef::of (state::StripRef::of (ch3));
            layers.removeFromUserLayer (id, ch3ref, 5);
            layer = layers.findLayer (engine, S::Center, id);
            expect (layer.faders.size() == 5 && layer.faders[1] == ch3ref);
            layers.addToUserLayer (id, ch3ref);
            layers.removeFromUserLayer (id, ch3ref, 0); // slot 1 doesn't hold it: nothing
            layers.removeFromUserLayer (id, ch3ref);    // the first copy
            layer = layers.findLayer (engine, S::Center, id);
            expect (layer.faders.size() == 6 && layer.faders[1].isEmpty() && layer.faders[5] == ch3ref);
            layers.setUserLayerSlot (id, 1, ch3ref);

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

        beginTest ("User layer slots are fixed: CONFIG drops land where they're dropped");
        {
            using state::FaderRef;
            const auto ch = [] (int id) { return FaderRef::of ({ StripKind::Channel, id }); };
            const auto blank = FaderRef::blank();
            const auto isBlank = [] (const std::vector<FaderRef>& f, std::initializer_list<int> slots)
            {
                for (int i : slots)
                    if (! layerslots::get (f, i).isEmpty())
                        return false;
                return true;
            };

            expect (blank.isEmpty() && ! blank.isStrip() && blank == FaderRef::blank() && ! (blank == ch (0)) && ! (ch (0) == blank));
            expect (FaderRef::fromC (blank.toC()).isEmpty());

            // The reported bug: an item dropped on slot 6 of an empty layer goes to slot 6.
            std::vector<FaderRef> f;
            layerslots::place (f, { ch (1) }, 5);
            expectEquals ((int) f.size(), 6);
            expect (f[5] == ch (1) && isBlank (f, { 0, 1, 2, 3, 4, 6, 7 }));
            // Another on slot 3: slot 6 stays.
            layerslots::place (f, { ch (2) }, 2);
            expect (f[2] == ch (2) && f[5] == ch (1) && isBlank (f, { 0, 1, 3, 4 }));
            // Several items fill the slots from the target on, replacing what's there.
            layerslots::place (f, { ch (3), ch (4) }, 4);
            expect (f[2] == ch (2) && f[4] == ch (3) && f[5] == ch (4) && isBlank (f, { 0, 1, 3, 6 }));
            // An item already in the layer gets another slot; the first copy stays.
            layerslots::place (f, { ch (2) }, 0);
            expect (f[0] == ch (2) && f[2] == ch (2) && f[4] == ch (3) && f[5] == ch (4) && isBlank (f, { 1, 3 }));
            // Dropped onto its own slot: nothing changes.
            const auto before = f;
            layerslots::place (f, { ch (3) }, 4);
            expect (f == before);
            layerslots::clear (f, 2);
            // A slot dragged onto a filled slot swaps with it; onto a blank slot it moves.
            layerslots::swap (f, 0, 5);
            expect (f[0] == ch (4) && f[5] == ch (2) && f[4] == ch (3));
            layerslots::swap (f, 4, 9);
            expect (f[9] == ch (3) && isBlank (f, { 1, 2, 3, 4, 6, 7, 8 }) && f[0] == ch (4) && f[5] == ch (2));
            expectEquals ((int) f.size(), 10);
            // Clearing blanks the slot alone; trailing blanks are dropped.
            layerslots::clear (f, 0);
            expect (isBlank (f, { 0 }) && f[5] == ch (2) && f[9] == ch (3));
            layerslots::clear (f, 9);
            expectEquals ((int) f.size(), 6);
            layerslots::set (f, layerslots::maxSlots(), ch (7));
            expectEquals ((int) f.size(), 6);
            layerslots::set (f, -1, ch (7));
            layerslots::swap (f, -1, 2);
            expectEquals ((int) f.size(), 6);

            // The surface: a blank slot (or past the end) is an inactive strip.
            Layer layer;
            layer.faders = { blank, ch (1), blank, FaderRef::ofDca (2) };
            expect (! LayerSet::surfaceTarget (layer, 0).has_value());
            expect (LayerSet::surfaceTarget (layer, 1) == std::optional<FaderRef> (ch (1)));
            expect (! LayerSet::surfaceTarget (layer, 2).has_value());
            expect (LayerSet::surfaceTarget (layer, 3)->dca);
            expect (! LayerSet::surfaceTarget (layer, 4).has_value() && ! LayerSet::surfaceTarget (layer, -1).has_value());

            // Through the C ABI and a show round trip: the blanks stay.
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumChannels (8);
            const auto c1 = FaderRef::of (state::StripRef::of (*engine.getChannels()[0]));
            const auto c5 = FaderRef::of (state::StripRef::of (*engine.getChannels()[4]));
            const auto id = show.layers.addUserLayer ("USER 1");
            show.layers.setUserLayerFaders (id, { blank, c1, blank, blank, FaderRef::ofDca (0), blank });
            auto user1 = show.layers.findLayer (engine, S::Left, "user-1");
            expectEquals ((int) user1.faders.size(), 5);
            expect (user1.faders[1] == c1 && user1.faders[4].dca && isBlank (user1.faders, { 0, 2, 3, 5 }));
            show.layers.setUserLayerSlot (id, 7, c5);
            show.layers.swapUserLayerSlots (id, 1, 3);
            show.layers.clearUserLayerSlot (id, 4);
            user1 = show.layers.findLayer (engine, S::Center, "user-1");
            expect (user1.faders.size() == 8 && user1.faders[3] == c1 && user1.faders[7] == c5 && isBlank (user1.faders, { 0, 1, 2, 4, 5, 6 }));

            auto xml = show.toXml();
            MixerEngine e2 (0);
            Show loaded (e2, host);
            expect (loaded.loadFromXml (*xml).wasOk());
            const auto again = loaded.layers.findLayer (e2, S::Right, "user-1");
            expect (again.faders == user1.faders);
            expect (! LayerSet::surfaceTarget (again, 0).has_value() && LayerSet::surfaceTarget (again, 3).has_value());

            // "Go to strip" never looks for a blank slot.
            expect (show.layers.findDefaultLayerContaining (engine, S::Left, blank).isEmpty());
        }

        beginTest ("CONFIG drops: on a slot, between two slots, duplicates, the layer's maximum");
        {
            using state::FaderRef;
            using DT = layerslots::DropTarget;
            const auto ch = [] (int id) { return FaderRef::of ({ StripKind::Channel, id }); };
            const auto blank = FaderRef::blank();

            // The page's slots as CONFIG lays them out, small (a 600 px wide
            // window): 40 x 30, 4 apart, a wider gap between the groups of four.
            std::vector<juce::Rectangle<int>> slots;
            for (int i = 0; i < 8; ++i)
                slots.push_back ({ 10 + 44 * i + (i / 4) * 8, 100, 40, 30 });
            const int first = 8; // the second page
            const auto at = [&] (int x, int y = 115) { return layerdrop::targetAt (slots, first, { x, y }); };
            // A slot's body: that slot; its left or right quarter: insert before or after it.
            expect (at (slots[2].getCentreX()) == DT { 10, false });
            expect (at (slots[2].getX() + 2) == DT { 10, true });
            expect (at (slots[2].getRight() - 2) == DT { 11, true });
            expect (at (slots[2].getX() - 2) == DT { 10, true }); // the gap left of it
            expect (at (slots[3].getRight() + 6) == DT { 12, true }); // the gap between the groups
            expect (at (slots[0].getX() - 1) == DT { 8, true });
            expect (at (slots[7].getRight()) == DT { 16, true }); // after the last on the page
            expect (at (slots[2].getCentreX(), 300).index < 0);    // off the slots
            expect (at (slots[2].getCentreX(), 97) == DT { 10, false });
            // The marker sits on the boundary, taller than the slots.
            const auto marker = layerdrop::markerAt (slots, 4, 4);
            expect (marker.getCentreX() >= slots[3].getRight() && marker.getCentreX() <= slots[4].getX());
            expect (marker.getY() < slots[4].getY() && marker.getBottom() > slots[4].getBottom());
            expect (layerdrop::markerAt (slots, 9, 4).isEmpty());

            // Items from the grid on a slot fill or replace exactly that slot; one
            // already in the layer is copied, the other copy stays.
            std::vector<FaderRef> f { ch (1), ch (2), blank, ch (3) };
            expect (layerslots::dropItems (f, { ch (5) }, { 2, false }));
            expect ((f == std::vector<FaderRef> { ch (1), ch (2), ch (5), ch (3) }));
            expect (layerslots::dropItems (f, { ch (1) }, { 3, false }));
            expect ((f == std::vector<FaderRef> { ch (1), ch (2), ch (5), ch (1) }));
            // Between two slots they're inserted and the following slots move right.
            expect (layerslots::dropItems (f, { ch (2) }, { 1, true }));
            expect ((f == std::vector<FaderRef> { ch (1), ch (2), ch (2), ch (5), ch (1) }));
            expect (layerslots::dropItems (f, { ch (7), ch (8) }, { 0, true }));
            expect ((f == std::vector<FaderRef> { ch (7), ch (8), ch (1), ch (2), ch (2), ch (5), ch (1) }));
            // An empty slot inserted (INSERT EMPTY SLOT) shifts too.
            expect (layerslots::insert (f, 2, { blank }));
            expect ((f == std::vector<FaderRef> { ch (7), ch (8), blank, ch (1), ch (2), ch (2), ch (5), ch (1) }));
            // Past the end: just placed, blanks before it.
            expect (layerslots::dropItems (f, { ch (9) }, { 10, true }));
            expect (f.size() == 11 && f[10] == ch (9) && f[8].isEmpty() && f[9].isEmpty());
            f.resize (8);

            // A slot dragged: onto a slot it swaps (moves into a blank one) ...
            expect (layerslots::dropSlot (f, 0, { 1, false }));
            expect ((f == std::vector<FaderRef> { ch (8), ch (7), blank, ch (1), ch (2), ch (2), ch (5), ch (1) }));
            expect (layerslots::dropSlot (f, 1, { 2, false }));
            expect ((f == std::vector<FaderRef> { ch (8), blank, ch (7), ch (1), ch (2), ch (2), ch (5), ch (1) }));
            // ... between two slots it's inserted there, its old slot blank.
            expect (layerslots::dropSlot (f, 6, { 0, true }));
            expect ((f == std::vector<FaderRef> { ch (5), ch (8), blank, ch (7), ch (1), ch (2), ch (2), blank, ch (1) }));
            expect (layerslots::dropSlot (f, 0, { 3, true }));
            expect ((f == std::vector<FaderRef> { blank, ch (8), blank, ch (5), ch (7), ch (1), ch (2), ch (2), blank, ch (1) }));
            // Next to itself, or a blank slot: nothing changes.
            const auto before = f;
            expect (layerslots::dropSlot (f, 3, { 3, true }) && layerslots::dropSlot (f, 3, { 4, true }));
            expect (layerslots::dropSlot (f, 2, { 5, false }) && layerslots::dropSlot (f, 3, {}));
            expect (f == before);

            // The layer's maximum: a trailing blank makes room, a filled last slot refuses.
            const int max = layerslots::maxSlots();
            expectEquals (max, 1024);
            std::vector<FaderRef> full ((size_t) max - 1, ch (1));
            expect (layerslots::dropItems (full, { ch (2) }, { 0, true }));
            expect ((int) full.size() == max && full[0] == ch (2) && full.back() == ch (1));
            const auto fullBefore = full;
            expect (! layerslots::dropItems (full, { ch (3) }, { 5, true }));
            expect (! layerslots::insert (full, 5, { blank }));
            expect (! layerslots::dropSlot (full, 9, { 2, true })); // the whole tail moves: no room either
            expect (full == fullBefore);
            expect (! layerslots::dropItems (full, { ch (3), ch (4) }, { max - 1, false }));
            expect (full == fullBefore);
            // Moving the last slot works: its old slot is the trailing blank that makes room.
            expect (layerslots::dropSlot (full, max - 1, { 0, true }));
            expect ((int) full.size() == max && full[0] == ch (1) && full[1] == ch (2));
            // Replacing a slot of a full layer is fine.
            expect (layerslots::dropItems (full, { ch (5) }, { max - 1, false }) && full.back() == ch (5));

            // The same model through the C ABI: insert, refuse when full, duplicates.
            LayerSet layers;
            const auto id = layers.addUserLayer ("Dup", { ch (1), blank, ch (1) });
            expect (layers.insertUserLayerSlots (id, 1, { ch (2), blank }));
            expect ((layers.getUserLayer (id)->faders == std::vector<FaderRef> { ch (1), ch (2), blank, blank, ch (1) }));
            layers.setUserLayerSlot (id, max - 1, ch (3));
            expect (! layers.insertUserLayerSlots (id, 0, { ch (4) }));
            expect ((int) layers.getUserLayer (id)->faders.size() == max && layers.getUserLayer (id)->faders[1] == ch (2));
        }

        beginTest ("The surface shows a fader twice: both strips drive one strip");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumChannels (4);
            auto& ch2 = *engine.getChannels()[1];
            const auto ref = state::FaderRef::of (state::StripRef::of (ch2));
            const auto id = show.layers.addUserLayer ("Twice", { ref, state::FaderRef::blank(), ref });
            show.layers.setSelectedLayerId (S::Left, "user-1");
            const auto layer = show.layers.getSelectedLayer (engine, S::Left);
            const auto a = LayerSet::surfaceTarget (layer, 0), b = LayerSet::surfaceTarget (layer, 2);
            expect (a.has_value() && b.has_value() && *a == *b && ! LayerSet::surfaceTarget (layer, 1).has_value());
            // Both resolve to the same strip, so a move on one shows on the other.
            expect (state::findStrip (engine, a->strip) == &ch2 && state::findStrip (engine, b->strip) == &ch2);
            state::findStrip (engine, a->strip)->params.faderDb = -12.5f;
            expectWithinAbsoluteError (state::findStrip (engine, b->strip)->params.faderDb.load(), -12.5f, 1.0e-6f);
            // Saved and loaded, both copies and the blank stay.
            auto xml = show.toXml();
            MixerEngine e2 (0);
            Show loaded (e2, host);
            expect (loaded.loadFromXml (*xml).wasOk());
            const auto again = loaded.layers.findLayer (e2, S::Center, "user-1").faders;
            expect (again.size() == 3 && again[0] == again[2] && again[1].isEmpty());
            expect (show.layers.findDefaultLayerContaining (engine, S::Left, ref) == "in-1");
            juce::ignoreUnused (id);
        }

        beginTest ("Spill layers, send, FX parameter and MIDI CC faders");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumChannels (4);
            engine.getChannels()[1]->params.dcaMask = 1u << 3;
            engine.getChannels()[3]->params.dcaMask = 1u << 3;

            expect (LayerSet::spillTarget (S::Right) == S::Center);
            expect (LayerSet::spillTarget (std::nullopt) == S::Left);

            auto& layers = show.layers;
            layers.setSelectedLayerId (S::Center, "inputs");
            layers.setOffset (S::Center, 8);
            const LayerSet::Spill dca { LayerSet::Spill::Kind::Dca, 3, {} };
            layers.startSpill (LayerSet::spillTarget (S::Right), dca);
            expect (layers.getSpill (S::Center).has_value() && *layers.getSpill (S::Center) == dca);
            expect (layers.findSpill (dca) == std::optional<S> (S::Center));
            expectEquals (layers.getSelectedLayerId (S::Center), juce::String (LayerSet::kSpillId));
            const auto spilled = layers.getSelectedLayer (engine, S::Center);
            expectEquals ((int) spilled.faders.size(), 2);
            expect (spilled.faders[0] == state::FaderRef::of (state::StripRef::of (*engine.getChannels()[1])));
            expectEquals (layers.getOffset (S::Center), 0);
            expect (layers.endSpill (S::Center));
            expectEquals (layers.getSelectedLayerId (S::Center), juce::String ("inputs"));
            expectEquals (layers.getOffset (S::Center), 8);

            // The selected strip's sends, and an FX slot's parameters.
            const auto ch1 = state::StripRef::of (*engine.getChannels()[0]);
            layers.startSpill (S::Left, { LayerSet::Spill::Kind::Sends, 0, ch1 });
            const auto sends = layers.getSelectedLayer (engine, S::Left);
            expectEquals ((int) sends.faders.size(), 16 + 4 + 8);
            expect (sends.faders[0].send && sends.faders[0].strip == ch1 && sends.faders[0].to.kind == StripKind::Bus);
            layers.setSelectedLayerId (S::Left, "in-2"); // a layer key ends the spill
            expect (! layers.getSpill (S::Left).has_value());

            // Send, FX parameter and MIDI CC faders in a user layer survive the C ABI and a show round trip.
            const auto send = state::FaderRef::ofSend (ch1, { StripKind::Matrix, 2 });
            const auto fxp = state::FaderRef::ofFxParam (4, 7);
            const auto midiCc = state::FaderRef::ofMidiCc (9, 74);
            expect (! midiCc.isStrip() && ! (midiCc == state::FaderRef::ofMidiCc (9, 75)) && state::FaderRef::ofMidiCc (99, 300).cc == 127);
            const auto id = layers.addUserLayer ("Mix", { send, fxp, midiCc });
            auto xml = show.toXml();
            MixerEngine e2 (0);
            Show loaded (e2, host);
            expect (loaded.loadFromXml (*xml).wasOk());
            const auto* user = loaded.layers.getUserLayer (id);
            expect (user != nullptr && user->faders.size() == 3 && user->faders[0] == send && user->faders[1] == fxp && user->faders[2] == midiCc);
            expect (! user->faders[0].isStrip() && ! (user->faders[0] == state::FaderRef::of (ch1)));
        }
    }
};

static LayerTests layerTests;
