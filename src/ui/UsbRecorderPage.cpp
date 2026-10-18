#include "ui/UsbRecorderPage.h"
#include "ui/ScreenWidgets.h"
#include "ui/WingPopups.h"

namespace livedaw::ui
{

namespace
{
    // fig. 78's colours [manual p. 87].
    const juce::Colour panelGrey { 0xff202020 };
    const juce::Colour keyGrey { 0xff333333 };
    const juce::Colour keyLine { 0xff4a4a4a };
    const juce::Colour rowLine { 0xff1c1c1c };
    const juce::Colour selectedFill { 0xff342d12 };
    const juce::Colour wavChip { 0xffa6c87a };
    const juce::Colour listChip { 0xff6ca0dc };
    const juce::Colour timeRed { 0xfff0453a };
    const juce::Colour recRed { 0xfff54d42 };
    const juce::Colour playGreen { 0xff1dee60 };
    const juce::Colour captionGrey { 0xff3c3c3c };
    const juce::Colour chipGrey { 0xff3b3b3b };
    const juce::Colour formatOff { 0xff59595b };
    const juce::Colour reelGrey { 0xff949494 };
    const juce::Colour timelineFill { 0xff7f6a2f };
    const juce::Colour titleBlue { 0xff448ad7 };
    const juce::Colour flagYellow { 0xffe8d020 };

    constexpr float kFigW = 1560.0f, kFigTop = 100.0f, kFigH = 878.0f;

    /** A rectangle given in fig. 78's coordinates (1560 x 978, the status bar
        above y = 100), scaled to the page. */
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

    /** A seven-segment readout (the WING's red time). */
    float sevenSegment (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text, juce::Colour on, juce::Justification just)
    {
        static const int masks[] = { 0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f };
        const float h = area.getHeight(), w = h * 0.5f, gap = h * 0.16f, colonW = h * 0.2f, t = juce::jmax (1.5f, h * 0.095f);
        float total = 0.0f;
        for (auto ch : text)
            total += (ch == ':' ? colonW : w) + gap;
        total -= gap;
        float x = just.testFlags (juce::Justification::right) ? area.getRight() - total
                  : just.testFlags (juce::Justification::left) ? area.getX()
                                                               : area.getCentreX() - total * 0.5f;
        const float y = area.getY();
        g.setColour (on);
        // A slight slant, as on the console.
        const auto slant = juce::AffineTransform::shear (-0.08f, 0.0f).translated (0.08f * (y + h), 0.0f);
        for (auto ch : text)
        {
            if (ch == ':')
            {
                const float d = t * 1.1f;
                juce::Path dots;
                dots.addEllipse (x + colonW * 0.5f - d * 0.5f, y + h * 0.3f - d * 0.5f, d, d);
                dots.addEllipse (x + colonW * 0.5f - d * 0.5f, y + h * 0.72f - d * 0.5f, d, d);
                g.fillPath (dots, slant);
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
            auto horiz = [&] (float yy) { p.addRoundedRectangle (x + t * 0.6f, yy - t * 0.5f, w - t * 1.2f, t, t * 0.4f); };
            auto vert = [&] (float xx, float y0, float y1) { p.addRoundedRectangle (xx - t * 0.5f, y0 + t * 0.6f, t, y1 - y0 - t * 1.2f, t * 0.4f); };
            if (mask & 0x01) horiz (y + t * 0.5f);
            if (mask & 0x02) vert (x + w - t * 0.5f, y, y + m);
            if (mask & 0x04) vert (x + w - t * 0.5f, y + m, y + h);
            if (mask & 0x08) horiz (y + h - t * 0.5f);
            if (mask & 0x10) vert (x + t * 0.5f, y + m, y + h);
            if (mask & 0x20) vert (x + t * 0.5f, y, y + m);
            if (mask & 0x40) horiz (y + m);
            g.fillPath (p, slant);
            x += w + gap;
        }
        return x - gap;
    }

    /** Two tape reels with the tape between them; `angle` turns the spokes. */
    void reels (juce::Graphics& g, juce::Rectangle<float> area, double angle, bool running)
    {
        const float d = juce::jmin (area.getHeight() * 0.86f, area.getWidth() * 0.46f);
        const float gapX = area.getWidth() * 0.02f;
        const float cx1 = area.getCentreX() - d * 0.5f - gapX, cx2 = area.getCentreX() + d * 0.5f + gapX, cy = area.getY() + d * 0.5f;
        const float t = juce::jmax (1.2f, d * 0.035f);
        const auto ink = running ? reelGrey.brighter (0.25f) : reelGrey;
        for (const float cx : { cx1, cx2 })
        {
            g.setColour (ink);
            g.drawEllipse (juce::Rectangle<float> (d, d).withCentre ({ cx, cy }), t);
            g.drawEllipse (juce::Rectangle<float> (d * 0.62f, d * 0.62f).withCentre ({ cx, cy }), t * 0.8f);
            g.fillEllipse (juce::Rectangle<float> (d * 0.18f, d * 0.18f).withCentre ({ cx, cy }));
            for (int k = 0; k < 3; ++k)
            {
                const auto a = (float) (angle + k * juce::MathConstants<double>::twoPi / 3.0);
                g.drawLine (cx + std::cos (a) * d * 0.12f, cy + std::sin (a) * d * 0.12f, cx + std::cos (a) * d * 0.31f, cy + std::sin (a) * d * 0.31f,
                            t * 1.4f);
            }
        }
        // The tape along the bottom, over a guide roller.
        const float by = cy + d * 0.5f;
        g.setColour (ink.withAlpha (0.8f));
        g.drawLine (cx1, by, cx2, by, t * 0.8f);
        g.setColour (juce::Colours::white);
        const float roller = d * 0.09f;
        g.fillEllipse (juce::Rectangle<float> (roller, roller).withCentre ({ area.getCentreX() + d * 0.15f, by }));
        g.setColour (ink);
        g.fillEllipse (juce::Rectangle<float> (roller * 0.8f, roller * 0.8f).withCentre ({ cx1 + d * 0.2f, by }));
        g.fillEllipse (juce::Rectangle<float> (roller * 0.8f, roller * 0.8f).withCentre ({ cx2 - d * 0.2f, by }));
    }

    /** The console's meter scale: -60, -48, -36, -24, -12, -6, 0 dB evenly spaced (0..1). */
    float meterPos (float db)
    {
        static constexpr float marks[] = { -60.0f, -48.0f, -36.0f, -24.0f, -12.0f, -6.0f, 0.0f };
        if (db <= marks[0])
            return 0.0f;
        for (int i = 1; i < 7; ++i)
            if (db <= marks[i])
                return ((float) (i - 1) + (db - marks[i - 1]) / (marks[i] - marks[i - 1])) / 6.0f;
        return 1.0f;
    }

    /** The meter bank: CLIP on top, a scale, one LED bar per channel. */
    void meterBank (juce::Graphics& g, juce::Rectangle<float> area, const std::array<float, 4>& levels, int channels)
    {
        g.setColour (juce::Colours::black);
        g.fillRect (area);
        auto r = area.reduced (area.getWidth() * 0.05f, area.getHeight() * 0.04f);
        const float capPx = juce::jmax (6.0f, r.getHeight() * 0.055f);
        g.setColour (Colours::textDim);
        g.setFont (font (capPx));
        auto top = r.removeFromTop (capPx * 1.6f);
        g.drawText ("CLIP", top.withWidth (r.getWidth() * 0.4f), juce::Justification::centredLeft, false);
        auto scale = r.removeFromLeft (r.getWidth() * 0.22f);
        // The CLIP row, then 18 segments (3 per scale step).
        constexpr int kSegments = 18;
        const float clipH = r.getHeight() / (float) (kSegments + 2);
        auto clipRow = r.removeFromTop (clipH * 1.4f);
        const float segH = r.getHeight() / (float) kSegments;
        const float marks[] = { 0.0f, -6.0f, -12.0f, -24.0f, -36.0f, -48.0f };
        g.setFont (font (juce::jmax (5.0f, segH * 1.1f)));
        for (float db : marks)
        {
            const float y = r.getBottom() - meterPos (db) * r.getHeight();
            g.drawText (juce::String ((int) db), scale.withY (y - segH).withHeight (segH * 2.0f).reduced (1.0f, 0.0f), juce::Justification::centredRight,
                        false);
        }
        const int n = juce::jlimit (1, 4, channels);
        const float barW = r.getWidth() / 4.0f;
        auto bars = r.withSizeKeepingCentre (barW * (float) n, r.getHeight());
        auto clips = clipRow.withSizeKeepingCentre (barW * (float) n, clipRow.getHeight());
        for (int ch = 0; ch < n; ++ch)
        {
            auto bar = bars.removeFromLeft (barW).reduced (barW * 0.1f, 0.0f);
            auto clipLed = clips.removeFromLeft (barW).reduced (barW * 0.1f, clipRow.getHeight() * 0.22f);
            const float pos = meterPos (levels[(size_t) ch]);
            const bool clipped = levels[(size_t) ch] >= -0.1f;
            g.setColour (clipped ? Colours::meterRed : Colours::meterRed.withAlpha (0.15f));
            g.fillRect (clipLed);
            for (int seg = 0; seg < kSegments; ++seg)
            {
                auto box = juce::Rectangle<float> (bar.getX(), bar.getBottom() - (float) (seg + 1) * segH, bar.getWidth(), segH).reduced (0.0f, segH * 0.16f);
                const bool lit = levels[(size_t) ch] > -59.5f && pos > ((float) seg + 0.5f) / (float) kSegments;
                const auto colour = seg >= kSegments - 3 ? Colours::meterYellow : Colours::meterGreen;
                g.setColour (lit ? colour : colour.withAlpha (0.12f));
                g.fillRect (box);
            }
        }
    }

    void chip (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, juce::Colour fill, juce::Colour ink)
    {
        g.setColour (fill);
        g.fillRect (r);
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
    t.text = file != juce::File() ? displayName (file) : juce::String ("USB");
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
        return;
    if (row->kind == (int) UsbRecorder::ItemKind::Folder)
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
    // Live DAW's "USB stick" is a folder: hold USB (or right-click) to move it.
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

    leftPanel = f (15, 105, 800, 945);
    toolbar = f (15, 105, 800, 183);
    listArea = f (15, 192, 800, 945);
    scrollBar = f (752, 195, 798, 945);
    scrollUp = f (752, 195, 798, 228);
    scrollDown = f (752, 912, 798, 945);
    if (playlistView)
    {
        listHeader = {};
        rowsArea = f (15, 195, 748, 945);
    }
    else
    {
        listHeader = f (15, 195, 748, 262);
        rowsArea = f (15, 265, 748, 945);
    }
    headerFolderKey = f (640, 200, 700, 258);
    headerOpenKey = f (704, 200, 744, 258);
    rowHeight = juce::jmax (18, f (0, 265, 10, 332).getHeight());

    // The toolbar keys (fig. 78 for the browser, fig. 79 for the playlist).
    const float browserX[7][2] = { { 18, 90 }, { 96, 168 }, { 198, 270 }, { 328, 400 }, { 408, 480 }, { 558, 630 }, { 706, 786 } };
    const float playlistX[8][2] = { { 31, 92 }, { 108, 178 }, { 215, 272 }, { 342, 399 }, { 495, 549 }, { 569, 626 }, { 646, 702 }, { 722, 780 } };
    for (size_t i = 0; i < toolKeys.size(); ++i)
    {
        if (playlistView)
            toolKeys[i] = f (playlistX[i][0], 108, playlistX[i][1], 180);
        else
            toolKeys[i] = i < 7 ? f (browserX[i][0], 108, browserX[i][1], 180) : juce::Rectangle<int>();
    }

    // Right: the recorder, the playlist keys, the player, the timeline.
    recBox = f (810, 105, 1545, 308);
    recMeter = f (815, 110, 948, 303);
    recTime = f (962, 112, 1250, 168);
    recReels = f (962, 180, 1170, 303);
    channelKeys = { f (1383, 115, 1456, 170), f (1462, 115, 1535, 170) };
    bitKeys = { f (1383, 184, 1456, 240), f (1462, 184, 1535, 240) };
    formatKey = f (1180, 184, 1376, 240);
    kbpsKey = bitKeys[0].getUnion (bitKeys[1]);
    recPathText = f (1180, 244, 1538, 266);
    recCaption = f (1180, 266, 1540, 306);
    recKeys = f (810, 318, 1545, 413);
    playlistKeys = f (810, 423, 1545, 498);
    playBox = f (810, 508, 1545, 713);
    playMeter = f (815, 513, 948, 708);
    playTime = f (962, 515, 1250, 602);
    playReels = f (962, 592, 1170, 708);
    playChips = f (1402, 535, 1530, 608);
    playCaption = f (1180, 670, 1540, 710);
    playKeys = f (810, 721, 1545, 816);
    timeline = f (815, 826, 1540, 940);
    const float keyX[4][2] = { { 815, 995 }, { 1002, 1180 }, { 1187, 1363 }, { 1370, 1540 } };
    for (size_t i = 0; i < 4; ++i)
    {
        recKeyAreas[i] = f (keyX[i][0], 325, keyX[i][1], 408);
        playlistKeyAreas[i] = f (keyX[i][0], 428, keyX[i][1], 494);
    }
    for (size_t i = 0; i < 3; ++i)
        playKeyAreas[i] = f (keyX[i][0], 728, keyX[i][1], 811);
    playKeyAreas[3] = f (1370, 728, 1452, 811);
    playKeyAreas[4] = f (1459, 728, 1540, 811);

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
        addKey (toolKeys[2], [self] { self->addSelected(); }, [isKind] { return isKind (UsbRecorder::ItemKind::Wav) || isKind (UsbRecorder::ItemKind::Folder); });
        addKey (toolKeys[3], [self] { self->newFolder(); });
        addKey (toolKeys[4], [self] { self->renameSelected(); }, [self] { return self->selected >= 0; });
        addKey (toolKeys[5], [self] { self->deleteSelected(); }, [self] { return self->selected >= 0; });
        addKey (toolKeys[6], [self] { self->setRecPath(); });
        addKey (headerFolderKey, [self] { self->openFolder (self->usb().getBrowserFolder().getParentDirectory()); },
                [self] { return self->usb().getBrowserFolder() != self->usb().getStorageFolder(); });
        addKey (headerOpenKey, [self, isKind]
        {
            if (isKind (UsbRecorder::ItemKind::Folder))
                self->openFolder (self->selectedRow()->file);
        }, [isKind] { return isKind (UsbRecorder::ItemKind::Folder); });
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
    addKey (playKeyAreas[0], [self] { self->playSelected(); });
    addKey (playKeyAreas[1], [&u] { u.togglePlayPause(); },
            [&u] { return u.getPlayState() == UsbRecorder::PlayState::Playing || u.getPlayState() == UsbRecorder::PlayState::Paused; });
    addKey (playKeyAreas[2], [&u] { u.stop(); });
    addKey (playKeyAreas[3], [&u] { u.previous(); }, [&u] { return ! u.getQueue().empty(); });
    addKey (playKeyAreas[4], [&u] { u.next(); }, [&u] { return ! u.getQueue().empty(); });
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
    auto r = area.toFloat();
    g.setColour (lit ? litColour : keyGrey);
    g.fillRect (r);
    g.setColour (lit ? litColour.darker (0.3f) : keyLine);
    g.drawRect (r, 1.2f);
    auto ink = lit ? juce::Colours::black : Colours::text;
    if (! enabled)
        ink = Colours::textDim.withAlpha (0.55f);
    auto content = r.reduced (r.getHeight() * 0.12f);
    if (icon)
    {
        const float s = text.isEmpty() ? juce::jmin (content.getWidth(), content.getHeight()) * 0.6f : content.getHeight() * 0.62f;
        auto iconBox = text.isEmpty() ? content.withSizeKeepingCentre (s, s) : content.removeFromLeft (content.getHeight()).withSizeKeepingCentre (s, s);
        icon (g, iconBox, ink);
    }
    if (text.isNotEmpty())
    {
        const int lines = juce::StringArray::fromLines (text).size();
        g.setColour (ink);
        g.setFont (font (juce::jmin (content.getHeight() * (lines > 1 ? 0.28f : 0.34f), getHeight() * 0.024f)));
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
    // A toolbar key: an icon over a small caption; lit keys are filled.
    auto tool = [&] (int i, const juce::String& caption, std::function<void (juce::Graphics&, juce::Rectangle<float>, juce::Colour)> icon, bool enabled,
                     bool lit = false, juce::Colour litColour = Colours::gold, juce::Colour ink = Colours::text)
    {
        const auto area = toolKeys[(size_t) i];
        if (area.isEmpty())
            return;
        auto r = area.toFloat();
        if (lit)
        {
            g.setColour (litColour);
            g.fillRect (r);
        }
        auto c = lit ? juce::Colours::black : ink;
        if (! enabled)
            c = c.withAlpha (0.3f);
        auto label = r.removeFromBottom (r.getHeight() * 0.3f);
        icon (g, r.reduced (r.getHeight() * 0.14f), c);
        g.setColour (lit ? juce::Colours::black : (enabled ? Colours::caption : Colours::textDim.withAlpha (0.5f)));
        if (ink != Colours::text && ! lit && enabled)
            g.setColour (ink);
        g.setFont (font (juce::jmin (label.getHeight() * 0.62f, getHeight() * 0.017f), false));
        g.drawFittedText (caption, label.toNearestInt().reduced (1, 0), juce::Justification::centredTop, 1, 0.7f);
    };
    auto shared = [] (Icon icon) { return [icon] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c) { drawIcon (gr, icon, r, c); }; };
    auto enabledKey = [this] (int i)
    {
        for (auto& k : keys)
            if (k.area == toolKeys[(size_t) i])
                return ! k.enabled || k.enabled();
        return false;
    };
    const auto songs = u.isPlaylistOpen() ? juce::String ((int) u.getQueue().size()) + " SONGS" : juce::String ("0 SONGS");
    tool (0, "USB", shared (Icon::UsbStick), true, ! playlistView, Colours::gold);
    tool (1, songs, musicNotes, true, playlistView || u.isPlaylistOpen(), playlistView ? Dark::choice : Dark::choice.withAlpha (0.55f));
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
        const auto* row = selectedRow();
        const bool folder = row != nullptr && row->kind == (int) UsbRecorder::ItemKind::Folder;
        tool (2, folder ? "ADD ALL" : "ADD", circlePlus, enabledKey (2));
        tool (3, "FOLDER", folderPlus, true);
        tool (4, "RENAME", shared (Icon::Edit), enabledKey (4));
        tool (5, "DELETE", shared (Icon::Trash), enabledKey (5));
        tool (6, "REC PATH", shared (Icon::Folder), true);
    }
}

void UsbRecorderPage::paintList (juce::Graphics& g)
{
    auto& u = usb();
    const float sh = (float) getHeight();
    // The folder row: its name, the up key (folder) and the open key (>).
    if (! listHeader.isEmpty())
    {
        g.setColour (rowLine);
        g.fillRect (listHeader.getX(), listHeader.getBottom(), listHeader.getWidth(), 2);
        const auto folder = u.getBrowserFolder();
        const bool top = folder == u.getStorageFolder();
        g.setColour (Colours::text);
        g.setFont (font (sh * 0.028f));
        g.drawFittedText (top ? juce::String ("USB") : folder.getFileName().toUpperCase(), listHeader.withRight (headerFolderKey.getX()).reduced (sh * 0.02f, 0),
                          juce::Justification::centredLeft, 1, 0.7f);
        drawIcon (g, Icon::Folder, headerFolderKey.toFloat().reduced (headerFolderKey.getHeight() * 0.18f), top ? Colours::textDim : Colours::text);
        const auto* row = selectedRow();
        const bool canOpen = row != nullptr && row->kind == (int) UsbRecorder::ItemKind::Folder;
        g.setColour (canOpen ? Dark::choice : Dark::choice.withAlpha (0.35f));
        g.fillRect (headerOpenKey);
        drawIcon (g, Icon::ChevronRight, headerOpenKey.toFloat().reduced (headerOpenKey.getWidth() * 0.2f), juce::Colours::white.withAlpha (canOpen ? 1.0f : 0.5f));
    }

    // The rows.
    const int n = visibleRows();
    const auto current = u.getCurrentFile();
    const auto playState = u.getPlayState();
    const auto recFile = u.isRecording() ? u.getRecFile() : juce::File();
    for (int v = 0; v <= n; ++v)
    {
        auto r = rowArea (v);
        if (r.getBottom() > rowsArea.getBottom() + 1)
            break;
        g.setColour (rowLine);
        g.fillRect (r.getX(), r.getBottom() - 2, r.getWidth(), 2);
        const int index = scroll + v;
        if (! juce::isPositiveAndBelow (index, (int) rows.size()))
            continue;
        const auto& row = rows[(size_t) index];
        auto inner = r.withTrimmedBottom (2).reduced (2, 1);
        if (index == selected)
        {
            g.setColour (selectedFill);
            g.fillRect (inner);
            g.setColour (Dark::selectedOutline.withAlpha (0.8f));
            g.drawRect (inner, 2);
        }
        auto t = inner.reduced ((int) (sh * 0.014f), 0);
        // The clip playing (green) or cued (grey); the file being recorded (red).
        const bool isCurrent = row.file == current && current != juce::File();
        if (row.file == recFile)
            recordDot (g, t.removeFromLeft (t.getHeight() / 2).toFloat(), recRed);
        else if (isCurrent && row.kind != (int) UsbRecorder::ItemKind::Folder)
        {
            auto tri = t.removeFromLeft (t.getHeight() / 2).toFloat().withSizeKeepingCentre ((float) t.getHeight() * 0.28f, (float) t.getHeight() * 0.34f);
            juce::Path p;
            p.addTriangle (tri.getX(), tri.getY(), tri.getRight(), tri.getCentreY(), tri.getX(), tri.getBottom());
            g.setColour (playState == UsbRecorder::PlayState::Playing ? playGreen : Colours::textDim);
            g.fillPath (p);
        }
        if (row.kind == (int) UsbRecorder::ItemKind::Folder)
        {
            drawIcon (g, Icon::Folder, t.removeFromLeft (t.getHeight()).toFloat().reduced ((float) t.getHeight() * 0.22f), Colours::caption);
            t.removeFromLeft ((int) (sh * 0.006f));
        }
        // The date and time (dim) and the type chip at the right.
        auto right = t.removeFromRight ((int) (t.getWidth() * 0.32f));
        auto date = right.removeFromRight ((int) (right.getWidth() * 0.56f));
        g.setColour (Colours::textDim);
        g.setFont (font (sh * 0.016f, false));
        g.drawText (row.modified.formatted ("%Y-%m-%d"), date.removeFromTop (date.getHeight() / 2).withTrimmedTop (date.getHeight() / 10), juce::Justification::bottomRight,
                    false);
        g.drawText (row.modified.formatted ("%H:%M:%S"), date.withTrimmedBottom (date.getHeight() / 5), juce::Justification::topRight, false);
        if (row.kind != (int) UsbRecorder::ItemKind::Folder)
        {
            auto c = right.removeFromRight ((int) (sh * 0.081f)).withTrimmedRight ((int) (sh * 0.006f)).toFloat();
            c = c.withHeight ((float) inner.getHeight() * 0.36f).withY ((float) inner.getY() + (float) inner.getHeight() * 0.08f);
            chip (g, c, row.kind == (int) UsbRecorder::ItemKind::Playlist ? juce::String ("M3U") : AudioFileFormat::typeName (AudioFileFormat::typeOfFile (row.file)),
                  row.kind == (int) UsbRecorder::ItemKind::Playlist ? listChip : wavChip, juce::Colours::black);
        }
        // SKIP / STOP flags (playlist), left of the chip.
        const int flag = juce::jmin (right.getHeight() * 6 / 10, right.getWidth() / 2);
        if (row.stop)
            stopCircle (g, right.removeFromRight (flag).toFloat().withSizeKeepingCentre ((float) flag, (float) flag).reduced ((float) flag * 0.08f), flagYellow);
        if (row.skip)
            drawIcon (g, Icon::Skip, right.removeFromRight (flag).toFloat().withSizeKeepingCentre ((float) flag, (float) flag).reduced ((float) flag * 0.14f), flagYellow);
        g.setColour (row.skip ? Colours::textDim : Colours::text);
        g.setFont (font (sh * 0.028f));
        g.drawFittedText (displayName (row.file), t, juce::Justification::centredLeft, 1, 0.75f);
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

    // The scroll bar: up / down arrows and the thumb.
    g.setColour (juce::Colour (0xff0a0a0a));
    g.fillRect (scrollBar);
    juce::Path up, down;
    auto ua = scrollUp.toFloat().reduced ((float) scrollUp.getWidth() * 0.22f, (float) scrollUp.getHeight() * 0.25f);
    up.addTriangle (ua.getCentreX(), ua.getY(), ua.getRight(), ua.getBottom(), ua.getX(), ua.getBottom());
    auto da = scrollDown.toFloat().reduced ((float) scrollDown.getWidth() * 0.22f, (float) scrollDown.getHeight() * 0.25f);
    down.addTriangle (da.getX(), da.getY(), da.getRight(), da.getY(), da.getCentreX(), da.getBottom());
    g.setColour (juce::Colour (0xff2a2a2a));
    g.fillPath (up);
    g.fillPath (down);
    const int total = (int) rows.size();
    if (total > n)
    {
        auto track = scrollBar.withTrimmedTop (scrollUp.getHeight()).withTrimmedBottom (scrollDown.getHeight()).reduced (scrollBar.getWidth() / 4, 2);
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

    // The state light and the red time.
    auto t = recTime.toFloat();
    auto light = t.removeFromLeft (t.getHeight());
    const bool blinkOn = (juce::Time::getMillisecondCounter() / 400) % 2 == 0;
    const auto lightColour = state == UsbRecorder::RecState::Recording ? recRed
                             : state == UsbRecorder::RecState::Paused  ? (blinkOn ? recRed : recRed.withAlpha (0.25f))
                             : state == UsbRecorder::RecState::Error   ? Colours::warning
                                                                       : juce::Colour (0xff4a2020);
    recordDot (g, light, lightColour);
    t.removeFromLeft (t.getHeight() * 0.25f);
    sevenSegment (g, t.withHeight (t.getHeight() * 0.86f).withY (t.getY() + t.getHeight() * 0.07f), timeText (u.getRecTime(), u.getSampleRate()),
                  u.isRecording() ? timeRed : timeRed.withAlpha (0.45f), juce::Justification::left);
    reels (g, recReels.toFloat(), reelAngle[0], state == UsbRecorder::RecState::Recording);

    // 2-CH | 4-CH and 16 bit | 24 bit.
    auto choice = [&] (juce::Rectangle<int> r, const juce::String& text, bool lit, bool enabled)
    {
        g.setColour (lit ? Dark::choice : formatOff);
        g.fillRect (r);
        g.setColour (lit ? juce::Colours::white : (enabled ? juce::Colour (0xffd0d0d0) : juce::Colour (0xff8a8a8a)));
        g.setFont (font ((float) r.getHeight() * 0.36f));
        g.drawFittedText (text, r.reduced (2, 0), juce::Justification::centred, 1, 0.7f);
    };
    const bool mp3 = u.getRecFileType() == AudioFileFormat::Type::Mp3;
    const int fmtChannels = u.getEffectiveRecChannels(), fmtBits = fs ? 16 : u.getRecBits();
    choice (channelKeys[0], "2-CH", fmtChannels == 2, ! u.isRecording());
    choice (channelKeys[1], "4-CH", fmtChannels == 4, ! u.isRecording() && ! fs && ! mp3);
    // FORMAT and (for MP3) the bit rate: lists, drawn like the console's dropdown fields.
    auto list = [&] (juce::Rectangle<int> r, const juce::String& caption, const juce::String& value, bool enabled)
    {
        g.setColour (formatOff);
        g.fillRect (r);
        auto inner = r.reduced (juce::jmax (2, r.getHeight() / 12));
        auto top = inner.removeFromTop (inner.getHeight() * 36 / 100);
        g.setColour (enabled ? juce::Colour (0xffd0d0d0) : juce::Colour (0xff8a8a8a));
        g.setFont (font ((float) top.getHeight() * 0.9f, false));
        g.drawFittedText (caption, top, juce::Justification::centredLeft, 1, 0.7f);
        auto chevron = inner.removeFromRight (inner.getHeight()).toFloat().reduced ((float) inner.getHeight() * 0.18f);
        drawIcon (g, Icon::ChevronDown, chevron, enabled ? juce::Colours::white : juce::Colour (0xff8a8a8a));
        g.setColour (enabled ? juce::Colours::white : juce::Colour (0xff8a8a8a));
        g.setFont (font ((float) inner.getHeight() * 0.72f));
        g.drawFittedText (value, inner, juce::Justification::centredLeft, 1, 0.7f);
    };
    list (formatKey, "FORMAT", AudioFileFormat::typeName (u.getRecFileType()), ! u.isRecording());
    if (mp3)
        list (kbpsKey, "BIT RATE", juce::String (u.getRecKbps()) + " kbps", ! u.isRecording());
    else
    {
        choice (bitKeys[0], "16 bit", fmtBits == 16, ! u.isRecording());
        choice (bitKeys[1], "24 bit", fmtBits == 24, ! u.isRecording() && ! fs);
    }

    // The REC PATH above the caption, the caption itself.
    const auto storage = u.getStorageFolder();
    auto rel = u.getRecPath().getRelativePathFrom (storage).replaceCharacter ('\\', '/');
    if (rel == "." || rel.isEmpty())
        rel = {};
    g.setColour (Colours::textDim);
    g.setFont (font (sh * 0.016f, false));
    g.drawFittedText ("/" + rel, recPathText, juce::Justification::centredRight, 1, 0.7f);
    g.setColour (captionGrey);
    g.setFont (font (sh * 0.034f));
    g.drawFittedText ("4-TRACK RECORDER", recCaption, juce::Justification::centredRight, 1, 0.7f);

    // REC, PAUSE, STOP, NEW FILE.
    g.setColour (panelGrey);
    g.fillRect (recKeys);
    const bool recording = state == UsbRecorder::RecState::Recording;
    const bool paused = state == UsbRecorder::RecState::Paused;
    paintKey (g, recKeyAreas[0], "REC", [recording] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c)
    { recordDot (gr, r, recording ? c : recRed); }, ! recording);
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
    // These are flat (no key body), as in fig. 78.
    auto flat = [&] (juce::Rectangle<int> area, const juce::String& text, std::function<void (juce::Graphics&, juce::Rectangle<float>, juce::Colour)> icon, bool enabled)
    {
        auto r = area.toFloat();
        g.setColour (juce::Colour (0xff292929));
        g.fillRect (r);
        const auto ink = enabled ? Colours::text : Colours::textDim.withAlpha (0.45f);
        auto content = r.reduced (r.getHeight() * 0.12f);
        icon (g, content.removeFromLeft (content.getHeight()).reduced (content.getHeight() * 0.12f), ink);
        g.setColour (ink);
        g.setFont (font (juce::jmin (content.getHeight() * 0.3f, (float) getHeight() * 0.02f)));
        g.drawFittedText (text, content.toNearestInt(), juce::Justification::centred, 2, 0.7f);
    };
    flat (playlistKeyAreas[0], "CREATE\nPLAYLIST", [] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c) { drawIcon (gr, Icon::Edit, r, c); }, true);
    flat (playlistKeyAreas[1], "OPEN\nPLAYLIST", [] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c) { boxArrow (gr, r, c, true); }, canOpen);
    flat (playlistKeyAreas[2], "SAVE\nPLAYLIST", [] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c) { drawIcon (gr, Icon::SaveTray, r, c); },
          u.isPlaylistOpen());
    flat (playlistKeyAreas[3], "CLOSE\nPLAYLIST", [] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c) { boxArrow (gr, r, c, false); },
          u.isPlaylistOpen());
}

void UsbRecorderPage::paintPlayer (juce::Graphics& g)
{
    auto& u = usb();
    const float sh = (float) getHeight();
    const auto state = u.getPlayState();
    UsbRecorder::FileInfo info;
    int64_t length = u.getLength();
    bool mismatch = false;
    bool known = u.getCurrentInfo (info, length, mismatch);
    if (! known)
    {
        // Not opened yet: the header of the cued (or selected) file.
        auto file = u.getCurrentFile();
        if (file == juce::File())
            if (const auto* row = selectedRow(); row != nullptr && row->kind == (int) UsbRecorder::ItemKind::Wav)
                file = row->file;
        if (file == juce::File())
            for (const auto& e : u.getQueue())
                if (! e.skip)
                {
                    file = e.file;
                    break;
                }
        if (file != juce::File() && UsbRecorder::readFileInfo (file, info))
        {
            known = true;
            mismatch = juce::roundToInt (u.getSampleRate()) != info.sampleRate;
            length = (int64_t) std::ceil ((double) info.frames * u.getSampleRate() / juce::jmax (1, info.sampleRate));
        }
    }

    g.setColour (panelGrey);
    g.fillRect (playBox);
    meterBank (g, playMeter.toFloat(), playLevels, known ? juce::jlimit (2, 4, info.channels) : 4);

    // The state (green play, paused, stopped), the time and the time left.
    auto t = playTime.toFloat();
    auto top = t.removeFromTop (t.getHeight() * 0.62f);
    auto light = top.removeFromLeft (top.getHeight() * 0.8f).reduced (top.getHeight() * 0.1f);
    if (state == UsbRecorder::PlayState::Paused)
        drawIcon (g, Icon::Pause, light, Colours::gold);
    else if (state == UsbRecorder::PlayState::Error)
        drawIcon (g, Icon::Cancel, light, Colours::warning);
    else
    {
        juce::Path p;
        auto tri = light.withSizeKeepingCentre (light.getWidth() * 0.62f, light.getHeight() * 0.7f);
        p.addTriangle (tri.getX(), tri.getY(), tri.getRight(), tri.getCentreY(), tri.getX(), tri.getBottom());
        g.setColour (state == UsbRecorder::PlayState::Playing ? playGreen : playGreen.withAlpha (0.3f));
        g.fillPath (p);
    }
    top.removeFromLeft (top.getHeight() * 0.2f);
    const auto pos = u.getPosition();
    const bool active = state == UsbRecorder::PlayState::Playing || state == UsbRecorder::PlayState::Paused;
    // The time, then the time left under its right end, smaller (fig. 78).
    const auto digits = top.withHeight (top.getHeight() * 0.74f).withY (top.getY() + top.getHeight() * 0.13f);
    const float end = sevenSegment (g, digits, timeText (pos, u.getSampleRate()), active ? timeRed : timeRed.withAlpha (0.45f), juce::Justification::left);
    const float restH = digits.getHeight() * 0.52f;
    auto rest = juce::Rectangle<float> (digits.getX(), digits.getBottom() + restH * 0.25f, end - digits.getX(), restH);
    sevenSegment (g, rest, "-" + timeText (juce::jmax<int64_t> (0, length - pos), u.getSampleRate()), active ? timeRed : timeRed.withAlpha (0.45f),
                  juce::Justification::right);
    reels (g, playReels.toFloat(), reelAngle[1], state == UsbRecorder::PlayState::Playing);

    // The format chips: WAV | 4-CH over 48K | 24 bit (the rate in orange when resampled).
    auto chips = playChips.toFloat();
    const float cw = chips.getWidth() * 0.48f, ch = chips.getHeight() * 0.44f;
    const auto ink = known ? juce::Colour (0xffb8b8b8) : juce::Colour (0xff6a6a6a);
    const auto typeName = known ? AudioFileFormat::typeName (info.type) : juce::String ("WAV");
    chip (g, { chips.getX(), chips.getY(), cw, ch }, typeName, chipGrey, ink);
    chip (g, { chips.getRight() - cw, chips.getY(), cw, ch }, known ? juce::String (info.channels) + "-CH" : juce::String ("-CH"), chipGrey, ink);
    const auto rate = known ? (info.sampleRate == 44100 ? juce::String ("44.1K") : juce::String (juce::roundToInt (info.sampleRate / 1000.0)) + "K") : juce::String ("--K");
    chip (g, { chips.getX(), chips.getBottom() - ch, cw, ch }, rate, known && mismatch ? Dark::orange : chipGrey, known && mismatch ? juce::Colours::black : ink);
    const auto depth = ! known ? juce::String ("-- bit")
                       : info.type == AudioFileFormat::Type::Mp3 ? juce::String (info.kbps) + "k"
                                                                  : juce::String (info.bits) + " bit";
    chip (g, { chips.getRight() - cw, chips.getBottom() - ch, cw, ch }, depth, chipGrey, ink);

    g.setColour (captionGrey);
    g.setFont (font (sh * 0.034f));
    g.drawFittedText ("4-TRACK PLAYER", playCaption, juce::Justification::centredRight, 1, 0.7f);

    // PLAY, PAUSE, STOP, < >.
    g.setColour (panelGrey);
    g.fillRect (playKeys);
    paintKey (g, playKeyAreas[0], "PLAY", [] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c) { drawIcon (gr, Icon::Play, r, c); }, true);
    paintKey (g, playKeyAreas[1], "PAUSE", [] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c) { drawIcon (gr, Icon::Pause, r, c); },
              active, state == UsbRecorder::PlayState::Paused, Colours::gold);
    paintKey (g, playKeyAreas[2], "STOP", [] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c) { drawIcon (gr, Icon::Stop, r, c); }, true);
    const bool any = ! u.getQueue().empty();
    paintKey (g, playKeyAreas[3], {}, [] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c) { drawIcon (gr, Icon::ChevronLeft, r, c); }, any);
    paintKey (g, playKeyAreas[4], {}, [] (juce::Graphics& gr, juce::Rectangle<float> r, juce::Colour c) { drawIcon (gr, Icon::ChevronRight, r, c); }, any);
}

void UsbRecorderPage::paintTimeline (juce::Graphics& g)
{
    auto& u = usb();
    const float sh = (float) getHeight();
    auto r = timeline.toFloat();
    g.setColour (juce::Colour (0xff111111));
    g.fillRect (r);
    g.setColour (juce::Colour (0xff343434));
    g.drawRect (r, 1.0f);
    g.fillRect (r.getX(), r.getCentreY(), r.getWidth(), 1.0f);
    const auto length = u.getLength();
    const auto pos = u.getPosition();
    const float frac = length > 0 ? (float) juce::jlimit (0.0, 1.0, (double) pos / (double) length) : 0.0f;
    const float x = r.getX() + frac * r.getWidth();
    g.setColour (timelineFill);
    g.fillRect (r.withRight (x).reduced (0.0f, 1.0f));
    g.setColour (Colours::gold);
    g.fillRect (x - 1.0f, r.getY(), 2.0f, r.getHeight());
    // The position (top left) and the length (top right).
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
        select (row);
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
