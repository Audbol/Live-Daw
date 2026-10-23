#include "ui/MainDisplay.h"
#include "dsp/DspUtils.h"
#include "ui/HomeWidgets.h"
#include "ui/Icons.h"
#include "ui/WingPopups.h"

namespace livedaw::ui
{

namespace
{
    // The keys left of the display.
    constexpr Screen screens[] = { Screen::Home, Screen::Effects, Screen::Meters, Screen::Routing,
                                   Screen::Setup, Screen::Library, Screen::Utility };
    // Every page, including the VIEW screens.
    constexpr Screen allScreens[] = { Screen::Home,    Screen::Effects,  Screen::Meters, Screen::Routing,
                                      Screen::Setup,   Screen::Library,  Screen::Utility, Screen::Overview,
                                      Screen::Faders,  Screen::Sends,    Screen::Config,  Screen::MuteGroups,
                                      Screen::Recorder, Screen::UsbRecorder };

    // SETUP's tabs (ctx.showSetupTab).
    constexpr int kSetupAudioTab = 1;
    constexpr int kSetupLogTab = 6;

    // SETUP > GENERAL's LIGHTS tiles (percent, defaults as SETUP shows them).
    constexpr const char* kMainLcd = "setup.brightness.mainLcd";
    constexpr int kMainLcdDefault = 80;

    const char* screenName (Screen s)
    {
        switch (s)
        {
            case Screen::Home:    return "HOME";
            case Screen::Effects: return "EFFECTS";
            case Screen::Meters:  return "METERS";
            case Screen::Routing: return "ROUTING";
            case Screen::Setup:   return "SETUP";
            case Screen::Library: return "LIBRARY";
            case Screen::Utility: return "UTILITY";
            case Screen::Overview:
            case Screen::Faders:
            case Screen::Sends:
            case Screen::Config:
            case Screen::MuteGroups:
                return viewScreenName (s);
            case Screen::Recorder: return "RECORDER";
            case Screen::UsbRecorder: return "USB RECORDER";
        }
        return "";
    }

    /** "8:39" and "AM", or "20:39" and "" in 24-hour mode. */
    std::pair<juce::String, juce::String> clockText (juce::PropertiesFile& settings, juce::Time t)
    {
        if (uiprefs::use24h (settings))
            return { t.formatted ("%H:%M"), {} };
        const int h = t.getHours() % 12;
        return { juce::String (h == 0 ? 12 : h) + t.formatted (":%M"), t.getHours() < 12 ? "AM" : "PM" };
    }

    /** A small badge on a corner of the title tile ("SOF" at the top right,
        "TALKBACK" at the bottom right). */
    void paintTabBadge (juce::Graphics& g, juce::Rectangle<float> tab, const juce::String& text, juce::Colour colour, bool top)
    {
        g.setColour (colour);
        g.drawRect (tab, 2.0f);
        const float h = juce::jmax (8.0f, tab.getHeight() * 0.3f);
        const auto font = wingFontPx (h * 0.56f, WingWeight::SemiBold);
        const float w = juce::GlyphArrangement::getStringWidth (font, text) + h * 0.8f;
        const auto badge = juce::Rectangle<float> (tab.getRight() - w, top ? tab.getY() : tab.getBottom() - h, w, h);
        g.fillRect (badge);
        g.setColour (juce::Colours::black);
        g.setFont (font);
        g.drawText (text, badge, juce::Justification::centred, false);
    }

    /** The style of a parameter-bar slot, completed from the binding where the
        page left it open, with the WING's colours by parameter type: levels
        (filled from the left) and frequencies blue, BAL blue, trim, pan and EQ
        gain white, width and Q green from the centre, tilt cyan from the
        centre, decay and reverb time red, pre-delay, multipliers and delay
        times green, distance and AP frequency yellow, preamp gain orange, and
        selectors gold with one lit segment. */
    KnobStyle resolveStyle (const KnobBinding& b, KnobStyle s)
    {
        const auto label = b.label.toUpperCase();
        const auto text = b.text ? b.text() : juce::String();
        const auto word = [&label] (const char* w) { return label == w || label.startsWith (juce::String (w) + " "); };
        const bool ms = text.endsWithIgnoreCase ("ms");
        const bool seconds = ! ms && text.endsWithIgnoreCase ("s") && text.containsAnyOf ("0123456789");
        const bool selector = b.steps > 1.0f && b.steps <= 24.0f;

        struct Choice
        {
            KnobStyle::Bar bar;
            juce::Colour colour;
        };
        const auto choose = [&]() -> Choice
        {
            if (word ("Q"))
                return { KnobStyle::Bar::Width, KnobColours::qGreen };
            if (label.contains ("WIDTH"))
                return { KnobStyle::Bar::Centre, KnobColours::green };
            if (label.contains ("TILT"))
                return { KnobStyle::Bar::Centre, KnobColours::cyan };
            if (word ("BAL") || word ("BALANCE"))
                return { KnobStyle::Bar::Thumb, KnobColours::blue };
            if (word ("PAN") || word ("TRIM"))
                return { KnobStyle::Bar::Thumb, KnobColours::white };
            if (label.contains ("DECAY") || word ("RT") || label.contains ("REV TIME") || (label.contains ("TIME") && seconds))
                return { KnobStyle::Bar::Fill, KnobColours::red };
            if (label.contains ("PRE DLY") || label.contains ("PREDLY") || label.contains ("PRE-DLY") || label.contains ("MULT")
                || ((label.contains ("TIME") || label.contains ("DELAY") || label.contains ("DLY")) && ms))
                return { KnobStyle::Bar::Thumb, KnobColours::timeGreen };
            if (word ("DIST") || label.contains ("AP FREQ") || word ("SAMPLES"))
                return { KnobStyle::Bar::Thumb, KnobColours::yellow };
            if (label.contains ("LOW CONT") || label.contains ("HIGH PROC"))
                return { KnobStyle::Bar::Fill, KnobColours::orange };
            if (label == "GAIN" && ! b.bipolar)
                return { KnobStyle::Bar::Thumb, KnobColours::orange };
            if (selector)
                return { KnobStyle::Bar::Segments, KnobColours::gold };
            if (b.steps > 1.0f)
                return { KnobStyle::Bar::Thumb, KnobColours::gold };
            if (text.endsWith ("Hz") || label.contains ("FREQ") || label.contains ("CUT"))
                return { KnobStyle::Bar::Thumb, KnobColours::blue };
            if (label.startsWith ("FX") && label.endsWith (" MIX")) // FX MIX, FX1 MIX: a level [D8]
                return { KnobStyle::Bar::Fill, KnobColours::blue };
            if ((text.endsWith ("dB") && ! b.bipolar) || text.contains ("oo") || text.contains (juce::String::fromUTF8 ("\xe2\x88\x9e")))
                return { KnobStyle::Bar::Fill, KnobColours::blue };
            if (b.bipolar)
                return { KnobStyle::Bar::Thumb, KnobColours::white };
            return { KnobStyle::Bar::Fill, KnobColours::white };
        };

        const auto c = choose();
        if (s.bar == KnobStyle::Bar::Auto)
            s.bar = c.bar;
        if (s.colour.isTransparent())
            s.colour = c.colour;
        return s;
    }
} // namespace

//==============================================================================
/** One of the six slots of the parameter bar, above its knob: the name on the
    left, the value on the right in the parameter's colour, and its track
    underneath. Drag sideways or turn the mouse wheel to change it (shift for
    fine steps), double-click to reset. Touching it outlines the control it
    changes on the screen. */
class MainDisplay::ParamBarSlot final : public juce::Component, private juce::Timer
{
public:
    ParamBarSlot()
    {
        setRepaintsOnMouseActivity (true);
        startTimerHz (20);
    }

    /** Hovered or dragged (`touched`); `pressed` when clicked. */
    std::function<void (bool touched, bool pressed)> onTouch;

    void setBinding (KnobBinding b, KnobStyle s)
    {
        binding = std::move (b);
        style = binding.isValid() ? resolveStyle (binding, s) : KnobStyle {};
        valueText = {};
        timerCallback();
        repaint();
    }

    const KnobBinding& getBinding() const noexcept { return binding; }
    const KnobStyle& getStyle() const noexcept { return style; }

    void paint (juce::Graphics& g) override
    {
        if (! binding.isValid())
            return;
        // WING geometry (of 800 px): the label and value on one line, the
        // track under them: a 2 px grey line with an 8x14 thumb, or an 8 px
        // fill bar / row of segments.
        const float screenH = getWingScreenHeight();
        const float thick = juce::jmax (3.0f, std::round (screenH * 0.01f));
        const float thumbH = juce::jmax (6.0f, std::round (screenH * 0.0175f));
        auto b = getLocalBounds();
        auto bar = b.removeFromBottom (juce::roundToInt (thumbH) + 2).toFloat();
        const bool touched = isMouseOverOrDragging() || focused || encoderTouched;
        // The label and value share the line: the font shrinks when both
        // wouldn't fit, and the value keeps its room on the right.
        const auto label = binding.label.toUpperCase();
        const float gap = juce::jmax (4.0f, screenH * 0.01f);
        auto font = wingFontPx (juce::jmin (TypeScale::paramLabel * screenH, (float) b.getHeight() * 0.5f));
        const float needed = juce::GlyphArrangement::getStringWidth (font, label) + juce::GlyphArrangement::getStringWidth (font, valueText) + gap;
        if (needed * 0.85f > (float) b.getWidth() && needed > 0.0f)
            font = font.withHeight (juce::jmax (6.0f, font.getHeight() * (float) b.getWidth() / (needed * 0.85f)));
        g.setFont (font);
        auto valueArea = b.removeFromRight (juce::jmin (b.getWidth() * 2 / 3, juce::roundToInt (juce::GlyphArrangement::getStringWidth (font, valueText)) + 1));
        b.removeFromRight (juce::roundToInt (gap));
        g.setColour (touched ? Colours::gold : Colours::caption);
        g.drawFittedText (label, b, juce::Justification::centredLeft, 1, 0.75f);
        // A negative (inverted) WIDTH turns red, as on the console.
        const auto colour = binding.label.containsIgnoreCase ("WIDTH") && valueText.trimStart().startsWithChar ('-')
                                ? KnobColours::red : style.colour;
        g.setColour (colour);
        g.drawFittedText (valueText, valueArea, juce::Justification::centredRight, 1, 0.75f);

        const float y = bar.getCentreY();
        const float x = bar.getX() + position * bar.getWidth();
        const auto line = [&] { g.setColour (KnobColours::track); g.fillRect (bar.getX(), y - 1.0f, bar.getWidth(), 2.0f); };
        switch (style.bar)
        {
            case KnobStyle::Bar::Segments:
            {
                const int n = juce::jlimit (2, 32, style.segments > 0 ? style.segments : (int) binding.steps);
                const int current = juce::roundToInt (position * (float) (n - 1));
                const float w = bar.getWidth() / (float) n;
                for (int i = 0; i < n; ++i)
                {
                    g.setColour (i == current ? colour : KnobColours::segmentOff);
                    g.fillRoundedRectangle (bar.getX() + (float) i * w + 0.5f, y - thick * 0.5f, juce::jmax (1.0f, w - 2.0f), thick, 1.0f);
                }
                return;
            }
            case KnobStyle::Bar::Fill:
            case KnobStyle::Bar::Centre:
            {
                line();
                const float from = style.bar == KnobStyle::Bar::Centre ? bar.getCentreX() : bar.getX();
                g.setColour (colour);
                g.fillRoundedRectangle (juce::jmin (from, x), y - thick * 0.5f, std::abs (x - from), thick, thick * 0.3f);
                if (style.bar == KnobStyle::Bar::Centre)
                {
                    g.setColour (Colours::text);
                    g.fillRect (bar.getCentreX() - 1.0f, y - thumbH * 0.5f, 2.0f, thumbH);
                }
                return;
            }
            case KnobStyle::Bar::Width:
            {
                // The lowest Q fills the track, a higher one less of it.
                line();
                const float half = (1.0f - position) * bar.getWidth() * 0.5f;
                g.setColour (colour);
                g.fillRoundedRectangle (bar.getCentreX() - half, y - thick * 0.5f, half * 2.0f, thick, thick * 0.3f);
                g.setColour (Colours::text);
                g.fillRect (bar.getCentreX() - 1.0f, y - thumbH * 0.5f, 2.0f, thumbH);
                return;
            }
            case KnobStyle::Bar::Thumb:
            case KnobStyle::Bar::Auto:
                line();
                g.setColour (colour);
                g.fillRoundedRectangle (juce::Rectangle<float> (thick * 0.9f, thumbH).withCentre ({ x, y }), thick * 0.3f);
                return;
        }
    }

    void setPosition (float p)
    {
        if (! binding.isValid() || ! binding.set)
            return;
        p = juce::jlimit (0.0f, 1.0f, p);
        if (binding.steps > 1.0f)
            p = std::round (p * (binding.steps - 1.0f)) / (binding.steps - 1.0f);
        binding.set (p);
        timerCallback();
    }

    float getPosition() const noexcept { return position; }

    /** The focus frame of USE CRSR/WHEEL is on this slot. */
    void setFocused (bool f)
    {
        if (focused != f)
        {
            focused = f;
            repaint();
        }
    }

    /** Its encoder under the screen is touched: the label lights. */
    void setEncoderTouched (bool t)
    {
        if (encoderTouched != t)
        {
            encoderTouched = t;
            repaint();
        }
    }

    /** Steps the value by `clicks` detents (one step of a stepped parameter,
        1 % of the range otherwise). */
    void nudge (int clicks)
    {
        const float step = binding.steps > 1.0f ? 1.0f / (binding.steps - 1.0f) : 0.01f;
        setPosition (position + (float) clicks * step);
    }

    void mouseEnter (const juce::MouseEvent&) override { touch (true, false); }
    void mouseExit (const juce::MouseEvent&) override
    {
        if (! isMouseButtonDown())
            touch (false, false);
    }
    void mouseDown (const juce::MouseEvent&) override
    {
        dragStart = position;
        touch (true, true);
    }
    void mouseUp (const juce::MouseEvent&) override { touch (isMouseOver(), false); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const float sensitivity = e.mods.isShiftDown() ? 0.2f : 1.0f;
        setPosition (dragStart + sensitivity * (float) (e.getDistanceFromDragStartX() - e.getDistanceFromDragStartY()) / (float) juce::jmax (100, getWidth() * 2));
    }
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (binding.reset)
            binding.reset();
        timerCallback();
    }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        const float step = binding.steps > 1.0f ? 1.0f / (binding.steps - 1.0f) : 0.01f;
        setPosition (position + (wheel.deltaY > 0 ? step : -step) * (binding.steps > 1.0f ? 1.0f : 2.0f));
    }

private:
    void touch (bool t, bool pressed)
    {
        if (onTouch)
            onTouch (t, pressed);
        repaint();
    }

    void timerCallback() override
    {
        if (! binding.isValid())
            return;
        const float p = binding.get();
        const auto t = binding.text ? binding.text() : juce::String();
        if (std::abs (p - position) > 1.0e-5f || t != valueText)
        {
            position = p;
            valueText = t;
            repaint();
        }
    }

    KnobBinding binding;
    KnobStyle style;
    float position = 0.0f, dragStart = 0.0f;
    juce::String valueText;
    bool focused = false, encoderTouched = false;
};

//==============================================================================
/** A rounded hardware key of the console beside the display (the screen keys,
    CLR SOLO, the blank ALT key); lit while toggled, the blank key with an LED.
    A short press runs `onPress`; holding it (half a second, or a right-click)
    runs `onHold` and keeps the key "held" for the console's two-key shortcuts:
    a right-click holds it for a few seconds, so that the mouse can press the
    second key. */
class MainDisplay::ConsoleKey final : public juce::Button, private juce::Timer
{
public:
    explicit ConsoleKey (const juce::String& keyText) : juce::Button (keyText) { startTimerHz (10); }

    juce::Colour litColour = Colours::keySelect; // a screen key lights green while its screen shows
    bool led = false; // the blank key: an LED instead of a lit key
    /** The screen keys and CLR SOLO: a black face in a rim that carries the
        light, the print lit too (lavender unlit, in the black glass round the
        display on either finish). */
    bool backlit = false;
    std::optional<Icon> icon; // printed instead of the text (the maximize key)
    std::function<void()> onPress, onHold;

    /** Held down, or held with a right-click in the last few seconds. */
    bool isHeld() const { return isDown() || juce::Time::getMillisecondCounter() < latchedUntil; }
    void releaseHold()
    {
        latchedUntil = 0;
        repaint();
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const bool lit = getToggleState();
        // On the black console the keys are backlit lavender, brightest in
        // the middle (the blank key stays dark round its LED).
        const auto& sc = surfaceColours();
        if (backlit)
        {
            // The rim and the print take the light; the face stays dark.
            const juce::Colour faceColour (0xff141416), rimUnlit (0xffaaa2e2);
            auto rimLight = lit ? SurfaceLighting::lit (litColour, faceColour) : SurfaceLighting::backlit (rimUnlit, faceColour);
            if (down)
                rimLight = rimLight.darker (0.2f);
            else if (over)
                rimLight = rimLight.brighter (0.06f);
            g.setColour (sc.keyBezel);
            g.fillRoundedRectangle (r, 5.0f);
            const auto ring = r.reduced (1.0f);
            const float rim = juce::jlimit (1.5f, 4.0f, juce::jmin (ring.getWidth(), ring.getHeight()) * 0.1f);
            g.setColour (rimLight);
            g.fillRoundedRectangle (ring, 4.5f);
            const auto face = ring.reduced (rim).withTrimmedBottom (rim * 0.4f);
            g.setColour (lit ? faceColour.interpolatedWith (litColour, 0.1f) : faceColour);
            g.fillRoundedRectangle (face, 3.5f);
            g.setColour (rimLight);
            const auto keyText = getButtonText();
            g.setFont (silkFont (juce::jlimit (10.0f, 15.0f, face.getHeight() * 0.3f)));
            g.drawFittedText (keyText, face.toNearestInt().reduced (2, 0), juce::Justification::centred, 2, 0.7f);
            return;
        }
        const bool backlitCap = ! led && ! sc.keyCapEdge.isTransparent();
        const auto unlitBody = SurfaceLighting::unlit (backlitCap ? sc.keyCap : juce::Colour (0xff2c2e32));
        auto fill = lit && ! led ? SurfaceLighting::lit (litColour, unlitBody) : unlitBody;
        if (down)
            fill = fill.brighter (0.15f);
        else if (over)
            fill = fill.brighter (0.06f);
        if (backlitCap && ! lit)
            g.setGradientFill (juce::ColourGradient (fill, r.getCentreX(), r.getCentreY(),
                                                     SurfaceLighting::unlit (sc.keyCapEdge), r.getX(), r.getY(), true));
        else
            g.setColour (fill);
        g.fillRoundedRectangle (r, 5.0f);
        const bool latched = juce::Time::getMillisecondCounter() < latchedUntil;
        g.setColour (latched ? Colours::gold : backlitCap ? sc.keyBezel : juce::Colour (0xff4a4d53));
        g.drawRoundedRectangle (r, 5.0f, latched ? 2.0f : 1.2f);
        if (led)
        {
            g.setColour (lit ? SurfaceLighting::lit (Colours::ledYellow, juce::Colour (0xff3a3a3a)) : juce::Colour (0xff3a3a3a));
            g.fillRoundedRectangle (r.reduced (r.getWidth() * 0.32f, r.getHeight() * 0.42f), 2.0f);
            return;
        }
        const auto ink = lit ? juce::Colours::black : backlitCap ? sc.keyPrint : sc.silk;
        g.setColour (ink);
        if (icon)
        {
            const float side = juce::jmin (r.getWidth(), r.getHeight()) * 0.62f;
            drawIcon (g, *icon, r.withSizeKeepingCentre (side, side), ink);
            return;
        }
        const auto keyText = getButtonText();
        const bool twoLines = keyText.containsChar ('\n');
        g.setFont (silkFont (juce::jlimit (10.0f, 15.0f, r.getHeight() * (twoLines ? 0.3f : 0.36f))));
        g.drawFittedText (keyText, r.toNearestInt().reduced (2, 0), juce::Justification::centred, twoLines ? 2 : 1, 0.7f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        // A key of the surface: the screen's pop-ups close.
        TransientPopups::closeAll();
        holdDone = false;
        if (e.mods.isPopupMenu() || e.mods.isCommandDown())
        {
            holdDone = true;
            latchedUntil = juce::Time::getMillisecondCounter() + 4000;
            repaint();
            if (onHold != nullptr)
                onHold();
            return;
        }
        downAt = juce::Time::getMillisecondCounter();
        juce::Button::mouseDown (e);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (holdDone && ! isDown())
            return;
        juce::Button::mouseUp (e);
    }

private:
    using juce::Button::clicked;
    void clicked() override
    {
        if (! holdDone && onPress != nullptr)
            onPress();
    }

    void timerCallback() override
    {
        if (isDown() && ! holdDone && juce::Time::getMillisecondCounter() - downAt >= 500u)
        {
            holdDone = true;
            if (onHold != nullptr)
                onHold();
        }
        const bool latched = juce::Time::getMillisecondCounter() < latchedUntil;
        if (latched != shownLatched)
        {
            shownLatched = latched;
            repaint();
        }
    }

    juce::uint32 downAt = 0, latchedUntil = 0;
    bool holdDone = false, shownLatched = false;
};

//==============================================================================
/** The knob right of the display: "fine adjustments after selecting an item".
    It changes the last touched control (a knob on the screen or a slot of the
    parameter bar) in steps a tenth of the normal size. */
class MainDisplay::ContextKnob final : public juce::Component
{
public:
    std::function<KnobBinding()> getTarget;
    std::function<void()> changed;
    /** Gets the turns first (the brightness shortcut); true: used up. */
    std::function<bool (int clicks)> intercept;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        auto knob = r.removeFromTop (juce::jmin (r.getWidth(), r.getHeight() - 14.0f)).reduced (3.0f);
        const float d = juce::jmin (knob.getWidth(), knob.getHeight());
        auto k = knob.withSizeKeepingCentre (d, d);
        g.setColour (juce::Colour (0xff111214));
        g.fillEllipse (k);
        // Knurled rim.
        g.setColour (juce::Colour (0xff3a3c40));
        for (int i = 0; i < 48; ++i)
        {
            const float a = (float) i / 48.0f * juce::MathConstants<float>::twoPi + angle;
            const auto c = k.getCentre();
            g.drawLine (c.x + std::sin (a) * d * 0.42f, c.y - std::cos (a) * d * 0.42f, c.x + std::sin (a) * d * 0.49f, c.y - std::cos (a) * d * 0.49f, 1.0f);
        }
        g.setColour (juce::Colour (0xff222428));
        g.fillEllipse (k.reduced (d * 0.1f));
        g.setColour (juce::Colour (0xff4a4d53));
        g.drawEllipse (k.reduced (d * 0.1f), 1.0f);
        const auto target = getTarget ? getTarget() : KnobBinding {};
        g.setColour (target.isValid() ? surfaceColours().silkDim : surfaceColours().silkDim.withAlpha (0.4f));
        g.setFont (silkFont (11.0f));
        g.drawFittedText (target.isValid() ? target.label.toUpperCase() : juce::String(), r.toNearestInt(), juce::Justification::centred, 1, 0.6f);
    }

    void mouseDown (const juce::MouseEvent&) override { lastY = 0; }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const int dy = e.getDistanceFromDragStartY() - lastY;
        lastY = e.getDistanceFromDragStartY();
        nudge ((float) -dy / 12.0f);
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override { nudge (w.deltaY > 0 ? 1.0f : -1.0f); }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        auto b = getTarget ? getTarget() : KnobBinding {};
        if (b.isValid() && b.reset)
            b.reset();
        if (changed)
            changed();
    }

private:
    /** `clicks` detents: one step of a stepped parameter, or 0.2 % of the range. */
    void nudge (float clicks)
    {
        accumulated += clicks;
        const float whole = std::trunc (accumulated);
        if (whole == 0.0f)
            return;
        accumulated -= whole;
        angle += whole * 0.08f;
        repaint();
        if (intercept && intercept ((int) whole))
            return;
        auto b = getTarget ? getTarget() : KnobBinding {};
        if (! b.isValid() || ! b.set)
            return;
        const float step = b.steps > 1.0f ? 1.0f / (b.steps - 1.0f) : 0.002f;
        b.set (juce::jlimit (0.0f, 1.0f, b.get() + whole * step));
        if (changed)
            changed();
    }

    int lastY = 0;
    float accumulated = 0.0f, angle = 0.0f;
};

//==============================================================================
/** One of the six touch encoders under the screen, centred under its slot of
    the parameter bar and bound to the same parameter. Touching it outlines the
    control it changes and lights the slot's label. */
class MainDisplay::Encoder final : public juce::Component
{
public:
    Encoder()
    {
        knob.setHardware (true);
        knob.addMouseListener (this, true);
        addAndMakeVisible (knob);
    }

    std::function<void (bool touched)> onTouch;

    void setBinding (const KnobBinding& b)
    {
        knob.setBinding (b);
        knob.setEnabled (b.isValid());
    }

    void resized() override { knob.setBounds (getLocalBounds()); }
    void mouseEnter (const juce::MouseEvent&) override { touch(); }
    void mouseExit (const juce::MouseEvent&) override { touch(); }
    void mouseDown (const juce::MouseEvent&) override { touch(); }
    void mouseUp (const juce::MouseEvent&) override { touch(); }

private:
    void touch()
    {
        const bool t = isMouseOverOrDragging (true) || knob.isMouseButtonDown (true);
        if (t != touched)
        {
            touched = t;
            if (onTouch)
                onTouch (t);
        }
    }

    DisplayKnob knob;
    bool touched = false;
};

//==============================================================================
/** The main L/R meter right of the display: 18 segments per side,
    CLIP -1 -2 -3 -4 -6 -8 -10 -12 -15 -18 -24 -30 -36 -42 -48 -54 -60. It shows
    the bus chosen with a right-click (MONITOR, or MAIN 1-4). */
class MainDisplay::MainMeter final : public juce::Component, private juce::Timer
{
public:
    explicit MainMeter (UiContext& c) : ctx (c)
    {
        source = juce::jlimit (0, kNumMains, ctx.settings.getIntValue ("ui.mainMeter", 0));
        startTimerHz (30);
    }

    static constexpr float thresholds[18] = { -0.1f, -1.0f, -2.0f, -3.0f, -4.0f, -6.0f, -8.0f, -10.0f, -12.0f,
                                              -15.0f, -18.0f, -24.0f, -30.0f, -36.0f, -42.0f, -48.0f, -54.0f, -60.0f };

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        auto header = r.removeFromTop (14.0f);
        auto labels = r.removeFromLeft (r.getWidth() * 0.42f);
        const float barW = juce::jmin (12.0f, (r.getWidth() - 6.0f) / 2.0f);
        const float x0 = r.getX() + 2.0f;
        g.setFont (silkFont (juce::jlimit (8.0f, 11.0f, r.getHeight() / 22.0f)));
        g.setColour (surfaceColours().silkDim);
        g.drawText ("L", juce::Rectangle<float> (x0, header.getY(), barW, header.getHeight()).toNearestInt(), juce::Justification::centred);
        g.drawText ("R", juce::Rectangle<float> (x0 + barW + 4.0f, header.getY(), barW, header.getHeight()).toNearestInt(), juce::Justification::centred);
        const float segH = r.getHeight() / 18.0f;
        for (int s = 0; s < 18; ++s)
        {
            const float y = r.getY() + (float) s * segH;
            g.setColour (surfaceColours().silkDim);
            g.drawText (s == 0 ? juce::String ("CLIP") : juce::String ((int) thresholds[s]), labels.withY (y).withHeight (segH).toNearestInt(), juce::Justification::centredRight);
            for (int ch = 0; ch < 2; ++ch)
            {
                const bool lit = level[(size_t) ch] >= thresholds[s];
                const auto on = s == 0 ? Colours::meterRed : (s < 5 ? Colours::meterYellow : Colours::meterGreen);
                g.setColour (lit ? on : juce::Colour (0xff1d1f22));
                g.fillRect (x0 + (float) ch * (barW + 4.0f), y + 1.0f, barW, segH - 2.0f);
            }
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() && e.getNumberOfClicks() < 2)
            return;
        juce::StringArray items { "MONITOR" };
        for (int i = 1; i <= kNumMains; ++i)
            items.add ("MAIN " + juce::String (i));
        popups::ListOptions options;
        options.caption = "MAIN METER";
        popups::chooseFrom (*this, items, source, [this] (int i) { choose (i); }, options);
    }

private:
    void choose (int s)
    {
        source = s;
        ctx.settings.setValue ("ui.mainMeter", s);
    }

    void timerCallback() override
    {
        // SETUP can change the source too.
        source = juce::jlimit (0, kNumMains, ctx.settings.getIntValue ("ui.mainMeter", source));
        bool changed = false;
        for (int ch = 0; ch < 2; ++ch)
        {
            float peak = 0.0f;
            if (source == 0)
                peak = ctx.meters.getMonitor (ch);
            else if (source - 1 < (int) ctx.engine.getMains().size())
                peak = ctx.meters.get (ctx.engine.getMains()[(size_t) (source - 1)].get(), false, ch);
            const float db = dsp::gainToDb (peak);
            if (std::abs (db - level[(size_t) ch]) > 0.3f)
            {
                level[(size_t) ch] = db;
                changed = true;
            }
        }
        if (changed)
            repaint();
    }

    UiContext& ctx;
    int source = 0;
    std::array<float, 2> level { -100.0f, -100.0f };
};

//==============================================================================
/** Draws a gold outline round the control the touched knob changes, and a
    thinner one round the control the fine-adjust knob (or the USE CRSR/WHEEL
    focus) is on. */
class MainDisplay::Highlight final : public juce::Component, private juce::Timer
{
public:
    Highlight()
    {
        setInterceptsMouseClicks (false, false);
        startTimerHz (10);
    }

    juce::Component::SafePointer<juce::Component> touched, focus;
    std::function<bool()> suppressed;

    void paint (juce::Graphics& g) override
    {
        if (suppressed && suppressed())
            return;
        auto outline = [&] (juce::Component* c, float thickness, float alpha)
        {
            if (c == nullptr || ! c->isShowing() || ! getParentComponent()->isParentOf (c))
                return;
            const auto area = getLocalArea (c, c->getLocalBounds()).toFloat().expanded (2.0f);
            g.setColour (Colours::gold.withAlpha (alpha));
            g.drawRect (area, thickness);
        };
        outline (focus.getComponent(), 1.5f, 0.75f);
        outline (touched.getComponent(), 2.0f, 1.0f);
    }

private:
    void timerCallback() override
    {
        auto boundsOf = [this] (juce::Component* c) { return c != nullptr && c->isShowing() ? getLocalArea (c, c->getLocalBounds()) : juce::Rectangle<int>(); };
        const auto now = std::make_pair (boundsOf (touched.getComponent()), boundsOf (focus.getComponent()));
        if (now != shown)
        {
            shown = now;
            repaint();
        }
    }

    std::pair<juce::Rectangle<int>, juce::Rectangle<int>> shown;
};

//==============================================================================
/** CHANNEL SELECT (p. 30, firmware 1.09 quick select): every channel, aux, bus,
    main and matrix as 112x64 tiles with input meters; the channels eight to a
    row, aux and buses below them, the mains and matrices in two columns at the
    right. Choose one to open its HOME. No title bar: Esc or the number box
    closes it. */
class MainDisplay::ChannelSelect final : public juce::Component, private juce::Timer
{
public:
    explicit ChannelSelect (UiContext& c) : ctx (c)
    {
        setWantsKeyboardFocus (true);
    }

    std::function<void()> chosen, onClose;

    void visibilityChanged() override
    {
        if (isVisible())
            startTimerHz (15);
        else
            stopTimer();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black);
        tiles.clear();
        auto* selected = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr;

        // WING pixels: the page is 1280 x 720 (y 80..800 of the screen).
        const int chRows = juce::jmax (1, ((int) ctx.engine.getChannels().size() + 7) / 8);
        const int auxRows = juce::jmax (1, ((int) ctx.engine.getAux().size() + 7) / 8);
        const int busRows = juce::jmax (1, ((int) ctx.engine.getBuses().size() + 7) / 8);
        const float needed = 32.0f + 72.0f * (float) (chRows + auxRows + busRows) + 40.0f + 32.0f;
        const float kx = (float) getWidth() / 1280.0f;
        const float ky = (float) getHeight() / 720.0f * juce::jmin (1.0f, 712.0f / needed);
        auto tileAt = [&] (float x, float y) { return juce::Rectangle<float> (x * kx, y * ky, 112.0f * kx, 64.0f * ky); };

        auto group = [&] (StripKind kind, float x0, float y0, int columns)
        {
            const auto& list = ctx.engine.getStrips (kind);
            for (int i = 0; i < (int) list.size(); ++i)
            {
                const auto& s = *list[(size_t) i];
                const auto r = tileAt (x0 + 120.0f * (float) (i % columns), y0 + 72.0f * (float) (i / columns));
                paintWingTile (g, r, tileInfo (s, &s == selected));
                tiles.push_back ({ r.toNearestInt(), list[(size_t) i].get() });
            }
        };
        group (StripKind::Channel, 20.0f, 32.0f, 8);
        const float auxY = 32.0f + 72.0f * (float) chRows + 40.0f;
        group (StripKind::Aux, 20.0f, auxY, 8);
        group (StripKind::Bus, 20.0f, auxY + 72.0f * (float) auxRows + 32.0f, 8);
        group (StripKind::Main, 1004.0f, 32.0f, 1);
        group (StripKind::Matrix, 1148.0f, 32.0f, 1);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        for (auto& [area, strip] : tiles)
            if (area.contains (e.getPosition()))
            {
                if (ctx.selectStrip)
                    ctx.selectStrip (strip);
                if (chosen)
                    chosen();
                return;
            }
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey && onClose)
        {
            onClose();
            return true;
        }
        return false;
    }

private:
    WingTileInfo tileInfo (const Strip& s, bool selected) const
    {
        WingTileInfo t;
        t.colour = stripDisplayColour (s);
        t.name = home::hasDefaultName (s) ? juce::String() : stripName (s);
        t.label = home::shortLabel (s);
        t.boxed = s.isInput();
        t.wingIcon = s.getIcon();
        if ((s.isInput() && stripHasSource (s)) || s.getKind() == StripKind::Main)
            t.kindIcon = stripIcon (s.getKind());
        // Firmware 3.0: "Channel select overlay metering set to input".
        float peak = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            peak = juce::jmax (peak, ctx.meters.get (&s, true, ch));
        t.meter = juce::jlimit (0.0f, 1.0f, (dsp::gainToDb (peak) + 60.0f) / 60.0f);
        t.selected = selected;
        return t;
    }

    void timerCallback() override { repaint(); }

    UiContext& ctx;
    std::vector<std::pair<juce::Rectangle<int>, Strip*>> tiles;
};

//==============================================================================
/** Notices clicks on knobs anywhere on the display: the fine-adjust knob then
    works on that knob. */
class MainDisplay::FocusTracker final : public juce::MouseListener
{
public:
    explicit FocusTracker (MainDisplay& d) : display (d) { juce::Desktop::getInstance().addGlobalMouseListener (this); }
    ~FocusTracker() override { juce::Desktop::getInstance().removeGlobalMouseListener (this); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (auto* c = e.eventComponent; c != nullptr && c != &display; c = c->getParentComponent())
        {
            if (! display.isParentOf (c))
                return;
            if (auto* k = dynamic_cast<DisplayKnob*> (c))
            {
                display.setFocus (k->getBinding(), k);
                return;
            }
            if (auto* k = dynamic_cast<home::KnobBox*> (c))
            {
                display.setFocus (k->getBinding(), k);
                return;
            }
        }
    }

private:
    MainDisplay& display;
};

//==============================================================================
/** A layer over the screen: the pop-up layer (clicks pass through where there's
    no pop-up), the touch blocker (swallows every click while touch is off) or
    the MAIN LCD brightness scrim (translucent black, never takes clicks). */
class MainDisplay::Overlay final : public juce::Component
{
public:
    enum class Kind
    {
        Popups,
        Blocker,
        Scrim
    };

    explicit Overlay (Kind k) : kind (k) { setInterceptsMouseClicks (kind == Kind::Blocker, kind == Kind::Popups); }

    ~Overlay() override
    {
        // Pop-ups still open live in this layer: they go with it.
        while (getNumChildComponents() > 0)
            delete getChildComponent (0);
    }

    float alpha = 0.0f;

    void paint (juce::Graphics& g) override
    {
        if (kind == Kind::Scrim && alpha > 0.002f)
            g.fillAll (juce::Colours::black.withAlpha (alpha));
    }

private:
    Kind kind;
};

//==============================================================================
MainDisplay::MainDisplay (UiContext& c) : ctx (c)
{
    home = std::make_unique<HomePage> (ctx);
    effects = std::make_unique<EffectsPage> (ctx);
    meters = std::make_unique<MetersPage> (ctx);
    routing = std::make_unique<RoutingPage> (ctx);
    setup = std::make_unique<SetupPage> (ctx);
    library = std::make_unique<LibraryPage> (ctx);
    utility = std::make_unique<UtilityPage> (ctx);
    overview = std::make_unique<OverviewViewPage> (ctx);
    faders = std::make_unique<FadersViewPage> (ctx);
    sends = std::make_unique<SendsViewPage> (ctx);
    config = std::make_unique<ConfigViewPage> (ctx);
    muteGroups = std::make_unique<MuteGroupsViewPage> (ctx);
    jassert (ctx.recording != nullptr);
    recorder = std::make_unique<RecorderPage> (ctx, *ctx.recording);
    usbRecorder = std::make_unique<UsbRecorderPage> (ctx);

    for (auto s : allScreens)
    {
        auto* p = page (s);
        p->knobsChanged = [this, s] { if (screen == s) updateKnobs(); };
        p->statusKeysChanged = [this, s] { if (screen == s) updateFrame(); };
        addChildComponent (p);
    }

    for (auto s : screens)
    {
        auto* b = screenButtons.add (new ConsoleKey (screenName (s)));
        b->backlit = true;
        b->onPress = [this, s] { screenKeyPressed (s); };
        addAndMakeVisible (b);
    }
    // Hold HOME: surface lock.
    screenButtons[0]->onHold = [this]
    {
        if (ctx.setSurfaceLocked && ctx.isSurfaceLocked)
            ctx.setSurfaceLocked (! ctx.isSurfaceLocked());
    };

    for (int i = 0; i < 6; ++i)
    {
        auto* slot = slots.add (new ParamBarSlot());
        slot->onTouch = [this, i, slot] (bool touched, bool pressed)
        {
            if (touched)
            {
                touchedSlot = i;
                highlight->touched = slot->getStyle().target;
                if (pressed)
                    setFocus (slot->getBinding(), slot->getStyle().target.getComponent());
            }
            else if (touchedSlot == i)
            {
                touchedSlot = -1;
                highlight->touched = nullptr;
            }
            highlight->repaint();
        };
        addAndMakeVisible (slot);

        auto* encoder = encoders.add (new Encoder());
        encoder->onTouch = [this, i, slot] (bool touched)
        {
            slot->setEncoderTouched (touched);
            if (touched)
            {
                touchedSlot = i;
                highlight->touched = slot->getStyle().target;
            }
            else if (touchedSlot == i)
            {
                touchedSlot = -1;
                highlight->touched = nullptr;
            }
            highlight->repaint();
        };
        addAndMakeVisible (encoder);
    }

    // Right of the display: CLR SOLO, the main meter, the knob, the blank key.
    clearSoloKey = std::make_unique<ConsoleKey> ("CLR\nSOLO");
    clearSoloKey->litColour = Colours::keySolo; // an orange rim, blinking while anything is soloed
    clearSoloKey->backlit = true;
    clearSoloKey->onPress = [this] { clearSoloPressed(); };
    clearSoloKey->setTooltip ("Release every channel and bus on the solo bus");
    addAndMakeVisible (*clearSoloKey);
    mainMeter = std::make_unique<MainMeter> (ctx);
    addAndMakeVisible (*mainMeter);
    contextKnob = std::make_unique<ContextKnob>();
    contextKnob->intercept = [this] (int clicks)
    {
        // Hold ALT and turn the knob: the display's brightness (MAIN LCD).
        if (! blankKey->isHeld())
            return false;
        const int now = ctx.settings.getIntValue (kMainLcd, kMainLcdDefault);
        ctx.settings.setValue (kMainLcd, juce::jlimit (5, 100, now + clicks));
        applyLighting();
        return true;
    };
    addAndMakeVisible (*contextKnob);
    blankKey = std::make_unique<ConsoleKey> ("");
    blankKey->led = true;
    blankKey->onPress = [this] { page (screen)->blankKeyPressed(); };
    addAndMakeVisible (*blankKey);
    // Under the right-hand column, level with the touch encoders: only the
    // screen (and its keys and knobs) in the window, or back (also F10, a
    // double-click on an empty part of the status bar).
    maximizeKey = std::make_unique<ConsoleKey> ("");
    maximizeKey->icon = Icon::Maximize;
    maximizeKey->onPress = [this]
    {
        if (ctx.setScreenMaximized && ctx.isScreenMaximized)
            ctx.setScreenMaximized (! ctx.isScreenMaximized());
    };
    maximizeKey->setTooltip ("Show only the screen, as large as the window allows (F10; Esc returns)");
    addAndMakeVisible (*maximizeKey);

    channelSelect = std::make_unique<ChannelSelect> (ctx);
    channelSelect->onClose = [this] { showChannelSelect (false); };
    channelSelect->chosen = [this] { showChannelSelect (false); showHomeTab (HomeTab::Home); };
    addChildComponent (*channelSelect);

    highlight = std::make_unique<Highlight>();
    highlight->suppressed = [this] { return channelSelect != nullptr && channelSelect->isVisible(); };
    addAndMakeVisible (*highlight);
    focusTracker = std::make_unique<FocusTracker> (*this);

    popupLayer = std::make_unique<Overlay> (Overlay::Kind::Popups);
    addAndMakeVisible (*popupLayer);
    touchBlocker = std::make_unique<Overlay> (Overlay::Kind::Blocker);
    addChildComponent (*touchBlocker);
    lcdScrim = std::make_unique<Overlay> (Overlay::Kind::Scrim);
    lcdScrim->setInterceptsMouseClicks (false, false);
    addAndMakeVisible (*lcdScrim);

    popups::setPopupLayerProvider ([this] (juce::Component& host) -> juce::Component*
    {
        return &host == this || isParentOf (&host) ? popupLayer.get() : nullptr;
    });
    ctx.setFineTarget = [this] (const KnobBinding& b, juce::Component* target) { setFineTarget (b, target); };
    ctx.moveScreenFocus = [this] (int dx, int dy) { moveFocus (dx, dy); };
    ctx.turnScreenFocus = [this] (int steps) { turnFocus (steps); };

    applyLighting();
    showScreen (Screen::Home);
    startTimerHz (4);
}

MainDisplay::~MainDisplay()
{
    ctx.setFineTarget = nullptr;
    ctx.moveScreenFocus = nullptr;
    ctx.turnScreenFocus = nullptr;
    popups::setPopupLayerProvider (nullptr);
    focusTracker.reset();
}

DisplayPage* MainDisplay::page (Screen s) const
{
    switch (s)
    {
        case Screen::Home:    return home.get();
        case Screen::Effects: return effects.get();
        case Screen::Meters:  return meters.get();
        case Screen::Routing: return routing.get();
        case Screen::Setup:   return setup.get();
        case Screen::Library: return library.get();
        case Screen::Utility: return utility.get();
        case Screen::Overview: return overview.get();
        case Screen::Faders: return faders.get();
        case Screen::Sends: return sends.get();
        case Screen::Config: return config.get();
        case Screen::MuteGroups: return muteGroups.get();
        case Screen::Recorder: return recorder.get();
        case Screen::UsbRecorder: return usbRecorder.get();
    }
    return home.get();
}

void MainDisplay::showScreen (Screen s)
{
    screen = s;
    showChannelSelect (false);
    focusIndex = -1;
    for (auto sc : allScreens)
        page (sc)->setVisible (sc == s);
    // The screen keys light for their screen; UTILITY only where it does
    // something (the timer keeps it up to date).
    for (size_t i = 0; i < std::size (screens); ++i)
        if (screens[i] != Screen::Utility)
            screenButtons[(int) i]->setToggleState (screens[i] == s, juce::dontSendNotification);
    page (s)->refresh();
    updateFrame();
    updateKnobs();
    repaint();
}

void MainDisplay::showHomeTab (HomeTab t)
{
    if (screen != Screen::Home)
        showScreen (Screen::Home);
    showChannelSelect (false);
    home->showTab (t);
    updateKnobs();
    repaint();
}

void MainDisplay::screenKeyPressed (Screen s)
{
    if (s == Screen::Utility)
    {
        utilityPressed();
        return;
    }
    // A screen key pressed again: the page decides (SETUP: CC EDIT, firmware 3.1).
    if (s == screen && page (s)->screenKeyPressedAgain())
        return;
    showScreen (s);
}

void MainDisplay::utilityPressed()
{
    // Hold CLR SOLO and press UTILITY: a screenshot of the display.
    if (clearSoloKey->isHeld())
    {
        clearSoloKey->releaseHold();
        if (ctx.takeScreenshot)
            ctx.takeScreenshot();
        return;
    }
    // "The UTILITY button only lights up where it serves a specific function."
    if (auto* p = page (screen); p->hasUtilityFunction())
        p->utilityPressed();
    else if (ctx.showSetupTab)
        ctx.showSetupTab (kSetupLogTab); // system health and the event log
}

void MainDisplay::clearSoloPressed()
{
    // SETUP + ALT + CLR SOLO: touch off; ALT + CLR SOLO: touch on again.
    if (blankKey->isHeld())
    {
        const bool setupHeld = screenButtons[4]->isHeld();
        if (touchDisabled || setupHeld)
            setTouchDisabled (! touchDisabled);
        blankKey->releaseHold();
        screenButtons[4]->releaseHold();
        return;
    }
    clearSolo();
}

void MainDisplay::setTouchDisabled (bool off)
{
    touchDisabled = off;
    touchBlocker->setVisible (off);
    if (off)
        touchBlocker->toFront (false);
    lcdScrim->toFront (false);
    repaint (statusArea);
}

void MainDisplay::refresh()
{
    page (screen)->refresh();
    if (screen != Screen::Home)
        home->refresh();
    updateKnobs();
    if (channelSelect->isVisible())
        channelSelect->repaint();
    repaint (statusArea);
}

void MainDisplay::refreshLibrary()
{
    library->refresh();
    repaint (statusArea);
}

void MainDisplay::updateFrame()
{
    // A screen without keys of its own shows none, as on the WING.
    statusKeys = page (screen)->getStatusKeys();
    resized();
    repaint();
}

void MainDisplay::updateKnobs()
{
    auto* p = page (screen);
    const auto bindings = p->getKnobs();
    const auto styles = p->getKnobStyles();
    for (size_t i = 0; i < 6; ++i)
    {
        const auto b = i < bindings.size() && p->usesKnobs() ? bindings[i] : KnobBinding {};
        slots[(int) i]->setBinding (b, i < styles.size() ? styles[i] : KnobStyle {});
        encoders[(int) i]->setBinding (b);
    }
    if (touchedSlot >= 0)
        highlight->touched = slots[touchedSlot]->getStyle().target;
    highlight->repaint();
}

void MainDisplay::setFocus (const KnobBinding& b, juce::Component* target)
{
    contextKnob->getTarget = [b] { return b; };
    highlight->focus = target;
    contextKnob->repaint();
    highlight->repaint();
}

void MainDisplay::setFineTarget (const KnobBinding& b, juce::Component* target)
{
    setFocus (b, target);
}

//==============================================================================
// USE CRSR/WHEEL: the focus frame.

std::vector<juce::Component*> MainDisplay::focusTargets() const
{
    std::vector<juce::Component*> targets;
    for (auto* slot : slots)
        if (slot->isVisible() && slot->getBinding().isValid())
            targets.push_back (slot);

    // The knobs on the page, top to bottom, then left to right.
    std::vector<juce::Component*> knobs;
    std::function<void (juce::Component&)> collect = [&] (juce::Component& c)
    {
        for (auto* child : c.getChildren())
        {
            if (! child->isVisible())
                continue;
            if (dynamic_cast<DisplayKnob*> (child) != nullptr || dynamic_cast<home::KnobBox*> (child) != nullptr)
                knobs.push_back (child);
            else
                collect (*child);
        }
    };
    collect (*page (screen));
    std::sort (knobs.begin(), knobs.end(), [this] (juce::Component* a, juce::Component* b)
    {
        const auto ra = getLocalArea (a, a->getLocalBounds()), rb = getLocalArea (b, b->getLocalBounds());
        if (std::abs (ra.getY() - rb.getY()) > juce::jmin (ra.getHeight(), rb.getHeight()) / 2)
            return ra.getY() < rb.getY();
        return ra.getX() < rb.getX();
    });
    targets.insert (targets.end(), knobs.begin(), knobs.end());
    return targets;
}

void MainDisplay::moveFocus (int dx, int dy)
{
    const auto targets = focusTargets();
    if (targets.empty())
        return;
    int index = juce::jlimit (-1, (int) targets.size() - 1, focusIndex);
    if (index < 0)
        index = 0;
    else if (dx != 0)
        index = (index + dx + (int) targets.size()) % (int) targets.size();
    else if (dy != 0)
    {
        // The nearest target above or below the current one.
        const auto from = getLocalArea (targets[(size_t) index], targets[(size_t) index]->getLocalBounds()).getCentre();
        int best = index;
        float bestDistance = std::numeric_limits<float>::max();
        for (int i = 0; i < (int) targets.size(); ++i)
        {
            const auto c = getLocalArea (targets[(size_t) i], targets[(size_t) i]->getLocalBounds()).getCentre();
            const int ddy = c.y - from.y;
            if (ddy == 0 || (ddy > 0) != (dy > 0))
                continue;
            const float distance = (float) std::abs (ddy) + 2.0f * (float) std::abs (c.x - from.x);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = i;
            }
        }
        index = best;
    }
    focusIndex = index;

    auto* target = targets[(size_t) index];
    for (auto* slot : slots)
        slot->setFocused (slot == target);
    if (auto* slot = dynamic_cast<ParamBarSlot*> (target))
        setFocus (slot->getBinding(), slot->getStyle().target.getComponent() != nullptr ? slot->getStyle().target.getComponent() : slot);
    else if (auto* k = dynamic_cast<DisplayKnob*> (target))
        setFocus (k->getBinding(), k);
    else if (auto* box = dynamic_cast<home::KnobBox*> (target))
        setFocus (box->getBinding(), box);
}

void MainDisplay::turnFocus (int steps)
{
    if (focusIndex < 0)
        moveFocus (0, 0);
    auto b = contextKnob->getTarget ? contextKnob->getTarget() : KnobBinding {};
    if (! b.isValid() || ! b.set)
        return;
    // Like a knob: one step of a stepped parameter, 1 % of the range otherwise.
    const float step = b.steps > 1.0f ? 1.0f / (b.steps - 1.0f) : 0.01f;
    b.set (juce::jlimit (0.0f, 1.0f, b.get() + (float) steps * step));
}

//==============================================================================
void MainDisplay::clearSolo()
{
    for (int k = 0; k < kNumStripKinds; ++k)
        for (auto& s : ctx.engine.getStrips (static_cast<StripKind> (k)))
            s->params.solo = false;
    for (int i = 0; i < kNumDcas; ++i)
        ctx.engine.getDca (i).solo = false;
    // CLR SOLO also ends LISTEN (the HOME screens' band / key listening).
    ctx.engine.getAnalysis().stopListening();
}

void MainDisplay::showChannelSelect (bool show)
{
    if (channelSelect == nullptr || channelSelect->isVisible() == show)
        return;
    channelSelect->setVisible (show);
    if (show)
    {
        channelSelect->toFront (true);
        highlight->toFront (false);
        popupLayer->toFront (false);
        if (touchDisabled)
            touchBlocker->toFront (false);
        lcdScrim->toFront (false);
    }
    repaint (statusArea);
}

void MainDisplay::applyLighting()
{
    // SETUP > GENERAL's LIGHTS: MAIN LCD dims the screen below its default
    // (which shows the console's colours as its screen draws them); BTNS/LEDS
    // and BACKLIGHT the lit and unlit keys of the surface.
    const float lcd = (float) juce::jlimit (0, 100, ctx.settings.getIntValue (kMainLcd, kMainLcdDefault)) / (float) kMainLcdDefault;
    const float alpha = juce::jmax (0.0f, 1.0f - lcd) * 0.8f;
    if (std::abs (alpha - lcdScrim->alpha) > 0.002f)
    {
        lcdScrim->alpha = alpha;
        lcdScrim->repaint();
    }
    const float leds = (float) juce::jlimit (0, 100, ctx.settings.getIntValue ("setup.brightness.btnsLeds", 90)) / 100.0f;
    const float backlight = (float) juce::jlimit (0, 100, ctx.settings.getIntValue ("setup.brightness.backlight", 25)) / 100.0f;
    if (std::abs (leds - SurfaceLighting::leds()) > 0.001f || std::abs (backlight - SurfaceLighting::backlight()) > 0.001f)
    {
        SurfaceLighting::leds() = leds;
        SurfaceLighting::backlight() = backlight;
        if (auto* top = getTopLevelComponent())
            top->repaint();
    }
}

void MainDisplay::timerCallback()
{
    ++ticks;
    const auto now = juce::Time::getCurrentTime();
    const auto time = now.formatted ("%H:%M:%S") + (uiprefs::use24h (ctx.settings) ? "" : "h") + (uiprefs::dayFirst (ctx.settings) ? "d" : "");
    // Repaint the status bar when anything it shows changes.
    // Unsigned, so the mixing may wrap around.
    juce::uint32 hash = 0;
    if (auto* s = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr)
        hash = (juce::uint32) juce::DefaultHashFunctions::generateHash (describeStrip (*s) + juce::String (s->getColourIndex())
                                                                            + juce::String (s->getIcon()) + (stripHasSource (*s) ? "s" : ""),
                                                                        1 << 30);
    hash = hash * 31u + (juce::uint32) ctx.engine.getNumFaultedFx();
    hash = hash * 31u + (ctx.deviceManager.getCurrentAudioDevice() != nullptr ? 1u : 0u);
    hash = hash * 31u + (ctx.sendsOnFader ? 1u : 0u) + (ctx.settings.getBoolValue (uiprefs::showSofFrame, true) ? 2u : 0u);
    hash = hash * 31u + (juce::uint32) ctx.show.getCurrentSceneIndex() + (ctx.settings.getBoolValue (uiprefs::alwaysShowScene, false) ? 1000u : 0u);
    // The SIP badge and the talkback frame.
    hash = hash * 31u + (juce::uint32) ctx.engine.soloMode.load() * 4u + (ctx.engine.sipActive.load() ? 2u : 0u) + (ctx.engine.isTalking() ? 1u : 0u);
    hash = hash * 31u + (juce::uint32) (ctx.engine.talk.assignKind.load() + 1) * 4096u + (juce::uint32) ctx.engine.talk.assignIndex.load();
    {
        const auto usbState = ctx.engine.getUsbRecorder().getRecState(); // the USB icon
        hash = hash * 31u + (juce::uint32) usbState * 2u + (usbState == UsbRecorder::RecState::Paused && blink ? 1u : 0u);
    }
    if (ctx.recording != nullptr)
    {
        const auto recState = ctx.recording->getState();
        hash = hash * 31u + (juce::uint32) recState + (recState == Recorder::State::Recording && blink ? 7u : 0u);
        if (ticks % 8 == 0)
            liveWritable = ctx.recording->getProjectsFolder().hasWriteAccess();
        hash = hash * 31u + (liveWritable ? 1u : 0u);
    }

    // The chips tile flashes (1 Hz) while there are FX faults nobody has looked at.
    const int faults = ctx.engine.getNumFaultedFx();
    acknowledgedFaults = juce::jmin (acknowledgedFaults, faults);
    flash = faults > acknowledgedFaults && (ticks / 2) % 2 == 0;
    hash = hash * 31u + (flash ? 1u : 0u);

    // The title tile follows the page.
    const auto title = page (screen)->getStatusTitle();
    hash = hash * 31u + (juce::uint32) juce::DefaultHashFunctions::generateHash (title.prefix + "|" + title.text + juce::String ((int) title.kind)
                                                                                     + juce::String ((int) title.fill.getARGB()), 1 << 30);
    if (time != shownTime || hash != shownHash)
    {
        shownTime = time;
        shownHash = hash;
        repaint (statusArea);
    }

    // CLR SOLO blinks while anything is soloed; the blank key's LED shows
    // whether the screen gives it a function, UTILITY's likewise.
    blink = ! blink;
    clearSoloKey->setToggleState (ctx.engine.anySoloActive.load() && blink, juce::dontSendNotification);
    blankKey->setToggleState (page (screen)->hasBlankKeyFunction(), juce::dontSendNotification);
    screenButtons[(int) std::size (screens) - 1]->setToggleState (page (screen)->hasUtilityFunction(), juce::dontSendNotification);
    applyLighting();
}

juce::Rectangle<int> MainDisplay::wingRect (float x, float y, float w, float h) const
{
    // WING screen pixels (1280 x 800) to the screen area.
    const float kx = (float) screenArea.getWidth() / 1280.0f, ky = (float) screenArea.getHeight() / 800.0f;
    return juce::Rectangle<float> ((float) screenArea.getX() + x * kx, (float) screenArea.getY() + y * ky, w * kx, h * ky).toNearestInt();
}

//==============================================================================
// The status bar (firmware 3.1): the title tile, the audio status, USB, the
// WING-LIVE cards, the screen's keys and the clock, all #202020 tiles.

void MainDisplay::paintTitleTile (juce::Graphics& g)
{
    const float k = (float) screenArea.getHeight() / 800.0f;
    auto tab = channelTab.toFloat();
    const float corner = 4.0f * k;
    const float bigCap = TypeScale::statusTitle * (float) screenArea.getHeight();
    const auto bigFont = wingFontPx (bigCap, WingWeight::SemiBold);

    // ALWAYS SHOW ACTIVE SCENE: the scene's name on the SNAP colour.
    const int sceneIndex = ctx.show.getCurrentSceneIndex();
    if (ctx.settings.getBoolValue (uiprefs::alwaysShowScene, false) && sceneIndex >= 0)
    {
        if (const auto scene = ctx.show.getScene (sceneIndex))
        {
            g.setColour (FileColours::snap);
            g.fillRoundedRectangle (tab, corner);
            auto t = tab.reduced (12.0f * k, 6.0f * k);
            g.setColour (juce::Colours::black);
            g.setFont (wingFontPx (TypeScale::statusCaption * (float) screenArea.getHeight()));
            g.drawText ("INT:", t.removeFromTop (t.getHeight() * 0.3f), juce::Justification::centredLeft, false);
            g.setFont (bigFont);
            g.drawFittedText (scene->name, t.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
            return;
        }
    }

    const auto title = page (screen)->getStatusTitle();
    auto* strip = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr;
    auto t = tab.reduced (10.0f * k, 10.0f * k);
    const float h = t.getHeight();

    auto drawText = [&] (const juce::String& text, juce::Colour ink, juce::Rectangle<float>& area)
    {
        g.setColour (ink);
        g.setFont (bigFont);
        g.drawFittedText (text, area.toNearestInt(), juce::Justification::centredLeft, 1, 0.75f);
    };

    if (title.kind == StatusTitle::Kind::Text && ! title.fill.isTransparent())
    {
        // A filled title (LIBRARY: the active scene in its file type's colour):
        // the prefix as a small caption ("INT:", the show) over the name.
        const auto ink = ! title.ink.isTransparent() ? title.ink : juce::Colours::black;
        g.setColour (title.fill);
        g.fillRoundedRectangle (tab, corner);
        auto f = tab.reduced (12.0f * k, 6.0f * k);
        g.setColour (ink);
        if (title.prefix.isNotEmpty())
        {
            g.setFont (wingFontPx (TypeScale::statusCaption * (float) screenArea.getHeight()));
            g.drawFittedText (title.prefix, f.removeFromTop (f.getHeight() * 0.3f).toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
        }
        if (title.icon.has_value())
        {
            const float ih = f.getHeight() * 0.8f;
            drawIcon (g, *title.icon, f.removeFromLeft (ih).withSizeKeepingCentre (ih, ih).reduced (ih * 0.1f), ink);
            f.removeFromLeft (8.0f * k);
        }
        g.setFont (bigFont);
        g.drawFittedText (title.text, f.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
    }
    else if (title.kind == StatusTitle::Kind::Channel)
    {
        // The selected strip's tile, in its colour, black print [I1].
        const auto colour = strip != nullptr ? strip->getColour() : Colours::screenTile;
        const auto ink = juce::Colours::black;
        g.setColour (colour);
        g.fillRoundedRectangle (tab, corner);
        if (strip != nullptr)
        {
            const auto label = home::shortLabel (*strip);
            if (strip->isOutput())
            {
                // Buses, mains and matrices: the short label without a box, then the name.
                auto labelArea = t.removeFromLeft (juce::jmax (h * 2.0f, juce::GlyphArrangement::getStringWidth (bigFont, label) + 16.0f * k));
                drawText (label, ink, labelArea);
                if (! home::hasDefaultName (*strip))
                    drawText (stripName (*strip), ink, t);
            }
            else
            {
                // The number in a black box, in the strip's colour, filling the
                // box; without a source a black outline and a black number.
                const auto numberFont = wingFontPx (juce::jmin (bigCap, h * 0.66f), WingWeight::SemiBold);
                const float boxW = juce::jmax (60.0f * k, juce::GlyphArrangement::getStringWidth (numberFont, label) + 18.0f * k);
                auto box = t.removeFromLeft (boxW);
                g.setColour (juce::Colours::black);
                if (stripHasSource (*strip))
                {
                    g.fillRect (box);
                    g.setColour (colour); // exactly the strip's colour, dark ones too
                }
                else
                {
                    g.drawRect (box.reduced (1.5f * k), 3.0f * k);
                }
                g.setFont (numberFont);
                g.drawText (label, box, juce::Justification::centred, false);
                t.removeFromLeft (16.0f * k);
                // The WING icon (none: nothing), a linked source's own glyph.
                auto iconArea = t.removeFromLeft (h * 1.3f).withSizeKeepingCentre (h * 1.3f, h * 1.3f);
                if (! drawWingIcon (g, strip->getIcon(), iconArea, ink) && strip->linkedSource() != nullptr)
                    drawIcon (g, sourceIcon (strip->getActiveInputLeft()), iconArea.reduced (h * 0.08f), ink);
                t.removeFromLeft (12.0f * k);
                if (! home::hasDefaultName (*strip))
                    drawText (stripName (*strip), ink, t);
            }
        }
    }
    else
    {
        // ROUTING's sources and outputs (grey), the VIEW screens' layer (dark,
        // grey text), or a plain title (CUSTOM CONTROLS, MUTE GROUPS).
        const bool source = title.kind == StatusTitle::Kind::Source;
        const bool layer = title.kind == StatusTitle::Kind::Layer;
        const auto fill = ! title.fill.isTransparent() ? title.fill : (source ? juce::Colour (0xff8a8a8a) : Colours::screenTile);
        const auto ink = ! title.ink.isTransparent() ? title.ink : (source ? juce::Colours::black : (layer ? Colours::textDim : Colours::caption));
        g.setColour (fill);
        g.fillRoundedRectangle (tab, corner);
        t.removeFromLeft (6.0f * k);
        if (title.prefix.isNotEmpty() && title.kind == StatusTitle::Kind::Text)
        {
            // A plain title's prefix (LIBRARY: the show) is a small caption over it.
            auto f = tab.reduced (12.0f * k, 6.0f * k);
            g.setColour (ink);
            g.setFont (wingFontPx (TypeScale::statusCaption * (float) screenArea.getHeight()));
            g.drawFittedText (title.prefix, f.removeFromTop (f.getHeight() * 0.3f).toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
            t = f;
        }
        else if (title.prefix.isNotEmpty())
        {
            auto prefixArea = t.removeFromLeft (juce::GlyphArrangement::getStringWidth (bigFont, title.prefix) + 14.0f * k);
            drawText (title.prefix, ink, prefixArea);
        }
        const auto icon = title.icon.has_value() ? title.icon : (source ? std::optional<Icon> (Icon::Xlr) : (layer ? std::optional<Icon> (Icon::Layers) : std::nullopt));
        if (icon.has_value())
        {
            drawIcon (g, *icon, t.removeFromLeft (h).reduced (h * 0.08f), title.iconInk.isTransparent() ? ink : title.iconInk);
            t.removeFromLeft (12.0f * k);
        }
        drawText (title.text, ink, t);
    }

    // SHOW SOF FRAME: a frame and a SOF badge while Sends on Fader is on.
    if (ctx.sendsOnFader && ctx.settings.getBoolValue (uiprefs::showSofFrame, true))
        paintTabBadge (g, tab, "SOF", Colours::gold, true);
    // The talkback frame (firmware 1.11, "frame on selected channel and during
    // talk"): on the talkback channel's tab, and on any tab while talking.
    if (auto* talkStrip = ctx.engine.getTalkStrip();
        talkStrip != nullptr && (ctx.engine.isTalking() || (ctx.getSelectedStrip && ctx.getSelectedStrip() == talkStrip)))
        paintTabBadge (g, tab, "TALKBACK", Colours::mute, false);
}

void MainDisplay::paintClock (juce::Graphics& g)
{
    // "8:39" large, AM/PM small and yellow above the seconds ":50", the date
    // in grey below it (3.1); 24-hour mode without AM/PM.
    const float sh = (float) screenArea.getHeight();
    auto c = clockTile.toFloat().reduced (6.0f * sh / 800.0f, 4.0f * sh / 800.0f);
    if (touchDisabled)
        c.removeFromLeft (14.0f * sh / 800.0f); // room for the touch-off X
    const auto now = juce::Time::getCurrentTime();
    const auto [hm, ampm] = clockText (ctx.settings, now);
    const auto big = wingFontPx (0.034f * sh, WingWeight::SemiBold);
    const auto small = wingFontPx (0.015f * sh, WingWeight::SemiBold);
    const auto timeRow = c.removeFromTop (c.getHeight() * 0.64f);
    const float hmW = juce::GlyphArrangement::getStringWidth (big, hm);
    const float smallW = juce::GlyphArrangement::getStringWidth (small, ":00") + 2.0f;
    const float x0 = timeRow.getCentreX() - (hmW + smallW) * 0.5f;
    g.setColour (Colours::caption);
    g.setFont (big);
    g.drawText (hm, juce::Rectangle<float> (x0, timeRow.getY(), hmW + 2.0f, timeRow.getHeight()), juce::Justification::centredLeft, false);
    auto side = juce::Rectangle<float> (x0 + hmW + 1.0f, timeRow.getY(), smallW + 4.0f, timeRow.getHeight()).reduced (0.0f, timeRow.getHeight() * 0.1f);
    g.setFont (small);
    if (ampm.isNotEmpty())
    {
        g.setColour (Colours::warning);
        g.drawText (ampm, side.removeFromTop (side.getHeight() * 0.5f), juce::Justification::centredLeft, false);
    }
    else
    {
        side.removeFromTop (side.getHeight() * 0.5f);
    }
    g.setColour (Colours::caption);
    g.drawText (now.formatted (":%S"), side, juce::Justification::centredLeft, false);
    g.setColour (Colours::textDim);
    g.setFont (wingFontPx (0.017f * sh, WingWeight::SemiBold));
    g.drawText (uiprefs::formatDate (ctx.settings, now), c, juce::Justification::centredTop, false);
}

void MainDisplay::paintStatusBar (juce::Graphics& g)
{
    g.setColour (Colours::screen);
    g.fillRect (statusArea);
    const float sh = (float) screenArea.getHeight(), k = sh / 800.0f;
    const float corner = 4.0f * k;
    auto tile = [&g, corner] (juce::Rectangle<int> r, juce::Colour fill = Colours::screenHeader)
    {
        g.setColour (fill);
        g.fillRoundedRectangle (r.toFloat(), corner);
    };

    paintTitleTile (g);

    // The audio status [I1]: the SIP badge, the sample rate and clock source
    // (green chips), the AES50 ports A, B, C and StageConnect (SC), grey: none
    // here. It flashes while there are new FX faults; a click opens SETUP >
    // AUDIO (the DSP load is on UTILITY).
    tile (chipsTile, flash ? juce::Colour (0xff680008) : Colours::screenHeader);
    {
        auto* device = ctx.deviceManager.getCurrentAudioDevice();
        auto r = chipsTile.toFloat();
        // SIP: lit while SOLO MODE is SIP, filled while it mutes (a solo is on).
        const auto sip = juce::Rectangle<float> (r.getX() + 5.0f * k, r.getY() + 5.0f * k, 24.0f * k, 24.0f * k);
        const bool sipMode = ctx.engine.soloMode.load() == ld::kSoloSip, sipMuting = sipMode && ctx.engine.sipActive.load();
        if (sipMuting)
        {
            g.setColour (Colours::warning);
            g.fillEllipse (sip.reduced (1.0f));
        }
        g.setColour (sipMuting ? juce::Colours::black : (sipMode ? Colours::warning : Colours::chipOff));
        g.drawEllipse (sip.reduced (1.0f), juce::jmax (1.0f, 1.5f * k));
        g.setFont (wingFontPx (6.0f * k));
        g.drawText ("SIP", sip, juce::Justification::centred, false);

        const float x0 = r.getX() + 38.0f * k, chipH = 14.0f * k;
        auto chip = [&] (float x, float y, float w, const juce::String& text, juce::Colour fill, juce::Colour ink)
        {
            drawChip (g, { x0 + x * k, r.getY() + y * k, w * k, chipH }, text, fill, ink);
        };
        const auto offInk = juce::Colour (0xff181c18);
        const auto rate = device != nullptr ? juce::String (juce::roundToInt (device->getCurrentSampleRate() / 1000.0)) + "K" : juce::String ("--");
        chip (0.0f, 5.0f, 28.0f, rate, device != nullptr ? Colours::green : Colours::mute, juce::Colours::black);
        chip (33.0f, 5.0f, 23.0f, device != nullptr ? "INT" : "NO DEV", device != nullptr ? Colours::green : Colours::mute, juce::Colours::black);
        for (int i = 0; i < 3; ++i)
            chip (22.0f * (float) i, 25.0f, 16.0f, juce::String::charToString ((juce::juce_wchar) ('A' + i)), Colours::chipOff, offInk);
        chip (0.0f, 45.0f, 28.0f, "SC", Colours::chipOff, offInk);
    }

    // The USB stick (the USB recorder's folder): opens the USB recorder; red while it records.
    tile (usbTile);
    const auto usbState = ctx.engine.getUsbRecorder().getRecState();
    const auto usbInk = usbState == UsbRecorder::RecState::Recording ? Colours::mute
                        : usbState == UsbRecorder::RecState::Paused  ? (blink ? Colours::mute : Colours::green)
                                                                     : Colours::green;
    drawIcon (g, Icon::UsbStick, usbTile.toFloat().reduced ((float) usbTile.getHeight() * 0.24f), usbInk);

    // The recorder (the WING-LIVE cards "1 2", "• LIVE •"): red while recording,
    // green while playing, amber when its folder can't be written.
    tile (liveTile);
    {
        auto r = liveTile.toFloat().reduced (6.0f * k, 5.0f * k);
        const auto state = ctx.recording != nullptr ? ctx.recording->getState() : Recorder::State::Stopped;
        const auto liveInk = state == Recorder::State::Recording ? (blink ? Colours::mute : Colours::mute.darker (0.6f))
                           : state == Recorder::State::Playing   ? Colours::green
                           : ! liveWritable                       ? Colours::warning
                                                                  : Colours::textDim;
        auto caption = r.removeFromBottom (r.getHeight() * 0.3f);
        auto cards = r.withSizeKeepingCentre (juce::jmin (r.getWidth(), r.getHeight() * 1.5f), r.getHeight());
        for (int i = 0; i < 2; ++i)
        {
            auto card = cards.removeFromLeft (cards.getWidth() * (i == 0 ? 0.5f : 1.0f)).reduced (cards.getHeight() * 0.03f, 0.0f);
            drawIcon (g, Icon::SdCard, card, liveInk);
            g.setColour (liveInk);
            g.setFont (wingFontPx (card.getHeight() * 0.3f));
            g.drawText (juce::String (i + 1), card.translated (0.0f, card.getHeight() * 0.06f), juce::Justification::centred, false);
        }
        g.setColour (liveInk);
        g.setFont (wingFontPx (TypeScale::statusCaption * sh));
        g.drawText (juce::String::fromUTF8 ("\xe2\x80\xa2  LIVE  \xe2\x80\xa2"), caption.translated (0.0f, 2.0f * k), juce::Justification::centred, false);
    }

    // The screen's keys: an icon above a small grey caption.
    for (size_t i = 0; i < statusKeys.size() && i < statusKeyAreas.size(); ++i)
    {
        const auto& key = statusKeys[i];
        const auto area = statusKeyAreas[i];
        tile (area, key.lit ? Colours::textDim : Colours::screenHeader); // lit: #707070 [3.1]
        const auto keyInk = key.lit ? juce::Colours::black : key.ink.value_or (Colours::textDim);
        auto r = area.toFloat().reduced (3.0f * k);
        g.setColour (keyInk);
        g.setFont (wingFontPx (juce::jmin (TypeScale::statusCaption * sh, r.getHeight() * 0.2f)));
        if (key.icon)
        {
            auto label = r.removeFromBottom (r.getHeight() * 0.34f);
            drawIcon (g, *key.icon, r.withSizeKeepingCentre (r.getHeight() * 0.86f, r.getHeight() * 0.86f).translated (0.0f, 2.0f * k), keyInk);
            g.drawFittedText (key.text, label.toNearestInt(), juce::Justification::centred, 1, 0.7f);
        }
        else
        {
            g.drawFittedText (key.text, r.toNearestInt(), juce::Justification::centred, 2, 0.7f);
        }
    }

    tile (clockTile);
    paintClock (g);

    // Touch off: a small X.
    if (touchDisabled)
    {
        const auto x = juce::Rectangle<float> ((float) clockTile.getX() + 5.0f * k, (float) clockTile.getY() + 6.0f * k, 11.0f * k, 11.0f * k);
        g.setColour (Colours::mute);
        g.drawLine ({ x.getTopLeft(), x.getBottomRight() }, 2.0f);
        g.drawLine ({ x.getTopRight(), x.getBottomLeft() }, 2.0f);
    }
}

void MainDisplay::paint (juce::Graphics& g)
{
    if (maximized)
    {
        // Screen only: the display's frame centred on black.
        g.fillAll (juce::Colours::black);
        g.setColour (surfaceColours().displayUnit);
        g.fillRoundedRectangle (frameArea.toFloat(), 6.0f);
    }
    else
    {
        g.fillAll (surfaceColours().displayUnit);
    }

    // The screen, framed by a bezel.
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (screenArea.expanded (5).toFloat(), 4.0f);
    g.setColour (Colours::outline);
    g.drawRoundedRectangle (screenArea.expanded (6).toFloat(), 5.0f, 1.0f);
    g.setColour (Colours::screen);
    g.fillRect (screenArea);

    paintStatusBar (g);
}

void MainDisplay::editFx (int slot)
{
    showScreen (Screen::Effects);
    effects->editSlot (slot);
}

void MainDisplay::mouseUp (const juce::MouseEvent& e)
{
    if (e.eventComponent != this)
        return;
    const auto p = e.getPosition();
    for (size_t i = 0; i < statusKeys.size() && i < statusKeyAreas.size(); ++i)
        if (statusKeyAreas[i].contains (p))
        {
            if (auto f = statusKeys[i].onClick)
                f(); // a copy: the click may change the keys
            return;
        }
    if (chipsTile.contains (p))
    {
        // Touching the audio status opens SETUP > AUDIO (firmware 1.08).
        acknowledgedFaults = ctx.engine.getNumFaultedFx();
        showScreen (Screen::Setup);
        if (ctx.showSetupTab)
            ctx.showSetupTab (kSetupAudioTab);
    }
    else if (usbTile.contains (p))
        showScreen (Screen::UsbRecorder); // "clicking on the USB icon on the top of every screen" (manual p. 20)
    else if (liveTile.contains (p))
        showScreen (Screen::Recorder);
    else if (channelTab.contains (p) && page (screen)->titleClicked())
        return;
    else if (numberBox.contains (p))
        showChannelSelect (! channelSelect->isVisible());
    else if (channelTab.contains (p))
        showHomeTab (HomeTab::Home);
}

void MainDisplay::setMaximized (bool shouldBe)
{
    if (maximized == shouldBe)
        return;
    maximized = shouldBe;
    maximizeKey->icon = maximized ? Icon::Restore : Icon::Maximize;
    maximizeKey->setTooltip (maximized ? "Back to the whole surface (F10 or Esc)"
                                       : "Show only the screen, as large as the window allows (F10; Esc returns)");
    maximizeKey->repaint();
    resized();
    repaint();
}

void MainDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.eventComponent != this || ! statusArea.contains (e.getPosition()))
        return;
    const auto p = e.getPosition();
    for (auto& tile : { channelTab, chipsTile, usbTile, liveTile, clockTile })
        if (tile.contains (p))
            return;
    for (auto& key : statusKeyAreas)
        if (key.contains (p))
            return;
    if (ctx.setScreenMaximized && ctx.isScreenMaximized)
        ctx.setScreenMaximized (! ctx.isScreenMaximized());
}

namespace
{
    // The frame around the screen: the key column, the right-hand column (at
    // least `rightMin`, at most `rightMax` for a screen `screenH` tall) and the
    // encoder row under the screen.
    int displayNavWidth (int width) { return juce::jlimit (60, 92, width * 8 / 100); }
    int displayRightMin (int width) { return juce::jlimit (56, 84, width * 7 / 100); }
    int displayRightMax (int width, int screenH) { return juce::jlimit (displayRightMin (width), 2 * displayRightMin (width), screenH * 22 / 100); }
    int displayEncoderHeight (int height) { return juce::jlimit (28, 52, (height - 16) * 9 / 100); }
    int displayScreenHeight (int height) { return height - 16 - displayEncoderHeight (height) - 8; }
} // namespace

int MainDisplay::preferredWidth (int height)
{
    const int screenW = displayScreenHeight (height) * 16 / 10;
    // The side columns depend on the width; a few rounds settle it.
    int w = 92 + 20 + screenW + 168;
    for (int i = 0; i < 3; ++i)
        w = displayNavWidth (w) + 20 + screenW + displayRightMax (w, displayScreenHeight (height));
    return w;
}

void MainDisplay::resized()
{
    auto b = getLocalBounds();
    const int navW = displayNavWidth (b.getWidth());
    const int rightMin = displayRightMin (b.getWidth());
    // The six touch encoders sit under the screen's bezel.
    const int encoderH = displayEncoderHeight (b.getHeight());
    if (maximized)
    {
        // Screen only: the frame shrinks around the largest 16:10 screen that
        // fits, centred (the rest is black).
        int w = b.getWidth() - navW - rightMin - 20, h = b.getHeight() - 16 - encoderH - 8;
        if (w * 10 > h * 16)
            w = h * 16 / 10;
        else
            h = w * 10 / 16;
        b = b.withSizeKeepingCentre (navW + w + 20 + rightMin, h + 16 + encoderH + 8);
    }
    else
    {
        // The screen is 16:10; width it doesn't need goes to the right column,
        // up to a point: past that the unit stays together, centred.
        const int screenH = b.getHeight() - 16 - encoderH - 8;
        const int unitW = navW + 20 + screenH * 16 / 10 + displayRightMax (b.getWidth(), screenH);
        if (b.getWidth() > unitW)
            b = b.withSizeKeepingCentre (unitW, b.getHeight());
    }
    frameArea = b;
    auto nav = b.removeFromLeft (navW);
    auto avail = b.withTrimmedRight (rightMin).reduced (10, 8);
    auto screenAvail = avail.withTrimmedBottom (encoderH + 8);

    int w = screenAvail.getWidth(), h = screenAvail.getHeight();
    if (w * 10 > h * 16)
        w = h * 16 / 10;
    else
        h = w * 10 / 16;
    screenArea = juce::Rectangle<int> (screenAvail.getX(), screenAvail.getY() + (screenAvail.getHeight() - h) / 2, w, h);
    // The screen's type scale follows its height.
    setWingScreenHeight ((float) screenArea.getHeight());
    encoderArea = juce::Rectangle<int> (screenArea.getX(), screenArea.getBottom() + 10, screenArea.getWidth(), encoderH);
    auto right = frameArea.withLeft (screenArea.getRight() + 10).reduced (6, 0);

    // The seven keys over the full height of the screen: HOME level with its
    // top, UTILITY level with its bottom.
    const int keyH = juce::jlimit (24, 46, screenArea.getHeight() * 8 / 100);
    const float pitch = (float) (screenArea.getHeight() - keyH) / 6.0f;
    auto keyRow = [&] (int i) { return juce::roundToInt ((float) screenArea.getY() + pitch * (float) i); };
    const int keyW = juce::jmin (nav.getWidth() - 14, 80);
    for (int i = 0; i < screenButtons.size(); ++i)
        screenButtons[i]->setBounds (juce::Rectangle<int> (nav.getCentreX() - keyW / 2, keyRow (i), keyW, keyH));

    // Right: CLR SOLO level with HOME, the meter from EFFECTS to SETUP, the knob
    // level with LIBRARY, the blank key level with UTILITY.
    const int rightKeyW = juce::jmin (right.getWidth(), keyW);
    clearSoloKey->setBounds (juce::Rectangle<int> (right.getCentreX() - rightKeyW / 2, keyRow (0), rightKeyW, keyH));
    const int meterW = juce::jmin (right.getWidth(), 64);
    mainMeter->setBounds (juce::Rectangle<int> (right.getCentreX() - meterW / 2, keyRow (1), meterW, keyRow (4) + keyH - keyRow (1)));
    const int knobSize = juce::jmin (right.getWidth(), juce::roundToInt (pitch * 1.1f) + 14);
    contextKnob->setBounds (juce::Rectangle<int> (knobSize, knobSize).withCentre ({ right.getCentreX(), keyRow (5) + keyH / 2 + 4 }));
    blankKey->setBounds (juce::Rectangle<int> (right.getCentreX() - rightKeyW / 2, keyRow (6), rightKeyW, keyH));
    const int maxKeySize = juce::jmin (rightKeyW, encoderH, keyH);
    maximizeKey->setBounds (juce::Rectangle<int> (maxKeySize, maxKeySize).withCentre ({ right.getCentreX(), encoderArea.getCentreY() }));

    // WING geometry: the status bar 80 of 800 px, the parameter bar 50.
    auto s = screenArea;
    const int statusH = juce::roundToInt ((float) s.getHeight() * 0.1f);
    statusArea = s.removeFromTop (statusH);
    const bool knobs = page (screen)->usesKnobs();
    slotArea = knobs ? s.removeFromBottom (juce::roundToInt ((float) screenArea.getHeight() * 50.0f / 800.0f)) : juce::Rectangle<int>();
    if (! knobs)
        s.removeFromBottom (4);
    pageArea = s;

    for (auto sc : allScreens)
        page (sc)->setBounds (s);
    const auto belowStatus = screenArea.withTrimmedTop (statusH);
    channelSelect->setBounds (belowStatus);
    highlight->setBounds (getLocalBounds());
    popupLayer->setBounds (belowStatus);
    for (auto* child : popupLayer->getChildren())
        child->setBounds (popupLayer->getLocalBounds());
    touchBlocker->setBounds (screenArea);
    lcdScrim->setBounds (screenArea.expanded (5));

    layoutStatusBar();

    // The parameter bar: slot i at x 28 + 212 i, 160 px wide; the encoders
    // centred under the slots.
    for (int i = 0; i < slots.size(); ++i)
    {
        slots[i]->setVisible (knobs);
        slots[i]->setBounds (wingRect (28.0f + 212.0f * (float) i, 752.0f, 160.0f, 46.0f));
        const int cx = wingRect (28.0f + 212.0f * (float) i + 80.0f, 0.0f, 0.0f, 0.0f).getX();
        encoders[i]->setBounds (juce::Rectangle<int> (encoderH, encoderH).withCentre ({ cx, encoderArea.getCentreY() }));
    }
}

void MainDisplay::layoutStatusBar()
{
    // WING pixels (firmware 3.1): tiles at y 8..72; the title 8..472, audio
    // status 480..584, USB 592..656, WING-LIVE 664..728, the screen's keys from
    // 736 (72 wide, 8 apart), the clock 1136..1272.
    channelTab = wingRect (8.0f, 8.0f, 464.0f, 64.0f);
    numberBox = wingRect (8.0f, 8.0f, 160.0f, 64.0f);
    chipsTile = wingRect (480.0f, 8.0f, 104.0f, 64.0f);
    usbTile = wingRect (592.0f, 8.0f, 64.0f, 64.0f);
    liveTile = wingRect (664.0f, 8.0f, 64.0f, 64.0f);
    clockTile = wingRect (1136.0f, 8.0f, 136.0f, 64.0f);
    statusKeyAreas.clear();
    const int n = (int) statusKeys.size();
    const float keyW = n <= 5 ? 72.0f : (392.0f - 8.0f * (float) (n - 1)) / (float) n;
    int left = 0, right = 0;
    for (auto& key : statusKeys)
        right += key.atRight ? 1 : 0;
    for (auto& key : statusKeys)
    {
        // The right-hand keys end at the clock (1128), the others start at 736.
        const float x = key.atRight ? 1128.0f - (keyW + 8.0f) * (float) (right--) + 8.0f : 736.0f + (keyW + 8.0f) * (float) (left++);
        statusKeyAreas.push_back (wingRect (x, 8.0f, keyW, 64.0f));
    }
}

} // namespace livedaw::ui
