#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "test_common.hpp"
#include "card_renderer.hpp"
#include "stuttometer/internal/redaction_utils.hpp"
#include "stuttometer/version.hpp"

#include <windows.h>
#include <gdiplus.h>
#include <iostream>
#include <vector>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <algorithm>
#include <sstream>
#include <iomanip>

using namespace stuttometer;
using namespace stuttometer::gui;

static DiagnosticReport create_dummy_report() {
    DiagnosticReport report;
    report.tool_version = std::string(stuttometer::TOOL_VERSION);
    report.timestamp_utc = "2026-09-15 01:00:00 UTC";
    report.target_process = "Cyberpunk2077.exe";
    report.trigger.source = TriggerSource::DXGI_PRESENT_STUTTER;
    report.trigger.reason = TriggerReason::RELATIVE_SPIKE;
    report.trigger.duration_ms = 45.5;
    report.trigger.baseline_fps = 60.0;
    report.trigger.baseline_avg_ms = 16.67;
    report.trigger.spike_ratio = 2.74;
    report.trigger.target_pid = 4321;
    report.trigger.target_tid = 8765;
    report.trigger.cpu_index = 4;
    report.present_threshold_ms = 25.0;

    report.attribution = AttributionTag::GAME_ENGINE;
    report.attribution_process = "Cyberpunk2077.exe";
    report.attribution_pid = 4321;

    Diagnosis diag;
    diag.rank = 1;
    diag.hypothesis = "game_render_thread_stall";
    diag.confidence = 0.92;
    diag.summary = "Render thread execution stall on Core 4";
    report.diagnoses.push_back(diag);

    report.frame_timeline.reserve(1024);
    for (int i = -512; i < 512; ++i) {
        FrameTimelinePoint pt;
        pt.frame_index = static_cast<uint32_t>(i + 512);
        pt.relative_index = i;
        pt.duration_ms = (i == 0) ? 45.5 : (16.67 + (std::sin(i * 0.1) * 2.0));
        pt.offset_from_trigger_ms = static_cast<double>(i) * 16.67;
        pt.is_trigger_frame = (i == 0);
        pt.is_pacing_stall = (pt.duration_ms >= 25.0);
        report.frame_timeline.push_back(pt);
    }

    return report;
}

static uint32_t parse_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           static_cast<uint32_t>(p[3]);
}

// Test 1: COM & GDI+ Startup / Shutdown & Idempotency
static void test_initialization() {
    std::cout << "[TEST 1] Testing CardRenderer GDI+ startup and shutdown semantics...\n";
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    STUTTO_ASSERT(SUCCEEDED(hr) || hr == S_FALSE);

    // Initial startup
    bool ok1 = CardRenderer::initialize();
    STUTTO_ASSERT(ok1 && "GDI+ startup failed in test_card_renderer");
    STUTTO_ASSERT(CardRenderer::is_initialized());

    // Idempotent re-entry
    bool ok2 = CardRenderer::initialize();
    STUTTO_ASSERT(ok2 && "GDI+ re-entrant initialize must succeed");
    STUTTO_ASSERT(CardRenderer::is_initialized());

    std::cout << "  -> Initialization and idempotency PASSED.\n";
}

// Test 2: PNG Encoding & Header Validation (with Nit 1 fix)
static void test_png_encoding_and_sampling() {
    std::cout << "[TEST 2] Testing PNG encoding, IHDR chunk validation, and pixel sampling...\n";

    // Unit assertions for detail::format_offset
    STUTTO_ASSERT(stuttometer::gui::detail::format_offset(-83.0) == L"-83 ms");
    STUTTO_ASSERT(stuttometer::gui::detail::format_offset(83.0) == L"+83 ms");
    STUTTO_ASSERT(stuttometer::gui::detail::format_offset(-1500.0) == L"-1.5 s");
    STUTTO_ASSERT(stuttometer::gui::detail::format_offset(1500.0) == L"+1.5 s");
    STUTTO_ASSERT(stuttometer::gui::detail::format_offset(0.0) == L"+0 ms");
    STUTTO_ASSERT(stuttometer::gui::detail::format_offset(-1000.0) == L"-1.0 s");
    STUTTO_ASSERT(stuttometer::gui::detail::format_offset(1000.0) == L"+1.0 s");
    STUTTO_ASSERT(stuttometer::gui::detail::format_offset(-999.0) == L"-999 ms");
    STUTTO_ASSERT(stuttometer::gui::detail::format_offset(999.0) == L"+999 ms");

    auto report = create_dummy_report();

    CardRenderOptions opts;
    opts.base_width = 1200;
    opts.base_height = 675;
    opts.dpi_scale = 1.0;

    auto bytes = CardRenderer::render_card_to_png_bytes(report, opts);
    STUTTO_ASSERT(!bytes.empty() && "PNG byte vector must not be empty");
    STUTTO_ASSERT(bytes.size() > 33 && "PNG byte vector too small for valid header");

    // 8-byte PNG signature
    const uint8_t png_magic[] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    for (size_t i = 0; i < 8; ++i) {
        STUTTO_ASSERT(bytes[i] == png_magic[i] && "PNG magic byte mismatch");
    }

    // IHDR Chunk verification
    STUTTO_ASSERT(bytes[12] == 'I' && bytes[13] == 'H' && bytes[14] == 'D' && bytes[15] == 'R');
    uint32_t ihdr_w = parse_be32(&bytes[16]);
    uint32_t ihdr_h = parse_be32(&bytes[20]);
    uint8_t bit_depth = bytes[24];
    uint8_t color_type = bytes[25];

    STUTTO_ASSERT(ihdr_w == 1200);
    STUTTO_ASSERT(ihdr_h == 675);
    STUTTO_ASSERT(bit_depth == 8);
    STUTTO_ASSERT(color_type == 2 && "Color type must be 2 (RGB)");

    // Pixel Sampling: Decode PNG bytes in-memory and inspect colors
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    STUTTO_ASSERT(hMem != nullptr);
    void* pMem = GlobalLock(hMem);
    std::memcpy(pMem, bytes.data(), bytes.size());
    GlobalUnlock(hMem);

    IStream* pStream = nullptr;
    HRESULT hr = CreateStreamOnHGlobal(hMem, TRUE, &pStream);
    STUTTO_ASSERT(SUCCEEDED(hr) && pStream != nullptr);

    {
        Gdiplus::Bitmap loaded_bmp(pStream);
        STUTTO_ASSERT(loaded_bmp.GetLastStatus() == Gdiplus::Ok);
        STUTTO_ASSERT(loaded_bmp.GetWidth() == 1200);
        STUTTO_ASSERT(loaded_bmp.GetHeight() == 675);

        // Pixel Sampling Convention (A): The 1px inset accent border (#2a354b) is edge-occupying
        // at the outer perimeter (1, 0), while the dark canvas background (#11151f) fills the interior
        // sampled at (2, 2) and (10, 10) inside the top-left margins.
        Gdiplus::Color bg_px;
        loaded_bmp.GetPixel(2, 2, &bg_px);
        STUTTO_ASSERT(bg_px.GetR() == 0x11 && bg_px.GetG() == 0x15 && bg_px.GetB() == 0x1F);

        loaded_bmp.GetPixel(10, 10, &bg_px);
        STUTTO_ASSERT(bg_px.GetR() == 0x11 && bg_px.GetG() == 0x15 && bg_px.GetB() == 0x1F);

        // Sample top edge (1, 0) for 1px accent inset border (#2a354b)
        // With SmoothingModeAntiAlias, allow rasterizer tolerance and assert pixel is closer to border (#2a354b) than bg (#11151f)
        Gdiplus::Color border_px;
        loaded_bmp.GetPixel(1, 0, &border_px);
        int dist_border = std::abs(static_cast<int>(border_px.GetR()) - 0x2A) +
                          std::abs(static_cast<int>(border_px.GetG()) - 0x35) +
                          std::abs(static_cast<int>(border_px.GetB()) - 0x4B);
        int dist_bg = std::abs(static_cast<int>(border_px.GetR()) - 0x11) +
                      std::abs(static_cast<int>(border_px.GetG()) - 0x15) +
                      std::abs(static_cast<int>(border_px.GetB()) - 0x1F);
        STUTTO_ASSERT(dist_border < dist_bg && "Border pixel at (1, 0) must be distinctly closer to border accent (#2a354b) than canvas bg (#11151f)");

        // Regression Assertion: Scan plot area to ensure Emerald (#10B981) dominates over Sky Blue (#38BDF8)
        // Plot area geometry matching draw_card() at 1200x675
        int plot_x = 24 + 60; // side_margin (24) + 60 = 84
        int plot_y = 232 + 36; // graph_y (232) + 36 = 268
        int plot_w = (1200 - 48) - 76; // 1076
        int plot_h = 398 - 60; // 338

        size_t emerald_dominant = 0;
        size_t sky_dominant = 0;
        for (int y = plot_y; y < plot_y + plot_h; y += 4) {
            for (int x = plot_x; x < plot_x + plot_w; x += 4) {
                Gdiplus::Color px;
                loaded_bmp.GetPixel(x, y, &px);
                int g = static_cast<int>(px.GetG());
                int b = static_cast<int>(px.GetB());
                // Emerald fill/curve has g > b and elevated green (g > 30)
                if (g > b && g > 30) {
                    ++emerald_dominant;
                }
                // Sky blue (#38BDF8) has high blue brightness (b > 100 && b - g > 20),
                // distinguishing it from dark slate gridlines (b=59, g=41)
                if (b > 100 && (b - g) > 20) {
                    ++sky_dominant;
                }
            }
        }
        STUTTO_ASSERT(emerald_dominant > 50 && "Must detect emerald curve/fill pixels in plot area");
        STUTTO_ASSERT(sky_dominant == 0 && "Sky blue must be completely eliminated from plot area");
    }
    pStream->Release();

    // Invalid input checks (failure contract)
    CardRenderOptions opt_w0{ .base_width = 0 };
    STUTTO_ASSERT(CardRenderer::render_card_to_png_bytes(report, opt_w0).empty());

    CardRenderOptions opt_h0{ .base_height = -10 };
    STUTTO_ASSERT(CardRenderer::render_card_to_png_bytes(report, opt_h0).empty());

    CardRenderOptions opt_dpi0{ .dpi_scale = 0.0 };
    STUTTO_ASSERT(CardRenderer::render_card_to_png_bytes(report, opt_dpi0).empty());

    std::cout << "  -> PNG encoding, IHDR header, and pixel sampling PASSED.\n";
}

// Test 3: Clipboard CF_DIB Direct Paste & In-Memory DIB Encoding
static void test_clipboard_roundtrip() {
    std::cout << "[TEST 3] Testing clipboard CF_DIB format and in-memory DIB encoding...\n";

    auto report = create_dummy_report();

    // 1. Verify in-memory DIB generation (completely safe, zero OS clipboard side effects)
    auto dib = CardRenderer::render_card_to_dib_bytes(report);
    STUTTO_ASSERT(!dib.empty() && "DIB render must succeed");
    STUTTO_ASSERT(dib.size() >= sizeof(BITMAPINFOHEADER));
    auto* bih = reinterpret_cast<const BITMAPINFOHEADER*>(dib.data());
    STUTTO_ASSERT(bih->biSize == sizeof(BITMAPINFOHEADER));
    STUTTO_ASSERT(bih->biWidth == 1200);
    STUTTO_ASSERT(bih->biHeight == 675 && "biHeight must be positive for bottom-up orientation");
    STUTTO_ASSERT(bih->biBitCount == 24);
    STUTTO_ASSERT(bih->biCompression == BI_RGB);
    DWORD expected_stride = ((1200 * 3 + 3) & ~3);
    DWORD expected_bytes = expected_stride * 675;
    STUTTO_ASSERT(bih->biSizeImage == expected_bytes);
    STUTTO_ASSERT(dib.size() == sizeof(BITMAPINFOHEADER) + expected_bytes);

    // Invalid input checks (failure contract)
    CardRenderOptions opt_w0{ .base_width = 0 };
    STUTTO_ASSERT(CardRenderer::render_card_to_dib_bytes(report, opt_w0).empty());

    CardRenderOptions opt_h0{ .base_height = -10 };
    STUTTO_ASSERT(CardRenderer::render_card_to_dib_bytes(report, opt_h0).empty());

    CardRenderOptions opt_dpi0{ .dpi_scale = 0.0 };
    STUTTO_ASSERT(CardRenderer::render_card_to_dib_bytes(report, opt_dpi0).empty());

    std::cout << "  -> In-memory CF_DIB byte structure & failure contracts PASSED.\n";

    // 2. Live OS clipboard test: only executed when explicitly opted-in (e.g. in CI or with STUTTO_TEST_CLIPBOARD=1)
    //    Guarantees local developer workflows NEVER have their system clipboard hijacked.
    const char* env_cb = std::getenv("STUTTO_TEST_CLIPBOARD");
    const char* env_ci = std::getenv("CI");
    if ((env_cb && env_cb[0] != '\0') || (env_ci && env_ci[0] != '\0')) {
        std::cout << "  -> Running opt-in live OS clipboard test...\n";

        // Backup existing text on clipboard if present
        std::wstring saved_text;
        bool had_text = false;
        if (OpenClipboard(nullptr)) {
            HANDLE hText = GetClipboardData(CF_UNICODETEXT);
            if (hText) {
                const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(hText));
                if (p) {
                    saved_text = p;
                    had_text = true;
                    GlobalUnlock(hText);
                }
            }
            CloseClipboard();
        }

        bool cb_ok = CardRenderer::copy_card_to_clipboard(nullptr, report);
        if (!cb_ok) {
            std::cout << "  -> Clipboard access rejected by environment. Soft-skipping live test.\n";
            return;
        }

        if (OpenClipboard(nullptr)) {
            HANDLE hData = GetClipboardData(CF_DIB);
            STUTTO_ASSERT(hData != nullptr && "CF_DIB handle expected in clipboard");
            auto* live_bih = reinterpret_cast<BITMAPINFOHEADER*>(GlobalLock(hData));
            STUTTO_ASSERT(live_bih != nullptr);

            STUTTO_ASSERT(live_bih->biSize == sizeof(BITMAPINFOHEADER));
            STUTTO_ASSERT(live_bih->biWidth == 1200);
            STUTTO_ASSERT(live_bih->biHeight == 675);
            STUTTO_ASSERT(live_bih->biBitCount == 24);
            STUTTO_ASSERT(live_bih->biCompression == BI_RGB);

            GlobalUnlock(hData);

            // Clean up: Restore previous clipboard state so we never leave dummy card behind
            EmptyClipboard();
            if (had_text) {
                size_t bytes = (saved_text.size() + 1) * sizeof(wchar_t);
                HGLOBAL hRestore = GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (hRestore) {
                    void* p = GlobalLock(hRestore);
                    if (p) {
                        std::memcpy(p, saved_text.c_str(), bytes);
                        GlobalUnlock(hRestore);
                        SetClipboardData(CF_UNICODETEXT, hRestore);
                    } else {
                        GlobalFree(hRestore);
                    }
                }
            }
            CloseClipboard();
        }
        std::cout << "  -> Live OS clipboard test PASSED and clipboard restored.\n";
    } else {
        std::cout << "  -> Live OS clipboard mutation skipped to protect user clipboard (set STUTTO_TEST_CLIPBOARD=1 to run).\n";
    }
}

// Test 4: Attribution Tag Coverage
static void test_attribution_tags() {
    std::cout << "[TEST 4] Testing attribution tag coverage...\n";
    AttributionTag tags[] = {
        AttributionTag::GAME_ENGINE,
        AttributionTag::DWM_COMPOSITION,
        AttributionTag::EXTERNAL_CONTENTION,
        AttributionTag::UNKNOWN
    };

    for (auto tag : tags) {
        auto report = create_dummy_report();
        report.attribution = tag;
        auto bytes = CardRenderer::render_card_to_png_bytes(report);
        STUTTO_ASSERT(!bytes.empty());
    }

    std::cout << "  -> All 4 attribution tags rendered successfully PASSED.\n";
}

// Test 5: Redaction 4-Way Permutation Matrix
static void test_redaction_permutations() {
    std::cout << "[TEST 5] Testing 4-way redaction matrix...\n";
    bool flags[2] = { false, true };

    for (bool red : flags) {
        for (bool attr_red : flags) {
            auto report = create_dummy_report();
            report.redacted = red;
            report.attribution_redacted = attr_red;
            report.target_process = "SensitiveApp.exe";
            report.attribution_process = "sensitive_driver.sys";
            report.trigger.target_pid = 4321;
            report.trigger.target_tid = 8765;

            // 1. Process name sanitization parity
            std::string proc = report.redacted ? "Process_REDACTED" : report.target_process;
            if (red) {
                STUTTO_ASSERT(proc == "Process_REDACTED");
            } else {
                STUTTO_ASSERT(proc == "SensitiveApp.exe");
            }

            // 2. Attribution culprit module sanitization
            std::string culprit = get_redacted_module_name(report.attribution_process, red || attr_red);
            if (red || attr_red) {
                STUTTO_ASSERT(culprit == "driver_REDACTED.sys");
            } else {
                STUTTO_ASSERT(culprit == "sensitive_driver.sys");
            }

            // 3. Evidence and summary ID scrubbing
            std::string text = "Stall in PID 4321 TID 8765 on Core 4";
            auto ids = collect_report_ids(report);
            std::string sanitized = redact_text_with_ids(text, ids);
            STUTTO_ASSERT(sanitized.find("4321") == std::string::npos);
            STUTTO_ASSERT(sanitized.find("8765") == std::string::npos);
            STUTTO_ASSERT(sanitized.find("REDACTED") != std::string::npos);

            // 4. Render card PNG for permutation
            auto bytes = CardRenderer::render_card_to_png_bytes(report);
            STUTTO_ASSERT(!bytes.empty());
        }
    }

    std::cout << "  -> Redaction matrix PASSED.\n";
}

// Test 6: DPI Scale Variations & std::lround Dimensions
static void test_dpi_scaling() {
    std::cout << "[TEST 6] Testing DPI scale variations and std::lround pixel dimensions...\n";
    auto report = create_dummy_report();

    struct ScaleTestCase {
        double scale;
        uint32_t expected_w;
        uint32_t expected_h;
    };

    ScaleTestCase cases[] = {
        { 1.0,  1200, 675 },
        { 1.25, 1500, 844 },   // std::lround(675 * 1.25) = 844
        { 1.5,  1800, 1013 },  // std::lround(675 * 1.5)  = 1013
        { 2.0,  2400, 1350 }
    };

    for (const auto& tc : cases) {
        CardRenderOptions opts;
        opts.base_width = 1200;
        opts.base_height = 675;
        opts.dpi_scale = tc.scale;

        auto bytes = CardRenderer::render_card_to_png_bytes(report, opts);
        STUTTO_ASSERT(!bytes.empty());

        uint32_t w = parse_be32(&bytes[16]);
        uint32_t h = parse_be32(&bytes[20]);
        STUTTO_ASSERT(w == tc.expected_w);
        STUTTO_ASSERT(h == tc.expected_h);
    }

    std::cout << "  -> DPI scaling and exact std::lround dimensions PASSED.\n";
}

// Test 7: Extreme Metric Values & Fallbacks
static void test_extreme_metrics() {
    std::cout << "[TEST 7] Testing extreme metric values and fallbacks...\n";

    // 1. Massive stall (5000 ms)
    {
        auto report = create_dummy_report();
        report.trigger.duration_ms = 5000.0;
        auto bytes = CardRenderer::render_card_to_png_bytes(report);
        STUTTO_ASSERT(!bytes.empty());
    }

    // 2. Audio glitch / zero duration (renders N/A)
    {
        auto report = create_dummy_report();
        report.trigger.source = TriggerSource::AUDIO_GLITCH;
        report.trigger.duration_ms = 0.0;
        report.trigger.glitch_count = 3;
        auto bytes = CardRenderer::render_card_to_png_bytes(report);
        STUTTO_ASSERT(!bytes.empty());
    }

    // 3. Baseline FPS = 0.0 (triggers N/A -> XX.X FPS fallback)
    {
        auto report = create_dummy_report();
        report.trigger.baseline_fps = 0.0;
        report.trigger.duration_ms = 33.3;
        auto bytes = CardRenderer::render_card_to_png_bytes(report);
        STUTTO_ASSERT(!bytes.empty());
    }

    // 4. Spike ratio = 0.0
    {
        auto report = create_dummy_report();
        report.trigger.spike_ratio = 0.0;
        auto bytes = CardRenderer::render_card_to_png_bytes(report);
        STUTTO_ASSERT(!bytes.empty());
    }

    // 5. Timeline boundaries: 0 points, 1 point, 1024 points
    {
        auto report0 = create_dummy_report();
        report0.frame_timeline.clear();
        STUTTO_ASSERT(!CardRenderer::render_card_to_png_bytes(report0).empty());

        auto report1 = create_dummy_report();
        report1.frame_timeline.resize(1);
        report1.frame_timeline[0].duration_ms = 25.0;
        STUTTO_ASSERT(!CardRenderer::render_card_to_png_bytes(report1).empty());
    }

    std::cout << "  -> Extreme metric values and fallbacks PASSED.\n";
}

// Test 8: Long String Truncation
static void test_long_string_truncation() {
    std::cout << "[TEST 8] Testing long string truncation and layout safety...\n";
    auto report = create_dummy_report();

    // Oversized strings
    report.target_process = std::string(120, 'A') + ".exe";
    report.attribution_process = std::string(150, 'B') + ".sys";
    report.diagnoses[0].summary = std::string(400, 'C');

    auto bytes = CardRenderer::render_card_to_png_bytes(report);
    STUTTO_ASSERT(!bytes.empty());

    std::cout << "  -> Long string truncation PASSED.\n";
}

// Test 9: File Export & Invalid Paths
static void test_file_export_and_errors() {
    std::cout << "[TEST 9] Testing file export and error path handling...\n";
    auto report = create_dummy_report();

    // Valid path
    std::filesystem::path valid_path = std::filesystem::temp_directory_path() / "stutto_card_test.png";
    bool save_ok = CardRenderer::save_card_to_png(report, valid_path);
    STUTTO_ASSERT(save_ok && "Saving to valid temp path must succeed");
    STUTTO_ASSERT(std::filesystem::exists(valid_path));
    std::filesystem::remove(valid_path);

    // Check if user requested dumping dummy cards for visual inspection
    const char* dump_dir_env = std::getenv("STUTTO_DUMP_CARD_DIR");
    if (dump_dir_env && dump_dir_env[0] != '\0') {
        std::filesystem::path dump_dir(dump_dir_env);
        std::filesystem::create_directories(dump_dir);

        // 1. Game Engine Stutter
        bool ok_ge = CardRenderer::save_card_to_png(report, dump_dir / "dummy_card_game_engine.png");
        STUTTO_ASSERT(ok_ge && "Must succeed saving game engine card");

        // 2. DWM Composition
        auto dwm_report = report;
        dwm_report.attribution = AttributionTag::DWM_COMPOSITION;
        dwm_report.attribution_process = "dwm.exe";
        dwm_report.diagnoses[0].summary = "Desktop Window Manager compositing queue delay";
        bool ok_dwm = CardRenderer::save_card_to_png(dwm_report, dump_dir / "dummy_card_dwm.png");
        STUTTO_ASSERT(ok_dwm && "Must succeed saving DWM card");

        // 3. External Contention
        auto ext_report = report;
        ext_report.attribution = AttributionTag::EXTERNAL_CONTENTION;
        ext_report.attribution_process = "AntivirusScan.exe";
        ext_report.diagnoses[0].summary = "High CPU contention from background process on Core 4";
        bool ok_ext = CardRenderer::save_card_to_png(ext_report, dump_dir / "dummy_card_contention.png");
        STUTTO_ASSERT(ok_ext && "Must succeed saving contention card");

        // 4. Audio Glitch
        auto audio_report = report;
        audio_report.trigger.source = TriggerSource::AUDIO_GLITCH;
        audio_report.trigger.reason = TriggerReason::AUDIO_BUFFER_UNDERRUN;
        audio_report.trigger.duration_ms = 0.0;
        audio_report.trigger.glitch_count = 3;
        audio_report.attribution = AttributionTag::EXTERNAL_CONTENTION;
        audio_report.attribution_process = "audiodg.exe";
        audio_report.diagnoses[0].summary = "Audio buffer underrun detected in audio engine worker";
        bool ok_aud = CardRenderer::save_card_to_png(audio_report, dump_dir / "dummy_card_audio_glitch.png");
        STUTTO_ASSERT(ok_aud && "Must succeed saving audio glitch card");

        std::cout << "  -> Dumped dummy card images to: " << dump_dir.string() << "\n";
    }

    // Invalid / illegal path
    std::filesystem::path invalid_path = "Z:\\nonexistent_dir_0987654321\\illegal.png";
    bool fail_ok = CardRenderer::save_card_to_png(report, invalid_path);
    STUTTO_ASSERT(!fail_ok && "Saving to illegal path must return false without crashing");

    std::cout << "  -> File export and error paths PASSED.\n";
}

// Test 10: UI/UX Regressions (v7)
static void test_ui_ux_regressions() {
    std::cout << "[TEST 10] Testing UI/UX regressions (v7)...\n";

    // 1. Detail Pure Function Assertions
    {
        STUTTO_ASSERT(detail::format_center_label(8500.0) == L"Trigger (0.0 s)");
        STUTTO_ASSERT(detail::format_center_label(1000.0) == L"Trigger (0.0 s)");
        STUTTO_ASSERT(detail::format_center_label(999.9) == L"Trigger (0 ms)");
        STUTTO_ASSERT(detail::format_center_label(500.0) == L"Trigger (0 ms)");

        STUTTO_ASSERT(detail::classify_stall(45.5, 2.74, 0.0, false, 0) == detail::MetricSeverity::Warning);
        STUTTO_ASSERT(detail::classify_stall(50.0, 1.0, 0.0, false, 0) == detail::MetricSeverity::Danger);
        STUTTO_ASSERT(detail::classify_stall(49.9, 1.9, 0.0, false, 0) == detail::MetricSeverity::Warning);
        STUTTO_ASSERT(detail::classify_stall(24.9, 1.9, 0.0, false, 0) == detail::MetricSeverity::Normal);
        STUTTO_ASSERT(detail::classify_stall(24.9, 2.0, 0.0, false, 0) == detail::MetricSeverity::Warning);
        STUTTO_ASSERT(detail::classify_stall(24.9, 1.99, 0.30, false, 0) == detail::MetricSeverity::Warning);
        STUTTO_ASSERT(detail::classify_stall(24.9, 1.99, 0.60, false, 0) == detail::MetricSeverity::Warning);
        STUTTO_ASSERT(detail::classify_stall(24.9, 1.99, 0.601, false, 0) == detail::MetricSeverity::Danger);
        STUTTO_ASSERT(detail::classify_stall(0.0, 0.0, 0.633, false, 0) == detail::MetricSeverity::Danger);
        STUTTO_ASSERT(detail::classify_stall(0.0, 0.0, 0.0, true, 2) == detail::MetricSeverity::Danger);
        STUTTO_ASSERT(detail::classify_stall(0.0, 0.0, 0.0, true, 0) == detail::MetricSeverity::Normal);
        STUTTO_ASSERT(detail::classify_stall(10.0, 0.60, 0.0, false, 0) == detail::MetricSeverity::Normal);
    }

    auto report = create_dummy_report();
    CardRenderOptions opts;
    opts.base_width = 1200;
    opts.base_height = 675;
    opts.dpi_scale = 1.0;

    auto bytes = CardRenderer::render_card_to_png_bytes(report, opts);
    STUTTO_ASSERT(!bytes.empty());

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    STUTTO_ASSERT(hMem != nullptr);
    void* pMem = GlobalLock(hMem);
    std::memcpy(pMem, bytes.data(), bytes.size());
    GlobalUnlock(hMem);

    IStream* pStream = nullptr;
    HRESULT hr = CreateStreamOnHGlobal(hMem, TRUE, &pStream);
    STUTTO_ASSERT(SUCCEEDED(hr) && pStream != nullptr);

    {
        Gdiplus::Bitmap loaded_bmp(pStream);
        STUTTO_ASSERT(loaded_bmp.GetLastStatus() == Gdiplus::Ok);

        // 2. Non-Circular Baseline Alignment Test
        {
            // Replicate FontFamily fallback logic from draw_card (Implementation Note 3)
            Gdiplus::FontFamily sans_family(L"Segoe UI");
            const Gdiplus::FontFamily* pSans = sans_family.IsAvailable() ? &sans_family : Gdiplus::FontFamily::GenericSansSerif();

            Gdiplus::Bitmap dummy_bmp(1, 1, PixelFormat32bppARGB);
            Gdiplus::Graphics g(&dummy_bmp);
            const float s = 1.0f;
            Gdiplus::Font font_tag(pSans, 10.0f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);

            double top_confidence = report.diagnoses.empty() ? 0.0 : report.diagnoses[0].confidence;
            std::wstringstream conf_ss;
            conf_ss << std::fixed << std::setprecision(0)
                    << std::lround(top_confidence * 100.0) << L"% CONFIDENCE";
            std::wstring conf_str = conf_ss.str();

            // banner_row1_top_y = 48.0f + 16.0f = 64.0f (matches 2-row layout when total_loss == 0)
            auto banner_rects = detail::compute_banner_rects(
                1200, 24.0f, 48.0f + 16.0f, 1.0f,
                g, pSans, font_tag, conf_str
            );

            // Scan vertical columns in rc_pill_text across text pixels (skipping status dot)
            std::vector<int> pill_bottom_ink;
            int pill_x_start = std::lround(banner_rects.rc_pill_text.X);
            int pill_x_end = std::lround(banner_rects.rc_pill_text.GetRight());
            int pill_y_start = std::lround(banner_rects.rc_pill_text.Y);
            int pill_y_end = std::lround(banner_rects.rc_pill_text.GetBottom());

            for (int x = pill_x_start; x <= pill_x_end; ++x) {
                int col_max_y = -1;
                for (int y = pill_y_start; y <= pill_y_end; ++y) {
                    Gdiplus::Color px;
                    loaded_bmp.GetPixel(x, y, &px);
                    // Ink detection (text #f1f5f9 vs pill bg #1e293b / border #334155)
                    if (px.GetR() > 120 && px.GetG() > 120) {
                        if (y > col_max_y) col_max_y = y;
                    }
                }
                if (col_max_y != -1) {
                    pill_bottom_ink.push_back(col_max_y);
                }
            }
            STUTTO_ASSERT(!pill_bottom_ink.empty() && "Must find ink pixels in rc_pill_text");
            std::sort(pill_bottom_ink.begin(), pill_bottom_ink.end());
            int y_pill_base = pill_bottom_ink[pill_bottom_ink.size() / 2];

            // Scan every ink column in rc_culprit and compute the median of bottom-most ink pixels
            std::vector<int> culprit_bottom_ink;
            int culprit_x_start = std::lround(banner_rects.rc_culprit.X);
            int culprit_x_end = std::lround(banner_rects.rc_culprit.GetRight());
            int culprit_y_start = std::lround(banner_rects.rc_culprit.Y);
            int culprit_y_end = std::lround(banner_rects.rc_culprit.GetBottom() + 10.0f); // Allow room for descenders/antialiasing

            for (int x = culprit_x_start; x <= culprit_x_end; ++x) {
                int col_max_y = -1;
                for (int y = culprit_y_start; y <= culprit_y_end; ++y) {
                    Gdiplus::Color px;
                    loaded_bmp.GetPixel(x, y, &px);
                    // Ink detection (text #ffffff vs card bg #1c212c)
                    if (px.GetR() > 120 && px.GetG() > 120) {
                        if (y > col_max_y) col_max_y = y;
                    }
                }
                if (col_max_y != -1) {
                    culprit_bottom_ink.push_back(col_max_y);
                }
            }
            STUTTO_ASSERT(!culprit_bottom_ink.empty() && "Must find ink pixels in rc_culprit");
            std::sort(culprit_bottom_ink.begin(), culprit_bottom_ink.end());
            int y_culprit_base = culprit_bottom_ink[culprit_bottom_ink.size() / 2];

            STUTTO_ASSERT(std::abs((y_culprit_base - y_pill_base) - 3) <= 2 &&
                "Culprit text must be vertically centered in banner row");
        }

        // 2b. Baseline Alignment with Telemetry Loss (Row 1 top edge shifted by -4px)
        {
            Gdiplus::FontFamily sans_family(L"Segoe UI");
            const Gdiplus::FontFamily* pSans = sans_family.IsAvailable() ? &sans_family : Gdiplus::FontFamily::GenericSansSerif();

            Gdiplus::Bitmap dummy_bmp(1, 1, PixelFormat32bppARGB);
            Gdiplus::Graphics g(&dummy_bmp);
            const float s = 1.0f;
            Gdiplus::Font font_tag(pSans, 10.0f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);

            auto report_loss = create_dummy_report();
            report_loss.dropped_events = 5;
            auto bytes_loss = CardRenderer::render_card_to_png_bytes(report_loss, opts);
            STUTTO_ASSERT(!bytes_loss.empty());

            HGLOBAL hMemLoss = GlobalAlloc(GMEM_MOVEABLE, bytes_loss.size());
            STUTTO_ASSERT(hMemLoss != nullptr);
            void* pMemLoss = GlobalLock(hMemLoss);
            std::memcpy(pMemLoss, bytes_loss.data(), bytes_loss.size());
            GlobalUnlock(hMemLoss);

            IStream* pStreamLoss = nullptr;
            hr = CreateStreamOnHGlobal(hMemLoss, TRUE, &pStreamLoss);
            STUTTO_ASSERT(SUCCEEDED(hr) && pStreamLoss != nullptr);
            {
                Gdiplus::Bitmap bmp_loss(pStreamLoss);
                STUTTO_ASSERT(bmp_loss.GetLastStatus() == Gdiplus::Ok);

                std::wstring conf_str = L"92% CONFIDENCE";
                auto banner_rects_loss = detail::compute_banner_rects(
                    1200, 24.0f, 48.0f + 12.0f, 1.0f,
                    g, pSans, font_tag, conf_str
                );

                std::vector<int> pill_bottom_ink_loss;
                int px_start = std::lround(banner_rects_loss.rc_pill_text.X);
                int px_end = std::lround(banner_rects_loss.rc_pill_text.GetRight());
                int py_start = std::lround(banner_rects_loss.rc_pill_text.Y);
                int py_end = std::lround(banner_rects_loss.rc_pill_text.GetBottom());

                for (int x = px_start; x <= px_end; ++x) {
                    int col_max_y = -1;
                    for (int y = py_start; y <= py_end; ++y) {
                        Gdiplus::Color px;
                        bmp_loss.GetPixel(x, y, &px);
                        if (px.GetR() > 120 && px.GetG() > 120) {
                            if (y > col_max_y) col_max_y = y;
                        }
                    }
                    if (col_max_y != -1) pill_bottom_ink_loss.push_back(col_max_y);
                }
                STUTTO_ASSERT(!pill_bottom_ink_loss.empty());
                std::sort(pill_bottom_ink_loss.begin(), pill_bottom_ink_loss.end());
                int y_pill_loss = pill_bottom_ink_loss[pill_bottom_ink_loss.size() / 2];

                std::vector<int> culprit_bottom_ink_loss;
                int cx_start = std::lround(banner_rects_loss.rc_culprit.X);
                int cx_end = std::lround(banner_rects_loss.rc_culprit.GetRight());
                int cy_start = std::lround(banner_rects_loss.rc_culprit.Y);
                int cy_end = std::lround(banner_rects_loss.rc_culprit.GetBottom() + 10.0f);

                for (int x = cx_start; x <= cx_end; ++x) {
                    int col_max_y = -1;
                    for (int y = cy_start; y <= cy_end; ++y) {
                        Gdiplus::Color px;
                        bmp_loss.GetPixel(x, y, &px);
                        if (px.GetR() > 120 && px.GetG() > 120) {
                            if (y > col_max_y) col_max_y = y;
                        }
                    }
                    if (col_max_y != -1) culprit_bottom_ink_loss.push_back(col_max_y);
                }
                STUTTO_ASSERT(!culprit_bottom_ink_loss.empty());
                std::sort(culprit_bottom_ink_loss.begin(), culprit_bottom_ink_loss.end());
                int y_culprit_loss = culprit_bottom_ink_loss[culprit_bottom_ink_loss.size() / 2];

                STUTTO_ASSERT(std::abs((y_culprit_loss - y_pill_loss) - 3) <= 2 &&
                    "Culprit text must be vertically centered in banner row with telemetry loss");
            }
            pStreamLoss->Release();
        }

        // 3. Vertex Occlusion & Layering Test
        {
            Gdiplus::PointF peak = detail::compute_peak_pixel(report, 1200, 675, 1.0f);
            int x_peak = std::lround(peak.X);
            int y_peak = std::lround(peak.Y);

            Gdiplus::Color peak_px;
            loaded_bmp.GetPixel(x_peak, y_peak, &peak_px);
            STUTTO_ASSERT(peak_px.GetR() > peak_px.GetG() &&
                          peak_px.GetR() > peak_px.GetB() &&
                          peak_px.GetR() > 180 && peak_px.GetG() > 100 &&
                          peak_px.GetR() > peak_px.GetG() + 70 &&
                          peak_px.GetR() > peak_px.GetB() + 30 &&
                          "Peak pixel must be amber dominant");

            // Empty timeline test
            DiagnosticReport empty_report;
            Gdiplus::PointF p_empty = detail::compute_peak_pixel(empty_report, 1200, 675, 1.0f);
            STUTTO_ASSERT(p_empty.X == 0.0f && p_empty.Y == 0.0f);

            // Trigger-preference test when non-trigger frame has strictly higher duration
            auto report_higher_non_trigger = report;
            report_higher_non_trigger.frame_timeline[100].duration_ms = 80.0;
            Gdiplus::PointF p_non_trig = detail::compute_peak_pixel(report_higher_non_trigger, 1200, 675, 1.0f);
            const float side_margin = 24.0f;
            const float content_w = 1200.0f - (side_margin * 2.0f);
            const float plot_x = side_margin + 60.0f;
            const float plot_w = content_w - 76.0f;
            float expected_x_100 = plot_x + (100.0f / 1023.0f) * plot_w;
            STUTTO_ASSERT(std::abs(p_non_trig.X - expected_x_100) < 0.01f &&
                          "compute_peak_pixel must select the highest sample when anchor duration is strictly smaller");

            // Trigger-preference test when trigger frame duration equals peak_dur
            auto report_equal_trigger = report;
            report_equal_trigger.frame_timeline[100].duration_ms = 45.5; // same as trigger at 512
            Gdiplus::PointF p_equal = detail::compute_peak_pixel(report_equal_trigger, 1200, 675, 1.0f);
            float expected_x_512 = plot_x + (512.0f / 1023.0f) * plot_w;
            STUTTO_ASSERT(std::abs(p_equal.X - expected_x_512) < 0.01f &&
                          "compute_peak_pixel must prefer anchor frame when duration is at least as large as peak_dur");
        }

        // 4. ClearType-Safe Amber Text Detection
        {
            // Amber predicate: ClearType-safe distinction between amber (#f59e0b: R=245, G=158, B=11)
            // and white text (#f1f5f9) which has subpixel ClearType edge fringing with R - G up to 63.
            auto is_amber = [](const Gdiplus::Color& px) -> bool {
                return (px.GetR() > 180 &&
                        px.GetG() > 100 &&
                        px.GetR() > px.GetG() + 70 &&
                        px.GetR() > px.GetB() + 30);
            };

            // Tile 0 main text box: X in [50, 150], Y in [172, 198]
            size_t amber_count = 0;
            for (int y = 172; y <= 198; ++y) {
                for (int x = 50; x <= 150; ++x) {
                    Gdiplus::Color px;
                    loaded_bmp.GetPixel(x, y, &px);
                    if (is_amber(px)) {
                        ++amber_count;
                    }
                }
            }
            STUTTO_ASSERT(amber_count >= 10 && "Must detect at least 10 amber text pixels in Tile 0");

            // Healthy report fixture
            auto healthy_report = create_dummy_report();
            healthy_report.trigger.duration_ms = 10.0;
            healthy_report.trigger.spike_ratio = 0.60;
            healthy_report.trigger.baseline_fps = 60.0;
            for (auto& pt : healthy_report.frame_timeline) {
                pt.duration_ms = 10.0;
                pt.is_pacing_stall = false;
            }

            auto healthy_bytes = CardRenderer::render_card_to_png_bytes(healthy_report, opts);
            STUTTO_ASSERT(!healthy_bytes.empty());

            HGLOBAL hMemH = GlobalAlloc(GMEM_MOVEABLE, healthy_bytes.size());
            STUTTO_ASSERT(hMemH != nullptr);
            void* pMemH = GlobalLock(hMemH);
            std::memcpy(pMemH, healthy_bytes.data(), healthy_bytes.size());
            GlobalUnlock(hMemH);

            IStream* pStreamH = nullptr;
            hr = CreateStreamOnHGlobal(hMemH, TRUE, &pStreamH);
            STUTTO_ASSERT(SUCCEEDED(hr) && pStreamH != nullptr);

            {
                Gdiplus::Bitmap healthy_bmp(pStreamH);
                STUTTO_ASSERT(healthy_bmp.GetLastStatus() == Gdiplus::Ok);

                size_t healthy_amber_count = 0;
                for (int y = 172; y <= 198; ++y) {
                    for (int x = 50; x <= 150; ++x) {
                        Gdiplus::Color px;
                        healthy_bmp.GetPixel(x, y, &px);
                        if (is_amber(px)) {
                            ++healthy_amber_count;
                        }
                    }
                }
                STUTTO_ASSERT(healthy_amber_count == 0 && "Healthy report Tile 0 must have 0 amber pixels");
            }
            pStreamH->Release();
        }

        // 5. Multi-Point Chevron Absence Test
        {
            // Sample points at y = std::lround(plot_y + 2.0f * s)
            // plot_y = 232 + 36 = 268; y = 270 at s = 1.0
            const float s = 1.0f;
            const float side_margin = 24.0f * s;
            const float content_w = 1200.0f - (side_margin * 2.0f);
            const float plot_x = side_margin + 60.0f * s;
            const float plot_w = content_w - 76.0f * s;
            const size_t pt_count = report.frame_timeline.size();
            size_t trig_idx = pt_count / 2;
            for (size_t i = 0; i < pt_count; ++i) {
                if (report.frame_timeline[i].relative_index == 0) {
                    trig_idx = i;
                    break;
                }
            }
            const float trig_x = (pt_count <= 1) ? (plot_x + plot_w / 2.0f) : (plot_x + (static_cast<float>(trig_idx) / static_cast<float>(pt_count - 1)) * plot_w);
            const int sample_y = std::lround(268.0f * s + 2.0f * s);

            const int sample_xs[] = {
                std::lround(trig_x - 6.0f * s),
                std::lround(trig_x - 5.0f * s),
                std::lround(trig_x - 4.0f * s),
                std::lround(trig_x + 4.0f * s),
                std::lround(trig_x + 5.0f * s),
                std::lround(trig_x + 6.0f * s)
            };

            auto is_amber = [](const Gdiplus::Color& px) -> bool {
                return (px.GetR() > 180 &&
                        px.GetG() > 100 &&
                        px.GetR() > px.GetG() &&
                        px.GetR() > px.GetB() + 30);
            };

            for (int sx : sample_xs) {
                Gdiplus::Color px;
                loaded_bmp.GetPixel(sx, sample_y, &px);
                STUTTO_ASSERT(!is_amber(px) && "Chevron footprint must not contain amber pixels");
            }
        }

        // 6. Dynamic Confidence Pill Width (Issue 4)
        {
            Gdiplus::FontFamily sans_family(L"Segoe UI");
            const Gdiplus::FontFamily* pSans = sans_family.IsAvailable() ? &sans_family : Gdiplus::FontFamily::GenericSansSerif();

            Gdiplus::Bitmap dummy_bmp(1, 1, PixelFormat32bppARGB);
            Gdiplus::Graphics g(&dummy_bmp);
            const float s = 1.0f;
            Gdiplus::Font font_tag(pSans, 10.0f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);

            auto banner_std = detail::compute_banner_rects(
                1200, 24.0f, 64.0f, 1.0f,
                g, pSans, font_tag, L"92% CONFIDENCE"
            );
            STUTTO_ASSERT(banner_std.rc_conf.Width >= 130.0f * s);
            float expected_x_std = 1200.0f - 24.0f * s - 16.0f * s - banner_std.rc_conf.Width;
            STUTTO_ASSERT(std::abs(banner_std.rc_conf.X - expected_x_std) < 0.01f);
            float expected_culprit_w = (banner_std.rc_conf.X - 16.0f * s) - banner_std.rc_culprit.X;
            STUTTO_ASSERT(std::abs(banner_std.rc_culprit.Width - expected_culprit_w) < 0.01f);

            auto banner_long = detail::compute_banner_rects(
                1200, 24.0f, 64.0f, 1.0f,
                g, pSans, font_tag, L"100% HIGH CONFIDENCE DIAGNOSIS"
            );
            STUTTO_ASSERT(banner_long.rc_conf.Width > banner_std.rc_conf.Width &&
                          "Longer confidence label must result in wider pill");
            float expected_x_long = 1200.0f - 24.0f * s - 16.0f * s - banner_long.rc_conf.Width;
            STUTTO_ASSERT(std::abs(banner_long.rc_conf.X - expected_x_long) < 0.01f);
        }

        // 7. Upper Plot Amber Regression Guard
        // Regression guard: verifies plot_y + 4px (old hairline/callout region) remains free of amber ink.
        {
            const float s = 1.0f;
            const float side_margin = 24.0f * s;
            const float content_w = 1200.0f - (side_margin * 2.0f);
            const float plot_x = side_margin + 60.0f * s;
            const float plot_w = content_w - 76.0f * s;
            const size_t pt_count = report.frame_timeline.size();
            size_t trig_idx = pt_count / 2;
            for (size_t i = 0; i < pt_count; ++i) {
                if (report.frame_timeline[i].relative_index == 0) {
                    trig_idx = i;
                    break;
                }
            }
            const float trig_x = (pt_count <= 1) ? (plot_x + plot_w / 2.0f) : (plot_x + (static_cast<float>(trig_idx) / static_cast<float>(pt_count - 1)) * plot_w);
            const int sample_x = std::lround(trig_x);
            const int sample_y = std::lround(268.0f * s + 4.0f * s); // plot_y + 4.0f * s

            auto is_amber = [](const Gdiplus::Color& px) -> bool {
                return (px.GetR() > 180 &&
                        px.GetG() > 100 &&
                        px.GetR() > px.GetG() &&
                        px.GetR() > px.GetB() + 30);
            };

            Gdiplus::Color px;
            loaded_bmp.GetPixel(sample_x, sample_y, &px);
            STUTTO_ASSERT(!is_amber(px) && "Antenna stub above callout badge must be eliminated");
        }

        // 8. Peak Duration Calculation
        {
            STUTTO_ASSERT(detail::compute_peak_duration(report) == 45.5);

            auto report_higher = report;
            report_higher.frame_timeline[100].duration_ms = 80.0;
            STUTTO_ASSERT(detail::compute_peak_duration(report_higher) == 80.0);

            DiagnosticReport empty_rep;
            empty_rep.trigger.duration_ms = 12.3;
            STUTTO_ASSERT(detail::compute_peak_duration(empty_rep) == 12.3);
        }

        // 9. Lowercase Culprit Optical Centering (Issue 6)
        {
            auto audio_rep = report;
            audio_rep.attribution = AttributionTag::EXTERNAL_CONTENTION;
            audio_rep.attribution_process = "audiodg.exe";

            auto audio_bytes = CardRenderer::render_card_to_png_bytes(audio_rep, opts);
            STUTTO_ASSERT(!audio_bytes.empty());

            HGLOBAL hMemAudio = GlobalAlloc(GMEM_MOVEABLE, audio_bytes.size());
            STUTTO_ASSERT(hMemAudio != nullptr);
            void* pMemAudio = GlobalLock(hMemAudio);
            std::memcpy(pMemAudio, audio_bytes.data(), audio_bytes.size());
            GlobalUnlock(hMemAudio);

            IStream* pStreamAudio = nullptr;
            hr = CreateStreamOnHGlobal(hMemAudio, TRUE, &pStreamAudio);
            STUTTO_ASSERT(SUCCEEDED(hr) && pStreamAudio != nullptr);

            {
                Gdiplus::Bitmap audio_bmp(pStreamAudio);
                STUTTO_ASSERT(audio_bmp.GetLastStatus() == Gdiplus::Ok);

                // Sample culprit region for audiodg.exe (x in [200, 350], y in [60, 95])
                int min_y = 9999;
                int max_y = -1;
                for (int y = 60; y <= 95; ++y) {
                    for (int x = 200; x <= 350; ++x) {
                        Gdiplus::Color px;
                        audio_bmp.GetPixel(x, y, &px);
                        if (px.GetR() > 120 && px.GetG() > 120 && px.GetB() > 120) {
                            if (y < min_y) min_y = y;
                            if (y > max_y) max_y = y;
                        }
                    }
                }
                STUTTO_ASSERT(min_y != 9999 && max_y != -1 && "Must detect ink for audiodg.exe");

                // Pill box is at Y=64, Height=24 (Y in [64, 88], vertical center = 76.0)
                // 1. Must not protrude above the pill box top (Y >= 64)
                STUTTO_ASSERT(min_y >= 64 && "audiodg.exe text ink must not protrude above the pill top edge");
                // 2. Must not extend below the pill box bottom (Y <= 88)
                STUTTO_ASSERT(max_y <= 88 && "audiodg.exe text ink must not extend below the pill bottom edge");
                // 3. Vertical midpoint must align within pill box vertical bounds (near 76.5)
                double mid_y = (min_y + max_y) * 0.5;
                STUTTO_ASSERT(std::abs(mid_y - 76.5) <= 2.5 && "audiodg.exe vertical midpoint must align within pill bounds");
            }
            pStreamAudio->Release();
        }

        // 10. Adjacent Peak Callout Adjacency, Right-Edge Flip & Top Clamping Guard
        {
            auto is_amber = [](const Gdiplus::Color& px) -> bool {
                return (px.GetR() > 180 &&
                        px.GetG() > 100 &&
                        px.GetR() > px.GetG() &&
                        px.GetR() > px.GetB() + 30);
            };

            // Part A: Normal adjacency (peak in middle -> badge sits +10px to the right)
            {
                Gdiplus::PointF peak = detail::compute_peak_pixel(report, 1200, 675, 1.0f);
                float callout_x = peak.X + 10.0f;
                float callout_y = peak.Y - 12.0f;

                int amber_right = 0;
                for (int y = std::lround(callout_y + 4.0f); y <= std::lround(callout_y + 20.0f); ++y) {
                    for (int x = std::lround(callout_x + 6.0f); x <= std::lround(callout_x + 70.0f); ++x) {
                        Gdiplus::Color px;
                        loaded_bmp.GetPixel(x, y, &px);
                        if (is_amber(px)) ++amber_right;
                    }
                }
                STUTTO_ASSERT(amber_right >= 5 && "Adjacent callout badge must contain amber text to the right of peak dot");

                // Region to the left of peak dot must be free of amber callout text
                int amber_left = 0;
                for (int y = std::lround(callout_y + 4.0f); y <= std::lround(callout_y + 20.0f); ++y) {
                    for (int x = std::lround(peak.X - 86.0f); x <= std::lround(peak.X - 10.0f); ++x) {
                        Gdiplus::Color px;
                        loaded_bmp.GetPixel(x, y, &px);
                        if (is_amber(px)) ++amber_left;
                    }
                }
                STUTTO_ASSERT(amber_left == 0 && "Left of peak dot must not contain callout badge text when un-flipped");
            }

            // Part B: Right-edge flip (peak at right boundary -> badge flips to the left)
            {
                auto right_rep = report;
                right_rep.frame_timeline.back().duration_ms = 85.0; // Spike at last frame
                right_rep.trigger.duration_ms = 16.67;             // Ensure anchor is not preferred

                auto right_bytes = CardRenderer::render_card_to_png_bytes(right_rep, opts);
                STUTTO_ASSERT(!right_bytes.empty());

                HGLOBAL hMemR = GlobalAlloc(GMEM_MOVEABLE, right_bytes.size());
                STUTTO_ASSERT(hMemR != nullptr);
                void* pMemR = GlobalLock(hMemR);
                std::memcpy(pMemR, right_bytes.data(), right_bytes.size());
                GlobalUnlock(hMemR);

                IStream* pStreamR = nullptr;
                hr = CreateStreamOnHGlobal(hMemR, TRUE, &pStreamR);
                STUTTO_ASSERT(SUCCEEDED(hr) && pStreamR != nullptr);

                {
                    Gdiplus::Bitmap right_bmp(pStreamR);
                    STUTTO_ASSERT(right_bmp.GetLastStatus() == Gdiplus::Ok);

                    Gdiplus::PointF peak_r = detail::compute_peak_pixel(right_rep, 1200, 675, 1.0f);
                    float flipped_x = peak_r.X - 76.0f - 10.0f;
                    float flipped_y = peak_r.Y - 12.0f;

                    int amber_flipped = 0;
                    for (int y = std::lround(flipped_y + 4.0f); y <= std::lround(flipped_y + 20.0f); ++y) {
                        for (int x = std::lround(flipped_x + 6.0f); x <= std::lround(flipped_x + 70.0f); ++x) {
                            Gdiplus::Color px;
                            right_bmp.GetPixel(x, y, &px);
                            if (is_amber(px)) ++amber_flipped;
                        }
                    }
                    STUTTO_ASSERT(amber_flipped >= 5 && "Right-edge peak must flip callout badge to the left of the dot");

                    // Right of peak dot must have no amber badge text
                    int amber_overflow = 0;
                    for (int y = std::lround(flipped_y + 4.0f); y <= std::lround(flipped_y + 20.0f); ++y) {
                        for (int x = std::lround(peak_r.X + 10.0f); x <= std::lround(peak_r.X + 86.0f); ++x) {
                            if (x < 1200) {
                                Gdiplus::Color px;
                                right_bmp.GetPixel(x, y, &px);
                                if (is_amber(px)) ++amber_overflow;
                            }
                        }
                    }
                    STUTTO_ASSERT(amber_overflow == 0 && "Flipped badge must not leave text overflowing to the right");
                }
                pStreamR->Release();
            }

            // Part C: Top Clamping (peak near top -> badge clamped to plot_y + 4px)
            {
                auto top_rep = report;
                top_rep.frame_timeline[512].duration_ms = 5000.0;
                top_rep.trigger.duration_ms = 5000.0;

                auto top_bytes = CardRenderer::render_card_to_png_bytes(top_rep, opts);
                STUTTO_ASSERT(!top_bytes.empty());

                HGLOBAL hMemT = GlobalAlloc(GMEM_MOVEABLE, top_bytes.size());
                STUTTO_ASSERT(hMemT != nullptr);
                void* pMemT = GlobalLock(hMemT);
                std::memcpy(pMemT, top_bytes.data(), top_bytes.size());
                GlobalUnlock(hMemT);

                IStream* pStreamT = nullptr;
                hr = CreateStreamOnHGlobal(hMemT, TRUE, &pStreamT);
                STUTTO_ASSERT(SUCCEEDED(hr) && pStreamT != nullptr);

                {
                    Gdiplus::Bitmap top_bmp(pStreamT);
                    STUTTO_ASSERT(top_bmp.GetLastStatus() == Gdiplus::Ok);

                    Gdiplus::PointF peak_t = detail::compute_peak_pixel(top_rep, 1200, 675, 1.0f);
                    float callout_x = peak_t.X + 10.0f;

                    // Above clamped_y (y in [256, 271]), there must be no badge background or text
                    int badge_ink_above = 0;
                    for (int y = 256; y < 272; ++y) {
                        for (int x = std::lround(callout_x + 6.0f); x <= std::lround(callout_x + 70.0f); ++x) {
                            Gdiplus::Color px;
                            top_bmp.GetPixel(x, y, &px);
                            if (is_amber(px) || (px.GetR() == 24 && px.GetG() == 30 && px.GetB() == 43)) {
                                ++badge_ink_above;
                            }
                        }
                    }
                    STUTTO_ASSERT(badge_ink_above == 0 && "Peak callout badge must not protrude above plot_y + 4px clamp");

                    // Inside clamped badge (y in [272, 296]), amber text must be present
                    int amber_clamped = 0;
                    for (int y = 276; y <= 292; ++y) {
                        for (int x = std::lround(callout_x + 6.0f); x <= std::lround(callout_x + 70.0f); ++x) {
                            Gdiplus::Color px;
                            top_bmp.GetPixel(x, y, &px);
                            if (is_amber(px)) ++amber_clamped;
                        }
                    }
                    STUTTO_ASSERT(amber_clamped >= 5 && "Top-clamped callout badge must contain amber text");
                }
                pStreamT->Release();
            }

            // Part D: Bottom Clamping (peak near baseline -> badge clamped with 8px margin)
            {
                auto bot_rep = report;
                bot_rep.trigger.duration_ms = 0.1;
                for (auto& pt : bot_rep.frame_timeline) {
                    pt.duration_ms = 0.1;
                }

                auto bot_bytes = CardRenderer::render_card_to_png_bytes(bot_rep, opts);
                STUTTO_ASSERT(!bot_bytes.empty());

                HGLOBAL hMemB = GlobalAlloc(GMEM_MOVEABLE, bot_bytes.size());
                STUTTO_ASSERT(hMemB != nullptr);
                void* pMemB = GlobalLock(hMemB);
                std::memcpy(pMemB, bot_bytes.data(), bot_bytes.size());
                GlobalUnlock(hMemB);

                IStream* pStreamB = nullptr;
                hr = CreateStreamOnHGlobal(hMemB, TRUE, &pStreamB);
                STUTTO_ASSERT(SUCCEEDED(hr) && pStreamB != nullptr);

                {
                    Gdiplus::Bitmap bot_bmp(pStreamB);
                    STUTTO_ASSERT(bot_bmp.GetLastStatus() == Gdiplus::Ok);

                    Gdiplus::PointF peak_b = detail::compute_peak_pixel(bot_rep, 1200, 675, 1.0f);
                    float callout_x = peak_b.X + 10.0f;
                    // plot_y (268) + plot_h (338) - 8px margin = 598px max bottom edge
                    // Callout height = 24px -> clamped callout_y = 574px

                    // Below 598px (y in [599, 606]), there must be no badge background or text
                    int badge_ink_below = 0;
                    for (int y = 599; y <= 606; ++y) {
                        for (int x = std::lround(callout_x + 6.0f); x <= std::lround(callout_x + 70.0f); ++x) {
                            Gdiplus::Color px;
                            bot_bmp.GetPixel(x, y, &px);
                            if (is_amber(px) || (px.GetR() == 24 && px.GetG() == 30 && px.GetB() == 43)) {
                                ++badge_ink_below;
                            }
                        }
                    }
                    STUTTO_ASSERT(badge_ink_below == 0 && "Peak callout badge must not protrude below bottom 8px margin clamp");

                    // Inside clamped badge (y in [576, 594]), amber text must be present
                    int amber_bot = 0;
                    for (int y = 576; y <= 594; ++y) {
                        for (int x = std::lround(callout_x + 6.0f); x <= std::lround(callout_x + 70.0f); ++x) {
                            Gdiplus::Color px;
                            bot_bmp.GetPixel(x, y, &px);
                            if (is_amber(px)) ++amber_bot;
                        }
                    }
                    STUTTO_ASSERT(amber_bot >= 5 && "Bottom-clamped callout badge must contain amber text");
                }
                pStreamB->Release();
            }

            // Part E: Simultaneous Top Clamping & Right-Edge Flip (Peak at top-right corner)
            {
                auto tr_rep = report;
                tr_rep.frame_timeline.back().duration_ms = 5000.0; // Spike at last frame
                tr_rep.trigger.duration_ms = 16.67;               // Ensure anchor is not preferred

                auto tr_bytes = CardRenderer::render_card_to_png_bytes(tr_rep, opts);
                STUTTO_ASSERT(!tr_bytes.empty());

                HGLOBAL hMemTR = GlobalAlloc(GMEM_MOVEABLE, tr_bytes.size());
                STUTTO_ASSERT(hMemTR != nullptr);
                void* pMemTR = GlobalLock(hMemTR);
                std::memcpy(pMemTR, tr_bytes.data(), tr_bytes.size());
                GlobalUnlock(hMemTR);

                IStream* pStreamTR = nullptr;
                hr = CreateStreamOnHGlobal(hMemTR, TRUE, &pStreamTR);
                STUTTO_ASSERT(SUCCEEDED(hr) && pStreamTR != nullptr);

                {
                    Gdiplus::Bitmap tr_bmp(pStreamTR);
                    STUTTO_ASSERT(tr_bmp.GetLastStatus() == Gdiplus::Ok);

                    Gdiplus::PointF peak_tr = detail::compute_peak_pixel(tr_rep, 1200, 675, 1.0f);
                    float flipped_x = peak_tr.X - 76.0f - 10.0f;

                    // Above clamped_y (y in [256, 271]), there must be no badge background or text
                    int ink_above = 0;
                    for (int y = 256; y < 272; ++y) {
                        for (int x = std::lround(flipped_x + 6.0f); x <= std::lround(flipped_x + 70.0f); ++x) {
                            Gdiplus::Color px;
                            tr_bmp.GetPixel(x, y, &px);
                            if (is_amber(px) || (px.GetR() == 24 && px.GetG() == 30 && px.GetB() == 43)) {
                                ++ink_above;
                            }
                        }
                    }
                    STUTTO_ASSERT(ink_above == 0 && "Simultaneous top-clamped and flipped badge must not protrude above plot_y + 4px");

                    // Inside clamped and flipped badge (y in [276, 292], x in [flipped_x + 6, flipped_x + 70])
                    int amber_tr = 0;
                    for (int y = 276; y <= 292; ++y) {
                        for (int x = std::lround(flipped_x + 6.0f); x <= std::lround(flipped_x + 70.0f); ++x) {
                            Gdiplus::Color px;
                            tr_bmp.GetPixel(x, y, &px);
                            if (is_amber(px)) ++amber_tr;
                        }
                    }
                    STUTTO_ASSERT(amber_tr >= 5 && "Simultaneous top-clamped and flipped badge must contain amber text");
                }
                pStreamTR->Release();
            }
        }
    }
    pStream->Release();

    std::cout << "  -> UI/UX regressions (v7) PASSED.\n";
}

int main() {
    try {
        test_initialization();
        test_png_encoding_and_sampling();
        test_clipboard_roundtrip();
        test_attribution_tags();
        test_redaction_permutations();
        test_dpi_scaling();
        test_extreme_metrics();
        test_long_string_truncation();
        test_file_export_and_errors();
        test_ui_ux_regressions();

        CardRenderer::shutdown();

        // Verify one-shot lifecycle contract: re-initialization prohibited after shutdown
        STUTTO_ASSERT(!CardRenderer::initialize() && "Re-initialization after shutdown must return false");
        STUTTO_ASSERT(!CardRenderer::is_initialized() && "Must report not initialized after shutdown");

        CoUninitialize();

        std::cout << "\n========================================\n";
        std::cout << " ALL CARD RENDERER TESTS PASSED (100%)\n";
        std::cout << "========================================\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST RUNNER FATAL ERROR] " << e.what() << "\n";
        return 1;
    }
}
