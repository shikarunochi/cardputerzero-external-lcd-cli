#pragma once

#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {
constexpr int width = 320, height = 240;
constexpr int spi_chunk = 4096;

[[noreturn]] void fail_system(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

class Fd {
public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() { if (fd_ >= 0) close(fd_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
private:
    int fd_;
};

int open_device(const std::string& path) {
    const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), path);
    return fd;
}

struct InitStep {
    enum Kind { command, delay, reset } kind;
    unsigned value;
    std::vector<uint8_t> bytes;
};

struct PanelConfig {
    std::string spi = "/dev/spidev0.1";
    std::string chip = "/dev/gpiochip0";
    unsigned dc = 22, rst = 26, speed = 0, mode = 4;
    unsigned x = 0, y = 0;
    bool has_offset = false;
    enum class Ext5vMode { cycle, automatic, off };
    Ext5vMode ext5v = Ext5vMode::automatic;
    unsigned ext5v_delay_ms = 150;
    std::vector<InitStep> init;
};

unsigned number(const std::string& token, unsigned max, int base = 10) {
    unsigned value = 0;
    const char* end = token.data() + token.size();
    const auto result = std::from_chars(token.data(), end, value, base);
    if (result.ec != std::errc() || result.ptr != end || value > max)
        throw std::runtime_error("invalid numeric argument: " + token);
    return value;
}

PanelConfig read_config(const char* path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error(std::string("cannot open config: ") + path);
    PanelConfig cfg;
    bool has_command = false;
    std::string line;
    unsigned line_no = 0;
    while (std::getline(file, line)) {
        ++line_no;
        const auto comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        std::istringstream input(line);
        std::string op, extra;
        if (!(input >> op)) continue;
        const auto arg = [&] {
            std::string token;
            if (!(input >> token)) throw std::runtime_error("missing argument");
            return token;
        };
        try {
            if (op == "spi") cfg.spi = arg();
            else if (op == "gpiochip") cfg.chip = arg();
            else if (op == "dc") cfg.dc = number(arg(), 65535);
            else if (op == "rst") cfg.rst = number(arg(), 65535);
            else if (op == "speed_hz") cfg.speed = number(arg(), 100000000);
            else if (op == "mode") cfg.mode = number(arg(), 3);
            else if (op == "ext5v") {
                const auto value = arg();
                if (value == "cycle") cfg.ext5v = PanelConfig::Ext5vMode::cycle;
                else if (value == "auto") cfg.ext5v = PanelConfig::Ext5vMode::automatic;
                else if (value == "off") cfg.ext5v = PanelConfig::Ext5vMode::off;
                else throw std::runtime_error("ext5v must be cycle, auto or off");
            } else if (op == "ext5v_delay_ms") {
                cfg.ext5v_delay_ms = number(arg(), 60000);
            }
            else if (op == "offset") {
                cfg.x = number(arg(), 65536 - width);
                cfg.y = number(arg(), 65536 - height);
                cfg.has_offset = true;
            } else if (op == "delay_ms") {
                cfg.init.push_back({InitStep::delay, number(arg(), 60000), {}});
            } else if (op == "rst_level") {
                cfg.init.push_back({InitStep::reset, number(arg(), 1), {}});
            } else if (op == "cmd") {
                InitStep step{InitStep::command, number(arg(), 255, 16), {}};
                std::string byte;
                while (input >> byte) step.bytes.push_back(number(byte, 255, 16));
                cfg.init.push_back(std::move(step));
                has_command = true;
            } else throw std::runtime_error("unknown directive: " + op);
            if (input >> extra) throw std::runtime_error("unexpected argument: " + extra);
        } catch (const std::runtime_error& error) {
            throw std::runtime_error(std::string(path) + ":" + std::to_string(line_no)
                                     + ": " + error.what());
        }
    }
    if (file.bad()) throw std::runtime_error("config read failed");
    if (!cfg.speed || cfg.mode > 3 || !cfg.has_offset || !has_command || cfg.dc == cfg.rst)
        throw std::runtime_error("configure speed_hz, mode, offset, at least one cmd, and distinct DC/RST");
    return cfg;
}

constexpr const char* ext5v_brightness = "/sys/class/leds/ext_5v_out/brightness";

ssize_t write_ext5v(int fd, char value) {
    ssize_t count;
    do { count = write(fd, &value, 1); } while (count < 0 && errno == EINTR);
    return count;
}

int enable_ext5v(const PanelConfig& cfg, bool& restore_off) {
    if (cfg.ext5v == PanelConfig::Ext5vMode::off) return -1;

    const bool cycle = cfg.ext5v == PanelConfig::Ext5vMode::cycle;
    int current = 0;
    const bool known = !cycle && static_cast<bool>(std::ifstream(ext5v_brightness) >> current);
    if (known && current > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg.ext5v_delay_ms));
        return -1;
    }
    const int fd = open(ext5v_brightness, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        const Fd rail(fd);
        const auto count = write_ext5v(rail.get(), '1');
        if (count < 0) fail_system(ext5v_brightness);
        if (count != 1) throw std::runtime_error("short write enabling Ext5V");
        restore_off = cycle || (known && current == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg.ext5v_delay_ms));
        return -1;
    }
    if (errno != ENOENT) fail_system(ext5v_brightness);

    // The launcher uses chip 1, line 12, active high on identified CardputerZero boards.
    if (access("/proc/cardputerzero_hw_id", F_OK) != 0)
        throw std::runtime_error("Ext5V sysfs node absent; CardputerZero GPIO map could not be identified");
    const Fd chip(open_device("/dev/gpiochip1"));
    gpio_v2_line_request request{};
    request.num_lines = 1;
    request.offsets[0] = 12;
    request.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    request.config.num_attrs = 1;
    request.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    request.config.attrs[0].attr.values = 1;
    request.config.attrs[0].mask = 1;
    std::strcpy(request.consumer, "cardputer-cli-ext5v");
    if (ioctl(chip.get(), GPIO_V2_GET_LINE_IOCTL, &request) < 0)
        fail_system("Ext5V /dev/gpiochip1 line 12");
    if (fcntl(request.fd, F_SETFD, FD_CLOEXEC) < 0) {
        const Fd cleanup(request.fd);
        fail_system("Ext5V GPIO close-on-exec");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg.ext5v_delay_ms));
    return request.fd;
}

class Ext5v {
public:
    explicit Ext5v(const PanelConfig& cfg) : gpio_(enable_ext5v(cfg, restore_off_)) {}

    ~Ext5v() {
        if (gpio_.get() >= 0) {
            gpio_v2_line_values levels{};
            levels.mask = 1;
            int result;
            do { result = ioctl(gpio_.get(), GPIO_V2_LINE_SET_VALUES_IOCTL, &levels); }
            while (result < 0 && errno == EINTR);
            if (result < 0) std::perror("disable Ext5V GPIO");
        } else if (restore_off_) {
            const Fd led(open(ext5v_brightness, O_WRONLY | O_CLOEXEC));
            if (led.get() < 0) {
                std::perror("open Ext5V to restore OFF");
            } else {
                const auto count = write_ext5v(led.get(), '0');
                if (count < 0) std::perror("restore Ext5V OFF");
                else if (count != 1) std::fputs("short write restoring Ext5V OFF\n", stderr);
            }
        }
    }

private:
    bool restore_off_ = false;
    Fd gpio_;
};

int request_gpio(const PanelConfig& cfg) {
    Fd chip(open_device(cfg.chip));
    gpio_v2_line_request request{};
    request.num_lines = 2;
    request.offsets[0] = cfg.dc;
    request.offsets[1] = cfg.rst;
    request.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    request.config.num_attrs = 1;
    auto& initial = request.config.attrs[0];
    initial.attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    initial.attr.values = 0b10; // DC low, RST high.
    initial.mask = 0b11;
    std::strcpy(request.consumer, "cardputer-cli");
    if (ioctl(chip.get(), GPIO_V2_GET_LINE_IOCTL, &request) < 0)
        fail_system("GPIO DC/RST request");
    if (fcntl(request.fd, F_SETFD, FD_CLOEXEC) < 0) {
        const Fd cleanup(request.fd);
        fail_system("GPIO close-on-exec");
    }
    return request.fd;
}

class Panel {
public:
    explicit Panel(const PanelConfig& cfg)
        : ext5v_(cfg), spi_(open_device(cfg.spi)), gpio_(request_gpio(cfg)),
          x_(cfg.x), y_(cfg.y) {
        uint8_t mode = cfg.mode, bits = 8;
        uint32_t speed = cfg.speed;
        if (ioctl(spi_.get(), SPI_IOC_WR_MODE, &mode) < 0 ||
            ioctl(spi_.get(), SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
            ioctl(spi_.get(), SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0)
            fail_system("configure SPI");
        for (const auto& step : cfg.init) {
            switch (step.kind) {
            case InitStep::command:
                command(step.value, step.bytes.data(), step.bytes.size());
                break;
            case InitStep::reset: pins(false, step.value); break;
            case InitStep::delay:
                std::this_thread::sleep_for(std::chrono::milliseconds(step.value));
                break;
            }
        }
    }

    void draw(int x, int y, int w, int h, const uint8_t* pixels) {
        window(0x2a, x_ + x, x_ + x + w - 1);
        window(0x2b, y_ + y, y_ + y + h - 1);
        command(0x2c, pixels, w * h * 2);
    }

private:
    void pins(bool dc, bool rst) {
        gpio_v2_line_values levels{};
        levels.mask = 0b11;
        levels.bits = unsigned(dc) | (unsigned(rst) << 1);
        if (ioctl(gpio_.get(), GPIO_V2_LINE_SET_VALUES_IOCTL, &levels) < 0)
            fail_system("set DC/RST");
        rst_ = rst;
    }

    void transfer(const uint8_t* data, size_t size) {
        while (size) {
            const size_t count = std::min<size_t>(size, spi_chunk);
            ssize_t sent;
            do { sent = write(spi_.get(), data, count); }
            while (sent < 0 && errno == EINTR);
            if (sent < 0) fail_system("SPI write");
            if (static_cast<size_t>(sent) != count)
                throw std::runtime_error("short SPI write");
            data += count;
            size -= count;
        }
    }

    void command(uint8_t byte, const uint8_t* data, size_t size) {
        pins(false, rst_);
        transfer(&byte, 1);
        if (size) {
            pins(true, rst_);
            transfer(data, size);
        }
    }

    void window(uint8_t op, unsigned first, unsigned last) {
        const uint8_t args[] = {uint8_t(first >> 8), uint8_t(first),
                                uint8_t(last >> 8), uint8_t(last)};
        command(op, args, sizeof args);
    }

    Ext5v ext5v_;
    Fd spi_, gpio_;
    unsigned x_, y_;
    bool rst_ = true;
};

} // namespace
