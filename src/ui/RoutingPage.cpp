#include "ui/Pages.h"

namespace livedaw::ui
{

namespace
{
    juce::StringArray numbered (const juce::StringArray& names, int count)
    {
        juce::StringArray items;
        for (int i = 0; i < count; ++i)
            items.add (juce::String (i + 1) + ": " + (i < names.size() ? names[i] : juce::String ("(not available)")));
        return items;
    }

    /** One strip's routing: name, left/right patch, and (inputs) the stereo switch. */
    class RoutingRow final : public juce::Component
    {
    public:
        RoutingRow()
        {
            for (auto* c : { (juce::Component*) &name, (juce::Component*) &left, (juce::Component*) &right, (juce::Component*) &stereo })
                addAndMakeVisible (c);
            name.setFont (juce::FontOptions (12.5f, juce::Font::bold));
            stereo.setColour (juce::TextButton::buttonOnColourId, Colours::accent);
        }

        void bind (Strip* s, bool outputs, const juce::StringArray& items)
        {
            strip = s;
            if (s == nullptr)
                return;
            name.setText (describeStrip (*s), juce::dontSendNotification);
            name.setColour (juce::Label::textColourId, s->getColour());
            if (items != shownItems || outputs != shownOutputs)
            {
                left.setItems (items, "OFF");
                right.setItems (items, outputs ? "OFF" : "NEXT");
                shownItems = items;
                shownOutputs = outputs;
            }
            left.bind (outputs ? &s->params.outputLeft : &s->params.inputLeft);
            right.bind (outputs ? &s->params.outputRight : &s->params.inputRight);
            stereo.setButtonText (outputs ? (s->isInput() ? "DIRECT OUT" : "STEREO") : "STEREO");
            stereo.bind (outputs ? nullptr : &s->params.stereo);
            stereo.setVisible (! outputs);
        }

        void resized() override
        {
            auto b = getLocalBounds().reduced (4, 3);
            name.setBounds (b.removeFromLeft (juce::jmax (140, b.getWidth() / 4)));
            stereo.setBounds (b.removeFromRight (90).reduced (4, 0));
            left.setBounds (b.removeFromLeft (b.getWidth() / 2).reduced (4, 0));
            right.setBounds (b.reduced (4, 0));
        }

    private:
        Strip* strip = nullptr;
        juce::Label name;
        IntCombo left, right;
        BoolButton stereo { "STEREO" };
        juce::StringArray shownItems;
        bool shownOutputs = false;
    };
} // namespace

class RoutingPage::Model final : public juce::ListBoxModel
{
public:
    explicit Model (UiContext& c) : ctx (c) {}

    void rebuild (int mode)
    {
        outputs = mode == 1;
        strips.clear();
        auto add = [this] (StripKind k)
        {
            for (auto& s : ctx.engine.getStrips (k))
                strips.push_back (s.get());
        };
        if (outputs)
        {
            for (auto k : { StripKind::Main, StripKind::Matrix, StripKind::Bus, StripKind::Channel, StripKind::Aux })
                add (k);
            items = numbered (ctx.engine.getHardwareOutputNames(), juce::jmax (64, ctx.engine.getHardwareOutputNames().size()));
        }
        else
        {
            add (StripKind::Channel);
            add (StripKind::Aux);
            items = numbered (ctx.engine.getHardwareInputNames(), juce::jmax (64, ctx.engine.getHardwareInputNames().size()));
        }
    }

    int getNumRows() override { return (int) strips.size(); }
    void paintListBoxItem (int, juce::Graphics&, int, int, bool) override {}

    juce::Component* refreshComponentForRow (int row, bool, juce::Component* existing) override
    {
        auto* r = dynamic_cast<RoutingRow*> (existing);
        if (r == nullptr)
        {
            delete existing;
            r = new RoutingRow();
        }
        r->bind (juce::isPositiveAndBelow (row, (int) strips.size()) ? strips[(size_t) row] : nullptr, outputs, items);
        r->setVisible (juce::isPositiveAndBelow (row, (int) strips.size()));
        return r;
    }

private:
    UiContext& ctx;
    std::vector<Strip*> strips;
    juce::StringArray items;
    bool outputs = false;
};

namespace
{
    class MonitorPanel final : public juce::Component
    {
    public:
        explicit MonitorPanel (UiContext& c) : ctx (c)
        {
            const auto outs = numbered (ctx.engine.getHardwareOutputNames(), juce::jmax (64, ctx.engine.getHardwareOutputNames().size()));
            left.setItems (outs, "OFF");
            right.setItems (outs, "OFF");
            left.bind (&ctx.engine.monitorOutputLeft);
            right.bind (&ctx.engine.monitorOutputRight);
            afl.bind (&ctx.engine.soloAfl);
            level.setBinding (bindLinear ("MONITOR", ctx.engine.monitorLevelDb, -60.0f, 10.0f, " dB", 1, 0.0f));
            for (auto* c2 : { (juce::Component*) &left, (juce::Component*) &right, (juce::Component*) &afl, (juce::Component*) &level,
                              (juce::Component*) &caption })
                addAndMakeVisible (c2);
            caption.setText ("Monitor A outputs (left / right). Without a solo the monitor follows Main 1.", juce::dontSendNotification);
            caption.setColour (juce::Label::textColourId, Colours::textDim);
        }

        void resized() override
        {
            auto b = getLocalBounds().reduced (10);
            caption.setBounds (b.removeFromTop (20));
            b.removeFromTop (6);
            auto row = b.removeFromTop (28);
            left.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (4, 0));
            right.setBounds (row.reduced (4, 0));
            b.removeFromTop (10);
            row = b.removeFromTop (90);
            afl.setBounds (row.removeFromLeft (160).withSizeKeepingCentre (150, 28));
            level.setBounds (row.removeFromLeft (110));
        }

    private:
        UiContext& ctx;
        IntCombo left, right;
        BoolButton afl { "SOLO AFL (else PFL)", Colours::solo };
        DisplayKnob level;
        juce::Label caption;
    };
} // namespace

RoutingPage::RoutingPage (UiContext& c) : ctx (c), model (std::make_unique<Model> (c))
{
    for (auto* b : { &inputsButton, &outputsButton, &monitorButton })
    {
        b->setColour (juce::TextButton::buttonOnColourId, Colours::accent);
        addAndMakeVisible (b);
    }
    inputsButton.onClick = [this] { setMode (0); };
    outputsButton.onClick = [this] { setMode (1); };
    monitorButton.onClick = [this] { setMode (2); };

    list.setModel (model.get());
    list.setRowHeight (34);
    list.setColour (juce::ListBox::backgroundColourId, Colours::screen);
    addAndMakeVisible (list);
    monitorPanel = std::make_unique<MonitorPanel> (ctx);
    addChildComponent (*monitorPanel);
    setMode (0);
}

RoutingPage::~RoutingPage()
{
    list.setModel (nullptr);
}

void RoutingPage::setMode (int m)
{
    mode = m;
    inputsButton.setToggleState (m == 0, juce::dontSendNotification);
    outputsButton.setToggleState (m == 1, juce::dontSendNotification);
    monitorButton.setToggleState (m == 2, juce::dontSendNotification);
    list.setVisible (m != 2);
    monitorPanel->setVisible (m == 2);
    refresh();
}

void RoutingPage::refresh()
{
    if (mode != 2)
    {
        model->rebuild (mode);
        list.updateContent();
        list.repaint();
    }
}

void RoutingPage::paint (juce::Graphics& g)
{
    g.fillAll (Colours::screen);
    if (mode == 2)
        return;

    // Same columns as RoutingRow.
    auto header = list.getBounds().withHeight (16).translated (0, -18).reduced (4, 0);
    header.setWidth (list.getWidth() - list.getVerticalScrollBar().getWidth() - 8);
    auto name = header.removeFromLeft (juce::jmax (140, header.getWidth() / 4));
    header.removeFromRight (90);
    auto left = header.removeFromLeft (header.getWidth() / 2);
    g.setColour (Colours::textDim);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText (mode == 0 ? "CHANNEL" : "STRIP", name, juce::Justification::centredLeft);
    g.drawText (mode == 0 ? "SOURCE L / MONO" : "OUTPUT L", left.reduced (4, 0), juce::Justification::centredLeft);
    g.drawText (mode == 0 ? "SOURCE R (stereo)" : "OUTPUT R", header.reduced (4, 0), juce::Justification::centredLeft);
}

void RoutingPage::resized()
{
    auto b = getLocalBounds().reduced (4);
    auto top = b.removeFromTop (32);
    for (auto* button : { &monitorButton, &outputsButton, &inputsButton })
        button->setBounds (top.removeFromRight (150).reduced (3, 2));
    b.removeFromTop (20);
    list.setBounds (b);
    monitorPanel->setBounds (b);
}

} // namespace livedaw::ui
