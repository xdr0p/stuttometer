#pragma once

#include <iostream>
#include <cstdlib>

#define STUTTO_ASSERT(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "[ASSERTION FAILED] " << #expr << "\n" \
                      << "  File: " << __FILE__ << ":" << __LINE__ << "\n" \
                      << "  Function: " << __func__ << "\n"; \
            std::abort(); \
        } \
    } while (0)
