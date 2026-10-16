#include "TestHelpers.h"
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
            engine.setNumInputs (3);
            auto* aux = engine.addBus (StripKind::Aux);
            engine.addBus (StripKind::Group);
            aux->setName ("Wedge 1");
            aux->params.outputLeft = 5;

            auto* kick = engine.getInputs()[1].get();
            auto& p = kick->params;
            kick->setName ("Kick");
            p.stereo = true;
            p.trimDb = 12.0f;
            p.hpfEnabled = true;
            p.gate.thresholdDb = -33.0f;
            p.eq.bands[2].gainDb = -4.5f;
            p.comp.ratio = 6.0f;
            p.recallSafe = true;
            kick->sends[(size_t) aux->getId()].enabled = true;
            kick->sends[(size_t) aux->getId()].levelDb = -3.0f;
            engine.monitorOutputLeft = 6;
            engine.outputCeilingDb = -1.0f;

            // Remove channel 1 so ids are no longer contiguous: they must survive.
            engine.removeStrip (engine.getInputs()[0].get());
            const int kickId = kick->getId();

            auto xml = show.toXml();

            MixerEngine restoredEngine (0);
            Show restored (restoredEngine, host);
            const auto result = restored.loadFromXml (*xml);
            expect (result.wasOk(), result.getErrorMessage());
            expectEquals ((int) restoredEngine.getInputs().size(), 2);
            expectEquals ((int) restoredEngine.getBuses().size(), 2);

            auto* r = state::findStrip (restoredEngine, { StripKind::Input, kickId });
            expect (r != nullptr);
            if (r == nullptr)
                return;

            expectEquals (r->getName(), juce::String ("Kick"));
            expect (r->params.stereo.load());
            expectEquals (r->params.trimDb.load(), 12.0f);
            expect (r->params.hpfEnabled.load());
            expectEquals (r->params.gate.thresholdDb.load(), -33.0f);
            expectEquals (r->params.eq.bands[2].gainDb.load(), -4.5f);
            expectEquals (r->params.comp.ratio.load(), 6.0f);
            expect (r->params.recallSafe.load());
            expect (r->sends[(size_t) aux->getId()].enabled.load());
            expectEquals (r->sends[(size_t) aux->getId()].levelDb.load(), -3.0f);
            expectEquals (restoredEngine.monitorOutputLeft.load(), 6);
            expectEquals (restoredEngine.outputCeilingDb.load(), -1.0f);
            expectEquals (restoredEngine.getBusInSlot (aux->getId())->getName(), juce::String ("Wedge 1"));

            // New channels must not reuse a restored id.
            expect (restoredEngine.addInput()->getId() > kickId);
        }

        beginTest ("Scenes store and recall the console");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumInputs (2);
            auto& ch1 = engine.getInputs()[0]->params;

            ch1.faderDb = -10.0f;
            ch1.eq.bands[1].gainDb = 4.0f;
            expectEquals (show.addScene ("Verse"), 0);

            ch1.faderDb = 3.0f;
            ch1.eq.bands[1].gainDb = -2.0f;
            ch1.mute = true;
            expectEquals (show.addScene ("Chorus"), 1);

            show.recallScene (0);
            expectEquals (ch1.faderDb.load(), -10.0f);
            expectEquals (ch1.eq.bands[1].gainDb.load(), 4.0f);
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
            engine.setNumInputs (2);
            for (int i = 0; i < 80; ++i)
                engine.addBus (StripKind::Aux);

            auto& send = engine.getInputs()[1]->sends[(size_t) 79];
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
            auto* restored = e2.getInputs()[1]->sends.find (79);
            expect (restored != nullptr && restored->enabled.load());
        }

        beginTest ("Recall scope limits what a scene changes");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumInputs (1);
            auto& p = engine.getInputs()[0]->params;

            p.faderDb = -20.0f;
            p.comp.thresholdDb = -30.0f;
            p.inputLeft = 0;
            show.addScene ("A");
            show.setSceneScope (0, state::Fader); // only faders

            p.faderDb = 0.0f;
            p.comp.thresholdDb = -10.0f;
            p.inputLeft = 5;
            show.recallScene (0);

            expectEquals (p.faderDb.load(), -20.0f);
            expectEquals (p.comp.thresholdDb.load(), -10.0f); // out of scope
            expectEquals (p.inputLeft.load(), 5);              // routing is never in the default scope
        }

        beginTest ("Recall safe channels are skipped by scenes but not by snippets");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumInputs (2);
            auto& safe = engine.getInputs()[0]->params;
            auto& other = engine.getInputs()[1]->params;

            safe.faderDb = -5.0f;
            other.faderDb = -5.0f;
            show.addScene ("A");
            show.addSnippet (0, "Safe fader", state::Fader, { state::StripRef::of (*engine.getInputs()[0]) });

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
            engine.setNumInputs (2);
            auto& a = engine.getInputs()[0]->params;
            auto& b = engine.getInputs()[1]->params;

            show.addScene ("Base");
            a.faderDb = -7.0f;
            a.eq.bands[0].gainDb = 5.0f;
            b.faderDb = -7.0f;
            show.addSnippet (0, "Ch1 fader", state::Fader, { state::StripRef::of (*engine.getInputs()[0]) });

            a.faderDb = 0.0f;
            a.eq.bands[0].gainDb = 0.0f;
            b.faderDb = 0.0f;
            show.recallSnippet (0, 0);

            expectEquals (a.faderDb.load(), -7.0f);
            expectEquals (a.eq.bands[0].gainDb.load(), 0.0f); // section not in snippet
            expectEquals (b.faderDb.load(), 0.0f);            // strip not in snippet
        }

        beginTest ("Auto-fire snippets apply after their scene");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumInputs (1);
            auto& p = engine.getInputs()[0]->params;

            p.faderDb = -12.0f;
            show.addScene ("Scene");
            p.faderDb = -3.0f;
            const int snip = show.addSnippet (0, "Override", state::Fader, { state::StripRef::of (*engine.getInputs()[0]) });
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
            engine.setNumInputs (1);
            auto& p = engine.getInputs()[0]->params;

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

        beginTest ("Scene recall restores plugin state but never swaps plugins");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumInputs (1);
            auto& strip = *engine.getInputs()[0];

            auto* plugin = new FakePlugin (0.5f);
            strip.inserts.setPlugin (0, std::unique_ptr<juce::AudioPluginInstance> (plugin));
            show.addScene ("Half");

            plugin->gain = 0.9f;
            show.recallScene (0);
            expectEquals (plugin->gain.load(), 0.5f);

            // A different plugin in the slot is left alone and reported.
            strip.inserts.setPlugin (0, std::make_unique<FakePlugin> (0.25f, FakePlugin::Mode::Gain, "other"));
            const auto result = show.recallScene (0);
            expectEquals (result.warnings.size(), 1);
            expect (strip.inserts.getPlugin (0) != nullptr);
            expectEquals (dynamic_cast<FakePlugin*> (strip.inserts.getPlugin (0))->gain.load(), 0.25f);
            engine.getGarbage().collectAllNow();
        }

        beginTest ("Show files are saved atomically and keep a backup");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumInputs (4);
            show.addScene ("One");
            show.addSnippet (0, "Snip", state::Fader | state::Sends,
                             { state::StripRef::of (*engine.getInputs()[2]) });
            show.setSceneNotes (0, "Opening");
            show.setSceneFadeSeconds (0, 1.5);

            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("LiveDAWTests");
            dir.deleteRecursively();
            dir.createDirectory();
            const auto file = dir.getChildFile ("test.lshow");

            expect (show.saveToFile (file).wasOk());
            engine.setNumInputs (6);
            expect (show.saveToFile (file).wasOk()); // second save makes a .bak
            const auto backup = file.withFileExtension (".lshow.bak");
            expect (backup.existsAsFile());

            MixerEngine e2 (0);
            Show loaded (e2, host);
            expect (loaded.loadFromFile (file).wasOk());
            expectEquals ((int) e2.getInputs().size(), 6);
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
            expectEquals ((int) e3.getInputs().size(), 4);

            dir.deleteRecursively();
        }

        beginTest ("Show files written by earlier (C++) versions load");
        {
            PluginHost host (nullptr);

            // A real autosave written by the C++ session code (JUCE's XML writer).
            const auto legacy = juce::File (__FILE__).getSiblingFile ("data").getChildFile ("legacy-v2-juce.lshow");
            expect (legacy.existsAsFile());
            MixerEngine engine (0);
            Show show (engine, host);
            expect (show.loadFromFile (legacy).wasOk());
            expectEquals ((int) engine.getInputs().size(), 19);
            expectEquals ((int) engine.getBuses().size(), 1);
            expectEquals (engine.getBuses()[0]->getName(), juce::String ("Aux 1"));
            expectEquals (engine.getInputs()[0]->getName(), juce::String ("Ch 1"));
            expect (engine.getMaster().getColour().getARGB() == 0xffb83d4fu);

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
            expectEquals ((int) e2.getInputs().size(), 1); // ...but everything else loads
            expectEquals (loaded.getName(), juce::String ("Gig \"A\" & <B>\nline 2"));
            expectEquals (e2.getInputs()[0]->getName(), juce::String::fromUTF8 ("Gr\xc3\xbc\xc3\x9f Gott \xe2\x99\xab"));
            expectEquals (e2.getInputs()[0]->params.faderDb.load(), -12.5f);
            expectEquals (loaded.getScene (0)->name, juce::String ("Verse"));
            expectEquals (loaded.getScene (0)->fadeSeconds, 2.5);
            expectEquals ((int) loaded.getSnippet (0, 0)->strips.size(), 2);
            expect (loaded.getSnippet (0, 0)->fireWithScene);
            loaded.setSceneFadeSeconds (0, 0.0);
            loaded.recallScene (0);
            expectEquals (e2.getInputs()[0]->params.faderDb.load(), -3.0f);
            file.deleteFile();
        }

        beginTest ("Safe mode loads a show without instantiating plugins");
        {
            MixerEngine engine (0);
            PluginHost host (nullptr);
            Show show (engine, host);
            engine.setNumInputs (1);
            engine.getInputs()[0]->inserts.setPlugin (0, std::make_unique<FakePlugin>());
            auto xml = show.toXml();

            MixerEngine e2 (0);
            Show safe (e2, host);
            expect (safe.loadFromXml (*xml, false).wasOk());
            expect (e2.getInputs()[0]->inserts.isEmpty());

            // Normal mode reports the plugin it can't create (no "Fake" format) but loads the rest.
            MixerEngine e3 (0);
            Show normal (e3, host);
            expect (normal.loadFromXml (*xml, true).failed());
            expectEquals ((int) e3.getInputs().size(), 1);
            engine.getGarbage().collectAllNow();
        }
    }
};

static ShowTests showTests;
