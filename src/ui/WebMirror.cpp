#include "ui/WebMirror.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>

namespace livedaw::ui
{

namespace
{
    /** A PNG tile bigger than this (a photo-like picture) is tried as JPEG. */
    constexpr size_t kTryJpegAbove = 6 * 1024;
    constexpr float kJpegQuality = 0.85f;
    /** The mirror's pixels stay under about 2560 x 1440 on high-DPI screens. */
    constexpr float kMaxPixels = 2560.0f * 1440.0f;

    juce::ModifierKeys modifiers (juce::uint32 keys, int buttons)
    {
        int f = 0;
        f |= (keys & 1) != 0 ? juce::ModifierKeys::shiftModifier : 0;
        f |= (keys & 2) != 0 ? juce::ModifierKeys::ctrlModifier : 0;
        f |= (keys & 4) != 0 ? juce::ModifierKeys::altModifier : 0;
        // The browser's command / Windows key is the host's command key (Ctrl off macOS).
        f |= (keys & 8) != 0 ? juce::ModifierKeys::commandModifier : 0;
        f |= (buttons & 1) != 0 ? juce::ModifierKeys::leftButtonModifier : 0;
        f |= (buttons & 2) != 0 ? juce::ModifierKeys::rightButtonModifier : 0;
        f |= (buttons & 4) != 0 ? juce::ModifierKeys::middleButtonModifier : 0;
        return juce::ModifierKeys (f);
    }

    juce::KeyPress keyPress (int key, juce::ModifierKeys mods)
    {
        if (key > 0)
        {
            // Shortcuts (Ctrl+S) carry no text.
            const bool shortcut = mods.isCtrlDown() || mods.isCommandDown() || mods.isAltDown();
            return { key, mods, shortcut ? 0 : (juce::juce_wchar) key };
        }
        if (key <= WebRemote::keyF1 && key >= WebRemote::keyF12)
            return { juce::KeyPress::F1Key + (WebRemote::keyF1 - key), mods, 0 };

        switch (key)
        {
            case WebRemote::keyEnter: return { juce::KeyPress::returnKey, mods, 0 };
            case WebRemote::keyEscape: return { juce::KeyPress::escapeKey, mods, 0 };
            case WebRemote::keyBackspace: return { juce::KeyPress::backspaceKey, mods, 0 };
            case WebRemote::keyTab: return { juce::KeyPress::tabKey, mods, 0 };
            case WebRemote::keyDelete: return { juce::KeyPress::deleteKey, mods, 0 };
            case WebRemote::keyLeft: return { juce::KeyPress::leftKey, mods, 0 };
            case WebRemote::keyRight: return { juce::KeyPress::rightKey, mods, 0 };
            case WebRemote::keyUp: return { juce::KeyPress::upKey, mods, 0 };
            case WebRemote::keyDown: return { juce::KeyPress::downKey, mods, 0 };
            case WebRemote::keyHome: return { juce::KeyPress::homeKey, mods, 0 };
            case WebRemote::keyEnd: return { juce::KeyPress::endKey, mods, 0 };
            case WebRemote::keyPageUp: return { juce::KeyPress::pageUpKey, mods, 0 };
            case WebRemote::keyPageDown: return { juce::KeyPress::pageDownKey, mods, 0 };
            case WebRemote::keyInsert: return { juce::KeyPress::insertKey, mods, 0 };
            default: return {};
        }
    }
} // namespace

//==============================================================================
/** Installed on the tracked windows as their cached image: JUCE reports every
    repaint of a window (and of anything in it) here. Desktop windows never
    paint from a cached image, so it only listens. */
class WebMirror::RepaintTracker final : public juce::CachedComponentImage
{
public:
    RepaintTracker (WebMirror& m, juce::Component& c) : mirror (m), owner (c) {}

    void paint (juce::Graphics& g) override { owner.paintEntireComponent (g, true); }
    bool invalidateAll() override
    {
        mirror.invalidate (owner, owner.getLocalBounds());
        return true;
    }
    bool invalidate (const juce::Rectangle<int>& area) override
    {
        mirror.invalidate (owner, area);
        return true;
    }
    void releaseResources() override {}

private:
    WebMirror& mirror;
    juce::Component& owner;
};

//==============================================================================
/** Encodes the changed tiles and puts them into the server. Tiles waiting are
    replaced by newer copies, so a slow encoder skips states, never queues. */
class WebMirror::Encoder final : private juce::Thread
{
public:
    explicit Encoder (WebRemote& s) : juce::Thread ("Web remote encoder"), server (s)
    {
        jpeg.setQuality (kJpegQuality);
        startThread();
    }

    ~Encoder() override
    {
        signalThreadShouldExit();
        notify();
        stopThread (4000);
    }

    void add (juce::uint32 generation, juce::uint32 index, juce::Image tile)
    {
        const juce::ScopedLock sl (lock);
        if (generation != pendingGeneration)
        {
            pending.clear();
            pendingGeneration = generation;
        }
        pending[index] = std::move (tile);
    }

    void flush() { notify(); }

    /** Drops what is waiting and stops the batch being encoded after its
        current tile (before the server restarts, when nobody watches). */
    void clear()
    {
        cancel = true;
        const juce::ScopedLock busy (encoding);
        const juce::ScopedLock sl (lock);
        pending.clear();
        cancel = false;
    }

    /** Held while tiles go to the server. */
    juce::CriticalSection encoding;

    /** Milliseconds spent encoding since the last call. */
    double takeEncodeMs() { return encodeMicros.exchange (0) / 1000.0; }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            wait (-1);
            const juce::ScopedLock busy (encoding);
            std::map<juce::uint32, juce::Image> batch;
            juce::uint32 generation;
            {
                const juce::ScopedLock sl (lock);
                batch.swap (pending);
                generation = pendingGeneration;
            }
            auto published = juce::Time::getMillisecondCounterHiRes();
            for (auto& [index, tile] : batch)
            {
                if (threadShouldExit())
                    return;
                if (cancel)
                    break;
                const auto start = juce::Time::getMillisecondCounterHiRes();
                auto format = WebRemote::png;
                auto data = encode (png, tile);
                if (data.getSize() > kTryJpegAbove)
                {
                    auto smaller = encode (jpeg, tile);
                    if (smaller.getSize() * 2 < data.getSize())
                    {
                        data = std::move (smaller);
                        format = WebRemote::jpeg;
                    }
                }
                server.putTile (generation, index, format, data);
                const auto now = juce::Time::getMillisecondCounterHiRes();
                encodeMicros += (juce::int64) ((now - start) * 1000.0);
                // A big batch (a whole screen) shows up as it goes.
                if (now - published > 1000.0 / kMaxFps)
                {
                    server.publish();
                    published = now;
                }
            }
            if (! batch.empty())
                server.publish();
        }
    }

    static juce::MemoryBlock encode (juce::ImageFileFormat& format, const juce::Image& tile)
    {
        juce::MemoryOutputStream out;
        format.writeImageToStream (tile, out);
        return out.getMemoryBlock();
    }

    WebRemote& server;
    juce::PNGImageFormat png;
    juce::JPEGImageFormat jpeg;
    juce::CriticalSection lock;
    std::map<juce::uint32, juce::Image> pending;
    juce::uint32 pendingGeneration = 0;
    std::atomic<juce::int64> encodeMicros { 0 };
    std::atomic<bool> cancel { false };
};

//==============================================================================
WebMirror::WebMirror (juce::Component& c, juce::PropertiesFile& settings, EventLog& l)
    : content (c), log (l), encoder (std::make_unique<Encoder> (server))
{
    logStatsEnabled = juce::SystemStats::getEnvironmentVariable ("SIDECAR_WEB_STATS", {}).isNotEmpty();
    const auto s = WebRemote::Settings::load (settings);
    const auto error = apply (s);
    if (error.isNotEmpty())
        log.error ("Web remote: " + error);
    else if (s.enabled)
        log.info ("Web remote on port " + juce::String (s.port));
}

WebMirror::~WebMirror()
{
    stopTimer();
    detach();
    encoder.reset();
    server.stop();
}

juce::String WebMirror::apply (const WebRemote::Settings& s)
{
    detach();
    juce::String error;
    {
        // The encoder must not talk to the server while it restarts.
        const juce::ScopedLock busy (encoder->encoding);
        encoder->clear();
        error = server.apply (s);
    }
    if (server.isRunning())
        startTimerHz (4);
    else
        stopTimer();
    return error;
}

//==============================================================================
void WebMirror::timerCallback()
{
    const bool watched = server.isWatched();
    if (window != nullptr && window != content.getTopLevelComponent())
        detach(); // the content moved to another window
    if (watched != (window != nullptr))
    {
        if (watched)
            attach();
        else
            detach();
        // Input and frames need a quick timer, waiting for a browser does not.
        startTimerHz (window != nullptr ? 2 * kMaxFps : 4);
    }
    if (window == nullptr)
        return;

    playInput();
    updateOverlays();
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (! dirty.isEmpty() && now - lastFrame >= 1000.0 / kMaxFps)
    {
        lastFrame = now;
        capture();
    }
    if (logStatsEnabled)
        logStats();
}

void WebMirror::attach()
{
    auto* top = content.getTopLevelComponent();
    if (top == nullptr || ! top->isOnDesktop())
        return;
    window = top;
    track (*top);
    image = {}; // a new screen: the browsers get everything
    dirty = top->getLocalBounds();
    statsStart = juce::Time::getMillisecondCounterHiRes();
}

void WebMirror::detach()
{
    untrack (window);
    for (auto& o : overlays)
        untrack (o.component);
    overlays.clear();
    window = nullptr;
    pressTarget = nullptr;
    heldPress.reset();
    warped = false;
    image = {};
    dirty.clear();
    encoder->clear();
}

void WebMirror::track (juce::Component& c)
{
    // Another cached image (setBufferedToImage) would be lost: leave such a window out.
    if (c.getCachedComponentImage() == nullptr)
        c.setCachedComponentImage (new RepaintTracker (*this, c));
}

void WebMirror::untrack (juce::Component* c)
{
    if (c != nullptr && dynamic_cast<RepaintTracker*> (c->getCachedComponentImage()) != nullptr)
        c->setCachedComponentImage (nullptr);
}

void WebMirror::invalidate (juce::Component& c, juce::Rectangle<int> area)
{
    if (window == nullptr)
        return;
    if (&c != window.getComponent())
    {
        const auto it = std::find_if (overlays.begin(), overlays.end(), [&c] (const Overlay& o) { return o.component == &c; });
        if (it == overlays.end())
            return;
        area += it->bounds.getPosition();
    }
    dirty.add (area.getIntersection (window->getLocalBounds()));
}

void WebMirror::updateOverlays()
{
    // The desktop windows in front of the main window (the desktop keeps them
    // back to front) that overlap it, except other app windows (plugin editors).
    std::vector<Overlay> now;
    auto& desktop = juce::Desktop::getInstance();
    const auto origin = window->getScreenPosition();
    bool inFront = false;
    for (int i = 0; i < desktop.getNumComponents(); ++i)
    {
        auto* c = desktop.getComponent (i);
        if (c == window.getComponent())
        {
            inFront = true;
            continue;
        }
        if (! inFront || c == nullptr || ! c->isVisible() || dynamic_cast<juce::ResizableWindow*> (c) != nullptr)
            continue;
        const auto bounds = c->getScreenBounds() - origin;
        if (bounds.intersects (window->getLocalBounds()))
            now.push_back ({ c, bounds });
    }

    for (auto& o : overlays)
    {
        const auto it = std::find_if (now.begin(), now.end(), [&o] (const Overlay& n) { return n.component == o.component; });
        if (it == now.end() || it->bounds != o.bounds)
        {
            dirty.add (o.bounds.getIntersection (window->getLocalBounds()));
            if (it == now.end())
                untrack (o.component);
        }
    }
    for (auto& n : now)
    {
        const auto it = std::find_if (overlays.begin(), overlays.end(), [&n] (const Overlay& o) { return o.component == n.component; });
        if (it == overlays.end() || it->bounds != n.bounds)
            dirty.add (n.bounds.getIntersection (window->getLocalBounds()));
        if (it == overlays.end())
            track (*n.component);
    }
    overlays = std::move (now);
}

void WebMirror::capture()
{
    const auto start = juce::Time::getMillisecondCounterHiRes();
    const auto bounds = window->getLocalBounds();
    if (bounds.isEmpty())
        return;

    // The screen's own pixel density (capped), so the mirror is as sharp as the window.
    float s = 1.0f;
    if (auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (window->getScreenBounds()))
        s = (float) display->scale * juce::Desktop::getInstance().getGlobalScaleFactor();
    s = juce::jlimit (1.0f, 2.0f, s);
    s = juce::jmin (s, std::sqrt (kMaxPixels / (float) (bounds.getWidth() * bounds.getHeight())));
    s = juce::jmax (1.0f, s);
    const int w = juce::roundToInt ((float) bounds.getWidth() * s), h = juce::roundToInt ((float) bounds.getHeight() * s);

    if (! image.isValid() || image.getWidth() != w || image.getHeight() != h)
    {
        image = juce::Image (juce::Image::RGB, w, h, true, juce::SoftwareImageType());
        scale = s;
        generation = server.resetScreen (w, h, kTileSize);
        dirty = bounds;
    }

    {
        juce::Graphics g (image);
        g.addTransform (juce::AffineTransform::scale (scale));
        g.reduceClipRegion (dirty);
        g.fillAll (juce::Colours::black);
        {
            const juce::Graphics::ScopedSaveState ss (g);
            window->paintEntireComponent (g, true);
        }
        for (auto& o : overlays)
        {
            if (o.component == nullptr)
                continue;
            const juce::Graphics::ScopedSaveState ss (g);
            g.setOrigin (o.bounds.getPosition());
            if (g.reduceClipRegion (o.component->getLocalBounds()))
                o.component->paintEntireComponent (g, false);
        }
    }

    // The tiles the repainted areas touch, then those of them that changed.
    const int cols = (w + kTileSize - 1) / kTileSize, rows = (h + kTileSize - 1) / kTileSize;
    std::vector<bool> seen ((size_t) (cols * rows));
    std::vector<juce::uint32> candidates;
    for (auto r : dirty)
    {
        const auto px = (r.toFloat() * scale).getSmallestIntegerContainer().expanded (1).getIntersection (image.getBounds());
        if (px.isEmpty())
            continue;
        for (int ty = px.getY() / kTileSize; ty <= (px.getBottom() - 1) / kTileSize; ++ty)
            for (int tx = px.getX() / kTileSize; tx <= (px.getRight() - 1) / kTileSize; ++tx)
                if (const auto i = (size_t) (ty * cols + tx); ! seen[i])
                {
                    seen[i] = true;
                    candidates.push_back ((juce::uint32) i);
                }
    }
    dirty.clear();

    std::vector<juce::uint32> changed;
    {
        const juce::Image::BitmapData pixels (image, juce::Image::BitmapData::readOnly);
        changed = server.changedTiles (pixels, candidates);
    }
    for (auto i : changed)
    {
        const auto x = (int) i % cols * kTileSize, y = (int) i / cols * kTileSize;
        const juce::Rectangle<int> r (x, y, juce::jmin (kTileSize, w - x), juce::jmin (kTileSize, h - y));
        encoder->add (generation, i, image.getClippedImage (r).createCopy());
    }
    if (! changed.empty())
        encoder->flush();

    const auto ms = juce::Time::getMillisecondCounterHiRes() - start;
    paintMs += ms;
    paintMaxMs = juce::jmax (paintMaxMs, ms);
    ++frames;
    tiles += (int) changed.size();
}

//==============================================================================
void WebMirror::playInput()
{
    ld::LdWebInput e {};
    for (int n = 0; n < 256 && window != nullptr; ++n)
    {
        if (heldPress.has_value())
        {
            e = *heldPress;
            heldPress.reset();
        }
        else if (! server.takeInput (e))
            break;
        play (e);
        if (heldPress.has_value())
            return; // the rest after the pointer has moved
    }
}

void WebMirror::play (const ld::LdWebInput& e)
{
    const auto mods = modifiers (e.mods, e.buttons);
    const auto time = juce::Time::currentTimeMillis();

    if (e.kind == 5 || e.kind == 6)
    {
        auto* peer = window->getPeer();
        if (peer == nullptr)
            return;
        // Keys go where the keyboard focus is, as when typed on this computer.
        juce::ModifierKeys::currentModifiers = mods.withFlags (juce::ModifierKeys::currentModifiers.getRawFlags()
                                                               & juce::ModifierKeys::allMouseButtonModifiers);
        peer->handleKeyUpOrDown (e.kind == 5);
        if (e.kind == 5)
            if (const auto key = keyPress (e.key, juce::ModifierKeys::currentModifiers.withoutMouseButtons()); key.isValid())
                peer->handleKeyPress (key);
        return;
    }

    // The window (or menu) under the pointer; a drag stays with the one it started in.
    const juce::Point<float> pos (e.x * (float) window->getWidth(), e.y * (float) window->getHeight());
    juce::Component* target = window;
    juce::Point<float> offset;
    if (pressTarget != nullptr && (e.kind == 0 || e.kind == 2) && juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown())
        target = pressTarget;
    else
        for (auto it = overlays.rbegin(); it != overlays.rend(); ++it)
            if (it->component != nullptr && it->bounds.toFloat().contains (pos) && it->component->getPeer() != nullptr
                && (it->component->getPeer()->getStyleFlags() & juce::ComponentPeer::windowIgnoresMouseClicks) == 0)
            {
                target = it->component;
                break;
            }
    if (target != window.getComponent())
        offset = (target->getScreenPosition() - window->getScreenPosition()).toFloat();
    auto* peer = target->getPeer();
    if (peer == nullptr)
        return;

    if (e.kind == 4)
    {
        juce::MouseWheelDetails wheel;
        wheel.deltaX = e.dx;
        wheel.deltaY = e.dy;
        wheel.isReversed = false;
        wheel.isSmooth = false;
        wheel.isInertial = false;
        peer->handleMouseWheel (juce::MouseInputSource::InputSourceType::mouse, pos - offset, time, wheel);
        return;
    }

    // Leaving the page: the pointer moves off the window.
    const auto where = e.kind == 3 ? juce::Point<float> (-100000.0f, -100000.0f) : pos - offset;
    if (e.kind == 1 && ! warped)
    {
        // A press first moves this computer's pointer there, as what asks for
        // the mouse position (a menu opening at the mouse) reads the real
        // pointer. The press waits for the next round, after the system's own
        // move event (no buttons, same place) has gone through.
        const auto screen = target->localPointToGlobal (where).roundToInt();
        if (juce::Desktop::getMousePosition() != screen)
        {
            juce::Desktop::setMousePosition (screen);
            heldPress = e;
            warped = true;
            return;
        }
    }
    warped = false;
    if (e.kind == 1)
        pressTarget = target;
    else if (e.kind == 2 && e.buttons == 0)
        pressTarget = nullptr;
    juce::ModifierKeys::currentModifiers = mods;
    peer->handleMouseEvent (juce::MouseInputSource::InputSourceType::mouse, where, mods, juce::MouseInputSource::defaultPressure,
                            juce::MouseInputSource::defaultOrientation, time);
}

void WebMirror::logStats()
{
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (now - statsStart < 1000.0)
        return;
    const auto st = server.getStatus();
    const auto seconds = (now - statsStart) / 1000.0;
    juce::Logger::writeToLog ("[web] " + juce::String (frames / seconds, 1) + " frames/s, paint " + juce::String (frames > 0 ? paintMs / frames : 0.0, 2)
                              + " ms (max " + juce::String (paintMaxMs, 2) + "), " + juce::String (tiles) + " tiles, encode "
                              + juce::String (encoder->takeEncodeMs(), 1) + " ms, sent " + juce::String ((double) (st.bytesSent - bytesAtStart) / seconds / 1024.0, 1)
                              + " KB/s, " + juce::String (image.getWidth()) + "x" + juce::String (image.getHeight()));
    statsStart = now;
    paintMs = paintMaxMs = 0;
    frames = tiles = 0;
    bytesAtStart = st.bytesSent;
}

} // namespace livedaw::ui
