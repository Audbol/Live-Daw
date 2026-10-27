#include "dsp/DspUtils.h"
#include "ui/Icons.h"
#include "ui/MeterBallistics.h"
#include "ui/PaintHash.h"
#include "ui/Pages.h"

namespace livedaw::ui
{

namespace
{
    constexpr int kChannelsPerPage = 40, kBusesPerPage = 16;

    // [3.1] WING pixels of the 1280 x 720 page: the title bar 8..1272 x 0..64,
    // the rows' headers 32 tall at 72 and 400 over their bodies (4 under,
    // 272 tall), the scale column 8..40; a group 16 px plus 23 per strip
    // wide, 4 apart, 12 between the blocks.
    constexpr float kTitleH = 64.0f, kHeaderH = 32.0f, kBodyGap = 4.0f, kBodyH = 272.0f;
    constexpr float kRowTops[] = { 72.0f, 400.0f };
    constexpr float kGroupPad = 8.0f, kGroupGap = 4.0f, kBlockGap = 12.0f, kSlot = 23.0f;
    // In a body: the number keys 8..27, the meter 45..228 (61 segments of 3),
    // the M keys 246..264.
    constexpr float kNumberTop = 8.0f, kNumberH = 19.0f, kMeterTop = 45.0f, kMeterBottom = 228.0f, kMuteTop = 246.0f, kMuteH = 18.0f;
    constexpr int kSegments = 61;

    const juce::Colour bodyFill { 0xff101010 };
    const juce::Colour segmentOff { 0xff202020 };
    const juce::Colour litGreen { 0xff00bc38 };
    const juce::Colour litYellow { 0xfff8c430 };

    /** The console's meter scale: CLIP, -6, -12, -24, -36, -48, -60 evenly spaced. */
    float meterPos (float db) { return meters::scalePos (db); }

    /** The segments lit at scale position `level` (a segment lights when the level reaches its middle). */
    int segmentsLit (float level) { return juce::jlimit (0, kSegments, (int) std::floor (level * (float) kSegments + 0.5f)); }

    /** A group's width in WING pixels (at 23 per strip). */
    float groupWidth (int count) { return kGroupPad * 2.0f + kSlot * (float) juce::jmax (4, count); }

    void numberKey (juce::Graphics& g, juce::Rectangle<float> r, int number, juce::Colour fill)
    {
        g.setColour (fill);
        g.fillRoundedRectangle (r, juce::jmax (1.0f, r.getHeight() * 0.12f));
        g.setColour (juce::Colours::black);
        // Two digits squeezed into the key, as on the console.
        g.setFont (wingFontPx (juce::jmin (r.getHeight() * 0.74f, r.getWidth() * (number >= 10 ? 0.62f : 0.9f)), WingWeight::SemiBold));
        g.drawFittedText (juce::String (number), r.toNearestInt(), juce::Justification::centred, 1, 0.5f);
    }
} // namespace

/** METERS > SETTINGS [3.1]: PRE or POST for the channels' (and aux'), buses',
    mains', matrices' and DCAs' meters over the dimmed page; the factory
    setting is PRE for channels and DCAs, POST for the others. */
class MetersPage::Settings final : public juce::Component
{
public:
    explicit Settings (MetersPage& p) : page (p)
    {
        closeKey.onClick = [this] { page.showSettings (false); };
        addAndMakeVisible (closeKey);
        for (int i = 0; i < prefs::kNumMeterTaps; ++i)
            addAndMakeVisible (keys.add (new ChoiceKeys (
                { "PRE", "POST" }, [this, i] { return prefs::meterPageTapPre (page.ctx, i) ? 0 : 1; },
                [this, i] (int v) { page.ctx.settings.setValue (prefs::meterPageTapKey (i), v == 0); }, ChoiceKeys::prePost())));
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (Colours::scrim);
        g.fillAll();
        const auto w = window();
        g.setColour (juce::Colours::black);
        g.fillRect (w);
        paintWindowTitle (g, w, "SETTINGS", true);
        g.setColour (Colours::screenPanel);
        g.fillRect (body());
        static const char* const names[] = { "CHANNELS", "BUSES", "MAINS", "MATRICES", "DCAS" };
        g.setColour (Colours::caption);
        g.setFont (wingFontPx (wingPx (14.0f)));
        const int x = body().getX() + juce::roundToInt (page.px (12.0f));
        for (int i = 0; i < keys.size(); ++i)
            g.drawText (names[i], juce::Rectangle<int> (x, keys[i]->getY(), keys[i]->getX() - x, keys[i]->getHeight()), juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        const auto w = window();
        closeKey.setBounds (w.withHeight (titleH()).removeFromRight (juce::roundToInt (page.px (128.0f))).reduced (juce::roundToInt (page.px (8.0f))));
        // [3.1] keys 128 x 48 at 200 px into the window, 56 apart from 12 down.
        for (int i = 0; i < keys.size(); ++i)
            keys[i]->setBounds (juce::Rectangle<float> ((float) w.getX() + page.px (200.0f), (float) body().getY() + page.py (12.0f + 56.0f * (float) i),
                                                        page.px (128.0f), page.py (48.0f))
                                    .toNearestInt());
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        // Touching the dimmed page closes the window.
        if (! window().contains (e.getPosition()))
            page.showSettings (false);
    }

private:
    /** [3.1] 340 x 364 at 460, 80; the title 64 tall, the rows 4 under it. */
    juce::Rectangle<int> window() const
    {
        return juce::Rectangle<float> (page.px (460.0f), page.py (80.0f), page.px (340.0f), page.py (364.0f)).toNearestInt();
    }
    int titleH() const { return juce::roundToInt (page.py (64.0f)); }
    juce::Rectangle<int> body() const { return window().withTrimmedTop (titleH() + juce::roundToInt (page.py (4.0f))); }

    MetersPage& page;
    CloseKey closeKey;
    juce::OwnedArray<ChoiceKeys> keys;
};

MetersPage::MetersPage (UiContext& c) : ctx (c)
{
    startTimerHz (25);
}

MetersPage::~MetersPage() = default;

std::vector<StatusKey> MetersPage::getStatusKeys()
{
    StatusKey key { "SETTINGS", Icon::Gear, settings != nullptr && settings->isVisible(),
                    [this] { showSettings (settings == nullptr || ! settings->isVisible()); } };
    key.atRight = true;
    return { key };
}

void MetersPage::showSettings (bool show)
{
    if (show && settings == nullptr)
    {
        settings = std::make_unique<Settings> (*this);
        addChildComponent (*settings);
    }
    if (settings != nullptr)
    {
        settings->setBounds (getLocalBounds());
        settings->setVisible (show);
    }
    notifyStatusKeysChanged();
}

int MetersPage::numPages() const
{
    const int ch = (int) ctx.engine.getStrips (StripKind::Channel).size();
    const int bus = (int) ctx.engine.getStrips (StripKind::Bus).size();
    return juce::jmax (1, (ch + kChannelsPerPage - 1) / kChannelsPerPage, (bus + kBusesPerPage - 1) / kBusesPerPage);
}

void MetersPage::timerCallback()
{
    std::vector<int> counts;
    for (int k = 0; k < kNumStripKinds; ++k)
        counts.push_back ((int) ctx.engine.getStrips (static_cast<StripKind> (k)).size());
    if (counts != laidOutCounts)
        resized();

    if (! isShowing())
        return;
    // The meters repaint strip by strip, only where a segment or the peak
    // hold moved; the rest of the page (keys, names, mutes) only when its
    // drawing changes (the hash leaves the meters out).
    size_t n = 0;
    for (auto& group : groups)
        for (int i = 0; i < group.count; ++i, ++n)
        {
            const auto sig = meterSignature (group, i);
            if (n >= shownMeters.size())
                shownMeters.resize (n + 1, -1);
            if (sig != shownMeters[n])
            {
                shownMeters[n] = sig;
                repaint (meterArea (group, i));
            }
        }
    hashing = true;
    const auto hash = paintHash (*this);
    hashing = false;
    if (hash != shownHash)
    {
        shownHash = hash;
        repaint();
    }
}

juce::int64 MetersPage::meterSignature (const Group& group, int index) const
{
    juce::int64 sig = isStereo (group, index) ? 1 : 0;
    for (int ch = 0; ch < 2; ++ch)
        sig = sig * 4096 + segmentsLit (levelOf (group, index, ch)) * 64 + segmentsLit (holdOf (group, index, ch));
    return sig;
}

juce::Rectangle<int> MetersPage::meterArea (const Group& group, int index) const
{
    const auto s = slot (group, index);
    return juce::Rectangle<float> (s.getX(), (float) group.body.getY() + py (kMeterTop) - 2.0f, s.getWidth(), py (kMeterBottom - kMeterTop) + 4.0f)
        .getSmallestIntegerContainer();
}

float MetersPage::unitsOf (const std::vector<Group*>& items)
{
    // The groups' widths in WING pixels, each with the gap after it.
    float units = 0.0f;
    for (auto* item : items)
        units += groupWidth (item->count) + kGroupGap;
    return units;
}

void MetersPage::layoutRow (int row, float k, std::vector<Group*> left, std::vector<Group*> centre, std::vector<Group*> right)
{
    // Left from the scale column, right up to the page's edge, centre between them.
    auto place = [this, row, k] (Group& g, float x)
    {
        const float w = groupWidth (g.count) * k;
        g.header = juce::Rectangle<float> (px (x), py (kRowTops[row]), px (w), py (kHeaderH)).toNearestInt();
        g.body = juce::Rectangle<float> (px (x), py (kRowTops[row] + kHeaderH + kBodyGap), px (w), py (kBodyH)).toNearestInt();
        return w + kGroupGap * k;
    };

    float x = 44.0f;
    for (auto* g : left)
        x += place (*g, x);
    const float leftEnd = x - kGroupGap * k;

    // The right-hand groups (MAIN, MATRIX) 12 apart.
    x = 1272.0f + kBlockGap * k - (unitsOf (right) + (kBlockGap - kGroupGap) * (float) right.size()) * k;
    const float rightStart = x;
    for (auto* g : right)
        x += place (*g, x) + (kBlockGap - kGroupGap) * k;

    x = (leftEnd + rightStart) / 2.0f - (unitsOf (centre) - kGroupGap) * k / 2.0f;
    for (auto* g : centre)
        x += place (*g, x);
}

void MetersPage::resized()
{
    if (settings != nullptr)
        settings->setBounds (getLocalBounds());
    laidOutCounts.clear();
    for (int k = 0; k < kNumStripKinds; ++k)
        laidOutCounts.push_back ((int) ctx.engine.getStrips (static_cast<StripKind> (k)).size());

    page = juce::jlimit (0, numPages() - 1, page);
    const int channels = laidOutCounts[(size_t) StripKind::Channel];
    const int buses = laidOutCounts[(size_t) StripKind::Bus];

    groups.clear();
    for (int first = page * kChannelsPerPage; first < juce::jmin (channels, (page + 1) * kChannelsPerPage); first += 8)
        groups.push_back ({ "CH " + juce::String (first + 1) + "-" + juce::String (juce::jmin (channels, first + 8)), StripKind::Channel, false, first,
                            juce::jmin (8, channels - first), LayerSet::Left, {}, {} });
    const size_t channelGroups = groups.size();
    groups.push_back ({ "AUX", StripKind::Aux, false, 0, laidOutCounts[(size_t) StripKind::Aux], LayerSet::Left, {}, {} });
    for (int first = page * kBusesPerPage; first < juce::jmin (buses, (page + 1) * kBusesPerPage); first += 8)
        groups.push_back ({ "BUS " + juce::String (first + 1) + "-" + juce::String (juce::jmin (buses, first + 8)), StripKind::Bus, false, first,
                            juce::jmin (8, buses - first), LayerSet::Center, {}, {} });
    const size_t busEnd = groups.size();
    groups.push_back ({ "DCA 1-8", StripKind::Bus, true, 0, 8, LayerSet::Center, {}, {} });
    groups.push_back ({ "DCA 9-16", StripKind::Bus, true, 8, kNumDcas - 8, LayerSet::Center, {}, {} });
    groups.push_back ({ "MAIN", StripKind::Main, false, 0, laidOutCounts[(size_t) StripKind::Main], LayerSet::Right, {}, {} });
    groups.push_back ({ "MATRIX", StripKind::Matrix, false, 0, laidOutCounts[(size_t) StripKind::Matrix], LayerSet::Right, {}, {} });

    titleArea = juce::Rectangle<float> (px (8.0f), 0.0f, px (1264.0f), py (kTitleH)).toNearestInt();
    auto keys = titleArea.reduced (juce::roundToInt (px (8.0f)));
    nextKey = numPages() > 1 ? keys.removeFromRight (keys.getHeight() * 2) : juce::Rectangle<int>();
    keys.removeFromRight (6);
    prevKey = numPages() > 1 ? keys.removeFromRight (keys.getHeight() * 2) : juce::Rectangle<int>();

    for (int row = 0; row < 2; ++row)
    {
        (row == 0 ? scaleHeaderTop : scaleHeaderBottom) = juce::Rectangle<float> (px (8.0f), py (kRowTops[row]), px (32.0f), py (kHeaderH)).toNearestInt();
        (row == 0 ? scaleTop : scaleBottom) =
            juce::Rectangle<float> (px (8.0f), py (kRowTops[row] + kHeaderH + kBodyGap), px (32.0f), py (kBodyH)).toNearestInt();
    }

    std::vector<Group*> channelsRow, auxRow, busRow, dcaRow, outRow;
    for (size_t i = 0; i < groups.size(); ++i)
    {
        auto* g = &groups[i];
        if (i < channelGroups)
            channelsRow.push_back (g);
        else if (i == channelGroups)
            auxRow.push_back (g);
        else if (i < busEnd)
            busRow.push_back (g);
        else if (g->dca)
            dcaRow.push_back (g);
        else
            outRow.push_back (g);
    }

    // One scale for both rows, like the console: 23 WING pixels a strip,
    // squeezed when a bigger console's groups would not fit.
    const float room = 1272.0f - 44.0f;
    const float needed = juce::jmax (unitsOf (channelsRow) + unitsOf (auxRow) + kBlockGap - kGroupGap * 2.0f,
                                     unitsOf (busRow) + unitsOf (dcaRow) + unitsOf (outRow) + kBlockGap * (float) (outRow.size() + 1) - kGroupGap * (float) (outRow.size() + 2));
    const float k = juce::jmin (1.0f, room / juce::jmax (1.0f, needed));
    slotW = kSlot * k;
    layoutRow (0, k, channelsRow, {}, auxRow);
    layoutRow (1, k, busRow, dcaRow, outRow);
}

juce::Rectangle<int> MetersPage::numberRow (const Group& group) const
{
    return juce::Rectangle<float> ((float) group.body.getX(), (float) group.body.getY() + py (kNumberTop), (float) group.body.getWidth(), py (kNumberH))
        .toNearestInt();
}

juce::Rectangle<int> MetersPage::muteRow (const Group& group) const
{
    return juce::Rectangle<float> ((float) group.body.getX(), (float) group.body.getY() + py (kMuteTop), (float) group.body.getWidth(), py (kMuteH))
        .toNearestInt();
}

juce::Rectangle<float> MetersPage::slot (const Group& group, int index) const
{
    const float w = px (slotW);
    return { (float) group.body.getX() + px (kGroupPad * slotW / kSlot) + w * (float) index, (float) group.body.getY(), w, (float) group.body.getHeight() };
}

Strip* MetersPage::stripOf (const Group& group, int index) const
{
    if (group.dca)
        return nullptr;
    auto& strips = ctx.engine.getStrips (group.kind);
    const int i = group.first + index;
    return i >= 0 && i < (int) strips.size() ? strips[(size_t) i].get() : nullptr;
}

bool MetersPage::isStereo (const Group& group, int index) const
{
    // A DCA shows its members' left and right [3.1].
    if (group.dca)
        return true;
    auto* s = stripOf (group, index);
    return s != nullptr && (s->params.stereo.load() || s->isOutput());
}

bool MetersPage::isMuted (const Group& group, int index) const
{
    if (group.dca)
        return ctx.engine.getDca (group.first + index).mute.load();
    auto* s = stripOf (group, index);
    return s != nullptr && s->params.mute.load();
}

float MetersPage::levelOf (const Group& group, int index, int channel) const
{
    return meterPos (dsp::gainToDb (peakOf (group, index, channel, false)));
}

float MetersPage::holdOf (const Group& group, int index, int channel) const
{
    return meterPos (dsp::gainToDb (peakOf (group, index, channel, true)));
}

float MetersPage::peakOf (const Group& group, int index, int channel, bool hold) const
{
    auto read = [this, hold] (const Strip* s, MeterPoint point, int ch) { return hold ? ctx.meters.getHold (s, point, ch) : ctx.meters.get (s, point, ch); };
    // PRE or POST as SETTINGS says for the kind of strip.
    if (! group.dca)
    {
        auto* s = stripOf (group, index);
        return s != nullptr ? read (s, prefs::meterPagePoint (ctx, s->getKind()), channel) : 0.0f;
    }

    // A DCA shows the loudest of its members, before or after their faders.
    const uint32_t bit = 1u << (uint32_t) (group.first + index);
    const auto point = prefs::meterPageTapPre (ctx, 4) ? MeterPoint::PreFader : MeterPoint::Output;
    float peak = 0.0f;
    for (auto kind : { StripKind::Channel, StripKind::Aux, StripKind::Bus, StripKind::Main, StripKind::Matrix })
        for (auto& s : ctx.engine.getStrips (kind))
            if ((s->params.dcaMask.load() & bit) != 0)
                peak = juce::jmax (peak, read (s.get(), point, channel));
    return peak;
}

void MetersPage::paintScale (juce::Graphics& g, juce::Rectangle<int> column, const Group& reference) const
{
    g.setColour (Colours::screenPanel);
    g.fillRect (column);
    const float top = (float) reference.body.getY() + py (kMeterTop), bottom = (float) reference.body.getY() + py (kMeterBottom);
    g.setColour (Colours::caption);
    g.setFont (wingFontPx (juce::jmax (5.0f, wingPx (8.5f))));
    const std::pair<const char*, float> marks[] = { { "CLIP", 0.0f }, { "-6", -6.0f }, { "-12", -12.0f }, { "-24", -24.0f },
                                                    { "-36", -36.0f }, { "-48", -48.0f }, { "-60", -60.0f } };
    // Labels that would overlap the previous one are left out (small windows).
    float lastY = -1.0e6f;
    const float minGap = g.getCurrentFont().getHeight() * 0.9f;
    for (auto& [text, db] : marks)
    {
        const float y = bottom - (bottom - top) * meterPos (db);
        if (y - lastY < minGap)
            continue;
        lastY = y;
        g.drawText (text, juce::Rectangle<float> ((float) column.getX(), y - minGap * 0.6f, (float) column.getWidth() - px (4.0f), minGap * 1.2f),
                    juce::Justification::centredRight);
    }
}

void MetersPage::paint (juce::Graphics& g)
{
    g.fillAll (Colours::screen);

    // Title bar.
    g.setColour (Colours::screenPanel);
    g.fillRect (titleArea);
    g.setColour (Colours::caption);
    g.setFont (wingFontPx (wingPx (19.0f)));
    juce::String title = "METERS";
    if (numPages() > 1)
        title << "  -  PAGE " << (page + 1) << " / " << numPages();
    g.drawText (title, titleArea, juce::Justification::centred);
    for (auto [r, icon] : { std::pair { prevKey, 0 }, std::pair { nextKey, 1 } })
    {
        if (r.isEmpty())
            continue;
        g.setColour (Colours::screenTile);
        g.fillRoundedRectangle (r.toFloat(), 2.0f);
        g.setColour (Colours::text);
        g.setFont (juce::FontOptions ((float) r.getHeight() * 0.7f, juce::Font::bold));
        g.drawText (icon == 0 ? "<" : ">", r, juce::Justification::centred);
    }

    if (groups.empty())
        return;
    g.setColour (Colours::screenHeader);
    g.fillRect (scaleHeaderTop);
    g.fillRect (scaleHeaderBottom);
    paintScale (g, scaleTop, groups.front());
    for (auto& group : groups)
        if (group.body.getY() >= scaleBottom.getY())
        {
            paintScale (g, scaleBottom, group);
            break;
        }

    const float k = slotW / kSlot;
    for (auto& group : groups)
    {
        g.setColour (Colours::screenHeader);
        g.fillRect (group.header);
        g.setColour (Colours::caption);
        g.setFont (wingFontPx (juce::jmin (wingPx (14.0f), (float) group.header.getHeight() * 0.5f)));
        g.drawFittedText (group.title, group.header.reduced (4, 0), juce::Justification::centred, 1);

        g.setColour (bodyFill);
        g.fillRect (group.body);

        const auto numbers = numberRow (group), mutes = muteRow (group);
        const float meterTop = (float) group.body.getY() + py (kMeterTop), meterBottom = (float) group.body.getY() + py (kMeterBottom);
        const float segH = (meterBottom - meterTop) / (float) kSegments;

        for (int i = 0; i < group.count; ++i)
        {
            const auto s = slot (group, i);
            auto* strip = stripOf (group, i);
            const int number = group.first + i + 1;
            // [3.1] number keys 19 px square in the strip's colour, 2 in from the slot.
            const auto numberArea = juce::Rectangle<float> (s.getX() + px (2.0f * k), (float) numbers.getY(), px (19.0f * k), (float) numbers.getHeight());
            numberKey (g, numberArea, number, group.dca ? wingColour (ctx.engine.getDca (group.first + i).colour.load())
                                                 : (strip != nullptr ? strip->getColour() : Colours::cyan)); // [3.1] unpatched strips too, undimmed

            // LED meter: one column 7 px wide, or two of 6 a pixel apart, with peak hold.
            // (Left out of the page's hash: the timer repaints meters strip by strip.)
            const bool stereo = isStereo (group, i);
            const float colW = px ((stereo ? 6.0f : 7.0f) * k), gap = px (k);
            const float total = stereo ? colW * 2.0f + gap : colW;
            float x = numberArea.getCentreX() - total / 2.0f;
            for (int ch = 0; ch < (stereo ? 2 : 1) && ! hashing; ++ch, x += colW + gap)
            {
                const int lit = segmentsLit (levelOf (group, i, ch));
                const int holdSeg = segmentsLit (holdOf (group, i, ch)) - 1;
                for (int seg = 0; seg < kSegments; ++seg)
                {
                    const float pos = ((float) seg + 0.5f) / (float) kSegments;
                    // [3.1] green, yellow from -18 dB, red at the top.
                    juce::Colour c = segmentOff;
                    if (seg < lit)
                        c = pos > meterPos (-0.5f) ? Colours::mute : (pos > meterPos (-18.0f) ? litYellow : litGreen);
                    else if (seg == holdSeg)
                        c = Colours::amber; // the peak-hold dash
                    g.setColour (c);
                    g.fillRect (x, meterBottom - (float) (seg + 1) * segH, colW, juce::jmax (1.0f, segH * 2.0f / 3.0f));
                }
            }

            // M: a dark key with a black M, red while muted.
            const auto m = juce::Rectangle<float> (s.getX() + px (3.0f * k), (float) mutes.getY(), px (17.0f * k), (float) mutes.getHeight());
            g.setColour (isMuted (group, i) ? Colours::mute : segmentOff);
            g.fillRoundedRectangle (m, juce::jmax (1.0f, m.getHeight() * 0.1f));
            g.setColour (juce::Colours::black);
            g.setFont (wingFontPx (juce::jmin (m.getHeight() * 0.62f, m.getWidth() * 0.6f), WingWeight::SemiBold));
            g.drawText ("M", m, juce::Justification::centred, false);
        }
    }
}

void MetersPage::bringToSection (const Group& group, int index)
{
    // The section selects the layer key holding the fader and scrolls it into view.
    std::optional<state::FaderRef> fader;
    if (group.dca)
        fader = state::FaderRef::ofDca (group.first + juce::jmax (0, index));
    else if (auto* s = stripOf (group, juce::jmax (0, index)))
        fader = state::FaderRef::of (state::StripRef::of (*s));
    if (! fader)
        return;
    if (ctx.revealFader)
    {
        ctx.revealFader (group.section, *fader);
        return;
    }
    const auto bank = ctx.show.layers.findDefaultLayerContaining (ctx.engine, group.section, *fader);
    if (bank.isNotEmpty() && bank != ctx.show.layers.getSelectedLayerId (group.section))
    {
        ctx.show.layers.setSelectedLayerId (group.section, bank);
        if (ctx.layersChanged)
            ctx.layersChanged();
    }
}

void MetersPage::mouseDown (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();
    if (prevKey.contains (pos) || nextKey.contains (pos))
    {
        page = (page + (nextKey.contains (pos) ? 1 : numPages() - 1)) % numPages();
        resized();
        repaint();
        return;
    }

    for (auto& group : groups)
    {
        if (group.header.contains (pos))
        {
            // Touching a group opens the FADERS screen of its section.
            bringToSection (group, 0);
            ctx.viewSection = group.section;
            if (ctx.showScreen)
                ctx.showScreen (Screen::Faders);
            return;
        }
        if (! group.body.contains (pos))
            continue;

        for (int i = 0; i < group.count; ++i)
        {
            if (! slot (group, i).contains (pos.toFloat()))
                continue;
            if (muteRow (group).contains (pos))
            {
                if (group.dca)
                    ctx.engine.getDca (group.first + i).mute = ! ctx.engine.getDca (group.first + i).mute.load();
                else if (auto* s = stripOf (group, i))
                    s->params.mute = ! s->params.mute.load();
            }
            else if (auto* s = stripOf (group, i))
            {
                if (ctx.selectStrip)
                    ctx.selectStrip (s);
                bringToSection (group, i);
            }
            else
                bringToSection (group, i);
            repaint();
            return;
        }
    }
}

} // namespace livedaw::ui
