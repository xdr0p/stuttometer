#include <algorithm>
#include <cstring>
#include "stuttometer/etw_session.hpp"
#include "stuttometer/ndjson_writer.hpp"
#include "stuttometer/privilege_utils.hpp"

namespace stuttometer {

void EtwSessionManager::handle_dxgi_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    rec.category = static_cast<uint16_t>(EventCategory::DXGI);

    uint64_t thread_key = make_thread_key(ctx.pid, ctx.tid);

    if (ctx.event_id == 42 || ctx.event_id == 55) { // Present Start / PresentMultiplaneOverlay Start
        uint64_t swapchain_ptr = 0;
        if (p_event->UserDataLength >= 8 && p_event->UserData) {
            std::memcpy(&swapchain_ptr, p_event->UserData, sizeof(uint64_t));
        }
        rec.auxiliary_data = swapchain_ptr;
        in_flight_present_.insert(thread_key, { ctx.timestamp, swapchain_ptr, ctx.pid, ctx.tid });
    } else if (ctx.event_id == 43 || ctx.event_id == 56) { // Present Stop / PresentMultiplaneOverlay Stop
        rec.auxiliary_data = 0; // Preserve NDJSON schema v1 contract for Present Stop

        PresentInFlight present_data{};
        bool has_in_flight = in_flight_present_.find_and_erase(thread_key, present_data);

        // Always push Event 43 to flight recorder and ndjson writer (N-2)
        // If orphaned (no matching Event 42), fallback skips inter-frame calculation to prevent baseline pollution
        if (has_in_flight && present_data.pid == ctx.pid) {
            uint64_t start_qpc = present_data.start_qpc;
            uint64_t swapchain_ptr = present_data.swapchain_ptr;
            uint64_t swapchain_key = make_swapchain_key(ctx.pid, swapchain_ptr);

            LastPresentEntry last_entry{};
            bool has_prev = last_present_table_.lookup(swapchain_key, last_entry);
            uint64_t prev_qpc = has_prev ? last_entry.last_present_qpc : 0;

            PresentDeltaResult delta_res = calculate_effective_present_duration(
                ctx.timestamp, prev_qpc, start_qpc, qpc_freq_, 10000000ULL
            );

            last_present_table_.insert(swapchain_key, { ctx.timestamp, ctx.pid, ctx.tid });

            uint32_t clamped_dur_us = static_cast<uint32_t>(std::min(delta_res.effective_dur_us, 10000000ULL));
            rec.duration_us = clamped_dur_us;
            flight_recorder_.push(rec);

            fprintf(stderr, "[D-A] has_inflight=%d pid_match=%d reset=%d dur_us=%llu state=%d suppressed=%llu ev_qpc=%llu now_qpc=%llu\n",
                    (int)has_in_flight,
                    (int)(present_data.pid == ctx.pid),
                    (int)delta_res.is_baseline_reset,
                    (unsigned long long)delta_res.effective_dur_us,
                    (int)trigger_engine_.current_state(),
                    (unsigned long long)trigger_engine_.suppressed_trigger_count(),
                    (unsigned long long)ctx.timestamp,
                    (unsigned long long)get_current_qpc());

            if (!delta_res.is_baseline_reset && delta_res.effective_dur_us > 0) {
                double dur_ms = delta_res.effective_dur_us / 1000.0;
                trigger_engine_.on_dxgi_present(ctx.pid, ctx.tid, dur_ms, ctx.timestamp, swapchain_key, ctx.cpu);
            }
        } else {
            fprintf(stderr, "[D-A-ORPHAN] has_inflight=%d pid_match=%d\n",
                    (int)has_in_flight, (int)(present_data.pid == ctx.pid));
            flight_recorder_.push(rec);
        }
    }

    NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
    if (writer) writer->push(rec);
}

void EtwSessionManager::handle_d3d12_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    const auto& desc = p_event->EventHeader.EventDescriptor;
    const uint16_t task = desc.Task;
    const uint8_t op = desc.Opcode;

    // Note: Non-PSO D3D12 events (draw calls, resource bindings, etc.) are intentionally filtered to preserve high signal-to-noise ratio in the categorized NDJSON stream.
    const bool is_pso_task = (task == 29 || task == 66 || task == 67 || ctx.event_id == 63 || ctx.event_id == 64 || ctx.event_id == 155 || ctx.event_id == 156 || ctx.event_id == 157 || ctx.event_id == 158);

    if (is_pso_task) {
        rec.category = static_cast<uint16_t>(EventCategory::D3D12_PSO_CREATE);

        uint64_t pso_ptr = 0;
        if (p_event->UserDataLength >= sizeof(uint64_t) && p_event->UserData) {
            if (p_event->UserDataLength >= 16) {
                std::memcpy(&pso_ptr, static_cast<const uint8_t*>(p_event->UserData) + 8, sizeof(uint64_t));
            } else {
                std::memcpy(&pso_ptr, p_event->UserData, sizeof(uint64_t));
            }
        }

        uint64_t pso_key = 0;
        const auto& act = p_event->EventHeader.ActivityId;
        if (!IsEqualGUID(act, GUID_NULL)) {
            pso_key = activity_id_to_key(act);
        }
        if (pso_key == 0 || pso_key == 1ULL) {
            pso_key = make_pso_key(ctx.tid, pso_ptr);
        }

        uint16_t flags = EventFlags::NONE;
        if (task == 29 || ctx.event_id == 63 || ctx.event_id == 64) {
            flags |= EventFlags::D3D12_GRAPHICS_PSO;
        } else if (task == 67 || ctx.event_id == 157 || ctx.event_id == 158) {
            flags |= EventFlags::D3D12_COMPUTE_PSO;
        }

        if (op == 1 || ctx.event_id == 63 || ctx.event_id == 155 || ctx.event_id == 157) { // win:Start
            in_flight_pso_table_.insert(pso_key, { ctx.timestamp, ctx.pid, ctx.tid, pso_ptr, flags });
            rec.flags = flags;
            rec.auxiliary_data = pso_ptr;
        } else if (op == 2 || ctx.event_id == 64 || ctx.event_id == 156 || ctx.event_id == 158) { // win:Stop
            PsoInFlight pso_data{};
            if (in_flight_pso_table_.find_and_erase(pso_key, pso_data)) {
                if (ctx.timestamp >= pso_data.start_qpc) {
                    const uint64_t delta_us = static_cast<uint64_t>(qpc_delta_to_us(ctx.timestamp - pso_data.start_qpc, qpc_freq_));
                    if (delta_us <= 10000000ULL) {
                        rec.duration_us = static_cast<uint32_t>(delta_us);
                    }
                }
                if (flags == EventFlags::NONE) {
                    flags = pso_data.flags;
                }
                if (pso_ptr == 0) {
                    pso_ptr = pso_data.pso_ptr;
                }
            }
            rec.flags = flags;
            rec.auxiliary_data = pso_ptr;
            if (rec.duration_us > 0) {
                flight_recorder_.push(rec);
            }
        } else {
            rec.flags = flags;
            rec.auxiliary_data = pso_ptr;
        }

        NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
        if (writer) writer->push(rec);
    }
}

} // namespace stuttometer
