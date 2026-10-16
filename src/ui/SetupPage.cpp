#include "ui/Pages.h"
#include "ui/WingWidgets.h"

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

    /** GENERAL, like the console's: CONSOLE info, CONFIGURATION (channels and
        buses), PREFERENCES, CLOCK and INITIALIZE. */
    class GeneralTab final : public juce::Component, private juce::Timer
    {
    public:
        explicit GeneralTab (UiContext& c)
            : ctx (c),
              protection (c.engine.outputProtection),
              solo ({ "PFL", "AFL" }, [&c] { return c.engine.soloAfl.load() ? 1 : 0; }, [&c] (int i) { c.engine.soloAfl = i == 1; })
        {
            editKey.onClick = [this]
            {
                auto* w = new juce::AlertWindow ("Show name", "Name of the show (saved with it):", juce::MessageBoxIconType::NoIcon, this);
                w->addTextEditor ("name", ctx.show.getName());
                w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
                w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w] (int r)
                {
                    if (r == 1)
                        ctx.show.setName (w->getTextEditorContents ("name").trim());
                    repaint();
                }), true);
            };
            chMinus.onClick = [this] { setChannels ((int) ctx.engine.getChannels().size() - 1); };
            chPlus.onClick = [this] { setChannels ((int) ctx.engine.getChannels().size() + 1); };
            chWing.onClick = [this] { setChannels (kWingChannels); };
            chWing.isOn = [this] { return (int) ctx.engine.getChannels().size() == kWingChannels; };
            busPlus.onClick = [this]
            {
                if (auto* b = ctx.engine.addBus())
                {
                    ctx.log.info ("Added " + b->getLabel());
                    if (ctx.structureChanged)
                        ctx.structureChanged();
                }
            };
            busMinus.onClick = [this]
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

            ceiling.setSliderStyle (juce::Slider::LinearHorizontal);
            ceiling.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            ceiling.setRange (-24.0, 6.0, 0.1);
            ceiling.setDoubleClickReturnValue (true, 0.0);
            ceiling.onValueChange = [this] { ctx.engine.outputCeilingDb = (float) ceiling.getValue(); repaint(); };

            initKey.outlined = true;
            initKey.offColour = Colours::screenPanel;
            initKey.onClick = [this]
            {
                juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, "Initialize console",
                                                    "Reset every strip, DCA, mute group and FX setting to a fresh 40 channel WING? "
                                                    "Scenes are kept.", "Initialize", "Cancel", this,
                                                    juce::ModalCallbackFunction::create ([this] (int r)
                {
                    if (r == 0)
                        return;
                    for (int i = 0; i < kNumFx; ++i)
                        clearFx (ctx, i);
                    ctx.engine.reset();
                    ctx.engine.setNumChannels (kWingChannels);
                    ctx.log.info ("Console initialized");
                    if (ctx.structureChanged)
                        ctx.structureChanged();
                }));
            };

            for (juce::Component* comp : { (juce::Component*) &editKey, (juce::Component*) &chMinus, (juce::Component*) &chPlus,
                                           (juce::Component*) &chWing, (juce::Component*) &busMinus, (juce::Component*) &busPlus,
                                           (juce::Component*) &protection, (juce::Component*) &ceiling, (juce::Component*) &solo,
                                           (juce::Component*) &initKey })
                addAndMakeVisible (comp);
            startTimerHz (2);
            timerCallback();
        }

        void timerCallback() override
        {
            if (! ceiling.isMouseButtonDown())
                ceiling.setValue (ctx.engine.outputCeilingDb.load(), juce::dontSendNotification);
            busMinus.setEnabled (ctx.engine.getBuses().size() > (size_t) kWingBuses);
            chMinus.setEnabled (! ctx.engine.getChannels().empty());
            solo.repaint();
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Colours::screen);
            for (auto [box, title] : { std::pair { consoleBox, "CONSOLE" }, std::pair { configBox, "CONFIGURATION" },
                                       std::pair { prefsBox, "PREFERENCES" }, std::pair { clockBox, "CLOCK" }, std::pair { initBox, "INITIALIZE" } })
                drawSectionBox (g, box, title);

            // Console info: grey caption over a white value.
            auto* device = ctx.deviceManager.getCurrentAudioDevice();
            const std::pair<juce::String, juce::String> info[] = {
                { "SHOW NAME", ctx.show.getName().isNotEmpty() ? ctx.show.getName() : juce::String ("UNTITLED") },
                { "SOFTWARE", "LIVE DAW " + juce::String (JUCE_APPLICATION_VERSION_STRING) },
                { "ENGINE", "RUST, " + juce::String (ctx.engine.getNumWorkerThreads()) + " DSP THREADS" },
                { "AUDIO DEVICE", device != nullptr ? device->getName() + "  " + juce::String (juce::roundToInt (device->getCurrentSampleRate())) + " Hz"
                                                    : juce::String ("NONE") } };
            auto r = infoArea;
            const int rowH = r.getHeight() / (int) std::size (info);
            for (size_t i = 0; i < std::size (info); ++i)
            {
                auto row = r.removeFromTop (rowH);
                g.setColour (Colours::textDim);
                g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
                g.drawText (info[i].first, row.removeFromTop (16), juce::Justification::bottomLeft);
                g.setColour (Colours::text);
                g.setFont (juce::FontOptions (juce::jmin (i == 0 ? 26.0f : 19.0f, (float) row.getHeight() * 0.9f), juce::Font::bold));
                g.drawFittedText (info[i].second, row.withTrimmedRight (i == 0 ? editKey.getWidth() + 4 : 0), juce::Justification::centredLeft, 1);
            }

            // Configuration rows.
            g.setColour (Colours::text);
            g.setFont (juce::FontOptions (juce::jmin (20.0f, (float) chRow.getHeight() * 0.5f), juce::Font::bold));
            g.drawText ("CHANNELS", chRow, juce::Justification::centredLeft);
            g.drawText ("BUSES", busRow, juce::Justification::centredLeft);
            g.setFont (juce::FontOptions (juce::jmin (26.0f, (float) chRow.getHeight() * 0.7f), juce::Font::bold));
            g.drawText (juce::String ((int) ctx.engine.getChannels().size()), chValue, juce::Justification::centred);
            g.drawText (juce::String ((int) ctx.engine.getBuses().size()), busValue, juce::Justification::centred);
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.drawFittedText ("Only the CPU limits channels and buses. Buses 9 and up can feed buses 1-8.", configNote, juce::Justification::topLeft, 2);

            // Preference rows.
            g.setColour (juce::Colour (0xff1c1c1c));
            for (int i = 1; i < (int) std::size (prefRows); ++i)
                g.fillRect (prefRows[i].withHeight (2).translated (0, -1));
            g.setColour (Colours::text);
            g.setFont (juce::FontOptions (juce::jmin (19.0f, (float) prefRows[0].getHeight() * 0.42f), juce::Font::bold));
            const char* prefNames[] = { "OUTPUT PROTECTION", "PROTECTION CEILING", "SOLO MODE" };
            for (size_t i = 0; i < std::size (prefRows); ++i)
                g.drawText (prefNames[i], prefRows[i].reduced (10, 0), juce::Justification::centredLeft);
            g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
            g.drawText (juce::String (ctx.engine.outputCeilingDb.load(), 1) + " dB", ceilingValue, juce::Justification::centredRight);

            // Clock: time and date boxes.
            const auto now = juce::Time::getCurrentTime();
            for (auto [box, title, value] : { std::tuple { timeBox, "TIME", now.formatted ("%H:%M:%S") }, std::tuple { dateBox, "DATE", now.formatted ("%Y-%m-%d") } })
            {
                g.setColour (juce::Colour (0xff1e1e1e));
                g.fillRect (box);
                g.setColour (Colours::screenTile);
                g.drawRect (box, 1);
                auto t = box.reduced (8, 4);
                g.setColour (Colours::text);
                g.setFont (juce::FontOptions (juce::jmin (17.0f, (float) t.getHeight() * 0.36f), juce::Font::bold));
                g.drawText (title, t.removeFromTop (t.getHeight() * 4 / 10), juce::Justification::bottomLeft);
                g.setFont (juce::FontOptions (juce::jmin (26.0f, (float) t.getHeight() * 0.8f), juce::Font::bold));
                g.drawText (value, t, juce::Justification::centredLeft);
            }
        }

        void resized() override
        {
            auto b = getLocalBounds().reduced (0, 6);
            const int gap = juce::jmax (8, b.getWidth() / 24);
            auto left = b.removeFromLeft (b.getWidth() * 30 / 100);
            b.removeFromLeft (gap);
            auto right = b.removeFromRight (b.getWidth() * 33 / 100);
            b.removeFromRight (gap);
            auto middle = b;

            consoleBox = left.removeFromTop (left.getHeight() * 56 / 100);
            left.removeFromTop (6);
            configBox = left;
            prefsBox = middle.withHeight (juce::jmin (middle.getHeight(), kPanelHeaderHeight + 30 + 3 * 54));
            clockBox = right.removeFromTop (right.getHeight() * 56 / 100);
            right.removeFromTop (6);
            initBox = right;

            infoArea = sectionContent (consoleBox, true);
            editKey.setBounds (infoArea.getRight() - 56, infoArea.getY() + 4, 56, 40);

            auto cfg = sectionContent (configBox, true);
            const int rowH = juce::jmin (40, cfg.getHeight() / 3);
            for (auto [row, minus, value, plus] : { std::tuple { &chRow, &chMinus, &chValue, &chPlus }, std::tuple { &busRow, &busMinus, &busValue, &busPlus } })
            {
                auto r = cfg.removeFromTop (rowH);
                cfg.removeFromTop (4);
                *row = r;
                auto keys = r.removeFromRight (juce::jmin (r.getWidth() * 62 / 100, 220));
                if (row == &chRow)
                {
                    chWing.setBounds (keys.removeFromRight (keys.getWidth() / 3).reduced (2, 2));
                    keys.removeFromRight (4);
                }
                minus->setBounds (keys.removeFromLeft (rowH).reduced (2));
                plus->setBounds (keys.removeFromRight (rowH).reduced (2));
                *value = keys;
            }
            busValue = busValue.withWidth (chValue.getWidth());
            busPlus.setBounds (busPlus.getBounds().withX (chPlus.getX()));
            configNote = cfg;

            auto prefs = sectionContent (prefsBox, true).expanded (8, 6);
            const int ph = prefs.getHeight() / (int) std::size (prefRows);
            for (auto& row : prefRows)
                row = prefs.removeFromTop (ph);
            protection.setBounds (prefRows[0].reduced (10, 0).removeFromRight (48).withSizeKeepingCentre (48, juce::jmin (36, ph - 12)));
            auto c = prefRows[1].reduced (10, 0).removeFromRight (prefRows[1].getWidth() / 2);
            ceilingValue = c.removeFromLeft (64);
            ceiling.setBounds (c.withSizeKeepingCentre (c.getWidth() - 6, 24));
            solo.setBounds (prefRows[2].reduced (10, 0).removeFromRight (130).withSizeKeepingCentre (130, juce::jmin (40, ph - 10)));

            auto clock = sectionContent (clockBox, true);
            const int boxH = juce::jmin (64, clock.getHeight() / 2 - 8);
            timeBox = clock.removeFromTop (boxH).withTrimmedRight (clock.getWidth() / 6);
            clock.removeFromTop (12);
            dateBox = clock.removeFromTop (boxH).withTrimmedRight (clock.getWidth() / 6);

            initKey.setBounds (sectionContent (initBox, true).withSizeKeepingCentre (juce::jmin (210, initBox.getWidth() - 30), 56));
        }

    private:
        void setChannels (int n)
        {
            n = juce::jlimit (0, 4096, n);
            if (n == (int) ctx.engine.getChannels().size())
                return;
            ctx.engine.setNumChannels (n);
            ctx.log.info ("Channel count set to " + juce::String (n));
            if (ctx.structureChanged)
                ctx.structureChanged();
        }

        UiContext& ctx;
        WingKey editKey { "EDIT", Icon::Edit }, chMinus { "-" }, chPlus { "+" }, chWing { "WING" }, busMinus { "-" }, busPlus { "+" },
            initKey { "INIT CONSOLE", Icon::Reset };
        OnChip protection;
        juce::Slider ceiling;
        SegmentedKeys solo;
        juce::Rectangle<int> consoleBox, configBox, prefsBox, clockBox, initBox, infoArea, chRow, busRow, chValue, busValue, configNote,
            ceilingValue, timeBox, dateBox;
        juce::Rectangle<int> prefRows[3];
    };

    /** Width of each fader section, as segmented keys. */
    class SurfaceTab final : public juce::Component
    {
    public:
        explicit SurfaceTab (UiContext& c) : ctx (c)
        {
            static constexpr int widths[] = { LayerSet::kAutoWidth, 4, 8, 12, 16, 24, 32 };
            for (int s = 0; s < LayerSet::kNumSections; ++s)
            {
                const auto section = static_cast<LayerSet::Section> (s);
                rows.add (new SegmentedKeys ({ "AUTO", "4", "8", "12", "16", "24", "32" },
                                             [this, section]
                                             {
                                                 const int w = ctx.show.layers.getWidth (section);
                                                 for (int i = 0; i < (int) std::size (widths); ++i)
                                                     if (widths[i] == w)
                                                         return i;
                                                 return -1;
                                             },
                                             [this, section] (int i)
                                             {
                                                 ctx.show.layers.setWidth (section, widths[i]);
                                                 notifyLayersChangedAsync (ctx);
                                             }));
                addAndMakeVisible (rows.getLast());
            }
        }

        void refresh()
        {
            for (auto* r : rows)
                r->repaint();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Colours::screen);
            drawSectionBox (g, box, "FADER SECTIONS");
            const char* names[] = { "LEFT SECTION", "CENTER SECTION", "RIGHT SECTION" };
            const char* wing[] = { "WING: 12", "WING: 8", "WING: 4" };
            for (int s = 0; s < LayerSet::kNumSections; ++s)
            {
                auto r = labels[s];
                g.setColour (Colours::text);
                g.setFont (juce::FontOptions (juce::jmin (20.0f, (float) r.getHeight() * 0.42f), juce::Font::bold));
                g.drawText (names[s], r.removeFromTop (r.getHeight() * 6 / 10), juce::Justification::bottomLeft);
                g.setColour (Colours::textDim);
                g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
                g.drawText (wing[s], r, juce::Justification::topLeft);
            }
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
            g.drawFittedText ("AUTO suits wide screens: the section shows as many faders as fit. Banks are cut to the section's width.", hint,
                              juce::Justification::topLeft, 2);
        }

        void resized() override
        {
            box = getLocalBounds().reduced (0, 6).withWidth (juce::jmin (getWidth(), 760));
            auto b = sectionContent (box, true);
            const int h = juce::jmin (60, b.getHeight() / 4);
            for (int s = 0; s < LayerSet::kNumSections; ++s)
            {
                auto row = b.removeFromTop (h);
                labels[s] = row.removeFromLeft (row.getWidth() * 32 / 100);
                rows[s]->setBounds (row.withSizeKeepingCentre (row.getWidth(), juce::jmin (42, h - 12)));
            }
            hint = b.reduced (0, 6);
        }

    private:
        UiContext& ctx;
        juce::OwnedArray<SegmentedKeys> rows;
        juce::Rectangle<int> box, labels[LayerSet::kNumSections], hint;
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
            list.setRowHeight (30);
            list.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff1e1e1e));
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
            auto badge = r.removeFromRight (130);
            drawChip (g, badge.toFloat(), safe ? "RECALL SAFE" : "-", safe ? Colours::mute : juce::Colour (0xff4b4b4b),
                      safe ? juce::Colours::black : Colours::textDim);
            g.setColour (s->getColour());
            g.fillRect (r.removeFromLeft (6));
            r.removeFromLeft (8);
            g.setColour (Colours::text);
            g.setFont (juce::FontOptions (17.0f, juce::Font::bold));
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

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Colours::screen);
            drawSectionBox (g, getLocalBounds().reduced (0, 6), "GLOBAL SAFES");
        }

        void resized() override
        {
            auto b = sectionContent (getLocalBounds().reduced (0, 6), true);
            auto top = b.removeFromTop (34);
            allOff.setBounds (top.removeFromRight (160));
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

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Colours::screen);
            drawSectionBox (g, dcaBox, "DCA NAMES");
            drawSectionBox (g, mgBox, "MUTE GROUP NAMES");
        }

        void resized() override
        {
            auto all = getLocalBounds().reduced (0, 6);
            dcaBox = all.removeFromLeft (all.getWidth() * 2 / 3);
            mgBox = all.withTrimmedLeft (6);
            auto b = sectionContent (mgBox, true);
            auto left = sectionContent (dcaBox, true);
            dcaCaption.setVisible (false);
            mgCaption.setVisible (false);
            const int h = juce::jmin (36, left.getHeight() / 8);
            const int w = left.getWidth() / 2;
            for (int i = 0; i < kNumDcas; ++i)
            {
                auto r = juce::Rectangle<int> (left.getX() + (i / 8) * w, left.getY() + (i % 8) * h, w - 8, h - 4);
                dcaColours[i]->setBounds (r.removeFromLeft (22));
                dcaNames[i]->setBounds (r.withTrimmedLeft (4));
            }
            for (int i = 0; i < kNumMuteGroups; ++i)
                mgNames[i]->setBounds (b.removeFromTop (h).reduced (0, 2));
        }

    private:
        UiContext& ctx;
        juce::Rectangle<int> dcaBox, mgBox;
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
            styleCaption (info, "", 16.0f, Colours::text);
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

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Colours::screen);
            drawSectionBox (g, getLocalBounds().reduced (0, 6).withWidth (juce::jmin (getWidth(), 760)), "PLUGINS");
        }

        void resized() override
        {
            auto b = sectionContent (getLocalBounds().reduced (0, 6).withWidth (juce::jmin (getWidth(), 760)), true);
            info.setBounds (b.removeFromTop (90));
            b.removeFromTop (8);
            scan.setBounds (b.removeFromTop (48).removeFromLeft (260));
        }

    private:
        UiContext& ctx;
        juce::Label info;
        WingKey scan { "SCAN FOR PLUGINS", Icon::Folder };
    };

    class AudioTab final : public juce::Component
    {
    public:
        explicit AudioTab (UiContext& c)
            : selector (c.deviceManager, 0, 256, 0, 256, false, false, false, false)
        {
            addAndMakeVisible (selector);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Colours::screen);
            drawSectionBox (g, getLocalBounds().reduced (0, 6), "AUDIO DEVICE");
        }

        void resized() override { selector.setBounds (sectionContent (getLocalBounds().reduced (0, 6), true)); }

    private:
        juce::AudioDeviceSelectorComponent selector;
    };
} // namespace

SetupPage::SetupPage (UiContext& c) : ctx (c)
{
    tabs.setTabBarDepth (44);
    tabs.setOutline (0);
    tabs.setColour (juce::TabbedComponent::backgroundColourId, juce::Colours::transparentBlack);
    const auto tabColour = Colours::screenPanel;
    tabs.addTab ("GENERAL", tabColour, new GeneralTab (ctx), true);
    tabs.addTab ("AUDIO", tabColour, new AudioTab (ctx), true);
    tabs.addTab ("SURFACE", tabColour, new SurfaceTab (ctx), true);
    tabs.addTab ("SAFES", tabColour, new SafesTab (ctx), true);
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
    // The console's tab bar sits on a grey strip.
    g.setColour (Colours::screenHeader.darker (0.15f));
    g.fillRect (getLocalBounds().withHeight (tabs.getTabBarDepth()));
}

void SetupPage::resized()
{
    tabs.setBounds (getLocalBounds());
}

} // namespace livedaw::ui
