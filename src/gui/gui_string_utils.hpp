#pragma once

#include <windows.h>
#include <cstdarg>
#include <cstddef>
#include <cwchar>
#include <string>
#include <string_view>
#include <utility>

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
// Replaces duplicated to_wide() and to_wide_str() across GUI modules.
inline std::wstring utf8_to_wide(std::string_view utf8) {
    if (utf8.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), needed);
    return result;
}

} // namespace stuttometer::gui
