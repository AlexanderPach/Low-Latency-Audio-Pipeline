#pragma once

#include <exception>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace test {

inline void require(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                                 ": requirement failed: " + expression);
    }
}

using Case = std::pair<std::string_view, void (*)()>;

inline int run(int argc, char** argv, std::initializer_list<Case> cases) {
    if (argc == 2) {
        for (const auto& [name, function] : cases) {
            if (name == argv[1]) {
                try {
                    function();
                    std::cout << "PASS: " << name << '\n';
                    return 0;
                } catch (const std::exception& error) {
                    std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
                    return 1;
                } catch (...) {
                    std::cerr << "FAIL: " << name << ": unexpected exception\n";
                    return 1;
                }
            }
        }
    }

    std::cerr << "Usage: " << argv[0] << " <case>\nAvailable cases:";
    for (const auto& [name, function] : cases) {
        std::cerr << ' ' << name;
    }
    std::cerr << '\n';
    return 2;
}

} // namespace test

// Unlike assert(), these checks also run in Release builds with NDEBUG defined.
#define REQUIRE(...) \
    ::test::require(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __FILE__, __LINE__)
