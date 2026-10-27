#pragma once

#include "daw/Session.h"
#include "daw/TimeGrid.h"
#include "ui/ScreenWidgets.h"
#include "ui/UiContext.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <set>

namespace livedaw::ui
{

/** The editor's palette: the WING's dark screens. */
namespace DawColours
{
    const juce::Colour background { 0xff0b0b0b };
    const juce::Colour bar { 0xff161616 };          // top bar, ruler
    const juce::Colour header { 0xff1c1c1c };       // track headers
    const juce::Colour headerSelected { 0xff2a2a2a };
    const juce::Colour lane { 0xff121212 };
    const juce::Colour laneAlt { 0xff151515 };
    const juce::Colour laneSelected { 0xff1b1b1b };
    const juce::Colour line { 0xff2c2c2c };
    const juce::Colour gridMajor { 0xff2e2e2e };
    const juce::Colour gridMinor { 0xff1f1f1f };
    const juce::Colour key { 0xff2e2e2e };
    const juce::Colour keyOutline { 0xff4a4a4a };
    const juce::Colour text { 0xffeeeeee };
    const juce::Colour textDim { 0xff8a8a8a };
    const juce::Colour playhead { 0xfff2f2f2 };
    const juce::Colour record { 0xffe53935 };
    const juce::Colour arm { 0xffe53935 };
    const juce::Colour mute { 0xfff08a24 };
    const juce::Colour solo { 0xfff5d63a };
    const juce::Colour loop { 0xff3e71cb };
    const juce::Colour marker { 0xffe8c35a };
    const juce::Colour selection { 0xfff3c650 };
    const juce::Colour gainLine { 0xfff3c650 };
} // namespace DawColours

/** Waveforms of the project's audio files: an AudioThumbnail per file in a
    shared cache. Finished thumbnails are kept in the project's Peaks folder,
    so reopening a big project draws at once. */
class DawWaveforms final : private juce::ChangeListener
{
public:
    explicit DawWaveforms (juce::AudioFormatManager&);
    ~DawWaveforms() override;

    /** The thumbnail of a file (loading in the background), or nullptr. */
    juce::AudioThumbnail* get (const juce::File&);
    /** Forgets every thumbnail (another project was opened). */
    void clear();
    /** Where finished thumbnails are kept (empty: nowhere). */
    void setPeaksFolder (const juce::File&);

    /** A thumbnail finished loading a part. */
    std::function<void()> onChange;

    static constexpr int kSamplesPerThumbSample = 256;

private:
    class Cache;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    juce::AudioFormatManager& formats;
    std::unique_ptr<Cache> cache;
    std::map<juce::String, std::unique_ptr<juce::AudioThumbnail>> thumbs;
};

/** What the editor's parts share: the session, the view (zoom, scroll, snap)
    and the selection. Positions on screen: x = (sample - viewStart) / zoom. */
struct DawEditorState
{
    DawEditorState (UiContext& c, daw::Session& s) : ctx (c), session (s), project (s.getProject()) {}

    UiContext& ctx;
    daw::Session& session;
    daw::Project& project;

    double samplesPerPixel = 480.0;
    double viewStart = 0.0;
    int scrollY = 0;
    daw::SnapMode snap = daw::SnapMode::Grid;
    bool showBars = false;       // position and ruler in bars|beats
    bool follow = true;          // page-scroll with the playhead
    std::set<daw::int64> selection;

    static constexpr double kMinSamplesPerPixel = 4.0, kMaxSamplesPerPixel = 200000.0;
    static constexpr int kMinTrackHeight = 34, kMaxTrackHeight = 300;

    daw::TimeGrid grid() const
    {
        daw::TimeGrid g;
        g.sampleRate = project.getSampleRate();
        g.tempo = project.getTempo();
        g.beatsPerBar = project.getSigNumerator();
        g.beatUnit = project.getSigDenominator();
        return g;
    }

    float xOf (daw::int64 sample) const { return (float) (((double) sample - viewStart) / samplesPerPixel); }
    daw::int64 sampleAt (float x) const { return juce::jmax<daw::int64> (0, (daw::int64) std::llround (viewStart + (double) x * samplesPerPixel)); }

    /** Snaps `position` to the grid (unless snapping is off or `bypass`, e.g. shift held). */
    daw::int64 snapToGrid (daw::int64 position, bool bypass = false) const
    {
        return bypass ? position : grid().snap (position, snap, samplesPerPixel);
    }

    /** Track layout and a copy of the tracks and their clips, refreshed from
        the project with refresh() (the views paint from these). */
    struct TrackRow
    {
        daw::int64 id = 0;
        int top = 0, height = 0;
    };
    std::vector<TrackRow> rows;
    std::vector<daw::TrackData> tracks;
    std::vector<std::vector<daw::ClipData>> clips;
    int totalHeight = 0;
    /** The lanes' size (set by the editor). */
    int viewWidth = 800, viewHeight = 400;

    void refresh();

    //==============================================================================
    // Edits (each one undoable action)

    std::vector<daw::ClipData> selectedClips() const;
    void selectOnly (daw::int64 clipId);
    void clearSelection();
    void selectAll();
    /** Drops ids of clips that no longer exist. */
    void pruneSelection();
    void deleteSelection();
    void duplicateSelection();
    void copySelection();
    void cutSelection();
    /** Pastes the clipboard at `position`, its first track on the selected track. */
    void paste (daw::int64 position);
    /** Splits the selected clips at `position` (or, with none selected, the
        clips there on the selected track). */
    void splitAt (daw::int64 position);
    /** Moves the selected clips by one grid step (or 10 ms without snapping). */
    void nudge (int direction, bool fine);
    void toggleMuteSelection();
    /** The crossfades on `tracks` before an edit of their clips. */
    std::vector<daw::Project::Crossfade> crossfadesOn (const std::set<daw::int64>& clipTracks) const;
    /** After the edit, with auto crossfades on: crossfades whose clips came
        apart lose their fades, and the overlaps on `clipTracks` get crossfades. */
    void fitCrossfades (const std::vector<daw::Project::Crossfade>& before, const std::set<daw::int64>& clipTracks);
    void addTrack();
    void deleteTrack (daw::int64 trackId);
    void selectTrack (daw::int64 trackId);

    // View
    void zoomAround (double newSamplesPerPixel, float anchorX);
    void zoomToFit();
    void scrollToShow (daw::int64 position);
    void clampScroll();

    /** The track row at a y position of the lanes (in view coordinates), or -1. */
    int rowAt (int viewY) const
    {
        const int y = viewY + scrollY;
        for (int i = 0; i < (int) rows.size(); ++i)
            if (y >= rows[(size_t) i].top && y < rows[(size_t) i].top + rows[(size_t) i].height)
                return i;
        return -1;
    }

    int rowOf (daw::int64 trackId) const
    {
        for (int i = 0; i < (int) rows.size(); ++i)
            if (rows[(size_t) i].id == trackId)
                return i;
        return -1;
    }

    bool isSelected (daw::int64 clipId) const { return selection.count (clipId) > 0; }

    /** Repaint everything that shows the view (zoom/scroll changed). */
    std::function<void()> viewChanged;
    /** Repaint what shows the selection. */
    std::function<void()> selectionChanged;
    /** Show a short message in the editor's status area. */
    std::function<void (const juce::String&)> status;
};

/** A transport / tool key of the editor: dark, an icon or text, lit in a colour. */
class DawKey final : public juce::Button, private juce::Timer
{
public:
    DawKey (const juce::String& keyText, std::optional<Icon> keyIcon = std::nullopt, juce::Colour lit = DawColours::selection)
        : juce::Button (keyText), icon (keyIcon), litColour (lit)
    {
        startTimerHz (10);
    }

    std::function<bool()> isLit;
    /** Draws the label even with an icon (icon left of it). */
    bool iconAndText = false;
    /** Ink of the icon when not lit (the record key's red dot). */
    juce::Colour iconColour = DawColours::text;

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const bool lit = isLit != nullptr && isLit();
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        auto fill = lit ? litColour : DawColours::key;
        if (down)
            fill = fill.brighter (0.2f);
        else if (over)
            fill = fill.brighter (0.08f);
        if (! isEnabled())
            fill = fill.darker (0.5f);
        g.setColour (fill);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (DawColours::keyOutline.withAlpha (lit ? 0.0f : 1.0f));
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
        auto ink = lit ? juce::Colours::black : DawColours::text;
        if (! isEnabled())
            ink = ink.withAlpha (0.35f);
        auto content = r.reduced (4.0f, 2.0f);
        if (icon.has_value())
        {
            const float s = juce::jmin (content.getHeight() * 0.7f, content.getWidth());
            auto box = iconAndText ? content.removeFromLeft (s).withSizeKeepingCentre (s, s) : content.withSizeKeepingCentre (s, s);
            drawIcon (g, *icon, box, lit ? ink : (isEnabled() ? iconColour : iconColour.withAlpha (0.35f)));
            if (! iconAndText)
                return;
            content.removeFromLeft (4.0f);
        }
        g.setColour (ink);
        g.setFont (juce::FontOptions (juce::jlimit (11.0f, 16.0f, content.getHeight() * 0.5f), juce::Font::bold));
        g.drawFittedText (getButtonText(), content.toNearestInt(), juce::Justification::centred, 2, 0.7f);
    }

private:
    void timerCallback() override
    {
        const bool lit = isLit != nullptr && isLit();
        if (lit != shownLit)
        {
            shownLit = lit;
            repaint();
        }
    }

    std::optional<Icon> icon;
    juce::Colour litColour;
    bool shownLit = false;
};

/** "48K", "44.1K" */
inline juce::String rateText (double rate)
{
    const double k = rate / 1000.0;
    return (std::abs (k - std::round (k)) < 0.01 ? juce::String (juce::roundToInt (k)) : juce::String (k, 1)) + "K";
}

/** "05h 33m 24s" */
inline juce::String durationText (double seconds)
{
    if (! std::isfinite (seconds) || seconds < 0.0)
        return "-";
    const auto s = (juce::int64) seconds;
    return juce::String (s / 3600).paddedLeft ('0', 2) + "h " + juce::String ((s / 60) % 60).paddedLeft ('0', 2) + "m "
           + juce::String (s % 60).paddedLeft ('0', 2) + "s";
}

/** The menu of record sources: hardware inputs, channel / aux pre or post
    fader, buses, mains and matrices post fader. */
void showSourceMenu (UiContext&, const Recorder::Source& current, juce::Component* target, std::function<void (Recorder::Source)> chosen);

/** The WING colour menu (1..18). */
void showColourMenu (int current, juce::Component* target, std::function<void (int)> chosen);

/** A text prompt (rename) over `parent`: `done` gets the new text unless cancelled. */
void askForText (juce::Component* parent, const juce::String& title, const juce::String& label, const juce::String& initial,
                 std::function<void (const juce::String&)> done);

/** Imports files as clips (FILE > Import audio, files dropped on the editor):
    with IMPORT: ASK EACH TIME it first asks over `parent` whether to keep the
    files in place or copy them into the project ("Remember my choice" sets the
    IMPORT setting). */
void importAudio (juce::Component* parent, daw::Session&, const juce::Array<juce::File>&, daw::int64 trackId, daw::int64 position);

/** "COPY TO PROJECT", "KEEP IN PLACE", "ASK EACH TIME". */
juce::String importModeName (daw::Session::ImportMode);

} // namespace livedaw::ui
