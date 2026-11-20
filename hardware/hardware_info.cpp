#include "hardware/hardware_info.hpp"

#include "hardware/platform.hpp"

namespace lx::hardware {

Result<HardwareInfo> detect_hardware() {
    HardwareInfo info;
    platform::detect_cpu(info.cpu);
    platform::detect_memory(info.memory);
    platform::detect_os(info.os);
    info.gpus = platform::detect_system_gpus();

    if (info.cpu.logical_cores == 0) {
        return Status::error(ErrorCode::InitializationFailed,
                             "unable to determine logical CPU count");
    }
    if (info.memory.total_bytes == 0) {
        return Status::error(ErrorCode::InitializationFailed,
                             "unable to determine total system memory");
    }
    return info;
}

Result<std::vector<GpuDeviceInfo>> enumerate_system_gpus() {
    return platform::detect_system_gpus();
}

u32 logical_processor_count() {
    return platform::logical_processor_count();
}

}  // namespace lx::hardware
