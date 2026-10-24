#include "ui/PluginChainWindow.h"
#include "ui/LookAndFeel.h"

namespace livedaw::ui
{

PluginChainWindow::PluginChainWindow (UiContext& c, int slot, std::function<void (PluginChainWindow&)> close)
    : juce::DocumentWindow ("FX " + juce::String (slot + 1), Colours::panel, closeButton | minimiseButton),
      ctx (c),
      view (c.engine.getFx(), slot,
            { [this] (juce::Component& target) { showAddMenu (target); },
              [this] (int index) { removeMember (index); },
              [this] { chainChanged(); },
              [this] (PluginChainView::Layout l) { ctx.settings.setValue (settingsKey ("Layout"), l == PluginChainView::Layout::focus ? "focus" : "row"); } }),
      onClose (std::move (close))
{
    setUsingNativeTitleBar (true);
    if (ctx.settings.getValue (settingsKey ("Layout")) == "focus")
        view.setLayout (PluginChainView::Layout::focus);
    setContentNonOwned (&view, true);
    setResizable (true, false);
    setResizeLimits (PluginChainView::kMinCardWidth, PluginChainView::kBarHeight + 3 * PluginChainView::kTitleHeight, 16384, 16384);
    chainChanged();

    // The size it had last time (within the screen).
    const auto saved = juce::StringArray::fromTokens (ctx.settings.getValue (settingsKey ("Size")), " ", {});
    if (saved.size() == 2 && saved[0].getIntValue() > 0 && saved[1].getIntValue() > 0)
    {
        auto area = juce::Rectangle<int> (1600, 1000);
        if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            area = display->userBounds.toNearestInt();
        view.setSize (juce::jmin (saved[0].getIntValue(), area.getWidth()), juce::jmin (saved[1].getIntValue(), area.getHeight() - 40));
    }
    centreWithSize (getWidth(), getHeight());
    remembering = true;
    setVisible (true);
}

juce::String PluginChainWindow::settingsKey (const char* what) const
{
    return "pluginWindow" + juce::String (what) + juce::String (getSlot() + 1);
}

void PluginChainWindow::resized()
{
    juce::DocumentWindow::resized();
    keepOnScreen();
    if (remembering && view.getWidth() > 0 && view.getHeight() > 0)
        ctx.settings.setValue (settingsKey ("Size"), juce::String (view.getWidth()) + " " + juce::String (view.getHeight()));
}

void PluginChainWindow::keepOnScreen()
{
    if (placing || ! isOnDesktop())
        return;
    const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds());
    if (display == nullptr)
        return;
    auto area = display->userBounds.toNearestInt();
    if (auto* peer = getPeer())
        if (const auto frame = peer->getFrameSizeIfPresent())
            area = frame->subtractedFrom (area);
    const auto bounds = getBounds();
    if (const auto fitted = bounds.constrainedWithin (area); fitted != bounds && ! area.isEmpty())
    {
        const juce::ScopedValueSetter<bool> busy (placing, true);
        setBounds (fitted);
    }
}

PluginChainWindow::~PluginChainWindow()
{
    clearContentComponent();
}

void PluginChainWindow::chainChanged()
{
    auto& fx = ctx.engine.getFx();
    const int slot = getSlot();
    if (! fx.hasPlugin (slot))
    {
        requestClose();
        return;
    }
    auto* chain = fx.getChain (slot);
    setName ("FX " + juce::String (slot + 1) + ": " + (chain != nullptr ? chain->describeMembers() : fx.getPlugin (slot)->getName()));
}

void PluginChainWindow::requestClose()
{
    juce::MessageManager::callAsync ([w = juce::Component::SafePointer<PluginChainWindow> (this)]
    {
        if (w != nullptr && w->onClose)
            w->onClose (*w);
    });
}

void PluginChainWindow::closeButtonPressed()
{
    requestClose();
}

void PluginChainWindow::showAddMenu (juce::Component& target)
{
    const int slot = getSlot();
    juce::Component::SafePointer<PluginChainWindow> safe (this);
    auto menu = pluginTypesMenu (ctx, [&c = ctx, slot, safe] (const juce::PluginDescription& desc)
    {
        addPluginToChain (c, slot, desc);
        if (safe != nullptr)
            safe->view.refresh();
    });
    if (menu.getNumItems() == 0)
        menu.addItem ("Scan for plugins...", [&c = ctx] { if (c.showPluginScanner) c.showPluginScanner(); });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target));
}

void PluginChainWindow::removeMember (int index)
{
    // After the click: removing the last plugin closes (deletes) this window.
    const int slot = getSlot();
    juce::MessageManager::callAsync ([&c = ctx, slot, index, safe = juce::Component::SafePointer<PluginChainWindow> (this)]
    {
        removePluginFromChain (c, slot, index);
        if (safe != nullptr)
            safe->view.refresh();
    });
}

} // namespace livedaw::ui
