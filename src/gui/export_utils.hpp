#pragma once

#include <windows.h>

namespace stuttometer::gui {

// Copy JSON string of the currently selected stutter report to clipboard
void copy_selected_report_json(HWND hwnd);

// Export JSON report of the currently selected stutter report to file via SaveFileDialog
void export_selected_report_json(HWND hwnd);

// Copy Visual Stutter Card of the currently selected stutter report to clipboard
void copy_selected_report_card(HWND hwnd);

// Export Visual Stutter Card of the currently selected stutter report to PNG file via SaveFileDialog
void export_selected_report_card(HWND hwnd);

} // namespace stuttometer::gui
