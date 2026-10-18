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

    const juce::Colour choice = Colours::valueBlue;    // lit choice key (MAIN, NORM|FINE|AUTO, MCU|HUI, ...)
    const juce::Colour choiceInk { 0xff0d1a33 };
    const juce::Colour pre = Colours::pfl;             // PRE, PFL
    const juce::Colour post = Colours::green;          // POST, AFL
    const juce::Colour olive { 0xff809800 };           // LIVE|STUDIO|SIP
    const juce::Colour orange = Colours::alt;          // ALT, USB HOST SPEED FS
    const juce::Colour on = Colours::gold;             // ON chip
    const juce::Colour gold = Colours::gold;           // tabs, selection
    const juce::Colour selectedRow { 0xff3b3215 };
    const juce::Colour selectedOutline = Colours::gold;
    const juce::Colour layer { 0xff3d72ce };           // VIEW: the active layer
    const juce::Colour section { 0xff829c01 };         // VIEW: the viewed fader section
    const juce::Colour muteGroup { 0xfff24e52 };       // MUTE GROUPS headers and engaged keys
    const juce::Colour faderFill = Colours::valueBlue;
    const juce::Colour muted { 0xffe0595b };
} // namespace Dark

/** LIBRARY file-type colours (tabs and list chips). */
namespace FileColours
{
    const juce::Colour show { 0xff2486bd };
    const juce::Colour snap { 0xffd08a30 };
    const juce::Colour snip { 0xffc5a16a };
    const juce::Colour clip { 0xffa6c87a };
    const juce::Colour fx { 0xffc47ac4 };
    const juce::Colour chan { 0xffd2d376 };
    const juce::Colour wav { 0xff5ea557 };
} // namespace FileColours

/** Console preferences kept in the app settings (SETUP). */
namespace prefs
{
    constexpr const char* consoleName = "setup.consoleName";
    constexpr const char* confirmLibraryLoad = "setup.confirmLibraryLoad";
    constexpr const char* touchFaderRes = "setup.touchFaderRes";          // 0 NORM, 1 FINE, 2 AUTO
    constexpr const char* clock12h = "setup.clock12h";
    constexpr const char* dateDayFirst = "setup.dateDayFirst";
    constexpr const char* layerLink = "setup.faderScreenLayerLink";       // VIEW screens follow the section's layer
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

    inline bool confirmLoad (UiContext& ctx) { return ctx.settings.getBoolValue (confirmLibraryLoad, true); }
    inline int faderResolution (UiContext& ctx) { return ctx.settings.getIntValue (touchFaderRes, 2); }
    inline bool screenFollowsLayer (UiContext& ctx) { return ctx.settings.getBoolValue (layerLink, true); }
} // namespace prefs

constexpr int kDarkHeaderHeight = 30;
/** The black gap between a panel's header and its body. */
constexpr int kDarkHeaderGap = 4;

/** The font of panel header titles, fitted to a header `headerHeight` px tall. */
inline juce::Font panelTitleFont (int headerHeight)
{
    return wingFontPx (juce::jmin (TypeScale::header * getWingScreenHeight(), (float) headerHeight * 0.42f));
}

/** Draws a dark WING panel: a header bar with the title centred, a thin black
    gap, then the body. Returns the body. An empty title draws only the body. */
inline juce::Rectangle<int> drawDarkPanel (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title, int headerHeight = kDarkHeaderHeight)
{
    if (title.isNotEmpty())
    {
        auto header = area.removeFromTop (headerHeight);
        g.setColour (Dark::header);
        g.fillRect (header);
        g.setColour (Dark::caption);
        g.setFont (panelTitleFont (headerHeight));
        g.drawFittedText (title, header.reduced (6, 0), juce::Justification::centred, 1);
        area.removeFromTop (kDarkHeaderGap);
    }
    g.setColour (Dark::body);
    g.fillRect (area);
    return area;
}

/** The body of a panel drawn by drawDarkPanel. */
inline juce::Rectangle<int> darkPanelBody (juce::Rectangle<int> area, bool hasTitle, int headerHeight = kDarkHeaderHeight)
{
    return hasTitle ? area.withTrimmedTop (headerHeight + kDarkHeaderGap) : area;
}

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
            auto ink = on ? Dark::choiceInk : Dark::caption;
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
        if (getter != nullptr && getter() != shown)
            repaint();
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
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        auto fill = shown ? Dark::on : Dark::key;
        if (! isEnabled())
            fill = fill.withMultipliedSaturation (0.5f).darker (0.55f);
        g.setColour (fill);
        g.fillRect (r);
        g.setColour (juce::Colours::black);
        g.drawRect (r, 1.0f);
        g.setColour (shown ? (isEnabled() ? juce::Colours::black : juce::Colours::black.withAlpha (0.6f)) : (isEnabled() ? Dark::textDim : Dark::textDim.darker (0.3f)));
        g.setFont (wingFontPx (wingKeyCap (r.getHeight(), 0.36f)));
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
        if (getter != nullptr && getter() != shown)
            repaint();
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

    bool arrowKey = false;          // a down arrow instead of the dots
    bool keyHidden = false;         // no key (INS: the FX PROCESSOR field has its own > key, D8)
    std::optional<Icon> icon;       // drawn left of the caption (ROUTING group selectors)

    /** While its list is open (popups::chooseFrom sets the anchor's
        "wingListOpen" property) the field turns grey, as on the console. */
    bool isListOpen() const { return getProperties().getWithDefault ("wingListOpen", false); }

    void paint (juce::Graphics& g) override
    {
        shownValue = getValue ? getValue() : juce::String();
        auto r = getLocalBounds();
        auto key = keyHidden ? juce::Rectangle<int>() : r.removeFromRight (juce::jmin (r.getHeight() * 2 / 3, r.getWidth() / 4));
        if (! keyHidden)
            r.removeFromRight (3);
        const float alpha = isEnabled() ? 1.0f : 0.45f;
        const bool open = isListOpen();
        g.setColour (open ? Dark::textDim : Dark::well);
        g.fillRect (r);
        g.setColour (Dark::key.withAlpha (alpha));
        g.fillRect (key);
        g.setColour (Dark::text.withAlpha (alpha));
        const auto kc = key.getCentre().toFloat();
        if (arrowKey && ! keyHidden)
        {
            juce::Path tri;
            tri.addTriangle (kc.x - 7.0f, kc.y - 4.0f, kc.x + 7.0f, kc.y - 4.0f, kc.x, kc.y + 5.0f);
            g.fillPath (tri);
        }
        else if (! keyHidden)
        {
            const float d = juce::jlimit (3.0f, 5.0f, (float) key.getHeight() * 0.1f);
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
        g.setColour ((open ? Dark::text : Dark::caption).withAlpha (alpha));
        g.setFont (wingFontPx (juce::jmin (TypeScale::body * screenH, (float) t.getHeight() * 0.22f)));
        g.drawFittedText (caption, t.removeFromTop (t.getHeight() * 40 / 100), juce::Justification::centredLeft, 1, 0.6f);
        g.setColour (Dark::text.withAlpha (alpha));
        g.setFont (wingFontPx (juce::jmin (TypeScale::value * screenH, (float) t.getHeight() * 0.5f)));
        g.drawFittedText (shownValue, t, juce::Justification::centredLeft, 1, 0.8f);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (isEnabled() && showMenu && getLocalBounds().contains (e.getPosition()))
            showMenu (*this);
    }

private:
    void timerCallback() override
    {
        if ((getValue && getValue() != shownValue) || isListOpen() != shownOpen)
        {
            shownOpen = isListOpen();
            repaint();
        }
    }

    bool shownOpen = false;
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
    void addCaption (const juce::String& text, bool rightAligned = true)
    {
        rows.push_back ({ text, nullptr, 0.0f, 0.7f, true, false, group });
        rows.back().rightAligned = rightAligned;
    }

    /** The following rows go into a new sub-box. */
    void newBox() { ++group; }

    /** Height in px of a single row (set by the owner from the screen size). */
    int rowHeight = 28;
    bool hasHeader() const { return title.isNotEmpty(); }

    /** Height the panel needs for its rows. */
    int preferredHeight() const
    {
        float units = 0.0f;
        for (auto& r : rows)
            units += r.heightRows;
        const int groups = rows.empty() ? 0 : rows.back().group + 1;
        return (hasHeader() ? kDarkHeaderHeight + kDarkHeaderGap : 0) + juce::roundToInt (units * (float) rowHeight) + groups * 8 + juce::jmax (0, groups - 1) * kDarkHeaderGap;
    }

    void paint (juce::Graphics& g) override
    {
        // The body is cut into sub-boxes by black gaps (SOLO SYSTEM, OPTIONS).
        const auto body = drawDarkPanel (g, getLocalBounds(), title);
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
            if (r.label.isEmpty())
                continue;
            auto l = r.labelArea;
            g.setColour (r.enabled ? Dark::caption : Dark::textDim.darker (0.2f));
            const bool caption = r.control == nullptr;
            g.setFont (wingFontPx (juce::jmin (TypeScale::body * getWingScreenHeight(), (float) rowHeight * (caption ? 0.26f : 0.34f))));
            const int textW = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), r.label);
            g.drawFittedText (r.label, l, caption && r.rightAligned ? juce::Justification::centredRight : juce::Justification::centredLeft, 1, 0.75f);
            if (r.info)
            {
                const float d = (float) juce::jmin (l.getHeight() - 6, 16);
                drawIcon (g, Icon::Info, juce::Rectangle<float> (d, d).withCentre ({ (float) juce::jmin (l.getRight() - (int) d, l.getX() + textW + 10) + d * 0.5f,
                                                                                    (float) l.getCentreY() }),
                          r.enabled ? Dark::text : Dark::textDim);
            }
        }
    }

    void resized() override
    {
        auto b = darkPanelBody (getLocalBounds(), hasHeader());
        boxes.clear();
        int lastGroup = -1;
        juce::Rectangle<int> box;
        for (auto& r : rows)
        {
            if (r.group != lastGroup)
            {
                if (lastGroup >= 0)
                {
                    boxes.push_back (box.withBottom (box.getBottom() + 4));
                    b.removeFromTop (4);
                    b.removeFromTop (kDarkHeaderGap);
                }
                box = b.withHeight (0);
                b.removeFromTop (4);
                lastGroup = r.group;
            }
            const int h = juce::roundToInt ((float) rowHeight * r.heightRows);
            auto row = b.removeFromTop (h);
            box = box.getUnion (row);
            auto inner = row.reduced (10, 0);
            if (r.control != nullptr)
            {
                const int ch = juce::jmin (inner.getHeight() - 4, juce::roundToInt ((float) rowHeight * r.heightRows) - 6);
                if (r.controlRows <= 0.0f)
                {
                    r.control->setBounds (inner.withSizeKeepingCentre (inner.getWidth(), ch));
                    inner = {};
                }
                else
                {
                    const int w = juce::jmin (inner.getWidth() * 55 / 100, juce::roundToInt ((float) rowHeight * r.controlRows));
                    r.control->setBounds (inner.removeFromRight (w).withSizeKeepingCentre (w, ch));
                    inner.removeFromRight (6);
                }
            }
            r.labelArea = inner;
        }
        if (lastGroup >= 0)
            boxes.push_back (box.withBottom (getHeight()));
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
    };

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
        g.drawFittedText (head, r.removeFromTop (juce::jmin (48, r.getHeight() / 3)), juce::Justification::centredLeft, 2);
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
