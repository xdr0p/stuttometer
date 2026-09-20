#include "export_utils.hpp"
#include "gui_state.hpp"
#include "card_renderer.hpp"
#include <stuttometer/json_reporter.hpp>

#include <windows.h>
#include <commdlg.h>
#include <filesystem>
#include <string>

namespace stuttometer::gui {

void copy_selected_report_json(HWND hwnd) {
    if (g_selected_stutter_index < 0 || g_selected_stutter_index >= static_cast<int>(g_stutters.size())) {
        return;
    }

    const auto& item = g_stutters[g_selected_stutter_index];
    if (!item.report) {
        return;
    }
    JsonReporter reporter;
    bool redact = g_settings_config.redact;
    std::string json = reporter.to_json_string(*item.report, redact, 2);
    std::wstring wjson = utf8_to_wstring(json);

    bool ok = false;
    if (OpenClipboard(hwnd)) {
        EmptyClipboard();
        HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, (wjson.size() + 1) * sizeof(wchar_t));
        if (hGlob) {
            wchar_t* pMem = static_cast<wchar_t*>(GlobalLock(hGlob));
            if (pMem) {
                wcscpy_s(pMem, wjson.size() + 1, wjson.c_str());
                GlobalUnlock(hGlob);
                if (SetClipboardData(CF_UNICODETEXT, hGlob)) {
                    ok = true;
                }
            }
            if (!ok) GlobalFree(hGlob);
        }
        CloseClipboard();
    }

    if (g_h_btn_copy) {
        SetWindowTextW(g_h_btn_copy, ok ? L"Copied \u2713" : L"Failed \u2715");
        InvalidateRect(g_h_btn_copy, NULL, TRUE);
        SetTimer(hwnd, reinterpret_cast<UINT_PTR>(g_h_btn_copy), 1500, NULL);
    }
}

void export_selected_report_json(HWND hwnd) {
    if (g_selected_stutter_index < 0 || g_selected_stutter_index >= static_cast<int>(g_stutters.size())) {
        return;
    }

    const auto& item = g_stutters[g_selected_stutter_index];
    if (!item.report) {
        return;
    }
    JsonReporter reporter;
    bool redact = g_settings_config.redact;

    wchar_t filename_buf[MAX_PATH] = L"stutto_report.json";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"JSON Files (*.json)\0*.json\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename_buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (GetSaveFileNameW(&ofn)) {
        std::string path = wstring_to_utf8(filename_buf);
        bool ok = reporter.save_to_file(*item.report, path, redact);
        if (g_h_btn_export) {
            SetWindowTextW(g_h_btn_export, ok ? L"Exported \u2713" : L"Failed \u2715");
            InvalidateRect(g_h_btn_export, NULL, TRUE);
            SetTimer(hwnd, reinterpret_cast<UINT_PTR>(g_h_btn_export), 1500, NULL);
        }
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

    if (g_h_btn_copy_card) {
        SetWindowTextW(g_h_btn_copy_card, ok ? L"Copied \u2713" : L"Failed \u2715");
        InvalidateRect(g_h_btn_copy_card, NULL, TRUE);
        SetTimer(hwnd, reinterpret_cast<UINT_PTR>(g_h_btn_copy_card), 1500, NULL);
    }
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
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"PNG Image (*.png)\0*.png\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename_buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (GetSaveFileNameW(&ofn)) {
        CardRenderOptions opts;
        bool ok = CardRenderer::save_card_to_png(*item.report, std::filesystem::path(filename_buf), opts);
        if (g_h_btn_export_card) {
            SetWindowTextW(g_h_btn_export_card, ok ? L"Exported \u2713" : L"Failed \u2715");
            InvalidateRect(g_h_btn_export_card, NULL, TRUE);
            SetTimer(hwnd, reinterpret_cast<UINT_PTR>(g_h_btn_export_card), 1500, NULL);
        }
    }
}

} // namespace stuttometer::gui
