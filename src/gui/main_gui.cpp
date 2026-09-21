#include "gui_controller.hpp"
#include "card_renderer.hpp"
#include "osd_toast.hpp"
#include "benchmark_view.hpp"
#include "sound_cues.hpp"
#include "theme.hpp"
#include "gui_state.hpp"
#include "dark_controls.hpp"
#include "export_utils.hpp"
#include "settings_dialog.hpp"
#include "resource.h"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shellapi.h>

#include <vector>
#include <deque>
#include <string>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <memory>
#include <algorithm>
#include <iterator>
#include <cmath>
#include <cstring>
#include <numbers>
#include <mutex>

namespace stuttometer::gui {

// Fixed-width geometry helper for the live telemetry badge on the Action Toolbar
static RECT get_telemetry_badge_rect(int client_width) {
    const int act_y = scale_dpi(116);
    const int act_h = scale_dpi(32);
    const int badge_w = g_settings_config.enable_audio ? scale_dpi(270) : scale_dpi(210);
    const int badge_x = client_width - scale_dpi(16) - badge_w;
    return { badge_x, act_y, badge_x + badge_w, act_y + act_h };
}

// Forward declarations
static void update_inspector(int selected_index);
static void update_clear_button_state();

static void update_fonts_for_dpi(UINT dpi) {
    create_theme_fonts(dpi);
    apply_fonts_to_main_controls();

    // Propagate to open Settings Dialog controls
    if (g_h_settings_dlg && IsWindow(g_h_settings_dlg)) {
        settings_dialog_apply_fonts(g_h_settings_dlg);
    }
}

// Update UI Inspector with details of selected stutter report
static void update_inspector(int selected_index) {
    if (selected_index < 0 || selected_index >= static_cast<int>(g_stutters.size())) {
        SCROLLINFO si{ sizeof(SCROLLINFO), SIF_POS | SIF_RANGE };
        GetScrollInfo(g_h_edit_inspector, SB_VERT, &si);
        int prev_pos = si.nPos;

        std::wostringstream oss;
        oss << L"\r\n  No stutter event selected.\r\n\r\n";
        oss << L"  Select an entry from the list above to inspect root-cause analysis, confidence factors, and supporting evidence timeline.\r\n";
        if (!g_engine_logs.empty()) {
            oss << L"\r\n  ------------------------------------------------------------------------------\r\n";
            oss << L"   ENGINE LOGS & DIAGNOSTIC MESSAGES:\r\n";
            oss << L"  ------------------------------------------------------------------------------\r\n";
            for (const auto& log : g_engine_logs) {
                oss << L"   " << log << L"\r\n";
            }
        }
        SetWindowTextW(g_h_edit_inspector, oss.str().c_str());
        if (prev_pos > 0) {
            SendMessageW(g_h_edit_inspector, EM_LINESCROLL, 0, prev_pos);
        }
        EnableWindow(g_h_btn_export, FALSE);
        EnableWindow(g_h_btn_copy, FALSE);
        EnableWindow(g_h_btn_export_card, FALSE);
        EnableWindow(g_h_btn_copy_card, FALSE);
        return;
    }

    const auto& item = g_stutters[selected_index];
    const auto& r = *item.report;

    std::wostringstream oss;
    oss << L"\r\n";
    oss << L"  ==============================================================================\r\n";
    oss << L"   [DIAGNOSTIC REPORT INSPECTOR] Event #" << item.id << L" | " << utf8_to_wstring(r.timestamp_utc) << L"\r\n";
    oss << L"  ==============================================================================\r\n\r\n";

    if (r.redacted) {
        oss << L"  TARGET PROCESS:      [REDACTED] (PID: 0, TID: 0)\r\n";
    } else {
        oss << L"  TARGET PROCESS:      " << utf8_to_wstring(r.target_process) << L" (PID: " << r.trigger.target_pid << L", TID: " << r.trigger.target_tid << L")\r\n";
    }
    oss << L"  ATTRIBUTION:         " << utf8_to_wstring(attribution_to_string(r.attribution));
    if (r.attribution_redacted) {
        oss << L" (REDACTED)\r\n";
    } else if (r.attribution_pid != 0) {
        oss << L" (" << utf8_to_wstring(r.attribution_process) << L" PID " << r.attribution_pid << L")\r\n";
    } else {
        oss << L" (" << utf8_to_wstring(r.attribution_process) << L")\r\n";
    }
    if (!r.frame_timeline.empty()) {
        oss << L"  RETAINED FRAMES:     " << r.frame_timeline.size() << L"\r\n";
    }
    oss << L"  TRIGGER CAUSE:       " << utf8_to_wstring(trigger_source_to_string(r.trigger.source))
        << L" (" << utf8_to_wstring(trigger_reason_to_string(r.trigger.reason)) << L")\r\n";
    if (r.trigger.baseline_avg_ms > 0.0) {
        oss << L"  BASELINE DELIVERY:   " << std::fixed << std::setprecision(1) << r.trigger.baseline_fps << L" FPS (" 
            << r.trigger.baseline_avg_ms << L" ms/frame, " << r.trigger.spike_ratio << L"x spike)\r\n";
    }
    const double eff_thresh = r.present_threshold_ms + std::max(0.5, r.present_threshold_ms * 0.05);
    oss << L"  STUTTER DURATION:    " << std::fixed << std::setprecision(2) << r.trigger.duration_ms << L" ms (Nominal: " << r.present_threshold_ms << L" ms, Effective: " << eff_thresh << L" ms)\r\n";
    oss << L"  CAPTURE WINDOW:      " << r.window_pre_ms << L" ms pre-trigger / " << r.window_post_ms << L" ms post-trigger\r\n";
    oss << L"  PROVIDER TIER:       " << utf8_to_wstring(r.provider_tier) << (r.redacted ? L" [REDACTED]" : L"") << L"\r\n\r\n";

    if (r.diagnoses.empty()) {
        oss << L"  ------------------------------------------------------------------------------\r\n";
        oss << L"   ROOT-CAUSE DIAGNOSIS: No anomalies detected exceeding thresholds.\r\n";
        oss << L"  ------------------------------------------------------------------------------\r\n";
    } else {
        oss << L"  ------------------------------------------------------------------------------\r\n";
        oss << L"   RANKED ROOT-CAUSE DIAGNOSES:\r\n";
        oss << L"  ------------------------------------------------------------------------------\r\n";

        for (const auto& diag : r.diagnoses) {
            oss << L"   Rank #" << diag.rank << L": " << utf8_to_wstring(diag.hypothesis) << L"\r\n";
            oss << L"   Confidence: " << std::fixed << std::setprecision(1) << (diag.confidence * 100.0) << L"%\r\n";
            oss << L"   Summary:    " << utf8_to_wstring(diag.summary) << L"\r\n\r\n";

            if (!diag.evidence.empty()) {
                oss << L"     Supporting Evidence Timeline (" << diag.evidence.size() << L" items):\r\n";
                for (size_t i = 0; i < diag.evidence.size(); ++i) {
                    const auto& ev = diag.evidence[i];
                    oss << L"     [" << (i + 1) << L"] " << utf8_to_wstring(ev.event_type) << L" | Module: " << utf8_to_wstring(ev.driver_module);
                    if (ev.duration_us > 0) {
                        oss << L" | Duration: " << std::fixed << std::setprecision(2) << (ev.duration_us / 1000.0) << L" ms";
                    }
                    oss << L" | Core: " << static_cast<int>(ev.cpu_core);
                    oss << L" | Offset: " << std::showpos << std::fixed << std::setprecision(2) << ev.offset_from_trigger_ms << L" ms" << std::noshowpos;
                    if (!ev.extra_info.empty()) {
                        oss << L" | " << utf8_to_wstring(ev.extra_info);
                    }
                    oss << L"\r\n";
                }
                oss << L"\r\n";
            }
        }
    }

    oss << L"  ------------------------------------------------------------------------------\r\n";
    oss << L"   FLIGHT RECORDER METRICS:\r\n";
    oss << L"  ------------------------------------------------------------------------------\r\n";
    oss << L"   Total Events: " << r.total_events << L"\r\n";
    oss << L"   DPC: " << r.event_counts.dpc << L" | ISR: " << r.event_counts.isr << L" | Disk I/O: " << r.event_counts.disk << L" | CSwitch: " << r.event_counts.cswitch << L"\r\n";
    oss << L"   DXGI Present: " << r.event_counts.dxgi << L" | Audio: " << r.event_counts.audio << L"\r\n";
    oss << L"   Buffer Drops: " << r.dropped_events << L" | Upstream Lost: " << r.etw_events_lost << L"\r\n";
    oss << L"   Evictions: " << r.unpaired_evictions << L" | Ins Failures: " << r.insertion_failures << L"\r\n";

    SetWindowTextW(g_h_edit_inspector, oss.str().c_str());
    EnableWindow(g_h_btn_export, TRUE);
    EnableWindow(g_h_btn_copy, TRUE);
    bool can_card = CardRenderer::is_initialized();
    EnableWindow(g_h_btn_export_card, can_card ? TRUE : FALSE);
    EnableWindow(g_h_btn_copy_card, can_card ? TRUE : FALSE);
}

// Insert new report item into UI
static void handle_new_report(std::unique_ptr<DiagnosticReport> report) {
    if (!report) return;

    StutterRecord rec{};
    rec.id = g_next_stutter_id++;
    rec.timestamp = report->timestamp_utc;
    rec.process_name = report->target_process.empty() ? ("PID:" + std::to_string(report->trigger.target_pid)) : report->target_process;
    std::string trig_str = std::string(trigger_source_to_string(report->trigger.source));
    if (report->trigger.reason != TriggerReason::NONE && report->trigger.reason != TriggerReason::STATIC_THRESHOLD) {
        trig_str += " (" + std::string(trigger_reason_to_string(report->trigger.reason)) + ")";
    }
    rec.trigger_reason = trig_str;
    rec.duration_ms = report->trigger.duration_ms;

    if (!report->diagnoses.empty()) {
        rec.top_hypothesis = report->diagnoses[0].hypothesis;
        rec.confidence = report->diagnoses[0].confidence;
    } else {
        rec.top_hypothesis = "none";
        rec.confidence = 0.0;
    }

    if (report->trigger.source == TriggerSource::AUDIO_GLITCH) {
        g_session_audio_count++;
    } else {
        g_session_stutter_count++;
    }

    rec.report = std::move(report);
    g_stutters.push_back(std::move(rec));

    constexpr size_t MAX_STUTTER_HISTORY = 500;
    if (g_stutters.size() > MAX_STUTTER_HISTORY) {
        ListView_DeleteItem(g_h_list_stutters, 0);
        g_stutters.pop_front();
        if (g_selected_stutter_index > 0) {
            --g_selected_stutter_index;
            if (g_h_list_stutters && IsWindow(g_h_list_stutters)) {
                ListView_SetItemState(g_h_list_stutters, g_selected_stutter_index,
                                      LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            }
        } else if (g_selected_stutter_index == 0) {
            g_selected_stutter_index = -1;
            update_inspector(-1);
        }
    }

    int new_index = static_cast<int>(g_stutters.size() - 1);
    g_has_received_data = true;

    LVITEMW lvi{};
    lvi.mask = LVIF_TEXT | LVIF_PARAM;
    lvi.iItem = new_index;
    lvi.iSubItem = 0;
    std::wstring id_str = std::to_wstring(g_stutters[new_index].id);
    lvi.pszText = id_str.data();
    lvi.lParam = static_cast<LPARAM>(g_stutters[new_index].id);

    ListView_InsertItem(g_h_list_stutters, &lvi);

    std::wstring w_time = utf8_to_wstring(g_stutters[new_index].timestamp);
    std::wstring w_proc = utf8_to_wstring(g_stutters[new_index].process_name);
    std::wstring w_trig = utf8_to_wstring(g_stutters[new_index].trigger_reason);

    std::wstring w_dur;
    if (g_stutters[new_index].report && g_stutters[new_index].report->trigger.source == TriggerSource::AUDIO_GLITCH) {
        w_dur = L"Glitch (x" + std::to_wstring(g_stutters[new_index].report->trigger.glitch_count) + L")";
    } else {
        std::wostringstream oss_dur;
        oss_dur << std::fixed << std::setprecision(1) << g_stutters[new_index].duration_ms << L" ms";
        w_dur = oss_dur.str();
    }

    std::wstring w_diag = utf8_to_wstring(g_stutters[new_index].top_hypothesis);
    std::wstring w_conf = std::to_wstring(static_cast<int>(g_stutters[new_index].confidence * 100.0 + 0.5)) + L"%";

    // ListView_SetItemText and LVM_INSERTITEM synchronously copy pszText into control-managed
    // memory during message processing, so stack-local .data() is safe without lifetime extension.
    ListView_SetItemText(g_h_list_stutters, new_index, 1, w_time.data());
    ListView_SetItemText(g_h_list_stutters, new_index, 2, w_proc.data());
    ListView_SetItemText(g_h_list_stutters, new_index, 3, w_trig.data());
    ListView_SetItemText(g_h_list_stutters, new_index, 4, w_dur.data());
    ListView_SetItemText(g_h_list_stutters, new_index, 5, w_diag.data());
    ListView_SetItemText(g_h_list_stutters, new_index, 6, w_conf.data());

    const bool should_follow = (g_selected_stutter_index < 0 || g_selected_stutter_index == new_index - 1);
    if (should_follow) {
        ListView_SetItemState(g_h_list_stutters, new_index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_h_list_stutters, new_index, FALSE);
        g_selected_stutter_index = new_index;
        update_inspector(new_index);
    }
    update_clear_button_state();
    update_metrics_text();

    g_status_text = L"Stutter Captured (" + w_proc + L")";
    RECT client_rc;
    GetClientRect(g_hwnd_main, &client_rc);
    int pill_w = scale_dpi(280);
    int pill_left = (client_rc.right - pill_w) / 2;
    int pill_top = scale_dpi(8);
    int pill_bottom = pill_top + scale_dpi(32);
    RECT rc_pill = { pill_left - scale_dpi(2), pill_top - scale_dpi(2), pill_left + pill_w + scale_dpi(2), pill_bottom + scale_dpi(2) };
    InvalidateRect(g_hwnd_main, &rc_pill, FALSE);

    RECT rc_b = get_telemetry_badge_rect(client_rc.right - client_rc.left);
    RECT rc_inv = { rc_b.left - scale_dpi(2), rc_b.top - scale_dpi(2), rc_b.right + scale_dpi(2), rc_b.bottom + scale_dpi(2) };
    InvalidateRect(g_hwnd_main, &rc_inv, FALSE);
}

static void update_clear_button_state() {
    bool is_idle = (g_current_session_state == GuiSessionState::IDLE);
    bool has_data = g_has_received_data || !g_stutters.empty() || !g_engine_logs.empty();
    EnableWindow(g_h_btn_clear, (is_idle && has_data) ? TRUE : FALSE);
}

// Clear history with explicit ListView invalidation
static void clear_stutter_history() {
    ListView_DeleteAllItems(g_h_list_stutters);
    g_stutters.clear();
    g_engine_logs.clear();
    g_has_received_data = false;
    g_selected_stutter_index = -1;
    g_session_stutter_count = 0;
    g_session_audio_count = 0;
    g_capture_start_tick = 0;
    g_capture_elapsed_seconds = 0;
    update_metrics_text();
    update_inspector(-1);
    update_clear_button_state();
    InvalidateRect(g_h_list_stutters, NULL, TRUE);
    InvalidateRect(g_hwnd_main, NULL, FALSE);
}

// Populate Process Combobox
static void handle_processes_updated(std::unique_ptr<ProcessList> procs) {
    if (!procs) return;

    if (*procs == g_cached_processes) {
        return; // List is identical, zero UI churn or listbox flicker
    }

    bool same_identities = (procs->size() == g_cached_processes.size());
    if (same_identities) {
        for (size_t i = 0; i < procs->size(); ++i) {
            if ((*procs)[i].pid != g_cached_processes[i].pid || (*procs)[i].name != g_cached_processes[i].name) {
                same_identities = false;
                break;
            }
        }
    }

    if (same_identities) {
        int cur_sel = static_cast<int>(SendMessageW(g_h_combo_process, CB_GETCURSEL, 0, 0));
        for (size_t i = 0; i < procs->size(); ++i) {
            const auto& p = (*procs)[i];
            if (p.window_title != g_cached_processes[i].window_title) {
                g_cached_processes[i].window_title = p.window_title;
                std::wstring item_str = p.name + (p.window_title.empty() ? L"" : L" (" + p.window_title.substr(0, 24) + L")");
                int item_idx = static_cast<int>(i + 1);
                SendMessageW(g_h_combo_process, CB_DELETESTRING, item_idx, 0);
                SendMessageW(g_h_combo_process, CB_INSERTSTRING, item_idx, reinterpret_cast<LPARAM>(item_str.c_str()));
                SendMessageW(g_h_combo_process, CB_SETITEMDATA, item_idx, static_cast<LPARAM>(i));
            }
        }
        if (cur_sel != CB_ERR) {
            SendMessageW(g_h_combo_process, CB_SETCURSEL, static_cast<WPARAM>(cur_sel), 0);
        }
        return;
    }

    std::wstring selected_proc_name;
    int cur_sel = static_cast<int>(SendMessageW(g_h_combo_process, CB_GETCURSEL, 0, 0));
    if (cur_sel > 0) {
        LRESULT data = SendMessageW(g_h_combo_process, CB_GETITEMDATA, cur_sel, 0);
        if (data >= 0 && data < static_cast<LRESULT>(g_cached_processes.size())) {
            selected_proc_name = g_cached_processes[data].name;
        }
    }

    g_cached_processes = *procs;

    SendMessageW(g_h_combo_process, CB_RESETCONTENT, 0, 0);
    int all_idx = static_cast<int>(SendMessageW(g_h_combo_process, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All Processes (System-Wide)")));
    SendMessageW(g_h_combo_process, CB_SETITEMDATA, all_idx, static_cast<LPARAM>(-1));

    std::wstring last_target_w = utf8_to_wstring(g_settings_last_target_process);
    int found_idx = CB_ERR;
    for (size_t i = 0; i < g_cached_processes.size(); ++i) {
        const auto& p = g_cached_processes[i];
        std::wstring item = p.name;
        if (!p.window_title.empty()) {
            item += L" (" + p.window_title.substr(0, 24) + L")";
        }
        int item_idx = static_cast<int>(SendMessageW(g_h_combo_process, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str())));
        SendMessageW(g_h_combo_process, CB_SETITEMDATA, item_idx, static_cast<LPARAM>(i));

        if (!selected_proc_name.empty() && _wcsicmp(p.name.c_str(), selected_proc_name.c_str()) == 0) {
            found_idx = item_idx;
        } else if (found_idx == CB_ERR && !last_target_w.empty() && _wcsicmp(p.name.c_str(), last_target_w.c_str()) == 0) {
            found_idx = item_idx;
        }
    }

    if (found_idx != CB_ERR) {
        SendMessageW(g_h_combo_process, CB_SETCURSEL, static_cast<WPARAM>(found_idx), 0);
        if (g_controller && g_controller->is_capturing() && !g_settings_last_target_process.empty()) {
            auto cfg = read_gui_config();
            g_controller->update_target_filter(cfg.target_pid, cfg.target_process_name);
        }
    } else {
        SendMessageW(g_h_combo_process, CB_SETCURSEL, 0, 0);
    }
    g_settings_last_target_process.clear();
    InvalidateRect(g_h_combo_process, NULL, TRUE);

    // If the dropdown list is currently dropped open, dynamically resize the listbox window
    // so no black unpainted gap or incorrect scroll height remains
    if (SendMessageW(g_h_combo_process, CB_GETDROPPEDSTATE, 0, 0)) {
        COMBOBOXINFO cbi = { sizeof(COMBOBOXINFO) };
        if (GetComboBoxInfo(g_h_combo_process, &cbi) && cbi.hwndList && IsWindow(cbi.hwndList)) {
            RECT rc_list;
            GetWindowRect(cbi.hwndList, &rc_list);
            int item_h = static_cast<int>(SendMessageW(g_h_combo_process, CB_GETITEMHEIGHT, 0, 0));
            if (item_h <= 0) item_h = scale_dpi(22);
            int total_items = static_cast<int>(g_cached_processes.size()) + 1; // +1 for "All Processes"
            int max_visible = 10;
            int visible_items = (total_items > max_visible) ? max_visible : total_items;
            int new_h = visible_items * item_h + scale_dpi(2);
            SetWindowPos(cbi.hwndList, NULL, 0, 0, rc_list.right - rc_list.left, new_h,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(cbi.hwndList, NULL, TRUE);
        }
    }
}

// Update session state
static void update_session_ui_state(GuiSessionState state) {
    GuiSessionState prev_state = g_current_session_state;
    g_current_session_state = state;

    // Play non-blocking synthesized audible confirmation cue if sound cues are enabled
    if (g_sound_cues_enabled) {
        if ((state == GuiSessionState::RUNNING || state == GuiSessionState::DEGRADED_USER_ONLY || state == GuiSessionState::DEGRADED_KERNEL_ONLY) &&
            (prev_state == GuiSessionState::IDLE || prev_state == GuiSessionState::STARTING)) {
            play_capture_sound(true);
        } else if (state == GuiSessionState::IDLE && prev_state != GuiSessionState::IDLE) {
            play_capture_sound(false);
        }
    }

    if (state == GuiSessionState::STARTING || state == GuiSessionState::RUNNING || state == GuiSessionState::DEGRADED_USER_ONLY || state == GuiSessionState::DEGRADED_KERNEL_ONLY) {
        if (g_capture_start_tick == 0) {
            g_capture_start_tick = GetTickCount64();
            g_capture_elapsed_seconds = 0;
            g_session_stutter_count = 0;
            g_session_audio_count = 0;
            update_metrics_text();
        }
    } else if (state == GuiSessionState::IDLE || state == GuiSessionState::STOPPING) {
        if (g_capture_start_tick != 0) {
            g_capture_elapsed_seconds = (GetTickCount64() - g_capture_start_tick) / 1000;
            g_capture_start_tick = 0;
            update_metrics_text();
        }
    }

    if (g_h_btn_session_summary) {
        EnableWindow(g_h_btn_session_summary, TRUE);
    }

    switch (state) {
        case GuiSessionState::IDLE:
            EnableWindow(g_h_btn_start, TRUE);
            EnableWindow(g_h_btn_stop, FALSE);
            EnableWindow(g_h_btn_settings, TRUE);
            EnableWindow(g_h_combo_process, TRUE);
            g_status_text = L"IDLE (Ready to monitor)";
            break;

        case GuiSessionState::STARTING:
            EnableWindow(g_h_btn_start, FALSE);
            EnableWindow(g_h_btn_stop, FALSE);
            EnableWindow(g_h_btn_settings, FALSE);
            EnableWindow(g_h_combo_process, FALSE);
            g_status_text = L"Starting Trace Sessions...";
            break;

        case GuiSessionState::RUNNING:
            EnableWindow(g_h_btn_start, FALSE);
            EnableWindow(g_h_btn_stop, TRUE);
            EnableWindow(g_h_btn_settings, FALSE);
            EnableWindow(g_h_combo_process, FALSE);
            g_status_text = L"CAPTURING (Kernel + User Active)";
            break;

        case GuiSessionState::DEGRADED_USER_ONLY:
            EnableWindow(g_h_btn_start, FALSE);
            EnableWindow(g_h_btn_stop, TRUE);
            EnableWindow(g_h_btn_settings, FALSE);
            EnableWindow(g_h_combo_process, FALSE);
            g_status_text = L"DEGRADED (User-Only DXGI/Audio)";
            break;

        case GuiSessionState::DEGRADED_KERNEL_ONLY:
            EnableWindow(g_h_btn_start, FALSE);
            EnableWindow(g_h_btn_stop, TRUE);
            EnableWindow(g_h_btn_settings, FALSE);
            EnableWindow(g_h_combo_process, FALSE);
            g_status_text = L"DEGRADED (Kernel-Only DPC/CSwitch)";
            break;

        case GuiSessionState::STOPPING:
            EnableWindow(g_h_btn_start, FALSE);
            EnableWindow(g_h_btn_stop, FALSE);
            EnableWindow(g_h_btn_settings, FALSE);
            EnableWindow(g_h_combo_process, FALSE);
            g_status_text = L"Stopping Trace Sessions...";
            break;
    }
    update_clear_button_state();
    if (g_h_list_stutters && ListView_GetItemCount(g_h_list_stutters) == 0) {
        InvalidateRect(g_h_list_stutters, NULL, TRUE);
    }
    InvalidateRect(g_hwnd_main, NULL, FALSE);
}

// Auto-fit ListView Column Widths
static void auto_fit_listview_columns(HWND hList, int list_width) {
    if (!hList || list_width <= 0) return;

    const int col_id_w     = scale_dpi(42);
    const int col_time_w   = scale_dpi(140);
    const int col_proc_w   = scale_dpi(145);
    const int col_trig_w   = scale_dpi(140);
    const int col_dur_w    = scale_dpi(120);
    const int col_conf_w   = scale_dpi(90);

    int total_fixed = col_id_w + col_time_w + col_proc_w + col_trig_w + col_dur_w + col_conf_w;
    int scrollbar_w = GetSystemMetrics(SM_CXVSCROLL);
    int col_diag_w = std::max(scale_dpi(200), list_width - total_fixed - scrollbar_w - scale_dpi(4));

    ListView_SetColumnWidth(hList, 0, col_id_w);
    ListView_SetColumnWidth(hList, 1, col_time_w);
    ListView_SetColumnWidth(hList, 2, col_proc_w);
    ListView_SetColumnWidth(hList, 3, col_trig_w);
    ListView_SetColumnWidth(hList, 4, col_dur_w);
    ListView_SetColumnWidth(hList, 5, col_diag_w);
    ListView_SetColumnWidth(hList, 6, col_conf_w);
}

// Layout child controls with strict DPI scaling and dynamic sizing
static void layout_controls(HWND /*hwnd*/, int width, int height) {
    int min_w = scale_dpi(1000);
    int min_h = scale_dpi(640);
    if (width < min_w) width = min_w;
    if (height < min_h) height = min_h;

    int margin = scale_dpi(16);

    // 0. Header Settings & Session Summary Buttons (Y: 8, Height: 32)
    int elem_h = scale_dpi(32);
    int elem_y = scale_dpi(8);
    int btn_set_w = scale_dpi(105);
    int btn_set_x = width - margin - btn_set_w;
    MoveWindow(g_h_btn_settings, btn_set_x, elem_y, btn_set_w, elem_h, TRUE);

    int btn_sum_w = scale_dpi(145);
    int btn_sum_x = btn_set_x - scale_dpi(8) - btn_sum_w;
    MoveWindow(g_h_btn_session_summary, btn_sum_x, elem_y, btn_sum_w, elem_h, TRUE);

    // 1. Configuration Card (Y: 54, Height: 52)
    const int card_y = scale_dpi(54);
    const int card_h = scale_dpi(52);
    const int ctrl_h = scale_dpi(26);
    const int ctrl_y = card_y + (card_h - ctrl_h) / 2; // Precise vertical center (Y: 67)
    const int lbl_y = ctrl_y;
    const int lbl_h = ctrl_h;
    const int gap_lbl_box = scale_dpi(8);

    // Target Process controls (Balanced dropdown width)
    const int col1_x = margin + scale_dpi(12);
    const int lbl_target_w = scale_dpi(48);
    const int combo_proc_w = scale_dpi(235);

    MoveWindow(g_h_lbl_target, col1_x, lbl_y, lbl_target_w, lbl_h, TRUE);
    MoveWindow(g_h_combo_process, col1_x + lbl_target_w + gap_lbl_box, ctrl_y + scale_dpi(2), combo_proc_w, scale_dpi(250), TRUE);

    // 2. Action Toolbar (Y: 116, Height: 32)
    int act_y = scale_dpi(116);
    int act_h = scale_dpi(32);
    int bx = margin;
    const int btn_w = scale_dpi(88);
    const int btn_gap = scale_dpi(8);

    MoveWindow(g_h_btn_start, bx, act_y, btn_w, act_h, TRUE);
    bx += btn_w + btn_gap;
    MoveWindow(g_h_btn_stop, bx, act_y, btn_w, act_h, TRUE);
    bx += btn_w + btn_gap;
    MoveWindow(g_h_btn_clear, bx, act_y, scale_dpi(75), act_h, TRUE);

    // 3. Stutter Events Table (ListView) and Diagnostic Inspector Card
    int content_top = act_y + act_h + scale_dpi(10);
    int content_bottom = height - margin;
    int total_avail = content_bottom - content_top;

    int min_table_h = scale_dpi(110);
    int min_insp_h = scale_dpi(150);

    int list_w = width - margin * 2;
    int list_h = (total_avail * 44) / 100;
    if (list_h < min_table_h) list_h = min_table_h;

    MoveWindow(g_h_list_stutters, margin, content_top, list_w, list_h, TRUE);
    auto_fit_listview_columns(g_h_list_stutters, list_w);

    // Inspector Card Section (with Integrated Header)
    int insp_card_y = content_top + list_h + scale_dpi(10);
    int insp_card_h = content_bottom - insp_card_y;
    if (insp_card_h < min_insp_h) insp_card_h = min_insp_h;

    int insp_hdr_h = scale_dpi(34);
    int btn_quick_w = scale_dpi(96);
    int btn_quick_h = scale_dpi(24);
    int btn_quick_y = insp_card_y + (insp_hdr_h - btn_quick_h) / 2;
    int btn_pair_gap = scale_dpi(4);
    int group_gap = scale_dpi(10);

    // Two paired pill groups: [Copy JSON | Export JSON]  [Copy Card | Export Card]
    int btn_exp_card_x = margin + list_w - scale_dpi(8) - btn_quick_w;
    int btn_cpy_card_x = btn_exp_card_x - btn_pair_gap - btn_quick_w;
    int btn_exp_json_x = btn_cpy_card_x - group_gap - btn_quick_w;
    int btn_cpy_json_x = btn_exp_json_x - btn_pair_gap - btn_quick_w;

    MoveWindow(g_h_btn_export_card, btn_exp_card_x, btn_quick_y, btn_quick_w, btn_quick_h, TRUE);
    MoveWindow(g_h_btn_copy_card, btn_cpy_card_x, btn_quick_y, btn_quick_w, btn_quick_h, TRUE);
    MoveWindow(g_h_btn_export, btn_exp_json_x, btn_quick_y, btn_quick_w, btn_quick_h, TRUE);
    MoveWindow(g_h_btn_copy, btn_cpy_json_x, btn_quick_y, btn_quick_w, btn_quick_h, TRUE);

    // Inset the multi-line edit control inside the Inspector card
    int edit_margin = scale_dpi(8);
    int insp_edit_x = margin + edit_margin;
    int insp_edit_y = insp_card_y + insp_hdr_h;
    int insp_edit_w = list_w - edit_margin * 2;
    int insp_edit_h = insp_card_h - insp_hdr_h - edit_margin;

    MoveWindow(g_h_edit_inspector, insp_edit_x, insp_edit_y, insp_edit_w, insp_edit_h, TRUE);
}

// Window Procedure
LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        // INTENTIONAL DESIGN DECISION: Fixed Window Dimensions
        // The GUI uses a carefully crafted, high-density dashboard layout. Window resizing is 
        // deliberately locked to the exact DPI-scaled base size (1020x660 @ 96 DPI) using ptMinTrackSize 
        // and ptMaxTrackSize to prevent layout distortion and maintain pixel-perfect visual aesthetics.
        case WM_GETMINMAXINFO: {
            LPMINMAXINFO mmi = (LPMINMAXINFO)lParam;
            UINT dpi = GetDpiForWindow(hwnd);
            if (dpi == 0) dpi = 96;
            int base_w = MulDiv(1020, dpi, 96);
            int base_h = MulDiv(660, dpi, 96);
            RECT rc = { 0, 0, base_w, base_h };
            DWORD dwStyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
            DWORD dwExStyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            AdjustWindowRectEx(&rc, dwStyle, FALSE, dwExStyle);
            int win_w = rc.right - rc.left;
            int win_h = rc.bottom - rc.top;
            mmi->ptMinTrackSize.x = win_w;
            mmi->ptMinTrackSize.y = win_h;
            mmi->ptMaxTrackSize.x = win_w;
            mmi->ptMaxTrackSize.y = win_h;
            return 0;
        }

        case WM_LBUTTONDOWN: {
            if (GetFocus() != hwnd) {
                SetFocus(hwnd);
            }
            break;
        }

        case WM_ERASEBKGND:
            return 1; // Double buffering handled cleanly in WM_PAINT

        case WM_CREATE: {
            g_hwnd_main = hwnd;
            g_is_admin = is_running_as_admin();
            apply_window_dark_titlebar(hwnd);
            apply_control_dark_theme(hwnd);

            // Initialize GDI Theme Cache
            g_theme.init();

            // Initialize DPI and Fonts
            UINT init_dpi = GetDpiForWindow(hwnd);
            if (init_dpi == 0) init_dpi = 96;
            update_fonts_for_dpi(init_dpi);

            g_controller = std::make_unique<GuiController>(hwnd);

            // Header Settings Button
            g_h_btn_settings = CreateWindowExW(0, L"BUTTON", L"Settings", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_SETTINGS, NULL, NULL);
            SetPropW(g_h_btn_settings, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::SecondarySlate));
            SendMessageW(g_h_btn_settings, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            SetWindowSubclass(g_h_btn_settings, DarkButtonSubclassProc, IDC_BTN_SETTINGS, 0);

            // Header Session Summary Button
            g_h_btn_session_summary = CreateWindowExW(0, L"BUTTON", L"Session Summary", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_SESSION_SUMMARY, NULL, NULL);
            SetPropW(g_h_btn_session_summary, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::SecondarySlate));
            SendMessageW(g_h_btn_session_summary, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            SetWindowSubclass(g_h_btn_session_summary, DarkButtonSubclassProc, IDC_BTN_SESSION_SUMMARY, 0);

            // Configuration Controls
            g_h_lbl_target = CreateWindowExW(0, L"STATIC", L"Target:", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            SendMessageW(g_h_lbl_target, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(g_h_lbl_target);

            g_h_combo_process = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, scale_dpi(235), scale_dpi(250), hwnd, (HMENU)(INT_PTR)IDC_COMBO_PROCESS, NULL, NULL);
            SendMessageW(g_h_combo_process, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            SendMessageW(g_h_combo_process, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scale_dpi(20));
            SendMessageW(g_h_combo_process, CB_SETITEMHEIGHT, (WPARAM)0, (LPARAM)scale_dpi(22));
            SendMessageW(g_h_combo_process, CB_SETMINVISIBLE, 10, 0);
            apply_control_dark_theme(g_h_combo_process);
            SetWindowSubclass(g_h_combo_process, DarkComboSubclassProc, 0, 0);

            // Action Buttons
            g_h_btn_start = CreateWindowExW(0, L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_START, NULL, NULL);
            SetPropW(g_h_btn_start, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::PrimaryEmerald));

            g_h_btn_stop = CreateWindowExW(0, L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_STOP, NULL, NULL);
            SetPropW(g_h_btn_stop, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::DangerRed));
            EnableWindow(g_h_btn_stop, FALSE);

            g_h_btn_clear = CreateWindowExW(0, L"BUTTON", L"Clear", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_CLEAR, NULL, NULL);
            SetPropW(g_h_btn_clear, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::SecondarySlate));
            EnableWindow(g_h_btn_clear, FALSE);

            // Inspector Header Action Buttons
            g_h_btn_copy_card = CreateWindowExW(0, L"BUTTON", L"Copy Card", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_COPY_CARD, NULL, NULL);
            SetPropW(g_h_btn_copy_card, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::QuickAction));
            SetPropW(g_h_btn_copy_card, L"OnCard", reinterpret_cast<HANDLE>(1));
            EnableWindow(g_h_btn_copy_card, FALSE);

            g_h_btn_export_card = CreateWindowExW(0, L"BUTTON", L"Export Card", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_EXPORT_CARD, NULL, NULL);
            SetPropW(g_h_btn_export_card, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::QuickAction));
            SetPropW(g_h_btn_export_card, L"OnCard", reinterpret_cast<HANDLE>(1));
            EnableWindow(g_h_btn_export_card, FALSE);

            g_h_btn_copy = CreateWindowExW(0, L"BUTTON", L"Copy JSON", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_COPY_JSON, NULL, NULL);
            SetPropW(g_h_btn_copy, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::QuickAction));
            SetPropW(g_h_btn_copy, L"OnCard", reinterpret_cast<HANDLE>(1));
            EnableWindow(g_h_btn_copy, FALSE);

            g_h_btn_export = CreateWindowExW(0, L"BUTTON", L"Export JSON", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_BTN_EXPORT_JSON, NULL, NULL);
            SetPropW(g_h_btn_export, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::QuickAction));
            SetPropW(g_h_btn_export, L"OnCard", reinterpret_cast<HANDLE>(1));
            EnableWindow(g_h_btn_export, FALSE);

            SetWindowSubclass(g_h_btn_start, DarkButtonSubclassProc, IDC_BTN_START, 0);
            SetWindowSubclass(g_h_btn_stop, DarkButtonSubclassProc, IDC_BTN_STOP, 0);
            SetWindowSubclass(g_h_btn_clear, DarkButtonSubclassProc, IDC_BTN_CLEAR, 0);
            SetWindowSubclass(g_h_btn_copy_card, DarkButtonSubclassProc, IDC_BTN_COPY_CARD, 0);
            SetWindowSubclass(g_h_btn_export_card, DarkButtonSubclassProc, IDC_BTN_EXPORT_CARD, 0);
            SetWindowSubclass(g_h_btn_copy, DarkButtonSubclassProc, IDC_BTN_COPY_JSON, 0);
            SetWindowSubclass(g_h_btn_export, DarkButtonSubclassProc, IDC_BTN_EXPORT_JSON, 0);

            HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
            if (!g_osd_toast.create(hInst)) {
                OutputDebugStringA("[STUTTOMETER] Warning: Failed to create OsdToast window.\n");
            }

            // Stutter Events ListView (LVS_EX_DOUBLEBUFFER: High throughput, flicker-free)
            g_h_list_stutters = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_VSCROLL, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_LIST_STUTTERS, NULL, NULL);
            SendMessageW(g_h_list_stutters, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            apply_control_dark_theme(g_h_list_stutters);
            SetWindowSubclass(g_h_list_stutters, ListViewSubclassProc, IDC_LIST_STUTTERS, 0);

            ListView_SetBkColor(g_h_list_stutters, COLOR_LIST_BG);
            ListView_SetTextBkColor(g_h_list_stutters, COLOR_LIST_BG);
            ListView_SetTextColor(g_h_list_stutters, COLOR_TEXT_PRI);
            ListView_SetExtendedListViewStyle(g_h_list_stutters, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

            g_h_list_header = ListView_GetHeader(g_h_list_stutters);
            if (g_h_list_header) {
                apply_control_dark_theme(g_h_list_header);
                SetWindowSubclass(g_h_list_header, HeaderSubclassProc, 0, 0);
            }

            struct ColumnDef {
                int fmt;
                int width_px;
                wchar_t title[32]; // Fixed stack buffer bounds column names at compile time with zero heap allocation
            };
            ColumnDef cols[] = {
                { LVCFMT_CENTER, scale_dpi(42),  L"#" },
                { LVCFMT_LEFT,   scale_dpi(140), L"Time (UTC)" },
                { LVCFMT_LEFT,   scale_dpi(145), L"Target Process" },
                { LVCFMT_LEFT,   scale_dpi(140), L"Trigger Reason" },
                { LVCFMT_RIGHT,  scale_dpi(120), L"Duration" },
                { LVCFMT_LEFT,   scale_dpi(280), L"Primary Culprit / Hypothesis" },
                { LVCFMT_RIGHT,  scale_dpi(90),  L"Confidence" }
            };

            for (size_t i = 0; i < std::size(cols); ++i) {
                LVCOLUMNW col{};
                col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
                col.fmt = cols[i].fmt;
                col.cx = cols[i].width_px;
                col.pszText = cols[i].title; // Stack-local wchar_t[] matches LPWSTR directly with zero casts
                ListView_InsertColumn(g_h_list_stutters, static_cast<int>(i), &col);
            }

            // Inspector Multi-line Viewer (Clean High-Contrast Monospace Dark Pane)
            g_h_edit_inspector = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_EDIT_INSPECTOR, NULL, NULL);
            SendMessageW(g_h_edit_inspector, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
            SendMessageW(g_h_edit_inspector, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
            apply_control_dark_theme(g_h_edit_inspector);
            SetWindowSubclass(g_h_edit_inspector, ReportInspectorSubclassProc, IDC_EDIT_INSPECTOR, 0);

            // Load persistent user configuration after controls are created
            load_user_settings();
            update_metrics_text();

            // Register Hotkey with loaded configuration
            UnregisterHotKey(hwnd, ID_HOTKEY_TOGGLE_CAPTURE);
            if (!RegisterHotKey(hwnd, ID_HOTKEY_TOGGLE_CAPTURE, g_hotkey_mods | MOD_NOREPEAT, g_hotkey_vk)) {
                g_status_text = L"Hotkey Warning: " + format_hotkey_display(g_hotkey_mods, g_hotkey_vk) + L" is in use by another app";
                append_engine_log(L"[WARN] Hotkey " + format_hotkey_display(g_hotkey_mods, g_hotkey_vk) + L" is in use by another application.");
            }

            RECT init_rc{};
            GetClientRect(hwnd, &init_rc);
            layout_controls(hwnd, init_rc.right, init_rc.bottom);

            update_inspector(-1);
            g_controller->enumerate_graphical_processes_async();
            return 0;
        }

        case WM_SIZE: {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            layout_controls(hwnd, width, height);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT pdis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
            if (pdis->CtlType == ODT_BUTTON) {
                draw_custom_button(pdis);
                return TRUE;
            }
            break;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT client_rc;
            GetClientRect(hwnd, &client_rc);
            int width = client_rc.right;
            int height = client_rc.bottom;

            HDC mem_dc = CreateCompatibleDC(hdc);
            HBITMAP mem_bmp = CreateCompatibleBitmap(hdc, width, height);
            HBITMAP old_bmp = static_cast<HBITMAP>(SelectObject(mem_dc, mem_bmp));
            HBRUSH old_br = static_cast<HBRUSH>(GetCurrentObject(mem_dc, OBJ_BRUSH));
            HFONT old_font = static_cast<HFONT>(GetCurrentObject(mem_dc, OBJ_FONT));
            HPEN old_pen = static_cast<HPEN>(GetCurrentObject(mem_dc, OBJ_PEN));

            RECT dummy{};

            // 1. Fill Main Canvas Background
            FillRect(mem_dc, &client_rc, g_theme.br_bg);

            // 2. Top Header Strip (Y: 0 to 48)
            const int hdr_h = scale_dpi(48);
            RECT header_rc = { 0, 0, width, hdr_h };
            if (IntersectRect(&dummy, &header_rc, &ps.rcPaint)) {
                FillRect(mem_dc, &header_rc, g_theme.br_header);

                SelectObject(mem_dc, g_theme.pen_header_border);
                MoveToEx(mem_dc, 0, hdr_h, NULL);
                LineTo(mem_dc, width, hdr_h);

                SetBkMode(mem_dc, TRANSPARENT);

                // Brand Title & Tagline
                SelectObject(mem_dc, g_font_title);
                SetTextColor(mem_dc, COLOR_TEXT_PRI);
                RECT title_rc = { scale_dpi(16), scale_dpi(6), scale_dpi(280), scale_dpi(28) };
                DrawTextW(mem_dc, L"STUTTOMETER", -1, &title_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                SelectObject(mem_dc, g_font_ui_sm_bold);
                SetTextColor(mem_dc, COLOR_TEXT_MUTED);
                RECT subtitle_rc = { scale_dpi(16), scale_dpi(27), scale_dpi(280), scale_dpi(45) };
                DrawTextW(mem_dc, L"REAL-TIME ETW DIAGNOSTIC", -1, &subtitle_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                // Real-Time Status Pill (True Horizontal Center)
                int elem_h = scale_dpi(32);
                int elem_y = scale_dpi(8);
                int pill_w = scale_dpi(280);
                int pill_left = (width - pill_w) / 2;
                int pill_right = pill_left + pill_w;
                RECT pill_rc = { pill_left, elem_y, pill_right, elem_y + elem_h };

                SelectObject(mem_dc, g_theme.br_pill);
                SelectObject(mem_dc, g_theme.pen_pill_border);
                RoundRect(mem_dc, pill_rc.left, pill_rc.top, pill_rc.right, pill_rc.bottom, scale_dpi(12), scale_dpi(12));

                // Status Dot: Universal Unicode Filled Circle via ClearType
                COLORREF dot_color = COLOR_TEXT_MUTED;
                if (g_current_session_state == GuiSessionState::RUNNING) {
                    dot_color = COLOR_ACCENT_EMERALD;
                } else if (g_current_session_state == GuiSessionState::DEGRADED_USER_ONLY || g_current_session_state == GuiSessionState::DEGRADED_KERNEL_ONLY || g_current_session_state == GuiSessionState::STARTING || g_current_session_state == GuiSessionState::STOPPING) {
                    dot_color = COLOR_ACCENT_AMB;
                }

                SelectObject(mem_dc, g_font_ui_bold);

                // True horizontal centering of combined (dot + gap + text) group
                RECT calc_rc = { 0, 0, 0, 0 };
                DrawTextW(mem_dc, g_status_text.c_str(), -1, &calc_rc, DT_CALCRECT | DT_SINGLELINE);
                int text_w = calc_rc.right - calc_rc.left;
                int dot_w = scale_dpi(14);
                int gap = scale_dpi(6);
                int total_content_w = dot_w + gap + text_w;
                int centered_x = pill_rc.left + (pill_w - total_content_w) / 2;
                int start_x = std::max<int>(static_cast<int>(pill_rc.left) + scale_dpi(10), centered_x);

                SetTextColor(mem_dc, dot_color);
                RECT dot_rc = { start_x, pill_rc.top, start_x + dot_w, pill_rc.bottom };
                DrawTextW(mem_dc, L"\u25CF", -1, &dot_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                SetTextColor(mem_dc, COLOR_TEXT_PRI);
                RECT status_text_rc = { start_x + dot_w + gap, pill_rc.top, pill_rc.right - scale_dpi(10), pill_rc.bottom };
                DrawTextW(mem_dc, g_status_text.c_str(), -1, &status_text_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            }

            // 3. Unified Configuration Command Bar (Y: 54, Height: 52)
            int card_y = scale_dpi(54);
            int card_h = scale_dpi(52);
            RECT cfg_card_rc = { scale_dpi(16), card_y, width - scale_dpi(16), card_y + card_h };
            if (IntersectRect(&dummy, &cfg_card_rc, &ps.rcPaint)) {
                SelectObject(mem_dc, g_theme.br_card);
                SelectObject(mem_dc, g_theme.pen_card_border);
                RoundRect(mem_dc, cfg_card_rc.left, cfg_card_rc.top, cfg_card_rc.right, cfg_card_rc.bottom, scale_dpi(12), scale_dpi(12));

                // Right-aligned Trigger Mode badge pill inside Configuration Card
                std::wstring mode_status_text;
                if (g_settings_config.frame_trigger_mode == FrameTriggerMode::HYBRID) {
                    mode_status_text = L"Trigger Mode: Hybrid (Auto Pacing & Judder)";
                } else if (g_settings_config.frame_trigger_mode == FrameTriggerMode::DYNAMIC_ONLY) {
                    mode_status_text = L"Trigger Mode: Dynamic Only (Relative & Judder)";
                } else {
                    int fps_val = static_cast<int>(std::round((g_settings_config.present_threshold_ms > 0.0) ? (1000.0 / g_settings_config.present_threshold_ms) : 60.0));
                    mode_status_text = L"Trigger Mode: Static (" + std::to_wstring(fps_val) + L" FPS Floor)";
                }

                SetBkMode(mem_dc, TRANSPARENT);
                SelectObject(mem_dc, g_font_ui_bold);
                SetTextColor(mem_dc, COLOR_TEXT_MUTED);

                RECT calc_mode_rc = { 0, 0, 0, 0 };
                DrawTextW(mem_dc, mode_status_text.c_str(), -1, &calc_mode_rc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
                int mode_text_w = calc_mode_rc.right - calc_mode_rc.left;
                int badge_mode_w = mode_text_w + scale_dpi(24);
                int badge_mode_h = scale_dpi(28);
                int badge_mode_y = card_y + (card_h - badge_mode_h) / 2;
                int badge_mode_x = cfg_card_rc.right - scale_dpi(12) - badge_mode_w;
                RECT badge_mode_rc = { badge_mode_x, badge_mode_y, badge_mode_x + badge_mode_w, badge_mode_y + badge_mode_h };

                SelectObject(mem_dc, g_theme.br_badge);
                SelectObject(mem_dc, g_theme.pen_badge_border);
                RoundRect(mem_dc, badge_mode_rc.left, badge_mode_rc.top, badge_mode_rc.right, badge_mode_rc.bottom, scale_dpi(8), scale_dpi(8));

                int mode_start_x = badge_mode_rc.left + (badge_mode_w - mode_text_w) / 2;
                RECT text_mode_rc = { mode_start_x, badge_mode_rc.top, mode_start_x + mode_text_w, badge_mode_rc.bottom };
                DrawTextW(mem_dc, mode_status_text.c_str(), -1, &text_mode_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }

            // 4. Live Telemetry Badge (Right Aligned on Action Toolbar)
            RECT badge_rc = get_telemetry_badge_rect(width);
            if (IntersectRect(&dummy, &badge_rc, &ps.rcPaint)) {
                SelectObject(mem_dc, g_theme.br_card);
                SelectObject(mem_dc, g_theme.pen_card_border);
                RoundRect(mem_dc, badge_rc.left, badge_rc.top, badge_rc.right, badge_rc.bottom, scale_dpi(10), scale_dpi(10));

                SetBkMode(mem_dc, TRANSPARENT);
                SelectObject(mem_dc, g_font_ui);
                SetTextColor(mem_dc, COLOR_TEXT_MUTED);

                RECT calc_rc = { 0, 0, 0, 0 };
                DrawTextW(mem_dc, g_metrics_text.c_str(), -1, &calc_rc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
                int text_w = calc_rc.right - calc_rc.left;
                int badge_w = badge_rc.right - badge_rc.left;
                int start_x = badge_rc.left + (badge_w - text_w) / 2;
                RECT text_rc = { start_x, badge_rc.top, start_x + text_w, badge_rc.bottom };
                DrawTextW(mem_dc, g_metrics_text.c_str(), -1, &text_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }

            // 5. Diagnostic Report Inspector Card Background
            RECT inspector_rect;
            GetWindowRect(g_h_edit_inspector, &inspector_rect);
            POINT pt_insp = { inspector_rect.left, inspector_rect.top };
            ScreenToClient(hwnd, &pt_insp);

            int insp_card_outer_y = pt_insp.y - scale_dpi(34);
            int insp_card_outer_bottom = client_rc.bottom - scale_dpi(16);
            RECT insp_card_box = { scale_dpi(16), insp_card_outer_y, width - scale_dpi(16), insp_card_outer_bottom };

            if (IntersectRect(&dummy, &insp_card_box, &ps.rcPaint)) {
                SelectObject(mem_dc, g_theme.br_card);
                SelectObject(mem_dc, g_theme.pen_card_border);
                RoundRect(mem_dc, insp_card_box.left, insp_card_box.top, insp_card_box.right, insp_card_box.bottom, scale_dpi(12), scale_dpi(12));

                // Inspector Header Divider Line
                SelectObject(mem_dc, g_theme.pen_card_divider);
                MoveToEx(mem_dc, insp_card_box.left, pt_insp.y - scale_dpi(4), NULL);
                LineTo(mem_dc, insp_card_box.right, pt_insp.y - scale_dpi(4));

                // Inspector Header Title & Status Badges
                SetBkMode(mem_dc, TRANSPARENT);
                SelectObject(mem_dc, g_font_ui_bold);

                std::wstring insp_label = L"DIAGNOSTIC REPORT INSPECTOR";
                if (g_selected_stutter_index >= 0 && g_selected_stutter_index < static_cast<int>(g_stutters.size())) {
                    insp_label += L" \u2014 Event #" + std::to_wstring(g_stutters[g_selected_stutter_index].id) + L" (" + utf8_to_wstring(g_stutters[g_selected_stutter_index].process_name) + L")";
                }

                // Calculate action buttons start boundary (4 buttons x 96px + gaps)
                int quick_total_w = scale_dpi(96 * 4 + 4 * 2 + 10 + 8);
                int quick_start_x = insp_card_box.right - quick_total_w;
                int first_badge_left = quick_start_x;

                if (g_selected_stutter_index >= 0 && g_selected_stutter_index < static_cast<int>(g_stutters.size())) {
                    const auto& rec = g_stutters[g_selected_stutter_index];
                    std::wstring tag_text = utf8_to_wstring(attribution_to_string(rec.report ? rec.report->attribution : AttributionTag::UNKNOWN));
                    COLORREF tag_color = get_attribution_color(rec.report ? rec.report->attribution : AttributionTag::UNKNOWN);

                    wchar_t conf_text[32];
                    swprintf_s(conf_text, L"%.1f%% Conf", rec.confidence * 100.0);
                    COLORREF conf_color = (rec.confidence >= 0.80) ? COLOR_ACCENT_EMERALD : ((rec.confidence >= 0.50) ? COLOR_ACCENT_AMB : COLOR_TEXT_MUTED);

                    std::wstring frames_text = std::to_wstring(rec.report ? rec.report->frame_timeline.size() : 0) + L" Frames";

                    RECT rc_calc{};
                    DrawTextW(mem_dc, tag_text.c_str(), -1, &rc_calc, DT_CALCRECT | DT_SINGLELINE);
                    int w_tag = (rc_calc.right - rc_calc.left) + scale_dpi(16);

                    rc_calc = {0, 0, 0, 0};
                    DrawTextW(mem_dc, conf_text, -1, &rc_calc, DT_CALCRECT | DT_SINGLELINE);
                    int w_conf = (rc_calc.right - rc_calc.left) + scale_dpi(16);

                    rc_calc = {0, 0, 0, 0};
                    DrawTextW(mem_dc, frames_text.c_str(), -1, &rc_calc, DT_CALCRECT | DT_SINGLELINE);
                    int w_frames = (rc_calc.right - rc_calc.left) + scale_dpi(16);

                    int badge_h = scale_dpi(22);
                    int badge_y = insp_card_outer_y + (scale_dpi(34) - badge_h) / 2;
                    int gap_b = scale_dpi(6);
                    int total_b_w = w_tag + gap_b + w_conf + gap_b + w_frames;

                    first_badge_left = quick_start_x - scale_dpi(14) - total_b_w;

                    // Draw Frame Count Badge
                    int cur_bx = first_badge_left;
                    RECT b_frames_rc = { cur_bx, badge_y, cur_bx + w_frames, badge_y + badge_h };
                    SelectObject(mem_dc, g_theme.br_badge);
                    SelectObject(mem_dc, g_theme.pen_badge_border);
                    RoundRect(mem_dc, b_frames_rc.left, b_frames_rc.top, b_frames_rc.right, b_frames_rc.bottom, scale_dpi(6), scale_dpi(6));
                    SetTextColor(mem_dc, COLOR_TEXT_MUTED);
                    DrawTextW(mem_dc, frames_text.c_str(), -1, &b_frames_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    cur_bx += w_frames + gap_b;

                    // Draw Confidence Badge
                    RECT b_conf_rc = { cur_bx, badge_y, cur_bx + w_conf, badge_y + badge_h };
                    RoundRect(mem_dc, b_conf_rc.left, b_conf_rc.top, b_conf_rc.right, b_conf_rc.bottom, scale_dpi(6), scale_dpi(6));
                    SetTextColor(mem_dc, conf_color);
                    DrawTextW(mem_dc, conf_text, -1, &b_conf_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    cur_bx += w_conf + gap_b;

                    // Draw Attribution Tag Badge
                    RECT b_tag_rc = { cur_bx, badge_y, cur_bx + w_tag, badge_y + badge_h };
                    RoundRect(mem_dc, b_tag_rc.left, b_tag_rc.top, b_tag_rc.right, b_tag_rc.bottom, scale_dpi(6), scale_dpi(6));
                    SetTextColor(mem_dc, tag_color);
                    DrawTextW(mem_dc, tag_text.c_str(), -1, &b_tag_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }

                RECT insp_hdr_text_rc = { insp_card_box.left + scale_dpi(12), insp_card_outer_y, first_badge_left - scale_dpi(10), pt_insp.y - scale_dpi(4) };
                SetTextColor(mem_dc, COLOR_TEXT_LABEL);
                DrawTextW(mem_dc, insp_label.c_str(), -1, &insp_hdr_text_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }

            SelectObject(mem_dc, old_pen);
            SelectObject(mem_dc, old_font);
            SelectObject(mem_dc, old_br);

            BitBlt(hdc, 0, 0, width, height, mem_dc, 0, 0, SRCCOPY);
            SelectObject(mem_dc, old_bmp);
            DeleteObject(mem_bmp);
            DeleteDC(mem_dc);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ACTIVATEAPP: {
            if (wParam /* activating */ && g_controller && !g_controller->is_capturing()) {
                static auto last_scan = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_scan).count() > 1500) {
                    last_scan = now;
                    g_controller->enumerate_graphical_processes_async();
                }
            }
            break;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            int wmEvent = HIWORD(wParam);

            if ((HWND)lParam == g_h_combo_process && wmEvent == CBN_SELCHANGE) {
                InvalidateRect((HWND)lParam, NULL, TRUE);
            }

            switch (wmId) {
                case IDC_BTN_SESSION_SUMMARY: {
                    if (g_controller) {
                        ShowBenchmarkView(hwnd, g_controller->get_session_benchmark(), g_settings_config.redact, []() {
                            return g_controller && g_controller->is_capturing();
                        });
                    }
                    break;
                }

                case IDC_BTN_SETTINGS: {
                    ShowSettingsDialog(hwnd);
                    break;
                }

                case IDC_BTN_START: {
                    auto cfg = read_gui_config();
                    if (!g_controller->start_session_async(cfg)) {
                        MessageBoxW(hwnd, L"Capture session is already active or starting.", L"Capture Notice", MB_OK | MB_ICONINFORMATION);
                    }
                    break;
                }

                case IDC_BTN_STOP: {
                    g_controller->stop_session_async();
                    break;
                }

                case IDC_BTN_CLEAR: {
                    clear_stutter_history();
                    break;
                }

                case IDC_BTN_EXPORT_JSON: {
                    export_selected_report_json(hwnd);
                    break;
                }

                case IDC_BTN_COPY_JSON: {
                    copy_selected_report_json(hwnd);
                    break;
                }

                case IDC_BTN_EXPORT_CARD: {
                    export_selected_report_card(hwnd);
                    break;
                }

                case IDC_BTN_COPY_CARD: {
                    copy_selected_report_card(hwnd);
                    break;
                }

                case IDC_COMBO_PROCESS: {
                    if (wmEvent == CBN_SELCHANGE) {
                        auto cfg = read_gui_config();
                        g_controller->update_target_filter(cfg.target_pid, cfg.target_process_name);
                    } else if (wmEvent == CBN_DROPDOWN) {
                        if (g_controller && !g_controller->is_capturing()) {
                            g_controller->enumerate_graphical_processes_async();
                        }
                    }
                    break;
                }
            }
            return 0;
        }

        case WM_NOTIFY: {
            LPNMHDR pnm = reinterpret_cast<LPNMHDR>(lParam);
            if (pnm->idFrom == IDC_LIST_STUTTERS) {
                if (pnm->code == LVN_ITEMCHANGED) {
                    LPNMLISTVIEW pnmlv = reinterpret_cast<LPNMLISTVIEW>(lParam);
                    if ((pnmlv->uChanged & LVIF_STATE) && (pnmlv->uNewState & LVIS_SELECTED)) {
                        g_selected_stutter_index = pnmlv->iItem;
                        update_inspector(g_selected_stutter_index);
                        if (g_h_edit_inspector && IsWindow(g_h_edit_inspector)) {
                            RECT client_rc{};
                            GetClientRect(hwnd, &client_rc);
                            RECT insp_wnd_rc{};
                            GetWindowRect(g_h_edit_inspector, &insp_wnd_rc);
                            POINT pt_insp = { insp_wnd_rc.left, insp_wnd_rc.top };
                            ScreenToClient(hwnd, &pt_insp);
                            RECT rc_hdr = { scale_dpi(16), pt_insp.y - scale_dpi(34), client_rc.right - scale_dpi(16), pt_insp.y };
                            InvalidateRect(hwnd, &rc_hdr, TRUE);
                        }
                    }
                } else if (pnm->code == NM_CUSTOMDRAW) {
                    LPNMLVCUSTOMDRAW pCustomDraw = reinterpret_cast<LPNMLVCUSTOMDRAW>(lParam);
                    switch (pCustomDraw->nmcd.dwDrawStage) {
                        case CDDS_PREPAINT:
                            return CDRF_NOTIFYITEMDRAW;

                        case CDDS_ITEMPREPAINT:
                            return CDRF_NOTIFYSUBITEMDRAW | CDRF_NOTIFYPOSTPAINT;

                        case CDDS_SUBITEM | CDDS_ITEMPREPAINT: {
                            int item_idx = static_cast<int>(pCustomDraw->nmcd.dwItemSpec);
                            int sub_idx  = pCustomDraw->iSubItem;
                            bool is_selected = (ListView_GetItemState(g_h_list_stutters, item_idx, LVIS_SELECTED) & LVIS_SELECTED) != 0;

                            if (is_selected) {
                                pCustomDraw->clrTextBk = COLOR_LIST_SEL;
                                pCustomDraw->clrText = RGB(255, 255, 255);
                            } else {
                                pCustomDraw->clrTextBk = (item_idx % 2 == 0) ? COLOR_LIST_BG : COLOR_LIST_ROW_ALT;
                                pCustomDraw->clrText = COLOR_TEXT_BRIGHT;

                                if (item_idx >= 0 && item_idx < static_cast<int>(g_stutters.size())) {
                                    const auto& rec = g_stutters[item_idx];
                                    if (sub_idx == 4) { // Duration
                                        if (!rec.report) {
                                            pCustomDraw->clrText = COLOR_SEV_NORMAL;
                                            return CDRF_DODEFAULT;
                                        }
                                        MetricSeverity sev = classify_severity(rec.report->trigger, rec.report->present_threshold_ms);
                                        pCustomDraw->clrText = get_severity_color(sev);
                                    } else if (sub_idx == 6) { // Confidence
                                        if (rec.confidence >= 0.80) {
                                            pCustomDraw->clrText = COLOR_ACCENT_EMERALD;
                                        } else if (rec.confidence >= 0.50) {
                                            pCustomDraw->clrText = COLOR_ACCENT_AMB;
                                        } else {
                                            pCustomDraw->clrText = COLOR_TEXT_MUTED;
                                        }
                                    }
                                }
                            }
                            return CDRF_DODEFAULT;
                        }

                        case CDDS_ITEMPOSTPAINT: {
                            int item_idx = static_cast<int>(pCustomDraw->nmcd.dwItemSpec);
                            if (item_idx >= 0 && item_idx < static_cast<int>(g_stutters.size())) {
                                const auto& rec = g_stutters[item_idx];
                                HBRUSH h_stripe_br = get_attribution_brush(rec.report ? rec.report->attribution : AttributionTag::UNKNOWN);
                                RECT rc_stripe = pCustomDraw->nmcd.rc;
                                rc_stripe.right = rc_stripe.left + scale_dpi(3);
                                FillRect(pCustomDraw->nmcd.hdc, &rc_stripe, h_stripe_br);
                            }
                            return CDRF_DODEFAULT;
                        }
                    }
                }
            }
            break;
        }

        case WM_TIMER: {
            if (wParam == reinterpret_cast<UINT_PTR>(g_h_btn_copy)) {
                KillTimer(hwnd, wParam);
                SetWindowTextW(g_h_btn_copy, L"Copy JSON");
                InvalidateRect(g_h_btn_copy, NULL, TRUE);
                return 0;
            } else if (wParam == reinterpret_cast<UINT_PTR>(g_h_btn_copy_card)) {
                KillTimer(hwnd, wParam);
                SetWindowTextW(g_h_btn_copy_card, L"Copy Card");
                InvalidateRect(g_h_btn_copy_card, NULL, TRUE);
                return 0;
            } else if (wParam == reinterpret_cast<UINT_PTR>(g_h_btn_export)) {
                KillTimer(hwnd, wParam);
                SetWindowTextW(g_h_btn_export, L"Export JSON");
                InvalidateRect(g_h_btn_export, NULL, TRUE);
                return 0;
            } else if (wParam == reinterpret_cast<UINT_PTR>(g_h_btn_export_card)) {
                KillTimer(hwnd, wParam);
                SetWindowTextW(g_h_btn_export_card, L"Export Card");
                InvalidateRect(g_h_btn_export_card, NULL, TRUE);
                return 0;
            }
            break;
        }

        case WM_DPICHANGED: {
            UINT new_dpi = LOWORD(wParam);
            update_fonts_for_dpi(new_dpi);
            const RECT* prcNewWindow = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, NULL,
                prcNewWindow->left,
                prcNewWindow->top,
                prcNewWindow->right - prcNewWindow->left,
                prcNewWindow->bottom - prcNewWindow->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            RECT client_rc;
            GetClientRect(hwnd, &client_rc);
            layout_controls(hwnd, client_rc.right - client_rc.left, client_rc.bottom - client_rc.top);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }

        case WM_HOTKEY: {
            if (g_h_settings_dlg != nullptr && IsWindow(g_h_settings_dlg)) {
                return 0; // Ignore hotkey while modal settings dialog is active
            }
            if (wParam == ID_HOTKEY_TOGGLE_CAPTURE && g_controller) {
                auto cur_state = g_controller->current_state();
                if (cur_state == GuiSessionState::IDLE) {
                    auto cfg = read_gui_config();
                    g_controller->start_session_async(cfg);
                } else if (g_controller->is_capturing()) {
                    g_controller->stop_session_async();
                }
            }
            return 0;
        }

        // Custom Stuttometer Notifications
        case WM_STUTTO_STATE_CHANGE: {
            auto state = static_cast<GuiSessionState>(wParam);
            update_session_ui_state(state);
            return 0;
        }

        case WM_STUTTO_TRIGGER: {
            std::unique_ptr<DiagnosticReport> report(reinterpret_cast<DiagnosticReport*>(lParam));
            if (report && g_settings_config.enable_osd) {
                g_osd_toast.show(*report, g_settings_config.osd_duration_ms, g_settings_config.osd_position);
            }
            handle_new_report(std::move(report));
            return 0;
        }

        case WM_STUTTO_METRICS: {
            std::unique_ptr<GuiMetrics> metrics(reinterpret_cast<GuiMetrics*>(lParam));
            if (metrics) {
                if (metrics->total_events_recorded > 0) {
                    g_has_received_data = true;
                }
                update_metrics_text();
                update_clear_button_state();

                // Invalidate the telemetry badge region with inflation
                RECT client_rc;
                GetClientRect(hwnd, &client_rc);
                RECT rc_b = get_telemetry_badge_rect(client_rc.right - client_rc.left);
                RECT rc_inv = { rc_b.left - scale_dpi(2), rc_b.top - scale_dpi(2), rc_b.right + scale_dpi(2), rc_b.bottom + scale_dpi(2) };
                InvalidateRect(hwnd, &rc_inv, FALSE);
            }
            return 0;
        }

        case WM_STUTTO_PROCESSES_UPDATED: {
            std::unique_ptr<ProcessList> procs(reinterpret_cast<ProcessList*>(lParam));
            handle_processes_updated(std::move(procs));
            return 0;
        }

        case WM_STUTTO_LOG: {
            std::unique_ptr<std::string> log_line(reinterpret_cast<std::string*>(lParam));
            if (log_line && !log_line->empty()) {
                g_has_received_data = true;
                append_engine_log(utf8_to_wstring(*log_line));
                if (g_selected_stutter_index < 0) {
                    update_inspector(-1);
                }
                update_clear_button_state();
            }
            return 0;
        }

        // High-Contrast Control Coloring Handlers
        case WM_CTLCOLORSTATIC: {
            HDC hdcStatic = (HDC)wParam;
            HWND hCtl = (HWND)lParam;
            if (hCtl == g_h_edit_inspector) {
                SetBkColor(hdcStatic, COLOR_INPUT_BG);
                SetTextColor(hdcStatic, COLOR_TEXT_PRI);
                return (LRESULT)g_theme.br_input;
            }
            if (hCtl == g_h_lbl_target) {
                SetBkColor(hdcStatic, COLOR_CARD_BG);
                SetTextColor(hdcStatic, COLOR_TEXT_PRI);
                return (LRESULT)g_theme.br_card;
            }
            SetBkColor(hdcStatic, COLOR_BG);
            SetTextColor(hdcStatic, COLOR_TEXT_PRI);
            return (LRESULT)g_theme.br_bg;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdcEdit = (HDC)wParam;
            HWND hCtl = (HWND)lParam;
            if (hCtl == g_h_edit_inspector) {
                SetBkColor(hdcEdit, COLOR_INPUT_BG);
                SetTextColor(hdcEdit, COLOR_TEXT_PRI);
                return (LRESULT)g_theme.br_input;
            }
            SetBkColor(hdcEdit, COLOR_INPUT_BG);
            SetTextColor(hdcEdit, COLOR_TEXT_PRI);
            return (LRESULT)g_theme.br_input;
        }

        case WM_CTLCOLORLISTBOX: {
            HDC hdcListBox = (HDC)wParam;
            SetBkColor(hdcListBox, COLOR_INPUT_BG);
            SetTextColor(hdcListBox, COLOR_TEXT_PRI);
            return (LRESULT)g_theme.br_input;
        }

        case WM_QUERYENDSESSION: {
            save_user_settings();
            return TRUE;
        }

        case WM_ENDSESSION: {
            if (wParam == TRUE) {
                if (g_controller) {
                    g_controller->shutdown();
                }
            }
            return 0;
        }

        case WM_CLOSE: {
            save_user_settings();
            ShowWindow(hwnd, SW_HIDE);
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY: {
            UnregisterHotKey(hwnd, ID_HOTKEY_TOGGLE_CAPTURE);

            g_osd_toast.destroy();

            if (g_controller) {
                g_controller->shutdown();
            }

            MSG msg;
            while (PeekMessageW(&msg, hwnd, WM_STUTTO_STATE_CHANGE, WM_STUTTO_LOG, PM_REMOVE)) {
                if (msg.message == WM_STUTTO_TRIGGER && msg.lParam) {
                    delete reinterpret_cast<DiagnosticReport*>(msg.lParam);
                } else if (msg.message == WM_STUTTO_METRICS && msg.lParam) {
                    delete reinterpret_cast<GuiMetrics*>(msg.lParam);
                } else if (msg.message == WM_STUTTO_PROCESSES_UPDATED && msg.lParam) {
                    delete reinterpret_cast<ProcessList*>(msg.lParam);
                } else if (msg.message == WM_STUTTO_LOG && msg.lParam) {
                    delete reinterpret_cast<std::string*>(msg.lParam);
                }
            }

            g_controller.reset();
            g_stutters.clear();
            g_engine_logs.clear();

            PostQuitMessage(0);
            return 0;
        }

        case WM_NCDESTROY: {
            // Destroy cached GDI objects & fonts after all child windows are destroyed
            g_theme.destroy();
            destroy_theme_fonts();
            break;
        }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

} // namespace stuttometer::gui

// Win32 Application Entry Point
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, PWSTR /*pCmdLine*/, int nCmdShow) {
    HRESULT hr_com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (!stuttometer::gui::CardRenderer::initialize()) {
        OutputDebugStringA("[Stuttometer] Warning: CardRenderer::initialize() failed. Visual cards disabled.\n");
    }

    INITCOMMONCONTROLSEX icex{};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES | ICC_HOTKEY_CLASS;
    InitCommonControlsEx(&icex);

    stuttometer::gui::init_process_dark_mode();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = stuttometer::gui::MainWindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL; // Handled in WM_PAINT to eliminate flicker
    wc.lpszClassName = L"StuttometerMainWindowClass";

    HICON hIconBig = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    HICON hIconSmall = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!hIconBig) hIconBig = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!hIconSmall) hIconSmall = hIconBig;

    wc.hIcon = hIconBig ? hIconBig : LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm = hIconSmall ? hIconSmall : wc.hIcon;

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"Failed to register window class.", L"Error", MB_OK | MB_ICONERROR);
        stuttometer::gui::CardRenderer::shutdown();
        if (SUCCEEDED(hr_com)) CoUninitialize();
        return 1;
    }

    UINT init_dpi = GetDpiForSystem();
    if (init_dpi == 0) {
        init_dpi = 96;
        HDC screen = GetDC(NULL);
        if (screen) {
            init_dpi = GetDeviceCaps(screen, LOGPIXELSY);
            ReleaseDC(NULL, screen);
        }
    }
    int base_w = MulDiv(1020, init_dpi, 96);
    int base_h = MulDiv(660, init_dpi, 96);

    RECT rc = { 0, 0, base_w, base_h };
    // INTENTIONAL DESIGN DECISION:
    // Fixed window style (no WS_THICKFRAME / WS_MAXIMIZEBOX) guarantees a locked, clean dashboard window.
    DWORD dwStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    AdjustWindowRectEx(&rc, dwStyle, FALSE, 0);

    int win_w = rc.right - rc.left;
    int win_h = rc.bottom - rc.top;

    // Center window on screen work area (accounting for Windows taskbar)
    RECT work_area{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    int screen_w = work_area.right - work_area.left;
    int screen_h = work_area.bottom - work_area.top;
    int pos_x = work_area.left + (screen_w > win_w ? (screen_w - win_w) / 2 : 0);
    int pos_y = work_area.top + (screen_h > win_h ? (screen_h - win_h) / 2 : 0);

    HWND hwnd = CreateWindowExW(
        0,
        L"StuttometerMainWindowClass",
        L"Stuttometer - Real-Time ETW Stutter & Glitch Diagnostic Utility",
        dwStyle,
        pos_x, pos_y,
        win_w, win_h,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd) {
        MessageBoxW(NULL, L"Failed to create main application window.", L"Error", MB_OK | MB_ICONERROR);
        stuttometer::gui::CardRenderer::shutdown();
        if (SUCCEEDED(hr_com)) CoUninitialize();
        return 1;
    }

    if (hIconBig) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
    if (hIconSmall) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);
    stuttometer::gui::apply_window_dark_titlebar(hwnd);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!hwnd || !IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (hIconBig) DestroyIcon(hIconBig);
    if (hIconSmall && hIconSmall != hIconBig) DestroyIcon(hIconSmall);

    stuttometer::gui::CardRenderer::shutdown();
    if (SUCCEEDED(hr_com)) CoUninitialize();

    return static_cast<int>(msg.wParam);
}
