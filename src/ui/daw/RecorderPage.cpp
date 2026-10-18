#include "ui/daw/RecorderPage.h"
#include "ui/HomeWidgets.h"
#include "ui/WingPopups.h"
#include "ui/daw/DawEditor.h"

namespace livedaw::ui
{

using daw::int64;

namespace
{
    const juce::Colour timeRed { 0xffff3b30 };
    const juce::Colour rowColour { 0xff262626 };

    /** "00h 00m 04s 12" (hundredths) */
    juce::String clockText (int64 samples, double rate)
    {
        const auto cs = (int64) ((double) juce::jmax<int64> (0, samples) * 100.0 / rate);
        return juce::String (cs / 360000).paddedLeft ('0', 2) + "h " + juce::String ((cs / 6000) % 60).paddedLeft ('0', 2) + "m "
               + juce::String ((cs / 100) % 60).paddedLeft ('0', 2) + "s " + juce::String (cs % 100).paddedLeft ('0', 2);
    }

    juce::String bytesText (juce::int64 bytes)
    {
        const double gb = (double) bytes / (1024.0 * 1024.0 * 1024.0);
        return gb >= 10.0 ? juce::String (juce::roundToInt (gb)) + " GB" : juce::String (gb, 1) + " GB";
    }

    void paintPanelHeader (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title)
    {
        g.setColour (Dark::header);
        g.fillRect (r);
        g.setColour (Dark::text);
        g.setFont (juce::FontOptions (juce::jmin (19.0f, (float) r.getHeight() * 0.62f), juce::Font::bold));
        g.drawFittedText (title, r, juce::Justification::centred, 1);
    }
} // namespace

//==============================================================================
/** SESSIONS: the projects in the recordings folder, newest first; the open
    one is highlighted. A click opens a session. */
class RecorderPage::SessionList final : public juce::ListBox, private juce::ListBoxModel
{
public:
    SessionList (RecorderPage& p) : page (p)
    {
        setModel (this);
        setRowHeight (40);
        setColour (backgroundColourId, Dark::body);
        setColour (outlineColourId, juce::Colours::transparentBlack);
        getVerticalScrollBar().setColour (juce::ScrollBar::thumbColourId, Dark::key);
    }

    ~SessionList() override { setModel (nullptr); }

    std::vector<juce::File> files;

    void reload()
    {
        files = page.session.getSessions();
        // Keep the open project in the list even if it lives elsewhere.
        auto& project = page.session.getProject();
        if (project.hasFolder() && std::find (files.begin(), files.end(), project.getFile()) == files.end())
            files.insert (files.begin(), project.getFile());
        updateContent();
        selectOpen();
        repaint();
    }

    void selectOpen()
    {
        auto& project = page.session.getProject();
        for (size_t i = 0; i < files.size(); ++i)
            if (project.hasFolder() && files[i] == project.getFile())
            {
                selectRow ((int) i, false, true);
                return;
            }
        deselectAllRows();
    }

    juce::File selectedFile() const
    {
        const int row = getSelectedRow();
        return juce::isPositiveAndBelow (row, (int) files.size()) ? files[(size_t) row] : juce::File();
    }

private:
    int getNumRows() override { return (int) files.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool rowSelected) override
    {
        if (! juce::isPositiveAndBelow (row, (int) files.size()))
            return;
        const auto& f = files[(size_t) row];
        auto& project = page.session.getProject();
        const bool open = project.hasFolder() && f == project.getFile();
        auto r = juce::Rectangle<int> (width, height).reduced (4, 2).toFloat();
        g.setColour (rowColour);
        g.fillRect (r);
        auto number = r.removeFromLeft (30.0f);
        g.setColour (open ? Dark::on : Dark::box);
        g.fillRect (number);
        g.setColour (open ? juce::Colours::black : Dark::text);
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.drawText (juce::String ((int) files.size() - row), number.toNearestInt(), juce::Justification::centred);
        auto t = r.reduced (8.0f, 2.0f);
        auto chip = t.removeFromRight (64.0f);
        g.setColour (Dark::text);
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawFittedText ((open ? project.getName() : f.getFileNameWithoutExtension()).toUpperCase() + (open && project.isDirty() ? " *" : ""),
                          t.removeFromTop (t.getHeight() * 0.55f).toNearestInt(), juce::Justification::centredLeft, 1, 0.75f);
        g.setColour (Dark::textDim);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (f.getParentDirectory().getCreationTime().formatted ("%Y-%m-%d  %H:%M"), t.toNearestInt(), juce::Justification::centredLeft);
        g.setColour (Dark::textDim);
        g.fillRect (chip.getX(), chip.getY() + 4.0f, 1.0f, chip.getHeight() - 8.0f);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (open ? juce::String (project.getNumTracks()) + " TR" : juce::String ("LDPROJ"), chip.toNearestInt(), juce::Justification::centred);
        if (rowSelected || open)
        {
            g.setColour (open ? Dark::on : Dark::selectedOutline);
            g.drawRect (juce::Rectangle<int> (width, height).reduced (4, 2), 2);
        }
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (! juce::isPositiveAndBelow (row, (int) files.size()) || e.mods.isPopupMenu())
            return;
        dawfiles::open (page.getTopLevelComponent(), page.session, files[(size_t) row]);
    }

    RecorderPage& page;
};

//==============================================================================
/** MARKERS of the open session: a click jumps to one. */
class RecorderPage::MarkerList final : public juce::ListBox, private juce::ListBoxModel
{
public:
    MarkerList (RecorderPage& p) : page (p)
    {
        setModel (this);
        setRowHeight (34);
        setColour (backgroundColourId, Dark::body);
        setColour (outlineColourId, juce::Colours::transparentBlack);
        getVerticalScrollBar().setColour (juce::ScrollBar::thumbColourId, Dark::key);
    }

    ~MarkerList() override { setModel (nullptr); }

    std::vector<daw::MarkerData> list;

    void reload()
    {
        const auto previous = selectedMarker();
        list = page.session.getProject().getMarkers();
        updateContent();
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].id == previous)
                selectRow ((int) i, false, true);
        repaint();
    }

    daw::int64 selectedMarker() const
    {
        const int row = getSelectedRow();
        return juce::isPositiveAndBelow (row, (int) list.size()) ? list[(size_t) row].id : 0;
    }

private:
    int getNumRows() override { return (int) list.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool rowSelected) override
    {
        if (! juce::isPositiveAndBelow (row, (int) list.size()))
            return;
        const auto& m = list[(size_t) row];
        auto r = juce::Rectangle<int> (width, height).reduced (4, 2).toFloat();
        g.setColour (rowColour);
        g.fillRect (r);
        auto tag = r.removeFromLeft (38.0f);
        g.setColour (rowSelected ? Dark::on : Dark::box);
        g.fillRect (tag);
        g.setColour (rowSelected ? juce::Colours::black : Dark::text);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText ("M" + juce::String (row + 1), tag.toNearestInt(), juce::Justification::centred);
        auto t = r.reduced (8.0f, 0.0f).toNearestInt();
        const auto grid = juce::String (daw::TimeGrid { page.session.getProject().getSampleRate() }.formatTime (m.position));
        g.setColour (Dark::text);
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawText (grid, t.removeFromLeft (84), juce::Justification::centredLeft);
        g.setColour (Dark::textDim);
        g.drawFittedText (m.name, t, juce::Justification::centredLeft, 1, 0.75f);
        if (rowSelected)
        {
            g.setColour (Dark::on);
            g.drawRect (juce::Rectangle<int> (width, height).reduced (4, 2), 2);
        }
    }

    void listBoxItemClicked (int row, const juce::MouseEvent&) override
    {
        if (juce::isPositiveAndBelow (row, (int) list.size()))
            page.session.locate (list[(size_t) row].position);
    }

    RecorderPage& page;
};

//==============================================================================
RecorderPage::RecorderPage (UiContext& c, daw::Session& s) : ctx (c), session (s)
{
    sessions = std::make_unique<SessionList> (*this);
    markers = std::make_unique<MarkerList> (*this);
    addAndMakeVisible (*sessions);
    addAndMakeVisible (*markers);

    for (auto* k : { &stopKey, &playKey, &prevKey, &nextKey, &addMarkerKey, &recordKey, &modeKey })
    {
        k->offColour = Dark::key;
        addAndMakeVisible (k);
    }
    stopKey.onClick = [this] { session.stop(); };
    stopKey.isOn = [this] { return session.getState() == Recorder::State::Stopped; };
    stopKey.setTooltip ("Stop");
    playKey.onClick = [this] { if (! session.isRolling()) session.play(); };
    playKey.isOn = [this] { return session.getState() == Recorder::State::Playing; };
    playKey.setTooltip ("Play");
    prevKey.onClick = [this] { session.jumpToMarker (false); };
    prevKey.setTooltip ("Previous marker");
    nextKey.onClick = [this] { session.jumpToMarker (true); };
    nextKey.setTooltip ("Next marker");
    addMarkerKey.onClick = [this] { session.addMarkerAtPosition(); };
    addMarkerKey.setTooltip ("Add a marker at the position");
    recordKey.onClick = [this]
    {
        if (session.isRecording())
        {
            session.stop();
            return;
        }
        auto r = session.record();
        if (r.failed())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Record", r.getErrorMessage());
    };
    recordKey.isOn = [this] { return session.isRecording(); };
    recordKey.setTooltip ("Record the armed tracks");
    recordKey.iconColour = Dark::pre;
    modeKey.onClick = [this] { session.setLoopEnabled (! session.isLoopEnabled()); };
    modeKey.isOn = [this] { return session.isLoopEnabled(); };
    modeKey.setTooltip ("MODE: PLAY or LOOP (the loop range is set in the editor's ruler or with the knobs)");

    for (auto* k : { &newKey, &saveKey, &editKey, &formatKey, &allChannelsKey, &soundcheckKey, &armAllKey, &disarmKey, &pagePrev, &pageNext })
    {
        styleIconKey (*k);
        addAndMakeVisible (k);
    }
    newKey.onClick = [this] { dawfiles::newProject (getTopLevelComponent(), session); };
    newKey.setTooltip ("A new session in " + session.getProjectsFolder().getFullPathName());
    saveKey.onClick = [this] { dawfiles::save (getTopLevelComponent(), session); };
    editKey.onClick = [this] { if (ctx.showEditor) ctx.showEditor (true); };
    formatKey.onClick = [this] { chooseRecordFormat(); };
    formatKey.setButtonText (session.getRecordFormat().shortName());
    formatKey.setTooltip ("Record format of the takes (32-bit float WAV, 24-bit WAV / AIFF / FLAC, MP3)");
    editKey.setTooltip ("Open the multitrack editor (Ctrl+E)");
    allChannelsKey.onClick = [this] { recordAllChannels(); };
    allChannelsKey.iconColour = Dark::pre;
    allChannelsKey.setTooltip ("A track per input channel, recording its input port and playing back on the playback channels, all armed");
    soundcheckKey.onClick = [this] { virtualSoundcheck(); };
    armAllKey.onClick = [this] { session.armAll (true); };
    disarmKey.onClick = [this] { session.armAll (false); };
    pagePrev.onClick = [this] { trackPage = juce::jmax (0, trackPage - 1); resized(); repaint(); };
    pageNext.onClick = [this] { trackPage = juce::jmin (numTrackPages() - 1, trackPage + 1); resized(); repaint(); };

    for (auto* k : { &renameKey, &deleteKey, &updateKey, &deleteMarkerKey })
        addAndMakeVisible (k);
    renameKey.onClick = [this]
    {
        const auto f = sessions->selectedFile();
        if (f == juce::File())
            return;
        askForText (getTopLevelComponent(), "RENAME SESSION", "Name:", f.getFileNameWithoutExtension(), [this, f] (const juce::String& name)
        {
            auto r = session.renameSession (f, name);
            if (r.failed())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Rename", r.getErrorMessage());
            sessions->reload();
        });
    };
    deleteKey.onClick = [this]
    {
        const auto f = sessions->selectedFile();
        if (f == juce::File())
            return;
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, "Delete session",
                                            "Move \"" + f.getFileNameWithoutExtension() + "\" with all its recordings to the trash?", "DELETE", "CANCEL", this,
                                            juce::ModalCallbackFunction::create ([this, f] (int r)
        {
            if (r == 0)
                return;
            auto result = session.deleteSession (f);
            if (result.failed())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Delete", result.getErrorMessage());
            sessions->reload();
        }));
    };
    updateKey.onClick = [this]
    {
        const auto id = markers->selectedMarker();
        for (auto& m : session.getProject().getMarkers())
            if (m.id == id)
            {
                session.getProject().beginEdit ("Update marker");
                session.getProject().setMarker (id, session.getPosition(), m.name);
            }
    };
    updateKey.setTooltip ("Move the selected marker to the position");
    deleteMarkerKey.onClick = [this]
    {
        if (const auto id = markers->selectedMarker(); id != 0)
        {
            session.getProject().beginEdit ("Delete marker");
            session.getProject().removeMarker (id);
        }
    };

    soundcheckKey.isOn = [this] { return isVirtualSoundcheckOn(); };
    soundcheckKey.setTooltip ("Each channel plays back its track: its ALT input becomes the track's playback channel and INPUT SELECT switches to GLOBAL ALT (again: back to MAIN)");
    startTimerHz (15);
}

RecorderPage::~RecorderPage() = default;

std::vector<StatusKey> RecorderPage::getStatusKeys()
{
    return { { "EDIT", Icon::Waveform, false, [this] { if (ctx.showEditor) ctx.showEditor (true); } } };
}

std::vector<KnobBinding> RecorderPage::getKnobs()
{
    auto& p = session.getProject();
    auto lengthOf = [this] { return (double) timelineLength(); };
    std::vector<KnobBinding> knobs;

    KnobBinding position;
    position.label = "POSITION";
    position.get = [this, lengthOf] { return (float) juce::jlimit (0.0, 1.0, (double) session.getPosition() / lengthOf()); };
    position.set = [this, lengthOf] (float v) { session.locate ((int64) ((double) v * lengthOf())); };
    position.text = [this, &p] { return daw::TimeGrid { p.getSampleRate() }.formatTime (session.getPosition()); };
    position.reset = [this] { session.locate (0); };
    knobs.push_back (position);

    KnobBinding loopStart;
    loopStart.label = "LOOP IN";
    loopStart.get = [&p, lengthOf] { return (float) juce::jlimit (0.0, 1.0, (double) p.getLoopStart() / lengthOf()); };
    loopStart.set = [&p, lengthOf] (float v)
    {
        p.beginEdit ("Loop range");
        p.setLoop (juce::jmin ((int64) ((double) v * lengthOf()), p.getLoopEnd()), p.getLoopEnd());
    };
    loopStart.text = [&p] { return daw::TimeGrid { p.getSampleRate() }.formatTime (p.getLoopStart()); };
    knobs.push_back (loopStart);

    KnobBinding loopEnd;
    loopEnd.label = "LOOP OUT";
    loopEnd.get = [&p, lengthOf] { return (float) juce::jlimit (0.0, 1.0, (double) p.getLoopEnd() / lengthOf()); };
    loopEnd.set = [&p, lengthOf] (float v)
    {
        p.beginEdit ("Loop range");
        p.setLoop (p.getLoopStart(), juce::jmax (p.getLoopStart(), (int64) ((double) v * lengthOf())));
    };
    loopEnd.text = [&p] { return daw::TimeGrid { p.getSampleRate() }.formatTime (p.getLoopEnd()); };
    knobs.push_back (loopEnd);

    KnobBinding mode;
    mode.label = "MODE";
    mode.steps = 2.0f;
    mode.get = [this] { return session.isLoopEnabled() ? 1.0f : 0.0f; };
    mode.set = [this] (float v) { session.setLoopEnabled (v > 0.5f); };
    mode.text = [this] { return juce::String (session.isLoopEnabled() ? "LOOP" : "PLAY"); };
    knobs.push_back (mode);
    return knobs;
}

//==============================================================================
int64 RecorderPage::timelineLength() const
{
    auto& p = session.getProject();
    const auto len = juce::jmax (p.getLength(), p.getLoopEnd(), (int64) session.getPosition());
    return juce::jmax<int64> ((int64) (p.getSampleRate() * 60.0), (int64) ((double) len * 1.05));
}

void RecorderPage::chooseRecordFormat()
{
    const auto choices = AudioFileFormat::recordChoices();
    juce::StringArray items;
    popups::ListOptions options;
    options.caption = "RECORD FORMAT";
    int selected = 0;
    for (size_t i = 0; i < choices.size(); ++i)
    {
        items.add (choices[i].describe());
        if (choices[i] == session.getRecordFormat())
            selected = (int) i;
        // MP3 needs 32, 44.1 or 48 kHz; nothing changes while recording.
        if (session.isRecording() || choices[i].check (2, session.getProject().getSampleRate()).isNotEmpty())
            options.disabled.push_back ((int) i);
    }
    juce::Component::SafePointer<RecorderPage> safe (this);
    popups::chooseFrom (formatKey, items, selected, [safe, choices] (int i)
    {
        if (safe == nullptr || ! juce::isPositiveAndBelow (i, (int) choices.size()))
            return;
        safe->session.setRecordFormat (choices[(size_t) i]);
        safe->formatKey.setButtonText (choices[(size_t) i].shortName());
    }, options);
}

void RecorderPage::refresh()
{
    sessions->reload();
    markers->reload();
    repaint();
}

void RecorderPage::visibilityChanged()
{
    if (isVisible())
        refresh();
}

int RecorderPage::tracksPerPage() const
{
    return juce::jmax (1, (trackGrid.getWidth() / 70) * 2);
}

int RecorderPage::numTrackPages() const
{
    return juce::jmax (1, (session.getProject().getNumTracks() + tracksPerPage() - 1) / tracksPerPage());
}

void RecorderPage::resized()
{
    auto b = getLocalBounds();
    header = b.removeFromTop (juce::jlimit (28, 38, getHeight() / 12));
    {
        auto h = header.reduced (2, 2);
        const int w = juce::jmin (100, h.getWidth() / 8);
        editKey.setBounds (h.removeFromRight (w));
        h.removeFromRight (4);
        saveKey.setBounds (h.removeFromRight (w));
        h.removeFromRight (4);
        newKey.setBounds (h.removeFromRight (w));
        h.removeFromRight (4);
        formatKey.setBounds (h.removeFromRight (w * 13 / 10));
    }
    b.removeFromTop (4);
    tracksPanel = b.removeFromBottom (juce::jlimit (104, 170, getHeight() * 30 / 100));
    b.removeFromBottom (4);
    const int w = b.getWidth();
    player = b.removeFromLeft (w * 46 / 100);
    b.removeFromLeft (4);
    sessionsPanel = b.removeFromLeft (b.getWidth() * 52 / 100);
    b.removeFromLeft (4);
    markersPanel = b;

    // Panel headers with their tool keys (NAME / DELETE, UPDATE / DELETE).
    const int headerH = kDarkHeaderHeight;
    auto sh = sessionsPanel.withHeight (headerH);
    renameKey.setBounds (sh.removeFromLeft (46));
    deleteKey.setBounds (sh.removeFromRight (46));
    sessions->setBounds (sessionsPanel.withTrimmedTop (headerH + 2));
    auto mh = markersPanel.withHeight (headerH);
    updateKey.setBounds (mh.removeFromLeft (52));
    deleteMarkerKey.setBounds (mh.removeFromRight (46));
    markers->setBounds (markersPanel.withTrimmedTop (headerH + 2));

    // PLAYER: chips, meters with the red time, transport, disk info, timeline.
    auto p = player.withTrimmedTop (headerH + 2).reduced (6, 4);
    timeline = p.removeFromBottom (juce::jlimit (16, 26, p.getHeight() / 9));
    p.removeFromBottom (4);
    infoRow = p.removeFromBottom (20);
    p.removeFromBottom (4);
    auto transport = p.removeFromBottom (juce::jlimit (28, 40, p.getHeight() / 4));
    p.removeFromBottom (4);
    chipsRow = p.removeFromTop (20);
    p.removeFromTop (2);
    timeArea = p.removeFromBottom (juce::jlimit (22, 36, p.getHeight() / 3));
    meterArea = p;
    {
        auto t = transport;
        modeKey.setBounds (t.removeFromRight (juce::jmin (64, t.getWidth() / 6)));
        t.removeFromRight (6);
        const int k = juce::jmin (t.getHeight() + 14, t.getWidth() / 6 - 3);
        for (auto* key : { &stopKey, &playKey, &prevKey, &nextKey, &addMarkerKey })
        {
            key->setBounds (t.removeFromLeft (k));
            t.removeFromLeft (3);
        }
        recordKey.setBounds (t.removeFromRight (k));
    }

    // TRACKS: the setup keys left, the tiles right.
    auto tr = tracksPanel.withTrimmedTop (headerH + 2).reduced (4, 4);
    auto keys = tr.removeFromLeft (juce::jmin (250, tr.getWidth() / 3));
    const int kh = (keys.getHeight() - 4) / 2;
    auto row1 = keys.removeFromTop (kh);
    keys.removeFromTop (4);
    allChannelsKey.setBounds (row1.removeFromLeft (row1.getWidth() / 2 - 2));
    row1.removeFromLeft (4);
    soundcheckKey.setBounds (row1);
    armAllKey.setBounds (keys.removeFromLeft (keys.getWidth() / 2 - 2));
    keys.removeFromLeft (4);
    disarmKey.setBounds (keys);
    tr.removeFromLeft (8);
    auto pages = tr.removeFromRight (30);
    pagePrev.setBounds (pages.removeFromTop (pages.getHeight() / 2).reduced (0, 2));
    pageNext.setBounds (pages.reduced (0, 2));
    trackGrid = tr;
    trackPage = juce::jlimit (0, numTrackPages() - 1, trackPage);

    // Tile areas.
    trackTiles.clear();
    const int perRow = juce::jmax (1, trackGrid.getWidth() / 70);
    const float tw = (float) trackGrid.getWidth() / (float) perRow, th = (float) trackGrid.getHeight() / 2.0f;
    auto& project = session.getProject();
    for (int i = 0; i < tracksPerPage(); ++i)
    {
        const int index = trackPage * tracksPerPage() + i;
        if (index >= project.getNumTracks())
            break;
        const auto r = juce::Rectangle<float> ((float) trackGrid.getX() + tw * (float) (i % perRow), (float) trackGrid.getY() + th * (float) (i / perRow), tw, th)
                           .reduced (2.0f)
                           .toNearestInt();
        trackTiles.push_back ({ r, project.getTrackData (index).id });
    }
    enableKey (pagePrev, trackPage > 0);
    enableKey (pageNext, trackPage < numTrackPages() - 1);
}

//==============================================================================
void RecorderPage::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
    auto& project = session.getProject();

    // Header: the card icon, RECORDER, the session.
    g.setColour (Dark::header);
    g.fillRect (header);
    auto h = header.reduced (8, 2);
    drawIcon (g, Icon::SdCard, h.removeFromLeft (h.getHeight()).reduced (4).toFloat(), Dark::text);
    h.removeFromLeft (6);
    g.setColour (Dark::text);
    g.setFont (juce::FontOptions (juce::jmin (20.0f, (float) h.getHeight() * 0.7f), juce::Font::bold));
    g.drawText ("RECORDER", h.removeFromLeft (110), juce::Justification::centredLeft);
    g.setColour (Dark::gold);
    g.drawFittedText ("SESSION: " + project.getName().toUpperCase() + (project.isDirty() ? " *" : ""),
                      h.withTrimmedRight (editKey.getRight() - newKey.getX() + 10), juce::Justification::centredLeft, 1, 0.7f);

    paintPanelHeader (g, player.withHeight (kDarkHeaderHeight), "PLAYER");
    g.setColour (Dark::body);
    g.fillRect (player.withTrimmedTop (kDarkHeaderHeight + 2));
    paintPanelHeader (g, sessionsPanel.withHeight (kDarkHeaderHeight), "SESSIONS");
    paintPanelHeader (g, markersPanel.withHeight (kDarkHeaderHeight), "MARKERS");
    paintPanelHeader (g, tracksPanel.withHeight (kDarkHeaderHeight), "TRACKS");
    g.setColour (Dark::body);
    g.fillRect (tracksPanel.withTrimmedTop (kDarkHeaderHeight + 2));
    paintPlayer (g);
    paintTracks (g);
}

void RecorderPage::paintPlayer (juce::Graphics& g)
{
    auto& project = session.getProject();
    // Chips: armed channels, the session, the rate.
    auto chips = chipsRow;
    drawChip (g, chips.removeFromLeft (46).toFloat(), juce::String (session.getNumArmedChannels()) + " CH", Dark::key, Dark::text);
    drawChip (g, chips.removeFromRight (46).toFloat(), rateText (project.getSampleRate()), session.rateMatches() ? Dark::key : Dark::pre, Dark::text);
    g.setColour (Dark::text);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawFittedText (project.getName().toUpperCase(), chips.reduced (8, 0), juce::Justification::centredLeft, 1, 0.7f);

    // Meters of the armed tracks.
    g.setColour (Dark::well);
    g.fillRect (meterArea);
    std::vector<std::pair<daw::int64, int>> armed;
    for (auto& t : project.getTracks())
        if (t.arm)
            armed.push_back ({ t.id, t.stereo ? 2 : 1 });
    if (armed.empty())
    {
        g.setColour (Dark::textDim);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawFittedText ("NO TRACK ARMED", meterArea, juce::Justification::centred, 1);
    }
    else
    {
        const int count = (int) juce::jmin<size_t> (armed.size(), 64);
        // At most 16 px per bar, the bank centred like the WING-LIVE's.
        const float w = juce::jmin (16.0f, (float) (meterArea.getWidth() - 8) / (float) count);
        auto area = meterArea.reduced (4, 4).toFloat();
        area = area.withSizeKeepingCentre (w * (float) count, area.getHeight());
        // Scale lines at -12 and -36 like the WING's meter bank.
        g.setColour (Dark::key);
        for (float db : { -12.0f, -36.0f })
            g.fillRect (area.getX(), area.getBottom() - (db + 60.0f) / 60.0f * area.getHeight(), area.getWidth(), 1.0f);
        for (int i = 0; i < count; ++i)
        {
            float db = -100.0f;
            for (int ch = 0; ch < armed[(size_t) i].second; ++ch)
                db = juce::jmax (db, session.getTrackLevel (armed[(size_t) i].first, ch));
            const float pos = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
            auto bar = juce::Rectangle<float> (area.getX() + w * (float) i + 1.0f, area.getY(), juce::jmax (1.0f, w - 2.0f), area.getHeight());
            g.setColour (juce::Colour (0xff1a1a1a));
            g.fillRect (bar);
            // Green to -12 dB, yellow to -3 dB, red above.
            const float h = bar.getHeight();
            auto lit = bar.removeFromBottom (h * pos);
            const float yellowFrom = lit.getBottom() - h * (48.0f / 60.0f), redFrom = lit.getBottom() - h * (57.0f / 60.0f);
            g.setColour (Colours::meterGreen);
            g.fillRect (lit.withTop (juce::jmax (lit.getY(), yellowFrom)));
            g.setColour (Colours::meterYellow);
            g.fillRect (lit.withTop (juce::jmax (lit.getY(), redFrom)).withBottom (juce::jmax (lit.getY(), yellowFrom)));
            g.setColour (Colours::meterRed);
            g.fillRect (lit.withBottom (juce::jmax (lit.getY(), redFrom)));
        }
    }

    // The red time (the take's length while recording, else the position).
    auto t = timeArea.reduced (juce::jmax (0, (timeArea.getWidth() - 260) / 2), 0);
    g.setColour (juce::Colours::black);
    g.fillRect (t);
    g.setColour (timeRed.withAlpha (0.6f));
    g.drawRect (t, 1);
    g.setColour (timeRed);
    g.setFont (juce::FontOptions ((float) t.getHeight() * 0.8f, juce::Font::bold));
    const auto shown = session.isRecording() ? session.getRecordedLength() : session.getPosition();
    g.drawFittedText (clockText (shown, project.getSampleRate()), t, juce::Justification::centred, 1, 0.8f);

    // Disk and record time.
    auto info = infoRow;
    drawChip (g, info.removeFromLeft (64).toFloat(), bytesText (session.getFreeDiskBytes()), Dark::box, Dark::textDim);
    info.removeFromLeft (10);
    g.setColour (Dark::text);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    const int dropouts = session.getRecorder().getDropouts();
    g.drawFittedText ("REC TIME LEFT: " + durationText (session.getRecordSecondsLeft())
                          + (dropouts > 0 ? "   DROPOUTS " + juce::String (dropouts) : juce::String()),
                      info, juce::Justification::centredLeft, 1, 0.7f);

    // Timeline: the position over the session, markers, the loop.
    const auto len = (double) timelineLength();
    auto tl = timeline.toFloat();
    g.setColour (Dark::well);
    g.fillRect (tl);
    if (project.getLoopEnd() > project.getLoopStart())
    {
        const float x1 = tl.getX() + (float) ((double) project.getLoopStart() / len) * tl.getWidth();
        const float x2 = tl.getX() + (float) ((double) project.getLoopEnd() / len) * tl.getWidth();
        g.setColour (Dark::choice.withAlpha (project.isLoopOn() ? 0.6f : 0.25f));
        g.fillRect (x1, tl.getY(), x2 - x1, tl.getHeight());
    }
    const float px = tl.getX() + (float) juce::jlimit (0.0, 1.0, (double) session.getPosition() / len) * tl.getWidth();
    g.setColour ((session.isRecording() ? timeRed : Dark::gold).withAlpha (0.75f));
    g.fillRect (juce::Rectangle<float> (tl.getX(), tl.getY(), px - tl.getX(), tl.getHeight()).reduced (0.0f, 3.0f));
    g.setColour (Dark::text);
    for (auto& m : project.getMarkers())
        g.fillRect (tl.getX() + (float) ((double) m.position / len) * tl.getWidth(), tl.getY(), 1.5f, tl.getHeight());
    g.setColour (juce::Colours::white);
    g.fillRect (px - 1.0f, tl.getY(), 2.0f, tl.getHeight());
}

void RecorderPage::paintTracks (juce::Graphics& g)
{
    auto& project = session.getProject();
    if (trackTiles.empty())
    {
        g.setColour (Dark::textDim);
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawFittedText ("NO TRACKS: RECORD ALL CHANNELS makes one per channel", trackGrid, juce::Justification::centred, 2);
        return;
    }
    for (auto& [r, id] : trackTiles)
    {
        const int index = project.indexOfTrack (id);
        if (index < 0)
            continue;
        const auto t = project.getTrackData (index);
        const auto colour = wingColour (t.colour);
        auto f = r.toFloat();
        g.setColour (t.arm ? juce::Colour (0xff3a1414) : rowColour);
        g.fillRect (f);
        g.setColour (colour);
        g.fillRect (f.removeFromTop (4.0f));
        auto content = f.reduced (4.0f, 2.0f);
        auto top = content.removeFromTop (content.getHeight() * 0.5f);
        g.setColour (t.arm ? Dark::pre : Dark::key);
        auto dot = top.removeFromRight (top.getHeight()).reduced (3.0f);
        g.fillEllipse (dot);
        g.setColour (Dark::textDim);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (juce::String (index + 1), top.toNearestInt(), juce::Justification::centredLeft);
        g.setColour (Dark::text);
        g.drawFittedText (t.name.toUpperCase(), content.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
        // Level along the bottom edge.
        const float db = juce::jmax (session.getTrackLevel (id, 0), session.getTrackLevel (id, 1));
        const float pos = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
        g.setColour (Colours::meterGreen);
        g.fillRect (f.getX(), f.getBottom() - 3.0f, f.getWidth() * pos, 3.0f);
        if (t.arm)
        {
            g.setColour (Dark::pre);
            g.drawRect (r, 1);
        }
    }
    g.setColour (Dark::textDim);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    if (numTrackPages() > 1)
        g.drawText (juce::String (trackPage + 1) + "/" + juce::String (numTrackPages()), pagePrev.getBounds().withY (pagePrev.getY() - 14).withHeight (12),
                    juce::Justification::centred);
}

//==============================================================================
void RecorderPage::mouseDown (const juce::MouseEvent& e)
{
    if (timeline.contains (e.getPosition()))
    {
        locateFromTimeline (e.x);
        return;
    }
    for (auto& [r, id] : trackTiles)
        if (r.contains (e.getPosition()))
        {
            auto& p = session.getProject();
            const int index = p.indexOfTrack (id);
            if (index < 0 || session.isRecordingTrack (id))
                return;
            p.setTrackProperty (id, daw::ids::arm, ! p.getTrackData (index).arm, false);
            repaint (tracksPanel);
            return;
        }
}

void RecorderPage::mouseDrag (const juce::MouseEvent& e)
{
    if (timeline.contains (e.getMouseDownPosition()))
        locateFromTimeline (e.x);
}

void RecorderPage::locateFromTimeline (int x)
{
    const double frac = juce::jlimit (0.0, 1.0, (double) (x - timeline.getX()) / (double) juce::jmax (1, timeline.getWidth()));
    session.locate ((int64) (frac * (double) timelineLength()));
}

void RecorderPage::recordAllChannels()
{
    std::vector<daw::Session::ChannelInfo> channels;
    for (auto& s : ctx.engine.getChannels())
    {
        daw::Session::ChannelInfo ch;
        ch.number = s->getNumber();
        ch.name = home::hasDefaultName (*s) ? juce::String() : s->getName();
        ch.colour = s->getColourIndex();
        ch.stereo = s->params.stereo.load();
        ch.inputPort = s->params.inputLeft.load();
        channels.push_back (ch);
    }
    auto apply = [this, channels]
    {
        session.setupRecordAllChannels (channels);
        resized();
        repaint();
    };
    if (session.getProject().getNumTracks() == 0)
    {
        apply();
        return;
    }
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Record all channels",
                                        "Set the session's tracks to one per channel (" + juce::String ((int) channels.size())
                                            + "), armed? Existing tracks are renamed and re-routed; their clips stay.",
                                        "SET UP", "CANCEL", this, juce::ModalCallbackFunction::create ([apply] (int r) { if (r != 0) apply(); }));
}

bool RecorderPage::canVirtualSoundcheck() const
{
    return session.getProject().getNumTracks() > 0;
}

bool RecorderPage::isVirtualSoundcheckOn() const
{
    return ctx.engine.isInputSelectGlobal() && ctx.engine.isGlobalAlt();
}

void RecorderPage::virtualSoundcheck()
{
    auto& engine = ctx.engine;
    if (isVirtualSoundcheckOn())
    {
        // Back to the live inputs.
        engine.setGlobalAlt (false);
        ctx.log.info ("Virtual soundcheck off: the channels listen to their MAIN inputs");
        return;
    }
    std::vector<daw::Session::ChannelInfo> channels;
    for (auto& s : engine.getChannels())
    {
        daw::Session::ChannelInfo ch;
        ch.number = s->getNumber();
        ch.stereo = s->params.stereo.load();
        ch.inputPort = s->getInputLeft();
        channels.push_back (ch);
    }
    const auto routes = session.virtualSoundcheckRoutes (channels);
    if (routes.empty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Virtual soundcheck",
                                                "No track records a channel's input or strip. RECORD ALL CHANNELS sets them up.");
        return;
    }
    // Each channel's ALT input becomes its track's playback channel; the
    // global INPUT SELECT then switches every channel to ALT at once.
    for (auto& r : routes)
        for (auto& s : engine.getChannels())
            if (s->getNumber() == r.channelNumber)
                s->setAltInput (Recorder::playbackSource (r.playChannel), r.stereo ? Recorder::playbackSource (r.playChannel + 1) : -1);
    engine.setInputSelectGlobal (true);
    engine.setGlobalAlt (true);
    ctx.log.info ("Virtual soundcheck: " + juce::String ((int) routes.size()) + " channels play back their tracks (ALT inputs, INPUT SELECT GLOBAL)");
}

void RecorderPage::timerCallback()
{
    auto& project = session.getProject();
    modeKey.setButtonText (session.isLoopEnabled() ? "LOOP" : "PLAY");
    if (const auto format = session.getRecordFormat().shortName(); formatKey.getButtonText() != format)
        formatKey.setButtonText (format); // changed in the editor's menu
    if (soundcheckKey.isEnabled() != canVirtualSoundcheck())
        enableKey (soundcheckKey, canVirtualSoundcheck());
    // Lists follow project changes (a new project file: the session list too).
    const auto file = project.hasFolder() ? project.getFile() : juce::File();
    if (file != shownProject)
    {
        shownProject = file;
        sessions->reload();
    }
    if (project.getChangeCount() != shownChange)
    {
        shownChange = project.getChangeCount();
        markers->reload();
        sessions->repaint();
        resized();
        repaint();
        return;
    }
    // Time, meters and state: the player and the tiles.
    const auto time = clockText (session.isRecording() ? session.getRecordedLength() : session.getPosition(), project.getSampleRate());
    if (time != shownTime || session.getState() != shownState || session.isRecording())
    {
        shownTime = time;
        shownState = session.getState();
        repaint (player);
        repaint (tracksPanel);
        repaint (header);
    }
}

} // namespace livedaw::ui
