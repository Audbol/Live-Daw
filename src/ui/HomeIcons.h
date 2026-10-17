#pragma once

#include "ui/Icons.h"
#include "ui/UiContext.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

/* The WING's channel icons (the /ch/n/icon numbers, named as in the
   Companion module), their categories on HOME > ICON/COLOR and the name
   suggestions HOME > NAME offers for them (manual p31: "After an icon has
   been selected, multiple name suggestions are displayed here"). */

namespace livedaw::ui::home
{

struct WingIconInfo
{
    int number;
    const char* name;
};

/** The seven categories of the icon gallery, in the order of its keys. */
struct WingIconCategory
{
    const char* name;
    std::vector<WingIconInfo> icons;
};

inline const std::vector<WingIconCategory>& wingIconCategories()
{
    static const std::vector<WingIconCategory> categories {
        { "GENERAL",
          { { 0, "Empty" }, { 1, "XLR Connector" }, { 2, "Jack Connector" }, { 3, "Mini Jack Connector" }, { 4, "RCA Connector" },
            { 5, "Fader" }, { 6, "FX" }, { 7, "Matrix" }, { 8, "Bass Clef" }, { 9, "Treble Clef" }, { 10, "Routing" }, { 11, "Bus" },
            { 12, "Sum" }, { 13, "Smile" }, { 14, "Wing Logo" } } },
        { "VOCALS AND MICS",
          { { 100, "Microphone" }, { 101, "Wireless Mic" }, { 102, "Handheld Mic" }, { 103, "Condenser Mic" }, { 104, "Boundary Mic" },
            { 105, "Lapel Mic" }, { 106, "Boundary Mic 2" }, { 107, "Headset Mic" }, { 108, "Overhead Mic" }, { 109, "Speaker" },
            { 110, "Studio Mic" }, { 111, "Podcast Mic" }, { 112, "Choir" }, { 113, "Female Vocal" }, { 114, "Male Vocal" } } },
        { "DRUMS AND PERCUSSION",
          { { 200, "Kick In" }, { 201, "Kick Out" }, { 202, "Snare Top" }, { 203, "Snare Bottom" }, { 204, "Snare Drum" },
            { 205, "Hi-Hat" }, { 206, "High Tom" }, { 207, "Mid Tom" }, { 208, "Low Tom" }, { 209, "Floor Tom" }, { 210, "Drum" },
            { 211, "Crash Cymbal" }, { 212, "Ride Cymbal" }, { 213, "Cowbell" }, { 214, "Tambourine" }, { 215, "Bongos" },
            { 216, "Conga" }, { 217, "Timpani" }, { 218, "Cajon" }, { 219, "Shaker" }, { 220, "Marimba" }, { 221, "Hang Drum" },
            { 222, "Triangle" }, { 223, "Drum Machine" }, { 224, "Claps" } } },
        { "STRINGS AND WINDS",
          { { 300, "E-Guitar 1" }, { 301, "E-Guitar 2" }, { 302, "Violin" }, { 303, "Oud" }, { 304, "Acoustic Guitar" },
            { 305, "E-Guitar 3" }, { 306, "E-Guitar 4" }, { 307, "E-Guitar 5" }, { 308, "Double Bass" }, { 309, "Viola" },
            { 310, "Cello" }, { 311, "Baritone" }, { 312, "Saxophone" }, { 313, "Trombone" }, { 314, "Trumpet" }, { 315, "Harp" },
            { 316, "Organ" }, { 317, "Harmonica" }, { 318, "Flute" }, { 319, "Clarinet" } } },
        { "KEYS",
          { { 400, "Grand Piano" }, { 401, "Upright Piano" }, { 402, "Synthesizer 1" }, { 403, "Synthesizer 2" },
            { 404, "Synthesizer 3" }, { 405, "Synthesizer 4" }, { 406, "Keytar" }, { 407, "Piano Stand" }, { 408, "Cross Stand" },
            { 409, "Hammond Organ" } } },
        { "SPEAKERS",
          { { 500, "Full Amp" }, { 501, "2x2 Cabinet" }, { 502, "Cabinet" }, { 503, "Speaker" }, { 504, "Speaker Pair" },
            { 505, "Speaker + Mic" }, { 506, "Subwoofer" }, { 507, "Studio Monitor" }, { 508, "Studio Monitor 2" },
            { 509, "Column Speaker" }, { 510, "Wall Speaker" }, { 511, "Hanging Speaker" }, { 512, "Speaker Stand" },
            { 513, "Stand Pair" }, { 514, "Delay Speakers" }, { 515, "Left Monitor" }, { 516, "Right Monitor" }, { 517, "Monitor" },
            { 518, "Large Monitor" }, { 519, "Large Monitor Pair" }, { 520, "Left Wedge" }, { 521, "Right Wedge" },
            { 522, "Center Wedge" }, { 523, "Line Array" }, { 524, "Ceiling Speaker" } } },
        { "SPECIALS",
          { { 600, "Hand" }, { 601, "Ear" }, { 602, "Headphones" }, { 603, "Speaker A" }, { 604, "Speaker B" }, { 605, "Laptop" },
            { 606, "MP3 Player" }, { 607, "Smartphone" }, { 608, "USB Drive" }, { 609, "SD Card" }, { 610, "CD" },
            { 611, "Turntable" }, { 612, "Reel-to-Reel" }, { 613, "Cassette" }, { 614, "Patchbay" } } },
    };
    return categories;
}

/** The gallery category of an icon number (0-6), or 0. */
inline int wingIconCategory (int icon)
{
    const int c = icon / 100;
    return c >= 0 && c <= 6 ? c : 0;
}

inline juce::String wingIconName (int icon)
{
    for (auto& category : wingIconCategories())
        for (auto& i : category.icons)
            if (i.number == icon)
                return i.name;
    return {};
}

/** HOME > NAME: up to five names for a channel with this icon. */
inline juce::StringArray nameSuggestions (int icon)
{
    switch (icon)
    {
        case 6:   return { "EFFECTS", "FX", "REVERB", "DELAY", "VERB" };
        case 8:   return { "BASS", "BASS DI", "BASS AMP", "BASS SYN", "SUB" };
        case 9:   return { "LEAD", "MELODY", "SOLO", "KEYS", "STRINGS" };
        case 11:  return { "BUS", "GROUP", "SUBGROUP", "STEM", "MIX" };
        case 12:  return { "SUM", "MAIN", "MONO", "LR", "MIX" };
        case 112: return { "CHOIR", "CHOIR L", "CHOIR R", "BGV", "ENSEMBLE" };
        case 113: return { "VOCAL", "VOX", "LEAD VOX", "BGV", "FEMALE" };
        case 114: return { "VOCAL", "VOX", "LEAD VOX", "BGV", "MALE" };
        case 107: return { "HEADSET", "HS", "PASTOR", "PRESENTER", "VOX" };
        case 105: return { "LAPEL", "LAV", "SPEECH", "HOST", "GUEST" };
        case 108: return { "OH", "OH L", "OH R", "OVERHEAD", "ROOM" };
        case 200: return { "KICK IN", "KICK", "BD", "BASSDRUM", "KICK SUB" };
        case 201: return { "KICK OUT", "KICK", "BD", "BASSDRUM", "KICK SUB" };
        case 202: return { "SNARE TOP", "SN TOP", "SNARE", "SD", "SNR" };
        case 203: return { "SNARE BOT", "SN BOT", "SNARE", "SD", "SNR" };
        case 204: return { "SNARE", "SD", "SNR", "SN TOP", "SN BOT" };
        case 205: return { "HIHAT", "HH", "HI-HAT", "HAT", "CYMBAL" };
        case 206: return { "TOM 1", "HI TOM", "RACK TOM", "TOM", "T1" };
        case 207: return { "TOM 2", "MID TOM", "RACK TOM", "TOM", "T2" };
        case 208: return { "TOM 3", "LO TOM", "TOM", "T3", "FLOOR" };
        case 209: return { "FLOOR TOM", "FTOM", "FLOOR", "TOM", "T4" };
        case 211: return { "CRASH", "CYMBAL", "OH L", "OH R", "OH" };
        case 212: return { "RIDE", "CYMBAL", "OH", "OH L", "OH R" };
        case 304: return { "AC GTR", "ACOUSTIC", "GTR", "GUITAR", "AGTR" };
        case 308: return { "BASS", "UPRIGHT", "DBL BASS", "CONTRA", "BASS DI" };
        case 312: return { "SAX", "ALTO SAX", "TENOR SAX", "HORNS", "WIND" };
        case 313: return { "TROMBONE", "TBN", "HORNS", "BRASS", "WIND" };
        case 314: return { "TRUMPET", "TPT", "HORNS", "BRASS", "WIND" };
        case 316:
        case 409: return { "ORGAN", "HAMMOND", "B3", "LESLIE", "KEYS" };
        case 400:
        case 401: return { "PIANO", "PNO", "GRAND", "PIANO L", "PIANO R" };
        case 506: return { "SUB", "SUBS", "LFE", "SUB L", "SUB R" };
        case 520:
        case 521:
        case 522: return { "WEDGE", "MON", "MONITOR", "WEDGE L", "WEDGE R" };
        case 523: return { "PA", "LINE ARRAY", "MAIN", "PA L", "PA R" };
        case 602: return { "IEM", "PHONES", "IEM L", "IEM R", "CUE" };
        case 605: return { "LAPTOP", "PLAYBACK", "PC", "MAC", "TRACKS" };
        case 606:
        case 607: return { "PLAYBACK", "MUSIC", "BGM", "PHONE", "MP3" };
        case 611: return { "DJ", "TURNTABLE", "DECK", "VINYL", "DJ L" };
        default:  break;
    }
    switch (wingIconCategory (icon))
    {
        case 1:  return { "VOCAL", "VOX", "LEAD VOX", "BGV", "MIC" };
        case 2:  return { "DRUMS", "PERC", "KIT", "DRUM", "SAMPLE" };
        case 3:  return { "GUITAR", "GTR", "E-GTR", "GTR L", "GTR R" };
        case 4:  return { "KEYS", "SYNTH", "PIANO", "KEYS L", "KEYS R" };
        case 5:  return { "MONITOR", "MON", "PA", "FILL", "DELAY" };
        case 6:  return { "PLAYBACK", "TRACKS", "MUSIC", "CLICK", "TALKBACK" };
        default: return {};
    }
}

namespace detail
{
    inline void strokeIcon (juce::Graphics& g, const juce::Path& p, juce::Rectangle<float> area, juce::Colour c)
    {
        auto path = p;
        path.applyTransform (juce::AffineTransform::scale (area.getWidth() / 24.0f, area.getHeight() / 24.0f).translated (area.getX(), area.getY()));
        g.setColour (c);
        g.strokePath (path, juce::PathStrokeType (juce::jmax (1.2f, area.getWidth() / 16.0f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    /** A handheld microphone: the grille, the handle. */
    inline juce::Path micPath()
    {
        juce::Path p;
        p.addEllipse (13.0f, 3.0f, 8.0f, 8.0f);
        p.startNewSubPath (14.5f, 10.0f);
        p.lineTo (4.0f, 20.5f);
        p.lineTo (5.5f, 22.0f);
        p.lineTo (16.0f, 11.5f);
        return p;
    }

    /** A drum: the shell and the two heads, two sticks. */
    inline juce::Path drumPath()
    {
        juce::Path p;
        p.addEllipse (3.0f, 9.0f, 18.0f, 6.0f);
        p.startNewSubPath (3.0f, 12.0f);
        p.lineTo (3.0f, 18.0f);
        p.addCentredArc (12.0f, 18.0f, 9.0f, 3.0f, 0.0f, -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi);
        p.lineTo (21.0f, 12.0f);
        p.startNewSubPath (6.0f, 3.0f);
        p.lineTo (11.0f, 10.0f);
        p.startNewSubPath (18.0f, 3.0f);
        p.lineTo (13.0f, 10.0f);
        return p;
    }

    /** A guitar: the body, the neck, the head. */
    inline juce::Path guitarPath()
    {
        juce::Path p;
        p.addEllipse (3.0f, 13.0f, 9.0f, 9.0f);
        p.addEllipse (6.0f, 10.0f, 6.0f, 6.0f);
        p.startNewSubPath (10.0f, 14.0f);
        p.lineTo (19.0f, 5.0f);
        p.addRectangle (18.0f, 2.0f, 4.0f, 4.0f);
        return p;
    }

    /** A keyboard on a stand. */
    inline juce::Path keysPath()
    {
        juce::Path p;
        p.addRectangle (2.0f, 7.0f, 20.0f, 9.0f);
        for (int i = 1; i < 7; ++i)
        {
            p.startNewSubPath (2.0f + (float) i * 20.0f / 7.0f, 11.0f);
            p.lineTo (2.0f + (float) i * 20.0f / 7.0f, 16.0f);
        }
        p.startNewSubPath (5.0f, 16.0f);
        p.lineTo (8.0f, 22.0f);
        p.startNewSubPath (19.0f, 16.0f);
        p.lineTo (16.0f, 22.0f);
        return p;
    }

    inline juce::Path smilePath()
    {
        juce::Path p;
        p.addEllipse (3.0f, 3.0f, 18.0f, 18.0f);
        p.addEllipse (8.5f, 8.5f, 1.5f, 1.5f);
        p.addEllipse (14.0f, 8.5f, 1.5f, 1.5f);
        p.addCentredArc (12.0f, 12.5f, 5.0f, 4.0f, 0.0f, juce::MathConstants<float>::pi * 0.6f, juce::MathConstants<float>::pi * 1.4f, true);
        return p;
    }

    inline juce::Path notePath()
    {
        juce::Path p;
        p.addEllipse (5.0f, 15.0f, 7.0f, 5.0f);
        p.startNewSubPath (12.0f, 17.5f);
        p.lineTo (12.0f, 3.0f);
        p.lineTo (18.0f, 7.0f);
        return p;
    }

    inline juce::Path discPath()
    {
        juce::Path p;
        p.addEllipse (2.0f, 2.0f, 20.0f, 20.0f);
        p.addEllipse (9.0f, 9.0f, 6.0f, 6.0f);
        return p;
    }

    inline void text (juce::Graphics& g, const juce::String& t, juce::Rectangle<float> area, juce::Colour c, bool italic = false)
    {
        g.setColour (c);
        g.setFont (juce::FontOptions (area.getHeight() * 0.62f, italic ? juce::Font::bold | juce::Font::italic : juce::Font::bold));
        g.drawFittedText (t, area.toNearestInt(), juce::Justification::centred, 1, 0.8f);
    }
} // namespace detail

/** Draws a WING channel icon (white line art), with the strip kind's icon for
    "Empty". Group A's Icons (drawWingIcon) will replace these placeholder
    glyphs, which only tell the categories apart. */
inline void drawChannelIcon (juce::Graphics& g, int icon, StripKind kind, juce::Rectangle<float> area, juce::Colour c)
{
    area = area.withSizeKeepingCentre (juce::jmin (area.getWidth(), area.getHeight()), juce::jmin (area.getWidth(), area.getHeight()));
    switch (icon)
    {
        case 0:  drawIcon (g, stripIcon (kind), area, c); return;
        case 1:  drawIcon (g, Icon::Xlr, area, c); return;
        case 2:
        case 3:
        case 4:  drawIcon (g, Icon::Jack, area, c); return;
        case 5:  drawIcon (g, Icon::Faders, area, c); return;
        case 6:  detail::text (g, "FX", area, c, true); return;
        case 7:  drawIcon (g, Icon::Matrix, area, c); return;
        case 8:
        case 9:  detail::strokeIcon (g, detail::notePath(), area, c); return;
        case 10: drawIcon (g, Icon::SourcesIn, area, c); return;
        case 11: drawIcon (g, Icon::Bus, area, c); return;
        case 12: detail::text (g, juce::String::fromUTF8 ("\xce\xa3"), area, c); return;
        case 13: detail::strokeIcon (g, detail::smilePath(), area, c); return;
        case 14: detail::text (g, "W", area, c); return;
        case 109: drawIcon (g, Icon::Speaker, area, c); return;
        case 602: drawIcon (g, Icon::Headphones, area, c); return;
        case 603:
        case 604: drawIcon (g, Icon::Speaker, area, c); return;
        case 608: drawIcon (g, Icon::UsbStick, area, c); return;
        case 609: drawIcon (g, Icon::SdCard, area, c); return;
        case 610:
        case 611: detail::strokeIcon (g, detail::discPath(), area, c); return;
        case 614: drawIcon (g, Icon::Link, area, c); return;
        default: break;
    }
    switch (wingIconCategory (icon))
    {
        case 1:  detail::strokeIcon (g, detail::micPath(), area, c); return;
        case 2:  detail::strokeIcon (g, detail::drumPath(), area, c); return;
        case 3:  detail::strokeIcon (g, detail::guitarPath(), area, c); return;
        case 4:  detail::strokeIcon (g, detail::keysPath(), area, c); return;
        case 5:  drawIcon (g, icon == 504 || icon == 513 || icon == 519 ? Icon::Speakers : Icon::Speaker, area, c); return;
        case 6:  drawIcon (g, Icon::Disk, area, c); return;
        default: drawIcon (g, stripIcon (kind), area, c); return;
    }
}

/** The category keys' glyphs (XLR, mic, drum, guitar, keys, speaker, gear). */
inline void drawIconCategory (juce::Graphics& g, int category, juce::Rectangle<float> area, juce::Colour c)
{
    static const int sample[] = { 1, 100, 210, 300, 400, 503, -1 };
    if (category == 6)
        drawIcon (g, Icon::Gear, area, c);
    else
        drawChannelIcon (g, sample[juce::jlimit (0, 5, category)], StripKind::Channel, area, c);
}

} // namespace livedaw::ui::home
