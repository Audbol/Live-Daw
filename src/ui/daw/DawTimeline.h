#pragma once

#include "ui/daw/DawCommon.h"

namespace livedaw::ui
{

/** The time ruler above the lanes: bars|beats or min:sec, the loop range and
    the marker lane. Click to locate, drag to set the loop range (drag its
    ends to adjust it), click a marker to jump to it, drag it to move it,
    double-click to rename it (or, on empty marker lane, to add one). */
class DawRuler final : public juce::Component
{
public:
    explicit DawRuler (DawEditorState&);

    static constexpr int kTimeHeight = 26, kMarkerHeight = 20;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    enum class Drag
    {
        None,
        Locate,
        LoopNew,
        LoopStart,
        LoopEnd,
        Marker
    };

    daw::int64 markerAt (juce::Point<int>) const;
    void markerMenu (daw::int64 markerId);

    DawEditorState& state;
    Drag drag = Drag::None;
    daw::int64 dragMarker = 0, dragAnchor = 0;
    bool dragged = false;
};

/** The track lanes with their clips: waveforms (finished files from their
    thumbnails, takes in progress from the recorder's peaks), fades, clip gain,
    names. Select (click, shift/ctrl-click, rubber band), move (also to other
    tracks; ctrl-drag copies), trim the edges, drag the fade handles at the top
    corners and the gain line. Files dropped from the OS are imported. */
class DawLanes final : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    DawLanes (DawEditorState&, DawWaveforms&);

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int x, int y) override;
    void fileDragMove (const juce::StringArray&, int x, int y) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

    /** Called every frame: repaints what takes in progress added. */
    void tick();
    /** The timeline position under the mouse, if the mouse is over the lanes. */
    std::optional<daw::int64> positionUnderMouse() const;

    /** Clicks on empty lane space locate the playhead here. */
    std::function<void (daw::int64)> onLocate;

private:
    enum class Zone
    {
        None,
        Body,
        TrimStart,
        TrimEnd,
        FadeIn,
        FadeOut,
        Gain
    };

    struct Hit
    {
        int row = -1;
        int index = -1; // clip index in the row
        Zone zone = Zone::None;
    };

    struct Moving
    {
        daw::ClipData original;
        int row = 0;
    };

    Hit hitAt (juce::Point<int>) const;
    juce::Rectangle<int> rowBounds (int row) const;
    juce::Rectangle<int> clipBounds (int row, const daw::ClipData&) const;
    static float gainY (juce::Rectangle<int> clip, float gainDb);
    std::vector<daw::int64> snapEdges (bool excludeSelection) const;
    daw::int64 snapPosition (daw::int64 position, bool bypassGrid, bool excludeSelection) const;

    void paintGrid (juce::Graphics&, juce::Rectangle<int> area) const;
    void paintClip (juce::Graphics&, int row, const daw::ClipData&, juce::Rectangle<int> clipArea) const;
    void paintFades (juce::Graphics&, const daw::ClipData&, juce::Rectangle<float> clip, juce::Rectangle<float> curveArea, bool showHandles) const;
    void paintTake (juce::Graphics&, int row, juce::Rectangle<int> clipArea) const;
    void clipMenu (int row, int index, daw::int64 at);
    void laneMenu (int row, daw::int64 at);

    void beginMove (const juce::MouseEvent&);
    void dragMove (const juce::MouseEvent&);

    DawEditorState& state;
    DawWaveforms& waves;

    Hit hover, down;
    juce::Point<int> downPos;
    bool dragging = false, copying = false, toggleOff = false;
    std::vector<Moving> moving;
    /** The crossfades on the tracks a move or trim started on. */
    std::vector<daw::Project::Crossfade> crossfadesBefore;
    daw::ClipData downClip;
    float downGainDb = 0.0f;
    daw::int64 lastDelta = 0;
    int lastRowDelta = 0;
    juce::Rectangle<int> rubber;
    std::set<daw::int64> rubberBase;
    int dropRow = -2;
    float dropX = -1.0f;
    std::map<daw::int64, int> shownTakeWidth; // track id -> px drawn of its take
};

/** The playhead line over the ruler and the lanes. */
class DawPlayhead final : public juce::Component
{
public:
    DawPlayhead() { setInterceptsMouseClicks (false, false); }

    bool recording = false;
    int rulerHeight = DawRuler::kTimeHeight + DawRuler::kMarkerHeight;

    void paint (juce::Graphics& g) override
    {
        const auto c = recording ? DawColours::record : DawColours::playhead;
        const float x = (float) getWidth() * 0.5f;
        g.setColour (c);
        g.fillRect (x - 0.5f, (float) DawRuler::kTimeHeight - 8.0f, 1.5f, (float) getHeight());
        juce::Path tri;
        tri.addTriangle (x - 6.0f, (float) DawRuler::kTimeHeight - 12.0f, x + 6.0f, (float) DawRuler::kTimeHeight - 12.0f, x, (float) DawRuler::kTimeHeight - 2.0f);
        g.fillPath (tri);
    }
};

} // namespace livedaw::ui
