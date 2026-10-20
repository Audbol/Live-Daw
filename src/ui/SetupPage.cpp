#include "engine/Models.h"
#include "engine/AppPaths.h"
#include "ui/CustomControlsPage.h"
#include "ui/MidiRemoteUi.h"
#include "ui/MonitorsPage.h"
#include "ui/Pages.h"
#include "ui/ScreenWidgets.h"
#include "ui/StartupNotice.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace livedaw::ui
{

// SETUP (manual 5.5): five tabs GENERAL | AUDIO | SURFACE | REMOTE | DAW with
// INITIALIZE CONSOLE and SHUTDOWN on the tab row, and an app-specific PLUGINS
// tab. Settings that only make sense on the hardware are drawn disabled so the
// screens keep the console's layout; so are the ones waiting for engine or
// support from files this page doesn't own (each marked with its item).

namespace
{
    /** Row height that fits `rows` rows in `height` px. */
    /** A settings row height that fits `rows` rows into `height`, at least
        36 of the WING's 800 px screen (18 px at 1920x1080, 12 px at 1280x800). */
    int minRowHeight() { return juce::jlimit (12, screenCap (18), juce::roundToInt (getWingScreenHeight() * 0.036f)); }

    int fitRowHeight (int height, float rows, int minH = -1, int maxH = -1)
    {
        if (minH < 0)
            minH = minRowHeight();
        if (maxH < 0)
            maxH = screenCap (30);
        return juce::jlimit (minH, juce::jmax (minH, maxH), (int) ((float) height / juce::jmax (1.0f, rows)));
    }

    std::unique_ptr<ChoiceKeys> fixedChoice (juce::StringArray labels, int lit, std::vector<juce::Colour> colours = {})
    {
        auto k = std::make_unique<ChoiceKeys> (std::move (labels), [lit] { return lit; }, nullptr, std::move (colours));
        k->setEnabled (false);
        return k;
    }

    // ALT inputs and ALT-selected tiles [3.1].
    const juce::Colour altOrange = Colours::alt;

    /** Independent toggle keys, each a boolean setting (CH AUTOSELECT L C R,
        USER LAYER LINK L/C C/R): lit blue when on. */
    class ToggleKeys final : public juce::Component
    {
    public:
        ToggleKeys (UiContext& c, juce::StringArray labels, std::vector<const char*> settingKeys)
            : ctx (c), names (std::move (labels)), keys (std::move (settingKeys))
        {
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            const float w = r.getWidth() / (float) juce::jmax (1, names.size());
            for (int i = 0; i < names.size(); ++i)
            {
                auto k = juce::Rectangle<float> (r.getX() + w * (float) i, r.getY(), w, r.getHeight()).reduced (1.0f, 0.0f);
                const bool on = isOn (i);
                g.setColour (on ? Dark::choice : Dark::key);
                g.fillRect (k);
                g.setColour (on ? Dark::choiceInk : Dark::text);
                g.setFont (wingFontFit (TypeScale::body, juce::jmin (k.getHeight() * 0.5f, k.getWidth() * 0.4f)));
                g.drawFittedText (names[i], k.toNearestInt().reduced (2, 0), juce::Justification::centred, 1, 0.7f);
            }
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (names.isEmpty() || ! getLocalBounds().contains (e.getPosition()))
                return;
            const int i = juce::jlimit (0, names.size() - 1, e.x * names.size() / juce::jmax (1, getWidth()));
            ctx.settings.setValue (keys[(size_t) i], ! isOn (i));
            repaint();
        }

    private:
        bool isOn (int i) const { return i < (int) keys.size() && ctx.settings.getBoolValue (keys[(size_t) i], false); }

        UiContext& ctx;
        juce::StringArray names;
        std::vector<const char*> keys;
    };

    void editText (juce::Component* parent, const juce::String& title, const juce::String& current, std::function<void (const juce::String&)> done)
    {
        popups::TextOptions options;
        options.maxLength = 24;
        popups::editText (*parent, title, current, [done] (const juce::String& text) { done (text.trim()); }, options);
    }

    juce::File dataDirectory()
    {
        return paths::appData();
    }

    //==============================================================================
    /** A lighting tile of SETUP > GENERAL: a ring gauge and "NN %". Drag up or
        down, or turn the wheel, to change it. */
    class LightTile final : public juce::Component
    {
    public:
        LightTile (UiContext& c, juce::String captionText, juce::String id, int defaultValue, juce::Colour tintColour)
            : ctx (c), caption (std::move (captionText)), key (prefs::brightnessPrefix + id), fallback (defaultValue), tint (tintColour)
        {
        }

        int value() const { return isEnabled() ? juce::jlimit (0, 100, ctx.settings.getIntValue (key, fallback)) : 0; }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            const float alpha = isEnabled() ? 1.0f : 0.45f;
            g.setColour (tint.withMultipliedAlpha (alpha));
            g.fillRect (r);
            auto ring = r.removeFromLeft (r.getHeight()).reduced (r.getHeight() * 0.16f);
            const float a0 = -juce::MathConstants<float>::pi * 0.78f, a1 = juce::MathConstants<float>::pi * 0.78f;
            const float rad = ring.getWidth() * 0.4f, thick = juce::jmax (2.0f, ring.getWidth() * 0.16f);
            juce::Path track, arc;
            track.addCentredArc (ring.getCentreX(), ring.getCentreY(), rad, rad, 0.0f, a0, a1, true);
            arc.addCentredArc (ring.getCentreX(), ring.getCentreY(), rad, rad, 0.0f, a0, a0 + (a1 - a0) * (float) value() / 100.0f, true);
            g.setColour (juce::Colours::black.withAlpha (0.45f));
            g.strokePath (track, juce::PathStrokeType (thick));
            g.setColour (Dark::text.withAlpha (alpha));
            g.strokePath (arc, juce::PathStrokeType (thick));

            auto t = r.reduced (2.0f, 3.0f);
            g.setColour (Dark::text.withAlpha (0.85f * alpha));
            auto captionArea = t.removeFromTop (t.getHeight() * 0.4f);
            g.setFont (fontFittingWidth (wingFontFit (TypeScale::caption, t.getHeight() * 0.5f), caption, captionArea.getWidth(), 0.7f));
            g.drawFittedText (caption, captionArea.toNearestInt(), juce::Justification::bottomLeft, 1, 0.7f);
            g.setColour (Dark::text.withAlpha (alpha));
            g.setFont (wingFontFit (TypeScale::value, t.getHeight() * 0.8f));
            g.drawFittedText (juce::String (value()) + " %", t.toNearestInt(), juce::Justification::centredLeft, 1);
        }

        void mouseDown (const juce::MouseEvent&) override { dragStart = value(); }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (isEnabled())
                set (dragStart - e.getDistanceFromDragStartY() / 2);
        }

        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
        {
            if (isEnabled())
                set (value() + (w.deltaY > 0.0f ? 5 : -5));
        }

        void mouseDoubleClick (const juce::MouseEvent&) override
        {
            if (isEnabled())
                set (fallback);
        }

    private:
        void set (int v)
        {
            ctx.settings.setValue (key, juce::jlimit (0, 100, v));
            // The surface (LCDs, colour bars, meters) and the display follow at once.
            if (auto* top = getTopLevelComponent())
                top->repaint();
        }

        UiContext& ctx;
        juce::String caption, key;
        int fallback, dragStart = 0;
        juce::Colour tint;
    };

    /** The TIME / DATE tiles: the clock, and a list for its format (12/24
        hours, YYYY-MM-DD or DD-MM-YYYY), which the status bar's clock uses. */
    class ClockTile final : public juce::Component
    {
    public:
        ClockTile (UiContext& c, bool isDate) : ctx (c), date (isDate) {}

        juce::String text() const
        {
            const auto now = juce::Time::getCurrentTime();
            return date ? uiprefs::formatDate (ctx.settings, now) : uiprefs::formatTime (ctx.settings, now);
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds();
            auto key = r.removeFromRight (juce::jmin (screenCap (22), r.getWidth() / 6));
            g.setColour (Dark::box);
            g.fillRect (r);
            g.setColour (Dark::key);
            g.fillRect (key.withTrimmedLeft (2));
            const auto kc = key.withTrimmedLeft (2).getCentre().toFloat();
            g.setColour (Dark::text);
            for (int i = -1; i <= 1; ++i)
                g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (kc.translated (0.0f, (float) i * 8.0f)));
            auto t = r.reduced (6, 3);
            g.setFont (wingFontFit (TypeScale::caption, (float) t.getHeight() * 0.3f));
            g.drawFittedText (date ? "DATE" : "TIME", t.removeFromTop (t.getHeight() * 4 / 10), juce::Justification::bottomLeft, 1);
            g.setFont (wingFontFit (TypeScale::value, (float) t.getHeight() * 0.85f));
            g.drawFittedText (text(), t, juce::Justification::centredLeft, 1, 0.8f);
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            juce::Component::SafePointer<ClockTile> safe (this);
            if (date)
                popups::chooseFrom (*this, { "YYYY-MM-DD", "DD-MM-YYYY" }, uiprefs::dayFirst (ctx.settings) ? 1 : 0, [safe] (int i)
                {
                    if (safe == nullptr)
                        return;
                    safe->ctx.settings.setValue (uiprefs::dateFormat, i == 1 ? "DD-MM-YYYY" : "YYYY-MM-DD");
                    safe->repaint();
                });
            else
                popups::chooseFrom (*this, { "24 HOURS", "12 HOURS" }, uiprefs::use24h (ctx.settings) ? 0 : 1, [safe] (int i)
                {
                    if (safe == nullptr)
                        return;
                    safe->ctx.settings.setValue (uiprefs::clock24, i == 0);
                    safe->repaint();
                });
        }

    private:
        UiContext& ctx;
        bool date;
    };

    //==============================================================================
    /** CONSOLE: name, software, engine, audio device, the console size and the data folders. */
    class ConsolePanel final : public juce::Component
    {
    public:
        explicit ConsolePanel (UiContext& c) : ctx (c)
        {
            editKey.onClick = [this]
            {
                editText (this, "CONSOLE NAME", ctx.settings.getValue (prefs::consoleName, "LIVE-DAW"), [this] (const juce::String& name)
                {
                    const auto newName = name.isNotEmpty() ? name.toUpperCase() : juce::String ("LIVE-DAW");
                    ctx.settings.setValue (prefs::consoleName, newName);
                    // The running WING remote and the MIDI identity answer with it right away
                    // (as when it is changed on SETUP > REMOTE).
                    if (ctx.engine.getWing().isRunning())
                        ctx.engine.getWing().setName (newName);
                    if (ctx.midi != nullptr)
                        ctx.midi->consoleName = newName;
                    repaint();
                });
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
            dataKey.onClick = [] { dataDirectory().createDirectory(); dataDirectory().revealToUser(); };
            showKey.onClick = [this]
            {
                const juce::File f (ctx.settings.getValue ("lastShowFile"));
                if (f.existsAsFile())
                    f.revealToUser();
            };
            // ABOUT: the start-up notice (the project, how it differs from a WING).
            aboutKey.onClick = [this] { startupNotice::show (ctx.settings, *this, false); };
            aboutKey.setTooltip ("About Sidecar: an independent hobby project; what differs from a real WING");
            styleIconKey (editKey, true);
            styleIconKey (aboutKey, true);
            for (auto* k : { &dataKey, &showKey })
                styleIconKey (*k);
            for (auto* k : { &chMinus, &chPlus, &chWing, &busMinus, &busPlus })
                k->offColour = Dark::key;
            for (auto* k : { &editKey, &aboutKey, &chMinus, &chPlus, &chWing, &busMinus, &busPlus, &dataKey, &showKey })
                addAndMakeVisible (k);
        }

        void update()
        {
            enableKey (busMinus, ctx.engine.getBuses().size() > (size_t) kWingBuses);
            enableKey (chMinus, ! ctx.engine.getChannels().empty());
            enableKey (showKey, juce::File (ctx.settings.getValue ("lastShowFile")).existsAsFile());
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            auto body = drawDarkPanel (g, getLocalBounds(), "CONSOLE");
            g.setColour (Dark::box);
            g.fillRect (infoBox);
            drawDarkPanel (g, dataBox, dataTitled ? "DATA" : "", darkHeaderHeight());
            (void) body;

            auto* device = ctx.deviceManager.getCurrentAudioDevice();
            const std::pair<juce::String, juce::String> info[] = {
                { "CONSOLE NAME (network)", ctx.settings.getValue (prefs::consoleName, "LIVE-DAW") },
                { "ENGINE", "RUST, " + juce::String (ctx.engine.getNumWorkerThreads()) + " DSP THREADS" },
                { "SOFTWARE", juce::String (kAppName).toUpperCase() + " " + juce::String (JUCE_APPLICATION_VERSION_STRING) },
                { "AUDIO DEVICE", device != nullptr ? device->getName().toUpperCase() : juce::String ("NONE") },
            };
            for (size_t i = 0; i < std::size (info); ++i)
            {
                auto row = infoRows[i];
                // [fig. 33] a faint small caption over a large value.
                g.setColour (Dark::textDim);
                g.setFont (wingFontPx (juce::jmin (TypeScale::statusCaption * getWingScreenHeight(), (float) row.getHeight() * 0.22f)));
                g.drawText (info[i].first, row.removeFromTop (row.getHeight() * 36 / 100), juce::Justification::bottomLeft);
                g.setColour (Dark::text);
                g.setFont (wingFontPx (juce::jmin ((i == 0 ? TypeScale::value : TypeScale::body) * getWingScreenHeight(), (float) row.getHeight() * 0.42f)));
                g.drawFittedText (info[i].second, row.withTrimmedRight (i == 0 ? editKey.getWidth() + 4 : (i == 2 ? aboutKey.getWidth() + 4 : 0)),
                                  juce::Justification::centredLeft, 1, 0.8f);
            }

            g.setColour (Dark::textDim);
            g.setFont (wingFontFit (TypeScale::statusCaption, (float) configCaption.getHeight() * 0.9f));
            g.drawText ("CONFIGURATION", configCaption, juce::Justification::bottomLeft);
            for (auto [row, label, value, valueArea] : { std::tuple { chRow, "CHANNELS", (int) ctx.engine.getChannels().size(), chValue },
                                                         std::tuple { busRow, "BUSES", (int) ctx.engine.getBuses().size(), busValue } })
            {
                g.setColour (Dark::text);
                g.setFont (wingFontFit (TypeScale::body, (float) row.getHeight() * 0.55f));
                g.drawText (label, row, juce::Justification::centredLeft);
                g.setFont (wingFontFit (TypeScale::value, (float) row.getHeight() * 0.7f, WingWeight::SemiBold));
                g.drawText (juce::String (value), valueArea, juce::Justification::centred);
            }
        }

        void resized() override
        {
            auto b = darkPanelBody (getLocalBounds(), true);
            // [fig. 33] the DATA box: its header over two 48 px keys (of the
            // WING's 800 px screen). In a small window the header goes, so the
            // keys keep a usable height.
            const int dataKeyH = juce::roundToInt (getWingScreenHeight() * 0.096f);
            dataTitled = darkHeaderHeight() + dataKeyH <= b.getHeight() / 3;
            dataBox = b.removeFromBottom (dataTitled ? darkHeaderHeight() + dataKeyH : juce::jlimit (16, dataKeyH, b.getHeight() / 4));
            b.removeFromBottom (4);
            infoBox = b;
            auto r = b.reduced (8, 4);
            const int configH = juce::jmin (screenCap (84), r.getHeight() * 32 / 100);
            auto config = r.removeFromBottom (configH);
            const int rowH = r.getHeight() / 4;
            for (auto& row : infoRows)
                row = r.removeFromTop (rowH);
            editKey.setBounds (infoRows[0].withTrimmedLeft (infoRows[0].getWidth() - juce::jmin (52, rowH + 12)).reduced (0, 2));
            aboutKey.setBounds (infoRows[2].withTrimmedLeft (infoRows[2].getWidth() - juce::jmin (52, rowH + 12)).reduced (0, 2));

            configCaption = config.removeFromTop (config.getHeight() / 4);
            const int h = config.getHeight() / 2;
            for (auto [row, minus, value, plus] : { std::tuple { &chRow, &chMinus, &chValue, &chPlus }, std::tuple { &busRow, &busMinus, &busValue, &busPlus } })
            {
                auto rr = config.removeFromTop (h).reduced (0, 2);
                *row = rr;
                auto keys = rr.removeFromRight (juce::jmin (rr.getWidth() * 62 / 100, screenCap (200)));
                auto wing = keys.removeFromRight (keys.getWidth() / 3);
                if (row == &chRow)
                    chWing.setBounds (wing.reduced (2, 0));
                minus->setBounds (keys.removeFromLeft (rr.getHeight() + 6).reduced (2, 0));
                plus->setBounds (keys.removeFromRight (rr.getHeight() + 6).reduced (2, 0));
                *value = keys;
            }

            auto d = darkPanelBody (dataBox, dataTitled, darkHeaderHeight()).reduced (6, 5);
            dataKey.setBounds (d.removeFromLeft (d.getWidth() / 2).reduced (3, 0));
            showKey.setBounds (d.reduced (3, 0));
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
        WingKey editKey { "EDIT", Icon::Edit }, aboutKey { "ABOUT", Icon::Info }, chMinus { "-" }, chPlus { "+" }, chWing { "WING" }, busMinus { "-" }, busPlus { "+" },
            dataKey { "DATA FOLDER", Icon::Folder }, showKey { "SHOW FOLDER", Icon::Folder };
        juce::Rectangle<int> infoBox, dataBox, infoRows[4], configCaption, chRow, busRow, chValue, busValue;
        bool dataTitled = true;
    };

    //==============================================================================
    /** GENERAL (fig. 33): lighting and clock tiles, then CONSOLE | OPERATION | OPTIONS. */
    class GeneralTab final : public juce::Component, private juce::Timer
    {
    public:
        explicit GeneralTab (UiContext& c) : ctx (c), console (c)
        {
            // Brightness (the main display, the surface LCDs, meters and colour bars) and the
            // rear-panel / external lamps, which this console doesn't have.
            // The tiles' tints [I9]: displays blue, LEDs red, scribble strips purple, PATCH brown, LAMP olive.
            const juce::Colour blue (0xff204878), red (0xff680000), purple (0xff780878), brown (0xff784800), olive (0xff686800);
            struct Spec { const char* caption; const char* id; int value; juce::Colour tint; bool enabled; };
            const Spec specs[] = { { "MAIN LCD", "mainLcd", 80, blue, true }, { "BTN LCD", "btnLcd", 80, blue, true },
                                   { "BTNS/LEDS", "btnsLeds", 90, red, true }, { "BACKLIGHT", "backlight", 25, red, true },
                                   { "METERS", "meters", 40, red, true }, { "COLOR LED", "colorLed", 25, purple, true },
                                   { "SCRIBBLE", "scribble", 60, purple, true }, { "SCR CONT", "scrCont", 50, purple, true },
                                   { "PATCH", "patch", 0, brown, false }, { "LAMP", "lamp", 0, olive, false } };
            for (auto& s : specs)
            {
                auto* t = lights.add (new LightTile (ctx, s.caption, s.id, s.value, s.tint));
                t->setEnabled (s.enabled);
                addAndMakeVisible (t);
            }
            addAndMakeVisible (time);
            addAndMakeVisible (date);
            addAndMakeVisible (console);

            operation.addRow ("CONFIRM LIBRARY LOAD", OnKey::forSetting (ctx.settings, prefs::confirmLibraryLoad, true), 1.3f);
            operation.addRow ("SHOW METER PAGE WHEN LOCKED", OnKey::fixed (false), 1.3f, false); // no screen lock
            // The status bar's title tile shows the active scene on every screen (MainDisplay reads it).
            operation.addRow ("ALWAYS SHOW ACTIVE SCENE", OnKey::forSetting (ctx.settings, uiprefs::alwaysShowScene, false), 1.3f);
            addAndMakeVisible (operation);

            options.addRow ("TOUCH FADER SELECT", OnKey::forSetting (ctx.settings, uiprefs::touchFaderSelect, false), 1.3f);
            options.addRow ("TOUCH FADER RES",
                            std::make_unique<ChoiceKeys> (juce::StringArray { "NORM", "FINE", "AUTO" },
                                                          [this] { return prefs::faderResolution (ctx); },
                                                          [this] (int i) { ctx.settings.setValue (prefs::touchFaderRes, i); }),
                            5.0f);
            // The CC section's arrows and wheel move a focus frame over the screen and edit it (firmware 1.13).
            options.addRow ("USE CRSR/WHEEL", OnKey::forSetting (ctx.settings, uiprefs::useCursorWheel, false), 1.3f);
            options.newBox();
            options.addRow ("MOUSE DISABLES TOUCH", OnKey::fixed (false), 1.3f, false);
            options.addRow ("MOUSE SPEED", std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox), 5.0f, false);
            options.newBox();
            // USB HOST SPEED: FS records 2 channels at 16 bit, HS 4 tracks at 24 bit (the USB recorder).
            options.addRow ("USB HOST SPEED",
                            std::make_unique<ChoiceKeys> (juce::StringArray { "FS", "HS" },
                                                          [this] { return ctx.engine.getUsbRecorder().isFullSpeedOnly() ? 0 : 1; },
                                                          [this] (int i)
                                                          {
                                                              ctx.engine.getUsbRecorder().setFullSpeedOnly (i == 0);
                                                              ctx.engine.getUsbRecorder().saveSettings (ctx.settings);
                                                          },
                                                          std::vector<juce::Colour> { Dark::orange, Dark::choice }),
                            3.2f, true, true);
            addAndMakeVisible (options);

            startTimerHz (1);
        }

        void timerCallback() override
        {
            time.repaint();
            date.repaint();
            console.update();
        }

        void refresh() { console.update(); }

        void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black); }

        void resized() override
        {
            auto b = getLocalBounds();
            auto top = b.removeFromTop (juce::jlimit (70, screenCap (120), b.getHeight() * 22 / 100));
            b.removeFromTop (6);
            const int gap = 4;
            const int timeW = top.getWidth() * 16 / 100;
            auto clock = top.removeFromRight (timeW);
            const int w = (top.getWidth() - gap) / 6;
            const int h = (top.getHeight() - gap) / 2;
            // Four columns of two brightness tiles, then the PATCH and LAMP columns.
            for (int i = 0; i < lights.size(); ++i)
            {
                const int col = i < 8 ? i / 2 : i - 4, row = i < 8 ? i % 2 : 0;
                lights[i]->setBounds (top.getX() + col * w, top.getY() + row * (h + gap), w - gap, h);
            }
            time.setBounds (clock.removeFromTop (h));
            clock.removeFromTop (gap);
            date.setBounds (clock.withHeight (h));

            const int colW = (b.getWidth() - 2 * gap) / 3;
            auto consoleArea = b.removeFromLeft (colW);
            b.removeFromLeft (gap);
            auto operationArea = b.removeFromLeft (colW);
            b.removeFromLeft (gap);
            auto optionsArea = b;
            console.setBounds (consoleArea);
            const int rowH = fitRowHeight (optionsArea.getHeight() - darkHeaderHeight() - 24, 6.0f);
            operation.rowHeight = options.rowHeight = rowH;
            operation.setBounds (operationArea.withHeight (juce::jmin (operationArea.getHeight(), operation.preferredHeight())));
            options.setBounds (optionsArea.withHeight (juce::jmin (optionsArea.getHeight(), options.preferredHeight())));
        }

    private:
        UiContext& ctx;
        juce::OwnedArray<LightTile> lights;
        ClockTile time { ctx, false }, date { ctx, true };
        ConsolePanel console;
        SettingsPanel operation { "OPERATION" }, options { "OPTIONS" };
    };

    //==============================================================================
    /** The INPUT SELECT channel grid (the first 40 channels + 8 aux): a cell
        lights blue while its channel listens to its MAIN input and orange while
        it listens to its ALT input [3.1]; a click switches the channel's own
        MAIN / ALT (used unless INPUT SELECT is GLOBAL). Cells of channels
        without an ALT input are dim. */
    class InputSelectGrid final : public juce::Component, private juce::Timer
    {
    public:
        explicit InputSelectGrid (UiContext& c) : ctx (c) { startTimerHz (4); }

        void paint (juce::Graphics& g) override
        {
            cells.clear();
            auto r = getLocalBounds().toFloat().reduced (4.0f);
            g.setColour (Dark::well);
            g.fillRect (getLocalBounds());
            const float captionH = 13.0f;
            const float cell = juce::jmin ((r.getWidth()) / 8.0f, (r.getHeight() - captionH * 2.0f - 6.0f) / 6.0f);
            auto drawGrid = [&] (const char* caption, StripKind kind, int rows)
            {
                g.setColour (Dark::caption);
                g.setFont (wingFontPx (juce::jmin (TypeScale::statusCaption * getWingScreenHeight(), captionH * 0.7f)));
                g.drawText (caption, r.removeFromTop (captionH), juce::Justification::centred);
                const auto& strips = ctx.engine.getStrips (kind);
                for (int i = 0; i < rows * 8; ++i)
                {
                    auto c = juce::Rectangle<float> (r.getCentreX() - cell * 4.0f + cell * (float) (i % 8), r.getY() + cell * (float) (i / 8), cell, cell).reduced (1.0f);
                    auto* s = i < (int) strips.size() ? strips[(size_t) i].get() : nullptr;
                    const bool has = s != nullptr && s->hasAltInput();
                    const bool alt = s != nullptr && s->isUsingAltInput();
                    // [D15] bright blue cells with dark numbers (orange on ALT); a
                    // channel without an ALT input a little darker.
                    g.setColour (alt ? altOrange : (has ? Dark::choice : Dark::choice.darker (0.35f)));
                    g.fillRect (c);
                    if (s != nullptr)
                    {
                        g.setColour (Dark::choiceInk);
                        g.setFont (wingFontPx (c.getHeight() * 0.46f));
                        g.drawText (juce::String (s->getNumber()), c, juce::Justification::centred);
                        cells.push_back ({ c, s });
                    }
                }
                r.removeFromTop (cell * (float) rows + 6.0f);
            };
            drawGrid ("CHANNEL", StripKind::Channel, 5);
            drawGrid ("AUX", StripKind::Aux, 1);
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            for (auto& [c, s] : cells)
                if (c.contains (e.position))
                {
                    s->setAltSelected (! s->isAltSelected());
                    repaint();
                    return;
                }
        }

    private:
        void timerCallback() override
        {
            // Repaint when a channel's ALT state changes.
            juce::String state;
            for (auto kind : { StripKind::Channel, StripKind::Aux })
                for (auto& s : ctx.engine.getStrips (kind))
                    state << (s->isUsingAltInput() ? 'A' : (s->hasAltInput() ? 'h' : '-'));
            if (state != shown)
            {
                shown = state;
                repaint();
            }
        }

        UiContext& ctx;
        std::vector<std::pair<juce::Rectangle<float>, Strip*>> cells;
        juce::String shown;
    };

    /** The audio device chooser, opened from the DEVICE field. */
    class DeviceBody final : public juce::Component
    {
    public:
        explicit DeviceBody (juce::AudioDeviceManager& dm) : selector (dm, 0, 256, 0, 256, false, false, false, false) { addAndMakeVisible (selector); }
        void resized() override { selector.setBounds (getLocalBounds().reduced (12, 0)); }

    private:
        juce::AudioDeviceSelectorComponent selector;
    };

    /** AUDIO (fig. 34): AUDIO CLOCK + INPUT SELECT | PREFERENCES | SOLO SYSTEM | AUDIO DEVICE + AUTOMIX + AUDIO ERRORS. */
    class AudioTab final : public juce::Component, private juce::Timer
    {
    public:
        AudioTab (UiContext& c, std::function<void (const juce::String&, std::unique_ptr<juce::Component>)> overlay)
            : ctx (c), showOverlay (std::move (overlay))
        {
            clock.addRow ({}, std::make_unique<DropdownField> ("CLOCK RATE", [this] { return rateText(); }, [this] (DropdownField& f) { showRates (f); }), 0.0f,
                          true, false, 1.7f);
            // The sync source is the audio interface's business.
            clock.addRow ({}, std::make_unique<DropdownField> ("SYNC SOURCE", [] { return juce::String ("INTERNAL"); }), 0.0f, false, false, 1.7f);
            addAndMakeVisible (clock);

            // The global MAIN / ALT switch (used by every channel while GLOBAL
            // INPUT SELECT OVERRIDE is on) and the channels' own switches.
            inputSelect.addRow ({}, std::make_unique<ChoiceKeys> (juce::StringArray { "MAIN", "ALT" }, [this] { return ctx.engine.isGlobalAlt() ? 1 : 0; },
                                                                 [this] (int i) { ctx.engine.setGlobalAlt (i == 1); }, std::vector<juce::Colour> { Dark::choice, altOrange }),
                                0.0f);
            inputSelect.addRow ({}, std::make_unique<InputSelectGrid> (ctx), 0.0f, true, false, 4.4f);
            addAndMakeVisible (inputSelect);

            prefsPanel.addRow ("MAIN LINK",
                               std::make_unique<ChoiceKeys> (models::list (models::MainLinkModes), [this] { return (int) ctx.engine.mainLink.load(); },
                                                             [this] (int i) { ctx.engine.mainLink = i; }),
                               6.4f);
            prefsPanel.addRow ("DCA MUTEGROUPS", OnKey::forAtomic (ctx.engine.dcaMuteGroups), 1.3f);
            prefsPanel.addRow ("MUTEGROUP/SIP OVERRIDE", OnKey::forAtomic (ctx.engine.muteOverride), 1.3f);
            prefsPanel.addRow ("STARTUP MAIN MUTE", OnKey::forAtomic (ctx.engine.startupMainMute), 1.3f);
            prefsPanel.addRow ("GLOBAL INPUT SELECT OVERRIDE", OnKey::forAtomic (ctx.engine.inputSelectGlobal), 1.3f);
            prefsPanel.newBox();
            // The channel a soloed source plays on (ROUTING > SOURCES' SRC SOLO).
            prefsPanel.addRow ("SOURCE SOLO",
                               std::make_unique<ChoiceKeys> (juce::StringArray { "OFF", "CH39", "AUX7" }, [this] { return (int) ctx.engine.monitor.sourceSolo.load(); },
                                                             [this] (int i)
                                                             {
                                                                 ctx.engine.clearSourceSolo();
                                                                 ctx.engine.monitor.sourceSolo = i;
                                                             }),
                               4.8f);
            // The channel hosting talkback (MONITORS > TALK can pick any input
            // or aux channel; then neither key is lit).
            prefsPanel.addRow ("TALKBACK",
                               std::make_unique<ChoiceKeys> (juce::StringArray { "OFF", "CH40", "AUX8" },
                                                             [this]
                                                             {
                                                                 const auto& t = ctx.engine.talk;
                                                                 const int kind = t.assignKind.load(), index = t.assignIndex.load();
                                                                 if (kind == ld::kTalkOff)
                                                                     return 0;
                                                                 if (kind == ld::kChannel && index == 39)
                                                                     return 1;
                                                                 return kind == ld::kAux && index == 7 ? 2 : -1;
                                                             },
                                                             [this] (int i)
                                                             {
                                                                 auto& t = ctx.engine.talk;
                                                                 t.assignIndex = i == 2 ? 7 : 39;
                                                                 t.assignKind = i == 0 ? ld::kTalkOff : (i == 2 ? ld::kAux : ld::kChannel);
                                                             }),
                               4.8f);
            prefsPanel.newBox();
            prefsPanel.addRow ("OUTPUT PROTECTION", OnKey::forAtomic (ctx.engine.outputProtection), 1.3f);
            auto ceilingSlider = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
            ceilingSlider->setRange (-24.0, 6.0, 0.1);
            ceilingSlider->setTextValueSuffix (" dB");
            ceilingSlider->setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
            ceilingSlider->setDoubleClickReturnValue (true, 0.0);
            ceiling = ceilingSlider.get();
            ceiling->onValueChange = [this] { ctx.engine.outputCeilingDb = (float) ceiling->getValue(); };
            prefsPanel.addRow ("CEILING", std::move (ceilingSlider), 7.0f);
            addAndMakeVisible (prefsPanel);

            // LIVE: each channel's SOLO BUS (phones / speakers); STUDIO: the solo
            // bus picked on MONITORS; SIP: solo in place (the SIP badge lights).
            solo.addRow ("SOLO MODE",
                         std::make_unique<ChoiceKeys> (juce::StringArray { "LIVE", "STUDIO", "SIP" }, [this] { return (int) ctx.engine.soloMode.load(); },
                                                       [this] (int i) { ctx.engine.soloMode = i; },
                                                       std::vector<juce::Colour> { Dark::olive, Dark::olive, Dark::olive }),
                         5.4f);
            solo.newBox();
            // PFL or AFL per strip type (CHANNEL SOLO covers the aux channels too).
            for (auto [name, kind] : { std::pair { "CHANNEL SOLO", StripKind::Channel }, std::pair { "BUS SOLO", StripKind::Bus },
                                       std::pair { "MAIN SOLO", StripKind::Main }, std::pair { "MATRIX SOLO", StripKind::Matrix } })
            {
                const auto k = kind;
                solo.addRow (name,
                             std::make_unique<ChoiceKeys> (juce::StringArray { "PFL", "AFL" }, [this, k] { return ctx.engine.isSoloAfl (k) ? 1 : 0; },
                                                           [this, k] (int i) { ctx.engine.setSoloAfl (k, i == 1); }, ChoiceKeys::prePost()),
                             3.6f);
            }
            solo.newBox();
            // Solo / select behaviour, applied by the main component to every
            // solo and selection (surface, screens, custom controls).
            const std::pair<const char*, const char*> soloPrefs[] = { { "EXCLUSIVE SOLO", uiprefs::exclusiveSolo },
                                                                      { "SOLO FOLLOWS SELECT", uiprefs::soloFollowsSelect },
                                                                      { "SELECT FOLLOWS SOLO", uiprefs::selectFollowsSolo },
                                                                      { "BUS SoF/SPILL ACTIVATES SOLO", uiprefs::sofActivatesSolo } };
            for (auto [name, key] : soloPrefs)
                solo.addRow (name, OnKey::forSetting (ctx.settings, key, false), 1.3f);
            addAndMakeVisible (solo);

            device.addRow ({}, std::make_unique<DropdownField> ("DEVICE", [this] { return deviceName(); },
                                                                [this] (DropdownField&) { showOverlay ("AUDIO DEVICE", std::make_unique<DeviceBody> (ctx.deviceManager)); }),
                           0.0f, true, false, 1.7f);
            device.addRow ({}, std::make_unique<DropdownField> ("IN/OUT", [this] { return channelsText(); }), 0.0f, false, false, 1.7f);
            addAndMakeVisible (device);

            automix.addRow ("X ENABLE", OnKey::forAtomic (ctx.engine.automixEnable[0]), 1.3f);
            automix.addRow ("Y ENABLE", OnKey::forAtomic (ctx.engine.automixEnable[1]), 1.3f);
            addAndMakeVisible (automix);

            clearKey.offColour = Dark::header;
            clearKey.iconAbove = true;
            clearKey.onClick = [this] { ctx.engine.outputClipEvents = 0; repaint(); };
            addAndMakeVisible (clearKey);
            startTimerHz (2);
            timerCallback();
        }

        void timerCallback() override
        {
            if (ceiling != nullptr && ! ceiling->isMouseButtonDown())
                ceiling->setValue (ctx.engine.outputCeilingDb.load(), juce::dontSendNotification);
            repaint (errorsArea);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colours::black);
            // The title stays centred, clear of the CLEAR key at the right.
            auto body = drawDarkPanel (g, errorsArea, " ");
            g.setColour (Dark::caption);
            g.setFont (panelTitleFont (darkHeaderHeight()));
            g.drawFittedText ("ERRORS", errorsArea.withHeight (darkHeaderHeight()).reduced (clearKey.getWidth() + 2, 0), juce::Justification::centred, 1, 0.8f);
            auto* d = ctx.deviceManager.getCurrentAudioDevice();
            const std::pair<const char*, juce::String> rows[] = { { "XRUNS", d != nullptr ? juce::String (d->getXRunCount()) : juce::String ("-") },
                                                                  { "SAFETY CLIPS", juce::String (ctx.engine.outputClipEvents.load()) } };
            auto r = body.reduced (8, 4);
            const int h = juce::jmin (screenCap (24), r.getHeight() / 2);
            for (auto& [label, value] : rows)
            {
                auto row = r.removeFromTop (h);
                g.setColour (Dark::text);
                g.setFont (wingFontFit (TypeScale::body, (float) h * 0.6f));
                g.drawText (label, row, juce::Justification::centredLeft);
                g.setColour (Dark::textDim);
                g.drawText (value, row, juce::Justification::centredRight);
            }
        }

        void resized() override
        {
            auto b = getLocalBounds();
            const int gap = 4, total = b.getWidth() - 3 * gap;
            auto c1 = b.removeFromLeft (total * 19 / 100);
            b.removeFromLeft (gap);
            auto c2 = b.removeFromLeft (total * 30 / 100);
            b.removeFromLeft (gap);
            auto c3 = b.removeFromLeft (total * 29 / 100);
            b.removeFromLeft (gap);
            auto c4 = b;

            const int rowH = fitRowHeight (getHeight() - darkHeaderHeight() - 30, 11.0f);
            for (auto* p : { &clock, &inputSelect, &prefsPanel, &solo, &device, &automix })
                p->rowHeight = rowH;

            clock.setBounds (c1.removeFromTop (clock.preferredHeight()));
            c1.removeFromTop (gap);
            inputSelect.setBounds (c1.withHeight (juce::jmin (c1.getHeight(), inputSelect.preferredHeight())));
            prefsPanel.setBounds (c2.withHeight (juce::jmin (c2.getHeight(), prefsPanel.preferredHeight())));
            solo.setBounds (c3.withHeight (juce::jmin (c3.getHeight(), solo.preferredHeight())));
            device.setBounds (c4.removeFromTop (device.preferredHeight()));
            c4.removeFromTop (gap);
            automix.setBounds (c4.removeFromTop (automix.preferredHeight()));
            c4.removeFromTop (gap);
            errorsArea = c4.withHeight (juce::jmin (c4.getHeight(), darkHeaderHeight() + 2 + rowH * 2 + 12));
            clearKey.setBounds (errorsArea.withHeight (darkHeaderHeight()).removeFromRight (darkHeaderHeight() + 16).reduced (2, 1));
        }

    private:
        juce::String rateText() const
        {
            if (auto* d = ctx.deviceManager.getCurrentAudioDevice())
            {
                const double sr = d->getCurrentSampleRate();
                return juce::String (sr / 1000.0, std::fmod (sr, 1000.0) == 0.0 ? 0 : 1) + "K";
            }
            return "--";
        }

        juce::String deviceName() const
        {
            auto* d = ctx.deviceManager.getCurrentAudioDevice();
            return d != nullptr ? d->getName().toUpperCase() : juce::String ("NONE");
        }

        juce::String channelsText() const
        {
            auto* d = ctx.deviceManager.getCurrentAudioDevice();
            if (d == nullptr)
                return "-";
            return juce::String (d->getActiveInputChannels().countNumberOfSetBits()) + "/" + juce::String (d->getActiveOutputChannels().countNumberOfSetBits())
                   + " CH";
        }

        void showRates (DropdownField& f)
        {
            auto* d = ctx.deviceManager.getCurrentAudioDevice();
            if (d == nullptr)
                return;
            const auto rates = d->getAvailableSampleRates();
            juce::StringArray items;
            int current = -1;
            for (int i = 0; i < rates.size(); ++i)
            {
                items.add (juce::String (rates[i] / 1000.0, std::fmod (rates[i], 1000.0) == 0.0 ? 0 : 1) + "K");
                if (std::abs (rates[i] - d->getCurrentSampleRate()) < 1.0)
                    current = i;
            }
            juce::Component::SafePointer<AudioTab> safe (this);
            popups::chooseFrom (f, items, current, [safe, rates] (int r)
            {
                if (safe == nullptr || ! juce::isPositiveAndBelow (r, rates.size()))
                    return;
                auto& c = safe->ctx;
                juce::AudioDeviceManager::AudioDeviceSetup setup;
                c.deviceManager.getAudioDeviceSetup (setup);
                setup.sampleRate = rates[r];
                const auto error = c.deviceManager.setAudioDeviceSetup (setup, true);
                if (error.isNotEmpty())
                    c.log.warning ("Clock rate: " + error);
                safe->repaint();
            });
        }

        UiContext& ctx;
        std::function<void (const juce::String&, std::unique_ptr<juce::Component>)> showOverlay;
        SettingsPanel clock { "AUDIO CLOCK" }, inputSelect { "INPUT SELECT" }, prefsPanel { "PREFERENCES" }, solo { "SOLO SYSTEM" },
            device { "AUDIO DEVICE" }, automix { "AUTOMIX" };
        juce::Slider* ceiling = nullptr;
        WingKey clearKey { "CLEAR", Icon::ClearCircle };
        juce::Rectangle<int> errorsArea;
    };

    //==============================================================================
    /** SURFACE (fig. 40): METERING | GENERAL | FADERS, plus the app's section widths. */
    class SurfaceTab final : public juce::Component
    {
    public:
        explicit SurfaceTab (UiContext& c) : ctx (c)
        {
            // The L/R meter beside the display: the monitor or a main (ui.mainMeter: 0 MONITOR, 1-4 MAIN n).
            metering.addRow ("MAIN METER",
                             std::make_unique<DropdownField> ("SOURCE",
                                                              [this]
                                                              {
                                                                  const int v = juce::jlimit (0, kNumMains, ctx.settings.getIntValue ("ui.mainMeter", 0));
                                                                  return v == 0 ? juce::String ("MONITOR") : "MAIN." + juce::String (v);
                                                              },
                                                              [this] (DropdownField& f)
                                                              {
                                                                  juce::StringArray items { "MONITOR" };
                                                                  for (int i = 1; i <= kNumMains; ++i)
                                                                      items.add ("MAIN." + juce::String (i));
                                                                  auto& settings = ctx.settings;
                                                                  popups::chooseFrom (f, items, ctx.settings.getIntValue ("ui.mainMeter", 0),
                                                                                      [&settings] (int i) { settings.setValue ("ui.mainMeter", i); });
                                                              }),
                             5.0f, true, false, 1.6f);
            metering.addRow ("MAIN METER TAP", fixedChoice ({ "AUTO", "PRE", "POST" }, 0, ChoiceKeys::autoPrePost()), 5.0f, false);
            // PRE or POST per strip type: the VIEW screens' meters follow these (the
            // surface's LED meters can read them with prefs::meterPoint).
            for (int i = 0; i < prefs::kNumMeterTaps; ++i)
                metering.addRow (prefs::meterTapNames[i],
                                 std::make_unique<ChoiceKeys> (juce::StringArray { "PRE", "POST" }, [this, i] { return prefs::meterTapPre (ctx, i) ? 0 : 1; },
                                                               [this, i] (int v) { ctx.settings.setValue (prefs::meterTapKey (i), v == 0); }, ChoiceKeys::prePost()),
                                 3.4f);
            addAndMakeVisible (metering);

            // SELECT on the selected strip again (ui.selDblClick: 0 HOME, 1 BUSFX, 2 OFF).
            general.addRow ("SEL DBL CLICK",
                            std::make_unique<ChoiceKeys> (juce::StringArray { "OFF", "HOME", "BUSFX" },
                                                          [this]
                                                          {
                                                              const int v = ctx.settings.getIntValue ("ui.selDblClick", 0);
                                                              return v == 2 ? 0 : v == 1 ? 2 : 1;
                                                          },
                                                          [this] (int i)
                                                          {
                                                              ctx.settings.setValue ("ui.selDblClick", i == 0 ? 2 : i == 2 ? 1 : 0);
                                                              notifyLayersChangedAsync (ctx);
                                                          }),
                            5.0f);
            // The rest change how the fader strips, the channel strip and the status bar behave.
            general.addRow ("TAP TEMPO FLASH", fixedChoice ({ "OFF", "8X", "ON" }, 0), 5.0f, false); // custom control tap keys
            general.addRow ("SOF BUTTON", choiceSetting (uiprefs::sofButton, { "AUTO", "ON", "FLASH" }, 0), 5.0f);
            general.addRow ("SHOW SOF FRAME", OnKey::forSetting (ctx.settings, uiprefs::showSofFrame, false), 1.3f);
            general.addRow ("SHOW SOURCE ON SCRIBBLE", onSetting (uiprefs::sourceOnScribble, true), 1.3f);
            general.addRow ("SHOW FADER VALUE ON SCRIBBLE", onSetting (uiprefs::faderValueOnScribble, false), 1.3f);
            general.addRow ("SCREEN FOLLOWS CH STRIP", OnKey::forSetting (ctx.settings, uiprefs::screenFollowsStrip, false), 1.3f);
            general.addRow ("CH STRIP TOUCH SELECT", OnKey::forSetting (ctx.settings, uiprefs::stripTouchSelect, false), 1.3f);
            general.addRow ("CH AUTOSELECT", std::make_unique<ToggleKeys> (ctx, juce::StringArray { "L", "C", "R" },
                                                                          std::vector<const char*> (std::begin (uiprefs::autoSelect), std::end (uiprefs::autoSelect))),
                            4.2f);
            general.addRow ("USE F1-F3 AS CUSTOM CONTROLS", onSetting (uiprefs::f1f3AsCustom, false), 1.3f);
            general.newBox();
            // App-specific: the dB value under every fader (ui.faderValues).
            general.addRow ("FADER VALUES",
                            std::make_unique<OnKey> ([this] { return ctx.settings.getBoolValue ("ui.faderValues", false); },
                                                     [this] (bool v)
                                                     {
                                                         ctx.settings.setValue ("ui.faderValues", v);
                                                         notifyLayersChangedAsync (ctx);
                                                     }),
                            1.3f);
            addAndMakeVisible (general);

            faders.addRow ("FADER SPEED", choiceSetting (uiprefs::faderSpeed, { "SLOW", "MED", "FAST" }, 1), 5.0f);
            faders.addRow ("FULL FADER PAGING", onSetting (uiprefs::fullFaderPaging, false), 1.3f);
            faders.addRow ("FADER/SCREEN LAYER LINK", OnKey::forSetting (ctx.settings, prefs::layerLink, true), 1.3f);
            faders.addRow ("ALTERNATIVE SOF MODE", onSetting (uiprefs::alternativeSof, false), 1.3f);
            faders.addRow ("RIGHT SECTION SENDS ON FADERS", onSetting (uiprefs::rightSectionSof, true), 1.3f);
            faders.addRow ("USER LAYER LINK", std::make_unique<ToggleKeys> (ctx, juce::StringArray { "L/C", "C/R" },
                                                                           std::vector<const char*> (std::begin (uiprefs::userLayerLink), std::end (uiprefs::userLayerLink))),
                           3.6f);
            faders.addRow ("DCA SPILL", onSetting (uiprefs::dcaSpill, false), 1.3f);
            faders.newBox();
            // App-specific: faders scrolled per press of the shift keys (ui.shiftStep).
            faders.addRow ("SHIFT STEP",
                           std::make_unique<ChoiceKeys> (juce::StringArray { "4", "8", "12" },
                                                         [this]
                                                         {
                                                             const int v = ctx.settings.getIntValue ("ui.shiftStep", 4);
                                                             return v == 12 ? 2 : v == 8 ? 1 : 0;
                                                         },
                                                         [this] (int i)
                                                         {
                                                             ctx.settings.setValue ("ui.shiftStep", (i + 1) * 4);
                                                             notifyLayersChangedAsync (ctx);
                                                         }),
                           4.2f);
            faders.addCaption ("SECTION WIDTH", false);
            static constexpr int widths[] = { LayerSet::kAutoWidth, 4, 8, 12, 16, 24, 32 };
            const char* names[] = { "LEFT", "CENTER", "RIGHT" };
            for (int s = 0; s < LayerSet::kNumSections; ++s)
            {
                const auto section = static_cast<LayerSet::Section> (s);
                faders.addRow (names[s],
                               std::make_unique<ChoiceKeys> (juce::StringArray { "AUTO", "4", "8", "12", "16", "24", "32" },
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
                                                             }),
                               8.4f);
            }
            addAndMakeVisible (faders);
        }

        void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black); }

        void resized() override
        {
            auto b = getLocalBounds();
            const int gap = 4, w = (b.getWidth() - 2 * gap) / 3;
            int rowH = fitRowHeight (getHeight() - darkHeaderHeight() - 24, 13.0f);
            // The tallest column (FADERS) sets the row height so none runs off the screen.
            auto tallest = [this] { return juce::jmax (metering.preferredHeight(), general.preferredHeight(), faders.preferredHeight()); };
            for (auto* p : { &metering, &general, &faders })
                p->rowHeight = rowH;
            while (rowH > 10 && tallest() > getHeight())
            {
                --rowH;
                for (auto* p : { &metering, &general, &faders })
                    p->rowHeight = rowH;
            }
            for (auto* p : { &metering, &general, &faders })
            {
                auto col = b.removeFromLeft (w);
                b.removeFromLeft (gap);
                p->setBounds (col.withHeight (juce::jmin (col.getHeight(), p->preferredHeight())));
            }
        }

    private:
        /** A choice row stored as an int setting. */
        std::unique_ptr<ChoiceKeys> choiceSetting (const char* key, juce::StringArray labels, int defaultValue)
        {
            auto& settings = ctx.settings;
            return std::make_unique<ChoiceKeys> (std::move (labels), [&settings, key, defaultValue] { return settings.getIntValue (key, defaultValue); },
                                                 [&settings, key] (int i) { settings.setValue (key, i); });
        }

        /** An ON key for a setting the fader strips read when they rebuild. */
        std::unique_ptr<OnKey> onSetting (const char* key, bool defaultValue)
        {
            return std::make_unique<OnKey> ([this, key, defaultValue] { return ctx.settings.getBoolValue (key, defaultValue); },
                                            [this, key] (bool v)
                                            {
                                                ctx.settings.setValue (key, v);
                                                notifyLayersChangedAsync (ctx);
                                            });
        }

        UiContext& ctx;
        SettingsPanel metering { "METERING" }, general { "GENERAL" }, faders { "FADERS" };
    };

    //==============================================================================
    /** Four number boxes (IP ADDRESS, SUBNET MASK, GATEWAY). */
    class OctetBoxes final : public juce::Component
    {
    public:
        explicit OctetBoxes (juce::StringArray v) : values (std::move (v)) {}

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            const float w = r.getWidth() / 4.0f;
            for (int i = 0; i < 4; ++i)
            {
                auto k = juce::Rectangle<float> (r.getX() + w * (float) i, r.getY(), w, r.getHeight()).reduced (2.0f, 0.0f);
                g.setColour (juce::Colour (0xff5a5a5a).withAlpha (isEnabled() ? 1.0f : 0.5f));
                g.fillRect (k);
                g.setColour (juce::Colours::black.withAlpha (isEnabled() ? 1.0f : 0.6f));
                g.setFont (wingFontFit (TypeScale::body, k.getHeight() * 0.62f));
                g.drawFittedText (values[i], k.toNearestInt(), juce::Justification::centred, 1);
            }
        }

    private:
        juce::StringArray values;
    };

    /** WING REMOTE: a server's ON key and its port (click to change it). The
        port turns red if the server is on but could not listen on it. */
    class ServerRow final : public juce::Component
    {
    public:
        ServerRow (UiContext& c, bool WingRemote::Settings::*enabledField, int WingRemote::Settings::*portField,
                   juce::String portTitle, std::function<void (const WingRemote::Settings&)> applyChange,
                   std::function<bool()> failedCheck)
            : ctx (c), enabled (enabledField), port (portField), title (std::move (portTitle)), apply (std::move (applyChange)),
              failed (std::move (failedCheck)),
              key ([this] { return WingRemote::Settings::load (ctx.settings).*enabled; },
                   [this] (bool on)
                   {
                       auto s = WingRemote::Settings::load (ctx.settings);
                       s.*enabled = on;
                       apply (s);
                   })
        {
            addAndMakeVisible (key);
        }

        void paint (juce::Graphics& g) override
        {
            auto r = portArea.toFloat();
            g.setColour (failed() ? Dark::muted : juce::Colour (0xff5a5a5a));
            g.fillRect (r);
            g.setColour (juce::Colours::black);
            g.setFont (wingFontFit (TypeScale::body, r.getHeight() * 0.62f));
            g.drawFittedText (juce::String (WingRemote::Settings::load (ctx.settings).*port), portArea, juce::Justification::centred, 1);
        }

        void resized() override
        {
            auto r = getLocalBounds();
            key.setBounds (r.removeFromLeft (juce::jmin (r.getHeight() * 3 / 2, r.getWidth() / 3)));
            r.removeFromLeft (4);
            portArea = r;
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (! portArea.contains (e.getPosition()))
                return;
            const auto current = WingRemote::Settings::load (ctx.settings).*port;
            editText (this, title, juce::String (current), [this] (const juce::String& text)
            {
                const int v = text.getIntValue();
                if (v <= 0 || v > 65535)
                    return;
                auto s = WingRemote::Settings::load (ctx.settings);
                s.*port = v;
                apply (s);
                repaint();
            });
        }

    private:
        UiContext& ctx;
        bool WingRemote::Settings::*enabled;
        int WingRemote::Settings::*port;
        juce::String title;
        std::function<void (const WingRemote::Settings&)> apply;
        std::function<bool()> failed;
        OnKey key;
        juce::Rectangle<int> portArea;
    };

    /** WING REMOTE status: errors and the connected clients. */
    class WingStatusView final : public juce::Component
    {
    public:
        WingRemote::Status status;

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Dark::well);
            auto r = getLocalBounds().reduced (8, 4);
            const float fh = juce::jlimit (11.0f, screenCap (14.0f), (float) getHeight() / 6.0f);
            const int lineH = juce::roundToInt (fh * 1.3f);
            g.setFont (juce::FontOptions (fh, juce::Font::bold));
            auto line = [&] (const juce::String& text, juce::Colour colour)
            {
                if (r.getHeight() < lineH)
                    return;
                g.setColour (colour);
                g.drawFittedText (text, r.removeFromTop (lineH), juce::Justification::centredLeft, 1, 0.8f);
            };

            if (! status.running)
            {
                line ("WING REMOTE IS OFF", Dark::textDim);
                return;
            }
            for (auto& e : status.errors)
                line (e.toUpperCase(), Dark::muted);
            line (juce::String ((int) status.clients.size()) + (status.clients.size() == 1 ? " CLIENT CONNECTED" : " CLIENTS CONNECTED"), Dark::text);
            for (auto& c : status.clients)
                line (juce::String (c.native ? "NATIVE  " : "OSC  ") + c.address
                          + (c.subscribed ? (c.native ? "  METERS" : "  SUBSCRIBED") : "") + "  " + juce::String (c.seconds) + " S",
                      Dark::textDim);
        }
    };

    //==============================================================================
    // MIDI (SETUP > REMOTE and DAW): the settings live in the app settings and
    // go to the MIDI service at once.

    MidiRemote::Settings midiSettings (UiContext& ctx)
    {
        return ctx.midi != nullptr ? ctx.midi->getSettings() : MidiRemote::Settings::load (ctx.settings);
    }

    void changeMidi (UiContext& ctx, const std::function<void (MidiRemote::Settings&)>& change)
    {
        auto s = midiSettings (ctx);
        change (s);
        midiui::applySettings (ctx, s);
    }

    /** A MIDI port's devices: its IN and OUT fields (NONE, the app's virtual
        port or a MIDI device of the computer). A device that is set but not
        there reads NOT FOUND until it comes back. */
    class MidiPortRow final : public juce::Component
    {
    public:
        MidiPortRow (UiContext& c, int portIndex, juce::String shortName) : ctx (c), port (portIndex), name (std::move (shortName))
        {
            for (int i = 0; i < 2; ++i)
            {
                const bool input = i == 0;
                fields[i] = std::make_unique<DropdownField> (name + (input ? " IN" : " OUT"), [this, input] { return valueText (input); },
                                                             [this, input] (DropdownField& f) { choose (f, input); });
                addAndMakeVisible (*fields[i]);
            }
        }

        void resized() override
        {
            auto r = getLocalBounds();
            fields[0]->setBounds (r.removeFromLeft (r.getWidth() / 2).withTrimmedRight (3));
            fields[1]->setBounds (r.withTrimmedLeft (3));
        }

    private:
        juce::String valueText (bool input) const
        {
            const auto s = midiSettings (ctx);
            const auto& device = input ? s.inputs[(size_t) port] : s.outputs[(size_t) port];
            if (device.isEmpty())
                return "NONE";
            const auto st = ctx.midi != nullptr ? ctx.midi->getPortStatus (port) : MidiRemote::PortStatus {};
            const bool missing = input ? st.inputMissing : st.outputMissing;
            const bool isVirtual = device == MidiRemote::virtualDevice;
            const auto shown = isVirtual ? MidiRemote::virtualPortName (port) : device;
            return (missing ? (isVirtual ? "UNAVAILABLE: " : "NOT FOUND: ") : "") + shown.toUpperCase();
        }

        void choose (DropdownField& f, bool input)
        {
            juce::StringArray items { "NONE" }, values { juce::String() };
            if (port != MidiRemote::Din && MidiRemote::canCreateVirtualPorts())
            {
                items.add ("VIRTUAL: " + MidiRemote::virtualPortName (port).toUpperCase());
                values.add (MidiRemote::virtualDevice);
            }
            for (auto& d : input ? MidiRemote::getInputDevices() : MidiRemote::getOutputDevices())
            {
                items.add (d.toUpperCase());
                values.add (d);
            }
            const auto s = midiSettings (ctx);
            const auto current = input ? s.inputs[(size_t) port] : s.outputs[(size_t) port];
            int selected = values.indexOf (current);
            if (selected < 0 && current.isNotEmpty())
            {
                items.add ("NOT FOUND: " + current.toUpperCase());
                values.add (current);
                selected = items.size() - 1;
            }
            auto* c = &ctx;
            const int p = port;
            popups::chooseFrom (f, items, selected, [c, p, input, values] (int i)
            {
                changeMidi (*c, [p, input, &values, i] (MidiRemote::Settings& st) { (input ? st.inputs : st.outputs)[(size_t) p] = values[i]; });
            });
        }

        UiContext& ctx;
        const int port;
        juce::String name;
        std::unique_ptr<DropdownField> fields[2];
    };

    /** The ⓘ of a MIDI REMOTE CONTROL row: the MIDI map of the function. */
    class MidiInfoBody final : public juce::Component
    {
    public:
        explicit MidiInfoBody (juce::String infoText) : text (std::move (infoText)) {}

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().reduced (24, 16);
            const auto lines = juce::StringArray::fromLines (text);
            const float fh = juce::jlimit (11.0f, screenCap (19.0f), (float) r.getHeight() / ((float) juce::jmax (8, lines.size()) * 1.6f));
            g.setFont (wingFontPx (fh));
            for (auto& line : lines)
            {
                g.setColour (line.endsWithChar (':') ? Dark::text : Dark::caption);
                g.drawFittedText (line, r.removeFromTop (juce::roundToInt (fh * 1.6f)), juce::Justification::centredLeft, 1, 0.7f);
            }
        }

    private:
        juce::String text;
    };

    /** One line per port: open or not, messages in and out. */
    juce::String portStatusText (UiContext& ctx, int port)
    {
        if (ctx.midi == nullptr)
            return {};
        const auto st = ctx.midi->getPortStatus (port);
        juce::String s = MidiRemote::portName (port) + ":  ";
        if (! st.inputOpen && ! st.outputOpen && ! st.inputMissing && ! st.outputMissing)
            return s + "NOT IN USE";
        s << "IN " << (st.inputOpen ? "OPEN" : st.inputMissing ? "NOT FOUND" : "-") << "   OUT "
          << (st.outputOpen ? "OPEN" : st.outputMissing ? "NOT FOUND" : "-") << "   " << st.received << " RECEIVED   " << st.sent << " SENT";
        return s;
    }

    /** REMOTE (fig. 42): MIDI REMOTE CONTROL | HA REMOTE + WING REMOTE | NETWORK + REMOTE LOCK.
        MIDI REMOTE CONTROL sets each function OFF, DIN or USB (ⓘ: its MIDI map); MIDI PORTS
        (Sidecar) picks the computer's devices for the WING's DIN and USB ports. HA remote is
        drawn disabled. WING REMOTE runs Sidecar's WING protocol servers (discovery, OSC,
        native) for WING remote apps; it starts with the app when enabled. */
    class RemoteTab final : public juce::Component, private juce::Timer
    {
    public:
        explicit RemoteTab (UiContext& c) : ctx (c)
        {
            const std::vector<juce::Colour> offLit { juce::Colour (0xffc6c6c6), Dark::choice, Dark::choice };
            for (int f = 0; f < MidiRemote::kNumFunctions; ++f)
                midi.addRow (MidiRemote::functionName (f),
                             std::make_unique<ChoiceKeys> (juce::StringArray { "OFF", "DIN", "USB" }, [this, f] { return midiSettings (ctx).routes[(size_t) f]; },
                                                           [this, f] (int route) { changeMidi (ctx, [f, route] (MidiRemote::Settings& st) { st.routes[(size_t) f] = route; }); },
                                                           offLit),
                             4.8f, true, true);
            midi.addMouseListener (this, false); // a row's ⓘ shows its MIDI map
            addAndMakeVisible (midi);
            ports.addRow ({}, std::make_unique<MidiPortRow> (ctx, MidiRemote::Din, "DIN"), 0.0f, true, false, 1.5f);
            ports.addRow ({}, std::make_unique<MidiPortRow> (ctx, MidiRemote::Usb, "USB"), 0.0f, true, false, 1.5f);
            addAndMakeVisible (ports);

            // AES50 preamp remote control: hardware only.
            ha.addCaption ("ENABLE HA REMOTE     ENABLE CUST SYNC");
            for (auto* port : { "AES50 A", "AES50 B", "AES50 C" })
            {
                auto pair = std::make_unique<juce::Component>();
                auto* a = OnKey::fixed (false).release();
                auto* b = OnKey::fixed (false).release();
                pair->addAndMakeVisible (a);
                pair->addAndMakeVisible (b);
                pairs.add (a);
                pairs.add (b);
                pairLayouts.push_back ({ pair.get(), a, b });
                ha.addRow (port, std::move (pair), 4.0f, false);
            }
            addAndMakeVisible (ha);

            // The computer's own network settings (read-only).
            const auto ip = juce::IPAddress::getLocalAddress().toString();
            juce::StringArray octets;
            octets.addTokens (ip, ".", {});
            while (octets.size() < 4)
                octets.add ("0");
            network.addRow ({}, std::make_unique<DropdownField> ("IP MODE", [] { return juce::String ("SYSTEM"); }), 0.0f, false, false, 1.6f);
            network.addCaption ("IP ADDRESS", false);
            network.addRow ({}, std::make_unique<OctetBoxes> (octets), 0.0f, false);
            network.addCaption ("SUBNET MASK", false);
            network.addRow ({}, std::make_unique<OctetBoxes> (juce::StringArray { "-", "-", "-", "-" }), 0.0f, false);
            network.addCaption ("GATEWAY", false);
            network.addRow ({}, std::make_unique<OctetBoxes> (juce::StringArray { "-", "-", "-", "-" }), 0.0f, false);
            addAndMakeVisible (network);

            lock.addRow ("OSC", OnKey::fixed (false), 1.3f, false, true);
            lock.addRow ("TCP", OnKey::fixed (false), 1.3f, false, true);
            addAndMakeVisible (lock);

            // WING REMOTE (Sidecar): the settings live in the app settings.
            auto change = [this] (const WingRemote::Settings& s)
            {
                s.save (ctx.settings);
                if (ctx.midi != nullptr)
                    ctx.midi->consoleName = s.name;
                applySettings();
            };
            wing.addRow ("ENABLE WING REMOTE",
                         std::make_unique<OnKey> ([this] { return WingRemote::Settings::load (ctx.settings).enabled; },
                                                  [this, change] (bool on)
                                                  {
                                                      auto s = WingRemote::Settings::load (ctx.settings);
                                                      s.enabled = on;
                                                      change (s);
                                                  }),
                         1.6f, true, true);
            wing.addRow ({}, std::make_unique<DropdownField> ("CONSOLE NAME", [this] { return WingRemote::Settings::load (ctx.settings).name; },
                                                               [this, change] (DropdownField& f)
                                                               {
                                                                   editText (&f, "CONSOLE NAME", WingRemote::Settings::load (ctx.settings).name,
                                                                             [this, change] (const juce::String& name)
                                                                             {
                                                                                 auto s = WingRemote::Settings::load (ctx.settings);
                                                                                 s.name = name.isNotEmpty() ? name.toUpperCase() : juce::String ("LIVE-DAW");
                                                                                 change (s);
                                                                             });
                                                               }),
                         0.0f, true, false, 1.3f);
            wing.newBox();
            using S = WingRemote::Settings;
            // A server that is on but not listening (port in use) shows its port in red.
            auto failed = [this] (bool S::*on, int WingRemote::Status::*bound)
            {
                return [this, on, bound]
                {
                    return wingStatus.status.running && WingRemote::Settings::load (ctx.settings).*on && wingStatus.status.*bound < 0;
                };
            };
            wing.addRow ("DISCOVERY (UDP)",
                         std::make_unique<ServerRow> (ctx, &S::discovery, &S::discoveryPort, "DISCOVERY PORT (UDP)", change,
                                                      failed (&S::discovery, &WingRemote::Status::discoveryPort)),
                         4.0f);
            wing.addRow ("OSC (UDP)",
                         std::make_unique<ServerRow> (ctx, &S::osc, &S::oscPort, "OSC PORT (UDP)", change, failed (&S::osc, &WingRemote::Status::oscPort)),
                         4.0f);
            wing.addRow ("NATIVE (TCP)",
                         std::make_unique<ServerRow> (ctx, &S::native, &S::nativePort, "NATIVE PORT (TCP)", change,
                                                      failed (&S::native, &WingRemote::Status::nativePort)),
                         4.0f);
            addAndMakeVisible (wing);
            addAndMakeVisible (wingStatus);

            // The SETUP page is built at start-up: start the servers if they were on.
            applySettings();
            startTimerHz (2);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colours::black);
            // The app's note on its MIDI ports, small and grey (it must fit the
            // space under MIDI PORTS on every window size).
            g.setColour (Dark::textDim);
            auto n = note;
            const float cap = TypeScale::statusCaption * getWingScreenHeight();
            g.setFont (wingFontPx (cap * 1.3f));
            for (int p : { (int) MidiRemote::Din, (int) MidiRemote::Usb })
                g.drawFittedText (portStatusText (ctx, p), n.removeFromTop (juce::roundToInt (cap * 3.0f)), juce::Justification::centredLeft, 1, 0.8f);
            n.removeFromTop (juce::roundToInt (cap));
            g.setFont (wingFontPx (cap));
            g.drawFittedText ("DIN AND USB ARE MIDI PORTS OF THIS COMPUTER (MIDI PORTS); USB IS THE VIRTUAL PORT \"SIDECAR MIDI CONTROL\" WHERE "
                              "THE SYSTEM ALLOWS IT. TOUCH A FUNCTION'S NAME FOR ITS MIDI MAP.\n\n"
                              "WING REMOTE: WING REMOTE APPS (OSC AND THE NATIVE PROTOCOL) CAN CONNECT TO SIDECAR. "
                              "EXPERIMENTAL: TESTED WITH LIBWING AND THE COMPANION MODULE'S PATHS, NOT WITH WING EDIT OR WING COPILOT.",
                              n, juce::Justification::topLeft, 8);
        }

        void resized() override
        {
            auto b = getLocalBounds();
            const int rowH = fitRowHeight (getHeight() - darkHeaderHeight() - 30, 10.0f);
            for (auto* p : { &midi, &ha, &network, &lock })
                p->rowHeight = rowH;
            // MIDI REMOTE CONTROL and MIDI PORTS share the left column with the port status.
            midi.rowHeight = ports.rowHeight = juce::jmin (rowH, fitRowHeight (getHeight() - 2 * darkHeaderHeight() - 110, 11.0f));
            wing.rowHeight = juce::jmax (minRowHeight(), rowH * 4 / 5); // leaves room for the client list
            const int gap = 4;
            auto c1 = b.removeFromLeft (b.getWidth() * 40 / 100);
            b.removeFromLeft (gap);
            auto c2 = b.removeFromLeft (b.getWidth() * 43 / 100);
            auto c4 = b.removeFromRight (b.getWidth() * 60 / 100);
            midi.setBounds (c1.removeFromTop (juce::jmin (c1.getHeight(), midi.preferredHeight())));
            c1.removeFromTop (gap);
            ports.setBounds (c1.removeFromTop (juce::jmin (c1.getHeight(), ports.preferredHeight())));
            note = c1.reduced (8, 10);
            info.layout (getLocalBounds());
            ha.setBounds (c2.removeFromTop (juce::jmin (c2.getHeight(), ha.preferredHeight())));
            c2.removeFromTop (gap);
            // WING REMOTE also takes the free column left of NETWORK.
            auto wingArea = c2.withRight (b.getRight() - gap);
            wing.setBounds (wingArea.removeFromTop (juce::jmin (wingArea.getHeight(), wing.preferredHeight())));
            wingArea.removeFromTop (gap);
            wingStatus.setBounds (wingArea);
            network.setBounds (c4.removeFromTop (juce::jmin (c4.getHeight() - lock.preferredHeight() - gap, network.preferredHeight())));
            c4.removeFromTop (gap);
            lock.setBounds (c4.withHeight (juce::jmin (c4.getHeight(), lock.preferredHeight())));
            for (auto& p : pairLayouts)
            {
                auto r = p.parent->getLocalBounds();
                const int w = juce::jmin (r.getHeight() * 3 / 2, r.getWidth() / 2 - 4);
                p.a->setBounds (r.removeFromLeft (r.getWidth() / 2).withSizeKeepingCentre (w, r.getHeight()));
                p.b->setBounds (r.withSizeKeepingCentre (w, r.getHeight()));
            }
        }

    private:
        struct Pair
        {
            juce::Component* parent;
            juce::Component* a;
            juce::Component* b;
        };

        /** Starts, restarts or stops the WING remote to match the settings (also
            when the console name changes on SETUP > GENERAL). */
        void applySettings()
        {
            const auto s = WingRemote::Settings::load (ctx.settings);
            applied = s;
            auto errors = ctx.engine.getWing().apply (s);
            for (auto& e : errors)
                ctx.log.error ("WING remote: " + e);
            if (s.enabled && errors.isEmpty())
                ctx.log.info ("WING remote on as \"" + s.name + "\"");
            wingStatus.status = ctx.engine.getWing().getStatus();
            wingStatus.repaint();
            wing.repaint();
        }

        /** A touch on a MIDI REMOTE CONTROL row's name (ⓘ): its MIDI map. */
        void mouseUp (const juce::MouseEvent& e) override
        {
            if (e.eventComponent != &midi || midi.getNumChildComponents() == 0)
                return;
            const auto pos = e.getEventRelativeTo (&midi).getPosition();
            const int top = darkPanelBody (midi.getLocalBounds(), true).getY() + 4;
            const int row = (pos.y - top) / juce::jmax (1, midi.rowHeight);
            if (pos.y < top || ! juce::isPositiveAndBelow (row, (int) MidiRemote::kNumFunctions) || pos.x >= midi.getChildComponent (0)->getX())
                return;
            info.show (*this, MidiRemote::functionName (row), std::make_unique<MidiInfoBody> (MidiRemote::functionInfo (row)));
        }

        void timerCallback() override
        {
            if (WingRemote::Settings::load (ctx.settings) != applied)
                applySettings();
            if (isShowing())
            {
                repaint (note); // the MIDI ports' status
                wingStatus.status = ctx.engine.getWing().getStatus();
                wingStatus.repaint();
                wing.repaint(); // the ports' listening state
            }
        }

        UiContext& ctx;
        SettingsPanel midi { "MIDI REMOTE CONTROL" }, ha { "HA REMOTE" }, network { "NETWORK" }, lock { "REMOTE LOCK" }, wing { "WING REMOTE" };
        SettingsPanel ports { "MIDI PORTS" };
        OverlayHolder info;
        WingStatusView wingStatus;
        WingRemote::Settings applied;
        juce::OwnedArray<juce::Component> pairs;
        std::vector<Pair> pairLayouts;
        juce::Rectangle<int> note;
    };

    /** The surface drawing of DAW CONTROL (fig. 43): the parts of the surface
        each emulated controller drives. The master unit ("1") takes the left
        section's faders 1-8 and, with USE UPPER CC FOR DAW, the upper custom
        controls; extender "2" the next eight faders (left 9-12, center 1-4),
        extender "3" the last eight (center 5-8, right 1-4). */
    class DawSurface final : public juce::Component
    {
    public:
        explicit DawSurface (UiContext& c) : ctx (c) {}

        void paint (juce::Graphics& g) override
        {
            const auto s = midiSettings (ctx).daw;
            const int units = s.usb ? (s.configuration == DawRemote::McuTwoExtenders ? 3 : s.configuration == DawRemote::McuExtender ? 2 : 1) : 1;
            const bool faders = s.configuration != DawRemote::CustomControlsOnly;
            const float alpha = s.enabled ? 1.0f : 0.45f;

            auto r = getLocalBounds().toFloat().reduced (2.0f);
            g.setColour (Colours::screenTile);
            g.fillRoundedRectangle (r, 4.0f);
            r = r.reduced (6.0f);
            g.setColour (Colours::screenDark);
            g.drawRect (r, 1.0f);

            // The top: channel strip panel, the display, the monitoring panel.
            auto top = r.removeFromTop (r.getHeight() * 0.40f).reduced (4.0f, 4.0f);
            const float u = r.getWidth() / 27.4f; // one fader strip
            g.setColour (Colours::screenDark);
            g.drawRect (top.withWidth (u * 4.5f), 1.0f);
            g.fillRect (top.withX (top.getX() + u * 5.0f).withWidth (u * 8.0f).reduced (u * 0.6f, 2.0f));
            g.drawRect (top.withX (top.getX() + u * 5.0f).withWidth (u * 8.0f), 1.0f);
            g.drawRect (top.withX (top.getX() + u * 13.4f).withWidth (u * 8.6f), 1.0f);
            g.drawRect (top.withX (top.getRight() - u * 4.6f).withWidth (u * 4.6f), 1.0f);

            // The fader sections and the custom controls column.
            auto bottom = r.reduced (0.0f, 4.0f);
            auto fader = [&] (int index)
            {
                // index 0-23 across left (12), center (8) and right (4).
                float x = bottom.getX() + u * (float) index;
                if (index >= 12)
                    x += u * 0.3f;
                if (index >= 20)
                    x += u * 0.3f + u * 2.6f + u * 0.3f;
                return juce::Rectangle<float> (x, bottom.getY(), u, bottom.getHeight());
            };
            auto ccColumn = juce::Rectangle<float> (bottom.getX() + u * 20.6f, bottom.getY(), u * 2.6f, bottom.getHeight());
            auto upperCc = ccColumn.withHeight (ccColumn.getHeight() * 0.42f);
            for (int i = 0; i < 24; ++i)
            {
                auto f = fader (i).reduced (u * 0.12f, 2.0f);
                g.setColour (Colours::screenDark);
                g.drawRect (f, 1.0f);
                g.fillRect (f.getCentreX() - 1.0f, f.getY() + f.getHeight() * 0.45f, 2.0f, f.getHeight() * 0.5f);
            }
            g.setColour (Colours::screenDark);
            g.drawRect (ccColumn.reduced (u * 0.1f, 2.0f), 1.0f);

            // The emulated units: yellow blocks with their number.
            auto block = [&] (juce::Rectangle<float> area, const juce::String& label)
            {
                g.setColour (Dark::on.withAlpha (0.45f * alpha));
                g.fillRect (area);
                g.setColour (Dark::on.withAlpha (alpha));
                g.drawRect (area, 1.5f);
                g.setColour (juce::Colours::black.withAlpha (alpha));
                g.setFont (wingFontPx (juce::jmin (screenCap (30.0f), area.getHeight() * 0.42f), WingWeight::SemiBold));
                g.drawText (label, area, juce::Justification::centred);
            };
            if (faders)
                for (int unit = 0; unit < units; ++unit)
                {
                    // A unit's eight faders: one block over the left and center
                    // sections, a second one past the custom controls.
                    const int first = unit * 8, last = first + 7;
                    if (first <= 19)
                        block (fader (first).getUnion (fader (juce::jmin (last, 19))).reduced (1.0f, 0.0f), juce::String (unit + 1));
                    if (last >= 20)
                        block (fader (juce::jmax (first, 20)).getUnion (fader (last)).reduced (1.0f, 0.0f), juce::String (unit + 1));
                }
            // The custom controls belong to the master unit: the lower part
            // always (the DAW layers), the upper part with USE UPPER CC FOR DAW.
            block ((s.upperCc ? ccColumn : ccColumn.withTrimmedTop (upperCc.getHeight())).reduced (1.0f, 0.0f), "1");
        }

    private:
        UiContext& ctx;
    };

    /** The PRESET field with its LOAD key: loads a DAW's preset into the four
        DAW layers of the lower custom controls. */
    class PresetRow final : public juce::Component
    {
    public:
        explicit PresetRow (UiContext& c) : ctx (c), selected (midiSettings (c).daw.preset)
        {
            styleIconKey (load);
            load.onClick = [this] { confirmLoad(); };
            addAndMakeVisible (field);
            addAndMakeVisible (load);
        }

        void resized() override
        {
            auto b = getLocalBounds();
            load.setBounds (b.removeFromRight (juce::jmin (screenCap (100), b.getWidth() * 2 / 5)).reduced (0, 2));
            b.removeFromRight (6);
            field.setBounds (b);
        }

    private:
        void confirmLoad()
        {
            if (selected <= 0)
                return;
            const auto name = DawRemote::presetNames()[selected];
            popups::confirm (*this, "LOAD DAW PRESET", "Load the " + name + " preset into the DAW layers DAW 1-4 of the custom controls?", "LOAD",
                             [this] { loadPreset (selected); });
        }

        void loadPreset (int preset)
        {
            if (ctx.customControls == nullptr)
                return;
            const auto groups = DawRemote::commandGroups();
            const auto& table = DawRemote::commands();
            for (int layer = 0; layer < cc::kNumDawLayers; ++layer)
                ctx.customControls->clearLayer (cc::kFirstDawLayer + layer);
            for (auto& k : DawRemote::presetKeys (preset))
            {
                const int layer = cc::kFirstDawLayer + k.layer, control = k.key % cc::kNumControls;
                auto c = ctx.customControls->get (layer, control);
                const cc::Function f { cc::BtnDawButtons, groups.indexOf (table[(size_t) k.command].group), k.command };
                (k.key < cc::kNumControls ? c.upper : c.lower) = f;
                c.name = DawRemote::presetColumnName (preset, k.layer, control);
                ctx.customControls->set (layer, control, c);
            }
            changeMidi (ctx, [preset] (MidiRemote::Settings& s) { s.daw.preset = preset; });
            ctx.log.info ("DAW preset " + DawRemote::presetNames()[preset] + " loaded into DAW 1-4");
        }

        UiContext& ctx;
        int selected = 0;
        DropdownField field { "PRESET", [this] { return DawRemote::presetNames()[juce::jlimit (0, DawRemote::presetNames().size() - 1, selected)]; },
                              [this] (DropdownField& f)
                              {
                                  popups::chooseFrom (f, DawRemote::presetNames(), selected, [this] (int i)
                                  {
                                      selected = i;
                                      field.repaint();
                                  });
                              } };
        WingKey load { "LOAD", Icon::LoadTray };
    };

    /** DAW (fig. 43): DAW CONTROL, then (Sidecar) the DAW's MIDI ports and
        the state of the emulated units. */
    class DawTab final : public juce::Component, private juce::Timer
    {
    public:
        explicit DawTab (UiContext& c) : ctx (c), surface (c)
        {
            auto get = [this] { return midiSettings (ctx).daw; };
            auto set = [this] (std::function<void (DawRemote::Settings&)> f)
            {
                changeMidi (ctx, [&f] (MidiRemote::Settings& s) { f (s.daw); });
                surface.repaint();
            };
            daw.addRow ("DAW ENABLE", std::make_unique<OnKey> ([get] { return get().enabled; }, [set] (bool v) { set ([v] (DawRemote::Settings& s) { s.enabled = v; }); }), 1.3f);
            daw.newBox(); // fig. 43: DAW ENABLE in its own box
            daw.addRow ("CONNECTION", std::make_unique<ChoiceKeys> (juce::StringArray { "DIN", "USB" }, [get] { return get().usb ? 1 : 0; },
                                                                    [set] (int i) { set ([i] (DawRemote::Settings& s) { s.usb = i == 1; }); },
                                                                    std::vector<juce::Colour> { Dark::post, Dark::post }),
                        3.2f);
            daw.addRow ("EMULATION", std::make_unique<ChoiceKeys> (juce::StringArray { "MCU", "HUI" }, [get] { return get().emulation == DawRemote::Emulation::Hui ? 1 : 0; },
                                                                   [set] (int i) { set ([i] (DawRemote::Settings& s) { s.emulation = i == 1 ? DawRemote::Emulation::Hui : DawRemote::Emulation::Mcu; }); }),
                        3.2f);
            daw.addRow ("USE UPPER CC FOR DAW", std::make_unique<OnKey> ([get] { return get().upperCc; }, [set] (bool v) { set ([v] (DawRemote::Settings& s) { s.upperCc = v; }); }), 1.3f);
            daw.addRow ("DISABLE WHEEL DURING PLAY",
                        std::make_unique<OnKey> ([get] { return get().disableWheel; }, [set] (bool v) { set ([v] (DawRemote::Settings& s) { s.disableWheel = v; }); }), 1.3f);
            daw.newBox();
            daw.addRow ({}, std::make_unique<DropdownField> ("CONFIGURATION", [get] { return DawRemote::configurationName (get().configuration); },
                                                             [get, set] (DropdownField& f)
                                                             {
                                                                 juce::StringArray items;
                                                                 for (int i = 0; i < DawRemote::kNumConfigurations; ++i)
                                                                     items.add (DawRemote::configurationName (i));
                                                                 popups::chooseFrom (f, items, get().configuration, [set] (int i)
                                                                 {
                                                                     set ([i] (DawRemote::Settings& s) { s.configuration = i; });
                                                                 });
                                                             }),
                        0.0f, true, false, 1.6f);
            daw.addRow ({}, std::make_unique<PresetRow> (ctx), 0.0f, true, false, 1.6f);
            addAndMakeVisible (daw);
            addAndMakeVisible (surface);

            for (int unit = 0; unit < DawRemote::kMaxUnits; ++unit)
                ports.addRow ({}, std::make_unique<MidiPortRow> (ctx, MidiRemote::Daw1 + unit, "MIDI DAW " + juce::String (unit + 1)), 0.0f, true, false, 1.5f);
            addAndMakeVisible (ports);
            startTimerHz (2);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colours::black);
            drawDarkPanel (g, panel, "DAW CONTROL");
            g.setColour (Dark::textDim);
            g.setFont (wingFont (TypeScale::meterScale * 1.3f));
            g.drawFittedText ("All third-party DAW trademarks are the property of their respective owners. Their use neither constitutes a claim of the "
                              "trademark nor affiliation of the trademark owners with the makers of this software. Product names are mentioned solely as "
                              "a reference for compatibility.",
                              disclaimer, juce::Justification::bottomLeft, 3);

            // The emulated units and DAW mode.
            auto r = status;
            const float fh = juce::jlimit (11.0f, screenCap (15.0f), (float) getHeight() / 34.0f);
            const int lineH = juce::roundToInt (fh * 1.45f);
            auto line = [&] (const juce::String& text, juce::Colour colour)
            {
                if (r.getHeight() < lineH)
                    return;
                g.setColour (colour);
                g.setFont (juce::FontOptions (fh, juce::Font::bold));
                g.drawFittedText (text, r.removeFromTop (lineH), juce::Justification::centredLeft, 1, 0.8f);
            };
            const auto s = midiSettings (ctx);
            auto* d = midiui::daw (ctx);
            if (! s.daw.enabled || d == nullptr)
                line ("DAW CONTROL IS OFF", Dark::textDim);
            else
            {
                line (juce::String ("DAW MODE ") + (d->isOn() ? "ON" : "OFF: PRESS DAW IN THE CUSTOM CONTROLS SECTION"), d->isOn() ? Dark::on : Dark::text);
                const juce::String kind = d->isHui() ? "HUI" : "MCU";
                for (int unit = 0; unit < d->getNumUnits(); ++unit)
                {
                    const auto name = unit == 0 ? kind : kind + " EXTENDER " + juce::String (unit);
                    const int port = s.daw.usb ? MidiRemote::Daw1 + unit : MidiRemote::Din;
                    line (name + " (" + MidiRemote::portName (port) + "):  " + (d->isConnected (unit) ? "ONLINE" : "WAITING FOR THE DAW"),
                          d->isConnected (unit) ? Dark::post : Dark::textDim);
                }
                if (! s.daw.usb)
                    line ("DIN: CHOOSE ITS DEVICES ON SETUP > REMOTE (MIDI PORTS)", Dark::textDim);
            }
            r.removeFromTop (lineH / 2);
            for (int p = MidiRemote::Daw1; p <= MidiRemote::Daw3; ++p)
                line (portStatusText (ctx, p), Dark::textDim);
        }

        void resized() override
        {
            auto b = getLocalBounds();
            disclaimer = b.removeFromBottom (44).reduced (6, 2);
            auto area = b.withWidth (b.getWidth() * 60 / 100);
            daw.rowHeight = fitRowHeight (area.getHeight() - darkHeaderHeight() - 20, 9.0f);
            panel = area.withHeight (juce::jmin (area.getHeight(), daw.preferredHeight() + darkHeaderHeight() + 2));
            auto body = darkPanelBody (panel, true);
            auto right = body.removeFromRight (body.getWidth() / 2);
            daw.setBounds (body);
            surface.setBounds (right.withHeight (juce::jmin (right.getHeight(), right.getWidth() * 50 / 100)).reduced (6));
            auto side = b.withTrimmedLeft (panel.getRight() + 6 - b.getX());
            ports.rowHeight = daw.rowHeight;
            ports.setBounds (side.removeFromTop (juce::jmin (side.getHeight(), ports.preferredHeight())));
            status = side.reduced (8, 10);
        }

    private:
        void timerCallback() override
        {
            if (isShowing())
                repaint (status);
        }

        UiContext& ctx;
        SettingsPanel daw, ports { "DAW PORTS" };
        DawSurface surface;
        juce::Rectangle<int> panel, disclaimer, status;
    };

    //==============================================================================
    /** PLUGINS (app-specific): plugin scanning; the plugins become FX slot devices. */
    class PluginsTab final : public juce::Component, private juce::ChangeListener
    {
    public:
        explicit PluginsTab (UiContext& c) : ctx (c)
        {
            scan.onClick = [this] { if (ctx.showPluginScanner) ctx.showPluginScanner(); };
            styleIconKey (scan);
            addAndMakeVisible (scan);
            ctx.host.getKnownPlugins().addChangeListener (this);
        }

        ~PluginsTab() override { ctx.host.getKnownPlugins().removeChangeListener (this); }

        void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colours::black);
            auto body = drawDarkPanel (g, box, "PLUGINS");
            auto r = body.reduced (12, 10);
            const auto types = ctx.host.getSortedTypes();
            g.setColour (Dark::text);
            g.setFont (wingFont (TypeScale::statusTitle, WingWeight::SemiBold));
            g.drawText (juce::String (types.size()) + " PLUGINS KNOWN", r.removeFromTop (34), juce::Justification::centredLeft);
            g.setColour (Dark::textDim);
            g.setFont (wingFont (TypeScale::body));
            juce::String text;
            text << "VST3, AU, LV2" << (ctx.host.getFormatManager().getNumFormats() > 3 ? ", VST2" : "")
                 << ". Load them into FX slots on the EFFECTS screen (device EXT), then assign the slots to channel inserts. "
                 << "Plugins are scanned in a separate process, so a crashing plugin can't take the console down.";
            g.drawFittedText (text, r.removeFromTop (70), juce::Justification::topLeft, 4);
        }

        void resized() override
        {
            box = getLocalBounds().withWidth (juce::jmin (getWidth(), screenCap (720))).withHeight (juce::jmin (getHeight(), screenCap (240)));
            scan.setBounds (darkPanelBody (box, true).reduced (12, 10).removeFromBottom (44).removeFromLeft (260));
        }

    private:
        UiContext& ctx;
        WingKey scan { "SCAN FOR PLUGINS", Icon::Folder };
        juce::Rectangle<int> box;
    };
} // namespace

//==============================================================================
SetupPage::SetupPage (UiContext& c) : ctx (c)
{
    tabRow.setTabs ({ { "GENERAL" }, { "AUDIO" }, { "SURFACE" }, { "REMOTE" }, { "DAW" }, { "PLUGINS", Dark::gold, true, 10 }, { "LOG" } });
    tabRow.onChange = [this] (int) { showTab(); };
    addAndMakeVisible (tabRow);

    auto overlay = [this] (const juce::String& title, std::unique_ptr<juce::Component> body) { overlays.show (*this, title, std::move (body)); };
    general = std::make_unique<GeneralTab> (ctx);
    pages.push_back (general.get());
    pages.push_back (ownedPages.add (new AudioTab (ctx, overlay)));
    pages.push_back (ownedPages.add (new SurfaceTab (ctx)));
    pages.push_back (ownedPages.add (new RemoteTab (ctx)));
    pages.push_back (ownedPages.add (new DawTab (ctx)));
    pages.push_back (ownedPages.add (new PluginsTab (ctx)));
    // LOG: system health and the event log (the UTILITY screen's content; the
    // WING's UTILITY key has no screen of its own).
    pages.push_back (ownedPages.add (new UtilityPage (ctx, true)));
    for (auto* p : pages)
        addChildComponent (p);

    styleIconKey (initKey);
    styleIconKey (shutdownKey);
    initKey.onClick = [this]
    {
        popups::confirm (*this, "INITIALIZE CONSOLE",
                         "Reset every strip, DCA, mute group and FX slot to a fresh 40-channel console? "
                         "GLOBAL safes are overwritten too. Scenes are kept.",
                         "INITIALIZE", [this]
                         {
                             for (int i = 0; i < kNumFx; ++i)
                                 clearFx (ctx, i);
                             ctx.engine.reset();
                             ctx.engine.setNumChannels (kWingChannels);
                             ctx.log.info ("Console initialized");
                             if (ctx.structureChanged)
                                 ctx.structureChanged();
                         },
                         true);
    };
    shutdownKey.onClick = [this]
    {
        // The console state is saved when the app quits.
        popups::confirm (*this, "SHUTDOWN", "IT IS SAFE TO SWITCH DOWN THE CONSOLE NOW", "QUIT", []
        {
            if (auto* app = juce::JUCEApplicationBase::getInstance())
                app->systemRequestedQuit();
        });
    };
    addAndMakeVisible (initKey);
    addAndMakeVisible (shutdownKey);
    showTab();

    // The hooks other screens use to open SETUP's tabs and sub-screens.
    ctx.showSetupTab = [this] (int tab)
    {
        if (ctx.showScreen)
            ctx.showScreen (Screen::Setup);
        closeSubScreen();
        tabRow.setCurrent (juce::jlimit (0, (int) pages.size() - 1, tab));
    };
    ctx.showCustomControls = [this] (int layer)
    {
        if (ctx.showScreen)
            ctx.showScreen (Screen::Setup);
        openCustomControls (layer);
    };
    ctx.showMonitors = [this]
    {
        if (ctx.showScreen)
            ctx.showScreen (Screen::Setup);
        openMonitors();
    };
}

bool SetupPage::screenKeyPressedAgain()
{
    // "Press SETUP twice for CC EDIT" (firmware 3.1); again goes back to the tabs.
    if (subScreen != nullptr)
        closeSubScreen();
    else
        openCustomControls (-1);
    return true;
}

SetupPage::~SetupPage()
{
    ctx.showSetupTab = nullptr;
    ctx.showCustomControls = nullptr;
    ctx.showMonitors = nullptr;
}

void SetupPage::visibilityChanged()
{
    // Another screen key: SETUP opens on its tabs next time.
    if (! isVisible())
        closeSubScreen();
}

void SetupPage::showSubScreen (std::unique_ptr<DisplayPage> screen)
{
    overlays.close();
    subScreen = std::move (screen);
    if (subScreen != nullptr)
    {
        subScreen->knobsChanged = [this] { notifyKnobsChanged(); };
        subScreen->statusKeysChanged = [this] { notifyStatusKeysChanged(); };
        addAndMakeVisible (*subScreen);
        subScreen->setBounds (getLocalBounds());
        subScreen->refresh();
    }
    for (auto* c : { (juce::Component*) &tabRow, (juce::Component*) &initKey, (juce::Component*) &shutdownKey })
        c->setVisible (subScreen == nullptr);
    showTab();
    notifyStatusKeysChanged();
    notifyKnobsChanged();
}

void SetupPage::closeSubScreen()
{
    if (subScreen != nullptr)
        showSubScreen (nullptr);
}

void SetupPage::openCustomControls (int layer)
{
    if (ctx.customControls != nullptr)
        showSubScreen (std::make_unique<CustomControlsPage> (ctx, *ctx.customControls, layer));
}

void SetupPage::openMonitors()
{
    showSubScreen (std::make_unique<MonitorsPage> (ctx));
}

std::vector<KnobBinding> SetupPage::getKnobs()
{
    return subScreen != nullptr ? subScreen->getKnobs() : std::vector<KnobBinding> {};
}

std::vector<KnobStyle> SetupPage::getKnobStyles()
{
    return subScreen != nullptr ? subScreen->getKnobStyles() : std::vector<KnobStyle> {};
}

StatusTitle SetupPage::getStatusTitle()
{
    return subScreen != nullptr ? subScreen->getStatusTitle() : StatusTitle {};
}

std::vector<StatusKey> SetupPage::getStatusKeys()
{
    // CUSTOM CONTROLS and MONITORS have no keys of their own.
    if (subScreen != nullptr)
        return subScreen->getStatusKeys();
    return {
        { "MONITORS", Icon::Monitors, false, [this] { openMonitors(); } },
        { "CC EDIT", Icon::CcEdit, false, [this] { openCustomControls (-1); } },
    };
}

void SetupPage::showTab()
{
    for (size_t i = 0; i < pages.size(); ++i)
        pages[i]->setVisible (subScreen == nullptr && (int) i == tabRow.getCurrent());
    overlays.close();
}

void SetupPage::refresh()
{
    if (subScreen != nullptr)
        subScreen->refresh();
    if (auto* g = dynamic_cast<GeneralTab*> (general.get()))
        g->refresh();
    for (auto* p : pages)
        p->repaint();
}

void SetupPage::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
    g.setColour (Dark::tabBar);
    g.fillRect (tabRow.getBounds().withLeft (0).withRight (getWidth()));
}

void SetupPage::resized()
{
    auto b = getLocalBounds();
    auto row = b.removeFromTop (juce::jlimit (30, screenCap (44), getHeight() / 12));
    b.removeFromTop (6);
    const int keyW = juce::jmin (screenCap (180), getWidth() * 13 / 100);
    // Narrow screens: the icon over a one-word caption.
    const bool narrow = keyW < 80;
    initKey.iconAbove = shutdownKey.iconAbove = narrow;
    initKey.setButtonText (narrow ? "INITIALIZE" : "INITIALIZE\nCONSOLE");
    shutdownKey.setBounds (row.removeFromRight (keyW).reduced (2, 2));
    row.removeFromRight (4);
    initKey.setBounds (row.removeFromRight (keyW).reduced (2, 2));
    row.removeFromRight (8);
    // Tabs about 12 % of the screen wide, as on the console, if all seven fit.
    const float tabPx = juce::jmin ((float) getWidth() * 0.12f, (float) (row.getWidth() - 12) / 7.0f);
    tabRow.tabFraction = tabPx / (float) juce::jmax (1, row.getWidth());
    tabRow.setBounds (row);
    for (auto* p : pages)
        p->setBounds (b);
    if (subScreen != nullptr)
        subScreen->setBounds (getLocalBounds());
    overlays.layout (getLocalBounds());
}

} // namespace livedaw::ui
