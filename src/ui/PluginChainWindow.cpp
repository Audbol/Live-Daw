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
              [this] { chainChanged(); } }),
      onClose (std::move (close))
{
    setUsingNativeTitleBar (true);
    setContentNonOwned (&view, true);
    chainChanged();
    centreWithSize (getWidth(), getHeight());
    setVisible (true);
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

    // A plugin on its own: resizable if its editor is (as a plugin window
    // always was); several scroll, so the window can be sized at will.
    setResizable (view.wantsResizableWindow(), false);
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
