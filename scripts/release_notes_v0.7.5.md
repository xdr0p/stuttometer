## Stuttometer v0.7.5 — UI Modernization, Control Centering & Layout Polish

Stuttometer v0.7.5 delivers extensive visual polish, layout modernization, and input-control refactoring across the desktop GUI and Settings dialog.

---

### Key Changes

#### 1. Native Single-Line Centered Edit Controls & Border Insetting
- **Eliminated Win32 Multiline Incremental Typing Glitches**: Migrated all numeric threshold and multiplier edit controls from `ES_MULTILINE | ES_CENTER` to native single-line `ES_CENTER | ES_AUTOHSCROLL`. Incremental typing (e.g. typing `'1'` then `'0'`) remains strictly centered without caret tracking offsets or line-wrapping quirks.
- **Dynamic Vertical & Horizontal Insetting (`WM_NCCALCSIZE`)**: `EditCenteredSubclassProc` dynamically calculates non-client padding based on active font metrics (`top_pad = (box_h - font_h) / 2`, `horz_pad = 4px`), providing pixel-perfect vertical centering while isolating client drawing from the rounded border.
- **Border Protection (`WM_NCPAINT`)**: Implemented client-rect clipping in `WM_NCPAINT`, guaranteeing that mouse clicks, drag-selections, focus transitions, and caret rendering never clip or overwrite the 1-pixel rounded input border.
- **Disabled Edit Box Uniformity (`WM_CTLCOLORSTATIC`)**: Routed disabled `EDIT` controls to return `COLOR_INPUT_BG` (`#0F172A`) and `g_theme.br_input`, eliminating inner gray rectangle artifacts on locked controls.
- **Standardized Box Widths**: Standardized all threshold and multiplier input boxes across Settings Cards 2, 3, and 4 to `scale_dpi(48)` (48px).

#### 2. Settings Dialog Polish & Preset Synchronization
- **Inline Target FPS Warning**: Eliminated overlapping amber text warning in Card 2; high-FPS sensitivity warnings (`> 83 FPS`) are rendered inline within the unit label (`10 – 500 FPS (High floor: high sensitivity)`) in amber text with zero control collision.
- **"Custom" Profile Renaming & Sync**: Renamed `"Custom Calibration"` to `"Custom"` across presets, sensitivity profiles, and benchmark views. Selecting `Custom` preset now synchronizes and unlocks the sensitivity profile and custom multiplier fields. Selecting a diverging sensitivity profile automatically updates the preset to `Custom` to ensure consistent serialization.

#### 3. Main Dashboard Modernization
- **Configuration Card Rebalance**: Relocated the Severity Filter dropdown to Row 1 (Configuration Card), cleanly separating pre-monitoring configuration from Row 2 actions (`Start`/`Stop`/`Clear`) and live telemetry.
- **Scannable Column Alignment**: Standardized left-alignment across diagnostic table headers and cells (`Duration`, `Confidence`, `Primary Culprit / Hypothesis`) for improved visual hierarchy.
- **Updated High-Resolution Assets**: Refreshed `main_dashboard.png` and `settings_dialog.png` in `assets/` and `README.md`.

---

### Test Suite
- **18/18 test suites passing 100%** under MSVC x64 in Release mode.
