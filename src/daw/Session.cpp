#include "daw/Session.h"

namespace livedaw::daw
{

namespace
{
    constexpr int kTimerHz = 30;
    constexpr juce::uint32 kAutosaveMs = 30000;
    constexpr juce::uint32 kGesturePushMs = 250;
    constexpr const char* recentKey = "daw.recentProjects";
    constexpr int kMaxRecent = 10;

    class JobThread final : public juce::Thread
    {
    public:
        explicit JobThread (std::function<void()> f) : juce::Thread ("Live DAW project job"), fn (std::move (f)) {}
        void run() override { fn(); }

    private:
        std::function<void()> fn;
    };
} // namespace

Session::Session (Recorder& r, juce::PropertiesFile* s) : recorder (r), settings (s)
{
    formats.registerBasicFormats();
    project.clear (recorder.getSampleRate());
    project.addListener (this);
    startTimerHz (kTimerHz);
}

Session::~Session()
{
    stopTimer();
    project.removeListener (this);
    cancel = true;
    if (worker != nullptr)
        worker->stopThread (20000);
    alive.reset();
}

void Session::message (const juce::String& text, bool error)
{
    if (onMessage)
        onMessage (text, error);
}

//==============================================================================
juce::File Session::getDefaultFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Live DAW").getChildFile ("Recordings");
}

juce::File Session::getProjectsFolder() const
{
    return defaultFolder != juce::File() ? defaultFolder : getDefaultFolder();
}

void Session::newProject()
{
    if (isRolling())
        stop();
    collectTakes();
    project.clear (recorder.getSampleRate());
}

juce::Result Session::createProject (const juce::String& name, const juce::File& parent)
{
    const auto clean = juce::File::createLegalFileName (name.trim());
    if (clean.isEmpty())
        return juce::Result::fail ("The project needs a name");
    const auto folder = (parent != juce::File() ? parent : getProjectsFolder()).getChildFile (clean);
    if (folder.getChildFile (clean + Project::fileExtension).exists())
        return juce::Result::fail ("A project called \"" + clean + "\" already exists");
    newProject();
    auto result = project.saveAs (folder);
    if (result.wasOk())
    {
        addRecent (project.getFile());
        message ("New project " + project.getFile().getFullPathName());
    }
    return result;
}

juce::Result Session::openProject (const juce::File& file, bool restoreAutosave)
{
    if (isRolling())
        stop();
    collectTakes();
    const auto autosaveFile = autosaveFileFor (file);
    const bool restore = restoreAutosave && hasNewerAutosave (file);
    auto result = restore ? project.load (autosaveFile, file) : project.load (file);
    if (result.failed())
        return result;
    if (restore)
        project.markDirty();
    else
        autosaveFile.deleteFile();
    addRecent (project.getFile());
    message ((restore ? "Restored the unsaved changes of " : "Opened project ") + file.getFullPathName());
    if (! rateMatches())
        message ("The project runs at " + juce::String (project.getSampleRate() / 1000.0, 1) + " kHz but the console at "
                     + juce::String (recorder.getSampleRate() / 1000.0, 1) + " kHz: playback runs at the wrong speed",
                 true);
    return result;
}

juce::Result Session::save()
{
    if (! project.hasFolder())
        return ensureFolder();
    auto result = project.save();
    if (result.wasOk())
    {
        autosaveFileFor (project.getFile()).deleteFile();
        addRecent (project.getFile());
    }
    return result;
}

juce::Result Session::saveAs (const juce::File& folder)
{
    const auto old = project.hasFolder() ? project.getFile() : juce::File();
    auto result = project.saveAs (folder);
    if (result.wasOk())
    {
        if (old != juce::File())
            autosaveFileFor (old).deleteFile();
        addRecent (project.getFile());
        message ("Saved project as " + project.getFile().getFullPathName());
    }
    return result;
}

juce::Result Session::ensureFolder()
{
    if (project.hasFolder())
        return juce::Result::ok();
    const auto base = "Session " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M%S");
    auto folder = getProjectsFolder().getChildFile (base);
    for (int i = 2; folder.exists(); ++i)
        folder = getProjectsFolder().getChildFile (base + " " + juce::String (i));
    auto result = project.saveAs (folder);
    if (result.wasOk())
    {
        addRecent (project.getFile());
        message ("Project saved as " + project.getFile().getFullPathName());
    }
    return result;
}

juce::File Session::autosaveFileFor (const juce::File& projectFile)
{
    return projectFile.getSiblingFile (projectFile.getFileName() + ".autosave");
}

bool Session::hasNewerAutosave (const juce::File& projectFile)
{
    const auto a = autosaveFileFor (projectFile);
    return a.existsAsFile() && a.getLastModificationTime() > projectFile.getLastModificationTime();
}

void Session::autosave()
{
    lastAutosave = juce::Time::getMillisecondCounter();
    if (! project.hasFolder() || ! project.isDirty())
        return;
    auto result = project.writeTo (autosaveFileFor (project.getFile()));
    if (result.failed())
        message ("Project autosave failed: " + result.getErrorMessage(), true);
}

void Session::addRecent (const juce::File& f)
{
    if (settings == nullptr || f == juce::File())
        return;
    auto list = getRecentProjects();
    list.removeString (f.getFullPathName());
    list.insert (0, f.getFullPathName());
    while (list.size() > kMaxRecent)
        list.remove (list.size() - 1);
    settings->setValue (recentKey, list.joinIntoString ("\n"));
}

juce::StringArray Session::getRecentProjects() const
{
    if (settings == nullptr)
        return {};
    auto list = juce::StringArray::fromLines (settings->getValue (recentKey));
    list.removeEmptyStrings();
    return list;
}

std::vector<juce::File> Session::getSessions() const
{
    std::vector<juce::File> list;
    for (const auto& entry : juce::RangedDirectoryIterator (getProjectsFolder(), false, "*", juce::File::findDirectories))
    {
        const auto dir = entry.getFile();
        const auto f = dir.getChildFile (dir.getFileName() + Project::fileExtension);
        if (f.existsAsFile())
            list.push_back (f);
    }
    std::sort (list.begin(), list.end(), [] (const juce::File& a, const juce::File& b)
    {
        return a.getParentDirectory().getCreationTime() > b.getParentDirectory().getCreationTime();
    });
    return list;
}

juce::Result Session::deleteSession (const juce::File& projectFile)
{
    if (project.hasFolder() && projectFile == project.getFile())
        return juce::Result::fail ("The open project can't be deleted");
    const auto folder = projectFile.getParentDirectory();
    if (! projectFile.existsAsFile() || folder == getProjectsFolder())
        return juce::Result::fail ("Not a project folder: " + folder.getFullPathName());
    if (! folder.moveToTrash() && ! folder.deleteRecursively())
        return juce::Result::fail ("Couldn't delete " + folder.getFullPathName());
    message ("Deleted project " + folder.getFullPathName());
    return juce::Result::ok();
}

juce::Result Session::renameSession (const juce::File& projectFile, const juce::String& newName, juce::File* renamed)
{
    const auto clean = juce::File::createLegalFileName (newName.trim());
    if (clean.isEmpty())
        return juce::Result::fail ("The project needs a name");
    const auto folder = projectFile.getParentDirectory();
    const auto target = folder.getSiblingFile (clean);
    if (target.exists())
        return juce::Result::fail ("\"" + clean + "\" already exists");
    const bool isOpen = project.hasFolder() && projectFile == project.getFile();
    if (isOpen && isRecording())
        return juce::Result::fail ("Stop the recording first");
    if (! folder.moveFileTo (target))
        return juce::Result::fail ("Couldn't rename " + folder.getFullPathName());
    const auto oldFile = target.getChildFile (projectFile.getFileName());
    const auto newFile = target.getChildFile (clean + Project::fileExtension);
    oldFile.moveFileTo (newFile);
    autosaveFileFor (oldFile).moveFileTo (autosaveFileFor (newFile));
    oldFile.getSiblingFile (oldFile.getFileName() + ".bak").deleteFile();
    if (isOpen)
        project.relocate (target);
    if (renamed != nullptr)
        *renamed = newFile;
    if (settings != nullptr)
    {
        auto list = getRecentProjects();
        list.removeString (projectFile.getFullPathName());
        settings->setValue (recentKey, list.joinIntoString ("\n"));
    }
    addRecent (newFile);
    return juce::Result::ok();
}

//==============================================================================
void Session::play()
{
    if (! rateMatches())
        message ("The project's sample rate differs from the console's: playback runs at the wrong speed", true);
    pushPlaylistNow();
    recorder.play();
}

void Session::stop()
{
    recordRequested = false;
    recorder.stop();
    collectTakes();
}

void Session::togglePlay()
{
    if (isRolling())
        stop();
    else
        play();
}

juce::Result Session::record()
{
    if (isRecording())
        return juce::Result::ok();
    collectTakes();

    const auto tracks = project.getTracks();
    bool any = false;
    for (auto& t : tracks)
        any = any || t.arm;
    if (! any)
        return juce::Result::fail ("No track is armed: press R on the tracks to record");

    if (auto r = ensureFolder(); r.failed())
        return r;
    const auto audio = project.getAudioFolder();
    audio.createDirectory();

    std::vector<Recorder::RecordTrack> list;
    std::map<int, int64> map;
    for (int i = 0; i < (int) tracks.size() && (int) list.size() < Recorder::kMaxRecordTracks; ++i)
    {
        const auto& t = tracks[(size_t) i];
        if (! t.arm)
            continue;
        Recorder::RecordTrack rt;
        rt.track = i;
        rt.source = t.source;
        rt.numChannels = t.stereo ? 2 : 1;
        rt.file = uniqueAudioFile (audio, t.name);
        rt.file.create(); // reserve the name for this take
        list.push_back (rt);
        map[i] = t.id;
    }

    pushPlaylistNow();
    auto result = recorder.record (list);
    if (result.failed())
    {
        for (auto& rt : list)
            if (rt.file.getSize() == 0)
                rt.file.deleteFile();
        return result;
    }
    recordTracks = map;
    recordRequested = true;
    recordStart = recorder.getPosition();
    peaks.clear();
    for (auto& rt : list)
        peaks[map[rt.track]].numChannels = rt.numChannels;
    message ("Recording " + juce::String ((int) list.size()) + " tracks into " + audio.getFullPathName());
    return result;
}

void Session::locate (int64 position)
{
    recorder.locate (juce::jmax<int64> (0, position));
}

void Session::jumpToMarker (bool forward)
{
    const auto pos = getPosition();
    std::vector<int64> points { 0, project.getLength() };
    for (auto& m : project.getMarkers())
        points.push_back (m.position);
    std::sort (points.begin(), points.end());
    // A little slack, so a jump back while playing passes the marker just left.
    const int64 slack = (int64) (project.getSampleRate() * (isRolling() ? 0.3 : 0.001));
    int64 target = forward ? project.getLength() : 0;
    if (forward)
    {
        for (auto p : points)
            if (p > pos + slack / 10)
            {
                target = p;
                break;
            }
    }
    else
    {
        for (auto it = points.rbegin(); it != points.rend(); ++it)
            if (*it < pos - slack)
            {
                target = *it;
                break;
            }
    }
    locate (target);
}

void Session::setLoopEnabled (bool on)
{
    project.setLoopOn (on);
    pushPlaylistNow();
}

int64 Session::addMarkerAtPosition()
{
    project.beginEdit ("Add marker");
    return project.addMarker (getPosition());
}

bool Session::rateMatches() const
{
    return std::abs (project.getSampleRate() - recorder.getSampleRate()) < 0.5;
}

//==============================================================================
bool Session::isRecordingTrack (int64 trackId) const
{
    if (! isRecording())
        return false;
    for (auto& [index, id] : recordTracks)
        if (id == trackId)
            return true;
    return false;
}

const std::vector<float>* Session::getRecordingPeaks (int64 trackId, int& numChannels) const
{
    const auto it = peaks.find (trackId);
    if (it == peaks.end())
        return nullptr;
    numChannels = it->second.numChannels;
    return &it->second.minMax;
}

float Session::getTrackLevel (int64 trackId, int channel) const
{
    const auto it = levels.find (trackId);
    return it == levels.end() ? -100.0f : it->second.db[juce::jlimit (0, 1, channel)];
}

int Session::getNumArmed() const
{
    int n = 0;
    for (auto& t : project.getTracks())
        n += t.arm ? 1 : 0;
    return n;
}

int Session::getNumArmedChannels() const
{
    int n = 0;
    for (auto& t : project.getTracks())
        n += t.arm ? (t.stereo ? 2 : 1) : 0;
    return n;
}

juce::int64 Session::getFreeDiskBytes() const
{
    auto folder = project.hasFolder() ? project.getFolder() : getProjectsFolder();
    while (! folder.exists() && folder.getParentDirectory() != folder)
        folder = folder.getParentDirectory();
    return folder.getBytesFreeOnVolume();
}

double Session::getRecordSecondsLeft() const
{
    const int channels = juce::jmax (1, getNumArmedChannels());
    const double bytesPerSecond = (double) channels * recorder.getSampleRate() * 4.0; // 32-bit float
    return (double) getFreeDiskBytes() / bytesPerSecond;
}

//==============================================================================
void Session::setupRecordAllChannels (const std::vector<ChannelInfo>& channels)
{
    project.beginEdit ("Record all channels");
    int playback = 0, index = 0;
    for (auto& ch : channels)
    {
        const int n = ch.stereo ? 2 : 1;
        if (playback + n > Recorder::kNumPlaybackChannels || index >= Recorder::kMaxRecordTracks)
            break;
        Recorder::Source source;
        if (ch.inputPort >= 0)
        {
            source.kind = Recorder::Source::Kind::HardwareInput;
            source.port = ch.inputPort;
        }
        else
        {
            source.kind = Recorder::Source::Kind::StripPreFader;
            source.stripKind = StripKind::Channel;
            source.stripNumber = ch.number;
        }
        const auto name = ch.name.isNotEmpty() ? ch.name : "CH " + juce::String (ch.number);
        if (index < project.getNumTracks())
        {
            const auto id = project.getTrackData (index).id;
            project.setTrackProperty (id, ids::name, name);
            project.setTrackProperty (id, ids::colour, ch.colour);
            project.setTrackProperty (id, ids::stereo, ch.stereo);
            project.setTrackProperty (id, ids::playChannel, playback);
            project.setTrackSource (id, source);
            project.setTrackProperty (id, ids::arm, true, false);
        }
        else
        {
            TrackData t;
            t.name = name;
            t.colour = ch.colour;
            t.stereo = ch.stereo;
            t.source = source;
            t.arm = true;
            t.playChannel = playback;
            project.addTrack (t);
        }
        playback += n;
        ++index;
    }
    for (int i = index; i < project.getNumTracks(); ++i)
        project.setTrackProperty (project.getTrackData (i).id, ids::arm, false, false);
    message ("Record all channels: " + juce::String (index) + " tracks armed");
}

void Session::armAll (bool arm)
{
    for (auto& t : project.getTracks())
        project.setTrackProperty (t.id, ids::arm, arm, false);
}

std::vector<Session::SoundcheckRoute> Session::virtualSoundcheckRoutes (const std::vector<ChannelInfo>& channels) const
{
    std::vector<SoundcheckRoute> routes;
    auto taken = [&routes] (int number)
    {
        for (auto& r : routes)
            if (r.channelNumber == number)
                return true;
        return false;
    };
    for (auto& t : project.getTracks())
    {
        int number = -1;
        if (t.source.kind == Recorder::Source::Kind::HardwareInput)
        {
            for (auto& ch : channels)
                if (ch.inputPort == t.source.port && ! taken (ch.number))
                {
                    number = ch.number;
                    break;
                }
        }
        else if (t.source.stripKind == StripKind::Channel)
        {
            for (auto& ch : channels)
                if (ch.number == t.source.stripNumber && ! taken (ch.number))
                    number = ch.number;
        }
        if (number > 0)
            routes.push_back ({ number, t.playChannel, t.stereo });
    }
    return routes;
}

//==============================================================================
void Session::runJob (const juce::String& name, std::function<void (const std::atomic<bool>&, std::atomic<float>&)> work)
{
    busy = true;
    jobName = name;
    jobProgress = 0.0f;
    cancel = false;
    std::weak_ptr<bool> token = alive;
    worker = std::make_unique<JobThread> ([this, work, token]
    {
        work (cancel, jobProgress);
        juce::MessageManager::callAsync ([this, token]
        {
            if (token.expired())
                return;
            if (worker != nullptr)
                worker->stopThread (1000);
            busy = false;
            jobName = {};
        });
    });
    worker->startThread();
}

void Session::importFiles (const juce::Array<juce::File>& files, int64 trackId, int64 position)
{
    if (files.isEmpty())
        return;
    if (busy)
    {
        message ("Wait for the " + jobName.toLowerCase() + " to finish", true);
        return;
    }
    if (auto r = ensureFolder(); r.failed())
    {
        message (r.getErrorMessage(), true);
        return;
    }
    const auto folder = project.getAudioFolder();
    const double rate = project.getSampleRate();
    std::weak_ptr<bool> token = alive;
    runJob ("IMPORT", [this, files, folder, rate, trackId, position, token] (const std::atomic<bool>& stop, std::atomic<float>& progress)
    {
        std::vector<ImportResult> results;
        for (int i = 0; i < files.size() && ! stop.load(); ++i)
            results.push_back (importAudioFile (formats, files[i], folder, rate, stop, [&progress, i, n = files.size()] (float p)
            {
                progress = ((float) i + p) / (float) n;
            }));

        juce::MessageManager::callAsync ([this, token, results, trackId, position]
        {
            if (token.expired())
                return;
            project.beginEdit ("Import audio");
            int index = project.indexOfTrack (trackId);
            int placed = 0;
            for (auto& r : results)
            {
                if (r.result.failed())
                {
                    message ("Import: " + r.result.getErrorMessage(), true);
                    continue;
                }
                if (index < 0 || index >= project.getNumTracks())
                {
                    TrackData t;
                    t.name = r.name;
                    t.stereo = r.numChannels > 1;
                    t.colour = 1 + (project.getNumTracks() % 18);
                    project.addTrack (t);
                    index = project.getNumTracks() - 1;
                }
                const auto track = project.getTrackData (index);
                if (project.getClips (track.id).empty() && track.stereo != (r.numChannels > 1))
                    project.setTrackProperty (track.id, ids::stereo, r.numChannels > 1);
                ClipData c;
                c.file = project.storedPath (r.file);
                c.fileLength = r.length;
                c.length = r.length;
                c.numChannels = r.numChannels;
                c.start = position;
                c.name = r.name;
                project.addClip (track.id, c);
                if (placed == 0)
                    selectedTrack = track.id;
                ++placed;
                ++index;
            }
            if (placed > 0)
                message ("Imported " + juce::String (placed) + (placed == 1 ? " file" : " files"));
        });
    });
}

void Session::exportTracks (const juce::File& folder)
{
    if (busy)
    {
        message ("Wait for the " + jobName.toLowerCase() + " to finish", true);
        return;
    }
    struct TrackJob
    {
        juce::File dest;
        std::vector<RenderClip> clips;
        float gainDb;
        int numChannels;
    };
    std::vector<TrackJob> jobs;
    const auto tracks = project.getTracks();
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        TrackJob j;
        const auto name = juce::String ((int) i + 1).paddedLeft ('0', 2) + " " + juce::File::createLegalFileName (tracks[i].name);
        j.dest = folder.getChildFile (name + ".wav");
        for (auto& c : project.getClips (tracks[i].id))
            j.clips.push_back ({ project.resolve (c.file), c });
        j.gainDb = tracks[i].gainDb;
        j.numChannels = tracks[i].stereo ? 2 : 1;
        if (! j.clips.empty())
            jobs.push_back (std::move (j));
    }
    if (jobs.empty())
    {
        message ("Nothing to export: no track has clips", true);
        return;
    }
    const auto length = project.getLength();
    const double rate = project.getSampleRate();
    std::weak_ptr<bool> token = alive;
    runJob ("EXPORT", [this, jobs, length, rate, folder, token] (const std::atomic<bool>& stop, std::atomic<float>& progress)
    {
        juce::StringArray errors;
        int done = 0;
        for (size_t i = 0; i < jobs.size() && ! stop.load(); ++i)
        {
            const auto& j = jobs[i];
            auto r = renderTrack (formats, j.clips, j.gainDb, j.numChannels, rate, length, j.dest, stop, [&progress, i, n = jobs.size()] (float p)
            {
                progress = ((float) i + p) / (float) n;
            });
            if (r.failed())
                errors.add (j.dest.getFileName() + ": " + r.getErrorMessage());
            else
                ++done;
        }
        juce::MessageManager::callAsync ([this, token, errors, done, folder]
        {
            if (token.expired())
                return;
            for (auto& e : errors)
                message ("Export: " + e, true);
            message ("Exported " + juce::String (done) + " tracks to " + folder.getFullPathName());
        });
    });
}

//==============================================================================
void Session::projectChanged()
{
    playlistDirty = true;
}

void Session::projectReplaced()
{
    playlistDirty = true;
    selectedTrack = 0;
    clipboard.clear();
    pushPlaylistNow();
    if (onProjectReplaced)
        onProjectReplaced();
}

void Session::pushPlaylistNow()
{
    recorder.setPlaylist (project.buildPlaylist());
    const bool loop = project.isLoopOn() && project.getLoopEnd() > project.getLoopStart();
    recorder.setLoop (project.getLoopStart(), project.getLoopEnd(), loop);
    lastLoopHash = (juce::uint32) (project.getLoopStart() * 31 + project.getLoopEnd() * 7 + (loop ? 1 : 0));
    playlistDirty = false;
    lastPush = juce::Time::getMillisecondCounter();
}

void Session::collectTakes()
{
    auto takes = recorder.takeFinishedTakes();
    if (takes.empty())
        return;
    project.beginEdit ("Record");
    const auto created = project.addTakes (takes, recordTracks, replaceOnRecord);
    for (auto& t : takes)
        if (const auto it = recordTracks.find (t.track); it != recordTracks.end())
            peaks.erase (it->second);
    if (! isRecording())
    {
        recordTracks.clear();
        levels.clear();
    }
    message ("Recorded " + juce::String ((int) created.size()) + (created.size() == 1 ? " take" : " takes"));
    // Takes are files on disk: keep the project file in step with them.
    if (project.hasFolder())
        if (auto r = project.save(); r.failed())
            message ("Couldn't save the project after recording: " + r.getErrorMessage(), true);
}

void Session::pollLevels()
{
    for (auto& [index, trackId] : recordTracks)
    {
        auto& level = levels[trackId];
        for (int ch = 0; ch < 2; ++ch)
        {
            const float db = recorder.getRecordLevel (index, ch);
            level.db[ch] = juce::jmax (db, level.db[ch] - 1.2f);
        }
    }
}

void Session::pollPeaks()
{
    float buffer[2048 * 4];
    for (auto& [index, trackId] : recordTracks)
    {
        auto& p = peaks[trackId];
        const int stride = 2 * p.numChannels;
        const int maxPeaks = (int) (std::size (buffer) / (size_t) stride);
        for (;;)
        {
            const auto first = (int64) (p.minMax.size() / (size_t) stride);
            const int n = recorder.getRecordingPeaks (index, first, maxPeaks, buffer);
            if (n <= 0)
                break;
            p.minMax.insert (p.minMax.end(), buffer, buffer + n * stride);
            if (n < maxPeaks)
                break;
        }
    }
}

void Session::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounter();
    if (recordRequested && recorder.getState() == Recorder::State::Stopped)
        recordRequested = false;
    collectTakes();
    if (isRecording())
    {
        pollLevels();
        pollPeaks();
    }
    else if (! levels.empty())
    {
        for (auto& [id, level] : levels)
            for (auto& db : level.db)
                db = juce::jmax (-100.0f, db - 1.2f);
    }

    if (playlistDirty && (! gesture || now - lastPush > kGesturePushMs))
        pushPlaylistNow();

    if (now - lastAutosave > kAutosaveMs)
        autosave();
}

} // namespace livedaw::daw
