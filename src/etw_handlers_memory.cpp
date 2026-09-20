#include <cstring>
#include "stuttometer/etw_session.hpp"
#include "stuttometer/ndjson_writer.hpp"
#include "stuttometer/privilege_utils.hpp"

namespace stuttometer {

void EtwSessionManager::handle_kernel_memory_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    if (ctx.event_id == 4) { // WorkingSetOutSwap Start
        rec.category = static_cast<uint16_t>(EventCategory::MEM_WORKING_SET_TRIM);

        if (p_event->UserDataLength >= 4 && p_event->UserData) {
            uint32_t target_proc = 0;
            std::memcpy(&target_proc, p_event->UserData, sizeof(uint32_t));
            if (target_proc != 0 && target_proc != 4) {
                WorkingSetTrimInFlight trim_entry{};
                trim_entry.start_qpc = ctx.timestamp;
                trim_entry.pid = target_proc;
                // WorkingSetOutSwap Start/Stop is keyed on target PID only.
                // NT memory management serializes trims per-process under Vm.WorkingSetMutex
                // (one working set per process), whereas ETW Start and Stop can be emitted on
                // different kernel worker threads or with TID 0.
                // A duplicate Start silently overwrites the prior in-flight entry (see
                // FixedInFlightTable::insert); the subsequent Stop correlates against the newer Start.
                const uint64_t trim_key = static_cast<uint64_t>(target_proc);
                in_flight_ws_trims_.insert(trim_key, trim_entry);
            }
        }

        NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
        if (writer) writer->push(rec);
    } else if (ctx.event_id == 5) { // WorkingSetOutSwap Stop
        rec.category = static_cast<uint16_t>(EventCategory::MEM_WORKING_SET_TRIM);

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
            // WorkingSetOutSwap Start/Stop is keyed on target PID only.
            // NT memory management serializes trims per-process under Vm.WorkingSetMutex
            // (one working set per process), whereas ETW Start and Stop can be emitted on
            // different kernel worker threads or with TID 0.
            // A duplicate Start silently overwrites the prior in-flight entry (see
            // FixedInFlightTable::insert); the subsequent Stop correlates against the newer Start.
            const uint64_t trim_key = static_cast<uint64_t>(target_proc);
            if (target_proc != 0 && target_proc != 4 && in_flight_ws_trims_.find_and_erase(trim_key, start_entry)) {
                if (ctx.timestamp >= start_entry.start_qpc) {
                    const uint64_t delta_us = static_cast<uint64_t>(qpc_delta_to_us(ctx.timestamp - start_entry.start_qpc, qpc_freq_));
                    if (delta_us <= 10000000ULL) {
                        rec.duration_us = static_cast<uint32_t>(delta_us);
                    }
                }
            }
            flight_recorder_.push(rec);
        }

        NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
        if (writer) writer->push(rec);
    } else if (ctx.event_id == 10 || ctx.event_id == 11) { // MdlAllocation (10) or ContAllocation (11)
        rec.category = static_cast<uint16_t>(EventCategory::MEM_PHYSICAL_ALLOC);

        if (p_event->UserDataLength >= 16 && p_event->UserData) {
            const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
            uint64_t dur_us = 0;
            uint64_t total_bytes = 0;
            std::memcpy(&dur_us, raw + 0, sizeof(uint64_t));
            std::memcpy(&total_bytes, raw + 8, sizeof(uint64_t));

            if (dur_us <= 10000000ULL) {
                rec.duration_us = static_cast<uint32_t>(dur_us);
            }
            rec.auxiliary_data = total_bytes;
            rec.flags = EventFlags::MEM_PHYSICAL_CONTIGUOUS;
            flight_recorder_.push(rec);
        }

        NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
        if (writer) writer->push(rec);
    }
}

} // namespace stuttometer
