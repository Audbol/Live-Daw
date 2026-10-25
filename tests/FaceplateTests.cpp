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
