#include "keyboard/keyboard.hpp"
#include "page_app/ui_app_st.hpp"
#include "st7789_lvgl.hpp"
#include "status_display.hpp"

#include <poll.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <system_error>

namespace {
volatile sig_atomic_t stopping = 0;
void stop(int signal) { stopping = signal; }

class RawInput {
public:
    RawInput() {
        if (tcgetattr(STDIN_FILENO, &saved_) < 0)
            throw std::system_error(errno, std::generic_category(), "stdin must be an interactive TTY");
        auto raw = saved_;
        cfmakeraw(&raw);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0)
            throw std::system_error(errno, std::generic_category(), "set raw input");
    }
    ~RawInput() {
        while (tcsetattr(STDIN_FILENO, TCSANOW, &saved_) < 0 && errno == EINTR) {}
    }
private:
    termios saved_{};
};

struct Lvgl {
    Lvgl() { lv_init(); }
    ~Lvgl() { lv_deinit(); }
};

uint32_t ticks() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void install_signals() {
    struct sigaction action{};
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    for (int sig : {SIGHUP, SIGINT, SIGTERM, SIGQUIT}) {
        if (sigaction(sig, &action, nullptr) < 0)
            throw std::system_error(errno, std::generic_category(), "sigaction");
    }
}

using Font = std::unique_ptr<lv_font_t, decltype(&lv_freetype_font_delete)>;
Font load_font(const char* path) {
    return Font(lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                       UISTPage::font_pixel_size, LV_FREETYPE_FONT_STYLE_NORMAL),
                lv_freetype_font_delete);
}

Font optional_font(const char* path) {
    if (access(path, R_OK) == 0) return load_font(path);
    return Font(nullptr, lv_freetype_font_delete);
}

int run(const char* panel_config, bool wide, const char* keyboard_path, const char* status_path) {
    install_signals();
    std::unique_ptr<Keyboard> keyboard;
    std::unique_ptr<RawInput> raw;
    if (keyboard_path) keyboard = std::make_unique<Keyboard>(keyboard_path);
    else raw = std::make_unique<RawInput>();
    Lvgl lvgl;
    lv_tick_set_cb(ticks);
    auto primary = load_font("fonts/JetBrainsMono-Bold.ttf");
    if (!primary) throw std::runtime_error("cannot load fonts/JetBrainsMono-Bold.ttf");
    auto cjk = optional_font("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
    auto symbols = optional_font("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf");
    if (cjk) cjk->fallback = symbols.get();
    primary->fallback = cjk ? cjk.get() : symbols.get();

    std::unique_ptr<lv_display_t, decltype(&lv_display_delete)>
        display(create_st7789_display(panel_config), lv_display_delete);
    if (!display) return 1;
    auto* screen = lv_display_get_screen_active(display.get());
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    UISTPage page(screen, primary.get(), wide);
    std::unique_ptr<StatusDisplay> status;
    if (status_path) {
        try { status = std::make_unique<StatusDisplay>(status_path); }
        catch (const std::exception& error) {
            std::fprintf(stderr, "Built-in LCD: %s\n", error.what());
        }
        lv_display_set_default(display.get());
    }
    bool prefix = false;
    for (;;) {
        const int delay = std::min<uint32_t>(lv_timer_handler(), 20);
        if (st7789_display_failed(display.get())) return 1;
        if (stopping) return 128 + stopping;
        if (page.finished()) return page.exit_status();
        pollfd fd{keyboard ? keyboard->fd() : STDIN_FILENO, POLLIN, 0};
        if (poll(&fd, 1, delay) < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "poll input");
        }
        if (keyboard) {
            if (fd.revents & (POLLHUP | POLLERR | POLLNVAL))
                throw std::runtime_error("keyboard device disconnected");
            keyboard->dispatch(page);
            continue;
        }
        if (!(fd.revents & POLLIN)) {
            if (fd.revents & (POLLHUP | POLLERR | POLLNVAL)) return 1;
            continue;
        }
        char bytes[4096];
        const ssize_t length = read(STDIN_FILENO, bytes, sizeof bytes);
        if (length < 0 && errno == EINTR) continue;
        if (length <= 0) return 1;
        for (ssize_t i = 0; i < length;) {
            const char key = bytes[i];
            if (prefix) {
                prefix = false;
                ++i;
                switch (key) {
                case 'q': return 0;
                case 'h': page.scroll(-1, 0); break;
                case 'j': page.scroll(0, 1); break;
                case 'k': page.scroll(0, -1); break;
                case 'l': page.scroll(1, 0); break;
                case 0x1d: page.input(&key, 1); break;
                default: {
                    const char pair[] = {0x1d, key};
                    page.input(pair, sizeof pair);
                    break;
                }
                }
            } else if (key == 0x1d) {
                prefix = true;
                ++i;
            } else {
                const auto* next = static_cast<const char*>(std::memchr(bytes + i, 0x1d, length - i));
                const ssize_t end = next ? next - bytes : length;
                page.input(bytes + i, end - i);
                i = end;
            }
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    constexpr const char* usage = "Usage: cardputer-cli [--wide] [--keyboard[=/dev/input/eventN]] [--status[=/dev/fbN]] panel.conf\n";
    bool wide = false;
    const char* keyboard_path = nullptr;
    const char* status_path = nullptr;
    const char* panel_config = nullptr;
    for (int n = 1; n < argc; ++n) {
        const char* arg = argv[n];
        if (std::strcmp(arg, "--help") == 0) {
            std::fputs(usage, stdout);
            return 0;
        }
        if (std::strcmp(arg, "--wide") == 0) wide = true;
        else if (std::strcmp(arg, "--keyboard") == 0) keyboard_path = "";
        else if (std::strncmp(arg, "--keyboard=", 11) == 0 && arg[11]) keyboard_path = arg + 11;
        else if (std::strcmp(arg, "--status") == 0) status_path = "";
        else if (std::strncmp(arg, "--status=", 9) == 0 && arg[9]) status_path = arg + 9;
        else if (!panel_config && arg[0] != '-') panel_config = arg;
        else {
            std::fputs(usage, stderr);
            return 2;
        }
    }
    if (!panel_config) {
        std::fputs(usage, stderr);
        return 2;
    }
    try {
        return run(panel_config, wide, keyboard_path, status_path);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "cardputer-cli: %s\n", error.what());
        return 1;
    }
}
