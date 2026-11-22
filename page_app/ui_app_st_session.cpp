/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_st.hpp"
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pty.h>
#include <sys/wait.h>
#include <unistd.h>

bool UISTPage::pty_open(const std::string& command, const std::list<std::string>& args)
{
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(command.c_str()));
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    winsize size{};
    size.ws_col = term_cols_;
    size.ws_row = term_rows_;
    size.ws_xpixel = term_cols_ * CHAR_W;
    size.ws_ypixel = term_rows_ * CHAR_H;
    child_ = forkpty(&pty_fd_, nullptr, nullptr, &size);
    if (child_ < 0) return false;
    if (child_ == 0) {
        execv(command.c_str(), argv.data());
        _exit(127);
    }
    if (fcntl(pty_fd_, F_SETFL, O_NONBLOCK) < 0 || fcntl(pty_fd_, F_SETFD, FD_CLOEXEC) < 0) {
        stop_pty();
        return false;
    }
    return true;
}

void UISTPage::stop_timers()
{
    poll_timer_.stop();
    cursor_timer_.stop();
}

bool UISTPage::start_timers()
{
    const bool poll_started = poll_timer_.start(
        [this] { return lv_timer_create(UISTPage::static_poll_cb, 30, this); },
        [](lv_timer_t *timer) { lv_timer_delete(timer); });
    const bool cursor_started = cursor_timer_.start(
        [this] { return lv_timer_create(UISTPage::static_cursor_cb, 500, this); },
        [](lv_timer_t *timer) { lv_timer_delete(timer); });
    if (poll_started && cursor_started) return true;
    stop_timers();
    return false;
}


void UISTPage::start_shell()
{
    terminal_active_ = pty_open("/bin/bash", {
        "-c", "cd ~ && exec env TERM=st-256color COLORTERM=truecolor /bin/bash -i"
    });
    if (!terminal_active_ || !start_timers()) {
        terminal_active_ = false;
        stop_pty();
        static constexpr char message[] = "CLI: failed to start shell\r\n";
        process_bytes(message, sizeof(message) - 1);
        waiting_key_to_exit_ = true;
    }
    render_all();
}

void UISTPage::stop_pty()
{
    pending_input_.clear();
    if (pty_fd_ >= 0) close(pty_fd_);
    pty_fd_ = -1;
    if (child_ <= 0) return;
    int status;
    pid_t result;
    do { result = waitpid(child_, &status, WNOHANG); } while (result < 0 && errno == EINTR);
    if (result == 0) {
        kill(child_, SIGKILL);
        do { result = waitpid(child_, &status, 0); } while (result < 0 && errno == EINTR);
    }
    child_ = -1;
}

int UISTPage::pty_read(char* buffer, size_t capacity)
{
    ssize_t n;
    do { n = read(pty_fd_, buffer, capacity); } while (n < 0 && errno == EINTR);
    if (n < 0 && errno == EAGAIN) return 0;
    return n > 0 ? static_cast<int>(n) : -1;
}

bool UISTPage::flush_input()
{
    while (!pending_input_.empty()) {
        const ssize_t n = write(pty_fd_, pending_input_.data(), pending_input_.size());
        if (n > 0) pending_input_.erase(0, n);
        else if (n < 0 && errno == EAGAIN) return true;
        else if (n >= 0 || errno != EINTR) return false;
    }
    return true;
}

int UISTPage::pty_write(const char* buffer, size_t length)
{
    constexpr size_t queue_limit = 65536;
    if (pty_fd_ < 0) return -1;
    if (length > queue_limit - pending_input_.size()) {
        recover_callback_failure();
        return -1;
    }
    pending_input_.append(buffer, length);
    return static_cast<int>(length);
}

void UISTPage::input(const char* bytes, size_t length)
{
    if (!terminal_active_) return;
    leave_scrollback();
    follow_cursor_in_big_mode();
    pty_write(bytes, length);
}

void UISTPage::scroll(int dx, int dy)
{
    if (big_mode_) pan_big_view(dx * 8, dy * 4);
    else if (dy) scrollback_page(-dy);
    render_all();
}

void UISTPage::static_poll_cb(lv_timer_t* timer) noexcept
{
    auto* self = static_cast<UISTPage*>(lv_timer_get_user_data(timer));
    if (!self) return;
    try {
        if (self->poll_timer_.current(timer)) self->poll_cb();
    } catch (...) {
        self->recover_callback_failure();
    }
}

void UISTPage::poll_cb()
{
    if (!terminal_active_) return;
    char buffer[1024];
    int length = 0;
    bool changed = false;
    for (int n = 0; n < 32 && (length = pty_read(buffer, sizeof(buffer))) > 0; ++n) {
        process_bytes(buffer, length);
        changed = true;
    }
    if (changed) {
        if (scrollback_offset_ > 0) dirty_all();
        follow_cursor_in_big_mode();
        if (big_mode_) dirty_all();
        render_all();
    }
    if (length >= 0) {
        if (!flush_input()) pending_input_.clear();
        return;
    }
    int status = 0;
    const pid_t result = waitpid(child_, &status, WNOHANG);
    if (result == 0 || (result < 0 && errno == EINTR)) return;
    if (result == child_) {
        if (WIFEXITED(status)) exit_status_ = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) exit_status_ = 128 + WTERMSIG(status);
        child_ = -1;
    }
    terminal_active_ = false;
    waiting_key_to_exit_ = true;
}

void UISTPage::static_cursor_cb(lv_timer_t *timer) noexcept
{
    auto *self = static_cast<UISTPage *>(lv_timer_get_user_data(timer));
    if (!self) return;
    try {
        if (self->cursor_timer_.current(timer)) self->cursor_cb();
    } catch (...) {
        self->recover_callback_failure();
    }
}

void UISTPage::cursor_cb()
{
    update_cursor();
    if (!terminal_active_ || !cursor_visible_mode_) {
        show_cursor(false);
        return;
    }
    show_cursor(!cursor_blink_visible_);
}


