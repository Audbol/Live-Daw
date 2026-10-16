#include "ui/Pages.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace livedaw::ui
{

namespace
{
    void styleCaption (juce::Label& l, const juce::String& text, float size = 12.0f, juce::Colour c = Colours::textDim)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (juce::FontOptions (size, juce::Font::bold));
        l.setColour (juce::Label::textColourId, c);
    }

    /** Channels and buses, output protection, console reset. */
    class ConsoleTab final : public juce::Component, private juce::Timer
    {
    public:
        explicit ConsoleTab (UiContext& c) : ctx (c)
        {
            styleCaption (channelsCaption, "CHANNELS (the WING has 40; only CPU limits the count here)");
            channels.setSliderStyle (juce::Slider::IncDecButtons);
            channels.setRange (0, 4096, 1);
            channels.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 70, 26);
            channels.onValueChange = [this]
            {
                const int n = (int) channels.getValue();
                if (n != (int) ctx.engine.getChannels().size())
                {
                    ctx.engine.setNumChannels (n);
                    ctx.log.info ("Channel count set to " + juce::String (n));
                    if (ctx.structureChanged)
                        ctx.structureChanged();
                }
            };
            wingButton.onClick = [this] { channels.setValue (kWingChannels); };

            styleCaption (busesCaption, "BUSES (16 on the WING; buses 9+ can feed buses 1-8)");
            addBus.onClick = [this]
            {
                if (auto* b = ctx.engine.addBus())
                {
                    ctx.log.info ("Added " + b->getLabel());
                    if (ctx.structureChanged)
                        ctx.structureChanged();
                }
            };
            removeBus.onClick = [this]
            {
                auto& buses = ctx.engine.getBuses();
                if (buses.size() > (size_t) kWingBuses)
                {
                    auto* last = buses.back().get();
                    const auto label = last->getLabel();
                    ctx.engine.removeStrip (last);
                    ctx.log.info ("Removed " + label);
                    if (ctx.structureChanged)
                        ctx.structureChanged();
                }
            };

            styleCaption (protectionCaption, "OUTPUT PROTECTION");
            protection.bind (&ctx.engine.outputProtection);
            ceiling.setBinding (bindLinear ("CEILING", ctx.engine.outputCeilingDb, -24.0f, 6.0f, " dB", 1, 0.0f));

            resetButton.setColour (juce::TextButton::buttonColourId, Colours::mute.darker (0.5f));
            resetButton.onClick = [this]
            {
                juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, "Reset console",
                                                    "Reset every strip, DCA, mute group and FX setting to a fresh 40 channel WING? "
                                                    "Scenes are kept.", "Reset", "Cancel", this,
                                                    juce::ModalCallbackFunction::create ([this] (int r)
                {
                    if (r == 0)
                        return;
                    for (int i = 0; i < kNumFx; ++i)
                        clearFx (ctx, i);
                    ctx.engine.reset();
                    ctx.engine.setNumChannels (kWingChannels);
                    ctx.log.info ("Console reset");
                    if (ctx.structureChanged)
                        ctx.structureChanged();
                }));
            };

            for (juce::Component* comp : { (juce::Component*) &channelsCaption, (juce::Component*) &channels, (juce::Component*) &wingButton,
                                           (juce::Component*) &busesCaption, (juce::Component*) &busCount, (juce::Component*) &addBus,
                                           (juce::Component*) &removeBus, (juce::Component*) &protectionCaption, (juce::Component*) &protection,
                                           (juce::Component*) &ceiling, (juce::Component*) &resetButton })
                addAndMakeVisible (comp);
            startTimerHz (4);
            timerCallback();
        }

        void timerCallback() override
        {
            if (! channels.isMouseButtonDown())
                channels.setValue ((double) ctx.engine.getChannels().size(), juce::dontSendNotification);
            busCount.setText (juce::String ((int) ctx.engine.getBuses().size()) + " buses", juce::dontSendNotification);
            removeBus.setEnabled (ctx.engine.getBuses().size() > (size_t) kWingBuses);
        }

        void paint (juce::Graphics& g) override { g.fillAll (Colours::screen); }

        void resized() override
        {
            auto b = getLocalBounds().reduced (12);
            channelsCaption.setBounds (b.removeFromTop (18));
            auto row = b.removeFromTop (30);
            channels.setBounds (row.removeFromLeft (160));
            wingButton.setBounds (row.removeFromLeft (110).reduced (6, 0));
            b.removeFromTop (14);
            busesCaption.setBounds (b.removeFromTop (18));
            row = b.removeFromTop (30);
            busCount.setBounds (row.removeFromLeft (100));
            addBus.setBounds (row.removeFromLeft (110).reduced (4, 0));
            removeBus.setBounds (row.removeFromLeft (130).reduced (4, 0));
            b.removeFromTop (14);
            protectionCaption.setBounds (b.removeFromTop (18));
            row = b.removeFromTop (80);
            protection.setBounds (row.removeFromLeft (160).withSizeKeepingCentre (150, 28));
            ceiling.setBounds (row.removeFromLeft (100));
            resetButton.setBounds (b.removeFromBottom (30).removeFromLeft (180));
        }

    private:
        UiContext& ctx;
        juce::Label channelsCaption, busesCaption, busCount, protectionCaption;
        juce::Slider channels;
        juce::TextButton wingButton { "WING (40)" }, addBus { "+ ADD BUS" }, removeBus { "- REMOVE BUS" },
            resetButton { "RESET CONSOLE..." };
        BoolButton protection { "PROTECTION ON", Colours::active };
        DisplayKnob ceiling;
    };

    /** Width of each fader section. */
    class SurfaceTab final : public juce::Component
    {
    public:
        explicit SurfaceTab (UiContext& c) : ctx (c)
        {
            const char* names[] = { "LEFT SECTION (WING: 12)", "CENTER SECTION (WING: 8)", "RIGHT SECTION (WING: 4)" };
            for (int s = 0; s < LayerSet::kNumSections; ++s)
            {
                auto* l = captions.add (new juce::Label());
                styleCaption (*l, names[s]);
                addAndMakeVisible (l);
                auto* box = boxes.add (new juce::ComboBox());
                box->addItem ("AUTO (fit the window)", 1);
                for (int w : { 4, 6, 8, 10, 12, 16, 20, 24, 32, 48 })
                    box->addItem (juce::String (w) + " faders", w + 1);
                box->onChange = [this, s, box]
                {
                    const int id = box->getSelectedId();
                    if (id <= 0)
                        return;
                    ctx.show.layers.setWidth (static_cast<LayerSet::Section> (s), id == 1 ? LayerSet::kAutoWidth : id - 1);
                    notifyLayersChangedAsync (ctx);
                };
                addAndMakeVisible (box);
            }
            styleCaption (hint, "AUTO suits wide screens: the section shows as many faders as fit. "
                                "Banks are cut to the section's width.", 12.0f);
            addAndMakeVisible (hint);
            refresh();
        }

        void refresh()
        {
            for (int s = 0; s < LayerSet::kNumSections; ++s)
            {
                const int w = ctx.show.layers.getWidth (static_cast<LayerSet::Section> (s));
                boxes[s]->setSelectedId (w == 0 ? 1 : w + 1, juce::dontSendNotification);
                if (boxes[s]->getSelectedId() == 0)
                {
                    boxes[s]->addItem (juce::String (w) + " faders", w + 1);
                    boxes[s]->setSelectedId (w + 1, juce::dontSendNotification);
                }
            }
        }

        void paint (juce::Graphics& g) override { g.fillAll (Colours::screen); }

        void resized() override
        {
            auto b = getLocalBounds().reduced (12);
            for (int s = 0; s < LayerSet::kNumSections; ++s)
            {
                captions[s]->setBounds (b.removeFromTop (18));
                boxes[s]->setBounds (b.removeFromTop (28).removeFromLeft (260));
                b.removeFromTop (12);
            }
            hint.setBounds (b.removeFromTop (40));
        }

    private:
        UiContext& ctx;
        juce::OwnedArray<juce::Label> captions;
        juce::OwnedArray<juce::ComboBox> boxes;
        juce::Label hint;
    };

    /** GLOBAL safes: strips that scene recalls leave alone. */
    class SafesTab final : public juce::Component, private juce::ListBoxModel
    {
    public:
        explicit SafesTab (UiContext& c) : ctx (c)
        {
            styleCaption (hint, "RECALL SAFE strips are skipped by scene recalls (snippets still apply).");
            addAndMakeVisible (hint);
            list.setModel (this);
            list.setRowHeight (24);
            list.setColour (juce::ListBox::backgroundColourId, Colours::screen);
            addAndMakeVisible (list);
            allOff.onClick = [this]
            {
                for (auto* s : strips)
                    s->params.recallSafe = false;
                list.repaint();
            };
            addAndMakeVisible (allOff);
            refresh();
        }

        ~SafesTab() override { list.setModel (nullptr); }

        void refresh()
        {
            strips.clear();
            for (int k = 0; k < kNumStripKinds; ++k)
                for (auto& s : ctx.engine.getStrips (static_cast<StripKind> (k)))
                    strips.push_back (s.get());
            list.updateContent();
            list.repaint();
        }

        int getNumRows() override { return (int) strips.size(); }

        void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool) override
        {
            if (! juce::isPositiveAndBelow (row, (int) strips.size()))
                return;
            auto* s = strips[(size_t) row];
            auto r = juce::Rectangle<int> (width, height).reduced (8, 2);
            const bool safe = s->params.recallSafe.load();
            auto badge = r.removeFromRight (120);
            g.setColour (safe ? Colours::mute : Colours::panelLight);
            g.fillRoundedRectangle (badge.toFloat(), 3.0f);
            g.setColour (safe ? juce::Colours::black : Colours::textDim);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText (safe ? "RECALL SAFE" : "-", badge, juce::Justification::centred);
            g.setColour (s->getColour());
            g.fillRect (r.removeFromLeft (4));
            r.removeFromLeft (8);
            g.setColour (Colours::text);
            g.setFont (juce::FontOptions (13.0f));
            g.drawText (describeStrip (*s), r, juce::Justification::centredLeft);
        }

        void listBoxItemClicked (int row, const juce::MouseEvent&) override
        {
            if (juce::isPositiveAndBelow (row, (int) strips.size()))
            {
                auto& p = strips[(size_t) row]->params.recallSafe;
                p = ! p.load();
                list.repaintRow (row);
            }
        }

        void paint (juce::Graphics& g) override { g.fillAll (Colours::screen); }

        void resized() override
        {
            auto b = getLocalBounds().reduced (12);
            auto top = b.removeFromTop (26);
            allOff.setBounds (top.removeFromRight (140));
            hint.setBounds (top);
            b.removeFromTop (6);
            list.setBounds (b);
        }

    private:
        UiContext& ctx;
        juce::Label hint;
        juce::ListBox list;
        juce::TextButton allOff { "ALL SAFES OFF" };
        std::vector<Strip*> strips;
    };

    /** DCA and mute group names and colours. */
    class GroupsTab final : public juce::Component
    {
    public:
        explicit GroupsTab (UiContext& c) : ctx (c)
        {
            styleCaption (dcaCaption, "DCA NAMES");
            styleCaption (mgCaption, "MUTE GROUP NAMES");
            addAndMakeVisible (dcaCaption);
            addAndMakeVisible (mgCaption);
            for (int i = 0; i < kNumDcas; ++i)
            {
                auto* e = dcaNames.add (new juce::TextEditor());
                e->onTextChange = [this, i, e] { ctx.engine.setDcaName (i, e->getText()); };
                addAndMakeVisible (e);
                auto* col = dcaColours.add (new juce::TextButton());
                col->onClick = [this, i, col]
                {
                    juce::PopupMenu m;
                    for (int k = 1; k <= 18; ++k)
                        m.addColouredItem (k, wingColourName (k), wingColour (k), true, ctx.engine.getDca (i).colour.load() == k);
                    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (col), [this, i, col] (int r)
                    {
                        if (r > 0)
                        {
                            ctx.engine.getDca (i).colour = r;
                            col->setColour (juce::TextButton::buttonColourId, wingColour (r));
                        }
                    });
                };
                addAndMakeVisible (col);
            }
            for (int i = 0; i < kNumMuteGroups; ++i)
            {
                auto* e = mgNames.add (new juce::TextEditor());
                e->onTextChange = [this, i, e] { ctx.engine.setMuteGroupName (i, e->getText()); };
                addAndMakeVisible (e);
            }
            refresh();
        }

        void refresh()
        {
            for (int i = 0; i < kNumDcas; ++i)
            {
                dcaNames[i]->setText (ctx.engine.getDcaName (i), false);
                dcaColours[i]->setColour (juce::TextButton::buttonColourId, wingColour (ctx.engine.getDca (i).colour.load()));
            }
            for (int i = 0; i < kNumMuteGroups; ++i)
                mgNames[i]->setText (ctx.engine.getMuteGroupName (i), false);
        }

        void paint (juce::Graphics& g) override { g.fillAll (Colours::screen); }

        void resized() override
        {
            auto b = getLocalBounds().reduced (12);
            auto left = b.removeFromLeft (b.getWidth() * 2 / 3);
            dcaCaption.setBounds (left.removeFromTop (18));
            const int h = juce::jmin (28, left.getHeight() / 8);
            const int w = left.getWidth() / 2;
            for (int i = 0; i < kNumDcas; ++i)
            {
                auto r = juce::Rectangle<int> (left.getX() + (i / 8) * w, left.getY() + (i % 8) * h, w - 8, h - 4);
                dcaColours[i]->setBounds (r.removeFromLeft (22));
                dcaNames[i]->setBounds (r.withTrimmedLeft (4));
            }
            b.removeFromLeft (8);
            mgCaption.setBounds (b.removeFromTop (18));
            for (int i = 0; i < kNumMuteGroups; ++i)
                mgNames[i]->setBounds (b.removeFromTop (h).reduced (0, 2));
        }

    private:
        UiContext& ctx;
        juce::Label dcaCaption, mgCaption;
        juce::OwnedArray<juce::TextEditor> dcaNames, mgNames;
        juce::OwnedArray<juce::TextButton> dcaColours;
    };

    /** Plugin scanning (the plugins become FX slot devices). */
    class PluginsTab final : public juce::Component, private juce::ChangeListener
    {
    public:
        explicit PluginsTab (UiContext& c) : ctx (c)
        {
            styleCaption (info, "");
            scan.onClick = [this] { if (ctx.showPluginScanner) ctx.showPluginScanner(); };
            addAndMakeVisible (info);
            addAndMakeVisible (scan);
            ctx.host.getKnownPlugins().addChangeListener (this);
            update();
        }

        ~PluginsTab() override { ctx.host.getKnownPlugins().removeChangeListener (this); }

        void changeListenerCallback (juce::ChangeBroadcaster*) override { update(); }

        void update()
        {
            const auto types = ctx.host.getSortedTypes();
            juce::String text;
            text << types.size() << " plugins known (VST3, AU, LV2" << (ctx.host.getFormatManager().getNumFormats() > 3 ? ", VST2" : "")
                 << "). Load them into FX slots on the EFFECTS screen, then assign the slots to channel inserts.\n"
                 << "Plugins are scanned in a separate process, so a crashing plugin can't take the console down.";
            info.setText (text, juce::dontSendNotification);
        }

        void paint (juce::Graphics& g) override { g.fillAll (Colours::screen); }

        void resized() override
        {
            auto b = getLocalBounds().reduced (12);
            info.setBounds (b.removeFromTop (60));
            scan.setBounds (b.removeFromTop (30).removeFromLeft (200));
        }

    private:
        UiContext& ctx;
        juce::Label info;
        juce::TextButton scan { "SCAN FOR PLUGINS..." };
    };

    class AudioTab final : public juce::Component
    {
    public:
        explicit AudioTab (UiContext& c)
            : selector (c.deviceManager, 0, 256, 0, 256, false, false, false, false)
        {
            addAndMakeVisible (selector);
        }

        void paint (juce::Graphics& g) override { g.fillAll (Colours::screen); }
        void resized() override { selector.setBounds (getLocalBounds().reduced (8)); }

    private:
        juce::AudioDeviceSelectorComponent selector;
    };
} // namespace

SetupPage::SetupPage (UiContext& c) : ctx (c)
{
    tabs.setTabBarDepth (30);
    tabs.setColour (juce::TabbedComponent::backgroundColourId, Colours::screen);
    const auto tabColour = Colours::screenPanel;
    tabs.addTab ("AUDIO", tabColour, new AudioTab (ctx), true);
    tabs.addTab ("CONSOLE", tabColour, new ConsoleTab (ctx), true);
    tabs.addTab ("SURFACE", tabColour, new SurfaceTab (ctx), true);
    tabs.addTab ("GLOBAL SAFES", tabColour, new SafesTab (ctx), true);
    tabs.addTab ("DCA / MUTE", tabColour, new GroupsTab (ctx), true);
    tabs.addTab ("PLUGINS", tabColour, new PluginsTab (ctx), true);
    addAndMakeVisible (tabs);
}

SetupPage::~SetupPage() = default;

void SetupPage::refresh()
{
    for (int i = 0; i < tabs.getNumTabs(); ++i)
    {
        if (auto* s = dynamic_cast<SafesTab*> (tabs.getTabContentComponent (i)))
            s->refresh();
        if (auto* g = dynamic_cast<GroupsTab*> (tabs.getTabContentComponent (i)))
            g->refresh();
        if (auto* f = dynamic_cast<SurfaceTab*> (tabs.getTabContentComponent (i)))
            f->refresh();
    }
}

void SetupPage::paint (juce::Graphics& g)
{
    g.fillAll (Colours::screen);
}

void SetupPage::resized()
{
    tabs.setBounds (getLocalBounds());
}

} // namespace livedaw::ui
