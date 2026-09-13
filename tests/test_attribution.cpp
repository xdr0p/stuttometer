#include "test_common.hpp"
#include "stuttometer/correlator.hpp"
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

int main() {
    std::cout << "=== Stuttometer Attribution Unit Tests ===\n";
    try {
        test_attribution_low_confidence();
        test_attribution_all_hypotheses();
        test_attribution_redaction();
        std::cout << ">>> All Attribution tests PASSED! <<<\n\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST FAILED] Exception: " << e.what() << "\n";
        return 1;
    }
}
