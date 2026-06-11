// disruptor.hpp
// Cache-Line Padding
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <array>
#include <optional>

#ifdef __cpp_lib_hardware_interference_size
    inline constexpr std::size_t kCacheLineSize =
        std::hardware_destructive_interference_size;
#else
    inline constexpr std::size_t kCacheLineSize = 64;
#endif