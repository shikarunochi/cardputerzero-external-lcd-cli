#pragma once

#include <memory>

class StatusDisplay {
public:
    explicit StatusDisplay(const char* device);
    ~StatusDisplay();

private:
    struct Dashboard;
    std::unique_ptr<Dashboard> dashboard_;
};
