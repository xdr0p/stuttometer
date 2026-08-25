#include "test_common.hpp"
#include "stuttometer/etw_session.hpp"
#include <windows.h>
#include <evntrace.h>
#include <iostream>

#pragma pack(push, 1)
struct CSwitchPayloadLayout {
    uint32_t new_thread_id;
    uint32_t old_thread_id;
    uint8_t  new_thread_priority;
    uint8_t  old_thread_priority;
    uint8_t  previous_c_state;
    uint8_t  spare_byte;
    uint8_t  old_thread_wait_reason;
    uint8_t  old_thread_wait_mode;
    uint8_t  old_thread_state;
    uint8_t  old_thread_wait_ideal_processor;
    uint32_t new_thread_wait_time;
    uint32_t reserved;
};
#pragma pack(pop)

static void test_kernel_opcodes_and_payloads() {
    std::cout << "[TEST] Validating Windows ETW Kernel Flags, Opcodes & MOF Payload Schemas...\n";

    // 1. Classic Kernel Event Flags in evntrace.h
    static_assert(EVENT_TRACE_FLAG_CSWITCH == 0x00000010, "EVENT_TRACE_FLAG_CSWITCH must be 0x10");
    static_assert(EVENT_TRACE_FLAG_DPC == 0x00000020, "EVENT_TRACE_FLAG_DPC must be 0x20");
    static_assert(EVENT_TRACE_FLAG_INTERRUPT == 0x00000040, "EVENT_TRACE_FLAG_INTERRUPT must be 0x40");
    static_assert(EVENT_TRACE_FLAG_DISK_IO == 0x00000100, "EVENT_TRACE_FLAG_DISK_IO must be 0x100");
    static_assert(EVENT_TRACE_FLAG_DISK_IO_INIT == 0x00000400, "EVENT_TRACE_FLAG_DISK_IO_INIT must be 0x400");

    STUTTO_ASSERT(EVENT_TRACE_FLAG_DPC == 0x00000020);
    STUTTO_ASSERT(EVENT_TRACE_FLAG_INTERRUPT == 0x00000040);
    STUTTO_ASSERT(EVENT_TRACE_FLAG_CSWITCH == 0x00000010);

    // 2. Kernel MOF Opcodes: DPC_Classic=66 (0x42), ISR=67 (0x43), DPC=68 (0x44), Timer=69 (0x45)
    constexpr uint8_t KERNEL_OPCODE_DPC_CLASSIC = 66;
    constexpr uint8_t KERNEL_OPCODE_ISR = 67;
    constexpr uint8_t KERNEL_OPCODE_DPC = 68;
    constexpr uint8_t KERNEL_OPCODE_TIMER = 69;

    STUTTO_ASSERT(KERNEL_OPCODE_DPC_CLASSIC == 66);
    STUTTO_ASSERT(KERNEL_OPCODE_ISR == 67);
    STUTTO_ASSERT(KERNEL_OPCODE_DPC == 68);
    STUTTO_ASSERT(KERNEL_OPCODE_TIMER == 69);

    // 3. CSwitch (PerfInfo_V2_TypeGroup1) 24-byte payload
    static_assert(sizeof(CSwitchPayloadLayout) == 24, "CSwitch payload layout must be strictly 24 bytes");
    static_assert(offsetof(CSwitchPayloadLayout, new_thread_id) == 0);
    static_assert(offsetof(CSwitchPayloadLayout, old_thread_id) == 4);
    static_assert(offsetof(CSwitchPayloadLayout, old_thread_wait_mode) == 13);
    static_assert(offsetof(CSwitchPayloadLayout, old_thread_state) == 14);

    STUTTO_ASSERT(sizeof(CSwitchPayloadLayout) == 24);
    std::cout << "  -> DPC (66/68), ISR (67), Timer (69) opcodes and 24-byte CSwitch payload PASSED.\n";
}

static void test_provider_guids_and_events() {
    std::cout << "[TEST] Validating DXGI, Audio & D3D12 Provider GUIDs...\n";

    // DXGI Provider {CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}
    STUTTO_ASSERT(stuttometer::DXGI_PROVIDER_GUID.Data1 == 0xCA11C036);
    STUTTO_ASSERT(stuttometer::DXGI_PROVIDER_GUID.Data2 == 0x0102);
    STUTTO_ASSERT(stuttometer::DXGI_PROVIDER_GUID.Data3 == 0x4A2D);

    // Audio Provider {AE4BD3BE-F36F-45B6-8D21-BDD6FB832853}
    STUTTO_ASSERT(stuttometer::AUDIO_PROVIDER_GUID.Data1 == 0xAE4BD3BE);
    STUTTO_ASSERT(stuttometer::AUDIO_PROVIDER_GUID.Data2 == 0xF36F);
    STUTTO_ASSERT(stuttometer::AUDIO_PROVIDER_GUID.Data3 == 0x45B6);

    // Direct3D12 Provider {5D8087DD-3A9B-4F56-90DF-49196CDC4F11}
    STUTTO_ASSERT(stuttometer::DIRECT3D12_PROVIDER_GUID.Data1 == 0x5D8087DD);
    STUTTO_ASSERT(stuttometer::DIRECT3D12_PROVIDER_GUID.Data2 == 0x3A9B);
    STUTTO_ASSERT(stuttometer::DIRECT3D12_PROVIDER_GUID.Data3 == 0x4F56);

    // Kernel Memory Provider {D1D93EF7-E1F2-4F45-9943-03D245FE6C00}
    STUTTO_ASSERT(stuttometer::KERNEL_MEMORY_PROVIDER_GUID.Data1 == 0xD1D93EF7);
    STUTTO_ASSERT(stuttometer::KERNEL_MEMORY_PROVIDER_GUID.Data2 == 0xE1F2);
    STUTTO_ASSERT(stuttometer::KERNEL_MEMORY_PROVIDER_GUID.Data3 == 0x4F45);

    // Kernel Process Provider {22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}
    STUTTO_ASSERT(stuttometer::KERNEL_PROCESS_PROVIDER_GUID.Data1 == 0x22FB2CD6);
    STUTTO_ASSERT(stuttometer::KERNEL_PROCESS_PROVIDER_GUID.Data2 == 0x0E7B);
    STUTTO_ASSERT(stuttometer::KERNEL_PROCESS_PROVIDER_GUID.Data3 == 0x422B);
    STUTTO_ASSERT(stuttometer::KERNEL_PROCESS_PROVIDER_GUID.Data4[0] == 0xA0);
    STUTTO_ASSERT(stuttometer::KERNEL_PROCESS_PROVIDER_GUID.Data4[1] == 0xC7);
    STUTTO_ASSERT(stuttometer::KERNEL_PROCESS_PROVIDER_GUID.Data4[2] == 0x2F);
    STUTTO_ASSERT(stuttometer::KERNEL_PROCESS_PROVIDER_GUID.Data4[3] == 0xAD);
    STUTTO_ASSERT(stuttometer::KERNEL_PROCESS_PROVIDER_GUID.Data4[4] == 0x1F);
    STUTTO_ASSERT(stuttometer::KERNEL_PROCESS_PROVIDER_GUID.Data4[5] == 0xD0);
    STUTTO_ASSERT(stuttometer::KERNEL_PROCESS_PROVIDER_GUID.Data4[6] == 0xE7);
    STUTTO_ASSERT(stuttometer::KERNEL_PROCESS_PROVIDER_GUID.Data4[7] == 0x16);

    STUTTO_ASSERT(stuttometer::category_to_string(stuttometer::EventCategory::D3D12_PSO_CREATE) == "D3D12_PSO_CREATE");
    STUTTO_ASSERT(stuttometer::category_to_string(stuttometer::EventCategory::DXGKRNL_VRAM_PAGING) == "DXGKRNL_VRAM_PAGING");
    STUTTO_ASSERT(stuttometer::category_to_string(stuttometer::EventCategory::MEM_VIRTUAL_ALLOC) == "MEM_VIRTUAL_ALLOC");
    STUTTO_ASSERT(stuttometer::category_to_string(stuttometer::EventCategory::MEM_WORKING_SET_TRIM) == "MEM_WORKING_SET_TRIM");
    STUTTO_ASSERT(stuttometer::category_to_string(stuttometer::EventCategory::MEM_PHYSICAL_ALLOC) == "MEM_PHYSICAL_ALLOC");

    // Validate 16-bit EventFlags constants
    static_assert(stuttometer::EventFlags::DXGI_VSYNC_WAIT == 0x0010);
    static_assert(stuttometer::EventFlags::CSWITCH_OUT_VOLUNTARY == 0x0020);
    static_assert(stuttometer::EventFlags::VRAM_DEMOTED_COMMITMENT == 0x0100);
    static_assert(stuttometer::EventFlags::VRAM_USAGE_OVER_BUDGET == 0x0200);
    static_assert(stuttometer::EventFlags::VRAM_PAGING_TRANSFER == 0x0400);
    static_assert(stuttometer::EventFlags::MEM_ALLOC_COMMIT == 0x1000);
    static_assert(stuttometer::EventFlags::MEM_WS_TRIM_OUTSWAP == 0x2000);
    static_assert(stuttometer::EventFlags::MEM_PHYSICAL_CONTIGUOUS == 0x4000);

    STUTTO_ASSERT(stuttometer::EventFlags::VRAM_DEMOTED_COMMITMENT == 0x0100);
    STUTTO_ASSERT(stuttometer::EventFlags::VRAM_USAGE_OVER_BUDGET == 0x0200);
    STUTTO_ASSERT(stuttometer::EventFlags::VRAM_PAGING_TRANSFER == 0x0400);
    STUTTO_ASSERT(stuttometer::EventFlags::MEM_ALLOC_COMMIT == 0x1000);
    STUTTO_ASSERT(stuttometer::EventFlags::MEM_WS_TRIM_OUTSWAP == 0x2000);
    STUTTO_ASSERT(stuttometer::EventFlags::MEM_PHYSICAL_CONTIGUOUS == 0x4000);

    std::cout << "  -> DXGI, Audio, D3D12 & Kernel-Memory provider GUIDs and flags PASSED.\n";
}

static void test_in_flight_struct_layouts() {
    std::cout << "[TEST] Validating in-flight tracking struct layouts & trivial copyability...\n";

    static_assert(std::is_trivially_copyable_v<stuttometer::PresentInFlight>);
    static_assert(std::is_trivially_copyable_v<stuttometer::DiskInFlight>);
    static_assert(std::is_trivially_copyable_v<stuttometer::ThreadSwitchOut>);
    static_assert(std::is_trivially_copyable_v<stuttometer::PsoInFlight>);
    static_assert(std::is_trivially_copyable_v<stuttometer::WorkingSetTrimInFlight>);
    static_assert(std::is_trivially_copyable_v<stuttometer::EtwEventRecord>);

    static_assert(sizeof(stuttometer::PresentInFlight) == 16);
    static_assert(sizeof(stuttometer::DiskInFlight) == 24);
    static_assert(sizeof(stuttometer::ThreadSwitchOut) == 16);
    static_assert(sizeof(stuttometer::PsoInFlight) == 32);
    static_assert(sizeof(stuttometer::WorkingSetTrimInFlight) == 16);
    static_assert(sizeof(stuttometer::EtwEventRecord) == 56);

    STUTTO_ASSERT(sizeof(stuttometer::PresentInFlight) == 16);
    STUTTO_ASSERT(sizeof(stuttometer::DiskInFlight) == 24);
    STUTTO_ASSERT(sizeof(stuttometer::ThreadSwitchOut) == 16);
    STUTTO_ASSERT(sizeof(stuttometer::PsoInFlight) == 32);
    STUTTO_ASSERT(sizeof(stuttometer::WorkingSetTrimInFlight) == 16);
    STUTTO_ASSERT(sizeof(stuttometer::EtwEventRecord) == 56);

    const uint64_t key1 = stuttometer::make_pso_key(1234, 0xDEADBEEF);
    const uint64_t key2 = stuttometer::make_pso_key(1234, 0xCAFEFACE);
    STUTTO_ASSERT(key1 != 0);
    STUTTO_ASSERT(key2 != 0);
    STUTTO_ASSERT(key1 != key2);

    std::cout << "  -> In-flight struct layout assertions PASSED.\n";
}

int main() {
    std::cout << "=== Stuttometer ETW Constants & Schema Tests ===\n";
    try {
        test_kernel_opcodes_and_payloads();
        test_provider_guids_and_events();
        test_in_flight_struct_layouts();
        std::cout << ">>> All ETW Constants tests PASSED! <<<\n\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n[TEST FAILED] Exception: " << e.what() << "\n";
        return 1;
    }
}
