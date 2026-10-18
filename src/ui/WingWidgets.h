#pragma once

#include "ui/Icons.h"
#include "ui/LookAndFeel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace livedaw::ui
{

/** The cap height of key text on the screen: about 2 % of the screen height,
    and at most `fraction` of the key's height. */
inline float wingKeyCap (float keyHeight, float fraction = 0.34f)
{
    return juce::jmin (0.02f * getWingScreenHeight(), keyHeight * fraction);
}

/** A flat screen key like the console's: #303030 framed in black, optional icon
    left of the text, lit in `onColour` (with black ink) while `isOn` returns
    true. An unlit toggle (one with `isOn`) has grey text, an action key white;
    an outlined key (SAFE, FILTER LO/HI) has a grey frame. */
class WingKey : public juce::Button, private juce::Timer
{
public:
    WingKey (const juce::String& keyText, std::optional<Icon> keyIcon = std::nullopt, juce::Colour on = Colours::gold)
        : juce::Button (keyText), icon (keyIcon), onColour (on)
    {
        startTimerHz (6);
    }

    std::function<bool()> isOn;
    juce::Colour offColour = Colours::screenKey;
    /** The icon's colour while the key is off (the recorder's red record dot). */
    std::optional<juce::Colour> iconColour;
    bool outlined = false;
    /** Icon above a small label (the library's FOLDER / RENAME / DELETE keys). */
    bool iconAbove = false;

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const bool on = isOn != nullptr && isOn();
        auto r = getLocalBounds().toFloat();
        auto fill = on ? onColour : offColour;
        if (down)
            fill = fill.brighter (0.15f);
        else if (over)
            fill = fill.brighter (0.06f);
        if (! isEnabled())
            fill = fill.darker (0.4f);
        g.setColour (fill);
        g.fillRect (r);
        g.setColour (outlined && ! on ? Colours::textDim : juce::Colours::black);
        g.drawRect (r, outlined ? 1.5f : 1.0f);
        const auto ink = on ? juce::Colours::black
                            : (! isEnabled() ? Colours::textDim.darker (0.3f) : (isOn != nullptr && ! outlined ? Colours::textDim : Colours::caption));
        if (iconAbove && icon.has_value())
        {
            auto label = r.removeFromBottom (r.getHeight() * 0.36f);
            drawIcon (g, *icon, r.reduced (r.getHeight() * 0.12f), ink);
            g.setColour (ink);
            g.setFont (wingFontPx (juce::jmin (TypeScale::statusCaption * getWingScreenHeight(), label.getHeight() * 0.6f)));
            g.drawFittedText (getButtonText(), label.toNearestInt().reduced (2, 0), juce::Justification::centredTop, 1, 0.75f);
            return;
        }
        auto content = r.reduced (juce::jmin (10.0f, r.getHeight() * 0.18f), 0.0f);
        if (icon.has_value())
        {
            const float s = juce::jmin (r.getHeight() * 0.62f, content.getWidth() * 0.4f);
            auto iconBox = getButtonText().isEmpty() ? content.withSizeKeepingCentre (s, s) : content.removeFromLeft (s).withSizeKeepingCentre (s, s);
            drawIcon (g, *icon, iconBox, ! on && iconColour.has_value() && isEnabled() ? *iconColour : ink);
            content.removeFromLeft (6.0f);
        }
        if (getButtonText().isNotEmpty())
        {
            const auto lineList = juce::StringArray::fromLines (getButtonText());
            const int lines = lineList.size();
            g.setColour (ink);
            // The longest line shrinks to fit rather than being cut (VIRTUAL SOUNDCHECK).
            auto font = wingFontPx (wingKeyCap (r.getHeight(), lines > 1 ? 0.26f : 0.34f));
            for (auto& line : lineList)
                font = fontFittingWidth (font, line, content.getWidth() - 2.0f);
            g.setFont (font);
            g.drawFittedText (getButtonText(), content.toNearestInt(), juce::Justification::centred, 2, 0.75f);
        }
    }

private:
    void timerCallback() override
    {
        if (isOn != nullptr && isOn() != lastOn)
        {
            lastOn = isOn();
            repaint();
        }
    }

    std::optional<Icon> icon;
    juce::Colour onColour;
    bool lastOn = false;
};

/** The console's "ON" chip: gold while the bound flag is set, click toggles it. */
class OnChip final : public WingKey
{
public:
    explicit OnChip (std::atomic<bool>& flag) : WingKey ("ON"), value (flag)
    {
        isOn = [this] { return value.load(); };
        offColour = Colours::screenKey;
        onClick = [this] { value = ! value.load(); };
    }

private:
    std::atomic<bool>& value;
};

/** A row of keys of which one is lit (NORM | FINE | AUTO on the console): the
    keys sit in a black frame, unlit #303030 with grey text. */
class SegmentedKeys final : public juce::Component
{
public:
    SegmentedKeys (juce::StringArray labels, std::function<int()> get, std::function<void (int)> set, juce::Colour on = Colours::gold)
        : names (std::move (labels)), getter (std::move (get)), setter (std::move (set)), onColour (on)
    {
    }

    void paint (juce::Graphics& g) override
    {
        const int current = getter != nullptr ? getter() : -1;
        auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colours::black);
        g.fillRect (r);
        r = r.reduced (1.0f);
        const float w = r.getWidth() / (float) juce::jmax (1, names.size());
        for (int i = 0; i < names.size(); ++i)
        {
            auto k = juce::Rectangle<float> (r.getX() + w * (float) i, r.getY(), w, r.getHeight()).reduced (1.0f, 0.0f);
            const bool on = i == current;
            g.setColour (on ? onColour : Colours::screenKey);
            g.fillRect (k);
            g.setColour (on ? juce::Colours::black : Colours::caption);
            g.setFont (wingFontPx (wingKeyCap (k.getHeight())));
            g.drawFittedText (names[i], k.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.75f);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (names.isEmpty() || ! getLocalBounds().contains (e.getPosition()) || setter == nullptr)
            return;
        setter (juce::jlimit (0, names.size() - 1, e.x * names.size() / juce::jmax (1, getWidth())));
        repaint();
    }

private:
    juce::StringArray names;
    std::function<int()> getter;
    std::function<void (int)> setter;
    juce::Colour onColour;
};

//==============================================================================
/** What a WING channel tile shows (CHANNEL SELECT, and the tiles of other
    screens that list strips). */
struct WingTileInfo
{
    juce::Colour colour;          // the strip's colour (already dimmed when it has no source)
    juce::String name;            // on the colour strip (empty for a default name)
    juce::String label;           // "1", "A1", "B1", "M1", "MX1"
    bool boxed = true;            // channels and aux: the number in a grey box
    int wingIcon = 0;             // /ch/n/icon, 0 = none
    std::optional<Icon> kindIcon; // drawn when wingIcon is 0 or unknown
    float meter = -1.0f;          // 0..1 level for the meter at the right edge (< 0: none)
    bool selected = false;        // the accent frame
};

/** Paints a WING channel tile (CHANNEL SELECT, 112x64 of 1280x800 px): the colour
    strip with the name, a #303030 body with the number (white in a grey box for
    channels and aux, grey text for buses, mains and matrices), the icon at the
    right, a level meter along the right edge, and a gold frame when selected. */
inline void paintWingTile (juce::Graphics& g, juce::Rectangle<float> r, const WingTileInfo& t)
{
    const auto tile = r;
    auto stripArea = r.removeFromTop (juce::jmax (4.0f, std::round (r.getHeight() * 22.0f / 64.0f)));
    g.setColour (t.colour);
    g.fillRect (stripArea);
    if (t.name.isNotEmpty())
    {
        g.setColour (t.colour.getPerceivedBrightness() > 0.62f ? juce::Colours::black : Colours::text);
        g.setFont (wingFontPx (juce::jmin (stripArea.getHeight() * 0.55f, 0.016f * getWingScreenHeight())));
        g.drawFittedText (t.name, stripArea.reduced (3.0f, 0.0f).toNearestInt(), juce::Justification::centred, 1, 0.7f);
    }
    r.removeFromTop (1.0f);
    g.setColour (Colours::screenTile);
    g.fillRect (r);

    if (t.meter >= 0.0f)
    {
        // Two columns of segments along the right edge.
        auto meter = r.removeFromRight (juce::jmax (4.0f, r.getHeight() * 0.22f)).reduced (1.0f, 2.0f);
        const int segments = 8;
        const float segH = meter.getHeight() / (float) segments;
        const int lit = juce::roundToInt (juce::jlimit (0.0f, 1.0f, t.meter) * (float) segments);
        for (int i = 0; i < segments; ++i)
            for (int col = 0; col < 2; ++col)
            {
                const auto seg = juce::Rectangle<float> (meter.getX() + (float) col * meter.getWidth() * 0.5f, meter.getBottom() - (float) (i + 1) * segH,
                                                         meter.getWidth() * 0.5f - 1.0f, segH - 1.0f);
                g.setColour (i < lit ? (i >= segments - 1 ? Colours::meterRed : (i >= segments - 2 ? Colours::meterYellow : Colours::meterGreen))
                                     : juce::Colours::black);
                g.fillRect (seg);
            }
    }

    auto body = r.reduced (r.getHeight() * 0.1f, r.getHeight() * 0.14f);
    const float cap = juce::jmin (body.getHeight() * 0.7f, 0.030f * getWingScreenHeight()); // I3: 15 % over the body text
    const auto font = wingFontPx (cap, WingWeight::SemiBold);
    g.setFont (font);
    if (t.boxed)
    {
        const float w = juce::GlyphArrangement::getStringWidth (font, t.label) + cap * 0.5f;
        auto box = body.removeFromLeft (juce::jmax (w, body.getHeight() * 0.8f)).withSizeKeepingCentre (juce::jmax (w, body.getHeight() * 0.8f), body.getHeight());
        g.setColour (Colours::chipOff);
        g.fillRoundedRectangle (box, 2.0f);
        g.setColour (Colours::text);
        g.drawText (t.label, box, juce::Justification::centred, false);
    }
    else
    {
        g.setColour (Colours::textDim.brighter (0.25f));
        g.drawText (t.label, body, juce::Justification::centredLeft, false);
    }
    auto iconArea = body.removeFromRight (body.getHeight()).reduced (body.getHeight() * 0.04f);
    const auto iconColour = Colours::textDim.brighter (0.3f);
    if (! (t.wingIcon > 0 && drawWingIcon (g, t.wingIcon, iconArea, iconColour)) && t.kindIcon.has_value())
        drawIcon (g, *t.kindIcon, iconArea.reduced (iconArea.getHeight() * 0.06f), iconColour);

    if (t.selected)
    {
        // The gold frame, just outside the tile.
        g.setColour (Colours::gold);
        g.drawRect (tile.expanded (3.0f), 2.0f);
    }
}

//==============================================================================
/** The pop-ups open on the screen that close as soon as a key of the surface is
    pressed ("most pop-ups automatically dismiss as soon as one of the buttons is
    pressed"). Pop-ups register a closer while open; SurfaceKey (and the screen
    keys beside the display) call closeAll() when pressed. Message thread only. */
class TransientPopups
{
public:
    using Closer = std::function<void()>;

    static void add (const void* owner, Closer closer)
    {
        remove (owner);
        list().push_back ({ owner, std::move (closer) });
    }

    static void remove (const void* owner)
    {
        auto& l = list();
        l.erase (std::remove_if (l.begin(), l.end(), [owner] (const auto& e) { return e.first == owner; }), l.end());
    }

    static bool anyOpen() { return ! list().empty(); }

    /** Closes every registered pop-up (each as if cancelled). */
    static void closeAll()
    {
        auto open = std::move (list());
        list().clear();
        for (auto& e : open)
            if (e.second != nullptr)
                e.second();
    }

private:
    static std::vector<std::pair<const void*, Closer>>& list()
    {
        static std::vector<std::pair<const void*, Closer>> open;
        return open;
    }
};

/** Brightness of the surface's key lighting (SETUP > GENERAL: BTNS/LEDS for lit
    keys, BACKLIGHT for unlit ones), 0..1. The main display applies the settings. */
struct SurfaceLighting
{
    static float& leds() { static float v = 0.9f; return v; }
    static float& backlight() { static float v = 0.25f; return v; }

    /** A lit key's colour at the LED brightness: dimmer toward the unlit body. */
    static juce::Colour lit (juce::Colour led, juce::Colour body)
    {
        return body.interpolatedWith (led, juce::jlimit (0.0f, 1.0f, 0.35f + 0.65f * leds() / 0.9f));
    }

    /** An unlit key's body with the backlight: a faint lift of the print. */
    static juce::Colour unlit (juce::Colour body) { return body.brighter (juce::jlimit (0.0f, 1.0f, backlight() - 0.25f) * 0.3f); }
};

/** A key of the control surface (as opposed to the touch screen): a rounded
    hardware key with its label printed on it ('\n' between lines), backlit in
    `ledColour` while `ledState` returns Lit, flashing at 2 Hz for Blink.

    A short press runs `onPress`; holding the key for kHoldMs runs `onHold`
    instead (and the release then does nothing). Right-click and Ctrl-click are
    the mouse's "hold". A key without `onHold` acts as soon as it's pressed,
    like the console's. */
class SurfaceKey : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    enum Led
    {
        Off = 0,
        Lit = 1,
        Blink = 2
    };

    enum class Shape
    {
        Rect,   // SELECT, SOLO, MUTE, layer keys
        Square, // the monitoring and EQ keys
        Round   // VIEW
    };

    static constexpr int kHoldMs = 500;

    explicit SurfaceKey (const juce::String& keyLabel = {}, std::optional<Icon> keyIcon = std::nullopt)
        : label (keyLabel), icon (keyIcon)
    {
        setRepaintsOnMouseActivity (true);
        startTimerHz (12);
    }

    std::function<void()> onPress, onHold;
    std::function<int()> ledState;
    juce::Colour ledColour = Colours::ledYellow;
    Shape shape = Shape::Rect;
    /** A key printed dark (LISTEN, DAW). */
    bool dark = false;
    /** A triangle printed before (-1) or after (+1) the label: the shift keys'
        "◄4" / "4►". Without a label: a triangle pointing left (-1), right (+1),
        up (-2) or down (+2). */
    int arrow = 0;

    void setLabel (const juce::String& newLabel)
    {
        if (label != newLabel)
        {
            label = newLabel;
            repaint();
        }
    }
    const juce::String& getLabel() const noexcept { return label; }
    void setIcon (std::optional<Icon> newIcon) { icon = newIcon; repaint(); }

    /** Whether the LED shows lit right now (Blink alternates). */
    bool isShownLit() const noexcept { return shownLit; }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        if (shape != Shape::Rect)
            r = r.withSizeKeepingCentre (juce::jmin (r.getWidth(), r.getHeight()), juce::jmin (r.getWidth(), r.getHeight()));
        const bool lit = shownLit && isEnabled();
        const bool down = isMouseButtonDown() && isEnabled();

        // The WING's keys: a frosted light grey cap (dark keys: black with a
        // printed frame) in a black bezel, the print dark. Lit, the cap takes
        // the LED colour and the bezel glows.
        const float bezel = juce::jlimit (1.5f, 3.0f, juce::jmin (r.getWidth(), r.getHeight()) * 0.09f);
        auto shapeOf = [this] (juce::Rectangle<float> area)
        {
            juce::Path p;
            if (shape == Shape::Round)
                p.addEllipse (area);
            else
                p.addRoundedRectangle (area, juce::jmin (4.0f, area.getHeight() * 0.2f));
            return p;
        };
        const auto outer = shapeOf (r);
        const auto capArea = r.reduced (bezel);
        const auto cap = shapeOf (capArea);

        g.setColour (Colours::keyBezel);
        g.fillPath (outer);
        if (lit)
        {
            g.setColour (ledColour.withAlpha (0.55f * juce::jlimit (0.3f, 1.0f, SurfaceLighting::leds() / 0.9f)));
            g.fillPath (outer);
        }

        const auto unlitCap = SurfaceLighting::unlit (dark ? Colours::keyCapDark : Colours::keyCap);
        auto body = lit ? SurfaceLighting::lit (ledColour, unlitCap) : unlitCap;
        if (! isEnabled() && ! dark)
            body = body.interpolatedWith (Colours::panel, 0.15f);
        if (down)
            body = body.darker (0.25f);
        else if (isMouseOver() && isEnabled())
            body = body.brighter (0.07f);
        g.setGradientFill (juce::ColourGradient (body.brighter (dark ? 0.05f : 0.1f), 0.0f, capArea.getY(),
                                                 body.darker (dark ? 0.05f : 0.12f), 0.0f, capArea.getBottom(), false));
        g.fillPath (cap);
        if (dark && ! lit)
        {
            g.setColour (Colours::silk.withAlpha (isEnabled() ? 0.85f : 0.35f));
            g.strokePath (shapeOf (capArea.reduced (juce::jmax (1.0f, bezel * 0.5f))), juce::PathStrokeType (1.2f));
        }

        juce::Colour ink;
        if (lit)
            ink = body.getPerceivedBrightness() > 0.55f ? body.darker (0.9f).withMultipliedSaturation (1.2f) : juce::Colours::white.withAlpha (0.9f);
        else if (dark)
            ink = isEnabled() ? Colours::silk : Colours::silk.withAlpha (0.35f);
        else
            ink = isEnabled() ? Colours::keyPrint : Colours::keyPrint.withAlpha (0.4f);
        auto content = capArea.reduced (juce::jmin (3.0f, capArea.getWidth() * 0.06f), 0.5f);
        if (label.isEmpty() && arrow != 0)
        {
            const auto t = content.withSizeKeepingCentre (content.getHeight() * 0.4f, content.getHeight() * 0.4f);
            juce::Path triangle;
            switch (arrow)
            {
                case -1: triangle.addTriangle (t.getX(), t.getCentreY(), t.getRight(), t.getY(), t.getRight(), t.getBottom()); break;
                case 1:  triangle.addTriangle (t.getRight(), t.getCentreY(), t.getX(), t.getY(), t.getX(), t.getBottom()); break;
                case -2: triangle.addTriangle (t.getCentreX(), t.getY(), t.getRight(), t.getBottom(), t.getX(), t.getBottom()); break;
                default: triangle.addTriangle (t.getCentreX(), t.getBottom(), t.getX(), t.getY(), t.getRight(), t.getY()); break;
            }
            g.setColour (ink);
            g.fillPath (triangle);
        }
        if (icon.has_value())
        {
            const float s = juce::jmin (content.getHeight() * 0.62f, content.getWidth() * 0.7f);
            auto iconBox = label.isEmpty() ? content.withSizeKeepingCentre (s, s)
                                           : content.removeFromLeft (s).withSizeKeepingCentre (s, s);
            drawIcon (g, *icon, iconBox, ink);
        }
        if (label.isNotEmpty())
        {
            const int lines = juce::StringArray::fromLines (label).size();
            const float size = juce::jlimit (7.0f, 16.0f, content.getHeight() * (lines > 1 ? 0.46f : 0.6f));
            g.setColour (ink);
            g.setFont (silkFont (size));
            if (arrow != 0)
            {
                // Triangle and label centred together.
                const float tw = size * 0.7f, gap = size * 0.2f;
                const float textW = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), label);
                auto group = content.withSizeKeepingCentre (tw + gap + textW, content.getHeight());
                auto tri = (arrow < 0 ? group.removeFromLeft (tw) : group.removeFromRight (tw)).withSizeKeepingCentre (tw, size * 0.8f);
                juce::Path triangle;
                if (arrow < 0)
                    triangle.addTriangle (tri.getX(), tri.getCentreY(), tri.getRight(), tri.getY(), tri.getRight(), tri.getBottom());
                else
                    triangle.addTriangle (tri.getRight(), tri.getCentreY(), tri.getX(), tri.getY(), tri.getX(), tri.getBottom());
                g.fillPath (triangle);
                g.drawText (label, group, arrow < 0 ? juce::Justification::centredRight : juce::Justification::centredLeft);
                return;
            }
            g.drawFittedText (label, content.toNearestInt(), juce::Justification::centred, lines, 0.6f);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! isEnabled())
            return;
        // Pressing any key of the surface closes the screen's pop-ups.
        TransientPopups::closeAll();
        pressed = false;
        holdDone = false;
        if (e.mods.isPopupMenu() || e.mods.isCommandDown())
        {
            holdDone = true;
            fire (onHold != nullptr ? onHold : onPress);
            return;
        }
        pressed = true;
        downTime = juce::Time::getMillisecondCounter();
        if (onHold == nullptr)
        {
            holdDone = true;
            fire (onPress);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const bool shortPress = pressed && ! holdDone && getLocalBounds().contains (e.getPosition());
        pressed = false;
        if (shortPress)
            fire (onPress);
    }

private:
    void fire (std::function<void()> f)
    {
        // The callback may rebuild (and delete) this key: run a copy.
        juce::Component::SafePointer<SurfaceKey> safe (this);
        if (f != nullptr)
            f();
        if (safe != nullptr)
            refreshLed();
    }

    void refreshLed()
    {
        const int state = ledState != nullptr ? ledState() : Off;
        const bool lit = state == Lit || (state == Blink && (juce::Time::getMillisecondCounter() / 250) % 2 == 0);
        if (lit != shownLit)
        {
            shownLit = lit;
            repaint();
        }
    }

    void timerCallback() override
    {
        if (pressed && ! holdDone && onHold != nullptr && juce::Time::getMillisecondCounter() - downTime >= (juce::uint32) kHoldMs)
        {
            holdDone = true;
            fire (onHold);
            return;
        }
        refreshLed();
    }

    juce::String label;
    std::optional<Icon> icon;
    juce::uint32 downTime = 0;
    bool pressed = false, holdDone = false, shownLit = false;
};

/** A round VIEW key: lit green while its screen is shown (`isActive`). */
class ViewKey final : public SurfaceKey
{
public:
    ViewKey() : SurfaceKey ("VIEW")
    {
        shape = Shape::Round;
        ledColour = Colours::viewGreen;
        ledState = [this] { return isActive != nullptr && isActive() ? Lit : Off; };
    }

    std::function<bool()> isActive;
};

} // namespace livedaw::ui
