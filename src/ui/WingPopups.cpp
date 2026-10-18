#include "ui/WingPopups.h"

#include "ui/LookAndFeel.h"
#include "ui/ScreenWidgets.h"

namespace livedaw::ui::popups
{

namespace
{
    std::function<juce::Component* (juce::Component&)>& layerProvider()
    {
        static std::function<juce::Component* (juce::Component&)> provider;
        return provider;
    }

    /** The component pop-ups for `host` cover: the main display's pop-up layer,
        or `host`'s window. */
    juce::Component* layerFor (juce::Component& host)
    {
        if (auto& provider = layerProvider(); provider != nullptr)
            if (auto* layer = provider (host))
                return layer;
        return host.getTopLevelComponent();
    }

    float screenHeight() { return getWingScreenHeight(); }

    /** A key of a pop-up: #101010 with a grey outline (keyboard keys, list rows)
        or #303030 (window keys). */
    void paintKey (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fill, juce::Colour outline)
    {
        g.setColour (fill);
        g.fillRect (r);
        g.setColour (outline);
        g.drawRect (r, 1.0f);
    }

    //==========================================================================
    /** What every pop-up shares: it covers its layer, owns itself (deleted when
        closed, or at shutdown), and closes as cancelled on Esc or when a key of
        the surface is pressed. */
    class Popup : public juce::Component, public juce::DeletedAtShutdown
    {
    public:
        Popup()
        {
            setWantsKeyboardFocus (true);
            TransientPopups::add (this, [safe = juce::Component::SafePointer<Popup> (this)] { if (safe != nullptr) safe->cancel(); });
        }

        ~Popup() override { TransientPopups::remove (this); }

        void open (juce::Component& layer)
        {
            layer.addAndMakeVisible (this);
            setBounds (layer.getLocalBounds());
            toFront (false);
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<Popup> (this)]
            {
                if (safe != nullptr && safe->isShowing())
                    safe->takeFocus();
            });
        }

        /** Closes the pop-up without its action. */
        void cancel() { closeThen ({}); }

    protected:
        /** Hides and deletes the pop-up, then runs `after` (which may open the
            next one). */
        void closeThen (std::function<void()> after)
        {
            if (closing)
                return;
            closing = true;
            TransientPopups::remove (this);
            onClosing();
            setVisible (false);
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<Popup> (this), after = std::move (after)]
            {
                delete safe.getComponent();
                if (after != nullptr)
                    after();
            });
        }

        virtual void onClosing() {}
        virtual void takeFocus() { grabKeyboardFocus(); }

        bool keyPressed (const juce::KeyPress& k) override
        {
            if (k == juce::KeyPress::escapeKey)
            {
                cancel();
                return true;
            }
            return false;
        }

        bool closing = false;
    };
} // namespace

void setPopupLayerProvider (std::function<juce::Component* (juce::Component&)> provider)
{
    layerProvider() = std::move (provider);
}

//==============================================================================
// The keyboard (D2, D9): the field band, the suggestion band and the key band,
// #202020 with black gaps between them; keys #101010 with a #484848 outline.

struct WingKeyboard::Impl
{
    enum class Kind
    {
        Char,
        Backspace,
        Caps,
        Shift,
        Space,
        Action,
        Suggestion
    };

    struct Key
    {
        Kind kind;
        juce::String main, shifted; // a letter's shifted form is its capital
        juce::Rectangle<float> area;
        int suggestion = -1;
    };

    juce::String text;
    int caret = 0;
    int maxLength = 16;
    bool shift = false, caps = false, fieldVisible = true, suggestionRow = false;
    std::optional<Icon> icon, actionIcon;
    juce::String actionText { "DONE" };
    juce::StringArray suggestions;

    std::vector<Key> keys;
    juce::Rectangle<float> fieldBand, suggestionBand, keyBand, field, clearKey, textArea;
    int pressed = -1;

    bool upper() const noexcept { return shift || caps; }

    static bool isLetter (const juce::String& s) { return s.length() == 1 && juce::CharacterFunctions::isLetter (s[0]); }

    juce::Font textFont() const
    {
        return wingFontPx (juce::jmin (TypeScale::value * screenHeight() * 1.15f, field.getHeight() * 0.36f));
    }

    Icon resolvedActionIcon() const
    {
        if (actionIcon.has_value())
            return *actionIcon;
        return actionText.trim().equalsIgnoreCase ("NEXT") ? Icon::ChevronRight : Icon::Check;
    }

    void layout (juce::Rectangle<float> r)
    {
        keys.clear();
        const bool showSuggestions = suggestionRow || ! suggestions.isEmpty();
        const float units = 5.2f + (fieldVisible ? 1.4f : 0.0f) + (showSuggestions ? 1.1f : 0.0f);
        const float u = r.getHeight() / units;
        const float gapY = juce::jmax (2.0f, u * 0.07f);

        fieldBand = suggestionBand = {};
        if (fieldVisible)
        {
            fieldBand = r.removeFromTop (u * 1.4f - gapY);
            r.removeFromTop (gapY);
        }
        if (showSuggestions)
        {
            suggestionBand = r.removeFromTop (u * 1.1f - gapY);
            r.removeFromTop (gapY);
        }
        keyBand = r;

        // The field: black, 70 % of the width, the clear key at its right.
        field = fieldBand.withSizeKeepingCentre (fieldBand.getWidth() * 0.7f, fieldBand.getHeight() * 0.8f);
        clearKey = field.withLeft (field.getRight() - field.getHeight()).reduced (field.getHeight() * 0.2f);
        textArea = field.withTrimmedRight (field.getHeight()).withTrimmedLeft (field.getHeight() * 0.25f);
        if (icon.has_value())
            textArea.removeFromLeft (field.getHeight() * 0.9f);

        // Keys on a grid of 11.5 key pitches across, 8 px gaps of 64 px pitches.
        const float margin = keyBand.getWidth() * 0.01f;
        const auto inner = keyBand.reduced (margin, margin);
        const float px = inner.getWidth() / 11.5f, py = inner.getHeight() / 5.0f;
        const float gx = px * 0.125f, gy = py * 0.125f;
        auto key = [&] (Kind kind, int row, float x, float w, juce::String main = {}, juce::String shifted = {})
        {
            keys.push_back ({ kind, std::move (main), std::move (shifted),
                              { inner.getX() + x * px, inner.getY() + (float) row * py, w * px - gx, py - gy } });
        };
        auto chars = [&] (int row, float x, const char* mains, const char* shifteds)
        {
            const juce::String m (mains), s (shifteds);
            for (int i = 0; i < m.length(); ++i)
                key (Kind::Char, row, x + (float) i, 1.0f, juce::String::charToString (m[i]), juce::String::charToString (s[i]));
        };
        chars (0, 0.0f, "1234567890", "!'#$%=&*+|");
        key (Kind::Backspace, 0, 10.0f, 1.5f);
        chars (1, 0.45f, "qwertyuiop-", "QWERTYUIOP_");
        chars (2, 0.66f, "asdfghjkl()", "ASDFGHJKL[]");
        key (Kind::Caps, 3, 0.0f, 1.0f);
        chars (3, 1.09f, "zxcvbnm,./", "ZXCVBNM<>:");
        key (Kind::Shift, 4, 0.0f, 2.0f);
        key (Kind::Space, 4, 2.09f, 6.88f, " ", " ");
        key (Kind::Action, 4, 8.97f, 2.53f + 0.125f);

        // Five suggestion keys across the suggestion band.
        if (! suggestionBand.isEmpty())
        {
            const auto band = suggestionBand.reduced (margin, suggestionBand.getHeight() * 0.1f);
            const float w = band.getWidth() / 5.0f;
            for (int i = 0; i < 5; ++i)
            {
                Key k { Kind::Suggestion, suggestions[i], {}, { band.getX() + (float) i * w, band.getY(), w - gx, band.getHeight() } };
                k.suggestion = i < suggestions.size() ? i : -1;
                keys.push_back (k);
            }
        }
    }

    int keyAt (juce::Point<float> p) const
    {
        for (int i = 0; i < (int) keys.size(); ++i)
            if (keys[(size_t) i].area.contains (p))
                return i;
        return -1;
    }

    /** Inserts `s` at the caret (as far as the length allows). */
    bool insert (const juce::String& s)
    {
        const int room = maxLength > 0 ? maxLength - text.length() : s.length();
        if (room <= 0 || s.isEmpty())
            return false;
        const auto add = s.substring (0, room);
        text = text.substring (0, caret) + add + text.substring (caret);
        caret += add.length();
        return true;
    }

    bool backspace()
    {
        if (caret <= 0)
            return false;
        text = text.substring (0, caret - 1) + text.substring (caret);
        --caret;
        return true;
    }

    /** The caret position nearest to x in the field. */
    int caretAt (float x) const
    {
        const auto font = textFont();
        int best = 0;
        float bestDistance = std::abs (x - textArea.getX());
        for (int i = 1; i <= text.length(); ++i)
        {
            const float cx = textArea.getX() + juce::GlyphArrangement::getStringWidth (font, text.substring (0, i));
            if (std::abs (x - cx) < bestDistance)
            {
                bestDistance = std::abs (x - cx);
                best = i;
            }
        }
        return best;
    }

    static void paintArrowUp (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour ink, bool underline)
    {
        const float s = juce::jmin (r.getWidth(), r.getHeight()) * 0.36f;
        const auto c = r.getCentre().translated (0.0f, underline ? -s * 0.2f : 0.0f);
        juce::Path p;
        p.addTriangle (c.x, c.y - s * 0.6f, c.x - s * 0.55f, c.y, c.x + s * 0.55f, c.y);
        p.addRectangle (c.x - s * 0.2f, c.y, s * 0.4f, s * 0.45f);
        g.setColour (ink);
        g.fillPath (p);
        if (underline)
            g.fillRect (c.x - s * 0.55f, c.y + s * 0.62f, s * 1.1f, juce::jmax (1.5f, s * 0.12f));
    }

    static void paintBackspace (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour ink)
    {
        const float h = juce::jmin (r.getHeight() * 0.36f, r.getWidth() * 0.3f), w = h * 1.5f;
        const auto b = juce::Rectangle<float> (w, h).withCentre (r.getCentre());
        juce::Path p;
        p.startNewSubPath (b.getX(), b.getCentreY());
        p.lineTo (b.getX() + h * 0.45f, b.getY());
        p.lineTo (b.getRight(), b.getY());
        p.lineTo (b.getRight(), b.getBottom());
        p.lineTo (b.getX() + h * 0.45f, b.getBottom());
        p.closeSubPath();
        const auto x = juce::Rectangle<float> (h * 0.42f, h * 0.42f).withCentre ({ b.getX() + h * 0.45f + (w - h * 0.45f) * 0.5f, b.getCentreY() });
        p.startNewSubPath (x.getTopLeft());
        p.lineTo (x.getBottomRight());
        p.startNewSubPath (x.getTopRight());
        p.lineTo (x.getBottomLeft());
        g.setColour (ink);
        g.strokePath (p, juce::PathStrokeType (juce::jmax (1.2f, h * 0.1f), juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
    }

    void paint (juce::Graphics& g, juce::Rectangle<float> bounds) const
    {
        g.setColour (juce::Colours::black);
        g.fillRect (bounds);
        g.setColour (Colours::screenPanel);
        for (auto band : { fieldBand, suggestionBand, keyBand })
            if (! band.isEmpty())
                g.fillRect (band);

        if (fieldVisible)
        {
            g.setColour (juce::Colours::black);
            g.fillRect (field);
            g.setColour (Colours::screenLine);
            g.drawRect (field, 1.0f);
            if (icon.has_value())
                drawIcon (g, *icon, juce::Rectangle<float> (field.getHeight() * 0.62f, field.getHeight() * 0.62f)
                                        .withCentre ({ field.getX() + field.getHeight() * 0.7f, field.getCentreY() }),
                          Colours::text);
            const auto font = textFont();
            g.setFont (font);
            g.setColour (Colours::text);
            g.drawText (text, textArea, juce::Justification::centredLeft, false);
            const float cx = juce::jmin (textArea.getRight(), textArea.getX() + juce::GlyphArrangement::getStringWidth (font, text.substring (0, caret)));
            const float ch = font.getHeight() * 0.95f;
            g.setColour (Colours::gold);
            g.fillRect (cx + 1.0f, textArea.getCentreY() - ch * 0.5f, 2.0f, ch);
            drawIcon (g, Icon::Cancel, clearKey, text.isEmpty() ? Colours::textDim : Colours::text);
        }

        const float keyCap = keyBand.getHeight() / 5.0f * 0.875f;
        for (int i = 0; i < (int) keys.size(); ++i)
        {
            const auto& k = keys[(size_t) i];
            const bool down = i == pressed;
            auto fill = down ? Colours::screenKey : Colours::screenWell;
            auto outline = Colours::screenLine;
            auto ink = Colours::text;
            if ((k.kind == Kind::Caps && caps) || (k.kind == Kind::Shift && shift))
            {
                fill = Colours::valueBlue;
                ink = juce::Colours::black;
            }
            if (k.kind == Kind::Action)
            {
                fill = down ? Colours::screenKey.brighter (0.1f) : Colours::screenKey;
                outline = Colours::caption.withAlpha (0.8f);
            }
            if (k.kind == Kind::Suggestion && k.suggestion < 0)
                fill = Colours::screenWell;
            paintKey (g, k.area, fill, outline);

            switch (k.kind)
            {
                case Kind::Char:
                {
                    const bool letter = isLetter (k.main);
                    if (letter)
                    {
                        g.setColour (ink);
                        g.setFont (wingFontPx (juce::jmin (screenHeight() * 0.03f, keyCap * 0.4f)));
                        g.drawText (upper() ? k.shifted : k.main, k.area, juce::Justification::centred, false);
                    }
                    else
                    {
                        // The shifted symbol small and grey above the main one.
                        auto top = k.area.withHeight (k.area.getHeight() * 0.38f).withTrimmedTop (k.area.getHeight() * 0.06f);
                        auto bottom = k.area.withTrimmedTop (k.area.getHeight() * 0.34f);
                        g.setColour (shift ? Colours::text : Colours::caption);
                        g.setFont (wingFontPx (keyCap * 0.17f));
                        g.drawText (k.shifted, top, juce::Justification::centred, false);
                        g.setColour (shift ? Colours::caption : ink);
                        g.setFont (wingFontPx (juce::jmin (screenHeight() * 0.03f, keyCap * 0.36f)));
                        g.drawText (k.main, bottom.withTrimmedBottom (k.area.getHeight() * 0.08f), juce::Justification::centred, false);
                    }
                    break;
                }
                case Kind::Backspace: paintBackspace (g, k.area, ink); break;
                case Kind::Caps:      paintArrowUp (g, k.area, ink, true); break;
                case Kind::Shift:     paintArrowUp (g, k.area, ink, false); break;
                case Kind::Space:     break;
                case Kind::Action:
                {
                    auto r = k.area.reduced (k.area.getHeight() * 0.18f, 0.0f);
                    const float s = k.area.getHeight() * 0.5f;
                    drawIcon (g, resolvedActionIcon(), r.removeFromLeft (s).withSizeKeepingCentre (s, s), Colours::caption);
                    g.setColour (Colours::caption);
                    g.setFont (wingFontPx (juce::jmin (TypeScale::body * screenHeight() * 1.1f, k.area.getHeight() * 0.3f)));
                    g.drawFittedText (actionText, r.toNearestInt(), juce::Justification::centred, 1, 0.8f);
                    break;
                }
                case Kind::Suggestion:
                    if (k.suggestion >= 0)
                    {
                        g.setColour (Colours::caption);
                        g.setFont (wingFontPx (juce::jmin (TypeScale::body * screenHeight() * 1.1f, k.area.getHeight() * 0.32f)));
                        g.drawFittedText (k.main, k.area.reduced (4.0f, 0.0f).toNearestInt(), juce::Justification::centred, 1, 0.8f);
                    }
                    break;
            }
        }
    }
};

WingKeyboard::WingKeyboard() : impl (std::make_unique<Impl>())
{
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (true);
}

WingKeyboard::~WingKeyboard() = default;

void WingKeyboard::setText (const juce::String& t)
{
    impl->text = impl->maxLength > 0 ? t.substring (0, impl->maxLength) : t;
    impl->caret = impl->text.length();
    repaint();
}

juce::String WingKeyboard::getText() const { return impl->text; }

void WingKeyboard::setActionText (const juce::String& t)
{
    impl->actionText = t;
    repaint();
}

void WingKeyboard::setIcon (std::optional<Icon> i)
{
    impl->icon = i;
    resized();
    repaint();
}

void WingKeyboard::setSuggestionRowAlwaysShown (bool shown)
{
    impl->suggestionRow = shown;
    resized();
    repaint();
}

void WingKeyboard::setSuggestions (const juce::StringArray& s)
{
    impl->suggestions = s;
    resized();
    repaint();
}

void WingKeyboard::setMaxLength (int n)
{
    impl->maxLength = n;
    setText (impl->text);
}

void WingKeyboard::setFieldVisible (bool v)
{
    impl->fieldVisible = v;
    resized();
    repaint();
}

void WingKeyboard::setActionIcon (std::optional<Icon> i)
{
    impl->actionIcon = i;
    repaint();
}

void WingKeyboard::paint (juce::Graphics& g) { impl->paint (g, getLocalBounds().toFloat()); }

void WingKeyboard::resized() { impl->layout (getLocalBounds().toFloat()); }

void WingKeyboard::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.position;
    if (impl->fieldVisible && impl->clearKey.expanded (4.0f).contains (p))
    {
        impl->text.clear();
        impl->caret = 0;
        repaint();
        if (onChange)
            onChange (impl->text);
        return;
    }
    if (impl->fieldVisible && impl->field.contains (p))
    {
        impl->caret = impl->caretAt (p.x);
        repaint();
        return;
    }
    impl->pressed = impl->keyAt (p);
    repaint();
}

void WingKeyboard::mouseExit (const juce::MouseEvent&)
{
    if (impl->pressed >= 0 && ! isMouseButtonDown())
    {
        impl->pressed = -1;
        repaint();
    }
}

void WingKeyboard::mouseUp (const juce::MouseEvent& e)
{
    const int index = impl->pressed;
    impl->pressed = -1;
    repaint();
    if (index < 0 || impl->keyAt (e.position) != index)
        return;

    const auto key = impl->keys[(size_t) index]; // a copy: callbacks may change the layout
    bool changed = false;
    switch (key.kind)
    {
        case Impl::Kind::Char:
        {
            // Caps lock capitalises letters; shift also gives the symbols.
            const bool shifted = Impl::isLetter (key.main) ? impl->upper() : impl->shift;
            changed = impl->insert (shifted ? key.shifted : key.main);
            impl->shift = false;
            break;
        }
        case Impl::Kind::Space:     changed = impl->insert (" "); impl->shift = false; break;
        case Impl::Kind::Backspace: changed = impl->backspace(); break;
        case Impl::Kind::Caps:      impl->caps = ! impl->caps; break;
        case Impl::Kind::Shift:     impl->shift = ! impl->shift; break;
        case Impl::Kind::Suggestion:
            if (key.suggestion >= 0)
            {
                setText (key.main);
                changed = true;
            }
            break;
        case Impl::Kind::Action:
            if (onAction)
                onAction (impl->text);
            return;
    }
    repaint();
    if (changed && onChange)
        onChange (impl->text);
}

bool WingKeyboard::keyPressed (const juce::KeyPress& k)
{
    bool changed = false;
    if (k == juce::KeyPress::escapeKey)
    {
        if (onCancel)
        {
            onCancel();
            return true;
        }
        return false;
    }
    if (k == juce::KeyPress::returnKey)
    {
        if (onAction)
            onAction (impl->text);
        return true;
    }
    if (k == juce::KeyPress::backspaceKey)
        changed = impl->backspace();
    else if (k == juce::KeyPress::deleteKey)
    {
        if (impl->caret < impl->text.length())
        {
            impl->text = impl->text.substring (0, impl->caret) + impl->text.substring (impl->caret + 1);
            changed = true;
        }
    }
    else if (k == juce::KeyPress::leftKey)
        impl->caret = juce::jmax (0, impl->caret - 1);
    else if (k == juce::KeyPress::rightKey)
        impl->caret = juce::jmin (impl->text.length(), impl->caret + 1);
    else if (k == juce::KeyPress::homeKey)
        impl->caret = 0;
    else if (k == juce::KeyPress::endKey)
        impl->caret = impl->text.length();
    else if (const auto c = k.getTextCharacter(); c >= ' ' && ! k.getModifiers().isCommandDown())
        changed = impl->insert (juce::String::charToString (c));
    else
        return false;

    repaint();
    if (changed && onChange)
        onChange (impl->text);
    return true;
}

//==============================================================================
namespace
{
    /** The CONTROL NAME window: a title bar with CLOSE, then the keyboard. */
    class TextWindow final : public Popup
    {
    public:
        TextWindow (juce::String titleText, const juce::String& initial, std::function<void (const juce::String&)> done, TextOptions opts)
            : title (std::move (titleText)), onDone (std::move (done)), onNext (opts.onNext), hasSuggestions (! opts.suggestions.isEmpty())
        {
            keyboard.setMaxLength (opts.maxLength);
            keyboard.setText (initial);
            keyboard.setIcon (opts.icon);
            keyboard.setSuggestions (opts.suggestions);
            keyboard.setActionText (onNext != nullptr ? juce::String ("NEXT") : opts.actionText);
            keyboard.onAction = [this] (const juce::String& t) { finish (t); };
            keyboard.onCancel = [this] { cancel(); };
            closeKey.onClick = [this] { cancel(); };
            addAndMakeVisible (keyboard);
            addAndMakeVisible (closeKey);
        }

        void paint (juce::Graphics& g) override
        {
            g.setColour (Colours::scrim);
            g.fillRect (getLocalBounds());
            const auto window = windowArea();
            g.setColour (juce::Colours::black);
            g.fillRect (window);
            paintWindowTitle (g, window, title);
        }

        void resized() override
        {
            auto w = windowArea();
            auto bar = w.removeFromTop (windowTitleHeight (w.getHeight()));
            closeKey.setBounds (bar.removeFromRight (juce::jmin (bar.getHeight() * 3, bar.getWidth() / 3)).reduced (4, 6));
            w.removeFromTop (juce::jmax (2, w.getHeight() / 120));
            keyboard.setBounds (w);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (! windowArea().contains (e.getPosition()))
                cancel();
        }

    private:
        /** Centred, about 59 % of the width (752 of 1280 px), as tall as the
            keyboard needs. */
        juce::Rectangle<int> windowArea() const
        {
            const auto b = getLocalBounds();
            const float screenH = screenHeight();
            const float units = 5.2f + 1.4f + (hasSuggestions ? 1.1f : 0.0f);
            float w = juce::jmin ((float) b.getWidth() * 0.62f, screenH * 0.94f);
            // Key pitch about w / 11.75; the keyboard is `units` pitches tall.
            auto heightFor = [&] (float width) { return screenH * 0.08f + width / 11.75f * units + 6.0f; };
            if (heightFor (w) > (float) b.getHeight() * 0.96f)
                w = ((float) b.getHeight() * 0.96f - screenH * 0.08f - 6.0f) * 11.75f / units;
            return b.withSizeKeepingCentre (juce::roundToInt (w), juce::roundToInt (heightFor (w)));
        }

        void finish (const juce::String& text)
        {
            closeThen ([done = onDone, next = onNext, text]
            {
                if (done != nullptr)
                    done (text);
                if (next != nullptr)
                    next();
            });
        }

        void takeFocus() override { keyboard.grabKeyboardFocus(); }

        juce::String title;
        std::function<void (const juce::String&)> onDone;
        std::function<void()> onNext;
        bool hasSuggestions;
        WingKeyboard keyboard;
        CloseKey closeKey;
    };

    //==========================================================================
    /** An anchored list (D10, D15): #101010 rows separated by black lines in a
        grey frame, the selected row gold with black text, unavailable rows dim,
        a scroll bar with ▲ ▼ when there are more than eight rows. */
    class ListPopup final : public Popup
    {
    public:
        ListPopup (juce::Component& anchorComponent, juce::StringArray itemTexts, int selectedIndex, std::function<void (int)> chosen, ListOptions opts)
            : anchor (&anchorComponent), items (std::move (itemTexts)), selected (selectedIndex), onChosen (std::move (chosen)), options (std::move (opts))
        {
            options.columns = juce::jlimit (1, 4, options.columns);
            anchor->getProperties().set ("wingListOpen", true);
            anchor->repaint();
        }

        void resized() override { place(); }

        void paint (juce::Graphics& g) override
        {
            g.setColour (juce::Colours::black);
            g.fillRect (list);
            if (! captionArea.isEmpty())
            {
                g.setColour (Colours::screenHeader);
                g.fillRect (captionArea.reduced (1, 1));
                g.setColour (Colours::caption);
                g.setFont (wingFontPx (juce::jmin (TypeScale::caption * screenHeight(), (float) captionArea.getHeight() * 0.4f)));
                g.drawFittedText (options.caption, captionArea.reduced (rowHeight / 3, 0), juce::Justification::centredLeft, 1, 0.8f);
            }
            const auto font = wingFontPx (juce::jmin (TypeScale::body * screenHeight() * 1.15f, (float) rowHeight * 0.32f));
            g.setFont (font);
            for (int c = 0; c < options.columns; ++c)
                for (int r = 0; r < visibleRows; ++r)
                {
                    const int index = indexAt (c, r + scroll);
                    auto row = rowArea (c, r);
                    if (index < 0)
                    {
                        g.setColour (Colours::screenWell);
                        g.fillRect (row);
                        continue;
                    }
                    const bool current = index == selected, hot = index == hover && ! current;
                    g.setColour (current ? Colours::gold : (hot ? Colours::screenKey : Colours::screenWell));
                    g.fillRect (row);
                    g.setColour (current ? juce::Colours::black : (isDisabled (index) ? Colours::chipOff : Colours::text));
                    g.drawFittedText (items[index], row.reduced (rowHeight / 4, 0), juce::Justification::centredLeft, 1, 0.8f);
                }
            if (! scrollBar.isEmpty())
            {
                g.setColour (Colours::screenWell);
                g.fillRect (scrollBar);
                const auto up = scrollBar.withHeight (scrollBar.getWidth()), down = scrollBar.withTop (scrollBar.getBottom() - scrollBar.getWidth());
                auto arrow = [&g] (juce::Rectangle<int> r, bool pointsUp)
                {
                    const auto a = r.toFloat().reduced ((float) r.getWidth() * 0.22f);
                    juce::Path p;
                    if (pointsUp)
                        p.addTriangle (a.getX(), a.getBottom() - a.getHeight() * 0.2f, a.getRight(), a.getBottom() - a.getHeight() * 0.2f, a.getCentreX(), a.getY() + a.getHeight() * 0.2f);
                    else
                        p.addTriangle (a.getX(), a.getY() + a.getHeight() * 0.2f, a.getRight(), a.getY() + a.getHeight() * 0.2f, a.getCentreX(), a.getBottom() - a.getHeight() * 0.2f);
                    g.setColour (Colours::caption);
                    g.fillPath (p);
                };
                arrow (up, true);
                arrow (down, false);
                auto track = scrollBar.withTrimmedTop (up.getHeight() + 2).withTrimmedBottom (down.getHeight() + 2).reduced (scrollBar.getWidth() / 5, 0);
                const int maxScroll = juce::jmax (1, numRows - visibleRows);
                const int thumbH = juce::jmax (8, track.getHeight() * visibleRows / juce::jmax (1, numRows));
                const int thumbY = track.getY() + (track.getHeight() - thumbH) * scroll / maxScroll;
                g.setColour (Colours::textDim);
                g.fillRect (track.withY (thumbY).withHeight (thumbH));
            }
            g.setColour (Colours::textDim);
            g.drawRect (list, 1);
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            const int h = hit (e.getPosition());
            if (h != hover)
            {
                hover = h;
                repaint (list);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            const auto p = e.getPosition();
            if (! list.contains (p))
            {
                cancel();
                return;
            }
            if (scrollBar.contains (p))
            {
                scrollBy (p.y < scrollBar.getCentreY() ? -1 : 1);
                return;
            }
            const int index = hit (p);
            if (index >= 0 && ! isDisabled (index))
                choose (index);
        }

        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
        {
            scrollBy (wheel.deltaY > 0 ? -1 : 1);
        }

        bool keyPressed (const juce::KeyPress& k) override
        {
            if (k == juce::KeyPress::upKey || k == juce::KeyPress::downKey)
            {
                const int step = k == juce::KeyPress::upKey ? -1 : 1;
                int next = (hover >= 0 ? hover : selected);
                for (int i = 0; i < items.size(); ++i)
                {
                    next = juce::jlimit (0, items.size() - 1, next + step);
                    if (! isDisabled (next))
                        break;
                }
                hover = next;
                reveal (hover);
                repaint();
                return true;
            }
            if (k == juce::KeyPress::returnKey)
            {
                const int index = hover >= 0 ? hover : selected;
                if (juce::isPositiveAndBelow (index, items.size()) && ! isDisabled (index))
                    choose (index);
                return true;
            }
            return Popup::keyPressed (k);
        }

    private:
        void onClosing() override
        {
            if (anchor != nullptr)
            {
                anchor->getProperties().remove ("wingListOpen");
                anchor->repaint();
            }
        }

        void choose (int index)
        {
            closeThen ([chosen = onChosen, index] { if (chosen != nullptr) chosen (index); });
        }

        bool isDisabled (int index) const
        {
            return std::find (options.disabled.begin(), options.disabled.end(), index) != options.disabled.end();
        }

        int indexAt (int column, int row) const
        {
            const int index = column * numRows + row;
            return row < numRows && index < items.size() ? index : -1;
        }

        juce::Rectangle<int> rowArea (int column, int row) const
        {
            return { body.getX() + column * columnWidth, body.getY() + row * rowHeight, columnWidth - (column < options.columns - 1 ? 2 : 0), rowHeight - 2 };
        }

        int hit (juce::Point<int> p) const
        {
            if (! body.contains (p) || scrollBar.contains (p))
                return -1;
            const int column = (p.x - body.getX()) / juce::jmax (1, columnWidth);
            const int row = (p.y - body.getY()) / juce::jmax (1, rowHeight);
            return column < options.columns && row < visibleRows ? indexAt (column, row + scroll) : -1;
        }

        void scrollBy (int rows)
        {
            const int s = juce::jlimit (0, juce::jmax (0, numRows - visibleRows), scroll + rows);
            if (s != scroll)
            {
                scroll = s;
                repaint (list);
            }
        }

        void reveal (int index)
        {
            if (index < 0)
                return;
            const int row = index % juce::jmax (1, numRows);
            if (row < scroll)
                scroll = row;
            else if (row >= scroll + visibleRows)
                scroll = row - visibleRows + 1;
        }

        /** Sizes the list and puts it under the anchor (beside it, or above, when
            there's no room below). */
        void place()
        {
            const auto bounds = getLocalBounds();
            const float screenH = screenHeight();
            rowHeight = juce::jmax (18, juce::roundToInt (screenH * 0.075f));
            const int captionH = options.caption.isNotEmpty() ? juce::roundToInt ((float) rowHeight * 0.75f) : 0;
            numRows = (items.size() + options.columns - 1) / options.columns;
            const int maxRows = juce::jmax (2, juce::jmin (8, (bounds.getHeight() - captionH - 4) / rowHeight));
            visibleRows = juce::jlimit (1, maxRows, numRows);
            const bool scrolls = numRows > visibleRows;

            const auto font = wingFontPx (juce::jmin (TypeScale::body * screenH * 1.15f, (float) rowHeight * 0.32f));
            float textW = 0.0f;
            for (auto& s : items)
                textW = juce::jmax (textW, juce::GlyphArrangement::getStringWidth (font, s));
            const auto anchorArea = anchor != nullptr ? getLocalArea (anchor, anchor->getLocalBounds()) : bounds.withSizeKeepingCentre (200, 40);
            const int scrollW = scrolls ? juce::roundToInt ((float) rowHeight * 0.6f) : 0;
            columnWidth = juce::jmax (juce::roundToInt (textW) + rowHeight, rowHeight * 2);
            if (options.columns == 1)
                columnWidth = juce::jmax (columnWidth, anchorArea.getWidth() - scrollW);
            columnWidth = juce::jmin (columnWidth, (bounds.getWidth() - 8 - scrollW) / options.columns);
            const int w = columnWidth * options.columns + scrollW + 2;
            const int h = captionH + visibleRows * rowHeight + 2;

            juce::Rectangle<int> r;
            if (anchorArea.getBottom() + 2 + h <= bounds.getBottom())
                r = { anchorArea.getX(), anchorArea.getBottom() + 2, w, h };
            else if (anchorArea.getRight() + 2 + w <= bounds.getRight() && anchorArea.getY() + h <= bounds.getBottom())
                r = { anchorArea.getRight() + 2, anchorArea.getY(), w, h };
            else if (anchorArea.getY() - 2 - h >= bounds.getY())
                r = { anchorArea.getX(), anchorArea.getY() - 2 - h, w, h };
            else
                r = { anchorArea.getRight() + 2, bounds.getBottom() - h, w, h };
            list = r.constrainedWithin (bounds);
            auto inner = list.reduced (1);
            captionArea = inner.removeFromTop (captionH);
            scrollBar = scrolls ? inner.removeFromRight (scrollW) : juce::Rectangle<int>();
            body = inner;
            reveal (selected);
        }

        juce::Component::SafePointer<juce::Component> anchor;
        juce::StringArray items;
        int selected;
        std::function<void (int)> onChosen;
        ListOptions options;
        juce::Rectangle<int> list, captionArea, body, scrollBar;
        int rowHeight = 30, columnWidth = 100, numRows = 0, visibleRows = 0, scroll = 0, hover = -1;
    };

    //==========================================================================
    /** A confirmation or notice: a centred window with a big message and
        CANCEL / OK at the bottom right; title bar and OK key red when the action
        destroys something ("snap restore dialog in red color"). */
    class MessageWindow final : public Popup
    {
    public:
        MessageWindow (juce::String titleText, juce::String messageText, juce::String okKeyText, std::function<void()> ok, bool isDanger, bool withCancel)
            : title (std::move (titleText)), message (std::move (messageText)), okText (std::move (okKeyText)), onOk (std::move (ok)),
              danger (isDanger), cancellable (withCancel)
        {
        }

        void paint (juce::Graphics& g) override
        {
            g.setColour (Colours::scrim);
            g.fillRect (getLocalBounds());
            const auto window = windowArea();
            g.setColour (juce::Colours::black);
            g.fillRect (window);
            auto body = paintWindowTitle (g, window, title, false, danger ? Colours::pfl : Colours::screenHeader,
                                          danger ? juce::Colours::white : Colours::caption);
            body.removeFromTop (4);
            g.setColour (Colours::screenPanel);
            g.fillRect (body);

            auto text = body.reduced (body.getWidth() / 14, 0).withTrimmedBottom (keyArea (body).getHeight() + body.getHeight() / 10);
            g.setColour (Colours::text);
            g.setFont (wingFontPx (juce::jmin (TypeScale::value * screenHeight(), (float) text.getHeight() * 0.16f)));
            g.drawFittedText (message, text, juce::Justification::centred, 5, 0.85f);

            layoutKeys (body);
            auto key = [&g] (juce::Rectangle<int> r, const juce::String& t, juce::Colour fill, juce::Colour ink, bool down)
            {
                if (r.isEmpty())
                    return;
                paintKey (g, r.toFloat(), down ? fill.brighter (0.15f) : fill, juce::Colours::black);
                g.setColour (ink);
                g.setFont (wingFontPx (juce::jmin (TypeScale::body * screenHeight() * 1.15f, (float) r.getHeight() * 0.32f)));
                g.drawFittedText (t, r.reduced (4, 0), juce::Justification::centred, 1, 0.8f);
            };
            key (cancelKey, "CANCEL", Colours::screenKey, Colours::caption, pressed == 1);
            key (okKey, okText, danger ? Colours::pfl : Colours::gold, danger ? juce::Colours::white : juce::Colours::black, pressed == 2);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            const auto p = e.getPosition();
            pressed = okKey.contains (p) ? 2 : (cancelKey.contains (p) ? 1 : 0);
            repaint();
            if (! windowArea().contains (p) && ! danger)
                cancel();
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            const auto p = e.getPosition();
            const int was = pressed;
            pressed = 0;
            repaint();
            if (was == 2 && okKey.contains (p))
                accept();
            else if (was == 1 && cancelKey.contains (p))
                cancel();
        }

        bool keyPressed (const juce::KeyPress& k) override
        {
            if (k == juce::KeyPress::returnKey)
            {
                accept();
                return true;
            }
            return Popup::keyPressed (k);
        }

    private:
        juce::Rectangle<int> windowArea() const
        {
            const auto b = getLocalBounds();
            return b.withSizeKeepingCentre (juce::jmax (260, b.getWidth() / 2), juce::jmax (150, juce::roundToInt ((float) b.getHeight() * 0.4f)));
        }

        juce::Rectangle<int> keyArea (juce::Rectangle<int> body) const
        {
            return body.removeFromBottom (juce::jmax (24, juce::roundToInt (screenHeight() * 0.08f))).reduced (8, 0);
        }

        void layoutKeys (juce::Rectangle<int> body)
        {
            auto keys = keyArea (body.withTrimmedBottom (8));
            const int w = juce::jmin (keys.getWidth() / 3, juce::roundToInt (screenHeight() * 0.22f));
            okKey = keys.removeFromRight (w);
            keys.removeFromRight (8);
            cancelKey = cancellable ? keys.removeFromRight (w) : juce::Rectangle<int>();
        }

        void accept()
        {
            closeThen ([ok = onOk] { if (ok != nullptr) ok(); });
        }

        juce::String title, message, okText;
        std::function<void()> onOk;
        bool danger, cancellable;
        juce::Rectangle<int> okKey, cancelKey;
        int pressed = 0;
    };
} // namespace

//==============================================================================
void editText (juce::Component& host, const juce::String& title, const juce::String& initial,
               std::function<void (const juce::String&)> onDone, TextOptions options)
{
    auto* layer = layerFor (host);
    if (layer == nullptr)
        return;
    (new TextWindow (title, initial, std::move (onDone), std::move (options)))->open (*layer);
}

void chooseFrom (juce::Component& anchor, const juce::StringArray& items, int selected,
                 std::function<void (int)> onChosen, ListOptions options)
{
    auto* layer = layerFor (anchor);
    if (layer == nullptr || items.isEmpty())
        return;
    (new ListPopup (anchor, items, selected, std::move (onChosen), std::move (options)))->open (*layer);
}

void confirm (juce::Component& host, const juce::String& title, const juce::String& text,
              const juce::String& okText, std::function<void()> onOk, bool danger)
{
    auto* layer = layerFor (host);
    if (layer == nullptr)
        return;
    (new MessageWindow (title, text, okText, std::move (onOk), danger, true))->open (*layer);
}

void warn (juce::Component& host, const juce::String& title, const juce::String& text)
{
    auto* layer = layerFor (host);
    if (layer == nullptr)
        return;
    (new MessageWindow (title, text, "OK", nullptr, false, false))->open (*layer);
}

} // namespace livedaw::ui::popups
