#pragma once

#include "ui/Icons.h"
#include "ui/LookAndFeel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <optional>

namespace livedaw::ui
{

/** A flat screen key like the console's: grey, optional icon left of the text,
    lit in `onColour` (with black ink) while `isOn` returns true. */
class WingKey : public juce::Button, private juce::Timer
{
public:
    WingKey (const juce::String& keyText, std::optional<Icon> keyIcon = std::nullopt, juce::Colour on = Colours::yellow)
        : juce::Button (keyText), icon (keyIcon), onColour (on)
    {
        startTimerHz (6);
    }

    std::function<bool()> isOn;
    juce::Colour offColour = juce::Colour (0xff5a5a5a);
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
        if (outlined)
        {
            g.setColour (juce::Colour (0xffbdbdbd));
            g.drawRect (r, 1.0f);
        }
        const auto ink = on ? juce::Colours::black : (isEnabled() ? Colours::text : Colours::textDim);
        if (iconAbove && icon.has_value())
        {
            auto label = r.removeFromBottom (r.getHeight() * 0.36f);
            drawIcon (g, *icon, r.reduced (r.getHeight() * 0.12f), ink);
            g.setColour (ink);
            g.setFont (juce::FontOptions (juce::jlimit (10.0f, 15.0f, label.getHeight() * 0.8f), juce::Font::bold));
            g.drawFittedText (getButtonText(), label.toNearestInt().reduced (2, 0), juce::Justification::centredTop, 1);
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
            g.setColour (ink);
            g.setFont (juce::FontOptions (juce::jlimit (12.0f, 22.0f, r.getHeight() * 0.46f), juce::Font::bold));
            g.drawFittedText (getButtonText(), content.toNearestInt(), juce::Justification::centred, 2);
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

/** The console's "ON" chip: yellow while the bound flag is set, click toggles it. */
class OnChip final : public WingKey
{
public:
    explicit OnChip (std::atomic<bool>& flag) : WingKey ("ON"), value (flag)
    {
        isOn = [this] { return value.load(); };
        offColour = juce::Colour (0xff4b4b4b);
        outlined = true;
        onClick = [this] { value = ! value.load(); };
    }

private:
    std::atomic<bool>& value;
};

/** A row of keys of which one is lit (NORM | FINE | AUTO on the console). */
class SegmentedKeys final : public juce::Component
{
public:
    SegmentedKeys (juce::StringArray labels, std::function<int()> get, std::function<void (int)> set, juce::Colour on = Colours::yellow)
        : names (std::move (labels)), getter (std::move (get)), setter (std::move (set)), onColour (on)
    {
    }

    void paint (juce::Graphics& g) override
    {
        const int current = getter != nullptr ? getter() : -1;
        auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff1e1e1e));
        g.fillRect (r);
        const float w = r.getWidth() / (float) juce::jmax (1, names.size());
        for (int i = 0; i < names.size(); ++i)
        {
            auto k = juce::Rectangle<float> (r.getX() + w * (float) i, r.getY(), w, r.getHeight()).reduced (1.0f);
            const bool on = i == current;
            g.setColour (on ? onColour : juce::Colour (0xff4b4b4b));
            g.fillRect (k);
            g.setColour (on ? juce::Colours::black : Colours::text);
            g.setFont (juce::FontOptions (juce::jlimit (12.0f, 22.0f, k.getHeight() * 0.48f), juce::Font::bold));
            g.drawFittedText (names[i], k.toNearestInt().reduced (2, 0), juce::Justification::centred, 1);
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
    juce::Colour ledColour = Colours::yellow;
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

        auto body = lit ? ledColour : (dark ? juce::Colour (0xff141517) : Colours::keyBody);
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
            g.setFont (juce::FontOptions (size, juce::Font::bold));
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
