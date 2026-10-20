#pragma once

#include "ui/Icons.h"
#include "ui/UiContext.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

/* The WING's channel icons on HOME: the gallery's categories (the icon set and
   its names are Icons' drawWingIcon / wingIconName / wingIconNumbers) and the
   name suggestions HOME > NAME offers for an icon (manual p31: "After an icon
   has been selected, multiple name suggestions are displayed here"). */

namespace livedaw::ui::home
{

struct WingIconInfo
{
    int number;
    juce::String name;
};

/** The seven categories of the icon gallery, in the order of its keys. */
struct WingIconCategory
{
    const char* name;
    std::vector<WingIconInfo> icons;
};

inline const std::vector<WingIconCategory>& wingIconCategories()
{
    static const std::vector<WingIconCategory> categories = []
    {
        static const char* names[] = { "GENERAL", "VOCALS AND MICS", "DRUMS AND PERCUSSION", "STRINGS AND WINDS", "KEYS", "SPEAKERS", "SPECIALS" };
        std::vector<WingIconCategory> list;
        for (int c = 0; c < 7; ++c)
        {
            WingIconCategory category { names[c], {} };
            // The general category starts with "Empty" (no icon).
            if (c == 0)
                category.icons.push_back ({ 0, "Empty" });
            for (int n : wingIconNumbers (c))
                if (n != 0)
                    category.icons.push_back ({ n, wingIconName (n) });
            list.push_back (std::move (category));
        }
        return list;
    }();
    return categories;
}

/** The gallery category of an icon number (0-6), or 0. */
inline int wingIconCategory (int icon)
{
    const int c = icon / 100;
    return c >= 0 && c <= 6 ? c : 0;
}

/** HOME > NAME: up to five names for a channel with this icon. */
inline juce::StringArray nameSuggestions (int icon)
{
    switch (icon)
    {
        case 1:   return { "XLR", "LINE" };
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

/** Draws a WING channel icon (white line art), or the strip kind's icon for
    "Empty" and numbers the set doesn't know. */
inline void drawChannelIcon (juce::Graphics& g, int icon, StripKind kind, juce::Rectangle<float> area, juce::Colour c)
{
    area = area.withSizeKeepingCentre (juce::jmin (area.getWidth(), area.getHeight()), juce::jmin (area.getWidth(), area.getHeight()));
    if (! drawWingIcon (g, icon, area, c))
        drawIcon (g, stripIcon (kind), area, c);
}

/** The category keys' glyphs (I13: XLR, mic, drum, guitar, keys, speaker, gear). */
inline void drawIconCategory (juce::Graphics& g, int category, juce::Rectangle<float> area, juce::Colour c)
{
    static const int sample[] = { 1, 102, 210, 300, 402, 503 };
    if (category >= 6)
        drawIcon (g, Icon::Gear, area, c);
    else
        drawChannelIcon (g, sample[juce::jlimit (0, 5, category)], StripKind::Channel, area, c);
}

} // namespace livedaw::ui::home
