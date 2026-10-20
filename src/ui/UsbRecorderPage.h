#pragma once

#include "ui/DisplayPage.h"
#include "ui/WingPopups.h"

#include <array>

namespace livedaw::ui
{

/** The USB Recorder / Player screen (manual 8.2, figs. 78 and 79), opened
    from the status bar's USB icon or by holding VIEW on the monitoring panel.

    Left: the browser of the "USB stick" (the storage folder): toolbar USB |
    N SONGS | ADD | FOLDER | RENAME | DELETE | REC PATH, the folder's
    folders, audio files (WAV, FLAC, AIFF, MP3) and playlists. N SONGS turns
    it into the playlist (USB | N SONGS | REPEAT | REMOVE | SKIP | STOP | UP |
    DOWN). Right: the 4-TRACK RECORDER (meters, the red time, the reels, the
    FORMAT list WAV / FLAC / AIFF / MP3, 2-CH|4-CH, 16|24 bit or, for MP3,
    the bit rate list, REC / PAUSE / STOP / NEW FILE), the playlist keys (CREATE / OPEN / SAVE /
    CLOSE PLAYLIST) and the 4-TRACK PLAYER (meters, the time and the time
    left, the reels, the file's format, PLAY / PAUSE / STOP / < >, the
    timeline). Everything is drawn here, keys included; the page uses the
    whole screen (no parameter bar). */
class UsbRecorderPage final : public DisplayPage, private juce::Timer
{
public:
    explicit UsbRecorderPage (UiContext&);
    ~UsbRecorderPage() override;

    bool usesKnobs() const override { return false; }
    StatusTitle getStatusTitle() override;
    void refresh() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void visibilityChanged() override;

    /** The 2-TRACK TRANSP keys and USB PLAY/REC custom controls share these
        (they act like the screen's keys). */
    static void pressRec (UiContext&);
    static void pressPlay (UiContext&);

private:
    /** A key of the screen: where it is, what it does on a click and on a hold. */
    struct Key
    {
        juce::Rectangle<int> area;
        std::function<void()> onClick, onHold;
        std::function<bool()> enabled;
    };

    struct Row
    {
        juce::File file;
        int kind = 0; // UsbRecorder::ItemKind for the browser; the playlist uses Wav
        uint64_t entry = 0;
        bool skip = false, stop = false;
        juce::Time modified;
    };

    void timerCallback() override;
    UsbRecorder& usb() const;
    void reload();
    void layout();
    void addKey (juce::Rectangle<int>, std::function<void()> onClick, std::function<bool()> enabled = {}, std::function<void()> onHold = {});
    int visibleRows() const;
    juce::Rectangle<int> rowArea (int visibleIndex) const;
    int rowAt (juce::Point<int>) const;
    void select (int row);
    void scrollBy (int rows);
    const Row* selectedRow() const;
    void openFolder (const juce::File&);
    void playSelected();
    void seekFromTimeline (int x);
    void saveSettings();
    void warn (const juce::String& title, const juce::Result&);

    // Toolbar actions.
    void showBrowser();
    void showPlaylist();
    void addSelected();
    void newFolder();
    void renameSelected();
    void deleteSelected();
    void setRecPath();
    void chooseStorageFolder();
    void savePlaylist();
    void closePlaylist();
    /** The recorder's FORMAT and MP3 bit rate lists. */
    void chooseRecFormat();
    void chooseKbps();
    void openList (juce::Rectangle<int> anchorArea, const juce::StringArray& items, int current, std::function<void (int)> chosen,
                   popups::ListOptions options);

    void paintToolbar (juce::Graphics&);
    void paintList (juce::Graphics&);
    void paintRecorder (juce::Graphics&);
    void paintPlaylistKeys (juce::Graphics&);
    /** The current file's format and its length in frames at the console's
        rate; before it has been opened, those of the cued (or selected) file,
        from its header. False when there is no file. */
    bool currentFileInfo (UsbRecorder::FileInfo&, int64_t& length, bool& mismatch) const;
    void paintPlayer (juce::Graphics&);
    void paintTimeline (juce::Graphics&);
    void paintKey (juce::Graphics&, juce::Rectangle<int>, const juce::String& text, std::function<void (juce::Graphics&, juce::Rectangle<float>, juce::Colour)> icon,
                   bool enabled, bool lit = false, juce::Colour litColour = {}) const;

    UiContext& ctx;
    bool playlistView = false;
    std::vector<Row> rows;
    int selected = -1, scroll = 0;
    juce::File folderShown;
    uint64_t queueVersion = 0;
    juce::File selectedFile; // kept across reloads

    // Layout (page coordinates).
    juce::Rectangle<int> leftPanel, toolbar, listArea, listHeader, rowsArea, scrollBar, scrollUp, scrollDown, headerFolderKey, headerOpenKey;
    juce::Rectangle<int> recBox, recKeys, playlistKeys, playBox, playKeys, timeline;
    juce::Rectangle<int> recMeter, recTime, recReels, recFormat, recPathText, recCaption;
    juce::Rectangle<int> playMeter, playTime, playReels, playChips, playCaption;
    std::array<juce::Rectangle<int>, 8> toolKeys;
    std::array<juce::Rectangle<int>, 4> recKeyAreas, playlistKeyAreas;
    std::array<juce::Rectangle<int>, 5> playKeyAreas;
    std::array<juce::Rectangle<int>, 2> channelKeys, bitKeys;
    juce::Rectangle<int> formatKey, kbpsKey;
    juce::Component listAnchor; // where the FORMAT / bit rate lists open
    std::vector<Key> keys;
    int rowHeight = 30;

    // Mouse state.
    int pressedKey = -1;
    juce::uint32 pressTime = 0;
    bool draggingTimeline = false;

    // Meters (dB, decaying) and what was last drawn.
    std::array<float, 4> recLevels {}, playLevels {};
    double reelAngle[2] {};
    juce::String shownState;
};

} // namespace livedaw::ui
