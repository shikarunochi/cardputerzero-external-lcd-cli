#pragma once

#include <lvgl.h>

// Call after lv_init(). On failure logs the reason and returns nullptr.
// The caller owns the returned display and releases it with lv_display_delete().
lv_display_t* create_st7789_display(const char* panel_config);
bool st7789_display_failed(lv_display_t* display);
