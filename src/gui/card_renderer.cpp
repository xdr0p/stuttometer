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

    // Canvas Background: #11151f
    Color color_bg(255, 0x11, 0x15, 0x1F);
    Color color_card(255, 0x18, 0x1E, 0x2B);
    Color color_card_border(255, 0x23, 0x2D, 0x3F);
    Color color_inset_border(255, 0x2A, 0x35, 0x4B);
    Color color_text_pri(255, 0xF1, 0xF5, 0xF9);
    Color color_text_sec(255, 0x94, 0xA3, 0xB8);
    Color color_text_muted(255, 0x64, 0x74, 0x8B);

    // Attribution Colors
    Color color_attr;
    std::wstring attr_label;
    switch (report.attribution) {
        case AttributionTag::GAME_ENGINE:
            color_attr = Color(255, 245, 158, 11); // Amber #f59e0b
            attr_label = L"GAME ENGINE";
            break;
        case AttributionTag::DWM_COMPOSITION:
            color_attr = Color(255, 168, 85, 247); // Purple #a855f7
            attr_label = L"DWM COMPOSITION";
            break;
        case AttributionTag::EXTERNAL_CONTENTION:
            color_attr = Color(255, 239, 68, 68); // Crimson #ef4444
            attr_label = L"EXTERNAL CONTENTION";
            break;
        case AttributionTag::UNKNOWN:
        default:
            color_attr = Color(255, 100, 116, 139); // Slate #64748b
            attr_label = L"UNKNOWN";
            break;
    }

    // Fill Canvas
    SolidBrush bg_brush(color_bg);
    g.FillRectangle(&bg_brush, 0, 0, width, height);

    // 1px Inset Accent Border (#2a354b)
    Pen border_pen(color_inset_border, 1.0f);
    g.DrawRectangle(&border_pen, 0, 0, width - 1, height - 1);

    // Font families
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
    Font font_mono_bold(pMono, 10.0f * s, FontStyleBold, UnitPixel);

    StringFormat fmt_left;
    fmt_left.SetAlignment(StringAlignmentNear);
    fmt_left.SetLineAlignment(StringAlignmentCenter);
    fmt_left.SetTrimming(StringTrimmingEllipsisCharacter);

    StringFormat fmt_right;
    fmt_right.SetAlignment(StringAlignmentFar);
    fmt_right.SetLineAlignment(StringAlignmentCenter);

    StringFormat fmt_center;
    fmt_center.SetAlignment(StringAlignmentCenter);
    fmt_center.SetLineAlignment(StringAlignmentCenter);

    SolidBrush br_pri(color_text_pri);
    SolidBrush br_sec(color_text_sec);
    SolidBrush br_muted(color_text_muted);
    SolidBrush br_card(color_card);
    Pen pen_card(color_card_border, 1.0f);

    // -------------------------------------------------------------------------
    // 1. HEADER SECTION (y: 16 to 64)
    // -------------------------------------------------------------------------
    float top_margin = 16.0f * s;
    float side_margin = 24.0f * s;
    float content_w = width - (side_margin * 2.0f);

    // Version Badge Pill
    std::string ver = report.tool_version.empty() ? "0.4.0" : report.tool_version;
    std::wstring ver_badge = L"STUTTOMETER v" + to_wide_str(ver);
    RectF rc_ver(side_margin, top_margin, 130.0f * s, 22.0f * s);
    SolidBrush br_ver_bg(Color(255, 30, 41, 59));
    Pen pen_ver(Color(255, 51, 65, 85), 1.0f);
    g.FillRectangle(&br_ver_bg, rc_ver);
    g.DrawRectangle(&pen_ver, rc_ver.X, rc_ver.Y, rc_ver.Width, rc_ver.Height);
    SolidBrush br_ver_txt(Color(255, 56, 189, 248)); // Sky Blue
    g.DrawString(ver_badge.c_str(), -1, &font_tag, rc_ver, &fmt_center, &br_ver_txt);

    // Target Process Name (with redaction handling)
    std::string proc_name = report.redacted ? "Process_REDACTED" : report.target_process;
    if (proc_name.empty()) proc_name = "System Telemetry Event";
    std::wstring proc_w = to_wide_str(proc_name);
    RectF rc_proc(side_margin + 138.0f * s, top_margin - 2.0f * s, 600.0f * s, 26.0f * s);
    g.DrawString(proc_w.c_str(), -1, &font_title, rc_proc, &fmt_left, &br_pri);

    // Timestamp UTC (Right aligned)
    std::wstring time_w = to_wide_str(report.timestamp_utc.empty() ? "N/A" : report.timestamp_utc);
    RectF rc_time(width - side_margin - 300.0f * s, top_margin, 300.0f * s, 22.0f * s);
    g.DrawString(time_w.c_str(), -1, &font_mono, rc_time, &fmt_right, &br_muted);

    // -------------------------------------------------------------------------
    // 2. BLAME BANNER (y: 52 to 142)
    // -------------------------------------------------------------------------
    float banner_y = 48.0f * s;
    float banner_h = 92.0f * s;
    RectF rc_banner(side_margin, banner_y, content_w, banner_h);
    g.FillRectangle(&br_card, rc_banner);
    g.DrawRectangle(&pen_card, rc_banner.X, rc_banner.Y, rc_banner.Width, rc_banner.Height);

    // Accent left stripe
    SolidBrush br_attr(color_attr);
    g.FillRectangle(&br_attr, rc_banner.X, rc_banner.Y, 5.0f * s, rc_banner.Height);

    // Attribution Badge Pill
    RectF rc_pill(side_margin + 16.0f * s, banner_y + 12.0f * s, 160.0f * s, 24.0f * s);
    SolidBrush br_pill_bg(Color(45, color_attr.GetR(), color_attr.GetG(), color_attr.GetB()));
    Pen pen_pill(color_attr, 1.0f);
    g.FillRectangle(&br_pill_bg, rc_pill);
    g.DrawRectangle(&pen_pill, rc_pill.X, rc_pill.Y, rc_pill.Width, rc_pill.Height);
    SolidBrush br_attr_txt(color_attr);
    g.DrawString(attr_label.c_str(), -1, &font_tag, rc_pill, &fmt_center, &br_attr_txt);

    // Culprit Process / Driver
    std::string culprit = report.attribution_process;
    if (report.redacted || report.attribution_redacted) {
        culprit = get_redacted_module_name(culprit, true);
    }
    if (culprit.empty()) culprit = "Unattributed Anomaly";
    std::wstring culprit_w = to_wide_str(culprit);
    RectF rc_culprit(rc_pill.GetRight() + 14.0f * s, banner_y + 10.0f * s, 480.0f * s, 26.0f * s);
    g.DrawString(culprit_w.c_str(), -1, &font_callout, rc_culprit, &fmt_left, &br_pri);

    // Confidence Badge (Right side)
    double top_confidence = report.diagnoses.empty() ? 0.0 : report.diagnoses[0].confidence;
    std::wstringstream conf_ss;
    conf_ss << std::fixed << std::setprecision(0) << std::round(top_confidence * 100.0) << L"% CONFIDENCE";
    std::wstring conf_str = conf_ss.str();
    RectF rc_conf(width - side_margin - 180.0f * s, banner_y + 12.0f * s, 164.0f * s, 24.0f * s);
    SolidBrush br_conf_bg(Color(255, 30, 41, 59));
    Pen pen_conf(Color(255, 51, 65, 85), 1.0f);
    g.FillRectangle(&br_conf_bg, rc_conf);
    g.DrawRectangle(&pen_conf, rc_conf.X, rc_conf.Y, rc_conf.Width, rc_conf.Height);
    SolidBrush br_conf_txt(Color(255, 226, 232, 240));
    g.DrawString(conf_str.c_str(), -1, &font_small_bold, rc_conf, &fmt_center, &br_conf_txt);

    // Summary Text (Row 2 of Blame Banner)
    std::string summary = report.diagnoses.empty() ? "No conclusive diagnosis available for this event." : report.diagnoses[0].summary;
    if (report.redacted || report.attribution_redacted) {
        auto ids = collect_report_ids(report);
        summary = redact_text_with_ids(summary, ids);
    }
    std::wstring summary_w = to_wide_str(summary);
    RectF rc_sum(side_margin + 16.0f * s, banner_y + 44.0f * s, content_w - 32.0f * s, 22.0f * s);
    g.DrawString(summary_w.c_str(), -1, &font_regular, rc_sum, &fmt_left, &br_sec);

    // Telemetry Loss Warning (if any events dropped)
    uint64_t total_loss = report.dropped_events + report.producer_dropped_events + report.etw_events_lost;
    if (total_loss > 0) {
        std::wstringstream loss_ss;
        loss_ss << L"[!] Telemetry Loss: " << total_loss << L" event(s) dropped upstream";
        std::wstring loss_w = loss_ss.str();
        RectF rc_loss(side_margin + 16.0f * s, banner_y + 68.0f * s, content_w - 32.0f * s, 18.0f * s);
        SolidBrush br_loss(Color(255, 245, 158, 11)); // Warning Amber
        g.DrawString(loss_w.c_str(), -1, &font_small_bold, rc_loss, &fmt_left, &br_loss);
    }

    // -------------------------------------------------------------------------
    // 3. METRICS GRID (y: 150 to 226)
    // -------------------------------------------------------------------------
    float grid_y = 148.0f * s;
    float grid_h = 76.0f * s;
    float card_gap = 10.0f * s;
    float cell_w = (content_w - (card_gap * 3.0f)) / 4.0f;

    auto draw_metric_tile = [&](int index, const wchar_t* title, const std::wstring& main_val, const std::wstring& sub_val, Color val_color) {
        float cell_x = side_margin + index * (cell_w + card_gap);
        RectF rc_cell(cell_x, grid_y, cell_w, grid_h);
        g.FillRectangle(&br_card, rc_cell);
        g.DrawRectangle(&pen_card, rc_cell.X, rc_cell.Y, rc_cell.Width, rc_cell.Height);

        RectF rc_title(cell_x + 10.0f * s, grid_y + 8.0f * s, cell_w - 20.0f * s, 16.0f * s);
        g.DrawString(title, -1, &font_small_bold, rc_title, &fmt_left, &br_muted);

        SolidBrush br_val(val_color);
        RectF rc_main(cell_x + 10.0f * s, grid_y + 24.0f * s, cell_w - 20.0f * s, 26.0f * s);
        g.DrawString(main_val.c_str(), -1, &font_callout, rc_main, &fmt_left, &br_val);

        RectF rc_sub(cell_x + 10.0f * s, grid_y + 50.0f * s, cell_w - 20.0f * s, 18.0f * s);
        g.DrawString(sub_val.c_str(), -1, &font_small, rc_sub, &fmt_left, &br_muted);
    };

    // Metric 1: Duration
    std::wstring dur_main;
    std::wstring dur_sub;
    if (report.trigger.source == TriggerSource::AUDIO_GLITCH) {
        dur_main = L"Glitch (x" + std::to_wstring(report.trigger.glitch_count > 0 ? report.trigger.glitch_count : 1) + L")";
        dur_sub = L"Audio buffer underrun";
    } else {
        std::wstringstream dss;
        dss << std::fixed << std::setprecision(1) << report.trigger.duration_ms << L" ms";
        dur_main = dss.str();

        std::wstringstream sss;
        sss << std::fixed << std::setprecision(2) << report.trigger.spike_ratio << L"x spike ratio";
        dur_sub = sss.str();
    }
    draw_metric_tile(0, L"STALL DURATION", dur_main, dur_sub, Color(255, 239, 68, 68));

    // Metric 2: Effective Framerate (Section 4 Division Guard)
    std::wstring fps_main;
    std::wstring fps_sub;
    if (report.trigger.duration_ms <= 0.0) {
        fps_main = L"N/A";
        fps_sub = L"Zero frame duration";
    } else if (report.trigger.baseline_fps <= 0.0) {
        double stall_fps = 1000.0 / report.trigger.duration_ms;
        std::wstringstream fss;
        fss << L"N/A -> " << std::fixed << std::setprecision(1) << stall_fps << L" FPS";
        fps_main = fss.str();
        fps_sub = L"Baseline framerate unavailable";
    } else {
        double stall_fps = 1000.0 / report.trigger.duration_ms;
        std::wstringstream fss;
        fss << std::fixed << std::setprecision(1) << report.trigger.baseline_fps << L" -> " << stall_fps << L" FPS";
        fps_main = fss.str();
        fps_sub = L"Framerate drop";
    }
    draw_metric_tile(1, L"EFFECTIVE FRAMERATE", fps_main, fps_sub, Color(255, 245, 158, 11));

    // Metric 3: Trigger Reason
    std::wstring src_main = to_wide_str(trigger_reason_to_string(report.trigger.reason));
    std::wstring src_sub = to_wide_str(trigger_source_to_string(report.trigger.source));
    draw_metric_tile(2, L"TRIGGER CLASSIFICATION", src_main, src_sub, Color(255, 56, 189, 248));

    // Metric 4: Core & TID
    std::wstring core_main = L"CPU Core " + std::to_wstring(static_cast<int>(report.trigger.cpu_index));
    std::wstring tid_sub = report.redacted ? L"TID: [REDACTED]" : (L"TID: " + std::to_wstring(report.trigger.target_tid));
    draw_metric_tile(3, L"EXECUTION THREAD", core_main, tid_sub, Color(255, 148, 163, 184));

    // -------------------------------------------------------------------------
    // 4. FRAME GRAPH SECTION (y: 234 to 625)
    // -------------------------------------------------------------------------
    float graph_y = 232.0f * s;
    float graph_h = 398.0f * s;
    RectF rc_graph(side_margin, graph_y, content_w, graph_h);
    g.FillRectangle(&br_card, rc_graph);
    g.DrawRectangle(&pen_card, rc_graph.X, rc_graph.Y, rc_graph.Width, rc_graph.Height);

    // Graph Title & Header
    RectF rc_gh(side_margin + 14.0f * s, graph_y + 10.0f * s, 400.0f * s, 20.0f * s);
    g.DrawString(L"FRAME PACING TIMELINE (1,024 SAMPLES)", -1, &font_small_bold, rc_gh, &fmt_left, &br_muted);

    // Graph Plot Area
    float plot_x = side_margin + 60.0f * s;
    float plot_y = graph_y + 36.0f * s;
    float plot_w = content_w - 76.0f * s;
    float plot_h = graph_h - 60.0f * s;

    // Timeline Max Ms Determination
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

    // Horizontal Gridlines & Frametime Labels (16.6ms, 33.3ms, 50ms, etc.)
    double grid_steps[] = { 16.67, 33.33, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0 };
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

    // Baseline threshold reference line (Dashed Sky Blue)
    if (report.trigger.baseline_avg_ms > 0.0 && report.trigger.baseline_avg_ms < max_ms) {
        float y_base = plot_y + plot_h - static_cast<float>((report.trigger.baseline_avg_ms / max_ms) * plot_h);
        Pen base_pen(Color(180, 56, 189, 248), 1.0f);
        base_pen.SetDashStyle(DashStyleDot);
        g.DrawLine(&base_pen, plot_x, y_base, plot_x + plot_w, y_base);

        RectF rc_base_lbl(plot_x + plot_w - 120.0f * s, y_base - 14.0f * s, 116.0f * s, 14.0f * s);
        SolidBrush br_sky(Color(200, 56, 189, 248));
        g.DrawString(L"Baseline Avg", -1, &font_mono, rc_base_lbl, &fmt_right, &br_sky);
    }

    // Pacing Stall threshold reference line (Dashed Amber)
    if (report.present_threshold_ms > 0.0 && report.present_threshold_ms < max_ms) {
        float y_thresh = plot_y + plot_h - static_cast<float>((report.present_threshold_ms / max_ms) * plot_h);
        Pen thresh_pen(Color(180, 245, 158, 11), 1.0f);
        thresh_pen.SetDashStyle(DashStyleDash);
        g.DrawLine(&thresh_pen, plot_x, y_thresh, plot_x + plot_w, y_thresh);

        RectF rc_thresh_lbl(plot_x + 10.0f * s, y_thresh - 14.0f * s, 160.0f * s, 14.0f * s);
        SolidBrush br_amb(Color(200, 245, 158, 11));
        g.DrawString(L"Stutter Threshold", -1, &font_mono, rc_thresh_lbl, &fmt_left, &br_amb);
    }

    // Empty Timeline Fallback
    if (report.frame_timeline.empty()) {
        RectF rc_empty(plot_x, plot_y, plot_w, plot_h);
        g.DrawString(L"No frame timeline points recorded in retained window", -1, &font_regular, rc_empty, &fmt_center, &br_muted);
    } else {
        const size_t pt_count = report.frame_timeline.size();

        // Identify anchor / trigger point (relative_index == 0)
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

        float trig_x = plot_x + (pt_count > 1 ? (static_cast<float>(trig_idx) / static_cast<float>(pt_count - 1)) * plot_w : plot_w / 2.0f);

        // Pre-trigger vs post-trigger background split tint
        SolidBrush br_pre_tint(Color(12, 56, 189, 248)); // Faint Sky Blue
        SolidBrush br_post_tint(Color(12, 148, 163, 184)); // Faint Slate
        g.FillRectangle(&br_pre_tint, plot_x, plot_y, trig_x - plot_x, plot_h);
        g.FillRectangle(&br_post_tint, trig_x, plot_y, (plot_x + plot_w) - trig_x, plot_h);

        // Batch Timeline Conversion to PointF
        std::vector<PointF> line_pts(pt_count);
        for (size_t i = 0; i < pt_count; ++i) {
            float px = plot_x + (pt_count > 1 ? (static_cast<float>(i) / static_cast<float>(pt_count - 1)) * plot_w : plot_w / 2.0f);
            double d_clamp = std::clamp(report.frame_timeline[i].duration_ms, 0.0, max_ms);
            float py = plot_y + plot_h - static_cast<float>((d_clamp / max_ms) * plot_h);
            line_pts[i] = PointF(px, py);
        }

        // Fill area under curve
        if (pt_count > 1) {
            std::vector<PointF> poly_pts;
            poly_pts.reserve(pt_count + 2);
            poly_pts.push_back(PointF(line_pts[0].X, plot_y + plot_h));
            for (const auto& p : line_pts) {
                poly_pts.push_back(p);
            }
            poly_pts.push_back(PointF(line_pts.back().X, plot_y + plot_h));

            SolidBrush br_curve_fill(Color(35, 56, 189, 248));
            g.FillPolygon(&br_curve_fill, poly_pts.data(), static_cast<INT>(poly_pts.size()));
        }

        // Batch Draw Curve
        Pen curve_pen(Color(255, 56, 189, 248), 1.5f * s);
        if (pt_count > 1) {
            g.DrawLines(&curve_pen, line_pts.data(), static_cast<INT>(line_pts.size()));
        } else {
            g.FillEllipse(&br_pri, line_pts[0].X - 3.0f, line_pts[0].Y - 3.0f, 6.0f, 6.0f);
        }

        // Vertical Trigger Marker Line (Amber #f59e0b)
        Pen trig_line_pen(Color(255, 245, 158, 11), 1.5f * s);
        trig_line_pen.SetDashStyle(DashStyleDash);
        g.DrawLine(&trig_line_pen, trig_x, plot_y, trig_x, plot_y + plot_h);

        // Downward Chevron Indicator at Top
        PointF chevron[3] = {
            PointF(trig_x - 6.0f * s, plot_y),
            PointF(trig_x + 6.0f * s, plot_y),
            PointF(trig_x, plot_y + 8.0f * s)
        };
        SolidBrush br_chev(Color(255, 245, 158, 11));
        g.FillPolygon(&br_chev, chevron, 3);

        // Floating Callout Badge displaying Peak Ms
        std::wstringstream peak_ss;
        peak_ss << std::fixed << std::setprecision(1) << report.trigger.duration_ms << L" ms";
        std::wstring peak_str = peak_ss.str();

        float callout_w = 68.0f * s;
        float callout_h = 22.0f * s;
        float callout_x = trig_x - (callout_w / 2.0f);
        if (callout_x < plot_x + 4.0f) callout_x = plot_x + 4.0f;
        if (callout_x + callout_w > plot_x + plot_w - 4.0f) callout_x = plot_x + plot_w - callout_w - 4.0f;
        float callout_y = plot_y + 12.0f * s;

        RectF rc_callout(callout_x, callout_y, callout_w, callout_h);
        SolidBrush br_callout_bg(Color(240, 24, 30, 43));
        Pen pen_callout(Color(255, 245, 158, 11), 1.0f);
        g.FillRectangle(&br_callout_bg, rc_callout);
        g.DrawRectangle(&pen_callout, rc_callout.X, rc_callout.Y, rc_callout.Width, rc_callout.Height);

        SolidBrush br_peak_txt(Color(255, 245, 158, 11));
        g.DrawString(peak_str.c_str(), -1, &font_small_bold, rc_callout, &fmt_center, &br_peak_txt);
    }

    // -------------------------------------------------------------------------
    // 5. FOOTER SECTION (y: 636 to 670)
    // -------------------------------------------------------------------------
    float footer_y = 636.0f * s;
    RectF rc_fl(side_margin, footer_y, 600.0f * s, 24.0f * s);
    g.DrawString(L"Stuttometer - Lightweight Real-Time Windows ETW Stutter & Glitch Diagnostic Utility", -1, &font_small, rc_fl, &fmt_left, &br_muted);

    RectF rc_fr(width - side_margin - 300.0f * s, footer_y, 300.0f * s, 24.0f * s);
    g.DrawString(L"github.com/xdr0p/stuttometer", -1, &font_small, rc_fr, &fmt_right, &br_muted);
}

bool CardRenderer::initialize() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_init_mutex);
        if (g_is_initialized) {
            return true; // Idempotent re-entry
        }
        if (g_ever_initialized) {
            return false; // Re-initialization after shutdown is prohibited
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
                            // Guard against ULONG truncation on the Read() cast below.
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
