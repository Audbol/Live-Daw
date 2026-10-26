#include "engine/Models.h"
#include "ui/Faceplate.h"
#include "ui/FaceplateDesign.h"
#include "ui/LookAndFeel.h"
#include "ui/ModelParams.h"

#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <memory>
#include <set>
#include <vector>

using namespace livedaw;

namespace
{
/** Models whose page is the console's own graph screen, not a front panel
    (the WING gate, ducker, compressor and expander; the WING EQ). */
bool nativePage (models::Table table, const juce::String& token)
{
    if (table == models::Dynamics)
        return token == "GATE" || token == "DUCK" || token == "COMP" || token == "EXP";
    return (table == models::ChannelEq || table == models::BusEq) && token == "STD";
}

/** Whether an element lets the user change its parameter. */
bool isControl (const ui::fp::Element& e)
{
    using T = ui::fp::Type;
    return e.type == T::Knob || e.type == T::Toggle || e.type == T::Key || e.type == T::Lamp || e.type == T::Fader || e.type == T::Value;
}
} // namespace

/** The drawn front panels of the processing models (FX rack devices, the
    dynamics and EQ models): every model has one, and every parameter has a
    control on it that moves with it. With SIDECAR_FACEPLATE_SNAPSHOT_DIR set,
    each panel page is saved as a 1280 x 800 PNG (the console's screen) for
    review, at the defaults and (*_moved) with every control at 30 % of its
    travel. */
class FaceplateTests final : public juce::UnitTest
{
public:
    FaceplateTests() : juce::UnitTest ("Faceplates", "LiveDAW") {}

    void runTest() override
    {
        const auto snapshots = juce::SystemStats::getEnvironmentVariable ("SIDECAR_FACEPLATE_SNAPSHOT_DIR", {});
        std::unique_ptr<ui::ConsoleLookAndFeel> look;
        if (snapshots.isNotEmpty())
        {
            look = std::make_unique<ui::ConsoleLookAndFeel>();
            juce::LookAndFeel::setDefaultLookAndFeel (look.get());
            juce::File (snapshots).createDirectory();
        }

        struct Entry
        {
            models::Table table;
            const char* prefix;
        };
        for (auto [table, prefix] : { Entry { models::Fx, "fx" }, Entry { models::Dynamics, "dyn" }, Entry { models::ChannelEq, "eq" },
                                      Entry { models::BusEq, "buseq" } })
        {
            beginTest (juce::String ("Every model of table ") + prefix + " has a front panel with all its parameters");
            const auto& all = models::table (table);
            for (const auto& m : all)
            {
                if (table == models::Fx && m.index <= ld::kFxExt)
                    continue;
                if (nativePage (table, m.token))
                    continue;
                if (snapshots.isNotEmpty())
                {
                    snapshot (juce::File (snapshots), prefix, table, m, false);
                    snapshot (juce::File (snapshots), prefix, table, m, true);
                }
                const auto* design = ui::fp::findDesign (table, m.token);
                expect (design != nullptr, m.token + " has no drawn panel");
                if (design == nullptr)
                    continue;

                // Every parameter is reachable; bound elements point at real parameters.
                std::set<int> reached;
                for (const auto& e : design->elements)
                {
                    expect (e.param < (int) m.params.size(), m.token + ": an element is bound to parameter " + juce::String (e.param));
                    if (isControl (e) && e.param >= 0 && e.param < (int) m.params.size())
                    {
                        reached.insert (e.param);
                        // A knob or fader turns as its value changes; its text follows.
                        const auto& p = m.params[(size_t) e.param];
                        if ((e.type == ui::fp::Type::Knob || e.type == ui::fp::Type::Fader) && p.min != p.max)
                        {
                            const float lo = p.fromNormalised (0.1f), hi = p.fromNormalised (0.9f);
                            expect (ui::fp::controlPosition (p, lo) < ui::fp::controlPosition (p, hi), m.token + ": " + p.label + " doesn't move");
                            expect (ui::consoleText (p, lo) != ui::consoleText (p, hi), m.token + ": " + p.label + " shows the same text");
                        }
                    }
                    for (int p : e.params)
                        expect (p >= -1 && p < (int) m.params.size(), m.token + ": a display or graph shows parameter " + juce::String (p)); // -1: not shown
                }
                for (int i = 0; i < (int) m.params.size(); ++i)
                    if (m.params[(size_t) i].min != m.params[(size_t) i].max) // a fixed value (FX PULSAR MIX) needs no control
                        expect (reached.count (i) == 1, m.token + ": no control for " + m.params[(size_t) i].label);
            }
        }

        beginTest ("A RATIO from a list reads as on the console (\"2.0:1\")");
        {
            const auto ratioText = [] (const juce::String& token, int index)
            {
                for (auto t : { models::Dynamics, models::Fx })
                    if (const int m = models::findModel (t, token); m >= 0)
                    {
                        const auto& model = models::model (t, m);
                        return ui::consoleText (model.params[(size_t) model.find ("ratio")], (float) index);
                    }
                return juce::String();
            };
            expectEquals (ratioText ("COMP", 5), juce::String ("2.0:1"));
            expectEquals (ratioText ("COMP", 16), juce::String ("100.0:1"));
            expectEquals (ratioText ("EXP", 5), juce::String ("1:2.0"));
            expectEquals (ratioText ("9000C", 1), juce::String ("1.43:1"));
            expectEquals (ratioText ("DEQ", 0), juce::String ("1.2:1"));
        }

        beginTest ("GRAPHIC EQ: the faders (with TRIM) and the curve are on pages of their own");
        {
            const auto* geq = ui::fp::findDesign (models::Fx, "GEQ");
            expect (geq != nullptr);
            if (geq != nullptr)
            {
                const auto& spec = models::model (models::Fx, models::findModel (models::Fx, "GEQ"));
                std::set<int> faderPages, graphPages, typePages;
                int faders = 0;
                for (const auto& e : geq->elements)
                {
                    if (e.type == ui::fp::Type::Fader && e.param >= 0)
                    {
                        ++faders;
                        faderPages.insert (e.page);
                    }
                    if (e.type == ui::fp::Type::Graph && e.graph == ui::fp::GraphKind::Response)
                        graphPages.insert (e.page);
                    if (isControl (e) && e.param == spec.find ("type"))
                        typePages.insert (e.page);
                }
                expectEquals (faders, 32); // 31 bands and TRIM
                expect (faderPages == std::set<int> { 0 }, "the faders aren't all on the first page");
                expect (graphPages == std::set<int> { 1 }, "the curve isn't on a page of its own");
                expect (typePages == std::set<int> { 1 });
                ui::Faceplate face;
                std::vector<std::atomic<float>> values (spec.params.size());
                for (size_t i = 0; i < spec.params.size(); ++i)
                    values[i] = spec.params[i].defaultValue;
                face.setBounds (0, 0, 578, 240); // small: no large screen needed
                face.bind (models::Fx, spec.index, values.data(), [] { return 0.0f; });
                expectEquals (face.getNumPages(), 2);
                face.setPage (1);
                expectEquals (face.getPage(), 1);
            }
        }

        beginTest ("GATED, REVERSE and DEL/REV have panels of their own, with their graphs");
        {
            const std::pair<const char*, ui::fp::GraphKind> reverbs[] = { { "GATED", ui::fp::GraphKind::Decay },
                                                                         { "REVERSE", ui::fp::GraphKind::Decay },
                                                                         { "DEL/REV", ui::fp::GraphKind::Taps } };
            const auto* hall = ui::fp::findDesign (models::Fx, "HALL");
            for (auto [token, graph] : reverbs)
            {
                const auto* d = ui::fp::findDesign (models::Fx, token);
                expect (d != nullptr, token);
                if (d == nullptr)
                    continue;
                bool hasGraph = false, display = false;
                for (const auto& e : d->elements)
                {
                    hasGraph = hasGraph || (e.type == ui::fp::Type::Graph && e.graph == graph && ! e.params.empty());
                    display = display || e.type == ui::fp::Type::Display; // the HALL remote's dot-matrix display
                }
                expect (hasGraph, juce::String (token) + " lost its graph");
                expect (! display, juce::String (token) + " still uses the HALL's remote");
                expect (hall == nullptr || d->panel != hall->panel, token);
            }
        }
    }

private:
    void snapshot (const juce::File& dir, const char* prefix, models::Table table, const models::Model& m, bool moved)
    {
        std::vector<std::atomic<float>> values (juce::jmax ((size_t) 1, m.params.size()));
        for (size_t i = 0; i < m.params.size(); ++i)
            values[i] = moved ? m.params[i].fromNormalised (0.3f) : m.params[i].defaultValue;

        ui::Faceplate face;
        // The FX editor's body on the 1280 x 800 screen: under the header row.
        face.setBounds (0, 0, 1280, 640);
        face.bind (table, m.index, values.data(), [] { return -6.0f; });
        for (int page = 0; page < face.getNumPages(); ++page)
        {
            face.setPage (page);
            juce::Image image (juce::Image::RGB, 1280, 800, true);
            {
                juce::Graphics g (image);
                g.fillAll (juce::Colour (0xff101010));
                g.setColour (juce::Colour (0xff282828));
                g.fillRect (0, 0, 1280, 140);
                g.setColour (juce::Colours::white);
                g.setFont (juce::FontOptions (40.0f, juce::Font::bold));
                juce::String title = m.name.toUpperCase() + "   (" + m.token + ")";
                if (face.getNumPages() > 1)
                    title << "   PAGE " << (page + 1) << "/" << face.getNumPages();
                g.drawText (title, 24, 0, 1232, 140, juce::Justification::centredLeft);
                g.drawImageAt (face.createComponentSnapshot (face.getLocalBounds(), true, 1.0f), 0, 150);
            }
            const auto fileName = juce::String (prefix) + "_" + juce::String (m.index).paddedLeft ('0', 2) + "_"
                              + m.token.retainCharacters ("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") + (page > 0 ? "_p" + juce::String (page + 1) : juce::String())
                              + (moved ? "_moved" : "") + ".png";
            auto file = dir.getChildFile (fileName);
            file.deleteFile();
            juce::FileOutputStream out (file);
            juce::PNGImageFormat().writeImageToStream (image, out);
        }
    }
};

static FaceplateTests faceplateTests;
