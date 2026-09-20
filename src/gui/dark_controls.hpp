#pragma once

#include <windows.h>
#include <commctrl.h>

namespace stuttometer::gui {

// Common helper: Dynamically vertically centers text in multiline edit controls based on font metrics
void apply_edit_centered_padding(HWND hwnd, HFONT hFont = nullptr);

// Custom Draw Owner-Drawn Buttons with Complete State Matrix (Zero per-frame allocations)
// Resolves parent container background solely via L"OnCard" window property.
void draw_custom_button(LPDRAWITEMSTRUCT pdis);

// Subclass procedures for dark-themed Win32 common controls
LRESULT CALLBACK EditCenteredSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
LRESULT CALLBACK ReportInspectorSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
LRESULT CALLBACK DarkButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
LRESULT CALLBACK HeaderSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
LRESULT CALLBACK ListViewSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
LRESULT CALLBACK DarkComboSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

} // namespace stuttometer::gui
