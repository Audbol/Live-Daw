#include "engine/Models.h"
#include "engine/AppPaths.h"
#include "engine/Recorder.h"
#include "engine/UsbRecorder.h"
#include "ui/CustomControlsPage.h"
#include "ui/HomeScreens.h"
#include "ui/MidiRemoteUi.h"
#include "ui/MonitorsPage.h"
#include "ui/Pages.h"
#include "ui/ScopeWindow.h"
#include "ui/ScreenWidgets.h"
#include "ui/StartupNotice.h"
#include "ui/WebMirror.h"

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

    /** A panel with the console's SETUP metrics: 64 px headers, rows of 52 / 56 px. */
    void setupPanel (SettingsPanel& p, int rowHeight)
    {
        p.headerHeight = setupHeaderHeight();
        p.headerGap = setupGap();
        p.wingRows = true;
        p.rowHeight = rowHeight;
    }

    /** A dropdown field's row: a 64 px field in 80 px. */
    constexpr float kFieldRows = 80.0f / 52.0f;

    /** Lit colours of a row of `n` keys whose first is OFF: a lit OFF is grey [fig. 34]. */
    std::vector<juce::Colour> offFirst (int n)
    {
        std::vector<juce::Colour> c ((size_t) n, Dark::choice);
        c[0] = Dark::offLit;
        return c;
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
            // [Fig. 40] separate keys, 6 px apart, each in a thin black frame.
            auto r = getLocalBounds().toFloat();
            const float gap = juce::jmax (2.0f, wingPx (6.0f));
            const float w = (r.getWidth() + gap) / (float) juce::jmax (1, names.size());
            for (int i = 0; i < names.size(); ++i)
            {
                auto k = juce::Rectangle<float> (r.getX() + w * (float) i, r.getY(), w - gap, r.getHeight());
                const bool on = isOn (i);
                g.setColour (juce::Colours::black);
                g.fillRect (k);
                k = k.reduced (1.0f);
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
            // [Fig. 33] a tinted tile in a thin black frame: a thick ring open at
            // the bottom (white over a grey track), the caption over "NN %".
            auto r = getLocalBounds().toFloat();
            const float alpha = isEnabled() ? 1.0f : 0.45f;
            g.setColour (juce::Colours::black);
            g.fillRect (r);
            r = r.reduced (juce::jmax (1.0f, wingPx (2.0f)));
            g.setColour (tint.withMultipliedAlpha (alpha));
            g.fillRect (r);
            const float h = r.getHeight();
            const juce::Point<float> centre (r.getX() + h * 0.55f, r.getCentreY());
            const float a0 = -juce::MathConstants<float>::pi * 0.8f, a1 = juce::MathConstants<float>::pi * 0.8f;
            const float rad = h * 0.3f, thick = juce::jmax (2.0f, h * 0.16f);
            juce::Path track, arc;
            track.addCentredArc (centre.x, centre.y, rad, rad, 0.0f, a0, a1, true);
            arc.addCentredArc (centre.x, centre.y, rad, rad, 0.0f, a0, a0 + (a1 - a0) * (float) value() / 100.0f, true);
            g.setColour (juce::Colour (0xff484848).withMultipliedAlpha (alpha));
            g.strokePath (track, juce::PathStrokeType (thick));
            g.setColour (Dark::text.withAlpha (alpha));
            g.strokePath (arc, juce::PathStrokeType (thick));

            auto t = r.withTrimmedLeft (h * 1.11f).reduced (0.0f, h * 0.06f);
            g.setColour (Dark::caption.withAlpha (alpha));
            auto captionArea = t.removeFromTop (t.getHeight() * 0.42f);
            g.setFont (fontFittingWidth (wingFontPx (juce::jmin (TypeScale::body * getWingScreenHeight(), captionArea.getHeight() * 0.62f)), caption,
                                         captionArea.getWidth(), 0.7f));
            g.drawFittedText (caption, captionArea.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
            g.setColour (Dark::text.withAlpha (alpha));
            g.setFont (wingFontPx (juce::jmin (TypeScale::value * getWingScreenHeight(), t.getHeight() * 0.72f)));
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
            // [Fig. 33] a #101010 well and its dots key, 2 px apart.
            auto r = getLocalBounds();
            auto key = r.removeFromRight (juce::roundToInt ((float) r.getWidth() * 0.16f));
            r.removeFromRight (juce::jmax (1, juce::roundToInt (wingPx (2.0f))));
            g.setColour (Dark::well);
            g.fillRect (r);
            g.setColour (Dark::key);
            g.fillRect (key);
            const auto kc = key.getCentre().toFloat();
            const float d = juce::jmax (3.0f, (float) key.getWidth() * 0.27f);
            g.setColour (Dark::caption);
            for (int i = -1; i <= 1; ++i)
                g.fillEllipse (juce::Rectangle<float> (d, d).withCentre (kc.translated (0.0f, (float) i * d * 1.6f)));
            auto t = r.reduced (juce::roundToInt (wingPx (8.0f)), juce::roundToInt (wingPx (4.0f)));
            g.setColour (Dark::caption);
            g.setFont (wingFontPx (juce::jmin (TypeScale::body * getWingScreenHeight(), (float) t.getHeight() * 0.28f)));
            g.drawFittedText (date ? "DATE" : "TIME", t.removeFromTop (t.getHeight() * 45 / 100), juce::Justification::centredLeft, 1);
            g.setColour (Dark::text);
            g.setFont (wingFontPx (juce::jmin (TypeScale::value * getWingScreenHeight(), (float) t.getHeight() * 0.6f)));
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
    /** CONSOLE: name, software, engine, audio device, the console size, the
        surface's finish and the data folders. */
    class ConsolePanel final : public juce::Component
    {
    public:
        explicit ConsolePanel (UiContext& c) : ctx (c)
        {
            editKey.onClick = [this]
            {
                editText (this, "CONSOLE NAME", ctx.settings.getValue (prefs::consoleName, "SIDECAR"), [this] (const juce::String& name)
                {
                    const auto newName = name.isNotEmpty() ? name.toUpperCase() : juce::String ("SIDECAR");
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
            addAndMakeVisible (finishKeys);
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
            drawDarkPanel (g, getLocalBounds(), "CONSOLE", setupHeaderHeight(), setupGap());
            // [Fig. 33] the info box and, under a black gap, the box of the two
            // folder keys (the console's USB DIRECT ACCESS) with its dim title.
            g.setColour (juce::Colours::black);
            g.fillRect (infoBox.getX(), infoBox.getBottom(), infoBox.getWidth(), dataBox.getY() - infoBox.getBottom());
            g.setColour (Dark::box);
            g.fillRect (infoBox);
            g.fillRect (dataBox);
            g.setColour (Dark::textDim.brighter (0.4f));
            g.setFont (wingFontPx (juce::jmin (TypeScale::body * getWingScreenHeight(), (float) dataTitle.getHeight() * 0.6f)));
            g.drawFittedText ("DATA", dataTitle, juce::Justification::centred, 1);

            auto* device = ctx.deviceManager.getCurrentAudioDevice();
            const std::pair<juce::String, juce::String> info[] = {
                { "CONSOLE NAME (network)", ctx.settings.getValue (prefs::consoleName, "SIDECAR") },
                // The audio thread and the workers, as the log and SETUP > LOG count them.
                { "ENGINE", "RUST, " + juce::String (ctx.engine.getNumWorkerThreads() + 1) + " DSP THREADS" },
                { "SOFTWARE", juce::String (kAppName).toUpperCase() + " " + juce::String (JUCE_APPLICATION_VERSION_STRING) },
                { "AUDIO DEVICE", device != nullptr ? device->getName().toUpperCase() : juce::String ("NONE") },
            };
            for (size_t i = 0; i < std::size (info); ++i)
            {
                auto row = infoRows[i];
                // [fig. 33] a faint small caption over a large value.
                g.setColour (Dark::textDim.brighter (0.3f));
                g.setFont (wingFontPx (juce::jmin (TypeScale::statusCaption * getWingScreenHeight(), (float) row.getHeight() * 0.22f)));
                g.drawText (info[i].first, row.removeFromTop (row.getHeight() * 40 / 100), juce::Justification::bottomLeft);
                g.setColour (Dark::text);
                g.setFont (wingFontPx (juce::jmin ((i == 0 ? TypeScale::value : TypeScale::body * 1.1f) * getWingScreenHeight(), (float) row.getHeight() * 0.42f)));
                g.drawFittedText (info[i].second, row.withTrimmedRight (i == 0 ? editKey.getWidth() + 4 : (i == 2 ? aboutKey.getWidth() + 4 : 0)),
                                  juce::Justification::centredLeft, 1, 0.8f);
            }

            g.setColour (Dark::textDim.brighter (0.3f));
            g.setFont (wingFontPx (juce::jmin (TypeScale::statusCaption * getWingScreenHeight(), (float) configCaption.getHeight() * 0.9f)));
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
            g.setFont (wingFontFit (TypeScale::body, (float) finishRow.getHeight() * 0.55f));
            g.drawFittedText ("SURFACE FINISH", finishRow.withRight (finishKeys.getX() - 4), juce::Justification::centredLeft, 1, 0.8f);
        }

        void resized() override
        {
            // [Fig. 33] the info box 296 px tall, a gap, the 100 px folder box.
            auto b = darkPanelBody (getLocalBounds(), true, setupHeaderHeight(), setupGap());
            dataBox = b.removeFromBottom (juce::jmin (b.getHeight() / 3, juce::roundToInt (wingPx (100.0f))));
            b.removeFromBottom (setupGap());
            infoBox = b;
            auto d = dataBox;
            dataTitle = d.removeFromTop (d.getHeight() * 30 / 100);
            d = d.reduced (juce::roundToInt (wingPx (16.0f)), 0).withTrimmedBottom (juce::roundToInt (wingPx (10.0f)));
            const int keyGap = juce::roundToInt (wingPx (16.0f));
            dataKey.setBounds (d.removeFromLeft ((d.getWidth() - keyGap) / 2));
            d.removeFromLeft (keyGap);
            showKey.setBounds (d);

            auto r = b.reduced (juce::roundToInt (wingPx (16.0f)), juce::roundToInt (wingPx (8.0f)));
            const int configH = r.getHeight() * 40 / 100;
            auto config = r.removeFromBottom (configH);
            const int rowH = r.getHeight() / 4;
            for (auto& row : infoRows)
                row = r.removeFromTop (rowH);
            const int keyW = juce::jmin (juce::roundToInt (wingPx (52.0f)), rowH + 12);
            editKey.setBounds (infoRows[0].withTrimmedLeft (infoRows[0].getWidth() - keyW).reduced (0, 2));
            aboutKey.setBounds (infoRows[2].withTrimmedLeft (infoRows[2].getWidth() - keyW).reduced (0, 2));

            configCaption = config.removeFromTop (config.getHeight() / 5);
            const int h = config.getHeight() / 3;
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
            finishRow = config.removeFromTop (h).reduced (0, 2);
            finishKeys.setBounds (finishRow.withTrimmedLeft (finishRow.getWidth() - juce::jmin (finishRow.getWidth() * 62 / 100, screenCap (200))).reduced (2, 0));
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
        juce::Rectangle<int> infoBox, dataBox, dataTitle, infoRows[4], configCaption, chRow, busRow, chValue, busValue, finishRow;
        // SURFACE FINISH: the surface view in the silver or the black WING's colours.
        ChoiceKeys finishKeys { { ConsoleLook::finishName (SurfaceFinish::Silver), ConsoleLook::finishName (SurfaceFinish::Black) },
                                [this] { return (int) ConsoleLook::load (ctx.settings).finish; },
                                [this] (int i)
                                {
                                    auto look = ConsoleLook::load (ctx.settings);
                                    look.finish = i == 1 ? SurfaceFinish::Black : SurfaceFinish::Silver;
                                    look.save (ctx.settings);
                                    setSurfaceFinish (look.finish);
                                    if (auto* top = getTopLevelComponent())
                                        top->repaint();
                                } };
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
            // The tiles' tints [fig. 33]: displays blue, LEDs red, scribble strips purple, PATCH and GLOW
            // brown, LAMP olive. CH STRIP is the channel strip's display (the setting keeps its old id).
            const juce::Colour blue (0xff102438), red (0xff381410), purple (0xff380020), brown (0xff383008), olive (0xff383800);
            struct Spec { const char* caption; const char* id; int value; juce::Colour tint; bool enabled; };
            const Spec specs[] = { { "MAIN LCD", "mainLcd", 80, blue, true }, { "CH STRIP", "btnLcd", 80, blue, true },
                                   { "BTNS/LEDS", "btnsLeds", 90, red, true }, { "BACKLIGHT", "backlight", 25, red, true },
                                   { "METERS", "meters", 40, red, true }, { "COLOR LED", "colorLed", 25, purple, true },
                                   { "SCRIBBLE", "scribble", 60, purple, true }, { "SCR CONT", "scrCont", 50, purple, true },
                                   { "PATCH", "patch", 0, brown, false }, { "GLOW", "glow", 0, brown, false },
                                   { "LAMP", "lamp", 0, olive, false } };
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

            // The CC section's arrows and wheel move a focus frame over the screen and edit it (firmware 1.13).
            options.addRow ("USE CRSR/WHEEL FOR PARAMETERS", OnKey::forSetting (ctx.settings, uiprefs::useCursorWheel, true), 1.3f);
            options.addRow ("TOUCH FADER SELECT", OnKey::forSetting (ctx.settings, uiprefs::touchFaderSelect, false), 1.3f);
            options.addRow ("TOUCH FADER RES",
                            std::make_unique<ChoiceKeys> (juce::StringArray { "NORM", "FINE", "AUTO" },
                                                          [this] { return prefs::faderResolution (ctx); },
                                                          [this] (int i) { ctx.settings.setValue (prefs::touchFaderRes, i); }),
                            3.6f);
            options.newBox();
            options.addRow ("MOUSE DISABLES TOUCH", OnKey::fixed (false), 1.3f, false);
            options.addRow ("MOUSE SPEED", std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox), 2.5f, false);
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
                                                          std::vector<juce::Colour> { Dark::orange, Dark::usbGreen }),
                            2.4f, true, true);
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

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colours::black);
            g.setColour (Dark::body);
            g.fillRect (lightsArea);
        }

        void resized() override
        {
            // [Fig. 33] in px of the 1280 x 800 screen: a 150 px band of
            // 168 x 62 tiles on a 176 px pitch with TIME and DATE at its right,
            // then three 416 px columns 8 px apart.
            auto b = getLocalBounds();
            const float sx = (float) b.getWidth() / 1264.0f;
            auto x = [&] (float px) { return b.getX() + juce::roundToInt (px * sx); };
            auto y = [&] (float px) { return b.getY() + juce::roundToInt (wingPx (px)); };
            lightsArea = juce::Rectangle<int>::leftTopRightBottom (b.getX(), b.getY(), b.getRight(), y (150.0f));
            for (int i = 0; i < lights.size(); ++i)
            {
                // Five columns of two tiles, then LAMP.
                const int col = i < 10 ? i / 2 : 5, row = i < 10 ? i % 2 : 0;
                lights[i]->setBounds (juce::Rectangle<int>::leftTopRightBottom (x (8.0f + 176.0f * (float) col), y (8.0f + 72.0f * (float) row),
                                                                                 x (176.0f + 176.0f * (float) col), y (70.0f + 72.0f * (float) row)));
            }
            time.setBounds (juce::Rectangle<int>::leftTopRightBottom (x (1064.0f), y (8.0f), x (1256.0f), y (70.0f)));
            date.setBounds (juce::Rectangle<int>::leftTopRightBottom (x (1064.0f), y (80.0f), x (1256.0f), y (142.0f)));

            const int top = y (160.0f);
            auto column = [&] (float left) { return juce::Rectangle<int>::leftTopRightBottom (x (left), top, x (left + 416.0f), b.getBottom()); };
            console.setBounds (column (0.0f));
            auto operationArea = column (424.0f), optionsArea = column (848.0f);
            int rowH = juce::roundToInt (wingPx (52.0f));
            for (auto* p : { &operation, &options })
                setupPanel (*p, rowH);
            while (rowH > 10 && options.preferredHeight() > optionsArea.getHeight())
                options.rowHeight = operation.rowHeight = --rowH;
            operation.setBounds (operationArea.withHeight (juce::jmin (operationArea.getHeight(), operation.preferredHeight())));
            options.setBounds (optionsArea.withHeight (juce::jmin (optionsArea.getHeight(), options.preferredHeight())));
        }

    private:
        UiContext& ctx;
        juce::OwnedArray<LightTile> lights;
        ClockTile time { ctx, false }, date { ctx, true };
        ConsolePanel console;
        SettingsPanel operation { "OPERATION" }, options { "OPTIONS" };
        juce::Rectangle<int> lightsArea;
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
            // [Fig. 34] 20 px rounded cells on a 22 px pitch, dark blue (dark
            // orange on ALT) with black numbers, under small grey captions.
            cells.clear();
            auto r = getLocalBounds().toFloat().reduced (4.0f);
            const float captionH = juce::jmin (wingPx (20.0f), r.getHeight() / 9.0f);
            const float cell = juce::jmin (wingPx (22.0f), r.getWidth() / 8.0f, (r.getHeight() - captionH * 2.0f - wingPx (16.0f)) / 6.0f);
            r.removeFromTop (wingPx (6.0f));
            auto drawGrid = [&] (const char* caption, StripKind kind, int rows)
            {
                g.setColour (Dark::caption);
                g.setFont (wingFontPx (juce::jmin (TypeScale::statusCaption * getWingScreenHeight(), captionH * 0.6f)));
                g.drawText (caption, r.removeFromTop (captionH), juce::Justification::centred);
                const auto& strips = ctx.engine.getStrips (kind);
                for (int i = 0; i < rows * 8; ++i)
                {
                    auto c = juce::Rectangle<float> (r.getCentreX() - cell * 4.0f + cell * (float) (i % 8), r.getY() + cell * (float) (i / 8), cell, cell)
                                 .reduced (cell * 0.05f);
                    auto* s = i < (int) strips.size() ? strips[(size_t) i].get() : nullptr;
                    const bool alt = s != nullptr && s->isUsingAltInput();
                    g.setColour (alt ? altCell : mainCell);
                    g.fillRoundedRectangle (c, cell * 0.12f);
                    if (s != nullptr)
                    {
                        g.setColour (juce::Colours::black);
                        g.setFont (wingFontPx (c.getHeight() * 0.5f));
                        g.drawText (juce::String (s->getNumber()), c, juce::Justification::centred);
                        cells.push_back ({ c, s });
                    }
                }
                r.removeFromTop (cell * (float) rows + wingPx (16.0f));
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
        const juce::Colour mainCell { 0xff384c80 }, altCell { 0xff583008 };
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
                          true, false, kFieldRows);
            // The sync source is the audio interface's business.
            clock.addRow ({}, std::make_unique<DropdownField> ("SYNC SOURCE", [] { return juce::String ("INTERNAL"); }), 0.0f, false, false, kFieldRows);
            addAndMakeVisible (clock);

            // The global MAIN / ALT switch (used by every channel while GLOBAL
            // INPUT SELECT OVERRIDE is on) and the channels' own switches.
            inputSelect.addRow ({}, std::make_unique<ChoiceKeys> (juce::StringArray { "MAIN", "ALT" }, [this] { return ctx.engine.isGlobalAlt() ? 1 : 0; },
                                                                 [this] (int i) { ctx.engine.setGlobalAlt (i == 1); }, std::vector<juce::Colour> { Colours::valueBlue, altOrange }),
                                0.0f, true, false, kFieldRows);
            inputSelect.newBox();
            inputSelect.addRow ({}, std::make_unique<InputSelectGrid> (ctx), 0.0f, true, false, 4.0f);
            addAndMakeVisible (inputSelect);

            prefsPanel.addRow ("MAIN LINK",
                               std::make_unique<ChoiceKeys> (models::list (models::MainLinkModes), [this] { return (int) ctx.engine.mainLink.load(); },
                                                             [this] (int i) { ctx.engine.mainLink = i; }, offFirst (4)),
                               4.8f);
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
                                                             },
                                                             offFirst (3)),
                               3.65f);
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
                                                             },
                                                             offFirst (3)),
                               3.65f);
            prefsPanel.newBox();
            prefsPanel.addRow ("OUTPUT PROTECTION", OnKey::forAtomic (ctx.engine.outputProtection), 1.3f);
            auto ceilingSlider = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
            ceilingSlider->setRange (-24.0, 6.0, 0.1);
            ceilingSlider->setTextValueSuffix (" dB");
            ceilingSlider->setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
            ceilingSlider->setDoubleClickReturnValue (true, 0.0);
            ceiling = ceilingSlider.get();
            ceiling->onValueChange = [this] { ctx.engine.outputCeilingDb = (float) ceiling->getValue(); };
            prefsPanel.addRow ("CEILING", std::move (ceilingSlider), 4.8f);
            addAndMakeVisible (prefsPanel);

            // LIVE: each channel's SOLO BUS (phones / speakers); STUDIO: the solo
            // bus picked on MONITORS; SIP: solo in place (the SIP badge lights).
            solo.addRow ("SOLO MODE",
                         std::make_unique<ChoiceKeys> (juce::StringArray { "LIVE", "STUDIO", "SIP" }, [this] { return (int) ctx.engine.soloMode.load(); },
                                                       [this] (int i) { ctx.engine.soloMode = i; },
                                                       std::vector<juce::Colour> { Dark::olive, Dark::olive, Dark::olive }),
                         3.4f);
            solo.newBox();
            // PFL or AFL per strip type (CHANNEL SOLO covers the aux channels too).
            for (auto [name, kind] : { std::pair { "CHANNEL SOLO", StripKind::Channel }, std::pair { "BUS SOLO", StripKind::Bus },
                                       std::pair { "MAIN SOLO", StripKind::Main }, std::pair { "MATRIX SOLO", StripKind::Matrix } })
            {
                const auto k = kind;
                solo.addRow (name,
                             std::make_unique<ChoiceKeys> (juce::StringArray { "PFL", "AFL" }, [this, k] { return ctx.engine.isSoloAfl (k) ? 1 : 0; },
                                                           [this, k] (int i) { ctx.engine.setSoloAfl (k, i == 1); }, ChoiceKeys::prePost()),
                             2.46f);
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
            solo.addRow ("SEND PAGE ACTIVATES SOLO", OnKey::fixed (false), 1.3f, false); // not there yet
            addAndMakeVisible (solo);

            device.addRow ({}, std::make_unique<DropdownField> ("DEVICE", [this] { return deviceName(); },
                                                                [this] (DropdownField&) { showOverlay ("AUDIO DEVICE", std::make_unique<DeviceBody> (ctx.deviceManager)); }),
                           0.0f, true, false, kFieldRows);
            device.addRow ({}, std::make_unique<DropdownField> ("IN/OUT", [this] { return channelsText(); }), 0.0f, false, false, kFieldRows);
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
            // [Fig. 34] AES50 PORTS' place: the title stays centred, clear of the CLEAR key at the right.
            auto body = drawDarkPanel (g, errorsArea, " ", setupHeaderHeight(), setupGap());
            g.setColour (Dark::caption);
            const auto title = errorsArea.withHeight (setupHeaderHeight()).reduced (clearKey.getWidth() + 2, 0);
            g.setFont (fontFittingWidth (panelTitleFont (setupHeaderHeight()), "ERRORS", (float) title.getWidth(), 0.8f));
            g.drawFittedText ("ERRORS", title, juce::Justification::centred, 1, 0.8f);
            auto* d = ctx.deviceManager.getCurrentAudioDevice();
            const std::pair<const char*, juce::String> rows[] = { { "XRUNS", d != nullptr ? juce::String (d->getXRunCount()) : juce::String ("-") },
                                                                  { "SAFETY CLIPS", juce::String (ctx.engine.outputClipEvents.load()) } };
            auto r = body.reduced (juce::roundToInt (wingPx (12.0f)), juce::roundToInt (wingPx (6.0f)));
            const int h = juce::jmin (juce::roundToInt (wingPx (28.0f)), r.getHeight() / 2);
            for (auto& [label, value] : rows)
            {
                auto row = r.removeFromTop (h);
                g.setColour (Dark::text);
                g.setFont (wingFontPx (juce::jmin (TypeScale::body * getWingScreenHeight(), (float) h * 0.55f)));
                g.drawText (label, row, juce::Justification::centredLeft);
                g.setColour (Dark::textDim);
                g.drawText (value, row, juce::Justification::centredRight);
            }
        }

        void resized() override
        {
            // [Fig. 34] columns of 244, 376, 376 and 244 px, 8 px apart.
            auto b = getLocalBounds();
            const int gap = setupGap();
            const float sx = (float) b.getWidth() / 1264.0f;
            auto take = [&] (float px)
            {
                auto c = b.removeFromLeft (juce::roundToInt (px * sx));
                b.removeFromLeft (gap);
                return c;
            };
            auto c1 = take (244.0f), c2 = take (376.0f), c3 = take (376.0f), c4 = b;

            int rowH = juce::roundToInt (wingPx (52.0f));
            for (auto* p : { &clock, &inputSelect, &prefsPanel, &solo, &device, &automix })
                setupPanel (*p, rowH);
            // A small window: rows shrink until the tallest column fits.
            auto tallest = [this, gap] { return juce::jmax (solo.preferredHeight(), clock.preferredHeight() + gap + inputSelect.preferredHeight(),
                                                             device.preferredHeight() + automix.preferredHeight() + errorsHeight() + 2 * gap); };
            while (rowH > 10 && tallest() > getHeight())
            {
                --rowH;
                for (auto* p : { &clock, &inputSelect, &prefsPanel, &solo, &device, &automix })
                    p->rowHeight = rowH;
            }
            if (ceiling != nullptr)
                ceiling->setTextBoxStyle (juce::Slider::TextBoxRight, false, screenCap (64), juce::jmin (rowH, screenCap (20)));

            clock.setBounds (c1.removeFromTop (clock.preferredHeight()));
            c1.removeFromTop (gap);
            inputSelect.setBounds (c1.withHeight (juce::jmin (c1.getHeight(), inputSelect.preferredHeight())));
            prefsPanel.setBounds (c2.withHeight (juce::jmin (c2.getHeight(), prefsPanel.preferredHeight())));
            solo.setBounds (c3.withHeight (juce::jmin (c3.getHeight(), solo.preferredHeight())));
            device.setBounds (c4.removeFromTop (device.preferredHeight()));
            c4.removeFromTop (gap);
            automix.setBounds (c4.removeFromTop (automix.preferredHeight()));
            c4.removeFromTop (gap);
            errorsArea = c4.withHeight (juce::jmin (c4.getHeight(), errorsHeight()));
            clearKey.setBounds (errorsArea.withHeight (setupHeaderHeight()).removeFromRight (setupHeaderHeight() + 16).reduced (2, 1));
        }

    private:
        /** ERRORS: its header and two 28 px rows (AES50 PORTS' 100 px box). */
        int errorsHeight() const { return setupHeaderHeight() + setupGap() + juce::roundToInt (wingPx (100.0f)); }

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
    /** SURFACE (fig. 40): METERING | GENERAL | FADERS. The app's own fader
        settings (SHIFT STEP, SECTION WIDTH, FADER VALUES) open from the
        SECTIONS key in the FADERS header. */
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
                             3.6f, true, false, kFieldRows);
            metering.addRow ("MAIN METER TAP", fixedChoice ({ "AUTO", "PRE", "POST" }, 2, ChoiceKeys::autoPrePost()), 3.6f, false);
            // PRE or POST per strip type: the VIEW screens' meters follow these (the
            // surface's LED meters can read them with prefs::meterPoint).
            for (int i = 0; i < prefs::kNumMeterTaps; ++i)
                metering.addRow (prefs::meterTapNames[i],
                                 std::make_unique<ChoiceKeys> (juce::StringArray { "PRE", "POST" }, [this, i] { return prefs::meterTapPre (ctx, i) ? 0 : 1; },
                                                               [this, i] (int v) { ctx.settings.setValue (prefs::meterTapKey (i), v == 0); }, ChoiceKeys::prePost()),
                                 2.4f);
            addAndMakeVisible (metering);

            // SELECT on the selected strip again (ui.selDblClick: 0 HOME, 1 BUSFX (the factory setting), 2 OFF).
            general.addRow ("SEL DBL CLICK",
                            std::make_unique<ChoiceKeys> (juce::StringArray { "OFF", "HOME", "BUSFX" },
                                                          [this]
                                                          {
                                                              const int v = ctx.settings.getIntValue ("ui.selDblClick", 1);
                                                              return v == 2 ? 0 : v == 1 ? 2 : 1;
                                                          },
                                                          [this] (int i)
                                                          {
                                                              ctx.settings.setValue ("ui.selDblClick", i == 0 ? 2 : i == 2 ? 1 : 0);
                                                              notifyLayersChangedAsync (ctx);
                                                          }),
                            3.7f);
            // The rest change how the fader strips, the channel strip and the status bar behave.
            general.addRow ("TAP TEMPO FLASH", fixedChoice ({ "OFF", "8X", "ON" }, 2), 3.7f, false); // custom control tap keys
            general.addRow ("SOF BUTTON", choiceSetting (uiprefs::sofButton, { "AUTO", "ON", "FLASH" }, 0), 3.7f);
            general.addRow ("SHOW SOF FRAME", OnKey::forSetting (ctx.settings, uiprefs::showSofFrame, true), 1.3f);
            general.addRow ("SHOW SOURCE ON SCRIBBLE", onSetting (uiprefs::sourceOnScribble, true), 1.3f);
            general.addRow ("SHOW FADER VALUE ON SCRIBBLE", onSetting (uiprefs::faderValueOnScribble, false), 1.3f);
            general.addRow ("USE F1-F3 AS CUSTOM CONTROLS", onSetting (uiprefs::f1f3AsCustom, false), 1.3f);
            general.addRow ("SCREEN FOLLOWS CH STRIP", OnKey::forSetting (ctx.settings, uiprefs::screenFollowsStrip, false), 1.3f);
            general.addRow ("CH STRIP TOUCH SELECT", OnKey::forSetting (ctx.settings, uiprefs::stripTouchSelect, true), 1.3f);
            addAndMakeVisible (general);

            faders.addRow ("FADER SPEED", choiceSetting (uiprefs::faderSpeed, { "SLOW", "MED", "FAST" }, 1), 3.65f);
            faders.addRow ("ALTERNATIVE SOF MODE", onSetting (uiprefs::alternativeSof, false), 1.3f);
            faders.addRow ("RIGHT SECTION SENDS ON FADER", onSetting (uiprefs::rightSectionSof, true), 1.3f);
            faders.addRow ("FULL FADER PAGING", onSetting (uiprefs::fullFaderPaging, false), 1.3f);
            faders.addRow ("CH AUTOSELECT", std::make_unique<ToggleKeys> (ctx, juce::StringArray { "L", "C", "R" },
                                                                         std::vector<const char*> (std::begin (uiprefs::autoSelect), std::end (uiprefs::autoSelect))),
                           3.6f);
            faders.addRow ("USER LAYER LINK", std::make_unique<ToggleKeys> (ctx, juce::StringArray { "L/C", "C/R" },
                                                                           std::vector<const char*> (std::begin (uiprefs::userLayerLink), std::end (uiprefs::userLayerLink))),
                           2.35f);
            faders.addRow ("DCA SPILL", onSetting (uiprefs::dcaSpill, false), 1.3f);
            faders.addRow ("FADER/SCREEN LAYER LINK", OnKey::forSetting (ctx.settings, prefs::layerLink, true), 1.3f);
            addAndMakeVisible (faders);

            sectionsKey.offColour = Dark::header;
            sectionsKey.iconAbove = true;
            sectionsKey.setTooltip ("Sidecar's fader sections: their widths, the shift step and the dB values under the faders");
            sectionsKey.onClick = [this] { sections.show (*this, "FADER SECTIONS", makeSectionsBody(), OverlayPlacement::centred (0.6f, 0.62f)); };
            addAndMakeVisible (sectionsKey);
        }

        void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black); }

        void resized() override
        {
            // [Fig. 40] three 416 px columns, 8 px apart.
            auto b = getLocalBounds();
            const int gap = setupGap(), w = (b.getWidth() - 2 * gap) / 3;
            int rowH = juce::roundToInt (wingPx (52.0f));
            for (auto* p : { &metering, &general, &faders })
                setupPanel (*p, rowH);
            // A small window: rows shrink until the tallest column fits.
            auto tallest = [this] { return juce::jmax (metering.preferredHeight(), general.preferredHeight(), faders.preferredHeight()); };
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
            const int hh = setupHeaderHeight();
            sectionsKey.setBounds (faders.getBounds().withHeight (hh).removeFromRight (hh * 2).reduced (2, 1));
            sections.layout (getLocalBounds());
        }

    private:
        /** FADER SECTIONS (Sidecar): the section widths, the shift step and FADER VALUES. */
        class SectionsBody final : public juce::Component
        {
        public:
            SectionsBody() { addAndMakeVisible (panel); }

            void resized() override
            {
                panel.rowHeight = juce::jlimit (14, juce::roundToInt (wingPx (52.0f)), getHeight() * 10 / 66);
                panel.wingRows = true;
                panel.setBounds (getLocalBounds());
            }

            SettingsPanel panel;
        };

        std::unique_ptr<juce::Component> makeSectionsBody()
        {
            auto body = std::make_unique<SectionsBody>();
            auto& p = body->panel;
            // The dB value under every fader (ui.faderValues).
            p.addRow ("FADER VALUES",
                      std::make_unique<OnKey> ([this] { return ctx.settings.getBoolValue ("ui.faderValues", false); },
                                               [this] (bool v)
                                               {
                                                   ctx.settings.setValue ("ui.faderValues", v);
                                                   notifyLayersChangedAsync (ctx);
                                               }),
                      1.3f);
            // Faders scrolled per press of the shift keys (ui.shiftStep).
            p.addRow ("SHIFT STEP",
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
                      3.6f);
            p.newBox();
            p.addCaption ("SECTION WIDTH", false);
            static constexpr int widths[] = { LayerSet::kAutoWidth, 4, 8, 12, 16, 24, 32 };
            const char* names[] = { "LEFT", "CENTER", "RIGHT" };
            for (int s = 0; s < LayerSet::kNumSections; ++s)
            {
                const auto section = static_cast<LayerSet::Section> (s);
                p.addRow (names[s],
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
            return body;
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
        WingKey sectionsKey { "SECTIONS", Icon::Faders };
        OverlayHolder sections;
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
                // [Fig. 42] light grey boxes with black numbers, 8 px apart.
                auto k = juce::Rectangle<float> (r.getX() + w * (float) i, r.getY(), w, r.getHeight()).reduced (juce::jmax (1.0f, wingPx (4.0f)), 0.0f);
                g.setColour (Dark::caption.withAlpha (isEnabled() ? 1.0f : 0.6f));
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

    /** WING REMOTE status: errors and the connected clients, two lines each
        (it fills the narrow column left of NETWORK). */
    class WingStatusView final : public juce::Component
    {
    public:
        WingRemote::Status status;

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Dark::body);
            auto r = getLocalBounds().reduced (juce::roundToInt (wingPx (8.0f)), juce::roundToInt (wingPx (6.0f)));
            const float fh = juce::jlimit (8.0f, screenCap (12.0f), wingPx (11.0f));
            const int lineH = juce::roundToInt (fh * 1.5f);
            auto line = [&] (const juce::String& text, juce::Colour colour)
            {
                if (r.getHeight() < lineH)
                    return;
                g.setColour (colour);
                g.setFont (wingFontPx (fh));
                g.drawFittedText (text, r.removeFromTop (lineH), juce::Justification::centredLeft, 1, 0.75f);
            };

            if (! status.running)
            {
                line ("WING REMOTE IS OFF", Dark::text);
                r.removeFromTop (lineH / 2);
                g.setColour (Dark::textDim);
                g.setFont (wingFontPx (fh * 0.85f));
                g.drawFittedText ("WING REMOTE APPS (OSC AND THE NATIVE PROTOCOL) CAN CONNECT TO SIDECAR. EXPERIMENTAL: TESTED WITH LIBWING AND THE "
                                  "COMPANION MODULE'S PATHS, NOT WITH WING EDIT OR WING COPILOT.",
                                  r, juce::Justification::topLeft, juce::jmax (1, r.getHeight() / juce::jmax (1, lineH - 2)));
                return;
            }
            for (auto& e : status.errors)
                line (e.toUpperCase(), Dark::muted);
            line (juce::String ((int) status.clients.size()) + (status.clients.size() == 1 ? " CLIENT" : " CLIENTS"), Dark::text);
            for (auto& c : status.clients)
            {
                line (juce::String (c.native ? "NATIVE" : "OSC") + (c.subscribed ? (c.native ? "  METERS" : "  SUBSCRIBED") : "") + "  " + juce::String (c.seconds) + " S",
                      Dark::caption);
                line (c.address, Dark::textDim);
            }
        }
    };

    //==============================================================================
    /** WEB REMOTE: the port and the PIN, each in a box (click to change). The
        port turns red if the server is on but could not listen on it. */
    class WebAccessRow final : public juce::Component
    {
    public:
        WebAccessRow (UiContext& c, std::function<void (const WebRemote::Settings&)> applyChange, std::function<bool()> failedCheck)
            : ctx (c), apply (std::move (applyChange)), failed (std::move (failedCheck))
        {
        }

        void paint (juce::Graphics& g) override
        {
            const auto s = WebRemote::Settings::load (ctx.settings);
            auto box = [&] (juce::Rectangle<int> area, const juce::String& text, bool red)
            {
                auto r = area.toFloat();
                g.setColour (red ? Dark::muted : juce::Colour (0xff5a5a5a));
                g.fillRect (r);
                g.setColour (juce::Colours::black);
                g.setFont (wingFontFit (TypeScale::body, r.getHeight() * 0.62f));
                g.drawFittedText (text, area.reduced (2, 0), juce::Justification::centred, 1, 0.8f);
            };
            box (portArea, "PORT " + juce::String (s.port), failed());
            box (pinArea, s.pin.isEmpty() ? juce::String ("NO PIN") : "PIN " + juce::String::repeatedString ("*", s.pin.length()), false);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (juce::roundToInt (wingPx (12.0f)), juce::roundToInt (wingPx (7.0f)));
            portArea = r.removeFromLeft (r.getWidth() / 2 - 2);
            r.removeFromLeft (4);
            pinArea = r;
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            const auto s = WebRemote::Settings::load (ctx.settings);
            if (portArea.contains (e.getPosition()))
                editText (this, "WEB REMOTE PORT", juce::String (s.port), [this] (const juce::String& text)
                {
                    const int v = text.getIntValue();
                    if (v <= 0 || v > 65535)
                        return;
                    auto changed = WebRemote::Settings::load (ctx.settings);
                    changed.port = v;
                    apply (changed);
                    repaint();
                });
            else if (pinArea.contains (e.getPosition()))
                editText (this, "WEB REMOTE PIN (4-8 DIGITS, EMPTY: NONE)", s.pin, [this] (const juce::String& text)
                {
                    if (! WebRemote::Settings::isValidPin (text))
                    {
                        ctx.log.warning ("Web remote: a PIN is 4 to 8 digits");
                        return;
                    }
                    auto changed = WebRemote::Settings::load (ctx.settings);
                    changed.pin = text;
                    apply (changed);
                    repaint();
                });
        }

    private:
        UiContext& ctx;
        std::function<void (const WebRemote::Settings&)> apply;
        std::function<bool()> failed;
        juce::Rectangle<int> portArea, pinArea;
    };

    /** WEB REMOTE status: the browsers watching (and waiting at the PIN) and
        the addresses to open, or the error (in the narrow column beside HA
        REMOTE). */
    class WebStatusView final : public juce::Component
    {
    public:
        WebRemote::Status status;
        juce::StringArray urls;

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Dark::body);
            auto r = getLocalBounds().reduced (juce::roundToInt (wingPx (8.0f)), juce::roundToInt (wingPx (6.0f)));
            const float fh = juce::jlimit (8.0f, screenCap (12.0f), wingPx (11.0f));
            const int lineH = juce::roundToInt (fh * 1.5f);
            auto line = [&] (const juce::String& text, juce::Colour colour, int lines = 1)
            {
                if (r.getHeight() < lineH)
                    return;
                g.setColour (colour);
                g.setFont (wingFontPx (fh));
                g.drawFittedText (text, r.removeFromTop (juce::jmin (r.getHeight(), lineH * lines)), juce::Justification::topLeft, lines, 0.75f);
            };

            line ("WEB REMOTE", Dark::caption);
            if (! status.running)
            {
                line ("OFF", Dark::text);
                r.removeFromTop (lineH / 2);
                g.setColour (Dark::textDim);
                g.setFont (wingFontPx (fh * 0.85f));
                g.drawFittedText ("BROWSERS ON THE NETWORK SHOW THIS SCREEN AND CONTROL IT.", r, juce::Justification::topLeft,
                                  juce::jmax (1, r.getHeight() / juce::jmax (1, lineH - 2)));
                return;
            }
            if (status.error.isNotEmpty())
            {
                line (status.error.toUpperCase(), Dark::muted, 4);
                return;
            }
            const int watching = status.numWatching(), waiting = (int) status.clients.size() - watching;
            line (juce::String (watching) + (watching == 1 ? " BROWSER" : " BROWSERS"), Dark::text);
            if (waiting > 0)
                line (juce::String (waiting) + " AT THE PIN", Dark::textDim);
            r.removeFromTop (lineH / 3);
            line ("OPEN", Dark::textDim);
            for (auto& url : urls)
                line (url.fromFirstOccurrenceOf ("://", false, false).trimCharactersAtEnd ("/"), Dark::caption);
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
    class MidiPortRow final : public juce::Component, private juce::Timer
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
            startTimerHz (2);
        }

        /** The captions also say whether the device is open and count its messages. */
        void timerCallback() override
        {
            if (ctx.midi == nullptr || ! isShowing())
                return;
            const auto st = ctx.midi->getPortStatus (port);
            fields[0]->setCaption (name + " IN" + (st.inputOpen ? "  OPEN  " + juce::String (st.received) + " RX" : juce::String()));
            fields[1]->setCaption (name + " OUT" + (st.outputOpen ? "  OPEN  " + juce::String (st.sent) + " TX" : juce::String()));
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

    /** The two-line column captions of HA REMOTE, over its pairs of ON keys. */
    class PairCaptions final : public juce::Component
    {
    public:
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds();
            g.setColour (Dark::caption);
            g.setFont (wingFontPx (juce::jmin (TypeScale::statusCaption * getWingScreenHeight(), (float) r.getHeight() * 0.3f)));
            for (auto* text : { "ENABLE\nHA REMOTE", "ENABLE\nCUST SYNC" })
                g.drawFittedText (text, r.removeFromLeft (getWidth() / 2), juce::Justification::centred, 2, 0.7f);
        }
    };

    /** REMOTE (fig. 42): MIDI REMOTE CONTROL | HA REMOTE | NETWORK + REMOTE LOCK.
        MIDI REMOTE CONTROL sets each function OFF, DIN or USB (ⓘ: its MIDI map); under it
        (Sidecar) the computer's devices for the WING's DIN and USB ports. HA remote is
        drawn disabled. WING REMOTE (Sidecar, under HA REMOTE, its console name and clients
        in the free column) runs the WING protocol servers (discovery, OSC, native) for
        WING remote apps; it starts with the app when enabled. WEB REMOTE (Sidecar, under
        NETWORK, which keeps only what this computer has: its address and name; its status
        and addresses in the free column) shows the screen in browsers on the network
        (ui::WebMirror). */
    class RemoteTab final : public juce::Component, private juce::Timer
    {
    public:
        explicit RemoteTab (UiContext& c) : ctx (c)
        {
            for (int f = 0; f < MidiRemote::kNumFunctions; ++f)
                midi.addRow (MidiRemote::functionName (f),
                             std::make_unique<ChoiceKeys> (juce::StringArray { "OFF", "DIN", "USB" }, [this, f] { return midiSettings (ctx).routes[(size_t) f]; },
                                                           [this, f] (int route) { changeMidi (ctx, [f, route] (MidiRemote::Settings& st) { st.routes[(size_t) f] = route; }); },
                                                           offFirst (3)),
                             3.2f, true, true);
            midi.addMouseListener (this, false); // a row's ⓘ shows its MIDI map
            addAndMakeVisible (midi);
            // DIN and USB are MIDI ports of this computer; USB is the virtual port
            // "Sidecar MIDI Control" where the system allows it.
            ports.addRow ({}, std::make_unique<MidiPortRow> (ctx, MidiRemote::Din, "DIN"), 0.0f);
            ports.addRow ({}, std::make_unique<MidiPortRow> (ctx, MidiRemote::Usb, "USB"), 0.0f);
            addAndMakeVisible (ports);

            // AES50 preamp remote control: hardware only.
            ha.addRow ({}, std::make_unique<PairCaptions>(), kPairRows, true, false, 0.9f);
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
                ha.addRow (port, std::move (pair), kPairRows, false);
            }
            addAndMakeVisible (ha);
            haClear.offColour = Dark::header;
            haClear.iconAbove = true;
            enableKey (haClear, false);
            addAndMakeVisible (haClear);

            // The computer's own network settings (read-only).
            const auto ip = juce::IPAddress::getLocalAddress().toString();
            juce::StringArray octets;
            octets.addTokens (ip, ".", {});
            while (octets.size() < 4)
                octets.add ("0");
            network.addCaption ("IP ADDRESS", false);
            network.addRow ({}, std::make_unique<OctetBoxes> (octets), 0.0f, false, false, 0.85f);
            network.addCaption ("DEVICE NAME: " + juce::SystemStats::getComputerName().toUpperCase(), false);
            addAndMakeVisible (network);

            // WEB REMOTE (Sidecar): the settings live in the app settings.
            auto changeWeb = [this] (const WebRemote::Settings& s)
            {
                s.save (ctx.settings);
                applyWebSettings();
            };
            web.addRow ("ENABLE",
                        std::make_unique<OnKey> ([this] { return WebRemote::Settings::load (ctx.settings).enabled; },
                                                 [this, changeWeb] (bool on)
                                                 {
                                                     auto s = WebRemote::Settings::load (ctx.settings);
                                                     s.enabled = on;
                                                     changeWeb (s);
                                                 }),
                        1.3f);
            web.addRow ({}, std::make_unique<WebAccessRow> (ctx, changeWeb, [this] { return webStatus.status.running && webStatus.status.port < 0; }),
                        0.0f);
            web.addRow ("VIEW ONLY",
                        std::make_unique<OnKey> ([this] { return WebRemote::Settings::load (ctx.settings).viewOnly; },
                                                 [this, changeWeb] (bool on)
                                                 {
                                                     auto s = WebRemote::Settings::load (ctx.settings);
                                                     s.viewOnly = on;
                                                     changeWeb (s);
                                                 }),
                        1.3f);
            addAndMakeVisible (web);
            addAndMakeVisible (webStatus);
            appliedWeb = WebRemote::Settings::load (ctx.settings); // the web remote started with the app
            refreshWebStatus();

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
                         1.3f);
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
                         3.0f);
            wing.addRow ("OSC (UDP)",
                         std::make_unique<ServerRow> (ctx, &S::osc, &S::oscPort, "OSC PORT (UDP)", change, failed (&S::osc, &WingRemote::Status::oscPort)),
                         3.0f);
            wing.addRow ("NATIVE (TCP)",
                         std::make_unique<ServerRow> (ctx, &S::native, &S::nativePort, "NATIVE PORT (TCP)", change,
                                                      failed (&S::native, &WingRemote::Status::nativePort)),
                         3.0f);
            addAndMakeVisible (wing);
            wingName = std::make_unique<DropdownField> ("CONSOLE NAME", [this] { return WingRemote::Settings::load (ctx.settings).name; },
                                                        [this, change] (DropdownField& f)
                                                        {
                                                            editText (&f, "CONSOLE NAME", WingRemote::Settings::load (ctx.settings).name,
                                                                      [this, change] (const juce::String& name)
                                                                      {
                                                                          auto s = WingRemote::Settings::load (ctx.settings);
                                                                          s.name = name.isNotEmpty() ? name.toUpperCase() : juce::String ("SIDECAR");
                                                                          change (s);
                                                                      });
                                                        });
            addAndMakeVisible (*wingName);
            addAndMakeVisible (wingStatus);

            // The SETUP page is built at start-up: start the servers if they were on.
            applySettings();
            startTimerHz (2);
        }

        void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black); }

        void resized() override
        {
            // [Fig. 42] MIDI REMOTE CONTROL 512 px, HA REMOTE 336, NETWORK 248
            // at the right edge; WING REMOTE (Sidecar) under HA REMOTE and its
            // console name and clients in the free column left of NETWORK; WEB
            // REMOTE (Sidecar) under NETWORK and its status in the free column.
            auto b = getLocalBounds();
            const int gap = setupGap();
            const float sx = (float) b.getWidth() / 1264.0f;
            auto x = [&] (float px) { return b.getX() + juce::roundToInt (px * sx); };
            int rowH = juce::roundToInt (wingPx (52.0f));
            for (auto* p : { &midi, &ports, &ha, &network, &web, &lock, &wing })
                setupPanel (*p, rowH);
            auto c1 = juce::Rectangle<int>::leftTopRightBottom (b.getX(), b.getY(), x (512.0f), b.getBottom());
            auto c2 = juce::Rectangle<int>::leftTopRightBottom (x (520.0f), b.getY(), x (856.0f), b.getBottom());
            auto c3 = juce::Rectangle<int>::leftTopRightBottom (x (864.0f), b.getY(), x (1008.0f), b.getBottom());
            auto c4 = juce::Rectangle<int>::leftTopRightBottom (x (1016.0f), b.getY(), b.getRight(), b.getBottom());
            // A small window: rows shrink until the columns fit.
            auto tallest = [&]
            {
                return juce::jmax (midi.preferredHeight() + gap + ports.preferredHeight(), ha.preferredHeight() + gap + wing.preferredHeight(),
                                   network.preferredHeight() + gap + web.preferredHeight() + gap + lock.preferredHeight());
            };
            ports.rowHeight = rowH * 44 / 52;
            while (rowH > 10 && tallest() > getHeight())
            {
                --rowH;
                for (auto* p : { &midi, &ha, &network, &web, &lock, &wing })
                    p->rowHeight = rowH;
                ports.rowHeight = rowH * 44 / 52;
            }
            midi.setBounds (c1.removeFromTop (juce::jmin (c1.getHeight(), midi.preferredHeight())));
            c1.removeFromTop (gap);
            ports.setBounds (c1.removeFromTop (juce::jmin (c1.getHeight(), ports.preferredHeight())));
            info.layout (getLocalBounds());
            ha.setBounds (c2.removeFromTop (juce::jmin (c2.getHeight(), ha.preferredHeight())));
            const int hh = setupHeaderHeight();
            haClear.setBounds (ha.getBounds().withHeight (hh).removeFromRight (hh + 16).reduced (2, 1));
            c2.removeFromTop (gap);
            wing.setBounds (c2.removeFromTop (juce::jmin (c2.getHeight(), wing.preferredHeight())));
            webStatus.setBounds (c3.withBottom (ha.getBottom()));
            c3 = c3.withTop (wing.getY());
            wingName->setBounds (c3.removeFromTop (juce::roundToInt (wingPx (64.0f))));
            c3.removeFromTop (gap);
            wingStatus.setBounds (c3.withBottom (juce::jmax (wing.getBottom(), ports.getBottom())));
            network.setBounds (c4.removeFromTop (network.preferredHeight()));
            c4.removeFromTop (gap);
            web.setBounds (c4.removeFromTop (juce::jmin (c4.getHeight() - lock.preferredHeight() - gap, web.preferredHeight())));
            c4.removeFromTop (gap);
            lock.setBounds (c4.withHeight (juce::jmin (c4.getHeight(), lock.preferredHeight())));
            for (auto& p : pairLayouts)
            {
                auto r = p.parent->getLocalBounds();
                const int kh = juce::roundToInt ((float) rowH * 38.0f / 52.0f), kw = juce::roundToInt ((float) rowH * 46.0f / 52.0f);
                p.a->setBounds (r.removeFromLeft (r.getWidth() / 2).withSizeKeepingCentre (kw, kh));
                p.b->setBounds (r.withSizeKeepingCentre (kw, kh));
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

        /** Starts, restarts or stops the web remote to match the settings. */
        void applyWebSettings()
        {
            const auto s = WebRemote::Settings::load (ctx.settings);
            appliedWeb = s;
            if (ctx.web == nullptr)
                return;
            const auto error = ctx.web->apply (s);
            if (error.isNotEmpty())
                ctx.log.error ("Web remote: " + error);
            else if (s.enabled)
                ctx.log.info ("Web remote on port " + juce::String (s.port) + (s.pin.isNotEmpty() ? " (with a PIN)" : "")
                              + (s.viewOnly ? ", view only" : ""));
            refreshWebStatus();
        }

        void refreshWebStatus()
        {
            if (ctx.web == nullptr)
                return;
            webStatus.status = ctx.web->getStatus();
            webStatus.urls = WebRemote::getUrls (webStatus.status.port);
            webStatus.repaint();
            web.repaint(); // the port's listening state
        }

        /** A touch on a MIDI REMOTE CONTROL row's name (ⓘ): its MIDI map. */
        void mouseUp (const juce::MouseEvent& e) override
        {
            if (e.eventComponent != &midi || midi.getNumChildComponents() == 0)
                return;
            const auto pos = e.getEventRelativeTo (&midi).getPosition();
            const int top = darkPanelBody (midi.getLocalBounds(), true, midi.headerHeight, midi.headerGap).getY() + 4;
            // Rows of keys are 56 px of the 800 px screen to the panel's 52.
            const int row = (pos.y - top) / juce::jmax (1, juce::roundToInt ((float) midi.rowHeight * 56.0f / 52.0f));
            if (pos.y < top || ! juce::isPositiveAndBelow (row, (int) MidiRemote::kNumFunctions) || pos.x >= midi.getChildComponent (0)->getX())
                return;
            info.show (*this, MidiRemote::functionName (row), std::make_unique<MidiInfoBody> (MidiRemote::functionInfo (row)));
        }

        void timerCallback() override
        {
            if (WingRemote::Settings::load (ctx.settings) != applied)
                applySettings();
            if (WebRemote::Settings::load (ctx.settings) != appliedWeb)
                applyWebSettings();
            if (isShowing())
            {
                refreshWebStatus();
                wingStatus.status = ctx.engine.getWing().getStatus();
                wingStatus.repaint();
                wing.repaint(); // the ports' listening state
            }
        }

        UiContext& ctx;
        /** HA REMOTE's pairs of ON keys: 160 px of the 800 px screen. */
        static constexpr float kPairRows = 160.0f / 52.0f;

        SettingsPanel midi { "MIDI REMOTE CONTROL" }, ha { "HA REMOTE" }, network { "NETWORK" }, lock { "REMOTE LOCK" }, wing { "WING REMOTE" };
        SettingsPanel web { "WEB REMOTE" };
        WebStatusView webStatus;
        WebRemote::Settings appliedWeb;
        SettingsPanel ports; // MIDI PORTS
        WingKey haClear { "CLEAR", Icon::ClearCircle };
        std::unique_ptr<DropdownField> wingName;
        OverlayHolder info;
        WingStatusView wingStatus;
        WingRemote::Settings applied;
        juce::OwnedArray<juce::Component> pairs;
        std::vector<Pair> pairLayouts;
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

            // [Fig. 43] the drawing on its own #202020 box.
            auto r = getLocalBounds().toFloat();
            g.setColour (Dark::body);
            g.fillRect (r);
            r = r.reduced (wingPx (12.0f));
            g.setColour (Colours::screenTile);
            g.fillRect (r);
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
                                                                    std::vector<juce::Colour> { Dark::usbGreen, Dark::usbGreen }),
                        2.1f);
            daw.addRow ("EMULATION", std::make_unique<ChoiceKeys> (juce::StringArray { "MCU", "HUI" }, [get] { return get().emulation == DawRemote::Emulation::Hui ? 1 : 0; },
                                                                   [set] (int i) { set ([i] (DawRemote::Settings& s) { s.emulation = i == 1 ? DawRemote::Emulation::Hui : DawRemote::Emulation::Mcu; }); }),
                        2.1f);
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
                        0.0f, true, false, kFieldRows);
            daw.addRow ({}, std::make_unique<PresetRow> (ctx), 0.0f, true, false, kFieldRows);
            addAndMakeVisible (daw);
            addAndMakeVisible (surface);

            for (int unit = 0; unit < DawRemote::kMaxUnits; ++unit)
                ports.addRow ({}, std::make_unique<MidiPortRow> (ctx, MidiRemote::Daw1 + unit, "MIDI DAW " + juce::String (unit + 1)), 0.0f, true, false, kFieldRows);
            addAndMakeVisible (ports);
            startTimerHz (2);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colours::black);
            // [Fig. 43] the header; the boxes under it draw themselves.
            auto header = panel.withHeight (setupHeaderHeight());
            g.setColour (Dark::header);
            g.fillRect (header);
            g.setColour (Dark::caption);
            g.setFont (panelTitleFont (header.getHeight()));
            g.drawText ("DAW CONTROL", header, juce::Justification::centred);
            g.setColour (Dark::textDim.brighter (0.3f));
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
            // [Fig. 43] DAW CONTROL 760 px wide: the settings in its left half,
            // the 376 x 262 drawing at the right; the disclaimer at the bottom.
            auto b = getLocalBounds();
            const int gap = setupGap();
            disclaimer = b.removeFromBottom (juce::roundToInt (wingPx (64.0f))).withTrimmedLeft (juce::roundToInt (wingPx (4.0f)));
            auto area = b.withWidth (juce::roundToInt ((float) b.getWidth() * 760.0f / 1264.0f));
            setupPanel (daw, juce::roundToInt (wingPx (52.0f)));
            setupPanel (ports, daw.rowHeight);
            while (daw.rowHeight > 10 && daw.preferredHeight() + setupHeaderHeight() + gap > area.getHeight())
                --daw.rowHeight;
            panel = area.withHeight (juce::jmin (area.getHeight(), daw.preferredHeight() + setupHeaderHeight() + gap));
            auto body = darkPanelBody (panel, true, setupHeaderHeight(), gap);
            auto right = body.removeFromRight ((body.getWidth() - gap) / 2);
            body.removeFromRight (gap);
            daw.setBounds (body);
            surface.setBounds (right.withHeight (juce::jmin (right.getHeight(), right.getWidth() * 262 / 376)));
            auto side = b.withTrimmedLeft (panel.getRight() + gap - b.getX());
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
    /** PLUGINS (app-specific): plugin scanning (the plugins become FX slot
        devices), RUN PLUGINS IN SANDBOX (experimental: each plugin loaded afterwards
        runs in a helper process, SandboxedPlugin), and DELAY COMPENSATION: the
        setting, the total compensation and each loaded plugin's latency. */
    class PluginsTab final : public juce::Component, private juce::ChangeListener, private juce::Timer
    {
    public:
        explicit PluginsTab (UiContext& c) : ctx (c)
        {
            scan.onClick = [this] { if (ctx.showPluginScanner) ctx.showPluginScanner(); };
            styleIconKey (scan);
            addAndMakeVisible (scan);
            ctx.host.getKnownPlugins().addChangeListener (this);

            // An app setting (MainComponent applies it at start-up).
            compensation.addRow ("DELAY COMPENSATION",
                                 std::make_unique<OnKey> ([this] { return ctx.engine.isLatencyCompensationOn(); },
                                                          [this] (bool on)
                                                          {
                                                              ctx.engine.setLatencyCompensation (on);
                                                              ctx.engine.updateLatencyCompensation();
                                                              ctx.settings.setValue (uiprefs::latencyCompensation, on);
                                                              repaint();
                                                          }),
                                 1.3f);
            // Saved by PluginHost; applies to plugins loaded from now on.
            compensation.addRow ("RUN PLUGINS IN SANDBOX (EXPERIMENTAL)",
                                 std::make_unique<OnKey> ([this] { return ctx.host.isSandboxEnabled(); },
                                                          [this] (bool on)
                                                          {
                                                              ctx.host.setSandboxEnabled (on);
                                                              ctx.log.info (on ? "Plugin sandbox on (experimental): plugins loaded from now on run in a helper process"
                                                                               : "Plugin sandbox off: plugins loaded from now on run inside Sidecar");
                                                              repaint();
                                                          }),
                                 1.3f);
            addAndMakeVisible (compensation);
            startTimerHz (4);
        }

        ~PluginsTab() override { ctx.host.getKnownPlugins().removeChangeListener (this); }

        void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colours::black);
            auto body = drawDarkPanel (g, box, "PLUGINS");
            auto r = body.reduced (12, 10);
            r.removeFromBottom (scan.getHeight() + 6);
            const auto types = ctx.host.getSortedTypes();
            g.setColour (Dark::text);
            auto title = r.removeFromTop (juce::jmin (r.getHeight() / 3, screenCap (34)));
            const auto known = juce::String (types.size()) + " PLUGINS KNOWN";
            g.setFont (fontFittingWidth (wingFontFit (TypeScale::statusTitle, (float) title.getHeight(), WingWeight::SemiBold), known, (float) title.getWidth()));
            g.drawText (known, title, juce::Justification::centredLeft);
            g.setColour (Dark::textDim);
            g.setFont (wingFont (TypeScale::body));
            juce::String text;
            text << "VST3, AU, LV2" << (ctx.host.getFormatManager().getNumFormats() > 3 ? ", VST2" : "")
                 << ". Load them into FX slots on the EFFECTS screen (device EXT), then assign the slots to channel inserts. "
                 << "Plugins are scanned in a separate process, so a crashing plugin can't take the console down. "
                 << "With RUN PLUGINS IN SANDBOX (below), each plugin loaded from then on also runs in its own process: if it "
                 << "crashes or hangs, only its FX slot is bypassed. That adds one audio block of latency (compensated).";
            g.drawFittedText (text, r, juce::Justification::topLeft, juce::jmax (1, r.getHeight() / juce::jmax (1, (int) g.getCurrentFont().getHeight())), 0.85f);

            paintCompensation (g);
        }

        void resized() override
        {
            auto b = getLocalBounds();
            const int w = juce::jmin (b.getWidth(), screenCap (720));
            box = b.removeFromTop (juce::jmin (b.getHeight() * 45 / 100, screenCap (240))).withWidth (w);
            scan.setBounds (darkPanelBody (box, true).reduced (12, 10).removeFromBottom (juce::jmin (44, box.getHeight() / 4)).removeFromLeft (juce::jmin (260, w / 2)));
            b.removeFromTop (8);
            auto lower = b.withWidth (w);
            compensation.rowHeight = fitRowHeight (lower.getHeight(), 6.0f);
            compensation.setBounds (lower.removeFromTop (juce::jmin (lower.getHeight() / 3, compensation.preferredHeight())));
            lower.removeFromTop (4);
            latencyList = lower;
        }

    private:
        /** The total compensation, the limit warning and the latency of every loaded plugin. */
        void paintCompensation (juce::Graphics& g)
        {
            auto& engine = ctx.engine;
            auto& fx = engine.getFx();
            auto body = drawDarkPanel (g, latencyList, "PLUGIN LATENCY").reduced (12, 6);
            const int rowH = fitRowHeight (body.getHeight(), (float) kNumFx / 2.0f + 2.0f);
            const double rate = juce::jmax (1.0, engine.getSampleRate());
            auto samples = [rate] (int n) { return juce::String (n) + " SMP (" + juce::String (1000.0 * n / rate, 1) + " MS)"; };

            auto total = body.removeFromTop (rowH);
            const auto totalText = engine.isLatencyCompensationOn() ? "TOTAL COMPENSATION: " + samples (engine.getTotalCompensation())
                                                                    : juce::String ("DELAY COMPENSATION OFF");
            g.setColour (Dark::text);
            g.setFont (fontFittingWidth (wingFontFit (TypeScale::paramLabel, (float) rowH * 0.8f, WingWeight::SemiBold), totalText, (float) total.getWidth()));
            g.drawFittedText (totalText, total, juce::Justification::centredLeft, 1, 0.8f);
            if (engine.isLatencyCompensationOn() && engine.isCompensationLimited())
            {
                auto warn = body.removeFromTop (rowH);
                g.setColour (Dark::pre);
                const juce::String limit = "A PATH NEEDS MORE THAN " + juce::String (MixerEngine::kMaxCompensation) + " SMP: IT STAYS LATE BY THE REST";
                g.setFont (fontFittingWidth (wingFontFit (TypeScale::body, (float) rowH * 0.8f), limit, (float) warn.getWidth()));
                g.drawFittedText (limit, warn, juce::Justification::centredLeft, 1, 0.8f);
            }
            body.removeFromTop (4);

            // Two columns of the slots holding a plugin.
            juce::Array<int> loaded;
            for (int slot = 0; slot < kNumFx; ++slot)
                if (fx.getPlugin (slot) != nullptr)
                    loaded.add (slot);
            g.setFont (wingFontFit (TypeScale::body, (float) rowH * 0.75f));
            if (loaded.isEmpty())
            {
                g.setColour (Dark::textDim);
                g.drawFittedText ("NO PLUGIN LOADED", body.removeFromTop (rowH), juce::Justification::centredLeft, 1, 0.8f);
                return;
            }
            const int colW = body.getWidth() / 2;
            const int perColumn = juce::jmax (1, body.getHeight() / rowH);
            for (int i = 0; i < loaded.size() && i < perColumn * 2; ++i)
            {
                const int slot = loaded[i];
                auto row = juce::Rectangle<int> (body.getX() + (i / perColumn) * colW, body.getY() + (i % perColumn) * rowH, colW - 8, rowH);
                const int latency = fx.getLatency (slot);
                auto value = row.removeFromRight (juce::jmin (row.getWidth() / 2, screenCap (150)));
                g.setColour (latency > MixerEngine::kMaxCompensation ? Dark::pre : (latency > 0 ? Dark::text : Dark::textDim));
                g.drawFittedText (samples (latency), value, juce::Justification::centredRight, 1, 0.75f);
                g.setColour (Dark::text);
                g.drawFittedText (fx.describe (slot).toUpperCase() + (fx.isSandboxed (slot) ? " (SANDBOX)" : ""), row.withTrimmedRight (6),
                                  juce::Justification::centredLeft, 1, 0.75f);
            }
        }

        void timerCallback() override
        {
            if (! isShowing())
                return;
            // Repaint only when what the panel shows changes.
            auto& fx = ctx.engine.getFx();
            juce::int64 hash = ctx.engine.getTotalCompensation() * 4 + (ctx.engine.isCompensationLimited() ? 1 : 0) + (ctx.engine.isLatencyCompensationOn() ? 2 : 0);
            for (int slot = 0; slot < kNumFx; ++slot)
                hash = hash * 31 + fx.getLatency (slot) + (fx.getPlugin (slot) != nullptr ? 7 : 0);
            if (hash != lastHash)
            {
                lastHash = hash;
                repaint();
            }
        }

        UiContext& ctx;
        WingKey scan { "SCAN FOR PLUGINS", Icon::Folder };
        SettingsPanel compensation;
        juce::Rectangle<int> box, latencyList;
        juce::int64 lastHash = -1;
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
    initKey.onClick = [this] { openInitialize(); };
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
    {
        closeSubScreen();
        initWindow.reset();
    }
}

void SetupPage::openInitialize()
{
    // INITIALIZE CONSOLE: what to reset, chosen like a scope.
    auto window = std::make_unique<ScopeWindow> (ctx, ScopeWindow::Mode::Initialize, "INITIALIZE CONSOLE", state::Scope::none());
    auto* w = window.get();
    window->onCancel = [this] { closeInitialize(); };
    window->onSave = [this, w] (const state::Scope& chosen, bool)
    {
        const auto sources = w->getSourceGroups();
        const bool everything = chosen.allStrips && chosen.sections == state::AllSections && chosen.dcas == state::Scope::kAllDcas
                                && chosen.muteGroups == state::Scope::kAllMuteGroups && chosen.fx == state::Scope::kAllFx;
        if (! everything && chosen.strips.empty() && ! chosen.allStrips && chosen.dcas == 0 && chosen.muteGroups == 0 && chosen.fx == 0 && sources == 0)
            return; // nothing chosen
        popups::confirm (*this, "INITIALIZE CONSOLE",
                         everything ? "Reset the whole console to a fresh 40-channel console? GLOBAL safes are overwritten too. Scenes are kept."
                                    : "Reset the chosen parts of the console? GLOBAL safes are overwritten too. Scenes are kept.",
                         "INIT", [this, chosen, sources, everything]
                         {
                             initialize (chosen, sources, everything);
                             closeInitialize();
                         },
                         true);
    };
    initWindow = std::move (window);
    addAndMakeVisible (*initWindow);
    initWindow->setBounds (getLocalBounds());
}

void SetupPage::closeInitialize()
{
    // Called from the window's own keys: delete it once the click is done.
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<SetupPage> (this)]
    {
        if (safe != nullptr)
            safe->initWindow.reset();
    });
}

void SetupPage::initialize (const state::Scope& chosen, juce::uint32 sources, bool everything)
{
    if (everything)
    {
        // Everything: a fresh 40-channel console, as before the parts could be chosen.
        for (int i = 0; i < kNumFx; ++i)
            clearFx (ctx, i);
        ctx.engine.reset();
        ctx.engine.setNumChannels (kWingChannels);
    }
    const int strips = everything ? 0 : initializeParts (ctx, chosen);
    // The source groups: the audio interface's inputs (LCL), the playback
    // channels (CRD), the USB player (PLAY) and the oscillators (OSC).
    int sourceCount = 0;
    for (int i = 0; i < ctx.engine.getNumSources(); ++i)
    {
        const int port = MixerEngine::sourcePort (i);
        const auto group = Recorder::isPlaybackSource (port)    ? ScopeWindow::SourceCard
                           : UsbRecorder::isPlayerSource (port) ? ScopeWindow::SourcePlayer
                           : MixerEngine::isOscSource (port)    ? ScopeWindow::SourceOsc
                                                                : ScopeWindow::SourceLocal;
        if ((sources & (juce::uint32) group) != 0)
        {
            ctx.engine.initializeSource (i);
            ++sourceCount;
        }
    }
    ctx.log.info (everything ? juce::String ("Console initialized")
                             : "Console initialized: " + juce::String (strips) + " strip(s), " + juce::String (sourceCount) + " source(s)");
    if (ctx.structureChanged)
        ctx.structureChanged();
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
    // [Fig. 33] the tab bar: a #202020 band 64 px tall, 8 px in from the sides.
    if (subScreen == nullptr)
    {
        g.setColour (Dark::header);
        g.fillRect (tabBar);
    }
}

void SetupPage::resized()
{
    // [Fig. 33] in px of the 1280 x 800 screen: the tab bar (56 px tabs,
    // INITIALIZE CONSOLE and SHUTDOWN 168 x 56 at its right), then the tab's
    // page 8 px under it and 8 px in from the sides.
    auto b = getLocalBounds();
    const int gap = setupGap();
    auto bar = b.removeFromTop (juce::jmax (30, juce::roundToInt (wingPx (64.0f)))).reduced (gap, 0);
    tabBar = bar;
    b = b.reduced (gap, 0).withTrimmedTop (gap).withTrimmedBottom (gap);
    auto row = bar.withHeight (bar.getHeight() * 56 / 64);
    const float sx = (float) bar.getWidth() / 1264.0f;
    const int keyW = juce::roundToInt (168.0f * sx);
    // Narrow screens: the icon over a one-word caption.
    const bool narrow = keyW < 80;
    initKey.iconAbove = shutdownKey.iconAbove = narrow;
    initKey.setButtonText (narrow ? "INITIALIZE" : "INITIALIZE\nCONSOLE");
    auto keys = bar.reduced (0, juce::roundToInt ((float) bar.getHeight() / 16.0f)).withTrimmedRight (juce::roundToInt (4.0f * sx));
    shutdownKey.setBounds (keys.removeFromRight (keyW));
    keys.removeFromRight (juce::roundToInt (12.0f * sx));
    initKey.setBounds (keys.removeFromRight (keyW));
    // The console's 158 px tabs where all seven fit, else as wide as they can be.
    const int space = initKey.getX() - row.getX() - juce::roundToInt (8.0f * sx);
    const int tabPx = juce::jmin (juce::roundToInt (160.0f * sx), (space - 10) / 7);
    tabRow.tabFraction = (float) tabPx / (float) juce::jmax (1, 7 * tabPx + 10);
    tabRow.setBounds (row.withWidth (7 * tabPx + 10));
    for (auto* p : pages)
        p->setBounds (b);
    if (subScreen != nullptr)
        subScreen->setBounds (getLocalBounds());
    if (initWindow != nullptr)
        initWindow->setBounds (getLocalBounds());
    overlays.layout (getLocalBounds());
}

} // namespace livedaw::ui
