#pragma once

#include "ui/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <vector>

/** The WING's on-screen pop-ups: floating windows over a 75 % black scrim on the
    main display, drawn like the console's (full on-screen keyboard for names,
    anchored lists with the selected row filled gold, confirmations that turn red
    when destructive). Use these instead of desktop AlertWindows / PopupMenus. */
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
    isn't called when the window is closed with CLOSE. */
void editText (juce::Component& host, const juce::String& title, const juce::String& initial,
               std::function<void (const juce::String&)> onDone, TextOptions = {});

/** The keyboard as a component to embed in a screen (HOME > NAME). */
class WingKeyboard : public juce::Component
{
public:
    WingKeyboard();
    ~WingKeyboard() override;

    void setText (const juce::String&);
    juce::String getText() const;
    void setActionText (const juce::String&);

    std::function<void (const juce::String&)> onChange;
    std::function<void (const juce::String&)> onAction;

    void paint (juce::Graphics&) override;
    void resized() override;

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

/** An anchored list under `anchor`; the selected row is filled gold. */
void chooseFrom (juce::Component& anchor, const juce::StringArray& items, int selected,
                 std::function<void (int)> onChosen, ListOptions = {});

/** An on-screen confirmation over `host`; red when `danger`. */
void confirm (juce::Component& host, const juce::String& title, const juce::String& text,
              const juce::String& okText, std::function<void()> onOk, bool danger = false);

/** An on-screen notice over `host` with an OK key. */
void warn (juce::Component& host, const juce::String& title, const juce::String& text);

} // namespace livedaw::ui::popups
