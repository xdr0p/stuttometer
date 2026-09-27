#include <cstring>
#include "stuttometer/etw_session.hpp"
#include "stuttometer/privilege_utils.hpp"

namespace stuttometer {

namespace {
[[nodiscard]] constexpr bool valid_trim_target(uint32_t pid) noexcept {
    return pid != 0 && pid != 4; // 0 = System Idle, 4 = System
}
} // namespace

void EtwSessionManager::handle_kernel_memory_event(
    PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx
) noexcept {
    if (ctx.event_id == 4) { // WorkingSetOutSwap Start
        rec.category = static_cast<uint16_t>(EventCategory::MEM_WORKING_SET_TRIM);
        if (p_event->UserDataLength >= 4 && p_event->UserData) {
            uint32_t target_proc = 0;
            std::memcpy(&target_proc, p_event->UserData, sizeof(uint32_t));
            if (valid_trim_target(target_proc)) {
                // PID-keyed: NT memory management serializes trims per-process under
                // Vm.WorkingSetMutex (one working set per process), whereas ETW Start
                // and Stop can be emitted on different kernel worker threads or with
                // TID 0. A duplicate Start silently overwrites the prior in-flight entry.
                WorkingSetTrimInFlight entry{};
                entry.start_qpc = ctx.timestamp;
                entry.pid = target_proc;
                in_flight_ws_trims_.insert(static_cast<uint64_t>(target_proc), entry);
            }
        }
        emit_ndjson_only(rec);
        return;
    }
    if (ctx.event_id == 5) { // WorkingSetOutSwap Stop
        rec.category = static_cast<uint16_t>(EventCategory::MEM_WORKING_SET_TRIM);
        bool data_ok = false;
        if (p_event->UserDataLength >= 16 && p_event->UserData) {
            const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
            uint32_t target_proc = 0;
            uint64_t pages_processed = 0;
            std::memcpy(&target_proc, raw + 0, sizeof(uint32_t));
            std::memcpy(&pages_processed, raw + 8, sizeof(uint64_t));

            rec.pid = target_proc;
            rec.auxiliary_data = pages_processed * 4096ULL;
            rec.flags = EventFlags::MEM_WS_TRIM_OUTSWAP;

            WorkingSetTrimInFlight start_entry{};
            if (valid_trim_target(target_proc) &&
                in_flight_ws_trims_.find_and_erase(
                    static_cast<uint64_t>(target_proc), start_entry)) {
                rec.duration_us = clamped_qpc_delta_us(
                    ctx.timestamp, start_entry.start_qpc, qpc_freq_,
                    KERNEL_SINGLE_EVENT_CAP_US);
            }
            data_ok = true;
        }
        emit_event(rec, data_ok);
        return;
    }
    if (ctx.event_id == 10 || ctx.event_id == 11) { // Mdl / Cont Allocation
        rec.category = static_cast<uint16_t>(EventCategory::MEM_PHYSICAL_ALLOC);
        bool data_ok = false;
        if (p_event->UserDataLength >= 16 && p_event->UserData) {
            const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
            uint64_t dur_us = 0;
            uint64_t total_bytes = 0;
            std::memcpy(&dur_us, raw + 0, sizeof(uint64_t));
            std::memcpy(&total_bytes, raw + 8, sizeof(uint64_t));

            // Payload-supplied duration (NOT a QPC delta). Cap applies to raw field.
            rec.duration_us = (dur_us <= KERNEL_SINGLE_EVENT_CAP_US)
                ? static_cast<uint32_t>(dur_us) : 0U;
            rec.auxiliary_data = total_bytes;
            rec.flags = EventFlags::MEM_PHYSICAL_CONTIGUOUS;
            data_ok = true;
        }
        emit_event(rec, data_ok);
        return;
    }
}

} // namespace stuttometer
