#include "card_renderer.hpp"
#include "stuttometer/internal/redaction_utils.hpp"

#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

#include <mutex>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <string>
#include <memory>
#include <cstring>

namespace stuttometer::gui {

static std::mutex g_init_mutex;
static bool g_ever_initialized = false;
static bool g_is_initialized = false;
static ULONG_PTR g_gdiplus_token = 0;

static std::mutex g_render_mutex;

static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0;
    UINT size = 0;

    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;

    std::vector<uint8_t> memory(size);
    auto* pImageCodecInfo = reinterpret_cast<Gdiplus::ImageCodecInfo*>(memory.data());

    Gdiplus::GetImageEncoders(num, size, pImageCodecInfo);

    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            return static_cast<int>(j);
        }
    }
    return -1;
}

static std::wstring to_wide_str(std::string_view utf8) {
    if (utf8.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), needed);
    return result;
}

namespace detail {
std::wstring format_offset(double ms) {
    std::wstringstream ss;
    ss << std::showpos;
    if (std::abs(ms) >= 1000.0) {
        ss << std::fixed << std::setprecision(1) << (ms / 1000.0) << L" s";
    } else {
        ss << std::fixed << std::setprecision(0) << ms << L" ms";
    }
    return ss.str();
}
} // namespace detail

// -----------------------------------------------------------------------------
// Rounded rectangle helper (GraphicsPath based) — mirrors the GUI's RoundRect
// -----------------------------------------------------------------------------
static std::unique_ptr<Gdiplus::GraphicsPath> make_rounded_path(const Gdiplus::RectF& rc, float radius) {
    auto path = std::make_unique<Gdiplus::GraphicsPath>();
    float d = radius * 2.0f;
    if (d > rc.Width)  d = rc.Width;
    if (d > rc.Height) d = rc.Height;
    if (d <= 0.0f) {
        path->AddRectangle(rc);
        return path;
    }
    path->AddArc(rc.X,                       rc.Y,                        d, d, 180.0f, 90.0f);
    path->AddArc(rc.GetRight() - d,          rc.Y,                        d, d, 270.0f, 90.0f);
    path->AddArc(rc.GetRight() - d,          rc.GetBottom() - d,          d, d,   0.0f, 90.0f);
    path->AddArc(rc.X,                       rc.GetBottom() - d,          d, d,  90.0f, 90.0f);
    path->CloseFigure();
    return path;
}

static void fill_rounded_rect(
    Gdiplus::Graphics& g,
    const Gdiplus::RectF& rc,
    float radius,
    Gdiplus::Brush* fill,
    Gdiplus::Pen* border
) {
    auto path = make_rounded_path(rc, radius);
    if (fill)   g.FillPath(fill, path.get());
    if (border) g.DrawPath(border, path.get());
}

// -----------------------------------------------------------------------------
// Friendly label formatters
// -----------------------------------------------------------------------------
static std::wstring format_trigger_reason_label(TriggerReason r) {
    switch (r) {
        case TriggerReason::STATIC_THRESHOLD:      return L"Static Threshold";
        case TriggerReason::RELATIVE_SPIKE:        return L"Relative Spike";
        case TriggerReason::STATISTICAL_OUTLIER:   return L"Statistical Outlier";
        case TriggerReason::CADENCE_JUDDER:        return L"Cadence Judder";
        case TriggerReason::AUDIO_BUFFER_UNDERRUN: return L"Audio Underrun";
        case TriggerReason::DWM_COMPOSITOR_GLITCH: return L"DWM Compositor Glitch";
        case TriggerReason::NONE:
        default:                                   return L"None";
    }
}

static std::wstring format_trigger_source_label(TriggerSource s) {
    switch (s) {
        case TriggerSource::DXGI_PRESENT_STUTTER: return L"DXGI Present";
        case TriggerSource::AUDIO_GLITCH:         return L"Audio Glitch";
        case TriggerSource::MANUAL:               return L"Manual";
        case TriggerSource::KERNEL_FRAME_STALL:   return L"Kernel Frame Stall";
        case TriggerSource::DWM_GLITCH:           return L"DWM Glitch";
        case TriggerSource::FRAME_PACING_JUDDER:  return L"Pacing Judder";
        case TriggerSource::NONE:
        default:                                  return L"None";
    }
}

// -----------------------------------------------------------------------------
// Main rendering
// -----------------------------------------------------------------------------
static void draw_card(
    Gdiplus::Graphics& g,
    int width,
    int height,
    const DiagnosticReport& report,
    const CardRenderOptions& options
) {
    using namespace Gdiplus;

    const float s = static_cast<float>(options.dpi_scale);

    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // --- Palette (aligned with Stuttometer Fluent Zinc dark theme) ---
    Color color_bg(255, 0x11, 0x15, 0x1F);              // #11151f (standalone canvas)
    Color color_card(255, 0x1C, 0x21, 0x2C);            // #1c212c (unified card panels & tiles)
    Color color_card_border(255, 0x28, 0x30, 0x42);     // #283042 (container outline)
    Color color_inset_border(255, 0x2A, 0x35, 0x4B);    // #2a354b (outer 1px inset — DO NOT CHANGE)
    Color color_text_pri(255, 0xFF, 0xFF, 0xFF);        // #ffffff (pure white)
    Color color_text_bright(255, 0xF1, 0xF5, 0xF9);     // #f1f5f9 (bright text)
    Color color_text_label(255, 0xCB, 0xD5, 0xE1);      // #cbd5e1 (slate label text)
    Color color_text_muted(255, 0x94, 0xA3, 0xB8);      // #94a3b8 (muted slate)
    Color color_accent_emerald(255, 16, 185, 129);      // #10b981 (emerald brand/healthy accent)
    Color color_accent_danger(255, 239, 68, 68);        // #ef4444 (crimson critical)
    Color color_accent_amb(255, 245, 158, 11);          // #f59e0b (amber warning/alert)

    // Attribution accent (unified 35% desaturated palette)
    Color color_attr;
    std::wstring attr_label;
    switch (report.attribution) {
        case AttributionTag::GAME_ENGINE:
            color_attr = Color(255, 218, 161, 66);
            attr_label = L"GAME ENGINE";
            break;
        case AttributionTag::DWM_COMPOSITION:
            color_attr = Color(255, 154, 100, 205);
            attr_label = L"DWM COMPOSITION";
            break;
        case AttributionTag::EXTERNAL_CONTENTION:
            color_attr = Color(255, 197, 86, 86);
            attr_label = L"EXTERNAL CONTENTION";
            break;
        case AttributionTag::UNKNOWN:
        default:
            color_attr = Color(255, 105, 115, 130);
            attr_label = L"UNKNOWN";
            break;
    }

    // ---- 1. Canvas Background ----
    SolidBrush bg_brush(color_bg);
    g.FillRectangle(&bg_brush, 0, 0, width, height);

    // ---- 2. Outer 1px inset border (TEST-CRITICAL: pixel (1,0) must be closer to this than the bg) ----
    // Draw at exactly the outer perimeter so pixel (0..1, 0) carries border color.
    {
        Pen border_pen(color_inset_border, 1.0f);
        g.DrawRectangle(&border_pen, 0, 0, width - 1, height - 1);
    }

    // --- Fonts ---
    FontFamily sans_family(L"Segoe UI");
    const FontFamily* pSans = sans_family.IsAvailable() ? &sans_family : FontFamily::GenericSansSerif();

    FontFamily mono_family(L"Consolas");
    const FontFamily* pMono = mono_family.IsAvailable() ? &mono_family : pSans;

    Font font_title(pSans, 16.0f * s, FontStyleBold, UnitPixel);
    Font font_tag(pSans, 10.0f * s, FontStyleBold, UnitPixel);
    Font font_callout(pSans, 18.0f * s, FontStyleBold, UnitPixel);
    Font font_regular(pSans, 11.0f * s, FontStyleRegular, UnitPixel);
    Font font_bold(pSans, 11.0f * s, FontStyleBold, UnitPixel);
    Font font_small(pSans, 9.5f * s, FontStyleRegular, UnitPixel);
    Font font_small_bold(pSans, 9.5f * s, FontStyleBold, UnitPixel);
    Font font_mono(pMono, 9.5f * s, FontStyleRegular, UnitPixel);

    StringFormat fmt_left;
    fmt_left.SetAlignment(StringAlignmentNear);
    fmt_left.SetLineAlignment(StringAlignmentCenter);
    fmt_left.SetTrimming(StringTrimmingEllipsisCharacter);
    fmt_left.SetFormatFlags(StringFormatFlagsNoWrap);

    StringFormat fmt_right;
    fmt_right.SetAlignment(StringAlignmentFar);
    fmt_right.SetLineAlignment(StringAlignmentCenter);
    fmt_right.SetTrimming(StringTrimmingEllipsisCharacter);
    fmt_right.SetFormatFlags(StringFormatFlagsNoWrap);

    StringFormat fmt_center;
    fmt_center.SetAlignment(StringAlignmentCenter);
    fmt_center.SetLineAlignment(StringAlignmentCenter);
    fmt_center.SetFormatFlags(StringFormatFlagsNoWrap);

    SolidBrush br_pri(color_text_pri);
    SolidBrush br_bright(color_text_bright);
    SolidBrush br_label(color_text_label);
    SolidBrush br_muted(color_text_muted);
    SolidBrush br_card(color_card);
    Pen pen_card(color_card_border, 1.0f);

    // =========================================================================
    // 1. HEADER (y: 16..64)
    // =========================================================================
    const float top_margin  = 16.0f * s;
    const float side_margin = 24.0f * s;
    const float content_w   = width - (side_margin * 2.0f);

    // Version badge pill (rounded)
    {
        std::string ver = report.tool_version.empty() ? "0.4.1" : report.tool_version;
        std::wstring ver_badge = L"STUTTOMETER v" + to_wide_str(ver);
        RectF rc_ver(side_margin, top_margin, 130.0f * s, 22.0f * s);

        SolidBrush br_ver_bg(Color(255, 30, 41, 59));
        Pen pen_ver(Color(255, 51, 65, 85), 1.0f);
        fill_rounded_rect(g, rc_ver, 6.0f * s, &br_ver_bg, &pen_ver);

        SolidBrush br_ver_txt(color_text_bright);
        g.DrawString(ver_badge.c_str(), -1, &font_tag, rc_ver, &fmt_center, &br_ver_txt);
    }

    // Target process name (subtractive header: "Target: <process>")
    {
        std::string proc_name = report.redacted ? "Process_REDACTED" : report.target_process;
        if (proc_name.empty()) proc_name = "System Telemetry Event";
        std::wstring proc_w = L"Target: " + to_wide_str(proc_name);
        RectF rc_proc(side_margin + 138.0f * s, top_margin, 600.0f * s, 22.0f * s);
        g.DrawString(proc_w.c_str(), -1, &font_tag, rc_proc, &fmt_left, &br_label);
    }

    // Timestamp (right aligned)
    {
        std::wstring time_w = to_wide_str(report.timestamp_utc.empty() ? "N/A" : report.timestamp_utc);
        RectF rc_time(width - side_margin - 300.0f * s, top_margin, 300.0f * s, 22.0f * s);
        g.DrawString(time_w.c_str(), -1, &font_mono, rc_time, &fmt_right, &br_muted);
    }

    // =========================================================================
    // 2. BLAME BANNER (y: 48..140)
    // =========================================================================
    const float banner_y = 48.0f * s;
    const float banner_h = 92.0f * s;
    const float card_radius = 8.0f * s;

    RectF rc_banner(side_margin, banner_y, content_w, banner_h);
    fill_rounded_rect(g, rc_banner, card_radius, &br_card, &pen_card);

    // Attribution status-dot pill
    {
        RectF rc_pill(side_margin + 16.0f * s, banner_y + 12.0f * s, 160.0f * s, 24.0f * s);
        SolidBrush br_pill_bg(Color(255, 30, 41, 59));
        Pen pen_pill(Color(255, 51, 65, 85), 1.0f);
        fill_rounded_rect(g, rc_pill, 6.0f * s, &br_pill_bg, &pen_pill);

        // Status dot (6px circle filled with desaturated category color)
        RectF rc_dot(rc_pill.X + 10.0f * s, rc_pill.Y + (rc_pill.Height - 6.0f * s) / 2.0f, 6.0f * s, 6.0f * s);
        SolidBrush br_dot(color_attr);
        g.FillEllipse(&br_dot, rc_dot);

        // Status text (bright white, right of dot)
        RectF rc_pill_txt(rc_pill.X + 22.0f * s, rc_pill.Y, rc_pill.Width - 26.0f * s, rc_pill.Height);
        g.DrawString(attr_label.c_str(), -1, &font_tag, rc_pill_txt, &fmt_left, &br_bright);
    }

    // Culprit process / driver
    {
        std::string culprit = report.attribution_process;
        if (report.redacted || report.attribution_redacted) {
            culprit = get_redacted_module_name(culprit, true);
        }
        if (culprit.empty()) culprit = "Unattributed Anomaly";
        std::wstring culprit_w = to_wide_str(culprit);
        RectF rc_culprit(
            side_margin + 16.0f * s + 160.0f * s + 14.0f * s,
            banner_y + 10.0f * s,
            480.0f * s,
            26.0f * s
        );
        g.DrawString(culprit_w.c_str(), -1, &font_callout, rc_culprit, &fmt_left, &br_pri);
    }

    // Confidence badge (right)
    {
        double top_confidence = report.diagnoses.empty() ? 0.0 : report.diagnoses[0].confidence;
        std::wstringstream conf_ss;
        conf_ss << std::fixed << std::setprecision(0)
                << std::lround(top_confidence * 100.0) << L"% CONFIDENCE";
        std::wstring conf_str = conf_ss.str();

        RectF rc_conf(width - side_margin - 180.0f * s, banner_y + 12.0f * s, 164.0f * s, 24.0f * s);
        SolidBrush br_conf_bg(Color(255, 30, 41, 59));
        Pen pen_conf(Color(255, 51, 65, 85), 1.0f);
        fill_rounded_rect(g, rc_conf, 6.0f * s, &br_conf_bg, &pen_conf);

        SolidBrush br_conf_txt(color_text_label);
        g.DrawString(conf_str.c_str(), -1, &font_small_bold, rc_conf, &fmt_center, &br_conf_txt);
    }

    // Summary (row 2 of blame banner)
    {
        std::string summary = report.diagnoses.empty()
            ? "No conclusive diagnosis available for this event."
            : report.diagnoses[0].summary;
        if (report.redacted || report.attribution_redacted) {
            auto ids = collect_report_ids(report);
            summary = redact_text_with_ids(summary, ids);
        }
        std::wstring summary_w = to_wide_str(summary);
        RectF rc_sum(side_margin + 16.0f * s, banner_y + 44.0f * s, content_w - 32.0f * s, 22.0f * s);
        g.DrawString(summary_w.c_str(), -1, &font_regular, rc_sum, &fmt_left, &br_label);
    }

    // Telemetry loss warning (amber alert)
    {
        uint64_t total_loss = report.dropped_events + report.producer_dropped_events + report.etw_events_lost;
        if (total_loss > 0) {
            std::wstringstream loss_ss;
            loss_ss << L"[!] Telemetry Loss: " << total_loss << L" event(s) dropped upstream";
            std::wstring loss_w = loss_ss.str();
            RectF rc_loss(side_margin + 16.0f * s, banner_y + 68.0f * s, content_w - 32.0f * s, 18.0f * s);
            SolidBrush br_loss(color_accent_amb);
            g.DrawString(loss_w.c_str(), -1, &font_small_bold, rc_loss, &fmt_left, &br_loss);
        }
    }

    // =========================================================================
    // 3. METRICS GRID (y: 148..224)
    // =========================================================================
    const float grid_y   = 148.0f * s;
    const float grid_h   = 76.0f * s;
    const float card_gap = 10.0f * s;
    const float cell_w   = (content_w - (card_gap * 3.0f)) / 4.0f;

    auto draw_metric_tile = [&](int index,
                                const wchar_t* title,
                                const std::wstring& main_val,
                                const std::wstring& sub_val,
                                Color val_color) {
        float cell_x = side_margin + index * (cell_w + card_gap);
        RectF rc_cell(cell_x, grid_y, cell_w, grid_h);
        fill_rounded_rect(g, rc_cell, card_radius, &br_card, &pen_card);

        RectF rc_title(cell_x + 10.0f * s, grid_y + 8.0f * s, cell_w - 20.0f * s, 16.0f * s);
        g.DrawString(title, -1, &font_small_bold, rc_title, &fmt_left, &br_muted);

        SolidBrush br_val(val_color);
        RectF rc_main(cell_x + 10.0f * s, grid_y + 24.0f * s, cell_w - 20.0f * s, 26.0f * s);
        g.DrawString(main_val.c_str(), -1, &font_callout, rc_main, &fmt_left, &br_val);

        RectF rc_sub(cell_x + 10.0f * s, grid_y + 50.0f * s, cell_w - 20.0f * s, 18.0f * s);
        g.DrawString(sub_val.c_str(), -1, &font_small, rc_sub, &fmt_left, &br_muted);
    };

    const bool is_audio_event = (report.trigger.source == TriggerSource::AUDIO_GLITCH);

    // Tile 1: Stall Duration (matches Session Summary: > 50.0 ms is critical red)
    {
        std::wstring dur_main;
        std::wstring dur_sub;
        Color val_color = color_text_bright;
        if (is_audio_event) {
            uint32_t gc = report.trigger.glitch_count > 0 ? report.trigger.glitch_count : 1;
            dur_main = L"Glitch (x" + std::to_wstring(gc) + L")";
            dur_sub  = L"Audio buffer underrun";
            // Card-local convention: highlight audio glitch underrun as critical
            val_color = (report.trigger.glitch_count > 0) ? color_accent_danger : color_text_bright;
        } else {
            std::wstringstream dss;
            dss << std::fixed << std::setprecision(1) << report.trigger.duration_ms << L" ms";
            dur_main = dss.str();

            std::wstringstream sss;
            sss << std::fixed << std::setprecision(2) << report.trigger.spike_ratio << L"x spike ratio";
            dur_sub = sss.str();

            val_color = (report.trigger.duration_ms > 50.0) ? color_accent_danger : color_text_bright;
        }
        draw_metric_tile(0, L"STALL DURATION", dur_main, dur_sub, val_color);
    }

    // Tile 2: Effective Framerate (state-driven colors, muted slate for N/A)
    {
        std::wstring fps_main;
        std::wstring fps_sub;
        Color val_color = color_text_bright;
        if (is_audio_event || report.trigger.duration_ms <= 0.0) {
            fps_main = L"N/A";
            fps_sub  = is_audio_event ? L"Audio-only event" : L"Zero frame duration";
            val_color = color_text_muted;
        } else if (report.trigger.baseline_fps <= 0.0) {
            double stall_fps = 1000.0 / report.trigger.duration_ms;
            std::wstringstream fss;
            fss << L"N/A -> " << std::fixed << std::setprecision(1) << stall_fps << L" FPS";
            fps_main = fss.str();
            fps_sub  = L"Baseline framerate unavailable";
            val_color = color_text_bright;
        } else {
            double stall_fps = 1000.0 / report.trigger.duration_ms;
            std::wstringstream fss;
            fss << std::fixed << std::setprecision(1) << report.trigger.baseline_fps
                << L" -> " << stall_fps << L" FPS";
            fps_main = fss.str();
            fps_sub  = L"Framerate drop";

            double drop = (report.trigger.baseline_fps - stall_fps) / report.trigger.baseline_fps;
            if (drop > 0.60) {
                val_color = color_accent_danger;
            } else if (drop >= 0.30) {
                val_color = color_accent_amb;
            } else {
                val_color = color_text_bright;
            }
        }
        draw_metric_tile(1, L"EFFECTIVE FRAMERATE", fps_main, fps_sub, val_color);
    }

    // Tile 3: Trigger Classification (neutral slate label)
    {
        std::wstring src_main = format_trigger_reason_label(report.trigger.reason);
        std::wstring src_sub  = format_trigger_source_label(report.trigger.source);
        draw_metric_tile(2, L"TRIGGER CLASSIFICATION", src_main, src_sub, color_text_label);
    }

    // Tile 4: Execution Thread (neutral slate label)
    {
        std::wstring core_main = L"CPU Core " + std::to_wstring(static_cast<int>(report.trigger.cpu_index));
        std::wstring tid_sub;
        if (report.redacted) {
            tid_sub = L"TID: [REDACTED]";
        } else if (report.trigger.target_tid == 0) {
            tid_sub = L"TID: N/A";
        } else {
            tid_sub = L"TID: " + std::to_wstring(report.trigger.target_tid);
        }
        draw_metric_tile(3, L"EXECUTION THREAD", core_main, tid_sub, color_text_label);
    }

    // =========================================================================
    // 4. FRAME GRAPH CARD (y: 232..625)
    // =========================================================================
    const float graph_y = 232.0f * s;
    const float graph_h = 398.0f * s;
    RectF rc_graph(side_margin, graph_y, content_w, graph_h);
    fill_rounded_rect(g, rc_graph, card_radius, &br_card, &pen_card);

    // Title
    {
        RectF rc_gh(side_margin + 14.0f * s, graph_y + 10.0f * s, 400.0f * s, 20.0f * s);
        const wchar_t* hdr = is_audio_event
            ? L"AUDIO UNDER-RUN TIMELINE"
            : L"FRAME PACING TIMELINE (1,024 SAMPLES)";
        g.DrawString(hdr, -1, &font_small_bold, rc_gh, &fmt_left, &br_muted);
    }

    // Plot area
    const float plot_x = side_margin + 60.0f * s;
    const float plot_y = graph_y + 36.0f * s;
    const float plot_w = content_w - 76.0f * s;
    const float plot_h = graph_h - 60.0f * s;

    if (is_audio_event) {
        // ---- Audio glitch visualization: a stylized waveform panel ----
        RectF rc_inner(plot_x, plot_y, plot_w, plot_h);
        SolidBrush br_inner(Color(255, 19, 22, 29));
        fill_rounded_rect(g, rc_inner, 6.0f * s, &br_inner, nullptr);

        // Flat baseline in emerald
        float mid_y = plot_y + plot_h * 0.5f;
        Pen baseline_pen(color_accent_emerald, 1.5f * s);
        g.DrawLine(&baseline_pen, plot_x + 20.0f * s, mid_y, plot_x + plot_w - 20.0f * s, mid_y);

        // Amplitude envelope at the trigger column (amber alert)
        const float trig_x = plot_x + plot_w * 0.5f;

        Pen env_pen(Color(200, 245, 158, 11), 1.5f * s);
        env_pen.SetDashStyle(DashStyleDash);
        g.DrawLine(&env_pen, trig_x, plot_y + 12.0f * s, trig_x, plot_y + plot_h - 12.0f * s);

        // Pulse spike at the trigger (amber alert)
        SolidBrush spike_br(color_accent_amb);
        RectF rc_pulse(trig_x - 6.0f * s, mid_y - 60.0f * s, 12.0f * s, 120.0f * s);
        fill_rounded_rect(g, rc_pulse, 3.0f * s, &spike_br, nullptr);

        // Explanatory message in muted slate
        RectF rc_msg(plot_x + 20.0f * s, plot_y + plot_h - 46.0f * s, plot_w - 40.0f * s, 26.0f * s);
        std::wstring msg = L"Audio buffer underrun captured via Microsoft-Windows-Audio (Event ID 11)";
        g.DrawString(msg.c_str(), -1, &font_regular, rc_msg, &fmt_center, &br_muted);
    } else {
        // ---- Normal frametime timeline ----
        double max_ms = 50.0;
        if (report.present_threshold_ms * 1.5 > max_ms) {
            max_ms = report.present_threshold_ms * 1.5;
        }
        if (report.trigger.duration_ms * 1.2 > max_ms) {
            max_ms = report.trigger.duration_ms * 1.2;
        }
        for (const auto& pt : report.frame_timeline) {
            if (pt.duration_ms * 1.15 > max_ms) {
                max_ms = pt.duration_ms * 1.15;
            }
        }
        if (max_ms > 5000.0) max_ms = 5000.0;

        // Horizontal gridlines
        const double grid_steps[] = { 16.67, 33.33, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0 };
        Pen grid_pen(Color(255, 30, 41, 59), 1.0f);
        grid_pen.SetDashStyle(DashStyleDash);

        for (double ms_val : grid_steps) {
            if (ms_val > max_ms * 0.95) continue;
            float y_pos = plot_y + plot_h - static_cast<float>((ms_val / max_ms) * plot_h);
            g.DrawLine(&grid_pen, plot_x, y_pos, plot_x + plot_w, y_pos);

            std::wstringstream ms_ss;
            ms_ss << std::fixed << std::setprecision(1) << ms_val << L" ms";
            std::wstring ms_str = ms_ss.str();
            RectF rc_lbl(side_margin + 4.0f * s, y_pos - 8.0f * s, 52.0f * s, 16.0f * s);
            g.DrawString(ms_str.c_str(), -1, &font_mono, rc_lbl, &fmt_right, &br_muted);
        }

        // Baseline avg reference line (subtle dotted slate, slate label)
        if (report.trigger.baseline_avg_ms > 0.0 && report.trigger.baseline_avg_ms < max_ms) {
            float y_base = plot_y + plot_h - static_cast<float>((report.trigger.baseline_avg_ms / max_ms) * plot_h);
            Pen base_pen(Color(180, 148, 163, 184), 1.0f);
            base_pen.SetDashStyle(DashStyleDot);
            g.DrawLine(&base_pen, plot_x, y_base, plot_x + plot_w, y_base);

            RectF rc_base_lbl(plot_x + plot_w - 120.0f * s, y_base - 14.0f * s, 116.0f * s, 14.0f * s);
            SolidBrush br_base_txt(color_text_muted);
            g.DrawString(L"Baseline Avg", -1, &font_mono, rc_base_lbl, &fmt_right, &br_base_txt);
        }

        // Stutter threshold reference line (dashed amber, slate label)
        if (report.present_threshold_ms > 0.0 && report.present_threshold_ms < max_ms) {
            float y_thresh = plot_y + plot_h - static_cast<float>((report.present_threshold_ms / max_ms) * plot_h);
            Pen thresh_pen(Color(180, 245, 158, 11), 1.0f);
            thresh_pen.SetDashStyle(DashStyleDash);
            g.DrawLine(&thresh_pen, plot_x, y_thresh, plot_x + plot_w, y_thresh);

            RectF rc_thresh_lbl(plot_x + 10.0f * s, y_thresh - 14.0f * s, 160.0f * s, 14.0f * s);
            SolidBrush br_thresh_txt(color_text_muted);
            g.DrawString(L"Stutter Threshold", -1, &font_mono, rc_thresh_lbl, &fmt_left, &br_thresh_txt);
        }

        if (report.frame_timeline.empty()) {
            RectF rc_empty(plot_x, plot_y, plot_w, plot_h);
            g.DrawString(L"No frame timeline points recorded in retained window",
                         -1, &font_regular, rc_empty, &fmt_center, &br_muted);
        } else {
            const size_t pt_count = report.frame_timeline.size();

            // Anchor / trigger frame
            size_t trig_idx = pt_count / 2;
            bool found_trig = false;
            for (size_t i = 0; i < pt_count; ++i) {
                if (report.frame_timeline[i].relative_index == 0) {
                    trig_idx = i;
                    found_trig = true;
                    break;
                }
            }
            if (!found_trig) {
                for (size_t i = 0; i < pt_count; ++i) {
                    if (report.frame_timeline[i].is_trigger_frame) {
                        trig_idx = i;
                        break;
                    }
                }
            }

            auto x_at = [&](size_t i) -> float {
                if (pt_count <= 1) return plot_x + plot_w / 2.0f;
                return plot_x + (static_cast<float>(i) / static_cast<float>(pt_count - 1)) * plot_w;
            };

            const float trig_x = x_at(trig_idx);

            // ---- Single subtle dark gradient with soft amber trigger column glow ----
            {
                LinearGradientBrush bg_grad(
                    PointF(plot_x, plot_y),
                    PointF(plot_x + plot_w, plot_y),
                    Color(255, 19, 23, 31),
                    Color(255, 22, 27, 36)
                );
                g.FillRectangle(&bg_grad, plot_x, plot_y, plot_w, plot_h);

                // Soft amber column glow near the trigger
                SolidBrush col_br(Color(15, 245, 158, 11));
                g.FillRectangle(&col_br, trig_x - 20.0f * s, plot_y, 40.0f * s, plot_h);
            }

            // ---- Batch convert to PointF ----
            std::vector<PointF> line_pts(pt_count);
            for (size_t i = 0; i < pt_count; ++i) {
                float px = x_at(i);
                double d_clamp = std::clamp(report.frame_timeline[i].duration_ms, 0.0, max_ms);
                float py = plot_y + plot_h - static_cast<float>((d_clamp / max_ms) * plot_h);
                line_pts[i] = PointF(px, py);
            }

            // ---- Area fill with vertical gradient (Emerald fading to transparent at baseline) ----
            if (pt_count > 1) {
                std::vector<PointF> poly_pts;
                poly_pts.reserve(pt_count + 2);
                poly_pts.push_back(PointF(line_pts[0].X, plot_y + plot_h));
                for (const auto& p : line_pts) poly_pts.push_back(p);
                poly_pts.push_back(PointF(line_pts.back().X, plot_y + plot_h));

                LinearGradientBrush area_grad(
                    PointF(0.0f, plot_y),
                    PointF(0.0f, plot_y + plot_h),
                    Color(120, 16, 185, 129),
                    Color(0,   16, 185, 129)
                );
                g.FillPolygon(&area_grad, poly_pts.data(), static_cast<INT>(poly_pts.size()));
            }

            // ---- Curve (Emerald) ----
            Pen curve_pen(color_accent_emerald, 1.5f * s);
            if (pt_count > 1) {
                g.DrawLines(&curve_pen, line_pts.data(), static_cast<INT>(line_pts.size()));
            } else {
                g.FillEllipse(&br_pri, line_pts[0].X - 3.0f, line_pts[0].Y - 3.0f, 6.0f, 6.0f);
            }

            // ---- Prominent trigger marker (amber alert) ----
            {
                Pen trig_line_pen(color_accent_amb, 2.0f * s);
                trig_line_pen.SetDashStyle(DashStyleDash);
                g.DrawLine(&trig_line_pen, trig_x, plot_y, trig_x, plot_y + plot_h);

                // Top chevron
                PointF chevron[3] = {
                    PointF(trig_x - 8.0f * s, plot_y),
                    PointF(trig_x + 8.0f * s, plot_y),
                    PointF(trig_x,            plot_y + 10.0f * s)
                };
                SolidBrush br_chev(color_accent_amb);
                g.FillPolygon(&br_chev, chevron, 3);
            }

            // ---- Ring-highlighted peak circle (makes the 1-px spike unmissable) ----
            {
                // Find the max-duration point in the retained timeline
                size_t peak_idx = 0;
                double peak_dur = -1.0;
                for (size_t i = 0; i < pt_count; ++i) {
                    if (report.frame_timeline[i].duration_ms > peak_dur) {
                        peak_dur = report.frame_timeline[i].duration_ms;
                        peak_idx = i;
                    }
                }

                // Prefer the anchor if it's the actual spike; else use the highest sample
                if (trig_idx < pt_count && report.frame_timeline[trig_idx].duration_ms >= peak_dur) {
                    peak_idx = trig_idx;
                    peak_dur = report.frame_timeline[trig_idx].duration_ms;
                }

                if (peak_dur > 0.0) {
                    const PointF& peak = line_pts[peak_idx];
                    SolidBrush ring_fill(color_accent_amb);
                    g.FillEllipse(&ring_fill, peak.X - 3.5f * s, peak.Y - 3.5f * s, 7.0f * s, 7.0f * s);

                    Pen ring_outer(Color(200, 245, 158, 11), 1.5f * s);
                    g.DrawEllipse(&ring_outer, peak.X - 8.0f * s, peak.Y - 8.0f * s, 16.0f * s, 16.0f * s);
                }
            }

            // ---- Floating peak callout badge ----
            {
                std::wstringstream peak_ss;
                peak_ss << std::fixed << std::setprecision(1) << report.trigger.duration_ms << L" ms";
                std::wstring peak_str = peak_ss.str();

                float callout_w = 76.0f * s;
                float callout_h = 24.0f * s;
                float callout_x = trig_x - (callout_w / 2.0f);
                if (callout_x < plot_x + 4.0f) callout_x = plot_x + 4.0f;
                if (callout_x + callout_w > plot_x + plot_w - 4.0f) {
                    callout_x = plot_x + plot_w - callout_w - 4.0f;
                }
                float callout_y = plot_y + 14.0f * s;

                RectF rc_callout(callout_x, callout_y, callout_w, callout_h);
                SolidBrush br_callout_bg(Color(240, 24, 30, 43));
                Pen pen_callout(color_accent_amb, 1.0f);
                fill_rounded_rect(g, rc_callout, 6.0f * s, &br_callout_bg, &pen_callout);

                SolidBrush br_peak_txt(color_accent_amb);
                g.DrawString(peak_str.c_str(), -1, &font_small_bold, rc_callout, &fmt_center, &br_peak_txt);
            }

            // ---- Dynamic X-axis milestone labels ----
            if (!report.frame_timeline.empty()) {
                std::wstring left_lbl   = detail::format_offset(report.frame_timeline.front().offset_from_trigger_ms);
                std::wstring center_lbl = L"Trigger (0 ms)";
                std::wstring right_lbl  = detail::format_offset(report.frame_timeline.back().offset_from_trigger_ms);

                float lbl_y = plot_y + plot_h + 4.0f * s;
                float lbl_h = 16.0f * s;

                RectF rc_left(plot_x, lbl_y, 80.0f * s, lbl_h);
                g.DrawString(left_lbl.c_str(), -1, &font_mono, rc_left, &fmt_left, &br_muted);

                RectF rc_center(trig_x - 60.0f * s, lbl_y, 120.0f * s, lbl_h);
                g.DrawString(center_lbl.c_str(), -1, &font_mono, rc_center, &fmt_center, &br_muted);

                RectF rc_right(plot_x + plot_w - 80.0f * s, lbl_y, 80.0f * s, lbl_h);
                g.DrawString(right_lbl.c_str(), -1, &font_mono, rc_right, &fmt_right, &br_muted);
            }
        }
    }

    // =========================================================================
    // 5. FOOTER (y: 636..670)
    // =========================================================================
    {
        const float footer_y = 636.0f * s;
        RectF rc_fl(side_margin, footer_y, 600.0f * s, 24.0f * s);
        g.DrawString(
            L"Stuttometer - Lightweight Real-Time Windows ETW Stutter & Glitch Diagnostic Utility",
            -1, &font_small, rc_fl, &fmt_left, &br_muted);

        RectF rc_fr(width - side_margin - 300.0f * s, footer_y, 300.0f * s, 24.0f * s);
        g.DrawString(L"github.com/xdr0p/stuttometer",
                     -1, &font_small, rc_fr, &fmt_right, &br_muted);
    }
}

// -----------------------------------------------------------------------------
// Public API (unchanged signatures & semantics)
// -----------------------------------------------------------------------------
bool CardRenderer::initialize() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_init_mutex);
        if (g_is_initialized) {
            return true; // Idempotent
        }
        if (g_ever_initialized) {
            return false; // One-shot: no re-init after shutdown
        }

        Gdiplus::GdiplusStartupInput input;
        input.GdiplusVersion = 1;
        input.DebugEventCallback = nullptr;
        input.SuppressBackgroundThread = FALSE;
        input.SuppressExternalCodecs = FALSE;

        if (Gdiplus::GdiplusStartup(&g_gdiplus_token, &input, nullptr) == Gdiplus::Ok) {
            g_is_initialized = true;
            g_ever_initialized = true;
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

void CardRenderer::shutdown() noexcept {
    try {
        std::lock_guard<std::mutex> render_lock(g_render_mutex);
        std::lock_guard<std::mutex> init_lock(g_init_mutex);
        if (g_is_initialized) {
            Gdiplus::GdiplusShutdown(g_gdiplus_token);
            g_is_initialized = false;
        }
    } catch (...) {
    }
}

bool CardRenderer::is_initialized() noexcept {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    return g_is_initialized;
}

std::vector<uint8_t> CardRenderer::render_card_to_png_bytes(
    const DiagnosticReport& report,
    const CardRenderOptions& options
) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_render_mutex);
        if (!is_initialized()) {
            return {};
        }

        if (options.base_width <= 0 || options.base_height <= 0 || options.dpi_scale <= 0.0) {
            return {};
        }

        int final_w = static_cast<int>(std::lround(options.base_width * options.dpi_scale));
        int final_h = static_cast<int>(std::lround(options.base_height * options.dpi_scale));
        if (final_w <= 0 || final_h <= 0) {
            return {};
        }

        std::vector<uint8_t> result;
        {
            Gdiplus::Bitmap bitmap(final_w, final_h, PixelFormat24bppRGB);
            if (bitmap.GetLastStatus() != Gdiplus::Ok) {
                return {};
            }

            {
                Gdiplus::Graphics g(&bitmap);
                if (g.GetLastStatus() != Gdiplus::Ok) {
                    return {};
                }
                draw_card(g, final_w, final_h, report, options);
            } // Flush Graphics

            IStream* pStream = nullptr;
            if (CreateStreamOnHGlobal(nullptr, TRUE, &pStream) == S_OK && pStream) {
                CLSID pngClsid;
                if (GetEncoderClsid(L"image/png", &pngClsid) != -1) {
                    if (bitmap.Save(pStream, &pngClsid, nullptr) == Gdiplus::Ok) {
                        STATSTG stat{};
                        if (SUCCEEDED(pStream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart > 0) {
                            if (stat.cbSize.QuadPart > MAXDWORD) { pStream->Release(); return {}; }
                            LARGE_INTEGER zero{};
                            zero.QuadPart = 0;
                            pStream->Seek(zero, STREAM_SEEK_SET, nullptr);
                            result.resize(static_cast<size_t>(stat.cbSize.QuadPart));
                            ULONG read = 0;
                            if (SUCCEEDED(pStream->Read(result.data(), static_cast<ULONG>(result.size()), &read))) {
                                result.resize(read);
                            }
                        }
                    }
                }
                pStream->Release();
            }
        }

        return result;
    } catch (...) {
        return {};
    }
}

bool CardRenderer::save_card_to_png(
    const DiagnosticReport& report,
    const std::filesystem::path& file_path,
    const CardRenderOptions& options
) noexcept {
    try {
        std::vector<uint8_t> bytes = render_card_to_png_bytes(report, options);
        if (bytes.empty()) {
            return false;
        }

        if (file_path.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(file_path.parent_path(), ec);
        }

        std::ofstream out(file_path, std::ios::binary);
        if (!out.is_open()) {
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        return out.good();
    } catch (...) {
        return false;
    }
}

bool CardRenderer::copy_card_to_clipboard(
    HWND owner_hwnd,
    const DiagnosticReport& report,
    const CardRenderOptions& options
) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_render_mutex);
        if (!is_initialized()) {
            return false;
        }

        if (options.base_width <= 0 || options.base_height <= 0 || options.dpi_scale <= 0.0) {
            return false;
        }

        int final_w = static_cast<int>(std::lround(options.base_width * options.dpi_scale));
        int final_h = static_cast<int>(std::lround(options.base_height * options.dpi_scale));
        if (final_w <= 0 || final_h <= 0) {
            return false;
        }

        Gdiplus::Bitmap bitmap(final_w, final_h, PixelFormat24bppRGB);
        if (bitmap.GetLastStatus() != Gdiplus::Ok) {
            return false;
        }

        {
            Gdiplus::Graphics g(&bitmap);
            if (g.GetLastStatus() != Gdiplus::Ok) {
                return false;
            }
            draw_card(g, final_w, final_h, report, options);
        } // Flush Graphics

        HBITMAP hbm = nullptr;
        if (bitmap.GetHBITMAP(Gdiplus::Color(0x11, 0x15, 0x1F), &hbm) != Gdiplus::Ok || !hbm) {
            return false;
        }

        DWORD row_stride = ((final_w * 3 + 3) & ~3);
        DWORD image_bytes = row_stride * final_h;
        DWORD total_size = sizeof(BITMAPINFOHEADER) + image_bytes;

        HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, total_size);
        if (!hGlobal) {
            DeleteObject(hbm);
            return false;
        }

        uint8_t* pBuf = static_cast<uint8_t*>(GlobalLock(hGlobal));
        if (!pBuf) {
            GlobalFree(hGlobal);
            DeleteObject(hbm);
            return false;
        }

        auto* bih = reinterpret_cast<BITMAPINFOHEADER*>(pBuf);
        std::memset(bih, 0, sizeof(BITMAPINFOHEADER));
        bih->biSize = sizeof(BITMAPINFOHEADER);
        bih->biWidth = final_w;
        bih->biHeight = final_h; // Positive => bottom-up
        bih->biPlanes = 1;
        bih->biBitCount = 24;
        bih->biCompression = BI_RGB;
        bih->biSizeImage = image_bytes;

        uint8_t* pPixels = pBuf + sizeof(BITMAPINFOHEADER);
        HDC screen_dc = GetDC(nullptr);
        BITMAPINFO bi{};
        bi.bmiHeader = *bih;
        int lines = GetDIBits(screen_dc, hbm, 0, final_h, pPixels, &bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, screen_dc);
        DeleteObject(hbm);
        GlobalUnlock(hGlobal);

        if (lines != final_h) {
            GlobalFree(hGlobal);
            return false;
        }

        if (!OpenClipboard(owner_hwnd)) {
            GlobalFree(hGlobal);
            return false;
        }
        EmptyClipboard();
        if (!SetClipboardData(CF_DIB, hGlobal)) {
            GlobalFree(hGlobal);
            CloseClipboard();
            return false;
        }
        CloseClipboard();
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace stuttometer::gui
