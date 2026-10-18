#include "engine/UsbRecorder.h"
#include "engine/RustStrings.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace livedaw
{

static_assert (UsbRecorder::kNumChannels == ld::kUsbChannels);
static_assert (UsbRecorder::kPlayerSourceBase == ld::kUsbPlayerSourceBase);
static_assert ((int) UsbRecorder::RecState::Stopped == 0 && (int) UsbRecorder::RecState::Recording == 1
               && (int) UsbRecorder::RecState::Paused == 2 && (int) UsbRecorder::RecState::Error == 3);
static_assert ((int) UsbRecorder::PlayState::Stopped == 0 && (int) UsbRecorder::PlayState::Playing == 1
               && (int) UsbRecorder::PlayState::Paused == 2 && (int) UsbRecorder::PlayState::Error == 3);

namespace
{
    constexpr int kTapKindSpan = 0x100000;

    // Settings keys.
    constexpr const char* kStorageKey = "usb.storage";
    constexpr const char* kRecPathKey = "usb.recPath";
    constexpr const char* kBrowserKey = "usb.browser";
    constexpr const char* kChannelsKey = "usb.recChannels";
    constexpr const char* kBitsKey = "usb.recBits";
    constexpr const char* kTapKey = "usb.tap";
    constexpr const char* kFullSpeedKey = "usb.hostSpeedFs";

    juce::Result failed (ld::LdString* error, const char* fallback)
    {
        const auto message = takeString (error);
        return juce::Result::fail (message.isNotEmpty() ? message : juce::String (fallback));
    }

    juce::File fileOf (ld::LdString* s)
    {
        const auto path = takeString (s);
        return path.isNotEmpty() ? juce::File (path) : juce::File();
    }

    bool isWav (const juce::File& f) { return f.hasFileExtension ("wav"); }
    bool isPlaylist (const juce::File& f) { return f.hasFileExtension ("m3u"); }
} // namespace

//==============================================================================
int UsbRecorder::Tap::code() const noexcept
{
    switch (kind)
    {
        case Kind::Off: return -1;
        case Kind::Source: return source >= 0 && source < ld::kUsbTapStripBase ? source : -1;
        case Kind::Strip:
            if (stripNumber < 1 || stripNumber > kTapKindSpan / 2)
                return -1;
            return ld::kUsbTapStripBase + (int) stripKind * kTapKindSpan + (stripNumber - 1) * 2 + juce::jlimit (0, 1, channel);
    }
    return -1;
}

UsbRecorder::Tap UsbRecorder::Tap::fromCode (int code) noexcept
{
    Tap t;
    if (code < 0)
        return t;
    if (code < ld::kUsbTapStripBase)
    {
        t.kind = Kind::Source;
        t.source = code;
        return t;
    }
    const int c = code - ld::kUsbTapStripBase;
    const int kind = c / kTapKindSpan;
    if (kind >= kNumStripKinds)
        return t;
    t.kind = Kind::Strip;
    t.stripKind = static_cast<StripKind> (kind);
    t.stripNumber = (c % kTapKindSpan) / 2 + 1;
    t.channel = c % 2;
    return t;
}

//==============================================================================
UsbRecorder::UsbRecorder (ld::Engine* e)
    : engine (e), storage (defaultStorageFolder()), recPath (storage), browserFolder (storage)
{
}

UsbRecorder::~UsbRecorder() = default;

juce::File UsbRecorder::defaultStorageFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Live DAW").getChildFile ("USB");
}

bool UsbRecorder::isInsideStorage (const juce::File& f) const
{
    return f == storage || f.isAChildOf (storage);
}

void UsbRecorder::setStorageFolder (const juce::File& folder)
{
    if (folder == juce::File())
        return;
    storage = folder;
    storage.createDirectory();
    if (! isInsideStorage (recPath) || ! recPath.isDirectory())
        recPath = storage;
    if (! isInsideStorage (browserFolder) || ! browserFolder.isDirectory())
        setBrowserFolder (storage);
}

std::vector<UsbRecorder::Item> UsbRecorder::listFolder (const juce::File& folder)
{
    std::vector<Item> items;
    for (const auto& entry : juce::RangedDirectoryIterator (folder, false, "*", juce::File::findFilesAndDirectories))
    {
        const auto& f = entry.getFile();
        if (f.getFileName().startsWithChar ('.') || entry.isHidden())
            continue;
        if (entry.isDirectory())
            items.push_back ({ f, ItemKind::Folder, entry.getModificationTime() });
        else if (isWav (f))
            items.push_back ({ f, ItemKind::Wav, entry.getModificationTime() });
        else if (isPlaylist (f))
            items.push_back ({ f, ItemKind::Playlist, entry.getModificationTime() });
    }
    std::sort (items.begin(), items.end(), [] (const Item& a, const Item& b)
    {
        if ((a.kind == ItemKind::Folder) != (b.kind == ItemKind::Folder))
            return a.kind == ItemKind::Folder;
        return a.file.getFileName().compareNatural (b.file.getFileName()) < 0;
    });
    return items;
}

juce::Result UsbRecorder::createFolder (const juce::File& parent, const juce::String& name)
{
    const auto clean = juce::File::createLegalFileName (name.trim());
    if (clean.isEmpty())
        return juce::Result::fail ("The folder needs a name");
    const auto folder = parent.getChildFile (clean);
    if (folder.exists())
        return juce::Result::fail ("\"" + clean + "\" already exists");
    auto r = folder.createDirectory();
    refreshQueue();
    return r;
}

juce::Result UsbRecorder::rename (const juce::File& item, const juce::String& newName, juce::File* renamed)
{
    auto clean = juce::File::createLegalFileName (newName.trim());
    if (clean.isEmpty())
        return juce::Result::fail ("The name is empty");
    if (! item.isDirectory() && item.getFileExtension().isNotEmpty() && ! clean.endsWithIgnoreCase (item.getFileExtension()))
        clean += item.getFileExtension();
    const auto target = item.getSiblingFile (clean);
    if (target == item)
        return juce::Result::ok();
    if (target.exists())
        return juce::Result::fail ("\"" + clean + "\" already exists");
    if (! item.moveFileTo (target))
        return juce::Result::fail ("Can't rename " + item.getFileName());
    if (renamed != nullptr)
        *renamed = target;
    if (recPath == item)
        recPath = target;
    refreshQueue();
    return juce::Result::ok();
}

juce::Result UsbRecorder::deleteItem (const juce::File& item)
{
    if (item == storage)
        return juce::Result::fail ("The USB folder itself can't be deleted");
    if (item == getRecFile() && isRecording())
        return juce::Result::fail ("The file is being recorded");
    const bool ok = item.isDirectory() ? item.deleteRecursively() : item.deleteFile();
    if (! ok)
        return juce::Result::fail ("Can't delete " + item.getFileName());
    if (recPath == item || recPath.isAChildOf (item))
        recPath = item.getParentDirectory();
    refreshQueue();
    return juce::Result::ok();
}

//==============================================================================
UsbRecorder::RecState UsbRecorder::getRecState() const { return static_cast<RecState> (ld::ld_usb_rec_state (engine)); }

juce::File UsbRecorder::nextRecordingFile (juce::Time when) const
{
    const auto base = "R_" + when.formatted ("%Y%m%d-%H%M%S");
    auto folder = recPath.isDirectory() ? recPath : storage;
    auto f = folder.getChildFile (base + ".wav");
    for (int n = 2; f.exists(); ++n)
        f = folder.getChildFile (base + "-" + juce::String (n) + ".wav");
    return f;
}

juce::Result UsbRecorder::record()
{
    if (getRecState() == RecState::Paused)
    {
        ld::ld_usb_rec_start (engine, "", 0, recChannels, recBits, nullptr);
        return juce::Result::ok();
    }
    if (isRecording())
        return juce::Result::ok();
    if (! recPath.isDirectory())
        recPath.createDirectory();
    const auto file = nextRecordingFile();
    const Utf8 path (file.getFullPathName());
    const int channels = fullSpeed ? 2 : recChannels;
    const int bits = fullSpeed ? 16 : recBits;
    ld::LdString* error = nullptr;
    if (! ld::ld_usb_rec_start (engine, path.data, path.size, channels, bits, &error))
        return failed (error, "Recording failed");
    refreshQueue();
    return juce::Result::ok();
}

void UsbRecorder::toggleRecPause()
{
    switch (getRecState())
    {
        case RecState::Recording: ld::ld_usb_rec_pause (engine); break;
        case RecState::Paused: record(); break;
        case RecState::Stopped:
        case RecState::Error: break;
    }
}

void UsbRecorder::stopRecording()
{
    ld::ld_usb_rec_stop (engine);
}

juce::Result UsbRecorder::newFile()
{
    if (! isRecording())
        return juce::Result::fail ("Not recording");
    const auto file = nextRecordingFile();
    const Utf8 path (file.getFullPathName());
    ld::LdString* error = nullptr;
    if (! ld::ld_usb_rec_new_file (engine, path.data, path.size, &error))
        return failed (error, "NEW FILE failed");
    refreshQueue();
    return juce::Result::ok();
}

juce::File UsbRecorder::getRecFile() const { return fileOf (ld::ld_usb_rec_file (engine)); }
int64_t UsbRecorder::getRecTime() const { return ld::ld_usb_rec_time (engine); }
int64_t UsbRecorder::getRecTotal() const { return ld::ld_usb_rec_total (engine); }

float UsbRecorder::getRecLevel (int port)
{
    return juce::Decibels::gainToDecibels (ld::ld_usb_rec_level (engine, port), -100.0f);
}

UsbRecorder::Tap UsbRecorder::getTap (int port) const { return Tap::fromCode (ld::ld_usb_rec_tap (engine, port)); }

void UsbRecorder::setTap (int port, const Tap& tap)
{
    if (juce::isPositiveAndBelow (port, kNumChannels))
        ld::ld_usb_set_rec_tap (engine, port, tap.code());
}

//==============================================================================
UsbRecorder::PlayState UsbRecorder::getPlayState() const { return static_cast<PlayState> (ld::ld_usb_play_state (engine)); }

void UsbRecorder::play() { ld::ld_usb_play (engine); }
void UsbRecorder::pause() { ld::ld_usb_pause (engine); }
void UsbRecorder::stop() { ld::ld_usb_stop (engine); }

void UsbRecorder::togglePlayPause()
{
    if (getPlayState() == PlayState::Playing)
        pause();
    else if (getPlayState() == PlayState::Paused)
        play();
}

void UsbRecorder::playEntry (uint64_t entry, bool start) { ld::ld_usb_load (engine, entry, 0, start); }

void UsbRecorder::playFile (const juce::File& file)
{
    uint64_t id = 0;
    for (const auto& e : getQueue())
        if (e.file == file)
        {
            id = e.id;
            break;
        }
    if (id == 0)
    {
        // Not in the queue (another folder): it becomes the browser's folder.
        setBrowserFolder (file.getParentDirectory());
        for (const auto& e : getQueue())
            if (e.file == file)
                id = e.id;
    }
    if (id == 0)
        return;
    if (id == getCurrentEntry() && getPlayState() != PlayState::Stopped && getPlayState() != PlayState::Error)
        play();
    else
        playEntry (id, true);
}

void UsbRecorder::seek (int64_t position) { ld::ld_usb_seek (engine, juce::jmax<int64_t> (0, position)); }
bool UsbRecorder::next() { return ld::ld_usb_next (engine); }
bool UsbRecorder::previous() { return ld::ld_usb_prev (engine); }

uint64_t UsbRecorder::getCurrentEntry() const { return ld::ld_usb_play_entry (engine); }

juce::File UsbRecorder::getCurrentFile() const
{
    const auto id = getCurrentEntry();
    if (const int index = ld::ld_usb_queue_index_of (engine, id); index >= 0)
        return fileOf (ld::ld_usb_queue_path (engine, index));
    return fileOf (ld::ld_usb_play_path (engine)); // a clip of a folder no longer browsed
}

int64_t UsbRecorder::getPosition() const { return ld::ld_usb_play_position (engine); }
int64_t UsbRecorder::getLength() const { return ld::ld_usb_play_length (engine); }
double UsbRecorder::getSampleRate() const { return ld::ld_engine_sample_rate (engine); }

float UsbRecorder::getPlayLevel (int ch)
{
    return juce::Decibels::gainToDecibels (ld::ld_usb_play_level (engine, ch), -100.0f);
}

bool UsbRecorder::readWavInfo (const juce::File& file, FileInfo& info)
{
    const Utf8 path (file.getFullPathName());
    int32_t channels = 0, rate = 0, bits = 0;
    int64_t frames = 0;
    if (! ld::ld_usb_wav_info (path.data, path.size, &channels, &rate, &bits, &frames))
        return false;
    info = { channels, rate, bits, frames };
    return true;
}

bool UsbRecorder::getCurrentInfo (FileInfo& info, int64_t& length, bool& rateMismatch) const
{
    int32_t channels = 0, rate = 0, bits = 0;
    bool mismatch = false;
    int64_t len = 0;
    if (! ld::ld_usb_play_info (engine, getCurrentEntry(), &channels, &rate, &bits, &len, &mismatch))
        return false;
    info = { channels, rate, bits, 0 };
    length = len;
    rateMismatch = mismatch;
    return true;
}

//==============================================================================
std::vector<UsbRecorder::Entry> UsbRecorder::getQueue() const
{
    std::vector<Entry> list;
    const int n = ld::ld_usb_queue_len (engine);
    list.reserve ((size_t) juce::jmax (0, n));
    for (int i = 0; i < n; ++i)
    {
        Entry e;
        if (! ld::ld_usb_queue_entry (engine, i, &e.id, &e.skip, &e.stop))
            break;
        e.file = fileOf (ld::ld_usb_queue_path (engine, i));
        list.push_back (e);
    }
    return list;
}

uint64_t UsbRecorder::getQueueVersion() const { return ld::ld_usb_queue_version (engine); }
int UsbRecorder::indexOf (uint64_t entry) const { return ld::ld_usb_queue_index_of (engine, entry); }

void UsbRecorder::setBrowserFolder (const juce::File& folder)
{
    browserFolder = folder;
    const Utf8 path (folder.getFullPathName());
    ld::ld_usb_queue_set_folder (engine, path.data, path.size);
}

void UsbRecorder::refreshQueue() { ld::ld_usb_queue_refresh (engine); }

bool UsbRecorder::isPlaylistOpen() const { return ld::ld_usb_queue_kind (engine) == 1; }
juce::File UsbRecorder::getPlaylistFile() const { return fileOf (ld::ld_usb_queue_playlist_file (engine)); }
bool UsbRecorder::isPlaylistDirty() const { return ld::ld_usb_queue_dirty (engine); }
void UsbRecorder::createPlaylist() { ld::ld_usb_playlist_new (engine); }

juce::Result UsbRecorder::openPlaylist (const juce::File& file)
{
    const Utf8 path (file.getFullPathName());
    ld::LdString* error = nullptr;
    if (! ld::ld_usb_playlist_open (engine, path.data, path.size, &error))
        return failed (error, "Can't open the playlist");
    return juce::Result::ok();
}

juce::Result UsbRecorder::savePlaylist (const juce::File& file)
{
    const auto target = file.hasFileExtension ("m3u") ? file : file.withFileExtension ("m3u");
    const Utf8 path (target.getFullPathName());
    ld::LdString* error = nullptr;
    if (! ld::ld_usb_playlist_save (engine, path.data, path.size, &error))
        return failed (error, "Can't save the playlist");
    return juce::Result::ok();
}

void UsbRecorder::closePlaylist()
{
    // Back to the browser folder's files.
    const Utf8 path (browserFolder.getFullPathName());
    ld::ld_usb_queue_set_folder (engine, path.data, path.size); // the folder to return to
    ld::ld_usb_playlist_close (engine);
}

uint64_t UsbRecorder::add (const juce::File& file)
{
    const Utf8 path (file.getFullPathName());
    return ld::ld_usb_queue_add (engine, path.data, path.size);
}

int UsbRecorder::addAll (const juce::File& folder)
{
    if (! isPlaylistOpen())
        createPlaylist();
    const Utf8 path (folder.getFullPathName());
    return ld::ld_usb_queue_add_all (engine, path.data, path.size);
}

void UsbRecorder::remove (int index) { ld::ld_usb_queue_remove (engine, index); }
int UsbRecorder::move (int index, int delta) { return ld::ld_usb_queue_move (engine, index, delta); }
void UsbRecorder::setSkip (int index, bool skip) { ld::ld_usb_queue_set_skip (engine, index, skip); }
void UsbRecorder::setStop (int index, bool stop) { ld::ld_usb_queue_set_stop (engine, index, stop); }
void UsbRecorder::setStopAll (bool stop) { ld::ld_usb_queue_set_stop_all (engine, stop); }
bool UsbRecorder::getRepeat() const { return ld::ld_usb_queue_repeat (engine); }
void UsbRecorder::setRepeat (bool repeat) { ld::ld_usb_queue_set_repeat (engine, repeat); }

//==============================================================================
void UsbRecorder::loadSettings (juce::PropertiesFile& settings)
{
    const auto stored = settings.getValue (kStorageKey);
    storage = stored.isNotEmpty() && juce::File::isAbsolutePath (stored) ? juce::File (stored) : defaultStorageFolder();
    storage.createDirectory();
    const auto rec = settings.getValue (kRecPathKey);
    recPath = rec.isNotEmpty() && juce::File::isAbsolutePath (rec) && juce::File (rec).isDirectory() ? juce::File (rec) : storage;
    const auto browse = settings.getValue (kBrowserKey);
    setBrowserFolder (browse.isNotEmpty() && juce::File::isAbsolutePath (browse) && juce::File (browse).isDirectory() ? juce::File (browse) : storage);
    setRecChannels (settings.getIntValue (kChannelsKey, 2));
    setRecBits (settings.getIntValue (kBitsKey, 24));
    fullSpeed = settings.getBoolValue (kFullSpeedKey, false);
    for (int port = 0; port < kNumChannels; ++port)
    {
        const auto key = kTapKey + juce::String (port + 1);
        if (settings.containsKey (key))
            ld::ld_usb_set_rec_tap (engine, port, settings.getIntValue (key, -1));
    }
}

void UsbRecorder::saveSettings (juce::PropertiesFile& settings) const
{
    settings.setValue (kStorageKey, storage.getFullPathName());
    settings.setValue (kRecPathKey, recPath.getFullPathName());
    settings.setValue (kBrowserKey, browserFolder.getFullPathName());
    settings.setValue (kChannelsKey, recChannels);
    settings.setValue (kBitsKey, recBits);
    settings.setValue (kFullSpeedKey, fullSpeed);
    for (int port = 0; port < kNumChannels; ++port)
        settings.setValue (kTapKey + juce::String (port + 1), ld::ld_usb_rec_tap (engine, port));
}

int UsbRecorder::getDropouts() const { return ld::ld_usb_dropouts (engine); }
bool UsbRecorder::waitForDisk (int timeoutMs) { return ld::ld_usb_wait_for_disk (engine, timeoutMs); }

} // namespace livedaw
