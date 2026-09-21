#include "test_common.hpp"
#include "stuttometer/etw_session.hpp"
#include "stuttometer/trigger_engine.hpp"
#include "stuttometer/correlator.hpp"
#include "stuttometer/json_reporter.hpp"
#include "stuttometer/privilege_utils.hpp"
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

    const uint64_t base_qpc = 1000000;

    // Sub-threshold glitch (< 3.667 ms) rejected
    STUTTO_ASSERT(!engine.on_dwm_glitch(100, 200, 3.0, base_qpc, 0));

    // Single 240Hz vblank glitch (4.167 ms) accepted even though present_threshold_ms = 16.67 ms!
    STUTTO_ASSERT(engine.on_dwm_glitch(100, 200, VBLANK_240HZ_MS, base_qpc, 0));

    stuttometer::TriggerInfo trig;
    uint64_t from_qpc = 0, to_qpc = 0;
    const uint64_t poll_qpc = base_qpc + stuttometer::ms_to_qpc_delta(35.0, qpc_freq);
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
    const uint64_t poll2_qpc = 20000000 + stuttometer::ms_to_qpc_delta(35.0, qpc_freq);
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
        std::cout << ">>> All Kernel Present & GPU Tracking tests PASSED! <<<\n\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST FAILED] Exception: " << e.what() << "\n";
        return 1;
    }
}
