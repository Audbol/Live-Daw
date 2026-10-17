#include "ui/WingPopups.h"

// Placeholder implementations: desktop dialogs and menus until the WING-style
// windows are drawn. The API is final; callers can use it already.
namespace livedaw::ui::popups
{

void editText (juce::Component& host, const juce::String& title, const juce::String& initial,
               std::function<void (const juce::String&)> onDone, TextOptions options)
{
    auto* window = new juce::AlertWindow (title, {}, juce::MessageBoxIconType::NoIcon, &host);
    window->addTextEditor ("text", initial);
    if (auto* editor = window->getTextEditor ("text"))
        editor->setInputRestrictions (options.maxLength);
    window->addButton (options.actionText, 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("CLOSE", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    window->enterModalState (true, juce::ModalCallbackFunction::create ([window, onDone] (int result)
    {
        if (result == 1 && onDone)
            onDone (window->getTextEditorContents ("text"));
    }), true);
}

struct WingKeyboard::Impl
{
    juce::TextEditor editor;
    juce::TextButton action { "DONE" };
};

WingKeyboard::WingKeyboard() : impl (std::make_unique<Impl>())
{
    addAndMakeVisible (impl->editor);
    addAndMakeVisible (impl->action);
    impl->editor.onTextChange = [this] { if (onChange) onChange (getText()); };
    impl->action.onClick = [this] { if (onAction) onAction (getText()); };
}

WingKeyboard::~WingKeyboard() = default;

void WingKeyboard::setText (const juce::String& t) { impl->editor.setText (t, false); }
juce::String WingKeyboard::getText() const { return impl->editor.getText(); }
void WingKeyboard::setActionText (const juce::String& t) { impl->action.setButtonText (t); }

void WingKeyboard::paint (juce::Graphics&) {}

void WingKeyboard::resized()
{
    auto b = getLocalBounds();
    impl->editor.setBounds (b.removeFromTop (juce::jmin (40, b.getHeight())));
    impl->action.setBounds (b.removeFromTop (juce::jmin (40, b.getHeight())).removeFromRight (120));
}

void chooseFrom (juce::Component& anchor, const juce::StringArray& items, int selected,
                 std::function<void (int)> onChosen, ListOptions options)
{
    juce::PopupMenu menu;
    if (options.caption.isNotEmpty())
        menu.addSectionHeader (options.caption);
    for (int i = 0; i < items.size(); ++i)
    {
        const bool enabled = std::find (options.disabled.begin(), options.disabled.end(), i) == options.disabled.end();
        menu.addItem (i + 1, items[i], enabled, i == selected);
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&anchor).withMinimumNumColumns (juce::jmax (1, options.columns)),
                        [onChosen] (int result) { if (result > 0 && onChosen) onChosen (result - 1); });
}

void confirm (juce::Component& host, const juce::String& title, const juce::String& text,
              const juce::String& okText, std::function<void()> onOk, bool danger)
{
    juce::ignoreUnused (danger);
    auto options = juce::MessageBoxOptions().withTitle (title).withMessage (text)
                       .withButton (okText).withButton ("CANCEL").withAssociatedComponent (&host)
                       .withIconType (danger ? juce::MessageBoxIconType::WarningIcon : juce::MessageBoxIconType::QuestionIcon);
    juce::AlertWindow::showAsync (options, [onOk] (int result) { if (result == 1 && onOk) onOk(); });
}

void warn (juce::Component& host, const juce::String& title, const juce::String& text)
{
    auto options = juce::MessageBoxOptions().withTitle (title).withMessage (text).withButton ("OK")
                       .withAssociatedComponent (&host).withIconType (juce::MessageBoxIconType::WarningIcon);
    juce::AlertWindow::showAsync (options, nullptr);
}

} // namespace livedaw::ui::popups
