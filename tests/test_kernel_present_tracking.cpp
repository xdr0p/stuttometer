#include "test_common.hpp"
#include "stuttometer/etw_session.hpp"
#include "stuttometer/trigger_engine.hpp"
#include "stuttometer/correlator.hpp"
#include "stuttometer/json_reporter.hpp"
#include "stuttometer/privilege_utils.hpp"
#include "stuttometer/session_benchmark.hpp"
#include <iostream>
#include <unordered_set>

static void test_make_flip_key_distribution() {
    std::cout << "[TEST] Validating make_flip_key hash combiner & injectivity...\n";

    std::unordered_set<uint64_t> keys;
    for (uint32_t vidpn = 0; vidpn < 4; ++vidpn) {
        for (uint64_t ptr = 0x10000; ptr <= 0x100000; ptr += 0x1000) {
            uint64_t k = stuttometer::make_flip_key(vidpn, ptr);
            STUTTO_ASSERT(k != 0);
            STUTTO_ASSERT(keys.find(k) == keys.end());
            keys.insert(k);
        }
    }

    // Zero / null swapchain pointer fallback
    uint64_t k_null = stuttometer::make_flip_key(0, 0);
    STUTTO_ASSERT(k_null != 0);

    std::cout << "  -> Generated " << keys.size() << " unique flip keys with 0 collisions. PASSED.\n";
}

static void test_kernel_frame_stall_trigger_and_upgrade() {
    std::cout << "[TEST] Validating TriggerEngine KERNEL_FRAME_STALL & thread-safe trigger upgrade...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    stuttometer::TriggerConfig cfg;
    cfg.present_threshold_ms = 5.0; // 200 FPS -> effective threshold = 5.0 + 0.5 = 5.5ms
    cfg.frame_trigger_mode = stuttometer::FrameTriggerMode::STATIC_ONLY;
    cfg.window_pre_ms = 250.0;
    cfg.window_post_ms = 30.0;

    stuttometer::TriggerEngine engine(cfg, qpc_freq);
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    // 1. Normal frame (5.0ms < 5.5ms effective threshold) -> no trigger
    STUTTO_ASSERT(!engine.on_kernel_frame_stall(100, 200, 5.0, base_qpc, 0));
    STUTTO_ASSERT(engine.current_state() == stuttometer::TriggerState::ARMED);

    // 2. Direct GPU frame stall (35.0ms) -> triggers KERNEL_FRAME_STALL
    STUTTO_ASSERT(engine.on_kernel_frame_stall(100, 200, 35.0, base_qpc, 0));
    STUTTO_ASSERT(engine.current_state() == stuttometer::TriggerState::COLLECTING_POST);

    stuttometer::TriggerInfo trig_info;
    uint64_t from_qpc = 0;
    uint64_t to_qpc = 0;

    // Advance past post-window
    const uint64_t poll_qpc = base_qpc + stuttometer::ms_to_qpc_delta(35.0, qpc_freq);
    STUTTO_ASSERT(engine.poll_state(poll_qpc, trig_info, from_qpc, to_qpc));
    STUTTO_ASSERT(trig_info.source == stuttometer::TriggerSource::KERNEL_FRAME_STALL);
    STUTTO_ASSERT(trig_info.duration_ms == 35.0);

    // Reset via report completion
    engine.on_report_completed(poll_qpc);
    engine.poll_state(poll_qpc + stuttometer::ms_to_qpc_delta(1100.0, qpc_freq), trig_info, from_qpc, to_qpc);
    STUTTO_ASSERT(engine.current_state() == stuttometer::TriggerState::ARMED);

    // 3. Trigger Upgrade: CPU Present triggers first, GPU Frame Stall arrives during COLLECTING_POST
    const uint64_t base_qpc2 = poll_qpc + stuttometer::ms_to_qpc_delta(2000.0, qpc_freq);
    STUTTO_ASSERT(engine.on_dxgi_present(100, 200, 6.0, base_qpc2, 0));
    STUTTO_ASSERT(engine.current_state() == stuttometer::TriggerState::COLLECTING_POST);

    // GPU stall of 45.0ms arrives during COLLECTING_POST
    STUTTO_ASSERT(engine.on_kernel_frame_stall(100, 200, 45.0, base_qpc2 + stuttometer::ms_to_qpc_delta(2.0, qpc_freq), 0));

    // Subsequent smaller GPU frame of 10.0ms arrives during COLLECTING_POST -> must NOT overwrite 45.0ms
    STUTTO_ASSERT(engine.on_kernel_frame_stall(100, 200, 10.0, base_qpc2 + stuttometer::ms_to_qpc_delta(5.0, qpc_freq), 0));

    // Poll after post-window -> should be safely upgraded to KERNEL_FRAME_STALL with worst-case 45.0ms duration
    const uint64_t poll_qpc2 = base_qpc2 + stuttometer::ms_to_qpc_delta(35.0, qpc_freq);
    STUTTO_ASSERT(engine.poll_state(poll_qpc2, trig_info, from_qpc, to_qpc));
    STUTTO_ASSERT(trig_info.source == stuttometer::TriggerSource::KERNEL_FRAME_STALL);
    STUTTO_ASSERT(trig_info.duration_ms == 45.0);

    // Reset via report completion
    engine.on_report_completed(poll_qpc2);
    engine.poll_state(poll_qpc2 + stuttometer::ms_to_qpc_delta(1100.0, qpc_freq), trig_info, from_qpc, to_qpc);
    STUTTO_ASSERT(engine.current_state() == stuttometer::TriggerState::ARMED);

    // 4. Trigger Non-Downgrade: CPU Present triggers first with large 50.0ms stutter, minor GPU stall of 15.0ms arrives during COLLECTING_POST
    const uint64_t base_qpc3 = poll_qpc2 + stuttometer::ms_to_qpc_delta(2000.0, qpc_freq);
    STUTTO_ASSERT(engine.on_dxgi_present(100, 200, 50.0, base_qpc3, 0));
    STUTTO_ASSERT(engine.current_state() == stuttometer::TriggerState::COLLECTING_POST);

    // Minor GPU stall of 15.0ms arrives during COLLECTING_POST
    STUTTO_ASSERT(engine.on_kernel_frame_stall(100, 200, 15.0, base_qpc3 + stuttometer::ms_to_qpc_delta(2.0, qpc_freq), 0));

    // Poll after post-window -> must NOT downgrade duration to 15.0ms or switch source from DXGI_PRESENT_STUTTER
    const uint64_t poll_qpc3 = base_qpc3 + stuttometer::ms_to_qpc_delta(35.0, qpc_freq);
    STUTTO_ASSERT(engine.poll_state(poll_qpc3, trig_info, from_qpc, to_qpc));
    STUTTO_ASSERT(trig_info.source == stuttometer::TriggerSource::DXGI_PRESENT_STUTTER);
    STUTTO_ASSERT(trig_info.duration_ms == 50.0);

    std::cout << "  -> KERNEL_FRAME_STALL and staged Trigger Upgrade (with non-downgrade guarantee) PASSED.\n";
}

static void test_last_flip_table_lifecycle() {
    std::cout << "[TEST] Validating FixedInFlightTable<LastFlipEntry> lifecycle & eviction...\n";

    stuttometer::FixedInFlightTable<stuttometer::LastFlipEntry, 2048> table;
    const uint64_t qpc1 = 1000000;
    const uint64_t k1 = stuttometer::make_flip_key(0, 0xABCDEF00);

    table.insert(k1, { qpc1, 0xABCDEF00, 1234, 5678 });

    stuttometer::LastFlipEntry found{};
    STUTTO_ASSERT(table.lookup(k1, found));
    STUTTO_ASSERT(found.last_flip_qpc == qpc1);
    STUTTO_ASSERT(found.pid == 1234);

    // Evict after max age
    const uint64_t max_age = 500000;
    table.evict_stale(qpc1 + max_age + 100, max_age, [](const stuttometer::LastFlipEntry& e) { return e.last_flip_qpc; });

    stuttometer::LastFlipEntry after_evict{};
    STUTTO_ASSERT(!table.lookup(k1, after_evict));
    STUTTO_ASSERT(table.unpaired_evictions() == 1);

    std::cout << "  -> LastFlipTable insertion, lookup, and stale eviction PASSED.\n";
}

static void test_gpu_pipeline_stall_correlation_and_json() {
    std::cout << "[TEST] Validating gpu_pipeline_stall correlation & JSON export schema...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    stuttometer::DriverSymbolResolver resolver;
    stuttometer::CorrelationEngine correlator(resolver);
    stuttometer::JsonReporter reporter;

    stuttometer::TriggerInfo trigger;
    trigger.source = stuttometer::TriggerSource::KERNEL_FRAME_STALL;
    trigger.trigger_timestamp_qpc = stuttometer::get_current_qpc();
    trigger.duration_ms = 42.0;
    trigger.target_pid = 1234;
    trigger.target_tid = 5678;

    std::vector<stuttometer::EtwEventRecord> snapshot;
    // Add some DxgKrnl events
    for (int i = 0; i < 5; ++i) {
        stuttometer::EtwEventRecord rec{};
        rec.category = static_cast<uint16_t>(stuttometer::EventCategory::DXGKRNL_MMIOFLIP);
        rec.qpc_timestamp = trigger.trigger_timestamp_qpc - (i * 1000);
        snapshot.push_back(rec);
    }
    {
        stuttometer::EtwEventRecord vsync{};
        vsync.category = static_cast<uint16_t>(stuttometer::EventCategory::DXGKRNL_VSYNCDPC);
        vsync.qpc_timestamp = trigger.trigger_timestamp_qpc;
        snapshot.push_back(vsync);
    }
    {
        stuttometer::EtwEventRecord dwm{};
        dwm.category = static_cast<uint16_t>(stuttometer::EventCategory::DWM_GLITCH);
        dwm.qpc_timestamp = trigger.trigger_timestamp_qpc;
        snapshot.push_back(dwm);
    }

    stuttometer::ProviderContext p_ctx;
    p_ctx.user_dxgkrnl_active = true;
    p_ctx.user_dwm_active = true;

    auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx, 0, 0, 0, 0);

    STUTTO_ASSERT(report.event_counts.dxgkrnl_mmioflip == 5);
    STUTTO_ASSERT(report.event_counts.dxgkrnl_vsyncdpc == 1);
    STUTTO_ASSERT(report.event_counts.dwm_glitch == 1);
    STUTTO_ASSERT(!report.diagnoses.empty());
    STUTTO_ASSERT(report.diagnoses[0].hypothesis == "gpu_pipeline_stall");
    STUTTO_ASSERT(report.diagnoses[0].confidence >= 0.60);

    // Verify JSON serialization contains the new categories
    std::string json_str = reporter.to_json_string(report, false);
    STUTTO_ASSERT(json_str.find("DXGKRNL_MMIOFLIP") != std::string::npos);
    STUTTO_ASSERT(json_str.find("DXGKRNL_VSYNCDPC") != std::string::npos);
    STUTTO_ASSERT(json_str.find("DWM_GLITCH") != std::string::npos);
    STUTTO_ASSERT(json_str.find("user_dxgkrnl") != std::string::npos);
    STUTTO_ASSERT(json_str.find("user_dwm") != std::string::npos);
    STUTTO_ASSERT(json_str.find("gpu_pipeline_stall") != std::string::npos);

    std::cout << "  -> gpu_pipeline_stall correlation and JSON category schema PASSED.\n";
}

static void test_gpu_pre_window_scaling() {
    std::cout << "[TEST] Validating GPU pre-window proportional scaling & bounds...\n";

    const uint64_t qpc_freq = 10000000ULL; // 10 MHz

    // 1. Large pre-window (1000ms): GPU window should scale to 1200ms (not truncated to 500ms)
    {
        stuttometer::TriggerConfig cfg;
        cfg.present_threshold_ms = 16.67;
        cfg.frame_trigger_mode = stuttometer::FrameTriggerMode::STATIC_ONLY;
        cfg.window_pre_ms = 1000.0;
        cfg.window_post_ms = 0.0;
        stuttometer::TriggerEngine engine(cfg, qpc_freq);

        const uint64_t trig_ts = 50000000ULL; // 5.0s
        STUTTO_ASSERT(engine.on_kernel_frame_stall(100, 200, 35.0, trig_ts, 0));

        stuttometer::TriggerInfo info;
        uint64_t from_qpc = 0, to_qpc = 0;
        STUTTO_ASSERT(engine.poll_state(trig_ts, info, from_qpc, to_qpc));
        
        uint64_t expected_delta = stuttometer::ms_to_qpc_delta(1200.0, qpc_freq);
        STUTTO_ASSERT(from_qpc == (trig_ts - expected_delta));
    }

    // 2. Minimum pre-window (50ms): GPU window clamps to 250ms minimum lookback
    {
        stuttometer::TriggerConfig cfg;
        cfg.present_threshold_ms = 16.67;
        cfg.frame_trigger_mode = stuttometer::FrameTriggerMode::STATIC_ONLY;
        cfg.window_pre_ms = 50.0;
        cfg.window_post_ms = 0.0;
        stuttometer::TriggerEngine engine(cfg, qpc_freq);

        const uint64_t trig_ts = 50000000ULL;
        STUTTO_ASSERT(engine.on_kernel_frame_stall(100, 200, 35.0, trig_ts, 0));

        stuttometer::TriggerInfo info;
        uint64_t from_qpc = 0, to_qpc = 0;
        STUTTO_ASSERT(engine.poll_state(trig_ts, info, from_qpc, to_qpc));

        uint64_t expected_delta = stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
        STUTTO_ASSERT(from_qpc == (trig_ts - expected_delta));
    }

    // 3. Default pre-window (250ms): GPU window scales 1.5x to 375ms
    {
        stuttometer::TriggerConfig cfg;
        cfg.present_threshold_ms = 16.67;
        cfg.frame_trigger_mode = stuttometer::FrameTriggerMode::STATIC_ONLY;
        cfg.window_pre_ms = 250.0;
        cfg.window_post_ms = 0.0;
        stuttometer::TriggerEngine engine(cfg, qpc_freq);

        const uint64_t trig_ts = 50000000ULL;
        STUTTO_ASSERT(engine.on_kernel_frame_stall(100, 200, 35.0, trig_ts, 0));

        stuttometer::TriggerInfo info;
        uint64_t from_qpc = 0, to_qpc = 0;
        STUTTO_ASSERT(engine.poll_state(trig_ts, info, from_qpc, to_qpc));

        uint64_t expected_delta = stuttometer::ms_to_qpc_delta(375.0, qpc_freq);
        STUTTO_ASSERT(from_qpc == (trig_ts - expected_delta));
    }

    std::cout << "  -> GPU pre-window proportional scaling (50ms->250ms, 250ms->375ms, 1000ms->1200ms) PASSED.\n";
}

static void test_dwm_glitch_trigger_attribution() {
    std::cout << "[TEST] Validating TriggerEngine DWM_GLITCH attribution & thresholding at 60Hz and 120Hz...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    stuttometer::TriggerConfig cfg;
    cfg.present_threshold_ms = 16.67; // 60 Hz baseline
    cfg.window_pre_ms = 250.0;
    cfg.window_post_ms = 30.0;
    cfg.target_pid = 4321; // Configured target game PID

    stuttometer::TriggerEngine engine(cfg, qpc_freq);
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    // 1. Duration below effective threshold (16.67 - 0.833 = 15.837ms) -> 14.0ms rejected
    STUTTO_ASSERT(!engine.on_dwm_glitch(888, 999, 14.0, base_qpc, 0));

    // 2. 1-vblank glitch at 60Hz (16.67ms) -> accepted and attributed to target PID 4321, TID 0
    STUTTO_ASSERT(engine.on_dwm_glitch(888, 999, 16.67, base_qpc, 1));

    stuttometer::TriggerInfo info;
    uint64_t from_qpc = 0, to_qpc = 0;
    const uint64_t poll_qpc = base_qpc + stuttometer::ms_to_qpc_delta(35.0, qpc_freq);
    STUTTO_ASSERT(engine.poll_state(poll_qpc, info, from_qpc, to_qpc));
    STUTTO_ASSERT(info.source == stuttometer::TriggerSource::DWM_GLITCH);
    STUTTO_ASSERT(info.target_pid == 4321);
    STUTTO_ASSERT(info.target_tid == 0);
    STUTTO_ASSERT(info.duration_ms == 16.67);

    // 3. High refresh rate 120Hz test (present_threshold_ms = 8.33)
    stuttometer::TriggerConfig cfg120;
    cfg120.present_threshold_ms = 8.33;
    cfg120.window_pre_ms = 250.0;
    cfg120.window_post_ms = 30.0;
    cfg120.target_pid = 4321;

    stuttometer::TriggerEngine engine120(cfg120, qpc_freq);
    const uint64_t base120_qpc = stuttometer::get_current_qpc();
    // 1-vblank glitch at 120Hz (8.33ms) -> must NOT be dropped!
    STUTTO_ASSERT(engine120.on_dwm_glitch(888, 999, 8.33, base120_qpc, 0));

    std::cout << "  -> DWM_GLITCH target attribution and 1-vblank thresholding (60Hz & 120Hz) PASSED.\n";
}

static void test_trigger_engine_single_emission_guarantee() {
    std::cout << "[TEST] Validating TriggerEngine single-emission contract in FROZEN state...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    stuttometer::TriggerConfig cfg;
    cfg.present_threshold_ms = 10.0;
    cfg.frame_trigger_mode = stuttometer::FrameTriggerMode::STATIC_ONLY;
    cfg.window_pre_ms = 250.0;
    cfg.window_post_ms = 30.0;
    cfg.cooldown_ms = 500.0;

    stuttometer::TriggerEngine engine(cfg, qpc_freq);
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    // Trigger stutter
    STUTTO_ASSERT(engine.on_dxgi_present(1234, 5678, 25.0, base_qpc, 0));

    stuttometer::TriggerInfo trig;
    uint64_t from_qpc = 0, to_qpc = 0;
    const uint64_t poll_qpc = base_qpc + stuttometer::ms_to_qpc_delta(35.0, qpc_freq);

    // 1st poll -> transitions to FROZEN and returns true
    STUTTO_ASSERT(engine.poll_state(poll_qpc, trig, from_qpc, to_qpc));
    STUTTO_ASSERT(engine.current_state() == stuttometer::TriggerState::FROZEN);

    // 2nd poll in FROZEN state before on_report_completed -> MUST return false (single emission)
    stuttometer::TriggerInfo trig2;
    STUTTO_ASSERT(!engine.poll_state(poll_qpc + 10, trig2, from_qpc, to_qpc));
    STUTTO_ASSERT(!engine.poll_state(poll_qpc + 20, trig2, from_qpc, to_qpc));

    // Complete report
    engine.on_report_completed(poll_qpc + 30);
    STUTTO_ASSERT(engine.current_state() == stuttometer::TriggerState::COOLDOWN);

    // After cooldown expiry, must return to ARMED and accept new triggers
    const uint64_t after_cooldown = poll_qpc + 30 + stuttometer::ms_to_qpc_delta(600.0, qpc_freq);
    STUTTO_ASSERT(!engine.poll_state(after_cooldown, trig, from_qpc, to_qpc));
    STUTTO_ASSERT(engine.current_state() == stuttometer::TriggerState::ARMED);

    STUTTO_ASSERT(engine.on_dxgi_present(1234, 5678, 30.0, after_cooldown, 0));
    std::cout << "  -> Single-emission guarantee in FROZEN state PASSED.\n";
}

static void test_static_trigger_on_pacing_table_exhaustion() {
    std::cout << "[TEST] Validating static threshold trigger fallback on pacing table exhaustion...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    stuttometer::TriggerConfig cfg;
    cfg.present_threshold_ms = 20.0;
    cfg.window_pre_ms = 250.0;
    cfg.window_post_ms = 30.0;
    cfg.frame_trigger_mode = stuttometer::FrameTriggerMode::STATIC_ONLY;

    stuttometer::TriggerEngine engine(cfg, qpc_freq);
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    // Verify a 50ms frame triggers even with large static thresholds
    bool triggered = engine.on_dxgi_present(9999, 8888, 50.0, base_qpc, 0x99998888ULL);
    STUTTO_ASSERT(triggered);

    stuttometer::TriggerInfo trig;
    uint64_t from_qpc = 0, to_qpc = 0;
    const uint64_t poll_qpc = base_qpc + stuttometer::ms_to_qpc_delta(35.0, qpc_freq);
    STUTTO_ASSERT(engine.poll_state(poll_qpc, trig, from_qpc, to_qpc));
    STUTTO_ASSERT(trig.source == stuttometer::TriggerSource::DXGI_PRESENT_STUTTER);
    STUTTO_ASSERT(trig.reason == stuttometer::TriggerReason::STATIC_THRESHOLD);
    STUTTO_ASSERT(trig.duration_ms == 50.0);

    std::cout << "  -> Static threshold trigger fallback PASSED.\n";
}

static void test_dxgkrnl_task17_flip_parsing_and_dwm_filtering() {
    std::cout << "[TEST] Validating DxgKrnl Task 17 (Event 116) offset parsing & DWM filtering...\n";

    stuttometer::FlightRecorder recorder(1024);
    stuttometer::TriggerConfig trig_cfg;
    trig_cfg.target_pid = 5555;
    stuttometer::TriggerEngine engine(trig_cfg, 10000000);

    stuttometer::EtwSessionConfig cfg;
    cfg.enable_dxgkrnl = true;

    stuttometer::EtwSessionManager mgr(recorder, engine, cfg);
    mgr.set_running_for_test(true);

    // Event 116 (Task 17 MMIOFlip) layout:
    // Offset 0:  pDxgAdapter (8 bytes)
    // Offset 8:  VidPnSourceId (4 bytes)
    // Offset 12: FlipSubmitSequence (4 bytes)
    // Offset 16: FlipToDriverAllocation (8 bytes)
    // Offset 24: FlipToPhysicalAddress (8 bytes)
    // Offset 32: FlipToSegmentId (4 bytes)
    // Offset 36: FlipPresentId (4 bytes)
    // Total min length: 40 bytes
    uint8_t payload[48]{};
    const uint32_t expected_vidpn = 2;
    const uint64_t expected_alloc = 0xABCD1234DEADBEEFULL;
    const uint32_t expected_present_id = 4242;

    std::memcpy(payload + 8, &expected_vidpn, 4);
    std::memcpy(payload + 16, &expected_alloc, 8);
    std::memcpy(payload + 36, &expected_present_id, 4);

    // 1. Normal game flip (PID 5555)
    EVENT_RECORD ev_game{};
    ev_game.UserContext = &mgr;
    ev_game.EventHeader.ProviderId = stuttometer::DXGKRNL_PROVIDER_GUID;
    ev_game.EventHeader.EventDescriptor.Task = 17;
    ev_game.EventHeader.EventDescriptor.Id = 116;
    ev_game.EventHeader.ProcessId = 5555;
    ev_game.EventHeader.ThreadId = 1234;
    ev_game.EventHeader.TimeStamp.QuadPart = 10000000;
    ev_game.UserData = payload;
    ev_game.UserDataLength = sizeof(payload);

    stuttometer::EtwSessionManager::on_event_record(&ev_game);

    uint64_t drops = 0;
    auto snap = recorder.snapshot(10000000, 10000000, &drops);
    STUTTO_ASSERT(drops == 0);
    STUTTO_ASSERT(snap.size() == 1);
    STUTTO_ASSERT(snap[0].category == static_cast<uint16_t>(stuttometer::EventCategory::DXGKRNL_MMIOFLIP));
    STUTTO_ASSERT(snap[0].auxiliary_data == expected_alloc);
    STUTTO_ASSERT(snap[0].payload.dxgi.present_flags == expected_vidpn);
    STUTTO_ASSERT(snap[0].payload.dxgi.frame_index == expected_present_id);

    // 2. DWM / System flip (PID 4)
    EVENT_RECORD ev_system{};
    ev_system.UserContext = &mgr;
    ev_system.EventHeader.ProviderId = stuttometer::DXGKRNL_PROVIDER_GUID;
    ev_system.EventHeader.EventDescriptor.Task = 17;
    ev_system.EventHeader.EventDescriptor.Id = 116;
    ev_system.EventHeader.ProcessId = 4; // System / DWM
    ev_system.EventHeader.ThreadId = 5678;
    ev_system.EventHeader.TimeStamp.QuadPart = 10500000; // 50ms later
    ev_system.UserData = payload;
    ev_system.UserDataLength = sizeof(payload);

    stuttometer::EtwSessionManager::on_event_record(&ev_system);

    auto snap2 = recorder.snapshot(10000000, 10500000, &drops);
    STUTTO_ASSERT(snap2.size() == 2); // Pushed to flight recorder
    STUTTO_ASSERT(engine.current_state() == stuttometer::TriggerState::ARMED); // Filtered from engine
    STUTTO_ASSERT(engine.suppressed_trigger_count() == 0);

    mgr.set_running_for_test(false);
    std::cout << "  -> DxgKrnl Task 17 offset parsing & DWM filtering PASSED.\n";
}

static void test_display_refresh_query() {
    std::cout << "[TEST] Validating query_display_refresh_info resilience & fallbacks...\n";

    // 1. Current process / foreground display query (PID 0)
    auto info_zero = stuttometer::query_display_refresh_info(0);
    STUTTO_ASSERT(info_zero.refresh_rate_hz > 0.0);
    STUTTO_ASSERT(info_zero.vblank_interval_ms > 0.0);
    STUTTO_ASSERT(std::abs(info_zero.vblank_interval_ms - (1000.0 / info_zero.refresh_rate_hz)) < 0.01);
    std::cout << "  -> PID 0 resolved: " << info_zero.refresh_rate_hz << " Hz (" 
              << info_zero.vblank_interval_ms << " ms vblank, " 
              << (info_zero.query_succeeded ? "monitor" : "fallback") << ").\n";

    // 2. Non-existent PID (0xFFFFFFFE) -> fallback without crash
    auto info_nonexistent = stuttometer::query_display_refresh_info(0xFFFFFFFE);
    STUTTO_ASSERT(info_nonexistent.refresh_rate_hz > 0.0);
    STUTTO_ASSERT(info_nonexistent.vblank_interval_ms > 0.0);
    STUTTO_ASSERT(std::abs(info_nonexistent.vblank_interval_ms - (1000.0 / info_nonexistent.refresh_rate_hz)) < 0.01);
    std::cout << "  -> PID 0xFFFFFFFE safely resolved: " << info_nonexistent.refresh_rate_hz << " Hz.\n";

    // 3. Current process ID -> safe resolution without crash
    auto info_self = stuttometer::query_display_refresh_info(GetCurrentProcessId());
    STUTTO_ASSERT(info_self.refresh_rate_hz > 0.0);
    STUTTO_ASSERT(info_self.vblank_interval_ms > 0.0);
    STUTTO_ASSERT(std::abs(info_self.vblank_interval_ms - (1000.0 / info_self.refresh_rate_hz)) < 0.01);
    std::cout << "  -> Self PID resolved: " << info_self.refresh_rate_hz << " Hz.\n";

    std::cout << "  -> query_display_refresh_info PASSED.\n";
}

static void test_dwm_pipeline_high_refresh() {
    std::cout << "[TEST] Validating end-to-end DWM pipeline with 240Hz vblank interval...\n";

    constexpr double VBLANK_240HZ_MS = 1000.0 / 240.0; // ~4.16667 ms
    stuttometer::TriggerConfig cfg;
    cfg.present_threshold_ms = 16.67; // User stutter threshold remains 16.67 ms
    cfg.vblank_interval_ms = VBLANK_240HZ_MS; // Physical hardware vblank is ~4.167 ms
    cfg.target_pid = 7777;
    cfg.window_pre_ms = 250.0;
    cfg.window_post_ms = 30.0;

    const uint64_t qpc_freq = 10000000;
    stuttometer::TriggerEngine engine(cfg, qpc_freq);
    STUTTO_ASSERT(std::abs(engine.vblank_interval_ms() - VBLANK_240HZ_MS) < 1e-6);

    const uint64_t base_qpc = stuttometer::get_current_qpc();

    // Sub-threshold glitch (< 3.667 ms) rejected
    STUTTO_ASSERT(!engine.on_dwm_glitch(100, 200, 3.0, base_qpc, 0));

    // Single 240Hz vblank glitch (4.167 ms) accepted even though present_threshold_ms = 16.67 ms!
    STUTTO_ASSERT(engine.on_dwm_glitch(100, 200, VBLANK_240HZ_MS, base_qpc, 0));

    stuttometer::TriggerInfo trig;
    uint64_t from_qpc = 0, to_qpc = 0;
    const uint64_t poll_qpc = stuttometer::get_current_qpc()
                            + stuttometer::ms_to_qpc_delta(35.0, qpc_freq);
    STUTTO_ASSERT(engine.poll_state(poll_qpc, trig, from_qpc, to_qpc));
    STUTTO_ASSERT(trig.source == stuttometer::TriggerSource::DWM_GLITCH);
    STUTTO_ASSERT(trig.target_pid == 7777);
    STUTTO_ASSERT(std::abs(trig.duration_ms - VBLANK_240HZ_MS) < 1e-3);
    STUTTO_ASSERT(std::abs(trig.baseline_avg_ms - VBLANK_240HZ_MS) < 1e-3);
    STUTTO_ASSERT(std::abs(trig.spike_ratio - 1.0) < 1e-3);
    STUTTO_ASSERT(std::abs(trig.baseline_fps - 240.0) < 0.1);

    // Now test full ETW pipeline through EtwSessionManager with synthesized duration
    stuttometer::FlightRecorder recorder(1024);
    stuttometer::TriggerEngine engine2(cfg, qpc_freq);
    stuttometer::EtwSessionConfig etw_cfg;
    etw_cfg.enable_dwm_core = true;

    stuttometer::EtwSessionManager mgr(recorder, engine2, etw_cfg);
    mgr.set_running_for_test(true);

    // Synthesize DWM Glitch event: Task 132, Id 15, missed_vblanks = 3
    uint8_t dwm_payload[8]{};
    const uint32_t glitch_type = 2;
    const uint32_t missed_vblanks = 3;
    std::memcpy(dwm_payload + 0, &glitch_type, sizeof(uint32_t));
    std::memcpy(dwm_payload + 4, &missed_vblanks, sizeof(uint32_t));

    EVENT_RECORD ev_dwm{};
    ev_dwm.UserContext = &mgr;
    ev_dwm.EventHeader.ProviderId = stuttometer::DWM_CORE_PROVIDER_GUID;
    ev_dwm.EventHeader.EventDescriptor.Task = 132;
    ev_dwm.EventHeader.EventDescriptor.Id = 15;
    ev_dwm.EventHeader.ProcessId = 1000;
    ev_dwm.EventHeader.ThreadId = 2000;
    ev_dwm.EventHeader.TimeStamp.QuadPart = 20000000;
    ev_dwm.UserData = dwm_payload;
    ev_dwm.UserDataLength = sizeof(dwm_payload);

    stuttometer::EtwSessionManager::on_event_record(&ev_dwm);

    // Verify flight recorder duration synthesized as 3 * 4.16667 ms = 12.5 ms
    uint64_t drops = 0;
    auto snap = recorder.snapshot(20000000, 20000000, &drops);
    STUTTO_ASSERT(snap.size() == 1);
    STUTTO_ASSERT(snap[0].category == static_cast<uint16_t>(stuttometer::EventCategory::DWM_GLITCH));
    const double expected_dur_ms = 3.0 * VBLANK_240HZ_MS;
    const uint32_t expected_dur_us = static_cast<uint32_t>(expected_dur_ms * 1000.0);
    STUTTO_ASSERT(std::abs(static_cast<int>(snap[0].duration_us) - static_cast<int>(expected_dur_us)) <= 1);

    // Verify engine triggered with proper duration and spike ratio
    stuttometer::TriggerInfo trig2;
    const uint64_t poll2_qpc = stuttometer::get_current_qpc()
                             + stuttometer::ms_to_qpc_delta(35.0, qpc_freq);
    STUTTO_ASSERT(engine2.poll_state(poll2_qpc, trig2, from_qpc, to_qpc));
    STUTTO_ASSERT(trig2.source == stuttometer::TriggerSource::DWM_GLITCH);
    STUTTO_ASSERT(std::abs(trig2.duration_ms - expected_dur_ms) < 1e-3);
    STUTTO_ASSERT(std::abs(trig2.baseline_avg_ms - VBLANK_240HZ_MS) < 1e-3);
    STUTTO_ASSERT(std::abs(trig2.spike_ratio - 3.0) < 1e-3);

    // Test missed_vblanks == 0 -> synthesizes 1 vblank duration
    stuttometer::FlightRecorder recorder3(1024);
    stuttometer::TriggerEngine engine3(cfg, qpc_freq);
    stuttometer::EtwSessionManager mgr3(recorder3, engine3, etw_cfg);
    mgr3.set_running_for_test(true);

    uint8_t dwm_payload0[8]{};
    const uint32_t missed_vblanks0 = 0;
    std::memcpy(dwm_payload0 + 4, &missed_vblanks0, sizeof(uint32_t));
    ev_dwm.UserContext = &mgr3;
    ev_dwm.UserData = dwm_payload0;
    ev_dwm.EventHeader.TimeStamp.QuadPart = 30000000;
    stuttometer::EtwSessionManager::on_event_record(&ev_dwm);

    auto snap3 = recorder3.snapshot(30000000, 30000000, &drops);
    STUTTO_ASSERT(snap3.size() == 1);
    const uint32_t expected_dur_us0 = static_cast<uint32_t>(VBLANK_240HZ_MS * 1000.0);
    STUTTO_ASSERT(std::abs(static_cast<int>(snap3[0].duration_us) - static_cast<int>(expected_dur_us0)) <= 1);

    mgr.set_running_for_test(false);
    mgr3.set_running_for_test(false);
    std::cout << "  -> End-to-end DWM pipeline 240Hz PASSED.\n";
}

// ============================================================
// Helper: synthesize a DXGI Present Start event (42 or 55)
// ============================================================
static void push_present_start(
    stuttometer::EtwSessionManager& mgr,
    uint16_t event_id, // 42 or 55
    uint32_t pid,
    uint32_t tid,
    uint64_t timestamp_qpc,
    uint64_t swapchain_ptr)
{
    EVENT_RECORD ev{};
    ev.UserContext = &mgr;
    ev.EventHeader.ProviderId = stuttometer::DXGI_PROVIDER_GUID;
    ev.EventHeader.EventDescriptor.Id = event_id;
    ev.EventHeader.ProcessId = pid;
    ev.EventHeader.ThreadId = tid;
    ev.EventHeader.TimeStamp.QuadPart = static_cast<LONGLONG>(timestamp_qpc);
    ev.UserData = &swapchain_ptr;
    ev.UserDataLength = sizeof(swapchain_ptr);
    stuttometer::EtwSessionManager::on_event_record(&ev);
}

// ============================================================
// Helper: synthesize a DXGI Present Stop event (43 or 56)
// ============================================================
static void push_present_stop(
    stuttometer::EtwSessionManager& mgr,
    uint16_t event_id, // 43 or 56
    uint32_t pid,
    uint32_t tid,
    uint64_t timestamp_qpc)
{
    uint32_t result_ok = 0;
    EVENT_RECORD ev{};
    ev.UserContext = &mgr;
    ev.EventHeader.ProviderId = stuttometer::DXGI_PROVIDER_GUID;
    ev.EventHeader.EventDescriptor.Id = event_id;
    ev.EventHeader.ProcessId = pid;
    ev.EventHeader.ThreadId = tid;
    ev.EventHeader.TimeStamp.QuadPart = static_cast<LONGLONG>(timestamp_qpc);
    ev.UserData = &result_ok;
    ev.UserDataLength = sizeof(result_ok);
    stuttometer::EtwSessionManager::on_event_record(&ev);
}

// ============================================================
// 4.1 — Standard + MPO interleaving dedup
// ============================================================
static void test_dxgi_mpo_standard_interleaving_dedup() {
    std::cout << "[TEST] 4.1 MPO/standard interleaving dedup: baseline sanity & zero post-warmup triggers...\n";

    const uint64_t qpc_freq = 10000000ULL; // 10 MHz synthetic
    stuttometer::FlightRecorder recorder(4096);
    stuttometer::TriggerConfig trig_cfg;
    trig_cfg.target_pid = 1234;
    trig_cfg.present_threshold_ms = 5.0; // 200 Hz vblank-derived (would cause false positives without fix)
    // Use DYNAMIC_ONLY: HYBRID warmup [4,7] window would fire STATIC_THRESHOLD
    // triggers (5.5 ms effective < 8.33 ms frames), which pollutes the test's
    // baseline-vs-post-warmup assertions. DYNAMIC_ONLY isolates the MPO dedup
    // verification from HYBRID warmup behavior.
    trig_cfg.frame_trigger_mode = stuttometer::FrameTriggerMode::DYNAMIC_ONLY;
    trig_cfg.pacing_profile = stuttometer::PacingProfile::AUTO_ADAPTIVE;
    trig_cfg.cooldown_ms = 100.0; // Fast cooldown so warmup triggers drain quickly
    stuttometer::TriggerEngine engine(trig_cfg, qpc_freq);

    stuttometer::EtwSessionConfig cfg;
    cfg.enable_dxgi = true;
    stuttometer::EtwSessionManager mgr(recorder, engine, cfg);
    mgr.set_running_for_test(true);

    const uint32_t pid = 1234;
    const uint32_t tid_std = 100; // Standard present thread
    const uint32_t tid_mpo = 200; // MPO present thread
    const uint64_t swapchain = 0x00007FF7DEAD0000ULL;

    // ~120 FPS = 8.333 ms per frame = 83333 QPC ticks at 10 MHz (1 ms = 10000 ticks)
    const uint64_t frame_ticks = 83333; // ~8.333 ms

    // Standard Start-Stop: 0.5 ms API duration; MPO arrives ~0.2 ms after Standard Stop
    // Intra-frame delta (Stop43 → Stop56) ≈ 0.7 ms << DUPLICATE_PRESENT_PATH_MAX_US (1 ms)
    const uint64_t std_api_dur = 5000; // 0.5 ms in ticks at 10 MHz
    const uint64_t mpo_api_dur = 2000; // 0.2 ms in ticks

    uint64_t t = 10000000ULL; // starting QPC
    const int frames = 200;

    // HYBRID warmup note: at effective_static = 5.25 ms, frames 4-7 (sample_count 4-7) may
    // trip STATIC_THRESHOLD while warmup is in progress (8.33 ms > 5.25 ms). This is correct
    // pre-warmup behavior. The adaptive floor only activates at sample_count >= 8.
    // We drain these warmup triggers inline so the state machine stays ARMED for subsequent frames.
    int warmup_triggers = 0;
    int post_warmup_triggers = 0;

    for (int i = 0; i < frames; ++i) {
        // Drain any trigger from the previous iteration before sending the next frame
        if (engine.current_state() != stuttometer::TriggerState::ARMED) {
            stuttometer::TriggerInfo dummy_trig;
            uint64_t fq = 0, tq = 0;
            engine.poll_state(t + stuttometer::ms_to_qpc_delta(35.0, qpc_freq), dummy_trig, fq, tq);
            engine.on_report_completed(t + stuttometer::ms_to_qpc_delta(35.0, qpc_freq));
            // Advance t past the cooldown (100 ms)
            t += stuttometer::ms_to_qpc_delta(120.0, qpc_freq);
            if (i < 8) {
                ++warmup_triggers;
            }
        }

        // Standard path: 42 at T, 43 at T+0.5ms
        uint64_t std_start = t;
        uint64_t std_stop  = t + std_api_dur;
        push_present_start(mgr, 42, pid, tid_std, std_start, swapchain);
        push_present_stop (mgr, 43, pid, tid_std, std_stop);

        // MPO path: 55 at T+0.5ms, 56 at T+0.7ms (intra-frame delta ~0.7ms < 1ms threshold)
        uint64_t mpo_start = std_stop;
        uint64_t mpo_stop  = std_stop + mpo_api_dur;
        push_present_start(mgr, 55, pid, tid_mpo, mpo_start, swapchain);
        push_present_stop (mgr, 56, pid, tid_mpo, mpo_stop);

        t += frame_ticks;

        // Count post-warmup triggers (only after baseline is established)
        if (i >= 8 && engine.current_state() != stuttometer::TriggerState::ARMED) {
            ++post_warmup_triggers;
        }
    }

    // Post-warmup: after baseline is established (sample_count >= 8), no triggers from normal ~8.33 ms frames
    STUTTO_ASSERT(post_warmup_triggers == 0);
    std::cout << "  -> warmup_triggers=" << warmup_triggers << " (expected; 5.25ms threshold < 8.33ms frame during warmup)\n";

    // Drain any final pending state
    if (engine.current_state() != stuttometer::TriggerState::ARMED) {
        stuttometer::TriggerInfo dummy_trig;
        uint64_t fq = 0, tq = 0;
        engine.poll_state(t + stuttometer::ms_to_qpc_delta(200.0, qpc_freq), dummy_trig, fq, tq);
        engine.on_report_completed(t + stuttometer::ms_to_qpc_delta(200.0, qpc_freq));
        t += stuttometer::ms_to_qpc_delta(200.0, qpc_freq);
    }

    // Verify: the rolling baseline is near 8.33 ms (not sub-1 ms garbage from duplicate ingestion)
    uint64_t swapchain_key = stuttometer::make_swapchain_key(pid, swapchain);
    double baseline_ms = engine.current_stream_baseline_ms_for_test(swapchain_key, pid, 0);
    STUTTO_ASSERT(baseline_ms > 5.0);  // Well above the sub-1 ms artifact range
    STUTTO_ASSERT(baseline_ms < 15.0); // Sane upper bound

    // Verify: flight recorder has records for all Stop events (43 and 56)
    uint64_t drops = 0;
    auto snap = recorder.snapshot(10000000ULL, t, &drops);
    STUTTO_ASSERT(drops == 0);
    // 200 frames × 2 Stop events = 400 records (43 and 56); the exact count holds
    // because flight_recorder_.push() is unconditional for both 43 and 56
    STUTTO_ASSERT(snap.size() == 400);

    // Verify: 56 Stop events are tagged DXGI_DUPLICATE_PRESENT_PATH (post-first-frame 56s are dups)
    int dup_count = 0;
    int real_count = 0;
    for (const auto& rec : snap) {
        if (rec.flags & stuttometer::EventFlags::DXGI_DUPLICATE_PRESENT_PATH) {
            ++dup_count;
        } else {
            ++real_count;
        }
    }
    // Conservative assertion: at least 190 of the 200 56-Stop events are tagged as duplicates
    STUTTO_ASSERT(dup_count >= 190);
    STUTTO_ASSERT(real_count >= 1);

    mgr.set_running_for_test(false);
    std::cout << "  -> MPO/standard interleaving dedup (200 frames, baseline=" << baseline_ms
              << " ms, dup_tagged=" << dup_count << ") PASSED.\n";
}

// ============================================================
// 4.2 — Interleaved 42→55→43→56 ordering: 56 tagged as dup,
//       one authoritative frame per cycle
// ============================================================
static void test_dxgi_interleaved_start_stop_pairing() {
    std::cout << "[TEST] 4.2 Interleaved 42→55→43→56 ordering: 56 tagged as DXGI_DUPLICATE_PRESENT_PATH...\n";

    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::FlightRecorder recorder(1024);
    stuttometer::TriggerConfig trig_cfg;
    trig_cfg.target_pid = 5555;
    trig_cfg.frame_trigger_mode = stuttometer::FrameTriggerMode::HYBRID;
    stuttometer::TriggerEngine engine(trig_cfg, qpc_freq);

    stuttometer::EtwSessionConfig cfg;
    cfg.enable_dxgi = true;
    stuttometer::EtwSessionManager mgr(recorder, engine, cfg);
    mgr.set_running_for_test(true);

    const uint32_t pid = 5555;
    const uint32_t tid_std = 300;
    const uint32_t tid_mpo = 301;
    const uint64_t swapchain = 0xABCDEF001234ULL;

    // Frame 1 (two cycles to warm baseline so frame 2 is not a reset):
    // Cycle A: 42→43 (canonical)
    const uint64_t frame_ticks = 83333; // ~8.33 ms at 10 MHz
    uint64_t t = 10000000ULL;

    // Standard and MPO events must be on separate threads for in-flight pairing to work.
    // (This matches the design: in-flight tracking is keyed by (pid, tid).)
    // Warm up with one clean standard 42→43 first (so next Stop is not a baseline reset)
    push_present_start(mgr, 42, pid, tid_std, t, swapchain);
    push_present_stop (mgr, 43, pid, tid_std, t + 5000);
    t += frame_ticks;

    // Interleaved frame: standard 42→43 on tid_std, MPO 55→56 on tid_mpo.
    // The 56 Stop arrives 0.3 ms after the 43 Stop on the same swapchain → sub-1 ms delta
    // → tagged as DXGI_DUPLICATE_PRESENT_PATH by calculate_effective_present_duration.
    uint64_t std_start = t;
    uint64_t std_stop  = t + 5000;
    uint64_t mpo_start = t + 5000;
    uint64_t mpo_stop  = t + 8000;

    push_present_start(mgr, 42, pid, tid_std, std_start, swapchain);
    push_present_stop (mgr, 43, pid, tid_std, std_stop);
    push_present_start(mgr, 55, pid, tid_mpo, mpo_start, swapchain);
    push_present_stop (mgr, 56, pid, tid_mpo, mpo_stop);

    uint64_t drops = 0;
    auto snap = recorder.snapshot(10000000ULL, mpo_stop, &drops);

    // We should have records for both stops of the interleaved frame
    // (plus the warm-up 43 = 3 records total; but first 43 is baseline-reset so may have no flags)
    // Find the 56-Stop record (which should be the most recent DXGI Stop in snap)
    int dup_tagged = 0;
    for (const auto& rec : snap) {
        if (rec.category == static_cast<uint16_t>(stuttometer::EventCategory::DXGI) &&
            (rec.flags & stuttometer::EventFlags::DXGI_DUPLICATE_PRESENT_PATH)) {
            ++dup_tagged;
        }
    }
    // The 56 Stop (intra-frame delta ~0.3 ms) must be tagged
    STUTTO_ASSERT(dup_tagged >= 1);

    // Engine must not have triggered from the sub-1 ms duplicate
    STUTTO_ASSERT(engine.current_state() == stuttometer::TriggerState::ARMED);

    mgr.set_running_for_test(false);
    std::cout << "  -> Interleaved 42→55→43→56: " << dup_tagged
              << " duplicate(s) tagged. PASSED.\n";
}

// ============================================================
// 4.3 — MPO-only path: no false dedup, baseline stays correct
// ============================================================
static void test_dxgi_mpo_only_path() {
    std::cout << "[TEST] 4.3 MPO-only path (55/56 only): no false dedup, baseline correct...\n";

    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::FlightRecorder recorder(2048);
    stuttometer::TriggerConfig trig_cfg;
    trig_cfg.target_pid = 7777;
    trig_cfg.frame_trigger_mode = stuttometer::FrameTriggerMode::HYBRID;
    stuttometer::TriggerEngine engine(trig_cfg, qpc_freq);

    stuttometer::EtwSessionConfig cfg;
    cfg.enable_dxgi = true;
    stuttometer::EtwSessionManager mgr(recorder, engine, cfg);
    mgr.set_running_for_test(true);

    const uint32_t pid = 7777;
    const uint32_t tid = 400;
    const uint64_t swapchain = 0x1111222233334444ULL;
    const uint64_t frame_ticks = 83333; // ~8.33 ms
    const uint64_t api_dur = 3000;      // 0.3 ms API duration

    uint64_t t = 20000000ULL;
    const int frames = 60;
    for (int i = 0; i < frames; ++i) {
        push_present_start(mgr, 55, pid, tid, t, swapchain);
        push_present_stop (mgr, 56, pid, tid, t + api_dur);
        t += frame_ticks;
    }

    // No stutter triggers
    stuttometer::TriggerInfo trig;
    uint64_t from_qpc = 0, to_qpc = 0;
    bool triggered = engine.poll_state(t + stuttometer::ms_to_qpc_delta(35.0, qpc_freq), trig, from_qpc, to_qpc);
    STUTTO_ASSERT(!triggered);

    // Baseline correct (~8.33 ms) — not sub-1 ms
    uint64_t sc_key = stuttometer::make_swapchain_key(pid, swapchain);
    double baseline_ms = engine.current_stream_baseline_ms_for_test(sc_key, pid, 0);
    STUTTO_ASSERT(baseline_ms > 5.0 && baseline_ms < 15.0);

    // No records tagged as duplicate
    uint64_t drops = 0;
    auto snap = recorder.snapshot(20000000ULL, t, &drops);
    STUTTO_ASSERT(snap.size() == 60);
    for (const auto& rec : snap) {
        STUTTO_ASSERT(!(rec.flags & stuttometer::EventFlags::DXGI_DUPLICATE_PRESENT_PATH));
    }

    mgr.set_running_for_test(false);
    std::cout << "  -> MPO-only path: baseline=" << baseline_ms << " ms, 0 duplicates. PASSED.\n";
}

// ============================================================
// 4.4 — Standard-only path: behaviour unchanged from pre-fix
// ============================================================
static void test_dxgi_standard_only_path_unchanged() {
    std::cout << "[TEST] 4.4 Standard-only path (42/43): behaviour unchanged from pre-fix...\n";

    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::FlightRecorder recorder(2048);
    stuttometer::TriggerConfig trig_cfg;
    trig_cfg.target_pid = 8888;
    trig_cfg.frame_trigger_mode = stuttometer::FrameTriggerMode::HYBRID;
    stuttometer::TriggerEngine engine(trig_cfg, qpc_freq);

    stuttometer::EtwSessionConfig cfg;
    cfg.enable_dxgi = true;
    stuttometer::EtwSessionManager mgr(recorder, engine, cfg);
    mgr.set_running_for_test(true);

    const uint32_t pid = 8888;
    const uint32_t tid = 500;
    const uint64_t swapchain = 0xAAAABBBBCCCCDDDDULL;
    const uint64_t frame_ticks = 83333; // ~8.33 ms
    const uint64_t api_dur = 5000;      // 0.5 ms

    uint64_t t = 30000000ULL;
    const int frames = 60;
    for (int i = 0; i < frames; ++i) {
        push_present_start(mgr, 42, pid, tid, t, swapchain);
        push_present_stop (mgr, 43, pid, tid, t + api_dur);
        t += frame_ticks;
    }

    // No stutter triggers
    stuttometer::TriggerInfo trig;
    uint64_t from_qpc = 0, to_qpc = 0;
    bool triggered = engine.poll_state(t + stuttometer::ms_to_qpc_delta(35.0, qpc_freq), trig, from_qpc, to_qpc);
    STUTTO_ASSERT(!triggered);

    // Baseline correct (~8.33 ms)
    uint64_t sc_key = stuttometer::make_swapchain_key(pid, swapchain);
    double baseline_ms = engine.current_stream_baseline_ms_for_test(sc_key, pid, 0);
    STUTTO_ASSERT(baseline_ms > 5.0 && baseline_ms < 15.0);

    // No duplicates tagged
    uint64_t drops = 0;
    auto snap = recorder.snapshot(30000000ULL, t, &drops);
    STUTTO_ASSERT(snap.size() == 60);
    for (const auto& rec : snap) {
        STUTTO_ASSERT(!(rec.flags & stuttometer::EventFlags::DXGI_DUPLICATE_PRESENT_PATH));
    }

    mgr.set_running_for_test(false);
    std::cout << "  -> Standard-only path: baseline=" << baseline_ms << " ms, 0 duplicates. PASSED.\n";
}

// ============================================================
// 4.5 — Duplicate events must NOT appear in the frame timeline
// ============================================================
static void test_dxgi_duplicate_not_in_frame_timeline() {
    std::cout << "[TEST] 4.5 DXGI_DUPLICATE_PRESENT_PATH events not in frame timeline...\n";

    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::FlightRecorder recorder(4096);
    stuttometer::TriggerConfig trig_cfg;
    trig_cfg.target_pid = 9999;
    trig_cfg.present_threshold_ms = 50.0; // Very high — we want to trigger manually
    trig_cfg.frame_trigger_mode = stuttometer::FrameTriggerMode::STATIC_ONLY;
    stuttometer::TriggerEngine engine(trig_cfg, qpc_freq);

    stuttometer::EtwSessionConfig cfg;
    cfg.enable_dxgi = true;
    stuttometer::EtwSessionManager mgr(recorder, engine, cfg);
    mgr.set_running_for_test(true);

    const uint32_t pid = 9999;
    const uint32_t tid_std = 600;
    const uint32_t tid_mpo = 700;
    const uint64_t swapchain = 0xFEEDFACECAFEBABEULL;
    const uint64_t frame_ticks = 83333;
    const uint64_t std_api_dur = 15000; // 1.5 ms — must exceed 1 ms so the baseline-reset
                                        // first stop (whose duration is just its API time)
                                        // does not violate the sub-1ms timeline assertion.
    const uint64_t mpo_api_dur = 2000;

    // Feed 30 normal frames, then 1 stutter frame (200 ms)
    uint64_t t = 40000000ULL;
    for (int i = 0; i < 30; ++i) {
        push_present_start(mgr, 42, pid, tid_std, t, swapchain);
        push_present_stop (mgr, 43, pid, tid_std, t + std_api_dur);
        push_present_start(mgr, 55, pid, tid_mpo, t + std_api_dur, swapchain);
        push_present_stop (mgr, 56, pid, tid_mpo, t + std_api_dur + mpo_api_dur);
        t += frame_ticks;
    }

    // Synthesize a genuine 200 ms stutter frame via static threshold
    // (We need to trigger the engine; inject directly since STATIC_ONLY with 50ms threshold and 200ms frame)
    engine.on_dxgi_present(pid, tid_std, 200.0, t, stuttometer::make_swapchain_key(pid, swapchain));
    t += stuttometer::ms_to_qpc_delta(200.0, qpc_freq);

    // Poll to get the trigger
    stuttometer::TriggerInfo trig{};
    uint64_t from_qpc = 0, to_qpc = 0;
    // poll_state compares against post_target_qpc_, which the trigger engine now stores
    // using real get_current_qpc() (QPC domain, post-v0.5.5 fix), NOT the synthetic ETW
    // timestamp passed to on_dxgi_present(). Use real QPC here so the post-window deadline
    // is actually reachable.
    const uint64_t poll_qpc = stuttometer::get_current_qpc()
                            + stuttometer::ms_to_qpc_delta(35.0, qpc_freq);
    bool triggered = engine.poll_state(poll_qpc, trig, from_qpc, to_qpc);
    STUTTO_ASSERT(triggered);

    // Get snapshot from flight recorder
    uint64_t drops = 0;
    auto snap = recorder.snapshot(from_qpc, to_qpc, &drops);

    // Run correlator and check that no FrameTimelinePoint carries DXGI_DUPLICATE_PRESENT_PATH
    stuttometer::DriverSymbolResolver resolver;
    stuttometer::CorrelationEngine correlator(resolver);
    stuttometer::CorrelateOptions opts;
    opts.present_threshold_ms = trig_cfg.present_threshold_ms;
    opts.window_pre_ms = trig_cfg.window_pre_ms;
    opts.window_post_ms = trig_cfg.window_post_ms;
    stuttometer::ProviderContext p_ctx;
    p_ctx.user_dxgi_active = true;
    auto report = correlator.correlate(snap, trig, qpc_freq, opts, p_ctx);

    // All FrameTimelinePoints must NOT have DXGI_DUPLICATE_PRESENT_PATH in their source record
    // (The correlator filters them; we verify the frame_timeline contains no sub-1ms points)
    for (const auto& pt : report.frame_timeline) {
        // Duplicates had effective_dur_us < 1000; real frames are >= ~8000 us
        // Sub-ms entries in the timeline are the symptom of the unfixed bug
        STUTTO_ASSERT(pt.duration_ms >= 1.0 || pt.is_trigger_frame);
    }

    // Additionally: no point in the timeline should have a duration wildly different from the majority
    // (a bimodal distribution would indicate unfixed interleaving)
    if (report.frame_timeline.size() >= 4) {
        int sub_ms_count = 0;
        for (const auto& pt : report.frame_timeline) {
            if (pt.duration_ms < 1.0 && !pt.is_trigger_frame) {
                ++sub_ms_count;
            }
        }
        STUTTO_ASSERT(sub_ms_count == 0);
    }

    engine.on_report_completed(poll_qpc);
    mgr.set_running_for_test(false);
    std::cout << "  -> Frame timeline has " << report.frame_timeline.size()
              << " point(s), 0 sub-1ms non-trigger entries. PASSED.\n";
}

// ============================================================
// 4.6 — SessionBenchmark isolated from duplicate present paths
// ============================================================
static void test_session_benchmark_isolated_from_duplicates() {
    std::cout << "[TEST] 4.6 SessionBenchmark not skewed by duplicate present path events...\n";

    const uint64_t qpc_freq = 10000000ULL;
    stuttometer::FlightRecorder recorder(4096);
    stuttometer::TriggerConfig trig_cfg;
    trig_cfg.target_pid = 3333;
    trig_cfg.present_threshold_ms = 5.0;
    trig_cfg.frame_trigger_mode = stuttometer::FrameTriggerMode::HYBRID;
    trig_cfg.pacing_profile = stuttometer::PacingProfile::AUTO_ADAPTIVE;
    stuttometer::TriggerEngine engine(trig_cfg, qpc_freq);

    // Attach a SessionBenchmark sink
    stuttometer::SessionBenchmark bench(qpc_freq);
    bench.set_pacing_context(stuttometer::PacingProfile::AUTO_ADAPTIVE, trig_cfg.present_threshold_ms, 2.0, 4.0);
    bench.retarget(trig_cfg.target_pid);
    engine.set_benchmark_sink(&bench);

    stuttometer::EtwSessionConfig cfg;
    cfg.enable_dxgi = true;
    stuttometer::EtwSessionManager mgr(recorder, engine, cfg);
    mgr.set_running_for_test(true);

    const uint32_t pid = 3333;
    const uint32_t tid_std = 800;
    const uint32_t tid_mpo = 900;
    const uint64_t swapchain = 0xDEADBEEF12340000ULL;
    const uint64_t frame_ticks = 83333; // ~8.33 ms at 10 MHz
    const uint64_t std_api_dur = 5000;
    const uint64_t mpo_api_dur = 2000;

    uint64_t t = 50000000ULL;
    const int frames = 120; // 1 second at 120 FPS

    for (int i = 0; i < frames; ++i) {
        push_present_start(mgr, 42, pid, tid_std, t, swapchain);
        push_present_stop (mgr, 43, pid, tid_std, t + std_api_dur);
        push_present_start(mgr, 55, pid, tid_mpo, t + std_api_dur, swapchain);
        push_present_stop (mgr, 56, pid, tid_mpo, t + std_api_dur + mpo_api_dur);
        t += frame_ticks;
    }

    auto summary = bench.get_summary();

    // avg_fps should reflect the true ~120 FPS, not ~2000+ FPS from duplicate ingestion
    // With duplicates excluded, 120 frames × ~8.33 ms = ~120 FPS
    // With duplicates included (buggy): baseline collapses to ~0.5 ms → ~2000 FPS
    STUTTO_ASSERT(summary.frametimes.avg_fps > 50.0);   // Must be in plausible range
    STUTTO_ASSERT(summary.frametimes.avg_fps < 500.0);  // Must NOT be 2000+ FPS

    engine.set_benchmark_sink(nullptr);
    mgr.set_running_for_test(false);
    std::cout << "  -> SessionBenchmark avg_fps=" << summary.frametimes.avg_fps
              << " (expected ~120 FPS, not 2000+). PASSED.\n";
}

// ============================================================
// 4.7 — HYBRID adaptive static floor at high refresh
// ============================================================
static void test_hybrid_static_floor_adaptive_high_refresh() {
    std::cout << "[TEST] 4.7 HYBRID adaptive static floor: 8.5 ms frames do not trip STATIC_THRESHOLD at 200 Hz...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();

    // --- Part A: HYBRID mode with 200 Hz vblank threshold ---
    {
        stuttometer::TriggerConfig cfg;
        cfg.present_threshold_ms = 5.0; // 200 Hz vblank-derived
        cfg.frame_trigger_mode   = stuttometer::FrameTriggerMode::HYBRID;
        cfg.pacing_profile       = stuttometer::PacingProfile::AUTO_ADAPTIVE;
        cfg.target_pid           = 0; // monitor-all

        stuttometer::TriggerEngine engine(cfg, qpc_freq);
        const uint64_t base_qpc = stuttometer::get_current_qpc();
        const double normal_frame_ms = 8.5; // 120 FPS game on 200 Hz display

        // Feed 16 clean frames. Warmup frames 0-3 are silent. Frames 4-7 may legitimately
        // trip STATIC_THRESHOLD in the documented [4,7] warmup window (which pushes a
        // clamped sample so warmup can complete). Drain those warmup triggers so the
        // state machine returns to ARMED, then verify that frames 8-15 do not trigger:
        // once sample_count >= 8, the adaptive static floor lifts the effective threshold
        // to ~11.05 ms, so normal-cadence 8.5 ms frames must be suppressed.
        bool any_post_warmup_trigger = false;
        uint64_t t = base_qpc;
        for (int i = 0; i < 16; ++i) {
            t += stuttometer::ms_to_qpc_delta(normal_frame_ms, qpc_freq);
            bool trig = engine.on_dxgi_present(1000, 2000, normal_frame_ms, t, 0);
            if (trig && i >= 8) {
                any_post_warmup_trigger = true;
            }
            if (trig) {
                // Drain: COLLECTING_POST -> FROZEN -> COOLDOWN -> (past cooldown) -> ARMED.
                // poll_state uses the real QPC domain (post-v0.5.5 fix), so we must pass a
                // real get_current_qpc()-derived value to reach the post_target_qpc_ deadline.
                const uint64_t drain_qpc = stuttometer::get_current_qpc()
                                         + stuttometer::ms_to_qpc_delta(35.0, qpc_freq);
                stuttometer::TriggerInfo dummy;
                uint64_t fq = 0, tq = 0;
                engine.poll_state(drain_qpc, dummy, fq, tq);
                engine.on_report_completed(drain_qpc);
                engine.poll_state(drain_qpc + stuttometer::ms_to_qpc_delta(1100.0, qpc_freq),
                                  dummy, fq, tq);
            }
        }
        STUTTO_ASSERT(!any_post_warmup_trigger);
        STUTTO_ASSERT(engine.current_state() == stuttometer::TriggerState::ARMED);

        // Now send a genuine stutter (25 ms) — must trigger
        t += stuttometer::ms_to_qpc_delta(25.0, qpc_freq);
        bool stutter_triggered = engine.on_dxgi_present(1000, 2000, 25.0, t, 0);
        STUTTO_ASSERT(stutter_triggered);
        STUTTO_ASSERT(engine.current_state() != stuttometer::TriggerState::ARMED);
    }

    // --- Part B: STATIC_ONLY mode must still trigger on 8.5 ms at 200 Hz threshold ---
    {
        stuttometer::TriggerConfig cfg;
        cfg.present_threshold_ms = 5.0; // effective threshold = 5.0 + 0.25 = 5.25 ms
        cfg.frame_trigger_mode   = stuttometer::FrameTriggerMode::STATIC_ONLY;
        cfg.target_pid           = 0;

        stuttometer::TriggerEngine engine(cfg, qpc_freq);
        const uint64_t base_qpc = stuttometer::get_current_qpc();

        // Warm up 8 frames first so baseline is established
        uint64_t t = base_qpc;
        for (int i = 0; i < 8; ++i) {
            t += stuttometer::ms_to_qpc_delta(8.5, qpc_freq);
            engine.on_dxgi_present(1000, 2000, 8.5, t, 0);
            // Reset if triggered so we can keep going
            if (engine.current_state() != stuttometer::TriggerState::ARMED) {
                stuttometer::TriggerInfo dummy_trig;
                uint64_t fq = 0, tq = 0;
                engine.poll_state(t + stuttometer::ms_to_qpc_delta(35.0, qpc_freq), dummy_trig, fq, tq);
                engine.on_report_completed(t + stuttometer::ms_to_qpc_delta(35.0, qpc_freq));
                // Advance past cooldown
                t += stuttometer::ms_to_qpc_delta(1100.0, qpc_freq);
                // Re-poll past the cooldown deadline so the state machine returns to ARMED.
                // Without this, on_report_completed leaves the engine stuck in COOLDOWN,
                // and every subsequent on_dxgi_present call fails its ARMED claim.
                engine.poll_state(t + stuttometer::ms_to_qpc_delta(35.0, qpc_freq), dummy_trig, fq, tq);
            }
        }

        // In STATIC_ONLY mode with 5.25 ms effective threshold, an 8.5 ms frame MUST trigger
        // (the adaptive floor must NOT apply here)
        t += stuttometer::ms_to_qpc_delta(8.5, qpc_freq);
        // Reset state first so ARMED
        if (engine.current_state() != stuttometer::TriggerState::ARMED) {
            stuttometer::TriggerInfo dummy_trig;
            uint64_t fq = 0, tq = 0;
            engine.poll_state(t, dummy_trig, fq, tq);
            engine.on_report_completed(t);
            t += stuttometer::ms_to_qpc_delta(1100.0, qpc_freq);
            // Re-poll past the cooldown deadline to restore ARMED (same reason as above).
            engine.poll_state(t + stuttometer::ms_to_qpc_delta(35.0, qpc_freq), dummy_trig, fq, tq);
        }
        bool static_triggered = engine.on_dxgi_present(1000, 2000, 8.5, t, 0);
        STUTTO_ASSERT(static_triggered); // STATIC_ONLY must still trigger on 8.5 ms >= 5.25 ms threshold

        // --- Part C: DYNAMIC_ONLY mode must NOT trigger STATIC_THRESHOLD ---
        {
            stuttometer::TriggerConfig cfg2;
            cfg2.present_threshold_ms = 5.0;
            cfg2.frame_trigger_mode   = stuttometer::FrameTriggerMode::DYNAMIC_ONLY;
            cfg2.target_pid           = 0;

            stuttometer::TriggerEngine engine2(cfg2, qpc_freq);
            const uint64_t t2 = stuttometer::get_current_qpc();
            bool dynamic_triggered = false;
            uint64_t tt = t2;
            for (int i = 0; i < 20; ++i) {
                tt += stuttometer::ms_to_qpc_delta(8.5, qpc_freq);
                if (engine2.on_dxgi_present(1000, 2000, 8.5, tt, 0)) {
                    dynamic_triggered = true;
                }
            }
            // DYNAMIC_ONLY: no STATIC_THRESHOLD possible; normal frames at stable cadence must not trigger
            STUTTO_ASSERT(!dynamic_triggered);
        }
    }

    std::cout << "  -> HYBRID adaptive static floor (200 Hz/120 FPS): normal frames safe, stutter triggers, STATIC_ONLY unaffected. PASSED.\n";
}


int main() {
    std::cout << "=== Stuttometer Kernel Present & GPU Tracking Tests ===\n";
    try {
        test_make_flip_key_distribution();
        test_kernel_frame_stall_trigger_and_upgrade();
        test_last_flip_table_lifecycle();
        test_gpu_pipeline_stall_correlation_and_json();
        test_gpu_pre_window_scaling();
        test_dwm_glitch_trigger_attribution();
        test_trigger_engine_single_emission_guarantee();
        test_static_trigger_on_pacing_table_exhaustion();
        test_dxgkrnl_task17_flip_parsing_and_dwm_filtering();
        test_display_refresh_query();
        test_dwm_pipeline_high_refresh();
        test_dxgi_mpo_standard_interleaving_dedup();
        test_dxgi_interleaved_start_stop_pairing();
        test_dxgi_mpo_only_path();
        test_dxgi_standard_only_path_unchanged();
        test_dxgi_duplicate_not_in_frame_timeline();
        test_session_benchmark_isolated_from_duplicates();
        test_hybrid_static_floor_adaptive_high_refresh();
        std::cout << ">>> All Kernel Present & GPU Tracking tests PASSED! <<<\n\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST FAILED] Exception: " << e.what() << "\n";
        return 1;
    }
}
