#include "st7789_lvgl.hpp"
#include "st7789_panel.hpp"

#include <array>
#include <cstdio>
#include <memory>

namespace {
struct DisplayContext {
    explicit DisplayContext(const char* path) : panel(read_config(path)) {}

    Panel panel;
    // Allow padding on each row as required by the LVGL configuration.
    alignas(LV_DRAW_BUF_ALIGN) std::array<uint8_t, (width * 2 + LV_DRAW_BUF_STRIDE_ALIGN) * 16> draw{};
    std::array<uint8_t, spi_chunk> wire{};
    bool failed = false;
};

void destroy(lv_event_t* event) {
    delete static_cast<DisplayContext*>(lv_event_get_user_data(event));
}

void flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    auto& ctx = *static_cast<DisplayContext*>(lv_display_get_driver_data(display));
    try {
        if (!ctx.failed) {
            if (area->x1 < 0 || area->y1 < 0 || area->x2 >= width || area->y2 >= height ||
                area->x2 < area->x1 || area->y2 < area->y1)
                throw std::runtime_error("flush rectangle outside the 320x240 panel");

            const int columns = lv_area_get_width(area);
            const int rows = lv_area_get_height(area);
            const uint32_t stride = lv_draw_buf_width_to_stride(columns, LV_COLOR_FORMAT_RGB565);
            const int rows_per_transfer = spi_chunk / (columns * 2);
            for (int top = 0; top < rows; top += rows_per_transfer) {
                const int count = std::min(rows - top, rows_per_transfer);
                auto* out = ctx.wire.data();
                for (int y = 0; y < count; ++y) {
                    const auto* row = pixels + (top + y) * stride;
                    for (int x = 0; x < columns; ++x) {
                        uint16_t pixel;
                        std::memcpy(&pixel, row + x * 2, sizeof pixel);
                        *out++ = pixel >> 8;
                        *out++ = pixel & 0xff;
                    }
                }
                ctx.panel.draw(area->x1, area->y1 + top, columns, count, ctx.wire.data());
            }
        }
    } catch (const std::exception& error) {
        ctx.failed = true;
        std::fprintf(stderr, "ST7789 display stopped: %s\n", error.what());
    }
    lv_display_flush_ready(display);
}
} // namespace

lv_display_t* create_st7789_display(const char* panel_config) {
    try {
        if (!panel_config || !*panel_config) throw std::invalid_argument("panel config path required");
        auto ctx = std::make_unique<DisplayContext>(panel_config);
        auto* display = lv_display_create(width, height);
        if (!display) throw std::runtime_error("lv_display_create failed");
        lv_display_add_event_cb(display, destroy, LV_EVENT_DELETE, ctx.get());
        lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(display, ctx->draw.data(), nullptr, ctx->draw.size(),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_driver_data(display, ctx.release());
        lv_display_set_flush_cb(display, flush);
        return display;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ST7789 display initialization: %s\n", error.what());
        return nullptr;
    }
}

bool st7789_display_failed(lv_display_t* display) {
    return static_cast<DisplayContext*>(lv_display_get_driver_data(display))->failed;
}
