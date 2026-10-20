#include "engine/WingRemote.h"

#include "engine/MixerEngine.h"
#include "engine/RustStrings.h"
#include "engine/Show.h"

namespace livedaw
{

WingRemote::WingRemote (MixerEngine& e) : engine (e), handle (ld::ld_wing_create (e.getHandle()))
{
}

WingRemote::~WingRemote()
{
    stopTimer();
    ld::ld_wing_destroy (handle);
}

//==============================================================================
WingRemote::Settings WingRemote::Settings::load (juce::PropertiesFile& p)
{
    Settings s;
    s.enabled = p.getBoolValue ("wing.enabled", s.enabled);
    s.name = p.getValue (nameKey, s.name).trim();
    if (s.name.isEmpty())
        s.name = "LIVE-DAW";
    auto port = [&p] (const char* key, int fallback)
    {
        const int v = p.getIntValue (key, fallback);
        return v > 0 && v < 65536 ? v : fallback;
    };
    s.discoveryPort = port ("wing.discoveryPort", s.discoveryPort);
    s.oscPort = port ("wing.oscPort", s.oscPort);
    s.nativePort = port ("wing.nativePort", s.nativePort);
    s.discovery = p.getBoolValue ("wing.discovery", s.discovery);
    s.osc = p.getBoolValue ("wing.osc", s.osc);
    s.native = p.getBoolValue ("wing.native", s.native);
    return s;
}

void WingRemote::Settings::save (juce::PropertiesFile& p) const
{
    p.setValue ("wing.enabled", enabled);
    p.setValue (nameKey, name);
    p.setValue ("wing.discoveryPort", discoveryPort);
    p.setValue ("wing.oscPort", oscPort);
    p.setValue ("wing.nativePort", nativePort);
    p.setValue ("wing.discovery", discovery);
    p.setValue ("wing.osc", osc);
    p.setValue ("wing.native", native);
}

bool WingRemote::Settings::operator== (const Settings& o) const
{
    return enabled == o.enabled && name == o.name && discoveryPort == o.discoveryPort && oscPort == o.oscPort
           && nativePort == o.nativePort && discovery == o.discovery && osc == o.osc && native == o.native;
}

//==============================================================================
juce::StringArray WingRemote::apply (const Settings& s)
{
    if (! s.enabled)
    {
        stop();
        return {};
    }

    auto sameServers = s;
    sameServers.name = running.name;
    if (isRunning() && sameServers == running)
    {
        if (s.name != running.name)
            setName (s.name);
        running = s;
        return getStatus().errors;
    }

    const Utf8 name (s.name);
    const auto enable = (juce::uint32) ((s.discovery ? 1 : 0) | (s.osc ? 2 : 0) | (s.native ? 4 : 0));
    ld::ld_wing_start (handle, name.data, name.size, s.discoveryPort, s.oscPort, s.nativePort, enable);
    running = s;
    return getStatus().errors;
}

void WingRemote::stop()
{
    ld::ld_wing_stop (handle);
    running.enabled = false;
}

bool WingRemote::isRunning() const
{
    return ld::ld_wing_running (handle);
}

void WingRemote::setName (const juce::String& newName)
{
    const Utf8 u (newName);
    ld::ld_wing_set_name (handle, u.data, u.size);
    running.name = newName;
}

WingRemote::Status WingRemote::getStatus() const
{
    Status st;
    st.running = isRunning();
    st.discoveryPort = ld::ld_wing_port (handle, 0);
    st.oscPort = ld::ld_wing_port (handle, 1);
    st.nativePort = ld::ld_wing_port (handle, 2);

    juce::StringArray lines;
    lines.addLines (takeString (ld::ld_wing_status (handle)));
    for (auto& line : lines)
    {
        juce::StringArray f;
        f.addTokens (line, "\t", {});
        if (f[0] == "error" && f.size() >= 2)
            st.errors.add (f[1]);
        else if (f[0] == "client" && f.size() >= 5)
            st.clients.push_back ({ f[1] == "native", f[2], f[3] == "1", f[4].getIntValue() });
        else if (f[0] == "traffic" && f.size() >= 3)
        {
            st.oscMessages = f[1].getLargeIntValue();
            st.nativeBytes = f[2].getLargeIntValue();
        }
    }
    return st;
}

//==============================================================================
juce::Result WingRemote::importSnapshot (const juce::File& file, juce::String& report)
{
    const Utf8 path (file.getFullPathName());
    ld::LdString* message = nullptr;
    const bool ok = ld::ld_wing_import_snap (handle, path.data, path.size, &message);
    report = takeString (message);
    engine.syncStrips(); // channels may have been added
    return ok ? juce::Result::ok() : juce::Result::fail (report);
}

juce::Result WingRemote::exportSnapshot (const juce::File& file)
{
    // The default folder (Documents) may not exist yet on a fresh system.
    if (auto dir = file.getParentDirectory().createDirectory(); dir.failed())
        return dir;
    const Utf8 path (file.getFullPathName());
    ld::LdString* message = nullptr;
    if (ld::ld_wing_export_snap (handle, path.data, path.size, &message))
        return juce::Result::ok();
    return juce::Result::fail (takeString (message));
}

juce::Result WingRemote::importShow (Show& show, const juce::File& file, juce::String& report)
{
    return show.importWingShow (handle, file, report);
}

//==============================================================================
void WingRemote::attachShow (Show* s, std::function<void (const juce::String&, const state::ApplyResult&)> recalled)
{
    libraryShow = s;
    onRecalled = std::move (recalled);
    if (libraryShow == nullptr)
    {
        stopTimer();
        ld::ld_wing_clear_library (handle);
        ld::ld_wing_clear_safes (handle);
        ld::ld_wing_clear_usb (handle);
        return;
    }
    publishLibrary();
    pumpSafes();
    pumpUsb();
    startTimerHz (10);
}

void WingRemote::timerCallback()
{
    if (isRunning())
        pump();
}

void WingRemote::pump()
{
    if (libraryShow == nullptr)
        return;
    pumpSafes();
    pumpUsb();
    int32_t action = 0, index = 0;
    while (ld::ld_wing_take_library_action (handle, &action, &index))
    {
        juce::String what;
        state::ApplyResult result;
        switch (action)
        {
            case 1:
                if (! juce::isPositiveAndBelow (index, libraryShow->getNumScenes()))
                    continue;
                what = "scene " + juce::String (index + 1);
                result = libraryShow->recallScene (index);
                break;
            case 2:
                what = "next scene";
                result = libraryShow->recallNext();
                break;
            case 3:
                what = "previous scene";
                result = libraryShow->recallPrevious();
                break;
            case 4:
                if (libraryShow->findSceneByTag (index) < 0)
                    continue;
                what = "scene #" + juce::String (index);
                result = libraryShow->recallSceneByTag (index);
                break;
            default: continue;
        }
        if (onRecalled)
            onRecalled (what + " (WING remote)", result);
    }
    publishLibrary();
}

void WingRemote::publishLibrary()
{
    const int current = libraryShow->getCurrentSceneIndex();
    const auto scene = libraryShow->getScene (current);
    const auto showName = libraryShow->getName();
    const auto sceneName = scene ? scene->name : juce::String();
    const Utf8 name (showName), sceneUtf8 (sceneName);
    ld::ld_wing_set_library (handle, name.data, name.size, libraryShow->getNumScenes(), current, sceneUtf8.data, sceneUtf8.size,
                             scene ? scene->tag : 0);
}

void WingRemote::pumpSafes()
{
    juce::uint32 dcas = 0, muteGroups = 0, fx = 0;
    auto safe = libraryShow->getGlobalSafe();
    if (ld::ld_wing_take_safes (handle, &dcas, &muteGroups, &fx))
    {
        safe.dcas = dcas;
        safe.muteGroups = muteGroups;
        safe.fx = fx;
        libraryShow->setGlobalSafe (safe);
    }
    ld::ld_wing_set_safes (handle, safe.dcas, safe.muteGroups, safe.fx);
}

void WingRemote::pumpUsb()
{
    auto& usb = engine.getUsbRecorder();
    const auto storage = usb.getStorageFolder();
    // Clients name files relative to the USB folder ("/" separated); nothing outside it.
    auto inStorage = [&] (const juce::String& relative)
    {
        const auto f = storage.getChildFile (relative.trimCharactersAtStart ("/\\"));
        return usb.isInsideStorage (f) ? f : juce::File();
    };

    int32_t channels = 0, bits = 0;
    ld::LdString* recPath = nullptr;
    if (ld::ld_wing_take_usb_settings (handle, &channels, &bits, &recPath))
    {
        usb.setRecChannels (channels);
        usb.setRecBits (bits);
        if (const auto folder = inStorage (takeString (recPath)); folder != juce::File())
            usb.setRecPath (folder);
    }

    int32_t action = 0;
    ld::LdString* file = nullptr;
    while (ld::ld_wing_take_usb_action (handle, &action, &file))
    {
        const auto name = takeString (file);
        switch (action)
        {
            case 1: usb.record(); break;
            case 2: usb.newFile(); break;
            case 3:
                if (const auto f = inStorage (name); f.existsAsFile())
                    usb.playFile (f);
                break;
            default: break;
        }
    }

    const auto rec = usb.getRecPath();
    const auto relative = rec == storage || ! usb.isInsideStorage (rec) ? juce::String() : rec.getRelativePathFrom (storage).replaceCharacter ('\\', '/');
    const Utf8 storageUtf8 (storage.getFullPathName()), recUtf8 (relative);
    ld::ld_wing_set_usb (handle, storageUtf8.data, storageUtf8.size, recUtf8.data, recUtf8.size, usb.getRecChannels(), usb.getRecBits());
}

juce::String WingRemote::coverageReport()
{
    return takeString (ld::ld_wing_coverage());
}

} // namespace livedaw
