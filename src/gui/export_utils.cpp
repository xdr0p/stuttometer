#include "export_utils.hpp"
#include "gui_state.hpp"
#include "gui_helpers.hpp"
#include "card_renderer.hpp"
#include <stuttometer/json_reporter.hpp>
#include <stuttometer/session_bundle.hpp>
#include "gui_controller.hpp"
#include "stuttometer/internal/gui_constants.hpp"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>

namespace stuttometer::gui {

namespace {

bool collect_session_export_data(
    std::vector<DiagnosticReport>& reports,
    BenchmarkSummary& summary,
    bool redact
) {
    if (g_stutters.empty()) {
        return false;
    }

    reports.clear();
    reports.reserve(g_stutters.size());
    for (const auto& item : g_stutters) {
        if (item.report) {
            reports.push_back(*item.report);
        }
    }
    if (reports.empty()) {
        return false;
    }

    summary = BenchmarkSummary{};
    if (g_controller) {
        auto bench = g_controller->get_session_benchmark();
        if (bench) {
            summary = bench->get_summary(redact);
        }
    }
    return true;
}

} // namespace

void copy_selected_report_json(HWND hwnd) {
    std::vector<const DiagnosticReport*> selected_reports;
    if (g_h_list_stutters) {
        int lv_idx = -1;
        while ((lv_idx = ListView_GetNextItem(g_h_list_stutters, lv_idx, LVNI_SELECTED)) != -1) {
            LVITEMW lvi{};
            lvi.mask = LVIF_PARAM;
            lvi.iItem = lv_idx;
            if (ListView_GetItem(g_h_list_stutters, &lvi)) {
                const uint32_t id = static_cast<uint32_t>(lvi.lParam);
                for (const auto& rec : g_stutters) {
                    if (rec.id == id && rec.report) {
                        selected_reports.push_back(rec.report.get());
                        break;
                    }
                }
            }
        }
    }

    if (selected_reports.empty()) {
        if (g_selected_stutter_index < 0 || g_selected_stutter_index >= static_cast<int>(g_stutters.size())) {
            return;
        }
        const auto& item = g_stutters[g_selected_stutter_index];
        if (!item.report) {
            return;
        }
        selected_reports.push_back(item.report.get());
    }

    JsonReporter reporter;
    const bool redact = g_settings_config.redact;
    std::string json;
    if (selected_reports.size() == 1) {
        json = reporter.to_json_string(*selected_reports[0], redact, 2);
    } else {
        nlohmann::json arr = nlohmann::json::array();
        for (const DiagnosticReport* report : selected_reports) {
            arr.push_back(reporter.to_json(*report, redact));
        }
        json = arr.dump(2);
    }
    copy_utf8_to_clipboard(hwnd, json);
}

void copy_session_bundle(HWND hwnd) {
    const bool redact = g_settings_config.redact;
    std::vector<DiagnosticReport> reports;
    BenchmarkSummary summary{};
    if (!collect_session_export_data(reports, summary, redact)) {
        return;
    }

    std::string json;
    bool ok = SessionBundleExporter::export_session_string(json, reports, summary, redact);
    if (ok) {
        ok = copy_utf8_to_clipboard(hwnd, json);
    }

    show_button_feedback(hwnd, g_h_btn_copy, ok ? L"Copied \u2713" : L"Failed \u2715", gui_constants::BUTTON_FEEDBACK_MS);
}

void export_session_bundle(HWND hwnd) {
    const bool redact = g_settings_config.redact;
    std::vector<DiagnosticReport> reports;
    BenchmarkSummary summary{};
    if (!collect_session_export_data(reports, summary, redact)) {
        return;
    }

    wchar_t filename_buf[MAX_PATH] = L"stutto_session.json";
    if (prompt_save_file(hwnd, L"JSON Files (*.json)\0*.json\0All Files (*.*)\0*.*\0", L"json", filename_buf)) {
        const std::filesystem::path out_path(filename_buf);
        const bool ok = SessionBundleExporter::export_session(out_path, reports, summary, redact);
        show_button_feedback(hwnd, g_h_btn_export, ok ? L"Exported \u2713" : L"Failed \u2715", gui_constants::BUTTON_FEEDBACK_MS);
    }
}

void copy_selected_report_card(HWND hwnd) {
    if (g_selected_stutter_index < 0 || g_selected_stutter_index >= static_cast<int>(g_stutters.size())) {
        return;
    }

    const auto& item = g_stutters[g_selected_stutter_index];
    if (!item.report) {
        return;
    }
    CardRenderOptions opts;
    bool ok = CardRenderer::copy_card_to_clipboard(hwnd, *item.report, opts);

    show_button_feedback(hwnd, g_h_btn_copy_card, ok ? L"Copied \u2713" : L"Failed \u2715", gui_constants::BUTTON_FEEDBACK_MS);
}

void export_selected_report_card(HWND hwnd) {
    if (g_selected_stutter_index < 0 || g_selected_stutter_index >= static_cast<int>(g_stutters.size())) {
        return;
    }

    const auto& item = g_stutters[g_selected_stutter_index];
    if (!item.report) {
        return;
    }
    wchar_t filename_buf[MAX_PATH] = L"stutto_card.png";
    if (prompt_save_file(hwnd, L"PNG Image (*.png)\0*.png\0All Files (*.*)\0*.*\0", L"png", filename_buf)) {
        CardRenderOptions opts;
        bool ok = CardRenderer::save_card_to_png(*item.report, std::filesystem::path(filename_buf), opts);
        show_button_feedback(hwnd, g_h_btn_export_card, ok ? L"Exported \u2713" : L"Failed \u2715", gui_constants::BUTTON_FEEDBACK_MS);
    }
}

} // namespace stuttometer::gui
