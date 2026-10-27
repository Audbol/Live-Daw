#pragma once

#include "ui/ScreenWidgets.h"
#include "ui/WingPopups.h"

namespace livedaw::ui
{

/** The left half of the LIBRARY screen (manual 5.6, fig. 44/45), a file browser
    with three locations:

    - SHOW: the open show's scenes, each followed by its snippets (indented; a
      green bar links the ones that fire with their scene, and a scene to the
      one below it that it LINKs), with REMOVE, RENAME, SKIP, LINK, UP, DOWN.
      Scene tags show as "#n", skipped scenes dimmed with the SKIP mark.
    - INT: the library folder (Documents/Sidecar/Library) and USB: a folder of
      your choice (USB again chooses another). Shows, scene and snippet files,
      FX and channel presets and audio clips with their type chip and date,
      folders first; FOLDER, RENAME, DELETE and sorting by name and type or
      by date.

    Rows whose type doesn't match the open tab on the right are greyed, as on
    the console. */
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

    enum class Location
    {
        Show,
        Internal,
        Usb
    };

    Item getSelected() const;
    void select (Item);

    Location getLocation() const noexcept { return location; }
    void setLocation (Location);
    /** The folder shown at INT / USB (INT's library folder at SHOW). */
    juce::File getFolder() const;
    /** The selected file at INT / USB (none at SHOW, or for a folder). */
    juce::File getSelectedFile() const;
    /** Selects a file of the shown folder. */
    void selectFile (const juce::File&);
    /** The library folder (INT). */
    static juce::File libraryFolder();

    /** Which rows are shown normally: 0 all (SHOW), 1 scenes (SNAP), 2 snippets (SNIP), -1 none. */
    void setTypeFilter (int filter);
    /** At INT / USB: the file extensions shown normally (e.g. "lfx", or
        "wav;mp3"; empty: none). */
    void setFileFilter (const juce::String& extension);

    /** Re-reads the show and the folder (call after they changed). */
    void refresh();

    std::function<void (Item)> onSelectionChanged;
    /** A row was double-clicked (GO on it). */
    std::function<void (Item)> onActivated;
    /** At INT / USB: the selected file changed, or a file was double-clicked. */
    std::function<void (const juce::File&)> onFileSelected, onFileActivated;
    /** The location changed. */
    std::function<void()> onLocationChanged;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class List;
    class FileList;

    void removeSelected();
    void renameSelected();
    juce::Component& popupHost();
    void moveSelected (int delta);
    void linkSelected();
    void skipSelected();
    void updateKeys();
    void chooseUsbFolder();
    void newFolder();
    void renameFile();
    void deleteFile();

    UiContext& ctx;
    std::unique_ptr<List> list;
    std::unique_ptr<FileList> files;
    Location location = Location::Show;
    std::unique_ptr<juce::FileChooser> chooser;
    ToolKey intKey { "INT", Icon::Storage }, usbKey { "USB", Icon::UsbKey }, showKey { "SHOW", Icon::ShowFile, FileColours::show },
        removeKey { "REMOVE", Icon::RemoveCircle }, renameKey { "RENAME", Icon::Edit }, skipKey { "SKIP", Icon::Skip, Dark::on },
        linkKey { "LINK", Icon::Link, FileColours::wav }, upKey { "UP", Icon::ChevronUp }, downKey { "DOWN", Icon::ChevronDown },
        folderKey { "FOLDER", Icon::FolderAdd }, fileRenameKey { "RENAME", Icon::Edit }, deleteKey { "DELETE", Icon::Trash },
        sortTypeKey { "NAME/TYPE", Icon::ListAll }, sortDateKey { "DATE", Icon::SortDate };
    juce::Rectangle<int> toolbar, listArea;
};

} // namespace livedaw::ui
