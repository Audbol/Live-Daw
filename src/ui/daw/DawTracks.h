#pragma once

#include "ui/daw/DawCommon.h"

namespace livedaw::ui
{

class DawTrackList;

/** A track's header: colour, number and name, R (arm) / M / S, the record
    source, the playback channel, the gain and the record level meter. Drag the
    name to reorder, the bottom edge to change the height; right-click for
    the track menu. */
class DawTrackHeader final : public juce::Component
{
public:
    DawTrackHeader (DawEditorState&, DawTrackList&, daw::int64 trackId);

    daw::int64 getTrackId() const noexcept { return trackId; }
    /** Repaints the meter if the level moved. */
    void updateMeter();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    enum class Part
    {
        None,
        Colour,
        Name,
        Arm,
        Mute,
        Solo,
        Source,
        Playback,
        Gain,
        Resize
    };

    const daw::TrackData* track() const;
    Part partAt (juce::Point<int>) const;
    void showMenu();
    void playbackMenu();
    void rename();

    DawEditorState& state;
    DawTrackList& list;
    const daw::int64 trackId;

    juce::Rectangle<int> colourArea, nameArea, armKey, muteKey, soloKey, sourceKey, playbackKey, gainKey, meterArea;
    Part downPart = Part::None;
    bool dragged = false;
    float downGain = 0.0f;
    int downHeight = 0;
    float shownLevel[2] { -100.0f, -100.0f };
};

/** The column of track headers left of the lanes, scrolled with them, with
    the + TRACK key under the last track. */
class DawTrackList final : public juce::Component, private juce::Timer
{
public:
    explicit DawTrackList (DawEditorState&);

    /** Tracks were added, removed or reordered, or the view scrolled. */
    void update();

    /** Reordering by dragging a header (y in this component). */
    void dragTrack (daw::int64 trackId, int y);
    void dropTrack (daw::int64 trackId, int y);

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    int insertionIndex (int y) const;

    DawEditorState& state;
    std::vector<std::unique_ptr<DawTrackHeader>> headers;
    DawKey addKey { "+ TRACK" };
    int dropLine = -1;
};

} // namespace livedaw::ui
