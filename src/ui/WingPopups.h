#pragma once

#include "ui/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <vector>

/** The WING's on-screen pop-ups: floating windows over a 75 % black scrim on the
    main display, drawn like the console's (full on-screen keyboard for names,
    anchored lists with the selected row filled gold, confirmations that turn red
    when destructive). Use these instead of desktop AlertWindows / PopupMenus.

    They open in the main display's pop-up layer (over the page and the
    parameter bar, under the status bar) when `host` is on the main display,
    otherwise over `host`'s window. Esc, CLOSE, a click on the dimmed page or a
    key of the surface closes them as cancelled. */
namespace livedaw::ui::popups
{

struct TextOptions
{
    std::optional<Icon> icon;
    juce::StringArray suggestions;   // name suggestions shown above the keyboard
    int maxLength = 16;
    juce::String actionText = "DONE";
    std::function<void()> onNext;    // shows a NEXT key (e.g. rename the next channel)
};

/** A CONTROL NAME-style keyboard window over `host`. `onDone` gets the text; it
    isn't called when the window is closed with CLOSE. With `onNext`, the action
    key reads NEXT: it calls `onDone`, closes the window, then calls `onNext`
    (which can open the window again for the next item). */
void editText (juce::Component& host, const juce::String& title, const juce::String& initial,
               std::function<void (const juce::String&)> onDone, TextOptions = {});

/** The keyboard as a component to embed in a screen (HOME > NAME): the name
    field (icon, text with a gold caret, the circled clear key), an optional row
    of suggestion keys, and the five key rows of the console (caps lock lit
    blue, the action key at the bottom right). Typing on the computer keyboard
    works while it has the keyboard focus. */
class WingKeyboard : public juce::Component
{
public:
    WingKeyboard();
    ~WingKeyboard() override;

    void setText (const juce::String&);
    juce::String getText() const;
    /** Where typing goes in the text (0 = before the first character). */
    int getCaretPosition() const;
    void setActionText (const juce::String&);

    /** The icon drawn left of the text in the field. */
    void setIcon (std::optional<Icon>);
    /** Name suggestions: a row of up to five keys under the field (empty: no row). */
    void setSuggestions (const juce::StringArray&);
    /** Keeps the row of five suggestion keys even when there are no
        suggestions (HOME > NAME draws it with blank keys, as on the console). */
    void setSuggestionRowAlwaysShown (bool);
    void setMaxLength (int);
    /** Whether the keyboard draws its own name field (default true). */
    void setFieldVisible (bool);
    /** The action key's icon: a ticked box for DONE, a chevron for NEXT (default:
        chosen from the action text). */
    void setActionIcon (std::optional<Icon>);

    std::function<void (const juce::String&)> onChange;
    std::function<void (const juce::String&)> onAction;
    /** Esc on the computer keyboard. */
    std::function<void()> onCancel;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

struct ListOptions
{
    int columns = 1;
    std::vector<int> disabled;  // indices drawn dim and not selectable
    juce::String caption;
};

/** An anchored list under `anchor` (or beside it when there's no room below);
    the selected row is filled gold. `onChosen` isn't called when the list is
    closed without a choice. While the list is open the anchor's
    "wingListOpen" property is true (DropdownField then draws itself grey). */
void chooseFrom (juce::Component& anchor, const juce::StringArray& items, int selected,
                 std::function<void (int)> onChosen, ListOptions = {});

/** A tile of chooseTile: `id` < 0 leaves a gap. */
struct Tile
{
    int id = -1;
    juce::String caption;
};

/** A grid of picture tiles over the page (the dynamics model list, 3.1):
    rows of tiles, extra space above the rows listed in `spacedRows`, the
    tile `selected` framed gold. `paintPicture` draws a tile's picture;
    `onChosen` gets the id touched (not called when closed). */
void chooseTile (juce::Component& anchor, std::vector<std::vector<Tile>> rows, std::vector<int> spacedRows, int selected,
                 std::function<void (juce::Graphics&, juce::Rectangle<float>, int id)> paintPicture, std::function<void (int id)> onChosen);

/** An on-screen confirmation over `host`; red when `danger`. */
void confirm (juce::Component& host, const juce::String& title, const juce::String& text,
              const juce::String& okText, std::function<void()> onOk, bool danger = false);

/** An on-screen notice over `host` with an OK key. */
void warn (juce::Component& host, const juce::String& title, const juce::String& text);

struct NoticeOptions
{
    juce::String okText = "OK";
    /** A tick key left of OK (empty: none) and its state when the window opens. */
    juce::String checkText;
    bool checked = false;
    /** Called when the window closes (OK, Esc or a key of the surface), with
        the tick key's state. */
    std::function<void (bool checked)> onClose;
    /** Cover `host` itself rather than the main display's pop-up layer or the
        window (the start-up notice covers the whole surface). */
    bool coverHost = false;
};

/** A long, scrollable notice over `host`: a title bar, the text, and OK at the
    bottom right. In `text`, a line starting with "## " is a heading, one
    starting with "- " a bullet (indented lines continue it), an empty line
    separates paragraphs. Its type follows the window's size, so it stays
    readable over a small screen and over a whole window. */
void notice (juce::Component& host, const juce::String& title, const juce::String& text, NoticeOptions = {});

/** Where pop-ups for `host` open: the main display registers its pop-up layer
    here (return nullptr for hosts it doesn't contain). */
void setPopupLayerProvider (std::function<juce::Component* (juce::Component& host)>);

} // namespace livedaw::ui::popups
