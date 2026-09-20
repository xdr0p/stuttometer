#pragma once

#include <cstdint>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include "constants.hpp"
#include "correlator.hpp"
#include "privilege_utils.hpp"
#include "frame_pacing_tracker.hpp"

namespace stuttometer {

enum class BindingFloorSource : uint8_t {
    NONE    = 0,
    DYNAMIC = 1,
    STATIC  = 2
};

inline std::string_view binding_floor_source_to_string(BindingFloorSource src) noexcept {
    switch (src) {
        case BindingFloorSource::DYNAMIC: return "dynamic";
        case BindingFloorSource::STATIC:  return "static";
        case BindingFloorSource::NONE:
        default:                          return "none";
    }
}

// 64-byte cache-line aligned seqlock slot
struct alignas(64) FrameSlot {
    std::atomic<uint64_t> sequence{0}; // 8 bytes (offset 0..7)  - seqlock (even = published, odd = writing)
    double duration_ms{0.0};           // 8 bytes (offset 8..15) - naturally 8-byte aligned
    uint64_t timestamp_qpc{0};         // 8 bytes (offset 16..23)
    uint32_t epoch{0};                 // 4 bytes (offset 24..27) - plain uint32_t (guarded by seqlock)
    uint8_t _pad[36]{0};               // 36 bytes (offset 28..63) - explicit padding to 64 bytes
};
static_assert(sizeof(FrameSlot) == 64, "FrameSlot must be strictly 64 bytes (1 cache line)");

struct FrametimePercentiles {
    double avg_fps{0.0};
    double p99_frametime_ms{0.0};
    double low_1pct_fps{0.0};
    double p999_frametime_ms{0.0};
    double low_01pct_fps{0.0};
    double max_frametime_ms{0.0};
};

struct HypothesisAttributionStat {
    std::string hypothesis;
    std::string top_driver_module;
    std::string top_driver; // backward-compatibility alias for top_driver_module
    uint32_t count{0};
    double total_stall_ms{0.0};
    double avg_confidence_pct{0.0};
    double stall_pct{0.0};
};

struct TagAttributionStat {
    AttributionTag tag{AttributionTag::UNKNOWN};
    uint32_t count{0};
    double total_stall_ms{0.0};
    double stall_pct{0.0};
};

struct BenchmarkSummary {
    std::string target_process;
    uint32_t target_pid{0};
    double duration_ms{0.0};
    uint64_t total_frames{0};
    uint64_t stutters_detected{0};
    uint64_t audio_glitches_detected{0};
    uint64_t dropped_pause_frames{0};
    FrametimePercentiles frametimes;
    std::vector<HypothesisAttributionStat> culprits; // Top 5 + "Other"
    std::vector<TagAttributionStat> tag_stats;
    double net_stall_ms{0.0};
    double worst_stutter_ms{0.0};
    std::string worst_stutter_hypothesis;
    bool redacted{false};

    // is_monitor_all: true when no target process is configured. In the current GUI,
    // this is equivalent to (target_pid == 0) because waiting-by-name mode is CLI-only.
    // Revisit if GUI waiting mode is added in the future.
    bool is_monitor_all{false};
    PacingProfile pacing_profile{PacingProfile::AUTO_ADAPTIVE};
    double rolling_baseline_ms{0.0};
    double rolling_fps{0.0};
    double estimated_dynamic_floor_ms{0.0};
    double static_floor_ms{0.0};
    double estimated_binding_floor_ms{0.0};
    BindingFloorSource binding_floor_source{BindingFloorSource::NONE};

    std::string to_json() const;
    std::string to_markdown() const;
};

class SessionBenchmark {
public:
    static constexpr size_t RING_CAPACITY = 262144;

    explicit SessionBenchmark(uint64_t qpc_freq);
    ~SessionBenchmark() = default;

    // Non-copyable, non-movable
    SessionBenchmark(const SessionBenchmark&) = delete;
    SessionBenchmark& operator=(const SessionBenchmark&) = delete;
    SessionBenchmark(SessionBenchmark&&) = delete;
    SessionBenchmark& operator=(SessionBenchmark&&) = delete;

    void ingest_frame(uint32_t pid, double duration_ms, uint64_t timestamp_qpc) noexcept;
    void ingest_report(const DiagnosticReport& report);

    void set_pacing_context(
        PacingProfile profile,
        double present_threshold_ms,
        double spike_multiplier,
        double min_spike_delta_ms
    ) noexcept;

    void update_pacing_telemetry(
        double baseline_ms,
        double effective_mult,
        double effective_delta
    ) noexcept;

    BenchmarkSummary get_summary(bool redact = false) const;
    void reset();
    void retarget(uint32_t new_pid);

    uint64_t dropped_pause_frames() const noexcept {
        return dropped_pause_frames_.load(std::memory_order_relaxed);
    }

private:
    void clear_attribution_locked();

    // Member declaration order matching constructor initializer list
    const uint64_t qpc_freq_;
    const uint64_t pause_ceiling_qpc_;
    const std::unique_ptr<double[]> scratch_buffer_;

    // Ring buffer
    const std::unique_ptr<FrameSlot[]> ring_;

    // Lock-free ring tracking & epoch
    std::atomic<uint64_t> head_{0};
    std::atomic<uint64_t> epoch_start_head_{0};
    // Indivisible 64-bit target state: High 32 bits = target_pid, Low 32 bits = epoch
    std::atomic<uint64_t> target_state_{0};
    // Note N-2: Uses GetTickCount64() with ~10-16ms resolution, sufficient for MM:SS display
    std::atomic<uint64_t> session_start_tick_ms_{0};
    std::atomic<uint64_t> dropped_pause_frames_{0};

    // Pacing context & live telemetry
    std::atomic<PacingProfile> pacing_profile_{PacingProfile::AUTO_ADAPTIVE};
    std::atomic<double> present_threshold_ms_{16.67};
    std::atomic<double> spike_multiplier_{2.0};
    std::atomic<double> min_spike_delta_ms_{4.0};
    std::atomic<bool> has_live_telemetry_{false};
    std::atomic<double> live_baseline_ms_{0.0};
    std::atomic<double> live_effective_mult_{2.0};
    std::atomic<double> live_effective_delta_{4.0};

    // Concurrency control:
    // summary_mutex_ guards get_summary execution and scratch_buffer_
    // attribution_mutex_ guards cumulative culprit statistics
    mutable std::mutex summary_mutex_;
    mutable std::mutex attribution_mutex_;

    // Attribution stats (guarded by attribution_mutex_)
    struct HypothesisRecord {
        std::string hypothesis;
        uint32_t count{0};
        double total_stall_ms{0.0};
        double total_confidence{0.0};
        std::unordered_map<std::string, uint32_t> driver_counts;
    };
    std::unordered_map<std::string, HypothesisRecord> hypothesis_stats_;
    std::unordered_map<AttributionTag, TagAttributionStat> tag_stats_;
    uint64_t stutters_detected_{0};
    uint64_t audio_glitches_detected_{0};
    double net_stall_ms_{0.0};
    double worst_stutter_ms_{0.0};
    std::string worst_stutter_hypothesis_;
    std::string target_process_;
};

} // namespace stuttometer
