#include "ui/daw/DawTimeline.h"

namespace livedaw::ui
{

using daw::int64;

namespace
{
    constexpr int kEdgeZone = 6;      // px at a clip's ends that trim it
    constexpr int kHandleZone = 7;    // px around a fade handle
    constexpr int kSnapPixels = 8;    // magnetic edges
    constexpr int kNameStrip = 15;    // clip name bar (tall enough clips)

    juce::String gainText (float db)
    {
        if (db <= -60.0f)
            return "-inf dB";
        return (db > 0.0f ? "+" : "") + juce::String (db, 1) + " dB";
    }

    juce::Colour inkOn (juce::Colour c) { return c.getPerceivedBrightness() > 0.55f ? juce::Colours::black : juce::Colours::white; }
} // namespace

//==============================================================================
DawRuler::DawRuler (DawEditorState& s) : state (s)
{
    setBufferedToImage (true);
}

void DawRuler::paint (juce::Graphics& g)
{
    auto b = getLocalBounds();
    g.fillAll (DawColours::bar);
    auto timeArea = b.removeFromTop (kTimeHeight);
    auto markerArea = b;
    const auto clip = g.getClipBounds();
    auto& project = state.project;

    // The loop range.
    if (project.getLoopEnd() > project.getLoopStart())
    {
        const float x1 = state.xOf (project.getLoopStart()), x2 = state.xOf (project.getLoopEnd());
        const bool on = project.isLoopOn();
        g.setColour (DawColours::loop.withAlpha (on ? 0.42f : 0.18f));
        g.fillRect (x1, (float) timeArea.getY(), x2 - x1, (float) timeArea.getHeight());
        g.setColour (DawColours::loop.withAlpha (on ? 1.0f : 0.5f));
        g.fillRect (x1, (float) timeArea.getY(), 2.0f, (float) timeArea.getHeight());
        g.fillRect (x2 - 2.0f, (float) timeArea.getY(), 2.0f, (float) timeArea.getHeight());
    }

    // Ticks and labels.
    const auto grid = state.grid();
    double major = 0.0, minor = 0.0;
    grid.rulerSteps (state.showBars, state.samplesPerPixel, major, minor);
    const double first = state.viewStart + (double) clip.getX() * state.samplesPerPixel;
    const double last = state.viewStart + (double) clip.getRight() * state.samplesPerPixel;
    g.setColour (DawColours::textDim.withAlpha (0.6f));
    if (minor > 0.0 && minor < major)
        for (double s = std::floor (first / minor) * minor; s <= last; s += minor)
            g.fillRect (state.xOf ((int64) s), (float) timeArea.getBottom() - 5.0f, 1.0f, 5.0f);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    for (double s = std::floor ((first - major * 2.0) / major) * major; s <= last; s += major)
    {
        if (s < 0.0)
            continue;
        const float x = state.xOf ((int64) std::llround (s));
        g.setColour (DawColours::textDim);
        g.fillRect (x, (float) timeArea.getY() + 4.0f, 1.0f, (float) timeArea.getHeight() - 4.0f);
        g.setColour (DawColours::text);
        const auto label = state.showBars ? grid.formatBarLabel ((int64) std::llround (s), major) : grid.formatTimeLabel ((int64) std::llround (s), major);
        g.drawText (label, juce::Rectangle<float> (x + 4.0f, (float) timeArea.getY() + 2.0f, 90.0f, 14.0f), juce::Justification::centredLeft);
    }

    // Marker lane.
    g.setColour (juce::Colour (0xff101010));
    g.fillRect (markerArea);
    g.setColour (DawColours::line);
    g.fillRect (timeArea.getX(), timeArea.getBottom() - 1, timeArea.getWidth(), 1);
    g.fillRect (b.getX(), getHeight() - 1, getWidth(), 1);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    for (auto& m : project.getMarkers())
    {
        const float x = state.xOf (m.position);
        if (x < (float) clip.getX() - 200.0f || x > (float) clip.getRight() + 2.0f)
            continue;
        const float w = juce::jmin (160.0f, (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), m.name) + 12.0f);
        auto flag = juce::Rectangle<float> (x, (float) markerArea.getY() + 2.0f, w, (float) markerArea.getHeight() - 4.0f);
        g.setColour (DawColours::marker);
        g.fillRect (x, (float) markerArea.getY(), 1.5f, (float) markerArea.getHeight());
        g.fillRoundedRectangle (flag.withTrimmedLeft (1.0f), 2.0f);
        g.setColour (juce::Colours::black);
        g.drawFittedText (m.name, flag.reduced (5.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
    }
}

int64 DawRuler::markerAt (juce::Point<int> p) const
{
    if (p.y < kTimeHeight)
        return 0;
    int64 best = 0;
    float bestX = -1.0e9f;
    for (auto& m : state.project.getMarkers())
    {
        const float x = state.xOf (m.position);
        // A marker's flag extends right of its line; the rightmost one under the mouse wins.
        if ((float) p.x >= x - 4.0f && (float) p.x <= x + 60.0f && x > bestX)
        {
            best = m.id;
            bestX = x;
        }
    }
    return best;
}

void DawRuler::markerMenu (daw::int64 markerId)
{
    juce::PopupMenu m;
    m.addItem ("Rename...", [this, markerId]
    {
        for (auto& mk : state.project.getMarkers())
            if (mk.id == markerId)
                askForText (getTopLevelComponent(), "RENAME MARKER", "Name:", mk.name, [this, mk] (const juce::String& name)
                {
                    state.project.beginEdit ("Rename marker");
                    state.project.setMarker (mk.id, mk.position, name);
                });
    });
    m.addItem ("Move to the playhead", [this, markerId]
    {
        for (auto& mk : state.project.getMarkers())
            if (mk.id == markerId)
            {
                state.project.beginEdit ("Move marker");
                state.project.setMarker (mk.id, state.session.getPosition(), mk.name);
            }
    });
    m.addItem ("Delete", [this, markerId]
    {
        state.project.beginEdit ("Delete marker");
        state.project.removeMarker (markerId);
    });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

void DawRuler::mouseMove (const juce::MouseEvent& e)
{
    auto& p = state.project;
    const bool loopEdge = e.y < kTimeHeight && p.getLoopEnd() > p.getLoopStart()
                          && (std::abs ((float) e.x - state.xOf (p.getLoopStart())) < 5.0f || std::abs ((float) e.x - state.xOf (p.getLoopEnd())) < 5.0f);
    setMouseCursor (loopEdge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void DawRuler::mouseDown (const juce::MouseEvent& e)
{
    auto& p = state.project;
    dragged = false;
    drag = Drag::None;
    const auto at = state.snapToGrid (state.sampleAt ((float) e.x), e.mods.isShiftDown());
    if (e.y >= kTimeHeight)
    {
        dragMarker = markerAt (e.getPosition());
        if (dragMarker != 0)
        {
            if (e.mods.isPopupMenu())
                markerMenu (dragMarker);
            else
                drag = Drag::Marker;
        }
        return;
    }
    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu m;
        m.addItem ("Loop on", true, p.isLoopOn(), [this] { state.session.setLoopEnabled (! state.project.isLoopOn()); });
        m.addItem ("Clear the loop range", p.getLoopEnd() > p.getLoopStart(), false, [this]
        {
            state.project.beginEdit ("Clear loop");
            state.project.setLoop (0, 0);
            state.session.setLoopEnabled (false);
        });
        m.addItem ("Add marker here", [this, at]
        {
            state.project.beginEdit ("Add marker");
            state.project.addMarker (at);
        });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
        return;
    }
    if (p.getLoopEnd() > p.getLoopStart() && std::abs ((float) e.x - state.xOf (p.getLoopStart())) < 5.0f)
        drag = Drag::LoopStart;
    else if (p.getLoopEnd() > p.getLoopStart() && std::abs ((float) e.x - state.xOf (p.getLoopEnd())) < 5.0f)
        drag = Drag::LoopEnd;
    else
        drag = Drag::Locate;
    dragAnchor = at;
}

void DawRuler::mouseDrag (const juce::MouseEvent& e)
{
    if (drag == Drag::None || (! dragged && e.getDistanceFromDragStart() < 3))
        return;
    auto& p = state.project;
    const auto at = state.snapToGrid (state.sampleAt ((float) e.x), e.mods.isShiftDown());
    if (! dragged)
    {
        dragged = true;
        state.session.setGestureActive (true);
        p.beginEdit (drag == Drag::Marker ? "Move marker" : "Loop range");
        if (drag == Drag::Locate)
            drag = Drag::LoopNew;
    }
    switch (drag)
    {
        case Drag::LoopNew:
            p.setLoop (dragAnchor, at);
            if (! p.isLoopOn())
                state.session.setLoopEnabled (true);
            break;
        case Drag::LoopStart:
            p.setLoop (at, p.getLoopEnd());
            break;
        case Drag::LoopEnd:
            p.setLoop (p.getLoopStart(), at);
            break;
        case Drag::Marker:
            for (auto& m : p.getMarkers())
                if (m.id == dragMarker)
                    p.setMarker (m.id, at, m.name);
            break;
        case Drag::None:
        case Drag::Locate:
            break;
    }
}

void DawRuler::mouseUp (const juce::MouseEvent& e)
{
    if (dragged)
    {
        state.session.setGestureActive (false);
        if (drag == Drag::LoopNew || drag == Drag::LoopStart || drag == Drag::LoopEnd)
            state.session.pushPlaylistNow();
    }
    else if (drag == Drag::Locate)
    {
        state.session.locate (state.snapToGrid (state.sampleAt ((float) e.x), e.mods.isShiftDown()));
    }
    else if (drag == Drag::Marker)
    {
        for (auto& m : state.project.getMarkers())
            if (m.id == dragMarker)
                state.session.locate (m.position);
    }
    drag = Drag::None;
    dragged = false;
}

void DawRuler::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.y < kTimeHeight)
        return;
    const auto id = markerAt (e.getPosition());
    if (id != 0)
    {
        for (auto& m : state.project.getMarkers())
            if (m.id == id)
                askForText (getTopLevelComponent(), "RENAME MARKER", "Name:", m.name, [this, m] (const juce::String& name)
                {
                    state.project.beginEdit ("Rename marker");
                    state.project.setMarker (m.id, m.position, name);
                });
        return;
    }
    state.project.beginEdit ("Add marker");
    state.project.addMarker (state.snapToGrid (state.sampleAt ((float) e.x), e.mods.isShiftDown()));
}

void DawRuler::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
        state.zoomAround (state.samplesPerPixel * (w.deltaY > 0 ? 0.8 : 1.25), (float) e.x);
    else
    {
        state.viewStart -= (double) (w.deltaY + w.deltaX) * 400.0 * state.samplesPerPixel;
        state.clampScroll();
        if (state.viewChanged)
            state.viewChanged();
    }
}

//==============================================================================
DawLanes::DawLanes (DawEditorState& s, DawWaveforms& w) : state (s), waves (w)
{
    setBufferedToImage (true);
    setOpaque (true);
}

juce::Rectangle<int> DawLanes::rowBounds (int row) const
{
    const auto& r = state.rows[(size_t) row];
    return { 0, r.top - state.scrollY, getWidth(), r.height };
}

juce::Rectangle<int> DawLanes::clipBounds (int row, const daw::ClipData& c) const
{
    const auto lane = rowBounds (row);
    const float x1 = juce::jlimit (-100.0f, (float) getWidth() + 100.0f, state.xOf (c.start));
    const float x2 = juce::jlimit (-100.0f, (float) getWidth() + 100.0f, state.xOf (c.end()));
    const int left = (int) std::floor (x1), right = juce::jmax (left + 2, (int) std::ceil (x2));
    return { left, lane.getY() + 1, right - left, lane.getHeight() - 3 };
}

float DawLanes::gainY (juce::Rectangle<int> clip, float gainDb)
{
    // -60 dB at the bottom, +12 dB at the top, 0 dB at 5/6.
    const float pos = juce::jlimit (0.0f, 1.0f, (gainDb + 60.0f) / 72.0f);
    const auto wave = clip.withTrimmedTop (clip.getHeight() >= 40 ? kNameStrip : 0);
    return (float) wave.getBottom() - 2.0f - pos * (float) (wave.getHeight() - 4);
}

DawLanes::Hit DawLanes::hitAt (juce::Point<int> p) const
{
    Hit h;
    h.row = state.rowAt (p.y);
    if (h.row < 0)
        return h;
    const auto& row = state.clips[(size_t) h.row];
    for (int i = (int) row.size(); --i >= 0;)
    {
        const auto& c = row[(size_t) i];
        const auto cb = clipBounds (h.row, c);
        if (! cb.expanded (1, 0).contains (p))
            continue;
        h.index = i;
        const float fadeInX = state.xOf (c.start + c.fadeIn), fadeOutX = state.xOf (c.end() - c.fadeOut);
        const bool top = p.y - cb.getY() <= kHandleZone + 2;
        if (cb.getWidth() > 24 && top && std::abs ((float) p.x - fadeInX) <= (float) kHandleZone)
            h.zone = Zone::FadeIn;
        else if (cb.getWidth() > 24 && top && std::abs ((float) p.x - fadeOutX) <= (float) kHandleZone)
            h.zone = Zone::FadeOut;
        else if (cb.getWidth() > 14 && p.x - cb.getX() <= kEdgeZone)
            h.zone = Zone::TrimStart;
        else if (cb.getWidth() > 14 && cb.getRight() - p.x <= kEdgeZone)
            h.zone = Zone::TrimEnd;
        else if (cb.getHeight() >= 30 && std::abs ((float) p.y - gainY (cb, c.gainDb)) <= 3.0f)
            h.zone = Zone::Gain;
        else
            h.zone = Zone::Body;
        return h;
    }
    return h;
}

std::vector<int64> DawLanes::snapEdges (bool excludeSelection) const
{
    std::vector<int64> edges { 0, state.session.getPosition() };
    for (auto& row : state.clips)
        for (auto& c : row)
            if (! (excludeSelection && state.isSelected (c.id)))
            {
                edges.push_back (c.start);
                edges.push_back (c.end());
            }
    for (auto& m : state.project.getMarkers())
        edges.push_back (m.position);
    if (state.project.getLoopEnd() > state.project.getLoopStart())
    {
        edges.push_back (state.project.getLoopStart());
        edges.push_back (state.project.getLoopEnd());
    }
    return edges;
}

int64 DawLanes::snapPosition (int64 position, bool bypass, bool excludeSelection) const
{
    if (bypass)
        return position;
    bool snapped = false;
    const auto edge = daw::snapToEdges (position, snapEdges (excludeSelection), (int64) (kSnapPixels * state.samplesPerPixel), &snapped);
    return snapped ? edge : state.snapToGrid (position);
}

//==============================================================================
void DawLanes::paintGrid (juce::Graphics& g, juce::Rectangle<int> area) const
{
    const auto grid = state.grid();
    double major = 0.0, minor = 0.0;
    grid.rulerSteps (state.showBars, state.samplesPerPixel, major, minor);
    const double first = state.viewStart + (double) area.getX() * state.samplesPerPixel;
    const double last = state.viewStart + (double) area.getRight() * state.samplesPerPixel;
    if (minor > 0.0 && minor < major)
    {
        g.setColour (DawColours::gridMinor);
        for (double s = std::floor (first / minor) * minor; s <= last; s += minor)
            g.fillRect (state.xOf ((int64) std::llround (s)), (float) area.getY(), 1.0f, (float) area.getHeight());
    }
    g.setColour (DawColours::gridMajor);
    for (double s = std::floor (first / major) * major; s <= last; s += major)
        g.fillRect (state.xOf ((int64) std::llround (s)), (float) area.getY(), 1.0f, (float) area.getHeight());
}

void DawLanes::paint (juce::Graphics& g)
{
    const auto clip = g.getClipBounds();
    g.setColour (DawColours::background);
    g.fillRect (clip);

    const int numRows = (int) state.rows.size();
    int firstRow = -1, lastRow = -1;
    for (int i = 0; i < numRows; ++i)
    {
        const auto r = rowBounds (i);
        if (! r.intersects (clip))
            continue;
        if (firstRow < 0)
            firstRow = i;
        lastRow = i;
        const bool selected = state.rows[(size_t) i].id == state.session.selectedTrack;
        g.setColour (selected ? DawColours::laneSelected : (i % 2 == 0 ? DawColours::lane : DawColours::laneAlt));
        g.fillRect (r);
    }

    // Grid over the lanes, the loop range shaded.
    const int tracksBottom = state.totalHeight - state.scrollY;
    auto gridArea = clip.withBottom (juce::jmin (clip.getBottom(), tracksBottom));
    if (! gridArea.isEmpty())
        paintGrid (g, gridArea);
    auto& p = state.project;
    if (p.isLoopOn() && p.getLoopEnd() > p.getLoopStart())
    {
        const float x1 = state.xOf (p.getLoopStart()), x2 = state.xOf (p.getLoopEnd());
        g.setColour (DawColours::loop.withAlpha (0.06f));
        g.fillRect (x1, 0.0f, x2 - x1, (float) getHeight());
    }

    if (firstRow >= 0)
        for (int i = firstRow; i <= lastRow; ++i)
        {
            const auto lane = rowBounds (i);
            g.setColour (DawColours::line);
            g.fillRect (lane.getX(), lane.getBottom() - 1, lane.getWidth(), 1);
            for (auto& c : state.clips[(size_t) i])
            {
                const auto cb = clipBounds (i, c);
                if (cb.intersects (clip))
                    paintClip (g, i, c, cb);
            }
            if (state.session.isRecordingTrack (state.rows[(size_t) i].id))
                paintTake (g, i, lane);
        }

    // Drop target while dragging files in.
    if (dropRow > -2)
    {
        const auto r = dropRow >= 0 ? rowBounds (dropRow) : juce::Rectangle<int> (0, tracksBottom, getWidth(), daw::Project::kDefaultTrackHeight);
        g.setColour (DawColours::selection.withAlpha (0.15f));
        g.fillRect (r);
        g.setColour (DawColours::selection);
        g.fillRect (dropX - 1.0f, (float) r.getY(), 2.0f, (float) r.getHeight());
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawText (dropRow >= 0 ? "IMPORT HERE" : "IMPORT ON NEW TRACKS", juce::Rectangle<float> (dropX + 6.0f, (float) r.getY() + 4.0f, 200.0f, 18.0f),
                    juce::Justification::centredLeft);
    }

    if (! rubber.isEmpty())
    {
        g.setColour (DawColours::selection.withAlpha (0.12f));
        g.fillRect (rubber);
        g.setColour (DawColours::selection.withAlpha (0.8f));
        g.drawRect (rubber, 1);
    }

    if (numRows == 0)
    {
        g.setColour (DawColours::textDim);
        g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
        g.drawFittedText ("NO TRACKS\nAdd one with + TRACK, drop audio files here, or use RECORD ALL CHANNELS on the recorder screen.",
                          getLocalBounds().reduced (40), juce::Justification::centred, 3);
    }
}

void DawLanes::paintClip (juce::Graphics& g, int row, const daw::ClipData& c, juce::Rectangle<int> area) const
{
    const auto& track = state.tracks[(size_t) row];
    const auto colour = wingColour (track.colour);
    const bool selected = state.isSelected (c.id);
    const bool hovered = hover.row == row && hover.index >= 0 && hover.index < (int) state.clips[(size_t) row].size()
                         && state.clips[(size_t) row][(size_t) hover.index].id == c.id;
    auto r = area.toFloat();
    const auto base = c.mute ? juce::Colour (0xff3a3a3a) : colour;

    g.setColour (base.withAlpha (c.mute ? 0.25f : (selected ? 0.42f : 0.30f)));
    g.fillRoundedRectangle (r, 3.0f);

    // Name bar.
    auto wave = r;
    if (r.getHeight() >= 40.0f)
    {
        auto bar = wave.removeFromTop ((float) kNameStrip);
        g.setColour (base.withAlpha (c.mute ? 0.45f : 0.85f));
        g.fillRoundedRectangle (bar, 3.0f);
        g.fillRect (bar.withTrimmedTop (bar.getHeight() * 0.5f));
        if (bar.getWidth() > 24.0f)
        {
            g.setColour (inkOn (base));
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            auto text = bar.reduced (5.0f, 0.0f);
            text.setLeft (juce::jmax (text.getX(), 4.0f)); // keep the name in view
            g.drawFittedText ((c.mute ? "[M] " : "") + c.name, text.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
            if (std::abs (c.gainDb) > 0.05f && bar.getWidth() > 140.0f)
                g.drawText (gainText (c.gainDb), text.toNearestInt(), juce::Justification::centredRight);
        }
    }

    // Waveform: only the part being repainted. An offline clip (its file
    // missing: a drive unplugged) shows so instead, and plays silence.
    const auto file = state.project.resolve (c.file);
    const bool offline = state.session.isOffline (c);
    auto* thumb = offline ? nullptr : waves.get (file);
    const auto visible = wave.getIntersection (g.getClipBounds().toFloat()).reduced (0.0f, 1.0f);
    if (thumb != nullptr && visible.getWidth() > 0.0f)
    {
        const double rate = state.project.getSampleRate();
        const double s0 = state.viewStart + (double) visible.getX() * state.samplesPerPixel;
        const double s1 = state.viewStart + (double) visible.getRight() * state.samplesPerPixel;
        const double t0 = ((double) c.fileOffset + (s0 - (double) c.start)) / rate;
        const double t1 = ((double) c.fileOffset + (s1 - (double) c.start)) / rate;
        const float gain = c.gainDb <= -60.0f ? 0.0f : juce::jmin (4.0f, juce::Decibels::decibelsToGain (c.gainDb));
        g.setColour (c.mute ? juce::Colour (0xff6a6a6a) : colour.interpolatedWith (juce::Colours::white, 0.45f));
        if (thumb->getTotalLength() > 0.0)
            thumb->drawChannels (g, visible.toNearestInt(), t0, t1, gain);
        else
        {
            g.setColour (DawColours::textDim);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText ("...", wave.reduced (6.0f, 2.0f).toNearestInt(), juce::Justification::centredLeft);
        }
    }
    else if (thumb == nullptr)
    {
        // Diagonal hatching over the clip, then the reason.
        {
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (wave.toNearestInt());
            g.setColour (DawColours::record.withAlpha (0.28f));
            const auto shown = wave.getIntersection (g.getClipBounds().toFloat());
            const float from = wave.getX() + 9.0f * std::floor ((shown.getX() - wave.getHeight() - wave.getX()) / 9.0f);
            for (float x = from; x < shown.getRight(); x += 9.0f)
                g.drawLine (x, wave.getBottom(), x + wave.getHeight(), wave.getY(), 1.5f);
        }
        if (wave.getWidth() > 40.0f)
        {
            g.setColour (DawColours::record);
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.drawFittedText ((wave.getWidth() > 160.0f ? "OFFLINE - FILE MISSING: " : "OFFLINE ") + file.getFullPathName(),
                              wave.reduced (6.0f, 2.0f).toNearestInt(), juce::Justification::centredLeft, 2);
        }
    }

    paintFades (g, c, r, wave, hovered || selected);

    // Gain line.
    if ((hovered || std::abs (c.gainDb) > 0.05f) && r.getHeight() >= 30.0f)
    {
        const float y = gainY (area, c.gainDb);
        g.setColour (DawColours::gainLine.withAlpha (hovered ? 0.9f : 0.45f));
        g.fillRect (r.getX() + 2.0f, y - 0.5f, r.getWidth() - 4.0f, 1.0f);
        if (hovered && hover.zone == Zone::Gain)
        {
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.drawText (gainText (c.gainDb), juce::Rectangle<float> (r.getX() + 6.0f, y - 15.0f, 80.0f, 13.0f), juce::Justification::centredLeft);
        }
    }

    g.setColour (selected ? DawColours::selection : base.withAlpha (0.7f));
    g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, selected ? 2.0f : 1.0f);
}

void DawLanes::paintFades (juce::Graphics& g, const daw::ClipData& c, juce::Rectangle<float> clipRect, juce::Rectangle<float> r, bool showHandles) const
{
    auto curve = [&] (bool in)
    {
        const int64 len = in ? c.fadeIn : c.fadeOut;
        const float w = (float) ((double) len / state.samplesPerPixel);
        if (w < 0.5f)
            return;
        const auto shape = in ? c.fadeInShape : c.fadeOutShape;
        const int steps = juce::jlimit (4, 48, (int) (w / 3.0f));
        juce::Path line, shade;
        const float x0 = in ? r.getX() : r.getRight() - w;
        for (int i = 0; i <= steps; ++i)
        {
            const float t = (float) i / (float) steps;
            const float gain = daw::clipmath::fadeGain (shape, in ? t : 1.0f - t);
            const juce::Point<float> pt (x0 + t * w, r.getBottom() - gain * r.getHeight());
            if (i == 0)
                line.startNewSubPath (pt);
            else
                line.lineTo (pt);
        }
        shade = line;
        if (in)
        {
            shade.lineTo (r.getX() + w, r.getY());
            shade.lineTo (r.getX(), r.getY());
        }
        else
        {
            shade.lineTo (r.getRight(), r.getY());
            shade.lineTo (r.getRight() - w, r.getY());
        }
        shade.closeSubPath();
        g.setColour (juce::Colours::black.withAlpha (0.42f));
        g.fillPath (shade);
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.strokePath (line, juce::PathStrokeType (1.2f));
    };
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (r.toNearestInt());
        curve (true);
        curve (false);
    }

    if (showHandles && clipRect.getWidth() > 24.0f)
    {
        for (const bool in : { true, false })
        {
            const float x = state.xOf (in ? c.start + c.fadeIn : c.end() - c.fadeOut);
            auto handle = juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ juce::jlimit (clipRect.getX() + 4.0f, clipRect.getRight() - 4.0f, x), clipRect.getY() + 4.5f });
            g.setColour (juce::Colours::white);
            g.fillRect (handle);
            g.setColour (juce::Colours::black);
            g.drawRect (handle, 1.0f);
        }
    }
}

void DawLanes::paintTake (juce::Graphics& g, int row, juce::Rectangle<int> lane) const
{
    const auto trackId = state.rows[(size_t) row].id;
    const auto start = state.session.getRecordStart();
    const auto length = state.session.getRecordedLength();
    const float x1 = state.xOf (start), x2 = state.xOf (start + length);
    if (x2 < 0.0f || x1 > (float) getWidth())
        return;
    auto r = juce::Rectangle<float> (x1, (float) lane.getY() + 1.0f, juce::jmax (2.0f, x2 - x1), (float) lane.getHeight() - 3.0f);
    g.setColour (DawColours::record.withAlpha (0.22f));
    g.fillRoundedRectangle (r, 3.0f);
    auto wave = r;
    if (r.getHeight() >= 40.0f)
    {
        auto bar = wave.removeFromTop ((float) kNameStrip);
        g.setColour (DawColours::record.withAlpha (0.85f));
        g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText ("REC", bar.reduced (5.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft);
    }

    int numChannels = 1;
    if (const auto* peaks = state.session.getRecordingPeaks (trackId, numChannels))
    {
        const int stride = 2 * numChannels;
        const int numPeaks = (int) (peaks->size() / (size_t) stride);
        const auto visible = wave.getIntersection (g.getClipBounds().toFloat());
        const float mid = wave.getCentreY(), half = wave.getHeight() * 0.5f - 1.0f;
        g.setColour (juce::Colour (0xffffb0a8));
        for (int x = (int) std::floor (visible.getX()); x < (int) std::ceil (visible.getRight()); ++x)
        {
            const double s0 = state.viewStart + (double) x * state.samplesPerPixel - (double) start;
            const double s1 = s0 + state.samplesPerPixel;
            int p0 = (int) (s0 / Recorder::kSamplesPerPeak), p1 = juce::jmax (p0 + 1, (int) std::ceil (s1 / Recorder::kSamplesPerPeak));
            p0 = juce::jmax (0, p0);
            p1 = juce::jmin (numPeaks, p1);
            if (p0 >= p1)
                continue;
            float lo = 0.0f, hi = 0.0f;
            for (int p = p0; p < p1; ++p)
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    lo = juce::jmin (lo, (*peaks)[(size_t) (p * stride + ch * 2)]);
                    hi = juce::jmax (hi, (*peaks)[(size_t) (p * stride + ch * 2 + 1)]);
                }
            g.fillRect ((float) x, mid - juce::jmin (1.0f, hi) * half, 1.0f, juce::jmax (1.0f, (juce::jmin (1.0f, hi) - juce::jmax (-1.0f, lo)) * half));
        }
    }
    g.setColour (DawColours::record);
    g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.5f);
}

//==============================================================================
void DawLanes::tick()
{
    const bool recording = state.session.isRecording();
    if (! recording)
    {
        if (! shownTakeWidth.empty())
        {
            shownTakeWidth.clear();
            repaint();
        }
        return;
    }
    const float x1 = state.xOf (state.session.getRecordStart());
    const float x2 = state.xOf (state.session.getRecordStart() + state.session.getRecordedLength());
    for (int i = 0; i < (int) state.rows.size(); ++i)
    {
        const auto id = state.rows[(size_t) i].id;
        if (! state.session.isRecordingTrack (id))
            continue;
        const int w = (int) (x2 - x1);
        auto& shown = shownTakeWidth[id];
        if (w == shown)
            continue;
        const auto lane = rowBounds (i);
        const int from = (int) std::floor (x1) + juce::jmin (shown, w) - 3;
        repaint (juce::Rectangle<int> (from, lane.getY(), std::abs (w - shown) + 8, lane.getHeight()).getIntersection (getLocalBounds()));
        shown = w;
    }
}

std::optional<int64> DawLanes::positionUnderMouse() const
{
    const auto p = getMouseXYRelative();
    if (! getLocalBounds().contains (p) || ! isMouseOver (true))
        return std::nullopt;
    return state.sampleAt ((float) p.x);
}

void DawLanes::mouseMove (const juce::MouseEvent& e)
{
    const auto h = hitAt (e.getPosition());
    if (h.row != hover.row || h.index != hover.index || h.zone != hover.zone)
    {
        // Repaint the clips whose handles appear or disappear.
        auto repaintHit = [this] (const Hit& x)
        {
            if (x.row >= 0 && x.index >= 0 && x.index < (int) state.clips[(size_t) x.row].size())
                repaint (clipBounds (x.row, state.clips[(size_t) x.row][(size_t) x.index]).expanded (6, 2));
        };
        repaintHit (hover);
        repaintHit (h);
        hover = h;
    }
    switch (h.zone)
    {
        case Zone::TrimStart:
        case Zone::TrimEnd:
        case Zone::FadeIn:
        case Zone::FadeOut: setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); break;
        case Zone::Gain:    setMouseCursor (juce::MouseCursor::UpDownResizeCursor); break;
        case Zone::Body:
        case Zone::None:    setMouseCursor (juce::MouseCursor::NormalCursor); break;
    }
}

void DawLanes::mouseExit (const juce::MouseEvent&)
{
    if (hover.index >= 0 && hover.row >= 0 && hover.index < (int) state.clips[(size_t) hover.row].size())
        repaint (clipBounds (hover.row, state.clips[(size_t) hover.row][(size_t) hover.index]).expanded (6, 2));
    hover = {};
}

void DawLanes::mouseDown (const juce::MouseEvent& e)
{
    down = hitAt (e.getPosition());
    downPos = e.getPosition();
    dragging = false;
    copying = false;
    moving.clear();
    lastDelta = 0;
    lastRowDelta = 0;
    rubber = {};
    const auto at = state.sampleAt ((float) e.x);

    if (down.row >= 0 && state.rows[(size_t) down.row].id != state.session.selectedTrack)
        state.selectTrack (state.rows[(size_t) down.row].id);

    if (e.mods.isPopupMenu())
    {
        if (down.index >= 0)
        {
            const auto id = state.clips[(size_t) down.row][(size_t) down.index].id;
            if (! state.isSelected (id))
                state.selectOnly (id);
            clipMenu (down.row, down.index, at);
        }
        else
        {
            laneMenu (down.row, state.snapToGrid (at));
        }
        down = {};
        return;
    }

    if (down.index < 0)
    {
        // Empty space: rubber band (adding to the selection with shift / ctrl).
        if (! e.mods.isShiftDown() && ! e.mods.isCommandDown())
            state.clearSelection();
        rubberBase = state.selection;
        return;
    }

    downClip = state.clips[(size_t) down.row][(size_t) down.index];
    downGainDb = downClip.gainDb;
    toggleOff = e.mods.isCommandDown() && state.isSelected (downClip.id);
    if (down.zone == Zone::Body)
    {
        if (e.mods.isShiftDown() || e.mods.isCommandDown())
        {
            if (! state.isSelected (downClip.id))
            {
                state.selection.insert (downClip.id);
                if (state.selectionChanged)
                    state.selectionChanged();
            }
        }
        else if (! state.isSelected (downClip.id))
        {
            state.selectOnly (downClip.id);
        }
    }
    else if (! state.isSelected (downClip.id))
    {
        state.selectOnly (downClip.id);
    }
}

void DawLanes::beginMove (const juce::MouseEvent& e)
{
    // The originals of every selected clip, then (ctrl) copies left in place.
    moving.clear();
    for (int r = 0; r < (int) state.clips.size(); ++r)
        for (auto& c : state.clips[(size_t) r])
            if (state.isSelected (c.id))
                moving.push_back ({ c, r });
    copying = e.mods.isCommandDown();
    std::set<int64> tracks;
    for (auto& m : moving)
        tracks.insert (state.rows[(size_t) m.row].id);
    crossfadesBefore = state.crossfadesOn (tracks);
    state.project.beginEdit (copying ? "Copy clips" : "Move clips");
    if (copying)
        for (auto& m : moving)
            state.project.addClip (state.rows[(size_t) m.row].id, m.original);
}

void DawLanes::dragMove (const juce::MouseEvent& e)
{
    if (moving.empty())
        return;
    const bool bypass = e.mods.isShiftDown();
    const auto rawDelta = (int64) std::llround ((double) (e.x - downPos.x) * state.samplesPerPixel);
    // Snap the grabbed clip's start (or, failing an edge, its end) and move the rest with it.
    auto target = snapPosition (downClip.start + rawDelta, bypass, true);
    if (! bypass)
    {
        bool endSnapped = false;
        const auto end = daw::snapToEdges (downClip.end() + rawDelta, snapEdges (true), (int64) (kSnapPixels * state.samplesPerPixel), &endSnapped);
        bool startSnapped = false;
        daw::snapToEdges (downClip.start + rawDelta, snapEdges (true), (int64) (kSnapPixels * state.samplesPerPixel), &startSnapped);
        if (endSnapped && ! startSnapped)
            target = end - downClip.length;
    }
    auto delta = target - downClip.start;
    int minRow = std::numeric_limits<int>::max(), maxRow = 0;
    for (auto& m : moving)
    {
        delta = juce::jmax (delta, -m.original.start);
        minRow = juce::jmin (minRow, m.row);
        maxRow = juce::jmax (maxRow, m.row);
    }
    int overRow = state.rowAt (e.y);
    if (overRow < 0)
        overRow = e.y < 0 ? 0 : (int) state.rows.size() - 1;
    const int rowDelta = juce::jlimit (-minRow, (int) state.rows.size() - 1 - maxRow, overRow - down.row);
    if (delta == lastDelta && rowDelta == lastRowDelta)
        return;
    lastDelta = delta;
    lastRowDelta = rowDelta;
    for (auto& m : moving)
    {
        state.project.setClip (daw::clipmath::moveTo (m.original, m.original.start + delta));
        const auto track = state.rows[(size_t) (m.row + rowDelta)].id;
        if (state.project.trackOfClip (m.original.id) != track)
            state.project.moveClipToTrack (m.original.id, track);
    }
}

void DawLanes::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
    {
        if (e.getDistanceFromDragStart() < 3)
            return;
        dragging = true;
        state.session.setGestureActive (true);
        switch (down.zone)
        {
            case Zone::Body:      beginMove (e); break;
            case Zone::TrimStart:
            case Zone::TrimEnd:
                crossfadesBefore = state.crossfadesOn ({ state.rows[(size_t) down.row].id });
                state.project.beginEdit ("Trim clip");
                break;
            case Zone::FadeIn:
            case Zone::FadeOut:   state.project.beginEdit ("Fade"); break;
            case Zone::Gain:      state.project.beginEdit ("Clip gain"); break;
            case Zone::None:      break;
        }
    }

    const bool bypass = e.mods.isShiftDown();
    const auto at = state.sampleAt ((float) e.x);
    switch (down.zone)
    {
        case Zone::Body:
            dragMove (e);
            break;
        case Zone::TrimStart:
            state.project.setClip (daw::clipmath::trimStart (downClip, snapPosition (at, bypass, true)));
            break;
        case Zone::TrimEnd:
            state.project.setClip (daw::clipmath::trimEnd (downClip, snapPosition (at, bypass, true)));
            break;
        case Zone::FadeIn:
        {
            auto c = downClip;
            c.fadeIn = juce::jlimit<int64> (0, c.length - c.fadeOut, snapPosition (at, bypass, true) - c.start);
            state.project.setClip (c);
            break;
        }
        case Zone::FadeOut:
        {
            auto c = downClip;
            c.fadeOut = juce::jlimit<int64> (0, c.length - c.fadeIn, c.end() - snapPosition (at, bypass, true));
            state.project.setClip (c);
            break;
        }
        case Zone::Gain:
        {
            const float db = juce::jlimit (-60.0f, 12.0f, downGainDb - (float) e.getDistanceFromDragStartY() * (bypass ? 0.05f : 0.25f));
            state.project.setClipProperty (downClip.id, daw::ids::gain, std::round (db * 10.0f) / 10.0f);
            break;
        }
        case Zone::None:
        {
            if (down.row < 0 && downPos.y > state.totalHeight - state.scrollY && e.getDistanceFromDragStart() < 3)
                break;
            const auto old = rubber;
            rubber = juce::Rectangle<int> (downPos, e.getPosition());
            auto sel = rubberBase;
            for (int r = 0; r < (int) state.rows.size(); ++r)
                for (auto& c : state.clips[(size_t) r])
                    if (clipBounds (r, c).intersects (rubber))
                        sel.insert (c.id);
            if (sel != state.selection)
            {
                state.selection = sel;
                if (state.selectionChanged)
                    state.selectionChanged();
            }
            repaint (old.getUnion (rubber).expanded (2));
            break;
        }
    }
}

void DawLanes::mouseUp (const juce::MouseEvent& e)
{
    if (dragging)
    {
        // Overlaps get crossfades; crossfades the edit pulled apart lose their fades.
        if (down.zone == Zone::Body)
        {
            std::set<int64> tracks;
            for (auto& m : moving)
            {
                tracks.insert (state.project.trackOfClip (m.original.id));
                if (juce::isPositiveAndBelow (m.row, (int) state.rows.size()))
                    tracks.insert (state.rows[(size_t) m.row].id); // where it came from (and a copy stays)
            }
            state.fitCrossfades (crossfadesBefore, tracks);
        }
        else if (down.zone == Zone::TrimStart || down.zone == Zone::TrimEnd)
        {
            state.fitCrossfades (crossfadesBefore, { state.project.trackOfClip (downClip.id) });
        }
        crossfadesBefore.clear();
        state.session.setGestureActive (false);
        if (! rubber.isEmpty())
            repaint (rubber.expanded (2));
    }
    else if (down.index >= 0 && down.zone == Zone::Body && ! e.mods.isPopupMenu())
    {
        if (e.mods.isCommandDown())
        {
            // Ctrl-click toggles (an unselected clip was added on the way down).
            if (toggleOff)
            {
                state.selection.erase (downClip.id);
                if (state.selectionChanged)
                    state.selectionChanged();
            }
        }
        else if (! e.mods.isShiftDown())
        {
            state.selectOnly (downClip.id);
        }
    }
    else if (down.index < 0 && down.row >= -1 && ! e.mods.isPopupMenu() && e.getDistanceFromDragStart() < 3)
    {
        if (onLocate)
            onLocate (snapPosition (state.sampleAt ((float) e.x), e.mods.isShiftDown(), false));
    }
    dragging = false;
    moving.clear();
    rubber = {};
}

void DawLanes::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto h = hitAt (e.getPosition());
    if (h.index >= 0)
    {
        const auto c = state.clips[(size_t) h.row][(size_t) h.index];
        askForText (getTopLevelComponent(), "RENAME CLIP", "Name:", c.name, [this, id = c.id] (const juce::String& name)
        {
            state.project.beginEdit ("Rename clip");
            state.project.setClipProperty (id, daw::ids::name, name);
        });
    }
    else if (h.row < 0 && e.y > state.totalHeight - state.scrollY)
    {
        state.addTrack();
    }
}

void DawLanes::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        state.zoomAround (state.samplesPerPixel * (w.deltaY > 0 ? 0.8 : 1.25), (float) e.x);
        return;
    }
    if (e.mods.isAltDown())
    {
        // Alt + wheel: the gain of the clip under the mouse.
        const auto h = hitAt (e.getPosition());
        if (h.index >= 0)
        {
            const auto c = state.clips[(size_t) h.row][(size_t) h.index];
            state.project.beginEdit ("Clip gain");
            const float step = (w.deltaY + w.deltaX) > 0 ? 0.5f : -0.5f;
            state.project.setClipProperty (c.id, daw::ids::gain, juce::jlimit (-60.0f, 12.0f, std::round ((c.gainDb + step) * 2.0f) / 2.0f));
        }
        return;
    }
    if (e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY))
    {
        const float d = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
        state.viewStart -= (double) d * 400.0 * state.samplesPerPixel;
    }
    else
    {
        state.scrollY -= juce::roundToInt (w.deltaY * 300.0f);
    }
    state.clampScroll();
    if (state.viewChanged)
        state.viewChanged();
}

//==============================================================================
void DawLanes::clipMenu (int row, int index, int64 at)
{
    const auto c = state.clips[(size_t) row][(size_t) index];
    juce::PopupMenu m;
    m.addSectionHeader (c.name.isNotEmpty() ? c.name : juce::String ("CLIP"));
    m.addItem ("Rename...", [this, c]
    {
        askForText (getTopLevelComponent(), "RENAME CLIP", "Name:", c.name, [this, id = c.id] (const juce::String& name)
        {
            state.project.beginEdit ("Rename clip");
            state.project.setClipProperty (id, daw::ids::name, name);
        });
    });
    m.addItem ("Mute", true, c.mute, [this] { state.toggleMuteSelection(); });
    auto shapes = [this, c] (bool in)
    {
        juce::PopupMenu sub;
        const std::pair<const char*, daw::FadeShape> list[] = { { "Linear", daw::FadeShape::Linear },
                                                                { "Equal power", daw::FadeShape::EqualPower },
                                                                { "S-curve", daw::FadeShape::SCurve } };
        for (auto& [name, shape] : list)
            sub.addItem (name, true, (in ? c.fadeInShape : c.fadeOutShape) == shape, [this, in, s = shape]
            {
                state.project.beginEdit ("Fade shape");
                for (auto& sc : state.selectedClips())
                {
                    auto copy = sc;
                    (in ? copy.fadeInShape : copy.fadeOutShape) = s;
                    state.project.setClip (copy);
                }
            });
        return sub;
    };
    m.addSubMenu ("Fade-in shape", shapes (true));
    m.addSubMenu ("Fade-out shape", shapes (false));
    m.addItem ("Remove fades", c.fadeIn > 0 || c.fadeOut > 0, false, [this]
    {
        state.project.beginEdit ("Remove fades");
        for (auto& sc : state.selectedClips())
        {
            auto copy = sc;
            copy.fadeIn = copy.fadeOut = 0;
            state.project.setClip (copy);
        }
    });
    m.addItem ("Reset gain (0 dB)", std::abs (c.gainDb) > 0.01f, false, [this]
    {
        state.project.beginEdit ("Reset clip gain");
        for (auto& sc : state.selectedClips())
            state.project.setClipProperty (sc.id, daw::ids::gain, 0.0f);
    });
    m.addSeparator();
    m.addItem ("Split here   (Shift+S)", daw::clipmath::canSplit (c, at), false, [this, at] { state.splitAt (at); });
    m.addItem ("Duplicate   (Ctrl+D)", [this] { state.duplicateSelection(); });
    m.addItem ("Copy   (Ctrl+C)", [this] { state.copySelection(); });
    m.addItem ("Cut   (Ctrl+X)", [this] { state.cutSelection(); });
    m.addItem ("Delete   (Del)", [this] { state.deleteSelection(); });
    m.addSeparator();
    m.addItem ("Crossfade the overlaps on this track", [this, track = state.rows[(size_t) row].id]
    {
        state.project.beginEdit ("Crossfade");
        state.project.crossfadeOverlaps (track);
    });
    m.addItem ("Show the file", [file = state.project.resolve (c.file)] { file.revealToUser(); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

void DawLanes::laneMenu (int row, int64 at)
{
    juce::PopupMenu m;
    m.addItem ("Paste here   (Ctrl+V pastes at the playhead)", ! state.session.clipboard.empty(), false, [this, at] { state.paste (at); });
    m.addItem ("Add marker here", [this, at]
    {
        state.project.beginEdit ("Add marker");
        state.project.addMarker (at);
    });
    m.addItem ("Locate here", [this, at] { state.session.locate (at); });
    m.addSeparator();
    m.addItem ("Add track", [this] { state.addTrack(); });
    if (row >= 0)
        m.addItem ("Delete this track", [this, id = state.rows[(size_t) row].id] { state.deleteTrack (id); });
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

//==============================================================================
bool DawLanes::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (daw::canImport (state.session.getFormats(), juce::File (f)))
            return true;
    return false;
}

void DawLanes::fileDragEnter (const juce::StringArray& files, int x, int y) { fileDragMove (files, x, y); }

void DawLanes::fileDragMove (const juce::StringArray&, int x, int y)
{
    const int row = state.rowAt (y);
    const float px = state.xOf (snapPosition (state.sampleAt ((float) x), false, false));
    if (row != dropRow || std::abs (px - dropX) > 0.5f)
    {
        dropRow = row;
        dropX = px;
        repaint();
    }
}

void DawLanes::fileDragExit (const juce::StringArray&)
{
    dropRow = -2;
    repaint();
}

void DawLanes::filesDropped (const juce::StringArray& files, int x, int y)
{
    dropRow = -2;
    repaint();
    juce::Array<juce::File> list;
    for (auto& f : files)
        if (daw::canImport (state.session.getFormats(), juce::File (f)))
            list.add (juce::File (f));
    const int row = state.rowAt (y);
    const auto at = snapPosition (state.sampleAt ((float) x), false, false);
    importAudio (getTopLevelComponent(), state.session, list, row >= 0 ? state.rows[(size_t) row].id : 0, at);
}

} // namespace livedaw::ui
