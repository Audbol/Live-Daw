#include "ui/MainDisplay.h"
#include "dsp/DspUtils.h"
#include "ui/HomeWidgets.h"
#include "ui/Icons.h"

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
                                      Screen::Faders,  Screen::Sends,    Screen::Config,  Screen::MuteGroups };

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
        }
        return "";
    }

    /** A rounded hardware key of the console's top panel; lit while toggled. */
    class HardwareKey final : public juce::TextButton
    {
    public:
        using juce::TextButton::TextButton;

        juce::Colour litColour = juce::Colour (0xfff2f2f2);
        bool led = false; // the blank key: an LED instead of a lit key

        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            auto r = getLocalBounds().toFloat().reduced (1.0f);
            const bool lit = getToggleState();
            auto fill = lit && ! led ? litColour : juce::Colour (0xff2c2e32);
            if (down)
                fill = fill.brighter (0.15f);
            else if (over)
                fill = fill.brighter (0.06f);
            g.setColour (fill);
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (juce::Colour (0xff4a4d53));
            g.drawRoundedRectangle (r, 5.0f, 1.2f);
            if (led)
            {
                g.setColour (lit ? Colours::yellow : juce::Colour (0xff3a3a3a));
                g.fillRoundedRectangle (r.reduced (r.getWidth() * 0.32f, r.getHeight() * 0.42f), 2.0f);
                return;
            }
            g.setColour (lit ? juce::Colours::black : Colours::text);
            const auto keyText = getButtonText();
            const bool twoLines = keyText.containsChar ('\n');
            g.setFont (juce::FontOptions (juce::jlimit (10.0f, 15.0f, r.getHeight() * (twoLines ? 0.3f : 0.36f)), juce::Font::bold));
            g.drawFittedText (keyText, r.toNearestInt().reduced (2, 0), juce::Justification::centred, twoLines ? 2 : 1, 0.7f);
        }
    };

    /** The style of a parameter-bar slot, completed from the binding where the
        page left it open. */
    KnobStyle resolveStyle (const KnobBinding& b, KnobStyle s)
    {
        const auto label = b.label.toUpperCase();
        const auto text = b.text ? b.text() : juce::String();
        const bool thumbLabel = label == "PAN" || label.startsWith ("PAN ") || label == "TRIM" || label == "BAL" || label == "BALANCE";
        if (s.bar == KnobStyle::Bar::Auto)
        {
            if (label.contains ("WIDTH"))
                s.bar = KnobStyle::Bar::Centre;
            else if (thumbLabel)
                s.bar = KnobStyle::Bar::Thumb;
            else if (b.steps > 1.0f)
                s.bar = b.steps <= 24.0f ? KnobStyle::Bar::Segments : KnobStyle::Bar::Thumb;
            else if (b.bipolar)
                s.bar = KnobStyle::Bar::Centre;
            else
                s.bar = KnobStyle::Bar::Fill;
        }
        if (s.colour.isTransparent())
        {
            if (label.contains ("WIDTH"))
                s.colour = KnobColours::green;
            else if (label.contains ("TILT"))
                s.colour = KnobColours::cyan;
            else if (label == "BAL" || label == "BALANCE")
                s.colour = KnobColours::blue;
            else if (thumbLabel)
                s.colour = KnobColours::white;
            else if (label == "DIST" || label == "TIME" || label == "SAMPLES" || b.steps > 1.0f)
                s.colour = KnobColours::yellow;
            else if (text.endsWith ("Hz") || label.contains ("FREQ") || (text.endsWith ("dB") && ! b.bipolar) || text.contains ("oo"))
                s.colour = KnobColours::blue;
            else
                s.colour = KnobColours::white;
        }
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
        auto b = getLocalBounds().reduced (6, 2);
        auto bar = b.removeFromBottom (juce::jlimit (6, 9, getHeight() / 5)).toFloat();
        const bool touched = isMouseOverOrDragging();
        g.setFont (juce::FontOptions (juce::jlimit (12.0f, 20.0f, (float) b.getHeight() * 0.72f), juce::Font::bold));
        g.setColour (touched ? Colours::yellow : Colours::text);
        g.drawFittedText (binding.label.toUpperCase(), b, juce::Justification::centredLeft, 1, 0.75f);
        g.setColour (style.colour);
        g.drawFittedText (valueText, b, juce::Justification::centredRight, 1, 0.75f);

        const float y = bar.getCentreY();
        const float x = bar.getX() + position * bar.getWidth();
        switch (style.bar)
        {
            case KnobStyle::Bar::Segments:
            {
                const int n = juce::jlimit (2, 32, style.segments > 0 ? style.segments : (int) binding.steps);
                const int current = juce::roundToInt (position * (float) (n - 1));
                const float w = bar.getWidth() / (float) n;
                for (int i = 0; i < n; ++i)
                {
                    g.setColour (i == current ? style.colour : juce::Colour (0xff4a4a4a));
                    g.fillRect (bar.getX() + (float) i * w + 0.5f, y - 2.5f, juce::jmax (1.0f, w - 1.5f), 5.0f);
                }
                return;
            }
            case KnobStyle::Bar::Fill:
            case KnobStyle::Bar::Centre:
            {
                g.setColour (juce::Colour (0xff4a4a4a));
                g.fillRect (bar.getX(), y - 1.0f, bar.getWidth(), 2.0f);
                const float from = style.bar == KnobStyle::Bar::Centre ? bar.getCentreX() : bar.getX();
                g.setColour (style.colour);
                g.fillRect (juce::jmin (from, x), y - 2.0f, std::abs (x - from), 4.0f);
                if (style.bar == KnobStyle::Bar::Centre)
                {
                    g.setColour (Colours::text);
                    g.fillRect (bar.getCentreX() - 1.0f, bar.getY(), 2.0f, bar.getHeight());
                }
                g.setColour (Colours::text);
                g.fillRect (x - 1.0f, y - 3.0f, 2.0f, 6.0f);
                return;
            }
            case KnobStyle::Bar::Thumb:
            case KnobStyle::Bar::Auto:
                g.setColour (juce::Colour (0xff4a4a4a));
                g.fillRect (bar.getX(), y - 1.0f, bar.getWidth(), 2.0f);
                g.setColour (Colours::text);
                g.fillRoundedRectangle (x - 3.5f, bar.getY(), 7.0f, bar.getHeight(), 1.5f);
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
        g.setColour (target.isValid() ? Colours::textDim : Colours::textDim.withAlpha (0.4f));
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
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
        auto b = getTarget ? getTarget() : KnobBinding {};
        if (! b.isValid() || ! b.set)
            return;
        accumulated += clicks;
        const float whole = std::trunc (accumulated);
        if (whole == 0.0f)
            return;
        accumulated -= whole;
        const float step = b.steps > 1.0f ? 1.0f / (b.steps - 1.0f) : 0.002f;
        b.set (juce::jlimit (0.0f, 1.0f, b.get() + whole * step));
        angle += whole * 0.08f;
        repaint();
        if (changed)
            changed();
    }

    int lastY = 0;
    float accumulated = 0.0f, angle = 0.0f;
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
        setTooltip();
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
        g.setFont (juce::FontOptions (juce::jlimit (8.0f, 11.0f, r.getHeight() / 22.0f), juce::Font::bold));
        g.setColour (Colours::textDim);
        g.drawText ("L", juce::Rectangle<float> (x0, header.getY(), barW, header.getHeight()).toNearestInt(), juce::Justification::centred);
        g.drawText ("R", juce::Rectangle<float> (x0 + barW + 4.0f, header.getY(), barW, header.getHeight()).toNearestInt(), juce::Justification::centred);
        const float segH = r.getHeight() / 18.0f;
        for (int s = 0; s < 18; ++s)
        {
            const float y = r.getY() + (float) s * segH;
            g.setColour (Colours::textDim);
            g.drawText (s == 0 ? juce::String ("CLIP") : juce::String ((int) thresholds[s]), labels.withY (y).withHeight (segH).toNearestInt(), juce::Justification::centredRight);
            for (int ch = 0; ch < 2; ++ch)
            {
                const bool lit = level[(size_t) ch] >= thresholds[s];
                const auto on = s == 0 ? Colours::mute : (s < 5 ? Colours::meterYellow : Colours::meterGreen);
                g.setColour (lit ? on : juce::Colour (0xff1d1f22));
                g.fillRect (x0 + (float) ch * (barW + 4.0f), y + 1.0f, barW, segH - 2.0f);
            }
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() && e.getNumberOfClicks() < 2)
            return;
        juce::PopupMenu m;
        m.addSectionHeader ("MAIN METER");
        m.addItem ("MONITOR", true, source == 0, [this] { choose (0); });
        for (int i = 1; i <= kNumMains; ++i)
            m.addItem ("MAIN " + juce::String (i), true, source == i, [this, i] { choose (i); });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
    }

private:
    void setTooltip() {}

    void choose (int s)
    {
        source = s;
        ctx.settings.setValue ("ui.mainMeter", s);
    }

    void timerCallback() override
    {
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
/** Draws a yellow outline round the control the touched knob changes, and a
    thinner one round the control the fine-adjust knob is on. */
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
            g.setColour (Colours::yellow.withAlpha (alpha));
            g.drawRect (area, thickness);
        };
        outline (focus.getComponent(), 1.0f, 0.6f);
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
/** CHANNEL SELECT (p. 30): every input channel, aux, bus, main and matrix;
    choose one to open its Channel Home. */
class MainDisplay::ChannelSelect final : public home::OverlayScreen
{
public:
    explicit ChannelSelect (UiContext& c) : home::OverlayScreen ("CHANNEL SELECT"), ctx (c) {}

    std::function<void()> chosen;

    void paint (juce::Graphics& g) override
    {
        OverlayScreen::paint (g);
        tiles.clear();
        auto* selected = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr;
        auto group = [&] (juce::Rectangle<int> area, const char* caption, StripKind kind, int columns, int rows)
        {
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.drawText (caption, area.removeFromTop (16), juce::Justification::centredLeft);
            const auto& list = ctx.engine.getStrips (kind);
            const float w = (float) area.getWidth() / (float) columns, h = (float) area.getHeight() / (float) juce::jmax (1, rows);
            for (int i = 0; i < (int) list.size() && i < columns * rows; ++i)
            {
                auto r = juce::Rectangle<float> ((float) area.getX() + (float) (i % columns) * w, (float) area.getY() + (float) (i / columns) * h, w, h).reduced (2.0f);
                home::paintStripTile (g, r, *list[(size_t) i], list[(size_t) i].get() == selected);
                tiles.push_back ({ r.toNearestInt(), list[(size_t) i].get() });
            }
        };
        const int chRows = juce::jmax (1, ((int) ctx.engine.getChannels().size() + 7) / 8);
        const int busRows = juce::jmax (1, ((int) ctx.engine.getBuses().size() + 7) / 8);
        auto left = gridLeft, right = gridRight;
        group (left, "CHANNEL", StripKind::Channel, 8, chRows);
        // AUX, BUS, then MAIN and MATRIX: rows as tall as the channels' where they fit.
        const int unit = juce::jmin ((right.getHeight() - 3 * 16) / (2 + busRows), (left.getHeight() - 16) / chRows);
        group (right.removeFromTop (unit * 1 + 16), "AUX", StripKind::Aux, 8, 1);
        group (right.removeFromTop (unit * busRows + 16), "BUS", StripKind::Bus, 8, busRows);
        auto last = right.removeFromTop (unit + 16);
        group (last.removeFromLeft (last.getWidth() * 4 / 12), "MAIN", StripKind::Main, 4, 1);
        last.removeFromLeft (8);
        group (last, "MATRIX", StripKind::Matrix, 8, 1);
    }

    void layoutContent (juce::Rectangle<int> b) override
    {
        b = b.reduced (6, 0);
        gridLeft = b.removeFromLeft (b.getWidth() / 2 - 6);
        b.removeFromLeft (12);
        gridRight = b;
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

private:
    UiContext& ctx;
    juce::Rectangle<int> gridLeft, gridRight;
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

    for (auto s : allScreens)
    {
        auto* p = page (s);
        p->knobsChanged = [this, s] { if (screen == s) updateKnobs(); };
        p->statusKeysChanged = [this, s] { if (screen == s) updateFrame(); };
        addChildComponent (p);
    }

    for (auto s : screens)
    {
        auto* b = screenButtons.add (new HardwareKey (screenName (s)));
        b->onClick = [this, s] { showScreen (s); };
        addAndMakeVisible (b);
    }

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
    }

    // Right of the display: CLR SOLO, the main meter, the knob, the blank key.
    clearSoloKey = std::make_unique<HardwareKey> ("CLR\nSOLO");
    static_cast<HardwareKey*> (clearSoloKey.get())->litColour = Colours::yellow;
    clearSoloKey->onClick = [this] { clearSolo(); };
    clearSoloKey->setTooltip ("Release every channel and bus on the solo bus");
    addAndMakeVisible (*clearSoloKey);
    mainMeter = std::make_unique<MainMeter> (ctx);
    addAndMakeVisible (*mainMeter);
    contextKnob = std::make_unique<ContextKnob>();
    addAndMakeVisible (*contextKnob);
    blankKey = std::make_unique<HardwareKey> ("");
    static_cast<HardwareKey*> (blankKey.get())->led = true;
    blankKey->onClick = [this] { page (screen)->blankKeyPressed(); };
    addAndMakeVisible (*blankKey);

    channelSelect = std::make_unique<ChannelSelect> (ctx);
    channelSelect->onClose = [this] { showChannelSelect (false); };
    channelSelect->chosen = [this] { showChannelSelect (false); showHomeTab (HomeTab::Home); };
    addChildComponent (*channelSelect);

    highlight = std::make_unique<Highlight>();
    highlight->suppressed = [this] { return channelSelect != nullptr && channelSelect->isVisible(); };
    addAndMakeVisible (*highlight);
    focusTracker = std::make_unique<FocusTracker> (*this);

    showScreen (Screen::Home);
    startTimerHz (4);
}

MainDisplay::~MainDisplay()
{
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
    }
    return home.get();
}

void MainDisplay::showScreen (Screen s)
{
    screen = s;
    showChannelSelect (false);
    for (auto sc : allScreens)
        page (sc)->setVisible (sc == s);
    for (size_t i = 0; i < std::size (screens); ++i)
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
        slots[(int) i]->setBinding (i < bindings.size() ? bindings[i] : KnobBinding {}, i < styles.size() ? styles[i] : KnobStyle {});
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

void MainDisplay::clearSolo()
{
    for (int k = 0; k < kNumStripKinds; ++k)
        for (auto& s : ctx.engine.getStrips (static_cast<StripKind> (k)))
            s->params.solo = false;
    for (int i = 0; i < kNumDcas; ++i)
        ctx.engine.getDca (i).solo = false;
}

void MainDisplay::showChannelSelect (bool show)
{
    if (channelSelect == nullptr || channelSelect->isVisible() == show)
        return;
    channelSelect->setVisible (show);
    if (show)
    {
        channelSelect->toFront (false);
        highlight->toFront (false);
    }
    repaint (statusArea);
}

void MainDisplay::timerCallback()
{
    const auto time = juce::Time::getCurrentTime().formatted ("%H:%M:%S");
    // Repaint the status bar when anything it shows changes.
    // Unsigned, so the mixing may wrap around.
    juce::uint32 hash = 0;
    if (auto* s = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr)
        hash = (juce::uint32) juce::DefaultHashFunctions::generateHash (describeStrip (*s) + juce::String (s->getColourIndex()), 1 << 30);
    hash = hash * 31u + (juce::uint32) juce::roundToInt (ctx.engine.getDspLoad() * 100.0f);
    hash = hash * 31u + (juce::uint32) ctx.engine.getNumFaultedFx();
    hash = hash * 31u + (ctx.deviceManager.getCurrentAudioDevice() != nullptr ? 1u : 0u);
    if (time != shownTime || hash != shownHash)
    {
        shownTime = time;
        shownHash = hash;
        repaint (statusArea);
    }

    // CLR SOLO blinks while anything is soloed; the blank key's LED shows
    // whether the screen gives it a function.
    blink = ! blink;
    clearSoloKey->setToggleState (ctx.engine.anySoloActive.load() && blink, juce::dontSendNotification);
    blankKey->setToggleState (page (screen)->hasBlankKeyFunction(), juce::dontSendNotification);
}

void MainDisplay::paintStatusBar (juce::Graphics& g)
{
    g.setColour (Colours::screen);
    g.fillRect (statusArea);

    // The selected channel's tab, in the strip's colour. The number sits in a
    // black box drawn in the strip's colour; unnamed buses, mains and matrices
    // show only their short name ("B7").
    auto* strip = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr;
    auto tab = channelTab.toFloat();
    const auto colour = strip != nullptr ? strip->getColour() : Colours::screenHeader;
    const auto ink = colour.getPerceivedBrightness() > 0.42f ? juce::Colours::black : juce::Colours::white;
    g.setColour (colour);
    g.fillRoundedRectangle (tab, 3.0f);
    if (strip != nullptr)
    {
        auto t = tab.reduced (5.0f, 5.0f);
        const float h = t.getHeight();
        const auto label = home::shortLabel (*strip);
        const bool plain = strip->isOutput() && home::hasDefaultName (*strip);
        g.setFont (juce::FontOptions (h * 0.92f, juce::Font::bold));
        if (plain)
        {
            g.setColour (ink);
            g.drawFittedText (label, t.reduced (4.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, 1);
        }
        else
        {
            auto box = t.removeFromLeft (juce::jmax (h * 1.1f, h * 0.48f * (float) label.length() + h * 0.3f));
            g.setColour (juce::Colours::black);
            g.fillRect (box);
            g.setColour (colour);
            g.setFont (juce::FontOptions (h * (label.length() >= 3 ? 0.74f : 0.86f), juce::Font::bold));
            g.drawFittedText (label, box.toNearestInt(), juce::Justification::centred, 1, 0.7f);
            t.removeFromLeft (h * 0.25f);
            drawIcon (g, stripIcon (strip->getKind()), t.removeFromLeft (h).reduced (h * 0.08f), ink);
            t.removeFromLeft (h * 0.2f);
            g.setColour (ink);
            g.setFont (juce::FontOptions (h * 0.92f, juce::Font::bold));
            g.drawFittedText (stripName (*strip).toUpperCase(), t.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
        }
    }

    // The status cluster: a round sync icon, two green chips (sample rate and
    // clock source), and small chips for DSP load and protection.
    auto tile = [&g] (juce::Rectangle<int> r)
    {
        g.setColour (juce::Colour (0xff2a2a2a));
        g.fillRoundedRectangle (r.toFloat(), 3.0f);
    };
    tile (chipsTile);
    auto chips = chipsTile.reduced (5, 6).toFloat();
    auto dot = chips.removeFromLeft (chips.getHeight() * 0.55f).withSizeKeepingCentre (chips.getHeight() * 0.5f, chips.getHeight() * 0.5f);
    auto* device = ctx.deviceManager.getCurrentAudioDevice();
    g.setColour (device != nullptr ? Colours::textDim : Colours::mute);
    g.drawEllipse (dot.reduced (1.0f), 1.5f);
    chips.removeFromLeft (4.0f);
    const float chipH = (chips.getHeight() - 3.0f) / 2.0f;
    auto row1 = chips.removeFromTop (chipH);
    chips.removeFromTop (3.0f);
    auto row2 = chips;
    const auto rate = device != nullptr ? juce::String (juce::roundToInt (device->getCurrentSampleRate() / 1000.0)) + "K" : juce::String ("--");
    const float cw = row1.getWidth() / 2.0f - 1.5f;
    drawChip (g, row1.removeFromLeft (cw), rate, device != nullptr ? Colours::green : Colours::mute, juce::Colours::black);
    row1.removeFromLeft (3.0f);
    drawChip (g, row1, device != nullptr ? "INT" : "NO DEV", device != nullptr ? Colours::green : Colours::mute, juce::Colours::black);
    const int faults = ctx.engine.getNumFaultedFx();
    const float sw = row2.getWidth() / 3.0f - 2.0f;
    drawChip (g, row2.removeFromLeft (sw * 1.4f), juce::String (juce::roundToInt (ctx.engine.getDspLoad() * 100.0f)) + "%", juce::Colour (0xff6a6a6a), juce::Colours::black);
    row2.removeFromLeft (3.0f);
    drawChip (g, row2.removeFromLeft (sw * 0.8f), "P", ctx.engine.outputProtection.load() ? Colours::green : juce::Colour (0xff4a4a4a), juce::Colours::black);
    row2.removeFromLeft (3.0f);
    drawChip (g, row2, "FX", faults > 0 ? Colours::mute : juce::Colour (0xff4a4a4a), juce::Colours::black);

    // The show files (the console's USB stick icon): opens LIBRARY.
    tile (usbTile);
    drawIcon (g, Icon::UsbStick, usbTile.toFloat().reduced (usbTile.getHeight() * 0.22f), Colours::green);

    // The screen's keys: an icon above a small label.
    for (size_t i = 0; i < statusKeys.size() && i < statusKeyAreas.size(); ++i)
    {
        const auto& key = statusKeys[i];
        const auto area = statusKeyAreas[i];
        g.setColour (key.lit ? juce::Colour (0xffd0d0d0) : juce::Colour (0xff2a2a2a));
        g.fillRoundedRectangle (area.toFloat(), 3.0f);
        const auto keyInk = key.lit ? juce::Colours::black : Colours::textDim;
        auto k = area.toFloat().reduced (3.0f);
        g.setColour (keyInk);
        g.setFont (juce::FontOptions (juce::jlimit (9.0f, 12.0f, k.getHeight() * 0.26f), juce::Font::bold));
        if (key.icon)
        {
            auto label = k.removeFromBottom (juce::jlimit (10.0f, 14.0f, k.getHeight() * 0.3f));
            drawIcon (g, *key.icon, k.reduced (k.getHeight() * 0.08f), keyInk);
            g.drawFittedText (key.text, label.toNearestInt(), juce::Justification::centred, 1, 0.7f);
        }
        else
        {
            g.drawFittedText (key.text, k.toNearestInt(), juce::Justification::centred, 2, 0.7f);
        }
    }

    // Clock.
    tile (clockTile);
    auto c = clockTile.reduced (6, 4);
    const auto now = juce::Time::getCurrentTime();
    g.setColour (Colours::text);
    g.setFont (juce::FontOptions ((float) c.getHeight() * 0.56f, juce::Font::bold));
    g.drawFittedText (now.formatted ("%H:%M:%S"), c.removeFromTop (c.getHeight() * 6 / 10), juce::Justification::centred, 1);
    g.setColour (Colours::textDim);
    g.setFont (juce::FontOptions ((float) c.getHeight() * 0.9f, juce::Font::bold));
    g.drawFittedText (now.formatted ("%Y-%m-%d"), c, juce::Justification::centred, 1);
}

void MainDisplay::paint (juce::Graphics& g)
{
    g.fillAll (Colours::panel);

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
    const auto p = e.getPosition();
    for (size_t i = 0; i < statusKeys.size() && i < statusKeyAreas.size(); ++i)
        if (statusKeyAreas[i].contains (p))
        {
            if (auto f = statusKeys[i].onClick)
                f(); // a copy: the click may change the keys
            return;
        }
    if (usbTile.contains (p))
        showScreen (Screen::Library);
    else if (numberBox.contains (p))
        showChannelSelect (! channelSelect->isVisible());
    else if (channelTab.contains (p))
        showHomeTab (HomeTab::Home);
}

void MainDisplay::resized()
{
    auto b = getLocalBounds();
    const int navW = juce::jlimit (60, 92, b.getWidth() * 8 / 100);
    const int rightMin = juce::jlimit (56, 84, b.getWidth() * 7 / 100);
    auto nav = b.removeFromLeft (navW);
    auto avail = b.withTrimmedRight (rightMin).reduced (10, 10);

    // The screen is 16:10; width it doesn't need goes to the right column.
    int w = avail.getWidth(), h = avail.getHeight();
    if (w * 10 > h * 16)
        w = h * 16 / 10;
    else
        h = w * 10 / 16;
    screenArea = juce::Rectangle<int> (avail.getX(), avail.getCentreY() - h / 2, w, h);
    auto right = getLocalBounds().withLeft (screenArea.getRight() + 10).reduced (6, 0);

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

    auto s = screenArea;
    const int statusH = juce::jlimit (34, 60, s.getHeight() * 83 / 1000);
    statusArea = s.removeFromTop (statusH);
    const bool knobs = page (screen)->usesKnobs();
    slotArea = knobs ? s.removeFromBottom (juce::jlimit (28, 44, s.getHeight() / 13)) : juce::Rectangle<int>();
    s.removeFromTop (4);
    if (! knobs)
        s.removeFromBottom (4);
    pageArea = s;

    for (auto sc : allScreens)
        page (sc)->setBounds (s);
    channelSelect->setBounds (screenArea.withTrimmedTop (statusH + 4));
    highlight->setBounds (getLocalBounds());

    layoutStatusBar();

    for (auto* slot : slots)
        slot->setVisible (knobs);
    auto k = slotArea.reduced (4, 0);
    const int sw = k.getWidth() / 6;
    for (auto* slot : slots)
        slot->setBounds (k.removeFromLeft (sw).reduced (6, 0));
}

void MainDisplay::layoutStatusBar()
{
    auto st = statusArea.reduced (3, 4);
    const int h = st.getHeight();
    clockTile = st.removeFromRight (juce::jmin (h * 3, st.getWidth() / 7));
    st.removeFromRight (4);
    statusKeyAreas.clear();
    const int keyW = juce::jmin (h * 6 / 5, st.getWidth() / 10);
    for (size_t i = 0; i < statusKeys.size(); ++i)
    {
        statusKeyAreas.insert (statusKeyAreas.begin(), st.removeFromRight (keyW));
        st.removeFromRight (4);
    }
    // Fig. 21: the channel tab about 37 % of the bar, then the status cluster
    // and the USB icon.
    channelTab = st.removeFromLeft (statusArea.getWidth() * 38 / 100);
    numberBox = channelTab.withWidth (juce::jmin (channelTab.getWidth() / 3, h * 2));
    st.removeFromLeft (4);
    chipsTile = st.removeFromLeft (juce::jmin (h * 5 / 2, st.getWidth() / 2));
    st.removeFromLeft (4);
    usbTile = st.removeFromLeft (juce::jmin (h, st.getWidth()));
}

} // namespace livedaw::ui
