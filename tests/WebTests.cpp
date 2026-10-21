#include "TestHelpers.h"
#include "engine/WebRemote.h"

#include <cstring>

using namespace livedaw;

namespace
{
juce::File tempDir (const juce::String& name)
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("livedaw-web-tests-" + name + "-" + juce::String (juce::Time::currentTimeMillis()));
    dir.deleteRecursively();
    dir.createDirectory();
    return dir;
}

/** Sends `request` to the server and returns all it answers. */
juce::String http (int port, const juce::String& request)
{
    juce::StreamingSocket s;
    if (! s.connect ("127.0.0.1", port, 2000))
        return {};
    s.write (request.toRawUTF8(), (int) request.getNumBytesAsUTF8());
    juce::MemoryOutputStream out;
    char buf[4096];
    for (int n; s.waitUntilReady (true, 2000) == 1 && (n = s.read (buf, sizeof (buf), false)) > 0;)
        out.write (buf, (size_t) n);
    return out.toString();
}

/** A browser's side of the WebSocket. */
struct Browser
{
    juce::StreamingSocket socket;

    bool connect (int port)
    {
        if (! socket.connect ("127.0.0.1", port, 2000))
            return false;
        const juce::String request ("GET /ws HTTP/1.1\r\nHost: 127.0.0.1:" + juce::String (port)
                                    + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                      "Sec-WebSocket-Version: 13\r\n\r\n");
        socket.write (request.toRawUTF8(), (int) request.getNumBytesAsUTF8());
        juce::String head;
        char c;
        while (! head.endsWith ("\r\n\r\n") && readExactly (&c, 1))
            head += c;
        return head.startsWith ("HTTP/1.1 101") && head.contains ("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    }

    bool readExactly (void* dest, int n)
    {
        return socket.waitUntilReady (true, 3000) == 1 && socket.read (dest, n, true) == n;
    }

    /** The next message (opcode, payload); opcode -1 on a timeout. */
    std::pair<int, juce::MemoryBlock> read()
    {
        juce::uint8 h[2];
        if (! readExactly (h, 2))
            return { -1, {} };
        juce::uint64 len = h[1] & 0x7f;
        if (len == 126 || len == 127)
        {
            juce::uint8 ext[8];
            const int n = len == 126 ? 2 : 8;
            if (! readExactly (ext, n))
                return { -1, {} };
            len = 0;
            for (int i = 0; i < n; ++i)
                len = (len << 8) | ext[i];
        }
        juce::MemoryBlock payload ((size_t) len);
        if (len > 0 && ! readExactly (payload.getData(), (int) len))
            return { -1, {} };
        return { h[0] & 0x0f, payload };
    }

    void send (int opcode, const void* data, size_t len)
    {
        juce::MemoryBlock frame;
        const juce::uint8 head[] { (juce::uint8) (0x80 | opcode), (juce::uint8) (0x80 | len) };
        const juce::uint8 mask[] { 1, 2, 3, 4 };
        frame.append (head, 2);
        frame.append (mask, 4);
        for (size_t i = 0; i < len; ++i)
        {
            const auto b = (juce::uint8) (static_cast<const juce::uint8*> (data)[i] ^ mask[i % 4]);
            frame.append (&b, 1);
        }
        socket.write (frame.getData(), (int) frame.getSize());
    }
};
} // namespace

class WebTests final : public juce::UnitTest
{
public:
    WebTests() : juce::UnitTest ("Web remote", "LiveDAW") {}

    void runTest() override
    {
        beginTest ("Settings persist in the app settings");
        {
            auto dir = tempDir ("settings");
            juce::PropertiesFile::Options o;
            o.applicationName = "webtest";
            juce::PropertiesFile props (dir.getChildFile ("t.settings"), o);
            auto s = WebRemote::Settings::load (props);
            expect (! s.enabled && s.port == 8080 && s.pin.isEmpty() && ! s.viewOnly);
            s.enabled = true;
            s.port = 9090;
            s.pin = "2468";
            s.viewOnly = true;
            s.save (props);
            expect (WebRemote::Settings::load (props) == s);
            props.setValue ("web.port", 70000); // out of range: the default
            props.setValue ("web.pin", "12a4"); // not a PIN: none
            expectEquals (WebRemote::Settings::load (props).port, 8080);
            expect (WebRemote::Settings::load (props).pin.isEmpty());
            expect (WebRemote::Settings::isValidPin ("") && WebRemote::Settings::isValidPin ("12345678"));
            expect (! WebRemote::Settings::isValidPin ("123") && ! WebRemote::Settings::isValidPin ("123456789"));
            dir.deleteRecursively();
        }

        beginTest ("The server serves the page and nothing else");
        {
            WebRemote web;
            WebRemote::Settings s;
            s.enabled = true;
            s.port = 0; // a free port
            expect (web.apply (s).isEmpty());
            const auto st = web.getStatus();
            expect (st.running && st.port > 0 && st.clients.empty() && ! web.isWatched());

            const auto page = http (st.port, "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
            expect (page.startsWith ("HTTP/1.1 200 OK") && page.contains ("<title>Sidecar</title>"), page.substring (0, 80));
            expect (http (st.port, "GET /etc/passwd HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").startsWith ("HTTP/1.1 404"));
            expect (http (st.port, "GET / HTTP/1.1\r\nHost: evil.example\r\n\r\n").startsWith ("HTTP/1.1 403"));

            // A port in use is reported, not fatal.
            WebRemote other;
            WebRemote::Settings taken = s;
            taken.port = st.port;
            expect (other.apply (taken).isNotEmpty());
            expect (other.isRunning() && other.getStatus().port == -1);

            expect (WebRemote::getUrls (st.port).joinIntoString (" ").contains ("127.0.0.1:" + juce::String (st.port)));
            s.enabled = false;
            web.apply (s);
            expect (! web.isRunning() && web.getStatus().port == -1);
        }

        beginTest ("Changed tiles reach a browser; its input comes back");
        {
            WebRemote web;
            WebRemote::Settings s;
            s.enabled = true;
            s.port = 0;
            s.pin = "4711";
            web.apply (s);
            const int port = web.getStatus().port;

            Browser b;
            expect (b.connect (port));
            auto [op, hello] = b.read();
            expect (op == 1 && hello.toString().contains ("\"pin\":true"));
            expect (! web.isWatched());
            b.send (1, "pin:4711", 8);
            expect (b.read().second.toString() == "{\"t\":\"ok\"}");
            expect (web.isWatched());

            // A 100 x 70 screen in 64 px tiles: 2 x 2.
            juce::Image screen (juce::Image::RGB, 100, 70, true, juce::SoftwareImageType());
            const auto generation = web.resetScreen (100, 70, 64);
            const std::vector<juce::uint32> all { 0, 1, 2, 3 };
            {
                const juce::Image::BitmapData px (screen, juce::Image::BitmapData::readOnly);
                expectEquals ((int) web.changedTiles (px, all).size(), 4);
                expect (web.changedTiles (px, all).empty());
            }
            screen.setPixelAt (90, 66, juce::Colours::red);
            {
                const juce::Image::BitmapData px (screen, juce::Image::BitmapData::readOnly);
                const auto changed = web.changedTiles (px, all);
                expect (changed.size() == 1 && changed[0] == 3);
            }
            const juce::MemoryBlock tile ("TILE", 4);
            expect (web.putTile (generation, 3, WebRemote::png, tile));
            expect (! web.putTile (generation + 1, 3, WebRemote::png, tile));
            web.publish();

            // The first frame (the size) may come before the tile.
            juce::MemoryBlock frame;
            auto endsWithTile = [&frame] { return frame.getSize() > 13 && std::memcmp (frame.begin() + frame.getSize() - 4, "TILE", 4) == 0; };
            for (int i = 0; i < 3 && ! endsWithTile(); ++i)
            {
                auto [type, m] = b.read();
                expectEquals (type, 2);
                frame = m;
                const juce::uint8 ack[] { 4, 0, 0, 0, 0 };
                b.send (2, ack, sizeof (ack));
            }
            expect (endsWithTile() && frame[0] == 1);
            if (frame.getSize() > 13)
                expectEquals ((int) (juce::uint8) frame[5] | ((int) (juce::uint8) frame[6] << 8), 100);

            juce::uint8 down[12] { 1, 1, 1, 0 };
            const float x = 0.5f, y = 0.25f;
            std::memcpy (down + 4, &x, 4);
            std::memcpy (down + 8, &y, 4);
            b.send (2, down, sizeof (down));
            ld::LdWebInput e {};
            for (int i = 0; i < 200 && ! web.takeInput (e); ++i)
                juce::Thread::sleep (5);
            expect (e.kind == 1 && e.buttons == 1 && e.x == 0.5f && e.y == 0.25f);
            expectEquals (web.getStatus().numWatching(), 1);
        }
    }
};

static WebTests webTests;
