#include "settings_dialog.hpp"
#include "theme.hpp"
#include "gui_state.hpp"
#include "dark_controls.hpp"

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>

namespace stuttometer::gui {

// Settings Dialog Control IDs
constexpr int IDC_SET_HOTKEY_EDIT    = 2001;
constexpr int IDC_SET_CHK_SOUND      = 2002;
constexpr int IDC_SET_CHK_REDACT     = 2003;
constexpr int IDC_SET_CHK_AUDIO      = 2004;
constexpr int IDC_SET_COMBO_TIER     = 2005;
constexpr int IDC_SET_EDIT_PRE_WIN   = 2006;
constexpr int IDC_SET_EDIT_POST_WIN  = 2007;
constexpr int IDC_SET_EDIT_COOLDOWN  = 2008;
constexpr int IDC_SET_COMBO_BUFFER   = 2009;
constexpr int IDC_SET_EDIT_DPC       = 2010;
constexpr int IDC_SET_EDIT_ISR       = 2011;
constexpr int IDC_SET_EDIT_DISK      = 2012;
constexpr int IDC_SET_EDIT_CSWITCH   = 2013;
constexpr int IDC_SET_EDIT_SMI       = 2014;
constexpr int IDC_SET_BTN_RESET      = 2015;
constexpr int IDC_SET_BTN_SAVE       = 2017;
constexpr int IDC_SET_CHK_ADVANCED   = 2018;
constexpr int IDC_SET_EDIT_MEM_ALLOC = 2019;
constexpr int IDC_SET_EDIT_MEM_TRIM  = 2020;
constexpr int IDC_SET_EDIT_MEM_PHYS  = 2021;
constexpr int IDC_SET_COMBO_TRIG_MODE = 2022;
constexpr int IDC_SET_EDIT_SPIKE_MULT = 2023;
constexpr int IDC_SET_EDIT_MIN_DELTA  = 2024;
constexpr int IDC_SET_CHK_JUDDER      = 2025;
constexpr int IDC_SET_EDIT_TARGET_FPS = 2026;
constexpr int IDC_SET_CHK_AUTO_SAVE        = 2027;
constexpr int IDC_SET_EDIT_AUTO_SAVE       = 2028;
constexpr int IDC_SET_BTN_BROWSE_AUTO_SAVE = 2029;
constexpr int IDC_SET_EDIT_D3D12_PSO       = 2030;
constexpr int IDC_SET_EDIT_VRAM_DEMOTED    = 2031;
constexpr int IDC_SET_CHK_OSD              = 2032;
constexpr int IDC_SET_COMBO_OSD_POS        = 2033;
constexpr int IDC_SET_BTN_CANCEL           = 2034;
constexpr int IDC_SET_COMBO_PACING_PROFILE = 2035;
constexpr int IDC_SET_LBL_PROFILE_HINT     = 2036;



// -----------------------------------------------------------------------------
// Settings Dialog Implementation
// -----------------------------------------------------------------------------
// Settings Dialog State & Event Handling
// -----------------------------------------------------------------------------
struct SettingsDialogState {
    uint32_t hotkey_vk{VK_F11};
    uint32_t hotkey_mods{MOD_CONTROL};
    uint32_t original_hotkey_vk{VK_F11};
    uint32_t original_hotkey_mods{MOD_CONTROL};
    bool advanced_unlocked{false};

    HWND h_hotkey_edit{nullptr};
    HWND h_chk_sound{nullptr};
    HWND h_chk_redact{nullptr};
    HWND h_chk_audio{nullptr};
    HWND h_chk_auto_save{nullptr};
    HWND h_edit_auto_save{nullptr};
    HWND h_btn_browse_auto_save{nullptr};
    HWND h_chk_osd{nullptr};
    HWND h_combo_osd_pos{nullptr};
    HWND h_chk_advanced{nullptr};
    HWND h_combo_tier{nullptr};
    HWND h_edit_pre_win{nullptr};
    HWND h_edit_post_win{nullptr};
    HWND h_edit_cooldown{nullptr};
    HWND h_combo_buffer{nullptr};
    HWND h_edit_dpc{nullptr};
    HWND h_edit_isr{nullptr};
    HWND h_edit_disk{nullptr};
    HWND h_edit_cswitch{nullptr};
    HWND h_edit_smi{nullptr};
    HWND h_edit_mem_alloc{nullptr};
    HWND h_edit_mem_trim{nullptr};
    HWND h_edit_mem_phys{nullptr};
    HWND h_edit_d3d12_pso{nullptr};
    HWND h_edit_vram_demoted{nullptr};
    HWND h_combo_trig_mode{nullptr};
    HWND h_combo_pacing_profile{nullptr};
    HWND h_edit_target_fps{nullptr};
    HWND h_edit_spike_mult{nullptr};
    HWND h_edit_min_delta{nullptr};
    HWND h_lbl_profile_hint{nullptr};
    HWND h_chk_judder{nullptr};
    HWND h_btn_reset{nullptr};
    HWND h_btn_cancel{nullptr};
    HWND h_btn_save{nullptr};

    PacingProfile current_profile{PacingProfile::AUTO_ADAPTIVE};
    PacingProfile previous_preset{PacingProfile::AUTO_ADAPTIVE};
    double custom_spike_mult{2.0};
    double custom_min_delta{4.0};
    uint32_t osd_duration_ms{3500};
    uint32_t target_pid{0};
    DisplayRefreshInfo detected_display{};
    bool smi_threshold_manual{false};
    // Set to true around any programmatic edit-control update (e.g. SetWindowTextW) to prevent spurious manual-flag commitment
    bool suppress_change_notification{false};

    // Synchronized Hit-Testing Rectangles for Master Banner & Checkbox Labels
    RECT rc_banner{};
    RECT rc_lbl_adv{};
    RECT rc_lbl_snd{};
    RECT rc_lbl_rd{};
    RECT rc_lbl_aud{};
    RECT rc_lbl_auto_save{};
    RECT rc_lbl_osd{};
    RECT rc_lbl_judder{};
};

static LRESULT CALLBACK SettingsHotkeySubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    auto* state = reinterpret_cast<SettingsDialogState*>(dwRefData);
    switch (uMsg) {
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, SettingsHotkeySubclassProc, uIdSubclass);
            break;

        case WM_SETFONT:
        case WM_SIZE: {
            LRESULT res = DefSubclassProc(hwnd, uMsg, wParam, lParam);
            HFONT hFont = (uMsg == WM_SETFONT) ? reinterpret_cast<HFONT>(wParam) : nullptr;
            apply_edit_centered_padding(hwnd, hFont);
            return res;
        }

        case WM_SETCURSOR: {
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            return TRUE;
        }

        case WM_SETFOCUS: {
            LRESULT res = DefSubclassProc(hwnd, uMsg, wParam, lParam);
            if (state) {
                state->original_hotkey_vk = state->hotkey_vk;
                state->original_hotkey_mods = state->hotkey_mods;
            }
            SetWindowTextW(hwnd, L"Ctrl/Alt + ...");
            apply_edit_centered_padding(hwnd);
            SendMessageW(hwnd, EM_SETSEL, static_cast<WPARAM>(-1), 0);
            HideCaret(hwnd);
            InvalidateRect(hwnd, NULL, TRUE);
            return res;
        }

        case WM_KILLFOCUS: {
            LRESULT res = DefSubclassProc(hwnd, uMsg, wParam, lParam);
            if (state) {
                std::wstring hk_str = format_hotkey_display(state->hotkey_mods, state->hotkey_vk);
                SetWindowTextW(hwnd, hk_str.c_str());
                apply_edit_centered_padding(hwnd);
            }
            SendMessageW(hwnd, EM_SETSEL, static_cast<WPARAM>(-1), 0);
            HideCaret(hwnd);
            InvalidateRect(hwnd, NULL, TRUE);
            return res;
        }

        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: {
            SetFocus(hwnd);
            HideCaret(hwnd);
            return 0;
        }

        case WM_PAINT: {
            LRESULT res = DefSubclassProc(hwnd, uMsg, wParam, lParam);
            HideCaret(hwnd);
            HDC hdc = GetDC(hwnd);
            if (hdc) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                HPEN pen = IsWindowEnabled(hwnd) ? g_theme.pen_input_border : g_theme.pen_card_border;
                HGDIOBJ old_pen = SelectObject(hdc, pen);
                HGDIOBJ old_br = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, scale_dpi(6), scale_dpi(6));
                SelectObject(hdc, old_br);
                SelectObject(hdc, old_pen);
                ReleaseDC(hwnd, hdc);
            }
            return res;
        }

        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;

        case WM_CHAR:
        case WM_UNICHAR:
        case WM_SYSCHAR:
            return 0; // Block raw typing

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (wParam == VK_ESCAPE) {
                if (state) {
                    state->hotkey_vk = state->original_hotkey_vk;
                    state->hotkey_mods = state->original_hotkey_mods;
                    std::wstring hk_str = format_hotkey_display(state->hotkey_mods, state->hotkey_vk);
                    SetWindowTextW(hwnd, hk_str.c_str());
                    apply_edit_centered_padding(hwnd);
                }
                SetFocus(GetParent(hwnd));
                return 0;
            }

            if (wParam == VK_CONTROL || wParam == VK_LCONTROL || wParam == VK_RCONTROL) {
                SetWindowTextW(hwnd, L"Ctrl + ...");
                apply_edit_centered_padding(hwnd);
                return 0;
            }
            if (wParam == VK_MENU || wParam == VK_LMENU || wParam == VK_RMENU) {
                SetWindowTextW(hwnd, L"Alt + ...");
                apply_edit_centered_padding(hwnd);
                return 0;
            }
            if (wParam == VK_SHIFT || wParam == VK_LSHIFT || wParam == VK_RSHIFT) {
                SetWindowTextW(hwnd, L"Shift + ...");
                apply_edit_centered_padding(hwnd);
                return 0;
            }

            // Note: Ctrl and Alt (along with standalone function/navigation keys) are the only
            // modifier hotkeys supported by design to ensure reliable global registration and prevent
            // conflicts with Windows shell shortcuts or accidental game interruption.
            UINT vk = static_cast<UINT>(wParam);
            bool ctrl_down  = ((GetKeyState(VK_CONTROL) & 0x8000) != 0) || ((GetKeyState(VK_LCONTROL) & 0x8000) != 0) || ((GetKeyState(VK_RCONTROL) & 0x8000) != 0);
            bool alt_down   = ((GetKeyState(VK_MENU) & 0x8000) != 0) || ((GetKeyState(VK_LMENU) & 0x8000) != 0) || ((GetKeyState(VK_RMENU) & 0x8000) != 0) || ((lParam & (1 << 29)) != 0);
            bool shift_down = ((GetKeyState(VK_SHIFT) & 0x8000) != 0) || ((GetKeyState(VK_LSHIFT) & 0x8000) != 0) || ((GetKeyState(VK_RSHIFT) & 0x8000) != 0);

            bool is_f_key = (vk >= VK_F1 && vk <= VK_F24) || vk == VK_PAUSE || vk == VK_SCROLL || vk == VK_INSERT;

            if ((vk == VK_F1 || vk == VK_F5 || vk == VK_F10) && !ctrl_down && !alt_down && !shift_down) {
                SetWindowTextW(hwnd, L"Ctrl/Alt + [Key]");
                apply_edit_centered_padding(hwnd);
                return 0;
            }

            if (!ctrl_down && !alt_down && !is_f_key) {
                SetWindowTextW(hwnd, L"Ctrl/Alt + [Key]");
                apply_edit_centered_padding(hwnd);
                return 0;
            }

            UINT fsModifiers = MOD_NOREPEAT;
            if (ctrl_down)  fsModifiers |= MOD_CONTROL;
            if (shift_down) fsModifiers |= MOD_SHIFT;
            if (alt_down)   fsModifiers |= MOD_ALT;

            if (state) {
                state->hotkey_vk = vk;
                state->hotkey_mods = fsModifiers;
                std::wstring hk_str = format_hotkey_display(state->hotkey_mods, state->hotkey_vk);
                SetWindowTextW(hwnd, hk_str.c_str());
                apply_edit_centered_padding(hwnd);
            }

            SetFocus(GetParent(hwnd));
            return 0;
        }
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

static void update_settings_dependencies(SettingsDialogState* state) {
    if (!state) return;
    bool auto_save_enabled = (SendMessageW(state->h_chk_auto_save, BM_GETCHECK, 0, 0) == BST_CHECKED);
    EnableWindow(state->h_edit_auto_save, auto_save_enabled ? TRUE : FALSE);
    EnableWindow(state->h_btn_browse_auto_save, auto_save_enabled ? TRUE : FALSE);

    bool osd_enabled = (SendMessageW(state->h_chk_osd, BM_GETCHECK, 0, 0) == BST_CHECKED);
    EnableWindow(state->h_combo_osd_pos, osd_enabled ? TRUE : FALSE);

    BOOL enable_adv = state->advanced_unlocked ? TRUE : FALSE;
    EnableWindow(state->h_combo_tier, enable_adv);
    EnableWindow(state->h_edit_pre_win, enable_adv);
    EnableWindow(state->h_edit_post_win, enable_adv);
    EnableWindow(state->h_edit_cooldown, enable_adv);
    EnableWindow(state->h_combo_buffer, enable_adv);
    EnableWindow(state->h_edit_dpc, enable_adv);
    EnableWindow(state->h_edit_isr, enable_adv);
    EnableWindow(state->h_edit_disk, enable_adv);
    EnableWindow(state->h_edit_cswitch, enable_adv);
    EnableWindow(state->h_edit_smi, enable_adv);
    EnableWindow(state->h_edit_mem_alloc, enable_adv);
    EnableWindow(state->h_edit_mem_trim, enable_adv);
    EnableWindow(state->h_edit_mem_phys, enable_adv);
    EnableWindow(state->h_edit_d3d12_pso, enable_adv);
    EnableWindow(state->h_edit_vram_demoted, enable_adv);
    EnableWindow(state->h_combo_trig_mode, enable_adv);
    EnableWindow(state->h_edit_target_fps, enable_adv);

    int tm_sel = static_cast<int>(SendMessageW(state->h_combo_trig_mode, CB_GETCURSEL, 0, 0));
    FrameTriggerMode mode = (tm_sel == 1) ? FrameTriggerMode::DYNAMIC_ONLY : ((tm_sel == 2) ? FrameTriggerMode::STATIC_ONLY : FrameTriggerMode::HYBRID);

    bool enable_profile = (mode != FrameTriggerMode::STATIC_ONLY);
    EnableWindow(state->h_combo_pacing_profile, enable_profile);

    bool enable_spike_edits = (state->advanced_unlocked && state->current_profile == PacingProfile::CUSTOM && mode != FrameTriggerMode::STATIC_ONLY);
    EnableWindow(state->h_edit_spike_mult, enable_spike_edits);
    EnableWindow(state->h_edit_min_delta, enable_spike_edits);

    EnableWindow(state->h_chk_judder, enable_adv);

    if (mode == FrameTriggerMode::STATIC_ONLY) {
        SetWindowTextW(state->h_lbl_profile_hint, L"(N/A in Static Only mode)");
    } else if (state->current_profile == PacingProfile::CUSTOM) {
        if (!state->advanced_unlocked) {
            SetWindowTextW(state->h_lbl_profile_hint, L"Custom profile active \u2014 click 'Unlock Advanced Settings' below to edit");
        } else {
            SetWindowTextW(state->h_lbl_profile_hint, L"");
        }
    } else {
        if (state->advanced_unlocked) {
            SetWindowTextW(state->h_lbl_profile_hint, L"Preset locked \u2014 select Custom Calibration to edit");
        } else {
            SetWindowTextW(state->h_lbl_profile_hint, L"");
        }
    }

    InvalidateRect(state->h_combo_tier, NULL, TRUE);
    InvalidateRect(state->h_combo_buffer, NULL, TRUE);
    InvalidateRect(state->h_edit_pre_win, NULL, TRUE);
    InvalidateRect(state->h_edit_post_win, NULL, TRUE);
    InvalidateRect(state->h_edit_cooldown, NULL, TRUE);
    InvalidateRect(state->h_edit_dpc, NULL, TRUE);
    InvalidateRect(state->h_edit_isr, NULL, TRUE);
    InvalidateRect(state->h_edit_disk, NULL, TRUE);
    InvalidateRect(state->h_edit_cswitch, NULL, TRUE);
    InvalidateRect(state->h_edit_smi, NULL, TRUE);
    InvalidateRect(state->h_edit_mem_alloc, NULL, TRUE);
    InvalidateRect(state->h_edit_mem_trim, NULL, TRUE);
    InvalidateRect(state->h_edit_mem_phys, NULL, TRUE);
    InvalidateRect(state->h_edit_d3d12_pso, NULL, TRUE);
    InvalidateRect(state->h_edit_vram_demoted, NULL, TRUE);
    InvalidateRect(state->h_combo_trig_mode, NULL, TRUE);
    InvalidateRect(state->h_combo_pacing_profile, NULL, TRUE);
    InvalidateRect(state->h_edit_target_fps, NULL, TRUE);
    InvalidateRect(state->h_edit_spike_mult, NULL, TRUE);
    InvalidateRect(state->h_edit_min_delta, NULL, TRUE);
    InvalidateRect(state->h_lbl_profile_hint, NULL, TRUE);
    InvalidateRect(state->h_chk_judder, NULL, TRUE);
}

static void toggle_advanced_settings(HWND hwnd, SettingsDialogState* state) {
    if (!state) return;
    if (!state->advanced_unlocked) {
        int res = MessageBoxW(hwnd,
            L"Modifying ETW provider tiers, buffer capacities, frame pacing triggers, or anomaly thresholds can impact diagnostic accuracy, memory consumption, or ETW tracing overhead.\n\nAre you sure you want to unlock advanced settings?",
            L"Advanced Settings Safeguard",
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (res == IDYES) {
            state->advanced_unlocked = true;
            SendMessageW(state->h_chk_advanced, BM_SETCHECK, BST_CHECKED, 0);
        } else {
            state->advanced_unlocked = false;
            SendMessageW(state->h_chk_advanced, BM_SETCHECK, BST_UNCHECKED, 0);
        }
    } else {
        state->advanced_unlocked = false;
        SendMessageW(state->h_chk_advanced, BM_SETCHECK, BST_UNCHECKED, 0);
    }
    update_settings_dependencies(state);
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static void layout_settings_controls(HWND hwnd, SettingsDialogState* state) {
    if (!state) return;

    RECT client_rc;
    GetClientRect(hwnd, &client_rc);
    int width = client_rc.right;

    const int margin = scale_dpi(16);
    const int gap = scale_dpi(14);
    const int col_w = (width - margin * 2 - gap) / 2;

    const int c_left_x = margin;
    const int c_right_x = c_left_x + col_w + gap;

    HDC hdc = GetDC(hwnd);
    HFONT old_font = (HFONT)SelectObject(hdc, g_font_ui_bold);

    const wchar_t* txt_snd = L"Enable Sound Cues";
    const wchar_t* txt_rd = L"Redact Personal Info (usernames & paths)";
    const wchar_t* txt_aud = L"Enable Audio Glitch Trigger";
    const wchar_t* txt_as = L"Auto-save JSON reports to folder:";
    const wchar_t* txt_osd = L"Enable In-Game OSD Toast";
    const wchar_t* txt_adv = L"Unlock Advanced Settings";
    const wchar_t* txt_jud = L"Enable Presentation Judder Detection";

    SIZE sz_snd{}, sz_rd{}, sz_aud{}, sz_as{}, sz_osd{}, sz_adv{}, sz_jud{};
    GetTextExtentPoint32W(hdc, txt_snd, static_cast<int>(wcslen(txt_snd)), &sz_snd);
    GetTextExtentPoint32W(hdc, txt_rd, static_cast<int>(wcslen(txt_rd)), &sz_rd);
    GetTextExtentPoint32W(hdc, txt_aud, static_cast<int>(wcslen(txt_aud)), &sz_aud);
    GetTextExtentPoint32W(hdc, txt_as, static_cast<int>(wcslen(txt_as)), &sz_as);
    GetTextExtentPoint32W(hdc, txt_osd, static_cast<int>(wcslen(txt_osd)), &sz_osd);
    GetTextExtentPoint32W(hdc, txt_adv, static_cast<int>(wcslen(txt_adv)), &sz_adv);
    GetTextExtentPoint32W(hdc, txt_jud, static_cast<int>(wcslen(txt_jud)), &sz_jud);

    SelectObject(hdc, old_font);
    ReleaseDC(hwnd, hdc);

    double v_scale = 1.0;
    int target_h = scale_dpi(564);
    if (client_rc.bottom < target_h) {
        v_scale = (std::max)(0.85, static_cast<double>(client_rc.bottom) / static_cast<double>(target_h));
    }
    auto scale_y = [v_scale](int y) -> int {
        return static_cast<int>(scale_dpi(y) * v_scale + 0.5);
    };
    int ctrl_h = (std::max)(scale_dpi(18), scale_y(24));

    // ==========================================
    // LEFT COLUMN: Card 1 (General Preferences) (Y: 16, H: 236)
    // ==========================================
    const int c1_x = c_left_x;
    const int c1_y = scale_y(16);
    const int c1_w = col_w;

    int r1_y = c1_y + scale_y(30);
    MoveWindow(state->h_hotkey_edit, c1_x + scale_dpi(140), r1_y, scale_dpi(110), ctrl_h, TRUE);
    apply_edit_centered_padding(state->h_hotkey_edit);

    int r2_y = c1_y + scale_y(58);
    MoveWindow(state->h_chk_sound, c1_x + scale_dpi(14), r2_y + (ctrl_h - scale_dpi(18)) / 2, scale_dpi(18), scale_dpi(18), TRUE);
    state->rc_lbl_snd = { c1_x + scale_dpi(32), r2_y, (std::min<int>)(c1_x + scale_dpi(32) + static_cast<int>(sz_snd.cx) + scale_dpi(6), c1_x + c1_w - scale_dpi(14)), r2_y + ctrl_h };

    int r3_y = c1_y + scale_y(86);
    MoveWindow(state->h_chk_redact, c1_x + scale_dpi(14), r3_y + (ctrl_h - scale_dpi(18)) / 2, scale_dpi(18), scale_dpi(18), TRUE);
    state->rc_lbl_rd = { c1_x + scale_dpi(32), r3_y, (std::min<int>)(c1_x + scale_dpi(32) + static_cast<int>(sz_rd.cx) + scale_dpi(6), c1_x + c1_w - scale_dpi(14)), r3_y + ctrl_h };

    int r4_y = c1_y + scale_y(114);
    MoveWindow(state->h_chk_audio, c1_x + scale_dpi(14), r4_y + (ctrl_h - scale_dpi(18)) / 2, scale_dpi(18), scale_dpi(18), TRUE);
    state->rc_lbl_aud = { c1_x + scale_dpi(32), r4_y, (std::min<int>)(c1_x + scale_dpi(32) + static_cast<int>(sz_aud.cx) + scale_dpi(6), c1_x + c1_w - scale_dpi(14)), r4_y + ctrl_h };

    int r5_y = c1_y + scale_y(142);
    MoveWindow(state->h_chk_auto_save, c1_x + scale_dpi(14), r5_y + (ctrl_h - scale_dpi(18)) / 2, scale_dpi(18), scale_dpi(18), TRUE);
    state->rc_lbl_auto_save = { c1_x + scale_dpi(32), r5_y, (std::min<int>)(c1_x + scale_dpi(32) + static_cast<int>(sz_as.cx) + scale_dpi(6), c1_x + c1_w - scale_dpi(14)), r5_y + ctrl_h };

    int r6_y = c1_y + scale_y(170);
    int browse_w = scale_dpi(66);
    int edit_as_x = c1_x + scale_dpi(32);
    int edit_as_w = c1_w - scale_dpi(32) - scale_dpi(14) - browse_w - scale_dpi(8);
    MoveWindow(state->h_edit_auto_save, edit_as_x, r6_y, edit_as_w, ctrl_h, TRUE);
    RECT rc_as = { scale_dpi(4), scale_dpi(3), edit_as_w - scale_dpi(4), ctrl_h };
    SendMessageW(state->h_edit_auto_save, EM_SETRECTNP, 0, (LPARAM)&rc_as);
    SendMessageW(state->h_edit_auto_save, EM_SETCUEBANNER, (WPARAM)FALSE, (LPARAM)L"Default: %LOCALAPPDATA%\\Stuttometer\\Reports");
    MoveWindow(state->h_btn_browse_auto_save, edit_as_x + edit_as_w + scale_dpi(8), r6_y, browse_w, ctrl_h, TRUE);

    int r7_osd_y = c1_y + scale_y(198);
    MoveWindow(state->h_chk_osd, c1_x + scale_dpi(14), r7_osd_y + (ctrl_h - scale_dpi(18)) / 2, scale_dpi(18), scale_dpi(18), TRUE);
    state->rc_lbl_osd = { c1_x + scale_dpi(32), r7_osd_y, c1_x + scale_dpi(216), r7_osd_y + ctrl_h };
    MoveWindow(state->h_combo_osd_pos, c1_x + scale_dpi(280), r7_osd_y + scale_dpi(1), c1_w - scale_dpi(294), scale_dpi(150), TRUE);

    // ==========================================
    // RIGHT COLUMN: Card 3 (Advanced Engine & Buffer Tuning) (Y: 16, H: 236)
    // ==========================================
    const int c3_x = c_right_x;
    const int c3_y = scale_y(16);
    const int c3_w = col_w;

    int r10_y = c3_y + scale_y(30);
    MoveWindow(state->h_combo_tier, c3_x + scale_dpi(140), r10_y + scale_dpi(1), c3_w - scale_dpi(154), scale_dpi(200), TRUE);

    int r11_y = c3_y + scale_y(64);
    MoveWindow(state->h_combo_buffer, c3_x + scale_dpi(140), r11_y + scale_dpi(1), c3_w - scale_dpi(154), scale_dpi(200), TRUE);

    int r12_y = c3_y + scale_y(128);
    MoveWindow(state->h_edit_pre_win, c3_x + scale_dpi(140), r12_y + scale_dpi(1), scale_dpi(48), ctrl_h, TRUE);

    int r13_y = c3_y + scale_y(160);
    MoveWindow(state->h_edit_post_win, c3_x + scale_dpi(140), r13_y + scale_dpi(1), scale_dpi(48), ctrl_h, TRUE);

    int r14_y = c3_y + scale_y(192);
    MoveWindow(state->h_edit_cooldown, c3_x + scale_dpi(140), r14_y + scale_dpi(1), scale_dpi(48), ctrl_h, TRUE);

    // ==========================================
    // LEFT COLUMN: Card 2 (Frame Pacing & Judder Triggers) (Y: 266, H: 232)
    // ==========================================
    const int c2_x = c_left_x;
    const int c2_y = scale_y(266);
    const int c2_w = col_w;

    int p1_y = c2_y + scale_y(30);
    MoveWindow(state->h_combo_trig_mode, c2_x + scale_dpi(134), p1_y + scale_dpi(1), c2_w - scale_dpi(148), scale_dpi(150), TRUE);

    int p2_y = c2_y + scale_y(60);
    MoveWindow(state->h_combo_pacing_profile, c2_x + scale_dpi(134), p2_y + scale_dpi(1), c2_w - scale_dpi(148), scale_dpi(150), TRUE);

    int p3_y = c2_y + scale_y(90);
    MoveWindow(state->h_edit_target_fps, c2_x + scale_dpi(134), p3_y + scale_dpi(1), scale_dpi(48), ctrl_h, TRUE);

    int p4_y = c2_y + scale_y(132);
    int sm_edit_x = c2_x + scale_dpi(134);
    int th_pacing_w = scale_dpi(44);
    MoveWindow(state->h_edit_spike_mult, sm_edit_x, p4_y + scale_dpi(1), th_pacing_w, ctrl_h, TRUE);

    int md_edit_x = c2_x + scale_dpi(354);
    MoveWindow(state->h_edit_min_delta, md_edit_x, p4_y + scale_dpi(1), th_pacing_w, ctrl_h, TRUE);

    int p5_hint_y = c2_y + scale_y(160);
    MoveWindow(state->h_lbl_profile_hint, c2_x + scale_dpi(14), p5_hint_y, c2_w - scale_dpi(28), scale_dpi(18), TRUE);

    int p6_jud_y = c2_y + scale_y(186);
    MoveWindow(state->h_chk_judder, c2_x + scale_dpi(14), p6_jud_y + (ctrl_h - scale_dpi(18)) / 2, scale_dpi(18), scale_dpi(18), TRUE);
    state->rc_lbl_judder = { c2_x + scale_dpi(32), p6_jud_y, (std::min<int>)(c2_x + scale_dpi(32) + static_cast<int>(sz_jud.cx) + scale_dpi(6), c2_x + c2_w - scale_dpi(14)), p6_jud_y + ctrl_h };

    // ==========================================
    // RIGHT COLUMN: Card 4 (Kernel & System Anomaly Thresholds) (Y: 266, H: 232)
    // Organized into 2 balanced sub-columns
    // ==========================================
    const int c4_x = c_right_x;
    const int c4_y = scale_y(266);

    const int col1_edit_x = c4_x + scale_dpi(134);
    const int col2_edit_x = c4_x + scale_dpi(354);
    const int th_edit_w = scale_dpi(48);

    int k1_y = c4_y + scale_y(52);
    int k2_y = c4_y + scale_y(84);
    int k3_y = c4_y + scale_y(116);
    int k4_y = c4_y + scale_y(148);
    int k5_y = c4_y + scale_y(180);

    // Left Sub-Column (CPU & GPU Pipeline)
    MoveWindow(state->h_edit_dpc, col1_edit_x, k1_y + scale_dpi(1), th_edit_w, ctrl_h, TRUE);
    MoveWindow(state->h_edit_isr, col1_edit_x, k2_y + scale_dpi(1), th_edit_w, ctrl_h, TRUE);
    MoveWindow(state->h_edit_cswitch, col1_edit_x, k3_y + scale_dpi(1), th_edit_w, ctrl_h, TRUE);
    MoveWindow(state->h_edit_smi, col1_edit_x, k4_y + scale_dpi(1), th_edit_w, ctrl_h, TRUE);
    MoveWindow(state->h_edit_d3d12_pso, col1_edit_x, k5_y + scale_dpi(1), th_edit_w, ctrl_h, TRUE);

    // Right Sub-Column (Memory & Storage)
    MoveWindow(state->h_edit_disk, col2_edit_x, k1_y + scale_dpi(1), th_edit_w, ctrl_h, TRUE);
    MoveWindow(state->h_edit_mem_alloc, col2_edit_x, k2_y + scale_dpi(1), th_edit_w, ctrl_h, TRUE);
    MoveWindow(state->h_edit_mem_trim, col2_edit_x, k3_y + scale_dpi(1), th_edit_w, ctrl_h, TRUE);
    MoveWindow(state->h_edit_mem_phys, col2_edit_x, k4_y + scale_dpi(1), th_edit_w, ctrl_h, TRUE);
    MoveWindow(state->h_edit_vram_demoted, col2_edit_x, k5_y + scale_dpi(1), th_edit_w, ctrl_h, TRUE);

    // ==========================================
    // FOOTER (Y: 514, H: 34)
    // ==========================================
    const int f_y = scale_y(514);
    const int f_h = (std::max)(scale_dpi(28), scale_y(34));
    MoveWindow(state->h_btn_reset, margin, f_y, scale_dpi(130), f_h, TRUE);

    int chk_adv_x = margin + scale_dpi(130) + scale_dpi(20);
    MoveWindow(state->h_chk_advanced, chk_adv_x, f_y + (f_h - scale_dpi(18)) / 2, scale_dpi(18), scale_dpi(18), TRUE);
    state->rc_lbl_adv = { chk_adv_x + scale_dpi(20), f_y, chk_adv_x + scale_dpi(20) + static_cast<int>(sz_adv.cx) + scale_dpi(8), f_y + f_h };

    int save_w = scale_dpi(125);
    int cancel_w = scale_dpi(100);
    int btn_gap = scale_dpi(10);
    int save_x = width - margin - save_w;
    int cancel_x = save_x - btn_gap - cancel_w;
    MoveWindow(state->h_btn_cancel, cancel_x, f_y, cancel_w, f_h, TRUE);
    MoveWindow(state->h_btn_save, save_x, f_y, save_w, f_h, TRUE);
}

static void dismiss_settings_dialog(HWND hDlg) {
    HWND hParent = GetWindow(hDlg, GW_OWNER);
    if (hParent && IsWindow(hParent)) {
        EnableWindow(hParent, TRUE);
        SetForegroundWindow(hParent);
        SetFocus(hParent);
    }
    DestroyWindow(hDlg);
}

static bool s_settings_saved = false;

static LRESULT CALLBACK SettingsWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<SettingsDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (uMsg) {
        case WM_CLOSE: {
            dismiss_settings_dialog(hwnd);
            return 0;
        }
        case WM_CREATE: {
            state = new SettingsDialogState();
            state->suppress_change_notification = true;
            GuiConfig active_cfg = read_gui_config();
            state->target_pid = active_cfg.target_pid;
            state->detected_display = query_display_refresh_info(state->target_pid);
            state->smi_threshold_manual = g_settings_config.smi_threshold_manual;
            state->hotkey_vk = g_hotkey_vk;
            state->hotkey_mods = g_hotkey_mods;
            state->osd_duration_ms = g_settings_config.osd_duration_ms;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

            // Preferences Controls
            std::wstring hk_str = format_hotkey_display(state->hotkey_mods, state->hotkey_vk);
            state->h_hotkey_edit = CreateWindowExW(0, L"EDIT", hk_str.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER | ES_READONLY, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_HOTKEY_EDIT, NULL, NULL);
            SendMessageW(state->h_hotkey_edit, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_hotkey_edit);
            SetWindowSubclass(state->h_hotkey_edit, SettingsHotkeySubclassProc, IDC_SET_HOTKEY_EDIT, reinterpret_cast<DWORD_PTR>(state));

            state->h_chk_sound = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_CHK_SOUND, NULL, NULL);
            SendMessageW(state->h_chk_sound, BM_SETCHECK, g_sound_cues_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
            apply_control_dark_theme(state->h_chk_sound);

            state->h_chk_redact = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_CHK_REDACT, NULL, NULL);
            SendMessageW(state->h_chk_redact, BM_SETCHECK, g_settings_config.redact ? BST_CHECKED : BST_UNCHECKED, 0);
            apply_control_dark_theme(state->h_chk_redact);

            state->h_chk_audio = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_CHK_AUDIO, NULL, NULL);
            SendMessageW(state->h_chk_audio, BM_SETCHECK, g_settings_config.enable_audio ? BST_CHECKED : BST_UNCHECKED, 0);
            apply_control_dark_theme(state->h_chk_audio);

            state->h_chk_auto_save = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_CHK_AUTO_SAVE, NULL, NULL);
            SendMessageW(state->h_chk_auto_save, BM_SETCHECK, !g_settings_config.output_dir.empty() ? BST_CHECKED : BST_UNCHECKED, 0);
            apply_control_dark_theme(state->h_chk_auto_save);

            state->h_edit_auto_save = CreateWindowExW(0, L"EDIT", g_settings_config.output_dir.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_AUTO_SAVE, NULL, NULL);
            SendMessageW(state->h_edit_auto_save, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            apply_control_dark_theme(state->h_edit_auto_save);
            SetWindowSubclass(state->h_edit_auto_save, EditCenteredSubclassProc, IDC_SET_EDIT_AUTO_SAVE, 0);
            SendMessageW(state->h_edit_auto_save, EM_SETCUEBANNER, (WPARAM)FALSE, (LPARAM)L"Default: %LOCALAPPDATA%\\Stuttometer\\Reports");

            state->h_btn_browse_auto_save = CreateWindowExW(0, L"BUTTON", L"Browse", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_BTN_BROWSE_AUTO_SAVE, NULL, NULL);
            SetPropW(state->h_btn_browse_auto_save, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::QuickAction));
            SetPropW(state->h_btn_browse_auto_save, L"OnCard", reinterpret_cast<HANDLE>(1));
            SetWindowSubclass(state->h_btn_browse_auto_save, DarkButtonSubclassProc, IDC_SET_BTN_BROWSE_AUTO_SAVE, 0);

            wchar_t num_buf[64]{};

            state->h_chk_osd = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_CHK_OSD, NULL, NULL);
            SendMessageW(state->h_chk_osd, BM_SETCHECK, g_settings_config.enable_osd ? BST_CHECKED : BST_UNCHECKED, 0);
            apply_control_dark_theme(state->h_chk_osd);

            state->h_combo_osd_pos = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_COMBO_OSD_POS, NULL, NULL);
            SendMessageW(state->h_combo_osd_pos, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            SendMessageW(state->h_combo_osd_pos, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scale_dpi(20));
            SendMessageW(state->h_combo_osd_pos, CB_SETITEMHEIGHT, (WPARAM)0, (LPARAM)scale_dpi(22));
            SendMessageW(state->h_combo_osd_pos, CB_ADDSTRING, 0, (LPARAM)L"Top-Right");
            SendMessageW(state->h_combo_osd_pos, CB_ADDSTRING, 0, (LPARAM)L"Bottom-Right");
            SendMessageW(state->h_combo_osd_pos, CB_ADDSTRING, 0, (LPARAM)L"Top-Left");
            SendMessageW(state->h_combo_osd_pos, CB_ADDSTRING, 0, (LPARAM)L"Bottom-Left");
            int osd_pos_idx = static_cast<int>(g_settings_config.osd_position);
            if (osd_pos_idx < 0 || osd_pos_idx > 3) osd_pos_idx = 0;
            SendMessageW(state->h_combo_osd_pos, CB_SETCURSEL, osd_pos_idx, 0);
            apply_control_dark_theme(state->h_combo_osd_pos);
            SetWindowSubclass(state->h_combo_osd_pos, DarkComboSubclassProc, IDC_SET_COMBO_OSD_POS, 0);

            // Advanced Settings Safeguard Checkbox
            state->h_chk_advanced = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_CHK_ADVANCED, NULL, NULL);
            SendMessageW(state->h_chk_advanced, BM_SETCHECK, BST_UNCHECKED, 0);
            apply_control_dark_theme(state->h_chk_advanced);

            // ETW Trace & Buffer Controls
            state->h_combo_tier = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_COMBO_TIER, NULL, NULL);
            SendMessageW(state->h_combo_tier, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            SendMessageW(state->h_combo_tier, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scale_dpi(20));
            SendMessageW(state->h_combo_tier, CB_SETITEMHEIGHT, (WPARAM)0, (LPARAM)scale_dpi(22));
            SendMessageW(state->h_combo_tier, CB_ADDSTRING, 0, (LPARAM)L"Standard (Kernel DPC + ISR + Disk I/O) [Default]");
            SendMessageW(state->h_combo_tier, CB_ADDSTRING, 0, (LPARAM)L"Full (Kernel DPC + Disk + Context Switch)");
            SendMessageW(state->h_combo_tier, CB_ADDSTRING, 0, (LPARAM)L"Minimal (User DXGI Present only)");
            int tier_sel = (g_settings_config.provider_tier == "full") ? 1 : ((g_settings_config.provider_tier == "minimal") ? 2 : 0);
            SendMessageW(state->h_combo_tier, CB_SETCURSEL, tier_sel, 0);
            apply_control_dark_theme(state->h_combo_tier);
            SetWindowSubclass(state->h_combo_tier, DarkComboSubclassProc, IDC_SET_COMBO_TIER, 0);

            swprintf_s(num_buf, L"%.1f", g_settings_config.window_pre_ms);
            state->h_edit_pre_win = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_PRE_WIN, NULL, NULL);
            SetWindowSubclass(state->h_edit_pre_win, EditCenteredSubclassProc, IDC_SET_EDIT_PRE_WIN, 0);
            SendMessageW(state->h_edit_pre_win, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_pre_win);

            swprintf_s(num_buf, L"%.1f", g_settings_config.window_post_ms);
            state->h_edit_post_win = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_POST_WIN, NULL, NULL);
            SetWindowSubclass(state->h_edit_post_win, EditCenteredSubclassProc, IDC_SET_EDIT_POST_WIN, 0);
            SendMessageW(state->h_edit_post_win, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_post_win);

            swprintf_s(num_buf, L"%.0f", g_settings_config.cooldown_ms);
            state->h_edit_cooldown = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_COOLDOWN, NULL, NULL);
            SetWindowSubclass(state->h_edit_cooldown, EditCenteredSubclassProc, IDC_SET_EDIT_COOLDOWN, 0);
            SendMessageW(state->h_edit_cooldown, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_cooldown);

            state->h_combo_buffer = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_COMBO_BUFFER, NULL, NULL);
            SendMessageW(state->h_combo_buffer, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            SendMessageW(state->h_combo_buffer, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scale_dpi(20));
            SendMessageW(state->h_combo_buffer, CB_SETITEMHEIGHT, (WPARAM)0, (LPARAM)scale_dpi(22));

            struct BufOption { const wchar_t* label; uint32_t slots; };
            BufOption buf_opts[] = {
                { L"65,536 slots (~4 MB RAM)", 65536 },
                { L"131,072 slots (~8 MB RAM)", 131072 },
                { L"262,144 slots (~16 MB RAM) [Default]", 262144 },
                { L"524,288 slots (~33 MB RAM)", 524288 },
                { L"1,048,576 slots (~67 MB RAM)", 1048576 }
            };
            int buf_sel_idx = 2;
            for (int i = 0; i < 5; ++i) {
                int idx = static_cast<int>(SendMessageW(state->h_combo_buffer, CB_ADDSTRING, 0, (LPARAM)buf_opts[i].label));
                SendMessageW(state->h_combo_buffer, CB_SETITEMDATA, idx, (LPARAM)buf_opts[i].slots);
                if (buf_opts[i].slots == g_settings_config.buffer_slots) buf_sel_idx = idx;
            }
            SendMessageW(state->h_combo_buffer, CB_SETCURSEL, buf_sel_idx, 0);
            apply_control_dark_theme(state->h_combo_buffer);
            SetWindowSubclass(state->h_combo_buffer, DarkComboSubclassProc, IDC_SET_COMBO_BUFFER, 0);

            // Correlation Cutoff Controls
            swprintf_s(num_buf, L"%u", g_settings_config.dpc_threshold_us);
            state->h_edit_dpc = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_DPC, NULL, NULL);
            SetWindowSubclass(state->h_edit_dpc, EditCenteredSubclassProc, IDC_SET_EDIT_DPC, 0);
            SendMessageW(state->h_edit_dpc, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_dpc);

            swprintf_s(num_buf, L"%u", g_settings_config.isr_threshold_us);
            state->h_edit_isr = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_ISR, NULL, NULL);
            SetWindowSubclass(state->h_edit_isr, EditCenteredSubclassProc, IDC_SET_EDIT_ISR, 0);
            SendMessageW(state->h_edit_isr, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_isr);

            swprintf_s(num_buf, L"%u", g_settings_config.disk_threshold_ms);
            state->h_edit_disk = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_DISK, NULL, NULL);
            SetWindowSubclass(state->h_edit_disk, EditCenteredSubclassProc, IDC_SET_EDIT_DISK, 0);
            SendMessageW(state->h_edit_disk, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_disk);

            swprintf_s(num_buf, L"%u", g_settings_config.cswitch_preempt_ms);
            state->h_edit_cswitch = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_CSWITCH, NULL, NULL);
            SetWindowSubclass(state->h_edit_cswitch, EditCenteredSubclassProc, IDC_SET_EDIT_CSWITCH, 0);
            SendMessageW(state->h_edit_cswitch, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_cswitch);

            swprintf_s(num_buf, L"%.1f", g_settings_config.smi_severity_threshold_ms);
            state->h_edit_smi = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_SMI, NULL, NULL);
            SetWindowSubclass(state->h_edit_smi, EditCenteredSubclassProc, IDC_SET_EDIT_SMI, 0);
            SendMessageW(state->h_edit_smi, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_smi);

            swprintf_s(num_buf, L"%u", g_settings_config.mem_alloc_threshold_mb);
            state->h_edit_mem_alloc = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_MEM_ALLOC, NULL, NULL);
            SetWindowSubclass(state->h_edit_mem_alloc, EditCenteredSubclassProc, IDC_SET_EDIT_MEM_ALLOC, 0);
            SendMessageW(state->h_edit_mem_alloc, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_mem_alloc);

            swprintf_s(num_buf, L"%u", g_settings_config.mem_trim_threshold_mb);
            state->h_edit_mem_trim = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_MEM_TRIM, NULL, NULL);
            SetWindowSubclass(state->h_edit_mem_trim, EditCenteredSubclassProc, IDC_SET_EDIT_MEM_TRIM, 0);
            SendMessageW(state->h_edit_mem_trim, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_mem_trim);

            swprintf_s(num_buf, L"%u", g_settings_config.mem_physical_latency_us);
            state->h_edit_mem_phys = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_MEM_PHYS, NULL, NULL);
            SetWindowSubclass(state->h_edit_mem_phys, EditCenteredSubclassProc, IDC_SET_EDIT_MEM_PHYS, 0);
            SendMessageW(state->h_edit_mem_phys, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_mem_phys);

            swprintf_s(num_buf, L"%u", g_settings_config.d3d12_pso_threshold_ms);
            state->h_edit_d3d12_pso = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_D3D12_PSO, NULL, NULL);
            SetWindowSubclass(state->h_edit_d3d12_pso, EditCenteredSubclassProc, IDC_SET_EDIT_D3D12_PSO, 0);
            SendMessageW(state->h_edit_d3d12_pso, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_d3d12_pso);

            swprintf_s(num_buf, L"%u", g_settings_config.vram_demoted_threshold_mb);
            state->h_edit_vram_demoted = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_VRAM_DEMOTED, NULL, NULL);
            SetWindowSubclass(state->h_edit_vram_demoted, EditCenteredSubclassProc, IDC_SET_EDIT_VRAM_DEMOTED, 0);
            SendMessageW(state->h_edit_vram_demoted, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_vram_demoted);

            // Frame Pacing & Dynamic Relative Trigger Controls
            state->h_combo_trig_mode = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_COMBO_TRIG_MODE, NULL, NULL);
            SendMessageW(state->h_combo_trig_mode, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            SendMessageW(state->h_combo_trig_mode, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scale_dpi(20));
            SendMessageW(state->h_combo_trig_mode, CB_SETITEMHEIGHT, (WPARAM)0, (LPARAM)scale_dpi(22));
            SendMessageW(state->h_combo_trig_mode, CB_ADDSTRING, 0, (LPARAM)L"Hybrid (Relative + Judder) [Default]");
            SendMessageW(state->h_combo_trig_mode, CB_ADDSTRING, 0, (LPARAM)L"Dynamic Only (Relative & Judder)");
            SendMessageW(state->h_combo_trig_mode, CB_ADDSTRING, 0, (LPARAM)L"Static FPS Floor Only");
            int tm_sel = (g_settings_config.frame_trigger_mode == FrameTriggerMode::DYNAMIC_ONLY) ? 1 : ((g_settings_config.frame_trigger_mode == FrameTriggerMode::STATIC_ONLY) ? 2 : 0);
            SendMessageW(state->h_combo_trig_mode, CB_SETCURSEL, tm_sel, 0);
            apply_control_dark_theme(state->h_combo_trig_mode);
            SetWindowSubclass(state->h_combo_trig_mode, DarkComboSubclassProc, IDC_SET_COMBO_TRIG_MODE, 0);

            state->h_combo_pacing_profile = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_COMBO_PACING_PROFILE, NULL, NULL);
            SendMessageW(state->h_combo_pacing_profile, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            SendMessageW(state->h_combo_pacing_profile, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scale_dpi(20));
            SendMessageW(state->h_combo_pacing_profile, CB_SETITEMHEIGHT, (WPARAM)0, (LPARAM)scale_dpi(22));
            SendMessageW(state->h_combo_pacing_profile, CB_ADDSTRING, 0, (LPARAM)L"Auto-Adaptive (Dynamic Baseline)");
            SendMessageW(state->h_combo_pacing_profile, CB_ADDSTRING, 0, (LPARAM)L"High-Refresh / Low-Latency (1.4x / 1.5ms)");
            SendMessageW(state->h_combo_pacing_profile, CB_ADDSTRING, 0, (LPARAM)L"Standard Presentation (Console Parity: 2.0x / 4.0ms)");
            SendMessageW(state->h_combo_pacing_profile, CB_ADDSTRING, 0, (LPARAM)L"Custom Calibration");

            state->current_profile = g_settings_config.pacing_profile;
            state->previous_preset = (g_settings_config.pacing_profile == PacingProfile::CUSTOM)
                                        ? PacingProfile::AUTO_ADAPTIVE
                                        : g_settings_config.pacing_profile;
            state->custom_spike_mult = g_settings_config.spike_multiplier;
            state->custom_min_delta = g_settings_config.min_spike_delta_ms;

            int prof_idx = 0;
            switch (state->current_profile) {
                case PacingProfile::AUTO_ADAPTIVE: prof_idx = 0; break;
                case PacingProfile::HIGH_REFRESH: prof_idx = 1; break;
                case PacingProfile::CONSERVATIVE: prof_idx = 2; break;
                case PacingProfile::CUSTOM: prof_idx = 3; break;
            }
            SendMessageW(state->h_combo_pacing_profile, CB_SETCURSEL, prof_idx, 0);
            apply_control_dark_theme(state->h_combo_pacing_profile);
            SetWindowSubclass(state->h_combo_pacing_profile, DarkComboSubclassProc, IDC_SET_COMBO_PACING_PROFILE, 0);

            if (!g_settings_config.present_threshold_manual) {
                wcscpy_s(num_buf, L"Auto");
            } else {
                double fps = (g_settings_config.present_threshold_ms > 0.0 && !std::isnan(g_settings_config.present_threshold_ms))
                    ? (1000.0 / g_settings_config.present_threshold_ms)
                    : 60.0;
                if (std::abs(fps - std::round(fps)) < 0.05) {
                    swprintf_s(num_buf, L"%.0f", fps);
                } else {
                    swprintf_s(num_buf, L"%.2f", fps);
                }
            }
            state->h_edit_target_fps = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_TARGET_FPS, NULL, NULL);
            SetWindowSubclass(state->h_edit_target_fps, EditCenteredSubclassProc, IDC_SET_EDIT_TARGET_FPS, 0);
            SendMessageW(state->h_edit_target_fps, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_target_fps);

            if (state->current_profile == PacingProfile::AUTO_ADAPTIVE) {
                wcscpy_s(num_buf, L"Auto");
            } else if (state->current_profile == PacingProfile::HIGH_REFRESH) {
                wcscpy_s(num_buf, L"1.4");
            } else if (state->current_profile == PacingProfile::CONSERVATIVE) {
                wcscpy_s(num_buf, L"2.0");
            } else {
                swprintf_s(num_buf, L"%.1f", state->custom_spike_mult);
            }
            state->h_edit_spike_mult = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_SPIKE_MULT, NULL, NULL);
            SetWindowSubclass(state->h_edit_spike_mult, EditCenteredSubclassProc, IDC_SET_EDIT_SPIKE_MULT, 0);
            SendMessageW(state->h_edit_spike_mult, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_spike_mult);

            if (state->current_profile == PacingProfile::AUTO_ADAPTIVE) {
                wcscpy_s(num_buf, L"Auto");
            } else if (state->current_profile == PacingProfile::HIGH_REFRESH) {
                wcscpy_s(num_buf, L"1.5");
            } else if (state->current_profile == PacingProfile::CONSERVATIVE) {
                wcscpy_s(num_buf, L"4.0");
            } else {
                swprintf_s(num_buf, L"%.1f", state->custom_min_delta);
            }
            state->h_edit_min_delta = CreateWindowExW(0, L"EDIT", num_buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_CENTER, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_EDIT_MIN_DELTA, NULL, NULL);
            SetWindowSubclass(state->h_edit_min_delta, EditCenteredSubclassProc, IDC_SET_EDIT_MIN_DELTA, 0);
            SendMessageW(state->h_edit_min_delta, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
            apply_control_dark_theme(state->h_edit_min_delta);

            state->h_lbl_profile_hint = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_LBL_PROFILE_HINT, NULL, NULL);
            SendMessageW(state->h_lbl_profile_hint, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
            apply_control_dark_theme(state->h_lbl_profile_hint);

            state->h_chk_judder = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_CHK_JUDDER, NULL, NULL);
            SendMessageW(state->h_chk_judder, BM_SETCHECK, g_settings_config.enable_judder_detection ? BST_CHECKED : BST_UNCHECKED, 0);
            apply_control_dark_theme(state->h_chk_judder);

            // Action Buttons
            state->h_btn_reset = CreateWindowExW(0, L"BUTTON", L"Reset Defaults", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_BTN_RESET, NULL, NULL);
            SetPropW(state->h_btn_reset, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::SecondarySlate));
            SetWindowSubclass(state->h_btn_reset, DarkButtonSubclassProc, IDC_SET_BTN_RESET, 0);

            state->h_btn_cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_BTN_CANCEL, NULL, NULL);
            SetPropW(state->h_btn_cancel, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::SecondarySlate));
            SetWindowSubclass(state->h_btn_cancel, DarkButtonSubclassProc, IDC_SET_BTN_CANCEL, 0);

            state->h_btn_save = CreateWindowExW(0, L"BUTTON", L"Save & Apply", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)IDC_SET_BTN_SAVE, NULL, NULL);
            SetPropW(state->h_btn_save, L"BtnStyle", reinterpret_cast<HANDLE>(BtnStyle::PrimaryEmerald));
            SetWindowSubclass(state->h_btn_save, DarkButtonSubclassProc, IDC_SET_BTN_SAVE, 0);

            if (!state->h_hotkey_edit || !state->h_chk_sound || !state->h_chk_redact ||
                !state->h_chk_audio || !state->h_btn_save || !state->h_btn_cancel || !state->h_combo_pacing_profile) {
                OutputDebugStringA("[GUI] Error: Failed to allocate essential controls for Settings dialog.\n");
                delete state;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                return -1;
            }

            layout_settings_controls(hwnd, state);
            update_settings_dependencies(state);
            state->suppress_change_notification = false;
            return 0;
        }

        case WM_SIZE: {
            layout_settings_controls(hwnd, state);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            POINT pt = { LOWORD(lParam), HIWORD(lParam) };
            if (state) {
                if (PtInRect(&state->rc_lbl_snd, pt)) {
                    BOOL cur = (SendMessageW(state->h_chk_sound, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    SendMessageW(state->h_chk_sound, BM_SETCHECK, cur ? BST_UNCHECKED : BST_CHECKED, 0);
                } else if (PtInRect(&state->rc_lbl_rd, pt)) {
                    BOOL cur = (SendMessageW(state->h_chk_redact, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    SendMessageW(state->h_chk_redact, BM_SETCHECK, cur ? BST_UNCHECKED : BST_CHECKED, 0);
                } else if (PtInRect(&state->rc_lbl_aud, pt)) {
                    BOOL cur = (SendMessageW(state->h_chk_audio, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    SendMessageW(state->h_chk_audio, BM_SETCHECK, cur ? BST_UNCHECKED : BST_CHECKED, 0);
                } else if (PtInRect(&state->rc_lbl_auto_save, pt)) {
                    BOOL cur = (SendMessageW(state->h_chk_auto_save, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    SendMessageW(state->h_chk_auto_save, BM_SETCHECK, cur ? BST_UNCHECKED : BST_CHECKED, 0);
                    update_settings_dependencies(state);
                    InvalidateRect(hwnd, NULL, TRUE);
                } else if (PtInRect(&state->rc_lbl_osd, pt)) {
                    BOOL cur = (SendMessageW(state->h_chk_osd, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    SendMessageW(state->h_chk_osd, BM_SETCHECK, cur ? BST_UNCHECKED : BST_CHECKED, 0);
                    update_settings_dependencies(state);
                    InvalidateRect(hwnd, NULL, TRUE);
                } else if (PtInRect(&state->rc_lbl_adv, pt)) {
                    toggle_advanced_settings(hwnd, state);
                } else if (PtInRect(&state->rc_lbl_judder, pt) && state->advanced_unlocked) {
                    BOOL cur = (SendMessageW(state->h_chk_judder, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    SendMessageW(state->h_chk_judder, BM_SETCHECK, cur ? BST_UNCHECKED : BST_CHECKED, 0);
                }
            }
            break;
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
            HBRUSH old_br = static_cast<HBRUSH>(SelectObject(mem_dc, g_theme.br_card));
            HPEN old_pen = static_cast<HPEN>(SelectObject(mem_dc, g_theme.pen_card_border));
            HFONT old_font = static_cast<HFONT>(GetCurrentObject(mem_dc, OBJ_FONT));

            FillRect(mem_dc, &client_rc, g_theme.br_bg);
            SetBkMode(mem_dc, TRANSPARENT);

            double v_scale = 1.0;
            int target_h = scale_dpi(564);
            if (height < target_h) {
                v_scale = (std::max)(0.85, static_cast<double>(height) / static_cast<double>(target_h));
            }
            auto scale_y = [v_scale](int y) -> int {
                return static_cast<int>(scale_dpi(y) * v_scale + 0.5);
            };
            int ctrl_h = (std::max)(scale_dpi(18), scale_y(24));

            const int margin = scale_dpi(16);
            const int gap = scale_dpi(14);
            const int col_w = (width - margin * 2 - gap) / 2;

            const int c_left_x = margin;
            const int c_right_x = c_left_x + col_w + gap;

            // Card 1: General Preferences (Left Top)
            const int c1_x = c_left_x;
            const int c1_y = scale_y(16);
            const int c1_w = col_w;
            const int c1_h = scale_y(236);

            // Card 3: Advanced Engine Tuning (Right Top)
            const int c3_x = c_right_x;
            const int c3_y = scale_y(16);
            const int c3_w = col_w;
            const int c3_h = scale_y(236);

            // Card 2: Frame Pacing & Judder Triggers (Left Bottom)
            const int c2_x = c_left_x;
            const int c2_y = scale_y(266);
            const int c2_w = col_w;
            const int c2_h = scale_y(232);

            // Card 4: Kernel & System Anomaly Thresholds (Right Bottom)
            const int c4_x = c_right_x;
            const int c4_y = scale_y(266);
            const int c4_w = col_w;
            const int c4_h = scale_y(232);

            SelectObject(mem_dc, g_theme.br_card);
            SelectObject(mem_dc, g_theme.pen_card_border);
            RoundRect(mem_dc, c1_x, c1_y, c1_x + c1_w, c1_y + c1_h, scale_dpi(10), scale_dpi(10));
            RoundRect(mem_dc, c3_x, c3_y, c3_x + c3_w, c3_y + c3_h, scale_dpi(10), scale_dpi(10));
            RoundRect(mem_dc, c2_x, c2_y, c2_x + c2_w, c2_y + c2_h, scale_dpi(10), scale_dpi(10));
            RoundRect(mem_dc, c4_x, c4_y, c4_x + c4_w, c4_y + c4_h, scale_dpi(10), scale_dpi(10));

            // Section Headers
            SelectObject(mem_dc, g_font_ui_sm_bold);
            SetTextColor(mem_dc, COLOR_TEXT_LABEL);

            RECT t1 = { c1_x + scale_dpi(14), c1_y + scale_dpi(8), c1_x + c1_w, c1_y + scale_dpi(24) };
            DrawTextW(mem_dc, L"GENERAL PREFERENCES", -1, &t1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            RECT t3 = { c3_x + scale_dpi(14), c3_y + scale_dpi(8), c3_x + c3_w, c3_y + scale_dpi(24) };
            DrawTextW(mem_dc, state->advanced_unlocked ? L"ADVANCED ENGINE & BUFFER TUNING" : L"ADVANCED ENGINE & BUFFER TUNING (LOCKED)", -1, &t3, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            RECT t2 = { c2_x + scale_dpi(14), c2_y + scale_dpi(8), c2_x + c2_w, c2_y + scale_dpi(24) };
            DrawTextW(mem_dc, state->advanced_unlocked ? L"FRAME PACING & JUDDER TRIGGERS" : L"FRAME PACING & JUDDER TRIGGERS (LOCKED)", -1, &t2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            RECT t4 = { c4_x + scale_dpi(14), c4_y + scale_dpi(8), c4_x + c4_w, c4_y + scale_dpi(24) };
            DrawTextW(mem_dc, state->advanced_unlocked ? L"KERNEL & SYSTEM ANOMALY THRESHOLDS" : L"KERNEL & SYSTEM ANOMALY THRESHOLDS (LOCKED)", -1, &t4, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            // Card 1 Labels (General Preferences)
            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, COLOR_TEXT_LABEL);

            int r1_y = c1_y + scale_y(30);
            RECT rc_lbl_hk = { c1_x + scale_dpi(14), r1_y, c1_x + scale_dpi(135), r1_y + ctrl_h };
            DrawTextW(mem_dc, L"Capture Hotkey:", -1, &rc_lbl_hk, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            DrawTextW(mem_dc, L"Enable Sound Cues", -1, &state->rc_lbl_snd, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            DrawTextW(mem_dc, L"Redact Personal Info (usernames & paths)", -1, &state->rc_lbl_rd, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            DrawTextW(mem_dc, L"Enable Audio Glitch Trigger", -1, &state->rc_lbl_aud, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            DrawTextW(mem_dc, L"Auto-save JSON reports to folder:", -1, &state->rc_lbl_auto_save, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            DrawTextW(mem_dc, L"Enable In-Game OSD Toast", -1, &state->rc_lbl_osd, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            int r7_osd_y = c1_y + scale_y(198);
            RECT rc_lbl_pos = { c1_x + scale_dpi(220), r7_osd_y, c1_x + scale_dpi(276), r7_osd_y + ctrl_h };
            DrawTextW(mem_dc, L"Position:", -1, &rc_lbl_pos, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            // Card 3 Labels (Advanced Engine & Buffer Tuning)
            COLORREF adv_lbl_color = state->advanced_unlocked ? COLOR_TEXT_LABEL : COLOR_TEXT_MUTED;
            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, adv_lbl_color);

            int r10_y = c3_y + scale_y(30);
            RECT rc_lbl_tier = { c3_x + scale_dpi(14), r10_y, c3_x + scale_dpi(136), r10_y + ctrl_h };
            DrawTextW(mem_dc, L"Provider Tier:", -1, &rc_lbl_tier, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            int r11_y = c3_y + scale_y(64);
            RECT rc_lbl_buf = { c3_x + scale_dpi(14), r11_y, c3_x + scale_dpi(136), r11_y + ctrl_h };
            DrawTextW(mem_dc, L"Buffer Capacity:", -1, &rc_lbl_buf, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            SelectObject(mem_dc, g_font_ui_sm_bold);
            SetTextColor(mem_dc, COLOR_TEXT_MUTED);
            RECT rc_sub_win = { c3_x + scale_dpi(14), c3_y + scale_y(100), c3_x + c3_w, c3_y + scale_y(116) };
            DrawTextW(mem_dc, L"EVENT CAPTURE WINDOWS", -1, &rc_sub_win, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, adv_lbl_color);

            int r12_y = c3_y + scale_y(128);
            RECT rc_lbl_pre = { c3_x + scale_dpi(14), r12_y, c3_x + scale_dpi(136), r12_y + ctrl_h };
            DrawTextW(mem_dc, L"Pre-Event Window:", -1, &rc_lbl_pre, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            int r13_y = c3_y + scale_y(160);
            RECT rc_lbl_post = { c3_x + scale_dpi(14), r13_y, c3_x + scale_dpi(136), r13_y + ctrl_h };
            DrawTextW(mem_dc, L"Post-Event Window:", -1, &rc_lbl_post, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            int r14_y = c3_y + scale_y(192);
            RECT rc_lbl_cd = { c3_x + scale_dpi(14), r14_y, c3_x + scale_dpi(136), r14_y + ctrl_h };
            DrawTextW(mem_dc, L"Trigger Cooldown:", -1, &rc_lbl_cd, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            SelectObject(mem_dc, g_font_ui);
            SetTextColor(mem_dc, COLOR_TEXT_MUTED);
            RECT rc_lbl_pre_unit = { c3_x + scale_dpi(204), r12_y, c3_x + c3_w - scale_dpi(14), r12_y + ctrl_h };
            DrawTextW(mem_dc, L"50.0 \u2013 1000.0 ms", -1, &rc_lbl_pre_unit, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT rc_lbl_post_unit = { c3_x + scale_dpi(204), r13_y, c3_x + c3_w - scale_dpi(14), r13_y + ctrl_h };
            DrawTextW(mem_dc, L"0.0 \u2013 200.0 ms", -1, &rc_lbl_post_unit, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT rc_lbl_cd_unit = { c3_x + scale_dpi(204), r14_y, c3_x + c3_w - scale_dpi(14), r14_y + ctrl_h };
            DrawTextW(mem_dc, L"100 \u2013 10000 ms", -1, &rc_lbl_cd_unit, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            // Card 2 Labels (Frame Pacing & Judder Triggers)
            SelectObject(mem_dc, g_font_ui_bold);

            int p1_y = c2_y + scale_y(30);
            RECT rc_lbl_tm = { c2_x + scale_dpi(14), p1_y, c2_x + scale_dpi(130), p1_y + ctrl_h };
            SetTextColor(mem_dc, adv_lbl_color);
            DrawTextW(mem_dc, L"Trigger Mode:", -1, &rc_lbl_tm, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            int tm_sel_paint = static_cast<int>(SendMessageW(state->h_combo_trig_mode, CB_GETCURSEL, 0, 0));
            FrameTriggerMode mode_paint = (tm_sel_paint == 1) ? FrameTriggerMode::DYNAMIC_ONLY : ((tm_sel_paint == 2) ? FrameTriggerMode::STATIC_ONLY : FrameTriggerMode::HYBRID);

            COLORREF prof_lbl_color = (mode_paint != FrameTriggerMode::STATIC_ONLY) ? COLOR_TEXT_LABEL : COLOR_TEXT_MUTED;
            SetTextColor(mem_dc, prof_lbl_color);
            int p2_y = c2_y + scale_y(60);
            RECT rc_lbl_prof = { c2_x + scale_dpi(14), p2_y, c2_x + scale_dpi(130), p2_y + ctrl_h };
            DrawTextW(mem_dc, L"Sensitivity Profile:", -1, &rc_lbl_prof, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            SetTextColor(mem_dc, adv_lbl_color);
            int p3_y = c2_y + scale_y(90);
            RECT rc_lbl_fps = { c2_x + scale_dpi(14), p3_y, c2_x + scale_dpi(130), p3_y + ctrl_h };
            DrawTextW(mem_dc, L"Target FPS Floor:", -1, &rc_lbl_fps, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            SelectObject(mem_dc, g_font_ui);
            SetTextColor(mem_dc, COLOR_TEXT_MUTED);
            RECT rc_lbl_fps_unit = { c2_x + scale_dpi(190), p3_y, c2_x + c2_w - scale_dpi(14), p3_y + ctrl_h };

            wchar_t fps_cur_buf[64]{};
            GetWindowTextW(state->h_edit_target_fps, fps_cur_buf, 64);
            std::wstring w_fps_cur(fps_cur_buf);
            size_t cur_start = w_fps_cur.find_first_not_of(L" \t\r\n");
            size_t cur_end = w_fps_cur.find_last_not_of(L" \t\r\n");
            if (cur_start == std::wstring::npos) {
                w_fps_cur.clear();
            } else {
                w_fps_cur = w_fps_cur.substr(cur_start, cur_end - cur_start + 1);
            }

            bool is_auto = (w_fps_cur.empty() || _wcsicmp(w_fps_cur.c_str(), L"Auto") == 0);
            if (is_auto) {
                wchar_t hint_str[128]{};
                int hz = (state->detected_display.refresh_rate_hz > 0.0 && !std::isnan(state->detected_display.refresh_rate_hz))
                    ? static_cast<int>(std::round(state->detected_display.refresh_rate_hz))
                    : 60;
                double vblank = (state->detected_display.vblank_interval_ms > 0.0 && !std::isnan(state->detected_display.vblank_interval_ms))
                    ? state->detected_display.vblank_interval_ms
                    : (1000.0 / hz);
                if (state->target_pid != 0) {
                    swprintf_s(hint_str, L"Auto (Target game display: %d Hz / %.2f ms)", hz, vblank);
                } else {
                    swprintf_s(hint_str, L"Auto (Primary display: %d Hz; adapts to game monitor on capture)", hz);
                }
                DrawTextW(mem_dc, hint_str, -1, &rc_lbl_fps_unit, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            } else {
                DrawTextW(mem_dc, L"10 \u2013 500 FPS (Manual floor)", -1, &rc_lbl_fps_unit, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            }

            // Amber warning notice under h_edit_target_fps if target FPS > 83
            if (!is_auto) {
                std::wstring w_fps_chk = w_fps_cur;
                std::replace(w_fps_chk.begin(), w_fps_chk.end(), L',', L'.');
                wchar_t* end_chk = nullptr;
                _locale_t c_locale = _create_locale(LC_ALL, "C");
                double target_fps_val = _wcstod_l(w_fps_chk.c_str(), &end_chk, c_locale);
                _free_locale(c_locale);
                if (target_fps_val > 83.0) {
                    SelectObject(mem_dc, g_font_ui);
                    SetTextColor(mem_dc, COLOR_ACCENT_AMB);
                    RECT rc_amber = { c2_x + scale_dpi(14), c2_y + scale_y(112), c2_x + c2_w - scale_dpi(14), c2_y + scale_y(128) };
                    DrawTextW(mem_dc, L"High target FPS \u2014 ordinary frame variance above 83 FPS may trigger stutter events.", -1, &rc_amber, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                }
            }

            int p4_y = c2_y + scale_y(132);
            int sm_edit_x = c2_x + scale_dpi(134);
            int sm_edit_w = scale_dpi(44);
            int md_edit_x = c2_x + scale_dpi(354);
            int md_edit_w = scale_dpi(44);

            COLORREF spike_lbl_col = (state->advanced_unlocked && state->current_profile == PacingProfile::CUSTOM && mode_paint != FrameTriggerMode::STATIC_ONLY) ? COLOR_TEXT_LABEL : COLOR_TEXT_MUTED;
            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, spike_lbl_col);

            RECT rc_lbl_sm = { c2_x + scale_dpi(14), p4_y, sm_edit_x - scale_dpi(4), p4_y + ctrl_h };
            DrawTextW(mem_dc, L"Spike Multiplier:", -1, &rc_lbl_sm, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT rc_lbl_md = { c2_x + scale_dpi(234), p4_y, md_edit_x - scale_dpi(4), p4_y + ctrl_h };
            DrawTextW(mem_dc, L"Min Spike Delta:", -1, &rc_lbl_md, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            SelectObject(mem_dc, g_font_ui);
            SetTextColor(mem_dc, COLOR_TEXT_MUTED);
            RECT rc_lbl_sm_unit = { sm_edit_x + sm_edit_w + scale_dpi(6), p4_y, c2_x + scale_dpi(228), p4_y + ctrl_h };
            DrawTextW(mem_dc, L"1.2\u201310x", -1, &rc_lbl_sm_unit, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT rc_lbl_md_unit = { md_edit_x + md_edit_w + scale_dpi(6), p4_y, c2_x + c2_w - scale_dpi(10), p4_y + ctrl_h };
            DrawTextW(mem_dc, L"1\u201350ms", -1, &rc_lbl_md_unit, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, adv_lbl_color);
            DrawTextW(mem_dc, L"Enable Presentation Judder Detection", -1, &state->rc_lbl_judder, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            // Card 4 Labels (Kernel & System Anomaly Thresholds)
            SelectObject(mem_dc, g_font_ui_sm_bold);
            SetTextColor(mem_dc, COLOR_TEXT_MUTED);

            RECT rc_sub_cpu = { c4_x + scale_dpi(14), c4_y + scale_y(28), c4_x + scale_dpi(220), c4_y + scale_y(44) };
            DrawTextW(mem_dc, L"CPU & GPU PIPELINE", -1, &rc_sub_cpu, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            RECT rc_sub_mem = { c4_x + scale_dpi(234), c4_y + scale_y(28), c4_x + c4_w - scale_dpi(14), c4_y + scale_y(44) };
            DrawTextW(mem_dc, L"MEMORY & STORAGE", -1, &rc_sub_mem, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, adv_lbl_color);

            int k1_y = c4_y + scale_y(52);
            int k2_y = c4_y + scale_y(84);
            int k3_y = c4_y + scale_y(116);
            int k4_y = c4_y + scale_y(148);
            int k5_y = c4_y + scale_y(180);

            // Left Sub-Column Labels (CPU & GPU Pipeline)
            int col1_lbl_x = c4_x + scale_dpi(14);
            int col1_lbl_w = scale_dpi(116);

            RECT rc_lbl_dpc = { col1_lbl_x, k1_y, col1_lbl_x + col1_lbl_w, k1_y + ctrl_h };
            DrawTextW(mem_dc, L"Driver DPC Spike:", -1, &rc_lbl_dpc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT rc_lbl_isr = { col1_lbl_x, k2_y, col1_lbl_x + col1_lbl_w, k2_y + ctrl_h };
            DrawTextW(mem_dc, L"Driver ISR Spike:", -1, &rc_lbl_isr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT rc_lbl_cs = { col1_lbl_x, k3_y, col1_lbl_x + col1_lbl_w, k3_y + ctrl_h };
            DrawTextW(mem_dc, L"CSwitch Preempt:", -1, &rc_lbl_cs, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT rc_lbl_smi = { col1_lbl_x, k4_y, col1_lbl_x + col1_lbl_w, k4_y + ctrl_h };
            DrawTextW(mem_dc, L"Hardware SMI Gap:", -1, &rc_lbl_smi, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT rc_lbl_d3d12 = { col1_lbl_x, k5_y, col1_lbl_x + col1_lbl_w, k5_y + ctrl_h };
            DrawTextW(mem_dc, L"D3D12 PSO Compile:", -1, &rc_lbl_d3d12, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            // Right Sub-Column Labels (Memory & Storage)
            int col2_lbl_x = c4_x + scale_dpi(234);
            int col2_lbl_w = scale_dpi(116);

            RECT rc_lbl_disk = { col2_lbl_x, k1_y, col2_lbl_x + col2_lbl_w, k1_y + ctrl_h };
            DrawTextW(mem_dc, L"Disk Latency Stall:", -1, &rc_lbl_disk, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT rc_lbl_mem_alloc = { col2_lbl_x, k2_y, col2_lbl_x + col2_lbl_w, k2_y + ctrl_h };
            DrawTextW(mem_dc, L"VirtualAlloc Stall:", -1, &rc_lbl_mem_alloc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT rc_lbl_mem_trim = { col2_lbl_x, k3_y, col2_lbl_x + col2_lbl_w, k3_y + ctrl_h };
            DrawTextW(mem_dc, L"WorkingSet Trim:", -1, &rc_lbl_mem_trim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT rc_lbl_mem_phys = { col2_lbl_x, k4_y, col2_lbl_x + col2_lbl_w, k4_y + ctrl_h };
            DrawTextW(mem_dc, L"Physical MDL Stall:", -1, &rc_lbl_mem_phys, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT rc_lbl_vram = { col2_lbl_x, k5_y, col2_lbl_x + col2_lbl_w, k5_y + ctrl_h };
            DrawTextW(mem_dc, L"VRAM Demoted Limit:", -1, &rc_lbl_vram, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            // Unit Labels
            SelectObject(mem_dc, g_font_ui);
            SetTextColor(mem_dc, COLOR_TEXT_MUTED);

            int col1_unit_x = c4_x + scale_dpi(192);
            int col2_unit_x = c4_x + scale_dpi(412);

            RECT rc_u_dpc = { col1_unit_x, k1_y, col1_unit_x + scale_dpi(26), k1_y + ctrl_h };
            DrawTextW(mem_dc, L"\u00B5s", -1, &rc_u_dpc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT rc_u_isr = { col1_unit_x, k2_y, col1_unit_x + scale_dpi(26), k2_y + ctrl_h };
            DrawTextW(mem_dc, L"\u00B5s", -1, &rc_u_isr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT rc_u_cs = { col1_unit_x, k3_y, col1_unit_x + scale_dpi(26), k3_y + ctrl_h };
            DrawTextW(mem_dc, L"ms", -1, &rc_u_cs, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT rc_u_smi = { col1_unit_x, k4_y, col1_unit_x + scale_dpi(26), k4_y + ctrl_h };
            DrawTextW(mem_dc, L"ms", -1, &rc_u_smi, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT rc_u_pso = { col1_unit_x, k5_y, col1_unit_x + scale_dpi(26), k5_y + ctrl_h };
            DrawTextW(mem_dc, L"ms", -1, &rc_u_pso, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT rc_u_disk = { col2_unit_x, k1_y, col2_unit_x + scale_dpi(28), k1_y + ctrl_h };
            DrawTextW(mem_dc, L"ms", -1, &rc_u_disk, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT rc_u_alloc = { col2_unit_x, k2_y, col2_unit_x + scale_dpi(28), k2_y + ctrl_h };
            DrawTextW(mem_dc, L"MB", -1, &rc_u_alloc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT rc_u_trim = { col2_unit_x, k3_y, col2_unit_x + scale_dpi(28), k3_y + ctrl_h };
            DrawTextW(mem_dc, L"MB", -1, &rc_u_trim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT rc_u_phys = { col2_unit_x, k4_y, col2_unit_x + scale_dpi(28), k4_y + ctrl_h };
            DrawTextW(mem_dc, L"\u00B5s", -1, &rc_u_phys, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            RECT rc_u_vram = { col2_unit_x, k5_y, col2_unit_x + scale_dpi(28), k5_y + ctrl_h };
            DrawTextW(mem_dc, L"MB", -1, &rc_u_vram, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

            // Footer: Advanced unlock label
            SelectObject(mem_dc, g_font_ui_bold);
            SetTextColor(mem_dc, state->advanced_unlocked ? COLOR_ACCENT_AMB : COLOR_TEXT_LABEL);
            DrawTextW(mem_dc, L"Unlock Advanced Settings", -1, &state->rc_lbl_adv, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            BitBlt(hdc, 0, 0, width, height, mem_dc, 0, 0, SRCCOPY);
            SelectObject(mem_dc, old_br);
            SelectObject(mem_dc, old_pen);
            SelectObject(mem_dc, old_font);
            SelectObject(mem_dc, old_bmp);
            DeleteObject(mem_bmp);
            DeleteDC(mem_dc);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdcStatic = (HDC)wParam;
            HWND hCtl = (HWND)lParam;
            if (state && hCtl == state->h_chk_advanced) {
                SetBkColor(hdcStatic, COLOR_BG);
                SetTextColor(hdcStatic, state->advanced_unlocked ? COLOR_ACCENT_AMB : COLOR_TEXT_LABEL);
                return (LRESULT)g_theme.br_bg;
            }
            if (state && hCtl == state->h_lbl_profile_hint) {
                SetBkColor(hdcStatic, COLOR_CARD_BG);
                SetTextColor(hdcStatic, COLOR_ACCENT_AMB);
                return (LRESULT)g_theme.br_card;
            }
            SetBkColor(hdcStatic, COLOR_CARD_BG);
            SetTextColor(hdcStatic, COLOR_TEXT_PRI);
            return (LRESULT)g_theme.br_card;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdcEdit = (HDC)wParam;
            HWND hCtl = (HWND)lParam;
            SetBkColor(hdcEdit, COLOR_INPUT_BG);
            if (!IsWindowEnabled(hCtl)) {
                SetTextColor(hdcEdit, COLOR_TEXT_MUTED);
            } else {
                SetTextColor(hdcEdit, COLOR_TEXT_BRIGHT);
            }
            return (LRESULT)g_theme.br_input;
        }

        case WM_CTLCOLORLISTBOX: {
            HDC hdcListBox = (HDC)wParam;
            SetBkColor(hdcListBox, COLOR_INPUT_BG);
            SetTextColor(hdcListBox, COLOR_TEXT_PRI);
            return (LRESULT)g_theme.br_input;
        }

        case WM_CTLCOLORBTN: {
            HWND hCtl = (HWND)lParam;
            SetBkMode((HDC)wParam, TRANSPARENT);
            if (state && hCtl == state->h_chk_advanced) {
                return (LRESULT)g_theme.br_bg;
            }
            return (LRESULT)g_theme.br_card;
        }

        case WM_DPICHANGED: {
            UINT new_dpi = LOWORD(wParam);
            create_theme_fonts(new_dpi);
            settings_dialog_apply_fonts(hwnd);
            const RECT* prcNewWindow = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, NULL,
                prcNewWindow->left,
                prcNewWindow->top,
                prcNewWindow->right - prcNewWindow->left,
                prcNewWindow->bottom - prcNewWindow->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            layout_settings_controls(hwnd, state);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);

            if (wmId == IDC_SET_CHK_ADVANCED) {
                toggle_advanced_settings(hwnd, state);
                return 0;
            }

            if (wmId == IDC_SET_COMBO_TRIG_MODE && HIWORD(wParam) == CBN_SELCHANGE) {
                update_settings_dependencies(state);
                InvalidateRect(hwnd, NULL, TRUE);
                return 0;
            }

            if (wmId == IDC_SET_COMBO_PACING_PROFILE && HIWORD(wParam) == CBN_SELCHANGE) {
                int sel = static_cast<int>(SendMessageW(state->h_combo_pacing_profile, CB_GETCURSEL, 0, 0));
                PacingProfile new_profile = PacingProfile::AUTO_ADAPTIVE;
                if (sel == 1) new_profile = PacingProfile::HIGH_REFRESH;
                else if (sel == 2) new_profile = PacingProfile::CONSERVATIVE;
                else if (sel == 3) new_profile = PacingProfile::CUSTOM;

                if (new_profile == PacingProfile::CUSTOM) {
                    if (!state->advanced_unlocked) {
                        int res = MessageBoxW(hwnd,
                            L"Custom Calibration allows modifying frame pacing multipliers and minimum spike deltas, which directly affect stutter detection sensitivity.\n\nAre you sure you want to unlock advanced settings?",
                            L"Advanced Settings Safeguard",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
                        if (res == IDYES) {
                            state->advanced_unlocked = true;
                            SendMessageW(state->h_chk_advanced, BM_SETCHECK, BST_CHECKED, 0);
                            state->current_profile = PacingProfile::CUSTOM;
                            wchar_t sm_buf[32], md_buf[32];
                            swprintf_s(sm_buf, L"%.1f", state->custom_spike_mult);
                            swprintf_s(md_buf, L"%.1f", state->custom_min_delta);
                            SetWindowTextW(state->h_edit_spike_mult, sm_buf);
                            SetWindowTextW(state->h_edit_min_delta, md_buf);
                        } else {
                            // Revert to previous preset
                            int prev_idx = 0;
                            switch (state->previous_preset) {
                                case PacingProfile::AUTO_ADAPTIVE: prev_idx = 0; break;
                                case PacingProfile::HIGH_REFRESH: prev_idx = 1; break;
                                case PacingProfile::CONSERVATIVE: prev_idx = 2; break;
                                default: prev_idx = 0; break;
                            }
                            SendMessageW(state->h_combo_pacing_profile, CB_SETCURSEL, prev_idx, 0);
                            state->current_profile = state->previous_preset;
                        }
                    } else {
                        state->current_profile = PacingProfile::CUSTOM;
                        wchar_t sm_buf[32], md_buf[32];
                        swprintf_s(sm_buf, L"%.1f", state->custom_spike_mult);
                        swprintf_s(md_buf, L"%.1f", state->custom_min_delta);
                        SetWindowTextW(state->h_edit_spike_mult, sm_buf);
                        SetWindowTextW(state->h_edit_min_delta, md_buf);
                    }
                } else {
                    // Preset selected
                    if (state->current_profile == PacingProfile::CUSTOM) {
                        wchar_t cbuf[64]{};
                        GetWindowTextW(state->h_edit_spike_mult, cbuf, 64);
                        std::wstring w_sm(cbuf); std::replace(w_sm.begin(), w_sm.end(), L',', L'.');
                        double v_sm = _wtof(w_sm.c_str());
                        if (v_sm >= 1.2 && v_sm <= 10.0) state->custom_spike_mult = v_sm;

                        GetWindowTextW(state->h_edit_min_delta, cbuf, 64);
                        std::wstring w_md(cbuf); std::replace(w_md.begin(), w_md.end(), L',', L'.');
                        double v_md = _wtof(w_md.c_str());
                        if (v_md >= 1.0 && v_md <= 50.0) state->custom_min_delta = v_md;
                    }
                    state->previous_preset = new_profile;
                    state->current_profile = new_profile;
                    if (new_profile == PacingProfile::AUTO_ADAPTIVE) {
                        SetWindowTextW(state->h_edit_spike_mult, L"Auto");
                        SetWindowTextW(state->h_edit_min_delta, L"Auto");
                    } else if (new_profile == PacingProfile::HIGH_REFRESH) {
                        SetWindowTextW(state->h_edit_spike_mult, L"1.4");
                        SetWindowTextW(state->h_edit_min_delta, L"1.5");
                    } else if (new_profile == PacingProfile::CONSERVATIVE) {
                        SetWindowTextW(state->h_edit_spike_mult, L"2.0");
                        SetWindowTextW(state->h_edit_min_delta, L"4.0");
                    }
                }
                update_settings_dependencies(state);
                InvalidateRect(hwnd, NULL, TRUE);
                return 0;
            }

            if (wmId == IDC_SET_EDIT_TARGET_FPS && HIWORD(wParam) == EN_CHANGE) {
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }

            if (wmId == IDC_SET_EDIT_SMI && HIWORD(wParam) == EN_CHANGE) {
                if (!state->suppress_change_notification) {
                    state->smi_threshold_manual = true;
                }
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }

            if (wmId == IDC_SET_CHK_AUTO_SAVE || wmId == IDC_SET_CHK_OSD) {
                update_settings_dependencies(state);
                InvalidateRect(hwnd, NULL, TRUE);
                return 0;
            }

            if (wmId == IDC_SET_BTN_BROWSE_AUTO_SAVE) {
                IFileOpenDialog* pFileOpen = nullptr;
                HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL, IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));
                if (SUCCEEDED(hr)) {
                    DWORD dwOptions = 0;
                    if (SUCCEEDED(pFileOpen->GetOptions(&dwOptions))) {
                        pFileOpen->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
                    }
                    pFileOpen->SetTitle(L"Select Stuttometer Auto-Save Reports Folder");
                    if (SUCCEEDED(pFileOpen->Show(hwnd))) {
                        IShellItem* pItem = nullptr;
                        if (SUCCEEDED(pFileOpen->GetResult(&pItem))) {
                            PWSTR pszFilePath = nullptr;
                            if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath))) {
                                SetWindowTextW(state->h_edit_auto_save, pszFilePath);
                                SendMessageW(state->h_chk_auto_save, BM_SETCHECK, BST_CHECKED, 0);
                                update_settings_dependencies(state);
                                CoTaskMemFree(pszFilePath);
                            }
                            pItem->Release();
                        }
                    }
                    pFileOpen->Release();
                }
                return 0;
            }

            if (wmId == IDC_SET_BTN_RESET) {
                state->suppress_change_notification = true;
                state->smi_threshold_manual = false;

                state->hotkey_vk = VK_F11;
                state->hotkey_mods = MOD_CONTROL;
                state->original_hotkey_vk = VK_F11;
                state->original_hotkey_mods = MOD_CONTROL;
                std::wstring hk_str = format_hotkey_display(state->hotkey_mods, state->hotkey_vk);
                SetWindowTextW(state->h_hotkey_edit, hk_str.c_str());

                SendMessageW(state->h_chk_sound, BM_SETCHECK, BST_CHECKED, 0);
                SendMessageW(state->h_chk_redact, BM_SETCHECK, BST_UNCHECKED, 0);
                SendMessageW(state->h_chk_audio, BM_SETCHECK, BST_CHECKED, 0);
                SendMessageW(state->h_chk_auto_save, BM_SETCHECK, BST_UNCHECKED, 0);
                SetWindowTextW(state->h_edit_auto_save, L"");
                SendMessageW(state->h_chk_osd, BM_SETCHECK, BST_UNCHECKED, 0);
                SendMessageW(state->h_combo_osd_pos, CB_SETCURSEL, 0, 0);
                state->osd_duration_ms = 3500;
                SendMessageW(state->h_combo_tier, CB_SETCURSEL, 0, 0);

                SetWindowTextW(state->h_edit_pre_win, L"250.0");
                SetWindowTextW(state->h_edit_post_win, L"30.0");
                SetWindowTextW(state->h_edit_cooldown, L"1000");
                SendMessageW(state->h_combo_buffer, CB_SETCURSEL, 2, 0);

                SetWindowTextW(state->h_edit_dpc, L"1000");
                SetWindowTextW(state->h_edit_isr, L"500");
                SetWindowTextW(state->h_edit_disk, L"20");
                SetWindowTextW(state->h_edit_cswitch, L"5");
                SetWindowTextW(state->h_edit_smi, L"33.3");
                SetWindowTextW(state->h_edit_mem_alloc, L"16");
                SetWindowTextW(state->h_edit_mem_trim, L"4");
                SetWindowTextW(state->h_edit_mem_phys, L"1000");
                SetWindowTextW(state->h_edit_d3d12_pso, L"5");
                SetWindowTextW(state->h_edit_vram_demoted, L"8");

                SendMessageW(state->h_combo_trig_mode, CB_SETCURSEL, 0, 0);

                SetWindowTextW(state->h_edit_target_fps, L"Auto");
                GuiConfig active_cfg = read_gui_config();
                state->target_pid = active_cfg.target_pid;
                state->detected_display = query_display_refresh_info(state->target_pid);

                state->current_profile = PacingProfile::AUTO_ADAPTIVE;
                state->previous_preset = PacingProfile::AUTO_ADAPTIVE;
                state->custom_spike_mult = 2.0;
                state->custom_min_delta = 4.0;
                SendMessageW(state->h_combo_pacing_profile, CB_SETCURSEL, 0, 0);
                SetWindowTextW(state->h_edit_spike_mult, L"Auto");
                SetWindowTextW(state->h_edit_min_delta, L"Auto");
                SendMessageW(state->h_chk_judder, BM_SETCHECK, BST_CHECKED, 0);

                SendMessageW(state->h_chk_advanced, BM_SETCHECK, BST_UNCHECKED, 0);
                state->advanced_unlocked = false;
                update_settings_dependencies(state);
                state->suppress_change_notification = false;
                RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
                return 0;
            }

            if (wmId == IDC_SET_BTN_CANCEL || wmId == IDCANCEL) {
                dismiss_settings_dialog(hwnd);
                return 0;
            }

            if (wmId == IDC_SET_BTN_SAVE) {
                wchar_t fps_raw_buf[64]{};
                GetWindowTextW(state->h_edit_target_fps, fps_raw_buf, 64);
                std::wstring w_fps_input(fps_raw_buf);
                size_t start = w_fps_input.find_first_not_of(L" \t\r\n");
                size_t end = w_fps_input.find_last_not_of(L" \t\r\n");
                if (start == std::wstring::npos) {
                    w_fps_input.clear();
                } else {
                    w_fps_input = w_fps_input.substr(start, end - start + 1);
                }
                std::replace(w_fps_input.begin(), w_fps_input.end(), L',', L'.');

                bool new_present_manual = false;
                double new_present_threshold_ms = 16.67;

                if (w_fps_input.empty() || _wcsicmp(w_fps_input.c_str(), L"Auto") == 0) {
                    new_present_manual = false;
                    new_present_threshold_ms = 16.67;
                } else {
                    wchar_t* end_ptr = nullptr;
                    _locale_t c_locale = _create_locale(LC_ALL, "C");
                    double v_fps = _wcstod_l(w_fps_input.c_str(), &end_ptr, c_locale);
                    _free_locale(c_locale);
                    if (end_ptr == w_fps_input.c_str() || *end_ptr != L'\0' || std::isnan(v_fps) || std::isinf(v_fps) || v_fps < 10.0 || v_fps > 500.0) {
                        MessageBoxW(hwnd, L"Please enter a valid Target FPS Floor between 10 and 500, or 'Auto'.", L"Invalid Setting", MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    new_present_manual = true;
                    new_present_threshold_ms = fps_to_present_threshold_ms(v_fps);
                }

                s_settings_saved = true;
                g_hotkey_vk = state->hotkey_vk;
                g_hotkey_mods = state->hotkey_mods;

                g_sound_cues_enabled = (SendMessageW(state->h_chk_sound, BM_GETCHECK, 0, 0) == BST_CHECKED);
                g_settings_config.redact = (SendMessageW(state->h_chk_redact, BM_GETCHECK, 0, 0) == BST_CHECKED);
                g_settings_config.enable_audio = (SendMessageW(state->h_chk_audio, BM_GETCHECK, 0, 0) == BST_CHECKED);
                g_settings_config.enable_osd = (SendMessageW(state->h_chk_osd, BM_GETCHECK, 0, 0) == BST_CHECKED);
                g_settings_config.osd_duration_ms = state->osd_duration_ms;
                int pos_sel = static_cast<int>(SendMessageW(state->h_combo_osd_pos, CB_GETCURSEL, 0, 0));
                if (pos_sel >= 0 && pos_sel <= 3) {
                    g_settings_config.osd_position = static_cast<OsdPosition>(pos_sel);
                }

                bool auto_save_checked = (SendMessageW(state->h_chk_auto_save, BM_GETCHECK, 0, 0) == BST_CHECKED);
                if (auto_save_checked) {
                    int len = GetWindowTextLengthW(state->h_edit_auto_save);
                    if (len > 0) {
                        std::wstring dir_w(len + 1, 0);
                        GetWindowTextW(state->h_edit_auto_save, dir_w.data(), len + 1);
                        dir_w.resize(len);
                        while (!dir_w.empty() && (dir_w.back() == L' ' || dir_w.back() == L'\\' || dir_w.back() == L'/')) {
                            dir_w.pop_back();
                        }
                        g_settings_config.output_dir = dir_w;
                    } else {
                        g_settings_config.output_dir.clear();
                    }
                } else {
                    g_settings_config.output_dir.clear();
                }

                int t_idx = static_cast<int>(SendMessageW(state->h_combo_tier, CB_GETCURSEL, 0, 0));
                if (t_idx == 1) g_settings_config.provider_tier = "full";
                else if (t_idx == 2) g_settings_config.provider_tier = "minimal";
                else g_settings_config.provider_tier = "standard";

                wchar_t buf[64]{};

                GetWindowTextW(state->h_edit_pre_win, buf, 64);
                std::wstring w_pre(buf); std::replace(w_pre.begin(), w_pre.end(), L',', L'.');
                double v_pre = _wtof(w_pre.c_str());
                g_settings_config.window_pre_ms = std::clamp(v_pre, 50.0, 1000.0);

                GetWindowTextW(state->h_edit_post_win, buf, 64);
                std::wstring w_post(buf); std::replace(w_post.begin(), w_post.end(), L',', L'.');
                double v_post = _wtof(w_post.c_str());
                g_settings_config.window_post_ms = std::clamp(v_post, 0.0, 200.0);

                GetWindowTextW(state->h_edit_cooldown, buf, 64);
                std::wstring w_cd(buf); std::replace(w_cd.begin(), w_cd.end(), L',', L'.');
                double v_cd = _wtof(w_cd.c_str());
                g_settings_config.cooldown_ms = std::clamp(v_cd, 100.0, 10000.0);

                int b_sel = static_cast<int>(SendMessageW(state->h_combo_buffer, CB_GETCURSEL, 0, 0));
                LRESULT b_data = SendMessageW(state->h_combo_buffer, CB_GETITEMDATA, b_sel, 0);
                if (b_data >= 65536 && b_data <= 1048576) {
                    g_settings_config.buffer_slots = static_cast<uint32_t>(b_data);
                }

                GetWindowTextW(state->h_edit_dpc, buf, 64);
                long v_dpc = _wtol(buf);
                g_settings_config.dpc_threshold_us = static_cast<uint32_t>(std::clamp(v_dpc, 100L, 50000L));

                GetWindowTextW(state->h_edit_isr, buf, 64);
                long v_isr = _wtol(buf);
                g_settings_config.isr_threshold_us = static_cast<uint32_t>(std::clamp(v_isr, 50L, 50000L));

                GetWindowTextW(state->h_edit_disk, buf, 64);
                long v_disk = _wtol(buf);
                g_settings_config.disk_threshold_ms = static_cast<uint32_t>(std::clamp(v_disk, 1L, 1000L));

                GetWindowTextW(state->h_edit_cswitch, buf, 64);
                long v_cs = _wtol(buf);
                g_settings_config.cswitch_preempt_ms = static_cast<uint32_t>(std::clamp(v_cs, 1L, 500L));

                GetWindowTextW(state->h_edit_smi, buf, 64);
                std::wstring w_smi(buf); std::replace(w_smi.begin(), w_smi.end(), L',', L'.');
                double v_smi = _wtof(w_smi.c_str());
                g_settings_config.smi_severity_threshold_ms = std::clamp(v_smi, 10.0, 100.0);

                GetWindowTextW(state->h_edit_mem_alloc, buf, 64);
                long v_mem_alloc = _wtol(buf);
                g_settings_config.mem_alloc_threshold_mb = static_cast<uint32_t>(std::clamp(v_mem_alloc, 1L, 1024L));

                GetWindowTextW(state->h_edit_mem_trim, buf, 64);
                long v_mem_trim = _wtol(buf);
                g_settings_config.mem_trim_threshold_mb = static_cast<uint32_t>(std::clamp(v_mem_trim, 1L, 1024L));

                GetWindowTextW(state->h_edit_mem_phys, buf, 64);
                long v_mem_phys = _wtol(buf);
                g_settings_config.mem_physical_latency_us = static_cast<uint32_t>(std::clamp(v_mem_phys, 50L, 50000L));

                GetWindowTextW(state->h_edit_d3d12_pso, buf, 64);
                long v_d3d12 = _wtol(buf);
                g_settings_config.d3d12_pso_threshold_ms = static_cast<uint32_t>(std::clamp(v_d3d12, 1L, 500L));

                GetWindowTextW(state->h_edit_vram_demoted, buf, 64);
                long v_vram = _wtol(buf);
                g_settings_config.vram_demoted_threshold_mb = static_cast<uint32_t>(std::clamp(v_vram, 1L, 1024L));

                int tm_idx = static_cast<int>(SendMessageW(state->h_combo_trig_mode, CB_GETCURSEL, 0, 0));
                if (tm_idx == 1) g_settings_config.frame_trigger_mode = FrameTriggerMode::DYNAMIC_ONLY;
                else if (tm_idx == 2) g_settings_config.frame_trigger_mode = FrameTriggerMode::STATIC_ONLY;
                else g_settings_config.frame_trigger_mode = FrameTriggerMode::HYBRID;

                g_settings_config.present_threshold_ms = new_present_threshold_ms;
                g_settings_config.present_threshold_manual = new_present_manual;
                g_settings_config.smi_threshold_manual = state->smi_threshold_manual;

                g_settings_config.pacing_profile = state->current_profile;
                if (state->current_profile == PacingProfile::CUSTOM) {
                    GetWindowTextW(state->h_edit_spike_mult, buf, 64);
                    std::wstring w_sm(buf); std::replace(w_sm.begin(), w_sm.end(), L',', L'.');
                    double v_sm = _wtof(w_sm.c_str());
                    g_settings_config.spike_multiplier = std::clamp(v_sm, 1.2, 10.0);

                    GetWindowTextW(state->h_edit_min_delta, buf, 64);
                    std::wstring w_md(buf); std::replace(w_md.begin(), w_md.end(), L',', L'.');
                    double v_md = _wtof(w_md.c_str());
                    g_settings_config.min_spike_delta_ms = std::clamp(v_md, 1.0, 50.0);
                } else if (state->current_profile == PacingProfile::HIGH_REFRESH) {
                    g_settings_config.spike_multiplier = HIGH_REFRESH_SPIKE_MULTIPLIER;
                    g_settings_config.min_spike_delta_ms = HIGH_REFRESH_MIN_DELTA_MS;
                } else if (state->current_profile == PacingProfile::CONSERVATIVE) {
                    g_settings_config.spike_multiplier = CONSERVATIVE_SPIKE_MULTIPLIER;
                    g_settings_config.min_spike_delta_ms = CONSERVATIVE_MIN_DELTA_MS;
                } else {
                    g_settings_config.spike_multiplier = 2.0;
                    g_settings_config.min_spike_delta_ms = 4.0;
                }

                g_settings_config.enable_judder_detection = (SendMessageW(state->h_chk_judder, BM_GETCHECK, 0, 0) == BST_CHECKED);

                if (g_hwnd_main && IsWindow(g_hwnd_main)) {
                    UnregisterHotKey(g_hwnd_main, ID_HOTKEY_TOGGLE_CAPTURE);
                    if (!RegisterHotKey(g_hwnd_main, ID_HOTKEY_TOGGLE_CAPTURE, g_hotkey_mods | MOD_NOREPEAT, g_hotkey_vk)) {
                        g_status_text = L"Hotkey Warning: " + format_hotkey_display(g_hotkey_mods, g_hotkey_vk) + L" is in use by another app";
                        append_engine_log(L"[WARN] Hotkey " + format_hotkey_display(g_hotkey_mods, g_hotkey_vk) + L" is in use by another application.");
                        MessageBoxW(hwnd, (L"Warning: The shortcut " + format_hotkey_display(g_hotkey_mods, g_hotkey_vk) + L" is currently in use by another application. Shortcut toggle capture will not be active until a different key is selected.").c_str(), L"Hotkey Conflict", MB_OK | MB_ICONWARNING);
                    }
                    update_metrics_text();
                    if (g_h_list_stutters) InvalidateRect(g_h_list_stutters, NULL, TRUE);
                    InvalidateRect(g_hwnd_main, NULL, TRUE);
                }

                save_user_settings();
                dismiss_settings_dialog(hwnd);
                return 0;
            }
            break;
        }

        case WM_NCDESTROY: {
            g_h_settings_dlg = nullptr;
            if (state) {
                delete state;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            break;
        }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

void ShowSettingsDialog(HWND hParent) {
    if (!hParent || !IsWindow(hParent)) return;

    // Temporarily unregister global hotkey while settings dialog is active to allow re-assignment
    UnregisterHotKey(hParent, ID_HOTKEY_TOGGLE_CAPTURE);
    s_settings_saved = false;

    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(hParent, GWLP_HINSTANCE);

    static bool s_class_registered = false;
    if (!s_class_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = SettingsWindowProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = L"StuttometerSettingsWindowClass";
        ATOM atom = RegisterClassExW(&wc);
        s_class_registered = (atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
    }

    UINT dpi = GetDpiForWindow(hParent);
    if (dpi == 0) dpi = 96;

    int client_w = MulDiv(960, dpi, 96);
    int client_h = MulDiv(564, dpi, 96);

    DWORD dwStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    RECT rc = { 0, 0, client_w, client_h };
    AdjustWindowRectEx(&rc, dwStyle, FALSE, 0);
    int outer_w = rc.right - rc.left;
    int outer_h = rc.bottom - rc.top;

    RECT rc_work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc_work, 0);
    int work_w = rc_work.right - rc_work.left;
    int work_h = rc_work.bottom - rc_work.top;
    if (outer_h > work_h - 20) {
        outer_h = work_h - 20;
    }
    if (outer_w > work_w - 20) {
        outer_w = work_w - 20;
    }

    RECT rc_parent{};
    GetWindowRect(hParent, &rc_parent);
    int pos_x = rc_parent.left + ((rc_parent.right - rc_parent.left) - outer_w) / 2;
    int pos_y = rc_parent.top + ((rc_parent.bottom - rc_parent.top) - outer_h) / 2;

    HWND hSettingsDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"StuttometerSettingsWindowClass",
        L"Stuttometer Settings",
        dwStyle,
        pos_x, pos_y,
        outer_w, outer_h,
        hParent, NULL, hInstance, NULL
    );

    if (!hSettingsDlg) return;
    g_h_settings_dlg = hSettingsDlg;

    apply_window_dark_titlebar(hSettingsDlg);
    EnableWindow(hParent, FALSE);
    ShowWindow(hSettingsDlg, SW_SHOW);
    UpdateWindow(hSettingsDlg);

    MSG msg{};
    while (IsWindow(hSettingsDlg) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (hParent && !IsWindow(hParent)) {
            DestroyWindow(hSettingsDlg);
            break;
        }
        if (!IsDialogMessageW(hSettingsDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!IsWindow(hSettingsDlg)) break;
    }

    if (hParent && IsWindow(hParent)) {
        EnableWindow(hParent, TRUE);
        SetForegroundWindow(hParent);
        SetFocus(hParent);

        UINT parent_dpi = GetDpiForWindow(hParent);
        if (parent_dpi == 0) parent_dpi = 96;
        if (g_current_dpi != parent_dpi) {
            create_theme_fonts(parent_dpi);
            apply_fonts_to_main_controls();
        }

        if (!s_settings_saved) {
            UnregisterHotKey(hParent, ID_HOTKEY_TOGGLE_CAPTURE);
            if (!RegisterHotKey(hParent, ID_HOTKEY_TOGGLE_CAPTURE, g_hotkey_mods | MOD_NOREPEAT, g_hotkey_vk)) {
                g_status_text = L"Hotkey Warning: " + format_hotkey_display(g_hotkey_mods, g_hotkey_vk) + L" is in use by another app";
                append_engine_log(L"[WARN] Hotkey " + format_hotkey_display(g_hotkey_mods, g_hotkey_vk) + L" is in use by another application.");
            }
            update_metrics_text();
            if (g_h_list_stutters) InvalidateRect(g_h_list_stutters, NULL, TRUE);
            InvalidateRect(hParent, NULL, TRUE);
        }
    }

    if (msg.message == WM_QUIT) {
        PostQuitMessage(static_cast<int>(msg.wParam));
    }
}


void settings_dialog_apply_fonts(HWND hDlg) {
    if (!hDlg || !IsWindow(hDlg)) return;
    auto* state = reinterpret_cast<SettingsDialogState*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));
    if (!state) return;

    if (state->h_hotkey_edit) SendMessageW(state->h_hotkey_edit, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (state->h_chk_sound) SendMessageW(state->h_chk_sound, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
    if (state->h_chk_redact) SendMessageW(state->h_chk_redact, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
    if (state->h_chk_audio) SendMessageW(state->h_chk_audio, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
    if (state->h_chk_auto_save) SendMessageW(state->h_chk_auto_save, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
    if (state->h_edit_auto_save) SendMessageW(state->h_edit_auto_save, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
    if (state->h_btn_browse_auto_save) SendMessageW(state->h_btn_browse_auto_save, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (state->h_chk_osd) SendMessageW(state->h_chk_osd, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
    if (state->h_combo_osd_pos) {
        SendMessageW(state->h_combo_osd_pos, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
        SendMessageW(state->h_combo_osd_pos, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scale_dpi(20));
        SendMessageW(state->h_combo_osd_pos, CB_SETITEMHEIGHT, (WPARAM)0, (LPARAM)scale_dpi(22));
    }
    if (state->h_chk_advanced) SendMessageW(state->h_chk_advanced, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
    if (state->h_combo_tier) {
        SendMessageW(state->h_combo_tier, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
        SendMessageW(state->h_combo_tier, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scale_dpi(20));
        SendMessageW(state->h_combo_tier, CB_SETITEMHEIGHT, (WPARAM)0, (LPARAM)scale_dpi(22));
    }
    if (state->h_edit_pre_win) SendMessageW(state->h_edit_pre_win, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (state->h_edit_post_win) SendMessageW(state->h_edit_post_win, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (state->h_edit_cooldown) SendMessageW(state->h_edit_cooldown, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (state->h_combo_buffer) {
        SendMessageW(state->h_combo_buffer, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
        SendMessageW(state->h_combo_buffer, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scale_dpi(20));
        SendMessageW(state->h_combo_buffer, CB_SETITEMHEIGHT, (WPARAM)0, (LPARAM)scale_dpi(22));
    }
    if (state->h_edit_dpc) SendMessageW(state->h_edit_dpc, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (state->h_edit_isr) SendMessageW(state->h_edit_isr, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (state->h_edit_disk) SendMessageW(state->h_edit_disk, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (state->h_edit_cswitch) SendMessageW(state->h_edit_cswitch, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (state->h_edit_smi) SendMessageW(state->h_edit_smi, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (state->h_edit_mem_alloc) SendMessageW(state->h_edit_mem_alloc, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (state->h_edit_mem_trim) SendMessageW(state->h_edit_mem_trim, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (state->h_edit_mem_phys) SendMessageW(state->h_edit_mem_phys, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (state->h_edit_d3d12_pso) SendMessageW(state->h_edit_d3d12_pso, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (state->h_edit_vram_demoted) SendMessageW(state->h_edit_vram_demoted, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (state->h_combo_trig_mode) {
        SendMessageW(state->h_combo_trig_mode, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
        SendMessageW(state->h_combo_trig_mode, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)scale_dpi(20));
        SendMessageW(state->h_combo_trig_mode, CB_SETITEMHEIGHT, (WPARAM)0, (LPARAM)scale_dpi(22));
    }
    if (state->h_edit_target_fps) SendMessageW(state->h_edit_target_fps, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (state->h_edit_spike_mult) SendMessageW(state->h_edit_spike_mult, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (state->h_edit_min_delta) SendMessageW(state->h_edit_min_delta, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (state->h_chk_judder) SendMessageW(state->h_chk_judder, WM_SETFONT, (WPARAM)g_font_ui, TRUE);
    if (state->h_btn_reset) SendMessageW(state->h_btn_reset, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (state->h_btn_cancel) SendMessageW(state->h_btn_cancel, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
    if (state->h_btn_save) SendMessageW(state->h_btn_save, WM_SETFONT, (WPARAM)g_font_ui_bold, TRUE);
}

} // namespace stuttometer::gui
