#include "ui/UiContext.h"

namespace livedaw::ui
{

bool loadPluginIntoSlot (UiContext& ctx, Strip& strip, int slot, const juce::PluginDescription& desc)
{
    juce::String error;
    auto instance = ctx.host.createInstance (desc, ctx.engine.getSampleRate(), ctx.engine.getBlockSize(), error);

    if (instance == nullptr)
    {
        ctx.log.warning ("Could not load " + desc.name + " on " + strip.getName() + ": " + error);
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                "Could not load plugin", desc.name + "\n" + error);
        return false;
    }

    if (auto* old = strip.inserts.getPlugin (slot))
        if (ctx.pluginRemoved)
            ctx.pluginRemoved (old);

    strip.inserts.setPlugin (slot, std::move (instance));
    ctx.log.info ("Loaded " + desc.name + " on " + strip.getName() + " insert " + juce::String (slot + 1));
    return true;
}

void clearSlot (UiContext& ctx, Strip& strip, int slot)
{
    if (auto* old = strip.inserts.getPlugin (slot))
    {
        if (ctx.pluginRemoved)
            ctx.pluginRemoved (old);

        strip.inserts.setPlugin (slot, nullptr);
    }
}

void showInsertSlotMenu (UiContext& ctx, Strip& strip, int slot, juce::Component* target)
{
    // Menus are asynchronous: resolve the strip again when an item is chosen, in
    // case it was removed while the menu was open.
    const auto ref = state::StripRef::of (strip);
    auto withStrip = [&ctx, ref] (auto fn)
    {
        return [&ctx, ref, fn]
        {
            if (auto* s = state::findStrip (ctx.engine, ref))
                fn (*s);
        };
    };

    juce::PopupMenu menu;
    auto* existing = strip.inserts.getPlugin (slot);

    if (existing != nullptr)
    {
        menu.addSectionHeader (existing->getName());
        menu.addItem ("Open editor", withStrip ([&ctx, slot] (Strip& s) { if (ctx.openPluginEditor) ctx.openPluginEditor (s, slot); }));

        const bool bypassed = strip.inserts.isBypassed (slot);
        menu.addItem (bypassed ? "Un-bypass" : "Bypass", withStrip ([slot, bypassed] (Strip& s)
        {
            s.inserts.clearFault (slot);
            s.inserts.setBypassed (slot, ! bypassed);
        }));

        if (strip.inserts.isFaulted (slot))
            menu.addItem ("Clear fault (keep bypassed)", withStrip ([slot] (Strip& s) { s.inserts.clearFault (slot); }));

        menu.addItem ("Remove", withStrip ([&ctx, slot] (Strip& s) { clearSlot (ctx, s, slot); }));
        menu.addSeparator();
    }

    juce::PopupMenu plugins;
    const auto types = ctx.host.getSortedTypes();
    juce::String currentMaker;
    juce::PopupMenu makerMenu;

    auto flushMaker = [&]
    {
        if (currentMaker.isNotEmpty())
            plugins.addSubMenu (currentMaker, makerMenu);
        makerMenu = {};
    };

    for (const auto& desc : types)
    {
        if (desc.isInstrument)
            continue;

        const auto maker = desc.manufacturerName.isEmpty() ? juce::String ("Unknown") : desc.manufacturerName;
        if (maker != currentMaker)
        {
            flushMaker();
            currentMaker = maker;
        }

        makerMenu.addItem (desc.name + " (" + desc.pluginFormatName + ")", withStrip ([&ctx, slot, desc] (Strip& s)
        {
            if (loadPluginIntoSlot (ctx, s, slot, desc) && ctx.openPluginEditor)
                ctx.openPluginEditor (s, slot);
        }));
    }
    flushMaker();

    menu.addSubMenu (existing != nullptr ? "Replace with" : "Insert plugin", plugins, ! types.isEmpty());
    menu.addItem ("Scan for plugins...", [&ctx] { if (ctx.showPluginScanner) ctx.showPluginScanner(); });

    auto options = juce::PopupMenu::Options();
    if (target != nullptr)
        options = options.withTargetComponent (target);
    menu.showMenuAsync (options);
}

void logRecall (UiContext& ctx, const juce::String& what, const state::ApplyResult& r)
{
    juce::String msg = "Recalled " + what;
    if (r.stripsSkippedSafe > 0)
        msg << " (" << r.stripsSkippedSafe << " recall safe)";
    if (r.stripsMissing > 0)
        msg << " (" << r.stripsMissing << " strips no longer exist)";
    ctx.log.info (msg);

    for (auto& w : r.warnings)
        ctx.log.warning (w);
}

void clickInsertSlot (UiContext& ctx, Strip& strip, int slot, juce::Component* target)
{
    if (strip.inserts.getPlugin (slot) != nullptr && ctx.openPluginEditor)
        ctx.openPluginEditor (strip, slot);
    else
        showInsertSlotMenu (ctx, strip, slot, target);
}

} // namespace livedaw::ui
