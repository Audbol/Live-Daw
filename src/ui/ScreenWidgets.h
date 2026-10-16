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

/** The darker palette of the WING's SETUP, LIBRARY and VIEW screens (manual
    figs 33-61): near-black panels, blue choice keys, red PRE/PFL and green
    POST/AFL, the warm yellow ON chip and gold SETUP tabs. */
namespace Dark
{
    const juce::Colour tabBar { 0xff111111 };
    const juce::Colour tabOff { 0xff1d1d1d };
    const juce::Colour tabOn { 0xff2f2f2f };
    const juce::Colour header { 0xff262626 };
    const juce::Colour body { 0xff1e1e1e };
    const juce::Colour box { 0xff2a2a2a };       // sub-boxes and rows inside a panel
    const juce::Colour key { 0xff3c3c3c };       // unlit keys
    const juce::Colour keyOutline { 0xff8e8e8e };
    const juce::Colour well { 0xff111111 };      // dropdown fields
    const juce::Colour text { 0xffeeeeee };
    const juce::Colour textDim { 0xff8a8a8a };

    const juce::Colour choice { 0xff3e71cb };    // lit choice key (NORM|FINE|AUTO, MCU|HUI, ...)
    const juce::Colour choiceInk { 0xff0d1a33 };
    const juce::Colour pre { 0xfffc3a3e };       // PRE, PFL
    const juce::Colour post { 0xff02c136 };      // POST, AFL
    const juce::Colour olive { 0xff819902 };     // LIVE|STUDIO|SIP
    const juce::Colour orange { 0xffbe5e12 };    // USB HOST SPEED FS
    const juce::Colour on { 0xfff3c650 };        // ON chip
    const juce::Colour gold { 0xffe8c35a };      // SETUP tabs
    const juce::Colour selectedRow { 0xff3b3215 };
    const juce::Colour selectedOutline { 0xffc8a040 };
    const juce::Colour layer { 0xff3d72ce };     // VIEW: the active layer
    const juce::Colour section { 0xff829c01 };   // VIEW: the viewed fader section
    const juce::Colour muteGroup { 0xfff24e52 }; // MUTE GROUPS headers and engaged keys
    const juce::Colour faderFill { 0xff4f8de4 };
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

    inline bool confirmLoad (UiContext& ctx) { return ctx.settings.getBoolValue (confirmLibraryLoad, true); }
    inline int faderResolution (UiContext& ctx) { return ctx.settings.getIntValue (touchFaderRes, 2); }
    inline bool screenFollowsLayer (UiContext& ctx) { return ctx.settings.getBoolValue (layerLink, true); }
} // namespace prefs

constexpr int kDarkHeaderHeight = 30;

/** Draws a dark WING panel: a header bar with the title centred, a thin black
    gap, then the body. Returns the body. An empty title draws only the body. */
inline juce::Rectangle<int> drawDarkPanel (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title, int headerHeight = kDarkHeaderHeight)
{
    if (title.isNotEmpty())
    {
        auto header = area.removeFromTop (headerHeight);
        g.setColour (Dark::header);
        g.fillRect (header);
        g.setColour (Dark::text);
        g.setFont (juce::FontOptions (juce::jmin (19.0f, (float) headerHeight * 0.62f), juce::Font::bold));
        g.drawFittedText (title, header.reduced (6, 0), juce::Justification::centred, 1);
        area.removeFromTop (2);
    }
    g.setColour (Dark::body);
    g.fillRect (area);
    return area;
}

/** The body of a panel drawn by drawDarkPanel. */
inline juce::Rectangle<int> darkPanelBody (juce::Rectangle<int> area, bool hasTitle, int headerHeight = kDarkHeaderHeight)
{
    return hasTitle ? area.withTrimmedTop (headerHeight + 2) : area;
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
        g.setFont (juce::FontOptions (juce::jlimit (8.0f, 13.0f, label.getHeight() * 0.85f), juce::Font::bold));
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
                g.setColour (t.colour);
                g.fillRect (r.withHeight (3.0f));
            }
            g.setColour (sel ? t.colour : (t.enabled ? Dark::textDim : Dark::textDim.withAlpha (0.4f)));
            g.setFont (juce::FontOptions (juce::jlimit (12.0f, 22.0f, r.getHeight() * 0.5f), juce::Font::bold));
            g.drawFittedText (t.name, r.toNearestInt().reduced (4, 0), juce::Justification::centred, 1);
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

    void paint (juce::Graphics& g) override
    {
        shown = getter != nullptr ? getter() : -1;
        const bool enabled = isEnabled();
        auto r = getLocalBounds().toFloat();
        const float w = r.getWidth() / (float) juce::jmax (1, names.size());
        for (int i = 0; i < names.size(); ++i)
        {
            auto k = juce::Rectangle<float> (r.getX() + w * (float) i, r.getY(), w, r.getHeight()).reduced (1.0f, 0.0f);
            const bool on = i == shown;
            auto fill = on ? (i < (int) litColours.size() ? litColours[(size_t) i] : Dark::choice) : Dark::key;
            auto ink = on ? Dark::choiceInk : Dark::text;
            if (! enabled)
            {
                fill = fill.withMultipliedSaturation (0.5f).darker (0.55f);
                ink = on ? ink : Dark::textDim.darker (0.3f);
            }
            g.setColour (fill);
            g.fillRect (k);
            g.setColour (on && ! enabled ? Dark::text.withAlpha (0.45f) : ink);
            g.setFont (juce::FontOptions (juce::jlimit (10.0f, 18.0f, juce::jmin (k.getHeight() * 0.5f, k.getWidth() * 0.34f)), juce::Font::bold));
            g.drawFittedText (names[i], k.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.7f);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (names.isEmpty() || setter == nullptr || ! getLocalBounds().contains (e.getPosition()))
            return;
        setter (juce::jlimit (0, names.size() - 1, e.x * names.size() / juce::jmax (1, getWidth())));
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
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (shown ? (isEnabled() ? juce::Colours::black : juce::Colours::black.withAlpha (0.6f)) : (isEnabled() ? Dark::text : Dark::textDim.darker (0.3f)));
        g.setFont (juce::FontOptions (juce::jlimit (9.0f, 15.0f, r.getHeight() * 0.48f), juce::Font::bold));
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
    std::optional<Icon> icon;       // drawn left of the caption (ROUTING group selectors)

    void paint (juce::Graphics& g) override
    {
        shownValue = getValue ? getValue() : juce::String();
        auto r = getLocalBounds();
        auto key = r.removeFromRight (juce::jmin (r.getHeight() * 2 / 3, r.getWidth() / 4));
        r.removeFromRight (3);
        const float alpha = isEnabled() ? 1.0f : 0.45f;
        g.setColour (Dark::well);
        g.fillRect (r);
        g.setColour (Dark::key.withAlpha (alpha));
        g.fillRect (key);
        g.setColour (Dark::text.withAlpha (alpha));
        const auto kc = key.getCentre().toFloat();
        if (arrowKey)
        {
            juce::Path tri;
            tri.addTriangle (kc.x - 7.0f, kc.y - 4.0f, kc.x + 7.0f, kc.y - 4.0f, kc.x, kc.y + 5.0f);
            g.fillPath (tri);
        }
        else
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
        g.setColour (Dark::textDim.withAlpha (alpha));
        g.setFont (juce::FontOptions (juce::jlimit (9.0f, 13.0f, (float) t.getHeight() * 0.3f), juce::Font::bold));
        g.drawFittedText (caption, t.removeFromTop (t.getHeight() * 38 / 100), juce::Justification::bottomLeft, 1);
        g.setColour (Dark::text.withAlpha (alpha));
        g.setFont (juce::FontOptions (juce::jlimit (12.0f, 24.0f, (float) t.getHeight() * 0.85f), juce::Font::bold));
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
        if (getValue && getValue() != shownValue)
            repaint();
    }

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
        return (hasHeader() ? kDarkHeaderHeight + 2 : 0) + juce::roundToInt (units * (float) rowHeight) + groups * 8 + 4;
    }

    void paint (juce::Graphics& g) override
    {
        drawDarkPanel (g, getLocalBounds(), title);
        for (auto& b : boxes)
        {
            g.setColour (Dark::box);
            g.fillRect (b);
        }
        for (auto& r : rows)
        {
            if (r.label.isEmpty())
                continue;
            auto l = r.labelArea;
            g.setColour (r.enabled ? Dark::text : Dark::textDim.darker (0.2f));
            const bool caption = r.control == nullptr;
            g.setFont (juce::FontOptions (juce::jlimit (10.0f, 15.0f, (float) rowHeight * (caption ? 0.4f : 0.5f)), juce::Font::bold));
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
        auto b = darkPanelBody (getLocalBounds(), hasHeader()).reduced (4, 4);
        boxes.clear();
        int lastGroup = -1;
        juce::Rectangle<int> box;
        for (auto& r : rows)
        {
            if (r.group != lastGroup)
            {
                if (lastGroup >= 0)
                {
                    boxes.push_back (box);
                    b.removeFromTop (6);
                }
                box = b.withHeight (0);
                lastGroup = r.group;
            }
            const int h = juce::roundToInt ((float) rowHeight * r.heightRows);
            auto row = b.removeFromTop (h);
            box = box.getUnion (row);
            auto inner = row.reduced (6, 0);
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
            boxes.push_back (box);
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
/** A window over a screen (the WING's INPUT SELECT, SCOPE or SD CARD windows):
    a title bar with a CLOSE key, and the content below. */
class ScreenOverlay final : public juce::Component
{
public:
    ScreenOverlay (juce::String titleText, std::unique_ptr<juce::Component> body) : title (std::move (titleText)), content (std::move (body))
    {
        closeKey.offColour = Dark::key;
        closeKey.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (closeKey);
        if (content != nullptr)
            addAndMakeVisible (*content);
    }

    std::function<void()> onClose;
    juce::Component* getContent() const noexcept { return content.get(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black);
        auto bar = getLocalBounds().removeFromTop (barHeight());
        g.setColour (Dark::header);
        g.fillRect (bar);
        g.setColour (Dark::text);
        g.setFont (juce::FontOptions (juce::jmin (22.0f, (float) bar.getHeight() * 0.55f), juce::Font::bold));
        g.drawText (title, bar.reduced (12, 0), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        auto bar = b.removeFromTop (barHeight());
        closeKey.setBounds (bar.removeFromRight (juce::jmin (130, bar.getWidth() / 4)).reduced (4));
        b.removeFromTop (6);
        if (content != nullptr)
            content->setBounds (b);
    }

    // Swallow clicks that would reach the screen underneath.
    void mouseDown (const juce::MouseEvent&) override {}

private:
    int barHeight() const { return juce::jlimit (30, 44, getHeight() / 11); }

    juce::String title;
    std::unique_ptr<juce::Component> content;
    WingKey closeKey { "CLOSE", Icon::Check };
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
        g.setFont (juce::FontOptions (juce::jmin (26.0f, (float) getHeight() * 0.08f), juce::Font::bold));
        g.drawFittedText (head, r.removeFromTop (juce::jmin (48, r.getHeight() / 3)), juce::Justification::centredLeft, 2);
        g.setColour (Dark::textDim);
        g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        g.drawFittedText (body, r, juce::Justification::topLeft, 8);
    }

private:
    juce::String head, body;
};

/** Owns at most one overlay over a page (create, show over `parent`, close). */
class OverlayHolder
{
public:
    void show (juce::Component& parent, const juce::String& title, std::unique_ptr<juce::Component> body)
    {
        overlay = std::make_unique<ScreenOverlay> (title, std::move (body));
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
