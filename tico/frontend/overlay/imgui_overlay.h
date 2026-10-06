// Copyright 2026 Azahar Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "overlay/overlay_ui.h"
#include "imgui.h"

#include <functional>
#include <string>

struct ImDrawData;
class IOverlayHost;

namespace SwitchFrontend::ImGuiOverlay {

// Characters the fonts need besides the overlay's own strings (e.g. the
// game's title); call before Init, which bakes the fonts.
void SetExtraGlyphText(std::string text);

// Creates the overlay's own ImGui context (the current one is left current)
// and its fonts, rasterized at font_scale times their 720p size, and uploads
// the font atlas, avatar and selection border through the host.
bool Init(IOverlayHost* host, float font_scale = 1.0f);
void Shutdown();

// Shows or hides the quick menu (the HUD and toasts are drawn either way).
void SetVisible(bool visible);
bool IsVisible();

// Edge-triggered menu navigation for the next built frame.
void FeedNav(const OverlayUI::NavInput& nav);
// The touchscreen's current state, read while the menu is open.
void FeedTouch(const OverlayUI::TouchInput& touch);

// Draws under everything in the frame being built (the game, when the
// renderer shows it through ImGui); null to draw nothing there.
void SetBackgroundDraw(std::function<void(ImDrawList* list, ImVec2 size)> draw);

// Builds this frame's overlay for a width x height surface. Returns the draw
// data to composite over the game.
ImDrawData* BuildFrame(float width, float height, float delta_time);

// The action the menu returned while building, once; None when there was none.
OverlayUI::Action ConsumeAction();

} // namespace SwitchFrontend::ImGuiOverlay
