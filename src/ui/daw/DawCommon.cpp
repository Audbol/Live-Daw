#include "ui/daw/DawCommon.h"

namespace livedaw::ui
{

/** Keeps finished thumbnails as files in the project's Peaks folder. */
class DawWaveforms::Cache final : public juce::AudioThumbnailCache
{
public:
    Cache() : juce::AudioThumbnailCache (512) {}

    juce::File folder;

    void saveNewlyFinishedThumbnail (const juce::AudioThumbnailBase& thumb, juce::int64 hash) override
    {
        if (folder == juce::File() || ! folder.createDirectory())
            return;
        const auto f = fileFor (hash);
        juce::FileOutputStream out (f);
        if (out.openedOk())
        {
            out.setPosition (0);
            out.truncate();
            thumb.saveTo (out);
        }
    }

    bool loadNewThumb (juce::AudioThumbnailBase& thumb, juce::int64 hash) override
    {
        if (folder == juce::File())
            return false;
        juce::FileInputStream in (fileFor (hash));
        return in.openedOk() && thumb.loadFrom (in);
    }

private:
    juce::File fileFor (juce::int64 hash) const { return folder.getChildFile (juce::String::toHexString (hash) + ".peaks"); }
};

DawWaveforms::DawWaveforms (juce::AudioFormatManager& f) : formats (f), cache (std::make_unique<Cache>()) {}

DawWaveforms::~DawWaveforms()
{
    thumbs.clear();
    cache.reset();
}

juce::AudioThumbnail* DawWaveforms::get (const juce::File& file)
{
    const auto key = file.getFullPathName();
    if (auto it = thumbs.find (key); it != thumbs.end())
        return it->second.get();
    if (! file.existsAsFile())
        return nullptr;
    auto thumb = std::make_unique<juce::AudioThumbnail> (kSamplesPerThumbSample, formats, *cache);
    thumb->addChangeListener (this);
    thumb->setSource (new juce::FileInputSource (file));
    auto* raw = thumb.get();
    thumbs[key] = std::move (thumb);
    return raw;
}

void DawWaveforms::clear()
{
    thumbs.clear();
    cache->clear();
}

void DawWaveforms::setPeaksFolder (const juce::File& f)
{
    cache->folder = f;
}

void DawWaveforms::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (onChange)
        onChange();
}

//==============================================================================
void showSourceMenu (UiContext& ctx, const Recorder::Source& current, juce::Component* target, std::function<void (Recorder::Source)> chosen)
{
    using Kind = Recorder::Source::Kind;
    auto is = [&current] (Kind k, int port, StripKind sk, int number)
    {
        if (current.kind != k)
            return false;
        return k == Kind::HardwareInput ? current.port == port : (current.stripKind == sk && current.stripNumber == number);
    };

    juce::PopupMenu menu;
    menu.addSectionHeader ("RECORD SOURCE");

    // Hardware inputs, in groups of 8.
    const auto& names = ctx.engine.getHardwareInputNames();
    const int numInputs = names.isEmpty() ? 64 : names.size();
    juce::PopupMenu inputs;
    for (int group = 0; group * 8 < numInputs; ++group)
    {
        juce::PopupMenu sub;
        for (int i = group * 8; i < juce::jmin (numInputs, group * 8 + 8); ++i)
        {
            const auto name = "IN " + juce::String (i + 1) + (i < names.size() && names[i].isNotEmpty() ? "  (" + names[i] + ")" : juce::String());
            sub.addItem (name, true, is (Kind::HardwareInput, i, StripKind::Channel, 0), [chosen, i]
            {
                Recorder::Source s;
                s.kind = Kind::HardwareInput;
                s.port = i;
                chosen (s);
            });
        }
        inputs.addSubMenu ("IN " + juce::String (group * 8 + 1) + "-" + juce::String (juce::jmin (numInputs, group * 8 + 8)), sub);
    }
    menu.addSubMenu ("HARDWARE INPUT", inputs, true, nullptr, current.kind == Kind::HardwareInput);

    auto strips = [&] (StripKind kind, Kind tap)
    {
        juce::PopupMenu sub;
        const auto& list = ctx.engine.getStrips (kind);
        for (size_t i = 0; i < list.size(); ++i)
        {
            const int number = list[i]->getNumber();
            sub.addItem (describeStrip (*list[i]), true, is (tap, 0, kind, number), [chosen, kind, tap, number]
            {
                Recorder::Source s;
                s.kind = tap;
                s.stripKind = kind;
                s.stripNumber = number;
                chosen (s);
            });
        }
        return sub;
    };
    auto channelGroups = [&] (Kind tap)
    {
        // 40+ channels: groups of 8.
        juce::PopupMenu sub;
        const auto& list = ctx.engine.getChannels();
        for (size_t g = 0; g < list.size(); g += 8)
        {
            juce::PopupMenu part;
            for (size_t i = g; i < juce::jmin (list.size(), g + 8); ++i)
            {
                const int number = list[i]->getNumber();
                part.addItem (describeStrip (*list[i]), true, is (tap, 0, StripKind::Channel, number), [chosen, tap, number]
                {
                    Recorder::Source s;
                    s.kind = tap;
                    s.stripKind = StripKind::Channel;
                    s.stripNumber = number;
                    chosen (s);
                });
            }
            sub.addSubMenu ("CH " + juce::String ((int) g + 1) + "-" + juce::String ((int) juce::jmin (list.size(), g + 8)), part);
        }
        return sub;
    };
    menu.addSubMenu ("CHANNEL PRE FADER", channelGroups (Kind::StripPreFader));
    menu.addSubMenu ("CHANNEL POST FADER", channelGroups (Kind::StripPostFader));
    menu.addSubMenu ("AUX PRE FADER", strips (StripKind::Aux, Kind::StripPreFader));
    menu.addSubMenu ("AUX POST FADER", strips (StripKind::Aux, Kind::StripPostFader));
    menu.addSubMenu ("BUS POST", strips (StripKind::Bus, Kind::StripPostFader));
    menu.addSubMenu ("MAIN POST", strips (StripKind::Main, Kind::StripPostFader));
    menu.addSubMenu ("MATRIX POST", strips (StripKind::Matrix, Kind::StripPostFader));
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target));
}

void showColourMenu (int current, juce::Component* target, std::function<void (int)> chosen)
{
    juce::PopupMenu menu;
    menu.addSectionHeader ("COLOUR");
    for (int i = 1; i <= 18; ++i)
    {
        juce::PopupMenu::Item item (wingColourName (i));
        item.setColour (wingColour (i));
        item.setTicked (i == current);
        item.setAction ([chosen, i] { chosen (i); });
        menu.addItem (std::move (item));
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target));
}

void askForText (juce::Component* parent, const juce::String& title, const juce::String& label, const juce::String& initial,
                 std::function<void (const juce::String&)> done)
{
    auto* w = new juce::AlertWindow (title, label, juce::MessageBoxIconType::NoIcon, parent);
    w->addTextEditor ("text", initial);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    if (auto* editor = w->getTextEditor ("text"))
        editor->selectAll();
    w->enterModalState (true, juce::ModalCallbackFunction::create ([w, done] (int r)
    {
        if (r != 0)
            done (w->getTextEditorContents ("text").trim());
    }), true);
}

juce::String importModeName (daw::Session::ImportMode m)
{
    switch (m)
    {
        case daw::Session::ImportMode::KeepInPlace: return "KEEP IN PLACE";
        case daw::Session::ImportMode::Ask: return "ASK EACH TIME";
        case daw::Session::ImportMode::Copy: break;
    }
    return "COPY TO PROJECT";
}

void importAudio (juce::Component* parent, daw::Session& session, const juce::Array<juce::File>& files, daw::int64 trackId, daw::int64 position)
{
    using Mode = daw::Session::ImportMode;
    if (files.isEmpty())
        return;
    if (session.getImportMode() != Mode::Ask)
    {
        session.importFiles (files, trackId, position);
        return;
    }
    const auto& p = session.getProject();
    const auto audio = p.hasFolder() ? p.getAudioFolder().getFullPathName() : session.getProjectsFolder().getFullPathName();
    const auto what = files.size() == 1 ? "\"" + files[0].getFileName() + "\"" : juce::String (files.size()) + " files";
    auto* w = new juce::AlertWindow ("IMPORT AUDIO",
                                     "Import " + what + ":\n\n"
                                         "KEEP IN PLACE: the clips play the files where they are, nothing is copied (a file at another "
                                         "sample rate, with more than two channels or in a compressed format still gets a converted copy).\n\n"
                                         "COPY TO PROJECT: every file is converted into the project's Audio folder (" + audio + ").",
                                     juce::MessageBoxIconType::QuestionIcon, parent);
    auto remember = std::make_shared<juce::ToggleButton> ("Remember my choice (FILE > Import)");
    remember->setSize (360, 26);
    w->addCustomComponent (remember.get());
    w->addButton ("KEEP IN PLACE", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("COPY TO PROJECT", 2);
    w->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([&session, files, trackId, position, remember] (int r)
    {
        if (r == 0)
            return;
        const bool keep = r == 1;
        if (remember->getToggleState())
            session.setImportMode (keep ? Mode::KeepInPlace : Mode::Copy);
        session.importFiles (files, trackId, position, keep);
    }), true);
}

//==============================================================================
void DawEditorState::refresh()
{
    tracks = project.getTracks();
    rows.clear();
    clips.clear();
    int y = 0;
    for (auto& t : tracks)
    {
        const int h = t.height > 0 ? juce::jlimit (kMinTrackHeight, kMaxTrackHeight, t.height) : daw::Project::kDefaultTrackHeight;
        rows.push_back ({ t.id, y, h });
        clips.push_back (project.getClips (t.id));
        y += h;
    }
    totalHeight = y;
    pruneSelection();
    clampScroll();
}

std::vector<daw::ClipData> DawEditorState::selectedClips() const
{
    std::vector<daw::ClipData> list;
    for (auto& row : clips)
        for (auto& c : row)
            if (isSelected (c.id))
                list.push_back (c);
    return list;
}

void DawEditorState::selectOnly (daw::int64 clipId)
{
    selection.clear();
    if (clipId != 0)
        selection.insert (clipId);
    if (selectionChanged)
        selectionChanged();
}

void DawEditorState::clearSelection()
{
    if (selection.empty())
        return;
    selection.clear();
    if (selectionChanged)
        selectionChanged();
}

void DawEditorState::selectAll()
{
    selection.clear();
    for (auto& row : clips)
        for (auto& c : row)
            selection.insert (c.id);
    if (selectionChanged)
        selectionChanged();
}

void DawEditorState::pruneSelection()
{
    std::set<daw::int64> existing;
    for (auto& row : clips)
        for (auto& c : row)
            if (isSelected (c.id))
                existing.insert (c.id);
    selection.swap (existing);
}

std::vector<daw::Project::Crossfade> DawEditorState::crossfadesOn (const std::set<daw::int64>& clipTracks) const
{
    std::vector<daw::Project::Crossfade> list;
    for (auto t : clipTracks)
        for (auto& x : project.getCrossfades (t))
            list.push_back (x);
    return list;
}

void DawEditorState::fitCrossfades (const std::vector<daw::Project::Crossfade>& before, const std::set<daw::int64>& clipTracks)
{
    if (! session.autoCrossfade)
        return;
    project.releaseCrossfades (before);
    for (auto t : clipTracks)
        project.crossfadeOverlaps (t);
}

void DawEditorState::deleteSelection()
{
    if (selection.empty())
        return;
    std::set<daw::int64> clipTracks;
    for (auto id : selection)
        clipTracks.insert (project.trackOfClip (id));
    const auto crossfades = crossfadesOn (clipTracks);
    project.beginEdit ("Delete clips");
    for (auto id : selection)
        project.removeClip (id);
    fitCrossfades (crossfades, {});
    clearSelection();
}

void DawEditorState::duplicateSelection()
{
    const auto list = selectedClips();
    if (list.empty())
        return;
    daw::int64 first = std::numeric_limits<daw::int64>::max(), last = 0;
    for (auto& c : list)
    {
        first = juce::jmin (first, c.start);
        last = juce::jmax (last, c.end());
    }
    project.beginEdit ("Duplicate clips");
    std::set<daw::int64> made;
    std::set<daw::int64> touched;
    for (auto& c : list)
    {
        const auto track = project.trackOfClip (c.id);
        made.insert (project.addClip (track, daw::clipmath::moveTo (c, c.start + (last - first))));
        touched.insert (track);
    }
    if (session.autoCrossfade)
        for (auto t : touched)
            project.crossfadeOverlaps (t);
    selection = made;
    if (selectionChanged)
        selectionChanged();
}

void DawEditorState::copySelection()
{
    const auto list = selectedClips();
    if (list.empty())
        return;
    daw::int64 first = std::numeric_limits<daw::int64>::max();
    int firstRow = std::numeric_limits<int>::max();
    for (auto& c : list)
    {
        first = juce::jmin (first, c.start);
        firstRow = juce::jmin (firstRow, rowOf (project.trackOfClip (c.id)));
    }
    session.clipboard.clear();
    for (auto& c : list)
    {
        auto copy = c;
        copy.start -= first;
        session.clipboard.push_back ({ rowOf (project.trackOfClip (c.id)) - firstRow, copy });
    }
    if (status)
        status ("Copied " + juce::String ((int) list.size()) + (list.size() == 1 ? " clip" : " clips"));
}

void DawEditorState::cutSelection()
{
    copySelection();
    if (selection.empty())
        return;
    std::set<daw::int64> clipTracks;
    for (auto id : selection)
        clipTracks.insert (project.trackOfClip (id));
    const auto crossfades = crossfadesOn (clipTracks);
    project.beginEdit ("Cut clips");
    for (auto id : selection)
        project.removeClip (id);
    fitCrossfades (crossfades, {});
    clearSelection();
}

void DawEditorState::paste (daw::int64 position)
{
    if (session.clipboard.empty())
        return;
    project.beginEdit ("Paste clips");
    int base = rowOf (session.selectedTrack);
    if (base < 0)
        base = 0;
    std::set<daw::int64> made, touched;
    for (auto& item : session.clipboard)
    {
        const int row = base + item.trackOffset;
        while (project.getNumTracks() <= row)
        {
            daw::TrackData t;
            t.colour = 1 + project.getNumTracks() % 18;
            project.addTrack (t);
        }
        const auto track = project.getTrackData (row).id;
        made.insert (project.addClip (track, daw::clipmath::moveTo (item.clip, position + item.clip.start)));
        touched.insert (track);
    }
    if (session.autoCrossfade)
        for (auto t : touched)
            project.crossfadeOverlaps (t);
    selection = made;
    if (selectionChanged)
        selectionChanged();
}

void DawEditorState::splitAt (daw::int64 position)
{
    std::vector<daw::int64> ids (selection.begin(), selection.end());
    if (ids.empty())
    {
        const int row = rowOf (session.selectedTrack);
        if (row >= 0)
            for (auto& c : clips[(size_t) row])
                if (daw::clipmath::canSplit (c, position))
                    ids.push_back (c.id);
    }
    if (ids.empty())
        return;
    project.beginEdit ("Split");
    const auto made = project.splitClips (ids, position);
    for (auto id : made)
        selection.insert (id);
    if (selectionChanged)
        selectionChanged();
}

void DawEditorState::nudge (int direction, bool fine)
{
    const auto list = selectedClips();
    if (list.empty())
        return;
    double step = grid().step (snap, samplesPerPixel);
    if (step <= 0.0)
        step = project.getSampleRate() * 0.01;
    if (fine)
        step = project.getSampleRate() * 0.001;
    auto delta = (daw::int64) std::llround (step) * direction;
    for (auto& c : list)
        delta = juce::jmax (delta, -c.start);
    if (delta == 0)
        return;
    std::set<daw::int64> clipTracks;
    for (auto& c : list)
        clipTracks.insert (project.trackOfClip (c.id));
    const auto crossfades = crossfadesOn (clipTracks);
    project.beginEdit ("Nudge");
    for (auto& c : list)
        project.setClip (daw::clipmath::moveTo (c, c.start + delta));
    fitCrossfades (crossfades, clipTracks);
}

void DawEditorState::toggleMuteSelection()
{
    const auto list = selectedClips();
    if (list.empty())
        return;
    bool allMuted = true;
    for (auto& c : list)
        allMuted = allMuted && c.mute;
    project.beginEdit (allMuted ? "Unmute clips" : "Mute clips");
    for (auto& c : list)
        project.setClipProperty (c.id, daw::ids::mute, ! allMuted);
}

void DawEditorState::addTrack()
{
    project.beginEdit ("Add track");
    daw::TrackData t;
    const int n = project.getNumTracks();
    t.colour = 1 + n % 18;
    t.source.kind = Recorder::Source::Kind::HardwareInput;
    t.source.port = n;
    const int row = rowOf (session.selectedTrack);
    selectTrack (project.addTrack (t, row >= 0 ? row + 1 : -1));
}

void DawEditorState::deleteTrack (daw::int64 trackId)
{
    if (session.isRecordingTrack (trackId))
    {
        if (status)
            status ("Stop the recording before deleting its track");
        return;
    }
    project.beginEdit ("Delete track");
    project.removeTrack (trackId);
}

void DawEditorState::selectTrack (daw::int64 trackId)
{
    session.selectedTrack = trackId;
    if (selectionChanged)
        selectionChanged();
}

void DawEditorState::zoomAround (double newSamplesPerPixel, float anchorX)
{
    newSamplesPerPixel = juce::jlimit (kMinSamplesPerPixel, kMaxSamplesPerPixel, newSamplesPerPixel);
    const double anchor = viewStart + (double) anchorX * samplesPerPixel;
    samplesPerPixel = newSamplesPerPixel;
    viewStart = anchor - (double) anchorX * samplesPerPixel;
    clampScroll();
    if (viewChanged)
        viewChanged();
}

void DawEditorState::zoomToFit()
{
    const double length = juce::jmax ((double) project.getLength(), project.getSampleRate() * 10.0);
    samplesPerPixel = juce::jlimit (kMinSamplesPerPixel, kMaxSamplesPerPixel, length * 1.04 / (double) juce::jmax (100, viewWidth));
    viewStart = 0.0;
    clampScroll();
    if (viewChanged)
        viewChanged();
}

void DawEditorState::scrollToShow (daw::int64 position)
{
    const float x = xOf (position);
    if (x >= 0.0f && x < (float) viewWidth)
        return;
    viewStart = (double) position - (double) viewWidth * 0.1 * samplesPerPixel;
    clampScroll();
    if (viewChanged)
        viewChanged();
}

void DawEditorState::clampScroll()
{
    viewStart = juce::jmax (0.0, viewStart);
    scrollY = juce::jlimit (0, juce::jmax (0, totalHeight + 40 - viewHeight), scrollY);
}

} // namespace livedaw::ui
