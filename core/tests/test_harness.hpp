#pragma once

#include <stdexcept>
#include <string_view>

inline void pms_check(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

