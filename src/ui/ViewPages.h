#pragma once

#include "ui/DisplayPage.h"

namespace livedaw::ui
{

/** The VIEW screens (manual 5.7-5.11), opened by the fader sections' VIEW keys
    and by each other's status-bar keys: OVERVIEW (every strip's blocks),
    FADERS, SENDS, CONFIG (custom fader layers) and MUTE GROUPS. */
class ViewPage : public DisplayPage
{
public:
    ViewPage (UiContext&, Screen);

    Screen getScreen() const noexcept { return screen; }

    bool usesKnobs() const override { return false; }
    std::vector<StatusKey> getStatusKeys() override;
    void paint (juce::Graphics&) override;

protected:
    UiContext& ctx;
    const Screen screen;
};

/** OVERVIEW: the processing blocks of every strip. */
class OverviewViewPage final : public ViewPage
{
public:
    explicit OverviewViewPage (UiContext& c) : ViewPage (c, Screen::Overview) {}
};

/** FADERS: all faders of a group of strips. */
class FadersViewPage final : public ViewPage
{
public:
    explicit FadersViewPage (UiContext& c) : ViewPage (c, Screen::Faders) {}
};

/** SENDS: the selected strip's sends as faders. */
class SendsViewPage final : public ViewPage
{
public:
    explicit SendsViewPage (UiContext& c) : ViewPage (c, Screen::Sends) {}
};

/** CONFIG: the custom fader layers (USER layers). */
class ConfigViewPage final : public ViewPage
{
public:
    explicit ConfigViewPage (UiContext& c) : ViewPage (c, Screen::Config) {}
};

/** MUTE GROUPS: the eight mute groups and their members. */
class MuteGroupsViewPage final : public ViewPage
{
public:
    explicit MuteGroupsViewPage (UiContext& c) : ViewPage (c, Screen::MuteGroups) {}
};

/** "OVERVIEW", "FADERS", ... */
const char* viewScreenName (Screen);

} // namespace livedaw::ui
