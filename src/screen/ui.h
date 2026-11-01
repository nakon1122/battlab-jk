#pragma once
#ifndef LGFX_USE_V1
#define LGFX_USE_V1
#endif
#include <LovyanGFX.hpp>
#include "model.h"

// Draw the whole page into `g`. `oy` is the screen row that maps to g's row 0,
// so the device can render the page in horizontal bands through a small sprite
// (no flicker, little RAM). The PC preview renders with oy = 0 into a
// full-size sprite.
void uiDraw(lgfx::LovyanGFX &g, int W, int H, int oy, const Model &m, const UiState &u);
// AP / first-run screen
void uiDrawSetupAp(lgfx::LovyanGFX &g, int W, int H, int oy, const Model &m);
// Handle a tap at screen x,y. Changes `u` and returns what the caller must do.
Action uiTouch(int W, int H, int x, int y, const Model &m, UiState &u);
// Swipe left/right on the single-printer page
void uiSwipe(int dir, const Model &m, UiState &u);
// Build the banner text for a new event
void uiMakeToast(const Event &e, UiState &u);
uint16_t uiBg();
