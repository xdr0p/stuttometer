#include "test_common.hpp"
#include "stuttometer/session_benchmark.hpp"
#include "stuttometer/trigger_engine.hpp"
#include "stuttometer/privilege_utils.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <cmath>

static void test_glass_smooth() {
    std::cout << "[TEST 1] Glass Smooth Test...\n";
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();
    stuttometer::SessionBenchmark benchmark(qpc_freq);
    benchmark.retarget(1234);

    const double frame_ms = 1000.0 / 60.0;
    const uint64_t frame_delta_qpc = stuttometer::ms_to_qpc_delta(frame_ms, qpc_freq);

    for (uint64_t i = 0; i < 60000; ++i) {
        benchmark.ingest_frame(1234, frame_ms, base_qpc + i * frame_delta_qpc);
    }

    auto summary = benchmark.get_summary();
    STUTTO_ASSERT(summary.total_frames == 60000);
    STUTTO_ASSERT(std::abs(summary.frametimes.avg_fps - 60.0) < 0.01);
    STUTTO_ASSERT(std::abs(summary.frametimes.low_1pct_fps - 60.0) < 0.01);
    STUTTO_ASSERT(std::abs(summary.frametimes.low_01pct_fps - 60.0) < 0.01);
    STUTTO_ASSERT(summary.stutters_detected == 0);
    std::cout << "[TEST 1] PASSED\n";
}

static void test_micro_stutter_percentiles() {
    std::cout << "[TEST 2] Micro-Stutter Percentile Validation...\n";
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    uint64_t cur_qpc = stuttometer::get_current_qpc();
    stuttometer::SessionBenchmark benchmark(qpc_freq);
    benchmark.retarget(1234);

    // 10,000 frames @ 16.6667ms
    for (int i = 0; i < 10000; ++i) {
        benchmark.ingest_frame(1234, 16.6667, cur_qpc);
        cur_qpc += stuttometer::ms_to_qpc_delta(16.6667, qpc_freq);
    }
    // 100 frames @ 50.0ms
    for (int i = 0; i < 100; ++i) {
        benchmark.ingest_frame(1234, 50.0, cur_qpc);
        cur_qpc += stuttometer::ms_to_qpc_delta(50.0, qpc_freq);
    }
    // 10 frames @ 100.0ms
    for (int i = 0; i < 10; ++i) {
        benchmark.ingest_frame(1234, 100.0, cur_qpc);
        cur_qpc += stuttometer::ms_to_qpc_delta(100.0, qpc_freq);
    }

    auto summary = benchmark.get_summary();
    STUTTO_ASSERT(summary.total_frames == 10110);
    // index_01pct = 9 -> low_01pct_fps = 1000 / 100.0 = 10.0
    STUTTO_ASSERT(std::abs(summary.frametimes.low_01pct_fps - 10.0) < 0.01);
    // index_1pct = 100 -> low_1pct_fps = 1000 / 50.0 = 20.0
    STUTTO_ASSERT(std::abs(summary.frametimes.low_1pct_fps - 20.0) < 0.01);
    std::cout << "[TEST 2] PASSED\n";
}

static void test_small_sample_guard() {
    std::cout << "[TEST 3] Small Sample Guard Test...\n";
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    uint64_t cur_qpc = stuttometer::get_current_qpc();
    stuttometer::SessionBenchmark benchmark(qpc_freq);
    benchmark.retarget(1234);

    for (int i = 0; i < 90; ++i) {
        benchmark.ingest_frame(1234, 16.67, cur_qpc);
        cur_qpc += stuttometer::ms_to_qpc_delta(16.67, qpc_freq);
    }
    auto summary90 = benchmark.get_summary();
    STUTTO_ASSERT(summary90.total_frames == 90);
    STUTTO_ASSERT(summary90.frametimes.low_1pct_fps == 0.0);
    STUTTO_ASSERT(summary90.frametimes.low_01pct_fps == 0.0);

    for (int i = 0; i < 410; ++i) { // 90 + 410 = 500
        benchmark.ingest_frame(1234, 16.67, cur_qpc);
        cur_qpc += stuttometer::ms_to_qpc_delta(16.67, qpc_freq);
    }
    auto summary500 = benchmark.get_summary();
    STUTTO_ASSERT(summary500.total_frames == 500);
    STUTTO_ASSERT(summary500.frametimes.low_1pct_fps > 0.0);
    STUTTO_ASSERT(summary500.frametimes.low_01pct_fps == 0.0);
    std::cout << "[TEST 3] PASSED\n";
}

static void test_pause_alt_tab_ceiling() {
    std::cout << "[TEST 4] Pause / Alt-Tab Ceiling Test...\n";
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    uint64_t cur_qpc = stuttometer::get_current_qpc();
    stuttometer::SessionBenchmark benchmark(qpc_freq);
    benchmark.retarget(1234);

    for (int i = 0; i < 100; ++i) {
        benchmark.ingest_frame(1234, 16.67, cur_qpc);
        cur_qpc += stuttometer::ms_to_qpc_delta(16.67, qpc_freq);
    }

    // 15-second gap frame
    cur_qpc += stuttometer::ms_to_qpc_delta(15000.0, qpc_freq);
    benchmark.ingest_frame(1234, 15000.0, cur_qpc);

    for (int i = 0; i < 100; ++i) {
        benchmark.ingest_frame(1234, 16.67, cur_qpc);
        cur_qpc += stuttometer::ms_to_qpc_delta(16.67, qpc_freq);
    }

    auto summary = benchmark.get_summary();
    STUTTO_ASSERT(summary.dropped_pause_frames == 1);
    STUTTO_ASSERT(summary.total_frames == 200);
    STUTTO_ASSERT(summary.frametimes.max_frametime_ms < 100.0);
    std::cout << "[TEST 4] PASSED\n";
}

static void test_non_positive_duration_guard() {
    std::cout << "[TEST 5] Non-Positive Duration Guard Test...\n";
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    uint64_t cur_qpc = stuttometer::get_current_qpc();
    stuttometer::SessionBenchmark benchmark(qpc_freq);
    benchmark.retarget(1234);

    benchmark.ingest_frame(1234, 0.0, cur_qpc);
    benchmark.ingest_frame(1234, -5.0, cur_qpc + 1000);

    auto summary = benchmark.get_summary();
    STUTTO_ASSERT(summary.total_frames == 0);
    std::cout << "[TEST 5] PASSED\n";
}

static void test_attribution_aggregation() {
    std::cout << "[TEST 6] Attribution Aggregation & Top Driver Test...\n";
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    stuttometer::SessionBenchmark benchmark(qpc_freq);
    benchmark.retarget(1234);

    auto make_rep = [](const std::string& hyp, const std::string& drv, double dur, double base) {
        stuttometer::DiagnosticReport r;
        r.trigger.target_pid = 1234;
        r.trigger.duration_ms = dur;
        r.trigger.baseline_avg_ms = base;
        stuttometer::Diagnosis d;
        d.hypothesis = hyp;
        d.confidence = 0.9;
        if (!drv.empty()) {
            stuttometer::EvidenceItem ev;
            ev.driver_module = drv;
            d.evidence.push_back(ev);
        }
        r.diagnoses.push_back(d);
        return r;
    };

    // 2x dpc_isr_spike, nvlddmkm.sys (50.0ms each)
    benchmark.ingest_report(make_rep("dpc_isr_spike", "nvlddmkm.sys", 50.0, 16.67));
    benchmark.ingest_report(make_rep("dpc_isr_spike", "nvlddmkm.sys", 50.0, 16.67));
    // 1x dpc_isr_spike, ndis.sys (25.0ms)
    benchmark.ingest_report(make_rep("dpc_isr_spike", "ndis.sys", 25.0, 16.67));
    // 1x d3d12_shader_pso_compilation_stall, d3d12.dll (100.0ms)
    benchmark.ingest_report(make_rep("d3d12_shader_pso_compilation_stall", "d3d12.dll", 100.0, 16.67));
    // 1x vram_exhaustion_paging_stall, dxgkrnl.sys (25.0ms)
    benchmark.ingest_report(make_rep("vram_exhaustion_paging_stall", "dxgkrnl.sys", 25.0, 16.67));

    auto summary = benchmark.get_summary();
    STUTTO_ASSERT(std::abs(summary.net_stall_ms - 250.0) < 0.01);
    STUTTO_ASSERT(summary.culprits.size() >= 3);

    // Culprits are sorted descending:
    // 1. dpc_isr_spike (125ms = 50.0%)
    // 2. d3d12_shader_pso_compilation_stall (100ms = 40.0%)
    // 3. vram_exhaustion_paging_stall (25ms = 10.0%)
    STUTTO_ASSERT(summary.culprits[0].hypothesis == "dpc_isr_spike");
    STUTTO_ASSERT(std::abs(summary.culprits[0].total_stall_ms - 125.0) < 0.01);
    STUTTO_ASSERT(std::abs(summary.culprits[0].stall_pct - 50.0) < 0.01);
    STUTTO_ASSERT(summary.culprits[0].top_driver == "nvlddmkm.sys");

    STUTTO_ASSERT(summary.culprits[1].hypothesis == "d3d12_shader_pso_compilation_stall");
    STUTTO_ASSERT(std::abs(summary.culprits[1].total_stall_ms - 100.0) < 0.01);
    STUTTO_ASSERT(std::abs(summary.culprits[1].stall_pct - 40.0) < 0.01);

    STUTTO_ASSERT(summary.culprits[2].hypothesis == "vram_exhaustion_paging_stall");
    STUTTO_ASSERT(std::abs(summary.culprits[2].total_stall_ms - 25.0) < 0.01);
    STUTTO_ASSERT(std::abs(summary.culprits[2].stall_pct - 10.0) < 0.01);
    std::cout << "[TEST 6] PASSED\n";
}

static void test_top_5_truncation() {
    std::cout << "[TEST 7] Top-5 Truncation & 'Other' Remainder Test...\n";
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    stuttometer::SessionBenchmark benchmark(qpc_freq);
    benchmark.retarget(1234);

    for (int i = 1; i <= 8; ++i) {
        stuttometer::DiagnosticReport r;
        r.trigger.target_pid = 1234;
        r.trigger.duration_ms = i * 10.0;
        stuttometer::Diagnosis d;
        d.hypothesis = "hyp_" + std::to_string(i);
        d.confidence = 0.8;
        r.diagnoses.push_back(d);
        benchmark.ingest_report(r);
    }

    auto summary = benchmark.get_summary();
    // 8 distinct hypotheses -> top 5 + "Other" = 6 entries
    STUTTO_ASSERT(summary.culprits.size() == 6);
    STUTTO_ASSERT(summary.culprits[0].hypothesis == "hyp_8");
    STUTTO_ASSERT(summary.culprits[1].hypothesis == "hyp_7");
    STUTTO_ASSERT(summary.culprits[2].hypothesis == "hyp_6");
    STUTTO_ASSERT(summary.culprits[3].hypothesis == "hyp_5");
    STUTTO_ASSERT(summary.culprits[4].hypothesis == "hyp_4");
    STUTTO_ASSERT(summary.culprits[5].hypothesis == "Other");
    // Other covers hyp_1 (10ms) + hyp_2 (20ms) + hyp_3 (30ms) = 60ms
    STUTTO_ASSERT(std::abs(summary.culprits[5].total_stall_ms - 60.0) < 0.01);
    std::cout << "[TEST 7] PASSED\n";
}

static void test_target_pid_filtering_and_epoch() {
    std::cout << "[TEST 8] Target PID Filtering & Epoch Exclusion Test...\n";
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    uint64_t cur_qpc = stuttometer::get_current_qpc();
    stuttometer::SessionBenchmark benchmark(qpc_freq);

    benchmark.retarget(100);
    for (int i = 0; i < 50; ++i) {
        benchmark.ingest_frame(100, 16.67, cur_qpc);
        cur_qpc += 1000;
    }

    benchmark.retarget(200);
    // Ingest 10 frames for PID 100 (assert dropped)
    for (int i = 0; i < 10; ++i) {
        benchmark.ingest_frame(100, 16.67, cur_qpc);
        cur_qpc += 1000;
    }
    // Ingest 25 frames for PID 200 (assert recorded)
    for (int i = 0; i < 25; ++i) {
        benchmark.ingest_frame(200, 16.67, cur_qpc);
        cur_qpc += 1000;
    }

    auto summary = benchmark.get_summary();
    STUTTO_ASSERT(summary.total_frames == 25);
    std::cout << "[TEST 8] PASSED\n";
}

static void test_reset_and_clean_restart() {
    std::cout << "[TEST 9] Reset & Clean Restart Test...\n";
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    uint64_t cur_qpc = stuttometer::get_current_qpc();
    stuttometer::SessionBenchmark benchmark(qpc_freq);

    benchmark.retarget(1234);
    for (int i = 0; i < 100; ++i) {
        benchmark.ingest_frame(1234, 16.67, cur_qpc);
        cur_qpc += 1000;
    }

    benchmark.reset();

    for (int i = 0; i < 50; ++i) {
        benchmark.ingest_frame(1234, 16.67, cur_qpc);
        cur_qpc += 1000;
    }

    auto summary = benchmark.get_summary();
    STUTTO_ASSERT(summary.total_frames == 50);
    std::cout << "[TEST 9] PASSED\n";
}

static void test_pii_redaction() {
    std::cout << "[TEST 10] PII Redaction Unit Test...\n";
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    stuttometer::SessionBenchmark benchmark(qpc_freq);
    benchmark.retarget(1234);

    benchmark.ingest_frame(1234, 16.67, stuttometer::get_current_qpc());

    stuttometer::DiagnosticReport r;
    r.target_process = "Game.exe";
    r.trigger.target_pid = 1234;
    r.trigger.duration_ms = 50.0;
    stuttometer::Diagnosis d;
    d.hypothesis = "dpc_isr_spike";
    d.confidence = 0.9;
    stuttometer::EvidenceItem ev;
    ev.driver_module = "nvlddmkm.sys";
    d.evidence.push_back(ev);
    r.diagnoses.push_back(d);
    benchmark.ingest_report(r);

    auto summary = benchmark.get_summary(true);
    STUTTO_ASSERT(summary.target_process == "Process_REDACTED");
    STUTTO_ASSERT(summary.target_pid == 0);
    STUTTO_ASSERT(!summary.culprits.empty());
    STUTTO_ASSERT(summary.culprits[0].top_driver == "driver_REDACTED.sys");
    STUTTO_ASSERT(summary.redacted == true);
    std::cout << "[TEST 10] PASSED\n";
}

static void test_concurrency_stress() {
    std::cout << "[TEST 11] Concurrency Stress & Invariant Test...\n";
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    stuttometer::SessionBenchmark benchmark(qpc_freq);
    benchmark.retarget(1234);

    std::atomic<bool> producers_running{true};
    const int NUM_PRODUCERS = 4;
    const int FRAMES_PER_PRODUCER = 25000;

    std::vector<std::thread> producers;
    producers.reserve(NUM_PRODUCERS);
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&benchmark, p, qpc_freq]() {
            uint64_t qpc = stuttometer::get_current_qpc() + p * 1000;
            for (int i = 0; i < FRAMES_PER_PRODUCER; ++i) {
                benchmark.ingest_frame(1234, 16.67, qpc);
                qpc += 100;
            }
        });
    }

    const int NUM_CONSUMERS = 2;
    std::vector<std::thread> consumers;
    consumers.reserve(NUM_CONSUMERS);
    std::atomic<bool> test_failed{false};

    for (int c = 0; c < NUM_CONSUMERS; ++c) {
        consumers.emplace_back([&benchmark, &producers_running, &test_failed]() {
            uint64_t last_valid = 0;
            while (producers_running.load(std::memory_order_relaxed)) {
                auto summary = benchmark.get_summary();
                if (!std::isfinite(summary.frametimes.avg_fps) ||
                    !std::isfinite(summary.frametimes.low_1pct_fps) ||
                    !std::isfinite(summary.frametimes.low_01pct_fps)) {
                    test_failed.store(true);
                }
                // Tolerant monotonicity
                if (summary.total_frames + 4 < last_valid) {
                    test_failed.store(true);
                }
                last_valid = summary.total_frames;

                // Guarded Positivity (Resolves B-10-1)
                if (summary.total_frames >= 1000) {
                    if (summary.frametimes.avg_fps <= 0.0 ||
                        summary.frametimes.low_1pct_fps <= 0.0 ||
                        summary.frametimes.low_01pct_fps <= 0.0) {
                        test_failed.store(true);
                    }
                }
            }
        });
    }

    for (auto& t : producers) {
        t.join();
    }
    producers_running.store(false, std::memory_order_release);
    for (auto& t : consumers) {
        t.join();
    }

    STUTTO_ASSERT(!test_failed.load());

    auto final_summary = benchmark.get_summary();
    STUTTO_ASSERT(final_summary.total_frames == 100000);
    STUTTO_ASSERT(final_summary.frametimes.avg_fps > 0.0);
    STUTTO_ASSERT(final_summary.frametimes.low_1pct_fps > 0.0);
    STUTTO_ASSERT(final_summary.frametimes.low_01pct_fps > 0.0);
    std::cout << "[TEST 11] PASSED\n";
}

static void test_trigger_engine_canonical_routing() {
    std::cout << "[TEST 12] TriggerEngine Canonical Routing Test...\n";
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();

    stuttometer::TriggerConfig config;
    config.present_threshold_ms = 16.67;
    config.target_pid = 100;
    stuttometer::TriggerEngine trigger_engine(config, qpc_freq);
    stuttometer::SessionBenchmark benchmark(qpc_freq);

    benchmark.retarget(100);
    trigger_engine.set_benchmark_sink(&benchmark);
    trigger_engine.update_target_pid(100, false);

    // Step 1: Call trigger_engine.on_dxgi_present(100, 1, 16.67, get_current_qpc(), 0, 0)
    trigger_engine.on_dxgi_present(100, 1, 16.67, stuttometer::get_current_qpc(), 0, 0);
    STUTTO_ASSERT(benchmark.get_summary().total_frames == 1);

    // Step 2: Call trigger_engine.on_kernel_frame_stall(100, 1, 16.67, get_current_qpc() + 1000, 0, 0)
    // Suppressed because DXGI flag is set for this target
    trigger_engine.on_kernel_frame_stall(100, 1, 16.67, stuttometer::get_current_qpc() + 1000, 0, 0);
    STUTTO_ASSERT(benchmark.get_summary().total_frames == 1);

    // Step 3: Retarget to PID 200
    benchmark.retarget(200);
    trigger_engine.update_target_pid(200, false);
    STUTTO_ASSERT(benchmark.get_summary().total_frames == 0); // clean reset for new target

    // Step 4: Advance timestamp beyond 1.0s guard strictly after Step 3's update_target_pid
    const uint64_t stall_qpc = stuttometer::get_current_qpc() + stuttometer::ms_to_qpc_delta(1100.0, qpc_freq);
    trigger_engine.on_kernel_frame_stall(200, 1, 16.67, stall_qpc, 0, 0);
    // Vulkan fallback path active, ingested!
    STUTTO_ASSERT(benchmark.get_summary().total_frames == 1);
    std::cout << "[TEST 12] PASSED\n";
}

static void test_serialization_and_tag_stats() {
    std::cout << "[TEST 13] Serialization Formatting & Deterministic Tag Stats Test...\n";
    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    stuttometer::SessionBenchmark benchmark(qpc_freq);
    benchmark.retarget(1234);

    // Empty summary markdown validation
    auto empty_summary = benchmark.get_summary();
    std::string md_empty = empty_summary.to_markdown();
    STUTTO_ASSERT(md_empty.find("| Max Frametime | N/A |") != std::string::npos);
    STUTTO_ASSERT(md_empty.find("| (None) | - | 0 | 0.0 ms | 0.0% |") != std::string::npos);

    // Redacted markdown validation (PID must be N/A)
    auto redacted_summary = benchmark.get_summary(true);
    std::string md_redacted = redacted_summary.to_markdown();
    STUTTO_ASSERT(md_redacted.find("- **Target Process:** Process_REDACTED (PID: N/A)") != std::string::npos);

    // Deterministic tag_stats descending sort
    stuttometer::DiagnosticReport r1;
    r1.trigger.target_pid = 1234;
    r1.trigger.duration_ms = 10.0;
    r1.attribution = stuttometer::AttributionTag::DWM_COMPOSITION;
    benchmark.ingest_report(r1);

    stuttometer::DiagnosticReport r2;
    r2.trigger.target_pid = 1234;
    r2.trigger.duration_ms = 50.0;
    r2.attribution = stuttometer::AttributionTag::EXTERNAL_CONTENTION;
    benchmark.ingest_report(r2);

    auto summary = benchmark.get_summary();
    STUTTO_ASSERT(summary.tag_stats.size() == 2);
    // Sorted descending by total_stall_ms: EXTERNAL_CONTENTION (50ms) > DWM_COMPOSITION (10ms)
    STUTTO_ASSERT(summary.tag_stats[0].tag == stuttometer::AttributionTag::EXTERNAL_CONTENTION);
    STUTTO_ASSERT(std::abs(summary.tag_stats[0].total_stall_ms - 50.0) < 0.01);
    STUTTO_ASSERT(summary.tag_stats[1].tag == stuttometer::AttributionTag::DWM_COMPOSITION);
    STUTTO_ASSERT(std::abs(summary.tag_stats[1].total_stall_ms - 10.0) < 0.01);

    std::cout << "[TEST 13] PASSED\n";
}

int main() {
    try {
        test_glass_smooth();
        test_micro_stutter_percentiles();
        test_small_sample_guard();
        test_pause_alt_tab_ceiling();
        test_non_positive_duration_guard();
        test_attribution_aggregation();
        test_top_5_truncation();
        test_target_pid_filtering_and_epoch();
        test_reset_and_clean_restart();
        test_pii_redaction();
        test_concurrency_stress();
        test_trigger_engine_canonical_routing();
        test_serialization_and_tag_stats();

        std::cout << "\nAll Session Benchmark tests PASSED successfully!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "\nTest suite failed with exception: " << ex.what() << "\n";
        return 1;
    }
}
