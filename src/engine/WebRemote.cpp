#include "engine/WebRemote.h"

#include "engine/RustStrings.h"

#include <algorithm>

namespace livedaw
{

WebRemote::WebRemote() : handle (ld::ld_web_create())
{
}

WebRemote::~WebRemote()
{
    ld::ld_web_destroy (handle);
}

//==============================================================================
bool WebRemote::Settings::isValidPin (const juce::String& pin)
{
    return pin.isEmpty() || (pin.length() >= 4 && pin.length() <= 8 && pin.containsOnly ("0123456789"));
}

WebRemote::Settings WebRemote::Settings::load (juce::PropertiesFile& p)
{
    Settings s;
    s.enabled = p.getBoolValue ("web.enabled", s.enabled);
    const int port = p.getIntValue ("web.port", s.port);
    if (port > 0 && port < 65536)
        s.port = port;
    const auto pin = p.getValue ("web.pin").trim();
    if (isValidPin (pin))
        s.pin = pin;
    s.viewOnly = p.getBoolValue ("web.viewOnly", s.viewOnly);
    return s;
}

void WebRemote::Settings::save (juce::PropertiesFile& p) const
{
    p.setValue ("web.enabled", enabled);
    p.setValue ("web.port", port);
    p.setValue ("web.pin", pin);
    p.setValue ("web.viewOnly", viewOnly);
}

bool WebRemote::Settings::operator== (const Settings& o) const
{
    return enabled == o.enabled && port == o.port && pin == o.pin && viewOnly == o.viewOnly;
}

int WebRemote::Status::numWatching() const
{
    return (int) std::count_if (clients.begin(), clients.end(), [] (const Client& c) { return c.watching; });
}

//==============================================================================
juce::String WebRemote::apply (const Settings& s)
{
    if (! s.enabled)
    {
        stop();
        return {};
    }
    if (isRunning() && s == running)
        return getStatus().error;

    const Utf8 pin (s.pin);
    ld::ld_web_start (handle, s.port, pin.data, pin.size, s.viewOnly);
    running = s;
    return getStatus().error;
}

void WebRemote::stop()
{
    ld::ld_web_stop (handle);
    running.enabled = false;
}

bool WebRemote::isRunning() const
{
    return ld::ld_web_running (handle);
}

WebRemote::Status WebRemote::getStatus() const
{
    Status st;
    st.running = isRunning();
    st.port = ld::ld_web_port (handle);

    juce::StringArray lines;
    lines.addLines (takeString (ld::ld_web_status (handle)));
    for (auto& line : lines)
    {
        juce::StringArray f;
        f.addTokens (line, "\t", {});
        if (f[0] == "error" && f.size() >= 2)
            st.error = f[1];
        else if (f[0] == "client" && f.size() >= 4)
            st.clients.push_back ({ f[1], f[2] == "1", f[3].getIntValue() });
        else if (f[0] == "traffic" && f.size() >= 3)
        {
            st.bytesSent = f[1].getLargeIntValue();
            st.framesSent = f[2].getLargeIntValue();
        }
    }
    return st;
}

juce::StringArray WebRemote::getUrls (int port)
{
    juce::StringArray lan, loopback;
    for (auto& ip : juce::IPAddress::getAllAddresses (false))
    {
        const auto url = "http://" + ip.toString() + ":" + juce::String (port) + "/";
        (ip.address[0] == 127 ? loopback : lan).addIfNotAlreadyThere (url);
    }
    lan.addArray (loopback);
    return lan;
}

//==============================================================================
bool WebRemote::isWatched() const
{
    return ld::ld_web_watched (handle);
}

juce::uint32 WebRemote::resetScreen (int width, int height, int tileSize)
{
    return ld::ld_web_reset_screen (handle, width, height, tileSize);
}

std::vector<juce::uint32> WebRemote::changedTiles (const juce::Image::BitmapData& pixels, const std::vector<juce::uint32>& candidates)
{
    std::vector<juce::uint32> changed (candidates.size());
    const auto n = ld::ld_web_changed_tiles (handle, pixels.data, pixels.lineStride, pixels.height, pixels.pixelStride,
                                             candidates.data(), candidates.size(), changed.data());
    changed.resize (n);
    return changed;
}

bool WebRemote::putTile (juce::uint32 generation, juce::uint32 index, TileFormat format, const juce::MemoryBlock& data)
{
    return ld::ld_web_put_tile (handle, generation, index, (int32_t) format, static_cast<const uint8_t*> (data.getData()), data.getSize());
}

void WebRemote::publish()
{
    ld::ld_web_publish (handle);
}

bool WebRemote::takeInput (ld::LdWebInput& event)
{
    return ld::ld_web_take_input (handle, &event);
}

} // namespace livedaw
