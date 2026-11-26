#pragma once

#include <windows.h>

namespace stuttometer::gui {

// Copy JSON string of the selected stutter row(s) to clipboard (Ctrl+C on table)
void copy_selected_report_json(HWND hwnd);

// Copy Visual Stutter Card of the currently selected stutter report to clipboard
void copy_selected_report_card(HWND hwnd);

// Export Visual Stutter Card of the currently selected stutter report to PNG file via SaveFileDialog
void export_selected_report_card(HWND hwnd);

// Copy entire in-memory session (all reports + BenchmarkSummary) as JSON to clipboard
void copy_session_bundle(HWND hwnd);

// Export entire in-memory session (all reports + BenchmarkSummary) to a single JSON file
void export_session_bundle(HWND hwnd);

} // namespace stuttometer::gui
