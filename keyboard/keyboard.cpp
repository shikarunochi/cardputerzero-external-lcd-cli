#include "keyboard.hpp"

#include "cp0_keyboard_keymap.h"
#include "keyboard_input.h"
#include "st_key_encoder.hpp"
#include "../page_app/ui_app_st.hpp"

#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr unsigned evdev_xkb_offset = 8;
constexpr auto repeat_delay = 400ms;
constexpr auto repeat_interval = 45ms;

std::string keyboard_path(const char* path) {
    if (path && *path) return path;
    for (const char* variable : {"APPLAUNCH_LINUX_KEYBOARD_DEVICE", "LV_LINUX_KEYBOARD_DEVICE"}) {
        if (const char* configured = std::getenv(variable); configured && *configured)
            return configured;
    }
    constexpr auto stable_path = "/dev/input/by-path/platform-3f804000.i2c-event";
    if (access(stable_path, F_OK) == 0 || errno != ENOENT) return stable_path;

    std::error_code error;
    const std::filesystem::directory_iterator end;
    for (auto it = std::filesystem::directory_iterator("/sys/class/input", error);
         !error && it != end; it.increment(error)) {
        const auto node = it->path().filename().string();
        if (node.compare(0, 5, "event") != 0) continue;
        std::string name;
        std::getline(std::ifstream(it->path() / "device/name"), name);
        if (name.find("tca8418") != std::string::npos) return "/dev/input/" + node;
    }
    throw std::runtime_error("CardputerZero keyboard was not found; use --keyboard=/dev/input/eventN");
}

struct InputFd {
    explicit InputFd(const std::string& path) : value(open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC)) {
        if (value < 0) throw std::system_error(errno, std::generic_category(), path);
    }
    ~InputFd() { close(value); }
    const int value;
};

const char* compose_locale() {
    for (const char* variable : {"LC_ALL", "LC_CTYPE", "LANG"}) {
        if (const char* locale = std::getenv(variable); locale && *locale) return locale;
    }
    return "C.UTF-8";
}

int ascii_control(unsigned char ch) {
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 1;
    if (ch >= '@' && ch <= '_') return ch & 0x1f;
    if (ch == ' ') return 0;
    if (ch == '?') return 127;
    return -1;
}
} // namespace

struct Keyboard::Device {
    explicit Device(const std::string& path) : fd(path) {
        libevdev* opened = nullptr;
        const int result = libevdev_new_from_fd(fd.value, &opened);
        if (result < 0) throw std::system_error(-result, std::generic_category(), path);
        evdev.reset(opened);
        if (!libevdev_has_event_code(evdev.get(), EV_KEY, KEY_A) ||
            !libevdev_has_event_code(evdev.get(), EV_KEY, KEY_ENTER))
            throw std::runtime_error(path + " is not a keyboard");

        context.reset(xkb_context_new(XKB_CONTEXT_NO_FLAGS));
        if (!context) throw std::runtime_error("cannot allocate XKB context");
        xkb_rule_names names{};
        names.rules = "evdev";
        names.layout = "us";
        keymap.reset(xkb_keymap_new_from_names(context.get(), &names, XKB_KEYMAP_COMPILE_NO_FLAGS));
        if (!keymap) throw std::runtime_error("cannot load US keyboard layout (xkb-data)");
        compose_table.reset(xkb_compose_table_new_from_locale(
            context.get(), compose_locale(), XKB_COMPOSE_COMPILE_NO_FLAGS));
        if (compose_table)
            compose.reset(xkb_compose_state_new(compose_table.get(), XKB_COMPOSE_STATE_NO_FLAGS));
        cp0_keyboard_keymap_load();
        resync();
    }

    void resync() {
        state.reset(xkb_state_new(keymap.get()));
        if (!state) throw std::runtime_error("cannot allocate XKB state");
        for (unsigned code : {KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_LEFTCTRL, KEY_RIGHTCTRL,
                              KEY_LEFTALT, KEY_RIGHTALT, KEY_LEFTMETA, KEY_RIGHTMETA}) {
            if (libevdev_get_event_value(evdev.get(), EV_KEY, code))
                xkb_state_update_key(state.get(), code + evdev_xkb_offset, XKB_KEY_DOWN);
        }
        if (compose) xkb_compose_state_reset(compose.get());
        repeating = 0;
    }

    bool modifier(const char* name) const {
        return xkb_state_mod_name_is_active(state.get(), name, XKB_STATE_MODS_EFFECTIVE) > 0;
    }

    bool emit(UISTPage& page, unsigned code, int event_state) {
        if (!STKeyEncoder::should_forward_event(code, event_state)) return false;
        const bool ctrl = modifier(XKB_MOD_NAME_CTRL);
        const bool alt = modifier(XKB_MOD_NAME_ALT);
        const bool shift = modifier(XKB_MOD_NAME_SHIFT);
        const auto mods = (ctrl ? KBD_MOD_CTRL : 0u) | (alt ? KBD_MOD_ALT : 0u);
        if (const int direction = STKeyEncoder::scrollback_direction(code, mods, shift)) {
            page.scroll(0, -direction);
            return true;
        }
        if (page.wide_mode()) {
            switch (code) {
            case KEY_F4: page.scroll(-1, 0); return true;
            case KEY_F5: page.scroll(0, -1); return true;
            case KEY_F7: page.scroll(0, 1); return true;
            case KEY_F8: page.scroll(1, 0); return true;
            }
        }

        const auto key = code + evdev_xkb_offset;
        const char* control = cp0_keyboard_control_utf8(code);
        std::array<char, 16> text{};
        if (control) {
            if (code == KEY_TAB && shift) control = "\033[Z";
        } else if (const auto* mapped = cp0_keyboard_keymap_lookup(code)) {
            if (ctrl && mapped->utf8[0] && !mapped->utf8[1]) {
                const int byte = ascii_control(mapped->utf8[0]);
                if (byte >= 0) {
                    text[0] = static_cast<char>(byte);
                    page.input(text.data(), 1);
                    return true;
                }
            }
            control = mapped->utf8;
        } else {
            const auto symbol = xkb_state_key_get_one_sym(state.get(), key);
            if (ctrl) {
                const auto codepoint = xkb_keysym_to_utf32(symbol);
                const int byte = codepoint <= 127 ? ascii_control(codepoint) : -1;
                if (byte >= 0) {
                    text[0] = static_cast<char>(byte);
                    page.input(text.data(), 1);
                    return true;
                }
            }
            if (compose && !ctrl && event_state == KBD_KEY_PRESSED) {
                xkb_compose_state_feed(compose.get(), symbol);
                switch (xkb_compose_state_get_status(compose.get())) {
                case XKB_COMPOSE_COMPOSING: return false;
                case XKB_COMPOSE_COMPOSED:
                    xkb_compose_state_get_utf8(compose.get(), text.data(), text.size());
                    xkb_compose_state_reset(compose.get());
                    break;
                case XKB_COMPOSE_CANCELLED:
                    xkb_compose_state_reset(compose.get());
                    return false;
                case XKB_COMPOSE_NOTHING: break;
                }
            }
            if (ctrl && code == KEY_SPACE) {
                page.input(text.data(), 1);
                return true;
            }
            if (!text[0]) xkb_state_key_get_utf8(state.get(), key, text.data(), text.size());
            control = text.data();
        }

        std::string bytes = STKeyEncoder::encode(code, control, page.application_cursor_mode());
        if (bytes.empty()) return false;
        if (alt && bytes.front() != '\x1b') bytes.insert(bytes.begin(), '\x1b');
        page.input(bytes.data(), bytes.size());
        return true;
    }

    void handle(UISTPage& page, const input_event& ev) {
        // Use our own repeat because the TCA8418 does not always advertise EV_REP.
        if (ev.type != EV_KEY || (ev.value != 0 && ev.value != 1)) return;
        const auto code = ev.code;
        if (!cp0_keyboard_keymap_lookup(code))
            xkb_state_update_key(state.get(), code + evdev_xkb_offset,
                                 ev.value ? XKB_KEY_DOWN : XKB_KEY_UP);
        if (!ev.value) {
            if (code == repeating) repeating = 0;
            return;
        }
        if (!emit(page, code, KBD_KEY_PRESSED)) return;
        repeating = 0;
        if (code != KEY_ESC && !modifier(XKB_MOD_NAME_CTRL) &&
            STKeyEncoder::should_forward_event(code, KBD_KEY_REPEATED) &&
            (cp0_keyboard_keymap_lookup(code) ||
             xkb_keymap_key_repeats(keymap.get(), code + evdev_xkb_offset))) {
            repeating = code;
            repeat_at = Clock::now() + repeat_delay;
        }
    }

    void dispatch(UISTPage& page) {
        for (int count = 0; count < 64; ++count) {
            input_event event{};
            const int flag = syncing ? LIBEVDEV_READ_FLAG_SYNC : LIBEVDEV_READ_FLAG_NORMAL;
            const int result = libevdev_next_event(evdev.get(), flag, &event);
            if (result == LIBEVDEV_READ_STATUS_SYNC) {
                syncing = true;
                repeating = 0;
            } else if (result == -EAGAIN) {
                if (!syncing) break;
                syncing = false;
                resync();
            } else if (result == -EINTR) {
                continue;
            } else if (result < 0) {
                throw std::system_error(-result, std::generic_category(), "read keyboard");
            } else {
                handle(page, event);
            }
        }
        if (repeating && Clock::now() >= repeat_at) {
            if (modifier(XKB_MOD_NAME_CTRL)) repeating = 0;
            else {
                emit(page, repeating, KBD_KEY_REPEATED);
                repeat_at = Clock::now() + repeat_interval;
            }
        }
    }

    InputFd fd;
    std::unique_ptr<libevdev, decltype(&libevdev_free)> evdev{nullptr, libevdev_free};
    std::unique_ptr<xkb_context, decltype(&xkb_context_unref)> context{nullptr, xkb_context_unref};
    std::unique_ptr<xkb_keymap, decltype(&xkb_keymap_unref)> keymap{nullptr, xkb_keymap_unref};
    std::unique_ptr<xkb_state, decltype(&xkb_state_unref)> state{nullptr, xkb_state_unref};
    std::unique_ptr<xkb_compose_table, decltype(&xkb_compose_table_unref)>
        compose_table{nullptr, xkb_compose_table_unref};
    std::unique_ptr<xkb_compose_state, decltype(&xkb_compose_state_unref)>
        compose{nullptr, xkb_compose_state_unref};
    unsigned repeating = 0;
    Clock::time_point repeat_at{};
    bool syncing = false;
};

Keyboard::Keyboard(const char* path) : device_(std::make_unique<Device>(keyboard_path(path))) {}
Keyboard::~Keyboard() = default;
int Keyboard::fd() const { return device_->fd.value; }
void Keyboard::dispatch(UISTPage& page) { device_->dispatch(page); }
