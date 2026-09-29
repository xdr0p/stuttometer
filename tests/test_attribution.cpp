#include "test_common.hpp"
#include "stuttometer/correlator.hpp"
#include "stuttometer/privilege_utils.hpp"
#include <iostream>

static void test_attribution_low_confidence() {
    stuttometer::DiagnosticReport report;
    report.target_process = "Game.exe";
    report.trigger.target_pid = 1234;

    // 1. Empty diagnoses
    auto res1 = stuttometer::compute_attribution(report, 999);
    STUTTO_ASSERT(res1.tag == stuttometer::AttributionTag::UNKNOWN);
    STUTTO_ASSERT(res1.pid == 0);
    STUTTO_ASSERT(res1.process == "Unknown");
    STUTTO_ASSERT(!res1.redacted);

    // 2. Low confidence (< 0.30)
    stuttometer::Diagnosis diag;
    diag.hypothesis = "dpc_isr_spike";
    diag.confidence = 0.25;
    report.diagnoses.push_back(diag);

    auto res2 = stuttometer::compute_attribution(report, 999);
    STUTTO_ASSERT(res2.tag == stuttometer::AttributionTag::UNKNOWN);
    STUTTO_ASSERT(res2.pid == 0);
    STUTTO_ASSERT(res2.process == "Unknown");
    STUTTO_ASSERT(!res2.redacted);

    std::cout << "[TEST] Low confidence & empty diagnoses PASSED.\n";
}

static void test_attribution_all_hypotheses() {
    uint32_t fake_dwm_pid = 8888;

    auto make_report = [](const std::string& hyp, double conf = 0.85) {
        stuttometer::DiagnosticReport r;
        r.target_process = "Game.exe";
        r.trigger.target_pid = 1234;
        stuttometer::Diagnosis diag;
        diag.hypothesis = hyp;
        diag.confidence = conf;
        r.diagnoses.push_back(diag);
        return r;
    };

    // 1. dpc_isr_spike (with driver)
    {
        auto r = make_report("dpc_isr_spike");
        stuttometer::EvidenceItem ev;
        ev.driver_module = "nvlddmkm.sys";
        r.diagnoses[0].evidence.push_back(ev);
        auto res = stuttometer::compute_attribution(r, fake_dwm_pid);
        STUTTO_ASSERT(res.tag == stuttometer::AttributionTag::EXTERNAL_CONTENTION);
        STUTTO_ASSERT(res.pid == 4);
        STUTTO_ASSERT(res.process == "nvlddmkm.sys (System)");
        STUTTO_ASSERT(!res.redacted);
    }
    // dpc_isr_spike (unresolved)
    {
        auto r = make_report("dpc_isr_spike");
        auto res = stuttometer::compute_attribution(r, fake_dwm_pid);
        STUTTO_ASSERT(res.tag == stuttometer::AttributionTag::EXTERNAL_CONTENTION);
        STUTTO_ASSERT(res.pid == 4);
        STUTTO_ASSERT(res.process == "System (unresolved driver)");
    }

    // 2. disk_io_stall
    {
        auto r = make_report("disk_io_stall");
        stuttometer::EvidenceItem ev;
        ev.pid = 4;
        r.diagnoses[0].evidence.push_back(ev);
        auto res = stuttometer::compute_attribution(r, fake_dwm_pid);
        STUTTO_ASSERT(res.tag == stuttometer::AttributionTag::EXTERNAL_CONTENTION);
        STUTTO_ASSERT(res.pid == 4);
        STUTTO_ASSERT(!res.process.empty());
    }

    // 3. context_switch_interference
    {
        auto r = make_report("context_switch_interference");
        stuttometer::EvidenceItem ev;
        ev.secondary_pid = 5678;
        r.diagnoses[0].evidence.push_back(ev);
        auto res = stuttometer::compute_attribution(r, fake_dwm_pid);
        STUTTO_ASSERT(res.tag == stuttometer::AttributionTag::EXTERNAL_CONTENTION);
        STUTTO_ASSERT(res.pid == 5678);
    }

    // 4. gpu_pipeline_stall
    {
        auto r = make_report("gpu_pipeline_stall");
        auto res = stuttometer::compute_attribution(r, fake_dwm_pid);
        STUTTO_ASSERT(res.tag == stuttometer::AttributionTag::GAME_ENGINE);
        STUTTO_ASSERT(res.pid == 1234);
        STUTTO_ASSERT(res.process == "Game.exe");
    }

    // 5. dwm_compositor_stall
    {
        auto r = make_report("dwm_compositor_stall");
        auto res = stuttometer::compute_attribution(r, fake_dwm_pid);
        STUTTO_ASSERT(res.tag == stuttometer::AttributionTag::DWM_COMPOSITION);
        STUTTO_ASSERT(res.pid == fake_dwm_pid);
        STUTTO_ASSERT(res.process == "dwm.exe");
    }
    {
        auto r = make_report("dwm_compositor_stall");
        auto res = stuttometer::compute_attribution(r, 0); // unresolved dwm
        STUTTO_ASSERT(res.tag == stuttometer::AttributionTag::DWM_COMPOSITION);
        STUTTO_ASSERT(res.pid == 4);
        STUTTO_ASSERT(res.process == "dwm.exe (unresolved, System)");
    }

    // 6. page_fault_stall (target vs external)
    {
        auto r = make_report("page_fault_stall");
        stuttometer::EvidenceItem ev;
        ev.pid = 1234; // target
        r.diagnoses[0].evidence.push_back(ev);
        auto res = stuttometer::compute_attribution(r, fake_dwm_pid);
        STUTTO_ASSERT(res.tag == stuttometer::AttributionTag::GAME_ENGINE);
        STUTTO_ASSERT(res.pid == 1234);
        STUTTO_ASSERT(res.process == "Game.exe");
    }
    {
        auto r = make_report("page_fault_stall");
        stuttometer::EvidenceItem ev;
        ev.pid = 9999; // external
        r.diagnoses[0].evidence.push_back(ev);
        auto res = stuttometer::compute_attribution(r, fake_dwm_pid);
        STUTTO_ASSERT(res.tag == stuttometer::AttributionTag::EXTERNAL_CONTENTION);
        STUTTO_ASSERT(res.pid == 9999);
    }

    // 7. thermal_throttle
    {
        auto r = make_report("thermal_throttle");
        auto res = stuttometer::compute_attribution(r, fake_dwm_pid);
        STUTTO_ASSERT(res.tag == stuttometer::AttributionTag::EXTERNAL_CONTENTION);
        STUTTO_ASSERT(res.pid == 0);
        STUTTO_ASSERT(res.process == "CPU Thermal Throttling (Hardware)");
    }

    // 8. antimalware_interference
    {
        auto r = make_report("antimalware_interference");
        stuttometer::EvidenceItem ev;
        ev.pid = 2222;
        r.diagnoses[0].evidence.push_back(ev);
        auto res = stuttometer::compute_attribution(r, fake_dwm_pid);
        STUTTO_ASSERT(res.tag == stuttometer::AttributionTag::EXTERNAL_CONTENTION);
        STUTTO_ASSERT(res.pid == 2222);
        STUTTO_ASSERT(res.process == "MsMpEng.exe");
    }

    // 9. d3d12_shader_pso_compilation_stall
    {
        auto r = make_report("d3d12_shader_pso_compilation_stall");
        auto res = stuttometer::compute_attribution(r, fake_dwm_pid);
        STUTTO_ASSERT(res.tag == stuttometer::AttributionTag::GAME_ENGINE);
        STUTTO_ASSERT(res.pid == 1234);
        STUTTO_ASSERT(res.process == "Game.exe");
    }

    // 10. vram_exhaustion_paging_stall (target vs external)
    {
        auto r = make_report("vram_exhaustion_paging_stall");
        stuttometer::EvidenceItem ev;
        ev.pid = 1234;
        r.diagnoses[0].evidence.push_back(ev);
        auto res = stuttometer::compute_attribution(r, fake_dwm_pid);
        STUTTO_ASSERT(res.tag == stuttometer::AttributionTag::GAME_ENGINE);
        STUTTO_ASSERT(res.pid == 1234);
    }
    {
        auto r = make_report("vram_exhaustion_paging_stall");
        stuttometer::EvidenceItem ev;
        ev.pid = 7777;
        r.diagnoses[0].evidence.push_back(ev);
        auto res = stuttometer::compute_attribution(r, fake_dwm_pid);
        STUTTO_ASSERT(res.tag == stuttometer::AttributionTag::EXTERNAL_CONTENTION);
        STUTTO_ASSERT(res.pid == 7777);
    }

    // 11. virtual_memory_allocation_stall (target vs external)
    {
        auto r = make_report("virtual_memory_allocation_stall");
        stuttometer::EvidenceItem ev;
        ev.pid = 0; // defaults to target
        r.diagnoses[0].evidence.push_back(ev);
        auto res = stuttometer::compute_attribution(r, fake_dwm_pid);
        STUTTO_ASSERT(res.tag == stuttometer::AttributionTag::GAME_ENGINE);
        STUTTO_ASSERT(res.pid == 1234);
    }
    {
        auto r = make_report("virtual_memory_allocation_stall");
        stuttometer::EvidenceItem ev;
        ev.pid = 5555;
        r.diagnoses[0].evidence.push_back(ev);
        auto res = stuttometer::compute_attribution(r, fake_dwm_pid);
        STUTTO_ASSERT(res.tag == stuttometer::AttributionTag::EXTERNAL_CONTENTION);
        STUTTO_ASSERT(res.pid == 5555);
    }

    // 12. low_memory_working_set_trim_stall
    {
        auto r = make_report("low_memory_working_set_trim_stall");
        auto res = stuttometer::compute_attribution(r, fake_dwm_pid);
        STUTTO_ASSERT(res.tag == stuttometer::AttributionTag::EXTERNAL_CONTENTION);
        STUTTO_ASSERT(res.pid == 4);
        STUTTO_ASSERT(res.process == "NT Kernel (System Memory Manager)");
    }

    // 13. physical_memory_allocation_latency
    {
        auto r = make_report("physical_memory_allocation_latency");
        auto res = stuttometer::compute_attribution(r, fake_dwm_pid);
        STUTTO_ASSERT(res.tag == stuttometer::AttributionTag::EXTERNAL_CONTENTION);
        STUTTO_ASSERT(res.pid == 4);
        STUTTO_ASSERT(res.process == "NT Kernel (System Memory Manager)");
    }

    // 14. frame_pacing_judder
    {
        auto r = make_report("frame_pacing_judder");
        auto res = stuttometer::compute_attribution(r, fake_dwm_pid);
        STUTTO_ASSERT(res.tag == stuttometer::AttributionTag::GAME_ENGINE);
        STUTTO_ASSERT(res.pid == 1234);
        STUTTO_ASSERT(res.process == "Game.exe");
    }

    // 15. unprofiled_hardware_or_smi_stall
    {
        auto r = make_report("unprofiled_hardware_or_smi_stall");
        auto res = stuttometer::compute_attribution(r, fake_dwm_pid);
        STUTTO_ASSERT(res.tag == stuttometer::AttributionTag::EXTERNAL_CONTENTION);
        STUTTO_ASSERT(res.pid == 0);
        STUTTO_ASSERT(res.process == "Hardware / BIOS SMI Execution");
    }

    // 16. Unknown hypothesis
    {
        auto r = make_report("some_future_unhandled_hypothesis");
        auto res = stuttometer::compute_attribution(r, fake_dwm_pid);
        STUTTO_ASSERT(res.tag == stuttometer::AttributionTag::UNKNOWN);
        STUTTO_ASSERT(res.pid == 0);
        STUTTO_ASSERT(res.process == "Unknown");
    }

    std::cout << "[TEST] All 16 hypotheses verified successfully.\n";
}

static void test_attribution_redaction() {
    stuttometer::DiagnosticReport report;
    report.target_process = "SecretGame.exe";
    report.trigger.target_pid = 1234;
    report.redacted = true;

    stuttometer::Diagnosis diag;
    diag.hypothesis = "gpu_pipeline_stall";
    diag.confidence = 0.90;
    report.diagnoses.push_back(diag);

    auto res = stuttometer::compute_attribution(report, 8888);
    STUTTO_ASSERT(res.tag == stuttometer::AttributionTag::GAME_ENGINE);
    STUTTO_ASSERT(res.pid == 0);
    STUTTO_ASSERT(res.process == "REDACTED");
    STUTTO_ASSERT(res.redacted);

    std::cout << "[TEST] Attribution redaction PASSED.\n";
}

static void test_hypothesis_attribution_mapping() {
    // NOTE: This test enumerates every hypothesis produced by correlator.cpp. If a new hypothesis is added to the correlator, update this test accordingly.
    using stuttometer::AttributionTag;
    using stuttometer::attribution_tag_for_hypothesis;

    // External contention (8 hypotheses)
    STUTTO_ASSERT(attribution_tag_for_hypothesis("dpc_isr_spike") == AttributionTag::EXTERNAL_CONTENTION);
    STUTTO_ASSERT(attribution_tag_for_hypothesis("disk_io_stall") == AttributionTag::EXTERNAL_CONTENTION);
    STUTTO_ASSERT(attribution_tag_for_hypothesis("context_switch_interference") == AttributionTag::EXTERNAL_CONTENTION);
    STUTTO_ASSERT(attribution_tag_for_hypothesis("thermal_throttle") == AttributionTag::EXTERNAL_CONTENTION);
    STUTTO_ASSERT(attribution_tag_for_hypothesis("antimalware_interference") == AttributionTag::EXTERNAL_CONTENTION);
    STUTTO_ASSERT(attribution_tag_for_hypothesis("low_memory_working_set_trim_stall") == AttributionTag::EXTERNAL_CONTENTION);
    STUTTO_ASSERT(attribution_tag_for_hypothesis("physical_memory_allocation_latency") == AttributionTag::EXTERNAL_CONTENTION);
    STUTTO_ASSERT(attribution_tag_for_hypothesis("unprofiled_hardware_or_smi_stall") == AttributionTag::EXTERNAL_CONTENTION);

    // Game engine (6 hypotheses including the three defaults)
    STUTTO_ASSERT(attribution_tag_for_hypothesis("gpu_pipeline_stall") == AttributionTag::GAME_ENGINE);
    STUTTO_ASSERT(attribution_tag_for_hypothesis("d3d12_shader_pso_compilation_stall") == AttributionTag::GAME_ENGINE);
    STUTTO_ASSERT(attribution_tag_for_hypothesis("frame_pacing_judder") == AttributionTag::GAME_ENGINE);
    STUTTO_ASSERT(attribution_tag_for_hypothesis("page_fault_stall") == AttributionTag::GAME_ENGINE);
    STUTTO_ASSERT(attribution_tag_for_hypothesis("vram_exhaustion_paging_stall") == AttributionTag::GAME_ENGINE);
    STUTTO_ASSERT(attribution_tag_for_hypothesis("virtual_memory_allocation_stall") == AttributionTag::GAME_ENGINE);

    // DWM composition (1 hypothesis)
    STUTTO_ASSERT(attribution_tag_for_hypothesis("dwm_compositor_stall") == AttributionTag::DWM_COMPOSITION);

    // Unknown / fallback (1 hypothesis + edge cases)
    STUTTO_ASSERT(attribution_tag_for_hypothesis("insufficient_evidence") == AttributionTag::UNKNOWN);
    STUTTO_ASSERT(attribution_tag_for_hypothesis("") == AttributionTag::UNKNOWN);
    STUTTO_ASSERT(attribution_tag_for_hypothesis("nonexistent_hypothesis") == AttributionTag::UNKNOWN);

    std::cout << "[TEST] Hypothesis attribution mapping PASSED.\n";
}

static void test_compute_attribution_pid_override() {
    using stuttometer::AttributionTag;

    auto test_override = [](const std::string& hyp) {
        // Case A: ev_pid != target_pid -> overrides to EXTERNAL_CONTENTION
        {
            stuttometer::DiagnosticReport r;
            r.target_process = "Game.exe";
            r.trigger.target_pid = 1234;
            stuttometer::Diagnosis diag;
            diag.hypothesis = hyp;
            diag.confidence = 0.85;
            stuttometer::EvidenceItem ev;
            ev.pid = 9999; // External process
            diag.evidence.push_back(ev);
            r.diagnoses.push_back(diag);

            auto res = stuttometer::compute_attribution(r, 8888);
            STUTTO_ASSERT(res.tag == AttributionTag::EXTERNAL_CONTENTION);
            STUTTO_ASSERT(res.pid == 9999);
        }
        // Case B: ev_pid == target_pid -> stays GAME_ENGINE
        {
            stuttometer::DiagnosticReport r;
            r.target_process = "Game.exe";
            r.trigger.target_pid = 1234;
            stuttometer::Diagnosis diag;
            diag.hypothesis = hyp;
            diag.confidence = 0.85;
            stuttometer::EvidenceItem ev;
            ev.pid = 1234; // Same as target
            diag.evidence.push_back(ev);
            r.diagnoses.push_back(diag);

            auto res = stuttometer::compute_attribution(r, 8888);
            STUTTO_ASSERT(res.tag == AttributionTag::GAME_ENGINE);
            STUTTO_ASSERT(res.pid == 1234);
            STUTTO_ASSERT(res.process == "Game.exe");
        }
        // Case C: ev_pid == 0 -> stays GAME_ENGINE
        {
            stuttometer::DiagnosticReport r;
            r.target_process = "Game.exe";
            r.trigger.target_pid = 1234;
            stuttometer::Diagnosis diag;
            diag.hypothesis = hyp;
            diag.confidence = 0.85;
            r.diagnoses.push_back(diag);

            auto res = stuttometer::compute_attribution(r, 8888);
            STUTTO_ASSERT(res.tag == AttributionTag::GAME_ENGINE);
            STUTTO_ASSERT(res.pid == 1234);
            STUTTO_ASSERT(res.process == "Game.exe");
        }
    };

    test_override("page_fault_stall");
    test_override("vram_exhaustion_paging_stall");
    test_override("virtual_memory_allocation_stall");

    std::cout << "[TEST] compute_attribution PID override PASSED.\n";
}

static void test_classify_severity() {
    using stuttometer::MetricSeverity;
    using stuttometer::TriggerInfo;
    using stuttometer::TriggerSource;
    using stuttometer::TriggerReason;

    // 1. Audio underrun -> unconditionally DANGER
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::AUDIO_GLITCH;
        trig.glitch_count = 1;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::DANGER);

        trig.glitch_count = 5;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::DANGER);
    }

    // 2. High refresh: 240Hz, 4.16ms baseline, 25ms stall -> spike_ratio = 6.0 >= 3.0 -> DANGER
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::DXGI_PRESENT_STUTTER;
        trig.duration_ms = 25.0;
        trig.baseline_avg_ms = 4.16;
        trig.baseline_fps = 240.0;
        trig.spike_ratio = 6.0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::DANGER);
    }

    // 3. Standard refresh: 60Hz, 16.66ms baseline, 35ms stall -> spike_ratio = 2.1 -> WARNING
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::DXGI_PRESENT_STUTTER;
        trig.duration_ms = 35.0;
        trig.baseline_avg_ms = 16.66;
        trig.baseline_fps = 60.0;
        trig.spike_ratio = 2.1;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::WARNING);
    }

    // 4. Low refresh: 30Hz, 33.33ms baseline, 25ms frame -> spike_ratio = 0.75 -> NORMAL
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::DXGI_PRESENT_STUTTER;
        trig.duration_ms = 25.0;
        trig.baseline_avg_ms = 33.33;
        trig.baseline_fps = 30.0;
        trig.spike_ratio = 0.75;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::NORMAL);
    }

    // 5. DWM glitch:
    // 0 vblanks -> duration 16.67, baseline 16.67, spike_ratio 1.0 -> NORMAL
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::DWM_GLITCH;
        trig.duration_ms = 16.67;
        trig.baseline_avg_ms = 16.67;
        trig.baseline_fps = 60.0;
        trig.spike_ratio = 1.0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::NORMAL);
    }
    // 1 vblank -> duration 16.67, baseline 16.67, spike_ratio 1.0 -> NORMAL
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::DWM_GLITCH;
        trig.duration_ms = 16.67;
        trig.baseline_avg_ms = 16.67;
        trig.baseline_fps = 60.0;
        trig.spike_ratio = 1.0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::NORMAL);
    }
    // 2 vblanks -> duration 33.34, baseline 16.67, spike_ratio 2.0 -> WARNING
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::DWM_GLITCH;
        trig.duration_ms = 33.34;
        trig.baseline_avg_ms = 16.67;
        trig.baseline_fps = 60.0;
        trig.spike_ratio = 2.0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::WARNING);
    }
    // 3 vblanks -> duration 50.01, baseline 16.67, spike_ratio 3.0 -> DANGER
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::DWM_GLITCH;
        trig.duration_ms = 50.01;
        trig.baseline_avg_ms = 16.67;
        trig.baseline_fps = 60.0;
        trig.spike_ratio = 3.0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::DANGER);
    }

    // 6. Judder magnitude classification:
    // Test values use hard-coded numeric literals (not named constants) to strictly lock the threshold
    // contract so future tuning changes force a deliberate, conscious test update.

    // --- DANGER tier single-condition and boundary tests ---
    // 6a. DANGER: only high_alt true (hard-coded literal: 30)
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::FRAME_PACING_JUDDER;
        trig.reason = TriggerReason::CADENCE_JUDDER;
        trig.judder_alternations = 30;
        trig.duration_ms = 0.0;
        trig.judder_max_swing_q100 = 0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::DANGER);
    }
    // 6b. Boundary just below DANGER high_alt: alternations = 29 -> drops to WARNING (hard-coded literal: 29)
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::FRAME_PACING_JUDDER;
        trig.reason = TriggerReason::CADENCE_JUDDER;
        trig.judder_alternations = 29;
        trig.duration_ms = 0.0;
        trig.judder_max_swing_q100 = 0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::WARNING);
    }
    // 6c. DANGER: only long_dur true (hard-coded literal: 500.0)
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::FRAME_PACING_JUDDER;
        trig.reason = TriggerReason::CADENCE_JUDDER;
        trig.judder_alternations = 0;
        trig.duration_ms = 500.0;
        trig.judder_max_swing_q100 = 0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::DANGER);
    }
    // 6d. Boundary just below DANGER long_dur: duration_ms = 499.9 -> drops to WARNING (hard-coded literal: 499.9)
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::FRAME_PACING_JUDDER;
        trig.reason = TriggerReason::CADENCE_JUDDER;
        trig.judder_alternations = 0;
        trig.duration_ms = 499.9;
        trig.judder_max_swing_q100 = 0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::WARNING);
    }
    // 6e. DANGER: only high_swing true (hard-coded literals: swing 60, alternations 5)
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::FRAME_PACING_JUDDER;
        trig.reason = TriggerReason::CADENCE_JUDDER;
        trig.judder_max_swing_q100 = 60;
        trig.judder_alternations = 5;
        trig.duration_ms = 0.0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::DANGER);
    }
    // 6f. Boundary just below DANGER high_swing: swing = 59, alternations = 5 -> drops to WARNING (hard-coded literal: 59)
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::FRAME_PACING_JUDDER;
        trig.reason = TriggerReason::CADENCE_JUDDER;
        trig.judder_max_swing_q100 = 59;
        trig.judder_alternations = 5;
        trig.duration_ms = 0.0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::WARNING);
    }
    // 6g. Conjunction isolation for DANGER swing: swing >= 60 but alternations < 5 (alt = 4) -> drops to WARNING (hard-coded literals: 60, 4)
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::FRAME_PACING_JUDDER;
        trig.reason = TriggerReason::CADENCE_JUDDER;
        trig.judder_max_swing_q100 = 60;
        trig.judder_alternations = 4;
        trig.duration_ms = 0.0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::WARNING);
    }

    // --- WARNING tier single-condition and boundary tests ---
    // 6h. WARNING: only mid_alt true (hard-coded literal: 10)
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::FRAME_PACING_JUDDER;
        trig.reason = TriggerReason::CADENCE_JUDDER;
        trig.judder_alternations = 10;
        trig.duration_ms = 0.0;
        trig.judder_max_swing_q100 = 0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::WARNING);
    }
    // 6i. Boundary just below WARNING mid_alt: alternations = 9 -> drops to NORMAL (hard-coded literal: 9)
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::FRAME_PACING_JUDDER;
        trig.reason = TriggerReason::CADENCE_JUDDER;
        trig.judder_alternations = 9;
        trig.duration_ms = 0.0;
        trig.judder_max_swing_q100 = 0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::NORMAL);
    }
    // 6j. WARNING: only mid_dur true (hard-coded literal: 150.0)
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::FRAME_PACING_JUDDER;
        trig.reason = TriggerReason::CADENCE_JUDDER;
        trig.judder_alternations = 0;
        trig.duration_ms = 150.0;
        trig.judder_max_swing_q100 = 0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::WARNING);
    }
    // 6k. Boundary just below WARNING mid_dur: duration_ms = 149.9 -> drops to NORMAL (hard-coded literal: 149.9)
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::FRAME_PACING_JUDDER;
        trig.reason = TriggerReason::CADENCE_JUDDER;
        trig.judder_alternations = 0;
        trig.duration_ms = 149.9;
        trig.judder_max_swing_q100 = 0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::NORMAL);
    }
    // 6l. WARNING: only mid_swing true (hard-coded literals: swing 40, alternations 3)
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::FRAME_PACING_JUDDER;
        trig.reason = TriggerReason::CADENCE_JUDDER;
        trig.judder_max_swing_q100 = 40;
        trig.judder_alternations = 3;
        trig.duration_ms = 0.0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::WARNING);
    }
    // 6m. Boundary just below WARNING mid_swing: swing = 39, alternations = 3 -> drops to NORMAL (hard-coded literal: 39)
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::FRAME_PACING_JUDDER;
        trig.reason = TriggerReason::CADENCE_JUDDER;
        trig.judder_max_swing_q100 = 39;
        trig.judder_alternations = 3;
        trig.duration_ms = 0.0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::NORMAL);
    }
    // 6n. Conjunction isolation for WARNING swing: swing >= 40 but alternations < 3 (alt = 2) -> drops to NORMAL (hard-coded literals: 40, 2)
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::FRAME_PACING_JUDDER;
        trig.reason = TriggerReason::CADENCE_JUDDER;
        trig.judder_max_swing_q100 = 40;
        trig.judder_alternations = 2;
        trig.duration_ms = 0.0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::NORMAL);
    }

    // --- NORMAL tier tests ---
    // 6o. NORMAL: all dimensions well below (hard-coded literals: 0, 0.0, 0)
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::FRAME_PACING_JUDDER;
        trig.reason = TriggerReason::CADENCE_JUDDER;
        trig.duration_ms = 0.0;
        trig.judder_alternations = 0;
        trig.judder_max_swing_q100 = 0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::NORMAL);
    }

    // 7. Fallback without baseline (baseline_avg_ms == 0.0 or spike_ratio == 0.0)
    // 60 Hz baseline (default ref_ms = 16.67, thresholds ~50.01 and ~25.005)
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::DXGI_PRESENT_STUTTER;
        trig.baseline_avg_ms = 0.0;
        trig.spike_ratio = 0.0;

        trig.duration_ms = 52.0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::DANGER);

        trig.duration_ms = 48.0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::WARNING);

        trig.duration_ms = 26.0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::WARNING);

        trig.duration_ms = 24.0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::NORMAL);

        trig.duration_ms = 0.0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::NORMAL);
    }
    // Multi-refresh tests:
    // 240 Hz (ref_ms = 1000.0 / 240.0 = 4.1667, thresholds = 12.5 and 6.25)
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::DXGI_PRESENT_STUTTER;
        trig.baseline_avg_ms = 0.0;
        trig.spike_ratio = 0.0;
        const double ref_240hz = 1000.0 / 240.0;

        trig.duration_ms = 13.5;
        STUTTO_ASSERT(stuttometer::classify_severity(trig, ref_240hz) == MetricSeverity::DANGER);

        trig.duration_ms = 11.5;
        STUTTO_ASSERT(stuttometer::classify_severity(trig, ref_240hz) == MetricSeverity::WARNING);

        trig.duration_ms = 7.0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig, ref_240hz) == MetricSeverity::WARNING);

        trig.duration_ms = 5.5;
        STUTTO_ASSERT(stuttometer::classify_severity(trig, ref_240hz) == MetricSeverity::NORMAL);
    }
    // 30 Hz (ref_ms = 1000.0 / 30.0 = 33.333, thresholds = 100.0 and 50.0)
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::DXGI_PRESENT_STUTTER;
        trig.baseline_avg_ms = 0.0;
        trig.spike_ratio = 0.0;
        const double ref_30hz = 1000.0 / 30.0;

        trig.duration_ms = 105.0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig, ref_30hz) == MetricSeverity::DANGER);

        trig.duration_ms = 95.0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig, ref_30hz) == MetricSeverity::WARNING);

        trig.duration_ms = 55.0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig, ref_30hz) == MetricSeverity::WARNING);

        trig.duration_ms = 45.0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig, ref_30hz) == MetricSeverity::NORMAL);
    }
    // Fallback without baseline for Judder with mid magnitude (duration >= 150.0) -> WARNING
    {
        TriggerInfo trig{};
        trig.source = TriggerSource::FRAME_PACING_JUDDER;
        trig.duration_ms = 150.0;
        trig.baseline_avg_ms = 0.0;
        trig.spike_ratio = 0.0;
        STUTTO_ASSERT(stuttometer::classify_severity(trig) == MetricSeverity::WARNING);
    }

    std::cout << "[TEST] classify_severity PASSED.\n";
}

static void test_vram_attribution_flag_filtering() {
    std::cout << "[TEST] Validating VRAM attribution flag filtering and fallback unblocking...\n";

    const uint64_t qpc_freq = stuttometer::get_qpc_frequency();
    const uint64_t base_qpc = stuttometer::get_current_qpc();

    stuttometer::DriverSymbolResolver driver_resolver;
    stuttometer::CorrelationEngine correlator(driver_resolver);

    const uint32_t target_pid = 1234;
    const uint32_t external_pid = 7777; // e.g. obs64.exe
    const uint32_t fake_dwm_pid = 8888;

    // Part A: Synthesize ETW record with VRAM_USAGE_OVER_BUDGET (17.8 MB) from external PID
    {
        stuttometer::TriggerInfo trigger{};
        trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
        trigger.reason = stuttometer::TriggerReason::RELATIVE_SPIKE;
        trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
        trigger.duration_ms = 45.0;
        trigger.target_pid = target_pid;
        trigger.target_tid = 5678;

        std::vector<stuttometer::EtwEventRecord> snapshot;
        stuttometer::EtwEventRecord vram{};
        vram.category = static_cast<uint16_t>(stuttometer::EventCategory::DXGKRNL_VRAM_PAGING);
        vram.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(10.0, qpc_freq);
        vram.pid = external_pid;
        vram.auxiliary_data = static_cast<uint64_t>(17.8 * 1024.0 * 1024.0);
        vram.flags = stuttometer::EventFlags::VRAM_USAGE_OVER_BUDGET; // NOT VRAM_DEMOTED_COMMITMENT
        snapshot.push_back(vram);

        stuttometer::ProviderContext p_ctx{};
        p_ctx.user_vram_paging_active = true;

        auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);
        report.target_process = "Game.exe";

        // Event counted in summary
        STUTTO_ASSERT(report.event_counts.dxgkrnl_vram_paging == 1);

        // vram_candidates must be empty -> no vram_exhaustion_paging_stall diagnosis
        for (const auto& diag : report.diagnoses) {
            STUTTO_ASSERT(diag.hypothesis != "vram_exhaustion_paging_stall");
        }

        // Fallback diagnosis gpu_pipeline_stall must be unblocked and present
        STUTTO_ASSERT(!report.diagnoses.empty());
        STUTTO_ASSERT(report.diagnoses[0].hypothesis == "gpu_pipeline_stall");

        // Attribution must not blame external_pid
        auto attr = stuttometer::compute_attribution(report, fake_dwm_pid);
        STUTTO_ASSERT(attr.tag == stuttometer::AttributionTag::GAME_ENGINE);
        STUTTO_ASSERT(attr.pid == target_pid);
        STUTTO_ASSERT(attr.process == "Game.exe");
    }

    // Part B: Synthesize ETW record with VRAM_DEMOTED_COMMITMENT (17.8 MB) from target PID
    {
        stuttometer::TriggerInfo trigger{};
        trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
        trigger.reason = stuttometer::TriggerReason::RELATIVE_SPIKE;
        trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
        trigger.duration_ms = 45.0;
        trigger.target_pid = target_pid;
        trigger.target_tid = 5678;

        std::vector<stuttometer::EtwEventRecord> snapshot;
        stuttometer::EtwEventRecord vram{};
        vram.category = static_cast<uint16_t>(stuttometer::EventCategory::DXGKRNL_VRAM_PAGING);
        vram.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(5.0, qpc_freq);
        vram.pid = target_pid;
        vram.auxiliary_data = static_cast<uint64_t>(17.8 * 1024.0 * 1024.0);
        vram.flags = stuttometer::EventFlags::VRAM_DEMOTED_COMMITMENT;
        snapshot.push_back(vram);

        stuttometer::ProviderContext p_ctx{};
        p_ctx.user_vram_paging_active = true;

        auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);
        report.target_process = "Game.exe";

        STUTTO_ASSERT(!report.diagnoses.empty());
        STUTTO_ASSERT(report.diagnoses[0].hypothesis == "vram_exhaustion_paging_stall");
        STUTTO_ASSERT(report.diagnoses[0].confidence >= 0.60);

        auto attr = stuttometer::compute_attribution(report, fake_dwm_pid);
        STUTTO_ASSERT(attr.tag == stuttometer::AttributionTag::GAME_ENGINE);
        STUTTO_ASSERT(attr.pid == target_pid);
    }

    // Part C: Verify external PID with distant/sub-threshold demotion does not attain attribution
    {
        stuttometer::TriggerInfo trigger{};
        trigger.source = stuttometer::TriggerSource::DXGI_PRESENT_STUTTER;
        trigger.reason = stuttometer::TriggerReason::RELATIVE_SPIKE;
        trigger.trigger_timestamp_qpc = base_qpc + stuttometer::ms_to_qpc_delta(250.0, qpc_freq);
        trigger.duration_ms = 45.0;
        trigger.target_pid = target_pid;
        trigger.target_tid = 5678;

        std::vector<stuttometer::EtwEventRecord> snapshot;
        stuttometer::EtwEventRecord vram{};
        vram.category = static_cast<uint16_t>(stuttometer::EventCategory::DXGKRNL_VRAM_PAGING);
        vram.qpc_timestamp = trigger.trigger_timestamp_qpc - stuttometer::ms_to_qpc_delta(90.0, qpc_freq);
        vram.pid = external_pid;
        vram.auxiliary_data = static_cast<uint64_t>(17.8 * 1024.0 * 1024.0);
        vram.flags = stuttometer::EventFlags::VRAM_DEMOTED_COMMITMENT;
        snapshot.push_back(vram);

        stuttometer::ProviderContext p_ctx{};
        p_ctx.user_vram_paging_active = true;

        auto report = correlator.correlate(snapshot, trigger, qpc_freq, p_ctx);
        report.target_process = "Game.exe";

        // Confidence without target PID and with low temporal proximity is < 0.30
        auto attr = stuttometer::compute_attribution(report, fake_dwm_pid);
        STUTTO_ASSERT(attr.tag != stuttometer::AttributionTag::EXTERNAL_CONTENTION || attr.pid != external_pid);
    }

    std::cout << "  -> test_vram_attribution_flag_filtering PASSED.\n";
}

int main() {
    std::cout << "=== Stuttometer Attribution Unit Tests ===\n";
    try {
        test_attribution_low_confidence();
        test_attribution_all_hypotheses();
        test_attribution_redaction();
        test_hypothesis_attribution_mapping();
        test_compute_attribution_pid_override();
        test_classify_severity();
        test_vram_attribution_flag_filtering();
        std::cout << ">>> All Attribution tests PASSED! <<<\n\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST FAILED] Exception: " << e.what() << "\n";
        return 1;
    }
}
