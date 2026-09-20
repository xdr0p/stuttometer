#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include "stuttometer/etw_session.hpp"
#include "stuttometer/ndjson_writer.hpp"
#include "stuttometer/privilege_utils.hpp"

namespace stuttometer {

void EtwSessionManager::handle_audio_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    if (ctx.event_id == 11) { // AudioGlitch Event ID 11
        rec.category = static_cast<uint16_t>(EventCategory::AUDIO);
        rec.flags |= EventFlags::AUDIO_BUFFER_UNDERRUN;
        uint32_t glitch_count = 1;
        int32_t error_code = 0;
        if (p_event->UserDataLength >= sizeof(uint32_t) && p_event->UserData) {
            std::memcpy(&glitch_count, p_event->UserData, sizeof(uint32_t));
            glitch_count = std::clamp(glitch_count, 1U, 1'000'000U);
        }
        if (p_event->UserDataLength >= (sizeof(uint32_t) + sizeof(int32_t)) && p_event->UserData) {
            std::memcpy(&error_code, static_cast<const uint8_t*>(p_event->UserData) + sizeof(uint32_t), sizeof(int32_t));
        }
        rec.payload.audio.glitch_count = glitch_count;
        rec.payload.audio.error_code = error_code;
        rec.auxiliary_data = glitch_count;

        flight_recorder_.push(rec);

        NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
        if (writer) writer->push(rec);

        trigger_engine_.on_audio_glitch(ctx.pid, ctx.tid, glitch_count, ctx.timestamp, ctx.cpu);
    }
}

void EtwSessionManager::handle_dwm_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    rec.category = static_cast<uint16_t>(EventCategory::DWM_GLITCH);

    const double vblank_ms = (trigger_engine_.vblank_interval_ms() > 0.0) 
        ? trigger_engine_.vblank_interval_ms() 
        : 16.67;
    uint32_t glitch_type = 0;
    uint32_t missed_vblanks = 0;
    if (p_event->UserDataLength >= 8 && p_event->UserData) {
        const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
        std::memcpy(&glitch_type, raw + 0, sizeof(uint32_t));
        std::memcpy(&missed_vblanks, raw + 4, sizeof(uint32_t));
    }
    rec.auxiliary_data = glitch_type;
    const double dur_ms = (missed_vblanks >= 1) ? (missed_vblanks * vblank_ms) : vblank_ms;
    rec.duration_us = static_cast<uint32_t>(std::clamp(dur_ms * 1000.0, 1000.0, 10000000.0));

    bool is_dedup = false;
    const uint64_t dedup_window_qpc = ms_to_qpc_delta(50.0, qpc_freq_);
    for (size_t i = 0; i < 16; ++i) {
        uint64_t recent_ts = recent_dwm_glitches_qpc_[i].load(std::memory_order_acquire);
        if (recent_ts > 0) {
            const uint64_t delta_qpc = (ctx.timestamp >= recent_ts) ? (ctx.timestamp - recent_ts) : (recent_ts - ctx.timestamp);
            if (delta_qpc < dedup_window_qpc) {
                is_dedup = true;
                break;
            }
        }
    }

    if (is_dedup) {
        rec.flags |= EventFlags::DWM_GLITCH_DEDUPLICATED;
    } else {
        uint32_t slot = (recent_dwm_glitch_idx_.fetch_add(1, std::memory_order_relaxed)) & 15;
        recent_dwm_glitches_qpc_[slot].store(ctx.timestamp, std::memory_order_release);
    }

    flight_recorder_.push(rec);

    NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
    if (writer) writer->push(rec);

    if (!is_dedup) {
        trigger_engine_.on_dwm_glitch(ctx.pid, ctx.tid, dur_ms, ctx.timestamp, ctx.cpu);
    }
}

void EtwSessionManager::handle_power_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    if (ctx.event_id == 37) {
        rec.category = static_cast<uint16_t>(EventCategory::THERMAL_THROTTLE);

        if (p_event->UserDataLength >= 24 && p_event->UserData) {
            const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
            uint32_t core_number = 0;
            uint32_t cap_duration_sec = 0;
            std::memcpy(&core_number, raw + 4, sizeof(uint32_t));
            std::memcpy(&cap_duration_sec, raw + 8, sizeof(uint32_t));
            if (core_number <= 1024 && cap_duration_sec <= 86400) {
                rec.cpu_index = static_cast<uint8_t>(std::min(core_number, 255U));
                rec.auxiliary_data = cap_duration_sec;
                rec.duration_us = 0;
                flight_recorder_.push(rec);
            }
        }

        NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
        if (writer) writer->push(rec);
    }
}

void EtwSessionManager::handle_antimalware_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    rec.category = static_cast<uint16_t>(EventCategory::ANTIMALWARE_SCAN);

    uint64_t scan_key = 0;
    const auto& act = p_event->EventHeader.ActivityId;
    if (!IsEqualGUID(act, GUID_NULL)) {
        scan_key = activity_id_to_key(act);
    }
    if (scan_key == 0) {
        scan_key = (static_cast<uint64_t>(ctx.pid) << 32) | (ctx.tid != 0 ? ctx.tid : 1);
    }

    if (ctx.opcode == 1) { // win:Start
        in_flight_scans_.insert(scan_key, { ctx.timestamp, ctx.pid, ctx.tid });
    } else if (ctx.opcode == 2) { // win:Stop
        AntimalwareScanInFlight scan_data{};
        if (in_flight_scans_.find_and_erase(scan_key, scan_data)) {
            if (ctx.timestamp >= scan_data.start_qpc) {
                const uint64_t delta_us = static_cast<uint64_t>(qpc_delta_to_us(ctx.timestamp - scan_data.start_qpc, qpc_freq_));
                if (delta_us <= 10000000ULL) {
                    rec.duration_us = static_cast<uint32_t>(delta_us);
                }
            }
        }
        if (rec.duration_us > 0) {
            flight_recorder_.push(rec);
        }
    }

    NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
    if (writer) writer->push(rec);
}

// Microsoft-Windows-Kernel-Process ProcessStart (Event ID 1) payload:
// - ProcessID:       win:UInt32        (offset 0, 4 bytes)
// - ParentProcessID: win:UInt32        (offset 4, 4 bytes)
// - ImageName:       win:UnicodeString (offset 8, null-terminated wchar_t)
void EtwSessionManager::handle_process_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    rec.category = static_cast<uint16_t>(EventCategory::PROCESS);

    if (ctx.event_id == 1 && p_event->UserDataLength >= 4 && p_event->UserData) {
        uint32_t target_pid = 0;
        std::memcpy(&target_pid, p_event->UserData, sizeof(uint32_t));
        if (target_pid != 0) {
            rec.pid = target_pid;
            std::string proc_name;
            if (p_event->UserDataLength >= 12) {
                const auto* raw_bytes = static_cast<const uint8_t*>(p_event->UserData) + 8;
                const auto* p_ws = reinterpret_cast<const wchar_t*>(raw_bytes);
                const size_t max_wchars = (p_event->UserDataLength - 8) / sizeof(wchar_t);
                size_t wlen = 0;
                bool null_terminated = false;
                bool valid_chars = true;
                while (wlen < max_wchars && wlen < MAX_PATH) {
                    wchar_t wc = p_ws[wlen];
                    if (wc == L'\0') {
                        null_terminated = true;
                        break;
                    }
                    if ((wc < 0x20 && wc != L'\t') || wc == 0x7F) {
                        valid_chars = false;
                        break;
                    }
                    ++wlen;
                }
                if (null_terminated && valid_chars && wlen > 0) {
                    std::string full_path = utf16_to_utf8(std::wstring_view(p_ws, wlen));
                    const size_t slash = full_path.find_last_of("\\/");
                    proc_name = (slash != std::string::npos) ? full_path.substr(slash + 1) : full_path;
                }
            }
            if (proc_name.empty()) {
                proc_name = get_process_name_by_pid(target_pid);
            }
            if (!proc_name.empty()) {
                trigger_engine_.on_process_launched(target_pid, proc_name);
            }
        }
    } else if (ctx.event_id == 2 && p_event->UserDataLength >= 4 && p_event->UserData) {
        uint32_t target_pid = 0;
        std::memcpy(&target_pid, p_event->UserData, sizeof(uint32_t));
        if (target_pid != 0) {
            rec.pid = target_pid;
            trigger_engine_.on_process_terminated(target_pid);
        }
    }

    NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
    if (writer) writer->push(rec);
}

} // namespace stuttometer
