#include "ui/WingPopups.h"

#include "ui/LookAndFeel.h"
#include "ui/ScreenWidgets.h"

#include <map>

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

    /** WING 3.1 (EQ / DYNAMICS MODEL, SELECT CHANNEL): only the screen the
        anchor is on dims; the block column left of it stays lit and the
        parameter bar under it goes blank. */
    void paintScreenScrim (juce::Graphics& g, juce::Component& popup, juce::Component* anchor)
    {
        auto dimmed = popup.getLocalBounds();
        if (auto* screen = anchor != nullptr ? anchor->getParentComponent() : nullptr)
        {
            const auto s = popup.getLocalArea (screen, screen->getLocalBounds());
            dimmed = dimmed.withLeft (juce::jlimit (0, popup.getWidth(), s.getX()));
            g.setColour (juce::Colours::black);
            g.fillRect (popup.getLocalBounds().withTop (juce::jlimit (0, popup.getHeight(), s.getBottom())));
            dimmed = dimmed.withBottom (juce::jlimit (0, popup.getHeight(), s.getBottom()));
        }
        g.setColour (Colours::scrim);
        g.fillRect (dimmed);
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
            focusBefore = juce::Component::getCurrentlyFocusedComponent();
            layer.addAndMakeVisible (this);
            setBounds (layer.getLocalBounds());
            toFront (false);
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<Popup> (this)]
            {
                if (safe != nullptr && safe->isShowing())
                    safe->takeFocus();
            });
            // The window may not have the system's focus yet (a pop-up opened
            // at start-up): keep asking for a moment, until the pop-up has it.
            focusRetry.startTimer (150);
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
            giveFocusBack();
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<Popup> (this), after = std::move (after)]
            {
                delete safe.getComponent();
                if (after != nullptr)
                    after();
            });
        }

        virtual void onClosing() {}
        virtual void takeFocus() { grabKeyboardFocus(); }

        // Pop-ups cover their layer, also when it changes size.
        void parentSizeChanged() override
        {
            if (auto* parent = getParentComponent())
                setBounds (parent->getLocalBounds());
        }

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

    private:
        /** The keyboard focus goes back where it was (or to the nearest
            component around the pop-up that takes it), so the app's
            shortcuts keep working after the pop-up closes. */
        void giveFocusBack()
        {
            // (Not the window or another component around the pop-up: the
            // nearest one that takes the focus is closer to what was in use.)
            if (auto* before = focusBefore.getComponent(); before != nullptr && before != this && ! before->isParentOf (this) && before->isShowing())
            {
                before->grabKeyboardFocus();
                return;
            }
            for (auto* c = getParentComponent(); c != nullptr; c = c->getParentComponent())
                if (c->getWantsKeyboardFocus() && c->isShowing())
                {
                    c->grabKeyboardFocus();
                    return;
                }
        }

        juce::Component::SafePointer<juce::Component> focusBefore;
        int focusTries = 0;
        juce::TimedCallback focusRetry { [this]
        {
            if (closing || hasKeyboardFocus (true))
                focusRetry.stopTimer();
            else if (auto* peer = getPeer(); peer == nullptr || ! peer->isFocused())
                return; // until the window has the system's focus
            else if (++focusTries > 10)
                focusRetry.stopTimer();
            else if (isShowing())
                takeFocus();
        } };
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
        // WING 3.1 pixels of a 64 px key pitch (NAME): the field band 88 tall,
        // the suggestions 72, the keys 328 (five rows), 4 px apart.
        const bool showSuggestions = suggestionRow || ! suggestions.isEmpty();
        const float units = 5.125f + (fieldVisible ? 1.4375f : 0.0f) + (showSuggestions ? 1.1875f : 0.0f);
        const float u = r.getHeight() / units;
        const float gapY = u * 0.0625f;

        fieldBand = suggestionBand = {};
        if (fieldVisible)
        {
            fieldBand = r.removeFromTop (u * 1.375f);
            r.removeFromTop (gapY);
        }
        if (showSuggestions)
        {
            suggestionBand = r.removeFromTop (u * 1.125f);
            r.removeFromTop (gapY);
        }
        keyBand = r;

        // The field: black, 70 % of the width, the clear key at its right.
        field = fieldBand.withSizeKeepingCentre (fieldBand.getWidth() * 0.7f, fieldBand.getHeight() * 0.8f);
        clearKey = field.withLeft (field.getRight() - field.getHeight()).reduced (field.getHeight() * 0.2f);
        textArea = field.withTrimmedRight (field.getHeight()).withTrimmedLeft (field.getHeight() * 0.25f);
        if (icon.has_value())
            textArea.removeFromLeft (field.getHeight() * 0.9f);

        // Keys on a grid of 11.5 key pitches across, 8 px gaps of 64 px
        // pitches, 8 px in from the band's left and top (WING 3.1).
        const float margin = keyBand.getWidth() * 8.0f / 752.0f;
        const auto inner = keyBand.withTrimmedLeft (margin).withTrimmedTop (margin);
        const float px = (inner.getWidth() - margin) / 11.5f, py = inner.getHeight() / 5.0f;
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
            // 134 px keys at a pitch of 150, 8 px in (56 of the band's 72 px).
            const auto band = suggestionBand.reduced (margin, margin);
            const float w = (band.getWidth() + 2.0f * gx) / 5.0f;
            for (int i = 0; i < 5; ++i)
            {
                Key k { Kind::Suggestion, suggestions[i], {}, { band.getX() + (float) i * w, band.getY(), w - 2.0f * gx, band.getHeight() } };
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
            // WING 3.1: the keys in a 2 px outline, their letters light grey.
            g.setColour (outline);
            g.fillRect (k.area);
            g.setColour (fill);
            g.fillRect (k.area.reduced (juce::jmax (1.0f, wingPx (2.0f))));
            if (ink == Colours::text)
                ink = Colours::caption;

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

int WingKeyboard::getCaretPosition() const { return impl->caret; }

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

void WingKeyboard::setCapsLock (bool on)
{
    impl->caps = on;
    repaint();
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
            : title (std::move (titleText)), onDone (std::move (done)), onNext (opts.onNext), hasSuggestions (! opts.suggestions.isEmpty()),
              live (opts.onChange != nullptr), centreX (opts.centreX)
        {
            keyboard.setCapsLock (opts.capsLock);
            keyboard.setMaxLength (opts.maxLength);
            keyboard.setText (initial);
            keyboard.setIcon (opts.icon);
            keyboard.setSuggestions (opts.suggestions);
            keyboard.setActionText (onNext != nullptr ? juce::String ("NEXT") : opts.actionText);
            keyboard.onAction = [this] (const juce::String& t) { finish (t); };
            keyboard.onCancel = [this] { cancel(); };
            closeKey.onClick = [this] { if (live) finish (keyboard.getText()); else cancel(); };
            keyboard.onChange = opts.onChange;
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
            const auto r = b.withSizeKeepingCentre (juce::roundToInt (w), juce::roundToInt (heightFor (w)));
            const int x = juce::roundToInt ((float) b.getWidth() * centreX - w / 2.0f);
            return r.withX (b.getX() + juce::jlimit (0, juce::jmax (0, b.getWidth() - r.getWidth()), x));
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
        bool hasSuggestions, live;
        float centreX;
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
            // WING 3.1 (a 60 px row pitch): rows #101010 behind 2 px black lines,
            // the chosen one gold 2 px inside its row, the names in 14 px light
            // grey capitals 8 px in, all in a 2 px #707070 frame.
            const float k = screenHeight() / 800.0f;
            const int inset = juce::jmax (1, juce::roundToInt (2.0f * k));
            g.setFont (listFont());
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
                    g.setColour (hot ? Colours::screenKey : Colours::screenWell);
                    g.fillRect (row);
                    if (current)
                    {
                        g.setColour (Colours::gold);
                        g.fillRect (row.reduced (inset));
                    }
                    g.setColour (current ? juce::Colours::black : (isDisabled (index) ? Colours::chipOff : juce::Colour (0xffc0c0c0)));
                    g.drawFittedText (items[index], row.withTrimmedLeft (juce::roundToInt (8.0f * k)).withTrimmedRight (inset * 2), juce::Justification::centredLeft, 1, 0.8f);
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
            g.setColour (juce::Colour (0xff707070));
            g.drawRect (list, frameWidth());
        }

        int frameWidth() const { return juce::jmax (1, juce::roundToInt (2.0f * screenHeight() / 800.0f)); }
        juce::Font listFont() const { return wingFontPx (juce::jmin (TypeScale::body * screenHeight(), (float) rowHeight * 0.3f)); }

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
            const int cell = column * numRows + row;
            return row < numRows && cell < (int) cells.size() ? cells[(size_t) cell] : -1;
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
            const auto cell = std::find (cells.begin(), cells.end(), index);
            if (index < 0 || cell == cells.end())
                return;
            const int row = (int) (cell - cells.begin()) % juce::jmax (1, numRows);
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
            cells.clear();
            for (int i = 0; i < items.size(); ++i)
            {
                if (std::find (options.gapsBefore.begin(), options.gapsBefore.end(), i) != options.gapsBefore.end())
                    cells.push_back (-1);
                cells.push_back (i);
            }
            numRows = ((int) cells.size() + options.columns - 1) / options.columns;
            const int maxRows = juce::jmax (2, juce::jmin (options.maxRows, (bounds.getHeight() - captionH - 4) / rowHeight));
            visibleRows = juce::jlimit (1, maxRows, numRows);
            const bool scrolls = numRows > visibleRows;

            const auto font = listFont();
            float textW = 0.0f;
            for (auto& s : items)
                textW = juce::jmax (textW, juce::GlyphArrangement::getStringWidth (font, s));
            const auto anchorArea = anchor != nullptr ? getLocalArea (anchor, anchor->getLocalBounds()) : bounds.withSizeKeepingCentre (200, 40);
            const int scrollW = scrolls ? juce::roundToInt ((float) rowHeight * 0.6f) : 0;
            columnWidth = juce::jmax (juce::roundToInt (textW) + rowHeight, rowHeight * 2);
            if (options.columns == 1) // as wide as the field, frame and all [3.1]
                columnWidth = juce::jmax (columnWidth, anchorArea.getWidth() - scrollW - 2 * frameWidth());
            columnWidth = juce::jmin (columnWidth, (bounds.getWidth() - 8 - scrollW) / options.columns);
            const int frame = frameWidth();
            const int w = columnWidth * options.columns + scrollW + frame * 2;
            const int h = captionH + visibleRows * rowHeight + frame * 2;

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
            auto inner = list.reduced (frame);
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
        std::vector<int> cells; // the item in each cell, column by column (-1: a gap)
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
    //==========================================================================
    /** The model list of a dynamics or EQ slot (firmware 3.1): a panel of
        172 x 92 tiles 8 px apart (WING pixels) under the model field, each the
        model's picture over its name; the rows after a spaced row (the
        compressors) on a lighter panel; the loaded model framed gold. Touch a
        tile to load it. */
    class TilePopup final : public Popup
    {
    public:
        TilePopup (juce::Component& anchorComponent, std::vector<std::vector<Tile>> tileRows, std::vector<int> spacedRows, int selectedId,
                   std::function<void (juce::Graphics&, juce::Rectangle<float>, int)> paintPicture, std::function<void (int)> chosen)
            : anchor (&anchorComponent), rows (std::move (tileRows)), spaced (std::move (spacedRows)), selected (selectedId),
              picture (std::move (paintPicture)), onChosen (std::move (chosen))
        {
            anchor->getProperties().set ("wingListOpen", true);
            anchor->repaint();
        }

        void paint (juce::Graphics& g) override
        {
            paintScreenScrim (g, *this, anchor);
            const auto f = frame();
            g.setColour (juce::Colour (0xff202020));
            g.fillRect (f);
            const float u = unit();
            const auto tiles = layout();
            if (! spaced.empty() && (size_t) spaced.front() < rows.size())
            {
                // The second section (the compressors) on #303030 from 8 px above its first row.
                size_t first = 0;
                for (size_t i = 0; i < (size_t) spaced.front(); ++i)
                    first += rows[i].size();
                if (first < tiles.size())
                {
                    g.setColour (juce::Colour (0xff303030));
                    g.fillRect (f.withTop (tiles[first].first.getY() - 8.0f * u));
                }
            }
            for (auto& [area, tile] : tiles)
            {
                if (tile.id < 0)
                    continue;
                const bool sel = tile.id == selected;
                const bool down = tile.id == pressed;
                g.setColour (down ? juce::Colour (0xff303030) : juce::Colours::black);
                g.fillRect (area);
                // The picture 156 x 48 at 8 px in, the name under it.
                const auto pictureArea = juce::Rectangle<float> (area.getX() + 8.0f * u, area.getY() + 8.0f * u, area.getWidth() - 16.0f * u, 48.0f * u);
                if (picture)
                    g.drawImage (pictureOf (tile.id, pictureArea, g.getInternalContext().getPhysicalPixelScaleFactor()), pictureArea);
                // (The name's middle 2 px over the middle of the strip under the picture, 3.1.)
                auto name = area.withTop (pictureArea.getBottom()).withTrimmedBottom (3.0f * u).reduced (4.0f * u, 0.0f);
                g.setColour (Colours::caption); // 13 px light grey capitals (3.1)
                g.setFont (wingFontPx (juce::jmin (13.0f * screenHeight() / 800.0f, name.getHeight() * 0.5f)));
                g.drawFittedText (tile.caption, name.toNearestInt(), juce::Justification::centred, 1, 0.7f);
                if (sel)
                {
                    g.setColour (Colours::gold);
                    g.drawRect (area, juce::jmax (2.0f, 4.0f * u));
                }
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            pressed = tileAt (e.position);
            repaint();
            if (! frame().contains (e.position))
                cancel();
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            const int was = pressed;
            pressed = -1;
            repaint();
            if (was >= 0 && tileAt (e.position) == was)
                closeThen ([chosen = onChosen, was] { if (chosen != nullptr) chosen (was); });
        }

    private:
        void onClosing() override
        {
            if (anchor != nullptr)
            {
                anchor->getProperties().set ("wingListOpen", false);
                anchor->repaint();
            }
        }

        /** A tile's picture, drawn once at its size: the page under the list
            keeps repainting (meters, graphs), and drawing every front panel
            each time would take the message thread's whole time. */
        const juce::Image& pictureOf (int id, juce::Rectangle<float> area, float scale)
        {
            auto& image = pictures[id];
            const int w = juce::jmax (1, juce::roundToInt (area.getWidth() * scale)), h = juce::jmax (1, juce::roundToInt (area.getHeight() * scale));
            if (! image.isValid() || image.getWidth() != w || image.getHeight() != h)
            {
                image = juce::Image (juce::Image::ARGB, w, h, true);
                juce::Graphics ig (image);
                ig.addTransform (juce::AffineTransform::scale ((float) w / area.getWidth(), (float) h / area.getHeight()));
                picture (ig, { area.getWidth(), area.getHeight() }, id);
            }
            return image;
        }

        /** One WING pixel, smaller when the grid would not fit the page. */
        float unit() const
        {
            size_t columns = 1;
            for (auto& row : rows)
                columns = juce::jmax (columns, row.size());
            const float w = 8.0f + 180.0f * (float) columns;
            const float h = 8.0f + 100.0f * (float) rows.size() + 8.0f * (float) spaced.size();
            return juce::jmin (screenHeight() / 800.0f, (float) getWidth() / (w + 16.0f), (float) getHeight() / (h + 16.0f));
        }

        /** The panel: under the anchor (16 px down, from its left edge), kept
            48 px from the screen's right edge and 24 px from its bottom. */
        juce::Rectangle<float> frame() const
        {
            const float u = unit();
            size_t columns = 1;
            for (auto& row : rows)
                columns = juce::jmax (columns, row.size());
            const float w = (8.0f + 180.0f * (float) columns) * u;
            const float h = (8.0f + 100.0f * (float) rows.size() + 8.0f * (float) spaced.size()) * u;
            auto a = anchor != nullptr ? getLocalArea (anchor, anchor->getLocalBounds()).toFloat() : juce::Rectangle<float> (8.0f * u, 0.0f, 0.0f, 0.0f);
            const float x = juce::jlimit (8.0f * u, juce::jmax (8.0f * u, (float) getWidth() - 48.0f * u - w), a.getX());
            const float y = juce::jlimit (8.0f * u, juce::jmax (8.0f * u, (float) getHeight() - 24.0f * u - h), a.getBottom() + 16.0f * u);
            return { x, y, w, h };
        }

        std::vector<std::pair<juce::Rectangle<float>, Tile>> layout() const
        {
            std::vector<std::pair<juce::Rectangle<float>, Tile>> out;
            const float u = unit();
            const auto f = frame();
            float y = f.getY() + 8.0f * u;
            for (size_t i = 0; i < rows.size(); ++i)
            {
                if (std::find (spaced.begin(), spaced.end(), (int) i) != spaced.end())
                    y += 8.0f * u;
                for (size_t c = 0; c < rows[i].size(); ++c)
                    out.push_back ({ juce::Rectangle<float> (f.getX() + (8.0f + 180.0f * (float) c) * u, y, 172.0f * u, 92.0f * u), rows[i][c] });
                y += 100.0f * u;
            }
            return out;
        }

        int tileAt (juce::Point<float> p) const
        {
            for (auto& [area, tile] : layout())
                if (tile.id >= 0 && area.contains (p))
                    return tile.id;
            return -1;
        }

        juce::Component::SafePointer<juce::Component> anchor;
        std::vector<std::vector<Tile>> rows;
        std::vector<int> spaced;
        int selected, pressed = -1;
        std::function<void (juce::Graphics&, juce::Rectangle<float>, int)> picture;
        std::map<int, juce::Image> pictures;
        std::function<void (int)> onChosen;
    };

    //==========================================================================
    /** SELECT CHANNEL (KEY SOURCE, firmware 3.1): a header with the none key
        (SELF, gold while chosen), the title and CLOSE, over a frame of
        channel tiles eight to a row, each the channel's colour over its
        number and icon. In WING pixels: 876 wide at the right of the screen,
        from 4 px above the anchor; 96 x 64 tiles 108 x 76 apart. */
    class ChannelPopup final : public Popup
    {
    public:
        ChannelPopup (juce::Component& anchorComponent, juce::String titleText, juce::String noneKey, std::vector<ChannelTile> channelTiles,
                      int selectedIndex, std::function<void (int)> chosen)
            : anchor (&anchorComponent), title (std::move (titleText)), noneText (std::move (noneKey)), tiles (std::move (channelTiles)),
              selected (selectedIndex), onChosen (std::move (chosen))
        {
            anchor->getProperties().set ("wingListOpen", true);
            anchor->repaint();
        }

        void paint (juce::Graphics& g) override
        {
            paintScreenScrim (g, *this, anchor);
            const float u = unit();
            const auto header = headerArea();
            g.setColour (juce::Colour (0xff202020));
            g.fillRect (header);
            const auto none = noneKey();
            g.setColour (selected < 0 ? Colours::gold : juce::Colour (0xff303030));
            g.fillRect (none);
            g.setColour (selected < 0 ? juce::Colours::black : Colours::text);
            g.setFont (wingFontPx (14.0f * u));
            g.drawText (noneText, none, juce::Justification::centred, false);
            g.setColour (Colours::text);
            // (3.1: the title in 20 px capitals, CLOSE in 15.5 px ones.)
            g.setFont (wingFontPx (20.0f * u));
            g.drawText (title, header, juce::Justification::centred, false);
            auto close = closeKey();
            g.setFont (wingFontPx (15.5f * u));
            g.drawText ("CLOSE", close.withTrimmedRight (40.0f * u), juce::Justification::centredRight, false);
            drawIcon (g, Icon::Check, close.removeFromRight (34.0f * u).withSizeKeepingCentre (34.0f * u, 34.0f * u), Colours::text);

            const auto grid = gridArea();
            g.setColour (juce::Colour (0xff202020));
            g.fillRect (grid);
            g.setColour (juce::Colours::black);
            g.fillRect (grid.reduced (4.0f * u));
            for (int i = 0; i < (int) tiles.size(); ++i)
            {
                const auto r = tileArea (i);
                const auto& t = tiles[(size_t) i];
                g.setColour (t.colour);
                g.fillRect (r.withHeight (22.0f * u));
                auto body = r.withTrimmedTop (22.0f * u);
                g.setColour (i == pressed ? juce::Colour (0xff484848) : juce::Colour (0xff303030));
                g.fillRect (body);
                // The number dark on a grey badge at the left, the icon at the right.
                const auto font = wingFontPx (22.0f * u, WingWeight::SemiBold);
                const float badgeW = juce::GlyphArrangement::getStringWidth (font, t.number) + 6.0f * u;
                const auto badge = juce::Rectangle<float> (body.getX() + 2.0f * u, body.getY() + 6.0f * u, badgeW, body.getHeight() - 10.0f * u);
                g.setColour (juce::Colour (0xff707070));
                g.fillRect (badge);
                g.setColour (juce::Colour (0xff202020));
                g.setFont (font);
                g.drawText (t.number, badge, juce::Justification::centred, false);
                if (t.paintIcon != nullptr)
                    t.paintIcon (g, body.withTrimmedLeft (body.getWidth() - 36.0f * u).reduced (5.0f * u));
                if (i == selected)
                {
                    g.setColour (Colours::gold);
                    g.drawRect (r, 3.0f * u);
                }
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            pressed = tileAt (e.position);
            repaint();
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            const int was = pressed;
            pressed = -1;
            repaint();
            if (was >= 0 && tileAt (e.position) == was)
                choose (was);
            else if (noneKey().contains (e.position))
                choose (-1);
            else if (closeKey().contains (e.position) || ! (headerArea().contains (e.position) || gridArea().contains (e.position)))
                cancel();
        }

    private:
        void choose (int index)
        {
            closeThen ([chosen = onChosen, index] { if (chosen != nullptr) chosen (index); });
        }

        void onClosing() override
        {
            if (anchor != nullptr)
            {
                anchor->getProperties().set ("wingListOpen", false);
                anchor->repaint();
            }
        }

        int numRows() const { return juce::jmax (1, ((int) tiles.size() + 7) / 8); }

        /** One WING pixel, smaller when the grid would not fit the page. */
        float unit() const
        {
            const float h = 64.0f + 8.0f + 12.0f + 76.0f * (float) numRows();
            return juce::jmin (screenHeight() / 800.0f, (float) getWidth() / 892.0f, (float) getHeight() / (h + 16.0f));
        }

        juce::Rectangle<float> headerArea() const
        {
            const float u = unit();
            const float w = 876.0f * u;
            const float gridH = (12.0f + 76.0f * (float) numRows()) * u;
            auto a = anchor != nullptr ? getLocalArea (anchor, anchor->getLocalBounds()).toFloat() : juce::Rectangle<float>();
            const float x = juce::jmax (0.0f, (float) getWidth() - 8.0f * u - w);
            const float y = juce::jlimit (0.0f, juce::jmax (0.0f, (float) getHeight() - 8.0f * u - gridH - 72.0f * u), a.getY() - 4.0f * u);
            return { x, y, w, 64.0f * u };
        }

        juce::Rectangle<float> gridArea() const
        {
            const auto h = headerArea();
            return { h.getX(), h.getBottom() + 8.0f * unit(), h.getWidth(), (12.0f + 76.0f * (float) numRows()) * unit() };
        }

        juce::Rectangle<float> noneKey() const
        {
            const auto h = headerArea();
            return { h.getX() + 12.0f * unit(), h.getY() + 4.0f * unit(), 96.0f * unit(), 56.0f * unit() };
        }

        juce::Rectangle<float> closeKey() const
        {
            const auto h = headerArea();
            return h.withTrimmedLeft (h.getWidth() - 136.0f * unit()).withTrimmedRight (16.0f * unit());
        }

        juce::Rectangle<float> tileArea (int i) const
        {
            const float u = unit();
            const auto grid = gridArea();
            return { grid.getX() + (12.0f + 108.0f * (float) (i % 8)) * u, grid.getY() + (12.0f + 76.0f * (float) (i / 8)) * u, 96.0f * u, 64.0f * u };
        }

        int tileAt (juce::Point<float> p) const
        {
            for (int i = 0; i < (int) tiles.size(); ++i)
                if (tileArea (i).contains (p))
                    return i;
            return -1;
        }

        juce::Component::SafePointer<juce::Component> anchor;
        juce::String title, noneText;
        std::vector<ChannelTile> tiles;
        int selected, pressed = -1;
        std::function<void (int)> onChosen;
    };

    /** Where a window drawn on the WING's 1160 x 708 px page (a channel's page,
        right of the module column) sits over `over`: on all of it, or, when
        `over` spans the screen (EFFECTS, 1280 px), from x 96 to 1256. */
    struct PageUnits
    {
        juce::Rectangle<float> page;
        float ux = 1.0f, uy = 1.0f, u = 1.0f; // u: for type and strokes

        PageUnits (juce::Rectangle<float> over, float screenWidth) : page (over)
        {
            if (over.getWidth() > screenWidth * 0.95f)
                page = over.withTrimmedLeft (over.getWidth() * 96.0f / 1280.0f).withWidth (over.getWidth() * 1160.0f / 1280.0f);
            // WING pixels are square: a page shorter than 708 px (a HOME slot
            // screen over the parameter bar) doesn't squash the window; on the
            // console INS's SELECT FX PROCESSOR sits where EFFECTS has it [3.1].
            ux = page.getWidth() / 1160.0f;
            uy = ux;
            u = ux;
        }

        /** A rectangle given in WING pixels from the page's top left. */
        juce::Rectangle<float> at (float x, float y, float w, float h) const { return { page.getX() + x * ux, page.getY() + y * uy, w * ux, h * uy }; }
    };

    //==========================================================================
    /** The FX TYPE list (firmware 3.1): over the page, picture tiles 172 x 92
        px on a #202020 ground, the first row common to every tab, the tabs
        down the right edge (the one shown #707070). Touch a tile to load it. */
    class TabbedTilePopup final : public Popup
    {
    public:
        TabbedTilePopup (juce::Component& anchorComponent, juce::Component& overComponent, std::vector<Tile> commonRow, std::vector<TileTab> tileTabs,
                         int shownTab, int selectedId, std::function<void (juce::Graphics&, juce::Rectangle<float>, int)> paintPicture,
                         std::function<void (int)> chosen)
            : anchor (&anchorComponent), over (&overComponent), common (std::move (commonRow)), tabs (std::move (tileTabs)),
              shown (juce::jlimit (0, juce::jmax (0, (int) tabs.size() - 1), shownTab)), selected (selectedId), picture (std::move (paintPicture)),
              onChosen (std::move (chosen))
        {
            anchor->getProperties().set ("wingListOpen", true);
            anchor->repaint();
        }

        void paint (juce::Graphics& g) override
        {
            const auto units = pageUnits();
            const float u = units.u;
            g.setColour (juce::Colours::black);
            g.fillRect (units.page);
            g.setColour (Colours::screenPanel);
            g.fillRect (units.at (0.0f, 0.0f, 1088.0f, 708.0f));

            for (auto& [area, tile] : tileLayout())
            {
                const bool down = tile.id == pressed;
                g.setColour (down ? Colours::screenTile : juce::Colours::black);
                g.fillRect (area);
                // [3.1] the picture 158 x 48 px, 7 px in and 8 down; the name under it.
                const float sx = area.getWidth() / 172.0f, sy = area.getHeight() / 92.0f;
                if (picture)
                    picture (g, juce::Rectangle<float> (area.getX() + 7.0f * sx, area.getY() + 8.0f * sy, 158.0f * sx, 48.0f * sy), tile.id);
                g.setColour (Colours::text);
                g.setFont (wingFontPx (12.5f * u));
                g.drawFittedText (tile.caption, juce::Rectangle<float> (area.getX(), area.getY() + 60.0f * sy, area.getWidth(), 26.0f * sy).toNearestInt().reduced (3, 0),
                                  juce::Justification::centred, 1, 0.7f);
                if (tile.id == selected)
                {
                    g.setColour (Colours::gold);
                    g.drawRect (area, 3.0f * u);
                }
            }

            for (size_t i = 0; i < tabs.size(); ++i)
            {
                const auto& tab = tabs[i];
                const auto r = tabArea ((int) i);
                const bool lit = (int) i == shown;
                g.setColour (lit ? Colours::textDim : Colours::screenHeader);
                g.fillRect (r);
                const auto ink = lit ? juce::Colours::black : (tab.enabled ? Colours::textDim : Colours::screenLine);
                g.setColour (ink);
                if (tab.icon.has_value())
                {
                    drawIcon (g, *tab.icon, juce::Rectangle<float> (r.getX(), r.getY() + 10.0f * u, r.getWidth(), 24.0f * u).withSizeKeepingCentre (32.0f * u, 24.0f * u), ink);
                    g.setFont (wingFontPx (10.0f * u, WingWeight::SemiBold));
                    g.drawFittedText (tab.title, r.withTrimmedTop (42.0f * u).withHeight (20.0f * u).toNearestInt(), juce::Justification::centred, 1, 0.8f);
                    continue;
                }
                g.setFont (wingFontPx (13.5f * u, WingWeight::SemiBold));
                g.drawFittedText (tab.title, r.withTrimmedTop (10.0f * u).withHeight (26.0f * u).toNearestInt(), juce::Justification::centred, 1, 0.8f);
                g.setFont (wingFontPx (10.0f * u));
                g.drawFittedText (tab.subtitle, r.withTrimmedTop (42.0f * u).withHeight (22.0f * u).toNearestInt(), juce::Justification::centred, 1, 0.8f);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (! pageUnits().page.contains (e.position))
            {
                cancel();
                return;
            }
            for (int i = 0; i < (int) tabs.size(); ++i)
                if (tabArea (i).contains (e.position) && tabs[(size_t) i].enabled)
                {
                    shown = i;
                    repaint();
                    return;
                }
            pressed = tileAt (e.position);
            repaint();
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            const int was = pressed;
            pressed = -1;
            repaint();
            if (was >= 0 && tileAt (e.position) == was)
                closeThen ([chosen = onChosen, was] { if (chosen != nullptr) chosen (was); });
        }

    private:
        void onClosing() override
        {
            if (anchor != nullptr)
            {
                anchor->getProperties().set ("wingListOpen", false);
                anchor->repaint();
            }
        }

        PageUnits pageUnits() const
        {
            return PageUnits (over != nullptr ? getLocalArea (over, over->getLocalBounds()).toFloat() : getLocalBounds().toFloat(), (float) getWidth());
        }

        // [3.1] tabs 64 px square, 72 px apart, 8 px right of the list.
        juce::Rectangle<float> tabArea (int i) const { return pageUnits().at (1096.0f, 72.0f * (float) i, 64.0f, 64.0f); }

        // [3.1] tiles 172 x 92 px, 180 px across and 100 px down.
        std::vector<std::pair<juce::Rectangle<float>, Tile>> tileLayout() const
        {
            std::vector<std::pair<juce::Rectangle<float>, Tile>> out;
            const auto units = pageUnits();
            auto addRow = [&] (const std::vector<Tile>& row, int r)
            {
                for (size_t c = 0; c < row.size(); ++c)
                    if (row[c].id >= 0)
                        out.push_back ({ units.at (8.0f + 180.0f * (float) c, 8.0f + 100.0f * (float) r, 172.0f, 92.0f), row[c] });
            };
            addRow (common, 0);
            if (juce::isPositiveAndBelow (shown, (int) tabs.size()))
                for (size_t r = 0; r < tabs[(size_t) shown].rows.size(); ++r)
                    addRow (tabs[(size_t) shown].rows[r], (int) r + 1);
            return out;
        }

        int tileAt (juce::Point<float> p) const
        {
            for (auto& [area, tile] : tileLayout())
                if (area.contains (p))
                    return tile.id;
            return -1;
        }

        juce::Component::SafePointer<juce::Component> anchor, over;
        std::vector<Tile> common;
        std::vector<TileTab> tabs;
        int shown, selected, pressed = -1;
        std::function<void (juce::Graphics&, juce::Rectangle<float>, int)> picture;
        std::function<void (int)> onChosen;
    };

    //==========================================================================
    /** SELECT FX PROCESSOR (firmware 3.1): a black window over the dimmed
        page with a #202020 title bar (NONE, the title, CLOSE), the 16 slots
        256 x 92 px in four columns with a #707070 outline, and the group
        captions under them. Touch a slot to use it. */
    class RackSlotPopup final : public Popup
    {
    public:
        RackSlotPopup (juce::Component& overComponent, std::vector<RackSlot> rackSlots, int selectedSlot,
                       std::function<bool (juce::Graphics&, juce::Rectangle<float>, int)> device,
                       std::function<void (juce::Graphics&, juce::Rectangle<float>, int)> sourceIcon, std::function<void (int)> chosen)
            : over (&overComponent), slots (std::move (rackSlots)), selected (selectedSlot), paintDevice (std::move (device)),
              paintSourceIcon (std::move (sourceIcon)), onChosen (std::move (chosen))
        {
        }

        void paint (juce::Graphics& g) override
        {
            const auto units = pageUnits();
            const float u = units.u;
            g.setColour (Colours::scrim);
            g.fillRect (getLocalBounds());
            g.setColour (juce::Colours::black);
            g.fillRect (units.at (48.0f, 72.0f, 1062.0f, 504.0f));

            // The title bar: NONE (a circled cross over its name), the title, CLOSE and its ticked box.
            g.setColour (Colours::screenHeader);
            g.fillRect (units.at (56.0f, 80.0f, 1047.0f, 64.0f));
            const auto noneKey = noneArea(), closeKey = closeArea();
            for (auto [key, id] : { std::pair (noneKey, kNone), std::pair (closeKey, kClose) })
                if (pressed == id)
                {
                    g.setColour (Colours::screenTile);
                    g.fillRect (key);
                }
            drawIcon (g, Icon::Cancel, units.at (80.0f, 88.0f, 32.0f, 32.0f), Colours::caption);
            g.setColour (Colours::caption);
            g.setFont (wingFontPx (10.0f * u));
            g.drawText ("NONE", units.at (56.0f, 124.0f, 80.0f, 20.0f), juce::Justification::centred, false);
            g.setFont (wingFontPx (20.0f * u));
            g.drawText ("SELECT FX PROCESSOR", units.at (300.0f, 80.0f, 560.0f, 64.0f), juce::Justification::centred, false);
            g.setFont (wingFontPx (14.5f * u));
            g.drawText ("CLOSE", units.at (900.0f, 80.0f, 140.0f, 64.0f), juce::Justification::centredRight, false);
            drawIcon (g, Icon::Check, units.at (1055.0f, 96.0f, 32.0f, 32.0f), Colours::caption);

            for (int i = 0; i < (int) slots.size(); ++i)
                paintSlot (g, i, slotArea (i), u);

            // The groups' captions, centred under their two columns.
            g.setColour (Colours::textDim);
            const auto font = wingFontPx (14.5f * u); // (3.1: 14.5 px capitals, the diamond 26 px wide)
            g.setFont (font);
            const float premiumW = juce::GlyphArrangement::getStringWidth (font, "PREMIUM FX");
            const auto premium = units.at (56.0f, 536.0f, 517.0f, 26.0f);
            const float x0 = premium.getCentreX() - (premiumW + 33.0f * u) * 0.5f;
            drawIcon (g, Icon::Premium, juce::Rectangle<float> (x0, premium.getY() + 1.0f * u, 26.0f * u, 24.0f * u), Colours::textDim);
            g.drawText ("PREMIUM FX", juce::Rectangle<float> (x0 + 33.0f * u, premium.getY(), premiumW + 4.0f, premium.getHeight()),
                        juce::Justification::centredLeft, false);
            g.drawText ("STANDARD FX", units.at (586.0f, 536.0f, 517.0f, 26.0f), juce::Justification::centred, false);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (! pageUnits().at (48.0f, 72.0f, 1062.0f, 504.0f).contains (e.position))
            {
                cancel();
                return;
            }
            pressed = hit (e.position);
            repaint();
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            const int was = pressed;
            pressed = kNothing;
            repaint();
            if (was == kNothing || hit (e.position) != was)
                return;
            if (was == kClose)
                cancel();
            else
                closeThen ([chosen = onChosen, was] { if (chosen != nullptr) chosen (was == kNone ? -1 : was); });
        }

    private:
        static constexpr int kNothing = -3, kClose = -2, kNone = -1;

        PageUnits pageUnits() const
        {
            return PageUnits (over != nullptr ? getLocalArea (over, over->getLocalBounds()).toFloat() : getLocalBounds().toFloat(), (float) getWidth());
        }

        juce::Rectangle<float> noneArea() const { return pageUnits().at (56.0f, 80.0f, 80.0f, 64.0f); }
        juce::Rectangle<float> closeArea() const { return pageUnits().at (968.0f, 80.0f, 135.0f, 64.0f); }

        // [3.1] slots run down the columns (1-4 in the first); a 13 px gap
        // between the premium and the standard pairs.
        juce::Rectangle<float> slotArea (int i) const
        {
            static constexpr float columns[] = { 56.0f, 317.0f, 586.0f, 847.0f };
            return pageUnits().at (columns[(i / 4) % 4], 149.0f + 97.0f * (float) (i % 4), 256.0f, 92.0f);
        }

        int hit (juce::Point<float> p) const
        {
            if (noneArea().contains (p))
                return kNone;
            if (closeArea().contains (p))
                return kClose;
            for (int i = 0; i < (int) slots.size(); ++i)
                if (slotArea (i).contains (p))
                    return i;
            return kNothing;
        }

        void paintSlot (juce::Graphics& g, int i, juce::Rectangle<float> r, float u) const
        {
            const auto& slot = slots[(size_t) i];
            const float sx = r.getWidth() / 256.0f, sy = r.getHeight() / 92.0f;
            auto at = [&r, sx, sy] (float x, float y, float w, float h) { return juce::Rectangle<float> (r.getX() + x * sx, r.getY() + y * sy, w * sx, h * sy); };
            g.setColour (pressed == i ? Colours::screenDark : juce::Colours::black);
            g.fillRect (r);
            g.setColour (i == selected ? Colours::gold : Colours::textDim);
            g.drawRect (r, (i == selected ? 3.0f : 1.5f) * u);

            // Who uses the slot: its colour over a grey box with its icon and name.
            if (slot.source.isNotEmpty())
            {
                g.setColour (slot.sourceColour);
                g.fillRect (at (8.0f, 8.0f, 64.0f, 16.0f));
                g.setColour (Colours::screenTile);
                g.fillRect (at (8.0f, 24.0f, 64.0f, 60.0f));
                if (paintSourceIcon)
                    paintSourceIcon (g, at (20.0f, 28.0f, 40.0f, 34.0f), i);
                g.setColour (Colours::caption);
                g.setFont (wingFontPx (10.0f * u));
                g.drawFittedText (slot.source, at (8.0f, 66.0f, 64.0f, 18.0f).toNearestInt(), juce::Justification::centred, 1, 0.7f);
            }

            const auto device = at (78.0f, 8.0f, 170.0f, 60.0f);
            if (! (paintDevice && paintDevice (g, device, i)))
            {
                g.setColour (Colours::screenPanel);
                g.fillRect (device);
            }
            g.setColour (Colours::caption);
            g.setFont (wingFontPx (10.0f * u));
            g.drawFittedText (slot.caption, juce::Rectangle<float> (device.getX(), r.getY() + 70.0f * sy, device.getWidth(), 18.0f * sy).toNearestInt(),
                              juce::Justification::centred, 1, 0.7f);

            // The slot's number, bottom right.
            const auto number = i < 9 ? at (238.0f, 73.0f, 16.0f, 18.0f) : at (234.0f, 73.0f, 20.0f, 18.0f);
            g.setColour (Colours::textDim);
            g.fillRect (number);
            g.setColour (juce::Colours::black);
            g.setFont (wingFontPx (9.0f * u, WingWeight::SemiBold));
            g.drawFittedText (juce::String (i + 1), number.toNearestInt(), juce::Justification::centred, 1, 0.6f);
        }

        juce::Component::SafePointer<juce::Component> over;
        std::vector<RackSlot> slots;
        int selected, pressed = kNothing;
        std::function<bool (juce::Graphics&, juce::Rectangle<float>, int)> paintDevice;
        std::function<void (juce::Graphics&, juce::Rectangle<float>, int)> paintSourceIcon;
        std::function<void (int)> onChosen;
    };

    //==========================================================================
    /** The text of a notice: headings (gold), paragraphs and bullets, laid out
        for a width; it sits in a viewport that scrolls it. */
    class NoticeText final : public juce::Component
    {
    public:
        explicit NoticeText (const juce::String& text)
        {
            juce::String paragraph;
            auto flush = [this, &paragraph]
            {
                if (paragraph.isNotEmpty())
                    blocks.push_back ({ paragraph, Kind::Paragraph });
                paragraph.clear();
            };
            for (auto line : juce::StringArray::fromLines (text))
            {
                line = line.trimEnd();
                if (line.startsWith ("## "))
                {
                    flush();
                    blocks.push_back ({ line.substring (3), Kind::Heading });
                }
                else if (line.startsWith ("- "))
                {
                    flush();
                    blocks.push_back ({ line.substring (2), Kind::Bullet });
                }
                else if (line.isEmpty())
                {
                    flush();
                }
                else if (! blocks.empty() && blocks.back().kind == Kind::Bullet && paragraph.isEmpty() && line.startsWith ("  "))
                {
                    blocks.back().text << " " << line.trim(); // a bullet's continuation line
                }
                else
                {
                    paragraph << (paragraph.isEmpty() ? "" : " ") << line.trim();
                }
            }
            flush();
            setInterceptsMouseClicks (false, false);
        }

        /** Lays the text out `width` px wide with capitals `capPx` tall; returns the height. */
        int layout (int width, float capPx)
        {
            cap = capPx;
            const auto body = wingFontPx (capPx), heading = wingFontPx (capPx * 1.12f, WingWeight::SemiBold);
            const float indent = capPx * 2.2f, gap = capPx * 0.9f;
            float y = capPx * 0.4f;
            laid.clear();
            for (size_t i = 0; i < blocks.size(); ++i)
            {
                const auto& block = blocks[i];
                const bool isHeading = block.kind == Kind::Heading, isBullet = block.kind == Kind::Bullet;
                if (isHeading && i > 0)
                    y += gap * 0.8f;
                juce::AttributedString as;
                as.setWordWrap (juce::AttributedString::byWord);
                as.setLineSpacing (capPx * 0.42f);
                as.append (block.text, isHeading ? heading : body, isHeading ? Colours::gold : Colours::text);
                const float x = isBullet ? indent : 0.0f;
                juce::TextLayout tl;
                tl.createLayout (as, juce::jmax (40.0f, (float) width - x));
                const float h = tl.getHeight();
                laid.push_back ({ std::move (tl), { x, y }, isBullet });
                y += h + (isBullet ? gap * 0.45f : gap);
            }
            return juce::roundToInt (y + capPx * 0.4f);
        }

        void paint (juce::Graphics& g) override
        {
            for (auto& l : laid)
            {
                if (l.bullet)
                {
                    // A small gold square level with the first line's capitals.
                    const float s = cap * 0.42f;
                    const float baseline = l.text.getNumLines() > 0 ? l.text.getLine (0).lineOrigin.y : cap;
                    g.setColour (Colours::gold);
                    g.fillRect (juce::Rectangle<float> (s, s).withCentre ({ cap * 0.9f, l.at.y + baseline - cap * 0.5f }));
                }
                l.text.draw (g, juce::Rectangle<float> (l.at.x, l.at.y, (float) getWidth() - l.at.x, l.text.getHeight()));
            }
        }

    private:
        enum class Kind
        {
            Paragraph,
            Heading,
            Bullet
        };
        struct Block
        {
            juce::String text;
            Kind kind;
        };
        struct Laid
        {
            juce::TextLayout text;
            juce::Point<float> at;
            bool bullet;
        };
        std::vector<Block> blocks;
        std::vector<Laid> laid;
        float cap = 12.0f;
    };

    /** A notice: a big centred window with a title bar, the scrolling text, an
        optional tick key and OK. Clicks on the dimmed page don't close it. */
    class NoticeWindow final : public Popup
    {
    public:
        NoticeWindow (juce::String titleText, const juce::String& text, NoticeOptions o)
            : title (std::move (titleText)), options (std::move (o)), body (text), checked (options.checked)
        {
            // The text is drawn once into an image: the live surface under
            // the scrim repaints all the time, and with it what lies over it.
            body.setBufferedToImage (true);
            viewport.setViewedComponent (&body, false);
            viewport.setScrollBarsShown (true, false);
            viewport.setWantsKeyboardFocus (false);
            addAndMakeVisible (viewport);
        }

        void resized() override
        {
            const auto b = getLocalBounds();
            // The type follows the layer: capitals 1.8 % of its height, readable on
            // a small main display and not huge over a 4K window.
            cap = juce::jlimit (9.0f, screenCap (30.0f), juce::jmin ((float) b.getHeight() * 0.018f, (float) b.getWidth() * 0.013f));
            window = b.withSizeKeepingCentre (juce::jmin (b.getWidth() - 16, juce::roundToInt (cap * 62.0f)),
                                              juce::roundToInt ((float) b.getHeight() * 0.9f));
            auto r = window;
            titleBar = r.removeFromTop (juce::roundToInt (cap * 3.2f));
            r.removeFromTop (4);
            panel = r;
            auto keys = r.removeFromBottom (juce::roundToInt (cap * 4.4f)).reduced (juce::roundToInt (cap), juce::roundToInt (cap * 0.7f));
            const int keyW = juce::roundToInt (cap * 9.0f);
            okKey = keys.removeFromRight (keyW);
            keys.removeFromRight (juce::roundToInt (cap));
            checkKey = options.checkText.isEmpty() ? juce::Rectangle<int>()
                                                   : keys.removeFromRight (juce::jmin (keys.getWidth(), juce::roundToInt (cap * 30.0f)));
            const int side = juce::roundToInt (cap * 1.6f);
            viewport.setScrollBarThickness (juce::jmax (8, juce::roundToInt (cap * 0.8f)));
            viewport.setBounds (r.reduced (side, juce::roundToInt (cap * 0.6f)));
            const int textW = viewport.getWidth() - viewport.getScrollBarThickness() - juce::roundToInt (cap);
            body.setSize (textW, body.layout (textW, cap));
        }

        void paint (juce::Graphics& g) override
        {
            g.setColour (Colours::scrim);
            g.fillRect (getLocalBounds());
            g.setColour (juce::Colours::black);
            g.fillRect (window.expanded (2));
            g.setColour (Colours::screenHeader);
            g.fillRect (titleBar);
            g.setColour (Colours::caption);
            g.setFont (wingFontPx (cap * 1.15f, WingWeight::SemiBold));
            g.drawFittedText (title, titleBar.reduced (juce::roundToInt (cap)), juce::Justification::centred, 1, 0.8f);
            g.setColour (Colours::screenPanel);
            g.fillRect (panel);
            g.setColour (juce::Colours::black);
            g.fillRect (panel.getX(), okKey.getY() - juce::roundToInt (cap * 0.7f) - 2, panel.getWidth(), 2);

            if (! checkKey.isEmpty())
            {
                paintKey (g, checkKey.toFloat(), pressed == 1 ? Colours::screenKey.brighter (0.15f) : Colours::screenKey, juce::Colours::black);
                auto k = checkKey.reduced (juce::roundToInt (cap * 0.6f), 0);
                const int boxSide = juce::roundToInt (cap * 1.6f);
                auto box = k.removeFromLeft (juce::roundToInt (cap * 1.9f)).withSizeKeepingCentre (boxSide, boxSide).toFloat();
                g.setColour (checked ? Colours::gold : Colours::caption);
                g.drawRect (box, juce::jmax (1.0f, cap * 0.12f));
                if (checked)
                {
                    juce::Path tick;
                    tick.startNewSubPath (box.getRelativePoint (0.22f, 0.52f));
                    tick.lineTo (box.getRelativePoint (0.43f, 0.74f));
                    tick.lineTo (box.getRelativePoint (0.8f, 0.28f));
                    g.strokePath (tick, juce::PathStrokeType (juce::jmax (1.5f, cap * 0.2f)));
                }
                k.removeFromLeft (juce::roundToInt (cap * 0.6f));
                g.setColour (checked ? Colours::text : Colours::caption);
                g.setFont (wingFontPx (cap * 0.95f));
                g.drawFittedText (options.checkText, k, juce::Justification::centredLeft, 2, 0.8f);
            }
            paintKey (g, okKey.toFloat(), pressed == 2 ? Colours::gold.brighter (0.15f) : Colours::gold, juce::Colours::black);
            g.setColour (juce::Colours::black);
            g.setFont (wingFontPx (cap * 1.05f, WingWeight::SemiBold));
            g.drawFittedText (options.okText, okKey, juce::Justification::centred, 1, 0.8f);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            const auto p = e.getPosition();
            pressed = okKey.contains (p) ? 2 : (checkKey.contains (p) ? 1 : 0);
            repaint();
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            const auto p = e.getPosition();
            const int was = pressed;
            pressed = 0;
            if (was == 1 && checkKey.contains (p))
                checked = ! checked;
            repaint();
            if (was == 2 && okKey.contains (p))
                closeThen ({});
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
        {
            // The wheel scrolls the text wherever the mouse is over the window.
            // Not through Viewport::mouseWheelMove: a wheel it can't use (at the
            // end of the text, sideways) goes back to its parent, this window,
            // which sent it on again until the stack ran out.
            viewport.useMouseWheelMoveIfNeeded (e.getEventRelativeTo (&viewport), wheel);
        }

        bool keyPressed (const juce::KeyPress& k) override
        {
            if (k == juce::KeyPress::returnKey)
            {
                closeThen ({});
                return true;
            }
            const int line = juce::roundToInt (cap * 2.0f), page = juce::jmax (line, viewport.getHeight() - line);
            const int code = k.getKeyCode();
            const int delta = code == juce::KeyPress::downKey                                             ? line
                            : code == juce::KeyPress::upKey                                               ? -line
                            : code == juce::KeyPress::pageDownKey || code == juce::KeyPress::spaceKey ? page
                            : code == juce::KeyPress::pageUpKey                                           ? -page
                                                                                                          : 0;
            if (delta != 0)
            {
                viewport.setViewPosition (0, viewport.getViewPositionY() + delta);
                return true;
            }
            if (code == juce::KeyPress::homeKey || code == juce::KeyPress::endKey)
            {
                viewport.setViewPosition (0, code == juce::KeyPress::homeKey ? 0 : body.getHeight());
                return true;
            }
            return Popup::keyPressed (k);
        }

    private:
        void onClosing() override
        {
            if (options.onClose != nullptr)
                options.onClose (checked);
        }

        juce::String title;
        NoticeOptions options;
        NoticeText body;
        juce::Viewport viewport;
        bool checked;
        float cap = 12.0f;
        juce::Rectangle<int> window, titleBar, panel, okKey, checkKey;
        int pressed = 0;
    };
} // namespace

//==============================================================================
void chooseTile (juce::Component& anchor, std::vector<std::vector<Tile>> rows, std::vector<int> spacedRows, int selected,
                 std::function<void (juce::Graphics&, juce::Rectangle<float>, int)> paintPicture, std::function<void (int)> onChosen)
{
    auto* layer = layerFor (anchor);
    if (layer == nullptr || rows.empty())
        return;
    (new TilePopup (anchor, std::move (rows), std::move (spacedRows), selected, std::move (paintPicture), std::move (onChosen)))->open (*layer);
}

void chooseTabbedTile (juce::Component& anchor, juce::Component& over, std::vector<Tile> common, std::vector<TileTab> tabs, int shownTab,
                       int selected, std::function<void (juce::Graphics&, juce::Rectangle<float>, int)> paintPicture, std::function<void (int)> onChosen)
{
    auto* layer = layerFor (anchor);
    if (layer == nullptr || tabs.empty())
        return;
    (new TabbedTilePopup (anchor, over, std::move (common), std::move (tabs), shownTab, selected, std::move (paintPicture), std::move (onChosen)))
        ->open (*layer);
}

void chooseRackSlot (juce::Component& over, std::vector<RackSlot> slots, int selected,
                     std::function<bool (juce::Graphics&, juce::Rectangle<float>, int)> paintDevice,
                     std::function<void (juce::Graphics&, juce::Rectangle<float>, int)> paintSourceIcon, std::function<void (int)> onChosen)
{
    auto* layer = layerFor (over);
    if (layer == nullptr || slots.empty())
        return;
    (new RackSlotPopup (over, std::move (slots), selected, std::move (paintDevice), std::move (paintSourceIcon), std::move (onChosen)))->open (*layer);
}

void chooseChannel (juce::Component& anchor, const juce::String& title, const juce::String& noneText, std::vector<ChannelTile> tiles, int selected,
                    std::function<void (int)> onChosen)
{
    auto* layer = layerFor (anchor);
    if (layer == nullptr)
        return;
    (new ChannelPopup (anchor, title, noneText, std::move (tiles), selected, std::move (onChosen)))->open (*layer);
}

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

void notice (juce::Component& host, const juce::String& title, const juce::String& text, NoticeOptions options)
{
    auto* layer = options.coverHost ? &host : layerFor (host);
    if (layer == nullptr)
        return;
    (new NoticeWindow (title, text, std::move (options)))->open (*layer);
}

void warn (juce::Component& host, const juce::String& title, const juce::String& text)
{
    auto* layer = layerFor (host);
    if (layer == nullptr)
        return;
    (new MessageWindow (title, text, "OK", nullptr, false, false))->open (*layer);
}

} // namespace livedaw::ui::popups
