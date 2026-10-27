#pragma once

#include "daw/Session.h"
#include "ui/DisplayPage.h"
#include "ui/ScreenWidgets.h"

namespace livedaw::ui
{

/** The recorder screen, laid out like the WING-LIVE SD card screens (manual
    chapter 8, figs. 74 and 75): the PLAYER (armed channels and rate, meters,
    the red time, the transport with the marker keys and MODE PLAY / A->B /
    LOOP, the disk and record time left, the timeline), the SESSIONS (projects in the recordings folder: open,
    rename, delete) and the MARKERS of the open session (jump, update,
    delete). Below: the tracks to arm and the one-click setups RECORD ALL
    CHANNELS and VIRTUAL SOUNDCHECK. EDIT (status bar) opens the multitrack
    editor; the format key in the header picks the record format; SETTINGS
    (status bar) holds SHOW METERS and AUTO INPUT SELECT (manual 8.1 p. 85-86).
    The knobs under the display move the position and the range's start and
    end (the A->B points, the loop borders). */
class RecorderPage final : public DisplayPage, private juce::Timer
{
public:
    RecorderPage (UiContext&, daw::Session&);
    ~RecorderPage() override;

    std::vector<KnobBinding> getKnobs() override;
    std::vector<StatusKey> getStatusKeys() override;
    void refresh() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void visibilityChanged() override;

    class SessionList;
    class MarkerList;
    class Settings;

    /** SETTINGS: shows or hides the window. */
    void showSettings (bool show);

private:
    void timerCallback() override;
    void recordAllChannels();
    void virtualSoundcheck();
    bool canVirtualSoundcheck() const;
    /** The channels VIRTUAL SOUNDCHECK switched still listen to the recorder (ALT). */
    bool isVirtualSoundcheckOn() const;
    /** The channels VIRTUAL SOUNDCHECK switched, with their INPUT SELECT before
        (GLOBAL, ALT); put back when it ends. */
    struct SoundcheckChannel
    {
        int number = 0;
        bool srcAuto = false;
    };
    std::vector<SoundcheckChannel> soundcheckChannels;
    void locateFromTimeline (int x);
    daw::int64 timelineLength() const;
    int tracksPerPage() const;
    int numTrackPages() const;

    void paintPlayer (juce::Graphics&);
    void paintTracks (juce::Graphics&);

    UiContext& ctx;
    daw::Session& session;
    std::unique_ptr<SessionList> sessions;
    std::unique_ptr<MarkerList> markers;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<Settings> settings;
    void cycleMode();

    WingKey stopKey { {}, Icon::Stop }, playKey { {}, Icon::Play }, prevKey { {}, Icon::MarkerPrev }, nextKey { {}, Icon::MarkerNext },
        addMarkerKey { {}, Icon::MarkerAdd }, recordKey { {}, Icon::Record, Dark::pre }, modeKey { "PLAY", std::nullopt, Dark::choice };
    WingKey newKey { "NEW", Icon::Plus }, saveKey { "SAVE", Icon::SaveTray }, editKey { "EDIT", Icon::Waveform };
    /** The record format (a list: WAV 32-bit float, WAV / AIFF / FLAC 24-bit, MP3). */
    WingKey formatKey { "WAV 32F", Icon::ChevronDown };
    void chooseRecordFormat();
    WingKey allChannelsKey { "RECORD ALL\nCHANNELS", Icon::Record }, soundcheckKey { "VIRTUAL\nSOUNDCHECK", Icon::Loop },
        armAllKey { "ARM ALL" }, disarmKey { "DISARM" }, pagePrev { {}, Icon::ChevronLeft }, pageNext { {}, Icon::ChevronRight };
    ToolKey renameKey { "NAME", Icon::Edit }, deleteKey { "DELETE", Icon::Trash }, updateKey { "UPDATE", Icon::Edit }, deleteMarkerKey { "DELETE", Icon::Trash };

    juce::Rectangle<int> header, player, sessionsPanel, markersPanel, tracksPanel;
    juce::Rectangle<int> chipsRow, meterArea, timeArea, infoRow, timeline, trackGrid;
    std::vector<std::pair<juce::Rectangle<int>, daw::int64>> trackTiles;
    int trackPage = 0;
    juce::String shownTime;
    juce::uint32 shownChange = 0;
    juce::File shownProject;
    Recorder::State shownState = Recorder::State::Stopped;
    std::vector<float> shownLevels;
};

} // namespace livedaw::ui
