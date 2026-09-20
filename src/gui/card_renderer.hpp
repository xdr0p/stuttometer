#pragma once

#include "stuttometer/correlator.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <vector>
#include <string>
#include <cstdint>
#include <filesystem>

namespace stuttometer::gui {

namespace detail {
    [[nodiscard]] std::wstring format_offset(double ms);
}

struct CardRenderOptions {
    int base_width{1200};
    int base_height{675};
    double dpi_scale{1.0};
};

class CardRenderer {
public:
    [[nodiscard]] static bool initialize() noexcept;
    static void shutdown() noexcept;
    [[nodiscard]] static bool is_initialized() noexcept;

    // Export to PNG file on disk
    [[nodiscard]] static bool save_card_to_png(
        const DiagnosticReport& report,
        const std::filesystem::path& file_path,
        const CardRenderOptions& options = {}
    ) noexcept;

    // Export to memory buffer for testing / verification (returns empty vector on failure)
    [[nodiscard]] static std::vector<uint8_t> render_card_to_png_bytes(
        const DiagnosticReport& report,
        const CardRenderOptions& options = {}
    ) noexcept;

    // Direct Windows clipboard copy (CF_DIB format)
    [[nodiscard]] static bool copy_card_to_clipboard(
        HWND owner_hwnd,
        const DiagnosticReport& report,
        const CardRenderOptions& options = {}
    ) noexcept;
};

} // namespace stuttometer::gui
