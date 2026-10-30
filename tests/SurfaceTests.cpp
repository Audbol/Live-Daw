#include "engine/Layers.h"
#include "ui/LookAndFeel.h"
#include "ui/Scribble.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>
#include <tuple>

using namespace livedaw;

namespace
{
/** A scribble LCD drawn into an image of its own size. */
juce::Image drawLcd (const ui::ScribbleInfo& info, int w = 72, int h = 52)
{
    juce::Image image (juce::Image::ARGB, w, h, true);
    juce::Graphics g (image);
    ui::drawScribble (g, juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h), info);
    return image;
}

/** How many pixels differ from the top-left one (0: one flat colour, no text). */
int inkedPixels (const juce::Image& image)
{
    const auto bg = image.getPixelAt (0, 0);
    int n = 0;
    for (int y = 0; y < image.getHeight(); ++y)
        for (int x = 0; x < image.getWidth(); ++x)
            n += image.getPixelAt (x, y) != bg ? 1 : 0;
    return n;
}

/** The rows from the first to the last with any ink (an empty range if none). */
juce::Range<int> inkedRows (const juce::Image& image)
{
    const auto bg = image.getPixelAt (0, 0);
    int top = -1, bottom = -1;
    for (int y = 0; y < image.getHeight(); ++y)
        for (int x = 0; x < image.getWidth(); ++x)
            if (image.getPixelAt (x, y) != bg)
            {
                top = top < 0 ? y : top;
                bottom = y;
                break;
            }
    return { juce::jmax (0, top), bottom + 1 };
}

/** What a strip shows, as FaderStrip decides it: a slot with no target is blank. */
ui::ScribbleInfo shownFor (const std::optional<state::FaderRef>& target, const ui::ScribbleInfo& lit)
{
    return target.has_value() ? lit : ui::blankScribble();
}
} // namespace

/** The surface's scribble strip LCDs: a blank layer slot, a strip past the end
    of a layer (or one whose target is gone) shows nothing at all, as on the
    console; a strip with a target shows its text. */
class SurfaceTests final : public juce::UnitTest
{
public:
    SurfaceTests() : juce::UnitTest ("Surface", "LiveDAW") {}

    void runTest() override
    {
        ui::ConsoleLookAndFeel look; // the screen fonts

        // What a channel without a source shows: "5" | "OFF", its name, a key's state.
        ui::ScribbleInfo channel;
        channel.number = "5";
        channel.tag = "OFF";
        channel.name = "VOX";
        channel.footer = "OFF";
        channel.backlight = 0.6f;
        channel.contrast = 0.5f;

        beginTest ("A strip with a target shows its text");
        expect (inkedPixels (drawLcd (channel)) > 20);
        {
            auto value = channel;
            value.value = "-3.5";
            value.caption = "TRIM";
            expect (inkedPixels (drawLcd (value)) > 20);
        }

        beginTest ("A blank strip shows no text: the LCD is one unlit colour");
        {
            const auto blank = ui::blankScribble();
            expect (blank.isEmpty());
            expectEquals (blank.backlight, 0.0f);
            expectEquals (inkedPixels (drawLcd (blank)), 0);
            // Unlit, nothing shows, whatever was filled in (no "OFF", no value).
            auto unlit = channel;
            unlit.value = "-oo";
            unlit.line2 = "OFF";
            unlit.backlight = 0.0f;
            expectEquals (inkedPixels (drawLcd (unlit)), 0);
            // A lit LCD with nothing on it draws no ink either.
            ui::ScribbleInfo empty;
            expectEquals (inkedPixels (drawLcd (empty)), 0);
            // Unlit is darker than lit.
            const auto unlitBg = drawLcd (blank).getPixelAt (0, 0), litBg = drawLcd (empty).getPixelAt (0, 0);
            expect (unlitBg.getBrightness() < litBg.getBrightness());
            // Small LCDs (a short window) as well.
            expectEquals (inkedPixels (drawLcd (unlit, 24, 18)), 0);
        }

        beginTest ("Blank slots and strips past the end of a user layer are blank on the surface");
        {
            Layer layer;
            layer.user = true;
            layer.faders = { state::FaderRef::blank(), state::FaderRef::blank(), state::FaderRef::blank() };
            for (int i = -1; i < 6; ++i) // blank slots, then past the end
            {
                const auto target = LayerSet::surfaceTarget (layer, i);
                expect (! target.has_value());
                expectEquals (inkedPixels (drawLcd (shownFor (target, channel))), 0);
            }
            state::FaderRef dca;
            dca.dca = true;
            dca.dcaIndex = 2;
            layer.faders[1] = dca;
            const auto target = LayerSet::surfaceTarget (layer, 1);
            expect (target.has_value());
            expect (inkedPixels (drawLcd (shownFor (target, channel))) > 20);
        }

        beginTest ("Letters with descenders (g, j, p, q, y) are raised onto the baseline and show whole, as on the WING");
        ui::ScribbleInfo named;
        named.name = "HHH";
        const int capHeight = inkedRows (drawLcd (named, 144, 104)).getLength();
        for (const auto& [caps, round, descending] : { std::tuple<const char*, const char*, const char*> { "HHH", "ooo", "ggg" },
                                                       { "HHHHH HHHHH", "ooooo ooooo", "ggggg ggggg" } })
        {
            named.name = caps;
            const auto capRows = inkedRows (drawLcd (named, 144, 104));
            named.name = round;
            const auto oRows = inkedRows (drawLcd (named, 144, 104));
            named.name = descending;
            const auto gRows = inkedRows (drawLcd (named, 144, 104));
            // On the baseline, like the capitals beside them...
            expectLessOrEqual (gRows.getEnd(), capRows.getEnd() + 1, descending);
            // ...with the whole tail: a "g" is about a third of a capital taller than an "o".
            expectGreaterOrEqual (gRows.getLength() - oRows.getLength(), capHeight / 5, descending);
        }
    }
};

static SurfaceTests surfaceTests;
