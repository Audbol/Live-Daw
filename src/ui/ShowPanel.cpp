#include "ui/ShowPanel.h"
#include "engine/AppPaths.h"

namespace livedaw::ui
{

//==============================================================================
class ShowPanel::List final : public juce::ListBox, private juce::ListBoxModel
{
public:
    explicit List (ShowPanel& p) : panel (p)
    {
        setModel (this);
        setColour (backgroundColourId, juce::Colours::black);
        setColour (outlineColourId, juce::Colours::transparentBlack);
        getVerticalScrollBar().setColour (juce::ScrollBar::thumbColourId, Dark::key);
        getVerticalScrollBar().setColour (juce::ScrollBar::backgroundColourId, Dark::body);
    }

    ~List() override { setModel (nullptr); }

    std::vector<Item> rows;
    int filter = 0;

    void rebuild()
    {
        rows.clear();
        auto& show = panel.ctx.show;
        for (int i = 0; i < show.getNumScenes(); ++i)
        {
            rows.push_back ({ i, -1 });
            if (const auto scene = show.getScene (i))
                for (int j = 0; j < scene->numSnippets; ++j)
                    rows.push_back ({ i, j });
        }
        updateContent();
        repaint();
    }

    int rowOf (Item item) const
    {
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i] == item)
                return (int) i;
        return -1;
    }

    Item itemAt (int row) const { return juce::isPositiveAndBelow (row, (int) rows.size()) ? rows[(size_t) row] : Item {}; }

private:
    int getNumRows() override { return (int) rows.size(); }

    /** Does a later snippet of the same scene (after row `row`) fire with it? */
    bool linkContinues (int row) const
    {
        const auto item = itemAt (row);
        for (int r = row + 1; r < (int) rows.size() && rows[(size_t) r].scene == item.scene; ++r)
            if (const auto sn = panel.ctx.show.getSnippet (item.scene, rows[(size_t) r].snippet); sn && sn->fireWithScene)
                return true;
        return false;
    }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool rowSelected) override
    {
        const auto item = itemAt (row);
        if (! item.isValid())
            return;
        auto& show = panel.ctx.show;
        const auto scene = show.getScene (item.scene);
        if (! scene)
            return;
        const auto snippet = item.isSnippet() ? show.getSnippet (item.scene, item.snippet) : std::nullopt;
        if (item.isSnippet() && ! snippet)
            return;

        const bool normal = filter == 0 || (filter == 1 && ! item.isSnippet()) || (filter == 2 && item.isSnippet());
        auto r = juce::Rectangle<int> (width, height).toFloat();
        g.setColour (Colours::screenPanel);
        g.fillRect (r.removeFromBottom (1.0f));
        if (rowSelected)
        {
            g.setColour (Dark::selectedRow);
            g.fillRect (r.reduced (1.0f));
            g.setColour (Dark::selectedOutline);
            g.drawRect (r.reduced (1.0f), 2.0f);
        }

        auto t = r.reduced (8.0f, 2.0f);
        auto bars = t.removeFromRight (12.0f); // the LINK bars, right of the chips
        if (item.isSnippet())
            t.removeFromLeft (24.0f);

        // The type chip with the scene number under it.
        const float chipW = juce::jmin (46.0f, t.getWidth() * 0.16f), chipH = juce::jlimit (12.0f, 18.0f, t.getHeight() * 0.4f);
        auto chipColumn = t.removeFromRight (chipW + 2.0f);
        const auto chipColour = item.isSnippet() ? FileColours::snip : FileColours::snap;
        auto chip = chipColumn.withHeight (chipH).withY (t.getY() + 2.0f);
        drawChip (g, chip, item.isSnippet() ? "SNIP" : "SNAP", chipColour.withMultipliedAlpha (normal ? 1.0f : 0.45f), juce::Colours::black);
        g.setColour (Dark::textDim);
        g.setFont (wingFontFit (TypeScale::statusCaption * 1.2f, t.getHeight() * 0.3f));
        const juce::String number = juce::String (item.scene + 1) + (item.isSnippet() ? "." + juce::String (item.snippet + 1) : juce::String());
        g.drawText (number, chipColumn.withTop (chip.getBottom()).toNearestInt(), juce::Justification::centredRight);

        // Snippets that fire with their scene: a green bar down the chip column (the console's LINK).
        const float barX = bars.getX() + 1.0f;
        g.setColour (FileColours::wav);
        if (! item.isSnippet() && linkContinues (row))
            g.fillRect (barX, r.getCentreY(), 4.0f, r.getBottom() + 1.0f - r.getCentreY());
        else if (item.isSnippet())
        {
            const bool linked = snippet->fireWithScene;
            const bool continues = linkContinues (row);
            if (linked || continues)
                g.fillRect (barX, r.getY(), 4.0f, (continues ? r.getBottom() + 1.0f : r.getCentreY()) - r.getY());
        }

        // A scene that LINKs to the scene below: a green bar joins them, and the
        // linked scene is indented.
        const auto previous = item.scene > 0 ? show.getScene (item.scene - 1) : std::nullopt;
        const bool linkedFromAbove = ! item.isSnippet() && previous && previous->linkNext;
        const float sceneBarX = bars.getX() + 7.0f;
        if (scene->linkNext && item.scene < show.getNumScenes() - 1)
            g.fillRect (sceneBarX, item.isSnippet() ? r.getY() : r.getCentreY(), 4.0f, item.isSnippet() ? r.getHeight() + 1.0f : r.getBottom() + 1.0f - r.getCentreY());
        if (linkedFromAbove)
        {
            g.fillRect (sceneBarX, r.getY(), 4.0f, r.getCentreY() - r.getY());
            t.removeFromLeft (16.0f);
        }

        if (item.isSnippet())
        {
            g.setColour (Dark::textDim.withMultipliedAlpha (normal ? 1.0f : 0.5f));
            g.setFont (wingFontFit (TypeScale::statusCaption, t.getHeight() * 0.28f));
            // "N PARS": what REC recorded, else the values the snippet holds.
            g.drawText (juce::String (snippet->parameterCount) + " PARS", t.removeFromRight (60.0f).withHeight (chipH + 4.0f).toNearestInt(),
                        juce::Justification::centredRight);
        }
        else if (scene->fadeSeconds > 0.0)
        {
            g.setColour (Dark::textDim.withMultipliedAlpha (normal ? 1.0f : 0.5f));
            g.setFont (wingFontFit (TypeScale::statusCaption, t.getHeight() * 0.28f));
            g.drawText ("FADE " + juce::String (scene->fadeSeconds, 1) + " s", t.removeFromRight (70.0f).withHeight (chipH + 4.0f).toNearestInt(),
                        juce::Justification::centredRight);
        }

        // The active (last recalled) scene.
        if (! item.isSnippet() && item.scene == show.getCurrentSceneIndex())
        {
            auto marker = t.removeFromLeft (16.0f);
            drawIcon (g, Icon::Play, marker.withSizeKeepingCentre (12.0f, 12.0f), FileColours::wav);
            t.removeFromLeft (4.0f);
        }

        if (! item.isSnippet())
        {
            // The scene tag (#1..#128) in a small blue chip at the top left.
            if (scene->tag > 0)
            {
                drawChip (g, juce::Rectangle<float> (t.getX(), t.getY() + 1.0f, 30.0f, juce::jlimit (10.0f, 14.0f, t.getHeight() * 0.32f)),
                          "#" + juce::String (scene->tag), Dark::choice.withMultipliedAlpha (normal ? 1.0f : 0.5f), juce::Colours::black);
                t.removeFromTop (juce::jlimit (6.0f, 10.0f, t.getHeight() * 0.22f));
            }
            // A skipped scene: the SKIP mark, and the name dimmed.
            if (scene->skip)
            {
                drawIcon (g, Icon::Skip, t.removeFromLeft (16.0f).withSizeKeepingCentre (12.0f, 12.0f), Dark::on.withMultipliedAlpha (normal ? 1.0f : 0.5f));
                t.removeFromLeft (4.0f);
            }
        }

        const bool dim = ! normal || (! item.isSnippet() && scene->skip);
        g.setColour (dim ? Dark::textDim.withAlpha (0.6f) : Dark::text);
        g.setFont (wingFontFit (TypeScale::value * 1.15f, t.getHeight() * 0.62f, WingWeight::SemiBold));
        g.drawFittedText ((item.isSnippet() ? snippet->name : scene->name).toUpperCase(), t.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
    }

    void selectedRowsChanged (int) override
    {
        panel.updateKeys();
        if (panel.onSelectionChanged)
            panel.onSelectionChanged (panel.getSelected());
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        if (panel.onActivated)
            panel.onActivated (itemAt (row));
    }

    ShowPanel& panel;
};

//==============================================================================
/** The INT / USB file list: folders first (".." to go up), then the shows and
    presets, each with its type chip and date. */
class ShowPanel::FileList final : public juce::ListBox, private juce::ListBoxModel
{
public:
    explicit FileList (ShowPanel& p) : panel (p)
    {
        setModel (this);
        setColour (backgroundColourId, juce::Colours::black);
        setColour (outlineColourId, juce::Colours::transparentBlack);
        getVerticalScrollBar().setColour (juce::ScrollBar::thumbColourId, Dark::key);
        getVerticalScrollBar().setColour (juce::ScrollBar::backgroundColourId, Dark::body);
    }

    ~FileList() override { setModel (nullptr); }

    struct Entry
    {
        juce::File file;
        bool parent = false, folder = false;
    };

    juce::File root, folder;
    juce::String filter;
    bool byDate = false;
    std::vector<Entry> entries;

    /** The file types the library lists, with their chip text and colour. */
    static bool typeOf (const juce::File& f, juce::String& chip, juce::Colour& colour)
    {
        const auto ext = f.getFileExtension().toLowerCase();
        if (ext == Show::fileExtension)
            chip = "SHOW", colour = FileColours::show;
        else if (ext == Show::fxPresetExtension)
            chip = "FX", colour = FileColours::fx;
        else if (ext == Show::channelPresetExtension)
            chip = "CHN", colour = FileColours::chan;
        else if (ext == ".snap")
            chip = "SNAP", colour = FileColours::snap;
        else
            return false;
        return true;
    }

    void rebuild()
    {
        const auto previous = selectedFile();
        entries.clear();
        if (folder.isDirectory())
        {
            if (folder != root && folder.isAChildOf (root))
                entries.push_back ({ folder.getParentDirectory(), true, true });
            std::vector<Entry> dirs, items;
            for (const auto& f : folder.findChildFiles (juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles, false))
            {
                juce::String chip;
                juce::Colour colour;
                if (f.isDirectory())
                    dirs.push_back ({ f, false, true });
                else if (typeOf (f, chip, colour))
                    items.push_back ({ f, false, false });
            }
            auto byName = [] (const Entry& a, const Entry& b) { return a.file.getFileName().compareNatural (b.file.getFileName()) < 0; };
            std::sort (dirs.begin(), dirs.end(), byName);
            std::sort (items.begin(), items.end(), [this, byName] (const Entry& a, const Entry& b)
            {
                if (byDate)
                    return a.file.getLastModificationTime() > b.file.getLastModificationTime();
                const auto ta = a.file.getFileExtension().toLowerCase(), tb = b.file.getFileExtension().toLowerCase();
                return ta != tb ? ta < tb : byName (a, b);
            });
            entries.insert (entries.end(), dirs.begin(), dirs.end());
            entries.insert (entries.end(), items.begin(), items.end());
        }
        updateContent();
        selectFile (previous);
        repaint();
    }

    juce::File selectedFile() const
    {
        const int row = getSelectedRow();
        if (! juce::isPositiveAndBelow (row, (int) entries.size()) || entries[(size_t) row].folder)
            return {};
        return entries[(size_t) row].file;
    }

    void selectFile (const juce::File& f)
    {
        for (size_t i = 0; i < entries.size(); ++i)
            if (! entries[i].parent && entries[i].file == f)
            {
                selectRow ((int) i);
                return;
            }
        deselectAllRows();
    }

    void open (const juce::File& dir)
    {
        folder = dir;
        rebuild();
        deselectAllRows();
    }

private:
    int getNumRows() override { return (int) entries.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool rowSelected) override
    {
        if (! juce::isPositiveAndBelow (row, (int) entries.size()))
            return;
        const auto& e = entries[(size_t) row];
        auto r = juce::Rectangle<int> (width, height).toFloat();
        g.setColour (Colours::screenPanel);
        g.fillRect (r.removeFromBottom (1.0f));
        if (rowSelected)
        {
            g.setColour (Dark::selectedRow);
            g.fillRect (r.reduced (1.0f));
            g.setColour (Dark::selectedOutline);
            g.drawRect (r.reduced (1.0f), 2.0f);
        }
        auto t = r.reduced (8.0f, 2.0f);

        if (e.folder)
        {
            drawIcon (g, Icon::Folder, t.removeFromLeft (t.getHeight() * 0.8f).reduced (2.0f), Dark::textDim);
            t.removeFromLeft (6.0f);
            g.setColour (Dark::text);
            g.setFont (wingFontFit (TypeScale::value * 1.15f, t.getHeight() * 0.62f, WingWeight::SemiBold));
            g.drawFittedText (e.parent ? juce::String ("..") : e.file.getFileName().toUpperCase(), t.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
            return;
        }

        juce::String chipText;
        juce::Colour colour;
        typeOf (e.file, chipText, colour);
        const bool normal = filter.isNotEmpty() && e.file.hasFileExtension (filter);
        // The type chip with the date and time under it.
        const float chipW = juce::jmin (46.0f, t.getWidth() * 0.16f), chipH = juce::jlimit (12.0f, 18.0f, t.getHeight() * 0.4f);
        auto dateColumn = t.removeFromRight (juce::jmax (chipW, 74.0f));
        auto chip = dateColumn.withWidth (chipW).withHeight (chipH).withY (t.getY() + 2.0f).withX (dateColumn.getRight() - chipW);
        drawChip (g, chip, chipText, colour.withMultipliedAlpha (normal ? 1.0f : 0.45f), juce::Colours::black);
        const auto time = e.file.getLastModificationTime();
        g.setColour (Dark::textDim);
        g.setFont (wingFontFit (TypeScale::statusCaption, t.getHeight() * 0.25f));
        g.drawText (time.formatted ("%Y-%m-%d  %H:%M:%S"), dateColumn.withTop (chip.getBottom()).toNearestInt(), juce::Justification::centredRight);

        g.setColour (normal ? Dark::text : Dark::textDim.withAlpha (0.6f));
        g.setFont (wingFontFit (TypeScale::value * 1.15f, t.getHeight() * 0.62f, WingWeight::SemiBold));
        g.drawFittedText (e.file.getFileNameWithoutExtension().toUpperCase(), t.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
    }

    void selectedRowsChanged (int) override
    {
        panel.updateKeys();
        if (panel.onFileSelected)
            panel.onFileSelected (selectedFile());
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        if (! juce::isPositiveAndBelow (row, (int) entries.size()))
            return;
        const auto e = entries[(size_t) row];
        if (e.folder)
        {
            open (e.file);
            panel.updateKeys();
            if (panel.onFileSelected)
                panel.onFileSelected ({});
        }
        else if (panel.onFileActivated)
            panel.onFileActivated (e.file);
    }

    ShowPanel& panel;
};

//==============================================================================
namespace
{
    constexpr const char* kUsbFolder = "library.usbFolder";
} // namespace

juce::File ShowPanel::libraryFolder()
{
    return paths::documents().getChildFile ("Library");
}

ShowPanel::ShowPanel (UiContext& c) : ctx (c)
{
    list = std::make_unique<List> (*this);
    addAndMakeVisible (*list);
    files = std::make_unique<FileList> (*this);
    addChildComponent (*files);

    // The locations: INT (the library folder), USB (a folder of your choice) and the open SHOW.
    intKey.litColour = Dark::on;
    intKey.isLit = [this] { return location == Location::Internal; };
    intKey.onClick = [this] { setLocation (Location::Internal); };
    usbKey.litColour = Dark::on;
    usbKey.isLit = [this] { return location == Location::Usb; };
    usbKey.onClick = [this]
    {
        const juce::File folder (ctx.settings.getValue (kUsbFolder));
        if (location == Location::Usb || ! folder.isDirectory())
            chooseUsbFolder();
        else
            setLocation (Location::Usb);
    };
    showKey.litColour = FileColours::show;
    showKey.isLit = [this] { return location == Location::Show; };
    showKey.onClick = [this] { setLocation (Location::Show); };
    showKey.caption = [this] { return juce::String (ctx.show.getNumScenes()) + " SCNS"; };
    folderKey.onClick = [this] { newFolder(); };
    fileRenameKey.onClick = [this] { renameFile(); };
    deleteKey.onClick = [this] { deleteFile(); };
    sortTypeKey.litColour = Dark::choice;
    sortTypeKey.isLit = [this] { return ! files->byDate; };
    sortTypeKey.onClick = [this] { files->byDate = false; files->rebuild(); };
    sortDateKey.litColour = Dark::choice;
    sortDateKey.isLit = [this] { return files->byDate; };
    sortDateKey.onClick = [this] { files->byDate = true; files->rebuild(); };
    for (auto* k : { &folderKey, &fileRenameKey, &deleteKey, &sortTypeKey, &sortDateKey })
        addChildComponent (k);
    skipKey.litColour = Dark::on;
    skipKey.isLit = [this]
    {
        const auto i = getSelected();
        const auto sc = i.isValid() && ! i.isSnippet() ? ctx.show.getScene (i.scene) : std::nullopt;
        return sc && sc->skip;
    };
    skipKey.onClick = [this] { skipSelected(); };

    removeKey.onClick = [this] { removeSelected(); };
    renameKey.onClick = [this] { renameSelected(); };
    linkKey.onClick = [this] { linkSelected(); };
    upKey.onClick = [this] { moveSelected (-1); };
    downKey.onClick = [this] { moveSelected (1); };

    for (auto* k : { &intKey, &usbKey, &showKey, &removeKey, &renameKey, &skipKey, &linkKey, &upKey, &downKey })
        addAndMakeVisible (k);
    refresh();
}

ShowPanel::~ShowPanel() = default;

ShowPanel::Item ShowPanel::getSelected() const
{
    return list->itemAt (list->getSelectedRow());
}

void ShowPanel::select (Item item)
{
    const int row = list->rowOf (item);
    if (row >= 0)
    {
        list->selectRow (row);
        list->scrollToEnsureRowIsOnscreen (row);
    }
    else
        list->deselectAllRows();
}

void ShowPanel::setTypeFilter (int filter)
{
    list->filter = filter;
    list->repaint();
}

void ShowPanel::setFileFilter (const juce::String& extension)
{
    files->filter = extension.trimCharactersAtStart (".");
    files->repaint();
}

void ShowPanel::setLocation (Location l)
{
    if (l == Location::Usb && ! juce::File (ctx.settings.getValue (kUsbFolder)).isDirectory())
    {
        chooseUsbFolder();
        return;
    }
    const bool changed = l != location;
    location = l;
    if (location != Location::Show)
    {
        const auto root = location == Location::Internal ? libraryFolder() : juce::File (ctx.settings.getValue (kUsbFolder));
        if (location == Location::Internal)
            root.createDirectory();
        if (files->root != root)
        {
            files->root = root;
            files->open (root);
        }
        else
            files->rebuild();
    }
    list->setVisible (location == Location::Show);
    files->setVisible (location != Location::Show);
    for (auto* k : { &removeKey, &renameKey, &skipKey, &linkKey, &upKey, &downKey })
        k->setVisible (location == Location::Show);
    for (auto* k : { &folderKey, &fileRenameKey, &deleteKey, &sortTypeKey, &sortDateKey })
        k->setVisible (location != Location::Show);
    updateKeys();
    repaint();
    if (changed && onLocationChanged)
        onLocationChanged();
}

juce::File ShowPanel::getFolder() const
{
    return location == Location::Show ? libraryFolder() : files->folder;
}

juce::File ShowPanel::getSelectedFile() const
{
    return location == Location::Show ? juce::File() : files->selectedFile();
}

void ShowPanel::selectFile (const juce::File& f)
{
    files->rebuild();
    files->selectFile (f);
}

void ShowPanel::chooseUsbFolder()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose the USB folder", juce::File (ctx.settings.getValue (kUsbFolder)));
    juce::Component::SafePointer<ShowPanel> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [safe] (const juce::FileChooser& fc)
    {
        const auto dir = fc.getResult();
        if (safe == nullptr || ! dir.isDirectory())
            return;
        safe->ctx.settings.setValue (kUsbFolder, dir.getFullPathName());
        safe->files->root = juce::File();
        safe->setLocation (Location::Usb);
    });
}

void ShowPanel::newFolder()
{
    juce::Component::SafePointer<ShowPanel> safe (this);
    popups::TextOptions options;
    options.icon = Icon::Folder;
    options.maxLength = 32;
    popups::editText (popupHost(), "NEW FOLDER", "NEW FOLDER", [safe] (const juce::String& text)
    {
        const auto name = juce::File::createLegalFileName (text.trim());
        if (safe == nullptr || name.isEmpty())
            return;
        const auto dir = safe->files->folder.getChildFile (name);
        if (! dir.createDirectory())
            safe->ctx.log.warning ("Could not create the folder " + dir.getFullPathName());
        safe->files->rebuild();
    }, options);
}

void ShowPanel::renameFile()
{
    const auto file = files->selectedFile();
    if (! file.existsAsFile())
        return;
    juce::Component::SafePointer<ShowPanel> safe (this);
    popups::TextOptions options;
    options.maxLength = 32;
    popups::editText (popupHost(), "RENAME", file.getFileNameWithoutExtension(), [safe, file] (const juce::String& text)
    {
        const auto name = juce::File::createLegalFileName (text.trim());
        if (safe == nullptr || name.isEmpty())
            return;
        const auto target = file.getSiblingFile (name + file.getFileExtension());
        if (target.exists() || ! file.moveFileTo (target))
        {
            popups::warn (safe->popupHost(), "RENAME", "Could not rename " + file.getFileName() + " to " + target.getFileName());
            return;
        }
        safe->files->rebuild();
        safe->files->selectFile (target);
    }, options);
}

void ShowPanel::deleteFile()
{
    const auto file = files->selectedFile();
    if (! file.existsAsFile())
        return;
    juce::Component::SafePointer<ShowPanel> safe (this);
    popups::confirm (popupHost(), "DELETE", "Move \"" + file.getFileName() + "\" to the trash?", "DELETE", [safe, file]
    {
        if (safe == nullptr)
            return;
        if (! file.moveToTrash())
            file.deleteFile();
        safe->files->rebuild();
        safe->updateKeys();
    }, true);
}

void ShowPanel::refresh()
{
    const auto selected = getSelected();
    list->rebuild();
    int row = list->rowOf (selected);
    if (row < 0 && selected.isValid())
        row = list->rowOf ({ selected.scene, -1 }); // a removed snippet: its scene
    if (row < 0)
        row = list->rowOf ({ juce::jmin (selected.scene, ctx.show.getNumScenes() - 1), -1 });
    if (row < 0 && ctx.show.getCurrentSceneIndex() >= 0)
        row = list->rowOf ({ ctx.show.getCurrentSceneIndex(), -1 });
    if (row >= 0 && row != list->getSelectedRow())
        list->selectRow (row);
    if (location != Location::Show)
        files->rebuild();
    updateKeys();
    repaint();
}

void ShowPanel::updateKeys()
{
    const bool file = files->selectedFile().existsAsFile();
    fileRenameKey.setEnabled (file);
    deleteKey.setEnabled (file);
    folderKey.setEnabled (files->folder.isDirectory());
    const auto item = getSelected();
    removeKey.setEnabled (item.isValid());
    renameKey.setEnabled (item.isValid());
    // SKIP: scenes. LINK: a scene to the scene below it, a snippet to its scene (fire with it).
    skipKey.setEnabled (item.isValid() && ! item.isSnippet());
    linkKey.setEnabled (item.isSnippet() || (item.isValid() && item.scene < ctx.show.getNumScenes() - 1));
    linkKey.isLit = [this]
    {
        const auto i = getSelected();
        if (i.isSnippet())
        {
            const auto sn = ctx.show.getSnippet (i.scene, i.snippet);
            return sn && sn->fireWithScene;
        }
        const auto sc = ctx.show.getScene (i.scene);
        return sc && sc->linkNext;
    };
    linkKey.litColour = FileColours::wav;
    upKey.setEnabled (item.isValid() && ! item.isSnippet() && item.scene > 0);
    downKey.setEnabled (item.isValid() && ! item.isSnippet() && item.scene < ctx.show.getNumScenes() - 1);
}

juce::Component& ShowPanel::popupHost()
{
    // The windows cover the whole LIBRARY screen, not just the list.
    auto* parent = getParentComponent();
    return parent != nullptr ? *parent : *this;
}

void ShowPanel::removeSelected()
{
    const auto item = getSelected();
    if (! item.isValid())
        return;
    const auto scene = ctx.show.getScene (item.scene);
    const auto snippet = item.isSnippet() ? ctx.show.getSnippet (item.scene, item.snippet) : std::nullopt;
    if (! scene || (item.isSnippet() && ! snippet))
        return;
    const auto name = item.isSnippet() ? snippet->name : scene->name;
    juce::Component::SafePointer<ShowPanel> safe (this);
    popups::confirm (popupHost(), "REMOVE", "Remove \"" + name + "\"" + (item.isSnippet() ? juce::String ("?") : juce::String (" and its snippets?")), "REMOVE",
                     [safe, item]
                     {
                         if (safe == nullptr)
                             return;
                         if (item.isSnippet())
                             safe->ctx.show.removeSnippet (item.scene, item.snippet);
                         else
                             safe->ctx.show.removeScene (item.scene);
                         safe->refresh();
                     },
                     true);
}

void ShowPanel::renameSelected()
{
    const auto item = getSelected();
    const auto scene = ctx.show.getScene (item.scene);
    const auto snippet = item.isSnippet() ? ctx.show.getSnippet (item.scene, item.snippet) : std::nullopt;
    if (! scene || (item.isSnippet() && ! snippet))
        return;
    popups::TextOptions options;
    options.maxLength = 32;
    juce::Component::SafePointer<ShowPanel> safe (this);
    popups::editText (popupHost(), item.isSnippet() ? "SNIPPET NAME" : "SCENE NAME", item.isSnippet() ? snippet->name : scene->name,
                      [safe, item] (const juce::String& text)
                      {
                          const auto name = text.trim();
                          if (safe == nullptr || name.isEmpty())
                              return;
                          if (item.isSnippet())
                              safe->ctx.show.setSnippetName (item.scene, item.snippet, name);
                          else
                              safe->ctx.show.setSceneName (item.scene, name);
                          safe->refresh();
                      },
                      options);
}

void ShowPanel::moveSelected (int delta)
{
    const auto item = getSelected();
    if (! item.isValid() || item.isSnippet())
        return;
    const int target = item.scene + delta;
    if (target < 0 || target >= ctx.show.getNumScenes())
        return;
    ctx.show.moveScene (item.scene, target);
    list->rebuild();
    select ({ target, -1 });
}

void ShowPanel::linkSelected()
{
    const auto item = getSelected();
    if (item.isSnippet())
    {
        if (const auto sn = ctx.show.getSnippet (item.scene, item.snippet))
            ctx.show.setSnippetFireWithScene (item.scene, item.snippet, ! sn->fireWithScene);
    }
    else if (const auto sc = ctx.show.getScene (item.scene))
        ctx.show.setSceneLink (item.scene, ! sc->linkNext);
    list->repaint();
    linkKey.repaint();
}

void ShowPanel::skipSelected()
{
    const auto item = getSelected();
    if (item.isSnippet())
        return;
    if (const auto sc = ctx.show.getScene (item.scene))
        ctx.show.setSceneSkip (item.scene, ! sc->skip);
    list->repaint();
    skipKey.repaint();
}

void ShowPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
    g.setColour (Dark::header);
    g.fillRect (toolbar);
    g.setColour (Dark::body);
    g.fillRect (listArea);
}

void ShowPanel::resized()
{
    auto b = getLocalBounds();
    toolbar = b.removeFromTop (juce::jlimit (36, 52, getHeight() * 11 / 100));
    b.removeFromTop (2);
    listArea = b;
    list->setRowHeight (juce::jlimit (28, 44, getHeight() / 12));
    list->setBounds (listArea);
    files->setRowHeight (juce::jlimit (28, 44, getHeight() / 12));
    files->setBounds (listArea);

    auto t = toolbar.reduced (2, 2);
    const int w = juce::jmin (t.getHeight() + 14, t.getWidth() / 10);
    for (auto* k : { &intKey, &usbKey, &showKey })
        k->setBounds (t.removeFromLeft (w));
    // At INT / USB: FOLDER, RENAME, (gap) DELETE, and the sort keys at the right.
    auto f = t;
    f.removeFromLeft (w / 2);
    sortDateKey.setBounds (f.removeFromRight (w));
    sortTypeKey.setBounds (f.removeFromRight (w));
    folderKey.setBounds (f.removeFromLeft (w));
    fileRenameKey.setBounds (f.removeFromLeft (w));
    f.removeFromLeft (w / 2);
    deleteKey.setBounds (f.removeFromLeft (w));
    t.removeFromLeft (juce::jmax (4, (t.getWidth() - 6 * w) / 2));
    for (auto* k : { &removeKey, &renameKey, &skipKey, &linkKey, &upKey, &downKey })
        k->setBounds (t.removeFromLeft (w));
}

} // namespace livedaw::ui
