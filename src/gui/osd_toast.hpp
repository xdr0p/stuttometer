#pragma once

#include "osd_types.hpp"
#include "stuttometer/correlator.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <chrono>

namespace stuttometer::gui {

struct OsdToastData {
    std::string process_name;
    double duration_ms{0.0};
    double spike_ratio{0.0};
    AttributionTag attribution{AttributionTag::UNKNOWN};
    std::string culprit; // Mirrors report.attribution_process (e.g. nvlddmkm.sys)
    std::string summary; // Mirrors top diagnosis summary (e.g. DPC routine spike)
    double confidence{0.0};
    stuttometer::TriggerSource source{stuttometer::TriggerSource::DXGI_PRESENT_STUTTER};
    uint32_t glitch_count{0};
    double present_threshold_ms{16.67};
    stuttometer::TriggerInfo trigger{};
};

class OsdToast {
public:
    OsdToast() noexcept;
    ~OsdToast() noexcept;

    bool create(HINSTANCE hInst) noexcept;
    void destroy() noexcept;

    void show(const DiagnosticReport& report, uint32_t duration_ms, OsdPosition position) noexcept;
    void hide() noexcept;

    [[nodiscard]] bool is_visible() const noexcept;
    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void on_timer() noexcept;
    void render(HDC hdc, const RECT& rc) noexcept;
    void update_position(OsdPosition position, uint32_t target_pid) noexcept;

    void recreate_fonts(UINT dpi) noexcept;
    void init_gdi_resources() noexcept;
    void destroy_gdi_resources() noexcept;

    HWND hwnd_{nullptr};
    HINSTANCE hinst_{nullptr};
    enum class State { HIDDEN, FADING_IN, DISPLAYING, FADING_OUT };
    State state_{State::HIDDEN};
    uint8_t current_alpha_{0};
    UINT current_dpi_{96};
    std::chrono::steady_clock::time_point state_start_tp_;
    std::chrono::steady_clock::time_point display_deadline_;
    uint32_t display_duration_ms_{3500};
    OsdToastData current_data_;
    OsdPosition current_position_{OsdPosition::TOP_RIGHT};
    UINT_PTR timer_id_{0};

    HFONT font_title_{nullptr};
    HFONT font_main_{nullptr};
    HFONT font_sub_{nullptr};

    HBRUSH br_bg_{nullptr};
    HPEN pen_border_{nullptr};

    static constexpr UINT_PTR TIMER_ID = 1001;
    static constexpr uint32_t FADE_IN_MS = 150;
    static constexpr uint32_t FADE_OUT_MS = 250;
    static constexpr uint8_t TARGET_ALPHA = 235;
};

} // namespace stuttometer::gui
