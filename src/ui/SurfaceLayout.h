#pragma once

#include <juce_graphics/juce_graphics.h>

#include <array>
#include <optional>

/** Surface geometry that has no component behind it: the fader strips' rows
    and the WING COMPACT's surface plan (the main component and the COMPACT's
    key section lay themselves out with it; the tests check it), and what the
    COMPACT's fader 13 and 16 keys are set to. */
namespace livedaw::ui::layout
{

/** A fader section's layer column (VIEW, the layer keys, the shift keys). */
constexpr int kBankColumnWidth = 56;

// A fader strip `w` px wide, top to bottom: a 3 px margin, the display window
// (the light bar's row and the LCD, wider than tall as on the console: about
// 1.4 : 1), a gap, then SELECT.
inline int scribbleLcdHeight (int w) { return juce::jlimit (38, 120, juce::roundToInt ((float) w * 0.72f)); }
inline int scribbleBarHeight (int w) { return juce::jlimit (3, 8, w / 12); }
/** The black window has room above the LCD, the light bar in its middle. */
inline int scribbleBarRow (int w) { return juce::jlimit (scribbleBarHeight (w) + 12, scribbleBarHeight (w) + 36, juce::roundToInt ((float) w * 0.4f)); }
inline int scribbleWindowHeight (int w) { return scribbleBarRow (w) + scribbleLcdHeight (w) + 5; }
inline int gapUnderScribble (int w) { return juce::jlimit (8, 36, w * 3 / 10); }
/** How far below a strip's top its SELECT key starts. */
inline int stripTopRowsHeight (int w) { return 3 + scribbleWindowHeight (w) + gapUnderScribble (w); }

/** The fader strips' width for a bottom row `height` px tall: at most about
    0.15 of it (70 .. 120 px), at least 40. */
inline int clampStripWidth (int stripW, int height) { return juce::jlimit (40, juce::jlimit (70, 120, height * 15 / 100), stripW); }

//==============================================================================
/** The WING COMPACT's surface (photos of a WING COMPACT; WING remote
    protocols, "Wing, a family"):

      +---------------------------------------+------------------+
      | screen keys | SCREEN | knob, CLR SOLO | logo, USB AUDIO, |
      |      the six encoders under it        | MONITORING       |
      +--------+------+------+------+---------+--+---------------+
      | VIEW   | 4 x  | 4 x  | 4 x  | BUS  DCA  VIEW  |
      | 1-12   |strips|strips|strips| MAIN USER       |
      | ...    |      |      |      | LCD       fader |
      | USER 2 |      |      |      | 1  2       13   |
      | <4 4>  |      |      |      | .. 16           |
      +--------+------+------+------+-----------------+

    Top: the display unit and, at its right, the panel with the logo, USB
    AUDIO/DATA and MONITORING/TALKBACK. Bottom: one fader section of 12
    (three modules of four) with the nine layer keys in its column, then the
    key section: BUS, DCA, MAIN MATRIX and USER (what the 16 keys do), VIEW,
    the small LCD, the 16 keys in two columns of eight and fader 13. */
namespace compact
{
    constexpr int kFaders = 12;
    constexpr int kNumKeys = 16;

    /** The key section's keys for strips `stripW` wide. */
    inline int keyWidth (int stripW) { return juce::jlimit (30, 64, stripW * 4 / 5); }
    /** The key section: two key columns and fader 13, with margins. */
    inline int keySectionWidth (int stripW) { return 2 * keyWidth (stripW) + stripW + 30; }
    /** The monitoring panel right of the display unit. */
    inline int panelWidth (int width) { return juce::jlimit (220, 400, width * 18 / 100); }

    struct Surface
    {
        juce::Rectangle<int> display, panel, splitter, faders, keys;
        int stripW = 40;
    };

    /** The surface in `window` with a top row `topH` tall, the display unit
        wanting `displayWidth` (MainDisplay::preferredWidth at that height).
        Very wide windows keep the parts at their natural width, centred. */
    inline Surface plan (juce::Rectangle<int> window, int topH, int displayWidth)
    {
        Surface s;
        const int width = window.getWidth();
        const int bottomH = window.getHeight() - topH - 6;

        // 12 strips, the bank column and the key section (about 2.6 strips).
        int stripW = (width - kBankColumnWidth - 30) * 10 / (kFaders * 10 + 26);
        stripW = clampStripWidth (stripW, bottomH);
        while (stripW > 40 && kBankColumnWidth + kFaders * stripW + keySectionWidth (stripW) > width)
            --stripW;
        s.stripW = stripW;
        const int fadersW = kBankColumnWidth + kFaders * stripW, keysW = keySectionWidth (stripW);
        const int panelW = panelWidth (width);

        const int consoleW = juce::jmin (width, juce::jmax (fadersW + keysW, displayWidth + panelW));
        auto area = window.withSizeKeepingCentre (consoleW, window.getHeight());
        auto top = area.removeFromTop (topH);
        s.splitter = area.removeFromTop (6);
        s.panel = top.removeFromRight (juce::jmin (panelW, top.getWidth() / 2));
        s.display = top.withSizeKeepingCentre (juce::jmin (top.getWidth(), displayWidth), top.getHeight());

        auto row = area.withSizeKeepingCentre (juce::jmin (area.getWidth(), fadersW + keysW), area.getHeight());
        s.faders = row.removeFromLeft (juce::jmin (fadersW, row.getWidth()));
        s.keys = row;
        return s;
    }

    /** The key section's parts, in its own coordinates. */
    struct KeySection
    {
        std::array<juce::Rectangle<int>, 4> modes; // BUS, DCA, MAIN MATRIX, USER
        juce::Rectangle<int> view, lcd, strip;
        std::array<juce::Rectangle<int>, kNumKeys> keys; // 1 2 / 3 4 / ... (odd keys at the left)
    };

    /** `bounds` (local), the fader strips `stripW` wide: the mode keys and
        VIEW level with the scribble strips, the LCD from the SELECT row down,
        the 16 keys under it; fader 13 (without a scribble strip) at the
        right, its SELECT level with the strips'. */
    inline KeySection planKeys (juce::Rectangle<int> bounds, int stripW)
    {
        KeySection k;
        const int topRows = stripTopRowsHeight (stripW);
        auto b = bounds;
        auto stripColumn = b.removeFromRight (stripW + 6).withTrimmedRight (6);
        // The strip starts 3 px above its SELECT key.
        k.strip = stripColumn.withTrimmedTop (juce::jmin (stripColumn.getHeight(), topRows - 3));

        auto c = b.reduced (10, 0).withTrimmedRight (4);
        const int keyW = juce::jmin (keyWidth (stripW), (c.getWidth() - 8) / 2);
        const int colX[2] = { c.getX(), c.getRight() - keyW };

        // The band level with the scribble strips: two rows of mode keys, VIEW
        // over fader 13.
        auto band = c.withHeight (juce::jmax (0, topRows - 6)).withTrimmedTop (6);
        const int modeH = juce::jlimit (12, 34, juce::jmin (band.getHeight() / 2 - 6, keyW * 3 / 4));
        const int rowPitch = band.getHeight() / 2;
        for (int i = 0; i < 4; ++i)
        {
            const int cy = band.getY() + rowPitch * (i / 2) + rowPitch / 2;
            k.modes[(size_t) i] = juce::Rectangle<int> (colX[i % 2], cy - modeH / 2, keyW, modeH);
        }
        const int viewSize = juce::jlimit (16, 40, juce::jmin (stripW * 2 / 3, band.getHeight() - 8));
        k.view = juce::Rectangle<int> (viewSize, viewSize).withCentre ({ stripColumn.getCentreX(), band.getCentreY() });

        // Under the band: the LCD, then the keys in rows of two.
        auto rest = c.withTop (topRows).withTrimmedBottom (6);
        k.lcd = rest.removeFromTop (juce::jlimit (juce::jmin (40, rest.getHeight() / 3), 140, rest.getHeight() * 28 / 100));
        rest.removeFromTop (juce::jmin (8, rest.getHeight()));
        const int pitch = rest.getHeight() / (kNumKeys / 2);
        const int keyH = juce::jmax (1, juce::jmin (keyW * 2 / 3, pitch - juce::jmax (3, pitch / 4)));
        for (int i = 0; i < kNumKeys; ++i)
        {
            const int cy = rest.getY() + pitch * (i / 2) + pitch / 2;
            k.keys[(size_t) i] = juce::Rectangle<int> (colX[i % 2], cy - keyH / 2, keyW, keyH);
        }
        return k;
    }

    /** Fader 13's assignment (/$ctl/cfg/mfdr): OFF, MAIN.1-4, MTX.1-8,
        DCA.1-16, CH.1-40, AUX.1-8, BUS.1-16 (the protocol's list; CH goes as
        far as the console has channels). */
    struct MainFader
    {
        enum Group
        {
            Off,
            Main,
            Matrix,
            Dca,
            Channel,
            Aux,
            Bus
        };
        Group group = Main;
        int number = 1; // 1-based

        bool operator== (const MainFader& o) const noexcept { return group == o.group && (group == Off || number == o.number); }
    };

    /** The groups' tokens ("OFF", "MAIN", "MTX", "DCA", "CH", "AUX", "BUS") and sizes. */
    inline const char* groupToken (MainFader::Group g)
    {
        static const char* const tokens[] = { "OFF", "MAIN", "MTX", "DCA", "CH", "AUX", "BUS" };
        return tokens[juce::jlimit (0, 6, (int) g)];
    }

    inline int groupSize (MainFader::Group g)
    {
        static const int sizes[] = { 1, 4, 8, 16, 40, 8, 16 };
        return sizes[juce::jlimit (0, 6, (int) g)];
    }

    /** "MAIN.1" (the default for anything it can't read), "DCA.3", "OFF". */
    inline MainFader parseMainFader (const juce::String& token)
    {
        const auto t = token.trim().toUpperCase();
        if (t == "OFF")
            return { MainFader::Off, 1 };
        for (int g = MainFader::Main; g <= MainFader::Bus; ++g)
        {
            const auto group = static_cast<MainFader::Group> (g);
            const auto prefix = juce::String (groupToken (group)) + ".";
            if (t.startsWith (prefix))
            {
                const int n = t.fromFirstOccurrenceOf (".", false, false).getIntValue();
                if (n >= 1 && n <= groupSize (group))
                    return { group, n };
            }
        }
        return {};
    }

    inline juce::String mainFaderToken (const MainFader& f)
    {
        return f.group == MainFader::Off ? juce::String ("OFF") : juce::String (groupToken (f.group)) + "." + juce::String (f.number);
    }

    /** "MAIN 1", "DCA 3", "OFF": as the LCD and the menu show it. */
    inline juce::String mainFaderName (const MainFader& f) { return mainFaderToken (f).replaceCharacter ('.', ' '); }

    /** What the 16 keys do (/$ctl/cfg/cscmode, the keys BUS, DCA, MAIN MATRIX and USER). */
    enum class KeyMode
    {
        Bus,
        Dca,
        Main,
        User
    };

    inline KeyMode keyModeFromToken (const juce::String& t)
    {
        return t == "DCA" ? KeyMode::Dca : t == "MAIN" ? KeyMode::Main : t == "USER" ? KeyMode::User : KeyMode::Bus;
    }

    /** What key `key` (0-based) puts on fader 13 when pressed [WING COMPACT]:
        in BUS mode BUS 1-16, in MAIN MATRIX mode MAIN 1-4 on keys 1-4 and
        MATRIX 1-8 on keys 9-16 (keys 5-8 do nothing); nothing in DCA and
        USER mode. */
    inline std::optional<MainFader> keyMainFader (KeyMode m, int key)
    {
        if (m == KeyMode::Bus && key >= 0 && key < 16)
            return MainFader { MainFader::Bus, key + 1 };
        if (m == KeyMode::Main && key >= 0 && key < 4)
            return MainFader { MainFader::Main, key + 1 };
        if (m == KeyMode::Main && key >= 8 && key < 16)
            return MainFader { MainFader::Matrix, key - 7 };
        return std::nullopt;
    }

    inline juce::String keyModeToken (KeyMode m)
    {
        return m == KeyMode::Dca ? "DCA" : m == KeyMode::Main ? "MAIN" : m == KeyMode::User ? "USER" : "BUS";
    }

    /** The COMPACT's custom control key sets (/$ctl/user/U1-U4, MM, D1-D4;
        WING manual 4.8), counted from USER 1-4. */
    constexpr int kSetMainMatrix = 4, kFirstDcaSet = 5;

    /** A key's custom control: the set and the control in it (0-3). */
    struct KeyControl
    {
        int set = 0, control = 0;
        bool operator== (const KeyControl& o) const noexcept { return set == o.set && control == o.control; }
    };

    /** The custom control key `key` (0-based) plays, if any [WING manual 4.2]:
        in USER mode the sets U1-U4 (keys 1-4 U1, ... 13-16 U4); in MAIN MATRIX
        mode keys 5-8, between MAIN 1-4 and MATRIX 1-8, the set M/M; in DCA
        mode with USE DCA BUTTONS 1-16 AS CC (`dcaCustom`) the sets D1-D4. */
    inline std::optional<KeyControl> keyControl (KeyMode m, int key, bool dcaCustom)
    {
        if (key < 0 || key >= kNumKeys)
            return std::nullopt;
        if (m == KeyMode::User)
            return KeyControl { key / 4, key % 4 };
        if (m == KeyMode::Main && key >= 4 && key < 8)
            return KeyControl { kSetMainMatrix, key - 4 };
        if (m == KeyMode::Dca && dcaCustom)
            return KeyControl { kFirstDcaSet + key / 4, key % 4 };
        return std::nullopt;
    }

    /** What a MAIN or MATRIX key does besides putting its main or matrix on
        fader 13: spill it (the strips feeding it onto the faders) or Sends on
        Fader to it (the faders show the channels' sends to it). */
    enum class KeyAction
    {
        None,
        Spill,
        Sends
    };

    struct KeyActions
    {
        KeyAction press = KeyAction::None, hold = KeyAction::None;
    };

    /** A MAIN MATRIX mode key's press and hold. MAIN SPILL / MATRIX SPILL
        (/$ctl/cfg/mainspill, mtxspill; WING manual 5.5) choose which of the two
        spills: off, a press sends (SOF) and a hold spills; on, the other way
        round (WING remote protocols). The factory setting is off for the mains
        (press: SOF to the main, as the manual's 4.2 says; hold: spill) and on
        for the matrices (hold: SOF to the matrix [a WING COMPACT's owner]).
        Sidecar has no matrix spill: a MATRIX key's spill does nothing. */
    inline KeyActions mainMatrixActions (int key, bool mainSpill, bool matrixSpill)
    {
        if (key >= 0 && key < 4)
            return mainSpill ? KeyActions { KeyAction::Spill, KeyAction::Sends } : KeyActions { KeyAction::Sends, KeyAction::Spill };
        if (key >= 8 && key < kNumKeys)
            return matrixSpill ? KeyActions { KeyAction::None, KeyAction::Sends } : KeyActions { KeyAction::Sends, KeyAction::None };
        return {};
    }
} // namespace compact

} // namespace livedaw::ui::layout
