#include "daw/Project.h"

namespace livedaw::daw
{

namespace
{
    int64 toInt64 (const juce::var& v) { return static_cast<int64> (v); }

    float dbToGain (float db) { return db <= -100.0f ? 0.0f : std::pow (10.0f, db / 20.0f); }

    const char* shapeName (FadeShape s)
    {
        switch (s)
        {
            case FadeShape::Linear:     return "linear";
            case FadeShape::EqualPower: return "power";
            case FadeShape::SCurve:     return "scurve";
        }
        return "power";
    }

    FadeShape shapeFromName (const juce::String& s)
    {
        if (s == "linear" || s == "0")
            return FadeShape::Linear;
        if (s == "scurve" || s == "2")
            return FadeShape::SCurve;
        return FadeShape::EqualPower;
    }

    const char* sourceKindName (Recorder::Source::Kind k)
    {
        switch (k)
        {
            case Recorder::Source::Kind::HardwareInput:  return "in";
            case Recorder::Source::Kind::StripPreFader:  return "pre";
            case Recorder::Source::Kind::StripPostFader: return "post";
        }
        return "in";
    }

    Recorder::Source::Kind sourceKindFromName (const juce::String& s)
    {
        if (s == "pre")
            return Recorder::Source::Kind::StripPreFader;
        if (s == "post")
            return Recorder::Source::Kind::StripPostFader;
        return Recorder::Source::Kind::HardwareInput;
    }

    juce::String uniqueName (const std::vector<TrackData>& tracks, const juce::String& base)
    {
        auto taken = [&] (const juce::String& n)
        {
            for (auto& t : tracks)
                if (t.name == n)
                    return true;
            return false;
        };
        if (base.isNotEmpty() && ! taken (base))
            return base;
        for (int i = (int) tracks.size() + 1;; ++i)
        {
            const auto n = (base.isNotEmpty() ? base + " " : juce::String ("Track ")) + juce::String (i);
            if (! taken (n))
                return n;
        }
    }
} // namespace

//==============================================================================
namespace clipmath
{
    void clampFades (ClipData& c)
    {
        c.fadeIn = juce::jlimit<int64> (0, c.length, c.fadeIn);
        c.fadeOut = juce::jlimit<int64> (0, c.length, c.fadeOut);
        if (c.fadeIn + c.fadeOut > c.length)
        {
            // Shrink both in proportion so they meet.
            const double scale = (double) c.length / (double) (c.fadeIn + c.fadeOut);
            c.fadeIn = (int64) ((double) c.fadeIn * scale);
            c.fadeOut = c.length - c.fadeIn;
        }
    }

    ClipData moveTo (ClipData c, int64 newStart)
    {
        c.start = juce::jmax<int64> (0, newStart);
        return c;
    }

    ClipData trimStart (ClipData c, int64 newStart)
    {
        // The new start can't reveal audio before the file's start, go below
        // the timeline's zero or leave less than the minimum length.
        const int64 earliest = c.start - juce::jmin (c.fileOffset, c.start);
        const int64 latest = c.end() - juce::jmin (kMinLength, c.length);
        newStart = juce::jlimit (earliest, juce::jmax (earliest, latest), newStart);
        const int64 delta = newStart - c.start;
        c.start += delta;
        c.fileOffset += delta;
        c.length -= delta;
        clampFades (c);
        return c;
    }

    ClipData trimEnd (ClipData c, int64 newEnd)
    {
        const int64 available = c.fileLength > 0 ? c.fileLength - c.fileOffset : std::numeric_limits<int64>::max() / 4;
        const int64 shortest = juce::jmin (kMinLength, c.length);
        c.length = juce::jlimit (shortest, juce::jmax (shortest, available), newEnd - c.start);
        clampFades (c);
        return c;
    }

    bool canSplit (const ClipData& c, int64 at)
    {
        return at >= c.start + kMinLength && at <= c.end() - kMinLength;
    }

    std::pair<ClipData, ClipData> split (const ClipData& c, int64 at)
    {
        jassert (canSplit (c, at));
        auto left = c, right = c;
        left.length = at - c.start;
        left.fadeOut = 0;
        right.start = at;
        right.fileOffset = c.fileOffset + (at - c.start);
        right.length = c.end() - at;
        right.fadeIn = 0;
        clampFades (left);
        clampFades (right);
        return { left, right };
    }

    std::vector<ClipData> removeRange (const ClipData& c, int64 from, int64 to)
    {
        if (to <= c.start || from >= c.end() || to <= from)
            return { c };
        std::vector<ClipData> parts;
        if (from - c.start >= kMinLength)
        {
            auto left = c;
            left.length = from - c.start;
            left.fadeOut = juce::jmin (left.fadeOut, left.length / 2);
            clampFades (left);
            parts.push_back (left);
        }
        if (c.end() - to >= kMinLength)
        {
            auto right = c;
            right.start = to;
            right.fileOffset = c.fileOffset + (to - c.start);
            right.length = c.end() - to;
            right.fadeIn = juce::jmin (right.fadeIn, right.length / 2);
            clampFades (right);
            parts.push_back (right);
        }
        return parts;
    }

    float fadeGain (FadeShape shape, float x) noexcept
    {
        x = juce::jlimit (0.0f, 1.0f, x);
        switch (shape)
        {
            case FadeShape::Linear:     return x;
            case FadeShape::EqualPower: return std::sin (x * juce::MathConstants<float>::halfPi);
            case FadeShape::SCurve:     return 0.5f - 0.5f * std::cos (x * juce::MathConstants<float>::pi);
        }
        return x;
    }
} // namespace clipmath

juce::String describeSource (const Recorder::Source& s)
{
    switch (s.kind)
    {
        case Recorder::Source::Kind::HardwareInput:
            return "IN " + juce::String (s.port + 1);
        case Recorder::Source::Kind::StripPreFader:
            return juce::String (label (s.stripKind)) + " " + juce::String (s.stripNumber) + " PRE";
        case Recorder::Source::Kind::StripPostFader:
            return juce::String (label (s.stripKind)) + " " + juce::String (s.stripNumber) + " POST";
    }
    return {};
}

//==============================================================================
Project::Project()
{
    state = makeEmpty (48000.0);
    state.addListener (this);
}

Project::~Project()
{
    state.removeListener (this);
}

juce::ValueTree Project::makeEmpty (double sampleRate)
{
    juce::ValueTree t (ids::LDPROJECT);
    t.setProperty (ids::version, kFormatVersion, nullptr);
    t.setProperty (ids::nextId, 1, nullptr);
    t.setProperty (ids::sampleRate, sampleRate, nullptr);
    t.setProperty (ids::tempo, 120.0, nullptr);
    t.setProperty (ids::sigNum, 4, nullptr);
    t.setProperty (ids::sigDen, 4, nullptr);
    t.setProperty (ids::loopStart, 0, nullptr);
    t.setProperty (ids::loopEnd, 0, nullptr);
    t.setProperty (ids::loopOn, false, nullptr);
    t.appendChild (juce::ValueTree (ids::TRACKS), nullptr);
    t.appendChild (juce::ValueTree (ids::MARKERS), nullptr);
    return t;
}

void Project::changed()
{
    ++changeCount;
    if (! loading)
        listeners.call ([] (Listener& l) { l.projectChanged(); });
}

void Project::replaced()
{
    listeners.call ([] (Listener& l) { l.projectReplaced(); });
}

int64 Project::newId()
{
    const auto id = juce::jmax<int64> (1, toInt64 (state[ids::nextId]));
    state.setProperty (ids::nextId, id + 1, nullptr);
    return id;
}

juce::ValueTree Project::tracksTree() const { return state.getChildWithName (ids::TRACKS); }
juce::ValueTree Project::markersTree() const { return state.getChildWithName (ids::MARKERS); }

//==============================================================================
void Project::clear (double sampleRate)
{
    loading = true;
    state.copyPropertiesAndChildrenFrom (makeEmpty (sampleRate > 0.0 ? sampleRate : 48000.0), nullptr);
    loading = false;
    folder = juce::File();
    undo.clearUndoHistory();
    savedChangeCount = ++changeCount;
    replaced();
}

juce::File Project::getFile() const
{
    return hasFolder() ? folder.getChildFile (folder.getFileName() + fileExtension) : juce::File();
}

juce::File Project::getAudioFolder() const
{
    return hasFolder() ? folder.getChildFile ("Audio") : juce::File();
}

juce::String Project::getName() const
{
    return hasFolder() ? folder.getFileName() : juce::String ("Untitled");
}

juce::File Project::resolve (const juce::String& stored) const
{
    if (juce::File::isAbsolutePath (stored))
        return juce::File (stored);
    return hasFolder() ? folder.getChildFile (stored) : juce::File();
}

juce::String Project::storedPath (const juce::File& f) const
{
    if (hasFolder() && f.isAChildOf (folder))
        return f.getRelativePathFrom (folder).replaceCharacter ('\\', '/');
    return f.getFullPathName();
}

juce::Result Project::writeTo (const juce::File& file) const
{
    auto xml = state.createXml();
    if (xml == nullptr)
        return juce::Result::fail ("Couldn't serialise the project");

    // Write a temporary file, check it reads back, then swap it in.
    const auto temp = file.getSiblingFile (file.getFileName() + ".tmp");
    if (! xml->writeTo (temp))
        return juce::Result::fail ("Couldn't write " + temp.getFullPathName());
    if (juce::parseXML (temp) == nullptr)
    {
        temp.deleteFile();
        return juce::Result::fail ("The written project didn't read back: " + temp.getFullPathName());
    }
    if (file.existsAsFile())
        file.copyFileTo (file.getSiblingFile (file.getFileName() + ".bak"));
    if (! temp.moveFileTo (file))
        return juce::Result::fail ("Couldn't replace " + file.getFullPathName());
    return juce::Result::ok();
}

juce::Result Project::saveAs (const juce::File& newFolder)
{
    if (newFolder == juce::File())
        return juce::Result::fail ("No folder");
    if (! newFolder.createDirectory() || ! newFolder.getChildFile ("Audio").createDirectory())
        return juce::Result::fail ("Couldn't create " + newFolder.getFullPathName());

    // Clip paths are relative to the folder: re-anchor them to the new one.
    const auto oldFolder = folder;
    if (oldFolder != newFolder)
    {
        for (auto track : tracksTree())
            for (auto clip : track)
            {
                const auto stored = clip[ids::file].toString();
                const auto file = juce::File::isAbsolutePath (stored) ? juce::File (stored)
                                  : (oldFolder != juce::File() ? oldFolder.getChildFile (stored) : juce::File());
                if (file != juce::File())
                {
                    const auto path = file.isAChildOf (newFolder) ? file.getRelativePathFrom (newFolder).replaceCharacter ('\\', '/') : file.getFullPathName();
                    clip.setProperty (ids::file, path, nullptr);
                }
            }
    }
    folder = newFolder;
    return save();
}

juce::Result Project::save()
{
    if (! hasFolder())
        return juce::Result::fail ("The project has no folder yet");
    getAudioFolder().createDirectory();
    auto result = writeTo (getFile());
    if (result.wasOk())
        savedChangeCount = changeCount;
    return result;
}

juce::Result Project::load (const juce::File& file, const juce::File& asFile)
{
    auto xml = juce::parseXML (file);
    if (xml == nullptr)
    {
        // Fall back to the backup of the last save.
        const auto backup = file.getSiblingFile (file.getFileName() + ".bak");
        xml = juce::parseXML (backup);
        if (xml == nullptr)
            return juce::Result::fail ("Couldn't read " + file.getFullPathName());
    }
    auto loaded = juce::ValueTree::fromXml (*xml);
    if (! loaded.hasType (ids::LDPROJECT))
        return juce::Result::fail (file.getFileName() + " is not a Sidecar project");
    if ((int) loaded[ids::version] > kFormatVersion)
        return juce::Result::fail (file.getFileName() + " was made by a newer version of Sidecar");

    // Fill in what an older or hand-edited file may lack.
    auto fresh = makeEmpty ((double) loaded.getProperty (ids::sampleRate, 48000.0));
    for (int i = 0; i < fresh.getNumProperties(); ++i)
        if (! loaded.hasProperty (fresh.getPropertyName (i)))
            loaded.setProperty (fresh.getPropertyName (i), fresh.getProperty (fresh.getPropertyName (i)), nullptr);
    for (auto id : { ids::TRACKS, ids::MARKERS })
        if (! loaded.getChildWithName (id).isValid())
            loaded.appendChild (juce::ValueTree (id), nullptr);

    loading = true;
    state.copyPropertiesAndChildrenFrom (loaded, nullptr);
    loading = false;
    folder = (asFile != juce::File() ? asFile : file).getParentDirectory();
    undo.clearUndoHistory();
    savedChangeCount = ++changeCount;
    replaced();
    return juce::Result::ok();
}

//==============================================================================
void Project::beginEdit (const juce::String& actionName)
{
    undo.beginNewTransaction (actionName);
}

double Project::getSampleRate() const { return juce::jmax (8000.0, (double) state[ids::sampleRate]); }
double Project::getTempo() const { return juce::jlimit (20.0, 400.0, (double) state[ids::tempo]); }
int Project::getSigNumerator() const { return juce::jlimit (1, 32, (int) state[ids::sigNum]); }
int Project::getSigDenominator() const
{
    const int d = (int) state[ids::sigDen];
    return d == 2 || d == 4 || d == 8 || d == 16 ? d : 4;
}

void Project::setTempo (double bpm) { state.setProperty (ids::tempo, juce::jlimit (20.0, 400.0, bpm), &undo); }

void Project::setTimeSignature (int numerator, int denominator)
{
    state.setProperty (ids::sigNum, juce::jlimit (1, 32, numerator), &undo);
    state.setProperty (ids::sigDen, denominator == 2 || denominator == 8 || denominator == 16 ? denominator : 4, &undo);
}

bool Project::setSampleRate (double rate)
{
    if (rate < 8000.0)
        return false;
    for (auto t : tracksTree())
        if (t.getNumChildren() > 0)
            return false;
    const double ratio = rate / getSampleRate();
    auto scaled = [ratio] (const juce::var& v) { return (int64) std::llround ((double) toInt64 (v) * ratio); };
    for (auto m : markersTree())
        m.setProperty (ids::position, scaled (m[ids::position]), &undo);
    state.setProperty (ids::loopStart, scaled (state[ids::loopStart]), &undo);
    state.setProperty (ids::loopEnd, scaled (state[ids::loopEnd]), &undo);
    state.setProperty (ids::sampleRate, rate, &undo);
    return true;
}

int64 Project::getLoopStart() const { return juce::jmax<int64> (0, toInt64 (state[ids::loopStart])); }
int64 Project::getLoopEnd() const { return juce::jmax (getLoopStart(), toInt64 (state[ids::loopEnd])); }
bool Project::isLoopOn() const { return (bool) state[ids::loopOn]; }

void Project::setLoop (int64 start, int64 end)
{
    if (end < start)
        std::swap (start, end);
    state.setProperty (ids::loopStart, juce::jmax<int64> (0, start), &undo);
    state.setProperty (ids::loopEnd, juce::jmax<int64> (0, end), &undo);
}

void Project::setLoopOn (bool on) { state.setProperty (ids::loopOn, on, nullptr); }
bool Project::isAToBOn() const { return (bool) state.getProperty (ids::abOn, false); }
void Project::setAToBOn (bool on) { state.setProperty (ids::abOn, on, nullptr); }

double Project::samplesPerBeat() const
{
    // The beat is the time signature's denominator note.
    return getSampleRate() * 60.0 / getTempo() * 4.0 / (double) getSigDenominator();
}

double Project::samplesPerBar() const { return samplesPerBeat() * (double) getSigNumerator(); }

int64 Project::getLength() const
{
    int64 end = 0;
    for (auto track : tracksTree())
        for (auto clip : track)
            end = juce::jmax (end, toInt64 (clip[ids::start]) + toInt64 (clip[ids::length]));
    return end;
}

//==============================================================================
int Project::getNumTracks() const { return tracksTree().getNumChildren(); }
juce::ValueTree Project::getTrack (int index) const { return tracksTree().getChild (index); }
juce::ValueTree Project::findTrack (int64 trackId) const { return tracksTree().getChildWithProperty (ids::id, trackId); }

int Project::indexOfTrack (int64 trackId) const
{
    const auto t = tracksTree();
    for (int i = 0; i < t.getNumChildren(); ++i)
        if (toInt64 (t.getChild (i)[ids::id]) == trackId)
            return i;
    return -1;
}

TrackData Project::trackFromTree (const juce::ValueTree& t)
{
    TrackData d;
    d.id = toInt64 (t[ids::id]);
    d.name = t[ids::name].toString();
    d.colour = juce::jlimit (1, 18, (int) t.getProperty (ids::colour, 1));
    d.stereo = (bool) t[ids::stereo];
    d.source.kind = sourceKindFromName (t[ids::srcKind].toString());
    d.source.port = juce::jmax (0, (int) t[ids::srcPort]);
    d.source.stripKind = stripKindFromString (t.getProperty (ids::srcStrip, "ch").toString());
    d.source.stripNumber = juce::jmax (1, (int) t.getProperty (ids::srcNumber, 1));
    d.arm = (bool) t[ids::arm];
    d.mute = (bool) t[ids::mute];
    d.solo = (bool) t[ids::solo];
    d.gainDb = (float) (double) t[ids::gain];
    d.playChannel = juce::jlimit (0, Recorder::kNumPlaybackChannels - 1, (int) t[ids::playChannel]);
    d.height = (int) t[ids::height];
    return d;
}

TrackData Project::getTrackData (int index) const { return trackFromTree (getTrack (index)); }

std::vector<TrackData> Project::getTracks() const
{
    std::vector<TrackData> list;
    for (auto t : tracksTree())
        list.push_back (trackFromTree (t));
    return list;
}

int Project::freePlaybackChannel (int numChannels) const
{
    std::vector<bool> used ((size_t) Recorder::kNumPlaybackChannels, false);
    for (auto& t : getTracks())
        for (int c = 0; c < (t.stereo ? 2 : 1); ++c)
            if (juce::isPositiveAndBelow (t.playChannel + c, Recorder::kNumPlaybackChannels))
                used[(size_t) (t.playChannel + c)] = true;
    for (int ch = 0; ch + numChannels <= Recorder::kNumPlaybackChannels; ++ch)
    {
        bool free = true;
        for (int c = 0; c < numChannels; ++c)
            free = free && ! used[(size_t) (ch + c)];
        if (free)
            return ch;
    }
    return 0;
}

bool Project::anySolo() const
{
    for (auto t : tracksTree())
        if ((bool) t[ids::solo])
            return true;
    return false;
}

int64 Project::addTrack (TrackData t, int index)
{
    const auto existing = getTracks();
    t.id = newId();
    t.name = uniqueName (existing, t.name);
    if (t.playChannel <= 0 && existing.size() > 0)
        t.playChannel = freePlaybackChannel (t.stereo ? 2 : 1);
    if (t.colour <= 0)
        t.colour = 1;

    juce::ValueTree tree (ids::TRACK);
    tree.setProperty (ids::id, t.id, nullptr);
    tree.setProperty (ids::name, t.name, nullptr);
    tree.setProperty (ids::colour, t.colour, nullptr);
    tree.setProperty (ids::stereo, t.stereo, nullptr);
    tree.setProperty (ids::srcKind, sourceKindName (t.source.kind), nullptr);
    tree.setProperty (ids::srcPort, t.source.port, nullptr);
    tree.setProperty (ids::srcStrip, toString (t.source.stripKind), nullptr);
    tree.setProperty (ids::srcNumber, t.source.stripNumber, nullptr);
    tree.setProperty (ids::arm, t.arm, nullptr);
    tree.setProperty (ids::mute, t.mute, nullptr);
    tree.setProperty (ids::solo, t.solo, nullptr);
    tree.setProperty (ids::gain, t.gainDb, nullptr);
    tree.setProperty (ids::playChannel, t.playChannel, nullptr);
    tree.setProperty (ids::height, t.height, nullptr);
    tracksTree().addChild (tree, index, &undo);
    return t.id;
}

void Project::removeTrack (int64 trackId)
{
    auto t = findTrack (trackId);
    if (t.isValid())
        tracksTree().removeChild (t, &undo);
}

void Project::moveTrack (int64 trackId, int newIndex)
{
    const int from = indexOfTrack (trackId);
    if (from < 0)
        return;
    newIndex = juce::jlimit (0, getNumTracks() - 1, newIndex);
    if (newIndex != from)
        tracksTree().moveChild (from, newIndex, &undo);
}

void Project::setTrackProperty (int64 trackId, const juce::Identifier& property, const juce::var& value, bool undoable)
{
    auto t = findTrack (trackId);
    if (t.isValid())
        t.setProperty (property, value, undoable ? &undo : nullptr);
}

void Project::setTrackSource (int64 trackId, const Recorder::Source& s)
{
    auto t = findTrack (trackId);
    if (! t.isValid())
        return;
    t.setProperty (ids::srcKind, sourceKindName (s.kind), &undo);
    t.setProperty (ids::srcPort, s.port, &undo);
    t.setProperty (ids::srcStrip, toString (s.stripKind), &undo);
    t.setProperty (ids::srcNumber, s.stripNumber, &undo);
}

//==============================================================================
ClipData Project::clipFromTree (const juce::ValueTree& c)
{
    ClipData d;
    d.id = toInt64 (c[ids::id]);
    d.file = c[ids::file].toString();
    d.fileOffset = juce::jmax<int64> (0, toInt64 (c[ids::fileOffset]));
    d.fileLength = juce::jmax<int64> (0, toInt64 (c[ids::fileLength]));
    d.numChannels = juce::jlimit (1, 2, (int) c.getProperty (ids::channels, 1));
    d.start = juce::jmax<int64> (0, toInt64 (c[ids::start]));
    d.length = juce::jmax<int64> (1, toInt64 (c[ids::length]));
    d.gainDb = (float) (double) c[ids::gain];
    d.fadeIn = juce::jmax<int64> (0, toInt64 (c[ids::fadeIn]));
    d.fadeOut = juce::jmax<int64> (0, toInt64 (c[ids::fadeOut]));
    d.fadeInShape = shapeFromName (c[ids::fadeInShape].toString());
    d.fadeOutShape = shapeFromName (c[ids::fadeOutShape].toString());
    d.mute = (bool) c[ids::mute];
    d.name = c[ids::name].toString();
    return d;
}

void Project::writeClip (juce::ValueTree& c, const ClipData& d, juce::UndoManager* um)
{
    c.setProperty (ids::file, d.file, um);
    c.setProperty (ids::fileOffset, d.fileOffset, um);
    c.setProperty (ids::fileLength, d.fileLength, um);
    c.setProperty (ids::channels, d.numChannels, um);
    c.setProperty (ids::start, d.start, um);
    c.setProperty (ids::length, d.length, um);
    c.setProperty (ids::gain, d.gainDb, um);
    c.setProperty (ids::fadeIn, d.fadeIn, um);
    c.setProperty (ids::fadeOut, d.fadeOut, um);
    c.setProperty (ids::fadeInShape, shapeName (d.fadeInShape), um);
    c.setProperty (ids::fadeOutShape, shapeName (d.fadeOutShape), um);
    c.setProperty (ids::mute, d.mute, um);
    c.setProperty (ids::name, d.name, um);
}

std::vector<ClipData> Project::getClips (int64 trackId) const
{
    std::vector<ClipData> list;
    for (auto c : findTrack (trackId))
        list.push_back (clipFromTree (c));
    return list;
}

juce::ValueTree Project::findClip (int64 clipId) const
{
    for (auto t : tracksTree())
    {
        auto c = t.getChildWithProperty (ids::id, clipId);
        if (c.isValid())
            return c;
    }
    return {};
}

std::optional<ClipData> Project::getClip (int64 clipId) const
{
    const auto c = findClip (clipId);
    if (! c.isValid())
        return std::nullopt;
    return clipFromTree (c);
}

int64 Project::trackOfClip (int64 clipId) const
{
    const auto c = findClip (clipId);
    return c.isValid() ? toInt64 (c.getParent()[ids::id]) : 0;
}

int64 Project::addClip (int64 trackId, ClipData d)
{
    auto t = findTrack (trackId);
    if (! t.isValid())
        return 0;
    d.id = newId();
    clipmath::clampFades (d);
    juce::ValueTree c (ids::CLIP);
    c.setProperty (ids::id, d.id, nullptr);
    writeClip (c, d, nullptr);
    t.appendChild (c, &undo);
    return d.id;
}

void Project::removeClip (int64 clipId)
{
    auto c = findClip (clipId);
    if (c.isValid())
        c.getParent().removeChild (c, &undo);
}

void Project::setClip (const ClipData& d)
{
    auto c = findClip (d.id);
    if (! c.isValid())
        return;
    auto clamped = d;
    clipmath::clampFades (clamped);
    writeClip (c, clamped, &undo);
}

void Project::setClipProperty (int64 clipId, const juce::Identifier& property, const juce::var& value)
{
    auto c = findClip (clipId);
    if (c.isValid())
        c.setProperty (property, value, &undo);
}

void Project::moveClipToTrack (int64 clipId, int64 trackId)
{
    auto c = findClip (clipId);
    auto t = findTrack (trackId);
    if (! c.isValid() || ! t.isValid() || c.getParent() == t)
        return;
    c.getParent().removeChild (c, &undo);
    t.appendChild (c, &undo);
}

std::vector<int64> Project::splitClips (const std::vector<int64>& clipIds, int64 at)
{
    std::vector<int64> created;
    for (auto id : clipIds)
    {
        const auto c = getClip (id);
        if (! c || ! clipmath::canSplit (*c, at))
            continue;
        const auto [left, right] = clipmath::split (*c, at);
        setClip (left);
        created.push_back (addClip (trackOfClip (id), right));
    }
    return created;
}

void Project::clearRange (int64 trackId, int64 from, int64 to, int64 keep)
{
    if (to <= from)
        return;
    for (auto& c : getClips (trackId))
    {
        if (c.id == keep || c.end() <= from || c.start >= to)
            continue;
        const auto parts = clipmath::removeRange (c, from, to);
        removeClip (c.id);
        for (auto& p : parts)
            addClip (trackId, p);
    }
}

void Project::crossfadeOverlaps (int64 trackId)
{
    auto clips = getClips (trackId);
    std::sort (clips.begin(), clips.end(), [] (const ClipData& a, const ClipData& b) { return a.start < b.start; });
    for (size_t i = 0; i < clips.size(); ++i)
        for (size_t j = i + 1; j < clips.size(); ++j)
        {
            auto& a = clips[i];
            auto& b = clips[j];
            if (b.start >= a.end())
                break;
            if (a.mute || b.mute || b.end() <= a.end())
                continue; // containment: no crossfade
            const int64 overlap = a.end() - b.start;
            if (a.fadeOut != overlap || b.fadeIn != overlap)
            {
                a.fadeOut = overlap;
                b.fadeIn = overlap;
                clipmath::clampFades (a);
                clipmath::clampFades (b);
                setClip (a);
                setClip (b);
            }
        }
}

std::vector<Project::Crossfade> Project::getCrossfades (int64 trackId) const
{
    std::vector<Crossfade> list;
    const auto clips = getClips (trackId);
    for (auto& a : clips)
        for (auto& b : clips)
        {
            const int64 overlap = a.end() - b.start;
            if (a.id != b.id && a.start < b.start && overlap > 0 && b.end() > a.end() && a.fadeOut == overlap && b.fadeIn == overlap)
                list.push_back ({ a.id, b.id, overlap });
        }
    return list;
}

void Project::releaseCrossfades (const std::vector<Crossfade>& crossfades)
{
    for (auto& x : crossfades)
    {
        const auto a = getClip (x.first), b = getClip (x.second);
        if (a && b && trackOfClip (a->id) == trackOfClip (b->id) && b->start < a->end() && a->start < b->end())
            continue; // still overlapping: crossfadeOverlaps() fits the fades
        if (a && a->fadeOut == x.length)
            setClipProperty (a->id, ids::fadeOut, (int64) 0);
        if (b && b->fadeIn == x.length)
            setClipProperty (b->id, ids::fadeIn, (int64) 0);
    }
}

//==============================================================================
std::vector<MarkerData> Project::getMarkers() const
{
    std::vector<MarkerData> list;
    for (auto m : markersTree())
        list.push_back ({ toInt64 (m[ids::id]), juce::jmax<int64> (0, toInt64 (m[ids::position])), m[ids::name].toString() });
    std::stable_sort (list.begin(), list.end(), [] (const MarkerData& a, const MarkerData& b) { return a.position < b.position; });
    return list;
}

int64 Project::addMarker (int64 position, const juce::String& name)
{
    const auto id = newId();
    juce::ValueTree m (ids::MARKER);
    m.setProperty (ids::id, id, nullptr);
    m.setProperty (ids::position, juce::jmax<int64> (0, position), nullptr);
    m.setProperty (ids::name, name.isNotEmpty() ? name : "M" + juce::String (markersTree().getNumChildren() + 1), nullptr);
    markersTree().appendChild (m, &undo);
    return id;
}

void Project::removeMarker (int64 markerId)
{
    auto m = markersTree().getChildWithProperty (ids::id, markerId);
    if (m.isValid())
        markersTree().removeChild (m, &undo);
}

void Project::setMarker (int64 markerId, int64 position, const juce::String& name)
{
    auto m = markersTree().getChildWithProperty (ids::id, markerId);
    if (! m.isValid())
        return;
    m.setProperty (ids::position, juce::jmax<int64> (0, position), &undo);
    m.setProperty (ids::name, name, &undo);
}

//==============================================================================
Recorder::Playlist Project::buildPlaylist() const
{
    Recorder::Playlist playlist;
    const bool solo = anySolo();
    for (auto t : tracksTree())
    {
        const auto track = trackFromTree (t);
        Recorder::PlaybackTrack p;
        p.channel = track.playChannel;
        p.numChannels = track.stereo ? 2 : 1;
        if (p.channel + p.numChannels > Recorder::kNumPlaybackChannels)
            p.channel = Recorder::kNumPlaybackChannels - p.numChannels;
        p.gain = dbToGain (track.gainDb);
        p.mute = track.mute || (solo && ! track.solo);
        for (auto c : t)
        {
            const auto clip = clipFromTree (c);
            if (clip.mute || clip.length <= 0)
                continue;
            const auto file = resolve (clip.file);
            if (! file.existsAsFile())
                continue;
            Recorder::Clip rc;
            rc.file = file;
            rc.fileOffset = clip.fileOffset;
            rc.start = clip.start;
            rc.length = clip.length;
            rc.gain = dbToGain (clip.gainDb);
            rc.fadeIn = clip.fadeIn;
            rc.fadeOut = clip.fadeOut;
            rc.fadeInShape = clip.fadeInShape;
            rc.fadeOutShape = clip.fadeOutShape;
            p.clips.push_back (rc);
        }
        std::sort (p.clips.begin(), p.clips.end(), [] (const Recorder::Clip& a, const Recorder::Clip& b) { return a.start < b.start; });
        playlist.push_back (std::move (p));
    }
    return playlist;
}

std::vector<int64> Project::addTakes (const std::vector<Recorder::Take>& takes, const std::map<int, int64>& trackOfRecordIndex, bool replace)
{
    std::vector<int64> created;
    for (auto& take : takes)
    {
        const auto it = trackOfRecordIndex.find (take.track);
        if (it == trackOfRecordIndex.end() || ! findTrack (it->second).isValid() || take.length <= 0)
            continue;
        if (replace)
            clearRange (it->second, take.start, take.start + take.length);
        ClipData c;
        c.file = storedPath (take.file);
        c.fileOffset = 0;
        c.fileLength = take.length;
        c.numChannels = take.numChannels;
        c.start = take.start;
        c.length = take.length;
        c.fadeIn = c.fadeOut = 0;
        c.name = take.file.getFileNameWithoutExtension();
        created.push_back (addClip (it->second, c));
    }
    return created;
}

} // namespace livedaw::daw
