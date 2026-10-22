#include "ui/UiContext.h"
#include "ui/WingPopups.h"
#include "engine/Models.h"
#include "ui/Faceplate.h"
#include "ui/LookAndFeel.h"

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
    if (! drawWingIcon (g, s.getIcon(), area, colour))
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

bool addPluginToChain (UiContext& ctx, int slot, const juce::PluginDescription& desc)
{
    auto& fx = ctx.engine.getFx();
    if (fx.getNumChainMembers (slot) >= PluginChain::kMaxMembers)
        return false;

    juce::String error;
    auto instance = ctx.host.createInstance (desc, ctx.engine.getSampleRate(), ctx.engine.getBlockSize(), error);
    if (instance == nullptr)
    {
        ctx.log.warning ("Could not add " + desc.name + " to FX " + juce::String (slot + 1) + ": " + error);
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                "Could not load plugin", desc.name + "\n" + error);
        return false;
    }

    if (! fx.hasPlugin (slot))
        fx.setPlugin (slot, std::move (instance)); // the first one
    else if (! fx.addToChain (slot, std::move (instance)))
        return false;
    ctx.log.info ("Added " + desc.name + " to FX " + juce::String (slot + 1) + " (" + juce::String (fx.getNumChainMembers (slot))
                  + (fx.getNumChainMembers (slot) == 1 ? " plugin)" : " plugins in its chain)"));
    return true;
}

void removePluginFromChain (UiContext& ctx, int slot, int index)
{
    auto& fx = ctx.engine.getFx();
    auto* plugin = fx.getChainMember (slot, index);
    if (plugin == nullptr)
        return;
    if (fx.getNumChainMembers (slot) == 1)
    {
        clearFx (ctx, slot); // the last one: the slot is empty (its window closes)
        return;
    }

    const auto name = plugin->getName();
    if (ctx.pluginRemoved)
        ctx.pluginRemoved (plugin); // its editor goes first
    fx.removeFromChain (slot, index);
    ctx.log.info ("Removed " + name + " from FX " + juce::String (slot + 1));
}

juce::PopupMenu pluginTypesMenu (UiContext& ctx, std::function<void (const juce::PluginDescription&)> chosen)
{
    // The scanned effects by manufacturer.
    juce::PopupMenu plugins;
    juce::String currentMaker;
    juce::PopupMenu makerMenu;

    auto flushMaker = [&]
    {
        if (currentMaker.isNotEmpty())
            plugins.addSubMenu (currentMaker, makerMenu);
        makerMenu = {};
    };

    for (const auto& desc : ctx.host.getSortedTypes())
    {
        if (desc.isInstrument)
            continue;

        const auto maker = desc.manufacturerName.isEmpty() ? juce::String ("Unknown") : desc.manufacturerName;
        if (maker != currentMaker)
        {
            flushMaker();
            currentMaker = maker;
        }

        makerMenu.addItem (desc.name + " (" + desc.pluginFormatName + ")", [chosen, desc] { chosen (desc); });
    }
    flushMaker();
    return plugins;
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

        const int members = fx.getNumChainMembers (slot);
        menu.addItem (members > 1 ? "Remove all " + juce::String (members) + " plugins" : juce::String ("Remove"), [&ctx, slot] { clearFx (ctx, slot); });
        menu.addSeparator();
    }

    const auto types = ctx.host.getSortedTypes();
    auto plugins = pluginTypesMenu (ctx, [&ctx, slot] (const juce::PluginDescription& desc)
    {
        if (loadPluginIntoFx (ctx, slot, desc) && ctx.openPluginEditor)
            ctx.openPluginEditor (slot);
    });

    if (existing != nullptr)
    {
        // Another plugin after the slot's (a chain of up to 8, in one window).
        menu.addSubMenu ("Add to chain", pluginTypesMenu (ctx, [&ctx, slot] (const juce::PluginDescription& desc)
                         {
                             if (addPluginToChain (ctx, slot, desc) && ctx.openPluginEditor)
                                 ctx.openPluginEditor (slot);
                         }),
                         ! types.isEmpty() && fx.getNumChainMembers (slot) < PluginChain::kMaxMembers);
    }
    menu.addSubMenu (existing != nullptr ? "Replace with" : "Load plugin (EXT)", plugins, ! types.isEmpty());
    menu.addItem ("Scan for plugins...", [&ctx] { if (ctx.showPluginScanner) ctx.showPluginScanner(); });

    auto options = juce::PopupMenu::Options();
    if (target != nullptr)
        options = options.withTargetComponent (target);
    menu.showMenuAsync (options);
}

void showFxDeviceMenu (UiContext& ctx, int slot, juce::Component* target, std::function<void()> chosen, juce::Component* over)
{
    if (target == nullptr)
        return;
    if (over == nullptr)
        over = target->getParentComponent() != nullptr ? target->getParentComponent() : target;

    // FX TYPE (3.1): NONE and EXTERNAL (here: a plugin) on every tab, then the
    // devices of the tab shown. The standard slots (9-16) can't load the
    // premium devices: their FX1-8 tab is dim and the list opens on STD.
    auto& fx = ctx.engine.getFx();
    const int current = fx.hasPlugin (slot) ? ld::kFxExt : fx.getModel (slot);
    const auto caption = [] (int model)
    {
        return model == ld::kFxExt ? juce::String ("EXTERNAL") : models::model (models::Fx, model).name.toUpperCase();
    };
    std::vector<popups::Tile> common { { ld::kFxNone, "NONE" }, { ld::kFxExt, caption (ld::kFxExt) } };
    std::vector<popups::TileTab> tabs;
    int shown = -1;
    for (const auto& tab : models::fxMenuTabs())
    {
        popups::TileTab t { tab.title, tab.subtitle, tab.premium ? std::optional<Icon> (Icon::Premium) : std::nullopt, {}, ! tab.premium || slot < 8 };
        for (const auto& row : tab.rows)
        {
            auto& r = t.rows.emplace_back();
            for (const int model : row)
            {
                r.push_back ({ model, caption (model) });
                if (model == current && t.enabled)
                    shown = (int) tabs.size();
            }
        }
        tabs.push_back (std::move (t));
    }
    if (shown < 0)
        shown = slot < 8 ? 0 : 1;

    auto picture = [] (juce::Graphics& g, juce::Rectangle<float> r, int model)
    {
        if (model == ld::kFxNone)
            drawIcon (g, Icon::Cancel, r.withSizeKeepingCentre (r.getHeight() * 0.67f, r.getHeight() * 0.67f), Colours::caption);
        else
            Faceplate::paintThumbnail (g, r, models::Fx, model, model == ld::kFxExt ? juce::String ("PLUGIN") : juce::String());
    };
    juce::Component::SafePointer<juce::Component> safeTarget (target);
    popups::chooseTabbedTile (*target, *over, std::move (common), std::move (tabs), shown, current, picture,
                              [&ctx, slot, safeTarget, chosen] (int model)
                              {
                                  auto& slots = ctx.engine.getFx();
                                  if (model == ld::kFxExt)
                                  {
                                      showFxSlotMenu (ctx, slot, safeTarget.getComponent());
                                      return;
                                  }
                                  if (! FxSlots::isAllowed (model, slot))
                                      return;
                                  if (slots.hasPlugin (slot))
                                      clearFx (ctx, slot); // closes the plugin's editor first
                                  slots.setModel (slot, model);
                                  if (chosen)
                                      chosen();
                              });
}

void showFxProcessorWindow (UiContext& ctx, juce::Component& over, int current, std::function<void (int)> chosen)
{
    // Each slot: who uses it (the first strip with it on an insert), its
    // device, or "- FREE -".
    auto& fx = ctx.engine.getFx();
    std::vector<popups::RackSlot> slots;
    std::vector<int> icons; // the users' WING icons (0: none)
    for (int i = 0; i < kNumFx; ++i)
    {
        popups::RackSlot slot { "- FREE -", {}, {} };
        const Strip* user = nullptr;
        bool post = false;
        for (auto kind : { StripKind::Channel, StripKind::Aux, StripKind::Bus, StripKind::Main, StripKind::Matrix })
            for (auto& s : ctx.engine.getStrips (kind))
                if (user == nullptr && (s->params.preInsert.fx.load() == i || (s->params.postInsert.fx.load() == i && s->params.postInsert.mode.load() == 0)))
                {
                    user = s.get();
                    post = s->params.preInsert.fx.load() != i;
                }
        if (user != nullptr)
        {
            slot.source = user->getLabel();
            slot.sourceColour = stripDisplayColour (*user);
            slot.caption = post ? "INSERT POST" : "INSERT PRE";
        }
        if (auto* plugin = fx.getPlugin (i))
            slot.caption = plugin->getName().toUpperCase();
        else if (fx.getModel (i) > ld::kFxExt)
            slot.caption = models::model (models::Fx, fx.getModel (i)).name.toUpperCase();
        slots.push_back (slot);
        icons.push_back (user != nullptr ? user->params.icon.load() : 0);
    }

    popups::chooseRackSlot (
        over, std::move (slots), current,
        [&ctx] (juce::Graphics& g, juce::Rectangle<float> r, int i)
        {
            auto& rack = ctx.engine.getFx();
            const int model = rack.getModel (i);
            if (rack.hasPlugin (i))
                Faceplate::paintThumbnail (g, r, models::Fx, ld::kFxExt, "PLUGIN");
            else if (model > ld::kFxExt)
                Faceplate::paintThumbnail (g, r, models::Fx, model, {});
            else
                return false;
            return true;
        },
        [icons] (juce::Graphics& g, juce::Rectangle<float> r, int i) { drawWingIcon (g, icons[(size_t) i], r, juce::Colours::white); },
        std::move (chosen));
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
