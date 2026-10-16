#include "dsp/DspUtils.h"
#include "ui/Icons.h"
#include "ui/Pages.h"

namespace livedaw::ui
{

namespace
{
    constexpr int kChannelsPerPage = 40, kBusesPerPage = 16, kScaleWidth = 34;

    /** The console's meter scale: CLIP, -6, -12, -24, -36, -48, -60 evenly spaced. */
    float meterPos (float db)
    {
        static constexpr float marks[] = { -60.0f, -48.0f, -36.0f, -24.0f, -12.0f, -6.0f, 0.0f };
        if (db <= marks[0])
            return 0.0f;
        for (size_t i = 1; i < std::size (marks); ++i)
            if (db <= marks[i])
                return ((float) (i - 1) + (db - marks[i - 1]) / (marks[i] - marks[i - 1])) / 6.0f;
        return 1.0f;
    }

    void numberKey (juce::Graphics& g, juce::Rectangle<float> r, int number, juce::Colour fill)
    {
        g.setColour (fill);
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (juce::jlimit (9.0f, 18.0f, juce::jmin (r.getHeight() * 0.86f, r.getWidth() * (number >= 10 ? 0.62f : 0.9f))),
                                      juce::Font::bold));
        g.drawFittedText (juce::String (number), r.toNearestInt(), juce::Justification::centred, 1, 0.6f);
    }
} // namespace

MetersPage::MetersPage (UiContext& c) : ctx (c)
{
    startTimerHz (25);
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

    // Peak hold: about a second, then fall.
    for (auto& group : groups)
        for (int i = 0; i < group.count; ++i)
            for (int ch = 0; ch < 2; ++ch)
            {
                const auto* key = static_cast<const char*> (group.dca ? (const void*) &ctx.engine.getDca (group.first + i)
                                                                      : (const void*) stripOf (group, i)) + ch;
                auto& h = holds[key];
                const float level = levelOf (group, i, ch);
                if (level >= h.level)
                {
                    h.level = level;
                    h.frames = 25;
                }
                else if (--h.frames < 0)
                    h.level = juce::jmax (level, h.level - 0.02f);
            }
    repaint();
}

float MetersPage::unitsOf (const std::vector<Group*>& items)
{
    float units = 0.0f;
    for (auto* item : items)
        units += (float) (juce::jmax (4, item->count) + 1);
    return units;
}

void MetersPage::layoutRow (juce::Rectangle<int> row, float unit, std::vector<Group*> left, std::vector<Group*> centre, std::vector<Group*> right)
{
    auto place = [&row, unit] (Group& g, float x)
    {
        const float w = unit * (float) (juce::jmax (4, g.count) + 1);
        auto area = juce::Rectangle<int> (juce::roundToInt (x), row.getY(), juce::roundToInt (w) - 4, row.getHeight());
        g.header = area.removeFromTop (kPanelHeaderHeight);
        area.removeFromTop (4);
        g.body = area;
        return w;
    };

    float x = (float) row.getX();
    for (auto* g : left)
        x += place (*g, x);
    const float leftEnd = x;

    x = (float) row.getRight() + 4.0f - unit * unitsOf (right);
    const float rightStart = x;
    for (auto* g : right)
        x += place (*g, x);

    x = (leftEnd + rightStart) / 2.0f - unit * unitsOf (centre) / 2.0f;
    for (auto* g : centre)
        x += place (*g, x);
}

void MetersPage::resized()
{
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

    auto b = getLocalBounds();
    titleArea = b.removeFromTop (juce::jmin (52, b.getHeight() / 9));
    auto keys = titleArea.reduced (6);
    nextKey = numPages() > 1 ? keys.removeFromRight (keys.getHeight() * 2) : juce::Rectangle<int>();
    keys.removeFromRight (6);
    prevKey = numPages() > 1 ? keys.removeFromRight (keys.getHeight() * 2) : juce::Rectangle<int>();
    b.removeFromTop (8);

    auto top = b.removeFromTop ((b.getHeight() - 16) / 2);
    b.removeFromTop (16);
    auto bottom = b;
    scaleTop = top.removeFromLeft (kScaleWidth);
    scaleBottom = bottom.removeFromLeft (kScaleWidth);

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

    // One scale for both rows, like the console: every group of eight is the same width.
    const float gaps = 2.0f;
    const float units = juce::jmax (unitsOf (channelsRow) + unitsOf (auxRow) + gaps,
                                    unitsOf (busRow) + unitsOf (dcaRow) + unitsOf (outRow) + gaps * 2.0f, 40.0f);
    const float unit = (float) top.getWidth() / units;
    layoutRow (top, unit, channelsRow, {}, auxRow);
    layoutRow (bottom, unit, busRow, dcaRow, outRow);
}

juce::Rectangle<int> MetersPage::numberRow (const Group& group) const
{
    return group.body.reduced (6, 0).withTrimmedTop (8).withHeight (juce::jlimit (16, 24, group.body.getHeight() / 11));
}

juce::Rectangle<int> MetersPage::muteRow (const Group& group) const
{
    auto r = group.body.reduced (6, 0).withTrimmedBottom (8);
    return r.removeFromBottom (juce::jlimit (14, 20, group.body.getHeight() / 13));
}

juce::Rectangle<float> MetersPage::slot (const Group& group, int index) const
{
    auto row = group.body.reduced (6, 0).toFloat();
    const float w = row.getWidth() / (float) juce::jmax (1, juce::jmax (4, group.count));
    return { row.getX() + w * (float) index, (float) group.body.getY(), w, (float) group.body.getHeight() };
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
    if (group.dca)
        return false;
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
    // The console's meters here are pre-fader; ours read the post-fader output
    // until the engine has a pre-fader tap (session E11: pre-fader meter taps).
    if (! group.dca)
        return meterPos (dsp::gainToDb (ctx.meters.get (stripOf (group, index), false, channel)));

    // A DCA shows the loudest of its members.
    const uint32_t bit = 1u << (uint32_t) (group.first + index);
    float peak = 0.0f;
    for (auto kind : { StripKind::Channel, StripKind::Aux, StripKind::Bus, StripKind::Main, StripKind::Matrix })
        for (auto& s : ctx.engine.getStrips (kind))
            if ((s->params.dcaMask.load() & bit) != 0)
                peak = juce::jmax (peak, ctx.meters.get (s.get(), false, channel));
    return meterPos (dsp::gainToDb (peak));
}

void MetersPage::paintScale (juce::Graphics& g, juce::Rectangle<int> column, const Group& reference) const
{
    g.setColour (Colours::screenHeader);
    g.fillRect (column.withTrimmedRight (4).withTrimmedTop (kPanelHeaderHeight + 4));
    const auto numbers = numberRow (reference), mutes = muteRow (reference);
    const float top = (float) numbers.getBottom() + 10.0f, bottom = (float) mutes.getY() - 8.0f;
    g.setColour (Colours::text);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    const std::pair<const char*, float> marks[] = { { "CLIP", 0.0f }, { "-6", -6.0f }, { "-12", -12.0f }, { "-24", -24.0f },
                                                    { "-36", -36.0f }, { "-48", -48.0f }, { "-60", -60.0f } };
    for (auto& [text, db] : marks)
    {
        const float y = bottom - (bottom - top) * meterPos (db);
        g.drawText (text, juce::Rectangle<float> ((float) column.getX(), y - 7.0f, (float) column.getWidth() - 6.0f, 14.0f),
                    juce::Justification::centredRight);
    }
}

void MetersPage::paint (juce::Graphics& g)
{
    g.fillAll (Colours::screen);

    // Title bar.
    g.setColour (Colours::screenPanel);
    g.fillRect (titleArea);
    g.setColour (Colours::text);
    g.setFont (juce::FontOptions (juce::jmin (26.0f, titleArea.getHeight() * 0.55f), juce::Font::bold));
    juce::String title = "METERS";
    if (numPages() > 1)
        title << "  -  PAGE " << (page + 1) << " / " << numPages();
    g.drawText (title, titleArea, juce::Justification::centred);
    for (auto [r, icon] : { std::pair { prevKey, 0 }, std::pair { nextKey, 1 } })
    {
        if (r.isEmpty())
            continue;
        g.setColour (Colours::screenHeader);
        g.fillRoundedRectangle (r.toFloat(), 2.0f);
        g.setColour (Colours::text);
        g.setFont (juce::FontOptions ((float) r.getHeight() * 0.7f, juce::Font::bold));
        g.drawText (icon == 0 ? "<" : ">", r, juce::Justification::centred);
    }

    if (groups.empty())
        return;
    paintScale (g, scaleTop, groups.front());
    for (auto& group : groups)
        if (group.body.getY() >= scaleBottom.getY())
        {
            paintScale (g, scaleBottom, group);
            break;
        }

    auto* selected = ctx.getSelectedStrip ? ctx.getSelectedStrip() : nullptr;

    for (auto& group : groups)
    {
        g.setColour (Colours::screenHeader);
        g.fillRect (group.header);
        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (juce::jmin (20.0f, group.header.getHeight() * 0.68f), juce::Font::bold));
        g.drawFittedText (group.title, group.header.reduced (4, 0), juce::Justification::centred, 1);

        g.setColour (juce::Colour (0xff262626));
        g.fillRect (group.body);

        const auto numbers = numberRow (group), mutes = muteRow (group);
        const float meterTop = (float) numbers.getBottom() + 10.0f, meterBottom = (float) mutes.getY() - 8.0f;
        const int segments = juce::jlimit (12, 40, juce::roundToInt ((meterBottom - meterTop) / 4.5f));
        const float segH = (meterBottom - meterTop) / (float) segments;

        for (int i = 0; i < group.count; ++i)
        {
            auto s = slot (group, i);
            const auto cell = s.reduced (juce::jmin (2.0f, s.getWidth() * 0.08f), 0.0f);
            auto* strip = stripOf (group, i);
            const int number = group.first + i + 1;
            // Number keys in the strip's colour; the selected strip gets a white frame.
            const auto numberArea = cell.withY ((float) numbers.getY()).withHeight ((float) numbers.getHeight());
            numberKey (g, numberArea, number, group.dca ? wingColour (ctx.engine.getDca (group.first + i).colour.load())
                                                 : (strip != nullptr ? strip->getColour() : Colours::cyan));
            if (strip != nullptr && strip == selected)
            {
                g.setColour (juce::Colours::white);
                g.drawRoundedRectangle (numberArea.expanded (1.5f), 2.0f, 2.0f);
            }

            // LED meter: two narrow columns (one for mono), green / yellow / red, with peak hold.
            const bool stereo = isStereo (group, i);
            const float colW = juce::jmin (5.0f, cell.getWidth() * 0.22f);
            const float gap = colW * 0.6f;
            const float total = stereo ? colW * 2.0f + gap : colW;
            float x = cell.getCentreX() - total / 2.0f;
            for (int ch = 0; ch < (stereo ? 2 : 1); ++ch, x += colW + gap)
            {
                const float level = levelOf (group, i, ch);
                const auto* key = static_cast<const char*> (group.dca ? (const void*) &ctx.engine.getDca (group.first + i) : (const void*) strip) + ch;
                const auto hold = holds.find (key);
                const int holdSeg = hold != holds.end() && hold->second.level > 0.0f ? juce::jmin (segments - 1, (int) (hold->second.level * (float) segments)) : -1;
                for (int k = 0; k < segments; ++k)
                {
                    const float pos = ((float) k + 0.5f) / (float) segments;
                    const bool lit = level >= pos;
                    juce::Colour c = juce::Colour (0xff4a4a4a);
                    if (lit)
                        c = pos > meterPos (-0.5f) ? Colours::mute : (pos > meterPos (-12.0f) ? Colours::meterYellow : Colours::meterGreen);
                    else if (k == holdSeg)
                        c = Colours::amber; // the peak-hold dash
                    g.setColour (c);
                    g.fillRect (x, meterBottom - (float) (k + 1) * segH + 0.5f, colW, juce::jmax (1.0f, segH - 1.5f));
                }
            }

            // Mute: an empty square, filled red while muted.
            const auto m = cell.withY ((float) mutes.getY()).withHeight ((float) mutes.getHeight()).withSizeKeepingCentre (
                juce::jmin (cell.getWidth(), (float) mutes.getHeight()), (float) mutes.getHeight()).reduced (1.0f);
            if (isMuted (group, i))
            {
                g.setColour (Colours::mute);
                g.fillRect (m);
            }
            g.setColour (juce::Colour (0xff5a5a5a));
            g.drawRect (m, 1.0f);
        }
    }
}

void MetersPage::bringToSection (const Group& group, Strip* s)
{
    juce::String bank;
    if (group.dca)
        bank = ctx.show.layers.findDefaultLayerContaining (ctx.engine, group.section, state::FaderRef::ofDca (group.first));
    else if (s != nullptr)
        bank = ctx.show.layers.findDefaultLayerContaining (ctx.engine, group.section, state::FaderRef::of (state::StripRef::of (*s)));
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
            bringToSection (group, stripOf (group, 0));
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
                bringToSection (group, s);
            }
            else
                bringToSection (group, nullptr);
            repaint();
            return;
        }
    }
}

} // namespace livedaw::ui
