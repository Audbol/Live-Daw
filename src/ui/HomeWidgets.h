#pragma once

#include "dsp/DspUtils.h"
#include "ui/DisplayPage.h"
#include "ui/DynamicsGraph.h"
#include "ui/Faceplate.h"
#include "ui/HomeIcons.h"
#include "ui/Icons.h"
#include "ui/Knobs.h"
#include "ui/ScreenWidgets.h"
#include "ui/UiContext.h"

#include <array>
#include <cmath>
#include <functional>

/* Widgets the HOME screens share, drawn after the WING's Channel Home and
   processing-slot screens (manual 5.1 and chapter 6). */

namespace livedaw::ui::home
{

//==============================================================================
// Colours of the HOME screens: the shared firmware 3.1 tokens (Colours,
// KnobColours) under the names these screens use, and the few colours only
// HOME has.
namespace colours
{
    const juce::Colour eqOn = Colours::valueBlue;   // block keys: EQ on; LC / HC chips
    const juce::Colour dynOn { 0xffe07a22 };        // dynamics on
    const juce::Colour insOn { 0xff9a2cf0 };        // inserts on
    const juce::Colour fader = Colours::valueBlue;  // screen faders, levels
    const juce::Colour tap = Colours::tapRed;       // TAP / PRE sends, the TAP POINT node
    const juce::Colour post = Colours::green;       // POST sends
    const juce::Colour group = Colours::valueBlue;  // GROUP sends
    const juce::Colour stereo { 0xff809800 };       // stereo source label (olive)
    const juce::Colour mono = Colours::valueBlue;   // mono source label
    const juce::Colour soloBus = Colours::green;    // SOLO BUS keys
    const juce::Colour panArc = Colours::width;     // pan / width arc
    const juce::Colour link = Colours::green;       // PAN LINK while linked
    const juce::Colour keyOff = Colours::screenKey; // unlit keys, the front tab
    const juce::Colour keyFrame = Colours::screenHeader; // the frame of the block keys
    const juce::Colour thumb = Colours::screenTile; // block thumbnails
    const juce::Colour well = Colours::screenWell;  // dark wells (fader slots, meters, list rows)
    const juce::Colour chipOff = Colours::chipOff;
    const juce::Colour feedOn = Colours::green;     // feed grid cell: sends to this bus

    const juce::Colour panel = Colours::screenPanel;  // panel header and body
    const juce::Colour tab = Colours::screenHeader;   // unselected tabs
    const juce::Colour outline = Colours::screenLine; // key and field outlines
    const juce::Colour dim = Colours::textDim;        // unlit or dim text
    const juce::Colour caption = Colours::caption;    // titles, captions
    const juce::Colour ink = Colours::text;           // white text
    const juce::Colour accent = Colours::gold;        // ON keys, selections, the front tab's text
    const juce::Colour choice = Colours::choice;      // selected choice key, selected EQ band
    const juce::Colour alt = Colours::alt;            // ALT input
    const juce::Colour mute = Colours::mute;          // MUTE lit
    const juce::Colour tilt = KnobColours::cyan;      // tool filters: TILT
    const juce::Colour maxer = KnobColours::orange;   //               MAXER
    const juce::Colour allPass = KnobColours::yellow; //               AP 90 / AP 180
    const juce::Colour eqCurve = Colours::amber;      // EQ curve
    const juce::Colour eqFill = Colours::eqFill;      // and the area under it
    const juce::Colour dcaTag { 0xff203860 };       // TAGS: DCA strips (unassigned)
    const juce::Colour muteTag { 0xff680008 };      //       mute group strips
    const juce::Colour talkOff { 0xff005c18 };      //       TALK tags
} // namespace colours

/** A screen font for HOME: the console's type scale (TypeScale), no taller
    than `maxHeightPx` so it fits small keys. */
inline juce::Font homeFont (float capFraction, float maxHeightPx, WingWeight weight = WingWeight::Medium)
{
    auto f = wingFont (capFraction, weight);
    return f.getHeight() > maxHeightPx ? f.withHeight (juce::jmax (6.0f, maxHeightPx)) : f;
}

//==============================================================================
// Text as the WING writes it.

/** "111.7 Hz", "1k25 Hz", "10k02 Hz": kHz use the k as the decimal point. */
inline juce::String wingHz (float hz, bool unit = true)
{
    juce::String t;
    if (hz < 1000.0f)
    {
        t = juce::String (hz, 1);
    }
    else
    {
        const int tens = juce::roundToInt (hz / 10.0f); // 10 Hz steps
        const int whole = tens / 100, frac = tens % 100;
        t = juce::String (whole) + "k" + (frac % 10 == 0 ? juce::String (frac / 10) : juce::String (frac).paddedLeft ('0', 2));
    }
    return unit ? t + " Hz" : t;
}

/** "0.0", "-6.4", "-oo" */
inline juce::String wingDb (float db)
{
    if (db <= -89.9f)
        return "-oo";
    return juce::String (db, 1);
}

/** The short name the WING shows for a strip: "1", "A1", "B7", "M1", "MX1". */
inline juce::String shortPrefix (StripKind k)
{
    switch (k)
    {
        case StripKind::Channel: return {};
        case StripKind::Aux:     return "A";
        case StripKind::Bus:     return "B";
        case StripKind::Main:    return "M";
        case StripKind::Matrix:  return "MX";
    }
    return {};
}

inline juce::String shortLabel (const Strip& s) { return shortPrefix (s.getKind()) + juce::String (s.getNumber()); }

/** True when the strip still has its default name ("BUS 7"). */
inline bool hasDefaultName (const Strip& s) { return stripName (s) == s.getLabel(); }

/** The TAP point names (manual 6.1), for the engine's tap list. */
inline juce::StringArray tapPointNames()
{
    return { "INPUT", "FILTER", "TAP 3", "TAP 4", "TAP 5", "PRE FDR", "POST FDR", "POST PROC" };
}

/** The tool filters' names (FILTER panel key 3, FILTER SETUP). */
inline juce::String toolFilterName (int model)
{
    const auto token = models::model (models::FilterTools, model).token;
    if (token == "TILT") return "TILT EQ";
    if (token == "MAX")  return "MAXER";
    if (token == "AP1")  return "AP 90";
    if (token == "AP2")  return "AP 180";
    return token;
}

inline Icon toolFilterIcon (int model)
{
    const auto token = models::model (models::FilterTools, model).token;
    if (token == "MAX")  return Icon::Maxer;
    if (token == "AP1")  return Icon::AllPass90;
    if (token == "AP2")  return Icon::AllPass180;
    return Icon::Tilt;
}

/** The tool filter's colour (FILTER SETUP, the INPUT thumbnail's chip, the
    parameter bar): TILT cyan, MAXER orange, the all-pass filters yellow. */
inline juce::Colour toolFilterColour (int model)
{
    const auto token = models::model (models::FilterTools, model).token;
    if (token == "MAX")  return colours::maxer;
    if (token == "AP1" || token == "AP2") return colours::allPass;
    return colours::tilt;
}

/** The tool filter's short token on the INPUT thumbnail: TILT, MAX, AP1, AP2. */
inline juce::String toolFilterToken (int model)
{
    return models::model (models::FilterTools, model).token;
}

/** Send mode names: input sources send from the TAP point, buses and mains pre fader. */
inline juce::String sendModeName (int mode, bool inputSource, bool shortForm = false)
{
    if (mode == 0)
        return inputSource ? "TAP" : "PRE";
    if (mode == 1)
        return "POST";
    return shortForm ? "GRP" : "GROUP";
}

inline juce::Colour sendModeColour (int mode)
{
    return mode == 0 ? colours::tap : (mode == 1 ? colours::post : colours::group);
}

/** Whether the dynamics model is one of the WING's own (drawn as a transfer
    curve) rather than an emulated analog unit (drawn as its faceplate). */
inline bool isNativeDynamics (int model)
{
    return hasTransferGraph (model);
}

//==============================================================================
// Bindings with the WING's value texts.

inline KnobBinding wingFader (const juce::String& label, std::atomic<float>& v)
{
    auto b = bindFader (label, v);
    b.text = [&v] { return wingDb (v.load()) + " dB"; };
    return b;
}

inline KnobBinding wingPan (const juce::String& label, std::atomic<float>& v)
{
    auto b = bindLinear (label, v, -100.0f, 100.0f, {}, 0, 0.0f);
    b.text = [&v] { return juce::String (juce::roundToInt (v.load())); };
    return b;
}

inline KnobBinding wingWidth (const juce::String& label, std::atomic<float>& v)
{
    return bindLinear (label, v, -150.0f, 150.0f, " %", 0, 100.0f);
}

inline KnobBinding wingTrim (const juce::String& label, std::atomic<float>& v)
{
    return bindLinear (label, v, -18.0f, 18.0f, " dB", 1, 0.0f);
}

inline KnobBinding wingFreq (const juce::String& label, std::atomic<float>& v, float min, float max, float def)
{
    auto b = bindLog (label, v, min, max, {}, def);
    b.text = [&v] { return wingHz (v.load()); };
    return b;
}

//==============================================================================
// Parameter bar styles.

inline KnobStyle styleOf (KnobStyle::Bar bar, juce::Colour c, juce::Component* target = nullptr, int segments = 0)
{
    KnobStyle s;
    s.bar = bar;
    s.colour = c;
    s.target = target;
    s.segments = segments;
    return s;
}

inline KnobStyle levelStyle (juce::Component* t = nullptr) { return styleOf (KnobStyle::Bar::Fill, KnobColours::blue, t); }
inline KnobStyle freqStyle (juce::Component* t = nullptr) { return styleOf (KnobStyle::Bar::Fill, KnobColours::blue, t); }
inline KnobStyle widthStyle (juce::Component* t = nullptr) { return styleOf (KnobStyle::Bar::Centre, KnobColours::green, t); }
inline KnobStyle thumbStyle (juce::Component* t = nullptr) { return styleOf (KnobStyle::Bar::Thumb, KnobColours::white, t); }
inline KnobStyle balStyle (juce::Component* t = nullptr) { return styleOf (KnobStyle::Bar::Thumb, KnobColours::blue, t); }
inline KnobStyle distStyle (juce::Component* t = nullptr) { return styleOf (KnobStyle::Bar::Fill, KnobColours::yellow, t); }
inline KnobStyle toolStyle (juce::Component* t = nullptr, juce::Colour c = colours::tilt) { return styleOf (KnobStyle::Bar::Centre, c, t); }

/** The 3.1 key: a flat fill inside a black frame (the frame shows on the grey
    panels, not on the black page). */
inline void paintKeyBody (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fill, juce::Colour frame = juce::Colours::black)
{
    g.setColour (frame);
    g.fillRoundedRectangle (r, 2.5f);
    const float inset = frame == juce::Colours::black ? juce::jlimit (1.0f, 2.0f, r.getHeight() * 0.04f) : 1.0f;
    g.setColour (fill.isTransparent() ? colours::panel : fill);
    g.fillRoundedRectangle (r.reduced (inset), 1.5f);
}

//==============================================================================
/** A screen key: grey with dim text, lit in `onColour` (black text) while
    `isOn` returns true. Text, an icon, an icon left of the text, or an icon
    above a small label. `outlined` keys (FREEZE, SPILL) show the panel through
    their frame while off. */
class IconKey final : public juce::TextButton, private juce::Timer
{
public:
    IconKey (const juce::String& keyText, std::optional<Icon> i, juce::Colour on = colours::accent) : juce::TextButton (keyText), icon (i), onColour (on)
    {
        startTimerHz (8);
    }

    std::function<bool()> isOn;
    bool iconAbove = false, outlined = false, twoLines = false, labelBeside = false;
    juce::Colour offColour = colours::keyOff;
    juce::Colour offInk = colours::dim;  // the text of an unlit key
    juce::Colour frame = juce::Colours::black;  // keyboard-style keys: #484848

    void setIcon (std::optional<Icon> i)
    {
        icon = i;
        repaint();
    }

    void setOnColour (juce::Colour c)
    {
        if (c != onColour)
        {
            onColour = c;
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const bool on = getToggleState();
        auto r = getLocalBounds().toFloat();
        if (labelBeside && icon.has_value())
        {
            // A square icon key with its label beside it on the panel
            // (the overview's LO CUT / HI CUT / TILT EQ, Fig. 21).
            auto keyArea = r.removeFromLeft (juce::jmin (r.getHeight(), r.getWidth() * 0.45f)).withSizeKeepingCentre (r.getHeight(), r.getHeight());
            auto fill = on ? onColour : offColour;
            if (down || (over && isEnabled()))
                fill = fill.brighter (down ? 0.15f : 0.07f);
            paintKeyBody (g, keyArea, fill, frame);
            drawIcon (g, *icon, keyArea.reduced (keyArea.getHeight() * 0.22f), on ? juce::Colours::black : offInk);
            r.removeFromLeft (r.getHeight() * 0.18f);
            g.setColour (isEnabled() ? offInk : colours::dim.withAlpha (0.55f));
            g.setFont (fontFittingWidth (homeFont (TypeScale::body, r.getHeight() * 0.42f), getButtonText(), r.getWidth()));
            g.drawFittedText (getButtonText(), r.toNearestInt(), juce::Justification::centredLeft, 1, 0.75f);
            return;
        }
        auto fill = on ? onColour : (outlined ? juce::Colours::transparentBlack : offColour);
        if (down)
            fill = (fill.isTransparent() ? colours::panel : fill).brighter (0.15f);
        else if (over && isEnabled())
            fill = (fill.isTransparent() ? colours::panel : fill).brighter (0.07f);
        if (! isEnabled() && on)
            fill = fill.darker (0.45f);
        paintKeyBody (g, r, fill, frame);
        const auto ink = on ? juce::Colours::black : (isEnabled() ? offInk : colours::dim.withAlpha (0.55f));
        g.setColour (ink);
        const auto keyText = getButtonText();

        if (! icon.has_value())
        {
            auto font = homeFont (TypeScale::body, r.getHeight() * (twoLines ? 0.34f : 0.5f));
            if (! twoLines)
                font = fontFittingWidth (font, keyText, r.getWidth() - 6.0f, 0.7f);
            g.setFont (font);
            // Narrow keys (MAIN SENDS PRE at 1280x800) give the text the full width.
            g.drawFittedText (keyText, r.toNearestInt().reduced (juce::jlimit (1, 3, getWidth() / 12), 0), juce::Justification::centred, twoLines ? 2 : 1,
                              0.7f);
            return;
        }
        if (keyText.isEmpty())
        {
            drawIcon (g, *icon, r.reduced (r.getHeight() * 0.2f), ink);
            return;
        }
        if (iconAbove)
        {
            auto label = r.removeFromBottom (juce::jmax (11.0f, r.getHeight() * 0.32f));
            drawIcon (g, *icon, r.reduced (r.getHeight() * 0.1f), ink);
            g.setColour (ink);
            g.setFont (fontFittingWidth (homeFont (TypeScale::statusCaption * 1.2f, label.getHeight() * 0.9f), keyText, label.getWidth() - 2.0f, 0.8f));
            g.drawFittedText (keyText, label.toNearestInt().reduced (1, 0), juce::Justification::centredTop, 1, 0.8f);
            return;
        }
        auto iconBox = r.removeFromLeft (juce::jmin (r.getHeight(), r.getWidth() * 0.4f)).reduced (r.getHeight() * 0.2f);
        drawIcon (g, *icon, iconBox, ink);
        g.setColour (ink);
        // Narrow keys (small windows) shrink the label rather than cut it.
        g.setFont (fontFittingWidth (homeFont (TypeScale::body, r.getHeight() * 0.46f), keyText, r.getWidth() - 4.0f, 0.8f));
        g.drawFittedText (keyText, r.toNearestInt().reduced (2, 0), juce::Justification::centredLeft, 1, 0.8f);
    }

private:
    void timerCallback() override
    {
        if (isOn && getToggleState() != isOn())
            setToggleState (isOn(), juce::dontSendNotification);
    }

    std::optional<Icon> icon;
    juce::Colour onColour;
};

/** A key bound to an atomic flag. */
inline void bindKey (IconKey& k, std::atomic<bool>& flag)
{
    k.isOn = [&flag] { return flag.load(); };
    k.onClick = [&flag] { flag = ! flag.load(); };
}

/** A WING tab key: dark grey with dim text; the current one lighter, with gold
    text and a gold line along its top. */
class WingTab final : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;

    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        auto r = getLocalBounds().toFloat();
        const bool front = getToggleState();
        g.setColour (front ? colours::keyOff : (over ? colours::tab.brighter (0.06f) : colours::tab));
        g.fillRect (r);
        if (front)
        {
            g.setColour (colours::accent);
            g.fillRect (r.withHeight (juce::jlimit (2.0f, screenCap (4.0f), r.getHeight() * 0.07f)));
        }
        g.setColour (! isEnabled() ? colours::dim.withAlpha (0.5f) : (front ? colours::accent : colours::dim));
        g.setFont (homeFont (front ? TypeScale::tab : TypeScale::tab * 0.85f, r.getHeight() * 0.6f));
        g.drawFittedText (getButtonText(), getLocalBounds().reduced (4, 0), juce::Justification::centred, 1, 0.8f);
    }
};

/** A WING drop-down field (EQ MODEL, FX PROCESSOR, KEY SOURCE...): the shared
    DropdownField, which greys while popups::chooseFrom's list is open. */
using ChoiceField = DropdownField;

/** A row of keys of which one is lit; keys can be disabled. */
class Segmented final : public juce::Component, private juce::Timer
{
public:
    Segmented (juce::StringArray labels, std::function<int()> get, std::function<void (int)> set, juce::Colour on = colours::choice)
        : names (std::move (labels)), getter (std::move (get)), setter (std::move (set)), onColour (on)
    {
        startTimerHz (6);
    }

    std::function<bool (int)> isEnabledAt;
    /** A colour per key (MAIN blue | ALT orange); default: the one colour. */
    std::function<juce::Colour (int)> colourAt;
    bool twoLines = false;
    /** Keys stacked top to bottom (INPUT SELECT's INDIVIDUAL | GLOBAL, D16). */
    bool vertical = false;

    juce::Colour onColourAt (int i) const { return colourAt != nullptr ? colourAt (i) : onColour; }

    void setNames (juce::StringArray n)
    {
        names = std::move (n);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const int current = getter != nullptr ? getter() : -1;
        shown = current;
        auto r = getLocalBounds().toFloat();
        const float w = r.getWidth() / (float) juce::jmax (1, names.size());
        const float h = r.getHeight() / (float) juce::jmax (1, names.size());
        for (int i = 0; i < names.size(); ++i)
        {
            auto k = vertical ? juce::Rectangle<float> (r.getX(), r.getY() + h * (float) i, r.getWidth(), h).reduced (0.0f, 1.0f)
                              : juce::Rectangle<float> (r.getX() + w * (float) i, r.getY(), w, r.getHeight()).reduced (1.0f, 0.0f);
            const bool on = i == current;
            const bool enabled = isEnabled() && (isEnabledAt == nullptr || isEnabledAt (i));
            auto fill = on ? onColourAt (i) : colours::keyOff;
            if (! enabled && on)
                fill = fill.darker (0.45f);
            paintKeyBody (g, k, fill);
            g.setColour (on ? juce::Colours::black : (enabled ? colours::dim.brighter (0.5f) : colours::dim.withAlpha (0.55f)));
            g.setFont (homeFont (TypeScale::body, k.getHeight() * (twoLines ? 0.34f : 0.5f)));
            g.drawFittedText (names[i], k.toNearestInt().reduced (2, 0), juce::Justification::centred, twoLines ? 2 : 1, 0.75f);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (names.isEmpty() || ! isEnabled() || ! getLocalBounds().contains (e.getPosition()) || setter == nullptr)
            return;
        const int i = juce::jlimit (0, names.size() - 1, vertical ? e.y * names.size() / juce::jmax (1, getHeight())
                                                                  : e.x * names.size() / juce::jmax (1, getWidth()));
        if (isEnabledAt == nullptr || isEnabledAt (i))
            setter (i);
        repaint();
    }

private:
    void timerCallback() override
    {
        if (getter != nullptr && getter() != shown)
            repaint();
    }

    juce::StringArray names;
    std::function<int()> getter;
    std::function<void (int)> setter;
    juce::Colour onColour;
    int shown = -2;
};

//==============================================================================
/** The pan display: a black ring whose green arc, centred on the pan position,
    spans the stereo width, with a white needle at the pan position. The large
    version (MAIN SENDS) has handles: drag the outer one for pan, the two on
    the L/R image line for width. */
class PanWidthDisplay final : public juce::Component, private juce::Timer
{
public:
    PanWidthDisplay (std::atomic<float>& p, std::atomic<float>& w, bool withHandles) : pan (p), width (w), handles (withHandles)
    {
        startTimerHz (12);
    }

    std::function<void()> onClick;

    static float panAngle (float pan) { return juce::jlimit (-100.0f, 100.0f, pan) / 100.0f * juce::MathConstants<float>::pi * 0.75f; }
    static float halfSpan (float width) { return juce::jmin (std::abs (width), 150.0f) / 100.0f * juce::MathConstants<float>::halfPi; }

    static void paintRing (juce::Graphics& g, juce::Rectangle<float> r, float pan, float width, bool segmented)
    {
        const float size = juce::jmin (r.getWidth(), r.getHeight());
        const auto c = r.getCentre();
        const float rad = size * 0.5f * (segmented ? 0.5f : 0.92f); // [D7] 150 of 600 px
        // [D7] the MAIN SENDS ring is thin (a fifth of its radius) with dark
        // grey segments; the thumbnail rings are thicker.
        const float t = rad * (segmented ? 0.2f : 0.3f);
        const float mid = rad - t * 0.5f;
        juce::Path ring;
        ring.addCentredArc (c.x, c.y, mid, mid, 0.0f, 0.0f, juce::MathConstants<float>::twoPi, true);
        g.setColour (juce::Colours::black);
        g.strokePath (ring, juce::PathStrokeType (t));
        if (segmented)
        {
            // The rest of the ring in grey segments.
            g.setColour (colours::thumb);
            for (int i = 0; i < 6; ++i)
            {
                juce::Path seg;
                const float a0 = (float) i * juce::MathConstants<float>::twoPi / 6.0f + 0.04f;
                seg.addCentredArc (c.x, c.y, mid, mid, 0.0f, a0, a0 + juce::MathConstants<float>::twoPi / 6.0f - 0.08f, true);
                g.strokePath (seg, juce::PathStrokeType (t * 0.9f));
            }
        }
        const float a = panAngle (pan), hs = halfSpan (width);
        if (hs > 0.01f)
        {
            juce::Path arc;
            arc.addCentredArc (c.x, c.y, mid, mid, 0.0f, a - hs, a + hs, true);
            g.setColour (colours::panArc);
            g.strokePath (arc, juce::PathStrokeType (t, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
        }
        if (! segmented)
        {
            g.setColour (juce::Colours::white);
            g.drawLine (c.x, c.y, c.x + std::sin (a) * (rad - t * 0.2f), c.y - std::cos (a) * (rad - t * 0.2f), juce::jmax (1.5f, size * 0.035f));
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const float p = pan.load(), w = width.load();
        paintRing (g, r, p, w, handles);
        if (! handles)
            return;

        // Pan needle with its handle, and the L/R image line with its two handles.
        const auto c = r.getCentre();
        const float size = juce::jmin (r.getWidth(), r.getHeight());
        const float reach = size * 0.5f * 0.86f, hr = juce::jmax (5.0f, size * 0.03f);
        const float a = panAngle (p), hs = halfSpan (w);
        const auto at = [c] (float angle, float radius) { return juce::Point<float> (c.x + std::sin (angle) * radius, c.y - std::cos (angle) * radius); };
        g.setColour (juce::Colour (0xffbdbdbd));
        const auto ph = at (a, reach);
        g.drawLine ({ c, ph }, 3.0f);
        // The L/R image line runs across the whole area, its handles outside the ring.
        const auto l = at (a - hs, reach * 0.9f), rr = at (a + hs, reach * 0.9f);
        if (l.getDistanceFrom (rr) > 1.0f)
        {
            const auto dir = (rr - l) / l.getDistanceFrom (rr);
            const float ext = juce::jmax (r.getWidth(), r.getHeight());
            g.drawLine ({ l - dir * ext, rr + dir * ext }, 2.0f);
        }
        for (auto pt : { ph, l, rr })
        {
            g.setColour (juce::Colours::black);
            g.fillEllipse (pt.x - hr, pt.y - hr, hr * 2.0f, hr * 2.0f);
            g.setColour (juce::Colour (0xffbdbdbd));
            g.drawEllipse (pt.x - hr, pt.y - hr, hr * 2.0f, hr * 2.0f, 2.0f);
        }
        g.setColour (juce::Colour (0xff5a5a5a));
        g.fillEllipse (c.x - hr, c.y + size * 0.38f - hr, hr * 2.0f, hr * 2.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        startPan = pan.load();
        startWidth = width.load();
        dragMode = 0;
        if (handles)
        {
            const auto c = getLocalBounds().toFloat().getCentre();
            const auto v = e.position - c;
            const float angle = std::atan2 (v.x, -v.y);
            const float a = panAngle (startPan), hs = halfSpan (startWidth);
            auto diff = [] (float x, float y) { return std::abs (std::remainder (x - y, juce::MathConstants<float>::twoPi)); };
            dragMode = (diff (angle, a) < juce::jmin (diff (angle, a - hs), diff (angle, a + hs))) ? 1 : 2;
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! handles)
        {
            pan = juce::jlimit (-100.0f, 100.0f, startPan + (float) e.getDistanceFromDragStartX() * 0.8f);
            width = juce::jlimit (-150.0f, 150.0f, startWidth - (float) e.getDistanceFromDragStartY() * 1.5f);
            dragged = true;
            repaint();
            return;
        }
        const auto c = getLocalBounds().toFloat().getCentre();
        const auto v = e.position - c;
        const float angle = std::atan2 (v.x, -v.y);
        if (dragMode == 1)
            pan = juce::jlimit (-100.0f, 100.0f, angle / (juce::MathConstants<float>::pi * 0.75f) * 100.0f);
        else
        {
            const float hs = std::abs (std::remainder (angle - panAngle (pan.load()), juce::MathConstants<float>::twoPi));
            const float w = juce::jmin (150.0f, hs / juce::MathConstants<float>::halfPi * 100.0f);
            width = startWidth < 0.0f ? -w : w;
        }
        dragged = true;
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! dragged && onClick && getLocalBounds().contains (e.getPosition()))
            onClick();
        dragged = false;
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        pan = 0.0f;
        width = 100.0f;
    }

private:
    void timerCallback() override
    {
        const auto now = std::make_pair (pan.load(), width.load());
        if (now != shown)
        {
            shown = now;
            repaint();
        }
    }

    std::atomic<float>& pan;
    std::atomic<float>& width;
    bool handles;
    float startPan = 0.0f, startWidth = 0.0f;
    int dragMode = 0;
    bool dragged = false;
    std::pair<float, float> shown { 1.0e9f, 0.0f };
};

//==============================================================================
/** A screen fader: a dark slot with a coloured fill and the value in a white
    tag (grey while the send is off). Optional dB scale on its left. */
class ScreenFader final : public juce::Component, private juce::Timer
{
public:
    explicit ScreenFader (std::atomic<float>& f, bool withScale = true) : faderDb (f), scale (withScale) { startTimerHz (20); }

    std::function<juce::Colour()> fillColour;
    std::function<bool()> isOn;
    juce::Colour wellColour = colours::well;  // the box behind the slot
    float scaleFraction = 0.4f;               // the share of the width the dB scale takes

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        // [D9] a 24 px value tag, a 12 px slot in an 80 px well (WING pixels).
        const float tagH = juce::jlimit (14.0f, juce::jmax (14.0f, r.getHeight() * 0.07f), wingPx (24.0f));
        auto scaleArea = scale ? r.removeFromLeft (r.getWidth() * scaleFraction) : juce::Rectangle<float>();
        // A dark well the fader's width, the black slot in its middle.
        auto well = r.withTrimmedLeft (scale ? juce::jmax (2.0f, wingPx (4.0f)) : 0.0f);
        auto slot = r.withSizeKeepingCentre (juce::jlimit (5.0f, juce::jmax (5.0f, r.getWidth() * 0.2f), wingPx (12.0f)), r.getHeight() - tagH);

        if (scale)
        {
            const auto font = homeFont (TypeScale::meterScale, screenCap (14.0f));
            const float lh = font.getHeight();
            g.setFont (font);
            g.setColour (colours::caption);
            for (float db : { 10.0f, 5.0f, 0.0f, -5.0f, -10.0f, -20.0f, -30.0f, -40.0f, -50.0f })
            {
                const float y = slot.getBottom() - slot.getHeight() * fader::dbToPos (db);
                g.drawText ((db > 0 ? "+" : "") + juce::String ((int) db), scaleArea.withY (y - lh * 0.5f).withHeight (lh).toNearestInt(),
                            juce::Justification::centredRight);
            }
            g.drawText ("-oo", scaleArea.withY (slot.getBottom() - lh * 0.5f).withHeight (lh).toNearestInt(), juce::Justification::centredRight);
        }

        g.setColour (wellColour);
        g.fillRect (well);
        g.setColour (juce::Colours::black);
        g.fillRect (slot);
        const float pos = fader::dbToPos (faderDb.load());
        const float y = slot.getBottom() - slot.getHeight() * pos;
        g.setColour (fillColour ? fillColour() : colours::fader);
        g.fillRect (slot.withTop (y).reduced (slot.getWidth() * 0.12f, 0.0f));

        // The value tag: white, grey while the send is off or at -oo.
        const bool on = (! isOn || isOn()) && faderDb.load() > -89.9f;
        auto tag = juce::Rectangle<float> (juce::jmin (r.getWidth() - 2.0f, juce::jmax (44.0f, tagH * 2.7f)), tagH).withCentre ({ slot.getCentreX(), y });
        tag.setX (juce::jlimit (0.0f, (float) getWidth() - tag.getWidth(), tag.getX()));
        g.setColour (on ? colours::ink : colours::dim);
        g.fillRect (tag);
        g.setColour (juce::Colours::black);
        g.setFont (homeFont (TypeScale::body, tagH * 0.8f));
        // A narrow fader (LEVEL at the minimum window size) leaves out the unit, not the value.
        auto text = wingDb (faderDb.load()) + " dB";
        if (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) * 0.7f > tag.getWidth() - 2.0f)
            text = wingDb (faderDb.load());
        g.drawFittedText (text, tag.toNearestInt().reduced (1, 0), juce::Justification::centred, 1, 0.7f);
    }

    void mouseDown (const juce::MouseEvent&) override { start = fader::dbToPos (faderDb.load()); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const float sens = e.mods.isShiftDown() ? 0.2f : 1.0f;
        faderDb = fader::posToDb (start - sens * (float) e.getDistanceFromDragStartY() / juce::jmax (50.0f, (float) getHeight() - 24.0f));
        repaint();
    }
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        faderDb = 0.0f;
        repaint();
    }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        faderDb = fader::posToDb (fader::dbToPos (faderDb.load()) + (w.deltaY > 0 ? 0.01f : -0.01f));
        repaint();
    }

private:
    void timerCallback() override
    {
        const float v = faderDb.load();
        const bool on = ! isOn || isOn();
        if (v != shown || on != shownOn)
        {
            shown = v;
            shownOn = on;
            repaint();
        }
    }

    std::atomic<float>& faderDb;
    bool scale;
    float start = 0.0f, shown = 1.0e9f;
    bool shownOn = true;
};

//==============================================================================
/** A segmented LED meter (1 or 2 bars) with an amber peak-hold dash, the scale
    `CLIP -6 -12 -24 -36 -48 -60` and `L R` under stereo bars. */
class LedMeter final : public juce::Component, private juce::Timer
{
public:
    LedMeter (std::function<float (int)> read, std::function<int()> channels) : reader (std::move (read)), numChannels (std::move (channels))
    {
        startTimerHz (25);
    }

    bool showScale = true;

    static float proportion (float db) { return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f); }

    void paint (juce::Graphics& g) override
    {
        // [D9] two 12 px bars 6 px apart, 6 px segments with 3 px gaps, the
        // unlit segments darker than the panel; the scale right-aligned at
        // their left, L and R under them (WING pixels).
        auto r = getLocalBounds().toFloat();
        const int n = juce::jlimit (1, 2, numChannels ? numChannels() : 2);
        const auto font = homeFont (TypeScale::meterScale, screenCap (13.0f));
        auto labels = n == 2 ? r.removeFromBottom (juce::jmax (12.0f, font.getHeight() * 1.3f)) : juce::Rectangle<float>();
        auto scaleArea = showScale ? r.removeFromLeft (juce::jmin (juce::jmax (24.0f, wingPx (32.0f)), r.getWidth() * 0.45f)) : juce::Rectangle<float>();
        r = r.reduced (0.0f, juce::jmax (4.0f, font.getHeight() * 0.5f));
        if (showScale)
        {
            g.setColour (colours::caption);
            g.setFont (font);
            for (float db : { 0.0f, -6.0f, -12.0f, -24.0f, -36.0f, -48.0f, -60.0f })
            {
                const float y = r.getBottom() - r.getHeight() * proportion (db);
                g.drawText (db == 0.0f ? juce::String ("CLIP") : juce::String ((int) db),
                            scaleArea.withY (y - font.getHeight() * 0.5f).withHeight (font.getHeight()).toNearestInt(), juce::Justification::centredRight);
            }
        }
        const float pitch = juce::jmax (4.0f, wingPx (9.0f));
        const int segments = juce::jlimit (8, 60, (int) (r.getHeight() / pitch));
        const float segH = r.getHeight() / (float) segments;
        const float gapX = juce::jmax (3.0f, wingPx (6.0f));
        const float barW = juce::jmin (juce::jmax (4.0f, wingPx (12.0f)), (r.getWidth() - gapX) / (float) n - gapX);
        float x = r.getX() + gapX;
        for (int ch = 0; ch < n; ++ch)
        {
            auto bar = juce::Rectangle<float> (x, r.getY(), barW, r.getHeight());
            const float lit = proportion (level[(size_t) ch]);
            for (int s = 0; s < segments; ++s)
            {
                const float top = (float) (s + 1) / (float) segments;
                auto seg = juce::Rectangle<float> (bar.getX(), bar.getBottom() - top * bar.getHeight(), bar.getWidth(), segH * 0.67f);
                const bool on = lit >= top - 0.5f / (float) segments;
                const bool clipSeg = s == segments - 1;
                g.setColour (on ? (clipSeg ? Colours::mute : Colours::meterGreen) : colours::well);
                g.fillRect (seg);
            }
            const float holdP = proportion (hold[(size_t) ch]);
            if (holdP > 0.01f)
            {
                g.setColour (Colours::amber);
                g.fillRect (bar.getX(), bar.getBottom() - holdP * bar.getHeight(), bar.getWidth(), 2.0f);
            }
            if (n == 2)
            {
                g.setColour (colours::caption);
                g.setFont (font);
                g.drawText (ch == 0 ? "L" : "R", juce::Rectangle<float> (bar.getX() - 4.0f, labels.getY(), bar.getWidth() + 8.0f, labels.getHeight()).toNearestInt(),
                            juce::Justification::centred);
            }
            x += barW + gapX;
        }
    }

private:
    void timerCallback() override
    {
        bool changed = false;
        const int n = numChannels ? numChannels() : 2;
        for (size_t ch = 0; ch < 2; ++ch)
        {
            const float db = (int) ch < n && reader ? dsp::gainToDb (reader ((int) ch)) : -100.0f;
            if (std::abs (db - level[ch]) > 0.4f)
            {
                level[ch] = db;
                changed = true;
            }
            if (db >= hold[ch])
            {
                hold[ch] = db;
                holdFrames[ch] = 30;
            }
            else if (--holdFrames[ch] <= 0 && hold[ch] > -100.0f)
            {
                hold[ch] = juce::jmax (-100.0f, hold[ch] - 2.0f);
                changed = true;
            }
        }
        if (changed)
            repaint();
    }

    std::function<float (int)> reader;
    std::function<int()> numChannels;
    std::array<float, 2> level { -100.0f, -100.0f }, hold { -100.0f, -100.0f };
    std::array<int, 2> holdFrames {};
};

//==============================================================================
/** The LEVEL panel's content: MUTE at the top left, the meter, SOLO at the
    bottom left and the fader. On OVERVIEW, ICON/COLOR, NAME, TAGS and MAIN. */
class LevelPanel final : public juce::Component
{
public:
    LevelPanel (UiContext& ctx, Strip& s)
        : meter ([&ctx, &s] (int ch) { return ctx.meters.get (&s, false, ch); }, [&s] { return s.getNumChannels(); }), fader (s.params.faderDb)
    {
        bindKey (mute, s.params.mute);
        bindKey (solo, s.params.solo);
        for (juce::Component* c : { (juce::Component*) &mute, (juce::Component*) &solo, (juce::Component*) &meter, (juce::Component*) &fader })
            addAndMakeVisible (c);
    }

    juce::Component* getFader() noexcept { return &fader; }

    void resized() override
    {
        // [D9] (WING pixels in a 192 x 504 body): MUTE 84 x 56 at the top
        // left, SOLO at the bottom left, the meter between them; the fader's
        // scale from x 96, its 80 px well at the right.
        auto l = getLocalBounds();
        auto meterCol = l.removeFromLeft (l.getWidth() * 50 / 100);
        const int kh = juce::jlimit (20, juce::jmax (20, l.getHeight() / 8), juce::roundToInt (wingPx (56.0f)));
        const int kw = juce::jmin (meterCol.getWidth(), juce::roundToInt (wingPx (84.0f)));
        mute.setBounds (meterCol.removeFromTop (kh).withWidth (kw));
        solo.setBounds (meterCol.removeFromBottom (kh).withWidth (kw));
        meter.setBounds (meterCol.reduced (0, juce::roundToInt (juce::jmax (4.0f, wingPx (16.0f)))));
        fader.scaleFraction = 0.26f;
        fader.setBounds (l);
    }

private:
    IconKey mute { "MUTE", std::nullopt, colours::mute }, solo { "SOLO", std::nullopt, colours::accent };
    LedMeter meter;
    ScreenFader fader;
};

//==============================================================================
/** A knob box (TRIM, FIXED GAIN, TAP WIDTH): a black box with a ring on the
    left, the caption small at the top and the value large. Drag to change. */
class KnobBox final : public juce::Component, private juce::Timer
{
public:
    explicit KnobBox (KnobBinding b) : binding (std::move (b)) { startTimerHz (12); }

    const KnobBinding& getBinding() const noexcept { return binding; }
    void setBinding (KnobBinding b)
    {
        binding = std::move (b);
        repaint();
    }
    juce::Colour ringColour = colours::ink;  // TAP WIDTH: green

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const bool enabled = isEnabled() && binding.isValid();
        paintKeyBody (g, r, colours::well);
        // A narrow box keeps at least half its width for the caption and value.
        // [D10] a thick ring about 70 % of the box height.
        const float ringW = juce::jmin (r.getHeight(), r.getWidth() * 0.5f);
        auto ring = r.removeFromLeft (ringW).withSizeKeepingCentre (ringW, ringW).reduced (ringW * 0.1f);
        const float a0 = juce::MathConstants<float>::pi * 1.25f, a1 = juce::MathConstants<float>::pi * 2.75f;
        const float pos = enabled ? binding.get() : 0.0f;
        juce::Path track, value;
        const float rad = ring.getWidth() * 0.4f;
        const float from = binding.bipolar ? (a0 + a1) * 0.5f : a0;
        track.addCentredArc (ring.getCentreX(), ring.getCentreY(), rad, rad, 0.0f, a0, a1, true);
        value.addCentredArc (ring.getCentreX(), ring.getCentreY(), rad, rad, 0.0f, from, a0 + pos * (a1 - a0), true);
        g.setColour (colours::chipOff);
        g.strokePath (track, juce::PathStrokeType (ring.getWidth() * 0.2f));
        if (enabled)
        {
            g.setColour (ringColour);
            g.strokePath (value, juce::PathStrokeType (ring.getWidth() * 0.2f));
        }
        r.removeFromLeft (4.0f);
        g.setColour (enabled ? colours::caption : colours::dim);
        g.setFont (homeFont (TypeScale::caption, r.getHeight() * 0.32f));
        g.drawFittedText (binding.label, r.removeFromTop (r.getHeight() * 0.4f).toNearestInt(), juce::Justification::bottomLeft, 1, 0.7f);
        g.setColour (enabled ? colours::ink : colours::dim);
        g.setFont (homeFont (TypeScale::value, r.getHeight() * 0.9f));
        g.drawFittedText (binding.text ? binding.text() : juce::String ("--"), r.toNearestInt(), juce::Justification::topLeft, 1, 0.7f);
    }

    void mouseDown (const juce::MouseEvent&) override { start = binding.isValid() ? binding.get() : 0.0f; }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! isEnabled() || ! binding.isValid())
            return;
        binding.set (juce::jlimit (0.0f, 1.0f, start - (float) e.getDistanceFromDragStartY() * (e.mods.isShiftDown() ? 0.0008f : 0.004f)));
        repaint();
    }
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (isEnabled() && binding.reset)
            binding.reset();
        repaint();
    }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        if (isEnabled() && binding.isValid())
            binding.set (juce::jlimit (0.0f, 1.0f, binding.get() + (w.deltaY > 0 ? 0.01f : -0.01f)));
        repaint();
    }

private:
    void timerCallback() override
    {
        const auto t = binding.isValid() && binding.text ? binding.text() : juce::String();
        if (t != shown)
        {
            shown = t;
            repaint();
        }
    }

    KnobBinding binding;
    float start = 0.0f;
    juce::String shown;
};

//==============================================================================
/** The feed grid of a bus, main or matrix INPUT panel: a cell per source,
    green while it sends to this strip. Click to open the Feed Configuration. */
class FeedGrid final : public juce::Component, private juce::Timer
{
public:
    FeedGrid (UiContext& c, Strip& s, std::function<void()> open) : ctx (c), strip (s), onOpen (std::move (open))
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        startTimerHz (4);
    }

    /** Whether `src` feeds this strip (and whether that send can exist at all). */
    static bool feeds (const Strip& target, const Strip& src, bool& possible)
    {
        possible = true;
        switch (target.getKind())
        {
            case StripKind::Bus:
            {
                if (&src == &target || (src.isBus() && ! (src.getId() >= kBusToBusTargets && target.getId() < kBusToBusTargets)))
                {
                    possible = false;
                    return false;
                }
                auto* send = src.sends.find (target.getId());
                return send != nullptr && send->enabled.load();
            }
            case StripKind::Main:
                if (src.getKind() == StripKind::Main || src.getKind() == StripKind::Matrix)
                {
                    possible = false;
                    return false;
                }
                return src.params.mains[juce::jlimit (0, kNumMains - 1, target.getId())].on.load();
            case StripKind::Matrix:
                if (src.getKind() == StripKind::Matrix)
                {
                    possible = false;
                    return false;
                }
                return src.params.matrix[juce::jlimit (0, kNumMatrices - 1, target.getId())].on.load();
            case StripKind::Channel:
            case StripKind::Aux:
                break;
        }
        possible = false;
        return false;
    }

    void paint (juce::Graphics& g) override
    {
        auto& e = ctx.engine;
        struct Group
        {
            const char* caption;
            const MixerEngine::StripList* list;
            int columns;
        };
        std::vector<Group> groups { { "CHANNEL", &e.getChannels(), 8 }, { "BUS", &e.getBuses(), 8 }, { "AUX", &e.getAux(), 8 } };
        if (strip.getKind() == StripKind::Matrix)
            groups.push_back ({ "MAIN", &e.getMains(), 4 });

        // Two columns: CHANNEL and AUX left, BUS (and MAIN) right.
        auto r = getLocalBounds().toFloat();
        auto left = r.removeFromLeft (r.getWidth() * 0.52f);
        r.removeFromLeft (r.getWidth() * 0.04f);
        auto right = r;
        const int chRows = juce::jmax (1, ((int) e.getChannels().size() + 7) / 8);
        const int busRows = juce::jmax (1, juce::jmin (2, ((int) e.getBuses().size() + 7) / 8));
        const float captionH = juce::jlimit (9.0f, screenCap (13.0f), getHeight() * 0.08f);
        const float cellH = juce::jmin ((left.getHeight() - 2.0f * captionH - 6.0f) / (float) (chRows + 1), left.getWidth() / 8.0f * 0.75f);

        auto drawGroup = [&] (juce::Rectangle<float>& area, const Group& grp, int maxRows)
        {
            g.setColour (colours::caption);
            g.setFont (homeFont (TypeScale::caption, captionH));
            g.drawText (grp.caption, area.removeFromTop (captionH).toNearestInt(), juce::Justification::centred);
            const int n = juce::jmin ((int) grp.list->size(), maxRows * grp.columns);
            const float w = area.getWidth() / 8.0f;
            const int rows = (n + grp.columns - 1) / grp.columns;
            for (int i = 0; i < n; ++i)
            {
                auto& src = *(*grp.list)[(size_t) i];
                bool possible = true;
                const bool on = feeds (strip, src, possible);
                auto cell = juce::Rectangle<float> (area.getX() + (float) (i % grp.columns) * w, area.getY() + (float) (i / grp.columns) * cellH, w, cellH).reduced (1.0f);
                g.setColour (on ? colours::feedOn : (possible ? colours::keyOff : colours::well));
                g.fillRect (cell);
                g.setColour (on ? juce::Colours::black : (possible ? colours::dim : colours::dim.withAlpha (0.3f)));
                g.setFont (homeFont (TypeScale::caption, cellH * 0.7f));
                g.drawFittedText (juce::String (src.getNumber()), cell.toNearestInt(), juce::Justification::centred, 1, 0.6f);
            }
            area.removeFromTop ((float) rows * cellH + 4.0f);
        };
        drawGroup (left, groups[0], chRows);
        drawGroup (left, groups[2], 1);
        drawGroup (right, groups[1], busRows);
        if (groups.size() > 3)
            drawGroup (right, groups[3], 1);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (getLocalBounds().contains (e.getPosition()) && onOpen)
            onOpen();
    }

private:
    void timerCallback() override { repaint(); }

    UiContext& ctx;
    Strip& strip;
    std::function<void()> onOpen;
};

//==============================================================================
/** The strip's free tags (HOME > TAGS; the WING's comma-separated `/ch/n/tags`). */
inline juce::StringArray stripTags (const Strip& s)
{
    juce::StringArray tags;
    tags.addTokens (takeString (ld::ld_strip_tags (s.getHandle())), ",", "");
    tags.trim();
    tags.removeEmptyStrings();
    return tags;
}

inline void setStripTags (const Strip& s, const juce::StringArray& tags)
{
    const auto joined = tags.joinIntoString (",");
    const Utf8 u (joined);
    ld::ld_strip_set_tags (s.getHandle(), u.data, u.size);
}

/** A tag chip (I12): a green tag shape with a dot at its point and the text in
    black. Returns the width it took. */
inline float paintTagChip (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, juce::Colour fill = colours::post)
{
    const juce::Font font (homeFont (TypeScale::body, r.getHeight() * 0.85f));
    const float tip = r.getHeight() * 0.45f;
    const float w = juce::jmin (r.getWidth(), tip + r.getHeight() * 0.5f + juce::GlyphArrangement::getStringWidth (font, text) + r.getHeight() * 0.4f);
    auto chip = r.withWidth (w);
    juce::Path shape;
    shape.startNewSubPath (chip.getX(), chip.getCentreY());
    shape.lineTo (chip.getX() + tip, chip.getY());
    shape.lineTo (chip.getRight(), chip.getY());
    shape.lineTo (chip.getRight(), chip.getBottom());
    shape.lineTo (chip.getX() + tip, chip.getBottom());
    shape.closeSubPath();
    g.setColour (fill);
    g.fillPath (shape);
    const float d = r.getHeight() * 0.22f;
    g.setColour (juce::Colours::black);
    g.fillEllipse (juce::Rectangle<float> (d, d).withCentre ({ chip.getX() + tip * 0.85f, chip.getCentreY() }));
    g.setFont (font);
    g.drawFittedText (text, chip.withTrimmedLeft (tip + d).toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
    return w;
}

//==============================================================================
/** A strip tile (Channel Select, INIT/COPY destinations, ROUTING-style grids):
    a name strip in the strip's colour, its icon and its number. */
inline void paintStripTile (juce::Graphics& g, juce::Rectangle<float> r, const Strip& s, bool selected, bool dim = false)
{
    // The shared WING tile (CHANNEL SELECT): colour strip and name, the number
    // (boxed for channels and aux), the icon, the gold frame when selected.
    WingTileInfo t;
    t.colour = dim ? stripDisplayColour (s).withMultipliedBrightness (0.5f) : stripDisplayColour (s);
    t.name = hasDefaultName (s) ? juce::String() : stripName (s);
    t.label = shortLabel (s);
    t.boxed = s.isInput();
    t.wingIcon = s.params.icon.load();
    t.kindIcon = stripIcon (s.getKind());
    t.selected = selected;
    paintWingTile (g, r, t);
}

//==============================================================================
/** Base of the overlay screens (CHANNEL INIT/COPY, FEED CONFIGURATION, RTA
    SETTINGS, CHANNEL SELECT): a title bar with `CLOSE ☑` at the right. */
class OverlayScreen : public juce::Component
{
public:
    explicit OverlayScreen (const juce::String& t) : title (t)
    {
        closeKey.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (closeKey);
        setInterceptsMouseClicks (true, true);
    }

    /** Where the window floats over the page (1.12): the whole page, docked at
        the right (EQ/RTA SETTINGS, 46 % wide) or centred. The rest of the page
        shows through the 75 % scrim; touching it closes the window. */
    void setPlacement (OverlayPlacement p)
    {
        placement = p;
        resized();
        repaint();
    }

    std::function<void()> onClose;

    void setTitle (const juce::String& t)
    {
        title = t;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto window = windowArea();
        if (placement.floats())
        {
            g.setColour (Colours::scrim);
            g.fillAll();
        }
        g.setColour (Colours::screen);
        g.fillRect (window);
        paintWindowTitle (g, window, title, placement.kind == OverlayPlacement::Kind::DockRight);
    }

    void resized() override
    {
        auto bar = titleBar();
        closeKey.setBounds (bar.removeFromRight (juce::jmin (bar.getHeight() * 3, bar.getWidth() / 4)).reduced (bar.getHeight() / 10));
        layoutContent (content());
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! windowArea().contains (e.getPosition()) && onClose)
            onClose();
    }

protected:
    virtual void layoutContent (juce::Rectangle<int>) {}

    juce::Rectangle<int> windowArea() const { return placement.windowIn (getLocalBounds()); }
    juce::Rectangle<int> titleBar() const
    {
        const auto window = windowArea();
        return window.withHeight (windowTitleHeight (window.getHeight()));
    }
    juce::Rectangle<int> content() const { return windowArea().withTrimmedTop (titleBar().getHeight() + 6); }

private:
    juce::String title;
    CloseKey closeKey;
    OverlayPlacement placement;
};

//==============================================================================
/** The transfer curve of a dynamics slot (the engine's static curve of its
    model), the WING's thumbnail of its own dynamics models, with the model's
    token at the top left. */
inline void paintTransferCurve (juce::Graphics& g, juce::Rectangle<float> r, const DynSlotParams& slot, bool, bool withToken)
{
    const auto values = slotValues (slot);
    const bool on = slot.on.load();
    paintTransferThumb (g, r, slot.model.load(), values.data(), on);
    if (withToken)
    {
        g.setColour (on ? colours::accent : colours::dim);
        g.setFont (juce::FontOptions (juce::jlimit (9.0f, screenCap (13.0f), r.getHeight() * 0.18f), juce::Font::bold));
        g.drawText (models::model (models::Dynamics, slot.model.load()).token, r.reduced (4.0f, 2.0f).toNearestInt(), juce::Justification::topLeft);
    }
}

} // namespace livedaw::ui::home
