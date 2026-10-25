#include "ui/UsbRecorderPage.h"
#include "ui/ScreenWidgets.h"
#include "ui/WingPopups.h"

namespace livedaw::ui
{

namespace
{
    // The USB screen's colours (firmware 3.1).
    const juce::Colour panelGrey { 0xff202020 };
    const juce::Colour keyGrey { 0xff303030 };     // an enabled key, framed #c0c0c0
    const juce::Colour keyOffInk { 0xff484848 };   // a disabled key's icon and caption
    const juce::Colour selectedFill { 0xff342d12 };
    const juce::Colour folderBlue { 0xffa0b8e0 };  // folder names and icons
    const juce::Colour openKey { 0xff282c38 };     // a folder row's > key
    const juce::Colour wavChip { 0xff98b870 };
    const juce::Colour listChip { 0xff6ca0dc };
    const juce::Colour dateGrey { 0xff585858 };
    const juce::Colour timeRed { 0xfff05050 };
    const juce::Colour recRed { 0xfff54d42 };
    const juce::Colour playGreen { 0xff1dee60 };
    const juce::Colour stopYellow { 0xfff8c430 };  // the state light while stopped
    const juce::Colour captionGrey { 0xff484848 };
    const juce::Colour chipGrey { 0xff585858 };
    const juce::Colour reelGrey { 0xff949494 };
    const juce::Colour timelineFill { 0xff7f6a2f };
    const juce::Colour titleBlue { 0xff448ad7 };
    const juce::Colour flagYellow { 0xffe8d020 };
    const juce::Colour playlistInk { 0xff204870 }; // the PLAYLIST tab while the browser shows

    constexpr float kFigW = 1280.0f, kFigTop = 80.0f, kFigH = 720.0f;

    /** A rectangle given in WING screen pixels (1280 x 800, the status bar
        above y = 80), scaled to the page. */
    juce::Rectangle<int> fig (juce::Rectangle<int> page, float x0, float y0, float x1, float y1)
    {
        const float sx = (float) page.getWidth() / kFigW, sy = (float) page.getHeight() / kFigH;
        return juce::Rectangle<float> ((float) page.getX() + x0 * sx, (float) page.getY() + (y0 - kFigTop) * sy, (x1 - x0) * sx, (y1 - y0) * sy)
            .toNearestInt();
    }

    /** "0:00:08" (h:mm:ss). */
    juce::String timeText (int64_t samples, double rate)
    {
        const auto s = (int64_t) ((double) juce::jmax<int64_t> (0, samples) / juce::jmax (1.0, rate));
        return juce::String (s / 3600) + ":" + juce::String ((s / 60) % 60).paddedLeft ('0', 2) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
    }

    /** "00:00:15" for the timeline. */
    juce::String clockText (int64_t samples, double rate)
    {
        const auto s = (int64_t) ((double) juce::jmax<int64_t> (0, samples) / juce::jmax (1.0, rate));
        return juce::String (s / 3600).paddedLeft ('0', 2) + ":" + juce::String ((s / 60) % 60).paddedLeft ('0', 2) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
    }

    juce::Font font (float px, bool semi = true) { return wingFontPx (px, semi ? WingWeight::SemiBold : WingWeight::Medium); }

    // ---- icons this screen needs beyond the shared set ------------------------------

    void musicNotes (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
    {
        r = r.withSizeKeepingCentre (juce::jmin (r.getWidth(), r.getHeight()), juce::jmin (r.getWidth(), r.getHeight()));
        const float s = r.getWidth(), t = juce::jmax (1.2f, s * 0.08f);
        g.setColour (c);
        const auto head = [&] (float cx, float cy)
        {
            g.drawEllipse (juce::Rectangle<float> (s * 0.26f, s * 0.19f).withCentre ({ cx, cy }), t);
        };
        const float x1 = r.getX() + s * 0.36f, x2 = r.getX() + s * 0.86f, top = r.getY() + s * 0.12f;
        head (x1 - s * 0.11f, r.getY() + s * 0.78f);
        head (x2 - s * 0.11f, r.getY() + s * 0.70f);
        g.drawLine (x1, top + s * 0.08f, x1, r.getY() + s * 0.76f, t);
        g.drawLine (x2, top, x2, r.getY() + s * 0.68f, t);
        juce::Path beam;
        beam.addQuadrilateral (x1, top + s * 0.08f, x2, top, x2, top + s * 0.14f, x1, top + s * 0.22f);
        g.fillPath (beam);
    }

    void repeatArrows (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
    {
        const float d = juce::jmin (r.getWidth(), r.getHeight()) * 0.78f;
        const auto centre = r.getCentre();
        const float rad = d * 0.5f, t = juce::jmax (1.2f, d * 0.085f);
        g.setColour (c);
        for (int k = 0; k < 2; ++k)
        {
            const float a0 = juce::MathConstants<float>::pi * ((float) k + 0.12f), a1 = a0 + juce::MathConstants<float>::pi * 0.72f;
            juce::Path arc;
            arc.addCentredArc (centre.x, centre.y, rad, rad, 0.0f, a0, a1, true);
            g.strokePath (arc, juce::PathStrokeType (t));
            // The arrow head at the end of the arc, pointing along it.
            const juce::Point<float> tip (centre.x + rad * std::sin (a1), centre.y - rad * std::cos (a1));
            const juce::Point<float> along (std::cos (a1), std::sin (a1));
            const juce::Point<float> out (std::sin (a1), -std::cos (a1));
            const float a = d * 0.22f;
            juce::Path head;
            head.addTriangle (tip + along * a * 0.9f, tip + out * a * 0.6f, tip - out * a * 0.6f);
            g.fillPath (head);
        }
    }

    void circlePlus (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
    {
        const float d = juce::jmin (r.getWidth(), r.getHeight()) * 0.8f;
        auto ring = juce::Rectangle<float> (d, d).withCentre (r.getCentre());
        const float t = juce::jmax (1.2f, d * 0.08f);
        g.setColour (c);
        g.drawEllipse (ring, t);
        g.drawLine (ring.getCentreX() - d * 0.25f, ring.getCentreY(), ring.getCentreX() + d * 0.25f, ring.getCentreY(), t);
        g.drawLine (ring.getCentreX(), ring.getCentreY() - d * 0.25f, ring.getCentreX(), ring.getCentreY() + d * 0.25f, t);
    }

    void folderPlus (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
    {
        drawIcon (g, Icon::Folder, r.withTrimmedRight (r.getWidth() * 0.18f).withTrimmedBottom (r.getHeight() * 0.1f), c);
        const float d = juce::jmin (r.getWidth(), r.getHeight()) * 0.5f;
        auto badge = juce::Rectangle<float> (d, d).withPosition (r.getRight() - d, r.getBottom() - d);
        g.setColour (panelGrey);
        g.fillEllipse (badge);
        circlePlus (g, badge.expanded (d * 0.1f), c);
    }

    void stopCircle (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
    {
        const float d = juce::jmin (r.getWidth(), r.getHeight()) * 0.8f;
        auto ring = juce::Rectangle<float> (d, d).withCentre (r.getCentre());
        g.setColour (c);
        g.drawEllipse (ring, juce::jmax (1.2f, d * 0.09f));
        g.fillRect (ring.withSizeKeepingCentre (d * 0.36f, d * 0.36f));
    }

    /** OPEN PLAYLIST: a box with an arrow out of its corner; CLOSE: into it. */
    void boxArrow (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c, bool out)
    {
        const float s = juce::jmin (r.getWidth(), r.getHeight()) * 0.78f;
        auto box = juce::Rectangle<float> (s, s).withCentre (r.getCentre());
        const float t = juce::jmax (1.2f, s * 0.08f);
        g.setColour (c);
        juce::Path p;
        p.startNewSubPath (box.getX() + s * 0.45f, box.getY());
        p.lineTo (box.getX(), box.getY());
        p.lineTo (box.getX(), box.getBottom());
        p.lineTo (box.getRight(), box.getBottom());
        p.lineTo (box.getRight(), box.getY() + s * 0.55f);
        g.strokePath (p, juce::PathStrokeType (t));
        const juce::Point<float> inner (box.getX() + s * 0.4f, box.getY() + s * 0.6f), outer (box.getRight() + s * 0.05f, box.getY() - s * 0.05f);
        const auto from = out ? inner : outer, to = out ? outer : inner;
        g.drawLine ({ from, to }, t);
        const auto dir = (to - from) / to.getDistanceFrom (from);
        const juce::Point<float> normal (-dir.y, dir.x);
        const float a = s * 0.28f;
        juce::Path head;
        head.addTriangle (to, to - dir * a + normal * a * 0.5f, to - dir * a - normal * a * 0.5f);
        g.fillPath (head);
    }

    void recordDot (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
    {
        const float d = juce::jmin (r.getWidth(), r.getHeight()) * 0.62f;
        g.setColour (c);
        g.fillEllipse (juce::Rectangle<float> (d, d).withCentre (r.getCentre()));
    }

    /** A seven-segment readout (the WING's red time): upright digits 16 x 26
        with 2 px segments; a colon is a blank gap, as on the console. */
    float sevenSegment (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text, juce::Colour on, juce::Justification just)
    {
        static const int masks[] = { 0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f };
        const float h = area.getHeight(), w = h * 0.62f, gap = h * 0.08f, colonW = h * 0.23f, t = juce::jmax (1.2f, h * 0.08f);
        float total = 0.0f;
        for (auto ch : text)
            total += (ch == ':' ? colonW : w) + gap;
        total -= gap;
        float x = just.testFlags (juce::Justification::right) ? area.getRight() - total
                  : just.testFlags (juce::Justification::left) ? area.getX()
                                                               : area.getCentreX() - total * 0.5f;
        const float y = area.getY();
        g.setColour (on);
        for (auto ch : text)
        {
            if (ch == ':')
            {
                x += colonW + gap;
                continue;
            }
            int mask = 0;
            if (ch >= '0' && ch <= '9')
                mask = masks[ch - '0'];
            else if (ch == '-')
                mask = 0x40;
            juce::Path p;
            const float m = h * 0.5f;
            auto horiz = [&] (float yy) { p.addRectangle (x + t * 0.8f, yy - t * 0.5f, w - t * 1.6f, t); };
            auto vert = [&] (float xx, float y0, float y1) { p.addRectangle (xx - t * 0.5f, y0 + t * 0.8f, t, y1 - y0 - t * 1.6f); };
            if (mask & 0x01) horiz (y + t * 0.5f);
            if (mask & 0x02) vert (x + w - t * 0.5f, y, y + m);
            if (mask & 0x04) vert (x + w - t * 0.5f, y + m, y + h);
            if (mask & 0x08) horiz (y + h - t * 0.5f);
            if (mask & 0x10) vert (x + t * 0.5f, y + m, y + h);
            if (mask & 0x20) vert (x + t * 0.5f, y, y + m);
            if (mask & 0x40) horiz (y + m);
            g.fillPath (p);
            x += w + gap;
        }
        return x - gap;
    }

    /** Two tape reels with the tape between them; `angle` turns the spokes. */
    void reels (juce::Graphics& g, juce::Rectangle<float> area, double angle, bool running)
    {
        // [3.1] two reels 8 px apart (71 px across): the rim, a hub ring of
        // 0.35 d round a light 0.155 d hub, three spokes out towards the rim;
        // the tape runs from under each reel down over guide rollers to the
        // white head between them, 0.58 d under the reels' centres.
        const float d = juce::jmin (area.getHeight() * 0.9f, area.getWidth() * 0.47f);
        const float gapX = d * 4.0f / 71.0f;
        const float cx1 = area.getCentreX() - d * 0.5f - gapX, cx2 = area.getCentreX() + d * 0.5f + gapX, cy = area.getY() + d * 0.5f;
        const float t = juce::jmax (1.2f, d * 0.04f);
        const auto ink = running ? reelGrey.brighter (0.25f) : reelGrey;
        const auto light = juce::Colour (0xffa0a4a0);
        for (const float cx : { cx1, cx2 })
        {
            g.setColour (ink);
            g.drawEllipse (juce::Rectangle<float> (d, d).withCentre ({ cx, cy }).reduced (t * 0.5f), t);
            g.drawEllipse (juce::Rectangle<float> (d * 0.35f, d * 0.35f).withCentre ({ cx, cy }), t * 0.6f);
            g.setColour (light);
            g.fillEllipse (juce::Rectangle<float> (d * 0.155f, d * 0.155f).withCentre ({ cx, cy }));
            for (int k = 0; k < 3; ++k)
            {
                const auto a = (float) (angle + k * juce::MathConstants<double>::twoPi / 3.0);
                g.drawLine (cx + std::cos (a) * d * 0.24f, cy + std::sin (a) * d * 0.24f, cx + std::cos (a) * d * 0.40f, cy + std::sin (a) * d * 0.40f,
                            t * 1.4f);
            }
        }
        // The tape and its rollers.
        const float by = cy + d * 0.58f, roller = d * 0.1f;
        const float x0 = cx1 - d * 0.105f, x1 = cx1 + d * 0.15f, x2 = cx2 - d * 0.16f, x3 = cx2 + d * 0.105f;
        g.setColour (ink);
        juce::Path tape;
        tape.startNewSubPath (cx1 - d * 0.40f, cy + d * 0.25f);
        tape.lineTo (x0, by);
        tape.lineTo (x3, by);
        tape.lineTo (cx2 + d * 0.40f, cy + d * 0.25f);
        g.strokePath (tape, juce::PathStrokeType (t * 0.6f));
        for (const float x : { x0, x1, x2, x3 })
        {
            g.setColour (Colours::screenPanel);
            g.fillEllipse (juce::Rectangle<float> (roller, roller).withCentre ({ x, by }));
            g.setColour (light);
            g.drawEllipse (juce::Rectangle<float> (roller, roller).withCentre ({ x, by }), t * 0.5f);
        }
        g.setColour (juce::Colours::white);
        g.fillRect (juce::Rectangle<float> (d * 0.11f, d * 0.11f).withCentre ({ area.getCentreX(), by }));
    }

    /** The recorder's and player's meter scale in segments from the bottom:
        -60, -48, -24, -12, -6 and CLIP three segments apart. */
    float meterSegments (float db)
    {
        static constexpr float marks[] = { -60.0f, -48.0f, -24.0f, -12.0f, -6.0f, 0.0f };
        if (db <= marks[0])
            return -1.0f;
        for (int i = 1; i < 6; ++i)
            if (db <= marks[i])
                return 3.0f * ((float) (i - 1) + (db - marks[i - 1]) / (marks[i] - marks[i - 1]));
        return 15.0f;
    }

    /** The meter bank [3.1], 106 x 157 WING pixels: the scale CLIP .. -60 at the
        left, four bars of 16 segments (lit for the channels in use), the top
        one CLIP. */
    void meterBank (juce::Graphics& g, juce::Rectangle<float> area, const std::array<float, 4>& levels, int channels)
    {
        g.setColour (juce::Colours::black);
        g.fillRect (area);
        const float sx = area.getWidth() / 106.0f, sy = area.getHeight() / 157.0f;
        constexpr int kSegments = 16;
        auto segment = [&] (int fromTop) { return area.getY() + (8.0f + 9.0f * (float) fromTop) * sy; };
        g.setColour (Colours::caption);
        // The scale shrinks to the bank's width (narrow in small windows) rather than being cut.
        g.setFont (fontFittingWidth (font (juce::jmax (5.0f, 7.0f * sy), false), "CLIP", 26.0f * sx, 0.8f));
        const std::pair<const char*, int> marks[] = { { "CLIP", 0 }, { "-6", 3 }, { "-12", 6 }, { "-24", 9 }, { "-48", 12 }, { "-60", 15 } };
        for (auto& [text, row] : marks)
            g.drawText (text, juce::Rectangle<float> (area.getX(), segment (row) - 6.0f * sy, 28.0f * sx, 18.0f * sy), juce::Justification::centredRight,
                        false);
        const int n = juce::jlimit (1, 4, channels);
        for (int ch = 0; ch < 4; ++ch)
        {
            const float x = area.getX() + (34.0f + 17.0f * (float) ch) * sx;
            const float lit = ch < n ? meterSegments (levels[(size_t) ch]) : -1.0f;
            for (int seg = 0; seg < kSegments; ++seg)
            {
                const int fromBottom = kSegments - 1 - seg;
                const auto colour = fromBottom == kSegments - 1 ? Colours::meterRed : (fromBottom >= 12 ? Colours::meterYellow : Colours::meterGreen);
                g.setColour (lit >= (float) fromBottom ? colour : juce::Colour (0xff101010));
                g.fillRect (x, segment (seg), 13.0f * sx, 6.0f * sy);
            }
        }
    }

    void chip (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, juce::Colour fill, juce::Colour ink)
    {
        g.setColour (fill);
        g.fillRoundedRectangle (r, juce::jmax (1.0f, r.getHeight() * 0.06f));
        g.setColour (ink);
        g.setFont (font (r.getHeight() * 0.5f));
        g.drawFittedText (text, r.toNearestInt().reduced (1, 0), juce::Justification::centred, 1, 0.7f);
    }

    juce::String displayName (const juce::File& f) { return f.isDirectory() ? f.getFileName() : f.getFileNameWithoutExtension(); }
} // namespace

//==============================================================================
UsbRecorderPage::UsbRecorderPage (UiContext& c) : ctx (c)
{
    usb().loadSettings (ctx.settings);
    reload();
    startTimerHz (20);
}

UsbRecorderPage::~UsbRecorderPage() = default;

UsbRecorder& UsbRecorderPage::usb() const { return ctx.engine.getUsbRecorder(); }

void UsbRecorderPage::saveSettings()
{
    usb().saveSettings (ctx.settings);
}

void UsbRecorderPage::openList (juce::Rectangle<int> anchorArea, const juce::StringArray& items, int current, std::function<void (int)> chosen,
                                popups::ListOptions options)
{
    if (listAnchor.getParentComponent() == nullptr)
    {
        listAnchor.setInterceptsMouseClicks (false, false);
        addChildComponent (listAnchor);
    }
    listAnchor.setBounds (anchorArea);
    popups::chooseFrom (listAnchor, items, current, std::move (chosen), std::move (options));
}

void UsbRecorderPage::chooseRecFormat()
{
    auto& u = usb();
    const auto types = { AudioFileFormat::Type::Wav, AudioFileFormat::Type::Flac, AudioFileFormat::Type::Aiff, AudioFileFormat::Type::Mp3 };
    juce::StringArray items;
    popups::ListOptions options;
    options.caption = "FORMAT";
    for (auto t : types)
        items.add (AudioFileFormat::typeName (t));
    // MP3 needs 32, 44.1 or 48 kHz.
    if (AudioFileFormat::mp3().check (2, u.getSampleRate()).isNotEmpty())
        options.disabled.push_back (3);
    juce::Component::SafePointer<UsbRecorderPage> safe (this);
    openList (formatKey, items, (int) u.getRecFileType(), [safe] (int i)
    {
        if (safe == nullptr || i < 0 || i > 3)
            return;
        safe->usb().setRecFileType (static_cast<AudioFileFormat::Type> (i));
        safe->saveSettings();
        safe->layout(); // the bit keys or the bit rate list
        safe->repaint();
    }, options);
}

void UsbRecorderPage::chooseKbps()
{
    auto& u = usb();
    juce::StringArray items;
    int current = 0;
    for (int b : AudioFileFormat::kMp3Bitrates)
    {
        if (b == u.getRecKbps())
            current = items.size();
        items.add (juce::String (b) + " kbps");
    }
    popups::ListOptions options;
    options.caption = "BIT RATE";
    juce::Component::SafePointer<UsbRecorderPage> safe (this);
    openList (kbpsKey, items, current, [safe] (int i)
    {
        if (safe == nullptr || ! juce::isPositiveAndBelow (i, (int) std::size (AudioFileFormat::kMp3Bitrates)))
            return;
        safe->usb().setRecKbps (AudioFileFormat::kMp3Bitrates[i]);
        safe->saveSettings();
        safe->repaint();
    }, options);
}

void UsbRecorderPage::warn (const juce::String& title, const juce::Result& r)
{
    if (r.failed())
        popups::warn (*this, title, r.getErrorMessage());
}

StatusTitle UsbRecorderPage::getStatusTitle()
{
    // The selected clip in a blue tile (fig. 78), else the one playing.
    StatusTitle t;
    t.kind = StatusTitle::Kind::Text;
    t.fill = titleBlue;
    t.ink = juce::Colours::black;
    const auto* row = selectedRow();
    auto file = row != nullptr ? row->file : usb().getCurrentFile();
    t.text = file != juce::File() ? displayName (file) : juce::String();
    return t;
}

void UsbRecorderPage::refresh()
{
    reload();
}

void UsbRecorderPage::visibilityChanged()
{
    if (isVisible())
    {
        usb().refreshQueue();
        reload();
    }
}

//==============================================================================
void UsbRecorderPage::reload()
{
    auto& u = usb();
    rows.clear();
    if (playlistView)
    {
        for (const auto& e : u.getQueue())
            rows.push_back ({ e.file, (int) UsbRecorder::ItemKind::Wav, e.id, e.skip, e.stop, e.file.getLastModificationTime() });
    }
    else
    {
        auto folder = u.getBrowserFolder();
        if (! folder.isDirectory() || ! u.isInsideStorage (folder))
        {
            folder = u.getStorageFolder();
            folder.createDirectory();
            u.setBrowserFolder (folder);
        }
        for (const auto& item : UsbRecorder::listFolder (folder))
            rows.push_back ({ item.file, (int) item.kind, 0, false, false, item.modified });
    }
    folderShown = u.getBrowserFolder();
    folderShownTime = folderShown.getLastModificationTime();
    queueVersion = u.getQueueVersion();
    selected = -1;
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].file == selectedFile)
            selected = (int) i;
    scroll = juce::jlimit (0, juce::jmax (0, (int) rows.size() - visibleRows()), scroll);
    layout();
    repaint();
    notifyStatusKeysChanged();
}

int UsbRecorderPage::visibleRows() const
{
    return juce::jmax (1, rowsArea.getHeight() / juce::jmax (1, rowHeight));
}

juce::Rectangle<int> UsbRecorderPage::rowArea (int visibleIndex) const
{
    return { rowsArea.getX(), rowsArea.getY() + visibleIndex * rowHeight, rowsArea.getWidth(), rowHeight };
}

int UsbRecorderPage::rowAt (juce::Point<int> p) const
{
    if (! rowsArea.contains (p))
        return -1;
    const int row = scroll + (p.y - rowsArea.getY()) / juce::jmax (1, rowHeight);
    return juce::isPositiveAndBelow (row, (int) rows.size()) ? row : -1;
}

const UsbRecorderPage::Row* UsbRecorderPage::selectedRow() const
{
    return juce::isPositiveAndBelow (selected, (int) rows.size()) ? &rows[(size_t) selected] : nullptr;
}

void UsbRecorderPage::select (int row)
{
    selected = juce::isPositiveAndBelow (row, (int) rows.size()) ? row : -1;
    selectedFile = selected >= 0 ? rows[(size_t) selected].file : juce::File();
    if (selected >= 0)
    {
        if (selected < scroll)
            scroll = selected;
        else if (selected >= scroll + visibleRows())
            scroll = selected - visibleRows() + 1;
    }
    repaint();
    notifyStatusKeysChanged(); // the title shows the selection
}

void UsbRecorderPage::scrollBy (int delta)
{
    scroll = juce::jlimit (0, juce::jmax (0, (int) rows.size() - visibleRows()), scroll + delta);
    repaint (leftPanel);
}

void UsbRecorderPage::openFolder (const juce::File& folder)
{
    if (! usb().isInsideStorage (folder))
        return;
    selectedFile = juce::File();
    // Going up selects the folder we came from.
    if (folderShown.isAChildOf (folder))
    {
        auto f = folderShown;
        while (f.getParentDirectory() != folder && f.getParentDirectory() != f)
            f = f.getParentDirectory();
        selectedFile = f;
    }
    usb().setBrowserFolder (folder);
    scroll = 0;
    saveSettings();
    reload();
    if (selected >= 0)
        select (selected);
}

//==============================================================================
void UsbRecorderPage::showBrowser()
{
    if (! playlistView)
    {
        openFolder (usb().getStorageFolder()); // USB again: the top of the stick
        return;
    }
    playlistView = false;
    selectedFile = usb().getCurrentFile();
    scroll = 0;
    reload();
}

void UsbRecorderPage::showPlaylist()
{
    if (playlistView)
        return;
    playlistView = true;
    selectedFile = usb().getCurrentFile();
    scroll = 0;
    reload();
}

void UsbRecorderPage::addSelected()
{
    const auto* row = selectedRow();
    if (row == nullptr)
        usb().addAll (usb().getBrowserFolder()); // ADD ALL: the folder shown
    else if (row->kind == (int) UsbRecorder::ItemKind::Folder)
        usb().addAll (row->file); // ADD ALL
    else if (row->kind == (int) UsbRecorder::ItemKind::Wav)
    {
        if (! usb().isPlaylistOpen())
            usb().createPlaylist();
        usb().add (row->file);
    }
    repaint();
}

void UsbRecorderPage::newFolder()
{
    juce::Component::SafePointer<UsbRecorderPage> safe (this);
    popups::TextOptions options;
    options.icon = Icon::Folder;
    options.maxLength = 32;
    popups::editText (*this, "NEW FOLDER", "NEW FOLDER", [safe] (const juce::String& name)
    {
        if (safe == nullptr)
            return;
        auto r = safe->usb().createFolder (safe->usb().getBrowserFolder(), name);
        safe->warn ("FOLDER", r);
        safe->selectedFile = safe->usb().getBrowserFolder().getChildFile (juce::File::createLegalFileName (name.trim()));
        safe->reload();
    }, options);
}

void UsbRecorderPage::renameSelected()
{
    const auto* row = selectedRow();
    if (row == nullptr || playlistView)
        return;
    const auto file = row->file;
    juce::Component::SafePointer<UsbRecorderPage> safe (this);
    popups::TextOptions options;
    options.icon = row->kind == (int) UsbRecorder::ItemKind::Folder ? Icon::Folder : Icon::Edit;
    options.maxLength = 48;
    popups::editText (*this, "RENAME", displayName (file), [safe, file] (const juce::String& name)
    {
        if (safe == nullptr)
            return;
        juce::File renamed;
        auto r = safe->usb().rename (file, name, &renamed);
        safe->warn ("RENAME", r);
        if (r.wasOk() && renamed != juce::File())
            safe->selectedFile = renamed;
        safe->saveSettings();
        safe->reload();
    }, options);
}

void UsbRecorderPage::deleteSelected()
{
    const auto* row = selectedRow();
    if (row == nullptr || playlistView)
        return;
    const auto file = row->file;
    juce::Component::SafePointer<UsbRecorderPage> safe (this);
    const auto what = file.isDirectory() ? "the folder \"" + file.getFileName() + "\" and everything in it" : "\"" + file.getFileName() + "\"";
    popups::confirm (*this, "DELETE", "Delete " + what + " from the USB folder?", "DELETE", [safe, file]
    {
        if (safe == nullptr)
            return;
        safe->warn ("DELETE", safe->usb().deleteItem (file));
        safe->saveSettings();
        safe->reload();
    }, true);
}

void UsbRecorderPage::setRecPath()
{
    // The selected folder, else the folder shown.
    const auto* row = selectedRow();
    auto folder = row != nullptr && row->kind == (int) UsbRecorder::ItemKind::Folder ? row->file : usb().getBrowserFolder();
    usb().setRecPath (folder);
    saveSettings();
    repaint();
}

void UsbRecorderPage::chooseStorageFolder()
{
    // Sidecar's "USB stick" is a folder: hold USB (or right-click) to move it.
    auto chooser = std::make_shared<juce::FileChooser> ("The USB folder (the recorder's stick)", usb().getStorageFolder());
    juce::Component::SafePointer<UsbRecorderPage> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [safe, chooser] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || fc.getResult() == juce::File())
            return;
        safe->usb().setStorageFolder (fc.getResult());
        safe->usb().setBrowserFolder (fc.getResult());
        safe->usb().setRecPath (fc.getResult());
        safe->saveSettings();
        // One stick: LIBRARY's USB location follows if it has none of its own.
        if (safe->ctx.settings.getValue ("library.usbFolder").isEmpty())
            safe->ctx.settings.setValue ("library.usbFolder", fc.getResult().getFullPathName());
        safe->playlistView = false;
        safe->reload();
    });
}

void UsbRecorderPage::savePlaylist()
{
    if (! usb().isPlaylistOpen())
        return;
    const auto existing = usb().getPlaylistFile();
    juce::Component::SafePointer<UsbRecorderPage> safe (this);
    popups::TextOptions options;
    options.icon = Icon::SaveTray;
    options.maxLength = 32;
    options.actionText = "SAVE";
    const auto folder = existing != juce::File() ? existing.getParentDirectory() : usb().getBrowserFolder();
    popups::editText (*this, "SAVE PLAYLIST", existing != juce::File() ? existing.getFileNameWithoutExtension() : juce::String ("PLAYLIST"),
                      [safe, folder] (const juce::String& name)
    {
        if (safe == nullptr)
            return;
        const auto clean = juce::File::createLegalFileName (name.trim());
        if (clean.isEmpty())
            return;
        safe->warn ("SAVE PLAYLIST", safe->usb().savePlaylist (folder.getChildFile (clean + UsbRecorder::kPlaylistExtension)));
        safe->reload();
    }, options);
}

void UsbRecorderPage::closePlaylist()
{
    if (! usb().isPlaylistOpen())
        return;
    juce::Component::SafePointer<UsbRecorderPage> safe (this);
    auto close = [safe]
    {
        if (safe == nullptr)
            return;
        safe->usb().closePlaylist();
        safe->playlistView = false;
        safe->reload();
    };
    if (usb().isPlaylistDirty() && ! usb().getQueue().empty())
        popups::confirm (*this, "CLOSE PLAYLIST", "The playlist has changes that aren't saved. Close it anyway?", "CLOSE", close, true);
    else
        close();
}

void UsbRecorderPage::playSelected()
{
    auto& u = usb();
    const auto state = u.getPlayState();
    if (state == UsbRecorder::PlayState::Paused)
    {
        u.play();
        return;
    }
    if (state == UsbRecorder::PlayState::Playing)
        return;
    const auto* row = selectedRow();
    if (row != nullptr && playlistView && row->entry != u.getCurrentEntry())
        u.playEntry (row->entry, true);
    else if (row != nullptr && ! playlistView && row->kind == (int) UsbRecorder::ItemKind::Wav && row->file != u.getCurrentFile())
        u.playFile (row->file);
    else
        u.play();
}

bool UsbRecorderPage::canPlay() const
{
    // Something to play: paused, a clip cued or selected, or a playlist.
    UsbRecorder::FileInfo info;
    int64_t length = 0;
    bool mismatch = false;
    return usb().getPlayState() == UsbRecorder::PlayState::Paused || currentFileInfo (info, length, mismatch);
}

bool UsbRecorderPage::canStep() const
{
    // < >: through the playlist, or the folder once a clip is cued.
    auto& u = usb();
    return ! u.getQueue().empty() && (u.isPlaylistOpen() || u.getCurrentFile() != juce::File());
}

int UsbRecorderPage::openKeyWidth() const { return fig (getLocalBounds(), 0, 0, 32, 0).getWidth(); }

void UsbRecorderPage::pressPlay (UiContext& ctx)
{
    auto& u = ctx.engine.getUsbRecorder();
    if (u.getPlayState() != UsbRecorder::PlayState::Playing)
        u.play();
}

void UsbRecorderPage::pressRec (UiContext& ctx)
{
    auto& u = ctx.engine.getUsbRecorder();
    if (u.getRecState() == UsbRecorder::RecState::Recording)
    {
        u.stopRecording();
        return;
    }
    if (auto r = u.record(); r.failed())
        ctx.log.warning ("USB recorder: " + r.getErrorMessage());
}

void UsbRecorderPage::seekFromTimeline (int x)
{
    auto& u = usb();
    const auto length = u.getLength();
    if (length <= 0 || timeline.getWidth() <= 0)
        return;
    const double t = juce::jlimit (0.0, 1.0, (double) (x - timeline.getX()) / (double) timeline.getWidth());
    u.seek ((int64_t) (t * (double) length));
    repaint (timeline);
}

//==============================================================================
void UsbRecorderPage::addKey (juce::Rectangle<int> r, std::function<void()> onClick, std::function<bool()> enabled, std::function<void()> onHold)
{
    keys.push_back ({ r, std::move (onClick), std::move (onHold), std::move (enabled) });
}

void UsbRecorderPage::resized()
{
    layout();
}

void UsbRecorderPage::layout()
{
    const auto b = getLocalBounds();
    auto f = [&b] (float x0, float y0, float x1, float y1) { return fig (b, x0, y0, x1, y1); };

    // [3.1] the browser 8..656 (the toolbar 80..144, the list 152..774 with
    // rows 50 tall, 56 apart, and the scroll bar at 618..650); the recorder,
    // its keys, the playlist keys, the player, its keys and the timeline in
    // 664..1272.
    leftPanel = f (8, 80, 656, 774);
    toolbar = f (8, 80, 656, 144);
    listArea = f (8, 152, 656, 774);
    scrollBar = f (618, 158, 650, 768);
    scrollUp = f (618, 158, 650, 184);
    scrollDown = f (618, 742, 650, 768);
    // In a folder of the stick, its row (with the key back up) heads the list.
    const bool topFolder = usb().getBrowserFolder() == usb().getStorageFolder();
    if (playlistView || topFolder)
    {
        listHeader = headerFolderKey = {};
        rowsArea = f (14, 158, 612, 774);
    }
    else
    {
        listHeader = f (14, 158, 612, 208);
        headerFolderKey = f (580, 158, 612, 208);
        rowsArea = f (14, 214, 612, 774);
    }
    rowHeight = juce::jmax (18, f (0, 158, 10, 214).getHeight());

    // The toolbar keys (60 x 56, fig. 79's spacing for the playlist).
    const float browserX[7][2] = { { 12, 72 }, { 76, 136 }, { 162, 222 }, { 270, 330 }, { 334, 394 }, { 458, 518 }, { 580, 642 } };
    const float playlistX[8][2] = { { 12, 72 }, { 76, 136 }, { 176, 223 }, { 281, 327 }, { 406, 450 }, { 467, 514 }, { 530, 576 }, { 592, 640 } };
    for (size_t i = 0; i < toolKeys.size(); ++i)
    {
        if (playlistView)
            toolKeys[i] = f (playlistX[i][0], 84, playlistX[i][1], 140);
        else
            toolKeys[i] = i < 7 ? f (browserX[i][0], 84, browserX[i][1], 140) : juce::Rectangle<int>();
    }

    // Right: the recorder, the playlist keys, the player, the timeline.
    recBox = f (664, 80, 1272, 253);
    recMeter = f (672, 88, 778, 245);
    recLight = f (797, 94, 821, 118);
    recTime = f (845, 93, 1100, 119);
    recReels = f (797, 152, 947, 232);
    channelFrame = f (1136, 88, 1264, 136);
    bitFrame = f (1136, 144, 1264, 192);
    channelKeys = { f (1138, 90, 1199, 134), f (1201, 90, 1262, 134) };
    bitKeys = { f (1138, 146, 1199, 190), f (1201, 146, 1262, 190) };
    formatKey = f (968, 144, 1128, 192); // Sidecar's file formats
    kbpsKey = bitKeys[0].getUnion (bitKeys[1]);
    recPathText = f (968, 198, 1264, 216);
    recCaption = f (968, 218, 1264, 248);
    recKeys = f (664, 257, 1272, 337);
    playlistKeys = f (664, 347, 1272, 403);
    playBox = f (664, 413, 1272, 586);
    playMeter = f (672, 421, 778, 578);
    playLight = f (797, 427, 821, 451);
    playTime = f (845, 425, 1100, 451);
    playRest = f (845, 458, 949, 474);
    playReels = f (797, 485, 947, 565);
    playChips = f (1154, 431, 1254, 499);
    playCaption = f (968, 551, 1264, 581);
    playKeys = f (664, 590, 1272, 670);
    timeline = f (672, 682, 1264, 766);
    const float keyX[4][2] = { { 672, 816 }, { 824, 968 }, { 976, 1120 }, { 1128, 1264 } };
    for (size_t i = 0; i < 4; ++i)
    {
        recKeyAreas[i] = f (keyX[i][0], 265, keyX[i][1], 329);
        playlistKeyAreas[i] = f (keyX[i][0], 351, keyX[i][1], 399);
    }
    for (size_t i = 0; i < 3; ++i)
        playKeyAreas[i] = f (keyX[i][0], 598, keyX[i][1], 662);
    playKeyAreas[3] = f (1128, 598, 1192, 662);
    playKeyAreas[4] = f (1200, 598, 1264, 662);

    // What the keys do.
    keys.clear();
    auto& u = usb();
    auto* self = this;
    if (playlistView)
    {
        auto selectedIndex = [self] { return self->selected; };
        addKey (toolKeys[0], [self] { self->showBrowser(); }, {}, [self] { self->chooseStorageFolder(); });
        addKey (toolKeys[1], [self] { self->showPlaylist(); });
        addKey (toolKeys[2], [&u] { u.setRepeat (! u.getRepeat()); }, [&u] { return u.isPlaylistOpen(); });
        addKey (toolKeys[3], [self, &u, selectedIndex]
        {
            if (selectedIndex() >= 0)
            {
                u.remove (selectedIndex());
                self->reload();
            }
        }, [self] { return self->selected >= 0; });
        addKey (toolKeys[4], [self, &u]
        {
            if (const auto* r = self->selectedRow())
                u.setSkip (self->selected, ! r->skip);
            self->reload();
        }, [self] { return self->selected >= 0; });
        addKey (toolKeys[5], [self, &u]
        {
            if (const auto* r = self->selectedRow())
                u.setStop (self->selected, ! r->stop);
            self->reload();
        }, [self] { return self->selected >= 0; },
        [self, &u]
        {
            // Held: every clip (or none, if all stop already).
            bool all = ! self->rows.empty();
            for (auto& r : self->rows)
                all = all && r.stop;
            u.setStopAll (! all);
            self->reload();
        });
        for (int d : { -1, 1 })
            addKey (toolKeys[d < 0 ? 6 : 7], [self, &u, d]
            {
                const int to = u.move (self->selected, d);
                if (to >= 0)
                {
                    self->reload();
                    self->select (to);
                }
            }, [self, d] { return self->selected >= 0 && juce::isPositiveAndBelow (self->selected + d, (int) self->rows.size()); });
    }
    else
    {
        auto isKind = [self] (UsbRecorder::ItemKind kind)
        {
            const auto* r = self->selectedRow();
            return r != nullptr && r->kind == (int) kind;
        };
        addKey (toolKeys[0], [self] { self->showBrowser(); }, {}, [self] { self->chooseStorageFolder(); });
        addKey (toolKeys[1], [self] { self->showPlaylist(); });
        addKey (toolKeys[2], [self] { self->addSelected(); }, [isKind] { return ! isKind (UsbRecorder::ItemKind::Playlist); });
        addKey (toolKeys[3], [self] { self->newFolder(); });
        addKey (toolKeys[4], [self] { self->renameSelected(); }, [self] { return self->selected >= 0; });
        addKey (toolKeys[5], [self] { self->deleteSelected(); }, [self] { return self->selected >= 0; });
        addKey (toolKeys[6], [self] { self->setRecPath(); });
        if (! headerFolderKey.isEmpty())
            addKey (headerFolderKey, [self] { self->openFolder (self->usb().getBrowserFolder().getParentDirectory()); });
    }
    addKey (scrollUp, [self] { self->scrollBy (-1); });
    addKey (scrollDown, [self] { self->scrollBy (1); });

    // The recorder.
    const auto stopped = [&u] { return ! u.isRecording(); };
    const bool mp3 = u.getRecFileType() == AudioFileFormat::Type::Mp3;
    addKey (formatKey, [self] { self->chooseRecFormat(); }, stopped);
    addKey (channelKeys[0], [self, &u] { u.setRecChannels (2); self->saveSettings(); }, stopped);
    addKey (channelKeys[1], [self, &u] { u.setRecChannels (4); self->saveSettings(); },
            [&u] { return ! u.isRecording() && ! u.isFullSpeedOnly() && u.getRecFileType() != AudioFileFormat::Type::Mp3; });
    if (mp3)
        addKey (kbpsKey, [self] { self->chooseKbps(); }, stopped);
    else
    {
        addKey (bitKeys[0], [self, &u] { u.setRecBits (16); self->saveSettings(); }, stopped);
        addKey (bitKeys[1], [self, &u] { u.setRecBits (24); self->saveSettings(); }, [&u] { return ! u.isRecording() && ! u.isFullSpeedOnly(); });
    }
    addKey (recKeyAreas[0], [self, &u]
    {
        self->warn ("REC", u.record());
        self->reload();
    }, [&u] { return u.getRecState() != UsbRecorder::RecState::Recording; });
    addKey (recKeyAreas[1], [&u] { u.toggleRecPause(); }, [&u] { return u.isRecording(); });
    addKey (recKeyAreas[2], [self, &u]
    {
        u.stopRecording();
        self->reload();
    }, [&u] { return u.isRecording() || u.getRecState() == UsbRecorder::RecState::Error; });
    addKey (recKeyAreas[3], [self, &u]
    {
        self->warn ("NEW FILE", u.newFile());
        self->reload();
    }, [&u] { return u.isRecording(); });

    // The playlist keys.
    addKey (playlistKeyAreas[0], [self, &u]
    {
        u.createPlaylist();
        self->playlistView = true;
        self->reload();
    });
    addKey (playlistKeyAreas[1], [self, &u]
    {
        if (const auto* r = self->selectedRow(); r != nullptr && r->kind == (int) UsbRecorder::ItemKind::Playlist)
        {
            self->warn ("OPEN PLAYLIST", u.openPlaylist (r->file));
            self->playlistView = true;
            self->selectedFile = juce::File();
            self->reload();
        }
    }, [self] { const auto* r = self->selectedRow(); return ! self->playlistView && r != nullptr && r->kind == (int) UsbRecorder::ItemKind::Playlist; });
    addKey (playlistKeyAreas[2], [self] { self->savePlaylist(); }, [&u] { return u.isPlaylistOpen(); });
    addKey (playlistKeyAreas[3], [self] { self->closePlaylist(); }, [&u] { return u.isPlaylistOpen(); });

    // The player.
    addKey (playKeyAreas[0], [self] { self->playSelected(); }, [self, &u] { return self->canPlay() && u.getPlayState() != UsbRecorder::PlayState::Playing; });
    addKey (playKeyAreas[1], [&u] { u.togglePlayPause(); },
            [&u] { return u.getPlayState() == UsbRecorder::PlayState::Playing || u.getPlayState() == UsbRecorder::PlayState::Paused; });
    addKey (playKeyAreas[2], [&u] { u.stop(); },
            [&u] { return u.getPlayState() == UsbRecorder::PlayState::Playing || u.getPlayState() == UsbRecorder::PlayState::Paused; });
    addKey (playKeyAreas[3], [&u] { u.previous(); }, [self] { return self->canStep(); });
    addKey (playKeyAreas[4], [&u] { u.next(); }, [self] { return self->canStep(); });
}

//==============================================================================
void UsbRecorderPage::timerCallback()
{
    auto& u = usb();
    // Meters: the peak since the last read, falling 1.5 dB per tick.
    for (int ch = 0; ch < 4; ++ch)
    {
        recLevels[(size_t) ch] = juce::jmax (u.getRecLevel (ch), recLevels[(size_t) ch] - 1.5f);
        playLevels[(size_t) ch] = juce::jmax (u.getPlayLevel (ch), playLevels[(size_t) ch] - 1.5f);
    }
    // The reels turn while the transports run.
    const auto rec = u.getRecState();
    const auto play = u.getPlayState();
    if (rec == UsbRecorder::RecState::Recording)
        reelAngle[0] += 0.25;
    if (play == UsbRecorder::PlayState::Playing)
        reelAngle[1] += 0.25;

    if (u.getQueueVersion() != queueVersion || (! playlistView && u.getBrowserFolder() != folderShown))
        reload();
    else if (! playlistView && isShowing() && ++folderCheckTicks >= 20)
    {
        // The folder changed on disk (files copied in, deleted or renamed outside the app).
        folderCheckTicks = 0;
        if (folderShown.getLastModificationTime() != folderShownTime)
        {
            u.refreshQueue();
            reload();
        }
    }

    // Repaint what changed.
    juce::String state;
    state << (int) rec << '|' << (int) play << '|' << (int) (u.getRecTime() / 4800) << '|' << (int) (u.getPosition() / 4800) << '|'
          << (int) u.getLength() << '|' << (juce::int64) u.getCurrentEntry() << '|' << (int) u.isPlaylistOpen() << (int) u.getRepeat();
    for (auto l : recLevels)
        state << '|' << juce::roundToInt (l);
    for (auto l : playLevels)
        state << '|' << juce::roundToInt (l);
    const bool blink = rec == UsbRecorder::RecState::Paused && (juce::Time::getMillisecondCounter() / 400) % 2 == 0;
    state << (blink ? "b" : "");
    if (state != shownState || rec == UsbRecorder::RecState::Recording || play == UsbRecorder::PlayState::Playing)
    {
        const bool entryChanged = ! shownState.isEmpty() && shownState.fromFirstOccurrenceOf ("|", false, false) != state.fromFirstOccurrenceOf ("|", false, false);
        shownState = state;
        repaint (recBox.getUnion (playBox).getUnion (timeline).getUnion (recKeys).getUnion (playKeys).getUnion (playlistKeys).getUnion (toolbar));
        if (entryChanged)
            repaint (leftPanel);
    }
}

//==============================================================================
void UsbRecorderPage::paintKey (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text,
                                std::function<void (juce::Graphics&, juce::Rectangle<float>, juce::Colour)> icon, bool enabled, bool lit,
                                juce::Colour litColour) const
{
    if (area.isEmpty())
        return;
    // [3.1] an enabled key #303030 framed #c0c0c0, a disabled one flat with a
    // black frame and #484848 ink; a lit key is filled.
    auto r = area.toFloat();
    g.setColour (lit ? litColour : (enabled ? keyGrey : panelGrey));
    g.fillRect (r);
    g.setColour (lit ? litColour.darker (0.3f) : (enabled ? Colours::caption : juce::Colours::black));
    g.drawRect (r, 1.0f);
    const auto ink = lit ? juce::Colours::black : (enabled ? Colours::caption : keyOffInk);
    auto content = r.reduced (r.getHeight() * 0.2f, r.getHeight() * 0.12f);
    if (icon)
    {
        const float s = text.isEmpty() ? juce::jmin (content.getWidth(), content.getHeight()) * 0.6f : content.getHeight() * 0.66f;
        auto iconBox = text.isEmpty() ? content.withSizeKeepingCentre (s, s) : content.removeFromLeft (content.getHeight()).withSizeKeepingCentre (s, s);
        icon (g, iconBox, ink);
    }
    if (text.isNotEmpty())
    {
        const int lines = juce::StringArray::fromLines (text).size();
        g.setColour (ink);
        g.setFont (font (juce::jmin (content.getHeight() * (lines > 1 ? 0.3f : 0.34f), getHeight() * 0.022f)));
        g.drawFittedText (text, content.toNearestInt(), juce::Justification::centred, lines, 0.7f);
    }
}

void UsbRecorderPage::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
    paintToolbar (g);
    paintList (g);
    paintRecorder (g);
    paintPlaylistKeys (g);
    paintPlayer (g);
    paintTimeline (g);
}

void UsbRecorderPage::paintToolbar (juce::Graphics& g)
{
    auto& u = usb();
    g.setColour (panelGrey);
    g.fillRect (toolbar);
    // A toolbar key: an icon over a small caption; lit keys are filled, an
    // unlit key with a fill of its own (PLAYLIST) shows it.
    auto tool = [&] (int i, const juce::String& caption, std::function<void (juce::Graphics&, juce::Rectangle<float>, juce::Colour)> icon, bool enabled,
                     bool lit = false, juce::Colour litColour = Colours::gold, juce::Colour ink = Colours::caption, juce::Colour fill = {})
    {
        const auto area = toolKeys[(size_t) i];
        if (area.isEmpty())
            return;
        auto r = area.toFloat();
        if (lit || ! fill.isTransparent())
        {
            g.setColour (lit ? litColour : fill);
            g.fillRect (r);
        }
        auto c = lit ? juce::Colours::black : (enabled ? ink : keyOffInk);
        auto label = r.removeFromBottom (r.getHeight() * 0.3f);
        icon (g, r.reduced (r.getHeight() * 0.1f), c);
        g.setColour (c);
        g.setFont (font (juce::jmin (label.getHeight() * 0.62f, getHeight() * 0.017f), false));
        g.drawFittedText (caption, label.toNearestInt().reduced (1, 0), juce::Justification::centredTop, 1, 0.7f);
    };
    auto shared = [] (Icon icon) { return [icon] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c) { drawIcon (gr, icon, r, c); }; };
    // [3.1] the stick drawn on its side.
    auto stick = [] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c)
    {
        juce::Graphics::ScopedSaveState state (gr);
        gr.addTransform (juce::AffineTransform::rotation (juce::MathConstants<float>::pi * 0.25f, r.getCentreX(), r.getCentreY()));
        drawIcon (gr, Icon::UsbStick, r, c);
    };
    auto enabledKey = [this] (int i)
    {
        for (auto& k : keys)
            if (k.area == toolKeys[(size_t) i])
                return ! k.enabled || k.enabled();
        return false;
    };
    // PLAYLIST, or the open playlist's number of clips.
    const int numSongs = u.isPlaylistOpen() ? (int) u.getQueue().size() : 0;
    const auto songs = ! u.isPlaylistOpen() ? juce::String ("PLAYLIST") : juce::String (numSongs) + (numSongs == 1 ? " SONG" : " SONGS");
    tool (0, "USB", stick, true, ! playlistView, Colours::gold);
    tool (1, songs, musicNotes, true, playlistView, Dark::choice, u.isPlaylistOpen() ? Dark::choice : playlistInk, juce::Colours::black);
    if (playlistView)
    {
        tool (2, "REPEAT", repeatArrows, enabledKey (2), u.getRepeat(), Colours::gold);
        tool (3, "REMOVE", shared (Icon::ClearCircle), enabledKey (3));
        const auto* row = selectedRow();
        tool (4, "SKIP", shared (Icon::Skip), enabledKey (4), row != nullptr && row->skip, flagYellow, flagYellow);
        tool (5, "STOP", stopCircle, enabledKey (5), row != nullptr && row->stop, flagYellow, flagYellow);
        tool (6, "UP", shared (Icon::ChevronUp), enabledKey (6));
        tool (7, "DOWN", shared (Icon::ChevronDown), enabledKey (7));
    }
    else
    {
        // [3.1] the browser's keys stay lit whatever is selected.
        const auto* row = selectedRow();
        const bool file = row != nullptr && row->kind == (int) UsbRecorder::ItemKind::Wav;
        tool (2, file ? "ADD" : "ADD ALL", circlePlus, true);
        tool (3, "FOLDER", folderPlus, true);
        tool (4, "RENAME", shared (Icon::Edit), true);
        tool (5, "DELETE", shared (Icon::Trash), true);
        tool (6, "REC PATH", shared (Icon::Folder), true);
    }
}

void UsbRecorderPage::paintList (juce::Graphics& g)
{
    auto& u = usb();
    const float sh = (float) getHeight();
    g.setColour (panelGrey);
    g.fillRect (listArea);
    const int gap = juce::jmax (1, rowHeight - fig (getLocalBounds(), 0, 0, 0, 50).getHeight()); // 6 px between rows
    const auto nameFont = font (sh * 0.0275f);
    // A folder's > key: its icon left of it.
    auto folderKeys = [&] (juce::Rectangle<int> inner, Icon arrow)
    {
        // [3.1] in a 50 px row: a 15 x 26 chevron drawn 3 px thick in the
        // 32 px key, the folder 34 x 29 ending 12 px left of the key, 10 px down.
        auto key = inner.removeFromRight (openKeyWidth());
        g.setColour (openKey);
        g.fillRect (key);
        const float wu = (float) inner.getHeight() / 50.0f;
        const auto kc = key.getCentre().toFloat();
        const float dx = (arrow == Icon::ChevronLeft ? -7.5f : 7.5f) * wu;
        juce::Path chevron;
        chevron.startNewSubPath (kc.x - dx, kc.y - 13.0f * wu);
        chevron.lineTo (kc.x + dx, kc.y);
        chevron.lineTo (kc.x - dx, kc.y + 13.0f * wu);
        g.setColour (folderBlue);
        g.strokePath (chevron, juce::PathStrokeType (juce::jmax (1.5f, 3.0f * wu)));
        auto icon = juce::Rectangle<float> ((float) key.getX() - 46.0f * wu, (float) inner.getY() + 10.0f * wu, 34.0f * wu, 29.0f * wu);
        drawIcon (g, Icon::Folder, icon.expanded (5.0f * wu), folderBlue); // (the glyph has a margin)
        inner.setRight ((int) icon.getX());
        return inner;
    };
    // In a folder of the stick: its row first, the key back up at its right.
    if (! listHeader.isEmpty())
    {
        auto inner = listHeader;
        g.setColour (juce::Colours::black);
        g.fillRect (inner);
        auto t = folderKeys (inner, Icon::ChevronLeft).withTrimmedLeft ((int) (sh * 0.015f));
        g.setColour (folderBlue);
        g.setFont (nameFont);
        g.drawFittedText (u.getBrowserFolder().getFileName().toUpperCase(), t, juce::Justification::centredLeft, 1, 0.7f);
    }

    // The rows.
    const int n = visibleRows();
    const auto current = u.getCurrentFile();
    const auto playState = u.getPlayState();
    const auto recFile = u.isRecording() ? u.getRecFile() : juce::File();
    for (int v = 0; v <= n + 1; ++v)
    {
        auto r = rowArea (v);
        if (r.getBottom() - gap > rowsArea.getBottom() + 1)
            break;
        const int index = scroll + v;
        if (! juce::isPositiveAndBelow (index, (int) rows.size()))
        {
            // [3.1] the empty rows under the list are black boxes too.
            g.setColour (juce::Colours::black);
            g.fillRect (r.withTrimmedBottom (gap));
            continue;
        }
        const auto& row = rows[(size_t) index];
        auto inner = r.withTrimmedBottom (gap);
        g.setColour (index == selected ? selectedFill : juce::Colours::black);
        g.fillRect (inner);
        if (index == selected)
        {
            g.setColour (Dark::selectedOutline.withAlpha (0.8f));
            g.drawRect (inner, 2);
        }
        const bool folder = row.kind == (int) UsbRecorder::ItemKind::Folder;
        auto t = folder ? folderKeys (inner, Icon::ChevronRight) : inner;
        t = t.withTrimmedLeft ((int) (sh * 0.015f)).withTrimmedRight ((int) (sh * 0.01f));
        // The clip playing (green) or cued (grey); the file being recorded (red).
        const bool isCurrent = row.file == current && current != juce::File();
        if (row.file == recFile)
            recordDot (g, t.removeFromLeft (t.getHeight() / 2).toFloat(), recRed);
        else if (isCurrent && ! folder)
        {
            auto tri = t.removeFromLeft (t.getHeight() / 2).toFloat().withSizeKeepingCentre ((float) t.getHeight() * 0.28f, (float) t.getHeight() * 0.34f);
            juce::Path p;
            p.addTriangle (tri.getX(), tri.getY(), tri.getRight(), tri.getCentreY(), tri.getX(), tri.getBottom());
            g.setColour (playState == UsbRecorder::PlayState::Playing ? playGreen : Colours::textDim);
            g.fillPath (p);
        }
        if (! folder)
        {
            // [3.1] the type chip, then the date over the time (grey) at the right.
            auto date = t.removeFromRight ((int) (sh * 0.13f));
            g.setColour (dateGrey);
            g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), sh * 0.024f, juce::Font::bold));
            g.drawText (row.modified.formatted ("%Y-%m-%d"), date.withHeight (date.getHeight() / 2), juce::Justification::centredRight, false);
            g.drawText (row.modified.formatted ("%H:%M:%S"), date.withTrimmedTop (date.getHeight() / 2), juce::Justification::centredRight, false);
            t.removeFromRight ((int) (sh * 0.008f));
            auto c = t.removeFromRight ((int) (sh * 0.067f)).toFloat();
            c = c.withHeight ((float) inner.getHeight() * 0.42f).withY ((float) inner.getY() + (float) inner.getHeight() * 0.06f);
            chip (g, c, row.kind == (int) UsbRecorder::ItemKind::Playlist ? juce::String ("M3U") : AudioFileFormat::typeName (AudioFileFormat::typeOfFile (row.file)),
                  row.kind == (int) UsbRecorder::ItemKind::Playlist ? listChip : wavChip, juce::Colours::black);
            // SKIP / STOP flags (playlist), left of the chip.
            const int flag = juce::jmin (t.getHeight() * 6 / 10, t.getWidth() / 4);
            if (row.stop)
                stopCircle (g, t.removeFromRight (flag).toFloat().withSizeKeepingCentre ((float) flag, (float) flag).reduced ((float) flag * 0.08f), flagYellow);
            if (row.skip)
                drawIcon (g, Icon::Skip, t.removeFromRight (flag).toFloat().withSizeKeepingCentre ((float) flag, (float) flag).reduced ((float) flag * 0.14f), flagYellow);
        }
        g.setColour (folder ? folderBlue : (row.skip ? Colours::textDim : Colours::caption));
        g.setFont (nameFont);
        g.drawFittedText (folder ? displayName (row.file).toUpperCase() : displayName (row.file), t, juce::Justification::centredLeft, 1, 0.75f);
    }
    if (rows.empty())
    {
        g.setColour (Colours::textDim);
        g.setFont (font (sh * 0.02f, false));
        const auto hint = playlistView ? (u.isPlaylistOpen() ? "The playlist is empty: ADD clips from the USB browser"
                                                             : "No playlist: CREATE PLAYLIST, or ADD a clip from the USB browser")
                                       : "No audio files or folders here";
        g.drawFittedText (hint, rowArea (0).reduced ((int) (sh * 0.02f), 0), juce::Justification::centredLeft, 2);
    }

    // The scroll bar: black arrows on the panel over a black track with the thumb.
    juce::Path up, down;
    auto ua = scrollUp.toFloat().withHeight ((float) scrollUp.getHeight() * 0.62f);
    up.addTriangle (ua.getCentreX(), ua.getY(), ua.getRight(), ua.getBottom(), ua.getX(), ua.getBottom());
    auto da = scrollDown.toFloat().withTrimmedTop ((float) scrollDown.getHeight() * 0.38f);
    down.addTriangle (da.getX(), da.getY(), da.getRight(), da.getY(), da.getCentreX(), da.getBottom());
    g.setColour (juce::Colours::black);
    g.fillPath (up);
    g.fillPath (down);
    auto track = scrollBar.withTrimmedTop (scrollUp.getHeight()).withTrimmedBottom (scrollDown.getHeight());
    g.fillRect (track);
    const int total = (int) rows.size();
    if (total > n)
    {
        track = track.reduced (scrollBar.getWidth() / 5, 2);
        const float h = (float) track.getHeight() * (float) n / (float) total;
        const float y = (float) track.getY() + ((float) track.getHeight() - h) * (float) scroll / (float) juce::jmax (1, total - n);
        g.setColour (juce::Colour (0xff3a3a3a));
        g.fillRect (juce::Rectangle<float> ((float) track.getX(), y, (float) track.getWidth(), h));
    }
}

void UsbRecorderPage::paintRecorder (juce::Graphics& g)
{
    auto& u = usb();
    const float sh = (float) getHeight();
    const auto state = u.getRecState();
    const bool fs = u.isFullSpeedOnly();
    const int channels = u.isRecording() ? juce::jmax (2, u.getRecChannels()) : (fs ? 2 : u.getRecChannels());
    g.setColour (panelGrey);
    g.fillRect (recBox);
    meterBank (g, recMeter.toFloat(), recLevels, channels);

    // The state light (a yellow square while stopped, red while recording,
    // blinking while paused) and the red time.
    const bool blinkOn = (juce::Time::getMillisecondCounter() / 400) % 2 == 0;
    const auto light = recLight.toFloat();
    if (state == UsbRecorder::RecState::Stopped)
    {
        g.setColour (stopYellow);
        g.fillRoundedRectangle (light, light.getHeight() * 0.12f);
    }
    else
    {
        recordDot (g, light.expanded (light.getWidth() * 0.3f), state == UsbRecorder::RecState::Error ? Colours::warning
                                                               : state == UsbRecorder::RecState::Paused && ! blinkOn ? recRed.withAlpha (0.25f)
                                                                                                                     : recRed);
    }
    sevenSegment (g, recTime.toFloat(), timeText (u.getRecTime(), u.getSampleRate()), timeRed, juce::Justification::left);
    reels (g, recReels.toFloat(), reelAngle[0], state == UsbRecorder::RecState::Recording);

    // 2-CH | 4-CH and 16 bit | 24 bit: pairs of keys in a black frame, the
    // chosen one blue with black ink.
    auto choice = [&] (juce::Rectangle<int> r, const juce::String& text, bool lit, bool enabled)
    {
        g.setColour (lit ? Dark::choice : keyGrey);
        g.fillRect (r);
        g.setColour (lit ? juce::Colours::black : (enabled ? Colours::caption : keyOffInk));
        g.setFont (font ((float) r.getHeight() * 0.4f));
        g.drawFittedText (text, r.reduced (2, 0), juce::Justification::centred, 1, 0.7f);
    };
    const bool mp3 = u.getRecFileType() == AudioFileFormat::Type::Mp3;
    const int fmtChannels = u.getEffectiveRecChannels(), fmtBits = fs ? 16 : u.getRecBits();
    g.setColour (juce::Colours::black);
    g.fillRect (channelFrame);
    choice (channelKeys[0], "2-CH", fmtChannels == 2, ! u.isRecording());
    choice (channelKeys[1], "4-CH", fmtChannels == 4, ! u.isRecording() && ! fs && ! mp3);
    // FORMAT and (for MP3) the bit rate: lists, drawn like the console's dropdown fields.
    auto list = [&] (juce::Rectangle<int> r, const juce::String& caption, const juce::String& value, bool enabled)
    {
        g.setColour (keyGrey);
        g.fillRect (r);
        auto inner = r.reduced (juce::jmax (2, r.getHeight() / 12));
        auto top = inner.removeFromTop (inner.getHeight() * 36 / 100);
        g.setColour (enabled ? Colours::caption : keyOffInk);
        g.setFont (font ((float) top.getHeight() * 0.9f, false));
        g.drawFittedText (caption, top, juce::Justification::centredLeft, 1, 0.7f);
        auto chevron = inner.removeFromRight (inner.getHeight()).toFloat().reduced ((float) inner.getHeight() * 0.18f);
        drawIcon (g, Icon::ChevronDown, chevron, enabled ? juce::Colours::white : keyOffInk);
        g.setColour (enabled ? juce::Colours::white : keyOffInk);
        g.setFont (font ((float) inner.getHeight() * 0.72f));
        g.drawFittedText (value, inner, juce::Justification::centredLeft, 1, 0.7f);
    };
    list (formatKey, "FORMAT", AudioFileFormat::typeName (u.getRecFileType()), ! u.isRecording());
    if (mp3)
        list (kbpsKey, "BIT RATE", juce::String (u.getRecKbps()) + " kbps", ! u.isRecording());
    else
    {
        g.setColour (juce::Colours::black);
        g.fillRect (bitFrame);
        choice (bitKeys[0], "16 bit", fmtBits == 16, ! u.isRecording());
        choice (bitKeys[1], "24 bit", fmtBits == 24, ! u.isRecording() && ! fs);
    }

    // The REC PATH (when not the stick's top) above the caption, the caption itself.
    const auto storage = u.getStorageFolder();
    auto rel = u.getRecPath().getRelativePathFrom (storage).replaceCharacter ('\\', '/');
    if (rel != "." && rel.isNotEmpty())
    {
        g.setColour (Colours::textDim);
        g.setFont (font (sh * 0.016f, false));
        g.drawFittedText ("/" + rel, recPathText, juce::Justification::centredRight, 1, 0.7f);
    }
    g.setColour (captionGrey);
    g.setFont (font (sh * 0.03f));
    g.drawFittedText ("4-TRACK RECORDER", recCaption, juce::Justification::centredRight, 1, 0.7f);

    // REC, PAUSE, STOP, NEW FILE.
    g.setColour (panelGrey);
    g.fillRect (recKeys);
    const bool recording = state == UsbRecorder::RecState::Recording;
    const bool paused = state == UsbRecorder::RecState::Paused;
    paintKey (g, recKeyAreas[0], "REC", [recording] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c)
    { recordDot (gr, r.expanded (r.getWidth() * 0.2f), recording ? recRed : c); }, ! recording);
    paintKey (g, recKeyAreas[1], "PAUSE", [] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c) { drawIcon (gr, Icon::Pause, r, c); },
              u.isRecording(), paused, Colours::gold);
    paintKey (g, recKeyAreas[2], "STOP", [] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c) { drawIcon (gr, Icon::Stop, r, c); },
              u.isRecording() || state == UsbRecorder::RecState::Error);
    paintKey (g, recKeyAreas[3], "NEW\nFILE", circlePlus, u.isRecording());
}

void UsbRecorderPage::paintPlaylistKeys (juce::Graphics& g)
{
    auto& u = usb();
    g.setColour (panelGrey);
    g.fillRect (playlistKeys);
    const auto* row = selectedRow();
    const bool canOpen = ! playlistView && row != nullptr && row->kind == (int) UsbRecorder::ItemKind::Playlist;
    // These are flat (no key body), the caption in two lines right of the icon.
    auto flat = [&] (juce::Rectangle<int> area, const juce::String& text, std::function<void (juce::Graphics&, juce::Rectangle<float>, juce::Colour)> icon, bool enabled)
    {
        auto r = area.toFloat();
        const auto ink = enabled ? Colours::caption : keyOffInk;
        auto content = r.reduced (r.getHeight() * 0.1f, r.getHeight() * 0.08f);
        icon (g, content.removeFromLeft (content.getHeight()).reduced (content.getHeight() * 0.1f), ink);
        g.setColour (ink);
        g.setFont (font (juce::jmin (content.getHeight() * 0.36f, (float) getHeight() * 0.024f)));
        g.drawFittedText (text, content.toNearestInt(), juce::Justification::centred, 2, 0.7f);
    };
    flat (playlistKeyAreas[0], "CREATE\nPLAYLIST", [] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c) { drawIcon (gr, Icon::Edit, r, c); }, true);
    flat (playlistKeyAreas[1], "OPEN\nPLAYLIST", [] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c) { boxArrow (gr, r, c, true); }, canOpen);
    flat (playlistKeyAreas[2], "SAVE\nPLAYLIST", [] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c) { drawIcon (gr, Icon::SaveTray, r, c); },
          u.isPlaylistOpen());
    flat (playlistKeyAreas[3], "CLOSE\nPLAYLIST", [] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c) { boxArrow (gr, r, c, false); },
          u.isPlaylistOpen());
}

bool UsbRecorderPage::currentFileInfo (UsbRecorder::FileInfo& info, int64_t& length, bool& mismatch) const
{
    auto& u = usb();
    length = u.getLength();
    mismatch = false;
    if (u.getCurrentInfo (info, length, mismatch))
        return true;

    // Not opened yet: the header of the cued (or selected) file.
    auto file = u.getCurrentFile();
    if (file == juce::File())
        if (const auto* row = selectedRow(); row != nullptr && row->kind == (int) UsbRecorder::ItemKind::Wav)
            file = row->file;
    // [3.1] a folder's clips wait for one to be chosen; a playlist cues its first.
    if (file == juce::File() && u.isPlaylistOpen())
        for (const auto& e : u.getQueue())
            if (! e.skip)
            {
                file = e.file;
                break;
            }
    if (file == juce::File() || ! UsbRecorder::readFileInfo (file, info))
        return false;
    mismatch = juce::roundToInt (u.getSampleRate()) != info.sampleRate;
    length = (int64_t) std::ceil ((double) info.frames * u.getSampleRate() / juce::jmax (1, info.sampleRate));
    return true;
}

void UsbRecorderPage::paintPlayer (juce::Graphics& g)
{
    auto& u = usb();
    const float sh = (float) getHeight();
    const auto state = u.getPlayState();
    UsbRecorder::FileInfo info;
    int64_t length = 0;
    bool mismatch = false;
    const bool known = currentFileInfo (info, length, mismatch);

    g.setColour (panelGrey);
    g.fillRect (playBox);
    meterBank (g, playMeter.toFloat(), playLevels, known ? juce::jlimit (2, 4, info.channels) : 4);

    // The state (green play, paused, a yellow square while stopped), the time
    // and under it the time left, smaller; both unlit without a clip.
    const auto light = playLight.toFloat();
    if (state == UsbRecorder::PlayState::Paused)
        drawIcon (g, Icon::Pause, light.expanded (light.getWidth() * 0.2f), Colours::gold);
    else if (state == UsbRecorder::PlayState::Error)
        drawIcon (g, Icon::Cancel, light.expanded (light.getWidth() * 0.2f), Colours::warning);
    else if (state == UsbRecorder::PlayState::Playing)
    {
        juce::Path p;
        auto tri = light.withSizeKeepingCentre (light.getWidth() * 0.8f, light.getHeight());
        p.addTriangle (tri.getX(), tri.getY(), tri.getRight(), tri.getCentreY(), tri.getX(), tri.getBottom());
        g.setColour (playGreen);
        g.fillPath (p);
    }
    else
    {
        g.setColour (stopYellow);
        g.fillRoundedRectangle (light, light.getHeight() * 0.12f);
    }
    const auto pos = u.getPosition();
    // [3.1] lit red even with no clip chosen (0 00 00 over 00 00 00).
    const float end = sevenSegment (g, playTime.toFloat(), timeText (pos, u.getSampleRate()), timeRed, juce::Justification::left);
    sevenSegment (g, playRest.toFloat().withRight (end), clockText (juce::jmax<int64_t> (0, length - pos), u.getSampleRate()), timeRed,
                  juce::Justification::right);
    reels (g, playReels.toFloat(), reelAngle[1], state == UsbRecorder::PlayState::Playing);

    // The format chips: WAV | 4-CH over 48K | 24 bit (the rate in orange when
    // resampled); four blank tiles without a clip.
    auto chips = playChips.toFloat();
    const float cw = chips.getWidth() * 0.48f, ch = chips.getHeight() * 0.47f;
    const juce::Rectangle<float> cells[] = { { chips.getX(), chips.getY(), cw, ch }, { chips.getRight() - cw, chips.getY(), cw, ch },
                                             { chips.getX(), chips.getBottom() - ch, cw, ch }, { chips.getRight() - cw, chips.getBottom() - ch, cw, ch } };
    if (! known)
    {
        for (auto& c : cells)
            chip (g, c, {}, chipGrey, juce::Colours::black);
    }
    else
    {
        const auto rate = info.sampleRate == 44100 ? juce::String ("44.1K") : juce::String (juce::roundToInt (info.sampleRate / 1000.0)) + "K";
        const auto depth = info.type == AudioFileFormat::Type::Mp3 ? juce::String (info.kbps) + "k" : juce::String (info.bits) + " bit";
        chip (g, cells[0], AudioFileFormat::typeName (info.type), chipGrey, juce::Colours::black);
        chip (g, cells[1], juce::String (info.channels) + "-CH", chipGrey, juce::Colours::black);
        chip (g, cells[2], rate, mismatch ? Dark::orange : chipGrey, juce::Colours::black);
        chip (g, cells[3], depth, chipGrey, juce::Colours::black);
    }

    g.setColour (captionGrey);
    g.setFont (font (sh * 0.03f));
    g.drawFittedText ("4-TRACK PLAYER", playCaption, juce::Justification::centredRight, 1, 0.7f);

    // PLAY, PAUSE, STOP, < >.
    const bool active = state == UsbRecorder::PlayState::Playing || state == UsbRecorder::PlayState::Paused;
    g.setColour (panelGrey);
    g.fillRect (playKeys);
    paintKey (g, playKeyAreas[0], "PLAY", [] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c) { drawIcon (gr, Icon::Play, r, c); },
              canPlay() && state != UsbRecorder::PlayState::Playing);
    paintKey (g, playKeyAreas[1], "PAUSE", [] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c) { drawIcon (gr, Icon::Pause, r, c); },
              active, state == UsbRecorder::PlayState::Paused, Colours::gold);
    paintKey (g, playKeyAreas[2], "STOP", [] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c) { drawIcon (gr, Icon::Stop, r, c); }, active);
    const bool any = canStep();
    paintKey (g, playKeyAreas[3], {}, [] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c) { drawIcon (gr, Icon::ChevronLeft, r, c); }, any);
    paintKey (g, playKeyAreas[4], {}, [] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c) { drawIcon (gr, Icon::ChevronRight, r, c); }, any);
}

void UsbRecorderPage::paintTimeline (juce::Graphics& g)
{
    auto& u = usb();
    const float sh = (float) getHeight();
    // [3.1] a #101010 box in a 2 px #585858 frame; empty without a clip.
    auto r = timeline.toFloat();
    g.setColour (chipGrey);
    g.fillRect (r);
    const float frame = juce::jmax (1.0f, r.getHeight() / 42.0f);
    r = r.reduced (frame);
    g.setColour (juce::Colour (0xff101010));
    g.fillRect (r);
    UsbRecorder::FileInfo info;
    int64_t length = 0;
    bool mismatch = false;
    if (! currentFileInfo (info, length, mismatch) || length <= 0)
        return;
    const auto pos = u.getPosition();
    const float frac = (float) juce::jlimit (0.0, 1.0, (double) pos / (double) length);
    const float x = r.getX() + frac * r.getWidth();
    g.setColour (timelineFill);
    g.fillRect (r.withRight (x));
    g.setColour (Colours::gold);
    g.fillRect (x - 1.0f, r.getY(), 2.0f, r.getHeight());
    // The position (top left) and the time left (top right).
    const float chipH = juce::jmax (10.0f, r.getHeight() * 0.18f);
    g.setFont (font (chipH * 0.62f, false));
    const auto posText = clockText (pos, u.getSampleRate());
    const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), posText) + chipH;
    auto posChip = juce::Rectangle<float> (r.getX() + sh * 0.006f, r.getY() + sh * 0.008f, w, chipH);
    g.setColour (juce::Colours::black.withAlpha (0.85f));
    g.fillRect (posChip);
    g.setColour (Colours::caption);
    g.drawText (posText, posChip, juce::Justification::centred, false);
    g.drawText ("-" + clockText (juce::jmax<int64_t> (0, length - pos), u.getSampleRate()), r.reduced (sh * 0.008f).withHeight (chipH), juce::Justification::centredRight,
                false);
}

//==============================================================================
void UsbRecorderPage::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();
    pressedKey = -1;
    draggingTimeline = false;
    if (timeline.contains (p))
    {
        // The cursor moves while paused or stopped (manual 8.2).
        if (usb().getPlayState() != UsbRecorder::PlayState::Playing)
        {
            draggingTimeline = true;
            seekFromTimeline (p.x);
        }
        return;
    }
    for (size_t i = 0; i < keys.size(); ++i)
        if (keys[i].area.contains (p))
        {
            if (keys[i].enabled && ! keys[i].enabled())
                return;
            pressedKey = (int) i;
            pressTime = juce::Time::getMillisecondCounter();
            if (e.mods.isPopupMenu() && keys[i].onHold)
            {
                pressedKey = -1;
                auto f = keys[i].onHold;
                f();
            }
            return;
        }
    if (const int row = rowAt (p); row >= 0)
    {
        select (row);
        if (rows[(size_t) row].kind == (int) UsbRecorder::ItemKind::Folder && p.x >= rowsArea.getRight() - openKeyWidth())
            openFolder (rows[(size_t) row].file);
    }
}

void UsbRecorderPage::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingTimeline)
        seekFromTimeline (e.x);
}

void UsbRecorderPage::mouseUp (const juce::MouseEvent& e)
{
    draggingTimeline = false;
    if (! juce::isPositiveAndBelow (pressedKey, (int) keys.size()))
        return;
    const auto key = keys[(size_t) pressedKey]; // a copy: the action may rebuild the keys
    pressedKey = -1;
    if (! key.area.contains (e.getPosition()))
        return;
    const bool held = juce::Time::getMillisecondCounter() - pressTime >= 500;
    if (held && key.onHold)
        key.onHold();
    else if (key.onClick)
        key.onClick();
    repaint();
}

void UsbRecorderPage::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int row = rowAt (e.getPosition());
    if (row < 0)
        return;
    const auto r = rows[(size_t) row];
    if (r.kind == (int) UsbRecorder::ItemKind::Folder)
        openFolder (r.file);
    else if (r.kind == (int) UsbRecorder::ItemKind::Playlist)
    {
        warn ("OPEN PLAYLIST", usb().openPlaylist (r.file));
        playlistView = true;
        selectedFile = juce::File();
        reload();
    }
    else if (playlistView)
        usb().playEntry (r.entry, true);
    else
        usb().playFile (r.file);
}

void UsbRecorderPage::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (leftPanel.contains (e.getPosition()))
        scrollBy (wheel.deltaY > 0 ? -1 : 1);
}

} // namespace livedaw::ui
