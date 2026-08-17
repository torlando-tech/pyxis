#pragma once
#include <cstdint>
#include <string>
using SemaphoreHandle_t = void*;
using TaskHandle_t = void*;
class String {
public:
    String() = default;
    String(const char* value) : value_(value ? value : "") {}
    std::size_t length() const { return value_.size(); }
    const char* c_str() const { return value_.c_str(); }
private:
    std::string value_;
};
