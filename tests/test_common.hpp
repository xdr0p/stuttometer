#pragma once

#include <iostream>
#include <sstream>
#include <stdexcept>

#define STUTTO_ASSERT(expr) \
    do { \
        if (!(expr)) { \
            std::ostringstream _stutto_assert_oss; \
            _stutto_assert_oss << "[ASSERTION FAILED] " << #expr << "\n" \
                               << "  File: " << __FILE__ << ":" << __LINE__ << "\n" \
                               << "  Function: " << __func__ << "\n"; \
            std::cerr << _stutto_assert_oss.str(); \
            throw std::runtime_error(_stutto_assert_oss.str()); \
        } \
    } while (0)
