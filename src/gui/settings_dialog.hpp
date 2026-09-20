#pragma once

#include <windows.h>

namespace stuttometer::gui {

// Display the modal Settings Dialog window
void ShowSettingsDialog(HWND hParent);

// Re-apply fonts dynamically to all settings dialog child controls on DPI change
void settings_dialog_apply_fonts(HWND hDlg);

} // namespace stuttometer::gui
