#include <algorithm>
#include <cstring>
#include "stuttometer/constants.hpp"
#include "stuttometer/internal/dxgkrnl_layout.hpp"
#include "stuttometer/etw_session.hpp"
#include "stuttometer/privilege_utils.hpp"

namespace stuttometer {

void EtwSessionManager::handle_dxgkrnl_flip_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    rec.category = static_cast<uint16_t>(EventCategory::DXGKRNL_MMIOFLIP);

    uint32_t vidpn_source_id = 0;
    uint64_t allocation_ptr = 0;
    uint32_t flip_present_id = 0;

    // Verified offsets for Task 17 (Event 116: MMIOFlip):
    // Offset 8 (4B): VidPnSourceId, Offset 16 (8B): FlipToDriverAllocation, Offset 36 (4B): FlipPresentId
    if (p_event->UserDataLength >= dxgkrnl_layout::task17::MIN_PAYLOAD_LEN_BASIC && p_event->UserData) {
        const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
        std::memcpy(&vidpn_source_id, raw + dxgkrnl_layout::task17::OFFSET_VIDPN_SOURCE_ID, sizeof(uint32_t));
        std::memcpy(&allocation_ptr, raw + dxgkrnl_layout::task17::OFFSET_FLIP_TO_DRIVER_ALLOCATION, sizeof(uint64_t));
        if (p_event->UserDataLength >= dxgkrnl_layout::task17::MIN_PAYLOAD_LEN_EXTENDED) {
            std::memcpy(&flip_present_id, raw + dxgkrnl_layout::task17::OFFSET_FLIP_PRESENT_ID, sizeof(uint32_t));
        }
    }

    rec.auxiliary_data = allocation_ptr;
    rec.payload.dxgi.present_flags = vidpn_source_id;
    rec.payload.dxgi.frame_index = flip_present_id;

    const uint64_t flip_key = make_flip_key(vidpn_source_id, allocation_ptr);
    LastFlipEntry last_entry{};
    bool has_prev = last_flip_table_.lookup(flip_key, last_entry);
    uint64_t prev_qpc = has_prev ? last_entry.last_flip_qpc : 0;

    double delivery_ms = 0.0;
    bool is_baseline = true;

    if (has_prev && prev_qpc > 0 && ctx.timestamp > prev_qpc) {
        uint64_t delta_qpc = ctx.timestamp - prev_qpc;
        double delta_us = qpc_delta_to_us(delta_qpc, qpc_freq_);
        if (delta_us <= static_cast<double>(PAUSE_CEILING_US)) { // 2.0s ceiling for loading / Alt-Tab
            delivery_ms = delta_us / 1000.0;
            is_baseline = false;
            rec.duration_us = static_cast<uint32_t>(delta_us);
        }
    }

    last_flip_table_.insert(flip_key, { ctx.timestamp, static_cast<uint32_t>(allocation_ptr & 0xFFFFFFFF), ctx.pid, ctx.tid });

    emit_event(rec);

    // DWM/System Filtering (S1, N-3):
    // In DWM-composed borderless mode, flips originate from DWM/System (PID 4) and are filtered here;
    // DXGI_PRESENT_STUTTER and DWM_GLITCH handle composed presentation.
    const uint32_t dwm_pid = cached_dwm_pid_.load(std::memory_order_relaxed);
    if (ctx.pid == 4 || (dwm_pid != 0 && ctx.pid == dwm_pid)) {
        return;
    }

    if (!is_baseline && delivery_ms > 0.0) {
        trigger_engine_.on_kernel_frame_stall(ctx.pid, ctx.tid, delivery_ms, ctx.timestamp, flip_key, ctx.cpu);
    }
}

void EtwSessionManager::handle_dxgkrnl_vsync_event(PEVENT_RECORD /*p_event*/, EtwEventRecord& rec, const EventContext& /*ctx*/) noexcept {
    rec.category = static_cast<uint16_t>(EventCategory::DXGKRNL_VSYNCDPC);
    emit_event(rec);
}

void EtwSessionManager::handle_dxgkrnl_vidmm_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    rec.category = static_cast<uint16_t>(EventCategory::DXGKRNL_VRAM_PAGING);
    const uint16_t task = p_event->EventHeader.EventDescriptor.Task;
    if (ctx.event_id == 370 || task == 222) {
        bool data_ok = false;
        if (p_event->UserDataLength >= 28 && p_event->UserData) {
            uint64_t commitment = 0;
            uint64_t old_commitment = 0;
            uint32_t process_id = 0;
            const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
            std::memcpy(&commitment, raw + 0, sizeof(uint64_t));
            std::memcpy(&old_commitment, raw + 8, sizeof(uint64_t));
            std::memcpy(&process_id, raw + 24, sizeof(uint32_t));

            if (commitment > 0) {
                rec.pid = (process_id != 0) ? process_id : ctx.pid;
                rec.auxiliary_data = commitment;
                rec.flags = EventFlags::VRAM_DEMOTED_COMMITMENT;
                data_ok = true;
            }
        }
        emit_event(rec, data_ok);
        return;
    }
    if (ctx.event_id == 367 || task == 219) {
        bool data_ok = false;
        if (p_event->UserDataLength >= 31 && p_event->UserData) {
            uint64_t new_usage = 0;
            uint32_t process_id = 0;
            uint8_t memory_segment_group = 0;
            const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
            std::memcpy(&new_usage, raw + 0, sizeof(uint64_t));
            std::memcpy(&process_id, raw + 24, sizeof(uint32_t));
            std::memcpy(&memory_segment_group, raw + 30, sizeof(uint8_t));

            if (memory_segment_group == 1 && new_usage > 0) {
                rec.pid = (process_id != 0) ? process_id : ctx.pid;
                rec.auxiliary_data = new_usage;
                rec.flags = EventFlags::VRAM_USAGE_OVER_BUDGET;
                data_ok = true;
            }
        }
        emit_event(rec, data_ok);
        return;
    }
    emit_ndjson_only(rec);
}

void EtwSessionManager::handle_dxgkrnl_paging_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& /*ctx*/) noexcept {
    rec.category = static_cast<uint16_t>(EventCategory::DXGKRNL_VRAM_PAGING);
    bool data_ok = false;
    if (p_event->UserDataLength >= 64 && p_event->UserData) {
        uint64_t number_of_pages = 0;
        const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
        std::memcpy(&number_of_pages, raw + 48, sizeof(uint64_t));

        if (number_of_pages > 0) {
            rec.auxiliary_data = number_of_pages * PAGE_SIZE_BYTES;
            rec.flags = EventFlags::VRAM_PAGING_TRANSFER;
            data_ok = true;
        }
    }
    emit_event(rec, data_ok);
}

} // namespace stuttometer
