#include "ui/UiContext.h"
#include "ui/WingPopups.h"
#include "engine/Models.h"

namespace livedaw::ui
{

juce::String stripName (const Strip& s)
{
    const auto name = s.getName();
    const auto label = s.getLabel();
    if (name.isEmpty() || name.removeCharacters (" ").equalsIgnoreCase (label.removeCharacters (" ")))
        return label;
    return name;
}

juce::String describeStrip (const Strip& s)
{
    const auto name = s.getName();
    const auto label = s.getLabel();
    // Default names ("Ch 3") just repeat the number.
    if (name.isEmpty() || name.equalsIgnoreCase (label) || name.removeCharacters (" ").equalsIgnoreCase (label.removeCharacters (" ")))
        return label;
    return label + " " + name;
}

void drawStripIcon (juce::Graphics& g, const Strip& s, juce::Rectangle<float> area, juce::Colour colour)
{
    // The strip's WING icon (/ch/n/icon), else its type's icon.
    if (! drawWingIcon (g, s.params.icon.load(), area, colour))
        drawIcon (g, stripIcon (s.getKind()), area, colour);
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

void showFxDeviceMenu (UiContext& ctx, int slot, juce::Component* target, std::function<void()> chosen)
{
    if (target == nullptr)
        return;

    // FX TYPE: the device list grouped as on the console (PREMIUM, STANDARD,
    // CHANNEL STRIPS; the group names are dim rows), then PLUGIN (EXT) and NONE.
    auto& fx = ctx.engine.getFx();
    const auto& devices = models::table (models::Fx);
    const int current = fx.getModel (slot);
    constexpr int kPlugin = -2, kNone = -1, kHeader = -3;
    juce::StringArray items;
    std::vector<int> values; // a device index, kPlugin, kNone or kHeader
    popups::ListOptions options;
    options.columns = 4;
    options.caption = "FX " + juce::String (slot + 1) + ": FX TYPE";
    int selected = -1;

    auto addGroup = [&] (models::Category category, const juce::String& title)
    {
        options.disabled.push_back (items.size());
        items.add (title);
        values.push_back (kHeader);
        for (const auto& m : devices)
        {
            if (m.category != category)
                continue;
            if (! FxSlots::isAllowed (m.index, slot))
                options.disabled.push_back (items.size());
            if (m.index == current)
                selected = items.size();
            items.add (m.name.toUpperCase());
            values.push_back (m.index);
        }
    };
    addGroup (models::Category::FxPremium, slot < 8 ? "PREMIUM FX" : "PREMIUM (FX 1-8 ONLY)");
    addGroup (models::Category::FxStandard, "STANDARD FX");
    addGroup (models::Category::FxChannel, "CHANNEL STRIPS");
    if (current == ld::kFxExt)
        selected = items.size();
    items.add ("PLUGIN (EXT)...");
    values.push_back (kPlugin);
    if (current == ld::kFxNone)
        selected = items.size();
    items.add ("NONE");
    values.push_back (kNone);

    juce::Component::SafePointer<juce::Component> safeTarget (target);
    popups::chooseFrom (*target, items, selected, [&ctx, slot, safeTarget, chosen, values] (int i)
    {
        if (! juce::isPositiveAndBelow (i, (int) values.size()) || values[(size_t) i] == kHeader)
            return;
        const int r = values[(size_t) i];
        auto& slots = ctx.engine.getFx();
        if (r == kPlugin)
        {
            showFxSlotMenu (ctx, slot, safeTarget.getComponent());
            return;
        }
        if (slots.hasPlugin (slot))
            clearFx (ctx, slot); // closes the plugin's editor first
        slots.setModel (slot, r == kNone ? ld::kFxNone : r);
        if (chosen)
            chosen();
    }, options);
}

void clickFxSlot (UiContext& ctx, int slot, juce::Component* target)
{
    auto& fx = ctx.engine.getFx();
    if (fx.getPlugin (slot) != nullptr && ctx.openPluginEditor)
        ctx.openPluginEditor (slot);
    else if (fx.getModel (slot) > ld::kFxExt && ctx.editFx)
        ctx.editFx (slot);
    else
        showFxDeviceMenu (ctx, slot, target, [&ctx, slot] { if (ctx.editFx) ctx.editFx (slot); });
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
