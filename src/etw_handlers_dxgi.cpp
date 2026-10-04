#include <algorithm>
#include <cstring>
#include "stuttometer/etw_session.hpp"
#include "stuttometer/privilege_utils.hpp"

namespace stuttometer {

void EtwSessionManager::handle_dxgi_event(
    PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx
) noexcept {
    rec.category = static_cast<uint16_t>(EventCategory::DXGI);
    const uint64_t thread_key = make_thread_key(ctx.pid, ctx.tid);

    if (ctx.event_id == 42 || ctx.event_id == 55) { // Present Start / PresentMultiplaneOverlay Start
        uint64_t swapchain_ptr = 0;
        if (p_event->UserDataLength >= 8 && p_event->UserData) {
            std::memcpy(&swapchain_ptr, p_event->UserData, sizeof(uint64_t));
        }
        rec.auxiliary_data = swapchain_ptr;
        in_flight_present_.insert(thread_key, { ctx.timestamp, swapchain_ptr, ctx.pid, ctx.tid });
        emit_ndjson_only(rec);
        return;
    }

    if (ctx.event_id == 43 || ctx.event_id == 56) { // Present Stop / PresentMultiplaneOverlay Stop
        rec.auxiliary_data = 0;
        PresentInFlight present_data{};
        const bool has_in_flight = in_flight_present_.find_and_erase(thread_key, present_data);

        // Always push Event 43/56 to flight recorder and ndjson writer (N-2)
        // If orphaned (no matching Event 42/55), fallback skips inter-frame calculation to prevent baseline pollution
        if (has_in_flight && present_data.pid == ctx.pid) {
            const uint64_t start_qpc = present_data.start_qpc;
            const uint64_t swapchain_ptr = present_data.swapchain_ptr;
            const uint64_t swapchain_key = make_swapchain_key(ctx.pid, swapchain_ptr);

            LastPresentEntry last_entry{};
            const bool has_prev = last_present_table_.lookup(swapchain_key, last_entry);
            const uint64_t prev_qpc = has_prev ? last_entry.last_present_qpc : 0;

            const PresentDeltaResult delta_res = calculate_effective_present_duration(
                ctx.timestamp,
                prev_qpc,
                start_qpc,
                qpc_freq_,
                PAUSE_CEILING_US,
                trigger_engine_.vblank_interval_ms()
            );

            // Tag duplicate present path artifacts before pushing so the flight recorder and NDJSON
            // stream carry the flag for data fidelity and the correlator can filter them from the
            // exported frame timeline. The canonical swapchain boundary is preserved in
            // last_present_table_ regardless (unconditional insert below).
            if (delta_res.is_duplicate_present_path) {
                rec.flags |= EventFlags::DXGI_DUPLICATE_PRESENT_PATH;
            }

            last_present_table_.insert(swapchain_key, { ctx.timestamp, ctx.pid, ctx.tid });

            // Emit cap: inter-frame semantics are bounded by the pause ceiling. A Stop
            // whose effective duration exceeds the pause ceiling is either a baseline
            // reset or a stale pairing; emit the event for raw-stream data fidelity but
            // bound the reported duration at the pause ceiling. The trigger path handles
            // baseline-reset exclusion independently below.
            const uint64_t emit_dur_us =
                std::min(delta_res.effective_dur_us, PAUSE_CEILING_US);
            rec.duration_us = static_cast<uint32_t>(emit_dur_us);
            emit_event(rec);

            // Guard pacing ingestion: duplicates must not pollute the rolling baseline, SessionBenchmark,
            // or trigger reports. The flight recorder push and NDJSON write above are unconditional so
            // that all Stop events remain observable in the raw stream.
            // NOTE: In-flight pairing assumes 42→43→55→56 ordering. If a title emits 42→55→43→56,
            // the 43-Stop pairs with the 55-Start (different thread keys), yielding a slightly wrong
            // API duration for rec.duration_us on event 43 (cosmetic only). The dedup still works
            // correctly because it operates on last_present_table_ inter-Stop deltas, not on API
            // durations. This ordering ambiguity is documented here and intentionally not fixed
            // (out of scope per design decision; see implementation plan Section 8).
            if (!delta_res.is_baseline_reset && delta_res.effective_dur_us > 0 &&
                !delta_res.is_duplicate_present_path) {
                const double dur_ms = delta_res.effective_dur_us / 1000.0;
                trigger_engine_.on_dxgi_present(
                    ctx.pid, ctx.tid, dur_ms, ctx.timestamp, swapchain_key, ctx.cpu);
            }
        } else {
            emit_event(rec);
        }
        return;
    }

    emit_ndjson_only(rec);
}

void EtwSessionManager::handle_d3d12_event(
    PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx
) noexcept {
    const auto& desc = p_event->EventHeader.EventDescriptor;
    const uint16_t task = desc.Task;
    const uint8_t op = desc.Opcode;

    const bool is_pso_task = (task == 29 || task == 66 || task == 67 ||
                              ctx.event_id == 63 || ctx.event_id == 64 ||
                              ctx.event_id == 155 || ctx.event_id == 156 ||
                              ctx.event_id == 157 || ctx.event_id == 158);
    if (!is_pso_task) return;

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
        emit_ndjson_only(rec);
        return;
    }

    if (op == 2 || ctx.event_id == 64 || ctx.event_id == 156 || ctx.event_id == 158) { // win:Stop
        PsoInFlight pso_data{};
        const bool found = in_flight_pso_table_.find_and_erase(pso_key, pso_data);
        if (found) {
            rec.duration_us = clamped_qpc_delta_us(
                ctx.timestamp, pso_data.start_qpc, qpc_freq_, KERNEL_SINGLE_EVENT_CAP_US);
            if (flags == EventFlags::NONE) flags = pso_data.flags;
            if (pso_ptr == 0) pso_ptr = pso_data.pso_ptr;
        }
        rec.flags = flags;
        rec.auxiliary_data = pso_ptr;
        const bool data_ok = found && (rec.duration_us > 0);
        emit_event(rec, data_ok);
        return;
    }

    rec.flags = flags;
    rec.auxiliary_data = pso_ptr;
    emit_ndjson_only(rec);
}

} // namespace stuttometer
