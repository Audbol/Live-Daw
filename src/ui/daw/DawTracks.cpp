#include "ui/daw/DawTracks.h"
#include "ui/RoutingGroups.h"

namespace livedaw::ui
{

using daw::int64;

namespace
{
    constexpr int kRowHeight = 24;
    constexpr int kResizeZone = 5;
    constexpr int kTwoRowHeight = 56;

    void paintKey (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, bool lit, juce::Colour litColour, bool enabled = true)
    {
        if (r.isEmpty())
            return;
        auto f = r.toFloat().reduced (1.0f);
        g.setColour (lit ? litColour : DawColours::key);
        g.fillRoundedRectangle (f, 2.5f);
        if (! lit)
        {
            g.setColour (DawColours::keyOutline);
            g.drawRoundedRectangle (f, 2.5f, 1.0f);
        }
        g.setColour (lit ? juce::Colours::black : (enabled ? DawColours::text : DawColours::textDim));
        g.setFont (juce::FontOptions (juce::jlimit (11.0f, 15.0f, f.getHeight() * 0.62f), juce::Font::bold));
        g.drawFittedText (text, f.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.7f);
    }

    void paintField (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& caption, const juce::String& value)
    {
        if (r.isEmpty())
            return;
        auto f = r.toFloat().reduced (1.0f);
        g.setColour (Dark::well);
        g.fillRoundedRectangle (f, 2.0f);
        g.setColour (DawColours::line);
        g.drawRoundedRectangle (f, 2.0f, 1.0f);
        auto t = f.reduced (4.0f, 0.0f).toNearestInt();
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.setColour (DawColours::textDim);
        const int cw = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), caption) + 4;
        g.drawText (caption, t.removeFromLeft (cw), juce::Justification::centredLeft);
        g.setColour (DawColours::text);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawFittedText (value, t, juce::Justification::centredLeft, 1, 0.7f);
    }

    float meterPos (float db) { return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f); }
} // namespace

//==============================================================================
DawTrackHeader::DawTrackHeader (DawEditorState& s, DawTrackList& l, int64 id) : state (s), list (l), trackId (id)
{
    setRepaintsOnMouseActivity (false);
}

const daw::TrackData* DawTrackHeader::track() const
{
    const int row = state.rowOf (trackId);
    return row >= 0 && row < (int) state.tracks.size() ? &state.tracks[(size_t) row] : nullptr;
}

void DawTrackHeader::resized()
{
    auto b = getLocalBounds().withTrimmedBottom (1);
    colourArea = b.removeFromLeft (8);
    meterArea = b.removeFromRight (12).reduced (2, 4);
    b.reduce (4, 3);
    auto row1 = b.removeFromTop (juce::jmin (kRowHeight, b.getHeight()));
    const int k = juce::jmin (26, row1.getHeight() + 2);
    soloKey = row1.removeFromRight (k);
    row1.removeFromRight (2);
    muteKey = row1.removeFromRight (k);
    row1.removeFromRight (2);
    armKey = row1.removeFromRight (k);
    row1.removeFromRight (4);
    nameArea = row1;
    sourceKey = playbackKey = gainKey = {};
    if (getHeight() >= kTwoRowHeight)
    {
        b.removeFromTop (3);
        auto row2 = b.removeFromTop (juce::jmin (kRowHeight - 2, b.getHeight()));
        const int w = row2.getWidth();
        sourceKey = row2.removeFromLeft (w * 40 / 100);
        row2.removeFromLeft (3);
        playbackKey = row2.removeFromLeft (w * 26 / 100);
        row2.removeFromLeft (3);
        gainKey = row2;
    }
}

void DawTrackHeader::paint (juce::Graphics& g)
{
    const auto* t = track();
    if (t == nullptr)
        return;
    const int row = state.rowOf (trackId);
    const bool selected = state.session.selectedTrack == trackId;
    g.fillAll (selected ? DawColours::headerSelected : DawColours::header);
    g.setColour (DawColours::line);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);
    g.fillRect (getWidth() - 1, 0, 1, getHeight());

    const auto colour = wingColour (t->colour);
    g.setColour (colour);
    g.fillRect (colourArea.withTrimmedBottom (1));
    if (selected)
    {
        g.setColour (colour.withAlpha (0.8f));
        g.fillRect (colourArea.getRight(), 0, 2, getHeight() - 1);
    }

    // Number and name.
    auto n = nameArea;
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.setColour (DawColours::textDim);
    g.drawText (juce::String (row + 1), n.removeFromLeft (20), juce::Justification::centredLeft);
    g.setColour (DawColours::text);
    g.setFont (juce::FontOptions (juce::jlimit (12.0f, 17.0f, (float) n.getHeight() * 0.7f), juce::Font::bold));
    g.drawFittedText (t->name + (t->stereo ? juce::String::fromUTF8 ("  \xe2\x97\x8f\xe2\x97\x8f") : juce::String()), n, juce::Justification::centredLeft, 1, 0.75f);

    const bool recording = state.session.isRecordingTrack (trackId);
    paintKey (g, armKey, "R", t->arm, recording && (juce::Time::getMillisecondCounter() / 400) % 2 == 0 ? DawColours::arm.brighter (0.4f) : DawColours::arm);
    paintKey (g, muteKey, "M", t->mute, DawColours::mute);
    paintKey (g, soloKey, "S", t->solo, DawColours::solo);

    const bool hardware = t->source.kind == Recorder::Source::Kind::HardwareInput;
    // An input is a source of the console's table (RECORD ALL CHANNELS takes each channel's): "LCL 3", "OSC 1".
    paintField (g, sourceKey, hardware ? "IN" : "SRC", hardware ? routing::sourceLabel (t->source.port) : daw::describeSource (t->source));
    const juce::String pb = t->stereo ? juce::String (t->playChannel + 1) + "-" + juce::String (t->playChannel + 2) : juce::String (t->playChannel + 1);
    paintField (g, playbackKey, "PB", pb);
    paintField (g, gainKey, "GAIN", t->gainDb <= -60.0f ? juce::String ("-inf") : (t->gainDb > 0.0f ? "+" : "") + juce::String (t->gainDb, 1));

    // Record level meter.
    g.setColour (juce::Colours::black);
    g.fillRect (meterArea);
    const int channels = t->stereo ? 2 : 1;
    const float w = (float) meterArea.getWidth() / (float) channels;
    for (int ch = 0; ch < channels; ++ch)
    {
        const float pos = meterPos (shownLevel[ch]);
        auto bar = juce::Rectangle<float> ((float) meterArea.getX() + w * (float) ch, (float) meterArea.getY(), w - (channels > 1 ? 1.0f : 0.0f), (float) meterArea.getHeight());
        auto lit = bar.removeFromBottom (bar.getHeight() * pos);
        g.setGradientFill (juce::ColourGradient (Colours::meterGreen, 0.0f, (float) meterArea.getBottom(), Colours::meterRed, 0.0f, (float) meterArea.getY(), false));
        g.fillRect (lit);
    }
    if (t->arm)
    {
        g.setColour (DawColours::arm.withAlpha (0.6f));
        g.drawRect (meterArea.expanded (1), 1);
    }
}

void DawTrackHeader::updateMeter()
{
    const auto* t = track();
    if (t == nullptr)
        return;
    bool changed = false;
    for (int ch = 0; ch < 2; ++ch)
    {
        const float db = state.session.getTrackLevel (trackId, ch);
        if (std::abs (db - shownLevel[ch]) > 0.5f)
        {
            shownLevel[ch] = db;
            changed = true;
        }
    }
    if (changed)
        repaint (meterArea.expanded (2));
    if (state.session.isRecordingTrack (trackId))
        repaint (armKey);
}

DawTrackHeader::Part DawTrackHeader::partAt (juce::Point<int> p) const
{
    if (p.y >= getHeight() - kResizeZone)
        return Part::Resize;
    if (colourArea.contains (p))
        return Part::Colour;
    if (armKey.contains (p))
        return Part::Arm;
    if (muteKey.contains (p))
        return Part::Mute;
    if (soloKey.contains (p))
        return Part::Solo;
    if (sourceKey.contains (p))
        return Part::Source;
    if (playbackKey.contains (p))
        return Part::Playback;
    if (gainKey.contains (p))
        return Part::Gain;
    return Part::Name;
}

void DawTrackHeader::mouseMove (const juce::MouseEvent& e)
{
    const auto part = partAt (e.getPosition());
    setMouseCursor (part == Part::Resize ? juce::MouseCursor::UpDownResizeCursor
                                         : (part == Part::Gain ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor));
}

void DawTrackHeader::mouseDown (const juce::MouseEvent& e)
{
    const auto* t = track();
    if (t == nullptr)
        return;
    dragged = false;
    downPart = partAt (e.getPosition());
    downGain = t->gainDb;
    downHeight = getHeight();
    if (state.session.selectedTrack != trackId)
        state.selectTrack (trackId);

    if (e.mods.isPopupMenu())
    {
        downPart = Part::None;
        showMenu();
        return;
    }

    auto& project = state.project;
    switch (downPart)
    {
        case Part::Colour:
            showColourMenu (t->colour, this, [this] (int c)
            {
                state.project.beginEdit ("Track colour");
                state.project.setTrackProperty (trackId, daw::ids::colour, c);
            });
            break;
        case Part::Arm:
            if (state.session.isRecordingTrack (trackId))
                break; // can't disarm a track while it records
            project.setTrackProperty (trackId, daw::ids::arm, ! t->arm, false);
            break;
        case Part::Mute:
            project.beginEdit ("Mute track");
            project.setTrackProperty (trackId, daw::ids::mute, ! t->mute);
            break;
        case Part::Solo:
            project.beginEdit ("Solo track");
            if (e.mods.isCommandDown())
            {
                // Ctrl: exclusive solo.
                for (auto& other : state.tracks)
                    project.setTrackProperty (other.id, daw::ids::solo, other.id == trackId && ! t->solo);
            }
            else
            {
                project.setTrackProperty (trackId, daw::ids::solo, ! t->solo);
            }
            break;
        case Part::Source:
            showSourceMenu (state.ctx, t->source, this, [this] (Recorder::Source s)
            {
                state.project.beginEdit ("Track source");
                state.project.setTrackSource (trackId, s);
            });
            break;
        case Part::Playback:
            playbackMenu();
            break;
        case Part::Gain:
            project.beginEdit ("Track gain");
            break;
        case Part::Name:
        case Part::Resize:
        case Part::None:
            break;
    }
}

void DawTrackHeader::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragged && e.getDistanceFromDragStart() < 3)
        return;
    dragged = true;
    switch (downPart)
    {
        case Part::Gain:
        {
            const float db = juce::jlimit (-60.0f, 12.0f, downGain - (float) e.getDistanceFromDragStartY() * (e.mods.isShiftDown() ? 0.05f : 0.25f));
            state.project.setTrackProperty (trackId, daw::ids::gain, std::round (db * 10.0f) / 10.0f);
            break;
        }
        case Part::Resize:
        {
            const int h = juce::jlimit (DawEditorState::kMinTrackHeight, DawEditorState::kMaxTrackHeight, downHeight + e.getDistanceFromDragStartY());
            state.project.setTrackProperty (trackId, daw::ids::height, h, false);
            break;
        }
        case Part::Name:
            list.dragTrack (trackId, e.getEventRelativeTo (&list).y);
            break;
        case Part::Colour:
        case Part::Arm:
        case Part::Mute:
        case Part::Solo:
        case Part::Source:
        case Part::Playback:
        case Part::None:
            break;
    }
}

void DawTrackHeader::mouseUp (const juce::MouseEvent& e)
{
    if (dragged && downPart == Part::Name)
        list.dropTrack (trackId, e.getEventRelativeTo (&list).y);
    downPart = Part::None;
    dragged = false;
}

void DawTrackHeader::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto part = partAt (e.getPosition());
    if (part == Part::Name)
        rename();
    else if (part == Part::Gain)
    {
        state.project.beginEdit ("Track gain");
        state.project.setTrackProperty (trackId, daw::ids::gain, 0.0f);
    }
    else if (part == Part::Resize)
        state.project.setTrackProperty (trackId, daw::ids::height, 0, false);
}

void DawTrackHeader::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const auto* t = track();
    if (t != nullptr && partAt (e.getPosition()) == Part::Gain)
    {
        state.project.beginEdit ("Track gain");
        state.project.setTrackProperty (trackId, daw::ids::gain, juce::jlimit (-60.0f, 12.0f, t->gainDb + (w.deltaY > 0 ? 0.5f : -0.5f)));
        return;
    }
    // Otherwise scroll the track list.
    getParentComponent()->mouseWheelMove (e.getEventRelativeTo (getParentComponent()), w);
}

void DawTrackHeader::rename()
{
    const auto* t = track();
    if (t == nullptr)
        return;
    askForText (getTopLevelComponent(), "RENAME TRACK", "Name:", t->name, [this] (const juce::String& name)
    {
        if (name.isEmpty())
            return;
        state.project.beginEdit ("Rename track");
        state.project.setTrackProperty (trackId, daw::ids::name, name);
    });
}

void DawTrackHeader::playbackMenu()
{
    const auto* t = track();
    if (t == nullptr)
        return;
    juce::PopupMenu m;
    m.addSectionHeader ("PLAYBACK CHANNEL");
    const int n = t->stereo ? 2 : 1;
    for (int group = 0; group < Recorder::kNumPlaybackChannels; group += 16)
    {
        juce::PopupMenu sub;
        for (int ch = group; ch < group + 16 && ch + n <= Recorder::kNumPlaybackChannels; ch += 1)
        {
            juce::String used;
            for (auto& other : state.tracks)
                if (other.id != trackId && ch < other.playChannel + (other.stereo ? 2 : 1) && other.playChannel < ch + n)
                    used = "  (" + other.name + ")";
            const auto label = "PLAY " + (n == 2 ? juce::String (ch + 1) + "-" + juce::String (ch + 2) : juce::String (ch + 1)) + used;
            sub.addItem (label, true, ch == t->playChannel, [this, ch]
            {
                state.project.beginEdit ("Playback channel");
                state.project.setTrackProperty (trackId, daw::ids::playChannel, ch);
            });
        }
        m.addSubMenu ("PLAY " + juce::String (group + 1) + "-" + juce::String (group + 16), sub, true, nullptr,
                      t->playChannel >= group && t->playChannel < group + 16);
    }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
}

void DawTrackHeader::showMenu()
{
    const auto* t = track();
    if (t == nullptr)
        return;
    const auto copy = *t;
    juce::PopupMenu m;
    m.addSectionHeader (copy.name);
    m.addItem ("Rename...", [this] { rename(); });
    m.addItem ("Colour...", [this, copy]
    {
        showColourMenu (copy.colour, this, [this] (int c)
        {
            state.project.beginEdit ("Track colour");
            state.project.setTrackProperty (trackId, daw::ids::colour, c);
        });
    });
    m.addItem ("Stereo", ! state.session.isRecordingTrack (trackId), copy.stereo, [this, copy]
    {
        state.project.beginEdit (copy.stereo ? "Mono track" : "Stereo track");
        state.project.setTrackProperty (trackId, daw::ids::stereo, ! copy.stereo);
    });
    m.addItem ("Record source...", [this, copy]
    {
        showSourceMenu (state.ctx, copy.source, this, [this] (Recorder::Source s)
        {
            state.project.beginEdit ("Track source");
            state.project.setTrackSource (trackId, s);
        });
    });
    m.addItem ("Playback channel...", [this] { playbackMenu(); });
    juce::PopupMenu heights;
    const std::pair<const char*, int> sizes[] = { { "Small", 40 }, { "Normal", daw::Project::kDefaultTrackHeight }, { "Large", 120 }, { "Huge", 200 } };
    for (auto& [name, h] : sizes)
        heights.addItem (name, true, getHeight() == h, [this, h = h] { state.project.setTrackProperty (trackId, daw::ids::height, h, false); });
    heights.addItem ("All tracks this size", [this]
    {
        for (auto& other : state.tracks)
            state.project.setTrackProperty (other.id, daw::ids::height, getHeight(), false);
    });
    m.addSubMenu ("Height", heights);
    m.addSeparator();
    m.addItem ("Add track below", [this] { state.addTrack(); });
    m.addItem ("Move up", state.rowOf (trackId) > 0, false, [this]
    {
        state.project.beginEdit ("Move track");
        state.project.moveTrack (trackId, state.rowOf (trackId) - 1);
    });
    m.addItem ("Move down", state.rowOf (trackId) < (int) state.rows.size() - 1, false, [this]
    {
        state.project.beginEdit ("Move track");
        state.project.moveTrack (trackId, state.rowOf (trackId) + 1);
    });
    m.addItem ("Delete track", [this] { state.deleteTrack (trackId); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

//==============================================================================
DawTrackList::DawTrackList (DawEditorState& s) : state (s)
{
    addKey.onClick = [this] { state.addTrack(); };
    addKey.setTooltip ("Add a track (after the selected one)");
    addAndMakeVisible (addKey);
    startTimerHz (30);
}

void DawTrackList::update()
{
    // Keep the headers of tracks that still exist; make the new ones.
    std::vector<std::unique_ptr<DawTrackHeader>> next;
    for (auto& row : state.rows)
    {
        std::unique_ptr<DawTrackHeader> h;
        for (auto& existing : headers)
            if (existing != nullptr && existing->getTrackId() == row.id)
                h = std::move (existing);
        if (h == nullptr)
        {
            h = std::make_unique<DawTrackHeader> (state, *this, row.id);
            addChildComponent (*h);
        }
        next.push_back (std::move (h));
    }
    headers = std::move (next);
    resized();
    for (auto& h : headers)
        h->repaint();
}

void DawTrackList::resized()
{
    for (size_t i = 0; i < headers.size() && i < state.rows.size(); ++i)
    {
        const auto& row = state.rows[i];
        const auto r = juce::Rectangle<int> (0, row.top - state.scrollY, getWidth(), row.height);
        headers[i]->setVisible (r.intersects (getLocalBounds()));
        if (headers[i]->isVisible())
            headers[i]->setBounds (r);
    }
    const int bottom = state.totalHeight - state.scrollY;
    addKey.setBounds (juce::Rectangle<int> (8, bottom + 8, juce::jmin (120, getWidth() - 16), 28));
}

void DawTrackList::paint (juce::Graphics& g)
{
    g.fillAll (DawColours::background);
}

void DawTrackList::paintOverChildren (juce::Graphics& g)
{
    if (dropLine >= 0)
    {
        g.setColour (DawColours::selection);
        g.fillRect (0, dropLine - 1, getWidth(), 3);
    }
}

void DawTrackList::timerCallback()
{
    for (auto& h : headers)
        if (h->isVisible())
            h->updateMeter();
}

int DawTrackList::insertionIndex (int y) const
{
    const int contentY = y + state.scrollY;
    for (int i = 0; i < (int) state.rows.size(); ++i)
        if (contentY < state.rows[(size_t) i].top + state.rows[(size_t) i].height / 2)
            return i;
    return (int) state.rows.size();
}

void DawTrackList::dragTrack (int64, int y)
{
    const int index = insertionIndex (y);
    const int line = index < (int) state.rows.size() ? state.rows[(size_t) index].top - state.scrollY : state.totalHeight - state.scrollY;
    if (line != dropLine)
    {
        dropLine = line;
        repaint();
    }
}

void DawTrackList::dropTrack (int64 trackId, int y)
{
    dropLine = -1;
    repaint();
    int index = insertionIndex (y);
    const int from = state.rowOf (trackId);
    if (from < 0)
        return;
    if (index > from)
        --index;
    if (index != from)
    {
        state.project.beginEdit ("Move track");
        state.project.moveTrack (trackId, index);
    }
}

void DawTrackList::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    state.scrollY -= juce::roundToInt (w.deltaY * 300.0f);
    state.clampScroll();
    if (state.viewChanged)
        state.viewChanged();
}

void DawTrackList::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu m;
        m.addItem ("Add track", [this] { state.addTrack(); });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }
}

} // namespace livedaw::ui
