#pragma once

#include "ui/ScreenWidgets.h"

namespace livedaw::ui
{

/** The left half of the LIBRARY screen with a show open (manual 5.6, fig. 45):
    the list toolbar (INT | USB | SHOW, REMOVE, RENAME, SKIP, LINK, UP, DOWN)
    and the show's scenes, each followed by its snippets (indented; a green bar
    links the ones that fire with their scene, and a scene to the one below it
    that it LINKs). Scene tags show as "#n", skipped scenes dimmed with the SKIP
    mark. Rows whose type doesn't match
    the open tab on the right are greyed, as on the console. */
class ShowPanel final : public juce::Component
{
public:
    explicit ShowPanel (UiContext&);
    ~ShowPanel() override;

    /** A row: a scene, or a snippet of a scene. */
    struct Item
    {
        int scene = -1, snippet = -1;
        bool isValid() const noexcept { return scene >= 0; }
        bool isSnippet() const noexcept { return snippet >= 0; }
        bool operator== (const Item& o) const noexcept { return scene == o.scene && snippet == o.snippet; }
    };

    Item getSelected() const;
    void select (Item);

    /** Which rows are shown normally: 0 all (SHOW), 1 scenes (SNAP), 2 snippets (SNIP), -1 none. */
    void setTypeFilter (int filter);

    /** Re-reads the show (call after it changed). */
    void refresh();

    std::function<void (Item)> onSelectionChanged;
    /** A row was double-clicked (GO on it). */
    std::function<void (Item)> onActivated;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class List;

    void removeSelected();
    void renameSelected();
    void moveSelected (int delta);
    void linkSelected();
    void skipSelected();
    void updateKeys();

    UiContext& ctx;
    std::unique_ptr<List> list;
    ToolKey intKey { "INT", Icon::Storage }, usbKey { "USB", Icon::UsbStick }, showKey { "SHOW", Icon::ShowFile },
        removeKey { "REMOVE", Icon::RemoveCircle }, renameKey { "RENAME", Icon::Edit }, skipKey { "SKIP", Icon::Skip, Dark::on },
        linkKey { "LINK", Icon::Link, FileColours::wav }, upKey { "UP", Icon::ChevronUp }, downKey { "DOWN", Icon::ChevronDown };
    juce::Rectangle<int> toolbar, listArea;
};

} // namespace livedaw::ui
