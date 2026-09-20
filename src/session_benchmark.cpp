#include "stuttometer/session_benchmark.hpp"
#include "stuttometer/internal/redaction_utils.hpp"
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace stuttometer {

std::string BenchmarkSummary::to_markdown() const {
    std::ostringstream oss;
    oss << "# Stuttometer Session Benchmark Summary\n\n";
    oss << "- **Target Process:** " << (target_process.empty() ? "N/A" : target_process)
        << " (PID: " << (target_pid == 0 ? "N/A" : std::to_string(target_pid)) << ")\n";

    const unsigned total_sec = static_cast<unsigned>(duration_ms / 1000.0);
    const unsigned mins = total_sec / 60;
    const unsigned secs = total_sec % 60;
    oss << "- **Duration:** " << std::setfill('0') << std::setw(2) << mins << ":"
        << std::setfill('0') << std::setw(2) << secs << "\n";
    oss << "- **Total Frames:** " << total_frames << "\n";
    oss << "- **Stutters Detected:** " << stutters_detected << "\n";
    oss << "- **Net Stall Time:** " << std::fixed << std::setprecision(1) << net_stall_ms << " ms\n\n";

    oss << "## Frame Pacing\n\n";
    oss << "| Metric | Value |\n";
    oss << "| :--- | :--- |\n";
    if (frametimes.avg_fps > 0.0) {
        oss << "| Average FPS | " << std::fixed << std::setprecision(1) << frametimes.avg_fps << " |\n";
    } else {
        oss << "| Average FPS | N/A |\n";
    }
    if (frametimes.low_1pct_fps > 0.0) {
        oss << "| 1% Low FPS | " << std::fixed << std::setprecision(1) << frametimes.low_1pct_fps << " |\n";
    } else {
        oss << "| 1% Low FPS | N/A |\n";
    }
    if (frametimes.low_01pct_fps > 0.0) {
        oss << "| 0.1% Low FPS | " << std::fixed << std::setprecision(1) << frametimes.low_01pct_fps << " |\n";
    } else {
        oss << "| 0.1% Low FPS | N/A |\n";
    }
    if (frametimes.max_frametime_ms > 0.0) {
        oss << "| Max Frametime | " << std::fixed << std::setprecision(1) << frametimes.max_frametime_ms << " ms |\n\n";
    } else {
        oss << "| Max Frametime | N/A |\n\n";
    }

    oss << "## Observed Presentation Cadence\n\n";
    oss << "| Metric | Value |\n";
    oss << "| :--- | :--- |\n";
    if (is_monitor_all) {
        oss << "| Active Profile | N/A (Monitor-All) |\n";
        oss << "| Status | Cadence telemetry unavailable in Monitor-All mode (select a target process) |\n\n";
    } else if (binding_floor_source == BindingFloorSource::NONE) {
        oss << "| Active Profile | " << pacing_profile_to_string(pacing_profile) << " |\n";
        oss << "| Baseline Cadence | Pending warmup (\u22658 frames) |\n";
        oss << "| Dynamic Trigger | Pending warmup (\u22658 frames) |\n";
        oss << "| Static Threshold | \u2265 " << std::fixed << std::setprecision(1) << static_floor_ms << " ms |\n";
        oss << "| Estimated Stall Floor | Pending warmup (\u22658 frames) |\n\n";
    } else {
        oss << "| Active Profile | " << pacing_profile_to_string(pacing_profile) << " |\n";
        oss << "| Baseline Cadence | " << std::fixed << std::setprecision(1) << rolling_baseline_ms << " ms ("
            << std::fixed << std::setprecision(1) << rolling_fps << " FPS) |\n";
        oss << "| Dynamic Trigger | \u2265 " << std::fixed << std::setprecision(1) << estimated_dynamic_floor_ms << " ms |\n";
        oss << "| Static Threshold | \u2265 " << std::fixed << std::setprecision(1) << static_floor_ms << " ms |\n";
        oss << "| Estimated Stall Floor | \u2265 " << std::fixed << std::setprecision(1) << estimated_binding_floor_ms << " ms ("
            << (binding_floor_source == BindingFloorSource::DYNAMIC ? "Dynamic" : "Static") << ") |\n\n";
    }

    oss << "## Culprit Attribution (Top Stutter Causes)\n\n";
    oss << "| Hypothesis | Top Driver | Count | Total Stall | Stall % |\n";
    oss << "| :--- | :--- | :--- | :--- | :--- |\n";
    if (culprits.empty()) {
        oss << "| (None) | - | 0 | 0.0 ms | 0.0% |\n";
    } else {
        for (const auto& c : culprits) {
            oss << "| " << c.hypothesis << " | "
                << (c.top_driver_module.empty() ? "-" : c.top_driver_module) << " | "
                << c.count << " | "
                << std::fixed << std::setprecision(1) << c.total_stall_ms << " ms | "
                << std::fixed << std::setprecision(1) << c.stall_pct << "% |\n";
        }
    }
    return oss.str();
}

std::string BenchmarkSummary::to_json() const {
    nlohmann::json j;
    j["schema_version"] = "1.2";
    j["target_process"] = target_process;
    j["target_pid"] = target_pid;
    j["duration_ms"] = duration_ms;
    j["total_frames"] = total_frames;
    j["stutters_detected"] = stutters_detected;
    j["dropped_pause_frames"] = dropped_pause_frames;
    j["redacted"] = redacted;
    j["frametimes"] = {
        {"avg_fps", frametimes.avg_fps},
        {"p99_frametime_ms", frametimes.p99_frametime_ms},
        {"low_1pct_fps", frametimes.low_1pct_fps},
        {"p999_frametime_ms", frametimes.p999_frametime_ms},
        {"low_01pct_fps", frametimes.low_01pct_fps},
        {"max_frametime_ms", frametimes.max_frametime_ms}
    };
    j["net_stall_ms"] = net_stall_ms;
    j["worst_stutter_ms"] = worst_stutter_ms;
    j["worst_stutter_hypothesis"] = worst_stutter_hypothesis;

    nlohmann::json cadence;
    cadence["pacing_profile"] = pacing_profile_to_string(pacing_profile);
    cadence["binding_floor_source"] = binding_floor_source_to_string(binding_floor_source);
    if (binding_floor_source == BindingFloorSource::NONE) {
        cadence["estimated_dynamic_floor_ms"] = nullptr;
        cadence["estimated_binding_floor_ms"] = nullptr;
        cadence["rolling_baseline_ms"] = nullptr;
        cadence["rolling_fps"] = nullptr;
    } else {
        cadence["estimated_dynamic_floor_ms"] = estimated_dynamic_floor_ms;
        cadence["estimated_binding_floor_ms"] = estimated_binding_floor_ms;
        cadence["rolling_baseline_ms"] = rolling_baseline_ms;
        cadence["rolling_fps"] = rolling_fps;
    }
    cadence["static_floor_ms"] = static_floor_ms;
    j["presentation_cadence"] = cadence;

    nlohmann::json culprits_arr = nlohmann::json::array();
    for (const auto& c : culprits) {
        culprits_arr.push_back({
            {"hypothesis", c.hypothesis},
            {"top_driver", c.top_driver_module},
            {"count", c.count},
            {"total_stall_ms", c.total_stall_ms},
            {"avg_confidence_pct", c.avg_confidence_pct},
            {"stall_pct", c.stall_pct}
        });
    }
    j["culprits"] = culprits_arr;

    nlohmann::json tag_arr = nlohmann::json::array();
    for (const auto& t : tag_stats) {
        tag_arr.push_back({
            {"tag", attribution_to_std_string(t.tag)},
            {"count", t.count},
            {"total_stall_ms", t.total_stall_ms},
            {"stall_pct", t.stall_pct}
        });
    }
    j["tag_stats"] = tag_arr;

    return j.dump(2);
}

SessionBenchmark::SessionBenchmark(uint64_t qpc_freq)
    : qpc_freq_(qpc_freq)
    , pause_ceiling_qpc_(ms_to_qpc_delta(PAUSE_CEILING_MS, qpc_freq))
    , scratch_buffer_(std::make_unique<double[]>(RING_CAPACITY))
    , ring_(std::make_unique<FrameSlot[]>(RING_CAPACITY))
{
}

void SessionBenchmark::ingest_frame(uint32_t pid, double duration_ms, uint64_t timestamp_qpc) noexcept {
    if (duration_ms <= 0.0) {
        return;
    }
    // D-1: Checking duration_ms directly is necessary and sufficient: the ETW subsystem
    // already suppresses baseline-reset frames (is_baseline_reset == true), and checking
    // duration directly avoids cross-producer MPSC data races that would arise from
    // maintaining an atomic last_timestamp_qpc_.
    if (duration_ms >= PAUSE_CEILING_MS) {
        dropped_pause_frames_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const uint64_t cur_state = target_state_.load(std::memory_order_acquire);
    const uint32_t target_pid = static_cast<uint32_t>(cur_state >> 32);
    const uint32_t current_epoch = static_cast<uint32_t>(cur_state & 0xFFFFFFFFULL);

    // Note N-1: Accept-all when target_pid == 0 (waiting for target mode or pure monitor-all)
    if (target_pid != 0 && pid != target_pid) {
        return;
    }

    // Latch session start tick on the first accepted frame of this epoch
    uint64_t expected_tick = 0;
    session_start_tick_ms_.compare_exchange_strong(expected_tick, GetTickCount64(), std::memory_order_release);

    const uint64_t ticket = head_.fetch_add(1, std::memory_order_relaxed);
    auto& slot = ring_[ticket % RING_CAPACITY];

    // D-3: Each producer claims a unique ticket via head_.fetch_add(1). Two producers
    // can only touch the same slot if separated by multiples of RING_CAPACITY (262,144 frames).
    // Slot sequence increments (odd = in-flight, even = published) ensure the reader's
    // seq_before == seq_after seqlock check reliably catches any concurrent overwrite.
    slot.sequence.fetch_add(1, std::memory_order_acq_rel);
    slot.duration_ms = duration_ms;
    slot.timestamp_qpc = timestamp_qpc;
    slot.epoch = current_epoch;
    slot.sequence.fetch_add(1, std::memory_order_release);
}

void SessionBenchmark::ingest_report(const DiagnosticReport& report) {
    std::lock_guard<std::mutex> lock(attribution_mutex_);

    const uint64_t cur_state = target_state_.load(std::memory_order_acquire);
    const uint32_t target_pid = static_cast<uint32_t>(cur_state >> 32);

    if (target_pid != 0 && report.trigger.target_pid != 0 && report.trigger.target_pid != target_pid) {
        return;
    }

    if (target_process_.empty() && !report.target_process.empty()) {
        target_process_ = report.target_process;
    }

    stutters_detected_++;
    const double stall_ms = report.trigger.duration_ms;
    net_stall_ms_ += stall_ms;

    std::string hyp = "unattributed";
    double conf = 0.0;
    std::string driver_mod;

    if (!report.diagnoses.empty()) {
        const auto& d = report.diagnoses[0];
        hyp = d.hypothesis;
        conf = d.confidence;
        for (const auto& ev : d.evidence) {
            if (!ev.driver_module.empty()) {
                driver_mod = ev.driver_module;
                break;
            }
        }
    }

    if (stall_ms > worst_stutter_ms_) {
        worst_stutter_ms_ = stall_ms;
        worst_stutter_hypothesis_ = hyp;
    }

    auto& h_rec = hypothesis_stats_[hyp];
    h_rec.hypothesis = hyp;
    h_rec.count++;
    h_rec.total_stall_ms += stall_ms;
    h_rec.total_confidence += conf;
    if (!driver_mod.empty()) {
        h_rec.driver_counts[driver_mod]++;
    }

    auto& t_rec = tag_stats_[report.attribution];
    t_rec.tag = report.attribution;
    t_rec.count++;
    t_rec.total_stall_ms += stall_ms;
}

BenchmarkSummary SessionBenchmark::get_summary(bool redact) const {
    std::lock_guard<std::mutex> sum_lock(summary_mutex_);

    BenchmarkSummary summary{};
    summary.redacted = redact;

    const PacingProfile snap_profile = pacing_profile_.load(std::memory_order_relaxed);
    const double snap_present_threshold = present_threshold_ms_.load(std::memory_order_relaxed);
    const double snap_spike_mult = spike_multiplier_.load(std::memory_order_relaxed);
    const double snap_min_delta = min_spike_delta_ms_.load(std::memory_order_relaxed);
    const bool snap_has_telemetry = has_live_telemetry_.load(std::memory_order_acquire);
    double snap_live_baseline = 0.0;
    double snap_live_mult = 2.0;
    double snap_live_delta = 4.0;
    if (snap_has_telemetry) {
        snap_live_baseline = live_baseline_ms_.load(std::memory_order_relaxed);
        snap_live_mult = live_effective_mult_.load(std::memory_order_relaxed);
        snap_live_delta = live_effective_delta_.load(std::memory_order_relaxed);
    }

    uint64_t start_head = epoch_start_head_.load(std::memory_order_acquire);
    uint64_t current_head = head_.load(std::memory_order_acquire);

    if (current_head < start_head) {
        start_head = epoch_start_head_.load(std::memory_order_acquire);
        current_head = head_.load(std::memory_order_acquire);
        if (current_head < start_head) {
            const uint64_t cur_state = target_state_.load(std::memory_order_acquire);
            const uint32_t snap_pid = static_cast<uint32_t>(cur_state >> 32);
            BenchmarkSummary empty_summary{};
            empty_summary.redacted = redact;
            empty_summary.is_monitor_all = (snap_pid == 0);
            empty_summary.target_pid = redact ? 0 : snap_pid;
            empty_summary.pacing_profile = snap_profile;
            empty_summary.static_floor_ms = calculate_effective_static_threshold(snap_present_threshold);
            return empty_summary;
        }
    }

    const uint64_t available = current_head - start_head;
    const uint64_t K = std::min<uint64_t>(available, RING_CAPACITY);
    const uint64_t scan_start = current_head - K;

    size_t valid_count = 0;
    // Reading snap_epoch after current_head means it may reflect a newer epoch if a reset raced 
    // with the scan, in which case old-epoch slots are safely excluded by the epoch check (Resolves M-10-3)
    const uint64_t cur_state = target_state_.load(std::memory_order_acquire);
    const uint32_t snap_epoch = static_cast<uint32_t>(cur_state & 0xFFFFFFFFULL);
    const uint32_t snap_pid = static_cast<uint32_t>(cur_state >> 32);

    double sum_dur_ms = 0.0;
    for (uint64_t ticket = scan_start; ticket < current_head; ++ticket) {
        const auto& slot = ring_[ticket % RING_CAPACITY];
        uint64_t seq_before = slot.sequence.load(std::memory_order_acquire);
        if (seq_before & 1) continue; // In-flight write

        double dur = slot.duration_ms;
        uint32_t ep = slot.epoch;
        std::atomic_thread_fence(std::memory_order_acquire);
        uint64_t seq_after = slot.sequence.load(std::memory_order_acquire);

        if (seq_before == seq_after && ep == snap_epoch && dur > 0.0) {
            scratch_buffer_[valid_count++] = dur;
            sum_dur_ms += dur;
        }
    }

    summary.total_frames = valid_count;
    summary.is_monitor_all = (snap_pid == 0);
    summary.target_pid = redact ? 0 : snap_pid;
    summary.pacing_profile = snap_profile;
    summary.static_floor_ms = calculate_effective_static_threshold(snap_present_threshold);
    summary.dropped_pause_frames = dropped_pause_frames_.load(std::memory_order_relaxed);

    // Presentation Cadence State Machine (strictly prior to std::sort)
    if (snap_has_telemetry) {
        // State 1: Live Pacing Telemetry Active
        summary.rolling_baseline_ms = snap_live_baseline;
        summary.rolling_fps = (snap_live_baseline > 0.0) ? (1000.0 / snap_live_baseline) : 0.0;
        summary.estimated_dynamic_floor_ms = std::max(
            snap_live_baseline * snap_live_mult,
            snap_live_baseline + snap_live_delta
        );
        if (summary.estimated_dynamic_floor_ms <= summary.static_floor_ms) {
            summary.estimated_binding_floor_ms = summary.estimated_dynamic_floor_ms;
            summary.binding_floor_source = BindingFloorSource::DYNAMIC;
        } else {
            summary.estimated_binding_floor_ms = summary.static_floor_ms;
            summary.binding_floor_source = BindingFloorSource::STATIC;
        }
    } else if (valid_count >= 8) {
        // State 2: Standalone Fallback (valid_count >= 8)
        const size_t N = std::min<size_t>(valid_count, 64);
        const size_t start_idx = valid_count - N;

        // Pass 1: Sanity Filter (< 100.0 ms)
        std::vector<double> pass1;
        pass1.reserve(N);
        for (size_t i = start_idx; i < valid_count; ++i) {
            if (scratch_buffer_[i] < 100.0) {
                pass1.push_back(scratch_buffer_[i]);
            }
        }

        if (pass1.size() < 8) {
            summary.rolling_baseline_ms = sum_dur_ms / valid_count;
        } else {
            std::sort(pass1.begin(), pass1.end());
            const double median_t1 = (pass1.size() % 2 == 1)
                ? pass1[pass1.size() / 2]
                : (pass1[pass1.size() / 2 - 1] + pass1[pass1.size() / 2]) / 2.0;

            // Pass 2: Median-Referenced Clean Baseline (exclude > 1.4 * median_t1)
            std::vector<double> pass2;
            pass2.reserve(pass1.size());
            const double threshold_pass2 = 1.4 * median_t1;
            for (double d : pass1) {
                if (d <= threshold_pass2) {
                    pass2.push_back(d);
                }
            }

            if (pass2.size() < 8) {
                summary.rolling_baseline_ms = median_t1;
            } else {
                std::sort(pass2.begin(), pass2.end());
                const double median_t2 = (pass2.size() % 2 == 1)
                    ? pass2[pass2.size() / 2]
                    : (pass2[pass2.size() / 2 - 1] + pass2[pass2.size() / 2]) / 2.0;
                summary.rolling_baseline_ms = median_t2;
            }
        }

        summary.rolling_fps = (summary.rolling_baseline_ms > 0.0) ? (1000.0 / summary.rolling_baseline_ms) : 0.0;

        double eff_mult = snap_spike_mult;
        double eff_delta = snap_min_delta;
        if (snap_profile == PacingProfile::AUTO_ADAPTIVE) {
            auto params = compute_adaptive_pacing_params(summary.rolling_baseline_ms);
            eff_mult = params.spike_multiplier;
            eff_delta = params.min_spike_delta_ms;
        } else if (snap_profile == PacingProfile::HIGH_REFRESH) {
            eff_mult = HIGH_REFRESH_SPIKE_MULTIPLIER;
            eff_delta = HIGH_REFRESH_MIN_DELTA_MS;
        } else if (snap_profile == PacingProfile::CONSERVATIVE) {
            eff_mult = CONSERVATIVE_SPIKE_MULTIPLIER;
            eff_delta = CONSERVATIVE_MIN_DELTA_MS;
        }

        summary.estimated_dynamic_floor_ms = std::max(
            summary.rolling_baseline_ms * eff_mult,
            summary.rolling_baseline_ms + eff_delta
        );
        if (summary.estimated_dynamic_floor_ms <= summary.static_floor_ms) {
            summary.estimated_binding_floor_ms = summary.estimated_dynamic_floor_ms;
            summary.binding_floor_source = BindingFloorSource::DYNAMIC;
        } else {
            summary.estimated_binding_floor_ms = summary.static_floor_ms;
            summary.binding_floor_source = BindingFloorSource::STATIC;
        }
    } else {
        // State 3: Warmup / Empty Session / Monitor-All
        summary.rolling_baseline_ms = 0.0;
        summary.rolling_fps = 0.0;
        summary.estimated_dynamic_floor_ms = 0.0;
        summary.estimated_binding_floor_ms = 0.0;
        summary.binding_floor_source = BindingFloorSource::NONE;
    }

    const uint64_t start_tick = session_start_tick_ms_.load(std::memory_order_acquire);
    if (start_tick == 0 || valid_count == 0) {
        summary.duration_ms = 0.0;
    } else {
        const uint64_t now_tick = GetTickCount64();
        summary.duration_ms = (now_tick >= start_tick) ? static_cast<double>(now_tick - start_tick) : 0.0;
    }

    if (valid_count > 0) {
        std::sort(scratch_buffer_.get(), scratch_buffer_.get() + valid_count, std::greater<double>());

        summary.frametimes.max_frametime_ms = scratch_buffer_[0];
        if (sum_dur_ms > 0.0) {
            summary.frametimes.avg_fps = (static_cast<double>(valid_count) * 1000.0) / sum_dur_ms;
        }

        if (valid_count >= 100) {
            const size_t idx_1pct = static_cast<size_t>(valid_count * 0.01) - 1;
            summary.frametimes.p99_frametime_ms = scratch_buffer_[idx_1pct];
            if (summary.frametimes.p99_frametime_ms > 0.0) {
                summary.frametimes.low_1pct_fps = 1000.0 / summary.frametimes.p99_frametime_ms;
            }
        }

        if (valid_count >= 1000) {
            const size_t idx_01pct = static_cast<size_t>(valid_count * 0.001) - 1;
            summary.frametimes.p999_frametime_ms = scratch_buffer_[idx_01pct];
            if (summary.frametimes.p999_frametime_ms > 0.0) {
                summary.frametimes.low_01pct_fps = 1000.0 / summary.frametimes.p999_frametime_ms;
            }
        }
    }

    // Attribution data
    {
        std::lock_guard<std::mutex> attr_lock(attribution_mutex_);
        summary.stutters_detected = stutters_detected_;
        summary.net_stall_ms = net_stall_ms_;
        summary.worst_stutter_ms = worst_stutter_ms_;
        summary.worst_stutter_hypothesis = worst_stutter_hypothesis_;

        if (redact) {
            summary.target_process = "Process_REDACTED";
        } else {
            summary.target_process = target_process_;
        }

        std::vector<HypothesisAttributionStat> all_hyp;
        all_hyp.reserve(hypothesis_stats_.size());

        for (const auto& [name, rec] : hypothesis_stats_) {
            HypothesisAttributionStat stat;
            stat.hypothesis = rec.hypothesis;
            stat.count = rec.count;
            stat.total_stall_ms = rec.total_stall_ms;
            stat.avg_confidence_pct = (rec.count > 0) ? (rec.total_confidence / rec.count * 100.0) : 0.0;
            stat.stall_pct = (net_stall_ms_ > 0.0) ? (rec.total_stall_ms / net_stall_ms_ * 100.0) : 0.0;

            // Find top driver
            std::string top_drv;
            uint32_t max_drv_cnt = 0;
            for (const auto& [drv, cnt] : rec.driver_counts) {
                if (cnt > max_drv_cnt) {
                    max_drv_cnt = cnt;
                    top_drv = drv;
                }
            }
            if (redact) {
                top_drv = get_redacted_module_name(top_drv, true);
            }
            stat.top_driver_module = top_drv;
            stat.top_driver = top_drv;
            all_hyp.push_back(std::move(stat));
        }

        std::sort(all_hyp.begin(), all_hyp.end(), [](const HypothesisAttributionStat& a, const HypothesisAttributionStat& b) {
            if (a.total_stall_ms != b.total_stall_ms) {
                return a.total_stall_ms > b.total_stall_ms;
            }
            return a.count > b.count;
        });

        if (all_hyp.size() <= 5) {
            summary.culprits = std::move(all_hyp);
        } else {
            summary.culprits.assign(all_hyp.begin(), all_hyp.begin() + 5);
            uint32_t other_count = 0;
            double other_stall = 0.0;
            double other_conf_sum = 0.0;
            for (size_t i = 5; i < all_hyp.size(); ++i) {
                other_count += all_hyp[i].count;
                other_stall += all_hyp[i].total_stall_ms;
                other_conf_sum += all_hyp[i].avg_confidence_pct * all_hyp[i].count;
            }
            HypothesisAttributionStat other;
            other.hypothesis = "Other";
            other.top_driver_module = "";
            other.top_driver = "";
            other.count = other_count;
            other.total_stall_ms = other_stall;
            other.avg_confidence_pct = (other_count > 0) ? (other_conf_sum / other_count) : 0.0;
            other.stall_pct = (net_stall_ms_ > 0.0) ? (other_stall / net_stall_ms_ * 100.0) : 0.0;
            summary.culprits.push_back(std::move(other));
        }

        for (const auto& [tag, tstat] : tag_stats_) {
            TagAttributionStat ts;
            ts.tag = tag;
            ts.count = tstat.count;
            ts.total_stall_ms = tstat.total_stall_ms;
            ts.stall_pct = (net_stall_ms_ > 0.0) ? (tstat.total_stall_ms / net_stall_ms_ * 100.0) : 0.0;
            summary.tag_stats.push_back(ts);
        }

        std::sort(summary.tag_stats.begin(), summary.tag_stats.end(), [](const TagAttributionStat& a, const TagAttributionStat& b) {
            if (a.total_stall_ms != b.total_stall_ms) {
                return a.total_stall_ms > b.total_stall_ms;
            }
            return a.count > b.count;
        });
    }

    return summary;
}

void SessionBenchmark::clear_attribution_locked() {
    hypothesis_stats_.clear();
    tag_stats_.clear();
    stutters_detected_ = 0;
    net_stall_ms_ = 0.0;
    worst_stutter_ms_ = 0.0;
    worst_stutter_hypothesis_.clear();
    target_process_.clear();
}

void SessionBenchmark::set_pacing_context(
    PacingProfile profile,
    double present_threshold_ms,
    double spike_multiplier,
    double min_spike_delta_ms
) noexcept {
    pacing_profile_.store(profile, std::memory_order_relaxed);
    present_threshold_ms_.store(present_threshold_ms, std::memory_order_relaxed);
    spike_multiplier_.store(spike_multiplier, std::memory_order_relaxed);
    min_spike_delta_ms_.store(min_spike_delta_ms, std::memory_order_relaxed);
}

void SessionBenchmark::update_pacing_telemetry(
    double baseline_ms,
    double effective_mult,
    double effective_delta
) noexcept {
    live_baseline_ms_.store(baseline_ms, std::memory_order_relaxed);
    live_effective_mult_.store(effective_mult, std::memory_order_relaxed);
    live_effective_delta_.store(effective_delta, std::memory_order_relaxed);
    has_live_telemetry_.store(true, std::memory_order_release);
}

void SessionBenchmark::retarget(uint32_t new_pid) {
    std::lock_guard<std::mutex> sum_lock(summary_mutex_);
    std::lock_guard<std::mutex> attr_lock(attribution_mutex_);

    const uint64_t cur_state = target_state_.load(std::memory_order_acquire);
    const uint32_t cur_epoch = static_cast<uint32_t>(cur_state & 0xFFFFFFFFULL);
    const uint32_t next_epoch = cur_epoch + 1;
    const uint64_t new_state = (static_cast<uint64_t>(new_pid) << 32) | next_epoch;

    clear_attribution_locked();
    has_live_telemetry_.store(false, std::memory_order_release);
    dropped_pause_frames_.store(0, std::memory_order_relaxed);
    epoch_start_head_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
    target_state_.store(new_state, std::memory_order_release);
    session_start_tick_ms_.store(0, std::memory_order_release);
}

void SessionBenchmark::reset() {
    std::lock_guard<std::mutex> sum_lock(summary_mutex_);
    std::lock_guard<std::mutex> attr_lock(attribution_mutex_);

    const uint64_t cur_state = target_state_.load(std::memory_order_acquire);
    const uint32_t cur_pid = static_cast<uint32_t>(cur_state >> 32);
    const uint32_t cur_epoch = static_cast<uint32_t>(cur_state & 0xFFFFFFFFULL);
    const uint32_t next_epoch = cur_epoch + 1;
    const uint64_t new_state = (static_cast<uint64_t>(cur_pid) << 32) | next_epoch;

    clear_attribution_locked();
    has_live_telemetry_.store(false, std::memory_order_release);
    dropped_pause_frames_.store(0, std::memory_order_relaxed);
    // D-2: Updating epoch_start_head_ before target_state_ guarantees that any producer
    // reading the old epoch and claiming a ticket >= epoch_start_head_ publishes with the old
    // epoch and is safely excluded by the reader's epoch filter. Both reset() and retarget()
    // are serialized under summary_mutex_, eliminating any concurrent reset races.
    epoch_start_head_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
    target_state_.store(new_state, std::memory_order_release);
    session_start_tick_ms_.store(0, std::memory_order_release);
}

} // namespace stuttometer
