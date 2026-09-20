#include <algorithm>
#include <cstring>
#include "stuttometer/etw_session.hpp"
#include "stuttometer/ndjson_writer.hpp"
#include "stuttometer/privilege_utils.hpp"
#include "etw_kernel_opcodes.hpp"

namespace stuttometer {

void EtwSessionManager::handle_nt_dpc_isr_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    // DPC Completion
    if (ctx.opcode == KERNEL_OPCODE_DPC_CLASSIC || ctx.event_id == KERNEL_OPCODE_DPC_CLASSIC || 
        ctx.opcode == KERNEL_OPCODE_DPC || ctx.event_id == KERNEL_OPCODE_DPC || 
        ctx.opcode == KERNEL_OPCODE_TIMER || ctx.event_id == KERNEL_OPCODE_TIMER || 
        (p_event->EventHeader.EventDescriptor.Task == 1 && ctx.opcode == 2)) {
        rec.category = static_cast<uint16_t>(EventCategory::DPC);

        if (p_event->UserDataLength >= 12 && p_event->UserData) {
            uint64_t initial_time = 0;
            uint64_t routine = 0;
            if (p_event->UserDataLength == 12) {
                uint32_t initial_time_32 = 0;
                std::memcpy(&initial_time_32, p_event->UserData, sizeof(uint32_t));
                initial_time = initial_time_32;
                std::memcpy(&routine, static_cast<const uint8_t*>(p_event->UserData) + 4, sizeof(uint64_t));
            } else if (p_event->UserDataLength >= 16) {
                std::memcpy(&initial_time, p_event->UserData, sizeof(uint64_t));
                std::memcpy(&routine, static_cast<const uint8_t*>(p_event->UserData) + 8, sizeof(uint64_t));
            }
            if (initial_time > 0 || routine > 0) {
                rec.payload.routine_addr = routine;
                rec.auxiliary_data = routine;
                if (ctx.timestamp >= initial_time) {
                    const uint64_t delta_us = static_cast<uint64_t>(qpc_delta_to_us(ctx.timestamp - initial_time, qpc_freq_));
                    if (delta_us <= 10000000ULL) {
                        rec.duration_us = static_cast<uint32_t>(delta_us);
                    }
                }
                flight_recorder_.push(rec);
            }
        }

        NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
        if (writer) writer->push(rec);
    }
    // ISR Completion
    else if (ctx.opcode == KERNEL_OPCODE_ISR_CLASSIC || ctx.event_id == KERNEL_OPCODE_ISR_CLASSIC || (p_event->EventHeader.EventDescriptor.Task == 2 && ctx.opcode == 2)) {
        rec.category = static_cast<uint16_t>(EventCategory::ISR);

        if (p_event->UserDataLength >= 12 && p_event->UserData) {
            uint64_t initial_time = 0;
            uint64_t routine = 0;
            if (p_event->UserDataLength == 12) {
                uint32_t initial_time_32 = 0;
                std::memcpy(&initial_time_32, p_event->UserData, sizeof(uint32_t));
                initial_time = initial_time_32;
                std::memcpy(&routine, static_cast<const uint8_t*>(p_event->UserData) + 4, sizeof(uint64_t));
            } else if (p_event->UserDataLength >= 16) {
                std::memcpy(&initial_time, p_event->UserData, sizeof(uint64_t));
                std::memcpy(&routine, static_cast<const uint8_t*>(p_event->UserData) + 8, sizeof(uint64_t));
            }
            if (initial_time > 0 || routine > 0) {
                rec.payload.routine_addr = routine;
                rec.auxiliary_data = routine;
                if (ctx.timestamp >= initial_time) {
                    const uint64_t delta_us = static_cast<uint64_t>(qpc_delta_to_us(ctx.timestamp - initial_time, qpc_freq_));
                    if (delta_us <= 10000000ULL) {
                        rec.duration_us = static_cast<uint32_t>(delta_us);
                    }
                }
                flight_recorder_.push(rec);
            }
        }

        NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
        if (writer) writer->push(rec);
    }
    // Kernel Profile / Sampled Profile (Opcode 46 / PerfInfo Sample)
    else if (ctx.opcode == 46 || (p_event->EventHeader.EventDescriptor.Task == 7 && ctx.opcode == 2)) {
        rec.category = static_cast<uint16_t>(EventCategory::PROFILE);
        if (p_event->UserDataLength >= sizeof(uint64_t) && p_event->UserData) {
            uint64_t ip = 0;
            std::memcpy(&ip, p_event->UserData, sizeof(uint64_t));
            rec.payload.routine_addr = ip;
            rec.auxiliary_data = ip;
        }
        flight_recorder_.push(rec);

        NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
        if (writer) writer->push(rec);
    }
}

void EtwSessionManager::handle_nt_cswitch_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    rec.category = static_cast<uint16_t>(EventCategory::CSWITCH);

    if (p_event->UserDataLength >= 15 && p_event->UserData) {
        const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
        uint32_t new_tid = 0;
        uint32_t old_tid = 0;
        uint8_t old_state = 0;

        std::memcpy(&new_tid, raw + 0, sizeof(uint32_t));
        std::memcpy(&old_tid, raw + 4, sizeof(uint32_t));
        std::memcpy(&old_state, raw + 14, sizeof(uint8_t));

        tid_to_pid_.insert(new_tid, { ctx.pid, ctx.timestamp });

        uint32_t old_pid = 0;
        TidPidEntry entry{};
        if (tid_to_pid_.lookup(old_tid, entry)) {
            old_pid = entry.pid;
        }

        if (old_tid != 0) {
            in_flight_threads_.insert(old_tid, { ctx.timestamp, old_pid, old_state });
        }

        ThreadSwitchOut so{};
        if (in_flight_threads_.find_and_erase(new_tid, so)) {
            if (so.pid == 0 || so.pid == ctx.pid) {
                if (ctx.timestamp >= so.qpc) {
                    const uint64_t dur_us = static_cast<uint64_t>(qpc_delta_to_us(ctx.timestamp - so.qpc, qpc_freq_));
                    rec.duration_us = (dur_us <= 10000000ULL) ? static_cast<uint32_t>(dur_us) : 10000000U;
                }
                if (so.wait_state == 5 || so.wait_state == 4) {
                    rec.flags |= EventFlags::CSWITCH_VOLUNTARY;
                }
            }
        }

        if (old_state == 5 || old_state == 4) {
            rec.flags |= EventFlags::CSWITCH_OUT_VOLUNTARY;
        }

        rec.tid = new_tid;
        rec.payload.cswitch.prev_tid = old_tid;
        rec.payload.cswitch.prev_pid = old_pid;

        flight_recorder_.push(rec);
    }

    NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
    if (writer) writer->push(rec);
}

void EtwSessionManager::handle_nt_disk_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    rec.category = static_cast<uint16_t>(EventCategory::DISK);

    if (ctx.opcode == KERNEL_OPCODE_DISK_READ_INIT || ctx.opcode == KERNEL_OPCODE_DISK_WRITE_INIT) {
        if (p_event->UserDataLength >= 12 && p_event->UserData) {
            uint64_t irp = 0;
            uint32_t issuing_tid = ctx.tid;
            std::memcpy(&irp, p_event->UserData, sizeof(uint64_t));
            std::memcpy(&issuing_tid, static_cast<const uint8_t*>(p_event->UserData) + 8, sizeof(uint32_t));
            if (issuing_tid == 0) issuing_tid = ctx.tid;
            in_flight_disk_.insert(irp, { ctx.timestamp, ctx.pid, issuing_tid, (ctx.opcode == KERNEL_OPCODE_DISK_WRITE_INIT) });
            rec.payload.file_key = irp;
            rec.auxiliary_data = 0;
            if (ctx.opcode == KERNEL_OPCODE_DISK_WRITE_INIT) {
                rec.flags |= EventFlags::DISK_IS_WRITE;
            }
        }
    } else if (ctx.opcode == KERNEL_OPCODE_DISK_READ || ctx.opcode == KERNEL_OPCODE_DISK_WRITE) {
        if (ctx.opcode == KERNEL_OPCODE_DISK_WRITE) {
            rec.flags |= EventFlags::DISK_IS_WRITE;
        }
        if (p_event->UserDataLength >= 40 && p_event->UserData) {
            const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
            uint32_t size_bytes = 0;
            uint64_t irp = 0;

            std::memcpy(&size_bytes, raw + 8, sizeof(uint32_t));
            std::memcpy(&irp, raw + 32, sizeof(uint64_t));

            rec.payload.file_key = irp;
            rec.auxiliary_data = size_bytes;

            DiskInFlight disk_data{};
            if (in_flight_disk_.find_and_erase(irp, disk_data)) {
                if (disk_data.pid != 0) {
                    rec.pid = disk_data.pid;
                    rec.tid = disk_data.tid;
                    if (ctx.timestamp >= disk_data.start_qpc) {
                        const uint64_t delta_us = static_cast<uint64_t>(qpc_delta_to_us(ctx.timestamp - disk_data.start_qpc, qpc_freq_));
                        if (delta_us <= 3000000ULL) {
                            rec.duration_us = static_cast<uint32_t>(delta_us);
                        }
                    }
                } else {
                    rec.duration_us = 0;
                }
                if (disk_data.is_write) {
                    rec.flags |= EventFlags::DISK_IS_WRITE;
                }
            } else {
                rec.duration_us = 0;
            }
            if (rec.duration_us > 0 || rec.pid != 0) {
                flight_recorder_.push(rec);
            }
        }
    }

    NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
    if (writer) writer->push(rec);
}

void EtwSessionManager::handle_nt_fault_event(PEVENT_RECORD p_event, EtwEventRecord& rec, const EventContext& ctx) noexcept {
    // Hard Page Fault (Opcode 32 / HardFault)
    if (ctx.opcode == KERNEL_OPCODE_HARDFAULT) {
        rec.category = static_cast<uint16_t>(EventCategory::PAGE_FAULT);

        if (p_event->UserDataLength >= 40 && p_event->UserData) {
            const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
            int64_t initial_time = 0;
            uint64_t file_object = 0;
            uint32_t byte_count = 0;
            std::memcpy(&initial_time, raw + 0, sizeof(int64_t));
            std::memcpy(&file_object, raw + 24, sizeof(uint64_t));
            std::memcpy(&byte_count, raw + 36, sizeof(uint32_t));
            rec.payload.file_key = file_object;
            rec.auxiliary_data = byte_count;

            uint64_t initial_time_ft = static_cast<uint64_t>(initial_time);
            constexpr uint64_t MIN_VALID_FILETIME = 125911584000000000ULL; // Jan 1, 2000 00:00:00 UTC
            uint64_t sync_utc = 0, sync_qpc = 0;
            read_sync_time(sync_utc, sync_qpc);

            if (initial_time_ft >= MIN_VALID_FILETIME && sync_utc > 0 && sync_qpc > 0 && ctx.timestamp >= sync_qpc) {
                const uint64_t delta_qpc = ctx.timestamp - sync_qpc;
                const uint64_t q = delta_qpc / qpc_freq_;
                const uint64_t r = delta_qpc % qpc_freq_;
                const uint64_t delta_100ns = (q * 10000000ULL) + ((r * 10000000ULL) / qpc_freq_);
                const uint64_t end_ft = sync_utc + delta_100ns;
                if (end_ft >= initial_time_ft) {
                    uint64_t dur_100ns = end_ft - initial_time_ft;
                    uint64_t dur_us = dur_100ns / 10;
                    if (dur_us <= 10000000ULL) {
                        rec.duration_us = static_cast<uint32_t>(dur_us);
                    }
                }
            } else {
                rec.duration_us = 0;
            }
            flight_recorder_.push(rec);
        }

        NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
        if (writer) writer->push(rec);
    }
    // VirtualAlloc (Opcode 98 under PAGE_FAULT_GUID)
    else if (ctx.opcode == KERNEL_OPCODE_VIRTUAL_ALLOC) {
        rec.category = static_cast<uint16_t>(EventCategory::MEM_VIRTUAL_ALLOC);

        if (p_event->UserDataLength >= 24 && p_event->UserData) {
            const auto* raw = static_cast<const uint8_t*>(p_event->UserData);
            uint64_t base_addr = 0;
            uint64_t region_size = 0;
            uint32_t alloc_pid = 0;
            uint32_t alloc_flags = 0;
            std::memcpy(&base_addr, raw + 0, sizeof(uint64_t));
            std::memcpy(&region_size, raw + 8, sizeof(uint64_t));
            std::memcpy(&alloc_pid, raw + 16, sizeof(uint32_t));
            std::memcpy(&alloc_flags, raw + 20, sizeof(uint32_t));

            rec.pid = alloc_pid;
            rec.payload.routine_addr = base_addr;
            rec.auxiliary_data = region_size;
            rec.flags = (alloc_flags & MEM_COMMIT) ? EventFlags::MEM_ALLOC_COMMIT : EventFlags::NONE;

            if (region_size >= (4 * 1024 * 1024ULL) && (alloc_flags & (MEM_COMMIT | MEM_RESET | MEM_LARGE_PAGES)) != 0) {
                flight_recorder_.push(rec);
            }
        }

        NdjsonWriter* writer = ndjson_writer_.load(std::memory_order_relaxed);
        if (writer) writer->push(rec);
    }
}

} // namespace stuttometer
