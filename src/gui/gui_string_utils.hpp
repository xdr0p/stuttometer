#pragma once

#include <windows.h>
#include <cstdarg>
#include <cstddef>
#include <cwchar>
#include <string>
#include <string_view>
#include <utility>

#include "stuttometer/privilege_utils.hpp"

namespace stuttometer::gui {

// Stack-buffer wide-string formatting. Zero heap allocation for short strings.
// Returns character count written (excluding NUL), or 0 on failure.
template <std::size_t N>
inline int wfmt(wchar_t (&buf)[N], const wchar_t* fmt, ...) noexcept {
    va_list args;
    va_start(args, fmt);
    const int n = vswprintf_s(buf, N, fmt, args);
    va_end(args);
    return (n > 0) ? n : 0;
}

// Centralized UTF-8 to UTF-16 wide string conversion.
// Forwards to stuttometer::utf8_to_utf16.
inline std::wstring utf8_to_wide(std::string_view utf8) {
    return stuttometer::utf8_to_utf16(utf8);
}

} // namespace stuttometer::gui
