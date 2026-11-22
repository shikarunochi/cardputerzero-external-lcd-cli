#include "status_display.hpp"

#include <lvgl.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/fb.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr int width = 320, height = 170;
constexpr uint32_t background = 0x0D1117, panel = 0x171F29;
constexpr uint32_t muted = 0xA8B8CC, text = 0xF0F6FC, accent = 0x72E4BD;

std::string framebuffer(const char* requested) {
    if (requested && *requested) return requested;
    for (const char* var : {"APPLAUNCH_LINUX_FBDEV_DEVICE", "LV_LINUX_FBDEV_DEVICE"}) {
        if (const char* value = std::getenv(var); value && *value) return value;
    }
    if (access("/dev/fb_lcd", F_OK) == 0) return "/dev/fb_lcd";
    std::ifstream list("/proc/fb");
    int id;
    std::string name;
    while (list >> id && std::getline(list, name)) {
        if (name.find("st7789") != std::string::npos) return "/dev/fb" + std::to_string(id);
    }
    return "/dev/fb0";
}

lv_display_t* open_status_display(const char* requested) {
    const auto path = framebuffer(requested);
    const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("cannot open " + path);
    fb_var_screeninfo info{};
    const int result = ioctl(fd, FBIOGET_VSCREENINFO, &info);
    close(fd);
    if (result < 0 || info.xres != width || info.yres != height)
        throw std::runtime_error(path + " is not a 320x170 framebuffer");
    auto* display = lv_linux_fbdev_create();
    if (!display) throw std::runtime_error("LVGL framebuffer creation failed");
    if (lv_linux_fbdev_set_file(display, path.c_str()) != LV_RESULT_OK) {
        lv_display_delete(display);
        throw std::runtime_error("cannot initialize " + path);
    }
    return display;
}

lv_obj_t* box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color, int radius = 0) {
    auto* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

lv_obj_t* label(lv_obj_t* parent, int x, int y, int w, const lv_font_t* font, uint32_t color) {
    auto* obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, w);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
    return obj;
}

struct CpuTotals { uint64_t total, idle; };
std::optional<CpuTotals> cpu_totals() {
    std::ifstream stat("/proc/stat");
    std::string tag;
    uint64_t user, nice, system, idle, wait, irq, softirq, steal;
    if (!(stat >> tag >> user >> nice >> system >> idle >> wait >> irq >> softirq >> steal) ||
        tag != "cpu") return {};
    return CpuTotals{user + nice + system + idle + wait + irq + softirq + steal, idle + wait};
}

std::string memory() {
    std::ifstream file("/proc/meminfo");
    std::string name;
    while (file >> name) {
        if (name == "MemAvailable:") {
            unsigned long kib;
            if (file >> kib) return "RAM " + std::to_string(kib / 1024) + " MiB";
            break;
        }
        std::getline(file, name);
    }
    return "RAM --";
}

std::string storage() {
    struct statvfs fs{};
    if (statvfs("/", &fs) != 0) return "Disk --";
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "Disk %.1f GiB",
                  static_cast<double>(fs.f_bavail) * fs.f_frsize / 1073741824.0);
    return buffer;
}

struct Battery { int percent = -1; bool charging = false; };
Battery battery() {
    std::error_code ec;
    const fs::directory_iterator end;
    for (auto it = fs::directory_iterator("/sys/class/power_supply", ec); !ec && it != end;
         it.increment(ec)) {
        const auto path = it->path();
        std::string type;
        if (!(std::ifstream(path / "type") >> type) || type != "Battery") continue;
        int present, percent;
        if (std::ifstream(path / "present") >> present && !present) continue;
        if (!(std::ifstream(path / "capacity") >> percent) || percent < 0 || percent > 100) continue;
        std::string state;
        std::ifstream(path / "status") >> state;
        return {percent, state == "Charging"};
    }
    return {};
}

struct Network { std::string wifi, lan; };
Network addresses() {
    Network out;
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) return out;
    const std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> list(interfaces, freeifaddrs);
    for (auto* item = interfaces; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
            !(item->ifa_flags & IFF_UP) || item->ifa_flags & IFF_LOOPBACK) continue;
        const std::string name(item->ifa_name);
        std::error_code ec;
        const bool wireless = name.compare(0, 2, "wl") == 0 ||
                              fs::exists(fs::path("/sys/class/net") / name / "wireless", ec);
        const bool wired = name.compare(0, 3, "eth") == 0 || name.compare(0, 2, "en") == 0;
        if (!wireless && !wired) continue;
        auto& address = wireless ? out.wifi : out.lan;
        if (!address.empty()) continue;
        char ip[INET_ADDRSTRLEN];
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
        if (inet_ntop(AF_INET, &ipv4->sin_addr, ip, sizeof ip)) address = ip;
    }
    return out;
}
} // namespace

struct StatusDisplay::Dashboard {
    explicit Dashboard(const char* path) : display(open_status_display(path), lv_display_delete) {
        auto* root = lv_display_get_screen_active(display.get());
        lv_obj_remove_style_all(root);
        lv_obj_set_style_bg_color(root, lv_color_hex(background), 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
        lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

        date = label(root, 12, 10, 120, &lv_font_montserrat_12, muted);
        time = label(root, 10, 26, 124, &lv_font_montserrat_32, text);
        box(root, 146, 11, 162, 53, panel, 7);
        battery_text = label(root, 158, 17, 140, &lv_font_montserrat_14, text);
        box(root, 158, 42, 135, 12, muted, 3);
        box(root, 160, 44, 131, 8, panel, 2);
        box(root, 293, 45, 3, 6, muted, 1);
        charge = box(root, 160, 44, 1, 8, accent, 2);
        charge_glint = box(root, 160, 44, 1, 8, 0xD5FFF0, 2);

        box(root, 12, 70, 296, 52, panel, 7);
        cpu = label(root, 20, 76, 62, &lv_font_montserrat_12, accent);
        lv_label_set_text(label(root, 20, 96, 68, &lv_font_montserrat_12, muted), "60 sec");
        box(root, 91, 96, 207, 1, 0x324252);
        graph = lv_line_create(root);
        lv_obj_set_pos(graph, 91, 78);
        lv_obj_set_style_line_color(graph, lv_color_hex(accent), 0);
        lv_obj_set_style_line_width(graph, 2, 0);
        lv_obj_add_flag(graph, LV_OBJ_FLAG_HIDDEN);

        ram = label(root, 12, 126, 148, &lv_font_montserrat_12, text);
        disk = label(root, 164, 126, 144, &lv_font_montserrat_12, text);
        wifi = label(root, 12, 149, 150, &lv_font_montserrat_12, accent);
        lan = label(root, 164, 149, 149, &lv_font_montserrat_12, accent);
        sample();
        animate();
        timer = lv_timer_create(callback, 250, this);
        if (!timer) throw std::runtime_error("cannot start the status timer");
    }

    ~Dashboard() { lv_timer_delete(timer); }

    static void callback(lv_timer_t* timer) noexcept {
        auto& self = *static_cast<Dashboard*>(lv_timer_get_user_data(timer));
        try {
            if (Clock::now() >= self.next_sample) self.sample();
            self.animate();
        } catch (...) {
            std::fputs("Built-in LCD: status refresh failed\n", stderr);
            lv_timer_pause(timer);
        }
    }

    void sample() {
        const auto sampled_at = Clock::now();
        next_sample += 1s;
        if (next_sample <= sampled_at) next_sample = sampled_at + 1s;
        const auto now = std::time(nullptr);
        std::tm local{};
        if (localtime_r(&now, &local)) {
            char buffer[40];
            std::strftime(buffer, sizeof buffer, "%Y-%m-%d", &local);
            lv_label_set_text(date, buffer);
            std::strftime(buffer, sizeof buffer, "%H:%M", &local);
            lv_label_set_text(time, buffer);
        }
        power = battery();
        if (power.percent < 0) lv_label_set_text(battery_text, "Battery --");
        else lv_label_set_text_fmt(battery_text, "Battery %d%%%s", power.percent,
                                   power.charging ? "  +" : "");
        lv_label_set_text(ram, memory().c_str());
        lv_label_set_text(disk, storage().c_str());
        const auto network = addresses();
        lv_label_set_text(wifi, network.wifi.empty() ? "Wi-Fi --" : ("Wi-Fi " + network.wifi).c_str());
        lv_label_set_text(lan, network.lan.empty() ? "LAN --" : ("LAN " + network.lan).c_str());

        const auto sample = cpu_totals();
        if (sample && previous && sample->total > previous->total && sample->idle >= previous->idle) {
            const auto total = sample->total - previous->total;
            const auto idle = std::min(total, sample->idle - previous->idle);
            const int load = static_cast<int>(100 * (total - idle) / total);
            std::rotate(history.begin(), history.begin() + 1, history.end());
            history.back() = load;
            count = std::min(count + 1, history.size());
            lv_label_set_text_fmt(cpu, "CPU %d%%", load);
            if (count > 1) {
                lv_obj_invalidate(graph);
                const size_t start = history.size() - count;
                for (size_t n = start; n < history.size(); ++n) {
                    points[n - start].x = 205.0f * n / (history.size() - 1);
                    points[n - start].y = 33.0f * (100 - history[n]) / 100;
                }
                lv_line_set_points(graph, points.data(), count);
                lv_obj_clear_flag(graph, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_label_set_text(cpu, "CPU --");
            if (previous) {
                count = 0;
                lv_obj_add_flag(graph, LV_OBJ_FLAG_HIDDEN);
            }
        }
        previous = sample;
    }

    void animate() {
        const int pixels = power.percent * 131 / 100;
        if (pixels <= 0) {
            lv_obj_add_flag(charge, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_width(charge, pixels);
            lv_obj_clear_flag(charge, LV_OBJ_FLAG_HIDDEN);
        }
        if (!power.charging || (pixels <= 0 && phase % 2)) {
            lv_obj_add_flag(charge_glint, LV_OBJ_FLAG_HIDDEN);
        } else {
            const int glint = std::clamp(pixels, 1, 12);
            lv_obj_set_width(charge_glint, glint);
            lv_obj_set_x(charge_glint, 160 + std::max(0, pixels - glint) * phase / 7);
            lv_obj_clear_flag(charge_glint, LV_OBJ_FLAG_HIDDEN);
        }
        phase = power.charging ? (phase + 1) % 8 : 0;
    }

    std::array<int, 60> history{};
    std::array<lv_point_precise_t, 60> points{};
    std::unique_ptr<lv_display_t, decltype(&lv_display_delete)> display;
    lv_obj_t *date, *time, *battery_text, *charge, *charge_glint, *cpu, *graph, *ram, *disk, *wifi, *lan;
    lv_timer_t* timer = nullptr;
    Clock::time_point next_sample;
    std::optional<CpuTotals> previous;
    Battery power;
    size_t count = 0;
    int phase = 0;
};

StatusDisplay::StatusDisplay(const char* device) : dashboard_(std::make_unique<Dashboard>(device)) {}
StatusDisplay::~StatusDisplay() = default;
