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
#include <gdiplus.h>
#include <vector>
#include <string>
#include <cstdint>
#include <filesystem>

namespace stuttometer::gui {

namespace detail {
    [[nodiscard]] std::wstring format_offset(double ms);

    [[nodiscard]] inline std::wstring format_center_label(double max_abs_offset_ms) {
        return (max_abs_offset_ms >= 1000.0) ? L"Trigger (0.0 s)" : L"Trigger (0 ms)";
    }

    enum class MetricSeverity { Normal, Warning, Danger };

    [[nodiscard]] inline MetricSeverity classify_stall(
        double dur_ms, double spike_ratio, double drop, bool is_audio, uint32_t glitches
    ) {
        if (is_audio) {
            return (glitches > 0) ? MetricSeverity::Danger : MetricSeverity::Normal;
        }
        if (dur_ms >= 50.0 || drop > 0.60) {
            return MetricSeverity::Danger;
        }
        if (dur_ms >= 25.0 || drop >= 0.30 || spike_ratio >= 2.0) {
            return MetricSeverity::Warning;
        }
        return MetricSeverity::Normal;
    }

    struct BannerRects {
        Gdiplus::RectF rc_pill;
        Gdiplus::RectF rc_pill_text; // Text region only: X in [rc_pill.X + 22*s, rc_pill.GetRight() - 4*s]
        Gdiplus::RectF rc_culprit;
        Gdiplus::RectF rc_conf;
    };

    // banner_row1_top_y: top edge of Row 1's 24px control strip, in final bitmap pixel coordinates
    [[nodiscard]] BannerRects compute_banner_rects(
        int width, float side_margin, float banner_row1_top_y, float s,
        Gdiplus::Graphics& g, const Gdiplus::FontFamily* pSans, const Gdiplus::Font& font_tag,
        const std::wstring& conf_str
    );

    /**
     * Computes exact peak PointF in bitmap pixels from report timeline geometry.
     * Follows the trigger-preference logic: prefers the anchor/trigger frame if its
     * duration is at least as large as the max duration sample; otherwise uses the
     * highest duration sample in the timeline.
     */
    [[nodiscard]] Gdiplus::PointF compute_peak_pixel(
        const DiagnosticReport& report, int width, int height, float s
    );

    /**
     * Computes exact peak duration in milliseconds matching compute_peak_pixel.
     */
    [[nodiscard]] double compute_peak_duration(const DiagnosticReport& report);
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

    // Export to DIB memory buffer (BITMAPINFOHEADER + 24bpp RGB pixels, returns empty vector on failure)
    [[nodiscard]] static std::vector<uint8_t> render_card_to_dib_bytes(
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
