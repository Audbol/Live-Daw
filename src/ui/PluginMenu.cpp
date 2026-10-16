#include "ui/UiContext.h"

namespace livedaw::ui
{

juce::String describeStrip (const Strip& s)
{
    const auto name = s.getName();
    const auto label = s.getLabel();
    // Default names ("Ch 3") just repeat the number.
    if (name.isEmpty() || name.equalsIgnoreCase (label) || name.removeCharacters (" ").equalsIgnoreCase (label.removeCharacters (" ")))
        return label;
    return label + " " + name;
}

bool loadPluginIntoFx (UiContext& ctx, int slot, const juce::PluginDescription& desc)
{
    juce::String error;
    auto instance = ctx.host.createInstance (desc, ctx.engine.getSampleRate(), ctx.engine.getBlockSize(), error);

    if (instance == nullptr)
    {
        ctx.log.warning ("Could not load " + desc.name + " into FX " + juce::String (slot + 1) + ": " + error);
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                "Could not load plugin", desc.name + "\n" + error);
        return false;
    }

    auto& fx = ctx.engine.getFx();
    if (auto* old = fx.getPlugin (slot))
        if (ctx.pluginRemoved)
            ctx.pluginRemoved (old);

    fx.setPlugin (slot, std::move (instance));
    fx.clearFlags (slot);
    ctx.log.info ("Loaded " + desc.name + " into FX " + juce::String (slot + 1));
    return true;
}

void clearFx (UiContext& ctx, int slot)
{
    auto& fx = ctx.engine.getFx();
    if (auto* old = fx.getPlugin (slot))
    {
        if (ctx.pluginRemoved)
            ctx.pluginRemoved (old);

        fx.setPlugin (slot, nullptr);
        fx.clearFlags (slot);
    }
}

void showFxSlotMenu (UiContext& ctx, int slot, juce::Component* target)
{
    auto& fx = ctx.engine.getFx();
    juce::PopupMenu menu;
    auto* existing = fx.getPlugin (slot);

    menu.addSectionHeader ("FX " + juce::String (slot + 1) + (existing != nullptr ? ": " + existing->getName() : juce::String (" (empty)")));

    if (existing != nullptr)
    {
        menu.addItem ("Open editor", [&ctx, slot] { if (ctx.openPluginEditor) ctx.openPluginEditor (slot); });

        const bool bypassed = fx.params (slot).bypass.load();
        menu.addItem (bypassed ? "Un-bypass" : "Bypass", [&ctx, slot, bypassed]
        {
            ctx.engine.getFx().clearFlags (slot);
            ctx.engine.getFx().params (slot).bypass = ! bypassed;
        });

        if (fx.isFaulted (slot) || fx.isDoubleAssigned (slot))
            menu.addItem ("Clear fault / double-assignment", [&ctx, slot] { ctx.engine.getFx().clearFlags (slot); });

        menu.addItem ("Remove", [&ctx, slot] { clearFx (ctx, slot); });
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

        makerMenu.addItem (desc.name + " (" + desc.pluginFormatName + ")", [&ctx, slot, desc]
        {
            if (loadPluginIntoFx (ctx, slot, desc) && ctx.openPluginEditor)
                ctx.openPluginEditor (slot);
        });
    }
    flushMaker();

    menu.addSubMenu (existing != nullptr ? "Replace with" : "Load plugin (EXT)", plugins, ! types.isEmpty());
    menu.addItem ("Scan for plugins...", [&ctx] { if (ctx.showPluginScanner) ctx.showPluginScanner(); });

    auto options = juce::PopupMenu::Options();
    if (target != nullptr)
        options = options.withTargetComponent (target);
    menu.showMenuAsync (options);
}

void clickFxSlot (UiContext& ctx, int slot, juce::Component* target)
{
    if (ctx.engine.getFx().getPlugin (slot) != nullptr && ctx.openPluginEditor)
        ctx.openPluginEditor (slot);
    else
        showFxSlotMenu (ctx, slot, target);
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

} // namespace livedaw::ui
