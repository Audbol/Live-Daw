#include "ui/FaceplateDesign.h"

#include <map>

/* The front panels as the console's screen draws them, measured in the
   screen's faceplate frame (1156 x 479 px; the unit's plate is 1096 x 336 at
   30, 70 unless a design says otherwise). Parameter indices follow the model
   tables (models_gen.rs). */

namespace livedaw::ui::fp
{

namespace
{
    using J = juce::Justification;

    constexpr juce::uint32 kWhite = 0xffeeeeee, kInk = 0xff1c1c1c;

    //==========================================================================
    // Dynamics models

    Design levelingAmp()
    {
        Design d;
        d.panel = juce::Colour (0xffc9c9c6);
        d.wear = 0.9f;
        const juce::uint32 red = 0xffb8432c;
        d.text ("LEVELING AMPLIFIER", 78, 116, 240, 22, 19, red, Look::Sans, J::centredLeft);
        d.path ("M 80 151 L 237 151 L 273 105 L 414 105", red, 2.2f);
        d.path ("M 80 161 L 245 161 L 281 113 L 414 113", red, 2.2f);
        // GAIN and PEAK REDUCTION: fluted black knobs over fine scales.
        for (auto [param, cx, cap] : { std::tuple { 0, 327.0f, "GAIN" }, { 1, 839.0f, "PEAK REDUCTION" } })
        {
            d.knob (param, cx, 270, 122, Look::BlackFluted)
                .scaleOf (51, "0|10|20|30|40|50|60|70|80|90|100", 250, false)
                .radii (1.05f, 1.42f)
                .lsize (15)
                .scol (0xff1e1e1e)
                .turned();
            d.caption (cap, cx, 358, 15, kInk);
        }
        // LIMIT / COMPRESS (MODE: up = LIM).
        d.caption ("LIMIT", 125, 230, 14, kInk);
        d.toggle (2, 125, 270, 26);
        d.caption ("COMPRESS", 127, 294, 14, kInk);
        d.caption ("ON", 998, 230, 14, kInk);
        d.toggle (-1, 998, 270, 26).at (1.0f);
        d.caption ("POWER", 998, 294, 14, kInk);
        // The meter in a chrome bezel.
        d.vu (471, 160, 226, 136, Look::VuAmber, "-20|-10|-7|-5|-3|-2|-1|0", { 20, 10, 7, 5, 3, 2, 1, 0 }, "DECIBELS");
        return d;
    }

    Design limitingAmp76()
    {
        Design d;
        d.panel = juce::Colour (0xff1e1e1f);
        d.texture = Texture::Brushed;
        d.wear = 1.0f;
        for (auto [param, cx, cap] : { std::tuple { 0, 175.0f, "INPUT" }, { 1, 397.0f, "OUTPUT" } })
        {
            d.knob (param, cx, 235, 112, Look::ChromeCollar)
                .scaleOf (17, "-48||-42||-36||-30||-24||-18||-12||-6||0", 310)
                .radii (1.16f, 1.34f)
                .lsize (13)
                .skip();
            d.caption (cap, cx, 324, 16, kWhite);
        }
        for (auto [param, cy, cap] : { std::tuple { 2, 173.0f, "ATTACK" }, { 3, 291.0f, "RELEASE" } })
        {
            d.knob (param, 605, cy, 62, Look::ChromeSmall).scaleOf (7, "1||3||5||7", 272).radii (1.22f, 1.5f).lsize (13).skip();
            d.caption (cap, 605, cy + 46, 16, 0xffd8d8d8);
        }
        // RATIO: an interlocked key stack (4, 8, 12, 20; ALL = every key in).
        d.caption ("RATIO", 752, 116, 14, kWhite);
        const char* ratios[] = { "20", "12", "8", "4" };
        for (int i = 0; i < 4; ++i)
        {
            const float y = 145.0f + 47.0f * (float) i;
            auto& k = d.key (4, 741, y, 24, 44, Look::StackKey);
            k.sel (3 - i);
            k.alsoOn = 4;
            d.text (ratios[i], 692, y + 12, 40, 20, 14, kWhite);
        }
        d.caption ("ALL", 752, 348, 16, kWhite);
        d.key (4, 727, 344, 50, 26, Look::Hidden).sel (4);
        d.vu (809, 148, 220, 102, Look::VuAmberDark, "-20|-10|-7|-5|-3|-1|0", { 20, 10, 7, 5, 3, 1, 0 }, "dB").vuZero = 0.93f;
        d.text ("76  LIMITING AMPLIFIER", 790, 322, 260, 24, 17, 0xffa8a8a8, Look::Sans).kerning (0.18f);
        return d;
    }

    Design soul9000Comp()
    {
        Design d;
        d.panel = juce::Colour (0xff4a4a4b);
        d.texture = Texture::Rough;
        d.wear = 0.6f;
        const juce::uint32 cap = 0xffa8a8a8;
        d.text ("SOUL 9000", 430, 80, 300, 32, 26, 0xffdedede, Look::SerifBold);
        d.text ("Channel Compressor", 430, 368, 300, 26, 18, 0xffe6e6e6, Look::Serif);
        d.key (-1, 195, 149, 55, 32, Look::SoulKey, "IN").size = 16;
        d.leds (215, 223, 14, 128, "20|14|10|6|3", { 20, 14, 10, 6, 3 }, true).labelSize = 13;
        auto blackKnob = [&] (int param, float cx, float cy, const char* lo, const char* hi, const char* caption)
        {
            d.knob (param, cx, cy, 70, Look::BlackDome).scaleOf (11).radii (1.3f, 1.6f).scol (0xffd8d8d8);
            d.text (lo, cx - 50, cy + 38, 40, 18, 13, 0xffdedede);
            d.text (hi, cx + 12, cy + 38, 40, 18, 13, 0xffdedede);
            d.caption (caption, cx, cy + 53, 16, cap);
        };
        blackKnob (0, 461, 288, "-30", "18", "THRESHOLD");
        blackKnob (3, 701, 190, "0.1", "4", "RELEASE");
        blackKnob (1, 941, 288, "1.1", "100", "RATIO");
        d.key (2, 676, 294, 56, 31, Look::SoulKey, "FAST").size = 16;
        d.caption ("ATTACK", 701, 342, 16, cap);
        d.key (4, 915, 149, 55, 32, Look::SoulKey, "PEAK").size = 16;
        return d;
    }

    Design soulBusComp()
    {
        Design d;
        d.panel = juce::Colour (0xff252526);
        d.texture = Texture::Rough;
        d.wear = 0.5f;
        auto& m = d.vu (174, 121, 222, 135, Look::VuWhite, "0|4|8|12|16|20", { 0, 4, 8, 12, 16, 20 }, "dB\nCOMPRESSION");
        m.reverse = true;
        m.range = 20.0f;
        m.housing = 100.0f;
        m.captionY = 0.72f;
        d.add (Type::Screw, 287, 280, 16, 16);
        d.text ("SOUL G", 185, 300, 200, 30, 25, 0xffdcdcdc, Look::Serif);
        d.text ("Bus Compressor", 185, 330, 200, 24, 17, 0xffdcdcdc, Look::SerifBold);
        const juce::uint32 blue = 0xff1f86ee;
        auto stepped = [&] (int param, float cx, const char* labels, const char* unit, const char* caption)
        {
            d.knob (param, cx, 170, 70, Look::ColourCap, blue).scaleOf (6, labels, 160).radii (1.24f, 1.62f).lsize (13);
            d.caption (unit, cx, 221, 12, 0xffcfcfcf);
            d.caption (caption, cx, 233, 16, kWhite);
        };
        stepped (2, 583, "0.1|0.3|1|3|10|30", "mS", "ATTACK");
        stepped (3, 783, "0.1|0.2|0.4|0.8|1.6|auto", "S", "RELEASE");
        stepped (1, 979, "1.5|2|3|4|5|10", "", "RATIO");
        auto level = [&] (int param, float cx, const char* lo, const char* hi, const char* caption, float fixed)
        {
            d.knob (param, cx, 298, 70, Look::ColourCap, blue).scaleOf (11, "", 270).radii (1.3f, 1.6f).at (fixed);
            d.text (lo, cx - 52, 340, 30, 16, 12, kWhite);
            d.text ("dB", cx - 15, 344, 30, 16, 11, 0xffcfcfcf);
            d.text (hi, cx + 22, 340, 30, 16, 12, kWhite);
            d.caption (caption, cx, 360, 16, kWhite);
        };
        level (0, 583, "-40", "0", "THRESHOLD", 0.5f);
        level (-1, 783, "-6", "+12", "MAKEUP", 0.33f);
        d.key (-1, 957, 288, 50, 30, Look::LitSquare, "IN", 0xffb2c22a).at (0.0f).size = 16;
        return d;
    }


    /** The Soul 9000 gate / compressor family: grey panel, serif title, keys with LEDs. */
    void soul9000Frame (Design& d, const char* footer)
    {
        d.panel = juce::Colour (0xff4a4a4b);
        d.texture = Texture::Rough;
        d.wear = 0.6f;
        d.text ("SOUL 9000", 430, 80, 300, 32, 26, 0xffdedede, Look::SerifBold);
        d.text (footer, 430, 368, 300, 26, 18, 0xffe6e6e6, Look::Serif);
        d.leds (215, 223, 14, 128, "20|14|10|6|3", { 20, 14, 10, 6, 3 }, true).labelSize = 13;
    }

    void soul9000Knob (Design& d, int param, float cx, float cy, const char* lo, const char* hi, const char* caption)
    {
        d.knob (param, cx, cy, 70, Look::BlackDome).scaleOf (11).radii (1.3f, 1.6f).scol (0xffd8d8d8);
        d.text (lo, cx - 50, cy + 38, 40, 18, 13, 0xffdedede);
        d.text (hi, cx + 12, cy + 38, 40, 18, 13, 0xffdedede);
        d.caption (caption, cx, cy + 53, 16, 0xffbdbdbd);
    }

    Design soul9000Gate()
    {
        Design d;
        soul9000Frame (d, "Gate/Expander");
        d.key (-1, 155, 150, 55, 32, Look::SoulKey, "ON").size = 16;
        soul9000Knob (d, 3, 381, 285, "0.1", "4", "RELEASE");
        soul9000Knob (d, 0, 581, 190, "-40", "0", "THRESHOLD");
        d.key (5, 555, 295, 55, 31, Look::SoulKey, "2:1").size = 16;
        d.caption ("EXPAND", 583, 342, 16, 0xffbdbdbd);
        soul9000Knob (d, 1, 781, 285, "0", "40", "RANGE");
        soul9000Knob (d, 2, 981, 190, "0", "4", "HOLD");
        d.key (4, 955, 295, 55, 31, Look::SoulKey, "FAST").size = 16;
        d.caption ("ATTACK", 982, 342, 16, 0xffbdbdbd);
        return d;
    }

    /** The Even 88 family: dark green-grey panel, knurled knobs over fan scales, keys with LEDs. */
    void evenFrame (Design& d, const char* model)
    {
        d.panel = juce::Colour (0xff243030);
        d.texture = Texture::Rough;
        d.wear = 0.5f;
        d.text ("Even", 820, 350, 120, 40, 38, 0xff8a9290, Look::Sans).j (J::centredRight);
        d.text (model, 942, 362, 140, 24, 18, 0xffb8c0be, Look::Sans).j (J::centredLeft);
    }

    Element& evenKnob (Design& d, int param, float cx, float cy, juce::uint32 cap, const char* labels, float sweep = 300.0f)
    {
        return d.knob (param, cx, cy, 72, Look::Knurled, cap).scaleOf (11, labels, sweep, false).radii (1.12f, 1.55f).lsize (12).scol (0xffd8d8d8);
    }

    Element& boxedLabel (Design& d, const char* s, float cx, float cy)
    {
        d.box (cx - 20, cy - 8, 40, 16, Look::Line, 0xffd8d8d8, 0, 1.2f);
        return d.text (s, cx - 20, cy - 8, 40, 16, 12, 0xffe0e0e0);
    }

    Design even88Gate()
    {
        Design d;
        evenFrame (d, "88-GATE");
        d.key (-1, 127, 137, 55, 30, Look::SoulKey, "ON", 0xfff0d020).size = 16;
        auto& m = d.leds (141, 204, 30, 132, "50||25||12||6||0", { 50, 40, 25, 18, 12, 9, 6, 3, 1, 0.5f }, true, 0xff38d048);
        m.dots = false;
        m.labelSize = 12;
        d.caption ("ATTACK", 247, 278, 15, 0xffd8d8d8);
        d.key (4, 219, 303, 55, 30, Look::SoulKey, "FAST").size = 16;
        evenKnob (d, 1, 380, 221, 0xff1a1a1a, "|5|10|15|20|25");
        boxedLabel (d, "EXP", 343, 263);
        d.caption ("HYSTERESIS", 380, 278, 15, 0xffd8d8d8);
        evenKnob (d, 0, 585, 221, 0xff8a8a8a, "-40||||||||||0");
        d.text ("dB", 570, 255, 30, 14, 11, 0xffd0d0d0);
        d.caption ("THRESHOLD", 585, 278, 15, 0xffd8d8d8);
        d.path ("M 535 221 L 520 221 L 520 306", 0xffd0d0d0, 1.5f);
        d.key (5, 492, 303, 55, 30, Look::SoulKey, "-40").size = 16;
        evenKnob (d, 2, 785, 221, 0xff1f6a22, "0|10|25|35|50|60");
        d.text ("dB", 770, 255, 30, 14, 11, 0xffd0d0d0);
        d.caption ("RANGE", 785, 278, 15, 0xffd8d8d8);
        evenKnob (d, 3, 983, 221, 0xff1a1a1a, "0.1|||||0.5|||||3");
        d.text ("S", 968, 255, 30, 14, 11, 0xffd0d0d0);
        d.caption ("RELEASE", 983, 278, 15, 0xffd8d8d8);
        return d;
    }

    Design even88Comp()
    {
        Design d;
        evenFrame (d, "88-COMP");
        d.panel = juce::Colour (0xff1f2422);
        d.key (-1, 123, 136, 57, 30, Look::SoulKey, "ON", 0xfff0d020).at (1.0f).size = 16;
        auto& m = d.leds (136, 205, 32, 135, "20||15||10||5||0", { 20, 17, 15, 12, 10, 7, 5, 3, 1, 0.5f }, true, 0xff38d048);
        m.dots = false;
        m.labelSize = 12;
        evenKnob (d, 1, 331, 218, 0xff8a8a8a, "20||||||||||-10");
        d.text ("dB", 316, 252, 30, 14, 11, 0xffd0d0d0);
        d.caption ("THRESHOLD", 331, 276, 15, 0xffd8d8d8);
        d.path ("M 281 218 L 262 218 L 262 306", 0xffd0d0d0, 1.5f);
        d.key (2, 235, 307, 53, 28, Look::SoulKey, "-20").size = 15;
        d.caption ("KNEE", 483, 276, 15, 0xffd8d8d8);
        d.key (0, 457, 307, 55, 28, Look::SoulKey, "HARD").size = 15;
        evenKnob (d, 3, 635, 218, 0xff1f6a22, "1|||||3|||||");
        boxedLabel (d, "LIM", 668, 262);
        d.caption ("RATIO", 635, 276, 15, 0xffd8d8d8);
        d.caption ("ATTACK", 786, 276, 15, 0xffd8d8d8);
        d.key (4, 760, 307, 55, 28, Look::SoulKey, "FAST").size = 15;
        evenKnob (d, 5, 937, 218, 0xff1a1a1a, "0.01|||||0.2|||||");
        boxedLabel (d, "AUTO", 975, 262).size = 11;
        d.text ("s", 922, 252, 30, 14, 11, 0xffd0d0d0);
        d.caption ("RELEASE", 937, 276, 15, 0xffd8d8d8);
        return d;
    }

    Design fairKid()
    {
        Design d;
        d.panel = juce::Colour (0xff1f1f20);
        d.texture = Texture::Rough;
        d.wear = 0.5f;
        d.caption ("ON", 93, 105, 14, kWhite);
        d.toggle (-1, 95, 143, 24).at (1.0f);
        d.lamp (95, 335, 16, 0xffff5a20).at (1.0f);
        auto& m = d.vu (175, 125, 207, 130, Look::VuCream, "-20|-10|-7|-5|-3|-2|-1|0", { 20, 10, 7, 5, 3, 2, 1, 0 }, "DECIBELS");
        m.housing = 112.0f;
        m.captionY = 0.8f;
        d.box (252, 255, 38, 105, Look::Fill, 0xff0c0c0c, 0xff3a3a3a, 1.0f);
        d.add (Type::Screw, 271, 318, 14, 14);
        auto knob = [&] (int param, float cx, float d0, const char* labels, float sweep, const char* caption, float fixed = 0.5f)
        {
            d.knob (param, cx, 238, d0, Look::BlackDot).scaleOf (0, labels, sweep).radii (1.2f, 1.42f).lsize (14).turned().at (fixed);
            d.caption (caption, cx, 141, 15, kWhite);
        };
        knob (-1, 517, 92, "-6|-3|0|+3|+6|+9|+12", 270, "OUT GAIN", 0.33f);
        knob (0, 685, 92, "20|18|16|14|12|10|8|6|4|2|0", 280, "INPUT GAIN");
        knob (1, 853, 92, "0|1|2|3|4|5|6|7|8|9|10", 280, "THRESHOLD");
        d.knob (2, 1020, 238, 60, Look::BlackDot).scaleOf (0, "1|2|3|4|5|6", 130).radii (1.2f, 1.5f).lsize (13);
        d.caption ("TIME CONSTANT", 1020, 141, 15, kWhite);
        d.text ("DC BIAS", 500, 352, 70, 18, 14, kWhite);
        d.knob (3, 602, 345, 34, Look::ChromeSmall).sweep = 120.0f;
        d.text ("Factory\nCal", 624, 352, 50, 32, 12, kWhite, Look::Sans, J::centredLeft);
        d.text ("FAIRKID", 950, 310, 150, 40, 32, 0xffd0302a, Look::Script);
        d.text ("MODEL 670", 950, 357, 150, 20, 14, 0xffb0b0b0, Look::SansLight);
        return d;
    }

    Design red3()
    {
        Design d;
        d.panel = juce::Colour (0xff9c1614);
        d.texture = Texture::Rough;
        d.wear = 0.7f;
        const juce::uint32 gold = 0xffd0a040;
        d.text ("RED", 95, 128, 180, 80, 84, 0xffe82a22, Look::Sans);
        d.text ("Compressor", 70, 206, 240, 44, 38, 0xfff4f0ea, Look::Serif);
        auto knob = [&] (int param, float cx, float cy, const char* labels, const char* lo, const char* unit, const char* hi, const char* caption,
                         float fixed = 0.5f)
        {
            d.knob (param, cx, cy, 76, Look::ChromeBar).scaleOf (0, labels, 270).radii (1.2f, 1.45f).lsize (13).scol (0xfff0e0c0).at (fixed);
            if (*lo != 0)
            {
                d.text (lo, cx - 50, cy + 36, 36, 16, 13, 0xfff0e0c0);
                d.text (unit, cx - 18, cy + 40, 36, 16, 12, gold);
                d.text (hi, cx + 14, cy + 36, 36, 16, 13, 0xfff0e0c0);
            }
            d.caption (caption, cx, cy + 54, 15, gold);
        };
        knob (1, 390, 169, "1.1|1.5|3|5|10", "", "", "", "RATIO");
        knob (0, 598, 169, "", "-48", "dB", "0", "THRESHOLD");
        knob (-1, 182, 313, "", "-6", "dB", "+12", "MAKE UP GAIN", 0.33f);
        knob (2, 390, 313, "", "1", "mS", "50", "ATTACK");
        knob (3, 598, 313, "0.1|0.25|0.6|1.5|4", "", "", "", "RELEASE");
        auto& m = d.vu (743, 120, 240, 240, Look::VuRound, "-20|-15|-10|-5|0|+5|+10", { 20, 15, 10, 5, 0, -5, -10 }, "dB");
        m.colour = juce::Colour (0xffd01818);
        m.captionY = 0.62f;
        m.linearDb (-20.0f, 10.0f);
        m.sweep = 100.0f;
        d.caption ("GAIN REDUCTION", 870, 378, 15, gold);
        d.lamp (713, 343, 18, 0xffff3020, 4);
        d.caption ("AUTO", 713, 367, 13, gold);
        d.lamp (1005, 343, 18, 0xffff3020).at (1.0f);
        d.caption ("IN", 1005, 367, 13, gold);
        return d;
    }

    Design bdx160()
    {
        Design d;
        d.panel = juce::Colour (0xff1e1e1e);
        d.screws = false;
        d.box (30, 70, 46, 336, Look::Wood, 0);
        d.box (1080, 70, 46, 336, Look::Wood, 0);
        for (float y : { 72.0f, 380.0f })
            d.box (76, y, 1004, 24, Look::Bevel, 0xffb8b8b8);
        d.caption ("THRESHOLD", 198, 122, 17, 0xffd8d8d8);
        d.text ("BELOW", 100, 159, 44, 16, 12, kWhite);
        d.lamp (165, 167, 11, 0xffff2a20).at (1.0f);
        d.lamp (232, 167, 11, 0xffff2a20);
        d.text ("ABOVE", 244, 159, 50, 16, 12, kWhite);
        d.knob (0, 198, 235, 66, Look::ChromeSmall).scaleOf (6, "0.01|0.05|0.1|0.5|1|5", 250, false).radii (1.15f, 1.6f).lsize (13);
        d.caption ("POWER", 198, 312, 13, kWhite);
        d.box (170, 333, 58, 22, Look::Fill, 0xff5a1a10, 0xff111111, 2.0f);
        d.box (178, 338, 42, 12, Look::Fill, 0xffa86018);
        d.caption ("COMPRESSION", 398, 122, 17, 0xffd8d8d8);
        d.knob (1, 398, 230, 78, Look::ChromeSmall).scaleOf (9, "", 270, false).radii (1.15f, 1.5f);
        d.text ("1:1.1", 337, 289, 40, 16, 12, kWhite);
        d.text ("1:50", 424, 289, 40, 16, 12, kWhite);
        d.caption ("OUTPUT GAIN", 598, 122, 17, 0xffd8d8d8);
        d.knob (-1, 598, 235, 64, Look::BlackDome).scaleOf (7, "-6|-3|0|+3|+6|+9|+12", 270).radii (1.15f, 1.6f).lsize (12).at (0.33f);
        d.box (752, 127, 260, 173, Look::Fill, 0xff0a0a0a, 0xff9a9a9a, 3.0f);
        d.vu (770, 143, 225, 137, Look::VuOrange, "-20|-15|-10|-5|0|+5|+10", { 20, 15, 10, 5, 0, -5, -10 }, "DECIBELS").linearDb (-20.0f, 10.0f).captionY = 0.8f;
        d.text ("bdx", 800, 310, 70, 40, 36, 0xffd02a20, Look::Sans).j (J::centredRight);
        d.text ("160", 872, 310, 80, 40, 36, kWhite, Look::Sans).j (J::centredLeft);
        d.text ("Compressor/Limiter", 790, 346, 180, 24, 19, kWhite);
        return d;
    }

    Design bdx560()
    {
        Design d;
        d.panel = juce::Colour (0xff1c1c1e);
        d.texture = Texture::Brushed;
        const char* top[] = { "40", "30", "20", "15", "10", "8", "6", "5", "4", "3", "2", "1", "0", "1", "2", "4", "6", "10", "+20" };
        d.leds (398, 96, 575, 14, "", std::vector<float> (19, 999.0f), false, 0xff30d040);
        for (int i = 0; i < 19; ++i)
            d.text (top[i], 383.0f + 575.0f * (float) i / 18.0f, 128, 30, 16, 12, kWhite);
        d.leds (398, 160, 352, 14, "", { 40, 30, 20, 15, 10, 8, 6, 5, 4, 3, 2, 1 }, false, 0xffff2a20);
        d.box (766, 167, 40, 0, Look::Line, 0xffd8d8d8, 0, 1.5f);
        d.text ("GAIN REDUCTION", 812, 159, 130, 16, 13, kWhite, Look::Sans, J::centredLeft);
        d.text ("bdx", 960, 106, 72, 40, 38, 0xffd02a20).j (J::centredRight);
        d.text ("560", 1034, 106, 80, 40, 38, kWhite).j (J::centredLeft);
        d.text ("EasyCompressor", 980, 148, 140, 22, 18, kWhite);
        d.lamp (165, 116, 12, 0xffff2a20);
        d.caption ("BYPASS", 163, 132, 14, kWhite);
        d.key (-1, 147, 156, 32, 32, Look::RoundKey);
        d.lamp (131, 255, 12, 0xff40e040).at (1.0f);
        d.lamp (163, 255, 12, 0xffff2a20);
        d.lamp (196, 255, 12, 0xffff2a20);
        d.text ("BELOW", 108, 264, 50, 16, 12, kWhite);
        d.text ("ABOVE", 172, 264, 50, 16, 12, kWhite);
        d.box (163, 262, 0, 36, Look::Line, 0xffd8d8d8, 0, 1.5f);
        d.lamp (163, 308, 12, 0xfff0e020, 2);
        d.caption ("EASY", 163, 324, 14, kWhite);
        d.key (2, 147, 348, 32, 32, Look::RoundKey);
        auto knob = [&] (int param, float cx, juce::uint32 col, const char* labels, float fixed = 0.5f)
        {
            d.knob (param, cx, 301, 76, Look::ColourDome, col).scaleOf (0, labels, 290).radii (1.2f, 1.5f).lsize (12).banded (0xff9a9a9a).at (fixed);
        };
        knob (0, 358, 0xffe01818, "-40|-30|-20|-10|0|+10|20");
        d.text ("dB", 343, 341, 30, 14, 11, kWhite);
        d.caption ("THRESHOLD", 358, 357, 15, kWhite);
        knob (1, 655, 0xff1a2ae0, "1:1|2:1|3:1|4:1|5:1|10:1|\xe2\x88\x9e:1|-5:1|-3:1|-2:1|-1:1");
        d.text ("COMPRESSION\nRATIO", 575, 355, 160, 34, 15, kWhite);
        knob (-1, 950, 0xff1a8a2a, "", 0.33f);
        d.text ("-6", 910, 341, 24, 14, 12, kWhite);
        d.text ("dB", 935, 345, 30, 14, 11, kWhite);
        d.text ("+12", 968, 341, 30, 14, 12, kWhite);
        d.caption ("OUTPUT GAIN", 950, 357, 15, kWhite);
        return d;
    }

    Design evenCompLim()
    {
        Design d;
        d.panel = juce::Colour (0xff3a4250);
        d.texture = Texture::Brushed;
        d.wear = 0.8f;
        const juce::uint32 blue = 0xff1a8af0;
        d.caption ("COMPRESS", 270, 88, 16, kWhite);
        d.caption ("LIMIT", 655, 88, 16, kWhite);
        d.box (502, 95, 0, 290, Look::Line, 0xffc8ccd4, 0, 1.5f);
        d.box (742, 95, 0, 290, Look::Line, 0xffc8ccd4, 0, 1.5f);
        auto knob = [&] (int param, float cx, float cy, const char* labels, float sweep)
        {
            d.knob (param, cx, cy, 70, Look::ColourCap, blue).scaleOf (0, labels, sweep).radii (1.2f, 1.55f).lsize (12).banded (0xffb8bcc4);
        };
        auto under = [&] (float cx, float cy, const char* lo, const char* unit, const char* hi, const char* caption)
        {
            if (*lo != 0)
            {
                d.text (lo, cx - 50, cy + 36, 34, 14, 12, kWhite);
                d.text (hi, cx + 16, cy + 36, 34, 14, 12, kWhite);
            }
            d.text (unit, cx - 20, cy + 40, 40, 14, 12, kWhite);
            d.caption (caption, cx, cy + 54, 15, kWhite);
        };
        knob (7, 182, 167, "100|400|800|1500|auto 1|auto 2", 200);
        under (182, 167, "", "mS", "", "RECOVERY");
        knob (9, 342, 167, "-6|-3|0|+3|+6|+9|+12", 230);
        under (342, 167, "", "", "", "GAIN");
        knob (5, 182, 311, "", 280);
        under (182, 311, "-35", "dB", "-5", "THRESHOLD");
        knob (6, 342, 311, "1.5|2|3|4|6", 200);
        under (342, 311, "", "", "", "RATIO");
        knob (2, 582, 167, "50|100|200|800|auto 1|auto 2", 200);
        under (582, 167, "", "mS", "", "RECOVERY");
        knob (1, 582, 311, "", 280);
        under (582, 311, "-12", "dB", "0", "THRESHOLD");
        auto sw = [&] (int param, float cx, float cy, const char* up, const char* down, const char* caption)
        {
            d.caption (up, cx, cy - 40, 13, kWhite);
            d.toggle (param, cx, cy, 24);
            if (*down != 0)
                d.caption (down, cx, cy + 27, 13, kWhite);
            d.caption (caption, cx, cy + 54, 15, kWhite);
        };
        sw (4, 446, 167, "in", "", "COMPRESS");
        sw (8, 446, 311, "fast", "slow", "ATTACK");
        sw (0, 685, 167, "in", "", "LIMIT");
        sw (3, 685, 311, "fast", "slow", "ATTACK");
        auto& m = d.vu (815, 105, 225, 107, Look::VuWhite, "0|4|8|12|16|20", { 0, 4, 8, 12, 16, 20 }, "dB\nCOMPRESSION");
        m.reverse = true;
        m.housing = 22.0f;
        m.captionY = 0.7f;
        d.caption ("POWER", 925, 266, 13, kWhite);
        d.key (-1, 902, 292, 48, 54, Look::LitSquare, "", 0xffe02020).at (1.0f);
        d.text ("Even", 760, 352, 110, 36, 34, 0xff8a9098).j (J::centredRight);
        d.text ("COMPRESSOR/LIMITER", 876, 364, 220, 22, 17, 0xffd0d4dc).j (J::centredLeft);
        return d;
    }

    Design noStressor()
    {
        Design d;
        d.panel = juce::Colour (0xff171717);
        d.texture = Texture::Rough;
        d.wear = 0.5f;
        d.leds (86, 120, 481, 14, "", { 26, 23, 20, 17, 14, 12, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1 }, false, 0xffff2a20);
        const char* gr[] = { "26", "23", "20", "17", "14", "12", "10", "9", "8", "7", "6", "5", "4", "3", "2", "1" };
        for (int i = 0; i < 16; ++i)
            d.text (gr[i], 71.0f + 481.0f * (float) i / 15.0f, 143, 30, 16, 12, kWhite);
        d.box (52, 149, 1063, 177, Look::Line, 0xffd8d8d8, 0, 2.0f);
        d.box (103, 278, 480, 48, Look::Line, 0xffd8d8d8, 0, 2.0f);
        d.text ("NoStressor", 60, 216, 200, 44, 34, 0xffd8d8d8, Look::Script);
        d.text ("GAIN REDUCTION", 262, 190, 130, 16, 12, kWhite);
        d.box (255, 228, 46, 22, Look::Bevel, 0xff2a2a2a);
        d.text ("BYPASS", 308, 230, 60, 16, 12, kWhite);
        d.lamp (374, 238, 12, 0xffff2a20).at (1.0f);
        const char* ratios[] = { "1:1.5", "2:1", "3:1", "4:1", "6:1", "10:1", "20:1", "NUKE" };
        for (int i = 0; i < 8; ++i)
        {
            const float x = 134.0f + 48.0f * (float) i;
            d.text (ratios[i], x - 24, 270, 48, 16, 12, kWhite, i == 7 ? Look::Script : Look::Sans);
            d.lamp (x, 303, 13, 0xff40e040, 4).sel (i);
        }
        d.box (510, 270, 48, 21, Look::Bevel, 0xffd8d8d8);
        d.caption ("RATIO", 535, 297, 12, kWhite);
        d.box (282, 318, 66, 16, Look::Fill, 0xff171717);
        d.caption ("RATIO", 315, 318, 13, kWhite);
        const struct
        {
            int param;
            float cx;
            const char* caption;
        } dials[] = { { 0, 655, "INPUT" }, { 2, 783, "ATTACK" }, { 3, 910, "RELEASE" }, { 1, 1038, "OUTPUT" } };
        for (auto& k : dials)
        {
            d.box (k.cx - 46, 141, 92, 18, Look::Fill, 0xff171717);
            d.caption (k.caption, k.cx, 141, 14, kWhite);
            d.box (k.cx, 160, 0, 14, Look::Line, 0xffd8d8d8, 0, 2.0f);
            d.knob (k.param, k.cx, 238, 116, Look::NumberDial).scaleOf (0, "0|1|2|3|4|5|6|7|8|9|10", 300).lsize (13);
        }
        return d;
    }

    Design pia2250()
    {
        Design d;
        d.panel = juce::Colour (0xff2c2e33);
        d.screws = false;
        for (float x : { 65.0f, 866.0f })
        {
            d.box (x, 88, 214, 304, Look::Inset, 0xff141414);
            d.box (x + 27, 106, 190, 258, Look::Fill, 0xff0e38a8);
            for (float cx : { x + 50.0f, x + 140.0f })
                d.box (cx - 6, 128, 12, 210, Look::Fill, 0xff0a1a40, 0xff3a5ac0, 1.0f);
        }
        d.box (1082, 70, 44, 336, Look::Bevel, 0xff1e5ad8);
        d.box (288, 72, 578, 333, Look::Bevel, 0xff1a2028);
        for (auto [x, y] : { std::pair { 300.0f, 82.0f }, { 852.0f, 82.0f }, { 300.0f, 394.0f }, { 852.0f, 394.0f } })
            d.add (Type::Screw, x, y, 10, 10);
        d.key (-1, 322, 112, 32, 28, Look::LitSquare, "ON", 0xffd8d8c8).at (1.0f).size = 13;
        d.lamp (372, 126, 9, 0xff30d040).at (1.0f);
        d.caption ("GAIN REDUCTION", 578, 98, 14, kWhite);
        d.leds (540, 126, 76, 10, "", { 12, 6, 3, 2, 1 }, false, 0xffff2020);
        const char* grl[] = { "12", "6", "3", "2", "1" };
        for (int i = 0; i < 5; ++i)
            d.text (grl[i], 530.0f + 19.0f * (float) i, 134, 20, 14, 11, kWhite);
        d.text ("Pia 2250", 700, 108, 180, 34, 28, 0xff6ab0e0, Look::Serif);
        const juce::uint32 dot = 0xff3aa0e8;
        auto knob = [&] (int param, float cx, juce::uint32 cap, const char* labels, float sweep, float centre = 0.0f)
        {
            const int n = juce::StringArray::fromTokens (labels, "|", "").size();
            d.knob (param, cx, 231, 64, Look::Lobed, cap).scaleOf (n, labels, sweep).radii (1.12f, 1.55f).lsize (12).scol (dot).centred (centre);
        };
        knob (0, 361, 0xff2a2a2a, "+10|+8|+4|0|-4|-8|-12|-20", 280);
        knob (1, 493, 0xff1a8a88, "1|2|3|5|10|20|\xe2\x88\x9e", 280);
        knob (2, 663, 0xffc8c8c8, "FAST|MED|SLOW", 110, -85);
        knob (3, 791, 0xffc8c8c8, ".05s|.12s|.25s|.75s|1.5s|3s", 280);
        for (auto [cx, cap] : { std::pair { 361.0f, "TRESH" }, { 493.0f, "RATIO" }, { 663.0f, "ATTACK" }, { 791.0f, "RELEASE" } })
            d.caption (cap, cx, 288, 14, kWhite);
        d.text ("HARD", 440, 339, 44, 16, 12, kWhite);
        d.toggle (4, 498, 347, 22).sideways();
        d.text ("SOFT", 512, 339, 44, 16, 12, kWhite);
        d.caption ("KNEE", 503, 366, 13, 0xff3aa0e8);
        d.text ("OLD", 598, 339, 44, 16, 12, kWhite);
        d.toggle (5, 655, 347, 22).sideways();
        d.text ("NEW", 668, 339, 44, 16, 12, kWhite);
        d.caption ("TYPE", 657, 366, 13, 0xff3aa0e8);
        return d;
    }

    Design lta100()
    {
        Design d;
        d.panel = juce::Colour (0xffaab8c8);
        d.wear = 1.0f;
        const juce::uint32 ink = 0xff1a1a1a;
        auto chooser = [&] (int param, float cy, const char* caption)
        {
            d.knob (param, 157, cy, 70, Look::BlackDot).sweep = 120.0f;
            d.text ("FAST", 82, cy - 28, 40, 16, 13, ink);
            d.text ("MEDIUM", 128, cy - 52, 60, 16, 13, ink);
            d.text ("SLOW", 194, cy - 28, 44, 16, 13, ink);
            d.caption (caption, 158, cy + 46, 15, ink);
        };
        chooser (2, 161, "ATTACK");
        chooser (3, 306, "RELEASE");
        for (auto [param, cx, cap] : { std::tuple { 0, 355.0f, "GAIN" }, { 1, 860.0f, "GAIN REDUCTION" } })
        {
            d.knob (param, cx, 251, 92, Look::BlackDot).scaleOf (11, "0|1|2|3|4|5|6|7|8|9|10", 270, false).radii (1.08f, 1.45f).lsize (13).scol (ink).banded (0xff1f5a9a);
            d.caption (cap, cx, 331, 15, ink);
        }
        d.vu (480, 145, 260, 92, Look::VuAmber, "-20|-10|-7|-5|-3|-1|0|+1|+2|+3", { 20, 10, 7, 5, 3, 1, 0, -1, -2, -3 }, "").vuZero = 0.75f;
        d.text ("LTA100Be", 508, 302, 200, 30, 25, ink);
        d.text ("Leveling Tube Amplifier", 458, 328, 300, 28, 23, ink);
        d.caption ("ACTIVE", 1028, 108, 15, ink);
        d.lamp (1028, 147, 18, 0xffff2a20).at (1.0f);
        d.knob (-1, 1028, 311, 64, Look::BlackDot).at (0.9f);
        d.text ("DRY", 980, 352, 40, 16, 13, ink);
        d.text ("WET", 1040, 352, 40, 16, 13, ink);
        return d;
    }

    Design eternalBliss()
    {
        Design d;
        d.panel = juce::Colour (0xff8a8c90);
        d.texture = Texture::Brushed;
        d.screws = false;
        d.path ("M 178 100 L 1058 100 Q 1074 100 1074 116 L 1074 360 Q 1074 376 1058 376 L 162 376 Q 146 376 147 360 L 162 116 Q 164 100 178 100 Z", 0xff050a18, 0.0f);
        d.path ("M 180 104 L 1056 104 Q 1070 104 1070 118 L 1070 358 Q 1070 372 1056 372 L 164 372 Q 151 372 152 358 L 166 118 Q 168 104 180 104 Z", 0xff14244e, 0.0f);
        for (auto [x, y] : { std::pair { 50.0f, 183.0f }, { 50.0f, 313.0f }, { 1098.0f, 165.0f }, { 1098.0f, 313.0f } })
            d.add (Type::Screw, x, y, 16, 16);
        auto& m = d.leds (121, 108, 14, 242, "1.5|3.0|4.5|6.0|7.5|9.0|10.5|12.0|13.5|15.0|16.5|18.0|19.5",
                          { 1.5f, 3, 4.5f, 6, 7.5f, 9, 10.5f, 12, 13.5f, 15, 16.5f, 18, 19.5f }, true, 0xffff2020);
        m.labelSize = 13;
        d.lamp (108, 368, 12, 0xff60c040);
        d.text ("GRL", 62, 360, 34, 16, 13, kWhite, Look::Sans, J::centredRight);
        auto knob = [&] (int param, float cx, float cy, const char* labels, float sweep, float fixed = 0.5f)
        {
            d.knob (param, cx, cy, 72, Look::Chrome).scaleOf (0, labels, sweep).radii (1.2f, 1.5f).lsize (13).at (fixed);
        };
        auto bottom = [&] (float cx, float cy, const char* lo, const char* unit, const char* hi)
        {
            d.text (lo, cx - 56, cy + 38, 40, 16, 13, kWhite);
            d.text (unit, cx - 20, cy + 42, 40, 16, 13, kWhite);
            d.text (hi, cx + 16, cy + 38, 40, 16, 13, kWhite);
        };
        auto title = [&] (const char* s, float x, float y) { d.text (s, x - 60, y - 10, 120, 22, 18, kWhite); };
        d.knob (0, 240, 178, 72, Look::Chrome).scaleOf (11, "", 270).radii (1.25f, 1.5f);
        bottom (240, 178, "-35", "dB", "0");
        title ("Threshold", 313, 131);
        knob (2, 487, 178, "0.6|1.1|1.9|3.4|6.0|10|30|50|100", 270);
        bottom (487, 178, "0.4", "ms", "150");
        title ("Attack", 541, 131);
        d.lamp (590, 190, 18, 0xffff3020, 4);
        d.caption ("Auto Fast", 590, 214, 14, kWhite);
        knob (3, 727, 178, "8|13|21|36|60|100|280|450|780", 270);
        bottom (727, 178, "5", "ms", "1k2");
        title ("Release", 785, 131);
        d.lamp (830, 190, 18, 0xffff3020, 5);
        d.caption ("Anti Log", 830, 214, 14, kWhite);
        knob (1, 967, 178, "1.3|1.6|2.0|5.0|10|-0.3|-1.0|-2.0|-3.0", 270);
        bottom (967, 178, "1.2", "1:X", "-4.0");
        title ("Ratio", 915, 131);
        knob (7, 727, 310, "19|17|15|13|11|9|5|3|1.5", 270);
        bottom (727, 310, "21", "dB", "-0");
        title ("Gr Limit", 785, 258);
        d.lamp (830, 320, 18, 0xffff3020, 6);
        d.caption ("On", 830, 344, 14, kWhite);
        d.knob (-1, 967, 310, 72, Look::Chrome).scaleOf (11, "", 270).radii (1.25f, 1.5f).at (0.33f);
        bottom (967, 310, "-6", "dB", "+12");
        title ("Gain", 1013, 260);
        d.lamp (260, 296, 18, 0xffff3020).at (1.0f);
        d.caption ("Active", 260, 319, 14, kWhite);
        d.knob (-1, 365, 290, 124, Look::SilverDial).sweep = 0.0f;
        d.path ("M 365 242 A 48 48 0 1 1 364.9 242 Z", 0xffe4e6ea, 0.0f);
        d.text ("eternal", 315, 266, 100, 30, 27, 0xff2a3040);
        d.text ("bliss", 315, 292, 100, 24, 21, 0xff3a4050);
        return d;
    }

    Design drawMoreComp()
    {
        Design d;
        d.panel = juce::Colour (0xff2b2927);
        d.texture = Texture::Rough;
        d.wear = 0.5f;
        const juce::uint32 line = 0xffc8c8c8, txt = 0xffd8d8d8;
        d.box (58, 103, 768, 273, Look::Line, line, 0, 1.8f);
        d.box (828, 103, 282, 273, Look::Line, line, 0, 1.8f);
        auto gapLabel = [&] (const char* s, float cx, float y, float w)
        {
            d.box (cx - w * 0.5f, y - 9, w, 18, Look::Fill, 0xff2b2927);
            d.caption (s, cx, y - 9, 15, txt);
        };
        gapLabel ("COMPRESSOR", 413, 103, 116);
        gapLabel ("PEAK", 965, 103, 52);
        for (auto [s, cx, w] : { std::tuple { "THRESHOLD", 126.0f, 96.0f }, { "RATIO", 269.0f, 60.0f }, { "ATTACK", 413.0f, 70.0f }, { "RELEASE", 557.0f, 80.0f },
                                 { "GAIN", 701.0f, 50.0f }, { "THRESHOLD", 913.0f, 96.0f }, { "RELEASE", 1037.0f, 80.0f } })
            gapLabel (s, cx, 376, w);
        d.caption ("GAIN REDUCTION", 198, 107, 14, txt);
        const char* gr[] = { "30", "20", "15", "10", "6", "4", "2", "1" };
        for (int i = 0; i < 8; ++i)
        {
            d.text (gr[i], 71.0f + 32.0f * (float) i, 127, 30, 16, 12, txt);
            d.text (gr[i], 838.0f + 32.2f * (float) i, 127, 30, 16, 12, txt);
        }
        d.leds (86, 144, 224, 14, "", { 30, 20, 15, 10, 6, 4, 2, 1 }, false, 0xffff2020);
        d.leds (853, 144, 225, 14, "", { 30, 20, 15, 10, 6, 4, 2, 1 }, false, 0xffff2020);
        d.text ("DRAW MORE", 520, 104, 220, 32, 27, 0xffcfcfcf, Look::Script);
        d.text ("D241", 590, 134, 80, 18, 15, 0xffcfcfcf);
        auto knob = [&] (int param, float cx, const char* labels, const char* unit, float fixed = 0.5f)
        {
            d.knob (param, cx, 239, 70, Look::BlackFluted, 0xff6a6a6a).scaleOf (0, labels, 290).radii (1.2f, 1.5f).lsize (13).scol (txt).at (fixed);
            d.text (unit, cx - 20, 283, 40, 16, 12, txt);
        };
        knob (0, 126, "0|-10|-20|-30|-40|-50|-60", "dB");
        knob (1, 269, "1.1|1.5|2|3|5|8|20|100", "dB");
        knob (2, 413, ".5|2|7|20|100", "mS");
        knob (3, 557, ".05|.2|.5|1|5", "Sec");
        knob (-1, 701, "-6|-3|0|3|6|9|12", "dB", 0.33f);
        knob (4, 897, "-20|-15|-10|-4|0", "dB");
        knob (5, 1037, ".05|.2|.5|1|5", "Sec");
        d.lamp (487, 301, 14, 0xff40e040, 6);
        d.path ("M 415 344 L 415 323 L 558 323 L 558 344", line, 1.5f);
        d.box (465, 315, 44, 16, Look::Fill, 0xff2b2927);
        d.caption ("AUTO", 487, 315, 13, txt);
        d.key (6, 475, 339, 24, 22, Look::StackKey).inv();
        d.lamp (773, 301, 14, 0xffff2020);
        d.caption ("BYPASS", 773, 315, 13, txt);
        d.key (-1, 762, 339, 24, 22, Look::StackKey);
        return d;
    }

    Design drawMoreGate()
    {
        Design d;
        d.panel = juce::Colour (0xff2b2927);
        d.texture = Texture::Rough;
        d.wear = 0.5f;
        d.screws = false;
        const juce::uint32 line = 0xffc8c8c8, txt = 0xffd8d8d8;
        for (float x : { 75.0f, 1080.0f })
            d.key (-1, x - 21, 212, 42, 42, Look::RoundKey);
        d.box (240, 102, 685, 273, Look::Line, line, 0, 1.8f);
        for (auto [s, w, y] : { std::tuple { "EXP/GATE", 90.0f, 102.0f }, { "THRESHOLD", 100.0f, 375.0f } })
        {
            d.box (582 - w * 0.5f, y - 9, w, 18, Look::Fill, 0xff2b2927);
            d.caption (s, 582, y - 9, 15, txt);
        }
        d.text ("DRAW MORE", 256, 222, 220, 32, 27, 0xffcfcfcf, Look::Script);
        d.text ("EXPANDER/GATE 241", 266, 252, 200, 20, 15, 0xffcfcfcf);
        d.lamp (437, 183, 12, 0xff40e040);
        d.path ("M 437 175 L 437 158 L 582 158 L 582 170", line, 1.5f);
        d.knob (0, 582, 236, 62, Look::ColourDome, 0xffe8e870).scaleOf (0, "-80|-72|-64|-56|-48|-40|-32|-24|-16|-8|0", 300).radii (1.2f, 1.6f).lsize (12).scol (txt);
        d.text ("dB", 567, 286, 30, 14, 11, txt);
        d.caption ("SLOW", 726, 206, 13, txt);
        d.lamp (726, 240, 12, 0xffff2020, 1);
        d.caption ("RELEASE", 726, 258, 13, txt);
        d.key (1, 715, 281, 24, 23, Look::StackKey).inv();
        d.lamp (870, 240, 12, 0xffff2020).at (1.0f);
        d.caption ("BYPASS", 870, 258, 13, txt);
        d.key (-1, 858, 281, 24, 23, Look::StackKey);
        return d;
    }

    Design deEsser902()
    {
        Design d;
        d.panel = juce::Colour (0xff1c1c1e);
        d.texture = Texture::Brushed;
        const char* gr[] = { "20", "15", "10", "8", "6", "5", "4", "3", "2", "1" };
        for (int i = 0; i < 10; ++i)
            d.text (gr[i], 351.0f + 32.0f * (float) i, 125, 30, 16, 12, kWhite);
        d.leds (366, 160, 288, 14, "", { 20, 15, 10, 8, 6, 5, 4, 3, 2, 1 }, false, 0xffff2020);
        d.box (670, 166, 40, 0, Look::Line, 0xffd8d8d8, 0, 1.5f);
        d.text ("GAIN REDUCTION", 716, 158, 130, 16, 13, kWhite, Look::Sans, J::centredLeft);
        d.text ("bdx", 960, 106, 72, 40, 38, 0xffd02a20).j (J::centredRight);
        d.text ("902", 1034, 106, 80, 40, 38, kWhite).j (J::centredLeft);
        d.text ("De - Esser", 1000, 146, 120, 22, 18, kWhite);
        for (auto [cx, s, param] : { std::tuple { 166.0f, "IN", -1 }, { 301.0f, "SOLO", -1 }, { 437.0f, "HF ONLY", 2 } })
        {
            d.lamp (cx, 255, 12, 0xffff2020, param).at (param < 0 && cx < 200 ? 1.0f : 0.0f);
            d.key (param, cx - 16, 287, 32, 32, Look::RoundKey);
            d.caption (s, cx, 325, 14, kWhite);
        }
        d.knob (0, 653, 301, 76, Look::ColourDome, 0xffe8e870).scaleOf (0, "800|1.4k|2.6k|4.4k|8k", 270).radii (1.2f, 1.55f).lsize (12).banded (0xff9a9a9a);
        d.text ("Hz", 638, 341, 30, 14, 11, kWhite);
        d.caption ("FREQUENCY", 653, 357, 15, kWhite);
        d.knob (1, 948, 301, 76, Look::ColourDome, 0xff2a2a2a).scaleOf (0, "0|6|12", 270).radii (1.2f, 1.55f).lsize (12).banded (0xff9a9a9a);
        d.text ("NORM", 868, 249, 40, 14, 11, kWhite);
        d.text ("dB", 933, 341, 30, 14, 11, kWhite);
        d.caption ("RANGE", 948, 357, 15, kWhite);
        return d;
    }

    void sourceExtractorPanel (Design& d)
    {
        d.panel = juce::Colour (0xffb4ccd8);
        d.wear = 1.0f;
        const juce::uint32 ink = 0xff1a1e22;
        for (auto [cx, s] : { std::pair { 124.0f, "POWER" }, { 183.0f, "ACTIVE" } })
        {
            d.lamp (cx, 148, 12, 0xffff3020);
            d.caption (s, cx, 163, 11, ink);
        }
        d.text ("PSE-545", 95, 210, 220, 52, 46, ink, Look::SansLight, J::centredLeft);
        d.box (308, 240, 70, 0, Look::Line, 0xff8a9aa4, 0, 3.0f);
        d.text ("Source Extractor", 95, 264, 320, 42, 35, 0xff1e2e5a, Look::SansLight, J::centredLeft);
        d.caption ("TRESHOLD", 547, 154, 16, ink);
        d.knob (0, 547, 268, 92, Look::BlackSkirt).scaleOf (7, "", 270, false).radii (1.0f, 1.0f).banded (0xff2e3e4c).scol (0xffb4ccd8);
        d.text ("-36", 488, 330, 36, 16, 14, ink);
        d.text ("-12", 529, 182, 36, 16, 14, ink);
        d.text ("+12", 572, 330, 36, 16, 14, ink);
        d.caption ("DEPTH", 752, 154, 16, ink);
        d.knob (1, 752, 268, 92, Look::WhiteDome).scaleOf (7, "", 270, false).radii (1.0f, 1.0f).banded (0xff2e3e4c).scol (0xffb4ccd8);
        d.text ("0", 694, 330, 36, 16, 14, ink);
        d.text ("10", 736, 182, 36, 16, 14, ink);
        d.text ("20", 779, 330, 36, 16, 14, ink);
    }

    Design sourceExtractor()
    {
        Design d;
        sourceExtractorPanel (d);
        const juce::uint32 ink = 0xff1a1e22;
        d.text ("FAST", 910, 202, 80, 20, 16, ink);
        d.key (2, 991, 190, 42, 42, Look::RoundKey, "", 0xff40c0ff);
        d.text ("PEAK", 910, 278, 80, 20, 16, ink);
        d.key (3, 991, 266, 42, 42, Look::RoundKey, "", 0xff40c0ff);
        return d;
    }

    Design autoRider()
    {
        Design d;
        d.panel = juce::Colour (0xffc6c6c4);
        d.wear = 0.9f;
        const juce::uint32 ink = 0xff1a1a1a;
        d.box (70, 83, 518, 308, Look::Line, 0xff3a3a3a, 0, 2.0f);
        d.box (600, 83, 165, 308, Look::Line, 0xff3a3a3a, 0, 2.0f);
        d.box (777, 83, 309, 308, Look::Line, 0xff3a3a3a, 0, 2.0f);
        const struct
        {
            int param;
            float cx, cy;
            const char* caption;
        } knobs[] = { { 0, 198, 149, "THRESHOLD" }, { 1, 334, 149, "TARGET" }, { 2, 470, 149, "SPEED" },
                      { 3, 198, 278, "RATIO" },     { 4, 334, 278, "HOLD" },   { 5, 470, 278, "RANGE" } };
        for (auto& k : knobs)
        {
            d.caption (k.caption, k.cx, k.cy - 58, 21, ink);
            d.knob (k.param, k.cx, k.cy, 70, Look::BlackSkirt);
            auto& v = d.add (Type::Value, k.cx - 60, k.cy + 39, 120, 22);
            v.param = k.param;
            v.look = Look::Sans;
            v.size = 17;
            v.colour = juce::Colour (ink);
            v.colour2 = juce::Colour (0x00000000);
        }
        d.key (-1, 89, 353, 26, 26, Look::LitSquare, "", 0xffeeeeee).at (1.0f);
        d.text ("ON", 122, 356, 40, 20, 17, ink, Look::Sans, J::centredLeft);
        d.text ("AUTO RIDER", 235, 348, 200, 36, 30, ink, Look::SansLight);
        const char* sc[] = { "+15", "+10", "+5", "0", "-5", "-10", "-15" };
        for (int i = 0; i < 7; ++i)
        {
            const float y = 119.0f + 34.5f * (float) i;
            d.text (sc[i], 616, y - 8, 40, 16, 13, ink);
            d.text (sc[i], 717, y - 8, 40, 16, 13, ink);
            d.box (656, y, 14, 0, Look::Line, ink, 0, 1.2f);
            d.box (700, y, 14, 0, Look::Line, ink, 0, 1.2f);
        }
        d.fader (-1, 660, 108, 50, 226, 0xffd8d8d8).at (0.5f);
        d.box (668, 358, 36, 18, Look::Fill, 0xff2a3a2a, 0xff111111, 2.0f);
        for (auto [x, s, cap] : { std::tuple { 838.0f, "IN", "THRESHOLD" }, { 990.0f, "OUT", "TARGET" } })
        {
            d.caption (s, x + 16, 112, 21, ink);
            d.box (x, 141, 34, 195, Look::Fill, 0xff0c2410, 0xff8a8a8a, 3.0f);
            d.caption (cap, x + 16, 358, 15, ink);
        }
        const char* db[] = { "-0", "-6", "-12", "-18", "-24", "-30", "-36", "-42", "-48", "-54" };
        for (int i = 0; i < 10; ++i)
        {
            const float y = 151.0f + 19.4f * (float) i;
            d.text (db[i], 908, y - 8, 40, 16, 13, ink);
            d.box (884, y, 10, 0, Look::Line, ink, 0, 1.5f);
            d.box (962, y, 10, 0, Look::Line, ink, 0, 1.5f);
        }
        d.text (">", 808, 270, 20, 20, 20, ink);
        d.text ("<", 1038, 198, 20, 20, 20, ink);
        return d;
    }

    Design soulWarmth()
    {
        Design d;
        d.panel = juce::Colour (0xff1d1d1d);
        d.texture = Texture::Rough;
        d.wear = 0.5f;
        d.screws = false;
        for (float x : { 75.0f, 1080.0f })
            d.key (-1, x - 21, 212, 42, 42, Look::RoundKey);
        d.text ("SOUL Warmth", 430, 88, 300, 32, 25, 0xffdedede, Look::SerifBold);
        d.key (-1, 195, 152, 55, 30, Look::SoulKey, "IN").size = 16;
        auto knob = [&] (int param, float cx, float cy, Look look, juce::uint32 cap, const char* lo, const char* hi, const char* caption)
        {
            d.knob (param, cx, cy, 72, look, cap).scaleOf (11).radii (1.3f, 1.6f).scol (0xffd8d8d8);
            d.text (lo, cx - 56, cy + 40, 44, 16, 13, 0xffdedede);
            d.text (hi, cx + 12, cy + 40, 44, 16, 13, 0xffdedede);
            d.caption (caption, cx, cy + 56, 15, 0xffcfcfcf);
        };
        knob (0, 402, 190, Look::ColourCap, 0xffe01818, "10%", "125%", "DRIVE");
        knob (1, 222, 287, Look::BlackDome, 0, "2nd", "3rd", "HARMONICS");
        knob (3, 582, 287, Look::ColourCap, 0xfff6f6f6, "-18", "+6", "TRIM");
        d.text ("dB", 567, 330, 30, 14, 11, 0xffdedede);
        d.text ("Preamp", 532, 370, 100, 22, 16, 0xffdedede);
        knob (2, 757, 190, Look::BlackDome, 0, "-", "+", "COLOR");
        knob (4, 937, 287, Look::ColourCap, 0xff7a7a7a, "0%", "100%", "MIX");
        return d;
    }

    Design waveDesigner()
    {
        Design d;
        d.panel = juce::Colour (0xffa4a4a6);
        d.texture = Texture::Brushed;
        d.screws = false;
        for (float x : { 78.0f, 1072.0f })
            d.add (Type::Screw, x, 230, 26, 26);
        d.path ("M 230 120 L 922 120 A 112 112 0 0 1 922 358 L 230 358 A 112 112 0 0 1 230 120 Z", 0xff6a6a6c, 0.0f);
        d.path ("M 232 126 L 920 126 A 106 106 0 0 1 920 352 L 232 352 A 106 106 0 0 1 232 126 Z", 0xff0c0c0c, 0.0f);
        auto knob = [&] (int param, float cx, float dia, const char* labels, float sweep)
        {
            d.knob (param, cx, 239, dia, Look::ColourCap, 0xfff2f2f2).scaleOf (31, labels, sweep).radii (1.12f, 1.32f).lsize (12).turned();
            d.text ("dB", cx - 15, 296 + (dia - 92) * 0.3f, 30, 14, 12, kWhite);
        };
        knob (0, 245, 92, "-15|-13|-10|-6|-3|0|+3|+6|+10|+13|+15", 280);
        d.caption ("ATTACK", 245, 150, 17, kWhite);
        knob (1, 462, 92, "-24|-18|-12|-6|0|+6|+12|+18|+24", 280);
        d.caption ("SUSTAIN", 462, 150, 17, kWhite);
        d.text ("WAVE DESIGNER", 598, 182, 160, 20, 17, kWhite, Look::SansLight);
        d.caption ("ON", 678, 228, 15, kWhite);
        d.key (-1, 658, 256, 40, 40, Look::LitSquare, "", 0xff2a7ae0).at (1.0f);
        knob (2, 900, 140, "-18|-15|-12|-9|-6|-3|+0|+3|+6|+9", 290);
        d.text ("OUTPUT GAIN", 950, 220, 120, 20, 16, kWhite).centre = 78.0f;
        d.lamp (170, 183, 9, 0xffff3020);
        d.text ("OVL", 180, 175, 40, 16, 12, kWhite);
        d.lamp (175, 296, 9, 0xffd8c020);
        d.text ("SIG", 185, 288, 40, 16, 12, kWhite);
        return d;
    }

    /** One band of the dynamic EQ's controls (params from `o`: thr, ratio, att, rel, filter, gain, freq, q, mode). */
    void dynEqBand (Design& d, int o, int page)
    {
        const juce::uint32 txt = 0xffeaeaea;
        auto on = [page] (Element& e) -> Element& { e.page = page; return e; };
        auto value = [&] (int param, float cx, float y)
        {
            auto& v = on (d.add (Type::Value, cx - 50, y, 100, 20));
            v.param = param;
            v.look = Look::Sans;
            v.size = 15;
            v.colour = juce::Colour (txt);
            v.colour2 = juce::Colour (0x00000000);
        };
        const struct
        {
            int param;
            float cx;
            const char* caption;
        } small[] = { { o + 2, 126, "Attack" }, { o + 3, 213, "Release" }, { o + 5, 390, "Gain" }, { o + 6, 476, "Freq" }, { o + 7, 563, "Q" } };
        for (auto& k : small)
        {
            on (d.caption (k.caption, k.cx, 134, 15, txt));
            on (d.knob (k.param, k.cx, 183, 44, Look::Chrome).scaleOf (25, "", 300, false).radii (1.05f, 1.0f).scol (0xffbdbdbd));
            value (k.param, k.cx, 211);
        }
        on (d.caption ("Above", 302, 134, 15, txt));
        on (d.toggle (o + 8, 302, 183, 24));
        on (d.caption ("Below", 302, 211, 15, txt));
        on (d.caption ("Threshold", 170, 241, 15, txt));
        on (d.knob (o, 170, 302, 62, Look::Chrome).scaleOf (31, "", 300, false).radii (1.05f, 1.0f).scol (0xffbdbdbd));
        value (o, 170, 337);
        on (d.caption ("Filter Types", 345, 241, 15, txt));
        const struct
        {
            const char* s;
            float cx, cy;
            int choice;
        } filters[] = { { "LP 6dB", 283, 281, 2 }, { "Flat", 345, 281, 0 }, { "HP 6dB", 410, 281, 4 },
                        { "LP 12dB", 283, 327, 3 }, { "BP", 345, 327, 1 }, { "HP 12dB", 410, 327, 5 } };
        for (auto& f : filters)
            on (d.key (o + 4, f.cx - 24, f.cy - 17, 48, 34, Look::FlatKey, f.s, 0xffe8a020)).sel (f.choice).size = 12;
        on (d.caption ("Ratio", 522, 241, 15, txt));
        on (d.knob (o + 1, 522, 303, 54, Look::Chrome).scaleOf (0, "1.2|1.3|1.5|2.0|3.0|5.0|10", 280).radii (1.2f, 1.45f).lsize (11).scol (txt));
    }

    Design dynamicEq()
    {
        Design d;
        d.backdrop = true;
        d.panel = juce::Colour (0xff4e4c42);
        d.texture = Texture::Brushed;
        d.screws = false;
        for (auto [x, y] : { std::pair { 46.0f, 87.0f }, { 1110.0f, 87.0f }, { 46.0f, 390.0f }, { 1110.0f, 390.0f } })
            d.add (Type::Screw, x, y, 22, 22);
        d.text ("Dynamic Equalizer", 80, 86, 220, 24, 20, 0xffeaeaea, Look::SansLight, J::centredLeft);
        d.box (68, 122, 557, 235, Look::Fill, 0xff382c26, 0xff6a3a2a, 2.0f);
        dynEqBand (d, 0, -1);
        auto& gr = d.add (Type::Graph, 674, 126, 408, 227);
        gr.params = { 6, 5, 7, 4 };
        return d;
    }

    Design dualDynamicEq()
    {
        Design d;
        d.panel = juce::Colour (0xff4a5468);
        d.texture = Texture::Brushed;
        d.screws = false;
        for (auto [x, y] : { std::pair { 46.0f, 87.0f }, { 1110.0f, 87.0f }, { 46.0f, 390.0f }, { 1110.0f, 390.0f } })
            d.add (Type::Screw, x, y, 22, 22);
        d.text ("Dual Dynamic EQ", 80, 86, 260, 26, 22, 0xffeaeaea, Look::SansLight, J::centredLeft);
        d.key (-1, 458, 88, 72, 24, Look::TabKey, "DEQ 1").sel (0).size = 14;
        d.key (-1, 537, 88, 72, 24, Look::TabKey, "DEQ 2").sel (1).size = 14;
        d.box (68, 121, 559, 242, Look::Fill, 0xff16223a, 0xff2a3a5a, 2.0f);
        dynEqBand (d, 0, 0);
        dynEqBand (d, 9, 1);
        auto& gr = d.add (Type::Graph, 674, 130, 410, 228);
        gr.params = { 6, 5, 7, 4, 15, 14, 16, 13 };
        return d;
    }

    Design transientLimiter()
    {
        Design d;
        d.panel = juce::Colour (0xff2a2828);
        d.texture = Texture::Rough;
        d.wear = 0.4f;
        d.screws = false;
        d.text ("Listen\nMic\nToolkit", 128, 94, 90, 72, 20, 0xffe8e8e8, Look::Serif, J::centredLeft);
        for (float x : { 73.0f, 1076.0f })
            d.key (-1, x - 21, 210, 42, 42, Look::RoundKey);
        auto knob = [&] (int param, float cx, float cy, Look look, juce::uint32 cap, const char* lo, const char* hi, const char* caption)
        {
            d.knob (param, cx, cy, 74, look, cap).scaleOf (11).radii (1.3f, 1.6f).scol (0xffd8d8d8);
            d.text (lo, cx - 56, cy + 40, 44, 16, 13, 0xffdedede);
            d.text (hi, cx + 12, cy + 40, 44, 16, 13, 0xffdedede);
            d.caption (caption, cx, cy + 56, 15, 0xffd8d8d8);
        };
        knob (0, 177, 288, Look::BlackDome, 0, "0", "1", "SPEED");
        knob (1, 376, 288, Look::ColourCap, 0xff1e6a2a, "-10", "10", "TRANSIENTS");
        knob (2, 575, 186, Look::BlackDome, 0, "-12", "12", "GAIN");
        d.text ("dB", 560, 226, 30, 14, 11, 0xffdedede);
        d.caption ("SHAPER", 575, 256, 15, 0xffd8d8d8);
        d.key (3, 548, 296, 56, 30, Look::SoulKey, "IN", 0xfff0d020).size = 16;
        knob (4, 776, 288, Look::ColourCap, 0xff8a8a8a, "0", "100", "COMPRESSION");
        knob (5, 975, 186, Look::BlackDome, 0, "-12", "12", "GAIN");
        d.text ("dB", 960, 226, 30, 14, 11, 0xffdedede);
        d.caption ("COMP", 975, 256, 15, 0xffd8d8d8);
        d.key (6, 948, 296, 56, 30, Look::SoulKey, "IN", 0xfff0d020).size = 16;
        return d;
    }

    Design oneKnobComp()
    {
        Design d;
        d.panel = juce::Colour (0xff353537);
        d.texture = Texture::Rough;
        d.wear = 0.4f;
        d.screws = false;
        d.box (36, 70, 22, 336, Look::Bevel, 0xff4a4a4c);
        d.box (1098, 70, 28, 336, Look::Bevel, 0xff4a4a4c);
        d.box (58, 88, 1040, 300, Look::Inset, 0xff222224);
        for (float x : { 108.0f, 203.0f, 301.0f, 398.0f, 495.0f, 689.0f, 786.0f, 883.0f, 980.0f })
            d.box (x - 7, 133, 14, 200, Look::Fill, 0xff151515, 0xff3a3a3a, 1.0f);
        d.box (566, 72, 99, 332, Look::Bevel, 0xff17191c);
        d.text ("One Knob\nCompressor", 566, 92, 99, 34, 14, 0xffd8d8d8, Look::SansLight);
        d.knob (0, 616, 181, 66, Look::Chrome).scaleOf (21, "", 280, false).radii (1.08f, 1.0f).scol (0xffd8d8d8);
        d.text ("0", 584, 216, 20, 14, 11, 0xffd8d8d8);
        d.text ("10", 628, 216, 20, 14, 11, 0xffd8d8d8);
        d.text ("GAIN\nREDUCTION", 566, 234, 99, 30, 13, 0xffd8d8d8);
        d.key (1, 597, 300, 36, 36, Look::RoundKey, "", 0xff60e060);
        d.text ("AUTO\nGAIN", 566, 346, 99, 30, 13, 0xffd8d8d8);
        return d;
    }

    //==========================================================================
    std::map<juce::String, Design> buildDesigns()
    {
        std::map<juce::String, Design> m;
        auto put = [&m] (const char* key, Design d)
        {
            d.token = key;
            m[key] = std::move (d);
        };
        put ("D:LA", levelingAmp());
        put ("D:76LA", limitingAmp76());
        put ("D:9000C", soul9000Comp());
        put ("D:SBUS", soulBusComp());
        put ("D:9000G", soul9000Gate());
        put ("D:E88", even88Gate());
        put ("D:E88C", even88Comp());
        put ("D:F670", fairKid());
        put ("D:RED3", red3());
        put ("D:B160", bdx160());
        put ("D:B560", bdx560());
        put ("D:ECL33", evenCompLim());
        put ("D:NSTR", noStressor());
        put ("D:2250", pia2250());
        put ("D:L100", lta100());
        put ("D:BLISS", eternalBliss());
        put ("D:D241C", drawMoreComp());
        put ("D:D241G", drawMoreGate());
        put ("D:DS902", deEsser902());
        put ("D:PSE", sourceExtractor());
        put ("D:RIDE", autoRider());
        put ("D:WARM", soulWarmth());
        put ("D:WAVE", waveDesigner());
        put ("D:DEQ", dynamicEq());
        put ("D:DEQ2", dualDynamicEq());
        put ("D:LMT", transientLimiter());
        put ("D:ONEC", oneKnobComp());
        return m;
    }
} // namespace

const Design* findDesign (models::Table table, const juce::String& token)
{
    static const std::map<juce::String, Design> designs = buildDesigns();
    const char* prefix = table == models::Dynamics ? "D:" : (table == models::Fx ? "F:" : "E:");
    if (auto it = designs.find (prefix + token); it != designs.end())
        return &it->second;
    return nullptr;
}

} // namespace livedaw::ui::fp
