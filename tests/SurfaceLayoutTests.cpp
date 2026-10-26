#include "ui/SurfaceLayout.h"

#include <juce_core/juce_core.h>

#include <vector>

using namespace livedaw::ui::layout;

class SurfaceLayoutTests final : public juce::UnitTest
{
public:
    SurfaceLayoutTests() : juce::UnitTest ("Surface layout", "LiveDAW") {}

    using Rect = juce::Rectangle<int>;

    /** Every part is inside `area`, not empty, and overlaps no other part. */
    void expectTiled (const std::vector<std::pair<juce::String, Rect>>& parts, Rect area, const juce::String& where)
    {
        for (size_t i = 0; i < parts.size(); ++i)
        {
            const auto& [part, r] = parts[i];
            expect (! r.isEmpty(), where + ": " + part + " is empty");
            expect (area.contains (r), where + ": " + part + " " + r.toString() + " is outside " + area.toString());
            for (size_t j = i + 1; j < parts.size(); ++j)
                expect (! r.intersects (parts[j].second), where + ": " + part + " " + r.toString() + " overlaps " + parts[j].first + " " + parts[j].second.toString());
        }
    }

    /** The main component's top row at the splitter's `fraction` (MainComponent::topRowHeight). */
    static int topRowHeight (Rect window, float fraction)
    {
        const int h = window.getHeight();
        const float aspect = (float) window.getWidth() / (float) juce::jmax (1, h);
        const float wideBoost = juce::jlimit (0.0f, 0.08f, (aspect - 2.0f) * 0.06f);
        return juce::jlimit (300, juce::jmax (300, h - 260), juce::roundToInt ((float) h * (fraction + wideBoost)));
    }

    void checkCompact (Rect window, float fraction, int displayWidth)
    {
        const auto where = juce::String (window.getWidth()) + "x" + juce::String (window.getHeight()) + " at " + juce::String (fraction, 2)
                           + ", display " + juce::String (displayWidth);
        const int topH = topRowHeight (window, fraction);
        const auto s = compact::plan (window, topH, displayWidth);
        expectTiled ({ { "display", s.display }, { "panel", s.panel }, { "splitter", s.splitter }, { "faders", s.faders }, { "keys", s.keys } },
                     window, where);
        expect (s.stripW >= 40, where);
        expectEquals (s.faders.getWidth(), kBankColumnWidth + compact::kFaders * s.stripW, where);
        expectEquals (s.keys.getWidth(), compact::keySectionWidth (s.stripW), where);
        expect (s.faders.getY() >= s.splitter.getBottom() && s.display.getBottom() <= s.splitter.getY(), where);
        expect (s.panel.getWidth() >= 200, where + ": the monitoring panel is too narrow");

        // The key section: mode keys and VIEW level with the scribble strips,
        // the LCD and 16 keys under them, fader 13 at the right.
        const auto k = compact::planKeys (s.keys.withZeroOrigin(), s.stripW);
        std::vector<std::pair<juce::String, Rect>> parts { { "VIEW", k.view }, { "LCD", k.lcd }, { "fader 13", k.strip } };
        const char* modeNames[] = { "BUS", "DCA", "MAIN MATRIX", "USER" };
        for (size_t i = 0; i < k.modes.size(); ++i)
            parts.push_back ({ modeNames[i], k.modes[i] });
        for (size_t i = 0; i < k.keys.size(); ++i)
            parts.push_back ({ "key " + juce::String ((int) i + 1), k.keys[i] });
        expectTiled (parts, s.keys.withZeroOrigin(), where + ", key section");
        // Fader 13 starts 3 px above its SELECT key, level with the strips' SELECT row.
        expectEquals (k.strip.getY() + 3, stripTopRowsHeight (s.stripW), where);
        expect (k.lcd.getY() >= stripTopRowsHeight (s.stripW) && k.keys[0].getY() > k.lcd.getBottom(), where);
        for (auto& m : k.modes)
            expect (m.getBottom() <= stripTopRowsHeight (s.stripW), where + ": a mode key below the scribble band");
        // Two columns of eight, 1 2 / 3 4 / ...
        expect (k.keys[0].getX() < k.keys[1].getX() && k.keys[0].getY() == k.keys[1].getY() && k.keys[2].getY() > k.keys[0].getY(), where);
    }

    void runTest() override
    {
        beginTest ("The WING COMPACT's surface fits 1366 x 768 without overlaps");
        {
            const Rect laptop (0, 0, 1366, 768);
            for (float fraction : { 0.52f, 0.3f, 0.7f })
                for (int displayWidth : { 700, 900, 1100, 1400 })
                    checkCompact (laptop, fraction, displayWidth);

            // At the default split the 16 keys are a usable size.
            const int topH = topRowHeight (laptop, 0.52f);
            const auto s = compact::plan (laptop, topH, 900);
            const auto k = compact::planKeys (s.keys.withZeroOrigin(), s.stripW);
            expect (k.keys[0].getHeight() >= 14 && k.keys[0].getWidth() >= 30, k.keys[0].toString());
            expect (k.lcd.getHeight() >= 40, k.lcd.toString());
            expectEquals (s.stripW, 70);
        }

        beginTest ("The WING COMPACT's surface on other windows");
        {
            for (auto window : { Rect (0, 0, 1280, 720), Rect (0, 0, 1920, 1080), Rect (0, 0, 2560, 1080), Rect (0, 0, 3840, 2160), Rect (0, 0, 1024, 768) })
                checkCompact (window, 0.52f, juce::jmin (window.getWidth() - 220, window.getHeight() * 9 / 10));

            // Very wide windows keep the parts at their width, centred.
            const Rect wide (0, 0, 3840, 1080);
            const auto s = compact::plan (wide, topRowHeight (wide, 0.52f), 1200);
            expect (s.display.getX() > 0 && s.faders.getX() > 0, s.faders.toString());
            expect (s.keys.getRight() < wide.getRight());
        }

        beginTest ("Fader 13's assignment and the 16 keys' mode");
        {
            using MF = compact::MainFader;
            expect (compact::parseMainFader ("MAIN.1") == MF { MF::Main, 1 });
            expect (compact::parseMainFader ("dca.16") == MF { MF::Dca, 16 });
            expect (compact::parseMainFader ("CH.40") == MF { MF::Channel, 40 });
            expect (compact::parseMainFader ("OFF") == MF { MF::Off, 1 });
            // Anything else: MAIN 1.
            expect (compact::parseMainFader ("") == MF {});
            expect (compact::parseMainFader ("MTX.9") == MF {});
            expect (compact::parseMainFader ("BUS.0") == MF {});
            expectEquals (compact::mainFaderToken ({ MF::Matrix, 3 }), juce::String ("MTX.3"));
            expectEquals (compact::mainFaderName ({ MF::Aux, 2 }), juce::String ("AUX 2"));
            expectEquals (compact::mainFaderToken ({ MF::Off, 7 }), juce::String ("OFF"));
            for (int g = MF::Off; g <= MF::Bus; ++g)
                for (int n = 1; n <= compact::groupSize (static_cast<MF::Group> (g)); ++n)
                {
                    const MF f { static_cast<MF::Group> (g), n };
                    expect (compact::parseMainFader (compact::mainFaderToken (f)) == f, compact::mainFaderToken (f));
                }

            for (auto m : { compact::KeyMode::Bus, compact::KeyMode::Dca, compact::KeyMode::Main, compact::KeyMode::User })
                expect (compact::keyModeFromToken (compact::keyModeToken (m)) == m);
            expect (compact::keyModeFromToken ("") == compact::KeyMode::Bus);

            // What a key puts on fader 13: BUS 1-16; MAIN 1-4 on keys 1-4, MATRIX 1-8 on 9-16.
            using KM = compact::KeyMode;
            expect (compact::keyMainFader (KM::Bus, 0) == MF { MF::Bus, 1 });
            expect (compact::keyMainFader (KM::Bus, 15) == MF { MF::Bus, 16 });
            expect (compact::keyMainFader (KM::Main, 3) == MF { MF::Main, 4 });
            expect (compact::keyMainFader (KM::Main, 8) == MF { MF::Matrix, 1 });
            expect (compact::keyMainFader (KM::Main, 15) == MF { MF::Matrix, 8 });
            for (int key = 4; key < 8; ++key)
                expect (! compact::keyMainFader (KM::Main, key).has_value(), "MAIN MATRIX key " + juce::String (key + 1));
            for (int key = 0; key < 16; ++key)
                expect (! compact::keyMainFader (KM::Dca, key) && ! compact::keyMainFader (KM::User, key));
            expect (! compact::keyMainFader (KM::Bus, 16).has_value());
        }
    }
};

static SurfaceLayoutTests surfaceLayoutTests;
