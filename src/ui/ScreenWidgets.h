#pragma once

#include "ui/Icons.h"
#include "ui/LookAndFeel.h"
#include "ui/UiContext.h"
#include "ui/WingWidgets.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace livedaw::ui
{

/** The WING's screen palette (firmware 3.1) under the names the SETUP, LIBRARY
    and VIEW screens use: #202020 panels, #303030 keys, #101010 wells, blue
    choice keys, red PRE/PFL and green POST/AFL, the gold ON chip and tabs. The
    same values as the screen tokens in Colours, so the app has one look. */
namespace Dark
{
    const juce::Colour tabBar = Colours::screen;
    const juce::Colour tabOff = Colours::screenHeader;
    const juce::Colour tabOn = Colours::screenTile;
    const juce::Colour header = Colours::screenHeader;
    const juce::Colour body = Colours::screenPanel;
    const juce::Colour box = Colours::screenPanel;     // sub-boxes inside a panel (black gaps between them)
    const juce::Colour key = Colours::screenKey;       // unlit keys
    const juce::Colour keyOutline = Colours::textDim;  // outlined keys (SAFE, FILTER LO/HI)
    const juce::Colour well = Colours::screenWell;     // dropdown fields, list rows
    const juce::Colour text = Colours::text;
    const juce::Colour textDim = Colours::textDim;
    const juce::Colour caption = Colours::caption;     // unlit choice-key text, field captions

    const juce::Colour choice = Colours::choice;       // lit choice key (NORM|FINE|AUTO, MCU|HUI, ...)
    const juce::Colour choiceInk { 0xff0d1a33 };
    const juce::Colour offLit = Colours::chipOff;      // a lit OFF key (MAIN LINK, SOURCE SOLO, MIDI functions)
    const juce::Colour pre = Colours::tapRed;          // PRE, PFL
    const juce::Colour post = Colours::green;          // POST, AFL
    const juce::Colour usbGreen { 0xff00bc00 };        // USB HOST SPEED HS, DAW CONNECTION USB
    const juce::Colour olive { 0xff809800 };           // LIVE|STUDIO|SIP
    const juce::Colour orange = Colours::alt;          // ALT, USB HOST SPEED FS
    const juce::Colour on = Colours::gold;             // ON chip
    const juce::Colour gold = Colours::gold;           // tabs, selection
    const juce::Colour selectedRow { 0xff3b3215 };
    const juce::Colour selectedOutline = Colours::gold;
    const juce::Colour layer = Colours::choice;        // VIEW: the active layer
    const juce::Colour section = olive;                 // VIEW: the viewed fader section
    const juce::Colour muteGroup { 0xfff05050 };       // MUTE GROUPS headers and engaged keys
    const juce::Colour faderFill = Colours::valueBlue;
    const juce::Colour muted { 0xffe0595b };
} // namespace Dark

/** LIBRARY file-type colours (tabs and list chips). */
namespace FileColours
{
    const juce::Colour show { 0xff2486bd };
    const juce::Colour snap { 0xfff0a028 };
    const juce::Colour snip { 0xffc5a16a };
    const juce::Colour clip { 0xffa6c87a };
    const juce::Colour fx { 0xffc47ac4 };
    const juce::Colour chan { 0xffd2d376 };
    const juce::Colour wav { 0xff5ea557 };
    const juce::Colour libraryTitle { 0xff904400 }; // the status bar's title tile on LIBRARY
} // namespace FileColours

/** Console preferences kept in the app settings (SETUP). */
namespace prefs
{
    constexpr const char* consoleName = "setup.consoleName";
    constexpr const char* confirmLibraryLoad = "setup.confirmLibraryLoad";
    constexpr const char* touchFaderRes = "setup.touchFaderRes";          // 0 NORM, 1 FINE, 2 AUTO
    constexpr const char* clock12h = "setup.clock12h";
    constexpr const char* dateDayFirst = "setup.dateDayFirst";
    constexpr const char* layerLink = "setup.faderScreenLayerLink";       // VIEW screens follow the section's layer (default off)
    constexpr bool layerLinkDefault = false;
    constexpr const char* brightnessPrefix = "setup.brightness.";

    /** SETUP > SURFACE > METERING: PRE or POST for the meters of each strip type. */
    constexpr int kNumMeterTaps = 5;
    constexpr const char* meterTapNames[kNumMeterTaps] = { "CHANNEL METERS", "BUS METERS", "MAIN METERS", "MATRIX METERS", "DCA METERS" };
    constexpr bool meterTapDefaultPre[kNumMeterTaps] = { true, false, false, false, true }; // the console's defaults
    inline juce::String meterTapKey (int i) { return "setup.meterTapPre." + juce::String (i); }
    inline bool meterTapPre (UiContext& ctx, int i)
    {
        i = juce::jlimit (0, kNumMeterTaps - 1, i);
        return ctx.settings.getBoolValue (meterTapKey (i), meterTapDefaultPre[i]);
    }
    /** Where a strip type's meters measure (aux channels follow CHANNEL METERS; DCA METERS is index 4). */
    inline MeterPoint meterPoint (UiContext& ctx, StripKind kind)
    {
        const int i = kind == StripKind::Bus ? 1 : kind == StripKind::Main ? 2 : kind == StripKind::Matrix ? 3 : 0;
        return meterTapPre (ctx, i) ? MeterPoint::PreFader : MeterPoint::Output;
    }
    /** METERS > SETTINGS: the METERS screen's own PRE or POST per strip type
        (the WING's /cfg/mtr/mtrpage; the surface's are /cfg/mtr/mtrsfc), with
        the same factory settings. */
    inline juce::String meterPageTapKey (int i) { return "setup.meterPageTapPre." + juce::String (i); }
    inline bool meterPageTapPre (UiContext& ctx, int i)
    {
        i = juce::jlimit (0, kNumMeterTaps - 1, i);
        return ctx.settings.getBoolValue (meterPageTapKey (i), meterTapDefaultPre[i]);
    }
    inline MeterPoint meterPagePoint (UiContext& ctx, StripKind kind)
    {
        const int i = kind == StripKind::Bus ? 1 : kind == StripKind::Main ? 2 : kind == StripKind::Matrix ? 3 : 0;
        return meterPageTapPre (ctx, i) ? MeterPoint::PreFader : MeterPoint::Output;
    }

    inline bool confirmLoad (UiContext& ctx) { return ctx.settings.getBoolValue (confirmLibraryLoad, true); }
    inline int faderResolution (UiContext& ctx) { return ctx.settings.getIntValue (touchFaderRes, 2); }
    inline bool screenFollowsLayer (UiContext& ctx) { return ctx.settings.getBoolValue (layerLink, layerLinkDefault); }
} // namespace prefs

/** Height of a dark panel's header bar (SETUP, LIBRARY, MONITORS, the recorder):
    48 of the WING's 800 px screen, so it follows the screen size like the HOME
    panel headers (30 px at 1920x1080, 20 px at 1280x800). */
inline int darkHeaderHeight() { return juce::jlimit (20, screenCap (36), juce::roundToInt (getWingScreenHeight() * 0.06f)); }
/** The black gap between a panel's header and its body. */
constexpr int kDarkHeaderGap = 4;

/** The font of panel header titles, fitted to a header `headerHeight` px tall. */
inline juce::Font panelTitleFont (int headerHeight)
{
    return wingFontPx (juce::jmin (TypeScale::header * getWingScreenHeight(), (float) headerHeight * 0.42f));
}

/** Draws a dark WING panel: a header bar with the title centred, a thin black
    gap, then the body. Returns the body. An empty title draws only the body. */
inline juce::Rectangle<int> drawDarkPanel (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title, int headerHeight = darkHeaderHeight(),
                                           int headerGap = kDarkHeaderGap)
{
    if (title.isNotEmpty())
    {
        auto header = area.removeFromTop (headerHeight);
        g.setColour (Dark::header);
        g.fillRect (header);
        g.setColour (Dark::caption);
        g.setFont (fontFittingWidth (panelTitleFont (headerHeight), title, (float) header.getWidth() - 12.0f, 0.7f));
        g.drawFittedText (title, header.reduced (6, 0), juce::Justification::centred, 1);
        area.removeFromTop (headerGap);
    }
    g.setColour (Dark::body);
    g.fillRect (area);
    return area;
}

/** The body of a panel drawn by drawDarkPanel. */
inline juce::Rectangle<int> darkPanelBody (juce::Rectangle<int> area, bool hasTitle, int headerHeight = darkHeaderHeight(), int headerGap = kDarkHeaderGap)
{
    return hasTitle ? area.withTrimmedTop (headerHeight + headerGap) : area;
}

/** SETUP's panel metrics, in px of the WING's 800 px screen: 64 px headers
    with an 8 px gap under them, 8 px between panels and around the page. */
inline int setupHeaderHeight() { return panelHeaderHeight(); }
inline int setupGap() { return juce::jmax (2, juce::roundToInt (wingPx (8.0f))); }

/** Enables or disables a key, dimming it when disabled (the console's
    unavailable keys are drawn faint). */
inline void enableKey (juce::Component& key, bool enabled)
{
    key.setEnabled (enabled);
    key.setAlpha (enabled ? 1.0f : 0.35f);
}

/** An outlined screen key with an icon (the WING's SAVE / LOAD / REMOVE ... keys). */
inline void styleIconKey (WingKey& k, bool iconAbove = false)
{
    k.outlined = ! iconAbove;
    k.iconAbove = iconAbove;
    k.offColour = iconAbove ? Dark::body : Dark::key;
}

//==============================================================================
/** A toolbar key: an icon over a small caption (the LIBRARY's INT / USB / SHOW,
    REMOVE, RENAME, SKIP, LINK ... keys). The icon and caption can have their
    own colour (SKIP is yellow, LINK green); a lit key is filled. */
class ToolKey final : public juce::Button, private juce::Timer
{
public:
    ToolKey (const juce::String& keyText, Icon keyIcon, juce::Colour inkColour = Dark::text) : juce::Button (keyText), icon (keyIcon), ink (inkColour)
    {
        startTimerHz (4);
    }

    std::function<bool()> isLit;
    std::function<juce::String()> caption; // overrides the text (SHOW: "5 SCNS")
    juce::Colour litColour = Dark::on;
    /** Lines of print instead of the icon and caption (LIBRARY's TYPE / ↓ / NAME). */
    juce::StringArray faceLines;
    /** The icon alone, filling the key (LIBRARY's sort-by-date key). */
    bool iconOnly = false;

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const bool lit = isLit != nullptr && isLit();
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        if (lit || over || down)
        {
            g.setColour (lit ? litColour : Dark::key.withAlpha (down ? 0.9f : 0.5f));
            g.fillRoundedRectangle (r, 2.0f);
        }
        auto c = lit ? juce::Colours::black : ink;
        if (! isEnabled())
            c = c.withAlpha (0.3f);
        if (! faceLines.isEmpty())
        {
            g.setColour (c);
            g.setFont (wingFontPx (r.getHeight() * 0.17f));
            const float lineH = r.getHeight() / (float) (faceLines.size() + 1);
            for (int i = 0; i < faceLines.size(); ++i)
            {
                const auto line = r.withY (r.getY() + lineH * (0.5f + (float) i)).withHeight (lineH);
                if (faceLines[i] == "|")
                {
                    // A small down arrow.
                    const auto cx = line.getCentreX();
                    const float h = line.getHeight() * 0.7f, y0 = line.getCentreY() - h * 0.5f;
                    g.drawLine (cx, y0, cx, y0 + h * 0.8f, juce::jmax (1.0f, h * 0.14f));
                    juce::Path head;
                    head.addTriangle (cx - h * 0.3f, y0 + h * 0.55f, cx + h * 0.3f, y0 + h * 0.55f, cx, y0 + h);
                    g.fillPath (head);
                }
                else
                    g.drawText (faceLines[i], line, juce::Justification::centred);
            }
            return;
        }
        if (iconOnly)
        {
            drawIcon (g, icon, r.reduced (r.getHeight() * 0.14f), c);
            return;
        }
        auto label = r.removeFromBottom (r.getHeight() * 0.34f);
        drawIcon (g, icon, r.reduced (r.getHeight() * 0.1f), c);
        g.setColour (c);
        g.setFont (wingFontPx (juce::jmin (TypeScale::statusCaption * getWingScreenHeight(), label.getHeight() * 0.62f)));
        g.drawFittedText (caption ? caption() : getButtonText(), label.toNearestInt().reduced (1, 0), juce::Justification::centredTop, 1, 0.7f);
    }

private:
    void timerCallback() override
    {
        const bool lit = isLit != nullptr && isLit();
        const auto shown = caption ? caption() : juce::String();
        if (lit != lastLit || shown != lastText)
        {
            lastLit = lit;
            lastText = shown;
            repaint();
        }
    }

    Icon icon;
    juce::Colour ink;
    bool lastLit = false;
    juce::String lastText;
};

//==============================================================================
/** The WING tab row: left-aligned tabs of a fixed width, each with its own
    colour (gold on SETUP, the file-type colours on LIBRARY). The selected tab
    is lighter, with coloured text and a coloured line along its top. */
class TabRow final : public juce::Component
{
public:
    struct Tab
    {
        juce::String name;
        juce::Colour colour = Dark::gold;
        bool enabled = true;
        int gapBefore = 0; // px before this tab (an app-specific tab after the WING's)
    };

    std::function<void (int)> onChange;
    /** Width of a tab as a fraction of the row (0 = share the row equally). */
    float tabFraction = 0.12f;

    void setTabs (std::vector<Tab> t)
    {
        tabs = std::move (t);
        repaint();
    }

    void setCurrent (int index, bool notify = true)
    {
        if (! juce::isPositiveAndBelow (index, (int) tabs.size()) || index == current)
            return;
        current = index;
        repaint();
        if (notify && onChange)
            onChange (index);
    }

    int getCurrent() const noexcept { return current; }

    void setTabEnabled (int index, bool enabled)
    {
        if (juce::isPositiveAndBelow (index, (int) tabs.size()))
        {
            tabs[(size_t) index].enabled = enabled;
            repaint();
        }
    }

    /** Right edge of the last tab. */
    int getTabsRight() const { return tabs.empty() ? 0 : tabArea ((int) tabs.size() - 1).getRight(); }

    juce::Rectangle<int> tabArea (int index) const
    {
        const int n = juce::jmax (1, (int) tabs.size());
        const int w = tabFraction > 0.0f ? juce::roundToInt ((float) getWidth() * tabFraction) : getWidth() / n;
        int x = 0;
        for (int i = 0; i < index; ++i)
            x += w + tabs[(size_t) i + 1].gapBefore;
        return { x, 0, w, getHeight() };
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (Dark::tabBar);
        g.fillRect (getLocalBounds());
        for (int i = 0; i < (int) tabs.size(); ++i)
        {
            const auto& t = tabs[(size_t) i];
            const bool sel = i == current;
            auto r = tabArea (i).reduced (1, 0).toFloat();
            g.setColour (sel ? Dark::tabOn : Dark::tabOff);
            g.fillRect (r);
            if (sel)
            {
                // The WING's 4 px top line (of a 56 px tab).
                g.setColour (t.colour);
                g.fillRect (r.withHeight (juce::jmax (2.0f, std::round (r.getHeight() * 0.07f))));
            }
            g.setColour (sel ? t.colour : (t.enabled ? Dark::textDim : Dark::textDim.withAlpha (0.4f)));
            const float cap = (sel ? TypeScale::tab : TypeScale::tab * 0.85f) * getWingScreenHeight();
            g.setFont (wingFontPx (juce::jmin (cap, r.getHeight() * 0.38f)));
            g.drawFittedText (t.name, r.toNearestInt().reduced (4, 0), juce::Justification::centred, 1, 0.8f);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        for (int i = 0; i < (int) tabs.size(); ++i)
            if (tabArea (i).contains (e.getPosition()) && tabs[(size_t) i].enabled)
                setCurrent (i);
    }

private:
    std::vector<Tab> tabs;
    int current = 0;
};

//==============================================================================
/** A WING choice row (NORM | FINE | AUTO): one key lit, blue with dark ink by
    default; PRE/PFL red, POST/AFL green and LIVE/STUDIO/SIP olive as on the
    console. Polls its getter so changes made elsewhere show. */
class ChoiceKeys final : public juce::Component, private juce::Timer
{
public:
    ChoiceKeys (juce::StringArray labels, std::function<int()> get, std::function<void (int)> set, std::vector<juce::Colour> lit = {})
        : names (std::move (labels)), getter (std::move (get)), setter (std::move (set)), litColours (std::move (lit))
    {
        startTimerHz (5);
    }

    static std::vector<juce::Colour> prePost() { return { Dark::pre, Dark::post }; }
    static std::vector<juce::Colour> autoPrePost() { return { Dark::choice, Dark::pre, Dark::post }; }

    /** Keys stacked top to bottom (MONITORS' SWITCH MODE, fig. 80). */
    bool vertical = false;

    void paint (juce::Graphics& g) override
    {
        shown = getter != nullptr ? getter() : -1;
        const bool enabled = isEnabled();
        auto r = getLocalBounds().toFloat();
        // The keys sit in a black frame, as on the console.
        g.setColour (juce::Colours::black);
        g.fillRect (r);
        r = r.reduced (1.0f);
        const float w = r.getWidth() / (float) juce::jmax (1, names.size());
        const float h = r.getHeight() / (float) juce::jmax (1, names.size());
        for (int i = 0; i < names.size(); ++i)
        {
            auto k = vertical ? juce::Rectangle<float> (r.getX(), r.getY() + h * (float) i, r.getWidth(), h).reduced (0.0f, 1.0f)
                              : juce::Rectangle<float> (r.getX() + w * (float) i, r.getY(), w, r.getHeight()).reduced (1.0f, 0.0f);
            const bool on = i == shown;
            auto fill = on ? (i < (int) litColours.size() ? litColours[(size_t) i] : Dark::choice) : Dark::key;
            // Dark ink on a lit key, light ink on the grey of a lit OFF.
            auto ink = on ? (fill.getPerceivedBrightness() < 0.4f ? Dark::text : Dark::choiceInk) : Dark::caption;
            if (! enabled)
            {
                fill = fill.withMultipliedSaturation (0.5f).darker (0.55f);
                ink = on ? ink : Dark::textDim.darker (0.3f);
            }
            g.setColour (fill);
            g.fillRect (k);
            g.setColour (on && ! enabled ? Dark::text.withAlpha (0.45f) : ink);
            g.setFont (wingFontPx (juce::jmin (wingKeyCap (k.getHeight()), k.getWidth() * 0.3f)));
            g.drawFittedText (names[i], k.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.7f);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (names.isEmpty() || setter == nullptr || ! getLocalBounds().contains (e.getPosition()))
            return;
        setter (juce::jlimit (0, names.size() - 1, vertical ? e.y * names.size() / juce::jmax (1, getHeight()) : e.x * names.size() / juce::jmax (1, getWidth())));
        repaint();
    }

private:
    void timerCallback() override
    {
        // (Noted here too, so a key that isn't painted doesn't repaint on every tick.)
        if (getter != nullptr && getter() != shown)
        {
            shown = getter();
            repaint();
        }
    }

    juce::StringArray names;
    std::function<int()> getter;
    std::function<void (int)> setter;
    std::vector<juce::Colour> litColours;
    int shown = -2;
};

//==============================================================================
/** The console's small square "ON" key: warm yellow with black ink when on. */
class OnKey final : public juce::Component, private juce::Timer
{
public:
    OnKey (std::function<bool()> get, std::function<void (bool)> set) : getter (std::move (get)), setter (std::move (set)) { startTimerHz (5); }

    static std::unique_ptr<OnKey> forAtomic (std::atomic<bool>& flag)
    {
        return std::make_unique<OnKey> ([&flag] { return flag.load(); }, [&flag] (bool v) { flag = v; });
    }

    static std::unique_ptr<OnKey> forSetting (juce::PropertiesFile& settings, const juce::String& key, bool defaultValue)
    {
        return std::make_unique<OnKey> ([&settings, key, defaultValue] { return settings.getBoolValue (key, defaultValue); },
                                        [&settings, key] (bool v) { settings.setValue (key, v); });
    }

    /** A key that only shows a state (the controls of features that aren't there yet). */
    static std::unique_ptr<OnKey> fixed (bool state)
    {
        auto k = std::make_unique<OnKey> ([state] { return state; }, nullptr);
        k->setEnabled (false);
        return k;
    }

    juce::String text { "ON" };

    void paint (juce::Graphics& g) override
    {
        shown = getter != nullptr && getter();
        // [3.1] a 48 x 40 key: a 2 px black frame round the face.
        auto r = getLocalBounds().toFloat();
        auto fill = shown ? Dark::on : Dark::key;
        if (! isEnabled())
            fill = fill.withMultipliedSaturation (0.5f).darker (0.55f);
        g.setColour (juce::Colours::black);
        g.fillRect (r);
        g.setColour (fill);
        g.fillRect (r.reduced (juce::jmax (1.0f, (float) juce::roundToInt (r.getHeight() / 20.0f))));
        g.setColour (shown ? (isEnabled() ? juce::Colours::black : juce::Colours::black.withAlpha (0.6f)) : (isEnabled() ? Dark::textDim : Dark::textDim.darker (0.3f)));
        // [3.1] small print: a 10 px cap in a 40 px key.
        g.setFont (wingFontPx (wingKeyCap (r.getHeight(), 0.27f)));
        g.drawFittedText (text, r.toNearestInt(), juce::Justification::centred, 1);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (setter != nullptr && getLocalBounds().contains (e.getPosition()))
        {
            setter (! (getter != nullptr && getter()));
            repaint();
        }
    }

private:
    void timerCallback() override
    {
        // (Noted here too, so a key that isn't painted doesn't repaint on every tick.)
        if (getter != nullptr && getter() != shown)
        {
            shown = getter();
            repaint();
        }
    }

    std::function<bool()> getter;
    std::function<void (bool)> setter;
    bool shown = false;
};

//==============================================================================
/** A WING dropdown field: a dark well with a small caption over a large value,
    and a separate key at its right (vertical dots, or a down arrow). */
class DropdownField final : public juce::Component, private juce::Timer
{
public:
    DropdownField (juce::String captionText, std::function<juce::String()> value, std::function<void (DropdownField&)> menu = {})
        : caption (std::move (captionText)), getValue (std::move (value)), showMenu (std::move (menu))
    {
        startTimerHz (3);
    }

    void setCaption (juce::String text)
    {
        caption = std::move (text);
        repaint();
    }

    bool arrowKey = false;          // a down arrow instead of the dots
    /** A ">" key instead of the dots (XOVER MODE, KEY FILTER): touching it
        calls `onKey` (lit while `keyLit` says so); the field opens the list. */
    bool chevronKey = false;
    std::function<void()> onKey;
    std::function<bool()> keyLit;
    bool keyHidden = false;         // no key (INS: the FX PROCESSOR field has its own > key, D8)
    std::optional<Icon> icon;       // drawn left of the caption (ROUTING group selectors)
    /** The SETUP / MONITOR fields' metrics [3.1]: a 2 px black frame, a key
        30 of 64 px wide behind a 2 px black line, a caption with a 14 px cap
        10 px down and a value with a 20 px cap 34 px down (in a 64 px field). */
    bool consoleMetrics = false;

    /** While its list is open (popups::chooseFrom sets the anchor's
        "wingListOpen" property) the field turns grey, as on the console. */
    bool isListOpen() const { return getProperties().getWithDefault ("wingListOpen", false); }

    void paint (juce::Graphics& g) override
    {
        shownValue = getValue ? getValue() : juce::String();
        auto r = getLocalBounds();
        const float u = (float) r.getHeight() / 64.0f; // consoleMetrics: one WING pixel
        if (consoleMetrics)
        {
            g.setColour (juce::Colours::black);
            g.fillRect (r);
            r = r.reduced (juce::jmax (1, juce::roundToInt (2.0f * u)));
        }
        auto key = keyHidden ? juce::Rectangle<int>()
                             : r.removeFromRight (consoleMetrics ? juce::roundToInt (30.0f * u) : juce::jmin (r.getHeight() * 2 / 3, r.getWidth() / 4));
        if (! keyHidden)
            r.removeFromRight (consoleMetrics ? juce::jmax (1, juce::roundToInt (2.0f * u)) : 3);
        const float alpha = isEnabled() ? 1.0f : 0.45f;
        const bool open = isListOpen();
        g.setColour (open ? Dark::textDim : Dark::well);
        g.fillRect (r);
        const bool lit = chevronKey && keyLit != nullptr && keyLit();
        g.setColour ((lit ? Dark::textDim : Dark::key).withAlpha (alpha));
        g.fillRect (key);
        g.setColour (Dark::text.withAlpha (alpha));
        const auto kc = key.getCentre().toFloat();
        if (chevronKey && ! keyHidden)
        {
            const float s = juce::jlimit (4.0f, screenCap (9.0f), (float) key.getHeight() * 0.16f);
            juce::Path chevron;
            chevron.startNewSubPath (kc.x - s * 0.5f, kc.y - s);
            chevron.lineTo (kc.x + s * 0.5f, kc.y);
            chevron.lineTo (kc.x - s * 0.5f, kc.y + s);
            g.strokePath (chevron, juce::PathStrokeType (juce::jmax (1.5f, s * 0.3f)));
        }
        else if (arrowKey && ! keyHidden)
        {
            juce::Path tri;
            tri.addTriangle (kc.x - 7.0f, kc.y - 4.0f, kc.x + 7.0f, kc.y - 4.0f, kc.x, kc.y + 5.0f);
            g.fillPath (tri);
        }
        else if (! keyHidden)
        {
            const float d = juce::jlimit (3.0f, screenCap (5.0f), (float) key.getHeight() * 0.1f);
            for (int i = -1; i <= 1; ++i)
                g.fillEllipse (juce::Rectangle<float> (d, d).withCentre (kc.translated (0.0f, (float) i * d * 2.2f)));
        }
        auto t = r.reduced (6, 3);
        if (icon.has_value())
        {
            auto ib = t.removeFromLeft (t.getHeight()).reduced (2);
            drawIcon (g, *icon, ib.toFloat(), Dark::text.withAlpha (alpha));
            t.removeFromLeft (6);
        }
        const float screenH = getWingScreenHeight();
        if (consoleMetrics && ! icon.has_value())
        {
            auto tc = r.withTrimmedLeft (juce::roundToInt (7.0f * u)).withTrimmedRight (juce::roundToInt (4.0f * u));
            g.setColour ((open ? Dark::text : Dark::caption).withAlpha (alpha));
            g.setFont (fontFittingWidth (wingFontPx (14.0f * u), caption, (float) tc.getWidth(), 0.6f));
            g.drawFittedText (caption, tc.withY (r.getY() + juce::roundToInt (2.0f * u)).withHeight (juce::roundToInt (26.0f * u)), juce::Justification::centredLeft, 1, 0.6f);
            g.setColour (Dark::text.withAlpha (alpha));
            g.setFont (fontFittingWidth (wingFontPx (20.0f * u), shownValue, (float) tc.getWidth(), 0.8f));
            g.drawFittedText (shownValue, tc.withY (r.getY() + juce::roundToInt (24.0f * u)).withHeight (juce::roundToInt (36.0f * u)), juce::Justification::centredLeft, 1, 0.8f);
            return;
        }
        g.setColour ((open ? Dark::text : Dark::caption).withAlpha (alpha));
        // Narrow fields (small windows) shrink the text rather than cut it.
        g.setFont (fontFittingWidth (wingFontPx (juce::jmin (TypeScale::body * screenH, (float) t.getHeight() * 0.22f)), caption, (float) t.getWidth(), 0.6f));
        g.drawFittedText (caption, t.removeFromTop (t.getHeight() * 40 / 100), juce::Justification::centredLeft, 1, 0.6f);
        g.setColour (Dark::text.withAlpha (alpha));
        g.setFont (fontFittingWidth (wingFontPx (juce::jmin (TypeScale::value * screenH, (float) t.getHeight() * 0.5f)), shownValue, (float) t.getWidth(), 0.8f));
        g.drawFittedText (shownValue, t, juce::Justification::centredLeft, 1, 0.8f);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! isEnabled() || ! getLocalBounds().contains (e.getPosition()))
            return;
        if (chevronKey && onKey && e.x >= getWidth() - (consoleMetrics ? getHeight() / 2 : juce::jmin (getHeight() * 2 / 3, getWidth() / 4)))
        {
            onKey();
            repaint();
            return;
        }
        if (showMenu)
            showMenu (*this);
    }

private:
    void timerCallback() override
    {
        // What is shown is noted here too: a field that isn't painted (on a
        // hidden page) would otherwise ask for a repaint on every tick.
        const bool lit = keyLit != nullptr && keyLit();
        if (auto now = getValue ? getValue() : juce::String(); now != shownValue || isListOpen() != shownOpen || lit != shownLit)
        {
            shownValue = now;
            shownOpen = isListOpen();
            shownLit = lit;
            repaint();
        }
    }

    bool shownOpen = false, shownLit = false;
    juce::String caption, shownValue;
    std::function<juce::String()> getValue;
    std::function<void (DropdownField&)> showMenu;
};

//==============================================================================
/** A WING settings panel: a header bar, then rows of "LABEL ..... [control]"
    in sub-boxes (the darker frames of OPTIONS or SOLO SYSTEM). Controls of
    features the console doesn't have yet are added disabled. */
class SettingsPanel final : public juce::Component
{
public:
    explicit SettingsPanel (juce::String panelTitle = {}) : title (std::move (panelTitle)) {}

    /** A row: label at the left, the control right-aligned, `controlRows` row
        heights wide (0 = the whole row). `heightRows` > 1 for dropdown fields. */
    juce::Component& addRow (const juce::String& label, std::unique_ptr<juce::Component> control, float controlRows, bool enabled = true,
                             bool info = false, float heightRows = 1.0f)
    {
        auto* c = control.release();
        owned.add (c);
        c->setEnabled (enabled);
        addAndMakeVisible (c);
        rows.push_back ({ label, c, controlRows, heightRows, enabled, info, group });
        return *c;
    }

    /** A row with only a caption (the column captions of HA REMOTE, right-aligned). */
    void addCaption (const juce::String& text, bool rightAligned = true, float scale = 0.75f)
    {
        rows.push_back ({ text, nullptr, 0.0f, 0.7f, true, false, group });
        rows.back().rightAligned = rightAligned;
        rows.back().captionScale = scale;
    }

    /** The following rows go into a new sub-box. */
    void newBox() { ++group; }

    /** Shows or hides the row of `control` (rows of another console model);
        the owner lays the panel out again. */
    void setRowShown (juce::Component& control, bool shown)
    {
        for (auto& r : rows)
            if (r.control == &control && r.hidden == shown)
            {
                r.hidden = ! shown;
                control.setVisible (shown);
                repaint();
            }
    }

    /** Renames the row of `control`. */
    void setRowLabel (juce::Component& control, const juce::String& label)
    {
        for (auto& r : rows)
            if (r.control == &control && r.label != label)
            {
                r.label = label;
                repaint();
            }
    }

    /** Height in px of a single row (set by the owner from the screen size). */
    int rowHeight = 28;
    /** The header bar and the black gap under it. */
    int headerHeight = darkHeaderHeight(), headerGap = kDarkHeaderGap;
    /** The console's own row metrics (SETUP): a row of keys is 56 px of the
        800 px screen and a row with an ON key 52 (`rowHeight`); keys are 46 px
        tall, the ON key 46 x 38, 12 px in from the sides. */
    bool wingRows = false;

    bool hasHeader() const { return title.isNotEmpty(); }

    /** Height the panel needs for its rows. */
    int preferredHeight() const
    {
        int px = 0;
        const auto shown = shownRows();
        for (size_t i = 0; i < shown.size(); ++i)
        {
            px += rowPx (*shown[i]);
            // A box's padding, above its first row and below its last.
            if (i == 0 || shown[i]->group != shown[i - 1]->group)
                px += boxPad (*shown[i]);
            // (The last box runs to the panel's bottom: no padding counted under it.)
            if (i + 1 < shown.size() && shown[i]->group != shown[i + 1]->group)
                px += boxPad (*shown[i]);
        }
        const int groups = shown.empty() ? 0 : shown.back()->group + 1;
        return (hasHeader() ? headerHeight + headerGap : 0) + px + juce::jmax (0, groups - 1) * kDarkHeaderGap;
    }

    void paint (juce::Graphics& g) override
    {
        // The body is cut into sub-boxes by black gaps (SOLO SYSTEM, OPTIONS).
        const auto body = drawDarkPanel (g, getLocalBounds(), title, headerHeight, headerGap);
        if (boxes.size() > 1)
        {
            g.setColour (juce::Colours::black);
            g.fillRect (body);
            for (auto& b : boxes)
            {
                g.setColour (Dark::box);
                g.fillRect (b);
            }
        }
        for (auto& r : rows)
        {
            if (r.label.isEmpty() || r.hidden)
                continue;
            auto l = r.labelArea;
            g.setColour (r.enabled ? Dark::caption : Dark::textDim.darker (0.2f));
            const bool caption = r.control == nullptr;
            const float cap = TypeScale::body * getWingScreenHeight() * (caption && wingRows ? r.captionScale : 1.0f);
            g.setFont (wingFontPx (juce::jmin (cap, (float) rowHeight * (caption ? 0.26f : 0.34f))));
            const float d = (float) juce::jmin (l.getHeight() - 6, screenCap (16));
            // The info icon keeps its own room, so a long label in a narrow
            // panel (SETUP > REMOTE at 1280x800) is squashed rather than covered.
            const auto textArea = r.info ? l.withTrimmedRight ((int) d + 8) : l;
            if (r.info)
                g.setFont (fontFittingWidth (g.getCurrentFont(), r.label, (float) textArea.getWidth()));
            const int textW = juce::jmin (textArea.getWidth(), juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), r.label));
            g.drawFittedText (r.label, textArea, caption && r.rightAligned ? juce::Justification::centredRight : juce::Justification::centredLeft,
                              caption ? 2 : 1, 0.75f);
            if (r.info)
            {
                // The WING sets the ⓘ in a column of its own, just left of the keys.
                const float x = wingRows ? (float) l.getRight() - d - (float) inset() * 0.5f : (float) juce::jmin (l.getRight() - (int) d, l.getX() + textW + 10);
                drawIcon (g, Icon::Info, juce::Rectangle<float> (d, d).withCentre ({ x + d * 0.5f, (float) l.getCentreY() }),
                          r.enabled ? Dark::text : Dark::textDim);
            }
        }
    }

    void resized() override
    {
        auto b = darkPanelBody (getLocalBounds(), hasHeader(), headerHeight, headerGap);
        boxes.clear();
        int lastGroup = -1, boxTop = b.getY();
        const Row* previous = nullptr;
        for (auto* shown : shownRows())
        {
            auto& r = *shown;
            if (r.group != lastGroup)
            {
                if (lastGroup >= 0)
                {
                    b.removeFromTop (boxPad (*previous));
                    boxes.push_back (b.withTop (boxTop).withBottom (b.getY()));
                    b.removeFromTop (kDarkHeaderGap);
                }
                boxTop = b.getY();
                b.removeFromTop (boxPad (r));
                lastGroup = r.group;
            }
            previous = &r;
            const int h = rowPx (r);
            auto row = b.removeFromTop (h);
            auto inner = row.reduced (inset(), 0);
            if (r.control != nullptr)
            {
                if (auto* field = dynamic_cast<DropdownField*> (r.control))
                    field->consoleMetrics = wingRows;
                int ch = juce::jmin (inner.getHeight() - 4, h - 6);
                if (wingRows)
                    ch = r.heightRows == 1.0f || isOnKey (r) ? juce::roundToInt ((float) rowHeight * (isOnKey (r) ? 40.0f : 46.0f) / 52.0f)
                                              : h - juce::roundToInt ((float) rowHeight * 16.0f / 52.0f); // a field: 64 px in 80
                if (r.controlRows <= 0.0f)
                {
                    r.control->setBounds (inner.withSizeKeepingCentre (inner.getWidth(), ch));
                    inner = {};
                }
                else
                {
                    const float wide = wingRows && isOnKey (r) ? 48.0f / 52.0f : r.controlRows;
                    const int w = juce::jmin (inner.getWidth() * (wingRows ? 75 : 55) / 100, juce::roundToInt ((float) rowHeight * wide));
                    auto keyArea = inner.removeFromRight (w).withSizeKeepingCentre (w, ch);
                    // [3.1] keys sit 4 px under the top of their row, not centred.
                    if (wingRows && (r.heightRows == 1.0f || isOnKey (r)))
                        keyArea.setY (row.getY() + juce::roundToInt ((float) rowHeight * 4.0f / 52.0f));
                    r.control->setBounds (keyArea);
                    inner.removeFromRight (6);
                }
            }
            r.labelArea = inner;
        }
        if (lastGroup >= 0)
            boxes.push_back (b.withTop (boxTop).withBottom (getHeight()));
    }

private:
    struct Row
    {
        juce::String label;
        juce::Component* control;
        float controlRows, heightRows;
        bool enabled, info;
        int group;
        juce::Rectangle<int> labelArea {};
        bool rightAligned = true;
        float captionScale = 0.75f;
        bool hidden = false;
    };

    std::vector<Row*> shownRows() const
    {
        std::vector<Row*> out;
        for (auto& r : rows)
            if (! r.hidden)
                out.push_back (const_cast<Row*> (&r));
        return out;
    }

    static bool isOnKey (const Row& r) { return dynamic_cast<OnKey*> (r.control) != nullptr; }

    /** [3.1] A box is padded 4 px above and below its rows, except next to a
        dropdown field, whose 80 px row already holds 8 px either side. */
    int boxPad (const Row& r) const { return wingRows && dynamic_cast<DropdownField*> (r.control) != nullptr ? 0 : 4; }

    int inset() const { return wingRows ? juce::roundToInt ((float) rowHeight * 12.0f / 52.0f) : 10; }

    int rowPx (const Row& r) const
    {
        // A single row of keys is a little taller than one with an ON key.
        const float tall = wingRows && r.heightRows == 1.0f && r.control != nullptr && ! isOnKey (r) ? 56.0f / 52.0f : r.heightRows;
        return juce::roundToInt ((float) rowHeight * tall);
    }

    juce::String title;
    juce::OwnedArray<juce::Component> owned;
    std::vector<Row> rows;
    std::vector<juce::Rectangle<int>> boxes;
    int group = 0;
};

//==============================================================================
/** Where a window sits over its page. The WING floats most windows over the
    page dimmed by a 75 % black scrim (INPUT SELECT centred, EQ/RTA SETTINGS
    docked at the right, SELECT MAIN INPUT under the status bar); CHANNEL
    INIT/COPY, SCOPE and FILTER SETUP cover the page. */
struct OverlayPlacement
{
    enum class Kind
    {
        FullPage,
        Centred,
        DockRight,
        BelowStatus
    };

    Kind kind = Kind::FullPage;
    float width = 1.0f, height = 1.0f; // shares of the page

    static OverlayPlacement fullPage() { return {}; }
    static OverlayPlacement centred (float w, float h) { return { Kind::Centred, w, h }; }
    static OverlayPlacement dockRight (float w) { return { Kind::DockRight, w, 1.0f }; }
    static OverlayPlacement belowStatus() { return { Kind::BelowStatus, 1.0f, 1.0f }; }

    bool floats() const noexcept { return kind == Kind::Centred || kind == Kind::DockRight; }

    /** The window's area within `page`. */
    juce::Rectangle<int> windowIn (juce::Rectangle<int> page) const
    {
        switch (kind)
        {
            case Kind::Centred:
                return page.withSizeKeepingCentre (juce::roundToInt ((float) page.getWidth() * width),
                                                   juce::roundToInt ((float) page.getHeight() * height));
            case Kind::DockRight:
                return page.removeFromRight (juce::roundToInt ((float) page.getWidth() * width)).reduced (0, 4);
            case Kind::FullPage:
            case Kind::BelowStatus:
                break;
        }
        return page;
    }
};

/** The title bar's CLOSE key: the caption and a ticked box ("CLOSE ☑"). */
class CloseKey final : public juce::Button
{
public:
    explicit CloseKey (const juce::String& keyText = "CLOSE") : juce::Button (keyText) {}

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat();
        if (over || down)
        {
            g.setColour (Colours::screenKey.withAlpha (down ? 1.0f : 0.6f));
            g.fillRect (r);
        }
        const auto ink = isEnabled() ? Colours::caption : Colours::textDim;
        auto box = r.removeFromRight (juce::jmin (r.getHeight(), r.getWidth() * 0.4f)).reduced (r.getHeight() * 0.14f);
        drawIcon (g, Icon::Check, box, ink);
        g.setColour (ink);
        g.setFont (wingFontPx (juce::jmin (TypeScale::body * getWingScreenHeight(), r.getHeight() * 0.32f)));
        g.drawFittedText (getButtonText(), r.toNearestInt().reduced (2, 0), juce::Justification::centredRight, 1, 0.8f);
    }
};

/** Height of a window's title bar: 64 of the WING's 800 px. */
inline int windowTitleHeight (int windowHeight)
{
    return juce::jlimit (24, juce::jmax (24, windowHeight / 5), juce::roundToInt (getWingScreenHeight() * 0.08f));
}

/** Paints a WING window's title bar (#202020, the title in caption grey, centred
    or left-aligned) and returns the area below it. */
inline juce::Rectangle<int> paintWindowTitle (juce::Graphics& g, juce::Rectangle<int> window, const juce::String& title,
                                              bool leftAligned = false, juce::Colour bar = Colours::screenHeader,
                                              juce::Colour ink = Colours::caption)
{
    auto titleBar = window.removeFromTop (windowTitleHeight (window.getHeight()));
    g.setColour (bar);
    g.fillRect (titleBar);
    g.setColour (ink);
    g.setFont (panelTitleFont (titleBar.getHeight()));
    const int side = titleBar.getHeight() * 3;
    g.drawFittedText (title, leftAligned ? titleBar.withTrimmedLeft (16).withTrimmedRight (side) : titleBar.reduced (side, 0),
                      leftAligned ? juce::Justification::centredLeft : juce::Justification::centred, 1, 0.8f);
    return window;
}

//==============================================================================
/** A window over a screen (the WING's INPUT SELECT, SCOPE or SD CARD windows):
    a title bar with the title and a CLOSE key, and the content below. A
    floating window dims the rest of its page; clicking the dimmed part closes
    it, as does any key of the surface. A full-page window covers the page. */
class ScreenOverlay final : public juce::Component
{
public:
    ScreenOverlay (juce::String titleText, std::unique_ptr<juce::Component> body, OverlayPlacement where = {})
        : title (std::move (titleText)), content (std::move (body)), placement (where)
    {
        closeKey.onClick = [this] { requestClose(); };
        addAndMakeVisible (closeKey);
        if (content != nullptr)
            addAndMakeVisible (*content);
        closesOnScrimClick = placement.floats();
        if (placement.floats())
            TransientPopups::add (this, [safe = juce::Component::SafePointer<ScreenOverlay> (this)] { if (safe != nullptr) safe->requestClose(); });
    }

    ~ScreenOverlay() override { TransientPopups::remove (this); }

    std::function<void()> onClose;
    /** A click on the dimmed page closes the window (floating windows). */
    bool closesOnScrimClick = false;

    juce::Component* getContent() const noexcept { return content.get(); }
    const OverlayPlacement& getPlacement() const noexcept { return placement; }

    /** The CLOSE key's caption ("CLOSE", or "DONE" on FILTER SETUP of old firmware). */
    void setCloseText (const juce::String& t) { closeKey.setButtonText (t); }

    /** The window's area in this component (the rest is the dimmed page). */
    juce::Rectangle<int> windowArea() const { return placement.windowIn (getLocalBounds()); }

    void paint (juce::Graphics& g) override
    {
        const auto window = windowArea();
        if (placement.floats())
        {
            g.setColour (Colours::scrim);
            g.fillRect (getLocalBounds());
            g.setColour (Colours::screenPanel);
            g.fillRect (window);
        }
        else
        {
            g.fillAll (juce::Colours::black);
        }
        paintWindowTitle (g, window, title, placement.kind == OverlayPlacement::Kind::DockRight);
    }

    void resized() override
    {
        auto b = windowArea();
        auto bar = b.removeFromTop (windowTitleHeight (b.getHeight()));
        closeKey.setBounds (bar.removeFromRight (juce::jmin (bar.getHeight() * 3, bar.getWidth() / 3)).reduced (4, 6));
        b.removeFromTop (6);
        if (content != nullptr)
            content->setBounds (placement.floats() ? b.reduced (6, 0).withTrimmedBottom (6) : b);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        // Swallow clicks that would reach the screen underneath; a click on the
        // dimmed page closes a floating window.
        if (closesOnScrimClick && ! windowArea().contains (e.getPosition()))
            requestClose();
    }

private:
    void requestClose()
    {
        if (onClose)
            onClose();
    }

    juce::String title;
    std::unique_ptr<juce::Component> content;
    OverlayPlacement placement;
    CloseKey closeKey;
};

/** A body for an overlay that only explains something. */
class MessageBody final : public juce::Component
{
public:
    MessageBody (juce::String headline, juce::String text) : head (std::move (headline)), body (std::move (text)) {}

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().reduced (24, 16);
        g.setColour (Dark::text);
        g.setFont (wingFontPx (juce::jmin (TypeScale::value * getWingScreenHeight() * 1.2f, (float) getHeight() * 0.06f)));
        g.drawFittedText (head, r.removeFromTop (juce::jmin (screenCap (48), r.getHeight() / 3)), juce::Justification::centredLeft, 2);
        g.setColour (Dark::caption);
        g.setFont (wingFont (TypeScale::body));
        g.drawFittedText (body, r, juce::Justification::topLeft, 8);
    }

private:
    juce::String head, body;
};

/** Owns at most one overlay over a page (create, show over `parent`, close). */
class OverlayHolder
{
public:
    void show (juce::Component& parent, const juce::String& title, std::unique_ptr<juce::Component> body, OverlayPlacement placement = {})
    {
        overlay = std::make_unique<ScreenOverlay> (title, std::move (body), placement);
        overlay->onClose = [this] { juce::MessageManager::callAsync ([this, alive = std::weak_ptr<bool> (token)] { if (! alive.expired()) overlay.reset(); }); };
        parent.addAndMakeVisible (*overlay);
        overlay->setBounds (parent.getLocalBounds());
        overlay->toFront (false);
    }

    void close() { overlay.reset(); }
    bool isShowing() const noexcept { return overlay != nullptr; }
    ScreenOverlay* get() const noexcept { return overlay.get(); }
    void layout (juce::Rectangle<int> bounds)
    {
        if (overlay != nullptr)
            overlay->setBounds (bounds);
    }

private:
    std::unique_ptr<ScreenOverlay> overlay;
    std::shared_ptr<bool> token = std::make_shared<bool> (true);
};

/** The strip's short WING label: "1", "A1", "B1", "M1", "MX1". */
inline juce::String shortStripLabel (const Strip& s)
{
    const auto n = juce::String (s.getNumber());
    switch (s.getKind())
    {
        case StripKind::Channel: return n;
        case StripKind::Aux:     return "A" + n;
        case StripKind::Bus:     return "B" + n;
        case StripKind::Main:    return "M" + n;
        case StripKind::Matrix:  return "MX" + n;
    }
    return n;
}

} // namespace livedaw::ui
