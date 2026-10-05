#include "test_common.hpp"
#include <stuttometer/gui_config.hpp>
#include <stuttometer/trigger_engine.hpp>
#include <stuttometer/constants.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <cmath>
#include <string>
#include <limits>
#include <vector>

using namespace stuttometer;

// 1. Auto Mode Deserialization: present_threshold_manual: false with min_fps_threshold: 144.0
// loads with manual = false, present_threshold_ms = 16.67.
static void test_auto_mode_deserialization() {
    std::cout << "[TEST] 1. Auto Mode Deserialization...\n";
    nlohmann::json j;
    j["present_threshold_manual"] = false;
    j["min_fps_threshold"] = 144.0;

    GuiConfig cfg;
    uint32_t vk = 0, mods = 0;
    bool sound = false;
    std::string proc;

    deserialize_gui_settings_from_json(j, cfg, vk, mods, sound, proc);

    STUTTO_ASSERT(!cfg.present_threshold_manual);
    STUTTO_ASSERT(std::abs(cfg.present_threshold_ms - DEFAULT_60HZ_VBLANK_MS) < 1e-6);
    std::cout << "  -> PASSED\n";
}

// 2. Manual Mode Deserialization: present_threshold_manual: true with min_fps_threshold: 120.0
// loads with manual = true, present_threshold_ms = 8.333...
static void test_manual_mode_deserialization() {
    std::cout << "[TEST] 2. Manual Mode Deserialization...\n";
    nlohmann::json j;
    j["present_threshold_manual"] = true;
    j["min_fps_threshold"] = 120.0;

    GuiConfig cfg;
    uint32_t vk = 0, mods = 0;
    bool sound = false;
    std::string proc;

    deserialize_gui_settings_from_json(j, cfg, vk, mods, sound, proc);

    STUTTO_ASSERT(cfg.present_threshold_manual);
    STUTTO_ASSERT(std::abs(cfg.present_threshold_ms - (1000.0 / 120.0)) < 1e-6);
    std::cout << "  -> PASSED\n";
}

// 3. Manual Mode Fault-Tolerance: present_threshold_manual: true with missing, null, or out-of-range
// min_fps_threshold (-5.0, 7.0, 1000.0) asserts manual == false and present_threshold_ms == 16.67.
static void test_manual_mode_fault_tolerance() {
    std::cout << "[TEST] 3. Manual Mode Fault-Tolerance...\n";

    // 3a. Missing min_fps_threshold
    {
        nlohmann::json j;
        j["present_threshold_manual"] = true;
        GuiConfig cfg;
        uint32_t vk = 0, mods = 0;
        bool sound = false;
        std::string proc;
        deserialize_gui_settings_from_json(j, cfg, vk, mods, sound, proc);
        STUTTO_ASSERT(!cfg.present_threshold_manual);
        STUTTO_ASSERT(std::abs(cfg.present_threshold_ms - DEFAULT_60HZ_VBLANK_MS) < 1e-6);
    }

    // 3b. Null min_fps_threshold
    {
        nlohmann::json j;
        j["present_threshold_manual"] = true;
        j["min_fps_threshold"] = nullptr;
        GuiConfig cfg;
        uint32_t vk = 0, mods = 0;
        bool sound = false;
        std::string proc;
        deserialize_gui_settings_from_json(j, cfg, vk, mods, sound, proc);
        STUTTO_ASSERT(!cfg.present_threshold_manual);
        STUTTO_ASSERT(std::abs(cfg.present_threshold_ms - DEFAULT_60HZ_VBLANK_MS) < 1e-6);
    }

    // 3c. Negative min_fps_threshold (-5.0)
    {
        nlohmann::json j;
        j["present_threshold_manual"] = true;
        j["min_fps_threshold"] = -5.0;
        GuiConfig cfg;
        uint32_t vk = 0, mods = 0;
        bool sound = false;
        std::string proc;
        deserialize_gui_settings_from_json(j, cfg, vk, mods, sound, proc);
        STUTTO_ASSERT(!cfg.present_threshold_manual);
        STUTTO_ASSERT(std::abs(cfg.present_threshold_ms - DEFAULT_60HZ_VBLANK_MS) < 1e-6);
    }

    // 3d. Below range min_fps_threshold (7.0 < 10.0)
    {
        nlohmann::json j;
        j["present_threshold_manual"] = true;
        j["min_fps_threshold"] = 7.0;
        GuiConfig cfg;
        uint32_t vk = 0, mods = 0;
        bool sound = false;
        std::string proc;
        deserialize_gui_settings_from_json(j, cfg, vk, mods, sound, proc);
        STUTTO_ASSERT(!cfg.present_threshold_manual);
        STUTTO_ASSERT(std::abs(cfg.present_threshold_ms - DEFAULT_60HZ_VBLANK_MS) < 1e-6);
    }

    // 3e. Above range min_fps_threshold (1000.0 > 500.0)
    {
        nlohmann::json j;
        j["present_threshold_manual"] = true;
        j["min_fps_threshold"] = 1000.0;
        GuiConfig cfg;
        uint32_t vk = 0, mods = 0;
        bool sound = false;
        std::string proc;
        deserialize_gui_settings_from_json(j, cfg, vk, mods, sound, proc);
        STUTTO_ASSERT(!cfg.present_threshold_manual);
        STUTTO_ASSERT(std::abs(cfg.present_threshold_ms - DEFAULT_60HZ_VBLANK_MS) < 1e-6);
    }

    // 3f. Lower boundary edge (9.999 < 10.0)
    {
        nlohmann::json j;
        j["present_threshold_manual"] = true;
        j["min_fps_threshold"] = 9.999;
        GuiConfig cfg;
        uint32_t vk = 0, mods = 0;
        bool sound = false;
        std::string proc;
        deserialize_gui_settings_from_json(j, cfg, vk, mods, sound, proc);
        STUTTO_ASSERT(!cfg.present_threshold_manual);
        STUTTO_ASSERT(std::abs(cfg.present_threshold_ms - DEFAULT_60HZ_VBLANK_MS) < 1e-6);
    }

    // 3g. Upper boundary edge (500.001 > 500.0)
    {
        nlohmann::json j;
        j["present_threshold_manual"] = true;
        j["min_fps_threshold"] = 500.001;
        GuiConfig cfg;
        uint32_t vk = 0, mods = 0;
        bool sound = false;
        std::string proc;
        deserialize_gui_settings_from_json(j, cfg, vk, mods, sound, proc);
        STUTTO_ASSERT(!cfg.present_threshold_manual);
        STUTTO_ASSERT(std::abs(cfg.present_threshold_ms - DEFAULT_60HZ_VBLANK_MS) < 1e-6);
    }

    // 3h. Exact valid boundaries: 10.0 and 500.0
    {
        nlohmann::json j10;
        j10["present_threshold_manual"] = true;
        j10["min_fps_threshold"] = 10.0;
        GuiConfig cfg10;
        uint32_t vk = 0, mods = 0;
        bool sound = false;
        std::string proc;
        deserialize_gui_settings_from_json(j10, cfg10, vk, mods, sound, proc);
        STUTTO_ASSERT(cfg10.present_threshold_manual);
        STUTTO_ASSERT(std::abs(cfg10.present_threshold_ms - 100.0) < 1e-6);

        nlohmann::json j500;
        j500["present_threshold_manual"] = true;
        j500["min_fps_threshold"] = 500.0;
        GuiConfig cfg500;
        deserialize_gui_settings_from_json(j500, cfg500, vk, mods, sound, proc);
        STUTTO_ASSERT(cfg500.present_threshold_manual);
        STUTTO_ASSERT(std::abs(cfg500.present_threshold_ms - 2.0) < 1e-6);
    }

    // 3i. Non-numeric types (string, boolean, array, object)
    {
        std::vector<nlohmann::json> bad_types = {
            "144.0",
            true,
            nlohmann::json::array({144.0}),
            nlohmann::json::object({{"fps", 144.0}})
        };
        for (const auto& bad_val : bad_types) {
            nlohmann::json j;
            j["present_threshold_manual"] = true;
            j["min_fps_threshold"] = bad_val;
            GuiConfig cfg;
            uint32_t vk = 0, mods = 0;
            bool sound = false;
            std::string proc;
            deserialize_gui_settings_from_json(j, cfg, vk, mods, sound, proc);
            STUTTO_ASSERT(!cfg.present_threshold_manual);
            STUTTO_ASSERT(std::abs(cfg.present_threshold_ms - DEFAULT_60HZ_VBLANK_MS) < 1e-6);
        }
    }

    // 3j. Direct validation of fps_to_present_threshold_ms robustness & clamping
    {
        STUTTO_ASSERT(std::abs(fps_to_present_threshold_ms(0.0) - DEFAULT_60HZ_VBLANK_MS) < 1e-6);
        STUTTO_ASSERT(std::abs(fps_to_present_threshold_ms(-10.0) - DEFAULT_60HZ_VBLANK_MS) < 1e-6);
        STUTTO_ASSERT(std::abs(fps_to_present_threshold_ms(std::numeric_limits<double>::quiet_NaN()) - DEFAULT_60HZ_VBLANK_MS) < 1e-6);
        STUTTO_ASSERT(std::abs(fps_to_present_threshold_ms(std::numeric_limits<double>::infinity()) - 2.0) < 1e-6);
        STUTTO_ASSERT(std::abs(fps_to_present_threshold_ms(-std::numeric_limits<double>::infinity()) - DEFAULT_60HZ_VBLANK_MS) < 1e-6);
        STUTTO_ASSERT(std::abs(fps_to_present_threshold_ms(1000.0) - 2.0) < 1e-6);
        STUTTO_ASSERT(std::abs(fps_to_present_threshold_ms(1.0) - 200.0) < 1e-6);
        STUTTO_ASSERT(std::abs(fps_to_present_threshold_ms(60.0) - (1000.0 / 60.0)) < 1e-6);
    }

    std::cout << "  -> PASSED\n";
}

// 4. Legacy Migration (Absent Flag): Missing present_threshold_manual key asserts manual == false, present_threshold_ms = 16.67.
static void test_legacy_migration_absent_flag() {
    std::cout << "[TEST] 4. Legacy Migration (Absent Flag)...\n";
    nlohmann::json j;
    j["min_fps_threshold"] = 144.0;
    // Note: present_threshold_manual intentionally omitted

    GuiConfig cfg;
    uint32_t vk = 0, mods = 0;
    bool sound = false;
    std::string proc;

    deserialize_gui_settings_from_json(j, cfg, vk, mods, sound, proc);

    STUTTO_ASSERT(!cfg.present_threshold_manual);
    STUTTO_ASSERT(std::abs(cfg.present_threshold_ms - DEFAULT_60HZ_VBLANK_MS) < 1e-6);
    std::cout << "  -> PASSED\n";
}

// 5. Legacy Stale Cleanup Round-Trip:
//    - Input: {"min_fps_threshold": 144.0} (absent flag).
//    - First deserialize: asserts manual == false, present_threshold_ms = 16.67.
//    - Serialize: asserts output JSON has present_threshold_manual == false and min_fps_threshold == nullptr.
//    - Second deserialize: asserts manual == false, present_threshold_ms = 16.67.
static void test_legacy_stale_cleanup_roundtrip() {
    std::cout << "[TEST] 5. Legacy Stale Cleanup Round-Trip...\n";
    nlohmann::json j_in;
    j_in["min_fps_threshold"] = 144.0;

    GuiConfig cfg1;
    uint32_t vk1 = 0x7A, mods1 = 2;
    bool sound1 = true;
    std::string proc1 = "LegacyApp.exe";

    deserialize_gui_settings_from_json(j_in, cfg1, vk1, mods1, sound1, proc1);
    STUTTO_ASSERT(!cfg1.present_threshold_manual);
    STUTTO_ASSERT(std::abs(cfg1.present_threshold_ms - DEFAULT_60HZ_VBLANK_MS) < 1e-6);

    nlohmann::json j_out = serialize_gui_settings_to_json(cfg1, vk1, mods1, sound1, proc1);
    STUTTO_ASSERT(j_out.contains("present_threshold_manual"));
    STUTTO_ASSERT(j_out["present_threshold_manual"] == false);
    STUTTO_ASSERT(j_out.contains("min_fps_threshold"));
    STUTTO_ASSERT(j_out["min_fps_threshold"].is_null());

    GuiConfig cfg2;
    uint32_t vk2 = 0, mods2 = 0;
    bool sound2 = false;
    std::string proc2;
    deserialize_gui_settings_from_json(j_out, cfg2, vk2, mods2, sound2, proc2);
    STUTTO_ASSERT(!cfg2.present_threshold_manual);
    STUTTO_ASSERT(std::abs(cfg2.present_threshold_ms - DEFAULT_60HZ_VBLANK_MS) < 1e-6);

    std::cout << "  -> PASSED\n";
}

// 6. Auto Mode Serialization: Config with manual = false writes present_threshold_manual: false and min_fps_threshold: null.
static void test_auto_mode_serialization() {
    std::cout << "[TEST] 6. Auto Mode Serialization...\n";
    GuiConfig cfg;
    cfg.present_threshold_manual = false;
    cfg.present_threshold_ms = DEFAULT_60HZ_VBLANK_MS;

    nlohmann::json j = serialize_gui_settings_to_json(cfg, 0x7A, 2, true, "Game.exe");
    STUTTO_ASSERT(j.contains("present_threshold_manual"));
    STUTTO_ASSERT(j["present_threshold_manual"] == false);
    STUTTO_ASSERT(j.contains("min_fps_threshold"));
    STUTTO_ASSERT(j["min_fps_threshold"].is_null());
    std::cout << "  -> PASSED\n";
}

// 7. Manual Mode Serialization: Config with manual = true (120 FPS) writes present_threshold_manual: true and min_fps_threshold: 120.0.
static void test_manual_mode_serialization() {
    std::cout << "[TEST] 7. Manual Mode Serialization...\n";
    GuiConfig cfg;
    cfg.present_threshold_manual = true;
    cfg.present_threshold_ms = fps_to_present_threshold_ms(120.0);

    nlohmann::json j = serialize_gui_settings_to_json(cfg, 0x7A, 2, true, "Game.exe");
    STUTTO_ASSERT(j.contains("present_threshold_manual"));
    STUTTO_ASSERT(j["present_threshold_manual"] == true);
    STUTTO_ASSERT(j.contains("min_fps_threshold"));
    STUTTO_ASSERT(j["min_fps_threshold"].is_number());
    STUTTO_ASSERT(std::abs(j["min_fps_threshold"].get<double>() - 120.0) < 1e-4);

    // Out of range or NaN present_threshold_ms in manual mode must clamp symmetrically
    {
        GuiConfig cfg_nan;
        cfg_nan.present_threshold_manual = true;
        cfg_nan.present_threshold_ms = std::numeric_limits<double>::quiet_NaN();
        nlohmann::json j_nan = serialize_gui_settings_to_json(cfg_nan, 0x7A, 2, true, "Game.exe");
        STUTTO_ASSERT(j_nan["min_fps_threshold"].is_number());
        STUTTO_ASSERT(std::abs(j_nan["min_fps_threshold"].get<double>() - 60.0) < 1e-4);

        GuiConfig cfg_huge;
        cfg_huge.present_threshold_manual = true;
        cfg_huge.present_threshold_ms = 1000.0; // 1 FPS, below 10 FPS
        nlohmann::json j_huge = serialize_gui_settings_to_json(cfg_huge, 0x7A, 2, true, "Game.exe");
        STUTTO_ASSERT(j_huge["min_fps_threshold"].is_number());
        STUTTO_ASSERT(std::abs(j_huge["min_fps_threshold"].get<double>() - 10.0) < 1e-4); // clamped to 10.0

        GuiConfig cfg_tiny;
        cfg_tiny.present_threshold_manual = true;
        cfg_tiny.present_threshold_ms = 0.5; // 2000 FPS, above 500 FPS
        nlohmann::json j_tiny = serialize_gui_settings_to_json(cfg_tiny, 0x7A, 2, true, "Game.exe");
        STUTTO_ASSERT(j_tiny["min_fps_threshold"].is_number());
        STUTTO_ASSERT(std::abs(j_tiny["min_fps_threshold"].get<double>() - 500.0) < 1e-4); // clamped to 500.0
    }

    std::cout << "  -> PASSED\n";
}

// 8. Full Settings Round-Trip (Auto Mode): Complete JSON with present_threshold_manual: false,
// hotkeys, last_target_process: "Game.exe", provider tier, and thresholds serializes and
// deserializes with 100% fidelity.
static void test_full_settings_roundtrip_auto() {
    std::cout << "[TEST] 8. Full Settings Round-Trip (Auto Mode)...\n";
    GuiConfig cfg_in;
    cfg_in.window_pre_ms = 300.0;
    cfg_in.window_post_ms = 50.0;
    cfg_in.present_threshold_ms = DEFAULT_60HZ_VBLANK_MS;
    cfg_in.present_threshold_manual = false;
    cfg_in.cooldown_ms = 2000.0;
    cfg_in.enable_audio = false;
    cfg_in.provider_tier = "full";
    cfg_in.buffer_slots = 524288;
    cfg_in.dpc_threshold_us = 1500;
    cfg_in.isr_threshold_us = 750;
    cfg_in.disk_threshold_ms = 35;
    cfg_in.cswitch_preempt_ms = 10;
    cfg_in.smi_severity_threshold_ms = 25.0;
    cfg_in.smi_threshold_manual = true;
    cfg_in.d3d12_pso_threshold_ms = 8;
    cfg_in.vram_demoted_threshold_mb = 16;
    cfg_in.mem_alloc_threshold_mb = 32;
    cfg_in.mem_trim_threshold_mb = 8;
    cfg_in.mem_physical_latency_us = 2000;
    cfg_in.redact = true;
    cfg_in.output_dir = L"C:\\StuttometerLogs";
    cfg_in.frame_trigger_mode = FrameTriggerMode::DYNAMIC_ONLY;
    cfg_in.pacing_profile = PacingProfile::HIGH_REFRESH;
    cfg_in.spike_multiplier = 1.4;
    cfg_in.min_spike_delta_ms = 1.5;
    cfg_in.enable_judder_detection = false;
    cfg_in.enable_osd = true;
    cfg_in.osd_duration_ms = 5000;
    cfg_in.osd_position = OsdPosition::BOTTOM_LEFT;

    uint32_t vk_in = 0x77; // F8
    uint32_t mods_in = 3;  // Alt+Ctrl
    bool sound_in = false;
    std::string proc_in = "Game.exe";

    nlohmann::json j = serialize_gui_settings_to_json(cfg_in, vk_in, mods_in, sound_in, proc_in);

    STUTTO_ASSERT(j["present_threshold_manual"] == false);
    STUTTO_ASSERT(j["min_fps_threshold"].is_null());

    GuiConfig cfg_out;
    uint32_t vk_out = 0, mods_out = 0;
    bool sound_out = true;
    std::string proc_out;

    deserialize_gui_settings_from_json(j, cfg_out, vk_out, mods_out, sound_out, proc_out);

    STUTTO_ASSERT(vk_out == vk_in);
    STUTTO_ASSERT(mods_out == mods_in);
    STUTTO_ASSERT(sound_out == sound_in);
    STUTTO_ASSERT(proc_out == proc_in);

    STUTTO_ASSERT(!cfg_out.present_threshold_manual);
    STUTTO_ASSERT(std::abs(cfg_out.present_threshold_ms - DEFAULT_60HZ_VBLANK_MS) < 1e-6);
    STUTTO_ASSERT(std::abs(cfg_out.window_pre_ms - cfg_in.window_pre_ms) < 1e-6);
    STUTTO_ASSERT(std::abs(cfg_out.window_post_ms - cfg_in.window_post_ms) < 1e-6);
    STUTTO_ASSERT(std::abs(cfg_out.cooldown_ms - cfg_in.cooldown_ms) < 1e-6);
    STUTTO_ASSERT(cfg_out.enable_audio == cfg_in.enable_audio);
    STUTTO_ASSERT(cfg_out.provider_tier == cfg_in.provider_tier);
    STUTTO_ASSERT(cfg_out.buffer_slots == cfg_in.buffer_slots);
    STUTTO_ASSERT(cfg_out.dpc_threshold_us == cfg_in.dpc_threshold_us);
    STUTTO_ASSERT(cfg_out.isr_threshold_us == cfg_in.isr_threshold_us);
    STUTTO_ASSERT(cfg_out.disk_threshold_ms == cfg_in.disk_threshold_ms);
    STUTTO_ASSERT(cfg_out.cswitch_preempt_ms == cfg_in.cswitch_preempt_ms);
    STUTTO_ASSERT(std::abs(cfg_out.smi_severity_threshold_ms - cfg_in.smi_severity_threshold_ms) < 1e-6);
    STUTTO_ASSERT(cfg_out.smi_threshold_manual == cfg_in.smi_threshold_manual);
    STUTTO_ASSERT(cfg_out.d3d12_pso_threshold_ms == cfg_in.d3d12_pso_threshold_ms);
    STUTTO_ASSERT(cfg_out.vram_demoted_threshold_mb == cfg_in.vram_demoted_threshold_mb);
    STUTTO_ASSERT(cfg_out.mem_alloc_threshold_mb == cfg_in.mem_alloc_threshold_mb);
    STUTTO_ASSERT(cfg_out.mem_trim_threshold_mb == cfg_in.mem_trim_threshold_mb);
    STUTTO_ASSERT(cfg_out.mem_physical_latency_us == cfg_in.mem_physical_latency_us);
    STUTTO_ASSERT(cfg_out.redact == cfg_in.redact);
    STUTTO_ASSERT(cfg_out.output_dir == cfg_in.output_dir);
    STUTTO_ASSERT(cfg_out.frame_trigger_mode == cfg_in.frame_trigger_mode);
    STUTTO_ASSERT(cfg_out.pacing_profile == cfg_in.pacing_profile);
    STUTTO_ASSERT(std::abs(cfg_out.spike_multiplier - cfg_in.spike_multiplier) < 1e-6);
    STUTTO_ASSERT(std::abs(cfg_out.min_spike_delta_ms - cfg_in.min_spike_delta_ms) < 1e-6);
    STUTTO_ASSERT(cfg_out.enable_judder_detection == cfg_in.enable_judder_detection);
    STUTTO_ASSERT(cfg_out.enable_osd == cfg_in.enable_osd);
    STUTTO_ASSERT(cfg_out.osd_duration_ms == cfg_in.osd_duration_ms);
    STUTTO_ASSERT(cfg_out.osd_position == cfg_in.osd_position);

    std::cout << "  -> PASSED\n";
}

// 9. Full Settings Round-Trip (Manual Mode): Complete JSON with present_threshold_manual: true,
// min_fps_threshold: 144.0, hotkeys, last_target_process: "Game.exe", provider tier, and thresholds
// serializes and deserializes with 100% fidelity.
static void test_full_settings_roundtrip_manual() {
    std::cout << "[TEST] 9. Full Settings Round-Trip (Manual Mode)...\n";
    GuiConfig cfg_in;
    cfg_in.present_threshold_manual = true;
    cfg_in.present_threshold_ms = fps_to_present_threshold_ms(144.0);
    cfg_in.window_pre_ms = 400.0;
    cfg_in.window_post_ms = 40.0;
    cfg_in.cooldown_ms = 1500.0;
    cfg_in.enable_audio = true;
    cfg_in.provider_tier = "minimal";
    cfg_in.buffer_slots = 131072;
    cfg_in.dpc_threshold_us = 2000;
    cfg_in.isr_threshold_us = 1000;
    cfg_in.disk_threshold_ms = 50;
    cfg_in.cswitch_preempt_ms = 15;
    cfg_in.smi_severity_threshold_ms = 33.3;
    cfg_in.smi_threshold_manual = false;
    cfg_in.d3d12_pso_threshold_ms = 10;
    cfg_in.vram_demoted_threshold_mb = 32;
    cfg_in.mem_alloc_threshold_mb = 64;
    cfg_in.mem_trim_threshold_mb = 16;
    cfg_in.mem_physical_latency_us = 5000;
    cfg_in.redact = false;
    cfg_in.output_dir = L"D:\\Captures";
    cfg_in.frame_trigger_mode = FrameTriggerMode::STATIC_ONLY;
    cfg_in.pacing_profile = PacingProfile::CONSERVATIVE;
    cfg_in.spike_multiplier = 2.0;
    cfg_in.min_spike_delta_ms = 4.0;
    cfg_in.enable_judder_detection = true;
    cfg_in.enable_osd = false;
    cfg_in.osd_duration_ms = 2500;
    cfg_in.osd_position = OsdPosition::TOP_LEFT;

    uint32_t vk_in = 0x70; // F1
    uint32_t mods_in = 1;  // Alt
    bool sound_in = true;
    std::string proc_in = "Cyberpunk2077.exe";

    nlohmann::json j = serialize_gui_settings_to_json(cfg_in, vk_in, mods_in, sound_in, proc_in);

    STUTTO_ASSERT(j["present_threshold_manual"] == true);
    STUTTO_ASSERT(j["min_fps_threshold"].is_number());
    STUTTO_ASSERT(std::abs(j["min_fps_threshold"].get<double>() - 144.0) < 1e-4);

    GuiConfig cfg_out;
    uint32_t vk_out = 0, mods_out = 0;
    bool sound_out = false;
    std::string proc_out;

    deserialize_gui_settings_from_json(j, cfg_out, vk_out, mods_out, sound_out, proc_out);

    STUTTO_ASSERT(vk_out == vk_in);
    STUTTO_ASSERT(mods_out == mods_in);
    STUTTO_ASSERT(sound_out == sound_in);
    STUTTO_ASSERT(proc_out == proc_in);

    STUTTO_ASSERT(cfg_out.present_threshold_manual);
    STUTTO_ASSERT(std::abs(cfg_out.present_threshold_ms - fps_to_present_threshold_ms(144.0)) < 1e-4);
    STUTTO_ASSERT(std::abs(cfg_out.window_pre_ms - cfg_in.window_pre_ms) < 1e-6);
    STUTTO_ASSERT(std::abs(cfg_out.window_post_ms - cfg_in.window_post_ms) < 1e-6);
    STUTTO_ASSERT(std::abs(cfg_out.cooldown_ms - cfg_in.cooldown_ms) < 1e-6);
    STUTTO_ASSERT(cfg_out.enable_audio == cfg_in.enable_audio);
    STUTTO_ASSERT(cfg_out.provider_tier == cfg_in.provider_tier);
    STUTTO_ASSERT(cfg_out.buffer_slots == cfg_in.buffer_slots);
    STUTTO_ASSERT(cfg_out.dpc_threshold_us == cfg_in.dpc_threshold_us);
    STUTTO_ASSERT(cfg_out.isr_threshold_us == cfg_in.isr_threshold_us);
    STUTTO_ASSERT(cfg_out.disk_threshold_ms == cfg_in.disk_threshold_ms);
    STUTTO_ASSERT(cfg_out.cswitch_preempt_ms == cfg_in.cswitch_preempt_ms);
    STUTTO_ASSERT(std::abs(cfg_out.smi_severity_threshold_ms - cfg_in.smi_severity_threshold_ms) < 1e-6);
    STUTTO_ASSERT(cfg_out.smi_threshold_manual == cfg_in.smi_threshold_manual);
    STUTTO_ASSERT(cfg_out.d3d12_pso_threshold_ms == cfg_in.d3d12_pso_threshold_ms);
    STUTTO_ASSERT(cfg_out.vram_demoted_threshold_mb == cfg_in.vram_demoted_threshold_mb);
    STUTTO_ASSERT(cfg_out.mem_alloc_threshold_mb == cfg_in.mem_alloc_threshold_mb);
    STUTTO_ASSERT(cfg_out.mem_trim_threshold_mb == cfg_in.mem_trim_threshold_mb);
    STUTTO_ASSERT(cfg_out.mem_physical_latency_us == cfg_in.mem_physical_latency_us);
    STUTTO_ASSERT(cfg_out.redact == cfg_in.redact);
    STUTTO_ASSERT(cfg_out.output_dir == cfg_in.output_dir);
    STUTTO_ASSERT(cfg_out.frame_trigger_mode == cfg_in.frame_trigger_mode);
    STUTTO_ASSERT(cfg_out.pacing_profile == cfg_in.pacing_profile);
    STUTTO_ASSERT(std::abs(cfg_out.spike_multiplier - cfg_in.spike_multiplier) < 1e-6);
    STUTTO_ASSERT(std::abs(cfg_out.min_spike_delta_ms - cfg_in.min_spike_delta_ms) < 1e-6);
    STUTTO_ASSERT(cfg_out.enable_judder_detection == cfg_in.enable_judder_detection);
    STUTTO_ASSERT(cfg_out.enable_osd == cfg_in.enable_osd);
    STUTTO_ASSERT(cfg_out.osd_duration_ms == cfg_in.osd_duration_ms);
    STUTTO_ASSERT(cfg_out.osd_position == cfg_in.osd_position);

    std::cout << "  -> PASSED\n";
}

// 10. Detection Presets Round-Trip for all 5 presets
static void test_detection_presets_roundtrip() {
    std::cout << "[TEST] 10. Detection Presets Round-Trip (All 5 presets)...\n";
    const DetectionPreset presets[5] = {
        DetectionPreset::BALANCED,
        DetectionPreset::COMPETITIVE,
        DetectionPreset::CONSERVATIVE,
        DetectionPreset::FORENSIC,
        DetectionPreset::CUSTOM
    };

    for (auto p : presets) {
        GuiConfig in_cfg;
        TriggerConfig dummy_trig{};
        apply_detection_preset(p, dummy_trig, in_cfg);
        if (p == DetectionPreset::CUSTOM) {
            in_cfg.pacing_profile = PacingProfile::CUSTOM;
            in_cfg.spike_multiplier = 3.5;
            in_cfg.min_spike_delta_ms = 8.0;
            in_cfg.judder_swing_ratio = 0.42;
            in_cfg.judder_min_alternations = 7;
        }

        nlohmann::json j = serialize_gui_settings_to_json(in_cfg, 0, 0, false);
        GuiConfig out_cfg;
        uint32_t vk = 0, mods = 0;
        bool snd = false;
        std::string proc;
        deserialize_gui_settings_from_json(j, out_cfg, vk, mods, snd, proc);

        STUTTO_ASSERT(out_cfg.detection_preset == p);
        if (p == DetectionPreset::CUSTOM) {
            STUTTO_ASSERT(std::abs(out_cfg.spike_multiplier - 3.5) < 1e-6);
            STUTTO_ASSERT(std::abs(out_cfg.min_spike_delta_ms - 8.0) < 1e-6);
            STUTTO_ASSERT(std::abs(out_cfg.judder_swing_ratio - 0.42) < 1e-6);
            STUTTO_ASSERT(out_cfg.judder_min_alternations == 7);
        } else if (p == DetectionPreset::CONSERVATIVE) {
            STUTTO_ASSERT(out_cfg.pacing_profile == PacingProfile::CONSERVATIVE);
            STUTTO_ASSERT(std::abs(out_cfg.judder_swing_ratio - 0.60) < 1e-6);
            STUTTO_ASSERT(out_cfg.judder_min_alternations == 8);
        } else if (p == DetectionPreset::BALANCED) {
            STUTTO_ASSERT(out_cfg.pacing_profile == PacingProfile::AUTO_ADAPTIVE);
            STUTTO_ASSERT(std::abs(out_cfg.judder_swing_ratio - 0.50) < 1e-6);
            STUTTO_ASSERT(out_cfg.judder_min_alternations == 5);
        }
    }

    // Explicitly verify CONSERVATIVE preset missed vblank floor settings on TriggerConfig.
    // Hard-coded literals (3) lock the preset contract against silent regression.
    {
        TriggerConfig trig{};
        GuiConfig gui{};
        apply_detection_preset(DetectionPreset::CONSERVATIVE, trig, gui);
        STUTTO_ASSERT(trig.dwm_min_missed_vblanks == 3);
        STUTTO_ASSERT(trig.kernel_frame_stall_min_missed_vblanks == 3);
    }

    std::cout << "  -> PASSED\n";
}

// 11. Schema 1 Migration Predicates
static void test_v1_settings_migration() {
    std::cout << "[TEST] 11. Schema 1 Settings Migration Predicates...\n";

    // 11a. Default v1 settings -> BALANCED
    {
        nlohmann::json j;
        j["settings_version"] = 1;
        j["spike_multiplier"] = 2.0;
        j["min_spike_delta_ms"] = 4.0;
        j["judder_swing_ratio"] = 0.35;
        j["pacing_profile"] = "auto_adaptive";

        GuiConfig out;
        uint32_t vk = 0, mods = 0;
        bool snd = false;
        std::string proc;
        deserialize_gui_settings_from_json(j, out, vk, mods, snd, proc);

        STUTTO_ASSERT(out.detection_preset == DetectionPreset::BALANCED);
        STUTTO_ASSERT(out.min_osd_severity == ReportSeverity::DANGER);
        STUTTO_ASSERT(out.list_severity_filter == ReportSeverity::WARNING);
    }

    // 11b. v1 with high_refresh pacing profile -> CUSTOM
    {
        nlohmann::json j;
        j["settings_version"] = 1;
        j["spike_multiplier"] = 2.0;
        j["min_spike_delta_ms"] = 4.0;
        j["pacing_profile"] = "high_refresh";

        GuiConfig out;
        uint32_t vk = 0, mods = 0;
        bool snd = false;
        std::string proc;
        deserialize_gui_settings_from_json(j, out, vk, mods, snd, proc);

        STUTTO_ASSERT(out.detection_preset == DetectionPreset::CUSTOM);
    }

    // 11c. v1 with non-default spike_multiplier -> CUSTOM
    {
        nlohmann::json j;
        j["settings_version"] = 1;
        j["spike_multiplier"] = 3.0;
        j["min_spike_delta_ms"] = 4.0;

        GuiConfig out;
        uint32_t vk = 0, mods = 0;
        bool snd = false;
        std::string proc;
        deserialize_gui_settings_from_json(j, out, vk, mods, snd, proc);

        STUTTO_ASSERT(out.detection_preset == DetectionPreset::CUSTOM);
    }

    std::cout << "  -> PASSED\n";
}

// 12. Deserialization Demotion (D1: conflicting fields demote preset to CUSTOM; matching fields preserve preset)
static void test_deserialization_demotion() {
    std::cout << "[TEST] 12. Deserialization Demotion (Decision D1)...\n";

    // 12a. Conflicting fields -> demotes to CUSTOM
    {
        nlohmann::json j;
        j["settings_version"] = 2;
        j["detection_preset"] = "competitive";
        // Conflicting raw fields that trigger demotion
        j["pacing_profile"] = "conservative";
        j["judder_swing_ratio"] = 0.85;

        GuiConfig out;
        uint32_t vk = 0, mods = 0;
        bool snd = false;
        std::string proc;
        deserialize_gui_settings_from_json(j, out, vk, mods, snd, proc);

        STUTTO_ASSERT(out.detection_preset == DetectionPreset::CUSTOM);
        STUTTO_ASSERT(out.pacing_profile == PacingProfile::CONSERVATIVE);
        STUTTO_ASSERT(std::abs(out.judder_swing_ratio - 0.85) < 1e-6);
    }

    // 12b. Matching fields -> preserves COMPETITIVE preset
    {
        nlohmann::json j;
        j["settings_version"] = 3;
        j["detection_preset"] = "competitive";
        j["pacing_profile"] = "auto_adaptive";
        j["judder_swing_ratio"] = 0.35;
        j["judder_min_alternations"] = 3;

        GuiConfig out;
        uint32_t vk = 0, mods = 0;
        bool snd = false;
        std::string proc;
        deserialize_gui_settings_from_json(j, out, vk, mods, snd, proc);

        STUTTO_ASSERT(out.detection_preset == DetectionPreset::COMPETITIVE);
        STUTTO_ASSERT(out.pacing_profile == PacingProfile::AUTO_ADAPTIVE);
        STUTTO_ASSERT(std::abs(out.judder_swing_ratio - 0.35) < 1e-6);
        STUTTO_ASSERT(out.judder_min_alternations == 3);
    }

    std::cout << "  -> PASSED\n";
}

// 13. Schema 3 & Advanced Settings Features
static void test_schema_3_settings() {
    std::cout << "[TEST] 13. Schema 3 & Advanced Settings Features...\n";

    // 13a. advanced_unlocked true round-trip
    {
        GuiConfig cfg;
        nlohmann::json j = serialize_gui_settings_to_json(cfg, 0x70, 0, true, true, "Game.exe");
        STUTTO_ASSERT(j["settings_version"] == 3);
        STUTTO_ASSERT(j["advanced_unlocked"] == true);

        GuiConfig out_cfg;
        uint32_t vk = 0, mods = 0;
        bool snd = false, adv = false;
        std::string proc;
        deserialize_gui_settings_from_json(j, out_cfg, vk, mods, snd, adv, proc);
        STUTTO_ASSERT(adv == true);
        STUTTO_ASSERT(proc == "Game.exe");
    }

    // 13b. advanced_unlocked false round-trip
    {
        GuiConfig cfg;
        nlohmann::json j = serialize_gui_settings_to_json(cfg, 0x70, 0, true, false, "Game.exe");
        STUTTO_ASSERT(j["advanced_unlocked"] == false);

        GuiConfig out_cfg;
        uint32_t vk = 0, mods = 0;
        bool snd = false, adv = true;
        std::string proc;
        deserialize_gui_settings_from_json(j, out_cfg, vk, mods, snd, adv, proc);
        STUTTO_ASSERT(adv == false);
    }

    // 13c. Absent advanced_unlocked in older schema files defaults to false
    {
        nlohmann::json j;
        j["settings_version"] = 2;
        j["detection_preset"] = "balanced";

        GuiConfig out_cfg;
        uint32_t vk = 0, mods = 0;
        bool snd = false, adv = true;
        std::string proc;
        deserialize_gui_settings_from_json(j, out_cfg, vk, mods, snd, adv, proc);
        STUTTO_ASSERT(adv == false);
    }

    // 13d. judder_min_alternations round-trips within range (1-50)
    {
        GuiConfig cfg;
        cfg.detection_preset = DetectionPreset::CUSTOM;
        cfg.judder_min_alternations = 12;

        nlohmann::json j = serialize_gui_settings_to_json(cfg, 0, 0, false, false);
        STUTTO_ASSERT(j["judder_min_alternations"] == 12);

        GuiConfig out_cfg;
        uint32_t vk = 0, mods = 0;
        bool snd = false, adv = false;
        std::string proc;
        deserialize_gui_settings_from_json(j, out_cfg, vk, mods, snd, adv, proc);
        STUTTO_ASSERT(out_cfg.judder_min_alternations == 12);
    }

    // 13e. Out-of-range judder_min_alternations (0, 51) retains struct default 5
    {
        nlohmann::json j0;
        j0["settings_version"] = 3;
        j0["detection_preset"] = "custom";
        j0["judder_min_alternations"] = 0;

        GuiConfig out0;
        uint32_t vk = 0, mods = 0;
        bool snd = false;
        std::string proc;
        deserialize_gui_settings_from_json(j0, out0, vk, mods, snd, proc);
        STUTTO_ASSERT(out0.judder_min_alternations == 5);

        nlohmann::json j51;
        j51["settings_version"] = 3;
        j51["detection_preset"] = "custom";
        j51["judder_min_alternations"] = 51;

        GuiConfig out51;
        deserialize_gui_settings_from_json(j51, out51, vk, mods, snd, proc);
        STUTTO_ASSERT(out51.judder_min_alternations == 5);
    }

    // 13f. Demotion triggered solely by judder_min_alternations mismatch
    {
        nlohmann::json j;
        j["settings_version"] = 3;
        j["detection_preset"] = "balanced";
        j["pacing_profile"] = "auto_adaptive";
        j["judder_swing_ratio"] = 0.50;
        j["judder_min_alternations"] = 3; // Mismatch: BALANCED expects 5

        GuiConfig out;
        uint32_t vk = 0, mods = 0;
        bool snd = false;
        std::string proc;
        deserialize_gui_settings_from_json(j, out, vk, mods, snd, proc);

        STUTTO_ASSERT(out.detection_preset == DetectionPreset::CUSTOM);
        STUTTO_ASSERT(out.judder_min_alternations == 3);
    }

    std::cout << "  -> PASSED\n";
}

int main() {
    std::cout << "========================================\n";
    std::cout << "Running test_gui_settings (13 scenarios)\n";
    std::cout << "========================================\n";

    try {
        test_auto_mode_deserialization();
        test_manual_mode_deserialization();
        test_manual_mode_fault_tolerance();
        test_legacy_migration_absent_flag();
        test_legacy_stale_cleanup_roundtrip();
        test_auto_mode_serialization();
        test_manual_mode_serialization();
        test_full_settings_roundtrip_auto();
        test_full_settings_roundtrip_manual();
        test_detection_presets_roundtrip();
        test_v1_settings_migration();
        test_deserialization_demotion();
        test_schema_3_settings();
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << "\n";
        return 1;
    }

    std::cout << "All 13 GUI settings test scenarios PASSED!\n";
    return 0;
}
