#pragma once

#include "dsp/DspUtils.h"
#include "ui/DisplayPage.h"
#include "ui/Faceplate.h"
#include "ui/Icons.h"
#include "ui/Knobs.h"
#include "ui/UiContext.h"

#include <array>
#include <cmath>
#include <functional>

/* Widgets the HOME screens share, drawn after the WING's Channel Home and
   processing-slot screens (manual 5.1 and chapter 6). */

namespace livedaw::ui::home
{

//==============================================================================
// Colours of the HOME screens.
namespace colours
{
    const juce::Colour eqOn { 0xff4aa3e0 };      // block keys: EQ on
    const juce::Colour dynOn { 0xffe07a22 };     // dynamics on
    const juce::Colour insOn { 0xff9a2cf0 };     // inserts on
    const juce::Colour fader { 0xff3d8de0 };     // screen faders
    const juce::Colour tap { 0xffe0393e };       // TAP / PRE sends
    const juce::Colour post { 0xff3cc04a };      // POST sends
    const juce::Colour group { 0xff3d8de0 };     // GROUP sends
    const juce::Colour stereo { 0xffc8b43a };    // stereo source label (olive)
    const juce::Colour mono { 0xff5aa0e8 };      // mono source label
    const juce::Colour soloBus { 0xffb5c51a };   // SOLO BUS keys
    const juce::Colour panArc { 0xff52c43c };    // pan / width arc
    const juce::Colour keyOff { 0xff4b4b4b };
    const juce::Colour keyFrame { 0xff2a2a2a };
    const juce::Colour thumb { 0xff333333 };     // block thumbnails
    const juce::Colour well { 0xff141414 };      // dark wells (fader slots, meters)
    const juce::Colour chipOff { 0xff5a5a5a };
    const juce::Colour feedOn { 0xff3cc04a };    // feed grid cell: sends to this bus
} // namespace colours

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
    const auto token = models::model (models::Dynamics, model).token;
    return token == "GATE" || token == "DUCK" || token == "COMP" || token == "EXP" || token == "LMT" || token == "ONEC";
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
inline KnobStyle toolStyle (juce::Component* t = nullptr) { return styleOf (KnobStyle::Bar::Centre, KnobColours::cyan, t); }

//==============================================================================
/** A screen key: grey, lit in `onColour` while `isOn` returns true. Text, an
    icon, an icon left of the text, or an icon above a small label. */
class IconKey final : public juce::TextButton, private juce::Timer
{
public:
    IconKey (const juce::String& keyText, std::optional<Icon> i, juce::Colour on = Colours::yellow) : juce::TextButton (keyText), icon (i), onColour (on)
    {
        startTimerHz (8);
    }

    std::function<bool()> isOn;
    bool iconAbove = false, outlined = false, twoLines = false;
    juce::Colour offColour = colours::keyOff;

    void setIcon (std::optional<Icon> i)
    {
        icon = i;
        repaint();
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const bool on = getToggleState();
        auto r = getLocalBounds().toFloat();
        auto fill = on ? onColour : offColour;
        if (down)
            fill = fill.brighter (0.15f);
        else if (over && isEnabled())
            fill = fill.brighter (0.07f);
        if (! isEnabled())
            fill = fill.darker (0.45f);
        g.setColour (fill);
        g.fillRoundedRectangle (r, 2.0f);
        if (outlined)
        {
            g.setColour (juce::Colour (0xffbdbdbd));
            g.drawRoundedRectangle (r.reduced (0.5f), 2.0f, 1.0f);
        }
        const auto ink = on ? juce::Colours::black : (isEnabled() ? Colours::text : Colours::textDim.withAlpha (0.6f));
        g.setColour (ink);
        const auto keyText = getButtonText();

        if (! icon.has_value())
        {
            g.setFont (juce::FontOptions (juce::jlimit (11.0f, 21.0f, r.getHeight() * (twoLines ? 0.34f : 0.48f)), juce::Font::bold));
            g.drawFittedText (keyText, r.toNearestInt().reduced (3, 0), juce::Justification::centred, twoLines ? 2 : 1, 0.8f);
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
            g.setFont (juce::FontOptions (juce::jlimit (9.0f, 14.0f, label.getHeight() * 0.85f), juce::Font::bold));
            g.drawFittedText (keyText, label.toNearestInt().reduced (1, 0), juce::Justification::centredTop, 1, 0.8f);
            return;
        }
        auto iconBox = r.removeFromLeft (juce::jmin (r.getHeight(), r.getWidth() * 0.4f)).reduced (r.getHeight() * 0.2f);
        drawIcon (g, *icon, iconBox, ink);
        g.setColour (ink);
        g.setFont (juce::FontOptions (juce::jlimit (10.0f, 19.0f, r.getHeight() * 0.42f), juce::Font::bold));
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

/** A WING tab key: grey, the current one with yellow text and a yellow top line. */
class WingTab final : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;

    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        auto r = getLocalBounds().toFloat();
        const bool front = getToggleState();
        g.setColour (front ? Colours::screenPanel : (over ? Colours::screenHeader.brighter (0.08f) : Colours::screenHeader));
        g.fillRect (r);
        if (front)
        {
            g.setColour (Colours::yellow);
            g.fillRect (r.withHeight (3.0f));
        }
        g.setColour (! isEnabled() ? Colours::textDim : (front ? Colours::yellow : Colours::textDim.brighter (0.2f)));
        g.setFont (juce::FontOptions (juce::jlimit (12.0f, 21.0f, r.getHeight() * 0.5f), juce::Font::bold));
        g.drawFittedText (getButtonText(), getLocalBounds().reduced (4, 0), juce::Justification::centred, 1, 0.8f);
    }
};

/** A row of keys of which one is lit; keys can be disabled. */
class Segmented final : public juce::Component, private juce::Timer
{
public:
    Segmented (juce::StringArray labels, std::function<int()> get, std::function<void (int)> set, juce::Colour on = colours::fader)
        : names (std::move (labels)), getter (std::move (get)), setter (std::move (set)), onColour (on)
    {
        startTimerHz (6);
    }

    std::function<bool (int)> isEnabledAt;
    bool twoLines = false;

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
        for (int i = 0; i < names.size(); ++i)
        {
            auto k = juce::Rectangle<float> (r.getX() + w * (float) i, r.getY(), w, r.getHeight()).reduced (1.5f, 0.0f);
            const bool on = i == current;
            const bool enabled = isEnabled() && (isEnabledAt == nullptr || isEnabledAt (i));
            auto fill = on ? onColour : colours::keyOff;
            if (! enabled)
                fill = fill.darker (0.45f);
            g.setColour (fill);
            g.fillRoundedRectangle (k, 2.0f);
            g.setColour (on ? juce::Colours::black : (enabled ? Colours::text : Colours::textDim.withAlpha (0.6f)));
            g.setFont (juce::FontOptions (juce::jlimit (10.0f, 19.0f, k.getHeight() * (twoLines ? 0.34f : 0.46f)), juce::Font::bold));
            g.drawFittedText (names[i], k.toNearestInt().reduced (2, 0), juce::Justification::centred, twoLines ? 2 : 1, 0.75f);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (names.isEmpty() || ! isEnabled() || ! getLocalBounds().contains (e.getPosition()) || setter == nullptr)
            return;
        const int i = juce::jlimit (0, names.size() - 1, e.x * names.size() / juce::jmax (1, getWidth()));
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
        const float rad = size * 0.5f * (segmented ? 0.62f : 0.92f);
        const float t = rad * (segmented ? 0.3f : 0.3f);
        const float mid = rad - t * 0.5f;
        juce::Path ring;
        ring.addCentredArc (c.x, c.y, mid, mid, 0.0f, 0.0f, juce::MathConstants<float>::twoPi, true);
        g.setColour (juce::Colours::black);
        g.strokePath (ring, juce::PathStrokeType (t));
        if (segmented)
        {
            // The rest of the ring in grey segments.
            g.setColour (juce::Colour (0xff4a4a4a));
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
        const auto l = at (a - hs, reach * 0.8f), rr = at (a + hs, reach * 0.8f);
        g.drawLine ({ l, rr }, 2.0f);
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

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const float tagH = juce::jlimit (16.0f, 24.0f, r.getHeight() * 0.07f);
        auto scaleArea = scale ? r.removeFromLeft (r.getWidth() * 0.4f) : juce::Rectangle<float>();
        auto slot = r.withSizeKeepingCentre (juce::jlimit (6.0f, 26.0f, r.getWidth() * 0.32f), r.getHeight() - tagH);

        if (scale)
        {
            g.setFont (juce::FontOptions (juce::jlimit (9.0f, 13.0f, r.getHeight() * 0.045f), juce::Font::bold));
            for (float db : { 10.0f, 5.0f, 0.0f, -5.0f, -10.0f, -20.0f, -30.0f, -40.0f, -50.0f })
            {
                const float y = slot.getBottom() - slot.getHeight() * fader::dbToPos (db);
                g.setColour (Colours::textDim);
                g.drawText ((db > 0 ? "+" : "") + juce::String ((int) db), scaleArea.withY (y - 7.0f).withHeight (14.0f).toNearestInt(),
                            juce::Justification::centredRight);
            }
            g.drawText ("-oo", scaleArea.withY (slot.getBottom() - 7.0f).withHeight (14.0f).toNearestInt(), juce::Justification::centredRight);
        }

        g.setColour (colours::well);
        g.fillRect (slot.expanded (2.0f, 0.0f));
        const float pos = fader::dbToPos (faderDb.load());
        const float y = slot.getBottom() - slot.getHeight() * pos;
        g.setColour (fillColour ? fillColour() : colours::fader);
        g.fillRect (slot.withTop (y).reduced (slot.getWidth() * 0.18f, 0.0f));

        const bool on = ! isOn || isOn();
        auto tag = juce::Rectangle<float> (juce::jmin (r.getWidth() - 2.0f, juce::jmax (44.0f, tagH * 3.2f)), tagH).withCentre ({ slot.getCentreX(), y });
        tag.setX (juce::jlimit (0.0f, (float) getWidth() - tag.getWidth(), tag.getX()));
        g.setColour (on ? Colours::text : juce::Colour (0xff8a8a8a));
        g.fillRect (tag);
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (tagH * 0.72f, juce::Font::bold));
        g.drawFittedText (wingDb (faderDb.load()) + " dB", tag.toNearestInt().reduced (1, 0), juce::Justification::centred, 1, 0.7f);
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
        auto r = getLocalBounds().toFloat();
        const int n = juce::jlimit (1, 2, numChannels ? numChannels() : 2);
        auto labels = n == 2 ? r.removeFromBottom (14.0f) : juce::Rectangle<float>();
        auto scaleArea = showScale ? r.removeFromLeft (juce::jmin (30.0f, r.getWidth() * 0.45f)) : juce::Rectangle<float>();
        r = r.reduced (0.0f, 4.0f);
        if (showScale)
        {
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (juce::jlimit (8.0f, 12.0f, r.getHeight() * 0.05f), juce::Font::bold));
            for (float db : { 0.0f, -6.0f, -12.0f, -24.0f, -36.0f, -48.0f, -60.0f })
            {
                const float y = r.getBottom() - r.getHeight() * proportion (db);
                g.drawText (db == 0.0f ? juce::String ("CLIP") : juce::String ((int) db), scaleArea.withY (y - 6.0f).withHeight (12.0f).toNearestInt(),
                            juce::Justification::centredRight);
            }
        }
        const int segments = juce::jlimit (8, 40, (int) (r.getHeight() / 5.0f));
        const float segH = r.getHeight() / (float) segments;
        const float barW = juce::jmin (12.0f, (r.getWidth() - 6.0f) / (float) n - 3.0f);
        float x = r.getX() + 4.0f;
        for (int ch = 0; ch < n; ++ch)
        {
            auto bar = juce::Rectangle<float> (x, r.getY(), barW, r.getHeight());
            g.setColour (colours::well);
            g.fillRect (bar.expanded (1.0f));
            const float lit = proportion (level[(size_t) ch]);
            for (int s = 0; s < segments; ++s)
            {
                const float top = (float) (s + 1) / (float) segments;
                auto seg = juce::Rectangle<float> (bar.getX(), bar.getBottom() - top * bar.getHeight() + 1.0f, bar.getWidth(), segH - 1.5f);
                const bool on = lit >= top - 0.5f / (float) segments;
                const bool clipSeg = s == segments - 1;
                g.setColour (on ? (clipSeg ? Colours::mute : Colours::meterGreen) : juce::Colour (0xff1f2a1f));
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
                g.setColour (Colours::textDim);
                g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
                g.drawText (ch == 0 ? "L" : "R", juce::Rectangle<float> (bar.getX() - 4.0f, labels.getY(), bar.getWidth() + 8.0f, labels.getHeight()).toNearestInt(),
                            juce::Justification::centred);
            }
            x += barW + 4.0f;
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
/** A knob box (TRIM, FIXED GAIN, TAP WIDTH): a black box with a ring on the
    left, the caption small at the top and the value large. Drag to change. */
class KnobBox final : public juce::Component, private juce::Timer
{
public:
    explicit KnobBox (KnobBinding b) : binding (std::move (b)) { startTimerHz (12); }

    const KnobBinding& getBinding() const noexcept { return binding; }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const bool enabled = isEnabled() && binding.isValid();
        g.setColour (juce::Colours::black);
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (juce::Colour (0xff3a3a3a));
        g.drawRoundedRectangle (r.reduced (0.5f), 2.0f, 1.0f);
        auto ring = r.removeFromLeft (r.getHeight()).reduced (r.getHeight() * 0.16f);
        const float a0 = juce::MathConstants<float>::pi * 1.25f, a1 = juce::MathConstants<float>::pi * 2.75f;
        const float pos = enabled ? binding.get() : 0.0f;
        juce::Path track, value;
        const float rad = ring.getWidth() * 0.4f;
        const float from = binding.bipolar ? (a0 + a1) * 0.5f : a0;
        track.addCentredArc (ring.getCentreX(), ring.getCentreY(), rad, rad, 0.0f, a0, a1, true);
        value.addCentredArc (ring.getCentreX(), ring.getCentreY(), rad, rad, 0.0f, from, a0 + pos * (a1 - a0), true);
        g.setColour (juce::Colour (0xff5a5a5a));
        g.strokePath (track, juce::PathStrokeType (ring.getWidth() * 0.15f));
        if (enabled)
        {
            g.setColour (Colours::text);
            g.strokePath (value, juce::PathStrokeType (ring.getWidth() * 0.15f));
        }
        r.removeFromLeft (4.0f);
        g.setColour (enabled ? Colours::text : Colours::textDim);
        g.setFont (juce::FontOptions (juce::jlimit (9.0f, 14.0f, r.getHeight() * 0.28f), juce::Font::bold));
        g.drawText (binding.label, r.removeFromTop (r.getHeight() * 0.4f).toNearestInt(), juce::Justification::bottomLeft);
        g.setFont (juce::FontOptions (juce::jlimit (12.0f, 26.0f, r.getHeight() * 0.8f), juce::Font::bold));
        g.drawFittedText (binding.text ? binding.text() : juce::String ("--"), r.toNearestInt(), juce::Justification::topLeft, 1, 0.8f);
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
        const float captionH = juce::jlimit (9.0f, 13.0f, getHeight() * 0.08f);
        const float cellH = juce::jmin ((left.getHeight() - 2.0f * captionH - 6.0f) / (float) (chRows + 1), left.getWidth() / 8.0f * 0.75f);

        auto drawGroup = [&] (juce::Rectangle<float>& area, const Group& grp, int maxRows)
        {
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (captionH, juce::Font::bold));
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
                g.setColour (on ? colours::feedOn : (possible ? juce::Colour (0xff262626) : juce::Colour (0xff1a1a1a)));
                g.fillRect (cell);
                g.setColour (on ? juce::Colours::black : (possible ? Colours::textDim : Colours::textDim.withAlpha (0.3f)));
                g.setFont (juce::FontOptions (juce::jlimit (7.0f, 12.0f, cellH * 0.62f), juce::Font::bold));
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
/** A strip tile (Channel Select, INIT/COPY destinations, ROUTING-style grids):
    a name strip in the strip's colour, its icon and its number. */
inline void paintStripTile (juce::Graphics& g, juce::Rectangle<float> r, const Strip& s, bool selected, bool dim = false)
{
    g.setColour (juce::Colour (0xff2e2e2e));
    g.fillRect (r);
    auto inner = r.reduced (2.0f);
    auto strip = inner.removeFromTop (juce::jmax (10.0f, inner.getHeight() * 0.28f));
    const auto colour = s.getColour();
    g.setColour (dim ? colour.withMultipliedBrightness (0.5f) : colour);
    g.fillRect (strip);
    g.setColour (colour.getPerceivedBrightness() > 0.45f ? juce::Colours::black : juce::Colours::white);
    g.setFont (juce::FontOptions (juce::jlimit (8.0f, 14.0f, strip.getHeight() * 0.85f), juce::Font::bold));
    g.drawFittedText (hasDefaultName (s) ? juce::String() : stripName (s).toUpperCase(), strip.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.7f);
    auto number = inner.removeFromBottom (juce::jmax (10.0f, inner.getHeight() * 0.36f));
    drawIcon (g, stripIcon (s.getKind()), inner.reduced (inner.getHeight() * 0.12f), dim ? Colours::textDim.withAlpha (0.5f) : Colours::textDim);
    g.setColour (dim ? Colours::textDim : Colours::text);
    g.setFont (juce::FontOptions (juce::jlimit (8.0f, 15.0f, number.getHeight() * 0.9f), juce::Font::bold));
    g.drawFittedText (shortLabel (s), number.toNearestInt(), juce::Justification::centred, 1);
    if (selected)
    {
        g.setColour (juce::Colours::white);
        g.drawRect (r, 2.0f);
    }
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

    std::function<void()> onClose;

    void setTitle (const juce::String& t)
    {
        title = t;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::screen);
        auto bar = titleBar().toFloat();
        g.setColour (Colours::screenHeader);
        g.fillRect (bar);
        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (juce::jlimit (14.0f, 22.0f, bar.getHeight() * 0.56f), juce::Font::bold));
        g.drawFittedText (title, bar.toNearestInt(), juce::Justification::centred, 1);
    }

    void resized() override
    {
        auto bar = titleBar();
        closeKey.setBounds (bar.removeFromRight (juce::jmin (110, bar.getWidth() / 5)).reduced (2));
        layoutContent (content());
    }

protected:
    virtual void layoutContent (juce::Rectangle<int>) {}
    juce::Rectangle<int> titleBar() const { return getLocalBounds().removeFromTop (juce::jlimit (28, 40, getHeight() / 12)); }
    juce::Rectangle<int> content() const { return getLocalBounds().withTrimmedTop (titleBar().getHeight() + 6); }

private:
    juce::String title;
    IconKey closeKey { "CLOSE", Icon::Check };
};

//==============================================================================
/** The transfer curve of a dynamics slot (input -60..0 dB against output),
    the WING's thumbnail of its own dynamics models. */
inline void paintTransferCurve (juce::Graphics& g, juce::Rectangle<float> r, const DynSlotParams& slot, bool gate, bool withToken)
{
    auto ratioOf = [&]
    {
        const auto& spec = models::model (models::Dynamics, slot.model.load());
        const int i = spec.find ("ratio");
        if (i < 0)
            return gate ? 100.0f : 4.0f;
        const auto& p = spec.params[(size_t) i];
        const int index = juce::roundToInt (slot.values[i].load());
        const auto text = p.choices.isEmpty() ? juce::String (slot.values[i].load())
                                              : p.choices[juce::jlimit (0, p.choices.size() - 1, index)];
        if (text.containsIgnoreCase ("gate"))
            return 100.0f;
        const float v = text.contains (":") ? text.fromFirstOccurrenceOf (":", false, false).getFloatValue() : text.getFloatValue();
        return v > 0.0f ? v : (gate ? 100.0f : 4.0f);
    };
    auto thresholdOf = [&]
    {
        const auto& spec = models::model (models::Dynamics, slot.model.load());
        const int i = spec.find ("thr");
        if (i < 0)
            return -20.0f;
        const auto& p = spec.params[(size_t) i];
        const float v = slot.values[i].load();
        return p.unit == "dB" ? v : -60.0f + 60.0f * p.toNormalised (v);
    };

    g.setColour (juce::Colours::black);
    g.fillRect (r);
    auto plot = r.reduced (juce::jmax (2.0f, r.getHeight() * 0.06f));
    g.setColour (juce::Colour (0xff2e2e2e));
    for (int i = 1; i < 4; ++i)
    {
        g.drawHorizontalLine (juce::roundToInt (plot.getY() + plot.getHeight() * (float) i / 4.0f), plot.getX(), plot.getRight());
        g.drawVerticalLine (juce::roundToInt (plot.getX() + plot.getWidth() * (float) i / 4.0f), plot.getY(), plot.getBottom());
    }
    const float thr = thresholdOf(), ratio = ratioOf();
    auto map = [&plot] (float inDb, float outDb)
    {
        return juce::Point<float> (plot.getX() + plot.getWidth() * (inDb + 60.0f) / 60.0f,
                                   plot.getBottom() - plot.getHeight() * juce::jlimit (0.0f, 1.0f, (outDb + 60.0f) / 60.0f));
    };
    juce::Path p;
    for (int i = 0; i <= 60; ++i)
    {
        const float in = -60.0f + (float) i;
        float out = in;
        if (gate && in < thr)
            out = thr + (in - thr) * juce::jmin (ratio, 8.0f);
        else if (! gate && in > thr)
            out = thr + (in - thr) / ratio;
        if (i == 0)
            p.startNewSubPath (map (in, out));
        else
            p.lineTo (map (in, out));
    }
    const bool on = slot.on.load();
    g.setColour (on ? Colours::text : Colours::textDim);
    g.strokePath (p, juce::PathStrokeType (juce::jmax (1.2f, r.getHeight() * 0.025f)));
    const auto knee = map (thr, thr);
    const float d = juce::jmax (3.0f, r.getHeight() * 0.07f);
    g.setColour (Colours::yellow);
    g.fillEllipse (knee.x - d * 0.5f, knee.y - d * 0.5f, d, d);
    if (withToken)
    {
        g.setColour (on ? Colours::yellow : Colours::textDim);
        g.setFont (juce::FontOptions (juce::jlimit (9.0f, 13.0f, r.getHeight() * 0.18f), juce::Font::bold));
        g.drawText (models::model (models::Dynamics, slot.model.load()).token, r.reduced (4.0f, 2.0f).toNearestInt(), juce::Justification::topLeft);
    }
}

} // namespace livedaw::ui::home
