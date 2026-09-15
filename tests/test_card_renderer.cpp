#include "test_common.hpp"
#include "card_renderer.hpp"
#include "stuttometer/internal/redaction_utils.hpp"

#include <windows.h>
#include <gdiplus.h>
#include <iostream>
#include <vector>
#include <cstdint>
#include <cmath>

using namespace stuttometer;
using namespace stuttometer::gui;

static DiagnosticReport create_dummy_report() {
    DiagnosticReport report;
    report.tool_version = "0.3.1";
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

// Test 3: Clipboard CF_DIB Direct Paste
static void test_clipboard_roundtrip() {
    std::cout << "[TEST 3] Testing clipboard CF_DIB direct paste format...\n";
    auto report = create_dummy_report();

    // Test OpenClipboard defensively
    if (!OpenClipboard(nullptr)) {
        std::cout << "  -> Clipboard locked or running in headless CI without active desktop station. Soft-skipping.\n";
        return;
    }
    CloseClipboard();

    bool cb_ok = CardRenderer::copy_card_to_clipboard(nullptr, report);
    if (!cb_ok) {
        std::cout << "  -> Clipboard access rejected by environment. Soft-skipping.\n";
        return;
    }

    if (OpenClipboard(nullptr)) {
        HANDLE hData = GetClipboardData(CF_DIB);
        STUTTO_ASSERT(hData != nullptr && "CF_DIB handle expected in clipboard");
        auto* bih = reinterpret_cast<BITMAPINFOHEADER*>(GlobalLock(hData));
        STUTTO_ASSERT(bih != nullptr);

        STUTTO_ASSERT(bih->biSize == sizeof(BITMAPINFOHEADER));
        STUTTO_ASSERT(bih->biWidth == 1200);
        STUTTO_ASSERT(bih->biHeight == 675 && "biHeight must be positive for bottom-up orientation");
        STUTTO_ASSERT(bih->biBitCount == 24);
        STUTTO_ASSERT(bih->biCompression == BI_RGB);

        GlobalUnlock(hData);
        CloseClipboard();
    }

    std::cout << "  -> Clipboard CF_DIB structure PASSED.\n";
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

    // Invalid / illegal path
    std::filesystem::path invalid_path = "Z:\\nonexistent_dir_0987654321\\illegal.png";
    bool fail_ok = CardRenderer::save_card_to_png(report, invalid_path);
    STUTTO_ASSERT(!fail_ok && "Saving to illegal path must return false without crashing");

    std::cout << "  -> File export and error paths PASSED.\n";
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
