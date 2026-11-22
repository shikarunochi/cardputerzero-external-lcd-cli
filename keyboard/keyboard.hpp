#pragma once

#include <memory>

class UISTPage;

class Keyboard {
public:
    // An empty path selects the launcher's keyboard, then the TCA8418 by name.
    explicit Keyboard(const char* path);
    ~Keyboard();
    int fd() const;
    void dispatch(UISTPage& page);

private:
    struct Device;
    std::unique_ptr<Device> device_;
};
