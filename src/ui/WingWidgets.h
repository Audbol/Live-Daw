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
            const int lines = juce::StringArray::fromLines (getButtonText()).size();
            g.setColour (ink);
            g.setFont (wingFontPx (wingKeyCap (r.getHeight(), lines > 1 ? 0.26f : 0.34f)));
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

        const auto unlitBody = SurfaceLighting::unlit (dark ? juce::Colour (0xff141517) : Colours::keyBody);
        auto body = lit ? SurfaceLighting::lit (ledColour, unlitBody) : unlitBody;
        if (down)
            body = body.darker (0.25f);
        else if (isMouseOver() && isEnabled())
            body = body.brighter (0.08f);

        juce::Path p;
        if (shape == Shape::Round)
            p.addEllipse (r);
        else
            p.addRoundedRectangle (r, juce::jmin (4.0f, r.getHeight() * 0.18f));
        g.setColour (body);
        g.fillPath (p);
        if (! lit)
        {
            // A slight bevel: lighter top edge.
            g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.07f), 0.0f, r.getY(),
                                                     juce::Colours::transparentWhite, 0.0f, r.getCentreY(), false));
            g.fillPath (p);
        }
        g.setColour (dark ? Colours::silk.withAlpha (0.6f) : Colours::keyEdge);
        g.strokePath (p, juce::PathStrokeType (1.0f));

        const auto ink = lit ? juce::Colours::black
                             : (isEnabled() ? Colours::silk : Colours::silk.withAlpha (0.3f));
        auto content = r.reduced (juce::jmin (4.0f, r.getWidth() * 0.08f), 1.0f);
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
            const float size = juce::jlimit (7.0f, 16.0f, content.getHeight() * (lines > 1 ? 0.36f : 0.46f));
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
            g.drawFittedText (label, content.toNearestInt(), juce::Justification::centred, lines, 0.7f);
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
