#include "TestHelpers.h"
#include "engine/Models.h"
#include "engine/Show.h"

using namespace livedaw;
using livedaw::test::FakePlugin;

class ShowTests final : public juce::UnitTest
{
public:
    ShowTests() : juce::UnitTest ("Show", "LiveDAW") {}

    void runTest() override
    {
        beginTest ("Console survives a show file round trip");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumChannels (3);
            auto* extraBus = engine.addBus();
            auto* bus = engine.getBusInSlot (0);
            bus->setName ("Wedge 1");
            bus->params.outputLeft = 5;

            auto* kick = engine.getChannels()[1].get();
            auto& p = kick->params;
            kick->setName ("Kick");
            kick->setColourIndex (9);
            p.stereo = true;
            p.input.trimDb = 12.0f;
            p.filter.lowCut = true;
            p.gate.values[0] = -33.0f; // GATE thr
            p.eq.values[7] = -4.5f;    // STD 2g
            kick->setModel (ModelBlock::Dynamics, models::findModel (models::Dynamics, "B560"));
            p.processOrder = 7;
            p.recallSafe = true;
            p.dcaMask = 0b101u;
            p.mains[2].on = true;
            kick->sends[0].enabled = true;
            kick->sends[0].levelDb = -3.0f;
            kick->sends[(size_t) extraBus->getId()].enabled = true;
            engine.monitorOutputLeft = 6;
            engine.outputCeilingDb = -1.0f;
            engine.setDcaName (2, "Drums");
            engine.getDca (2).faderDb = -5.0f;
            engine.setMuteGroupName (0, "Band");
            engine.muteGroups = 1u;

            // Remove channel 1 so ids are no longer contiguous: they must survive.
            engine.removeStrip (engine.getChannels()[0].get());
            const int kickId = kick->getId();

            auto xml = show.toXml();

            MixerEngine restoredEngine (0);
            Show restored (restoredEngine, host);
            const auto result = restored.loadFromXml (*xml);
            expect (result.wasOk(), result.getErrorMessage());
            expectEquals ((int) restoredEngine.getChannels().size(), 2);
            expectEquals ((int) restoredEngine.getBuses().size(), 17);

            auto* r = state::findStrip (restoredEngine, { StripKind::Channel, kickId });
            expect (r != nullptr);
            if (r == nullptr)
                return;

            expectEquals (r->getName(), juce::String ("Kick"));
            expectEquals (r->getColourIndex(), 9);
            expect (r->params.stereo.load());
            expectEquals (r->params.input.trimDb.load(), 12.0f);
            expect (r->params.filter.lowCut.load());
            expectEquals (r->params.gate.values[0].load(), -33.0f);
            expectEquals (r->params.eq.values[7].load(), -4.5f);
            expectEquals (models::model (models::Dynamics, r->params.dynamics.model.load()).token, juce::String ("B560"));
            expectEquals (r->params.processOrder.load(), 7);
            expect (r->params.recallSafe.load());
            expectEquals ((int) r->params.dcaMask.load(), 0b101);
            expect (r->params.mains[2].on.load());
            expect (r->sends[0].enabled.load());
            expectEquals (r->sends[0].levelDb.load(), -3.0f);
            expect (r->sends[(size_t) extraBus->getId()].enabled.load());
            expectEquals (restoredEngine.monitorOutputLeft.load(), 6);
            expectEquals (restoredEngine.outputCeilingDb.load(), -1.0f);
            expectEquals (restoredEngine.getBusInSlot (0)->getName(), juce::String ("Wedge 1"));
            expectEquals (restoredEngine.getDcaName (2), juce::String ("Drums"));
            expectEquals (restoredEngine.getDca (2).faderDb.load(), -5.0f);
            expectEquals (restoredEngine.getMuteGroupName (0), juce::String ("Band"));
            expectEquals ((int) restoredEngine.muteGroups.load(), 1);

            // New channels must not reuse a restored id.
            expect (restoredEngine.addChannel()->getId() > kickId);
            engine.getGarbage().collectAllNow();
        }

        beginTest ("Scenes store and recall the console");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumChannels (2);
            auto& ch1 = engine.getChannels()[0]->params;

            ch1.faderDb = -10.0f;
            ch1.eq.values[4] = 4.0f;
            expectEquals (show.addScene ("Verse"), 0);

            ch1.faderDb = 3.0f;
            ch1.eq.values[4] = -2.0f;
            ch1.mute = true;
            expectEquals (show.addScene ("Chorus"), 1);

            show.recallScene (0);
            expectEquals (ch1.faderDb.load(), -10.0f);
            expectEquals (ch1.eq.values[4].load(), 4.0f);
            expect (! ch1.mute.load());
            expectEquals (show.getCurrentSceneIndex(), 0);

            show.recallNext();
            expectEquals (ch1.faderDb.load(), 3.0f);
            expect (ch1.mute.load());
            expectEquals (show.getCurrentSceneIndex(), 1);

            show.recallPrevious();
            expectEquals (show.getCurrentSceneIndex(), 0);
        }

        beginTest ("Scenes recall sends to high-numbered buses");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumChannels (2);
            for (int i = 0; i < 80 - kWingBuses; ++i)
                engine.addBus();

            auto& send = engine.getChannels()[1]->sends[(size_t) 79];
            send.enabled = true;
            send.levelDb = -7.0f;
            show.addScene ("High sends");

            send.enabled = false;
            send.levelDb = 0.0f;
            show.recallScene (0);
            expect (send.enabled.load());
            expectEquals (send.levelDb.load(), -7.0f);

            // And through a show file.
            auto xml = show.toXml();
            MixerEngine e2 (0);
            Show loaded (e2, host);
            expect (loaded.loadFromXml (*xml).wasOk());
            expectEquals ((int) e2.getBuses().size(), 80);
            auto* restored = e2.getChannels()[1]->sends.find (79);
            expect (restored != nullptr && restored->enabled.load());
        }

        beginTest ("Recall scope limits what a scene changes");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumChannels (1);
            auto& p = engine.getChannels()[0]->params;

            p.faderDb = -20.0f;
            p.dynamics.values[0] = -30.0f;
            p.inputLeft = 0;
            show.addScene ("A");
            show.setSceneScope (0, state::Fader); // only faders

            p.faderDb = 0.0f;
            p.dynamics.values[0] = -10.0f;
            p.inputLeft = 5;
            show.recallScene (0);

            expectEquals (p.faderDb.load(), -20.0f);
            expectEquals (p.dynamics.values[0].load(), -10.0f); // out of scope
            expectEquals (p.inputLeft.load(), 5);              // routing is never in the default scope
        }

        beginTest ("Recall safe channels are skipped by scenes but not by snippets");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumChannels (2);
            auto& safe = engine.getChannels()[0]->params;
            auto& other = engine.getChannels()[1]->params;

            safe.faderDb = -5.0f;
            other.faderDb = -5.0f;
            show.addScene ("A");
            show.addSnippet (0, "Safe fader", state::Fader, { state::StripRef::of (*engine.getChannels()[0]) });

            safe.faderDb = 0.0f;
            other.faderDb = 0.0f;
            safe.recallSafe = true;

            const auto result = show.recallScene (0);
            expectEquals (safe.faderDb.load(), 0.0f);
            expectEquals (other.faderDb.load(), -5.0f);
            expectEquals (result.stripsSkippedSafe, 1);

            show.recallSnippet (0, 0);
            expectEquals (safe.faderDb.load(), -5.0f);
        }

        beginTest ("Snippets only touch their strips and sections");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumChannels (2);
            auto& a = engine.getChannels()[0]->params;
            auto& b = engine.getChannels()[1]->params;

            show.addScene ("Base");
            a.faderDb = -7.0f;
            a.eq.values[0] = 5.0f;
            b.faderDb = -7.0f;
            show.addSnippet (0, "Ch1 fader", state::Fader, { state::StripRef::of (*engine.getChannels()[0]) });

            a.faderDb = 0.0f;
            a.eq.values[0] = 0.0f;
            b.faderDb = 0.0f;
            show.recallSnippet (0, 0);

            expectEquals (a.faderDb.load(), -7.0f);
            expectEquals (a.eq.values[0].load(), 0.0f); // section not in snippet
            expectEquals (b.faderDb.load(), 0.0f);            // strip not in snippet
        }

        beginTest ("Auto-fire snippets apply after their scene");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumChannels (1);
            auto& p = engine.getChannels()[0]->params;

            p.faderDb = -12.0f;
            show.addScene ("Scene");
            p.faderDb = -3.0f;
            const int snip = show.addSnippet (0, "Override", state::Fader, { state::StripRef::of (*engine.getChannels()[0]) });
            show.setSnippetFireWithScene (0, snip, true);

            p.faderDb = 0.0f;
            show.recallScene (0);
            expectEquals (p.faderDb.load(), -3.0f);
        }

        beginTest ("Scene fades glide continuous values");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumChannels (1);
            auto& p = engine.getChannels()[0]->params;

            p.faderDb = -40.0f;
            p.mute = true;
            show.addScene ("Fade target");
            show.setSceneFadeSeconds (0, 5.0);

            p.faderDb = 0.0f;
            p.mute = false;
            show.recallScene (0);

            expect (show.isFading());
            expectEquals (p.faderDb.load(), 0.0f); // starts from the current value
            expect (p.mute.load());                // switches apply immediately

            show.cancelFade(); // jumps to the end
            expect (! show.isFading());
            expectEquals (p.faderDb.load(), -40.0f);
        }

        beginTest ("Scene recall restores FX state but never swaps plugins");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            auto& fx = engine.getFx();

            auto* plugin = new FakePlugin (0.5f);
            fx.setPlugin (0, std::unique_ptr<juce::AudioPluginInstance> (plugin));
            show.addScene ("Half");

            plugin->gain = 0.9f;
            show.recallScene (0);
            expectEquals (plugin->gain.load(), 0.5f);

            // A different plugin in the slot is left alone and reported.
            fx.setPlugin (0, std::make_unique<FakePlugin> (0.25f, FakePlugin::Mode::Gain, "other"));
            const auto result = show.recallScene (0);
            expectEquals (result.warnings.size(), 1);
            expect (fx.getPlugin (0) != nullptr);
            expectEquals (dynamic_cast<FakePlugin*> (fx.getPlugin (0))->gain.load(), 0.25f);
            engine.getGarbage().collectAllNow();
        }

        beginTest ("Show files are saved atomically and keep a backup");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumChannels (4);
            show.addScene ("One");
            show.addSnippet (0, "Snip", state::Fader | state::Sends,
                             { state::StripRef::of (*engine.getChannels()[2]) });
            show.setSceneNotes (0, "Opening");
            show.setSceneFadeSeconds (0, 1.5);

            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("LiveDAWTests");
            dir.deleteRecursively();
            dir.createDirectory();
            const auto file = dir.getChildFile ("test.lshow");

            expect (show.saveToFile (file).wasOk());
            engine.setNumChannels (6);
            expect (show.saveToFile (file).wasOk()); // second save makes a .bak
            const auto backup = file.withFileExtension (".lshow.bak");
            expect (backup.existsAsFile());

            MixerEngine e2 (0);
            Show loaded (e2, host);
            expect (loaded.loadFromFile (file).wasOk());
            expectEquals ((int) e2.getChannels().size(), 6);
            expectEquals (loaded.getNumScenes(), 1);
            expectEquals (loaded.getScene (0)->notes, juce::String ("Opening"));
            expectEquals (loaded.getScene (0)->fadeSeconds, 1.5);
            expectEquals (loaded.getScene (0)->numSnippets, 1);
            expectEquals ((int) loaded.getSnippet (0, 0)->strips.size(), 1);

            // A corrupted show falls back to the backup.
            file.replaceWithText ("<LiveDAWShow broken");
            MixerEngine e3 (0);
            Show recovered (e3, host);
            expect (recovered.loadFromFile (file).wasOk());
            expectEquals ((int) e3.getChannels().size(), 4);

            dir.deleteRecursively();
        }

        beginTest ("Show files written by earlier versions load in the WING layout");
        {
            PluginHost host (nullptr);

            // A real autosave written by the C++ session code (JUCE's XML writer).
            const auto legacy = juce::File (__FILE__).getSiblingFile ("data").getChildFile ("legacy-v2-juce.lshow");
            expect (legacy.existsAsFile());
            MixerEngine engine (0);
            Show show (engine, host);
            expect (show.loadFromFile (legacy).wasOk());
            expectEquals ((int) engine.getChannels().size(), 19);
            expectEquals ((int) engine.getBuses().size(), 16);
            expectEquals (engine.getBuses()[0]->getName(), juce::String ("Aux 1"));
            expect (! engine.getBuses()[0]->params.mains[0].on.load(), "an aux stays off the main");
            expectEquals (engine.getChannels()[0]->getName(), juce::String ("Ch 1"));
            expectEquals (engine.getMain().getColourIndex(), 9);

            // Scenes, snippets, plugins and awkward text, written by JUCE.
            juce::XmlElement root ("LiveDAWShow");
            root.setAttribute ("version", 2);
            root.setAttribute ("name", "Gig \"A\" & <B>\nline 2");
            root.setAttribute ("currentScene", 0);
            auto* console = root.createNewChildElement ("Console");
            auto* inputs = console->createNewChildElement ("Inputs");
            auto* strip = inputs->createNewChildElement ("Strip");
            strip->setAttribute ("kind", "input");
            strip->setAttribute ("id", 7);
            strip->setAttribute ("name", juce::String::fromUTF8 ("Gr\xc3\xbc\xc3\x9f Gott \xe2\x99\xab"));
            strip->setAttribute ("faderDb", -12.5);
            auto* insert = strip->createNewChildElement ("Inserts")->createNewChildElement ("Insert");
            insert->setAttribute ("slot", 0);
            juce::PluginDescription desc;
            desc.name = "Missing Reverb";
            desc.pluginFormatName = "VST3";
            desc.fileOrIdentifier = "/nowhere/reverb.vst3";
            insert->addChildElement (desc.createXml().release());
            insert->createNewChildElement ("State")->addTextElement ("AAAA");
            auto* scene = root.createNewChildElement ("Scenes")->createNewChildElement ("Scene");
            scene->setAttribute ("name", "Verse");
            scene->setAttribute ("fade", 2.5);
            auto* stateXml = scene->createNewChildElement ("State");
            auto* captured = stateXml->createNewChildElement ("Strip");
            captured->setAttribute ("ref", "input:7");
            captured->setAttribute ("sections", (int) state::Fader);
            captured->setAttribute ("faderDb", -3.0);
            auto* snippet = scene->createNewChildElement ("Snippets")->createNewChildElement ("Snippet");
            snippet->setAttribute ("name", "Solo");
            snippet->setAttribute ("strips", "input:7,master:0");
            snippet->setAttribute ("fireWithScene", 1);

            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("LiveDAWLegacy.lshow");
            expect (root.writeTo (file));
            MixerEngine e2 (0);
            Show loaded (e2, host);
            const auto result = loaded.loadFromFile (file);
            expect (result.failed()); // the plugin can't be created here...
            expect (result.getErrorMessage().contains ("Missing Reverb"));
            expectEquals ((int) e2.getChannels().size(), 1); // ...but everything else loads
            auto& ch = *e2.getChannels()[0];
            expectEquals (ch.params.preInsert.fx.load(), 0, "the plugin moved to FX slot 1");
            expectEquals (loaded.getName(), juce::String ("Gig \"A\" & <B>\nline 2"));
            expectEquals (ch.getName(), juce::String::fromUTF8 ("Gr\xc3\xbc\xc3\x9f Gott \xe2\x99\xab"));
            expectEquals (ch.params.faderDb.load(), -12.5f);
            expectEquals (loaded.getScene (0)->name, juce::String ("Verse"));
            expectEquals (loaded.getScene (0)->fadeSeconds, 2.5);
            const auto snip = loaded.getSnippet (0, 0);
            expectEquals ((int) snip->strips.size(), 2);
            expect (snip->strips[0] == state::StripRef { StripKind::Channel, 7 });
            expect (snip->strips[1] == state::StripRef { StripKind::Main, 0 });
            expect (snip->fireWithScene);
            loaded.setSceneFadeSeconds (0, 0.0);
            loaded.recallScene (0);
            expectEquals (ch.params.faderDb.load(), -3.0f);
            file.deleteFile();
        }

        beginTest ("Safe mode loads a show without instantiating plugins");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumChannels (1);
            engine.getFx().setPlugin (0, std::make_unique<FakePlugin>());
            auto xml = show.toXml();

            MixerEngine e2 (0);
            Show safe (e2, host);
            expect (safe.loadFromXml (*xml, false).wasOk());
            expect (! e2.getFx().isLoaded (0));

            // Normal mode reports the plugin it can't create (no "Fake" format) but loads the rest.
            MixerEngine e3 (0);
            Show normal (e3, host);
            expect (normal.loadFromXml (*xml, true).failed());
            expectEquals ((int) e3.getChannels().size(), 1);
            engine.getGarbage().collectAllNow();
        }
    }
};

static ShowTests showTests;
